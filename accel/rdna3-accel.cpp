/*
 * Derived from the NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * AMD (RDNA3-Hackintosh) port: identifiers renamed so nothing collides
 * with a NullMoth install; see PROVENANCE.md. Used with the
 * author's permission for AMD-only work.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include "../common/rdna3_bootargs.h"
#include "../common/rdna3_a3_display.h"
#include <sys/proc.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <libkern/version.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelTask.h"
#include "IOAccelMemoryMap.h"
#define RD3K_SS_KERNEL_ABI 1
#include "rdna3k_abi.h"
class RD3MemoryMap;
static unsigned long long rd3AccelGpuVaAlloc(IOAccelMemoryMap *map,
                                           unsigned long long len, unsigned long long align);
static void               rd3AccelGpuVaFree(IOAccelMemoryMap *map);
static bool               rd3AccelFlipTo(IOAccelResource2 *res, IOService *fb, unsigned head);
static bool               rd3AccelCopyToScanout(IOAccelResource2 *res, IOService *fb, unsigned head);
void                      rd3AccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2);
void                      rd3AccelSwapToPanel(const void *surf, IOAccelResource2 *a1, IOAccelResource2 *a2);
void                      rd3AccelSurfaceGone(const void *surf);
bool                      rd3AccelSurfaceGuard(bool enter, const void *surf, const char *who);
void                      rd3AccelResourceGone(const void *res);
bool                      rd3AccelResHasContent(IOAccelResource2 *res);
IOReturn                  rd3AccelCopyForward(const void *surf, unsigned fbIndex, IOAccelResource2 *src,
                                              IOAccelResource2 *dst, const void *rects, unsigned count,
                                              unsigned smode);
static void               rd3AccelCopyForwardCheck(IOAccelResource2 *res);
static void               rd3AccelNotePresented(const void *vm, unsigned head);
static void               rd3AccelA3DispDirty(unsigned surfaceID, unsigned long long phys);
/* why rd3AccelA3Policy refused an import: picks the counter and the text of
 * the refusal's log line (card run 58b review: a WindowServer caller whose
 * task did not match was logged and counted as "a WindowServer input") */
enum RD3A3Why { kA3WhyNone = 0, kA3WhyOff, kA3WhyTaskMismatch, kA3WhyNoName, kA3WhyDisplay, kA3WhyWs };
static unsigned           rd3AccelA3Policy(void *surf, task_t task, unsigned surfaceID, unsigned *headsOut, unsigned *whyOut);
static SInt64             rd3AccelA3WhyCount(unsigned why);
static void               rd3AccelA3Why(char *why, unsigned long n, unsigned reason, unsigned heads, void *surf);
static bool               rd3AccelA3RefusedFirst(unsigned surfaceID, SInt64 n);
void                      rd3AccelVidmemGone(const void *vm);
static void               rd3AccelWriteStamp(int index, unsigned int value);
static bool               rd3AccelLocksInit(void);
/* Navi 3x display devices (data/amdgpu.ids); the core kext must also claim the card */
static bool rd3SupportedDevice(unsigned dev)
{
    switch (dev) {
    case 0x7448: case 0x744c: case 0x745e:            /* Navi 31 */
    case 0x7470: case 0x747e:                         /* Navi 32: RX 7800 XT / 7700 XT */
    case 0x7480: case 0x7483: case 0x7489:            /* Navi 33 */
        return true;
    default:
        return false;
    }
}
/* accel/Makefile: hash of this kext's sources, so a report tells which
 * build the Auxiliary KC actually loaded (tools/triage_reports.py) */
#ifndef ACCEL_SRC
#define ACCEL_SRC "unknown"
#endif
#define ALOG(fmt, ...) do { kprintf("RDNA3Accelerator: " fmt "\n", ##__VA_ARGS__); IOLog("RDNA3Accelerator: " fmt "\n", ##__VA_ARGS__); } while (0)
#include <libkern/c++/OSSymbol.h>
#include <IOKit/IODeviceMemory.h>
#include "tahoe_fwd.h"   // macOS 26: inherited slots Apple no longer exports, forwarded through its vtables
#include "rdna3accel_stubs.h"
#include <sys/sysctl.h>
#include <libkern/OSAtomic.h>
static volatile SInt64 gCntVmAlloc, gCntVmAllocBytes, gCntVmDealloc, gCntVmDeallocBytes, gCntVmRefused,
                       gCntResNew, gCntResFree, gCntMapCommit, gCntMapRelease, gCntMapFree;
static void rdna3accelInstallSysctl(void);
/* the raw offsets into Apple's objects (re/, iofam/, +0x88 / +0x98 /
 * +0xc8 ...) and the vtable slots were measured on one Darwin release per
 * build: macOS 15 (Darwin 24), or macOS 26 (Darwin 25) for the NM_TAHOE
 * build. On any other they would read and write the wrong fields */
#ifdef NM_TAHOE
static const int kRd3VerifiedDarwin = 25;
#else
static const int kRd3VerifiedDarwin = 24;
#endif
static int gRd3AccelArmed = 0;
static const unsigned kRd3MaxFB = 4;
static unsigned gRd3HeadsPublished = 0, gRd3HeadsRegistered = 0;
static unsigned gRd3HeadsPiped = 0;

extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(io.github.voten641.RDNA3Accelerator, "0.1", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

class RD3EventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(RD3EventMachine)
    NM_TAHOE_FWD(RD3EventMachine)
public:
    void writeStamp(int s, vendevtCommandRec *r, unsigned int v) APPLE_KEXT_OVERRIDE {
        (void)r;
        rd3AccelWriteStamp(s, v);
    }
    void prepareBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM prepareBarrier(%p)", b); }
    void completeBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM completeBarrier(%p)", b); }
    void writeBarrierElement(vendevtBarrierRec *b, int i, unsigned int v) APPLE_KEXT_OVERRIDE { ALOG("EM writeBarrierElement(%p, %d, %u)", b, i, v); }
    bool checkGPUProgress() APPLE_KEXT_OVERRIDE { return true; }
    bool checkChannelProgress(int ch) APPLE_KEXT_OVERRIDE { return true; }
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE { ALOG("EM eventTimeout(%d) — ignored, we do not use family channels", stamp); return nullptr; }
};
OSDefineMetaClassAndStructors(RD3EventMachine, IOAccelEventMachineFast2)
static void (*gEnableAccel)(const char *why) = nullptr;
static int gRd3Scanout = 0;
/* boot-args rdna3accelfb=<n> / rdna3acceliop=1: what `sysctl debug.rdna3accelfb=<n>`
 * and `debug.rdna3accel_iop=1` do, applied as soon as head 0's framebuffer
 * publishes (before WindowServer opens it) - the VM run has no shell to type
 * them in once the desktop is black */
static int gBootAccelFb = 0, gBootIop = 0;
/* boot-arg rdna3accelsdma=1 (A2): swap copies onto a scanout by SDMA in the
 * core kext (rdna3CopyVram) instead of the CPU through the BAR */
static int gBootSdmaCopy = 0;
/* boot-arg rdna3accelcf (card run 54): 1 (default) RD3Surface::submitCopyForward
 * copies the rects by SDMA (the CPU for small ones when SDMA refuses), 0 answers
 * kIOReturnUnsupported as the family's own did (logged, nothing copied) */
static int gBootCopyForward = 1;
/* card run 58 (2026-10-15c, the first run of the A3 plugin): WindowServer's
 * display framebuffer IOSurfaces became BOs over their own VRAM, which no
 * swap ever scans out (the legacy surface's buffers are other allocations,
 * filled from the IOSurface's system memory), so the main screen kept an old
 * frame. common/rdna3_a3_display.h: such a surface stays out of A3 unless
 * gA3Display (boot-arg rdna3a3display=1, sysctl debug.rdna3accel_a3display).
 * Each head's scanout size, from initFramebufferResource and its swaps, says
 * which WindowServer surfaces are display framebuffers. The A3 display
 * surfaces imported anyway (gA3Display=1) are remembered with their VRAM, so
 * the swap path can say whether the panel ever shows what WindowServer drew
 * ("a3 display:" lines, debug.rdna3accel_swaps_*). */
static int gA3Display = 0;
/* card run 58 (2026-10-15d, flicker on both screens): WindowServer's other
 * inputs out of A3 unless gA3Ws (rdna3a3ws=1); gA3On 0 (rdna3a3=0) keeps
 * every import out. rdna3_a3_display.h, rd3_a3_ws_policy. */
static int gA3Ws = 0, gA3On = 1;
static SInt64 gCntA3WsRefused, gCntA3OffRefused;
/* the flicker test of each head's legacy swaps (rd3_flicker_abab): the
 * sample hashes of its last two pictures, how often the newest one was the
 * picture of two swaps before */
enum { kFlickBands = 8, kFlickCols = 48, kFlickRows0 = 4, kFlickRows = 2, kFlickBudget = 3000 };
/* written only from a head's own swaps (one WindowServer thread per surface
 * swap): not locked, a torn hash costs one A B A B more or less */
static uint32_t gSwapHash1[kRd3MaxFB][kFlickBands], gSwapHash2[kRd3MaxFB][kFlickBands];
static uint64_t gSwapHashRun[kRd3MaxFB];   /* hashes in a row; a skip starts again */
static SInt64 gCntSwapAbab[kRd3MaxFB], gCntSwapHashed[kRd3MaxFB], gCntSwapHashSkip[kRd3MaxFB];
/* of those A B A B swaps, the ones where more than one band alternated: a
 * picture taking turns, not a blinking caret or one small status item */
static SInt64 gCntSwapAbabWide[kRd3MaxFB];
/* sysctl debug.rdna3accel_flicker: 0 off, 1 (default) the first 3000
 * hashed swaps of each head, 2 every swap (the samples cost ~1000 BAR
 * reads per swap) */
static int gFlicker = 1;
/* the size of each head's own legacy swaps (only a swap that named the head's
 * framebuffer resource): the second source next to the framebuffers' answers */
static volatile uint32_t gHeadSwapW[kRd3MaxFB], gHeadSwapH[kRd3MaxFB];
static SInt64 gCntA3DispRefused, gCntA3DispImported, gCntA3WsOther, gCntA3TaskMismatch, gCntA3NoName;
static SInt64 gCntSwapHead[kRd3MaxFB], gCntSwapA3Head[kRd3MaxFB];
enum { kA3DispSlots = 4 * 4 };   /* 4 * kRd3MaxFB: every head's compositing surfaces */
struct RD3A3Disp { unsigned surfaceID, heads; unsigned long long phys; const void *vm; };
static RD3A3Disp gA3Disp[kA3DispSlots];
static unsigned gA3DispNext;
static IOSimpleLock *volatile gA3DispLock;
static unsigned long long gA3HeadShown[kRd3MaxFB];   /* the VRAM each head's last swap scanned out */
static unsigned gA3DirtySurf[kRd3MaxFB];             /* the display surface WindowServer marked drawn last (each head it may be) */
static unsigned long long gA3DirtyPhys[kRd3MaxFB];   /* and its VRAM */
static volatile UInt32 gBootGatesQueued = 0;   /* bit i: head i's gates queued */
static void rd3AccelBootGates(void *, void *);
extern "C" {
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter(struct thread_call *call);
}
static int gRd3Aperture = 0;
class RD3DisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(RD3DisplayMachine)
    NM_TAHOE_FWD(RD3DisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { ALOG("DM displayModeWillChange"); return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        ALOG("DM displayModeDidChange -> true");
        if (gEnableAccel) gEnableAccel("displayModeDidChange");
        return true;
    }
};
OSDefineMetaClassAndStructors(RD3DisplayMachine, IOAccelLegacyDisplayMachine)
#define RD3_PRIVATE_VA_BASE  0x800000000000ull
#define RD3_PRIVATE_VA_SIZE  0x000010000000ull
class RD3Task : public IOAccelTask {
    OSDeclareDefaultStructors(RD3Task)
    NM_TAHOE_FWD(RD3Task)
public:
    IORangeAllocator *fVa = nullptr;
    unsigned fAllocLogged = 0, fPrivate = 0;
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE {
        const unsigned long long len = map ? ((IOAccelMemoryMap *)map)->getLength() : 0;
        const unsigned long long align = map ? *(const volatile unsigned long long *)((const unsigned char *)map + 0x80) : 0;
        unsigned long long got = rd3AccelGpuVaAlloc((IOAccelMemoryMap *)map, len, align);
        if (!got && fAllocLogged < 12)
            ALOG("RD3Task::allocate: RM refused %llu bytes (align %llu) -- no GPU address", len, align);
        return got;
    }
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE {
        (void)addr;
        rd3AccelGpuVaFree((IOAccelMemoryMap *)map);
    }
};
OSDefineMetaClassAndStructors(RD3Task, IOAccelTask)
class RD3MemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(RD3MemoryMap)
    NM_TAHOE_FWD(RD3MemoryMap)
public:
    IOAccelMemory     *fMem       = nullptr;
    unsigned int       fHVirt     = 0;
    unsigned int       fHRmMemory = 0;
    unsigned long long fGpuVa     = 0;
    unsigned long long fVaLen     = 0;
    bool               fMapped    = false;
    unsigned int fCommits = 0;
    bool init(IOGraphicsAccelerator2 *a, IOAccelTask *t, IOAccelMemory *m,
              unsigned int o) APPLE_KEXT_OVERRIDE {
        if (!IOAccelMemoryMap::init(a, t, m, o)) return false;
        fMem = m;
        if (fMem) fMem->retain();
        return true;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fMem) { fMem->release(); fMem = nullptr; }
        IOAccelMemoryMap::free();
    }
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE;
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE;
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return fMapped; }
};
OSDefineMetaClassAndStructors(RD3MemoryMap, IOAccelMemoryMap)

#define RD3_CARRIERS 32
class IOAccelRDNA3Carrier : public IOService {
    OSDeclareDefaultStructors(IOAccelRDNA3Carrier)
};
OSDefineMetaClassAndStructors(IOAccelRDNA3Carrier, IOService)

class RDNA3Accelerator : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(RDNA3Accelerator)
    NM_TAHOE_FWD(RDNA3Accelerator)
    IOAccelRDNA3Carrier *fCarriers[RD3_CARRIERS] = { nullptr };
    unsigned            fCarrierNext = 0;
    unsigned            fCarrierLogged = 0;
