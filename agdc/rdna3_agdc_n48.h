/*
 * rdna3_agdc_n48.h - RDNA3AGDC's AGDC answers after Navi48-MacOS
 * (DRIVER-PLAN N45 (2) and (3)), each behind its own switch, as A/B for
 * the card. Plain C without kernel headers, like rdna3_agdc_k5.h (whose
 * structures and answers it wraps); tests/test_agdc_n48.c checks every
 * byte the switches change.
 *
 * SPDX-License-Identifier: MIT (our code; it builds on rdna3_agdc_k5.h,
 * PolyForm-Noncommercial-1.0.0, see PROVENANCE.md).
 *
 * The inline-buffer reply and the 0x921 bit-0 finding are ported from
 * Navi48-MacOS by Almosst-DEV (https://github.com/Almosst-DEV/Navi48-MacOS,
 * commit d1f7bad, src/navi48-bringup/src/apple/display_pipe_guard.h:
 * n48_agdc_fill_pipeline_caps, n48_agdc_fill_link_config_t),
 * Copyright (c) 2026 the Navi48 project, BSD-3-Clause (licence text in
 * /NOTICE). The offsets and values are theirs, the code is ours. Their
 * offsets were read on macOS 26; the card runs macOS 15.8.1 (24H32), where
 * none of them has been confirmed: that is why each one is a switch.
 *
 * What macOS 26.4.1 (25E253, x86_64) shows (IOPresentment.framework in the
 * dyld cache, `dyld_info -disassemble`; AMDRadeonX6000Framebuffer in
 * SystemKernelExtensions.kc), DRIVER-PLAN N45:
 *  - __GatherAGDCDisplayPipelineCapabilities bzeroes one 0x196c reply per
 *    type, writes fbIndex at +0x00 and the u64 0x8_000000TT at +0x04 (type,
 *    capacity hint 8 at +0x08), calls 0x711 in == out, and asks 0x10 first
 *    (a failure or count 0 there is 0x2006 and ends the gather), then 0x8,
 *    0x4, 0x2, 0x1, 0x40, 0x20, 0x200, 0x80, 0x100, 0x400 .. 0x8000. Any
 *    other type that fails only leaves its "present" byte clear; the gather
 *    still returns 0. A success with count > 8 is cut to 8 (so a success
 *    that leaves the hint in place reads as 8 entries: every success here
 *    writes its count).
 *  - IOPresentmentCreateInlineBufferDictionary takes the first type-0x40
 *    entry whose +0x0c and +0x10 are both non-zero: +0x00 Flags
 *    (& 0x803fffff), +0x04 ActivationLatency and +0x08 DeactivationLatency
 *    (Float32), +0x0c MaxHeight, +0x10 MaxWidth (SInt32), +0x14 an AGDC
 *    pixel-format mask (u32, mapped to PixelFormats). Type 0x40 refused =
 *    no InlineBuffer dictionary, and __inlineBufferFromDictionary returns 0
 *    without one; a success with count 0 gives an empty dictionary, which
 *    is Navi48's measured 0x2004. So type 0x40 is answered whole or refused,
 *    never zero-filled.
 *  - __GatherAGDCLogicalDeviceCapabilities: 0x921 failing is 0x80e (no
 *    capabilities at all); reply +0x08 bit 0 calls
 *    __GatherAGDCLinkCapabilities, whose result is ignored: it sets
 *    kIOPresentmentLinkCurrentMaximumBPC to 2, then asks 0x925 (and 0x988)
 *    to refine it. We refuse 0x925, so bit 0 set = BPC 2, clear = 0.
 *  - __PopulateScalerEntries reads a scaler entry's records (u64 count at
 *    +0x1c, records from +0x24, and +0x1a4 / +0x1ac, stride 0x20) as
 *    +0x00 Height, +0x04 Width, +0x08 MaxHeight, +0x0c MaxWidth, +0x10 AGDC
 *    pixel formats, +0x14 filter type (pshufd 0xb1 into the details, then
 *    __createBestWorstDictionaryEntry). So entry +0x24 is the HEIGHT, as
 *    NullMoth's answer has it, and Navi48's width-first entry would report
 *    1080 wide x 1920 high. AMD's AMDRadeonX6000_AmdAgdcServices::
 *    getFbCapabilityForScaler (0xbf71180) writes path+0x720 at +0x24 and
 *    path+0x71c at +0x28 and the markers 0x00050005 / 4 at +0x34 / +0x38;
 *    the older AtiDeviceControl::getFbCapabilityForScaler writes 0x70007
 *    and filter 0 or 4 (NullMoth's 0x70007 / 0 is that one).
 *
 *  (a) 0x711 capability type 0x40, the inline buffer. Switch rdna3agdcib /
 *      debug.rdna3agdc_ib, a bit mask: bit 0 answers type 0x40 (one entry:
 *      count 1, Flags 1, both latencies 1.0f as its bit pattern - the
 *      kernel has no FPU -, MaxHeight / MaxWidth = the head's raster,
 *      pixel formats 1), bit 1 zero-fills every other type but 0x10 and
 *      0x40 with success (count 0, Navi48's answer: on 26 it sets that
 *      type's present byte with no entries). Default 3, Navi48's working
 *      set: our card runs so far (every type but 0x10 refused) never had
 *      planes in a transaction.
 *  (b) 0x921 kAGDCLinkConfig flags. Switch rdna3agdclinkflags /
 *      debug.rdna3agdc_linkflags: 0 = 0x8|0x2|0x1 (NullMoth's NVRMAGDC, as
 *      before), 1 = 0x8|0x2 (bit 0 clear), 2 = 0 (Navi48 answers no flag).
 *      Default 0: NullMoth's answer works on 15 with NullMoth, and on 26
 *      bit 0 only changes LinkCurrentMaximumBPC.
 *  (c) 0x711 type 0x10, the plane scaler. Switch rdna3agdcscaler /
 *      debug.rdna3agdc_scaler: 0 = NullMoth (height first, 0x70007, filter
 *      0), 1 = AMD's markers (height first as IOPresentment reads it,
 *      pixel formats 0x00050005, filter 4), 2 = Navi48's entry as written
 *      (width at +0x24, the same markers). Default 0: NullMoth's order is
 *      the one IOPresentment 26 reads.
 *
 * Deliberate differences from Navi48's replies: the fbIndex and type words
 * of a 0x711 reply and the id of a 0x921 reply keep what IOPresentment
 * sent (Navi48 zeroes the whole buffer); the 0x921 reply keeps the rest of
 * NullMoth's link (groupID -1, port 1 + index, state 1, one link, RGB
 * 8 bpc) where Navi48 leaves those words 0.
 */
