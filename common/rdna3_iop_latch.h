/*
 * rdna3_iop_latch.h - the IOP copy's timing and its latched variant (DRIVER-PLAN N68, bare metal 2026-10-31).
 *
 * Plain C99, no kernel headers: accel/rdna3-accel.cpp includes it, tests/test_iop_latch.c checks it on the host.
 *
 * The owner's bare-metal Mac (macOS 26.7.2, two 1920x1080 heads, the Metal plugin; WindowServer presents 10-bit
 * 'l10r' surfaces) presents at 60 Hz, but a dragged window advances by very uneven steps per refresh (the 240 fps
 * videos: 10 1 8 10 4 8 11 0 21 9 44 ...), seen as ~30 fps. RD3DisplayPipe::performTransaction converts each l10r
 * frame on the CPU straight into the head's LIVE scanout buffer at whatever scan position the panel is at, and the
 * transaction completes at once, before any vblank: two copies may land in one refresh (one never shown), a refresh
 * may get none, and a copy that ends mid-scan tears. This header holds what decides and measures that:
 *
 *  K1  rd3_il_txn_slow: a transaction that held WindowServer's transaction thread over RD3_IL_SLOW_US (logged with
 *      its split: gates, the N59 stale wait, the copy, the latch wait);
 *  K2  rd3_il_vbl_class / rd3_il_scan_bin: where in the scan a copy ended - the head's OTG frame against the copy
 *      before (SAME: two copies in one refresh, SKIP: refreshes with no copy) and the scanline in quarters of the
 *      active height or in the vertical blank;
 *  fix rd3_il_pick / rd3_il_d_freeable: the latched copy (debug.rdna3accel_iop_latch). Each head gets a second
 *      scanout buffer D next to its own (home); a frame is copied into the one the panel does not scan and flipped
 *      to through the head's present queue (N38), and the transaction completes when the HUBP latched it - amdgpu's
 *      page flip: amdgpu_dm_commit_planes programs the new address, dm_pflip_high_irq sends the event at the latch
 *      (drm_crtc_send_vblank_event), and the next commit for the CRTC waits for that flip
 *      (drm_atomic_helper_wait_for_dependencies -> drm_crtc_commit_wait): one flip in flight per CRTC, the buffer
 *      being written is never the one scanned.
 *
 * Addresses are the buffers' CPU (BAR) addresses as the present queue keeps them (0: none).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_IOP_LATCH_H
#define RDNA3_IOP_LATCH_H

#include <stdint.h>
#include "rdna3_flip_queue.h"   /* RD3_FQ_FOREIGN: a pending flip the present queue does not know the target of */

/* ---- K1: a transaction over this held WindowServer's thread for half a 60 Hz refresh ---- */
#define RD3_IL_SLOW_US 8000u
static inline int rd3_il_txn_slow(uint64_t us)
{
    return us > RD3_IL_SLOW_US;
}

/* ---- K2: the head's OTG frame at the end of a copy, against the head's copy before ---- */
#define RD3_IL_FRAME_MASK 0xffffffu   /* OTG_FRAME_COUNT is 24 bits (RD3KDisplayStatus.frame) */
enum rd3_il_vbl {
    RD3_IL_VBL_FIRST = 0,   /* no copy before on this head, or the counter went back (a mode set, a reset) */
    RD3_IL_VBL_NEXT = 1,    /* one refresh after the copy before: one new frame per refresh */
    RD3_IL_VBL_SAME = 2,    /* the same refresh as the copy before: of the two, at most one was ever on the panel */
    RD3_IL_VBL_SKIP = 3     /* more than one refresh later: the panel repeated a frame (*delta - 1 refreshes) */
};
static inline enum rd3_il_vbl rd3_il_vbl_class(int have_prev, uint32_t prev, uint32_t now, uint32_t *delta)
{
    const uint32_t d = (now - prev) & RD3_IL_FRAME_MASK;
    if (delta)
        *delta = 0;
    if (!have_prev || d >= (RD3_IL_FRAME_MASK >> 1))   /* half the counter back: a reset, not 2^23 refreshes */
        return RD3_IL_VBL_FIRST;
    if (delta)
        *delta = d;
    return d == 0 ? RD3_IL_VBL_SAME : d == 1 ? RD3_IL_VBL_NEXT : RD3_IL_VBL_SKIP;
}

/* the scanline as a bin: 0..3 the quarters of the active height, RD3_IL_BIN_BLANK the vertical blank (the OTG in
 * its blank, or a line at or past the active height); height 0 (unknown) puts every line in the blank bin */
#define RD3_IL_BINS 4u
#define RD3_IL_BIN_BLANK 4u
static inline unsigned rd3_il_scan_bin(uint32_t scanline, int in_vblank, uint32_t height)
{
    if (in_vblank || !height || scanline >= height)
        return RD3_IL_BIN_BLANK;
    return (unsigned)(((uint64_t)scanline * RD3_IL_BINS) / height);
}

