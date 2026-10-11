/*
 * dcn32_modeset.c - see dcn32_modeset.h.
 *
 * Ported from (Linux v6.12, MIT): optc1_program_timing,
 * optc1_program_global_sync, optc1_set_vtg_params, optc32_disable_crtc,
 * optc32_enable_crtc, hubp2_program_deadline, dml32_rq_dlg_get_dlg_reg,
 * dml32_CalculateVUpdateAndDynamicMetadataParameters and the VStartup /
 * MIN_DST_Y_NEXT_START part of dml32 ModeSupportAndSystemConfiguration;
 * with clock limits, dml32_CalculateRequiredDispclk / dml32_CalculateDPPCLK
 * and the raise half of dcn32_update_clocks (hw/dcn32_clk.c). DP SST
 * heads: dce110_apply_single_controller_ctx_to_hw's order (pixel clock,
 * timing, OTG on, then link_set_dpms_on), the link through hw/dcn32_dp.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_modeset.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_dig.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_clone.h"
#include "gen/dcn_3_2_0_scale.h"
#include "gen/dcn_3_2_0_mode.h"
#include "gen/dcn_3_2_0_dp.h"
#include "gen/dcn_3_2_0_dsc.h"
#include "dcn32_dp_mst.h"
#include "dcn32_hwseq.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))
#define FIELD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)

#define RI(d, reg, n, stride) rd((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n))
#define WI(d, reg, n, stride, v) wr((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n), (v))
#define AI(d, reg, n, stride) \
    rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n))

#define HUBP_S RD_CLONE_HUBP_STRIDE
#define DPP_S  RD_CLONE_DPP_STRIDE
#define OPP_S  RD_CLONE_OPP_STRIDE
#define OTG_S  RD_CLONE_OTG_STRIDE
#define ODM_S  RD_CLONE_ODM_STRIDE
#define DIG_S  RD_CLONE_DIGFE_STRIDE
#define VTG_S  (regVTG1_CONTROL - regVTG0_CONTROL)
#define MUX_S  (regMPC_OUT1_MUX - regMPC_OUT0_MUX)

/* dml32: the DCHUB reference clock of DCN 3.2 when nothing else says */
#define RD_DCN32_REFCLK_KHZ 100000u

static uint32_t rd(rdna3_dev *d, uint32_t seg, uint32_t off)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off));
}

static void wr(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off), v);
}

static uint64_t udiv_round(uint64_t a, uint64_t b) { return b ? (a + b / 2) / b : 0; }
static uint64_t udiv_ceil(uint64_t a, uint64_t b) { return b ? (a + b - 1) / b : 0; }
/* v * num / den, 64-bit, rounded */
static uint32_t scale(uint32_t v, uint64_t num, uint64_t den) { return (uint32_t)udiv_round((uint64_t)v * num, den); }

static void read_regs(rdna3_dev *d, uint32_t pipe, uint32_t otg, uint32_t opp, rdna3_modeset_regs *r)
{
    r->h_total = RI(d, regOTG0_OTG_H_TOTAL, otg, OTG_S);
    r->h_blank = RI(d, regOTG0_OTG_H_BLANK_START_END, otg, OTG_S);
    r->h_sync = RI(d, regOTG0_OTG_H_SYNC_A, otg, OTG_S);
    r->h_sync_cntl = RI(d, regOTG0_OTG_H_SYNC_A_CNTL, otg, OTG_S);
    r->v_total = RI(d, regOTG0_OTG_V_TOTAL, otg, OTG_S);
    r->v_blank = RI(d, regOTG0_OTG_V_BLANK_START_END, otg, OTG_S);
    r->v_sync = RI(d, regOTG0_OTG_V_SYNC_A, otg, OTG_S);
    r->v_sync_cntl = RI(d, regOTG0_OTG_V_SYNC_A_CNTL, otg, OTG_S);
    r->vstartup = RI(d, regOTG0_OTG_VSTARTUP_PARAM, otg, OTG_S);
    r->vupdate = RI(d, regOTG0_OTG_VUPDATE_PARAM, otg, OTG_S);
    r->vready = RI(d, regOTG0_OTG_VREADY_PARAM, otg, OTG_S);
    r->vtg = RI(d, regVTG0_CONTROL, otg, VTG_S);
    r->blank0 = RI(d, regHUBPREQ0_BLANK_OFFSET_0, pipe, HUBP_S);
    r->blank1 = RI(d, regHUBPREQ0_BLANK_OFFSET_1, pipe, HUBP_S);
    r->dst_dim = RI(d, regHUBPREQ0_DST_DIMENSIONS, pipe, HUBP_S);
    r->after_scaler = RI(d, regHUBPREQ0_DST_AFTER_SCALER, pipe, HUBP_S);
    r->prefetch = RI(d, regHUBPREQ0_PREFETCH_SETTINGS, pipe, HUBP_S);
    r->line_pre = RI(d, regHUBPREQ0_PER_LINE_DELIVERY_PRE, pipe, HUBP_S);
    r->line = RI(d, regHUBPREQ0_PER_LINE_DELIVERY, pipe, HUBP_S);
    r->ref_to_pix = RI(d, regHUBPREQ0_REF_FREQ_TO_PIX_FREQ, pipe, HUBP_S);
    r->qos_wm = RI(d, regHUBPREQ0_DCN_TTU_QOS_WM, pipe, HUBP_S);
    r->global_ttu = RI(d, regHUBPREQ0_DCN_GLOBAL_TTU_CNTL, pipe, HUBP_S);
    r->surf0_cntl0 = RI(d, regHUBPREQ0_DCN_SURF0_TTU_CNTL0, pipe, HUBP_S);
    r->surf0_cntl1 = RI(d, regHUBPREQ0_DCN_SURF0_TTU_CNTL1, pipe, HUBP_S);
    r->viewport_dim = RI(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, pipe, HUBP_S);
    r->recout_size = RI(d, regDSCL0_RECOUT_SIZE, pipe, DPP_S);
    r->mpc_size = RI(d, regDSCL0_MPC_SIZE, pipe, DPP_S);
    r->oppbuf = RI(d, regOPPBUF0_OPPBUF_CONTROL, opp, OPP_S);
}

