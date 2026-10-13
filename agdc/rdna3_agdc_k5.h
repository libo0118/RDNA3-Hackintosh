/*
 * Derived from the NullMoth NVIDIA driver for macOS (kexts/NVRMAGDC/nvrm-agdc-k5.h)
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * AMD (RDNA3-Hackintosh) port, used with the author's permission for
 * AMD-only work. See PROVENANCE.md.
 *
 * The answers RDNA3AGDC gives IOPresentment's two display-topology queries
 * ("K5"): kAGDCLinkConfig (0x921) for a framebuffer index and the extended
 * capability 0x711 of type 0x10 (plane scaler). Plain C without kernel
 * headers, so the host tests (tests/test_agdc_k5.c) build it as is.
 *
 * A framebuffer publishes its current timing as the property RDNA3Timing:
 * RD3K5_TIMING_WORDS 32-bit words, word 0 the tag.
 */
#ifndef RDNA3_AGDC_K5_H
#define RDNA3_AGDC_K5_H
#include <stdint.h>

#define RD3K5_CMD_VENDOR      0x1u
#define RD3K5_CMD_GPUCAPS     0x980u
#define RD3K5_CMD_LINKCONFIG  0x921u
#define RD3K5_CMD_FBCAPEX     0x711u
#define RD3K5_CAP_PLANESCALER 0x10u
#define RD3K5_TIMING_TAG      0x4b350001u
#define RD3K5_TIMING_WORDS    13
#define RD3K5_TIMING_PROP     "RDNA3Timing"

/* the RDNA3Timing words */
enum { RD3K5_T_TAG, RD3K5_T_PCLK, RD3K5_T_HVIS, RD3K5_T_HSS, RD3K5_T_HSE, RD3K5_T_HTOT, RD3K5_T_VVIS, RD3K5_T_VSS,
       RD3K5_T_VSE, RD3K5_T_VTOT, RD3K5_T_REFRESH_MHZ, RD3K5_T_HSYNCPOS, RD3K5_T_VSYNCPOS };

#pragma pack(push, 1)
struct rd3k5_timing {
    uint32_t hScaledInset, vScaledInset, scalerFlags, hScaled, vScaled, signalConfig, signalLevels;
    uint64_t pixelClock, minPixelClock, maxPixelClock;
    uint32_t hActive, hBlanking, hSyncOffset, hSyncPulseWidth;
    uint32_t vActive, vBlanking, vSyncOffset, vSyncPulseWidth;
    uint32_t hBorderLeft, hBorderRight, vBorderTop, vBorderBottom;
    uint32_t hSyncConfig, hSyncLevel, vSyncConfig, vSyncLevel;
    uint32_t numLinks, vBlankingExtension;
    uint16_t pixelEncoding, bitsPerColorComponent, colorimetry, dynamicRange;
    uint16_t dscBitsPerPixel, dscSliceHeight, dscSliceWidth, field8a;
    uint8_t  tail[8];
};
/* AGDCLinkConfig_t */
struct rd3k5_link_config {
    int32_t  id;
    int32_t  groupID;
    uint64_t flags;
    struct rd3k5_timing timing;
    uint32_t port, stream;
    uint32_t state;
};
/* AGDCFBGetExtendedCapability_t */
struct rd3k5_fb_ext_cap {
    uint32_t fbIndex, type, count;
    uint8_t  entry[8][0x32c];
};
/* AGDCVendorInfo_t (command 1) */
struct rd3k5_vendor_info {
    uint32_t version;
    char     vendor[0x20];
    uint32_t vendorID;
    uint32_t vendorClass;
};
#pragma pack(pop)
_Static_assert(sizeof(struct rd3k5_timing) == 0x94, "AGDC timing is 0x94");
_Static_assert(sizeof(struct rd3k5_link_config) == 0xb0, "AGDCLinkConfig_t is 0xb0");
_Static_assert(__builtin_offsetof(struct rd3k5_link_config, port) == 0xa4, "stream address at +0xa4");
_Static_assert(sizeof(struct rd3k5_fb_ext_cap) == 0x196c, "AGDCFBGetExtendedCapability_t is 0x196c");
_Static_assert(sizeof(struct rd3k5_vendor_info) == 0x2c, "AGDCVendorInfo_t is 0x2c");

static inline void rd3k5_zero(void *p, unsigned long n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }
static inline void rd3k5_put32(uint8_t *e, unsigned off, uint32_t v)
{
    e[off] = (uint8_t)v; e[off + 1] = (uint8_t)(v >> 8); e[off + 2] = (uint8_t)(v >> 16); e[off + 3] = (uint8_t)(v >> 24);
}

