/*
 * smu13.h - SMU 13.0.10 (MP1, PMFW) for Navi 32, ported from Linux amdgpu
 * smu_cmn.c / smu_v13_0.c / smu_v13_0_0_ppt.c / amdgpu_smu.c (MIT). amdgpu
 * drives MP1 13.0.0 (Navi 31) and 13.0.10 (Navi 32) with smu_v13_0_0_ppt.c
 * and smu13_driver_if_v13_0_0.h.
 * MP1 13.0.7 only has firmware-status and TestMessage/version queries here;
 * its PM initialization, table transfers and mailbox controls are rejected.
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
#include "rdna3_pmctl.h"

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
#define PPSMC_MSG_DALNotPresent         0x4E

/* Unknown/missing MP1 discovery fails before register access. 13.0.7 allows
 * only TestMessage / GetSmuVersion / GetDriverIfVersion, not PM commands. */
int smu13_send_msg(rdna3_dev *d, uint32_t msg, uint32_t param, uint32_t *out);
/* Waits for PMFW to be up (response register non-zero), then
 * TestMessage + GetSmuVersion + GetDriverIfVersion. On 13.0.7 the firmware
 * status check runs first, even when PM is disabled or not ported. */
int smu13_hello(rdna3_dev *d, uint32_t *smu_version, uint32_t *if_version);
/* smu_v13_0_check_fw_status / smu_v13_0_7_check_fw_status:
 * MP1_FIRMWARE_FLAGS.INTERRUPTS_ENABLED at the discovered IP's address. */
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
/* struct smu_13_0_0_overdrive_table overdrive_table at 211: max[] and min[]
 * (uint32 per SMU_13_0_0_ODSETTING_*) */
#define RD_SMU_PP_OD_TABLE_OFF      211
#define RD_SMU_PP_OD_MAX_OFF        (RD_SMU_PP_OD_TABLE_OFF + 44)
#define RD_SMU_PP_OD_MIN_OFF        (RD_SMU_PP_OD_MAX_OFF + 64 * 4)
#define RD_SMU_13_0_0_ODSETTING_POWERPERCENTAGE 4
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

    /* ---- PM control (smu13_pm_query and the functions below) ---- */
    uint64_t supported;                /* smu_feature.supported: running after setup */
    int32_t  od_ppt_pct_max, od_ppt_pct_min;   /* powerplay_table OD POWERPERCENTAGE */
    int      od_enabled;               /* smu->od_enabled */
    int      od_valid;                 /* od / od_boot / od_user read from the PMFW */
    int      user_od;                  /* smu->user_dpm_profile.user_od */
    uint32_t od_feature_mask;          /* adev->pm.od_feature_mask (RD_OD_OPS_SUPPORT_*) */
    OverDriveTableExternal_t od, od_boot, od_user;   /* overdrive_table, boot_, user_ */
    uint32_t current_power_limit;      /* smu->current_power_limit */
    uint32_t power_limit_default, power_limit_min, power_limit_max;
    uint32_t power_profile_mode, workload_mask;
    int      dc_controlled_by_gpio, no_fan;
    int      baco_platform, maco_support;   /* smu_baco_context */
    int      baco_state;               /* RD_SMU_BACO_STATE_* */
    uint32_t bios_scratch_reg;         /* firmware_info bios_scratch_reg_startaddr */
    uint32_t df_cstate;                /* last SetExternalClientDfCstateAllow, ~0 = none */
    uint64_t unique_id;                /* adev->unique_id */
    struct { uint32_t min, peak, standard; } pstate_gfx, pstate_uclk, pstate_soc, pstate_fclk;

    /* stage 4 item J (smu_v13_0_0_set_default_dpm_table's VCLK / DCLK and
     * PCIe tables, the forced performance level, the thermal interrupts) */
    rdna3_smu_dpm vclk, dclk;
    struct { uint32_t min, peak, standard; } pstate_vclk, pstate_dclk;
    int      vdclk_valid;              /* vclk / dclk read from the PMFW */
    uint32_t pcie_gen[NUM_LINK_LEVELS], pcie_lane[NUM_LINK_LEVELS];   /* pcie_table */
    uint32_t dpm_level, saved_dpm_level;   /* smu_dpm_context, RD_DPM_FORCED_LEVEL_* */
    int      irq_enabled;              /* smu_v13_0_set_irq_state(ENABLE) */
    uint64_t throttle_int_counter;     /* SMU_IH_INTERRUPT_CONTEXT_ID_THERMAL_THROTTLING */
    int      ac_power;                 /* adev->pm.ac_power (AC/DC context ids) */
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
/* With d->amdgpu_exact (rdna3amdgpu=1) smu13_pm_init follows
 * smu_smc_hw_setup's own order: 'allowed' == RD_SMU_FEATURES_AMDGPU becomes
 * smu_v13_0_0_get_allowed_feature_mask with the default ppfeaturemask
 * (GFXOFF allowed from PMFW 78.58), no OverridePcieParameters before
 * EnableAllSmuFeatures, and after the DPM tables (VCLK / DCLK included)
 * smu_v13_0_update_pcie_parameters on every level of the pcie_table,
 * smu_v13_0_enable_thermal_alert and smu_v13_0_notify_display_change
 * (d->amdgpu_dc). */

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
/* what decides idle power: the GFX clock before/after deep sleep, memory
 * clock after deep sleep, PCIe and UCLK activity (smu_v13_0_0_get_gpu_metrics) */
