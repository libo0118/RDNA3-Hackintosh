/*
 * dcn32_pstate.c - see dcn32_pstate.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_pstate.h"
#include "dcn32_otg.h"
#include "dcn32_hpo.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_mode.h"
#include "gen/dcn_3_2_0_hwseq.h"
#include "gen/dcn_3_2_0_pstate.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))

/* paste reg##_BASE_IDX in the outermost macro, before 'reg' expands */
#define A(d, reg) rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg)
#define RR(d, reg) rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg))
#define WR(d, reg, v) rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg), (v))
#define HUBP_S (regHUBP1_DCHUBP_CNTL - regHUBP0_DCHUBP_CNTL)   /* 0xdc */
#define OTG_S  (regOTG1_OTG_CONTROL - regOTG0_OTG_CONTROL)     /* 0x80 */
#define RH(d, reg, n) rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + HUBP_S * (n))
#define WH(d, reg, n, v) rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + HUBP_S * (n), (v))
#define RO(d, reg, n) rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + OTG_S * (n))

/* dcn3_2_soc percentages / widths (dml/dcn32/dcn32_fpu.c) */
#define SOC_RETURN_BUS_BYTES          64
#define SOC_FABRIC_RETURN_BYTES       64
#define SOC_PCT_SDP_AFTER_URGENT      90
#define SOC_PCT_FABRIC_AFTER_URGENT   67
#define SOC_PCT_DRAM_PIXEL_ONLY       20    /* pct_ideal_dram_sdp_bw_after_urgent_pixel_only */
#define SOC_PCT_DRAM_STROBE           67    /* pct_ideal_dram_bw_after_urgent_strobe */
#define SOC_OOO_RETURN_PER_CHANNEL    4096  /* urgent_out_of_order_return_per_channel_* */
/* dcn3_2_ip */
#define IP_PIXEL_CHUNK_KB             8
#define IP_LINE_BUFFER_BITS           1171920u
#define IP_LB_FIXED_BPP               57u
#define IP_MAX_LB_LINES               32u
#define IP_CONFIG_RETURN_BUFFER_KB    1280u
#define IP_DPPCLK_DELAY_SUBTOTAL      47u
#define IP_DPPCLK_DELAY_SCL           50u
#define IP_DPPCLK_DELAY_SCL_LB_ONLY   16u
#define IP_DPPCLK_DELAY_CNVC_FORMATTER 28u
#define IP_DPPCLK_DELAY_CNVC_CURSOR   6u
#define IP_DISPCLK_DELAY_SUBTOTAL     125u
#define IP_MAX_INTER_DCN_TILE_REPEATERS 8u
#define DML_ARB_TO_RET_DELAY          (7 + 95)   /* __DML_ARB_TO_RET_DELAY__ */
#define DML_MAX_VRATIO_PRE            4          /* __DML_MAX_BW_RATIO_PRE__ */
/* dcn32_resource.h */
#define DCN3_2_MAX_DET_SEGS           18u
#define DCN3_2_DET_SEG_KB             64u
#define DCN3_2_MALL_MBLK_SIZE_BYTES   65536u
#define DCN3_2_DCFCLK_DS_INIT_KHZ     10000u
#define MIN_SUBVP_DCFCLK_KHZ          400000u
/* dc->caps (dcn32_resource_construct) */
#define CAPS_CACHE_LINE_SIZE          64u
#define CAPS_CACHE_NUM_WAYS           16u
#define CAPS_MALL_PER_CHANNEL_MB      4u
#define DCN32_DCHUB_REFCLK_MHZ        100u

