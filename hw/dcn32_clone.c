/*
 * dcn32_clone.c - see dcn32_clone.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_clone.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_dig.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_clone.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))

/* instance n of a register: offset of instance 0 plus n strides (paste
 * reg##_BASE_IDX here, before 'reg' expands to its number) */
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
#define DIG_S   RD_CLONE_DIGFE_STRIDE
#define DET_S   (regDCHUBBUB_DET1_CTRL - regDCHUBBUB_DET0_CTRL)
#define MUX_S   (regMPC_OUT1_MUX - regMPC_OUT0_MUX)
#define PG_S    (regDOMAIN1_PG_CONFIG - regDOMAIN0_PG_CONFIG)
#define PRC_S   (regOTG1_PIXEL_RATE_CNTL - regOTG0_PIXEL_RATE_CNTL)    /* DCCG per OTG */
#define DPPDTO_S (regDPPCLK1_DTO_PARAM - regDPPCLK0_DTO_PARAM)
#define NDIG    5          /* DIGA..DIGE / UNIPHY A..E */
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

/* the DIG front end an enabled back end sources, or -1 */
static int be_source(rdna3_dev *d, uint32_t be)
{
    uint32_t en = RI(d, regDIG0_DIG_BE_EN_CNTL, be, DIG_S), src, f;
    if (en == 0xffffffffu || !GET(en, DIG0_DIG_BE_EN_CNTL, DIG_ENABLE))
        return -1;
    src = GET(RI(d, regDIG0_DIG_BE_CNTL, be, DIG_S), DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT);
    for (f = 0; f < NDIG; f++)
        if (src & (1u << f))
            return (int)f;
    return -1;
}

static int fe_free(rdna3_dev *d, uint32_t fe, int src_fe)
{
    uint32_t be;
    if ((int)fe == src_fe)
        return 0;
    for (be = 0; be < NDIG; be++)
        if (be_source(d, be) == (int)fe)
            return 0;
    return 1;
}

