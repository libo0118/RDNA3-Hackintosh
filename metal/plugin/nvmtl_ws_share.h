/*
 * nvmtl_ws_share.h - the CONSUMER side of N71 (c): WindowServer samples an application's VRAM drawable directly, through a
 * READ-ONLY import, instead of uploading its CPU copy (DRIVER-PLAN N71, risks R1-R4).
 *
 * Plain C99 (no Objective-C, no Vulkan): tests/test_ws_share.c checks every decision exactly as NVMTLDevice.m,
 * NVMTLObjects.m and nvmtl_vk.c use it. The rules:
 *
 *  - Eligibility (nvmtl_wss_eligible): only WindowServer, only when nothing switched it off (NVMTL_WS_SHARE=0, the kernel's
 *    debug.rdna3accel_a3share not 1 - live, the session stop after 4 hazards, a surface demoted for its life), and only a texture that can never be written: usage ShaderRead
 *    (+ PixelFormatView) alone - Unknown (0) may be written, RenderTarget and ShaderWrite are writes, a write-combined CPU
 *    cache mode is a CPU writer. Anything else takes the private path.
 *  - The result of the kernel's import is a VkImage with SAMPLED | TRANSFER_SRC usage only, over a buffer with
 *    TRANSFER_SRC only: never mapped, rendered into, stored to, copied into or uploaded into (the kernel maps the BO
 *    without WRITEABLE too, so a GPU write would fault; the plugin's own paths refuse before that).
 *  - At wrapper creation (nvmtl_wss_plan), from three facts read without importing: the producer's allocation generation
 *    (an IOSurface value the producer sets when the kernel recorded its share, 0 = never), whether the producer's tag is
 *    current (nvmtl_vram_tag_current), and the cached read-only backing of the (surface, plane) with its generation:
 *      tag stale          -> the private path (the CPU copy is the newer one; nothing is ever uploaded into the alias)
 *      producer gen 0     -> the private path as today (nothing shared)
 *      same generation    -> the cached backing (no import)
 *      other generation   -> a new import (sh_reimp)
 *      no backing         -> an import (sh_imp)
 *  - At a sample (nvmtl_wss_sample): tag current = skip the upload (sh_skip); stale = no upload either (sh_stale), the surface
 *    is demoted so the next wrapper is private.
 *  - R1, read-after-write: the tag generation recorded at the composite's commit and compared at its completion
 *    (nvmtl_wss_hazard): a move means the producer finished a NEXT frame into the drawable while WindowServer's samples were
 *    in flight (sh_war); that surface is demoted for its life; NVMTL_WSS_HAZARD_MAX hazards stop shared imports for the session.
 *  - R4, any import error: the private path, the surface demoted (so it is asked once), counted per errno.
 */
#ifndef NVMTL_WS_SHARE_H
#define NVMTL_WS_SHARE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

/* MTLTextureUsage bits */
#define NVMTL_WSS_USAGE_READ   0x1u
#define NVMTL_WSS_USAGE_WRITE  0x2u
#define NVMTL_WSS_USAGE_RT     0x4u
#define NVMTL_WSS_USAGE_VIEW   0x10u

enum nvmtl_wss_why {
    NVMTL_WSS_OK = 0,
    NVMTL_WSS_NOT_WS,        /* not WindowServer */
    NVMTL_WSS_KILL,          /* NVMTL_WS_SHARE=0 / NVMTL_NO_SURFACE_SHARE=1 */
    NVMTL_WSS_SWITCH_OFF,    /* debug.rdna3accel_a3share is 0 or absent (live: the A/B back to the private path) */
    NVMTL_WSS_SESSION_OFF,   /* 4 hazards: no shared imports for the rest of the session */
    NVMTL_WSS_DEMOTED,       /* this surface had a hazard / an import error / a stale sample */
    NVMTL_WSS_USAGE_UNKNOWN, /* usage 0: Metal lets such a texture be used any way */
    NVMTL_WSS_RENDER_TARGET,
    NVMTL_WSS_SHADER_WRITE,
    NVMTL_WSS_CPU_ACCESS,    /* write-combined CPU cache mode: a CPU writer */
    NVMTL_WSS_USAGE_OTHER,   /* a usage bit this code does not know */
    NVMTL_WSS_N
};
static inline const char *nvmtl_wss_why_name(int why)
{
    static const char *const n[NVMTL_WSS_N] = { "eligible", "not WindowServer", "NVMTL_WS_SHARE=0", "debug.rdna3accel_a3share not 1", "stopped for the session (hazards)",
        "surface demoted", "usage Unknown", "RenderTarget usage", "ShaderWrite usage", "CPU write access", "unknown usage bits" };
    return why >= 0 && why < NVMTL_WSS_N ? n[why] : "?";
}

