/*
 * rdna3_pmctl - power management of the RX 7800 XT from macOS: what amdgpu
 * offers on Linux through hwmon and the pp_* / gpu_od sysfs files, over
 * RDNA3Accel's user client (selector 1, common/rdna3_pmctl.h).
 *
 *   rdna3_pmctl [info]                 fan, power limit, overdrive, clocks,
 *                                      profile, thermal limits, gpu_metrics
 *   rdna3_pmctl fan auto|manual|full   pwm1_enable 2 / 1 / 0
 *   rdna3_pmctl fan pwm <0-255>        pwm1 (manual mode first)
 *   rdna3_pmctl fan rpm <rpm>          fan1_target (manual mode first)
 *   rdna3_pmctl power <W>              power1_cap (0 = the current one)
 *   rdna3_pmctl od sclk <min> <max>    pp_od_clk_voltage "s 0 <min>", "s 1 <max>", "c"
 *   rdna3_pmctl od mclk <min> <max>    "m 0 <min>", "m 1 <max>", "c"
 *   rdna3_pmctl od vddgfx <mV>         "vo <mV>", "c"
 *   rdna3_pmctl od fan-curve <i> <C> <%>   gpu_od/fan_ctrl/fan_curve
 *   rdna3_pmctl od acoustic-limit <rpm> | acoustic-target <rpm> |
 *                  fan-target-temp <C> | fan-min-pwm <%> |
 *                  zero-rpm <0|1> | zero-rpm-stop <C>
 *   rdna3_pmctl od restore [what]      "r" (everything) or one fan setting
 *                                      (fan-curve, acoustic-limit, ...)
 *   rdna3_pmctl profile <n|name> [clk fps minType minFreq boostType boostFreq
 *                       limit_c err err_rate]   pp_power_profile_mode
 *   rdna3_pmctl profiles               the activity monitor table of each profile
 *   rdna3_pmctl levels sclk|mclk|socclk|fclk <mask>   pp_dpm_* "echo <levels>"
 *   rdna3_pmctl dfcstate allow|disallow
 *   rdna3_pmctl baco-cycle             BACO enter + exit, GPU up again (rdna3baco=1)
 *   rdna3_pmctl perflevel auto|low|high|manual|profile_standard|profile_min_sclk|
 *                         profile_min_mclk|profile_peak|profile_exit
 *                                      power_dpm_force_performance_level
 *   rdna3_pmctl selftest               the card test of ladder step pmctl: every
 *                                      write once and back, ends with
 *                                      "PMCTL: ALL PASSED" or "PMCTL: FAILED"
 *
 * Reads work for every user; writes need the boot-arg rdna3pmctl=1
 * (overdrive: rdna3pmctl=2) and root.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include "rdna3_pmctl.h"

static io_connect_t conn;

static int pm_call(uint32_t op, uint32_t n, const int32_t *a, rdna3_pmctl_reply *rp)
{
    rdna3_pmctl_req rq;
    uint64_t err = 0;
    uint32_t nout = 1;
    size_t rsz = sizeof(*rp);
    uint32_t i;
    kern_return_t kr;

    if (n > RD_PMCTL_MAX_ARGS) {   /* never send a count the arguments do not back */
        fprintf(stderr, "%u arguments: at most %u\n", n, RD_PMCTL_MAX_ARGS);
        return -EINVAL;
    }
    memset(&rq, 0, sizeof(rq));
    rq.abi = RDNA3_PMCTL_ABI;
    rq.op = op;
    rq.nargs = n;
    for (i = 0; i < n; i++)
        rq.a[i] = a[i];
    kr = IOConnectCallMethod(conn, RDNA3_PMCTL_SELECTOR, NULL, 0, &rq, sizeof(rq), &err, &nout, rp, &rsz);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOConnectCallMethod: 0x%x (an older kext without PM control?)\n", kr);
        return -EIO;
    }
    return rp->err;
}

