/*
 * dcn32_txstate.c - see dcn32_txstate.h.
 *
 * Ported from (Linux v6.12, MIT): DC's hardware state log, dcn10_log_hw_state
 * (dcn10_hwseq.c) and the read-backs it and the link code use:
 * optc1_read_otg_state (OTG_CONTROL, V_TOTAL, V_TOTAL_MIN / MAX /
 * CONTROL), dcn10_link_encoder_is_dig_enabled (DIG_BE_EN_CNTL.DIG_ENABLE),
 * dcn10_get_dig_frontend (DIG_BE_CNTL.DIG_FE_SOURCE_SELECT),
 * enc1_dig_source_otg (DIG_FE_CNTL.DIG_SOURCE_SELECT), enc1_read_state
 * (the stream encoder's DIG / HDMI registers) and dccg32_get_pixel_rate_div
 * style reads of the DCCG. Registers only read, never written; what the
 * problem bits call wrong, and which fields are status or self-clearing,
 * is the field's meaning in dcn_3_2_0_sh_mask.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_txstate.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_mode.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_dig.h"
#include "gen/dcn_3_2_0_hdmi.h"
#include "gen/dcn_3_2_0_audio.h"
#include "gen/dcn_3_2_0_dp.h"
#include "gen/dcn_3_2_0_tx.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define M(reg, field) ((uint32_t)reg##__##field##_MASK)

#define TX_MAX_OTG 4

/* which instance a register belongs to, and its stride */
enum { TX_OTG, TX_PRC, TX_FE, TX_AFMT, TX_BE, TX_PHY };
#define OTG_S  (regOTG1_OTG_CONTROL - regOTG0_OTG_CONTROL)                  /* 0x80 */
#define PRC_S  (regOTG1_PIXEL_RATE_CNTL - regOTG0_PIXEL_RATE_CNTL)          /* 4 */
#define DIG_S  (regDIG1_DIG_FE_CNTL - regDIG0_DIG_FE_CNTL)                  /* 0x100 */
#define AFMT_S (regAFMT1_AFMT_INFOFRAME_CONTROL0 - regAFMT0_AFMT_INFOFRAME_CONTROL0)
/* PHYxSYMCLK_CLOCK_CNTL, SYMCLKx_CLOCK_ENABLE and DIO_LINKx_CNTL are one
 * register per PHY, A..E in a row (dcn_3_2_0_offset.h) */
#define PHY_S  (regPHYBSYMCLK_CLOCK_CNTL - regPHYASYMCLK_CLOCK_CNTL)        /* 1 */
typedef char tx_phy_strides[(PHY_S == 1 && regSYMCLKB_CLOCK_ENABLE - regSYMCLKA_CLOCK_ENABLE == PHY_S &&
                             regDIO_LINKB_CNTL - regDIO_LINKA_CNTL == PHY_S) ? 1 : -1];

/* vol: status and self-clearing fields, left out of the change detection;
 * imp: a change always matters (routing, enables, clock selects, DRR) */
typedef struct {
    const char *name;
    uint16_t off;
    uint8_t seg, inst, group, imp;
    uint32_t vol;
} tx_reg;

