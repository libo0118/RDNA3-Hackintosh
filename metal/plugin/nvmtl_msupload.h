/*
 * Metal blit copyFromBuffer:toTexture: into a MULTISAMPLED colour texture.
 *
 * Vulkan has no buffer -> multisampled image copy: vkCmdCopyBufferToImage needs a 1-sample image and RADV asserts
 * (`assert(image->vk.samples == 1)` in gfx_or_compute_copy_memory_to_image; card run 2026-10-25c: Chess aborted three
 * times in glReadPixels -> gldBlitFramebufferData -> copyFromBuffer:toTexture: on a 4-sample 971x680 texture). Apple's GL
 * renderer does exactly that upload, so the plugin writes the region itself: a full-screen-triangle pass at the image's sample
 * count whose fragment shader reads the element's bytes from the buffer (by device address) and writes it (checked on RADV with
 * 2, 4 and 8 samples, sRGB through a UNORM view: tests/vk_msupload_probe.c).
 *
 * What the card showed (run 2026-10-25f): the upload Chess makes is into a 4-sample R8_UINT texture, bytesPerRow 3884 =
 * 971 * 4 samples * 1 B, from the buffer that the stencil of a 4-sample D32_S8 texture was copied to (nvmtl_msbuf.h). So a row
 * holds `samples` elements per pixel and the pass writes sample s from element x * samples + s (one draw per sample, the
 * fragment shader restricts its coverage to that sample). A row too short for that (a single-sample layout) falls back to the
 * first version: every sample of a pixel gets the element at x.
 *
 * This file is the decision: which images and regions the pass carries, the colour mode and the view format it needs.
 * Everything else is refused with the reason (a log line in the plugin). Pure C99, no Vulkan headers (numeric VkFormat
 * values): tests/test_msupload.c. SPDX-License-Identifier: MIT
 */
#ifndef NVMTL_MSUPLOAD_H
#define NVMTL_MSUPLOAD_H
#include <stdint.h>
#include "nvmtl_msbuf.h"

/* the fragment shader's mode: float output (msu.frag) ... */
enum { NVMTL_MSU_RGBA8 = 0, NVMTL_MSU_BGRA8 = 1, NVMTL_MSU_R8 = 2, NVMTL_MSU_RG8 = 3 };
/* ... and unsigned-integer output (msu.frag -DUINT) */
enum { NVMTL_MSU_U8 = 0, NVMTL_MSU_U8X2 = 1, NVMTL_MSU_U8X4 = 2, NVMTL_MSU_U16 = 3, NVMTL_MSU_U32 = 4 };

typedef struct {
    uint32_t view_fmt;   /* VkFormat of the attachment view (the UNORM twin of an sRGB image: the bytes are copied, not converted) */
    uint32_t mode;       /* NVMTL_MSU_*: how the shader orders the bytes */
    uint32_t uint_out;   /* 1: the integer shader module (R8_UINT ...), 0: the float one */
    uint32_t bpp;        /* bytes per element in the buffer */
    uint32_t inter;      /* 1: a row holds `samples` elements per pixel (one draw per sample); 0: one element per pixel for every sample */
    uint64_t row;        /* bytes per row of the buffer (row_bytes, or one tight row when it was 0) */
} nvmtl_msu_plan;

/* Returns 0 and fills *plan, or -1 and *why. img_* describe the destination (mtl_type 4 = 2D multisample, 8 = array),
 * level/layer the Metal destination subresource, aspect_override the blit option's aspect (0 = none), x/y/w/h the region,
 * row_bytes/off/buf_size the source buffer. */
static inline int nvmtl_msu_plan_make(uint32_t img_fmt, uint32_t samples, uint32_t mtl_type, uint32_t layers, uint32_t mips,
                                      uint32_t level, uint32_t layer, uint32_t aspect_override,
                                      uint32_t img_w, uint32_t img_h, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                      uint64_t row_bytes, uint64_t off, uint64_t buf_size,
                                      nvmtl_msu_plan *plan, const char **why)
{
    uint32_t view, mode, bpp, u = 0;
    if (samples != 2 && samples != 4 && samples != 8) { *why = "not a 2, 4 or 8 sample image"; return -1; }
    if (mtl_type != 4) { *why = "only a 2D multisample texture is carried (not a multisample array)"; return -1; }
    if (layers > 1 || mips > 1 || level != 0 || layer != 0) { *why = "only level 0 slice 0 of a multisample texture exists"; return -1; }
    if (aspect_override) { *why = "a depth/stencil blit option on a multisample colour texture"; return -1; }
    switch (img_fmt) {
    case 44: case 50: view = 44; mode = NVMTL_MSU_BGRA8; bpp = 4; break;   /* B8G8R8A8 UNORM / SRGB */
    case 37: case 43: view = 37; mode = NVMTL_MSU_RGBA8; bpp = 4; break;   /* R8G8B8A8 UNORM / SRGB */
    case 9:  case 15: view = 9;  mode = NVMTL_MSU_R8;    bpp = 1; break;   /* R8 UNORM / SRGB */
    case 16:          view = 16; mode = NVMTL_MSU_RG8;   bpp = 2; break;   /* R8G8 UNORM */
    case 13: view = 13; mode = NVMTL_MSU_U8;   bpp = 1; u = 1; break;      /* R8_UINT (Chess: the stencil resolve) */
    case 20: view = 20; mode = NVMTL_MSU_U8X2; bpp = 2; u = 1; break;      /* R8G8_UINT */
    case 41: view = 41; mode = NVMTL_MSU_U8X4; bpp = 4; u = 1; break;      /* R8G8B8A8_UINT */
    case 74: view = 74; mode = NVMTL_MSU_U16;  bpp = 2; u = 1; break;      /* R16_UINT */
    case 98: view = 98; mode = NVMTL_MSU_U32;  bpp = 4; u = 1; break;      /* R32_UINT */
    case 124: case 125: case 126: case 127: case 128: case 129: case 130:
        *why = "a multisample DEPTH/STENCIL destination is not carried (needs a depth-export variant of the pass)"; return -1;
    default: *why = "this colour format is not carried by the multisample upload pass (8-bit RGBA/BGRA/R/RG UNORM or sRGB, R8/RG8/RGBA8/R16/R32 UINT only)"; return -1;
    }
    if (!w || !h || !img_w || !img_h || x >= img_w || y >= img_h || w > img_w - x || h > img_h - y) { *why = "the region is empty or outside the image"; return -1; }
    const uint64_t used = nvmtl_msbuf_used(w, samples, bpp);
    uint32_t inter = 1;
    if (!row_bytes) row_bytes = used;                       /* no pitch given: one tight row of `samples` elements per pixel */
    else if (row_bytes < used) inter = 0;                   /* a single-sample row: every sample of a pixel gets the one element */
    if (row_bytes < (uint64_t)w * bpp) { *why = "bytesPerRow is smaller than one row of the region"; return -1; }
    if (row_bytes > 0xffffffffu) { *why = "bytesPerRow does not fit the pass's 32-bit row"; return -1; }
    const uint64_t need = inter ? used : (uint64_t)w * bpp;
    if (off > buf_size || (uint64_t)(h - 1) * row_bytes + need > buf_size - off) { *why = "the region reads past the end of the buffer"; return -1; }
    plan->view_fmt = view; plan->mode = mode; plan->uint_out = u; plan->bpp = bpp; plan->inter = inter; plan->row = row_bytes;
    return 0;
}
#endif