public:
    IOBufferMemoryDescriptor *fStamps = nullptr; IORangeAllocator *fRanges[32] = {};
    uint64_t  getStampMemory(unsigned int *p) APPLE_KEXT_OVERRIDE {
        if (!fStamps) { fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 16384, 4096); if (fStamps) { fStamps->prepare(); bzero(fStamps->getBytesNoCopy(), 16384); } }
        if (p) *p = fStamps ? 4096 : 0;
        ALOG("getStampMemory(%p) -> descriptor %p (%u stamps, 16 KB)", p, fStamps, p ? *p : 0); return (uint64_t)fStamps; }
    OSObject *newEventMachine() APPLE_KEXT_OVERRIDE { RD3EventMachine *m = OSTypeAlloc(RD3EventMachine); ALOG("newEventMachine -> %p (family inits it)", m); return m; }
    OSObject *newGPUTask(const char *who) {
        for (unsigned i = 0; i < 32; i++) if (!fRanges[i]) {
            fRanges[i] = IORangeAllocator::withRange(1ULL << 40, 4096);
            if (fRanges[i]) fRanges[i]->allocateRange(0, 0x100000);
        }
        RD3Task *t = OSTypeAlloc(RD3Task); bool ok = t && t->init(this, 1, fRanges);
        ALOG("%s -> %p init=%d", who, t, ok); if (t && !ok) { t->release(); t = nullptr; } return t; }
    OSObject *createUserGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createUserGPUTask"); }
    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p) { ALOG("populateAccelConfig(NULL) — nothing to do"); return; }
        if (((uintptr_t)p & 0xFFF) + 0x70 > 0x1000) {
            ALOG("populateAccelConfig(%p) — 0x70 bytes would cross out of this page; REFUSING to touch it", p);
            return;
        }
        ALOG("populateAccelConfig(%p) — what the FAMILY pre-filled, before we write a byte:", p);
        for (unsigned i = 0; i < 0x70; i += 8)
            ALOG("   cfg+0x%02x = 0x%016llx", i, *(const uint64_t *)(p + i));

        *(uint64_t *)(p + 0x00) = (uint64_t)(uintptr_t)"AMDRadeonRDNA3Accelerator";
        *(uint32_t *)(p + 0x08) = 0x30844300u;
        *(uint32_t *)(p + 0x0c) = 15u;
        *(uint32_t *)(p + 0x18) = 8u;
        *(uint32_t *)(p + 0x30) = 0x40004000u;
        *(uint32_t *)(p + 0x34) = 0x00008000u;
        *(uint32_t *)(p + 0x38) = 0x00008000u;
        *(uint8_t  *)(p + 0x44) = 0;
        *(uint8_t  *)(p + 0x46) = 1;
        *(uint8_t  *)(p + 0x47) = 1;
        *(uint64_t *)(p + 0x48) = 0x0000001F0000000FULL;
        *(uint64_t *)(p + 0x50) = 0x7FFFFFFF0000001FULL;
        *(uint64_t *)(p + 0x58) = 0x0000000C00000000ULL;
        *(uint32_t *)(p + 0x64) = 0x4Bu;
        *(uint32_t *)(p + 0x28) = kRd3MaxFB;
        ALOG("populateAccelConfig(%p) — filled: AccelCaps=15, flags=0x30844300, maxtex=16384x16384, maxfb=%u written (cfg+0x2c left %u)",
             p, kRd3MaxFB, *(const uint32_t *)(p + 0x2c));
    }
    bool      configureDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("configureDevice(%p) %04x:%04x — RM owns the card (RDNA3Accel.kext); nothing to do here yet", pci, pci ? pci->configRead16(0) : 0, pci ? pci->configRead16(2) : 0); return true; }
    void      teardownDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("teardownDevice(%p)", pci); }
    OSObject *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createKernelGPUTask"); }
    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE {
        RD3DisplayPipe *p = OSTypeAlloc(RD3DisplayPipe);
        if (!p) { ALOG("newDisplayPipe: OSTypeAlloc FAILED - the family will store this NULL unchecked"); return NULL; }
        ALOG("newDisplayPipe -> %p", p); return p; }
    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE { RD3DisplayMachine *d = OSTypeAlloc(RD3DisplayMachine);
        fDM = d;
        ALOG("newDisplayMachine -> %p", d); return d; }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE { RD3GLContext *c = OSTypeAlloc(RD3GLContext); ALOG("newGLContext -> %p", c); return c; }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE { RD3CLContext *c = OSTypeAlloc(RD3CLContext); ALOG("newCLContext -> %p", c); return c; }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE { RD3Surface *o = OSTypeAlloc(RD3Surface); ALOG("newSurface -> %p", o); return o; }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE { RD32DContext *c = OSTypeAlloc(RD32DContext); ALOG("new2DContext -> %p", c); return c; }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE { RD3VideoContext *c = OSTypeAlloc(RD3VideoContext); ALOG("newVideoContext -> %p", c); return c; }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE { RD3SysMemory *m = OSTypeAlloc(RD3SysMemory); ALOG("newSysMemory -> %p", m); return m; }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE { RD3VidMemory *m = OSTypeAlloc(RD3VidMemory); ALOG("newVidMemory -> %p", m); return m; }
    OSObject *newResource() APPLE_KEXT_OVERRIDE { RD3Resource *r = OSTypeAlloc(RD3Resource); OSAddAtomic64(1, &gCntResNew); ALOG("newResource -> %p", r); return r; }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE { RD3MemoryMap *m = OSTypeAlloc(RD3MemoryMap); ALOG("newMemoryMap -> %p", m); return m; }

    RD3DisplayMachine *fDM = nullptr;
    IOPCIDevice *fPCI = nullptr;
    IONotifier *fFBNote = nullptr;
    static RDNA3Accelerator *gAccel;
    IOService *fFB = nullptr;
    IOService *volatile fFBs[kRd3MaxFB] = {};
    unsigned fFBRegistered = 0;
    IOLock *fRegLock = nullptr;
    OSObject *fMetalPlugin = nullptr;
    static bool fbPublished(void *target, void *ref, IOService *newService, IONotifier *note) {
        (void)ref; (void)note;
        RDNA3Accelerator *me = (RDNA3Accelerator *)target;
        if (!me || !newService) return true;
        unsigned idx = 0;
        OSNumber *num = OSDynamicCast(OSNumber, newService->getProperty("IOFBDependentIndex"));
        if (num) idx = num->unsigned32BitValue();
        const char *what = "kept";
        if (idx >= kRd3MaxFB) what = "OUT OF RANGE -- this head will get no pipe";
        else {
            newService->retain();
            if (OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFBs[idx])) OSBitOrAtomic(1u << idx, &gRd3HeadsPublished);
            else { newService->release(); what = "DUPLICATE -- the first one keeps the slot"; }
        }
        newService->retain();
        if (idx == 0 && me->fFBs[0] == newService) {
            IOService *old;
            do { old = me->fFB; } while (!OSCompareAndSwapPtr(old, newService, (void *volatile *)&me->fFB));
            if (old == newService) newService->release();
            else if (old) {
                bool slotted = false;
                for (unsigned j = 0; j < kRd3MaxFB; j++) if (me->fFBs[j] == old) slotted = true;
                ALOG("fFB: head 0 (%s) displaced %s, which published first (%s)", newService->getName(), old->getName(),
                     slotted ? "its extra reference returned" : "NOT slotted: keeping its reference rather than risk a reader");
                if (slotted) old->release();
            }
        /* only head 0 may become the primary framebuffer (NullMoth 249ef8c):
         * another head publishing first stayed fFB on their machines */
        } else if (idx != 0 || !OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFB)) newService->release();
        unsigned before = me->fDM ? me->fDM->getFramebufferCount() : 0xFFFFFFFFu;
        ALOG("%s published -- display machine framebuffer count = %u (index %u %s)", newService->getName(), before, idx, what);
        if (gRd3AccelArmed)
            ALOG("  index %u published AFTER the arm: it gets a pipe only when debug.rdna3accelfb=3 is written again, "
                 "and WindowServer composites it only after a WindowServer restart", idx);
        else
            ALOG("  to register it:  sudo sysctl -w debug.rdna3accelfb=1   (2 = re-run the family's own sweep)");
        /* every head that publishes (the extended desktop's head 1 comes after
         * the clone, well after head 0) gets the same treatment */
        if (gBootAccelFb && idx < kRd3MaxFB && !(OSBitOrAtomic(1u << idx, &gBootGatesQueued) & (1u << idx))) {
            struct thread_call *tc = thread_call_allocate(rd3AccelBootGates, nullptr);
            if (tc) { thread_call_enter(tc); ALOG("  boot-args rdna3accelfb=%d rdna3acceliop=%d: applying them now for head %u", gBootAccelFb, gBootIop, idx); }
            else ALOG("  boot-arg rdna3accelfb=%d: no thread call - use the sysctl", gBootAccelFb);
        }
        return true;
    }
    void deliverModeChange() {
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        ALOG("  delivering display_mode_will_change/did_change (nest %u, pipes %u)",
             *(const volatile uint32_t *)(dm + 0x10c), *(const volatile uint32_t *)(dm + 0x108));
        fDM->display_mode_will_change(0);
        fDM->display_mode_did_change(0);
        reportPipeGates("after deliverModeChange");
    }
    void reportPipeGates(const char *when) {
        uint8_t accelFlags = *(const volatile uint8_t *)((const uint8_t *)this + 0xc78);
        ALOG("  GATES %s: accel+0xc78 = 0x%02x (enabled bit 0x2 %s)",
             when, accelFlags, (accelFlags & 0x2) ? "SET" : "CLEAR <- kIOReturnNotReady");
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        uint32_t n = *(const volatile uint32_t *)(dm + 0x108);
        if (n > 8) { ALOG("  GATES %s: implausible pipe count %u -- not walking it", when, n); return; }
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *p = *(uint8_t **)(dm + 0x88 + 8 * i);
            if (!p) { ALOG("  GATES %s: pipe %u is NULL", when, i); continue; }
            ALOG("  GATES %s: pipe %u active(+0x298) = %u, wsaaDefer(+0x282) = %u",
                 when, i, *(const volatile uint8_t *)(p + 0x298), *(const volatile uint8_t *)(p + 0x282));
        }
    }
    /* the pipe slot whose framebuffer (pipe + 0x98, as the walk below reads
     * it) is 'fb', or -1 */
    int pipeOfFramebuffer(IOService *fb) {
        if (!fDM || !fb) return -1;
        uint8_t *dm = (uint8_t *)fDM;
        uint32_t n = *(const volatile uint32_t *)(dm + 0x108);
        if (n > 16) return -1;
        for (uint32_t k = 0; k < n; k++) {
            uint8_t *p = *(uint8_t *const volatile *)(dm + 0x88 + 8 * k);
            if (p && *(IOService *const volatile *)(p + 0x98) == fb)
                return (int)k;
        }
        return -1;
    }
    unsigned registerPendingLocked(const char *why) {
        unsigned added = 0;
        for (unsigned i = 0; i < kRd3MaxFB; i++) {
            IOService *fb = fFBs[i];
            if (!fb) {
                if (gRd3HeadsPublished >> i)
                    ALOG("  %s: index %u has not published (published 0x%x): stopping, so no head above it gets a pipe "
                         "this walk", why, i, gRd3HeadsPublished);
                break;
            }
            if (fFBRegistered & (1u << i)) continue;
            /* the family may have registered this framebuffer itself and
             * given it a pipe already (card run 39: head 0 was known, the
             * count stayed 1 -> 1, the head was never marked piped, and
             * "only head piped" sent both displays' frames to head 1 -
             * one monitor flickering, the other black) */
            {
                int have = pipeOfFramebuffer(fb);
                if (have >= 0) {
                    fFBRegistered |= 1u << i;
                    gRd3HeadsRegistered = fFBRegistered;
                    gRd3HeadsPiped |= 1u << i;
                    ALOG("  %s: index %u already has pipe %d (registered by the family): marked piped", why, i, have);
                    added++;
                    continue;
                }
            }
            unsigned b = fDM->getFramebufferCount();
            ALOG("  %s: calling found_framebuffer(%s index %u) -- count before %u", why, fb->getName(), i, b);
            fDM->found_framebuffer((IOFramebuffer *)fb);
            unsigned c = fDM->getFramebufferCount();
            fFBRegistered |= 1u << i;
            gRd3HeadsRegistered = fFBRegistered;
            if (c != b + 1) {
                ALOG("  %s: index %u -- count went %u -> %u; the family did not add it (already known, or the cap "
                     "accel+0xcb0 = %u refused it). Stopping here so no later head takes this one's pipe number.",
                     why, i, b, c, *(const volatile uint32_t *)((const uint8_t *)this + 0xcb0));
                break;
            }
            uint8_t *dm = (uint8_t *)fDM;
            void *pipe = (b < 16) ? *(void *const volatile *)(dm + 0x88 + 8 * b) : nullptr;
            if (!pipe) {
                *(volatile uint32_t *)(dm + 0x108) = b;
                ALOG("  %s: index %u -- the family stored a NULL pipe in slot %u (0xe3b5, unchecked) and counted it; "
                     "count put back %u -> %u so no broadcast dereferences it (0xe320). This head gets no pipe this boot.",
                     why, i, b, c, fDM->getFramebufferCount());
                break;
            }
            IOService *pfb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
            if (pfb != fb) {
                ALOG("  %s: index %u -- pipe %u (%p) drives %p, not %s %p; keeping the pipe (it is real), stopping "
                     "the walk so no later head's pipe number is off by this one", why, i, b, pipe, pfb, fb->getName(), fb);
                break;
            }
            ALOG("  %s: index %u -> pipe %u (%p), count %u", why, i, b, pipe, c);
            gRd3HeadsPiped |= 1u << i;
            added++;
        }
        return added;
    }
    void onCountGrewLocked() {
        deliverModeChange();
        gRd3AccelArmed = 1;
        if (fMetalPlugin && !getProperty("MetalPluginName")) {
            setProperty("MetalPluginName", fMetalPlugin);
            ALOG("  MetalPluginName restored -- a NEW process that asks Metal for this accelerator "
                 "can now load the plugin (already-running ones will not)");
        }
        if (!getProperty("IOGLBundleName")) {
            setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
            ALOG("  IOGLBundleName=AppleMetalOpenGLRenderer -- NEW GL processes get a hardware renderer over Metal");
        }
        ALOG("  debug.rdna3accelfb now reads 1 -- armed-only Metal clients may have a device THIS BOOT ONLY "
             "(heads published 0x%x, registered 0x%x, piped 0x%x)", gRd3HeadsPublished, gRd3HeadsRegistered, gRd3HeadsPiped);
    }
    void registerFramebuffer(int how) {
        if (fRegLock) IOLockLock(fRegLock);
        registerFramebufferLocked(how);
        if (fRegLock) IOLockUnlock(fRegLock);
    }
    bool registerFramebufferLocked(int how) {
        if (!fDM) { ALOG("registerFramebuffer(%d): no display machine", how); return false; }
        if (!fFB) { ALOG("registerFramebuffer(%d): no framebuffer has published yet", how); return false; }
        unsigned before = fDM->getFramebufferCount();
        ALOG("registerFramebuffer(%d): framebuffer count before = %u", how, before);
        if (how == 2) {
            if (before != 0) ALOG("  already %u -- refusing to re-run the family's sweep", before);
            else { ALOG("  re-running IOAccelDisplayMachine::start(pci) -- the family's own sweep"); fDM->start(fPCI); }
        } else {
            registerPendingLocked("sysctl");
        }
        unsigned after = fDM->getFramebufferCount();
        ALOG("  framebuffer count after = %u", after);
        if (after > before) { onCountGrewLocked(); return true; }
        return false;
    }
    const OSSymbol *fSymAlloc = nullptr, *fSymFree = nullptr;
    /* VRAM belongs to the core kext (RDNA3Accel), not to a framebuffer:
     * on AMD the allocator lives next to the GEM heap. */
    IOService *core() {
        if (!fCore) {
            OSDictionary *m = serviceMatching("RDNA3Accel");
            if (m) { fCore = waitForMatchingService(m, 100ULL * 1000 * 1000); m->release(); }
        }
        return fCore;
    }
    IOReturn askFramebufferForVram(struct RD3KVramRequest *r, bool alloc) {
        IOService *c = core();
        if (!c) return kIOReturnNotAttached;
        if (!fSymAlloc) fSymAlloc = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_ALLOC);
        if (!fSymFree)  fSymFree  = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_FREE);
        const OSSymbol *fn = alloc ? fSymAlloc : fSymFree;
        if (!fn) return kIOReturnNoMemory;
        return c->callPlatformFunction(fn, false, r, nullptr, nullptr, nullptr);
    }
    IOReturn askFramebufferForScanout(struct RD3KVramRequest *r, IOService *fb = nullptr) {
        if (!fb) fb = fFB;
        if (!fb) return kIOReturnNotAttached;
        if (!fSymScanout) fSymScanout = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_SCANOUT);
        return fb->callPlatformFunction(fSymScanout, false, r, nullptr, nullptr, nullptr);
    }
    const OSSymbol *fSymScanout = nullptr;
    IOService *pipeFramebuffer(const void *pipe, unsigned *idx) {
        if (!pipe) return nullptr;
        IOService *fb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
        if (!fb) return nullptr;
        for (unsigned i = 0; i < kRd3MaxFB; i++)
            if (fFBs[i] == fb) { if (idx) *idx = i; return fb; }
        return nullptr;
    }
    IOService *fCore = nullptr;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE {
        if ((type & 0xFFFF0000u) != RD3K_UC_TYPE_BASE) {
            IOReturn fr = IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);
            if (fUcLogged < 40) { fUcLogged++;
                ALOG("newUserClient(type %u / 0x%x) -> family returned 0x%x, client %s",
                     (unsigned)type, (unsigned)type, (unsigned)fr,
                     (handler && *handler) ? (*handler)->getMetaClass()->getClassName() : "(none)"); }
            return fr;
        }

        if (!fCore) {
            OSDictionary *m = serviceMatching("RDNA3Accel");
            if (m) fCore = waitForMatchingService(m, 100ULL * 1000 * 1000);
        }
        if (!fCore) { ALOG("newUserClient(0x%x): no RDNA3Accel service", type); return kIOReturnNotAttached; }

        const OSSymbol *fnSym = OSSymbol::withCStringNoCopy(RD3K_FN_NEW_CLIENT);
        if (!fnSym) return kIOReturnNoMemory;
        IOUserClient *uc = nullptr;
        IOReturn r = fCore->callPlatformFunction(fnSym, false, (void *)owningTask,
                                                 (void *)(uintptr_t)(type & 0xFFFFu), &uc, nullptr);
        fnSym->release();
        if (r != kIOReturnSuccess || !uc) { ALOG("newUserClient(0x%x): RDNA3Accel refused (0x%x)", type, r); return r ? r : kIOReturnNoResources; }

        IOService *carrier = nullptr;
        for (unsigned i = 0; i < RD3_CARRIERS && !carrier; i++)
            carrier = fCarriers[(fCarrierNext + i) % RD3_CARRIERS];
        fCarrierNext++;
        if (!carrier) {
            ALOG("newUserClient(0x%x): NO CARRIER available -- refusing rather than attaching to the "
                 "accelerator, which sleeps at ~1019 children", type);
            uc->release(); return kIOReturnNoResources;
        }
        if (!uc->attach(carrier))  { ALOG("newUserClient(0x%x): attach failed", type); uc->release(); return kIOReturnInternalError; }
        if (!uc->start(carrier))   { ALOG("newUserClient(0x%x): start failed", type); uc->detach(carrier); uc->release(); return kIOReturnInternalError; }
        ALOG("newUserClient(0x%x) -> %s on the ACCELERATOR (rm type %u)", type, uc->getMetaClass()->getClassName(), type & 0xFFFFu);
        *handler = uc;
        return kIOReturnSuccess;
    }
    static void triggerFromSysctl(int how) {
        if (!gAccel) { ALOG("sysctl debug.rdna3accelfb=%d but no RDNA3Accelerator is running", how); return; }
        if (how >= 4) gRd3Aperture = 1;
        if (how >= 3) {
            gRd3Scanout = 1;
            ALOG("  level %d: registering any published head that has no pipe yet", how);
            if (gAccel->fRegLock) IOLockLock(gAccel->fRegLock);
            if (gAccel->registerFramebufferLocked(1))
                ALOG("sysctl debug.rdna3accelfb=%d: scanout-backed display pipe ENABLED; the registration just delivered the mode "
                     "change -- not delivering it twice", how);
            else {
                ALOG("sysctl debug.rdna3accelfb=%d: scanout-backed display pipe ENABLED, re-delivering the mode change", how);
                gAccel->deliverModeChange();
            }
            if (gAccel->fRegLock) IOLockUnlock(gAccel->fRegLock);
            return;
        }
        gAccel->registerFramebuffer(how);
    }
    static void enableFromHook(const char *why) {
        if (!gAccel) return;
        gAccel->enableAccelerator();
        gAccel->fEnableCount++;
        ALOG("enableAccelerator() from %s (call #%u)", why, gAccel->fEnableCount);
        gAccel->setProperty("RDNA3AccelEnabledFrom", why);
        gAccel->setProperty("RDNA3AccelEnableCount", gAccel->fEnableCount, 32);
    }
    unsigned fEnableCount = 0;
    unsigned fUcLogged = 0;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        ALOG("probe(%s) score %d", provider ? provider->getName() : "?", score ? (int)*score : 0);
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            const unsigned cls = pci->configRead8(0x0B), sub = pci->configRead8(0x0A);
            const unsigned ven = pci->configRead16(0x00), dev = pci->configRead16(0x02);
            if (cls != 0x03 || (sub != 0x00 && sub != 0x02) || ven != 0x1002 || !rd3SupportedDevice(dev)) {
                ALOG("%04x:%04x class %02x.%02x is not an RDNA3 display device we support - not claiming",
                     ven, dev, cls, sub);
                return NULL;
            }
            ALOG("RDNA3 display device %04x:%04x class %02x.%02x - claiming", ven, dev, cls, sub);
        }
        return IOGraphicsAccelerator2::probe(provider, score);
    }
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        ALOG("build %s", ACCEL_SRC);
        setProperty("RDNA3,AccelSource", ACCEL_SRC);
        { char off[8]; if (PE_parse_boot_argn("-rdna3off", off, sizeof off)) return false; }
        /* on in the default profile (no rdna3lvl, rdna3_bootargs.h); a test step
         * without rdna3accel=1 keeps it off (boot-loop guard) */
        if (!rd3_arg("rdna3accel", 0)) { ALOG("rdna3accel off: NOT starting (boot-loop guard)"); return false; }
        /* kRd3VerifiedDarwin: the release this build's offsets were measured on */
        if (version_major != kRd3VerifiedDarwin && !rd3_arg("rdna3accelanyos", 0)) {
            ALOG("Darwin %d, but this build's object offsets were verified on Darwin %d only: NOT starting "
                 "(rdna3accelanyos=1 overrides)", version_major, kRd3VerifiedDarwin);
            return false;
        }
        if (!rd3AccelLocksInit()) { ALOG("no memory for the module's locks: NOT starting"); return false; }
        gBootAccelFb = (int)rd3_arg("rdna3accelfb", (uint32_t)gBootAccelFb);
        gBootIop = (int)rd3_arg("rdna3acceliop", (uint32_t)gBootIop);
        gBootSdmaCopy = (int)rd3_arg("rdna3accelsdma", 0);
        gBootCopyForward = (int)rd3_arg("rdna3accelcf", 1);
        gA3Display = rd3_a3_display_on() ? 1 : 0;
        gA3Ws = rd3_a3_ws_on() ? 1 : 0;
        gA3On = rd3_a3_on() ? 1 : 0;
        ALOG("A3: %s (card run 58, 2026-10-15d; boot-args rdna3a3ws / rdna3a3, sysctl debug.rdna3accel_a3ws / _a3)",
             !gA3On ? "OFF for every process (rdna3a3=0): every IOSurface goes through copies"
             : gA3Ws ? "WindowServer's other inputs MAY become BOs (rdna3a3ws=1: the 2026-10-15d flicker)"
                     : "WindowServer's inputs are kept out (composited through copies); applications keep A3 "
                       "for their own and shared surfaces");
        ALOG("A3: WindowServer's display framebuffers %s (card run 58; boot-arg rdna3a3display / sysctl "
             "debug.rdna3accel_a3display)", gA3Display
             ? "MAY become BOs over their own VRAM, which no legacy swap scans out (rdna3a3display=1: the run 58 picture)"
             : "are kept out of A3: WindowServer composites them through copies the legacy swap pages on");
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            unsigned waited = 0;
            while (pci->getProperty("rdna3-claimed") != kOSBooleanTrue && waited < 300) { IOSleep(100); waited++; }
            if (pci->getProperty("rdna3-claimed") != kOSBooleanTrue) {
                ALOG("RDNA3Accel never claimed %04x:%04x in 30 s - NOT starting the accelerator on a GPU with no RM",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("RDNA3Accel owns %04x:%04x (waited %u ms) - starting", pci->configRead16(0x00), pci->configRead16(0x02), waited * 100);
            /* the card's name and VRAM size the core kext put on the PCI device: Apple's AMD
             * accelerators carry them too, and System Information reads them there */
            static const char *const kIdKeys[] = { "VRAM,totalMB", "VRAM,totalsize", "model" };
            char have[64] = "";
            for (unsigned k = 0; k < sizeof kIdKeys / sizeof kIdKeys[0]; k++) {
                OSObject *v = pci->getProperty(kIdKeys[k]);
                if (v) setProperty(kIdKeys[k], v);
                strlcat(have, k ? (v ? " +" : " -") : (v ? "+" : "-"), sizeof have);
                strlcat(have, kIdKeys[k], sizeof have);
            }
            ALOG("identity keys from the PCI device (+ copied, - absent): %s", have);
        }
        ALOG("start(%s): calling IOGraphicsAccelerator2::start — the family's own bring-up", provider ? provider->getName() : "?");
        bool r = IOGraphicsAccelerator2::start(provider);
        ALOG("IOGraphicsAccelerator2::start -> %d", r);
        if (r) {
            registerService(); ALOG("registerService() called — Metal can now find MetalPluginName=%s", "RDNA3MTLDriver");
            gAccel = this; gEnableAccel = &RDNA3Accelerator::enableFromHook;
            for (unsigned i = 0; i < RD3_CARRIERS; i++) {
                IOAccelRDNA3Carrier *c = OSTypeAlloc(IOAccelRDNA3Carrier);
                if (!c) break;
                if (!c->init()) { c->release(); break; }
                if (!c->attach(this)) { c->release(); break; }
                if (!c->start(this)) { c->detach(this); c->release(); break; }
                fCarriers[i] = c;
            }
            { unsigned n = 0; for (unsigned i = 0; i < RD3_CARRIERS; i++) if (fCarriers[i]) n++;
              ALOG("user-client carriers: %u of %u ready (each holds ~1019 clients before "
                   "IOService::attach starts sleeping)", n, (unsigned)RD3_CARRIERS); }
            enableFromHook("start");
            fPCI = OSDynamicCast(IOPCIDevice, provider);
            ALOG("display machine framebuffer count after the family sweep = %u", fDM ? fDM->getFramebufferCount() : 0xFFFFFFFFu);
            fRegLock = IOLockAlloc();
            ALOG("multi-head: up to %u heads, registration lock %p%s", kRd3MaxFB, fRegLock,
                 fRegLock ? "" : " -- NULL: two sysctl writers could interleave a registration");
            OSDictionary *fbm = serviceMatching("RDNA3Framebuffer");
            if (fbm) {
                fFBNote = addMatchingNotification(gIOFirstPublishNotification, fbm, &RDNA3Accelerator::fbPublished, this, nullptr, 0);
                fbm->release();
                ALOG("watching for RDNA3Framebuffer to publish -> notifier %p", fFBNote);
                rdna3accelInstallSysctl();
                if (getProperty("MetalPluginName")) {
                    OSObject *mp = getProperty("MetalPluginName"); if (mp) mp->retain();
                    fMetalPlugin = mp;
                    removeProperty("MetalPluginName");
                    ALOG("MetalPluginName withheld -- nothing will load the Metal plugin until "
                         "`sysctl -w debug.rdna3accelfb=1` puts it back, and a reboot takes it away again");
                }
            } else ALOG("serviceMatching(RDNA3Framebuffer) returned NULL -- cannot watch for the framebuffer");
        }
        return r;
    }
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE { ALOG("stop"); IOGraphicsAccelerator2::stop(provider); }
    virtual IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(RDNA3Accelerator, IOGraphicsAccelerator2)
RDNA3Accelerator *RDNA3Accelerator::gAccel = nullptr;

/* the module's global locks: made once, by start() before the family can
 * call in, with a compare-and-swap so two makers never both publish one */
static IOSimpleLock *rd3SimpleLockOnce(IOSimpleLock *volatile *p)
{
    if (!*p) {
        IOSimpleLock *l = IOSimpleLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)p)) IOSimpleLockFree(l);
    }
    return *p;
}
static IOLock *rd3LockOnce(IOLock *volatile *p)
{
    if (!*p) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)p)) IOLockFree(l);
    }
    return *p;
}

/* the display pipe's framebuffer resource of each head (initFramebufferResource):
 * a legacy surface swap names no head, its arguments may be this resource.
 * gHeadFbResLock: a resource leaves the table (RD3Resource::free) before it
 * is freed, so one looked at under the lock is alive */
static IOAccelResource2 *volatile gHeadFbRes[kRd3MaxFB];
static IOSimpleLock *volatile gHeadFbResLock;
IOAccelMemory *RD3DisplayPipe::initFramebufferResource(unsigned int index, IOAccelResource2 *res)
{
    if (!gRd3Scanout) {
        ALOG("RD3DisplayPipe::initFramebufferResource(index %u): scanout gate CLOSED -- returning NULL, "
             "which is the family's own default (sysctl debug.rdna3accelfb=3 opens it)", index);
        return nullptr;
    }
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a || !res) { ALOG("RD3DisplayPipe::initFramebufferResource: no accelerator or no resource"); return nullptr; }
    unsigned head = 0;
    IOService *fb = a->pipeFramebuffer(this, &head);
    if (!fb) {
        ALOG("RD3DisplayPipe::initFramebufferResource(index %u): pipe %p drives framebuffer %p, which is none of ours -- "
             "refusing (never head 0's pages: that IS the bug)", index, this,
             *(void *const volatile *)((const uint8_t *)this + 0x98));
        return nullptr;
    }

    struct RD3KVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = RD3K_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&r, fb);
    if (ret != kIOReturnSuccess || !r.phys || !r.actualSize) {
        ALOG("RD3DisplayPipe::initFramebufferResource: framebuffer would not describe its scanout (0x%x)", (unsigned)ret);
        return nullptr;
    }

    OSObject *o = a->createVidMemory(nullptr, res, r.actualSize, nullptr);
    RD3VidMemory *m = OSDynamicCast(RD3VidMemory, o);
    if (!m) {
        ALOG("RD3DisplayPipe::initFramebufferResource: createVidMemory gave %p, not an RD3VidMemory", o);
        if (o) o->release();
        return nullptr;
    }
    if (head < kRd3MaxFB && gHeadFbResLock) {
        IOSimpleLockLock(gHeadFbResLock);
        gHeadFbRes[head] = res;   /* the swap path finds its head by this resource */
        IOSimpleLockUnlock(gHeadFbResLock);
    }
    m->fScanout = true;
    m->fPhys    = r.phys;
    m->fBytes   = r.actualSize;
    m->fAllocationCookie = r.allocationCookie;   /* VRAM offset: copy forward's SDMA source or target */
    *(volatile unsigned long long *)((unsigned char *)m + 0xc8) = r.phys;
    ALOG("RD3DisplayPipe::initFramebufferResource(head %u, index %u, res %p) -> vidmem %p over the LIVE SCANOUT "
         "phys 0x%llx %u x %u pitch %u (%llu bytes)",
         head, index, res, m, (unsigned long long)r.phys, r.width, r.height, r.pitch,
         (unsigned long long)r.actualSize);
    return (IOAccelMemory *)m;
}

static RD3VidMemory *rd3res_vidmem(IOAccelResource2 *r)
{
    if (!r) return nullptr;
    OSObject *o = *(OSObject **)((unsigned char *)r + 0x88);
    return OSDynamicCast(RD3VidMemory, o);
}

bool RD3Resource::addToAperture()
{
    RD3VidMemory *m = rd3res_vidmem(this);
    if (fApLogged < 4) { fApLogged++;
        ALOG("RD3Resource::addToAperture -> true (vidmem %p phys 0x%llx %llu bytes, already in BAR1)",
             m, m ? m->fPhys : 0ull, m ? m->fBytes : 0ull); }
    return m && m->fPhys && m->fBytes;
}

void RD3Resource::removeFromAperture() { }

