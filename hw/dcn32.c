/*
 * dcn32.c - see dcn32.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32.h"
#include "dcn32_clk.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_dig.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_scale.h"
#include "gen/dcn_3_2_0_crc.h"
#include "gen/dcn_3_2_0_mode.h"
#include "gen/dcn_3_2_0_bw.h"

#define HUBP_STRIDE (regHUBP1_DCHUBP_CNTL - regHUBP0_DCHUBP_CNTL)    /* 0xdc */
#define OTG_STRIDE  (regOTG1_OTG_CONTROL - regOTG0_OTG_CONTROL)      /* 0x80 */
#define DPP_STRIDE  (regCNVC_CUR1_CURSOR0_CONTROL - regCNVC_CUR0_CURSOR0_CONTROL)   /* 0x16b */
#define PG_STRIDE   (regDOMAIN1_PG_CONFIG - regDOMAIN0_PG_CONFIG)                  /* 2 */

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))

/* instance registers: paste reg##_BASE_IDX in the outermost macro, before
 * 'reg' can expand to its number */
#define RH(d, reg, n) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + HUBP_STRIDE * (n))
#define WH(d, reg, n, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + HUBP_STRIDE * (n), (v))
#define RO(d, reg, n) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + OTG_STRIDE * (n))
#define WO(d, reg, n, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + OTG_STRIDE * (n), (v))

#define RD(d, reg, n) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + DPP_STRIDE * (n))
#define WD(d, reg, n, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + DPP_STRIDE * (n), (v))

static void read_timing(rdna3_dev *d, uint32_t o, rdna3_dcn_head *h)
{
    /* optc1_read_otg_state + optc1_get_hw_timing */
    uint32_t ht = GET(RO(d, regOTG0_OTG_H_TOTAL, o), OTG0_OTG_H_TOTAL, OTG_H_TOTAL);
    uint32_t vt = GET(RO(d, regOTG0_OTG_V_TOTAL, o), OTG0_OTG_V_TOTAL, OTG_V_TOTAL);
    uint32_t hb = RO(d, regOTG0_OTG_H_BLANK_START_END, o);
    uint32_t vb = RO(d, regOTG0_OTG_V_BLANK_START_END, o);
    uint32_t hs = RO(d, regOTG0_OTG_H_SYNC_A, o), vs = RO(d, regOTG0_OTG_V_SYNC_A, o);
    uint32_t hbs = GET(hb, OTG0_OTG_H_BLANK_START_END, OTG_H_BLANK_START);
    uint32_t hbe = GET(hb, OTG0_OTG_H_BLANK_START_END, OTG_H_BLANK_END);
    uint32_t vbs = GET(vb, OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_START);
    uint32_t vbe = GET(vb, OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_END);

    h->h_total = ht + 1;
    h->v_total = vt + 1;
    h->h_active = hbs > hbe ? hbs - hbe : 0;
    h->v_active = vbs > vbe ? vbs - vbe : 0;
    h->h_front = ht + 1 > hbs ? ht + 1 - hbs : 0;
    h->v_front = vt + 1 > vbs ? vt + 1 - vbs : 0;
    h->h_sync = GET(hs, OTG0_OTG_H_SYNC_A, OTG_H_SYNC_A_END) - GET(hs, OTG0_OTG_H_SYNC_A, OTG_H_SYNC_A_START);
    h->v_sync = GET(vs, OTG0_OTG_V_SYNC_A, OTG_V_SYNC_A_END) - GET(vs, OTG0_OTG_V_SYNC_A, OTG_V_SYNC_A_START);
    h->h_sync_neg = GET(RO(d, regOTG0_OTG_H_SYNC_A_CNTL, o), OTG0_OTG_H_SYNC_A_CNTL, OTG_H_SYNC_A_POL);
    h->v_sync_neg = GET(RO(d, regOTG0_OTG_V_SYNC_A_CNTL, o), OTG0_OTG_V_SYNC_A_CNTL, OTG_V_SYNC_A_POL);
}

#define DIG_STRIDE (regDIG1_DIG_FE_CNTL - regDIG0_DIG_FE_CNTL)          /* 0x100 */
#define RDIG(d, reg, n) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + DIG_STRIDE * (n))
#define RD_DCN_MAX_DIGS 5   /* DIGA..DIGE, UNIPHY A..E (link_enc_regs[] of dcn32) */

int dcn32_otg_dig(rdna3_dev *d, uint32_t otg, int *fe, uint32_t *mode)
{
    uint32_t be;
    for (be = 0; be < RD_DCN_MAX_DIGS; be++) {
        uint32_t en = RDIG(d, regDIG0_DIG_BE_EN_CNTL, be), cntl, src, f;
        if (en == 0xffffffffu || !GET(en, DIG0_DIG_BE_EN_CNTL, DIG_ENABLE))
            continue;
        cntl = RDIG(d, regDIG0_DIG_BE_CNTL, be);
        /* dcn10_get_dig_frontend: one-hot DIG_FE_SOURCE_SELECT -> ENGINE_ID_DIGx */
        src = GET(cntl, DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT);
        if (!src || (src & (src - 1)))
            continue;
        for (f = 0; !(src & (1u << f)); f++)
            ;
        if (f >= RD_DCN_MAX_DIGS)
            continue;
        /* enc1_dig_source_otg */
        if (GET(RDIG(d, regDIG0_DIG_FE_CNTL, f), DIG0_DIG_FE_CNTL, DIG_SOURCE_SELECT) != otg)
            continue;
        if (fe)
            *fe = (int)f;
        if (mode)
            *mode = GET(cntl, DIG0_DIG_BE_CNTL, DIG_MODE);   /* dcn10_get_dig_mode */
        return (int)be;
    }
    return -1;
}