typedef struct rdna3_pm_detail {
    uint32_t gfx_activity, uclk_activity, pcie_busy;
    uint32_t gfxclk_target, gfxclk_pre_ds, gfxclk_post_ds, memclk_post_ds, fclk_post_ds;
    uint32_t socclk, dcefclk, socket_w, board_w, vddgfx_mv;
} rdna3_pm_detail;
void     smu13_metric_detail(const rdna3_smu *s, rdna3_pm_detail *o);

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
/* smu_v13_0_gfx_off_control: AllowGfxOff / DisallowGfxOff. With it allowed
 * the RLC powers the GFX off when idle; GC registers must not be touched
 * then (doorbells wake it). Needs FEATURE_GFXOFF in the allowed mask. */
int smu13_gfx_off(rdna3_dev *d, int allow);

/* smu_v13_0_0_mode1_reset (13.0.10): DEBUGSMC_MSG_Mode1Reset, param 0 (no
 * RAS recovery) through the debug mailbox (C2PMSG_53/75/54, no response),
 * then SMU13_MODE1_RESET_WAIT_TIME.
 * Afterwards the PSP bootloader is back (psp13_wait_for_bootloader) and
 * everything from level 4 on has to be redone. */
#define RD_DEBUGSMC_MSG_Mode1Reset   2
#define RD_SMU_MODE1_RESET_WAIT_MS   500
int smu13_mode1_reset(rdna3_dev *d);

/* ---- PM control: fan, power limit, overdrive, clocks / profiles, BACO ----
 * Ports of smu_v13_0.c / smu_v13_0_0_ppt.c (v6.12; the zero RPM overdrive
 * controls are v6.14's). Units and return values as in amdgpu. Userspace
 * reaches them through smu13_pmctl (common/rdna3_pmctl.h). */

/* smu_cmn.h */
#define RD_FDO_PWM_MODE_STATIC      1
#define RD_FDO_PWM_MODE_STATIC_RPM  5
/* enum smu_baco_state / enum smu_baco_seq (amdgpu_smu.h) */
#define RD_SMU_BACO_STATE_ENTER 0
#define RD_SMU_BACO_STATE_EXIT  1
#define RD_SMU_BACO_STATE_NONE  2
#define RD_BACO_SEQ_BACO        0
#define RD_BACO_SEQ_MSR         1
#define RD_BACO_SEQ_BAMACO      2
#define RD_BACO_SEQ_ULPS        3
/* amdgpu_dpm.h */
#define RD_BACO_SUPPORT  (1u << 0)
#define RD_MACO_SUPPORT  (1u << 1)
/* smu_v13_0.h */
#define RD_CTF_OFFSET_EDGE     5
#define RD_CTF_OFFSET_HOTSPOT  5
#define RD_CTF_OFFSET_MEM      5
/* smu_v13_0_0_ppt.c */
#define RD_SMU_13_0_0_BUSY_THRESHOLD 15
/* the PMFW 13.0.10 version from which smu_v13_0_0_set_power_profile_mode
 * adds the POWER_SAVING workload to every profile */
#define RD_SMU_13_0_10_PROFILE_POWERSAVING_FW 0x00504500

/* smu_late_init's queries on what smu13_pm_init set up, with no message
 * the default profile does not send already: smu_v13_0_0_check_powerplay_table's
 * BACO / OD / fan bits, populate_umd_state_clk, get_power_limit (GetPptLimit),
 * get_unique_id (metrics table). 'od_wanted' is amdgpu's od_enabled
 * (ppfeaturemask PP_OVERDRIVE_MASK; rdna3pmctl=2). 'vbios' (may be NULL)
 * gives the BIOS scratch registers BACO exit clears. */