void *RD3Resource::getApertureMemoryDescriptor(unsigned long long *a0)
{
    if (a0) *a0 = 0;
    if (!gRd3Aperture) {
        if (fApLogged < 4) { fApLogged++;
            ALOG("RD3Resource::getApertureMemoryDescriptor: aperture gate CLOSED -- returning nullptr "
                 "(sysctl debug.rdna3accelfb=4 opens it)"); }
        return nullptr;
    }
    if (fApDesc) return fApDesc;
    RD3VidMemory *m = rd3res_vidmem(this);
    if (!m || !m->fPhys || !m->fBytes) {
        if (fApLogged < 8) { fApLogged++;
            ALOG("RD3Resource::getApertureMemoryDescriptor: no BAR1-backed vidmem at +0x88 (mem %p)", m); }
        return nullptr;
    }
    IODeviceMemory *d = IODeviceMemory::withRange((IOPhysicalAddress)m->fPhys, (IOPhysicalLength)m->fBytes);
    if (!d) { ALOG("RD3Resource::getApertureMemoryDescriptor: IODeviceMemory::withRange(0x%llx, %llu) FAILED",
                   m->fPhys, m->fBytes); return nullptr; }
    fApDesc = d;
    if (fApLogged < 8) { fApLogged++;
        ALOG("RD3Resource::getApertureMemoryDescriptor -> %p over BAR1 phys 0x%llx (%llu bytes), offset 0",
             d, m->fPhys, m->fBytes); }
    return fApDesc;
}

static void rd3SVForgetResource(RD3Resource *resource);

void RD3Resource::free()
{
    rd3SVForgetResource(this);
    if (fApDesc) { fApDesc->release(); fApDesc = nullptr; }
    rd3AccelResourceGone(this);
    OSAddAtomic64(1, &gCntResFree);
    IOAccelResource2::free();
}

bool RD3VidMemory::allocPhysical()
{
    if (fScanout) {
        if (!fPhys || !fBytes) { ALOG("RD3VidMemory::allocPhysical(%p): scanout with no address", this); return false; }
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
        ALOG("RD3VidMemory::allocPhysical(%p): SCANOUT phys 0x%llx (%llu bytes) -- already backed, nothing allocated",
             this, fPhys, fBytes);
        return true;
    }
    if (fRmHandle) {
        ALOG("RD3VidMemory::allocPhysical(%p): already backed by 0x%llx (%llu bytes) -- false, as AMD's does",
             this, fPhys, fBytes);
        return false;
    }
    const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
    if (!want) { ALOG("RD3VidMemory::allocPhysical(%p): length at +0x40 is 0 -- nothing to back", this); return false; }
    if (!RDNA3Accelerator::gAccel) { ALOG("RD3VidMemory::allocPhysical(%p): no accelerator", this); return false; }

    struct RD3KVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = RD3K_VRAM_ABI_VERSION;
    r.size    = want;
    IOReturn ret = RDNA3Accelerator::gAccel->askFramebufferForVram(&r, true);
    if (ret != kIOReturnSuccess || !r.handle || !r.phys) {
        OSAddAtomic64(1, &gCntVmRefused);
        ALOG("RD3VidMemory::allocPhysical(%p): %llu bytes REFUSED by the framebuffer (0x%x)",
             this, want, (unsigned)ret);
        return false;
    }
    fRmHandle = r.handle; fKva = r.kva; fPhys = r.phys; fBytes = r.actualSize; fAllocationCookie = r.allocationCookie;
    OSAddAtomic64(1, &gCntVmAlloc); OSAddAtomic64((SInt64)fBytes, &gCntVmAllocBytes);
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
    ALOG("RD3VidMemory::allocPhysical(%p): %llu bytes of REAL VRAM -- phys 0x%llx kva %p [BAR1 %llu]",
         this, fBytes, fPhys, fKva, r.mappedTotal);
    return true;
}

void RD3VidMemory::deallocPhysical()
{
    rd3AccelVidmemGone(this);
    if (fScanout) {
        ALOG("RD3VidMemory::deallocPhysical(%p): SCANOUT -- not ours to free, only forgetting it", this);
        fPhys = 0; fBytes = 0; fScanout = false;
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
        return;
    }
    if (!fRmHandle) return;
    OSAddAtomic64(1, &gCntVmDealloc); OSAddAtomic64((SInt64)fBytes, &gCntVmDeallocBytes);
    struct RD3KVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = RD3K_VRAM_ABI_VERSION;
    r.handle = fRmHandle; r.kva = fKva; r.actualSize = fBytes; r.allocationCookie = fAllocationCookie;
    if (RDNA3Accelerator::gAccel) RDNA3Accelerator::gAccel->askFramebufferForVram(&r, false);
    fRmHandle = nullptr; fKva = nullptr; fPhys = 0; fBytes = 0; fAllocationCookie = 0;
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
}

class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
};
class IOAccelDisplayPipeTransaction2 {
public:
    IOSurface *getPlaneIOSurface(unsigned int, unsigned int) const;
    OSObject  *getPlaneResource(unsigned int, unsigned int) const;
};

static int    gRd3Iop = 0;
static SInt64 gCntIopOk, gCntIopFail, gCntIopEmpty, gCntIopSrcRes, gCntIopSrcSurf, gCntIopBlank;
static SInt64 gCntIopOkHead[kRd3MaxFB];
#define RD3IOP_LOG_MAX 40
static unsigned gIopLogs = 0;
#define RD3IOPLOG(fmt, ...) do { if (gIopLogs < RD3IOP_LOG_MAX) { gIopLogs++; ALOG("iop: " fmt, ##__VA_ARGS__); } } while (0)
#define RD3IOP_FAIL(rc, fmt, ...) do { OSAddAtomic64(1, &gCntIopFail); RD3IOPLOG(fmt, ##__VA_ARGS__); return (rc); } while (0)

void RD3DisplayPipe::iopDropSources()
{
    for (unsigned i = 0; i < kIopSrcSlots; i++) {
        RD3IopSrc *e = &fIopSrc[i];
        if (e->map) { e->map->release(); e->map = nullptr; }
        if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
        e->id = 0;
    }
}

void RD3DisplayPipe::free()
{
    iopDropSources();
    IOAccelLegacyDisplayPipe::free();
}

const void *RD3DisplayPipe::iopMapSurface(unsigned id, IOMemoryDescriptor *md, unsigned long long need)
{
    for (unsigned i = 0; i < kIopSrcSlots; i++)
        if (fIopSrc[i].md == md && fIopSrc[i].id == id && fIopSrc[i].map)
            return (fIopSrc[i].map->getLength() >= need) ? (const void *)fIopSrc[i].map->getVirtualAddress() : nullptr;
    if (md->getLength() < need) return nullptr;
    RD3IopSrc *e = &fIopSrc[fIopNext % kIopSrcSlots]; fIopNext++;
    if (e->map) { e->map->release(); e->map = nullptr; }
    if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
    e->id = 0;
    if (md->prepare() != kIOReturnSuccess) return nullptr;
    IOMemoryMap *m = md->map(kIOMapReadOnly);
    if (!m) { md->complete(); return nullptr; }
    md->retain(); e->md = md; e->map = m; e->id = id;
    RD3IOPLOG("mapped source surface 0x%x (%llu bytes) into slot %u", id, (unsigned long long)m->getLength(), (fIopNext - 1) % kIopSrcSlots);
    return (const void *)m->getVirtualAddress();
}

IOReturn RD3DisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *)
{
    return kIOReturnSuccess;
}

static bool rd3AccelIopAsyncBusy(const void *pipe);
bool RD3DisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *)
{
    return gRd3Iop != 0 && !rd3AccelIopAsyncBusy(this);
}

static bool rd3AccelSurfCopyReady(IOSurface *s, unsigned head);
static bool rd3AccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe);
static void rd3AccelIopHome(IOService *fb, unsigned head);

/* The base class's performTransaction for a plane-less (gamma) transaction.
 * Card run 30: with this display pipe active, macOS sends the gamma tables
 * in display-pipe transactions without a plane (debug.rdna3accel_iop_empty
 * +6 for 3 tables x 2 heads) and never calls IOFramebuffer::setGammaTable;
 * without the accelerator setGammaTable reaches the framebuffer and the
 * MPCC OGAM works. IOAcceleratorFamily2's own implementation presumably
 * hands such a transaction's gamma to the pipe's IOFramebuffer, so with
 * gamma on (the default; rdna3gamma=0 turns it off) it gets the
 * transaction instead of the "done" we answer.
 * Card run 34: from the Auxiliary KC the weak imports of
 * IOAccelLegacyDisplayPipe / IOAccelDisplayPipe::performTransaction passed
 * the OSKextSymbolIsResolved test although neither symbol exists on 24H32,
 * and the call jumped into kernel data (NX page fault in
 * IOAccelDisplayPipe::event_interrupt_gated): ld -kext leaves a
 * RIP-relative reference to an undefined symbol unrelocated (see
 * gen_tahoe_fwd.py). So no symbol is named any more: the implementation
 * is read from IOAccelLegacyDisplayPipe's exported vtable, loaded through
 * the GOT (which the kernel linker fills) as a weak reference, at the
 * slot our own override occupies - exactly the function the family itself
 * would call for a plain IOAccelLegacyDisplayPipe. */
extern "C" {
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
}
static void *const *rd3LegacyPipeVtable(void)
{
    void *const *vt;
    __asm__(".weak_reference __ZTV24IOAccelLegacyDisplayPipe\n\t"
            "movq __ZTV24IOAccelLegacyDisplayPipe@GOTPCREL(%%rip), %0" : "=r"(vt));
    return vt;
}
/* __cxa_pure_virtual (libkern exports it), through the GOT like the vtable:
 * a pure virtual slot holds it, and calling it panics */
static const void *rd3PureVirtual(void)
{
    const void *p;
    __asm__("movq ___cxa_pure_virtual@GOTPCREL(%%rip), %0" : "=r"(p));
    return p;
}
/* the byte offset of performTransaction from a vtable's address point
 * (Itanium ABI: a virtual member pointer holds 1 + that offset) */
static long rd3PerformTransactionSlot(void)
{
    IOReturn (IOAccelDisplayPipe::*pm)(IOAccelDisplayPipeTransaction2 *) = &IOAccelDisplayPipe::performTransaction;
    struct { uintptr_t ptr; long adj; } raw;
    static_assert(sizeof(pm) == sizeof(raw), "Itanium member function pointer");
    memcpy(&raw, &pm, sizeof(raw));
    if (!(raw.ptr & 1) || raw.adj)
        return -1;
    return (long)(raw.ptr - 1);
}
static int    gIopGammaBase = -1;   /* rd3_gamma_on(), read on first use */
static SInt64 gCntIopEmptyBase, gCntIopEmptyBaseErr;
/* card run 35: a forward target must lie inside IOAcceleratorFamily2's code,
 * i.e. near a function the kext imports strongly from it (the kernel linker
 * resolves that one or refuses to load us). One MiB, as decided in run 35;
 * a refusal logs the distance, so a legitimate target farther away shows
 * in the report instead of being jumped to blindly. */
static const uintptr_t kRd3FamilyCodeSpan = 1u << 20;
static uintptr_t rd3FamilyDistance(const void *p)
{
    const uintptr_t anchor = (uintptr_t)&_ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv;
    const uintptr_t a = (uintptr_t)p;
    return a > anchor ? a - anchor : anchor - a;
}
/* the base class's performTransaction as read off IOAccelLegacyDisplayPipe's
 * vtable; why == nullptr: it may be called */
struct Rd3IopBase {
    void *const *vt;
    long        slot;
    void       *fn;    /* the slot's content (also when refused, for the log) */
    const char *why;
};
static Rd3IopBase rd3IopBaseTarget(void)
{
    Rd3IopBase b = { nullptr, -1, nullptr, nullptr };
    /* rdna3accelanyos=1 lets the accelerator start on another release, but
     * the slot comes from this build's headers: never jump through it there */
    if (version_major != kRd3VerifiedDarwin) {
        b.why = "the vtable layout was verified on another Darwin release";
        return b;
    }
    b.vt = rd3LegacyPipeVtable();
    b.slot = rd3PerformTransactionSlot();
    if (!b.vt || (const void *)b.vt == gOSKextUnresolved || (const void *)b.vt == (const void *)&gOSKextUnresolved) {
        b.why = "IOAccelLegacyDisplayPipe's vtable is not exported";
        return b;
    }
    if (b.slot < 0 || (b.slot & 7)) {
        b.why = "no performTransaction slot";
        return b;
    }
    /* the address point is 2 words in (offset to top, RTTI) */
    b.fn = b.vt[2 + b.slot / 8];
    if (!b.fn || b.fn == gOSKextUnresolved || b.fn == (const void *)&gOSKextUnresolved)
        b.why = "the slot is unresolved";
    else if (b.fn == rd3PureVirtual())   /* pure in the base on this release (PR #2 review) */
        b.why = "the slot is pure virtual";
    else if (rd3FamilyDistance(b.fn) >= kRd3FamilyCodeSpan)
        b.why = "the slot points outside IOAcceleratorFamily2";
    return b;
}
static IOReturn rd3AccelIopGammaToBase(void *pipe, IOAccelDisplayPipeTransaction2 *txn, bool *handled)
{
    typedef IOReturn (*PerformFn)(void *, IOAccelDisplayPipeTransaction2 *);
    const char *which = "IOAccelLegacyDisplayPipe vtable";
    *handled = false;
    if (gIopGammaBase < 0)
        gIopGammaBase = rd3_gamma_on() ? 1 : 0;
    if (!gIopGammaBase)
        return kIOReturnSuccess;
    const Rd3IopBase b = rd3IopBaseTarget();
    static SInt32 gIopBaseLogs;
    static volatile UInt32 gIopNoBaseLogged;
    if (b.why) {
        if (OSCompareAndSwap(0, 1, &gIopNoBaseLogged))
            ALOG("iop: gamma on but no base performTransaction: %s (Darwin %d, verified %d, vtable 0x%llx, slot %ld, "
                 "target 0x%llx, %llu bytes from signalTransactionInterrupt, unresolved 0x%llx): plane-less "
                 "transaction answered as before",
                 b.why, version_major, kRd3VerifiedDarwin, (unsigned long long)(uintptr_t)b.vt, b.slot,
                 (unsigned long long)(uintptr_t)b.fn, b.fn ? (unsigned long long)rd3FamilyDistance(b.fn) : 0ull,
                 (unsigned long long)(uintptr_t)gOSKextUnresolved);
        return kIOReturnSuccess;
    }
    const IOReturn r = ((PerformFn)b.fn)(pipe, txn);
    *handled = true;
    OSAddAtomic64(1, r == kIOReturnSuccess ? &gCntIopEmptyBase : &gCntIopEmptyBaseErr);
    if (OSIncrementAtomic(&gIopBaseLogs) < 8)
        ALOG("iop: plane-less transaction handed to %s::performTransaction -> 0x%x", which, (unsigned)r);
    return r;
}

IOReturn RD3DisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    if (!gRd3Iop) return (IOReturn)0xe00002c7;
    if (!txn) RD3IOP_FAIL(kIOReturnBadArgument, "performTransaction(NULL)");
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    unsigned head = 0;
    IOService *fb = a ? a->pipeFramebuffer(this, &head) : nullptr;
    if (!fb) RD3IOP_FAIL(kIOReturnNotAttached, "no accelerator, or pipe %p drives a framebuffer that is none of ours", this);

    IOSurface *s  = txn->getPlaneIOSurface(0, 0);
    OSObject  *ro = txn->getPlaneResource(0, 0);
    if (!s && !ro) {
        /* card runs 26-28: the gamma tables never reached the framebuffer;
         * a transaction with no plane carries gamma/options only - the
         * first ones dumped so the layout can be read off a report */
        const SInt64 e = OSAddAtomic64(1, &gCntIopEmpty);
        static SInt32 gIopEmptyDumps;
        if (e < 4 && OSIncrementAtomic(&gIopEmptyDumps) < 4) {
            const volatile uint32_t *w = (const volatile uint32_t *)txn;
            for (unsigned o = 0; o < 48; o += 8)
                ALOG("iop: empty #%lld +0x%03x: %08x %08x %08x %08x %08x %08x %08x %08x", (long long)e, o * 4,
                     w[o], w[o + 1], w[o + 2], w[o + 3], w[o + 4], w[o + 5], w[o + 6], w[o + 7]);
        }
        /* gamma on (rdna3gamma=0 off): the base class gets it (rd3AccelIopGammaToBase) */
        bool handled;
        const IOReturn br = rd3AccelIopGammaToBase(this, txn, &handled);
        return handled ? br : kIOReturnSuccess;
    }

    const volatile unsigned int *src = nullptr; unsigned w = 0, h = 0, pitch = 0; bool fromRes = false;
    RD3Resource *res = OSDynamicCast(RD3Resource, ro);
    RD3VidMemory *vm = res ? rd3res_vidmem(res) : nullptr;
    static SInt32 gIopK6Logs;
    if (gIopK6Logs < 6) {
        OSIncrementAtomic(&gIopK6Logs);
        const unsigned char *kr = (const unsigned char *)res;
        ALOG("iop: K6 plane 0: surface %s id 0x%x %ux%u row %u fmt 0x%08x | resource %s w %u h %u +0xb8 %u +0xc0 %u vidmem %llu bytes kva %d scanout %d",
             s ? "yes" : "NO", s ? (unsigned)s->getSurfaceID() : 0u, s ? (unsigned)s->getWidth() : 0u, s ? (unsigned)s->getHeight() : 0u,
             s ? (unsigned)s->getBytesPerRow() : 0u, s ? (unsigned)s->getPixelFormat() : 0u,
             ro ? ro->getMetaClass()->getClassName() : "(none)",
             res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb0) : 0u, res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb2) : 0u,
             res ? *(const volatile unsigned int *)(kr + 0xb8) : 0u, res ? *(const volatile unsigned int *)(kr + 0xc0) : 0u,
             vm ? (unsigned long long)vm->fBytes : 0ull, (vm && vm->fKva) ? 1 : 0, vm ? (int)vm->fScanout : -1);
    }
    if (!s && vm && vm->fKva) {
        const unsigned char *rr = (const unsigned char *)res;
        w     = *(const volatile unsigned short *)(rr + 0xb0);
        h     = *(const volatile unsigned short *)(rr + 0xb2);
        pitch = *(const volatile unsigned int   *)(rr + 0xb8);
        if ((unsigned long long)pitch < 4ull * w) RD3IOP_FAIL(kIOReturnBadArgument, "resource %ux%u: +0xb8 reads %u, which is not a row pitch (need >= %u) — not a surface buffer", w, h, pitch, 4u * w);
        if ((unsigned long long)h * pitch > vm->fBytes) RD3IOP_FAIL(kIOReturnOverrun, "resource %ux%u pitch %u exceeds its %llu bytes", w, h, pitch, (unsigned long long)vm->fBytes);
        src = (const volatile unsigned int *)vm->fKva; fromRes = true;
    } else if (s && rd3AccelIopFlip(fb, head, s, this)) {
        OSAddAtomic64(1, &gCntIopSrcSurf); OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]);
        return kIOReturnSuccess;
    } else if (s) {
        /* NullMoth c2af33d: a copied frame must come from the surface's
         * newest pixels. Copying while they were still only in VRAM put a
         * stale, one-colour buffer on the panel every other frame (their
         * full-screen flicker on the copy path, nvaccel_iop_blank about half
         * of the copied frames). Such a frame keeps the previous picture. */
        if (!rd3AccelSurfCopyReady(s, head)) return kIOReturnSuccess;
        const unsigned fmt = s->getPixelFormat();
        if (fmt != 0x42475241u) RD3IOP_FAIL(kIOReturnUnsupported, "surface 0x%x format 0x%08x is not BGRA", s->getSurfaceID(), fmt);
        const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
        if (!W || !H || P < 4 || W > 16384 || H > 16384 || P > 16384ull * 8) RD3IOP_FAIL(kIOReturnBadArgument, "surface geometry %llux%llu pitch %llu", W, H, P);
        if (P < 4 * W || (P & 3)) RD3IOP_FAIL(kIOReturnBadArgument, "surface %llux%llu: row pitch %llu is under 4*W or not word-aligned", W, H, P);
        IOMemoryDescriptor *md = s->getMemoryDescriptor();
        if (!md) RD3IOP_FAIL(kIOReturnNoMemory, "surface 0x%x has no memory descriptor", s->getSurfaceID());
        src = (const volatile unsigned int *)iopMapSurface(s->getSurfaceID(), md, H * P);
        if (!src) RD3IOP_FAIL(kIOReturnVMError, "surface 0x%x would not prepare/map %llu bytes", s->getSurfaceID(), H * P);
        w = (unsigned)W; h = (unsigned)H; pitch = (unsigned)P;
    } else {
        RD3IOP_FAIL(kIOReturnUnsupported, "plane resource %p is not an RD3Resource with a CPU mapping, and there is no IOSurface", ro);
    }
    if (!w || !h || pitch < 4) RD3IOP_FAIL(kIOReturnBadArgument, "source geometry %ux%u pitch %u", w, h, pitch);

    struct RD3KVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = RD3K_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q, fb);
    if (ret != kIOReturnSuccess || !q.kva || q.pitch < 4 || !q.height) RD3IOP_FAIL(kIOReturnNotReady, "framebuffer would not describe its scanout (0x%x kva %p pitch %u)", (unsigned)ret, q.kva, q.pitch);
    if (w != q.width || h != q.height) RD3IOP_FAIL(kIOReturnBadArgument, "source %ux%u is not the scanout's %ux%u", w, h, q.width, q.height);

    rd3AccelIopHome(fb, head);
    const unsigned words = (((pitch < q.pitch) ? pitch : q.pitch) & ~3u) / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    volatile unsigned int *d = (volatile unsigned int *)q.kva;
    for (unsigned y = 0; y < h; y++) {
        const volatile unsigned int *sp = src + (unsigned long)y * spr;
        volatile unsigned int *dp = d + (unsigned long)y * dpr;
        for (unsigned x = 0; x < words; x++) dp[x] = sp[x];
    }
    OSAddAtomic64(1, fromRes ? &gCntIopSrcRes : &gCntIopSrcSurf);
    const SInt64 n = OSAddAtomic64(1, &gCntIopOk);
    const SInt64 nh = OSAddAtomic64(1, &gCntIopOkHead[head]);
    unsigned nz = 0, varied = 0; const unsigned first = src[0] & 0xffffffu;
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
        unsigned sx = (w * (2 * j + 1)) / 16; if (sx >= spr) sx = spr - 1;
        const unsigned v = src[(unsigned long)((h * (2 * i + 1)) / 16) * spr + sx] & 0xffffffu;
        if (v) nz++;
        if (v != first) varied++;
    }
    if (!varied) OSAddAtomic64(1, &gCntIopBlank);
    if ((n <= 3 || n == 600 || nh == 0) && gIopK6Logs < 12) {
        OSIncrementAtomic(&gIopK6Logs);
        ALOG("iop: K6 transaction #%lld (head %u #%lld) on the panel: %ux%u pitch %u from %s — 64 samples: nonzero %u varied %u", (long long)n, head, (long long)nh, w, h, pitch,
             fromRes ? "our VRAM resource" : "the IOSurface's memory", nz, varied);
    }
    return kIOReturnSuccess;
}

