/*
 * soc21_cg.c - clock gating outside GC on Navi 32: HDP memory shutdown
 * and ATHUB medium-grain clock gating + memory light sleep.
 *
 * amdgpu_device_set_cg_state(GATE) walks every IP's set_clockgating_state;
 * for GC 11.0.3 soc21_common_early_init sets cg_flags = GFX (CGCG, CGLS,
 * MGCG, REPEATER_FGCG; done in gfx11.c), VCN/JPEG MGCG (hw/vcn40.c: the
 * firmware gates VCN itself, JPEG_CGC_CTRL in jpeg40 start/stop),
 * HDP_SD, ATHUB_MGCG and ATHUB_LS. No MC (MMHUB), IH or BIF flags, so the
 * MMHUB 3.0 and IH 6.0 clock-gating paths are no-ops on this card and are
 * not ported (tools/progress/not-applicable.txt); NBIO 4.3's BIF gating only
 * ungates (rdna3amdgpu=1).
 *
 * Ported from (Linux v6.12, MIT):
 *   soc21_common_set_clockgating_state -> hdp_v6_0_update_clock_gating
 *   gmc_v11_0_set_clockgating_state    -> athub_v3_0_set_clockgating
 *
 * SPDX-License-Identifier: MIT
 */
#include "rdna3_hw.h"
#include "soc21_cg.h"
#include "gfx11.h"
#include "gen/athub_3_0_0_cg.h"
#include "gen/hdp_6_0_0_cg.h"
#include "gen/nbio_4_3_0_soc.h"
#include "doorbell.h"
#include "navi3x_regs.h"

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
    int h, a;
    /* rdna3amdgpu=1: soc21_common_set_clockgating_state on NBIO 4.3 runs the
     * BIF medium-grain gating and light sleep before HDP (no BIF cg_flags
     * on GC 11.0.3: the ungate direction clears them) */
    if (d->amdgpu_exact && d->ipver[HWIP_NBIO].present && d->ipver[HWIP_NBIO].major == 4 &&
        d->ipver[HWIP_NBIO].minor == 3 && d->ipver[HWIP_NBIO].rev <= 1) {
        nbio43_update_medium_grain_clock_gating(d, enable, 0);
        nbio43_update_medium_grain_light_sleep(d, enable, 0);
    }
    h = hdp_cg(d, enable);
    a = athub_cg(d, enable);
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

/* Ported from soc21_update_umd_stable_pstate */
void soc21_update_umd_stable_pstate(void *ctx, int enter)
{
    rdna3_dev *d = (rdna3_dev *)ctx;
    gfx11_rlc_safe_mode(d, enter);   /* amdgpu_gfx_rlc_enter_safe_mode / _exit_safe_mode */
}

/* ---- soc21 common block / NBIO 4.3 ---- */

/* pasted here so 'reg' is not expanded before ##_BASE_IDX */
#define RNB(d, reg)    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_NBIO, 0, reg##_BASE_IDX, reg))
#define WNB(d, reg, v) rdna3_wreg((d), rdna3_reg_offset((d), HWIP_NBIO, 0, reg##_BASE_IDX, reg), (v))

int rd_should_use_aspm(int amdgpu_aspm, int pcie_dpm, int link_aspm_enabled)
{
    switch (amdgpu_aspm) {
    case -1:
        break;
    case 0:
        return 0;
    case 1:
        return 1;
    default:
        return 0;
    }
    /* not an APU */
    if (!pcie_dpm)
        return 0;
    return link_aspm_enabled != 0;
}

/* nbio_v4_3_program_ltr */
static void nbio43_program_ltr(rdna3_dev *d, int ltr_path)
{
    uint32_t def, data;

    def = RNB(d, regRCC_EP_DEV0_0_EP_PCIE_TX_LTR_CNTL);
    data = 0x35EB;
    data &= ~(uint32_t)RCC_EP_DEV0_0_EP_PCIE_TX_LTR_CNTL__LTR_PRIV_MSG_DIS_IN_PM_NON_D0_MASK;
    data &= ~(uint32_t)RCC_EP_DEV0_0_EP_PCIE_TX_LTR_CNTL__LTR_PRIV_RST_LTR_IN_DL_DOWN_MASK;
    if (def != data)
        WNB(d, regRCC_EP_DEV0_0_EP_PCIE_TX_LTR_CNTL, data);

    def = data = RNB(d, regRCC_STRAP0_RCC_BIF_STRAP2);
    data &= ~(uint32_t)RCC_STRAP0_RCC_BIF_STRAP2__STRAP_LTR_IN_ASPML1_DIS_MASK;
    if (def != data)
        WNB(d, regRCC_STRAP0_RCC_BIF_STRAP2, data);

    def = data = RNB(d, regBIF_CFG_DEV0_EPF0_DEVICE_CNTL2);
    if (ltr_path)
        data |= BIF_CFG_DEV0_EPF0_DEVICE_CNTL2__LTR_EN_MASK;
    else
        data &= ~(uint32_t)BIF_CFG_DEV0_EPF0_DEVICE_CNTL2__LTR_EN_MASK;
    if (def != data)
        WNB(d, regBIF_CFG_DEV0_EPF0_DEVICE_CNTL2, data);
}