int dcn32_clone_plan(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head,
                     const rdna3_display_path *dst, uint32_t pclk_khz, rdna3_dcn_clone *c)
{
    const rdna3_dcn_head *h;
    uint32_t sel, p, o, i, f;
    int found;

    if (!dc || !c || !dst || head >= dc->nheads || !pclk_khz)
        return RDNA3_EINVAL;
    h = &dc->head[head];
    {
        rdna3_dcn_clone z = { 0 };
        *c = z;
    }
    /* TMDS sources only: DP would need the link trained on the new PHY */
    if (h->dig_fe < 0 || h->dig_be < 0 || (h->dig_mode != 2 && h->dig_mode != 3)) {
        RERR(d, "clone: head %u is not a TMDS stream (DIG FE %d BE %d mode %u)", head, h->dig_fe,
             h->dig_be, h->dig_mode);
        return RDNA3_EINVAL;
    }
    if (dst->transmitter < 0 || dst->transmitter >= NDIG) {
        RERR(d, "clone: connector without a usable transmitter (%d)", dst->transmitter);
        return RDNA3_EINVAL;
    }
    c->src_pipe = h->pipe;
    c->src_otg = h->otg;
    c->src_fe = h->dig_fe;
    c->src_be = h->dig_be;
    c->dig_mode = h->dig_mode;
    c->pclk_khz = pclk_khz;

    /* source tree: OTG <- OPP (OPTC_SEG0_SRC_SEL) <- MPC_OUT mux <- MPCC */
    sel = RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, c->src_otg, ODM_S);
    if (GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_NUM_OF_INPUT_SEGMENT) != 0) {
        RERR(d, "clone: source OTG %u uses ODM combine (0x%x)", c->src_otg, sel);
        return RDNA3_EINVAL;
    }
    c->src_opp = GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL);
    c->src_mpcc = GET(RI(d, regMPC_OUT0_MUX, c->src_opp, MUX_S), MPC_OUT0_MUX, MPC_OUT_MUX);
    if (c->src_opp >= RD_DCN_MAX_PIPES || c->src_mpcc >= RD_DCN_MAX_PIPES) {
        RERR(d, "clone: source OPP %u / MPCC %u unknown", c->src_opp, c->src_mpcc);
        return RDNA3_EINVAL;
    }

    /* destination pipe: not lit, its MPCC in no tree (HUBP registers read
     * nothing useful while its power domain is gated) */
    found = 0;
    for (p = 0; p < RD_DCN_MAX_PIPES && !found; p++) {
        int used = 0;
        for (i = 0; i < dc->nheads; i++)
            used |= dc->head[i].pipe == p;
        if (used || GET(RI(d, regMPCC0_MPCC_TOP_SEL, p, MPCC_S), MPCC0_MPCC_TOP_SEL, MPCC_TOP_SEL) != MPCC_NONE)
            continue;
        c->dst_pipe = p;
        found = 1;
    }
    if (!found) {
        RERR(d, "clone: no free pipe");
        return RDNA3_EBUSY;
    }
    /* destination OTG: not running, not a lit head's, and no enabled DIG
     * back end's front end sources it (the GOP can leave one behind) */
    found = 0;
    for (o = 0; o < RD_DCN_MAX_PIPES && !found; o++) {
        int used = 0;
        for (i = 0; i < dc->nheads; i++)
            used |= dc->head[i].otg == o;
        for (i = 0; i < NDIG; i++) {
            int fe = be_source(d, i);
            if (fe >= 0 && GET(RI(d, regDIG0_DIG_FE_CNTL, (uint32_t)fe, DIG_S), DIG0_DIG_FE_CNTL,
                               DIG_SOURCE_SELECT) == o)
                used = 1;
        }
        if (used || o == c->src_opp ||
            GET(RI(d, regOTG0_OTG_CONTROL, o, OTG_S), OTG0_OTG_CONTROL, OTG_MASTER_EN))
            continue;
        c->dst_otg = o;
        found = 1;
    }
    if (!found) {
        RERR(d, "clone: no free OTG");
        return RDNA3_EBUSY;
    }
    c->dst_opp = c->dst_otg;     /* 1:1, as optc32_enable_crtc assumes */
    c->dst_mpcc = c->dst_pipe;   /* MPCC n belongs to DPP n */

    c->dst_be = (uint32_t)dst->transmitter;
    if (be_source(d, c->dst_be) >= 0) {
        RERR(d, "clone: transmitter %u is lit already", c->dst_be);
        return RDNA3_EBUSY;
    }
    /* stream encoder: the transmitter's own DIG (link encoder's
     * preferred_engine) when free, else the first free one */
    found = 0;
    if (fe_free(d, c->dst_be, c->src_fe)) {
        c->dst_fe = c->dst_be;
        found = 1;
    }
    for (f = 0; f < NDIG && !found; f++)
        if (fe_free(d, f, c->src_fe)) {
            c->dst_fe = f;
            found = 1;
        }
    if (!found) {
        RERR(d, "clone: no free DIG front end");
        return RDNA3_EBUSY;
    }
    c->pll = c->dst_be;          /* each combo PHY has its PLL */
    c->hpd = dst->hpd;
    c->encoder_id = (uint8_t)dst->encoder_objid;
    c->connector_id = (uint8_t)dst->connector_objid;

    /* DET: the GOP gave its pipe the segments; split them when the new
     * pipe has none (dcn32_program_det_size, 64 KiB segments) */
    c->det_src = GET(RI(d, regDCHUBBUB_DET0_CTRL, c->src_pipe, DET_S), DCHUBBUB_DET0_CTRL, DET0_SIZE);
    c->det_dst = GET(RI(d, regDCHUBBUB_DET0_CTRL, c->dst_pipe, DET_S), DCHUBBUB_DET0_CTRL, DET0_SIZE);
    c->det_src_new = c->det_src;
    c->det_dst_new = c->det_dst;
    if (c->det_dst < 2) {
        if (c->det_src < 4) {
            RERR(d, "clone: source pipe has %u DET segments, nothing to split", c->det_src);
            return RDNA3_EBUSY;
        }
        c->det_dst_new = c->det_src / 2;
        c->det_src_new = c->det_src - c->det_dst_new;
    }
    RLOG(d, "clone: head %u (pipe %u otg %u opp %u mpcc %u DIG FE %d BE %d mode %u) -> pipe %u "
         "otg %u opp %u DIG FE %u BE %u PLL %u HPD %d enc 0x%x conn 0x%x, %u kHz, DET %u/%u -> %u/%u",
         head, c->src_pipe, c->src_otg, c->src_opp, c->src_mpcc, c->src_fe, c->src_be, c->dig_mode,
         c->dst_pipe, c->dst_otg, c->dst_opp, c->dst_fe, c->dst_be, c->pll, c->hpd, c->encoder_id,
         c->connector_id, c->pclk_khz, c->det_src, c->det_dst, c->det_src_new, c->det_dst_new);
    return RDNA3_OK;
}

