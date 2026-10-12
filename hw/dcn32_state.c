/*
 * dcn32_state.c - see dcn32_state.h.
 *
 * Ported from (Linux v6.12, MIT): link_set_dpms_off / link_set_dpms_on
 * (TMDS: dcn10_link_encoder_disable_output, enable_link_hdmi ->
 * dcn10_link_encoder_enable_tmds_output), the dm_suspend / dm_resume replay
 * of the cached state (amdgpu_dm_commit_zero_streams, dc_commit_streams ->
 * dcn20_enable_stream_timing, optc32_disable_crtc / optc32_enable_crtc,
 * dcn32_hubp_pg_control, dcn32_program_det_size,
 * dcn31_program_pix_clk -> set_pixel_clock_v7, encoder_control_digx_v1_5,
 * dcn10_link_encoder_hw_init), hubbub3_init_dchub_sys_ctx (the system
 * aperture it programs is part of the saved global state).
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_state.h"
#include "dcn32_modeset.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_dig.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_clone.h"
#include "gen/dcn_3_2_0_mode.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))

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
#define PRC_S   (regOTG1_PIXEL_RATE_CNTL - regOTG0_PIXEL_RATE_CNTL)
#define DPPDTO_S (regDPPCLK1_DTO_PARAM - regDPPCLK0_DTO_PARAM)
#define VTG_S   (regVTG1_CONTROL - regVTG0_CONTROL)
#define OGAM_S  0x5eu      /* MPCC_OGAM1_MPCC_OGAM_CONTROL - MPCC_OGAM0_... (dcn_3_2_0_offset.h) */
#define NDIG    5
#define MPCC_NONE 0xfu

#define PGFSM_POWER_ON  0
#define PGFSM_POWER_OFF 2

/* dml32: the DCHUB reference clock of DCN 3.2 */
#define RD_DCN32_REFCLK_KHZ 100000u

static uint32_t rd(rdna3_dev *d, uint32_t seg, uint32_t off)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off));
}

static void wr(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off), v);
}

/* the DIG back end's TMDS setup (as dcn32_clone.c copies it) */
static const uint16_t st_tmds_be[] = {
    regDIG0_TMDS_CNTL, regDIG0_TMDS_CONTROL_CHAR, regDIG0_TMDS_CONTROL0_FEEDBACK,
    regDIG0_TMDS_SYNC_CHAR_PATTERN_0_1, regDIG0_TMDS_SYNC_CHAR_PATTERN_2_3, regDIG0_TMDS_CTL_BITS,
    regDIG0_TMDS_DCBALANCER_CONTROL, regDIG0_TMDS_SYNC_DCBALANCE_CHAR, regDIG0_TMDS_CTL0_1_GEN_CNTL,
    regDIG0_TMDS_CTL2_3_GEN_CNTL,
};
/* hubp2_cursor_set_attributes / hubp2_cursor_set_position (HUBP stride) */
static const uint16_t st_cursor[] = {
    regCURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH, regCURSOR0_0_CURSOR_SURFACE_ADDRESS, regCURSOR0_0_CURSOR_SIZE,
    regCURSOR0_0_CURSOR_CONTROL, regCURSOR0_0_CURSOR_POSITION, regCURSOR0_0_CURSOR_HOT_SPOT,
    regCURSOR0_0_CURSOR_DST_OFFSET,
};

/* which instance of the head a block is at */
enum { INST_PIPE, INST_MPCC, INST_OPP, INST_OTG, INST_FE, INST_BE };
#define BLK(list, seg, stride, inst) { list, sizeof(list) / sizeof(list[0]), seg, stride, inst }
static const struct {
    const uint16_t *list;
    uint32_t n, seg, stride, inst;
} kBlocks[] = {
    /* the order restore writes them in: timing first, then the plane */
    BLK(rd_clone_otg, RD_CLONE_OTG_SEG, OTG_S, INST_OTG),
    BLK(rd_clone_odm, RD_CLONE_ODM_SEG, ODM_S, INST_OTG),
    BLK(rd_clone_opp, RD_CLONE_OPP_SEG, OPP_S, INST_OPP),
    BLK(rd_clone_dpp, RD_CLONE_DPP_SEG, DPP_S, INST_PIPE),
    BLK(rd_clone_hubp, RD_CLONE_HUBP_SEG, HUBP_S, INST_PIPE),
    BLK(st_cursor, 2, HUBP_S, INST_PIPE),
    BLK(rd_clone_mpcc, RD_CLONE_MPCC_SEG, MPCC_S, INST_MPCC),
    BLK(rd_clone_digfe, RD_CLONE_DIGFE_SEG, DIG_S, INST_FE),
    BLK(st_tmds_be, 2, DIG_S, INST_BE),
};
#define NBLOCKS (sizeof(kBlocks) / sizeof(kBlocks[0]))
enum { B_OTG, B_ODM, B_OPP, B_DPP, B_HUBP, B_CURSOR, B_MPCC, B_DIGFE, B_TMDS };
typedef char rd_state_regs_fit[(sizeof(rd_clone_otg) + sizeof(rd_clone_odm) + sizeof(rd_clone_opp) +
                                sizeof(rd_clone_dpp) + sizeof(rd_clone_hubp) + sizeof(st_cursor) +
                                sizeof(rd_clone_mpcc) + sizeof(rd_clone_digfe) + sizeof(st_tmds_be)) / 2 <=
                               RD_DCN_STATE_REGS ? 1 : -1];

/* The global part: the DCHUBBUB system context (hubbub3_init_dchub_sys_ctx),
 * SDPIF and arbiter / watermark setup (hubbub32_init, hubbub32_program_
 * watermarks), the CRB split (hubbub32_program_compbuf_size) and the DCCG
 * globals of dccg2_init / dcn32_init_hw; offsets from dcn_3_2_0_offset.h */