/* the RDNA3Timing words from a mode's detailed timing: 0, or why not */
static inline int rd3k5_timing_from(uint32_t t[RD3K5_TIMING_WORDS], uint64_t pclkHz, uint32_t hActive,
                                    uint32_t hBlank, uint32_t hSyncOff, uint32_t hSyncW, uint32_t vActive,
                                    uint32_t vBlank, uint32_t vSyncOff, uint32_t vSyncW, uint32_t refreshMilliHz,
                                    int hSyncPos, int vSyncPos)
{
    rd3k5_zero(t, RD3K5_TIMING_WORDS * sizeof(uint32_t));
    if (!pclkHz || pclkHz > 0xffffffffull || !hActive || !vActive || !hBlank || !vBlank)
        return 1;
    if (!hSyncW || !vSyncW || hSyncOff + hSyncW > hBlank || vSyncOff + vSyncW > vBlank)
        return 2;
    t[RD3K5_T_TAG] = RD3K5_TIMING_TAG;
    t[RD3K5_T_PCLK] = (uint32_t)pclkHz;
    t[RD3K5_T_HVIS] = hActive;
    t[RD3K5_T_HSS] = hActive + hSyncOff;
    t[RD3K5_T_HSE] = hActive + hSyncOff + hSyncW;
    t[RD3K5_T_HTOT] = hActive + hBlank;
    t[RD3K5_T_VVIS] = vActive;
    t[RD3K5_T_VSS] = vActive + vSyncOff;
    t[RD3K5_T_VSE] = vActive + vSyncOff + vSyncW;
    t[RD3K5_T_VTOT] = vActive + vBlank;
    t[RD3K5_T_REFRESH_MHZ] = refreshMilliHz;
    t[RD3K5_T_HSYNCPOS] = hSyncPos ? 1u : 0u;
    t[RD3K5_T_VSYNCPOS] = vSyncPos ? 1u : 0u;
    return 0;
}

static inline int rd3k5_timing_bad(const uint32_t *t, unsigned long bytes)
{
    if (!t || bytes != RD3K5_TIMING_WORDS * sizeof(uint32_t)) return 1;
    if (t[RD3K5_T_TAG] != RD3K5_TIMING_TAG) return 2;
    if (!t[RD3K5_T_PCLK] || !t[RD3K5_T_HVIS] || !t[RD3K5_T_VVIS]) return 3;
    if (!(t[RD3K5_T_HVIS] <= t[RD3K5_T_HSS] && t[RD3K5_T_HSS] < t[RD3K5_T_HSE] && t[RD3K5_T_HSE] <= t[RD3K5_T_HTOT])) return 4;
    if (!(t[RD3K5_T_VVIS] <= t[RD3K5_T_VSS] && t[RD3K5_T_VSS] < t[RD3K5_T_VSE] && t[RD3K5_T_VSE] <= t[RD3K5_T_VTOT])) return 5;
    if (t[RD3K5_T_HTOT] == t[RD3K5_T_HVIS] || t[RD3K5_T_VTOT] == t[RD3K5_T_VVIS]) return 6;
    return 0;
}

/* one link, RGB 8 bpc, the head's timing, on port 1 + index */
static inline void rd3k5_fill_link(struct rd3k5_link_config *l, const uint32_t *t, uint32_t port)
{
    int32_t id = l->id;
    struct rd3k5_timing *g = &l->timing;
    rd3k5_zero(l, sizeof *l);
    l->id = id;
    l->groupID = -1;
    l->flags = 0x8 | 0x1 | 0x2;
    g->pixelClock = g->minPixelClock = g->maxPixelClock = t[RD3K5_T_PCLK];
    g->hActive = t[RD3K5_T_HVIS]; g->hBlanking = t[RD3K5_T_HTOT] - t[RD3K5_T_HVIS];
    g->hSyncOffset = t[RD3K5_T_HSS] - t[RD3K5_T_HVIS]; g->hSyncPulseWidth = t[RD3K5_T_HSE] - t[RD3K5_T_HSS];
    g->vActive = t[RD3K5_T_VVIS]; g->vBlanking = t[RD3K5_T_VTOT] - t[RD3K5_T_VVIS];
    g->vSyncOffset = t[RD3K5_T_VSS] - t[RD3K5_T_VVIS]; g->vSyncPulseWidth = t[RD3K5_T_VSE] - t[RD3K5_T_VSS];
    g->hSyncConfig = t[RD3K5_T_HSYNCPOS] ? 1 : 0; g->vSyncConfig = t[RD3K5_T_VSYNCPOS] ? 1 : 0;
    g->numLinks = 1;
    g->pixelEncoding = 0x0001;
    g->bitsPerColorComponent = 0x0002;
    g->colorimetry = 0x0001;
    g->dynamicRange = 0x0001;
    l->port = port;
    l->stream = 0;
    l->state = 1;
}

