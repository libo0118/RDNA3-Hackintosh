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
 * the count is 0. A wait that runs out clears the surface (counted as a
 * timeout by the caller): a mark whose ready never comes (a crashed
 * WindowServer, a lost completion) costs one bounded wait, not one per
 * page-off. The marks a timeout dropped are kept as "forgiven" in the
 * surface's slot (count 0): the late readies that still come take a
 * forgiven one first and are answered LATE, so a late ready never takes the
 * count of a newer command buffer's pending mark (ECC review of N41, open
 * item 4). A ready that finds forgiven marks while newer marks are pending
 * also takes the forgiven one: the newer write's own ready then ends the
 * wait (later, never earlier; the bound covers a ready that never comes).
 * A table that is full first reuses a slot that holds only forgiven marks
 * (the oldest), then drops the surface pending the longest (its waiters
 * stop waiting: never longer than the bound; its marks are not kept, its
 * late readies find no slot and are LATE). A ready for a surface the table
 * does not hold is counted as late, nothing else.
 *
 * A waiter whose surface was cleared under it (another waiter's timeout,
 * an eviction, the waits switched off) is told so (RD3_SPW_CLEARED) rather
 * than READY: its write did not finish, the caller does not count it as a
 * successful wait. The table remembers the last RD3_SP_CLEARED clears for
 * that (surface id, pending episode).
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
#define RD3_SP_CLEARED   (2u * RD3_SP_SLOTS) /* clears remembered for the waiters (RD3_SPW_CLEARED): a whole clear() fits */

struct rd3_sp_slot {
    uint32_t id;          /* the IOSurface id (0: free) */
    uint32_t count;       /* pending marks */
    uint32_t forgiven;    /* marks a timeout dropped whose readies have not come yet (late) */
    uint64_t seq;         /* the pending episode: set when count goes 0 -> 1 (the oldest goes when full) */
    uint64_t since_ns;    /* the episode's first pending mark */
};

struct rd3_sp_clear_rec {
    uint32_t id;
    uint64_t seq;         /* the episode that was cleared */
};

struct rd3_sp {
    struct rd3_sp_slot s[RD3_SP_SLOTS];
    uint64_t seq;
    struct rd3_sp_clear_rec cleared[RD3_SP_CLEARED];
    unsigned cleared_next;
};

enum rd3_sp_mark_res {
    RD3_SPM_PENDING = 0,    /* counted */
    RD3_SPM_EVICTED = 1,    /* counted; the table was full: *evicted lost its marks */
    RD3_SPM_SATURATED = 2,  /* the surface already holds RD3_SP_MAX_COUNT marks: not counted */
    RD3_SPM_STILL = 3,      /* ready: one mark taken, others still pending */
    RD3_SPM_READY = 4,      /* ready: the last mark taken, wake the waiters */
    RD3_SPM_LATE = 5,       /* ready of a mark a timeout dropped (forgiven), or of a surface not held */
    RD3_SPM_BAD = 6         /* id 0 or no table */
};

enum rd3_sp_wait_res {
    RD3_SPW_READY = 0,      /* not pending (or the waited episode ended with its ready): go on */
    RD3_SPW_WAIT = 1,       /* pending, the deadline not reached: sleep until it or a ready */
    RD3_SPW_TIMEOUT = 2,    /* pending at the deadline: the surface was cleared by this waiter */
    RD3_SPW_CLEARED = 3     /* the episode this waiter waited for was cleared by someone else (not ready) */
};

static inline struct rd3_sp_slot *rd3_sp_find(struct rd3_sp *t, uint32_t id)
{
    for (unsigned i = 0; i < RD3_SP_SLOTS; i++)
        if (t->s[i].id == id) return &t->s[i];
    return 0;
}

static inline void rd3_sp_slot_free(struct rd3_sp_slot *s)
{
    s->id = 0; s->count = 0; s->forgiven = 0; s->seq = 0; s->since_ns = 0;
}

/* an episode of surface 'id' ended without its ready: its waiters are told */
static inline void rd3_sp_note_cleared(struct rd3_sp *t, uint32_t id, uint64_t seq)
{
    struct rd3_sp_clear_rec *c = &t->cleared[t->cleared_next++ % RD3_SP_CLEARED];
    c->id = id; c->seq = seq;
}

static inline int rd3_sp_was_cleared(const struct rd3_sp *t, uint32_t id, uint64_t seq)
{
    for (unsigned i = 0; seq && i < RD3_SP_CLEARED; i++)
        if (t->cleared[i].id == id && t->cleared[i].seq == seq) return 1;
    return 0;
}

/* a slot for a new surface: a free one, else the oldest holding only forgiven marks, else the oldest
 * pending one (*evicted: its id; its episode is noted cleared) */
