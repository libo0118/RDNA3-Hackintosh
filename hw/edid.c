/*
 * edid.c - see edid.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "edid.h"

int edid_block_valid(const uint8_t *b)
{
    uint32_t i, sum = 0;
    for (i = 0; i < 128; i++)
        sum += b[i];
    return (sum & 0xff) == 0;
}

static void add_mode(rdna3_edid *e, const rdna3_mode *m)
{
    uint32_t i;
    if (!m->h_active || !m->v_active)
        return;
    for (i = 0; i < e->nmodes; i++) {
        rdna3_mode *o = &e->mode[i];
        if (o->h_active == m->h_active && o->v_active == m->v_active &&
            o->interlaced == m->interlaced &&
            (o->refresh_mhz + 500) / 1000 == (m->refresh_mhz + 500) / 1000) {
            if (m->pixel_khz && !o->pixel_khz)
                *o = *m;           /* a detailed timing beats a standard one */
            o->preferred |= m->preferred;
            return;
        }
    }
    if (e->nmodes < RD_EDID_MAX_MODES)
        e->mode[e->nmodes++] = *m;
}

/* 18-byte detailed timing descriptor */
static int parse_dtd(const uint8_t *p, rdna3_mode *m)
{
    uint32_t pclk = (uint32_t)(p[0] | (p[1] << 8)) * 10;   /* kHz */
    uint32_t ha, hb, va, vb, hfp, hsw, vfp, vsw;
    if (!pclk)
        return 0;
    ha = p[2] | ((p[4] & 0xf0) << 4);
    hb = p[3] | ((p[4] & 0x0f) << 8);
    va = p[5] | ((p[7] & 0xf0) << 4);
    vb = p[6] | ((p[7] & 0x0f) << 8);
    hfp = p[8] | ((p[11] & 0xc0) << 2);
    hsw = p[9] | ((p[11] & 0x30) << 4);
    vfp = (p[10] >> 4) | ((p[11] & 0x0c) << 2);
    vsw = (p[10] & 0x0f) | ((p[11] & 0x03) << 4);
    if (!ha || !va || !hb || !vb)
        return 0;
    m->h_active = ha;
    m->v_active = va;
    m->h_total = ha + hb;
    m->v_total = va + vb;
    m->h_front = hfp;
    m->h_sync = hsw;
    m->v_front = vfp;
    m->v_sync = vsw;
    m->pixel_khz = pclk;
    m->interlaced = (p[17] & 0x80) ? 1 : 0;
    /* digital separate sync: bit 4 = 1 and bits 2/1 are the polarities */
    if ((p[17] & 0x18) == 0x18) {
        m->v_sync_pos = (p[17] & 0x04) ? 1 : 0;
        m->h_sync_pos = (p[17] & 0x02) ? 1 : 0;
    } else {
        m->v_sync_pos = m->h_sync_pos = 0;
    }
    /* refresh in mHz: pclk[kHz] * 1e6 / (ht * vt) */
    {
        uint64_t num = (uint64_t)pclk * 1000000ull;
        uint64_t den = (uint64_t)m->h_total * m->v_total;
        if (m->interlaced)
            num *= 2;
        m->refresh_mhz = den ? (uint32_t)(num / den) : 0;
    }
    return 1;
}

static void std_timing(rdna3_edid *e, const uint8_t *p)
{
    rdna3_mode m;
    uint32_t h, v, i;
    if (p[0] == 0x01 && p[1] == 0x01)
        return;
    if (p[0] == 0)
        return;
    h = ((uint32_t)p[0] + 31) * 8;
    switch (p[1] >> 6) {
    case 0: v = (e->version == 1 && e->revision < 3) ? h : h * 10 / 16; break;
    case 1: v = h * 3 / 4; break;
    case 2: v = h * 4 / 5; break;
    default: v = h * 9 / 16; break;
    }
    for (i = 0; i < sizeof(m); i++)
        ((uint8_t *)&m)[i] = 0;
    m.h_active = h;
    m.v_active = v;
    m.refresh_mhz = ((uint32_t)(p[1] & 0x3f) + 60) * 1000;
    add_mode(e, &m);
}