static uint64_t div_ceil(uint64_t a, uint64_t b) { return b ? (a + b - 1) / b : 0; }
static uint32_t umin32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax32(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint64_t umin64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint64_t umax64(uint64_t a, uint64_t b) { return a > b ? a : b; }

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ======================================================================
 * The bounding box (dcn3_2_soc) and the VBIOS overrides
 * ====================================================================== */

void dcn32_pstate_soc_init(rdna3_dcn32_soc *soc, const rdna3_vbios *vb)
{
    uint32_t i;
    for (i = 0; i < sizeof(*soc); i++)
        ((uint8_t *)soc)[i] = 0;
    soc->dram_clock_change_latency_ns = 400000;
    soc->fclk_change_latency_ns = 25000;
    soc->sr_exit_ns = 42970;
    soc->sr_enter_plus_exit_ns = 49940;
    soc->urgent_latency_ns = 4000;
    soc->urgent_adj_fclk_component_ns = 1000;
    soc->urgent_adj_fclk_ref_mhz = 3000;
    soc->usr_retraining_ns = 2000;
    soc->smn_latency_ns = 2000;
    soc->round_trip_ping_cycles = 263;
    soc->num_chans = 24;
    soc->chan_width_bytes = 2;
    soc->dprefclk_ss_divider = 1000;
    if (vb) {
        uint16_t sz;
        uint8_t frev = 0, crev = 0;
        /* bios_parser_get_vram_info: umc_info v4.0 first, then vram_info
         * v3.0 (v2.3 - v2.5 keep the bounding box default) */
        const uint8_t *t = vbios_data_table(vb, RD_ATOM_DT_UMC_INFO, &sz, &frev, &crev);
        int got = 0;
        if (t && frev == 4 && crev == 0 && sz >= 66 && t[64]) {
            /* atom_umc_info_v4_0: channel_num at 64, channel_width at 65 */
            soc->num_chans = t[64];
            soc->chan_width_bytes = (1u << (t[65] & 31)) / 8;
            got = 1;
        }
        t = vbios_data_table(vb, RD_ATOM_DT_VRAM_INFO, &sz, &frev, &crev);
        if (!got && t && frev == 3 && crev == 0 && sz >= 27 && t[25]) {
            /* atom_vram_info_header_v3_0: channel_num 25, channel_width 26 */
            soc->num_chans = t[25];
            soc->chan_width_bytes = (1u << (t[26] & 31)) / 8;
        }
        if (!soc->chan_width_bytes)
            soc->chan_width_bytes = 2;
        /* get_soc_bb_info_v4_5: atom_display_controller_info_v4_5's
         * max_mclk_chg_lat / max_sr_exit_lat / max_sr_enter_exit_lat
         * (100 ns units) at 56 / 60 / 64; dcn32_update_bw_bounding_box_fpu
         * takes the non-zero ones */
        t = vbios_data_table(vb, RD_ATOM_DT_DCE_INFO, &sz, &frev, &crev);
        if (t && frev == 4 && crev == 5 && sz >= 68) {
            uint32_t mclk = rd32(t + 56), exit = rd32(t + 60), enter_exit = rd32(t + 64);
            if (mclk)
                soc->dram_clock_change_latency_ns = mclk * 100;
            if (enter_exit)
                soc->sr_enter_plus_exit_ns = enter_exit * 100;
            if (exit)
                soc->sr_exit_ns = exit * 100;
            soc->vbios_bb = mclk || exit || enter_exit;
        }
        /* dcn32_clock_read_ss_info: get_ss_entry_number is 1 on
         * atomfirmware; get_spread_spectrum_info(AS_SIGNAL_TYPE_GPU_PLL)
         * on dce_info v4.5 (get_ss_info_v4_5) is BP_RESULT_UNSUPPORTED ("SMU
         * Display PLL supposed to be without spread"): ss_on_dprefclk stays
         * false. The v4.1 - v4.4 tables are other ASICs'. */
        soc->ss_on_dprefclk = 0;
    }
    /* dcn32_calc_num_avail_chans_for_mall for GC 11.0.3: all 16 channels'
     * MALL when the part has all 16, else 8 */
    soc->mall_avail_chans = soc->num_chans == 16 ? 16 : 8;
    soc->max_cab_bytes = (uint64_t)soc->mall_avail_chans * CAPS_MALL_PER_CHANNEL_MB * 1024 * 1024;
}

/* ======================================================================
 * DAL mailbox messages (dcn32_clk_mgr_smu_msg.c, dcn30_clk_mgr_smu_msg.c)
 * ====================================================================== */

int dcn32_smu_send_fclk_pstate_message(rdna3_dev *d, int enable)
{
    const rdna3_ipver *dce = &d->ipver[HWIP_DCE];
    if (dce->present && dce->major == 3 && dce->minor == 2 && dce->rev == 1) {
        RERR(d, "DAL SMU: DCN 3.2.1 has no FCLK p-state notification");
        return RDNA3_EOPNOTSUPP;
    }
    RLOG(d, "DAL SMU: FCLK p-state support %d", enable);
    return dcn32_smu_send(d, RD_DALSMC_MSG_SetFclkSwitchAllow,
                          enable ? RD_FCLK_PSTATE_SUPPORTED : RD_FCLK_PSTATE_NOTSUPPORTED, NULL);
}

int dcn32_smu_send_cab_for_uclk_message(rdna3_dev *d, uint32_t num_ways)
{
    uint32_t param = (num_ways << 1) | (num_ways > 0);
    RLOG(d, "DAL SMU: %u MALL ways for SubVP", num_ways);
    return dcn32_smu_send(d, RD_DALSMC_MSG_SetCabForUclkPstate, param, NULL);
}

int dcn32_smu_transfer_wm_table_dram_2_smu(rdna3_dev *d)
{
    return dcn32_smu_send(d, RD_DALSMC_MSG_TransferTableDram2Smu, RD_DAL_TABLE_WATERMARKS, NULL);
}

int dcn32_smu_set_pme_workaround(rdna3_dev *d)
{
    return dcn32_smu_send(d, RD_DALSMC_MSG_BacoAudioD3PME, 0, NULL);
}

int dcn32_enable_pme_wa(rdna3_dev *d, const rdna3_dcn32_bw *bw)
{
    if (!bw->smu_present)
        return RDNA3_OK;
    return dcn32_smu_set_pme_workaround(d);
}

/* dcn30_smu_set_min_deep_sleep_dcef_clk */
int dcn32_smu_set_min_deep_sleep_dcfclk(rdna3_dev *d, uint32_t mhz)
{
    return dcn32_smu_send(d, RD_DALSMC_MSG_SetMinDeepSleepDcfclk, mhz, NULL);
}

/* dcn30_smu_set_hard_max_by_freq */
int dcn32_smu_set_hard_max_by_freq(rdna3_dev *d, uint32_t ppclk, uint32_t mhz)
{
    return dcn32_smu_send(d, RD_DALSMC_MSG_SetHardMaxByFreq, (ppclk << 16) | (mhz & 0xffff), NULL);
}

/* dcn30_smu_get_dpm_freq_by_index; 0 when the PMFW does not answer */
static uint32_t dpm_freq_by_index(rdna3_dev *d, uint32_t ppclk, uint32_t idx)
{
    uint32_t v = 0;
    if (dcn32_smu_send(d, RD_DALSMC_MSG_GetDpmFreqByIndex, (ppclk << 16) | (idx & 0xff), &v))
        return 0;
    return v;
}

/* dcn30_smu_get_dc_mode_max_dpm_freq */
static uint32_t dc_mode_max(rdna3_dev *d, uint32_t ppclk)
{
    uint32_t v = 0;
    if (dcn32_smu_send(d, RD_DALSMC_MSG_GetDcModeMaxDpmFreq, ppclk << 16, &v))
        return 0;
    return v;
}

/* dcn32_init_single_clock: level 0xff gives the count (bit 31: fine
 * grained, min and max), then each level */
static void init_single_clock(rdna3_dev *d, uint32_t ppclk, uint32_t *mhz, uint32_t *n)
{
    uint32_t ret = dpm_freq_by_index(d, ppclk, 0xff), i;
    *n = (ret & (1u << 31)) ? 2 : (ret & 0xff);
    if (*n > RD_DCN32_DPM_LEVELS)
        *n = RD_DCN32_DPM_LEVELS;
    for (i = 0; i < *n; i++)
        mhz[i] = dpm_freq_by_index(d, ppclk, i) & 0xffff;
}

int dcn32_pstate_clk_init(rdna3_dev *d, const rdna3_dcn32_soc *soc, rdna3_dcn32_bw *bw)
{
    uint32_t i, ver = 0;
    for (i = 0; i < sizeof(*bw); i++)
        ((uint8_t *)bw)[i] = 0;
    /* dcn32_init_clocks: smu_present when GetSmuVersion answers */
    if (dcn32_smu_send(d, RD_DALSMC_MSG_GetSmuVersion, 0, &ver))
        return RDNA3_ENODEV;
    bw->smu_present = 1;
    bw->pmfw = ver;
    init_single_clock(d, RD_PPCLK_DCFCLK, bw->dcfclk_mhz, &bw->n_dcfclk);
    bw->dcmode_dcfclk_mhz = dc_mode_max(d, RD_PPCLK_DCFCLK);
    init_single_clock(d, RD_PPCLK_SOCCLK, bw->socclk_mhz, &bw->n_socclk);
    bw->dcmode_socclk_mhz = dc_mode_max(d, RD_PPCLK_SOCCLK);
    /* debug.disable_dtb_ref_clk_switch is false on DCN 3.2 */
    init_single_clock(d, RD_PPCLK_DTBCLK, bw->dtbclk_mhz, &bw->n_dtbclk);
    bw->dcmode_dtbclk_mhz = dc_mode_max(d, RD_PPCLK_DTBCLK);
    init_single_clock(d, RD_PPCLK_DISPCLK, bw->dispclk_mhz, &bw->n_dispclk);
    bw->dcmode_dispclk_mhz = umin32(dc_mode_max(d, RD_PPCLK_DISPCLK), 1950);
    init_single_clock(d, RD_PPCLK_DPPCLK, bw->dppclk_mhz, &bw->n_dppclk);
    bw->dcmode_dppclk_mhz = umin32(dc_mode_max(d, RD_PPCLK_DPPCLK), 1950);
    bw->dpm_present = bw->n_dcfclk && bw->n_dtbclk && bw->n_dispclk;
    /* "HW recommends limit of 1950 MHz in display clock for all DCN3.2.x" */
    for (i = 0; i < bw->n_dispclk; i++)
        bw->dispclk_mhz[i] = umin32(bw->dispclk_mhz[i], 1950);
    for (i = 0; i < bw->n_dppclk; i++)
        bw->dppclk_mhz[i] = umin32(bw->dppclk_mhz[i], 1950);
    /* dcn32_get_memclk_states_from_smu */
    init_single_clock(d, RD_PPCLK_UCLK, bw->memclk_mhz, &bw->n_memclk);
    bw->dcmode_memclk_mhz = dc_mode_max(d, RD_PPCLK_UCLK);
    if (!bw->n_memclk)
        bw->n_memclk = 1;   /* "memclk must have at least one level" */
    init_single_clock(d, RD_PPCLK_FCLK, bw->fclk_mhz, &bw->n_fclk);
    bw->dcmode_fclk_mhz = dc_mode_max(d, RD_PPCLK_FCLK);
    bw->max_memclk_mhz = bw->memclk_mhz[bw->n_memclk - 1];
    if (bw->dpm_present && !umax32(bw->n_memclk, bw->n_fclk))
        bw->dpm_present = 0;
    /* dcn32_get_memclk_states_from_smu ends in update_bw_bounding_box:
     * dcn32_update_bw_bounding_box_fpu (the default, non-legacy path) */
    if (bw->memclk_mhz[0] && dcn32_build_soc_states(soc, bw))
        RERR(d, "pstate: no DCFCLK / DISPCLK / DTBCLK DPM levels for the bounding box");
    /* then dcn32_build_wm_range_table (it reads clock_limits[2]) */
    dcn32_build_wm_range_table(soc, bw);
    RLOG(d, "pstate: PMFW 0x%08x: DCFCLK %u..%u MHz (%u), UCLK %u..%u MHz (%u), FCLK %u..%u MHz (%u), "
         "DTBCLK %u MHz.., %u states, %u x %u-byte channels, latencies %u / %u / %u ns%s",
         bw->pmfw, bw->dcfclk_mhz[0], bw->dcfclk_mhz[bw->n_dcfclk ? bw->n_dcfclk - 1 : 0], bw->n_dcfclk,
         bw->memclk_mhz[0], bw->max_memclk_mhz, bw->n_memclk, bw->fclk_mhz[0],
         bw->fclk_mhz[bw->n_fclk ? bw->n_fclk - 1 : 0], bw->n_fclk, bw->dtbclk_mhz[0], bw->nstates,
         soc->num_chans, soc->chan_width_bytes, soc->dram_clock_change_latency_ns, soc->sr_exit_ns,
         soc->sr_enter_plus_exit_ns, soc->vbios_bb ? " (VBIOS)" : "");
    return RDNA3_OK;
}

/* dcn32_build_wm_range_table_fpu */
void dcn32_build_wm_range_table(const rdna3_dcn32_soc *soc, rdna3_dcn32_bw *bw)
{
    uint16_t min_uclk = (uint16_t)bw->memclk_mhz[0], min_dcfclk = (uint16_t)bw->dcfclk_mhz[0];
    uint16_t setb_min_uclk = min_uclk;
    /* dcfclk_mhz_for_the_second_state: dcn3_2_soc.clock_limits[2] */
    uint32_t dcfclk_second = bw->nstates > 2 ? bw->state[2].dcfclk_mhz : 0;
    uint32_t i;

    bw->wm[RD_WM_B].min_dcfclk = (uint16_t)(dcfclk_second ? dcfclk_second : bw->dcfclk_mhz[0]);
    if (bw->memclk_mhz[2])
        setb_min_uclk = (uint16_t)bw->memclk_mhz[2];
    for (i = 0; i < RD_WM_SET_COUNT; i++) {
        bw->wm[i].valid = 1;
        bw->wm[i].pstate_latency_ns = soc->dram_clock_change_latency_ns;
        bw->wm[i].fclk_change_latency_ns = soc->fclk_change_latency_ns;
        bw->wm[i].sr_exit_ns = soc->sr_exit_ns;
        bw->wm[i].sr_enter_plus_exit_ns = soc->sr_enter_plus_exit_ns;
        bw->wm[i].max_dcfclk = 0xffff;
        bw->wm[i].max_uclk = 0xffff;
        bw->wm[i].min_uclk = min_uclk;
        if (i != RD_WM_B)
            bw->wm[i].min_dcfclk = min_dcfclk;
    }
    /* Set A - normal, Set B - performance (DPM[2] DCFCLK / UCLK) */
    bw->wm[RD_WM_A].wm_type = RD_WATERMARKS_CLOCK_RANGE;
    bw->wm[RD_WM_B].wm_type = RD_WATERMARKS_CLOCK_RANGE;
    bw->wm[RD_WM_B].min_uclk = setb_min_uclk;
    /* Set C - dummy p-state (bb_overrides.dummy_clock_change_latency_ns
     * is 0 on DCN 3.2, not the 0x7FFFFFFF that turns it off) */
    bw->wm[RD_WM_C].pstate_latency_ns = 50000;
    bw->wm[RD_WM_C].wm_type = RD_WATERMARKS_DUMMY_PSTATE;
    for (i = 0; i < 4; i++)
        bw->dummy_dram_mts[i] = bw->memclk_mhz[i] * 16;
    bw->dummy_latency_us[0] = 50;
    bw->dummy_latency_us[1] = 9;
    bw->dummy_latency_us[2] = 8;
    bw->dummy_latency_us[3] = 5;
    /* Set D - MALL: the lowest dummy latency, SR times / 2 */
    bw->wm[RD_WM_D].pstate_latency_ns = bw->dummy_latency_us[3] * 1000;
    bw->wm[RD_WM_D].sr_exit_ns = soc->sr_exit_ns / 2;
    bw->wm[RD_WM_D].sr_enter_plus_exit_ns = soc->sr_enter_plus_exit_ns / 2;
    bw->wm[RD_WM_D].wm_type = RD_WATERMARKS_MALL;
}

void dcn32_wm_range_table(const rdna3_dcn32_bw *bw, uint8_t out[RD_WM_TABLE_BYTES])
{
    uint32_t i;
    for (i = 0; i < RD_WM_TABLE_BYTES; i++)
        out[i] = 0;
    /* only WmSetting and Flags (WatermarkRowGeneric_t of DCN 3.2) */
    for (i = 0; i < RD_WM_SET_COUNT; i++)
        if (bw->wm[i].valid) {
            out[4 * i] = (uint8_t)i;
            out[4 * i + 1] = bw->wm[i].wm_type;
        }
}

int dcn32_notify_wm_ranges(rdna3_dev *d, const rdna3_dcn32_bw *bw, uint64_t table_off)
{
    uint8_t t[RD_WM_TABLE_BYTES];
    uint64_t mc = d->fb_start + table_off;
    int r;
    if (!bw->smu_present)
        return RDNA3_ENODEV;
    dcn32_wm_range_table(bw, t);
    rdna3_vram_write(d, table_off, t, sizeof(t));
    rdna3_hdp_flush(d);
    /* dcn30_smu_set_dram_addr_high / _low, then the transfer */
    r = dcn32_smu_send(d, RD_DALSMC_MSG_SetDalDramAddrHigh, (uint32_t)(mc >> 32), NULL);
    if (!r)
        r = dcn32_smu_send(d, RD_DALSMC_MSG_SetDalDramAddrLow, (uint32_t)mc, NULL);
    if (!r)
        r = dcn32_smu_transfer_wm_table_dram_2_smu(d);
    RLOG(d, "pstate: watermark range table at MC 0x%llx to the PMFW: %d (A %u/%u B %u/%u C %u/%u D %u/%u)",
         (unsigned long long)mc, r, t[0], t[1], t[4], t[5], t[8], t[9], t[12], t[13]);
    return r;
}

/* ---- build_synthetic_soc_states ---- */

/* calculate_net_bw_in_kbytes_sec */
static uint64_t net_bw_kbps(const rdna3_dcn32_soc *soc, const rdna3_dcn32_state *e)
{
    uint64_t mem = (uint64_t)e->dram_mts * soc->num_chans * soc->chan_width_bytes * SOC_PCT_DRAM_PIXEL_ONLY * 10;
    uint64_t fab = (uint64_t)e->fclk_mhz * SOC_RETURN_BUS_BYTES * SOC_PCT_FABRIC_AFTER_URGENT * 10;
    uint64_t sdp = (uint64_t)e->dcfclk_mhz * SOC_RETURN_BUS_BYTES * SOC_PCT_SDP_AFTER_URGENT * 10;
    return umin64(mem, umin64(fab, sdp));
}

/* get_optimal_ntuple: the two other clocks matching the one given */
static void optimal_ntuple(const rdna3_dcn32_soc *soc, rdna3_dcn32_state *e)
{
    uint64_t chan = (uint64_t)soc->num_chans * soc->chan_width_bytes * SOC_PCT_DRAM_PIXEL_ONLY;
    if (e->dcfclk_mhz) {
        uint64_t bw = (uint64_t)e->dcfclk_mhz * SOC_RETURN_BUS_BYTES * SOC_PCT_SDP_AFTER_URGENT;   /* x100 */
        e->fclk_mhz = (uint32_t)(bw / (SOC_RETURN_BUS_BYTES * SOC_PCT_FABRIC_AFTER_URGENT));
        e->dram_mts = (uint32_t)(bw / chan);
    } else if (e->fclk_mhz) {
        uint64_t bw = (uint64_t)e->fclk_mhz * SOC_RETURN_BUS_BYTES * SOC_PCT_FABRIC_AFTER_URGENT;
        e->dcfclk_mhz = (uint32_t)(bw / (SOC_RETURN_BUS_BYTES * SOC_PCT_SDP_AFTER_URGENT));
        e->dram_mts = (uint32_t)(bw / chan);
    } else if (e->dram_mts) {
        uint64_t bw = (uint64_t)e->dram_mts * chan;
        e->fclk_mhz = (uint32_t)(bw / (SOC_RETURN_BUS_BYTES * SOC_PCT_FABRIC_AFTER_URGENT));
        e->dcfclk_mhz = (uint32_t)(bw / (SOC_RETURN_BUS_BYTES * SOC_PCT_SDP_AFTER_URGENT));
    }
}

static void insert_sorted(rdna3_dcn32_bw *bw, const rdna3_dcn32_state *e)
{
    uint32_t i, idx = 0;
    if (bw->nstates >= RD_DCN32_MAX_STATES)
        return;
    while (idx < bw->nstates && e->net_bw_kbps > bw->state[idx].net_bw_kbps)
        idx++;
    for (i = bw->nstates; i > idx; i--)
        bw->state[i] = bw->state[i - 1];
    bw->state[idx] = *e;
    bw->nstates++;
}

static void remove_at(rdna3_dcn32_bw *bw, uint32_t idx)
{
    uint32_t i;
    if (!bw->nstates || idx >= bw->nstates)
        return;
    for (i = idx; i + 1 < bw->nstates; i++)
        bw->state[i] = bw->state[i + 1];
    bw->nstates--;
}

int dcn32_build_soc_states(const rdna3_dcn32_soc *soc, rdna3_dcn32_bw *bw)
{
    static const uint32_t sta[5] = { 199, 615, 906, 1324, 1564 };
    uint32_t max_dcfclk = 0, max_fclk = 0, max_memclk = 0, max_dispclk = 0, max_dtbclk = 0;
    uint32_t n_uclk = 0, n_fclk = 0, n_dc_uclk = 0, n_dc_fclk = 0;
    uint32_t min_dcfclk = 199, min_fclk = 299, i, j;
    rdna3_dcn32_state e;

    for (i = 0; i < RD_DCN32_DPM_LEVELS; i++) {
        max_dcfclk = umax32(max_dcfclk, bw->dcfclk_mhz[i]);
        max_fclk = umax32(max_fclk, bw->fclk_mhz[i]);
        max_memclk = umax32(max_memclk, bw->memclk_mhz[i]);
        max_dispclk = umax32(max_dispclk, bw->dispclk_mhz[i]);
        max_dtbclk = umax32(max_dtbclk, bw->dtbclk_mhz[i]);
        if (bw->memclk_mhz[i]) {
            n_uclk++;
            if (bw->memclk_mhz[i] <= bw->dcmode_memclk_mhz)
                n_dc_uclk++;
        }
        if (bw->fclk_mhz[i]) {
            n_fclk++;
            if (bw->fclk_mhz[i] <= bw->dcmode_fclk_mhz)
                n_dc_fclk++;
        }
    }
    /* !disable_dc_mode_overwrite (DCN 3.2's default): the DC mode maxima
     * replace the maxima where reported (override_max_clk_values) and only
     * the levels within them count */
    if (bw->dcmode_dcfclk_mhz)
        max_dcfclk = bw->dcmode_dcfclk_mhz;
    if (bw->dcmode_fclk_mhz)
        max_fclk = bw->dcmode_fclk_mhz;
    if (bw->dcmode_memclk_mhz)
        max_memclk = bw->dcmode_memclk_mhz;
    if (bw->dcmode_dispclk_mhz)
        max_dispclk = bw->dcmode_dispclk_mhz;
    if (bw->dcmode_dtbclk_mhz)
        max_dtbclk = bw->dcmode_dtbclk_mhz;
    n_uclk = n_dc_uclk;
    n_fclk = n_dc_fclk;
    if (bw->n_dcfclk && bw->fclk_mhz[0] > min_fclk)
        min_fclk = bw->fclk_mhz[0];
    if (!max_dcfclk || !max_dispclk || !max_dtbclk)
        return RDNA3_EINVAL;
    if (!max_fclk)
        max_fclk = max_dcfclk * SOC_PCT_SDP_AFTER_URGENT / SOC_PCT_FABRIC_AFTER_URGENT;

    bw->nstates = 0;
    for (i = 0; i < 5; i++) {
        e.dcfclk_mhz = sta[i];
        e.fclk_mhz = e.dram_mts = 0;
        optimal_ntuple(soc, &e);
        e.net_bw_kbps = net_bw_kbps(soc, &e);
        insert_sorted(bw, &e);
    }
    e.dcfclk_mhz = max_dcfclk;
    e.fclk_mhz = e.dram_mts = 0;
    optimal_ntuple(soc, &e);
    e.net_bw_kbps = net_bw_kbps(soc, &e);
    insert_sorted(bw, &e);
    for (i = 0; i < n_uclk; i++) {
        e.dcfclk_mhz = e.fclk_mhz = 0;
        e.dram_mts = bw->memclk_mhz[i] * 16;
        optimal_ntuple(soc, &e);
        e.net_bw_kbps = net_bw_kbps(soc, &e);
        insert_sorted(bw, &e);
    }
    if (n_fclk > 2) {
        for (i = 0; i < n_fclk; i++) {
            e.dcfclk_mhz = e.dram_mts = 0;
            e.fclk_mhz = bw->fclk_mhz[i];
            optimal_ntuple(soc, &e);
            e.net_bw_kbps = net_bw_kbps(soc, &e);
            insert_sorted(bw, &e);
        }
    } else {
        e.dcfclk_mhz = e.dram_mts = 0;
        e.fclk_mhz = max_fclk;
        optimal_ntuple(soc, &e);
        e.net_bw_kbps = net_bw_kbps(soc, &e);
        insert_sorted(bw, &e);
    }
    /* states beyond what is supported */
    for (i = bw->nstates; i-- > 0;)
        if (bw->state[i].dcfclk_mhz > max_dcfclk || bw->state[i].fclk_mhz > max_fclk ||
            bw->state[i].dram_mts > max_memclk * 16)
            remove_at(bw, i);
    /* the max DC limits entry, then sort_entries_with_same_bw /
     * remove_inconsistent_entries */
    e.dcfclk_mhz = max_dcfclk;
    e.fclk_mhz = max_fclk;
    e.dram_mts = max_memclk * 16;
    e.net_bw_kbps = net_bw_kbps(soc, &e);
    insert_sorted(bw, &e);
    /* sort_entries_with_same_bw: a run of equal bandwidth by DCFCLK */
    for (i = 0; i + 1 < bw->nstates; i++) {
        uint32_t start = i, end, k, m;
        if (bw->state[i].net_bw_kbps != bw->state[i + 1].net_bw_kbps)
            continue;
        end = ++i;
        while (i + 1 < bw->nstates && bw->state[i + 1].net_bw_kbps == bw->state[start].net_bw_kbps)
            end = ++i;
        for (m = start; m < end; m++)
            for (k = start; k < end; k++)
                if (bw->state[k].dcfclk_mhz > bw->state[k + 1].dcfclk_mhz) {
                    rdna3_dcn32_state t = bw->state[k];
                    bw->state[k] = bw->state[k + 1];
                    bw->state[k + 1] = t;
                }
    }
    /* remove_inconsistent_entries: equal bandwidth with UCLK or FCLK going down */
    for (i = 0; i + 1 < bw->nstates; i++)
        if (bw->state[i].net_bw_kbps == bw->state[i + 1].net_bw_kbps &&
            (bw->state[i].dram_mts > bw->state[i + 1].dram_mts || bw->state[i].fclk_mhz > bw->state[i + 1].fclk_mhz))
            remove_at(bw, i);
    /* round UCLK up to a DPM level, FCLK up (coarse) or to the minimum */
    for (i = 0; i < bw->nstates; i++) {
        for (j = 0; j < n_uclk; j++)
            if (bw->memclk_mhz[j] * 16 >= bw->state[i].dram_mts) {
                bw->state[i].dram_mts = bw->memclk_mhz[j] * 16;
                break;
            }
        if (n_fclk > 2) {
            for (j = 0; j < n_fclk; j++)
                if (bw->fclk_mhz[j] >= bw->state[i].fclk_mhz) {
                    bw->state[i].fclk_mhz = bw->fclk_mhz[j];
                    break;
                }
        } else if (bw->state[i].fclk_mhz < min_fclk) {
            bw->state[i].fclk_mhz = min_fclk;
        }
        if (bw->state[i].dcfclk_mhz < min_dcfclk)
            bw->state[i].dcfclk_mhz = min_dcfclk;
    }
    /* duplicates are neighbours */
    for (i = 0; i + 1 < bw->nstates;) {
        if (bw->state[i].dcfclk_mhz == bw->state[i + 1].dcfclk_mhz &&
            bw->state[i].fclk_mhz == bw->state[i + 1].fclk_mhz && bw->state[i].dram_mts == bw->state[i + 1].dram_mts)
            remove_at(bw, i + 1);
        else
            i++;
    }
    return RDNA3_OK;
}

/* ======================================================================
 * The decision: a DML 3.2 subset for linear single-plane streams
 * ====================================================================== */

#define PS_PER_US 1000000ull

uint32_t dcn32_refresh_rate_ceil(const rdna3_pstate_plane *p)
{
    uint64_t f = (uint64_t)p->h_total * p->v_total;
    return f ? (uint32_t)(((uint64_t)p->pclk_khz * 1000 + f - 1) / f) : 0;
}

/* the line time (HTotal / PixelClock) in ps */
static uint64_t line_ps(const rdna3_pstate_plane *p)
{
    return p->pclk_khz ? (uint64_t)p->h_total * 1000000000ull / p->pclk_khz : 0;
}

/* one DPP's swath width: the viewport split over the ODM pipes */
static uint32_t swath_w(const rdna3_pstate_plane *p)
{
    uint32_t n = p->dpp_count ? p->dpp_count : 1;
    return (p->src_w + n - 1) / n;
}

/* VRatio / HRatio x 1000 (source / destination) */
static uint32_t vratio_x1000(const rdna3_pstate_plane *p)
{
    return p->v_active ? (uint32_t)((uint64_t)p->src_h * 1000 / p->v_active) : 1000;
}
static uint32_t hratio_x1000(const rdna3_pstate_plane *p)
{
    return p->h_active ? (uint32_t)((uint64_t)p->src_w * 1000 / p->h_active) : 1000;
}

/* the read bandwidth of a plane (ReadBandwidthLuma), MB/s:
 * SwathWidthY x BytePerPixel x VRatio / LineTime x DPPPerSurface */
static uint64_t read_bw(const rdna3_pstate_plane *p)
{
    uint64_t lt = line_ps(p);
    return lt ? (uint64_t)p->src_w * p->bpp * vratio_x1000(p) * PS_PER_US / (lt * 1000) : 0;
}

/* dml32_CalculateSurfaceSizeInMall for a linear surface whose viewport
 * moves (64 x 1 read blocks): ceil(min(w, vpw + 63), 64) x vph x bpp */
static uint64_t surface_size_in_mall(const rdna3_pstate_plane *p, uint32_t vp_h)
{
    return div_ceil(p->src_w + 63, 64) * 64 * vp_h * p->bpp;
}

/* dml32_CalculateDCFCLKDeepSleep for one plane, kHz: max(1.15 x
 * SwathWidth x Bpp / 64 / LineDeliveryTime, PixelClock / 16) with
 * LineDeliveryTime = SwathWidth x DPP / HRatio / PixelClock (VRatio <= 1;
 * the upscales this driver does) */
static uint64_t dcfclk_ds_plane_khz(const rdna3_pstate_plane *p)
{
    uint64_t a = 115ull * p->src_w * p->bpp * p->pclk_khz / (6400ull * (p->h_active ? p->h_active : 1));
    return umax64(a, p->pclk_khz / 16);
}

/* dml32_CalculateTWait (DRR displays are not set up by this driver) */
static uint64_t twait_ps(uint32_t prefetch_mode, uint32_t mall, uint64_t dram_ps, uint64_t fclk_ps, uint64_t urgent_ps,
                         uint64_t sr_enter_exit_ps)
{
    if (prefetch_mode == 0 && mall != RD_MALL_PSTATE_FULL_FRAME && mall != RD_MALL_PSTATE_SUB_VIEWPORT &&
        mall != RD_MALL_PSTATE_PHANTOM)
        return umax64(umax64(dram_ps + urgent_ps, sr_enter_exit_ps), urgent_ps);
    if (prefetch_mode <= 1 && mall != RD_MALL_PSTATE_PHANTOM)
        return umax64(umax64(fclk_ps + urgent_ps, sr_enter_exit_ps), urgent_ps);
    if (prefetch_mode <= 2 && mall != RD_MALL_PSTATE_PHANTOM)
        return umax64(sr_enter_exit_ps, urgent_ps);
    return urgent_ps;
}

/* the dml32_CalculatePrefetchSchedule terms of one linear plane without
 * GPUVM / DCC / dynamic metadata: 1 when it gets a prefetch schedule, its
 * prefetch bandwidth (MB/s) in *bw. VStartup is the maximum
 * (MaxVStartupLines = vblank - 1), PrefetchSourceLinesY 2
 * (dml32_CalculatePrefetchSourceLines with VInitPreFill 1 and one-line
 * swaths), Tvm / Tr0 the no-VM quarter / half lines. */
static int prefetch_ok(const rdna3_pstate_plane *p, uint64_t twait, uint64_t tcalc, uint64_t tsetup,
                       uint32_t dstx, uint32_t dsty, uint64_t *bw)
{
    uint64_t lt = line_ps(p), sw_bytes, bw_oto, equ_x4, oto_x4, lsw_x4, t, lines_x4;
    uint32_t vstartup = umin32(p->v_total - p->v_active - 1, 1023);
    int64_t equ_ps;
    if (!lt)
        return 0;
    /* prefetch_sw_bytes = PrefetchSourceLinesY x swath_width_luma_ub x Bpp */
    sw_bytes = 2ull * div_ceil(swath_w(p), 64) * 64 * p->bpp;
    /* prefetch_bw_oto = max(Bpp x PixelClock / DPP, sw_bytes / (2 lines)) */
    bw_oto = umax64((uint64_t)p->bpp * p->pclk_khz / 1000 / (p->dpp_count ? p->dpp_count : 1),
                    sw_bytes * PS_PER_US / (2 * lt));
    /* Lsw_oto = ceil(4 x max(sw_bytes / bw_oto / LineTime, min_Lsw = 1)) / 4 */
    t = bw_oto ? sw_bytes * PS_PER_US / bw_oto : 0;   /* ps */
    lsw_x4 = umax64(div_ceil(4 * t, lt), 4);
    /* dst_y_prefetch_oto = Tvm_oto_lines (1/4) + 2 x Tr0_oto_lines (1/2) + Lsw_oto */
    oto_x4 = 1 + 4 + lsw_x4;
    /* dst_y_prefetch_equ = VStartup - (TSetup + max(TWait + TCalc, Tdmdl)) / LineTime
     *                      - (DSTYAfterScaler + DSTXAfterScaler / HTotal), at most 63.75,
     * floor(4 x (x + 0.125)) / 4 */
    equ_ps = (int64_t)vstartup * (int64_t)lt - (int64_t)(tsetup + twait + tcalc) -
             (int64_t)dsty * (int64_t)lt - (int64_t)((uint64_t)dstx * lt / p->h_total);
    if (equ_ps <= 0)
        return 0;
    equ_x4 = ((uint64_t)equ_ps * 4 + lt / 2) / lt;
    if (equ_x4 > 255)
        equ_x4 = 255;
    if (equ_x4 <= 4)                       /* dst_y_prefetch_equ > 1 */
        return 0;
    if (oto_x4 < equ_x4) {
        /* the oto schedule: its bandwidth, LinesForPrefetchBandwidth = oto */
        lines_x4 = oto_x4 - 1 - 4;
        *bw = bw_oto;
    } else {
        /* PrefetchBandwidth4 = sw_bytes / (Tpre - Tvm_trips_rounded - 2 x Tr0_trips_rounded) */
        lines_x4 = equ_x4 > 5 ? equ_x4 - 5 : 0;
        *bw = lines_x4 ? sw_bytes * PS_PER_US * 4 / (lines_x4 * lt) : 0;
    }
    /* LinesToRequestPrefetchPixelData >= 1, VRatioPrefetchY = 2 / lines <= 4,
     * DestinationLinesForPrefetch >= 2 */
    if (lines_x4 < 4 || 2 * 4 > DML_MAX_VRATIO_PRE * lines_x4 || umax64(equ_x4, oto_x4) < 8)
        return 0;
    return 1;
}

int dcn32_dml_eval(const rdna3_dcn32_soc *soc, const rdna3_pstate_ctx *ctx, const rdna3_dcn32_state *st,
                   uint32_t vlevel, uint32_t dram_latency_ns, uint32_t fclk_latency_ns, uint32_t sr_exit_ns,
                   uint32_t sr_enter_exit_ns, rdna3_dml_wm_out *o)
{
    uint64_t sdp, fab, dram, rbw, total_rd = 0, ds_khz, extra_bytes, urgent, extra, u_wm, dram_wm, fclk_wm, usr_wm;
    uint64_t dram_ps = (uint64_t)dram_latency_ns * 1000, fclk_ps = (uint64_t)fclk_latency_ns * 1000;
    uint64_t hiding[RD_DCN_MAX_PIPES], lb_lines[RD_DCN_MAX_PIPES], det_lines[RD_DCN_MAX_PIPES];
    uint32_t k, pm, n = ctx->nplanes, dpp_total = 0, dispclk = ctx->dispclk_khz, dppclk = ctx->dppclk_khz;
    uint32_t number = 0, last_without = 0, method = 0, min_fclk_k = 0;
    int found_fclk = 0;
    int64_t min_fclk_margin = 0, second_min = 0;
    int same_timing_fclk = 1;
    uint32_t i;

    for (i = 0; i < sizeof(*o); i++)
        ((uint8_t *)o)[i] = 0;
    if (!n || !st->dcfclk_mhz || !st->fclk_mhz || !st->dram_mts)
        return 0;
    if (!dispclk)
        dispclk = 1000000;
    if (!dppclk)
        dppclk = dispclk;
    /* dml32_get_return_bw_mbps (no host VM): the DRAM at the strobe
     * efficiency below state 2 */
    sdp = (uint64_t)SOC_RETURN_BUS_BYTES * st->dcfclk_mhz * SOC_PCT_SDP_AFTER_URGENT / 100;
    fab = (uint64_t)st->fclk_mhz * SOC_FABRIC_RETURN_BYTES * SOC_PCT_FABRIC_AFTER_URGENT / 100;
    dram = (uint64_t)st->dram_mts * soc->num_chans * soc->chan_width_bytes *
           (vlevel < 2 ? SOC_PCT_DRAM_STROBE : SOC_PCT_DRAM_PIXEL_ONLY) / 100;
    rbw = umin64(sdp, umin64(fab, dram));
    o->return_bw = rbw;
    if (!rbw)
        return 0;
    /* dml32_CalculateDCFCLKDeepSleep: max(8 MHz, 1.15 x ReadBandwidth / 64, the planes') */
    for (k = 0; k < n; k++) {
        total_rd += read_bw(&ctx->plane[k]);
        dpp_total += ctx->plane[k].dpp_count ? ctx->plane[k].dpp_count : 1;
    }
    ds_khz = umax64(8000, 115ull * total_rd * 1000 / (100 * 64));
    for (k = 0; k < n; k++)
        ds_khz = umax64(ds_khz, dcfclk_ds_plane_khz(&ctx->plane[k]));
    o->dcfclk_ds_khz = (uint32_t)ds_khz;
    /* dml32_CalculateUrgentLatency with the fabric clock adjustment */
    {
        int64_t u = (int64_t)soc->urgent_latency_ns * 1000 +
                    (int64_t)soc->urgent_adj_fclk_component_ns * 1000 *
                        ((int64_t)soc->urgent_adj_fclk_ref_mhz * 1000 / (int64_t)st->fclk_mhz - 1000) / 1000;
        urgent = u > 0 ? (uint64_t)u : 0;
    }
    /* dml32_CalculateExtraLatency: (RoundTripPing + ARB_TO_RET) / DCFCLK +
     * (ReorderingBytes + DPPs x 8 KiB pixel chunk) / ReturnBW */
    extra_bytes = (uint64_t)soc->num_chans * SOC_OOO_RETURN_PER_CHANNEL + (uint64_t)dpp_total * IP_PIXEL_CHUNK_KB * 1024;
    extra = (uint64_t)(soc->round_trip_ping_cycles + DML_ARB_TO_RET_DELAY) * PS_PER_US / st->dcfclk_mhz +
            extra_bytes * PS_PER_US / rbw;
    o->urgent_latency_ns = (uint32_t)(urgent / 1000);
    o->extra_latency_ns = (uint32_t)(extra / 1000);
    /* the watermarks */
    u_wm = urgent + extra;
    usr_wm = u_wm + (uint64_t)soc->usr_retraining_ns * 1000 + (uint64_t)soc->smn_latency_ns * 1000;
    dram_wm = dram_ps + u_wm;
    fclk_wm = fclk_ps + u_wm;
    o->urgent_ns = (uint32_t)(u_wm / 1000);
    o->usr_ns = (uint32_t)(usr_wm / 1000);
    o->dram_ns = (uint32_t)(dram_wm / 1000);
    o->fclk_ns = (uint32_t)(fclk_wm / 1000);
    o->sr_exit_ns = (uint32_t)(((uint64_t)sr_exit_ns * 1000 + extra + 10ull * 1000000000ull / ds_khz) / 1000);
    o->sr_enter_exit_ns = (uint32_t)(((uint64_t)sr_enter_exit_ns * 1000 + extra + 10ull * 1000000000ull / ds_khz) / 1000);

    /* the PrefetchMode search (dm_prefetch_support_uclk_fclk_and_stutter_if_possible:
     * the lowest mode whose prefetch schedule and bandwidth fit) */
    o->prefetch_mode = 4;
    for (pm = 0; pm < 4; pm++) {
        uint64_t total = 0;
        int ok = 1;
        for (k = 0; k < n && ok; k++) {
            const rdna3_pstate_plane *p = &ctx->plane[k];
            uint64_t lt = line_ps(p), pf_bw = 0, tw, tcalc, tsetup, rep, upd_w, rdy, det, t_det, burst;
            uint32_t scaler = p->src_w != p->h_active || p->src_h != p->v_active, dpp_cycles, dstx, dsty;
            uint64_t pix;
            tw = twait_ps(pm, p->mall, dram_ps, fclk_ps, urgent, (uint64_t)sr_enter_exit_ns * 1000);
            /* TCalc = 24 / DCFClkDeepSleep */
            tcalc = 24ull * 1000000000ull / ds_khz;
            /* dml32_CalculateVUpdateAndDynamicMetadataParameters: TSetup =
             * (VUpdateOffsetPix + VUpdateWidthPix + VReadyOffsetPix) / PixelClock */
            rep = IP_MAX_INTER_DCN_TILE_REPEATERS * (2ull * 1000000000ull / dppclk + 3ull * 1000000000ull / dispclk);
            upd_w = div_ceil((14ull * 1000000000ull / ds_khz + 12ull * 1000000000ull / dppclk + rep) * p->pclk_khz,
                             1000000000ull);
            rdy = div_ceil(umax64(150ull * 1000000000ull / dppclk, rep + 20ull * 1000000000ull / ds_khz +
                                                                       10ull * 1000000000ull / dppclk) * p->pclk_khz,
                           1000000000ull);
            tsetup = (div_ceil(p->h_total, 4) + upd_w + rdy) * 1000000000ull / p->pclk_khz;
            /* DSTXAfterScaler = DPPCycles x PixelClock / Dppclk + DISPCLKCycles x
             * PixelClock / Dispclk (+ 18 with ODM combine) */
            dpp_cycles = IP_DPPCLK_DELAY_SUBTOTAL + IP_DPPCLK_DELAY_CNVC_FORMATTER +
                         (scaler ? IP_DPPCLK_DELAY_SCL : IP_DPPCLK_DELAY_SCL_LB_ONLY) + IP_DPPCLK_DELAY_CNVC_CURSOR;
            pix = (uint64_t)dpp_cycles * p->pclk_khz / dppclk + (uint64_t)IP_DISPCLK_DELAY_SUBTOTAL * p->pclk_khz / dispclk +
                  (p->dpp_count > 1 ? 18 : 0);
            dsty = (uint32_t)(pix / p->h_total);
            dstx = (uint32_t)(pix - (uint64_t)dsty * p->h_total);
            if (!prefetch_ok(p, tw, tcalc, tsetup, dstx, dsty, &pf_bw))
                ok = 0;
            /* dml32_CalculateUrgentBurstFactor, luma: the DET's time over the
             * urgent latency; no hiding -> not supported */
            det = (uint64_t)p->det_kb * 1024 / (p->dpp_count ? p->dpp_count : 1);
            t_det = (det / p->bpp / umax32(swath_w(p), 1)) * lt * 1000 / vratio_x1000(p);
            if (p->mall != RD_MALL_PSTATE_PHANTOM && t_det <= urgent)
                ok = 0;
            burst = t_det > urgent ? t_det * 1000 / (t_det - urgent) : 1000;
            total += umax64(read_bw(p) * burst / 1000, pf_bw);
        }
        if (ok && total <= rbw) {
            o->prefetch_mode = pm;
            o->total_rd_bw = total;
            break;
        }
    }
    o->vblank_ok = o->prefetch_mode < 4;

    /* dml32_CalculateWatermarksMALLUseAndDRAMSpeedChangeSupport */
    for (k = 0; k < n; k++) {
        const rdna3_pstate_plane *p = &ctx->plane[k];
        uint64_t lt = line_ps(p), det, lines, t_det, hide;
        uint32_t vtaps = (p->src_h != p->v_active) ? 2 : 1, sw = umax32(swath_w(p), 1);
        uint32_t hr = umax32(hratio_x1000(p), 1000);
        /* LBLatencyHidingSourceLinesY = min(MaxLineBufferLines,
         * floor(LineBufferSize / LBBitPerPixel / (SwathWidth / max(HRatio, 1)))) - (vtaps - 1) */
        lb_lines[k] = umin64(IP_MAX_LB_LINES, (uint64_t)IP_LINE_BUFFER_BITS / IP_LB_FIXED_BPP * hr / (1000ull * sw)) -
                      (vtaps - 1);
        /* no unbounded requesting for linear surfaces */
        det = (uint64_t)p->det_kb * 1024 / (p->dpp_count ? p->dpp_count : 1);
        lines = det / p->bpp / sw;      /* rounded down to the one-line swath */
        det_lines[k] = lines;
        t_det = lines * lt * 1000 / vratio_x1000(p);
        hide = lb_lines[k] * lt * 1000 / vratio_x1000(p) + t_det;
        {
            /* - (DSTXAfterScaler / HTotal + DSTYAfterScaler) x LineTime: the
             * same DPP / DISPCLK delay as above */
            uint32_t scaler = p->src_w != p->h_active || p->src_h != p->v_active;
            uint32_t dpp_cycles = IP_DPPCLK_DELAY_SUBTOTAL + IP_DPPCLK_DELAY_CNVC_FORMATTER +
                                  (scaler ? IP_DPPCLK_DELAY_SCL : IP_DPPCLK_DELAY_SCL_LB_ONLY) + IP_DPPCLK_DELAY_CNVC_CURSOR;
            uint64_t pix = (uint64_t)dpp_cycles * p->pclk_khz / dppclk +
                           (uint64_t)IP_DISPCLK_DELAY_SUBTOTAL * p->pclk_khz / dispclk + (p->dpp_count > 1 ? 18 : 0);
            uint64_t after = pix * lt / p->h_total;
            hide = hide > after ? hide - after : 0;
        }
        /* more than one surface: - (1 - 1 / N) x SwathHeight x LineTime / VRatio */
        if (n > 1) {
            uint64_t s = (uint64_t)(n - 1) * lt * 1000 / ((uint64_t)n * vratio_x1000(p));
            hide = hide > s ? hide - s : 0;
        }
        hiding[k] = hide;
        o->active_margin_ns[k] = (int32_t)(((int64_t)hide - (int64_t)u_wm - (int64_t)dram_wm) / 1000);
        o->fclk_margin_ns[k] = (int32_t)(((int64_t)hide - (int64_t)u_wm - (int64_t)fclk_wm) / 1000);
        /* SubViewportLinesNeededInMALL: src_y_pstate + src_y_ahead + meta rows (none) */
        {
            uint64_t dst_y_pstate = div_ceil(dram_ps + urgent, lt);
            uint64_t src_y_pstate = div_ceil(dst_y_pstate * vratio_x1000(p), 1000);
            o->subvp_lines[k] = (uint32_t)(src_y_pstate + det_lines[k] + lb_lines[k]);
        }
    }
    /* FCLK: the minimum active margin over the non-phantom surfaces; a
     * second, unsynchronised surface must have margin for vblank */
    for (k = 0; k < n; k++)
        if (ctx->plane[k].mall != RD_MALL_PSTATE_PHANTOM &&
            (!found_fclk || o->fclk_margin_ns[k] < min_fclk_margin)) {
            found_fclk = 1;
            min_fclk_margin = o->fclk_margin_ns[k];
            min_fclk_k = k;
        }
    for (k = 0; k < n; k++)
        if (k != min_fclk_k) {   /* SynchronizedSurfaces: only i == j for unsynchronised OTGs */
            if (ctx->plane[k].mall != RD_MALL_PSTATE_PHANTOM &&
                (same_timing_fclk || o->fclk_margin_ns[k] < second_min))
                second_min = o->fclk_margin_ns[k];
            same_timing_fclk = 0;
        }
    if (min_fclk_margin > 0)
        o->fclk_change = RD_FCLK_CHANGE_VACTIVE;
    else if ((same_timing_fclk || second_min > 0) && o->prefetch_mode <= 1)
        o->fclk_change = RD_FCLK_CHANGE_VBLANK;
    else
        o->fclk_change = RD_FCLK_CHANGE_UNSUPPORTED;
    /* USR retraining */
    o->usr_ok = 1;
    for (k = 0; k < n; k++)
        if (ctx->plane[k].mall != RD_MALL_PSTATE_PHANTOM && hiding[k] < usr_wm)
            o->usr_ok = 0;
    /* DRAMClockChangeSupportNumber */
    for (k = 0; k < n; k++) {
        uint32_t m = ctx->plane[k].mall;
        if (m != RD_MALL_PSTATE_FULL_FRAME && m != RD_MALL_PSTATE_SUB_VIEWPORT && m != RD_MALL_PSTATE_PHANTOM &&
            o->active_margin_ns[k] < 0) {
            if (o->prefetch_mode > 0)
                number = 2;
            else if (number == 0) {
                number = 1;
                last_without = k;
            } else if (number == 1 && last_without != k) {
                number = 2;   /* not synchronised */
            }
        }
    }
    for (k = 0; k < n; k++) {
        if (ctx->plane[k].mall == RD_MALL_PSTATE_FULL_FRAME)
            method = 1;
        else if (ctx->plane[k].mall == RD_MALL_PSTATE_SUB_VIEWPORT)
            method = 2;
    }
    if (method == 0)
        o->dram_change = number == 0 ? RD_DRAM_CHANGE_VACTIVE : number == 1 ? RD_DRAM_CHANGE_VBLANK
                                                                          : RD_DRAM_CHANGE_UNSUPPORTED;
    else if (method == 1)
        o->dram_change = number == 0 ? RD_DRAM_CHANGE_VACTIVE_W_MALL_FULL_FRAME
                       : number == 1 ? RD_DRAM_CHANGE_VBLANK_W_MALL_FULL_FRAME : RD_DRAM_CHANGE_UNSUPPORTED;
    else
        o->dram_change = number == 0 ? RD_DRAM_CHANGE_VACTIVE_W_MALL_SUB_VP
                       : number == 1 ? RD_DRAM_CHANGE_VBLANK_W_MALL_SUB_VP : RD_DRAM_CHANGE_UNSUPPORTED;
    return o->vblank_ok;
}

static void wm_from(const rdna3_dml_wm_out *o, rdna3_dcn_wm *w)
{
    w->urgent_ns = o->urgent_ns;
    w->urgent_latency_ns = o->urgent_latency_ns;
    w->cstate_enter_plus_exit_ns = o->sr_enter_exit_ns;
    w->cstate_exit_ns = o->sr_exit_ns;
    w->pstate_change_ns = o->dram_ns;
    w->fclk_pstate_change_ns = o->fclk_ns;
    w->usr_retraining_ns = o->usr_ns;
    /* FractionOfUrgentBandwidth x 1000 (the steady + prefetch read
     * bandwidth; no immediate flip: the flip fraction is the same) */
    w->frac_urg_bw_nom = o->return_bw ? (uint32_t)umin64(o->total_rd_bw * 1000 / o->return_bw, 0x3ff) : 0;
    w->frac_urg_bw_flip = w->frac_urg_bw_nom;
}

void dcn32_set_det_allocations(rdna3_pstate_ctx *ctx)
{
    uint32_t k, streams = 0, total = 0;
    for (k = 0; k < RD_DCN_MAX_PIPES; k++)
        ctx->det_segs[k] = 0;
    for (k = 0; k < ctx->nplanes; k++)
        if (ctx->plane[k].mall != RD_MALL_PSTATE_PHANTOM)
            streams++;
    for (k = 0; k < ctx->nplanes; k++) {
        rdna3_pstate_plane *p = &ctx->plane[k];
        uint32_t segs, n = p->dpp_count ? p->dpp_count : 1;
        if (p->mall == RD_MALL_PSTATE_PHANTOM) {
            /* "Phantom pipe requires that DET_SIZE = 0" */
            p->det_kb = 0;
            continue;
        }
        /* one pipe: DCN3_2_MAX_DET_SIZE (unbounded requesting only for
         * non-linear surfaces); else dcn32_determine_det_override: 18 /
         * streams per stream, per plane, split over the plane's pipes */
        segs = streams == 1 && n == 1 ? DCN3_2_MAX_DET_SEGS : (streams ? DCN3_2_MAX_DET_SEGS / streams : 4) / n;
        ctx->det_segs[p->pipe] = segs;
        if (n > 1 && p->odm_pipe < RD_DCN_MAX_PIPES)
            ctx->det_segs[p->odm_pipe] = segs;
        p->det_kb = segs * n * DCN3_2_DET_SEG_KB;
        total += segs * n;
    }
    /* compbuf_size_kb = config_return_buffer_size_in_kbytes - the DETs */
    ctx->compbuf_kb = IP_CONFIG_RETURN_BUFFER_KB - total * DCN3_2_DET_SEG_KB;
}

uint32_t dcn32_calculate_mall_ways_from_bytes(const rdna3_dcn32_soc *soc, uint64_t bytes)
{
    uint64_t lines_used, total_lines, per_way, ways;
    if (!bytes)
        return 0;
    lines_used = bytes / CAPS_CACHE_LINE_SIZE + 2;
    total_lines = soc->max_cab_bytes / CAPS_CACHE_LINE_SIZE;
    per_way = total_lines / CAPS_CACHE_NUM_WAYS;
    if (!per_way)
        return 0xff;
    ways = lines_used / per_way;
    if (lines_used % per_way)
        ways++;
    return (uint32_t)umin64(ways, 0xff);
}

uint32_t dcn32_helper_calculate_mall_bytes_for_cursor(uint32_t cursor_bytes, int ignore_cursor_buf)
{
    /* only beyond the DCN cursor buffer (16 KiB); one mblk extra for misalignment */
    if (cursor_bytes && (ignore_cursor_buf || cursor_bytes > 16384))
        return (uint32_t)((div_ceil(cursor_bytes, DCN3_2_MALL_MBLK_SIZE_BYTES) + 1) * DCN3_2_MALL_MBLK_SIZE_BYTES);
    return 0;
}

uint32_t dcn32_calculate_cab_allocation(const rdna3_dcn32_soc *soc, const rdna3_pstate_ctx *ctx)
{
    uint64_t bytes = ctx->mall_ss_size_bytes;
    uint32_t k;
    for (k = 0; k < ctx->nplanes; k++)
        if (ctx->plane[k].mall != RD_MALL_PSTATE_PHANTOM)
            bytes += dcn32_helper_calculate_mall_bytes_for_cursor(ctx->plane[k].cursor_bytes, 0);
    return dcn32_calculate_mall_ways_from_bytes(soc, bytes);
}

int dcn32_check_no_memory_request_for_cab(const rdna3_pstate_ctx *ctx)
{
    /* a stream with a plane and no PSR fails it; a blanked HUBP is this
     * driver's display sleep (no requests) */
    uint32_t k;
    for (k = 0; k < ctx->nplanes; k++)
        if (ctx->plane[k].mall != RD_MALL_PSTATE_PHANTOM && !ctx->plane[k].blanked)
            return 0;
    return 1;
}

int dcn32_pstate_validate(const rdna3_dcn32_soc *soc, const rdna3_dcn32_bw *bw, rdna3_pstate_ctx *ctx)
{
    rdna3_dml_wm_out o, tmp;
    uint32_t v, k, set_b, idx;
    int subvp = 0, dram_req, pstate_en, found = 0;

    if (!bw->nstates)
        return RDNA3_EINVAL;
    for (k = 0; k < ctx->nplanes; k++)
        if (ctx->plane[k].mall == RD_MALL_PSTATE_SUB_VIEWPORT || ctx->plane[k].mall == RD_MALL_PSTATE_PHANTOM)
            subvp = 1;
    /* dram_clock_change_requirement_final: true unless SubVP (or FPO) */
    dram_req = !subvp;
    for (v = 0; v < bw->nstates; v++) {
        if (!dcn32_dml_eval(soc, ctx, &bw->state[v], v, bw->wm[RD_WM_A].pstate_latency_ns,
                            bw->wm[RD_WM_A].fclk_change_latency_ns, bw->wm[RD_WM_A].sr_exit_ns,
                            bw->wm[RD_WM_A].sr_enter_plus_exit_ns, &o))
            continue;
        if (dram_req && v != bw->nstates - 1 && o.dram_change == RD_DRAM_CHANGE_UNSUPPORTED)
            continue;
        found = 1;
        break;
    }
    if (!found)
        return RDNA3_EINVAL;
    /* SubVP: "check that vlevel requested supports pstate or not, if not,
     * select the lowest vlevel that supports it" */
    if (subvp && o.dram_change == RD_DRAM_CHANGE_UNSUPPORTED) {
        uint32_t w;
        for (w = v + 1; w < bw->nstates; w++)
            if (dcn32_dml_eval(soc, ctx, &bw->state[w], w, bw->wm[RD_WM_A].pstate_latency_ns,
                               bw->wm[RD_WM_A].fclk_change_latency_ns, bw->wm[RD_WM_A].sr_exit_ns,
                               bw->wm[RD_WM_A].sr_enter_plus_exit_ns, &tmp) &&
                tmp.dram_change != RD_DRAM_CHANGE_UNSUPPORTED) {
                v = w;
                o = tmp;
                break;
            }
    }
    ctx->vlevel = v;
    ctx->dml = o;
    pstate_en = o.dram_change != RD_DRAM_CHANGE_UNSUPPORTED;
    ctx->fw_based_mclk_switching = 0;   /* FPO: no DRR stream (see the header) */
    if (subvp && !pstate_en) {
        /* the SubVP + DRR override of dcn32_calculate_wm_and_dlg_fpu */
        o.dram_change = RD_DRAM_CHANGE_VBLANK_W_MALL_SUB_VP;
        pstate_en = 1;
    }
    ctx->dram_change = o.dram_change;
    /* dcn32_calculate_dlg_params: the clocks */
    ctx->p_state_change_support = pstate_en;
    ctx->fclk_p_state_change_support = o.fclk_change != RD_FCLK_CHANGE_UNSUPPORTED;
    ctx->dcfclk_khz = bw->state[v].dcfclk_mhz * 1000;
    ctx->fclk_khz = bw->state[v].fclk_mhz * 1000;
    ctx->dramclk_khz = bw->state[v].dram_mts * 1000 / 16;
    ctx->socclk_khz = 0;
    ctx->dcfclk_deep_sleep_khz = o.dcfclk_ds_khz;
    ctx->ref_dtbclk_khz = bw->dtbclk_mhz[0] * 1000;   /* dtbclk_en false */
    /* dcn32_override_min_req_dcfclk */
    if (subvp && ctx->dcfclk_khz <= MIN_SUBVP_DCFCLK_KHZ)
        ctx->dcfclk_khz = MIN_SUBVP_DCFCLK_KHZ;

    /* Set B: clock_limits[2] (DCFCLK of state 2) with set B's latencies */
    set_b = bw->nstates > 2 ? 2 : 0;
    {
        rdna3_dcn32_state sb = bw->state[set_b];
        if (bw->nstates <= 2)
            sb.dcfclk_mhz = 615;   /* "DCFCLK Vmin_lv" */
        dcn32_dml_eval(soc, ctx, &sb, set_b, bw->wm[RD_WM_B].pstate_latency_ns, bw->wm[RD_WM_B].fclk_change_latency_ns,
                       bw->wm[RD_WM_B].sr_exit_ns, bw->wm[RD_WM_B].sr_enter_plus_exit_ns, &tmp);
        wm_from(&tmp, &ctx->wm[RD_WM_B]);
    }
    /* Set C: the dummy p-state latency of the UCLK level the state runs */
    idx = 0;
    if (subvp) {
        /* dcn32_find_dummy_latency_index_for_fw_based_mclk_switch: the
         * lowest dummy latency that still switches */
        for (idx = 0; idx < 4; idx++) {
            dcn32_dml_eval(soc, ctx, &bw->state[v], v, bw->dummy_latency_us[idx] * 1000,
                           bw->wm[RD_WM_A].fclk_change_latency_ns, bw->wm[RD_WM_A].sr_exit_ns,
                           bw->wm[RD_WM_A].sr_enter_plus_exit_ns, &tmp);
            if (tmp.dram_change != RD_DRAM_CHANGE_UNSUPPORTED)
                break;
        }
        if (idx == 4)
            idx = 3;
    } else {
        uint32_t min_dram = bw->state[v].dram_mts;
        if (o.dram_change == RD_DRAM_CHANGE_UNSUPPORTED)
            min_dram = bw->memclk_mhz[bw->n_memclk ? bw->n_memclk - 1 : 0] * 16;
        for (idx = 3; idx > 0; idx--)
            if (min_dram + 160 > bw->dummy_dram_mts[idx])
                break;
    }
    ctx->dummy_latency_index = idx;
    dcn32_dml_eval(soc, ctx, &bw->state[v], v, bw->dummy_latency_us[idx] * 1000, bw->wm[RD_WM_C].fclk_change_latency_ns,
                   bw->wm[RD_WM_C].sr_exit_ns, bw->wm[RD_WM_C].sr_enter_plus_exit_ns, &tmp);
    wm_from(&tmp, &ctx->wm[RD_WM_C]);
    /* "PMFW will set PSTATE_CHANGE_TYPE = 1 (FCLK) for UCLK dummy p-state":
     * the FCLK watermark of set C is the dummy DRAM one */
    ctx->wm[RD_WM_C].fclk_pstate_change_ns = ctx->wm[RD_WM_C].pstate_change_ns;
    if (!pstate_en) {
        /* A = C with no p-state watermark, the FCLK one from the FCLK latency */
        ctx->wm[RD_WM_A] = ctx->wm[RD_WM_C];
        ctx->wm[RD_WM_A].pstate_change_ns = 0;
        ctx->wm[RD_WM_A].fclk_pstate_change_ns = tmp.fclk_ns;
    } else {
        wm_from(&o, &ctx->wm[RD_WM_A]);
    }
    /* "Make set D = set A since we do not optimized watermarks for MALL" */
    ctx->wm[RD_WM_D] = ctx->wm[RD_WM_A];

    /* MALL sizes: the static screen holds every main surface, SubVP the phantoms */
    ctx->mall_ss_size_bytes = ctx->mall_subvp_size_bytes = 0;
    for (k = 0; k < ctx->nplanes; k++) {
        const rdna3_pstate_plane *p = &ctx->plane[k];
        if (p->mall == RD_MALL_PSTATE_PHANTOM)
            ctx->mall_subvp_size_bytes += surface_size_in_mall(p, p->v_active);
        else
            ctx->mall_ss_size_bytes += surface_size_in_mall(p, p->src_h);
    }
    /* dcn32_helper_calculate_num_ways_for_subvp */
    ctx->num_ways = ctx->mall_subvp_size_bytes ? dcn32_calculate_mall_ways_from_bytes(soc, ctx->mall_subvp_size_bytes) : 0;
    return RDNA3_OK;
}

const char *dcn32_dram_change_name(uint32_t c)
{
    static const char *const n[] = { "vactive", "vblank", "vblank (DRR)", "vactive + MALL full frame",
                                     "vblank + MALL full frame", "vactive + SubVP", "vblank + SubVP", "unsupported" };
    return c < sizeof(n) / sizeof(n[0]) ? n[c] : "?";
}


/* ======================================================================
 * DCHUBBUB (hubbub/dcn32/dcn32_hubbub.c)
 * ====================================================================== */

#define WM_SET_S (regDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_B - regDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A)
#define DET_S    (regDCHUBBUB_DET1_CTRL - regDCHUBBUB_DET0_CTRL)

/* convert_and_clamp (64-bit: DC's 32-bit product of the SubVP 4 s
 * watermark wraps and still lands above the clamp) */
static uint32_t convert_and_clamp(uint32_t wm_ns, uint32_t refclk_mhz, uint32_t clamp)
{
    uint64_t v = (uint64_t)wm_ns * refclk_mhz / 1000;
    return v > clamp ? clamp : (uint32_t)v;
}

/* one watermark of one set: the field fills the register (REG_SET) */
static void wm_write(rdna3_dev *d, uint32_t reg_a, uint32_t set, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, regDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A_BASE_IDX,
                                   reg_a + WM_SET_S * set), v);
}

