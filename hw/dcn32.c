/*
 * dcn32.c - see dcn32.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32.h"
#include "gen/dcn_3_2_1.h"

#define HUBP_STRIDE (regHUBP1_DCHUBP_CNTL - regHUBP0_DCHUBP_CNTL)    /* 0xdc */
#define OTG_STRIDE  (regOTG1_OTG_CONTROL - regOTG0_OTG_CONTROL)      /* 0x80 */

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
        dc->requested[p] = h->address;
        RLOG(d, "DCN: head %u: pipe %u otg %u %ux%u pitch %u fmt %u sw %u dcc %u at MC 0x%llx, "
             "total %ux%u", dc->nheads, p, otg, h->width, h->height, h->pitch_px, h->format,
             h->sw_mode, h->dcc, (unsigned long long)h->address, h->h_total, h->v_total);
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
