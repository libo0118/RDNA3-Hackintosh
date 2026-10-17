/*
 * test_swap_copy.c - common/rdna3_swap_copy.h on the host (card run
 * 2026-10-17b: both screens mostly black, windows appear and vanish; the
 * swap flipped the panel to WindowServer's back instead of copying it into
 * the front as IOAccelLegacySurface::submitSwapCopy asks).
 *
 * The model: a head at 60 Hz whose HUBP takes a flipped address at the next
 * vblank, two scanout buffers (home and the accelerator's D), WindowServer
 * swapping at random times and drawing into the back right after the swap
 * returns. Checked: the copy never writes the buffer the panel scans (no
 * torn frame) with D, every swap's frame is on the panel by the vblank
 * after it, no swap waits longer than the latch cap, the front holds the
 * last frame for copy forward; without D the copy is direct (may tear).
 * Then the decision helpers one by one: roles, the empty-back action per
 * mode, the target, the wait, the extent, copy forward's source, and the
 * landed buffer of card run 2026-10-17c (a swap naming a back WindowServer
 * never drew presents the buffer its frame did land in).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_swap_copy.h"
#include "rdna3_flip_latch.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { PERIOD = 16667, POLL = 1000, COPY_US = 650, SWAPS = 2000 };

static uint32_t rng = 777u;
static uint32_t rnd(uint32_t n)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 8) % n;
}
static uint64_t next_vblank(uint64_t t)
{
    return (t / PERIOD + 1) * PERIOD;
}

/* a head: what the HUBP reads (0 home, 1 D), the address programmed and
 * when it latches; the frame number each buffer holds */
struct head {
    int scanning, programmed;
    uint64_t latch_at;
    unsigned frame[2];
    unsigned front;     /* the frame in WindowServer's front */
};
static void advance(struct head *h, uint64_t t)
{
    if (h->programmed != h->scanning && t >= h->latch_at)
        h->scanning = h->programmed;
}

struct result { unsigned torn, late, swaps; uint64_t max_wait; };

/* haveD: the second buffer exists; returns what happened over SWAPS swaps */
static struct result simulate(int haveD)
{
    struct head h;
    struct result r;
    enum rd3_sc_shows shows = RD3_SCS_OTHER;
    int flipped = 0, latched = 1;
    uint64_t t = 1000, flip_t = 0;
    unsigned frame;
    memset(&h, 0, sizeof h);
    memset(&r, 0, sizeof r);
    for (frame = 1; frame <= SWAPS; frame++) {
        enum rd3_sc_target tg;
        int buf;
        uint64_t wait = 0, copy_end;
        t += 2000 + rnd(30000);   /* WindowServer's next present */
        advance(&h, t);
        if (flipped && !latched && h.scanning == h.programmed)
            latched = 1;
        tg = rd3_sc_target(shows, haveD);
        /* the kext polls the flip state every 1 ms until it latched or the cap */
        while (rd3_sc_must_wait(tg, flipped, latched)) {
            const enum rd3_latch_step s = rd3_latch_step(1, h.scanning != h.programmed, 0, 0, t - flip_t,
                                                         rd3_latch_cap_us(PERIOD));
            if (s != RD3_LATCH_WAIT) {
                latched = 1;
                break;
            }
            t += POLL;
            wait += POLL;
            advance(&h, t);
        }
        if (wait > r.max_wait)
            r.max_wait = wait;
        buf = tg == RD3_SCT_D ? 1 : 0;
        /* the copy writes 'buf' from t to t + COPY_US: torn when the HUBP
         * reads it meanwhile (its address latched, not merely programmed) */
        copy_end = t + COPY_US;
        if (h.scanning == buf || (h.programmed == buf && h.latch_at < copy_end))
            r.torn++;
        t = copy_end;
        h.frame[buf] = frame;
        h.front = frame;   /* the second copy: the front */
        /* the flip */
        h.programmed = buf;
        h.latch_at = next_vblank(t);
        flip_t = t;
        flipped = 1;
        latched = 0;
        shows = rd3_sc_after_flip(shows, tg, 1);
        /* the frame is on the panel at the next vblank */
        advance(&h, next_vblank(t));
        if (h.frame[h.scanning] != frame)
            r.late++;
        CHECK(h.front == frame);
        r.swaps++;
    }
    return r;
}

