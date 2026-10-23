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

enum { PERIOD = 16667, WORKER = 300, MAXREC = 8192, CAP = 35000 };
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
    int stuck;               /* the head's flips never latch (blanked, DPMS off, a stuck HUBP) */
    int abandoned;           /* the pending flip is ours, given up by the queue (completed early) */
    int iop_flipped;         /* the kernel's gIopFlipped: an IOP flip of ours moved the head off home */
    uint64_t latch_t;        /* when the last flip latched */
    uint64_t next_vbl, worker_t;
    uint64_t inflight_t;     /* when the queue's in-flight flip (ours or another path's) began: its cap runs from it */
    int worker_due;
    /* records */
    uint32_t prog_tok[MAXREC];
    uint64_t prog_t[MAXREC], prog_latch[MAXREC];
    unsigned char prog_early[MAXREC];
    unsigned nprog;
    unsigned nsub, same, queued, replaced, joined, done, refused, dropped;
    unsigned busy, foreign_to, giveup, forced, early;
    unsigned copied;         /* IOP copy fallbacks: a frame copied onto home, its transaction complete at once */
    uint32_t last_copy_tok;  /* the newest copied frame: no older present is ever programmed after it */
    uint32_t newest_tok;
    uint64_t newest_addr;
    unsigned waits, inplace, arriving, scanned_free, wrote_on_panel;
    uint64_t wait_max;
    /* per present (token): completions (an IOP transaction's pipe signalled: exactly once), when submitted and
     * when it left the queue (programmed, replaced, joined, the same buffer, dropped) */
    unsigned char comp[MAXREC];
    uint64_t sub_t[MAXREC], res_t[MAXREC];
};

static void sim_init(struct sim *s)
{
    memset(s, 0, sizeof *s);
    rd3_fq_init(&s->q);
    s->inuse = HOME;
    s->next_vbl = PERIOD;
}

/* the present's transaction completes (the accelerator signals its pipe) */
static void complete(struct sim *s, uint32_t tok)
{
    CHECK(tok && tok <= s->nsub);
    if (tok && tok < MAXREC) {
        CHECK(s->comp[tok] == 0);   /* exactly once */
        s->comp[tok]++;
    }
}
/* the present left the queue (programmed or completed without a flip of its own) */
static void resolve(struct sim *s, uint32_t tok)
{
    if (tok && tok < MAXREC && !s->res_t[tok])
        s->res_t[tok] = s->now ? s->now : 1;
}

static void hw_program(struct sim *s, uint64_t addr, uint32_t tok, int force)
{
    CHECK(!s->pend || force);              /* at most one flip in flight (but the framebuffer's flip anyway) */
    CHECK(tok == s->newest_tok);           /* the newest present each time */
    if (s->nprog) CHECK(tok > s->prog_tok[s->nprog - 1]);   /* in order */
    CHECK(tok > s->last_copy_tok);         /* never over a newer copied frame */
    s->iop_flipped = 1;                    /* the model's presents are IOP transactions (rd3AccelPqProgramLocked) */
    s->req = addr;
    s->pend = 1;
    s->foreign = 0;
    s->abandoned = 0;
    s->inflight_t = s->now;
    resolve(s, tok);
    if (s->nprog < MAXREC) {
        s->prog_tok[s->nprog] = tok;
        s->prog_t[s->nprog] = s->now;
        s->prog_latch[s->nprog] = 0;
        s->prog_early[s->nprog] = 0;
        s->nprog++;
    }
}

/* the accelerator's rd3AccelPqProgramLocked and what follows it: with NOWAIT the framebuffer answers Busy
 * while a flip is pending (rd3_fq_busy keeps the present); 'force' (rd3_fq_verdict: the head stuck) flips
 * anyway after the framebuffer's 20 ms */
static void program(struct sim *s, uint64_t addr, uint32_t tok, int force)
{
    uint32_t d = 0;
    if (s->pend && !force) {
        s->busy++;
        if (rd3_fq_busy(&s->q, &d)) {   /* never from the accelerator's two callers: their slot is empty */
            CHECK(0);
            complete(s, d);
            resolve(s, d);
        }
        s->inflight_t = s->now;
        return;
    }
    if (s->pend)
        s->forced++;
    hw_program(s, addr, tok, force);
}