#ifndef RDNA3_AGDC_N48_H
#define RDNA3_AGDC_N48_H
#include <stdint.h>
#include "rdna3_agdc_k5.h"

#define RD3N48_CAP_INLINEBUF  0x40u
#define RD3N48_IB_LATENCY_1F  0x3f800000u   /* 1.0f */
#define RD3N48_SCALER_PIXFMT_AMD 0x00050005u
#define RD3N48_SCALER_FILTER_AMD 4u

/* (a) debug.rdna3agdc_ib bits */
#define RD3N48_IB_ANSWER      0x1u
#define RD3N48_IB_ZERO        0x2u
#define RD3N48_IB_ALL         (RD3N48_IB_ANSWER | RD3N48_IB_ZERO)
#define RD3N48_IB_DEFAULT     RD3N48_IB_ALL
/* (b) debug.rdna3agdc_linkflags */
#define RD3N48_LINKFLAGS_NULLMOTH 0u
#define RD3N48_LINKFLAGS_NOBIT0   1u
#define RD3N48_LINKFLAGS_ZERO     2u
#define RD3N48_LINKFLAGS_MAX      RD3N48_LINKFLAGS_ZERO
#define RD3N48_LINKFLAGS_DEFAULT  RD3N48_LINKFLAGS_NULLMOTH
/* (c) debug.rdna3agdc_scaler */
#define RD3N48_SCALER_NULLMOTH    0u
#define RD3N48_SCALER_AMD         1u
#define RD3N48_SCALER_NAVI48      2u
#define RD3N48_SCALER_MAX         RD3N48_SCALER_NAVI48
#define RD3N48_SCALER_DEFAULT     RD3N48_SCALER_NULLMOTH