static inline struct rd3_sp_slot *rd3_sp_take_slot(struct rd3_sp *t, uint32_t *evicted)
{
    struct rd3_sp_slot *s = rd3_sp_find(t, 0), *fg = 0, *old = 0;
    if (s) return s;
    for (unsigned i = 0; i < RD3_SP_SLOTS; i++) {
        struct rd3_sp_slot *c = &t->s[i];
        if (!c->count) { if (!fg || c->seq < fg->seq) fg = c; }
        else if (!old || c->seq < old->seq) old = c;
    }
    if (fg) return fg;
    if (evicted) *evicted = old->id;
    rd3_sp_note_cleared(t, old->id, old->seq);
    return old;
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
        if (s->forgiven) {   /* a ready of a mark a timeout dropped: never a newer mark's count */
            s->forgiven--;
            if (!s->count && !s->forgiven) rd3_sp_slot_free(s);
            return RD3_SPM_LATE;
        }
        if (!s->count) { rd3_sp_slot_free(s); return RD3_SPM_LATE; }   /* not reached: such a slot is free */
        if (--s->count) return RD3_SPM_STILL;
        rd3_sp_slot_free(s);
        return RD3_SPM_READY;
    }
    if (s) {
        if (s->count >= RD3_SP_MAX_COUNT) return RD3_SPM_SATURATED;
        if (!s->count) { s->seq = ++t->seq; s->since_ns = now_ns; }   /* a new episode (forgiven marks kept) */
        s->count++;
        return RD3_SPM_PENDING;
    }
    uint32_t ev = 0;
    s = rd3_sp_take_slot(t, &ev);
    if (evicted) *evicted = ev;
    rd3_sp_slot_free(s);
    s->id = id; s->count = 1; s->seq = ++t->seq; s->since_ns = now_ns;
    return ev ? RD3_SPM_EVICTED : RD3_SPM_PENDING;
}

/* the marks surface 'id' holds (0: ready) */
static inline uint32_t rd3_sp_pending(struct rd3_sp *t, uint32_t id)
{
    if (!t || !id) return 0;
    const struct rd3_sp_slot *s = rd3_sp_find(t, id);
    return s ? s->count : 0;
}

/* the forgiven marks of surface 'id' (readies still to come late) */
static inline uint32_t rd3_sp_forgiven(struct rd3_sp *t, uint32_t id)
{
    if (!t || !id) return 0;
    const struct rd3_sp_slot *s = rd3_sp_find(t, id);
    return s ? s->forgiven : 0;
}

/* one step of a bounded wait for surface 'id': READY, WAIT (sleep until
 * deadline_ns or a ready), TIMEOUT (pending at the deadline: cleared by this
 * call, *dropped the marks it held, now forgiven), CLEARED (the episode this
 * waiter saw pending was cleared by another waiter, an eviction or a clear).
 * *epoch (0 before the first step; may be 0 for no CLEARED answers) is the
 * waiter's episode, set at its first WAIT. */
static inline enum rd3_sp_wait_res rd3_sp_wait_step(struct rd3_sp *t, uint32_t id, uint64_t now_ns,
                                                    uint64_t deadline_ns, uint32_t *dropped, uint64_t *epoch)
{
    if (dropped) *dropped = 0;
    if (!t || !id) return RD3_SPW_READY;
    struct rd3_sp_slot *s = rd3_sp_find(t, id);
    const uint64_t mine = epoch ? *epoch : 0;
    if (!s || !s->count || (mine && s->seq != mine))   /* the waited episode ended: by its ready, or cleared */
        return rd3_sp_was_cleared(t, id, mine) ? RD3_SPW_CLEARED : RD3_SPW_READY;
    if (epoch && !mine) *epoch = s->seq;
    if (now_ns < deadline_ns) return RD3_SPW_WAIT;
    if (dropped) *dropped = s->count;
    const uint64_t fg = (uint64_t)s->forgiven + s->count;
    s->forgiven = fg > RD3_SP_MAX_COUNT ? RD3_SP_MAX_COUNT : (uint32_t)fg;
    rd3_sp_note_cleared(t, id, s->seq);
    s->count = 0; s->since_ns = 0;
    return RD3_SPW_TIMEOUT;
}

/* surfaces pending now (the sysctl's gauge) */
static inline unsigned rd3_sp_busy(const struct rd3_sp *t)
{
    unsigned n = 0;
    for (unsigned i = 0; t && i < RD3_SP_SLOTS; i++)
        if (t->s[i].id && t->s[i].count) n++;
    return n;
}

/* every mark goes (the waits switched off: none may hold a page-off later); the
 * surfaces that were pending (their waiters: CLEARED) */
static inline unsigned rd3_sp_clear(struct rd3_sp *t)
{
    unsigned n = 0;
    for (unsigned i = 0; t && i < RD3_SP_SLOTS; i++) {
        if (t->s[i].id && t->s[i].count) { n++; rd3_sp_note_cleared(t, t->s[i].id, t->s[i].seq); }
        rd3_sp_slot_free(&t->s[i]);
    }
    return n;
}

/* the plane word of RD3K_ESC_SURF_DIRTY carries the GPU marks (accel/rdna3k_abi.h
 * RD3K_SS_PLANE_*): decoded here so the host test checks the same bits */
#define RD3_SP_PLANE_GPU_MARK 0x80000000u   /* a pending/ready mark, not a dirty mark: bit 0 = pending */
#define RD3_SP_PLANE_TRYLOCK  0x40000000u   /* a dirty mark that gives up when the accelerator lock is taken */
#define RD3_SP_PLANE_FLAGS    (RD3_SP_PLANE_GPU_MARK | RD3_SP_PLANE_TRYLOCK)

#endif
