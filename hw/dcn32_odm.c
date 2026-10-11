/*
 * dcn32_odm.c - see dcn32_odm.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_odm.h"
#include "dcn32_clk.h"
#include "dcn32_dsc.h"
#include "dcn32_otg.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_clone.h"
#include "gen/dcn_3_2_0_scale.h"
#include "gen/dcn_3_2_0_mode.h"
#include "gen/dcn_3_2_0_odm.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))
#define FIELD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)

#define RI(d, reg, n, stride) rd((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n))
#define WI(d, reg, n, stride, v) wr((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n), (v))
#define AI(d, reg, n, stride) \
    rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n))

#define HUBP_S  RD_CLONE_HUBP_STRIDE
#define DPP_S   RD_CLONE_DPP_STRIDE
#define MPCC_S  RD_CLONE_MPCC_STRIDE
#define OPP_S   RD_CLONE_OPP_STRIDE
#define ODM_S   RD_CLONE_ODM_STRIDE
#define OTG_S   RD_CLONE_OTG_STRIDE
#define DET_S   (regDCHUBBUB_DET1_CTRL - regDCHUBBUB_DET0_CTRL)
#define MUX_S   (regMPC_OUT1_MUX - regMPC_OUT0_MUX)
#define PG_S    (regDOMAIN1_PG_CONFIG - regDOMAIN0_PG_CONFIG)
#define DPPDTO_S (regDPPCLK1_DTO_PARAM - regDPPCLK0_DTO_PARAM)
#define MPCC_NONE 0xfu

#define PGFSM_POWER_ON  0
#define PGFSM_POWER_OFF 2

static uint32_t rd(rdna3_dev *d, uint32_t seg, uint32_t off)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off));
}

static void wr(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off), v);
}

static void copy_list(rdna3_dev *d, const uint16_t *list, uint32_t n, uint32_t seg, uint32_t stride,
                      uint32_t from, uint32_t to)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        wr(d, seg, list[i] + stride * to, rd(d, seg, list[i] + stride * from));
}
#define COPY(d, blk, BLK, from, to) \
    copy_list((d), rd_clone_##blk, sizeof(rd_clone_##blk) / sizeof(rd_clone_##blk[0]), \
              RD_CLONE_##BLK##_SEG, RD_CLONE_##BLK##_STRIDE, (from), (to))

/* ---- the ODM decision (dml32) ---- */

uint32_t dcn32_odm_mode(int hdmi, uint32_t h_active, uint32_t pclk_khz, uint32_t state_dispclk_khz,
                        uint32_t vco_khz, int dsc, uint32_t dsc_slices)
{
    /* dml32_CalculateRequiredDispclk at the state's DISPCLK: 0 when even
     * the clock without the ramping margin is above it (RequiredDispclk >
     * StateDispclk in dml32) */
    int without_fits = dcn32_required_dispclk(pclk_khz, vco_khz, state_dispclk_khz) != 0;
    int two_to_one_fits = dcn32_required_dispclk(pclk_khz / 2, vco_khz, state_dispclk_khz) != 0;

    /* Output == dm_hdmi: never combined; 4:1 is not taken for DP / HDMI */
    if (hdmi)
        return 1;
    if ((!without_fits && two_to_one_fits) || (dsc && h_active > RD_DCN32_MAX_PIXELS_PER_LINE_PER_DSC_UNIT) ||
        (dsc_slices <= 8 && dsc_slices > 4))
        return 2;
    return 1;
}

/* ---- dc_resource.c ---- */

uint32_t dcn32_odm_slice_dst_width(uint32_t h_active, uint32_t opp_cnt, int is_last_segment, int two_pix)
{
    uint32_t width;
    if (!opp_cnt)
        return 0;
    width = h_active / opp_cnt;
    if ((width % 2) && two_pix)
        width++;
    if (is_last_segment)
        width = h_active - width * (opp_cnt - 1);
    return width;
}

/* ---- hwseq / OPP ---- */

uint32_t dcn32_odm_get_config(rdna3_dev *d, uint32_t otg, uint32_t *opp_inst)
{
    uint32_t sel = RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, otg, ODM_S);
    uint32_t n = GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_NUM_OF_INPUT_SEGMENT);
    uint32_t cnt = n == 0 ? 1 : n == 1 ? 2 : 4;
    if (opp_inst) {
        opp_inst[0] = GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL);
        if (cnt > 1)
            opp_inst[1] = GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG1_SRC_SEL);
        if (cnt > 2) {
            opp_inst[2] = GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG2_SRC_SEL);
            opp_inst[3] = GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG3_SRC_SEL);
        }
    }
    return cnt;
}