/* the answers on top of rd3k5_answer's (RD3K5_R_*) */
#define RD3N48_R_IB          3   /* type 0x40 answered with the inline-buffer entry */
#define RD3N48_R_ZERO        4   /* another type zero-filled, success */
/* *why for the new refusals (rd3k5_answer's stay as they are) */
#define RD3N48_WHY_IB_INDEX   0x41
#define RD3N48_WHY_ZERO_INDEX 0x51

struct rd3n48_sw { uint32_t ib, linkflags, scaler; };

/* a value a switch does not have becomes its default; returns a mask of the
 * switches replaced (1 ib, 2 linkflags, 4 scaler) for the log line */
static inline unsigned rd3n48_sw_fix(struct rd3n48_sw *sw)
{
    unsigned bad = 0;
    if (sw->ib > RD3N48_IB_ALL) { sw->ib = RD3N48_IB_DEFAULT; bad |= 1u; }
    if (sw->linkflags > RD3N48_LINKFLAGS_MAX) { sw->linkflags = RD3N48_LINKFLAGS_DEFAULT; bad |= 2u; }
    if (sw->scaler > RD3N48_SCALER_MAX) { sw->scaler = RD3N48_SCALER_DEFAULT; bad |= 4u; }
    return bad;
}

/* the 0x921 flags word per switch value (an unknown value answers
 * NullMoth's) */
static inline uint64_t rd3n48_link_flags(uint32_t sw)
{
    if (sw == RD3N48_LINKFLAGS_NOBIT0) return 0x8u | 0x2u;
    if (sw == RD3N48_LINKFLAGS_ZERO) return 0;
    return 0x8u | 0x1u | 0x2u;
}

/* (c) the plane-scaler entry with AMD's markers: rd3k5_fill_planescaler's
 * entry (count 1, 0x80004, both record groups of one record) with pixel
 * formats 0x00050005 and filter type 4; widthFirst puts the width at the
 * records' +0x00 as Navi48 wrote it (IOPresentment 26 reads +0x00 as the
 * height) */
static inline void rd3n48_fill_scaler_amd(struct rd3k5_fb_ext_cap *c, uint32_t w, uint32_t h, int widthFirst)
{
    static const unsigned rec[2] = { 0x024, 0x1ac };
    const uint32_t a = widthFirst ? w : h, b = widthFirst ? h : w;
    uint32_t idx = c->fbIndex, type = c->type;
    uint8_t *e = c->entry[0];
    unsigned i;
    rd3k5_zero(c, sizeof *c);
    c->fbIndex = idx; c->type = type;
    c->count = 1;
    rd3k5_put32(e, 0x000, 0x80004u);
    rd3k5_put32(e, 0x01c, 1);   /* u64 record count, first group */
    rd3k5_put32(e, 0x1a4, 1);   /* u64 record count, second group */
    for (i = 0; i < 2; i++) {
        rd3k5_put32(e, rec[i] + 0x00, a); rd3k5_put32(e, rec[i] + 0x04, b);
        rd3k5_put32(e, rec[i] + 0x08, a); rd3k5_put32(e, rec[i] + 0x0c, b);
        rd3k5_put32(e, rec[i] + 0x10, RD3N48_SCALER_PIXFMT_AMD);
        rd3k5_put32(e, rec[i] + 0x14, RD3N48_SCALER_FILTER_AMD);
    }
}

/* (a) the inline-buffer reply: one entry (reply +0x0c, stride 0x32c) */
static inline void rd3n48_fill_inline_buffer(struct rd3k5_fb_ext_cap *c, uint32_t w, uint32_t h)
{
    uint32_t idx = c->fbIndex, type = c->type;
    uint8_t *e = c->entry[0];
    rd3k5_zero(c, sizeof *c);
    c->fbIndex = idx; c->type = type;
    c->count = 1;
    rd3k5_put32(e, 0x00, 1);                       /* Flags */
    rd3k5_put32(e, 0x04, RD3N48_IB_LATENCY_1F);    /* ActivationLatency */
    rd3k5_put32(e, 0x08, RD3N48_IB_LATENCY_1F);    /* DeactivationLatency */
    rd3k5_put32(e, 0x0c, h);                       /* MaxHeight, non-zero */
    rd3k5_put32(e, 0x10, w);                       /* MaxWidth, non-zero */
    rd3k5_put32(e, 0x14, 1); rd3k5_put32(e, 0x18, 0);   /* pixel formats (Navi48: SInt64 1) */
}