int dcn32_read_state(rdna3_dev *d, rdna3_dcn *dc)
{
    uint32_t p;
    dc->nheads = 0;
    dc->present = d->ipver[HWIP_DCE].present;
    if (!dc->present)
        return 0;
    for (p = 0; p < RD_DCN_MAX_PIPES; p++) {
        uint32_t cntl = RH(d, regHUBP0_DCHUBP_CNTL, p), otg, dim;
        rdna3_dcn_head *h = &dc->head[dc->nheads];
        if (cntl == 0xffffffffu || GET(cntl, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN))
            continue;
        /* a power-gated pipe (dcn32_hubp_pg_control) reads back nothing
         * meaningful: BLANK_EN 0 / VTG_SEL 0 would look lit on OTG 0 */
        if (GET(rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, regDOMAIN0_PG_STATUS_BASE_IDX,
                                               regDOMAIN0_PG_STATUS + PG_STRIDE * p)),
                DOMAIN0_PG_STATUS, DOMAIN_PGFSM_PWR_STATUS))
            continue;
        otg = GET(cntl, HUBP0_DCHUBP_CNTL, HUBP_VTG_SEL);
        if (otg >= RD_DCN_MAX_PIPES ||
            !GET(RO(d, regOTG0_OTG_CONTROL, otg), OTG0_OTG_CONTROL, OTG_MASTER_EN))
            continue;
        {
            /* a second pipe on a head's OTG: the ODM pipe of that head
             * (get_odm_config), not a head of its own */
            uint32_t k, x;
            int odm = 0;
            for (k = 0; k < dc->nheads && !odm; k++) {
                rdna3_dcn_head *m = &dc->head[k];
                if (m->otg != otg || m->odm_cnt > 1)
                    continue;
                x = GET(RH(d, regHUBP0_DCSURF_PRI_VIEWPORT_START, p), HUBP0_DCSURF_PRI_VIEWPORT_START,
                        PRI_VIEWPORT_X_START);
                m->odm_cnt = 2;
                if (x == 0) {
                    /* this pipe has the left half: it is the head's, the
                     * one found first is the ODM pipe */
                    uint32_t q = m->pipe;
                    x = GET(RH(d, regHUBP0_DCSURF_PRI_VIEWPORT_START, q), HUBP0_DCSURF_PRI_VIEWPORT_START,
                            PRI_VIEWPORT_X_START);
                    m->pipe = p;
                    m->odm_pipe = q;
                    m->width = x + GET(RH(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, q),
                                       HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, PRI_VIEWPORT_WIDTH);
                    dc->requested[p] = dc->requested[q];
                } else {
                    m->odm_pipe = p;
                    m->width = x + GET(RH(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, p),
                                       HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, PRI_VIEWPORT_WIDTH);
                    dc->requested[p] = dc->requested[m->pipe];
                }
                m->odm_x = x;
                RLOG(d, "DCN: head %u: pipe %u is its ODM pipe from x %u (%ux%u)", k, m->odm_pipe, x, m->width,
                     m->height);
                odm = 1;
            }
            if (odm)
                continue;
        }
        h->odm_cnt = 0;
        h->odm_pipe = 0;
        h->odm_x = 0;
        h->pipe = p;
        h->otg = otg;
        h->format = GET(RH(d, regHUBP0_DCSURF_SURFACE_CONFIG, p), HUBP0_DCSURF_SURFACE_CONFIG,
                        SURFACE_PIXEL_FORMAT);
        h->sw_mode = GET(RH(d, regHUBP0_DCSURF_TILING_CONFIG, p), HUBP0_DCSURF_TILING_CONFIG, SW_MODE);
        h->dcc = GET(RH(d, regHUBPREQ0_DCSURF_SURFACE_CONTROL, p), HUBPREQ0_DCSURF_SURFACE_CONTROL,
                     PRIMARY_SURFACE_DCC_EN);
        dim = RH(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, p);
        h->width = GET(dim, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, PRI_VIEWPORT_WIDTH);
        h->height = GET(dim, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, PRI_VIEWPORT_HEIGHT);
        /* hubp2_program_size writes surface_pitch - 1 */
        h->pitch_px = GET(RH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, p), HUBPREQ0_DCSURF_SURFACE_PITCH, PITCH) + 1;
        h->address = RH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, p) |
                     ((uint64_t)(RH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, p) & 0xffff) << 32);
        read_timing(d, otg, h);
        h->dig_fe = -1;
        h->dig_mode = 0;
        h->dig_be = dcn32_otg_dig(d, otg, &h->dig_fe, &h->dig_mode);
        dc->requested[p] = h->address;
        RLOG(d, "DCN: head %u: pipe %u otg %u %ux%u pitch %u fmt %u sw %u dcc %u at MC 0x%llx, "
             "total %ux%u, DIG FE %d BE %d mode %u", dc->nheads, p, otg, h->width, h->height,
             h->pitch_px, h->format, h->sw_mode, h->dcc, (unsigned long long)h->address, h->h_total,
             h->v_total, h->dig_fe, h->dig_be, h->dig_mode);
        dc->nheads++;
    }
    return (int)dc->nheads;
}

int dcn32_head_usable(rdna3_dev *d, const rdna3_dcn_head *h)
{
    uint64_t end = (uint64_t)h->pitch_px * 4 * h->height;
    if (h->format != RD_DCN_FMT_ARGB8888 && h->format != RD_DCN_FMT_ABGR8888)
        return 0;
    if (h->sw_mode != 0 || h->dcc || !h->width || !h->height || h->pitch_px < h->width)
        return 0;
    /* the surface sits in VRAM (MC FB aperture) */
    return h->address >= d->fb_start && h->address - d->fb_start + end <= d->vram_size;
}

/* the DSCL registers dpp1_dscl_set_scaler_manual_scale writes, then the
 * HUBP size registers hubp2_program_size writes, in one list for save /
 * restore (DSCL instance n at DPP_STRIDE, HUBP at HUBP_STRIDE) */
static const uint32_t kDscl[] = {
    regDSCL0_DSCL_AUTOCAL, regDSCL0_DSCL_CONTROL, regDSCL0_RECOUT_START, regDSCL0_RECOUT_SIZE,
    regDSCL0_MPC_SIZE, regDSCL0_SCL_MODE, regDSCL0_SCL_HORZ_FILTER_SCALE_RATIO,
    regDSCL0_SCL_VERT_FILTER_SCALE_RATIO, regDSCL0_SCL_HORZ_FILTER_SCALE_RATIO_C,
    regDSCL0_SCL_VERT_FILTER_SCALE_RATIO_C, regDSCL0_SCL_HORZ_FILTER_INIT,
    regDSCL0_SCL_HORZ_FILTER_INIT_C, regDSCL0_SCL_VERT_FILTER_INIT, regDSCL0_SCL_VERT_FILTER_INIT_BOT,
    regDSCL0_SCL_VERT_FILTER_INIT_C, regDSCL0_SCL_VERT_FILTER_INIT_BOT_C, regDSCL0_SCL_TAP_CONTROL,
    regDSCL0_DSCL_2TAP_CONTROL,
};
#define N_DSCL (sizeof(kDscl) / sizeof(kDscl[0]))
static const uint32_t kHubp[] = {
    regHUBP0_DCSURF_PRI_VIEWPORT_START, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION,
    regHUBPREQ0_DCSURF_SURFACE_PITCH,
};
#define N_HUBP (sizeof(kHubp) / sizeof(kHubp[0]))
typedef char rd_scale_regs_fit[(N_DSCL + N_HUBP <= RD_SCALE_REGS) ? 1 : -1];

static uint32_t dscl_off(rdna3_dev *d, uint32_t reg, uint32_t p)
{
    return rdna3_reg_offset(d, HWIP_DCE, 0, regDSCL0_SCL_MODE_BASE_IDX, reg) + DPP_STRIDE * p;
}

static uint32_t hubp_off(rdna3_dev *d, uint32_t reg, uint32_t p)
{
    return rdna3_reg_offset(d, HWIP_DCE, 0, regHUBP0_DCHUBP_CNTL_BASE_IDX, reg) + HUBP_STRIDE * p;
}