/* may this texture be a read-only alias of an application's shared VRAM? cpu_write: a write-combined CPU cache mode */
static inline int nvmtl_wss_eligible(int is_ws, int kill, int switch_on, int session_off, int demoted, uint32_t usage, int cpu_write)
{
    if (!is_ws) return NVMTL_WSS_NOT_WS;
    if (kill) return NVMTL_WSS_KILL;
    if (!switch_on) return NVMTL_WSS_SWITCH_OFF;
    if (session_off) return NVMTL_WSS_SESSION_OFF;
    if (demoted) return NVMTL_WSS_DEMOTED;
    if (!usage) return NVMTL_WSS_USAGE_UNKNOWN;
    if (usage & NVMTL_WSS_USAGE_RT) return NVMTL_WSS_RENDER_TARGET;
    if (usage & NVMTL_WSS_USAGE_WRITE) return NVMTL_WSS_SHADER_WRITE;
    if (usage & ~(NVMTL_WSS_USAGE_READ | NVMTL_WSS_USAGE_VIEW)) return NVMTL_WSS_USAGE_OTHER;
    if (cpu_write) return NVMTL_WSS_CPU_ACCESS;
    return NVMTL_WSS_OK;
}

/* ---- counters (`vk: ws frame: SUMMARY` sh_*) ---- */
typedef struct {
    uint64_t imp, skip, stale, reimp, refused, war, demoted;
    uint64_t wr;                                               /* a write recorded against a read-only alias (refused or detected) */
    uint64_t inelig;                                           /* wrappers of a shared surface that stayed private: a usage that can write, a CPU writer */
    uint64_t e_perm, e_nospc, e_noent, e_nodev, e_nxio, e_other;   /* refused imports by errno */
} nvmtl_wss_stats;
#define NVMTL_WSS_INC(s, f) __atomic_fetch_add(&(s)->f, 1, __ATOMIC_RELAXED)

/* the slot of an import errno; returns the name too */
static inline void nvmtl_wss_count_err(nvmtl_wss_stats *s, int err)
{
    NVMTL_WSS_INC(s, refused);
    switch (err) {
    case EPERM:  NVMTL_WSS_INC(s, e_perm); break;
    case ENOSPC: NVMTL_WSS_INC(s, e_nospc); break;
    case ENOENT: NVMTL_WSS_INC(s, e_noent); break;
    case ENODEV: NVMTL_WSS_INC(s, e_nodev); break;
    case ENXIO:  NVMTL_WSS_INC(s, e_nxio); break;
    default:     NVMTL_WSS_INC(s, e_other); break;
    }
}

/* ---- the demotion state (the caller serialises) ---- */
#define NVMTL_WSS_DEMOTE_MAX 128u   /* surfaces remembered; the oldest is forgotten when full (it then gets one more chance) */
#define NVMTL_WSS_HAZARD_MAX 4u
typedef struct {
    uint32_t ids[NVMTL_WSS_DEMOTE_MAX];
    unsigned n, next;
    unsigned hazards;
    int session_off;
} nvmtl_wss_state;

static inline int nvmtl_wss_demoted(const nvmtl_wss_state *st, uint32_t sid)
{
    for (unsigned i = 0; i < st->n; i++) if (st->ids[i] == sid) return 1;
    return 0;
}
/* 1 when sid was not demoted before */
static inline int nvmtl_wss_demote(nvmtl_wss_state *st, nvmtl_wss_stats *s, uint32_t sid)
{
    if (!sid || nvmtl_wss_demoted(st, sid)) return 0;
    if (st->n < NVMTL_WSS_DEMOTE_MAX) st->ids[st->n++] = sid;
    else { st->ids[st->next] = sid; st->next = (st->next + 1) % NVMTL_WSS_DEMOTE_MAX; }
    NVMTL_WSS_INC(s, demoted);
    return 1;
}

/* R1: the tag generation at the composite's commit (g0) and at its completion (g1). Returns a bit set:
 * 1 a hazard (g0 != g1; counted sh_war), 2 the surface was newly demoted, 4 this hazard was the one that stops the session. */
#define NVMTL_WSS_H_HAZARD 1
#define NVMTL_WSS_H_DEMOTED 2
#define NVMTL_WSS_H_SESSION 4
static inline int nvmtl_wss_hazard(nvmtl_wss_state *st, nvmtl_wss_stats *s, uint32_t sid, uint32_t g0, uint32_t g1)
{
    int r = 0;
    if (g0 == g1) return 0;
    r |= NVMTL_WSS_H_HAZARD;
    NVMTL_WSS_INC(s, war);
    st->hazards++;
    if (nvmtl_wss_demote(st, s, sid)) r |= NVMTL_WSS_H_DEMOTED;
    if (st->hazards >= NVMTL_WSS_HAZARD_MAX && !st->session_off) { st->session_off = 1; r |= NVMTL_WSS_H_SESSION; }
    return r;
}

