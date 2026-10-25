/*
 * RDNA3Platform.cpp - the core kext's answers to the accelerator kext
 * (accel/, IOGraphicsAccelerator2 subclass) over callPlatformFunction.
 * The interface is accel/rdna3k_abi.h.
 *
 *   rdna3AllocVram / rdna3FreeVram   VRAM from the same heap as GEM BOs,
 *                                    CPU-visible through BAR0
 *   rdna3DeviceHandles               device cookie (this)
 *   rdna3GpuVaAlloc / Map / Free     GPU VA ranges for the family's tasks,
 *                                    backed by one GPUVM context (its own
 *                                    VMID) the accelerator's VRAM is mapped
 *                                    into, linear 4 KiB PTEs
 *   rdna3SurfPageoff                 copies an accelerator VRAM allocation
 *                                    into a list of system pages (IOSurface
 *                                    page-off): SDMA0 through a GART window
 *                                    from rdna3lvl=7, else the CPU via BAR
 *   rdna3CopyVram                    a pitched rectangle between two
 *                                    VRAM allocations by SDMA0 (the
 *                                    accelerator's swap copy, A2)
 *   rdna3SurfShare                   not offered (no RM-style cross-client
 *                                    handles on AMD; RADV shares by GEM)
 *   rdna3RefVram                     a GEM BO over an accelerator
 *                                    allocation holds it
 *                                    (RD_NR_XNU_SURFACE_BO, A3)
 *   rdna3VramRefs                    how many hold an accelerator
 *                                    allocation (N41: parks released
 *                                    under pressure only without a BO)
 *   rdna3NewAccelClient              an IOAccelRDNA3Bridge (RDNA3UserClient)
 *                                    for a process whose sandbox refuses
 *                                    RDNA3UserClient (WindowServer, card
 *                                    run 47); the accelerator attaches it
 *
 * Display heads (Stage 3, RDNA3Display.cpp) are answered here for
 * RDNA3Framebuffer; the accelerator's own scanout/flip requests go to the
 * framebuffer, which forwards them.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"
#include "RDNA3UserClient.hpp"

#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>

#define RD3K_SS_KERNEL_ABI 1
#include "rdna3k_abi.h"
#include "RDNA3Bringup.hpp"
#include <IOKit/IOMemoryDescriptor.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
#include <sys/sysctl.h>

/* DRIVER-PLAN N41: the UAPI's flags reach the accelerator's escape unchanged */
static_assert(RD_XNU_SURFACE_PLANE_GPU_MARK == RD3K_SS_PLANE_GPU_MARK, "uapi and accelerator GPU mark bits differ");
static_assert(RD_XNU_SURFACE_PLANE_TRYLOCK == RD3K_SS_PLANE_TRYLOCK, "uapi and accelerator try-lock bits differ");

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

/* GPU VA window for the family's tasks: 1 TiB at 64 TiB, inside the low
 * half of the 48-bit space and away from where RADV places its own VAs. */
static const uint64_t kAccelVaBase = 0x0000400000000000ull;
static const uint64_t kAccelVaSize = 0x0000010000000000ull; /* 1 TiB */

struct RDNA3VramHandle {
    uint64_t offset, size;
    RDNA3VramHandle *prev, *next;   /* fVramHandles, under fDrmLock */
    /* the accelerator's own reference until rdna3FreeVram, one per GEM BO
     * over it (RD_NR_XNU_SURFACE_BO); the range goes back at zero, and the
     * allocation stays on the eviction list until then */
    uint32_t refs;
    uint32_t owner;   /* RD3_VO_*: who asked, for the heap report */
};

/* the accelerator's live VRAM allocations (the eviction list before a
 * sleep); fDrmLock held */
void RDNA3Accel::platformForEachVram(void (*fn)(void *ctx, uint64_t off, uint64_t size), void *ctx)
{
    for (RDNA3VramHandle *h = fVramHandles; h; h = h->next)
        fn(ctx, h->offset, h->size);
}

/* from the bring-up thread once GPUVM runs (fVmReady): the accelerator's
 * VM takes its VMID before any DRM client can, so WindowServer's IOSurface
 * maps never find the pool empty */
void RDNA3Accel::platformVmReady()
{
    RD3DrmScope lk(this);
    if (fAccelVmStopping || !fBringup || !fAccelVa || (fAccelVm && fBringup->vmLive(fAccelVm)))
        return;
    if (fAccelVm)
        fBringup->vmDestroy(fAccelVm);
    fAccelVm = fBringup->vmCreate(false, true);
    fAccelVmRefusalLogged = !fAccelVm;
    if (!fAccelVm)
        LOG("accelerator: no VM at GPUVM start: IOSurface GPU maps fail until one is free");
}

bool RDNA3Accel::platformInit()
{
    fAccelVa = (range_alloc *)drmZalloc(this, sizeof(*fAccelVa));
    if (!fAccelVa)
        return false;
    range_alloc_init(fAccelVa, kAccelVaBase, kAccelVaSize);
    /* the accelerator kext waits for this before it starts */
    if (fPci)
        fPci->setProperty("rdna3-claimed", kOSBooleanTrue);
    return true;
}

void RDNA3Accel::platformFree()
{
    if (fAccelVm && fBringup)
        fBringup->vmDestroy(fAccelVm);
    fAccelVm = nullptr;
    if (fPci)
        fPci->removeProperty("rdna3-claimed");
    if (fAccelVa)
        drmFreeMem(this, fAccelVa, sizeof(*fAccelVa));
    fAccelVa = nullptr;
}