#define R(name, reg, inst, group, imp, vol) \
    { name, (uint16_t)(reg), (uint8_t)(reg##_BASE_IDX), inst, group, imp, (uint32_t)(vol) }
enum {
    I_OTG_CONTROL, I_H_TOTAL, I_V_TOTAL, I_V_TOTAL_MIN, I_V_TOTAL_MAX, I_V_TOTAL_CONTROL, I_PIXEL_RATE,
    I_PHYPLL_RATE, I_FE_CNTL, I_FIFO_CTRL0, I_HDMI_CONTROL, I_HDMI_STATUS, I_HDMI_GC, I_HDMI_VBI, I_HDMI_ACR,
    I_HDMI_AUDIO, I_INFOFRAME0, I_INFOFRAME1, I_GENERIC0, I_GENERIC1, I_AFMT_INFOFRAME0, I_AFMT_CNTL,
    I_AFMT_AUDIO, I_BE_CNTL, I_BE_EN, I_TMDS_CNTL, I_CLOCK_PATTERN, I_FORCE_DISABLE, I_DIO_LINK, I_PHYSYMCLK,
    I_SYMCLK_EN, I_COUNT
};
#define GENERIC_SEND (M(DIG0_HDMI_GENERIC_PACKET_CONTROL0, HDMI_GENERIC0_SEND) * 0x11111111u)
static const tx_reg kTx[] = {
    R("OTG_CONTROL", regOTG0_OTG_CONTROL, TX_OTG, 0, 1, M(OTG0_OTG_CONTROL, OTG_CURRENT_MASTER_EN_STATE)),
    R("H_TOTAL", regOTG0_OTG_H_TOTAL, TX_OTG, 0, 1, 0),
    R("V_TOTAL", regOTG0_OTG_V_TOTAL, TX_OTG, 0, 1, 0),
    R("V_TOTAL_MIN", regOTG0_OTG_V_TOTAL_MIN, TX_OTG, 0, 1, 0),
    R("V_TOTAL_MAX", regOTG0_OTG_V_TOTAL_MAX, TX_OTG, 0, 1, 0),
    R("V_TOTAL_CONTROL", regOTG0_OTG_V_TOTAL_CONTROL, TX_OTG, 0, 1, 0),
    R("PIXEL_RATE_CNTL", regOTG0_PIXEL_RATE_CNTL, TX_PRC, 0, 1,
      M(OTG0_PIXEL_RATE_CNTL, DPDTO0_ENABLE_STATUS) | M(OTG0_PIXEL_RATE_CNTL, DTBCLKDTO0_ENABLE_STATUS) |
      M(OTG0_PIXEL_RATE_CNTL, OTG0_DIO_FIFO_ERROR) | M(OTG0_PIXEL_RATE_CNTL, OTG0_DIO_ERROR_COUNT)),
    R("PHYPLL_PIXEL_RATE_CNTL", regOTG0_PHYPLL_PIXEL_RATE_CNTL, TX_PRC, 0, 1, 0),
    R("DIG_FE_CNTL", regDIG0_DIG_FE_CNTL, TX_FE, 1, 1, M(DIG0_DIG_FE_CNTL, DOLBY_VISION_METADATA_PACKET_MISSED)),
    R("DIG_FIFO_CTRL0", regDIG0_DIG_FIFO_CTRL0, TX_FE, 1, 0,
      M(DIG0_DIG_FIFO_CTRL0, DIG_FIFO_ERROR) | M(DIG0_DIG_FIFO_CTRL0, DIG_FIFO_RESET_DONE)),
    R("HDMI_CONTROL", regDIG0_HDMI_CONTROL, TX_FE, 1, 0, M(DIG0_HDMI_CONTROL, HDMI_ERROR_ACK)),
    R("HDMI_STATUS", regDIG0_HDMI_STATUS, TX_FE, 1, 0, 0xffffffffu),
    R("HDMI_GC", regDIG0_HDMI_GC, TX_FE, 1, 0, 0),
    R("HDMI_VBI_PACKET_CONTROL", regDIG0_HDMI_VBI_PACKET_CONTROL, TX_FE, 1, 0,
      M(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_NULL_SEND) | M(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_GC_SEND) |
      M(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_ISRC_SEND) | M(DIG0_HDMI_VBI_PACKET_CONTROL, HDMI_ACP_SEND)),
    R("HDMI_ACR_PACKET_CONTROL", regDIG0_HDMI_ACR_PACKET_CONTROL, TX_FE, 1, 0,
      M(DIG0_HDMI_ACR_PACKET_CONTROL, HDMI_ACR_SEND)),
    R("HDMI_AUDIO_PACKET_CONTROL", regDIG0_HDMI_AUDIO_PACKET_CONTROL, TX_FE, 1, 0, 0),
    R("HDMI_INFOFRAME_CONTROL0", regDIG0_HDMI_INFOFRAME_CONTROL0, TX_FE, 2, 0,
      M(DIG0_HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_SEND) | M(DIG0_HDMI_INFOFRAME_CONTROL0, HDMI_MPEG_INFO_SEND)),
    R("HDMI_INFOFRAME_CONTROL1", regDIG0_HDMI_INFOFRAME_CONTROL1, TX_FE, 2, 0, 0),
    R("HDMI_GENERIC_PACKET_CONTROL0", regDIG0_HDMI_GENERIC_PACKET_CONTROL0, TX_FE, 2, 0, GENERIC_SEND),
    R("HDMI_GENERIC_PACKET_CONTROL1", regDIG0_HDMI_GENERIC_PACKET_CONTROL1, TX_FE, 2, 0, 0),
    R("AFMT_INFOFRAME_CONTROL0", regAFMT0_AFMT_INFOFRAME_CONTROL0, TX_AFMT, 2, 0,
      M(AFMT0_AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE)),
    R("AFMT_CNTL", regDIG0_AFMT_CNTL, TX_FE, 2, 0, M(DIG0_AFMT_CNTL, AFMT_AUDIO_CLOCK_ON)),
    R("AFMT_AUDIO_PACKET_CONTROL", regAFMT0_AFMT_AUDIO_PACKET_CONTROL, TX_AFMT, 2, 0,
      M(AFMT0_AFMT_AUDIO_PACKET_CONTROL, AFMT_AUDIO_FIFO_OVERFLOW_ACK) |
      M(AFMT0_AFMT_AUDIO_PACKET_CONTROL, AFMT_60958_CS_UPDATE) |
      M(AFMT0_AFMT_AUDIO_PACKET_CONTROL, AFMT_AZ_AUDIO_ENABLE_CHG_ACK)),
    R("DIG_BE_CNTL", regDIG0_DIG_BE_CNTL, TX_BE, 3, 1, 0),
    R("DIG_BE_EN_CNTL", regDIG0_DIG_BE_EN_CNTL, TX_BE, 3, 1, 0),
    R("TMDS_CNTL", regDIG0_TMDS_CNTL, TX_BE, 3, 0, 0),
    R("DIG_CLOCK_PATTERN", regDIG0_DIG_CLOCK_PATTERN, TX_BE, 3, 0, 0),
    R("FORCE_DIG_DISABLE", regDIG0_FORCE_DIG_DISABLE, TX_BE, 3, 1, 0),
    R("DIO_LINK_CNTL", regDIO_LINKA_CNTL, TX_PHY, 3, 1, 0),
    R("PHYSYMCLK_CLOCK_CNTL", regPHYASYMCLK_CLOCK_CNTL, TX_PHY, 3, 1, 0),
    R("SYMCLK_CLOCK_ENABLE", regSYMCLKA_CLOCK_ENABLE, TX_PHY, 3, 1, 0),
};
#undef R
typedef char tx_table_fits[(sizeof(kTx) / sizeof(kTx[0]) == RD_TX_NREGS && I_COUNT == RD_TX_NREGS) ? 1 : -1];

/* group 4: per DIG / PHY, when the chain is not known */
static const uint8_t kDig[RD_TX_DIG_REGS] = { I_BE_EN, I_BE_CNTL, I_FE_CNTL, I_DIO_LINK, I_PHYSYMCLK, I_SYMCLK_EN };
static const char *const kDigName[RD_TX_DIG_REGS] = { "BE_EN", "BE_CNTL", "FE_CNTL", "DIO_LINK", "PHYSYMCLK",
                                                      "SYMCLK" };

static uint32_t reg_addr(uint32_t i, uint32_t otg, uint32_t fe, uint32_t be)
{
    uint32_t off = kTx[i].off;
    switch (kTx[i].inst) {
    case TX_OTG:  return off + OTG_S * otg;
    case TX_PRC:  return off + PRC_S * otg;
    case TX_FE:   return off + DIG_S * fe;
    case TX_AFMT: return off + AFMT_S * fe;
    case TX_BE:   return off + DIG_S * be;
    default:      return off + PHY_S * be;
    }
}

static uint32_t rd(rdna3_dev *d, uint32_t i, uint32_t otg, uint32_t fe, uint32_t be)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, kTx[i].seg, reg_addr(i, otg, fe, be)));
}

