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
 *   back read empty (rd3AccelResHasContent's 16 samples) and the front did
 *   not: a frame the panel should never get in this scheme.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_SWAP_FRONT_H
#define RDNA3_SWAP_FRONT_H

/* the swap's buffer that goes to the panel */
enum rd3_swap_pick { RD3_SWAP_SKIP = 0, RD3_SWAP_A1 = 1, RD3_SWAP_A2 = 2 };

/* a1IsFront / a2IsFront: that argument is a head's framebuffer resource
 * (the front); a1Content / a2Content: rd3AccelResHasContent. With the front
 * known the other argument is the back and the only candidate: an empty
 * back (nothing drawn yet) is skipped, never replaced by the front. Without
 * (or with two fronts) the older rule: the argument that has content, a2
 * first (probe runs: a2 held the composited frame). */
static inline enum rd3_swap_pick rd3_swap_source(int a1IsFront, int a2IsFront, int a1Content, int a2Content)
{
    if (a1IsFront && !a2IsFront)
        return a2Content ? RD3_SWAP_A2 : RD3_SWAP_SKIP;
    if (a2IsFront && !a1IsFront)
        return a1Content ? RD3_SWAP_A1 : RD3_SWAP_SKIP;
    if (a2Content)
        return RD3_SWAP_A2;
    if (a1Content)
        return RD3_SWAP_A1;
    return RD3_SWAP_SKIP;
}

/* where copy forward copies from */
enum rd3_cf_pick {
    RD3_CF_SKIP_SAME = 0,      /* source and target are one buffer */
    RD3_CF_COPY_SRC,           /* the front WindowServer named holds the last frame (it was shown) */
    RD3_CF_COPY_SHOWN,         /* the buffer the head showed last holds it: copy from there */
    RD3_CF_SKIP_BACK_IS_LAST,  /* the target is the buffer the head showed last: nothing to bring forward */
    RD3_CF_SKIP_NOT_SHOWN      /* no head known and the front was never shown: nothing to copy from */
};

/* srcIsDst: the front and the back are one buffer; srcShown: the front went
 * to some panel; headKnown: the surface's head is known and has shown a
 * buffer of its own; shownIsDst / shownIsSrc: that buffer is the back / the
 * front. The head's last shown buffer is the truth when known. */
static inline enum rd3_cf_pick rd3_cf_source(int srcIsDst, int srcShown, int headKnown, int shownIsDst, int shownIsSrc)
{
    if (srcIsDst)
        return RD3_CF_SKIP_SAME;
    if (headKnown) {
        if (shownIsDst)
            return RD3_CF_SKIP_BACK_IS_LAST;
        return shownIsSrc ? RD3_CF_COPY_SRC : RD3_CF_COPY_SHOWN;
    }
    return srcShown ? RD3_CF_COPY_SRC : RD3_CF_SKIP_NOT_SHOWN;
}

#endif
