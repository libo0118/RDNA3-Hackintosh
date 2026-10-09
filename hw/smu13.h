/*
 * smu13.h - SMU 13.0.10 (MP1, PMFW) for Navi 32, ported from Linux amdgpu
 * smu_cmn.c / smu_v13_0.c / smu_v13_0_0_ppt.c / amdgpu_smu.c (MIT). amdgpu
 * drives MP1 13.0.0 (Navi 31) and 13.0.10 (Navi 32) with smu_v13_0_0_ppt.c
 * and smu13_driver_if_v13_0_0.h.
 *
 * Mailbox: C2PMSG_66 = message, C2PMSG_82 = argument, C2PMSG_90 = response.
 *
 * Power management (smu13_pm_init, the smu_hw_init -> smu_smc_hw_setup
 * sequence): tables for the driver <-> PMFW exchange live in VRAM; the
 * PPTable comes from the PMFW ("combo" table, TransferTableSmu2Dram) with
 * the BoardTable appended from the VBIOS smc_dpm_info, is written back,
 * then the allowed feature set is enabled (EnableAllSmuFeatures) and the
 * DPM tables are read back. Metrics (clocks, temperatures, power, fan) come
 * from SMU_TABLE_SMU_METRICS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_SMU13_H
#define RDNA3_SMU13_H

#include "rdna3_hw.h"
#include "vbios.h"
#include "gen/smu13_driver_if_v13_0_0.h"

#ifdef __cplusplus
extern "C" {
#endif

/* smu_v13_0_0_ppsmc.h */
#define PPSMC_Result_OK                 0x1
#define PPSMC_Result_Failed             0xFF
#define PPSMC_Result_UnknownCmd         0xFE
#define PPSMC_Result_CmdRejectedPrereq  0xFD
#define PPSMC_Result_CmdRejectedBusy    0xFC
#define PPSMC_MSG_TestMessage           0x1
#define PPSMC_MSG_GetSmuVersion         0x2
#define PPSMC_MSG_GetDriverIfVersion    0x3
#define PPSMC_MSG_SetAllowedFeaturesMaskLow  0x4
#define PPSMC_MSG_SetAllowedFeaturesMaskHigh 0x5
#define PPSMC_MSG_EnableAllSmuFeatures  0x6
#define PPSMC_MSG_DisableAllSmuFeatures 0x7
#define PPSMC_MSG_EnableSmuFeaturesLow  0x8
#define PPSMC_MSG_EnableSmuFeaturesHigh 0x9
#define PPSMC_MSG_DisableSmuFeaturesLow 0xA
#define PPSMC_MSG_DisableSmuFeaturesHigh 0xB
#define PPSMC_MSG_GetRunningSmuFeaturesLow  0xC
#define PPSMC_MSG_GetRunningSmuFeaturesHigh 0xD
#define PPSMC_MSG_SetDriverDramAddrHigh 0xE
#define PPSMC_MSG_SetDriverDramAddrLow  0xF
#define PPSMC_MSG_SetToolsDramAddrHigh  0x10
#define PPSMC_MSG_SetToolsDramAddrLow   0x11
#define PPSMC_MSG_TransferTableSmu2Dram 0x12
#define PPSMC_MSG_TransferTableDram2Smu 0x13
#define PPSMC_MSG_UseDefaultPPTable     0x14
#define PPSMC_MSG_EnterBaco             0x15
#define PPSMC_MSG_ExitBaco              0x16
#define PPSMC_MSG_ArmD3                 0x17
#define PPSMC_MSG_SetSoftMinByFreq      0x19
#define PPSMC_MSG_SetSoftMaxByFreq      0x1A
#define PPSMC_MSG_SetHardMinByFreq      0x1B
#define PPSMC_MSG_SetHardMaxByFreq      0x1C
#define PPSMC_MSG_GetMinDpmFreq         0x1D
#define PPSMC_MSG_GetMaxDpmFreq         0x1E
#define PPSMC_MSG_GetDpmFreqByIndex     0x1F
#define PPSMC_MSG_OverridePcieParameters 0x20
#define PPSMC_MSG_DramLogSetDramAddrHigh 0x21
#define PPSMC_MSG_DramLogSetDramAddrLow 0x22
#define PPSMC_MSG_DramLogSetDramSize    0x23
#define PPSMC_MSG_SetWorkloadMask       0x24
#define PPSMC_MSG_GetVoltageByDpm       0x25
#define PPSMC_MSG_GetDcModeMaxDpmFreq   0x27
#define PPSMC_MSG_AllowGfxOff           0x28
#define PPSMC_MSG_DisallowGfxOff        0x29
#define PPSMC_MSG_PowerUpVcn            0x2A
#define PPSMC_MSG_PowerDownVcn          0x2B
#define PPSMC_MSG_PowerUpJpeg           0x2C
#define PPSMC_MSG_PowerDownJpeg         0x2D
#define PPSMC_MSG_PrepareMp1ForUnload   0x2E
#define PPSMC_MSG_Mode1Reset            0x2F
#define PPSMC_MSG_SetPptLimit           0x32
#define PPSMC_MSG_GetPptLimit           0x33
#define PPSMC_MSG_ReenableAcDcInterrupt 0x34
#define PPSMC_MSG_NotifyPowerSource     0x35
#define PPSMC_MSG_RunDcBtc              0x36
#define PPSMC_MSG_SetExternalClientDfCstateAllow 0x3B
#define PPSMC_MSG_SetMGpuFanBoostLimitRpm 0x3C
#define PPSMC_MSG_SetGpoAllow           0x41
#define PPSMC_MSG_AllowIHHostInterrupt  0x4C
#define PPSMC_MSG_EnableUCLKShadow      0x51