IOReturn RDNA3Accel::platformVramAlloc(RD3KVramRequest *r)
{
    if (!r || r->version != RD3K_VRAM_ABI_VERSION || !r->size)
        return kIOReturnBadArgument;
    if (!fVramHeap || !fVramMap || !fVramDev || !fDrmLock) {
        __sync_fetch_and_add(&fVramPools.refused_other, 1);   /* no heap yet / any more: not an exhaustion */
        return kIOReturnNotReady;
    }

    uint64_t size = (r->size + 4095) & ~4095ull;
    /* who asks (rdna3k_abi.h RD3K_VRAM_OWNER_*; an older caller sends 0) */
    int owner = (int)(r->flags & RD3K_VRAM_OWNER_MASK);
    if (owner > RD3_VO_ACCEL_LAST)
        owner = RD3_VO_ACCEL;
    RDNA3VramHandle *h = (RDNA3VramHandle *)IOMalloc(sizeof(*h));
    if (!h) {
        __sync_fetch_and_add(&fVramPools.refused_other, 1);
        return kIOReturnNoMemory;
    }

    IOLockLock(fDrmLock);
    /* the reserve in front of the visible heap first, then the general
     * visible part (common/rdna3_vram_place.h); 64 KiB aligned */
    uint64_t off = rdna3_vram_alloc_accel(&fVramPools, size, owner);
    /* the accelerator touches it through kva: it must be CPU visible */
    if (off != RANGE_ALLOC_FAIL && off + size > fVramMap->getLength()) {
        vramFreeRange(off, size, owner);
        rdna3_vram_note_refused(&fVramPools, owner, size);
        off = RANGE_ALLOC_FAIL;
    }
    if (off != RANGE_ALLOC_FAIL) {
        fAccelVramBytes += size;
        h->offset = off;
        h->size = size;
        h->refs = 1;
        h->owner = (uint32_t)owner;
        h->prev = nullptr;
        h->next = fVramHandles;
        if (fVramHandles)
            fVramHandles->prev = h;
        fVramHandles = h;
    } else {
        /* the display's own memory is gone: the heap by owner, once per
         * refusal (rate-limited) */
        vramNoteRefusal("accelerator allocation", size, RD3_VP_ACCEL_ALIGN, owner);
    }
    uint64_t total = fAccelVramBytes;
    vramSummaryMaybe(false);
    IOLockUnlock(fDrmLock);

    if (off == RANGE_ALLOC_FAIL) {
        IOFree(h, sizeof(*h));
        return kIOReturnNoSpace;
    }
    bzero((void *)(fVramMap->getVirtualAddress() + off), (size_t)size);

    r->actualSize = size;
    r->phys = fVramDev->getPhysicalAddress() + off;
    r->kva = (void *)(fVramMap->getVirtualAddress() + off);
    r->handle = h;
    r->mappedTotal = total;
    /* the cookie is the VRAM offset: RDNA3Framebuffer hands it back in
     * flips and rdna3DisplayFlip adds the FB aperture base (fb_start) */
    r->allocationCookie = off;
    return kIOReturnSuccess;
}

IOReturn RDNA3Accel::platformVramFree(RD3KVramRequest *r)
{
    if (!r || !r->handle || !fVramHeap || !fDrmLock)
        return kIOReturnBadArgument;
    IOLockLock(fDrmLock);
    platformVramPutLocked((RDNA3VramHandle *)r->handle);
    IOLockUnlock(fDrmLock);
    r->handle = nullptr;
    return kIOReturnSuccess;
}

/* fDrmLock held */
void RDNA3Accel::platformVramPutLocked(RDNA3VramHandle *h)
{
    if (h && !h->refs) {   /* a put too many: a refcount bug, the range may already be someone else's */
        LOG("VRAM: a reference dropped on a free accelerator allocation (%p) - ignored", h);
        return;
    }
    if (!h || --h->refs)
        return;
    vramFreeRange(h->offset, h->size, (int)h->owner);
    fAccelVramBytes -= h->size;
    if (h->prev)
        h->prev->next = h->next;
    else
        fVramHandles = h->next;
    if (h->next)
        h->next->prev = h->prev;
    IOFree(h, sizeof(*h));
}

/* ---- the visible heap's report (card run 2026-10-17c) ----
 * Who holds the CPU-visible VRAM, at every refusal, every 30 s while
 * allocations come and go, and in debug.rdna3_vram (collected by
 * tools/macos/collect.sh into 38-vram-heap.txt; tools/triage_reports.py
 * judges it). fDrmLock held unless said. */

static const char *const kVramOwnerName[RD3_VO_N] = {
    "accel", "fb-surface", "fb-cursor", "swap-D", "gem", "gem-beyond-BAR0", "kernel"
};
static const char *const kVramPoolName[RD3_POOL_N] = { "reserve", "gem", "beyond-BAR0" };

#define KIB(v) ((unsigned long long)((v) >> 10))

/* the owners with something held or ever held: "accel 49200 KiB x5 (peak 65000
 * KiB, refused 0)"; an entry is written whole or not at all */
static int vramHeldText(const rdna3_vram_pools *p, char *buf, size_t n)
{
    size_t len = 0;
    buf[0] = 0;
    for (int o = 0; o < RD3_VO_N; o++) {
        const rdna3_vram_stat *st = &p->own[o];
        char ent[96];
        if (!st->peak_bytes && !st->refused)
            continue;
        int w = snprintf(ent, sizeof(ent), "%s %llu KiB x%u (peak %llu KiB, refused %u)", kVramOwnerName[o],
                         KIB(st->bytes), st->live, KIB(st->peak_bytes), st->refused);
        if (w < 0 || (size_t)w >= sizeof(ent) || len + (len ? 2 : 0) + (size_t)w + 1 > n) {
            if (len + 4 < n)
                snprintf(buf + len, n - len, "%s...", len ? ", " : "");   /* the rest did not fit */
            break;
        }
        len += (size_t)snprintf(buf + len, n - len, "%s%s", len ? ", " : "", ent);
    }
    return (int)len;
}

/* back into the pool the offset lies in. A refused put (outside the heap, a
 * double free) is counted in free_fail and logged (the first 8, then every
 * 64th); a range the full free list could not take is lost and says so
 * (the lists hold 4096 ranges). The owner's totals follow
 * rdna3_vram_free's rules. fDrmLock held. */
