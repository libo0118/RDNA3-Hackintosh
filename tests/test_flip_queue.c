/*
 * test_flip_queue.c - common/rdna3_flip_queue.h on the host (DRIVER-PLAN
 * N38: mode 0 presents without blocking WindowServer).
 *
 * The model: a head at 60 Hz (vblank every 16667 us). A flip programmed at
 * t latches at the first vblank after t; until then the HUBP scans the
 * buffer before. The accelerator's per-head worker looks WORKER us after
 * each vblank (the framebuffer's VBL thread wakes it), sees the latch,
 * completes the in-flight present and programs the queued one. The
 * presenter (WindowServer's swap / flip-buffer present) never waits; the
 * writers the driver sees (copy forward's target, the family's page-on)
 * wait only for a buffer still leaving the panel.
 *
 * Replayed: card run 2026-10-18b's sequences - head 1's flip-buffer burst
 * (submitFlipBuffer rotating 0x8a020000 / 0x8a810000 / 0x88060000 with a
 * copy forward into the next one ~2-3 ms before each, 11:51:47.12-.96) and
 * head 0's / head 1's mode 0 swaps (buf[1] copied forward into and swapped
 * every time: the buffer the panel already scans) - plus a presenter faster
 * than the panel, a 3-buffer rotation at random phases and another path's
 * flip in flight (Busy). Checked: every frame programmed in order and the
 * newest one each time, at most one flip in flight, no present lost (each
 * one programmed, replaced by a newer one, joined or the same buffer), the
 * last one on the panel, a completion only after its flip latched, the
 * scanned buffer never reported free, no write into the buffer the panel
 * reads (but the one-back-buffer in-place case), and in 18b's sequences no
 * wait at all.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_flip_queue.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { PERIOD = 16667, WORKER = 300, MAXREC = 8192 };
#define HOME 0x87830000ull   /* the framebuffer's own scanout: never a WindowServer buffer */

static uint32_t rng = 777u;
static uint32_t rnd(uint32_t n)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 8) % n;
}

struct sim {
    struct rd3_fq q;
    uint64_t now;
    uint64_t inuse;          /* what the HUBP scans */
    uint64_t req;            /* programmed, latches at the next vblank while 'pend' */
    int pend;
    int foreign;             /* the pending flip is another path's */
    uint64_t latch_t;        /* when the last flip latched */
    uint64_t next_vbl, worker_t;
    int worker_due;
    /* records */
    uint32_t prog_tok[MAXREC];
    uint64_t prog_t[MAXREC], prog_latch[MAXREC];
    unsigned nprog;
    unsigned nsub, same, queued, replaced, joined, done, refused;
    uint32_t newest_tok;
    uint64_t newest_addr;
    unsigned waits, inplace, arriving, scanned_free, wrote_on_panel, early;
    uint64_t wait_max;
};

static void sim_init(struct sim *s)
{
    memset(s, 0, sizeof *s);
    rd3_fq_init(&s->q);
    s->inuse = HOME;
    s->next_vbl = PERIOD;
}

static void hw_program(struct sim *s, uint64_t addr, uint32_t tok)
{
    CHECK(!s->pend);                       /* at most one flip in flight */
    CHECK(tok == s->newest_tok);           /* the newest present each time */
    if (s->nprog) CHECK(tok > s->prog_tok[s->nprog - 1]);   /* in order */
    s->req = addr;
    s->pend = 1;
    s->foreign = 0;
    if (s->nprog < MAXREC) {
        s->prog_tok[s->nprog] = tok;
        s->prog_t[s->nprog] = s->now;
        s->prog_latch[s->nprog] = 0;
        s->nprog++;
    }
}