/* the invariants after every step */
static void inv(const struct sim *s)
{
    if (s->stuck || s->abandoned)
        return;   /* a flip given up stays pending on the head: checked by the stuck test's bounds */
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

/* the flip-done path's verdict (rd3AccelPqLatchedLocked): 'latched' 1 its look saw the latch, 0 the flip
 * was given up (the cap ran out) */
static void verdict(struct sim *s, int latched)
{
    int had = 0, fg = 0, force = 0;
    uint32_t tok = 0, ntok = 0;
    uint64_t naddr = 0;
    const enum rd3_fq_act a = rd3_fq_verdict(&s->q, latched, &had, &tok, &naddr, &ntok, &fg, &force);
    if (had) {
        unsigned i;
        s->done++;
        complete(s, tok);
        for (i = 0; i < s->nprog && s->prog_tok[i] != tok; i++) {}
        CHECK(i < s->nprog);
        if (latched && i < s->nprog) {
            /* its flip latched before this completion */
            s->prog_latch[i] = s->latch_t;
            CHECK(s->latch_t > s->prog_t[i] && s->now >= s->latch_t);
        } else if (i < s->nprog) {
            s->prog_early[i] = 1;
            s->early++;
            if (s->pend && !s->foreign)
                s->abandoned = 1;
        }
    }
    if (fg)
        s->foreign_to++;
    if (a == RD3_FQ_PROGRAM) {
        if (force)
            s->giveup++;
        program(s, naddr, ntok, force);
    }
    inv(s);
}

/* a latch the flip-done path has not seen yet, taken by a present or a writer (rd3AccelPqLookLocked) */
static void look(struct sim *s)
{
    if (s->q.inflight && !s->pend)
        verdict(s, 1);
}

static void run_until(struct sim *s, uint64_t t)
{
    for (;;) {
        const uint64_t nw = s->worker_due ? s->worker_t : UINT64_MAX;
        const uint64_t nt = s->q.inflight && s->pend ? s->inflight_t + CAP : UINT64_MAX;
        uint64_t n = s->next_vbl < nw ? s->next_vbl : nw;
        if (nt < n)
            n = nt;
        if (n > t)
            break;
        s->now = n;
        if (n == nt) {
            verdict(s, 0);   /* the worker's cap ran out: given up */
            continue;
        }
        if (n == nw) {
            s->worker_due = 0;
            look(s);
            continue;
        }
        if (s->pend && !s->stuck) {
            s->inuse = s->foreign ? HOME : s->req;
            s->pend = 0;
            s->foreign = 0;
            s->abandoned = 0;
            s->latch_t = s->now;
        }
        s->worker_due = 1;
        s->worker_t = s->now + WORKER;
        s->next_vbl += PERIOD;
    }
    if (t > s->now)
        s->now = t;
}

/* a present (a swap, a flip-buffer call, an IOP transaction) in the accelerator's order
 * (rd3AccelPresentSubmit): the latch look first, then rd3_fq_present (the same buffer only with nothing in
 * flight or queued: the HUBP's address read when rd3_fq_same_wants) */
static void present(struct sim *s, uint64_t t, uint64_t addr, int use_same)
{
    uint32_t dropped = 0;
    run_until(s, t);
    look(s);
    const uint32_t tok = ++s->nsub;
    if (tok < MAXREC)
        s->sub_t[tok] = s->now;
    s->newest_tok = tok;
    s->newest_addr = addr;
    const int want = use_same && rd3_fq_same_wants(&s->q, addr);
    switch (rd3_fq_present(&s->q, addr, tok, use_same, want, want ? s->inuse : 0, want ? s->pend : 0, &dropped)) {
    case RD3_FQ_SAME:
        CHECK(s->inuse == addr && !s->pend);
        s->same++;
        complete(s, tok);
        resolve(s, tok);
        break;
    case RD3_FQ_PROGRAM:
        if (s->pend) {
            /* another path's flip (or one given up) is still pending: the framebuffer answers Busy */
            CHECK(s->foreign || s->stuck || s->abandoned);
            s->queued++;
        }
        program(s, addr, tok, 0);
        break;
    case RD3_FQ_QUEUED:
        s->queued++;
        break;
    case RD3_FQ_REPLACED:
        /* the older queued present never reaches the panel: its transaction completes now (ECC review) */
        s->queued++;
        s->replaced++;
        CHECK(dropped && dropped < tok);
        complete(s, dropped);
        resolve(s, dropped);
        break;
    case RD3_FQ_JOINED:
        /* the in-flight flip's own address: its transaction completes now (ECC review) */
        s->joined++;
        complete(s, tok);
        resolve(s, tok);
        break;
    default:
        CHECK(0);
        break;
    }
    inv(s);
}

/* a writer the driver sees (copy forward's target, a page-on) */
static void write_buf(struct sim *s, uint64_t t, uint64_t addr)
{
    run_until(s, t);
    /* the gate reads the HUBP before it waits: a latch not seen yet is taken here */
    look(s);
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

/* a display mode change (rd3AccelPresentForgetAll): the queued present dropped, its transaction completes */
static void mode_change(struct sim *s, uint64_t t)
{
    uint32_t d = 0;
    run_until(s, t);
    if (rd3_fq_forget(&s->q, &d)) {
        s->dropped++;
        complete(s, d);
        resolve(s, d);
    }
    inv(s);
}

/* a buffer freed (rd3AccelPresentForgetVm): a queued present of it dropped */
static void vm_free(struct sim *s, uint64_t t, uint64_t addr)
{
    uint32_t d = 0;
    run_until(s, t);
    if (rd3_fq_forget_addr(&s->q, addr, &d)) {
        s->dropped++;
        complete(s, d);
        resolve(s, d);
    }
    CHECK(s->q.queued != addr && s->q.shown != addr);
    inv(s);
}

/* the copy fallback's frame (rd3AccelIopTransaction after rd3AccelIopHome): copied onto home, its
 * transaction complete at once; it is the newest frame */
static void copy_frame(struct sim *s)
{
    const uint32_t tok = ++s->nsub;
    if (tok < MAXREC)
        s->sub_t[tok] = s->now;
    s->newest_tok = tok;
    s->newest_addr = HOME;
    s->last_copy_tok = tok;
    s->copied++;
    complete(s, tok);
    resolve(s, tok);
}

/* the IOP copy fallback (rd3AccelIopHome, then the copy). Under the queue's lock: with the head home already
 * (gIopFlipped clear) no flip, a queued IOP present dropped - programmed at the next latch it would cover the
 * copied frame (ECC review round 2). Else the look, the flip home - the framebuffer waits up to 20 ms for the
 * pending flip, then programs home - then rd3_fq_moved */
static void flip_home(struct sim *s, uint64_t t)
{
    int had = 0;
    uint32_t tok = 0, d = 0;
    run_until(s, t);
    if (!s->iop_flipped) {
        if (rd3_fq_drop_queued(&s->q, &d)) {
            s->dropped++;
            complete(s, d);
            resolve(s, d);
        }
        copy_frame(s);
        inv(s);
        return;
    }
    look(s);
    if (s->pend && !s->stuck) {
        /* the framebuffer's wait: the pending flip latches at the next vblank (the worker is held off by the
         * lock: its look comes later and finds the home flip) */
        const uint64_t v = s->next_vbl;
        s->inuse = s->foreign ? HOME : s->req;
        s->pend = 0;
        s->foreign = 0;
        s->abandoned = 0;
        s->latch_t = v;
        s->now = v + 1;
        s->next_vbl += PERIOD;
        s->worker_due = 0;
    }
    if (rd3_fq_moved(&s->q, &had, &tok, &d)) {
        s->dropped++;
        complete(s, d);
        resolve(s, d);
    }
    if (had) {
        unsigned i;
        s->done++;
        complete(s, tok);
        for (i = 0; i < s->nprog && s->prog_tok[i] != tok; i++) {}
        CHECK(i < s->nprog);
        if (i < s->nprog && s->latch_t > s->prog_t[i])
            s->prog_latch[i] = s->latch_t;   /* the framebuffer waited for its latch before flipping home */
        else if (i < s->nprog)
            s->prog_early[i] = 1;
    }
    s->req = HOME;
    s->pend = 1;
    s->foreign = 1;
    s->inflight_t = s->now;
    s->iop_flipped = 0;
    copy_frame(s);
    inv(s);
}

/* after the script: the last present reaches the panel, none lost, each transaction complete once */
static void finish(struct sim *s, const char *name)
{
    run_until(s, s->now + 6 * CAP);
    CHECK(!s->pend && !s->q.inflight && !s->q.queued);
    if (s->newest_tok) CHECK(s->inuse == s->newest_addr);
    CHECK(s->nsub == s->nprog + s->replaced + s->joined + s->same + s->dropped + s->copied);
    CHECK(s->done == s->nprog);
    CHECK(s->scanned_free == 0 && s->wrote_on_panel == 0);
    for (unsigned i = 0; i < s->nprog && i < MAXREC; i++)
        CHECK(s->prog_early[i] || s->prog_latch[i] > s->prog_t[i]);
    for (unsigned k = 1; k <= s->nsub && k < MAXREC; k++)
        CHECK(s->comp[k] == 1 && s->res_t[k]);
    printf("  %s: %u presents: %u programmed, %u queued (%u replaced: newest wins), %u joined, %u the same buffer, "
           "%u dropped; %u writer wait(s) (max %llu us), %u in place, %u into an arriving buffer; %u Busy, %u early, "
           "%u forced\n", name, s->nsub, s->nprog, s->queued, s->replaced, s->joined, s->same, s->dropped, s->waits,
           (unsigned long long)s->wait_max, s->inplace, s->arriving, s->busy, s->early, s->forced);
}

/* the longest a present waited in the queue before it was programmed or completed */
static uint64_t max_queue_us(const struct sim *s)
{
    uint64_t m = 0;
    for (unsigned k = 1; k <= s->nsub && k < MAXREC; k++)
        if (s->res_t[k] > s->sub_t[k] && s->res_t[k] - s->sub_t[k] > m)
            m = s->res_t[k] - s->sub_t[k];
    return m;
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
        /* a newer frame copied onto the panel already home: only the queued present goes */
        q.queued = 0x4000; q.queued_tok = 15; q.shown = 0; q.stuck_run = 2;
        CHECK(rd3_fq_drop_queued(&q, &d) == 1 && d == 15 && !q.queued && !q.queued_tok &&
              q.inflight == RD3_FQ_FOREIGN && q.stuck_run == 2);
        CHECK(rd3_fq_drop_queued(&q, &d) == 0);
    }
    /* the IOP zero-copy flip: VRAM newest AND written by the plugin (its generation: this allocation) */
    {
        CHECK(rd3_iop_flip_ok(1, 7, 7));
        CHECK(!rd3_iop_flip_ok(0, 7, 7));    /* system memory newer */
        CHECK(!rd3_iop_flip_ok(1, 0, 7));    /* never written by the plugin */
        CHECK(!rd3_iop_flip_ok(1, 6, 7));    /* written into an earlier allocation (same VRAM offset, new generation) */
        CHECK(!rd3_iop_flip_ok(1, 0, 0));
    }
    /* the same buffer (rd3_fq_present): skipped only with nothing in flight or queued (ECC review of N38) */
    {
        struct rd3_fq q;
        uint32_t d = 0, tok = 0, ntok = 0;
        int had = 0, fg = 0, force = 0;
        uint64_t na = 0;
        rd3_fq_init(&q);
        CHECK(!rd3_fq_same_wants(&q, 0x1000));                     /* nothing recorded: flipped */
        CHECK(rd3_fq_present(&q, 0x1000, 1, 1, 1, 0x1000, 0, &d) == RD3_FQ_PROGRAM);
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE && q.shown == 0x1000);
        CHECK(rd3_fq_same_wants(&q, 0x1000) && !rd3_fq_same_wants(&q, 0x2000));
        CHECK(rd3_fq_present(&q, 0x1000, 2, 1, 1, 0x1000, 0, &d) == RD3_FQ_SAME && !q.inflight);
        CHECK(rd3_fq_present(&q, 0x1000, 3, 0, 1, 0x1000, 0, &d) == RD3_FQ_PROGRAM);   /* samenoflip 0 */
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE && tok == 3);
        CHECK(rd3_fq_present(&q, 0x1000, 4, 1, 0, 0, 0, &d) == RD3_FQ_PROGRAM);         /* HUBP unreadable */
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE);
        CHECK(rd3_fq_present(&q, 0x1000, 5, 1, 1, 0x2000, 0, &d) == RD3_FQ_PROGRAM);    /* another buffer scanned */
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE);
        CHECK(rd3_fq_present(&q, 0x1000, 6, 1, 1, 0x1000, 1, &d) == RD3_FQ_PROGRAM);    /* a flip pending */
        /* the HUBP already on 0x1000 but a present of 0x2000 queued behind its flip: 0x1000 is not skipped -
         * skipped, the queued 0x2000 would be programmed over the newer frame at the latch */
        CHECK(rd3_fq_present(&q, 0x2000, 7, 1, 1, 0x2000, 0, &d) == RD3_FQ_QUEUED);
        CHECK(!rd3_fq_same_wants(&q, 0x1000));
        CHECK(rd3_fq_present(&q, 0x1000, 8, 1, 1, 0x1000, 0, &d) == RD3_FQ_REPLACED && d == 7 && q.queued == 0x1000);
        /* the verdict: a latch seen ends the stuck run; another path's flip given up counts it, the queued
         * present forced from RD3_FQ_STUCK_RUN of them in a row */
        CHECK(rd3_fq_verdict(&q, 1, &had, &tok, &na, &ntok, &fg, &force) == RD3_FQ_PROGRAM && had && tok == 6 &&
              na == 0x1000 && ntok == 8 && !fg && !force && q.stuck_run == 0);
        CHECK(rd3_fq_busy(&q, &d) == 0 && q.inflight == RD3_FQ_FOREIGN);
        for (unsigned i = 1; i < RD3_FQ_STUCK_RUN; i++) {
            CHECK(rd3_fq_verdict(&q, 0, &had, &tok, &na, &ntok, &fg, &force) == RD3_FQ_PROGRAM && !had && fg &&
                  !force && q.stuck_run == i);
            CHECK(rd3_fq_busy(&q, &d) == 0);
        }
        CHECK(rd3_fq_verdict(&q, 0, &had, &tok, &na, &ntok, &fg, &force) == RD3_FQ_PROGRAM && fg && force &&
              na == 0x1000 && ntok == 8);
        /* our own flip given up: not another path's, the run stays; the next one still forced */
        CHECK(rd3_fq_submit(&q, 0x3000, 9, &d) == RD3_FQ_QUEUED);
        CHECK(rd3_fq_verdict(&q, 0, &had, &tok, &na, &ntok, &fg, &force) == RD3_FQ_PROGRAM && had && tok == 8 &&
              !fg && force && q.stuck_run == RD3_FQ_STUCK_RUN);
        CHECK(rd3_fq_verdict(&q, 1, &had, &tok, &na, &ntok, &fg, &force) == RD3_FQ_IDLE && had && tok == 9 &&
              !force && q.stuck_run == 0 && q.shown == 0x3000);
        /* another path flipped the head (the IOP copy fallback's home): the queued one dropped, the in-flight
         * one completes, that flip in flight, nothing of ours on the panel */
        CHECK(rd3_fq_submit(&q, 0x4000, 10, &d) == RD3_FQ_PROGRAM);
        CHECK(rd3_fq_submit(&q, 0x5000, 11, &d) == RD3_FQ_QUEUED);
        CHECK(rd3_fq_moved(&q, &had, &tok, &d) == 1 && d == 11 && had && tok == 10 && q.inflight == RD3_FQ_FOREIGN &&
              !q.queued && !q.shown);
        CHECK(rd3_fq_reuse(&q, 0x3000) == RD3_FQ_FREE && rd3_fq_reuse(&q, 0x4000) == RD3_FQ_FREE);
        CHECK(rd3_fq_moved(&q, &had, &tok, &d) == 0 && !had);   /* again: another path's flip already in flight */
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE && !had && !q.shown);
        /* a mode change wakes the writers waiting for a buffer leaving the panel: it is not leaving any more */
        CHECK(rd3_fq_submit(&q, 0x6000, 12, &d) == RD3_FQ_PROGRAM);
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE && q.shown == 0x6000);
        CHECK(rd3_fq_submit(&q, 0x7000, 13, &d) == RD3_FQ_PROGRAM && rd3_fq_reuse(&q, 0x6000) == RD3_FQ_LEAVING);
        q.stuck_run = 2;
        CHECK(rd3_fq_forget(&q, &d) == 0 && rd3_fq_reuse(&q, 0x6000) == RD3_FQ_FREE && q.stuck_run == 0);
        /* a buffer freed while a writer waits for it: not leaving any more either */
        CHECK(rd3_fq_latched(&q, &had, &tok, &na, &ntok) == RD3_FQ_IDLE && q.shown == 0x7000);
        CHECK(rd3_fq_submit(&q, 0x6000, 14, &d) == RD3_FQ_PROGRAM && rd3_fq_reuse(&q, 0x7000) == RD3_FQ_LEAVING);
        CHECK(rd3_fq_forget_addr(&q, 0x7000, &d) == 0 && rd3_fq_reuse(&q, 0x7000) == RD3_FQ_FREE);
        /* no VRAM address: nothing to program (the accelerator counts it refused) */
        CHECK(rd3_fq_present(&q, 0, 15, 1, 1, 0, 0, &d) == RD3_FQ_IDLE);
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

    /* ECC review of N38 (A): a present of the buffer the HUBP already scans, while an older present is queued
     * behind the flip in flight. The look runs first (it takes the latch and programs the queued one), then the
     * same-buffer check, which skips only with nothing in flight or queued - else the older queued frame would
     * be programmed over the newer one at the next latch */
    {
        struct sim s;
        const uint64_t A = 0x81000000ull, B = 0x82000000ull;
        sim_init(&s);
        present(&s, 1000, A, 1);
        run_until(&s, PERIOD + WORKER + 100);                    /* A latched and seen: the queue's record */
        present(&s, PERIOD + 2000, B, 1);                        /* programmed */
        present(&s, PERIOD + 3000, A, 1);                        /* queued behind B */
        CHECK(s.q.inflight == B && s.q.queued == A);
        present(&s, 2 * PERIOD + 50, B, 1);                      /* B latched, the worker not there yet */
        CHECK(s.q.inflight == A && s.q.queued == B && s.same == 0);
        finish(&s, "the buffer on the panel presented behind a queued one");
        CHECK(s.inuse == B);
    }
    /* IOP transactions faster than the panel: every replaced one completes at once, exactly once (E) */
    {
        struct sim s;
        sim_init(&s);
        uint64_t t = 500;
        for (unsigned i = 0; i < 900; i++) {
            t += 1500 + rnd(4000);
            present(&s, t, 0x90000000ull + (uint64_t)(i % 3) * 0x01000000ull, 0);
        }
        finish(&s, "IOP transactions every 1.5-5.5 ms");
        CHECK(s.replaced > 100);
        CHECK(max_queue_us(&s) <= PERIOD + WORKER);
    }
    /* a display mode change with a present queued (rd3AccelPresentForgetAll), then a buffer freed while
     * queued (rd3AccelPresentForgetVm): dropped, completed, never programmed */
    {
        struct sim s;
        sim_init(&s);
        present(&s, 1000, 0xa1000000ull, 0);
        present(&s, 2000, 0xa2000000ull, 0);                     /* queued */
        mode_change(&s, 3000);
        CHECK(s.dropped == 1 && !s.q.queued && s.q.inflight == 0xa1000000ull);
        run_until(&s, 2 * PERIOD);
        CHECK(s.inuse == 0xa1000000ull);
        present(&s, 2 * PERIOD + 1000, 0xa3000000ull, 0);
        present(&s, 2 * PERIOD + 2000, 0xa4000000ull, 0);        /* queued */
        vm_free(&s, 2 * PERIOD + 3000, 0xa4000000ull);
        vm_free(&s, 2 * PERIOD + 3100, 0xa1000000ull);           /* the buffer leaving the panel: forgotten */
        CHECK(s.dropped == 2 && rd3_fq_reuse(&s.q, 0xa1000000ull) == RD3_FQ_FREE);
        present(&s, 4 * PERIOD, 0xa5000000ull, 0);
        finish(&s, "a mode change and a freed buffer with presents queued");
        CHECK(s.inuse == 0xa5000000ull);
    }
    /* the IOP copy fallback after a queued flip (C): the copy goes onto home, so the head flips home; the
     * queued IOP flip must not be programmed over the copied frame later */
    {
        struct sim s;
        sim_init(&s);
        present(&s, 1000, 0xb1000000ull, 0);
        present(&s, 2000, 0xb2000000ull, 0);                     /* queued behind the first */
        flip_home(&s, 3000);
        CHECK(s.dropped == 1 && s.q.inflight == RD3_FQ_FOREIGN && !s.q.queued);
        run_until(&s, 3 * PERIOD);
        CHECK(s.inuse == HOME && !s.q.inflight && !s.q.shown);   /* the copied frame stays */
        present(&s, 3 * PERIOD + 1000, 0xb3000000ull, 0);
        finish(&s, "the IOP copy fallback flips home with a flip queued");
        CHECK(s.inuse == 0xb3000000ull);
        for (unsigned i = 0; i < s.nprog; i++) CHECK(s.prog_tok[i] != 2);   /* the dropped one never programmed */
    }
    /* ECC review round 2: a copy fallback (Z1) flipped home; a flippable IOP transaction (Y) queued behind
     * that flip home (not programmed: the head still home); a newer non-flippable one (Z2) takes the copy
     * fallback. The head is home already, so no flip, but Y is dropped - programmed at the home flip's latch
     * it would cover Z2's copied frame, a stale frame that stays if Z2 was the last update */
    {
        struct sim s;
        const uint64_t X = 0xe1000000ull, Y = 0xe2000000ull;
        sim_init(&s);
        present(&s, 1000, X, 0);                                 /* tok 1: an IOP flip, programmed */
        run_until(&s, PERIOD + WORKER + 100);                    /* X latched and seen */
        flip_home(&s, PERIOD + 2000);                            /* Z1 (tok 2): home in flight */
        CHECK(s.q.inflight == RD3_FQ_FOREIGN && !s.iop_flipped && s.inuse == X);
        present(&s, PERIOD + 3000, Y, 0);                        /* tok 3: queued behind the flip home */
        CHECK(s.q.queued == Y && s.q.queued_tok == 3 && !s.iop_flipped);
        flip_home(&s, PERIOD + 4000);                            /* Z2 (tok 4): copied, Y dropped */
        CHECK(s.dropped == 1 && !s.q.queued && s.q.inflight == RD3_FQ_FOREIGN && s.comp[3] == 1);
        finish(&s, "a copy fallback with an IOP flip queued behind the flip home");
        CHECK(s.inuse == HOME && s.copied == 2 && s.nprog == 1);
        for (unsigned i = 0; i < s.nprog; i++) CHECK(s.prog_tok[i] != 3);   /* Y never programmed */
    }
    /* a head whose flips never latch (blanked, DPMS off, a stuck HUBP; B): our flip given up, the next one
     * Busy behind it, that one given up RD3_FQ_STUCK_RUN times, then flipped anyway - each present programmed
     * or replaced in bounded time, every transaction complete; once the head latches again the run ends */
    {
        struct sim s;
        sim_init(&s);
        s.stuck = 1;
        uint64_t t = 1000;
        for (unsigned i = 0; i < 180; i++, t += PERIOD)
            present(&s, t, 0xc0000000ull + (uint64_t)(i & 1) * 0x01000000ull, 0);
        CHECK(s.foreign_to == RD3_FQ_STUCK_RUN);                 /* then forced: no more Busy waits */
        CHECK(s.giveup > 10 && s.forced == s.giveup && s.early > 10);
        CHECK(max_queue_us(&s) <= (RD3_FQ_STUCK_RUN + 2) * (uint64_t)CAP);
        const unsigned g = s.giveup;
        s.stuck = 0;                                             /* the head latches again */
        for (unsigned i = 0; i < 120; i++, t += PERIOD)
            present(&s, t, 0xc0000000ull + (uint64_t)(i & 1) * 0x01000000ull, 0);
        CHECK(s.giveup <= g + 1 && s.q.stuck_run == 0);
        finish(&s, "a head whose flips never latch, then latch again");
    }
    /* another path's flip pending forever (B): an IOP transaction behind it completes in bounded time */
    {
        struct sim s;
        sim_init(&s);
        s.stuck = 1;
        s.req = HOME; s.pend = 1; s.foreign = 1;
        present(&s, 1000, 0xd1000000ull, 0);
        CHECK(s.q.inflight == RD3_FQ_FOREIGN && s.q.queued == 0xd1000000ull);
        run_until(&s, 1000 + (RD3_FQ_STUCK_RUN + 1) * (uint64_t)CAP);
        CHECK(s.foreign_to == RD3_FQ_STUCK_RUN && s.giveup == 1 && s.forced == 1 && s.res_t[1]);
        run_until(&s, s.now + 2 * CAP);
        CHECK(s.comp[1] == 1);                                   /* given up too: its pipe signalled */
        s.stuck = 0;
        present(&s, s.now + 1000, 0xd2000000ull, 0);
        finish(&s, "a present behind another path's flip that never latches");
    }

    if (fails) {
        printf("test_flip_queue: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_flip_queue: all checks passed\n");
    return 0;
}
