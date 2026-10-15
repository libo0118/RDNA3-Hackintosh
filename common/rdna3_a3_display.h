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
 * off. Until the swap flips a surface's own VRAM, a WindowServer surface
 * the size of a head's scanout stays out of A3 and the plugin composites
 * it through copies, as before A3; boot-arg rdna3a3display=1 (or sysctl
 * debug.rdna3accel_a3display=1) lets it in again.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_A3_DISPLAY_H
#define RDNA3_A3_DISPLAY_H

#include <stdint.h>
#include <string.h>

#define RD3_A3_DISPLAY_PROC "WindowServer"

/* The head whose display framebuffer an IOSurface of w x h pixels looked
 * up by process 'proc' is, or -1: only WindowServer's, only the size a
 * head scans out (head_w/head_h[i], 0 = head i unknown). Another process's
 * surface of that size (a full-screen app) is its own: it keeps A3. */
static inline int rd3_a3_display_head(const char *proc, uint64_t w, uint64_t h,
                                      const volatile uint32_t *head_w, const volatile uint32_t *head_h,
                                      unsigned heads)
{
    unsigned i;
    if (!proc || strcmp(proc, RD3_A3_DISPLAY_PROC) != 0 || !w || !h || !head_w || !head_h)
        return -1;
    for (i = 0; i < heads; i++) {
        const uint32_t hw = head_w[i], hh = head_h[i];
        if (hw && hh && hw == w && hh == h)
            return (int)i;
    }
    return -1;
}

#endif
