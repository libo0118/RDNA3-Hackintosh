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
 * perfmon MGCG update after it does nothing without
 * AMD_CG_SUPPORT_GFX_PERF_CLK, which GC 11.0.3 does not set.
 * 'ctx' is the rdna3_dev (rdna3_umd_pstate_fn). */
void soc21_update_umd_stable_pstate(void *ctx, int enter);

/* ---- soc21 common block and NBIO 4.3 (rdna3amdgpu=1) ---- */
#define RD_IP_VERSION(maj, min, rev) (((uint32_t)(maj) << 16) | ((uint32_t)(min) << 8) | (uint32_t)(rev))
/* amdgpu_device_should_use_aspm: amdgpu.aspm (-1 auto, 0 off, 1 on), the
 * PMFW's PCIe DPM (pp_feature PP_PCIE_DPM_MASK) and the link's ASPM state
 * (pcie_aspm_enabled) */
int  rd_should_use_aspm(int amdgpu_aspm, int pcie_dpm, int link_aspm_enabled);
/* soc21_program_aspm -> nbio_v4_3_program_aspm (+ nbio_v4_3_program_ltr):
 * only for the PCIE IP 7.4.0 / 7.6.0 ('pcie_ver' RD_IP_VERSION); 'ltr_path'
 * = pdev->ltr_path. Returns 1 when it programmed. */
int  soc21_program_aspm(rdna3_dev *d, int should_use_aspm, uint32_t pcie_ver, int ltr_path);
/* nbio_v4_3_init_registers: NBIO 4.3.0 clears STRAP_NO_SOFT_RESET_DEV0_F2 */
void nbio43_init_registers(rdna3_dev *d);
/* nbio_v4_3_update_medium_grain_clock_gating / _light_sleep: 'supported'
 * = cg_flags BIF_MGCG / BIF_LS (GC 11.0.3 has neither, so only the ungate
 * direction writes) */
void nbio43_update_medium_grain_clock_gating(rdna3_dev *d, int enable, int supported);
void nbio43_update_medium_grain_light_sleep(rdna3_dev *d, int enable, int supported);
/* soc21_common_late_init (bare metal, no NBIO RAS): the selfring doorbell
 * aperture; soc21_common_hw_fini (= soc21_common_suspend): both doorbell
 * apertures off */
void soc21_common_late_init(rdna3_dev *d);
void soc21_common_hw_fini(rdna3_dev *d);
/* soc21_need_reset_on_init: the sOS already runs (MP0 C2PMSG_81 != 0),
 * the driver was not unloaded cleanly */
int  soc21_need_reset_on_init(rdna3_dev *d);
/* soc21_need_reset_on_resume: an aborted S3 (in_s3 && !suspend_complete)
 * with the TOS still running - C2PMSG_81 changes within 100 ms */
int  soc21_need_reset_on_resume(rdna3_dev *d, int in_s3, int suspend_complete);

#ifdef __cplusplus
}
#endif
#endif