static void write_regs(rdna3_dev *d, uint32_t pipe, uint32_t otg, uint32_t opp, const rdna3_modeset_regs *r)
{
    /* optc1_program_timing */
    WI(d, regOTG0_OTG_H_TOTAL, otg, OTG_S, r->h_total);
    WI(d, regOTG0_OTG_H_SYNC_A, otg, OTG_S, r->h_sync);
    WI(d, regOTG0_OTG_H_BLANK_START_END, otg, OTG_S, r->h_blank);
    WI(d, regOTG0_OTG_H_SYNC_A_CNTL, otg, OTG_S, r->h_sync_cntl);
    WI(d, regOTG0_OTG_V_TOTAL, otg, OTG_S, r->v_total);
    /* optc1_set_vtotal_min_max(v_total, v_total) */
    WI(d, regOTG0_OTG_V_TOTAL_MIN, otg, OTG_S, r->v_total);
    WI(d, regOTG0_OTG_V_TOTAL_MAX, otg, OTG_S, r->v_total);
    WI(d, regOTG0_OTG_V_SYNC_A, otg, OTG_S, r->v_sync);
    WI(d, regOTG0_OTG_V_BLANK_START_END, otg, OTG_S, r->v_blank);
    WI(d, regOTG0_OTG_V_SYNC_A_CNTL, otg, OTG_S, r->v_sync_cntl);
    {
        uint32_t v = RI(d, regOTG0_OTG_INTERLACE_CONTROL, otg, OTG_S);
        WI(d, regOTG0_OTG_INTERLACE_CONTROL, otg, OTG_S, SET(v, OTG0_OTG_INTERLACE_CONTROL, OTG_INTERLACE_ENABLE, 0));
    }
    /* optc1_program_global_sync + optc1_set_vtg_params (VTG stays off: the
     * enable comes with the OTG) */
    WI(d, regOTG0_OTG_VSTARTUP_PARAM, otg, OTG_S, r->vstartup);
    WI(d, regOTG0_OTG_VUPDATE_PARAM, otg, OTG_S, r->vupdate);
    WI(d, regOTG0_OTG_VREADY_PARAM, otg, OTG_S, r->vready);
    WI(d, regVTG0_CONTROL, otg, VTG_S, SET(r->vtg, VTG0_CONTROL, VTG0_ENABLE, 0));
    /* hubp2_program_deadline */
    WI(d, regHUBPREQ0_BLANK_OFFSET_0, pipe, HUBP_S, r->blank0);
    WI(d, regHUBPREQ0_BLANK_OFFSET_1, pipe, HUBP_S, r->blank1);
    WI(d, regHUBPREQ0_DST_DIMENSIONS, pipe, HUBP_S, r->dst_dim);
    WI(d, regHUBPREQ0_DST_AFTER_SCALER, pipe, HUBP_S, r->after_scaler);
    WI(d, regHUBPREQ0_PREFETCH_SETTINGS, pipe, HUBP_S, r->prefetch);
    WI(d, regHUBPREQ0_PER_LINE_DELIVERY_PRE, pipe, HUBP_S, r->line_pre);
    WI(d, regHUBPREQ0_PER_LINE_DELIVERY, pipe, HUBP_S, r->line);
    WI(d, regHUBPREQ0_REF_FREQ_TO_PIX_FREQ, pipe, HUBP_S, r->ref_to_pix);
    WI(d, regHUBPREQ0_DCN_TTU_QOS_WM, pipe, HUBP_S, r->qos_wm);
    WI(d, regHUBPREQ0_DCN_GLOBAL_TTU_CNTL, pipe, HUBP_S, r->global_ttu);
    WI(d, regHUBPREQ0_DCN_SURF0_TTU_CNTL0, pipe, HUBP_S, r->surf0_cntl0);
    WI(d, regHUBPREQ0_DCN_SURF0_TTU_CNTL1, pipe, HUBP_S, r->surf0_cntl1);
    /* the plane covers the active area: hubp2_program_size, recout, MPC */
    WI(d, regHUBP0_DCSURF_PRI_VIEWPORT_START, pipe, HUBP_S, 0);
    WI(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, pipe, HUBP_S, r->viewport_dim);
    WI(d, regDSCL0_RECOUT_START, pipe, DPP_S, 0);
    WI(d, regDSCL0_RECOUT_SIZE, pipe, DPP_S, r->recout_size);
    WI(d, regDSCL0_MPC_SIZE, pipe, DPP_S, r->mpc_size);
    WI(d, regOPPBUF0_OPPBUF_CONTROL, opp, OPP_S, r->oppbuf);
}

/* the TMDS head's DIG chain, as dcn32_read_state found it */
int dcn32_modeset_plan(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                       uint32_t cur_pclk_khz, const rdna3_mode *m, rdna3_dcn_modeset *ms)
{
    return dcn32_modeset_plan_clk(d, dc, head, path, cur_pclk_khz, m, NULL, ms);
}

int dcn32_modeset_plan_clk(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                           uint32_t cur_pclk_khz, const rdna3_mode *m, const rdna3_dcn_clk_limits *lim,
                           rdna3_dcn_modeset *ms)
{
    return dcn32_modeset_plan_dp(d, dc, head, path, NULL, cur_pclk_khz, m, lim, ms);
}

/* the running timing of head 'h' at 'pclk_khz' as a mode (MSA, link settings) */
void dcn32_modeset_head_mode(const rdna3_dcn_head *h, uint32_t pclk_khz, rdna3_mode *m)
{
    uint32_t i;
    for (i = 0; i < sizeof(*m); i++)
        ((uint8_t *)m)[i] = 0;
    m->h_active = h->h_active;
    m->v_active = h->v_active;
    m->h_total = h->h_total;
    m->v_total = h->v_total;
    m->h_front = h->h_front;
    m->h_sync = h->h_sync;
    m->v_front = h->v_front;
    m->v_sync = h->v_sync;
    m->h_sync_pos = h->h_sync_neg ? 0 : 1;
    m->v_sync_pos = h->v_sync_neg ? 0 : 1;
    m->pixel_khz = pclk_khz;
}

/* the TMDS rate check of a DVI / HDMI head: without an HDMI configuration
 * the HDMI 1.4 limit; with one dcn10_link_encoder_validate_output_with_stream
 * for an RGB 8 bpc stream (phy_pix_clk = get_tmds_output_pixel_clock_100hz),
 * above 340 MHz only with RD_HDMI_20 */
static int tmds_mode_ok(const rdna3_dcn_head *h, const rdna3_hdmi_cfg *hc, const rdna3_mode *m)
{
    rdna3_link_enc_features ft;
    rdna3_crtc_format f = { RD_PIXEL_ENCODING_RGB, RD_COLOR_DEPTH_888, 0, 0 };
    uint32_t sig = h->dig_mode == 3 ? RD_SIGNAL_HDMI_TYPE_A : RD_SIGNAL_DVI_SINGLE_LINK, phy;

    if (!hc || !(hc->flags & RD_HDMI_STREAM))
        return m->pixel_khz <= 340000;
    phy = dcn32_tmds_output_pixel_clock_100hz(m->pixel_khz * 10, &f) / 10;
    if (phy > 340000 && !(hc->flags & RD_HDMI_20))
        return 0;
    dcn32_link_enc_features(hc->enc_caps, &ft);
    return dcn32_link_enc_validate_output(&ft, sig, sig, &f, m->pixel_khz, phy, hc->max_tmds_clk_mhz);
}

static int plan_common(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                       rdna3_dp_link *dp, const rdna3_hdmi_cfg *hc, uint32_t cur_pclk_khz, const rdna3_mode *m,
                       const rdna3_dcn_clk_limits *lim, rdna3_dcn_modeset *ms);

int dcn32_modeset_plan_dp(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                          rdna3_dp_link *dp, uint32_t cur_pclk_khz, const rdna3_mode *m,
                          const rdna3_dcn_clk_limits *lim, rdna3_dcn_modeset *ms)
{
    return plan_common(d, dc, head, path, dp, NULL, cur_pclk_khz, m, lim, ms);
}

