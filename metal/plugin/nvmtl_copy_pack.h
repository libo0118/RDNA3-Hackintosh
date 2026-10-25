/*
 * What a Metal blit copy between a texture and a buffer becomes in Vulkan.
 *
 * vkCmdCopyImageToBuffer / vkCmdCopyBufferToImage valid usage the plugin
 * must keep (a RADV that is handed anything else is not required to survive;
 * card run 2026-10-25c: Chess died right after the plugin recorded such a
 * copy during GL's glReadPixels depth resolve):
 *   - the image has VK_SAMPLE_COUNT_1_BIT (Metal's blit cannot read or write a
 *     multisampled texture either);
 *   - one aspect per region, and it is one the image's format has;
 *   - a depth or stencil aspect has its own packing: D16 is 2 B, D24 (also the
 *     D24 of D24_S8) and D32 (also the D32 of D32_S8) are 4 B, stencil is 1 B,
 *     whatever the image's "bytes per pixel" says;
 *   - the buffer offset of a depth/stencil copy is a multiple of 4.
 *
 * Numeric Vulkan values on purpose: the host test includes this file without
 * the Vulkan headers. SPDX-License-Identifier: MIT
 */
#ifndef NVMTL_COPY_PACK_H
#define NVMTL_COPY_PACK_H
#include <stdint.h>
#include <stddef.h>

enum {
    NVMTL_CP_D16 = 124, NVMTL_CP_X8D24 = 125, NVMTL_CP_D32 = 126, NVMTL_CP_S8 = 127,
    NVMTL_CP_D16S8 = 128, NVMTL_CP_D24S8 = 129, NVMTL_CP_D32S8 = 130,
    NVMTL_CP_ASPECT_COLOR = 1, NVMTL_CP_ASPECT_DEPTH = 2, NVMTL_CP_ASPECT_STENCIL = 4
};

/* the aspects the format has */
static inline uint32_t nvmtl_cp_format_aspects(uint32_t vkfmt)
{
    switch (vkfmt) {
    case NVMTL_CP_D16: case NVMTL_CP_X8D24: case NVMTL_CP_D32: return NVMTL_CP_ASPECT_DEPTH;
    case NVMTL_CP_S8: return NVMTL_CP_ASPECT_STENCIL;
    case NVMTL_CP_D16S8: case NVMTL_CP_D24S8: case NVMTL_CP_D32S8: return NVMTL_CP_ASPECT_DEPTH | NVMTL_CP_ASPECT_STENCIL;
    default: return NVMTL_CP_ASPECT_COLOR;
    }
}

/* Decides the aspect and the bytes per element of one copy region.
 *   aspect_override: 0, or the single aspect a Metal blit option asked for
 *   (MTLBlitOptionDepthFromDepthStencil -> 2, StencilFromDepthStencil -> 4).
 *   img_bpp: the image's own bytes per element (colour formats), 0 = 4.
 *   offset: the buffer offset, checked for the depth/stencil rule.
 * Returns 0 and fills *aspect / *bpp, or -1 and *why (a static string) says
 * which rule the copy breaks. */
static inline int nvmtl_cp_resolve(uint32_t vkfmt, uint32_t samples, uint32_t img_bpp, uint32_t aspect_override,
                                   uint64_t offset, uint32_t *aspect, uint32_t *bpp, const char **why)
{
    const uint32_t has = nvmtl_cp_format_aspects(vkfmt);
    uint32_t a;
    if (samples > 1) { *why = "the image is multisampled (Vulkan copies need 1 sample; a Metal blit cannot copy a multisample texture)"; return -1; }
    if (aspect_override) {
        if (aspect_override & (aspect_override - 1)) { *why = "the blit options name more than one aspect"; return -1; }
        if (aspect_override & ~has) { *why = "the blit option names an aspect this format does not have"; return -1; }
        a = aspect_override;
    } else {
        /* a combined depth/stencil copied without an option: depth, as Metal's default for such a copy */
        a = (has & NVMTL_CP_ASPECT_DEPTH) ? NVMTL_CP_ASPECT_DEPTH : has;
    }
    if (a == NVMTL_CP_ASPECT_COLOR) {
        *bpp = img_bpp ? img_bpp : 4u;
    } else if (a == NVMTL_CP_ASPECT_STENCIL) {
        *bpp = 1u;
    } else {
        *bpp = (vkfmt == NVMTL_CP_D16 || vkfmt == NVMTL_CP_D16S8) ? 2u : 4u;
    }
    if (a != NVMTL_CP_ASPECT_COLOR && (offset & 3u)) { *why = "a depth/stencil copy needs a buffer offset that is a multiple of 4 (Vulkan valid usage of vkCmdCopyImageToBuffer / BufferToImage, bufferOffset for depth/stencil formats; Metal allows other offsets) - the copy is DROPPED"; return -1; }
    *aspect = a;
    return 0;
}
#endif
