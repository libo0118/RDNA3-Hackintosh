/*
 * rdna3_flip_queue.h - a head's presents without blocking the presenter
 * (DRIVER-PLAN N38). Plain C99, no kernel headers: the accelerator keeps
 * one per head, tests/test_flip_queue.c and tests/sim_navi32.c check it on
 * the host.
 *
 * Until now a mode 0 legacy swap programmed its flip and slept until the
 * HUBP took the new address (rdna3_flip_latch.h), under the family's lock on
 * WindowServer's thread: card run 2026-10-18b measured a median 14.8 ms per
 * flip and frames every third vblank (~20 fps). amdgpu never sleeps there:
 * amdgpu_dm_commit_planes programs the flip and returns, dm_pflip_high_irq
 * completes the page-flip event once the HUBP latched it
 * (drm_crtc_send_vblank_event), and the next commit for the CRTC waits for
 * that event (drm_atomic_helper_wait_for_flip_done) - at most one flip in
 * flight per CRTC. The queue below is that model for a head:
 *   - a present with nothing in flight is programmed at once (PROGRAM);
 *   - a present while a flip is in flight waits in one slot (QUEUED); a
 *     newer present replaces it (REPLACED: newest wins, the older frame is
 *     never shown) - the presenter itself never waits;
 *   - the flip-done path (the accelerator's per-head worker, woken from the
 *     framebuffer's vblank) sees the latch (rd3_fq_latched): the in-flight
 *     present completes there and not before, and the queued one, if any,
 *     is programmed then;
 *   - the framebuffer answering Busy (another path's flip still pending)
 *     keeps the present queued behind that flip (rd3_fq_busy), never drops it;
 *     a head whose flips never latch (blanked, a stuck HUBP) is bounded:
 *     after RD3_FQ_STUCK_RUN such flips given up the queued one is flipped
 *     anyway (rd3_fq_verdict);
 *   - a present of the buffer the head scans is not flipped again only with
 *     nothing in flight or queued (rd3_fq_present, after the latch look).
 * A legacy swap has no event of its own the driver could complete later
 * (submitSwapCopy's IOAccelEvent is the family's; DRIVER-PLAN N38): what
 * WindowServer gets back when the swap returns is the buffer leaving the
 * panel, and it may write it as soon as the swap returns. rd3_fq_reuse says
 * what a write into a buffer would hit; the accelerator makes the writers
 * it sees (the family's page-on, copy forward) wait for the latch only when
 * they target the buffer still leaving the panel - the reuse point where
 * userspace waits for the page-flip event under amdgpu.
 *
 * Addresses are VRAM addresses (0: none). Tokens are the caller's (a swap
 * number); 0 is a valid token.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_FLIP_QUEUE_H
#define RDNA3_FLIP_QUEUE_H

#include <stdint.h>

/* 'inflight' when the framebuffer answered Busy: a flip of another path
 * (an IOP transaction, modes 1/2, a flip home) is pending; ours waits */
#define RD3_FQ_FOREIGN 1ull

struct rd3_fq {
    uint64_t shown;      /* where the head's last latched flip of ours went (0: unknown) */
    uint64_t inflight;   /* programmed, its latch not seen yet (0: none; RD3_FQ_FOREIGN: another path's flip) */
    uint64_t queued;     /* the newest present waiting for the in-flight flip (0: none) */
    uint32_t inflight_tok, queued_tok;
    uint32_t joined;     /* presents of the in-flight address that complete with it */
    uint32_t stuck_run;  /* flips given up in a row with another path's flip in flight (rd3_fq_verdict) */
};

enum rd3_fq_act {
    RD3_FQ_IDLE = 0,      /* nothing to program */
    RD3_FQ_PROGRAM = 1,   /* program this address now: the head has no flip in flight */
    RD3_FQ_QUEUED = 2,    /* a flip is in flight: queued, programmed once that one latched */
    RD3_FQ_REPLACED = 3,  /* queued, replacing an older queued present (newest wins; *dropped is its token) */
    RD3_FQ_JOINED = 4,    /* the in-flight flip's own address, nothing queued: completes with that flip */
    RD3_FQ_SAME = 5       /* the buffer the head already scans, nothing in flight or queued: not flipped
                           * again (rd3_fq_present) */
};

