/*
 * test_iop_latch.c - common/rdna3_iop_latch.h on the host (DRIVER-PLAN N68, bare metal 2026-10-31: a dragged
 * window steps unevenly although every refresh is presented).
 *
 * The K2 frame classes and scanline bins as the kext counts them; the latched copy's buffer choice over a model
 * of one head (home, D, the present queue's record, the HUBP's address): a frame never goes into the buffer the
 * panel scans or will scan, a flip in flight is waited for, and alternating presents land on alternating
 * buffers; D is freed only when the HUBP is seen elsewhere.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_iop_latch.h"   /* includes rdna3_flip_queue.h: the head's present queue, used by the models below */

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { HOME = 0x1000, D = 0x9000, E = 0xd000, SURF = 0x5000 };

static void test_txn(void)
{
    CHECK(!rd3_il_txn_slow(0) && !rd3_il_txn_slow(RD3_IL_SLOW_US) && rd3_il_txn_slow(RD3_IL_SLOW_US + 1));
}

static void test_vbl(void)
{
    uint32_t d = 99;
    CHECK(rd3_il_vbl_class(0, 0, 5, &d) == RD3_IL_VBL_FIRST && d == 0);
    CHECK(rd3_il_vbl_class(1, 5, 5, &d) == RD3_IL_VBL_SAME && d == 0);
    CHECK(rd3_il_vbl_class(1, 5, 6, &d) == RD3_IL_VBL_NEXT && d == 1);
    CHECK(rd3_il_vbl_class(1, 5, 8, &d) == RD3_IL_VBL_SKIP && d == 3);
    /* the 24-bit counter wraps: still the next refresh */
    CHECK(rd3_il_vbl_class(1, RD3_IL_FRAME_MASK, 0, &d) == RD3_IL_VBL_NEXT && d == 1);
    /* the counter went back (a mode set restarted the OTG): a new start, not 16 million refreshes */
    CHECK(rd3_il_vbl_class(1, 1000, 10, &d) == RD3_IL_VBL_FIRST && d == 0);
    CHECK(rd3_il_vbl_class(1, 1, 2, NULL) == RD3_IL_VBL_NEXT);

    CHECK(rd3_il_scan_bin(0, 0, 1080) == 0);
    CHECK(rd3_il_scan_bin(269, 0, 1080) == 0 && rd3_il_scan_bin(270, 0, 1080) == 1);
    CHECK(rd3_il_scan_bin(539, 0, 1080) == 1 && rd3_il_scan_bin(540, 0, 1080) == 2);
    CHECK(rd3_il_scan_bin(810, 0, 1080) == 3 && rd3_il_scan_bin(1079, 0, 1080) == 3);
    CHECK(rd3_il_scan_bin(1080, 0, 1080) == RD3_IL_BIN_BLANK && rd3_il_scan_bin(1100, 0, 1080) == RD3_IL_BIN_BLANK);
    CHECK(rd3_il_scan_bin(10, 1, 1080) == RD3_IL_BIN_BLANK);   /* the OTG says blank */
    CHECK(rd3_il_scan_bin(10, 0, 0) == RD3_IL_BIN_BLANK);      /* no height known */
}

static struct rd3_il_state st(uint64_t shown, uint64_t inflight, uint64_t queued, int known, uint64_t scan,
                              uint64_t last)
{
    struct rd3_il_state s;
    memset(&s, 0, sizeof s);
    s.home = HOME; s.d = D;
    s.shown = shown; s.inflight = inflight; s.queued = queued;
    s.scan_known = known; s.scan = scan; s.last = last;
    return s;
}