#define CFG ~0u
static const struct { uint16_t seg, off; uint32_t mask; } kGlobal[] = {
    /* mask: the configuration bits (status, request and clear bits are
     * left to the hardware) */
    { 2, 0x046f, 0x7e00e000u }, { 2, 0x0470, CFG }, { 2, 0x0471, CFG },   /* DCHUBBUB_SDPIF_CFG0..2 */
    { 2, 0x0475, CFG }, { 2, 0x0476, CFG }, { 2, 0x0477, CFG },   /* DCN_VM_FB_LOCATION_BASE/TOP, FB_OFFSET */
    { 2, 0x0478, CFG }, { 2, 0x0479, CFG }, { 2, 0x047a, CFG },   /* DCN_VM_AGP_BOT/TOP/BASE */
    { 2, 0x0484, CFG },                               /* SDPIF_REQUEST_RATE_LIMIT */
    { 2, 0x04ba, 0x0001001fu },                       /* DCHUBBUB_COMPBUF_CTRL (before the DETs) */
    { 2, 0x04f9, CFG }, { 2, 0x04fa, CFG }, { 2, 0x04fc, CFG },   /* ARB_DF_REQ_OUTSTAND, SAT_LEVEL, DRAM_STATE */
    { 2, 0x04fd, 0x00000f00u },                       /* ARB_USR_RETRAINING_CNTL: the force bits */
    { 2, 0x04fe, CFG }, { 2, 0x04ff, CFG }, { 2, 0x0500, CFG }, { 2, 0x0501, CFG }, { 2, 0x0502, CFG },
    { 2, 0x0503, CFG }, { 2, 0x0504, CFG }, { 2, 0x0505, CFG }, { 2, 0x0506, CFG },   /* watermark set A */
    { 2, 0x0507, CFG }, { 2, 0x0508, CFG }, { 2, 0x0509, CFG }, { 2, 0x050a, CFG }, { 2, 0x050b, CFG },
    { 2, 0x050c, CFG }, { 2, 0x050d, CFG }, { 2, 0x050e, CFG }, { 2, 0x050f, CFG },   /* set B */
    { 2, 0x0510, CFG }, { 2, 0x0511, CFG }, { 2, 0x0512, CFG }, { 2, 0x0513, CFG }, { 2, 0x0514, CFG },
    { 2, 0x0515, CFG }, { 2, 0x0516, CFG }, { 2, 0x0517, CFG }, { 2, 0x0518, CFG },   /* set C */
    { 2, 0x0519, CFG }, { 2, 0x051a, CFG }, { 2, 0x051b, CFG }, { 2, 0x051c, CFG }, { 2, 0x051d, CFG },
    { 2, 0x051e, CFG }, { 2, 0x051f, CFG }, { 2, 0x0520, CFG }, { 2, 0x0521, CFG },   /* set D */
    { 2, 0x0523, 0x00000001u }, { 2, 0x0524, CFG }, { 2, 0x0525, CFG },   /* ARB_MALL_CNTL, TIMEOUT_ENABLE, GLOBAL_TIMER */
    { 2, 0x0533, CFG }, { 2, 0x0534, CFG },           /* DCHUBBUB_CLOCK_CNTL, DCFCLK_CNTL */
    { 1, 0x0074, CFG }, { 1, 0x007c, CFG }, { 1, 0x0049, CFG }, { 1, 0x0069, CFG },   /* DCCG_GATE_DISABLE_CNTL, 2, 4, 5 */
    { 1, 0x0050, CFG }, { 1, 0x007f, CFG }, { 1, 0x007b, CFG }, { 1, 0x0070, CFG },   /* FGCG_REP, DISP_CNTL, us / ms time base */
    { 1, 0x0058, CFG }, { 1, 0x004a, CFG }, { 1, 0x0068, CFG },   /* DPREFCLK_CNTL, DPSTREAMCLK_CNTL, DTBCLK_P_CNTL */
    { 1, 0x00a0, CFG }, { 1, 0x00a1, CFG }, { 1, 0x00a2, CFG }, { 1, 0x00a3, CFG }, { 1, 0x00a4, CFG },   /* SYMCLKA..E_CLOCK_ENABLE */
};
#undef CFG
#define NGLOBAL (sizeof(kGlobal) / sizeof(kGlobal[0]))
typedef char rd_state_globals_fit[NGLOBAL <= RD_DCN_STATE_GLOBALS ? 1 : -1];
#define G_FB_BASE 3          /* index of DCN_VM_FB_LOCATION_BASE */
#define G_FB_TOP  4

static uint32_t inst_of(const rdna3_dcn_head_state *h, uint32_t which)
{
    switch (which) {
    case INST_MPCC: return h->mpcc;
    case INST_OPP:  return h->opp;
    case INST_OTG:  return h->otg;
    case INST_FE:   return h->fe;
    case INST_BE:   return h->be;
    default:        return h->pipe;
    }
}

/* index of block b's first register in reg[] */
static uint32_t blk_base(uint32_t b)
{
    uint32_t i, n = 0;
    for (i = 0; i < b; i++)
        n += kBlocks[i].n;
    return n;
}

static uint32_t blk_addr(const rdna3_dcn_head_state *h, uint32_t b, uint32_t k)
{
    return kBlocks[b].list[k] + kBlocks[b].stride * inst_of(h, kBlocks[b].inst);
}

static void blk_write(rdna3_dev *d, const rdna3_dcn_head_state *h, uint32_t b)
{
    uint32_t k, base = blk_base(b);
    for (k = 0; k < kBlocks[b].n; k++)
        wr(d, kBlocks[b].seg, blk_addr(h, b, k), h->reg[base + k]);
}

/* dcn10_is_dig_enabled */
static int be_enabled(rdna3_dev *d, uint32_t be)
{
    uint32_t en = RI(d, regDIG0_DIG_BE_EN_CNTL, be, DIG_S);
    return en != 0xffffffffu && GET(en, DIG0_DIG_BE_EN_CNTL, DIG_ENABLE);
}

