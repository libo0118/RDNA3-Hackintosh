/*
 * rdna3_gpu_busy.h - how long the GPU's kernel rings had work in flight, in
 * total (the device's utilisation) and per submitting process (its GPU
 * time). Plain C, no kernel headers: the core kext (RDNA3Bringup) keeps one
 * under a spinlock, tests/test_gpu_busy.c checks it on the host.
 *
 * Port of amdgpu's per-client engine time (amdgpu_ctx_fence_time,
 * amdgpu_ctx_entity_time, amdgpu_ctx_mgr_usage; amdgpu_show_fdinfo prints
 * it as drm-engine-<ip>): a job's time is its fence's finished timestamp
 * minus its scheduled timestamp (drm_sched_fence). amdgpu's scheduled stamp
 * is drm_sched's run_job, the moment the job is written to the ring; ours
 * is the submission, which writes the ring directly (no scheduler). The
 * finished stamp is amdgpu_fence_process's: when the driver reads the
 * ring's fence value (amdgpu_fence_read) past the job's seq - from the EOP
 * interrupt, a waiter, or the accelerator's 250 ms take.
 *
 * One refinement over amdgpu: a job queued behind another is counted by
 * amdgpu from its run_job, so two jobs in flight on one ring both count the
 * same time. A ring executes in order, so here a job's time starts at the
 * later of its submission and the previous job's end on the same ring. The
 * per-job times of a ring then add up to the time the ring had work, and
 * that time - per ring, and the union over the rings - is the device's busy
 * time (Activity Monitor's "Device Utilization %").
 *
 * Why not the SMU: amdgpu's gpu_busy_percent is the PMFW's
 * METRICS_AVERAGE_GFXACTIVITY (smu_v13_0_0_get_smu_metrics_data ->
 * AverageGfxActivity; smu_v13_0_7 shares the 13.0.0 metrics table), which
 * hw/smu13.c reads as well. On this card it reads 100 % at idle in every
 * card run ("PM: activity 100%", DRIVER-PLAN P2): unusable until P2 is
 * understood.
 *
 * Seqs are the rings' 32-bit fence values (gfx11 / sdma60 fence_seq),
 * compared wrap-safe. A submission whose seq is not after the newest one
 * queued means the ring was reset (fence_seq back to 0): the ring's queue is
 * flushed (counted, its jobs get no time). A job older than
 * RD3_BUSY_STALE_NS is flushed the same way (a reset that left the fence
 * value behind). A full queue merges the new job into the newest entry
 * (counted): the ring's busy time stays exact, the job's time goes to that
 * entry's process. A reset that completes the fences (amdgpu's
 * force_completion) ends every job of the ring at once with its time
 * (rd3_busy_end_all), before the ring's fence_seq starts again at 0.
 *
 * A process's time summed over several rings can be more than the wall
 * time (GFX and SDMA at once); Activity Monitor would show more than 100 %.
 * rd3_busy_take hands out at most the wall time since the last take per
 * process and carries the rest over, at most one more such span (the
 * excess is dropped, counted in capped_ns).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_GPU_BUSY_H
#define RDNA3_GPU_BUSY_H

#include <stdint.h>
#include <string.h>

#define RD3_BUSY_GFX     0u
#define RD3_BUSY_COMPUTE 1u
#define RD3_BUSY_SDMA    2u
#define RD3_BUSY_RINGS   3u
#define RD3_BUSY_FIFO    64u                       /* jobs in flight per ring */
#define RD3_BUSY_PIDS    32u                       /* processes with time not yet taken */
#define RD3_BUSY_STALE_NS (30ull * 1000000000ull)  /* 3 x amdgpu's 10 s job timeout */

struct rd3_busy_job {
    uint32_t seq;
    int32_t  pid;
    uint64_t sub_ns;          /* submitted */
};

struct rd3_busy_ring {
    struct rd3_busy_job q[RD3_BUSY_FIFO];
    uint32_t head, n;         /* the oldest job at q[head], n in flight */
    uint64_t busy_since;      /* n > 0: since when the ring has work */
    uint64_t busy_ns;         /* closed busy intervals */
    uint64_t last_end;        /* the newest completion seen */
    uint64_t flushed;         /* jobs flushed with no time on this ring */
};

struct rd3_busy_pid {
    int32_t  pid;
    uint32_t used, dirty;     /* dirty: something to take */
    uint32_t pend_jobs;
    uint64_t pend_ns;         /* GPU time not yet taken */
    uint64_t total_ns;
    uint64_t last_sub;        /* the newest submission (ns) */
};

