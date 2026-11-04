/*
 * rdna3_pmctl.h - the power-management control interface between
 * RDNA3Accel and userspace (userspace/rdna3_pmctl.c), shared by the kext,
 * hw/smu13.c (which carries the requests out) and the simulator.
 *
 * macOS has no sysfs: what amdgpu offers through hwmon (pwm1, pwm1_enable,
 * fan1_target, power1_cap), pp_od_clk_voltage, the gpu_od/fan_ctrl files,
 * pp_power_profile_mode, pp_dpm_* and gpu_metrics is one request /
 * reply pair on the RDNA3UserClient (selector RDNA3_PMCTL_SELECTOR, struct
 * in = rdna3_pmctl_req, struct out = rdna3_pmctl_reply). The values and
 * their semantics are amdgpu's: fan modes are amd_fan_ctrl_mode, OD edits
 * are enum PP_OD_DPM_TABLE_COMMAND with the same inputs the sysfs files
 * take, profiles are enum PP_SMC_POWER_PROFILE.
 *
 * Gates (boot-args, all off by default):
 *   reads (RD_PMCTL_INFO) use only what the default profile already uses
 *   (the metrics table, GetPptLimit, GetRunningSmuFeatures, the PPTable);
 *   rdna3pmctl=1  every write below except overdrive, and the reads that
 *                 need new tables (overdrive table, activity monitor);
 *   rdna3pmctl=2  + overdrive (amdgpu's ppfeaturemask PP_OVERDRIVE_MASK:
 *                 smu->od_enabled): OD edits, power limit above the
 *                 PPTable's message limit;
 *   rdna3baco=1   RD_PMCTL_BACO_CYCLE (needs rdna3pmctl=1 too).
 * Writes also need an administrator (IOUserClient privilege check).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_PMCTL_H
#define RDNA3_PMCTL_H

#include <stdint.h>

#define RDNA3_PMCTL_SELECTOR 1   /* RDNA3UserClient::kSelPm */
#define RDNA3_PMCTL_ABI      1

/* gates the kext was booted with (rdna3_pmctl_info.gates) */
#define RD_PMCTL_GATE_WRITE  0x1u   /* rdna3pmctl >= 1 */
#define RD_PMCTL_GATE_OD     0x2u   /* rdna3pmctl >= 2 */
#define RD_PMCTL_GATE_BACO   0x4u   /* rdna3baco=1 */
#define RD_PMCTL_GATE_ADMIN  0x8u   /* this caller is an administrator */

/* enum amd_fan_ctrl_mode (kgd_pp_interface.h) */
#define RD_FAN_CTRL_NONE     0   /* full speed */
#define RD_FAN_CTRL_MANUAL   1
#define RD_FAN_CTRL_AUTO     2

/* enum PP_OD_DPM_TABLE_COMMAND (kgd_pp_interface.h; the zero RPM pair is
 * v6.14's) */
#define RD_PP_OD_EDIT_SCLK_VDDC_TABLE        0
#define RD_PP_OD_EDIT_MCLK_VDDC_TABLE        1
#define RD_PP_OD_EDIT_CCLK_VDDC_TABLE        2
#define RD_PP_OD_EDIT_VDDC_CURVE             3
#define RD_PP_OD_RESTORE_DEFAULT_TABLE       4
#define RD_PP_OD_COMMIT_DPM_TABLE            5
#define RD_PP_OD_EDIT_VDDGFX_OFFSET          6
#define RD_PP_OD_EDIT_FAN_CURVE              7
#define RD_PP_OD_EDIT_ACOUSTIC_LIMIT         8
#define RD_PP_OD_EDIT_ACOUSTIC_TARGET        9
#define RD_PP_OD_EDIT_FAN_TARGET_TEMPERATURE 10
#define RD_PP_OD_EDIT_FAN_MINIMUM_PWM        11
#define RD_PP_OD_EDIT_FAN_ZERO_RPM_ENABLE    12
#define RD_PP_OD_EDIT_FAN_ZERO_RPM_STOP_TEMP 13