static void model(void)
{
    const struct result two = simulate(1), one = simulate(0);
    CHECK(two.swaps == SWAPS);
    CHECK(two.torn == 0);                       /* the copy never writes the scanned buffer */
    CHECK(two.late == 0);                       /* each frame on the panel at the vblank after it */
    CHECK(two.max_wait <= PERIOD + POLL);       /* at most a frame and a poll */
    CHECK(one.torn > 0);                        /* one buffer: the copy races the scanout */
    CHECK(one.late == 0);
    CHECK(one.max_wait == 0);                   /* nothing to wait for */
}

static void roles(void)
{
    struct rd3_sc_roles r = rd3_sc_roles(1, 0);
    CHECK(r.back == 2 && r.front == 1 && r.frontIsFb);
    r = rd3_sc_roles(0, 1);   /* card run 2026-10-17b's swap #518: a2 a head's resource */
    CHECK(r.back == 1 && r.front == 2 && r.frontIsFb);
    r = rd3_sc_roles(0, 0);   /* a surface no head shows: submitSwapCopy(event, front, back) */
    CHECK(r.back == 2 && r.front == 1 && !r.frontIsFb);
    r = rd3_sc_roles(1, 1);
    CHECK(r.back == 2 && r.front == 1 && r.frontIsFb);
}

static void actions(void)
{
    int pic;
    /* pixels, or not sampled (no mapping): always copied */
    for (pic = 0; pic < 2; pic++) {
        CHECK(rd3_sc_action(RD3_SWAPMODE_COPY, 1, 1, pic) == RD3_SC_COPY);
        CHECK(rd3_sc_action(RD3_SWAPMODE_COPY_ALL, 1, 1, pic) == RD3_SC_COPY);
        CHECK(rd3_sc_action(RD3_SWAPMODE_COPY, 0, 0, pic) == RD3_SC_COPY);
    }
    /* mode 1: an empty back never goes over a picture; with none to lose it is copied */
    CHECK(rd3_sc_action(RD3_SWAPMODE_COPY, 1, 0, 1) == RD3_SC_HOLD);
    CHECK(rd3_sc_action(RD3_SWAPMODE_COPY, 1, 0, 0) == RD3_SC_COPY_EMPTY);
    /* mode 2 copies everything */
    CHECK(rd3_sc_action(RD3_SWAPMODE_COPY_ALL, 1, 0, 1) == RD3_SC_COPY_EMPTY);
    CHECK(rd3_sc_action(RD3_SWAPMODE_COPY_ALL, 1, 0, 0) == RD3_SC_COPY_EMPTY);
    /* card run 2026-10-17b's head 0: B1 drawn, B2 always empty, in turns:
     * mode 2 shows black every other swap, mode 1 never (after the first picture) */
    {
        unsigned held = 0, black1 = 0, black2 = 0, i;
        int picture = 0;
        for (i = 0; i < 200; i++) {
            const int empty = i & 1;
            const enum rd3_sc_act a1 = rd3_sc_action(RD3_SWAPMODE_COPY, 1, !empty, picture);
            const enum rd3_sc_act a2 = rd3_sc_action(RD3_SWAPMODE_COPY_ALL, 1, !empty, picture);
            black1 += a1 == RD3_SC_COPY_EMPTY;
            held += a1 == RD3_SC_HOLD;
            black2 += a2 == RD3_SC_COPY_EMPTY;
            if (a1 == RD3_SC_COPY)
                picture = 1;
        }
        CHECK(black1 == 0 && held == 100 && black2 == 100);
    }
    CHECK(rd3_swapmode_valid(0) && rd3_swapmode_valid(1) && rd3_swapmode_valid(2));
    CHECK(!rd3_swapmode_valid(-1) && !rd3_swapmode_valid(3));
    CHECK(RD3_SWAPMODE_DEFAULT == RD3_SWAPMODE_COPY);
}