int smu13_send_msg(rdna3_dev *d, uint32_t msg, uint32_t param, uint32_t *out);
/* Waits for PMFW to be up (response register non-zero), then
 * TestMessage + GetSmuVersion + GetDriverIfVersion. */
int smu13_hello(rdna3_dev *d, uint32_t *smu_version, uint32_t *if_version);
/* smu_v13_0_check_fw_status: MP1_FIRMWARE_FLAGS.INTERRUPTS_ENABLED */
int smu13_check_fw_status(rdna3_dev *d);

/* ---- power management ---- */

#define RD_SMU_FEATURE(bit) (1ull << (bit))

/* smu_v13_0_0_init_allowed_features with amdgpu's default ppfeaturemask on
 * GC 11.0.3: every bit, minus ATHUB_MMHUB_PG (soc21 sets no ATHUB/MMHUB
 * PG flags for 11.0.3) and minus GFXOFF (nothing here sends
 * AllowGfxOff/DisallowGfxOff yet). */
#define RD_SMU_FEATURES_AMDGPU \
    (~0ull & ~(RD_SMU_FEATURE(FEATURE_ATHUB_MMHUB_PG_BIT) | RD_SMU_FEATURE(FEATURE_GFXOFF_BIT)))

/* Default for a driver without Display Core: amdgpu's DC tells the PMFW
 * when memory / fabric p-state switches and DCN clock changes are safe for
 * the scanout (watermarks, FclkPstate / UclkPstate messages via DMCUB). We
 * keep the firmware's scanout running untouched, so until DC lands the
 * clocks that can underflow it stay at boot: no UCLK/FCLK/DCN DPM, no
 * memory voltage scaling, no FW D-states. GFXCLK/SOCCLK/MP0CLK/link DPM,
 * deep sleep, throttlers, thermal CTF and fan control do run. */
#define RD_SMU_FEATURES_SAFE \
    (RD_SMU_FEATURES_AMDGPU & ~(RD_SMU_FEATURE(FEATURE_DPM_UCLK_BIT) | \
     RD_SMU_FEATURE(FEATURE_DPM_FCLK_BIT) | RD_SMU_FEATURE(FEATURE_VMEMP_SCALING_BIT) | \
     RD_SMU_FEATURE(FEATURE_VDDIO_MEM_SCALING_BIT) | RD_SMU_FEATURE(FEATURE_DPM_DCN_BIT) | \
     RD_SMU_FEATURE(FEATURE_FW_DSTATE_BIT) | RD_SMU_FEATURE(FEATURE_SOC_MPCLK_DS_BIT) | \
     RD_SMU_FEATURE(FEATURE_BACO_MPCLK_DS_BIT) | RD_SMU_FEATURE(FEATURE_DS_FCLK_BIT)))

