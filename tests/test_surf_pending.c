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
 * slot holding only forgiven marks before it drops a pending surface. Then
 * the round-2 reviews: a ready that never comes costs at most two bounded
 * waits, not one per later page-off (the ghost of its forgiven mark goes for
 * good at the second timeout), forgiven marks age out, and the timeout
 * budget stops the waits after RD3_SP_BUDGET timeouts with no real ready.
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

/* round-2 review of N41: a lost ready, then a page-off and a write (pending + its own ready) per frame. The
 * write's own ready takes the forgiven mark (the table cannot tell it from the lost one's late ready), so the
 * lost mark's ghost sits in the count: the second timeout drops it for good. Before the fix every page-off of
 * the surface ran into the bound (forgiven -> count -> forgiven ...) */
static void lost_ready_bounded(void)
{
    struct rd3_sp t;
    uint64_t now = 1000 * MS;
    unsigned timeouts = 0, waits = 0;
    memset(&t, 0, sizeof t);
    CHECK(rd3_sp_mark(&t, 7, 1, now, 0) == RD3_SPM_PENDING);   /* write 1: its ready never comes */
    for (unsigned i = 0; i < 8; i++) {
        uint64_t ep = 0;
        enum rd3_sp_wait_res r = rd3_sp_wait_step(&t, 7, now, now + 100 * MS, 0, &ep);
        if (r == RD3_SPW_WAIT) {
            waits++;
            now += 100 * MS;
            r = rd3_sp_wait_step(&t, 7, now, now, 0, &ep);
            if (r == RD3_SPW_TIMEOUT) timeouts++;
        }
        now += 1 * MS;
        CHECK(rd3_sp_mark(&t, 7, 1, now, 0) == RD3_SPM_PENDING);   /* the next write */
        (void)rd3_sp_mark(&t, 7, 0, now + 1 * MS, 0);              /* and its own ready */
        now += 2 * MS;
    }
    CHECK(timeouts == 2 && waits == 2);
    CHECK(rd3_sp_pending(&t, 7) == 0 && rd3_sp_forgiven(&t, 7) == 0 && rd3_sp_busy(&t) == 0);
}

/* forgiven marks older than RD3_SP_FORGIVE_MS are aged out: the next write long after is not misread */
static void forgiven_aged(void)
{
    struct rd3_sp t;
    memset(&t, 0, sizeof t);
    CHECK(rd3_sp_mark(&t, 3, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 3, 100 * MS, 100 * MS, 0, 0) == RD3_SPW_TIMEOUT && rd3_sp_forgiven(&t, 3) == 1);
    const uint64_t later = 100 * MS + (uint64_t)RD3_SP_FORGIVE_MS * MS + 1;
    CHECK(rd3_sp_mark(&t, 3, 1, later, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_mark(&t, 3, 0, later + 1 * MS, 0) == RD3_SPM_READY && t.aged == 1);   /* its own ready */
    CHECK(rd3_sp_find(&t, 3) == 0 && rd3_sp_wait_step(&t, 3, later + 2 * MS, later + 102 * MS, 0, 0) == RD3_SPW_READY);
    /* a forgiven-only slot whose late ready comes after the age: LATE, the slot goes */
    CHECK(rd3_sp_mark(&t, 4, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 4, 100 * MS, 100 * MS, 0, 0) == RD3_SPW_TIMEOUT);
    CHECK(rd3_sp_mark(&t, 4, 0, later, 0) == RD3_SPM_LATE && rd3_sp_find(&t, 4) == 0 && t.aged == 2);
    /* within the age a late ready still takes its forgiven mark (the earlier rule) */
    CHECK(rd3_sp_mark(&t, 5, 1, 0, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_wait_step(&t, 5, 100 * MS, 100 * MS, 0, 0) == RD3_SPW_TIMEOUT);
    CHECK(rd3_sp_mark(&t, 5, 1, 200 * MS, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_mark(&t, 5, 0, 300 * MS, 0) == RD3_SPM_LATE && rd3_sp_pending(&t, 5) == 1 && t.aged == 2);
}

/* the timeout budget (security review of N41): RD3_SP_BUDGET timeouts with no real ready between them and new
 * waits are skipped (SKIP, nothing cleared) until a real ready; LATE readies do not end it */
static void timeout_budget(void)
{
    struct rd3_sp t;
    uint64_t ep;
    memset(&t, 0, sizeof t);
    for (uint32_t i = 1; i <= RD3_SP_BUDGET; i++) {
        CHECK(rd3_sp_mark(&t, i, 1, 0, 0) == RD3_SPM_PENDING);
        CHECK(t.starved == 0);
        CHECK(rd3_sp_wait_step(&t, i, 100 * MS, 100 * MS, 0, 0) == RD3_SPW_TIMEOUT);
    }
    CHECK(t.starved == 1 && t.starvations == 1);
    CHECK(rd3_sp_mark(&t, 50, 1, 101 * MS, 0) == RD3_SPM_PENDING);
    ep = 0;
    CHECK(rd3_sp_wait_step(&t, 50, 102 * MS, 202 * MS, 0, &ep) == RD3_SPW_SKIP && ep == 0 && t.skipped == 1);
    CHECK(rd3_sp_pending(&t, 50) == 1);                                       /* nothing cleared */
    CHECK(rd3_sp_mark(&t, 1, 0, 103 * MS, 0) == RD3_SPM_LATE && t.starved);   /* a late ready: still starved */
    CHECK(rd3_sp_mark(&t, 50, 1, 104 * MS, 0) == RD3_SPM_PENDING);
    CHECK(rd3_sp_mark(&t, 50, 0, 105 * MS, 0) == RD3_SPM_STILL && !t.starved); /* a real one: waits again */
    ep = 0;
    CHECK(rd3_sp_wait_step(&t, 50, 106 * MS, 206 * MS, 0, &ep) == RD3_SPW_WAIT && ep);
    /* a real ready between timeouts starts the count again */
    memset(&t, 0, sizeof t);
    for (uint32_t i = 1; i <= 3 * RD3_SP_BUDGET; i++) {
        CHECK(rd3_sp_mark(&t, i, 1, 0, 0) == RD3_SPM_PENDING);
        CHECK(rd3_sp_wait_step(&t, i, 100 * MS, 100 * MS, 0, 0) == RD3_SPW_TIMEOUT);
        if (i % (RD3_SP_BUDGET - 1) == 0) {
            CHECK(rd3_sp_mark(&t, 1000 + i, 1, 0, 0) == RD3_SPM_PENDING);
            CHECK(rd3_sp_mark(&t, 1000 + i, 0, 0, 0) == RD3_SPM_READY);
        }
    }
    CHECK(!t.starved && !t.starvations);
    /* the switch-off starts afresh too */
    for (uint32_t i = 1; i <= RD3_SP_BUDGET; i++) {
        (void)rd3_sp_mark(&t, 2000 + i, 1, 0, 0);
        (void)rd3_sp_wait_step(&t, 2000 + i, 100 * MS, 100 * MS, 0, 0);
    }
    CHECK(t.starved);
    (void)rd3_sp_clear(&t);
    CHECK(!t.starved && !t.timeouts_in_row);
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
    lost_ready_bounded();
    forgiven_aged();
    timeout_budget();
    if (fails) { printf("test_surf_pending: %d FAILED\n", fails); return 1; }
    printf("test_surf_pending: ok\n");
    return 0;
}