/* enum PP_SMC_POWER_PROFILE (kgd_pp_interface.h) */
#define RD_PP_SMC_POWER_PROFILE_BOOTUP_DEFAULT 0
#define RD_PP_SMC_POWER_PROFILE_FULLSCREEN3D   1
#define RD_PP_SMC_POWER_PROFILE_POWERSAVING    2
#define RD_PP_SMC_POWER_PROFILE_VIDEO          3
#define RD_PP_SMC_POWER_PROFILE_VR             4
#define RD_PP_SMC_POWER_PROFILE_COMPUTE        5
#define RD_PP_SMC_POWER_PROFILE_CUSTOM         6
#define RD_PP_SMC_POWER_PROFILE_WINDOW3D       7
#define RD_PP_SMC_POWER_PROFILE_CAPPED         8
#define RD_PP_SMC_POWER_PROFILE_UNCAPPED       9
#define RD_PP_SMC_POWER_PROFILE_COUNT          10

/* the clocks of pp_dpm_* (enum pp_clock_type subset) */
#define RD_PP_SCLK    0
#define RD_PP_MCLK    1
#define RD_PP_SOCCLK  2
#define RD_PP_FCLK    3

/* OD_OPS_SUPPORT_* (amdgpu_dpm.h, v6.14) */
#define RD_OD_OPS_SUPPORT_FAN_CURVE_RETRIEVE                  (1u << 0)
#define RD_OD_OPS_SUPPORT_FAN_CURVE_SET                       (1u << 1)
#define RD_OD_OPS_SUPPORT_ACOUSTIC_LIMIT_THRESHOLD_RETRIEVE   (1u << 2)
#define RD_OD_OPS_SUPPORT_ACOUSTIC_LIMIT_THRESHOLD_SET        (1u << 3)
#define RD_OD_OPS_SUPPORT_ACOUSTIC_TARGET_THRESHOLD_RETRIEVE  (1u << 4)
#define RD_OD_OPS_SUPPORT_ACOUSTIC_TARGET_THRESHOLD_SET       (1u << 5)
#define RD_OD_OPS_SUPPORT_FAN_TARGET_TEMPERATURE_RETRIEVE     (1u << 6)
#define RD_OD_OPS_SUPPORT_FAN_TARGET_TEMPERATURE_SET          (1u << 7)
#define RD_OD_OPS_SUPPORT_FAN_MINIMUM_PWM_RETRIEVE            (1u << 8)
#define RD_OD_OPS_SUPPORT_FAN_MINIMUM_PWM_SET                 (1u << 9)
#define RD_OD_OPS_SUPPORT_FAN_ZERO_RPM_ENABLE_RETRIEVE        (1u << 10)
#define RD_OD_OPS_SUPPORT_FAN_ZERO_RPM_ENABLE_SET             (1u << 11)
#define RD_OD_OPS_SUPPORT_FAN_ZERO_RPM_STOP_TEMP_RETRIEVE     (1u << 12)
#define RD_OD_OPS_SUPPORT_FAN_ZERO_RPM_STOP_TEMP_SET          (1u << 13)

/* smu_v13_0_0_ppt.c PP_OD_FEATURE_* setting ids (od_lim_min/max index) */
#define RD_OD_SET_GFXCLK_FMIN            0
#define RD_OD_SET_GFXCLK_FMAX            1
#define RD_OD_SET_UCLK_FMIN              2
#define RD_OD_SET_UCLK_FMAX              3
#define RD_OD_SET_GFX_VF_CURVE           4
#define RD_OD_SET_FAN_CURVE_TEMP         5
#define RD_OD_SET_FAN_CURVE_PWM          6
#define RD_OD_SET_FAN_ACOUSTIC_LIMIT     7
#define RD_OD_SET_FAN_ACOUSTIC_TARGET    8
#define RD_OD_SET_FAN_TARGET_TEMPERATURE 9
#define RD_OD_SET_FAN_MINIMUM_PWM        10
#define RD_OD_SET_FAN_ZERO_RPM_ENABLE    11
#define RD_OD_SET_FAN_ZERO_RPM_STOP_TEMP 12
#define RD_OD_SET_COUNT                  13

