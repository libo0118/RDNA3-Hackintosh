/*
 * [[color(n)]] framebuffer fetch in a translated fragment module.
 *
 * The translator maps a Metal fragment input `[[color(n)]]` to a Vulkan input
 * attachment: a UniformConstant variable of an OpTypeImage with Dim SubpassData,
 * descriptor set 0, binding 192 + n (translator reflect/mod.rs
 * COLOR_INPUT_BINDING_BASE), read with OpImageRead. The plugin's descriptor set
 * layout has no binding 192..199, so a read that survives into the pipeline makes
 * RADV load an image descriptor from a binding it does not have (card run
 * 2026-10-25c: WindowServer and Terminal built such modules; random colours and
 * corrupt glyphs are the suspects).
 *
 * This file only looks at the module (scan) and, when asked, replaces each such
 * read by a null constant of the read's type, so the result is defined (all
 * zero) instead of whatever the descriptor memory holds. It does not make the
 * read work; that needs the colour attachment as a real input attachment of the
 * render pass (DRIVER-PLAN N10).
 *
 * Pure C99, no Vulkan headers: tests/test_spirv_fbfetch.c. SPDX-License-Identifier: MIT
 */
#ifndef NVMTL_SPIRV_FBFETCH_H
#define NVMTL_SPIRV_FBFETCH_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#define NVMTL_FBFETCH_BASE   192u
#define NVMTL_FBFETCH_COUNT  8u
/* room a patched module may grow by, in words: one OpConstantNull (3 words) per distinct result type */
#define NVMTL_FBFETCH_GROW   (3u * NVMTL_FBFETCH_COUNT)

typedef struct {
    uint32_t decl_mask;   /* bit n: a subpass-input variable at binding 192 + n is declared */
    uint32_t read_mask;   /* bit n: at least one OpImageRead of it is in the module */
    uint32_t reads;       /* OpImageRead instructions of subpass-input images */
    uint32_t patched;     /* of them replaced by zero (patch only) */
} nvmtl_fbfetch;

enum { NVMTL_OP_TYPE_IMAGE = 25, NVMTL_OP_TYPE_POINTER = 32, NVMTL_OP_VARIABLE = 59, NVMTL_OP_LOAD = 61, NVMTL_OP_DECORATE = 71,
       NVMTL_OP_IMAGE_READ = 98, NVMTL_OP_COPY_OBJECT = 83, NVMTL_OP_CONSTANT_NULL = 46, NVMTL_OP_FUNCTION = 54, NVMTL_OP_NOP = 0 };

/* Per id, one byte of facts about it (ids are bounded by the module's id bound, which is capped below) and one byte for the
 * binding band: 0 none, 1 + n = binding 192 + n. 2 bytes per id: a 1M-id module costs 2 MB. */
enum { NVMTL_FB_IMG_TYPE = 1, NVMTL_FB_PTR_TYPE = 2, NVMTL_FB_VAR = 4, NVMTL_FB_LOADED = 8 };
#define NVMTL_FBFETCH_MAX_BOUND (1u << 20)

/* One walk. When `zero` is set the reads found are rewritten in place (OpCopyObject of a null constant + OpNop padding)
 * and the module grows by the null constants: the caller's buffer holds cap words, *nw is the module's length and is
 * updated. Returns 0 (also when the module has no subpass input), -1 for a malformed module, an id bound over
 * NVMTL_FBFETCH_MAX_BOUND, out of memory, or one that does not fit; after a -1 with `zero` set the buffer may be half
 * rewritten: the caller discards it (it works on a copy). */