int dcn32_modeset_plan_hdmi(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                            const rdna3_hdmi_cfg *hc, uint32_t cur_pclk_khz, const rdna3_mode *m,
                            const rdna3_dcn_clk_limits *lim, rdna3_dcn_modeset *ms)
{
    return plan_common(d, dc, head, path, NULL, hc, cur_pclk_khz, m, lim, ms);
}

static int plan_common(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                       rdna3_dp_link *dp, const rdna3_hdmi_cfg *hc, uint32_t cur_pclk_khz, const rdna3_mode *m,
                       const rdna3_dcn_clk_limits *lim, rdna3_dcn_modeset *ms)
{
    const rdna3_dcn_head *h;
    rdna3_otg_timing t;
    rdna3_modeset_regs *c, *w;
    uint64_t refclk_khz, r_old, r_new;   /* ref/pix ratios in 2^19 units */
    uint32_t i, vblank, vfp, vstartup, upd_off, upd_w, rdy, old_vstartup, old_upd_w, old_rdy, v_init, vb_end;
    uint32_t pf_q, pf_old, margin_q, vratio, w_old;
    uint64_t tsetup_lines4;

    if (!dc || !ms || !m || !path || head >= dc->nheads)
        return RD_MODESET_BAD_HEAD;
    h = &dc->head[head];
    for (i = 0; i < sizeof(*ms); i++)
        ((uint8_t *)ms)[i] = 0;
    if (h->dig_fe < 0 || h->dig_be < 0 ||
        (h->dig_mode != 2 && h->dig_mode != 3 && !((h->dig_mode == 0 || h->dig_mode == 5) && dp)))
        return RD_MODESET_NOT_TMDS;
    if (!m->pixel_khz || !m->h_total || !m->v_total)
        return RD_MODESET_NO_TIMING;
    if (m->interlaced)
        return RD_MODESET_INTERLACED;
    if (h->dig_mode != 0 && h->dig_mode != 5 && !tmds_mode_ok(h, hc, m))
        return RD_MODESET_TMDS_CLOCK;
    if (h->dig_mode == 0 || h->dig_mode == 5) {
        rdna3_dp_link_settings ls;
        uint32_t ppc, opp, req;
        if (!dp->caps_valid || dp->be != (uint32_t)h->dig_be || dp->fe != (uint32_t)h->dig_fe || dp->otg != h->otg)
            return RD_MODESET_DP_LINK;
        /* an MST head (DIG_MODE 5) only with its MST link; an SST head may
         * turn MST (the MST takeover of rdna3mst) */
        if (h->dig_mode == 5 && !dp->mst)
            return RD_MODESET_DP_LINK;
        /* link_validate_mode_timing: an active converter's limits */
        if (!dcn32_dp_validate_mode_timing(dp, m))
            return RD_MODESET_DONGLE;
        opp = GET(RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, h->otg, ODM_S), ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL);
        dp->opp = opp < RD_DCN_MAX_PIPES ? opp : h->otg;
        dp->dsc_inst = h->pipe;   /* dcn20_acquire_dsc: the pipe's own DSC */
        /* DSC running that this driver did not set (the GOP's): not taken over */
        if (GET(RI(d, regODM0_OPTC_DATA_FORMAT_CONTROL, h->otg, ODM_S), ODM0_OPTC_DATA_FORMAT_CONTROL, OPTC_DSC_MODE) &&
            !dp->stream_dsc)
            return RD_MODESET_DP_LINK;
        if (dp->mst) {
            /* dm_dp_mst_is_port_support_mode + the PBN / time slots */
            req = dcn32_dp_mode_kbps(m, 8, 0);
            if (dcn32_dp_mst_mode_fits(dp, req, m->pixel_khz, &ms->dp_mst_pbn, &ms->dp_mst_slots))
                return RD_MODESET_LINK_BW;
        } else {
            /* apply_dsc_policy_for_stream (rdna3dsc), then link_decide_link_settings
             * over the link cap (dp_validate_mode_timing) */
            ms->dp_want_dsc = dcn32_dp_decide_dsc(dp, m, 8, &ms->dp_want_dsc_cfg);
            req = dcn32_dp_mode_kbps(m, 8, ms->dp_want_dsc ? &ms->dp_want_dsc_cfg : 0);
            if (dcn32_dp_decide_link_settings(dp, req, &ls))
                return RD_MODESET_LINK_BW;
        }
        /* the stream encoder's pixels per cycle stay as the boot set them:
         * two need is_h_timing_divisible_by_2 */
        ppc = dp->pix_per_cycle ? dp->pix_per_cycle
                                : (GET(RI(d, regDIG0_DIG_FIFO_CTRL0, (uint32_t)h->dig_fe, DIG_S), DIG0_DIG_FIFO_CTRL0,
                                       DIG_FIFO_OUTPUT_PIXEL_MODE) ? 2 : 1);
        if (ppc == 2 && ((m->h_total | m->h_active | m->h_front | m->h_sync) & 1))
            return RD_MODESET_SIZE;
    }
    if (m->h_active < 320 || m->v_active < 200 || m->h_active > 4096 || m->v_active > 2160 ||
        m->v_total - m->v_active < 4)
        return RD_MODESET_SIZE;
    ms->head = head;
    ms->pipe = h->pipe;
    ms->otg = h->otg;
    ms->opp = GET(RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, h->otg, ODM_S), ODM0_OPTC_DATA_SOURCE_SELECT,
                  OPTC_SEG0_SRC_SEL);
    if (ms->opp >= RD_DCN_MAX_PIPES)
        ms->opp = h->otg;
    ms->fe = (uint32_t)h->dig_fe;
    ms->be = (uint32_t)h->dig_be;
    ms->dig_mode = h->dig_mode;
    ms->hpd = path->hpd;
    ms->encoder_id = (uint8_t)path->encoder_objid;
    ms->connector_id = (uint8_t)path->connector_objid;
    dcn32_clk_read(d, &ms->clk);
    if (h->dig_mode == 0 || h->dig_mode == 5) {
        ms->dp = dp;
        if (ms->clk.dprefclk_khz)
            dp->dprefclk_khz = ms->clk.dprefclk_khz;
    } else if (hc && (hc->flags & RD_HDMI_STREAM)) {
        ms->hdmi = *hc;
        ms->tmds_want = *m;
        dcn32_modeset_head_mode(h, cur_pclk_khz, &ms->tmds_cur);
    }

    if (lim && lim->dispclk_max_khz && lim->dppclk_max_khz) {
        /* dcn32_update_clocks: what DML 3.2 asks, raised before the timing */
        uint32_t disp = dcn32_required_dispclk(m->pixel_khz, ms->clk.vco_khz, lim->dispclk_max_khz);
        uint32_t dpp = dcn32_required_dppclk(m->pixel_khz, ms->clk.vco_khz);
        if (!disp || dpp > lim->dppclk_max_khz)
            return RD_MODESET_CLOCKS;
        ms->pmfw = lim->pmfw;
        if (ms->clk.dispclk_khz < disp)
            ms->raise_dispclk_khz = disp;
        if (dcn32_pipe_dppclk(d, &ms->clk, h->pipe) < dpp)
            ms->raise_dppclk_khz = dpp;
    } else if (dcn32_clk_fits(&ms->clk, m->pixel_khz)) {
        return RD_MODESET_CLOCKS;
    }

    c = &ms->cur;
    w = &ms->want;
    read_regs(d, h->pipe, h->otg, ms->opp, c);
    c->h_active = h->h_active;
    c->v_active = h->v_active;
    /* the reference clock from the running ratio when the pixel clock is
     * known, else DCN 3.2's 100 MHz (dccg32_get_dccg_ref_freq: the
     * xtalin) and the clock from the ratio */
    r_old = GET(c->ref_to_pix, HUBPREQ0_REF_FREQ_TO_PIX_FREQ, REF_FREQ_TO_PIX_FREQ);
    if (cur_pclk_khz && r_old)
        refclk_khz = udiv_round((uint64_t)r_old * cur_pclk_khz, 1u << 19);
    else
        refclk_khz = RD_DCN32_REFCLK_KHZ;
    /* the register truncates the ratio: within 0.1 % it is the 100 MHz */
    if (refclk_khz > RD_DCN32_REFCLK_KHZ - 100 && refclk_khz < RD_DCN32_REFCLK_KHZ + 100)
        refclk_khz = RD_DCN32_REFCLK_KHZ;
    if (!cur_pclk_khz && r_old)
        cur_pclk_khz = (uint32_t)udiv_round(refclk_khz << 19, r_old);
    if (!r_old && cur_pclk_khz)
        r_old = udiv_round(refclk_khz << 19, cur_pclk_khz);
    if (!cur_pclk_khz || !r_old)
        return RD_MODESET_BAD_HEAD;
    c->pclk_khz = cur_pclk_khz;
    r_new = (refclk_khz << 19) / m->pixel_khz;
    if (r_new >= 4ull << 19)          /* ASSERT(ref_freq_to_pix_freq < 4.0) */
        return RD_MODESET_SIZE;

    *w = *c;
    w->pclk_khz = m->pixel_khz;
    w->h_active = m->h_active;
    w->v_active = m->v_active;

    /* optc1_program_timing (dcn32_otg_timing has the front porch workaround) */
    if (dcn32_otg_timing(m, ms->dp != NULL, &t))
        return RD_MODESET_NO_TIMING;
    if (ms->dp) {
        dcn32_modeset_head_mode(h, cur_pclk_khz, &ms->dp_cur);
        ms->dp_want = *m;
        ms->dp_cur_dsc = ms->dp->stream_dsc;
        ms->dp_cur_dsc_cfg = ms->dp->dsc_cfg;
        if (ms->dp_want_dsc)
            RLOG(d, "modeset: head %u: %ux%u needs DSC: %u x %u slices, %u.%04u bpp", head, m->h_active,
                 m->v_active, ms->dp_want_dsc_cfg.num_slices_h, ms->dp_want_dsc_cfg.num_slices_v,
                 ms->dp_want_dsc_cfg.bits_per_pixel / 16, (ms->dp_want_dsc_cfg.bits_per_pixel % 16) * 10000 / 16);
        if (ms->dp->mst)
            RLOG(d, "modeset: head %u: MST stream to port %d: PBN %u, %u time slots", head, ms->dp->mst_port,
                 ms->dp_mst_pbn, ms->dp_mst_slots);
    }
    w->h_total = t.h_total;
    w->h_sync = FIELD(OTG0_OTG_H_SYNC_A, OTG_H_SYNC_A_START, t.h_sync_start) |
                FIELD(OTG0_OTG_H_SYNC_A, OTG_H_SYNC_A_END, t.h_sync_end);
    w->h_blank = FIELD(OTG0_OTG_H_BLANK_START_END, OTG_H_BLANK_START, t.h_blank_start) |
                 FIELD(OTG0_OTG_H_BLANK_START_END, OTG_H_BLANK_END, t.h_blank_end);
    w->h_sync_cntl = SET(c->h_sync_cntl, OTG0_OTG_H_SYNC_A_CNTL, OTG_H_SYNC_A_POL, t.h_sync_pol);
    w->v_total = t.v_total;
    w->v_sync = FIELD(OTG0_OTG_V_SYNC_A, OTG_V_SYNC_A_START, t.v_sync_start) |
                FIELD(OTG0_OTG_V_SYNC_A, OTG_V_SYNC_A_END, t.v_sync_end);
    w->v_blank = FIELD(OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_START, t.v_blank_start) |
                 FIELD(OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_END, t.v_blank_end);
    w->v_sync_cntl = SET(c->v_sync_cntl, OTG0_OTG_V_SYNC_A_CNTL, OTG_V_SYNC_A_POL, t.v_sync_pol);

    /* dml32: VStartup = MaxVStartupLines = vblank - max(1, writeback delay) */
    vblank = m->v_total - m->v_active;
    vstartup = vblank - 1;
    if (vstartup > 1023)
        vstartup = 1023;
    old_vstartup = GET(c->vstartup, OTG0_OTG_VSTARTUP_PARAM, VSTARTUP_START);
    w->vstartup = SET(c->vstartup, OTG0_OTG_VSTARTUP_PARAM, VSTARTUP_START, vstartup);
    /* dml32_CalculateVUpdateAndDynamicMetadataParameters: VUpdateOffset =
     * ceil(htotal / 4); VUpdateWidth and VReadyOffset are times (clocks
     * that do not change here) times the pixel clock */
    upd_off = (uint32_t)udiv_ceil(m->h_total, 4);
    old_upd_w = GET(c->vupdate, OTG0_OTG_VUPDATE_PARAM, VUPDATE_WIDTH);
    old_rdy = GET(c->vready, OTG0_OTG_VREADY_PARAM, VREADY_OFFSET);
    upd_w = (uint32_t)udiv_ceil((uint64_t)old_upd_w * m->pixel_khz, cur_pclk_khz);
    rdy = (uint32_t)udiv_ceil((uint64_t)old_rdy * m->pixel_khz, cur_pclk_khz);
    w->vupdate = FIELD(OTG0_OTG_VUPDATE_PARAM, VUPDATE_OFFSET, upd_off) |
                 FIELD(OTG0_OTG_VUPDATE_PARAM, VUPDATE_WIDTH, upd_w);
    w->vready = SET(c->vready, OTG0_OTG_VREADY_PARAM, VREADY_OFFSET, rdy);
    /* optc1_set_vtg_params: VCOUNT_INIT = v_total - vfp; FP2 when VSTARTUP
     * reaches before the end of blank */
    vfp = m->v_front < 1 ? 1 : m->v_front;
    v_init = m->v_total - vfp;
    vb_end = v_init - m->v_active;
    {
        int32_t line_start = (int32_t)vb_end - (int32_t)vstartup + 1;
        uint32_t fp2 = line_start < 0 ? (uint32_t)(-line_start) : 0;
        w->vtg = SET(SET(c->vtg, VTG0_CONTROL, VTG0_FP2, fp2), VTG0_CONTROL, VTG0_VCOUNT_INIT, v_init);
    }

    /* dml32_rq_dlg_get_dlg_reg: the timing-only registers exactly */
    w->ref_to_pix = SET(c->ref_to_pix, HUBPREQ0_REF_FREQ_TO_PIX_FREQ, REF_FREQ_TO_PIX_FREQ, r_new);
    w->dst_dim = SET(c->dst_dim, HUBPREQ0_DST_DIMENSIONS, REFCYC_PER_HTOTAL,
                     (uint32_t)((r_new * m->h_total) >> 11));   /* x 2^8, r in 2^19 */
    w->blank0 = FIELD(HUBPREQ0_BLANK_OFFSET_0, REFCYC_H_BLANK_END, (uint32_t)((r_new * t.h_blank_end) >> 19)) |
                FIELD(HUBPREQ0_BLANK_OFFSET_0, DLG_V_BLANK_END, t.v_blank_end);
    w->qos_wm = SET(c->qos_wm, HUBPREQ0_DCN_TTU_QOS_WM, QoS_LEVEL_HIGH_WM,
                    (uint32_t)((4ull * m->h_total * r_new) >> 19));
    /* MIN_DST_Y_NEXT_START = (vtotal - vfp) + 1 + floor(4 TSetup / line) / 4,
     * TSetup = (VUpdateOffset + VUpdateWidth + VReadyOffset) / pclk; x 4 */
    tsetup_lines4 = (4ull * (upd_off + upd_w + rdy)) / m->h_total;
    w->blank1 = SET(c->blank1, HUBPREQ0_BLANK_OFFSET_1, MIN_DST_Y_NEXT_START,
                    (uint32_t)(4ull * (v_init + 1) + tsetup_lines4));
    /* the delivery rates: the same bytes per pixel time, rescaled to the
     * new pixel time (and, per line, the new width) */
    w_old = c->h_active ? c->h_active : m->h_active;
    w->after_scaler = SET(c->after_scaler, HUBPREQ0_DST_AFTER_SCALER, REFCYC_X_AFTER_SCALER,
                          scale(GET(c->after_scaler, HUBPREQ0_DST_AFTER_SCALER, REFCYC_X_AFTER_SCALER), r_new, r_old));
    w->line = SET(c->line, HUBPREQ0_PER_LINE_DELIVERY, REFCYC_PER_LINE_DELIVERY_L,
                  scale(GET(c->line, HUBPREQ0_PER_LINE_DELIVERY, REFCYC_PER_LINE_DELIVERY_L),
                        r_new * m->h_active, r_old * w_old));
    w->line_pre = SET(c->line_pre, HUBPREQ0_PER_LINE_DELIVERY_PRE, REFCYC_PER_LINE_DELIVERY_PRE_L,
                      scale(GET(c->line_pre, HUBPREQ0_PER_LINE_DELIVERY_PRE, REFCYC_PER_LINE_DELIVERY_PRE_L),
                            r_new * m->h_active, r_old * w_old));
    w->surf0_cntl0 = SET(c->surf0_cntl0, HUBPREQ0_DCN_SURF0_TTU_CNTL0, REFCYC_PER_REQ_DELIVERY,
                         scale(GET(c->surf0_cntl0, HUBPREQ0_DCN_SURF0_TTU_CNTL0, REFCYC_PER_REQ_DELIVERY), r_new, r_old));
    w->surf0_cntl1 = SET(c->surf0_cntl1, HUBPREQ0_DCN_SURF0_TTU_CNTL1, REFCYC_PER_REQ_DELIVERY_PRE,
                         scale(GET(c->surf0_cntl1, HUBPREQ0_DCN_SURF0_TTU_CNTL1, REFCYC_PER_REQ_DELIVERY_PRE), r_new, r_old));
    /* min_ttu_vblank: the vertical blank's time share is what it bounds */
    {
        uint64_t t_old = (uint64_t)(GET(c->v_total, OTG0_OTG_V_TOTAL, OTG_V_TOTAL) + 1 - c->v_active) *
                         (GET(c->h_total, OTG0_OTG_H_TOTAL, OTG_H_TOTAL) + 1) * m->pixel_khz;
        uint64_t t_new = (uint64_t)vblank * m->h_total * cur_pclk_khz;
        uint32_t ttu = GET(c->global_ttu, HUBPREQ0_DCN_GLOBAL_TTU_CNTL, MIN_TTU_VBLANK);
        w->global_ttu = SET(c->global_ttu, HUBPREQ0_DCN_GLOBAL_TTU_CNTL, MIN_TTU_VBLANK, scale(ttu, t_new, t_old));
    }
    /* prefetch: the GOP's margin between VSTARTUP and the prefetch start
     * kept; fewer lines to prefetch in -> a higher vratio */
    pf_old = GET(c->prefetch, HUBPREQ0_PREFETCH_SETTINGS, DST_Y_PREFETCH);   /* quarter lines */
    margin_q = old_vstartup * 4 > pf_old ? old_vstartup * 4 - pf_old : 0;
    pf_q = vstartup * 4 > margin_q + 8 ? vstartup * 4 - margin_q : 8;
    if (pf_q > pf_old)
        pf_q = pf_old;   /* never prefetch longer than the GOP's schedule */
    if (pf_q > 255)
        pf_q = 255;
    vratio = GET(c->prefetch, HUBPREQ0_PREFETCH_SETTINGS, VRATIO_PREFETCH);
    if (pf_q && pf_q < pf_old)
        vratio = scale(vratio, pf_old, pf_q);
    if (vratio > (4u << 19))
        vratio = 4u << 19;   /* dml32: VRatioPrefetch <= 4 */
    w->prefetch = FIELD(HUBPREQ0_PREFETCH_SETTINGS, DST_Y_PREFETCH, pf_q) |
                  FIELD(HUBPREQ0_PREFETCH_SETTINGS, VRATIO_PREFETCH, vratio);

    /* the plane */
    w->viewport_dim = m->h_active | m->v_active << 16;
    w->recout_size = m->h_active | m->v_active << DSCL0_RECOUT_SIZE__RECOUT_HEIGHT__SHIFT;
    w->mpc_size = m->h_active | m->v_active << DSCL0_MPC_SIZE__MPC_HEIGHT__SHIFT;
    if (GET(c->oppbuf, OPPBUF0_OPPBUF_CONTROL, OPPBUF_ACTIVE_WIDTH))
        w->oppbuf = SET(c->oppbuf, OPPBUF0_OPPBUF_CONTROL, OPPBUF_ACTIVE_WIDTH, m->h_active);
    RLOG(d, "modeset: head %u (pipe %u otg %u opp %u DIG %u/%u mode %u) %ux%u@%u kHz -> %ux%u %u kHz "
         "(htotal %u vtotal %u): vstartup %u->%u, refclk %llu kHz, ref/pix 0x%llx->0x%llx",
         head, ms->pipe, ms->otg, ms->opp, ms->fe, ms->be, ms->dig_mode, c->h_active, c->v_active,
         cur_pclk_khz, m->h_active, m->v_active, m->pixel_khz, m->h_total, m->v_total, old_vstartup,
         vstartup, (unsigned long long)refclk_khz, (unsigned long long)r_old, (unsigned long long)r_new);
    if (ms->raise_dispclk_khz || ms->raise_dppclk_khz)
        RLOG(d, "modeset: needs DISPCLK %u kHz (runs %u) DPPCLK %u kHz (pipe %u runs %u)", ms->raise_dispclk_khz,
             ms->clk.dispclk_khz, ms->raise_dppclk_khz, ms->pipe, dcn32_pipe_dppclk(d, &ms->clk, ms->pipe));
    return RD_MODESET_OK;
}