static int rd3AccelSetIop(int v)
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a) return ENXIO;
    if (v) {
        OSDictionary *caps = OSDictionary::withCapacity(2);
        if (!caps) return ENOMEM;
        caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
        caps->setObject("TransactionsSupported", kOSBooleanTrue);
        a->setProperty("IOAccelDisplayPipeCapabilities", caps);
        caps->release();
        gRd3Iop = 1;
        ALOG("iop: debug.rdna3accel_iop=1 -- capabilities published, transaction hooks OPEN");
    } else {
        gRd3Iop = 0;
        a->removeProperty("IOAccelDisplayPipeCapabilities");
        ALOG("iop: debug.rdna3accel_iop=0 -- capabilities removed, transaction hooks answer as the base class");
    }
    return 0;
}
static int rdna3acceliop_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gRd3Iop;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    return rd3AccelSetIop(v);
}
/* the boot-arg presets, on a thread call queued by fbPublished */
static void rd3AccelBootGates(void *, void *)
{
    /* rdna3agdc (RDNA3AGDC.kext): the NVIDIA driver's order - the
     * framebuffers registered first, then the AppleGraphicsDeviceControl
     * object IOPresentment asks about them, then the display-pipe
     * capabilities that make WindowServer use the pipes */
    if (rd3_arg("rdna3agdc", 0) && gBootIop && !gRd3Iop) {
        ALOG("boot-arg rdna3accelfb=%d: as if `sysctl -w debug.rdna3accelfb=%d` was typed (rdna3agdc: before the pipe "
             "capabilities)", gBootAccelFb, gBootAccelFb);
        RDNA3Accelerator::triggerFromSysctl(gBootAccelFb);
        int ag = -99, i;
        for (i = 0; i < 50; i++) {
            size_t len = sizeof(ag);
            if (sysctlbyname("debug.rdna3agdc", &ag, &len, nullptr, 0) == 0 && ag == 1) break;
            IOSleep(200);
        }
        ALOG("rdna3agdc: AGDC state %d after %d ms%s", ag, i * 200,
             ag == 1 ? "" : " (not up: RDNA3AGDC.kext missing, not approved or refused - the pipes open anyway)");
        if (!gRd3Iop)
            rd3AccelSetIop(1);
        return;
    }
    if (gBootIop && !gRd3Iop)
        rd3AccelSetIop(1);
    ALOG("boot-arg rdna3accelfb=%d: as if `sysctl -w debug.rdna3accelfb=%d` was typed", gBootAccelFb, gBootAccelFb);
    RDNA3Accelerator::triggerFromSysctl(gBootAccelFb);
}
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_iop, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, rdna3acceliop_sysctl, "I",
            "K3: 1 = publish IOAccelDisplayPipeCapabilities and perform display-pipe transactions, 0 = behave as the base class (default)");

static int rdna3accelfb_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gRd3AccelArmed;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    RDNA3Accelerator::triggerFromSysctl(v);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, rdna3accelfb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, rdna3accelfb_sysctl, "I",
            "register the RDNA3 framebuffer with the accelerator display machine (1 = found_framebuffer, 2 = family sweep)");
#define RD3ACCEL_CNT(nm, var, desc) SYSCTL_QUAD(_debug, OID_AUTO, nm, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&var, desc)
RD3ACCEL_CNT(rdna3accel_vm_alloc,         gCntVmAlloc,        "RD3VidMemory::allocPhysical successes");
RD3ACCEL_CNT(rdna3accel_vm_alloc_bytes,   gCntVmAllocBytes,   "bytes of VRAM granted to RD3VidMemory");
RD3ACCEL_CNT(rdna3accel_vm_dealloc,       gCntVmDealloc,      "RD3VidMemory::deallocPhysical calls with a handle");
RD3ACCEL_CNT(rdna3accel_vm_dealloc_bytes, gCntVmDeallocBytes, "bytes of VRAM given back by RD3VidMemory");
RD3ACCEL_CNT(rdna3accel_vm_refused,       gCntVmRefused,      "allocPhysical refused by the framebuffer");
RD3ACCEL_CNT(rdna3accel_res_new,          gCntResNew,         "RD3Resource objects created");
RD3ACCEL_CNT(rdna3accel_res_free,         gCntResFree,        "RD3Resource objects freed");
RD3ACCEL_CNT(rdna3accel_map_commit,       gCntMapCommit,      "GPU page-table commits (MAP_MEMORY_DMA ok)");
RD3ACCEL_CNT(rdna3accel_map_release,      gCntMapRelease,     "GPU page-table releases (UNMAP_ONLY)");
RD3ACCEL_CNT(rdna3accel_map_free,         gCntMapFree,        "GPU VA ranges freed (rd3AccelGpuVaFree)");
RD3ACCEL_CNT(rdna3accel_iop_ok,           gCntIopOk,          "K3: display-pipe transactions copied to the panel");
RD3ACCEL_CNT(rdna3accel_iop_ok_h0,        gCntIopOkHead[0],   "frames copied to head 0's panel");
RD3ACCEL_CNT(rdna3accel_iop_ok_h1,        gCntIopOkHead[1],   "frames copied to head 1's panel");
RD3ACCEL_CNT(rdna3accel_iop_ok_h2,        gCntIopOkHead[2],   "frames copied to head 2's panel");
RD3ACCEL_CNT(rdna3accel_iop_ok_h3,        gCntIopOkHead[3],   "frames copied to head 3's panel");
SYSCTL_UINT(_debug, OID_AUTO, rdna3accel_heads_published, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gRd3HeadsPublished, 0,
            "bit i = head i's framebuffer has published to the accelerator");
SYSCTL_UINT(_debug, OID_AUTO, rdna3accel_heads_registered, CTLFLAG_RD | CTLFLAG_LOCKED, &gRd3HeadsRegistered, 0,
            "bit i = head i's framebuffer was handed to the display machine (found_framebuffer)");
SYSCTL_UINT(_debug, OID_AUTO, rdna3accel_heads_piped, CTLFLAG_RD | CTLFLAG_LOCKED, &gRd3HeadsPiped, 0,
            "bit i = head i has a display pipe that drives ITS OWN framebuffer (pipe+0x98 read back)");
static_assert(kRd3MaxFB == 4, "kRd3MaxFB changed: add debug.rdna3accel_iop_ok_h<i> for every new head");
RD3ACCEL_CNT(rdna3accel_iop_fail,         gCntIopFail,        "K3: transactions refused (see the capped iop: log lines)");
RD3ACCEL_CNT(rdna3accel_iop_empty,        gCntIopEmpty,       "K3: transactions with no plane (gamma/options only)");
RD3ACCEL_CNT(rdna3accel_iop_empty_base,   gCntIopEmptyBase,   "gamma on: plane-less transactions the base class performed");
RD3ACCEL_CNT(rdna3accel_iop_empty_base_err, gCntIopEmptyBaseErr, "gamma on: plane-less transactions the base class refused");
RD3ACCEL_CNT(rdna3accel_iop_src_res,      gCntIopSrcRes,      "K3: frames sourced from our VRAM resource");
RD3ACCEL_CNT(rdna3accel_iop_src_surf,     gCntIopSrcSurf,     "K3: frames sourced from the IOSurface memory");
RD3ACCEL_CNT(rdna3accel_iop_blank,        gCntIopBlank,       "K6: frames copied whose 64 samples were all ONE colour (a blank frame)");
static SInt64 gCntIopCopySkip;
RD3ACCEL_CNT(rdna3accel_iop_copy_skip,    gCntIopCopySkip,    "frames kept on the panel because the surface's newest pixels were not yet in system memory");
static int    gIopFlip = 0;
static SInt64 gCntIopFlip, gCntIopFlipHome, gCntIopFlipNo[8];
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_iop_flip, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gIopFlip, 0, "1 = scan the composited IOSurface out of its own VRAM (no copy)");
RD3ACCEL_CNT(rdna3accel_iop_flips,        gCntIopFlip,        "frames scanned out of the surface's own VRAM (zero copy)");
RD3ACCEL_CNT(rdna3accel_iop_flip_home,    gCntIopFlipHome,    "flips back to the boot scanout before a copied frame");
RD3ACCEL_CNT(rdna3accel_iop_flip_lock,    gCntIopFlipNo[3],   "copied because the accelerator lock was busy");
RD3ACCEL_CNT(rdna3accel_iop_flip_novram,  gCntIopFlipNo[4],   "copied because the surface has no RM VRAM");
RD3ACCEL_CNT(rdna3accel_iop_flip_stale,   gCntIopFlipNo[5],   "copied because system memory holds the newest copy");
RD3ACCEL_CNT(rdna3accel_iop_flip_refused, gCntIopFlipNo[7],   "copied because the framebuffer refused the flip");
static SInt64 gCntIopFlipHit, gCntIopFlipMiss;
RD3ACCEL_CNT(rdna3accel_iop_flip_hit,     gCntIopFlipHit,     "swaps resolved from the cache (no accelerator lock)");
RD3ACCEL_CNT(rdna3accel_iop_flip_miss,    gCntIopFlipMiss,    "swaps that resolved under the accelerator lock");
static int    gIopAsync = 1;
static SInt64 gCntIopAsync, gCntIopAsyncRefused, gCntIopAsyncDrop;
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_iop_async, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopAsync, 0, "1 = the swap runs on a thread call and completes through signalTransactionInterrupt (Apple's shape); 0 = the earlier synchronous flip");
RD3ACCEL_CNT(rdna3accel_iop_async_flips,   gCntIopAsync,        "swaps completed asynchronously");
RD3ACCEL_CNT(rdna3accel_iop_async_refused, gCntIopAsyncRefused, "async swaps the display engine refused (that frame stayed off the panel)");
RD3ACCEL_CNT(rdna3accel_iop_async_drop,    gCntIopAsyncDrop,    "frames dropped because a flip was still in flight (never expected)");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_a3display, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gA3Display, 0,
           "card run 58: 1 = WindowServer's display framebuffers may become A3 BOs (their own VRAM, which no swap scans out); "
           "0 (default, boot-arg rdna3a3display) = kept out, composited through copies");
RD3ACCEL_CNT(rdna3accel_a3display_refused,  gCntA3DispRefused,  "card run 58: display framebuffer imports kept out of A3");
RD3ACCEL_CNT(rdna3accel_a3display_imported, gCntA3DispImported, "card run 58: display framebuffers imported anyway (rdna3a3display=1)");
RD3ACCEL_CNT(rdna3accel_a3display_other,    gCntA3WsOther,      "card run 58: WindowServer imports of no head's scanout size (not display framebuffers)");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_a3ws, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gA3Ws, 0,
           "card run 58 (2026-10-15d): 1 = WindowServer's other inputs may become A3 BOs; 0 (default, boot-arg rdna3a3ws) = copies");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_a3, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gA3On, 0,
           "0 = no IOSurface of any process becomes an A3 BO (boot-arg rdna3a3=0); 1 (default)");
RD3ACCEL_CNT(rdna3accel_a3ws_refused,       gCntA3WsRefused,    "card run 58: WindowServer inputs kept out of A3 (rdna3a3ws=0)");
RD3ACCEL_CNT(rdna3accel_a3off_refused,      gCntA3OffRefused,   "imports refused because A3 is off (rdna3a3=0)");
RD3ACCEL_CNT(rdna3accel_swaps_abab_h0, gCntSwapAbab[0], "head 0's swaps that showed the picture of two swaps before (A B A B: flicker)");
RD3ACCEL_CNT(rdna3accel_swaps_abab_h1, gCntSwapAbab[1], "head 1's swaps that showed the picture of two swaps before (A B A B: flicker)");
RD3ACCEL_CNT(rdna3accel_swaps_abab_h2, gCntSwapAbab[2], "head 2's swaps that showed the picture of two swaps before (A B A B: flicker)");
RD3ACCEL_CNT(rdna3accel_swaps_abab_h3, gCntSwapAbab[3], "head 3's swaps that showed the picture of two swaps before (A B A B: flicker)");
RD3ACCEL_CNT(rdna3accel_swaps_ababwide_h0, gCntSwapAbabWide[0], "head 0's A B A B swaps where more than one band alternated");
RD3ACCEL_CNT(rdna3accel_swaps_ababwide_h1, gCntSwapAbabWide[1], "head 1's A B A B swaps where more than one band alternated");
RD3ACCEL_CNT(rdna3accel_swaps_ababwide_h2, gCntSwapAbabWide[2], "head 2's A B A B swaps where more than one band alternated");
RD3ACCEL_CNT(rdna3accel_swaps_ababwide_h3, gCntSwapAbabWide[3], "head 3's A B A B swaps where more than one band alternated");
RD3ACCEL_CNT(rdna3accel_swaps_hashed_h0, gCntSwapHashed[0], "head 0's swaps the flicker test hashed (its denominator)");
RD3ACCEL_CNT(rdna3accel_swaps_hashed_h1, gCntSwapHashed[1], "head 1's swaps the flicker test hashed (its denominator)");
RD3ACCEL_CNT(rdna3accel_swaps_hashed_h2, gCntSwapHashed[2], "head 2's swaps the flicker test hashed (its denominator)");
RD3ACCEL_CNT(rdna3accel_swaps_hashed_h3, gCntSwapHashed[3], "head 3's swaps the flicker test hashed (its denominator)");
RD3ACCEL_CNT(rdna3accel_swaps_hashskip_h0, gCntSwapHashSkip[0], "head 0's swaps the flicker test could not hash");
RD3ACCEL_CNT(rdna3accel_swaps_hashskip_h1, gCntSwapHashSkip[1], "head 1's swaps the flicker test could not hash");
RD3ACCEL_CNT(rdna3accel_swaps_hashskip_h2, gCntSwapHashSkip[2], "head 2's swaps the flicker test could not hash");
RD3ACCEL_CNT(rdna3accel_swaps_hashskip_h3, gCntSwapHashSkip[3], "head 3's swaps the flicker test could not hash");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_flicker, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gFlicker, 0,
           "card run 58: the flicker test of the legacy swaps - 0 off, 1 (default) the first 3000 hashed swaps per head, 2 always");
RD3ACCEL_CNT(rdna3accel_a3display_task_mismatch, gCntA3TaskMismatch, "card run 58: imports for another task than the caller's (display check not run)");
RD3ACCEL_CNT(rdna3accel_a3display_noname,   gCntA3NoName,       "card run 58: imports by a caller without a process name (display check not run)");
RD3ACCEL_CNT(rdna3accel_swaps_h0,    gCntSwapHead[0],   "legacy surface swaps presented on head 0");
RD3ACCEL_CNT(rdna3accel_swaps_h1,    gCntSwapHead[1],   "legacy surface swaps presented on head 1");
RD3ACCEL_CNT(rdna3accel_swaps_h2,    gCntSwapHead[2],   "legacy surface swaps presented on head 2");
RD3ACCEL_CNT(rdna3accel_swaps_h3,    gCntSwapHead[3],   "legacy surface swaps presented on head 3");
RD3ACCEL_CNT(rdna3accel_swaps_a3_h0, gCntSwapA3Head[0], "head 0's swaps that scanned out an A3 display BO");
RD3ACCEL_CNT(rdna3accel_swaps_a3_h1, gCntSwapA3Head[1], "head 1's swaps that scanned out an A3 display BO");
RD3ACCEL_CNT(rdna3accel_swaps_a3_h2, gCntSwapA3Head[2], "head 2's swaps that scanned out an A3 display BO");
RD3ACCEL_CNT(rdna3accel_swaps_a3_h3, gCntSwapA3Head[3], "head 3's swaps that scanned out an A3 display BO");
static_assert(kRd3MaxFB == 4, "kRd3MaxFB changed: add debug.rdna3accel_swaps_h<i> / swaps_a3_h<i> for every new head");
static int gCrcHead = 0;
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_crc_head, CTLFLAG_RW | CTLFLAG_LOCKED, &gCrcHead, 0, "b85k-crc: which head debug.rdna3accel_crc reads");
static int rdna3accel_crc_sysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_crc, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, rdna3accel_crc_sysctl, "I",
            "b85k-crc: display CRC32 of what head rdna3accel_crc_head is showing (0 = unavailable)");
static bool gSysctlInstalled = false;
static void rdna3accelInstallSysctl(void)
{
    if (gSysctlInstalled) return;
    sysctl_register_oid(&sysctl__debug_rdna3accelfb);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_alloc);       sysctl_register_oid(&sysctl__debug_rdna3accel_vm_alloc_bytes);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_dealloc);     sysctl_register_oid(&sysctl__debug_rdna3accel_vm_dealloc_bytes);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_refused);
    sysctl_register_oid(&sysctl__debug_rdna3accel_res_new);        sysctl_register_oid(&sysctl__debug_rdna3accel_res_free);
    sysctl_register_oid(&sysctl__debug_rdna3accel_map_commit);     sysctl_register_oid(&sysctl__debug_rdna3accel_map_release);
    sysctl_register_oid(&sysctl__debug_rdna3accel_map_free);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_ok);         sysctl_register_oid(&sysctl__debug_rdna3accel_iop_fail);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_empty);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_empty_base); sysctl_register_oid(&sysctl__debug_rdna3accel_iop_empty_base_err);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_src_res);    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_src_surf);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_blank);   sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_skip);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_flip);       sysctl_register_oid(&sysctl__debug_rdna3accel_iop_flips);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_flip_home);  sysctl_register_oid(&sysctl__debug_rdna3accel_iop_flip_lock);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_flip_novram); sysctl_register_oid(&sysctl__debug_rdna3accel_iop_flip_stale);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_flip_refused);
    sysctl_register_oid(&sysctl__debug_rdna3accel_crc_head);       sysctl_register_oid(&sysctl__debug_rdna3accel_crc);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_flip_hit);   sysctl_register_oid(&sysctl__debug_rdna3accel_iop_flip_miss);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_async);      sysctl_register_oid(&sysctl__debug_rdna3accel_iop_async_flips);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_async_refused); sysctl_register_oid(&sysctl__debug_rdna3accel_iop_async_drop);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_ok_h0);      sysctl_register_oid(&sysctl__debug_rdna3accel_iop_ok_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_ok_h2);      sysctl_register_oid(&sysctl__debug_rdna3accel_iop_ok_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_heads_published); sysctl_register_oid(&sysctl__debug_rdna3accel_heads_registered);
    sysctl_register_oid(&sysctl__debug_rdna3accel_heads_piped);
    sysctl_register_oid(&sysctl__debug_rdna3accel_a3display);
    sysctl_register_oid(&sysctl__debug_rdna3accel_a3display_refused); sysctl_register_oid(&sysctl__debug_rdna3accel_a3display_imported);
    sysctl_register_oid(&sysctl__debug_rdna3accel_a3display_other);   sysctl_register_oid(&sysctl__debug_rdna3accel_a3display_task_mismatch);
    sysctl_register_oid(&sysctl__debug_rdna3accel_a3display_noname);
    sysctl_register_oid(&sysctl__debug_rdna3accel_a3ws);            sysctl_register_oid(&sysctl__debug_rdna3accel_a3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_a3ws_refused);    sysctl_register_oid(&sysctl__debug_rdna3accel_a3off_refused);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_abab_h0);   sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_abab_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_abab_h2);   sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_abab_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_ababwide_h0); sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_ababwide_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_ababwide_h2); sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_ababwide_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_hashed_h0); sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_hashed_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_hashed_h2); sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_hashed_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_hashskip_h0); sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_hashskip_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_hashskip_h2); sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_hashskip_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_flicker);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_h0);       sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_h2);       sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_a3_h0);    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_a3_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_a3_h2);    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_a3_h3);
    gSysctlInstalled = true;
    ALOG("sysctl debug.rdna3accelfb installed -- `sudo sysctl -w debug.rdna3accelfb=1` registers the framebuffer");
}

static inline void rd3AccelZero(void *p, unsigned long n)
{ volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static struct RD3KRmHandles gRmHandles;
static bool                 gRmHandlesValid;

static unsigned rd3RmHandlesLogged;

static bool rd3AccelRmHandles(struct RD3KRmHandles *out)
{
    if (gRmHandlesValid) { *out = gRmHandles; return true; }
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a) {
        if (rd3RmHandlesLogged < 8) { rd3RmHandlesLogged++;
            ALOG("rd3AccelRmHandles: no RDNA3Accelerator instance yet -- too early"); }
        return false;
    }
    IOService *core = a->core();
    if (!core) {
        if (rd3RmHandlesLogged < 8) { rd3RmHandlesLogged++;
            ALOG("rd3AccelRmHandles: the core kext (RDNA3Accel) has not published -- will retry"); }
        return false;
    }
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_GPUVA_FN_HANDLES);
    if (!sym) {
        if (rd3RmHandlesLogged < 8) { rd3RmHandlesLogged++;
            ALOG("rd3AccelRmHandles: OSSymbol allocation failed"); }
        return false;
    }
    struct RD3KRmHandles h;
    rd3AccelZero(&h, sizeof h);
    h.version = RD3K_GPUVA_ABI_VERSION;
    IOReturn r = core->callPlatformFunction(sym, false, &h, nullptr, nullptr, nullptr);
    sym->release();
    if (r != kIOReturnSuccess || !h.kapiDevice) {
        if (rd3RmHandlesLogged < 8) { rd3RmHandlesLogged++;
            ALOG("rd3AccelRmHandles: core kext gave no device (ret 0x%x, kapiDevice %p)",
                 (unsigned)r, h.kapiDevice); }
        return false;
    }
    gRmHandles = h; gRmHandlesValid = true;
    ALOG("RM handles: kapiDevice %p (hClient/hDevice resolved inside RDNA3Accel by name) "
         "*** the accelerator can now build GPU page tables ***", h.kapiDevice);
    *out = h;
    return true;
}

static IOService *rd3AccelCore()
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a) return nullptr;
    return a->core();
}

static IOReturn rd3AccelGpuVaCall(const char *fn, struct RD3KGpuVaRequest *g)
{
    IOService *core = rd3AccelCore();
    if (!core) return kIOReturnNotAttached;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(fn);
    if (!sym) return kIOReturnNoMemory;
    IOReturn r = core->callPlatformFunction(sym, false, g, nullptr, nullptr, nullptr);
    sym->release();
    return r;
}

extern "C" {
void *_ZN13IOSurfaceRoot13lookupSurfaceEjP4task(void *root, unsigned int id, task_t task);
void *_ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej(void *accel, void *surface, unsigned int plane);
void  _ZN16IOAccelVidMemory4wireEv(void *vidmem);
void  _ZN20IOSurfaceDeviceCache10setCurrentEv(void *cache);
void *_ZNK9IOSurface19getMemoryDescriptorEv(void *surface);
unsigned int _ZNK9IOSurface12getSurfaceIDEv(void *surface);
void  _ZN22IOGraphicsAccelerator29lock_busyEv(void *accel);
void  _ZN22IOGraphicsAccelerator211unlock_busyEv(void *accel);
}
static bool rd3FamLock(RDNA3Accelerator *a, bool tryOnly)
{
    unsigned char *b = (unsigned char *)a;
    IOLock *lk = *(IOLock *volatile *)(b + 0x88);
    if (!lk) return false;
    if (tryOnly) { if (!IOLockTryLock(lk)) return false; }
    else {
        OSIncrementAtomic((volatile SInt32 *)(b + 0x90));
        IOLockLock(lk);
        OSDecrementAtomic((volatile SInt32 *)(b + 0x90));
    }
    _ZN22IOGraphicsAccelerator29lock_busyEv(a);
    a->acceleratorDidLock("", 0);
    return true;
}
static void rd3FamUnlock(RDNA3Accelerator *a)
{
    a->acceleratorWillUnlock("", 0);
    _ZN22IOGraphicsAccelerator211unlock_busyEv(a);
    IOLockUnlock(*(IOLock *volatile *)((unsigned char *)a + 0x88));
}
static unsigned gNVSVLockMiss;
enum { kNVSVPend = 512 };
static unsigned gNVSVPendId[kNVSVPend];
static RD3Resource *gNVSVPendResource[kNVSVPend];
static IOSimpleLock *volatile gNVSVPendLock;
/* op 1 marks surface 'id' pending (owned by 'resource'), 0 clears, 2 asks */
static bool rd3SVPend(unsigned id, int op, RD3Resource *resource = nullptr)
{
    if (!id) return op == 1 ? false : false;
    if (!gNVSVPendLock) return op == 1 ? false : false;
    bool r = false; int freeSlot = -1;
    IOSimpleLockLock(gNVSVPendLock);
    for (int i = 0; i < kNVSVPend; i++) {
        if (gNVSVPendId[i] == id) {
            r = true;
            if (op == 0) { gNVSVPendId[i] = 0; gNVSVPendResource[i] = nullptr; }
            else if (op == 1) gNVSVPendResource[i] = resource;
            break;
        }
        if (!gNVSVPendId[i] && freeSlot < 0) freeSlot = i;
    }
    if (op == 1 && !r) { if (freeSlot >= 0) { gNVSVPendId[freeSlot] = id; gNVSVPendResource[freeSlot] = resource; r = true; } }
    else if (op == 1) r = true;
    IOSimpleLockUnlock(gNVSVPendLock);
    return r;
}
/* NullMoth 79e1c2f: a pending mark belongs to the resource that last
 * dirtied the surface; a freed resource drops only its own marks (no device
 * cache read during destruction, a newer owner's mark stays), so the 512
 * slots do not fill with surfaces that are gone */