int dcn32_tx_state_read(rdna3_dev *d, uint32_t otg, int fe, int be, rdna3_tx_state *t)
{
    uint32_t i, k;
    if (!t)
        return RDNA3_EINVAL;
    for (i = 0; i < sizeof(*t); i++)
        ((uint8_t *)t)[i] = 0;
    if (!d || otg >= TX_MAX_OTG || fe >= RD_TX_MAX_DIG || be >= RD_TX_MAX_DIG)
        return RDNA3_EINVAL;
    t->otg = otg;
    t->fe = fe >= 0 ? (uint32_t)fe : RD_TX_NONE;
    t->be = be >= 0 ? (uint32_t)be : RD_TX_NONE;
    /* the timing generator first: all ones is no register file at all */
    t->v[I_OTG_CONTROL] = rd(d, I_OTG_CONTROL, otg, 0, 0);
    if (t->v[I_OTG_CONTROL] == 0xffffffffu)
        return RDNA3_ENODEV;
    for (i = 0; i < RD_TX_NREGS; i++) {
        uint32_t inst = kTx[i].inst;
        if (((inst == TX_FE || inst == TX_AFMT) && fe < 0) || ((inst == TX_BE || inst == TX_PHY) && be < 0))
            continue;
        t->v[i] = rd(d, i, otg, fe < 0 ? 0 : (uint32_t)fe, be < 0 ? 0 : (uint32_t)be);
        t->read_mask |= 1u << i;
    }
    /* a chain not followed: which DIG is on, what it takes, where the PHYs point */
    if (fe < 0 || be < 0) {
        t->all_digs = 1;
        for (i = 0; i < RD_TX_MAX_DIG; i++)
            for (k = 0; k < RD_TX_DIG_REGS; k++)
                t->dig[i][k] = rd(d, kDig[k], otg, i, i);
    }
    t->valid = 1;
    return RDNA3_OK;
}

