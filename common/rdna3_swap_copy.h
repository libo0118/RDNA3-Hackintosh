/*
 * rdna3_swap_copy.h - a legacy surface swap as the copy Apple's family asks
 * for (card run 2026-10-17b). Plain C, no kernel headers: the accelerator
 * uses it and tests/test_swap_copy.c checks it on the host.
 *
 * IOAccelLegacySurface::submitSwapCopy(event, front, back) asks for the back
 * to be COPIED into the front - the display pipe's framebuffer resource,
 * which on Apple's drivers is what the head scans out - and the back is
 * WindowServer's again the moment the call returns. Copy forward
 * (submitCopyForward(event, fb, buffer 0 = the front, buffer 1 = the next
 * back, rects)) then refreshes the next back from the front, which holds the
 * last frame because of that copy. Until this run the accelerator flipped
 * the panel to the back instead (zero-copy, mode 0): WindowServer drew the
 * next frame into the buffer on the panel (card run 2026-10-17b: head 0
 * rewrote 85 of 229 shown frames in place), the front never held a frame
 * (so copy forward needed its own "copy from the buffer the head shows"
 * rules, rdna3_swap_front.h), and freed backs were still scanned
 * (swaps_shownfreed: 14 + 10).
 *
 * Mode 1 (the default, debug.rdna3accel_swap_mode / boot-arg rdna3swapmode)
 * copies. The front resource stopped being the scanout on our card: the
 * family maps it into the GPU page table, which the scanout's vidmem cannot
 * be, frees it and gives the resource ordinary VRAM (card run 58b:
 * 0x87040000 / 0x88060000), so the panel needs its own copy. Each head has
 * two scanout buffers of the accelerator: the framebuffer's own ("home",
 * rdna3ScanoutInfo) and one the accelerator allocates (D, the head's mode
 * size). A swap
 *   1. waits until the head's last flip latched (rd3_sc_must_wait: the
 *      buffer the copy goes into may still be scanned until then; bounded by
 *      rdna3_flip_latch.h's cap),
 *   2. copies the back into the buffer NOT on the panel (rd3_sc_target) by
 *      SDMA (rdna3CopyVram, synchronous) and flips the head to it; the HUBP
 *      takes the new address at the next vblank, so no frame shows a half
 *      copy (amdgpu: a flip of a whole new buffer, latched at vblank -
 *      dm_pflip_high_irq - never a write into the buffer being scanned),
 *   3. copies the back into the front too (WindowServer's front, never
 *      scanned in this mode), so copy forward reads the last frame where
 *      Apple's family expects it (rd3_sc_cf_source).
 * The back is free when the call returns (both copies are done). Bounded:
 * one wait of at most the latch cap (2.5 frames, 35-100 ms) and two
 * synchronous copies (~0.65 ms each for 1920x1080 on the card). Without D
 * (no VRAM for it) the copy goes into home while the panel reads it
 * (RD3_SCT_HOME_DIRECT: may tear, counted). The panel never scans a buffer
 * WindowServer owns, so no freed buffer stays on it.
 *
 * WindowServer presents backs it never drew (card run 2026-10-17b: head
 * 0's second back, 0x8b8f0000, read empty at all of its 120 swaps; the
 * family's aperture access - its page-on, the only CPU path into a legacy
 * buffer - named only the first back). Copied, each of them would put a
 * black frame over the picture every other swap. So mode 1 copies a back
 * that reads empty only when there is no picture to lose (the front and the
 * head's last copy are empty too, a black start); otherwise the panel and
 * the front keep the last frame (RD3_SC_HOLD, swaps_copyempty_h<N>, traced,
 * the first ones logged with where WindowServer's frame did land). Mode 2
 * copies everything, black frames too (Apple's literal copy, for the A/B).
 * Mode 0 is the flip of card run 2026-10-17.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_SWAP_COPY_H
#define RDNA3_SWAP_COPY_H

#include "rdna3_swap_front.h"

/* 0 flip (card run 2026-10-17), 1 copy keeping the picture over an empty
 * back (the default), 2 copy everything */
enum rd3_swap_mode { RD3_SWAPMODE_FLIP = 0, RD3_SWAPMODE_COPY = 1, RD3_SWAPMODE_COPY_ALL = 2 };
#define RD3_SWAPMODE_DEFAULT RD3_SWAPMODE_COPY
#define RD3_SWAPMODE_MAX     RD3_SWAPMODE_COPY_ALL

static inline int rd3_swapmode_valid(long v)
{
    return v >= RD3_SWAPMODE_FLIP && v <= RD3_SWAPMODE_MAX;
}

/* which argument is the back (the copy's source) and which the front (its
 * destination): 1 = a1, 2 = a2. The one that is a head's framebuffer
 * resource is the front; with none (a surface no head shows) or both,
 * submitSwapCopy(event, front, back): a1 the front, a2 the back */
struct rd3_sc_roles { int back, front, frontIsFb; };
static inline struct rd3_sc_roles rd3_sc_roles(int a1IsFb, int a2IsFb)
{
    struct rd3_sc_roles r;
    r.back = 2;
    r.front = 1;
    r.frontIsFb = a1IsFb;
    if (a2IsFb && !a1IsFb) {
        r.back = 1;
        r.front = 2;
        r.frontIsFb = 1;
    }
    return r;
}

/* what a swap does with its back */
enum rd3_sc_act {
    RD3_SC_COPY = 0,        /* the back has pixels (or could not be sampled): copied */
    RD3_SC_COPY_EMPTY = 1,  /* the back read empty: copied anyway (mode 2, or nothing to lose) */
    RD3_SC_HOLD = 2         /* mode 1: an empty back over a picture: not copied, the last frame stays */
};
/* sampled: the back's 64 samples were read (a mapping and a geometry);
 * pixels: one was not 0; picture: the front, or the head's last copy, has
 * pixels (what an empty back would overwrite) */