/* a capability type we have no answer for: count 0, no entry */
static inline void rd3n48_fill_zero(struct rd3k5_fb_ext_cap *c)
{
    uint32_t idx = c->fbIndex, type = c->type;
    rd3k5_zero(c, sizeof *c);
    c->fbIndex = idx; c->type = type;
}

/* what an answer was, for the log lines and counters */
struct rd3n48_info { uint32_t type, w, h; uint64_t flags; uint32_t scaler; };

/* rd3k5_answer with the switches (*sw already through rd3n48_sw_fix): the
 * same arguments and RD3K5_R_* codes, plus RD3N48_R_IB / RD3N48_R_ZERO;
 * *inf says what was answered (type 0xffffffff: not a 0x711) */
static inline int rd3n48_answer(unsigned cmd, const void *in, unsigned long inSize, void *out, unsigned long outSize,
                                const struct rd3k5_head *h, unsigned nh, const struct rd3n48_sw *sw, int *why,
                                int32_t *idxOut, struct rd3n48_info *inf)
{
    int r = rd3k5_answer(cmd, in, inSize, out, outSize, h, nh, why, idxOut);
    struct rd3k5_fb_ext_cap *c = (struct rd3k5_fb_ext_cap *)out;
    rd3k5_zero(inf, sizeof *inf);
    inf->type = 0xffffffffu;
    if (r == RD3K5_R_LINK) {
        struct rd3k5_link_config *l = (struct rd3k5_link_config *)out;
        l->flags = rd3n48_link_flags(sw->linkflags);
        inf->flags = l->flags;
        inf->w = l->timing.hActive; inf->h = l->timing.vActive;
        return r;
    }
    if (cmd != RD3K5_CMD_FBCAPEX)
        return r;
    /* *idxOut is set once rd3k5_answer checked both sizes and copied the
     * query into 'out' */
    if (*idxOut >= 0 || r == RD3K5_R_CAPS || r == RD3K5_R_UNSUPPORTED)
        inf->type = c->type;
    if (r != RD3K5_R_CAPS && r != RD3K5_R_UNSUPPORTED)
        return r;
    if (r == RD3K5_R_CAPS) {
        const uint32_t *t = h[c->fbIndex].t;
        inf->w = t[RD3K5_T_HVIS]; inf->h = t[RD3K5_T_VVIS];
        inf->scaler = sw->scaler;
        if (sw->scaler == RD3N48_SCALER_AMD || sw->scaler == RD3N48_SCALER_NAVI48)
            rd3n48_fill_scaler_amd(c, inf->w, inf->h, sw->scaler == RD3N48_SCALER_NAVI48);
        else
            inf->scaler = RD3N48_SCALER_NULLMOTH;
        return r;
    }
    if (c->type == RD3N48_CAP_INLINEBUF) {
        const uint32_t *t;
        int bad;
        if (!(sw->ib & RD3N48_IB_ANSWER)) return r;   /* refused: no InlineBuffer dictionary */
        if (c->fbIndex >= nh) { *why = RD3N48_WHY_IB_INDEX; return RD3K5_R_BADARG; }
        t = h[c->fbIndex].t;
        bad = t ? rd3k5_timing_bad(t, h[c->fbIndex].tBytes) : 0x31;
        if (bad) { *why = bad; return RD3K5_R_NOTFOUND; }
        inf->w = t[RD3K5_T_HVIS]; inf->h = t[RD3K5_T_VVIS];
        rd3n48_fill_inline_buffer(c, inf->w, inf->h);
        return RD3N48_R_IB;
    }
    if (sw->ib & RD3N48_IB_ZERO) {
        if (c->fbIndex >= nh) { *why = RD3N48_WHY_ZERO_INDEX; return RD3K5_R_BADARG; }
        rd3n48_fill_zero(c);
        return RD3N48_R_ZERO;
    }
    return r;
}