/* the common shape of hubbub32_program_*_watermarks: raise now, lower only
 * when safe, else report it pending */
static int wm_update(rdna3_dev *d, uint32_t *cur, uint32_t want, int safe_to_lower, uint32_t reg_a, uint32_t set,
                     uint32_t value)
{
    if (safe_to_lower || want > *cur) {
        *cur = want;
        wm_write(d, reg_a, set, value);
        return 0;
    }
    return want < *cur;
}

int hubbub32_program_watermarks(rdna3_dev *d, rdna3_hubbub_wm *hw, const rdna3_dcn_wm wm[RD_WM_SET_COUNT],
                                uint32_t refclk_mhz, int safe_to_lower)
{
    int pending = 0;
    uint32_t s, v;
    /* hubbub32_program_urgent_watermarks */
    for (s = 0; s < RD_WM_SET_COUNT; s++) {
        rdna3_dcn_wm *c = &hw->set[s];
        const rdna3_dcn_wm *w = &wm[s];
        pending |= wm_update(d, &c->urgent_ns, w->urgent_ns, safe_to_lower, regDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A, s,
                             convert_and_clamp(w->urgent_ns, refclk_mhz, 0x3fff));
        pending |= wm_update(d, &c->frac_urg_bw_flip, w->frac_urg_bw_flip, safe_to_lower,
                             regDCHUBBUB_ARB_FRAC_URG_BW_FLIP_A, s, w->frac_urg_bw_flip);
        pending |= wm_update(d, &c->frac_urg_bw_nom, w->frac_urg_bw_nom, safe_to_lower,
                             regDCHUBBUB_ARB_FRAC_URG_BW_NOM_A, s, w->frac_urg_bw_nom);
        pending |= wm_update(d, &c->urgent_latency_ns, w->urgent_latency_ns, safe_to_lower,
                             regDCHUBBUB_ARB_REFCYC_PER_TRIP_TO_MEMORY_A, s,
                             convert_and_clamp(w->urgent_latency_ns, refclk_mhz, 0x3fff));
    }
    /* hubbub32_program_stutter_watermarks */
    for (s = 0; s < RD_WM_SET_COUNT; s++) {
        rdna3_dcn_wm *c = &hw->set[s];
        const rdna3_dcn_wm *w = &wm[s];
        pending |= wm_update(d, &c->cstate_enter_plus_exit_ns, w->cstate_enter_plus_exit_ns, safe_to_lower,
                             regDCHUBBUB_ARB_ALLOW_SR_ENTER_WATERMARK_A, s,
                             convert_and_clamp(w->cstate_enter_plus_exit_ns, refclk_mhz, 0xffff));
        pending |= wm_update(d, &c->cstate_exit_ns, w->cstate_exit_ns, safe_to_lower,
                             regDCHUBBUB_ARB_ALLOW_SR_EXIT_WATERMARK_A, s,
                             convert_and_clamp(w->cstate_exit_ns, refclk_mhz, 0xffff));
    }
    /* hubbub32_program_pstate_watermarks: UCLK, then FCLK */
    for (s = 0; s < RD_WM_SET_COUNT; s++)
        pending |= wm_update(d, &hw->set[s].pstate_change_ns, wm[s].pstate_change_ns, safe_to_lower,
                             regDCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A, s,
                             convert_and_clamp(wm[s].pstate_change_ns, refclk_mhz, 0xffff));
    for (s = 0; s < RD_WM_SET_COUNT; s++)
        pending |= wm_update(d, &hw->set[s].fclk_pstate_change_ns, wm[s].fclk_pstate_change_ns, safe_to_lower,
                             regDCHUBBUB_ARB_FCLK_PSTATE_CHANGE_WATERMARK_A, s,
                             convert_and_clamp(wm[s].fclk_pstate_change_ns, refclk_mhz, 0xffff));
    /* hubbub32_program_usr_watermarks */
    for (s = 0; s < RD_WM_SET_COUNT; s++)
        pending |= wm_update(d, &hw->set[s].usr_retraining_ns, wm[s].usr_retraining_ns, safe_to_lower,
                             regDCHUBBUB_ARB_USR_RETRAINING_WATERMARK_A, s,
                             convert_and_clamp(wm[s].usr_retraining_ns, refclk_mhz, 0x3fff));
    /* hubbub1_allow_self_refresh_control(!debug.disable_stutter): stutter
     * allowed = SELF_REFRESH_FORCE_ENABLE 0, FORCE_VALUE 0 */
    v = RR(d, regDCHUBBUB_ARB_DRAM_STATE_CNTL);
    v = SET(v, DCHUBBUB_ARB_DRAM_STATE_CNTL, DCHUBBUB_ARB_ALLOW_SELF_REFRESH_FORCE_VALUE, 0);
    v = SET(v, DCHUBBUB_ARB_DRAM_STATE_CNTL, DCHUBBUB_ARB_ALLOW_SELF_REFRESH_FORCE_ENABLE, 0);
    WR(d, regDCHUBBUB_ARB_DRAM_STATE_CNTL, v);
    /* hubbub32_force_usr_retraining_allow(debug.force_usr_allow = false);
     * kept forced when the mirror forced it */
    hubbub32_force_usr_retraining_allow(d, hw->usr_force_keep ? 1 : 0);
    return pending;
}

