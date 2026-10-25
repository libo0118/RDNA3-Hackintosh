/*
 * Metal framebuffer fetch ([[color(n)]]) on Vulkan: the decisions, apart from the Vulkan calls (pure C99, tests/test_fbfetch_plan.c).
 *
 * Mechanism (nvmtl_vk.c, probed on a real RADV by tests/vk_fbfetch_probe.c): a pipeline whose fragment module READS a
 * subpass input at binding 192 + n makes the render pass it is drawn in declare colour attachment n as input attachment n of the
 * same subpass (both references in layout GENERAL, plus a by-region self-dependency). The descriptor (INPUT_ATTACHMENT, a view of the
 * current colour attachment) is in descriptor set NVMTL_FBF_SET, written once per pass, and a barrier
 * (COLOR_ATTACHMENT_OUTPUT/WRITE -> FRAGMENT_SHADER/INPUT_ATTACHMENT_READ, by region) is recorded before every draw that reads it.
 * Anything this file refuses is turned into a zero vector in the shader instead (a defined value, never an undefined descriptor),
 * counted and logged with the reason below.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NVMTL_FBFETCH_PLAN_H
#define NVMTL_FBFETCH_PLAN_H
#include <stdint.h>
#include <stddef.h>

#define NVMTL_FBF_SET     3u    /* descriptor set of the input attachments; 0 vertex, 1 fragment, 2 bindless heap (or an empty layout) */
#define NVMTL_FBF_NCOL    8u    /* [[color(0..7)]] = binding 192..199 */
#define NVMTL_FBF_KEY_SHIFT 16u /* render-pass variant key = load bits | fbin << 16 (load bits stay below 1 << 10) */

/* why a read, a pipeline or an attachment cannot be served; 0 = served */
enum {
    NVMTL_FBF_OK = 0,
    NVMTL_FBF_NO_ATTACHMENT,   /* the pipeline has no colour attachment n (or n >= 8) */
    NVMTL_FBF_MULTISAMPLE,     /* per-sample fetch of a multisample target is not carried (the module's image type is not MS) */
    NVMTL_FBF_INTEGER_FORMAT,  /* integer attachment: the translated module's sampled type is not known to match */
    NVMTL_FBF_NO_LAYOUT,       /* the input-attachment descriptor layout / pool could not be made */
    NVMTL_FBF_LAYERED,         /* a layered pass: the module reads a non-arrayed image */
    NVMTL_FBF_VOLUME,          /* a slice of a 3D texture */
    NVMTL_FBF_NO_USAGE,        /* the image was not created with INPUT_ATTACHMENT usage */
    NVMTL_FBF_NO_VIEW,         /* vkCreateImageView of the input view failed */
    NVMTL_FBF_FORCED_ZERO,     /* NVMTL_FBFETCH_ZERO=1 */
    NVMTL_FBF_TESS,            /* a tessellation pipeline draws through a path the barrier does not cover */
    NVMTL_FBF_REASONS
};

static inline const char *nvmtl_fbf_reason_text(unsigned r)
{
    static const char *const t[NVMTL_FBF_REASONS] = {
        "served", "the pipeline has no colour attachment at that index (normal: the translator reads every colour input it declares)", "a multisample target (per-sample fetch is not carried)",
        "an integer colour format", "no input-attachment descriptor layout", "a layered render pass", "a slice of a 3D texture",
        "the image has no INPUT_ATTACHMENT usage", "the input view could not be created", "NVMTL_FBFETCH_ZERO=1", "a tessellation pipeline" };
    return r < NVMTL_FBF_REASONS ? t[r] : "?";
}

typedef struct {
    uint32_t read_mask;        /* bit n: the module reads [[color(n)]] */
    uint32_t ncol;             /* colour slots of the pipeline */
    uint32_t cfmt_present;     /* bit n: slot n has a format */
    uint32_t cfmt_int;         /* bit n: slot n's format is an integer one */
    uint32_t samples;          /* 0 or 1: single sample */
    uint32_t layout_ok;        /* the descriptor layout exists */
    uint32_t forced_zero;      /* NVMTL_FBFETCH_ZERO */
    uint32_t tess;
} nvmtl_fbf_in;

/* Per read: a read of colour attachment n is served when the pipeline has a floating-point attachment n and nothing global refuses;
 * every other read is zeroed (the module is patched, a defined value). A module the translator wrote declares AND reads all the colour
 * inputs of the function it came from, used or not (card run 2026-10-25d: Terminal, 0x7 declared, one colour attachment), so reads of
 * slots the pipeline does not have are normal and are simply zeroed while the real one is served.
 * *serve: mask of served reads (out->fbread), *zero: mask of reads to zero, *why: the first reason a read is zeroed (NVMTL_FBF_OK if none). */