static int has(const rdna3_tx_state *t, uint32_t i)
{
    return (t->read_mask >> i) & 1u;
}

uint32_t dcn32_tx_state_problems(const rdna3_tx_state *t)
{
    uint32_t p = 0, be_on = 0, vt, vmin, vmax, ctl;
    if (!t || !t->valid)
        return 0;
    if (!GET(t->v[I_OTG_CONTROL], OTG0_OTG_CONTROL, OTG_MASTER_EN))
        p |= RD_TX_P_OTG_OFF;
    else if (!GET(t->v[I_OTG_CONTROL], OTG0_OTG_CONTROL, OTG_CURRENT_MASTER_EN_STATE))
        p |= RD_TX_P_NOT_RUNNING;
    /* the status fields the change detection leaves out: a DTO asked for
     * that does not run (the pixel clock of a DP / HPO stream) */
    if ((GET(t->v[I_PIXEL_RATE], OTG0_PIXEL_RATE_CNTL, DP_DTO0_ENABLE) &&
         !GET(t->v[I_PIXEL_RATE], OTG0_PIXEL_RATE_CNTL, DPDTO0_ENABLE_STATUS)) ||
        (GET(t->v[I_PIXEL_RATE], OTG0_PIXEL_RATE_CNTL, DTBCLK_DTO0_ENABLE) &&
         !GET(t->v[I_PIXEL_RATE], OTG0_PIXEL_RATE_CNTL, DTBCLKDTO0_ENABLE_STATUS)))
        p |= RD_TX_P_DTO_NOT_RUNNING;
    /* optc1_read_otg_state / optc32_set_drr: V_TOTAL_MIN_SEL / MAX_SEL put
     * the DRR bounds in charge of the frame length */
    ctl = t->v[I_V_TOTAL_CONTROL];
    vt = GET(t->v[I_V_TOTAL], OTG0_OTG_V_TOTAL, OTG_V_TOTAL);
    vmin = GET(t->v[I_V_TOTAL_MIN], OTG0_OTG_V_TOTAL_MIN, OTG_V_TOTAL_MIN);
    vmax = GET(t->v[I_V_TOTAL_MAX], OTG0_OTG_V_TOTAL_MAX, OTG_V_TOTAL_MAX);
    if (GET(ctl, OTG0_OTG_V_TOTAL_CONTROL, OTG_V_TOTAL_MIN_SEL) || GET(ctl, OTG0_OTG_V_TOTAL_CONTROL, OTG_V_TOTAL_MAX_SEL)) {
        if (!vmin || !vmax)
            p |= RD_TX_P_DRR_ZERO;
        else if (vmin != vt || vmax != vt)
            p |= RD_TX_P_DRR;
    }
    if (GET(t->v[I_PIXEL_RATE], OTG0_PIXEL_RATE_CNTL, OTG0_DIO_FIFO_ERROR) ||
        GET(t->v[I_PIXEL_RATE], OTG0_PIXEL_RATE_CNTL, OTG0_DIO_ERROR_COUNT))
        p |= RD_TX_P_DIO_FIFO;
    /* nothing about a DIG that was not read can be called fine */
    if (!has(t, I_BE_EN) || !has(t, I_FE_CNTL))
        p |= RD_TX_P_NO_CHAIN;
    if (has(t, I_BE_EN)) {
        /* dcn10_link_encoder_is_dig_enabled */
        be_on = GET(t->v[I_BE_EN], DIG0_DIG_BE_EN_CNTL, DIG_ENABLE);
        if (!be_on)
            p |= RD_TX_P_DIG_OFF;
        else if (!GET(t->v[I_BE_EN], DIG0_DIG_BE_EN_CNTL, DIG_SYMCLK_BE_ON))
            p |= RD_TX_P_BE_SYMCLK;
        if (GET(t->v[I_FORCE_DISABLE], DIG0_FORCE_DIG_DISABLE, FORCE_DIG_DISABLE))
            p |= RD_TX_P_FORCED_OFF;
        /* dcn10_get_dig_frontend: one-hot, the FE this BE takes */
        if (has(t, I_FE_CNTL) && GET(t->v[I_BE_CNTL], DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT) != (1u << t->fe))
            p |= RD_TX_P_ROUTE;
        /* TMDS goes through the DIG (DIO), never an HPO encoder */
        if (GET(t->v[I_DIO_LINK], DIO_LINKA_CNTL, ENC_TYPE_SEL) != 0 &&
            GET(t->v[I_BE_CNTL], DIG0_DIG_BE_CNTL, DIG_MODE) >= 2 &&
            GET(t->v[I_BE_CNTL], DIG0_DIG_BE_CNTL, DIG_MODE) <= 4)
            p |= RD_TX_P_NOT_DIO;
    }
    if (has(t, I_FE_CNTL)) {
        /* enc1_dig_source_otg */
        if (GET(t->v[I_FE_CNTL], DIG0_DIG_FE_CNTL, DIG_SOURCE_SELECT) != t->otg)
            p |= RD_TX_P_ROUTE;
        if (be_on && !GET(t->v[I_FE_CNTL], DIG0_DIG_FE_CNTL, DIG_SYMCLK_FE_ON))
            p |= RD_TX_P_FE_SYMCLK;
        if (GET(t->v[I_FIFO_CTRL0], DIG0_DIG_FIFO_CTRL0, DIG_FIFO_ERROR))
            p |= RD_TX_P_DIG_FIFO;
        if (GET(t->v[I_HDMI_GC], DIG0_HDMI_GC, HDMI_GC_AVMUTE) ||
            GET(t->v[I_HDMI_STATUS], DIG0_HDMI_STATUS, HDMI_ACTIVE_AVMUTE))
            p |= RD_TX_P_AVMUTE;
        if (GET(t->v[I_HDMI_STATUS], DIG0_HDMI_STATUS, HDMI_ERROR_INT))
            p |= RD_TX_P_HDMI_ERROR;
    }
    return p;
}