void hubbub32_init_watermarks(rdna3_dev *d)
{
    static const uint32_t regs[] = {
        regDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A, regDCHUBBUB_ARB_FRAC_URG_BW_FLIP_A,
        regDCHUBBUB_ARB_FRAC_URG_BW_NOM_A, regDCHUBBUB_ARB_REFCYC_PER_TRIP_TO_MEMORY_A,
        regDCHUBBUB_ARB_ALLOW_SR_ENTER_WATERMARK_A, regDCHUBBUB_ARB_ALLOW_SR_EXIT_WATERMARK_A,
        regDCHUBBUB_ARB_USR_RETRAINING_WATERMARK_A, regDCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A,
        regDCHUBBUB_ARB_FCLK_PSTATE_CHANGE_WATERMARK_A,
    };
    uint32_t i, s;
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint32_t v = rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, regDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A_BASE_IDX,
                                                    regs[i]));
        for (s = 1; s < RD_WM_SET_COUNT; s++)
            wm_write(d, regs[i], s, v);
    }
}

static uint32_t wm_read(rdna3_dev *d, uint32_t reg_a, uint32_t set)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, regDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A_BASE_IDX,
                                          reg_a + WM_SET_S * set));
}

void hubbub32_wm_read_state(rdna3_dev *d, rdna3_hubbub_wm_regs sets[RD_WM_SET_COUNT])
{
    uint32_t s;
    for (s = 0; s < RD_WM_SET_COUNT; s++) {
        sets[s].data_urgent = GET(wm_read(d, regDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A, s),
                                  DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A, DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A);
        sets[s].sr_enter = GET(wm_read(d, regDCHUBBUB_ARB_ALLOW_SR_ENTER_WATERMARK_A, s),
                               DCHUBBUB_ARB_ALLOW_SR_ENTER_WATERMARK_A, DCHUBBUB_ARB_ALLOW_SR_ENTER_WATERMARK_A);
        sets[s].sr_exit = GET(wm_read(d, regDCHUBBUB_ARB_ALLOW_SR_EXIT_WATERMARK_A, s),
                              DCHUBBUB_ARB_ALLOW_SR_EXIT_WATERMARK_A, DCHUBBUB_ARB_ALLOW_SR_EXIT_WATERMARK_A);
        sets[s].dram_clk_change = GET(wm_read(d, regDCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A, s),
                                      DCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A,
                                      DCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A);
        sets[s].usr_retrain = GET(wm_read(d, regDCHUBBUB_ARB_USR_RETRAINING_WATERMARK_A, s),
                                  DCHUBBUB_ARB_USR_RETRAINING_WATERMARK_A, DCHUBBUB_ARB_USR_RETRAINING_WATERMARK_A);
        sets[s].fclk_pstate_change = GET(wm_read(d, regDCHUBBUB_ARB_FCLK_PSTATE_CHANGE_WATERMARK_A, s),
                                         DCHUBBUB_ARB_FCLK_PSTATE_CHANGE_WATERMARK_A,
                                         DCHUBBUB_ARB_FCLK_PSTATE_CHANGE_WATERMARK_A);
    }
}

