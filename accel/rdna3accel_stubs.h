/*
 * Derived from the NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * AMD (RDNA3-Hackintosh) port: identifiers renamed so nothing collides
 * with a NullMoth install; see PROVENANCE.md. Used with the
 * author's permission for AMD-only work.
 */

#pragma once
#include "IOAccelResource2.h"
#include "IOAccelSysMemory.h"
#include "IOAccelVidMemory.h"
#include "IOAccelLegacySurface.h"
#include "IOAccelLegacyDisplayPipe.h"
#include "IOAccelGLContext2.h"
#include "IOAccel2DContext2.h"
#include "IOAccelCLContext2.h"
#include "IOAccelVideoContext2.h"
class RD3Resource : public IOAccelResource2 {
    OSDeclareDefaultStructors(RD3Resource)
    NM_TAHOE_FWD(RD3Resource)
public:
    unsigned int fPbLogged = 0;
    void rebuildPagingBuffer() APPLE_KEXT_OVERRIDE { if (fPbLogged < 4) { fPbLogged++; ALOG("RD3Resource::rebuildPagingBuffer -- nothing to rebuild until there are page tables (rung 5)"); } }
    uint64_t getLevelOffset(unsigned char a0, unsigned char a1, int* a2) APPLE_KEXT_OVERRIDE {
        const unsigned char *r = (const unsigned char *)this;
        if (a2) {
            unsigned long long pitch = *(const volatile unsigned long long *)(r + 0xb8);
            unsigned base = r[0xae];
            *a2 = (int)(pitch >> ((a1 >= base) ? (unsigned)(a1 - base) : 0u));
        }
        return (unsigned long long)a0 * *(const volatile unsigned long long *)(r + 0xd0);
    }
    uint64_t getBackingLevelOffset(unsigned char a0, unsigned char a1, int* a2) APPLE_KEXT_OVERRIDE {
        const unsigned char *r = (const unsigned char *)this;
        if (a2) {
            unsigned long long pitch = *(const volatile unsigned long long *)(r + 0xc0);
            unsigned base = r[0xae];
            *a2 = (int)(pitch >> ((a1 >= base) ? (unsigned)(a1 - base) : 0u));
        }
        return (unsigned long long)a0 * *(const volatile unsigned long long *)(r + 0xd0);
    }
    uint64_t calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long* a0, unsigned long long* a1) APPLE_KEXT_OVERRIDE {
        const unsigned char *r = (const unsigned char *)this;
        unsigned long long size = *(const volatile unsigned long long *)(r + 0xd0);
        if (a1) *a1 = 1;
        if (a0) *a0 = size;
        return size;
    }
    bool addToAperture() APPLE_KEXT_OVERRIDE;
    void removeFromAperture() APPLE_KEXT_OVERRIDE;
    void *getApertureMemoryDescriptor(unsigned long long *a0) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    void pageoff(IOAccelEvent *, bool, bool *, unsigned long long) APPLE_KEXT_OVERRIDE;
    unsigned int fApLogged = 0;
    unsigned int fApCached = 0;   /* cached aperture descriptors handed out (the trace's rate limit) */
    /* the generation (RD3VidMemory::fAllocGen) of the VRAM the Metal plugin last wrote (its dirty escape):
     * the IOP zero-copy flip's marker (DRIVER-PLAN N38, rd3_iop_flip_ok). A generation, not the allocation
     * cookie: the cookie is the VRAM offset, which a new allocation can have again (ECC review of N38) */
    unsigned long long fPluginWroteGen = 0;
    IOMemoryDescriptor *fApDesc = nullptr;
};
OSDefineMetaClassAndStructors(RD3Resource, IOAccelResource2)
class RD3SysMemory : public IOAccelSysMemory {
    OSDeclareDefaultStructors(RD3SysMemory)
    NM_TAHOE_FWD(RD3SysMemory)
public:
};
OSDefineMetaClassAndStructors(RD3SysMemory, IOAccelSysMemory)
class RD3VidMemory : public IOAccelVidMemory {
    OSDeclareDefaultStructors(RD3VidMemory)
    NM_TAHOE_FWD(RD3VidMemory)
public:
    void              *fRmHandle = nullptr;
    unsigned long long fAllocationCookie = 0;
    void              *fKva      = nullptr;
    unsigned long long fPhys     = 0;
    unsigned long long fBytes    = 0;
    /* this allocation's number (allocPhysical; 0: none): never reused, unlike fAllocationCookie */
    unsigned long long fAllocGen = 0;
    bool fScanout = false;