static void targets(void)
{
    CHECK(rd3_sc_target(RD3_SCS_OTHER, 1) == RD3_SCT_D);
    CHECK(rd3_sc_target(RD3_SCS_HOME, 1) == RD3_SCT_D);
    CHECK(rd3_sc_target(RD3_SCS_D, 1) == RD3_SCT_HOME);
    CHECK(rd3_sc_target(RD3_SCS_D, 0) == RD3_SCT_HOME_DIRECT);
    CHECK(rd3_sc_target(RD3_SCS_OTHER, 0) == RD3_SCT_HOME_DIRECT);
    CHECK(rd3_sc_tear_free(RD3_SCT_D) && rd3_sc_tear_free(RD3_SCT_HOME));
    CHECK(!rd3_sc_tear_free(RD3_SCT_HOME_DIRECT) && !rd3_sc_tear_free(RD3_SCT_NONE));
    /* the wait: only for a pending flip of ours, only into a buffer that may be leaving the panel */
    CHECK(rd3_sc_must_wait(RD3_SCT_D, 1, 0));
    CHECK(!rd3_sc_must_wait(RD3_SCT_D, 1, 1));
    CHECK(!rd3_sc_must_wait(RD3_SCT_D, 0, 0));
    CHECK(!rd3_sc_must_wait(RD3_SCT_HOME_DIRECT, 1, 0));
    /* the panel after the flip */
    CHECK(rd3_sc_after_flip(RD3_SCS_OTHER, RD3_SCT_D, 1) == RD3_SCS_D);
    CHECK(rd3_sc_after_flip(RD3_SCS_D, RD3_SCT_HOME, 1) == RD3_SCS_HOME);
    CHECK(rd3_sc_after_flip(RD3_SCS_D, RD3_SCT_HOME, 0) == RD3_SCS_D);   /* refused: still D */
    CHECK(rd3_sc_after_flip(RD3_SCS_OTHER, RD3_SCT_HOME_DIRECT, 1) == RD3_SCS_HOME);
    /* D's life: (re)allocated for a new size, freed only off the panel */
    CHECK(rd3_sc_d_stale(0, 0, 0, 1920, 1080));
    CHECK(!rd3_sc_d_stale(1, 1920, 1080, 1920, 1080));
    CHECK(rd3_sc_d_stale(1, 1920, 1080, 960, 540));
    CHECK(rd3_sc_d_freeable(RD3_SCS_HOME, 0, 1) && rd3_sc_d_freeable(RD3_SCS_OTHER, 1, 1));
    CHECK(!rd3_sc_d_freeable(RD3_SCS_D, 0, 1) && !rd3_sc_d_freeable(RD3_SCS_D, 1, 1));
    /* off D by a flip not seen latched yet: the HUBP may still read D */
    CHECK(!rd3_sc_d_freeable(RD3_SCS_HOME, 1, 0) && !rd3_sc_d_freeable(RD3_SCS_OTHER, 1, 0));
    CHECK(rd3_sc_pitch(1920) == 7680 && rd3_sc_pitch(960) == 3840 && rd3_sc_pitch(1600) == 6400);
    CHECK(rd3_sc_pitch(1366) == 5632);
}

static void extents(void)
{
    unsigned rb, rows;
    /* same size */
    CHECK(rd3_sc_extent(1920, 1080, 7680, 8294400, 1920, 1080, 7680, 8294400, &rb, &rows));
    CHECK(rb == 7680 && rows == 1080);
    /* a 1920x1080 back on a 960x540 head (the mode switch's window): clipped */
    CHECK(rd3_sc_extent(1920, 1080, 7680, 8294400, 960, 540, 3840, 2073600, &rb, &rows));
    CHECK(rb == 3840 && rows == 540);
    /* rows that would end past the destination's allocation */
    CHECK(rd3_sc_extent(1920, 1080, 7680, 8294400, 1920, 1080, 7680, 7680 * 100, &rb, &rows));
    CHECK(rows == 100);
    /* the source's allocation shorter than its height */
    CHECK(rd3_sc_extent(1920, 1080, 7680, 7680 * 10 + 100, 1920, 1080, 7680, 8294400, &rb, &rows));
    CHECK(rows == 10);
    /* nothing to copy */
    CHECK(!rd3_sc_extent(0, 1080, 7680, 8294400, 1920, 1080, 7680, 8294400, &rb, &rows) && !rb && !rows);
    CHECK(!rd3_sc_extent(1920, 1080, 4000, 8294400, 1920, 1080, 7680, 8294400, &rb, &rows));   /* pitch < width */
    CHECK(!rd3_sc_extent(1920, 1080, 7680, 100, 1920, 1080, 7680, 8294400, &rb, &rows));       /* not one row */
}

