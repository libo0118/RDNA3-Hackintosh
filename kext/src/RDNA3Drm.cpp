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
#include "gfx11_info.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODMACommand.h>
#include <pexpert/pexpert.h>

#include <libkern/copyio.h>   /* copyin/copyout (kpi.libkern) */

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)


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
        if (!a->fVramHeap || !a->fVramHeapHi)
            return -RD_ENOMEM;
        /* amdgpu_bo_placement_from_domain: CPU_ACCESS_REQUIRED stays in the
         * visible part; NO_CPU_ACCESS goes outside it first; anything else
         * prefers visible (nothing here migrates a BO on a CPU fault) */
        uint64_t align = bo->alignment > 4096 ? bo->alignment : 4096;
        range_alloc *first = (bo->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS) ? a->fVramHeapHi
                                                                           : a->fVramHeap;
        range_alloc *second = first == a->fVramHeap ? a->fVramHeapHi : a->fVramHeap;
        uint64_t off = range_alloc_get(first, bo->size, align);
        if (off == RANGE_ALLOC_FAIL && !(bo->flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED))
            off = range_alloc_get(second, bo->size, align);
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
    return a->gttAlloc(bo);
}

/* GTT: amdgpu's dma mask for GFX 11 is 44 bits; the pages are wired by
 * the DMA command for the BO's whole life (TTM pins GTT BOs while bound) */
int RDNA3Accel::gttAlloc(rdna3_bo *bo)
{
    const uint64_t mask = (1ull << 44) - 4096;
    uint64_t npages = (bo->size + 4095) / 4096;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, npages * 4096, mask);
    if (!md)
        return -RD_ENOMEM;
    bzero(md->getBytesNoCopy(), (size_t)(npages * 4096));
    return gttWire(bo, md, false);
}

/* amdgpu_gem_userptr_ioctl: the client's own pages become a GTT BO. The
 * descriptor over the task's range is prepared (wired) for as long as the
 * BO lives; the DMA command hands out the bus address of every page. */
int RDNA3Accel::drmBoUserptr(void *ctx, void *fileOs, rdna3_bo *bo, uint64_t uaddr, uint32_t flags)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    task_t task = (task_t)fileOs;
    IOOptionBits dir = (flags & AMDGPU_GEM_USERPTR_READONLY) ? kIODirectionIn : kIODirectionInOut;
    IOMemoryDescriptor *md;
    if (!task)
        return -RD_EINVAL;
    md = IOMemoryDescriptor::withAddressRange(uaddr, bo->size, dir, task);
    if (!md)
        return -RD_EFAULT;
    if (md->prepare() != kIOReturnSuccess) {
        md->release();
        return -RD_EFAULT;
    }
    return a->gttWire(bo, md, true);
}

/* a GTT backing from any memory descriptor (takes the reference) */
int RDNA3Accel::gttWire(rdna3_bo *bo, IOMemoryDescriptor *md, bool prepared)
{
    RDNA3Gtt *g = (RDNA3Gtt *)IOMalloc(sizeof(*g));
    if (!g) {
        if (prepared)
            md->complete();
        md->release();
        return -RD_ENOMEM;
    }
    bzero(g, sizeof(*g));
    g->md = md;
    g->prepared = prepared;
    g->npages = (bo->size + 4095) / 4096;
    g->pages = (uint64_t *)IOMalloc((size_t)(g->npages * sizeof(uint64_t)));
    if (!g->pages)
        goto fail;
    g->dma = IODMACommand::withSpecification(IODMACommand::OutputHost64, 44, 0,
                                             IODMACommand::kMapped, 0, 4096);
    if (!g->dma || g->dma->setMemoryDescriptor(g->md, true) != kIOReturnSuccess)
        goto fail;
    {
        uint64_t off = 0, page = 0;
        while (page < g->npages) {
            IODMACommand::Segment64 seg[8];
            UInt32 n = 8;
            if (g->dma->gen64IOVMSegments(&off, seg, &n) != kIOReturnSuccess || !n)
                goto fail;
            for (UInt32 i = 0; i < n; i++)
                for (uint64_t b = 0; b < seg[i].fLength && page < g->npages; b += 4096)
                    g->pages[page++] = seg[i].fIOVMAddr + b;
        }
    }
    bo->backing = g;
    return 0;
fail:
    gttFree(g);
    return -RD_ENOMEM;
}

void RDNA3Accel::gttFree(RDNA3Gtt *g)
{
    if (!g)
        return;
    if (g->dma) {
        g->dma->clearMemoryDescriptor(true);
        g->dma->release();
    }
    if (g->pages)
        IOFree(g->pages, (size_t)(g->npages * sizeof(uint64_t)));
    if (g->md && g->prepared)
        g->md->complete();
    OSSafeReleaseNULL(g->md);
    IOFree(g, sizeof(*g));
}