struct rd3_busy {
    struct rd3_busy_ring ring[RD3_BUSY_RINGS];
    struct rd3_busy_pid  pid[RD3_BUSY_PIDS];
    uint32_t nbusy;           /* rings with work */
    uint64_t any_since, any_ns;
    uint64_t jobs, merged, flushed, stale, lost_jobs, lost_ns, capped_ns;
    uint64_t take_last;       /* the previous take's time */
};

/* what a take returns: totals since init, the busy time up to 'now' */
struct rd3_busy_snap {
    uint64_t now;
    uint64_t any_ns;
    uint64_t ring_ns[RD3_BUSY_RINGS];
    uint64_t ring_flushed[RD3_BUSY_RINGS];
    uint64_t jobs, merged, flushed, stale, lost_jobs, lost_ns, capped_ns;
    uint32_t inflight;
};

struct rd3_busy_take_ent {
    int32_t  pid;
    uint32_t jobs;
    uint64_t ns;              /* GPU time since the last take */
    uint64_t last_sub;        /* the newest submission (ns) */
};

static inline void rd3_busy_init(struct rd3_busy *b)
{
    memset(b, 0, sizeof(*b));
}

static inline int rd3_busy_seq_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static inline uint64_t rd3_busy_since(uint64_t now, uint64_t t)
{
    return now > t ? now - t : 0;
}

static inline void rd3_busy_open_(struct rd3_busy *b, struct rd3_busy_ring *r, uint64_t now)
{
    r->busy_since = now;
    if (b->nbusy++ == 0)
        b->any_since = now;
}

static inline void rd3_busy_close_(struct rd3_busy *b, struct rd3_busy_ring *r, uint64_t now)
{
    r->busy_ns += rd3_busy_since(now, r->busy_since);
    if (b->nbusy && --b->nbusy == 0)
        b->any_ns += rd3_busy_since(now, b->any_since);
}

/* the process's slot; create: take a free one, else the least recently
 * submitting one that has nothing to take; NULL when none */
static inline struct rd3_busy_pid *rd3_busy_pid_slot(struct rd3_busy *b, int32_t pid, int create)
{
    struct rd3_busy_pid *freep = 0, *old = 0;
    uint32_t i;
    for (i = 0; i < RD3_BUSY_PIDS; i++) {
        struct rd3_busy_pid *p = &b->pid[i];
        if (p->used && p->pid == pid)
            return p;
        if (!p->used) {
            if (!freep)
                freep = p;
        } else if (!p->dirty && (!old || p->last_sub < old->last_sub)) {
            old = p;
        }
    }
    if (!create)
        return 0;
    if (!freep)
        freep = old;
    if (!freep)
        return 0;
    memset(freep, 0, sizeof(*freep));
    freep->used = 1;
    freep->pid = pid;
    return freep;
}

static inline void rd3_busy_charge_(struct rd3_busy *b, int32_t pid, uint64_t ns)
{
    struct rd3_busy_pid *p = rd3_busy_pid_slot(b, pid, 1);
    if (!p) {
        b->lost_jobs++;
        b->lost_ns += ns;
        return;
    }
    p->pend_ns += ns;
    p->total_ns += ns;
    p->pend_jobs++;
    p->dirty = 1;
}

/* the ring's jobs dropped with no time (reset, stale): its busy interval
 * ends now */
static inline void rd3_busy_flush(struct rd3_busy *b, uint32_t ring, uint64_t now)
{
    struct rd3_busy_ring *r;
    if (ring >= RD3_BUSY_RINGS)
        return;
    r = &b->ring[ring];
    if (!r->n)
        return;
    b->flushed += r->n;
    r->flushed += r->n;
    r->n = 0;
    r->head = 0;
    r->last_end = now;
    rd3_busy_close_(b, r, now);
}

/* a job written to the ring with fence 'seq' by 'pid' */
static inline void rd3_busy_submit(struct rd3_busy *b, uint32_t ring, uint32_t seq, int32_t pid, uint64_t now)
{
    struct rd3_busy_ring *r;
    struct rd3_busy_pid *p;
    if (ring >= RD3_BUSY_RINGS)
        return;
    r = &b->ring[ring];
    if (r->n && !rd3_busy_seq_after(seq, r->q[(r->head + r->n - 1) % RD3_BUSY_FIFO].seq))
        rd3_busy_flush(b, ring, now);   /* the ring's fence_seq went back: a reset */
    p = rd3_busy_pid_slot(b, pid, 1);
    if (p) {
        p->last_sub = now;
        p->dirty = 1;
    }
    b->jobs++;
    if (r->n == RD3_BUSY_FIFO) {
        r->q[(r->head + r->n - 1) % RD3_BUSY_FIFO].seq = seq;   /* merged into the newest */
        b->merged++;
        return;
    }
    if (!r->n)
        rd3_busy_open_(b, r, now);
    r->q[(r->head + r->n) % RD3_BUSY_FIFO].seq = seq;
    r->q[(r->head + r->n) % RD3_BUSY_FIFO].pid = pid;
    r->q[(r->head + r->n) % RD3_BUSY_FIFO].sub_ns = now;
    r->n++;
}