/* u3.19 in a 3.24 field, low 5 bits zero (dc_fixpt_u3d19(x) << 5); x in 1/2^24 */
static uint32_t u3d19(uint64_t x) { return (uint32_t)(((x >> 5) << 5) & 0x07ffffffu); }

/* (ratio + taps + 1) / 2 as SCL_x_INIT: frac 0.24 (u0d19 << 5), int 4 bits */
static uint32_t scl_init(uint64_t ratio, uint32_t taps, uint64_t add)
{
    uint64_t init = (ratio + ((uint64_t)(taps + 1) << 24)) / 2 + add;
    return (uint32_t)((((init & 0xffffff) >> 5) << 5) | ((init >> 24) & 0xf) << 24);
}

/* optc3_lock: the pipe's double-buffered registers wait for the unlock */
static void otg_lock(rdna3_dev *d, uint32_t otg, int lock)
{
    uint32_t v, t;
    if (lock) {
        v = RO(d, regOTG0_OTG_GLOBAL_CONTROL2, otg);
        WO(d, regOTG0_OTG_GLOBAL_CONTROL2, otg, SET(v, OTG0_OTG_GLOBAL_CONTROL2, OTG_MASTER_UPDATE_LOCK_SEL, otg));
    }
    WO(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg, lock ? OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK : 0);
    for (t = 0; lock && t < 10 && !GET(RO(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg),
                                       OTG0_OTG_MASTER_UPDATE_LOCK, UPDATE_LOCK_STATUS); t++)
        d->os.udelay(d->os.ctx, 1);
}

/* rdna3scaledlg: the registers a scaled plane needs besides the ratios,
 * saved with the GOP's bypass state (DSCL ones at DPP_STRIDE, HUBPREQ
 * ones at HUBP_STRIDE); DST_DIMENSIONS is read only (refcyc_per_htotal) */
enum { BW_LB_FMT, BW_LB_MEM, BW_AFTER, BW_LINE, BW_LINE_PRE, BW_REQ, BW_REQ_PRE, BW_DST_DIM, BW_N };
typedef char rd_scale_bw_fit[(BW_N <= RD_SCALE_BW_REGS) ? 1 : -1];
static const uint32_t kBw[BW_N] = {
    regDSCL0_LB_DATA_FORMAT, regDSCL0_LB_MEMORY_CTRL, regHUBPREQ0_DST_AFTER_SCALER,
    regHUBPREQ0_PER_LINE_DELIVERY, regHUBPREQ0_PER_LINE_DELIVERY_PRE, regHUBPREQ0_DCN_SURF0_TTU_CNTL0,
    regHUBPREQ0_DCN_SURF0_TTU_CNTL1, regHUBPREQ0_DST_DIMENSIONS,
};

/* dcn3_2_ip: dppclk_delay_scl 50, dppclk_delay_scl_lb_only 16 */
#define RD_DCN32_DPPCLK_DELAY_SCL         50u
#define RD_DCN32_DPPCLK_DELAY_SCL_LB_ONLY 16u
#define RD_DCN32_DCHUB_REFCLK_KHZ         100000u
/* dcn32_dpp_cap.max_lb_partitions */
#define RD_DCN32_MAX_LB_PARTITIONS 31u

static uint32_t bw_off(rdna3_dev *d, uint32_t k, uint32_t p)
{
    return k <= BW_LB_MEM ? dscl_off(d, kBw[k], p) : hubp_off(d, kBw[k], p);
}

/* dscl32_calc_lb_num_partitions (scaler on: memory 970 / 1290 / 970 + 1290
 * + 484 entries of 6 pixels, no alpha) + dpp1_dscl_find_lb_memory_config:
 * the first configuration that holds 'vtaps' lines (ceil(vratio) = 1) */
static uint32_t lb_memory_config(uint32_t line, uint32_t vtaps)
{
    uint32_t mls = (line + 5) / 6;
    if (!mls)
        mls = 1;
    if (970 / mls >= vtaps)
        return 1;   /* LB_MEMORY_CONFIG_1 */
    if (1290 / mls >= vtaps)
        return 2;   /* LB_MEMORY_CONFIG_2 */
    return 0;       /* LB_MEMORY_CONFIG_0: all of it */
}

static uint32_t ceil64(uint32_t x) { return (x + 63) & ~63u; }