static void rd3SVForgetResource(RD3Resource *resource)
{
    if (!resource || !gNVSVPendLock) return;
    IOSimpleLockLock(gNVSVPendLock);
    for (unsigned i = 0; i < kNVSVPend; i++) if (gNVSVPendResource[i] == resource) {
        gNVSVPendId[i] = 0;
        gNVSVPendResource[i] = nullptr;
    }
    IOSimpleLockUnlock(gNVSVPendLock);
}
static void    *gNVSVRoot;
static unsigned gNVSVOk, gNVSVRefused;
static inline void rd3SVZero(void *p, unsigned long n) { volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static unsigned gNVSVDirty, gNVSVDirtyRefused, gNVSVPageoff, gNVSVPageoffFail;
static unsigned rd3AccelSurfDirty(void *cache, RD3Resource *res, unsigned plane)
{
    volatile unsigned char *r = (volatile unsigned char *)res;
    unsigned char type = r[0x14];
    unsigned int fl = *(volatile unsigned int *)(r + 0xc);
    volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
    if (type != 0xc0 || (fl & 0x8000) || !valid || plane > 15) return RD3K_SS_NO_CACHE;
    if (!rd3SVPend(_ZNK9IOSurface12getSurfaceIDEv(*(void *volatile *)((unsigned char *)cache + 0x10)), 1, res)) return RD3K_SS_NO_CACHE;
    _ZN20IOSurfaceDeviceCache10setCurrentEv(cache);
    valid[6] |= (unsigned short)(1u << plane);
    valid[0] &= (unsigned short)~(1u << plane);
    return 0;
}
static IOReturn rd3AccelSurfVram(struct RD3KSurfShareEsc *p, task_t task, unsigned cmd)
{
    p->hSrcClient = p->hSrcMemory = 0;
    if (p->version != RD3K_SURFSHARE_VERSION) { p->status = RD3K_SS_BAD_VERSION; return kIOReturnSuccess; }
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    struct RD3KRmHandles h; rd3SVZero(&h, sizeof h);
    if (!task || !a || !rd3AccelRmHandles(&h)) { p->status = RD3K_SS_OFF; return kIOReturnSuccess; }
    if (!gNVSVRoot) {
        OSDictionary *m = IOService::serviceMatching("IOSurfaceRoot");
        if (m) { gNVSVRoot = IOService::waitForMatchingService(m, 5ULL * 1000 * 1000 * 1000); m->release(); }
        if (!gNVSVRoot) { p->status = RD3K_SS_OFF; return kIOReturnSuccess; }
    }
    void *surf = _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(gNVSVRoot, p->surfaceID, task);
    if (!surf) { p->status = RD3K_SS_NO_SURFACE; gNVSVRefused++; return kIOReturnSuccess; }
    /* card run 58: the RM share is the same VRAM as a GEM import - the same rule, before the wire */
    if (cmd != RD3K_ESC_SURF_DIRTY) {
        unsigned dh = 0, reason = kA3WhyNone;
        const unsigned st = rd3AccelA3Policy(surf, task, p->surfaceID, &dh, &reason);
        if (st) {
            const SInt64 n = rd3AccelA3WhyCount(reason);
            char why[256];
            rd3AccelA3Why(why, sizeof why, reason, dh, surf);
            ((OSObject *)surf)->release();
            p->status = st; gNVSVRefused++;
            if (rd3AccelA3RefusedFirst(p->surfaceID, n))
                ALOG("surfvram: surface %u plane %u refused: %s (%lld refused)", p->surfaceID, p->plane, why, (long long)n);
            return kIOReturnSuccess;
        }
    }
    if (!rd3FamLock(a, false)) { ((OSObject *)surf)->release(); p->status = RD3K_SS_OFF; return kIOReturnSuccess; }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, p->plane);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    RD3Resource *res = OSDynamicCast(RD3Resource, ctx);
    RD3VidMemory *vm = res ? rd3res_vidmem(res) : nullptr;
    if (cmd == RD3K_ESC_SURF_DIRTY) {
        p->status = (cache && res && vm && vm->fRmHandle) ? rd3AccelSurfDirty(cache, res, p->plane) : RD3K_SS_NO_CACHE;
        if (p->status == 0) gNVSVDirty++; else gNVSVDirtyRefused++;
        if (p->status == 0 && gCntA3DispImported) rd3AccelA3DispDirty(p->surfaceID, vm->fPhys);
        if (gNVSVDirty + gNVSVDirtyRefused <= 6 || ((gNVSVDirty + gNVSVDirtyRefused) & 4095) == 0)
            ALOG("surfvram dirty: surface %u plane %u type 0x%x -> %#x (dirty %u, refused %u)", p->surfaceID, p->plane,
                 res ? (unsigned)((volatile unsigned char *)res)[0x14] : 0u, p->status, gNVSVDirty, gNVSVDirtyRefused);
        if (cache) ((OSObject *)cache)->release();
        rd3FamUnlock(a);
        ((OSObject *)surf)->release();
        return kIOReturnSuccess;
    }
    if (vm && !vm->fRmHandle && !vm->fScanout) _ZN16IOAccelVidMemory4wireEv(vm);
    if (!cache || !res) p->status = RD3K_SS_NO_CACHE;
    else if (!vm || !vm->fRmHandle || !vm->fBytes) p->status = RD3K_SS_NO_VRAM;
    else if (p->size > vm->fBytes) p->status = RD3K_SS_TOO_SMALL;
    else {
        struct RD3KSurfShareRm g; rd3SVZero(&g, sizeof g);
        g.version = RD3K_SS_RM_VERSION; g.op = RD3K_SS_RM_SHARE_KAPI;
        g.kapiDevice = h.kapiDevice; g.kapiMemory = vm->fRmHandle; g.target = p->hClient; g.status = RD3K_SS_OFF;
        IOService *core = rd3AccelCore();
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_SS_FN_RM);
        if (core && sym && core->callPlatformFunction(sym, false, &g, nullptr, nullptr, nullptr) == kIOReturnSuccess) {}
        if (sym) sym->release();
        p->status = g.status; p->hSrcClient = g.hClient; p->hSrcMemory = g.hObject; p->size = vm->fBytes;
    }
    if (p->status == 0) gNVSVOk++; else gNVSVRefused++;
    if (gNVSVOk + gNVSVRefused <= 8 || ((gNVSVOk + gNVSVRefused) & 1023) == 0)
        ALOG("surfvram: surface %u plane %u -> cache %p res %p vidmem %p (%llu B) -> client %#x mem %#x status %#x (ok %u, refused %u)",
             p->surfaceID, p->plane, cache, res, vm, vm ? vm->fBytes : 0ull, p->hSrcClient, p->hSrcMemory, p->status, gNVSVOk, gNVSVRefused);
    if (cache) ((OSObject *)cache)->release();
    rd3FamUnlock(a);
    ((OSObject *)surf)->release();
    return kIOReturnSuccess;
}

/* RD3K_ESC_SURF_GEM (RD_NR_XNU_SURFACE_BO): the VRAM allocation behind an
 * IOSurface plane for a GEM BO the core kext builds over it, so RADV draws
 * where WindowServer and the display read (A3). The same lookup as the RM
 * share above, in the client's task; the reference is taken while the
 * family lock still holds the surface's vidmem. */
static volatile SInt32 gNVSVGemOk, gNVSVGemRefused, gNVSVGemCause[16];   /* by status & 15: 0x80000001..8 apart */
/* every answer but the first imports is counted; each refusal cause logs
 * its first few, so the card report says why a surface stayed private */
static void rd3AccelSurfGemLog(const struct RD3KSurfGem *p, const char *why, const void *vm, unsigned long long bytes)
{
    if (p->status == 0) {
        SInt32 n = OSIncrementAtomic(&gNVSVGemOk);
        if (n < 8 || (n & 4095) == 0)
            ALOG("surfgem: surface %u plane %u -> vidmem %p (%llu B) (ok %d, refused %d)", p->surfaceID, p->plane, vm,
                 bytes, (int)n + 1, (int)gNVSVGemRefused);
        return;
    }
    OSIncrementAtomic(&gNVSVGemRefused);
    SInt32 c = OSIncrementAtomic(&gNVSVGemCause[p->status & 15u]);
    if (c < 6 || (c & 1023) == 0)
        ALOG("surfgem: surface %u plane %u refused: %s (status %#x, %d of this cause; ok %d, refused %d)", p->surfaceID,
             p->plane, why, p->status, (int)c + 1, (int)gNVSVGemOk, (int)gNVSVGemRefused);
}
/* IOSurfaceRoot, found once (two first callers race: the loser drops its reference) */
static void *rd3AccelSurfaceRoot(void)
{
    if (gNVSVRoot) return gNVSVRoot;
    OSDictionary *m = IOService::serviceMatching("IOSurfaceRoot");
    if (!m) return nullptr;
    IOService *r = IOService::waitForMatchingService(m, 5ULL * 1000 * 1000 * 1000);
    m->release();
    if (!r) {
        static volatile SInt32 said;
        if (OSIncrementAtomic(&said) < 3) ALOG("surfgem: no IOSurfaceRoot after 5 s");
        return nullptr;
    }
    if (!OSCompareAndSwapPtr(nullptr, r, &gNVSVRoot)) r->release();
    return gNVSVRoot;
}
/* card run 58: the heads whose display framebuffer 'surf' may be (a mask,
 * 0 = none or not decided). The process name is read on the caller's own
 * thread (the ioctl's); the size is compared with what each framebuffer
 * scans out now (askFramebufferForScanout, no lock held here) and with the
 * size of the head's own swaps. Every way the check can miss is counted and
 * logged: a refusal that never fires must not be silent. */
/* counts one miss; true for the first 8, then every 1024th (log those) */
static bool rd3AccelA3Miss(SInt64 *cnt, long long *nOut)
{
    const SInt64 n = OSAddAtomic64(1, cnt);
    *nOut = (long long)n + 1;
    return n < 8 || (n & 1023) == 0;
}
static unsigned rd3AccelA3Policy(void *surf, task_t task, unsigned surfaceID, unsigned *headsOut, unsigned *whyOut)
{
    *headsOut = 0;
    *whyOut = kA3WhyNone;
    if (!surf) return 0;
    IOSurface *s = (IOSurface *)surf;
    const unsigned W = (unsigned)s->getWidth(), H = (unsigned)s->getHeight();
    if (!gA3On) {   /* rdna3a3=0: no IOSurface of any process becomes a BO */
        OSAddAtomic64(1, &gCntA3OffRefused);
        *whyOut = kA3WhyOff;
        return RD3K_SS_POLICY;
    }
    /* a process the kernel cannot name might be WindowServer: refused
     * (copies) unless every WindowServer rule is open anyway */
    const bool openAnyway = gA3Ws && (gA3Display || gIopFlip);
    if (task != current_task()) {
        long long n;
        if (rd3AccelA3Miss(&gCntA3TaskMismatch, &n))
            ALOG("surfgem: surface %u %ux%u imported for another task than the caller's: its process is unknown -- %s "
                 "(%lld so far)", surfaceID, W, H, openAnyway ? "a BO (rdna3a3ws=1 and rdna3a3display=1)"
                                                                 : "kept out of A3, copies", n);
        if (openAnyway) return 0;
        *whyOut = kA3WhyTaskMismatch;
        return RD3K_SS_POLICY;
    }
    char proc[32];
    proc[0] = 0;
    proc_selfname(proc, (int)sizeof proc);
    if (!proc[0]) {
        long long n;
        if (rd3AccelA3Miss(&gCntA3NoName, &n))
            ALOG("surfgem: surface %u %ux%u: the caller has no process name -- %s (%lld so far)", surfaceID, W, H,
                 openAnyway ? "a BO (rdna3a3ws=1 and rdna3a3display=1)" : "kept out of A3, copies", n);
        if (openAnyway) return 0;
        *whyOut = kA3WhyNoName;
        return RD3K_SS_POLICY;
    }
    if (!rd3_a3_display_proc(proc)) return 0;   /* an application's own or shared surface: A3 */
    /* the display-pipe flip (debug.rdna3accel_iop_flip=1) scans the IOSurface's own VRAM */
    const int displayOn = gA3Display || gIopFlip;
    uint32_t lw[kRd3MaxFB], lh[kRd3MaxFB];
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    for (unsigned i = 0; i < kRd3MaxFB; i++) {
        lw[i] = lh[i] = 0;
        IOService *fb = a ? a->fFBs[i] : nullptr;
        if (!fb) continue;
        struct RD3KVramRequest q;
        rd3SVZero(&q, sizeof q);
        q.version = RD3K_VRAM_ABI_VERSION;
        if (a->askFramebufferForScanout(&q, fb) == kIOReturnSuccess) { lw[i] = q.width; lh[i] = q.height; }
    }
    const unsigned mask = rd3_a3_size_heads(W, H, lw, lh, kRd3MaxFB) |
                          rd3_a3_size_heads(W, H, gHeadSwapW, gHeadSwapH, kRd3MaxFB);
    *headsOut = mask;
    long long n;
    if (!mask && rd3AccelA3Miss(&gCntA3WsOther, &n) && gA3Ws) {   /* counted always, logged when it goes in */
        char heads[112];
        heads[0] = 0;
        for (unsigned i = 0; i < kRd3MaxFB; i++) {
            char one[28];
            snprintf(one, sizeof one, "%s%ux%u/%ux%u", i ? " " : "", lw[i], lh[i],
                     (unsigned)gHeadSwapW[i], (unsigned)gHeadSwapH[i]);
            strlcat(heads, one, sizeof heads);
        }
        ALOG("surfgem: WindowServer surface %u %ux%u is not a display framebuffer (heads, scanout/swap size: %s) "
             "(%lld so far): a BO (rdna3a3ws=1)", surfaceID, W, H, heads, n);
    }
    switch (rd3_a3_ws_policy(1, mask, displayOn, gA3Ws)) {
    case RD3_A3_REFUSE_DISPLAY: OSAddAtomic64(1, &gCntA3DispRefused); *whyOut = kA3WhyDisplay; return RD3K_SS_DISPLAY;
    case RD3_A3_REFUSE_WS:      OSAddAtomic64(1, &gCntA3WsRefused);   *whyOut = kA3WhyWs;      return RD3K_SS_POLICY;
    default:                    return 0;
    }
}
/* a display surface imported anyway (rdna3a3display=1): its VRAM, for the swap path's verdict */
static void rd3AccelA3DispNote(unsigned surfaceID, unsigned heads, unsigned long long phys, const void *vm)
{
    if (!gA3DispLock || !heads) return;
    IOSimpleLockLock(gA3DispLock);
    unsigned k = kA3DispSlots;
    for (unsigned i = 0; i < kA3DispSlots && k == kA3DispSlots; i++)
        if (gA3Disp[i].surfaceID == surfaceID) k = i;
    for (unsigned i = 0; i < kA3DispSlots && k == kA3DispSlots; i++)
        if (!gA3Disp[i].surfaceID) k = i;
    if (k == kA3DispSlots) k = gA3DispNext++ % kA3DispSlots;   /* full: an old entry goes */
    gA3Disp[k].surfaceID = surfaceID; gA3Disp[k].heads = heads; gA3Disp[k].phys = phys; gA3Disp[k].vm = vm;
    IOSimpleLockUnlock(gA3DispLock);
}
/* its vidmem is freed: the entry goes (the address may come back for another surface) */
static void rd3AccelA3DispGone(const void *vm)
{
    if (!gA3DispLock || !vm || !gCntA3DispImported) return;
    IOSimpleLockLock(gA3DispLock);
    for (unsigned i = 0; i < kA3DispSlots; i++)
        if (gA3Disp[i].vm == vm) { gA3Disp[i].surfaceID = 0; gA3Disp[i].heads = 0; gA3Disp[i].phys = 0; gA3Disp[i].vm = nullptr; }
    IOSimpleLockUnlock(gA3DispLock);
}
/* the dirty mark of an imported display surface: what WindowServer drew last, for each head it may be */
static void rd3AccelA3DispDirty(unsigned surfaceID, unsigned long long phys)
{
    if (!gA3DispLock || !surfaceID) return;
    IOSimpleLockLock(gA3DispLock);
    for (unsigned i = 0; i < kA3DispSlots; i++)
        if (gA3Disp[i].surfaceID == surfaceID) {
            for (unsigned h = 0; h < kRd3MaxFB; h++)
                if (gA3Disp[i].heads & (1u << h)) { gA3DirtySurf[h] = surfaceID; gA3DirtyPhys[h] = phys; }
            break;
        }
    IOSimpleLockUnlock(gA3DispLock);
}
/* a display refusal is logged once per surface (the last 32 kept): the
 * plugin retries one surface many times, and triage needs every surface's
 * own line to tell the policy's EPERM from any other. True: log this one. */
static bool rd3AccelA3RefusedFirst(unsigned surfaceID, SInt64 n)
{
    static unsigned seen[32], next;
    bool fresh = true;
    if (gA3DispLock) {
        IOSimpleLockLock(gA3DispLock);
        for (unsigned i = 0; i < 32 && fresh; i++) if (seen[i] == surfaceID) fresh = false;
        if (fresh) seen[next++ % 32] = surfaceID;
        IOSimpleLockUnlock(gA3DispLock);
    }
    return fresh || (n & 1023) == 0;
}
/* the refusals of this cause so far (each was counted by rd3AccelA3Policy,
 * so never 0 for a refusal): the log throttle's count */
static SInt64 rd3AccelA3WhyCount(unsigned why)
{
    switch (why) {
    case kA3WhyOff:          return gCntA3OffRefused;
    case kA3WhyTaskMismatch: return gCntA3TaskMismatch;
    case kA3WhyNoName:       return gCntA3NoName;
    case kA3WhyDisplay:      return gCntA3DispRefused;
    case kA3WhyWs:           return gCntA3WsRefused;
    default:                 return 0;
    }
}
/* the refusal and its reason (kA3Why*) for the surfgem / surfvram log */
static void rd3AccelA3Why(char *why, unsigned long n, unsigned reason, unsigned heads, void *surf)
{
    const unsigned w = (unsigned)((IOSurface *)surf)->getWidth(), h = (unsigned)((IOSurface *)surf)->getHeight();
    if (reason == kA3WhyDisplay)
        snprintf(why, n, "WindowServer's display framebuffer of head mask 0x%x (%ux%u): the legacy swaps scan out the "
                 "legacy surface's own VRAM, never a BO over this one (card run 58) -- copies instead; "
                 "rdna3a3display=1 lets it in", heads, w, h);
    else if (reason == kA3WhyOff)
        snprintf(why, n, "A3 is off (rdna3a3=0): no IOSurface becomes a BO (%ux%u) -- copies instead", w, h);
    else if (reason == kA3WhyTaskMismatch)
        snprintf(why, n, "imported for another task than the caller's (%ux%u): its process is unknown and might be "
                 "WindowServer -- copies instead", w, h);
    else if (reason == kA3WhyNoName)
        snprintf(why, n, "a process the kernel could not name (%ux%u), which might be WindowServer -- copies instead",
                 w, h);
    else if (reason == kA3WhyWs)
        snprintf(why, n, "a WindowServer input (%ux%u): its writers (GPU, CPU, the plugin's copies) are not ordered "
                 "with the VRAM WindowServer would sample (card run 58: flicker on both screens) -- copies instead; "
                 "rdna3a3ws=1 lets it in", w, h);
    else
        snprintf(why, n, "an unknown reason %u (%ux%u) -- copies instead", reason, w, h);
}
static IOReturn rd3AccelSurfGem(struct RD3KSurfGem *p, task_t task)
{
    p->kapiMemory = nullptr; p->bytes = 0;
    if (p->version != RD3K_SURFGEM_VERSION) {
        p->status = RD3K_SS_BAD_VERSION; rd3AccelSurfGemLog(p, "the core kext speaks another version", nullptr, 0);
        return kIOReturnSuccess;
    }
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    IOService *core = rd3AccelCore();
    if (!task || !a || !core) {
        p->status = RD3K_SS_OFF; rd3AccelSurfGemLog(p, !task ? "no task" : !a ? "accelerator not started" : "no core kext", nullptr, 0);
        return kIOReturnSuccess;
    }
    void *root = rd3AccelSurfaceRoot();
    if (!root) { p->status = RD3K_SS_OFF; rd3AccelSurfGemLog(p, "no IOSurfaceRoot", nullptr, 0); return kIOReturnSuccess; }
    void *surf = _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(root, p->surfaceID, task);
    if (!surf) { p->status = RD3K_SS_NO_SURFACE; rd3AccelSurfGemLog(p, "no such surface for this task", nullptr, 0); return kIOReturnSuccess; }
    /* card run 58: before the wire below, which would give it a second VRAM copy */
    unsigned dispHeads = 0, reason = kA3WhyNone;
    const unsigned pst = rd3AccelA3Policy(surf, task, p->surfaceID, &dispHeads, &reason);
    if (pst) {
        const SInt64 n = rd3AccelA3WhyCount(reason);
        p->status = pst;
        OSIncrementAtomic(&gNVSVGemRefused);
        OSIncrementAtomic(&gNVSVGemCause[pst & 15u]);
        if (rd3AccelA3RefusedFirst(p->surfaceID, n)) {
            char why[256];
            rd3AccelA3Why(why, sizeof why, reason, dispHeads, surf);
            ALOG("surfgem: surface %u plane %u refused: %s (status %#x, %lld of this cause; ok %d, refused %d)",
                 p->surfaceID, p->plane, why, p->status, (long long)n, (int)gNVSVGemOk, (int)gNVSVGemRefused);
        }
        ((OSObject *)surf)->release();
        return kIOReturnSuccess;
    }
    if (!rd3FamLock(a, false)) {
        ((OSObject *)surf)->release();
        p->status = RD3K_SS_OFF; rd3AccelSurfGemLog(p, "the family lock is gone (accelerator stopping)", nullptr, 0);
        return kIOReturnSuccess;
    }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, p->plane);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    RD3Resource *res = OSDynamicCast(RD3Resource, ctx);
    RD3VidMemory *vm = res ? rd3res_vidmem(res) : nullptr;
    const char *why = nullptr;
    IOReturn rr = kIOReturnSuccess;
    if (vm && !vm->fRmHandle && !vm->fScanout) _ZN16IOAccelVidMemory4wireEv(vm);
    if (!cache || !res) { p->status = RD3K_SS_NO_CACHE; why = !cache ? "no device cache (never used on this GPU)" : "no resource in its cache"; }
    else if (!vm) { p->status = RD3K_SS_NO_VRAM; why = "the resource has no vidmem"; }
    else if (vm->fScanout) { p->status = RD3K_SS_NO_VRAM; why = "a scanout buffer"; }
    else if (!vm->fRmHandle || !vm->fBytes) { p->status = RD3K_SS_NO_VRAM; why = "its vidmem did not wire"; }
    else {
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_REF);
        rr = sym ? core->callPlatformFunction(sym, false, vm->fRmHandle, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
        if (sym) sym->release();
        if (rr == kIOReturnSuccess) { p->status = 0; p->kapiMemory = vm->fRmHandle; p->bytes = vm->fBytes; }
        else { p->status = RD3K_SS_NO_VRAM; why = "the core kext refused the reference (allocation unknown or freed)"; }
    }
    rd3AccelSurfGemLog(p, why ? why : "?", vm, vm ? vm->fBytes : 0ull);
    if (rr != kIOReturnSuccess) ALOG("surfgem: rdna3RefVram -> %#x", rr);
    /* a display framebuffer let in (rdna3a3display=1, or the display-pipe flip): its VRAM for the swap verdict */
    const bool a3Note = p->status == 0 && dispHeads && vm;
    const unsigned long long a3Phys = a3Note ? vm->fPhys : 0;
    const void *a3Vm = vm;
    if (cache) ((OSObject *)cache)->release();
    rd3FamUnlock(a);
    if (a3Note) {
        const SInt64 n = OSAddAtomic64(1, &gCntA3DispImported);
        rd3AccelA3DispNote(p->surfaceID, dispHeads, a3Phys, a3Vm);
        if (n < 8 || (n & 1023) == 0)
            ALOG("surfgem: display surface %u (head mask 0x%x) -> a BO over its own VRAM at phys 0x%llx "
                 "(rdna3a3display=1; the legacy swap path: the a3 display: lines say whether a swap ever scans "
                 "this out)", p->surfaceID, dispHeads, a3Phys);
    }
    ((OSObject *)surf)->release();
    return kIOReturnSuccess;
}