/* one plane scaler entry: source and destination limits = the raster */
static inline void rd3k5_fill_planescaler(struct rd3k5_fb_ext_cap *c, const uint32_t *t)
{
    uint32_t idx = c->fbIndex, type = c->type, w = t[RD3K5_T_HVIS], h = t[RD3K5_T_VVIS];
    uint8_t *e = c->entry[0];
    rd3k5_zero(c, sizeof *c);
    c->fbIndex = idx; c->type = type;
    c->count = 1;
    rd3k5_put32(e, 0x000, 0x80004);
    rd3k5_put32(e, 0x004, 0);
    rd3k5_put32(e, 0x01c, 1);
    rd3k5_put32(e, 0x024, h); rd3k5_put32(e, 0x028, w); rd3k5_put32(e, 0x02c, h); rd3k5_put32(e, 0x030, w);
    rd3k5_put32(e, 0x034, 0x70007);
    rd3k5_put32(e, 0x038, 0);
    rd3k5_put32(e, 0x1a4, 1);
    rd3k5_put32(e, 0x1ac, h); rd3k5_put32(e, 0x1b0, w); rd3k5_put32(e, 0x1b4, h); rd3k5_put32(e, 0x1b8, w);
    rd3k5_put32(e, 0x1bc, 0x70007);
    rd3k5_put32(e, 0x1c0, 0);
}

#define RD3K5_R_UNSUPPORTED  0
#define RD3K5_R_LINK         1
#define RD3K5_R_CAPS         2
#define RD3K5_R_BADARG     (-1)
#define RD3K5_R_NOTFOUND   (-2)
struct rd3k5_head { const uint32_t *t; unsigned long tBytes; };

/* answers 0x921 / 0x711 for 'nh' framebuffer slots (index = IOFBDependentIndex);
 * *why says why a query was refused, *idxOut the index it named (-1 none) */
static inline int rd3k5_answer(unsigned cmd, const void *in, unsigned long inSize, void *out, unsigned long outSize,
                               const struct rd3k5_head *h, unsigned nh, int *why, int32_t *idxOut)
{
    int link, bad;
    unsigned long want;
    uint32_t idx;
    const uint32_t *t;
    *idxOut = -1;
    if (cmd != RD3K5_CMD_LINKCONFIG && cmd != RD3K5_CMD_FBCAPEX) return RD3K5_R_UNSUPPORTED;
    link = cmd == RD3K5_CMD_LINKCONFIG;
    want = link ? sizeof(struct rd3k5_link_config) : sizeof(struct rd3k5_fb_ext_cap);
    if (!out || outSize != want) { *why = link ? 0x10 : 0x20; return RD3K5_R_BADARG; }
    if (!in || inSize != want) { *why = link ? 0x12 : 0x22; return RD3K5_R_BADARG; }
    if (in != out) {
        const uint8_t *s = (const uint8_t *)in;
        uint8_t *d = (uint8_t *)out;
        unsigned long n = want;
        if (d < s) while (n--) *d++ = *s++;
        else while (n--) d[n] = s[n];
    }
    idx = link ? (uint32_t)((struct rd3k5_link_config *)out)->id : ((struct rd3k5_fb_ext_cap *)out)->fbIndex;
    *idxOut = (int32_t)idx;
    if (!link && ((struct rd3k5_fb_ext_cap *)out)->type != RD3K5_CAP_PLANESCALER) return RD3K5_R_UNSUPPORTED;
    if (idx >= nh) { *why = link ? 0x11 : 0x21; return RD3K5_R_BADARG; }
    t = h[idx].t;
    bad = t ? rd3k5_timing_bad(t, h[idx].tBytes) : 0x31;
    if (bad) { *why = bad; return RD3K5_R_NOTFOUND; }
    if (link) { rd3k5_fill_link((struct rd3k5_link_config *)out, t, 1u + idx); return RD3K5_R_LINK; }
    rd3k5_fill_planescaler((struct rd3k5_fb_ext_cap *)out, t);
    return RD3K5_R_CAPS;
}
#endif