static void test_pick(void)
{
    struct rd3_il_state s = st(0, 0, 0, 1, HOME, 0);
    /* no D or a D equal to home: no latched copy */
    s.d = 0;
    CHECK(rd3_il_pick(&s) == RD3_IL_NONE);
    s.d = HOME;
    CHECK(rd3_il_pick(&s) == RD3_IL_NONE);
    /* first frame after boot: the HUBP scans home -> D */
    s = st(0, 0, 0, 1, HOME, 0);
    CHECK(rd3_il_pick(&s) == RD3_IL_D);
    /* the same with the HUBP unreadable: nothing of ours known, D (home is where the boot left it) */
    s = st(0, 0, 0, 0, 0, 0);
    CHECK(rd3_il_pick(&s) == RD3_IL_D);
    /* D latched: home next */
    s = st(D, 0, 0, 1, D, D);
    CHECK(rd3_il_pick(&s) == RD3_IL_HOME);
    s = st(D, 0, 0, 0, 0, D);
    CHECK(rd3_il_pick(&s) == RD3_IL_HOME);
    /* a flip in flight (ours or another path's): wait for its latch */
    s = st(HOME, D, 0, 1, HOME, D);
    CHECK(rd3_il_pick(&s) == RD3_IL_WAIT);
    s = st(0, 1 /* RD3_FQ_FOREIGN */, 0, 1, HOME, 0);
    CHECK(rd3_il_pick(&s) == RD3_IL_WAIT);
    /* the head on a zero-copy IOSurface: both free, alternate from the last */
    s = st(SURF, 0, 0, 1, SURF, D);
    CHECK(rd3_il_pick(&s) == RD3_IL_HOME);
    s = st(SURF, 0, 0, 1, SURF, HOME);
    CHECK(rd3_il_pick(&s) == RD3_IL_D);
    /* the HUBP is the truth: a stale queue record (shown D) does not keep D while the HUBP scans home */
    s = st(D, 0, 0, 1, HOME, D);
    CHECK(rd3_il_pick(&s) == RD3_IL_D);
    /* HUBP unknown, the queue says home, our last flip said D (a fallback flipped home since): neither free */
    s = st(HOME, 0, 0, 0, 0, D);
    CHECK(rd3_il_pick(&s) == RD3_IL_NONE);
}

/* one head over 600 presents: the model HUBP latches every flip at the next vblank, the transaction waits for it;
 * the buffer written is never the one scanned, and the targets alternate */
static void test_model(void)
{
    uint64_t scan = HOME, inflight = 0, last = 0, shown = 0;
    unsigned waits = 0, n_d = 0, n_home = 0, written_scanned = 0, repeats = 0;
    uint64_t prev_target = 0;
    for (unsigned i = 0; i < 600; i++) {
        struct rd3_il_state s = st(shown, inflight, 0, (i % 7) != 3, scan, last);   /* every 7th: HUBP unreadable */
        enum rd3_il_pick p = rd3_il_pick(&s);
        if (p == RD3_IL_WAIT) {
            /* the latch: the worker completes the flip, the HUBP takes it */
            waits++;
            scan = inflight; shown = inflight; inflight = 0;
            s = st(shown, inflight, 0, (i % 7) != 3, scan, last);
            p = rd3_il_pick(&s);
        }
        CHECK(p == RD3_IL_HOME || p == RD3_IL_D);
        const uint64_t target = p == RD3_IL_D ? D : HOME;
        if (target == scan) written_scanned++;
        if (target == prev_target) repeats++;
        prev_target = target;
        if (p == RD3_IL_D) n_d++; else n_home++;
        inflight = target;   /* programmed, NOWAIT: in flight until the next vblank */
        last = target;
        if (i % 2) { scan = inflight; shown = inflight; inflight = 0; }   /* half the frames latch before the next */
    }
    CHECK(written_scanned == 0);
    CHECK(repeats == 0);
    CHECK(n_d == 300 && n_home == 300);
    CHECK(waits == 300);
}

static void test_freeable(void)
{
    struct rd3_il_state s = st(0, 0, 0, 1, HOME, D);
    CHECK(rd3_il_d_freeable(&s));                    /* the HUBP on home, nothing of D queued */
    s = st(0, 0, 0, 1, D, D);
    CHECK(!rd3_il_d_freeable(&s));                   /* scanned */
    s = st(0, D, 0, 1, HOME, D);
    CHECK(!rd3_il_d_freeable(&s));                   /* in flight */
    s = st(0, 1, D, 1, HOME, D);
    CHECK(!rd3_il_d_freeable(&s));                   /* queued */
    s = st(0, 1 /* RD3_FQ_FOREIGN */, 0, 1, HOME, D);
    CHECK(!rd3_il_d_freeable(&s));                   /* C1: a pending flip of unknown target (it may be to D) */
    s = st(0, HOME, 0, 1, D, D);
    CHECK(!rd3_il_d_freeable(&s));                   /* the flip away from D not latched: still scanned */
    s = st(0, HOME, 0, 1, HOME, D);
    CHECK(!rd3_il_d_freeable(&s));                   /* any flip in flight: kept until it latched */
    s = st(D, 0, 0, 1, HOME, D);
    CHECK(!rd3_il_d_freeable(&s));                   /* the queue's record still says D: next time */
    s = st(0, 0, 0, 0, 0, HOME);
    CHECK(!rd3_il_d_freeable(&s));                   /* the HUBP unreadable: kept */
    s = st(0, 0, 0, 1, HOME, 0);
    s.d = 0;
    CHECK(!rd3_il_d_freeable(&s));
}