/* a bounded string builder: never writes past len, always terminates */
typedef struct {
    char *buf;
    size_t len, n;
} tx_out;

static void out_s(tx_out *o, const char *s)
{
    while (*s) {
        if (o->buf && o->len && o->n + 1 < o->len)
            o->buf[o->n] = *s;
        o->n++;
        s++;
    }
    if (o->buf && o->len)
        o->buf[o->n < o->len ? o->n : o->len - 1] = 0;
}

/* a cut text ends in "..." (the caller's count still says how much) */
static void out_mark_cut(tx_out *o)
{
    size_t k;
    if (!o->buf || o->len < 4 || o->n < o->len)
        return;
    for (k = o->len - 4; k < o->len - 1; k++)
        o->buf[k] = '.';
    o->buf[o->len - 1] = 0;
}

static void out_hex(tx_out *o, uint32_t v)
{
    char t[11];
    int i, k = 0;
    t[k++] = '0';
    t[k++] = 'x';
    for (i = 28; i > 0 && !((v >> i) & 0xfu); i -= 4)
        ;
    for (; i >= 0; i -= 4)
        t[k++] = "0123456789abcdef"[(v >> i) & 0xfu];
    t[k] = 0;
    out_s(o, t);
}

static void out_u(tx_out *o, uint32_t v)
{
    char t[11];
    int k = 10;
    t[k] = 0;
    do {
        t[--k] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k);
    out_s(o, t + k);
}

