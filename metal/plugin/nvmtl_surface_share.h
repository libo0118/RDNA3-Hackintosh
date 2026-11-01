/*
 * nvmtl_surface_share.h - the PRODUCER side of N71 (c): whether an application's IOSurface textures live in the surface's
 * family VRAM (item 7), and when it tells the kernel that WindowServer may import the finished frame read-only
 * (drm_xnu_surface_share). DRIVER-PLAN N71.
 *
 * With BAR0 at 1 GiB the visible heap has room, so an app's CAMetalLayer drawables became family-VRAM surfaces; the
 * kernel refuses WindowServer that import unless the producer recorded a share (`debug.rdna3accel_a3share`, default 0),
 * so WindowServer read every frame through the BAR with the CPU (~4.5 ms a 1080p frame, the game 123-145 fps instead of
 * ~215). Plain C99, so tests/test_surface_share.c checks the decisions exactly as nvmtl_vk.c and the command buffer's
 * publish (NVMTLEventSync.m) use them.
 *
 * (1) The gate: nvmtl_surface_share_kept_private(). With a3share 0 (or absent) and the kernel's a3ws policy 0 an app's
 *     textures stay private, exactly as the N71 (a) interim; a3share 1 or a3ws 1 lets them live in family VRAM.
 * (2) The record: one nvmtl_ps_tex per VRAM texture. nvmtl_ps_decide() says whether to call drm_xnu_surface_share now (after
 *     the GPU finished the presenting buffer, before the publish and the tag bump), nvmtl_ps_result() folds the errno in:
 *       0       recorded for this allocation generation (never asked again for it)
 *       ENXIO   the kernel's switch is off (or the accelerator not up): asked again after the next re-read of the switch
 *       ENODEV, ENOSYS   an older accelerator / kext: no more asks in this process
 *       EPERM   the surface was re-backed since this import: its VRAM is not what this texture draws into any more, so a
 *               record would show WindowServer an allocation nobody updates; no record for this import (a new import of
 *               the surface starts a new record)
 *       ENOENT  no writable import of the plane on the connection that holds it: no record for this import
 */
#ifndef NVMTL_SURFACE_SHARE_H
#define NVMTL_SURFACE_SHARE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>

/* 1: this process's IOSurface textures stay private (copies) although the import works. is_ws: the process is
 * WindowServer; a3ws: debug.rdna3accel_a3ws (-1 when the sysctl is absent: an older accelerator, no policy to follow);
 * forced: NVMTL_SURFACE_SHARE=1 (share regardless); a3share: debug.rdna3accel_a3share (-1 absent, else 0 / 1): 1 lets an
 * app's textures live in family VRAM because the kernel gives WindowServer a read-only import of what it shares. */
static inline int nvmtl_surface_share_kept_private(int is_ws, int a3ws, int forced, int a3share)
{
    return !forced && !is_ws && a3ws == 0 && a3share != 1;
}

/* the kernel's switch, read live: re-read at most once a second. last/at: the cached value and when it was read (ns, 0 never). */
#define NVMTL_PS_REREAD_NS 1000000000ull
static inline int nvmtl_ps_reread_due(uint64_t last_read_ns, uint64_t now_ns)
{
    return !last_read_ns || now_ns < last_read_ns || now_ns - last_read_ns >= NVMTL_PS_REREAD_NS;
}

/* ---- the record ---- */
typedef struct {
    uint64_t gen;          /* the allocation generation of the import (drm_xnu_surface_last_import; 0: an older kext) */
    uint64_t shared_gen;   /* the generation recorded; == gen once done */
    uint64_t retry_ns;     /* ENXIO: not before */
    uint32_t tries;        /* answers of the "other" kind for this import */
    uint8_t  dead;         /* no more asks for this import (EPERM, ENOENT, repeated failures) */
    uint8_t  said;         /* the outcome was logged for this import */
} nvmtl_ps_tex;
typedef struct { uint8_t stop; } nvmtl_ps_proc;   /* ENODEV / ENOSYS: an older kernel, no more asks in this process */

/* the prefilter before the call into nvmtl_vk.c (NVMTLObjects.m nvmtl_ps_after_gpu), per part and surface: 1 = worth asking nvmtl_ps_decide.
 * vram_on: a writable family-VRAM texture; ro: a read-only alias; has_surf: it has an IOSurface. Done, dead and "no generation, said already" skip. */
static inline int nvmtl_ps_wants(const nvmtl_ps_tex *t, int vram_on, int ro, int has_surf)
{
    if (!vram_on || ro || !has_surf || t->dead) return 0;
    if (!t->gen) return !t->said;
    return t->shared_gen != t->gen;
}