static void cf_sources(void)
{
    struct rd3_sc_cf_in in;
    memset(&in, 0, sizeof in);
    in.srcIsDst = 1;
    CHECK(rd3_sc_cf_source(&in) == RD3_SCCF_SAME);
    CHECK(rd3_sc_cf_source(0) == RD3_SCCF_SAME);
    in.srcIsDst = 0;
    CHECK(rd3_sc_cf_source(&in) == RD3_SCCF_SRC);          /* no head's front: the surface's buffer 0 */
    in.srcIsHeadFront = 1;
    in.frontCurrent = 1;
    CHECK(rd3_sc_cf_source(&in) == RD3_SCCF_SRC);          /* Apple's model: the front holds the last frame */
    in.frontCurrent = 0;
    in.scanoutKnown = 1;
    CHECK(rd3_sc_cf_source(&in) == RD3_SCCF_SCANOUT);      /* the front copy failed: the panel's buffer */
    in.scanoutKnown = 0;
    CHECK(rd3_sc_cf_source(&in) == RD3_SCCF_STALE);
}

/* ECC review round 1: D is freed (a mode change, the swaps' new size) only
 * once the HUBP cannot read it. A head swapping between D and home, an IOP
 * or mode 0 flip now and then (posted: the panel moved, not latched yet),
 * and a resize every so often: D is never freed while the HUBP reads it or
 * has it programmed. */
static void free_model(void)
{
    struct head h;
    enum rd3_sc_shows shows = RD3_SCS_HOME;
    int flipped = 0, latched = 1, haveD = 1, frees = 0, deferred = 0, bad = 0;
    uint64_t t = 1000;
    unsigned i;
    memset(&h, 0, sizeof h);
    for (i = 0; i < 4000; i++) {
        t += 1000 + rnd(20000);
        advance(&h, t);
        /* the swap's look at the head (rd3AccelScEnsureD) */
        if (flipped && !latched && h.scanning == h.programmed)
            latched = 1;
        if (rnd(9) == 0) {
            /* a resize: the old D goes only when freeable */
            if (haveD && !rd3_sc_d_freeable(shows, flipped, latched)) {
                deferred++;
            } else if (haveD) {
                if (h.scanning == 1 || h.programmed == 1)
                    bad++;
                frees++;
                haveD = 0;
            }
        }
        if (rnd(7) == 0) {
            /* someone else's flip (IOP, mode 0): buffer 2, posted to the swaps */
            h.programmed = 2;
            h.latch_at = next_vblank(t);
            shows = RD3_SCS_OTHER;
            flipped = 1;
            latched = 0;
            continue;
        }
        {
            const enum rd3_sc_target tg = rd3_sc_target(shows, haveD);
            const int buf = tg == RD3_SCT_D ? 1 : 0;
            if (!haveD)
                haveD = 1;   /* the new D, allocated for the next swap */
            h.programmed = buf;
            h.latch_at = next_vblank(t + COPY_US);
            shows = rd3_sc_after_flip(shows, tg, 1);
            flipped = 1;
            latched = 0;
        }
    }
    CHECK(frees > 0);
    CHECK(deferred > 0);      /* the model did reach the case */
    CHECK(bad == 0);
}

/* rdna3_flip_latch.h's backoff in modes 1/2: a head whose flips never latch
 * costs RD3_LATCH_TIMEOUT_RUN capped waits, then only looks; a latch seen
 * turns the waits on again */