static void out_c(tx_out *o, char c)
{
    char t[2] = { c, 0 };
    out_s(o, t);
}

static void out_inst(tx_out *o, uint32_t inst)
{
    out_c(o, inst == RD_TX_NONE ? '?' : (char)('A' + inst));
}

size_t dcn32_tx_problems_str(uint32_t p, char *buf, size_t len)
{
    static const char *const names[RD_TX_P_NBITS] = {
        "OTG off", "DIG back end off", "back end symbol clock off", "front end symbol clock off",
        "DIG routing not this OTG", "DIG forced off", "DIG FIFO error", "DIO FIFO error", "AV mute",
        "HDMI error", "DRR on (V_TOTAL_MIN/MAX not the nominal total)", "DRR bound 0",
        "the PHY takes an HPO encoder", "DIG chain not found (its registers not read, see every DIG)",
        "OTG enabled but not running", "pixel clock DTO enabled but not running",
    };
    tx_out o = { buf, len, 0 };
    uint32_t i, any = 0;
    if (buf && len)
        buf[0] = 0;
    for (i = 0; i < RD_TX_P_NBITS; i++) {
        if (!(p & (1u << i)))
            continue;
        if (any++)
            out_s(&o, ", ");
        out_s(&o, names[i]);
    }
    if (!any)
        out_s(&o, "none");
    out_mark_cut(&o);
    return o.n;
}

size_t dcn32_tx_chain_str(const rdna3_tx_state *t, char *buf, size_t len)
{
    tx_out o = { buf, len, 0 };
    if (buf && len)
        buf[0] = 0;
    if (!t)
        return 0;
    out_s(&o, "OTG");
    out_u(&o, t->otg);
    out_s(&o, " -> DIG");
    out_inst(&o, t->fe);
    out_s(&o, " -> UNIPHY");
    out_inst(&o, t->be);
    return o.n;
}

