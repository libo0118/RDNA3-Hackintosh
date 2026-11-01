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

/* DRIVER-PLAN N71 (c): an application's VRAM drawable that WindowServer
 * imports READ-ONLY and samples directly. Card run 58 / 58b went wrong
 * because WindowServer WROTE into shared VRAM (its CPU surface-in uploaded
 * stale system memory over what other processes drew); reading it never
 * was the problem. The producer (an application holding an A3 import of
 * the surface) records a share of one allocation generation after its GPU
 * finished the frame; WindowServer's import of that same generation is
 * then a read-only BO (no CPU mapping, no writable GPU page) instead of a
 * refusal. Generations (RD3VidMemory::fAllocGen) are never 0 and never
 * reused, so a record outlives nothing: a freed or re-backed allocation has
 * another generation and the record no longer matches. */
#define RD3_A3_WS_UID 88u   /* _windowserver */

/* WindowServer for the shared import: its name AND uid 88 (a process that
 * is merely named WindowServer gets the old refusal) */
static inline int rd3_a3_ws_identity(int name_is_ws, uint32_t uid)
{
    return name_is_ws && uid == RD3_A3_WS_UID;
}

/* a record matches the live allocation: the same generation, never 0 */
static inline int rd3_a3_share_matches(uint64_t share_gen, uint64_t cur_gen)
{
    return share_gen != 0 && share_gen == cur_gen;
}

/* may the caller record a share: not WindowServer (is_ws: the name OR uid
 * 88, the stricter test), and its own A3 import (import_gen, the
 * generation its BO was made over) is of the allocation the surface has
 * now (cur_gen) */
static inline int rd3_a3_share_may_record(int is_ws, uint64_t import_gen, uint64_t cur_gen)
{
    return !is_ws && rd3_a3_share_matches(import_gen, cur_gen);
}

/* What A3 does with an import (card run 58): RD3_A3_ALLOW, or the reason it
 * stays out. is_ws: the importer is WindowServer; display_heads: the heads
 * whose display framebuffer the surface may be (rd3_a3_display_heads);
 * display_on: rdna3a3display=1 (or a display-pipe flip of the surface's own
 * VRAM); ws_on: rdna3a3ws=1. Only WindowServer's imports are ever refused.
 * N71 (c): share_on (debug.rdna3accel_a3share, a caller that can make the
 * BO read-only, WindowServer by name and uid): a share record of the
 * current allocation (share_gen == cur_gen) gives RD3_A3_ALLOW_SHARED, a
 * read-only import, before the display / ws refusals; share_on 0 is the
 * card run 58 table exactly. */
enum { RD3_A3_ALLOW = 0, RD3_A3_REFUSE_DISPLAY = 1, RD3_A3_REFUSE_WS = 2, RD3_A3_ALLOW_SHARED = 3 };
static inline int rd3_a3_ws_policy(int is_ws, unsigned display_heads, int display_on, int ws_on, int share_on,
                                   uint64_t share_gen, uint64_t cur_gen)
{
    if (!is_ws)
        return RD3_A3_ALLOW;
    if (share_on && rd3_a3_share_matches(share_gen, cur_gen))
        return RD3_A3_ALLOW_SHARED;
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

/* The same per band of the picture (h, h1, h2: one hash per band, 'bands'
 * of them): a menu or a status item alternating in its own band counts
 * even when the rest stands still. 'n' is the number of hashes in a row
 * taken so far (the newest included); a skipped swap starts it again. */
static inline unsigned rd3_flicker_abab_bands(const uint32_t *h, const uint32_t *h1, const uint32_t *h2, unsigned bands,
                                              uint64_t n)
{
    unsigned i, k = 0;
    for (i = 0; h && h1 && h2 && i < bands; i++)
        k += rd3_flicker_abab(h[i], h1[i], h2[i], n) ? 1u : 0u;
    return k;   /* how many bands alternate: one is a caret or a small item, more a picture */
}
static inline int rd3_flicker_abab_any(const uint32_t *h, const uint32_t *h1, const uint32_t *h2, unsigned bands,
                                       uint64_t n)
{
    return rd3_flicker_abab_bands(h, h1, h2, bands, n) > 0;
}

/* the rows of band 'band' (of 'bands', the first one the top 32 rows: the
 * menu bar at 1x and 2x) of a picture 'height' rows high that a flicker
 * hash samples: 'rows' of them, written to out[]; returns how many */
static inline unsigned rd3_flicker_band_rows(unsigned band, unsigned bands, unsigned height, unsigned rows,
                                             unsigned *out)
{
    unsigned top, len, i, k = 0;
    if (!bands || band >= bands || !height || !rows || !out)
        return 0;
    if (band == 0) {
        top = 0;
        len = height < 32 ? height : 32;
    } else {
        const unsigned rest = height > 32 ? height - 32 : 0;
        top = 32 + (unsigned)(((uint64_t)rest * (band - 1)) / (bands - 1));
        len = 32 + (unsigned)(((uint64_t)rest * band) / (bands - 1)) - top;
    }
    for (i = 0; i < rows && len; i++) {
        const unsigned y = top + (unsigned)(((uint64_t)len * (2 * i + 1)) / (2 * rows));
        if (y < height)
            out[k++] = y;
    }
    return k;
}

#endif
