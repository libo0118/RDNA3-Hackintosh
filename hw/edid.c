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

static void zero_mode(rdna3_mode *m)
{
    uint32_t i;
    for (i = 0; i < sizeof(*m); i++)
        ((uint8_t *)m)[i] = 0;
}

static uint32_t refresh_mhz(uint32_t pclk_khz, uint32_t ht, uint32_t vt, int interlaced)
{
    uint64_t num = (uint64_t)pclk_khz * 1000000ull * (interlaced ? 2 : 1);
    uint64_t den = (uint64_t)ht * vt;
    return den ? (uint32_t)(num / den) : 0;
}

/* a timing in drm_mode's terms (hdisplay, hsync_start, hsync_end, htotal) */
static void set_timing(rdna3_mode *m, uint32_t pclk, uint32_t ha, uint32_t hss, uint32_t hse, uint32_t ht,
                       uint32_t va, uint32_t vss, uint32_t vse, uint32_t vt, int hpos, int vpos)
{
    m->pixel_khz = pclk;
    m->h_active = ha;
    m->h_front = hss - ha;
    m->h_sync = hse - hss;
    m->h_total = ht;
    m->v_active = va;
    m->v_front = vss - va;
    m->v_sync = vse - vss;
    m->v_total = vt;
    m->h_sync_pos = (uint8_t)hpos;
    m->v_sync_pos = (uint8_t)vpos;
    m->refresh_mhz = refresh_mhz(pclk, ht, vt, m->interlaced);
}

/* drm_edid.c drm_dmt_modes (VESA DMT 1.0 r13), the entries standard and
 * established timings name on today's monitors; plus the two Apple
 * established modes of edid_est_modes */
static const struct dmt {
    uint16_t ha, hss, hse, ht, va, vss, vse, vt;
    uint32_t pclk;
    uint8_t hz, hpos, vpos;
} dmt[] = {
    {  640,  656,  752,  800,  480,  490,  492,  525,  25175, 60, 0, 0 },
    {  640,  664,  704,  832,  480,  489,  492,  520,  31500, 72, 0, 0 },
    {  640,  656,  720,  840,  480,  481,  484,  500,  31500, 75, 0, 0 },
    {  640,  704,  768,  864,  480,  483,  486,  525,  30240, 67, 0, 0 },   /* Apple */
    {  720,  738,  846,  900,  400,  412,  414,  449,  28320, 70, 0, 1 },
    {  800,  824,  896, 1024,  600,  601,  603,  625,  36000, 56, 1, 1 },
    {  800,  840,  968, 1056,  600,  601,  605,  628,  40000, 60, 1, 1 },
    {  800,  856,  976, 1040,  600,  637,  643,  666,  50000, 72, 1, 1 },
    {  800,  816,  896, 1056,  600,  601,  604,  625,  49500, 75, 1, 1 },
    {  832,  864,  928, 1152,  624,  625,  628,  667,  57284, 75, 0, 0 },   /* Apple */
    { 1024, 1048, 1184, 1344,  768,  771,  777,  806,  65000, 60, 0, 0 },
    { 1024, 1048, 1184, 1328,  768,  771,  777,  806,  75000, 70, 0, 0 },
    { 1024, 1040, 1136, 1312,  768,  769,  772,  800,  78750, 75, 1, 1 },
    { 1152, 1216, 1344, 1600,  864,  865,  868,  900, 108000, 75, 1, 1 },
    { 1280, 1390, 1430, 1650,  720,  725,  730,  750,  74250, 60, 1, 1 },
    { 1280, 1352, 1480, 1680,  800,  803,  809,  831,  83500, 60, 0, 1 },
    { 1280, 1376, 1488, 1800,  960,  961,  964, 1000, 108000, 60, 1, 1 },
    { 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, 108000, 60, 1, 1 },
    { 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, 135000, 75, 1, 1 },
    { 1360, 1424, 1536, 1792,  768,  771,  777,  795,  85500, 60, 1, 1 },
    { 1366, 1436, 1579, 1792,  768,  771,  774,  798,  85500, 60, 1, 1 },
    { 1400, 1488, 1632, 1864, 1050, 1053, 1057, 1089, 121750, 60, 0, 1 },
    { 1440, 1520, 1672, 1904,  900,  903,  909,  934, 106500, 60, 0, 1 },
    { 1600, 1624, 1704, 1800,  900,  901,  904, 1000, 108000, 60, 1, 1 },
    { 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, 162000, 60, 1, 1 },
    { 1680, 1784, 1960, 2240, 1050, 1053, 1059, 1089, 146250, 60, 0, 1 },
    { 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 148500, 60, 1, 1 },
    { 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235, 154000, 60, 1, 0 },
};

int edid_dmt(uint32_t h, uint32_t v, uint32_t hz, rdna3_mode *m)
{
    uint32_t i;
    for (i = 0; i < sizeof(dmt) / sizeof(dmt[0]); i++) {
        const struct dmt *t = &dmt[i];
        if (t->ha != h || t->va != v || t->hz != hz)
            continue;
        zero_mode(m);
        set_timing(m, t->pclk, t->ha, t->hss, t->hse, t->ht, t->va, t->vss, t->vse, t->vt, t->hpos, t->vpos);
        m->origin = RD_MODE_FROM_DMT;
        return 0;
    }
    return -1;
}

