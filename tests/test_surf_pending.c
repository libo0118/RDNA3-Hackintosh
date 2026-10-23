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
 * time and one that does not. Then the ECC review of N41: a late ready
 * after a timeout never takes a newer command buffer's count (the dropped
 * marks are forgiven, open item 4), a waiter whose surface was cleared under
 * it is told CLEARED (not a successful wait), and a full table reuses a
 * slot holding only forgiven marks before it drops a pending surface.
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
    CHECK(rd3_sp_wait_step(&t, 5, 0, 100 * MS, &dropped, 0) == RD3_SPW_READY && dropped == 0);
    CHECK(rd3_sp_mark(&t, 5, 1, 1 * MS, &ev) == RD3_SPM_PENDING && ev == 0);
    CHECK(rd3_sp_pending(&t, 5) == 1 && rd3_sp_busy(&t) == 1);
    CHECK(rd3_sp_wait_step(&t, 5, 2 * MS, 100 * MS, &dropped, 0) == RD3_SPW_WAIT);
    CHECK(rd3_sp_mark(&t, 5, 0, 3 * MS, 0) == RD3_SPM_READY);
    CHECK(rd3_sp_pending(&t, 5) == 0 && rd3_sp_busy(&t) == 0);
    CHECK(rd3_sp_wait_step(&t, 5, 4 * MS, 100 * MS, &dropped, 0) == RD3_SPW_READY);
    /* a ready with nothing pending: late, nothing changes */
    CHECK(rd3_sp_mark(&t, 5, 0, 5 * MS, 0) == RD3_SPM_LATE && rd3_sp_busy(&t) == 0);
    /* id 0 never waits */
    CHECK(rd3_sp_wait_step(&t, 0, 0, 1, 0, 0) == RD3_SPW_READY);
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
    CHECK(rd3_sp_wait_step(&t, 7, 6 * MS, 100 * MS, 0, 0) == RD3_SPW_WAIT);
    CHECK(rd3_sp_wait_step(&t, 9, 6 * MS, 100 * MS, 0, 0) == RD3_SPW_READY);   /* another surface never waits */
    CHECK(rd3_sp_mark(&t, 8, 0, 9 * MS, 0) == RD3_SPM_READY);
    CHECK(rd3_sp_mark(&t, 7, 0, 9 * MS, 0) == RD3_SPM_READY);     /* cb2 done */
    CHECK(rd3_sp_wait_step(&t, 7, 9 * MS, 100 * MS, 0, 0) == RD3_SPW_READY);
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
    CHECK(rd3_sp_wait_step(&t, 11, 50 * MS, 100 * MS, &dropped, 0) == RD3_SPW_WAIT && dropped == 0);
    CHECK(rd3_sp_wait_step(&t, 11, 100 * MS, 100 * MS, &dropped, 0) == RD3_SPW_TIMEOUT && dropped == 2);
    /* cleared: the next page-off does not wait again */
    CHECK(rd3_sp_wait_step(&t, 11, 101 * MS, 201 * MS, &dropped, 0) == RD3_SPW_READY);
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
        r = rd3_sp_wait_step(t, id, now, deadline, 0, 0);
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

/* ECC review of N41, open item 4: a ready that comes after its wait timed out
 * must not take the count of a newer command buffer's pending mark */