/* another path's flip (Busy) given up this many times in a row - a blanked
 * or DPMS-off head, a stuck HUBP, a flip of another path that never latches
 * - and the queued present is programmed without RD3K_FLIP_FLAG_NOWAIT:
 * the framebuffer waits at most 20 ms for the pending flip and flips anyway
 * (its path before N38; amdgpu: drm_atomic_helper_wait_for_flip_done gives
 * up on a flip_done that never comes, logs it and commits the next flip).
 * The same run as the swap latch's (RD3_LATCH_TIMEOUT_RUN): a head that
 * never latches costs a bounded time per frame, never the frame itself */
#define RD3_FQ_STUCK_RUN 3u

static inline void rd3_fq_init(struct rd3_fq *q)
{
    q->shown = q->inflight = q->queued = 0;
    q->inflight_tok = q->queued_tok = q->joined = 0;
    q->stuck_run = 0;
}

/* whether a flip of ours or someone else's is in flight */
static inline int rd3_fq_pending(const struct rd3_fq *q)
{
    return q->inflight != 0;
}

/* a new present of 'addr'. Never asks the caller to wait. The queue never
 * skips a present of the address on the panel by itself (the record may be
 * stale after another path's flip): the caller decides that from the
 * HUBP's own address (rd3_latch_same_shown) before it presents */
static inline enum rd3_fq_act rd3_fq_submit(struct rd3_fq *q, uint64_t addr, uint32_t tok, uint32_t *dropped)
{
    if (!addr || addr == RD3_FQ_FOREIGN)
        return RD3_FQ_IDLE;
    if (!q->inflight) {
        q->inflight = addr;
        q->inflight_tok = tok;
        q->joined = 0;
        return RD3_FQ_PROGRAM;
    }
    if (!q->queued && addr == q->inflight) {
        q->joined++;
        return RD3_FQ_JOINED;
    }
    if (q->queued) {
        if (dropped)
            *dropped = q->queued_tok;
        q->queued = addr;
        q->queued_tok = tok;
        return RD3_FQ_REPLACED;
    }
    q->queued = addr;
    q->queued_tok = tok;
    return RD3_FQ_QUEUED;
}

/* whether a present of 'addr' may be the buffer the head already scans:
 * the queue's own record of the head's last latched flip is 'addr' and
 * nothing is in flight or queued (the caller then reads the HUBP's address
 * and passes it to rd3_fq_present). A present behind a flip in flight or a
 * queued one is never skipped: the queued present would be programmed over
 * it at the latch (ECC review of N38: a stale frame) */
static inline int rd3_fq_same_wants(const struct rd3_fq *q, uint64_t addr)
{
    return addr && addr != RD3_FQ_FOREIGN && addr == q->shown && !q->inflight && !q->queued;
}

/* a present, in the accelerator's order (rd3AccelPresentSubmit, under its
 * lock): the caller has taken a latch it can see now first (the flip-done
 * path's look), then this decides: the buffer the head scans (the queue's
 * record and the HUBP's address 'scan', no flip pending, 'same_on' the
 * swap_samenoflip knob) is RD3_FQ_SAME - not flipped, nothing waits -
 * anything else goes to rd3_fq_submit. 'scan_known' 0 (the HUBP's address
 * unreadable) never skips */
static inline enum rd3_fq_act rd3_fq_present(struct rd3_fq *q, uint64_t addr, uint32_t tok, int same_on,
                                             int scan_known, uint64_t scan, int scan_pending, uint32_t *dropped)
{
    if (same_on && rd3_fq_same_wants(q, addr) && scan_known && scan == addr && !scan_pending)
        return RD3_FQ_SAME;
    return rd3_fq_submit(q, addr, tok, dropped);
}

/* the framebuffer answered Busy to the PROGRAM just returned (or to the
 * queued present the flip-done path programs): another path's flip is
 * pending. The present waits behind it (Busy is kept, never dropped). A
 * newer present queued meanwhile stays the one shown (newest wins): the
 * Busy one then counts as replaced (returns 1, *dropped its token) */
