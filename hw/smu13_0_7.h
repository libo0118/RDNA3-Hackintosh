/* SMU 13.0.7's private ABI crosses this header only as decoded values.
 * Do not include either vendor driver_if header here. SPDX-License-Identifier: MIT */
#ifndef RDNA3_SMU13_0_7_H
#define RDNA3_SMU13_0_7_H
#include "rdna3_hw.h"
#include "vbios.h"
#include "rdna3_pmctl.h"
#ifdef __cplusplus
extern "C" {
#endif

#define RD_SMU_13_0_7_IF_VERSION 0x35u

typedef struct rdna3_pm_detail {
    uint32_t gfx_activity, uclk_activity, pcie_busy;
    uint32_t gfxclk_target, gfxclk_pre_ds, gfxclk_post_ds, memclk_post_ds, fclk_post_ds;
    uint32_t socclk, dcefclk, socket_w, board_w, vddgfx_mv;
} rdna3_pm_detail;

typedef struct rdna3_smu_13_0_7 {
    uint8_t pp_frev, pp_crev, thermal_controller;
    uint16_t shutdown_temp;
    uint32_t platform_caps, sku_version, board_version;
    int board_table_appended, maco_support;
    uint16_t temp_edge, temp_hotspot, temp_mem, fan_max_rpm;
    int16_t fan_temp_offset;
    uint16_t base_clock_ac, game_clock_ac, ppt_ac;
    uint16_t ppt_message_limit_ac, ppt_message_limit_dc;
    uint8_t pcie_gen[3], pcie_lane[3];
    uint16_t lclk[3];
    rdna3_gpu_metrics gpu_metrics;
    rdna3_pm_detail detail;
    uint32_t clock_mhz[13];
    uint32_t fan_pwm;
} rdna3_smu_13_0_7;

/* smu_v13_0_7_setup_pptable + smu_cmn_write_pptable (SCPM never uploads). */
int smu13_0_7_setup_pptable(rdna3_dev *d, rdna3_smu_13_0_7 *p, uint64_t driver_table,
                           const rdna3_vbios *vbios, uint32_t if_version, int scpm);
int smu13_0_7_read_metrics(rdna3_dev *d, rdna3_smu_13_0_7 *p, uint64_t driver_table);
int smu13_0_7_bootup_values(rdna3_dev *d, const rdna3_vbios *vbios, rdna3_vbios_boot *boot);
uint64_t smu13_0_7_allowed_features(uint32_t pp_feature, int athub_mmhub_pg);
int smu13_0_7_message_supported(uint32_t msg);

/* Shared C2PMSG 66/82/90 transport, implemented by smu13.c. */
int smu13_send_msg(rdna3_dev *d, uint32_t msg, uint32_t param, uint32_t *out);
#ifdef __cplusplus
}
#endif
#endif