/* the invariants after every step */
static void inv(const struct sim *s)
{
    /* the scanned buffer (one of ours) is never reported free */
    if (s->inuse != HOME && s->q.shown)
        CHECK(rd3_fq_in_use(&s->q, s->inuse));
    if (s->pend && !s->foreign)
        CHECK(rd3_fq_in_use(&s->q, s->req) && s->q.inflight == s->req);
    if (s->pend && s->foreign)
        CHECK(s->q.inflight == RD3_FQ_FOREIGN);
    if (!s->pend)
        CHECK(!s->q.inflight || s->q.inflight != RD3_FQ_FOREIGN);
}

/* the flip-done path WORKER us after a vblank: sees the latch, completes,
 * programs the queued present */
static void worker(struct sim *s)
{
    int had = 0;
    uint32_t tok = 0, ntok = 0;
    uint64_t naddr = 0;
    if (!s->q.inflight || s->pend)
        return;
    const enum rd3_fq_act a = rd3_fq_latched(&s->q, &had, &tok, &naddr, &ntok);
    if (had) {
        s->done++;
        /* its flip latched before this completion */
        unsigned i;
        for (i = 0; i < s->nprog && s->prog_tok[i] != tok; i++) {}
        CHECK(i < s->nprog);
        if (i < s->nprog) {
            s->prog_latch[i] = s->latch_t;
            CHECK(s->latch_t > s->prog_t[i] && s->now >= s->latch_t);
        }
    }
    if (a == RD3_FQ_PROGRAM)
        hw_program(s, naddr, ntok);
    inv(s);
}

static void run_until(struct sim *s, uint64_t t)
{
    for (;;) {
        const uint64_t nw = s->worker_due ? s->worker_t : UINT64_MAX;
        const uint64_t n = s->next_vbl < nw ? s->next_vbl : nw;
        if (n > t)
            break;
        s->now = n;
        if (n == nw) {
            s->worker_due = 0;
            worker(s);
            continue;
        }
        if (s->pend) {
            s->inuse = s->foreign ? HOME : s->req;
            s->pend = 0;
            s->foreign = 0;
            s->latch_t = s->now;
        }
        s->worker_due = 1;
        s->worker_t = s->now + WORKER;
        s->next_vbl += PERIOD;
    }
    if (t > s->now)
        s->now = t;
}

/* a present (a swap or a flip-buffer call): 'same' is the caller's own
 * check of the HUBP's address (rd3_latch_same_shown) before the queue */
static void present(struct sim *s, uint64_t t, uint64_t addr, int use_same)
{
    uint32_t dropped = 0;
    run_until(s, t);
    /* the presenter reads the HUBP first: a latch the worker has not seen yet is taken here */
    if (s->q.inflight && !s->pend) worker(s);
    const uint32_t tok = ++s->nsub;
    if (use_same && addr == s->inuse && !s->pend) {
        s->same++;
        inv(s);
        return;
    }
    s->newest_tok = tok;
    s->newest_addr = addr;
    switch (rd3_fq_submit(&s->q, addr, tok, &dropped)) {
    case RD3_FQ_PROGRAM:
        if (s->pend) {
            /* another path's flip is still pending: the framebuffer answers Busy */
            CHECK(s->foreign);
            CHECK(!rd3_fq_busy(&s->q, &dropped));
            s->queued++;
        } else {
            hw_program(s, addr, tok);
        }
        break;
    case RD3_FQ_QUEUED:   s->queued++; break;
    case RD3_FQ_REPLACED: s->queued++; s->replaced++; CHECK(dropped && dropped < tok); break;
    case RD3_FQ_JOINED:   s->joined++; break;
    default:              CHECK(0); break;
    }
    inv(s);
}