void RDNA3Accel::vramFreeRange(uint64_t off, uint64_t size, int owner)
{
    const int pool = rdna3_vram_pool_of(&fVramPools, off);
    range_alloc *ra = fVramPools.pool[pool];
    const int r = rdna3_vram_free(&fVramPools, off, size, owner);
    if (r) {
        const uint32_t n = r == -1 ? fVramPools.free_fail : ra ? ra->lost_ranges : 1;
        if (n <= 8 || !(n & 63))
            LOG("VRAM: could not give %llu KiB at 0x%llx (%s) back to the %s heap: %s (%u so far, %llu KiB; "
                "%llu KiB lost in this heap)", KIB(size), (unsigned long long)off,
                owner >= 0 && owner < RD3_VO_N ? kVramOwnerName[owner] : "?", kVramPoolName[pool],
                r == -1 ? "outside the heap or freed twice - the owner still holds it" : "the free list is full",
                n, KIB(r == -1 ? fVramPools.free_fail_bytes : size), ra ? KIB(ra->lost_bytes) : 0ull);
    }
    vramSummaryMaybe(false);
}

/* one refusal: what was asked, what each heap has, who holds the rest.
 * Rate-limited: the first 16, then every 64th; the counters are in
 * debug.rdna3_vram either way */
void RDNA3Accel::vramNoteRefusal(const char *who, uint64_t size, uint64_t align, int owner)
{
    const rdna3_vram_pools *p = &fVramPools;
    const uint32_t n = ++fVramRefusalLogs;
    if (n == 1)
        vramSummaryMaybe(true);   /* the state around the first one, whatever the 30 s say */
    if (n > 16 && (n & 63))
        return;
    char held[512];
    vramHeldText(p, held, sizeof(held));
    const range_alloc *acc = p->pool[RD3_POOL_ACC], *gem = p->pool[RD3_POOL_GEM], *hi = p->pool[RD3_POOL_HI];
    if (!acc || !gem || !hi)
        return;
    LOG("VRAM REFUSED #%u: %s, %llu KiB by %s; held: %s; alignment %llu KiB, placed at %llu KiB; free reserve "
        "%llu KiB (largest %llu KiB), visible heap %llu KiB (largest %llu KiB), beyond BAR0 %llu KiB (largest "
        "%llu KiB); free ranges %u/%u/%u, lost %llu KiB",
        n, who, KIB(size), kVramOwnerName[owner], held[0] ? held : "nothing", KIB(align),
        KIB(rdna3_vram_place_align(size, align)),
        KIB(range_alloc_free_bytes(acc)), KIB(range_alloc_largest_free(acc)),
        KIB(range_alloc_free_bytes(gem)), KIB(range_alloc_largest_free(gem)),
        KIB(range_alloc_free_bytes(hi)), KIB(range_alloc_largest_free(hi)), acc->nfree, gem->nfree, hi->nfree,
        KIB(acc->lost_bytes + gem->lost_bytes + hi->lost_bytes));
}

/* every 30 s at most, and only when something allocates or frees */
void RDNA3Accel::vramSummaryMaybe(bool force)
{
    uint64_t now;
    if (!fVramPools.pool[RD3_POOL_ACC])
        return;
    absolutetime_to_nanoseconds(mach_continuous_time(), &now);
    if (!fVramSummaryAt) {
        fVramSummaryAt = now ? now : 1;
        if (!force)
            return;
    }
    if (!force && now - fVramSummaryAt < 30ull * 1000000000ull)
        return;
    fVramSummaryAt = now;
    const rdna3_vram_pools *p = &fVramPools;
    if (!p->pool[RD3_POOL_ACC] || !p->pool[RD3_POOL_GEM] || !p->pool[RD3_POOL_HI])
        return;
    char held[512];
    vramHeldText(p, held, sizeof(held));
    LOG("VRAM heap: visible %llu KiB (reserve %llu KiB) free %llu + %llu KiB, beyond BAR0 free %llu KiB, "
        "refused %u, GEM blocked by the reserve %u; held: %s",
        KIB(p->vis_end - p->pool[RD3_POOL_ACC]->base),
        KIB(p->acc_end - p->pool[RD3_POOL_ACC]->base),
        KIB(range_alloc_free_bytes(p->pool[RD3_POOL_ACC])), KIB(range_alloc_free_bytes(p->pool[RD3_POOL_GEM])),
        KIB(range_alloc_free_bytes(p->pool[RD3_POOL_HI])), p->refused_total, p->gem_reserve_blocked,
        held[0] ? held : "nothing");
}

static size_t vramAppend(char *buf, size_t n, size_t len, int w)
{
    if (w < 0 || len >= n)
        return len;
    return (size_t)w < n - len ? len + (size_t)w : n - 1;
}

