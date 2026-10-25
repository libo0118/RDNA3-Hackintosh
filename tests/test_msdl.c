/* The multisample texture -> buffer download of the Metal plugin (metal/plugin/nvmtl_msdl.h, nvmtl_msbuf.h): which copies it carries,
 * and that the word-by-word walk that msdl.comp does covers exactly the bytes of the region, once each, in Apple's layout.
 * Card run 2026-10-25f (Chess): stencil of a 971x680 4-sample D32_S8 texture -> buffer, bytesPerRow 3884.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvmtl_msdl.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* fmt samples type layers mips level layer asp  W H  x y w h  row off buf */
static int mk(uint32_t fmt, uint32_t smp, uint32_t type, uint32_t layers, uint32_t mips, uint32_t lvl, uint32_t lay, uint32_t asp,
              uint32_t W, uint32_t H, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t row, uint64_t off, uint64_t buf, nvmtl_msdl_plan *p)
{
    const char *why = 0;
    int rc = nvmtl_msdl_plan_make(fmt, smp, type, layers, mips, lvl, lay, asp, W, H, x, y, w, h, row, off, buf, p, &why);
    CHECK(rc == 0 || (why && why[0]));
    return rc;
}

static uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

/* Walk every aligned word that covers the region the way the shader does and check: each (pixel, sample, byte) of the region is
 * produced exactly once at the byte address nvmtl_msbuf_elem_offset() says; no byte outside the region (the padding of a row, the bytes
 * before and after) is ever claimed. */
static void walk(uint32_t base_addr_lo, uint32_t samples, uint32_t w, uint32_t h, uint32_t bpp, uint32_t row)
{
    const uint32_t lead = base_addr_lo & 3u;
    const uint64_t used = nvmtl_msbuf_used(w, samples, bpp), span = nvmtl_msbuf_span(row, h, w, samples, bpp);
    const uint32_t nwords = nvmtl_msbuf_words(lead, span);
    uint8_t *hit = calloc((size_t)nwords * 4u + 8u, 1);
    CHECK(hit != NULL);
    if (!hit) return;
    uint64_t claimed = 0;
    for (uint32_t i = 0; i < nwords; i++)
        for (uint32_t b = 0; b < 4; b++) {
            uint32_t px, y, s, k;
            if (!nvmtl_msbuf_locate(i, b, lead, row, (uint32_t)used, h, bpp, samples, &px, &y, &s, &k)) continue;
            const uint64_t want = nvmtl_msbuf_elem_offset(row, bpp, samples, px, y, s) + k;   /* from byte 0 of the region */
            CHECK(px < w && y < h && s < samples && k < bpp);
            CHECK(want + lead == 4ull * i + b);
            hit[4u * i + b]++;
            claimed++;
        }
    CHECK(claimed == (uint64_t)h * used);
    for (uint64_t o = 0; o < (uint64_t)nwords * 4; o++) {
        const int inside = o >= lead && (o - lead) / row < h && (o - lead) % row < used;
        CHECK(hit[o] == (inside ? 1 : 0));
    }
    free(hit);
}