static void wait_backoff(void)
{
    struct rd3_latch_state st;
    unsigned i, waits = 0, looks = 0;
    memset(&st, 0, sizeof st);
    st.latched = 1;
    CHECK(rd3_sc_wait_plan(RD3_SCT_D, 0, 1, 0) == RD3_SCW_NONE);
    CHECK(rd3_sc_wait_plan(RD3_SCT_HOME_DIRECT, 1, 0, 0) == RD3_SCW_NONE);   /* nothing to protect */
    for (i = 0; i < 100; i++) {
        const enum rd3_sc_wait w = rd3_sc_wait_plan(RD3_SCT_D, 1, 0, st.off);
        if (w == RD3_SCW_WAIT) {
            enum rd3_latch_event ev;
            waits++;
            ev = rd3_latch_timeout(&st);   /* the head never latches */
            CHECK(ev == (waits == RD3_LATCH_TIMEOUT_RUN ? RD3_LATCH_EV_OFF : RD3_LATCH_EV_NONE));
        } else {
            CHECK(w == RD3_SCW_LOOK);
            looks++;
            rd3_latch_give_up(&st);
        }
    }
    CHECK(waits == RD3_LATCH_TIMEOUT_RUN);   /* at most 3 x the cap, then never again */
    CHECK(looks == 100 - RD3_LATCH_TIMEOUT_RUN);
    CHECK(rd3_latch_seen(&st) == RD3_LATCH_EV_RESUMED);   /* a look saw it latched */
    CHECK(rd3_sc_wait_plan(RD3_SCT_D, 1, 0, st.off) == RD3_SCW_WAIT);
}

/* the front record forgets a freed allocation: a new buffer at the same
 * address and VRAM offset is not "the front holding the last frame" */
static void front_record(void)
{
    struct rd3_sc_front f;
    int a, b;
    memset(&f, 0, sizeof f);
    rd3_sc_front_set(&f, &a, 0x1000);
    CHECK(rd3_sc_front_current(&f, &a, 0x1000));
    CHECK(!rd3_sc_front_current(&f, &a, 0x2000));
    CHECK(!rd3_sc_front_current(&f, &b, 0x1000));
    CHECK(!rd3_sc_front_current(&f, 0, 0));
    CHECK(!rd3_sc_front_forget(&f, &b));        /* another vidmem freed: kept */
    CHECK(rd3_sc_front_current(&f, &a, 0x1000));
    CHECK(rd3_sc_front_forget(&f, &a));         /* freed */
    CHECK(!rd3_sc_front_current(&f, &a, 0x1000));   /* reallocated at the same place: not current */
    CHECK(!rd3_sc_front_forget(&f, 0));
}

/* the SDMA breaker: a dead engine costs a bounded number of copies */
static void breaker(void)
{
    struct rd3_sc_brk b;
    enum rd3_sc_brk_ev ev;
    unsigned long long n;
    unsigned tries = 0, flips = 0, opened = 0, reopened = 0, probes = 0;
    memset(&b, 0, sizeof b);
    /* failures that are not timeouts: three in a row */
    CHECK(rd3_sc_brk_allow(&b, 1, &ev) && ev == RD3_SC_BRK_NONE);
    CHECK(rd3_sc_brk_fail(&b, 1, 0) == RD3_SC_BRK_NONE);
    CHECK(rd3_sc_brk_fail(&b, 2, 0) == RD3_SC_BRK_NONE);
    CHECK(rd3_sc_brk_ok(&b) == RD3_SC_BRK_NONE);     /* a success ends the run */
    CHECK(rd3_sc_brk_fail(&b, 3, 0) == RD3_SC_BRK_NONE);
    CHECK(rd3_sc_brk_fail(&b, 4, 0) == RD3_SC_BRK_NONE);
    CHECK(rd3_sc_brk_fail(&b, 5, 0) == RD3_SC_BRK_OPENED);
    CHECK(!rd3_sc_brk_allow(&b, 6, &ev) && ev == RD3_SC_BRK_NONE);
    CHECK(rd3_sc_brk_allow(&b, 5 + RD3_SC_BRK_RETRY_US, &ev) && ev == RD3_SC_BRK_PROBE);
    CHECK(rd3_sc_brk_ok(&b) == RD3_SC_BRK_CLOSED);
    CHECK(rd3_sc_brk_allow(&b, 5 + RD3_SC_BRK_RETRY_US + 1, &ev));
    /* one timeout opens it at once */
    memset(&b, 0, sizeof b);
    CHECK(rd3_sc_brk_fail(&b, 10, 1) == RD3_SC_BRK_OPENED);
    /* a dead engine over 10000 swaps at 60 Hz (every copy times out): one
     * try per RD3_SC_BRK_RETRY_US of the clock (round 2: not per swaps) */
    memset(&b, 0, sizeof b);
    for (n = 1; n <= 10000; n++) {
        const unsigned long long t = n * 16667ull;
        if (!rd3_sc_brk_allow(&b, t, &ev)) {
            flips++;
            continue;
        }
        if (ev == RD3_SC_BRK_PROBE)
            probes++;
        tries++;
        ev = rd3_sc_brk_fail(&b, t, 1);
        if (ev == RD3_SC_BRK_OPENED)
            opened++;
        if (ev == RD3_SC_BRK_REOPENED)
            reopened++;
    }
    CHECK(opened == 1 && b.trips == 1);
    CHECK(tries == 1 + probes && reopened == probes);
    CHECK(probes == 16);                         /* 166 s: one try per 10 s */
    CHECK(flips == 10000 - tries);
    /* a quiet desktop: a swap every 2 s still gets a try every 10 s */
    memset(&b, 0, sizeof b);
    probes = 0;
    for (n = 1; n <= 100; n++) {
        const unsigned long long t = n * 2000000ull;
        if (rd3_sc_brk_allow(&b, t, &ev)) {
            if (ev == RD3_SC_BRK_PROBE)
                probes++;
            (void)rd3_sc_brk_fail(&b, t, 1);
        }
    }
    CHECK(probes == 19);
}