#include <mach/mach_time.h>
static IOLock *volatile gNVSVStageLock;
static IOBufferMemoryDescriptor *gNVSVStageBuf;
static unsigned gNVSVStaged, gNVSVStageNoBuf, gNVSVStageBackoff;
static IOLock *rd3SVStageLock(void) { return rd3LockOnce(&gNVSVStageLock); }
static IOBufferMemoryDescriptor *rd3SVStage(unsigned long long len)
{
    if (gNVSVStageBuf && gNVSVStageBuf->getLength() >= len) return gNVSVStageBuf;
    if (len > (64ull << 20) || gNVSVStageBackoff) { if (gNVSVStageBackoff) gNVSVStageBackoff--; return nullptr; }
    const unsigned long long want = (len + 0xFFFFFull) & ~0xFFFFFull;
    IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                                      (vm_size_t)want, PAGE_SIZE);
    if (b && b->prepare() != kIOReturnSuccess) { b->release(); b = nullptr; }
    if (!b) {
        gNVSVStageNoBuf++; gNVSVStageBackoff = 1024;
        ALOG("surfvram pageoff: no %llu B physically contiguous staging buffer (miss %u) -- per-page copies for the next 1024", want, gNVSVStageNoBuf);
        return nullptr;
    }
    if (gNVSVStageBuf) { gNVSVStageBuf->complete(); gNVSVStageBuf->release(); }
    gNVSVStageBuf = b;
    ALOG("surfvram pageoff: staging buffer %llu B, physically contiguous", want);
    return b;
}

/* RD3K_SS_FN_PAGEOFF: the core's copy engine moves the VRAM of 'vm' into
 * 'pages' (4 KiB each); the status, the copy's time in *usec */
static unsigned rd3SVPageoffCall(const struct RD3KRmHandles *h, RD3VidMemory *vm, unsigned long long *pages,
                                 unsigned long long n, unsigned long long len, unsigned long long *usec)
{
    struct RD3KSurfPageoff q; rd3SVZero(&q, sizeof q);
    q.version = RD3K_SS_RM_VERSION; q.status = RD3K_SS_OFF; q.kapiDevice = h->kapiDevice; q.kapiMemory = vm->fRmHandle;
    q.pages = pages; q.pageCount = n; q.length = len;
    IOService *core = rd3AccelCore();
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_SS_FN_PAGEOFF);
    if (core && sym) core->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
    if (sym) sym->release();
    *usec = q.usec;
    return q.status;
}

void RD3Resource::pageoff(IOAccelEvent *ev, bool a1, bool *out, unsigned long long a3)
{
    volatile unsigned char *r = (volatile unsigned char *)this;
    void *cache = *(void *volatile *)(r + 0xe0);
    RD3VidMemory *vm = rd3res_vidmem(this);
    struct RD3KRmHandles h; rd3SVZero(&h, sizeof h);
    void *surf = (r[0x14] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
    if (!surf || !vm || !vm->fRmHandle || vm->fScanout || !vm->fBytes || !rd3AccelRmHandles(&h)) {
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    IOMemoryDescriptor *md = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
    if (!md || md->prepare(kIODirectionNone) != kIOReturnSuccess) {
        gNVSVPageoffFail++;
        ALOG("surfvram pageoff: res %p surface pages could not be wired (md %p) -- family pageoff instead", this, md);
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    bool mayUnwire = true;
    unsigned long long len = md->getLength();
    if (len > vm->fBytes) len = vm->fBytes;
    unsigned long long pages = (len + 4095) >> 12;
    bool ok = false; unsigned st = RD3K_SS_OFF; unsigned long long usec = 0, cpuUs = 0;
    bool staged = false;
    if (IOLock *lk = rd3SVStageLock()) {
        IOLockLock(lk);
        IOBufferMemoryDescriptor *sb = rd3SVStage(len);
        IOByteCount sl = 0;
        addr64_t sp = sb ? sb->getPhysicalSegment(0, &sl, kIOMemoryMapperNone) : 0;
        unsigned long long *spa = (sp && sl >= len && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *spa) : nullptr;
        if (spa) {
            for (unsigned long long k = 0; k < pages; k++) spa[k] = sp + k * 4096;
            st = rd3SVPageoffCall(&h, vm, spa, pages, len, &usec);
            if (st == 0) {
                const uint64_t t0 = mach_absolute_time();
                const IOByteCount w = md->writeBytes(0, sb->getBytesNoCopy(), (IOByteCount)len);
                cpuUs = (mach_absolute_time() - t0) / 1000;
                ok = (w == (IOByteCount)len);
                if (!ok) st = RD3K_SS_TOO_SMALL;
                else gNVSVStaged++;
            } else if (st == 0x65u) {
                ALOG("surfvram pageoff: CE copy TIMED OUT into the staging buffer -- abandoning it (%llu B)", (unsigned long long)sb->getLength());
                gNVSVStageBuf = nullptr;
            }
            staged = true;
            IOFree(spa, pages * sizeof *spa);
        }
        IOLockUnlock(lk);
    }
    unsigned long long *pa = (!staged && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *pa) : nullptr;
    if (pa) {
        unsigned long long i = 0;
        for (IOByteCount off = 0; i < pages; ) {
            IOByteCount seglen = 0;
            addr64_t phys = md->getPhysicalSegment(off, &seglen, kIOMemoryMapperNone);
            if (!phys || !seglen) break;
            for (IOByteCount k = 0; k < seglen && i < pages; k += 4096, off += 4096) pa[i++] = phys + k;
        }
        if (i == pages) {
            st = rd3SVPageoffCall(&h, vm, pa, pages, len, &usec);
            ok = st == 0;
            if (st == 0x65u) mayUnwire = false;
        }
        IOFree(pa, pages * sizeof *pa);
    }
    if (mayUnwire) md->complete(kIODirectionNone);
    else ALOG("surfvram pageoff: res %p CE copy TIMED OUT -- surface pages stay wired (%llu B)", this, len);
    if (ok) {
        rd3SVPend(_ZNK9IOSurface12getSurfaceIDEv(surf), 0);
        volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
        if (valid) valid[0] |= 1;
        gNVSVPageoff++;
    } else gNVSVPageoffFail++;
    if (out) *out = ok;
    if (gNVSVPageoff + gNVSVPageoffFail <= 8 || ((gNVSVPageoff + gNVSVPageoffFail) & 1023) == 0)
        ALOG("surfvram pageoff: res %p %llu B on the copy engine -> %#x in %llu us%s + cpu %llu us (ok %u, failed %u, staged %u)",
             this, len, st, usec, staged ? " (staged)" : " (per page)", cpuUs, gNVSVPageoff, gNVSVPageoffFail, gNVSVStaged);
}

static IOSurface *gIopFlipFront[kRd3MaxFB], *gIopFlipPrev[kRd3MaxFB];
static bool gIopFlipped[kRd3MaxFB];
static unsigned gIopFlipLogs;
static bool rd3AccelIopFlipNo(int why) { OSAddAtomic64(1, &gCntIopFlipNo[why & 7]); return false; }
struct RD3IopFlipCache { IOSurface *s; RD3Resource *res; RD3VidMemory *vm; void *kmem; unsigned long long bytes; unsigned long long use; };
static RD3IopFlipCache gIopFlipCache[8];
static IOSimpleLock *volatile gIopFlipCacheLock;
static unsigned long long gIopFlipCacheTick;
static bool rd3AccelIopCacheGet(IOSurface *s, RD3Resource **res, RD3VidMemory **vm, void **kmem, unsigned long long *bytes)
{
    if (!gIopFlipCacheLock) return false;
    bool hit = false;
    IOSimpleLockLock(gIopFlipCacheLock);
    for (unsigned i = 0; i < 8; i++) if (gIopFlipCache[i].s == s) {
        RD3VidMemory *v = gIopFlipCache[i].vm;
        if (v && v->fRmHandle == gIopFlipCache[i].kmem && v->fBytes == gIopFlipCache[i].bytes) {
            *res = gIopFlipCache[i].res; *vm = v; *kmem = gIopFlipCache[i].kmem; *bytes = gIopFlipCache[i].bytes;
            gIopFlipCache[i].use = ++gIopFlipCacheTick; hit = true;
        }
        break;
    }
    IOSimpleLockUnlock(gIopFlipCacheLock);
    return hit;
}
static void rd3AccelIopCachePut(IOSurface *s, RD3Resource *res, RD3VidMemory *vm, void *kmem, unsigned long long bytes)
{
    if (!rd3SimpleLockOnce(&gIopFlipCacheLock)) return;
    IOSurface *drop = nullptr;
    ((OSObject *)s)->retain();
    IOSimpleLockLock(gIopFlipCacheLock);
    unsigned k = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (gIopFlipCache[i].s == s) { k = i; break; }
        if (!gIopFlipCache[i].s) { k = i; break; }
        if (gIopFlipCache[i].use < gIopFlipCache[k].use) k = i;
    }
    drop = gIopFlipCache[k].s;
    gIopFlipCache[k].s = s; gIopFlipCache[k].res = res; gIopFlipCache[k].vm = vm; gIopFlipCache[k].kmem = kmem;
    gIopFlipCache[k].bytes = bytes; gIopFlipCache[k].use = ++gIopFlipCacheTick;
    IOSimpleLockUnlock(gIopFlipCacheLock);
    if (drop) ((OSObject *)drop)->release();
}
/* every global lock, before the family can call into this module */
/* card run 50: a panic at IOAccelLegacySurface::clientClose+0x1cc as
 * WindowServer exited during a shutdown. clientClose takes the
 * accelerator's lock_busy through the surface's pointer to it (+0x12c8)
 * and unlocks through the same field at the end; IOAccelSurface::
 * didTerminate -> detach_surface zeroes that field without lock_busy
 * (probe runs 4-6: the writers are stop, which takes lock_busy, stopLocked
 * and detach_surface). A kmutil load of a new accelerator build terminated
 * the running one while WindowServer's surfaces were closing. RD3Surface
 * runs both under a recursive lock of the table below (recursive: a
 * synchronous terminate from inside clientClose re-enters on the same
 * thread). Should didTerminate win, clientClose finds the pointer NULL but
 * skips its body: it tests the "started" byte (+0xf2) first, which
 * stopLocked and detach_surface clear before the pointer (probe run 6). */
/* The race is per surface: a small striped table keyed by the surface's
 * address, so one surface's long close never holds up another's
 * termination (ECC review); no member added to the subclass of a base read
 * by raw offsets. */
enum { kRd3SurfLocks = 16 };
static IORecursiveLock *volatile gSurfCloseLock[kRd3SurfLocks];
static bool rd3SurfCloseLockOnce(void)
{
    for (unsigned i = 0; i < kRd3SurfLocks; i++) {
        if (!gSurfCloseLock[i]) {
            IORecursiveLock *l = IORecursiveLockAlloc();
            if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSurfCloseLock[i])) IORecursiveLockFree(l);
        }
        if (!gSurfCloseLock[i]) return false;
    }
    return true;
}
/* returns whether the lock was taken: the exit unlocks only then */
bool rd3AccelSurfaceGuard(bool enter, const void *surf, const char *who)
{
    static SInt32 gWaitLogs;
    IORecursiveLock *l = gSurfCloseLock[((uintptr_t)surf >> 6) % kRd3SurfLocks];
    if (!l) return false;
    if (!enter) { IORecursiveLockUnlock(l); return true; }
    if (!IORecursiveLockTryLock(l)) {
        if (OSIncrementAtomic(&gWaitLogs) < 8)
            ALOG("surface %p %s waits for its close/terminate twin (card run 50 race)", surf, who);
        IORecursiveLockLock(l);
    }
    return true;
}

static bool rd3AccelLocksInit(void)
{
    return rd3SimpleLockOnce(&gHeadFbResLock) && rd3SimpleLockOnce(&gNVSVPendLock) &&
           rd3SimpleLockOnce(&gIopFlipCacheLock) && rd3LockOnce(&gNVSVStageLock) &&
           rd3SimpleLockOnce(&gA3DispLock) &&
           rd3SurfCloseLockOnce();
}

extern "C" {
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
}
struct RD3IopAsync { void *pipe; IOService *fb; IOSurface *s; struct RD3KFlipRequest f; volatile SInt32 busy; struct thread_call *call; };
static RD3IopAsync gIopAs[kRd3MaxFB];
static SInt32 gIopAsyncRefuseRun;
static bool rd3AccelIopAsyncBusy(const void *pipe)
{
    for (unsigned h = 0; h < kRd3MaxFB; h++)
        if (gIopAs[h].pipe == pipe && __atomic_load_n(&gIopAs[h].busy, __ATOMIC_ACQUIRE)) return true;
    return false;
}
static void rd3AccelIopAsyncRun(void *p0, void *)
{
    RD3IopAsync *e = (RD3IopAsync *)p0;
    const unsigned head = (unsigned)(e - gIopAs);
    IOSurface *s = e->s; e->s = nullptr;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("rd3FlipToSurfacePure");
    IOReturn rr = sym ? e->fb->callPlatformFunction(sym, false, &e->f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr == kIOReturnSuccess) {
        gIopAsyncRefuseRun = 0;
        if (gIopFlipPrev[head]) ((OSObject *)gIopFlipPrev[head])->release();
        gIopFlipPrev[head] = gIopFlipFront[head]; gIopFlipFront[head] = s; gIopFlipped[head] = true;
        const SInt64 n = OSAddAtomic64(1, &gCntIopFlip); OSAddAtomic64(1, &gCntIopAsync);
        if (n < 4 || n == 600 || (n & 0xffff) == 0)
            ALOG("iop: ASYNC swap #%lld head %u: surface 0x%x %ux%u pitch %u scanned out of its own VRAM (flipResult %d)",
                 (long long)n + 1, head, s->getSurfaceID(), e->f.width, e->f.height, e->f.pitch, e->f.flipResult);
    } else {
        OSAddAtomic64(1, &gCntIopAsyncRefused); OSAddAtomic64(1, &gCntIopFlipNo[7]);
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: async flip REFUSED (0x%x, flipResult %d) head %u -- that frame is not on the panel",
                                                     (unsigned)rr, e->f.flipResult, head); }
        if (++gIopAsyncRefuseRun >= 3 && gIopAsync) {
            gIopAsync = 0;
            ALOG("iop: b89k 3 async refusals in a row -- debug.rdna3accel_iop_async=0 (synchronous flip, which copies on refusal)");
        }
        ((OSObject *)s)->release();
    }
    __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE);
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(e->pipe, nullptr);
}
static bool rd3AccelIopAsyncStart(void *pipe, IOService *fb, unsigned head, IOSurface *s, const struct RD3KFlipRequest *f)
{
    RD3IopAsync *e = &gIopAs[head];
    for (unsigned i = 0; i < 50 && __atomic_load_n(&e->busy, __ATOMIC_ACQUIRE); i++) IOSleep(1);
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
        OSAddAtomic64(1, &gCntIopAsyncDrop);
        return true;
    }
    if (!e->call) e->call = thread_call_allocate(rd3AccelIopAsyncRun, e);
    if (!e->call) return false;
    ((OSObject *)s)->retain();
    e->pipe = pipe; e->fb = fb; e->s = s;
    for (unsigned i = 0; i < sizeof *f; i++) ((volatile unsigned char *)&e->f)[i] = ((const unsigned char *)f)[i];
    __atomic_store_n(&e->busy, 1, __ATOMIC_RELEASE);
    thread_call_enter(e->call);
    return true;
}
static bool rd3AccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)
{
    if (!gIopFlip || !fb || !s || head >= kRd3MaxFB) return false;
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a) return rd3AccelIopFlipNo(1);
    if (s->getPixelFormat() != 0x42475241u) return rd3AccelIopFlipNo(2);
    RD3Resource *res = nullptr; RD3VidMemory *vm = nullptr; void *kmem = nullptr; unsigned long long bytes = 0;
    if (rd3AccelIopCacheGet(s, &res, &vm, &kmem, &bytes)) {
        OSAddAtomic64(1, &gCntIopFlipHit);
    } else {
        if (!rd3FamLock(a, true)) return rd3AccelIopFlipNo(3);
        void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
        OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
        res = OSDynamicCast(RD3Resource, ctx);
        vm = res ? rd3res_vidmem(res) : nullptr;
        kmem = (vm && vm->fRmHandle && vm->fBytes && !vm->fScanout) ? vm->fRmHandle : nullptr;
        bytes = vm ? vm->fBytes : 0;
        if (cache) ((OSObject *)cache)->release();
        rd3FamUnlock(a);
        OSAddAtomic64(1, &gCntIopFlipMiss);
        if (kmem) rd3AccelIopCachePut(s, res, vm, kmem, bytes);
    }
    volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
    const bool vramNewest = valid && (valid[6] & 1u);
    if (!kmem) return rd3AccelIopFlipNo(4);
    if (!vramNewest) return rd3AccelIopFlipNo(5);
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || P < 4 * W || H * P > bytes) return rd3AccelIopFlipNo(6);
    struct RD3KFlipRequest f; rd3SVZero(&f, sizeof f);
    f.version = RD3K_FLIP_ABI_VERSION; f.kapiMemory = kmem; f.allocationCookie = vm->fAllocationCookie; f.width = (unsigned)W; f.height = (unsigned)H; f.pitch = (unsigned)P;
    rd3AccelNotePresented(vm, head);
    if (gIopAsync && pipe) return rd3AccelIopAsyncStart(pipe, fb, head, s, &f);
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("rd3FlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr != kIOReturnSuccess) {
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: flip REFUSED (0x%x, flipResult %d) head %u %llux%llu pitch %llu -- copying",
                                                     (unsigned)rr, f.flipResult, head, W, H, P); }
        return rd3AccelIopFlipNo(7);
    }
    ((OSObject *)s)->retain();
    if (gIopFlipPrev[head]) ((OSObject *)gIopFlipPrev[head])->release();
    gIopFlipPrev[head] = gIopFlipFront[head]; gIopFlipFront[head] = s; gIopFlipped[head] = true;
    const SInt64 n = OSAddAtomic64(1, &gCntIopFlip);
    if (n < 4 || n == 600 || (n & 0xffff) == 0)
        ALOG("iop: zero-copy swap #%lld head %u: surface 0x%x %llux%llu pitch %llu scanned out of its own VRAM (flipResult %d)",
             (long long)n + 1, head, s->getSurfaceID(), W, H, P, f.flipResult);
    return true;
}
static int rdna3accel_crc_sysctl SYSCTL_HANDLER_ARGS
{
    unsigned v = 0;
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    IOService *fb = a ? a->fFB : nullptr;
    /* DCN OTG CRC (amdgpu_dm_crc.c); the framebuffer answers once Stage 3 has heads */
    if (fb && gCrcHead >= 0 && gCrcHead < 8) {
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_FN_DISPLAY_CRC);
        if (sym) { fb->callPlatformFunction(sym, false, (void *)(uintptr_t)gCrcHead, &v, nullptr, nullptr); sym->release(); }
    }
    int out = (int)v;
    return sysctl_handle_int(oidp, &out, 0, req);
}
static void rd3AccelIopHome(IOService *fb, unsigned head)
{
    if (!fb || head >= kRd3MaxFB || !gIopFlipped[head]) return;
    struct RD3KFlipRequest f; rd3SVZero(&f, sizeof f);
    f.version = RD3K_FLIP_ABI_VERSION; f.flags = 1u;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("rd3FlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    OSAddAtomic64(1, &gCntIopFlipHome);
    if (rr != kIOReturnSuccess && gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: HOME flip refused (0x%x) head %u", (unsigned)rr, head); }
    gIopFlipped[head] = false;
}
/* True when the surface's system memory holds its newest pixels, after
 * moving them there if the GPU drew them into VRAM. Unlike
 * the old scan-out sync it waits a few ms for the accelerator lock and
 * keeps the pending mark when the page-off fails. At most 4 frames in a row
 * are held back per head: a surface that never syncs is copied as before
 * rather than freezing the panel (NullMoth c2af33d). */
static unsigned gIopCopyHeld[kRd3MaxFB];
static bool rd3AccelSurfCopyReady(IOSurface *s, unsigned head)
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!s || !a) return true;
    const unsigned id = _ZNK9IOSurface12getSurfaceIDEv((void *)s);
    bool ready = !rd3SVPend(id, 2);
    if (!ready) {
        bool locked = false;
        for (unsigned i = 0; i < 8 && !(locked = rd3FamLock(a, true)); i++) IOSleep(1);
        if (locked) {
            void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
            OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
            RD3Resource *res = OSDynamicCast(RD3Resource, ctx);
            volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
            if (!res || !valid || (valid[0] & 1u)) ready = true;
            else { bool out = false; res->pageoff(nullptr, true, &out, 0); ready = out || (valid[0] & 1u); }
            if (ready) rd3SVPend(id, 0);
            if (cache) ((OSObject *)cache)->release();
            rd3FamUnlock(a);
        } else if (++gNVSVLockMiss <= 8 || (gNVSVLockMiss & 1023) == 0) {
            ALOG("surfvram copy: accelerator lock busy for 8 ms, surface %u stays pending (misses %u)", id, gNVSVLockMiss);
        }
    }
    if (head >= kRd3MaxFB) return true;
    if (ready) { gIopCopyHeld[head] = 0; return true; }
    if (++gIopCopyHeld[head] > 4) { gIopCopyHeld[head] = 0; return true; }
    OSAddAtomic64(1, &gCntIopCopySkip);
    return false;
}

static unsigned rd3VaAllocLogged, rd3VaMapLogged;

static unsigned long long rd3AccelGpuVaAlloc(IOAccelMemoryMap *map,
                                            unsigned long long len, unsigned long long align)
{
    RD3MemoryMap *mm = OSDynamicCast(RD3MemoryMap, map);
    if (!mm || !len) return 0;
    if (mm->fGpuVa) return mm->fGpuVa;

    struct RD3KRmHandles h;
    if (!rd3AccelRmHandles(&h)) return 0;

    struct RD3KGpuVaRequest g;
    rd3AccelZero(&g, sizeof g);
    g.version = RD3K_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.size = len; g.align = align;
    if (rd3AccelGpuVaCall(RD3K_GPUVA_FN_ALLOC, &g) != kIOReturnSuccess || !g.gpuva) {
        if (rd3VaAllocLogged < 8) { rd3VaAllocLogged++;
            ALOG("gpuVaAlloc: RM refused %llu bytes (align %llu) -> status 0x%x", len, align, g.status); }
        return 0;
    }
    mm->fHVirt = g.hVirt; mm->fGpuVa = g.gpuva; mm->fVaLen = len;
    if (rd3VaAllocLogged < 8) { rd3VaAllocLogged++;
        ALOG("gpuVaAlloc: %llu bytes -> gpuVA 0x%llx (hVirt 0x%x)", len, g.gpuva, g.hVirt); }
    return g.gpuva;
}

