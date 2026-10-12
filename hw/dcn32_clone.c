/*
 * dcn32_clone.c - see dcn32_clone.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_clone.h"
#include "smu13.h"
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
#define VTG_S   (regVTG1_CONTROL - regVTG0_CONTROL)
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
    c->arb_dram_state = rd(d, 2, RD_DCHUBBUB_ARB_DRAM_STATE_CNTL);
    c->arb_usr_retraining = rd(d, 2, RD_DCHUBBUB_ARB_USR_RETRAINING_CNTL);
    c->pstate_force = 0;
    c->smu_probe = 0;
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
    /* B5 subset: the clocks are shared, the GOP sized them for its head */
    dcn32_clk_read(d, &c->clk);
    RLOG(d, "clone: DCN clocks VCO %u DISPCLK %u DPPCLK %u DPREFCLK %u DCFCLK %u DTBCLK %u kHz",
         c->clk.vco_khz, c->clk.dispclk_khz, c->clk.dppclk_khz, c->clk.dprefclk_khz,
         c->clk.dcfclk_khz, c->clk.dtbclk_khz);
    if (dcn32_clk_fits(&c->clk, pclk_khz)) {
        RERR(d, "clone: DISPCLK/DPPCLK below the %u kHz pixel rate", pclk_khz);
        return RDNA3_EBUSY;
    }
    c->displays = dc->nheads + 1;
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

void dcn32_hubbub_log(rdna3_dev *d, const char *when)
{
    uint32_t ds = rd(d, 2, RD_DCHUBBUB_ARB_DRAM_STATE_CNTL);
    uint32_t usr = rd(d, 2, RD_DCHUBBUB_ARB_USR_RETRAINING_CNTL);
    RLOG(d, "DCHUBBUB %s: DRAM_STATE_CNTL 0x%08x USR_RETRAINING_CNTL 0x%08x (retraining %s, %s)", when, ds,
         usr, (usr & RD_ARB_USR_RETRAINING_REQUEST) ? "REQUESTED" : "not requested",
         (usr & RD_ARB_ALLOW_USR_RETRAINING) ? "allowed" : "not allowed");
}

/* is the PMFW still answering its driver mailbox? */
static int pmfw_alive(rdna3_dev *d, const rdna3_dcn_clone *c, const char *after)
{
    uint32_t v = 0;
    int r;
    if (!c->smu_probe)
        return RDNA3_OK;
    r = smu13_send_msg(d, PPSMC_MSG_TestMessage, 0x5a5a, &v);
    if (!r && v != 0x5a5b)
        r = RDNA3_EIO;
    if (r) {
        RERR(d, "clone: the PMFW stopped answering after %s (%d): stopping here", after, r);
        return RDNA3_EIO;
    }
    RLOG(d, "clone: PMFW answers after %s", after);
    return RDNA3_OK;
}

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
    if (!ret)
        ret = pmfw_alive(d, c, "SetPixelClock");
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
    /* optc1_program_timing: the VTG's FP2 / VCOUNT_INIT; optc32_enable_crtc:
     * "VTG enable first is for HW workaround", then the master enable */
    v = RI(d, regVTG0_CONTROL, c->src_otg, VTG_S);
    WI(d, regVTG0_CONTROL, c->dst_otg, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 0));
    WI(d, regVTG0_CONTROL, c->dst_otg, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 1));
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

    /* 4. stream encoder (DIG front end): connected to the new OTG
     * (enc1_dig_connect_to_otg), then STREAM_SETUP, then - as
     * enc3_stream_encoder_hdmi_set_stream_attribute after its VBIOS call -
     * the source's HDMI registers, without infoframes or audio */
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
        if (!ret)
            ret = pmfw_alive(d, c, "DIGxEncoderControl STREAM_SETUP");
        if (ret) {
            RERR(d, "clone: DIGxEncoderControl STREAM_SETUP through the DMCUB failed (%d)", ret);
            goto fail;
        }
    }
    COPY(d, digfe, DIGFE, (uint32_t)c->src_fe, c->dst_fe);
    WI(d, regDIG0_HDMI_INFOFRAME_CONTROL0, c->dst_fe, DIG_S, 0);
    WI(d, regDIG0_HDMI_INFOFRAME_CONTROL1, c->dst_fe, DIG_S, 0);
    WI(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL0, c->dst_fe, DIG_S, 0);

    /* 5. link encoder: TMDS setup of the source's back end, DIG mode,
     * front end select, then the transmitter */
    for (i = 0; i < sizeof(tmds_be) / sizeof(tmds_be[0]); i++)
        wr(d, regDIG0_TMDS_CNTL_BASE_IDX, tmds_be[i] + DIG_S * c->dst_be,
           rd(d, regDIG0_TMDS_CNTL_BASE_IDX, tmds_be[i] + DIG_S * (uint32_t)c->src_be));
    v = RI(d, regDIG0_DIG_BE_CNTL, c->dst_be, DIG_S);
    v = SET(v, DIG0_DIG_BE_CNTL, DIG_MODE, c->dig_mode);
        /* dcn10_link_encoder_connect_dig_be_to_fe (one front end per back end) */
    v = SET(v, DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT, 1u << c->dst_fe);
    WI(d, regDIG0_DIG_BE_CNTL, c->dst_be, DIG_S, v);
    ret = vbios_transmitter(dm, c, RD_ATOM_TX_INIT);
    if (!ret)
        ret = pmfw_alive(d, c, "TransmitterControl INIT");
    if (!ret)
        ret = vbios_transmitter(dm, c, RD_ATOM_TX_ENABLE);
    if (!ret)
        ret = pmfw_alive(d, c, "TransmitterControl ENABLE");
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
    /* two heads that are not synchronised: the arbiter never allows a
     * p-state change by itself; amdgpu forces it then (UCLK stays pinned) */
    dcn32_hubbub_log(d, "with the clone");
    if (c->pstate_force & 1) {
        v = rd(d, 2, RD_DCHUBBUB_ARB_DRAM_STATE_CNTL);
        wr(d, 2, RD_DCHUBBUB_ARB_DRAM_STATE_CNTL, v | RD_ARB_ALLOW_PSTATE_FORCE_VALUE | RD_ARB_ALLOW_PSTATE_FORCE_ENABLE);
    }
    if (c->pstate_force & 2) {
        v = rd(d, 2, RD_DCHUBBUB_ARB_USR_RETRAINING_CNTL);
        wr(d, 2, RD_DCHUBBUB_ARB_USR_RETRAINING_CNTL, v | RD_ARB_USR_FORCE_VALUE | RD_ARB_USR_FORCE_ENABLE);
    }
    if (c->pstate_force)
        dcn32_hubbub_log(d, "forced");
    /* the PMFW may react a little later */
    if (c->smu_probe) {
        d->os.msleep(d->os.ctx, 200);
        ret = pmfw_alive(d, c, "the clone lit (+200 ms)");
        if (ret)
            goto fail;
    }
    /* No dcn30_smu_set_num_of_displays here: on the RX 7800 XT (PMFW
     * 80.92) the driver mailbox stopped answering right after NumOfDisplays
     * 2 went through the DAL mailbox (metrics "msg 0x12 timed out", then
     * never ready again), and the host could not take the card back. */
    return RDNA3_OK;