void dcn32_opp_pipe_clock_control(rdna3_dev *d, uint32_t opp, int enable)
{
    uint32_t v = RI(d, regOPP_PIPE0_OPP_PIPE_CONTROL, opp, OPP_S);
    WI(d, regOPP_PIPE0_OPP_PIPE_CONTROL, opp, OPP_S, SET(v, OPP_PIPE0_OPP_PIPE_CONTROL, OPP_PIPE_CLOCK_EN, enable ? 1 : 0));
}

/* opp2_get_left_edge_extra_pixel_count (force_chroma_subsampling_1tap off) */
static uint32_t opp2_get_left_edge_extra_pixel_count(uint32_t pixel_encoding, int is_primary)
{
    if ((pixel_encoding == RD_PIXEL_ENCODING_YCBCR422 || pixel_encoding == RD_PIXEL_ENCODING_YCBCR420) && !is_primary)
        return 1;
    return 0;
}

void dcn32_opp_program_left_edge_extra_pixel(rdna3_dev *d, uint32_t opp, uint32_t pixel_encoding, int is_primary)
{
    uint32_t v = RI(d, regFMT0_FMT_422_CONTROL, opp, OPP_S);
    WI(d, regFMT0_FMT_422_CONTROL, opp, OPP_S,
       SET(v, FMT0_FMT_422_CONTROL, FMT_LEFT_EDGE_EXTRA_PIXEL_COUNT,
           opp2_get_left_edge_extra_pixel_count(pixel_encoding, is_primary)));
}

void dcn32_update_odm(rdna3_dev *d, uint32_t otg, const uint32_t *opp_inst, uint32_t opp_cnt, uint32_t h_active,
                      uint32_t pixel_encoding)
{
    uint32_t k;
    if (opp_cnt > 1)
        dcn32_otg_set_odm_combine(d, otg, opp_inst, opp_cnt, dcn32_odm_slice_dst_width(h_active, opp_cnt, 0, 0));
    else
        dcn32_otg_set_odm_bypass(d, otg, pixel_encoding == RD_PIXEL_ENCODING_YCBCR420);
    /* the ODM pipes (not the OTG master): OPP clock, left edge */
    for (k = 1; k < opp_cnt; k++) {
        dcn32_opp_pipe_clock_control(d, opp_inst[k], 1);
        dcn32_opp_program_left_edge_extra_pixel(d, opp_inst[k], pixel_encoding, 0);
    }
}

/* ---- the ODM pipe ---- */

/* dcn10_power_on_plane_resources / dcn32_hubp_pg_control (DPP shares the
 * HUBP's power domain on DCN 3.2) */
static int pipe_power(rdna3_dev *d, uint32_t pipe, int on)
{
    uint32_t v, want = on ? PGFSM_POWER_ON : PGFSM_POWER_OFF;
    int ret;
    WI(d, regDC_IP_REQUEST_CNTL, 0, 0, DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK);
    v = RI(d, regDOMAIN0_PG_CONFIG, pipe, PG_S);
    WI(d, regDOMAIN0_PG_CONFIG, pipe, PG_S, SET(v, DOMAIN0_PG_CONFIG, DOMAIN_POWER_GATE, on ? 0 : 1));
    ret = rdna3_wait_reg(d, AI(d, regDOMAIN0_PG_STATUS, pipe, PG_S),
                         want << DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS__SHIFT,
                         DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS_MASK, 0, 1000);
    WI(d, regDC_IP_REQUEST_CNTL, 0, 0, 0);
    if (ret)
        RERR(d, "odm: pipe %u power %s timed out", pipe, on ? "on" : "off");
    return ret;
}

/* dcn32_program_det_size */
static int det_set(rdna3_dev *d, uint32_t pipe, uint32_t segs)
{
    uint32_t v = RI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S);
    int ret;
    WI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S, SET(v, DCHUBBUB_DET0_CTRL, DET0_SIZE, segs));
    ret = rdna3_wait_reg(d, AI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S),
                         segs << DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT__SHIFT,
                         DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT_MASK, 0, 1000);
    if (ret)
        RERR(d, "odm: DET%u -> %u segments timed out", pipe, segs);
    return ret;
}