/* ---- N68 (a): the mailbox (rd3_il_mb_pick, rd3_fq_take_queued) ---- */
static struct rd3_il_state st3(uint64_t shown, uint64_t inflight, uint64_t queued, int qlb, int known, uint64_t scan,
                               uint64_t last)
{
    struct rd3_il_state s = st(shown, inflight, queued, known, scan, last);
    s.e = E;
    s.queued_lb = qlb;
    return s;
}

static void test_mb_pick(void)
{
    unsigned i = 9;
    struct rd3_il_state s = st3(0, 0, 0, 0, 1, HOME, 0);
    /* no E, or two buffers of one address: not the mailbox (the caller keeps two buffers) */
    s.e = 0;
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_NONE);
    s.e = D;
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_NONE);
    /* first frame: the HUBP on home -> D (round robin from home) */
    s = st3(0, 0, 0, 0, 1, HOME, HOME);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_COPY && i == 1);
    /* D in flight, home scanned: E, no wait (the two-buffer pick waits here) */
    s = st3(HOME, D, 0, 0, 1, HOME, D);
    CHECK(rd3_il_pick(&s) == RD3_IL_WAIT);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_COPY && i == 2);
    /* home scanned, D in flight, E queued (ours): all in use -> E taken back */
    s = st3(HOME, D, E, 1, 1, HOME, E);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_TAKE && i == 2);
    /* ... but not a queued present of another path (not this path's frame): wait */
    s = st3(HOME, D, E, 0, 1, HOME, E);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_WAIT);
    /* a flip of unknown target in flight: wait, as the two-buffer scheme does */
    s = st3(0, RD3_FQ_FOREIGN, 0, 0, 1, HOME, D);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_WAIT);
    /* the HUBP unreadable: the queue's record and the last buffer written count as in use */
    s = st3(D, E, 0, 0, 0, 0, E);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_COPY && i == 0);
    s = st3(D, E, HOME, 1, 0, 0, HOME);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_TAKE && i == 0);
    /* the HUBP unreadable before any latch of ours (the boot: home on the panel), D in flight, E queued and
     * written last: home is not free (round 4's two-buffer 'last' rule would miss it) - E taken back */
    s = st3(0, D, E, 1, 0, 0, E);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_TAKE && i == 2);
    s = st3(0, D, 0, 0, 0, 0, D);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_COPY && i == 2);
    /* the HUBP is the truth: a stale queue record (shown home) does not keep home while the HUBP scans E */
    s = st3(HOME, D, 0, 0, 1, E, D);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_COPY && i == 0);
    /* the queued buffer maybe on the panel (HUBP unknown, the record says it is shown; never expected): not taken */
    s = st3(E, D, E, 1, 0, 0, HOME);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_WAIT);
    /* the head on a zero-copy surface: all three free, round robin from the last written */
    s = st3(SURF, 0, 0, 0, 1, SURF, E);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_COPY && i == 0);
    s = st3(SURF, 0, 0, 0, 1, SURF, 0);
    CHECK(rd3_il_mb_pick(&s, &i) == RD3_IL_MB_COPY && i == 0);
    CHECK(rd3_il_mb_buf(&s, 0) == HOME && rd3_il_mb_buf(&s, 1) == D && rd3_il_mb_buf(&s, 2) == E &&
          rd3_il_mb_buf(&s, 3) == 0);

    /* rd3_fq_take_queued: only the queued address, never the one in flight */
    struct rd3_fq q;
    uint32_t tok = 0;
    rd3_fq_init(&q);
    CHECK(rd3_fq_submit(&q, D, 1, NULL) == RD3_FQ_PROGRAM);
    CHECK(rd3_fq_submit(&q, E, 2, NULL) == RD3_FQ_QUEUED);
    CHECK(!rd3_fq_take_queued(&q, D, &tok));                 /* in flight: cannot be taken back */
    CHECK(!rd3_fq_take_queued(&q, 0, &tok) && !rd3_fq_take_queued(&q, RD3_FQ_FOREIGN, &tok));
    CHECK(rd3_fq_take_queued(&q, E, &tok) && tok == 2 && !q.queued && q.inflight == D);
    CHECK(!rd3_fq_take_queued(&q, E, &tok));                 /* once */
}