/* smu_v13_0_0_dpm_features: any of these running = "dpm is running" */
#define RD_SMU_FEATURES_DPM \
    (RD_SMU_FEATURE(FEATURE_DPM_GFXCLK_BIT) | RD_SMU_FEATURE(FEATURE_DPM_UCLK_BIT) | \
     RD_SMU_FEATURE(FEATURE_DPM_LINK_BIT) | RD_SMU_FEATURE(FEATURE_DPM_SOCCLK_BIT) | \
     RD_SMU_FEATURE(FEATURE_DPM_FCLK_BIT) | RD_SMU_FEATURE(FEATURE_DPM_MP0CLK_BIT))

/* smu_13_0_0_powerplay_table (smu_v13_0_0_pptable.h, #pragma pack(1)),
 * the "combo" table TransferTableSmu2Dram(TABLE_COMBO_PPTABLE) delivers;
 * PPTable_t follows the driver portion at RD_SMU_PP_SMC_PPTABLE_OFF. */
#define RD_SMU_PP_TABLE_FORMAT_REVISION 15
#define RD_SMU_PP_SMC_PPTABLE_OFF   832
#define RD_SMU_PP_PLATFORM_CAPS_OFF 18
#define RD_SMU_PP_THERMAL_CTRL_OFF  22
#define RD_SMU_PP_SHUTDOWN_TEMP_OFF 29
#define RD_SMU_PP_PLATFORM_CAP_POWERPLAY   0x1
#define RD_SMU_PP_PLATFORM_CAP_HARDWAREDC  0x4
#define RD_SMU_PP_PLATFORM_CAP_BACO        0x8
#define RD_SMU_PP_PLATFORM_CAP_MACO        0x10
#define RD_SMU_COMBO_PPTABLE_SIZE   0x4000   /* MP0_MP1_DATA_REGION_SIZE_COMBOPPTABLE */
#define RD_SMU_TOOL_SIZE            0x19000  /* SMU13_TOOL_SIZE (PMSTATUSLOG) */
#define RD_SMU_DRIVER_TABLE_SIZE    0x4000   /* >= every table we exchange */

#define RD_SMU_DPM_MAX_LEVELS 16
typedef struct rdna3_smu_dpm {
    uint32_t count;
    uint32_t level[RD_SMU_DPM_MAX_LEVELS]; /* MHz */
    int      fine_grained;                /* bit 31 of GetDpmFreqByIndex(0xff) */
} rdna3_smu_dpm;

typedef struct rdna3_smu {
    uint32_t version, if_version;
    int      fw_status_ok;

    /* VRAM offsets (smu_init_fb_allocations) */
    uint64_t driver_table, tool_table;

    rdna3_vbios_boot boot;

    /* driver portion of the powerplay table */
    uint8_t  pp_frev, pp_crev;
    uint32_t platform_caps;
    uint8_t  thermal_controller;
    uint16_t software_shutdown_temp;
    int      board_table_appended;
    int      scpm;                     /* PSP owns the PPTable / feature mask */

    PPTable_t pptable;                 /* what was written to the PMFW */

    uint64_t allowed, enabled;         /* feature masks */
    int      dpm_running;

    rdna3_smu_dpm gfx, uclk, soc, fclk;
    uint32_t ppt_limit_w;              /* GetPptLimit (AC) */
    uint32_t pcie_gen_cap, pcie_width_cap, pcie_levels;

    SmuMetricsExternal_t metrics;      /* last smu13_read_metrics */
    int      metrics_valid;
} rdna3_smu;

/* Allocates the driver and tool tables in VRAM (needs rdna3_vram_alloc_init
 * done), reads the boot-up values from the VBIOS and runs smu_smc_hw_setup.
 * 'scpm' (psp13_scpm_enabled): the PSP validated and handed the PPTable to
 * the PMFW, so like amdgpu the BoardTable is not appended, the PPTable not
 * written and the allowed mask not sent; the PMFW runs its own feature set.
 * Then, when 'allowed' keeps UCLK/FCLK DPM off but the PMFW runs them,
 * both are pinned to their top level (SetHardMinByFreq) so no memory or
 * fabric p-state switch can disturb the firmware's scanout. Sequence:
 * SetDriverDramAddr, SetToolsDramAddr, combo PPTable from the PMFW,
 * BoardTable from the VBIOS, write PPTable, RunDcBtc, allowed features,
 * OverridePcieParameters (caps from the PCIe link: gen 0..4 = Gen1..Gen5,
 * width 1..7 = x1,x2,x4,x8,x12,x16,x32), EnableAllSmuFeatures, running
 * features, DPM tables, PPT limit, default workload. 'vbios' may be NULL
 * when the ROM could not be read: the PMFW's BoardTable is then written
 * back unchanged, which amdgpu only does with SCPM. */