/* get_clock_source_id: a TMDS stream's pixel clock comes from the PHY PLL of
 * its transmitter (CLOCK_SOURCE_COMBO_PHY_PLL0 + n = ATOM_COMBOPHY_PLL0 + n) */
static int vbios_pclk(rdna3_dmub *dm, const rdna3_dcn_modeset *ms, uint32_t khz)
{
    rdna3_pixclk_v1_7 p = { 0 };
    p.pixclk_100hz = khz * 10;
    p.pll_id = (uint8_t)(RD_ATOM_COMBOPHY_PLL0 + ms->be);
    p.encoderobjid = ms->encoder_id;
    p.encoder_mode = ms->dig_mode == 2 ? RD_ATOM_ENCODER_MODE_DVI : RD_ATOM_ENCODER_MODE_HDMI;
    p.crtc_id = (uint8_t)ms->otg;
    return dmub32_set_pixel_clock(dm, &p);
}

static int vbios_tx(rdna3_dmub *dm, const rdna3_dcn_modeset *ms, uint8_t action, uint32_t khz)
{
    rdna3_dig_tx_v1_7 t = { 0 };
    t.phyid = (uint8_t)ms->be;
    t.action = action;
    t.lanenum = 4;
    t.hpdsel = (uint8_t)(ms->hpd >= 0 ? ms->hpd + 1 : 0);
    t.connobj_id = ms->connector_id;
    t.digmode = (uint8_t)ms->dig_mode;
    t.digfe_sel = (uint8_t)(1u << ms->fe);
    t.symclk_10khz = khz / 10;
    return dmub32_transmitter_control(dm, &t);
}