/* a writer the driver sees (copy forward's target, a page-on) */
static void write_buf(struct sim *s, uint64_t t, uint64_t addr)
{
    run_until(s, t);
    /* the gate reads the HUBP before it waits: a latch not seen yet is taken here */
    if (s->q.inflight && !s->pend) worker(s);
    enum rd3_fq_use u = rd3_fq_reuse(&s->q, addr);
    if (addr == s->inuse && u == RD3_FQ_FREE)
        s->scanned_free++;
    if (u == RD3_FQ_LEAVING) {
        const uint64_t t0 = s->now;
        s->waits++;
        while (rd3_fq_reuse(&s->q, addr) == RD3_FQ_LEAVING && s->now < t0 + 3 * PERIOD)
            run_until(s, s->now + 100);
        u = rd3_fq_reuse(&s->q, addr);
        CHECK(u != RD3_FQ_LEAVING);
        if (s->now - t0 > s->wait_max) s->wait_max = s->now - t0;
    }
    if (u == RD3_FQ_SCANNED) s->inplace++;
    else if (u == RD3_FQ_ARRIVING) s->arriving++;
    else if (addr == s->inuse) s->wrote_on_panel++;
    inv(s);
}

/* after the script: the last present reaches the panel, none lost */
static void finish(struct sim *s, const char *name)
{
    run_until(s, s->now + 4 * PERIOD);
    CHECK(!s->pend && !s->q.inflight && !s->q.queued);
    if (s->newest_tok) CHECK(s->inuse == s->newest_addr);
    CHECK(s->nsub == s->nprog + s->replaced + s->joined + s->same);
    CHECK(s->done == s->nprog);
    CHECK(s->scanned_free == 0 && s->wrote_on_panel == 0);
    for (unsigned i = 0; i < s->nprog; i++) CHECK(s->prog_latch[i] > s->prog_t[i]);
    printf("  %s: %u presents: %u programmed, %u queued (%u replaced: newest wins), %u joined, %u the same buffer; "
           "%u writer wait(s) (max %llu us), %u in place, %u into an arriving buffer\n", name, s->nsub, s->nprog,
           s->queued, s->replaced, s->joined, s->same, s->waits, (unsigned long long)s->wait_max, s->inplace,
           s->arriving);
}

/* card run 2026-10-18b, head 1, 11:51:47 (ms of the trace's clock): a copy
 * forward into the next buffer, then submitFlipBuffer of it (#3-#21) */
struct step { double ms; char kind; uint64_t addr; };   /* 'c' copy forward target, 'f' flip-buffer present */
#define A1 0x8a020000ull
#define B1 0x8a810000ull
#define C1 0x88060000ull
static const struct step burst18b[] = {
    { 90347.972, 'c', A1 }, { 90361.276, 'f', A1 }, { 90385.770, 'c', B1 }, { 90388.639, 'f', B1 },
    { 90402.710, 'c', C1 }, { 90404.598, 'f', C1 }, { 90419.304, 'c', A1 }, { 90422.046, 'f', A1 },
    { 90436.073, 'c', B1 }, { 90438.187, 'f', B1 }, { 90452.913, 'c', C1 }, { 90455.011, 'f', C1 },
    { 90471.422, 'f', A1 },
    { 90986.429, 'c', A1 }, { 90989.727, 'f', A1 }, { 91002.583, 'c', B1 }, { 91006.126, 'f', B1 },
    { 91036.464, 'c', C1 }, { 91039.908, 'f', C1 }, { 91058.017, 'f', A1 }, { 91068.806, 'c', B1 },
    { 91072.315, 'f', B1 }, { 91085.614, 'c', C1 }, { 91090.332, 'f', C1 }, { 91102.223, 'c', A1 },
    { 91106.174, 'f', A1 }, { 91118.499, 'c', B1 }, { 91122.150, 'f', B1 }, { 91135.491, 'c', C1 },
    { 91138.456, 'f', C1 }, { 91149.654, 'c', A1 }, { 91152.541, 'f', A1 }, { 91168.805, 'c', B1 },
    { 91172.517, 'f', B1 }, { 91185.625, 'c', C1 }, { 91189.214, 'f', C1 },
};
/* head 0, 11:54:12-16: copy forward into buf[1] (0x88850000, the buffer on
 * the panel), then the swap of it; buf[2] (empty) skipped */
