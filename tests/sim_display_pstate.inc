/*
 * sim_display_pstate.inc - included by sim_navi32.c before sim_wr: the model
 * of the PMFW's DAL messages for display power (item O: the DPM tables,
 * the watermark range table it fetches from VRAM, SetFclkSwitchAllow,
 * SetCabForUclkPstate, BacoAudioD3PME, deep sleep DCFCLK, the UCLK hard
 * max), the DMCUB commands CAB_FOR_SS and FW_ASSISTED_MCLK_SWITCH (FAMS
 * setup, HANDLE_SUBVP_CMD decoded), a phantom HUBP's request drain, and an
 * underflow check: could the PMFW switch UCLK (UCLK not pinned, or the
 * arbiter forced to allow) while a lit plane cannot hide the switch?
 * The tests are test_dc_pstate (called after test_dc_k).
 *
 * The model only acts while SP.on: the older tests keep the DAL handler
 * of sim_navi32.c (which still answers the messages it knows: hard
 * minimums, the version, DISPCLK / DPPCLK / DTBCLK levels).
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_pstate.h"
#include "dcn32_subvp.h"
#include "gen/dcn_3_2_0_pstate.h"

#define SP_HUBP(reg, n) (DCN_B2 + (reg) + RD_CLONE_HUBP_STRIDE * (n))
#define SP_OPP(reg, n)  (DCN_B2 + (reg) + RD_CLONE_OPP_STRIDE * (n))
#define SP_UCLK_LEVELS 4

static struct {
    int on;
    uint32_t uclk[SP_UCLK_LEVELS], fclk[4], dcfclk[4], socclk[3];
    uint32_t dram_hi, dram_lo, transfers, table_id, table_bad;
    uint8_t table[RD_WM_TABLE_BYTES];
    uint32_t fclk_allow, fclk_msgs, cab_ways, cab_msgs, pme, ds_dcfclk, hard_max[16], always_wait;
    /* DMCUB */
    unsigned fams_setup, cab_cmds, subvp_cmds;
    uint32_t cab_sub, cab_ways_dmub;
    uint8_t subvp_cmd[RD_DMUB_RB_CMD_SIZE];
} SP;

/* the decoded DMUB_CMD__HANDLE_SUBVP_CMD (dmub_cmd.h, packed) */
typedef struct {
    uint32_t wm_a_cache, vint_margin, allow_width;
    struct {
        uint32_t mode, pix_clk_100hz, vblank_start, vblank_end, mall_lines, prefetch_lines, p2m_lines, delay_lines;
        uint32_t htotal, vtotal, main_idx, phantom_idx, num, den, vstartup, vblank_idx;
    } p[2];
} sp_subvp_cmd;

static uint32_t sp16(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8; }
static uint32_t sp32(const uint8_t *b) { return sp16(b) | sp16(b + 2) << 16; }

static void sp_decode(const uint8_t *c, sp_subvp_cmd *o)
{
    uint32_t i;
    memset(o, 0, sizeof(*o));
    o->wm_a_cache = sp16(c + 4);
    o->vint_margin = c[6];
    o->allow_width = c[7];
    for (i = 0; i < 2; i++) {
        const uint8_t *p = c + 8 + 28 * i;
        o->p[i].mode = p[27];
        o->p[i].pix_clk_100hz = sp32(p);
        if (p[27] == RD_MCLK_SWITCH_SUBVP) {
            o->p[i].vblank_start = sp16(p + 4);
            o->p[i].vblank_end = sp16(p + 6);
            o->p[i].mall_lines = sp16(p + 8);
            o->p[i].prefetch_lines = sp16(p + 10);
            o->p[i].p2m_lines = sp16(p + 12);
            o->p[i].delay_lines = sp16(p + 14);
            o->p[i].htotal = sp16(p + 16);
            o->p[i].vtotal = sp16(p + 18);
            o->p[i].main_idx = p[20];
            o->p[i].phantom_idx = p[21];
            o->p[i].num = p[22];
            o->p[i].den = p[23];
        } else if (p[27] == RD_MCLK_SWITCH_VBLANK) {
            o->p[i].vblank_start = sp16(p + 4);
            o->p[i].vblank_end = sp16(p + 6);
            o->p[i].vstartup = sp16(p + 8);
            o->p[i].vtotal = sp16(p + 10);
            o->p[i].htotal = sp16(p + 12);
            o->p[i].vblank_idx = p[14];
        }
    }
}

/* DMCUB ring commands of item O (dmub_inbox's default case) */
static int pst_dmub_cmd(uint8_t *c, uint32_t hdr)
{
    uint32_t type = hdr & 0xff, sub = (hdr >> 8) & 0xff, len = (hdr >> 24) & 0x3f;
    if (type == RD_DMUB_CMD_CAB_FOR_SS && len == 2 && sub <= RD_DMUB_CAB_DCN_SS_NOT_FIT_IN_CAB) {
        SP.cab_cmds++;
        SP.cab_sub = sub;
        SP.cab_ways_dmub = c[4];
        return 1;
    }
    if (type == RD_DMUB_CMD_FW_ASSISTED_MCLK_SWITCH && sub == RD_DMUB_FAMS_SETUP_FW_CTRL && len == 52) {
        SP.fams_setup++;
        return 1;
    }
    if (type == RD_DMUB_CMD_FW_ASSISTED_MCLK_SWITCH && sub == RD_DMUB_FAMS_HANDLE_SUBVP_CMD && len == 60) {
        SP.subvp_cmds++;
        memcpy(SP.subvp_cmd, c, RD_DMUB_RB_CMD_SIZE);
        return 1;
    }
    return 0;
}

static uint32_t sp_level(const uint32_t *t, uint32_t n, uint32_t idx)
{
    return idx == 0xff ? n : idx < n ? t[idx] : 0;
}

static int pst_wr(uint32_t dw, uint32_t v)
{
    uint32_t arg, clk, idx, out;
    int known = 1;
    if (!SP.on || dw != RD_DAL_MSG_REG)
        return 0;
    arg = S.reg[RD_DAL_ARG_REG];
    clk = arg >> 16;
    idx = arg & 0xff;
    out = arg;
    switch (v) {
    case RD_DALSMC_MSG_GetDpmFreqByIndex:
        if (clk == RD_PPCLK_UCLK)
            out = sp_level(SP.uclk, SP_UCLK_LEVELS, idx);
        else if (clk == RD_PPCLK_FCLK)
            out = sp_level(SP.fclk, 4, idx);
        else if (clk == RD_PPCLK_DCFCLK)
            out = sp_level(SP.dcfclk, 4, idx);
        else if (clk == RD_PPCLK_SOCCLK)
            out = sp_level(SP.socclk, 3, idx);
        else
            known = 0;   /* DISPCLK / DPPCLK / DTBCLK: sim_navi32.c */
        break;
    case RD_DALSMC_MSG_GetDcModeMaxDpmFreq:
        /* AC power: the DC-mode maxima are the top levels */
        out = clk == RD_PPCLK_UCLK ? SP.uclk[SP_UCLK_LEVELS - 1] : clk == RD_PPCLK_FCLK ? SP.fclk[3]
            : clk == RD_PPCLK_DCFCLK ? SP.dcfclk[3] : clk == RD_PPCLK_SOCCLK ? SP.socclk[2]
            : clk == RD_PPCLK_DTBCLK ? 600 : (clk == RD_PPCLK_DISPCLK || clk == RD_PPCLK_DPPCLK) && S.dal_dpm_n
            ? S.dal_dpm[S.dal_dpm_n - 1] : 0;
        break;
    case RD_DALSMC_MSG_SetDalDramAddrHigh:
        SP.dram_hi = arg;
        break;
    case RD_DALSMC_MSG_SetDalDramAddrLow:
        SP.dram_lo = arg;
        break;
    case RD_DALSMC_MSG_TransferTableDram2Smu: {
        const uint8_t *p = mc_ptr((uint64_t)SP.dram_hi << 32 | SP.dram_lo, RD_WM_TABLE_BYTES);
        SP.transfers++;
        SP.table_id = arg;
        if (p)
            memcpy(SP.table, p, RD_WM_TABLE_BYTES);
        else
            SP.table_bad++;
        break;
    }
    case RD_DALSMC_MSG_SetFclkSwitchAllow:
        SP.fclk_allow = arg;
        SP.fclk_msgs++;
        break;
    case RD_DALSMC_MSG_SetCabForUclkPstate:
        SP.cab_ways = arg;
        SP.cab_msgs++;
        break;
    case RD_DALSMC_MSG_BacoAudioD3PME:
        SP.pme++;
        break;
    case RD_DALSMC_MSG_SetMinDeepSleepDcfclk:
        SP.ds_dcfclk = arg;
        break;
    case RD_DALSMC_MSG_SetHardMaxByFreq:
        if (clk < 16)
            SP.hard_max[clk] = arg & 0xffff;
        break;
    case RD_DALSMC_MSG_SetAlwaysWaitDmcubResp:
        SP.always_wait = arg;
        break;
    default:
        known = 0;
    }
    if (!known)
        return 0;
    S.dal_msgs++;
    S.reg[RD_DAL_ARG_REG] = out;
    S.reg[RD_DAL_RESP_REG] = RD_DALSMC_RESULT_OK;
    *R(dw) = v;
    return 1;
}