void RDNA3Accel::drmBoFree(void *ctx, rdna3_bo *bo)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        range_alloc *h = bo->vram_offset < a->fVramHeapVisEnd ? a->fVramHeap : a->fVramHeapHi;
        if (h)
            range_alloc_put(h, bo->vram_offset, bo->size);
    } else if (bo->backing) {
        gttFree((RDNA3Gtt *)bo->backing);
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
                          uint32_t n, const rdna3_cs_job *job, uint64_t *seq)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->submit(vm, ip, ring, ibs, n, job, seq) : -RD_ENODEV;
}

uint64_t RDNA3Accel::drmGpuClock(void *ctx)
{
    /* read-only SMUIO registers through the display's register view: no
     * lock needed, and present from rdna3lvl=2 */
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fDispDev ? rdna3_gpu_clock_counter(a->fDispDev) : 0;
}

int RDNA3Accel::drmFenceSignaled(void *ctx, uint32_t ip, uint32_t ring, uint64_t seq)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->fenceSignaled(ip, ring, seq) : 1;
}

/* Called from rdna3_drm_ioctl, i.e. with fDrmLock held. The sleep runs
 * without it, so one client's wait (up to the 10 s cap) does not stall the
 * others; the UAPI holds no pointers across it (fences by value, syncobjs
 * looked up again) and drmClose waits for in-flight ioctls. Recovery after
 * the cap runs with the lock again, once, if the fence is still pending. */
int RDNA3Accel::drmFenceWait(void *ctx, uint32_t ip, uint32_t ring, uint64_t seq, uint64_t absNs)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    bool cap = false;
    int r;
    if (!a->fBringup)
        return 0;
    IOLockUnlock(a->fDrmLock);
    r = a->fBringup->fenceWait(ip, ring, seq, absNs, &cap);
    IOLockLock(a->fDrmLock);
    if (cap)
        r = a->fBringup->fenceSignaled(ip, ring, seq) ? 0 : a->fBringup->recoverHang(ip, ring, seq);
    return r;
}

int RDNA3Accel::drmSensor(void *ctx, uint32_t type, uint32_t *value)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->sensor(type, value) : -RD_EINVAL;
}

int RDNA3Accel::drmVmFaultInfo(void *ctx, void *vm, uint64_t *addr, uint32_t *status, uint32_t *vmhub)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->vmFaultInfo(vm, addr, status, vmhub) : -RD_EINVAL;
}

uint32_t RDNA3Accel::drmVmQuery(void *ctx, void *vm)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->vmQuery(vm) : 0;
}