fail:
    dcn32_clone_disable(d, dm, c);
    return ret;
}

static void crc_read(rdna3_dev *d, uint32_t otg, uint32_t crc[3])
{
    uint32_t rg = RI(d, regOTG0_OTG_CRC0_DATA_RG, otg, OTG_S), b = RI(d, regOTG0_OTG_CRC0_DATA_B, otg, OTG_S);
    crc[0] = GET(rg, OTG0_OTG_CRC0_DATA_RG, CRC0_R_CR);
    crc[1] = GET(rg, OTG0_OTG_CRC0_DATA_RG, CRC0_G_Y);
    crc[2] = GET(b, OTG0_OTG_CRC0_DATA_B, CRC0_B_CB);
}

int dcn32_clone_verify(rdna3_dev *d, const rdna3_dcn_clone *c, uint32_t wait_ms, rdna3_clone_check *k)
{
    const uint32_t fmask = OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;
    uint32_t saved_src = RI(d, regOTG0_OTG_CRC_CNTL, c->src_otg, OTG_S);
    uint32_t saved_dst = RI(d, regOTG0_OTG_CRC_CNTL, c->dst_otg, OTG_S);
    uint32_t f0s = RI(d, regOTG0_OTG_STATUS_FRAME_COUNT, c->src_otg, OTG_S) & fmask;
    uint32_t f0d = RI(d, regOTG0_OTG_STATUS_FRAME_COUNT, c->dst_otg, OTG_S) & fmask;
    uint32_t i;
    /* optc1_configure_crc as dc_stream_configure_crc: windows over the
     * active area, continuous */
    dcn32_otg_crc_config(d, c->src_otg, 1);
    dcn32_otg_crc_config(d, c->dst_otg, 1);
    d->os.msleep(d->os.ctx, wait_ms);
    k->frames_src = ((RI(d, regOTG0_OTG_STATUS_FRAME_COUNT, c->src_otg, OTG_S) & fmask) - f0s) & fmask;
    k->frames_dst = ((RI(d, regOTG0_OTG_STATUS_FRAME_COUNT, c->dst_otg, OTG_S) & fmask) - f0d) & fmask;
    crc_read(d, c->src_otg, k->crc_src);
    crc_read(d, c->dst_otg, k->crc_dst);
    WI(d, regOTG0_OTG_CRC_CNTL, c->src_otg, OTG_S, saved_src);
    WI(d, regOTG0_OTG_CRC_CNTL, c->dst_otg, OTG_S, saved_dst);
    k->running = k->frames_dst > 0;
    k->crc_match = 1;
    k->crc_valid = 0;
    for (i = 0; i < 3; i++) {
        k->crc_match &= k->crc_src[i] == k->crc_dst[i];
        k->crc_valid |= k->crc_src[i] || k->crc_dst[i];
    }
    /* all zero on the card (RX 7800 XT, first run): the CRC block did not
     * produce anything, which proves nothing about the picture */
    if (!k->crc_valid)
        k->crc_match = 0;
    RLOG(d, "clone: check over %u ms: frames boot %u clone %u, CRC boot %04x/%04x/%04x clone "
         "%04x/%04x/%04x (%s)", wait_ms, k->frames_src, k->frames_dst, k->crc_src[0], k->crc_src[1],
         k->crc_src[2], k->crc_dst[0], k->crc_dst[1], k->crc_dst[2],
         !k->running ? "the clone OTG does not run" : !k->crc_valid ? "no CRC: picture not compared" :
         k->crc_match ? "same picture" : "different picture");
    return k->running && (k->crc_match || !k->crc_valid) ? RDNA3_OK : RDNA3_EIO;
}