/* dcn32_hubp_pg_control under DC_IP_REQUEST_CNTL (dcn10_power_on_plane_resources);
 * DPP shares the HUBP's domain on DCN 3.2 */
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
        RERR(d, "clone: pipe %u power %s timed out (PG_STATUS 0x%x)", pipe, on ? "on" : "off",
             RI(d, regDOMAIN0_PG_STATUS, pipe, PG_S));
    return ret;
}

static int det_set(rdna3_dev *d, uint32_t pipe, uint32_t segs)
{
    uint32_t v = RI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S);
    int ret;
    WI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S, SET(v, DCHUBBUB_DET0_CTRL, DET0_SIZE, segs));
    ret = rdna3_wait_reg(d, AI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S),
                         segs << DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT__SHIFT,
                         DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT_MASK, 0, 1000);
    if (ret)
        RERR(d, "clone: DET%u -> %u segments timed out (0x%x, COMPBUF 0x%x)", pipe, segs,
             RI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S), RI(d, regDCHUBBUB_COMPBUF_CTRL, 0, 0));
    return ret;
}

/* shrink before grow, as dcn32_program_det_size callers order it */
static int det_program(rdna3_dev *d, uint32_t shrink_pipe, uint32_t shrink, uint32_t grow_pipe,
                       uint32_t grow)
{
    int ret = det_set(d, shrink_pipe, shrink);
    if (!ret)
        ret = det_set(d, grow_pipe, grow);
    return ret;
}

/* DCCG: the OTG pixel rate source, DP DTO, the K1/K2 divider and the DPP
 * clock DTO, as the source pipe has them */
static void dccg_copy(rdna3_dev *d, const rdna3_dcn_clone *c)
{
    const uint32_t cfg = OTG0_PIXEL_RATE_CNTL__OTG0_PIXEL_RATE_SOURCE_MASK |
                         OTG0_PIXEL_RATE_CNTL__DTBCLK_DTO0_ENABLE_MASK |
                         OTG0_PIXEL_RATE_CNTL__DP_DTO0_ENABLE_MASK |
                         OTG0_PIXEL_RATE_CNTL__DP_DTO0_DS_DISABLE_MASK |
                         OTG0_PIXEL_RATE_CNTL__PIPE0_DTO_SRC_SEL_MASK;
    uint32_t v, div, k, ctl, dto;

    WI(d, regDP_DTO0_PHASE, c->dst_otg, PRC_S, RI(d, regDP_DTO0_PHASE, c->src_otg, PRC_S));
    WI(d, regDP_DTO0_MODULO, c->dst_otg, PRC_S, RI(d, regDP_DTO0_MODULO, c->src_otg, PRC_S));
    v = RI(d, regOTG0_PIXEL_RATE_CNTL, c->dst_otg, PRC_S) & ~cfg;
    WI(d, regOTG0_PIXEL_RATE_CNTL, c->dst_otg, PRC_S, v | (RI(d, regOTG0_PIXEL_RATE_CNTL, c->src_otg, PRC_S) & cfg));
    /* dccg32_set_pixel_rate_div: 3 bits (K1, K2) per OTG */
    div = RI(d, regOTG_PIXEL_RATE_DIV, 0, 0);
    k = (div >> (3 * c->src_otg)) & 7u;
    WI(d, regOTG_PIXEL_RATE_DIV, 0, 0, (div & ~(7u << (3 * c->dst_otg))) | (k << (3 * c->dst_otg)));
    /* dccg2_update_dpp_dto: phase/modulo first, then the enable */
    dto = RI(d, regDPPCLK0_DTO_PARAM, c->src_pipe, DPPDTO_S);
    WI(d, regDPPCLK0_DTO_PARAM, c->dst_pipe, DPPDTO_S, dto);
    ctl = RI(d, regDPPCLK_DTO_CTRL, 0, 0);
    k = (ctl >> (4 * c->src_pipe)) & 3u;
    WI(d, regDPPCLK_DTO_CTRL, 0, 0, (ctl & ~(3u << (4 * c->dst_pipe))) | (k << (4 * c->dst_pipe)));
}

