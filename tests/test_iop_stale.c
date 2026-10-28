/*
 * test_iop_stale.c - common/rdna3_iop_stale.h on the host (DRIVER-PLAN N59, card run 2026-10-26: animations
 * step back to an earlier frame).
 *
 * The model: WindowServer draws frame n into buffer n % 3 of its present ring every 16.667 ms and sends the
 * transaction at once; the plugin's copy-out of the pixels (which moves the surface's seed) lands 0..6 ms later,
 * sometimes after the display pipe's copy. Without the wait the panel then gets the frame three back (the
 * owner's 1,2,3,1,4,...); with rd3_is_wait_us / rd3_is_waited, polled every 1 ms as the kext polls, never. Then
 * the cap (ECC review of 5a8765b): buffers re-presented with nothing written, or timeout, timeout, timeout,
 * moved repeating, wait in vain at most RD3_IS_CAP_MS per second; a wait that sees the write is never charged;
 * the next window waits again.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_iop_stale.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t rng = 777u;
static uint32_t rnd(uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

enum { FRAME_NS = 16666667 };

/* 600 frames, `late_every` in 1 late; returns the frames that showed an older frame than the one before */
static unsigned run(uint32_t wait_ms, uint32_t late_every, unsigned *stale_seen, unsigned *waited_ok)
{
    rd3_is_head h; rd3_is_init(&h);
    uint32_t seed[3] = {0, 0, 0}, content[3] = {0, 0, 0};
    unsigned back = 0; uint32_t shown = 0;
    *stale_seen = *waited_ok = 0;
    for (uint32_t n = 3; n < 603; n++) {
        const uint64_t now = (uint64_t)n * FRAME_NS;
        const uint32_t b = n % 3, id = 100 + b;
        const uint32_t late_ms = rnd(late_every) == 0 ? 1 + rnd(6) : 0;
        if (!late_ms) { content[b] = n; seed[b]++; }
        uint64_t last = 0;
        int v = rd3_is_check(&h, id, seed[b], seed[b], &last);
        if (n >= 6) CHECK(v != RD3_IS_NEW);
        if (v == RD3_IS_STALE) {
            (*stale_seen)++;
            CHECK(late_ms > 0);
            const uint64_t allow = rd3_is_wait_us(&h, wait_ms, now);
            if (allow) {
                int moved = 0; uint64_t us = 0;
                while (us < allow && !moved) { us += 1000; if (us >= (uint64_t)late_ms * 1000u) moved = 1; }   /* 1 ms polls */
                if (moved) { content[b] = n; seed[b]++; (*waited_ok)++; }
                rd3_is_waited(&h, moved, us);
            }
        }
        rd3_is_note(&h, id, seed[b], seed[b], now);
        if (content[b] < shown) back++;
        shown = content[b];
        if (late_ms && content[b] != n) { content[b] = n; seed[b]++; }   /* the late copy-out lands after the copy */
    }
    return back;
}

int main(void)
{
    unsigned stale, ok;
    const unsigned back0 = run(0, 4, &stale, &ok);
    CHECK(back0 > 50);                 /* count only: the step back the owner saw */
    CHECK(stale > 50 && ok == 0);
    rng = 777u;
    const unsigned back1 = run(RD3_IS_WAIT_MS, 4, &stale, &ok);
    CHECK(back1 == 0);                 /* the wait: never an older frame; successful waits are not charged */
    CHECK(stale > 50 && ok == stale);
    rng = 777u;
    const unsigned back2 = run(RD3_IS_WAIT_MS, 1, &stale, &ok);
    CHECK(back2 == 0 && ok == stale);  /* every frame late: still all waited for */

    /* the cap: an unchanged buffer re-presented at 60 Hz waits in vain at most RD3_IS_CAP_MS per second */
    rd3_is_head h; rd3_is_init(&h);
    rd3_is_note(&h, 7, 5, 5, 1);
    uint64_t vain = 0; unsigned capped_windows = 0;
    for (unsigned i = 0; i < 180; i++) {   /* 3 s */
        const uint64_t now = 1000000ull + (uint64_t)i * FRAME_NS;
        CHECK(rd3_is_check(&h, 7, 5, 5, NULL) == RD3_IS_STALE);
        const uint64_t a = rd3_is_wait_us(&h, 12, now);
        CHECK(a <= 12000u);
        if (a) { vain += a; rd3_is_waited(&h, 0, a); }
        else if (h.capped == 1) capped_windows++;
    }
    CHECK(vain == 3u * RD3_IS_CAP_MS * 1000u);   /* exactly the cap in each of the 3 windows */
    CHECK(capped_windows == 3);                  /* the kext logs once per window that caps */
    /* timeout, timeout, timeout, moved repeating (the review's case): the timeouts still hit the cap */
    rd3_is_init(&h);
    vain = 0;
    for (unsigned i = 0; i < 60; i++) {
        const uint64_t now = 1000000ull + (uint64_t)i * FRAME_NS;
        const uint64_t a = rd3_is_wait_us(&h, 12, now);
        if (!a) continue;
        const int moved = i % 4 == 3;
        if (!moved) vain += a;
        rd3_is_waited(&h, moved, moved ? 2000u : a);
    }
    CHECK(vain == RD3_IS_CAP_MS * 1000u);
    CHECK(rd3_is_wait_us(&h, 0, 2000000000ull) == 0);
    CHECK(rd3_is_wait_us(&h, 12, 5000000000ull) == 12000u);   /* a new window waits again */

    /* seeds that moved are FRESH, an unknown id NEW, slots are reused round robin */
    rd3_is_note(&h, 7, 5, 5, 1);
    CHECK(rd3_is_check(&h, 7, 5, 6, NULL) == RD3_IS_FRESH);
    CHECK(rd3_is_check(&h, 7, 6, 5, NULL) == RD3_IS_FRESH);
    CHECK(rd3_is_check(&h, 8, 5, 5, NULL) == RD3_IS_NEW);
    CHECK(rd3_is_check(&h, 0, 0, 0, NULL) == RD3_IS_NEW);
    rd3_is_init(&h);
    for (uint32_t i = 1; i <= RD3_IS_SLOTS + 1; i++) rd3_is_note(&h, i, i, i, i);
    CHECK(rd3_is_check(&h, 1, 1, 1, NULL) == RD3_IS_NEW);          /* the oldest went */
    uint64_t t = 0;
    CHECK(rd3_is_check(&h, RD3_IS_SLOTS + 1, RD3_IS_SLOTS + 1, RD3_IS_SLOTS + 1, &t) == RD3_IS_STALE && t == RD3_IS_SLOTS + 1);
    CHECK(rd3_is_wait_ms(-3) == 0 && rd3_is_wait_ms(0) == 0 && rd3_is_wait_ms(12) == 12 && rd3_is_wait_ms(5000) == RD3_IS_WAIT_MAX_MS);
    CHECK(rd3_is_wait_ms_u(0) == 0 && rd3_is_wait_ms_u(12) == 12 && rd3_is_wait_ms_u(0x80000000u) == RD3_IS_WAIT_MAX_MS
          && rd3_is_wait_ms_u(0xffffffffu) == RD3_IS_WAIT_MAX_MS);

    printf("test_iop_stale: %s (count only: %u frames stepped back; with the wait: %u, every frame late: %u)\n",
           fails ? "FAILED" : "ok", back0, back1, back2);
    return fails != 0;
}
