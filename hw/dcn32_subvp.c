/*
 * dcn32_subvp.c - see dcn32_subvp.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_subvp.h"
#include "dcn32_otg.h"
#include "dcn32_odm.h"
#include "dcn32_hpo.h"
#include "dcn32_hwseq.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_clone.h"
#include "gen/dcn_3_2_0_scale.h"
#include "gen/dcn_3_2_0_mode.h"
#include "gen/dcn_3_2_0_hwseq.h"
#include "gen/dcn_3_2_0_pstate.h"
#include "gen/dcn_3_2_0_otg.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))
#define FIELD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)

/* instance n of a register (reg##_BASE_IDX pasted before 'reg' expands) */
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
#define PRC_S   (regOTG1_PIXEL_RATE_CNTL - regOTG0_PIXEL_RATE_CNTL)
#define DPPDTO_S (regDPPCLK1_DTO_PARAM - regDPPCLK0_DTO_PARAM)
#define VTG_S   (regVTG1_CONTROL - regVTG0_CONTROL)
#define MPCC_NONE 0xfu

#define PGFSM_POWER_ON  0
#define PGFSM_POWER_OFF 2
/* enum test_pattern_mode: TEST_PATTERN_MODE_HORIZONTALBARS */
#define DPG_MODE_HORIZONTALBARS 4

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

static void zero(void *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        ((uint8_t *)p)[i] = 0;
}

/* (a x b) / c in us from lines x htotal over the pixel clock (kHz) */
static uint32_t lines_us(uint32_t lines, uint32_t htotal, uint32_t pclk_khz)
{
    return pclk_khz ? (uint32_t)((uint64_t)lines * htotal * 1000 / pclk_khz) : 0;
}

/* ---- the decision (dcn32_fpu.c / dcn32_resource_helpers.c) ---- */

int dcn32_subvp_in_use(const rdna3_pstate_ctx *ctx)
{
    uint32_t k;
    for (k = 0; k < ctx->nplanes; k++)
        if (ctx->plane[k].mall == RD_MALL_PSTATE_SUB_VIEWPORT || ctx->plane[k].mall == RD_MALL_PSTATE_PHANTOM)
            return 1;
    return 0;
}

int dcn32_check_native_scaling_for_res(const rdna3_pstate_plane *p, uint32_t w, uint32_t h)
{
    /* timing, plane source and destination all w x h (the plane covers
     * the stream: dst = the active area) */
    return p->h_active == w && p->v_active == h && p->src_w == w && p->src_h == h;
}

int dcn32_is_center_timing(const rdna3_pstate_plane *p)
{
    /* stream src / dst height against v_addressable, then the plane's:
     * the plane is scaled to the whole active area here, so only a
     * source height other than the active height counts */
    return p->src_h != p->v_active;
}

static const struct { uint32_t w, h; } subvp_high_refresh_res[] = {
    { 3840, 2160 }, { 3440, 1440 }, { 2560, 1440 }, { 1920, 1080 },
};
#define SUBVP_HIGH_REFRESH_MIN 120
#define SUBVP_HIGH_REFRESH_MAX 175
static const struct { uint32_t w, h; } subvp_active_margin_res[] = {
    { 2560, 1440 }, { 1920, 1080 },
};
#define SUBVP_ACTIVE_MARGIN_MIN 55
#define SUBVP_ACTIVE_MARGIN_MAX 65

int dcn32_allow_subvp_high_refresh_rate(const rdna3_pstate_ctx *ctx, const rdna3_pstate_plane *p)
{
    uint32_t k, i, min_refresh = SUBVP_HIGH_REFRESH_MAX, r;
    for (k = 0; k < ctx->nplanes; k++) {
        if (ctx->plane[k].mall == RD_MALL_PSTATE_PHANTOM)
            continue;
        r = dcn32_refresh_rate_ceil(&ctx->plane[k]);
        if (r < min_refresh)
            min_refresh = r;
    }
    if (min_refresh < SUBVP_HIGH_REFRESH_MIN)
        return 0;
    r = dcn32_refresh_rate_ceil(p);
    if (r < SUBVP_HIGH_REFRESH_MIN || r > SUBVP_HIGH_REFRESH_MAX)
        return 0;
    /* no FreeSync here: the single-stream "!allow_freesync" holds */
    for (i = 0; i < sizeof(subvp_high_refresh_res) / sizeof(subvp_high_refresh_res[0]); i++)
        if (dcn32_check_native_scaling_for_res(p, subvp_high_refresh_res[i].w, subvp_high_refresh_res[i].h))
            return 1;
    return 0;
}

int dcn32_allow_subvp_with_active_margin(const rdna3_pstate_plane *p)
{
    uint32_t i, r = dcn32_refresh_rate_ceil(p);
    for (i = 0; i < sizeof(subvp_active_margin_res) / sizeof(subvp_active_margin_res[0]); i++)
        if (r >= SUBVP_ACTIVE_MARGIN_MIN && r <= SUBVP_ACTIVE_MARGIN_MAX &&
            dcn32_check_native_scaling_for_res(p, subvp_active_margin_res[i].w, subvp_active_margin_res[i].h))
            return 1;
    return 0;
}

uint32_t dcn32_subvp_free_pipes(const rdna3_pstate_ctx *ctx, uint32_t used_mask)
{
    uint32_t k, used = used_mask;
    for (k = 0; k < ctx->nplanes; k++) {
        used |= 1u << ctx->plane[k].pipe;
        if (ctx->plane[k].dpp_count > 1 && ctx->plane[k].odm_pipe < RD_DCN_MAX_PIPES)
            used |= 1u << ctx->plane[k].odm_pipe;
    }
    return ~used & ((1u << RD_DCN_MAX_PIPES) - 1);
}

static uint32_t popcount4(uint32_t m)
{
    uint32_t n = 0;
    while (m) {
        n += m & 1;
        m >>= 1;
    }
    return n;
}

/* dcn32_enough_pipes_for_subvp: free pipes for the smallest non-SubVP
 * stream, and at least one pipe in use */
static int enough_pipes_for_subvp(const rdna3_pstate_ctx *ctx, uint32_t used_mask)
{
    uint32_t k, min_split = RD_DCN_MAX_PIPES + 1, free_pipes = popcount4(dcn32_subvp_free_pipes(ctx, used_mask));
    for (k = 0; k < ctx->nplanes; k++)
        if (ctx->plane[k].mall == RD_MALL_PSTATE_DISABLE) {
            uint32_t n = ctx->plane[k].dpp_count ? ctx->plane[k].dpp_count : 1;
            if (n < min_split)
                min_split = n;
        }
    return free_pipes >= min_split && free_pipes < RD_DCN_MAX_PIPES;
}

/* dcn32_assign_subvp_pipe: of the candidates, the longest frame time.
 * RD_DCN_MAX_PIPES when none. */
static uint32_t assign_subvp_pipe(const rdna3_pstate_ctx *ctx, uint32_t used_mask)
{
    uint32_t k, best = RD_DCN_MAX_PIPES, free_pipes = popcount4(dcn32_subvp_free_pipes(ctx, used_mask));
    uint64_t max_frame_ns = 0;
    for (k = 0; k < ctx->nplanes; k++) {
        const rdna3_pstate_plane *p = &ctx->plane[k];
        uint32_t refresh = dcn32_refresh_rate_ceil(p), n = p->dpp_count ? p->dpp_count : 1;
        int32_t margin = ctx->dml.active_margin_ns[k];
        uint64_t frame_ns;
        /* not ODM (simplification, see the header), not scaled, <= 1800
         * MHz, not SubVP yet, below 120 Hz or an allowed high refresh
         * rate, no natural vactive switch unless the active-margin list
         * allows it (no PSR, no TMZ here) */
        if (p->mall != RD_MALL_PSTATE_DISABLE || n > 1 || dcn32_is_center_timing(p) ||
            p->pclk_khz / 1000 > RD_DCN3_2_MAX_SUBVP_PIXEL_RATE_MHZ ||
            !(refresh < 120 || dcn32_allow_subvp_high_refresh_rate(ctx, p)) ||
            !(margin <= 0 || dcn32_allow_subvp_with_active_margin(p)))
            continue;
        if (n > free_pipes)
            continue;
        frame_ns = (uint64_t)p->v_total * p->h_total * 1000000 / p->pclk_khz;
        if (frame_ns > max_frame_ns) {
            max_frame_ns = frame_ns;
            best = k;
        }
    }
    return best;
}

/* dcn32_set_phantom_stream_timing + dcn32_helper_populate_phantom_dlg_params:
 * the phantom plane of ctx->plane[main] on pipe 'pipe' */
