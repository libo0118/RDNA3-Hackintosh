/*
 * dcn32_hwseq.c - see dcn32_hwseq.h. Ported from Linux amdgpu Display Core
 * (hwss/dcn32/dcn32_hwseq.c, dccg/dcn32/dcn32_dccg.c,
 * clk_mgr/dcn32/dcn32_clk_mgr.c, hubp/dcn32/dcn32_hubp.c, core/dc_resource.c),
 * MIT, Copyright Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_hwseq.h"
#include "dcn32_clk.h"
#include "gen/dcn_3_2_0_hwseq.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define FLD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)
#define SET(v, reg, field, x) (((v) & ~(uint32_t)reg##__##field##_MASK) | FLD(reg, field, x))
/* paste reg##_BASE_IDX in the outermost macro, before 'reg' can expand */
#define A(d, reg) rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg)
#define R(d, reg) rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg))
#define W(d, reg, v) rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg), (v))

#define HUBP_DB_S (regHUBP1_HUBPREQ_DEBUG_DB - regHUBP0_HUBPREQ_DEBUG_DB)

/* DENTIST_DIVIDER_RANGE_SCALE_FACTOR */
#define DENTIST_SCALE 4u

uint32_t dcn32_get_dispclk_from_dentist(rdna3_dev *d, uint32_t vco_khz)
{
    uint32_t wdiv = GET(R(d, regDENTIST_DISPCLK_CNTL), DENTIST_DISPCLK_CNTL, DENTIST_DISPCLK_WDIVIDER);
    uint32_t div = dcn32_dentist_divider(wdiv);   /* dentist_get_divider_from_did */
    /* DISPCLK in kHz */
    return div ? (uint32_t)((uint64_t)DENTIST_SCALE * vco_khz / div) : 0;
}

uint32_t dcn32_dccg_trigger_dio_fifo_resync(rdna3_dev *d)
{
    uint32_t v = R(d, regDENTIST_DISPCLK_CNTL);
    uint32_t rdiv = GET(v, DENTIST_DISPCLK_CNTL, DENTIST_DISPCLK_RDIVIDER);
    /* not valid for the WDIVIDER to be set to 0 */
    if (rdiv != 0)
        W(d, regDENTIST_DISPCLK_CNTL, SET(v, DENTIST_DISPCLK_CNTL, DENTIST_DISPCLK_WDIVIDER, rdiv));
    return rdiv;
}

uint32_t dcn32_resync_fifo_dccg_dio(rdna3_dev *d)
{
    /* no OTG master of a DPMS-off or virtual stream to stop first, none to
     * bring back after */
    return dcn32_dccg_trigger_dio_fifo_resync(d);
}

/* is_h_timing_divisible_by_2 */
static int is_h_timing_divisible_by_2(const rdna3_mode *m)
{
    uint32_t h_blank_start = (m->h_total - m->h_front) & 0xffff;
    uint32_t h_blank_end = (h_blank_start - m->h_active) & 0xffff;
    /* HTOTAL, h blank start / end and h sync start / end even (h sync start is 0) */
    return m->h_total % 2 == 0 && h_blank_start % 2 == 0 && h_blank_end % 2 == 0 && m->h_sync % 2 == 0;
}

int dcn32_is_dp_dig_pixel_rate_div_policy(uint32_t signal, const rdna3_mode *m)
{
    if (!is_h_timing_divisible_by_2(m))
        return 0;
    /* dc->debug.enable_dp_dig_pixel_rate_div_policy = 1 (dcn32_resource.c) */
    return signal == RD_K12_SIGNAL_DP;
}

