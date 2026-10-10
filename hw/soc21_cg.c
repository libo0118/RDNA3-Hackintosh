/*
 * soc21_cg.c - clock gating outside GC on Navi 32: HDP memory shutdown
 * and ATHUB medium-grain clock gating + memory light sleep.
 *
 * amdgpu_device_set_cg_state(GATE) walks every IP's set_clockgating_state;
 * for GC 11.0.3 soc21_common_early_init sets cg_flags = GFX (CGCG, CGLS,
 * MGCG, REPEATER_FGCG; done in gfx11.c), VCN/JPEG MGCG (no VCN here),
 * HDP_SD, ATHUB_MGCG and ATHUB_LS. No MC (MMHUB), IH or BIF flags, so the
 * MMHUB 3.0, IH 6.0 and NBIO 4.3 clock-gating paths are no-ops on this card
 * and are not ported (tools/progress/not-applicable.txt).
 *
 * Ported from (Linux v6.12, MIT):
 *   soc21_common_set_clockgating_state -> hdp_v6_0_update_clock_gating
 *   gmc_v11_0_set_clockgating_state    -> athub_v3_0_set_clockgating
 *
 * SPDX-License-Identifier: MIT
 */
#include "rdna3_hw.h"
#include "soc21_cg.h"
#include "gen/athub_3_0_0_cg.h"
#include "gen/hdp_6_0_0_cg.h"

/* athub_v3_0.c: the register moved on 3.0.1 and 3.3.0 */
#define regATHUB_MISC_CNTL_V3_0_1 0x00d7
#define regATHUB_MISC_CNTL_V3_3_0 0x00d8

#define SETF(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | ((x) ? (uint32_t)reg##__##field##_MASK : 0))

/* athub_v3_0_get_cg_cntl / _set_cg_cntl: 0 when the version has no CG */
static uint32_t athub_cntl_reg(rdna3_dev *d)
{
    const rdna3_ipver *v = &d->ipver[HWIP_ATHUB];
    if (!v->present || v->major != 3)
        return 0;
    if (v->minor == 0 && v->rev == 1)
        return rdna3_reg_offset(d, HWIP_ATHUB, 0, regATHUB_MISC_CNTL_BASE_IDX, regATHUB_MISC_CNTL_V3_0_1);
    if (v->minor == 3 && v->rev == 0)
        return rdna3_reg_offset(d, HWIP_ATHUB, 0, regATHUB_MISC_CNTL_BASE_IDX, regATHUB_MISC_CNTL_V3_3_0);
    if (v->minor == 0 && v->rev <= 2)
        return rdna3_reg_offset(d, HWIP_ATHUB, 0, regATHUB_MISC_CNTL_BASE_IDX, regATHUB_MISC_CNTL);
    return 0;
}

/* athub_v3_0_update_medium_grain_clock_gating + _light_sleep */
static int athub_cg(rdna3_dev *d, int enable)
{
    uint32_t reg = athub_cntl_reg(d), def, data;
    if (!reg)
        return RDNA3_ENODEV;
    def = data = rdna3_rreg(d, reg);
    data = SETF(data, ATHUB_MISC_CNTL, CG_ENABLE, enable);
    if (def != data)
        rdna3_wreg(d, reg, data);
    def = data;
    data = SETF(data, ATHUB_MISC_CNTL, CG_MEM_LS_ENABLE, enable);
    if (def != data)
        rdna3_wreg(d, reg, data);
    return RDNA3_OK;
}

