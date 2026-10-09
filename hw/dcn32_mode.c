/*
 * dcn32_mode.c - see dcn32_mode.h. Formulas from Linux amdgpu Display Core
 * (dcn10_optc.c optc1_program_timing, link bandwidth helpers), MIT.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_mode.h"

int dcn32_otg_timing(const rdna3_mode *m, int dp, rdna3_otg_timing *t)
{
    uint32_t i, vfp;
    for (i = 0; i < sizeof(*t); i++)
        ((uint8_t *)t)[i] = 0;
    if (!m->pixel_khz || !m->h_total || !m->v_total || m->h_active >= m->h_total ||
        m->v_active >= m->v_total)
        return RDNA3_EINVAL;
    /* apply_front_porch_workaround */
    vfp = m->v_front;
    if (m->interlaced ? vfp < 2 : vfp < 1)
        vfp = m->interlaced ? 2 : 1;

    t->h_total = m->h_total - 1;
    t->h_sync_start = 0;
    t->h_sync_end = m->h_sync;
    /* blank_start = line end - front porch, blank_end = blank_start - active */
    t->h_blank_start = m->h_total - m->h_front;
    t->h_blank_end = t->h_blank_start - m->h_active;
    t->h_sync_pol = m->h_sync_pos ? 0 : 1;

    t->v_total = m->v_total - 1;
    t->v_sync_start = 0;
    t->v_sync_end = m->v_sync;
    t->v_blank_start = m->v_total - vfp;
    t->v_blank_end = t->v_blank_start - m->v_active;
    t->v_sync_pol = m->v_sync_pos ? 0 : 1;

    t->interlace = m->interlaced ? 1 : 0;
    if (dp) {
        t->start_point = 1;
        t->field_num = m->interlaced ? 1 : 0;
    }
    return RDNA3_OK;
}

uint64_t dcn32_dp_link_kbps(uint32_t link_rate_code, uint32_t lanes, int downspread)
{
    /* LINK_RATE_REF_FREQ_IN_KHZ 27000: a rate code counts 0.27 Gbps of raw
     * line rate per lane, i.e. code * 27000 kB/s of 8b/10b payload */
    uint64_t kbps = (uint64_t)link_rate_code * 27000ull * 8ull * lanes;
    if (downspread)
        kbps = kbps * 995 / 1000;
    return kbps;
}

uint64_t dcn32_mode_kbps(const rdna3_mode *m, uint32_t bpp)
{
    return (uint64_t)m->pixel_khz * bpp;
}

int dcn32_mode_check(const rdna3_mode *m, uint32_t link_rate_code, uint32_t lanes, int downspread)
{
    if (!m->pixel_khz || !m->h_total || !m->v_total)
        return RD_MODE_NO_TIMING;
    if (m->interlaced)
        return RD_MODE_INTERLACED;
    if (!m->h_active || !m->v_active || m->h_active > RD_DCN32_MAX_H_ACTIVE || m->v_active > 8192)
        return RD_MODE_SIZE;
    if (m->pixel_khz > RD_DCN32_MAX_PCLK_KHZ)
        return RD_MODE_PIXEL_CLOCK;
    if (link_rate_code && lanes &&
        dcn32_mode_kbps(m, 24) > dcn32_dp_link_kbps(link_rate_code, lanes, downspread))
        return RD_MODE_LINK_BW;
    return RD_MODE_OK;
}

static int near(uint32_t a, uint32_t b, uint32_t permille)
{
    uint64_t d = a > b ? a - b : b - a;
    return d * 1000 <= (uint64_t)(a > b ? a : b) * permille;
}

int dcn32_match_head_mode(const rdna3_dcn_head *h, uint32_t pclk_khz, const rdna3_edid *e)
{
    uint32_t i;
    for (i = 0; i < e->nmodes; i++) {
        const rdna3_mode *m = &e->mode[i];
        if (!m->pixel_khz)
            continue;
        if (m->h_active != h->h_active || m->v_active != h->v_active || m->h_total != h->h_total ||
            m->v_total != h->v_total || m->h_sync != h->h_sync || m->v_sync != h->v_sync ||
            m->h_front != h->h_front)
            continue;
        if (pclk_khz && !near(pclk_khz, m->pixel_khz, 5))
            continue;
        return (int)i;
    }
    return -1;
}
