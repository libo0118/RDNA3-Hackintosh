/*
 * dcn32.c - see dcn32.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_dig.h"
#include "gen/dcn_3_2_0_pipe.h"

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

int dcn32_set_blank(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, int blank)
{
    uint32_t p, cntl, t;
    if (i >= dc->nheads)
        return RDNA3_EINVAL;
    p = dc->head[i].pipe;
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
    return RDNA3_OK;
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
    return inuse != dc->requested[p];
}

int dcn32_cursor_set_image(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, uint64_t mc, uint32_t size)
{
    uint32_t p, v, pitch, lpc;
    if (i >= dc->nheads)
        return RDNA3_EINVAL;
    p = dc->head[i].pipe;
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

int dcn32_cursor_set_position(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, int32_t x, int32_t y,
                              int visible)
{
    const rdna3_dcn_head *h;
    uint32_t p, v, hx = 0, hy = 0, size;
    int on;
    if (i >= dc->nheads)
        return RDNA3_EINVAL;
    h = &dc->head[i];
    p = h->pipe;
    size = dc->cursor_size[p];
    if (!size)
        return RDNA3_EINVAL;   /* hubp2_cursor_set_position: no attributes yet */
    /* the hardware draws the image with its hot spot at POSITION, so the
     * top-left corner is POSITION - HOT_SPOT. POSITION is unsigned: a
     * cursor hanging off the top/left edge gets POSITION 0 and the
     * overhang as hot spot (amdgpu_dm get_cursor_position) */
    if (x < 0) { hx = (uint32_t)(-x); x = 0; }
    if (y < 0) { hy = (uint32_t)(-y); y = 0; }
    on = visible && hx < size && hy < size && (uint32_t)x < h->width && (uint32_t)y < h->height;
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

void dcn32_crc_enable(rdna3_dev *d, const rdna3_dcn *dc, uint32_t i, int enable)
{
    uint32_t o, v;
    if (i >= dc->nheads)
        return;
    o = dc->head[i].otg;
    /* optc1_configure_crc: clear, then whole-frame continuous CRC */
    WO(d, regOTG0_OTG_CRC_CNTL, o, 0);
    if (!enable)
        return;
    v = SET(0u, OTG0_OTG_CRC_CNTL, OTG_CRC_CONT_EN, 1);
    v = SET(v, OTG0_OTG_CRC_CNTL, OTG_CRC_EN, 1);
    WO(d, regOTG0_OTG_CRC_CNTL, o, v);
}

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