int dcn32_state_save(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_display_path *path,
                     uint32_t pclk_khz, rdna3_dcn_state *st)
{
    const rdna3_dcn_head *g;
    rdna3_dcn_head_state *h;
    uint32_t b, k, n, r, o_fe, o_be, o_mode, o_fe_cntl, o_be_cntl;
    int o_same, o_hpd, o_held, o_dsc;
    uint8_t o_enc, o_conn;
    rdna3_dsc_config o_dsc_cfg;

    if (!dc || !st || head >= dc->nheads || head >= RD_DCN_MAX_PIPES)
        return RDNA3_EINVAL;
    g = &dc->head[head];
    h = &st->head[head];
    /* what the last snapshot of this pipe knew (a link in standby reads
     * no DIG; the VBIOS path may be unknown to the caller) */
    o_same = h->valid && h->pipe == g->pipe && h->otg == g->otg;
    o_fe = h->fe;
    o_be = h->be;
    o_mode = h->dig_mode;
    o_hpd = h->hpd;
    o_enc = h->encoder_id;
    o_conn = h->connector_id;
    o_held = o_same && h->dp_held;
    o_fe_cntl = h->dig_fe_cntl;
    o_be_cntl = h->dig_be_cntl;
    o_dsc = h->dp_dsc;
    o_dsc_cfg = h->dp_dsc_cfg;
    for (k = 0; k < sizeof(*h); k++)
        ((uint8_t *)h)[k] = 0;
    h->pipe = g->pipe;
    h->otg = g->otg;
    h->opp = GET(RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, g->otg, ODM_S), ODM0_OPTC_DATA_SOURCE_SELECT,
                 OPTC_SEG0_SRC_SEL);
    if (h->opp >= RD_DCN_MAX_PIPES)
        h->opp = g->otg;
    h->mpcc = GET(RI(d, regMPC_OUT0_MUX, h->opp, MUX_S), MPC_OUT0_MUX, MPC_OUT_MUX);
    if (h->mpcc >= RD_DCN_MAX_PIPES)
        h->mpcc = g->pipe;   /* MPCC n belongs to DPP n */
    /* the DIG chain: as read now, or - link down for standby - as last saved */
    if (g->dig_fe >= 0 && g->dig_be >= 0) {
        h->fe = (uint32_t)g->dig_fe;
        h->be = (uint32_t)g->dig_be;
        h->dig_mode = g->dig_mode;
    } else if (o_same && o_mode) {
        h->fe = o_fe;
        h->be = o_be;
        h->dig_mode = o_mode;
    } else {
        h->fe = h->be = NDIG;
    }
    if (path) {
        h->hpd = path->hpd;
        h->encoder_id = (uint8_t)path->encoder_objid;
        h->connector_id = (uint8_t)path->connector_objid;
    } else if (o_same) {
        h->hpd = o_hpd;
        h->encoder_id = o_enc;
        h->connector_id = o_conn;
    } else {
        h->hpd = -1;
    }
    h->tmds = h->be < NDIG && (h->dig_mode == 2 || h->dig_mode == 3) && h->connector_id;
    h->dp = h->be < NDIG && h->dig_mode == 0 && g->odm_cnt < 2;
    if (o_held) {
        /* the link is still down from this code: its stream's DSC as it was */
        h->dp_held = 1;
        h->dp_dsc = o_dsc;
        h->dp_dsc_cfg = o_dsc_cfg;
    }
    /* the pixel clock: given, else from the DLG's ref/pix ratio */
    r = GET(RI(d, regHUBPREQ0_REF_FREQ_TO_PIX_FREQ, g->pipe, HUBP_S), HUBPREQ0_REF_FREQ_TO_PIX_FREQ,
            REF_FREQ_TO_PIX_FREQ);
    h->pclk_khz = pclk_khz ? pclk_khz : r ? (uint32_t)(((uint64_t)RD_DCN32_REFCLK_KHZ << 19) / r) : 0;
    h->geo = *g;
    h->cursor_size = dc->cursor_size[g->pipe];
    h->cursor_on = dc->cursor_on[g->pipe];

    h->hubp_cntl = RI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S);
    h->otg_control = RI(d, regOTG0_OTG_CONTROL, h->otg, OTG_S);
    h->vtg_control = RI(d, regVTG0_CONTROL, h->otg, VTG_S);
    h->odm_select = RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, h->otg, ODM_S);
    h->mpcc_top = RI(d, regMPCC0_MPCC_TOP_SEL, h->mpcc, MPCC_S);
    h->mpcc_bot = RI(d, regMPCC0_MPCC_BOT_SEL, h->mpcc, MPCC_S);
    h->mpcc_opp = RI(d, regMPCC0_MPCC_OPP_ID, h->mpcc, MPCC_S);
    h->mpc_out_mux = RI(d, regMPC_OUT0_MUX, h->opp, MUX_S);
    h->ogam_control = RI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, h->mpcc, OGAM_S);
    h->addr_lo = RI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, h->pipe, HUBP_S);
    h->addr_hi = RI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, h->pipe, HUBP_S);
    h->det = RI(d, regDCHUBBUB_DET0_CTRL, h->pipe, DET_S);
    h->pg_status = RI(d, regDOMAIN0_PG_STATUS, h->pipe, PG_S);
    if (o_held) {
        /* link_set_dpms_off disconnected the DIG (reset_dio_stream_encoder):
         * the routing as the link ran */
        h->dig_fe_cntl = o_fe_cntl;
        h->dig_be_cntl = o_be_cntl;
    } else if (h->be < NDIG) {
        h->dig_fe_cntl = RI(d, regDIG0_DIG_FE_CNTL, h->fe, DIG_S);
        h->dig_be_cntl = RI(d, regDIG0_DIG_BE_CNTL, h->be, DIG_S);
    }
    h->pixel_rate_cntl = RI(d, regOTG0_PIXEL_RATE_CNTL, h->otg, PRC_S);
    h->phypll_rate_cntl = RI(d, regOTG0_PHYPLL_PIXEL_RATE_CNTL, h->otg, PRC_S);
    h->dp_dto_phase = RI(d, regDP_DTO0_PHASE, h->otg, PRC_S);
    h->dp_dto_modulo = RI(d, regDP_DTO0_MODULO, h->otg, PRC_S);
        /* dccg32_get_pixel_rate_div: K1 and K2 of this OTG */
    h->pixel_rate_k = (RI(d, regOTG_PIXEL_RATE_DIV, 0, 0) >> (3 * h->otg)) & 7u;
    h->dppclk_dto = RI(d, regDPPCLK0_DTO_PARAM, h->pipe, DPPDTO_S);
    h->dppclk_dto_ctrl = (RI(d, regDPPCLK_DTO_CTRL, 0, 0) >> (4 * h->pipe)) & 3u;

    n = 0;
    for (b = 0; b < NBLOCKS; b++) {
        int none = (kBlocks[b].inst == INST_FE || kBlocks[b].inst == INST_BE) && h->be >= NDIG;
        for (k = 0; k < kBlocks[b].n; k++)
            h->reg[n++] = none ? 0 : rd(d, kBlocks[b].seg, blk_addr(h, b, k));
    }
    h->nregs = n;
    for (k = 0; k < NGLOBAL; k++)
        st->global[k] = rd(d, kGlobal[k].seg, kGlobal[k].off);
    st->nglobal = NGLOBAL;
    dcn32_clk_read(d, &st->clk);
    h->valid = 1;
    st->saves++;
    RLOG(d, "state: head %u saved: pipe %u otg %u opp %u mpcc %u DIG %u/%u mode %u %ux%u %u kHz at MC 0x%llx, "
         "%u registers, %s", head, h->pipe, h->otg, h->opp, h->mpcc, h->fe, h->be, h->dig_mode, g->h_active,
         g->v_active, h->pclk_khz, (unsigned long long)g->address, n,
         h->tmds ? "TMDS: restore and standby possible"
                 : h->dp ? "DP SST: restore and standby with its link" : "not TMDS / DP SST (or no VBIOS path): blank only");
    return RDNA3_OK;
}