size_t dcn32_tx_state_format(const rdna3_tx_state *t, uint32_t g, char *buf, size_t len)
{
    tx_out o = { buf, len, 0 };
    uint32_t i, k, any = 0;
    if (buf && len)
        buf[0] = 0;
    if (!t || !t->valid || g >= RD_TX_GROUPS || (g == 4 && !t->all_digs))
        return 0;
    if (g == 4) {
        out_s(&o, "every DIG (the chain was not known):");
        for (i = 0; i < RD_TX_MAX_DIG; i++) {
            out_s(&o, i ? "; " : " ");
            out_c(&o, (char)('A' + i));
            out_c(&o, ':');
            for (k = 0; k < RD_TX_DIG_REGS; k++) {
                out_c(&o, ' ');
                out_s(&o, kDigName[k]);
                out_c(&o, ' ');
                out_hex(&o, t->dig[i][k]);
            }
        }
        out_mark_cut(&o);
        return o.n;
    }
    if (g == 0) {
        out_s(&o, "OTG");
        out_u(&o, t->otg);
    } else if (g < 3) {
        out_s(&o, "DIG");
        out_inst(&o, t->fe);
        out_s(&o, g == 1 ? " FE" : " packets");
    } else {
        out_s(&o, "DIG BE / UNIPHY");
        out_inst(&o, t->be);
    }
    out_c(&o, ':');
    for (i = 0; i < RD_TX_NREGS; i++) {
        if (kTx[i].group != g)
            continue;
        out_c(&o, ' ');
        out_s(&o, kTx[i].name);
        out_c(&o, ' ');
        if (has(t, i))
            out_hex(&o, t->v[i]);
        else
            out_c(&o, '-');
        any++;
    }
    out_mark_cut(&o);
    return any ? o.n : 0;
}

static void diff_one(tx_out *o, uint32_t *n, const char *pre, const char *name, uint32_t a, uint32_t b)
{
    if ((*n)++)
        out_s(o, ", ");
    out_s(o, pre);
    out_s(o, name);
    out_c(o, ' ');
    out_hex(o, a);
    out_s(o, " -> ");
    out_hex(o, b);
}

uint32_t dcn32_tx_state_diff(const rdna3_tx_state *was, const rdna3_tx_state *now, char *buf, size_t len,
                             int *important)
{
    tx_out o = { buf, len, 0 };
    uint32_t i, k, n = 0;
    if (buf && len)
        buf[0] = 0;
    if (important)
        *important = 0;
    if (!was || !now || !was->valid || !now->valid)
        return 0;
    if (was->otg != now->otg || was->fe != now->fe || was->be != now->be) {
        char a[48], b[48];
        dcn32_tx_chain_str(was, a, sizeof(a));
        dcn32_tx_chain_str(now, b, sizeof(b));
        out_s(&o, "another chain: ");
        out_s(&o, a);
        out_s(&o, " => ");
        out_s(&o, b);
        out_mark_cut(&o);
        if (important)
            *important = 1;
        return RD_TX_NREGS;
    }
    for (i = 0; i < RD_TX_NREGS; i++) {
        if (!has(was, i) || !has(now, i) || ((was->v[i] ^ now->v[i]) & ~kTx[i].vol) == 0)
            continue;
        if (important && kTx[i].imp)
            *important = 1;
        diff_one(&o, &n, "", kTx[i].name, was->v[i], now->v[i]);
    }
    if (was->all_digs && now->all_digs) {
        for (i = 0; i < RD_TX_MAX_DIG; i++)
            for (k = 0; k < RD_TX_DIG_REGS; k++) {
                char pre[6] = { 'D', 'I', 'G', (char)('A' + i), ' ', 0 };
                if (((was->dig[i][k] ^ now->dig[i][k]) & ~kTx[kDig[k]].vol) == 0)
                    continue;
                if (important)
                    *important = 1;
                diff_one(&o, &n, pre, kDigName[k], was->dig[i][k], now->dig[i][k]);
            }
    }
    out_mark_cut(&o);
    return n;
}
