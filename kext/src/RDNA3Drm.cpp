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
#include "RDNA3Trace.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <sys/random.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODMACommand.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <sys/kauth.h>
#include <mach/mach_time.h>

#include <libkern/copyio.h>   /* copyin/copyout (kpi.libkern) */
#include <sys/sysctl.h>
#include <sys/proc.h>

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

extern "C" uint64_t mach_continuous_time(void);


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
         * prefers visible. amdgpu lets the rest land outside the BAR and
         * moves it in on a CPU fault (amdgpu_bo_fault_reserve_notify);
         * nothing migrates a BO here, and a BO outside the BAR cannot be
         * mapped (drmMemoryForOffset), so only NO_CPU_ACCESS may go there:
         * the rest falls back to GTT in the UAPI (card run 52: RADV's
         * descriptor pools, VRAM without either flag, landed outside the
         * full visible part, their map failed, every Chess pipeline with
         * VK_ERROR_OUT_OF_DEVICE_MEMORY) */
        uint64_t align = bo->alignment > 4096 ? bo->alignment : 4096;
        range_alloc *first = (bo->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS) ? a->fVramHeapHi
                                                                           : a->fVramHeap;
        range_alloc *second = first == a->fVramHeap ? a->fVramHeapHi : a->fVramHeap;
        range_alloc *from = first;
        uint64_t off = range_alloc_get(first, bo->size, align);
        if (off == RANGE_ALLOC_FAIL && (bo->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS)) {
            from = second;
            off = range_alloc_get(second, bo->size, align);
        }
        if (off == RANGE_ALLOC_FAIL) {
            /* the UAPI places it in GTT instead (card run 51: the visible
             * part was full); how full, for the report */
            static SInt32 gVramFull;
            if (OSIncrementAtomic(&gVramFull) < 8)
                LOG("GEM: %llu KiB of VRAM%s did not fit (visible heap %llu KiB free, the rest %llu KiB free): GTT",
                    (unsigned long long)(bo->size >> 10),
                    (bo->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS) ? " without CPU access" : " the CPU may map",
                    (unsigned long long)(range_alloc_free_bytes(a->fVramHeap) >> 10),
                    (unsigned long long)(range_alloc_free_bytes(a->fVramHeapHi) >> 10));
            return -RD_ENOMEM;
        }
        bo->vram_offset = off;
        bo->backing = nullptr;
        /* AMDGPU_GEM_CREATE_VRAM_CLEARED (the UAPI sets it on every VRAM
         * BO, as amdgpu_gem_create_ioctl): by the SDMA ring when it runs
         * (amdgpu_fill_buffer; a BAR bzero of a big BO would hold fDrmLock
         * for long), else by the CPU through BAR0, moving the BO into the
         * visible part if it has to; a BO that cannot be cleared is not
         * handed out */
        if (bo->flags & AMDGPU_GEM_CREATE_VRAM_CLEARED) {
            int cr = a->fBringup ? a->fBringup->clearVram(off, bo->size) : RDNA3_ENODEV;
            if (cr == RDNA3_ETIME) {
                /* the fill may still land: the range is not reused */
                LOG("VRAM clear of %llu KiB at 0x%llx timed out: range retired",
                    (unsigned long long)(bo->size >> 10), (unsigned long long)off);
                return -RD_ENOMEM;
            }
            if (cr != RDNA3_OK) {
                bool visible = a->fVramMap && off + bo->size <= a->fVramMap->getLength();
                if (!visible) {
                    range_alloc_put(from, off, bo->size);
                    off = from != a->fVramHeap ? range_alloc_get(a->fVramHeap, bo->size, align)
                                               : RANGE_ALLOC_FAIL;
                    if (off == RANGE_ALLOC_FAIL)
                        return -RD_ENOMEM;
                    from = a->fVramHeap;
                    if (!a->fVramMap || off + bo->size > a->fVramMap->getLength()) {
                        range_alloc_put(from, off, bo->size);
                        return -RD_ENOMEM;
                    }
                    bo->vram_offset = off;
                }
                bzero((void *)(a->fVramMap->getVirtualAddress() + off), (size_t)bo->size);
            }
        }
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
    /* READONLY: the GPU only reads the pages (memory -> device = Out) */
    IOOptionBits dir = (flags & AMDGPU_GEM_USERPTR_READONLY) ? kIODirectionOut : kIODirectionInOut;
    IOMemoryDescriptor *md;
    if (!task)
        return -RD_EINVAL;
    md = IOMemoryDescriptor::withAddressRange(uaddr, bo->size, dir, task);
    if (!md)
        return -RD_EFAULT;
    IOReturn pr = md->prepare();
    if (pr != kIOReturnSuccess && dir == kIODirectionInOut) {
        /* RADV imports every host pointer read-write (amdgpu_create_bo_from_user_mem has no READONLY), but
         * WindowServer gets the apps' CoreAnimation backing stores mapped read-only, so wiring them for writing
         * fails and each such Metal buffer fell back to a copy that never saw the app's next drawing (card runs
         * 54-56: every "imported host pointer" refused, System Settings' sidebar stale while scrolling). Wire
         * them read-only and map them so: the GPU only reads those pages. */
        md->release();
        md = IOMemoryDescriptor::withAddressRange(uaddr, bo->size, kIODirectionOut, task);
        if (!md)
            return -RD_EFAULT;
        IOReturn pr2 = md->prepare();
        if (pr2 == kIOReturnSuccess) {
            bo->userptr_ro = 1;
            if (a->fUserptrRo++ < 4)
                LOG("GEM_USERPTR: %llu bytes at 0x%llx are not writable (0x%x): wired read-only, the GPU maps them "
                    "read-only (%u so far)", (unsigned long long)bo->size, (unsigned long long)uaddr, (unsigned)pr,
                    a->fUserptrRo);
        }
        pr = pr2;
    }
    if (pr != kIOReturnSuccess) {
        if (a->fUserptrFails++ < 8)
            LOG("GEM_USERPTR: %llu bytes at 0x%llx (flags 0x%x) could not be wired (0x%x) - refused",
                (unsigned long long)bo->size, (unsigned long long)uaddr, flags, (unsigned)pr);
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
    if (bo->foreign) {   /* an IOSurface's VRAM: the accelerator's allocation */
        a->platformVramPutLocked((struct RDNA3VramHandle *)bo->backing);
        bo->backing = nullptr;
    } else if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        range_alloc *h = bo->vram_offset < a->fVramHeapVisEnd ? a->fVramHeap : a->fVramHeapHi;
        if (h)
            range_alloc_put(h, bo->vram_offset, bo->size);
    } else if (bo->backing) {
        RDNA3Gtt *g = (RDNA3Gtt *)bo->backing;
        if (g->gartSlot && a->fBringup)   /* a user queue's wptr page was bound to GART */
            a->fBringup->gartRelease(g->gartSlot);
        gttFree(g);
        bo->backing = nullptr;
    }
}

