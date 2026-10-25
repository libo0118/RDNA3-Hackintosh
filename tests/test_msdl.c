/* The multisample texture -> buffer download of the Metal plugin (metal/plugin/nvmtl_msdl.h, nvmtl_msbuf.h): which copies it carries,
 * and that the word-by-word walk that msdl.comp does covers exactly the bytes of the region, once each, in Apple's layout.
 * Card run 2026-10-25f (Chess): stencil of a 971x680 4-sample D32_S8 texture -> buffer, bytesPerRow 3884.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvmtl_msdl.h"
#include "nvmtl_msstat.h"
#include "nvmtl_msupload.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* fmt samples type layers mips level layer asp  W H  x y w h  row off buf */
static int mk(uint32_t fmt, uint32_t smp, uint32_t type, uint32_t layers, uint32_t mips, uint32_t lvl, uint32_t lay, uint32_t asp,
              uint32_t W, uint32_t H, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t row, uint64_t off, uint64_t buf, nvmtl_msdl_plan *p)
{
    const char *why = 0;
    int rc = nvmtl_msdl_plan_make(fmt, smp, type, layers, mips, lvl, lay, asp, W, H, x, y, w, h, row, off, buf, buf, p, &why);
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
    /* the allocation behind the Metal length (ECC round 1): the last word may end beyond the length when the buffer owns that memory */
    { const char *why = 0; nvmtl_msdl_plan q;
      CHECK(nvmtl_msdl_plan_make(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 1, 20, 1, 21, 24, &q, &why) == 0);   /* [1,21) -> the word [20,24) is inside 24 */
      CHECK(nvmtl_msdl_plan_make(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 1, 20, 1, 21, 23, &q, &why) != 0);
      CHECK(nvmtl_msdl_plan_make(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 1, 20, 1, 21, 0, &q, &why) != 0);   /* a cap below the length is the length */
      CHECK(nvmtl_msdl_plan_make(130, 4, 4, 1, 1, 0, 0, 4, 64, 64, 0, 0, 5, 1, 20, 2, 21, 1000, &q, &why) != 0);   /* the region itself must fit the LENGTH */ }
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
    /* ---- nvmtl_msstat.h: the SUMMARY lines, the reason table, the format index ---- */
    {
        unsigned done[NVMTL_MSS_A_N] = { 1, 2, 4 }, ref[NVMTL_MSS_R_N] = { 5, 0, 0, 1, 0, 2 }, fail[NVMTL_MSS_F_N] = { 0, 0, 0, 0, 1, 0, 0, 0, 3, 0 };
        char b[1024]; int n = nvmtl_msdl_summary_text(b, sizeof b, done, ref, fail, 7, 8);
        CHECK(n > 0 && n < 700);   /* the plugin's log line buffer is 768 B */
        CHECK(strstr(b, "msdl: SUMMARY done=7 d_colour=1 d_depth=2 d_stencil=4 refused=8 r_plan=5 r_inrp=0 r_nodev=0 r_noaddr=1 r_nousage=0 r_pipe=2 failed=4 ") == b);
        CHECK(strstr(b, "f_pipeline=1 f_view=0 f_retire=0 f_set=3 resolve_r8u=7 resolve_other=8"));
        unsigned ui[NVMTL_MSS_NFMT] = { 4 }, uf[NVMTL_MSS_NFMT] = { 0, 0, 0, 0, 0, 3 }, ur[NVMTL_MSS_R_N] = { 0 }, uff[NVMTL_MSS_F_N] = { 0 };
        n = nvmtl_msupload_summary_text(b, sizeof b, ui, uf, 4, 1, ur, uff);
        CHECK(n > 0 && n < 700);
        CHECK(strstr(b, "msupload: SUMMARY done=7 done_int=4 inter=4 flat=3 wide=1 refused=0 ") == b);
        CHECK(strstr(b, " up_inter_13=4") && strstr(b, " up_flat_44=3") && !strstr(b, "up_inter_44") && !strstr(b, "up_flat_13"));
        for (int i = 0; i < NVMTL_MSS_NFMT; i++) { ui[i] = 99999999u; uf[i] = 99999999u; }   /* counts of 8 digits: the longest line still fits the log's 768 B line buffer */
        for (int i = 0; i < NVMTL_MSS_R_N; i++) ur[i] = 99999999u;
        for (int i = 0; i < NVMTL_MSS_F_N; i++) uff[i] = 99999999u;
        n = nvmtl_msupload_summary_text(b, sizeof b, ui, uf, 99999999u, 99999999u, ur, uff);
        CHECK(n > 0 && n < 760);
        for (int i = 0; i < NVMTL_MSS_A_N; i++) done[i] = 99999999u;
        for (int i = 0; i < NVMTL_MSS_R_N; i++) ref[i] = 99999999u;
        for (int i = 0; i < NVMTL_MSS_F_N; i++) fail[i] = 99999999u;
        n = nvmtl_msdl_summary_text(b, sizeof b, done, ref, fail, 99999999u, 99999999u);
        CHECK(n > 0 && n < 760);
        CHECK(nvmtl_mss_fmt_index(13) == 0 && nvmtl_mss_fmt_index(44) == 5 && nvmtl_mss_fmt_index(97) == -1);
        for (uint32_t f = 0; f < NVMTL_MSS_NFMT; f++) {   /* every view format the upload plan can return has a slot */
            nvmtl_msu_plan q; const char *why = 0;
            const uint32_t fmts[] = { 13, 20, 41, 74, 98, 44, 37, 9, 16 };
            CHECK(nvmtl_msu_plan_make(fmts[f], 4, 4, 1, 1, 0, 0, 0, 64, 64, 0, 0, 4, 4, 0, 0, 1u << 20, &q, &why) == 0 && nvmtl_mss_fmt_index(q.view_fmt) >= 0);
        }
        nvmtl_mss_reason tab[NVMTL_MSS_REASONS]; memset(tab, 0, sizeof tab);
        char dup[40]; strcpy(dup, "same text");
        CHECK(nvmtl_mss_reason_count(tab, "same text") == 1 && nvmtl_mss_reason_count(tab, dup) == 2 && nvmtl_mss_reason_count(tab, "other") == 1 && nvmtl_mss_reason_count(tab, "same text") == 3);
        char buf[32]; unsigned last = 0;   /* a full table: the surplus is counted, none is lost */
        for (int i = 0; i < 40; i++) { snprintf(buf, sizeof buf, "reason %d", i); last = nvmtl_mss_reason_count(tab, buf); }
        CHECK(last >= 25);
        CHECK(nvmtl_mss_pow2(1) && nvmtl_mss_pow2(2) && nvmtl_mss_pow2(1024) && !nvmtl_mss_pow2(0) && !nvmtl_mss_pow2(3) && !nvmtl_mss_pow2(1023));
    }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_msdl: ok\n");
    return 0;
}