static void established(rdna3_edid *e, const uint8_t *p)
{
    /* bytes 35-37: the classic VESA/IBM/Apple modes */
    static const struct { uint8_t byte, bit; uint16_t h, v, hz; } tab[] = {
        { 0, 7, 720, 400, 70 }, { 0, 6, 720, 400, 88 }, { 0, 5, 640, 480, 60 },
        { 0, 4, 640, 480, 67 }, { 0, 3, 640, 480, 72 }, { 0, 2, 640, 480, 75 },
        { 0, 1, 800, 600, 56 }, { 0, 0, 800, 600, 60 }, { 1, 7, 800, 600, 72 },
        { 1, 6, 800, 600, 75 }, { 1, 5, 832, 624, 75 }, { 1, 4, 1024, 768, 87 },
        { 1, 3, 1024, 768, 60 }, { 1, 2, 1024, 768, 70 }, { 1, 1, 1024, 768, 75 },
        { 1, 0, 1280, 1024, 75 }, { 2, 7, 1152, 870, 75 },
    };
    uint32_t i, k;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (p[tab[i].byte] & (1u << tab[i].bit)) {
            rdna3_mode m;
            for (k = 0; k < sizeof(m); k++)
                ((uint8_t *)&m)[k] = 0;
            m.h_active = tab[i].h;
            m.v_active = tab[i].v;
            m.refresh_mhz = (uint32_t)tab[i].hz * 1000;
            m.interlaced = tab[i].v == 768 && tab[i].hz == 87;
            add_mode(e, &m);
        }
}

static void descriptor(rdna3_edid *e, const uint8_t *p, int first)
{
    rdna3_mode m;
    uint32_t i;
    if (p[0] || p[1]) {
        for (i = 0; i < sizeof(m); i++)
            ((uint8_t *)&m)[i] = 0;
        if (parse_dtd(p, &m)) {
            m.preferred = first ? 1 : 0;
            add_mode(e, &m);
        }
        return;
    }
    switch (p[3]) {
    case 0xfc: {   /* monitor name */
        uint32_t n = 0;
        for (i = 5; i < 18 && p[i] != 0x0a; i++)
            if (p[i] >= 0x20 && p[i] < 0x7f && n < sizeof(e->name) - 1)
                e->name[n++] = (char)p[i];
        while (n && e->name[n - 1] == ' ')
            n--;
        e->name[n] = 0;
        break;
    }
    case 0xfa:     /* six more standard timings */
        for (i = 5; i + 1 < 17; i += 2)
            std_timing(e, p + i);
        break;
    default:
        break;
    }
}

/* CTA-861 extension: detailed timings after the data block collection */
static void cta_block(rdna3_edid *e, const uint8_t *b)
{
    uint32_t off = b[2], i;
    if (b[0] != 0x02 || off < 4 || off > 127 - 18)
        return;
    for (i = off; i + 18 <= 127; i += 18) {
        rdna3_mode m;
        uint32_t k;
        if (!b[i] && !b[i + 1])
            break;
        for (k = 0; k < sizeof(m); k++)
            ((uint8_t *)&m)[k] = 0;
        if (parse_dtd(b + i, &m))
            add_mode(e, &m);
    }
}

int edid_parse(const uint8_t *d, uint32_t len, rdna3_edid *e)
{
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    uint32_t i, blocks;
    uint16_t mfg;

    for (i = 0; i < sizeof(*e); i++)
        ((uint8_t *)e)[i] = 0;
    if (!d || len < 128 || (len & 127))
        return -1;
    for (i = 0; i < 8; i++)
        if (d[i] != hdr[i])
            return -1;
    if (!edid_block_valid(d))
        return -1;

    mfg = (uint16_t)((d[8] << 8) | d[9]);
    e->manufacturer[0] = (char)('A' - 1 + ((mfg >> 10) & 0x1f));
    e->manufacturer[1] = (char)('A' - 1 + ((mfg >> 5) & 0x1f));
    e->manufacturer[2] = (char)('A' - 1 + (mfg & 0x1f));
    e->manufacturer[3] = 0;
    e->product = (uint16_t)(d[10] | (d[11] << 8));
    e->serial = (uint32_t)d[12] | ((uint32_t)d[13] << 8) | ((uint32_t)d[14] << 16) |
                ((uint32_t)d[15] << 24);
    e->week = d[16];
    e->year = d[17];
    e->version = d[18];
    e->revision = d[19];
    e->digital = (d[20] & 0x80) ? 1 : 0;
    e->width_cm = d[21];
    e->height_cm = d[22];

    established(e, d + 35);
    for (i = 38; i < 54; i += 2)
        std_timing(e, d + i);
    for (i = 54; i + 18 <= 126; i += 18)
        descriptor(e, d + i, i == 54 && (d[24] & 0x02));

    blocks = len / 128;
    e->extensions = blocks - 1;
    for (i = 1; i < blocks; i++)
        if (edid_block_valid(d + i * 128))
            cta_block(e, d + i * 128);
    return 0;
}