/* enum amd_dpm_forced_level (amd_shared.h) */
#define RD_DPM_FORCED_LEVEL_AUTO             0x1u
#define RD_DPM_FORCED_LEVEL_MANUAL           0x2u
#define RD_DPM_FORCED_LEVEL_LOW              0x4u
#define RD_DPM_FORCED_LEVEL_HIGH             0x8u
#define RD_DPM_FORCED_LEVEL_PROFILE_STANDARD 0x10u
#define RD_DPM_FORCED_LEVEL_PROFILE_MIN_SCLK 0x20u
#define RD_DPM_FORCED_LEVEL_PROFILE_MIN_MCLK 0x40u
#define RD_DPM_FORCED_LEVEL_PROFILE_PEAK     0x80u
#define RD_DPM_FORCED_LEVEL_PROFILE_EXIT     0x100u
#define RD_DPM_FORCED_LEVEL_PERF_DETERMINISM 0x200u
enum rdna3_pmctl_op {
    RD_PMCTL_INFO = 0,       /* -> reply.info */
    RD_PMCTL_FAN_MODE,       /* a[0] = RD_FAN_CTRL_* (amdgpu pwm1_enable) */
    RD_PMCTL_FAN_PWM,        /* a[0] = 0..255, fan mode must be manual (pwm1) */
    RD_PMCTL_FAN_RPM,        /* a[0] = rpm, fan mode must be manual (fan1_target) */
    RD_PMCTL_POWER_LIMIT,    /* a[0] = W, 0 = the current one (power1_cap) */
    RD_PMCTL_OD_EDIT,        /* a[0] = RD_PP_OD_*, a[1..nargs-1] = its input */
    RD_PMCTL_PROFILE,        /* a[0] = RD_PP_SMC_POWER_PROFILE_*; CUSTOM: a[1..9]
                              * = clock (0 gfx, 1 fclk) + 8 coefficients */
    RD_PMCTL_PROFILE_COEFF,  /* a[0] = WORKLOAD_PPLIB_*_BIT -> reply.coeff */
    RD_PMCTL_FORCE_LEVELS,   /* a[0] = RD_PP_*CLK, a[1] = level mask (pp_dpm_*) */
    RD_PMCTL_DF_CSTATE,      /* a[0] = 0 disallow, 1 allow */
    RD_PMCTL_BACO_CYCLE,     /* enter BACO, exit, bring the GPU up again */
    RD_PMCTL_PERF_LEVEL,     /* a[0] = RD_DPM_FORCED_LEVEL_* (power_dpm_force_performance_level) */
    RD_PMCTL_OP_COUNT
};

#define RD_PMCTL_MAX_ARGS 24

typedef struct rdna3_pmctl_req {
    uint32_t abi;            /* RDNA3_PMCTL_ABI */
    uint32_t op;             /* enum rdna3_pmctl_op */
    uint32_t nargs;
    uint32_t pad;
    int32_t  a[RD_PMCTL_MAX_ARGS];
} rdna3_pmctl_req;

/* gpu_metrics_v1_3 fields smu_v13_0_0_get_gpu_metrics fills */
typedef struct rdna3_gpu_metrics {
    uint32_t temperature_edge, temperature_hotspot, temperature_mem;
    uint32_t temperature_vrgfx, temperature_vrsoc, temperature_vrmem;
    uint32_t average_gfx_activity, average_umc_activity, average_mm_activity;
    uint32_t average_socket_power;
    uint32_t average_gfxclk_frequency, average_uclk_frequency;
    uint32_t average_vclk0_frequency, average_dclk0_frequency;
    uint32_t average_vclk1_frequency, average_dclk1_frequency;
    uint32_t current_gfxclk, current_socclk, current_uclk;
    uint32_t current_vclk0, current_dclk0, current_vclk1, current_dclk1;
    uint32_t throttle_status;
    uint32_t current_fan_speed;
    uint32_t pcie_link_width, pcie_link_speed;   /* lanes, 0.1 GT/s */
    uint32_t voltage_gfx, voltage_soc, voltage_mem;
    uint32_t pad;
    uint64_t energy_accumulator;
    uint64_t indep_throttle_status;
} rdna3_gpu_metrics;

/* struct smu_temperature_range, millidegrees C */
typedef struct rdna3_temp_range {
    int32_t min, max, edge_emergency_max;
    int32_t hotspot_min, hotspot_crit_max, hotspot_emergency_max;
    int32_t mem_min, mem_crit_max, mem_emergency_max;
    int32_t software_shutdown_temp, software_shutdown_temp_offset;
} rdna3_temp_range;