static void set_phantom_stream_timing(const rdna3_pstate_ctx *ctx, uint32_t main, uint32_t pipe,
                                      rdna3_pstate_plane *ph)
{
    const rdna3_pstate_plane *m = &ctx->plane[main];
    uint32_t delay_lines, vactive, bp;
    /* lines for the p-state allow width and the FW processing delay */
    delay_lines = (uint32_t)((uint64_t)(RD_SUBVP_FW_PROCESSING_DELAY_US + RD_SUBVP_PSTATE_ALLOW_WIDTH_US) *
                             m->pclk_khz / 1000 / m->h_total);
    /* SubViewportLinesNeededInMALL + those + 16 (SUB_VP_START_LINE is
     * aligned to two 8-line swaths); no meta rows (one DPP, no DCC) */
    vactive = ctx->dml.subvp_lines[main] + delay_lines + RD_SUBVP_SWATH_HEIGHT_MARGIN_LINES;
    /* the back porch: the main pipe's VSTARTUP (what its OTG runs; DML's
     * maximum when not known) */
    bp = m->vstartup ? m->vstartup : m->v_total - m->v_active - 1;
    *ph = *m;
    ph->pipe = ph->otg = pipe;
    ph->odm_pipe = RD_DCN_MAX_PIPES;
    ph->dpp_count = 1;
    ph->v_active = vactive;
    ph->v_front = 1;
    ph->v_total = vactive + 1 + m->v_sync + bp;
    /* src.height = phantom vactive x main src / main dst */
    ph->src_h = (uint32_t)((uint64_t)vactive * m->src_h / m->v_active);
    ph->det_kb = 0;
    /* MaxVStartupLines = VTotal - VActive - max(1, writeback delay) */
    ph->vstartup = ph->v_total - ph->v_active - 1;
    if (ph->vstartup > 1023)
        ph->vstartup = 1023;
    ph->cursor_bytes = 0;
    ph->blanked = 0;
    ph->mall = RD_MALL_PSTATE_PHANTOM;
    ph->refresh_hz = dcn32_refresh_rate_ceil(ph);
    ph->paired = main + 1;
}

/* the phantom of main plane k (index), or RD_DCN_MAX_PIPES */
static uint32_t phantom_of(const rdna3_pstate_ctx *ctx, uint32_t k)
{
    uint32_t p = ctx->plane[k].paired;
    return p && p - 1 < ctx->nplanes ? p - 1 : RD_DCN_MAX_PIPES;
}

/* subvp_subvp_admissable */
static int subvp_subvp_admissable(const rdna3_pstate_ctx *ctx)
{
    uint32_t k, n = 0, min_r = SUBVP_HIGH_REFRESH_MIN, max_r = 0, r;
    for (k = 0; k < ctx->nplanes; k++)
        if (ctx->plane[k].mall == RD_MALL_PSTATE_SUB_VIEWPORT) {
            r = dcn32_refresh_rate_ceil(&ctx->plane[k]);
            if (r < min_r)
                min_r = r;
            if (r > max_r)
                max_r = r;
            n++;
        }
    return n == 2 && ((min_r < 120 && max_r < 120) ||
                      (min_r >= SUBVP_HIGH_REFRESH_MIN && max_r <= SUBVP_HIGH_REFRESH_MAX));
}

/* subvp_subvp_schedulable: the longest microschedule fits in half of each
 * active region less the other's vblank */
static int subvp_subvp_schedulable(const rdna3_pstate_ctx *ctx, const rdna3_subvp_state *sv)
{
    const rdna3_pstate_plane *m[2] = { NULL, NULL };
    uint32_t i, max_us = 0, idx = 0;
    int32_t va1, va2, vb1, vb2;
    for (i = 0; i < sv->n && idx < 2; i++) {
        const rdna3_pstate_plane *ph = &ctx->plane[sv->ph[i].phantom];
        uint32_t lines = (ph->v_total - ph->v_front) + ph->v_active;
        uint32_t t = lines_us(lines, ph->h_total, ph->pclk_khz) + RD_SUBVP_PREFETCH_END_TO_MALL_START_US +
                     RD_SUBVP_FW_PROCESSING_DELAY_US + 1;
        if (t > max_us)
            max_us = t;
        m[idx++] = &ctx->plane[sv->ph[i].main];
    }
    if (idx < 2)
        return 0;
    va1 = (int32_t)lines_us(m[0]->v_active, m[0]->h_total, m[0]->pclk_khz);
    va2 = (int32_t)lines_us(m[1]->v_active, m[1]->h_total, m[1]->pclk_khz);
    vb1 = (int32_t)lines_us(m[0]->v_total - m[0]->v_active, m[0]->h_total, m[0]->pclk_khz);
    vb2 = (int32_t)lines_us(m[1]->v_total - m[1]->v_active, m[1]->h_total, m[1]->pclk_khz);
    return (va1 - vb2) / 2 > (int32_t)max_us && (va2 - vb1) / 2 > (int32_t)max_us;
}

/* dcn32_subvp_vblank_admissable (no DRR, no PSR here) */
static int subvp_vblank_admissable(const rdna3_pstate_ctx *ctx)
{
    uint32_t k, subvp = 0, others = 0, refresh = 0;
    int disallow = 0;
    for (k = 0; k < ctx->nplanes; k++) {
        const rdna3_pstate_plane *p = &ctx->plane[k];
        if (p->mall == RD_MALL_PSTATE_SUB_VIEWPORT) {
            subvp++;
            /* disallow_subvp_in_active_plus_blank: 1920x1080 is FPO's */
            disallow |= p->v_active == 1080 && p->h_active == 1920;
            refresh = dcn32_refresh_rate_ceil(p);
        } else if (p->mall == RD_MALL_PSTATE_DISABLE) {
            others++;
        }
    }
    return subvp == 1 && others == 1 && refresh < 120 && !disallow &&
           ctx->dml.dram_change == RD_DRAM_CHANGE_VBLANK_W_MALL_SUB_VP;
}

/* subvp_vblank_schedulable: the SubVP active region holds the MALL
 * prefetch, one frame of the vblank display and the larger of its
 * blanking and the MALL region (uint16 microseconds as DC) */
static int subvp_vblank_schedulable(const rdna3_pstate_ctx *ctx)
{
    const rdna3_pstate_plane *vb = NULL, *m = NULL, *ph = NULL;
    uint32_t k;
    int32_t prefetch, mall, frame, active, blank;
    for (k = 0; k < ctx->nplanes; k++) {
        if (!vb && ctx->plane[k].mall == RD_MALL_PSTATE_DISABLE)
            vb = &ctx->plane[k];
        if (!m && ctx->plane[k].mall == RD_MALL_PSTATE_SUB_VIEWPORT) {
            uint32_t p = phantom_of(ctx, k);
            m = &ctx->plane[k];
            ph = p < ctx->nplanes ? &ctx->plane[p] : NULL;
        }
    }
    if (!vb || !m || !ph)
        return 0;
    prefetch = (uint16_t)(lines_us(ph->v_total - ph->v_front, ph->h_total, ph->pclk_khz) +
                          RD_SUBVP_PREFETCH_END_TO_MALL_START_US);
    mall = (uint16_t)lines_us(ph->v_active, ph->h_total, ph->pclk_khz);
    frame = (uint16_t)lines_us(vb->v_total, vb->h_total, vb->pclk_khz);
    blank = (uint16_t)lines_us(vb->v_total - vb->v_active, vb->h_total, vb->pclk_khz);
    active = (uint16_t)lines_us(m->v_active, m->h_total, m->pclk_khz);
    return active - prefetch - frame - (blank > mall ? blank : mall) > 0;
}

/* subvp_validate_static_schedulability */
static int static_schedulability(const rdna3_pstate_ctx *ctx, const rdna3_subvp_state *sv)
{
    uint32_t k, subvp = 0, others = 0, vactive = 0;
    for (k = 0; k < ctx->nplanes; k++) {
        if (ctx->plane[k].mall == RD_MALL_PSTATE_SUB_VIEWPORT)
            subvp++;
        if (ctx->plane[k].mall == RD_MALL_PSTATE_DISABLE) {
            others++;
            if (ctx->dml.active_margin_ns[k] > 0)
                vactive++;
        }
    }
    if (subvp == 2)
        return subvp_subvp_admissable(ctx) && subvp_subvp_schedulable(ctx, sv);
    if (subvp == 1 && others == 0)
        return 1;   /* single SubVP: supported when DML supports it */
    if (subvp == 1 && others == 1)
        return subvp_vblank_admissable(ctx) && subvp_vblank_schedulable(ctx);   /* no DRR display here */
    /* SubVP + VACTIVE and three displays are not supported */
    (void)vactive;
    return 0;
}

/* assign_subvp_index: the SubVP mains in pipe order */
static void assign_subvp_index(const rdna3_pstate_ctx *ctx, rdna3_subvp_state *sv)
{
    uint32_t p, i, idx = 0;
    for (p = 0; p < RD_DCN_MAX_PIPES; p++)
        for (i = 0; i < sv->n; i++)
            if (ctx->plane[sv->ph[i].main].pipe == p)
                sv->ph[i].subvp_index = idx++;
}