static inline void nvmtl_fbf_plan(const nvmtl_fbf_in *in, uint32_t *serve, uint32_t *zero, unsigned *why)
{
    unsigned r = NVMTL_FBF_OK;
    uint32_t sv = 0, zr = 0;
    const uint32_t reads = in->read_mask & 0xffu;
    unsigned g = NVMTL_FBF_OK;   /* a global reason: no read is served */
    if (in->forced_zero) g = NVMTL_FBF_FORCED_ZERO;
    else if (!in->layout_ok) g = NVMTL_FBF_NO_LAYOUT;
    else if (in->samples > 1) g = NVMTL_FBF_MULTISAMPLE;
    else if (in->tess) g = NVMTL_FBF_TESS;
    for (uint32_t n = 0; n < NVMTL_FBF_NCOL; n++) {
        if (!(reads & (1u << n))) continue;
        unsigned rr = g;
        if (!rr && (n >= in->ncol || !(in->cfmt_present & (1u << n)))) rr = NVMTL_FBF_NO_ATTACHMENT;
        else if (!rr && (in->cfmt_int & (1u << n))) rr = NVMTL_FBF_INTEGER_FORMAT;
        if (rr) { zr |= 1u << n; if (r == NVMTL_FBF_OK) r = rr; } else sv |= 1u << n;
    }
    if (serve) *serve = sv;
    if (zero) *zero = zr;
    if (why) *why = r;
}

/* Can this attachment of the pass in progress be an input attachment? (the pipeline said yes, the pass has the facts) */
static inline unsigned nvmtl_fbf_attachment_reason(uint32_t samples, uint32_t layers, uint32_t mtl_type, int has_input_usage)
{
    if (samples > 1) return NVMTL_FBF_MULTISAMPLE;
    if (layers > 1) return NVMTL_FBF_LAYERED;
    if (mtl_type == 7) return NVMTL_FBF_VOLUME;
    if (!has_input_usage) return NVMTL_FBF_NO_USAGE;
    return NVMTL_FBF_OK;
}

/* Render-pass variant key: the load bits and the input-attachment mask. A pass with no input attachment keeps the plain key. */
static inline uint32_t nvmtl_fbf_rp_key(uint32_t load, uint32_t fbin) { return (load & ((1u << NVMTL_FBF_KEY_SHIFT) - 1u)) | (fbin << NVMTL_FBF_KEY_SHIFT); }
static inline uint32_t nvmtl_fbf_key_load(uint32_t key) { return key & ((1u << NVMTL_FBF_KEY_SHIFT) - 1u); }
static inline uint32_t nvmtl_fbf_key_fbin(uint32_t key) { return key >> NVMTL_FBF_KEY_SHIFT; }

/* The subpass's pInputAttachments: element n is colour attachment n's index when bit n of fbin is set and that slot exists, else `unused`.
 * Returns the array length (highest used element + 1, 0 when there is none). att[i] is the render-pass attachment index of colour slot i. */
static inline uint32_t nvmtl_fbf_input_refs(uint32_t fbin, uint32_t ncol, const uint32_t *att, uint32_t unused, uint32_t *out)
{
    uint32_t n = 0;
    if (ncol > NVMTL_FBF_NCOL) ncol = NVMTL_FBF_NCOL;
    for (uint32_t i = 0; i < ncol; i++) {
        const int used = (fbin & (1u << i)) && att[i] != unused;
        out[i] = used ? att[i] : unused;
        if (used) n = i + 1;
    }
    return n;
}

/* The input attachments a pass needs that the pass in progress lacks: the encoder restarts the pass (LOAD) with the union. */
static inline uint32_t nvmtl_fbf_missing(uint32_t pass_fbin, uint32_t pipeline_fbread) { return pipeline_fbread & ~pass_fbin; }

/* The attachments a pass declares as inputs: those the pass already had, those a pipeline reads, those a caller asked for, only existing slots. */
static inline uint32_t nvmtl_fbf_pass_fbin(uint32_t requested, uint32_t pipeline_fbread, uint32_t slots_present, uint32_t samples)
{
    return samples > 1 ? 0u : ((requested | pipeline_fbread) & slots_present & ((1u << NVMTL_FBF_NCOL) - 1u));
}
#endif