/* ---- the plan at wrapper creation ---- */
enum { NVMTL_WSS_PLAN_PRIVATE = 0, NVMTL_WSS_PLAN_CACHED, NVMTL_WSS_PLAN_IMPORT, NVMTL_WSS_PLAN_REIMPORT };
enum { NVMTL_WSS_PW_NONE = 0, NVMTL_WSS_PW_NOSHARE, NVMTL_WSS_PW_STALE };   /* why PRIVATE */
static inline int nvmtl_wss_plan(int have_cached, uint64_t cached_gen, uint64_t producer_gen, int tag_current, int *why)
{
    int w = NVMTL_WSS_PW_NONE, p;
    if (!producer_gen) { w = NVMTL_WSS_PW_NOSHARE; p = NVMTL_WSS_PLAN_PRIVATE; }
    else if (!tag_current) { w = NVMTL_WSS_PW_STALE; p = NVMTL_WSS_PLAN_PRIVATE; }
    else if (have_cached && cached_gen == producer_gen) p = NVMTL_WSS_PLAN_CACHED;
    else if (have_cached) p = NVMTL_WSS_PLAN_REIMPORT;
    else p = NVMTL_WSS_PLAN_IMPORT;
    if (why) *why = w;
    return p;
}

/* ---- a sample ---- */
enum { NVMTL_WSS_SAMPLE_SKIP = 0, NVMTL_WSS_SAMPLE_STALE };
static inline int nvmtl_wss_sample(int tag_current) { return tag_current ? NVMTL_WSS_SAMPLE_SKIP : NVMTL_WSS_SAMPLE_STALE; }

/* ---- what the kernel gave (drm_xnu_surface_last_import) ---- */
enum { NVMTL_WSS_IMP_ERR = 0, NVMTL_WSS_IMP_RO, NVMTL_WSS_IMP_RO_UNUSABLE, NVMTL_WSS_IMP_RW };
/* import_err: the errno of the import (0 worked); ro: DRM_XNU_SURFACE_SHARED_RO; usable: the texture is eligible and its
 * alias could be built read-only. A writable answer (the kernel's own policy for WindowServer's surfaces) is no share. */
static inline int nvmtl_wss_import_class(int import_err, int ro, int usable)
{
    if (import_err) return NVMTL_WSS_IMP_ERR;
    if (!ro) return NVMTL_WSS_IMP_RW;
    return usable ? NVMTL_WSS_IMP_RO : NVMTL_WSS_IMP_RO_UNUSABLE;
}

/* ---- the producer's allocation generation, as an IOSurface value ("NVMTLShareGen:p<plane>", 12 bytes) ---- */
#define NVMTL_WSS_GEN_MAGIC 0x31474853u   /* "SHG1" */
typedef struct { uint32_t magic, lo, hi; } nvmtl_wss_gen_rec;
static inline nvmtl_wss_gen_rec nvmtl_wss_gen_pack(uint64_t gen)
{
    nvmtl_wss_gen_rec r = { NVMTL_WSS_GEN_MAGIC, (uint32_t)gen, (uint32_t)(gen >> 32) };
    return r;
}
/* 0 for a missing or foreign record */
static inline uint64_t nvmtl_wss_gen_unpack(const void *p, size_t n)
{
    nvmtl_wss_gen_rec r;
    if (!p || n != sizeof r) return 0;
    memcpy(&r, p, sizeof r);
    return r.magic == NVMTL_WSS_GEN_MAGIC ? ((uint64_t)r.hi << 32 | r.lo) : 0;
}

/* ---- the SUMMARY tail ---- */
static inline uint64_t nvmtl_wss_events(const nvmtl_wss_stats *s)
{
    return __atomic_load_n(&s->imp, __ATOMIC_RELAXED) + __atomic_load_n(&s->skip, __ATOMIC_RELAXED) + __atomic_load_n(&s->stale, __ATOMIC_RELAXED) +
           __atomic_load_n(&s->reimp, __ATOMIC_RELAXED) + __atomic_load_n(&s->refused, __ATOMIC_RELAXED) + __atomic_load_n(&s->war, __ATOMIC_RELAXED) +
           __atomic_load_n(&s->demoted, __ATOMIC_RELAXED) + __atomic_load_n(&s->wr, __ATOMIC_RELAXED) + __atomic_load_n(&s->inelig, __ATOMIC_RELAXED);
}
/* the key=value tail of `vk: ws frame: SUMMARY` for a process that runs the consumer (WindowServer): printed even when all are 0,
 * so the triage can tell "the consumer path ran and imported nothing" from "an older plugin". session_off: sh_off. */
static inline int nvmtl_wss_format(const nvmtl_wss_stats *s, int session_off, char *out, size_t cap)
{
#define W_(x) ((unsigned long long)__atomic_load_n(&s->x, __ATOMIC_RELAXED))
    if (!cap) return 0;
    snprintf(out, cap, "sh_imp=%llu sh_skip=%llu sh_stale=%llu sh_reimp=%llu sh_refused=%llu sh_war=%llu sh_demoted=%llu sh_wr=%llu "
                       "sh_inelig=%llu sh_eperm=%llu sh_enospc=%llu sh_enoent=%llu sh_enodev=%llu sh_enxio=%llu sh_eother=%llu sh_off=%d",
             W_(imp), W_(skip), W_(stale), W_(reimp), W_(refused), W_(war), W_(demoted), W_(wr), W_(inelig),
             W_(e_perm), W_(e_nospc), W_(e_noent), W_(e_nodev), W_(e_nxio), W_(e_other), session_off ? 1 : 0);
#undef W_
    return 1;
}

#endif