static int pst_rd(uint32_t dw, uint32_t *out)
{
    uint32_t n;
    if (!SP.on)
        return 0;
    /* a blanked phantom HUBP (MALL_SEL 1) drains its requests at once */
    for (n = 0; n < RD_DCN_MAX_PIPES; n++)
        if (dw == SP_HUBP(regHUBP0_DCHUBP_CNTL, n) &&
            (S.reg[SP_HUBP(regHUBP0_DCHUBP_MALL_CONFIG, n)] & HUBP0_DCHUBP_MALL_CONFIG__USE_MALL_SEL_MASK) == 1 &&
            (S.reg[dw] & HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK)) {
            *out = S.reg[dw] | HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK;
            return 1;
        }
    return 0;
}

/* ---- the underflow check ---- */

/* what the test says a lit head is (the model does not decode timings
 * from the OTG: the pixel clock is the PHY's) */
typedef struct {
    uint32_t pipe, otg, pclk_khz, h_total, v_total, v_active, src_w, bpp;
} sp_head;

/* the heads that could underflow if the PMFW switched UCLK now: it can
 * when the arbiter's allow is forced on, or when UCLK is not pinned at the
 * top level and every pipe's arbiter allows (a buffered time of at least
 * the programmed set A UCLK watermark, in active or in vblank). A pipe is
 * at risk when its DET + line buffer do not hold 'latency_us' and it is
 * not covered: a SubVP main is when a phantom on MALL_SEL 1 and the
 * DMCUB's SubVP command for its OTG bring a MALL region that does. */
static uint32_t sp_underflow_risk(const sp_head *h, uint32_t n, uint32_t latency_us)
{
    uint32_t i, risk = 0, wm_cyc = S.reg[DCN_B2 + regDCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A] & 0xffff;
    uint32_t arb = S.reg[DCN_B2 + regDCHUBBUB_ARB_DRAM_STATE_CNTL];
    int forced = (arb & DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_ENABLE_MASK) &&
                 (arb & DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_VALUE_MASK);
    int pinned = S.dal_hard_min[RD_PPCLK_UCLK] >= SP.uclk[SP_UCLK_LEVELS - 1];
    uint32_t wm_us = wm_cyc / 100;   /* DCHUB REFCLK 100 MHz */
    sp_subvp_cmd cmd;
    int allow_all = 1;
    sp_decode(SP.subvp_cmd, &cmd);
    if (pinned && !forced)
        return 0;
    for (i = 0; i < n && !forced; i++) {
        uint32_t line_ns = (uint32_t)((uint64_t)h[i].h_total * 1000000 / h[i].pclk_khz);
        uint32_t det = ((S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + h[i].pipe] >> 8) & 0x1f) * 65536;
        uint32_t lines = det / (h[i].src_w * h[i].bpp) + (1171920u / 57 / h[i].src_w < 32 ? 1171920u / 57 / h[i].src_w : 32);
        uint32_t act_us = lines * line_ns / 1000, vblank_us = (h[i].v_total - h[i].v_active) * line_ns / 1000;
        int subvp = (S.reg[SP_HUBP(regHUBP0_DCHUBP_VMPG_CONFIG, h[i].pipe)] &
                     HUBP0_DCHUBP_VMPG_CONFIG__FORCE_ONE_ROW_FOR_FRAME_MASK) != 0;
        /* a pipe allows when it holds the watermark; a SubVP main's allow
         * is the firmware's (forced while it runs) */
        if (!subvp && act_us < wm_us && vblank_us < wm_us)
            allow_all = 0;
    }
    if (!allow_all)
        return 0;   /* the arbiter never lets the PMFW switch: UCLK stays where it is */
    for (i = 0; i < n; i++) {
        uint32_t line_ns = (uint32_t)((uint64_t)h[i].h_total * 1000000 / h[i].pclk_khz);
        uint32_t det = ((S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + h[i].pipe] >> 8) & 0x1f) * 65536;
        uint32_t lines = det / (h[i].src_w * h[i].bpp) + (1171920u / 57 / h[i].src_w < 32 ? 1171920u / 57 / h[i].src_w : 32);
        uint32_t act_us = lines * line_ns / 1000, k;
        int covered = act_us >= latency_us;
        if (!covered && !forced && wm_us >= latency_us)
            covered = 1;   /* the arbiter waits for a buffer that holds the switch (vblank) */
        if (!covered && (S.reg[SP_HUBP(regHUBP0_DCHUBP_VMPG_CONFIG, h[i].pipe)] &
                         HUBP0_DCHUBP_VMPG_CONFIG__FORCE_ONE_ROW_FOR_FRAME_MASK))
            for (k = 0; k < 2; k++)
                if (cmd.p[k].mode == RD_MCLK_SWITCH_SUBVP && cmd.p[k].main_idx == h[i].otg &&
                    cmd.p[k].phantom_idx < RD_DCN_MAX_PIPES &&
                    (S.reg[SP_HUBP(regHUBP0_DCHUBP_MALL_CONFIG, cmd.p[k].phantom_idx)] &
                     HUBP0_DCHUBP_MALL_CONFIG__USE_MALL_SEL_MASK) == 1 &&
                    (uint64_t)(cmd.p[k].mall_lines + lines) * line_ns / 1000 >= latency_us)
                    covered = 1;
        if (!covered)
            risk++;
    }
    return risk;
}

/* ---- tests ---- */

static void sp_plane(rdna3_pstate_plane *p, uint32_t pipe, uint32_t pclk, uint32_t ha, uint32_t va, uint32_t ht,
                     uint32_t vt, uint32_t vf, uint32_t vs)
{
    memset(p, 0, sizeof(*p));
    p->pipe = p->otg = pipe;
    p->odm_pipe = RD_DCN_MAX_PIPES;
    p->dpp_count = 1;
    p->pclk_khz = pclk;
    p->h_active = ha;
    p->v_active = va;
    p->h_total = ht;
    p->v_total = vt;
    p->v_front = vf;
    p->v_sync = vs;
    p->src_w = ha;
    p->src_h = va;
    p->bpp = 4;
    p->vstartup = vt - va - 1;
    p->refresh_hz = dcn32_refresh_rate_ceil(p);
}

