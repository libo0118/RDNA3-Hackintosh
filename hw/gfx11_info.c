/*
 * gfx11_info.c - see gfx11_info.h. Port of gfx_v11_0_get_cu_info,
 * gfx_v11_0_setup_rb and their helpers (Linux amdgpu gfx_v11_0.c, MIT).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gfx11_info.h"
#include "gen/gc_11_0_0_harvest.h"

#define GETF(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define RR(d, reg) rdna3_rreg((d), rdna3_reg_offset((d), HWIP_GC, 0, reg##_BASE_IDX, reg))
#define WR(d, reg, v) rdna3_wreg((d), rdna3_reg_offset((d), HWIP_GC, 0, reg##_BASE_IDX, reg), (v))

static uint32_t bitmask(uint32_t n) { return n >= 32 ? 0xffffffffu : ((1u << n) - 1); }
static uint32_t popcount32(uint32_t v)
{
    uint32_t c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

/* gfx_v11_0_select_se_sh: 0xffffffff = broadcast */
static void select_se_sh(rdna3_dev *d, uint32_t se, uint32_t sh)
{
    uint32_t v = GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK;
    v |= se == 0xffffffffu ? GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK
                           : ((se << GRBM_GFX_INDEX__SE_INDEX__SHIFT) & GRBM_GFX_INDEX__SE_INDEX_MASK);
    v |= sh == 0xffffffffu ? GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK
                           : ((sh << GRBM_GFX_INDEX__SA_INDEX__SHIFT) & GRBM_GFX_INDEX__SA_INDEX_MASK);
    WR(d, regGRBM_GFX_INDEX, v);
}

static uint32_t sa_active_bitmap(rdna3_dev *d, const rdna3_gfx_cu_info *ci)
{
    uint32_t dis = GETF(RR(d, regCC_GC_SA_UNIT_DISABLE), CC_GC_SA_UNIT_DISABLE, SA_DISABLE);
    uint32_t udis = GETF(RR(d, regGC_USER_SA_UNIT_DISABLE), GC_USER_SA_UNIT_DISABLE, SA_DISABLE);
    return bitmask(ci->max_sa_per_se * ci->max_se) & ~(dis | udis);
}

static uint32_t rb_active_bitmap(rdna3_dev *d, const rdna3_gfx_cu_info *ci)
{
    uint32_t dis = GETF(RR(d, regCC_RB_BACKEND_DISABLE), CC_RB_BACKEND_DISABLE, BACKEND_DISABLE);
    uint32_t udis = GETF(RR(d, regGC_USER_RB_BACKEND_DISABLE), GC_USER_RB_BACKEND_DISABLE, BACKEND_DISABLE);
    return bitmask(ci->max_backends_per_se * ci->max_se) & ~(dis | udis);
}

/* gfx_v11_0_get_wgp_active_bitmap_per_sh for the selected SE/SA */
static uint32_t wgp_active_bitmap(rdna3_dev *d, const rdna3_gfx_cu_info *ci)
{
    uint32_t v = RR(d, regCC_GC_SHADER_ARRAY_CONFIG) | RR(d, regGC_USER_SHADER_ARRAY_CONFIG);
    v = (v & CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK) >> CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT;
    return ~v & bitmask(ci->max_cu_per_sa >> 1);
}

int gfx11_get_cu_info(rdna3_dev *d, rdna3_gfx_cu_info *ci)
{
    uint32_t i, j, k, max_sa, width, per_sa, rb = 0;

    if (!ci->max_se || !ci->max_sa_per_se || !ci->max_cu_per_sa || ci->max_se > 8 ||
        ci->max_sa_per_se > 2)
        return RDNA3_EINVAL;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            ci->cu_bitmap[i][j] = 0;
    ci->cu_active = 0;

    /* gfx_v11_0_setup_rb */
    ci->sa_active = sa_active_bitmap(d, ci);
    max_sa = ci->max_se * ci->max_sa_per_se;
    width = ci->max_backends_per_se / ci->max_sa_per_se;
    per_sa = bitmask(width);
    for (i = 0; i < max_sa; i++)
        if (ci->sa_active & (1u << i))
            rb |= per_sa << (i * width);
    ci->rb_active = rb & rb_active_bitmap(d, ci);
    ci->num_rbs = popcount32(ci->rb_active);

    /* gfx_v11_0_get_cu_info (no amdgpu.disable_cu masks) */
    for (i = 0; i < ci->max_se; i++) {
        for (j = 0; j < ci->max_sa_per_se; j++) {
            uint32_t wgp, cu = 0;
            if (!((ci->sa_active >> (i * ci->max_sa_per_se + j)) & 1))
                continue;
            select_se_sh(d, i, j);
            wgp = wgp_active_bitmap(d, ci);
            for (k = 0; k < 16; k++)
                if (wgp & (1u << k))
                    cu |= 3u << (2 * k);   /* one active WGP = two CUs */
            ci->cu_bitmap[i % 4][j + (i / 4) * 2] = cu;
            ci->cu_active += popcount32(cu & bitmask(ci->max_cu_per_sa));
        }
    }
    select_se_sh(d, 0xffffffffu, 0xffffffffu);
    return RDNA3_OK;
}
