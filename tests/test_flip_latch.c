/*
 * test_flip_latch.c - common/rdna3_flip_latch.h on the host (card run
 * 2026-10-17: windows and menus flicker on both screens; the swap returned
 * before its flip latched).
 *
 * The model: a head at 60 Hz (vblank every 16667 us), a flip programmed at
 * the swap latches at the next vblank; until then the panel scans the
 * buffer shown before. WindowServer gets the old buffer back when the swap
 * returns and starts writing into it (copy forward, the next frame) 100 us
 * later. Without the wait it writes into the buffer the panel still scans
 * (the half-drawn frame the user sees); with rd3_latch_step's wait, polled
 * every 1 ms as the kext polls, never, and no swap waits longer than a
 * frame and a poll.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_flip_latch.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { PERIOD = 16667, POLL = 1000, WRITE_DELAY = 100, SWAPS = 600 };

static uint32_t rng = 12345u;
static uint32_t rnd(uint32_t n)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 8) % n;
}

/* the vblank after t (a flip at t latches there) */
static uint64_t next_vblank(uint64_t t)
{
    return (t / PERIOD + 1) * PERIOD;
}

struct run { unsigned on_panel, swaps, timeouts; uint64_t max_wait; };

/* 'wait': the swap polls rd3_latch_step until the flip latched (the fix);
 * 'bufs': back buffers taking turns (1: the buffer drawn is the one shown) */
static struct run simulate(int wait, unsigned bufs)
{
    struct run r = { 0, 0, 0, 0 };
    uint64_t t = 1000, latch_at = 0;   /* the last flip's latch time */
    int shown = -1, prev = -1;
    unsigned next = 0;
    rng = 777u;
    for (unsigned i = 0; i < SWAPS; i++) {
        /* WindowServer draws buffer 'next' and swaps it at t */
        const int buf = (int)next;
        prev = shown;
        shown = buf;
        latch_at = next_vblank(t);
        uint64_t ret = t;
        if (wait) {
            const uint32_t frame0 = (uint32_t)(t / PERIOD);
            uint64_t now = t;
            enum rd3_latch_step s;
            do {
                now += POLL;
                s = rd3_latch_step(1, now < latch_at, frame0, (uint32_t)(now / PERIOD), now - t);
            } while (s == RD3_LATCH_WAIT);
            if (s == RD3_LATCH_TIMEOUT)
                r.timeouts++;
            ret = now;
            if (now - t > r.max_wait)
                r.max_wait = now - t;
        }
        /* the buffer WindowServer writes next: with two backs the one shown
         * before this swap; it starts after the swap returned */
        next = (next + 1) % bufs;
        const uint64_t write = ret + WRITE_DELAY;
        const int target = (int)next;
        /* the panel scans 'prev' until latch_at, 'shown' after */
        const int scanned = write < latch_at ? prev : shown;
        if (bufs > 1 && target == scanned)
            r.on_panel++;
        r.swaps++;
        /* the next swap 2..30 ms later */
        t = write + 2000 + rnd(28000);
    }
    return r;
}

