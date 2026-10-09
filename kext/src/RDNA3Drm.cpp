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
#include "RDNA3Bringup.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <pexpert/pexpert.h>

#include <libkern/copyio.h>   /* copyin/copyout (kpi.libkern) */

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

/* GPUVM hooks: forward to the bring-up, which owns the hardware state */
void *RDNA3Accel::drmVmCreate(void *ctx)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (a->fBringup)
        return a->fBringup->vmCreate();
    static int recordOnly;   /* no bring-up (rdna3lvl < 4): any non-NULL token */
    return &recordOnly;
}

void RDNA3Accel::drmVmDestroy(void *ctx, void *vm)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (a->fBringup)
        a->fBringup->vmDestroy(vm);
}

int RDNA3Accel::drmVmMap(void *ctx, void *vm, rdna3_bo *bo, uint64_t va, uint64_t off,
                         uint64_t size, uint32_t flags)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->vmMap(vm, bo, va, off, size, flags) : 0;
}

void RDNA3Accel::drmVmUnmap(void *ctx, void *vm, uint64_t va, uint64_t size)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (a->fBringup)
        a->fBringup->vmUnmap(vm, va, size);
}

int RDNA3Accel::drmSubmit(void *ctx, void *vm, uint32_t ip, uint32_t ring, const rdna3_ib *ibs,
                          uint32_t n, uint64_t *seq)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->submit(vm, ip, ring, ibs, n, seq) : -RD_ENODEV;
}

int RDNA3Accel::drmFenceSignaled(void *ctx, uint32_t ip, uint32_t ring, uint64_t seq)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->fenceSignaled(ip, ring, seq) : 1;
}

int RDNA3Accel::drmFenceWait(void *ctx, uint32_t ip, uint32_t ring, uint64_t seq, uint64_t absNs)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->fenceWait(ip, ring, seq, absNs) : 0;
}

void RDNA3Accel::drmRingReady(uint32_t ip, uint32_t mask)
{
    if (!fDrm || !fDrmLock)
        return;
    IOLockLock(fDrmLock);
    rdna3_drm_device_set_ring_ready(fDrm, ip, mask);
    IOLockUnlock(fDrmLock);
    LOG("HW IP %u ring mask 0x%x ready for command submission", ip, mask);
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
    ops.vm_create = drmVmCreate;
    ops.vm_destroy = drmVmDestroy;
    ops.vm_map = drmVmMap;
    ops.vm_unmap = drmVmUnmap;
    ops.submit = drmSubmit;
    ops.fence_signaled = drmFenceSignaled;
    ops.fence_wait = drmFenceWait;
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
    /* GB_ADDR_CONFIG (gfx_v11_0 get_gb_addr_config): read once, cache.
     * Only at rdna3lvl>=3, the level that already reads GC registers. */
    if (fLevel >= 3 && fDrm->gc_base0 && fMmio) {
        uint32_t off = (fDrm->gc_base0 + 0x13de) * 4;
        if (off + 4 <= fMmioLen)
            fDrm->gb_addr_config = *(volatile uint32_t *)(fMmio + off);
    }
    if (!fDrm->gb_addr_config)
        fDrm->gb_addr_config = 0x00000545; /* TODO: verify on hardware */
    setProperty("RDNA3,GBAddrConfig", fDrm->gb_addr_config, 32);

    uint32_t enumOnly = 0;
    if (PE_parse_boot_argn("rdna3enum", &enumOnly, sizeof(enumOnly)) && enumOnly) {
        rdna3_drm_device_set_enumeration_only(fDrm, 1);
        LOG("rdna3enum=1: reporting accel_working + rings for enumeration; CS still fails");
    }
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
