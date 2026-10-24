/*
 * test_agdc_n48.c - RDNA3AGDC's answers after Navi48-MacOS (DRIVER-PLAN N45
 * (2), (3); agdc/rdna3_agdc_n48.h) for a 1920x1080@60 head: the byte layout
 * of each reply per switch value, the zero fill of the other 0x711 types,
 * the refusals, the type slots past 32 and their text.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "../agdc/rdna3_agdc_n48.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t rd32(const void *p, unsigned off)
{
    const uint8_t *e = (const uint8_t *)p;
    return (uint32_t)e[off] | (uint32_t)e[off + 1] << 8 | (uint32_t)e[off + 2] << 16 | (uint32_t)e[off + 3] << 24;
}
/* bytes [from, to) of 'p' are all 0 */
static int zero_from(const void *p, unsigned from, unsigned to)
{
    const uint8_t *b = (const uint8_t *)p;
    for (unsigned i = from; i < to; i++)
        if (b[i]) return 0;
    return 1;
}
/* the non-zero dwords of the reply are exactly offs/vals (the rest is 0) */
static int exactly(const void *p, unsigned len, const unsigned *offs, const uint32_t *vals, unsigned n)
{
    for (unsigned off = 0; off < len; off += 4) {
        uint32_t want = 0;
        for (unsigned i = 0; i < n; i++)
            if (offs[i] == off) want = vals[i];
        if (rd32(p, off) != want) {
            printf("  dword +%#x is %#x, want %#x\n", off, rd32(p, off), want);
            return 0;
        }
    }
    return 1;
}

static struct rd3k5_fb_ext_cap cq, cr;
static struct rd3k5_link_config lq, lr;

/* a 0x711 query as IOPresentment writes it: fbIndex, then the u64
 * 0x8_000000TT at +0x04 (type, capacity hint 8 at +0x08), garbage after */
static void query711(uint32_t idx, uint32_t type)
{
    memset(&cq, 0xa5, sizeof cq);
    cq.fbIndex = idx;
    cq.type = type;
    cq.count = 8;
}