void dcn32_state_drop(rdna3_dcn_state *st, uint32_t head)
{
    if (st && head < RD_DCN_MAX_PIPES)
        st->head[head].valid = 0;
}

int dcn32_state_check(rdna3_dev *d, const rdna3_dcn_state *st, uint32_t head, uint32_t wait_ms,
                      rdna3_dcn_state_check *k)
{
    const rdna3_dcn_head_state *h;
    uint32_t b, i, n, f0, f1, cntl;
    const uint32_t fmask = OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;

    if (!st || !k || head >= RD_DCN_MAX_PIPES || !st->head[head].valid)
        return RDNA3_EINVAL;
    h = &st->head[head];
    {
        rdna3_dcn_state_check z = { 0 };
        *k = z;
    }
    k->powered = !GET(RI(d, regDOMAIN0_PG_STATUS, h->pipe, PG_S), DOMAIN0_PG_STATUS, DOMAIN_PGFSM_PWR_STATUS);
    k->otg_on = GET(RI(d, regOTG0_OTG_CONTROL, h->otg, OTG_S), OTG0_OTG_CONTROL, OTG_MASTER_EN);
    cntl = RI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S);
    k->blanked = cntl == 0xffffffffu || GET(cntl, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN);
    k->dig_on = h->be < NDIG && be_enabled(d, h->be);
    k->timing_ok = RI(d, regOTG0_OTG_H_TOTAL, h->otg, OTG_S) == h->reg[blk_base(B_OTG) + 0] &&
                   RI(d, regOTG0_OTG_V_TOTAL, h->otg, OTG_S) == h->reg[blk_base(B_OTG) + 5];
    k->sys_ok = st->nglobal == NGLOBAL && rd(d, kGlobal[G_FB_BASE].seg, kGlobal[G_FB_BASE].off) == st->global[G_FB_BASE] &&
                rd(d, kGlobal[G_FB_TOP].seg, kGlobal[G_FB_TOP].off) == st->global[G_FB_TOP];
    k->routed = GET(cntl, HUBP0_DCHUBP_CNTL, HUBP_VTG_SEL) == GET(h->hubp_cntl, HUBP0_DCHUBP_CNTL, HUBP_VTG_SEL) &&
                RI(d, regMPCC0_MPCC_TOP_SEL, h->mpcc, MPCC_S) == h->mpcc_top &&
                RI(d, regMPC_OUT0_MUX, h->opp, MUX_S) == h->mpc_out_mux &&
                (h->be >= NDIG || (GET(RI(d, regDIG0_DIG_FE_CNTL, h->fe, DIG_S), DIG0_DIG_FE_CNTL, DIG_SOURCE_SELECT) ==
                                   GET(h->dig_fe_cntl, DIG0_DIG_FE_CNTL, DIG_SOURCE_SELECT) &&
                                   (RI(d, regDIG0_DIG_BE_CNTL, h->be, DIG_S) &
                                    (DIG0_DIG_BE_CNTL__DIG_MODE_MASK | DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK)) ==
                                   (h->dig_be_cntl &
                                    (DIG0_DIG_BE_CNTL__DIG_MODE_MASK | DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK))));
    for (b = 0, n = 0; b < NBLOCKS; b++)
        for (i = 0; i < kBlocks[b].n; i++, n++)
            if ((kBlocks[b].inst != INST_FE && kBlocks[b].inst != INST_BE) || h->be < NDIG)
                k->ndiff += rd(d, kBlocks[b].seg, blk_addr(h, b, i)) != h->reg[n];
    f0 = RI(d, regOTG0_OTG_STATUS_FRAME_COUNT, h->otg, OTG_S) & fmask;
    if (wait_ms) {
        d->os.msleep(d->os.ctx, wait_ms);
        f1 = RI(d, regOTG0_OTG_STATUS_FRAME_COUNT, h->otg, OTG_S) & fmask;
        k->frames = (f1 - f0) & fmask;
    }
    /* the link may be down on purpose (standby): that alone is no loss */
    k->lost = !k->powered || !k->otg_on || !k->timing_ok || !k->sys_ok || !k->routed ||
              (wait_ms && k->otg_on && !k->frames);
    RLOG(d, "state: head %u (pipe %u otg %u): power %d OTG %d blank %d DIG %d timing %d sysctx %d routing %d, "
         "%u registers differ, %u frames in %u ms -> %s", head, h->pipe, h->otg, k->powered, k->otg_on, k->blanked,
         k->dig_on, k->timing_ok, k->sys_ok, k->routed, k->ndiff, k->frames, wait_ms, k->lost ? "LOST" : "intact");
    return k->lost;
}

int dcn32_state_restore_global(rdna3_dev *d, const rdna3_dcn_state *st)
{
    uint32_t k, v, m;
    int n = 0;
    if (!st || st->nglobal != NGLOBAL)
        return 0;
    /* only after a reset or a power loss (the system aperture is gone):
     * a sleep that kept it leaves what the driver set since the snapshot */
    if (rd(d, kGlobal[G_FB_BASE].seg, kGlobal[G_FB_BASE].off) == st->global[G_FB_BASE] &&
        rd(d, kGlobal[G_FB_TOP].seg, kGlobal[G_FB_TOP].off) == st->global[G_FB_TOP])
        return 0;
    for (k = 0; k < NGLOBAL; k++) {
        v = rd(d, kGlobal[k].seg, kGlobal[k].off);
        m = kGlobal[k].mask;
        if ((v & m) != (st->global[k] & m)) {
            wr(d, kGlobal[k].seg, kGlobal[k].off, (v & ~m) | (st->global[k] & m));
            n++;
        }
    }
    if (n)
        RLOG(d, "state: %d global DCHUBBUB / DCCG registers put back", n);
    return n;
}

/* dcn32_hubp_pg_control under DC_IP_REQUEST_CNTL, as dcn32_clone.c */
static int pipe_power_on(rdna3_dev *d, uint32_t pipe)
{
    uint32_t v;
    int ret;
    WI(d, regDC_IP_REQUEST_CNTL, 0, 0, DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK);
    v = RI(d, regDOMAIN0_PG_CONFIG, pipe, PG_S);
    WI(d, regDOMAIN0_PG_CONFIG, pipe, PG_S, SET(v, DOMAIN0_PG_CONFIG, DOMAIN_POWER_GATE, 0));
    ret = rdna3_wait_reg(d, AI(d, regDOMAIN0_PG_STATUS, pipe, PG_S),
                         PGFSM_POWER_ON << DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS__SHIFT,
                         DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS_MASK, 0, 1000);
    WI(d, regDC_IP_REQUEST_CNTL, 0, 0, 0);
    if (ret)
        RERR(d, "state: pipe %u power on timed out (PG_STATUS 0x%x)", pipe, RI(d, regDOMAIN0_PG_STATUS, pipe, PG_S));
    return ret;
}