/* a lit head in the register file: OTG timing, HUBP and its DLG, DET */
static void sp_head_regs(const rdna3_pstate_plane *p)
{
    uint32_t o = p->otg, q = p->pipe;
    S.reg[DCN_B2 + regDOMAIN0_PG_STATUS + 2 * q] = 0;
    S.reg[OTG(regOTG0_OTG_H_TOTAL, o)] = p->h_total - 1;
    S.reg[OTG(regOTG0_OTG_V_TOTAL, o)] = p->v_total - 1;
    S.reg[OTG(regOTG0_OTG_V_BLANK_START_END, o)] =
        (p->v_total - p->v_front) | (p->v_total - p->v_front - p->v_active) << 16;
    S.reg[OTG(regOTG0_OTG_V_SYNC_A, o)] = p->v_sync << 16;
    S.reg[OTG(regOTG0_OTG_VSTARTUP_PARAM, o)] = p->vstartup;
    S.reg[OTG(regOTG0_OTG_CONTROL, o)] = OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    S.reg[DCN_B2 + regODM0_OPTC_DATA_SOURCE_SELECT + RD_CLONE_ODM_STRIDE * o] =
        q << ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL__SHIFT | 0xfu << ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG1_SRC_SEL__SHIFT;
    S.reg[HUBP(regHUBP0_DCHUBP_CNTL, q)] = o << HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT;
    S.reg[HUBP(regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, q)] = p->src_w | p->src_h << 16;
    S.reg[HUBP(regHUBP0_DCSURF_PRI_VIEWPORT_START, q)] = 0;
    S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, q)] = 0x10000000u * (q + 1);
    S.reg[HUBP(regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, q)] = 0x80;
    S.reg[HUBP(regHUBPREQ0_BLANK_OFFSET_0, q)] = 0x100 | (p->v_total - p->v_front - p->v_active) << 16;
    /* MIN_DST_Y_NEXT_START = (VTotal - VFrontPorch + 1) x 4 + TSetup 0.75 line */
    S.reg[HUBP(regHUBPREQ0_BLANK_OFFSET_1, q)] = 4 * (p->v_total - p->v_front + 1) + 3;
    S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + q] = (p->det_kb / 64) | (p->det_kb / 64) << 8;
    /* the sRGB degamma ROM, a GAMCOR curve in use */
    S.reg[DCN_B2 + regCNVC_CFG0_PRE_DEGAM + RD_CLONE_DPP_STRIDE * q] = 1;
    S.reg[DCN_B2 + regCM0_CM_GAMCOR_CONTROL + RD_CLONE_DPP_STRIDE * q] = 1;
}

static void sp_head_of(sp_head *h, const rdna3_pstate_plane *p)
{
    h->pipe = p->pipe;
    h->otg = p->otg;
    h->pclk_khz = p->pclk_khz;
    h->h_total = p->h_total;
    h->v_total = p->v_total;
    h->v_active = p->v_active;
    h->src_w = p->src_w;
    h->bpp = p->bpp;
}