int smu13_pm_init(rdna3_dev *d, rdna3_smu *s, const rdna3_vbios *vbios, uint64_t allowed,
                  uint32_t pcie_gen_cap, uint32_t pcie_width_cap, int scpm);

/* smu_cmn_update_table: VRAM driver table <-> PMFW table 'table_id' */
int smu13_table_read(rdna3_dev *d, rdna3_smu *s, uint32_t table_id, uint32_t arg, void *dst,
                     uint32_t size);
int smu13_table_write(rdna3_dev *d, rdna3_smu *s, uint32_t table_id, uint32_t arg,
                      const void *src, uint32_t size);

/* SMU_TABLE_SMU_METRICS -> s->metrics (smu_cmn_get_metrics_table) */
int smu13_read_metrics(rdna3_dev *d, rdna3_smu *s);
/* convenience over s->metrics (units as in smu_v13_0_0_get_smu_metrics_data) */
uint32_t smu13_metric_gfxclk_mhz(const rdna3_smu *s);     /* AverageGfxclkFrequencyPreDs */
uint32_t smu13_metric_uclk_mhz(const rdna3_smu *s);       /* CurrClock[PPCLK_UCLK] */
uint32_t smu13_metric_temp_edge_c(const rdna3_smu *s);
uint32_t smu13_metric_temp_hotspot_c(const rdna3_smu *s);
uint32_t smu13_metric_temp_mem_c(const rdna3_smu *s);
uint32_t smu13_metric_power_w(const rdna3_smu *s);        /* AverageSocketPower */
uint32_t smu13_metric_gfx_activity(const rdna3_smu *s);   /* % */
uint32_t smu13_metric_fan_rpm(const rdna3_smu *s);
uint32_t smu13_metric_vddgfx_mv(const rdna3_smu *s);
uint32_t smu13_metric_throttlers(const rdna3_smu *s);     /* bit i = ThrottlingPercentage[i] != 0 */

/* smu_v13_0_set_single_dpm_table for one PPCLK_* clock */
int smu13_get_dpm_table(rdna3_dev *d, uint32_t ppclk, rdna3_smu_dpm *t);
/* smu_v13_0_set_soft_freq_limited_range (MHz; 0 = leave) */
int smu13_set_soft_freq_range(rdna3_dev *d, const rdna3_smu *s, uint32_t ppclk, uint32_t min_mhz,
                              uint32_t max_mhz);
/* smu_v13_0_set_ppt_limit / smu_v13_0_get_ppt_limit (W, AC) */
int smu13_set_power_limit(rdna3_dev *d, rdna3_smu *s, uint32_t watts);
int smu13_get_power_limit(rdna3_dev *d, rdna3_smu *s, uint32_t *watts);
/* smu_v13_0_0_set_power_profile_mode: WORKLOAD_PPLIB_*_BIT */
int smu13_set_workload(rdna3_dev *d, uint32_t workload_bit);

/* smu_v13_0_0_mode1_reset (13.0.10): DEBUGSMC_MSG_Mode1Reset, param 0 (no
 * RAS recovery) through the debug mailbox (C2PMSG_53/75/54, no response),
 * then SMU13_MODE1_RESET_WAIT_TIME.
 * Afterwards the PSP bootloader is back (psp13_wait_for_bootloader) and
 * everything from level 4 on has to be redone. */
#define RD_DEBUGSMC_MSG_Mode1Reset   2
#define RD_SMU_MODE1_RESET_WAIT_MS   500
int smu13_mode1_reset(rdna3_dev *d);

/* smu_hw_fini for a driver unload: on 13.0.10 the PMFW handles the DPM
 * features itself (smu_disable_dpms returns early), the driver only sends
 * PrepareMp1ForUnload (smu_reset_mp1_state). */
int smu13_pm_fini(rdna3_dev *d, rdna3_smu *s);

#ifdef __cplusplus
}
#endif
#endif