/* ECC review round 2: a panel move posted from outside the family's lock
 * never undoes a later flip of the swap's own. A head's record against
 * what its HUBP was programmed with last, over random interleavings of
 * IOP flips (posted, applied at the next swap) and swaps' own flips. */
static void posts(void)
{
    unsigned i, wrong = 0, ignored = 0;
    unsigned long long clock = 1, lastOwn = 0, postAt = 0;
    int posted = 0, recorded = 0 /* 0 ours, 1 someone else's */, hubp = 0;
    CHECK(rd3_sc_post_applies(5, 4) && !rd3_sc_post_applies(4, 4) && !rd3_sc_post_applies(3, 4));
    for (i = 0; i < 20000; i++) {
        const unsigned r = rnd(3);
        clock++;
        if (r == 0) {              /* an IOP flip programs another buffer and posts it */
            hubp = 1;
            postAt = clock;
            posted = 1;
        } else {                   /* a swap: applies posts, then (r == 2) flips its own buffer */
            if (posted) {
                if (rd3_sc_post_applies(postAt, lastOwn))
                    recorded = 1;
                else
                    ignored++;
                posted = 0;
            }
            if (recorded != hubp)
                wrong++;
            if (r == 2) {
                clock++;
                lastOwn = clock;
                hubp = 0;
                recorded = 0;
                /* a post made before this flip but applied after it (the race of round 2) */
                if (rnd(4) == 0) {
                    postAt = lastOwn - 1;
                    posted = 1;
                }
            }
        }
    }
    CHECK(wrong == 0);
    CHECK(ignored > 0);    /* the race was exercised */
}

/* ---- the landed buffer (card run 2026-10-17c) ----
 * A surface of three buffers: 0 the head's framebuffer resource (the
 * front: never a candidate), 1 an IOSurface-backed back (type 0xc0), 2 a
 * back without an IOSurface (type 0x40). Each buffer holds a frame number
 * (0: empty, its samples all zero); the head's signature table remembers
 * what it last sampled in each; the panel shows a frame number. A swap
 * samples the back it names, then (rd3_sc_landed_wanted) the others, as
 * the accelerator does (rd3AccelSwapToPanel, rd3AccelScanOthers). */
