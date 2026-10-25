/* [[color(n)]] framebuffer fetch in a translated fragment module (metal/plugin/nvmtl_spirv_fbfetch.h):
 * the scan finds the subpass-input reads at binding 192 + n, the zeroing makes them defined.
 * A valid synthetic module is also run through spirv-val (before and after the patch) when the tool exists.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvmtl_spirv_fbfetch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define I(op, ...) (((uint32_t)(sizeof((uint32_t[]){__VA_ARGS__}) / 4 + 1) << 16) | (op)), __VA_ARGS__
#define I0(op) ((1u << 16) | (op))
static size_t put(uint32_t *m, size_t n, const uint32_t *w, size_t cnt) { memcpy(m + n, w, cnt * 4); return n + cnt; }
#define PUT(m, n, ...) do { const uint32_t w_[] = { __VA_ARGS__ }; (n) = put((m), (n), w_, sizeof w_ / 4); } while (0)

/* A VALID fragment shader with two subpass inputs: binding 192 (float, a vec4 read twice) and 193 (uint, a uvec4 read once).
 * ids: 1 void, 2 fn type, 3 float, 4 vec4, 5 float subpass image, 6 ptr, 7 var(192), 8 main, 9 label, 10 uint, 11 uvec4,
 *      12 uint subpass image, 13 ptr, 14 var(193), 15 int, 16 ivec2, 17 const 0, 18 coord, 19 loaded1, 20 read1, 21 read2,
 *      22 loaded2, 23 read3, 24 out ptr, 25 out var, 26 uint0 const, 27 converted float vec4 */
static size_t build_valid(uint32_t *m)
{
    size_t n = 0;
    PUT(m, n, 0x07230203u, 0x00010300u, 0, 30, 0);
    PUT(m, n, I(17, 1), I(17, 40));                                   /* Shader, InputAttachment */
    PUT(m, n, I(14, 0, 1));                                           /* OpMemoryModel Logical GLSL450 */
    PUT(m, n, I(15, 4, 8, 0x6e69616du, 0u, 25));                      /* OpEntryPoint Fragment %8 "main" %25 */
    PUT(m, n, I(16, 8, 7));                                           /* OpExecutionMode OriginUpperLeft */
    PUT(m, n, I(71, 7, 34, 0), I(71, 7, 33, 192), I(71, 7, 43, 0));
    PUT(m, n, I(71, 14, 34, 0), I(71, 14, 33, 193), I(71, 14, 43, 1));
    PUT(m, n, I(71, 25, 30, 0));                                      /* Location 0 */
    PUT(m, n, I(19, 1), I(33, 2, 1), I(22, 3, 32), I(23, 4, 3, 4), I(25, 5, 3, 6, 0, 0, 0, 2, 0), I(32, 6, 0, 5), I(59, 6, 7, 0));
    PUT(m, n, I(21, 10, 32, 0), I(23, 11, 10, 4), I(25, 12, 10, 6, 0, 0, 0, 2, 0), I(32, 13, 0, 12), I(59, 13, 14, 0));
    PUT(m, n, I(21, 15, 32, 1), I(23, 16, 15, 2), I(43, 15, 17, 0), I(44, 16, 18, 17, 17));
    PUT(m, n, I(32, 24, 3, 4), I(59, 24, 25, 3));
    PUT(m, n, I(54, 1, 8, 0, 2), I(248, 9));
    PUT(m, n, I(61, 5, 19, 7), I(98, 4, 20, 19, 18), I(98, 4, 21, 19, 18));
    PUT(m, n, I(61, 12, 22, 14), I(98, 11, 23, 22, 18), I(112, 4, 27, 23));   /* OpConvertUToF */
    PUT(m, n, I(129, 4, 28, 20, 21));                                         /* OpFAdd */
    PUT(m, n, I(129, 4, 29, 28, 27), I(62, 25, 29), I0(253), I0(56));
    return n;
}

static int have_val(void) { static int v = -1; if (v < 0) v = system("command -v spirv-val >/dev/null 2>&1") == 0; return v; }
static int validate(const uint32_t *m, size_t n)
{
    const char *path = "build/host/test_spirv_fbfetch.valid.spv";
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(m, 4, n, f); fclose(f);
    char cmd[256]; snprintf(cmd, sizeof cmd, "spirv-val --target-env spv1.3 %s >/dev/null 2>build/host/test_spirv_fbfetch.val.log", path);
    int rc = system(cmd);
    remove(path);
    return rc;
}