/* the line buffer and DLG/TTU for a w x h source over nw x nh (see dcn32.h) */
static void scale_bw(rdna3_dev *d, rdna3_dcn_scale *sc, uint32_t p, uint32_t w, uint32_t nw)
{
    const uint32_t *g = sc->bw_reg[p];
    uint32_t cfg, x, y, htot, v, k;
    uint64_t num, den;

    /* dpp1_dscl_set_lb, float format: INTERLEAVE_EN 0, ALPHA_EN =
     * per_pixel_alpha (off for a primary plane) */
    cfg = lb_memory_config(w < nw ? w : nw, 2);
    rdna3_wreg(d, bw_off(d, BW_LB_FMT, p), 0);
    rdna3_wreg(d, bw_off(d, BW_LB_MEM, p),
               cfg << DSCL0_LB_MEMORY_CTRL__MEMORY_CONFIG__SHIFT |
               RD_DCN32_MAX_LB_PARTITIONS << DSCL0_LB_MEMORY_CTRL__LB_MAX_PARTITIONS__SHIFT);
    /* refcyc_per_req_delivery(_pre)_l: 64 / (HRatio x pclk), HRatio = w / nw */
    for (k = BW_REQ; k <= BW_REQ_PRE; k++) {
        v = GET(g[k], HUBPREQ0_DCN_SURF0_TTU_CNTL0, REFCYC_PER_REQ_DELIVERY);
        v = (uint32_t)(((uint64_t)v * nw + w / 2) / w);
        if (v > HUBPREQ0_DCN_SURF0_TTU_CNTL0__REFCYC_PER_REQ_DELIVERY_MASK)
            v = HUBPREQ0_DCN_SURF0_TTU_CNTL0__REFCYC_PER_REQ_DELIVERY_MASK;
        rdna3_wreg(d, bw_off(d, k, p), SET(g[k], HUBPREQ0_DCN_SURF0_TTU_CNTL0, REFCYC_PER_REQ_DELIVERY, v));
    }
    /* refcyc_per_line_delivery(_pre)_l: swath_width_ub / HRatio / pclk */
    num = (uint64_t)ceil64(w) * nw;
    den = (uint64_t)w * ceil64(nw);
    for (k = BW_LINE; k <= BW_LINE_PRE; k++) {
        v = GET(g[k], HUBPREQ0_PER_LINE_DELIVERY, REFCYC_PER_LINE_DELIVERY_L);
        v = (uint32_t)((v * num + den / 2) / den);
        if (v > HUBPREQ0_PER_LINE_DELIVERY__REFCYC_PER_LINE_DELIVERY_L_MASK)
            v = HUBPREQ0_PER_LINE_DELIVERY__REFCYC_PER_LINE_DELIVERY_L_MASK;
        rdna3_wreg(d, bw_off(d, k, p), SET(g[k], HUBPREQ0_PER_LINE_DELIVERY, REFCYC_PER_LINE_DELIVERY_L, v));
    }
    /* DSTXAfterScaler: the scaler's pipeline instead of the LB-only one,
     * DPPCLK cycles in refclk cycles (= pixels x ref_freq_to_pix_freq) */
    x = GET(g[BW_AFTER], HUBPREQ0_DST_AFTER_SCALER, REFCYC_X_AFTER_SCALER);
    y = GET(g[BW_AFTER], HUBPREQ0_DST_AFTER_SCALER, DST_Y_AFTER_SCALER);
    if (!GET(sc->reg[p][5], DSCL0_SCL_MODE, DSCL_MODE)) {   /* kDscl[5]: the GOP's SCL_MODE */
        rdna3_dcn_clocks c;
        uint32_t dpp;
        dcn32_clk_read(d, &c);
        dpp = dcn32_pipe_dppclk(d, &c, p);
        if (dpp)
            x += (uint32_t)(((uint64_t)(RD_DCN32_DPPCLK_DELAY_SCL - RD_DCN32_DPPCLK_DELAY_SCL_LB_ONLY) *
                             RD_DCN32_DCHUB_REFCLK_KHZ + dpp - 1) / dpp);
        htot = GET(g[BW_DST_DIM], HUBPREQ0_DST_DIMENSIONS, REFCYC_PER_HTOTAL) >> 8;
        while (htot && x >= htot && y < 7) {
            x -= htot;
            y++;
        }
    }
    rdna3_wreg(d, bw_off(d, BW_AFTER, p),
               SET(SET(g[BW_AFTER], HUBPREQ0_DST_AFTER_SCALER, REFCYC_X_AFTER_SCALER, x),
                   HUBPREQ0_DST_AFTER_SCALER, DST_Y_AFTER_SCALER, y));
    RLOG(d, "DCN: pipe %u scaled plane %u of %u: LB config %u, req delivery 0x%x -> 0x%x, line delivery %u -> %u, "
         "after scaler %u/%u -> %u/%u", p, w, nw, cfg,
         GET(g[BW_REQ], HUBPREQ0_DCN_SURF0_TTU_CNTL0, REFCYC_PER_REQ_DELIVERY),
         GET(rdna3_rreg(d, bw_off(d, BW_REQ, p)), HUBPREQ0_DCN_SURF0_TTU_CNTL0, REFCYC_PER_REQ_DELIVERY),
         GET(g[BW_LINE], HUBPREQ0_PER_LINE_DELIVERY, REFCYC_PER_LINE_DELIVERY_L),
         GET(rdna3_rreg(d, bw_off(d, BW_LINE, p)), HUBPREQ0_PER_LINE_DELIVERY, REFCYC_PER_LINE_DELIVERY_L),
         GET(g[BW_AFTER], HUBPREQ0_DST_AFTER_SCALER, DST_Y_AFTER_SCALER),
         GET(g[BW_AFTER], HUBPREQ0_DST_AFTER_SCALER, REFCYC_X_AFTER_SCALER), y, x);
}

int dcn32_set_source(rdna3_dev *d, rdna3_dcn *dc, rdna3_dcn_scale *sc, uint32_t i, uint64_t mc,
                     uint32_t w, uint32_t h, uint32_t pitch_px)
{
    rdna3_dcn_head *hd;
    uint32_t p, k, nw, nh, v;
    uint64_t hr, vr;
    if (i >= dc->nheads)
        return RDNA3_EINVAL;
    hd = &dc->head[i];
    p = hd->pipe;
    if (!sc->saved[p]) {
        sc->native_w[p] = hd->width;
        sc->native_h[p] = hd->height;
    }
    nw = sc->native_w[p];
    nh = sc->native_h[p];
    /* the scaler is per pipe: an ODM head takes its native size only */
    if (hd->odm_cnt > 1 && (w != nw || h != nh))
        return RDNA3_EINVAL;
    if (!w || !h || w > nw || h > nh || w * 4 < nw || h * 4 < nh || pitch_px < w || pitch_px > 0x4000 ||
        (mc & 0xff) || mc < d->fb_start || mc - d->fb_start + (uint64_t)pitch_px * 4 * h > d->vram_size)
        return RDNA3_EINVAL;
    if (!sc->saved[p] && (w != nw || h != nh)) {
        for (k = 0; k < N_DSCL; k++)
            sc->reg[p][k] = rdna3_rreg(d, dscl_off(d, kDscl[k], p));
        for (k = 0; k < N_HUBP; k++)
            sc->reg[p][N_DSCL + k] = rdna3_rreg(d, hubp_off(d, kHubp[k], p));
        sc->saved[p] = 1;
        sc->bw_saved[p] = 0;
        if (sc->dlg) {
            for (k = 0; k < BW_N; k++)
                sc->bw_reg[p][k] = rdna3_rreg(d, bw_off(d, k, p));
            sc->bw_saved[p] = 1;
        }
    }
    otg_lock(d, hd->otg, 1);
    if (w == nw && h == nh) {
        if (sc->saved[p]) {
            for (k = 0; k < N_DSCL; k++)
                rdna3_wreg(d, dscl_off(d, kDscl[k], p), sc->reg[p][k]);
            for (k = 0; k < N_HUBP; k++)
                rdna3_wreg(d, hubp_off(d, kHubp[k], p), sc->reg[p][N_DSCL + k]);
            for (k = 0; sc->bw_saved[p] && k < BW_DST_DIM; k++)
                rdna3_wreg(d, bw_off(d, k, p), sc->bw_reg[p][k]);
            sc->saved[p] = 0;
            sc->bw_saved[p] = 0;
        }
        v = RH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, p);
        WH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, p, SET(v, HUBPREQ0_DCSURF_SURFACE_PITCH, PITCH, pitch_px - 1));
        if (hd->odm_cnt > 1)
            WH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, hd->odm_pipe,
               SET(v, HUBPREQ0_DCSURF_SURFACE_PITCH, PITCH, pitch_px - 1));
    } else {
        hr = ((uint64_t)w << 24) / nw;
        vr = ((uint64_t)h << 24) / nh;
        /* autocal off, boundary mode clean, recout = the whole active area */
        rdna3_wreg(d, dscl_off(d, regDSCL0_DSCL_AUTOCAL, p), 0);
        rdna3_wreg(d, dscl_off(d, regDSCL0_DSCL_CONTROL, p), 0);
        rdna3_wreg(d, dscl_off(d, regDSCL0_RECOUT_START, p), 0);
        rdna3_wreg(d, dscl_off(d, regDSCL0_RECOUT_SIZE, p), nw | nh << DSCL0_RECOUT_SIZE__RECOUT_HEIGHT__SHIFT);
        rdna3_wreg(d, dscl_off(d, regDSCL0_MPC_SIZE, p), nw | nh << DSCL0_MPC_SIZE__MPC_HEIGHT__SHIFT);
        v = rdna3_rreg(d, dscl_off(d, regDSCL0_SCL_MODE, p));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_MODE, p),
                   SET(v, DSCL0_SCL_MODE, DSCL_MODE, 1));   /* DSCL_MODE_SCALING_444_RGB_ENABLE */
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_HORZ_FILTER_SCALE_RATIO, p), u3d19(hr));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_VERT_FILTER_SCALE_RATIO, p), u3d19(vr));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_HORZ_FILTER_SCALE_RATIO_C, p), u3d19(hr));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_VERT_FILTER_SCALE_RATIO_C, p), u3d19(vr));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_HORZ_FILTER_INIT, p), scl_init(hr, 2, 0));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_HORZ_FILTER_INIT_C, p), scl_init(hr, 2, 0));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_VERT_FILTER_INIT, p), scl_init(vr, 2, 0));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_VERT_FILTER_INIT_BOT, p), scl_init(vr, 2, vr));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_VERT_FILTER_INIT_C, p), scl_init(vr, 2, 0));
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_VERT_FILTER_INIT_BOT_C, p), scl_init(vr, 2, vr));
        /* 2 taps everywhere (taps - 1), hardcoded bilinear coefficients */
        rdna3_wreg(d, dscl_off(d, regDSCL0_SCL_TAP_CONTROL, p),
                   1u << DSCL0_SCL_TAP_CONTROL__SCL_V_NUM_TAPS__SHIFT |
                   1u << DSCL0_SCL_TAP_CONTROL__SCL_H_NUM_TAPS__SHIFT |
                   1u << DSCL0_SCL_TAP_CONTROL__SCL_V_NUM_TAPS_C__SHIFT |
                   1u << DSCL0_SCL_TAP_CONTROL__SCL_H_NUM_TAPS_C__SHIFT);
        rdna3_wreg(d, dscl_off(d, regDSCL0_DSCL_2TAP_CONTROL, p),
                   DSCL0_DSCL_2TAP_CONTROL__SCL_H_2TAP_HARDCODE_COEF_EN_MASK |
                   DSCL0_DSCL_2TAP_CONTROL__SCL_V_2TAP_HARDCODE_COEF_EN_MASK);
        /* hubp2_program_size: the viewport is the whole (smaller) surface */
        WH(d, regHUBP0_DCSURF_PRI_VIEWPORT_START, p, 0);
        WH(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, p, w | h << 16);
        v = RH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, p);
        WH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, p, SET(v, HUBPREQ0_DCSURF_SURFACE_PITCH, PITCH, pitch_px - 1));
        /* rdna3scaledlg: line buffer and DLG/TTU for the scaled plane */
        if (sc->bw_saved[p])
            scale_bw(d, sc, p, w, nw);
    }
    /* hubp2_program_surface_flip_and_addr: HIGH first, the LOW write latches */
    WH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, p, (uint32_t)(mc >> 32) & 0xffff);
    WH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, p, (uint32_t)mc);
    if (hd->odm_cnt > 1) {
        WH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, hd->odm_pipe, (uint32_t)(mc >> 32) & 0xffff);
        WH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, hd->odm_pipe, (uint32_t)mc);
        dc->requested[hd->odm_pipe] = mc;
    }
    otg_lock(d, hd->otg, 0);
    hd->width = w;
    hd->height = h;
    hd->pitch_px = pitch_px;
    hd->address = mc;
    dc->requested[p] = mc;
    RLOG(d, "DCN: head %u source %ux%u pitch %u at MC 0x%llx on a %ux%u timing (%s)", i, w, h, pitch_px,
         (unsigned long long)mc, nw, nh, w == nw && h == nh ? "native, scaler restored" : "DPP scaler");
    return RDNA3_OK;
}

