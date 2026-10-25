/*
 * test_gpu_busy.c - common/rdna3_gpu_busy.h (the rings' busy time and the
 * GPU time per process, amdgpu_ctx_fence_time's model) and
 * common/rdna3_gpu_util.h (the utilisation window, IOUserClientCreator's
 * pid) on the host (DRIVER-PLAN N49: Activity Monitor showed GPU 0 % for
 * every process, the SMU's activity reads 100 % at idle).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "rdna3_gpu_busy.h"
#include "rdna3_gpu_util.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define MS 1000000ull

static struct rd3_busy B;

static uint64_t pid_total(int32_t pid)
{
    struct rd3_busy_pid *p = rd3_busy_pid_slot(&B, pid, 0);
    return p ? p->total_ns : 0;
}

/* one job: busy from submission to the fence read, all of it the process's */
static void single(void)
{
    struct rd3_busy_snap s;
    rd3_busy_init(&B);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 1, 208, 10 * MS);
    rd3_busy_totals(&B, 12 * MS, &s);
    CHECK(s.any_ns == 2 * MS && s.ring_ns[RD3_BUSY_GFX] == 2 * MS && s.inflight == 1);   /* in progress counts */
    CHECK(rd3_busy_done(&B, RD3_BUSY_GFX, 0, 13 * MS) == 0);   /* fence 0: not yet */
    CHECK(rd3_busy_done(&B, RD3_BUSY_GFX, 1, 14 * MS) == 1);
    rd3_busy_totals(&B, 100 * MS, &s);
    CHECK(s.any_ns == 4 * MS && s.ring_ns[RD3_BUSY_GFX] == 4 * MS && s.inflight == 0);
    CHECK(pid_total(208) == 4 * MS && s.jobs == 1 && !s.lost_ns);
}

/* jobs queued behind each other on one ring: a job starts when the one before
 * ended, the per-job times add up to the ring's busy time (amdgpu counts both
 * from run_job: 3 + 5 = 8 ms here instead of 5) */
static void queued(void)
{
    struct rd3_busy_snap s;
    rd3_busy_init(&B);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 7, 208, 0);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 8, 814, 2 * MS);
    CHECK(rd3_busy_done(&B, RD3_BUSY_GFX, 7, 3 * MS) == 1);
    CHECK(rd3_busy_done(&B, RD3_BUSY_GFX, 8, 5 * MS) == 1);
    rd3_busy_totals(&B, 9 * MS, &s);
    CHECK(pid_total(208) == 3 * MS && pid_total(814) == 2 * MS);
    CHECK(s.ring_ns[RD3_BUSY_GFX] == 5 * MS && pid_total(208) + pid_total(814) == s.ring_ns[RD3_BUSY_GFX]);
    /* two ended at one fence read: the first gets the span, the second 0, the sum stays the ring's time */
    rd3_busy_submit(&B, RD3_BUSY_GFX, 9, 208, 10 * MS);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 10, 814, 11 * MS);
    CHECK(rd3_busy_done(&B, RD3_BUSY_GFX, 10, 15 * MS) == 2);
    rd3_busy_totals(&B, 20 * MS, &s);
    CHECK(s.ring_ns[RD3_BUSY_GFX] == 10 * MS && pid_total(208) + pid_total(814) == 10 * MS);
}

/* rings overlapping: the device is busy for the union, each ring for its own */
static void overlap(void)
{
    struct rd3_busy_snap s;
    rd3_busy_init(&B);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 1, 208, 0);
    rd3_busy_submit(&B, RD3_BUSY_SDMA, 1, 208, 2 * MS);
    rd3_busy_done(&B, RD3_BUSY_GFX, 1, 4 * MS);
    rd3_busy_done(&B, RD3_BUSY_SDMA, 1, 6 * MS);
    rd3_busy_submit(&B, RD3_BUSY_COMPUTE, 1, 99, 10 * MS);
    rd3_busy_done(&B, RD3_BUSY_COMPUTE, 1, 11 * MS);
    rd3_busy_totals(&B, 20 * MS, &s);
    CHECK(s.any_ns == 7 * MS);
    CHECK(s.ring_ns[RD3_BUSY_GFX] == 4 * MS && s.ring_ns[RD3_BUSY_SDMA] == 4 * MS &&
          s.ring_ns[RD3_BUSY_COMPUTE] == 1 * MS);
    CHECK(pid_total(208) == 8 * MS && pid_total(99) == 1 * MS);
}

