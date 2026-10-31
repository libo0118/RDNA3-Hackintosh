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
#include "rdna3_iop_latch.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { HOME = 0x1000, D = 0x9000, SURF = 0x5000 };

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
    s = st(D, 0, 0, 1, HOME, D);
    CHECK(!rd3_il_d_freeable(&s));                   /* the queue's record still says D: next time */
    s = st(0, 0, 0, 0, 0, HOME);
    CHECK(!rd3_il_d_freeable(&s));                   /* the HUBP unreadable: kept */
    s = st(0, 0, 0, 1, HOME, 0);
    s.d = 0;
    CHECK(!rd3_il_d_freeable(&s));
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
    test_names();
    printf("test_iop_latch: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