static void hubp_set_blank(rdna3_dev *d, uint32_t p, int blank);

int dcn32_set_blank(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, int blank)
{
    if (i >= dc->nheads)
        return RDNA3_EINVAL;
    hubp_set_blank(d, dc->head[i].pipe, blank);
    /* dcn20_blank_pixel_data walks the ODM pipes too */
    if (dc->head[i].odm_cnt > 1)
        hubp_set_blank(d, dc->head[i].odm_pipe, blank);
    return RDNA3_OK;
}

/* hubp2_set_blank */
static void hubp_set_blank(rdna3_dev *d, uint32_t p, int blank)
{
    uint32_t cntl, t;
    cntl = RH(d, regHUBP0_DCHUBP_CNTL, p);
    if (blank && cntl) {
        /* hubp2_set_blank_regs: REG_WAIT(HUBP_NO_OUTSTANDING_REQ, 1, 1 us, 100000) */
        for (t = 0; t < 100000 && !GET(RH(d, regHUBP0_DCHUBP_CNTL, p), HUBP0_DCHUBP_CNTL,
                                        HUBP_NO_OUTSTANDING_REQ); t++)
            d->os.udelay(d->os.ctx, 1);
        cntl = RH(d, regHUBP0_DCHUBP_CNTL, p);
    }
    cntl = SET(cntl, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, blank ? 1 : 0);
    cntl = SET(cntl, HUBP0_DCHUBP_CNTL, HUBP_TTU_DISABLE, blank ? 1 : 0);
    WH(d, regHUBP0_DCHUBP_CNTL, p, cntl);
}