/* debug.rdna3_vram: one line of key=value for the triage (kib = KiB) */
void RDNA3Accel::vramReport(char *buf, size_t n)
{
    const rdna3_vram_pools *p = &fVramPools;
    const range_alloc *ra[RD3_POOL_N] = { p->pool[RD3_POOL_ACC], p->pool[RD3_POOL_GEM], p->pool[RD3_POOL_HI] };
    static const char *const pn[RD3_POOL_N] = { "acc", "gem", "hi" };
    static const char *const on[RD3_VO_N] = { "accel", "fb_surface", "fb_cursor", "swap_d", "gem", "gem_hi", "kernel" };
    uint64_t lost = 0;
    uint32_t refusedAccel = 0;
    for (int i = 0; i < RD3_POOL_N; i++)
        lost += ra[i]->lost_bytes;
    for (int o = 0; o <= RD3_VO_ACCEL_LAST; o++)
        refusedAccel += p->own[o].refused;
    size_t len = vramAppend(buf, n, 0,
        snprintf(buf, n, "visible_kib=%llu reserve_kib=%llu reserve_arg=%lld refused=%u refused_accel=%u "
                 "refused_gem=%u refused_hi=%u reserve_blocked=%u hi_full=%u hi_clear_fail=%u lost_kib=%llu "
                 "free_fail=%u free_fail_kib=%llu retired_n=%u retired_kib=%llu refused_other=%u gtt_fail=%u "
                 "gtt_fail_kib=%llu bomap_live=%u bomap_live_kib=%llu bomap_made=%u bomap_reused=%u "
                 "bomap_refused=%u bomap_quota=%u bomap_nodesc=%u bomap_nobo=%u bomap_fail=%d",
                 KIB(p->vis_end - ra[0]->base), KIB(p->acc_end - ra[0]->base), (long long)fVisReserveArg,
                 p->refused_total, refusedAccel, p->own[RD3_VO_GEM].refused, p->own[RD3_VO_GEM_HI].refused,
                 p->gem_reserve_blocked, p->hi_full, fVramHiClearFails, KIB(lost),
                 p->free_fail, KIB(p->free_fail_bytes), p->retired_n, KIB(p->retired_bytes), p->refused_other,
                 fDrm ? fDrm->gtt_fallback_fail : 0u, fDrm ? KIB(fDrm->gtt_fallback_fail_bytes) : 0ull,
                 fBoMapLive, KIB(fBoMapLiveBytes), fBoMapMade, fBoMapReused, fBoMapRefused, fBoMapQuota,
                 fBoMapNoDesc, fBoMapNoBo,
                 (int)fBoMapFail));
    for (int i = 0; i < RD3_POOL_N && len + 1 < n; i++)
        len = vramAppend(buf, n, len,
            snprintf(buf + len, n - len, " free_%s_kib=%llu largest_%s_kib=%llu nfree_%s=%u full_skips_%s=%u",
                     pn[i], KIB(range_alloc_free_bytes(ra[i])), pn[i], KIB(range_alloc_largest_free(ra[i])),
                     pn[i], ra[i]->nfree, pn[i], ra[i]->full_skips));
    for (int o = 0; o < RD3_VO_N && len + 1 < n; o++) {
        const rdna3_vram_stat *st = &p->own[o];
        len = vramAppend(buf, n, len,
            snprintf(buf + len, n - len, " %s_kib=%llu %s_n=%u %s_peak_kib=%llu %s_refused=%u %s_refused_kib=%llu",
                     on[o], KIB(st->bytes), on[o], st->live, on[o], KIB(st->peak_bytes), on[o], st->refused,
                     on[o], KIB(st->refused_bytes)));
    }
}

int RDNA3Accel::vramSysctlRead(char *buf, size_t n)
{
    if (!fDrmLock || !fVramPools.pool[RD3_POOL_ACC])
        return ENXIO;
    IOLockLock(fDrmLock);
    vramReport(buf, n);
    IOLockUnlock(fDrmLock);
    return 0;
}

static RDNA3Accel *gVramOwner;
static SInt32 gVramSysctlBusy;