static inline enum rd3_sc_act rd3_sc_action(int mode, int sampled, int pixels, int picture)
{
    if (!sampled || pixels)
        return RD3_SC_COPY;
    if (mode == RD3_SWAPMODE_COPY && picture)
        return RD3_SC_HOLD;
    return RD3_SC_COPY_EMPTY;
}

/* what the head's panel shows as far as the accelerator knows */
enum rd3_sc_shows {
    RD3_SCS_OTHER = 0,   /* not the accelerator's D: home, a WindowServer buffer (mode 0), an IOP surface */
    RD3_SCS_HOME = 1,    /* home, flipped to by a mode 1/2 swap or a mode change */
    RD3_SCS_D = 2        /* the accelerator's buffer */
};
/* where the copy goes */
enum rd3_sc_target {
    RD3_SCT_NONE = 0,
    RD3_SCT_HOME = 1,        /* home, not on the panel: copied, then flipped to */
    RD3_SCT_D = 2,           /* D, not on the panel: copied, then flipped to */
    RD3_SCT_HOME_DIRECT = 3  /* no D: home while the panel may read it (may tear), then flipped to */
};
static inline enum rd3_sc_target rd3_sc_target(enum rd3_sc_shows shows, int haveD)
{
    if (!haveD)
        return RD3_SCT_HOME_DIRECT;
    return shows == RD3_SCS_D ? RD3_SCT_HOME : RD3_SCT_D;
}
/* the copy is tear-free: it goes into a buffer the panel does not read */
static inline int rd3_sc_tear_free(enum rd3_sc_target t)
{
    return t == RD3_SCT_HOME || t == RD3_SCT_D;
}
/* wait for the head's last flip before copying: it may still leave the
 * buffer the copy goes into (two buffers take turns); not seen latched yet */
static inline int rd3_sc_must_wait(enum rd3_sc_target t, int flipped, int latched)
{
    return rd3_sc_tear_free(t) && flipped && !latched;
}
/* what the panel shows after the swap's flip (done or refused) */
static inline enum rd3_sc_shows rd3_sc_after_flip(enum rd3_sc_shows before, enum rd3_sc_target t, int flipDone)
{
    if (!flipDone)
        return before;
    return t == RD3_SCT_D ? RD3_SCS_D : RD3_SCS_HOME;
}

/* D's geometry for a head showing w x h: shapeSurface's pitch rule (256
 * byte rows), 4-byte pixels */
static inline unsigned rd3_sc_pitch(unsigned w)
{
    return (w * 4u + 255u) & ~255u;
}
/* D must be (re)allocated: none yet, or another mode's size */
static inline int rd3_sc_d_stale(int haveD, unsigned dW, unsigned dH, unsigned w, unsigned h)
{
    return !haveD || dW != w || dH != h;
}
/* D may be freed now: the panel does not show it */
static inline int rd3_sc_d_freeable(enum rd3_sc_shows shows)
{
    return shows != RD3_SCS_D;
}

/* the part of a copy both sides hold: the narrower width and the lower
 * height, rows that end inside the destination's allocation; 0: nothing */
static inline int rd3_sc_extent(unsigned sw, unsigned sh, unsigned spitch, unsigned long long sbytes, unsigned dw,
                                unsigned dh, unsigned dpitch, unsigned long long dbytes, unsigned *rowBytes,
                                unsigned *rows)
{
    const unsigned w = sw < dw ? sw : dw;
    unsigned h = sh < dh ? sh : dh;
    const unsigned rb = w * 4u;
    *rowBytes = 0;
    *rows = 0;
    if (!w || !h || rb > spitch || rb > dpitch || rb > sbytes || rb > dbytes)
        return 0;
    if ((unsigned long long)(h - 1u) * spitch + rb > sbytes)
        h = (unsigned)((sbytes - rb) / spitch) + 1u;
    if ((unsigned long long)(h - 1u) * dpitch + rb > dbytes)
        h = (unsigned)((dbytes - rb) / dpitch) + 1u;
    *rowBytes = rb;
    *rows = h;
    return 1;
}

/* copy forward's source in modes 1 and 2 */
enum rd3_sc_cf {
    RD3_SCCF_SAME = 0,     /* the front and the target are one buffer: nothing to do */
    RD3_SCCF_SRC = 1,      /* the front WindowServer named holds the last frame (the swap copied it there) */
    RD3_SCCF_SCANOUT = 2,  /* the head's front missed that copy: from the scanout buffer the panel shows */
    RD3_SCCF_STALE = 3     /* neither holds it: nothing copied (counted, the rects stay pending) */
};
struct rd3_sc_cf_in {
    int srcIsDst;        /* the front and the target are one buffer */
    int srcIsHeadFront;  /* the front is a head's framebuffer resource (else: a surface no head shows) */
    int frontCurrent;    /* that head's last swap copied into this front (same allocation) */
    int scanoutKnown;    /* that head shows a scanout buffer of the accelerator, its last frame */
};
static inline enum rd3_sc_cf rd3_sc_cf_source(const struct rd3_sc_cf_in *in)
{
    if (!in || in->srcIsDst)
        return RD3_SCCF_SAME;
    if (!in->srcIsHeadFront || in->frontCurrent)
        return RD3_SCCF_SRC;
    return in->scanoutKnown ? RD3_SCCF_SCANOUT : RD3_SCCF_STALE;
}

#endif