int dcn32_subvp_validate(const rdna3_dcn32_soc *soc, const rdna3_dcn32_bw *bw, rdna3_pstate_ctx *ctx,
                         uint32_t used_mask, int gecc, rdna3_subvp_state *sv)
{
    rdna3_pstate_ctx save = *ctx;
    uint32_t vlevel_temp, k, det_planned = ctx->compbuf_kb != 0;
    int found = 0;

    zero(sv, sizeof(*sv));
    if (!bw->nstates || !ctx->nplanes || gecc || dcn32_subvp_in_use(ctx))
        return 0;
    /* the entry: no p-state switch, or one only at a higher UCLK than state 0 */
    if (!(ctx->dram_change == RD_DRAM_CHANGE_UNSUPPORTED ||
          (bw->state[ctx->vlevel].dram_mts != bw->state[0].dram_mts &&
           ctx->dram_change != RD_DRAM_CHANGE_UNSUPPORTED)))
        return 0;
    vlevel_temp = ctx->vlevel;
    /* every plane fetches (dcn32_all_pipes_have_stream_and_plane) */
    for (k = 0; k < ctx->nplanes; k++)
        if (ctx->plane[k].blanked)
            return 0;
    while (!found && sv->n < 2 && ctx->nplanes < RD_DCN_MAX_PIPES && enough_pipes_for_subvp(ctx, used_mask)) {
        uint32_t main = assign_subvp_pipe(ctx, used_mask), free_mask, q;
        rdna3_subvp_phantom *ph;
        if (main >= ctx->nplanes)
            break;
        /* dcn32_add_phantom_pipes: the lowest free pipe (its OTG, OPP and
         * MPCC share the index) */
        free_mask = dcn32_subvp_free_pipes(ctx, used_mask);
        for (q = 0; q < RD_DCN_MAX_PIPES && !(free_mask & (1u << q)); q++)
            ;
        if (q >= RD_DCN_MAX_PIPES)
            break;
        ph = &sv->ph[sv->n];
        ph->main = main;
        ph->phantom = ctx->nplanes;
        ph->main_pipe = ctx->plane[main].pipe;
        ph->main_otg = ctx->plane[main].otg;
        ph->main_opp = ctx->plane[main].pipe;
        ph->pipe = q;
        set_phantom_stream_timing(ctx, main, q, &ctx->plane[ctx->nplanes]);
        ph->vactive = ctx->plane[ctx->nplanes].v_active;
        ph->vfront = ctx->plane[ctx->nplanes].v_front;
        ph->vsync = ctx->plane[ctx->nplanes].v_sync;
        ph->vtotal = ctx->plane[ctx->nplanes].v_total;
        ph->vbp = ph->vtotal - ph->vactive - ph->vfront - ph->vsync;
        ph->hactive = ctx->plane[main].h_active;
        ctx->plane[main].mall = RD_MALL_PSTATE_SUB_VIEWPORT;
        ctx->plane[main].paired = ctx->nplanes + 1;
        ctx->nplanes++;
        sv->n++;
        /* the DETs again: the phantom gets none, the streams share 18 */
        if (det_planned)
            dcn32_set_det_allocations(ctx);
        /* revalidate (p-state no longer required; the lowest state that
         * switches is taken) */
        if (dcn32_pstate_validate(soc, bw, ctx))
            continue;
        if (ctx->dram_change != RD_DRAM_CHANGE_UNSUPPORTED && static_schedulability(ctx, sv))
            found = 1;
    }
    /* only worth it on a lower UCLK */
    if (found && bw->state[ctx->vlevel].dram_mts >= bw->state[vlevel_temp].dram_mts)
        found = 0;
    if (!found) {
        *ctx = save;
        zero(sv, sizeof(*sv));
        return 0;
    }
    assign_subvp_index(ctx, sv);
    ctx->subvp = 1;
    ctx->subvp_plane = sv->ph[0].main;
    ctx->phantom_plane = sv->ph[0].phantom;
    return 1;
}