static const char *errname(int e)
{
    switch (e) {
    case 0: return "ok";
    case -1: return "EPERM (not root)";
    case -95: return "EOPNOTSUPP (boot-arg rdna3pmctl / rdna3baco off, or not on this card)";
    case -22: return "EINVAL";
    case -5: return "EIO (the PMFW refused)";
    case -19: return "ENODEV (PM not up)";
    default: return "error";
    }
}

static const char *const kProfiles[RD_PP_SMC_POWER_PROFILE_COUNT] = {
    "BOOTUP_DEFAULT", "3D_FULL_SCREEN", "POWER_SAVING", "VIDEO", "VR", "COMPUTE", "CUSTOM",
    "WINDOW_3D", "CAPPED", "UNCAPPED",
};

/* pp_dpm_*: the levels, '*' on the one the clock runs at */
static void print_levels(const char *name, const uint32_t *lv, uint32_t n, uint32_t cur)
{
    uint32_t k;
    printf("%s (now %u MHz):", name, cur);
    for (k = 0; k < n && k < 16; k++)
        printf(" %u: %uMhz%s", k, lv[k], lv[k] == cur ? " *" : "");
    printf("\n");
}

static void print_info(const rdna3_pmctl_info *i)
{
    static const char *const fan[] = { "full speed (none)", "manual", "auto (PMFW)" };
    const rdna3_gpu_metrics *m = &i->metrics;
    unsigned k;

    printf("gates: %s%s%s%s\n", i->gates & RD_PMCTL_GATE_WRITE ? "rdna3pmctl " : "read-only ",
           i->gates & RD_PMCTL_GATE_OD ? "overdrive " : "", i->gates & RD_PMCTL_GATE_BACO ? "baco " : "",
           i->gates & RD_PMCTL_GATE_ADMIN ? "(admin)" : "(not admin)");
    printf("features running: 0x%016llx\n", (unsigned long long)i->features_enabled);
    printf("fan: %s, %u rpm, pwm %u/255, max %u rpm%s\n", i->fan_mode < 3 ? fan[i->fan_mode] : "?",
           i->fan_rpm, i->fan_pwm, i->fan_max_rpm, i->no_fan ? " (no fan control)" : "");
    printf("power limit: %u W (default %u W, range %u..%u W)\n", i->power_current, i->power_default,
           i->power_min, i->power_max);
    print_levels("pp_dpm_sclk", i->gfxclk_lv, i->gfxclk_levels, i->gfxclk_cur);
    print_levels("pp_dpm_mclk", i->uclk_lv, i->uclk_levels, i->uclk_cur);
    print_levels("pp_dpm_socclk", i->socclk_lv, i->socclk_levels, i->socclk_cur);
    print_levels("pp_dpm_fclk", i->fclk_lv, i->fclk_levels, i->fclk_cur);
    printf("sclk standard %u MHz, peak %u MHz; mclk peak %u MHz\n", i->gfxclk_standard, i->gfxclk_peak,
           i->uclk_peak);
    printf("profile: %u %s, workload mask 0x%x\n", i->power_profile_mode,
           i->power_profile_mode < RD_PP_SMC_POWER_PROFILE_COUNT ? kProfiles[i->power_profile_mode] : "?",
           i->workload_mask);
    printf("overdrive: %s, PMFW features 0x%x, ops 0x%x, PPT -%d..+%d %%\n",
           i->od_enabled ? "enabled" : "off (rdna3pmctl=2)", i->od_feature_ctrl, i->od_ops,
           i->od_ppt_pct_min, i->od_ppt_pct_max);
    if (i->od_valid) {
        printf("  OD_SCLK: %d..%d MHz   OD_MCLK: %d..%d MHz   OD_VDDGFX_OFFSET: %d mV   PPT %+d %%%s\n",
               i->od_gfxclk_min, i->od_gfxclk_max, i->od_uclk_min, i->od_uclk_max, i->od_vddgfx_offset,
               i->od_ppt, i->user_od ? "   (user settings)" : "");
        printf("  OD_FAN_CURVE (%s):", i->od_fan_mode ? "manual linear" : "auto");
        for (k = 0; k < 5; k++)
            printf(" %u: %uC %u%%", k, i->od_fan_curve_temp[k], i->od_fan_curve_pwm[k]);
        printf("\n  acoustic limit %u rpm, target %u rpm, fan target %u C, min pwm %u %%, zero rpm %u (stop %u C)\n",
               i->od_acoustic_limit, i->od_acoustic_target, i->od_fan_target_temp, i->od_fan_min_pwm,
               i->od_zero_rpm_enable, i->od_zero_rpm_stop_temp);
    } else {
        printf("  (overdrive table not read: rdna3pmctl=1 or 2)\n");
    }
    printf("  OD_RANGE: SCLK %d..%d MHz, MCLK %d..%d MHz, VDDGFX %d..%d mV, curve %d..%d C / %d..%d %%\n",
           i->od_lim_min[RD_OD_SET_GFXCLK_FMIN], i->od_lim_max[RD_OD_SET_GFXCLK_FMAX],
           i->od_lim_min[RD_OD_SET_UCLK_FMIN], i->od_lim_max[RD_OD_SET_UCLK_FMAX],
           i->od_lim_min[RD_OD_SET_GFX_VF_CURVE], i->od_lim_max[RD_OD_SET_GFX_VF_CURVE],
           i->od_lim_min[RD_OD_SET_FAN_CURVE_TEMP], i->od_lim_max[RD_OD_SET_FAN_CURVE_TEMP],
           i->od_lim_min[RD_OD_SET_FAN_CURVE_PWM], i->od_lim_max[RD_OD_SET_FAN_CURVE_PWM]);
    printf("thermal: edge %d C (emergency %d), hotspot %d C (emergency %d), mem %d C (emergency %d), shutdown %d C\n",
           i->thermal.max / 1000, i->thermal.edge_emergency_max / 1000, i->thermal.hotspot_crit_max / 1000,
           i->thermal.hotspot_emergency_max / 1000, i->thermal.mem_crit_max / 1000,
           i->thermal.mem_emergency_max / 1000, i->thermal.software_shutdown_temp);
    printf("unique id: 0x%016llx, BACO support 0x%x (1 BACO, 2 MACO), state %u, DF C-state %d\n",
           (unsigned long long)i->unique_id, i->baco_support, i->baco_state, (int)i->df_cstate);
    printf("gpu_metrics: edge %u C, hotspot %u C, mem %u C, vr gfx %u soc %u mem %u C\n",
           m->temperature_edge, m->temperature_hotspot, m->temperature_mem, m->temperature_vrgfx,
           m->temperature_vrsoc, m->temperature_vrmem);
    printf("  activity gfx %u %% umc %u %% mm %u %%, %u W, gfxclk %u MHz, uclk %u MHz, socclk %u MHz\n",
           m->average_gfx_activity, m->average_umc_activity, m->average_mm_activity, m->average_socket_power,
           m->average_gfxclk_frequency, m->average_uclk_frequency, m->current_socclk);
    printf("  fan %u rpm, PCIe x%u %u.%u GT/s, vddgfx %u mV soc %u mV mem %u mV, throttle 0x%x (indep 0x%llx)\n",
           m->current_fan_speed, m->pcie_link_width, m->pcie_link_speed / 10, m->pcie_link_speed % 10,
           m->voltage_gfx, m->voltage_soc, m->voltage_mem, m->throttle_status,
           (unsigned long long)m->indep_throttle_status);
}