/* ---- user queues (rdna3userq=1): the OS half of mes_userqueue.c ---- */

/* amdgpu_bo_kmap: VRAM inside the BAR0 window, a kernel GTT buffer */
void *RDNA3Accel::drmBoKmap(void *ctx, rdna3_bo *bo)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (bo->placement == AMDGPU_GEM_DOMAIN_VRAM)
        return (a->fVramMap && bo->vram_offset + bo->size <= a->fVramMap->getLength())
                   ? (void *)(a->fVramMap->getVirtualAddress() + bo->vram_offset)
                   : nullptr;
    if (bo->placement == AMDGPU_GEM_DOMAIN_GTT && bo->backing && !bo->userptr) {
        IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, ((RDNA3Gtt *)bo->backing)->md);
        return b ? b->getBytesNoCopy() : nullptr;
    }
    return nullptr;
}

int RDNA3Accel::drmBoGpuAddr(void *ctx, rdna3_bo *bo, uint64_t *mc)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->gpuAddr(bo, mc) : -RD_ENODEV;
}

int RDNA3Accel::drmUserqMqdInit(void *ctx, uint32_t ip, void *mqd, const amd_mqd_prop *p)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->userqMqdInit(ip, mqd, p) : -RD_ENODEV;
}

int RDNA3Accel::drmUserqAdd(void *ctx, void *vm, amd_mes_add_queue *in)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->userqAdd(vm, in) : -RD_ENODEV;
}

int RDNA3Accel::drmUserqRemove(void *ctx, const amd_mes_remove_queue *in)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    return a->fBringup ? a->fBringup->userqRemove(in) : -RD_ENODEV;
}

/* CLOCK_MONOTONIC as userspace deadlines count it on macOS */
uint64_t RDNA3Accel::drmNowNs(void *)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_continuous_time(), &ns);
    return ns;
}

/* a user queue fence wait: done() under fDrmLock, the sleeps without it
 * (as drmFenceWait), capped at 10 s like a kernel fence wait. A user queue
 * that never gets there is the client's: no recovery runs for it. */
int RDNA3Accel::drmUserqWait(void *ctx, int (*done)(void *), void *arg, uint64_t absNs)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    const uint64_t capNs = 10ull * 1000 * 1000 * 1000;
    uint64_t start = drmNowNs(ctx), now, until;
    if (!a->fBringup)
        return done(arg) ? 0 : -RD_ETIME;
    /* a deadline already gone is a poll (fenceWait's now >= absNs), not
     * an unsigned wrap into the 10 s cap (PR #2 review) */
    if (absNs != UINT64_MAX && absNs <= start)
        until = start;
    else
        until = absNs != UINT64_MAX && absNs - start < capNs ? absNs : start + capNs;
    for (;;) {
        if (done(arg))
            return 0;
        now = drmNowNs(ctx);
        if (now >= until)
            return -RD_ETIME;
        IOLockUnlock(a->fDrmLock);
        a->fBringup->waitInterrupt(until);
        IOLockLock(a->fDrmLock);
    }
}