#define A0 0x88850000ull
static const struct step mode0_h0[] = {
    { 235664.644, 'c', A0 }, { 235674.394, 's', A0 }, { 236211.919, 'c', A0 }, { 236224.552, 's', A0 },
    { 236981.497, 'c', A0 }, { 236991.414, 's', A0 }, { 237247.879, 'c', A0 }, { 237257.798, 's', A0 },
    { 238314.632, 'c', A0 }, { 238324.261, 's', A0 }, { 239397.842, 'c', A0 }, { 239407.088, 's', A0 },
};
/* head 1, 11:56:14-16 and 11:59:28: copy forward into 0x8a020000, the swap of it */
static const struct step mode0_h1[] = {
    { 358213.000, 'c', A1 }, { 358223.522, 's', A1 }, { 358460.000, 'c', A1 }, { 358470.082, 's', A1 },
    { 358645.000, 'c', A1 }, { 358653.594, 's', A1 }, { 358810.000, 'c', A1 }, { 358819.812, 's', A1 },
    { 359310.000, 'c', A1 }, { 359319.791, 's', A1 }, { 359530.000, 'c', A1 }, { 359536.760, 's', A1 },
    { 551580.000, 'c', A1 }, { 551589.579, 's', A1 }, { 551630.000, 'c', A1 }, { 551640.118, 's', A1 },
    { 551715.000, 'c', A1 }, { 551722.860, 's', A1 },
};

static void replay(const char *name, const struct step *st, unsigned n, int expect_waits)
{
    struct sim s;
    sim_init(&s);
    const double t0 = st[0].ms - 5.0;
    for (unsigned i = 0; i < n; i++) {
        const uint64_t t = (uint64_t)((st[i].ms - t0) * 1000.0);
        if (st[i].kind == 'c') write_buf(&s, t, st[i].addr);
        else present(&s, t, st[i].addr, st[i].kind == 's');
    }
    finish(&s, name);
    if (!expect_waits) CHECK(s.waits == 0);   /* WindowServer's thread never waited */
}