int dcn32_clone_link(rdna3_dev *d, rdna3_dmub *dm, const rdna3_dcn_clone *c, int on)
{
    uint32_t v, t;
    int ret;
    if (!dm || !dm->hw_init)
        return RDNA3_ENODEV;
    v = RI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S);
    if (!on) {
        /* hubp2_set_blank_regs: wait for the outstanding requests first */
        for (t = 0; t < 100000 && !GET(RI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S), HUBP0_DCHUBP_CNTL,
                                        HUBP_NO_OUTSTANDING_REQ); t++)
            d->os.udelay(d->os.ctx, 1);
        v = RI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S);
        WI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 1));
        ret = vbios_transmitter(dm, c, RD_ATOM_TX_DISABLE);
    } else {
        ret = vbios_transmitter(dm, c, RD_ATOM_TX_ENABLE);
        v = RI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S);
        WI(d, regHUBP0_DCHUBP_CNTL, c->dst_pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 0));
    }
    RLOG(d, "clone: link %s on UNIPHY%c (%d)", on ? "up" : "down (monitor standby)", 'A' + c->dst_be, ret);
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
    /* optc32_disable_crtc: the VTG goes with the OTG */
    v = RI(d, regVTG0_CONTROL, c->dst_otg, VTG_S);
    WI(d, regVTG0_CONTROL, c->dst_otg, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 0));
    /* dce110_clock_source_power_down sends nothing on DCN 3.2: it asks for
     * controller CONTROLLER_ID_UNDEFINED, which controller_id_to_atom2
     * refuses, so set_pixel_clock_v7 returns before the DMCUB; the PHY PLL
     * goes with the transmitter's DISABLE. SetPixelClock with 0 for the
     * CRTC just disabled hung the DMCUB on the card (2026-10-12: every
     * later command timed out, the clone could not be lit again) */
    v = RI(d, regMPC_OUT0_MUX, c->dst_opp, MUX_S);
    WI(d, regMPC_OUT0_MUX, c->dst_opp, MUX_S, SET(v, MPC_OUT0_MUX, MPC_OUT_MUX, MPCC_NONE));
    WI(d, regMPCC0_MPCC_TOP_SEL, c->dst_mpcc, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_BOT_SEL, c->dst_mpcc, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_OPP_ID, c->dst_mpcc, MPCC_S, MPCC_NONE);
    if (c->det_dst_new != c->det_dst &&
        GET(RI(d, regDCHUBBUB_DET0_CTRL, c->dst_pipe, DET_S), DCHUBBUB_DET0_CTRL, DET0_SIZE) != c->det_dst)
        det_program(d, c->dst_pipe, c->det_dst, c->src_pipe, c->det_src);
    /* dcn20_plane_atomic_disable: HUBP and DPP clocks off, the DPP DTO
     * disabled, then the power gate */
    v = RI(d, regHUBP0_HUBP_CLK_CNTL, c->dst_pipe, HUBP_S);
    WI(d, regHUBP0_HUBP_CLK_CNTL, c->dst_pipe, HUBP_S, SET(v, HUBP0_HUBP_CLK_CNTL, HUBP_CLOCK_ENABLE, 0));
    v = RI(d, regDPP_TOP0_DPP_CONTROL, c->dst_pipe, DPP_S);
    WI(d, regDPP_TOP0_DPP_CONTROL, c->dst_pipe, DPP_S, SET(v, DPP_TOP0_DPP_CONTROL, DPP_CLOCK_ENABLE, 0));
    v = RI(d, regDPPCLK_DTO_CTRL, 0, 0);
    WI(d, regDPPCLK_DTO_CTRL, 0, 0, v & ~(3u << (4 * c->dst_pipe)));
    pipe_power(d, c->dst_pipe, 0);
    /* one head again: the arbiter's own p-state / retraining decisions */
    if (c->pstate_force) {
        wr(d, 2, RD_DCHUBBUB_ARB_DRAM_STATE_CNTL, c->arb_dram_state);
        wr(d, 2, RD_DCHUBBUB_ARB_USR_RETRAINING_CNTL, c->arb_usr_retraining);
    }
    RLOG(d, "clone: pipe %u off (transmitter %u: %d)", c->dst_pipe, c->dst_be, ret);
    return ret;
}