static int vbios_stream(rdna3_dmub *dm, const rdna3_dcn_modeset *ms, uint32_t khz)
{
    rdna3_dig_enc_v1_5 e = { 0 };
    e.digid = (uint8_t)ms->fe;
    e.action = RD_ATOM_ENCODER_CMD_STREAM_SETUP;
    e.digmode = ms->dig_mode == 2 ? RD_ATOM_ENCODER_MODE_DVI : RD_ATOM_ENCODER_MODE_HDMI;
    e.lanenum = 4;
    e.pclk_10khz = khz / 10;
    e.bitpercolor = RD_ATOM_PANEL_8BIT_PER_COLOR;
    return dmub32_encoder_control(dm, &e);
}

/* one direction of the switch: stream down (running at 'running_khz'),
 * timing 'r' (mode 'tm'), stream up */
static int program(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn *dc, const rdna3_dcn_modeset *ms,
                   const rdna3_modeset_regs *r, const rdna3_mode *tm, uint64_t mc, uint32_t pitch_px,
                   uint32_t running_khz)
{
    int symclk_on_tx_off = 0;
    /* rdna3hdmi: DC's HDMI stream (an RGB 8 bpc stream, phy_pix_clk = the pixel clock) */
    const rdna3_crtc_format fmt = { RD_PIXEL_ENCODING_RGB, RD_COLOR_DEPTH_888, 0, 0 };
    int dc_tmds = (ms->hdmi.flags & RD_HDMI_STREAM) != 0, hdmi = dc_tmds && ms->dig_mode == 3;
    uint32_t v;
    int ret, ret2;

    /* link_set_dpms_off: set_avmute (dcn30_set_avmute: two frames), blank */
    if (hdmi)
        dcn32_enc_set_avmute(d, ms->fe, 1, (int)ms->otg);
    /* hubp2_set_blank, dcn10_link_encoder_disable_output */
    dcn32_set_blank(d, dc, ms->head, 1);
    if (hdmi) {
        /* the sink back to HDMI 1.4 rates ("vbios only handles 165Mhz"),
         * then dce110_disable_stream's info packets off and attribute reset */
        if (ms->hdmi.ddc >= 0 && (ret2 = dcn32_hdmi_write_scdc(d, (uint32_t)ms->hdmi.ddc, 165000, 0)) != 0)
            RLOG(d, "modeset: head %u: SCDC write (legacy rates) failed (%d)", ms->head, ret2);
        dcn32_enc_stop_hdmi_info_packets(d, ms->fe);
        dcn32_enc_reset_hdmi_stream_attribute(d, ms->fe);
    }
    ret = vbios_tx(dm, ms, RD_ATOM_TX_DISABLE, r->pclk_khz);
    /* dcn32_disable_link_output -> apply_symclk_on_tx_off_wa: the OTG of an
     * HDMI stream still runs on SYMCLK, which the PHY just stopped: the
     * pixel clock again (SYMCLK_ON_TX_OFF) */
    if (ms->dc_seq && ms->dig_mode == 3 && running_khz &&
        GET(RI(d, regOTG0_OTG_CONTROL, ms->otg, OTG_S), OTG0_OTG_CONTROL, OTG_MASTER_EN)) {
        ret2 = vbios_pclk(dm, ms, running_khz);
        if (ret2)
            RERR(d, "modeset: head %u: SYMCLK on with TX off (%u kHz) failed (%d)", ms->head, running_khz, ret2);
        else
            symclk_on_tx_off = 1;
    }
    /* optc32_disable_crtc: OTG then VTG off, REG_WAIT(OTG_BUSY, 0, 1 us, 150000) */
    v = RI(d, regOTG0_OTG_CONTROL, ms->otg, OTG_S);
    WI(d, regOTG0_OTG_CONTROL, ms->otg, OTG_S, SET(v, OTG0_OTG_CONTROL, OTG_MASTER_EN, 0));
    v = RI(d, regVTG0_CONTROL, ms->otg, VTG_S);
    WI(d, regVTG0_CONTROL, ms->otg, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 0));
    if (rdna3_wait_reg(d, AI(d, regOTG0_OTG_CLOCK_CONTROL, ms->otg, OTG_S), 0,
                       OTG0_OTG_CLOCK_CONTROL__OTG_BUSY_MASK, 0, 150000))
        RERR(d, "modeset: OTG %u stays busy after the disable", ms->otg);
    /* dcn20_reset_back_end_for_pipe: no OTG on SYMCLK any more, the link
     * output off again (SYMCLK_OFF_TX_OFF) */
    if (symclk_on_tx_off) {
        ret2 = vbios_tx(dm, ms, RD_ATOM_TX_DISABLE, running_khz);
        if (ret2 && !ret)
            ret = ret2;
    }
    /* dcn31_program_pix_clk */
    ret2 = vbios_pclk(dm, ms, r->pclk_khz);
    if (ret2 && !ret)
        ret = ret2;
    write_regs(d, ms->pipe, ms->otg, ms->opp, r);
    /* hubp2_program_surface_config's pitch and the address (HIGH first) */
    v = RI(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, ms->pipe, HUBP_S);
    WI(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, ms->pipe, HUBP_S, SET(v, HUBPREQ0_DCSURF_SURFACE_PITCH, PITCH, pitch_px - 1));
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, ms->pipe, HUBP_S, (uint32_t)(mc >> 32) & 0xffff);
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, ms->pipe, HUBP_S, (uint32_t)mc);
    /* the DIG front end for the new clock; no infoframes (the GOP's AVI
     * frame named its own mode) */
    ret2 = vbios_stream(dm, ms, r->pclk_khz);
    if (ret2 && !ret)
        ret = ret2;
    if (hdmi) {
        /* enc32_stream_encoder_hdmi_set_stream_attribute's register half */
        dcn32_enc_hdmi_set_stream_attribute(d, ms->fe, &fmt, r->pclk_khz, ms->hdmi.lte_340_scramble);
    } else {
        if (dc_tmds)
            dcn32_enc_dvi_set_stream_attribute(d, ms->fe, &fmt);
        WI(d, regDIG0_HDMI_INFOFRAME_CONTROL0, ms->fe, DIG_S, 0);
        WI(d, regDIG0_HDMI_INFOFRAME_CONTROL1, ms->fe, DIG_S, 0);
        WI(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL0, ms->fe, DIG_S, 0);
    }
    /* optc32_enable_crtc: "VTG enable first is for HW workaround" */
    v = RI(d, regVTG0_CONTROL, ms->otg, VTG_S);
    WI(d, regVTG0_CONTROL, ms->otg, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 1));
    v = RI(d, regOTG0_OTG_CONTROL, ms->otg, OTG_S);
    WI(d, regOTG0_OTG_CONTROL, ms->otg, OTG_S,
       SET(SET(v, OTG0_OTG_CONTROL, OTG_DISABLE_POINT_CNTL, 2), OTG0_OTG_CONTROL, OTG_MASTER_EN, 1));
    /* enable_link_hdmi: write_scdc_data, dcn10_link_encoder_enable_tmds_output,
     * read_scdc_data */
    if (hdmi && ms->hdmi.ddc >= 0 &&
        (ret2 = dcn32_hdmi_write_scdc(d, (uint32_t)ms->hdmi.ddc, r->pclk_khz, ms->hdmi.lte_340_scramble)) != 0)
        RLOG(d, "modeset: head %u: SCDC write for %u kHz failed (%d)", ms->head, r->pclk_khz, ret2);
    ret2 = vbios_tx(dm, ms, RD_ATOM_TX_ENABLE, r->pclk_khz);
    if (ret2 && !ret)
        ret = ret2;
    if (hdmi) {
        rdna3_hdmi_info_frames f;   /* 280 bytes */
        if (ms->hdmi.ddc >= 0) {
            rdna3_scdc_state sc;
            ret2 = dcn32_hdmi_read_scdc(d, (uint32_t)ms->hdmi.ddc, &sc);
            RLOG(d, "modeset: head %u: HDMI_CONTROL 0x%x, SCDC TMDS_CONFIG 0x%x scrambler 0x%x flags 0x%x 0x%x (%d)",
                 ms->head, dcn32_enc_hdmi_control(d, ms->fe), sc.tmds_config, sc.scrambler_status,
                 sc.status_flags[0], sc.status_flags[1], ret2);
        }
        /* dce110_enable_stream -> dcn30_update_info_frame: the AVI / vendor
         * infoframes resource_build_info_frame made for the stream */
        dcn32_hdmi_build_info_frames(&f, tm, &fmt, ms->hdmi.hdmi_sink);
        dcn32_enc_update_hdmi_info_packets(d, ms->fe, &f);
    }
    /* clear the sticky underflow bit for the check, then unblank */
    v = RI(d, regHUBP0_DCHUBP_CNTL, ms->pipe, HUBP_S);
    WI(d, regHUBP0_DCHUBP_CNTL, ms->pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_UNDERFLOW_CLEAR, 1));
    dcn32_set_blank(d, dc, ms->head, 0);
    /* link_set_dpms_on: AV mute off */
    if (hdmi)
        dcn32_enc_set_avmute(d, ms->fe, 0, -1);
    return ret;
}