enum {   /* nvmtl_ps_decide */
    NVMTL_PS_NO_WS = 0,   /* WindowServer never records */
    NVMTL_PS_NO_KILL,     /* NVMTL_NO_SURFACE_SHARE=1 or NVMTL_WS_SHARE=0 */
    NVMTL_PS_NO_STOP,     /* an older kernel answered */
    NVMTL_PS_NO_NOGEN,    /* the import gave no generation (an older kext / libdrm_xnu): the kernel cannot match a record */
    NVMTL_PS_NO_DEAD,     /* EPERM / ENOENT for this import */
    NVMTL_PS_NO_DONE,     /* recorded already */
    NVMTL_PS_NO_SWITCH,   /* debug.rdna3accel_a3share is not 1 */
    NVMTL_PS_NO_WAIT,     /* ENXIO: after the next re-read */
    NVMTL_PS_CALL         /* ask the kernel now */
};
static inline int nvmtl_ps_decide(const nvmtl_ps_tex *t, const nvmtl_ps_proc *p, int is_ws, int kill, int a3share, uint64_t now_ns)
{
    if (is_ws) return NVMTL_PS_NO_WS;
    if (kill) return NVMTL_PS_NO_KILL;
    if (p->stop) return NVMTL_PS_NO_STOP;
    if (a3share != 1) return NVMTL_PS_NO_SWITCH;   /* with the switch at 0 nothing at all happens (no log, no count) */
    if (!t->gen) return NVMTL_PS_NO_NOGEN;
    if (t->dead) return NVMTL_PS_NO_DEAD;
    if (t->shared_gen == t->gen) return NVMTL_PS_NO_DONE;
    if (t->retry_ns && now_ns < t->retry_ns) return NVMTL_PS_NO_WAIT;
    return NVMTL_PS_CALL;
}

enum { NVMTL_PSR_RECORDED = 0, NVMTL_PSR_RETRY, NVMTL_PSR_STOP, NVMTL_PSR_REBACKED, NVMTL_PSR_NOIMPORT, NVMTL_PSR_OTHER, NVMTL_PSR_N };
#define NVMTL_PS_OTHER_TRIES 3u   /* an unexpected errno is asked again (a second later) this many times, then the import is dead */
/* the kernel's answer: err 0 or the errno of drm_xnu_surface_share. Returns the NVMTL_PSR_* class. */
static inline int nvmtl_ps_result(nvmtl_ps_tex *t, nvmtl_ps_proc *p, int err, uint64_t now_ns)
{
    switch (err) {
    case 0:       t->shared_gen = t->gen; t->retry_ns = 0; return NVMTL_PSR_RECORDED;
    case ENXIO:   t->retry_ns = now_ns + NVMTL_PS_REREAD_NS; return NVMTL_PSR_RETRY;
    case ENODEV:
    case ENOSYS:  p->stop = 1; return NVMTL_PSR_STOP;
    case EPERM:   t->dead = 1; return NVMTL_PSR_REBACKED;
    case ENOENT:  t->dead = 1; return NVMTL_PSR_NOIMPORT;
    default:
        if (++t->tries >= NVMTL_PS_OTHER_TRIES) t->dead = 1;
        t->retry_ns = now_ns + NVMTL_PS_REREAD_NS;
        return NVMTL_PSR_OTHER;
    }
}
static inline const char *nvmtl_ps_errno_name(int err)
{
    switch (err) {
    case 0: return "ok"; case ENXIO: return "ENXIO"; case ENODEV: return "ENODEV"; case ENOSYS: return "ENOSYS";
    case EPERM: return "EPERM"; case ENOENT: return "ENOENT"; case EINVAL: return "EINVAL"; case ENOMEM: return "ENOMEM";
    case EBUSY: return "EBUSY"; case ENOSPC: return "ENOSPC"; default: return "other";
    }
}

/* ---- the counters (`vk: ws frame: SUMMARY` pr_*; every process) ---- */
typedef struct {
    uint64_t recorded, retry, stop, rebacked, noimport, other, nogen, calls;
} nvmtl_ps_stats;
static inline void nvmtl_ps_count(nvmtl_ps_stats *s, int cls)
{
    __atomic_fetch_add(&s->calls, 1, __ATOMIC_RELAXED);
    switch (cls) {
    case NVMTL_PSR_RECORDED: __atomic_fetch_add(&s->recorded, 1, __ATOMIC_RELAXED); break;
    case NVMTL_PSR_RETRY:    __atomic_fetch_add(&s->retry, 1, __ATOMIC_RELAXED); break;
    case NVMTL_PSR_STOP:     __atomic_fetch_add(&s->stop, 1, __ATOMIC_RELAXED); break;
    case NVMTL_PSR_REBACKED: __atomic_fetch_add(&s->rebacked, 1, __ATOMIC_RELAXED); break;
    case NVMTL_PSR_NOIMPORT: __atomic_fetch_add(&s->noimport, 1, __ATOMIC_RELAXED); break;
    default:                 __atomic_fetch_add(&s->other, 1, __ATOMIC_RELAXED); break;
    }
}
static inline uint64_t nvmtl_ps_events(const nvmtl_ps_stats *s)
{
    return __atomic_load_n(&s->calls, __ATOMIC_RELAXED) + __atomic_load_n(&s->nogen, __ATOMIC_RELAXED);
}
/* the key=value tail of the SUMMARY; 0 (nothing written) when no share was ever asked for */
static inline int nvmtl_ps_format(const nvmtl_ps_stats *s, char *out, size_t cap)
{
#define P_(x) ((unsigned long long)__atomic_load_n(&s->x, __ATOMIC_RELAXED))
    if (!nvmtl_ps_events(s) || !cap) return 0;
    snprintf(out, cap, "pr_shared=%llu pr_enxio=%llu pr_stop=%llu pr_eperm=%llu pr_enoent=%llu pr_other=%llu pr_nogen=%llu",
             P_(recorded), P_(retry), P_(stop), P_(rebacked), P_(noimport), P_(other), P_(nogen));
#undef P_
    return 1;
}

#endif