int  smu13_pm_query(rdna3_dev *d, rdna3_smu *s, const rdna3_vbios *vbios, int od_wanted);

/* fan (smu_v13_0.c, smu_v13_0_0_ppt.c); the THM block must be in the
 * discovery table */
uint32_t smu13_get_fan_control_mode(rdna3_dev *d, rdna3_smu *s);
int  smu13_set_fan_control_mode(rdna3_dev *d, rdna3_smu *s, uint32_t mode);
int  smu13_set_fan_speed_pwm(rdna3_dev *d, rdna3_smu *s, uint32_t speed);
int  smu13_set_fan_speed_rpm(rdna3_dev *d, rdna3_smu *s, uint32_t speed);
int  smu13_get_fan_speed_pwm(rdna3_dev *d, rdna3_smu *s, uint32_t *speed);
int  smu13_get_fan_speed_rpm(rdna3_dev *d, rdna3_smu *s, uint32_t *speed);

/* power limit: smu_v13_0_0_get_power_limit, and smu_set_power_limit's
 * range check + smu_v13_0_0_set_power_limit (above the message limit
 * through the OD table's Ppt percentage, od_enabled only) */
int  smu13_get_power_limits(rdna3_dev *d, rdna3_smu *s, uint32_t *current_w, uint32_t *default_w,
                            uint32_t *max_w, uint32_t *min_w);
int  smu13_set_ppt_limit(rdna3_dev *d, rdna3_smu *s, uint32_t watts);

/* overdrive (OverDriveTable_t through TABLE_OVERDRIVE) */
int  smu13_is_od_feature_supported(const rdna3_smu *s, int od_feature_bit);
void smu13_get_od_setting_limits(const rdna3_smu *s, int od_setting, int32_t *min, int32_t *max);
int  smu13_get_overdrive_table(rdna3_dev *d, rdna3_smu *s, OverDriveTableExternal_t *t);
int  smu13_upload_overdrive_table(rdna3_dev *d, rdna3_smu *s, OverDriveTableExternal_t *t);
int  smu13_set_default_od_settings(rdna3_dev *d, rdna3_smu *s, int in_suspend);
int  smu13_restore_user_od_settings(rdna3_dev *d, rdna3_smu *s);
/* type = RD_PP_OD_*, input/size as amdgpu's sysfs parser hands them */
int  smu13_od_edit_dpm_table(rdna3_dev *d, rdna3_smu *s, uint32_t type, const long *input,
                             uint32_t size);

/* clocks / profiles */
int  smu13_get_dpm_ultimate_freq(const rdna3_smu *s, uint32_t ppclk, uint32_t *min, uint32_t *max);
int  smu13_force_clk_levels(rdna3_dev *d, rdna3_smu *s, uint32_t ppclk, uint32_t mask);
/* the clock now, from the metrics table (PPCLK_*, MHz) */
int  smu13_get_current_clk_freq_by_table(rdna3_dev *d, rdna3_smu *s, uint32_t ppclk, uint32_t *value);
void smu13_populate_umd_state_clk(rdna3_smu *s);
/* input[size] = PP_SMC_POWER_PROFILE_*; CUSTOM: size 9, input[0..8] */
int  smu13_set_power_profile_mode(rdna3_dev *d, rdna3_smu *s, const long *input, uint32_t size);
int  smu13_get_power_profile_coeff(rdna3_dev *d, rdna3_smu *s, uint32_t workload_bit,
                                   rdna3_pmctl_coeff *out);
int  smu13_get_thermal_temperature_range(const rdna3_smu *s, rdna3_temp_range *range);
void smu13_get_unique_id(rdna3_dev *d, rdna3_smu *s);
int  smu13_get_gpu_metrics(rdna3_dev *d, rdna3_smu *s, rdna3_gpu_metrics *m);
uint64_t smu13_indep_throttler_status(uint32_t throttler_status);
int  smu13_set_df_cstate(rdna3_dev *d, rdna3_smu *s, uint32_t state);

/* BACO (smu_v13_0.c). Not used by any suspend path: the kext reaches it
 * only through RD_PMCTL_BACO_CYCLE behind rdna3baco=1. 'runpm_audio' is
 * amdgpu's in_runpm && smu_cmn_is_audio_func_enabled (ArmD3 sequence). */