/* ---- the DMUB command (dc_dmub_srv.c) ---- */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t gcd32(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* reduce_fraction */
static void reduce_fraction(uint32_t num, uint32_t den, uint32_t *out_num, uint32_t *out_den)
{
    uint32_t g = gcd32(num, den);
    *out_num = g ? num / g : num;
    *out_den = g ? den / g : den;
}

/* lines for 'us' at the plane's line time, rounded up */
static uint32_t us_lines_ceil(uint32_t us, const rdna3_pstate_plane *p)
{
    uint64_t pclk_hz = (uint64_t)p->pclk_khz * 1000, ht = (uint64_t)p->h_total * 1000000;
    return (uint32_t)(((uint64_t)us * pclk_hz + ht - 1) / ht);
}

#define CFG_OFF       4    /* config_data */
#define PIPE_DATA(i)  (CFG_OFF + 4 + 28 * (i))

/* populate_subvp_cmd_pipe_info */
static void subvp_pipe_info(const rdna3_pstate_ctx *ctx, const rdna3_subvp_phantom *ph, uint8_t *pd)
{
    const rdna3_pstate_plane *m = &ctx->plane[ph->main], *p = &ctx->plane[ph->phantom];
    uint32_t num, den;
    put32(pd + 0, m->pclk_khz * 10);
    put16(pd + 4, m->v_total - m->v_front);
    put16(pd + 6, m->v_total - m->v_front - m->v_active);
    put16(pd + 8, p->v_active);
    /* prefetch lines = VACTIVE + BP + VSYNC of the phantom */
    put16(pd + 10, p->v_total - p->v_front);
    put16(pd + 12, us_lines_ceil(RD_SUBVP_PREFETCH_END_TO_MALL_START_US, p));
    put16(pd + 14, us_lines_ceil(RD_SUBVP_FW_PROCESSING_DELAY_US, p));
    put16(pd + 16, m->h_total);
    put16(pd + 18, m->v_total);
    pd[20] = (uint8_t)m->otg;
    pd[21] = (uint8_t)p->otg;
    /* the stream is unscaled (src = dst); the plane's src / dst heights */
    reduce_fraction(m->src_h, m->v_active, &num, &den);
    pd[22] = (uint8_t)num;
    pd[23] = (uint8_t)den;
    pd[24] = 0;   /* is_drr */
    pd[25] = RD_SUBVP_MAIN_SPLIT_NONE;
    pd[26] = RD_SUBVP_MAIN_SPLIT_NONE;
    pd[27] = RD_MCLK_SWITCH_SUBVP;
}

/* populate_subvp_cmd_vblank_pipe_info (no DRR) */
static void subvp_vblank_info(const rdna3_pstate_plane *v, uint8_t *pd)
{
    put32(pd + 0, v->pclk_khz * 10);
    put16(pd + 4, v->v_total - v->v_front);
    put16(pd + 6, v->v_total - v->v_front - v->v_active);
    put16(pd + 8, v->vstartup);
    put16(pd + 10, v->v_total);
    put16(pd + 12, v->h_total);
    pd[14] = (uint8_t)v->pipe;
    pd[27] = RD_MCLK_SWITCH_VBLANK;
}

/* update_subvp_prefetch_end_to_mall_start: both MALL regions start together */
static void update_prefetch_end_to_mall_start(const rdna3_pstate_ctx *ctx, const rdna3_subvp_state *sv,
                                              const uint32_t slot[2], uint8_t *cmd)
{
    const rdna3_pstate_plane *p0 = &ctx->plane[sv->ph[0].phantom], *p1 = &ctx->plane[sv->ph[1].phantom];
    /* DC divides by (pix_clk_hz + 15): kept */
    uint64_t t0 = (uint64_t)(p0->v_total - p0->v_front) * p0->h_total * 1000000 /
                  ((uint64_t)p0->pclk_khz * 1000 + RD_SUBVP_PREFETCH_END_TO_MALL_START_US);
    uint64_t t1 = (uint64_t)(p1->v_total - p1->v_front) * p1->h_total * 1000000 /
                  ((uint64_t)p1->pclk_khz * 1000 + RD_SUBVP_PREFETCH_END_TO_MALL_START_US);
    if (t0 > t1)
        put16(cmd + PIPE_DATA(slot[1]) + 12,
              us_lines_ceil(RD_SUBVP_PREFETCH_END_TO_MALL_START_US + (uint32_t)(t0 - t1), p1));
    else if (t1 > t0)
        put16(cmd + PIPE_DATA(slot[0]) + 12,
              us_lines_ceil(RD_SUBVP_PREFETCH_END_TO_MALL_START_US + (uint32_t)(t1 - t0), p0));
}

void dcn32_subvp_build_cmd(const rdna3_pstate_ctx *ctx, const rdna3_subvp_state *sv, uint32_t refclk_mhz, int enable,
                           uint8_t cmd[RD_DMUB_RB_CMD_SIZE])
{
    uint32_t hdr = RD_DMUB_HDR(RD_DMUB_CMD_FW_ASSISTED_MCLK_SWITCH, RD_DMUB_FAMS_HANDLE_SUBVP_CMD,
                               RD_DMUB_RB_CMD_SIZE - 4, 0);
    uint32_t p, k, slot = 0, sv_slot[2] = { 0, 0 }, n_subvp = 0;
    zero(cmd, RD_DMUB_RB_CMD_SIZE);
    put32(cmd, hdr);
    if (!enable)
        return;
    /* the pipes in DC's order (pipe index), mains and the other heads */
    for (p = 0; p < RD_DCN_MAX_PIPES && slot < 2; p++)
        for (k = 0; k < ctx->nplanes && slot < 2; k++) {
            const rdna3_pstate_plane *pl = &ctx->plane[k];
            if (pl->pipe != p)
                continue;
            if (pl->mall == RD_MALL_PSTATE_SUB_VIEWPORT) {
                uint32_t i;
                for (i = 0; i < sv->n; i++)
                    if (sv->ph[i].main == k) {
                        if (n_subvp < 2)
                            sv_slot[n_subvp] = slot;
                        n_subvp++;
                        subvp_pipe_info(ctx, &sv->ph[i], cmd + PIPE_DATA(slot++));
                    }
            } else if (pl->mall == RD_MALL_PSTATE_DISABLE) {
                subvp_vblank_info(pl, cmd + PIPE_DATA(slot++));
            }
        }
    if (n_subvp == 2 && sv->n == 2) {
        /* sv->ph[] in pipe order is what DC's subvp_pipes[] holds */
        uint32_t order[2];
        if (ctx->plane[sv->ph[0].main].pipe < ctx->plane[sv->ph[1].main].pipe) {
            order[0] = sv_slot[0];
            order[1] = sv_slot[1];
        } else {
            order[0] = sv_slot[1];
            order[1] = sv_slot[0];
        }
        update_prefetch_end_to_mall_start(ctx, sv, order, cmd);
    }
    cmd[CFG_OFF + 3] = RD_SUBVP_PSTATE_ALLOW_WIDTH_US;
    cmd[CFG_OFF + 2] = RD_SUBVP_VERTICAL_INT_MARGIN_US;
    /* the real set A p-state watermark, in DCHUB refclk cycles */
    {
        uint64_t wm = (uint64_t)ctx->wm[RD_WM_A].pstate_change_ns * refclk_mhz / 1000;
        put16(cmd + CFG_OFF, wm < 0xFFFF ? (uint32_t)wm : 0xFFFF);
    }
}

int dcn32_commit_subvp_config(rdna3_dmub *dm, const rdna3_pstate_ctx *ctx, rdna3_subvp_state *sv, uint32_t refclk_mhz)
{
    uint32_t k;
    int enable = 0;
    if (!dm || !dm->hw_init)
        return RDNA3_ENODEV;
    for (k = 0; k < ctx->nplanes; k++)
        enable |= ctx->plane[k].mall == RD_MALL_PSTATE_SUB_VIEWPORT;
    dcn32_subvp_build_cmd(ctx, sv, refclk_mhz, enable, sv->cmd);
    return dmub32_cmd_execute(dm, sv->cmd, 0, 100000);
}

int dcn32_subvp_pipe_control_lock(rdna3_dmub *dm, rdna3_subvp_state *sv, int subvp_in_use, int subvp_prev_use,
                                  int lock)
{
    /* should_lock_all_pipes (a full update) */
    if (!(subvp_in_use || subvp_prev_use))
        return RDNA3_OK;
    if (!dm || !dm->hw_init)
        return RDNA3_ENODEV;
    sv->locks++;
    return dmub32_hw_lock_inbox0(dm, lock);
}

int dcn32_subvp_pipe_control_lock_fast(rdna3_dmub *dm, rdna3_subvp_state *sv, int subvp_immediate_flip, int lock)
{
    if (!subvp_immediate_flip)
        return RDNA3_OK;
    if (!dm || !dm->hw_init)
        return RDNA3_ENODEV;
    sv->locks++;
    return dmub32_hw_lock_inbox0(dm, lock);
}

#define WR(d, reg, v) rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg), (v))
#define RR(d, reg) rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg))

void dmub_dcn32_save_surf_addr(rdna3_dev *d, uint64_t addr, uint64_t meta_addr, uint32_t subvp_index)
{
    uint32_t index;
    if (subvp_index == 0) {
        index = RR(d, regDMCUB_SCRATCH15);
        if (index) {
            WR(d, regDMCUB_SCRATCH9, (uint32_t)addr);
            WR(d, regDMCUB_SCRATCH11, (uint32_t)meta_addr);
        } else {
            WR(d, regDMCUB_SCRATCH12, (uint32_t)addr);
            WR(d, regDMCUB_SCRATCH13, (uint32_t)meta_addr);
        }
        WR(d, regDMCUB_SCRATCH15, !index);
    } else if (subvp_index == 1) {
        index = RR(d, regDMCUB_SCRATCH23);
        if (index) {
            WR(d, regDMCUB_SCRATCH18, (uint32_t)addr);
            WR(d, regDMCUB_SCRATCH19, (uint32_t)meta_addr);
        } else {
            WR(d, regDMCUB_SCRATCH20, (uint32_t)addr);
            WR(d, regDMCUB_SCRATCH22, (uint32_t)meta_addr);
        }
        WR(d, regDMCUB_SCRATCH23, !index);
    }
}

/* ---- OPP display pattern generator (dcn20_opp.c) ---- */

/* opp2_dpg_set_blank_color (black: COLOR_SPACE_SRGB's full-range zero) */
static void opp2_dpg_set_blank_color(rdna3_dev *d, uint32_t opp, uint32_t r, uint32_t g, uint32_t b)
{
    WI(d, regDPG0_DPG_COLOUR_B_CB, opp, OPP_S, (b << 6) << 16 | (b << 6));
    WI(d, regDPG0_DPG_COLOUR_G_Y, opp, OPP_S, (g << 6) << 16 | (g << 6));
    WI(d, regDPG0_DPG_COLOUR_R_CR, opp, OPP_S, (r << 6) << 16 | (r << 6));
}

static void opp2_program_dpg_dimensions(rdna3_dev *d, uint32_t opp, uint32_t width, uint32_t height)
{
    WI(d, regDPG0_DPG_DIMENSIONS, opp, OPP_S,
       FIELD(DPG0_DPG_DIMENSIONS, DPG_ACTIVE_WIDTH, width) | FIELD(DPG0_DPG_DIMENSIONS, DPG_ACTIVE_HEIGHT, height));
}

/* opp2_set_disp_pattern_generator, CONTROLLER_DP_TEST_PATTERN_SOLID_COLOR
 * (black) and _VIDEOMODE (off) */
static void opp2_set_disp_pattern_generator(rdna3_dev *d, uint32_t opp, int solid, uint32_t width, uint32_t height,
                                            uint32_t offset)
{
    uint32_t v;
    opp2_program_dpg_dimensions(d, opp, width, height);
    WI(d, regDPG0_DPG_OFFSET_SEGMENT, opp, OPP_S, FIELD(DPG0_DPG_OFFSET_SEGMENT, DPG_X_OFFSET, offset));
    if (!solid) {
        WI(d, regDPG0_DPG_CONTROL, opp, OPP_S, 0);
        WI(d, regDPG0_DPG_COLOUR_R_CR, opp, OPP_S, 0);
        WI(d, regDPG0_DPG_COLOUR_G_Y, opp, OPP_S, 0);
        WI(d, regDPG0_DPG_COLOUR_B_CB, opp, OPP_S, 0);
        WI(d, regDPG0_DPG_RAMP_CONTROL, opp, OPP_S, 0);
        return;
    }
    opp2_dpg_set_blank_color(d, opp, 0, 0, 0);
    v = RI(d, regDPG0_DPG_CONTROL, opp, OPP_S);
    v = SET(v, DPG0_DPG_CONTROL, DPG_EN, 1);
    v = SET(v, DPG0_DPG_CONTROL, DPG_MODE, DPG_MODE_HORIZONTALBARS);
    WI(d, regDPG0_DPG_CONTROL, opp, OPP_S, v);
    opp2_program_dpg_dimensions(d, opp, width, height);
}

static int opp2_dpg_is_blanked(rdna3_dev *d, uint32_t opp)
{
    return GET(RI(d, regDPG0_DPG_CONTROL, opp, OPP_S), DPG0_DPG_CONTROL, DPG_EN) == 1 &&
           GET(RI(d, regDPG0_DPG_STATUS, opp, OPP_S), DPG0_DPG_STATUS, DPG_DOUBLE_BUFFER_PENDING) == 0;
}

static int opp2_dpg_is_pending(rdna3_dev *d, uint32_t opp)
{
    return GET(RI(d, regDPG0_DPG_CONTROL, opp, OPP_S), DPG0_DPG_CONTROL, DPG_EN) == 1 &&
           GET(RI(d, regDPG0_DPG_STATUS, opp, OPP_S), DPG0_DPG_STATUS, DPG_DOUBLE_BUFFER_PENDING) == 1;
}

void dcn32_opp_dpg_blank(rdna3_dev *d, uint32_t opp, uint32_t width, uint32_t height)
{
    opp2_set_disp_pattern_generator(d, opp, 1, width, height, 0);
}

void dcn32_opp_dpg_off(rdna3_dev *d, uint32_t opp)
{
    opp2_set_disp_pattern_generator(d, opp, 0, 0, 0, 0);
}

int dcn32_opp_dpg_is_blanked(rdna3_dev *d, uint32_t opp)
{
    return opp2_dpg_is_blanked(d, opp);
}

int dcn32_wait_for_blank_complete(rdna3_dev *d, uint32_t opp)
{
    uint32_t i;
    for (i = 0; i < 1000; i++) {
        if (!opp2_dpg_is_pending(d, opp))
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 100);
    }
    RERR(d, "subvp: OPP %u DPG blank still pending after 100 ms", opp);
    return RDNA3_ETIME;
}

