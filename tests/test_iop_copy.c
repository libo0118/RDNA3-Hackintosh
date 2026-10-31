/*
 * test_iop_copy.c - common/rdna3_iop_copy.h on the host (DRIVER-PLAN N61): the display pipe's copy of a presented
 * surface into the scanout puts the same bytes where the 32-bit loop put them, with either loop, at any pitch.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdna3_iop_copy.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* the reference: the old loop of performTransaction (32-bit words, min(pitch) & ~3 per row) */
static void ref_copy(uint8_t *dst, uint32_t dpitch, const uint8_t *src, uint32_t spitch, uint32_t h, uint32_t row_bytes)
{
    for (uint32_t y = 0; y < h; y++) memcpy(dst + (size_t)y * dpitch, src + (size_t)y * spitch, row_bytes);
}

static int one(uint32_t w, uint32_t h, uint32_t spitch, uint32_t dpitch, uint32_t soff, uint32_t doff, int want_width)
{
    const size_t sn = (size_t)spitch * h + 16, dn = (size_t)dpitch * h + 16;
    uint8_t *src = malloc(sn), *a = malloc(dn), *b = malloc(dn);
    if (!src || !a || !b) { free(src); free(a); free(b); return 0; }
    for (size_t i = 0; i < sn; i++) src[i] = (uint8_t)(i * 131u + 7u);
    memset(a, 0xA5, dn); memset(b, 0xA5, dn);
    const uint32_t row = ((spitch < dpitch ? spitch : dpitch) & ~3u) < w * 4u ? ((spitch < dpitch ? spitch : dpitch) & ~3u) : w * 4u;
    const int width = rd3_iop_copy_rows(a + doff, dpitch, src + soff, spitch, h, row);
    ref_copy(b + doff, dpitch, src + soff, spitch, h, row);
    const int ok = !memcmp(a, b, dn) && width == want_width;
    free(src); free(a); free(b);
    return ok;
}

/* N63: the core's GART range table */
static void test_gart_table(void)
{
    struct rd3_ig t;
    uint64_t off = 0, off2 = 0, off3 = 0, o = 0, pg = 0;
    memset(&t, 0, sizeof t);
    rd3_ig_reset(&t, 4u << 20, 1000);
    CHECK(t.base == (4u << 20) && t.pages == 1000 && t.live == 0);
    CHECK(rd3_ig_alloc(&t, 0, &off) == 0 && rd3_ig_alloc(&t, 1001, &off) == 0);   /* nothing, too big */
    const uint64_t h1 = rd3_ig_alloc(&t, 400, &off);
    CHECK(h1 && off == (4u << 20) && rd3_ig_find(&t, h1) && t.live == 1);
    const uint64_t h2 = rd3_ig_alloc(&t, 400, &off2);
    CHECK(h2 && h2 != h1 && off2 == (4u << 20) + 400u * 4096u);
    CHECK(rd3_ig_alloc(&t, 201, &off3) == 0);                                        /* 200 left */
    CHECK(rd3_ig_release(&t, h1, &o, &pg) && o == off && pg == 400 && t.live == 1);
    CHECK(!rd3_ig_find(&t, h1) && !rd3_ig_release(&t, h1, &o, &pg));                /* gone, once */
    const uint64_t h3 = rd3_ig_alloc(&t, 300, &off3);                                /* first fit: the hole at 0 */
    CHECK(h3 && off3 == (4u << 20) && h3 != h1);
    CHECK(!rd3_ig_find(&t, h1));                                                     /* same slot, new generation */
    const uint64_t h4 = rd3_ig_alloc(&t, 100, &o);                                   /* between 300 and 400 */
    CHECK(h4 && o == (4u << 20) + 300u * 4096u);
    const uint64_t h5 = rd3_ig_alloc(&t, 200, &o);                                   /* after h2 */
    CHECK(h5 && o == (4u << 20) + 800u * 4096u);
    CHECK(rd3_ig_alloc(&t, 1, &o) == 0);                                             /* full */
    /* a reset (GPU reset, wake: a new GART table) forgets every range; old handles name nothing */
    rd3_ig_reset(&t, 4u << 20, 1000);
    CHECK(!rd3_ig_find(&t, h2) && !rd3_ig_find(&t, h3) && t.live == 0);
    const uint64_t h6 = rd3_ig_alloc(&t, 10, &o);
    CHECK(h6 && h6 != h2 && h6 != h3 && h6 != h4 && h6 != h5 && o == (4u << 20));
    CHECK(!rd3_ig_find(&t, 0) && !rd3_ig_find(&t, 0x100000000ull) && !rd3_ig_find(&t, ((uint64_t)t.gen << 32) | 33u));
    /* slots run out before the area */
    rd3_ig_reset(&t, 0, 1u << 20);
    unsigned got = 0;
    for (unsigned i = 0; i < RD3_IG_SLOTS + 2; i++) got += rd3_ig_alloc(&t, 1, &o) != 0;
    CHECK(got == RD3_IG_SLOTS && t.live == RD3_IG_SLOTS);
    /* the source rectangle inside the range */
    CHECK(rd3_ig_rect_ok(1080ull * 7680, 0, 7680, 7680, 1080));
    CHECK(!rd3_ig_rect_ok(1080ull * 7680, 4, 7680, 7680, 1080));
    CHECK(rd3_ig_rect_ok(1080ull * 7680 + 4, 4, 7680, 7680, 1080));
    CHECK(rd3_ig_rect_ok(1079ull * 7744 + 7680, 0, 7744, 7680, 1080));               /* the last row needs no pad */
    CHECK(!rd3_ig_rect_ok(1079ull * 7744 + 7679, 0, 7744, 7680, 1080));
    CHECK(!rd3_ig_rect_ok(100, 0, 8, 16, 2) && !rd3_ig_rect_ok(100, 0, 8, 8, 0) && !rd3_ig_rect_ok(100, 100, 8, 8, 1));
    CHECK(rd3_ig_rect_ok(~0ull, 0, 0xffffffffu, 0xffffffffu, 0xffffffffu));          /* the largest span fits 64 bits */
}