void hubbub32_force_wm_propagate_to_pipes(rdna3_dev *d, const rdna3_hubbub_wm *hw, uint32_t refclk_mhz)
{
    wm_write(d, regDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A, 0,
             convert_and_clamp(hw->set[RD_WM_A].urgent_ns, refclk_mhz, 0x3fff));
}

void hubbub32_force_usr_retraining_allow(rdna3_dev *d, int allow)
{
    uint32_t v = RR(d, regDCHUBBUB_ARB_USR_RETRAINING_CNTL);
    v = SET(v, DCHUBBUB_ARB_USR_RETRAINING_CNTL, DCHUBBUB_ARB_ALLOW_USR_RETRAINING_FORCE_VALUE, allow ? 1 : 0);
    v = SET(v, DCHUBBUB_ARB_USR_RETRAINING_CNTL, DCHUBBUB_ARB_ALLOW_USR_RETRAINING_FORCE_ENABLE, allow ? 1 : 0);
    WR(d, regDCHUBBUB_ARB_USR_RETRAINING_CNTL, v);
}

void dcn32_hubbub_force_pstate(rdna3_dev *d, int force, int allow)
{
    /* hubbub3_force_pstate_change_control */
    uint32_t v = RR(d, regDCHUBBUB_ARB_DRAM_STATE_CNTL);
    v = SET(v, DCHUBBUB_ARB_DRAM_STATE_CNTL, DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_VALUE, allow ? 1 : 0);
    v = SET(v, DCHUBBUB_ARB_DRAM_STATE_CNTL, DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_ENABLE, force ? 1 : 0);
    WR(d, regDCHUBBUB_ARB_DRAM_STATE_CNTL, v);
}