static int od_edit(uint32_t type, uint32_t n, const int32_t *in, int commit, rdna3_pmctl_reply *rp)
{
    int32_t a[RD_PMCTL_MAX_ARGS];
    uint32_t i;
    int r;
    a[0] = (int32_t)type;
    for (i = 0; i < n; i++)
        a[1 + i] = in[i];
    r = pm_call(RD_PMCTL_OD_EDIT, n + 1, a, rp);
    if (r || !commit)
        return r;
    a[0] = RD_PP_OD_COMMIT_DPM_TABLE;
    return pm_call(RD_PMCTL_OD_EDIT, 1, a, rp);
}

static int od_name(const char *s)
{
    static const struct { const char *n; int t; } k[] = {
        { "fan-curve", RD_PP_OD_EDIT_FAN_CURVE }, { "acoustic-limit", RD_PP_OD_EDIT_ACOUSTIC_LIMIT },
        { "acoustic-target", RD_PP_OD_EDIT_ACOUSTIC_TARGET },
        { "fan-target-temp", RD_PP_OD_EDIT_FAN_TARGET_TEMPERATURE },
        { "fan-min-pwm", RD_PP_OD_EDIT_FAN_MINIMUM_PWM }, { "zero-rpm", RD_PP_OD_EDIT_FAN_ZERO_RPM_ENABLE },
        { "zero-rpm-stop", RD_PP_OD_EDIT_FAN_ZERO_RPM_STOP_TEMP },
    };
    unsigned i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (!strcmp(s, k[i].n))
            return k[i].t;
    return -1;
}