/* dcn32_program_det_size for one pipe */
static int det_set(rdna3_dev *d, uint32_t pipe, uint32_t segs)
{
    uint32_t v = RI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S);
    int ret;
    WI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S, SET(v, DCHUBBUB_DET0_CTRL, DET0_SIZE, segs));
    ret = rdna3_wait_reg(d, AI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S),
                         segs << DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT__SHIFT,
                         DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT_MASK, 0, 1000);
    if (ret)
        RERR(d, "state: DET%u -> %u segments timed out (0x%x)", pipe, segs, RI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S));
    return ret;
}

static int vbios_tx(rdna3_dmub *dm, const rdna3_dcn_head_state *h, uint8_t action)
{
    /* transmitter_control_v1_7 as dcn10_link_encoder_enable_tmds_output /
     * _disable_output / _hw_init (INIT) fill it */
    rdna3_dig_tx_v1_7 t = { 0 };
    t.phyid = (uint8_t)h->be;
    t.action = action;
    t.lanenum = 4;
    t.hpdsel = (uint8_t)(h->hpd >= 0 ? h->hpd + 1 : 0);
    t.connobj_id = h->connector_id;
    if (action != RD_ATOM_TX_INIT) {
        t.digmode = (uint8_t)h->dig_mode;
        t.digfe_sel = (uint8_t)(1u << h->fe);
        t.symclk_10khz = h->pclk_khz / 10;
    }
    return dmub32_transmitter_control(dm, &t);
}

static int vbios_pclk(rdna3_dmub *dm, const rdna3_dcn_head_state *h, uint32_t khz)
{
    /* set_pixel_clock_v7 as dcn31_program_pix_clk fills it for TMDS; 0
     * stops the PHY PLL (dce110_clock_source_power_down) */
    rdna3_pixclk_v1_7 p = { 0 };
    p.pixclk_100hz = khz * 10;
    p.pll_id = (uint8_t)(RD_ATOM_COMBOPHY_PLL0 + h->be);
    p.encoderobjid = h->encoder_id;
    p.encoder_mode = h->dig_mode == 2 ? RD_ATOM_ENCODER_MODE_DVI : RD_ATOM_ENCODER_MODE_HDMI;
    p.crtc_id = (uint8_t)h->otg;
    return dmub32_set_pixel_clock(dm, &p);
}

static int vbios_stream(rdna3_dmub *dm, const rdna3_dcn_head_state *h)
{
    rdna3_dig_enc_v1_5 e = { 0 };
    e.digid = (uint8_t)h->fe;
    e.action = RD_ATOM_ENCODER_CMD_STREAM_SETUP;
    e.digmode = h->dig_mode == 2 ? RD_ATOM_ENCODER_MODE_DVI : RD_ATOM_ENCODER_MODE_HDMI;
    e.lanenum = 4;
    e.pclk_10khz = h->pclk_khz / 10;
    e.bitpercolor = RD_ATOM_PANEL_8BIT_PER_COLOR;
    return dmub32_encoder_control(dm, &e);
}

/* hubp2_set_blank_regs: the outstanding requests drain first */
static void hubp_blank(rdna3_dev *d, uint32_t pipe)
{
    uint32_t v = RI(d, regHUBP0_DCHUBP_CNTL, pipe, HUBP_S), t;
    if (v == 0xffffffffu || GET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN))
        return;
    for (t = 0; t < 100000 && !GET(RI(d, regHUBP0_DCHUBP_CNTL, pipe, HUBP_S), HUBP0_DCHUBP_CNTL,
                                    HUBP_NO_OUTSTANDING_REQ); t++)
        d->os.udelay(d->os.ctx, 1);
    v = RI(d, regHUBP0_DCHUBP_CNTL, pipe, HUBP_S);
    v = SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 1);
    WI(d, regHUBP0_DCHUBP_CNTL, pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_TTU_DISABLE, 1));
}

/* optc32_disable_crtc: OTG then VTG, REG_WAIT(OTG_BUSY, 0, 1 us, 150000) */
static void otg_disable(rdna3_dev *d, uint32_t otg)
{
    uint32_t v = RI(d, regOTG0_OTG_CONTROL, otg, OTG_S);
    if (!GET(v, OTG0_OTG_CONTROL, OTG_MASTER_EN))
        return;
    WI(d, regOTG0_OTG_CONTROL, otg, OTG_S, SET(v, OTG0_OTG_CONTROL, OTG_MASTER_EN, 0));
    v = RI(d, regVTG0_CONTROL, otg, VTG_S);
    WI(d, regVTG0_CONTROL, otg, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 0));
    if (rdna3_wait_reg(d, AI(d, regOTG0_OTG_CLOCK_CONTROL, otg, OTG_S), 0, OTG0_OTG_CLOCK_CONTROL__OTG_BUSY_MASK,
                       0, 150000))
        RERR(d, "state: OTG %u stays busy after the disable", otg);
}

/* link_set_dpms_off, TMDS: the stream blanked, then disable_link ->
 * dcn10_link_encoder_disable_output (nothing when the DIG is off) */
static int link_off(rdna3_dev *d, rdna3_dmub *dm, const rdna3_dcn_head_state *h)
{
    hubp_blank(d, h->pipe);
    if (!be_enabled(d, h->be))
        return RDNA3_OK;
    return vbios_tx(dm, h, RD_ATOM_TX_DISABLE);
}

/* a DP head's link: the one driving its DIG and OTG, SST */
static int dp_link_for(rdna3_dev *d, const rdna3_dcn_head_state *h, const rdna3_dp_link *dp, uint32_t head)
{
    if (!h->dp || !dp || !dp->d || dp->mst || dp->otg != h->otg || dp->fe != h->fe || dp->be != h->be) {
        RERR(d, "state: head %u: %s: blank only", head,
             !h->dp ? (h->be < NDIG && h->dig_mode == 5 ? "a DP MST head" : h->be < NDIG && h->dig_mode == 0
                                                                                  ? "an ODM combined DP head"
                                                                                  : "neither TMDS nor DP SST")
                    : !dp || !dp->d ? "a DP head without its link"
                    : dp->mst      ? "its DP link runs MST"
                                   : "its DP link drives another DIG / OTG");
        return RDNA3_EINVAL;
    }
    return RDNA3_OK;
}

/* link_set_dpms_off for a DP head: the stream's DSC goes into the snapshot
 * first (the link forgets it), unless an earlier one is held there */