/* hdp_v6_0_update_clock_gating with cg_flags = HDP_SD */
static int hdp_cg(rdna3_dev *d, int enable)
{
    uint32_t clk, pwr;
    if (!d->ipver[HWIP_HDP].present || d->ipver[HWIP_HDP].major != 6)
        return RDNA3_ENODEV;
    clk = RREG32_SOC15(d, HDP, 0, regHDP_CLK_CNTL);
    pwr = RREG32_SOC15(d, HDP, 0, regHDP_MEM_POWER_CTRL);
    /* before the clock/power mode switch force the IPH & RC clock on */
    clk = SETF(clk, HDP_CLK_CNTL, RC_MEM_CLK_SOFT_OVERRIDE, 1);
    WREG32_SOC15(d, HDP, 0, regHDP_CLK_CNTL, clk);
    /* disable clock and power gating before any change */
    pwr = SETF(pwr, HDP_MEM_POWER_CTRL, ATOMIC_MEM_POWER_CTRL_EN, 0);
    pwr = SETF(pwr, HDP_MEM_POWER_CTRL, ATOMIC_MEM_POWER_LS_EN, 0);
    pwr = SETF(pwr, HDP_MEM_POWER_CTRL, ATOMIC_MEM_POWER_DS_EN, 0);
    pwr = SETF(pwr, HDP_MEM_POWER_CTRL, ATOMIC_MEM_POWER_SD_EN, 0);
    pwr = SETF(pwr, HDP_MEM_POWER_CTRL, RC_MEM_POWER_CTRL_EN, 0);
    pwr = SETF(pwr, HDP_MEM_POWER_CTRL, RC_MEM_POWER_LS_EN, 0);
    pwr = SETF(pwr, HDP_MEM_POWER_CTRL, RC_MEM_POWER_DS_EN, 0);
    pwr = SETF(pwr, HDP_MEM_POWER_CTRL, RC_MEM_POWER_SD_EN, 0);
    WREG32_SOC15(d, HDP, 0, regHDP_MEM_POWER_CTRL, pwr);
    if (enable) {
        /* only one of LS/DS/SD; this card has SD. IPH and RC CTRL_EN must
         * be set for the SRAM power modes */
        pwr = SETF(pwr, HDP_MEM_POWER_CTRL, ATOMIC_MEM_POWER_SD_EN, 1);
        pwr = SETF(pwr, HDP_MEM_POWER_CTRL, RC_MEM_POWER_SD_EN, 1);
        pwr = SETF(pwr, HDP_MEM_POWER_CTRL, ATOMIC_MEM_POWER_CTRL_EN, 1);
        pwr = SETF(pwr, HDP_MEM_POWER_CTRL, RC_MEM_POWER_CTRL_EN, 1);
        WREG32_SOC15(d, HDP, 0, regHDP_MEM_POWER_CTRL, pwr);
    }
    /* the override goes after the mode change */
    clk = SETF(clk, HDP_CLK_CNTL, RC_MEM_CLK_SOFT_OVERRIDE, 0);
    WREG32_SOC15(d, HDP, 0, regHDP_CLK_CNTL, clk);
    return RDNA3_OK;
}

int soc21_set_clock_gating(rdna3_dev *d, int enable)
{
    int h = hdp_cg(d, enable), a = athub_cg(d, enable);
    RLOG(d, "soc21 CG %s: HDP SD %s, ATHUB %u.%u.%u MGCG+LS %s", enable ? "on" : "off",
         h ? "n/a" : "done", d->ipver[HWIP_ATHUB].major, d->ipver[HWIP_ATHUB].minor,
         d->ipver[HWIP_ATHUB].rev, a ? "n/a" : "done");
    return h && a ? RDNA3_ENODEV : RDNA3_OK;
}

/* hdp_v6_0_get_clockgating_state + athub_v3_0_get_clockgating */
uint32_t soc21_clock_gating_state(rdna3_dev *d)
{
    uint32_t f = 0, reg = athub_cntl_reg(d), v;
    if (d->ipver[HWIP_HDP].present) {
        v = RREG32_SOC15(d, HDP, 0, regHDP_MEM_POWER_CTRL);
        if (v & HDP_MEM_POWER_CTRL__ATOMIC_MEM_POWER_SD_EN_MASK)
            f |= RD_SOC_CG_HDP_SD;
    }
    if (reg) {
        v = rdna3_rreg(d, reg);
        if (v & ATHUB_MISC_CNTL__CG_ENABLE_MASK)
            f |= RD_SOC_CG_ATHUB_MGCG;
        if (v & ATHUB_MISC_CNTL__CG_MEM_LS_ENABLE_MASK)
            f |= RD_SOC_CG_ATHUB_LS;
    }
    return f;
}
