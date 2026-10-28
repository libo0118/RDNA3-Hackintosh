/*
 * test_iop_stale.c - common/rdna3_iop_stale.h on the host (DRIVER-PLAN N59, card run 2026-10-26: animations
 * step back to an earlier frame).
 *
 * The model: WindowServer draws frame n into buffer n % 3 of its present ring and sends the transaction at
 * once; the plugin's copy-out of the pixels (which moves the surface's seed) lands 0..6 ms later, sometimes
 * after the display pipe's copy. Without the wait the panel then gets the frame three back (the owner's
 * 1,2,3,1,4,...); with rd3_is_should_wait / rd3_is_waited, polled every 1 ms as the kext polls, never. Then
 * the budget: a buffer re-presented with nothing written waits RD3_IS_BUDGET times, then only every
 * RD3_IS_PROBE-th time, and one wait that sees the seeds move arms the waits again.
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

/* one run of 600 frames; returns the number of frames that showed an older frame than the one before */
static unsigned run(uint32_t wait_ms, unsigned *stale_seen, unsigned *waited_ok)
{
    rd3_is_head h; rd3_is_init(&h);
    uint32_t seed[3] = {0, 0, 0}, content[3] = {0, 0, 0};
    unsigned back = 0; uint32_t shown = 0;
    *stale_seen = *waited_ok = 0;
    for (uint32_t n = 3; n < 603; n++) {
        const uint32_t b = n % 3, id = 100 + b;
        const uint32_t late_ms = rnd(4) == 0 ? 1 + rnd(6) : 0;   /* a quarter of the copy-outs come after the transaction */
        if (!late_ms) { content[b] = n; seed[b]++; }
        uint64_t last = 0;
        int v = rd3_is_check(&h, id, seed[b], seed[b], &last);
        if (n >= 6) CHECK(v != RD3_IS_NEW);
        if (v == RD3_IS_STALE) {
            (*stale_seen)++;
            CHECK(late_ms > 0);
            if (rd3_is_should_wait(&h, wait_ms)) {
                int moved = 0;
                for (uint32_t t = 1; t <= wait_ms; t++)           /* the kext polls every 1 ms */
                    if (t >= late_ms) { content[b] = n; seed[b]++; moved = 1; break; }
                rd3_is_waited(&h, moved);
                if (moved) (*waited_ok)++;
            }
        }
        rd3_is_note(&h, id, seed[b], seed[b], n);
        if (content[b] < shown) back++;
        shown = content[b];
        if (late_ms && content[b] != n) { content[b] = n; seed[b]++; }   /* the late copy-out lands after the copy */
    }
    return back;
}

int main(void)
{
    unsigned stale, ok;
    const unsigned back0 = run(0, &stale, &ok);
    CHECK(back0 > 50);           /* count only: the step back the owner saw */
    CHECK(stale > 50 && ok == 0);
    rng = 777u;
    const unsigned back1 = run(RD3_IS_WAIT_MS, &stale, &ok);
    CHECK(back1 == 0);           /* the wait: never an older frame */
    CHECK(stale > 50 && ok == stale);

    /* the budget: an unchanged buffer re-presented */
    rd3_is_head h; rd3_is_init(&h);
    rd3_is_note(&h, 7, 5, 5, 1);
    unsigned waits = 0;
    for (unsigned i = 0; i < 2 * RD3_IS_PROBE + RD3_IS_BUDGET; i++) {
        CHECK(rd3_is_check(&h, 7, 5, 5, NULL) == RD3_IS_STALE);
        if (rd3_is_should_wait(&h, 12)) { waits++; rd3_is_waited(&h, 0); }
    }
    CHECK(waits == RD3_IS_BUDGET + 2);   /* the budget, then two probes */
    rd3_is_waited(&h, 1);                /* a probe that saw the write: armed again */
    CHECK(rd3_is_should_wait(&h, 12) == 1);
    CHECK(rd3_is_should_wait(&h, 0) == 0);

    /* seeds that moved are FRESH, an unknown or forgotten id NEW, slots are reused round robin */
    CHECK(rd3_is_check(&h, 7, 5, 6, NULL) == RD3_IS_FRESH);
    CHECK(rd3_is_check(&h, 7, 6, 5, NULL) == RD3_IS_FRESH);
    CHECK(rd3_is_check(&h, 8, 5, 5, NULL) == RD3_IS_NEW);
    rd3_is_forget(&h, 7);
    CHECK(rd3_is_check(&h, 7, 5, 5, NULL) == RD3_IS_NEW);
    CHECK(rd3_is_check(&h, 0, 0, 0, NULL) == RD3_IS_NEW);
    for (uint32_t i = 1; i <= RD3_IS_SLOTS + 1; i++) rd3_is_note(&h, i, i, i, i);
    CHECK(rd3_is_check(&h, 1, 1, 1, NULL) == RD3_IS_NEW);          /* the oldest went */
    uint64_t t = 0;
    CHECK(rd3_is_check(&h, RD3_IS_SLOTS + 1, RD3_IS_SLOTS + 1, RD3_IS_SLOTS + 1, &t) == RD3_IS_STALE && t == RD3_IS_SLOTS + 1);
    CHECK(rd3_is_wait_ms(-3) == 0 && rd3_is_wait_ms(0) == 0 && rd3_is_wait_ms(12) == 12 && rd3_is_wait_ms(5000) == RD3_IS_WAIT_MAX_MS);

    printf("test_iop_stale: %s (count only: %u frames stepped back; with the wait: %u)\n", fails ? "FAILED" : "ok", back0, back1);
    return fails != 0;
}
