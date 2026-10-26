/*
 * test_fbgate.c - common/rdna3_fbgate.h on the host: which framebuffer
 * controller gates the accelerator closes before it calls the display
 * machine (card run 2026-10-26j, DRIVER-PLAN N58), and a lock-order model
 * of the hang: the thread call's acquisitions against AppleGPUWrangler's
 * (IOFramebuffer::message -> probeAccelerator -> requestProbe) and the
 * family's own (display_change_handler inside the gate). An edge A -> B is
 * "B taken while A held" (a recursive re-take of a held gate adds none); a
 * cycle is a possible deadlock.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "rdna3_fbgate.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* the locks of the model: the accelerator lock and one gate per controller */
enum { L_ACCEL = 0, L_GATE0 = 1, NLOCKS = 1 + RD3_FBGATE_MAX };
static unsigned char edge[NLOCKS][NLOCKS];

static void take_seq(const int *seq, unsigned n)
{
    int held[NLOCKS] = {0};
    for (unsigned i = 0; i < n; i++) {
        int b = seq[i];
        if (held[b])
            continue;   /* the controller gate is recursive for its owner */
        for (int a = 0; a < NLOCKS; a++)
            if (held[a])
                edge[a][b] = 1;
        held[b] = 1;
    }
}

static int visit(int v, int *state)
{
    state[v] = 1;
    for (int w = 0; w < NLOCKS; w++) {
        if (!edge[v][w])
            continue;
        if (state[w] == 1 || (state[w] == 0 && visit(w, state)))
            return 1;
    }
    state[v] = 2;
    return 0;
}

static int has_cycle(void)
{
    int state[NLOCKS] = {0};
    for (int v = 0; v < NLOCKS; v++)
        if (!state[v] && visit(v, state))
            return 1;
    return 0;
}

/* the controller (gate lock) of head h: the index of its workloop's first head */
static int gate_of(const void *const wl[], unsigned h)
{
    for (unsigned i = 0; i <= h; i++)
        if (wl[i] == wl[h])
            return L_GATE0 + (int)i;
    return -1;
}

/* the hang's two paths plus the family's own path for every head; 'ours'
 * is the thread call's sequence before the display machine runs (the
 * gates of the plan, or none: the old code) */
static int model_deadlocks(const void *const fb[], const void *const wl[], unsigned heads, int gated)
{
    memset(edge, 0, sizeof(edge));
    struct rd3_fbgate_plan p;
    rd3_fbgate_plan(fb, wl, heads, &p);
    int seq[2 * NLOCKS + 1];
    unsigned n = 0;
    if (gated)
        for (unsigned k = 0; k < p.n; k++)
            seq[n++] = gate_of(wl, p.head[k]);
    /* display_mode_did_change: the accelerator lock, then every pipe's
     * getAttributeExt closes that head's controller gate */
    seq[n++] = L_ACCEL;
    for (unsigned h = 0; h < heads; h++)
        if (fb[h] && wl[h])
            seq[n++] = gate_of(wl, h);
    take_seq(seq, n);
    for (unsigned h = 0; h < heads; h++) {
        if (!fb[h] || !wl[h])
            continue;
        /* AppleGPUWrangler: IOFramebuffer::message (gate) -> requestProbe (accelerator lock);
         * IOGraphics' notification -> display_change_handler: the same order */
        int w[2] = { gate_of(wl, h), L_ACCEL };
        take_seq(w, 2);
    }
    return has_cycle();
}

/* ECC review of 0535fd7: a head publishes while the thread waits for the
 * gates planned at entry ('entry' -> 'late' framebuffers; wl[] is fixed
 * per head). The old code ran the body under the entry plan's gates and
 * registered every published head; the new one plans again inside the
 * innermost gate, tries again while that plan needs a gate not held, and
 * registers only the heads of the plan it ran under. A registration is
 * modelled as found_framebuffer: the accelerator lock, then that head's
 * controller gate (the new pipe asks its framebuffer). Returns the heads
 * registered; *cycle says whether the lock graph with AppleGPUWrangler's
 * gate -> accelerator lock for every published head has a cycle. */
static unsigned late_head(const void *const entry[], const void *const late[], const void *const wl[], unsigned heads,
                          int newCode, int *cycle, unsigned *tries)
{
    memset(edge, 0, sizeof(edge));
    struct rd3_fbgate_plan p, now;
    const void *held[RD3_FBGATE_MAX];
    const void *const *fbAtPlan = entry;
    unsigned t;
    for (t = 1; ; t++) {
        rd3_fbgate_plan(fbAtPlan, wl, heads, &p);
        for (unsigned k = 0; k < p.n; k++)
            held[k] = wl[p.head[k]];
        rd3_fbgate_plan(late, wl, heads, &now);   /* the heads once every gate is held */
        if (!newCode || rd3_fbgate_covered(held, p.n, wl, &now) || t == RD3_FBGATE_TRIES)
            break;
        fbAtPlan = late;   /* planned again: the late head is published by now */
    }
    *tries = t;
    unsigned published = 0;
    for (unsigned h = 0; h < heads; h++)
        if (late[h])
            published |= 1u << h;
    unsigned reg = newCode ? rd3_fbgate_registrable(published, 0, now.gated) : published;
    if (newCode && !rd3_fbgate_covered(held, p.n, wl, &now))
        reg = 0;   /* tries used up: nothing registered */
    int seq[3 * NLOCKS];
    unsigned n = 0;
    for (unsigned k = 0; k < p.n; k++)
        seq[n++] = gate_of(wl, p.head[k]);
    for (unsigned h = 0; h < heads; h++)
        if (reg & (1u << h)) {
            seq[n++] = L_ACCEL;
            seq[n++] = gate_of(wl, h);
        }
    take_seq(seq, n);
    for (unsigned h = 0; h < heads; h++)
        if (published & (1u << h)) {
            int w[2] = { gate_of(wl, h), L_ACCEL };
            take_seq(w, 2);
        }
    *cycle = has_cycle();
    return reg;
}