uint32_t dcn32_calculate_dccg_k1_k2_values(uint32_t signal, const rdna3_mode *m, int ycbcr420,
                                           int two_pix_per_container, uint32_t odm_combine_factor,
                                           uint32_t *k1_div, uint32_t *k2_div)
{
    *k1_div = RD_PIXEL_RATE_DIV_NA;
    *k2_div = RD_PIXEL_RATE_DIV_NA;
    if (signal == RD_K12_SIGNAL_DP_128B_132B) {
        *k1_div = RD_PIXEL_RATE_DIV_BY_1;
        *k2_div = RD_PIXEL_RATE_DIV_BY_1;
    } else if (signal == RD_K12_SIGNAL_TMDS) {
        *k1_div = RD_PIXEL_RATE_DIV_BY_1;
        *k2_div = ycbcr420 ? RD_PIXEL_RATE_DIV_BY_2 : RD_PIXEL_RATE_DIV_BY_4;
    } else if (signal == RD_K12_SIGNAL_DP) {
        if (two_pix_per_container) {
            *k1_div = RD_PIXEL_RATE_DIV_BY_1;
            *k2_div = RD_PIXEL_RATE_DIV_BY_2;
        } else {
            *k1_div = RD_PIXEL_RATE_DIV_BY_1;
            *k2_div = RD_PIXEL_RATE_DIV_BY_4;
            if (odm_combine_factor == 2 || dcn32_is_dp_dig_pixel_rate_div_policy(signal, m))
                *k2_div = RD_PIXEL_RATE_DIV_BY_2;
        }
    }
    return odm_combine_factor;
}

void dcn32_enable_power_gating_plane(rdna3_dev *d, int enable)
{
    uint32_t force_on = enable ? 0 : 1;   /* force on = power gating disabled */
    uint32_t org_ip_request_cntl = GET(R(d, regDC_IP_REQUEST_CNTL), DC_IP_REQUEST_CNTL, IP_REQUEST_EN);
    if (org_ip_request_cntl == 0)
        W(d, regDC_IP_REQUEST_CNTL, FLD(DC_IP_REQUEST_CNTL, IP_REQUEST_EN, 1));
    /* DCHUBP0/1/2/3 */
    W(d, regDOMAIN0_PG_CONFIG, SET(R(d, regDOMAIN0_PG_CONFIG), DOMAIN0_PG_CONFIG, DOMAIN_POWER_FORCEON, force_on));
    W(d, regDOMAIN1_PG_CONFIG, SET(R(d, regDOMAIN1_PG_CONFIG), DOMAIN1_PG_CONFIG, DOMAIN_POWER_FORCEON, force_on));
    W(d, regDOMAIN2_PG_CONFIG, SET(R(d, regDOMAIN2_PG_CONFIG), DOMAIN2_PG_CONFIG, DOMAIN_POWER_FORCEON, force_on));
    W(d, regDOMAIN3_PG_CONFIG, SET(R(d, regDOMAIN3_PG_CONFIG), DOMAIN3_PG_CONFIG, DOMAIN_POWER_FORCEON, force_on));
    /* DCS0/1/2/3 */
    W(d, regDOMAIN16_PG_CONFIG, SET(R(d, regDOMAIN16_PG_CONFIG), DOMAIN16_PG_CONFIG, DOMAIN_POWER_FORCEON, force_on));
    W(d, regDOMAIN17_PG_CONFIG, SET(R(d, regDOMAIN17_PG_CONFIG), DOMAIN17_PG_CONFIG, DOMAIN_POWER_FORCEON, force_on));
    W(d, regDOMAIN18_PG_CONFIG, SET(R(d, regDOMAIN18_PG_CONFIG), DOMAIN18_PG_CONFIG, DOMAIN_POWER_FORCEON, force_on));
    W(d, regDOMAIN19_PG_CONFIG, SET(R(d, regDOMAIN19_PG_CONFIG), DOMAIN19_PG_CONFIG, DOMAIN_POWER_FORCEON, force_on));
    if (org_ip_request_cntl == 0)
        W(d, regDC_IP_REQUEST_CNTL, 0);
}

void dcn32_hubp_init(rdna3_dev *d, uint32_t inst)
{
    rdna3_wreg(d, A(d, regHUBP0_HUBPREQ_DEBUG_DB) + HUBP_DB_S * inst, 1u << 8);
}

int dcn32_are_clock_states_equal(const rdna3_dc_clocks *a, const rdna3_dc_clocks *b)
{
    if (a->dispclk_khz != b->dispclk_khz)
        return 0;
    else if (a->dppclk_khz != b->dppclk_khz)
        return 0;
    else if (a->dcfclk_khz != b->dcfclk_khz)
        return 0;
    else if (a->dcfclk_deep_sleep_khz != b->dcfclk_deep_sleep_khz)
        return 0;
    else if (a->dramclk_khz != b->dramclk_khz)
        return 0;
    else if (a->p_state_change_support != b->p_state_change_support)
        return 0;
    else if (a->fclk_p_state_change_support != b->fclk_p_state_change_support)
        return 0;
    return 1;
}
