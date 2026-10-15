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
};

/* the accelerator's live VRAM allocations (the eviction list before a
 * sleep); fDrmLock held */
void RDNA3Accel::platformForEachVram(void (*fn)(void *ctx, uint64_t off, uint64_t size), void *ctx)
{
    for (RDNA3VramHandle *h = fVramHandles; h; h = h->next)
        fn(ctx, h->offset, h->size);
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
    if (!fVramHeap || !fVramMap || !fVramDev || !fDrmLock)
        return kIOReturnNotReady;

    uint64_t size = (r->size + 4095) & ~4095ull;
    RDNA3VramHandle *h = (RDNA3VramHandle *)IOMalloc(sizeof(*h));
    if (!h)
        return kIOReturnNoMemory;

    IOLockLock(fDrmLock);
    uint64_t off = range_alloc_get(fVramHeap, size, 65536);
    /* the accelerator touches it through kva: it must be CPU visible */
    if (off != RANGE_ALLOC_FAIL && off + size > fVramMap->getLength()) {
        range_alloc_put(fVramHeap, off, size);
        off = RANGE_ALLOC_FAIL;
    }
    if (off != RANGE_ALLOC_FAIL) {
        fAccelVramBytes += size;
        h->offset = off;
        h->size = size;
        h->refs = 1;
        h->prev = nullptr;
        h->next = fVramHandles;
        if (fVramHandles)
            fVramHandles->prev = h;
        fVramHandles = h;
    }
    uint64_t total = fAccelVramBytes;
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
    if (!h || !h->refs || --h->refs)
        return;
    range_alloc_put(fVramHeap, h->offset, h->size);
    fAccelVramBytes -= h->size;
    if (h->prev)
        h->prev->next = h->next;
    else
        fVramHandles = h->next;
    if (h->next)
        h->next->prev = h->prev;
    IOFree(h, sizeof(*h));
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

/* the accelerator kext's service, for the IOSurface lookups it answers
 * (RD3ACCEL_SS_FN); it may start after the first client opens */
IOService *RDNA3Accel::accelService()
{
    if (fAccelSvc && fAccelSvc->isInactive())   /* the accelerator kext went away or restarted */
        OSSafeReleaseNULL(fAccelSvc);
    if (!fAccelSvc) {
        OSDictionary *m = IOService::serviceMatching("RDNA3Accelerator");
        if (m) {
            fAccelSvc = IOService::copyMatchingService(m);   /* retained */
            m->release();
        }
    }
    return fAccelSvc;
}

static int surfaceStatusErrno(unsigned int st)
{
    switch (st) {
    case 0:                   return 0;
    case RD3K_SS_NO_SURFACE:  return -RD_ENOENT;
    case RD3K_SS_BAD_VERSION: return -RD_EINVAL;
    default:                  return -RD_ENODEV;   /* no vidmem yet, accelerator not up */
    }
}

/* ops.surface_bo, fDrmLock held: the accelerator takes its own family lock
 * and then ours (rdna3RefVram), so ours is dropped around the call */
int RDNA3Accel::drmSurfaceBo(void *ctx, void *fileOs, rdna3_bo *bo, uint32_t surfaceId, uint32_t plane)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    IOService *acc = a->accelService();
    if (!acc || !fileOs)
        return -RD_ENODEV;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3ACCEL_SS_FN);
    if (!sym)
        return -RD_ENOMEM;
    RD3KSurfGem g;
    bzero(&g, sizeof(g));
    g.version = RD3K_SURFGEM_VERSION;
    g.surfaceID = surfaceId;
    g.plane = plane;
    g.status = RD3K_SS_OFF;
    IOLockUnlock(a->fDrmLock);
    IOReturn rr = acc->callPlatformFunction(sym, false, &g, fileOs, (void *)(uintptr_t)RD3K_ESC_SURF_GEM, nullptr);
    IOLockLock(a->fDrmLock);
    sym->release();
    if (rr != kIOReturnSuccess)
        return -RD_ENODEV;
    if (g.status || !g.kapiMemory)
        return g.status ? surfaceStatusErrno(g.status) : -RD_ENODEV;
    RDNA3VramHandle *h = (RDNA3VramHandle *)g.kapiMemory;
    bo->vram_offset = h->offset;
    bo->size = h->size;
    bo->backing = h;   /* the reference rdna3RefVram took; drmBoFree drops it */
    static SInt32 gSurfBoLog;
    if (OSIncrementAtomic(&gSurfBoLog) < 8)
        LOG("GEM: IOSurface %u plane %u -> %llu KiB of the accelerator's VRAM at 0x%llx (A3)", surfaceId, plane,
            (unsigned long long)(h->size >> 10), (unsigned long long)h->offset);
    return 0;
}

