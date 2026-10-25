/*
 * Metal blit copyFromTexture:toBuffer: out of a MULTISAMPLED texture.
 *
 * Vulkan has no multisample image -> buffer copy (vkCmdCopyImageToBuffer needs 1 sample), and until this pass the plugin dropped
 * the copy: card run 2026-10-25f, Chess (Apple's GL over the plugin), every process, 8 to 16 times per minute:
 *   copy_image_to_buffer: REFUSED - the image is multisampled (vk format 130, 4 sample(s), type 4, aspect option 4, ...)
 * Format 130 is D32_SFLOAT_S8_UINT, aspect option 4 is the STENCIL of it. That copy is the first of five steps with which
 * Apple's GL resolves the stencil of a multisample framebuffer (glBlitFramebuffer, Metal has no stencil resolve in a blit):
 *   1. blit the 4-sample stencil to a buffer (bytesPerRow 3884 = 971 * 4 samples * 1 B)             <- this pass
 *   2. blit the buffer into a 4-sample R8_UINT texture (the same row)                                <- nvmtl_msupload.h
 *   3. render pass with a RESOLVE attachment: the R8_UINT texture -> a 1-sample R8_UINT texture (integer: sample 0)
 *   4. blit that texture to a buffer (971 B rows), 5. blit the buffer into the 1-sample stencil.
 * With step 1 dropped, 2 refused (R8_UINT was not a carried format) and the others unchanged, the 1-sample stencil received a
 * resolve of uninitialised memory every frame: Chess's cyan streaks on the pieces and red specks (the board reflections are
 * stencil-masked), in every rung of the A/B ladder because no scheduling switch can bring back a copy that is never made.
 *
 * The pass is a compute shader (nvmtl_msdl_spv.h) that reads the texture through a SAMPLED view of the right aspect
 * (texelFetch with a sample index: stencil through a usampler2DMS view of the stencil aspect, depth through sampler2DMS, colour
 * through a UNORM twin view) and writes the bytes to the buffer in the layout of nvmtl_msbuf.h. This file is the decision: which
 * images, aspects and regions it carries and the shader mode they need. Everything else is refused with the reason.
 *
 * Pure C99, no Vulkan headers (numeric VkFormat values): tests/test_msdl.c. SPDX-License-Identifier: MIT
 */
#ifndef NVMTL_MSDL_H
#define NVMTL_MSDL_H
#include <stdint.h>
#include "nvmtl_msbuf.h"

/* msdl.comp modes: float module ... */
enum { NVMTL_MSDL_D32F = 0, NVMTL_MSDL_D16 = 1, NVMTL_MSDL_RGBA8 = 2, NVMTL_MSDL_BGRA8 = 3, NVMTL_MSDL_R8 = 4, NVMTL_MSDL_RG8 = 5 };
/* ... and unsigned-integer module (stencil, R8_UINT ...) */
enum { NVMTL_MSDL_U8 = 0, NVMTL_MSDL_U8X2 = 1, NVMTL_MSDL_U8X4 = 2, NVMTL_MSDL_U16 = 3, NVMTL_MSDL_U32 = 4 };
enum { NVMTL_MSDL_ASPECT_COLOR = 1, NVMTL_MSDL_ASPECT_DEPTH = 2, NVMTL_MSDL_ASPECT_STENCIL = 4 };

typedef struct {
    uint32_t view_fmt;   /* VkFormat of the sampled view (the image's own for depth/stencil, the UNORM twin of an sRGB colour image) */
    uint32_t aspect;     /* NVMTL_MSDL_ASPECT_*: the aspect the view has */
    uint32_t mode;       /* msdl.comp mode */
    uint32_t uint_in;    /* 1: the integer module (stencil, R8_UINT ...) */
    uint32_t bpp;        /* bytes per element in the buffer */
    uint64_t row;        /* bytes per buffer row (row_bytes, or one tight row when it was 0) */
    uint64_t used;       /* bytes of a row the region fills = w * samples * bpp */
    uint64_t span;       /* bytes from byte 0 of the region to the end of its last row */
} nvmtl_msdl_plan;

/* The format/aspect part of the decision, shared with the census line. aspect_override: the blit option's aspect (2 depth,
 * 4 stencil, 0 none = depth of a depth/stencil format, else colour). */