static inline int nvmtl_spirv_fbfetch(uint32_t *w, size_t *nw_io, size_t cap, int zero, nvmtl_fbfetch *out)
{
    size_t nw = *nw_io;
    memset(out, 0, sizeof *out);
    if (nw < 5 || w[0] != 0x07230203u) return -1;
    const uint32_t bound = w[3];
    if (!bound || bound > NVMTL_FBFETCH_MAX_BOUND) return -1;
    uint8_t *fact = (uint8_t *)calloc(bound, 1), *band = (uint8_t *)calloc(bound, 1);   /* band: of a decorated id, then of a variable, then of a loaded image */
    if (!fact || !band) { free(fact); free(band); return -1; }
    int rc = 0;
    size_t first_fn = 0;
    /* pass 1: decorations (they precede the types), image types with Dim SubpassData (6), pointers to them, their variables */
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) { rc = -1; break; }
        if (op == NVMTL_OP_FUNCTION && !first_fn) first_fn = i;
        if (op == NVMTL_OP_DECORATE && wc >= 4 && w[i + 1] < bound && w[i + 2] == 33 && w[i + 3] >= NVMTL_FBFETCH_BASE
            && w[i + 3] < NVMTL_FBFETCH_BASE + NVMTL_FBFETCH_COUNT) band[w[i + 1]] = (uint8_t)(1u + (w[i + 3] - NVMTL_FBFETCH_BASE));
        if (op == NVMTL_OP_TYPE_IMAGE && wc >= 9 && w[i + 1] < bound && w[i + 3] == 6) fact[w[i + 1]] |= NVMTL_FB_IMG_TYPE;
        if (op == NVMTL_OP_TYPE_POINTER && wc == 4 && w[i + 1] < bound && w[i + 2] == 0 && w[i + 3] < bound && (fact[w[i + 3]] & NVMTL_FB_IMG_TYPE))
            fact[w[i + 1]] |= NVMTL_FB_PTR_TYPE;
        if (op == NVMTL_OP_VARIABLE && wc >= 4 && w[i + 2] < bound && w[i + 1] < bound && (fact[w[i + 1]] & NVMTL_FB_PTR_TYPE)) {
            fact[w[i + 2]] |= NVMTL_FB_VAR;
            if (band[w[i + 2]]) out->decl_mask |= 1u << (band[w[i + 2]] - 1u);   /* outside 192..199: not ours */
        }
        i += wc;
    }
    if (!rc) {
        /* pass 2: loads of the in-band variables, then the reads */
        uint32_t nulls[NVMTL_FBFETCH_COUNT][2]; uint32_t nnull = 0;
        uint32_t next_id = bound;
        for (size_t i = 5; i < nw && !rc; ) {
            uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
            if (op == NVMTL_OP_LOAD && wc >= 4 && w[i + 2] < bound && w[i + 3] < bound && (fact[w[i + 3]] & NVMTL_FB_VAR) && band[w[i + 3]]) {
                fact[w[i + 2]] |= NVMTL_FB_LOADED; band[w[i + 2]] = band[w[i + 3]];
            } else if (op == NVMTL_OP_IMAGE_READ && wc >= 5 && w[i + 3] < bound && (fact[w[i + 3]] & NVMTL_FB_LOADED)) {
                const uint32_t n = (uint32_t)band[w[i + 3]] - 1u;
                out->reads++; out->read_mask |= 1u << n;
                if (zero) {
                    /* a null constant of the read's result type, declared once before the first function */
                    uint32_t rt = w[i + 1], res = w[i + 2], nid = 0;
                    for (uint32_t k = 0; k < nnull; k++) if (nulls[k][0] == rt) nid = nulls[k][1];
                    if (!nid) {
                        if (nnull == NVMTL_FBFETCH_COUNT) { rc = -1; break; }
                        nid = next_id++; nulls[nnull][0] = rt; nulls[nnull][1] = nid; nnull++;
                    }
                    w[i] = (4u << 16) | NVMTL_OP_COPY_OBJECT; w[i + 1] = rt; w[i + 2] = res; w[i + 3] = nid;
                    for (uint32_t k = 4; k < wc; k++) w[i + k] = (1u << 16) | NVMTL_OP_NOP;
                    out->patched++;
                }
            }
            i += wc;
        }
        if (!rc && zero && nnull) {
            const size_t add = 3u * nnull;
            if (!first_fn || nw + add > cap) rc = -1;
            else {
                memmove(w + first_fn + add, w + first_fn, (nw - first_fn) * sizeof *w);
                for (uint32_t k = 0; k < nnull; k++) {
                    w[first_fn + 3u * k] = (3u << 16) | NVMTL_OP_CONSTANT_NULL; w[first_fn + 3u * k + 1] = nulls[k][0]; w[first_fn + 3u * k + 2] = nulls[k][1];
                }
                nw += add; w[3] = next_id; *nw_io = nw;
            }
        }
    }
    free(fact); free(band);
    return rc;
}
#endif