/* One head of a 60 Hz panel and a producer presenting 'per' frames per refresh (the game behind WindowServer:
 * 300-400 fps against 60). The model HUBP latches the address programmed before a vblank at that vblank; the
 * present queue is the kext's (rdna3_flip_queue.h), its flip-done worker programs the queued present at the latch.
 *  mbox 0: the two-buffer scheme (rd3_il_pick): a transaction waits for the flip in flight and completes only at
 *          its own latch (the producer's thread is held until then);
 *  mbox 1: the mailbox (rd3_il_mb_pick + rd3_fq_take_queued): the transaction completes at the copy.
 * Every copy is checked against the HUBP: never into the buffer it scans or has pending, never into one in flight
 * or queued. 'unknown': every 7th look cannot read the HUBP. 'latch_mid': a latch falls between a take-back and its
 * present (the worker finds nothing queued). */
struct mbsim {
    unsigned txns, waits, latched, replaced, taken, foreign_waits, bad_write, two_latch, older_shown, dropped_q;
    unsigned shown_frame, max_txn_per_vbl;
};
static void sim_mb(int mbox, unsigned per, unsigned vblanks, int unknown, int latch_mid, struct mbsim *r)
{
    const uint64_t buf[3] = { HOME, D, E };
    unsigned content[3] = { 0, 0, 0 };   /* the frame number each buffer holds */
    uint64_t scan = HOME, pend = 0, last = HOME;
    struct rd3_fq q;
    unsigned frame = 0, look = 0;
    memset(r, 0, sizeof *r);
    rd3_fq_init(&q);
    for (unsigned v = 0; v < vblanks; v++) {
        unsigned in_vbl = 0, programs = 0;
        int blocked = 0;   /* mbox 0: the producer's transaction is held until a latch */
        for (unsigned k = 0; k < per && !blocked; k++) {
            struct rd3_il_state s;
            unsigned idx = 0;
            int took = 0;
            const int known = !(unknown && (look++ % 7) == 3);
            memset(&s, 0, sizeof s);
            s.home = HOME; s.d = D; s.e = mbox ? E : 0;
            s.shown = q.shown; s.inflight = q.inflight; s.queued = q.queued; s.queued_lb = q.queued != 0;
            s.scan_known = known; s.scan = known ? scan : 0; s.last = last;
            if (mbox) {
                enum rd3_il_mb a = rd3_il_mb_pick(&s, &idx);
                if (a == RD3_IL_MB_TAKE) {
                    uint32_t tok = 0;
                    CHECK(rd3_fq_take_queued(&q, buf[idx], &tok));
                    took = 1;
                    r->taken++;
                    r->replaced++;
                } else if (a != RD3_IL_MB_COPY) {
                    r->waits++;   /* never expected for a producer of our own frames */
                    break;
                }
            } else {
                enum rd3_il_pick p = rd3_il_pick(&s);
                if (p == RD3_IL_WAIT) {   /* held until the vblank: this refresh gets no more transactions */
                    r->waits++;
                    break;
                }
                CHECK(p == RD3_IL_HOME || p == RD3_IL_D);
                idx = p == RD3_IL_D ? 1u : 0u;
            }
            /* the copy: never into what the HUBP scans or has pending, nor into a present of the queue */
            if (buf[idx] == scan || buf[idx] == pend || buf[idx] == q.inflight || buf[idx] == q.queued)
                r->bad_write++;
            content[idx] = ++frame;
            if (latch_mid && took && (v % 5) == 2 && pend) {
                /* the vblank falls between the take-back and the present: the worker finds nothing queued */
                scan = pend; pend = 0;
                {
                    int had = 0;
                    uint32_t dt = 0, nt = 0;
                    uint64_t na = 0;
                    CHECK(rd3_fq_latched(&q, &had, &dt, &na, &nt) == RD3_FQ_IDLE && had);
                }
                programs = 0;   /* a vblank of its own */
                r->latched++;
                if (content[scan == HOME ? 0 : scan == D ? 1 : 2] < r->shown_frame) r->older_shown++;
                r->shown_frame = content[scan == HOME ? 0 : scan == D ? 1 : 2];
            }
            {
                uint32_t dropped = 0;
                const enum rd3_fq_act a = rd3_fq_submit(&q, buf[idx], frame, &dropped);
                if (a == RD3_FQ_PROGRAM) {
                    if (pend) r->two_latch++;   /* a second address before the vblank: never */
                    pend = buf[idx];
                    programs++;
                } else if (a == RD3_FQ_REPLACED) {
                    r->dropped_q++;
                    r->replaced++;
                } else {
                    CHECK(a == RD3_FQ_QUEUED);
                }
            }
            last = buf[idx];
            r->txns++;
            in_vbl++;
            if (!mbox) {   /* complete at its latch: the producer's thread is held, the next one after the vblank */
                blocked = 1;
                r->waits++;
            }
        }
        if (in_vbl > r->max_txn_per_vbl) r->max_txn_per_vbl = in_vbl;
        /* the vblank: the HUBP latches what is pending; the worker completes it and programs the queued one */
        if (pend) {
            int had = 0;
            uint32_t dt = 0, nt = 0;
            uint64_t na = 0;
            scan = pend; pend = 0;
            r->latched++;
            {
                const unsigned c = content[scan == HOME ? 0 : scan == D ? 1 : 2];
                if (c < r->shown_frame) r->older_shown++;
                r->shown_frame = c;
            }
            if (rd3_fq_latched(&q, &had, &dt, &na, &nt) == RD3_FQ_PROGRAM) {
                pend = na;   /* in flight now: latches at the next vblank */
                programs++;
            }
            CHECK(had);
        }
        if (programs > 2) r->two_latch++;   /* one at the producer, one at the latch at most */
    }
}

