/*
 * nvmtl_ws_share.h - the CONSUMER side of N71 (c): WindowServer samples an application's VRAM drawable directly, through a
 * READ-ONLY import, instead of uploading its CPU copy (DRIVER-PLAN N71, risks R1-R4).
 *
 * Plain C99 (no Objective-C, no Vulkan): tests/test_ws_share.c checks every decision exactly as NVMTLDevice.m,
 * NVMTLObjects.m and nvmtl_vk.c use it. The rules:
 *
 *  - Eligibility (nvmtl_wss_eligible): only WindowServer, only when nothing switched it off (NVMTL_WS_SHARE=0, the kernel's
 *    debug.rdna3accel_a3share not 1 - live, the session backstop after 64 hazards of all producers, a surface demoted for its life; a producer demoted after 4 of its own), and only a texture that can never be written: usage ShaderRead
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
 *  - At a sample (nvmtl_wss_sample): tag current = skip the upload (sh_skip); stale = no upload either (sh_stale). The sample
 *    does not demote: the next wrapper's plan sees the stale tag itself and is private while it stays stale.
 *  - R1, read-after-write: the producer's tag (seed, generation) recorded at the composite's commit and compared at its
 *    completion (nvmtl_wss_hazard; the CPU copy-out fallback moves the seed without the generation): a move means the producer finished a NEXT frame into the drawable while WindowServer's samples were
 *    in flight (sh_war); that surface is demoted for its life. Hazards are counted per PRODUCER (the pid the kernel names
 *    with the read-only import): NVMTL_WSS_PID_HAZARD_MAX of one producer demote that producer (every surface it shares
 *    takes the private path, sh_pdem), so one application cannot turn sharing off for the others (ECC security review);
 *    NVMTL_WSS_HAZARD_MAX hazards of all producers together stop shared imports for the session (a backstop).
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
    NVMTL_WSS_DEMOTED,       /* this surface had a hazard / an import error / a write asked of its alias */
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
    uint64_t rw;                                               /* a WRITABLE import where a read-only one was planned (the record is gone, a3ws 1): given back, never used */
    uint64_t inelig;                                           /* wrappers of a shared surface that stayed private: a usage that can write, a CPU writer */
    uint64_t e_perm, e_nospc, e_noent, e_nodev, e_nxio, e_other;   /* refused imports by errno */
    uint64_t pdem;                                             /* producers demoted (their hazards) */
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
#define NVMTL_WSS_HAZARD_MAX 64u      /* all producers together: the session backstop */
#define NVMTL_WSS_PID_HAZARD_MAX 4u   /* one producer: that producer is demoted */
#define NVMTL_WSS_PIDS 64u            /* producers remembered; a full table forgets a not-demoted one first */
typedef struct { int32_t pid; unsigned hazards, logs; int demoted; } nvmtl_wss_pid;
typedef struct {
    uint32_t ids[NVMTL_WSS_DEMOTE_MAX];
    unsigned n, next;
    unsigned hazards;
    int session_off;
    nvmtl_wss_pid pids[NVMTL_WSS_PIDS];
    unsigned npid, pnext;
} nvmtl_wss_state;

/* the producer's entry (pid > 0; NULL for 0, an older kernel that names none); create: make one when missing */
static inline nvmtl_wss_pid *nvmtl_wss_pid_slot(nvmtl_wss_state *st, int32_t pid, int create)
{
    unsigned i, k;
    if (pid <= 0) return NULL;
    for (i = 0; i < st->npid; i++) if (st->pids[i].pid == pid) return &st->pids[i];
    if (!create) return NULL;
    if (st->npid < NVMTL_WSS_PIDS) k = st->npid++;
    else {   /* full: the next not-demoted entry in turn, a demoted one only when every entry is */
        k = st->pnext % NVMTL_WSS_PIDS;
        for (i = 0; i < NVMTL_WSS_PIDS && st->pids[k].demoted; i++) k = (k + 1) % NVMTL_WSS_PIDS;
        st->pnext = (k + 1) % NVMTL_WSS_PIDS;
    }
    st->pids[k].pid = pid; st->pids[k].hazards = 0; st->pids[k].logs = 0; st->pids[k].demoted = 0;
    return &st->pids[k];
}
static inline int nvmtl_wss_producer_demoted(nvmtl_wss_state *st, int32_t pid)
{
    const nvmtl_wss_pid *p = nvmtl_wss_pid_slot(st, pid, 0);
    return p && p->demoted;
}
/* a log line about this producer is due: its first 4, then every 64th (pid 0: always) */
static inline int nvmtl_wss_pid_log_due(nvmtl_wss_state *st, int32_t pid)
{
    nvmtl_wss_pid *p = nvmtl_wss_pid_slot(st, pid, 1);
    if (!p) return 1;
    return p->logs++ < 4u || p->logs % 64u == 0;
}

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

