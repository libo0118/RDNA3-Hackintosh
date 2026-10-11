/*
 * dcn32_hwseq.h - pieces of DCN 3.2's hardware sequencer and its helpers,
 * ported from Linux amdgpu Display Core (v6.12):
 *
 *   dcn32_hwseq.c   dcn32_resync_fifo_dccg_dio (after each stream is
 *                   programmed: the DIO FIFOs resynced to DISPCLK),
 *                   dcn32_calculate_dccg_k1_k2_values,
 *                   dcn32_is_dp_dig_pixel_rate_div_policy,
 *                   dcn32_enable_power_gating_plane
 *   dcn32_dccg.c    dccg32_trigger_dio_fifo_resync
 *   dcn32_clk_mgr.c dcn32_get_dispclk_from_dentist,
 *                   dcn32_are_clock_states_equal
 *   dcn32_hubp.c    hubp32_init
 *
 * The SYMCLK work-around of dcn32_disable_link_output
 * (apply_symclk_on_tx_off_wa) runs in hw/dcn32_modeset.c, which owns the
 * VBIOS pixel clock command.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_HWSEQ_H
#define RDNA3_DCN32_HWSEQ_H

#include "rdna3_hw.h"
#include "edid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* dcn32_get_dispclk_from_dentist: DISPCLK from DENTIST_DISPCLK_WDIVIDER at
 * DENTIST VCO 'vco_khz' (the divider ID clamped as dentist_get_divider_from_did does) */
uint32_t dcn32_get_dispclk_from_dentist(rdna3_dev *d, uint32_t vco_khz);

/* dccg32_trigger_dio_fifo_resync: DENTIST_DISPCLK_WDIVIDER rewritten with
 * the RDIVIDER the hardware runs (not when that reads 0). Returns the
 * divider written, 0 for none. */
uint32_t dcn32_dccg_trigger_dio_fifo_resync(rdna3_dev *d);
/* dcn32_resync_fifo_dccg_dio for this driver's streams: none is DPMS off
 * or virtual, so no OTG is stopped around the resync */
uint32_t dcn32_resync_fifo_dccg_dio(rdna3_dev *d);

/* enum pixel_rate_div */
enum { RD_PIXEL_RATE_DIV_BY_1 = 0, RD_PIXEL_RATE_DIV_BY_2 = 1, RD_PIXEL_RATE_DIV_BY_4 = 3,
       RD_PIXEL_RATE_DIV_NA = 0xf };
/* the signal families dcn32_calculate_dccg_k1_k2_values tells apart */
enum { RD_K12_SIGNAL_TMDS = 0, RD_K12_SIGNAL_DP, RD_K12_SIGNAL_DP_128B_132B };
/* dcn32_is_dp_dig_pixel_rate_div_policy (debug.enable_dp_dig_pixel_rate_div_policy
 * is 1 on DCN 3.2): a DP 8b/10b stream whose horizontal timing divides by 2 */
int  dcn32_is_dp_dig_pixel_rate_div_policy(uint32_t signal, const rdna3_mode *m);
/* dcn32_calculate_dccg_k1_k2_values: K1 / K2 (RD_PIXEL_RATE_DIV_*) of a
 * stream; 'ycbcr420' and 'two_pix_per_container' as the timing has them,
 * 'odm_combine_factor' (1 = none). Returns the ODM factor. */
uint32_t dcn32_calculate_dccg_k1_k2_values(uint32_t signal, const rdna3_mode *m, int ycbcr420,
                                           int two_pix_per_container, uint32_t odm_combine_factor,
                                           uint32_t *k1_div, uint32_t *k2_div);

/* dcn32_enable_power_gating_plane: DOMAIN0-3 (HUBP) and 16-19 (DSC)
 * POWER_FORCEON cleared (enable) or set, inside an IP request */
void dcn32_enable_power_gating_plane(rdna3_dev *d, int enable);

/* hubp32_init: HUBPREQ_DEBUG_DB = 1 << 8 on HUBP 'inst' */
void dcn32_hubp_init(rdna3_dev *d, uint32_t inst);

/* struct dc_clocks, the part dcn32_are_clock_states_equal compares */
typedef struct rdna3_dc_clocks {
    uint32_t dispclk_khz, dppclk_khz, dcfclk_khz, dcfclk_deep_sleep_khz, dramclk_khz;
    int      p_state_change_support, fclk_p_state_change_support;
} rdna3_dc_clocks;
int  dcn32_are_clock_states_equal(const rdna3_dc_clocks *a, const rdna3_dc_clocks *b);

#ifdef __cplusplus
}
#endif
#endif