static inline int rd3_fq_busy(struct rd3_fq *q, uint32_t *dropped)
{
    int replaced = 0;
    if (!q->inflight || q->inflight == RD3_FQ_FOREIGN)
        return 0;
    if (q->queued) {
        if (dropped)
            *dropped = q->inflight_tok;
        replaced = 1;
    } else {
        q->queued = q->inflight;
        q->queued_tok = q->inflight_tok;
    }
    q->inflight = RD3_FQ_FOREIGN;
    q->inflight_tok = 0;
    q->joined = 0;
    return replaced;
}

/* the framebuffer refused the PROGRAM (a size other than the mode's, the
 * core's flip failed): nothing in flight; the present's token is returned
 * (it never reached the panel) */
static inline uint32_t rd3_fq_refused(struct rd3_fq *q)
{
    const uint32_t tok = q->inflight_tok;
    if (q->inflight != RD3_FQ_FOREIGN)
        q->inflight = 0;
    q->inflight_tok = 0;
    q->joined = 0;
    return tok;
}

/* the in-flight flip latched (or, 'timeout', did not in time and is given
 * up: the caller counts its completion as early). *done_tok the present
 * that completes now (*had_done 0 for another path's flip); returns
 * RD3_FQ_PROGRAM with *next_addr / *next_tok when a queued present is to
 * be programmed now (already in flight in the record), else RD3_FQ_IDLE */
static inline enum rd3_fq_act rd3_fq_latched(struct rd3_fq *q, int *had_done, uint32_t *done_tok,
                                             uint64_t *next_addr, uint32_t *next_tok)
{
    if (had_done)
        *had_done = q->inflight && q->inflight != RD3_FQ_FOREIGN;
    if (done_tok)
        *done_tok = q->inflight_tok;
    if (q->inflight && q->inflight != RD3_FQ_FOREIGN)
        q->shown = q->inflight;
    else if (q->inflight == RD3_FQ_FOREIGN)
        q->shown = 0;   /* someone else's address: nothing of ours known on the panel */
    q->inflight = 0;
    q->inflight_tok = 0;
    q->joined = 0;
    if (!q->queued)
        return RD3_FQ_IDLE;
    q->inflight = q->queued;
    q->inflight_tok = q->queued_tok;
    q->queued = 0;
    q->queued_tok = 0;
    if (next_addr)
        *next_addr = q->inflight;
    if (next_tok)
        *next_tok = q->inflight_tok;
    return RD3_FQ_PROGRAM;
}

/* the flip-done path's verdict on the flip in flight, in the accelerator's
 * order (rd3AccelPqLatchedLocked): 'latched' 1 seen latched, 0 given up
 * (not latched in time, the state unreadable). rd3_fq_latched's outputs;
 * *foreign_gaveup 1 when that was another path's flip (Busy) given up;
 * *force 1 when the queued present handed out is to be programmed without
 * RD3K_FLIP_FLAG_NOWAIT: RD3_FQ_STUCK_RUN such flips in a row since a
 * latch was last seen (the head's flips stay pending: without it, Busy ->
 * given up -> Busy again forever, no frame and an IOP transaction never
 * complete). A latch seen ends the run */
static inline enum rd3_fq_act rd3_fq_verdict(struct rd3_fq *q, int latched, int *had_done, uint32_t *done_tok,
                                             uint64_t *next_addr, uint32_t *next_tok, int *foreign_gaveup,
                                             int *force)
{
    const int foreign = q->inflight == RD3_FQ_FOREIGN;
    enum rd3_fq_act a;
    if (latched)
        q->stuck_run = 0;
    else if (foreign && q->stuck_run < 0xffffffffu)
        q->stuck_run++;
    if (foreign_gaveup)
        *foreign_gaveup = !latched && foreign;
    a = rd3_fq_latched(q, had_done, done_tok, next_addr, next_tok);
    if (force)
        *force = a == RD3_FQ_PROGRAM && q->stuck_run >= RD3_FQ_STUCK_RUN;
    return a;
}

/* another path flipped the head (the IOP copy fallback's flip home, which
 * the framebuffer programs after the pending flip latched or 20 ms): the
 * queued present is dropped (returns 1, *dropped its token - programmed
 * at the next latch it would cover the picture that path put there), the
 * in-flight present of ours completes now (*had_done, *done_tok: its buffer
 * is left by that flip), and that flip is what is in flight now (FOREIGN):
 * nothing of ours known on the panel */