int dcn32_odm_acquire(rdna3_dev *d, const rdna3_dcn *dc, uint32_t main_pipe, uint32_t main_opp, uint32_t otg,
                      rdna3_odm_pipe *p)
{
    uint32_t cur_opp[4] = { 0, 0, 0, 0 }, cur = dcn32_odm_get_config(d, otg, cur_opp), q, o, i;
    int found = 0;
    p->main_pipe = main_pipe;
    p->main_opp = main_opp;
    p->otg = otg;
    /* the pipe this OTG combines already (pipe_ctx of the current state) */
    if (cur == 2 && cur_opp[1] < RD_DCN_MAX_PIPES && cur_opp[1] != main_opp) {
        p->pipe = cur_opp[1];
        found = 1;
    }
    for (q = 0; q < RD_DCN_MAX_PIPES && !found; q++) {
        int used = q == main_pipe || q == main_opp;
        for (i = 0; dc && i < dc->nheads; i++)
            used |= dc->head[i].pipe == q || dc->head[i].otg == q;
        if (used || GET(RI(d, regMPCC0_MPCC_TOP_SEL, q, MPCC_S), MPCC0_MPCC_TOP_SEL, MPCC_TOP_SEL) != MPCC_NONE ||
            GET(RI(d, regMPC_OUT0_MUX, q, MUX_S), MPC_OUT0_MUX, MPC_OUT_MUX) != MPCC_NONE)
            continue;
        /* no running OTG reads this OPP */
        for (o = 0; o < RD_DCN_MAX_PIPES; o++) {
            uint32_t opps[4] = { 0, 0, 0, 0 }, n, k;
            if (o == otg || !GET(RI(d, regOTG0_OTG_CONTROL, o, OTG_S), OTG0_OTG_CONTROL, OTG_MASTER_EN))
                continue;
            n = dcn32_odm_get_config(d, o, opps);
            for (k = 0; k < n; k++)
                used |= opps[k] == q;
        }
        if (used)
            continue;
        p->pipe = q;
        found = 1;
    }
    if (!found) {
        RERR(d, "odm: no free pipe for OTG %u", otg);
        return RDNA3_EBUSY;
    }
    /* a free pipe's HUBP, DPP, MPCC, OPP and DSC share its index */
    p->opp = p->mpcc = p->dsc = p->pipe;
    p->det_main = GET(RI(d, regDCHUBBUB_DET0_CTRL, main_pipe, DET_S), DCHUBBUB_DET0_CTRL, DET0_SIZE);
    p->det_odm = GET(RI(d, regDCHUBBUB_DET0_CTRL, p->pipe, DET_S), DCHUBBUB_DET0_CTRL, DET0_SIZE);
    p->det_main_new = p->det_main;
    p->det_odm_new = p->det_odm;
    if (cur == 2 && p->pipe == cur_opp[1]) {
        /* combined already: the split this driver made (an idle pipe has
         * no DET of its own) goes back to the main pipe on the disable */
        p->det_main = p->det_main_new + p->det_odm_new;
        p->det_odm = 0;
    } else if (p->det_odm < 2) {
        if (p->det_main < 4) {
            RERR(d, "odm: pipe %u has %u DET segments, nothing to split", main_pipe, p->det_main);
            return RDNA3_EBUSY;
        }
        p->det_odm_new = p->det_main / 2;
        p->det_main_new = p->det_main - p->det_odm_new;
    }
    RLOG(d, "odm: OTG %u: pipe %u / OPP %u + pipe %u / OPP %u / DSC %u, DET %u/%u -> %u/%u", otg, main_pipe, main_opp,
         p->pipe, p->opp, p->dsc, p->det_main, p->det_odm, p->det_main_new, p->det_odm_new);
    return RDNA3_OK;
}