int  smu13_get_bamaco_support(rdna3_dev *d, rdna3_smu *s);
int  smu13_baco_get_state(const rdna3_smu *s);
int  smu13_baco_set_state(rdna3_dev *d, rdna3_smu *s, int state, uint32_t seq);
int  smu13_baco_set_armd3_sequence(rdna3_dev *d, rdna3_smu *s, uint32_t seq);
int  smu13_baco_enter(rdna3_dev *d, rdna3_smu *s, int runpm_audio, uint32_t seq);
int  smu13_baco_exit(rdna3_dev *d, rdna3_smu *s, int runpm_audio);

/* RD_PMCTL_* request -> reply (reply.info always filled). 'gates' are the
 * RD_PMCTL_GATE_* the caller may use. RD_PMCTL_BACO_CYCLE needs the OS
 * (re-init after the exit) and is RDNA3_EOPNOTSUPP here. */
int  smu13_pmctl(rdna3_dev *d, rdna3_smu *s, const rdna3_pmctl_req *rq, rdna3_pmctl_reply *rp,
                 uint32_t gates);
void smu13_pmctl_info(rdna3_dev *d, rdna3_smu *s, uint32_t gates, rdna3_pmctl_info *o);

/* smu_hw_fini for a driver unload: on 13.0.10 the PMFW handles the DPM
 * features itself (smu_disable_dpms returns early), the driver only sends
 * PrepareMp1ForUnload (smu_reset_mp1_state). With rdna3amdgpu=1 exactly
 * amdgpu's: the thermal alert off, and no UNLOAD (smu_reset_mp1_state sends
 * it only on a 13.0.10 without display hardware). */
int smu13_pm_fini(rdna3_dev *d, rdna3_smu *s);

/* ---- stage 4 item J ---- */

/* enum pp_mp1_state (kgd_pp_interface.h) */
#define RD_PP_MP1_STATE_NONE     0
#define RD_PP_MP1_STATE_SHUTDOWN 1
#define RD_PP_MP1_STATE_UNLOAD   2
#define RD_PP_MP1_STATE_RESET    3
/* smu_v13_0_0_set_mp1_state: UNLOAD sends PrepareMp1ForUnload(0x55), then
 * after a BACO exit smu_v13_0_disable_pmfw_state; every other state is
 * ignored on 13.0.0 / 13.0.10 */
int smu13_set_mp1_state(rdna3_dev *d, rdna3_smu *s, int mp1_state);
/* smu_v13_0_disable_pmfw_state: MP1_FIRMWARE_FLAGS cleared through SMN */
int smu13_disable_pmfw_state(rdna3_dev *d);

/* amdgpu_pp_feature_mask default and the bits smu_v13_0_0_get_allowed_feature_mask reads */
#define RD_PP_FEATURE_MASK_DEFAULT 0xfff7bfffu
#define RD_PP_SCLK_DPM_MASK        0x1u
#define RD_PP_MCLK_DPM_MASK        0x2u
#define RD_PP_PCIE_DPM_MASK        0x4u
#define RD_PP_SCLK_DEEP_SLEEP_MASK 0x8u
#define RD_PP_ULV_MASK             0x100u
#define RD_PP_SOCCLK_DPM_MASK      0x1000u
#define RD_PP_GFXOFF_MASK          0x8000u
/* smu_v13_0_0_get_allowed_feature_mask: 'athub_mmhub_pg' = soc21's
 * pg_flags have both AMD_PG_SUPPORT_ATHUB and _MMHUB (not on GC 11.0.3) */
uint64_t smu13_get_allowed_feature_mask(uint32_t smc_fw_version, uint32_t pp_feature,
                                        int athub_mmhub_pg);
/* smu_v13_0_0_is_mode1_reset_supported: PMFW 78.41 and later (the cached
 * smu_cmn_get_smc_version) */
int smu13_is_mode1_reset_supported(const rdna3_smu *s);
/* smu_v13_0_notify_display_change: DALNotPresent when no display core
 * drives the DCN ('dc_present' = amdgpu_device_has_dc_support) */
int smu13_notify_display_change(rdna3_dev *d, int dc_present);
/* smu_v13_0_set_power_source: NotifyPowerSource AC (0) / DC (1); smu13_pm_query
 * sends AC like smu_late_init's smu_set_ac_dc with rdna3amdgpu=1 */
int smu13_set_power_source(rdna3_dev *d, int dc);
/* smu_v13_0_update_pcie_parameters with PP_PCIE_DPM on: every level of the
 * pcie_table clamped to the link caps and sent (OverridePcieParameters) */
