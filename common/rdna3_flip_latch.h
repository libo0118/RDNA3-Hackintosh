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
 * display sleep): at least two 60 Hz frames, at least 2.5 of the head's
 * own frames (30 Hz, VRR: rd3_latch_cap_us), at most RD3_LATCH_CAP_MAX_US */
#define RD3_LATCH_CAP_US 35000u
#define RD3_LATCH_CAP_MAX_US 100000u
/* timeouts in a row after which a head stops waiting until a flip is seen
 * latched again (a head that never latches must not cost every swap) */
#define RD3_LATCH_TIMEOUT_RUN 3u

/* the limit of a swap's wait for its flip, from that flip, for a head whose
 * frame lasts frame_us (0: unknown, 60 Hz assumed) */
static inline uint64_t rd3_latch_cap_us(uint64_t frame_us)
{
    uint64_t cap = frame_us * 5u / 2u;
    if (cap < RD3_LATCH_CAP_US)
        cap = RD3_LATCH_CAP_US;
    if (cap > RD3_LATCH_CAP_MAX_US)
        cap = RD3_LATCH_CAP_MAX_US;
    return cap;
}

enum rd3_latch_step {
    RD3_LATCH_DONE = 0,     /* latched: the panel reads the new buffer */
    RD3_LATCH_WAIT = 1,     /* still pending: look again */
    RD3_LATCH_TIMEOUT = 2,  /* this flip still pending after RD3_LATCH_FRAMES frames or the cap: stuck */
    RD3_LATCH_UNKNOWN = 3   /* no status from the core */
};

/* One look at the head after its flip: status_ok (the core answered),
 * pending (dcn32_flip_pending), the OTG frame counter (24 bit) when the
 * flip was programmed and now, the microseconds since that flip, the cap
 * (rd3_latch_cap_us). Measured from the flip only: whatever the swap did
 * before it (the hashes, flipToLocked's wait for the flip before) never
 * turns a stuck flip into anything but a timeout (ECC review round 2). */
static inline enum rd3_latch_step rd3_latch_step(int status_ok, int pending, uint32_t frame0, uint32_t frame,
                                                 uint64_t flip_us, uint64_t cap_us)
{
    if (!status_ok)
        return RD3_LATCH_UNKNOWN;
    if (!pending)
        return RD3_LATCH_DONE;
    if (((frame - frame0) & 0xffffffu) >= RD3_LATCH_FRAMES || flip_us >= cap_us)
        return RD3_LATCH_TIMEOUT;
    return RD3_LATCH_WAIT;
}

/* A head's latch state across swaps (the accelerator keeps one per head).
 * A swap waits once, right after its own flip; the next swap's entry only
 * looks (a pending flip there is counted, a latched one ends the run):
 *   latched  - the last flip was seen latched (or there is nothing to wait for)
 *   gave_up  - the last flip timed out, was not waited for or not readable
 *   run      - timeouts in a row; RD3_LATCH_TIMEOUT_RUN of them turn 'off' on
 *   off      - the head's waits are off until a flip is seen latched
 * Every transition of 'off' is returned as an event the caller logs once. */
struct rd3_latch_state {
    unsigned run;
    int off, gave_up, latched;
};
enum rd3_latch_event { RD3_LATCH_EV_NONE = 0, RD3_LATCH_EV_OFF = 1, RD3_LATCH_EV_RESUMED = 2 };

/* a new flip was programmed */
static inline void rd3_latch_flipped(struct rd3_latch_state *st)
{
    st->latched = 0;
    st->gave_up = 0;
}
/* a flip was seen latched (by a wait, right after the flip, at the next
 * swap's entry or by copy forward's look): the run ends, waits resume */
static inline enum rd3_latch_event rd3_latch_seen(struct rd3_latch_state *st)
{
    const int was_off = st->off;
    st->latched = 1;
    st->gave_up = 0;
    st->run = 0;
    st->off = 0;
    return was_off ? RD3_LATCH_EV_RESUMED : RD3_LATCH_EV_NONE;
}
/* the flip did not latch in time: given up, one more in the run */
static inline enum rd3_latch_event rd3_latch_timeout(struct rd3_latch_state *st)
{
    st->gave_up = 1;
    st->run++;
    if (!st->off && st->run >= RD3_LATCH_TIMEOUT_RUN) {
        st->off = 1;
        return RD3_LATCH_EV_OFF;
    }
    return RD3_LATCH_EV_NONE;
}
/* the flip is not waited for (state unreadable, waits off or disabled):
 * not a timeout */