static int rdna3_vram_sysctl SYSCTL_HANDLER_ARGS
{
    (void)oidp; (void)arg1; (void)arg2;
    const size_t cap = 3072;   /* the longest line is under 2000 bytes */
    int err = ENXIO;
    OSIncrementAtomic(&gVramSysctlBusy);
    RDNA3Accel *a = gVramOwner;
    char *buf = a ? (char *)IOMalloc(cap) : nullptr;
    if (buf) {
        bzero(buf, cap);
        err = a->vramSysctlRead(buf, cap);
        if (!err)
            err = SYSCTL_OUT(req, buf, strlen(buf) + 1);
        IOFree(buf, cap);
    } else if (a) {
        err = ENOMEM;
    }
    OSDecrementAtomic(&gVramSysctlBusy);
    return err;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
SYSCTL_PROC(_debug, OID_AUTO, rdna3_vram, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED, nullptr, 0,
            rdna3_vram_sysctl, "A",
            "card run 2026-10-17c: CPU-visible VRAM by owner, refusals, free lists (key=value; "
            "tools/triage_reports.py 'VISIBLE VRAM')");
#pragma clang diagnostic pop

void RDNA3Accel::vramSysctlRegister()
{
    if (fVramSysctl)
        return;
    gVramOwner = this;
    sysctl_register_oid(&sysctl__debug_rdna3_vram);
    fVramSysctl = true;
}

void RDNA3Accel::vramSysctlUnregister()
{
    if (!fVramSysctl)
        return;
    sysctl_unregister_oid(&sysctl__debug_rdna3_vram);
    gVramOwner = nullptr;
    fVramSysctl = false;
    int i = 0;
    for (; i < 200 && gVramSysctlBusy; i++)   /* a read in flight finishes first */
        IOSleep(10);
    if (gVramSysctlBusy)
        LOG("VRAM: debug.rdna3_vram was still being read after 2 s; going on");
}

/* RD3K_VRAM_FN_REF: the accelerator, still holding the surface's vidmem,
 * lends its allocation to a GEM BO. Only a live handle of ours is taken. */
IOReturn RDNA3Accel::platformVramRef(void *handle)
{
    IOReturn r = kIOReturnNotFound;
    if (!handle || !fDrmLock)
        return kIOReturnBadArgument;
    IOLockLock(fDrmLock);
    for (RDNA3VramHandle *h = fVramHandles; h; h = h->next)
        if (h == handle && h->refs) {
            h->refs++;
            r = kIOReturnSuccess;
            break;
        }
    IOLockUnlock(fDrmLock);
    return r;
}

/* RD3K_VRAM_FN_REFS (DRIVER-PLAN N41, security review): how many hold an
 * accelerator allocation - 1 the accelerator only, more a GEM BO too. The
 * accelerator gives a parked allocation back under VRAM pressure only when
 * no BO holds it. */
IOReturn RDNA3Accel::platformVramRefs(void *handle, unsigned int *refs)
{
    IOReturn r = kIOReturnNotFound;
    if (!handle || !refs || !fDrmLock)
        return kIOReturnBadArgument;
    *refs = 0;
    IOLockLock(fDrmLock);
    for (RDNA3VramHandle *h = fVramHandles; h; h = h->next)
        if (h == handle && h->refs) {
            *refs = h->refs;
            r = kIOReturnSuccess;
            break;
        }
    IOLockUnlock(fDrmLock);
    return r;
}

/* the accelerator kext's service, for the IOSurface lookups it answers
 * (RD3ACCEL_SS_FN); it may start after the first client opens. fDrmLock
 * held; the caller gets its own reference (the lock is dropped around the
 * call, and another thread may forget an inactive service meanwhile). */
IOService *RDNA3Accel::accelService()
{
    if (fAccelSvc && fAccelSvc->isInactive()) {   /* the accelerator kext went away or restarted */
        LOG("GEM: the accelerator service went inactive - looking it up again");
        OSSafeReleaseNULL(fAccelSvc);
    }
    if (!fAccelSvc) {
        OSDictionary *m = IOService::serviceMatching("RDNA3Accelerator");
        if (m) {
            fAccelSvc = IOService::copyMatchingService(m);   /* retained */
            m->release();
        }
    }
    if (fAccelSvc)
        fAccelSvc->retain();
    return fAccelSvc;
}

/* the accelerator's RD3K_SS_* answer as the ioctl's errno; each cause has
 * its own so the client (and the log) can tell them apart */
static int surfaceStatusErrno(unsigned int st)
{
    switch (st) {
    case 0:                   return 0;
    case RD3K_SS_NO_SURFACE:  return -RD_ENOENT;   /* no such surface for this task */
    case RD3K_SS_NO_CACHE:
    case RD3K_SS_NO_VRAM:     return -RD_ENODEV;   /* the accelerator has no VRAM for it (yet) */
    case RD3K_SS_DISPLAY:     return -RD_EPERM;    /* a display framebuffer: kept out of A3 (card run 58) */
    case RD3K_SS_POLICY:      return -RD_EPERM;    /* WindowServer's inputs / rdna3a3=0: kept out of A3 */
    case RD3K_SS_BUSY:        return -RD_EBUSY;    /* N41: a TRYLOCK dirty mark found the accelerator's lock taken */
    default:                  return -RD_ENXIO;    /* accelerator off / not up / other ABI */
    }
}

static const char *surfaceStatusName(unsigned int st)
{
    switch (st) {
    case 0:                   return "ok";
    case RD3K_SS_BAD_VERSION: return "accelerator kext of another ABI";
    case RD3K_SS_OFF:         return "accelerator not up";
    case RD3K_SS_NO_SURFACE:  return "no such surface for this process";
    case RD3K_SS_NO_CACHE:    return "the accelerator never drew it";
    case RD3K_SS_NO_VRAM:     return "no VRAM behind it";
    case RD3K_SS_TOO_SMALL:   return "too small";
    case RD3K_SS_DISPLAY:     return "WindowServer's display framebuffer, kept out of A3 (rdna3a3display=1 lets it in)";
    case RD3K_SS_POLICY:      return "kept out of A3: a WindowServer input (rdna3a3ws=1 lets it in), or rdna3a3=0";
    case RD3K_SS_BUSY:        return "the accelerator's lock was taken (a try-lock dirty mark, N41)";
    default:                  return "?";
    }
}

/* the first refusals of each cause, then one in 1024 */
static void surfaceRefusedLog(const char *what, uint32_t surfaceId, uint32_t plane, unsigned int st, IOReturn rr)
{
    static SInt32 gRefused[8];
    unsigned i = st == 0 ? 0 : (st & 7u);
    SInt32 n = OSIncrementAtomic(&gRefused[i]);
    if (n < 6 || (n & 1023) == 0)
        LOG("GEM: IOSurface %u plane %u %s refused: %s (status %#x, call %#x, %d of this cause)", surfaceId, plane,
            what, rr != kIOReturnSuccess ? "the accelerator did not answer" : surfaceStatusName(st), st, rr,
            (int)n + 1);
}

/* RD3ACCEL_SS_FN 'cmd' on the accelerator; fDrmLock held and dropped
 * around the call (the accelerator takes its family lock, then ours).
 * Returns the IOReturn of the call; *svc false: no accelerator service. */
IOReturn RDNA3Accel::accelSurfaceCall(void *arg, void *fileOs, unsigned cmd, bool *svc)
{
    IOService *acc = accelService();
    *svc = acc != nullptr;
    if (!acc)
        return kIOReturnNotAttached;
    /* made once: the dirty mark comes every frame */
    if (!fAccelSsSym)
        fAccelSsSym = OSSymbol::withCString(RD3ACCEL_SS_FN);
    const OSSymbol *sym = fAccelSsSym;
    if (!sym) {
        acc->release();
        return kIOReturnNoMemory;
    }
    IOLockUnlock(fDrmLock);
    IOReturn rr = acc->callPlatformFunction(sym, false, arg, fileOs, (void *)(uintptr_t)cmd, nullptr);
    IOLockLock(fDrmLock);
    acc->release();
    return rr;
}

/* ops.surface_bo, fDrmLock held */
int RDNA3Accel::drmSurfaceBo(void *ctx, void *fileOs, rdna3_bo *bo, uint32_t surfaceId, uint32_t plane)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    RD3KSurfGem g;
    bool svc;
    if (!fileOs)
        return -RD_ENXIO;
    bzero(&g, sizeof(g));
    g.version = RD3K_SURFGEM_VERSION;
    g.surfaceID = surfaceId;
    g.plane = plane;
    g.status = RD3K_SS_OFF;
    IOReturn rr = a->accelSurfaceCall(&g, fileOs, RD3K_ESC_SURF_GEM, &svc);
    if (!svc) {
        static SInt32 gNoSvc;
        if (OSIncrementAtomic(&gNoSvc) < 4)
            LOG("GEM: IOSurface %u refused: no RDNA3Accelerator service (accelerator kext not started)", surfaceId);
        return -RD_ENXIO;
    }
    if (rr != kIOReturnSuccess || g.status || !g.kapiMemory) {
        surfaceRefusedLog("import", surfaceId, plane, g.status, rr);
        if (rr != kIOReturnSuccess)
            return -RD_ENXIO;
        return g.status ? surfaceStatusErrno(g.status) : -RD_ENODEV;
    }
    RDNA3VramHandle *h = (RDNA3VramHandle *)g.kapiMemory;
    bo->vram_offset = h->offset;
    bo->size = h->size;
    bo->backing = h;   /* the reference rdna3RefVram took; drmBoFree drops it */
    static SInt32 gSurfBoLog;
    SInt32 n = OSIncrementAtomic(&gSurfBoLog);
    if (n < 8 || (n & 4095) == 0)
        LOG("GEM: IOSurface %u plane %u -> %llu KiB of the accelerator's VRAM at 0x%llx (A3, import %d)", surfaceId,
            plane, (unsigned long long)(h->size >> 10), (unsigned long long)h->offset, (int)n + 1);
    return 0;
}

