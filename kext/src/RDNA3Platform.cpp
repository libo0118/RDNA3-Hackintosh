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
 *   rdna3SurfShare                   not offered (no RM-style cross-client
 *                                    handles on AMD; RADV shares by GEM)
 *   rdna3NewAccelClient              not offered: RADV opens RDNA3Accel
 *                                    directly (RDNA3UserClient)
 *
 * Display heads (Stage 3, RDNA3Display.cpp) are answered here for
 * RDNA3Framebuffer; the accelerator's own scanout/flip requests go to the
 * framebuffer, which forwards them.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"

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
};

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
    if (off != RANGE_ALLOC_FAIL)
        fAccelVramBytes += size;
    uint64_t total = fAccelVramBytes;
    IOLockUnlock(fDrmLock);

    if (off == RANGE_ALLOC_FAIL) {
        IOFree(h, sizeof(*h));
        return kIOReturnNoSpace;
    }
    h->offset = off;
    h->size = size;
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
    RDNA3VramHandle *h = (RDNA3VramHandle *)r->handle;
    IOLockLock(fDrmLock);
    range_alloc_put(fVramHeap, h->offset, h->size);
    fAccelVramBytes -= h->size;
    IOLockUnlock(fDrmLock);
    IOFree(h, sizeof(*h));
    r->handle = nullptr;
    return kIOReturnSuccess;
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
        if (fn->isEqualTo(RD3K_DISPLAY_FN_BLANK))
            return displayBlank((uint32_t)(uintptr_t)p1, p2 != nullptr);
        if (fn->isEqualTo(RD3K_FN_DISPLAY_CRC))
            return displayCrc((uint32_t)(uintptr_t)p1, (uint32_t *)p2);
    }
    if (fn && fDrm) {
        if (fn->isEqualTo(RD3K_VRAM_FN_ALLOC))
            return platformVramAlloc((RD3KVramRequest *)p1);
        if (fn->isEqualTo(RD3K_VRAM_FN_FREE))
            return platformVramFree((RD3KVramRequest *)p1);
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
        if (fn->isEqualTo(RD3K_FN_NEW_CLIENT))
            return kIOReturnUnsupported;
        if (fn->isEqualTo(RD3K_SS_FN_PAGEOFF))
            return platformPageoff((RD3KSurfPageoff *)p1);
    }
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}