/* ---- the latched copy: which buffer a frame goes into ---- */
enum rd3_il_pick {
    RD3_IL_NONE = 0,   /* neither buffer is free: the caller copies into home directly, counted */
    RD3_IL_HOME = 1,   /* the framebuffer's own scanout, not on the panel */
    RD3_IL_D = 2,      /* the second buffer, not on the panel */
    RD3_IL_WAIT = 3    /* a flip is in flight: wait for its latch, then ask again (one flip in flight per head) */
};
/* what the caller knows of the head: the two buffers, its present queue (rdna3_flip_queue.h: the last latched
 * flip of ours, the one in flight - RD3_FQ_FOREIGN counts - and the queued one), the HUBP's address
 * (SURFACE_EARLIEST_INUSE as a BAR address; scan_known 0 when unreadable) and the buffer the latched path last
 * flipped to (last: what the head scans when nothing else says so) */
struct rd3_il_state {
    uint64_t home, d;
    uint64_t shown, inflight, queued;
    int scan_known;
    uint64_t scan;
    uint64_t last;
    uint64_t e;      /* the mailbox's third buffer (0: none; rd3_il_mb_pick) - rd3_il_pick ignores it */
    int queued_lb;   /* the queued present is a frame of this path (one of home / D / E, never programmed) */
};
/* the panel reads x now or will: queued, or scanned (the HUBP's address when it is known; else the queue's
 * record and the latched path's own). With the HUBP unreadable and no latch of ours on the queue's record (the
 * boot, a mode set, rd3_fq_moved / rd3_fq_forget after the direct-copy fallback's flip home) home counts as
 * scanned: the panel is on home or on a surface not ours, and 'last' may be stale (ECC cpp-reviewer of 0a47349, M2:
 * last still D after a flip home, home was counted free and written while scanned) */
static inline int rd3_il_in_use(const struct rd3_il_state *s, uint64_t x)
{
    if (x == s->queued || x == s->inflight)
        return 1;
    if (s->scan_known)
        return x == s->scan;
    if (!s->shown && x == s->home)
        return 1;
    return x == s->shown || x == s->last;
}
static inline enum rd3_il_pick rd3_il_pick(const struct rd3_il_state *s)
{
    int hb, db;
    if (!s->home || !s->d || s->home == s->d)
        return RD3_IL_NONE;
    if (s->inflight)
        return RD3_IL_WAIT;
    hb = rd3_il_in_use(s, s->home);
    db = rd3_il_in_use(s, s->d);
    if (!hb && !db)   /* the head scans another surface (a zero-copy flip): alternate from the last one */
        return s->last == s->d ? RD3_IL_HOME : RD3_IL_D;
    if (!hb)
        return RD3_IL_HOME;
    if (!db)
        return RD3_IL_D;
    return RD3_IL_NONE;
}
/* D may be given back (switched off, another mode's size, the accelerator's stop): nothing of the queue holds it
 * and the panel does not scan it - the HUBP's own address must say so, with no flip of any target in flight
 * (unknown: kept; a freed D is VRAM the next allocation scribbles on while it is on the panel) */
static inline int rd3_il_d_freeable(const struct rd3_il_state *s)
{
    if (!s->d)
        return 0;
    /* any flip in flight or queued may be one to D (RD3_FQ_FOREIGN, a pending flip the queue does not know of: its
     * target unknown; ECC cpp-reviewer of aeb57cf, C1): kept until it latched */
    if (s->inflight || s->queued || s->d == s->shown)
        return 0;
    return s->scan_known && s->scan != s->d;
}

/* ---- N68 OPEN (a): the mailbox - a third buffer E, the transaction complete once its frame is queued ----
 *
 * Bare-metal round 4 (2026-11-01): with rd3_il_pick above a windowed game ran at a flat 119-120 fps (300-400 with
 * the latch off): each transaction waited for the flip before it to latch (rd3AccelIopLbWait) and completed only at
 * its own latch, so WindowServer's transaction thread - and through it the game's drawables - ran at the panel's
 * rate. With two buffers that wait cannot go: one is scanned, the other is pending until the vblank.
 *
 * amdgpu does not let a vsync flip get ahead of the one pending either: a nonblocking commit while the CRTC's last
 * commit has not flipped is refused (drm_atomic_helper_setup_commit -> stall_checks: -EBUSY), a blocking one stalls
 * only on the commit two back (stall_checks waits for its cleanup_done: one commit flipping, one waiting in its
 * commit work for that flip - drm_atomic_helper_wait_for_dependencies), and DC reprograms a HUBP's address only
 * after the flip before it is done (hubp2_is_flip_pending: SURFACE_FLIP_PENDING, or SURFACE_EARLIEST_INUSE not yet
 * the requested address; hubp2_program_surface_flip_and_addr writes the address registers high then low, the low
 * write arming the flip). A pending flip is never re-targeted (only an immediate flip, which tears). The newest-wins
 * part lives above KMS: the compositor keeps the newest client buffer and commits it at the page-flip event, the
 * buffer it replaced is released unshown. Here the kernel is that compositor for WindowServer's display pipe, and
 * the head's present queue (rdna3_flip_queue.h, N38) already has the slot: one flip in flight, one queued present
 * programmed by the flip-done worker at the latch, a newer present replacing the queued one.
 *
 * So each head gets a third buffer E and the frame goes into a buffer that is neither scanned, in flight nor
 * queued (with three there always is one, or the queued one is taken back first: it was never programmed, the HUBP
 * never saw it), then into the queue; the transaction completes when the copy is done, never at a latch. At most
 * one flip per vblank latches (the queue's one in flight), the newest frame wins (each frame replaces the queued
 * one), no buffer is written while the HUBP scans it or has it pending, and nothing waits for a vblank. A flip
 * of unknown target in flight (RD3_FQ_FOREIGN: a mode set's, the framebuffer's own) is waited for as before. Without
 * E (its VRAM refused) the head keeps the two-buffer scheme above. */