static void dp_link_down(rdna3_dcn_head_state *h, rdna3_dp_link *dp, rdna3_dmub *dm)
{
    if (!h->dp_held) {
        h->dp_dsc = dp->stream_dsc;
        h->dp_dsc_cfg = dp->dsc_cfg;
        h->dp_held = 1;
    }
    dp->dm = dm;
    dcn32_dp_link_set_dpms_off(dp);
}

/* the stream's resources for link_set_dpms_on: DSC as held, one ODM
 * segment (program_dp of dcn32_modeset.c) */
static void dp_stream_back(const rdna3_dcn_head_state *h, rdna3_dp_link *dp, rdna3_dmub *dm)
{
    dp->dm = dm;
    if (h->dp_held) {
        dp->stream_dsc = h->dp_dsc;
        dp->dsc_cfg = h->dp_dsc_cfg;
    }
    dp->odm_opp_cnt = 1;
    dp->odm_opp[0] = h->opp;
    dp->odm_dsc[0] = dp->dsc_inst;
}

int dcn32_head_link(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn *dc, rdna3_dcn_state *st, uint32_t head, int on,
                    rdna3_dp_link *dp)
{
    rdna3_dcn_head_state *h;
    uint32_t v;
    int ret = RDNA3_OK;
    if (!dc || !st || head >= dc->nheads || head >= RD_DCN_MAX_PIPES || !st->head[head].valid)
        return RDNA3_EINVAL;
    h = &st->head[head];
    if (h->pipe != dc->head[head].pipe || (!h->tmds && dp_link_for(d, h, dp, head)))
        return RDNA3_EINVAL;   /* the caller blanks */
    if (!dm || !dm->hw_init)
        return RDNA3_ENODEV;
    if (!on && !h->tmds) {
        hubp_blank(d, h->pipe);
        dp_link_down(h, dp, dm);
    } else if (!on) {
        ret = link_off(d, dm, h);
    } else {
        if (!h->tmds) {
            /* link_set_dpms_on on the running OTG: trained again */
            rdna3_mode m;
            dcn32_modeset_head_mode(&h->geo, h->pclk_khz, &m);
            dp_stream_back(h, dp, dm);
            ret = dcn32_dp_link_set_dpms_on(dp, &m, 0);
            h->dp_held = 0;
        } else if (!be_enabled(d, h->be)) {
            /* enable_link_hdmi -> dcn10_link_encoder_enable_tmds_output */
            ret = vbios_tx(dm, h, RD_ATOM_TX_ENABLE);
        }
        /* the unblank (hubp2_set_blank(false)) */
        v = RI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S);
        v = SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 0);
        WI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_TTU_DISABLE, 0));
    }
    RLOG(d, "state: head %u link %s on UNIPHY%c (%d)", head, on ? "up" : "down (monitor standby)", 'A' + h->be, ret);
    return ret;
}

int dcn32_state_head_off(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn *dc, rdna3_dcn_state *st, uint32_t head,
                         rdna3_dp_link *dp)
{
    rdna3_dcn_head_state *h;
    int ret = RDNA3_OK, isdp;
    if (!dc || !st || head >= dc->nheads || head >= RD_DCN_MAX_PIPES || !st->head[head].valid)
        return RDNA3_EINVAL;
    h = &st->head[head];
    isdp = !h->tmds && dp_link_for(d, h, dp, head) == RDNA3_OK;
    if ((!h->tmds && !isdp) || !dm || !dm->hw_init) {
        hubp_blank(d, h->pipe);   /* what can be done without the PHY commands */
        return h->tmds || isdp ? RDNA3_ENODEV : RDNA3_EINVAL;
    }
    /* dcn20_reset_back_end_for_pipe: link_set_dpms_off, disable_crtc,
     * the pixel clock source powered down (a DP stream's is the DP DTO) */
    if (isdp) {
        hubp_blank(d, h->pipe);
        dp_link_down(h, dp, dm);
    } else {
        ret = link_off(d, dm, h);
    }
    otg_disable(d, h->otg);
    if (!ret && !isdp)
        ret = vbios_pclk(dm, h, 0);
    RLOG(d, "state: head %u off for the sleep (%d)", head, ret);
    return ret;
}