int main(void)
{
    nvmtl_msdl_plan p;
    /* Chess, the first step of the stencil resolve: D32_S8 (130), 4 samples, option StencilFromDepthStencil (4), whole image, row 3884, a 4 MiB buffer */
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 971, 680, 0, 0, 971, 680, 3884, 0, 4194304, &p) == 0 && p.aspect == NVMTL_MSDL_ASPECT_STENCIL && p.mode == NVMTL_MSDL_U8
          && p.uint_in && p.bpp == 1 && p.view_fmt == 130 && p.row == 3884 && p.used == 3884 && p.span == 3884ull * 680);
    /* depth of the same texture (no option, or the depth option) */
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 0, 971, 680, 0, 0, 971, 680, 3884 * 4, 0, 3884u * 4 * 680, &p) == 0 && p.aspect == NVMTL_MSDL_ASPECT_DEPTH && p.mode == NVMTL_MSDL_D32F && p.bpp == 4 && !p.uint_in);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 2, 971, 680, 0, 0, 971, 680, 3884 * 4, 0, 3884u * 4 * 680, &p) == 0 && p.aspect == NVMTL_MSDL_ASPECT_DEPTH);
    CHECK(mk(126, 2, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 64, 64, 512, 0, 512u * 64, &p) == 0 && p.mode == NVMTL_MSDL_D32F);
    CHECK(mk(124, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 64, 64, 512, 0, 512u * 64, &p) == 0 && p.mode == NVMTL_MSDL_D16 && p.bpp == 2);
    CHECK(mk(128, 4, 4, 1, 1, 0, 0, 2, 64, 64, 0, 0, 64, 64, 512, 0, 512u * 64, &p) == 0 && p.mode == NVMTL_MSDL_D16);
    CHECK(mk(127, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 64, 64, 256, 0, 256u * 64, &p) == 0 && p.aspect == NVMTL_MSDL_ASPECT_STENCIL);   /* S8 has only stencil */
    CHECK(mk(129, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 64, 64, 256, 0, 256u * 64, &p) == 0 && p.aspect == NVMTL_MSDL_ASPECT_STENCIL);   /* D24_S8: its stencil is a byte */
    CHECK(mk(129, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 64, 64, 1024, 0, 1024u * 64, &p) != 0);   /* D24 depth: no byte layout */
    CHECK(mk(125, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 64, 64, 1024, 0, 1024u * 64, &p) != 0);
    /* aspect options that the format does not have, or two at once */
    CHECK(mk(126, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 8, 8, 128, 0, 1024, &p) != 0);
    CHECK(mk(127, 4, 4, 1, 1, 0, 0, 2, 64, 64, 0, 0, 8, 8, 32, 0, 256, &p) != 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 6, 64, 64, 0, 0, 8, 8, 128, 0, 1024, &p) != 0);
    CHECK(mk(44, 4, 4, 1, 1, 0, 0, 2, 64, 64, 0, 0, 8, 8, 128, 0, 1024, &p) != 0);   /* a depth option on a colour texture */
    /* colour: sRGB through the UNORM twin, the integer formats */
    CHECK(mk(50, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 128, 0, 1024, &p) == 0 && p.view_fmt == 44 && p.mode == NVMTL_MSDL_BGRA8 && p.bpp == 4 && !p.uint_in);
    CHECK(mk(43, 8, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 256, 0, 2048, &p) == 0 && p.view_fmt == 37 && p.mode == NVMTL_MSDL_RGBA8);
    CHECK(mk(9, 2, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 16, 0, 128, &p) == 0 && p.mode == NVMTL_MSDL_R8 && p.bpp == 1);
    CHECK(mk(16, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 64, 0, 512, &p) == 0 && p.mode == NVMTL_MSDL_RG8 && p.bpp == 2);
    CHECK(mk(13, 4, 4, 1, 1, 0, 0, 0, 971, 680, 0, 0, 971, 680, 3884, 0, 4194304, &p) == 0 && p.uint_in && p.mode == NVMTL_MSDL_U8 && p.view_fmt == 13);
    CHECK(mk(20, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 64, 0, 512, &p) == 0 && p.mode == NVMTL_MSDL_U8X2 && p.uint_in);
    CHECK(mk(41, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 128, 0, 1024, &p) == 0 && p.mode == NVMTL_MSDL_U8X4);
    CHECK(mk(74, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 64, 0, 512, &p) == 0 && p.mode == NVMTL_MSDL_U16);
    CHECK(mk(98, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 128, 0, 1024, &p) == 0 && p.mode == NVMTL_MSDL_U32);
    CHECK(mk(97, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 256, 0, 2048, &p) != 0);   /* RGBA16F */
    CHECK(mk(64, 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 8, 8, 128, 0, 1024, &p) != 0);   /* A2B10G10R10 */
    /* sample counts, types, subresources */
    CHECK(mk(130, 1, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 8, 8, 8, 0, 64, &p) != 0);
    CHECK(mk(130, 3, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 8, 8, 24, 0, 192, &p) != 0);
    CHECK(mk(130, 16, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 8, 8, 128, 0, 1024, &p) != 0);
    CHECK(mk(130, 4, 8, 2, 1, 0, 0, 4, 64, 64, 0, 0, 8, 8, 32, 0, 256, &p) != 0);   /* a multisample array */
    CHECK(mk(130, 4, 2, 1, 1, 0, 0, 4, 64, 64, 0, 0, 8, 8, 32, 0, 256, &p) != 0);   /* a plain 2D texture is the 1-sample copy's business */
    CHECK(mk(130, 4, 4, 1, 2, 0, 0, 4, 64, 64, 0, 0, 8, 8, 32, 0, 256, &p) != 0);
    CHECK(mk(130, 4, 4, 1, 1, 1, 0, 4, 64, 64, 0, 0, 8, 8, 32, 0, 256, &p) != 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 1, 4, 64, 64, 0, 0, 8, 8, 32, 0, 256, &p) != 0);
    /* the row: 0 = one tight row; smaller than the region's row; padded */
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 2, 0, 0, 40, &p) == 0 && p.row == 20 && p.span == 40);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 2, 19, 0, 1000, &p) != 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 2, 20, 0, 1000, &p) == 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 2, 64, 0, 64 + 20, &p) == 0 && p.span == 84);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 2, 64, 0, 83, &p) != 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 2, 0x80000000ull, 0, ~0ull, &p) != 0);
    /* the buffer: offsets (any alignment), the last word, a size that is not a multiple of 4 */
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 1, 20, 3, 23, &p) != 0);   /* off 3, ends at 23: the last word is [20,24), 24 > 23 */
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 1, 20, 3, 24, &p) == 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 1, 20, 4, 23, &p) != 0);   /* too short */
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 1, 20, 1, 21, &p) != 0);   /* fits to the byte, but its last word [20,24) is not inside a 21 B buffer */
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 1, 20, 0xfffffffffffffff0ull, 1000, &p) != 0);
    /* region */
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 0, 4, 16, 0, 64, &p) != 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 4, 0, 16, 0, 64, &p) != 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 61, 0, 4, 4, 16, 0, 256, &p) != 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 60, 60, 4, 4, 16, 0, 256, &p) == 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 64, 0, 1, 1, 4, 0, 4, &p) != 0);
    CHECK(mk(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 0xffffffffu, 4, 16, 0, 64, &p) != 0);

    /* the word walk: Chess's whole-image copy, then odd shapes with every alignment of the region's first byte */
    walk(0, 4, 971, 8, 1, 3884);
    uint32_t seed = 12345;
    for (int n = 0; n < 400; n++) {
        const uint32_t samples = 1u << (1 + rnd(&seed) % 3), bpp = 1u + rnd(&seed) % 4, w = 1 + rnd(&seed) % 40, h = 1 + rnd(&seed) % 9;
        const uint32_t used = (uint32_t)nvmtl_msbuf_used(w, samples, bpp), row = used + rnd(&seed) % 11, lo = rnd(&seed);
        walk(lo, samples, w, h, bpp, row);
    }
    /* the dispatch: covers every word, rows of groups wrap at 32768 */
    for (uint32_t n = 1; n < 3000000u; n = n * 3 + 1) {
        uint32_t gx, gy, stride;
        nvmtl_msbuf_dispatch(n, &gx, &gy, &stride);
        CHECK(gx >= 1 && gx <= 32768 && gy >= 1 && stride == gx * 64u && (uint64_t)gx * gy * 64u >= n && (gy == 1 || (uint64_t)gx * (gy - 1) * 64u < n));
    }
    { uint32_t gx, gy, stride; nvmtl_msbuf_dispatch(0, &gx, &gy, &stride); CHECK(gx == 1 && gy == 1); }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_msdl: ok\n");
    return 0;
}