/* the 32-bit fence wraps; a reset (fence_seq back to 0) flushes; a stale job flushes */
static void wrap_reset_stale(void)
{
    struct rd3_busy_snap s;
    rd3_busy_init(&B);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 0xffffffffu, 1, 0);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 0x00000001u, 1, 1 * MS);   /* after the wrap: queued, no flush */
    rd3_busy_totals(&B, 1 * MS, &s);
    CHECK(s.inflight == 2 && !s.flushed);
    CHECK(rd3_busy_done(&B, RD3_BUSY_GFX, 0xffffffffu, 2 * MS) == 1);
    CHECK(rd3_busy_done(&B, RD3_BUSY_GFX, 0x00000001u, 3 * MS) == 1);
    /* a reset: seq 1 again while 5 is in flight */
    rd3_busy_submit(&B, RD3_BUSY_GFX, 5, 1, 10 * MS);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 1, 2, 12 * MS);
    rd3_busy_totals(&B, 12 * MS, &s);
    CHECK(s.flushed == 1 && s.inflight == 1 && s.ring_ns[RD3_BUSY_GFX] == 3 * MS + 2 * MS);
    CHECK(rd3_busy_done(&B, RD3_BUSY_GFX, 1, 13 * MS) == 1);
    CHECK(pid_total(2) == 1 * MS);
    /* stale: a job never seen done for longer than RD3_BUSY_STALE_NS */
    rd3_busy_submit(&B, RD3_BUSY_SDMA, 40, 3, 20 * MS);
    CHECK(rd3_busy_stale(&B, 20 * MS + RD3_BUSY_STALE_NS) == 0);
    CHECK(rd3_busy_stale(&B, 21 * MS + RD3_BUSY_STALE_NS) == 1);
    rd3_busy_totals(&B, 22 * MS + RD3_BUSY_STALE_NS, &s);
    CHECK(s.stale == 1 && s.flushed == 2 && s.inflight == 0 && pid_total(3) == 0);
    /* an old fence read after the flush does nothing */
    CHECK(rd3_busy_done(&B, RD3_BUSY_SDMA, 40, 23 * MS + RD3_BUSY_STALE_NS) == 0);
}

/* a full queue merges into the newest job: the ring's time stays exact */
static void full(void)
{
    struct rd3_busy_snap s;
    uint32_t i;
    rd3_busy_init(&B);
    for (i = 1; i <= RD3_BUSY_FIFO + 10; i++)
        rd3_busy_submit(&B, RD3_BUSY_COMPUTE, i, 5, i * MS);
    rd3_busy_totals(&B, 100 * MS, &s);
    CHECK(s.inflight == RD3_BUSY_FIFO && s.merged == 10 && s.jobs == RD3_BUSY_FIFO + 10);
    CHECK(rd3_busy_done(&B, RD3_BUSY_COMPUTE, RD3_BUSY_FIFO + 9, 200 * MS) == RD3_BUSY_FIFO - 1);
    CHECK(rd3_busy_done(&B, RD3_BUSY_COMPUTE, RD3_BUSY_FIFO + 10, 300 * MS) == 1);   /* the merged entry */
    rd3_busy_totals(&B, 400 * MS, &s);
    CHECK(s.ring_ns[RD3_BUSY_COMPUTE] == 299 * MS && pid_total(5) == 299 * MS && !s.inflight);
}

/* takes: each process's time once, the slots reused, nothing lost while free
 * slots or idle ones exist */
