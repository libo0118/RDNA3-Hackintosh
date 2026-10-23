/*
 * nvmtl_linear_pitch.h - which bytesPerRow a linear buffer texture can alias.
 *
 * Plain C, no Vulkan and no Objective-C, so tests/test_linear_pitch.c checks
 * it on the host exactly as nvmtl_vk_image_create_buffer_alias uses it.
 *
 * The alias is a VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT image with the
 * explicit modifier DRM_FORMAT_MOD_LINEAR and the app's bytesPerRow as the
 * plane's rowPitch. RADV takes a custom linear pitch only when it is a
 * multiple of 256 B: ac_surface_override_offset_stride() /
 * ac_surface_get_pitch_align() want 256 / bytes-per-element pixels on GFX9+,
 * and the image descriptor of GFX10.3+ (ac_descriptors.c) takes a custom
 * pitch in units of 256 B, which is a hardware rule. Anything else fails in
 * vkCreateImage with VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT
 * (-1000158000), card runs 2026-10-17d and 2026-10-18b: every alias that
 * worked had pitch % 256 == 0, every failure had not. (The 32 B unit the NVK
 * port checked is NVIDIA's.) Such a texture is not aliased; the caller falls
 * back to a private copy.
 */
#ifndef NVMTL_LINEAR_PITCH_H
#define NVMTL_LINEAR_PITCH_H

#include <stddef.h>
#include <stdint.h>

#define NVMTL_LINEAR_PITCH_ALIGN 256u

/* 1: row_pitch (bytes) can be the explicit rowPitch of a linear DRM-modifier image
 * of bpe bytes per element (0: unknown, only the 256 B rule is checked) */
static inline int nvmtl_linear_pitch_ok(size_t row_pitch, uint32_t bpe)
{
    if (!row_pitch || row_pitch % NVMTL_LINEAR_PITCH_ALIGN) return 0;
    if (bpe && row_pitch % bpe) return 0;
    return 1;
}

#endif
