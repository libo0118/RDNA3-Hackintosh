/*
 * RDNA3Drm.cpp - RDNA3Accel's side of the DRM/amdgpu UAPI: memory for
 * buffer objects and the glue between RDNA3UserClient and uapi/.
 *
 * GTT BOs: IOBufferMemoryDescriptor (pageable=no, shared with user).
 *          TODO(stage2-gmc): DMA-map them for the GPU (IODMACommand) once
 *          GART exists.
 * VRAM BOs: range in VRAM [512 MiB, VRAM - 256 MiB); CPU mappable only
 *          inside BAR0 (needs Resizable BAR for anything above 256 MiB).
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"
#include "RDNA3Regs.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>

extern "C" {
extern int copyin(const user_addr_t uaddr, void *kaddr, size_t len);
extern int copyout(const void *kaddr, user_addr_t udaddr, size_t len);
}

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

static const uint64_t kUserVramStart = 512ull << 20;
static const uint64_t kUserVramTopReserve = 256ull << 20;

void *RDNA3Accel::drmZalloc(void *, size_t n)
{
    void *p = IOMalloc(n);
    if (p)
        bzero(p, n);
    return p;
}

void RDNA3Accel::drmFreeMem(void *, void *p, size_t n)
{
    if (p)
        IOFree(p, n);
}

int RDNA3Accel::drmBoAlloc(void *ctx, rdna3_bo *bo)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        if (!a->fVramHeap)
            return -RD_ENOMEM;
        uint64_t off = range_alloc_get(a->fVramHeap, bo->size,
                                       bo->alignment > 4096 ? bo->alignment : 4096);
        if (off == RANGE_ALLOC_FAIL)
            return -RD_ENOMEM;
        bo->vram_offset = off;
        bo->backing = nullptr;
        /* AMDGPU_GEM_CREATE_VRAM_CLEARED: clear through BAR0 when visible */
        if ((bo->flags & AMDGPU_GEM_CREATE_VRAM_CLEARED) && a->fVramMap &&
            off + bo->size <= a->fVramMap->getLength())
            bzero((void *)(a->fVramMap->getVirtualAddress() + off), (size_t)bo->size);
        return 0;
    }
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, bo->size, 4096);
    if (!md)
        return -RD_ENOMEM;
    bzero(md->getBytesNoCopy(), (size_t)bo->size);
    bo->backing = md;
    return 0;
}

void RDNA3Accel::drmBoFree(void *ctx, rdna3_bo *bo)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        if (a->fVramHeap)
            range_alloc_put(a->fVramHeap, bo->vram_offset, bo->size);
    } else if (bo->backing) {
        ((IOBufferMemoryDescriptor *)bo->backing)->release();
        bo->backing = nullptr;
    }
}

int RDNA3Accel::drmCopyin(void *, void *dst, uint64_t uaddr, size_t len)
{
    return copyin((user_addr_t)uaddr, dst, len) ? -RD_EFAULT : 0;
}

int RDNA3Accel::drmCopyout(void *, uint64_t uaddr, const void *src, size_t len)
{
    return copyout(src, (user_addr_t)uaddr, len) ? -RD_EFAULT : 0;
}

bool RDNA3Accel::drmInit(const amd_discovery *disc)
{
    rdna3_drm_ops ops = {};
    ops.ctx = this;
    ops.zalloc = drmZalloc;
    ops.free = drmFreeMem;
    ops.bo_alloc = drmBoAlloc;
    ops.bo_free = drmBoFree;
    ops.copyin = drmCopyin;
    ops.copyout = drmCopyout;
    ops.gpu_clock = nullptr; /* TODO(stage2-gfx): RLC GPU clock counter */

    fDrmLock = IOLockAlloc();
    fDrm = (rdna3_drm_device *)drmZalloc(this, sizeof(*fDrm));
    fVramHeap = (range_alloc *)drmZalloc(this, sizeof(*fVramHeap));
    if (!fDrmLock || !fDrm || !fVramHeap) {
        drmFree();
        return false;
    }

    fVramDev = fPci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (fVramDev)
        fVramDev->retain();
    fVramMap = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
                                                 kIOMapWriteCombineCache);
    uint64_t visible = fVramMap ? fVramMap->getLength() : 0;

    if (fVramSize > kUserVramStart + kUserVramTopReserve)
        range_alloc_init(fVramHeap, kUserVramStart,
                         fVramSize - kUserVramStart - kUserVramTopReserve);
    else
        range_alloc_init(fVramHeap, 0, 0);

    uint64_t gtt = 8ull << 30; /* TODO: from system memory size, like amdgpu */
    uint8_t rev = fPci->configRead8(kIOPCIConfigRevisionID);
    rdna3_drm_device_init(fDrm, &ops, disc, fPci->configRead16(kIOPCIConfigDeviceID), rev,
                          0 /* TODO: NBIO strap rev_id */, fVramSize,
                          visible > fVramSize ? fVramSize : visible, gtt);
    LOG("DRM/amdgpu UAPI ready: VRAM %llu MiB (%llu MiB visible), user heap %llu MiB",
        (unsigned long long)(fVramSize >> 20), (unsigned long long)(visible >> 20),
        (unsigned long long)(range_alloc_free_bytes(fVramHeap) >> 20));
    setProperty("IOUserClientClass", "RDNA3UserClient");
    return true;
}

void RDNA3Accel::drmFree()
{
    if (fDrm)
        drmFreeMem(this, fDrm, sizeof(*fDrm));
    if (fVramHeap)
        drmFreeMem(this, fVramHeap, sizeof(*fVramHeap));
    if (fDrmLock)
        IOLockFree(fDrmLock);
    OSSafeReleaseNULL(fVramMap);
    OSSafeReleaseNULL(fVramDev);
    fDrm = nullptr;
    fVramHeap = nullptr;
    fDrmLock = nullptr;
}

int RDNA3Accel::drmOpen(rdna3_drm_file *f)
{
    IOLockLock(fDrmLock);
    int r = rdna3_drm_file_open(fDrm, f);
    IOLockUnlock(fDrmLock);
    return r;
}

void RDNA3Accel::drmClose(rdna3_drm_file *f)
{
    IOLockLock(fDrmLock);
    rdna3_drm_file_close(f);
    IOLockUnlock(fDrmLock);
}

int RDNA3Accel::drmIoctl(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size)
{
    IOLockLock(fDrmLock);
    int r = rdna3_drm_ioctl(f, nr, data, size);
    IOLockUnlock(fDrmLock);
    return r;
}

IOMemoryDescriptor *RDNA3Accel::drmMemoryForOffset(rdna3_drm_file *f, uint64_t off,
                                                   IOOptionBits *options)
{
    IOMemoryDescriptor *md = nullptr;
    IOLockLock(fDrmLock);
    rdna3_bo *bo = rdna3_drm_bo_by_mmap_offset(f, off);
    if (bo && bo->placement != AMDGPU_GEM_DOMAIN_VRAM && bo->backing) {
        md = (IOBufferMemoryDescriptor *)bo->backing;
        md->retain();
        *options = 0;
    } else if (bo && fVramDev && bo->vram_offset + bo->size <= fVramDev->getLength()) {
        md = IOSubMemoryDescriptor::withSubRange(fVramDev, bo->vram_offset, bo->size,
                                                 kIODirectionInOut);
        *options = kIOMapWriteCombineCache;
    }
    IOLockUnlock(fDrmLock);
    return md;
}