static void takes(void)
{
    struct rd3_busy_take_ent t[RD3_BUSY_PIDS + 4];
    struct rd3_busy_snap s;
    uint32_t n, i;
    int32_t p;
    rd3_busy_init(&B);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 1, 208, 0);
    n = rd3_busy_take(&B, 1 * MS, t, 8);   /* submitted, nothing done: last_sub only */
    CHECK(n == 1 && t[0].pid == 208 && t[0].ns == 0 && t[0].jobs == 0 && t[0].last_sub == 0);
    CHECK(rd3_busy_take(&B, 2 * MS, t, 8) == 0);
    rd3_busy_done(&B, RD3_BUSY_GFX, 1, 6 * MS);
    n = rd3_busy_take(&B, 20 * MS, t, 8);
    CHECK(n == 1 && t[0].pid == 208 && t[0].ns == 6 * MS && t[0].jobs == 1);
    CHECK(rd3_busy_take(&B, 21 * MS, t, 8) == 0);
    /* 40 processes, one job each: 32 slots, the idle ones reused after a take */
    for (p = 1000; p < 1040; p++) {
        rd3_busy_submit(&B, RD3_BUSY_SDMA, (uint32_t)(p - 999) + 100, p, (uint64_t)p * MS);
        rd3_busy_done(&B, RD3_BUSY_SDMA, (uint32_t)(p - 999) + 100, (uint64_t)p * MS + MS);
        if (p == 1020) {
            n = rd3_busy_take(&B, 3000 * MS, t, RD3_BUSY_PIDS + 4);
            CHECK(n == 21);
        }
    }
    n = rd3_busy_take(&B, 5000 * MS, t, RD3_BUSY_PIDS + 4);
    rd3_busy_totals(&B, 2000 * MS, &s);
    CHECK(n == 19 && s.lost_ns == 0);
    for (i = 0; i < n; i++)
        CHECK(t[i].ns == MS && t[i].jobs == 1);
    /* every slot dirty: a new process's time is lost (counted), never someone else's taken */
    rd3_busy_init(&B);
    for (p = 1; p <= (int32_t)RD3_BUSY_PIDS; p++)
        rd3_busy_submit(&B, RD3_BUSY_GFX, (uint32_t)p, p, (uint64_t)p);
    rd3_busy_done(&B, RD3_BUSY_GFX, RD3_BUSY_PIDS, 50);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 100, 77, 100);
    rd3_busy_done(&B, RD3_BUSY_GFX, 100, 200);
    rd3_busy_totals(&B, 300, &s);
    CHECK(s.lost_jobs == 1 && s.lost_ns == 100 && pid_total(77) == 0);
    n = rd3_busy_take(&B, 1000, t, 4);   /* the rest wait for the next take */
    CHECK(n == 4 && rd3_busy_take(&B, 2000, t, RD3_BUSY_PIDS) == RD3_BUSY_PIDS - 4);
}

/* a process busy on two rings at once: at most the wall time per take, one span carried, the rest dropped
 * (Activity Monitor would show more than 100 %); a reset's force_completion ends every job with its time */