static void late_after_timeout(void)
{
    struct rd3_sp t;
    uint32_t dropped = 0;
    memset(&t, 0, sizeof t);
    CHECK(rd3_sp_mark(&t, 5, 1, 0, 0) == RD3_SPM_PENDING);                                  /* cb1 commit */
    CHECK(rd3_sp_wait_step(&t, 5, 100 * MS, 100 * MS, &dropped, 0) == RD3_SPW_TIMEOUT && dropped == 1);
    CHECK(rd3_sp_pending(&t, 5) == 0 && rd3_sp_forgiven(&t, 5) == 1 && rd3_sp_busy(&t) == 0);
    CHECK(rd3_sp_mark(&t, 5, 1, 101 * MS, 0) == RD3_SPM_PENDING);                           /* cb2 commit */
    CHECK(rd3_sp_mark(&t, 5, 0, 102 * MS, 0) == RD3_SPM_LATE);                              /* cb1's late ready */
    CHECK(rd3_sp_pending(&t, 5) == 1 && rd3_sp_forgiven(&t, 5) == 0);                       /* cb2 still writes */
    CHECK(rd3_sp_wait_step(&t, 5, 103 * MS, 203 * MS, 0, 0) == RD3_SPW_WAIT);
    CHECK(rd3_sp_mark(&t, 5, 0, 104 * MS, 0) == RD3_SPM_READY && rd3_sp_busy(&t) == 0);     /* cb2's ready */
    CHECK(rd3_sp_find(&t, 5) == 0);                                                         /* the slot is free again */
    /* the other order: cb2's ready first takes the forgiven mark (LATE), cb1's late one then ends the wait - later,
     * never earlier than cb2's write */
    CHECK(rd3_sp_mark(&t, 6, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 6, 100 * MS, 100 * MS, 0, 0) == RD3_SPW_TIMEOUT);
    CHECK(rd3_sp_mark(&t, 6, 1, 101 * MS, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_mark(&t, 6, 0, 102 * MS, 0) == RD3_SPM_LATE && rd3_sp_pending(&t, 6) == 1);
    CHECK(rd3_sp_mark(&t, 6, 0, 103 * MS, 0) == RD3_SPM_READY && rd3_sp_find(&t, 6) == 0);
    /* forgiven marks saturate, never wrap */
    memset(&t, 0, sizeof t);
    for (uint32_t i = 0; i < RD3_SP_MAX_COUNT; i++) (void)rd3_sp_mark(&t, 9, 1, 0, 0);
    CHECK(rd3_sp_wait_step(&t, 9, 100 * MS, 100 * MS, 0, 0) == RD3_SPW_TIMEOUT);
    CHECK(rd3_sp_mark(&t, 9, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 9, 200 * MS, 100 * MS, 0, 0) == RD3_SPW_TIMEOUT);
    CHECK(rd3_sp_forgiven(&t, 9) == RD3_SP_MAX_COUNT);
}

/* silent-failure review 7: a second waiter whose surface another waiter's timeout (or an eviction, or the
 * switch-off) cleared is told CLEARED, not READY: its write did not finish */
static void cleared_waiters(void)
{
    struct rd3_sp t;
    uint64_t ea = 0, eb = 0;
    uint32_t ev = 0;
    memset(&t, 0, sizeof t);
    CHECK(rd3_sp_mark(&t, 7, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 7, 10 * MS, 100 * MS, 0, &ea) == RD3_SPW_WAIT && ea);       /* waiter A */
    CHECK(rd3_sp_wait_step(&t, 7, 20 * MS, 120 * MS, 0, &eb) == RD3_SPW_WAIT && eb == ea); /* waiter B */
    CHECK(rd3_sp_wait_step(&t, 7, 100 * MS, 100 * MS, 0, &ea) == RD3_SPW_TIMEOUT);        /* A times out */
    CHECK(rd3_sp_wait_step(&t, 7, 101 * MS, 120 * MS, 0, &eb) == RD3_SPW_CLEARED);        /* B: not ready */
    /* even when a new episode began in between */
    eb = 0;
    CHECK(rd3_sp_mark(&t, 8, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 8, 1 * MS, 50 * MS, 0, &eb) == RD3_SPW_WAIT);
    ea = 0;
    CHECK(rd3_sp_wait_step(&t, 8, 2 * MS, 2 * MS, 0, &ea) == RD3_SPW_TIMEOUT);
    CHECK(rd3_sp_mark(&t, 8, 1, 3 * MS, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 8, 4 * MS, 50 * MS, 0, &eb) == RD3_SPW_CLEARED);
    /* a real ready, then a new episode: the earlier waiter's write is done (READY) */
    memset(&t, 0, sizeof t);
    eb = 0;
    CHECK(rd3_sp_mark(&t, 4, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 4, 1 * MS, 50 * MS, 0, &eb) == RD3_SPW_WAIT);
    CHECK(rd3_sp_mark(&t, 4, 0, 2 * MS, 0) == RD3_SPM_READY);
    CHECK(rd3_sp_mark(&t, 4, 1, 3 * MS, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 4, 4 * MS, 50 * MS, 0, &eb) == RD3_SPW_READY);
    /* evicted under a waiter */
    memset(&t, 0, sizeof t);
    for (uint32_t i = 1; i <= RD3_SP_SLOTS; i++) (void)rd3_sp_mark(&t, i, 1, i, 0);
    eb = 0;
    CHECK(rd3_sp_wait_step(&t, 1, 1 * MS, 50 * MS, 0, &eb) == RD3_SPW_WAIT);
    CHECK(rd3_sp_mark(&t, 500, 1, 2 * MS, &ev) == RD3_SPM_EVICTED && ev == 1);
    CHECK(rd3_sp_wait_step(&t, 1, 3 * MS, 50 * MS, 0, &eb) == RD3_SPW_CLEARED);
    /* switched off under a waiter */
    eb = 0;
    CHECK(rd3_sp_wait_step(&t, 2, 1 * MS, 50 * MS, 0, &eb) == RD3_SPW_WAIT);
    CHECK(rd3_sp_clear(&t) == RD3_SP_SLOTS);
    CHECK(rd3_sp_wait_step(&t, 2, 3 * MS, 50 * MS, 0, &eb) == RD3_SPW_CLEARED);
    /* a first step on a surface that is not pending is READY, whatever was cleared before */
    eb = 0;
    CHECK(rd3_sp_wait_step(&t, 2, 4 * MS, 50 * MS, 0, &eb) == RD3_SPW_READY && eb == 0);
}

/* a full table reuses a slot holding only forgiven marks before it drops a pending surface */
static void forgiven_evicted_first(void)
{
    struct rd3_sp t;
    uint32_t ev = 99;
    memset(&t, 0, sizeof t);
    for (uint32_t i = 1; i <= RD3_SP_SLOTS; i++) (void)rd3_sp_mark(&t, i, 1, i, 0);
    CHECK(rd3_sp_wait_step(&t, 40, 100 * MS, 100 * MS, 0, 0) == RD3_SPW_TIMEOUT);   /* 40: forgiven only */
    CHECK(rd3_sp_busy(&t) == RD3_SP_SLOTS - 1);
    CHECK(rd3_sp_mark(&t, 900, 1, 0, &ev) == RD3_SPM_PENDING && ev == 0);            /* 40's slot, nothing evicted */
    CHECK(rd3_sp_pending(&t, 1) == 1 && rd3_sp_pending(&t, 900) == 1 && rd3_sp_forgiven(&t, 40) == 0);
    CHECK(rd3_sp_mark(&t, 40, 0, 0, 0) == RD3_SPM_LATE);                             /* its late ready: nothing held */
    CHECK(rd3_sp_mark(&t, 901, 1, 0, &ev) == RD3_SPM_EVICTED && ev == 1);            /* then the oldest pending */
}

int main(void)
{
    marks();
    overlapping();
    never_completed();
    full_and_saturated();
    page_off_loop();
    late_after_timeout();
    cleared_waiters();
    forgiven_evicted_first();
    if (fails) { printf("test_surf_pending: %d FAILED\n", fails); return 1; }
    printf("test_surf_pending: ok\n");
    return 0;
}