static void test_mb_model(void)
{
    struct mbsim a, b, c, u;
    /* the old scheme: a producer at 5x the refresh is paced to one transaction per refresh (WindowServer's thread,
     * and through it the game: the round 4 cap; two heads on one thread made it 120 a second) */
    sim_mb(0, 5, 600, 0, 0, &a);
    CHECK(a.max_txn_per_vbl == 1);
    CHECK(a.txns <= 600 && a.txns >= 590);
    CHECK(a.waits >= 590);
    CHECK(a.bad_write == 0 && a.two_latch == 0 && a.older_shown == 0);
    /* the mailbox: every transaction completes at once (5 a refresh), none waits; one flip latches per vblank */
    sim_mb(1, 5, 600, 0, 0, &b);
    CHECK(b.txns == 3000 && b.max_txn_per_vbl == 5);
    CHECK(b.waits == 0);
    CHECK(b.bad_write == 0);
    CHECK(b.two_latch == 0);
    CHECK(b.latched <= 600 && b.latched >= 598);
    CHECK(b.older_shown == 0);
    /* newest wins: the panel shows the newest frame queued before the latch (each refresh's last but at most a
     * refresh late), and every frame not shown was counted replaced: presented = latched + replaced + still queued
     * or in flight */
    CHECK(b.shown_frame >= 3000 - 2 * 5);
    CHECK(b.replaced == b.taken + b.dropped_q && b.dropped_q == 0);
    CHECK(b.txns >= b.latched + b.replaced && b.txns - b.latched - b.replaced <= 2);
    /* the HUBP unreadable every 7th look, and latches between a take-back and its present: still no write into a
     * scanned or pending buffer, still no wait, the counts still add up */
    sim_mb(1, 5, 600, 1, 0, &u);
    CHECK(u.bad_write == 0 && u.two_latch == 0 && u.waits == 0 && u.older_shown == 0);
    CHECK(u.txns - u.latched - u.replaced <= 2);
    sim_mb(1, 5, 600, 0, 1, &c);
    CHECK(c.bad_write == 0 && c.two_latch == 0 && c.waits == 0 && c.older_shown == 0);
    CHECK(c.txns - c.latched - c.replaced <= 2);
    /* a producer at the refresh or slower: nothing replaced, every frame latched */
    sim_mb(1, 1, 600, 0, 0, &c);
    CHECK(c.replaced == 0 && c.latched == 600 && c.waits == 0 && c.bad_write == 0);
    printf("test_iop_latch: 5x producer - two buffers %u transactions in 600 refreshes (%u waits); mailbox %u "
           "transactions, %u latched, %u replaced, 0 waits\n", a.txns, a.waits, b.txns, b.latched, b.replaced);
}

static void test_names(void)
{
    CHECK(!strcmp(rd3_il_fb_name(RD3_IL_FB_QUEUE), "queue") && !strcmp(rd3_il_fb_name(RD3_IL_FB_GEOM), "geom"));
    CHECK(!strcmp(rd3_il_fb_name(RD3_IL_FB_N), "?"));
    for (unsigned i = 0; i < RD3_IL_FB_N; i++) CHECK(strcmp(rd3_il_fb_name(i), "?") != 0);
}

int main(void)
{
    test_txn();
    test_vbl();
    test_pick();
    test_model();
    test_freeable();
    test_mb_pick();
    test_mb_model();
    test_names();
    printf("test_iop_latch: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