/* R1: the tag generation at the composite's commit (g0) and at its completion (g1); pid: the producer the kernel named with the
 * import (0 unknown). Returns a bit set: 1 a hazard (g0 != g1: nvmtl_wss_tag_key of the tag's seed and generation; counted sh_war),
 * 2 the surface was newly demoted, 4 this hazard was the one that stops the session (the backstop), 8 it demoted its producer. */
#define NVMTL_WSS_H_HAZARD 1
#define NVMTL_WSS_H_DEMOTED 2
#define NVMTL_WSS_H_SESSION 4
#define NVMTL_WSS_H_PRODUCER 8
static inline uint64_t nvmtl_wss_tag_key(uint32_t seed, uint32_t gen) { return (uint64_t)seed << 32 | gen; }
static inline int nvmtl_wss_hazard(nvmtl_wss_state *st, nvmtl_wss_stats *s, uint32_t sid, int32_t pid, uint64_t g0, uint64_t g1)
{
    int r = 0;
    nvmtl_wss_pid *p;
    if (g0 == g1) return 0;
    r |= NVMTL_WSS_H_HAZARD;
    NVMTL_WSS_INC(s, war);
    st->hazards++;
    if (nvmtl_wss_demote(st, s, sid)) r |= NVMTL_WSS_H_DEMOTED;
    p = nvmtl_wss_pid_slot(st, pid, 1);
    if (p && ++p->hazards >= NVMTL_WSS_PID_HAZARD_MAX && !p->demoted) {
        p->demoted = 1;
        NVMTL_WSS_INC(s, pdem);
        r |= NVMTL_WSS_H_PRODUCER;
    }
    if (st->hazards >= NVMTL_WSS_HAZARD_MAX && !st->session_off) { st->session_off = 1; r |= NVMTL_WSS_H_SESSION; }
    return r;
}

/* ---- the plan at wrapper creation ---- */
enum { NVMTL_WSS_PLAN_PRIVATE = 0, NVMTL_WSS_PLAN_CACHED, NVMTL_WSS_PLAN_IMPORT, NVMTL_WSS_PLAN_REIMPORT,
       NVMTL_WSS_PLAN_DEMOTED };   /* nvmtl_wss_plan_demote: the cached backing's producer was demoted */
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

/* ECC review of N71 (c): the producer's demotion (4 hazards) is checked for a NEW import (NVMTLDevice.m, after the kernel names the
 * producer) and here for the cached read-only backing, whose producer pid sits on its import (rb->_vramBuf.share_pid): a plan that
 * would reuse it (CACHED) or replace it with another generation of the same surface (REIMPORT) becomes PLAN_DEMOTED, so a demoted
 * producer's surfaces are not sampled read-only through a cache entry either (IMPORT has no backing, so no pid yet). */
static inline int nvmtl_wss_plan_demote(int plan, int producer_demoted)
{
    return producer_demoted && (plan == NVMTL_WSS_PLAN_CACHED || plan == NVMTL_WSS_PLAN_REIMPORT) ? NVMTL_WSS_PLAN_DEMOTED : plan;
}

/* ---- what a wrapper creation does about a surface (NVMTLDevice.m nvmtlNewSurfaceTexture) ----
 * why: nvmtl_wss_eligible's answer for the wrapper; pgen: the producer's generation on the surface. The caller reads pgen only when
 * nvmtl_wss_reads_gen(why) (a3share 0, NVMTL_WS_SHARE=0 and every app never touch the surface's values: "as before"). */
enum { NVMTL_WSS_WA_UNTOUCHED = 0,   /* today's path, byte for byte */
       NVMTL_WSS_WA_PLAN,            /* nvmtl_wss_plan decides */
       NVMTL_WSS_WA_PRIVATE_INELIG,  /* shared, but this texture can write: private, counted (sh_inelig) and logged */
       NVMTL_WSS_WA_PRIVATE_QUIET }; /* shared, demoted or the session stopped: private, already said */