/* the small hand-made module of the first tests (not valid SPIR-V, enough for the scanner) */
static size_t build(uint32_t *m, uint32_t binding, int with_read, uint32_t dim)
{
    size_t n = 0;
    PUT(m, n, 0x07230203u, 0x00010300u, 0, 20, 0);
    PUT(m, n, I(17, 40), I(71, 7, 34, 0), I(71, 7, 33, binding), I(71, 7, 43, 0),
        I(19, 1), I(33, 2, 1), I(22, 3, 32), I(23, 4, 3, 4), I(25, 5, 3, dim, 0, 0, 0, 2, 0), I(32, 6, 0, 5), I(59, 6, 7, 0),
        I(21, 13, 32, 1), I(23, 14, 13, 2), I(44, 14, 11, 15, 15), I(54, 1, 8, 0, 2), I(248, 9), I(61, 5, 10, 7));
    if (with_read) PUT(m, n, I(98, 4, 12, 10, 11));
    PUT(m, n, I0(253), I0(56));
    return n;
}

int main(void)
{
    uint32_t m[1024], c[1024 + NVMTL_FBFETCH_GROW];
    nvmtl_fbfetch f;
    size_t n = build(m, 192, 1, 6);
    /* scan: no change to the module */
    memcpy(c, m, n * 4); size_t nn = n;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024 + NVMTL_FBFETCH_GROW, 0, &f) == 0);
    CHECK(f.decl_mask == 1u && f.read_mask == 1u && f.reads == 1 && f.patched == 0 && nn == n && !memcmp(c, m, n * 4));
    /* zero: the read becomes OpCopyObject of a null constant declared before the function, the bound grows by one */
    nn = n;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024 + NVMTL_FBFETCH_GROW, 1, &f) == 0);
    CHECK(f.patched == 1 && nn == n + 3 && c[3] == 21);
    { int null_at = -1, copy_at = -1, fn_at = -1, read_left = 0;
      for (size_t i = 5; i < nn; ) { uint32_t wc = c[i] >> 16, op = c[i] & 0xffffu; if (!wc) break;
          if (op == 46) null_at = (int)i;
          if (op == 83) copy_at = (int)i;
          if (op == 54) fn_at = (int)i;
          if (op == 98) read_left = 1;
          i += wc; }
      CHECK(null_at > 0 && fn_at > null_at && copy_at > fn_at && !read_left);
      CHECK(null_at > 0 && c[null_at + 1] == 4 && c[null_at + 2] == 20);        /* null of the vec4 type, the new id */
      CHECK(copy_at > 0 && c[copy_at + 1] == 4 && c[copy_at + 2] == 12 && c[copy_at + 3] == 20);
      CHECK(c[copy_at + 4] == ((1u << 16) | 0u)); }                                /* the old 5th word is an OpNop */
    /* binding 193..199 are found too; 200 and 191 are not subpass inputs of this band */
    n = build(m, 195, 1, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 0, &f) == 0 && f.decl_mask == 1u << 3 && f.read_mask == 1u << 3);
    n = build(m, 199, 1, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 0, &f) == 0 && f.decl_mask == 1u << 7);
    n = build(m, 200, 1, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 0, &f) == 0 && f.decl_mask == 0 && f.reads == 0);
    n = build(m, 191, 1, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 0, &f) == 0 && f.decl_mask == 0 && f.reads == 0);
    /* declared but never read (constant folding removed it): declared, no read */
    n = build(m, 192, 0, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 1, &f) == 0 && f.decl_mask == 1u && f.read_mask == 0 && f.patched == 0 && nn == n);
    /* an ordinary 2D image at that binding is not a subpass input */
    n = build(m, 192, 1, 1); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 1, &f) == 0 && f.decl_mask == 0 && f.reads == 0 && nn == n);
    /* no room to grow: refused, not truncated */
    n = build(m, 192, 1, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, n, 1, &f) == -1);
    /* not SPIR-V, a truncated instruction, an id bound of 0 or over the cap */
    nn = n; memcpy(c, m, n * 4); c[0] = 0;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 0, &f) == -1);
    nn = n; memcpy(c, m, n * 4); c[5] = (0x7fffu << 16) | 17;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 0, &f) == -1);
    nn = n; memcpy(c, m, n * 4); c[3] = 0;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 0, &f) == -1);
    nn = n; memcpy(c, m, n * 4); c[3] = NVMTL_FBFETCH_MAX_BOUND + 1;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 0, &f) == -1);
    nn = n; memcpy(c, m, n * 4); c[3] = NVMTL_FBFETCH_MAX_BOUND;                /* the cap itself is fine (ids all below it) */
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 0, &f) == 0 && f.reads == 1);
    /* a read whose image id is >= the bound is ignored, not indexed */
    n = build(m, 192, 1, 6); nn = n; memcpy(c, m, n * 4);
    { size_t i = 5; while (i < nn && (c[i] & 0xffffu) != 98) i += c[i] >> 16;
      c[i + 3] = 0x7ffffff0u; c[3] = 20; }
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 1, &f) == 0 && f.reads == 0 && f.patched == 0 && f.decl_mask == 1u);
    /* the same with a load / pointer / variable id out of range */
    n = build(m, 192, 1, 6); nn = n; memcpy(c, m, n * 4);
    { size_t i = 5; while (i < nn && (c[i] & 0xffffu) != 61) i += c[i] >> 16;
      c[i + 2] = 0x7ffffff0u; c[i + 3] = 0x7ffffff1u; }
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024, 1, &f) == 0 && f.reads == 0);

    /* two reads of ONE result type share one null constant; reads of two types get two (valid module, checked with spirv-val) */
    n = build_valid(m);
    nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024 + NVMTL_FBFETCH_GROW, 0, &f) == 0);
    CHECK(f.decl_mask == 3u && f.read_mask == 3u && f.reads == 3 && f.patched == 0 && nn == n);
    nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024 + NVMTL_FBFETCH_GROW, 1, &f) == 0);
    CHECK(f.patched == 3 && nn == n + 6 && c[3] == m[3] + 2);                   /* two null constants, two new ids */
    { unsigned nulls = 0, copies = 0, reads = 0;
      for (size_t i = 5; i < nn; ) { uint32_t wc = c[i] >> 16, op = c[i] & 0xffffu; if (!wc) break;
          nulls += op == 46; copies += op == 83; reads += op == 98; i += wc; }
      CHECK(nulls == 2 && copies == 3 && reads == 0); }
    if (have_val()) {
        CHECK(validate(m, n) == 0);                                              /* the generator is sound */
        CHECK(validate(c, nn) == 0);                                             /* and so is the patched module */
    } else printf("test_spirv_fbfetch: spirv-val not found, the validation step is skipped\n");
    /* a read whose image operand is not a loaded subpass image is not counted: 2 of the 3 reads remain */
    nn = n; memcpy(c, m, n * 4);
    { size_t i = 5; while (i < nn && (c[i] & 0xffffu) != 98) i += c[i] >> 16; c[i + 3] = 1; }     /* image operand not a loaded subpass image */
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024 + NVMTL_FBFETCH_GROW, 1, &f) == 0 && f.reads == 2 && f.patched == 2);

    /* more than 8 distinct result types: refused (the buffer is the caller's copy) */
    { n = 0;
      PUT(m, n, 0x07230203u, 0x00010300u, 0, 200, 0, I(71, 7, 34, 0), I(71, 7, 33, 192), I(22, 3, 32), I(25, 5, 3, 6, 0, 0, 0, 2, 0), I(32, 6, 0, 5), I(59, 6, 7, 0));
      for (uint32_t t = 0; t < 9; t++) PUT(m, n, I(23, 20 + t, 3, 4));
      PUT(m, n, I(54, 1, 8, 0, 2), I(248, 9), I(61, 5, 10, 7));
      for (uint32_t t = 0; t < 9; t++) PUT(m, n, I(98, 20 + t, 100 + t, 10, 11));
      PUT(m, n, I0(253), I0(56));
      nn = n; memcpy(c, m, n * 4);
      CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024 + 3 * 16, 0, &f) == 0 && f.reads == 9);
      nn = n; memcpy(c, m, n * 4);
      CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024 + 3 * 16, 1, &f) == -1);
      /* exactly 8 types is fine */
      n = 0;
      PUT(m, n, 0x07230203u, 0x00010300u, 0, 200, 0, I(71, 7, 34, 0), I(71, 7, 33, 192), I(22, 3, 32), I(25, 5, 3, 6, 0, 0, 0, 2, 0), I(32, 6, 0, 5), I(59, 6, 7, 0));
      for (uint32_t t = 0; t < 8; t++) PUT(m, n, I(23, 20 + t, 3, 4));
      PUT(m, n, I(54, 1, 8, 0, 2), I(248, 9), I(61, 5, 10, 7));
      for (uint32_t t = 0; t < 8; t++) PUT(m, n, I(98, 20 + t, 100 + t, 10, 11));
      PUT(m, n, I0(253), I0(56));
      nn = n; memcpy(c, m, n * 4);
      CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024 + 3 * 16, 1, &f) == 0 && f.patched == 8 && nn == n + 24 && c[3] == 208); }
    /* nvmtl_spirv_fbfetch_mask: only the reads whose colour index is in the mask are zeroed (valid module: reads of 192 x2 and 193 x1) */
    n = build_valid(m); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch_mask(c, &nn, 1024 + NVMTL_FBFETCH_GROW, 2u, &f) == 0 && f.reads == 3 && f.patched == 1 && f.read_mask == 3u);
    { int reads_left = 0, copies = 0;
      for (size_t i = 5; i < nn; ) { uint32_t wc = c[i] >> 16, op = c[i] & 0xffffu; if (!wc) break; if (op == 98) reads_left++; if (op == 83) copies++; i += wc; }
      CHECK(reads_left == 2 && copies == 1);                                              /* the two reads of colour(0) stay, colour(1)'s became a copy */
      if (have_val()) CHECK(validate(c, nn) == 0); }
    n = build_valid(m); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch_mask(c, &nn, 1024 + NVMTL_FBFETCH_GROW, 0u, &f) == 0 && f.patched == 0 && nn == n && !memcmp(c, m, n * 4));
    n = build_valid(m); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch_mask(c, &nn, 1024 + NVMTL_FBFETCH_GROW, 1u, &f) == 0 && f.patched == 2);
    /* nvmtl_spirv_fbfetch_set: the variables of binding 192..199 move to the new set, nothing else does */
    n = build_valid(m); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch_set(c, nn, 3) == 2);
    { int moved = 0, other = 0;
      for (size_t i = 5; i < nn; ) { uint32_t wc = c[i] >> 16, op = c[i] & 0xffffu; if (!wc) break;
          if (op == 71 && c[i + 2] == 34) { if (c[i + 3] == 3 && (c[i + 1] == 7 || c[i + 1] == 14)) moved++; else other++; }
          i += wc; }
      CHECK(moved == 2 && other == 0); }
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 1024 + NVMTL_FBFETCH_GROW, 0, &f) == 0 && f.decl_mask == 3u && f.read_mask == 3u);   /* still seen after the move */
    n = build(m, 191, 1, 6); nn = n; memcpy(c, m, n * 4);                                                        /* binding 191: not ours, stays at set 0 */
    CHECK(nvmtl_spirv_fbfetch_set(c, nn, 3) == 0 && !memcmp(c, m, n * 4));
    n = build(m, 192, 1, 6); nn = n; memcpy(c, m, n * 4); c[0] = 0;
    CHECK(nvmtl_spirv_fbfetch_set(c, nn, 3) == -1);
    n = build(m, 192, 1, 6); nn = n; memcpy(c, m, n * 4); c[3] = NVMTL_FBFETCH_MAX_BOUND + 1;
    CHECK(nvmtl_spirv_fbfetch_set(c, nn, 3) == -1);
    n = build(m, 192, 1, 6); nn = n; memcpy(c, m, n * 4); c[5] = (0x7fffu << 16) | 17;
    CHECK(nvmtl_spirv_fbfetch_set(c, nn, 3) == -1);
    n = build(m, 194, 1, 6); nn = n; memcpy(c, m, n * 4);                                                        /* DescriptorSet precedes Binding in `build` */
    CHECK(nvmtl_spirv_fbfetch_set(c, nn, 3) == 1);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_spirv_fbfetch: ok\n");
    return 0;
}