static int clk_name(const char *s)
{
    if (!strcmp(s, "sclk")) return RD_PP_SCLK;
    if (!strcmp(s, "mclk")) return RD_PP_MCLK;
    if (!strcmp(s, "socclk")) return RD_PP_SOCCLK;
    if (!strcmp(s, "fclk")) return RD_PP_FCLK;
    return -1;
}

static int profile_index(const char *s)
{
    int i;
    char *end;
    long v = strtol(s, &end, 0);
    if (*s && !*end)
        return (int)v;
    for (i = 0; i < RD_PP_SMC_POWER_PROFILE_COUNT; i++)
        if (!strcasecmp(s, kProfiles[i]))
            return i;
    return -1;
}

static void print_profiles(rdna3_pmctl_reply *rp)
{
    static const char *const wl[8] = { "DEFAULT", "FULL_SCREEN_3D", "POWER_SAVING", "VIDEO", "VR",
                                       "COMPUTE", "CUSTOM", "WINDOW_3D" };
    int32_t w;
    printf("%-16s %-6s %5s %7s %7s %7s %7s %10s %10s %10s\n", "WORKLOAD", "CLOCK", "FPS", "MinType",
           "MinFreq", "BstType", "BstFreq", "limit_c", "err", "err_rate");
    for (w = 0; w < 8; w++) {
        int r = pm_call(RD_PMCTL_PROFILE_COEFF, 1, &w, rp);
        if (r) {
            printf("%-16s %s\n", wl[w], errname(r));
            continue;
        }
        printf("%-16s %-6s %5d %7d %7d %7d %7d %10d %10d %10d\n", wl[w], "GFXCLK", rp->coeff.gfx[0],
               rp->coeff.gfx[1], rp->coeff.gfx[2], rp->coeff.gfx[3], rp->coeff.gfx[4], rp->coeff.gfx[5],
               rp->coeff.gfx[6], rp->coeff.gfx[7]);
        printf("%-16s %-6s %5d %7d %7d %7d %7d %10d %10d %10d\n", "", "FCLK", rp->coeff.fclk[0],
               rp->coeff.fclk[1], rp->coeff.fclk[2], rp->coeff.fclk[3], rp->coeff.fclk[4], rp->coeff.fclk[5],
               rp->coeff.fclk[6], rp->coeff.fclk[7]);
    }
}

/* ---- selftest: each write once, read back through the reply, undone ---- */