int dcn32_state_restore(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn *dc, rdna3_dcn_state *st, uint32_t head,
                        int blank, int link_off_wanted, rdna3_dp_link *dp)
{
    rdna3_dcn_head_state *h;
    rdna3_dcn_clocks ck;
    rdna3_mode mode;
    uint32_t v, keep_be_mask, live_be, ogam, div, ctl;
    int fresh, be_lost, ret = RDNA3_OK, r2, isdp, dp_stream = 0;
    const uint32_t prc_cfg = OTG0_PIXEL_RATE_CNTL__OTG0_PIXEL_RATE_SOURCE_MASK |
                             OTG0_PIXEL_RATE_CNTL__DTBCLK_DTO0_ENABLE_MASK |
                             OTG0_PIXEL_RATE_CNTL__DP_DTO0_ENABLE_MASK |
                             OTG0_PIXEL_RATE_CNTL__DP_DTO0_DS_DISABLE_MASK |
                             OTG0_PIXEL_RATE_CNTL__PIPE0_DTO_SRC_SEL_MASK;

    if (!dc || !st || head >= dc->nheads || head >= RD_DCN_MAX_PIPES || !st->head[head].valid)
        return RDNA3_EINVAL;
    h = &st->head[head];
    isdp = !h->tmds;
    if (isdp && dp_link_for(d, h, dp, head))
        return RDNA3_EINVAL;
    if (isdp && !h->pclk_khz) {
        RERR(d, "state: head %u: a DP head without its pixel clock: no restore", head);
        return RDNA3_EINVAL;
    }
    dcn32_modeset_head_mode(&h->geo, h->pclk_khz, &mode);
    if (!dm || !dm->hw_init) {
        RERR(d, "state: head %u: no DMCUB to drive the PHY, no restore", head);
        return RDNA3_ENODEV;
    }
    /* dcn32_init_clocks / dcn32_update_clocks(safe_to_lower = false) after
     * a power loss: the PMFW came back at its boot clocks; the ones the
     * head ran at (or what its pixel rate needs) are asked back through
     * the DAL mailbox before anything is programmed */
    dcn32_clk_read(d, &ck);
    if (dcn32_clk_fits(&ck, h->pclk_khz)) {
        uint32_t pmfw = 0, disp, dpp;
        int rr = dcn32_smu_send(d, RD_DALSMC_MSG_GetSmuVersion, 0, &pmfw);
        disp = dcn32_required_dispclk(h->pclk_khz, ck.vco_khz, 0);
        dpp = dcn32_required_dppclk(h->pclk_khz, ck.vco_khz);
        if (st->clk.dispclk_khz > disp)
            disp = st->clk.dispclk_khz;
        if (st->clk.dppclk_khz > dpp)
            dpp = st->clk.dppclk_khz;
        if (!rr)
            rr = dcn32_clk_raise(d, pmfw, h->pipe, disp, dpp, &ck);
        if (rr || dcn32_clk_fits(&ck, h->pclk_khz)) {
            RERR(d, "state: head %u: DISPCLK %u / DPPCLK %u kHz no longer carry %u kHz (saved: %u / %u), "
                 "raising them failed (%d)", head, ck.dispclk_khz, ck.dppclk_khz, h->pclk_khz,
                 st->clk.dispclk_khz, st->clk.dppclk_khz, rr);
            return RDNA3_EBUSY;
        }
        RLOG(d, "state: head %u: clocks raised for %u kHz: DISPCLK %u / DPPCLK %u kHz", head, h->pclk_khz,
             ck.dispclk_khz, ck.dppclk_khz);
    }
    /* the global part first; the pipe lost its power or the system
     * context: the LUT RAMs went with it */
    fresh = dcn32_state_restore_global(d, st) > 0;
    fresh |= GET(RI(d, regDOMAIN0_PG_STATUS, h->pipe, PG_S), DOMAIN0_PG_STATUS, DOMAIN_PGFSM_PWR_STATUS) != 0;
    keep_be_mask = DIG0_DIG_BE_CNTL__DIG_MODE_MASK | DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK;
    live_be = RI(d, regDIG0_DIG_BE_CNTL, h->be, DIG_S);
    be_lost = fresh || (live_be & keep_be_mask) != (h->dig_be_cntl & keep_be_mask);
    RLOG(d, "state: head %u restore: pipe %u otg %u DIG %u/%u %s %u kHz%s%s", head, h->pipe, h->otg, h->fe, h->be,
         isdp ? "DP" : "TMDS", h->pclk_khz, fresh ? ", pipe / system context lost" : "",
         be_lost ? ", PHY setup lost" : "");

    /* 1. whatever still runs goes down first (link_set_dpms_off, disable_crtc) */
    if (!GET(RI(d, regDOMAIN0_PG_STATUS, h->pipe, PG_S), DOMAIN0_PG_STATUS, DOMAIN_PGFSM_PWR_STATUS))
        hubp_blank(d, h->pipe);
    if (isdp)
        dp_link_down(h, dp, dm);   /* the link's own state too, wherever the power left it */
    else if (be_enabled(d, h->be))
        ret = vbios_tx(dm, h, RD_ATOM_TX_DISABLE);
    otg_disable(d, h->otg);

    /* 2. dcn10_power_on_plane_resources, dcn32_program_det_size */
    if (GET(RI(d, regDOMAIN0_PG_STATUS, h->pipe, PG_S), DOMAIN0_PG_STATUS, DOMAIN_PGFSM_PWR_STATUS)) {
        r2 = pipe_power_on(d, h->pipe);
        if (r2)
            return r2;
    }
    if (GET(RI(d, regDCHUBBUB_DET0_CTRL, h->pipe, DET_S), DCHUBBUB_DET0_CTRL, DET0_SIZE) !=
        GET(h->det, DCHUBBUB_DET0_CTRL, DET0_SIZE)) {
        r2 = det_set(d, h->pipe, GET(h->det, DCHUBBUB_DET0_CTRL, DET0_SIZE));
        if (r2)
            return r2;
    }

    /* 3. dcn20_enable_stream_timing: DCCG, pixel clock, timing */
    WI(d, regDP_DTO0_PHASE, h->otg, PRC_S, h->dp_dto_phase);
    WI(d, regDP_DTO0_MODULO, h->otg, PRC_S, h->dp_dto_modulo);
    v = RI(d, regOTG0_PIXEL_RATE_CNTL, h->otg, PRC_S) & ~prc_cfg;
    WI(d, regOTG0_PIXEL_RATE_CNTL, h->otg, PRC_S, v | (h->pixel_rate_cntl & prc_cfg));
    div = RI(d, regOTG_PIXEL_RATE_DIV, 0, 0);
    WI(d, regOTG_PIXEL_RATE_DIV, 0, 0, (div & ~(7u << (3 * h->otg))) | (h->pixel_rate_k << (3 * h->otg)));
    WI(d, regDPPCLK0_DTO_PARAM, h->pipe, DPPDTO_S, h->dppclk_dto);
    ctl = RI(d, regDPPCLK_DTO_CTRL, 0, 0);
    WI(d, regDPPCLK_DTO_CTRL, 0, 0, (ctl & ~(3u << (4 * h->pipe))) | (h->dppclk_dto_ctrl << (4 * h->pipe)));
    if (isdp) {
        /* the stream's resources again (validation: the link setting, the
         * HPO encoders for 128b/132b), DTBCLK for those (dcn32_update_clocks
         * with dtbclk_en: the PMFW forgot it with the power), then the DP
         * DTO / pixel rate divider of dcn20_enable_stream_timing */
        dp_stream_back(h, dp, dm);
        if (ck.dprefclk_khz)
            dp->dprefclk_khz = ck.dprefclk_khz;   /* the DP DTO's reference, as dcn32_modeset_plan_dp */
        if (!dp->verified)
            dcn32_dp_verify_link_cap(dp, RD_DP_LINK_TRAINING_MAX_VERIFY_RETRY);   /* a new sink: as program_dp */
        r2 = dcn32_dp_acquire_stream(dp, &mode);
        if (!r2 && dp->hpo_active) {
            uint32_t pmfw = 0;
            r2 = dcn32_smu_send(d, RD_DALSMC_MSG_GetSmuVersion, 0, &pmfw);
            if (!r2)
                r2 = dcn32_clk_dtbclk_enable(d, pmfw, &dp->dtbclk_khz);
            if (r2)
                RERR(d, "state: head %u: DTBCLK for the 128b/132b link not set (%d)", head, r2);
        }
        if (r2) {
            RERR(d, "state: head %u: the DP stream gets no link resources (%d): left blank", head, r2);
            dp->hpo_active = 0;
            if (!ret)
                ret = r2;
        } else {
            dcn32_dp_enable_stream_timing(dp, &mode);
            dp_stream = 1;
        }
    } else {
        r2 = vbios_pclk(dm, h, h->pclk_khz);
        if (r2 && !ret)
            ret = r2;
    }
    blk_write(d, h, B_OTG);
    WI(d, regOTG0_OTG_CONTROL, h->otg, OTG_S, SET(h->otg_control, OTG0_OTG_CONTROL, OTG_MASTER_EN, 0));
    blk_write(d, h, B_ODM);
    WI(d, regODM0_OPTC_DATA_SOURCE_SELECT, h->otg, ODM_S, h->odm_select);
    blk_write(d, h, B_OPP);
    WI(d, regVTG0_CONTROL, h->otg, VTG_S, SET(h->vtg_control, VTG0_CONTROL, VTG0_ENABLE, 0));

    /* 4. the plane: DPP, HUBP (blanked), cursor, MPC tree */
    blk_write(d, h, B_DPP);
    v = RI(d, regCM0_CM_GAMCOR_CONTROL, h->pipe, DPP_S);
    if (fresh && GET(v, CM0_CM_GAMCOR_CONTROL, CM_GAMCOR_MODE)) {
        RLOG(d, "state: head %u: degamma LUT RAM lost, bypassed", head);
        WI(d, regCM0_CM_GAMCOR_CONTROL, h->pipe, DPP_S, SET(v, CM0_CM_GAMCOR_CONTROL, CM_GAMCOR_MODE, 0));
    }
    blk_write(d, h, B_HUBP);
    WI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S, SET(h->hubp_cntl, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 1));
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, h->pipe, HUBP_S, h->addr_hi);
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, h->pipe, HUBP_S, h->addr_lo);
    blk_write(d, h, B_CURSOR);
    blk_write(d, h, B_MPCC);
    WI(d, regMPCC0_MPCC_TOP_SEL, h->mpcc, MPCC_S, h->mpcc_top);
    WI(d, regMPCC0_MPCC_BOT_SEL, h->mpcc, MPCC_S, h->mpcc_bot);
    WI(d, regMPCC0_MPCC_OPP_ID, h->mpcc, MPCC_S, h->mpcc_opp);
    WI(d, regMPC_OUT0_MUX, h->opp, MUX_S, h->mpc_out_mux);
    ogam = h->ogam_control;
    if (fresh && GET(ogam, MPCC_OGAM0_MPCC_OGAM_CONTROL, MPCC_OGAM_MODE) >= 2) {
        RLOG(d, "state: head %u: output gamma LUT RAM lost, bypassed (the gamma table must be loaded again)", head);
        ogam = SET(ogam, MPCC_OGAM0_MPCC_OGAM_CONTROL, MPCC_OGAM_MODE, 0);
    }
    WI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, h->mpcc, OGAM_S, ogam);

    /* 5. link_set_dpms_on: enc1_dig_connect_to_otg, STREAM_SETUP, the
     * front end's HDMI registers without infoframes (their VPG contents
     * are not kept), the back end's TMDS setup and routing; a DP stream
     * only connects its front end here, its link does the rest in step 6 */
    if (isdp) {
        v = RI(d, regDIG0_DIG_FE_CNTL, h->fe, DIG_S);
        WI(d, regDIG0_DIG_FE_CNTL, h->fe, DIG_S,
           (v & ~DIG0_DIG_FE_CNTL__DIG_SOURCE_SELECT_MASK) | (h->dig_fe_cntl & DIG0_DIG_FE_CNTL__DIG_SOURCE_SELECT_MASK));
        if (fresh) {
            /* dcn10_link_encoder_hw_init, as init_hw after a power loss
             * (link_set_dpms_off disconnected the back end: its routing
             * says nothing here) */
            r2 = dcn32_dp_link_encoder_hw_init(dp);
            if (r2 && !ret)
                ret = r2;
        }
        goto enable;
    }
    WI(d, regDIG0_DIG_FE_CNTL, h->fe, DIG_S, h->dig_fe_cntl);
    r2 = vbios_stream(dm, h);
    if (r2 && !ret)
        ret = r2;
    blk_write(d, h, B_DIGFE);
    WI(d, regDIG0_HDMI_INFOFRAME_CONTROL0, h->fe, DIG_S, 0);
    WI(d, regDIG0_HDMI_INFOFRAME_CONTROL1, h->fe, DIG_S, 0);
    WI(d, regDIG0_HDMI_GENERIC_PACKET_CONTROL0, h->fe, DIG_S, 0);
    blk_write(d, h, B_TMDS);
    v = RI(d, regDIG0_DIG_BE_CNTL, h->be, DIG_S);
    WI(d, regDIG0_DIG_BE_CNTL, h->be, DIG_S, (v & ~keep_be_mask) | (h->dig_be_cntl & keep_be_mask));
    if (be_lost) {
        r2 = vbios_tx(dm, h, RD_ATOM_TX_INIT);   /* dcn10_link_encoder_hw_init */
        if (r2 && !ret)
            ret = r2;
    }