enum { LB_FRONT = 0, LB_B1 = 1, LB_B2 = 2, LB_N = 3 };
struct lmodel {
    unsigned content[LB_N];
    unsigned sig[LB_N];
    int seen[LB_N];
    int iosurface[LB_N];
    unsigned panel;          /* the frame on the panel, 0 black */
    unsigned held, landed, black, copied;
};
static void lm_init(struct lmodel *m)
{
    memset(m, 0, sizeof *m);
    m->iosurface[LB_B1] = 1;
    m->content[LB_FRONT] = 1000;   /* the front has pixels: a picture an empty back would overwrite */
}
/* the head samples buffer i: changed since its last look (never seen: -1) */
static int lm_sample(struct lmodel *m, unsigned i)
{
    const int ch = m->seen[i] ? m->content[i] != m->sig[i] : -1;
    m->sig[i] = m->content[i];
    m->seen[i] = 1;
    return ch;
}
/* one swap naming 'back'; landed: the new rule on (else the old one) */
static void lm_swap(struct lmodel *m, unsigned back, int mode, int landed)
{
    const int pixels = m->content[back] != 0;
    const int chb = lm_sample(m, back);
    enum rd3_sc_act act = rd3_sc_action(mode, 1, pixels, m->content[LB_FRONT] != 0 || m->panel != 0);
    unsigned src = back;
    struct rd3_sc_cand c[RD3_SC_LANDED_MAX];
    unsigned idx[RD3_SC_LANDED_MAX], n = 0;
    /* the others are sampled at an empty back either way (the trace's SCAN) */
    const int wanted = rd3_sc_landed_wanted(mode, 1, pixels, chb != 0, m->iosurface[back]);
    if (wanted || !pixels) {
        for (unsigned i = 0; i < LB_N; i++) {
            if (i == back || i == LB_FRONT)
                continue;
            const int ch = lm_sample(m, i);
            c[n].sampled = 1;
            c[n].pixels = m->content[i] != 0;
            c[n].changed = ch > 0;
            c[n].shown = m->panel != 0 && m->content[i] == m->panel;
            c[n].iosurface = m->iosurface[i];
            idx[n++] = i;
        }
    }
    if (landed && wanted) {
        const int pk = rd3_sc_landed_pick(c, n);
        if (pk >= 0) {
            act = RD3_SC_COPY_LANDED;
            src = idx[pk];
        }
    }
    switch (act) {
    case RD3_SC_HOLD:
        m->held++;
        break;
    case RD3_SC_COPY_EMPTY:
        m->black++;
        m->panel = 0;
        break;
    case RD3_SC_COPY_LANDED:
        m->landed++;
        m->panel = m->content[src];
        break;
    default:
        m->copied++;
        m->panel = m->content[src];
        break;
    }
}

static void landed_model(void)
{
    enum { FRAMES = 200 };
    /* the brief's model: every frame lands in B1, the swaps name B1 and B2
     * in turns; each swap must show the newest frame, none held, none black */
    for (int landed = 0; landed <= 1; landed++) {
        struct lmodel m;
        unsigned stale = 0;
        lm_init(&m);
        for (unsigned k = 1; k <= FRAMES; k++) {
            m.content[LB_B1] = k;
            lm_swap(&m, k & 1 ? LB_B1 : LB_B2, RD3_SWAPMODE_COPY, landed);
            stale += m.panel != k;
        }
        if (landed) {
            CHECK(stale == 0);
            CHECK(m.held == 0);
            CHECK(m.black == 0);
            CHECK(m.landed == FRAMES / 2);
        } else {   /* the control: the old rule holds every second frame */
            CHECK(stale == FRAMES / 2);
            CHECK(m.held == FRAMES / 2);
            CHECK(m.landed == 0);
        }
    }
    /* card run 2026-10-17c as the trace shows it: the frame lands in B1, the
     * swap of B1 copies it, the swap of B2 follows 0.3 ms later with nothing
     * drawn in between - B1 is unchanged and already on the panel: held,
     * no frame lost, the landed rule does not fire */
    {
        struct lmodel m;
        unsigned stale = 0;
        lm_init(&m);
        for (unsigned k = 1; k <= FRAMES; k++) {
            m.content[LB_B1] = k;
            lm_swap(&m, LB_B1, RD3_SWAPMODE_COPY, 1);
            lm_swap(&m, LB_B2, RD3_SWAPMODE_COPY, 1);
            stale += m.panel != k;
        }
        CHECK(stale == 0);
        CHECK(m.landed == 0);
        CHECK(m.held == FRAMES);
        CHECK(m.copied == FRAMES);
    }
    /* a back without an IOSurface that holds an old frame (pixels, never
     * redrawn): the old rule copied it, an old picture every second swap;
     * the new one presents B1's newest frame */
    for (int landed = 0; landed <= 1; landed++) {
        struct lmodel m;
        unsigned stale = 0;
        lm_init(&m);
        m.content[LB_B2] = 7;
        for (unsigned k = 1; k <= FRAMES; k++) {
            m.content[LB_B1] = 100 + k;
            lm_swap(&m, k & 1 ? LB_B1 : LB_B2, RD3_SWAPMODE_COPY, landed);
            stale += m.panel != 100 + k;
        }
        /* the first swap of B2 cannot tell (never seen: trusted, copied) */
        CHECK(landed ? stale == 1 : stale == FRAMES / 2);
        CHECK(m.black == 0);
    }
    /* mode 2: an empty back and a landed buffer: the landed frame, not black */
    {
        struct lmodel m;
        lm_init(&m);
        m.content[LB_B1] = 5;
        lm_swap(&m, LB_B1, RD3_SWAPMODE_COPY_ALL, 1);
        m.content[LB_B1] = 6;
        lm_swap(&m, LB_B2, RD3_SWAPMODE_COPY_ALL, 1);
        CHECK(m.panel == 6);
        CHECK(m.black == 0);
        CHECK(m.landed == 1);
        /* nothing new anywhere: mode 2 still copies the empty back (black) */
        lm_swap(&m, LB_B2, RD3_SWAPMODE_COPY_ALL, 1);
        CHECK(m.black == 1);
    }
}