static inline void rd3_latch_give_up(struct rd3_latch_state *st)
{
    st->gave_up = 1;
}
/* nothing recorded is on the panel any more (a mode switch, a copy onto the
 * scanout): every wait armed again */
static inline enum rd3_latch_event rd3_latch_rearm(struct rd3_latch_state *st)
{
    return rd3_latch_seen(st);
}
/* whether the swap waits after its own flip */
static inline int rd3_latch_wait_flip(int enabled, const struct rd3_latch_state *st)
{
    return enabled && !st->off;
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

/* a mode 0 swap whose back is the buffer the head already scans (card run
 * 2026-10-18b: 628 of 655 flips went to the address on the panel, one back
 * buffer; each still waited for its latch, median 14.8 ms, under the
 * family's lock, and WindowServer's frames came every 3rd vblank). The
 * address register written again with the same value changes nothing the
 * HUBP reads, so such a swap is not flipped and not waited for - unless the
 * flip that put the buffer there may still be pending. 'shown' the buffer
 * the head's last flip went to (0: nothing recorded), 'latched' that flip
 * was seen latched; 'scan_known' the core told what the HUBP scans
 * ('scan', SURFACE_EARLIEST_INUSE as the swap's address) and whether a flip
 * is pending ('scan_pending'). The scanned address, when known, overrides
 * the record: another buffer there means the record is stale (flip), a
 * pending flip means wait as before. amdgpu completes the swap's event from
 * dm_pflip_high_irq instead (DRIVER-PLAN N28): this is the cheap part. */
enum rd3_same_shown {
    RD3_SAME_NO = 0,       /* another buffer, nothing recorded or off: flip and wait as before */
    RD3_SAME_SKIP = 1,     /* the buffer on the panel, its flip latched: not flipped, no wait */
    RD3_SAME_PENDING = 2   /* the buffer the last flip went to, not seen latched: flip and wait as before */
};
static inline enum rd3_same_shown rd3_latch_same_shown(int enabled, uint64_t shown, int latched, int scan_known,
                                                       uint64_t scan, int scan_pending, uint64_t back)
{
    if (!enabled || !back || back != shown)
        return RD3_SAME_NO;
    if (scan_known) {
        if (scan != back)
            return RD3_SAME_NO;
        return scan_pending ? RD3_SAME_PENDING : RD3_SAME_SKIP;
    }
    return latched ? RD3_SAME_SKIP : RD3_SAME_PENDING;
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

/* the head's frame time from the core's measured refresh (milli-Hz; under
 * 1 Hz: unknown, 0) */
static inline uint32_t rd3_latch_frame_us(uint32_t refresh_mhz)
{
    return refresh_mhz >= 1000u ? (uint32_t)(1000000000ull / refresh_mhz) : 0u;
}
/* asks of a head's refresh per mode before 60 Hz is kept for good */
#define RD3_LATCH_FRAME_TRIES 8u
/* whether the refresh is asked again: not answered yet, tries left */
static inline int rd3_latch_ask_frame(int answered, unsigned tries)
{
    return !answered && tries < RD3_LATCH_FRAME_TRIES;
}

/* whether a skipped swap's empty back is read again 4 ms later (a sleep in
 * the swap): only with the latch wait on (swap_latch=0 sleeps nowhere), the
 * first 8 of a head, then every 256th; 'sample' counts the head's skips
 * from 0 */
static inline int rd3_empty_resample_due(int latch_on, uint64_t sample)
{
    return latch_on && (sample < 8u || (sample & 255u) == 0);
}

/* rate limit of the driver's log lines: the first 'first', then every
 * (mask + 1)th ('mask' a power of two minus one); k counts from 0 */
static inline int rd3_log_due(uint64_t k, uint64_t first, uint64_t mask)
{
    return k < first || (k & mask) == 0;
}

#endif