int hubbub32_get_mall_en(rdna3_dev *d)
{
    uint32_t v = RR(d, regDCHUBBUB_ARB_MALL_CNTL);
    return GET(v, DCHUBBUB_ARB_MALL_CNTL, MALL_IN_USE) && GET(v, DCHUBBUB_ARB_MALL_CNTL, MALL_PREFETCH_COMPLETE);
}

void dcn32_hubbub_read_crb(rdna3_dev *d, rdna3_hubbub_wm *hw)
{
    uint32_t p;
    /* dcn32_init_crb: the DET and compbuf sizes the hardware runs */
    for (p = 0; p < RD_DCN_MAX_PIPES; p++)
        hw->det_segs[p] = GET(rdna3_rreg(d, A(d, regDCHUBBUB_DET0_CTRL) + DET_S * p), DCHUBBUB_DET0_CTRL,
                              DET0_SIZE_CURRENT);
    hw->compbuf_segs = GET(RR(d, regDCHUBBUB_COMPBUF_CTRL), DCHUBBUB_COMPBUF_CTRL, COMPBUF_SIZE_CURRENT);
    /* hubbub2->crb_size_segs = config_return_buffer_size_in_kbytes / 64 */
    hw->crb_segs = IP_CONFIG_RETURN_BUFFER_KB / DCN3_2_DET_SEG_KB;
}

void dcn32_program_det_size(rdna3_dev *d, rdna3_hubbub_wm *hw, uint32_t pipe, uint32_t det_kb)
{
    uint32_t segs = (det_kb + DCN3_2_DET_SEG_KB - 1) / DCN3_2_DET_SEG_KB, a, v, k, sum = 0;
    if (pipe >= RD_DCN_MAX_PIPES)
        return;
    a = A(d, regDCHUBBUB_DET0_CTRL) + DET_S * pipe;
    v = rdna3_rreg(d, a);
    rdna3_wreg(d, a, SET(v, DCHUBBUB_DET0_CTRL, DET0_SIZE, segs));
    hw->det_segs[pipe] = segs;
    for (k = 0; k < RD_DCN_MAX_PIPES; k++)
        sum += hw->det_segs[k];
    if (sum + hw->compbuf_segs > hw->crb_segs)
        RERR(d, "pstate: CRB config warning: DET %u,%u,%u,%u + compbuf %u > %u segments", hw->det_segs[0],
             hw->det_segs[1], hw->det_segs[2], hw->det_segs[3], hw->compbuf_segs, hw->crb_segs);
}

int dcn32_program_compbuf_size(rdna3_dev *d, rdna3_hubbub_wm *hw, uint32_t compbuf_kb, int safe_to_increase)
{
    uint32_t segs = (compbuf_kb + DCN3_2_DET_SEG_KB - 1) / DCN3_2_DET_SEG_KB, p, v;
    int r = RDNA3_OK;
    if (!(safe_to_increase || segs <= hw->compbuf_segs))
        return RDNA3_OK;
    if (segs > hw->compbuf_segs) {
        /* the DETs must have shrunk first (REG_WAIT DETn_SIZE_CURRENT, 100 x 1 us) */
        for (p = 0; p < RD_DCN_MAX_PIPES; p++)
            if (rdna3_wait_reg(d, A(d, regDCHUBBUB_DET0_CTRL) + DET_S * p,
                               hw->det_segs[p] << DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT__SHIFT,
                               DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT_MASK, 0, 100))
                r = RDNA3_ETIME;
    }
    v = RR(d, regDCHUBBUB_COMPBUF_CTRL);
    WR(d, regDCHUBBUB_COMPBUF_CTRL, SET(v, DCHUBBUB_COMPBUF_CTRL, COMPBUF_SIZE, segs));
    hw->compbuf_segs = segs;
    if (GET(RR(d, regDCHUBBUB_COMPBUF_CTRL), DCHUBBUB_COMPBUF_CTRL, CONFIG_ERROR)) {
        RERR(d, "pstate: COMPBUF %u segments: CONFIG_ERROR", segs);
        r = RDNA3_EIO;
    }
    return r;
}

/* ======================================================================
 * HUBP (hubp/dcn32/dcn32_hubp.c)
 * ====================================================================== */

void hubp32_update_force_pstate_disallow(rdna3_dev *d, uint32_t pipe, int disallow)
{
    uint32_t v = RH(d, regHUBPREQ0_UCLK_PSTATE_FORCE, pipe);
    v = SET(v, HUBPREQ0_UCLK_PSTATE_FORCE, DATA_UCLK_PSTATE_FORCE_EN, disallow ? 1 : 0);
    v = SET(v, HUBPREQ0_UCLK_PSTATE_FORCE, DATA_UCLK_PSTATE_FORCE_VALUE, 0);
    WH(d, regHUBPREQ0_UCLK_PSTATE_FORCE, pipe, v);
}

void hubp32_update_force_cursor_pstate_disallow(rdna3_dev *d, uint32_t pipe, int disallow)
{
    uint32_t v = RH(d, regHUBPREQ0_UCLK_PSTATE_FORCE, pipe);
    v = SET(v, HUBPREQ0_UCLK_PSTATE_FORCE, CURSOR_UCLK_PSTATE_FORCE_EN, disallow ? 1 : 0);
    v = SET(v, HUBPREQ0_UCLK_PSTATE_FORCE, CURSOR_UCLK_PSTATE_FORCE_VALUE, 0);
    WH(d, regHUBPREQ0_UCLK_PSTATE_FORCE, pipe, v);
}

void hubp32_update_mall_sel(rdna3_dev *d, uint32_t pipe, uint32_t mall_sel, int cache_cursor)
{
    uint32_t v = RH(d, regHUBP0_DCHUBP_MALL_CONFIG, pipe);
    v = SET(v, HUBP0_DCHUBP_MALL_CONFIG, USE_MALL_SEL, mall_sel);
    v = SET(v, HUBP0_DCHUBP_MALL_CONFIG, USE_MALL_FOR_CURSOR, cache_cursor ? 1 : 0);
    WH(d, regHUBP0_DCHUBP_MALL_CONFIG, pipe, v);
}

/* ======================================================================
 * hwseq (hwss/dcn32/dcn32_hwseq.c)
 * ====================================================================== */

void dcn32_update_mall_sel(rdna3_dev *d, const rdna3_dcn32_soc *soc, const rdna3_pstate_ctx *ctx)
{
    uint32_t ways = dcn32_calculate_cab_allocation(soc, ctx), k;
    for (k = 0; k < ctx->nplanes; k++) {
        const rdna3_pstate_plane *p = &ctx->plane[k];
        /* cursor_size: pitch x height x 4 (ARGB); cached above 16 KiB */
        int cache_cursor = p->cursor_bytes > 16384;
        uint32_t sel = p->mall == RD_MALL_PSTATE_PHANTOM ? 1 : (ways <= CAPS_CACHE_NUM_WAYS ? 2 : 0);
        if (p->mall == RD_MALL_PSTATE_PHANTOM)
            cache_cursor = 0;
        hubp32_update_mall_sel(d, p->pipe, sel, cache_cursor);
        if (p->dpp_count > 1 && p->odm_pipe < RD_DCN_MAX_PIPES)
            hubp32_update_mall_sel(d, p->odm_pipe, sel, cache_cursor);
    }
}

void dcn32_update_force_pstate(rdna3_dev *d, const rdna3_pstate_ctx *ctx, const rdna3_pstate_ctx *old)
{
    uint32_t k, j;
    /* not SubVP main (nor FPO): unforced */
    for (k = 0; k < ctx->nplanes; k++) {
        const rdna3_pstate_plane *p = &ctx->plane[k];
        if (p->mall == RD_MALL_PSTATE_SUB_VIEWPORT)
            continue;
        hubp32_update_force_pstate_disallow(d, p->pipe, 0);
        hubp32_update_force_cursor_pstate_disallow(d, p->pipe, 0);
    }
    /* SubVP mains that were not before: forced disallow (the DMCUB moderates) */
    for (k = 0; k < ctx->nplanes; k++) {
        const rdna3_pstate_plane *p = &ctx->plane[k];
        int was = 0;
        if (p->mall != RD_MALL_PSTATE_SUB_VIEWPORT)
            continue;
        for (j = 0; old && j < old->nplanes; j++)
            if (old->plane[j].pipe == p->pipe && old->plane[j].mall == RD_MALL_PSTATE_SUB_VIEWPORT)
                was = 1;
        if (!was) {
            hubp32_update_force_pstate_disallow(d, p->pipe, 1);
            hubp32_update_force_cursor_pstate_disallow(d, p->pipe, 1);
        }
    }
}

void dcn32_interdependent_update_lock(rdna3_dev *d, const rdna3_pstate_ctx *ctx, int lock)
{
    uint32_t k;
    for (k = 0; k < ctx->nplanes; k++) {
        const rdna3_pstate_plane *p = &ctx->plane[k];
        if (p->mall == RD_MALL_PSTATE_PHANTOM || !dcn32_otg_is_enabled(d, p->otg))
            continue;
        dcn32_otg_lock(d, p->otg, lock);
    }
}

/* ======================================================================
 * The clock manager's p-state half (clk_mgr/dcn32/dcn32_clk_mgr.c)
 * ====================================================================== */

static int should_set_clock(int safe_to_lower, uint32_t calc, uint32_t cur)
{
    return (safe_to_lower && calc < cur) || calc > cur;
}

static int should_update_pstate_support(int safe_to_lower, int calc, int cur)
{
    if (cur != calc) {
        if (calc && safe_to_lower)
            return 1;
        if (!calc && !safe_to_lower)
            return 1;
    }
    return 0;
}

static uint32_t mhz_ceil(uint32_t khz) { return (khz + 999) / 1000; }

void dcn32_update_clocks_update_dtb_dto(rdna3_dev *d, uint32_t otg_mask, uint32_t ref_dtbclk_khz)
{
    uint32_t o;
    for (o = 0; o < RD_DCN_MAX_PIPES; o++)
        if (otg_mask & (1u << o))
            dcn32_dccg_set_dtbclk_dto(d, o, 0, ref_dtbclk_khz, 0);
}