static int fails, checks;
#define T_CHECK(c, ...) do { checks++; if (c) printf("  ok    " __VA_ARGS__); \
                             else { fails++; printf("  FAIL  " __VA_ARGS__); } printf("\n"); } while (0)

static int selftest(void)
{
    static rdna3_pmctl_reply rp, boot;
    int32_t a[RD_PMCTL_MAX_ARGS];
    int r, w;

    r = pm_call(RD_PMCTL_INFO, 0, NULL, &boot);
    if (r) {
        printf("info: %d %s\nPMCTL: FAILED\n", r, errname(r));
        return 1;
    }
    print_info(&boot.info);
    if (!(boot.info.gates & RD_PMCTL_GATE_WRITE) || !(boot.info.gates & RD_PMCTL_GATE_ADMIN)) {
        printf("PMCTL: NOTHING TESTED (needs rdna3pmctl=1 and root)\n");
        return 1;
    }

    printf("== power limit\n");
    a[0] = (int32_t)boot.info.power_current - 10;
    r = pm_call(RD_PMCTL_POWER_LIMIT, 1, a, &rp);
    T_CHECK(r == 0 && rp.info.power_current == (uint32_t)a[0], "power limit %d W -> %d (GetPptLimit %u W)", a[0], r,
            rp.info.power_current);
    a[0] = (int32_t)boot.info.power_current;
    r = pm_call(RD_PMCTL_POWER_LIMIT, 1, a, &rp);
    T_CHECK(r == 0 && rp.info.power_current == boot.info.power_current, "power limit back to %d W -> %d", a[0], r);
    a[0] = (int32_t)boot.info.power_max + 1;
    T_CHECK(pm_call(RD_PMCTL_POWER_LIMIT, 1, a, &rp) == -22, "power limit %d W (above the range) refused", a[0]);

    printf("== fan\n");
    /* card run 33: PMFW 80.92 (SMU 13.0.10) answers the FAN_CONTROL
     * feature off with OK but keeps its own control, so the mode stays auto
     * and the PWM is refused; amdgpu moved these boards to the OD fan curve
     * (tested below). Reported, not counted. */
    a[0] = RD_FAN_CTRL_MANUAL;
    r = pm_call(RD_PMCTL_FAN_MODE, 1, a, &rp);
    if (r == 0 && rp.info.fan_mode == RD_FAN_CTRL_MANUAL) {
        T_CHECK(1, "fan manual -> %d (mode %u)", r, rp.info.fan_mode);
        a[0] = 128;
        r = pm_call(RD_PMCTL_FAN_PWM, 1, a, &rp);
        T_CHECK(r == 0, "fan pwm 128 -> %d", r);
        sleep(4);
        pm_call(RD_PMCTL_INFO, 0, NULL, &rp);
        printf("  info  after 4 s: pwm %u/255, %u rpm\n", rp.info.fan_pwm, rp.info.fan_rpm);
    } else {
        printf("  info  fan manual -> %d, mode stays %u: the PMFW keeps the fan (use the OD fan curve)\n", r,
               rp.info.fan_mode);
    }
    a[0] = RD_FAN_CTRL_AUTO;
    r = pm_call(RD_PMCTL_FAN_MODE, 1, a, &rp);
    T_CHECK(r == 0 && rp.info.fan_mode == RD_FAN_CTRL_AUTO, "fan auto -> %d (mode %u)", r, rp.info.fan_mode);

    printf("== profile\n");
    a[0] = RD_PP_SMC_POWER_PROFILE_COMPUTE;
    r = pm_call(RD_PMCTL_PROFILE, 1, a, &rp);
    T_CHECK(r == 0 && rp.info.workload_mask == (1u << 5), "profile COMPUTE -> %d (mask 0x%x)", r,
            rp.info.workload_mask);
    a[0] = RD_PP_SMC_POWER_PROFILE_BOOTUP_DEFAULT;
    r = pm_call(RD_PMCTL_PROFILE, 1, a, &rp);
    T_CHECK(r == 0 && rp.info.workload_mask == 1u, "profile BOOTUP_DEFAULT -> %d", r);
    for (w = 0; w < 8; w++) {
        r = pm_call(RD_PMCTL_PROFILE_COEFF, 1, &w, &rp);
        T_CHECK(r == 0, "activity monitor of workload %d -> %d (gfx FPS %d, MinActiveFreq %d)", w, r,
                rp.coeff.gfx[0], rp.coeff.gfx[2]);
    }

    printf("== clock levels\n");
    a[0] = RD_PP_SOCCLK; a[1] = 1;
    r = pm_call(RD_PMCTL_FORCE_LEVELS, 2, a, &rp);
    T_CHECK(r == 0, "socclk level 0 only -> %d", r);
    a[1] = (int32_t)((1u << boot.info.socclk_levels) - 1);
    r = pm_call(RD_PMCTL_FORCE_LEVELS, 2, a, &rp);
    T_CHECK(r == 0, "socclk levels 0x%x (all) again -> %d", a[1], r);

    printf("== DF C-states\n");
    a[0] = 0;
    T_CHECK(pm_call(RD_PMCTL_DF_CSTATE, 1, a, &rp) == 0 && rp.info.df_cstate == 0, "DF C-state disallow");
    a[0] = 1;
    T_CHECK(pm_call(RD_PMCTL_DF_CSTATE, 1, a, &rp) == 0 && rp.info.df_cstate == 1, "DF C-state allow");

    if (boot.info.gates & RD_PMCTL_GATE_OD) {
        printf("== overdrive\n");
        T_CHECK(boot.info.od_valid && boot.info.od_enabled, "OD table read, OD enabled (features 0x%x)",
                boot.info.od_feature_ctrl);
        if (boot.info.od_valid) {
            int32_t in[3];
            {
                int32_t s4[4] = { 0, boot.info.od_gfxclk_min, 1, boot.info.od_gfxclk_max - 100 };
                r = od_edit(RD_PP_OD_EDIT_SCLK_VDDC_TABLE, 4, s4, 1, &rp);
                T_CHECK(r == 0 && rp.info.od_gfxclk_max == boot.info.od_gfxclk_max - 100,
                        "OD sclk max %d MHz -> %d", s4[3], r);
            }
            in[0] = (int32_t)boot.info.od_zero_rpm_stop_temp + 5;
            r = od_edit(RD_PP_OD_EDIT_FAN_ZERO_RPM_STOP_TEMP, 1, in, 1, &rp);
            printf("  info  OD zero rpm stop %d C -> %d %s (needs the zero-fan OD feature)\n", in[0], r,
                   errname(r));
            /* a point inside OD_RANGE (the auto curve reads all 0, below
             * the limits, which smu_v13_0_0_od_edit_dpm_table refuses) */
            in[0] = 2;
            in[1] = (boot.info.od_lim_min[RD_OD_SET_FAN_CURVE_TEMP] + boot.info.od_lim_max[RD_OD_SET_FAN_CURVE_TEMP]) / 2;
            in[2] = (boot.info.od_lim_min[RD_OD_SET_FAN_CURVE_PWM] + boot.info.od_lim_max[RD_OD_SET_FAN_CURVE_PWM]) / 2;
            r = od_edit(RD_PP_OD_EDIT_FAN_CURVE, 3, in, 1, &rp);
            T_CHECK(r == 0 && rp.info.od_fan_mode == 1 && rp.info.od_fan_curve_pwm[2] == (uint32_t)in[2],
                    "OD fan curve point 2: %d C %d %% -> %d", in[1], in[2], r);
            sleep(4);
            pm_call(RD_PMCTL_INFO, 0, NULL, &rp);
            printf("  info  fan with the curve: %u rpm, pwm %u/255\n", rp.info.fan_rpm, rp.info.fan_pwm);
            in[0] = RD_PP_OD_EDIT_FAN_CURVE;
            r = od_edit(RD_PP_OD_RESTORE_DEFAULT_TABLE, 1, in, 0, &rp);
            T_CHECK(r == 0 && rp.info.od_fan_mode == 0, "OD fan curve restored -> %d", r);
            r = od_edit(RD_PP_OD_RESTORE_DEFAULT_TABLE, 0, NULL, 0, &rp);
            T_CHECK(r == 0 && rp.info.od_gfxclk_max == boot.info.od_gfxclk_max, "OD restore all -> %d", r);
        }
    }
    pm_call(RD_PMCTL_INFO, 0, NULL, &rp);
    printf("== after the test\n");
    print_info(&rp.info);
    printf("PMCTL: %s (%d of %d checks)\n", fails ? "FAILED" : "ALL PASSED", checks - fails, checks);
    return fails ? 1 : 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: rdna3_pmctl [info | fan auto|manual|full | fan pwm N | fan rpm N | power W |\n"
                    "       od sclk MIN MAX | od mclk MIN MAX | od vddgfx MV | od fan-curve I C PCT |\n"
                    "       od acoustic-limit|acoustic-target|fan-target-temp|fan-min-pwm|zero-rpm|zero-rpm-stop V |\n"
                    "       od restore [what] | profile N|NAME [9 custom values] | profiles |\n"
                    "       levels sclk|mclk|socclk|fclk MASK | dfcstate allow|disallow | baco-cycle |\n"
                    "       perflevel auto|low|high|manual|profile_standard|profile_min_sclk|profile_min_mclk|\n"
                    "                 profile_peak|profile_exit | selftest]\n");
    return 2;
}