int dcn32_init_blank(rdna3_dev *d, uint32_t otg)
{
    uint32_t opp[4] = { 0, 0, 0, 0 }, n = dcn32_odm_get_config(d, otg, opp), w = 0, h = 0;
    if (n == 0 || opp[0] >= RD_DCN_MAX_PIPES)
        return RDNA3_EINVAL;
    dcn32_otg_get_active_size(d, otg, &w, &h);
    if (n == 2) {
        w /= 2;
        if (opp[1] >= RD_DCN_MAX_PIPES)
            return RDNA3_EINVAL;
    }
    opp2_set_disp_pattern_generator(d, opp[0], 1, w, h, 0);
    if (n == 2)
        opp2_set_disp_pattern_generator(d, opp[1], 1, w, h, 1);
    return dcn32_wait_for_blank_complete(d, opp[0]);
}

int dcn32_blank_phantom(rdna3_dev *d, uint32_t otg, uint32_t width, uint32_t height)
{
    uint32_t opp[4] = { 0, 0, 0, 0 };
    dcn32_odm_get_config(d, otg, opp);
    if (opp[0] >= RD_DCN_MAX_PIPES)
        return RDNA3_EINVAL;
    opp2_set_disp_pattern_generator(d, opp[0], 1, width, height, 0);
    if (dcn32_otg_is_enabled(d, otg))
        return dcn32_wait_for_blank_complete(d, opp[0]);
    return RDNA3_OK;
}

/* ---- the plane's input color (dcn32_hwseq.c, dcn30_dpp.c, dcn32_mpc.c) ---- */

#define MCM_S 0x90u   /* MPCC_MCM1 - MPCC_MCM0 (dcn_3_2_0_offset.h) */

uint32_t dcn32_pre_degam_tf(uint32_t pre_degam)
{
    static const uint32_t tf[] = { RD_TF_SRGB, RD_TF_GAMMA22, RD_TF_GAMMA24, RD_TF_GAMMA26, RD_TF_BT709, RD_TF_PQ,
                                   RD_TF_HLG };
    uint32_t sel = GET(pre_degam, CNVC_CFG0_PRE_DEGAM, PRE_DEGAM_SELECT);
    if (!GET(pre_degam, CNVC_CFG0_PRE_DEGAM, PRE_DEGAM_MODE) || sel >= sizeof(tf) / sizeof(tf[0]))
        return RD_TF_UNITY;
    return tf[sel];
}

/* dpp3_set_pre_degam */
static void dpp3_set_pre_degam(rdna3_dev *d, uint32_t dpp, uint32_t tf)
{
    uint32_t en = 1, sel = 0;
    switch (tf) {
    case RD_TF_SRGB:    sel = 0; break;
    case RD_TF_GAMMA22: sel = 1; break;
    case RD_TF_GAMMA24: sel = 2; break;
    case RD_TF_GAMMA26: sel = 3; break;
    case RD_TF_BT709:   sel = 4; break;
    case RD_TF_PQ:      sel = 5; break;
    case RD_TF_HLG:     sel = 6; break;
    default:            en = 0;   /* LINEAR / UNITY: bypass */
    }
    WI(d, regCNVC_CFG0_PRE_DEGAM, dpp, DPP_S,
       FIELD(CNVC_CFG0_PRE_DEGAM, PRE_DEGAM_MODE, en) | FIELD(CNVC_CFG0_PRE_DEGAM, PRE_DEGAM_SELECT, sel));
}

int dcn32_set_mcm_luts(rdna3_dev *d, uint32_t mpcc)
{
    /* no blend_tf: mpc32_program_1dlut(NULL) -> 1D LUT bypass */
    WI(d, regMPCC_MCM0_MPCC_MCM_1DLUT_CONTROL, mpcc, MCM_S, 0);
    /* no in_shaper_func: mpc32_program_shaper(NULL) -> bypass */
    WI(d, regMPCC_MCM0_MPCC_MCM_SHAPER_CONTROL, mpcc, MCM_S, 0);
    /* lut3d_func not initialized: mpc32_program_3dlut(NULL) -> bypass */
    WI(d, regMPCC_MCM0_MPCC_MCM_3DLUT_MODE, mpcc, MCM_S, 0);
    return 0;
}

int dcn32_set_input_transfer_func(rdna3_dev *d, uint32_t dpp, uint32_t mpcc, uint32_t tf)
{
    uint32_t v;
    /* in_transfer_func TF_TYPE_PREDEFINED -> its tf, else UNITY */
    dpp3_set_pre_degam(d, dpp, tf);
    /* no HWPWL / distributed points curve: dpp3_program_gamcor_lut(NULL) */
    v = RI(d, regCM0_CM_GAMCOR_CONTROL, dpp, DPP_S);
    WI(d, regCM0_CM_GAMCOR_CONTROL, dpp, DPP_S, SET(v, CM0_CM_GAMCOR_CONTROL, CM_GAMCOR_MODE, 0));
    dcn32_set_mcm_luts(d, mpcc);
    return 1;
}

int dcn32_set_mpc_shaper_3dlut(rdna3_dev *d, uint32_t mpcc, int lut3d_initialized)
{
    (void)d;
    (void)mpcc;
    (void)lut3d_initialized;
    /* only a stream's initialized 3D LUT (amdgpu_dm's CRTC LUT3D) is
     * programmed here, with its shaper; macOS gives the stream none, so
     * DC's result stays false and nothing is written */
    return 0;
}

/* ---- HUBP / OTG (dcn32_hubp.c, dcn32_optc.c) ---- */

void hubp32_phantom_hubp_post_enable(rdna3_dev *d, uint32_t pipe)
{
    uint32_t v = RI(d, regHUBPREQ0_DCSURF_FLIP_CONTROL2, pipe, HUBP_S);
    /* "For phantom pipe enable, disable GSL" */
    WI(d, regHUBPREQ0_DCSURF_FLIP_CONTROL2, pipe, HUBP_S, SET(v, HUBPREQ0_DCSURF_FLIP_CONTROL2, SURFACE_GSL_ENABLE, 0));
    v = RI(d, regHUBP0_DCHUBP_CNTL, pipe, HUBP_S);
    WI(d, regHUBP0_DCHUBP_CNTL, pipe, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 1));
    if (RI(d, regHUBP0_DCHUBP_CNTL, pipe, HUBP_S)) {
        /* a gated HUBP reads 0: no wait then */
        if (rdna3_wait_reg(d, AI(d, regHUBP0_DCHUBP_CNTL, pipe, HUBP_S), HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK,
                           HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK, 0, 200))
            RLOG(d, "subvp: phantom HUBP %u still has requests outstanding", pipe);
    }
}

void hubp32_prepare_subvp_buffering(rdna3_dev *d, uint32_t pipe, int enable)
{
    uint32_t v = RI(d, regHUBP0_DCHUBP_VMPG_CONFIG, pipe, HUBP_S);
    WI(d, regHUBP0_DCHUBP_VMPG_CONFIG, pipe, HUBP_S, SET(v, HUBP0_DCHUBP_VMPG_CONFIG, FORCE_ONE_ROW_FOR_FRAME, enable ? 1 : 0));
    /* "the cursor does not use the MALL (it ignores MALL_SEL)" */
    v = RI(d, regCURSOR0_0_CURSOR_CONTROL, pipe, HUBP_S);
    WI(d, regCURSOR0_0_CURSOR_CONTROL, pipe, HUBP_S, SET(v, CURSOR0_0_CURSOR_CONTROL, CURSOR_REQ_MODE, enable ? 1 : 0));
}

void optc32_phantom_crtc_post_enable(rdna3_dev *d, uint32_t otg)
{
    uint32_t v = RI(d, regOTG0_OTG_CONTROL, otg, OTG_S);
    /* "Disable immediately." */
    v = SET(v, OTG0_OTG_CONTROL, OTG_DISABLE_POINT_CNTL, 0);
    WI(d, regOTG0_OTG_CONTROL, otg, OTG_S, SET(v, OTG0_OTG_CONTROL, OTG_MASTER_EN, 0));
    if (rdna3_wait_reg(d, AI(d, regOTG0_OTG_CLOCK_CONTROL, otg, OTG_S), 0, OTG0_OTG_CLOCK_CONTROL__OTG_BUSY_MASK, 0,
                       100000))
        RERR(d, "subvp: phantom OTG %u still busy", otg);
}