typedef struct rdna3_pmctl_info {
    uint32_t gates;              /* RD_PMCTL_GATE_* */
    uint32_t pm_up;
    uint64_t features_enabled;
    /* fan */
    uint32_t fan_mode;           /* RD_FAN_CTRL_*; UINT32_MAX = unobserved/unsupported mode */
    uint32_t fan_pwm;            /* 0..255 */
    uint32_t fan_rpm, fan_max_rpm, no_fan; /* no usable fan control, not physical topology */
    /* power limit, W (UINT32_MAX = unavailable; separate from metrics power) */
    uint32_t power_current, power_default, power_min, power_max;
    /* overdrive */
    uint32_t od_enabled;         /* smu->od_enabled */
    uint32_t od_valid;           /* the tables below were read from the PMFW */
    uint32_t od_feature_ctrl;    /* OverDriveLimitsBasicMax.FeatureCtrlMask (PP_OD_FEATURE_*_BIT) */
    uint32_t od_ops;             /* RD_OD_OPS_SUPPORT_* */
    uint32_t user_od;            /* the user's OD table differs from the boot one */
    int32_t  od_gfxclk_min, od_gfxclk_max;
    int32_t  od_uclk_min, od_uclk_max;
    int32_t  od_vddgfx_offset;   /* VoltageOffsetPerZoneBoundary[0], mV */
    int32_t  od_ppt;             /* % */
    uint32_t od_fan_mode;        /* FAN_MODE_AUTO / FAN_MODE_MANUAL_LINEAR */
    uint32_t od_fan_curve_temp[6], od_fan_curve_pwm[6];
    uint32_t od_acoustic_limit, od_acoustic_target, od_fan_target_temp, od_fan_min_pwm;
    uint32_t od_zero_rpm_enable, od_zero_rpm_stop_temp;
    int32_t  od_lim_min[RD_OD_SET_COUNT], od_lim_max[RD_OD_SET_COUNT];
    int32_t  od_ppt_pct_min, od_ppt_pct_max;   /* powerplay_table OD POWERPERCENTAGE */
    /* clocks, MHz: smu_v13_0_0_get_dpm_ultimate_freq, populate_umd_state_clk */
    uint32_t gfxclk_min, gfxclk_max, uclk_min, uclk_max, socclk_min, socclk_max;
    uint32_t fclk_min, fclk_max;
    uint32_t gfxclk_standard, gfxclk_peak, uclk_standard, uclk_peak;
    uint32_t gfxclk_levels, uclk_levels, socclk_levels, fclk_levels;   /* DPM levels (pp_dpm_*) */
    uint32_t gfxclk_lv[16], uclk_lv[16], socclk_lv[16], fclk_lv[16];   /* MHz */
    /* smu_v13_0_0_get_current_clk_freq_by_table, MHz */
    uint32_t gfxclk_cur, uclk_cur, socclk_cur, fclk_cur;
    /* profile */
    uint32_t power_profile_mode;   /* RD_PP_SMC_POWER_PROFILE_* */
    uint32_t workload_mask;        /* what SetWorkloadMask got */
    /* thermal, identity, BACO */
    rdna3_temp_range thermal;
    uint64_t unique_id;
    uint32_t baco_support;         /* BACO_SUPPORT (1) | MACO_SUPPORT (2) */
    uint32_t baco_state;           /* SMU_BACO_STATE_ENTER 0 / EXIT 1 / NONE 2 */
    uint32_t df_cstate;            /* last DF C-state request, ~0 = none */
    uint32_t pad;
    rdna3_gpu_metrics metrics;
} rdna3_pmctl_info;

/* DpmActivityMonitorCoeffInt_t's GFXCLK / FCLK columns of
 * smu_v13_0_0_get_power_profile_mode */
typedef struct rdna3_pmctl_coeff {
    uint32_t workload;
    int32_t  gfx[8], fclk[8];   /* FPS, MinActiveFreqType, MinActiveFreq, BoosterFreqType,
                                 * BoosterFreq, PD_Data_limit_c, PD_Data_error_coeff,
                                 * PD_Data_error_rate_coeff */
} rdna3_pmctl_coeff;

typedef struct rdna3_pmctl_reply {
    int32_t  err;            /* 0 or a negative errno (RDNA3_E*) */
    uint32_t op;
    rdna3_pmctl_info  info;  /* every op answers with the state after it */
    rdna3_pmctl_coeff coeff;
} rdna3_pmctl_reply;

#endif