int dcn32_clk_update_pstate(rdna3_dev *d, const rdna3_dcn32_bw *bw, rdna3_dcn32_clks *c, const rdna3_pstate_ctx *ctx,
                            int safe_to_lower, uint32_t otg_mask)
{
    int r = RDNA3_OK, e, update_uclk = 0, update_fclk;
    uint32_t got = 0, new_ways = ctx->num_ways;
    if (!bw->smu_present)
        return RDNA3_ENODEV;
#define MSG(x) do { e = (x); c->msgs++; if (e && !r) r = e; } while (0)
    /* NumOfDisplays: not sent (dcn32_smu_num_displays) */
    c->fclk_prev_p_state_change_support = c->fclk_p_state_change_support;
    if (should_update_pstate_support(safe_to_lower, ctx->fclk_p_state_change_support, c->fclk_p_state_change_support)) {
        c->fclk_p_state_change_support = ctx->fclk_p_state_change_support;
        /* FCLK switching allowed (DCN 3.2.0, not 3.2.1) */
        if (c->fclk_p_state_change_support &&
            !(d->ipver[HWIP_DCE].major == 3 && d->ipver[HWIP_DCE].minor == 2 && d->ipver[HWIP_DCE].rev == 1))
            MSG(dcn32_smu_send_fclk_pstate_message(d, 1));
    }
    if (should_set_clock(safe_to_lower, ctx->dcfclk_khz, c->dcfclk_khz)) {
        c->dcfclk_khz = ctx->dcfclk_khz;
        MSG(dcn32_smu_set_hard_min(d, RD_PPCLK_DCFCLK, mhz_ceil(c->dcfclk_khz), bw->pmfw, NULL));
    }
    if (should_set_clock(safe_to_lower, ctx->dcfclk_deep_sleep_khz, c->dcfclk_deep_sleep_khz)) {
        c->dcfclk_deep_sleep_khz = ctx->dcfclk_deep_sleep_khz;
        MSG(dcn32_smu_set_min_deep_sleep_dcfclk(d, mhz_ceil(c->dcfclk_deep_sleep_khz)));
    }
    if (should_set_clock(safe_to_lower, ctx->socclk_khz, c->socclk_khz))
        c->socclk_khz = ctx->socclk_khz;   /* "We don't actually care about socclk" */
    c->prev_p_state_change_support = c->p_state_change_support;
    c->prev_num_ways = c->num_ways;
    if (c->num_ways != new_ways && c->num_ways < new_ways) {
        c->num_ways = new_ways;
        MSG(dcn32_smu_send_cab_for_uclk_message(d, c->num_ways));
    }
    if (should_update_pstate_support(safe_to_lower, ctx->p_state_change_support, c->p_state_change_support)) {
        c->p_state_change_support = ctx->p_state_change_support;
        /* "to disable P-State switching, set UCLK min = max" (no DC mode softmax) */
        if (!c->p_state_change_support)
            MSG(dcn32_smu_set_hard_min(d, RD_PPCLK_UCLK, bw->max_memclk_mhz, bw->pmfw, NULL));
    }
    /* dcn32_smu_wait_for_dmub_ack_mclk */
    MSG(dcn32_smu_send(d, RD_DALSMC_MSG_SetAlwaysWaitDmcubResp, ctx->fw_based_mclk_switching ? 1 : 0, NULL));
    update_fclk = safe_to_lower && c->fclk_p_state_change_support != c->fclk_prev_p_state_change_support;
    if (!c->fclk_p_state_change_support && update_fclk &&
        !(d->ipver[HWIP_DCE].major == 3 && d->ipver[HWIP_DCE].minor == 2 && d->ipver[HWIP_DCE].rev == 1))
        MSG(dcn32_smu_send_fclk_pstate_message(d, 0));
    if (should_set_clock(safe_to_lower, ctx->dramclk_khz, c->dramclk_khz)) {
        c->dramclk_khz = ctx->dramclk_khz;
        update_uclk = 1;
    }
    /* UCLK at what the state asks while switching is supported, or to
     * switch again */
    if (c->p_state_change_support && (update_uclk || !c->prev_p_state_change_support))
        MSG(dcn32_smu_set_hard_min(d, RD_PPCLK_UCLK, mhz_ceil(c->dramclk_khz), bw->pmfw, NULL));
    if (c->num_ways != new_ways && c->num_ways > new_ways) {
        c->num_ways = new_ways;
        MSG(dcn32_smu_send_cab_for_uclk_message(d, c->num_ways));
    }
    /* DTBCLK: MHz precision compared, the PMFW's kHz answer kept */
    if (should_set_clock(safe_to_lower, ctx->ref_dtbclk_khz / 1000, c->ref_dtbclk_khz / 1000)) {
        e = dcn32_smu_set_hard_min(d, RD_PPCLK_DTBCLK, mhz_ceil(ctx->ref_dtbclk_khz), bw->pmfw, &got);
        c->msgs++;
        if (e && !r)
            r = e;
        c->ref_dtbclk_khz = got;
        dcn32_update_clocks_update_dtb_dto(d, otg_mask, c->ref_dtbclk_khz);
    }
#undef MSG
    c->fw_based_mclk_switching = ctx->fw_based_mclk_switching;
    c->valid = 1;
    return r;
}

int dcn32_set_hard_max_memclk(rdna3_dev *d, const rdna3_dcn32_bw *bw)
{
    if (!bw->smu_present)
        return RDNA3_ENODEV;
    return dcn32_smu_set_hard_max_by_freq(d, RD_PPCLK_UCLK, bw->max_memclk_mhz);
}

int dcn32_initialize_min_clocks(rdna3_dev *d, const rdna3_dcn32_bw *bw, rdna3_dcn32_clks *c)
{
    rdna3_pstate_ctx min;
    uint32_t i;
    for (i = 0; i < sizeof(min); i++)
        ((uint8_t *)&min)[i] = 0;
    min.dcfclk_deep_sleep_khz = DCN3_2_DCFCLK_DS_INIT_KHZ;
    min.dcfclk_khz = bw->dcfclk_mhz[0] * 1000;
    min.socclk_khz = bw->socclk_mhz[0] * 1000;
    min.dramclk_khz = bw->memclk_mhz[0] * 1000;
    min.ref_dtbclk_khz = bw->dtbclk_mhz[0] * 1000;
    min.fclk_p_state_change_support = 1;
    min.p_state_change_support = 1;
    /* DISPCLK: the dentist's (boot optimizations on); DPPCLK
     * (entries[0]) is not lowered: the GOP's pipes keep scanning (DC
     * powers them down first in init_pipes), hw/dcn32_clk.c only raises */
    /* dcn32_init_clocks: p-state and FCLK switching supported at the start */
    for (i = 0; i < sizeof(*c); i++)
        ((uint8_t *)c)[i] = 0;
    c->p_state_change_support = c->prev_p_state_change_support = 1;
    c->fclk_p_state_change_support = c->fclk_prev_p_state_change_support = 1;
    /* no stream in DC's state yet: no OTG for the DTB DTO */
    return dcn32_clk_update_pstate(d, bw, c, &min, 1, 0);
}

/* ======================================================================
 * DMCUB commands (dc_dmub_srv.c, dcn32_hwseq.c)
 * ====================================================================== */


int dcn32_dmub_p_state_delegate_off(rdna3_dmub *dm)
{
    /* struct dmub_rb_cmd_fw_assisted_mclk_switch: header, config_data
     * { fams_enabled 0, visual_confirm 0, vactive_stretch_margin_us 0,
     * pipe_data[4] } = 52 bytes of payload, all zero */
    uint8_t cmd[RD_DMUB_RB_CMD_SIZE];
    uint32_t i, hdr = RD_DMUB_HDR(RD_DMUB_CMD_FW_ASSISTED_MCLK_SWITCH, RD_DMUB_FAMS_SETUP_FW_CTRL, 52, 0);
    for (i = 0; i < sizeof(cmd); i++)
        cmd[i] = 0;
    cmd[0] = (uint8_t)hdr;
    cmd[1] = (uint8_t)(hdr >> 8);
    cmd[2] = (uint8_t)(hdr >> 16);
    cmd[3] = (uint8_t)(hdr >> 24);
    return dmub32_cmd_execute(dm, cmd, 0, 100000);
}

int dcn32_dmub_cab_cmd(rdna3_dmub *dm, uint32_t sub, uint32_t ways)
{
    /* struct dmub_rb_cmd_cab_for_ss: header, cab_alloc_ways, debug_bits */
    uint8_t cmd[RD_DMUB_RB_CMD_SIZE];
    uint32_t i, hdr = RD_DMUB_HDR(RD_DMUB_CMD_CAB_FOR_SS, sub, 2, 0);
    for (i = 0; i < sizeof(cmd); i++)
        cmd[i] = 0;
    cmd[0] = (uint8_t)hdr;
    cmd[1] = (uint8_t)(hdr >> 8);
    cmd[2] = (uint8_t)(hdr >> 16);
    cmd[3] = (uint8_t)(hdr >> 24);
    cmd[4] = (uint8_t)ways;
    return dmub32_cmd_execute(dm, cmd, 0, 100000);
}

int dcn32_apply_idle_power_optimizations(rdna3_dmub *dm, const rdna3_dcn32_soc *soc, const rdna3_pstate_ctx *ctx,
                                         int enable)
{
    uint32_t ways;
    if (!dm || !dm->hw_init)
        return 0;
    /* no PSR on these streams (no eDP) */
    if (!enable) {
        dcn32_dmub_cab_cmd(dm, RD_DMUB_CAB_NO_IDLE_OPTIMIZATION, 0);
        return 1;
    }
    if (dcn32_check_no_memory_request_for_cab(ctx)) {
        dcn32_dmub_cab_cmd(dm, RD_DMUB_CAB_NO_DCN_REQ, 0);
        return 1;
    }
    ways = dcn32_calculate_cab_allocation(soc, ctx);
    /* MALL is not for stereo or TMZ surfaces: neither exists here */
    if (ways <= CAPS_CACHE_NUM_WAYS) {
        dcn32_dmub_cab_cmd(dm, RD_DMUB_CAB_DCN_SS_FIT_IN_CAB, ways);
        return 1;
    }
    return 0;
}

/* ======================================================================
 * dcn32_init_hw's part, and the commit
 * ====================================================================== */

int dcn32_power_init(rdna3_dev *d, rdna3_dcn32_power *pw, const rdna3_vbios *vb, uint64_t wm_table_off,
                     int keep_force)
{
    uint32_t i, v;
    int r;
    for (i = 0; i < sizeof(*pw); i++)
        ((uint8_t *)pw)[i] = 0;
    pw->refclk_mhz = DCN32_DCHUB_REFCLK_MHZ;   /* hubbub2_get_dchub_ref_freq: 100 MHz */
    pw->wm_table_off = wm_table_off;
    pw->keep_force = keep_force;
    dcn32_pstate_soc_init(&pw->soc, vb);
    /* init_clocks */
    r = dcn32_pstate_clk_init(d, &pw->soc, &pw->bw);
    if (r) {
        RERR(d, "pstate: the PMFW does not answer the DAL mailbox (%d): nothing done", r);
        return r;
    }
    /* notify_wm_ranges, set_hard_max_memclk */
    r = dcn32_notify_wm_ranges(d, &pw->bw, wm_table_off);
    if (!r)
        r = dcn32_set_hard_max_memclk(d, &pw->bw);
    /* force_pstate_change_control(false, false) */
    if (!keep_force)
        dcn32_hubbub_force_pstate(d, 0, 0);
    /* a USR retraining allow the mirror forced stays (keep_force) */
    v = RR(d, regDCHUBBUB_ARB_USR_RETRAINING_CNTL);
    pw->hubbub.usr_force_keep =
        keep_force && GET(v, DCHUBBUB_ARB_USR_RETRAINING_CNTL, DCHUBBUB_ARB_ALLOW_USR_RETRAINING_FORCE_ENABLE) &&
        GET(v, DCHUBBUB_ARB_USR_RETRAINING_CNTL, DCHUBBUB_ARB_ALLOW_USR_RETRAINING_FORCE_VALUE);
    /* init_crb: the CRB as the hardware runs it, the reserved space
     * (pixel_chunk_size 8 KiB: / 32 and / 128), DET_DEPTH */
    dcn32_hubbub_read_crb(d, &pw->hubbub);
    WR(d, regCOMPBUF_RESERVED_SPACE,
       ((8192u / 32) << COMPBUF_RESERVED_SPACE__COMPBUF_RESERVED_SPACE_64B__SHIFT) |
           ((8192u / 128) << COMPBUF_RESERVED_SPACE__COMPBUF_RESERVED_SPACE_ZS__SHIFT));
    v = RR(d, regDCHUBBUB_DEBUG_CTRL_0);
    WR(d, regDCHUBBUB_DEBUG_CTRL_0, SET(v, DCHUBBUB_DEBUG_CTRL_0, DET_DEPTH, 0x47F));
    /* set_request_limit: GC 11.0.3 sets sdpif_request_limit_words_per_umc 16
     * (hubbub32_set_request_limit: 3 x channels x words / 4) */
    {
        uint32_t limit = 3 * pw->soc.num_chans * 16 / 4;
        if (limit > 0xfff)
            limit = 0xfff;
        v = RR(d, regSDPIF_REQUEST_RATE_LIMIT);
        WR(d, regSDPIF_REQUEST_RATE_LIMIT, SET(v, SDPIF_REQUEST_RATE_LIMIT, SDPIF_REQUEST_RATE_LIMIT, limit));
    }
    /* the watermark state starts from the registers (set A copied: the
     * GOP's, hubbub32_init_watermarks is for a non-accelerated boot) */
    {
        rdna3_hubbub_wm_regs regs[RD_WM_SET_COUNT];
        hubbub32_wm_read_state(d, regs);
        for (i = 0; i < RD_WM_SET_COUNT; i++) {
            pw->hubbub.set[i].urgent_ns = regs[i].data_urgent * 1000 / pw->refclk_mhz;
            pw->hubbub.set[i].cstate_enter_plus_exit_ns = regs[i].sr_enter * 1000 / pw->refclk_mhz;
            pw->hubbub.set[i].cstate_exit_ns = regs[i].sr_exit * 1000 / pw->refclk_mhz;
            pw->hubbub.set[i].pstate_change_ns = regs[i].dram_clk_change * 1000 / pw->refclk_mhz;
            pw->hubbub.set[i].usr_retraining_ns = regs[i].usr_retrain * 1000 / pw->refclk_mhz;
            pw->hubbub.set[i].fclk_pstate_change_ns = regs[i].fclk_pstate_change * 1000 / pw->refclk_mhz;
        }
    }
    {
        int m = dcn32_initialize_min_clocks(d, &pw->bw, &pw->clks);
        if (!r)
            r = m;
    }
    RLOG(d, "pstate: init: WM table %d, CRB DET %u/%u/%u/%u compbuf %u segments, min clocks DCFCLK %u kHz UCLK %u kHz",
         r, pw->hubbub.det_segs[0], pw->hubbub.det_segs[1], pw->hubbub.det_segs[2], pw->hubbub.det_segs[3],
         pw->hubbub.compbuf_segs, pw->clks.dcfclk_khz, pw->clks.dramclk_khz);
    return r;
}

uint32_t dcn32_pstate_planes_from_heads(rdna3_dev *d, const rdna3_dcn *dc, const uint32_t *pclk_khz,
                                        const uint32_t *cursor_bytes, rdna3_pstate_ctx *ctx)
{
    uint32_t i, n = 0;
    for (i = 0; i < sizeof(*ctx); i++)
        ((uint8_t *)ctx)[i] = 0;
    {
        rdna3_dcn_clocks ck;
        if (dcn32_clk_read(d, &ck)) return 0; /* no usable planes on a clock-read failure */
        ctx->dispclk_khz = ck.dispclk_khz;
        ctx->dppclk_khz = ck.dppclk_khz;
    }
    for (i = 0; dc && i < dc->nheads && n < RD_DCN_MAX_PIPES; i++) {
        const rdna3_dcn_head *h = &dc->head[i];
        rdna3_pstate_plane *p = &ctx->plane[n];
        if (!pclk_khz || !pclk_khz[i] || !h->h_total || !h->v_total)
            continue;
        p->pipe = h->pipe;
        p->otg = h->otg;
        p->dpp_count = h->odm_cnt > 1 ? 2 : 1;
        p->odm_pipe = h->odm_cnt > 1 ? h->odm_pipe : RD_DCN_MAX_PIPES;
        p->pclk_khz = pclk_khz[i];
        p->h_total = h->h_total;
        p->v_total = h->v_total;
        p->h_active = h->h_active;
        p->v_active = h->v_active;
        p->v_front = h->v_front;
        p->v_sync = h->v_sync;
        p->src_w = h->width ? h->width : h->h_active;
        p->src_h = h->height ? h->height : h->v_active;
        p->bpp = (h->format >= 24 && h->format < 32) ? 8 : 4;   /* 64 bpp formats (ARGB16161616) */
        p->det_kb = 0;
        {
            uint32_t a = A(d, regDCHUBBUB_DET0_CTRL);
            p->det_kb = GET(rdna3_rreg(d, a + DET_S * h->pipe), DCHUBBUB_DET0_CTRL, DET0_SIZE_CURRENT) * DCN3_2_DET_SEG_KB;
            if (p->dpp_count > 1)
                p->det_kb += GET(rdna3_rreg(d, a + DET_S * h->odm_pipe), DCHUBBUB_DET0_CTRL, DET0_SIZE_CURRENT) *
                             DCN3_2_DET_SEG_KB;
        }
        p->vstartup = GET(RO(d, regOTG0_OTG_VSTARTUP_PARAM, h->otg), OTG0_OTG_VSTARTUP_PARAM, VSTARTUP_START);
        p->cursor_bytes = cursor_bytes ? cursor_bytes[i] : 0;
        p->blanked = GET(RH(d, regHUBP0_DCHUBP_CNTL, h->pipe), HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN);
        p->mall = RD_MALL_PSTATE_DISABLE;
        p->refresh_hz = dcn32_refresh_rate_ceil(p);
        n++;
    }
    ctx->nplanes = n;
    return n;
}