/* N63: the descriptor's physical segments as a page list in GART order */
static void test_pages(void)
{
    uint64_t l[16], n = 0;
    /* page-aligned, two segments: 3 pages + 2 pages */
    CHECK(rd3_iop_pages_add(l, 16, &n, 0, 0, 0x10000, 3 * 4096) == 0 && n == 3);
    CHECK(rd3_iop_pages_add(l, 16, &n, 0, 3 * 4096, 0x80000, 2 * 4096 - 100) == 0 && n == 5);
    CHECK(l[0] == 0x10000 && l[2] == 0x12000 && l[3] == 0x80000 && l[4] == 0x81000);
    CHECK(rd3_iop_range_pages(0, 5 * 4096 - 100) == 5);
    /* a descriptor that starts 0x100 into its page, split in the middle of a page that continues */
    n = 0;
    CHECK(rd3_iop_pages_add(l, 16, &n, 0x100, 0, 0x20100, 0x1f80) == 0 && n == 3);   /* ends at 0x22080 */
    CHECK(rd3_iop_pages_add(l, 16, &n, 0x100, 0x1f80, 0x22080, 0xf80 + 4096) == 0 && n == 4);
    CHECK(l[0] == 0x20000 && l[1] == 0x21000 && l[2] == 0x22000 && l[3] == 0x23000);
    CHECK(rd3_iop_range_pages(0x100, 0x1f80 + 0xf80 + 4096) == 4);
    /* a segment that resumes inside ANOTHER page, or at a page start where the range is mid-page: refused */
    n = 0;
    CHECK(rd3_iop_pages_add(l, 16, &n, 0, 0, 0x30000, 0x800) == 0 && n == 1);
    CHECK(rd3_iop_pages_add(l, 16, &n, 0, 0x800, 0x40800, 0x800) == -1);
    CHECK(rd3_iop_pages_add(l, 16, &n, 0, 0x800, 0x40000, 0x800) == -1);
    CHECK(rd3_iop_pages_add(l, 16, &n, 0, 0x800, 0x30800, 0x800) == 0 && n == 1);   /* the same page continued */
    n = 0;
    CHECK(rd3_iop_pages_add(l, 16, &n, 0, 0x10, 0x30010, 16) == -1);   /* mid-page start, nothing before it */
    /* the list's capacity */
    n = 0;
    CHECK(rd3_iop_pages_add(l, 2, &n, 0, 0, 0x50000, 3 * 4096) == -2 && n == 2);
    CHECK(rd3_iop_pages_add(l, 2, &n, 0, 0, 0x50000, 0) == 0);
}