static void rd3AccelGpuVaFree(IOAccelMemoryMap *map)
{
    RD3MemoryMap *mm = OSDynamicCast(RD3MemoryMap, map);
    if (!mm || !mm->fHVirt) return;
    struct RD3KRmHandles h;
    if (!rd3AccelRmHandles(&h)) return;
    struct RD3KGpuVaRequest g;
    rd3AccelZero(&g, sizeof g);
    g.version = RD3K_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = mm->fHVirt; g.hMemory = mm->fMapped ? mm->fHRmMemory : 0; g.size = mm->fVaLen;
    g.gpuva = mm->fGpuVa;
    rd3AccelGpuVaCall(RD3K_GPUVA_FN_FREE, &g);
    OSAddAtomic64(1, &gCntMapFree);
    mm->fHVirt = 0; mm->fGpuVa = 0; mm->fVaLen = 0; mm->fMapped = false;
}

bool RD3MemoryMap::commitIntoGPUPageTable()
{
    if (fMapped) return true;
    RD3VidMemory *vm = OSDynamicCast(RD3VidMemory, fMem);
    void *kapiMem = vm ? vm->fRmHandle : nullptr;
    unsigned long long len = fVaLen ? fVaLen : getLength();
    if (!fHVirt || !kapiMem || !len) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: NOT MAPPED -- hVirt 0x%x kapiMem %p len %llu (mem %p%s)",
                 fHVirt, kapiMem, len, fMem, vm ? "" : ", not an RD3VidMemory"); }
        return false;
    }
    struct RD3KRmHandles h;
    if (!rd3AccelRmHandles(&h)) return false;
    struct RD3KGpuVaRequest g;
    rd3AccelZero(&g, sizeof g);
    g.version = RD3K_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice; g.kapiMemory = kapiMem;
    g.hVirt = fHVirt; g.size = len; g.gpuva = fGpuVa;
    g.isVidmem = 1;
    if (rd3AccelGpuVaCall(RD3K_GPUVA_FN_MAP, &g) != kIOReturnSuccess) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: MAP_MEMORY_DMA refused gpuVA 0x%llx len %llu -> status 0x%x",
                 fGpuVa, len, g.status); }
        return false;
    }
    fHRmMemory = g.hMemory; fMapped = true;
    OSAddAtomic64(1, &gCntMapCommit);
    if (g.gpuva) fGpuVa = g.gpuva;
    if (fCommits < 8) { fCommits++;
        ALOG("MM commit: MAPPED gpuVA 0x%llx len %llu <- hMem 0x%x  *** the GPU can reach these bytes ***",
             fGpuVa, len, g.hMemory); }
    return true;
}

void RD3MemoryMap::releaseFromGPUPageTable()
{
    if (!fMapped || !fHVirt) return;
    struct RD3KRmHandles h;
    if (!rd3AccelRmHandles(&h)) return;
    struct RD3KGpuVaRequest g;
    rd3AccelZero(&g, sizeof g);
    g.version = RD3K_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = fHVirt; g.hMemory = fHRmMemory; g.size = fVaLen;
    g.gpuva = fGpuVa;
    g.flags = RD3K_GPUVA_FLAG_UNMAP_ONLY;
    rd3AccelGpuVaCall(RD3K_GPUVA_FN_FREE, &g);
    fMapped = false;
    OSAddAtomic64(1, &gCntMapRelease);
}

static void rd3AccelWriteStamp(int index, unsigned int value)
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a || !a->fStamps || index < 0 || index >= 4096) return;
    volatile unsigned int *base = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
    if (!base) return;
    base[index] = value;
}

static unsigned rd3FrontSamples, rd3FrontFlips;
static unsigned rd3FrontLastSum;
static void rd3AccelSampleFront(RD3VidMemory *vm, unsigned w, unsigned h, unsigned pitch)
{
    if (!vm || !vm->fKva || !pitch || !w || !h) return;
#define RD3FRONT_EVERY 10u
    if (++rd3FrontFlips % RD3FRONT_EVERY) return;
    if (rd3FrontSamples >= 40) return;
    rd3FrontSamples++;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    const unsigned p1 = (w > 100  && h > 100)  ? px[100  * ppr + 100]  : 0u;
    const unsigned p2 = (w > 960  && h > 540)  ? px[540  * ppr + 960]  : 0u;
    const unsigned p3 = (w > 1800 && h > 1000) ? px[1000 * ppr + 1800] : 0u;
    const bool moved = (rd3FrontSamples > 1) && (sum != rd3FrontLastSum);
    ALOG("FRONT SURFACE sample %u: sum 0x%08x %s nonzero %u/%u "
         "px(100,100)=0x%08x px(960,540)=0x%08x px(1800,1000)=0x%08x %ux%u pitch %u",
         rd3FrontSamples, sum,
         moved ? "*** MOVED -- A LIVE WRITER IS PAINTING THE SCANNED-OUT SURFACE ***"
               : (rd3FrontSamples > 1 ? "(UNCHANGED since last sample -- frozen, not proof of a picture)"
                                     : "(first sample -- motion is the proof, wait for the next)"),
         nonzero, n, p1, p2, p3, w, h, pitch);
    rd3FrontLastSum = sum;
}

static unsigned rd3SwapProbes;
static void rd3AccelResStats(IOAccelResource2 *res, const char *tag)
{
    if (!res) { ALOG("  swap %s: (null)", tag); return; }
    RD3VidMemory *vm = rd3res_vidmem(res);
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!vm || !vm->fKva || !pitch || !w || !h) {
        ALOG("  swap %s: %ux%u pitch %u vm %p kva %p scanout %d -- NOT SAMPLEABLE",
             tag, w, h, pitch, vm, vm ? vm->fKva : nullptr, vm ? (int)vm->fScanout : -1);
        return;
    }
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    ALOG("  swap %s: %ux%u pitch %u scanout %d  sum 0x%08x  nonzero %u/%u  %s",
         tag, w, h, pitch, (int)vm->fScanout, sum, nonzero, n,
         nonzero ? "*** HAS CONTENT -- THIS IS THE SOURCE ***" : "(empty)");
}
bool rd3AccelResHasContent(IOAccelResource2 *res)
{
    if (!res) return false;
    RD3VidMemory *vm = rd3res_vidmem(res);
    if (!vm || !vm->fKva) return false;
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || pitch < 4) return false;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    for (unsigned iy = 1; iy <= 4; iy++)
        for (unsigned ix = 1; ix <= 4; ix++) {
            const unsigned x = (w * ix) / 5, y = (h * iy) / 5;
            if (x < w && y < h && px[y * ppr + x]) return true;
        }
    return false;
}

void rd3AccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2)
{
    if (rd3SwapProbes >= 6) return;
    rd3SwapProbes++;
    ALOG("SWAP PROBE %u -- which argument holds the composited frame?", rd3SwapProbes);
    rd3AccelResStats(a1, "a1");
    rd3AccelResStats(a2, "a2");
}

static unsigned rd3FlipWhy = 0;
#define RD3FLIPWHY(...) do { if (rd3FlipWhy < 20) { rd3FlipWhy++; ALOG(__VA_ARGS__); } } while (0)

#define RD3SCANOUT_COPY_MAX 120
static unsigned rd3ScanoutCopies = 0;
/* A2 (rdna3accelsdma=1): SDMA copies (no cap: the GPU does them), the
 * failures in a row that send the swaps back to the CPU */
static unsigned rd3SdmaCopies = 0, rd3SdmaFailRun = 0;
static bool rd3SdmaCopyOff = false;

static bool rd3AccelCopyBySdma(RD3VidMemory *vm, IOService *fb, unsigned head, const struct RD3KVramRequest *q,
                               unsigned pitch, unsigned rowb, unsigned rows)
{
    struct RD3KCopyRequest c;
    for (unsigned i = 0; i < sizeof c; i++) ((volatile unsigned char *)&c)[i] = 0;
    c.version = RD3K_COPY_ABI_VERSION;
    c.srcCookie = vm->fAllocationCookie;
    c.dstCookie = q->allocationCookie;
    c.srcPitch = pitch;
    c.dstPitch = q->pitch;
    c.rowBytes = rowb;
    c.rows = rows;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_COPY_FN);
    if (!sym) return false;
    IOReturn ret = fb->callPlatformFunction(sym, false, &c, nullptr, nullptr, nullptr);
    sym->release();
    if (ret != kIOReturnSuccess) {
        rd3SdmaFailRun++;
        ALOG("swap copy by SDMA refused (0x%x, status %d) head %u: %u rows x %u bytes 0x%llx -> 0x%llx%s", (unsigned)ret,
             c.status, head, rows, rowb, c.srcCookie, c.dstCookie,
             rd3SdmaFailRun >= 3 ? " -- three in a row: the CPU copies from now on" : "");
        if (rd3SdmaFailRun >= 3) rd3SdmaCopyOff = true;
        return false;
    }
    rd3SdmaFailRun = 0;
    rd3SdmaCopies++;
    if (rd3SdmaCopies <= 3 || rd3SdmaCopies % 600 == 0)
        ALOG("swap copy by SDMA (head %u) #%u: %u rows x %u bytes, 0x%llx pitch %u -> 0x%llx pitch %u in %llu us",
             head, rd3SdmaCopies, rows, rowb, c.srcCookie, pitch, c.dstCookie, q->pitch, c.usec);
    return true;
}

static bool rd3AccelCopyToScanout(IOAccelResource2 *res, IOService *fb, unsigned head)
{
    const bool bySdma = gBootSdmaCopy && !rd3SdmaCopyOff;
    if (!bySdma && rd3ScanoutCopies >= RD3SCANOUT_COPY_MAX) return false;
    if (!res) return false;
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a || !fb) { RD3FLIPWHY("copyToScanout: no accelerator/framebuffer for head %u", head); return false; }
    RD3VidMemory *vm = rd3res_vidmem(res);
    if (!vm || !vm->fKva) { RD3FLIPWHY("copyToScanout: source has no kva"); return false; }

    const unsigned char *rr = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(rr + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(rr + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(rr + 0xb8);
    if (!w || !h || pitch < 4) { RD3FLIPWHY("copyToScanout: source geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct RD3KVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = RD3K_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q, fb);
    if (ret != kIOReturnSuccess || !q.kva || !q.pitch) {
        RD3FLIPWHY("copyToScanout: framebuffer would not describe its scanout (0x%x kva %p pitch %u)",
                  (unsigned)ret, q.kva, q.pitch);
        return false;
    }

    const unsigned rows = (h < q.height) ? h : q.height;
    const unsigned rowb = ((pitch < q.pitch) ? pitch : q.pitch) & ~3u;
    if (bySdma && rows && rowb && rd3AccelCopyBySdma(vm, fb, head, &q, pitch, rowb, rows))
        return true;
    if (rd3ScanoutCopies >= RD3SCANOUT_COPY_MAX) return false;
    const unsigned words = rowb / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    const volatile unsigned int *s = (const volatile unsigned int *)vm->fKva;
    volatile unsigned int *d = (volatile unsigned int *)q.kva;
    for (unsigned y = 0; y < rows; y++) {
        const volatile unsigned int *sp = s + (unsigned long)y * spr;
        volatile unsigned int *dp = d + (unsigned long)y * dpr;
        for (unsigned x = 0; x < words; x++) dp[x] = sp[x];
    }
    rd3ScanoutCopies++;
    if (rd3ScanoutCopies <= 3 || rd3ScanoutCopies == RD3SCANOUT_COPY_MAX)
        ALOG("*** COPIED THE COMPOSITED FRAME ONTO THE PANEL *** (head %u) #%u: %ux%u, %u rows x %u bytes, "
             "src kva %p pitch %u -> scanout kva %p phys 0x%llx pitch %u%s",
             head, rd3ScanoutCopies, w, h, rows, rowb, vm->fKva, pitch, q.kva,
             (unsigned long long)q.phys, q.pitch,
             rd3ScanoutCopies == RD3SCANOUT_COPY_MAX ? "  [CAP REACHED -- stopping, panel keeps this frame]" : "");
    return true;
}

static bool rd3AccelFlipTo(IOAccelResource2 *res, IOService *fb, unsigned head)
{
    if (!res) { RD3FLIPWHY("rd3AccelFlipTo: no resource"); return false; }
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a || !fb) { RD3FLIPWHY("rd3AccelFlipTo: accel %p fb %p (head %u)", a, fb, head); return false; }
    RD3VidMemory *vm = rd3res_vidmem(res);
    if (!vm || !vm->fRmHandle) {
        RD3FLIPWHY("rd3AccelFlipTo: vm %p fRmHandle %p -- the composited surface has no RM memory to flip to",
                  vm, vm ? vm->fRmHandle : nullptr);
        return false;
    }
    if (vm->fScanout) { RD3FLIPWHY("rd3AccelFlipTo: already the scanout"); return false; }

    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || !pitch) { RD3FLIPWHY("rd3AccelFlipTo: geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct RD3KFlipRequest f;
    volatile unsigned char *z = (volatile unsigned char *)&f;
    for (unsigned i = 0; i < sizeof f; i++) z[i] = 0;
    f.version = RD3K_FLIP_ABI_VERSION;
    f.kapiMemory = vm->fRmHandle; f.allocationCookie = vm->fAllocationCookie;
    f.width = w; f.height = h; f.pitch = pitch;

    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_FLIP_FN);
    if (!sym) { RD3FLIPWHY("rd3AccelFlipTo: OSSymbol allocation failed"); return false; }
    IOReturn rr = fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr);
    sym->release();
    if (rr != kIOReturnSuccess) {
        RD3FLIPWHY("rd3AccelFlipTo: framebuffer refused the flip (0x%x, flipResult %d) for %ux%u pitch %u kapiMemory %p",
                  (unsigned)rr, f.flipResult, w, h, pitch, vm->fRmHandle);
        return false;
    }
    RD3FLIPWHY("rd3AccelFlipTo: *** FLIPPED *** head %u to %ux%u pitch %u kapiMemory %p (flipResult %d)",
              head, w, h, pitch, vm->fRmHandle, f.flipResult);
    rd3AccelSampleFront(vm, w, h, pitch);
    return true;
}

/* A legacy surface swap (submitSwapCopy) names no display. Which head it is
 * for, tried in order, the first that answers wins and is logged once per
 * head (card run 15: two heads piped, every swap refused):
 *   1. one head piped: that head
 *   2. the surface was seen before: the head it got then
 *   3. an argument IS a head's display-pipe framebuffer resource (a1, the
 *      destination, before a2)
 *   4. an argument shares that resource's vidmem
 *   5. a new surface: the next piped head nobody has claimed yet, in head
 *      order (WindowServer makes one swap surface per display, in order)
 * Card run 17 (pictures of both pipes): the second monitor showed the first
 * one's frame now and then. WindowServer re-creates its pipe resources
 * (initFramebufferResource ran five times per head) and swap surfaces; a
 * freed resource's address came back for the other display and matched the
 * stale pointer. Freed surfaces and resources now leave the tables, a known
 * surface keeps its head, and every change of the answer is logged. */
static const void *volatile gSurfHead[8];
static unsigned    gSurfHeadIdx[8];
static unsigned    gSwapHow[kRd3MaxFB];
static unsigned long long gHeadSrcPhys[kRd3MaxFB];   /* what each head showed last */
static unsigned    gSwapCount, gRouteLogs;
#define RD3ROUTELOG(...) do { if (gRouteLogs < 200) { gRouteLogs++; ALOG(__VA_ARGS__); } } while (0)

void rd3AccelSurfaceGone(const void *surf)
{
    for (unsigned i = 0; i < 8; i++)
        if (gSurfHead[i] == surf) {
            RD3ROUTELOG("swap: surface %p (head %u) freed", surf, gSurfHeadIdx[i]);
            gSurfHead[i] = nullptr;
        }
}

void rd3AccelResourceGone(const void *res)
{
    unsigned gone = 0;
    if (!gHeadFbResLock) return;   /* nothing was ever entered */
    IOSimpleLockLock(gHeadFbResLock);
    for (unsigned h = 0; h < kRd3MaxFB; h++)
        if (gHeadFbRes[h] == res) {
            gHeadFbRes[h] = nullptr;
            gone |= 1u << h;
        }
    IOSimpleLockUnlock(gHeadFbResLock);
    for (unsigned h = 0; h < kRd3MaxFB; h++)
        if (gone & (1u << h))
            RD3ROUTELOG("swap: head %u's pipe resource %p freed", h, res);
}

static unsigned long long rd3res_phys(IOAccelResource2 *r)
{
    RD3VidMemory *m = rd3res_vidmem(r);
    return m ? m->fPhys : 0;
}

/* card run 58: which VRAM each head's legacy swaps scan out against what
 * WindowServer drew. A swap that named the head's framebuffer resource
 * (how 3 / 4) gives the head's swap size (the second source of
 * rd3AccelA3Policy). A summary per head at its 8th, 120th and 1200th
 * swap, then every 65536th: with display surfaces imported
 * (rdna3a3display=1), no swap that scans one of them out is the run 58
 * picture; with them kept out, no refusal at all means the check missed. */
/* card run 58 (2026-10-15d): the flicker test. Per band of the picture
 * each legacy swap puts on a head (the top 32 rows - the menu bar, status
 * items - then 7 bands over the rest), a hash of 48 columns x 2-4 rows read
 * through the BAR (as rd3AccelResHasContent does); a swap where any band
 * shows its picture of two swaps before and not the one before (A B A B)
 * is two buffers with different contents taking turns - an idle desktop
 * does not swap at all, a blinking caret does it twice a second. Counted per
 * head against the swaps hashed (debug.rdna3accel_swaps_abab_h<N> /
 * swaps_hashed_h<N>), logged at the 8th, 120th, 1200th and every 65536th;
 * a swap that cannot be hashed is counted and logged (first 4 per head) and
 * starts the run again. */
static void rd3AccelFlickerNote(unsigned head, IOAccelResource2 *src, unsigned long long phys, SInt64 n)
{
    if (!gFlicker || head >= kRd3MaxFB) return;
    if (gFlicker == 1 && gCntSwapHashed[head] >= kFlickBudget) {
        static bool said[kRd3MaxFB];
        if (!said[head]) {
            said[head] = true;
            ALOG("flicker: head %u: the test stopped after %d hashed swaps (%lld A B A B); debug.rdna3accel_flicker=2 "
                 "keeps it on", head, (int)kFlickBudget, (long long)gCntSwapAbab[head]);
        }
        return;
    }
    RD3VidMemory *vm = rd3res_vidmem(src);
    const unsigned char *r = (const unsigned char *)src;
    const unsigned w = *(const volatile unsigned short *)(r + 0xb0), h = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int *)(r + 0xb8);
    const char *skip = !vm ? "no vidmem" : !vm->fKva ? "no kernel mapping of its VRAM" : !w || !h ? "no geometry"
                     : pitch < 4u * w ? "pitch under 4 x width"
                     : (unsigned long long)h * pitch > vm->fBytes ? "geometry past its allocation" : nullptr;
    if (skip) {
        const SInt64 k = OSAddAtomic64(1, &gCntSwapHashSkip[head]);
        gSwapHashRun[head] = 0;
        if (k < 4 || (k & 0xffff) == 0)
            ALOG("flicker: head %u: swap %lld not hashed: %s (%lld skipped) -- not counted in swaps_hashed_h%u",
                 head, (long long)n, skip, (long long)k + 1, head);
        return;
    }
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    uint32_t hash[kFlickBands];
    for (unsigned b = 0; b < kFlickBands; b++) {
        unsigned rows[kFlickRows0];
        const unsigned nr = rd3_flicker_band_rows(b, kFlickBands, h, b ? kFlickRows : kFlickRows0, rows);
        uint32_t v = 2166136261u;
        for (unsigned i = 0; i < nr; i++)
            for (unsigned c = 0; c < kFlickCols; c++) {
                const unsigned x = (w * (2 * c + 1)) / (2 * kFlickCols);
                v = (v ^ (px[(unsigned long)rows[i] * (pitch / 4) + x] & 0xffffffu)) * 16777619u;
            }
        hash[b] = v;
    }
    const uint64_t run = ++gSwapHashRun[head];
    OSAddAtomic64(1, &gCntSwapHashed[head]);
    const unsigned bands = rd3_flicker_abab_bands(hash, gSwapHash1[head], gSwapHash2[head], kFlickBands, run);
    if (bands) {
        const SInt64 k = OSAddAtomic64(1, &gCntSwapAbab[head]) + 1;
        if (bands > 1) OSAddAtomic64(1, &gCntSwapAbabWide[head]);
        if (k == 8 || k == 120 || k == 1200 || (k & 0xffff) == 0)
            ALOG("flicker: head %u: swap %lld showed alternating pictures (A B A B in %u of %u bands, candidate flicker), "
                 "%lld of %lld hashed swaps so far, %lld in more than one band (phys 0x%llx, %ux%u)", head, (long long)n,
                 bands, (unsigned)kFlickBands, (long long)k, (long long)gCntSwapHashed[head],
                 (long long)gCntSwapAbabWide[head], phys, w, h);
    }
    for (unsigned b = 0; b < kFlickBands; b++) {
        gSwapHash2[head][b] = gSwapHash1[head][b];
        gSwapHash1[head][b] = hash[b];
    }
}

static void rd3AccelA3SwapNote(unsigned head, IOAccelResource2 *src, unsigned long long phys, unsigned how)
{
    if (head >= kRd3MaxFB || !src) return;
    const unsigned char *r = (const unsigned char *)src;
    const unsigned w = *(const volatile unsigned short *)(r + 0xb0), h = *(const volatile unsigned short *)(r + 0xb2);
    if (w && h && (how == 3 || how == 4)) { gHeadSwapW[head] = w; gHeadSwapH[head] = h; }
    gA3HeadShown[head] = phys;
    bool isA3 = false;
    unsigned dispN = 0, dirtySurf = 0;
    unsigned long long dispPhys = 0, dirtyPhys = 0;
    if (gCntA3DispImported && gA3DispLock) {
        IOSimpleLockLock(gA3DispLock);
        for (unsigned i = 0; i < kA3DispSlots; i++) {
            if (!gA3Disp[i].surfaceID) continue;
            if (phys && gA3Disp[i].phys == phys) isA3 = true;   /* any display BO, whichever head it was for */
            if (gA3Disp[i].heads & (1u << head)) { dispN++; dispPhys = gA3Disp[i].phys; }
        }
        dirtySurf = gA3DirtySurf[head]; dirtyPhys = gA3DirtyPhys[head];
        IOSimpleLockUnlock(gA3DispLock);
    }
    const SInt64 n = OSAddAtomic64(1, &gCntSwapHead[head]) + 1;
    const SInt64 nA3 = isA3 ? OSAddAtomic64(1, &gCntSwapA3Head[head]) + 1 : gCntSwapA3Head[head];
    rd3AccelFlickerNote(head, src, phys, n);
    if (n != 8 && n != 120 && n != 1200 && (n & 0xffff)) return;
    if (!dispN) {
        const long long refused = (long long)gCntA3DispRefused;
        ALOG("a3 display: head %u: %lld legacy swaps (%lld A B A B of %lld hashed), it scans out phys 0x%llx (%ux%u); %s (%lld refused, "
             "%lld other WindowServer surfaces, %lld of them kept out, %lld unchecked)%s", head, (long long)n,
             (long long)gCntSwapAbab[head], (long long)gCntSwapHashed[head], phys, w, h,
             gA3Display ? "no display surface of this head became a BO"
                        : "WindowServer's display framebuffers are kept out of A3, composited through copies",
             refused, (long long)gCntA3WsOther, (long long)gCntA3WsRefused,
             (long long)(gCntA3TaskMismatch + gCntA3NoName),
             /* only when WindowServer imported something the rule looked at and refused nothing */
             /* the WindowServer policy (default) refuses every WindowServer import: nothing to miss then */
             gA3Ws && !gA3Display && !gIopFlip && !refused && (gCntA3WsOther + gCntA3TaskMismatch + gCntA3NoName) > 0
                 ? " *** the refusal never fired ***" : "");
        return;
    }
    ALOG("a3 display: head %u: %lld legacy swaps, %lld scanned out an A3 BO; it shows phys 0x%llx, WindowServer drew "
         "display surface %u at phys 0x%llx last (%u BO(s) that may be this head's, e.g. 0x%llx) -- %s", head,
         (long long)n, (long long)nA3, phys, dirtySurf, dirtyPhys, dispN, dispPhys,
         nA3 ? "the panel shows what WindowServer draws"
             : "*** WINDOWSERVER DRAWS WHERE THE PANEL DOES NOT READ (card run 58) ***");
}