void optc32_disable_phantom_otg(rdna3_dev *d, uint32_t otg)
{
    uint32_t v = RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, otg, ODM_S);
    v = SET(v, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL, 0xf);
    v = SET(v, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG1_SRC_SEL, 0xf);
    v = SET(v, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG2_SRC_SEL, 0xf);
    v = SET(v, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG3_SRC_SEL, 0xf);
    v = SET(v, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_NUM_OF_INPUT_SEGMENT, 0);
    WI(d, regODM0_OPTC_DATA_SOURCE_SELECT, otg, ODM_S, v);
    v = RI(d, regOTG0_OTG_CONTROL, otg, OTG_S);
    WI(d, regOTG0_OTG_CONTROL, otg, OTG_S, SET(v, OTG0_OTG_CONTROL, OTG_MASTER_EN, 0));
}

uint32_t dcn32_apply_update_flags_for_phantom(int otg_master)
{
    uint32_t f = RD_PIPE_UPD_ENABLE | RD_PIPE_UPD_MPCC | RD_PIPE_UPD_DPPCLK | RD_PIPE_UPD_HUBP_INTERDEP |
                 RD_PIPE_UPD_HUBP_RQ_DLG | RD_PIPE_UPD_GAMUT_REMAP | RD_PIPE_UPD_SCALER | RD_PIPE_UPD_VIEWPORT |
                 RD_PIPE_UPD_DET_SIZE;
    if (otg_master)
        f |= RD_PIPE_UPD_ODM | RD_PIPE_UPD_GLOBAL_SYNC;
    return f;
}

void dcn32_update_phantom_vp_position(rdna3_dev *d, rdna3_subvp_phantom *ph, uint32_t main_vp_y)
{
    uint32_t v;
    if (ph->vp_y == main_vp_y)
        return;
    /* src_rect.y of the main plane; resource_build_scaling_params for the
     * phantom: its viewport starts there too */
    ph->vp_y = main_vp_y;
    v = RI(d, regHUBP0_DCSURF_PRI_VIEWPORT_START, ph->pipe, HUBP_S);
    WI(d, regHUBP0_DCSURF_PRI_VIEWPORT_START, ph->pipe, HUBP_S,
       SET(v, HUBP0_DCSURF_PRI_VIEWPORT_START, PRI_VIEWPORT_Y_START, main_vp_y));
}

/* ---- the phantom pipe ---- */

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
        RERR(d, "subvp: pipe %u power %s timed out", pipe, on ? "on" : "off");
    return ret;
}

static int det_zero(rdna3_dev *d, uint32_t pipe)
{
    uint32_t v = RI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S);
    WI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S, SET(v, DCHUBBUB_DET0_CTRL, DET0_SIZE, 0));
    return rdna3_wait_reg(d, AI(d, regDCHUBBUB_DET0_CTRL, pipe, DET_S), 0, DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT_MASK, 0,
                          1000);
}

/* dcn31_program_pix_clk for SIGNAL_TYPE_VIRTUAL: the DP DTO at the main
 * stream's pixel clock (phase / modulo in Hz, PIPE_DTO_SRC_SEL 1), K1 1 /
 * K2 4 (dcn32_calculate_dccg_k1_k2_values, one pixel per container) */
static void phantom_pix_clk(rdna3_dev *d, uint32_t otg, uint32_t pclk_khz, uint32_t dprefclk_khz, int on)
{
    uint32_t v = RI(d, regOTG0_PIXEL_RATE_CNTL, otg, PRC_S);
    if (on) {
        WI(d, regDP_DTO0_PHASE, otg, PRC_S, pclk_khz * 1000);
        WI(d, regDP_DTO0_MODULO, otg, PRC_S, dprefclk_khz * 1000);
        v = SET(v, OTG0_PIXEL_RATE_CNTL, DP_DTO0_ENABLE, 1);
        v = SET(v, OTG0_PIXEL_RATE_CNTL, PIPE0_DTO_SRC_SEL, 1);
        WI(d, regOTG0_PIXEL_RATE_CNTL, otg, PRC_S, v);
        dcn32_dccg_set_pixel_rate_div(d, otg, RD_PIXEL_RATE_DIV_BY_1, RD_PIXEL_RATE_DIV_BY_4);
    } else {
        WI(d, regOTG0_PIXEL_RATE_CNTL, otg, PRC_S, SET(v, OTG0_PIXEL_RATE_CNTL, DP_DTO0_ENABLE, 0));
    }
}

/* the phantom's OTG: the main OTG's horizontal timing and global sync,
 * the phantom's vertical timing (optc1_program_timing, optc1_set_vtg_params) */
static void phantom_timing(rdna3_dev *d, const rdna3_subvp_phantom *ph, const rdna3_pstate_plane *p)
{
    uint32_t q = ph->pipe, mo = ph->main_otg, v, blank_start, blank_end, fp2 = 0;
    int32_t line_start;
    COPY(d, otg, OTG, mo, q);
    WI(d, regOTG0_OTG_V_TOTAL, q, OTG_S, p->v_total - 1);
    WI(d, regOTG0_OTG_V_TOTAL_MIN, q, OTG_S, p->v_total - 1);
    WI(d, regOTG0_OTG_V_TOTAL_MAX, q, OTG_S, p->v_total - 1);
    WI(d, regOTG0_OTG_V_TOTAL_CONTROL, q, OTG_S, 0);
    wr(d, RD_CLONE_OTG_SEG, 0x1b9a /* OTG0_OTG_DRR_CONTROL */ + OTG_S * q, 0);
    blank_start = p->v_total - p->v_front;
    blank_end = blank_start - p->v_active;
    WI(d, regOTG0_OTG_V_BLANK_START_END, q, OTG_S,
       FIELD(OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_START, blank_start) |
           FIELD(OTG0_OTG_V_BLANK_START_END, OTG_V_BLANK_END, blank_end));
    v = RI(d, regOTG0_OTG_VSTARTUP_PARAM, q, OTG_S);
    WI(d, regOTG0_OTG_VSTARTUP_PARAM, q, OTG_S, SET(v, OTG0_OTG_VSTARTUP_PARAM, VSTARTUP_START, p->vstartup));
    /* the VTG: VCOUNT_INIT at the blank start, FP2 when VSTARTUP reaches
     * past the blank end */
    line_start = (int32_t)blank_end - (int32_t)p->vstartup + 1;
    if (line_start < 0)
        fp2 = (uint32_t)-line_start;
    v = RI(d, regVTG0_CONTROL, mo, VTG_S);
    v = SET(v, VTG0_CONTROL, VTG0_FP2, fp2);
    v = SET(v, VTG0_CONTROL, VTG0_VCOUNT_INIT, blank_start);
    WI(d, regVTG0_CONTROL, q, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 0));
    WI(d, regVTG0_CONTROL, q, VTG_S, SET(v, VTG0_CONTROL, VTG0_ENABLE, 1));
    /* the ODM input: one segment from OPP q (optc32_set_odm_bypass), no
     * memory of its own */
    COPY(d, odm, ODM, mo, q);
    v = RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, mo, ODM_S);
    v = SET(v, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_NUM_OF_INPUT_SEGMENT, 0);
    v = SET(v, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL, q);
    v = SET(v, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG1_SRC_SEL, 0xf);
    WI(d, regODM0_OPTC_DATA_SOURCE_SELECT, q, ODM_S, v);
    WI(d, regODM0_OPTC_MEMORY_CONFIG, q, ODM_S, 0);
}

/* the phantom's plane: the main pipe's HUBP / DPP / DLG with the phantom
 * viewport height and vertical DLG terms (see the header) */