/* the same for a DP SST head: link_set_dpms_off, the timing with the DP DTO
 * (dcn31_program_pix_clk), the link cap verified once (detection's
 * dp_verify_link_cap_with_retries), OTG on, link_set_dpms_on */
static int program_dp(rdna3_dev *d, rdna3_dcn *dc, const rdna3_dcn_modeset *ms, const rdna3_modeset_regs *r,
                      const rdna3_mode *m, int dsc, const rdna3_dsc_config *dsc_cfg, uint64_t mc, uint32_t pitch_px)
{
    rdna3_dp_link *dp = ms->dp;
    uint32_t v;
    int ret;

    /* hubp2_set_blank, link_set_dpms_off */
    dcn32_set_blank(d, dc, ms->head, 1);
    dcn32_dp_link_set_dpms_off(dp);
    /* optc32_disable_crtc */
    v = RI(d, regOTG0_OTG_CONTROL, ms->otg, OTG_S);
    WI(d, regOTG0_OTG_CONTROL, ms->otg, OTG_S, SET(v, OTG0_OTG_CONTROL, OTG_MASTER_EN, 0));
    v = RI(d, regVTG0_CONTROL, ms->otg, VTG_S);
    WI(d, regVTG0_CONTROL, ms->otg, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 0));
    if (rdna3_wait_reg(d, AI(d, regOTG0_OTG_CLOCK_CONTROL, ms->otg, OTG_S), 0,
                       OTG0_OTG_CLOCK_CONTROL__OTG_BUSY_MASK, 0, 150000))
        RERR(d, "modeset: OTG %u stays busy after the disable", ms->otg);
    /* dcn31_program_pix_clk: DP signals run on the DP DTO */
    dcn32_dp_program_pix_clk(dp, r->pclk_khz);
    write_regs(d, ms->pipe, ms->otg, ms->opp, r);
    v = RI(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, ms->pipe, HUBP_S);
    WI(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, ms->pipe, HUBP_S, SET(v, HUBPREQ0_DCSURF_SURFACE_PITCH, PITCH, pitch_px - 1));
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, ms->pipe, HUBP_S, (uint32_t)(mc >> 32) & 0xffff);
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, ms->pipe, HUBP_S, (uint32_t)mc);
    /* the link cap: what the sink trains at, falling back from its max
     * (verify_link_capability; an MST link is not trained to verify it:
     * verify_link_capability_non_destructive takes the max link cap) */
    if (!dp->verified) {
        if (dp->mst) {
            dp->verified_link_cap = dp->max_link_cap;
            dp->verified = 1;
        } else {
            dcn32_dp_verify_link_cap(dp, RD_DP_LINK_TRAINING_MAX_VERIFY_RETRY);
        }
    }
    /* optc32_enable_crtc */
    v = RI(d, regVTG0_CONTROL, ms->otg, VTG_S);
    WI(d, regVTG0_CONTROL, ms->otg, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 1));
    v = RI(d, regOTG0_OTG_CONTROL, ms->otg, OTG_S);
    WI(d, regOTG0_OTG_CONTROL, ms->otg, OTG_S,
       SET(SET(v, OTG0_OTG_CONTROL, OTG_DISABLE_POINT_CNTL, 2), OTG0_OTG_CONTROL, OTG_MASTER_EN, 1));
    /* link_set_dpms_on (DC trains without fallback here: the verified
     * cap already fell back) */
    dp->stream_dsc = dsc;
    dp->dsc_cfg = *dsc_cfg;
    ret = dcn32_dp_link_set_dpms_on(dp, m, 0);
    if (ret && dp->stream_dsc)
        RERR(d, "modeset: head %u: the DSC stream did not come up (%d)", ms->head, ret);
    v = RI(d, regHUBP0_DCHUBP_CNTL, ms->pipe, HUBP_S);
    WI(d, regHUBP0_DCHUBP_CNTL, ms->pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_UNDERFLOW_CLEAR, 1));
    dcn32_set_blank(d, dc, ms->head, 0);
    return ret;
}