static void test_dc_pstate(rdna3_dev *dev, rdna3_dmub *dm)
{
    static uint32_t save[MMIO_LEN / 4];
    static rdna3_dcn32_power pw;
    static rdna3_pstate_ctx ctx, c2;
    static rdna3_subvp_state cur, next;
    const uint64_t wm_off = BAR_LEN - 0x10000;   /* in the modelled BAR */
    sp_head heads[4];
    sp_subvp_cmd cmd;
    uint32_t k, wmA, dal0;
    unsigned locks0;
    int r;

    memcpy(save, S.reg, sizeof(save));
    memset(&SP, 0, sizeof(SP));
    SP.on = 1;
    /* the RX 7800 XT's levels (amdgpu pp_dpm_*: UCLK 96..1218 MHz) */
    SP.uclk[0] = 96; SP.uclk[1] = 456; SP.uclk[2] = 772; SP.uclk[3] = 1218;
    SP.fclk[0] = 601; SP.fclk[1] = 1000; SP.fclk[2] = 1500; SP.fclk[3] = 1800;
    SP.dcfclk[0] = 506; SP.dcfclk[1] = 1001; SP.dcfclk[2] = 1500; SP.dcfclk[3] = 1800;
    SP.socclk[0] = 478; SP.socclk[1] = 960; SP.socclk[2] = 1200;
    S.dal_dpm_n = 4;
    S.dal_dpm[0] = 358; S.dal_dpm[1] = 716; S.dal_dpm[2] = 1075; S.dal_dpm[3] = 1433;
    SK.dmub_model = 1;
    SK.otg_model = 1;
    CHECK(dm->hw_init && S.dmub.running);

    /* ---- dcn32_init_hw's part ---- */
    S.reg[DCN_B2 + regDCHUBBUB_ARB_DRAM_STATE_CNTL] =
        DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_ENABLE_MASK |
        DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_VALUE_MASK;
    CHECK(dcn32_power_init(dev, &pw, NULL, wm_off, 0) == RDNA3_OK);
    CHECK(pw.bw.smu_present && pw.bw.pmfw == 0x00505c00 && pw.bw.n_memclk == 4 && pw.bw.memclk_mhz[3] == 1218 &&
          pw.bw.n_fclk == 4 && pw.bw.n_dcfclk == 4 && pw.bw.n_dtbclk == 2 && pw.bw.n_dispclk == 4 && pw.bw.nstates >= 4);
    /* the states: sorted by bandwidth, the lowest at the lowest UCLK */
    CHECK(pw.bw.state[0].dram_mts == 96 * 16 && pw.bw.state[pw.bw.nstates - 1].dram_mts == 1218 * 16);
    for (k = 1; k < pw.bw.nstates; k++)
        CHECK(pw.bw.state[k].net_bw_kbps >= pw.bw.state[k - 1].net_bw_kbps);
    /* the WatermarksExternal_t the PMFW fetched: A, B clock range, C dummy
     * p-state, D MALL, nothing else */
    CHECK(SP.transfers == 1 && SP.table_bad == 0 && SP.table_id == RD_DAL_TABLE_WATERMARKS &&
          ((uint64_t)SP.dram_hi << 32 | SP.dram_lo) == FB_START + wm_off);
    CHECK(SP.table[0] == 0 && SP.table[1] == RD_WATERMARKS_CLOCK_RANGE && SP.table[4] == 1 &&
          SP.table[5] == RD_WATERMARKS_CLOCK_RANGE && SP.table[8] == 2 && SP.table[9] == RD_WATERMARKS_DUMMY_PSTATE &&
          SP.table[12] == 3 && SP.table[13] == RD_WATERMARKS_MALL);
    for (k = 16; k < RD_WM_TABLE_BYTES; k++)
        CHECK(SP.table[k] == 0);
    /* UCLK hard max at the top, the arbiter's force cleared, the minimum
     * clocks (DCFCLK and UCLK at level 0, deep sleep 10 MHz; FCLK switching
     * starts as supported: no message) */
    CHECK(SP.hard_max[RD_PPCLK_UCLK] == 1218);
    CHECK(!(S.reg[DCN_B2 + regDCHUBBUB_ARB_DRAM_STATE_CNTL] &
            DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_ENABLE_MASK));
    CHECK(S.dal_hard_min[RD_PPCLK_DCFCLK] == 506 && S.dal_hard_min[RD_PPCLK_UCLK] == 96 && SP.ds_dcfclk == 10);
    /* the PME workaround (the audio path's) */
    CHECK(dcn32_enable_pme_wa(dev, &pw.bw) == RDNA3_OK && SP.pme == 1);

    /* ---- two 1080p60 heads: vactive switching at the lowest state ---- */
    memset(&ctx, 0, sizeof(ctx));
    ctx.nplanes = 2;
    sp_plane(&ctx.plane[0], 0, 148500, 1920, 1080, 2200, 1125, 4, 5);
    sp_plane(&ctx.plane[1], 1, 148500, 1920, 1080, 2200, 1125, 4, 5);
    ctx.dispclk_khz = ctx.dppclk_khz = 600000;
    dcn32_set_det_allocations(&ctx);
    CHECK(ctx.det_segs[0] == 9 && ctx.det_segs[1] == 9 && ctx.compbuf_kb == 1280 - 18 * 64);
    CHECK(dcn32_pstate_validate(&pw.soc, &pw.bw, &ctx) == RDNA3_OK);
    printf("sim_navi32: pstate 2x1080p60: state %u %s margins %d/%d us, WM A urgent %u p-state %u sr %u ns\n", ctx.vlevel,
           dcn32_dram_change_name(ctx.dram_change), ctx.dml.active_margin_ns[0] / 1000, ctx.dml.active_margin_ns[1] / 1000,
           ctx.wm[RD_WM_A].urgent_ns, ctx.wm[RD_WM_A].pstate_change_ns, ctx.wm[RD_WM_A].cstate_enter_plus_exit_ns);
    CHECK(ctx.vlevel == 0 && ctx.p_state_change_support && ctx.dram_change == RD_DRAM_CHANGE_VACTIVE &&
          ctx.fclk_p_state_change_support && ctx.dml.active_margin_ns[0] > 0);
    /* DC's set A on the card: urgent 17.32 us, p-state 417.32 us */
    CHECK(ctx.wm[RD_WM_A].pstate_change_ns > 400000 && ctx.wm[RD_WM_A].pstate_change_ns < 440000 &&
          ctx.wm[RD_WM_A].urgent_ns > 4000 && ctx.wm[RD_WM_A].urgent_ns < 40000);
    /* no SubVP for a configuration that switches */
    CHECK(dcn32_subvp_validate(&pw.soc, &pw.bw, &ctx, 0, 0, &next) == 0 && next.n == 0);
    sp_head_regs(&ctx.plane[0]);
    sp_head_regs(&ctx.plane[1]);
    sp_head_of(&heads[0], &ctx.plane[0]);
    sp_head_of(&heads[1], &ctx.plane[1]);
    SP.fams_setup = 0;
    CHECK(dcn32_power_apply(dev, dm, &pw, &ctx) == RDNA3_OK);
    wmA = S.reg[DCN_B2 + regDCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A] & 0xffff;
    CHECK(wmA == (ctx.wm[RD_WM_A].pstate_change_ns * 100 + 999) / 1000 ||
          wmA == ctx.wm[RD_WM_A].pstate_change_ns * 100 / 1000);
    CHECK(S.dal_hard_min[RD_PPCLK_UCLK] == SP.uclk[0] && SP.fams_setup == 1 && SP.always_wait == 0);
    CHECK((S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL] & 0x1f) == 9 && (S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + 1] & 0x1f) == 9);
    CHECK(sp_underflow_risk(heads, 2, 400) == 0);
    /* the arbiter forced to allow while a plane cannot hide the switch: the model sees the risk */
    S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + 1] = 0x101;
    S.reg[DCN_B2 + regDCHUBBUB_ARB_DRAM_STATE_CNTL] |=
        DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_ENABLE_MASK |
        DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_VALUE_MASK;
    CHECK(sp_underflow_risk(heads, 2, 400) == 1);
    S.reg[DCN_B2 + regDCHUBBUB_ARB_DRAM_STATE_CNTL] &=
        ~(DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_ENABLE_MASK |
          DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_VALUE_MASK);
    S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + 1] = 0x909;

    /* idle: CAB. 2 x 8 MB surfaces do not fit the 64 MB / 16 ways? They
     * do: the ways the DMCUB gets; all heads blanked: no DCN requests */
    {
        uint32_t ways = dcn32_calculate_cab_allocation(&pw.soc, &ctx);
        SP.cab_cmds = 0;
        CHECK(ways > 0 && ways <= 16);
        CHECK(dcn32_apply_idle_power_optimizations(dm, &pw.soc, &ctx, 1) == 1 && SP.cab_cmds == 1 &&
              SP.cab_sub == RD_DMUB_CAB_DCN_SS_FIT_IN_CAB && SP.cab_ways_dmub == ways);
        CHECK(dcn32_apply_idle_power_optimizations(dm, &pw.soc, &ctx, 0) == 1 && SP.cab_cmds == 2 &&
              SP.cab_sub == RD_DMUB_CAB_NO_IDLE_OPTIMIZATION);
        c2 = ctx;
        c2.plane[0].blanked = c2.plane[1].blanked = 1;
        CHECK(dcn32_check_no_memory_request_for_cab(&c2) &&
              dcn32_apply_idle_power_optimizations(dm, &pw.soc, &c2, 1) == 1 && SP.cab_sub == RD_DMUB_CAB_NO_DCN_REQ);
        /* a cursor bigger than its 16 KiB buffer: one 64 KiB block extra, cached in the MALL */
        CHECK(dcn32_helper_calculate_mall_bytes_for_cursor(256 * 256 * 4, 0) == (4 + 1) * 65536u &&
              dcn32_helper_calculate_mall_bytes_for_cursor(64 * 64 * 4, 0) == 0);
    }

    /* the mirror's forced allow kept (rdna3pstate): only vactive is safe.
     * A 1080p60 head with one DET segment hides nothing in active: UCLK
     * stays pinned */
    c2 = ctx;
    c2.nplanes = 1;
    c2.plane[0].det_kb = 64;
    c2.compbuf_kb = 0;
    CHECK(dcn32_pstate_validate(&pw.soc, &pw.bw, &c2) == RDNA3_OK);
    printf("sim_navi32: pstate 1080p60 DET 64 KiB: state %u %s margin %d us\n", c2.vlevel,
           dcn32_dram_change_name(c2.dram_change), c2.dml.active_margin_ns[0] / 1000);
    CHECK(c2.dram_change == RD_DRAM_CHANGE_VBLANK && c2.p_state_change_support);
    pw.keep_force = 1;
    CHECK(dcn32_power_prepare(dev, dm, &pw, &c2) == RDNA3_OK && !c2.p_state_change_support &&
          S.dal_hard_min[RD_PPCLK_UCLK] == 1218);
    pw.keep_force = 0;
    CHECK(dcn32_power_apply(dev, dm, &pw, &ctx) == RDNA3_OK && S.dal_hard_min[RD_PPCLK_UCLK] == SP.uclk[0]);

    /* ---- one 3840x2160 head at 144 Hz: no natural switch, SubVP ---- */
    memset(&ctx, 0, sizeof(ctx));
    ctx.nplanes = 1;
    sp_plane(&ctx.plane[0], 0, 1290800, 3840, 2160, 3920, 2287, 3, 5);
    ctx.dispclk_khz = ctx.dppclk_khz = 1400000;
    dcn32_set_det_allocations(&ctx);
    CHECK(dcn32_pstate_validate(&pw.soc, &pw.bw, &ctx) == RDNA3_OK);
    printf("sim_navi32: pstate 4K144: state %u %s margin %d us, SubVP lines %u\n", ctx.vlevel,
           dcn32_dram_change_name(ctx.dram_change), ctx.dml.active_margin_ns[0] / 1000, ctx.dml.subvp_lines[0]);
    CHECK(ctx.dram_change == RD_DRAM_CHANGE_UNSUPPORTED && !ctx.p_state_change_support &&
          ctx.vlevel == pw.bw.nstates - 1);
    c2 = ctx;
    CHECK(dcn32_subvp_validate(&pw.soc, &pw.bw, &c2, 0, 1, &next) == 0);   /* GECC on: never */
    CHECK(dcn32_subvp_validate(&pw.soc, &pw.bw, &c2, 0, 0, &next) == 1);
    printf("sim_navi32: pstate 4K144 SubVP: state %u %s, phantom %ux%u vtotal %u on pipe %u, %u ways\n", c2.vlevel,
           dcn32_dram_change_name(c2.dram_change), c2.plane[1].h_active, c2.plane[1].v_active, c2.plane[1].v_total,
           next.ph[0].pipe, c2.num_ways);
    CHECK(next.n == 1 && c2.nplanes == 2 && c2.subvp && c2.plane[0].mall == RD_MALL_PSTATE_SUB_VIEWPORT &&
          c2.plane[1].mall == RD_MALL_PSTATE_PHANTOM && c2.plane[0].paired == 2 && c2.plane[1].paired == 1);
    CHECK(c2.p_state_change_support && c2.vlevel < ctx.vlevel &&
          pw.bw.state[c2.vlevel].dram_mts < pw.bw.state[ctx.vlevel].dram_mts);
    /* the phantom: the lowest free pipe; lines = MALL lines + (15 + 20) us + 16,
     * front porch 1, back porch the main's VSTARTUP */
    {
        const rdna3_pstate_plane *m = &c2.plane[0], *p = &c2.plane[1];
        uint32_t delay = (uint32_t)(35ull * m->pclk_khz / 1000 / m->h_total);
        CHECK(next.ph[0].pipe == 1 && p->pipe == 1 && p->otg == 1 && p->det_kb == 0 && c2.det_segs[1] == 0);
        CHECK(p->v_active == ctx.dml.subvp_lines[0] + delay + 16 && p->v_front == 1 &&
              p->v_total == p->v_active + 1 + m->v_sync + m->vstartup && p->vstartup == p->v_total - p->v_active - 1);
        CHECK(c2.det_segs[0] == 18 && c2.dcfclk_khz >= 400000 && c2.num_ways > 0);
    }
    /* no free pipe (1..3 taken elsewhere): none */
    CHECK(dcn32_subvp_validate(&pw.soc, &pw.bw, &ctx, 0xe, 0, &next) == 0 && next.n == 0 && ctx.nplanes == 1);

    /* the bring-up: HW lock, phantom up, the DMCUB command, unlock */
    ctx.plane[0].det_kb = 18 * 64;
    sp_head_regs(&ctx.plane[0]);
    S.reg[OTG(regOTG0_OTG_CONTROL, 1)] = 0;
    S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] = HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
    S.reg[DCN_B2 + regDOMAIN0_PG_STATUS + 2 * 1] = 2u << DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS__SHIFT;
    S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + 1] = 0x909;
    S.reg[DCN_B3 + regMPCC_MCM0_MPCC_MCM_3DLUT_MODE + 0x90] = 2;
    S.reg[DCN_B3 + regMPCC_MCM0_MPCC_MCM_SHAPER_CONTROL + 0x90] = 1;
    memset(&cur, 0, sizeof(cur));
    CHECK(dcn32_subvp_validate(&pw.soc, &pw.bw, &ctx, 0, 0, &next) == 1);
    sp_head_of(&heads[0], &ctx.plane[0]);
    locks0 = SK.inbox0_cmds;
    SP.subvp_cmds = 0;
    dal0 = S.dal_msgs;
    r = dcn32_subvp_commit(dev, dm, &pw, &ctx, &cur, &next, 600000);
    CHECK(r == RDNA3_OK && cur.enabled && cur.n == 1 && S.dal_msgs > dal0);
    CHECK(SK.inbox0_cmds == locks0 + 2 && SK.inbox0_last == dmub32_inbox0_hw_lock_word(0) && cur.locks == 2);
    CHECK(SP.subvp_cmds == 1);
    sp_decode(SP.subvp_cmd, &cmd);
    {
        const rdna3_pstate_plane *m = &ctx.plane[0], *p = &ctx.plane[1];
        uint32_t l15 = (uint32_t)((15ull * m->pclk_khz * 1000 + m->h_total * 1000000ull - 1) / (m->h_total * 1000000ull));
        CHECK(cmd.p[0].mode == RD_MCLK_SWITCH_SUBVP && cmd.p[1].mode == 0 && cmd.p[0].pix_clk_100hz == 12908000 &&
              cmd.p[0].main_idx == 0 && cmd.p[0].phantom_idx == 1 && cmd.p[0].mall_lines == p->v_active &&
              cmd.p[0].prefetch_lines == p->v_total - 1 && cmd.p[0].vblank_start == 2287 - 3 &&
              cmd.p[0].vblank_end == 2287 - 3 - 2160 && cmd.p[0].htotal == 3920 && cmd.p[0].vtotal == 2287 &&
              cmd.p[0].num == 1 && cmd.p[0].den == 1);
        /* 15 us in lines at 3.04 us per line, rounded up: 5 */
        CHECK(l15 == 5 && cmd.p[0].p2m_lines == l15 && cmd.p[0].delay_lines == l15);
        CHECK(cmd.allow_width == 20 && cmd.vint_margin == 30 &&
              cmd.wm_a_cache == (ctx.wm[RD_WM_A].pstate_change_ns * 100 / 1000 < 0xffff
                                     ? ctx.wm[RD_WM_A].pstate_change_ns * 100 / 1000 : 0xffff));
        /* the phantom pipe: powered, DET 0, MALL_SEL 1, blanked, its OTG
         * off again (post-enable) with the phantom timing, the DP DTO at
         * the main's pixel clock, its own MPC tree and OPP, the DPG black */
        CHECK(!(S.reg[DCN_B2 + regDOMAIN0_PG_STATUS + 2] & DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS_MASK));
        CHECK((S.reg[DCN_B2 + regDCHUBBUB_DET0_CTRL + 1] & 0x1f) == 0);
        CHECK((S.reg[HUBP(regHUBP0_DCHUBP_MALL_CONFIG, 1)] & 3) == 1 &&
              (S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] & HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK) &&
              ((S.reg[HUBP(regHUBP0_DCHUBP_CNTL, 1)] & HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL_MASK) >>
               HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT) == 1);
        CHECK(!(S.reg[OTG(regOTG0_OTG_CONTROL, 1)] & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) &&
              S.reg[OTG(regOTG0_OTG_V_TOTAL, 1)] == p->v_total - 1 &&
              S.reg[OTG(regOTG0_OTG_V_BLANK_START_END, 1)] ==
                  ((p->v_total - 1) | (p->v_total - 1 - p->v_active) << 16) &&
              (S.reg[OTG(regOTG0_OTG_VSTARTUP_PARAM, 1)] & 0x3ff) == p->vstartup &&
              S.reg[OTG(regOTG0_OTG_H_TOTAL, 1)] == 3920 - 1);
        CHECK(S.reg[DCN_B1 + regDP_DTO0_PHASE + (regDP_DTO1_PHASE - regDP_DTO0_PHASE)] == 1290800u * 1000 &&
              S.reg[DCN_B1 + regDP_DTO0_MODULO + (regDP_DTO1_PHASE - regDP_DTO0_PHASE)] == 600000u * 1000 &&
              (S.reg[DCN_B1 + regOTG0_PIXEL_RATE_CNTL + (regOTG1_PIXEL_RATE_CNTL - regOTG0_PIXEL_RATE_CNTL)] &
               OTG0_PIXEL_RATE_CNTL__DP_DTO0_ENABLE_MASK));
        CHECK(((S.reg[DCN_B2 + regODM0_OPTC_DATA_SOURCE_SELECT + RD_CLONE_ODM_STRIDE] &
                ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL_MASK) >> ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL__SHIFT) == 1);
        CHECK((S.reg[SP_OPP(regDPG0_DPG_CONTROL, 1)] & DPG0_DPG_CONTROL__DPG_EN_MASK) &&
              S.reg[SP_OPP(regDPG0_DPG_DIMENSIONS, 1)] == (3840u << 16 | p->v_active));
        /* the input color of a copy of the main plane: its degamma ROM, no
         * GAMCOR curve, the MCM LUTs in bypass */
        CHECK(S.reg[DCN_B2 + regCNVC_CFG0_PRE_DEGAM + RD_CLONE_DPP_STRIDE] == 1 &&
              !(S.reg[DCN_B2 + regCM0_CM_GAMCOR_CONTROL + RD_CLONE_DPP_STRIDE] & CM0_CM_GAMCOR_CONTROL__CM_GAMCOR_MODE_MASK) &&
              S.reg[DCN_B3 + regMPCC_MCM0_MPCC_MCM_3DLUT_MODE + 0x90] == 0 &&
              S.reg[DCN_B3 + regMPCC_MCM0_MPCC_MCM_SHAPER_CONTROL + 0x90] == 0 &&
              S.reg[DCN_B3 + regMPCC_MCM0_MPCC_MCM_1DLUT_CONTROL + 0x90] == 0);
        CHECK(dcn32_pre_degam_tf(1) == RD_TF_SRGB && dcn32_pre_degam_tf(0x51) == RD_TF_PQ &&
              dcn32_pre_degam_tf(0) == RD_TF_UNITY && dcn32_set_mpc_shaper_3dlut(dev, 1, 0) == 0);
        CHECK((S.reg[HUBP(regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, 1)] >> 16) == p->v_active &&
              ((S.reg[HUBP(regHUBPREQ0_BLANK_OFFSET_0, 1)] >> 16) & 0x7fff) == p->v_total - 1 - p->v_active &&
              (S.reg[HUBP(regHUBPREQ0_BLANK_OFFSET_1, 1)] & 0x3ffff) == 4 * (p->v_total - 1 + 1) + 3);
        /* the main: FORCE_ONE_ROW_FOR_FRAME, the cursor off the MALL, p-state
         * allow left to the firmware (force disallow) */
        CHECK((S.reg[HUBP(regHUBP0_DCHUBP_VMPG_CONFIG, 0)] & HUBP0_DCHUBP_VMPG_CONFIG__FORCE_ONE_ROW_FOR_FRAME_MASK) &&
              (S.reg[HUBP(regCURSOR0_0_CURSOR_CONTROL, 0)] & CURSOR0_0_CURSOR_CONTROL__CURSOR_REQ_MODE_MASK) &&
              (S.reg[HUBP(regHUBPREQ0_UCLK_PSTATE_FORCE, 0)] & HUBPREQ0_UCLK_PSTATE_FORCE__DATA_UCLK_PSTATE_FORCE_EN_MASK) &&
              !(S.reg[HUBP(regHUBPREQ0_UCLK_PSTATE_FORCE, 0)] & HUBPREQ0_UCLK_PSTATE_FORCE__DATA_UCLK_PSTATE_FORCE_VALUE_MASK));
        /* UCLK let down to the SubVP state, set A's p-state the real one again */
        CHECK(S.dal_hard_min[RD_PPCLK_UCLK] == pw.bw.state[ctx.vlevel].dram_mts / 16 &&
              S.dal_hard_min[RD_PPCLK_UCLK] < 1218);
        CHECK((S.reg[DCN_B2 + regDCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A] & 0xffff) < 0xffff);
        heads[0].v_total = m->v_total;
        CHECK(sp_underflow_risk(heads, 1, 400) == 0);
        /* the same configuration again: nothing re-programmed, the
         * firmware told again under the lock */
        next = cur;
        CHECK(dcn32_subvp_commit(dev, dm, &pw, &ctx, &cur, &next, 600000) == RDNA3_OK && SP.subvp_cmds == 2 &&
              cur.enabled && cur.locks == 4);
        /* a flip of the main: the address for the firmware, toggling the select */
        CHECK(dcn32_subvp_index_of(&cur, 0) == 0 && dcn32_subvp_index_of(&cur, 2) == -1);
        S.reg[DMR(regDMCUB_SCRATCH15)] = 0;
        dmub_dcn32_save_surf_addr(dev, 0x8012345000ull, 0, 0);
        CHECK(S.reg[DMR(regDMCUB_SCRATCH12)] == 0x12345000u && S.reg[DMR(regDMCUB_SCRATCH15)] == 1);
        dmub_dcn32_save_surf_addr(dev, 0x8022345000ull, 0, 0);
        CHECK(S.reg[DMR(regDMCUB_SCRATCH9)] == 0x22345000u && S.reg[DMR(regDMCUB_SCRATCH15)] == 0);
        dmub_dcn32_save_surf_addr(dev, 0x8032345000ull, 0, 1);
        CHECK(S.reg[DMR(regDMCUB_SCRATCH20)] == 0x32345000u && S.reg[DMR(regDMCUB_SCRATCH23)] == 1);
        /* the phantom follows the main's viewport position */
        dcn32_update_phantom_vp_position(dev, &cur.ph[0], 8);
        CHECK(((S.reg[HUBP(regHUBP0_DCSURF_PRI_VIEWPORT_START, 1)] >> 16) & 0x3fff) == 8 && cur.ph[0].vp_y == 8);
    }
    /* topology: the same phantom of the same main is seamless */
    CHECK(dcn32_is_pipe_topology_transition_seamless(&ctx, &ctx));
    c2 = ctx;
    c2.plane[0].v_active = 1080;
    CHECK(!dcn32_is_pipe_topology_transition_seamless(&ctx, &c2));
    CHECK(dcn32_apply_update_flags_for_phantom(1) == 0x7ff && dcn32_apply_update_flags_for_phantom(0) == 0x1ff);

    /* SubVP off: the phantom gone (gated, MALL_SEL 0), the header-only
     * command, the main's buffering back, UCLK pinned (no switch) */
    memset(&c2, 0, sizeof(c2));
    c2.nplanes = 1;
    c2.plane[0] = ctx.plane[0];
    c2.plane[0].mall = RD_MALL_PSTATE_DISABLE;
    c2.plane[0].paired = 0;
    c2.dispclk_khz = c2.dppclk_khz = 1400000;
    dcn32_set_det_allocations(&c2);
    CHECK(dcn32_pstate_validate(&pw.soc, &pw.bw, &c2) == RDNA3_OK);
    memset(&next, 0, sizeof(next));
    CHECK(dcn32_subvp_commit(dev, dm, &pw, &c2, &cur, &next, 600000) == RDNA3_OK && !cur.enabled && SP.subvp_cmds == 3);
    sp_decode(SP.subvp_cmd, &cmd);
    CHECK(cmd.p[0].mode == 0 && cmd.wm_a_cache == 0 && SP.subvp_cmd[0] == RD_DMUB_CMD_FW_ASSISTED_MCLK_SWITCH);
    CHECK((S.reg[DCN_B2 + regDOMAIN0_PG_STATUS + 2] & DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS_MASK) &&
          (S.reg[HUBP(regHUBP0_DCHUBP_MALL_CONFIG, 1)] & 3) == 0 &&
          !(S.reg[OTG(regOTG0_OTG_CONTROL, 1)] & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) &&
          ((S.reg[DCN_B2 + regODM0_OPTC_DATA_SOURCE_SELECT + RD_CLONE_ODM_STRIDE] &
            ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL_MASK) >> ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL__SHIFT) == 0xf &&
          !(S.reg[SP_OPP(regDPG0_DPG_CONTROL, 1)] & DPG0_DPG_CONTROL__DPG_EN_MASK));
    CHECK(!(S.reg[HUBP(regHUBP0_DCHUBP_VMPG_CONFIG, 0)] & HUBP0_DCHUBP_VMPG_CONFIG__FORCE_ONE_ROW_FOR_FRAME_MASK) &&
          !(S.reg[HUBP(regHUBPREQ0_UCLK_PSTATE_FORCE, 0)] & HUBPREQ0_UCLK_PSTATE_FORCE__DATA_UCLK_PSTATE_FORCE_EN_MASK));
    CHECK(S.dal_hard_min[RD_PPCLK_UCLK] == 1218 && sp_underflow_risk(heads, 1, 400) == 0);

    /* ---- two unsynchronised 4K60 heads: SubVP on both ---- */
    memset(&ctx, 0, sizeof(ctx));
    ctx.nplanes = 2;
    sp_plane(&ctx.plane[0], 0, 594000, 3840, 2160, 4400, 2250, 8, 10);
    sp_plane(&ctx.plane[1], 1, 594000, 3840, 2160, 4400, 2250, 8, 10);
    ctx.dispclk_khz = ctx.dppclk_khz = 1000000;
    dcn32_set_det_allocations(&ctx);
    CHECK(dcn32_pstate_validate(&pw.soc, &pw.bw, &ctx) == RDNA3_OK);
    printf("sim_navi32: pstate 2x4K60: state %u %s margins %d/%d us\n", ctx.vlevel, dcn32_dram_change_name(ctx.dram_change),
           ctx.dml.active_margin_ns[0] / 1000, ctx.dml.active_margin_ns[1] / 1000);
    CHECK(ctx.dram_change == RD_DRAM_CHANGE_UNSUPPORTED);
    c2 = ctx;
    r = dcn32_subvp_validate(&pw.soc, &pw.bw, &c2, 0, 0, &next);
    printf("sim_navi32: pstate 2x4K60 SubVP: %d, %u phantoms, state %u %s\n", r, next.n, c2.vlevel,
           dcn32_dram_change_name(c2.dram_change));
    CHECK(r == 1 && next.n == 2 && c2.nplanes == 4 && next.ph[0].pipe == 2 && next.ph[1].pipe == 3 &&
          c2.dram_change == RD_DRAM_CHANGE_VACTIVE_W_MALL_SUB_VP && next.ph[0].subvp_index == 0 &&
          next.ph[1].subvp_index == 1);
    {
        uint8_t raw[RD_DMUB_RB_CMD_SIZE];
        dcn32_subvp_build_cmd(&c2, &next, 100, 1, raw);
        sp_decode(raw, &cmd);
        CHECK(cmd.p[0].mode == RD_MCLK_SWITCH_SUBVP && cmd.p[1].mode == RD_MCLK_SWITCH_SUBVP &&
              cmd.p[0].main_idx == 0 && cmd.p[1].main_idx == 1 && cmd.p[0].phantom_idx == 2 && cmd.p[1].phantom_idx == 3);
        /* identical phantoms: no prefetch lengthened */
        CHECK(cmd.p[0].p2m_lines == cmd.p[1].p2m_lines);
    }
    /* the same two heads with one pipe taken elsewhere: one SubVP + vblank,
     * not schedulable at 60 Hz: none */
    c2 = ctx;
    CHECK(dcn32_subvp_validate(&pw.soc, &pw.bw, &c2, 0x8, 0, &next) == 0 && c2.nplanes == 2);

    /* the mirror's forces kept (keep_force): the arbiter's p-state allow
     * and the USR retraining allow survive the init and the updates, and
     * a configuration that is not vactive keeps UCLK pinned */
    {
        const uint32_t usr = DCHUBBUB_ARB_USR_RETRAINING_CNTL__DCHUBBUB_ARB_ALLOW_USR_RETRAINING_FORCE_VALUE_MASK |
                             DCHUBBUB_ARB_USR_RETRAINING_CNTL__DCHUBBUB_ARB_ALLOW_USR_RETRAINING_FORCE_ENABLE_MASK;
        const uint32_t pst = DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_ENABLE_MASK |
                             DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_VALUE_MASK;
        S.reg[DCN_B2 + regDCHUBBUB_ARB_USR_RETRAINING_CNTL] |= usr;
        S.reg[DCN_B2 + regDCHUBBUB_ARB_DRAM_STATE_CNTL] |= pst;
        CHECK(dcn32_power_init(dev, &pw, NULL, wm_off, 1) == RDNA3_OK && pw.keep_force && pw.hubbub.usr_force_keep);
        CHECK(dcn32_power_apply(dev, dm, &pw, &ctx) == RDNA3_OK);
        CHECK((S.reg[DCN_B2 + regDCHUBBUB_ARB_USR_RETRAINING_CNTL] & usr) == usr &&
              (S.reg[DCN_B2 + regDCHUBBUB_ARB_DRAM_STATE_CNTL] & pst) == pst && S.dal_hard_min[RD_PPCLK_UCLK] == 1218);
    }

    printf("sim_navi32: display power tests ran (%u DAL messages, %u SubVP commands, %u CAB commands)\n", S.dal_msgs,
           SP.subvp_cmds, SP.cab_cmds);
    {
        /* the register file as before, but the DMCUB ring where it is now
         * (the driver's inbox1 pointer moved on) */
        uint32_t w = S.reg[DMR(regDMCUB_INBOX1_WPTR)], rp = S.reg[DMR(regDMCUB_INBOX1_RPTR)];
        memcpy(S.reg, save, sizeof(save));
        S.reg[DMR(regDMCUB_INBOX1_WPTR)] = w;
        S.reg[DMR(regDMCUB_INBOX1_RPTR)] = rp;
    }
    SP.on = 0;
    SK.dmub_model = 0;
    SK.otg_model = 0;
    S.dal_dpm_n = 0;
    memset(S.dal_hard_min, 0, sizeof(S.dal_hard_min));
}