enable:
    /* 6. optc32_enable_crtc: "VTG enable first is for HW workaround" */
    WI(d, regVTG0_CONTROL, h->otg, VTG_S, SET(h->vtg_control, VTG0_CONTROL, VTG0_ENABLE, 1));
    WI(d, regOTG0_OTG_CONTROL, h->otg, OTG_S, SET(h->otg_control, OTG0_OTG_CONTROL, OTG_MASTER_EN, 1));
    if (!link_off_wanted && isdp && dp_stream) {
        /* link_set_dpms_on: the link trained again (without fallback, as
         * program_dp: the verified link cap already fell back); a failed
         * training leaves the SST stream on, as DC does */
        r2 = dcn32_dp_link_set_dpms_on(dp, &mode, 0);
        h->dp_held = 0;
        if (r2 && !ret)
            ret = r2;
    } else if (!link_off_wanted && !isdp) {
        r2 = vbios_tx(dm, h, RD_ATOM_TX_ENABLE);   /* dcn10_link_encoder_enable_tmds_output */
        if (r2 && !ret)
            ret = r2;
    }
    v = RI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S);
    WI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_UNDERFLOW_CLEAR, 1));
    if (!blank && !link_off_wanted && (!isdp || dp_stream)) {
        v = RI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S);
        v = SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 0);
        WI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_TTU_DISABLE, 0));
    }
    dc->head[head] = h->geo;
    dc->requested[h->pipe] = h->geo.address;
    dc->cursor_size[h->pipe] = h->cursor_size;
    dc->cursor_on[h->pipe] = h->cursor_on;
    st->restores++;
    RLOG(d, "state: head %u restored (%d): OTG_CONTROL 0x%x DIG_BE_CNTL 0x%x BE_EN 0x%x HUBP 0x%x", head, ret,
         RI(d, regOTG0_OTG_CONTROL, h->otg, OTG_S), RI(d, regDIG0_DIG_BE_CNTL, h->be, DIG_S),
         RI(d, regDIG0_DIG_BE_EN_CNTL, h->be, DIG_S), RI(d, regHUBP0_DCHUBP_CNTL, h->pipe, HUBP_S));
    return ret;
}