static void update_head(rdna3_dcn *dc, rdna3_dcn_scale *sc, const rdna3_dcn_modeset *ms,
                        const rdna3_modeset_regs *r, uint64_t mc, uint32_t pitch_px)
{
    rdna3_dcn_head *h = &dc->head[ms->head];
    uint32_t hb = r->h_blank, vb = r->v_blank;
    h->width = h->h_active = r->h_active;
    h->height = h->v_active = r->v_active;
    h->h_total = GET(r->h_total, OTG0_OTG_H_TOTAL, OTG_H_TOTAL) + 1;
    h->v_total = GET(r->v_total, OTG0_OTG_V_TOTAL, OTG_V_TOTAL) + 1;
    h->h_front = h->h_total - GET(hb, OTG0_OTG_H_BLANK_START_END, OTG_H_BLANK_START);
    h->v_front = h->v_total - GET(vb, OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_START);
    h->h_sync = GET(r->h_sync, OTG0_OTG_H_SYNC_A, OTG_H_SYNC_A_END);
    h->v_sync = GET(r->v_sync, OTG0_OTG_V_SYNC_A, OTG_V_SYNC_A_END);
    h->h_sync_neg = GET(r->h_sync_cntl, OTG0_OTG_H_SYNC_A_CNTL, OTG_H_SYNC_A_POL);
    h->v_sync_neg = GET(r->v_sync_cntl, OTG0_OTG_V_SYNC_A_CNTL, OTG_V_SYNC_A_POL);
    h->pitch_px = pitch_px;
    h->address = mc;
    dc->requested[ms->pipe] = mc;
    if (ms->dp)
        h->dig_mode = ms->dp->mst ? 5 : 0;   /* an SST head taken over as MST runs DIG_MODE 5 */
    if (sc) {
        sc->saved[ms->pipe] = 0;   /* the bypass state is the one just written */
        sc->bw_saved[ms->pipe] = 0;
        sc->native_w[ms->pipe] = r->h_active;
        sc->native_h[ms->pipe] = r->v_active;
    }
}

