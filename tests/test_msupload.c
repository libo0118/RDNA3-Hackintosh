/* Which buffer -> multisample texture uploads the plugin carries (metal/plugin/nvmtl_msupload.h).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "nvmtl_msupload.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* fmt samples type layers mips level layer asp  W H  x y w h  row off buf */
static int mk(uint32_t fmt, uint32_t smp, uint32_t type, uint32_t layers, uint32_t mips, uint32_t lvl, uint32_t lay, uint32_t asp,
              uint32_t W, uint32_t H, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t row, uint64_t off, uint64_t buf, nvmtl_msu_plan *p)
{
    const char *why = 0;
    int rc = nvmtl_msu_plan_make(fmt, smp, type, layers, mips, lvl, lay, asp, W, H, x, y, w, h, row, off, buf, p, &why);
    CHECK(rc == 0 || (why && why[0]));
    return rc;
}

int main(void)
{
    nvmtl_msu_plan p;
    /* Chess: a 971x680 4-sample BGRA8 sRGB texture, the whole image, a tightly padded buffer */
    CHECK(mk(50, 4, 4, 1, 1, 0, 0, 0, 971, 680, 0, 0, 971, 680, 3904, 0, 3904u * 680, &p) == 0 && p.view_fmt == 44 && p.mode == NVMTL_MSU_BGRA8 && p.bpp == 4
          && p.inter == 0 && !p.uint_out);   /* a row of one element per pixel: the single-sample layout */
    CHECK(mk(44, 8, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 64, 64, 256, 0, 256u * 64, &p) == 0 && p.view_fmt == 44);
    CHECK(mk(37, 2, 4, 1, 1, 0, 0, 0, 64, 64, 1, 2, 3, 4, 12, 7, 12u * 4 + 7, &p) == 0 && p.view_fmt == 37 && p.mode == NVMTL_MSU_RGBA8 && p.inter == 0);
    /* Chess (card run 2026-10-25f): the stencil resolve uploads a 971x680 4-sample R8_UINT texture, bytesPerRow 3884 = 971 * 4 samples * 1 B, buffer of 4 MiB */
    CHECK(mk(13, 4, 4, 1, 1, 0, 0, 0, 971, 680, 0, 0, 971, 680, 3884, 0, 4194304, &p) == 0 && p.view_fmt == 13 && p.mode == NVMTL_MSU_U8 && p.uint_out
          && p.bpp == 1 && p.inter == 1 && p.row == 3884);
    CHECK(mk(13, 4, 4, 1, 1, 0, 0, 0, 971, 680, 0, 0, 971, 680, 3884, 0, 3884u * 679 + 3884, &p) == 0);
    CHECK(mk(13, 4, 4, 1, 1, 0, 0, 0, 971, 680, 0, 0, 971, 680, 3884, 0, 3884u * 679 + 3883, &p) != 0);   /* one byte short */
    CHECK(mk(13, 4, 4, 1, 1, 0, 0, 0, 971, 680, 0, 0, 971, 680, 971, 0, 971u * 680, &p) == 0 && p.inter == 0);   /* a single-sample row */
    CHECK(mk(13, 8, 4, 1, 1, 0, 0, 0, 16, 16, 0, 0, 16, 16, 128, 0, 128u * 16, &p) == 0 && p.inter == 1);
    CHECK(mk(13, 8, 4, 1, 1, 0, 0, 0, 16, 16, 0, 0, 16, 16, 127, 0, 127u * 16, &p) == 0 && p.inter == 0);
    CHECK(mk(20, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 64, 0, 512, &p) == 0 && p.mode == NVMTL_MSU_U8X2 && p.bpp == 2 && p.uint_out && p.inter == 1);
    CHECK(mk(41, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 128, 0, 1024, &p) == 0 && p.mode == NVMTL_MSU_U8X4 && p.bpp == 4 && p.inter == 1);
    CHECK(mk(74, 2, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 32, 0, 256, &p) == 0 && p.mode == NVMTL_MSU_U16 && p.bpp == 2 && p.inter == 1);
    CHECK(mk(98, 2, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 64, 0, 512, &p) == 0 && p.mode == NVMTL_MSU_U32 && p.bpp == 4 && p.inter == 1);
    CHECK(mk(43, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 32, 0, 256, &p) == 0 && p.view_fmt == 37);
    CHECK(mk(9, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 10, 10, 10, 0, 100, &p) == 0 && p.mode == NVMTL_MSU_R8 && p.bpp == 1);
    CHECK(mk(15, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 10, 10, 16, 0, 160, &p) == 0 && p.view_fmt == 9);
    CHECK(mk(16, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 10, 10, 20, 0, 200, &p) == 0 && p.mode == NVMTL_MSU_RG8 && p.bpp == 2);
    /* bytesPerRow 0 = one tight row of `samples` elements per pixel */
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 5, 1, 0, 0, 80, &p) == 0 && p.row == 80 && p.inter == 1);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 5, 2, 0, 0, 80, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 5, 1, 0, 0, 79, &p) != 0);
    /* the exact fit and one byte less */
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 2, 100, 0, 164, &p) == 0 && p.inter == 1);   /* 100 + 4 px * 4 samples * 4 B */
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 2, 100, 0, 163, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 2, 100, 1, 164, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 2, 60, 0, 76, &p) == 0 && p.inter == 0);   /* a short row: 60 + 4 px * 4 B */
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 2, 60, 0, 75, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 1, 16, 0, 16, &p) == 0);
    /* sample counts */
    CHECK(mk(44, 1, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 3, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 16, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    /* arrays, levels, layers, 3D / cube types, depth option */
    CHECK(mk(44, 4, 8, 2, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 4, 2, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 2, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 1, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 1, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 2, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    /* depth/stencil and other colour formats */
    for (uint32_t f = 124; f <= 130; f++) CHECK(mk(f, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(97, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 32, 0, 128, &p) != 0);   /* RGBA16F */
    CHECK(mk(14, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);    /* R8_SINT */
    CHECK(mk(64, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0, 64, &p) != 0);    /* A2B10G10R10 */
    /* region */
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 0, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 0, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 61, 0, 4, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 60, 60, 4, 4, 16, 0, 64, &p) == 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 64, 0, 1, 1, 4, 0, 4, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 0xffffffffu, 4, 16, 0, 64, &p) != 0);
    /* row smaller than the region's row, offsets and sizes near 2^64 */
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 15, 0, 1000, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 0xffffffffffffff00ull, 0, 1000, &p) != 0);
    CHECK(mk(13, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 0x100000000ull, 0, 0xffffffffffffffffull, &p) != 0);   /* a row beyond the pass's 32 bits */
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 16, 0xfffffffffffffff0ull, 1000, &p) != 0);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_msupload: ok\n");
    return 0;
}
