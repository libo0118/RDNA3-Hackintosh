/*
 * soc21_cg.h - clock gating outside GC (HDP, ATHUB) for Navi 32.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SOC21_CG_H
#define SOC21_CG_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AMD_CG_STATE_GATE / UNGATE for the non-GFX blocks amdgpu gates on GC
 * 11.0.3 (cg_flags HDP_SD, ATHUB_MGCG, ATHUB_LS). RDNA3_ENODEV when
 * neither block is known. */
int      soc21_set_clock_gating(rdna3_dev *d, int enable);

#define RD_SOC_CG_HDP_SD      (1u << 0)
#define RD_SOC_CG_ATHUB_MGCG  (1u << 1)
#define RD_SOC_CG_ATHUB_LS    (1u << 2)
/* what is gated now (get_clockgating_state) */
uint32_t soc21_clock_gating_state(rdna3_dev *d);

/* soc21_update_umd_stable_pstate (amdgpu_asic_update_umd_stable_pstate)
 * around the UMD profile levels: RLC safe mode entered / left; the
 * update_perfmon_mgcg after it (gfx_v11_0_update_perf_clk) does nothing
 * without AMD_CG_SUPPORT_GFX_PERF_CLK, which GC 11.0.3 does not set.
 * 'ctx' is the rdna3_dev (rdna3_umd_pstate_fn). */
void soc21_update_umd_stable_pstate(void *ctx, int enter);

#ifdef __cplusplus
}
#endif
#endif