int dcn32_scanout_info(rdna3_dev *d, uint32_t pipe, rdna3_scanout_info *s)
{
    uint32_t cntl, dim, i;
    for (i = 0; i < sizeof(*s); i++)
        ((uint8_t *)s)[i] = 0;
    s->pipe = pipe;
    if (pipe >= RD_DCN_MAX_PIPES)
        return RDNA3_EINVAL;
    /* the same power check as dcn32_read_state: a gated HUBP reads junk */
    if (GET(rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, regDOMAIN0_PG_STATUS_BASE_IDX,
                                           regDOMAIN0_PG_STATUS + PG_STRIDE * pipe)),
            DOMAIN0_PG_STATUS, DOMAIN_PGFSM_PWR_STATUS))
        return RDNA3_OK;
    s->powered = 1;
    cntl = RH(d, regHUBP0_DCHUBP_CNTL, pipe);
    if (cntl == 0xffffffffu)
        return RDNA3_OK;
    s->otg = GET(cntl, HUBP0_DCHUBP_CNTL, HUBP_VTG_SEL);
    s->blanked = GET(cntl, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN);
    s->underflow = GET(cntl, HUBP0_DCHUBP_CNTL, HUBP_UNDERFLOW_STATUS);
    /* optc1_is_tg_enabled: OTG_MASTER_EN */
    s->lit = !s->blanked && s->otg < RD_DCN_MAX_PIPES &&
             GET(RO(d, regOTG0_OTG_CONTROL, s->otg), OTG0_OTG_CONTROL, OTG_MASTER_EN);
    s->format = GET(RH(d, regHUBP0_DCSURF_SURFACE_CONFIG, pipe), HUBP0_DCSURF_SURFACE_CONFIG,
                    SURFACE_PIXEL_FORMAT);
    s->sw_mode = GET(RH(d, regHUBP0_DCSURF_TILING_CONFIG, pipe), HUBP0_DCSURF_TILING_CONFIG, SW_MODE);
    s->dcc = GET(RH(d, regHUBPREQ0_DCSURF_SURFACE_CONTROL, pipe), HUBPREQ0_DCSURF_SURFACE_CONTROL,
                 PRIMARY_SURFACE_DCC_EN);
    dim = RH(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, pipe);
    s->width = GET(dim, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, PRI_VIEWPORT_WIDTH);
    s->height = GET(dim, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, PRI_VIEWPORT_HEIGHT);
    s->pitch_px = GET(RH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, pipe), HUBPREQ0_DCSURF_SURFACE_PITCH,
                      PITCH) + 1;
    s->flip_pending = GET(RH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, pipe),
                          HUBPREQ0_DCSURF_FLIP_CONTROL, SURFACE_FLIP_PENDING);
    s->address = RH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, pipe) |
                 ((uint64_t)(RH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, pipe) & 0xffff) << 32);
    s->inuse = RH(d, regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, pipe) |
               ((uint64_t)(RH(d, regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, pipe) & 0xffff) << 32);
    /* what the pipe has to keep up with: DET / compbuf, clocks, scaler, DLG */
    {
        rdna3_dcn_clocks c;
        uint32_t seg = regDCHUBBUB_DET0_CTRL_BASE_IDX;
        s->det_segs = GET(rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, seg, regDCHUBBUB_DET0_CTRL +
                                                           (regDCHUBBUB_DET1_CTRL - regDCHUBBUB_DET0_CTRL) * pipe)),
                          DCHUBBUB_DET0_CTRL, DET0_SIZE_CURRENT);
        s->compbuf_segs = GET(rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, regDCHUBBUB_COMPBUF_CTRL_BASE_IDX,
                                                             regDCHUBBUB_COMPBUF_CTRL)),
                              DCHUBBUB_COMPBUF_CTRL, COMPBUF_SIZE_CURRENT);
        dcn32_clk_read(d, &c);
        s->dispclk_khz = c.dispclk_khz;
        s->dppclk_khz = dcn32_pipe_dppclk(d, &c, pipe);
        s->dscl_mode = GET(RD(d, regDSCL0_SCL_MODE, pipe), DSCL0_SCL_MODE, DSCL_MODE);
        s->lb_memory_ctrl = RD(d, regDSCL0_LB_MEMORY_CTRL, pipe);
        s->dscl_mem_pwr = RD(d, regDSCL0_DSCL_MEM_PWR_CTRL, pipe);
        s->dlg_after_scaler = RH(d, regHUBPREQ0_DST_AFTER_SCALER, pipe);
        s->dlg_req_delivery = GET(RH(d, regHUBPREQ0_DCN_SURF0_TTU_CNTL0, pipe), HUBPREQ0_DCN_SURF0_TTU_CNTL0,
                                  REFCYC_PER_REQ_DELIVERY);
        s->dlg_line_delivery = GET(RH(d, regHUBPREQ0_PER_LINE_DELIVERY, pipe), HUBPREQ0_PER_LINE_DELIVERY,
                                   REFCYC_PER_LINE_DELIVERY_L);
    }
    return RDNA3_OK;
}

int dcn32_clear_underflow(rdna3_dev *d, uint32_t pipe)
{
    uint32_t v;
    if (pipe >= RD_DCN_MAX_PIPES)
        return RDNA3_EINVAL;
    if (GET(rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, regDOMAIN0_PG_STATUS_BASE_IDX,
                                           regDOMAIN0_PG_STATUS + PG_STRIDE * pipe)),
            DOMAIN0_PG_STATUS, DOMAIN_PGFSM_PWR_STATUS))
        return RDNA3_ENODEV;
    v = RH(d, regHUBP0_DCHUBP_CNTL, pipe);
    WH(d, regHUBP0_DCHUBP_CNTL, pipe, SET(v, HUBP0_DCHUBP_CNTL, HUBP_UNDERFLOW_CLEAR, 1));
    return RDNA3_OK;
}

int dcn32_scanout_sample(rdna3_dev *d, const rdna3_scanout_info *s, uint32_t out_w,
                         uint32_t out_h, uint32_t row0, uint32_t nrows, uint8_t *rgb)
{
    uint64_t mc = s->inuse ? s->inuse : s->address, off, len;
    volatile uint8_t *base;
    uint32_t r, x;
    if ((s->format != RD_DCN_FMT_ARGB8888 && s->format != RD_DCN_FMT_ABGR8888) || s->sw_mode ||
        s->dcc || !s->width || !s->height || s->pitch_px < s->width || !out_w || !out_h ||
        row0 + nrows > out_h || mc < d->fb_start)
        return RDNA3_EINVAL;
    off = mc - d->fb_start;
    len = (uint64_t)s->pitch_px * 4 * s->height;
    base = rdna3_vram_cpu(d, off, len);
    if (!base)
        return RDNA3_EINVAL;
    for (r = 0; r < nrows; r++) {
        uint32_t sy = (uint32_t)((uint64_t)(row0 + r) * s->height / out_h);
        const volatile uint32_t *row = (const volatile uint32_t *)(base + (uint64_t)sy * s->pitch_px * 4);
        for (x = 0; x < out_w; x++) {
            uint32_t v = row[(uint64_t)x * s->width / out_w];
            uint8_t *o = rgb + ((uint64_t)r * out_w + x) * 3;
            if (s->format == RD_DCN_FMT_ARGB8888) {   /* 0xAARRGGBB */
                o[0] = (uint8_t)(v >> 16); o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)v;
            } else {                                  /* 0xAABBGGRR */
                o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16);
            }
        }
    }
    return RDNA3_OK;
}

