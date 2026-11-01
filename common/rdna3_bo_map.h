/*
 * rdna3_bo_map.h - a CPU mapping of a GEM BO: what may be mapped, and the
 * rule that keeps the BO (its VRAM range, its doorbell pages) alive for as
 * long as a mapping of it exists (DRIVER-PLAN N47 (b)). Plain C over
 * uapi/rdna3_drm.h: the core's RDNA3BoMemory (kext/src/RDNA3Drm.cpp) uses it
 * and tests/test_bo_map.c checks it with the real UAPI on the host.
 *
 * Ported from amdgpu / DRM:
 * - amdgpu_gem_object_mmap: a userptr BO, or one created with
 *   AMDGPU_GEM_CREATE_NO_CPU_ACCESS, is never mapped (-EPERM). gem_mmap
 *   (uapi/rdna3_drm.c) already refuses to hand out their offsets, but the
 *   offsets are predictable and every userptr BO has offset 0, so the map
 *   itself checks again (rd3_bomap_classify).
 * - drm_gem_mmap_obj takes a GEM reference that the VMA holds
 *   (drm_gem_vm_open / drm_gem_vm_close; ttm_bo_vm_open / ttm_bo_vm_close
 *   for split VMAs): closing every handle, or the file, leaves the BO and
 *   its TTM resource alive until the last mapping goes, so the VRAM is not
 *   handed to anyone else while it is still mapped.
 *
 * On macOS the mapping is an IOMemoryMap and it holds exactly one object:
 * the descriptor whose doMap ran (IOMemoryDescriptor::makeMapping,
 * setMemoryDescriptor). The core gives every VRAM / doorbell BO one
 * descriptor (RDNA3BoMemory) that holds a BO reference for its whole life
 * and maps itself, so each IOMemoryMap of the BO holds the BO. The BO points
 * back at it weakly (rdna3_bo::os_map) so that a second map, and
 * IOConnectUnmapMemory64's lookup (kIOMapReference, which searches the
 * descriptor's own mappings), find the same descriptor. A lookup retains it
 * under a lock; the release that would free it clears the link under that
 * same lock first (rd3_bomap_release_unlinks), so a lookup never retains a
 * descriptor that is already going (xnu keeps OSObject::taggedTryRetain
 * private).
 */
#ifndef RDNA3_BO_MAP_H
#define RDNA3_BO_MAP_H

#include "rdna3_drm.h"

/* what drmMemoryForOffset may hand out for a BO */
enum rd3_bomap_kind {
    RD3_BOMAP_NO_BO = 0,          /* no BO of the file at that offset */
    RD3_BOMAP_VRAM,               /* BAR0, write-combined, a per-BO descriptor */
    RD3_BOMAP_DOORBELL,           /* BAR2, uncached, a per-BO descriptor */
    RD3_BOMAP_GTT,                /* the BO's own pages: the map retains their descriptor */
    RD3_BOMAP_REFUSE_USERPTR,     /* amdgpu_gem_object_mmap: -EPERM */
    RD3_BOMAP_REFUSE_NO_CPU,      /* AMDGPU_GEM_CREATE_NO_CPU_ACCESS (and on-chip GDS/GWS/OA) */
    RD3_BOMAP_REFUSE_OUTSIDE,     /* a VRAM / doorbell BO beyond the BAR the CPU sees */
    RD3_BOMAP_REFUSE_NO_BACKING,  /* a GTT BO without pages */
    RD3_BOMAP_KINDS
};

static inline int rd3_bomap_refused(enum rd3_bomap_kind k)
{
    return k >= RD3_BOMAP_REFUSE_USERPTR && k < RD3_BOMAP_KINDS;
}

static inline const char *rd3_bomap_kind_name(enum rd3_bomap_kind k)
{
    switch (k) {
    case RD3_BOMAP_NO_BO:             return "no BO of this file at that offset";
    case RD3_BOMAP_VRAM:              return "VRAM";
    case RD3_BOMAP_DOORBELL:          return "doorbell";
    case RD3_BOMAP_GTT:               return "GTT";
    case RD3_BOMAP_REFUSE_USERPTR:    return "a userptr BO (amdgpu_gem_object_mmap: -EPERM)";
    case RD3_BOMAP_REFUSE_NO_CPU:     return "created without CPU access (amdgpu_gem_object_mmap: -EPERM)";
    case RD3_BOMAP_REFUSE_OUTSIDE:    return "outside the BAR the CPU sees";
    case RD3_BOMAP_REFUSE_NO_BACKING: return "a GTT BO without pages";
    default:                          return "?";
    }
}

/* [off, off + size) inside a BAR of 'bar' bytes, without overflow */
static inline int rd3_bomap_in_bar(uint64_t off, uint64_t size, uint64_t bar)
{
    return size <= bar && off <= bar - size;
}

/* the kind of mapping a BO gets (NULL: none at that offset). 'vram_bar' /
 * 'doorbell_bar' are the lengths of BAR0 / BAR2 (0: not mapped) */