/* ops.surface_dirty, fDrmLock held: after a GPU write through such a BO */
int RDNA3Accel::drmSurfaceDirty(void *ctx, void *fileOs, uint32_t surfaceId, uint32_t plane)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    RD3KSurfShareEsc e;
    bool svc;
    if (!fileOs)
        return -RD_ENXIO;
    bzero(&e, sizeof(e));
    e.version = RD3K_SURFSHARE_VERSION;
    e.surfaceID = surfaceId;
    e.plane = plane;
    e.status = RD3K_SS_OFF;
    IOReturn rr = a->accelSurfaceCall(&e, fileOs, RD3K_ESC_SURF_DIRTY, &svc);
    if (!svc || rr != kIOReturnSuccess)
        return -RD_ENXIO;
    /* surface 0 is the Metal plugin's probe for the ioctl: not a refusal. A
     * try-lock dirty mark that found the lock taken is expected (N41: the
     * mark taken at commit stands), counted by the accelerator */
    if (e.status && surfaceId && e.status != RD3K_SS_BUSY)
        surfaceRefusedLog((plane & RD3K_SS_PLANE_GPU_MARK) ? "GPU pending/ready mark" : "dirty mark", surfaceId,
                          plane & ~(RD3K_SS_PLANE_GPU_MARK | RD3K_SS_PLANE_TRYLOCK), e.status, rr);
    return surfaceStatusErrno(e.status);
}