static inline int rd3_fq_moved(struct rd3_fq *q, int *had_done, uint32_t *done_tok, uint32_t *dropped)
{
    const int had = q->queued != 0;
    if (had_done)
        *had_done = q->inflight && q->inflight != RD3_FQ_FOREIGN;
    if (done_tok)
        *done_tok = q->inflight != RD3_FQ_FOREIGN ? q->inflight_tok : 0;
    if (had && dropped)
        *dropped = q->queued_tok;
    q->inflight = RD3_FQ_FOREIGN;
    q->inflight_tok = 0;
    q->joined = 0;
    q->queued = 0;
    q->queued_tok = 0;
    q->shown = 0;
    return had;
}

/* a mode change or another path's flip moved the panel: nothing of ours is
 * known to be on it; the queued present (returns 1, *dropped its token) is
 * dropped - it was meant for a picture that is gone. The in-flight flip
 * stays: its latch is still seen (or given up) by the flip-done path */
static inline int rd3_fq_forget(struct rd3_fq *q, uint32_t *dropped)
{
    const int had = q->queued != 0;
    if (had && dropped)
        *dropped = q->queued_tok;
    q->shown = 0;
    q->queued = 0;
    q->queued_tok = 0;
    q->stuck_run = 0;   /* a new picture: the head's flips are given their chance again */
    return had;
}

/* the buffer at 'addr' is freed: a queued present of it is dropped
 * (returns 1, *dropped its token) and the record forgets it. An in-flight
 * flip cannot be taken back (the caller counts a buffer freed while
 * scanned) */
static inline int rd3_fq_forget_addr(struct rd3_fq *q, uint64_t addr, uint32_t *dropped)
{
    int had = 0;
    if (!addr)
        return 0;
    if (q->queued == addr) {
        if (dropped)
            *dropped = q->queued_tok;
        q->queued = 0;
        q->queued_tok = 0;
        had = 1;
    }
    if (q->shown == addr)
        q->shown = 0;
    return had;
}

/* what a write into the buffer at 'addr' would hit */
enum rd3_fq_use {
    RD3_FQ_FREE = 0,      /* not on the panel and not about to be */
    RD3_FQ_LEAVING = 1,   /* scanned until the in-flight flip latches: a writer waits for that latch */
    RD3_FQ_SCANNED = 2,   /* the panel's buffer, no flip away from it: written in place (one back buffer) -
                           * waiting would not help, the buffer stays on the panel */
    RD3_FQ_ARRIVING = 3   /* in flight or queued: written before it is shown (the newer frame wins) */
};
static inline enum rd3_fq_use rd3_fq_reuse(const struct rd3_fq *q, uint64_t addr)
{
    if (!addr)
        return RD3_FQ_FREE;
    if (addr == q->shown) {
        if (q->inflight && q->inflight != addr)
            return RD3_FQ_LEAVING;
        return RD3_FQ_SCANNED;
    }
    if (addr == q->inflight || addr == q->queued)
        return RD3_FQ_ARRIVING;
    return RD3_FQ_FREE;
}

/* whether the buffer at 'addr' is on the panel or about to be (never
 * reported free while the head scans it) */
static inline int rd3_fq_in_use(const struct rd3_fq *q, uint64_t addr)
{
    return rd3_fq_reuse(q, addr) != RD3_FQ_FREE;
}

/* The zero-copy flip of an IOP transaction's IOSurface (debug.rdna3accel_
 * iop_flip): only VRAM the family marks newest (its validity bit) AND the
 * Metal plugin wrote - the dirty escape (RD3K_ESC_SURF_DIRTY) recorded the
 * allocation's generation (a number of its own per allocPhysical, never 0
 * and never reused: a new allocation at the same VRAM offset is not the
 * one written, ECC review of N38), which the plugin sends from surface-out only after
 * the command buffer's submission was waited for (NVMTLEventSync.m), so a
 * marked VRAM copy holds the GPU's finished frame. VRAM the family marks
 * newest that the plugin never wrote holds no frame: such a surface is
 * copied from its system memory instead (NullMoth's design, our code) */
static inline int rd3_iop_flip_ok(int vram_newest, uint64_t wrote_gen, uint64_t alloc_gen)
{
    return vram_newest && wrote_gen != 0 && wrote_gen == alloc_gen;
}

#endif
