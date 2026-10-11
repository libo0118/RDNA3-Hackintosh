/*
 * gfx11_info.h - what is left of the GC after harvesting, read the way
 * gfx_v11_0_get_cu_info / gfx_v11_0_setup_rb do (Linux amdgpu, MIT):
 * active shader arrays (CC/GC_USER_SA_UNIT_DISABLE), render backends
 * (CC/GC_USER_RB_BACKEND_DISABLE) and per-SA WGPs (CC/GC_USER_SHADER_ARRAY
 * _CONFIG, selected through GRBM_GFX_INDEX). Feeds AMDGPU_INFO_DEV_INFO
 * so RADV sees the real CU and RB counts.
 *
 * Writes GRBM_GFX_INDEX (and restores broadcast); the caller makes sure
 * nothing else selects SE/SA meanwhile.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_GFX11_INFO_H
#define RDNA3_GFX11_INFO_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdna3_gfx_cu_info {
    /* in: the configuration from discovery (gc_info) */
    uint32_t max_se, max_sa_per_se, max_cu_per_sa, max_backends_per_se;
    /* out */
    uint32_t sa_active;            /* bit SE * max_sa_per_se + SA */
    uint32_t rb_active;            /* backend_enable_mask */
    uint32_t num_rbs;
    uint32_t cu_bitmap[4][4];      /* cu_info->bitmap[0] layout (SE4..7 in columns 2-3) */
    uint32_t cu_active;            /* cu_info->number */
} rdna3_gfx_cu_info;

int gfx11_get_cu_info(rdna3_dev *d, rdna3_gfx_cu_info *ci);
/* gfx_v11_0_get_tcc_info (gfx_v11_0_constants_init): the disabled GL2
 * channels, CGTS_TCC_DISABLE | CGTS_USER_TCC_DISABLE, TCC_DISABLE in bits
 * 15:0 and HI_TCC_DISABLE in 23:16 (adev->gfx.config.tcc_disabled_mask,
 * AMDGPU_INFO_DEV_INFO's tcc_disabled_mask) */
uint32_t gfx11_get_tcc_info(rdna3_dev *d);

#ifdef __cplusplus
}
#endif
#endif