int edid_cvt_rb(uint32_t h, uint32_t v, uint32_t hz, rdna3_mode *m)
{
    /* drm_cvt_mode(reduced = true, interlaced = false, margins = false) */
    const uint64_t HV = 1000;           /* HV_FACTOR */
    const uint32_t MIN_VBLANK = 460;    /* CVT_RB_MIN_VBLANK, us */
    const uint32_t H_SYNC = 32, H_BLANK = 160, V_FPORCH = 3, MIN_V_BPORCH = 6;
    uint32_t hd = h - h % 8, vsync, vbi, vt, ht, hse, hss;
    uint64_t hperiod, clock;
    if (!h || !v || !hz)
        return -1;
    /* the aspect ratio picks the vsync width */
    if (v * 4 / 3 == h) vsync = 4;
    else if (v * 16 / 9 == h) vsync = 5;
    else if (v * 16 / 10 == h) vsync = 6;
    else if (v * 5 / 4 == h || v * 15 / 9 == h) vsync = 7;
    else vsync = 10;
    hperiod = (HV * 1000000ull - (uint64_t)MIN_VBLANK * HV * hz) / ((uint64_t)v * hz);
    if (!hperiod)
        return -1;
    vbi = (uint32_t)((uint64_t)MIN_VBLANK * HV / hperiod) + 1;
    if (vbi < V_FPORCH + vsync + MIN_V_BPORCH)
        vbi = V_FPORCH + vsync + MIN_V_BPORCH;
    vt = v + vbi;
    ht = hd + H_BLANK;
    clock = (uint64_t)ht * HV * 1000 / hperiod;
    clock -= clock % 250;               /* CVT_CLOCK_STEP */
    hse = ht - H_BLANK / 2;
    hss = hse - H_SYNC;
    zero_mode(m);
    set_timing(m, (uint32_t)clock, hd, hss, hse, ht, v, v + V_FPORCH, v + V_FPORCH + vsync, vt, 1, 0);
    m->origin = RD_MODE_FROM_CVT;
    return 0;
}

/* drm_edid.c edid_cea_modes_1: the progressive VICs a desktop uses */
static const struct cta {
    uint8_t vic, hz, pos;
    uint16_t ha, hss, hse, ht, va, vss, vse, vt;
    uint32_t pclk;
} cta[] = {
    {  1, 60, 0,  640,  656,  752,  800,  480,  490,  492,  525,  25175 },
    {  2, 60, 0,  720,  736,  798,  858,  480,  489,  495,  525,  27000 },
    {  3, 60, 0,  720,  736,  798,  858,  480,  489,  495,  525,  27000 },
    {  4, 60, 1, 1280, 1390, 1430, 1650,  720,  725,  730,  750,  74250 },
    { 16, 60, 1, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 148500 },
    { 17, 50, 0,  720,  732,  796,  864,  576,  581,  586,  625,  27000 },
    { 18, 50, 0,  720,  732,  796,  864,  576,  581,  586,  625,  27000 },
    { 19, 50, 1, 1280, 1720, 1760, 1980,  720,  725,  730,  750,  74250 },
    { 31, 50, 1, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, 148500 },
    { 32, 24, 1, 1920, 2558, 2602, 2750, 1080, 1084, 1089, 1125,  74250 },
    { 33, 25, 1, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125,  74250 },
    { 34, 30, 1, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125,  74250 },
    { 63, 120, 1, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 297000 },
    { 64, 100, 1, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, 297000 },
    { 93, 24, 1, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, 297000 },
    { 94, 25, 1, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, 297000 },
    { 95, 30, 1, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, 297000 },
    { 96, 50, 1, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, 594000 },
    { 97, 60, 1, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, 594000 },
};

static void cta_vic(rdna3_edid *e, uint8_t svd);

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
    if (b[0] != 0x02 || off < 4 || off > 127)
        return;
    /* data block collection: the video data block's short video
     * descriptors (cea_db_tag 2) */
    for (i = 4; i < off;) {
        uint32_t tag = b[i] >> 5, n = b[i] & 0x1f, k;
        if (i + 1 + n > off)
            break;
        if (tag == 2)
            for (k = 0; k < n; k++)
                cta_vic(e, b[i + 1 + k]);
        i += 1 + n;
    }
    if (off > 127 - 18)
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

    /* standard and established entries carry size and rate only: their
     * timing from DMT, else CVT reduced blanking (drm_mode_std); the
     * interlaced 1024x768@87 has neither and stays without one */
    for (i = 0; i < e->nmodes; i++) {
        rdna3_mode *m = &e->mode[i], t;
        uint32_t hz = (m->refresh_mhz + 500) / 1000;
        if (m->pixel_khz || m->interlaced)
            continue;
        if (edid_dmt(m->h_active, m->v_active, hz, &t) && edid_cvt_rb(m->h_active, m->v_active, hz, &t))
            continue;
        t.preferred = m->preferred;
        *m = t;
    }
    return 0;
}

static void cta_vic(rdna3_edid *e, uint8_t svd)
{
    /* CTA-861-F: 129-192 are VICs 1-64 with the native flag in bit 7 */
    uint8_t vic = svd >= 129 && svd <= 192 ? (uint8_t)(svd & 0x7f) : svd;
    uint32_t i;
    for (i = 0; i < sizeof(cta) / sizeof(cta[0]); i++) {
        const struct cta *c = &cta[i];
        rdna3_mode m;
        if (c->vic != vic)
            continue;
        zero_mode(&m);
        set_timing(&m, c->pclk, c->ha, c->hss, c->hse, c->ht, c->va, c->vss, c->vse, c->vt, c->pos, c->pos);
        m.origin = RD_MODE_FROM_CTA;
        add_mode(e, &m);
        return;
    }
}
