/*
 * rdna3_iop_stale.h - a display-pipe (IOP) copy of a surface nothing wrote since the head last showed it
 * (DRIVER-PLAN N59). Plain C, no kernel headers: accel/rdna3-accel.cpp keeps one table per head under a
 * spinlock, tests/test_iop_stale.c checks it on the host.
 *
 * Card run 2026-10-26 (app survey): continuous animations step BACK to an earlier frame and go on (frames
 * shown 1,2,3,1,4,2,5,...). WindowServer composites each display into a small ring of IOPresentment
 * buffers (the plugin trace of that run: surfaces 24 / 34 / 36 take one full-screen pass per frame in turn)
 * and sends each to RD3DisplayPipe::performTransaction, which copies the buffer's system memory to the
 * scanout (rdna3acceliop=1; WindowServer's display surfaces are kept out of A3, so the Metal plugin draws
 * them into a private image and copies the pixels into the IOSurface with the CPU AFTER the GPU finished:
 * -nvmtlSurfaceOut, under an IOSurfaceLock that moves the surface's seed). Nothing orders that copy-out
 * before the transaction: a transaction that arrives first copies what the buffer held the last time it was
 * shown, a frame two or three back - exactly the owner's step back.
 *
 * The kernel sees it without the plugin: a buffer of the ring is presented because WindowServer drew a new
 * frame into it, so its IOSurface's write seeds (IOSurface::getLockSeed / getUnlockSeed) must have moved
 * since this head last copied it. Seeds that did not move = nobody wrote it since = the panel is about to
 * get an old frame (STALE). The display pipe then waits, bounded, for the seeds to move (the copy-out that
 * is still on its way; amdgpu's equivalent is the implicit sync of a scanout BO: amdgpu_dm_commit_planes
 * waits for the BO's reservation fences, dma_resv_wait_timeout, before it flips to it).
 *
 * A legitimate re-present of an unchanged buffer would wait in vain every time: RD3_IS_BUDGET timeouts in a
 * row with no wait that saw the seeds move stop the waits (counted as skipped); every RD3_IS_PROBE-th
 * stale present after that waits again, and one that sees the seeds move re-arms the waits.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_IOP_STALE_H
#define RDNA3_IOP_STALE_H

#include <stdint.h>
#include <string.h>

#define RD3_IS_SLOTS   8u    /* surfaces remembered per head (WindowServer's ring is 2-3 buffers) */
#define RD3_IS_BUDGET  4u    /* timeouts in a row before the waits stop */
#define RD3_IS_PROBE   64u   /* a stale present that waits again while the waits are stopped */
#define RD3_IS_WAIT_MS 12u   /* the default bound of one wait: less than a 60 Hz frame */
#define RD3_IS_WAIT_MAX_MS 100u

typedef struct { uint32_t id, lseed, useed; uint64_t t; } rd3_is_ent;
typedef struct {
    rd3_is_ent e[RD3_IS_SLOTS];
    uint32_t next;       /* the slot the next new surface takes (round robin) */
    uint32_t misses;     /* timeouts in a row */
    uint32_t stopped;    /* stale presents not waited for since the waits stopped */
} rd3_is_head;

enum rd3_is_verdict {
    RD3_IS_NEW = 0,      /* the head has not copied this surface yet (or forgot it) */
    RD3_IS_FRESH = 1,    /* its seeds moved since the head last copied it */
    RD3_IS_STALE = 2     /* the same seeds as at its last copy: nothing wrote it since */
};

static inline void rd3_is_init(rd3_is_head *h) { memset(h, 0, sizeof *h); }

/* the verdict for a present of surface `id` whose seeds are now (lseed, useed); *last_t the time of its last
 * copy on this head (0 for NEW). Records nothing. */
static inline int rd3_is_check(const rd3_is_head *h, uint32_t id, uint32_t lseed, uint32_t useed, uint64_t *last_t)
{
    if (last_t) *last_t = 0;
    if (!id) return RD3_IS_NEW;
    for (uint32_t i = 0; i < RD3_IS_SLOTS; i++) {
        const rd3_is_ent *e = &h->e[i];
        if (e->id != id) continue;
        if (last_t) *last_t = e->t;
        return (e->lseed == lseed && e->useed == useed) ? RD3_IS_STALE : RD3_IS_FRESH;
    }
    return RD3_IS_NEW;
}

/* the seeds the surface had when the head copied it at `now` (its slot kept, or the next one taken) */
static inline void rd3_is_note(rd3_is_head *h, uint32_t id, uint32_t lseed, uint32_t useed, uint64_t now)
{
    if (!id) return;
    rd3_is_ent *e = 0;
    for (uint32_t i = 0; i < RD3_IS_SLOTS && !e; i++) if (h->e[i].id == id) e = &h->e[i];
    if (!e) { e = &h->e[h->next % RD3_IS_SLOTS]; h->next = (h->next + 1) % RD3_IS_SLOTS; }
    e->id = id; e->lseed = lseed; e->useed = useed; e->t = now;
}

/* a surface that is gone (or whose memory changed): forgotten, its next present is NEW */
static inline void rd3_is_forget(rd3_is_head *h, uint32_t id)
{
    for (uint32_t i = 0; i < RD3_IS_SLOTS; i++) if (h->e[i].id == id) memset(&h->e[i], 0, sizeof h->e[i]);
}

/* a STALE present: wait for its seeds (1) or not (0). wait_ms 0 = count only. While the budget is spent,
 * only every RD3_IS_PROBE-th stale present waits (a probe) */
static inline int rd3_is_should_wait(rd3_is_head *h, uint32_t wait_ms)
{
    if (!wait_ms) return 0;
    if (h->misses < RD3_IS_BUDGET) return 1;
    h->stopped++;
    return (h->stopped % RD3_IS_PROBE) == 0;
}

/* after a wait: the seeds moved (the write came: the waits are armed again) or the bound passed */
static inline void rd3_is_waited(rd3_is_head *h, int moved)
{
    if (moved) { h->misses = 0; h->stopped = 0; }
    else if (h->misses < 0xffffffffu) h->misses++;
}

/* the wait bound a switch value asks for (ms), clamped */
static inline uint32_t rd3_is_wait_ms(int v)
{
    if (v <= 0) return 0;
    return (uint32_t)v > RD3_IS_WAIT_MAX_MS ? RD3_IS_WAIT_MAX_MS : (uint32_t)v;
}

#endif
