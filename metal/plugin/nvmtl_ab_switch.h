/*
 * nvmtl_ab_switch.h - the decisions behind the plugin's A/B switches for the glyph / tile artefacts (DRIVER-PLAN N49).
 *
 * Plain C, no Vulkan and no Objective-C, so tests/test_ab_switch.c checks them on the host exactly as nvmtl_vk.c and
 * NVMTLObjects.m use them:
 *   - which NVMTL_RADV_DEBUG / NVMTL_RADV_PERFTEST values reach RADV's environment (the value lands in the environment of
 *     WindowServer: lowercase letters, digits, '_' and ',' only, 200 characters at most) and how they join a value that is there;
 *   - the layout class a LOAD pass is counted under and whether it needs a barrier to make its declared initial layout true;
 *   - which managed upload ranges the snapshot covers.
 */
#ifndef NVMTL_AB_SWITCH_H
#define NVMTL_AB_SWITCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define NVMTL_RADV_FLAGS_MAX 200u
#define NVMTL_SNAP_MAX_BYTES (1u << 20)

/* 1: v is a list RADV_DEBUG / RADV_PERFTEST may be given: [a-z0-9_,]+, at most NVMTL_RADV_FLAGS_MAX characters */
static inline int nvmtl_radv_flag_ok(const char *v)
{
    if (!v || !*v || strlen(v) > NVMTL_RADV_FLAGS_MAX) return 0;
    for (const char *c = v; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == ',')) return 0;
    return 1;
}

/* out = old,add (or add alone when old is unset or empty); 0 when it fits in cap bytes, -1 otherwise (out is then empty) */
static inline int nvmtl_radv_flag_join(char *out, size_t cap, const char *old, const char *add)
{
    const int has = old && *old;
    const int n = snprintf(out, cap, "%s%s%s", has ? old : "", has ? "," : "", add ? add : "");
    if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return -1; }
    return 0;
}

/* The slot a LOAD pass is counted under, by the layout the image is tracked in: VkImageLayout 0..8 as they are (UNDEFINED, GENERAL,
 * COLOR_ATTACHMENT, DEPTH_STENCIL_ATTACHMENT, DEPTH_STENCIL_READ_ONLY, SHADER_READ_ONLY, TRANSFER_SRC, TRANSFER_DST, PREINITIALIZED), 9 = anything else */
static inline unsigned nvmtl_load_layout_slot(uint32_t layout) { return layout < 9u ? layout : 9u; }

/* 1: a LOAD pass whose image is tracked in `have` while the render pass declares `want` needs a barrier from one to the other to make
 * the declaration true (UNDEFINED included: the image was never written, which also initialises its compression metadata) */
static inline int nvmtl_load_needs_barrier(uint32_t have, uint32_t want, int enabled) { return enabled && have != want; }

/* 1: a managed upload range of n bytes is read from a private snapshot taken at the flush (0 < n <= 1 MiB, the knob on) */
static inline int nvmtl_snap_wanted(size_t n, int enabled) { return enabled && n && n <= NVMTL_SNAP_MAX_BYTES; }

#endif