static void cap_and_reset(void)
{
    struct rd3_busy_take_ent t[4];
    struct rd3_busy_snap s;
    rd3_busy_init(&B);
    rd3_busy_submit(&B, RD3_BUSY_GFX, 1, 5, 0);
    rd3_busy_submit(&B, RD3_BUSY_SDMA, 1, 5, 0);
    rd3_busy_done(&B, RD3_BUSY_GFX, 1, 10 * MS);
    rd3_busy_done(&B, RD3_BUSY_SDMA, 1, 10 * MS);
    CHECK(pid_total(5) == 20 * MS);
    CHECK(rd3_busy_take(&B, 10 * MS, t, 4) == 1 && t[0].ns == 10 * MS && t[0].jobs == 2);
    CHECK(rd3_busy_take(&B, 12 * MS, t, 4) == 1 && t[0].ns == 2 * MS && t[0].jobs == 0);
    CHECK(rd3_busy_take(&B, 20 * MS, t, 4) == 1 && t[0].ns == 2 * MS);
    CHECK(rd3_busy_take(&B, 30 * MS, t, 4) == 0);
    rd3_busy_totals(&B, 30 * MS, &s);
    CHECK(s.capped_ns == 6 * MS);
    /* force_completion: the three jobs end at the reset, before fence_seq starts again at 0 */
    rd3_busy_submit(&B, RD3_BUSY_COMPUTE, 900, 7, 40 * MS);
    rd3_busy_submit(&B, RD3_BUSY_COMPUTE, 901, 8, 41 * MS);
    rd3_busy_submit(&B, RD3_BUSY_COMPUTE, 902, 7, 42 * MS);
    CHECK(rd3_busy_end_all(&B, RD3_BUSY_COMPUTE, 50 * MS) == 3);
    CHECK(rd3_busy_end_all(&B, RD3_BUSY_COMPUTE, 51 * MS) == 0);
    rd3_busy_submit(&B, RD3_BUSY_COMPUTE, 1, 7, 60 * MS);   /* seq 1 again: no flush, the ring was empty */
    rd3_busy_totals(&B, 60 * MS, &s);
    CHECK(pid_total(7) == 10 * MS && pid_total(8) == 0 && s.flushed == 0 && s.inflight == 1 &&
          s.ring_ns[RD3_BUSY_COMPUTE] == 10 * MS && s.ring_flushed[RD3_BUSY_COMPUTE] == 0);
    rd3_busy_submit(&B, RD3_BUSY_COMPUTE, 1, 7, 61 * MS);   /* not after: a reset nobody told, flushed */
    rd3_busy_totals(&B, 62 * MS, &s);
    CHECK(s.flushed == 1 && s.ring_flushed[RD3_BUSY_COMPUTE] == 1 && s.inflight == 1);
}

/* the utilisation window and the creator string */
static void util(void)
{
    struct rd3_util_win w;
    int valid = 7;
    int32_t pid = -1;
    memset(&w, 0, sizeof w);
    CHECK(rd3_util_feed(&w, 0, 1000, &valid) == 0 && !valid);                     /* primes */
    CHECK(rd3_util_feed(&w, 250, 2000, &valid) == 25 && valid);
    CHECK(rd3_util_feed(&w, 1250, 3000, &valid) == 63 && valid);                  /* (250 + 1000) / 2000 */
    CHECK(rd3_util_feed(&w, 1250, 4000, &valid) == 42);
    CHECK(rd3_util_feed(&w, 1250, 5000, &valid) == 31);
    CHECK(rd3_util_feed(&w, 1250, 6000, &valid) == 25);                           /* the 250 left the window */
    CHECK(rd3_util_feed(&w, 1250, 7000, &valid) == 0);
    CHECK(rd3_util_feed(&w, 9999, 8000, &valid) == 100);                          /* clamped */
    CHECK(rd3_util_feed(&w, 10, 9000, &valid) == 0 && !valid);                    /* went back: primes again */
    CHECK(rd3_util_feed(&w, 20, 9000, &valid) == 0 && !valid);                    /* no time passed */
    CHECK(rd3_util_feed(&w, 520, 10000, &valid) == 50 && valid);
    CHECK(rd3_ucc_pid("pid 814, Chess", &pid) && pid == 814);
    CHECK(rd3_ucc_pid("pid 208, WindowServer", &pid) && pid == 208);
    pid = -1;
    CHECK(!rd3_ucc_pid("pid , x", &pid) && !rd3_ucc_pid("pid 12 x", &pid) && !rd3_ucc_pid("PID 1, x", &pid));
    CHECK(!rd3_ucc_pid("pid 99999999999, x", &pid) && !rd3_ucc_pid(0, &pid) && pid == -1);
}

int main(void)
{
    single();
    queued();
    overlap();
    wrap_reset_stale();
    full();
    takes();
    cap_and_reset();
    util();
    if (fails) {
        printf("test_gpu_busy: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_gpu_busy: all checks passed\n");
    return 0;
}
