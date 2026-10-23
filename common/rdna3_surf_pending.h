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
 * also takes the forgiven one (later, never earlier): the table cannot tell
 * the late ready from the newer write's own one. If it was the newer
 * write's, the dropped mark's ghost now sits in the count; that pending
 * mark is remembered as a ghost and the next timeout drops it for good
 * instead of forgiving it again (round-2 review of N41: a lost ready made
 * every later page-off of that surface wait the full bound, the forgiven
 * mark moving between "forgiven" and the count at each write). So a ready
 * that never comes costs at most two bounded waits: its own, and one more
 * when a newer write's ready took its forgiven mark (the table cannot tell
 * the two readies apart; never one per page-off). Forgiven marks older than
 * RD3_SP_FORGIVE_MS are aged out before a ready takes one: a ready that
 * late is not coming (counted in aged), the next write's ready ends its own
 * wait.
 *
 * Timeout budget (defence in depth, security review of N41): after
 * RD3_SP_BUDGET timeouts with no real ready between them (READY / STILL,
 * not LATE) the table is "starved": new waits answer RD3_SPW_SKIP (pending,
 * not waited) until the next real ready. A WindowServer whose readies all
 * get lost costs RD3_SP_BUDGET bounded waits, not one per page-off.
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
#define RD3_SP_FORGIVE_MS (10u * RD3_SP_WAIT_MS) /* forgiven marks older than this are aged out before a ready takes one */
#define RD3_SP_BUDGET    4u               /* timeouts with no real ready between them: then waits are skipped */

struct rd3_sp_slot {
    uint32_t id;          /* the IOSurface id (0: free) */
    uint32_t count;       /* pending marks */
    uint32_t forgiven;    /* marks a timeout dropped whose readies have not come yet (late) */
    uint32_t ghost;       /* pending marks that may be a forgiven mark a newer write's ready took (<= count) */
    uint64_t forgiven_ns; /* when a timeout last added forgiven marks (their age) */
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
    /* the timeout budget: timeouts since the last real ready, starved (waits skipped) */
    uint32_t timeouts_in_row;
    uint32_t starved;
    /* statistics for the caller's sysctls */
    uint64_t aged;            /* forgiven marks aged out (their readies never came) */
    uint64_t ghosts_dropped;  /* pending marks a timeout dropped for good as ghosts */
    uint64_t starvations;     /* times the budget ran out */
    uint64_t skipped;         /* waits skipped while starved */
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
    RD3_SPW_CLEARED = 3,    /* the episode this waiter waited for was cleared by someone else (not ready) */
    RD3_SPW_SKIP = 4        /* pending, not waited: the timeout budget is spent until a real ready (starved) */
};

static inline struct rd3_sp_slot *rd3_sp_find(struct rd3_sp *t, uint32_t id)
{
    for (unsigned i = 0; i < RD3_SP_SLOTS; i++)
        if (t->s[i].id == id) return &t->s[i];
    return 0;
}

static inline void rd3_sp_slot_free(struct rd3_sp_slot *s)
{
    s->id = 0; s->count = 0; s->forgiven = 0; s->ghost = 0; s->forgiven_ns = 0; s->seq = 0; s->since_ns = 0;
}

/* a real ready (not late): the budget starts again, waits are no longer skipped */
static inline void rd3_sp_real_ready(struct rd3_sp *t)
{
    t->timeouts_in_row = 0;
    t->starved = 0;
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
        if (s->forgiven && now_ns > s->forgiven_ns &&
            now_ns - s->forgiven_ns > (uint64_t)RD3_SP_FORGIVE_MS * 1000000ull) {   /* their readies are not coming */
            t->aged += s->forgiven;
            s->forgiven = 0; s->forgiven_ns = 0;
            if (!s->count) { rd3_sp_slot_free(s); return RD3_SPM_LATE; }
        }
        if (s->forgiven) {   /* a ready of a mark a timeout dropped: never a newer mark's count */
            s->forgiven--;
            if (s->count && s->ghost < s->count) s->ghost++;   /* or the newer write's own: a ghost may stay */
            if (!s->count && !s->forgiven) rd3_sp_slot_free(s);
            return RD3_SPM_LATE;
        }
        if (!s->count) { rd3_sp_slot_free(s); return RD3_SPM_LATE; }   /* not reached: such a slot is free */
        rd3_sp_real_ready(t);
        if (--s->count) { if (s->ghost > s->count) s->ghost = s->count; return RD3_SPM_STILL; }
        rd3_sp_slot_free(s);
        return RD3_SPM_READY;
    }
    if (s) {
        if (s->count >= RD3_SP_MAX_COUNT) return RD3_SPM_SATURATED;
        if (!s->count) { s->seq = ++t->seq; s->since_ns = now_ns; s->ghost = 0; }   /* a new episode (forgiven kept) */
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
 * call, *dropped the marks it held, forgiven but for the ghosts, which go for
 * good), CLEARED (the episode this waiter saw pending was cleared by another
 * waiter, an eviction or a clear), SKIP (pending, but the timeout budget is
 * spent: a first step does not wait, nothing is cleared). *epoch (0 before
 * the first step; may be 0 for no CLEARED answers) is the waiter's episode,
 * set at its first WAIT. */