int dcn32_flip(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, uint64_t mc, uint32_t pitch_px,
               int immediate)
{
    rdna3_dcn_head *h;
    uint32_t p, fc, pitch_reg;
    int lock;
    if (i >= dc->nheads)
        return RDNA3_EINVAL;
    h = &dc->head[i];
    p = h->pipe;
    if ((mc & 0xff) || pitch_px < h->width || pitch_px > 0x4000 ||
        mc < d->fb_start || mc - d->fb_start + (uint64_t)pitch_px * 4 * h->height > d->vram_size)
        return RDNA3_EINVAL;
    lock = pitch_px != h->pitch_px;
    fc = RH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, p);
    fc = SET(fc, HUBPREQ0_DCSURF_FLIP_CONTROL, SURFACE_FLIP_TYPE, immediate ? 1 : 0);
    if (lock)   /* pitch and address must land in the same frame */
        fc = SET(fc, HUBPREQ0_DCSURF_FLIP_CONTROL, SURFACE_UPDATE_LOCK, 1);
    WH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, p, fc);
    if (lock) {
        pitch_reg = RH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, p);
        pitch_reg = SET(pitch_reg, HUBPREQ0_DCSURF_SURFACE_PITCH, PITCH, pitch_px - 1);
        WH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, p, pitch_reg);
        h->pitch_px = pitch_px;
    }
    /* hubp2_program_surface_flip_and_addr: HIGH first, the LOW write latches */
    WH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, p, (uint32_t)(mc >> 32) & 0xffff);
    WH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, p, (uint32_t)mc);
    if (lock) {
        fc = SET(fc, HUBPREQ0_DCSURF_FLIP_CONTROL, SURFACE_UPDATE_LOCK, 0);
        WH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, p, fc);
    }
    dc->requested[p] = mc;
    /* the ODM pipe scans the same surface (dcn20_update_plane_addr per pipe) */
    if (h->odm_cnt > 1) {
        uint32_t q = h->odm_pipe, qfc = RH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, q);
        qfc = SET(qfc, HUBPREQ0_DCSURF_FLIP_CONTROL, SURFACE_FLIP_TYPE, immediate ? 1 : 0);
        if (lock)
            WH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, q, SET(qfc, HUBPREQ0_DCSURF_FLIP_CONTROL, SURFACE_UPDATE_LOCK, 1));
        else
            WH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, q, qfc);
        if (lock) {
            pitch_reg = RH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, q);
            WH(d, regHUBPREQ0_DCSURF_SURFACE_PITCH, q, SET(pitch_reg, HUBPREQ0_DCSURF_SURFACE_PITCH, PITCH, pitch_px - 1));
        }
        WH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, q, (uint32_t)(mc >> 32) & 0xffff);
        WH(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, q, (uint32_t)mc);
        if (lock)
            WH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, q, qfc);
        dc->requested[q] = mc;
    }
    h->address = mc;
    return RDNA3_OK;
}

int dcn32_flip_pending(rdna3_dev *d, rdna3_dcn *dc, uint32_t i)
{
    uint32_t p;
    uint64_t inuse;
    if (i >= dc->nheads)
        return 0;
    p = dc->head[i].pipe;
    if (GET(RH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, p), HUBPREQ0_DCSURF_FLIP_CONTROL, SURFACE_FLIP_PENDING))
        return 1;
    inuse = RH(d, regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, p) |
            ((uint64_t)(RH(d, regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, p) & 0xffff) << 32);
    if (inuse != dc->requested[p])
        return 1;
    if (dc->head[i].odm_cnt > 1) {
        p = dc->head[i].odm_pipe;
        if (GET(RH(d, regHUBPREQ0_DCSURF_FLIP_CONTROL, p), HUBPREQ0_DCSURF_FLIP_CONTROL, SURFACE_FLIP_PENDING))
            return 1;
        inuse = RH(d, regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, p) |
                ((uint64_t)(RH(d, regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, p) & 0xffff) << 32);
        return inuse != dc->requested[p];
    }
    return 0;
}

static int cursor_image(rdna3_dev *d, rdna3_dcn *dc, uint32_t p, uint64_t mc, uint32_t size);

int dcn32_cursor_set_image(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, uint64_t mc, uint32_t size)
{
    int r;
    if (i >= dc->nheads)
        return RDNA3_EINVAL;
    r = cursor_image(d, dc, dc->head[i].pipe, mc, size);
    /* dcn10_set_cursor_attribute on every pipe of the stream */
    if (!r && dc->head[i].odm_cnt > 1)
        r = cursor_image(d, dc, dc->head[i].odm_pipe, mc, size);
    return r;
}

static int cursor_image(rdna3_dev *d, rdna3_dcn *dc, uint32_t p, uint64_t mc, uint32_t size)
{
    uint32_t v, pitch, lpc;
    switch (size) {
    case 64:  pitch = 0; lpc = 3; break;   /* CURSOR_PITCH_64, LINE_PER_CHUNK_8 */
    case 128: pitch = 1; lpc = 2; break;   /* 128, 4 */
    case 256: pitch = 2; lpc = 1; break;   /* 256, 2 */
    default:  return RDNA3_EINVAL;
    }
    if ((mc & 0xff) || mc < d->fb_start || mc - d->fb_start + 4ull * size * size > d->vram_size)
        return RDNA3_EINVAL;
    WH(d, regCURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH, p, (uint32_t)(mc >> 32) & 0xffff);
    WH(d, regCURSOR0_0_CURSOR_SURFACE_ADDRESS, p, (uint32_t)mc);
    v = SET(0u, CURSOR0_0_CURSOR_SIZE, CURSOR_WIDTH, size);
    v = SET(v, CURSOR0_0_CURSOR_SIZE, CURSOR_HEIGHT, size);
    WH(d, regCURSOR0_0_CURSOR_SIZE, p, v);
    v = RH(d, regCURSOR0_0_CURSOR_CONTROL, p);
    v = SET(v, CURSOR0_0_CURSOR_CONTROL, CURSOR_MODE, RD_DCN_CURSOR_MODE_PREMULT);
    v = SET(v, CURSOR0_0_CURSOR_CONTROL, CURSOR_2X_MAGNIFY, 0);
    v = SET(v, CURSOR0_0_CURSOR_CONTROL, CURSOR_PITCH, pitch);
    v = SET(v, CURSOR0_0_CURSOR_CONTROL, CURSOR_LINES_PER_CHUNK, lpc);
    WH(d, regCURSOR0_0_CURSOR_CONTROL, p, v);
    /* no shift of the cursor HDL schedule, chunk deadline adjust 3 */
    v = SET(0u, HUBPREQ0_CURSOR_SETTINGS, CURSOR0_CHUNK_HDL_ADJUST, 3);
    WH(d, regHUBPREQ0_CURSOR_SETTINGS, p, v);
    /* DPP side: same mode, no expansion, no degamma ROM */
    v = RD(d, regCNVC_CUR0_CURSOR0_CONTROL, p);
    v = SET(v, CNVC_CUR0_CURSOR0_CONTROL, CUR0_MODE, RD_DCN_CURSOR_MODE_PREMULT);
    v = SET(v, CNVC_CUR0_CURSOR0_CONTROL, CUR0_EXPANSION_MODE, 0);
    v = SET(v, CNVC_CUR0_CURSOR0_CONTROL, CUR0_ROM_EN, 0);
    WD(d, regCNVC_CUR0_CURSOR0_CONTROL, p, v);
    dc->cursor_size[p] = size;
    return RDNA3_OK;
}

static int cursor_position(rdna3_dev *d, rdna3_dcn *dc, const rdna3_dcn_head *h, uint32_t p, int32_t x, int32_t y,
                           int visible);

