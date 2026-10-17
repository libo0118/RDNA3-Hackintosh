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
 * mode, the target, the wait, the extent, copy forward's source.
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
    CHECK(rd3_sc_d_freeable(RD3_SCS_HOME) && rd3_sc_d_freeable(RD3_SCS_OTHER) && !rd3_sc_d_freeable(RD3_SCS_D));
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

int main(void)
{
    model();
    roles();
    actions();
    targets();
    extents();
    cf_sources();
    if (fails) {
        printf("test_swap_copy: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_swap_copy: all checks passed\n");
    return 0;
}