int dcn32_program_outstanding_updates(rdna3_dev *d, rdna3_dcn32_power *pw)
{
    return dcn32_program_compbuf_size(d, &pw->hubbub, pw->cur.compbuf_kb, 1);
}

static uint32_t ctx_otg_mask(const rdna3_pstate_ctx *ctx)
{
    uint32_t k, m = 0;
    for (k = 0; k < ctx->nplanes; k++)
        m |= 1u << ctx->plane[k].otg;
    return m;
}

int dcn32_power_prepare(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn32_power *pw, rdna3_pstate_ctx *ctx)
{
    rdna3_dcn_wm prep[RD_WM_SET_COUNT];
    uint32_t k, p;
    int r = RDNA3_OK, e, subvp = 0;

    for (k = 0; k < ctx->nplanes; k++)
        subvp |= ctx->plane[k].mall != RD_MALL_PSTATE_DISABLE;
    /* the arbiter's allow forced (keep_force): a switch can come at any
     * moment, so only a natural vactive switch is safe; UCLK stays pinned
     * (at the top state's watermarks) for anything else */
    if (pw->keep_force && ctx->p_state_change_support && ctx->dram_change != RD_DRAM_CHANGE_VACTIVE) {
        RLOG(d, "pstate: p-state allow forced and the switch needs %s: UCLK stays pinned",
             dcn32_dram_change_name(ctx->dram_change));
        ctx->p_state_change_support = 0;
    }
    /* dcn32_prepare_bandwidth (no FAMS) -> dcn20_prepare_bandwidth:
     * update_clocks(raise), watermarks (raise; set A's p-state at 4 s
     * while SubVP pipes run: the real value comes with the optimize, after
     * the DMCUB has it), the compbuf shrink */
    e = dcn32_clk_update_pstate(d, &pw->bw, &pw->clks, ctx, 0, ctx_otg_mask(ctx));
    if (e)
        r = e;
    for (k = 0; k < RD_WM_SET_COUNT; k++)
        prep[k] = ctx->wm[k];
    if (subvp)
        prep[RD_WM_A].pstate_change_ns = 4000000000u;
    hubbub32_program_watermarks(d, &pw->hubbub, prep, pw->refclk_mhz, 0);
    /* the CRB split only when the DET allocation was planned
     * (dcn32_set_det_allocations); else the DETs stay as they run */
    if (ctx->compbuf_kb)
        dcn32_program_compbuf_size(d, &pw->hubbub, ctx->compbuf_kb, 0);
    /* dc_dmub_srv_p_state_delegate(dc, false) */
    if (dm && dm->hw_init && !ctx->fw_based_mclk_switching)
        dcn32_dmub_p_state_delegate_off(dm);
    /* the DETs: shrink first, then grow (a phantom pipe's DET is set to 0
     * when it is brought up) */
    for (p = 0; p < RD_DCN_MAX_PIPES; p++) {
        int phantom = 0;
        for (k = 0; k < ctx->nplanes; k++)
            phantom |= ctx->plane[k].pipe == p && ctx->plane[k].mall == RD_MALL_PSTATE_PHANTOM;
        if (ctx->det_segs[p] < pw->hubbub.det_segs[p] && (ctx->det_segs[p] || phantom))
            dcn32_program_det_size(d, &pw->hubbub, p, ctx->det_segs[p] * DCN3_2_DET_SEG_KB);
    }
    for (p = 0; p < RD_DCN_MAX_PIPES; p++)
        if (ctx->det_segs[p] > pw->hubbub.det_segs[p])
            dcn32_program_det_size(d, &pw->hubbub, p, ctx->det_segs[p] * DCN3_2_DET_SEG_KB);
    /* dcn32_program_mall_pipe_config: MALL_SEL (FORCE_ONE_ROW_FOR_FRAME of
     * SubVP mains: hw/dcn32_subvp.c), then dcn32_update_force_pstate */
    dcn32_update_mall_sel(d, &pw->soc, ctx);
    dcn32_update_force_pstate(d, ctx, pw->have_cur ? &pw->cur : NULL);
    return r;
}

int dcn32_power_optimize(rdna3_dev *d, rdna3_dcn32_power *pw, rdna3_pstate_ctx *ctx)
{
    uint32_t k;
    int r = RDNA3_OK, e;
    /* dcn20_optimize_bandwidth: watermarks lowered (the real set A),
     * compbuf grown, update_clocks(lower) */
    hubbub32_program_watermarks(d, &pw->hubbub, ctx->wm, pw->refclk_mhz, 1);
    if (ctx->compbuf_kb) {
        e = dcn32_program_compbuf_size(d, &pw->hubbub, ctx->compbuf_kb, 1);
        if (e)
            r = e;
    }
    e = dcn32_clk_update_pstate(d, &pw->bw, &pw->clks, ctx, 1, ctx_otg_mask(ctx));
    if (e && !r)
        r = e;
    pw->cur = *ctx;
    pw->have_cur = 1;
    RLOG(d, "pstate: state %u: UCLK %s (%s, prefetch mode %u) min %u kHz, FCLK %s, DCFCLK %u kHz deep sleep %u kHz, "
         "WM A urgent %u p-state %u fclk %u sr %u/%u ns, C p-state %u ns (dummy %u), MALL %llu B (%u ways), compbuf %u KiB: %d",
         ctx->vlevel, ctx->p_state_change_support ? "switches" : "PINNED at max", dcn32_dram_change_name(ctx->dram_change),
         ctx->dml.prefetch_mode, ctx->dramclk_khz, ctx->fclk_p_state_change_support ? "switches" : "pinned",
         ctx->dcfclk_khz, ctx->dcfclk_deep_sleep_khz, ctx->wm[RD_WM_A].urgent_ns, ctx->wm[RD_WM_A].pstate_change_ns,
         ctx->wm[RD_WM_A].fclk_pstate_change_ns, ctx->wm[RD_WM_A].cstate_exit_ns,
         ctx->wm[RD_WM_A].cstate_enter_plus_exit_ns, ctx->wm[RD_WM_C].pstate_change_ns, ctx->dummy_latency_index,
         (unsigned long long)ctx->mall_ss_size_bytes, dcn32_calculate_cab_allocation(&pw->soc, ctx), ctx->compbuf_kb, r);
    for (k = 0; k < ctx->nplanes; k++)
        RLOG(d, "pstate: pipe %u OTG %u %ux%u %u kHz DET %u KiB: active margin %d us, FCLK margin %d us, SubVP lines %u%s",
             ctx->plane[k].pipe, ctx->plane[k].otg, ctx->plane[k].h_active, ctx->plane[k].v_active,
             ctx->plane[k].pclk_khz, ctx->plane[k].det_kb, ctx->dml.active_margin_ns[k] / 1000,
             ctx->dml.fclk_margin_ns[k] / 1000, ctx->dml.subvp_lines[k],
             ctx->plane[k].mall == RD_MALL_PSTATE_SUB_VIEWPORT ? " (SubVP main)"
             : ctx->plane[k].mall == RD_MALL_PSTATE_PHANTOM    ? " (phantom)" : "");
    return r;
}

int dcn32_power_apply(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn32_power *pw, rdna3_pstate_ctx *ctx)
{
    int r = dcn32_power_prepare(d, dm, pw, ctx), e = dcn32_power_optimize(d, pw, ctx);
    return r ? r : e;
}

/* ======================================================================
 * DRIVER-PLAN N74: the UCLK hard minimum of the default path (rdna3mall 0)
 * ====================================================================== */

uint32_t dcn32_uclk_pin_lit(uint32_t nheads, uint32_t down_mask, uint32_t up_mask, uint32_t gone_mask,
                            int mirror_pipe)
{
    uint32_t i, n = 0;
    for (i = 0; i < nheads && i < 32; i++) {
        uint32_t b = 1u << i;
        if ((gone_mask & b) || ((down_mask & b) && !(up_mask & b)))
            continue;
        n++;
    }
    return n + (mirror_pipe ? 1u : 0u);
}

int dcn32_uclk_pin_decide(uint32_t lit, int synchronized, int off, uint32_t *why)
{
    uint32_t w;
    int support;
    if (off) {
        w = RD_UCLK_WHY_OFF;
        support = 1;
    } else if (lit == 0) {
        w = RD_UCLK_WHY_NONE;   /* no plane: DML has nothing to hide the switch from */
        support = 1;
    } else if (lit == 1) {
        w = RD_UCLK_WHY_ONE;    /* DRAMClockChangeSupportNumber 1: vblank */
        support = 1;
    } else if (synchronized) {
        w = RD_UCLK_WHY_SYNC;   /* SynchronizeTimingsFinal: the planes' vblanks coincide */
        support = 1;
    } else {
        /* DRAMClockChangeSupportNumber 2 (a second unsynchronised plane
         * without active margin) -> dm_dram_clock_change_unsupported; no
         * SubVP, no FPO candidate -> p_state_change_support false */
        w = RD_UCLK_WHY_UNSYNC;
        support = 0;
    }
    if (why)
        *why = w;
    return support;
}

const char *dcn32_uclk_pin_why_name(uint32_t why)
{
    static const char *const n[RD_UCLK_WHY_COUNT] = {
        "no head lit: nothing fetches",
        "one head: its vblank hides the switch",
        "the heads' timings synchronised: one shared vblank",
        "unsynchronised, no FPO / SubVP: no shared vblank",
        "switched off (rdna3uclkpin=0 / debug.rdna3_uclk_pin 0)",
    };
    return why < RD_UCLK_WHY_COUNT ? n[why] : "?";
}

int dcn32_uclk_pin_init(rdna3_dev *d, rdna3_uclk_pin *u, int resync)
{
    uint32_t i, ver = 0;
    for (i = 0; i < sizeof(*u); i++)
        ((uint8_t *)u)[i] = 0;
    /* dcn32_initialize_min_clocks: p-state switching supported at the start */
    u->p_state_change_support = 1;
    u->resync = resync ? 1 : 0;
    /* dcn32_init_clocks: smu_present when GetSmuVersion answers */
    u->last_err = dcn32_smu_send(d, RD_DALSMC_MSG_GetSmuVersion, 0, &ver);
    if (u->last_err) {
        RERR(d, "uclk pin: GetSmuVersion over the DAL mailbox -> %d (-5 refused, -62 no answer): UCLK is left to the PMFW",
             u->last_err);
        return RDNA3_ENODEV;
    }
    u->pmfw = ver;
    /* dcn32_get_memclk_states_from_smu: the UCLK levels (dcn32_init_single_clock) */
    init_single_clock(d, RD_PPCLK_UCLK, u->memclk_mhz, &u->n_memclk);
    if (!u->n_memclk || !u->memclk_mhz[0] || !u->memclk_mhz[u->n_memclk - 1]) {
        u->last_err = RDNA3_ENODEV;
        RERR(d, "uclk pin: PMFW 0x%08x lists no UCLK DPM level (%u levels, first %u MHz): UCLK is left to it", ver,
             u->n_memclk, u->memclk_mhz[0]);
        return RDNA3_ENODEV;
    }
    u->min_mhz = u->memclk_mhz[0];
    u->max_mhz = u->memclk_mhz[u->n_memclk - 1];
    u->ready = 1;
    RLOG(d, "uclk pin: PMFW 0x%08x, UCLK %u levels %u..%u MHz%s", ver, u->n_memclk, u->min_mhz, u->max_mhz,
         u->resync ? ", the minimum sent before may still be in place: the first decision is sent" : "");
    return RDNA3_OK;
}

int dcn32_uclk_pin_update(rdna3_dev *d, rdna3_uclk_pin *u, uint32_t lit, int synchronized, int off, int *sent)
{
    uint32_t why = 0, mhz;
    int support = dcn32_uclk_pin_decide(lit, synchronized, off, &why), r, taken = 0;
    if (sent)
        *sent = 0;
    u->decisions++;
    u->lit = lit;
    u->why = why;
    if (!u->ready)
        return RDNA3_ENODEV;
    /* should_update_pstate_support: only a change (or a minimum not known) */
    if (support == u->p_state_change_support && !u->resync)
        return RDNA3_OK;
    /* dcn32_update_clocks: "to disable P-State switching, set UCLK min = max";
     * switching again: the hard minimum at the state's DRAM clock (the lowest
     * level: dcn32_initialize_min_clocks') */
    mhz = support ? u->min_mhz : u->max_mhz;
    /* pmfw 0: dcn32_smu_set_hard_min skips the status wait (the PMFW
     * versions without it) - once the status proved unreliable */
    r = dcn32_smu_set_hard_min_ex(d, RD_PPCLK_UCLK, mhz, u->status_unreliable ? 0 : u->pmfw, NULL, &taken);
    if (sent)
        *sent = 1;
    u->last_err = r;
    u->last_unconfirmed = u->status_unreliable && r == RDNA3_OK;
    if (r == RDNA3_ETIME && taken) {
        /* accepted, ReturnHardMinStatus did not confirm it within 2 s:
         * dcn32_smu_set_hard_min_by_freq prints and goes on; resending
         * would only wait the 2 s again, and so would every later
         * transition: no status wait from now on */
        u->unconfirmed++;
        u->last_unconfirmed = 1;
        u->status_unreliable = 1;
        RERR(d, "uclk pin: SetHardMinByFreq(UCLK, %u MHz) accepted but not confirmed by ReturnHardMinStatus in 2 s: "
             "taken as set; the status wait is skipped from now on (each would hold the display lock 2 s)", mhz);
        r = RDNA3_OK;
    }
    if (r) {
        u->fails++;
        u->consecutive_fails++;
        u->resync = 1;   /* not known what the PMFW holds now: the next decision is sent again */
        if (u->consecutive_fails >= RD_UCLK_PIN_MAX_FAILS) {
            u->ready = 0;
            u->gave_up = 1;
            RERR(d, "uclk pin: SetHardMinByFreq(UCLK, %u MHz) for %u head(s) (%s) -> %d, %u in a row: GIVING UP, no "
                 "DAL message until the next bring-up (UCLK left as the PMFW has it)", mhz, lit,
                 dcn32_uclk_pin_why_name(why), r, u->consecutive_fails);
        } else {
            RERR(d, "uclk pin: SetHardMinByFreq(UCLK, %u MHz) for %u head(s) (%s) -> %d (%u failed, %u in a row): "
                 "tried again at the next decision", mhz, lit, dcn32_uclk_pin_why_name(why), r, u->fails,
                 u->consecutive_fails);
        }
        return r;
    }
    u->consecutive_fails = 0;
    u->p_state_change_support = support;
    u->resync = 0;
    u->hard_min_mhz = mhz;
    if (support)
        u->releases++;
    else
        u->pins++;
    return RDNA3_OK;
}

void dcn32_uclk_pin_power_lost(rdna3_uclk_pin *u)
{
    /* as dcn32_initialize_min_clocks starts; what the PMFW holds is not
     * known (its own after a power loss, ours after a suspend that kept the
     * power): the next decision is sent */
    u->p_state_change_support = 1;
    u->hard_min_mhz = 0;
    u->resync = 1;
}