/* ---- the 0x711 type counters --------------------------------------------
 * IOPresentment asks sixteen capability types, one bit each (0x1 CSC ..
 * 0x8000 Cursor; 0x10 plane scaler, 0x40 inline buffer). The old counter
 * (debug.rdna3agdc_k5_types, bit t = type t) stopped at type 31, so 0x40
 * and up were never seen. Slot k counts type 1 << k, slot 16 any other
 * value. */
#define RD3N48_TYPE_SLOTS 17u
#define RD3N48_TYPE_OTHER 16u
static inline unsigned rd3n48_type_slot(uint32_t type)
{
    unsigned k;
    if (!type || (type & (type - 1u)) || type > 0x8000u) return RD3N48_TYPE_OTHER;
    for (k = 0; !(type & (1u << k)); k++) {}
    return k;
}

/* appends s to buf (cap bytes, always NUL-terminated); the new length */
static inline unsigned rd3n48_put(char *buf, unsigned cap, unsigned n, const char *s)
{
    while (*s && n + 1 < cap) buf[n++] = *s++;
    if (cap) buf[n < cap ? n : cap - 1] = 0;
    return n;
}
static inline unsigned rd3n48_putu(char *buf, unsigned cap, unsigned n, uint32_t v, int hex)
{
    char d[12];
    unsigned i = 0;
    const uint32_t base = hex ? 16u : 10u;
    do { d[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v && i < sizeof d);
    if (hex) n = rd3n48_put(buf, cap, n, "0x");
    while (i) { const char c[2] = { d[--i], 0 }; n = rd3n48_put(buf, cap, n, c); }
    return n;
}
/* "0x1 3/0, 0x10 56/56, 0x40 56/54, other 2/0": per type asked, the
 * queries / the ones answered with success, in slot order (0x1 first),
 * types never asked left out; "none" when no 0x711 came. */
#define RD3N48_TYPES_TEXT 544u   /* 17 * (", 0x8000 " + 2 * 10 digits + "/") + 1 fits */
static inline void rd3n48_types_text(char *buf, unsigned cap, const uint32_t asked[RD3N48_TYPE_SLOTS],
                                     const uint32_t answered[RD3N48_TYPE_SLOTS])
{
    unsigned n = 0, k;
    if (!cap) return;
    buf[0] = 0;
    for (k = 0; k < RD3N48_TYPE_SLOTS; k++) {
        if (!asked[k] && !answered[k]) continue;
        if (n) n = rd3n48_put(buf, cap, n, ", ");
        if (k == RD3N48_TYPE_OTHER) n = rd3n48_put(buf, cap, n, "other");
        else n = rd3n48_putu(buf, cap, n, 1u << k, 1);
        n = rd3n48_put(buf, cap, n, " ");
        n = rd3n48_putu(buf, cap, n, asked[k], 0);
        n = rd3n48_put(buf, cap, n, "/");
        n = rd3n48_putu(buf, cap, n, answered[k], 0);
    }
    if (!n) rd3n48_put(buf, cap, 0, "none");
}

/* the switches in words, for the start line and the change lines */
static inline const char *rd3n48_ib_text(uint32_t v)
{
    switch (v) {
    case 0: return "type 0x40 and the other types refused (as before N45)";
    case 1: return "type 0x40 answered (inline buffer), the other types refused";
    case 2: return "type 0x40 refused, the other types zero-filled + success";
    case 3: return "type 0x40 answered (inline buffer), the other types zero-filled + success (Navi48)";
    default: return "unknown value";
    }
}
static inline const char *rd3n48_linkflags_text(uint32_t v)
{
    switch (v) {
    case 0: return "flags 0xb (NullMoth; bit 0 starts __GatherAGDCLinkCapabilities)";
    case 1: return "flags 0xa (bit 0 clear)";
    case 2: return "flags 0 (Navi48)";
    default: return "unknown value";
    }
}
static inline const char *rd3n48_scaler_text(uint32_t v)
{
    switch (v) {
    case 0: return "NullMoth (height at +0x24, pixel formats 0x70007, filter 0)";
    case 1: return "AMD markers (height at +0x24, pixel formats 0x50005, filter 4)";
    case 2: return "Navi48 as written (WIDTH at +0x24, pixel formats 0x50005, filter 4)";
    default: return "unknown value";
    }
}
#endif
