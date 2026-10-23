/*
 * test_surf_pending.c - common/rdna3_surf_pending.h on the host (DRIVER-PLAN
 * N41: a CPU lock of a surface WindowServer's GPU is still writing waits for
 * that write, bounded, instead of reading stale or zero system memory).
 *
 * First each mark and the wait one by one, then models: two command buffers
 * in flight on one surface (the surface is ready only after both), a
 * pending write whose ready never comes (one bounded wait, cleared, the
 * late ready only counted), a full table (the oldest goes, never a wait
 * past the bound), saturation, and a page-off loop as the kernel runs it
 * (sleep until the deadline or a ready) against a GPU that completes in
 * time and one that does not.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "rdna3_surf_pending.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define MS 1000000ull

static void marks(void)
{
    struct rd3_sp t;
    uint32_t ev = 99, dropped = 99;
    memset(&t, 0, sizeof t);
    CHECK(rd3_sp_mark(&t, 0, 1, 0, &ev) == RD3_SPM_BAD && ev == 0);
    CHECK(rd3_sp_mark(0, 5, 1, 0, 0) == RD3_SPM_BAD);
    CHECK(rd3_sp_pending(&t, 5) == 0 && rd3_sp_busy(&t) == 0);
    CHECK(rd3_sp_wait_step(&t, 5, 0, 100 * MS, &dropped) == RD3_SPW_READY && dropped == 0);
    CHECK(rd3_sp_mark(&t, 5, 1, 1 * MS, &ev) == RD3_SPM_PENDING && ev == 0);
    CHECK(rd3_sp_pending(&t, 5) == 1 && rd3_sp_busy(&t) == 1);
    CHECK(rd3_sp_wait_step(&t, 5, 2 * MS, 100 * MS, &dropped) == RD3_SPW_WAIT);
    CHECK(rd3_sp_mark(&t, 5, 0, 3 * MS, 0) == RD3_SPM_READY);
    CHECK(rd3_sp_pending(&t, 5) == 0 && rd3_sp_busy(&t) == 0);
    CHECK(rd3_sp_wait_step(&t, 5, 4 * MS, 100 * MS, &dropped) == RD3_SPW_READY);
    /* a ready with nothing pending: late, nothing changes */
    CHECK(rd3_sp_mark(&t, 5, 0, 5 * MS, 0) == RD3_SPM_LATE && rd3_sp_busy(&t) == 0);
    /* id 0 never waits */
    CHECK(rd3_sp_wait_step(&t, 0, 0, 1, 0) == RD3_SPW_READY);
    /* the ABI's flag bits are distinct and above every plane */
    CHECK((RD3_SP_PLANE_GPU_MARK & RD3_SP_PLANE_TRYLOCK) == 0 && (RD3_SP_PLANE_FLAGS & 0xffffu) == 0);
}