static inline enum rd3_bomap_kind rd3_bomap_classify(const rdna3_bo *bo, uint64_t vram_bar,
                                                     uint64_t doorbell_bar)
{
    if (!bo)
        return RD3_BOMAP_NO_BO;
    if (bo->userptr)
        return RD3_BOMAP_REFUSE_USERPTR;
    if ((bo->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS) || bo->gpu_ro || (bo->placement & RD_ONCHIP_DOMAINS))
        return RD3_BOMAP_REFUSE_NO_CPU;
    if (bo->placement == AMDGPU_GEM_DOMAIN_DOORBELL)
        return rd3_bomap_in_bar(bo->vram_offset, bo->size, doorbell_bar) ? RD3_BOMAP_DOORBELL
                                                                         : RD3_BOMAP_REFUSE_OUTSIDE;
    if (bo->placement != AMDGPU_GEM_DOMAIN_VRAM)
        return bo->backing ? RD3_BOMAP_GTT : RD3_BOMAP_REFUSE_NO_BACKING;
    return rd3_bomap_in_bar(bo->vram_offset, bo->size, vram_bar) ? RD3_BOMAP_VRAM
                                                                 : RD3_BOMAP_REFUSE_OUTSIDE;
}

/* What a BO held by a CPU mapping costs is charged to the uid that made the
 * mapping (rdna3_drm_bomap_charge), since such a BO outlives its handles,
 * its file and the file's per-file limits (ECC security review of N47 (b):
 * map + GEM_CLOSE loops, or close and reopen, pinned VRAM and kernel
 * objects without bound). The caps: RD3_BOMAP_DEVICE_MAX descriptors on the
 * device; for any uid but root and _windowserver (as rdna3_vmid_admit) at
 * most RD_MAX_HANDLES descriptors (a file's own handle cap) and
 * RD3_BOMAP_OWNER_BYTES_MAX bytes: twice the CPU-visible VRAM, which ranges
 * of VRAM BOs cannot exceed; the rest is IOSurface imports, which the
 * surface quota (RD_SURFACE_BO_FILE_MAX, counting what mappings hold) bounds. */
#define RD3_BOMAP_DEVICE_MAX 65536u
#define RD3_BOMAP_OWNER_BYTES_MAX(vis) (2 * (uint64_t)(vis))

enum rd3_bomap_quota {
    RD3_BOMAP_Q_OK = 0,
    RD3_BOMAP_Q_DEVICE_COUNT,
    RD3_BOMAP_Q_OWNER_COUNT,
    RD3_BOMAP_Q_OWNER_BYTES,
    RD3_BOMAP_Q_TABLE_FULL,
};

static inline const char *rd3_bomap_quota_name(int q)
{
    switch (q) {
    case RD3_BOMAP_Q_OK:           return "ok";
    case RD3_BOMAP_Q_DEVICE_COUNT: return "the device's limit of mapped BOs (RD3_BOMAP_DEVICE_MAX)";
    case RD3_BOMAP_Q_OWNER_COUNT:  return "this uid's limit of mapped BOs (RD_MAX_HANDLES)";
    case RD3_BOMAP_Q_OWNER_BYTES:  return "this uid's limit of mapped bytes (twice the visible VRAM)";
    case RD3_BOMAP_Q_TABLE_FULL:   return "no slot left for another uid (RD_BOMAP_OWNERS)";
    default:                       return "?";
    }
}

/* the system's own clients, as rdna3_vmid_admit: only the device's limit */
static inline int rd3_bomap_uid_exempt(uint32_t uid)
{
    return uid == 0 || uid == RD_UID_WINDOWSERVER;
}

/* may 'uid', holding 'owner_n' descriptors of 'owner_bytes', get one more of
 * 'size' bytes while the device holds 'dev_n'? */
static inline enum rd3_bomap_quota rd3_bomap_quota_admit(uint32_t uid, uint32_t owner_n, uint64_t owner_bytes,
                                                         uint32_t dev_n, uint64_t size, uint64_t owner_bytes_max)
{
    if (dev_n >= RD3_BOMAP_DEVICE_MAX)
        return RD3_BOMAP_Q_DEVICE_COUNT;
    if (rd3_bomap_uid_exempt(uid))
        return RD3_BOMAP_Q_OK;
    if (owner_n >= RD_MAX_HANDLES)
        return RD3_BOMAP_Q_OWNER_COUNT;
    if (size > owner_bytes_max || owner_bytes > owner_bytes_max - size)
        return RD3_BOMAP_Q_OWNER_BYTES;
    return RD3_BOMAP_Q_OK;
}

/* a per-BO descriptor holds a BO reference for its whole life */
static inline int rd3_bomap_per_bo(enum rd3_bomap_kind k)
{
    return k == RD3_BOMAP_VRAM || k == RD3_BOMAP_DOORBELL;
}

/* the BO's descriptor if it has one (the caller holds the link lock and
 * retains what it gets before dropping it) */
static inline void *rd3_bomap_lookup(void *const *link)
{
    return *link;
}

/* a new descriptor for a BO without one (link lock held) */
static inline void rd3_bomap_link(void **link, void *self)
{
    if (!*link)
        *link = self;
}

/* One release of the per-BO descriptor 'self', under the link lock.
 * 'count_before' is its reference count before this release, 'free_when'
 * OSObject's threshold (the object is freed when the count drops below
 * it). Returns 1 when this release frees it: the BO's link to it is cleared
 * here, before the lock is dropped, so no lookup can find it any more and
 * the free may run outside the lock. Returns 0 otherwise: the release must
 * then decrement under the lock, so that two releases never both see a
 * count above the threshold and leave a linked object at zero. */
static inline int rd3_bomap_release_unlinks(void **link, const void *self, int count_before, int free_when)
{
    if (count_before - 1 >= free_when)
        return 0;
    if (*link == self)
        *link = NULL;
    return 1;
}

#endif