static inline int nvmtl_msdl_format(uint32_t fmt, uint32_t aspect_override, uint32_t *view_fmt, uint32_t *aspect, uint32_t *mode,
                                    uint32_t *uint_in, uint32_t *bpp, const char **why)
{
    uint32_t has;
    switch (fmt) {
    case 124: case 125: case 126: has = NVMTL_MSDL_ASPECT_DEPTH; break;
    case 127: has = NVMTL_MSDL_ASPECT_STENCIL; break;
    case 128: case 129: case 130: has = NVMTL_MSDL_ASPECT_DEPTH | NVMTL_MSDL_ASPECT_STENCIL; break;
    default: has = NVMTL_MSDL_ASPECT_COLOR; break;
    }
    uint32_t a;
    if (aspect_override) {
        if (aspect_override & (aspect_override - 1)) { *why = "the blit options name more than one aspect"; return -1; }
        if (aspect_override & ~has) { *why = "the blit option names an aspect this format does not have"; return -1; }
        a = aspect_override;
    } else a = (has & NVMTL_MSDL_ASPECT_DEPTH) ? NVMTL_MSDL_ASPECT_DEPTH : has;
    *aspect = a; *view_fmt = fmt; *uint_in = 0;
    if (a == NVMTL_MSDL_ASPECT_STENCIL) { *mode = NVMTL_MSDL_U8; *uint_in = 1; *bpp = 1; return 0; }
    if (a == NVMTL_MSDL_ASPECT_DEPTH) {
        switch (fmt) {
        case 126: case 130: *mode = NVMTL_MSDL_D32F; *bpp = 4; return 0;
        case 124: case 128: *mode = NVMTL_MSDL_D16; *bpp = 2; return 0;
        default: *why = "the 24-bit depth of this format has no byte layout in a Metal blit"; return -1;
        }
    }
    switch (fmt) {
    case 44: case 50: *view_fmt = 44; *mode = NVMTL_MSDL_BGRA8; *bpp = 4; return 0;   /* B8G8R8A8 UNORM / SRGB */
    case 37: case 43: *view_fmt = 37; *mode = NVMTL_MSDL_RGBA8; *bpp = 4; return 0;   /* R8G8B8A8 UNORM / SRGB */
    case 9:  case 15: *view_fmt = 9;  *mode = NVMTL_MSDL_R8;    *bpp = 1; return 0;   /* R8 UNORM / SRGB */
    case 16: *view_fmt = 16; *mode = NVMTL_MSDL_RG8; *bpp = 2; return 0;              /* R8G8 UNORM */
    case 13: *mode = NVMTL_MSDL_U8;   *uint_in = 1; *bpp = 1; return 0;               /* R8_UINT */
    case 20: *mode = NVMTL_MSDL_U8X2; *uint_in = 1; *bpp = 2; return 0;               /* R8G8_UINT */
    case 41: *mode = NVMTL_MSDL_U8X4; *uint_in = 1; *bpp = 4; return 0;               /* R8G8B8A8_UINT */
    case 74: *mode = NVMTL_MSDL_U16;  *uint_in = 1; *bpp = 2; return 0;               /* R16_UINT */
    case 98: *mode = NVMTL_MSDL_U32;  *uint_in = 1; *bpp = 4; return 0;               /* R32_UINT */
    default: *why = "this colour format is not carried by the multisample download pass (8-bit RGBA/BGRA/R/RG UNORM or sRGB, R8/RG8/RGBA8/R16/R32 UINT only)"; return -1;
    }
}

/* Returns 0 and fills *plan, or -1 and *why (a static string). img_* describe the source image (mtl_type 4 = 2D multisample,
 * 8 = array), level/layer the Metal source subresource, x/y/w/h the region, row_bytes/off/buf_size the destination buffer. */
static inline int nvmtl_msdl_plan_make(uint32_t img_fmt, uint32_t samples, uint32_t mtl_type, uint32_t layers, uint32_t mips,
                                       uint32_t level, uint32_t layer, uint32_t aspect_override,
                                       uint32_t img_w, uint32_t img_h, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                       uint64_t row_bytes, uint64_t off, uint64_t buf_size,
                                       nvmtl_msdl_plan *plan, const char **why)
{
    uint32_t view, aspect, mode, u, bpp;
    if (samples != 2 && samples != 4 && samples != 8) { *why = "not a 2, 4 or 8 sample image"; return -1; }
    if (mtl_type != 4) { *why = "only a 2D multisample texture is carried (not a multisample array)"; return -1; }
    if (layers > 1 || mips > 1 || level != 0 || layer != 0) { *why = "only level 0 slice 0 of a multisample texture exists"; return -1; }
    if (nvmtl_msdl_format(img_fmt, aspect_override, &view, &aspect, &mode, &u, &bpp, why)) return -1;
    if (!w || !h || !img_w || !img_h || x >= img_w || y >= img_h || w > img_w - x || h > img_h - y) { *why = "the region is empty or outside the image"; return -1; }
    const uint64_t used = nvmtl_msbuf_used(w, samples, bpp);
    if (!row_bytes) row_bytes = used;
    if (row_bytes < used) { *why = "bytesPerRow is smaller than one row of the region (width * samples * bytes per element)"; return -1; }
    if (row_bytes >= 0x80000000ull) { *why = "bytesPerRow is 2 GiB or more"; return -1; }
    const uint64_t span = nvmtl_msbuf_span(row_bytes, h, w, samples, bpp);
    if (span >= 0x80000000ull) { *why = "the region is 2 GiB or more"; return -1; }
    if (off > buf_size || span > buf_size - off) { *why = "the region writes past the end of the buffer"; return -1; }
    /* the shader stores whole 32-bit words (a buffer's address is aligned to at least 4): the last word touched must be inside it */
    if (((off + span + 3u) & ~(uint64_t)3u) > buf_size) { *why = "the region's last 32-bit word is not inside the buffer (its size is not a multiple of 4)"; return -1; }
    plan->view_fmt = view; plan->aspect = aspect; plan->mode = mode; plan->uint_in = u; plan->bpp = bpp;
    plan->row = row_bytes; plan->used = used; plan->span = span;
    return 0;
}
#endif
