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
 * What the waits may cost in vain is capped per head: at most RD3_IS_CAP_MS of TIMED-OUT waiting in a one-second window
 * (ECC review of 5a8765b: a count of timeouts in a row is reset by one late write, so timeout, timeout,
 * timeout, moved repeating never tripped it, and a writer always later than the bound stopped the waits for
 * good). A legitimate re-present of an unchanged buffer then costs at most the cap per second; a wait that sees
 * the write is not charged; a writer always later than the bound is waited for again in every new window. A wait
 * is never longer than what is left of the cap.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_IOP_STALE_H
#define RDNA3_IOP_STALE_H

#include <stdint.h>
#include <string.h>

#define RD3_IS_SLOTS   8u    /* surfaces remembered per head (WindowServer's ring is 2-3 buffers) */
#define RD3_IS_CAP_MS  48u   /* the most a head waits in one second (about three 60 Hz frames) */
#define RD3_IS_WINDOW_NS 1000000000ull
#define RD3_IS_WAIT_MS 12u   /* the default bound of one wait: less than a 60 Hz frame */
#define RD3_IS_WAIT_MAX_MS 100u

typedef struct { uint32_t id, lseed, useed; uint64_t t; } rd3_is_ent;
typedef struct {
    rd3_is_ent e[RD3_IS_SLOTS];
    uint32_t next;       /* the slot the next new surface takes (round robin; nothing forgets a freed id: a reused id whose new
                          * seeds equal the old ones by chance gives one false STALE, a wait its first write ends) */
    uint64_t win_start;  /* the current one-second window (ns) */
    uint64_t win_us;     /* waited in it, us */
    uint32_t capped;     /* stale presents in it not waited for (the cap): the kext logs when a window begins capping */
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

/* a STALE present at `now_ns`: how long it may wait, in us (0: not at all - wait_ms 0 = count only, or the head's
 * cap for this one-second window is spent; then h->capped counts it and is 1 for the window's first one) */
static inline uint64_t rd3_is_wait_us(rd3_is_head *h, uint32_t wait_ms, uint64_t now_ns)
{
    if (!wait_ms) return 0;
    if (!h->win_start || now_ns - h->win_start >= RD3_IS_WINDOW_NS || now_ns < h->win_start) {
        h->win_start = now_ns; h->win_us = 0; h->capped = 0;
    }
    const uint64_t cap = (uint64_t)RD3_IS_CAP_MS * 1000u, want = (uint64_t)wait_ms * 1000u;
    if (h->win_us >= cap) { h->capped++; return 0; }
    return cap - h->win_us < want ? cap - h->win_us : want;
}

/* after a wait: one that ended without the write (a timeout: waiting in vain) is charged to the window; one that
 * saw the write cost what the late frame needed and is the fix, never charged */
static inline void rd3_is_waited(rd3_is_head *h, int moved, uint64_t waited_us)
{
    if (!moved) h->win_us += waited_us;
}

/* the wait bound a switch value asks for (ms), clamped: the sysctl's int, and the boot-arg's unsigned value
 * (a value of 2^31 or more must not turn negative and silently mean 0: it is the maximum) */
static inline uint32_t rd3_is_wait_ms(int v)
{
    if (v <= 0) return 0;
    return (uint32_t)v > RD3_IS_WAIT_MAX_MS ? RD3_IS_WAIT_MAX_MS : (uint32_t)v;
}
static inline uint32_t rd3_is_wait_ms_u(uint32_t v) { return v > RD3_IS_WAIT_MAX_MS ? RD3_IS_WAIT_MAX_MS : v; }

#endif
