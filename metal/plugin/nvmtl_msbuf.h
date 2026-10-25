/*
 * The byte layout of a Metal blit between a MULTISAMPLED texture and a buffer, and the word-by-word view of it that the
 * download shader (nvmtl_msdl_spv.h, msdl.comp) works in.
 *
 * Evidence (card run 2026-10-25f, Chess = Apple's GL over the plugin, every process): the stencil of a 971x680 4-sample
 * D32_S8 texture is copied to a buffer with bytesPerRow 3884 = 971 * 4 samples * 1 B, and the same buffer is copied to a
 * 4-sample R8_UINT texture with the same row. So a buffer row holds `samples` elements per pixel: element x * samples + s is
 * sample s of pixel x (the sample index is the fast axis). Nothing in the logs says more about the order inside a pixel, so
 * the plugin keeps the sample index fast on both directions: whatever Apple does between the two copies (it only resolves
 * the R8_UINT texture), the plugin's own two passes agree with each other.
 *
 * Pure C99, no Vulkan headers: tests/test_msdl.c, tests/test_msupload.c. SPDX-License-Identifier: MIT
 */
#ifndef NVMTL_MSBUF_H
#define NVMTL_MSBUF_H
#include <stdint.h>

/* byte offset of element (x, y, sample s) from byte 0 of the region in the buffer */
static inline uint64_t nvmtl_msbuf_elem_offset(uint64_t row, uint32_t bpp, uint32_t samples, uint32_t x, uint32_t y, uint32_t s)
{
    return (uint64_t)y * row + ((uint64_t)x * samples + s) * bpp;
}
/* bytes one row of a region w pixels wide fills */
static inline uint64_t nvmtl_msbuf_used(uint32_t w, uint32_t samples, uint32_t bpp) { return (uint64_t)w * samples * bpp; }
/* bytes from byte 0 of the region to the end of its last row */
static inline uint64_t nvmtl_msbuf_span(uint64_t row, uint32_t h, uint32_t w, uint32_t samples, uint32_t bpp)
{
    return (uint64_t)(h - 1) * row + nvmtl_msbuf_used(w, samples, bpp);
}
/* aligned 32-bit words that cover `span` bytes starting `lead` (0..3) bytes into the first one */
static inline uint32_t nvmtl_msbuf_words(uint32_t lead, uint64_t span) { return (uint32_t)((lead + span + 3u) / 4u); }

/* A mirror of msdl.comp's inner loop. Byte b (0..3) of aligned word i; `lead` = the region's byte 0 address & 3. Returns 1 and
 * the element it belongs to when the byte is inside the region, 0 when it is padding or outside (the shader leaves it alone). */
static inline int nvmtl_msbuf_locate(uint32_t i, uint32_t b, uint32_t lead, uint32_t row, uint32_t used, uint32_t h, uint32_t bpp,
                                     uint32_t samples, uint32_t *px, uint32_t *y, uint32_t *s, uint32_t *k)
{
    const uint32_t o = 4u * i + b;
    if (o < lead) return 0;
    const uint32_t d = o - lead, yy = d / row, j = d - yy * row;
    if (yy >= h || j >= used) return 0;
    const uint32_t e = j / bpp;
    *k = j - e * bpp; *px = e / samples; *s = e - *px * samples; *y = yy;
    return 1;
}

/* the dispatch of nwords invocations: groups of 64 along x, wrapped into y; the shader's index is gid.y * stride + gid.x */
static inline void nvmtl_msbuf_dispatch(uint32_t nwords, uint32_t *gx, uint32_t *gy, uint32_t *stride)
{
    const uint32_t groups = (nwords + 63u) / 64u;
    *gx = groups < 32768u ? (groups ? groups : 1u) : 32768u;
    *gy = (groups + *gx - 1u) / *gx;
    if (!*gy) *gy = 1u;
    *stride = *gx * 64u;
}
#endif