/* DRIVER-PLAN N74 (bare metal 2026-11-01, round 3 item 13): the default
 * path (rdna3mall 0) programs none of DC's DETs or watermarks; two lit
 * unsynchronised heads must get UCLK's hard minimum at the top level
 * (dcn32_update_clocks with p_state_change_support false), one head the
 * lowest level back. The underflow model says which heads the PMFW's
 * switch would hit: the GOP's small DETs, the arbiter allowing. */
static void test_uclk_pin(rdna3_dev *dev)
{
    static uint32_t save[MMIO_LEN / 4];
    static rdna3_uclk_pin u;
    rdna3_pstate_plane p[2];
    sp_head heads[2];
    uint32_t why = 99, msgs;
    int sent = 9, k;

    memcpy(save, S.reg, sizeof(save));
    memset(&SP, 0, sizeof(SP));
    memset(S.dal_hard_min, 0, sizeof(S.dal_hard_min));
    S.reg[RD_DAL_RESP_REG] = RD_DALSMC_RESULT_OK;   /* the PMFW's idle state */

    /* the decision alone */
    CHECK(dcn32_uclk_pin_decide(0, 0, 0, &why) == 1 && why == RD_UCLK_WHY_NONE);
    CHECK(dcn32_uclk_pin_decide(1, 0, 0, &why) == 1 && why == RD_UCLK_WHY_ONE);
    CHECK(dcn32_uclk_pin_decide(2, 0, 0, &why) == 0 && why == RD_UCLK_WHY_UNSYNC);
    CHECK(dcn32_uclk_pin_decide(3, 0, 0, &why) == 0 && why == RD_UCLK_WHY_UNSYNC);
    CHECK(dcn32_uclk_pin_decide(2, 1, 0, &why) == 1 && why == RD_UCLK_WHY_SYNC);
    CHECK(dcn32_uclk_pin_decide(2, 0, 1, &why) == 1 && why == RD_UCLK_WHY_OFF);
    CHECK(dcn32_uclk_pin_decide(2, 0, 0, NULL) == 0);
    /* the lit count: two heads; head 1 in standby (rdna3dpms) -> one; the
     * raise before its unblank counts it; a failed unblank does not; the
     * rdna3mirror=1 pipe adds one */
    CHECK(dcn32_uclk_pin_lit(2, 0, 0, 0, 0) == 2);
    CHECK(dcn32_uclk_pin_lit(2, 0x2, 0, 0, 0) == 1);
    CHECK(dcn32_uclk_pin_lit(2, 0x2, 0x2, 0, 0) == 2);
    CHECK(dcn32_uclk_pin_lit(2, 0x2, 0x2, 0x2, 0) == 1);
    CHECK(dcn32_uclk_pin_lit(2, 0, 0, 0x2, 0) == 1);
    CHECK(dcn32_uclk_pin_lit(1, 0, 0, 0, 1) == 2);
    CHECK(dcn32_uclk_pin_lit(0, 0, 0, 0, 0) == 0);
    CHECK(dcn32_uclk_pin_decide(dcn32_uclk_pin_lit(2, 0x2, 0x2, 0, 0), 0, 0, NULL) == 0);
    for (why = 0; why < RD_UCLK_WHY_COUNT; why++)
        CHECK(strcmp(dcn32_uclk_pin_why_name(why), "?") != 0);
    CHECK(strcmp(dcn32_uclk_pin_why_name(RD_UCLK_WHY_COUNT), "?") == 0);

    /* no UCLK levels (the PMFW does not list them): never ready, nothing sent */
    msgs = S.dal_msgs;
    CHECK(dcn32_uclk_pin_init(dev, &u, 0) == RDNA3_ENODEV && !u.ready);
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_ENODEV && sent == 0 &&
          S.dal_hard_min[RD_PPCLK_UCLK] == 0 && u.decisions == 1 && u.why == RD_UCLK_WHY_UNSYNC);

    /* the RX 7800 XT's UCLK levels */
    SP.on = 1;
    SP.uclk[0] = 96; SP.uclk[1] = 456; SP.uclk[2] = 772; SP.uclk[3] = 1218;
    S.reg[RD_DAL_RESP_REG] = RD_DALSMC_RESULT_OK;
    msgs = S.dal_msgs;
    CHECK(dcn32_uclk_pin_init(dev, &u, 0) == RDNA3_OK && u.ready && u.pmfw == 0x00505c00 && u.n_memclk == 4 &&
          u.min_mhz == 96 && u.max_mhz == 1218 && u.p_state_change_support && !u.resync);
    /* the init only reads: no minimum sent */
    CHECK(S.dal_hard_min[RD_PPCLK_UCLK] == 0 && S.dal_msgs > msgs);

    /* two lit 1080p60 heads as the GOP / the clone left them: one DET
     * segment each, the set A UCLK watermark 0 (every pipe allows) */
    sp_plane(&p[0], 0, 148500, 1920, 1080, 2200, 1125, 4, 5);
    sp_plane(&p[1], 1, 148500, 1920, 1080, 2200, 1125, 4, 5);
    p[0].det_kb = p[1].det_kb = 64;
    for (k = 0; k < 2; k++) {
        sp_head_regs(&p[k]);
        sp_head_of(&heads[k], &p[k]);
        S.reg[SP_HUBP(regHUBP0_DCHUBP_VMPG_CONFIG, k)] = 0;
    }
    S.reg[DCN_B2 + regDCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A] = 0;
    S.reg[DCN_B2 + regDCHUBBUB_ARB_DRAM_STATE_CNTL] = 0;

    /* one head: switching supported, as before: nothing sent */
    msgs = S.dal_msgs;
    CHECK(dcn32_uclk_pin_update(dev, &u, 1, 0, 0, &sent) == RDNA3_OK && sent == 0 && S.dal_msgs == msgs &&
          S.dal_hard_min[RD_PPCLK_UCLK] == 0 && u.why == RD_UCLK_WHY_ONE);
    /* the second head lit: the PMFW's switch would hit both */
    CHECK(sp_underflow_risk(heads, 2, 400) == 2);
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && sent == 1);
    CHECK(S.dal_hard_min[RD_PPCLK_UCLK] == 1218 && !u.p_state_change_support && u.hard_min_mhz == 1218 &&
          u.pins == 1 && u.releases == 0 && u.fails == 0 && u.why == RD_UCLK_WHY_UNSYNC && u.lit == 2);
    CHECK(sp_underflow_risk(heads, 2, 400) == 0);
    /* the same again (a mode set on one of them): nothing new */
    msgs = S.dal_msgs;
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && sent == 0 && S.dal_msgs == msgs);
    /* back to one head: the lowest level as the minimum again */
    CHECK(dcn32_uclk_pin_update(dev, &u, 1, 0, 0, &sent) == RDNA3_OK && sent == 1);
    CHECK(S.dal_hard_min[RD_PPCLK_UCLK] == 96 && u.p_state_change_support && u.releases == 1 && u.hard_min_mhz == 96);
    /* synchronised heads, or switched off: not pinned */
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 1, 0, &sent) == RDNA3_OK && sent == 0 && u.why == RD_UCLK_WHY_SYNC);
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 1, &sent) == RDNA3_OK && sent == 0 && u.why == RD_UCLK_WHY_OFF &&
          S.dal_hard_min[RD_PPCLK_UCLK] == 96);
    /* pinned, then switched off live: released */
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && sent == 1 && S.dal_hard_min[RD_PPCLK_UCLK] == 1218);
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 1, &sent) == RDNA3_OK && sent == 1 && S.dal_hard_min[RD_PPCLK_UCLK] == 96 &&
          u.releases == 2);

    /* the PMFW refuses SetHardMinByFreq: the state stays, the next decision tries again */
    S.dal_refuse = RD_DALSMC_MSG_SetHardMinByFreq;
    S.dal_refused = 0;
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_EIO && sent == 1 && S.dal_refused == 1);
    CHECK(u.p_state_change_support && u.resync && u.fails == 1 && u.last_err == RDNA3_EIO && u.pins == 2);
    S.dal_refuse = 0;
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && sent == 1 &&
          S.dal_hard_min[RD_PPCLK_UCLK] == 1218 && !u.resync && u.last_err == 0 && u.pins == 3);
    /* a refused release: tried again too, even with the same decision */
    S.dal_refuse = RD_DALSMC_MSG_SetHardMinByFreq;
    CHECK(dcn32_uclk_pin_update(dev, &u, 1, 0, 0, &sent) == RDNA3_EIO && !u.p_state_change_support && u.resync);
    S.dal_refuse = 0;
    CHECK(dcn32_uclk_pin_update(dev, &u, 1, 0, 0, &sent) == RDNA3_OK && sent == 1 &&
          S.dal_hard_min[RD_PPCLK_UCLK] == 96 && u.p_state_change_support);

    /* pinned, then the card lost its power: the PMFW's own minimum; the next
     * two-head decision pins again */
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && S.dal_hard_min[RD_PPCLK_UCLK] == 1218);
    dcn32_uclk_pin_power_lost(&u);
    S.dal_hard_min[RD_PPCLK_UCLK] = 0;
    CHECK(u.p_state_change_support && u.hard_min_mhz == 0 && u.resync);
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && sent == 1 && S.dal_hard_min[RD_PPCLK_UCLK] == 1218);
    /* a suspend that kept the power (the pin still in the PMFW): the wake's
     * one-head decision is sent, not skipped as "unchanged" */
    dcn32_uclk_pin_power_lost(&u);
    CHECK(dcn32_uclk_pin_update(dev, &u, 1, 0, 0, &sent) == RDNA3_OK && sent == 1 && S.dal_hard_min[RD_PPCLK_UCLK] == 96 &&
          !u.resync);

    /* accepted, but ReturnHardMinStatus never shows UCLK (0x20): taken as
     * set, not sent again, no failure */
    S.dal_status_omit = RD_CHECK_HARD_MIN_CLK_UCLK;
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && sent == 1 && S.dal_hard_min[RD_PPCLK_UCLK] == 1218);
    CHECK(!u.p_state_change_support && u.unconfirmed == 1 && u.last_unconfirmed && !u.resync && u.fails == 2 &&
          u.consecutive_fails == 0 && u.last_err == RDNA3_ETIME);
    CHECK(u.status_unreliable);
    msgs = S.dal_msgs;
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && sent == 0 && S.dal_msgs == msgs);
    /* from now on no status wait: one message per transition, taken
     * unconfirmed (no 2 s under the display lock each time) */
    CHECK(dcn32_uclk_pin_update(dev, &u, 1, 0, 0, &sent) == RDNA3_OK && sent == 1 && S.dal_msgs == msgs + 1 &&
          u.last_unconfirmed && u.last_err == 0 && S.dal_hard_min[RD_PPCLK_UCLK] == 96 && u.unconfirmed == 1);
    S.dal_status_omit = 0;
    msgs = S.dal_msgs;
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && S.dal_msgs == msgs + 1 && u.last_unconfirmed &&
          S.dal_hard_min[RD_PPCLK_UCLK] == 1218);
    CHECK(dcn32_uclk_pin_update(dev, &u, 1, 0, 0, &sent) == RDNA3_OK && S.dal_hard_min[RD_PPCLK_UCLK] == 96);

    /* SetHardMinByFreq never answered (the mailbox wait times out): a
     * failure each; RD_UCLK_PIN_MAX_FAILS in a row and the pin gives up */
    S.dal_silent = RD_DALSMC_MSG_SetHardMinByFreq;
    S.dal_silenced = 0;
    for (k = 0; k < RD_UCLK_PIN_MAX_FAILS; k++) {
        CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_ETIME && sent == 1);
        S.reg[RD_DAL_RESP_REG] = RD_DALSMC_RESULT_OK;   /* the PMFW answers the next one's wait at once */
    }
    CHECK(S.dal_silenced == RD_UCLK_PIN_MAX_FAILS && u.gave_up && !u.ready && u.consecutive_fails == RD_UCLK_PIN_MAX_FAILS &&
          u.p_state_change_support);
    msgs = S.dal_msgs;
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_ENODEV && sent == 0 && S.dal_msgs == msgs);
    S.dal_silent = 0;

    /* GetSmuVersion refused at the init: not ready, nothing sent */
    S.dal_refuse = RD_DALSMC_MSG_GetSmuVersion;
    CHECK(dcn32_uclk_pin_init(dev, &u, 0) == RDNA3_ENODEV && !u.ready && !u.gave_up && u.last_err == RDNA3_EIO);
    S.dal_refuse = 0;
    S.reg[RD_DAL_RESP_REG] = RD_DALSMC_RESULT_OK;
    msgs = S.dal_msgs;
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_ENODEV && S.dal_msgs == msgs);
    /* GetSmuVersion never answered */
    S.dal_silent = RD_DALSMC_MSG_GetSmuVersion;
    CHECK(dcn32_uclk_pin_init(dev, &u, 0) == RDNA3_ENODEV && !u.ready && u.last_err == RDNA3_ETIME);
    S.dal_silent = 0;
    S.reg[RD_DAL_RESP_REG] = RD_DALSMC_RESULT_OK;

    /* a bring-up again after a pin (resync): the first decision is sent,
     * even one head's release */
    CHECK(dcn32_uclk_pin_init(dev, &u, 1) == RDNA3_OK && u.resync && u.p_state_change_support);
    CHECK(dcn32_uclk_pin_update(dev, &u, 1, 0, 0, &sent) == RDNA3_OK && sent == 1 && S.dal_hard_min[RD_PPCLK_UCLK] == 96 &&
          !u.resync);
    CHECK(dcn32_uclk_pin_init(dev, &u, 1) == RDNA3_OK);
    CHECK(dcn32_uclk_pin_update(dev, &u, 2, 0, 0, &sent) == RDNA3_OK && sent == 1 && S.dal_hard_min[RD_PPCLK_UCLK] == 1218);
    printf("sim_navi32: N74 UCLK pin: two heads -> hard min %u MHz, one head -> %u MHz\n", u.max_mhz, u.min_mhz);

    memcpy(S.reg, save, sizeof(save));
    SP.on = 0;
    S.dal_refuse = S.dal_silent = S.dal_status_omit = 0;
    memset(S.dal_hard_min, 0, sizeof(S.dal_hard_min));
}