IOReturn RDNA3Accel::platformGpuVa(int op, RD3KGpuVaRequest *g)
{
    if (!g || g->version != RD3K_GPUVA_ABI_VERSION || !fAccelVa || !fDrmLock)
        return kIOReturnBadArgument;
    g->status = 0;
    switch (op) {
    case 0: { /* alloc */
        uint64_t align = g->align > 65536 ? g->align : 65536;
        uint64_t size = (g->size + 65535) & ~65535ull;
        if (!size)
            return kIOReturnBadArgument;
        IOLockLock(fDrmLock);
        uint64_t va = range_alloc_get(fAccelVa, size, align);
        IOLockUnlock(fDrmLock);
        if (va == RANGE_ALLOC_FAIL) {
            g->status = -1;
            return kIOReturnNoSpace;
        }
        g->gpuva = va;
        g->size = size;
        g->hVirt = 1; /* non-zero: "this map has a range" */
        return kIOReturnSuccess;
    }
    case 1: { /* map: linear PTEs for the VRAM allocation at g->gpuva */
        const RDNA3VramHandle *h = (const RDNA3VramHandle *)g->kapiMemory;
        if (!g->gpuva || !h || (g->gpuva & 4095))
            return kIOReturnBadArgument;
        uint64_t size = g->size ? ((g->size + 4095) & ~4095ull) : h->size;
        if (size > h->size)
            size = h->size;
        IOLockLock(fDrmLock);
        if (fAccelVmStopping) {   /* the kext stops: no VM to map into */
            IOLockUnlock(fDrmLock);
            g->status = -RD_ENODEV;
            return kIOReturnNotReady;
        }
        if (fBringup && (!fAccelVm || !fBringup->vmLive(fAccelVm))) {
            /* made before the GMC was up: record-only, make a real one
             * (platformVmReady normally made it when the GMC came up) */
            if (fAccelVm)
                fBringup->vmDestroy(fAccelVm);
            fAccelVm = fBringup->vmCreate(fAccelVmRefusalLogged, true);
            if (!fAccelVm && !fAccelVmRefusalLogged) {
                LOG("accelerator: no VMID or page tables for its VM: IOSurface GPU maps fail until one "
                    "is free (logged once)");
                fAccelVmRefusalLogged = true;
            } else if (fAccelVm && fBringup->vmLive(fAccelVm)) {
                if (fAccelVmRefusalLogged)
                    LOG("accelerator: its VM has a VMID again");
                fAccelVmRefusalLogged = false;   /* a later failure logs again */
            }
        }
        /* with the GMC up and no VMID or page tables left there is no VM
         * (vmCreate logged it): the map fails instead of doing nothing */
        int r = (fBringup && !fAccelVm) ? -RD_ENOSPC : 0;
        if (fBringup && fAccelVm && fBringup->vmLive(fAccelVm)) {
            rdna3_bo bo;
            bzero(&bo, sizeof(bo));
            bo.size = h->size;
            bo.domains = bo.placement = AMDGPU_GEM_DOMAIN_VRAM;
            bo.vram_offset = h->offset;
            r = fBringup->vmMap(fAccelVm, &bo, g->gpuva, 0, size,
                                AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE);
        }
        IOLockUnlock(fDrmLock);
        if (r) {
            g->status = r;
            return kIOReturnNoMemory;
        }
        g->hMemory = 1;
        return kIOReturnSuccess;
    }
    case 2: /* free (or unmap only) */
        if (!g->gpuva || !g->size)
            return (g->flags & RD3K_GPUVA_FLAG_UNMAP_ONLY) ? kIOReturnSuccess : kIOReturnBadArgument;
        IOLockLock(fDrmLock);
        if (fBringup && fAccelVm)
            fBringup->vmUnmap(fAccelVm, g->gpuva, (g->size + 4095) & ~4095ull);
        if (!(g->flags & RD3K_GPUVA_FLAG_UNMAP_ONLY))
            range_alloc_put(fAccelVa, g->gpuva, (g->size + 65535) & ~65535ull);
        IOLockUnlock(fDrmLock);
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

/* IOSurface page-off: copy the allocation's bytes into the caller's wired
 * system pages. Like NVIDIA's copy-engine job, SDMA0 does it when the GMC
 * and the DMA ring are up (the pages go into a GART window, amdgpu's
 * amdgpu_ttm_map_buffer pattern); otherwise the CPU reads VRAM through the
 * BAR mapping (uncached, slow but exact). */
IOReturn RDNA3Accel::platformPageoff(RD3KSurfPageoff *q)
{
    if (!q || q->version != RD3K_SS_RM_VERSION) {
        if (q) q->status = RD3K_SS_BAD_VERSION;
        return kIOReturnBadArgument;
    }
    const RDNA3VramHandle *h = (const RDNA3VramHandle *)q->kapiMemory;
    if (!h || !fVramMap) {
        q->status = RD3K_SS_NO_VRAM;
        return kIOReturnSuccess;
    }
    if (!q->pages || q->length > h->size || q->pageCount * 4096 < q->length) {
        q->status = RD3K_SS_TOO_SMALL;
        return kIOReturnSuccess;
    }
    uint64_t t0 = mach_absolute_time(), ns = 0;
    if (fBringup && fDrmLock) {
        IOLockLock(fDrmLock);
        /* re-checked under the lock: stop takes fBringup out under it */
        int r = fBringup ? fBringup->copyVramToPages(h->offset, q->pages, q->pageCount, q->length) : RDNA3_ENODEV;
        IOLockUnlock(fDrmLock);
        if (r == RDNA3_OK) {
            absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
            q->usec = ns / 1000;
            q->status = 0;
            return kIOReturnSuccess;
        }
        if (r != RDNA3_ENODEV)
            LOG("pageoff: SDMA copy failed (%d), the CPU does it", r);
    }
    const uint8_t *src = (const uint8_t *)(fVramMap->getVirtualAddress() + h->offset);
    uint64_t done = 0;
    for (uint64_t i = 0; i < q->pageCount && done < q->length; i++) {
        uint64_t n = q->length - done < 4096 ? q->length - done : 4096;
        /* task NULL: the range is a physical address (64-bit safe) */
        IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
            (mach_vm_address_t)q->pages[i], (mach_vm_size_t)n, kIODirectionIn | kIOMemoryMapperNone,
            nullptr);
        if (!md) {
            q->status = RD3K_SS_OFF;
            return kIOReturnNoMemory;
        }
        IOByteCount w = md->writeBytes(0, src + done, (IOByteCount)n);
        md->release();
        if (w != n) {
            q->status = RD3K_SS_TOO_SMALL;
            return kIOReturnSuccess;
        }
        done += n;
    }
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    q->usec = ns / 1000;
    q->status = 0;
    return kIOReturnSuccess;
}

IOReturn RDNA3Accel::platformCopyVram(RD3KCopyRequest *q)
{
    if (!q || q->version != RD3K_COPY_ABI_VERSION)
        return kIOReturnBadArgument;
    if (!fBringup || !fDrmLock) {
        q->status = RDNA3_ENODEV;
        return kIOReturnNotReady;
    }
    uint64_t t0 = mach_absolute_time(), ns = 0;
    /* the source was written through the write-combined BAR mapping:
     * drained before the engine reads it (the HDP flush is the ring's) */
    __asm__ volatile("mfence" ::: "memory");
    IOLockLock(fDrmLock);
    /* re-checked under the lock: stop takes fBringup out under it */
    int r = fBringup ? fBringup->copyVramRect(q->srcCookie, q->srcPitch, q->dstCookie, q->dstPitch, q->rowBytes, q->rows)
                     : RDNA3_ENODEV;
    IOLockUnlock(fDrmLock);
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    q->usec = ns / 1000;
    q->status = r;
    if (r != RDNA3_OK && fCopyVramFails++ < 8)
        LOG("copy: %ux%u bytes 0x%llx (pitch %u) -> 0x%llx (pitch %u) by SDMA failed (%d)", q->rowBytes, q->rows,
            q->srcCookie, q->srcPitch, q->dstCookie, q->dstPitch, r);
    return r == RDNA3_OK ? kIOReturnSuccess : kIOReturnIOError;
}

/* DRIVER-PLAN N49: the kernel rings' busy time and the GPU time per process
 * since the last take, for the accelerator's PerformanceStatistics and its
 * contexts' accumulatedGPUTime (Activity Monitor) */
IOReturn RDNA3Accel::platformGpuBusy(RD3KGpuBusy *q)
{
    rd3_busy_snap s;
    rd3_busy_take_ent t[RD3K_GPUBUSY_PIDS];
    if (!q || q->version != RD3K_GPUBUSY_VERSION)
        return kIOReturnBadArgument;
    q->npids = 0;
    if (!fDrm || !fBringup || !fDrmLock)
        return kIOReturnNotReady;   /* no GPU (yet): not 'an older core' (ECC round 1) */
    IOLockLock(fDrmLock);
    const uint32_t n = fBringup ? fBringup->busyTake(&s, t, RD3K_GPUBUSY_PIDS) : 0;
    const bool live = fBringup && s.now != 0;
    if (live)
        fBringup->busyCounts(q->uncounted, q->sdmaEnds);
    IOLockUnlock(fDrmLock);
    if (!live)
        return kIOReturnNotReady;   /* no busy accounting (no memory at start): the accelerator logs it */
    q->nowNs = s.now;
    q->busyNs = s.any_ns;
    for (uint32_t r = 0; r < RD3K_GPUBUSY_RINGS && r < RD3_BUSY_RINGS; r++)
        q->ringNs[r] = s.ring_ns[r];
    q->jobs = s.jobs;
    q->merged = s.merged;
    q->flushed = s.flushed;
    q->stale = s.stale;
    q->lostJobs = s.lost_jobs;
    q->lostNs = s.lost_ns;
    q->inflight = s.inflight;
    q->cappedNs = s.capped_ns;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = 0;
        nanoseconds_to_absolutetime(t[i].last_sub, &abs);
        q->pids[i].pid = t[i].pid;
        q->pids[i].jobs = t[i].jobs;
        q->pids[i].gpuNs = t[i].ns;
        q->pids[i].lastSubmitAbs = abs;
    }
    q->npids = n;
    return kIOReturnSuccess;
}