static void test_sdma_policy(void)
{
    uint32_t x, y;
    CHECK(rd3_iop_sdma_verify_due(0) && rd3_iop_sdma_verify_due(255) && !rd3_iop_sdma_verify_due(257) && rd3_iop_sdma_verify_due(272));
    for (uint64_t seq = 0; seq < 50; seq++)
        for (uint32_t i = 0; i < 16; i++) {
            rd3_iop_sample_xy(i, 1920, 1080, seq, &x, &y);
            CHECK(x < 1920 && y < 1080);
        }
    rd3_iop_sample_xy(0, 1920, 1080, 7, &x, &y);  CHECK(x == 0 && y == 0);
    rd3_iop_sample_xy(15, 1920, 1080, 7, &x, &y); CHECK(x == 1919 && y == 1079);
    rd3_iop_sample_xy(5, 1, 1, 3, &x, &y);        CHECK(x == 0 && y == 0);
    rd3_iop_sample_xy(5, 0, 0, 3, &x, &y);        CHECK(x == 0 && y == 0);
    CHECK(!strcmp(rd3_iop_sdma_fb_name(RD3_ISF_COPY), "copy") && !strcmp(rd3_iop_sdma_fb_name(RD3_ISF_N), "?"));
    CHECK(!strcmp(rd3_iop_sdma_fb_name(RD3_ISF_SCANOUT), "scanout") && RD3_ISF_N == 8);
    CHECK(!strcmp(rd3_iop_sdma_fb_name(RD3_ISF_FORMAT), "format"));
    /* the scanout's VRAM offset against its BAR address */
    CHECK(rd3_iop_scanout_ok(0x80000000ull, 0x80000000ull + 0x1000000, 0x1000000, 8294400, 0));
    CHECK(rd3_iop_scanout_ok(0x80000000ull, 0x80000000ull, 0, 8294400, 16ull << 30));        /* offset 0 at the BAR's start */
    CHECK(!rd3_iop_scanout_ok(0x80000000ull, 0x80800000ull, 0, 8294400, 0));                 /* console elsewhere, offset 0 */
    CHECK(!rd3_iop_scanout_ok(0, 0x80000000ull, 0, 8294400, 0));                             /* no base learned */
    CHECK(!rd3_iop_scanout_ok(0x80000000ull, 0x1000, 0x2000, 16, 0));                        /* phys under the offset */
    CHECK(!rd3_iop_scanout_ok(0x80000000ull, 0x80000000ull + (16ull << 30) - 4096, (16ull << 30) - 4096, 8192, 16ull << 30));
}