int dcn32_odm_pipe_enable(rdna3_dev *d, const rdna3_odm_pipe *p, uint32_t h_active, uint32_t v_active,
                          uint32_t h_blank_end, uint64_t ref_to_pix)
{
    const uint32_t keep_hubp = HUBP0_DCHUBP_CNTL__HUBP_VREADY_AT_OR_AFTER_VSYNC_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_DISABLE_STOP_DATA_DURING_VM_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_UNBOUNDED_REQ_MODE_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_TTU_MODE_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_TIMEOUT_THRESHOLD_MASK;
    uint32_t w0 = dcn32_odm_slice_dst_width(h_active, 2, 0, 0), w1 = dcn32_odm_slice_dst_width(h_active, 2, 1, 0);
    uint32_t v, ctl, k, b0;
    int ret;

    /* power and DET (shrink before grow) */
    ret = pipe_power(d, p->pipe, 1);
    if (!ret && p->det_main_new != p->det_main) {
        ret = det_set(d, p->main_pipe, p->det_main_new);
        if (!ret)
            ret = det_set(d, p->pipe, p->det_odm_new);
    }
    if (ret)
        return ret;
    /* dccg2_update_dpp_dto: the main pipe's DPP clock (phase / modulo first) */
    WI(d, regDPPCLK0_DTO_PARAM, p->pipe, DPPDTO_S, RI(d, regDPPCLK0_DTO_PARAM, p->main_pipe, DPPDTO_S));
    ctl = RI(d, regDPPCLK_DTO_CTRL, 0, 0);
    k = (ctl >> (4 * p->main_pipe)) & 3u;
    WI(d, regDPPCLK_DTO_CTRL, 0, 0, (ctl & ~(3u << (4 * p->pipe))) | (k << (4 * p->pipe)));
    /* the plane: DPP, HUBP (blanked, on the master's VTG), surface */
    COPY(d, dpp, DPP, p->main_pipe, p->pipe);
    COPY(d, hubp, HUBP, p->main_pipe, p->pipe);
    WI(d, regHUBP0_DCHUBP_CNTL, p->pipe, HUBP_S,
       (RI(d, regHUBP0_DCHUBP_CNTL, p->main_pipe, HUBP_S) & keep_hubp) | HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK |
       (p->otg << HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT));
    WI(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, p->pipe, HUBP_S, RI(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, p->main_pipe, HUBP_S));
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, p->pipe, HUBP_S,
       RI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, p->main_pipe, HUBP_S));
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, p->pipe, HUBP_S,
       RI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, p->main_pipe, HUBP_S));
    /* the DLG: dml32_rq_dlg_get_dlg_reg_v2 for ODM pipe index 1 */
    v = RI(d, regHUBPREQ0_DST_DIMENSIONS, p->main_pipe, HUBP_S);
    WI(d, regHUBPREQ0_DST_DIMENSIONS, p->pipe, HUBP_S, v);
    b0 = RI(d, regHUBPREQ0_BLANK_OFFSET_0, p->main_pipe, HUBP_S);
    WI(d, regHUBPREQ0_BLANK_OFFSET_0, p->pipe, HUBP_S,
       SET(b0, HUBPREQ0_BLANK_OFFSET_0, REFCYC_H_BLANK_END,
           (uint32_t)((ref_to_pix * (h_blank_end + h_active / 2)) >> 19)));
    /* resource_build_scaling_params per ODM slice (an unscaled plane over
     * the active area): viewport x = the slice offset, recout / MPC size
     * the slice; the main pipe keeps x 0 with the first slice */
    WI(d, regHUBP0_DCSURF_PRI_VIEWPORT_START, p->main_pipe, HUBP_S, 0);
    WI(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, p->main_pipe, HUBP_S, w0 | v_active << 16);
    WI(d, regHUBP0_DCSURF_PRI_VIEWPORT_START, p->pipe, HUBP_S,
       FIELD(HUBP0_DCSURF_PRI_VIEWPORT_START, PRI_VIEWPORT_X_START, w0));
    WI(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, p->pipe, HUBP_S, w1 | v_active << 16);
    WI(d, regDSCL0_RECOUT_START, p->main_pipe, DPP_S, 0);
    WI(d, regDSCL0_RECOUT_SIZE, p->main_pipe, DPP_S, w0 | v_active << DSCL0_RECOUT_SIZE__RECOUT_HEIGHT__SHIFT);
    WI(d, regDSCL0_MPC_SIZE, p->main_pipe, DPP_S, w0 | v_active << DSCL0_MPC_SIZE__MPC_HEIGHT__SHIFT);
    WI(d, regDSCL0_RECOUT_START, p->pipe, DPP_S, 0);
    WI(d, regDSCL0_RECOUT_SIZE, p->pipe, DPP_S, w1 | v_active << DSCL0_RECOUT_SIZE__RECOUT_HEIGHT__SHIFT);
    WI(d, regDSCL0_MPC_SIZE, p->pipe, DPP_S, w1 | v_active << DSCL0_MPC_SIZE__MPC_HEIGHT__SHIFT);
    /* the OPP (FMT, OPP_PIPE_CONTROL) as the main one, OPPBUF the slice */
    COPY(d, opp, OPP, p->main_opp, p->opp);
    v = RI(d, regOPPBUF0_OPPBUF_CONTROL, p->main_opp, OPP_S);
    if (GET(v, OPPBUF0_OPPBUF_CONTROL, OPPBUF_ACTIVE_WIDTH)) {
        WI(d, regOPPBUF0_OPPBUF_CONTROL, p->main_opp, OPP_S, SET(v, OPPBUF0_OPPBUF_CONTROL, OPPBUF_ACTIVE_WIDTH, w0));
        WI(d, regOPPBUF0_OPPBUF_CONTROL, p->opp, OPP_S, SET(v, OPPBUF0_OPPBUF_CONTROL, OPPBUF_ACTIVE_WIDTH, w1));
    }
    /* mpc1_insert_plane: the ODM pipe's MPCC alone in its OPP's tree */
    COPY(d, mpcc, MPCC, p->main_pipe, p->mpcc);
    WI(d, regMPCC0_MPCC_TOP_SEL, p->mpcc, MPCC_S, p->pipe);
    WI(d, regMPCC0_MPCC_BOT_SEL, p->mpcc, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_OPP_ID, p->mpcc, MPCC_S, p->opp);
    v = RI(d, regMPC_OUT0_MUX, p->opp, MUX_S);
    WI(d, regMPC_OUT0_MUX, p->opp, MUX_S, SET(v, MPC_OUT0_MUX, MPC_OUT_MUX, p->mpcc));
    RLOG(d, "odm: pipe %u + %u on OTG %u: slices %u + %u of %ux%u, refcyc_h_blank_end 0x%x / 0x%x", p->main_pipe,
         p->pipe, p->otg, w0, w1, h_active, v_active, GET(b0, HUBPREQ0_BLANK_OFFSET_0, REFCYC_H_BLANK_END),
         GET(RI(d, regHUBPREQ0_BLANK_OFFSET_0, p->pipe, HUBP_S), HUBPREQ0_BLANK_OFFSET_0, REFCYC_H_BLANK_END));
    return RDNA3_OK;
}