int main(void)
{
    int fbA, fbB, wlX, wlY;
    struct rd3_fbgate_plan p;

    /* nothing published: nothing to close */
    {
        const void *fb[2] = { 0, 0 }, *wl[2] = { 0, 0 };
        rd3_fbgate_plan(fb, wl, 2, &p);
        CHECK(p.n == 0 && p.gated == 0 && p.noWl == 0);
    }
    /* card run 2026-10-26j: two heads of one controller share its gate */
    {
        const void *fb[2] = { &fbA, &fbB }, *wl[2] = { &wlX, &wlX };
        rd3_fbgate_plan(fb, wl, 2, &p);
        CHECK(p.n == 1 && p.head[0] == 0 && p.gated == 0x3 && p.noWl == 0);
        CHECK(model_deadlocks(fb, wl, 2, 0));    /* the old order: the hang */
        CHECK(!model_deadlocks(fb, wl, 2, 1));   /* the gate first: none */
    }
    /* two controllers: both gates, in head order */
    {
        const void *fb[2] = { &fbA, &fbB }, *wl[2] = { &wlX, &wlY };
        rd3_fbgate_plan(fb, wl, 2, &p);
        CHECK(p.n == 2 && p.head[0] == 0 && p.head[1] == 1 && p.gated == 0x3);
        CHECK(model_deadlocks(fb, wl, 2, 0));
        CHECK(!model_deadlocks(fb, wl, 2, 1));
    }
    /* only head 1 published (head 0 not yet) */
    {
        const void *fb[2] = { 0, &fbB }, *wl[2] = { 0, &wlY };
        rd3_fbgate_plan(fb, wl, 2, &p);
        CHECK(p.n == 1 && p.head[0] == 1 && p.gated == 0x2 && p.noWl == 0);
        CHECK(!model_deadlocks(fb, wl, 2, 1));
    }
    /* a published head without a controller workloop is named, not gated */
    {
        const void *fb[2] = { &fbA, &fbB }, *wl[2] = { &wlX, 0 };
        rd3_fbgate_plan(fb, wl, 2, &p);
        CHECK(p.n == 1 && p.head[0] == 0 && p.gated == 0x1 && p.noWl == 0x2);
    }
    /* more heads than the plan holds: the extra ones are not looked at */
    {
        const void *fb[RD3_FBGATE_MAX + 2], *wl[RD3_FBGATE_MAX + 2];
        static int w[RD3_FBGATE_MAX + 2];
        for (unsigned i = 0; i < RD3_FBGATE_MAX + 2; i++) { fb[i] = &w[i]; wl[i] = &w[i]; }
        rd3_fbgate_plan(fb, wl, RD3_FBGATE_MAX + 2, &p);
        CHECK(p.n == RD3_FBGATE_MAX && p.head[RD3_FBGATE_MAX - 1] == RD3_FBGATE_MAX - 1);
        CHECK(p.gated == (1u << RD3_FBGATE_MAX) - 1);
    }
    /* the late head (review of 0535fd7): head 1 publishes, on its own controller, while the thread waits for head 0's gate */
    {
        int cyc;
        unsigned tries;
        const void *entry[2] = { &fbA, 0 }, *late[2] = { &fbA, &fbB }, *wl[2] = { &wlX, &wlY };
        CHECK(late_head(entry, late, wl, 2, 0, &cyc, &tries) == 0x3 && cyc);    /* old: head 1 registered ungated */
        CHECK(late_head(entry, late, wl, 2, 1, &cyc, &tries) == 0x3 && !cyc && tries == 2);   /* new: planned again */
        /* on head 0's controller: the held gate covers it, no second try */
        const void *wlS[2] = { &wlX, &wlX };
        CHECK(late_head(entry, late, wlS, 2, 1, &cyc, &tries) == 0x3 && !cyc && tries == 1);
        /* the covered check itself */
        struct rd3_fbgate_plan nowp;
        const void *held1[1] = { &wlX };
        rd3_fbgate_plan(late, wl, 2, &nowp);
        CHECK(!rd3_fbgate_covered(held1, 1, wl, &nowp));
        rd3_fbgate_plan(late, wlS, 2, &nowp);
        CHECK(rd3_fbgate_covered(held1, 1, wlS, &nowp));
        CHECK(rd3_fbgate_covered(held1, 0 + 1, wl, &(struct rd3_fbgate_plan){ 0, { 0 }, 0, 0 }));
    }
    /* a head publishing during the body itself is outside the mask the body runs with */
    CHECK(rd3_fbgate_registrable(0x3, 0x0, 0x1) == 0x1);
    CHECK(rd3_fbgate_registrable(0x3, 0x1, 0x3) == 0x2);
    CHECK(rd3_fbgate_registrable(0x2, 0x0, 0x1) == 0x0);
    if (fails) { printf("test_fbgate: %d FAILED\n", fails); return 1; }
    printf("test_fbgate: all checks passed\n");
    return 0;
}