int dcn32_cursor_set_position(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, int32_t x, int32_t y,
                              int visible)
{
    int r;
    if (i >= dc->nheads)
        return RDNA3_EINVAL;
    r = cursor_position(d, dc, &dc->head[i], dc->head[i].pipe, x, y, visible);
    /* dcn10_set_cursor_position on the ODM pipe: the same position in the
     * plane, shown where it falls into that pipe's viewport */
    if (!r && dc->head[i].odm_cnt > 1)
        r = cursor_position(d, dc, &dc->head[i], dc->head[i].odm_pipe, x, y, visible);
    return r;
}

static int cursor_position(rdna3_dev *d, rdna3_dcn *dc, const rdna3_dcn_head *h, uint32_t p, int32_t x, int32_t y,
                           int visible)
{
    uint32_t v, hx = 0, hy = 0, size;
    /* the pipe's viewport (hubp2_cursor_set_position's visibility check) */
    int32_t vx = h->odm_cnt > 1 && p == h->odm_pipe ? (int32_t)h->odm_x : 0;
    int32_t vw = h->odm_cnt > 1 ? (p == h->odm_pipe ? (int32_t)(h->width - h->odm_x) : (int32_t)h->odm_x)
                                : (int32_t)h->width;
    int on;
    size = dc->cursor_size[p];
    if (!size)
        return RDNA3_EINVAL;   /* hubp2_cursor_set_position: no attributes yet */
    /* the hardware draws the image with its hot spot at POSITION, so the
     * top-left corner is POSITION - HOT_SPOT. POSITION is unsigned: a
     * cursor hanging off the top/left edge gets POSITION 0 and the
     * overhang as hot spot (amdgpu_dm get_cursor_position) */
    if (x < 0) { hx = (uint32_t)(-x); x = 0; }
    if (y < 0) { hy = (uint32_t)(-y); y = 0; }
    on = visible && hx < size && hy < size && (uint32_t)x < h->width && (uint32_t)y < h->height &&
         x - (int32_t)hx - vx < vw && x - (int32_t)hx - vx + (int32_t)size > 0;
    if (hx > 255) hx = 255;
    if (hy > 255) hy = 255;
    if (on != dc->cursor_on[p]) {
        v = RH(d, regCURSOR0_0_CURSOR_CONTROL, p);
        WH(d, regCURSOR0_0_CURSOR_CONTROL, p, SET(v, CURSOR0_0_CURSOR_CONTROL, CURSOR_ENABLE, on));
        v = RD(d, regCNVC_CUR0_CURSOR0_CONTROL, p);
        WD(d, regCNVC_CUR0_CURSOR0_CONTROL, p, SET(v, CNVC_CUR0_CURSOR0_CONTROL, CUR0_ENABLE, on));
        dc->cursor_on[p] = on;
    }
    v = SET(0u, CURSOR0_0_CURSOR_POSITION, CURSOR_X_POSITION, (uint32_t)x);
    v = SET(v, CURSOR0_0_CURSOR_POSITION, CURSOR_Y_POSITION, (uint32_t)y);
    WH(d, regCURSOR0_0_CURSOR_POSITION, p, v);
    v = SET(0u, CURSOR0_0_CURSOR_HOT_SPOT, CURSOR_HOT_SPOT_X, hx);
    v = SET(v, CURSOR0_0_CURSOR_HOT_SPOT, CURSOR_HOT_SPOT_Y, hy);
    WH(d, regCURSOR0_0_CURSOR_HOT_SPOT, p, v);
    WH(d, regCURSOR0_0_CURSOR_DST_OFFSET, p, 0);
    return RDNA3_OK;
}

/* optc1_get_vblank_counter */
uint32_t dcn32_frame_count(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i)
{
    if (i >= dc->nheads)
        return 0;
    return GET(RO(d, regOTG0_OTG_STATUS_FRAME_COUNT, dc->head[i].otg), OTG0_OTG_STATUS_FRAME_COUNT,
               OTG_FRAME_COUNT);
}

uint32_t dcn32_scanline(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i)
{
    if (i >= dc->nheads)
        return 0;
    return GET(RO(d, regOTG0_OTG_STATUS_POSITION, dc->head[i].otg), OTG0_OTG_STATUS_POSITION,
               OTG_VERT_COUNT);
}

int dcn32_in_vblank(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i)
{
    if (i >= dc->nheads)
        return 0;
    return (int)GET(RO(d, regOTG0_OTG_STATUS, dc->head[i].otg), OTG0_OTG_STATUS, OTG_V_BLANK);
}

void dcn32_otg_crc_config(rdna3_dev *d, uint32_t o, int enable)
{
    rdna3_dcn_head t;
    uint32_t v, xw, yw;
    WO(d, regOTG0_OTG_CRC_CNTL, o, 0);
    if (!enable)
        return;
    read_timing(d, o, &t);
    xw = SET(0u, OTG0_OTG_CRC0_WINDOWA_X_CONTROL, OTG_CRC0_WINDOWA_X_END, t.h_active);
    yw = SET(0u, OTG0_OTG_CRC0_WINDOWA_Y_CONTROL, OTG_CRC0_WINDOWA_Y_END, t.v_active);
    WO(d, regOTG0_OTG_CRC0_WINDOWA_X_CONTROL, o, xw);
    WO(d, regOTG0_OTG_CRC0_WINDOWA_Y_CONTROL, o, yw);
    WO(d, regOTG0_OTG_CRC0_WINDOWB_X_CONTROL, o, xw);   /* same field layout */
    WO(d, regOTG0_OTG_CRC0_WINDOWB_Y_CONTROL, o, yw);
    v = SET(0u, OTG0_OTG_CRC_CNTL, OTG_CRC_CONT_EN, 1);
    v = SET(v, OTG0_OTG_CRC_CNTL, OTG_CRC0_SELECT, 0);   /* UNION_WINDOW_A_B */
    v = SET(v, OTG0_OTG_CRC_CNTL, OTG_CRC_EN, 1);
    WO(d, regOTG0_OTG_CRC_CNTL, o, v);
}

void dcn32_crc_enable(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i, int enable)
{
    if (i < dc->nheads)
        dcn32_otg_crc_config(d, dc->head[i].otg, enable);
}

/* optc1_get_crc */
int dcn32_crc_read(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i, uint32_t *r, uint32_t *g,
                   uint32_t *b)
{
    uint32_t o, rg, bb;
    if (i >= dc->nheads)
        return RDNA3_EINVAL;
    o = dc->head[i].otg;
    if (!GET(RO(d, regOTG0_OTG_CRC_CNTL, o), OTG0_OTG_CRC_CNTL, OTG_CRC_EN))
        return RDNA3_ENODEV;
    rg = RO(d, regOTG0_OTG_CRC0_DATA_RG, o);
    bb = RO(d, regOTG0_OTG_CRC0_DATA_B, o);
    *r = GET(rg, OTG0_OTG_CRC0_DATA_RG, CRC0_R_CR);
    *g = GET(rg, OTG0_OTG_CRC0_DATA_RG, CRC0_G_Y);
    *b = GET(bb, OTG0_OTG_CRC0_DATA_B, CRC0_B_CB);
    return RDNA3_OK;
}