void dcn32_odm_pipe_blank(rdna3_dev *d, const rdna3_odm_pipe *p, int blank)
{
    uint32_t v = RI(d, regHUBP0_DCHUBP_CNTL, p->pipe, HUBP_S);
    if (!blank)
        v = SET(v, HUBP0_DCHUBP_CNTL, HUBP_UNDERFLOW_CLEAR, 1);
    WI(d, regHUBP0_DCHUBP_CNTL, p->pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, blank ? 1 : 0));
}

void dcn32_odm_pipe_disable(rdna3_dev *d, const rdna3_odm_pipe *p)
{
    uint32_t v;
    dcn32_odm_pipe_blank(d, p, 1);
    /* mpc2_remove_mpcc, the OPP clock off */
    v = RI(d, regMPC_OUT0_MUX, p->opp, MUX_S);
    WI(d, regMPC_OUT0_MUX, p->opp, MUX_S, SET(v, MPC_OUT0_MUX, MPC_OUT_MUX, MPCC_NONE));
    WI(d, regMPCC0_MPCC_TOP_SEL, p->mpcc, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_BOT_SEL, p->mpcc, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_OPP_ID, p->mpcc, MPCC_S, MPCC_NONE);
    dcn32_opp_pipe_clock_control(d, p->opp, 0);
    if (p->det_main_new != p->det_main &&
        GET(RI(d, regDCHUBBUB_DET0_CTRL, p->pipe, DET_S), DCHUBBUB_DET0_CTRL, DET0_SIZE) != p->det_odm) {
        det_set(d, p->pipe, p->det_odm);
        det_set(d, p->main_pipe, p->det_main);
    }
    /* dcn20_plane_atomic_disable: HUBP and DPP clocks, the DPP DTO, the gate */
    v = RI(d, regHUBP0_HUBP_CLK_CNTL, p->pipe, HUBP_S);
    WI(d, regHUBP0_HUBP_CLK_CNTL, p->pipe, HUBP_S, SET(v, HUBP0_HUBP_CLK_CNTL, HUBP_CLOCK_ENABLE, 0));
    v = RI(d, regDPP_TOP0_DPP_CONTROL, p->pipe, DPP_S);
    WI(d, regDPP_TOP0_DPP_CONTROL, p->pipe, DPP_S, SET(v, DPP_TOP0_DPP_CONTROL, DPP_CLOCK_ENABLE, 0));
    v = RI(d, regDPPCLK_DTO_CTRL, 0, 0);
    WI(d, regDPPCLK_DTO_CTRL, 0, 0, v & ~(3u << (4 * p->pipe)));
    pipe_power(d, p->pipe, 0);
    RLOG(d, "odm: pipe %u off (OTG %u back to pipe %u alone)", p->pipe, p->otg, p->main_pipe);
}