static void phantom_plane(rdna3_dev *d, const rdna3_subvp_phantom *ph, const rdna3_pstate_plane *m,
                          const rdna3_pstate_plane *p)
{
    const uint32_t keep_hubp = HUBP0_DCHUBP_CNTL__HUBP_DISABLE_STOP_DATA_DURING_VM_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK | HUBP0_DCHUBP_CNTL__HUBP_TTU_MODE_MASK |
                               HUBP0_DCHUBP_CNTL__HUBP_TIMEOUT_THRESHOLD_MASK;
    uint32_t q = ph->pipe, mp = ph->main_pipe, v, ctl, k, nds;
    /* dccg2_update_dpp_dto: the main pipe's DPP clock */
    WI(d, regDPPCLK0_DTO_PARAM, q, DPPDTO_S, RI(d, regDPPCLK0_DTO_PARAM, mp, DPPDTO_S));
    ctl = RI(d, regDPPCLK_DTO_CTRL, 0, 0);
    k = (ctl >> (4 * mp)) & 3u;
    WI(d, regDPPCLK_DTO_CTRL, 0, 0, (ctl & ~(3u << (4 * q))) | (k << (4 * q)));
    COPY(d, dpp, DPP, mp, q);
    /* the phantom plane is a copy of the main's: its predefined degamma;
     * no GAMCOR curve (the LUT RAM is not copied) and no MCM LUTs */
    dcn32_set_input_transfer_func(d, q, q, dcn32_pre_degam_tf(RI(d, regCNVC_CFG0_PRE_DEGAM, mp, DPP_S)));
    COPY(d, hubp, HUBP, mp, q);
    /* blanked, on the phantom OTG, VREADY before VSYNC (one front porch
     * line: VREADY_AT_OR_AFTER_VSYNC false), not unbounded (DET 0) */
    WI(d, regHUBP0_DCHUBP_CNTL, q, HUBP_S,
       (RI(d, regHUBP0_DCHUBP_CNTL, mp, HUBP_S) & keep_hubp) | HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK |
           (q << HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL__SHIFT));
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, q, HUBP_S,
       RI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, mp, HUBP_S));
    WI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, q, HUBP_S,
       RI(d, regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, mp, HUBP_S));
    /* viewport: the main's position, the phantom's source height */
    v = RI(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, mp, HUBP_S);
    WI(d, regHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, q, HUBP_S,
       SET(v, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, PRI_VIEWPORT_HEIGHT, p->src_h));
    v = RI(d, regDSCL0_RECOUT_SIZE, mp, DPP_S);
    WI(d, regDSCL0_RECOUT_SIZE, q, DPP_S, SET(v, DSCL0_RECOUT_SIZE, RECOUT_HEIGHT, p->v_active));
    v = RI(d, regDSCL0_MPC_SIZE, mp, DPP_S);
    WI(d, regDSCL0_MPC_SIZE, q, DPP_S, SET(v, DSCL0_MPC_SIZE, MPC_HEIGHT, p->v_active));
    /* DLG: the vblank end, and MIN_DST_Y_NEXT_START = (VTotal - VFrontPorch
     * + 1) x 4 + the TSetup fraction (the main's, the same line time) */
    v = RI(d, regHUBPREQ0_BLANK_OFFSET_0, mp, HUBP_S);
    WI(d, regHUBPREQ0_BLANK_OFFSET_0, q, HUBP_S,
       SET(v, HUBPREQ0_BLANK_OFFSET_0, DLG_V_BLANK_END, p->v_total - p->v_front - p->v_active));
    v = RI(d, regHUBPREQ0_BLANK_OFFSET_1, mp, HUBP_S);
    nds = GET(v, HUBPREQ0_BLANK_OFFSET_1, MIN_DST_Y_NEXT_START);
    if (nds >= 4 * (m->v_total - m->v_front))
        nds = nds - 4 * (m->v_total - m->v_front) + 4 * (p->v_total - p->v_front);
    else
        nds = 4 * (p->v_total - p->v_front + 1);
    WI(d, regHUBPREQ0_BLANK_OFFSET_1, q, HUBP_S, SET(v, HUBPREQ0_BLANK_OFFSET_1, MIN_DST_Y_NEXT_START, nds));
    /* MALL_SEL 1 (dcn32_update_mall_sel), no FORCE_ONE_ROW_FOR_FRAME */
    hubp32_update_mall_sel(d, q, 1, 0);
    hubp32_prepare_subvp_buffering(d, q, 0);
}

static void phantom_mpc_opp(rdna3_dev *d, const rdna3_subvp_phantom *ph, const rdna3_pstate_plane *p)
{
    uint32_t q = ph->pipe, v;
    COPY(d, opp, OPP, ph->main_opp, q);
    dcn32_opp_pipe_clock_control(d, q, 1);
    COPY(d, mpcc, MPCC, ph->main_pipe, q);
    WI(d, regMPCC0_MPCC_TOP_SEL, q, MPCC_S, q);
    WI(d, regMPCC0_MPCC_BOT_SEL, q, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_OPP_ID, q, MPCC_S, q);
    v = RI(d, regMPC_OUT0_MUX, q, MUX_S);
    WI(d, regMPC_OUT0_MUX, q, MUX_S, SET(v, MPC_OUT0_MUX, MPC_OUT_MUX, q));
    /* dcn32_set_output_transfer_func of the phantom stream: no stream
     * shaper / 3D LUT here, nothing programmed */
    dcn32_set_mpc_shaper_3dlut(d, q, 0);
    /* blank_pixel_data(true) before the enable: the DPG black */
    dcn32_opp_dpg_blank(d, q, p->h_active, p->v_active);
}

static void phantom_plane_off(rdna3_dev *d, uint32_t q)
{
    uint32_t v = RI(d, regHUBP0_DCHUBP_CNTL, q, HUBP_S);
    WI(d, regHUBP0_DCHUBP_CNTL, q, HUBP_S, SET(v, HUBP0_DCHUBP_CNTL, HUBP_BLANK_EN, 1));
    v = RI(d, regMPC_OUT0_MUX, q, MUX_S);
    WI(d, regMPC_OUT0_MUX, q, MUX_S, SET(v, MPC_OUT0_MUX, MPC_OUT_MUX, MPCC_NONE));
    WI(d, regMPCC0_MPCC_TOP_SEL, q, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_BOT_SEL, q, MPCC_S, MPCC_NONE);
    WI(d, regMPCC0_MPCC_OPP_ID, q, MPCC_S, MPCC_NONE);
    hubp32_update_mall_sel(d, q, 0, 0);
}

int dcn32_enable_phantom_streams(rdna3_dev *d, const rdna3_pstate_ctx *ctx, rdna3_subvp_state *sv,
                                 uint32_t dprefclk_khz)
{
    uint32_t i, v;
    int r;
    if (!dprefclk_khz)
        return RDNA3_EINVAL;
    for (i = 0; i < sv->n; i++) {
        rdna3_subvp_phantom *ph = &sv->ph[i];
        const rdna3_pstate_plane *m = &ctx->plane[ph->main], *p = &ctx->plane[ph->phantom];
        uint32_t q = ph->pipe;
        /* a pipe that runs a head is never taken (dcn32_subvp_validate):
         * no double-buffer wait for a pipe turning phantom */
        /* dcn32_apply_update_flags_for_phantom: the phantom (an OTG
         * master) takes every step of a full enable */
        uint32_t upd = dcn32_apply_update_flags_for_phantom(1);
        r = pipe_power(d, q, 1);
        if (!r && (upd & RD_PIPE_UPD_DET_SIZE))
            r = det_zero(d, q);   /* "Phantom pipe requires that DET_SIZE = 0" */
        if (r)
            return r;
        phantom_pix_clk(d, q, p->pclk_khz, dprefclk_khz, 1);
        if (upd & (RD_PIPE_UPD_ODM | RD_PIPE_UPD_GLOBAL_SYNC))
            phantom_timing(d, ph, p);
        if (upd & (RD_PIPE_UPD_ENABLE | RD_PIPE_UPD_HUBP_RQ_DLG | RD_PIPE_UPD_VIEWPORT))
            phantom_plane(d, ph, m, p);
        if (upd & RD_PIPE_UPD_MPCC)
            phantom_mpc_opp(d, ph, p);
        ph->vp_y = GET(RI(d, regHUBP0_DCSURF_PRI_VIEWPORT_START, ph->main_pipe, HUBP_S), HUBP0_DCSURF_PRI_VIEWPORT_START,
                       PRI_VIEWPORT_Y_START);
        /* optc32_enable_crtc, then dcn20_enable_stream_timing's
         * phantom_crtc_post_enable and the HUBP's post-enable */
        v = RI(d, regOTG0_OTG_CONTROL, q, OTG_S);
        v = SET(v, OTG0_OTG_CONTROL, OTG_DISABLE_POINT_CNTL, 2);
        WI(d, regOTG0_OTG_CONTROL, q, OTG_S, SET(v, OTG0_OTG_CONTROL, OTG_MASTER_EN, 1));
        optc32_phantom_crtc_post_enable(d, q);
        hubp32_phantom_hubp_post_enable(d, q);
        RLOG(d, "subvp: phantom of pipe %u (OTG %u, %ux%u) on pipe %u: %u lines (vtotal %u, back porch %u, VSTARTUP %u), "
             "MALL index %u", ph->main_pipe, ph->main_otg, m->h_active, m->v_active, q, p->v_active, p->v_total, ph->vbp,
             p->vstartup, ph->subvp_index);
    }
    /* dcn32_program_mall_pipe_config: FORCE_ONE_ROW_FOR_FRAME and
     * CURSOR_REQ_MODE on the mains */
    for (i = 0; i < sv->n; i++)
        hubp32_prepare_subvp_buffering(d, sv->ph[i].main_pipe, 1);
    sv->enabled = 1;
    return RDNA3_OK;
}

