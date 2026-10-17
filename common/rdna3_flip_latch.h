/*
 * rdna3_flip_latch.h - when a legacy swap's flip reaches the panel, and
 * whether a buffer changed while the panel scanned it (card run
 * 2026-10-17, the ~10th run of "windows and menus flicker on both
 * screens"). Plain C, no kernel headers: the accelerator uses it and
 * tests/test_flip_latch.c checks it on the host.
 *
 * The accelerator presents a legacy swap by flipping the head to the back
 * buffer itself (rd3AccelFlipTo -> RDNA3Framebuffer::flipToLocked ->
 * dcn32_flip). The flip only programs the HUBP address; the HUBP takes it
 * at the next vblank, up to a frame later, and until then it keeps reading
 * the buffer shown before. submitSwapCopy returned at once, so WindowServer
 * got the old buffer back (copy forward into it, the next frame drawn and
 * paged on into it) while the panel still scanned it: for up to a frame the
 * panel shows a half-drawn picture - the wallpaper without the windows.
 * amdgpu sends the page-flip event only from dm_pflip_high_irq, once the
 * HUBP took the new address (drm_crtc_send_vblank_event), the next commit
 * waits for it (drm_atomic_helper_wait_for_flip_done), and userspace reuses
 * the old buffer only after the event; the swap now waits the same way
 * before it returns (debug.rdna3accel_swap_latch, default 1).
 *
 * The latch is read as dcn32_flip_pending reads it (hubp2_is_flip_pending:
 * SURFACE_FLIP_PENDING, then DCSURF_SURFACE_EARLIEST_INUSE against the
 * address asked for), through the core's rdna3DisplayStatus.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_FLIP_LATCH_H
#define RDNA3_FLIP_LATCH_H

#include <stdint.h>

/* OTG frames after the flip by which it must have latched (the flip is
 * programmed inside frame f and latches at the vblank that ends it; two
 * frame-counter steps later it is late) */
#define RD3_LATCH_FRAMES 2u
/* the wait's limit when the frame counter does not move (a blanked head,
 * display sleep): two 60 Hz frames */
#define RD3_LATCH_CAP_US 35000u
/* timeouts in a row after which a head stops waiting until a flip is seen
 * latched again (a head that never latches must not cost every swap 35 ms) */
#define RD3_LATCH_TIMEOUT_RUN 3u

enum rd3_latch_step {
    RD3_LATCH_DONE = 0,     /* latched: the panel reads the new buffer */
    RD3_LATCH_WAIT = 1,     /* still pending: look again */
    RD3_LATCH_TIMEOUT = 2,  /* still pending after RD3_LATCH_FRAMES frames or RD3_LATCH_CAP_US */
    RD3_LATCH_UNKNOWN = 3   /* no status from the core */
};

/* One look at the head after a flip: status_ok (the core answered),
 * pending (dcn32_flip_pending), the OTG frame counter (24 bit) when the
 * flip was programmed and now, the microseconds waited so far. */
static inline enum rd3_latch_step rd3_latch_step(int status_ok, int pending, uint32_t frame0, uint32_t frame,
                                                 uint64_t waited_us)
{
    if (!status_ok)
        return RD3_LATCH_UNKNOWN;
    if (!pending)
        return RD3_LATCH_DONE;
    if (((frame - frame0) & 0xffffffu) >= RD3_LATCH_FRAMES || waited_us >= RD3_LATCH_CAP_US)
        return RD3_LATCH_TIMEOUT;
    return RD3_LATCH_WAIT;
}

/* whether a swap waits for its flip: the sysctl on, and the head not in a
 * run of timeouts (one flip seen latched ends the run) */
static inline int rd3_latch_should_wait(int enabled, unsigned timeout_run)
{
    return enabled && timeout_run < RD3_LATCH_TIMEOUT_RUN;
}

/* where a buffer stands against the head's panel: 'shown' the buffer the
 * last flip went to, 'prev' the one before it, 'latched' that flip was seen
 * latched, 'pending' the flip state read now (it decides only when not
 * latched yet). The previous buffer is on the panel until the flip away
 * from it latches. */
enum rd3_panel { RD3_PANEL_NO = 0, RD3_PANEL_SHOWN = 1, RD3_PANEL_LEAVING = 2 };
static inline enum rd3_panel rd3_latch_panel(uint64_t shown, uint64_t prev, int latched, int pending, uint64_t buf)
{
    if (!buf)
        return RD3_PANEL_NO;
    if (buf == shown)
        return RD3_PANEL_SHOWN;
    if (buf == prev && !latched && pending)
        return RD3_PANEL_LEAVING;
    return RD3_PANEL_NO;
}

/* the bands of a buffer whose sample hash changed between its flip and
 * now: bit b = band b ('bands' up to 32) */
static inline unsigned rd3_torn_bands(const uint32_t *at_flip, const uint32_t *now, unsigned bands)
{
    unsigned b, mask = 0;
    for (b = 0; at_flip && now && b < bands && b < 32; b++)
        if (at_flip[b] != now[b])
            mask |= 1u << b;
    return mask;
}

/* what a change of the shown buffer was. same_next: this swap shows the
 * same buffer again (one back buffer: WindowServer draws each frame into
 * the buffer the panel scans, by design of a zero-copy flip); otherwise it
 * changed while on the panel and is flipped away from now - nobody should
 * write a shown buffer (torn: the panel showed the writes) */
enum rd3_torn_kind { RD3_TORN_NONE = 0, RD3_TORN_SHOWN = 1, RD3_TORN_INPLACE = 2 };
static inline enum rd3_torn_kind rd3_torn_classify(unsigned changed, int same_next)
{
    if (!changed)
        return RD3_TORN_NONE;
    return same_next ? RD3_TORN_INPLACE : RD3_TORN_SHOWN;
}

/* rate limit of the driver's log lines: the first 'first', then every
 * (mask + 1)th ('mask' a power of two minus one); k counts from 0 */
static inline int rd3_log_due(uint64_t k, uint64_t first, uint64_t mask)
{
    return k < first || (k & mask) == 0;
}

#endif
