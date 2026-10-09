/*
 * RDNA3Platform.cpp - the core kext's answers to the accelerator kext
 * (accel/, IOGraphicsAccelerator2 subclass) over callPlatformFunction.
 * The interface is accel/rdna3k_abi.h.
 *
 *   rdna3AllocVram / rdna3FreeVram   VRAM from the same heap as GEM BOs,
 *                                    CPU-visible through BAR0
 *   rdna3DeviceHandles               device cookie (this)
 *   rdna3GpuVaAlloc / Map / Free     GPU VA ranges for the family's tasks.
 *                                    TODO(stage2-gpuvm): Map writes the
 *                                    task's page tables; today it records
 *                                    the range only (no GPU work runs yet)
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

#include "rdna3k_abi.h"

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
    /* MC address the GPU sees: FB aperture base + offset. TODO(stage2):
     * take fb_start from the GMC once the bring-up publishes it; until then
     * the offset itself, which is what the GMC maps at FB base 0. */
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
    case 1: /* map: TODO(stage2-gpuvm) write PTEs for g->kapiMemory at g->gpuva */
        if (!g->gpuva || !g->kapiMemory)
            return kIOReturnBadArgument;
        return kIOReturnSuccess;
    case 2: /* free (or unmap only) */
        if (g->flags & RD3K_GPUVA_FLAG_UNMAP_ONLY)
            return kIOReturnSuccess;
        if (!g->gpuva || !g->size)
            return kIOReturnBadArgument;
        IOLockLock(fDrmLock);
        range_alloc_put(fAccelVa, g->gpuva, (g->size + 65535) & ~65535ull);
        IOLockUnlock(fDrmLock);
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
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
    }
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}
