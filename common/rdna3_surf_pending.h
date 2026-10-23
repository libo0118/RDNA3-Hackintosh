/*
 * rdna3_surf_pending.h - which IOSurfaces WindowServer's GPU is still
 * writing (DRIVER-PLAN N41). Plain C, no kernel headers: the accelerator
 * uses it and tests/test_surf_pending.c checks it on the host.
 *
 * Evidence-driven, no amdgpu analogue (macOS IOSurfaceLock semantics). A
 * normal IOSurfaceLock waits for GPU work still in flight on the surface
 * (only kIOSurfaceLockAvoidSync skips it) and WindowServer relies on that:
 * a window capture locks a surface it is still drawing. Our Metal plugin
 * marks a surface's VRAM newest only after its GPU work completed, so a
 * lock that comes first finds the system copy current and reads stale or
 * zero bytes: black window captures, stale panes. The NullMoth NVIDIA
 * driver (79f47d0, 40daaa6, e75be1e) has WindowServer mark the surfaces a
 * command buffer writes "pending" at commit and "ready" after the GPU
 * finished the last part writing each, and the page-off waits, bounded,
 * for a surface's pending marks to clear. This is that table, written for
 * this driver.
 *
 * Marks are counted per surface: two command buffers in flight on the same
 * surface each add one, each ready takes one, the surface is ready when
 * the count is 0. A table that is full drops its oldest surface (its
 * waiters stop waiting: never longer than the bound). A wait that runs
 * out clears the surface (counted as a timeout by the caller): a mark
 * whose ready never comes (a crashed WindowServer, a lost completion)
 * costs one bounded wait, not one per page-off. A ready for a surface
 * the table does not hold (its wait timed out, or it was dropped) is
 * counted as late, nothing else.
 *
 * Locking is the caller's (an IOLock in the kernel: the waiter sleeps on
 * it, the ready mark wakes it). Times are the caller's clock in ns.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_SURF_PENDING_H
#define RDNA3_SURF_PENDING_H

#include <stdint.h>

#define RD3_SP_SLOTS     64u
#define RD3_SP_MAX_COUNT 0xffffu          /* marks one surface may hold; more are refused (saturated) */
#define RD3_SP_WAIT_MS   100u             /* the page-off's bound: a GPU that has not finished a composite in 100 ms is hung */
#define RD3_SP_WAIT_MAX_MS 1000u          /* the sysctl's ceiling */

struct rd3_sp_slot {
    uint32_t id;          /* the IOSurface id (0: free) */
    uint32_t count;       /* pending marks */
    uint64_t seq;         /* when the surface became pending (the oldest goes when full) */
    uint64_t since_ns;    /* the first pending mark */
};

struct rd3_sp {
    struct rd3_sp_slot s[RD3_SP_SLOTS];
    uint64_t seq;
};

enum rd3_sp_mark_res {
    RD3_SPM_PENDING = 0,    /* counted */
    RD3_SPM_EVICTED = 1,    /* counted; the table was full: *evicted lost its marks */
    RD3_SPM_SATURATED = 2,  /* the surface already holds RD3_SP_MAX_COUNT marks: not counted */
    RD3_SPM_STILL = 3,      /* ready: one mark taken, others still pending */
    RD3_SPM_READY = 4,      /* ready: the last mark taken, wake the waiters */
    RD3_SPM_LATE = 5,       /* ready for a surface not pending (timed out, dropped, never marked) */
    RD3_SPM_BAD = 6         /* id 0 or no table */
};

enum rd3_sp_wait_res {
    RD3_SPW_READY = 0,      /* not pending: go on */
    RD3_SPW_WAIT = 1,       /* pending, the deadline not reached: sleep until it or a ready */
    RD3_SPW_TIMEOUT = 2     /* pending at the deadline: the surface was cleared */
};

static inline struct rd3_sp_slot *rd3_sp_find(struct rd3_sp *t, uint32_t id)
{
    for (unsigned i = 0; i < RD3_SP_SLOTS; i++)
        if (t->s[i].id == id) return &t->s[i];
    return 0;
}