static int vbios_pixel_clock(rdna3_dmub *dm, const rdna3_dcn_clone *c, int enable)
{
    /* set_pixel_clock_v7 as dcn31_program_pix_clk fills it for TMDS:
     * PHY PLL of the transmitter, crtc = OTG, 8 bpc, XTALIN reference */
    rdna3_pixclk_v1_7 p = { 0 };
    p.pixclk_100hz = enable ? c->pclk_khz * 10 : 0;
    p.pll_id = (uint8_t)(RD_ATOM_COMBOPHY_PLL0 + c->pll);
    p.encoderobjid = c->encoder_id;
    p.encoder_mode = c->dig_mode == 2 ? RD_ATOM_ENCODER_MODE_DVI : RD_ATOM_ENCODER_MODE_HDMI;
    p.crtc_id = (uint8_t)c->dst_otg;
    return dmub32_set_pixel_clock(dm, &p);
}

static int vbios_transmitter(rdna3_dmub *dm, const rdna3_dcn_clone *c, uint8_t action)
{
    /* transmitter_control_v1_7 as dcn10_link_encoder_enable_tmds_output /
     * dcn10_link_encoder_hw_init (INIT) fill it */
    rdna3_dig_tx_v1_7 t = { 0 };
    t.phyid = (uint8_t)c->dst_be;
    t.action = action;
    t.lanenum = 4;
    t.hpdsel = (uint8_t)(c->hpd >= 0 ? c->hpd + 1 : 0);
    t.connobj_id = c->connector_id;
    if (action != RD_ATOM_TX_INIT) {
        t.digmode = (uint8_t)c->dig_mode;   /* ATOM_TRANSMITTER_DIGMODE_V6_DVI 2, _HDMI 3 */
        t.digfe_sel = (uint8_t)(1u << c->dst_fe);
        t.symclk_10khz = c->pclk_khz / 10;
    }
    return dmub32_transmitter_control(dm, &t);
}

static const uint16_t tmds_be[] = {
    regDIG0_TMDS_CNTL, regDIG0_TMDS_CONTROL_CHAR, regDIG0_TMDS_CONTROL0_FEEDBACK,
    regDIG0_TMDS_SYNC_CHAR_PATTERN_0_1, regDIG0_TMDS_SYNC_CHAR_PATTERN_2_3, regDIG0_TMDS_CTL_BITS,
    regDIG0_TMDS_DCBALANCER_CONTROL, regDIG0_TMDS_SYNC_DCBALANCE_CHAR, regDIG0_TMDS_CTL0_1_GEN_CNTL,
    regDIG0_TMDS_CTL2_3_GEN_CNTL,
};