void RDNA3Accel::drmUserqReady(uint32_t ipMask, uint64_t doorbellSize)
{
    if (!fDrm || !fDrmLock || !fUserqArg)
        return;
    if (!fDoorbellDev) {
        fDoorbellDev = fPci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
        if (fDoorbellDev)
            fDoorbellDev->retain();
    }
    if (!fDoorbellDev)
        return;
    {
        RD3DrmScope lk(this);
        rdna3_drm_device_set_userq(fDrm, ipMask, doorbellSize);
    }
    setProperty("RDNA3,UserQueueIPs", ipMask, 32);
    LOG("user queues: DRM_AMDGPU_USERQ for IP mask 0x%x, %llu KiB of doorbells", ipMask,
        (unsigned long long)(doorbellSize >> 10));
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

void RDNA3Accel::drmVmOrphaned(void *ctx, void *vm, int reclaimable)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (a->fBringup)
        a->fBringup->vmOrphaned(vm, reclaimable != 0);
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

void RDNA3Accel::drmVmReplayIfNeeded()
{
    bool vramLost = false;
    if (!fBringup || !fDrm)
        return;
    /* closed files whose jobs a recovery has finished now: their VMIDs back */
    int reaped = rdna3_drm_device_reap_orphans(fDrm);
    if (reaped)
        LOG("%d closed client(s) whose jobs outlived them: VM, mappings and BOs freed now", reaped);
    if (fBringup->takeVmReplay()) {
        int failed = rdna3_drm_device_vm_replay(fDrm);
        LOG("VRAM lost: the clients' GPUVM ranges mapped again%s", failed ? " (some failed)" : "");
        setNumber("RDNA3,VMReplayFailures", (uint64_t)failed);
    }
    /* amdgpu_userq_post_reset: the queues the restarted scheduler forgot */
    if (fBringup->takeUserqRestart(&vramLost)) {
        int failed = rdna3_drm_device_userq_reset(fDrm, vramLost ? 1 : 0);
        LOG("user queues after the reset: %s%s", vramLost ? "hung (VRAM lost)" : "mapped again",
            failed ? ", some failed" : "");
        setNumber("RDNA3,UserqResetFailures", (uint64_t)failed);
    }
}

void RDNA3Accel::drmVramLostSync()
{
    uint32_t n;
    if (!fBringup || !fDrm)
        return;
    n = fBringup->vramLostCount();
    if (n < fVramLostTold) {
        /* not expected: forgetVramLoss runs before the sync. The next
         * loss is then told again rather than missed */
        LOG("VRAM lost count went back (%u -> %u) after the clients were told", fVramLostTold, n);
        fVramLostTold = n;
        return;
    }
    if (fVramLostTold == n)
        return;
    while (fVramLostTold < n) {
        rdna3_drm_device_vram_lost(fDrm);
        fVramLostTold++;
    }
    LOG("VRAM lost for the clients: AMDGPU_INFO_VRAM_LOST_COUNTER %u, older contexts see VRAMLOST%s",
        fDrm->vram_lost_counter, fDrm->vram_lost_cs_refuse ? " and their submissions fail with -ECANCELED"
                                                            : " (rdna3vramlostcs=0: they keep submitting)");
    setNumber("RDNA3,VRAMLostCounter", fDrm->vram_lost_counter);
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
    const uint64_t t0 = mach_absolute_time();
    IOLockUnlock(a->fDrmLock);
    r = a->fBringup->fenceWait(ip, ring, seq, absNs, &cap);
    IOLockLock(a->fDrmLock);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    bool recovered = false;
    int rr = 0;
    if (cap) {
        recovered = !a->fBringup->fenceSignaled(ip, ring, seq);
        r = recovered ? (rr = a->fBringup->recoverHang(ip, ring, seq)) : 0;
    }
    {
        /* card run 2026-10-17b's trace: the wait, and a recovery it caused */
        struct rd3_trace_ev e;
        bzero(&e, sizeof e);
        e.type = RD3_TR_GPUWAIT;
        e.head = 0xff;
        e.arg = (uint8_t)ip;
        e.flags = (uint8_t)ring;
        e.v0 = (uint32_t)proc_selfpid();
        e.v1 = (uint32_t)(ns / 1000 > 0xffffffffull ? 0xffffffffull : ns / 1000);
        e.v2 = (uint32_t)seq;
        e.v3 = cap ? 1u : 0u;
        e.v4 = (uint32_t)r;
        rdna3TraceCore(&e);
        if (recovered) {
            e.type = RD3_TR_RESET;
            e.v0 = (uint32_t)rr;
            e.v1 = e.v3 = e.v4 = 0;
            rdna3TraceCore(&e);
        }
    }
    a->drmVmReplayIfNeeded();
    a->drmVramLostSync();   /* a reset that lost VRAM: nothing brings it back */
    return r;
}

void RDNA3Accel::statsTick(void *owner, void *)
{
    RDNA3Accel *a = (RDNA3Accel *)owner;
    uint32_t v[8];
    bool ok = false;
    if (a->fStatsStop)
        return;
    if (a->fDrmLock) {
        IOLockLock(a->fDrmLock);
        ok = a->fBringup && a->fBringup->stats(v);
        /* the idle-power experiment (rdna3pmexp=1) reads no GC registers */
        if (ok && !a->fBringup->pmExperimentTick() && a->fStatsTicks % 15 == 0)
            a->fBringup->logBusy();
        if (ok)
            a->fBringup->idleCapTick();
        IOLockUnlock(a->fDrmLock);
    }
    if (ok) {
        static const char *const keys[8] = {
            "Device Utilization %", "Temperature(C)", "Junction Temperature(C)", "Memory Temperature(C)",
            "Fan Speed(RPM)", "Core Clock(MHz)", "Memory Clock(MHz)", "Total Power(W)",
        };
        OSDictionary *d = OSDictionary::withCapacity(9);
        if (d) {
            for (unsigned i = 0; i < 8; i++) {
                OSNumber *n = OSNumber::withNumber((unsigned long long)v[i], 32);
                if (n) {
                    d->setObject(keys[i], n);
                    n->release();
                }
            }
            if (OSNumber *n = OSNumber::withNumber((unsigned long long)v[0], 32)) {
                d->setObject("GPU Activity(%)", n);
                n->release();
            }
            a->setProperty("PerformanceStatistics", d);
            d->release();
        }
        /* the trend from boot on (the first card run read ~197 W, 2700 MHz
         * and 100 % activity 90 s after boot, even at rdna3lvl=5) */
        if (a->fStatsTicks++ % 15 == 0)
            LOG("PM: activity %u%%, gfxclk %u MHz, uclk %u MHz, %u W, edge %u C, hotspot %u C, mem %u C, fan %u rpm",
                v[0], v[5], v[6], v[7], v[1], v[2], v[3], v[4]);
    }
    /* what each head sends after its OTG, every 2 s (RDNA3Resume.cpp),
     * whether the SMU's metrics came or not */
    if (!a->fStatsStop)
        a->displayTxCheck(nullptr);
    if (!a->fStatsStop && a->fStatsCall) {
        uint64_t dl;
        clock_interval_to_deadline(2000, kMillisecondScale, &dl);
        thread_call_enter_delayed(a->fStatsCall, dl);
    }
}

int RDNA3Accel::pmCtl(const rdna3_pmctl_req *rq, rdna3_pmctl_reply *rp, bool admin)
{
    int r;
    if (!fDrmLock)
        return -RD_ENODEV;
    IOLockLock(fDrmLock);
    if (fBringup) {
        r = fBringup->pmctl(rq, rp, admin);
        drmVmReplayIfNeeded();   /* rdna3baco: the chip went through BACO */
        drmVramLostSync();
    } else {
        bzero(rp, sizeof(*rp));
        rp->op = rq->op;
        rp->err = r = RDNA3_ENODEV;
    }
    IOLockUnlock(fDrmLock);
    return r;
}

void RDNA3Accel::statsStart()
{
    fStatsStop = false;
    if (!fStatsCall)
        fStatsCall = thread_call_allocate((thread_call_func_t)statsTick, this);
    if (fStatsCall)
        thread_call_enter(fStatsCall);
}

void RDNA3Accel::statsStop()
{
    fStatsStop = true;
    if (fStatsCall) {
        rd3_thread_call_drain_free(fStatsCall);
        fStatsCall = nullptr;
    }
}

void RDNA3Accel::drmSetFirmware(const uint32_t ver[32], const uint32_t feature[32],
                                const uint32_t ta[RD_TA_TYPES])
{
    memcpy(fFwVer, ver, sizeof(fFwVer));
    memcpy(fFwFeat, feature, sizeof(fFwFeat));
    memcpy(fTaVer, ta, sizeof(fTaVer));
    if (fDrm && fDrmLock) {
        RD3DrmScope lk(this);
        memcpy(fDrm->fw_version, fFwVer, sizeof(fFwVer));
        memcpy(fDrm->fw_feature, fFwFeat, sizeof(fFwFeat));
        memcpy(fDrm->ta_version, fTaVer, sizeof(fTaVer));
    }
}

void RDNA3Accel::drmSetVbios(const uint8_t *image, uint32_t len, const drm_amdgpu_info_vbios *info)
{
    if (fVbiosCopy || !image || !len || len > (4u << 20))
        return;   /* once: the image does not change */
    uint8_t *c = (uint8_t *)IOMalloc(len);
    if (!c)
        return;
    memcpy(c, image, len);
    RD3DrmScope lk(this);
    fVbiosCopy = c;
    fVbiosCopyLen = len;
    fVbiosInfo = *info;
    if (fDrm) {
        fDrm->vbios = fVbiosCopy;
        fDrm->vbios_len = fVbiosCopyLen;
        fDrm->vbios_info = fVbiosInfo;
    }
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

void RDNA3Accel::drmSetAsicInfo(uint32_t xclkKhz, uint32_t vramType, uint32_t vramWidth, uint64_t mallSize)
{
    if (!fDrm || !fDrmLock)
        return;
    RD3DrmScope lk(this);
    rdna3_drm_device_set_asic_info(fDrm, xclkKhz, vramType, vramWidth, mallSize);
}

void RDNA3Accel::drmSetClocks(uint32_t minSclkKhz, uint32_t maxSclkKhz, uint32_t minMclkKhz,
                              uint32_t maxMclkKhz)
{
    if (!fDrm || !fDrmLock)
        return;
    RD3DrmScope lk(this);
    rdna3_drm_device_set_clocks(fDrm, minSclkKhz, maxSclkKhz, minMclkKhz, maxMclkKhz);
}

void RDNA3Accel::drmVcnReady(uint32_t ipVersion, uint32_t numInst, uint32_t harvest, uint32_t instReady,
                             bool jpegReady)
{
    if (!fDrm || !fDrmLock)
        return;
    {
        RD3DrmScope lk(this);
        rdna3_drm_device_set_vcn(fDrm, ipVersion, numInst, harvest, instReady, jpegReady ? 1 : 0);
    }
    setProperty("RDNA3,VCNInstancesReady", instReady, 32);
    LOG("VCN %u.%u.%u: %u instance(s), unified rings 0x%x, JPEG %s for command submission",
        (ipVersion >> 16) & 0xff, (ipVersion >> 8) & 0xff, ipVersion & 0xff, numInst, instReady,
        jpegReady ? "ready" : "off");
}

void RDNA3Accel::drmRingReady(uint32_t ip, uint32_t mask)
{
    if (!fDrm || !fDrmLock)
        return;
    {
        RD3DrmScope lk(this);
        rdna3_drm_device_set_ring_ready(fDrm, ip, mask);
    }
    LOG("HW IP %u ring mask 0x%x ready for command submission", ip, mask);
}

/* BO export tokens (RD_NR_XNU_BO_EXPORT): libkern's random() (seeded
 * from the kernel's entropy at boot) mixed with the timebase - only KPI
 * symbols, so the kext keeps loading on every macOS */
/* PRIME export tokens are capabilities: the kernel's CSPRNG (yarrow /
 * Fortuna behind read_random), not random(), whose state a client could
 * recover from its own tokens (ECC security review) */
static uint64_t drmRandom64(void *)
{
    uint64_t v = 0;
    read_random(&v, sizeof(v));
    return v;
}

/* DRM_AMDGPU_SCHED and context priorities above NORMAL: Linux lets the
 * DRM master or CAP_SYS_NICE (amdgpu_ctx_priority_permit, DRM_MASTER on the
 * ioctl). macOS has no DRM master - the window server is no DRM client - so
 * the caller's credentials decide: root. Called in the client's thread. */
static int drmPrivileged(void *)
{
    return kauth_cred_issuser(kauth_cred_get()) ? 1 : 0;
}

bool RDNA3Accel::drmInit(const amd_discovery *disc)
{
    if (!rdna3TraceCoreInit())
        LOG("trace: no memory for the command-submission trace ring - debug.rdna3_drmtrace stays empty "
            "(every event counted in its header's nomem)");
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
    ops.vm_orphaned = drmVmOrphaned;
    ops.vm_map = drmVmMap;
    ops.vm_unmap = drmVmUnmap;
    ops.submit = drmSubmit;
    ops.fence_signaled = drmFenceSignaled;
    ops.fence_wait = drmFenceWait;
    ops.vm_query = drmVmQuery;
    ops.bo_userptr = drmBoUserptr;
    ops.sensor = drmSensor;
    ops.scanout = drmScanout;
    ops.txstate = drmTxState;
    ops.vm_fault_info = drmVmFaultInfo;
    ops.random64 = drmRandom64;
    ops.gpu_clock = drmGpuClock;   /* AMDGPU_INFO_TIMESTAMP: SMUIO GOLDEN_TSC */
    ops.privileged = drmPrivileged;
    /* every profile: wait-any slices its waits by this clock (PR #2 review:
     * with it only under rdna3userq=1, wait-any waited on the first fence) */
    ops.now_ns = drmNowNs;
    ops.surface_bo = drmSurfaceBo;
    ops.surface_dirty = drmSurfaceDirty;
    {
        /* user queues: the hooks only with rdna3userq=1, so the default
         * profile has no path into them; the UAPI offers queues once the
         * bring-up's MES runs (drmUserqReady) */
        uint32_t uq = 0;
        PE_parse_boot_argn("rdna3userq", &uq, sizeof(uq));
        fUserqArg = uq != 0;
        if (fUserqArg) {
            ops.bo_kmap = drmBoKmap;
            ops.bo_gpu_addr = drmBoGpuAddr;
            ops.userq_mqd_init = drmUserqMqdInit;
            ops.userq_add_hw_queue = drmUserqAdd;
            ops.userq_remove_hw_queue = drmUserqRemove;
            ops.userq_wait = drmUserqWait;
        }
        /* rdna3vcn=1: amdgpu_cs_patch_ibs reads the video IBs (VCN create
         * messages, the JPEG IB it copies) through the kernel's BO view */
        uint32_t vcn = 0;
        PE_parse_boot_argn("rdna3vcn", &vcn, sizeof(vcn));
        if (vcn)
            ops.bo_kmap = drmBoKmap;
    }

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
    memcpy(fDrm->fw_version, fFwVer, sizeof(fFwVer));
    memcpy(fDrm->fw_feature, fFwFeat, sizeof(fFwFeat));
    memcpy(fDrm->ta_version, fTaVer, sizeof(fTaVer));
    fDrm->vbios = fVbiosCopy;
    fDrm->vbios_len = fVbiosCopyLen;
    fDrm->vbios_info = fVbiosInfo;
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
        /* gfx_v11_0_constants_init: get_tcc_info after get_cu_info
         * (rdna3amdgpu=1; DEV_INFO's tcc_disabled_mask stays 0 otherwise) */
        uint32_t exact = 0;
        if (PE_parse_boot_argn("rdna3amdgpu", &exact, sizeof(exact)) && exact) {
            uint32_t tcc = gfx11_get_tcc_info(fDispDev);
            rdna3_drm_device_set_tcc_disabled(fDrm, tcc);
            setProperty("RDNA3,TCCDisabledMask", tcc, 32);
            /* amdgpu_discovery_get_mall_info: gmc.mall_size for DEV_INFO */
            if (fDiscSocOk && fDiscSoc.mall_size)
                rdna3_drm_device_set_asic_info(fDrm, 0, 0, 0, fDiscSoc.mall_size);
        }
    }

    /* amdgpu_cs_pass1's refusal of contexts older than a VRAM loss; the
     * kill switch rdna3vramlostcs=0 lets them submit on (debug) */
    uint32_t lostCs = 1;
    PE_parse_boot_argn("rdna3vramlostcs", &lostCs, sizeof(lostCs));
    rdna3_drm_device_set_vram_lost_cs(fDrm, lostCs ? 1 : 0);
    setProperty("RDNA3,VRAMLostRefusesCS", lostCs ? 1 : 0, 32);
    LOG("VRAM loss: %s", lostCs ? "contexts made before it fail their submissions with -ECANCELED "
                                  "(amdgpu_cs_pass1; rdna3vramlostcs=0 turns that off)"
                                : "rdna3vramlostcs=0: contexts made before it keep submitting; the "
                                  "counter and VRAMLOST still report it");
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
    rdna3TraceCoreFini();
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
    OSSafeReleaseNULL(fAccelSvc);
    OSSafeReleaseNULL(fAccelSsSym);
    OSSafeReleaseNULL(fDoorbellDev);
    if (fVbiosCopy)
        IOFree(fVbiosCopy, fVbiosCopyLen);
    fVbiosCopy = nullptr;
    fVbiosCopyLen = 0;
    fDrm = nullptr;
    fVramHeap = nullptr;
    fVramHeapHi = nullptr;
    fDrmLock = nullptr;
}

int RDNA3Accel::drmOpen(rdna3_drm_file *f, bool console)
{
    /* the opener's thread (IOServiceOpen -> the user client's start) */
    uint32_t uid = kauth_cred_getuid(kauth_cred_get());
    IOLockLock(fDrmLock);
    rdna3_drm_device_reap_orphans(fDrm);   /* a leaked VMID whose job is done comes back */
    /* one VMID per file: with none left for this client (rdna3_vmid_admit:
     * pool, per-uid cap, the reserve) the open fails - the user client does
     * not start, libdrm's open fails, the Metal plugin falls back to no
     * device - instead of handing out a VM that cannot submit */
    if (fBringup && !fBringup->vmCanOpen(uid, console)) {
        IOLockUnlock(fDrmLock);
        return -RD_ENOSPC;
    }
    int r = rdna3_drm_file_open(fDrm, f);
    IOLockUnlock(fDrmLock);
    if (r)
        LOG("DRM open failed (%d)", r);
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

/* refused AMDGPU_CS per errno, for the trace's rate limit (under fDrmLock) */
enum { kCsRefusedSlots = 8 };
struct CsRefused { int err; uint32_t n, skipped; };
static CsRefused gCsRefused[kCsRefusedSlots];

int RDNA3Accel::drmIoctl(rdna3_drm_file *f, uint32_t nr, void *data, uint32_t size)
{
    IOLockLock(fDrmLock);
    f->inflight++;
    /* amdgpu_gfx_off_ctrl(false) around anything that may touch the GFX */
    RDNA3Bringup *gb = fBringup;
    if (gb)
        gb->gfxOffHold();
    int r = rdna3_drm_ioctl(f, nr, data, size);
    if (gb)
        gb->gfxOffRelease();
    if (r && nr == RD_NR_AMDGPU(DRM_AMDGPU_CS)) {
        /* card run 2026-10-17b's trace: a submission refused anywhere (the
         * chunk parse, a guilty context, the ring): arg 0xff, the errno in
         * v4 (ECC review round 1); per errno the first 64, then every 64th
         * (v1 that errno's count, v2 those not traced since the last one;
         * round 2: a client retrying in a loop must not flush the ring) */
        unsigned slot = kCsRefusedSlots;
        for (unsigned i = 0; i < kCsRefusedSlots && slot == kCsRefusedSlots; i++)
            if (gCsRefused[i].err == r || !gCsRefused[i].n) slot = i;
        if (slot == kCsRefusedSlots) slot = kCsRefusedSlots - 1;   /* full: the last slot takes the rest */
        CsRefused &c = gCsRefused[slot];
        c.err = c.n ? c.err : r;
        c.n++;
        if (c.n <= 64 || (c.n & 63) == 0) {
            struct rd3_trace_ev e;
            bzero(&e, sizeof e);
            e.type = RD3_TR_CS;
            e.head = 0xff;
            e.arg = 0xff;
            e.v0 = (uint32_t)proc_selfpid();
            e.v1 = c.n;
            e.v2 = c.skipped;
            e.v4 = (uint32_t)r;
            rdna3TraceCore(&e);
            c.skipped = 0;
        } else {
            c.skipped++;
        }
        if (fCsFailLogs < 8 || (fCsFailLogs & 1023) == 0)
            LOG("drm: AMDGPU_CS from pid %d refused (%d) (%u such)", proc_selfpid(), r, fCsFailLogs + 1);
        fCsFailLogs++;
    }
    if (--f->inflight == 0)
        IOLockWakeup(fDrmLock, &f->inflight, false);
    IOLockUnlock(fDrmLock);
    return r;
}

/* A CPU mapping of a VRAM or doorbell BO holds the BO until it is
 * unmapped: the IOMemoryMap keeps this descriptor, and the range goes back
 * to the heap only when the descriptor is freed. Before, a client could
 * close the BO with the mapping still in place and then read and write
 * whatever the range was handed to next (another process's windows; ECC
 * security review of A3, which also lets it pick the range). GTT BOs are
 * IOBufferMemoryDescriptors the map retains itself. */
class RDNA3BoMemory : public IOSubMemoryDescriptor {
    OSDeclareDefaultStructors(RDNA3BoMemory)
public:
    static RDNA3BoMemory *withBo(RDNA3Accel *a, IOMemoryDescriptor *of, rdna3_bo *bo);
protected:
    virtual void free() override;
private:
    RDNA3Accel *fAccel = nullptr;
    rdna3_bo   *fBo = nullptr;
};
OSDefineMetaClassAndStructors(RDNA3BoMemory, IOSubMemoryDescriptor)

/* fDrmLock held */
RDNA3BoMemory *RDNA3BoMemory::withBo(RDNA3Accel *a, IOMemoryDescriptor *of, rdna3_bo *bo)
{
    RDNA3BoMemory *m = OSTypeAlloc(RDNA3BoMemory);
    if (!m)
        return nullptr;
    if (!m->initSubRange(of, (IOByteCount)bo->vram_offset, (IOByteCount)bo->size, kIODirectionInOut)) {
        m->release();
        return nullptr;
    }
    a->retain();
    m->fAccel = a;
    rdna3_drm_bo_get(bo);
    m->fBo = bo;
    return m;
}

/* never with fDrmLock held: drmMemoryForOffset hands the descriptor out
 * after unlocking, and the map's last release comes from the client */
void RDNA3BoMemory::free()
{
    if (fBo && fAccel)
        fAccel->drmBoRelease(fBo);
    fBo = nullptr;
    OSSafeReleaseNULL(fAccel);
    IOSubMemoryDescriptor::free();
}

void RDNA3Accel::drmBoRelease(rdna3_bo *bo)
{
    if (!fDrmLock)
        return;
    IOLockLock(fDrmLock);
    if (fDrm)   /* after drmFree the device's BOs are gone with it */
        rdna3_drm_bo_put(fDrm, bo);
    IOLockUnlock(fDrmLock);
}

IOMemoryDescriptor *RDNA3Accel::drmMemoryForOffset(rdna3_drm_file *f, uint64_t off,
                                                   IOOptionBits *options)
{
    IOMemoryDescriptor *md = nullptr;
    IOLockLock(fDrmLock);
    /* a map that waited for the lock while the file closed meets a dead
     * file (its BO table is gone) */
    rdna3_bo *bo = (f->closing || !f->bos) ? nullptr : rdna3_drm_bo_by_mmap_offset(f, off);
    if (bo && bo->placement == AMDGPU_GEM_DOMAIN_DOORBELL) {
        /* a user queue's doorbell page in BAR2, uncached */
        if (fDoorbellDev && bo->vram_offset + bo->size <= fDoorbellDev->getLength()) {
            md = RDNA3BoMemory::withBo(this, fDoorbellDev, bo);
            *options = kIOMapInhibitCache;
        }
    } else if (bo && bo->placement != AMDGPU_GEM_DOMAIN_VRAM && bo->backing) {
        md = ((RDNA3Gtt *)bo->backing)->md;
        md->retain();
        *options = 0;
    } else if (bo && fVramDev && bo->vram_offset + bo->size <= fVramDev->getLength()) {
        md = RDNA3BoMemory::withBo(this, fVramDev, bo);
        *options = kIOMapWriteCombineCache;
    } else if (bo && bo->placement == AMDGPU_GEM_DOMAIN_VRAM) {
        /* outside the BAR: the client's map fails (its driver says
         * OUT_OF_DEVICE_MEMORY); say which BO for the report */
        static SInt32 gMapOutside;
        if (OSIncrementAtomic(&gMapOutside) < 8)
            LOG("GEM: map of a %llu KiB VRAM BO at 0x%llx refused: outside the %llu MiB BAR (flags 0x%llx)",
                (unsigned long long)(bo->size >> 10), (unsigned long long)bo->vram_offset,
                (unsigned long long)((fVramDev ? fVramDev->getLength() : 0) >> 20),
                (unsigned long long)bo->flags);
    }
    IOLockUnlock(fDrmLock);
    return md;
}

/* ------------------- the trace ring (card run 2026-10-17b) ------------------- */

static struct rd3_trace_ring gCoreTr;
static IOSimpleLock *gCoreTrLock;
static volatile SInt64 gCoreTrNomem;
static uint64_t gCoreTrCursor;     /* the stream reader's last event number */
static bool gCoreTrSysctl;

void rdna3TraceCore(const struct rd3_trace_ev *in)
{
    IOSimpleLock *lk = gCoreTrLock;
    if (!in || !lk || !gCoreTr.ev) {
        OSIncrementAtomic64(&gCoreTrNomem);
        return;
    }
    const uint64_t t = mach_absolute_time();
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(lk);
    struct rd3_trace_ev *e = rd3_trace_next(&gCoreTr);
    if (e) {
        const uint32_t seq = e->seq;
        *e = *in;
        e->seq = seq;
        e->t = t;
    }
    IOSimpleLockUnlockEnableInterrupt(lk, is);
}

/* one read: the header, then the events (all kept, or those since the
 * stream reader's last read); root only. The window is fixed at the start
 * and copied RD3_TRACE_CHUNK events at a time, interrupts back on between
 * chunks (ECC review round 1: the whole ring under a spinlock with
 * interrupts off); the stream's cursor moves only after the copyout. */
static int rdna3TraceCoreRead(struct sysctl_req *req, bool stream)
{
    if (req->newptr)
        return EPERM;
    if (!kauth_cred_issuser(kauth_cred_get()))
        return EPERM;
    const size_t maxLen = sizeof(struct rd3_trace_hdr) + (size_t)RD3_TRACE_CAP * sizeof(struct rd3_trace_ev);
    if (!req->oldptr)
        return SYSCTL_OUT(req, nullptr, maxLen);
    uint8_t *buf = (uint8_t *)IOMalloc(maxLen);
    if (!buf)
        return ENOMEM;
    bzero(buf, sizeof(struct rd3_trace_hdr));
    struct rd3_trace_hdr *h = (struct rd3_trace_hdr *)buf;
    struct rd3_trace_ev *ev = (struct rd3_trace_ev *)(buf + sizeof *h);
    mach_timebase_info_data_t tb = { 1, 1 };
    clock_timebase_info(&tb);
    uint32_t n = 0;
    uint64_t written = 0, lost = 0, cursor0 = 0, to = 0;
    if (gCoreTrLock && gCoreTr.ev) {
        uint64_t next;
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(gCoreTrLock);
        to = written = gCoreTr.written;
        cursor0 = gCoreTrCursor;
        if (stream) {
            next = rd3_trace_stream_from(&gCoreTr, cursor0, &lost);
        } else {
            next = rd3_trace_oldest(&gCoreTr);
            lost = rd3_trace_lost(&gCoreTr);
        }
        IOSimpleLockUnlockEnableInterrupt(gCoreTrLock, is);
        while (next <= to && n < RD3_TRACE_CAP) {
            const uint32_t want = RD3_TRACE_CAP - n < RD3_TRACE_CHUNK ? RD3_TRACE_CAP - n : RD3_TRACE_CHUNK;
            is = IOSimpleLockLockDisableInterrupt(gCoreTrLock);
            const uint32_t got = rd3_trace_copy_chunk(&gCoreTr, &next, to, ev + n, want, &lost);
            IOSimpleLockUnlockEnableInterrupt(gCoreTrLock, is);
            n += got;
            if (!got)
                break;
        }
    }
    clock_sec_t ws = 0;
    clock_usec_t wus = 0;
    h->magic = RD3_TRACE_MAGIC;
    h->version = RD3_TRACE_VERSION;
    h->ev_size = sizeof(struct rd3_trace_ev);
    h->cap = RD3_TRACE_CAP;
    h->count = n;
    h->written = written;
    h->lost = lost;
    h->now = mach_absolute_time();
    clock_get_calendar_microtime(&ws, &wus);
    h->wall_us = (uint64_t)ws * 1000000ull + (uint64_t)wus;
    h->nomem = (uint64_t)gCoreTrNomem;
    h->source = RD3_TRACE_SRC_CORE;
    h->tb_numer = tb.numer;
    h->tb_denom = tb.denom;
    h->read_kind = stream ? RD3_TRACE_READ_STREAM : RD3_TRACE_READ_ALL;
    h->flags = RD3_TRACE_HDR_CHUNKED;
    h->hdr_size = sizeof *h;
    const int err = SYSCTL_OUT(req, buf, sizeof *h + (size_t)n * sizeof *ev);
    IOFree(buf, maxLen);
    if (!err && stream && gCoreTrLock) {
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(gCoreTrLock);
        if (gCoreTrCursor == cursor0)
            gCoreTrCursor = to;
        IOSimpleLockUnlockEnableInterrupt(gCoreTrLock, is);
    }
    return err;
}
static int rdna3_drmtrace_sysctl SYSCTL_HANDLER_ARGS
{
    (void)oidp; (void)arg1; (void)arg2;
    return rdna3TraceCoreRead(req, false);
}
static int rdna3_drmtrace_stream_sysctl SYSCTL_HANDLER_ARGS
{
    (void)oidp; (void)arg1; (void)arg2;
    return rdna3TraceCoreRead(req, true);
}
/* the SDK's SYSCTL_STRUCT_INIT leaves oid_link to the registration */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
SYSCTL_PROC(_debug, OID_AUTO, rdna3_drmtrace, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED, nullptr,
            0, rdna3_drmtrace_sysctl, "S,rd3_trace",
            "card run 2026-10-17b: command submissions, fences, waits, resets, GPU faults (common/rdna3_trace.h; root)");
SYSCTL_PROC(_debug, OID_AUTO, rdna3_drmtrace_stream, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED,
            nullptr, 0, rdna3_drmtrace_stream_sysctl, "S,rd3_trace",
            "the same, only the events since the last read of this one (root)");
#pragma clang diagnostic pop

bool rdna3TraceCoreInit(void)
{
    if (!gCoreTrLock)
        gCoreTrLock = IOSimpleLockAlloc();
    if (gCoreTrLock && !gCoreTr.ev) {
        struct rd3_trace_ev *ev = (struct rd3_trace_ev *)IOMalloc((size_t)RD3_TRACE_CAP * sizeof *ev);
        if (ev) {
            bzero(ev, (size_t)RD3_TRACE_CAP * sizeof *ev);
            IOInterruptState is = IOSimpleLockLockDisableInterrupt(gCoreTrLock);
            gCoreTr.cap = RD3_TRACE_CAP;
            gCoreTr.written = 0;
            gCoreTrCursor = 0;   /* a new ring: the stream starts at its first event (ECC review round 1) */
            gCoreTr.ev = ev;
            IOSimpleLockUnlockEnableInterrupt(gCoreTrLock, is);
        }
    }
    if (!gCoreTrSysctl) {
        sysctl_register_oid(&sysctl__debug_rdna3_drmtrace);
        sysctl_register_oid(&sysctl__debug_rdna3_drmtrace_stream);
        gCoreTrSysctl = true;
    }
    return gCoreTr.ev != nullptr;
}

void rdna3TraceCoreFini(void)
{
    if (gCoreTrSysctl) {
        sysctl_unregister_oid(&sysctl__debug_rdna3_drmtrace_stream);
        sysctl_unregister_oid(&sysctl__debug_rdna3_drmtrace);
        gCoreTrSysctl = false;
    }
    if (gCoreTrLock && gCoreTr.ev) {
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(gCoreTrLock);
        struct rd3_trace_ev *ev = gCoreTr.ev;
        gCoreTr.ev = nullptr;
        gCoreTr.cap = 0;
        IOSimpleLockUnlockEnableInterrupt(gCoreTrLock, is);
        IOFree(ev, (size_t)RD3_TRACE_CAP * sizeof *ev);
    }
}