static inline int nvmtl_wss_reads_gen(int why)
{
    return why != NVMTL_WSS_NOT_WS && why != NVMTL_WSS_KILL && why != NVMTL_WSS_SWITCH_OFF;
}
static inline int nvmtl_wss_wrapper_action(int why, uint64_t pgen)
{
    if (!nvmtl_wss_reads_gen(why) || !pgen) return NVMTL_WSS_WA_UNTOUCHED;
    if (why == NVMTL_WSS_OK) return NVMTL_WSS_WA_PLAN;
    if (why == NVMTL_WSS_DEMOTED || why == NVMTL_WSS_SESSION_OFF) return NVMTL_WSS_WA_PRIVATE_QUIET;
    return NVMTL_WSS_WA_PRIVATE_INELIG;
}
/* the plan's outcome for the image creation: whether it asks for an import (ws_plan) and whether it skips the kernel */
enum { NVMTL_WSS_PO_NONE = 0, NVMTL_WSS_PO_IMPORT, NVMTL_WSS_PO_STALE_PRIVATE, NVMTL_WSS_PO_PRIVATE,
       NVMTL_WSS_PO_DEMOTED_PRIVATE };   /* the producer was demoted: private without asking the kernel, the surface demoted */
static inline int nvmtl_wss_plan_outcome(int plan, int pw)
{
    if (plan == NVMTL_WSS_PLAN_DEMOTED) return NVMTL_WSS_PO_DEMOTED_PRIVATE;
    if (plan == NVMTL_WSS_PLAN_IMPORT || plan == NVMTL_WSS_PLAN_REIMPORT) return NVMTL_WSS_PO_IMPORT;
    return pw == NVMTL_WSS_PW_STALE ? NVMTL_WSS_PO_STALE_PRIVATE : NVMTL_WSS_PO_PRIVATE;
}

/* ---- a sample ---- */
enum { NVMTL_WSS_SAMPLE_SKIP = 0, NVMTL_WSS_SAMPLE_STALE };
static inline int nvmtl_wss_sample(int tag_current) { return tag_current ? NVMTL_WSS_SAMPLE_SKIP : NVMTL_WSS_SAMPLE_STALE; }

/* ---- what the kernel gave (drm_xnu_surface_last_import) ---- */
enum { NVMTL_WSS_IMP_ERR = 0, NVMTL_WSS_IMP_RO, NVMTL_WSS_IMP_RO_UNUSABLE, NVMTL_WSS_IMP_RW };
/* what the image creation does with a SUCCESSFUL import: ws_plan != 0 = a read-only one was planned (the producer shared the surface) */
enum { NVMTL_WSS_IV_WRITABLE_AS_BEFORE = 0,   /* a writable import, none planned: WindowServer's own surface, today's path */
       NVMTL_WSS_IV_USE_RO,                   /* planned and read-only: build the sample-only alias */
       NVMTL_WSS_IV_DROP_RW,                  /* planned but WRITABLE (the record is gone, a3ws 1): give it back, sh_rw, demote, private - it must never be uploaded into */
       NVMTL_WSS_IV_DROP_RO_UNPLANNED };      /* read-only but not planned (no producer generation on the surface): give it back, demote, private, no sh_refused */
static inline int nvmtl_wss_import_verdict(int ro, int ws_plan)
{
    if (ro) return ws_plan ? NVMTL_WSS_IV_USE_RO : NVMTL_WSS_IV_DROP_RO_UNPLANNED;
    return ws_plan ? NVMTL_WSS_IV_DROP_RW : NVMTL_WSS_IV_WRITABLE_AS_BEFORE;
}
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
           __atomic_load_n(&s->demoted, __ATOMIC_RELAXED) + __atomic_load_n(&s->wr, __ATOMIC_RELAXED) + __atomic_load_n(&s->inelig, __ATOMIC_RELAXED) + __atomic_load_n(&s->rw, __ATOMIC_RELAXED);
}
/* the key=value tail of `vk: ws frame: SUMMARY` for a process that runs the consumer (WindowServer): printed even when all are 0,
 * so the triage can tell "the consumer path ran and imported nothing" from "an older plugin". session_off: sh_off. */
static inline int nvmtl_wss_format(const nvmtl_wss_stats *s, int session_off, char *out, size_t cap)
{
#define W_(x) ((unsigned long long)__atomic_load_n(&s->x, __ATOMIC_RELAXED))
    if (!cap) return 0;
    snprintf(out, cap, "sh_imp=%llu sh_skip=%llu sh_stale=%llu sh_reimp=%llu sh_refused=%llu sh_war=%llu sh_demoted=%llu sh_wr=%llu "
                       "sh_inelig=%llu sh_rw=%llu sh_eperm=%llu sh_enospc=%llu sh_enoent=%llu sh_enodev=%llu sh_enxio=%llu sh_eother=%llu sh_off=%d sh_pdem=%llu",
             W_(imp), W_(skip), W_(stale), W_(reimp), W_(refused), W_(war), W_(demoted), W_(wr), W_(inelig), W_(rw),
             W_(e_perm), W_(e_nospc), W_(e_noent), W_(e_nodev), W_(e_nxio), W_(e_other), session_off ? 1 : 0, W_(pdem));
#undef W_
    return 1;
}

#endif