int dcn32_clone_enable(rdna3_dev *d, rdna3_dmub *dm, const rdna3_dcn_clone *c)
{
    const uint32_t keep_hubp = HUBP0_DCHUBP_CNTL__HUBP_VREADY_AT_OR_AFTER_VSYNC_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_DISABLE_STOP_DATA_DURING_VM_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_UNBOUNDED_REQ_MODE_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_TTU_MODE_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_TIMEOUT_THRESHOLD_MASK;
    const uint32_t keep_otg = OTG0_OTG_CONTROL__OTG_START_POINT_CNTL_MASK |
                              OTG0_OTG_CONTROL__OTG_FIELD_NUMBER_CNTL_MASK |
                              OTG0_OTG_CONTROL__OTG_FIELD_NUMBER_POLARITY_MASK |
                              OTG0_OTG_CONTROL__OTG_OUT_MUX_MASK;
    uint32_t v, i, otgc;
    int ret;

    if (!dm || !dm->hw_init) {
        RERR(d, "clone: the DMCUB is not running (rdna3dmub=1): no way to drive the PHY");
        return RDNA3_ENODEV;
    }
    /* 1. pipe power and DET */
    ret = pipe_power(d, c->dst_pipe, 1);
    if (!ret && c->det_dst_new != c->det_dst)
        ret = det_program(d, c->src_pipe, c->det_src_new, c->dst_pipe, c->det_dst_new);
    if (ret)
        goto fail;

    /* 2. dcn20_enable_stream_timing: pixel clock, timing, ODM, enable */
    dccg_copy(d, c);
    ret = vbios_pixel_clock(dm, c, 1);
    if (ret) {
        RERR(d, "clone: SetPixelClock through the DMCUB failed (%d)", ret);
        goto fail;
    }
    COPY(d, otg, OTG, c->src_otg, c->dst_otg);
    otgc = RI(d, regOTG0_OTG_CONTROL, c->src_otg, OTG_S) & keep_otg;
    WI(d, regOTG0_OTG_CONTROL, c->dst_otg, OTG_S, otgc);
    COPY(d, odm, ODM, c->src_otg, c->dst_otg);
    v = RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, c->src_otg, ODM_S);
    WI(d, regODM0_OPTC_DATA_SOURCE_SELECT, c->dst_otg, ODM_S,
       SET(v, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL, c->dst_opp));
    COPY(d, opp, OPP, c->src_opp, c->dst_opp);
    /* optc32_enable_crtc */
    WI(d, regOTG0_OTG_CONTROL, c->dst_otg, OTG_S,
       SET(SET(otgc, OTG0_OTG_CONTROL, OTG_DISABLE_POINT_CNTL, 2), OTG0_OTG_CONTROL, OTG_MASTER_EN, 1));

    /* 3. the plane: DPP, HUBP (blanked), MPC tree */
    COPY(d, dpp, DPP, c->src_pipe, c->dst_pipe);
    v = RI(d, regCM0_CM_GAMCOR_CONTROL, c->src_pipe, DPP_S);
    if (GET(v, CM0_CM_GAMCOR_CONTROL, CM_GAMCOR_MODE)) {
        /* the LUT RAM itself is not copied: bypass instead of garbage */
        RLOG(d, "clone: source degamma LUT in use (0x%x), bypassed on the clone", v);
        WI(d, regCM0_CM_GAMCOR_CONTROL, c->dst_pipe, DPP_S, SET(v, CM0_CM_GAMCOR_CONTROL, CM_GAMCOR_MODE, 0));
    }
    COPY(d, hubp, HUBP, c->src_pipe, c->dst_pipe);
    WI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S,
       (RI(d, regHUBP0_DCHUBP_CNTL, c->src_pipe, HUBP_S) & keep_hubp) |
       HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK | (c->dst_otg << HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT));
    /* hubp2_program_surface_flip_and_addr: HIGH first, LOW latches */
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, c->dst_pipe, HUBP_S,
       RI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, c->src_pipe, HUBP_S));
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, c->dst_pipe, HUBP_S,
       RI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, c->src_pipe, HUBP_S));
    COPY(d, mpcc, MPCC, c->src_mpcc, c->dst_mpcc);
    /* mpc1_insert_plane: one plane, no bottom */
    WI(d, regMPCC0_MPCC_TOP_SEL, c->dst_mpcc, MPCC_S, c->dst_pipe);
    WI(d, regMPCC0_MPCC_BOT_SEL, c->dst_mpcc, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_OPP_ID, c->dst_mpcc, MPCC_S, c->dst_opp);
    v = RI(d, regMPC_OUT0_MUX, c->dst_opp, MUX_S);
    WI(d, regMPC_OUT0_MUX, c->dst_opp, MUX_S, SET(v, MPC_OUT0_MUX, MPC_OUT_MUX, c->dst_mpcc));

    /* 4. stream encoder (DIG front end): the source's HDMI setup, no
     * infoframes or audio, sourcing the new OTG; then STREAM_SETUP */
    COPY(d, digfe, DIGFE, (uint32_t)c->src_fe, c->dst_fe);
    WI(d, regDIG0_HDMI_INFOFRAME_CONTROL0, c->dst_fe, DIG_S, 0);
    WI(d, regDIG0_HDMI_INFOFRAME_CONTROL1, c->dst_fe, DIG_S, 0);
    WI(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL0, c->dst_fe, DIG_S, 0);
    v = RI(d, regDIG0_DIG_FE_CNTL, (uint32_t)c->src_fe, DIG_S);
    WI(d, regDIG0_DIG_FE_CNTL, c->dst_fe, DIG_S, SET(v, DIG0_DIG_FE_CNTL, DIG_SOURCE_SELECT, c->dst_otg));
    {
        rdna3_dig_enc_v1_5 e = { 0 };
        e.digid = (uint8_t)c->dst_fe;
        e.action = RD_ATOM_ENCODER_CMD_STREAM_SETUP;
        e.digmode = c->dig_mode == 2 ? RD_ATOM_ENCODER_MODE_DVI : RD_ATOM_ENCODER_MODE_HDMI;
        e.lanenum = 4;
        e.pclk_10khz = c->pclk_khz / 10;
        e.bitpercolor = RD_ATOM_PANEL_8BIT_PER_COLOR;
        ret = dmub32_encoder_control(dm, &e);
        if (ret) {
            RERR(d, "clone: DIGxEncoderControl STREAM_SETUP through the DMCUB failed (%d)", ret);
            goto fail;
        }
    }

    /* 5. link encoder: TMDS setup of the source's back end, DIG mode,
     * front end select, then the transmitter */
    for (i = 0; i < sizeof(tmds_be) / sizeof(tmds_be[0]); i++)
        wr(d, regDIG0_TMDS_CNTL_BASE_IDX, tmds_be[i] + DIG_S * c->dst_be,
           rd(d, regDIG0_TMDS_CNTL_BASE_IDX, tmds_be[i] + DIG_S * (uint32_t)c->src_be));
    v = RI(d, regDIG0_DIG_BE_CNTL, c->dst_be, DIG_S);
    v = SET(v, DIG0_DIG_BE_CNTL, DIG_MODE, c->dig_mode);
    v = SET(v, DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT, 1u << c->dst_fe);
    WI(d, regDIG0_DIG_BE_CNTL, c->dst_be, DIG_S, v);
    ret = vbios_transmitter(dm, c, RD_ATOM_TX_INIT);
    if (!ret)
        ret = vbios_transmitter(dm, c, RD_ATOM_TX_ENABLE);
    if (ret) {
        RERR(d, "clone: DIG1TransmitterControl through the DMCUB failed (%d)", ret);
        goto fail;
    }

    /* 6. hubp2_set_blank(false) */
    v = RI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S);
    WI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 0));
    RLOG(d, "clone: pipe %u lit: OTG_CONTROL 0x%x DIG_BE_CNTL 0x%x BE_EN 0x%x PG 0x%x DET 0x%x "
         "PIXEL_RATE 0x%x PHYPLL 0x%x", c->dst_pipe, RI(d, regOTG0_OTG_CONTROL, c->dst_otg, OTG_S),
         RI(d, regDIG0_DIG_BE_CNTL, c->dst_be, DIG_S), RI(d, regDIG0_DIG_BE_EN_CNTL, c->dst_be, DIG_S),
         RI(d, regDOMAIN0_PG_STATUS, c->dst_pipe, PG_S), RI(d, regDCHUBBUB_DET0_CTRL, c->dst_pipe, DET_S),
         RI(d, regOTG0_PIXEL_RATE_CNTL, c->dst_otg, PRC_S),
         RI(d, regOTG0_PHYPLL_PIXEL_RATE_CNTL, c->dst_otg, PRC_S));
    return RDNA3_OK;

