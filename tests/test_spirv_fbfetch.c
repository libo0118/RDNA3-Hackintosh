/* [[color(n)]] framebuffer fetch in a translated fragment module (metal/plugin/nvmtl_spirv_fbfetch.h):
 * the scan finds the subpass-input reads at binding 192 + n, the zeroing makes them defined.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "nvmtl_spirv_fbfetch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define I(op, ...) (((uint32_t)(sizeof((uint32_t[]){__VA_ARGS__}) / 4 + 1) << 16) | (op)), __VA_ARGS__
/* ids: 1 void, 2 fn type, 3 float, 4 vec4, 5 image(SubpassData), 6 ptr, 7 var(binding 192), 8 main, 9 label, 10 loaded, 11 coord, 12 result,
 *      13 int, 14 ivec2, 15 const0 int, 16 const vec2 composite */
static size_t build(uint32_t *m, uint32_t binding, int with_read, uint32_t dim)
{
    size_t n = 0;
    uint32_t hdr[5] = { 0x07230203u, 0x00010300u, 0, 20, 0 };
    memcpy(m, hdr, sizeof hdr); n = 5;
    uint32_t body[] = {
        I(17, 40),                                  /* OpCapability InputAttachment */
        I(71, 7, 34, 0),                            /* DescriptorSet 0 */
        I(71, 7, 33, binding),                      /* Binding */
        I(71, 7, 43, 0),                            /* InputAttachmentIndex 0 */
        I(19, 1),                                   /* OpTypeVoid */
        I(33, 2, 1),                                /* OpTypeFunction void */
        I(22, 3, 32),                               /* OpTypeFloat 32 */
        I(23, 4, 3, 4),                             /* OpTypeVector 4 */
        I(25, 5, 3, dim, 0, 0, 0, 2, 0),            /* OpTypeImage float Dim 0 0 0 2 Unknown */
        I(32, 6, 0, 5),                             /* OpTypePointer UniformConstant */
        I(59, 6, 7, 0),                             /* OpVariable UniformConstant */
        I(21, 13, 32, 1),                           /* OpTypeInt 32 1 */
        I(23, 14, 13, 2),                           /* OpTypeVector int 2 */
        I(44, 14, 11, 15, 15),                      /* (stand in for a coordinate) */
        I(54, 1, 8, 0, 2),                          /* OpFunction */
        I(248, 9),                                  /* OpLabel */
        I(61, 5, 10, 7),                            /* OpLoad the image */
    };
    memcpy(m + n, body, sizeof body); n += sizeof body / 4;
    if (with_read) { uint32_t rd[] = { I(98, 4, 12, 10, 11) }; memcpy(m + n, rd, sizeof rd); n += sizeof rd / 4; }
    { uint32_t tail[] = { (1u << 16) | 253u, (1u << 16) | 56u }; memcpy(m + n, tail, sizeof tail); n += sizeof tail / 4; }
    return n;
}

int main(void)
{
    uint32_t m[256], c[256 + NVMTL_FBFETCH_GROW];
    nvmtl_fbfetch f;
    size_t n = build(m, 192, 1, 6);
    /* scan: no change to the module */
    memcpy(c, m, n * 4); size_t nn = n;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 256 + NVMTL_FBFETCH_GROW, 0, &f) == 0);
    CHECK(f.decl_mask == 1u && f.read_mask == 1u && f.reads == 1 && f.patched == 0 && nn == n && !memcmp(c, m, n * 4));
    /* zero: the read becomes OpCopyObject of a null constant declared before the function, the bound grows by one */
    nn = n;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 256 + NVMTL_FBFETCH_GROW, 1, &f) == 0);
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
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 256, 0, &f) == 0 && f.decl_mask == 1u << 3 && f.read_mask == 1u << 3);
    n = build(m, 200, 1, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 256, 0, &f) == 0 && f.decl_mask == 0 && f.reads == 0);
    n = build(m, 191, 1, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 256, 0, &f) == 0 && f.decl_mask == 0 && f.reads == 0);
    /* declared but never read (constant folding removed it): declared, no read */
    n = build(m, 192, 0, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 256, 1, &f) == 0 && f.decl_mask == 1u && f.read_mask == 0 && f.patched == 0 && nn == n);
    /* an ordinary 2D image at that binding is not a subpass input */
    n = build(m, 192, 1, 1); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 256, 1, &f) == 0 && f.decl_mask == 0 && f.reads == 0 && nn == n);
    /* no room to grow: refused, not truncated */
    n = build(m, 192, 1, 6); nn = n; memcpy(c, m, n * 4);
    CHECK(nvmtl_spirv_fbfetch(c, &nn, n, 1, &f) == -1);
    /* not SPIR-V, a truncated instruction */
    nn = n; memcpy(c, m, n * 4); c[0] = 0;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 256, 0, &f) == -1);
    nn = n; memcpy(c, m, n * 4); c[5] = (0x7fffu << 16) | 17;
    CHECK(nvmtl_spirv_fbfetch(c, &nn, 256, 0, &f) == -1);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("test_spirv_fbfetch: ok\n");
    return 0;
}