IOReturn RDNA3Accel::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2,
                                          void *p3, void *p4)
{
    if (fn) {
        if (fn->isEqualTo(RD3K_DISPLAY_FN_INFO))
            return displayInfo((RD3KDisplayInfo *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_FLIP))
            return displayFlip((RD3KDisplayFlip *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_STATUS))
            return displayStatus((RD3KDisplayStatus *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_SCANADDR))
            return displayScanAddr((RD3KDisplayScanAddr *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_CURSOR))
            return displayCursor((RD3KDisplayCursor *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_EDID))
            return displayEdid((RD3KDisplayEdid *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_SOURCE))
            return displaySetSource((RD3KDisplaySource *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_MODES))
            return displayModes((RD3KDisplayModes *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_MODE)) {
            const IOReturn r = displaySetMode((RD3KDisplayMode *)p1);
            if (r == kIOReturnSuccess)   /* a head a wake / reset left down is lit again */
                displayIrqHeadBack(((RD3KDisplayMode *)p1)->head, "a mode set");
            displayIrqSync("mode set", true);   /* N42: the head's sources as registered, re-written */
            return r;
        }
        if (fn->isEqualTo(RD3K_DISPLAY_FN_IRQ))
            return displayIrqCall((RD3KDisplayIrq *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_BLANK))
            return displayBlank((uint32_t)(uintptr_t)p1, p2 != nullptr);
        if (fn->isEqualTo(RD3K_FN_DISPLAY_CRC))
            return displayCrc((uint32_t)(uintptr_t)p1, (uint32_t *)p2);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_HOTPLUG))
            return displayHotplugQuery((RD3KDisplayHotplug *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_GAMMA))
            return displayGamma((RD3KDisplayGamma *)p1);
    }
    /* before the fDrm gate: without the DRM it answers NotReady, not
     * IOService's Unsupported ('an older core', ECC round 1) */
    if (fn && fn->isEqualTo(RD3K_GPUBUSY_FN))
        return platformGpuBusy((RD3KGpuBusy *)p1);
    if (fn && fDrm) {
        if (fn->isEqualTo(RD3K_VRAM_FN_ALLOC))
            return platformVramAlloc((RD3KVramRequest *)p1);
        if (fn->isEqualTo(RD3K_VRAM_FN_FREE))
            return platformVramFree((RD3KVramRequest *)p1);
        if (fn->isEqualTo(RD3K_VRAM_FN_REF))
            return platformVramRef(p1);
        if (fn->isEqualTo(RD3K_VRAM_FN_REFS))
            return platformVramRefs(p1, (unsigned int *)p2);
        if (fn->isEqualTo(RD3K_GPUVA_FN_HANDLES)) {
            RD3KRmHandles *h = (RD3KRmHandles *)p1;
            if (!h || h->version != RD3K_GPUVA_ABI_VERSION)
                return kIOReturnBadArgument;
            h->kapiDevice = this;
            h->hClient = h->hDevice = h->hSubDevice = 0;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(RD3K_GPUVA_FN_ALLOC))
            return platformGpuVa(0, (RD3KGpuVaRequest *)p1);
        if (fn->isEqualTo(RD3K_GPUVA_FN_MAP))
            return platformGpuVa(1, (RD3KGpuVaRequest *)p1);
        if (fn->isEqualTo(RD3K_GPUVA_FN_FREE))
            return platformGpuVa(2, (RD3KGpuVaRequest *)p1);
        if (fn->isEqualTo(RD3K_FN_NEW_CLIENT)) {
            /* p1 the task, p2 the type's low 16 bits, p3 IOUserClient ** */
            IOUserClient **out = (IOUserClient **)p3;
            if (!out || !p1)
                return kIOReturnBadArgument;
            *out = nullptr;
            if (!drmAvailable())
                return kIOReturnNotReady;
            IOAccelRDNA3Bridge *uc = OSTypeAlloc(IOAccelRDNA3Bridge);
            if (!uc)
                return kIOReturnNoMemory;
            uc->setOwner(this);
            if (!uc->initWithTask((task_t)p1, nullptr, (UInt32)(uintptr_t)p2)) {
                uc->release();
                return kIOReturnNoResources;
            }
            *out = uc;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(RD3K_SS_FN_PAGEOFF))
            return platformPageoff((RD3KSurfPageoff *)p1);
        if (fn->isEqualTo(RD3K_COPY_FN))
            return platformCopyVram((RD3KCopyRequest *)p1);
    }
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}
