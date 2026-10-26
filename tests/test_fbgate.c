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
    if (fails) { printf("test_fbgate: %d FAILED\n", fails); return 1; }
    printf("test_fbgate: all checks passed\n");
    return 0;
}
