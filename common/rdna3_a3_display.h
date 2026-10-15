/*
 * rdna3_a3_display.h - the IOSurfaces A3 keeps out: WindowServer's display
 * framebuffers (card run 58). Plain C, no kernel headers: the accelerator
 * uses it and tests/test_a3_display.c checks it on the host.
 *
 * Card run 2026-10-15c (the first with the A3 plugin): WindowServer's
 * 1920x1080 compositing IOSurfaces became GEM BOs over their own VRAM
 * ("GEM: IOSurface 1..4 ... at 0xd0f0000 / 0xd8e0000 / 0xe0d0000 /
 * 0xe8d0000", allocated by the import). The legacy surface swap scans out
 * other allocations (the IOAccelSurface's buffers, 0xb100000, 0xc0e0000,
 * 0xc8d0000: no flip ever went to an A3 BO); the family fills them from the
 * IOSurface's system memory, which "surface-out -> dirty, no copy" no
 * longer refreshed. The main screen kept an old frame (the wallpaper and
 * the menu bar) and showed a new one only when a CPU lock paged the VRAM
 * off. Until the legacy swap flips a surface's own VRAM, a WindowServer
 * surface the size of a head's scanout (asked of the framebuffers at the
 * import, and the size of the head's own swaps) stays out of A3 and the
 * plugin composites it through copies, as before A3; boot-arg
 * rdna3a3display=1 (or sysctl debug.rdna3accel_a3display=1) lets it in
 * again. The process is all the kernel knows of a surface: an application's
 * full-screen surface that WindowServer imports is refused too (WindowServer
 * then reads it through copies; DRIVER-PLAN N27).
 *
 * Card run 2026-10-15d (with the rule above): windows and menus flickered on
 * both screens. WindowServer had imported 615 of its other inputs (window
 * backing stores, the menu bar, status items: "WindowServer surface 3
 * 1920x24 is not a display framebuffer") as BOs; their writers mix the GPU
 * (plugin apps, through A3), the CPU (apps the plugin does not serve, CA's
 * software paths) and the plugin's per-process copies, and nothing orders
 * the VRAM copy WindowServer samples with the newest one. Until that
 * protocol is proven on the card, every WindowServer import stays out of A3
 * (rd3_a3_ws_policy): A3 serves an application's own surfaces and the ones
 * it shares with other applications; WindowServer reads them through
 * copies, which a CPU lock pages off (Apple's page-off), as before A3.
 * Boot-arg rdna3a3ws=1 (sysctl debug.rdna3accel_a3ws=1) lets WindowServer's
 * non-display inputs in again; rdna3a3=0 turns A3 off for every process.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_A3_DISPLAY_H
#define RDNA3_A3_DISPLAY_H

#include <stdint.h>
#include <string.h>

#define RD3_A3_DISPLAY_PROC "WindowServer"

/* the process whose display framebuffers these are (an empty or other name: not) */
static inline int rd3_a3_display_proc(const char *proc)
{
    return proc && strcmp(proc, RD3_A3_DISPLAY_PROC) == 0;
}

/* bit i: head i scans out w x h (head_w/head_h[i], 0 = head i unknown).
 * Two heads of one size both match: the surface may be either's. */
static inline unsigned rd3_a3_size_heads(uint64_t w, uint64_t h, const volatile uint32_t *head_w,
                                         const volatile uint32_t *head_h, unsigned heads)
{
    unsigned i, mask = 0;
    if (!w || !h || !head_w || !head_h)
        return 0;
    for (i = 0; i < heads && i < 32; i++) {
        const uint32_t hw = head_w[i], hh = head_h[i];
        if (hw && hh && hw == w && hh == h)
            mask |= 1u << i;
    }
    return mask;
}

/* The heads whose display framebuffer an IOSurface of w x h pixels looked
 * up by process 'proc' may be (a bit mask, 0 = none): only WindowServer's,
 * only the size a head scans out. Another process's surface of that size
 * (a full-screen app) is its own: it keeps A3. */
static inline unsigned rd3_a3_display_heads(const char *proc, uint64_t w, uint64_t h,
                                            const volatile uint32_t *head_w, const volatile uint32_t *head_h,
                                            unsigned heads)
{
    if (!rd3_a3_display_proc(proc))
        return 0;
    return rd3_a3_size_heads(w, h, head_w, head_h, heads);
}

/* What A3 does with an import (card run 58): RD3_A3_ALLOW, or the reason it
 * stays out. is_ws: the importer is WindowServer; display_heads: the heads
 * whose display framebuffer the surface may be (rd3_a3_display_heads);
 * display_on: rdna3a3display=1 (or a display-pipe flip of the surface's own
 * VRAM); ws_on: rdna3a3ws=1. Only WindowServer's imports are ever refused. */
enum { RD3_A3_ALLOW = 0, RD3_A3_REFUSE_DISPLAY = 1, RD3_A3_REFUSE_WS = 2 };
static inline int rd3_a3_ws_policy(int is_ws, unsigned display_heads, int display_on, int ws_on)
{
    if (!is_ws)
        return RD3_A3_ALLOW;
    if (display_heads)
        return display_on ? RD3_A3_ALLOW : RD3_A3_REFUSE_DISPLAY;
    return ws_on ? RD3_A3_ALLOW : RD3_A3_REFUSE_WS;
}

/* card run 58 (2026-10-15d): a head's legacy swaps showing the picture of
 * two swaps before, not the one before (A B A B), is the flicker of two
 * buffers with different contents. 'h' is the sample hash of the newest
 * swap, h1 and h2 the two before it; n the head's swaps so far. */
static inline int rd3_flicker_abab(uint32_t h, uint32_t h1, uint32_t h2, uint64_t n)
{
    return n >= 3 && h == h2 && h != h1;
}

#endif