void RDNA3Accel::drmSetClocks(uint32_t minSclkKhz, uint32_t maxSclkKhz, uint32_t minMclkKhz,
                              uint32_t maxMclkKhz)
{
    if (!fDrm || !fDrmLock)
        return;
    IOLockLock(fDrmLock);
    rdna3_drm_device_set_clocks(fDrm, minSclkKhz, maxSclkKhz, minMclkKhz, maxMclkKhz);
    IOLockUnlock(fDrmLock);
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
    ops.vm_query = drmVmQuery;
    ops.bo_userptr = drmBoUserptr;
    ops.sensor = drmSensor;
    ops.vm_fault_info = drmVmFaultInfo;
    ops.gpu_clock = drmGpuClock;   /* AMDGPU_INFO_TIMESTAMP: SMUIO GOLDEN_TSC */

    fDrmLock = IOLockAlloc();
    fDrm = (rdna3_drm_device *)drmZalloc(this, sizeof(*fDrm));
    fVramHeap = (range_alloc *)drmZalloc(this, sizeof(*fVramHeap));
    fVramHeapHi = (range_alloc *)drmZalloc(this, sizeof(*fVramHeapHi));
    if (!fDrmLock || !fDrm || !fVramHeap || !fVramHeapHi) {
        drmFree();
        return false;
    }

    fVramDev = fPci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (fVramDev)
        fVramDev->retain();
    fVramMap = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
                                                 kIOMapWriteCombineCache);
    uint64_t visible = fVramMap ? fVramMap->getLength() : 0;

    /* user heaps between the kernel's two windows (layout in RDNA3Accel.hpp) */
    uint64_t heapStart = (fDispReserveEnd > kVramKernelLowStart ? fDispReserveEnd
                                                                : kVramKernelLowStart) +
                         kVramKernelLowSize;
    uint64_t heapEnd = fVramSize > kVramKernelTopSize ? fVramSize - kVramKernelTopSize : 0;
    uint64_t visEnd = visible < heapEnd ? visible : heapEnd;
    if (visEnd > heapStart)
        range_alloc_init(fVramHeap, heapStart, visEnd - heapStart);
    else
        range_alloc_init(fVramHeap, 0, 0);
    if (visEnd < heapStart)
        visEnd = heapStart;
    if (heapEnd > visEnd)
        range_alloc_init(fVramHeapHi, visEnd, heapEnd - visEnd);
    else
        range_alloc_init(fVramHeapHi, 0, 0);
    fVramHeapVisEnd = visEnd;

    /* amdgpu sizes GTT at TTM's page limit, half of system RAM; the kext
     * cannot read the RAM size through the KPIs it links (sysctlbyname is
     * not exported, max_mem only in the unsupported KPI), so a fixed 8 GiB
     * that any machine able to run this card has twice over */
    uint64_t gtt = 8ull << 30;
    uint8_t rev = fPci->configRead8(kIOPCIConfigRevisionID);
    /* nbio_v4_3_get_rev_id: the chip revision is the NBIO strap, not the
     * PCI revision; external_rev = rev_id + 0x20 for GC 11.0.3, which is
     * how Mesa tells NAVI32 apart */
    uint32_t revId = 0;
    if (fDispDev) {
        revId = rdna3_nbio_rev_id(fDispDev);
        setProperty("RDNA3,ChipRevId", revId, 32);
    }
    rdna3_drm_device_init(fDrm, &ops, disc, fPci->configRead16(kIOPCIConfigDeviceID), rev,
                          revId, fVramSize, visible > fVramSize ? fVramSize : visible, gtt);
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

    /* harvesting (gfx_v11_0_get_cu_info / setup_rb) at rdna3lvl>=3, on the
     * display's register view; the bring-up thread, which also selects
     * SE/SA through GRBM_GFX_INDEX, has not started yet */
    if (fLevel >= 3 && fDispDev && disc->has_gc_info) {
        rdna3_gfx_cu_info ci;
        bzero(&ci, sizeof(ci));
        ci.max_se = disc->gc.num_se;
        ci.max_sa_per_se = disc->gc.num_sa_per_se;
        ci.max_cu_per_sa = 2 * (disc->gc.num_wgp0_per_sa + disc->gc.num_wgp1_per_sa);
        ci.max_backends_per_se = disc->gc.num_rb_per_se;
        if (gfx11_get_cu_info(fDispDev, &ci) == RDNA3_OK && ci.cu_active) {
            rdna3_drm_device_set_cu_info(fDrm, ci.cu_bitmap, ci.cu_active, ci.rb_active);
            setProperty("RDNA3,ActiveCUs", ci.cu_active, 32);
            setProperty("RDNA3,ActiveRBMask", ci.rb_active, 32);
            setProperty("RDNA3,ActiveSAMask", ci.sa_active, 32);
            LOG("GC: %u active CUs, RB mask 0x%x (%u RBs), SA mask 0x%x", ci.cu_active, ci.rb_active,
                ci.num_rbs, ci.sa_active);
        }
    }

    uint32_t enumOnly = 0;
    if (PE_parse_boot_argn("rdna3enum", &enumOnly, sizeof(enumOnly)) && enumOnly) {
        rdna3_drm_device_set_enumeration_only(fDrm, 1);
        LOG("rdna3enum=1: reporting accel_working + rings for enumeration; CS still fails");
    }
    LOG("DRM/amdgpu UAPI ready: VRAM %llu MiB (%llu MiB visible), user heap %llu MiB visible + "
        "%llu MiB", (unsigned long long)(fVramSize >> 20), (unsigned long long)(visible >> 20),
        (unsigned long long)(range_alloc_free_bytes(fVramHeap) >> 20),
        (unsigned long long)(range_alloc_free_bytes(fVramHeapHi) >> 20));
    setProperty("IOUserClientClass", "RDNA3UserClient");
    return true;
}

void RDNA3Accel::drmFree()
{
    if (fDrm && fDrm->ops.bo_free)   /* user fence BOs still held (rings are down) */
        rdna3_drm_device_fini(fDrm);
    if (fDrm)
        drmFreeMem(this, fDrm, sizeof(*fDrm));
    if (fVramHeap)
        drmFreeMem(this, fVramHeap, sizeof(*fVramHeap));
    if (fVramHeapHi)
        drmFreeMem(this, fVramHeapHi, sizeof(*fVramHeapHi));
    if (fDrmLock)
        IOLockFree(fDrmLock);
    OSSafeReleaseNULL(fVramMap);
    OSSafeReleaseNULL(fVramDev);
    fDrm = nullptr;
    fVramHeap = nullptr;
    fVramHeapHi = nullptr;
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
    /* another thread of the client may sit in a fence wait with the lock
     * dropped: let it finish before the file goes away */
    while (f->inflight)
        IOLockSleep(fDrmLock, &f->inflight, THREAD_UNINT);
    rdna3_drm_file_close(f);
    IOLockUnlock(fDrmLock);
}

int RDNA3Accel::drmIoctl(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size)
{
    IOLockLock(fDrmLock);
    f->inflight++;
    int r = rdna3_drm_ioctl(f, nr, data, size);
    if (--f->inflight == 0)
        IOLockWakeup(fDrmLock, &f->inflight, false);
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
        md = ((RDNA3Gtt *)bo->backing)->md;
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