int main(void)
{
    /* the queue's rules, one by one */
    {
        struct rd3_fq q;
        uint32_t d = 99, tok = 0, ntok = 0;
        int had = 0;
        uint64_t na = 0;
        rd3_fq_init(&q);
        CHECK(rd3_fq_submit(&q, 0, 1, &d) == RD3_FQ_IDLE);
        CHECK(rd3_fq_submit(&q, 0x1000, 1, &d) == RD3_FQ_PROGRAM && q.inflight == 0x1000);
        CHECK(rd3_fq_submit(&q, 0x1000, 2, &d) == RD3_FQ_JOINED && q.joined == 1);
        CHECK(rd3_fq_submit(&q, 0x2000, 3, &d) == RD3_FQ_QUEUED && q.queued == 0x2000);
        CHECK(rd3_fq_submit(&q, 0x3000, 4, &d) == RD3_FQ_REPLACED && d == 3 && q.queued == 0x3000);
        /* the in-flight address again with a newer one queued: queued too (newest wins) */
        CHECK(rd3_fq_submit(&q, 0x1000, 5, &d) == RD3_FQ_REPLACED && d == 4 && q.queued == 0x1000);
        /* while 0x1000 is in flight, the buffer before it (unknown here) and 0x1000 itself */
        CHECK(rd3_fq_reuse(&q, 0x1000) == RD3_FQ_ARRIVING && rd3_fq_reuse(&q, 0x4000) == RD3_FQ_FREE);
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_PROGRAM && had && tok == 1 && na == 0x1000 &&
              ntok == 5 && q.shown == 0x1000 && q.inflight == 0x1000 && !q.queued);
        /* the same address in flight again: the panel's buffer is not "leaving" */
        CHECK(rd3_fq_reuse(&q, 0x1000) == RD3_FQ_SCANNED);
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE && had && tok == 5 && !q.inflight);
        CHECK(rd3_fq_reuse(&q, 0x1000) == RD3_FQ_SCANNED);
        CHECK(rd3_fq_submit(&q, 0x2000, 6, &d) == RD3_FQ_PROGRAM);
        CHECK(rd3_fq_reuse(&q, 0x1000) == RD3_FQ_LEAVING && rd3_fq_reuse(&q, 0x2000) == RD3_FQ_ARRIVING);
        CHECK(rd3_fq_in_use(&q, 0x1000) && rd3_fq_in_use(&q, 0x2000) && !rd3_fq_in_use(&q, 0x3000));
        /* refused: nothing in flight, its token back, the panel's record stays */
        CHECK(rd3_fq_refused(&q) == 6 && !q.inflight && q.shown == 0x1000);
        /* Busy: kept behind the other path's flip, programmed at its latch, the panel's record unknown then */
        CHECK(rd3_fq_submit(&q, 0x2000, 7, &d) == RD3_FQ_PROGRAM);
        CHECK(rd3_fq_busy(&q, &d) == 0 && q.inflight == RD3_FQ_FOREIGN && q.queued == 0x2000 && q.queued_tok == 7);
        CHECK(rd3_fq_reuse(&q, 0x1000) == RD3_FQ_LEAVING);   /* another flip pending: still leaving */
        CHECK(rd3_fq_submit(&q, 0x3000, 8, &d) == RD3_FQ_REPLACED && d == 7);
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_PROGRAM && !had && na == 0x3000 && ntok == 8 &&
              q.shown == 0);
        /* a buffer freed: its queued present dropped, the record forgets it */
        CHECK(rd3_fq_submit(&q, 0x4000, 9, &d) == RD3_FQ_QUEUED);
        CHECK(rd3_fq_forget_addr(&q, 0x4000, &d) == 1 && d == 9 && !q.queued);
        CHECK(rd3_fq_forget_addr(&q, 0x5000, &d) == 0 && rd3_fq_forget_addr(&q, 0, &d) == 0);
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE && had && tok == 8 && q.shown == 0x3000);
        CHECK(rd3_fq_forget_addr(&q, 0x3000, &d) == 0 && q.shown == 0);
        /* a mode change: the queued present dropped, the in-flight one still seen latched */
        CHECK(rd3_fq_submit(&q, 0x1000, 10, &d) == RD3_FQ_PROGRAM);
        CHECK(rd3_fq_submit(&q, 0x2000, 11, &d) == RD3_FQ_QUEUED);
        CHECK(rd3_fq_forget(&q, &d) == 1 && d == 11 && !q.queued && q.inflight == 0x1000 && !q.shown);
        CHECK(rd3_fq_forget(&q, &d) == 0);
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE && had && tok == 10);
        /* the FOREIGN marker is no address */
        CHECK(rd3_fq_submit(&q, RD3_FQ_FOREIGN, 12, &d) == RD3_FQ_IDLE);
        /* Busy with a newer present already queued (from the flip-done path): the newer one wins */
        CHECK(rd3_fq_submit(&q, 0x2000, 13, &d) == RD3_FQ_PROGRAM);
        q.queued = 0x3000; q.queued_tok = 14;
        CHECK(rd3_fq_busy(&q, &d) == 1 && d == 13 && q.queued == 0x3000 && q.inflight == RD3_FQ_FOREIGN);
        CHECK(rd3_fq_busy(&q, &d) == 0);   /* already behind another flip */
    }
    /* the IOP zero-copy flip: VRAM newest AND written by the plugin (its cookie, this allocation) */
    {
        CHECK(rd3_iop_flip_ok(1, 0x5000, 0x5000));
        CHECK(!rd3_iop_flip_ok(0, 0x5000, 0x5000));    /* system memory newer */
        CHECK(!rd3_iop_flip_ok(1, 0, 0x5000));         /* never written by the plugin */
        CHECK(!rd3_iop_flip_ok(1, 0x4000, 0x5000));    /* written into an earlier allocation */
        CHECK(!rd3_iop_flip_ok(1, 0, 0));
    }

    printf("test_flip_queue: card run 2026-10-18b's sequences\n");
    replay("head 1 flip-buffer burst (3 buffers)", burst18b, sizeof burst18b / sizeof burst18b[0], 0);
    replay("head 0 mode 0 (buf[1] every swap)", mode0_h0, sizeof mode0_h0 / sizeof mode0_h0[0], 0);
    replay("head 1 mode 0 (0x8a020000 every swap)", mode0_h1, sizeof mode0_h1 / sizeof mode0_h1[0], 0);

    /* a presenter faster than the panel: two buffers, a write into the next
     * one 1 ms before each present, every 4-7 ms */
    {
        struct sim s;
        sim_init(&s);
        uint64_t t = 1000;
        for (unsigned i = 0; i < 1500; i++) {
            const uint64_t b = (i & 1) ? 0x20000000ull : 0x30000000ull;
            t += 4000 + rnd(3000);
            write_buf(&s, t - 1000, b);
            present(&s, t, b, 0);
        }
        finish(&s, "2 buffers, presents every 4-7 ms");
        /* the writer into the buffer leaving the panel waits for its latch (the panel's rate paces it) */
        CHECK(s.waits > 0);
        CHECK(s.wait_max <= PERIOD + WORKER);   /* a writer waits at most a frame */
    }
    /* four buffers, presents every 3-6 ms: frames replaced in the queue (newest wins) */
    {
        struct sim s;
        sim_init(&s);
        uint64_t t = 1000;
        for (unsigned i = 0; i < 1500; i++) {
            const uint64_t b = 0x10000000ull * (1 + (i & 3));
            t += 3000 + rnd(3000);
            write_buf(&s, t - 800, b);
            present(&s, t, b, 0);
        }
        finish(&s, "4 buffers, presents every 3-6 ms");
        CHECK(s.replaced > 0);
        CHECK(s.wait_max <= PERIOD + WORKER);
    }
    /* presents phase-locked right at the vblank: one frame of latency in the queue, every frame still shown */
    {
        struct sim s;
        sim_init(&s);
        for (unsigned i = 0; i < 600; i++) {
            const uint64_t b = 0x40000000ull + (i % 3) * 0x01000000ull;
            const uint64_t t = (uint64_t)(i + 1) * PERIOD - 100 + rnd(200);
            write_buf(&s, t - 2500, b);
            present(&s, t, b, 0);
        }
        finish(&s, "3 buffers presented at the vblank");
    }
    /* three buffers at the panel's rate after its vblank: nothing queued, no writer ever waits */
    {
        struct sim s;
        sim_init(&s);
        for (unsigned i = 0; i < 2000; i++) {
            const uint64_t b = 0x40000000ull + (i % 3) * 0x01000000ull;
            /* WindowServer presents after its VBL callback (18b: ~2.5 ms after a vblank), with jitter */
            const uint64_t t = (uint64_t)(i + 1) * PERIOD + 1000 + rnd(11000);
            write_buf(&s, t - 800, b);
            present(&s, t, b, 0);
        }
        finish(&s, "3 buffers, a present 1-12 ms after each vblank");
        CHECK(s.waits == 0 && s.queued == 0);
    }
    /* another path's flip pending at a present (an IOP or home flip): Busy, kept, programmed at its latch */
    {
        struct sim s;
        sim_init(&s);
        present(&s, 1000, 0x50000000ull, 0);
        run_until(&s, 3 * PERIOD);
        s.req = HOME; s.pend = 1; s.foreign = 1;   /* the other path's flip */
        present(&s, 3 * PERIOD + 2000, 0x60000000ull, 0);
        CHECK(s.q.inflight == RD3_FQ_FOREIGN && s.q.queued == 0x60000000ull);
        present(&s, 3 * PERIOD + 4000, 0x70000000ull, 0);   /* newest wins behind it */
        finish(&s, "a present behind another path's flip");
        CHECK(s.inuse == 0x70000000ull);
    }

    if (fails) {
        printf("test_flip_queue: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_flip_queue: all checks passed\n");
    return 0;
}