    bool allocPhysical() APPLE_KEXT_OVERRIDE;
    void deallocPhysical() APPLE_KEXT_OVERRIDE;

    uint64_t getPhysicalSegment(unsigned long long a0, unsigned long long* a1) APPLE_KEXT_OVERRIDE {
        const unsigned long long total = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
        if (!fPhys || a0 >= total) { if (a1) *a1 = 0; return 0; }
        if (a1) *a1 = total - a0;
        return fPhys + a0;
    }
};
OSDefineMetaClassAndStructors(RD3VidMemory, IOAccelVidMemory)
class RD3Surface : public IOAccelLegacySurface {
    OSDeclareDefaultStructors(RD3Surface)
    NM_TAHOE_FWD(RD3Surface)
public:
    /* the swap's vblank sync hook: traced every time (card run 2026-10-17b),
     * logged the first 8 times and every 4096th (it came with every swap) */
    void setSyncCommand(unsigned int* a0, unsigned int& a1, unsigned int& a2, _AMDSurfaceSwapSyncOptions const& a3) const APPLE_KEXT_OVERRIDE {
        (void)a0; (void)a3;
        static volatile SInt32 gSyncLogs;
        const SInt32 k = OSIncrementAtomic(&gSyncLogs);
        if (k < 8 || (k & 4095) == 0) ALOG("RD3Surface::setSyncCommand (%d so far; a1 %u a2 %u)", (int)k + 1, a1, a2);
        rd3AccelTraceSync(this, 0, a1, a2);
    }
    void _pure335() APPLE_KEXT_OVERRIDE { ALOG("RD3Surface::_pure335"); }
    bool copyFromBuffer(int a0, int a1, int a2, int a3, unsigned int a4, unsigned int a5, IOAccelEvent* a6, IOAccelResource2* a7, IOAccelSysMemory* a8, unsigned int a9, unsigned long long a10, unsigned int a11) APPLE_KEXT_OVERRIDE { ALOG("RD3Surface::copyFromBuffer"); return false; }
    bool copyToBuffer(int a0, int a1, int a2, int a3, unsigned int a4, unsigned int a5, IOAccelEvent* a6, IOAccelResource2* a7, IOAccelSysMemory* a8, unsigned int a9, unsigned long long a10, unsigned int a11) APPLE_KEXT_OVERRIDE { ALOG("RD3Surface::copyToBuffer"); return false; }
    void free() APPLE_KEXT_OVERRIDE {
        rd3AccelSurfaceGone(this);   /* its swap -> head answer goes with it */
        IOAccelLegacySurface::free();
    }
    /* clientClose and didTerminate (-> detach_surface, which zeroes the
     * accelerator pointer clientClose unlocks through) never overlap:
     * rd3AccelSurfaceGuard, card run 50's shutdown panic */
    IOReturn clientClose() APPLE_KEXT_OVERRIDE {
        bool g = rd3AccelSurfaceGuard(true, this, "clientClose");
        IOReturn r = IOAccelLegacySurface::clientClose();
        if (g) rd3AccelSurfaceGuard(false, this, nullptr);
        return r;
    }
    bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) APPLE_KEXT_OVERRIDE {
        bool g = rd3AccelSurfaceGuard(true, this, "didTerminate");
        bool r = IOAccelLegacySurface::didTerminate(provider, options, defer);
        if (g) rd3AccelSurfaceGuard(false, this, nullptr);
        return r;
    }
    void submitSwapCopy(IOAccelEvent* a0, IOAccelResource2* a1, IOAccelResource2* a2) APPLE_KEXT_OVERRIDE {
        (void)a0;
        rd3AccelSwapProbe(a1, a2);
        /* the surface's own buffers (as shapeSurface reads them): the torn
         * test reads the buffer the head showed only through one of them */
        rd3AccelSwapToPanel(this, a1, a2, (IOAccelResource2 *const *)((const uint8_t *)this + 0x11C0), 32);
    }
    /* copy_forward's (set_shape bit 12) back-buffer refresh: rd3AccelCopyForward */
    IOReturn submitCopyForward(IOAccelEvent* a0, unsigned int a1, IOAccelResource2* a2, IOAccelResource2* a3,
                               IOAccelBounds const* a4, unsigned int a5) APPLE_KEXT_OVERRIDE {
        (void)a0;
        /* the surface's own buffers (as shapeSurface reads them): copy
         * forward copies only between these (ECC review of 60526e4) */
        return rd3AccelCopyForward(this, a1, a2, a3, (const void *)a4, a5,
                                   *(const volatile uint32_t *)((const uint8_t *)this + 0x11B0),
                                   (IOAccelResource2 *const *)((const uint8_t *)this + 0x11C0), 32);
    }
    void shapeSurface(unsigned int a0, unsigned short a1, unsigned short a2) APPLE_KEXT_OVERRIDE {
        const unsigned w = a1, h = a2;
        uint32_t smode = *(const volatile uint32_t *)((const uint8_t *)this + 0x11B0);
        unsigned bpp = ((smode >> 16) & 0xf) + 1;
        if (bpp < 4 || bpp > 16) bpp = 4;
        unsigned pitch = (unsigned)((w * bpp + 255u) & ~255u);
        unsigned size  = pitch * h;
        IOAccelResource2 **bufs = (IOAccelResource2 **)((uint8_t *)this + 0x11C0);
        unsigned present = 0, shaped = 0;
        for (unsigned i = 0; i < 32; i++) {
            if (!(a0 & (1u << i))) continue;
            IOAccelResource2 *r = OSDynamicCast(IOAccelResource2, bufs[i]);
            if (!r) continue;
            present++;
            if (r->initSurfaceBuffer(this, w, h, size, pitch, 0)) shaped++;
        }
        ALOG("RD3Surface::shapeSurface(mode 0x%x, %u x %u) smode 0x%x bpp %u pitch %u size %u -> %u present, %u shaped",
             a0, w, h, smode, bpp, pitch, size, present, shaped);
        rd3AccelTraceShape(this, a0, w, h, smode, present, shaped, bufs);   /* card run 2026-10-17b (Q5) */
        static int gShapeDumps = 0;
        if (gShapeDumps < 4) { gShapeDumps++;
            const volatile unsigned long long *qq = (const volatile unsigned long long *)((const unsigned char *)this + 0x1160);
            for (unsigned o = 0; o < 0x60; o += 16)
                ALOG("   surf+0x%04x: %016llx %016llx", 0x1160 + o, qq[o / 8], qq[o / 8 + 1]);
        }
    }
    /* the family's virtuals around a present (card run 2026-10-17d): traced
     * and forwarded to IOAccelLegacySurface's own, always (naked wrappers in
     * rdna3-accel.cpp, "the flip-present hooks": the base's return registers
     * pass back untouched whatever its real return type) */
    void didSubmitSwap(unsigned int a0, unsigned int a1) APPLE_KEXT_OVERRIDE;
    bool isBackBufferReady(unsigned int a0) APPLE_KEXT_OVERRIDE;
    void submitUpdate(unsigned int a0, IOAccelBounds* a1, unsigned int a2) APPLE_KEXT_OVERRIDE;
    void pickPresentType(unsigned int a0) APPLE_KEXT_OVERRIDE;
    uint32_t getDirtyBufferBitsFromPrivateModeBits(unsigned long long a0) APPLE_KEXT_OVERRIDE {
        static volatile SInt32 gDirtyLogs;
        const SInt32 k = OSIncrementAtomic(&gDirtyLogs);
        if (k < 8 || (k & 4095) == 0)
            ALOG("RD3Surface::getDirtyBufferBitsFromPrivateModeBits(0x%llx) -> 0 (%d so far)", (unsigned long long)a0, (int)k + 1);
        rd3AccelTraceSync(this, 1, a0 & 0xffffffffull, a0 >> 32);
        return 0;
    }
};
OSDefineMetaClassAndStructors(RD3Surface, IOAccelLegacySurface)
class RD3DisplayPipe : public IOAccelLegacyDisplayPipe {
    OSDeclareDefaultStructors(RD3DisplayPipe)
    NM_TAHOE_FWD(RD3DisplayPipe)
public:
    IOAccelMemory *initFramebufferResource(unsigned int index, IOAccelResource2 *res) APPLE_KEXT_OVERRIDE;
    IOReturn validateTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    IOReturn performTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    bool     isTransactionComplete(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    void     free() APPLE_KEXT_OVERRIDE;
    /* the family's flip-buffer present path (card run 2026-10-17d: copy
     * forward bursts with no swap): traced, counted, forwarded to
     * IOAccelLegacyDisplayPipe's own always; debug.rdna3accel_flipbuf_present=1
     * also presents the buffer (naked wrappers in rdna3-accel.cpp) */
    void     submitScanoutFlipBuffer(IOAccelEvent *a0, unsigned int a1, IOAccelResource2 *a2,
                                     IOAccelResource2 *a3) APPLE_KEXT_OVERRIDE;
    void     submitFlipBuffer(IOAccelEvent *a0, unsigned int a1, IOAccelResource2 *a2,
                              IOAccelResource2 *a3) APPLE_KEXT_OVERRIDE;
    enum { kIopSrcSlots = 4 };
    struct RD3IopSrc { unsigned id; IOMemoryDescriptor *md; IOMemoryMap *map; } fIopSrc[kIopSrcSlots];
    unsigned fIopNext;
    const void *iopMapSurface(unsigned id, IOMemoryDescriptor *md, unsigned long long need);
    void        iopDropSources();
};
OSDefineMetaClassAndStructors(RD3DisplayPipe, IOAccelLegacyDisplayPipe)
class RD3GLContext : public IOAccelGLContext2 {
    OSDeclareDefaultStructors(RD3GLContext)
    NM_TAHOE_FWD(RD3GLContext)
public:
    void _pure362() APPLE_KEXT_OVERRIDE { ALOG("RD3GLContext::_pure362"); }
    void _pure363() APPLE_KEXT_OVERRIDE { ALOG("RD3GLContext::_pure363"); }
    void _pure364() APPLE_KEXT_OVERRIDE { ALOG("RD3GLContext::_pure364"); }
    void _pure367() APPLE_KEXT_OVERRIDE { ALOG("RD3GLContext::_pure367"); }
};
OSDefineMetaClassAndStructors(RD3GLContext, IOAccelGLContext2)
class RD32DContext : public IOAccel2DContext2 {
    OSDeclareDefaultStructors(RD32DContext)
    NM_TAHOE_FWD(RD32DContext)
public:
    void _pure362() APPLE_KEXT_OVERRIDE { ALOG("RD32DContext::_pure362"); }
    void _pure363() APPLE_KEXT_OVERRIDE { ALOG("RD32DContext::_pure363"); }
};
OSDefineMetaClassAndStructors(RD32DContext, IOAccel2DContext2)
class RD3CLContext : public IOAccelCLContext2 {
    OSDeclareDefaultStructors(RD3CLContext)
    NM_TAHOE_FWD(RD3CLContext)
public:
};
OSDefineMetaClassAndStructors(RD3CLContext, IOAccelCLContext2)
class RD3VideoContext : public IOAccelVideoContext2 {
    OSDeclareDefaultStructors(RD3VideoContext)
    NM_TAHOE_FWD(RD3VideoContext)
public:
};
OSDefineMetaClassAndStructors(RD3VideoContext, IOAccelVideoContext2)