/* the ring's fence value read at 'now' (amdgpu_fence_process): every job up
 * to it is done; returns how many ended */
static inline uint32_t rd3_busy_done(struct rd3_busy *b, uint32_t ring, uint32_t done_seq, uint64_t now)
{
    struct rd3_busy_ring *r;
    uint32_t ended = 0;
    if (ring >= RD3_BUSY_RINGS)
        return 0;
    r = &b->ring[ring];
    while (r->n && !rd3_busy_seq_after(r->q[r->head].seq, done_seq)) {
        const struct rd3_busy_job *j = &r->q[r->head];
        const uint64_t start = j->sub_ns > r->last_end ? j->sub_ns : r->last_end;
        rd3_busy_charge_(b, j->pid, rd3_busy_since(now, start));
        if (now > r->last_end)
            r->last_end = now;
        r->head = (r->head + 1) % RD3_BUSY_FIFO;
        r->n--;
        ended++;
        if (!r->n)
            rd3_busy_close_(b, r, now);
    }
    return ended;
}

/* a reset completed the ring's fences (force_completion): every job in
 * flight ends now, with its time; returns how many */
static inline uint32_t rd3_busy_end_all(struct rd3_busy *b, uint32_t ring, uint64_t now)
{
    const struct rd3_busy_ring *r;
    if (ring >= RD3_BUSY_RINGS)
        return 0;
    r = &b->ring[ring];
    if (!r->n)
        return 0;
    return rd3_busy_done(b, ring, r->q[(r->head + r->n - 1) % RD3_BUSY_FIFO].seq, now);
}

/* the oldest job of each ring older than RD3_BUSY_STALE_NS: the ring
 * flushed (a reset that left the fence value behind); returns the rings */
static inline uint32_t rd3_busy_stale(struct rd3_busy *b, uint64_t now)
{
    uint32_t i, n = 0;
    for (i = 0; i < RD3_BUSY_RINGS; i++) {
        struct rd3_busy_ring *r = &b->ring[i];
        if (r->n && rd3_busy_since(now, r->q[r->head].sub_ns) > RD3_BUSY_STALE_NS) {
            b->stale += r->n;
            rd3_busy_flush(b, i, now);
            n++;
        }
    }
    return n;
}

static inline void rd3_busy_totals(const struct rd3_busy *b, uint64_t now, struct rd3_busy_snap *s)
{
    uint32_t i;
    memset(s, 0, sizeof(*s));
    s->now = now;
    s->any_ns = b->any_ns + (b->nbusy ? rd3_busy_since(now, b->any_since) : 0);
    for (i = 0; i < RD3_BUSY_RINGS; i++) {
        const struct rd3_busy_ring *r = &b->ring[i];
        s->ring_ns[i] = r->busy_ns + (r->n ? rd3_busy_since(now, r->busy_since) : 0);
        s->inflight += r->n;
        s->ring_flushed[i] = r->flushed;
    }
    s->jobs = b->jobs;
    s->merged = b->merged;
    s->flushed = b->flushed;
    s->stale = b->stale;
    s->lost_jobs = b->lost_jobs;
    s->lost_ns = b->lost_ns;
    s->capped_ns = b->capped_ns;
}

/* the processes with time or a submission since the last take, at most
 * 'max' (the rest wait for the next take); each gets at most the wall time
 * since the last take, the rest carried over (at most one more such span);
 * returns how many */
static inline uint32_t rd3_busy_take(struct rd3_busy *b, uint64_t now, struct rd3_busy_take_ent *out, uint32_t max)
{
    const uint64_t span = rd3_busy_since(now, b->take_last);
    uint32_t i, n = 0;
    for (i = 0; i < RD3_BUSY_PIDS && n < max; i++) {
        struct rd3_busy_pid *p = &b->pid[i];
        uint64_t ns;
        if (!p->used || !p->dirty)
            continue;
        ns = p->pend_ns < span ? p->pend_ns : span;
        out[n].pid = p->pid;
        out[n].jobs = p->pend_jobs;
        out[n].ns = ns;
        out[n].last_sub = p->last_sub;
        n++;
        p->pend_ns -= ns;
        if (p->pend_ns > span) {
            b->capped_ns += p->pend_ns - span;
            p->pend_ns = span;
        }
        p->pend_jobs = 0;
        p->dirty = p->pend_ns != 0;
    }
    b->take_last = now;
    return n;
}

#endif /* RDNA3_GPU_BUSY_H */