int main(void)
{
    uint32_t t[2][RD3K5_TIMING_WORDS];
    struct rd3k5_head h[2];
    struct rd3n48_sw sw;
    struct rd3n48_info inf;
    int why = 0;
    int32_t idx = -1;
    const unsigned len = sizeof cr;

    CHECK(rd3k5_timing_from(t[0], 148500000ull, 1920, 280, 88, 44, 1080, 45, 4, 5, 60000, 1, 1) == 0);
    h[0].t = t[0]; h[0].tBytes = sizeof t[0];
    h[1].t = NULL; h[1].tBytes = 0;

    /* ---- (a) type 0x40, the inline buffer */
    sw.ib = RD3N48_IB_DEFAULT; sw.linkflags = RD3N48_LINKFLAGS_DEFAULT; sw.scaler = RD3N48_SCALER_DEFAULT;
    CHECK(sw.ib == 3 && sw.linkflags == 0 && sw.scaler == 0);
    query711(0, RD3N48_CAP_INLINEBUF);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3N48_R_IB);
    {
        /* reply offsets as Navi48 gives them: +0x08 count, +0x0c Flags,
         * +0x10/+0x14 Float32 1.0, +0x18 MaxHeight, +0x1c MaxWidth,
         * +0x20 PixelFormats (SInt64) 1 */
        static const unsigned o[] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20 };
        static const uint32_t v[] = { 0, 0x40, 1, 1, 0x3f800000u, 0x3f800000u, 1080, 1920, 1 };
        CHECK(exactly(&cr, len, o, v, 9));
    }
    CHECK(idx == 0 && inf.type == 0x40 && inf.w == 1920 && inf.h == 1080);
    /* in place (in == out), as IOPresentment calls it */
    query711(0, RD3N48_CAP_INLINEBUF);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cq, sizeof cq, h, 2, &sw, &why, &idx, &inf) == RD3N48_R_IB);
    CHECK(memcmp(&cq, &cr, sizeof cq) == 0);
    /* index 1 has no timing (MaxWidth/MaxHeight would be 0): refused */
    query711(1, RD3N48_CAP_INLINEBUF);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_NOTFOUND);
    CHECK(why == 0x31 && idx == 1);
    /* index 2 does not exist */
    query711(2, RD3N48_CAP_INLINEBUF);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_BADARG);
    CHECK(why == (int)RD3N48_WHY_IB_INDEX && idx == 2);
    /* a wrong size is still rd3k5_answer's refusal */
    query711(0, RD3N48_CAP_INLINEBUF);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr - 4, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_BADARG);
    CHECK(why == 0x20);

    /* ---- (a) the other types: zero-filled + success, fbIndex/type kept */
    {
        static const uint32_t types[] = { 0x1, 0x2, 0x4, 0x8, 0x20, 0x80, 0x100, 0x8000, 0x11, 0x10000 };
        static const unsigned o[] = { 0x00, 0x04 };
        for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) {
            uint32_t v[2] = { 1, types[i] };
            query711(1, types[i]);   /* no timing needed */
            CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3N48_R_ZERO);
            CHECK(exactly(&cr, len, o, v, 2));
            CHECK(inf.type == types[i]);
        }
        query711(2, 0x1);
        CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_BADARG);
        CHECK(why == (int)RD3N48_WHY_ZERO_INDEX);
    }

    /* ---- (a) per switch value */
    sw.ib = 0;   /* as before N45: every type but 0x10 refused */
    query711(0, RD3N48_CAP_INLINEBUF);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_UNSUPPORTED);
    query711(0, 0x2);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_UNSUPPORTED);
    sw.ib = RD3N48_IB_ANSWER;   /* 0x40 only */
    query711(0, RD3N48_CAP_INLINEBUF);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3N48_R_IB);
    query711(0, 0x2);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_UNSUPPORTED);
    sw.ib = RD3N48_IB_ZERO;   /* 0x40 is never zero-filled (on 26 that is Navi48's measured 0x2004) */
    query711(0, RD3N48_CAP_INLINEBUF);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_UNSUPPORTED);
    CHECK(inf.type == 0x40);
    query711(0, 0x2);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3N48_R_ZERO);
    CHECK(cr.count == 0 && cr.fbIndex == 0 && cr.type == 2 && zero_from(&cr, 8, len));
    /* the plane scaler is never zero-filled: a head without timing stays refused (count 0 would be 0x2006 anyway) */
    query711(1, RD3K5_CAP_PLANESCALER);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_NOTFOUND);
    sw.ib = RD3N48_IB_DEFAULT;

    /* ---- unknown switch values become the defaults */
    {
        struct rd3n48_sw s = { 3, 2, 2 };
        CHECK(rd3n48_sw_fix(&s) == 0 && s.ib == 3 && s.linkflags == 2 && s.scaler == 2);
        s.ib = 4; s.linkflags = 3; s.scaler = 0xffffffffu;
        CHECK(rd3n48_sw_fix(&s) == 7);
        CHECK(s.ib == RD3N48_IB_DEFAULT && s.linkflags == RD3N48_LINKFLAGS_DEFAULT && s.scaler == RD3N48_SCALER_DEFAULT);
        s.ib = 0; s.linkflags = 9; s.scaler = 1;
        CHECK(rd3n48_sw_fix(&s) == 2 && s.ib == 0 && s.linkflags == 0 && s.scaler == 1);
    }

    /* ---- (c) the plane scaler, type 0x10, per switch value */
    query711(0, RD3K5_CAP_PLANESCALER);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_CAPS);
    CHECK(inf.scaler == 0 && inf.w == 1920 && inf.h == 1080 && inf.type == 0x10);
    {
        /* NullMoth's, unchanged (reply offset = entry offset + 0x0c) */
        static const unsigned o[] = { 0x00, 0x04, 0x08, 0x0c, 0x28, 0x30, 0x34, 0x38, 0x3c, 0x40,
                                      0x1b0, 0x1b8, 0x1bc, 0x1c0, 0x1c4, 0x1c8 };
        static const uint32_t v[] = { 0, 0x10, 1, 0x80004u, 1, 1080, 1920, 1080, 1920, 0x70007u,
                                      1, 1080, 1920, 1080, 1920, 0x70007u };
        CHECK(exactly(&cr, len, o, v, 16));
    }
    sw.scaler = RD3N48_SCALER_AMD;
    query711(0, RD3K5_CAP_PLANESCALER);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_CAPS);
    CHECK(inf.scaler == 1);
    {
        /* AMD's markers in IOPresentment 26's order (record +0x00 = Height) */
        static const unsigned o[] = { 0x00, 0x04, 0x08, 0x0c, 0x28, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44,
                                      0x1b0, 0x1b8, 0x1bc, 0x1c0, 0x1c4, 0x1c8, 0x1cc };
        static const uint32_t v[] = { 0, 0x10, 1, 0x80004u, 1, 1080, 1920, 1080, 1920, 0x00050005u, 4,
                                      1, 1080, 1920, 1080, 1920, 0x00050005u, 4 };
        CHECK(exactly(&cr, len, o, v, 18));
    }
    sw.scaler = RD3N48_SCALER_NAVI48;
    query711(0, RD3K5_CAP_PLANESCALER);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_CAPS);
    CHECK(inf.scaler == 2);
    {
        /* Navi48's n48_agdc_fill_pipeline_caps type 0x10 bytes, reply offsets */
        static const unsigned o[] = { 0x00, 0x04, 0x08, 0x0c, 0x28, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44,
                                      0x1b0, 0x1b8, 0x1bc, 0x1c0, 0x1c4, 0x1c8, 0x1cc };
        static const uint32_t v[] = { 0, 0x10, 1, 0x80004u, 1, 1920, 1080, 1920, 1080, 0x00050005u, 4,
                                      1, 1920, 1080, 1920, 1080, 0x00050005u, 4 };
        CHECK(exactly(&cr, len, o, v, 18));
    }
    sw.scaler = 7;   /* unknown: NullMoth's */
    query711(0, RD3K5_CAP_PLANESCALER);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_CAPS);
    CHECK(inf.scaler == 0 && rd32(&cr, 0x40) == 0x70007u && rd32(&cr, 0x30) == 1080);
    /* the scaler of a head without timing stays refused */
    sw.scaler = RD3N48_SCALER_AMD;
    query711(1, RD3K5_CAP_PLANESCALER);
    CHECK(rd3n48_answer(RD3K5_CMD_FBCAPEX, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_NOTFOUND);
    CHECK(inf.type == 0x10 && why == 0x31);   /* the log line names the type */
    sw.scaler = RD3N48_SCALER_DEFAULT;

    /* ---- (b) 0x921 flags per switch value; the rest of the link unchanged */
    {
        static const struct { uint32_t sw; uint64_t flags; } k[] = {
            { 0, 0xb }, { 1, 0xa }, { 2, 0 }, { 3, 0xb }, { 0xffffffffu, 0xb } };
        for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
            CHECK(rd3n48_link_flags(k[i].sw) == k[i].flags);
            sw.linkflags = k[i].sw;
            memset(&lq, 0, sizeof lq);
            lq.id = 0;
            CHECK(rd3n48_answer(RD3K5_CMD_LINKCONFIG, &lq, sizeof lq, &lr, sizeof lr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_LINK);
            CHECK(lr.flags == k[i].flags && inf.flags == k[i].flags && inf.type == 0xffffffffu);
            CHECK((lr.flags & 1) == (k[i].flags & 1));
            CHECK(lr.groupID == -1 && lr.port == 1 && lr.state == 1 && lr.timing.hActive == 1920 && lr.timing.vActive == 1080);
            CHECK(inf.w == 1920 && inf.h == 1080);
        }
        sw.linkflags = RD3N48_LINKFLAGS_NOBIT0;
        lq.id = 1;   /* no timing: refused as before */
        CHECK(rd3n48_answer(RD3K5_CMD_LINKCONFIG, &lq, sizeof lq, &lr, sizeof lr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_NOTFOUND);
        sw.linkflags = RD3N48_LINKFLAGS_DEFAULT;
    }
    /* another command is not ours */
    CHECK(rd3n48_answer(0x925, &cq, sizeof cq, &cr, sizeof cr, h, 2, &sw, &why, &idx, &inf) == RD3K5_R_UNSUPPORTED);

    /* ---- the type slots: every single-bit type up to 0x8000, past 32 */
    CHECK(rd3n48_type_slot(0x1) == 0 && rd3n48_type_slot(0x10) == 4 && rd3n48_type_slot(0x20) == 5);
    CHECK(rd3n48_type_slot(0x40) == 6 && rd3n48_type_slot(0x80) == 7 && rd3n48_type_slot(0x8000) == 15);
    CHECK(rd3n48_type_slot(0) == RD3N48_TYPE_OTHER && rd3n48_type_slot(0x11) == RD3N48_TYPE_OTHER);
    CHECK(rd3n48_type_slot(0x10000) == RD3N48_TYPE_OTHER && rd3n48_type_slot(0xffffffffu) == RD3N48_TYPE_OTHER);
    {
        uint32_t asked[RD3N48_TYPE_SLOTS] = { 0 }, ans[RD3N48_TYPE_SLOTS] = { 0 };
        char b[RD3N48_TYPES_TEXT];
        rd3n48_types_text(b, sizeof b, asked, ans);
        CHECK(strcmp(b, "none") == 0);
        asked[rd3n48_type_slot(0x10)] = 56; ans[rd3n48_type_slot(0x10)] = 56;
        asked[rd3n48_type_slot(0x40)] = 56; ans[rd3n48_type_slot(0x40)] = 54;
        asked[rd3n48_type_slot(0x1)] = 3;
        asked[RD3N48_TYPE_OTHER] = 2;
        rd3n48_types_text(b, sizeof b, asked, ans);
        CHECK(strcmp(b, "0x1 3/0, 0x10 56/56, 0x40 56/54, other 2/0") == 0);
        if (strcmp(b, "0x1 3/0, 0x10 56/56, 0x40 56/54, other 2/0")) printf("  got '%s'\n", b);
        /* the longest text fits the buffer the kext uses */
        for (unsigned k = 0; k < RD3N48_TYPE_SLOTS; k++) asked[k] = ans[k] = 0xffffffffu;
        memset(b, 'x', sizeof b);
        rd3n48_types_text(b, sizeof b, asked, ans);
        CHECK(strlen(b) < sizeof b - 1);
        CHECK(strstr(b, "0x8000 4294967295/4294967295, other 4294967295/4294967295") != NULL);
        /* a short buffer is cut, never overrun */
        {
            char s[12];
            memset(s, 'x', sizeof s);
            rd3n48_types_text(s, 8, asked, ans);
            CHECK(strlen(s) == 7 && s[8] == 'x');
        }
    }

    /* ---- the switch texts */
    CHECK(strstr(rd3n48_ib_text(3), "zero-filled") && strstr(rd3n48_ib_text(0), "refused"));
    CHECK(strstr(rd3n48_ib_text(2), "0x40 refused") && strstr(rd3n48_scaler_text(2), "WIDTH"));
    CHECK(strstr(rd3n48_ib_text(9), "unknown") && strstr(rd3n48_linkflags_text(1), "0xa"));
    CHECK(strstr(rd3n48_linkflags_text(5), "unknown") && strstr(rd3n48_scaler_text(1), "AMD"));
    CHECK(strstr(rd3n48_scaler_text(3), "unknown"));

    if (fails) {
        printf("test_agdc_n48: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_agdc_n48: all passed\n");
    return 0;
}