int dcn32_modeset_apply(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn *dc, rdna3_dcn_scale *sc,
                        const rdna3_dcn_modeset *ms, uint64_t mc, uint32_t pitch_px,
                        uint64_t old_mc, uint32_t old_pitch_px, uint32_t settle_ms)
{
    const uint32_t fmask = OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;
    uint32_t f0, f1, uf;
    int ret;

    if (!dm || !dm->hw_init)
        return RDNA3_ENODEV;
    if (ms->dp)
        ms->dp->dm = dm;   /* the transmitter commands go through it */
    if (!dc || ms->head >= dc->nheads || pitch_px < ms->want.h_active || (mc & 0xff) || mc < d->fb_start ||
        mc - d->fb_start + (uint64_t)pitch_px * 4 * ms->want.v_active > d->vram_size)
        return RDNA3_EINVAL;
    /* dcn32_update_clocks(safe_to_lower = false) before the new timing:
     * the faster clocks first, nothing touched when they do not come */
    if (ms->raise_dispclk_khz || ms->raise_dppclk_khz) {
        ret = dcn32_clk_raise(d, ms->pmfw, ms->pipe, ms->raise_dispclk_khz, ms->raise_dppclk_khz, NULL);
        if (ret) {
            RERR(d, "modeset: head %u: DISPCLK %u / DPPCLK %u kHz not set (%d): stays at %ux%u", ms->head,
                 ms->raise_dispclk_khz, ms->raise_dppclk_khz, ret, ms->cur.h_active, ms->cur.v_active);
            return RDNA3_EBUSY;
        }
    }
    /* a scaled source first goes back to the GOP's bypass state */
    if (sc && sc->saved[ms->pipe])
        dcn32_set_source(d, dc, sc, ms->head, old_mc, sc->native_w[ms->pipe], sc->native_h[ms->pipe], old_pitch_px);
    ret = ms->dp ? program_dp(d, dc, ms, &ms->want, &ms->dp_want, ms->dp_want_dsc, &ms->dp_want_dsc_cfg, mc, pitch_px)
                 : program(d, dm, dc, ms, &ms->want, &ms->tmds_want, mc, pitch_px, ms->cur.pclk_khz);
    /* dce110_apply_ctx_to_hw: dcn32_resync_fifo_dccg_dio after the stream */
    if (ms->dc_seq)
        RLOG(d, "modeset: head %u: DIO FIFO resync (DISPCLK divider 0x%x)", ms->head, dcn32_resync_fifo_dccg_dio(d));
    if (!ret) {
        f0 = RI(d, regOTG0_OTG_STATUS_FRAME_COUNT, ms->otg, OTG_S) & fmask;
        d->os.msleep(d->os.ctx, settle_ms);
        f1 = RI(d, regOTG0_OTG_STATUS_FRAME_COUNT, ms->otg, OTG_S) & fmask;
        uf = GET(RI(d, regHUBP0_DCHUBP_CNTL, ms->pipe, HUBP_S), HUBP0_DCHUBP_CNTL, HUBP_UNDERFLOW_STATUS);
        if (f1 == f0 || uf) {
            RERR(d, "modeset: head %u at %ux%u %u kHz %s: back to %ux%u", ms->head, ms->want.h_active,
                 ms->want.v_active, ms->want.pclk_khz, f1 == f0 ? "does not run" : "underflows",
                 ms->cur.h_active, ms->cur.v_active);
            ret = RDNA3_EIO;
        }
    } else {
        RERR(d, "modeset: %s failed (%d): back to %ux%u", ms->dp ? "the DP link" : "a VBIOS command", ret,
             ms->cur.h_active, ms->cur.v_active);
    }
    if (ret) {
        if (ms->dp)
            program_dp(d, dc, ms, &ms->cur, &ms->dp_cur, ms->dp_cur_dsc, &ms->dp_cur_dsc_cfg, old_mc, old_pitch_px);
        else
            program(d, dm, dc, ms, &ms->cur, &ms->tmds_cur, old_mc, old_pitch_px, ms->want.pclk_khz);
        if (ms->dc_seq)
            dcn32_resync_fifo_dccg_dio(d);


        update_head(dc, sc, ms, &ms->cur, old_mc, old_pitch_px);
        return ret;
    }
    update_head(dc, sc, ms, &ms->want, mc, pitch_px);
    RLOG(d, "modeset: head %u runs %ux%u at %u kHz (frames %u in %u ms)", ms->head, ms->want.h_active,
         ms->want.v_active, ms->want.pclk_khz, (f1 - f0) & fmask, settle_ms);
    return RDNA3_OK;
}