void dcn32_disable_phantom_streams(rdna3_dev *d, rdna3_subvp_state *sv)
{
    uint32_t i, v;
    if (!sv->enabled)
        return;
    for (i = sv->n; i-- > 0;) {
        const rdna3_subvp_phantom *ph = &sv->ph[i];
        uint32_t q = ph->pipe;
        hubp32_prepare_subvp_buffering(d, ph->main_pipe, 0);
        /* dc_commit_state_no_check: blank, the phantom OTG on so the plane
         * disable gets its double-buffer update, the plane off, then
         * disable_phantom_crtc (at the falling VUPDATE) */
        dcn32_blank_phantom(d, q, ph->hactive, ph->vactive);
        v = RI(d, regOTG0_OTG_CONTROL, q, OTG_S);
        v = SET(v, OTG0_OTG_CONTROL, OTG_DISABLE_POINT_CNTL, 2);
        WI(d, regOTG0_OTG_CONTROL, q, OTG_S, SET(v, OTG0_OTG_CONTROL, OTG_MASTER_EN, 1));
        phantom_plane_off(d, q);
        /* one phantom frame for the update (well under 10 ms) */
        d->os.msleep(d->os.ctx, 10);
        optc32_disable_phantom_otg(d, q);
        rdna3_wait_reg(d, AI(d, regOTG0_OTG_CLOCK_CONTROL, q, OTG_S), 0, OTG0_OTG_CLOCK_CONTROL__OTG_BUSY_MASK, 0, 100000);
        /* reset_back_end_for_pipe / enable_stream_gating / cs_power_down,
         * then dcn20_plane_atomic_disable: clocks off, the DPG off, gated */
        dcn32_otg_enable_optc_clock(d, q, 0);
        phantom_pix_clk(d, q, 0, 0, 0);
        dcn32_opp_dpg_off(d, q);
        dcn32_opp_pipe_clock_control(d, q, 0);
        v = RI(d, regHUBP0_HUBP_CLK_CNTL, q, HUBP_S);
        WI(d, regHUBP0_HUBP_CLK_CNTL, q, HUBP_S, SET(v, HUBP0_HUBP_CLK_CNTL, HUBP_CLOCK_ENABLE, 0));
        v = RI(d, regDPP_TOP0_DPP_CONTROL, q, DPP_S);
        WI(d, regDPP_TOP0_DPP_CONTROL, q, DPP_S, SET(v, DPP_TOP0_DPP_CONTROL, DPP_CLOCK_ENABLE, 0));
        v = RI(d, regDPPCLK_DTO_CTRL, 0, 0);
        WI(d, regDPPCLK_DTO_CTRL, 0, 0, v & ~(3u << (4 * q)));
        pipe_power(d, q, 0);
        RLOG(d, "subvp: phantom pipe %u off", q);
    }
    sv->enabled = 0;
}

/* ---- topology transitions ---- */

int is_subvp_phantom_topology_transition_seamless(const rdna3_pstate_ctx *cur, const rdna3_pstate_ctx *next,
                                                  uint32_t cur_k, uint32_t next_k)
{
    const rdna3_pstate_plane *a = &cur->plane[cur_k], *b = &next->plane[next_k];
    /* both phantoms, paired with the same main stream (the same head on
     * the same OTG with the same timing: this driver's stream_id) */
    const rdna3_pstate_plane *am, *bm;
    if (a->mall != RD_MALL_PSTATE_PHANTOM || b->mall != RD_MALL_PSTATE_PHANTOM || !a->paired || !b->paired ||
        a->paired > cur->nplanes || b->paired > next->nplanes)
        return 0;
    am = &cur->plane[a->paired - 1];
    bm = &next->plane[b->paired - 1];
    return am->otg == bm->otg && am->h_active == bm->h_active && am->v_active == bm->v_active &&
           am->pclk_khz == bm->pclk_khz;
}

int dcn32_is_pipe_topology_transition_seamless(const rdna3_pstate_ctx *cur, const rdna3_pstate_ctx *next)
{
    uint32_t p, i, j;
    for (p = 0; p < RD_DCN_MAX_PIPES; p++) {
        uint32_t ci = RD_DCN_MAX_PIPES, ni = RD_DCN_MAX_PIPES;
        for (i = 0; i < cur->nplanes; i++)
            if (cur->plane[i].pipe == p)
                ci = i;
        for (j = 0; j < next->nplanes; j++)
            if (next->plane[j].pipe == p)
                ni = j;
        /* adding or removing free pipes is always seamless */
        if (ci == RD_DCN_MAX_PIPES || ni == RD_DCN_MAX_PIPES)
            continue;
        /* OTG master with the same stream (OTG and size), or the same phantom */
        if (cur->plane[ci].otg == next->plane[ni].otg && cur->plane[ci].mall != RD_MALL_PSTATE_PHANTOM &&
            next->plane[ni].mall != RD_MALL_PSTATE_PHANTOM && cur->plane[ci].h_active == next->plane[ni].h_active &&
            cur->plane[ci].v_active == next->plane[ni].v_active && cur->plane[ci].pclk_khz == next->plane[ni].pclk_khz)
            continue;
        if (is_subvp_phantom_topology_transition_seamless(cur, next, ci, ni))
            continue;
        return 0;
    }
    return 1;
}

/* ---- the commit (dc_commit_state_no_check / commit_planes_for_stream) ---- */

static int same_phantoms(const rdna3_subvp_state *a, const rdna3_subvp_state *b)
{
    uint32_t i;
    if (!a->enabled || a->n != b->n)
        return 0;
    for (i = 0; i < a->n; i++)
        if (a->ph[i].pipe != b->ph[i].pipe || a->ph[i].main_pipe != b->ph[i].main_pipe ||
            a->ph[i].main_otg != b->ph[i].main_otg || a->ph[i].vactive != b->ph[i].vactive ||
            a->ph[i].vtotal != b->ph[i].vtotal || a->ph[i].hactive != b->ph[i].hactive)
            return 0;
    return 1;
}

int dcn32_subvp_index_of(const rdna3_subvp_state *sv, uint32_t main_pipe)
{
    uint32_t i;
    for (i = 0; sv && sv->enabled && i < sv->n; i++)
        if (sv->ph[i].main_pipe == main_pipe)
            return (int)sv->ph[i].subvp_index;
    return -1;
}

int dcn32_subvp_commit(rdna3_dev *d, rdna3_dmub *dm, rdna3_dcn32_power *pw, rdna3_pstate_ctx *ctx,
                       rdna3_subvp_state *cur, rdna3_subvp_state *next, uint32_t dprefclk_khz)
{
    int in_use = next->n > 0, prev = cur->enabled, same = same_phantoms(cur, next), r, e;
    uint32_t i;

    if ((in_use || prev) && (!dm || !dm->hw_init))
        return RDNA3_ENODEV;
    /* the DMCUB HW lock for a full update while SubVP runs or ran */
    next->locks = cur->locks;
    e = dcn32_subvp_pipe_control_lock(dm, next, in_use, prev, 1);
    if (e)
        RERR(d, "subvp: DMCUB HW lock: %d", e);
    r = dcn32_power_prepare(d, dm, pw, ctx);
    if (prev && !same)
        dcn32_disable_phantom_streams(d, cur);
    if (in_use && !same) {
        e = dcn32_enable_phantom_streams(d, ctx, next, dprefclk_khz);
        if (e) {
            RERR(d, "subvp: phantom bring-up failed (%d): SubVP off", e);
            next->enabled = 1;
            dcn32_disable_phantom_streams(d, next);
            next->n = 0;
            in_use = 0;
            if (!r)
                r = e;
        }
    } else if (in_use) {
        for (i = 0; i < next->n; i++)
            next->ph[i].vp_y = cur->ph[i].vp_y;
        next->enabled = 1;
    }
    /* dcn32_commit_subvp_config: the pipes to the firmware (the header
     * alone when SubVP ends) */
    if (in_use || prev) {
        e = dcn32_commit_subvp_config(dm, ctx, next, pw->refclk_mhz);
        if (e) {
            RERR(d, "subvp: FW_ASSISTED_MCLK_SWITCH / HANDLE_SUBVP_CMD: %d", e);
            if (!r)
                r = e;
        }
        /* unlock: the SubVP mains in vblank first */
        for (i = 0; i < next->n && in_use; i++)
            dcn32_otg_wait_for_state(d, next->ph[i].main_otg, RD_CRTC_STATE_VBLANK);
        e = dcn32_subvp_pipe_control_lock(dm, next, in_use, prev, 0);
        if (e)
            RERR(d, "subvp: DMCUB HW unlock: %d", e);
    }
    e = dcn32_power_optimize(d, pw, ctx);
    if (e && !r)
        r = e;
    *cur = *next;
    RLOG(d, "subvp: %s, %u phantom(s), %u DMCUB lock commands: %d",
         in_use ? "on" : prev ? "off (was on)" : "not used", cur->n, cur->locks, r);
    return r;
}