fail:
    dcn32_clone_disable(d, dm, c);
    return ret;
}

int dcn32_clone_disable(rdna3_dev *d, rdna3_dmub *dm, const rdna3_dcn_clone *c)
{
    uint32_t v;
    int ret = RDNA3_OK;

    /* hubp2_set_blank(true), dcn10_link_encoder_disable_output,
     * optc1_disable_crtc, mpc2_remove_mpcc, then power and DET back */
    v = RI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S);
    WI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 1));
    if (dm && dm->hw_init && be_source(d, c->dst_be) >= 0)
        ret = vbios_transmitter(dm, c, RD_ATOM_TX_DISABLE);
    v = RI(d, regDIG0_DIG_BE_CNTL, c->dst_be, DIG_S);
    WI(d, regDIG0_DIG_BE_CNTL, c->dst_be, DIG_S, SET(v, DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT, 0));
    v = RI(d, regOTG0_OTG_CONTROL, c->dst_otg, OTG_S);
    v = SET(v, OTG0_OTG_CONTROL, OTG_DISABLE_POINT_CNTL, 0);
    WI(d, regOTG0_OTG_CONTROL, c->dst_otg, OTG_S, SET(v, OTG0_OTG_CONTROL, OTG_MASTER_EN, 0));
    v = RI(d, regMPC_OUT0_MUX, c->dst_opp, MUX_S);
    WI(d, regMPC_OUT0_MUX, c->dst_opp, MUX_S, SET(v, MPC_OUT0_MUX, MPC_OUT_MUX, MPCC_NONE));
    WI(d, regMPCC0_MPCC_TOP_SEL, c->dst_mpcc, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_BOT_SEL, c->dst_mpcc, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_OPP_ID, c->dst_mpcc, MPCC_S, MPCC_NONE);
    if (c->det_dst_new != c->det_dst &&
        GET(RI(d, regDCHUBBUB_DET0_CTRL, c->dst_pipe, DET_S), DCHUBBUB_DET0_CTRL, DET0_SIZE) != c->det_dst)
        det_program(d, c->dst_pipe, c->det_dst, c->src_pipe, c->det_src);
    pipe_power(d, c->dst_pipe, 0);
    RLOG(d, "clone: pipe %u off (transmitter %u: %d)", c->dst_pipe, c->dst_be, ret);
    return ret;
}