int main(void)
{
    /* rd3_latch_step */
    CHECK(rd3_latch_step(0, 1, 0, 0, 0) == RD3_LATCH_UNKNOWN);
    CHECK(rd3_latch_step(1, 0, 5, 5, 0) == RD3_LATCH_DONE);
    CHECK(rd3_latch_step(1, 1, 5, 5, 1000) == RD3_LATCH_WAIT);
    CHECK(rd3_latch_step(1, 1, 5, 6, 16000) == RD3_LATCH_WAIT);      /* one vblank passed: may still be latching */
    CHECK(rd3_latch_step(1, 1, 5, 7, 20000) == RD3_LATCH_TIMEOUT);   /* two frames: late */
    CHECK(rd3_latch_step(1, 1, 0xffffff, 1, 20000) == RD3_LATCH_TIMEOUT);   /* the 24-bit counter wraps */
    CHECK(rd3_latch_step(1, 1, 0xffffff, 0, 1000) == RD3_LATCH_WAIT);
    CHECK(rd3_latch_step(1, 1, 9, 9, RD3_LATCH_CAP_US) == RD3_LATCH_TIMEOUT);   /* a frozen counter: the cap */
    CHECK(rd3_latch_step(1, 1, 9, 9, RD3_LATCH_CAP_US - 1) == RD3_LATCH_WAIT);

    /* rd3_latch_should_wait: off, on, a run of timeouts stops it */
    CHECK(!rd3_latch_should_wait(0, 0));
    CHECK(rd3_latch_should_wait(1, 0));
    CHECK(rd3_latch_should_wait(1, RD3_LATCH_TIMEOUT_RUN - 1));
    CHECK(!rd3_latch_should_wait(1, RD3_LATCH_TIMEOUT_RUN));

    /* a head that never latches (blanked): each swap stops at the cap, and
     * after RD3_LATCH_TIMEOUT_RUN of them the swaps stop waiting */
    {
        unsigned run = 0, waited = 0;
        for (unsigned i = 0; i < 10; i++) {
            if (!rd3_latch_should_wait(1, run))
                continue;
            uint64_t w = 0;
            enum rd3_latch_step s;
            do {
                w += POLL;
                s = rd3_latch_step(1, 1, 100, 100, w);
            } while (s == RD3_LATCH_WAIT);
            CHECK(s == RD3_LATCH_TIMEOUT && w <= RD3_LATCH_CAP_US + POLL);
            run++;
            waited++;
        }
        CHECK(waited == RD3_LATCH_TIMEOUT_RUN);
    }

    /* rd3_latch_panel */
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 1, 0x100) == RD3_PANEL_SHOWN);
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 1, 0x200) == RD3_PANEL_LEAVING);
    CHECK(rd3_latch_panel(0x100, 0x200, 1, 1, 0x200) == RD3_PANEL_NO);   /* seen latched */
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 0, 0x200) == RD3_PANEL_NO);   /* latched by now */
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 1, 0x300) == RD3_PANEL_NO);
    CHECK(rd3_latch_panel(0x100, 0x200, 0, 1, 0) == RD3_PANEL_NO);

    /* rd3_torn_bands / rd3_torn_classify */
    {
        uint32_t a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, b[8];
        memcpy(b, a, sizeof b);
        CHECK(rd3_torn_bands(a, b, 8) == 0);
        CHECK(rd3_torn_classify(rd3_torn_bands(a, b, 8), 0) == RD3_TORN_NONE);
        b[0] = 99;
        b[5] = 99;
        CHECK(rd3_torn_bands(a, b, 8) == 0x21u);
        CHECK(rd3_torn_bands(a, b, 4) == 0x01u);
        CHECK(rd3_torn_bands(NULL, b, 8) == 0);
        CHECK(rd3_torn_classify(0x21u, 0) == RD3_TORN_SHOWN);
        CHECK(rd3_torn_classify(0x21u, 1) == RD3_TORN_INPLACE);
    }

    /* rd3_log_due */
    CHECK(rd3_log_due(0, 8, 1023) && rd3_log_due(7, 8, 1023) && !rd3_log_due(8, 8, 1023));
    CHECK(rd3_log_due(1024, 8, 1023) && !rd3_log_due(1025, 8, 1023));

    /* the model: two backs taking turns */
    {
        const struct run before = simulate(0, 2), after = simulate(1, 2);
        printf("test_flip_latch: two backs, %u swaps: %u writes into the buffer the panel scanned without the wait, "
               "%u with it (longest wait %llu us, %u timeouts)\n", before.swaps, before.on_panel, after.on_panel,
               (unsigned long long)after.max_wait, after.timeouts);
        CHECK(before.on_panel > before.swaps / 2);   /* the old swap: most frames written on the panel */
        CHECK(after.on_panel == 0);
        CHECK(after.timeouts == 0);
        CHECK(after.max_wait <= PERIOD + POLL);
    }
    /* one back: the buffer drawn is the one shown either way (not counted
     * here; the kext counts it as rewritten in place) */
    {
        const struct run one = simulate(1, 1);
        CHECK(one.on_panel == 0 && one.timeouts == 0);
    }

    if (fails) {
        printf("test_flip_latch: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_flip_latch: all checks passed\n");
    return 0;
}