/* read-modify-write that writes only on a change (amdgpu's def / data) */
static void rmw(rdna3_dev *d, uint32_t off, uint32_t clr, uint32_t set)
{
    uint32_t def, data;
    def = data = rdna3_rreg(d, off);
    data &= ~clr;
    data |= set;
    if (def != data)
        rdna3_wreg(d, off, data);
}
#define RMW(reg, clr, set) \
    rmw(d, rdna3_reg_offset(d, HWIP_NBIO, 0, reg##_BASE_IDX, reg), (uint32_t)(clr), (uint32_t)(set))

int soc21_program_aspm(rdna3_dev *d, int should_use_aspm, uint32_t pcie_ver, int ltr_path)
{
    if (!should_use_aspm)
        return 0;
    /* nbio_v4_3_program_aspm (CONFIG_PCIEASPM) */
    if (pcie_ver != RD_IP_VERSION(7, 4, 0) && pcie_ver != RD_IP_VERSION(7, 6, 0))
        return 0;

    RMW(regPCIE_LC_CNTL, PCIE_LC_CNTL__LC_L1_INACTIVITY_MASK | PCIE_LC_CNTL__LC_L0S_INACTIVITY_MASK,
        PCIE_LC_CNTL__LC_PMI_TO_L1_DIS_MASK);
    RMW(regPCIE_LC_CNTL7, 0, PCIE_LC_CNTL7__LC_NBIF_ASPM_INPUT_EN_MASK);
    RMW(regPCIE_LC_CNTL3, 0, PCIE_LC_CNTL3__LC_DSC_DONT_ENTER_L23_AFTER_PME_ACK_MASK);
    RMW(regRCC_STRAP0_RCC_BIF_STRAP3,
        RCC_STRAP0_RCC_BIF_STRAP3__STRAP_VLINK_ASPM_IDLE_TIMER_MASK | RCC_STRAP0_RCC_BIF_STRAP3__STRAP_VLINK_PM_L1_ENTRY_TIMER_MASK, 0);
    RMW(regRCC_STRAP0_RCC_BIF_STRAP5, RCC_STRAP0_RCC_BIF_STRAP5__STRAP_VLINK_LDN_ENTRY_TIMER_MASK, 0);
    RMW(regBIF_CFG_DEV0_EPF0_DEVICE_CNTL2, BIF_CFG_DEV0_EPF0_DEVICE_CNTL2__LTR_EN_MASK, 0);
    WNB(d, regBIF_CFG_DEV0_EPF0_PCIE_LTR_CAP, 0x10011001);
    RMW(regPSWUSP0_PCIE_LC_CNTL2, PSWUSP0_PCIE_LC_CNTL2__LC_RCV_L0_TO_RCV_L0S_DIS_MASK,
        PSWUSP0_PCIE_LC_CNTL2__LC_ALLOW_PDWN_IN_L1_MASK | PSWUSP0_PCIE_LC_CNTL2__LC_ALLOW_PDWN_IN_L23_MASK);
    RMW(regPCIE_LC_CNTL4, 0, PCIE_LC_CNTL4__LC_L1_POWERDOWN_MASK);
    RMW(regPCIE_LC_RXRECOVER_RXSTANDBY_CNTL, 0, PCIE_LC_RXRECOVER_RXSTANDBY_CNTL__LC_RX_L0S_STANDBY_EN_MASK);

    nbio43_program_ltr(d, ltr_path);

    RMW(regRCC_STRAP0_RCC_BIF_STRAP3, 0,
        (0x5DE0u << RCC_STRAP0_RCC_BIF_STRAP3__STRAP_VLINK_ASPM_IDLE_TIMER__SHIFT) |
        (0x0010u << RCC_STRAP0_RCC_BIF_STRAP3__STRAP_VLINK_PM_L1_ENTRY_TIMER__SHIFT));
    RMW(regRCC_STRAP0_RCC_BIF_STRAP5, 0, 0x0010u << RCC_STRAP0_RCC_BIF_STRAP5__STRAP_VLINK_LDN_ENTRY_TIMER__SHIFT);
    RMW(regPCIE_LC_CNTL, PCIE_LC_CNTL__LC_PMI_TO_L1_DIS_MASK,
        (0x0u << PCIE_LC_CNTL__LC_L0S_INACTIVITY__SHIFT) | (0x9u << PCIE_LC_CNTL__LC_L1_INACTIVITY__SHIFT));
    RMW(regPCIE_LC_CNTL3, PCIE_LC_CNTL3__LC_DSC_DONT_ENTER_L23_AFTER_PME_ACK_MASK, 0);
    return 1;
}

void nbio43_init_registers(rdna3_dev *d)
{
    uint32_t data;
    const rdna3_ipver *v = &d->ipver[HWIP_NBIO];
    if (v->present && v->major == 4 && v->minor == 3 && v->rev == 0) {
        data = RNB(d, regRCC_DEV0_EPF2_STRAP2);
        data &= ~(uint32_t)RCC_DEV0_EPF2_STRAP2__STRAP_NO_SOFT_RESET_DEV0_F2_MASK;
        WNB(d, regRCC_DEV0_EPF2_STRAP2, data);
    }
}

/* nbio_v4_3_update_medium_grain_clock_gating */
void nbio43_update_medium_grain_clock_gating(rdna3_dev *d, int enable, int supported)
{
    const uint32_t m = CPM_CONTROL__LCLK_DYN_GATE_ENABLE_MASK | CPM_CONTROL__TXCLK_DYN_GATE_ENABLE_MASK |
                       CPM_CONTROL__TXCLK_LCNT_GATE_ENABLE_MASK | CPM_CONTROL__TXCLK_REGS_GATE_ENABLE_MASK |
                       CPM_CONTROL__TXCLK_PRBS_GATE_ENABLE_MASK | CPM_CONTROL__REFCLK_REGS_GATE_ENABLE_MASK;
    uint32_t def, data;
    if (enable && !supported)
        return;
    def = data = RNB(d, regCPM_CONTROL);
    if (enable)
        data |= m;
    else
        data &= ~m;
    if (def != data)
        WNB(d, regCPM_CONTROL, data);
}

/* nbio_v4_3_update_medium_grain_light_sleep */
void nbio43_update_medium_grain_light_sleep(rdna3_dev *d, int enable, int supported)
{
    uint32_t def, data;
    if (enable && !supported)
        return;
    def = data = RNB(d, regPCIE_CNTL2);
    if (enable)
        data |= PCIE_CNTL2__SLV_MEM_LS_EN_MASK;
    else
        data &= ~(uint32_t)PCIE_CNTL2__SLV_MEM_LS_EN_MASK;
    if (def != data)
        WNB(d, regPCIE_CNTL2, data);
}

void soc21_common_late_init(rdna3_dev *d)
{
    /* "Enable selfring doorbell aperture late because doorbell BAR
     * aperture will change if resize BAR successfully in gmc sw_init." */
    if (d->doorbell_ok)
        nbio43_enable_doorbell_selfring_aperture(d, 1);
}

void soc21_common_hw_fini(rdna3_dev *d)
{
    nbio43_enable_doorbell_aperture(d, 0);
    nbio43_enable_doorbell_selfring_aperture(d, 0);
}

int soc21_need_reset_on_init(rdna3_dev *d)
{
    /* sOS sign of life: the system driver and the sOS are already loaded */
    return RREG32_SOC15(d, MP0, 0, regMP0_SMN_C2PMSG_81) != 0;
}

int soc21_need_reset_on_resume(rdna3_dev *d, int in_s3, int suspend_complete)
{
    uint32_t a, b;
    if (!in_s3 || suspend_complete)
        return 0;
    a = RREG32_SOC15(d, MP0, 0, regMP0_SMN_C2PMSG_81);
    d->os.msleep(d->os.ctx, 100);
    b = RREG32_SOC15(d, MP0, 0, regMP0_SMN_C2PMSG_81);
    return a != b;
}