int main(int argc, char **argv)
{
    static rdna3_pmctl_reply rp;
    int32_t a[RD_PMCTL_MAX_ARGS];
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RDNA3Accel"));
    const char *cmd = argc > 1 ? argv[1] : "info";
    int r = 0, i;

    if (!svc) {
        fprintf(stderr, "RDNA3Accel not found\n");
        return 1;
    }
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
        fprintf(stderr, "cannot open RDNA3Accel\n");
        IOObjectRelease(svc);
        return 1;
    }
    IOObjectRelease(svc);

    if (!strcmp(cmd, "info")) {
        r = pm_call(RD_PMCTL_INFO, 0, NULL, &rp);
        if (!r)
            print_info(&rp.info);
    } else if (!strcmp(cmd, "selftest")) {
        r = selftest();
        IOServiceClose(conn);
        return r;
    } else if (!strcmp(cmd, "fan") && argc >= 3) {
        if (!strcmp(argv[2], "auto") || !strcmp(argv[2], "manual") || !strcmp(argv[2], "full")) {
            a[0] = !strcmp(argv[2], "auto") ? RD_FAN_CTRL_AUTO
                 : !strcmp(argv[2], "manual") ? RD_FAN_CTRL_MANUAL : RD_FAN_CTRL_NONE;
            r = pm_call(RD_PMCTL_FAN_MODE, 1, a, &rp);
        } else if ((!strcmp(argv[2], "pwm") || !strcmp(argv[2], "rpm")) && argc >= 4) {
            a[0] = atoi(argv[3]);
            r = pm_call(!strcmp(argv[2], "pwm") ? RD_PMCTL_FAN_PWM : RD_PMCTL_FAN_RPM, 1, a, &rp);
        } else {
            return usage();
        }
    } else if (!strcmp(cmd, "power") && argc >= 3) {
        a[0] = atoi(argv[2]);
        r = pm_call(RD_PMCTL_POWER_LIMIT, 1, a, &rp);
    } else if (!strcmp(cmd, "od") && argc >= 3) {
        const char *w = argv[2];
        if ((!strcmp(w, "sclk") || !strcmp(w, "mclk")) && argc >= 5) {
            int32_t in[4] = { 0, atoi(argv[3]), 1, atoi(argv[4]) };
            r = od_edit(!strcmp(w, "sclk") ? RD_PP_OD_EDIT_SCLK_VDDC_TABLE : RD_PP_OD_EDIT_MCLK_VDDC_TABLE, 4,
                        in, 1, &rp);
        } else if (!strcmp(w, "vddgfx") && argc >= 4) {
            a[0] = atoi(argv[3]);
            r = od_edit(RD_PP_OD_EDIT_VDDGFX_OFFSET, 1, a, 1, &rp);
        } else if (!strcmp(w, "fan-curve") && argc >= 6) {
            int32_t in[3] = { atoi(argv[3]), atoi(argv[4]), atoi(argv[5]) };
            r = od_edit(RD_PP_OD_EDIT_FAN_CURVE, 3, in, 1, &rp);
        } else if (!strcmp(w, "restore")) {
            if (argc >= 4) {
                if (od_name(argv[3]) < 0)
                    return usage();
                a[0] = od_name(argv[3]);
                r = od_edit(RD_PP_OD_RESTORE_DEFAULT_TABLE, 1, a, 0, &rp);
            } else {
                r = od_edit(RD_PP_OD_RESTORE_DEFAULT_TABLE, 0, NULL, 0, &rp);
            }
        } else if (od_name(w) >= 0 && argc >= 4) {
            a[0] = atoi(argv[3]);
            r = od_edit((uint32_t)od_name(w), 1, a, 1, &rp);
        } else {
            return usage();
        }
    } else if (!strcmp(cmd, "profile") && argc >= 3) {
        a[0] = profile_index(argv[2]);
        if (a[0] < 0)
            return usage();
        if (argc - 2 > RD_PMCTL_MAX_ARGS)
            return usage();
        for (i = 3; i < argc; i++)
            a[i - 2] = atoi(argv[i]);
        r = pm_call(RD_PMCTL_PROFILE, (uint32_t)(argc - 2), a, &rp);
    } else if (!strcmp(cmd, "profiles")) {
        print_profiles(&rp);
        IOServiceClose(conn);
        return 0;
    } else if (!strcmp(cmd, "levels") && argc >= 4) {
        a[0] = clk_name(argv[2]);
        if (a[0] < 0)
            return usage();
        a[1] = (int32_t)strtoul(argv[3], NULL, 0);
        r = pm_call(RD_PMCTL_FORCE_LEVELS, 2, a, &rp);
    } else if (!strcmp(cmd, "dfcstate") && argc >= 3) {
        a[0] = !strcmp(argv[2], "allow");
        r = pm_call(RD_PMCTL_DF_CSTATE, 1, a, &rp);
    } else if (!strcmp(cmd, "perflevel") && argc >= 3) {
        /* amdgpu_set_power_dpm_force_performance_level's names */
        static const char *const lv[] = { "auto", "manual", "low", "high", "profile_standard",
                                          "profile_min_sclk", "profile_min_mclk", "profile_peak",
                                          "profile_exit" };
        a[0] = 0;
        for (i = 0; i < (int)(sizeof(lv) / sizeof(lv[0])); i++)
            if (!strcmp(argv[2], lv[i]))
                a[0] = (int32_t)(1u << i);
        if (!a[0])
            return usage();
        r = pm_call(RD_PMCTL_PERF_LEVEL, 1, a, &rp);
    } else if (!strcmp(cmd, "baco-cycle")) {
        r = pm_call(RD_PMCTL_BACO_CYCLE, 0, NULL, &rp);
        printf("BACO: %s\n", r ? "FAILED" : "cycle done, GPU up again");
    } else {
        return usage();
    }
    if (strcmp(cmd, "info"))
        printf("%s: %d %s\n", cmd, r, errname(r));
    IOServiceClose(conn);
    return r ? 1 : 0;
}