static void landed_rules(void)
{
    /* when to look */
    CHECK(!rd3_sc_landed_wanted(RD3_SWAPMODE_FLIP, 1, 0, 0, 0));      /* mode 0: never */
    CHECK(rd3_sc_landed_wanted(RD3_SWAPMODE_COPY, 1, 0, 0, 1));       /* an empty back */
    CHECK(rd3_sc_landed_wanted(RD3_SWAPMODE_COPY_ALL, 1, 0, 1, 1));
    CHECK(!rd3_sc_landed_wanted(RD3_SWAPMODE_COPY, 0, 0, 0, 0));      /* not sampled: copied as before */
    CHECK(!rd3_sc_landed_wanted(RD3_SWAPMODE_COPY, 1, 1, 0, 1));      /* an IOSurface back with pixels */
    CHECK(!rd3_sc_landed_wanted(RD3_SWAPMODE_COPY, 1, 1, 1, 0));      /* a type 0x40 back that changed */
    CHECK(rd3_sc_landed_wanted(RD3_SWAPMODE_COPY, 1, 1, 0, 0));       /* a type 0x40 back that did not */
    CHECK(!rd3_sc_landed_wanted(7, 1, 0, 0, 0));
    /* which one */
    struct rd3_sc_cand c[4];
    memset(c, 0, sizeof c);
    CHECK(rd3_sc_landed_pick(c, 4) == -1);
    CHECK(rd3_sc_landed_pick(NULL, 4) == -1);
    c[0] = (struct rd3_sc_cand){ 1, 1, 1, 0, 0 };   /* changed, type 0x40 */
    c[1] = (struct rd3_sc_cand){ 1, 1, 1, 0, 1 };   /* changed, IOSurface: preferred */
    CHECK(rd3_sc_landed_pick(c, 2) == 1);
    CHECK(rd3_sc_landed_pick(c, 1) == 0);
    c[1].shown = 1;                                  /* already on the panel */
    CHECK(rd3_sc_landed_pick(c, 2) == 0);
    c[0].pixels = 0;                                 /* changed to empty (cleared) */
    CHECK(rd3_sc_landed_pick(c, 2) == -1);
    c[2] = (struct rd3_sc_cand){ 1, 1, 0, 0, 1 };   /* not changed */
    c[3] = (struct rd3_sc_cand){ 0, 1, 1, 0, 1 };   /* not sampled */
    CHECK(rd3_sc_landed_pick(c, 4) == -1);
    c[3].sampled = 1;
    CHECK(rd3_sc_landed_pick(c, 4) == 3);
    /* bounded: candidates past RD3_SC_LANDED_MAX are never read */
    struct rd3_sc_cand many[RD3_SC_LANDED_MAX + 2];
    memset(many, 0, sizeof many);
    many[RD3_SC_LANDED_MAX] = (struct rd3_sc_cand){ 1, 1, 1, 0, 1 };
    CHECK(rd3_sc_landed_pick(many, RD3_SC_LANDED_MAX + 2) == -1);
}

int main(void)
{
    model();
    roles();
    actions();
    targets();
    extents();
    cf_sources();
    free_model();
    wait_backoff();
    front_record();
    breaker();
    posts();
    landed_model();
    landed_rules();
    if (fails) {
        printf("test_swap_copy: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_swap_copy: all checks passed\n");
    return 0;
}