/* bare metal 2026-10-31: 'l10r' (ARGB2101010, A[31:30] R[29:20] G[19:10] B[9:0]) into the 8-bit scanout word */
static uint32_t pack_l10r(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return (a << 30) | (r << 20) | (g << 10) | b; }
static void test_l10r(void)
{
    CHECK(rd3_iop_fmt_kind(0x42475241u) == RD3_IOP_FMT_IS_BGRA);
    CHECK(rd3_iop_fmt_kind(0x6c313072u) == RD3_IOP_FMT_IS_L10R);
    CHECK(rd3_iop_fmt_kind(0x77333072u) == RD3_IOP_FMT_IS_OTHER && rd3_iop_fmt_kind(0) == RD3_IOP_FMT_IS_OTHER);
    /* the fourcc is the ASCII name, big-endian in the word (IOSurface's convention) */
    CHECK(RD3_IOP_FMT_L10R == (((uint32_t)'l' << 24) | ((uint32_t)'1' << 16) | ((uint32_t)'0' << 8) | (uint32_t)'r'));
    /* opaque white, black, pure channels: each channel's top 8 bits land in the BGRA word */
    CHECK(rd3_iop_l10r_px(pack_l10r(3, 1023, 1023, 1023)) == 0xffffffffu);
    CHECK(rd3_iop_l10r_px(pack_l10r(3, 0, 0, 0)) == 0xff000000u);
    CHECK(rd3_iop_l10r_px(pack_l10r(3, 1023, 0, 0)) == 0xffff0000u);   /* red -> bits 23:16 */
    CHECK(rd3_iop_l10r_px(pack_l10r(3, 0, 1023, 0)) == 0xff00ff00u);   /* green -> bits 15:8 */
    CHECK(rd3_iop_l10r_px(pack_l10r(3, 0, 0, 1023)) == 0xff0000ffu);   /* blue -> bits 7:0 */
    CHECK(rd3_iop_l10r_px(pack_l10r(0, 0, 0, 0)) == 0u);
    CHECK(rd3_iop_l10r_px(pack_l10r(1, 0, 0, 0)) == 0x55000000u && rd3_iop_l10r_px(pack_l10r(2, 0, 0, 0)) == 0xaa000000u);
    /* 10 -> 8 bits drops the two low bits: 0x200 (half) -> 0x80, 0x203 -> 0x80, 0x204 -> 0x81, 0x3 -> 0 */
    CHECK(rd3_iop_l10r_px(pack_l10r(3, 0x200, 0x203, 0x204)) == 0xff808081u);
    CHECK(rd3_iop_l10r_px(pack_l10r(3, 0x3, 0x3, 0x3)) == 0xff000000u);
    /* rows: both loops give the per-pixel conversion; bytes beyond each row and below the last stay untouched */
    const uint32_t cases[][5] = {   /* w, h, spitch, dpitch, src offset */
        { 1920, 6, 7680, 7680, 0 },  /* WindowServer's 1920x1080 l10r frame: 64-bit loop */
        { 1366, 4, 5504, 5464, 0 },  /* a padded source pitch */
        { 1365, 4, 5460, 5504, 0 },  /* an odd width: the 32-bit loop */
        { 640, 3, 2560, 2560, 4 },   /* a source only 4-byte aligned */
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        const uint32_t w = cases[c][0], h = cases[c][1], sp = cases[c][2], dp = cases[c][3], so = cases[c][4];
        const size_t sn = (size_t)sp * h + 16, dn = (size_t)dp * h + 16;
        uint8_t *src = malloc(sn), *dst = malloc(dn);
        if (!src || !dst) { free(src); free(dst); CHECK(0); continue; }
        for (size_t i = 0; i < sn; i++) src[i] = (uint8_t)(i * 151u + 3u);
        memset(dst, 0xA5, dn);
        const uint32_t row = w * 4u;
        const int width = rd3_iop_convert_l10r_rows(dst, dp, src + so, sp, h, row);
        CHECK(width == (((so & 7u) || (sp & 7u) || (dp & 7u) || (row & 7u)) ? 4 : 8));
        int bad = 0;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v, o;
                memcpy(&v, src + so + (size_t)y * sp + 4u * x, 4);
                memcpy(&o, dst + (size_t)y * dp + 4u * x, 4);
                if (o != rd3_iop_l10r_px(v)) bad++;
            }
            for (uint32_t b = row; b < dp; b++) if (dst[(size_t)y * dp + b] != 0xA5) bad++;
        }
        for (size_t b = (size_t)dp * h; b < dn; b++) if (dst[b] != 0xA5) bad++;
        CHECK(bad == 0);
        free(src); free(dst);
    }
}

int main(void)
{
    test_gart_table();
    test_pages();
    test_sdma_policy();
    test_l10r();
    CHECK(rd3_iop_copy64_ok(0x1000, 0x2000, 7680, 7680, 7680));
    CHECK(!rd3_iop_copy64_ok(0x1004, 0x2000, 7680, 7680, 7680));
    CHECK(!rd3_iop_copy64_ok(0x1000, 0x2000, 7684, 7680, 7680));
    CHECK(!rd3_iop_copy64_ok(0x1000, 0x2000, 7680, 7680, 7676));
    /* malloc gives 16-byte alignment: offsets 0 keep 8-byte alignment */
    CHECK(one(1920, 8, 7680, 7680, 0, 0, 8));        /* WindowServer's 1920x1080 frame, rows of 7680 */
    CHECK(one(1920, 8, 7744, 7680, 0, 0, 8));        /* a padded source pitch */
    CHECK(one(1366, 6, 5464, 5504, 0, 0, 8));        /* 1366 * 4 = 5464 = 683 * 8 */
    CHECK(one(1365, 6, 5460, 5504, 0, 0, 4));        /* an odd width: 5460 is not a multiple of 8 */
    CHECK(one(1920, 4, 7680, 7680, 4, 0, 4));        /* a source only 4-byte aligned */
    CHECK(one(1920, 4, 7680, 7680, 0, 4, 4));        /* a destination only 4-byte aligned */
    CHECK(one(800, 5, 3204, 3200, 0, 0, 4));         /* an odd source pitch */
    CHECK(rd3_iop_mbps(8294400, 4000) == 2073 && rd3_iop_mbps(1, 0) == 0);
    CHECK(rd3_iop_copy_log_due(0) && rd3_iop_copy_log_due(3) && !rd3_iop_copy_log_due(4) && rd3_iop_copy_log_due(2048));
    printf("test_iop_copy: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