/* a pending (pending != 0) or ready mark of surface 'id' at 'now_ns' */
static inline enum rd3_sp_mark_res rd3_sp_mark(struct rd3_sp *t, uint32_t id, int pending, uint64_t now_ns,
                                               uint32_t *evicted)
{
    if (evicted) *evicted = 0;
    if (!t || !id) return RD3_SPM_BAD;
    struct rd3_sp_slot *s = rd3_sp_find(t, id);
    if (!pending) {
        if (!s) return RD3_SPM_LATE;
        if (--s->count) return RD3_SPM_STILL;
        s->id = 0; s->seq = 0; s->since_ns = 0;
        return RD3_SPM_READY;
    }
    if (s) {
        if (s->count >= RD3_SP_MAX_COUNT) return RD3_SPM_SATURATED;
        s->count++;
        return RD3_SPM_PENDING;
    }
    enum rd3_sp_mark_res r = RD3_SPM_PENDING;
    s = rd3_sp_find(t, 0);
    if (!s) {   /* full: the surface pending the longest goes */
        s = &t->s[0];
        for (unsigned i = 1; i < RD3_SP_SLOTS; i++)
            if (t->s[i].seq < s->seq) s = &t->s[i];
        if (evicted) *evicted = s->id;
        r = RD3_SPM_EVICTED;
    }
    s->id = id; s->count = 1; s->seq = ++t->seq; s->since_ns = now_ns;
    return r;
}

/* the marks surface 'id' holds (0: ready) */
static inline uint32_t rd3_sp_pending(struct rd3_sp *t, uint32_t id)
{
    if (!t || !id) return 0;
    const struct rd3_sp_slot *s = rd3_sp_find(t, id);
    return s ? s->count : 0;
}

/* one step of a bounded wait for surface 'id': READY, WAIT (sleep until
 * deadline_ns or a ready), TIMEOUT (pending at the deadline: cleared,
 * *dropped the marks it held). */
static inline enum rd3_sp_wait_res rd3_sp_wait_step(struct rd3_sp *t, uint32_t id, uint64_t now_ns,
                                                    uint64_t deadline_ns, uint32_t *dropped)
{
    if (dropped) *dropped = 0;
    if (!t || !id) return RD3_SPW_READY;
    struct rd3_sp_slot *s = rd3_sp_find(t, id);
    if (!s) return RD3_SPW_READY;
    if (now_ns < deadline_ns) return RD3_SPW_WAIT;
    if (dropped) *dropped = s->count;
    s->id = 0; s->count = 0; s->seq = 0; s->since_ns = 0;
    return RD3_SPW_TIMEOUT;
}

/* surfaces pending now (the sysctl's gauge) */
static inline unsigned rd3_sp_busy(const struct rd3_sp *t)
{
    unsigned n = 0;
    for (unsigned i = 0; t && i < RD3_SP_SLOTS; i++)
        if (t->s[i].id) n++;
    return n;
}

/* every mark goes (the waits switched off: none may hold a page-off later) */
static inline unsigned rd3_sp_clear(struct rd3_sp *t)
{
    unsigned n = 0;
    for (unsigned i = 0; t && i < RD3_SP_SLOTS; i++)
        if (t->s[i].id) { n++; t->s[i].id = 0; t->s[i].count = 0; t->s[i].seq = 0; t->s[i].since_ns = 0; }
    return n;
}

/* the plane word of RD3K_ESC_SURF_DIRTY carries the GPU marks (accel/rdna3k_abi.h
 * RD3K_SS_PLANE_*): decoded here so the host test checks the same bits */
#define RD3_SP_PLANE_GPU_MARK 0x80000000u   /* a pending/ready mark, not a dirty mark: bit 0 = pending */
#define RD3_SP_PLANE_TRYLOCK  0x40000000u   /* a dirty mark that gives up when the accelerator lock is taken */
#define RD3_SP_PLANE_FLAGS    (RD3_SP_PLANE_GPU_MARK | RD3_SP_PLANE_TRYLOCK)

#endif