int smu13_update_pcie_parameters(rdna3_dev *d, rdna3_smu *s, uint32_t pcie_gen_cap,
                                 uint32_t pcie_width_cap);
/* smu_v13_0_deep_sleep_control / _gfx_ulv_control / _gpo_control */
int smu13_deep_sleep_control(rdna3_dev *d, rdna3_smu *s, int enable);
int smu13_gfx_ulv_control(rdna3_dev *d, rdna3_smu *s, int enable);
int smu13_gpo_control(rdna3_dev *d, int enable);

/* smu_v13_0_set_performance_level: soft min / max of every DPM clock for
 * the level (VCLK / DCLK per VCN instance) */
int smu13_set_performance_level(rdna3_dev *d, rdna3_smu *s, uint32_t level);
/* smu_force_performance_level (power_dpm_force_performance_level):
 * smu_enable_umd_pstate around the UMD profile levels (GPO, GFX ULV and
 * deep sleep off; 'umd_pstate' = amdgpu_asic_update_umd_stable_pstate,
 * soc21: RLC safe mode, called with enter 1 / 0; may be NULL), then the
 * level when it changed (smu_adjust_power_state_dynamic) */
typedef void (*rdna3_umd_pstate_fn)(void *ctx, int enter);
int smu13_force_performance_level(rdna3_dev *d, rdna3_smu *s, uint32_t level,
                                  rdna3_umd_pstate_fn umd_pstate, void *ctx);

/* ---- thermal / MP1 interrupts (smu_v13_0_register_irq_handler) ---- */
#define RD_IH_CLIENT_THM          0x0f   /* SOC21_IH_CLIENTID_THM */
#define RD_IH_CLIENT_ROM_SMUIO    0x16   /* SOC21_IH_CLIENTID_ROM_SMUIO */
#define RD_IH_CLIENT_MP1          0x1f   /* SOC21_IH_CLIENTID_MP1 */
#define RD_THM_SRCID_DIG_THERM_L2H 0     /* THM_11_0__SRCID__THM_DIG_THERM_L2H */
#define RD_THM_SRCID_DIG_THERM_H2L 1
#define RD_SMUIO_SRCID_GPIO19     83     /* SMUIO_11_0__SRCID__SMUIO_GPIO19: hardware CTF */
#define RD_SMU_IH_INTERRUPT_ID_TO_DRIVER 0xFE
#define RD_SMU_IH_CTX_AC                0x3
#define RD_SMU_IH_CTX_DC                0x4
#define RD_SMU_IH_CTX_THERMAL_THROTTLING 0x7
#define RD_SMU_IH_CTX_FAN_ABNORMAL      0x8
#define RD_SMU_IH_CTX_FAN_RECOVERY      0x9
/* smu_v13_0_set_irq_state: the THM high / low alert at the thermal range
 * (DIG_THERM_INTH = software shutdown temperature) and the MP1 software
 * interrupt (ID 0xFE) enabled or masked */
int smu13_set_irq_state(rdna3_dev *d, rdna3_smu *s, int enable);
/* smu_v13_0_enable_thermal_alert / _disable_thermal_alert (with
 * smu_v13_0_process_pending_interrupt: AllowIHHostInterrupt only when the
 * board's DC is GPIO controlled and ACDC runs) */
int smu13_enable_thermal_alert(rdna3_dev *d, rdna3_smu *s);
int smu13_disable_thermal_alert(rdna3_dev *d, rdna3_smu *s);
/* the IV is one smu_v13_0_register_irq_handler added */
int smu13_iv_is_smu(uint32_t client_id, uint32_t src_id);
/* smu_v13_0_irq_process: what the IV asks the OS to do */
#define RD_SMU_IRQ_NONE     0
#define RD_SMU_IRQ_SWCTF    1   /* check the hotspot in 50 ms (smu_swctf_delayed_work_handler) */
#define RD_SMU_IRQ_HWCTF    2   /* hardware CTF: shut the system down now */
#define RD_SMU_IRQ_UNDER    3   /* under the temperature range */
#define RD_SMU_IRQ_THROTTLE 4
int smu13_irq_process(rdna3_dev *d, rdna3_smu *s, uint32_t client_id, uint32_t src_id,
                      uint32_t ctxid);
/* smu_swctf_delayed_work_handler: 1 = the hotspot is at or above the
 * software shutdown temperature (or unreadable), shut down */
int smu13_swctf_check(rdna3_dev *d, rdna3_smu *s, uint32_t *hotspot_c);

#ifdef __cplusplus
}
#endif
#endif