/* two command buffers write surface 7; cb1 completes first */
static void overlapping(void)
{
    struct rd3_sp t;
    memset(&t, 0, sizeof t);
    CHECK(rd3_sp_mark(&t, 7, 1, 0, 0) == RD3_SPM_PENDING);        /* cb1 commit */
    CHECK(rd3_sp_mark(&t, 7, 1, 1 * MS, 0) == RD3_SPM_PENDING);   /* cb2 commit */
    CHECK(rd3_sp_mark(&t, 8, 1, 1 * MS, 0) == RD3_SPM_PENDING);   /* cb2 also writes 8 */
    CHECK(rd3_sp_pending(&t, 7) == 2);
    CHECK(rd3_sp_mark(&t, 7, 0, 5 * MS, 0) == RD3_SPM_STILL);     /* cb1 done: cb2 still writes it */
    CHECK(rd3_sp_wait_step(&t, 7, 6 * MS, 100 * MS, 0) == RD3_SPW_WAIT);
    CHECK(rd3_sp_wait_step(&t, 9, 6 * MS, 100 * MS, 0) == RD3_SPW_READY);   /* another surface never waits */
    CHECK(rd3_sp_mark(&t, 8, 0, 9 * MS, 0) == RD3_SPM_READY);
    CHECK(rd3_sp_mark(&t, 7, 0, 9 * MS, 0) == RD3_SPM_READY);     /* cb2 done */
    CHECK(rd3_sp_wait_step(&t, 7, 9 * MS, 100 * MS, 0) == RD3_SPW_READY);
    /* commit order is not completion order: cb2 completing first is the same */
    CHECK(rd3_sp_mark(&t, 7, 1, 10 * MS, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_mark(&t, 7, 1, 10 * MS, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_mark(&t, 7, 0, 11 * MS, 0) == RD3_SPM_STILL);
    CHECK(rd3_sp_mark(&t, 7, 0, 12 * MS, 0) == RD3_SPM_READY && rd3_sp_busy(&t) == 0);
}

/* a pending write that never completes */
static void never_completed(void)
{
    struct rd3_sp t;
    uint32_t dropped = 0;
    memset(&t, 0, sizeof t);
    CHECK(rd3_sp_mark(&t, 11, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_mark(&t, 11, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 11, 50 * MS, 100 * MS, &dropped) == RD3_SPW_WAIT && dropped == 0);
    CHECK(rd3_sp_wait_step(&t, 11, 100 * MS, 100 * MS, &dropped) == RD3_SPW_TIMEOUT && dropped == 2);
    /* cleared: the next page-off does not wait again */
    CHECK(rd3_sp_wait_step(&t, 11, 101 * MS, 201 * MS, &dropped) == RD3_SPW_READY);
    /* the readies that finally come are late, nothing else */
    CHECK(rd3_sp_mark(&t, 11, 0, 300 * MS, 0) == RD3_SPM_LATE);
    CHECK(rd3_sp_mark(&t, 11, 0, 300 * MS, 0) == RD3_SPM_LATE);
    CHECK(rd3_sp_busy(&t) == 0);
}

static void full_and_saturated(void)
{
    struct rd3_sp t;
    uint32_t ev = 0;
    memset(&t, 0, sizeof t);
    for (uint32_t i = 1; i <= RD3_SP_SLOTS; i++)
        CHECK(rd3_sp_mark(&t, i, 1, i, &ev) == RD3_SPM_PENDING && ev == 0);
    CHECK(rd3_sp_busy(&t) == RD3_SP_SLOTS);
    /* surface 1 is the oldest: it goes */
    CHECK(rd3_sp_mark(&t, 1000, 1, 100, &ev) == RD3_SPM_EVICTED && ev == 1);
    CHECK(rd3_sp_pending(&t, 1) == 0 && rd3_sp_pending(&t, 1000) == 1 && rd3_sp_busy(&t) == RD3_SP_SLOTS);
    CHECK(rd3_sp_mark(&t, 1001, 1, 101, &ev) == RD3_SPM_EVICTED && ev == 2);
    /* a re-mark does not make a surface younger than one marked later (its first mark counts) */
    CHECK(rd3_sp_mark(&t, 3, 1, 102, &ev) == RD3_SPM_PENDING && ev == 0);
    CHECK(rd3_sp_mark(&t, 1002, 1, 103, &ev) == RD3_SPM_EVICTED && ev == 3);
    CHECK(rd3_sp_clear(&t) == RD3_SP_SLOTS && rd3_sp_busy(&t) == 0);
    /* saturation */
    memset(&t, 0, sizeof t);
    for (uint32_t i = 0; i < RD3_SP_MAX_COUNT; i++) (void)rd3_sp_mark(&t, 4, 1, 0, 0);
    CHECK(rd3_sp_pending(&t, 4) == RD3_SP_MAX_COUNT);
    CHECK(rd3_sp_mark(&t, 4, 1, 0, 0) == RD3_SPM_SATURATED && rd3_sp_pending(&t, 4) == RD3_SP_MAX_COUNT);
}

/* the kernel's page-off loop: steps of 'tick' ns; the GPU's ready comes at 'ready_at' (0: never) */
static enum rd3_sp_wait_res page_off(struct rd3_sp *t, uint32_t id, uint64_t start, uint64_t ready_at,
                                     uint64_t *waited_ns, unsigned *sleeps)
{
    const uint64_t deadline = start + (uint64_t)RD3_SP_WAIT_MS * MS, tick = 7 * MS;
    uint64_t now = start;
    enum rd3_sp_wait_res r;
    *sleeps = 0;
    for (;;) {
        r = rd3_sp_wait_step(t, id, now, deadline, 0);
        if (r != RD3_SPW_WAIT) break;
        /* sleep until the deadline or a ready wakes it */
        uint64_t next = now + tick > deadline ? deadline : now + tick;
        if (ready_at && ready_at > now && ready_at <= next) {
            next = ready_at;
            CHECK(rd3_sp_mark(t, id, 0, ready_at, 0) == RD3_SPM_READY);
        }
        now = next;
        (*sleeps)++;
        CHECK(*sleeps < 1000);
        if (*sleeps >= 1000) break;
    }
    *waited_ns = now - start;
    return r;
}

static void page_off_loop(void)
{
    struct rd3_sp t;
    uint64_t waited;
    unsigned sleeps;
    memset(&t, 0, sizeof t);
    /* the GPU finishes 23 ms in: the page-off copies then, not before */
    CHECK(rd3_sp_mark(&t, 21, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(page_off(&t, 21, 0, 23 * MS, &waited, &sleeps) == RD3_SPW_READY);
    CHECK(waited == 23 * MS && sleeps >= 1);
    /* never: the wait ends at the bound, cleared */
    CHECK(rd3_sp_mark(&t, 22, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(page_off(&t, 22, 1000 * MS, 0, &waited, &sleeps) == RD3_SPW_TIMEOUT);
    CHECK(waited == (uint64_t)RD3_SP_WAIT_MS * MS && rd3_sp_pending(&t, 22) == 0);
    /* not pending: no wait at all */
    CHECK(page_off(&t, 23, 0, 0, &waited, &sleeps) == RD3_SPW_READY && waited == 0 && sleeps == 0);
}

int main(void)
{
    marks();
    overlapping();
    never_completed();
    full_and_saturated();
    page_off_loop();
    if (fails) { printf("test_surf_pending: %d FAILED\n", fails); return 1; }
    printf("test_surf_pending: ok\n");
    return 0;
}
