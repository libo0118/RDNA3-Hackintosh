/*
 * rdna3_swap_front.h - which buffer of a WindowServer legacy surface swap
 * goes to the panel, and where copy forward takes the last frame from.
 * Plain C, no kernel headers: the accelerator uses it and
 * tests/test_swap_front.c checks it on the host.
 *
 * IOAccelLegacySurface's model (probe runs 7-10, card runs 54-58): buffer 0
 * of a display surface is the front, the display pipe's framebuffer
 * resource (RD3DisplayPipe::initFramebufferResource; after the boot its own
 * VRAM, 0x87040000 / 0x88060000 in card run 58b). Buffers 1 and 2 (shape
 * mode 0x6) are the backs WindowServer draws into. submitSwapCopy(event,
 * front, back) asks for the back to be copied into the front, which the
 * panel shows; copy_forward then copies the regions the last frame changed
 * and this one does not redraw from the front (holding the last frame) into
 * the next back. The accelerator flips the panel to the back itself
 * instead (zero-copy, rd3AccelFlipTo), so the front never holds a frame:
 *
 * - the last frame is in the buffer the head showed last. With one back
 *   (the login window) that is the back copy forward writes to, and nothing
 *   needs copying (card run 56). With two backs taking turns (the user's
 *   WindowServer after login, 2026-10-15b..16) it is the other back: card
 *   run 58b skipped those copies ("the front buffer was never shown"), so
 *   each back kept what changed two frames before, and the panel showed
 *   the two in turns - A B A B on both screens (flicker: head 1, 120 of 204
 *   hashed swaps, 6 of 8 bands). rd3_cf_source copies from the shown buffer.
 * - the source of a swap is the back. The front was shown only when the
 *   back read empty (rd3AccelResHasContent's samples) and the front did
 *   not: a frame the panel should never get in this scheme. A back that
 *   keeps reading empty is a black or clear frame (a fade, the lock
 *   screen): the third swap of it in a row goes to the panel.
 * - copy forward copies only between the calling surface's own buffers
 *   (ECC review of 60526e4): the buffer a head shows may be another
 *   surface's, another process's frame - never a source for this one.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_SWAP_FRONT_H
#define RDNA3_SWAP_FRONT_H

/* the swap's buffer that goes to the panel */
enum rd3_swap_pick { RD3_SWAP_SKIP = 0, RD3_SWAP_A1 = 1, RD3_SWAP_A2 = 2 };

/* the swaps in a row of one empty back after which it goes to the panel */
#define RD3_SWAP_EMPTY_RUN 3u

/* a1IsFront / a2IsFront: that argument is a head's framebuffer resource
 * (the front); a1Content / a2Content: rd3AccelResContent found pixels;
 * emptyRun: the swaps of this same empty back skipped in a row before this
 * one. With the front known the other argument is the back and the only
 * candidate: an empty back is skipped (nothing drawn yet) until it has
 * been swapped RD3_SWAP_EMPTY_RUN times in a row (a black or clear frame),
 * never replaced by the front. Without (or with two fronts) the older rule:
 * the argument that has content, a2 first (probe runs: a2 held the
 * composited frame). */
static inline enum rd3_swap_pick rd3_swap_source(int a1IsFront, int a2IsFront, int a1Content, int a2Content,
                                                 unsigned emptyRun)
{
    if (a1IsFront && !a2IsFront)
        return a2Content || emptyRun + 1u >= RD3_SWAP_EMPTY_RUN ? RD3_SWAP_A2 : RD3_SWAP_SKIP;
    if (a2IsFront && !a1IsFront)
        return a1Content || emptyRun + 1u >= RD3_SWAP_EMPTY_RUN ? RD3_SWAP_A1 : RD3_SWAP_SKIP;
    if (a2Content)
        return RD3_SWAP_A2;
    if (a1Content)
        return RD3_SWAP_A1;
    return RD3_SWAP_SKIP;
}

/* where copy forward copies from */
enum rd3_cf_pick {
    RD3_CF_SKIP_SAME = 0,       /* source and target are one buffer */
    RD3_CF_COPY_SRC,            /* the front WindowServer named holds the last frame (it was shown) */
    RD3_CF_COPY_SHOWN,          /* the surface's own buffer the head showed last holds it: copy from there */
    RD3_CF_SKIP_BACK_IS_LAST,   /* the target is the buffer the head showed last: nothing to bring forward */
    RD3_CF_SKIP_NO_HEAD,        /* no head known and the front never shown: nothing to copy from */
    RD3_CF_SKIP_NOTHING_SHOWN,  /* the head is known but shows nothing recorded (before its first swap, after a
                                   free or a failed flip): the front is not the last frame either */
    RD3_CF_SKIP_FOREIGN         /* the head shows a buffer that is not this surface's: never a source */
};

struct rd3_cf_in {
    int srcIsDst;     /* the front and the back are one buffer */
    int headKnown;    /* the surface's head is known */
    int headShown;    /* that head has a record of the buffer it shows */
    int shownIsDst;   /* that buffer is the target */
    int shownIsSrc;   /* that buffer is the front WindowServer named */
    int shownOwned;   /* that buffer is one of the calling surface's own */
    int srcShown;     /* the front is recorded as shown on some head (used when no head is known) */
};

static inline enum rd3_cf_pick rd3_cf_source(const struct rd3_cf_in *in)
{
    if (!in || in->srcIsDst)
        return RD3_CF_SKIP_SAME;
    if (in->headKnown) {
        if (!in->headShown)
            return RD3_CF_SKIP_NOTHING_SHOWN;
        if (in->shownIsDst)
            return RD3_CF_SKIP_BACK_IS_LAST;
        if (in->shownIsSrc)
            return RD3_CF_COPY_SRC;
        return in->shownOwned ? RD3_CF_COPY_SHOWN : RD3_CF_SKIP_FOREIGN;
    }
    return in->srcShown ? RD3_CF_COPY_SRC : RD3_CF_SKIP_NO_HEAD;
}

/* A copy that left rects stale in its target: copy forward moves only the
 * regions the last frame changed, so a missed region stays stale in that
 * buffer until it is redrawn, and shows in turns with the other back (the
 * A B A B again). The target is remembered; the next copy forward into it
 * copies the whole picture from the shown buffer (rd3_cf_full), so one
 * miss costs one frame. The miss is not reported to CoreDisplay: on
 * kIOReturnUnsupported it switches copy forward off for good ("copy
 * forward failed ... disabling", card run 54) and keeps drawing only the
 * damage, which would leave every later frame stale; it is counted and
 * logged instead (debug.rdna3accel_cf_miss_h<N>, cf_healed_h<N>). */

/* copy the whole picture instead of the rects: the target missed a copy */
static inline int rd3_cf_full(int dstMissedCopy)
{
    return dstMissedCopy != 0;
}

#endif