/* ops.surface_dirty: after a GPU write through such a BO */
int RDNA3Accel::drmSurfaceDirty(void *ctx, void *fileOs, uint32_t surfaceId, uint32_t plane)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    IOService *acc = a->accelService();
    if (!acc || !fileOs)
        return -RD_ENODEV;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3ACCEL_SS_FN);
    if (!sym)
        return -RD_ENOMEM;
    RD3KSurfShareEsc e;
    bzero(&e, sizeof(e));
    e.version = RD3K_SURFSHARE_VERSION;
    e.surfaceID = surfaceId;
    e.plane = plane;
    e.status = RD3K_SS_OFF;
    IOLockUnlock(a->fDrmLock);
    IOReturn rr = acc->callPlatformFunction(sym, false, &e, fileOs, (void *)(uintptr_t)RD3K_ESC_SURF_DIRTY, nullptr);
    IOLockLock(a->fDrmLock);
    sym->release();
    return rr != kIOReturnSuccess ? -RD_ENODEV : surfaceStatusErrno(e.status);
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
        if (fBringup && (!fAccelVm || !fBringup->vmLive(fAccelVm))) {
            /* made before the GMC was up: record-only, make a real one */
            if (fAccelVm)
                fBringup->vmDestroy(fAccelVm);
            fAccelVm = fBringup->vmCreate();
        }
        int r = 0;
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
        int r = fBringup->copyVramToPages(h->offset, q->pages, q->pageCount, q->length);
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
    int r = fBringup->copyVramRect(q->srcCookie, q->srcPitch, q->dstCookie, q->dstPitch, q->rowBytes, q->rows);
    IOLockUnlock(fDrmLock);
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    q->usec = ns / 1000;
    q->status = r;
    if (r != RDNA3_OK && fCopyVramFails++ < 8)
        LOG("copy: %ux%u bytes 0x%llx (pitch %u) -> 0x%llx (pitch %u) by SDMA failed (%d)", q->rowBytes, q->rows,
            q->srcCookie, q->srcPitch, q->dstCookie, q->dstPitch, r);
    return r == RDNA3_OK ? kIOReturnSuccess : kIOReturnIOError;
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
        if (fn->isEqualTo(RD3K_DISPLAY_FN_CURSOR))
            return displayCursor((RD3KDisplayCursor *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_EDID))
            return displayEdid((RD3KDisplayEdid *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_SOURCE))
            return displaySetSource((RD3KDisplaySource *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_MODES))
            return displayModes((RD3KDisplayModes *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_MODE))
            return displaySetMode((RD3KDisplayMode *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_BLANK))
            return displayBlank((uint32_t)(uintptr_t)p1, p2 != nullptr);
        if (fn->isEqualTo(RD3K_FN_DISPLAY_CRC))
            return displayCrc((uint32_t)(uintptr_t)p1, (uint32_t *)p2);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_HOTPLUG))
            return displayHotplugQuery((RD3KDisplayHotplug *)p1);
        if (fn->isEqualTo(RD3K_DISPLAY_FN_GAMMA))
            return displayGamma((RD3KDisplayGamma *)p1);
    }
    if (fn && fDrm) {
        if (fn->isEqualTo(RD3K_VRAM_FN_ALLOC))
            return platformVramAlloc((RD3KVramRequest *)p1);
        if (fn->isEqualTo(RD3K_VRAM_FN_FREE))
            return platformVramFree((RD3KVramRequest *)p1);
        if (fn->isEqualTo(RD3K_VRAM_FN_REF))
            return platformVramRef(p1);
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
