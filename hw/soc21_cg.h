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

#ifdef __cplusplus
}
#endif
#endif