#define RD3_IL_NBUF 3u   /* home, D, E */
enum rd3_il_mb {
    RD3_IL_MB_NONE = 0,   /* no buffers to choose from (no E, or two of the same address): the caller decides */
    RD3_IL_MB_COPY = 1,   /* copy into buf[*idx]: not scanned, not in flight, not queued */
    RD3_IL_MB_TAKE = 2,   /* all three in use, buf[*idx] is the queued frame of this path: take it back out of the
                           * queue (rd3_fq_take_queued; newest wins), then copy into it */
    RD3_IL_MB_WAIT = 3    /* a flip of unknown target in flight, or no buffer free with nothing to take: wait for
                           * the latch, then ask again */
};
/* the three buffers in rd3_il_state's order: 0 home, 1 D, 2 E */
static inline uint64_t rd3_il_mb_buf(const struct rd3_il_state *s, unsigned i)
{
    return i == 0 ? s->home : i == 1 ? s->d : i == 2 ? s->e : 0;
}
static inline enum rd3_il_mb rd3_il_mb_pick(const struct rd3_il_state *s, unsigned *idx)
{
    unsigned i, k, start = 0;
    *idx = 0;
    if (!s->home || !s->d || !s->e || s->home == s->d || s->home == s->e || s->d == s->e)
        return RD3_IL_MB_NONE;
    if (s->inflight == RD3_FQ_FOREIGN)   /* its target may be any of the three */
        return RD3_IL_MB_WAIT;
    for (i = 0; i < RD3_IL_NBUF; i++)    /* round robin from the buffer the path wrote last */
        if (s->last && rd3_il_mb_buf(s, i) == s->last)
            start = i + 1;
    for (k = 0; k < RD3_IL_NBUF; k++) {
        i = (start + k) % RD3_IL_NBUF;
        if (!rd3_il_in_use(s, rd3_il_mb_buf(s, i))) {
            *idx = i;
            return RD3_IL_MB_COPY;
        }
    }
    /* all three in use: one scanned, one in flight, one queued - the queued one was never programmed. Taken back
     * only when the HUBP is not on it (it cannot be: it was free when it was written) and it is this path's frame */
    for (i = 0; s->queued && s->queued_lb && i < RD3_IL_NBUF; i++)
        if (rd3_il_mb_buf(s, i) == s->queued && s->queued != s->inflight &&
            (s->scan_known ? s->scan != s->queued : s->shown != s->queued)) {
            *idx = i;
            return RD3_IL_MB_TAKE;
        }
    return RD3_IL_MB_WAIT;
}

/* why a frame of the latched path went to the direct copy instead (debug.rdna3accel_iop_latch_fb_<name>) */
enum rd3_il_fb {
    RD3_IL_FB_QUEUE = 0,     /* no present queue (debug.rdna3accel_iop_queue 0, not started) */
    RD3_IL_FB_VRAM = 1,      /* no second buffer: its VRAM was refused (tried again later) */
    RD3_IL_FB_RESIZE = 2,    /* a mode of another size: the old D still on the panel, not freed yet */
    RD3_IL_FB_BUSY = 3,      /* both buffers in use, or a flip still in flight after the wait */
    RD3_IL_FB_REFUSED = 4,   /* the framebuffer refused the flip (the frame copied again into home) */
    RD3_IL_FB_GEOM = 5,      /* the frame and D have no common extent (D's pitch / size) */
    RD3_IL_FB_N = 6
};
static inline const char *rd3_il_fb_name(unsigned why)
{
    static const char *const n[RD3_IL_FB_N] = { "queue", "vram", "resize", "busy", "refused", "geom" };
    return why < RD3_IL_FB_N ? n[why] : "?";
}

/* D's VRAM: after a refusal the next try waits this many latched transactions (~10 s at 60 Hz); the visible heap
 * on the bare-metal Mac had 21 of 144 MiB free with the plugin running */
#define RD3_IL_VRAM_RETRY 600u

#endif