static inline enum rd3_sp_wait_res rd3_sp_wait_step(struct rd3_sp *t, uint32_t id, uint64_t now_ns,
                                                    uint64_t deadline_ns, uint32_t *dropped, uint64_t *epoch)
{
    if (dropped) *dropped = 0;
    if (!t || !id) return RD3_SPW_READY;
    struct rd3_sp_slot *s = rd3_sp_find(t, id);
    const uint64_t mine = epoch ? *epoch : 0;
    if (!s || !s->count || (mine && s->seq != mine))   /* the waited episode ended: by its ready, or cleared */
        return rd3_sp_was_cleared(t, id, mine) ? RD3_SPW_CLEARED : RD3_SPW_READY;
    if (!mine && t->starved) { t->skipped++; return RD3_SPW_SKIP; }
    if (epoch && !mine) *epoch = s->seq;
    if (now_ns < deadline_ns) return RD3_SPW_WAIT;
    if (dropped) *dropped = s->count;
    const uint32_t ghost = s->ghost > s->count ? s->count : s->ghost;
    const uint32_t keep = s->count - ghost;   /* the ghosts had their bounded wait already: dropped for good */
    t->ghosts_dropped += ghost;
    const uint64_t fg = (uint64_t)s->forgiven + keep;
    s->forgiven = fg > RD3_SP_MAX_COUNT ? RD3_SP_MAX_COUNT : (uint32_t)fg;
    if (keep) s->forgiven_ns = now_ns;
    rd3_sp_note_cleared(t, id, s->seq);
    s->count = 0; s->ghost = 0; s->since_ns = 0;
    if (!s->forgiven) rd3_sp_slot_free(s);
    if (++t->timeouts_in_row >= RD3_SP_BUDGET && !t->starved) { t->starved = 1; t->starvations++; }
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
    if (t) rd3_sp_real_ready(t);   /* a fresh start: the budget too */
    return n;
}

/* who may mark (security review of N41): the process name alone is not proof (any local process can be
 * called "WindowServer" and hold page-offs on the family's lock with its pending marks). A mark is taken
 * only from a process named WindowServer that runs as _windowserver (uid RD3_SP_WS_UID) or is the display
 * client (the pid of WindowServer's legacy swaps / display-pipe transactions, 0 none seen yet), pending
 * marks only while WindowServer's surfaces can be VRAM at all (ws_vram: rdna3a3ws, rdna3a3display or the
 * IOP flip; else the wait is MOOT and the plugin backs off as for the switch-off). Readies of the real
 * WindowServer are always taken (a mark pending from before must still clear) */
#define RD3_SP_WS_UID 88u   /* _windowserver on macOS */
enum rd3_sp_gate {
    RD3_SPG_OK = 0,
    RD3_SPG_OFF = 1,       /* debug.rdna3accel_surface_wait 0 */
    RD3_SPG_NOTWS = 2,     /* not named WindowServer */
    RD3_SPG_NOTREAL = 3,   /* named WindowServer, but neither uid 88 nor the display client's pid */
    RD3_SPG_MOOT = 4       /* a pending mark while no WindowServer surface can be VRAM */
};
static inline enum rd3_sp_gate rd3_sp_mark_gate(int on, int pending, int named_ws, unsigned uid, int pid,
                                                int display_pid, int ws_vram)
{
    if (!on) return RD3_SPG_OFF;
    if (!named_ws) return RD3_SPG_NOTWS;
    if (uid != RD3_SP_WS_UID && (display_pid <= 0 || pid != display_pid)) return RD3_SPG_NOTREAL;
    if (pending && !ws_vram) return RD3_SPG_MOOT;
    return RD3_SPG_OK;
}

/* the plane word of RD3K_ESC_SURF_DIRTY carries the GPU marks (accel/rdna3k_abi.h
 * RD3K_SS_PLANE_*): decoded here so the host test checks the same bits */
#define RD3_SP_PLANE_GPU_MARK 0x80000000u   /* a pending/ready mark, not a dirty mark: bit 0 = pending */
#define RD3_SP_PLANE_TRYLOCK  0x40000000u   /* a dirty mark that gives up when the accelerator lock is taken */
#define RD3_SP_PLANE_FLAGS    (RD3_SP_PLANE_GPU_MARK | RD3_SP_PLANE_TRYLOCK)

#endif