void rd3AccelSwapToPanel(const void *surf, IOAccelResource2 *a1, IOAccelResource2 *a2)
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a) return;
    rd3AccelCopyForwardCheck(a1);
    rd3AccelCopyForwardCheck(a2);
    IOAccelResource2 *src = rd3AccelResHasContent(a2) ? a2 : rd3AccelResHasContent(a1) ? a1 : nullptr;
    if (!src) return;
    const unsigned piped = gRd3HeadsPiped;
    const unsigned n = ++gSwapCount;
    unsigned head = kRd3MaxFB, how = 0;
    if (piped && !(piped & (piped - 1))) {
        for (head = 0; !(piped & (1u << head)); head++) ;
        how = 1;
    }
    unsigned slot = 8;
    for (unsigned i = 0; i < 8; i++)
        if (gSurfHead[i] == surf) slot = i;
    if (how == 0 && slot < 8 && (piped & (1u << gSurfHeadIdx[slot]))) { head = gSurfHeadIdx[slot]; how = 2; }
    IOAccelResource2 *pipeRes[kRd3MaxFB] = {};
    if (how == 0 && gHeadFbResLock) {
        RD3VidMemory *v1 = rd3res_vidmem(a1), *v2 = rd3res_vidmem(a2);
        IOSimpleLockLock(gHeadFbResLock);
        for (unsigned h = 0; h < kRd3MaxFB; h++)
            pipeRes[h] = gHeadFbRes[h];
        for (unsigned h = 0; how == 0 && h < kRd3MaxFB; h++)
            if (pipeRes[h] && pipeRes[h] == a1) { head = h; how = 3; }
        for (unsigned h = 0; how == 0 && h < kRd3MaxFB; h++)
            if (pipeRes[h] && pipeRes[h] == a2) { head = h; how = 3; }
        for (unsigned h = 0; how == 0 && h < kRd3MaxFB; h++) {
            RD3VidMemory *hv = rd3res_vidmem(pipeRes[h]);   /* alive: still in the table */
            if (hv && (hv == v1 || hv == v2)) { head = h; how = 4; }
        }
        IOSimpleLockUnlock(gHeadFbResLock);
    }
    if (how == 0) {
        unsigned claimed = 0;
        for (unsigned i = 0; i < 8; i++)
            if (gSurfHead[i]) claimed |= 1u << gSurfHeadIdx[i];
        for (unsigned h = 0; h < kRd3MaxFB && how == 0; h++)
            if ((piped & (1u << h)) && !(claimed & (1u << h))) { head = h; how = 5; }
    }
    if (how == 0 || head >= kRd3MaxFB || !a->fFBs[head]) {
        RD3FLIPWHY("swap: surface %p (a1 %p a2 %p) matched no head (piped 0x%x, pipe resources %p %p %p %p)",
                   surf, a1, a2, piped, pipeRes[0], pipeRes[1], pipeRes[2], pipeRes[3]);
        return;
    }
    if (slot == 8) {   /* remember the surface -> head answer */
        for (unsigned i = 0; i < 8; i++)
            if (!gSurfHead[i]) { gSurfHead[i] = surf; gSurfHeadIdx[i] = head; break; }
    } else if (gSurfHeadIdx[slot] != head) {
        RD3ROUTELOG("swap #%u: surface %p moves from head %u to head %u", n, surf, gSurfHeadIdx[slot], head);
        gSurfHeadIdx[slot] = head;
    }
    static const char *const kHow[6] = { "?", "only head piped", "surface seen before",
                                         "argument is the pipe resource",
                                         "argument shares the pipe vidmem",
                                         "new surface -> next unclaimed head" };
    if (gSwapHow[head] != how) {
        gSwapHow[head] = how;
        ALOG("swap: surface %p -> head %u (%s; a1 %p a2 %p src %s)", surf, head, kHow[how], a1, a2,
             src == a2 ? "a2" : "a1");
    }
    /* the frame each head shows: a change, or one head taking the other's
     * frame, is what made the panels swap pictures */
    const unsigned long long phys = rd3res_phys(src);
    if (phys != gHeadSrcPhys[head]) {
        for (unsigned h = 0; h < kRd3MaxFB; h++)
            if (h != head && phys && gHeadSrcPhys[h] == phys)
                RD3ROUTELOG("swap #%u: head %u now gets head %u's frame (phys 0x%llx, surface %p, %s)",
                            n, head, h, phys, surf, kHow[how]);
        RD3ROUTELOG("swap #%u: head %u frame phys 0x%llx -> 0x%llx (surface %p, %s, a1 %p a2 %p)",
                    n, head, gHeadSrcPhys[head], phys, surf, kHow[how], a1, a2);
        gHeadSrcPhys[head] = phys;
    }
    rd3AccelA3SwapNote(head, src, phys, how);
    IOService *fb = a->fFBs[head];
    if (!rd3AccelFlipTo(src, fb, head))
        rd3AccelCopyToScanout(src, fb, head);
    rd3AccelNotePresented(rd3res_vidmem(src), head);
}

/* ---- copy forward (card run 54) ----
 * CoreDisplay's copy_forward_region -> IOAccelSetSurfaceFramebufferShape with
 * shape bit 12 -> IOAccelLegacySurface::set_shape -> copy_forward, which
 * calls submitCopyForward(event, framebuffer index, buffer 0 (the front,
 * just shown), buffer 1 (the back, drawn next), rects, count) under the
 * accelerator's busy lock and, on success, marks both resources' events
 * (probe runs 8-9). The family's own answers kIOReturnUnsupported, which
 * CoreDisplay only logs ("copy forward failed ... disabling", 813 times in
 * card run 54) and ignores: the back buffer kept regions two frames old
 * (System Settings' sidebar while scrolling). The rects are the last frame's
 * damage minus this frame's (CGXDisplayCreateCopyForwardShape), so small.
 * Copied here by SDMA (rdna3CopyVram, synchronous), the CPU for small rects
 * when SDMA refuses. WindowServer composites into per-process copies of its
 * framebuffer IOSurfaces (the plugin's G13 surface-in/out): whether a copy
 * made here survives to the flip, or the next surface-out writes the stale
 * copy over it, is what rd3AccelCopyForwardCheck logs. */
struct RD3CfBounds { short x, y, w, h; };
static unsigned gCfCalls, gCfRects, gCfSdma, gCfCpu, gCfSkipped, gCfFailed, gCfSdmaFailRun, gCfLogs;
static unsigned long long gCfBytes, gCfUsec, gCfUsecMax;
static bool gCfSdmaOff;
#define RD3CFLOG(...) do { if (gCfLogs < 60) { gCfLogs++; ALOG(__VA_ARGS__); } } while (0)
/* one pixel of the last copy, looked at again when its buffer is swapped */
static struct { const void *vm; unsigned x, y, pitch, before, copied, call; bool armed; } gCfCheck;
static unsigned gCfChecks, gCfKept, gCfReverted, gCfOther, gCfNotShown;
/* the buffers that last went to a panel. A legacy swap is presented by
 * flipping its back buffer itself (zero-copy), never by the swap copy into
 * the front buffer, so the front stays empty and the back already holds
 * the last frame: a copy forward from a buffer that was never shown would
 * copy black over it (card run 56: "copied 0x00000000" on every check,
 * the screen black but for what changed). Pointers are only compared. */
static struct { const void *volatile vm; volatile unsigned long long cookie; } gCfShown[kRd3MaxFB];
static void rd3AccelNotePresented(const void *vm, unsigned head)
{
    const RD3VidMemory *m = (const RD3VidMemory *)vm;
    if (!m || head >= kRd3MaxFB) return;
    gCfShown[head].vm = nullptr;   /* never a pointer with another buffer's cookie */
    gCfShown[head].cookie = m->fAllocationCookie;
    gCfShown[head].vm = vm;
}
/* a freed vidmem leaves the table: its address may come back for a new,
 * never-shown buffer */
void rd3AccelVidmemGone(const void *vm)
{
    rd3AccelA3DispGone(vm);
    for (unsigned h = 0; vm && h < kRd3MaxFB; h++)
        if (gCfShown[h].vm == vm) gCfShown[h].vm = nullptr;
}
/* the frame on a panel now: the buffer each head showed last, or the scanout itself */
static bool rd3AccelWasPresented(const RD3VidMemory *vm)
{
    if (!vm) return false;
    if (vm->fScanout) return true;
    for (unsigned h = 0; h < kRd3MaxFB; h++)
        if (gCfShown[h].vm == vm && gCfShown[h].cookie == vm->fAllocationCookie) return true;
    return false;
}

static bool rd3res_geom(IOAccelResource2 *r, unsigned *w, unsigned *h, unsigned *pitch)
{
    const unsigned char *rr = (const unsigned char *)r;
    *w     = *(const volatile unsigned short *)(rr + 0xb0);
    *h     = *(const volatile unsigned short *)(rr + 0xb2);
    *pitch = *(const volatile unsigned int   *)(rr + 0xb8);
    return *w && *h && *pitch >= *w * 4u;
}

static unsigned rd3px(RD3VidMemory *vm, unsigned pitch, unsigned x, unsigned y)
{
    return *(const volatile unsigned int *)((const volatile unsigned char *)vm->fKva + (unsigned long)y * pitch + x * 4ul);
}

static void rd3AccelCopyForwardCheck(IOAccelResource2 *res)
{
    RD3VidMemory *vm = rd3res_vidmem(res);
    if (!vm || !gCfCheck.armed || gCfCheck.vm != vm || !vm->fKva) return;
    gCfCheck.armed = false;
    unsigned w, h, pitch;
    if (!rd3res_geom(res, &w, &h, &pitch) || pitch != gCfCheck.pitch || gCfCheck.x >= w || gCfCheck.y >= h ||
        (unsigned long long)gCfCheck.y * pitch + gCfCheck.x * 4ull + 4 > vm->fBytes) return;
    const unsigned now = rd3px(vm, pitch, gCfCheck.x, gCfCheck.y);
    const char *what;
    gCfChecks++;
    if (now == gCfCheck.copied)      { gCfKept++;     what = "KEPT (the copy reached the flip)"; }
    else if (now == gCfCheck.before) { gCfReverted++; what = "REVERTED to the stale pixel (overwritten after the copy)"; }
    else                             { gCfOther++;    what = "OTHER (redrawn or a newer frame)"; }
    if (gCfChecks <= 24 || gCfChecks % 500 == 0)
        ALOG("copy forward check #%u (call %u): pixel (%u,%u) stale 0x%08x, copied 0x%08x, at its swap 0x%08x -> %s "
             "[kept %u, reverted %u, other %u]", gCfChecks, gCfCheck.call, gCfCheck.x, gCfCheck.y, gCfCheck.before,
             gCfCheck.copied, now, what, gCfKept, gCfReverted, gCfOther);
}

/* one side of a copy forward: where its pixels are and how many bytes back
 * them. The display pipe's framebuffer vidmem is asked of its framebuffer
 * every time (as rd3AccelCopyToScanout does): a mode switch moves the
 * scanout and frees the old one, so a cached offset could aim SDMA at freed
 * VRAM. The rows are clamped to the bytes really allocated. */
struct RD3CfSide {
    RD3VidMemory *vm;
    unsigned long long cookie, bytes;
    unsigned w, h, pitch;
    bool cpu;   /* has a kernel mapping that matches cookie / pitch */
};
static bool rd3AccelCfSide(IOAccelResource2 *res, RD3CfSide *o)
{
    o->vm = rd3res_vidmem(res);
    if (!o->vm || !rd3res_geom(res, &o->w, &o->h, &o->pitch)) return false;
    if (o->vm->fScanout) {
        RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
        IOService *fb = nullptr;
        if (a && gHeadFbResLock) {
            IOSimpleLockLock(gHeadFbResLock);
            for (unsigned h = 0; h < kRd3MaxFB && !fb; h++)
                if (gHeadFbRes[h] == res) fb = a->fFBs[h];
            IOSimpleLockUnlock(gHeadFbResLock);
        }
        if (!fb) return false;
        struct RD3KVramRequest q;
        for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
        q.version = RD3K_VRAM_ABI_VERSION;
        if (a->askFramebufferForScanout(&q, fb) != kIOReturnSuccess || !q.pitch || !q.actualSize) return false;
        o->cookie = q.allocationCookie; o->bytes = q.actualSize; o->pitch = q.pitch;
        if (q.width && q.width < o->w) o->w = q.width;
        if (q.height && q.height < o->h) o->h = q.height;
        o->cpu = false;
    } else {
        if (!o->vm->fRmHandle || !o->vm->fBytes) return false;
        o->cookie = o->vm->fAllocationCookie; o->bytes = o->vm->fBytes;
        o->cpu = o->vm->fKva != nullptr;
    }
    /* 4-byte pixels only: a pitch of twice that is an 8-byte (F16 / EDR) buffer */
    if (o->pitch < o->w * 4u || (o->w >= 64 && o->pitch >= o->w * 8u)) return false;
    if ((unsigned long long)o->w * 4u > o->bytes) return false;
    const unsigned long long fit = (o->bytes - (unsigned long long)o->w * 4u) / o->pitch + 1;
    if (fit < o->h) o->h = (unsigned)fit;
    return o->h != 0;
}

static unsigned gCfSdmaOffAt;
static bool rd3AccelCfSdma(const RD3CfSide &S, const RD3CfSide &D, unsigned long long so, unsigned long long dof,
                           unsigned rowb, unsigned rows, unsigned long long *us)
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    IOService *fb = nullptr;
    for (unsigned i = 0; a && i < kRd3MaxFB && !fb; i++) fb = a->fFBs[i];
    if (gCfSdmaOff && gCfCalls - gCfSdmaOffAt >= 600) {   /* try again now and then */
        gCfSdmaOff = false; gCfSdmaFailRun = 0;
    }
    if (!fb || gCfSdmaOff) return false;
    struct RD3KCopyRequest c;
    for (unsigned i = 0; i < sizeof c; i++) ((volatile unsigned char *)&c)[i] = 0;
    c.version = RD3K_COPY_ABI_VERSION;
    c.srcCookie = S.cookie + so;
    c.dstCookie = D.cookie + dof;
    c.srcPitch = S.pitch; c.dstPitch = D.pitch; c.rowBytes = rowb; c.rows = rows;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_COPY_FN);
    if (!sym) return false;
    IOReturn ret = fb->callPlatformFunction(sym, false, &c, nullptr, nullptr, nullptr);
    sym->release();
    if (ret != kIOReturnSuccess) {
        gCfSdmaFailRun++;
        ALOG("copy forward by SDMA refused (0x%x, status %d): %u rows x %u bytes 0x%llx -> 0x%llx%s", (unsigned)ret,
             c.status, rows, rowb, c.srcCookie, c.dstCookie,
             gCfSdmaFailRun >= 3 ? " -- three in a row: small rects by the CPU for the next 600 calls" : "");
        if (gCfSdmaFailRun >= 3) { gCfSdmaOff = true; gCfSdmaOffAt = gCfCalls; }
        return false;
    }
    gCfSdmaFailRun = 0;
    *us += c.usec;
    return true;
}

/* the CPU through the BAR: reads of VRAM are uncached, so only small rects */
#define RD3CF_CPU_MAX (256u * 1024u)
static void rd3AccelCfCpu(const RD3CfSide &S, const RD3CfSide &D, unsigned long long so, unsigned long long dof,
                          unsigned rowb, unsigned rows)
{
    for (unsigned y = 0; y < rows; y++) {
        const volatile unsigned int *sp = (const volatile unsigned int *)((const volatile unsigned char *)S.vm->fKva + so + (unsigned long long)y * S.pitch);
        volatile unsigned int *dp = (volatile unsigned int *)((volatile unsigned char *)D.vm->fKva + dof + (unsigned long long)y * D.pitch);
        for (unsigned x = 0; x < rowb / 4; x++) dp[x] = sp[x];
    }
}

static bool rd3AccelCfClip(const RD3CfBounds &r, unsigned w, unsigned h, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = r.x; *y0 = r.y; *x1 = *x0 + r.w; *y1 = *y0 + r.h;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > (int)w) *x1 = (int)w;
    if (*y1 > (int)h) *y1 = (int)h;
    return *x1 > *x0 && *y1 > *y0;
}

IOReturn rd3AccelCopyForward(const void *surf, unsigned fbIndex, IOAccelResource2 *src, IOAccelResource2 *dst,
                             const void *rects, unsigned count, unsigned smode)
{
    const unsigned n = ++gCfCalls;
    RD3CfSide S = {}, D = {};
    const bool okS = src && rd3AccelCfSide(src, &S), okD = dst && rd3AccelCfSide(dst, &D);
    const RD3CfBounds *b = (const RD3CfBounds *)rects;
    if (n <= 12)
        RD3CFLOG("copy forward #%u: surface %p fb index %u smode 0x%x, %u rect(s)%s; src %p (vm %p %ux%u pitch %u "
                 "off 0x%llx of %llu%s%s) -> dst %p (vm %p %ux%u pitch %u off 0x%llx of %llu%s%s)", n, surf, fbIndex,
                 smode, count, gBootCopyForward ? "" : " [rdna3accelcf=0: refused]", src, S.vm, S.w, S.h, S.pitch,
                 S.cookie, S.bytes, S.vm && S.vm->fScanout ? " scanout" : "", okS ? "" : " NOT COPYABLE", dst, D.vm,
                 D.w, D.h, D.pitch, D.cookie, D.bytes, D.vm && D.vm->fScanout ? " scanout" : "",
                 okD ? "" : " NOT COPYABLE");
    if (n <= 12 && b)
        for (unsigned i = 0; i < count && i < 4; i++)
            RD3CFLOG("   rect %u: %d,%d %dx%d", i, b[i].x, b[i].y, b[i].w, b[i].h);
    if (!gBootCopyForward) return kIOReturnUnsupported;
    if (!okS || !okD || (count && !b)) {
        gCfFailed++;
        RD3CFLOG("copy forward #%u: buffers not copyable (src %d, dst %d) -- refused (%u refused so far)",
                 n, (int)okS, (int)okD, gCfFailed);
        return kIOReturnUnsupported;
    }
    if (S.vm == D.vm || S.cookie == D.cookie) { gCfSkipped++; return kIOReturnSuccess; }
    if (!rd3AccelWasPresented(S.vm)) {
        /* nothing to bring forward: the back buffer is the frame on the panel */
        if (gCfNotShown++ < 6 || gCfNotShown % 2000 == 0)
            ALOG("copy forward #%u: the front buffer (vm %p) was never shown -- the back buffer already holds the "
                 "last frame, nothing copied (%u such calls)", n, S.vm, gCfNotShown);
        return kIOReturnSuccess;
    }
    const unsigned w = S.w < D.w ? S.w : D.w, h = S.h < D.h ? S.h : D.h;
    const bool cpu = S.cpu && D.cpu;
    unsigned long long us = 0, bytes = 0;
    unsigned best = 0, bestArea = 0, done = 0, failed = 0;
    int x0, y0, x1, y1;
    for (unsigned i = 0; i < count; i++) {
        if (!rd3AccelCfClip(b[i], w, h, &x0, &y0, &x1, &y1)) continue;
        const unsigned area = (unsigned)(x1 - x0) * (unsigned)(y1 - y0);
        if (area > bestArea) { bestArea = area; best = i; }
    }
    /* the check pixel: one of the biggest rect whose stale and new values differ */
    unsigned cx = 0, cy = 0, before = 0, want = 0;
    bool check = false;
    if (cpu && bestArea && (n <= 400 || n % 200 == 0) && !gCfCheck.armed &&
        rd3AccelCfClip(b[best], w, h, &x0, &y0, &x1, &y1)) {
        for (unsigned gy = 1; gy <= 4 && !check; gy++)
            for (unsigned gx = 1; gx <= 4 && !check; gx++) {
                const unsigned x = (unsigned)(x0 + (x1 - x0) * (int)gx / 5), y = (unsigned)(y0 + (y1 - y0) * (int)gy / 5);
                before = rd3px(D.vm, D.pitch, x, y);
                want = rd3px(S.vm, S.pitch, x, y);
                if (before != want) { cx = x; cy = y; check = true; }
            }
    }
    for (unsigned i = 0; i < count; i++) {
        if (!rd3AccelCfClip(b[i], w, h, &x0, &y0, &x1, &y1)) continue;
        const unsigned rowb = (unsigned)(x1 - x0) * 4u, rows = (unsigned)(y1 - y0);
        const unsigned long long so = (unsigned long long)y0 * S.pitch + (unsigned)x0 * 4u;
        const unsigned long long dof = (unsigned long long)y0 * D.pitch + (unsigned)x0 * 4u;
        if (rd3AccelCfSdma(S, D, so, dof, rowb, rows, &us)) gCfSdma++;
        else if (cpu && (unsigned long long)rowb * rows <= RD3CF_CPU_MAX) { rd3AccelCfCpu(S, D, so, dof, rowb, rows); gCfCpu++; }
        else { gCfFailed++; failed++; continue; }
        bytes += (unsigned long long)rowb * rows;
        done++;
    }
    __asm__ volatile("mfence" ::: "memory");   /* the CPU's writes through the write-combined BAR */
    gCfRects += done; gCfBytes += bytes; gCfUsec += us;
    if (us > gCfUsecMax) gCfUsecMax = us;
    if (check) {
        const unsigned after = rd3px(D.vm, D.pitch, cx, cy);
        gCfCheck.vm = D.vm; gCfCheck.x = cx; gCfCheck.y = cy; gCfCheck.pitch = D.pitch;
        gCfCheck.before = before; gCfCheck.copied = after; gCfCheck.call = n;
        __asm__ volatile("" ::: "memory");
        gCfCheck.armed = true;
        if (after != want)
            RD3CFLOG("copy forward #%u: pixel (%u,%u) reads 0x%08x after the copy, the front has 0x%08x -- the copy "
                     "did NOT land", n, cx, cy, after, want);
    }
    if (n <= 12 || n % 600 == 0)
        ALOG("copy forward #%u: %u of %u rect(s), %llu KiB in %llu us%s [total: %u calls, %u rects (%u SDMA, %u CPU, "
             "%u failed), %llu MiB, %llu us SDMA, max %llu us per call, %u same buffer]", n, done, count, bytes >> 10, us,
             gCfSdmaOff ? " (SDMA off)" : "", n, gCfRects, gCfSdma, gCfCpu, gCfFailed, gCfBytes >> 20, gCfUsec,
             gCfUsecMax, gCfSkipped);
    return failed ? kIOReturnUnsupported : kIOReturnSuccess;
}

IOReturn RDNA3Accelerator::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && p1 && fn->isEqualTo(RD3ACCEL_SS_FN)) {
        if ((unsigned)(uintptr_t)p3 == RD3K_ESC_SURF_GEM)
            return rd3AccelSurfGem((struct RD3KSurfGem *)p1, (task_t)p2);
        return rd3AccelSurfVram((struct RD3KSurfShareEsc *)p1, (task_t)p2, (unsigned)(uintptr_t)p3);
    }
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

NM_TAHOE_FWD_DEFS
