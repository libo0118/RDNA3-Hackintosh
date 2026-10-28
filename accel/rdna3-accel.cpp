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
#include <kern/clock.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include "../common/rdna3_bootargs.h"
#include "../common/rdna3_a3_display.h"
#include "../common/rdna3_swap_front.h"
#include "../common/rdna3_flip_latch.h"
#include "../common/rdna3_swap_copy.h"
#include "../common/rdna3_trace.h"
#include "../common/rdna3_write_gen.h"
#include "../common/rdna3_flip_queue.h"
#include "../common/rdna3_vram_park.h"
#include "../common/rdna3_surf_pending.h"
#include "../common/rdna3_gpu_util.h"   // N49: Activity Monitor's GPU numbers
#include "../common/rdna3_tcall.h"   // thread calls stopped with public KPIs only (macOS 26: card run 2026-10-25b)
#include "../common/rdna3_fbgate.h"  // N58: the framebuffers' controller gates before the display machine
#include "../common/rdna3_iop_stale.h"   // N59: an IOP copy of a buffer nothing wrote since it was last shown
#include "../common/rdna3_iop_copy.h"    // N61: the IOP copy into the scanout and its cost
#include <IOKit/IOWorkLoop.h>
#include <mach/mach_time.h>
#include <sys/kauth.h>
#include <sys/random.h>
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
struct RD3FlipWhy { const char *why; IOReturn ret; int flipResult; };
static bool               rd3AccelFlipTo(IOAccelResource2 *res, IOService *fb, unsigned head, RD3FlipWhy *why);
static bool               rd3AccelCopyToScanout(IOAccelResource2 *res, IOService *fb, unsigned head);
void                      rd3AccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2);
void                      rd3AccelSwapToPanel(const void *surf, IOAccelResource2 *a1, IOAccelResource2 *a2,
                                              IOAccelResource2 *const *bufs, unsigned nbufs);
void                      rd3AccelSurfaceGone(const void *surf);
bool                      rd3AccelSurfaceGuard(bool enter, const void *surf, const char *who);
void                      rd3AccelResourceGone(const void *res);
bool                      rd3AccelResHasContent(IOAccelResource2 *res);
IOReturn                  rd3AccelCopyForward(const void *surf, unsigned fbIndex, IOAccelResource2 *src,
                                              IOAccelResource2 *dst, const void *rects, unsigned count,
                                              unsigned smode, IOAccelResource2 *const *bufs, unsigned nbufs);
static void               rd3AccelCopyForwardCheck(IOAccelResource2 *res);
static unsigned long long rd3AccelNotePresented(const void *vm, unsigned head);
static bool               rd3res_geom(IOAccelResource2 *r, unsigned *w, unsigned *h, unsigned *pitch);
static void               rd3AccelForgetShown(unsigned head);
static void               rd3AccelForgetShownIf(unsigned head, unsigned long long seq);
static void               rd3AccelA3DispDirty(unsigned surfaceID, unsigned long long phys);
static void               rd3AccelLatchForgetAll(const char *why);
static IOAccelResource2  *rd3AccelCfOwn(IOAccelResource2 *const *bufs, unsigned nbufs, const void *vm,
                                        IOAccelResource2 *res);
/* why rd3AccelA3Policy refused an import: picks the counter and the text of
 * the refusal's log line (card run 58b review: a WindowServer caller whose
 * task did not match was logged and counted as "a WindowServer input") */
enum RD3A3Why { kA3WhyNone = 0, kA3WhyOff, kA3WhyTaskMismatch, kA3WhyNoName, kA3WhyDisplay, kA3WhyWs };
static unsigned           rd3AccelA3Policy(void *surf, task_t task, unsigned surfaceID, unsigned *headsOut, unsigned *whyOut);
static SInt64             rd3AccelA3WhyCount(unsigned why);
static void               rd3AccelA3Why(char *why, unsigned long n, unsigned reason, unsigned heads, void *surf);
static bool               rd3AccelA3RefusedFirst(unsigned surfaceID, SInt64 n);
void                      rd3AccelVidmemGone(const void *vm);
static void               rd3AccelVidmemGoneAt(const void *vm, unsigned long long phys, bool vmToo = true);
static void               rd3AccelWriteStamp(int index, unsigned int value);
static void               rd3AccelSurfNoteDisplayClient(const char *path);
static bool               rd3AccelLocksInit(void);
/* card run 2026-10-17b: the per-frame trace (rdna3_trace.h) and the swap as
 * a copy (rdna3_swap_copy.h); hooks the classes above their definitions call */
void                      rd3AccelTraceShape(const void *surf, unsigned mask, unsigned w, unsigned h, unsigned smode,
                                             unsigned present, unsigned shaped, IOAccelResource2 *const *bufs);
void                      rd3AccelTraceSync(const void *surf, unsigned what, unsigned long long v0,
                                            unsigned long long v1);
static void               rd3AccelTraceMode(unsigned arg);
static void               rd3AccelScModeChanged(void);
static void               rd3AccelScPanelMoved(unsigned head, const char *why, uint64_t issuedAt, bool foreign);
static void               rd3AccelScFrontGone(const void *vm);
static void               rd3AccelScStaleReset(unsigned head, const char *why);
/* the write generations (card run 2026-10-17d, rdna3_write_gen.h): a write into a buffer (the family's page-on,
 * a copy forward's target), a freed buffer, a head whose panel moved without a swap */
enum { kWgAperture = 0, kWgApertureDesc = 1, kWgCf = 2, kWgApCached = 3 };
static void               rd3AccelWgWrite(const void *res, unsigned why);
static void               rd3AccelWgFreed(unsigned long long phys);
static void               rd3AccelWgHeadReset(unsigned head, const char *why);
/* APERTURE's v2: the path (getApertureMemoryDescriptor's every return, ECC review round 1) */
enum { kApWhyNew = 0, kApWhyGate = 1, kApWhyCached = 2, kApWhyNoVram = 3, kApWhyRange = 4 };
static void               rd3AccelTraceAperture(const void *res, unsigned what, bool ok, unsigned why,
                                                unsigned count = 0);
/* VMALLOC / VMFREE flags beyond the scanout's (N41, common/rdna3_trace.h): the parked allocation
 * handed back / parked instead of freed / a parked allocation released */
enum { kRd3VmTrPark = RD3_TRVM_PARKED, kRd3VmTrParkGone = RD3_TRVM_PARK_GONE, kRd3VmTrFreeLive = RD3_TRVM_FREE_LIVE,
       kRd3VmTrObjGone = RD3_TRVM_OBJ_GONE };
static void               rd3AccelTraceVm(const void *vm, bool alloc, bool ok, unsigned long long phys,
                                          unsigned long long bytes, unsigned long long mapped, IOReturn ret,
                                          bool scanout, unsigned extra = 0, unsigned v2 = 0);
static void               rd3AccelTraceSurfRes(const void *res, unsigned surfaceID, unsigned plane, unsigned how,
                                               unsigned long long phys, unsigned long long bytes,
                                               bool idZeroToo = false);
static unsigned           rd3AccelResSurfaceId(const void *res);
static unsigned           rd3AccelResType(const void *res);
static void               rd3AccelVramBase(unsigned long long phys, unsigned long long cookie);
extern "C" unsigned int   _ZNK9IOSurface12getSurfaceIDEv(void *surface);
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
/* each VRAM allocation's number (RD3VidMemory::fAllocGen, never 0, never reused; DRIVER-PLAN N38) */
static volatile SInt64 gVmAllocGen;
/* N41: aperture descriptors built again because the VRAM moved (RD3Resource::getApertureMemoryDescriptor) */
static volatile SInt64 gCntApDescMoved;
/* N41 switches (set at start from the boot-args): debug.rdna3accel_vm_sticky / rdna3vmsticky (the VRAM
 * parking, RD3VidMemory::deallocPhysical) and debug.rdna3accel_surface_wait / rdna3surfwait (the page-off's
 * wait for WindowServer's pending GPU write, rd3AccelSurfGpuWait) */
static int gVmSticky = 1, gSurfWait = 1, gSurfWaitMs = (int)RD3_SP_WAIT_MS;
static int gIopStaleWaitMs = (int)RD3_IS_WAIT_MS;   /* N59: debug.rdna3accel_iop_stale_wait_ms */
static void rdna3accelInstallSysctl(void);
static void rd3AccelInstallSysctl2(void);
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
static_assert(kRd3MaxFB <= RD3_FBGATE_MAX, "rd3_fbgate_plan looks at RD3_FBGATE_MAX heads");
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
/* debug.rdna3accel_iop_flip (the IOP zero-copy flip; its sysctl is with the IOP counters) */
static int gIopFlip = 0;
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
/* card run 2026-10-17 (rdna3_flip_latch.h): the swap waits until its flip
 * latched before it returns (sysctl debug.rdna3accel_swap_latch, boot-arg
 * rdna3swaplatch, default 1; 0 = return at once as before, the next flip
 * waits in flipToLocked), and the test of the buffers the panel showed
 * (debug.rdna3accel_swap_torn / rdna3swaptorn: 0 off, 1 (default) the first
 * 20000 checks per head, 2 always; one more band hash per swap) */
static int gSwapLatch = 1, gSwapTorn = 1;
/* card run 2026-10-18b (rd3_latch_same_shown): a mode 0 swap whose back is
 * the buffer the head already scans, its flip latched, is not flipped again
 * and not waited for (sysctl debug.rdna3accel_swap_samenoflip, boot-arg
 * rdna3swapsamenoflip, default 1; 0 = flipped and waited for as before) */
static int gSwapSameNoFlip = 1;
/* DRIVER-PLAN N38 (common/rdna3_flip_queue.h): a mode 0 swap (and, with
 * flipbuf_present 1, a flip-buffer present) programs its flip and returns;
 * one flip in flight per head, a newer present queued behind it (newest
 * wins) and programmed from the flip-done worker, the copy forward / page-on
 * into the buffer still leaving the panel waits for its latch (sysctl
 * debug.rdna3accel_swap_async, boot-arg rdna3swapasync, default 1; 0 = the
 * swap waits for its latch as before, debug.rdna3accel_swap_latch) */
static int gSwapAsync = 1;
/* an IOP transaction's zero-copy flip (iop_flip 1, iop_async 1) goes through
 * the same queue: Busy kept, the newest queued instead of dropped, the
 * transaction complete once its flip latched (debug.rdna3accel_iop_queue,
 * boot-arg rdna3iopqueue, default 1; 0 = the old worker: up to 50 ms of 1 ms
 * sleeps on WindowServer's thread, then the frame dropped) */
static int gIopQueue = 1;
/* that zero-copy flip only for VRAM the Metal plugin wrote (its dirty mark
 * recorded this allocation: rd3_iop_flip_ok), else the copy
 * (debug.rdna3accel_iop_wrote, boot-arg rdna3iopwrote, default 1) */
static int gIopWrote = 1;
/* the core's GPU activity into the accelerator's PerformanceStatistics
 * (Activity Monitor's GPU column), read-copy-modify-write every 250 ms
 * (debug.rdna3accel_perfstats, boot-arg rdna3perfstats, default 1) */
static int gPerfStats = 1;
static bool gPresReady;                 /* the present queue's lock and workers exist */
static SInt64 gCntPqIopUnwritten;        /* IOP zero-copy flips refused: the plugin never wrote that VRAM */
/* the present queue could not be set up (no lock or thread call): swap_async and iop_queue were forced to 0
 * whatever the boot-args said (sysctl debug.rdna3accel_present_init_failed, read-only; ECC review of N38) */
static int gPresInitFailed;
/* the present queue's lock (also every update of the IOP flip's scanned surface records, gIopFlipFront /
 * gIopFlipPrev, on any path); never freed */
static IOLock *volatile gPresLock;
static bool rd3AccelPresentInit(void);
static void rd3AccelPresentStop(void);
static void rd3AccelIopStop(void);
static void rd3AccelPresentReuseRes(IOAccelResource2 *res, unsigned what);
static void rd3AccelPresentForgetAll(const char *why);
static bool rd3AccelPresentIopBusy(const void *pipe);
static void rd3AccelPerfStart(void);
static void rd3AccelPerfStop(void);
enum { kTornBudget = 20000 };
/* per head: the buffer the last flip went to and the one before it (on the
 * panel until that flip latched), when it was programmed, whether it was
 * seen latched, its band hash at the swap. Written by the head's swaps (and
 * a copy forward's latch look, both under the family's lock); the free path
 * only clears pointers, which are only compared, never followed */
struct RD3HeadLatch {
    /* compare-only pointers: never followed (a buffer is read only through
     * a live resource of the swapping surface); the free path clears them
     * on any thread, a race costs one count more or less */
    const void *volatile vm, *volatile prevVm;
    unsigned long long phys, prevPhys, cookie;
    uint64_t flipAbs;
    struct rd3_latch_state st;         /* rdna3_flip_latch.h: latched, given up, timeouts in a row, waits off */
    bool hashed;
    uint32_t latchUs;                  /* flip to latch, when seen (0: not measured) */
    uint32_t hash[kFlickBands];
};
static RD3HeadLatch gLatch[kRd3MaxFB] = { { nullptr, nullptr, 0, 0, 0, 0, { 0, 0, 0, 1 }, false, 0, { 0 } },
                                            { nullptr, nullptr, 0, 0, 0, 0, { 0, 0, 0, 1 }, false, 0, { 0 } },
                                            { nullptr, nullptr, 0, 0, 0, 0, { 0, 0, 0, 1 }, false, 0, { 0 } },
                                            { nullptr, nullptr, 0, 0, 0, 0, { 0, 0, 0, 1 }, false, 0, { 0 } } };
/* the counters (debug.rdna3accel_swaps_latch*_h<N>, _torn*, cf_onpanel,
 * swaps_shownfreed, swaps_empty{filled,still}): every outcome counted */
static SInt64 gCntLatchPend[kRd3MaxFB], gCntLatchWait[kRd3MaxFB], gCntLatchUs[kRd3MaxFB], gCntLatchMax[kRd3MaxFB];
static SInt64 gCntLatchTimeout[kRd3MaxFB], gCntLatchUnknown[kRd3MaxFB], gCntLatchOff[kRd3MaxFB];
static SInt64 gCntTornChk[kRd3MaxFB], gCntTorn[kRd3MaxFB], gCntInplace[kRd3MaxFB], gCntTornSkip[kRd3MaxFB];
static SInt64 gCntCfOnPanel[kRd3MaxFB], gCntShownFreed[kRd3MaxFB], gCntEmptyFilled[kRd3MaxFB], gCntEmptyStill[kRd3MaxFB];
static SInt64 gCntLatchImmediate[kRd3MaxFB], gCntFlipFailed[kRd3MaxFB];
/* mode 0 swaps to the buffer already on the panel, not flipped again (card run 2026-10-18b) */
static SInt64 gCntLatchSame[kRd3MaxFB];
/* 1 while the head's waits are off after RD3_LATCH_TIMEOUT_RUN timeouts (a state, not a count) */
static SInt64 gCntLatchWaitsOff[kRd3MaxFB];
/* the size of each head's own legacy swaps (only a swap that named the head's
 * framebuffer resource): the second source next to the framebuffers' answers */
static volatile uint32_t gHeadSwapW[kRd3MaxFB], gHeadSwapH[kRd3MaxFB];
static SInt64 gCntA3DispRefused, gCntA3DispImported, gCntA3WsOther, gCntA3TaskMismatch, gCntA3NoName;
static SInt64 gCntSwapHead[kRd3MaxFB], gCntSwapA3Head[kRd3MaxFB];
/* card run 58b (2026-10-16): the front (a head's framebuffer resource) and
 * copy forward's source, rdna3_swap_front.h; every outcome counted (the
 * card is debugged from its report: a path without a counter is silent).
 * Swaps: skipped (nothing to show; per head when the front named it, and
 * those where the front had pixels the back did not), shown after a run of
 * empty ones, matched no head, changed buffer, new buffers.
 * Copy forwards per head: from the buffer it showed last (all rects
 * copied), from the front (it was shown), not needed (the back is the last
 * frame), nothing shown yet, the head shows another surface's buffer (never
 * a source), rects left stale (a miss: the target is copied whole next
 * time), healed by that whole copy. Globally: calls, no head known,
 * the shown buffer's record not usable, refused (not copyable),
 * same buffer, rdna3accelcf=0. */
static SInt64 gCntSwapSkipped, gCntSwapNoHead, gCntSwapSkipHead[kRd3MaxFB], gCntSwapFrontSkip[kRd3MaxFB];
static SInt64 gCntSwapEmptyShown[kRd3MaxFB], gCntSwapBufChg[kRd3MaxFB], gCntBufNew[kRd3MaxFB];
static SInt64 gCntCfShown[kRd3MaxFB], gCntCfSrc[kRd3MaxFB], gCntCfBackLast[kRd3MaxFB];
static SInt64 gCntCfNothingShown[kRd3MaxFB], gCntCfForeign[kRd3MaxFB], gCntCfMiss[kRd3MaxFB], gCntCfHealed[kRd3MaxFB];
static SInt64 gCntCfCalls, gCntCfNoHead, gCntCfShownStale, gCntCfRefused, gCntCfSame, gCntCfDisabled;
/* the pending-rects table (rdna3_swap_front.h): calls whose source blocked
 * a buffer's pending rects, calls that copied them anyway after the limit,
 * lists dropped from a full table, lists turned into the whole picture */
static SInt64 gCntCfPendBlocked, gCntCfPendForced, gCntCfPendEvicted, gCntCfPendWhole;
/* copy forwards with no head known whose rects could not all be copied:
 * stale, with no table to keep them in */
static SInt64 gCntCfNoHeadMiss;
/* guards gCfPend: copy forward runs under the family's busy lock, the
 * vidmem free path (rd3AccelVidmemGone) on any thread without it */
static IOSimpleLock *volatile gCfPendLock;
/* guards gScStale (the stale back, 2026-10-17c): the swaps and copy
 * forward under the family's lock, the free paths on any thread */
static IOSimpleLock *volatile gScStaleLock;
/* guards the write generations (gWg, gWgHead: card run 2026-10-17d): the
 * page-on path and the VRAM free path on any thread, the swaps, copy
 * forward and the flip-buffer hooks */
static IOSimpleLock *volatile gWgLock;
static IOSimpleLock *volatile gIopStaleLock;   /* N59: gIopStale */
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
int thread_call_enter_delayed(struct thread_call *call, uint64_t deadline);
int thread_call_cancel(struct thread_call *call);
int thread_call_free(struct thread_call *call);
}
static int gRd3Aperture = 0;

/* ---- the per-frame trace (card run 2026-10-17b, common/rdna3_trace.h) ----
 * Every legacy swap, copy forward, shape, VRAM allocation and free, aperture
 * access (the family's page-on), page-off, IOSurface answer, mode change and
 * flip in a ring of RD3_TRACE_CAP events (IOMalloc'd once at start, one
 * IOSimpleLock, never taken from interrupt context), a per-second table
 * (the last RD3_TRACE_SECS seconds, counts that stop at 65535), sysctls to
 * read them (root only, copied in chunks of RD3_TRACE_CHUNK) and the first
 * RD3_TRACE_BOOT_DUMP events in the kernel log. Times are
 * mach_absolute_time, as in the core's ring (debug.rdna3_drmtrace) and the
 * Metal plugin's traces. */
static struct rd3_trace_ring gTr;
static IOSimpleLock *volatile gTrLock;
static struct rd3_trace_sec *gTrSec;   /* RD3_TRACE_SECS x RD3_TRACE_SEC_ROWS */
/* the object names' SipHash key (never leaves the kernel) and the boot's
 * random id in the headers (ECC review round 2: no reversible salt) */
static uint8_t gTrKey[16];
static uint32_t gTrBootId;
static bool gTrKeyed;
static IOSimpleLock *volatile gSurfResLock;   /* the SURFRES dedup table */
static volatile SInt64 gCntTrNomem, gCntTrWritten;
static uint64_t gTrCursor;             /* the stream reader's last event number (under gTrLock) */
/* debug.rdna3accel_swap_mode / boot-arg rdna3swapmode (rdna3_swap_copy.h),
 * debug.rdna3accel_swap_ab (autoreport.sh's A/B phase, a label), and
 * debug.rdna3accel_trace_scan (1: sample the surface's other buffers at
 * every swap, 0: only at empty backs) */
static volatile int gSwapMode = RD3_SWAPMODE_DEFAULT;
static volatile int gSwapAb = 0;
static int gTrScan = 0;   /* card run 2026-10-17d: off, as in the bisect's good step 5 (sysctl turns it on) */
/* debug.rdna3accel_swap_landed / boot-arg rdna3swaplanded (card run
 * 2026-10-17c, rdna3_swap_copy.h): 1 modes 1/2 present the landed buffer
 * instead of a suspect back; 0 (the default) they only count, trace and log
 * what they would have presented (the rule fired 0 times in that run's
 * pattern and can show a half-drawn frame: the next report says whether it
 * matters) */
static int gSwapLanded = 0;
/* debug.rdna3accel_swap_stale / boot-arg rdna3swapstale (card run
 * 2026-10-17c, rdna3_swap_copy.h): 1 (the default, the bug is proven) mode 1
 * holds a back WindowServer did not redraw since the head presented it; 0
 * copies it as before and only counts (swaps_stalehold_would_h<N>) and
 * traces (SWAP v1 bit 24) what it would have held */
static int gSwapStale = 1;
/* debug.rdna3accel_swap_gen / boot-arg rdna3swapgen (card run 2026-10-17d,
 * rdna3_write_gen.h): 1 (the default) modes 0 and 1 present a back only
 * when WindowServer wrote it (the family's page-on, a copy forward into it)
 * after the frame the head shows, and copy forward takes the front as its
 * source when it was written after that frame; 0 only counts
 * (swaps_genhold_would_h<N>) and traces (SWAP v1 bit 26) what it would have
 * held. Mode 2 (the copy-everything control) never holds. */
static int gSwapGen = 1;
/* debug.rdna3accel_flipbuf_present / boot-arg rdna3flipbufpresent (card
 * run 2026-10-17d): IOAccelLegacyDisplayPipe::submitFlipBuffer and
 * submitScanoutFlipBuffer (the family's other present path, the copy
 * forward bursts with no swap) are always traced, counted and forwarded to
 * the family; 1 also presents their buffer as the swap mode does (copy
 * into D / home in mode 1, flip in mode 0) with the latch wait and the
 * write generations; 0 (the default) only traces */
static int gFlipbufPresent = 0;
static struct thread_call *gTrDumpCall;
static volatile UInt32 gTrDumpBusy;
static uint64_t gTrDumpFrom, gTrDumpTo;
static const char *gTrDumpWhy = "";
static void rd3TraceDumpQueue(uint64_t from, uint64_t to, const char *why);

static inline uint32_t rd3TrObj(const void *p)
{
    return rd3_trace_obj((uint64_t)(uintptr_t)p, gTrKey);
}
static inline struct rd3_trace_ev rd3TrEv(unsigned type, unsigned head = 0xff)
{
    struct rd3_trace_ev e;
    bzero(&e, sizeof e);
    e.type = (uint8_t)type;
    e.head = (uint8_t)(head < kRd3MaxFB ? head : 0xffu);
    return e;
}
static inline uint32_t rd3TrSat(uint64_t v)
{
    return v > 0xffffffffull ? 0xffffffffu : (uint32_t)v;
}
/* the per-second row's counts of one event */
static void rd3TrSecNote(struct rd3_trace_sec *r, const struct rd3_trace_ev &e)
{
    r->events++;
    r->mode = (uint8_t)gSwapMode;
    r->ab = (uint8_t)gSwapAb;
    switch (e.type) {
    case RD3_TR_SWAP: {
        const bool shown = e.arg == RD3_TRS_FLIP || e.arg == RD3_TRS_FLIP_COPIED || e.arg == RD3_TRS_COPY;
        rd3_trace_inc16(&r->swaps);
        if (shown) rd3_trace_inc16(&r->shown);
        if (e.arg == RD3_TRS_SKIP || e.arg == RD3_TRS_HOLD) rd3_trace_inc16(&r->held);
        if (e.flags & RD3_TRF_BACK_EMPTY) {
            rd3_trace_inc16(&r->empty);
            if (shown) rd3_trace_inc16(&r->black);
        }
        break;
    }
    case RD3_TR_CF:
        rd3_trace_inc16(&r->cf);
        if (e.v4) rd3_trace_inc16(&r->cferr);
        break;
    case RD3_TR_APERTURE:
        if (e.arg == 1) rd3_trace_inc16(&r->aperture);
        break;
    case RD3_TR_VMALLOC:
        if (!e.arg) rd3_trace_inc16(&r->vmrefused);
        break;
    case RD3_TR_VMFREE:
        if (e.flags & 0x0eu) rd3_trace_inc16(&r->freedshown);
        break;
    default:
        break;
    }
}
/* one event (its seq and time set here); thread context only */
static void rd3Trace(const struct rd3_trace_ev &in)
{
    IOSimpleLock *lk = gTrLock;
    if (!lk || !gTr.ev) {
        OSAddAtomic64(1, &gCntTrNomem);
        return;
    }
    const uint64_t t = mach_absolute_time();
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t, &ns);
    IOSimpleLockLock(lk);
    struct rd3_trace_ev *e = rd3_trace_next(&gTr);
    if (e) {
        const uint32_t seq = e->seq;
        *e = in;
        e->seq = seq;
        e->t = t;
    }
    struct rd3_trace_sec *row = rd3_trace_sec_get(gTrSec, ns / 1000000000ull, in.head < kRd3MaxFB ? in.head : 4u);
    if (row) rd3TrSecNote(row, in);
    const uint64_t w = gTr.written;
    IOSimpleLockUnlock(lk);
    gCntTrWritten = (SInt64)w;
    if (w == RD3_TRACE_BOOT_DUMP)
        rd3TraceDumpQueue(1, RD3_TRACE_BOOT_DUMP, "the first events of this boot");
}

/* the kernel log's copy: one "tr" line per event (RD3_TRACE_LINE_FMT), on
 * a thread call (never in a swap, never under the family's lock), 20 lines
 * then a 5 ms pause, one dump at a time */
static void rd3TraceDumpRun(void *, void *)
{
    unsigned k = 0, missing = 0;
    ALOG("trace: events %llu..%llu (%s) -- format: tr source seq t type head arg flags obj v0 v1 v2 v3 v4 pa pb pc "
         "ba bb (common/rdna3_trace.h, tools/rdna3_trace.py)", gTrDumpFrom, gTrDumpTo, gTrDumpWhy);
    for (uint64_t seq = gTrDumpFrom; seq && seq <= gTrDumpTo; seq++) {
        struct rd3_trace_ev e;
        bool have = false;
        IOSimpleLockLock(gTrLock);
        const long i = rd3_trace_find(&gTr, seq);
        if (i >= 0 && gTr.ev) { e = gTr.ev[i]; have = true; }
        IOSimpleLockUnlock(gTrLock);
        if (!have) { missing++; continue; }
        ALOG(RD3_TRACE_LINE_FMT, RD3_TRACE_SRC_ACCEL, e.seq, (unsigned long long)e.t, e.type, e.head, e.arg, e.flags,
             e.obj, e.v0, e.v1, e.v2, e.v3, e.v4, (unsigned long long)e.pa, (unsigned long long)e.pb,
             (unsigned long long)e.pc, (unsigned long long)rd3_trace_le64(e.ba), (unsigned long long)rd3_trace_le64(e.bb));
        if (++k % 20 == 0) IOSleep(5);
    }
    ALOG("trace: dump done, %u line(s), %u event(s) already overwritten", k, missing);
    __atomic_store_n(&gTrDumpBusy, 0, __ATOMIC_RELEASE);
}
static void rd3TraceDumpQueue(uint64_t from, uint64_t to, const char *why)
{
    if (!gTrDumpCall || !gTrLock || !OSCompareAndSwap(0, 1, &gTrDumpBusy)) return;
    gTrDumpFrom = from; gTrDumpTo = to; gTrDumpWhy = why;
    thread_call_enter(gTrDumpCall);
}
/* the ring, the per-second table, the dump's thread call; without memory
 * the trace stays off, every event counted (debug.rdna3accel_swap_trace_nomem) */
static bool rd3TraceInit(void)
{
    if (!gTrLock) {
        IOSimpleLock *l = IOSimpleLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gTrLock)) IOSimpleLockFree(l);
    }
    if (!gTrLock) return false;
    if (!gSurfResLock) {
        IOSimpleLock *l = IOSimpleLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSurfResLock)) IOSimpleLockFree(l);
    }
    if (!gTrKeyed) {   /* once: the names stay the same for the whole boot */
        read_random(gTrKey, sizeof gTrKey);
        read_random(&gTrBootId, sizeof gTrBootId);
        if (!gTrBootId) gTrBootId = 1;
        gTrKeyed = true;
    }
    if (!gTr.ev) {
        const size_t evb = (size_t)RD3_TRACE_CAP * sizeof(struct rd3_trace_ev);
        const size_t secb = (size_t)RD3_TRACE_SECS * RD3_TRACE_SEC_ROWS * sizeof(struct rd3_trace_sec);
        struct rd3_trace_ev *ev = (struct rd3_trace_ev *)IOMalloc(evb);
        struct rd3_trace_sec *sec = (struct rd3_trace_sec *)IOMalloc(secb);
        if (ev) bzero(ev, evb);
        if (sec) bzero(sec, secb);
        IOSimpleLockLock(gTrLock);
        gTrSec = sec;
        gTr.cap = ev ? RD3_TRACE_CAP : 0;
        gTr.written = 0;
        gTr.ev = ev;
        IOSimpleLockUnlock(gTrLock);
    }
    if (!gTrDumpCall) gTrDumpCall = thread_call_allocate(rd3TraceDumpRun, nullptr);
    ALOG("trace: %s (%u events, %u-second table %s, kernel log dump %s; sysctl -b debug.rdna3accel_trace, root)",
         gTr.ev ? "on" : "OFF: no memory for the ring", RD3_TRACE_CAP, RD3_TRACE_SECS, gTrSec ? "on" : "OFF",
         gTrDumpCall ? "ready" : "OFF (no thread call)");
    ALOG("trace: content samples are read through the BAR with no HDP invalidate before them, as amdgpu does on "
         "Navi 32 (amdgpu_device_invalidate_hdp is a no-op on HDP 6.0: hdp_v6_0_funcs has only flush_hdp); "
         "the trace headers say so (RD3_TRACE_HDR_HDP_NOINV)");
    return gTr.ev != nullptr;
}
/* the CPU (BAR) address of VRAM offset 0, learned from any allocation:
 * turns the HUBP's MC addresses and the cookies into the trace's addresses */
static volatile unsigned long long gVramPhysBase;
static void rd3AccelVramBase(unsigned long long phys, unsigned long long cookie)
{
    if (!gVramPhysBase && phys && phys >= cookie) gVramPhysBase = phys - cookie;
}
class RD3DisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(RD3DisplayMachine)
    NM_TAHOE_FWD(RD3DisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { ALOG("DM displayModeWillChange"); rd3AccelTraceMode(0); return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        ALOG("DM displayModeDidChange -> true");
        rd3AccelTraceMode(1);
        rd3AccelLatchForgetAll("display mode change");   /* the framebuffer flipped to its own scanout */
        rd3AccelScModeChanged();                          /* mode 1/2: the panel shows home now */
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

/* the thread that holds the family's lock, as the family's own lock path
 * tells (acceleratorDidLock / acceleratorWillUnlock, card run 2026-10-17d):
 * a flip-buffer present (debug.rdna3accel_flipbuf_present=1) runs only under
 * that lock - its own thread's, or taken when nobody holds it. Compared
 * with current_thread() only, never followed. */
static void *volatile gFamOwner;
class RDNA3Accelerator : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(RDNA3Accelerator)
    NM_TAHOE_FWD(RDNA3Accelerator)
    IOAccelRDNA3Carrier *fCarriers[RD3_CARRIERS] = { nullptr };
    unsigned            fCarrierNext = 0;
    unsigned            fCarrierLogged = 0;
public:
    void acceleratorDidLock(char const *fn, int line) APPLE_KEXT_OVERRIDE {
        IOGraphicsAccelerator2::acceleratorDidLock(fn, line);
        gFamOwner = (void *)current_thread();
    }
    void acceleratorWillUnlock(char const *fn, int line) APPLE_KEXT_OVERRIDE {
        gFamOwner = nullptr;   /* a nested unlock clears it too: a present then waits for the next time (safe) */
        IOGraphicsAccelerator2::acceleratorWillUnlock(fn, line);
    }
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
    unsigned registerPendingLocked(const char *why, unsigned gated) {
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
            if (!rd3_fbgate_registrable(1u << i, fFBRegistered, gated)) {
                ALOG("  %s: index %u published after its controller gate was planned (gated 0x%x): NOT registered now, "
                     "stopping (N58 lock order); its own boot-arg thread call or the next debug.rdna3accelfb write "
                     "registers it", why, i, gated);
                break;
            }
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
    /* N58 (card run 2026-10-26j, kernel stackshot: every -cpu host boot
     * hung there): the display machine's entry points this driver calls
     * itself (found_framebuffer, display_mode_will_change / did_change,
     * start) take the accelerator lock (IOGraphicsAccelerator2 +0x88) and
     * then, per pipe, IOFramebuffer::getAttributeExt closes the framebuffer
     * controller's gate. IOGraphics takes them the other way round: its
     * framebuffer notifications (IOAccelDisplayPipe::display_change_handler,
     * the family's own caller of these) and IOFramebuffer::message ->
     * probeAccelerator -> requestProbe run INSIDE the controller gate and
     * then take the accelerator lock. A thread call delivering the mode
     * change while AppleGPUWrangler messaged the head that had just
     * published was a lock cycle. So the gates come first, as in IOGraphics:
     * the controller workloop of every published head (IOFramebuffer::
     * getWorkLoop, the IOGraphicsControllerWorkLoop whose gate getAttributeExt
     * and message close; recursive for its owner), once each, in head order
     * (common/rdna3_fbgate.h), always under fRegLock. Lock order:
     * fRegLock -> controller gates -> accelerator lock. */
    struct FbGateRun {
        RDNA3Accelerator *me;
        IOWorkLoop *wl[kRd3MaxFB];
        unsigned n, k;
        void (*body)(RDNA3Accelerator *, int, unsigned);
        int how;
        bool ran, stale;
        unsigned gated, nowGated;
    };
    /* the published heads and their controller workloops, planned */
    void fbGatePlan(const void *fb[], const void *wlOf[], IOWorkLoop *got[], struct rd3_fbgate_plan *p) {
        for (unsigned i = 0; i < kRd3MaxFB; i++) {
            IOService *f = fFBs[i];
            got[i] = f ? f->getWorkLoop() : nullptr;
            fb[i] = f;
            wlOf[i] = got[i];
        }
        rd3_fbgate_plan(fb, wlOf, kRd3MaxFB, p);
    }
    /* one gate per level: IOWorkLoop::runAction closes the gate through the
     * workloop's own (virtual) closeGate - closeGate / openGate themselves
     * are protected. The innermost level plans the heads again (one may
     * have published while this thread waited for a gate) and runs the
     * body only when every gate the heads need now is held. */
    static IOReturn fbGateStep(OSObject *, void *arg0, void *, void *, void *) {
        FbGateRun *r = (FbGateRun *)arg0;
        if (r->k < r->n) {
            IOWorkLoop *wl = r->wl[r->k++];
            return wl->runAction(&RDNA3Accelerator::fbGateStep, r->me, r);
        }
        const void *fb[kRd3MaxFB], *wlOf[kRd3MaxFB], *held[kRd3MaxFB];
        IOWorkLoop *got[kRd3MaxFB];
        struct rd3_fbgate_plan now;
        r->me->fbGatePlan(fb, wlOf, got, &now);
        for (unsigned k = 0; k < r->n; k++)
            held[k] = r->wl[k];
        r->nowGated = now.gated;
        if (!rd3_fbgate_covered(held, r->n, wlOf, &now)) {
            r->stale = true;
            return kIOReturnSuccess;
        }
        r->gated = now.gated;
        r->body(r->me, r->how, now.gated);
        r->ran = true;
        return kIOReturnSuccess;
    }
    /* call with fRegLock held */
    void runInFbGates(const char *why, void (*body)(RDNA3Accelerator *, int, unsigned), int how) {
        unsigned prevGated = 0;
        for (unsigned t = 1; t <= RD3_FBGATE_TRIES; t++) {
            const void *fb[kRd3MaxFB], *wlOf[kRd3MaxFB];
            IOWorkLoop *got[kRd3MaxFB];
            struct rd3_fbgate_plan p;
            fbGatePlan(fb, wlOf, got, &p);
            FbGateRun r = { this, {}, 0, 0, body, how, false, false, 0, 0 };
            for (unsigned k = 0; k < p.n; k++) {
                r.wl[r.n] = got[p.head[k]];
                r.wl[r.n++]->retain();
            }
            if (t > 1)
                ALOG("N58 %s: head(s) 0x%x published while this thread waited for the gates of heads 0x%x: planning again "
                     "(try %u of %u)", why, p.gated & ~prevGated, prevGated, t, RD3_FBGATE_TRIES);
            if (p.noWl)
                ALOG("N58 %s: inside %u framebuffer controller gate(s) (heads 0x%x), then the display machine; head(s) "
                     "0x%x published WITHOUT a controller workloop: not gated, not registered now", why, p.n, p.gated, p.noWl);
            else
                ALOG("N58 %s: inside %u framebuffer controller gate(s) (heads 0x%x), then the display machine "
                     "(IOGraphics' order: gate, then the accelerator lock)", why, p.n, p.gated);
            IOReturn ret = fbGateStep(nullptr, &r, nullptr, nullptr, nullptr);
            for (unsigned k = 0; k < r.n; k++)
                r.wl[k]->release();
            if (r.ran)
                return;
            if (!r.stale) {
                ALOG("N58 %s: a controller workloop refused the action (runAction 0x%x at gate %u of %u): NOTHING "
                     "registered or delivered", why, (unsigned)ret, r.k, r.n);
                return;
            }
            prevGated = p.gated;
        }
        ALOG("N58 %s: heads kept publishing while this thread waited for their gates (%u tries, last 0x%x): NOTHING "
             "registered or delivered; write debug.rdna3accelfb again", why, RD3_FBGATE_TRIES, prevGated);
    }
    static void registerBody(RDNA3Accelerator *a, int how, unsigned gated) { a->registerFramebufferLocked(how, gated); }
    void registerFramebuffer(int how) {
        if (fRegLock) IOLockLock(fRegLock);
        runInFbGates("registerFramebuffer", &RDNA3Accelerator::registerBody, how);
        if (fRegLock) IOLockUnlock(fRegLock);
    }
    /* gated: the heads inside a held controller gate (runInFbGates, N58);
     * only those are handed to the display machine */
    bool registerFramebufferLocked(int how, unsigned gated) {
        if (!fDM) { ALOG("registerFramebuffer(%d): no display machine", how); return false; }
        if (!fFB) { ALOG("registerFramebuffer(%d): no framebuffer has published yet", how); return false; }
        unsigned before = fDM->getFramebufferCount();
        ALOG("registerFramebuffer(%d): framebuffer count before = %u", how, before);
        if (how == 2) {
            unsigned published = 0;
            for (unsigned i = 0; i < kRd3MaxFB; i++) if (fFBs[i]) published |= 1u << i;
            if (before != 0) ALOG("  already %u -- refusing to re-run the family's sweep", before);
            else if (published & ~gated)
                ALOG("  head(s) 0x%x published outside the held controller gates (gated 0x%x): refusing the family's sweep "
                     "now, it would register them with the accelerator lock before their gate (N58); write 2 again",
                     published & ~gated, gated);
            else { ALOG("  re-running IOAccelDisplayMachine::start(pci) -- the family's own sweep"); fDM->start(fPCI); }
        } else {
            registerPendingLocked("sysctl", gated);
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
    static void scanoutBody(RDNA3Accelerator *a, int how, unsigned gated) {
        if (a->registerFramebufferLocked(1, gated))
            ALOG("sysctl debug.rdna3accelfb=%d: scanout-backed display pipe ENABLED; the registration just delivered the mode "
                 "change -- not delivering it twice", how);
        else {
            ALOG("sysctl debug.rdna3accelfb=%d: scanout-backed display pipe ENABLED, re-delivering the mode change", how);
            a->deliverModeChange();
        }
    }
    static void triggerFromSysctl(int how) {
        if (!gAccel) { ALOG("sysctl debug.rdna3accelfb=%d but no RDNA3Accelerator is running", how); return; }
        if (how >= 4) gRd3Aperture = 1;
        if (how >= 3) {
            gRd3Scanout = 1;
            ALOG("  level %d: registering any published head that has no pipe yet", how);
            if (gAccel->fRegLock) IOLockLock(gAccel->fRegLock);
            gAccel->runInFbGates("debug.rdna3accelfb>=3", &RDNA3Accelerator::scanoutBody, how);
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
        gSwapLatch = (int)rd3_arg("rdna3swaplatch", 1);
        gSwapTorn = (int)rd3_arg("rdna3swaptorn", 1);
        gSwapSameNoFlip = rd3_arg("rdna3swapsamenoflip", 1) ? 1 : 0;
        gSwapAsync = rd3_arg("rdna3swapasync", 1) ? 1 : 0;
        gIopQueue = rd3_arg("rdna3iopqueue", 1) ? 1 : 0;
        gIopWrote = rd3_arg("rdna3iopwrote", 1) ? 1 : 0;
        gPerfStats = rd3_arg("rdna3perfstats", 1) ? 1 : 0;
        gVmSticky = rd3_arg("rdna3vmsticky", 1) ? 1 : 0;
        gSurfWait = rd3_arg("rdna3surfwait", 1) ? 1 : 0;
        {
            const uint32_t want = rd3_arg("rdna3iopstalewait", RD3_IS_WAIT_MS);
            gIopStaleWaitMs = (int)rd3_is_wait_ms_u(want);
            if (want != (uint32_t)gIopStaleWaitMs)
                ALOG("iop stale: boot-arg rdna3iopstalewait=%u clamped to %d ms (the most a stale IOP copy waits)", want,
                     gIopStaleWaitMs);
        }
        ALOG("iop stale: an IOP copy of a buffer nothing wrote since the head last showed it %s (N59; boot-arg "
             "rdna3iopstalewait, sysctl debug.rdna3accel_iop_stale_wait_ms; counters debug.rdna3accel_iop_stale*)",
             gIopStaleWaitMs ? "waits, bounded, for the write (the plugin's copy-out of the new frame)" : "is only counted (0)");
        if (!rd3AccelPresentInit()) {
            ALOG("present queue: no lock or thread call -- OFF: swap_async %d and iop_queue %d forced to 0 (mode 0 "
                 "swaps wait for their latch as before; debug.rdna3accel_present_init_failed=1)", gSwapAsync,
                 gIopQueue);
            gPresInitFailed = 1;
            gSwapAsync = 0;
            gIopQueue = 0;
        }
        ALOG("swap async: %s (DRIVER-PLAN N38; boot-arg rdna3swapasync, sysctl debug.rdna3accel_swap_async)",
             gSwapAsync ? "1 = a mode 0 swap / flip-buffer present programs its flip and returns: one in flight per "
                          "head, newer ones queued (newest wins), the flip-done worker programs them at the latch, "
                          "a copy forward / page-on into the buffer leaving the panel waits for it "
                          "(swaps_pq_*_h<N>)"
                        : "0 = a mode 0 swap waits for its flip's latch (debug.rdna3accel_swap_latch)");
        ALOG("iop queue: %s; iop wrote: %s (N38; boot-args rdna3iopqueue / rdna3iopwrote, sysctl "
             "debug.rdna3accel_iop_queue / _iop_wrote)",
             gIopQueue ? "1 = an IOP zero-copy flip goes through the head's present queue (Busy kept, newest "
                         "queued, the transaction complete at the latch)"
                       : "0 = the old worker (up to 50 ms of sleeps on WindowServer's thread, then dropped)",
             gIopWrote ? "1 = zero-copy only of VRAM the plugin wrote" : "0 = VRAM marked newest is enough");
        ALOG("vm sticky: %s (N41; boot-arg rdna3vmsticky, sysctl debug.rdna3accel_vm_sticky)",
             gVmSticky ? "1 = deallocPhysical parks a surface's VRAM, the next allocPhysical hands the same VRAM back "
                         "(the memory the Metal plugin draws into; vm_parked / vm_unparked / vm_park_released)"
                       : "0 = every deallocPhysical frees; the next allocPhysical gets new VRAM (the old behaviour)");
        /* the plugin learns the mode from the kernel's answer to its first pending mark (ENXIO: off, retried each
         * second); no property (ECC review of N41: the RD3AccelSurfaceWait property was read by nobody) */
        ALOG("surface wait: %s (N41; boot-arg rdna3surfwait, sysctl debug.rdna3accel_surface_wait, bound "
             "debug.rdna3accel_surface_wait_ms %d)%s",
             gSurfWait ? "1 = WindowServer's pending GPU writes are marked at commit (plugin) and a page-off / IOP "
                         "flip waits for them, bounded"
                       : "0 = no marks taken (answered ENXIO), no wait; the plugin marks after GPU completion",
             gSurfWaitMs,
             gSurfWait && !gA3Ws ? (!gA3Display && !gIopFlip
                                    ? " -- MOOT: rdna3a3ws=0 (and no rdna3a3display / IOP flip) keeps WindowServer's "
                                      "surfaces out of A3 (the plugin draws them in system memory); its pending marks "
                                      "are refused (ENXIO, the plugin backs off), the page-off wait is NOT exercised "
                                      "(N41 open item 1; rdna3a3ws=1 engages it)"
                                    : " -- only WindowServer's display framebuffers can be VRAM (rdna3a3ws=0): the "
                                      "wait covers those")
                                 : "");
        /* security review of N41: whose marks are taken (the card's report confirms the uid) */
        ALOG("surface wait: marks are taken only from a process named WindowServer that runs as _windowserver (uid "
             "%u) or is the display client (the pid of its legacy swaps / display-pipe transactions; "
             "debug.rdna3accel_surface_wait_ws_pid); others are refused (surface_wait_notreal); %u timeouts in a row "
             "with no ready stop the waits until a ready (surface_wait_starved / _skipped)", RD3_SP_WS_UID,
             RD3_SP_BUDGET);
        ALOG("perfstats: %s (N38; boot-arg rdna3perfstats, sysctl debug.rdna3accel_perfstats)",
             gPerfStats ? "1 = the core's GPU activity into the accelerator's PerformanceStatistics every 250 ms "
                          "(Device / Renderer / Tiler Utilization %)"
                        : "0 = not published (Activity Monitor shows the family's own value)");
        ALOG("swap same: %s (card run 2026-10-18b; boot-arg rdna3swapsamenoflip, sysctl "
             "debug.rdna3accel_swap_samenoflip)",
             gSwapSameNoFlip ? "1 = a mode 0 swap to the buffer the panel already scans (its flip latched) is not "
                               "flipped again and not waited for (swaps_latchsame_h<N>)"
                             : "0 = such a swap flips and waits for the latch as before");
        gSwapLanded = rd3_arg("rdna3swaplanded", 0) ? 1 : 0;
        ALOG("swap landed: %s (card run 2026-10-17c; boot-arg rdna3swaplanded, sysctl debug.rdna3accel_swap_landed)",
             gSwapLanded ? "1 = modes 1/2 present the buffer WindowServer's frame landed in instead of an empty / "
                           "stale back"
                         : "0 = only counted and traced (swaps_copylanded_would_h<N>), the back's old rule stands");
        gSwapGen = rd3_arg("rdna3swapgen", 1) ? 1 : 0;
        ALOG("swap gen: %s (card run 2026-10-17d; boot-arg rdna3swapgen, sysctl debug.rdna3accel_swap_gen)",
             gSwapGen ? "1 = modes 0 and 1 present a back only when WindowServer wrote it (page-on, copy forward) "
                        "after the head's frame, else hold it (swaps_genhold_h<N>); copy forward takes such a "
                        "front as its source (cf_gennewer_h<N>)"
                      : "0 = only counted and traced (swaps_genhold_would_h<N>), the backs presented as before");
        gFlipbufPresent = rd3_arg("rdna3flipbufpresent", 0) ? 1 : 0;
        ALOG("flipbuf present: %s (card run 2026-10-17d; boot-arg rdna3flipbufpresent, sysctl "
             "debug.rdna3accel_flipbuf_present)",
             gFlipbufPresent ? "1 = IOAccelLegacyDisplayPipe::submitFlipBuffer / submitScanoutFlipBuffer present "
                               "their buffer as the swap mode does (flipbuf_presented_h<N>)"
                             : "0 = the flip-buffer calls are traced, counted (flipbuf_h<N>) and forwarded only");
        gSwapStale = rd3_arg("rdna3swapstale", 1) ? 1 : 0;
        ALOG("swap stale: %s (card run 2026-10-17c; boot-arg rdna3swapstale, sysctl debug.rdna3accel_swap_stale)",
             gSwapStale ? "1 = mode 1 holds a back WindowServer did not redraw since the head presented it "
                          "(swaps_stalehold_h<N>)"
                        : "0 = such a back is copied as before, only counted and traced (swaps_stalehold_would_h<N>)");
        {
            const uint32_t m = rd3_arg("rdna3swapmode", RD3_SWAPMODE_DEFAULT);
            gSwapMode = rd3_swapmode_valid((long)m) ? (int)m : RD3_SWAPMODE_DEFAULT;
            ALOG("swap mode %d (card run 2026-10-17b; boot-arg rdna3swapmode%s, sysctl debug.rdna3accel_swap_mode): %s",
                 gSwapMode, rd3_swapmode_valid((long)m) ? "" : " out of range, the default taken",
                 gSwapMode == RD3_SWAPMODE_FLIP ? "0 = the panel flips to WindowServer's back (card run 2026-10-17)"
                 : gSwapMode == RD3_SWAPMODE_COPY ? "1 = the back is COPIED into a scanout buffer of the accelerator "
                                                    "and the front (Apple's submitSwapCopy), an empty back never "
                                                    "over a picture"
                                                  : "2 = copy, every back (black frames too)");
        }
        ALOG("swap latch: %s; torn test %s (card run 2026-10-17; boot-args rdna3swaplatch / rdna3swaptorn, sysctl "
             "debug.rdna3accel_swap_latch / _swap_torn)", gSwapLatch
             ? "a swap returns only once its flip latched (the panel stopped reading the old buffer, as "
               "dm_pflip_high_irq's event)"
             : "OFF: a swap returns at once, the old buffer may still be on the panel (rdna3swaplatch=0)",
             gSwapTorn == 0 ? "off" : gSwapTorn == 1 ? "on for the first 20000 checks per head" : "always on");
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
            rd3AccelPerfStart();   /* N38: Activity Monitor's GPU column (debug.rdna3accel_perfstats) */
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
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE {
        ALOG("stop");
        rd3AccelPerfStop();
        rd3AccelPresentStop();   /* N38: workers stopped, queued presents completed, thread calls freed */
        IOGraphicsAccelerator2::stop(provider);
        rd3AccelIopStop();   /* N38 (ECC review round 2): no IOP transaction comes any more */
    }
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
    rd3AccelVramBase(r.phys, r.allocationCookie);
    {
        struct rd3_trace_ev e = rd3TrEv(RD3_TR_MODE, head);
        e.arg = 2;
        e.obj = rd3TrObj(res);
        e.v0 = (r.width & 0xffffu) | (r.height << 16);
        e.v1 = r.pitch;
        e.pa = r.phys;
        rd3Trace(e);
    }
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
    const bool ok = m && m->fPhys && m->fBytes;
    rd3AccelTraceAperture(this, 1, ok, ok ? kApWhyNew : kApWhyNoVram);   /* the family's CPU access: its page-on (card run 2026-10-17b) */
    if (ok) rd3AccelWgWrite(this, kWgAperture);   /* WindowServer writes it now (card run 2026-10-17d) */
    if (ok) rd3AccelPresentReuseRes(this, 2);     /* not into a buffer still leaving the panel (N38) */
    return ok;
}

void RD3Resource::removeFromAperture() { }

void *RD3Resource::getApertureMemoryDescriptor(unsigned long long *a0)
{
    if (a0) *a0 = 0;
    if (!gRd3Aperture) {
        if (fApLogged < 4) { fApLogged++;
            ALOG("RD3Resource::getApertureMemoryDescriptor: aperture gate CLOSED -- returning nullptr "
                 "(sysctl debug.rdna3accelfb=4 opens it)"); }
        rd3AccelTraceAperture(this, 2, false, kApWhyGate);
        return nullptr;
    }
    if (fApDesc) {
        /* N41: a descriptor over a range the vidmem no longer holds (re-backed elsewhere or with another size:
         * a grown park, vm_sticky 0; or released: nothing live, nothing parked there) would send the family's
         * CPU writes into freed VRAM: built again below, or none. With no VRAM now but the same range parked
         * the cached one stays (still allocated). rd3_vp_apdesc_keep (ECC review of N41, L1) */
        RD3VidMemory *cm = rd3res_vidmem(this);
        if (cm && !rd3_vp_apdesc_keep(cm->fPhys, cm->fBytes, &cm->fPark, fApPhys, fApBytes)) {
            const SInt64 n = OSAddAtomic64(1, &gCntApDescMoved);
            if (rd3_log_due((uint64_t)n, 8, 1023))
                ALOG("RD3Resource(%p)::getApertureMemoryDescriptor: the cached descriptor covers phys 0x%llx (%llu "
                     "bytes), the vidmem now holds 0x%llx (%llu bytes)%s -- %s (vm_apdesc_moved %lld)", this,
                     fApPhys, fApBytes, cm->fPhys, cm->fBytes,
                     cm->fPhys ? "" : rd3_vp_valid(&cm->fPark) ? ", another range parked" : ", nothing parked",
                     cm->fPhys ? "built again" : "dropped", (long long)n + 1);
            fApDesc->release(); fApDesc = nullptr; fApPhys = 0; fApBytes = 0; fApCached = 0;
        }
    }
    if (fApDesc) {
        /* the cached descriptor: every page-on of a buffer comes here; the
         * first 4 per resource traced, then every 256th (v3 the count) */
        const unsigned c = ++fApCached;
        if (c <= 4 || (c & 255) == 0) rd3AccelTraceAperture(this, 2, true, kApWhyCached, c);
        rd3AccelWgWrite(this, kWgApCached);   /* the page-on's descriptor: a write follows (2026-10-17d; counted apart) */
        rd3AccelPresentReuseRes(this, 2);     /* not into a buffer still leaving the panel (N38) */
        return fApDesc;
    }
    RD3VidMemory *m = rd3res_vidmem(this);
    if (!m || !m->fPhys || !m->fBytes) {
        if (fApLogged < 8) { fApLogged++;
            ALOG("RD3Resource::getApertureMemoryDescriptor: no BAR1-backed vidmem at +0x88 (mem %p)", m); }
        rd3AccelTraceAperture(this, 2, false, kApWhyNoVram);
        return nullptr;
    }
    IODeviceMemory *d = IODeviceMemory::withRange((IOPhysicalAddress)m->fPhys, (IOPhysicalLength)m->fBytes);
    if (!d) { ALOG("RD3Resource::getApertureMemoryDescriptor: IODeviceMemory::withRange(0x%llx, %llu) FAILED",
                   m->fPhys, m->fBytes); rd3AccelTraceAperture(this, 2, false, kApWhyRange); return nullptr; }
    fApDesc = d;
    fApPhys = m->fPhys; fApBytes = m->fBytes;
    rd3AccelTraceAperture(this, 2, true, kApWhyNew);
    rd3AccelWgWrite(this, kWgApertureDesc);
    rd3AccelPresentReuseRes(this, 2);
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

/* ---- DRIVER-PLAN N41: a surface's VRAM kept across the family's deallocPhysical / allocPhysical ----
 * common/rdna3_vram_park.h. Card traces 2026-10-17d / 17c: the family freed the VRAM of surfaces the
 * Metal plugin had imported (its BO keeps the first allocation through RD3K_VRAM_FN_REF) under the live
 * surface 64 / 83 times and backed them elsewhere 13 / 19 times, so the family, the dirty mark and
 * every page-off moved to VRAM nobody wrote: grey / stale windows, windows that only flash, black
 * captures. deallocPhysical parks the allocation, the next allocPhysical hands it back when it is big
 * enough (same memory, same contents, same generation fAllocGen), a resource that grew releases it,
 * free() releases what is parked and what is still live. A parked allocation stays in every table the
 * free path cleans (present queue, latch, swap-copy, write generations): it is still allocated and
 * nothing else can be handed its range; the tables forget it when it is really released. NullMoth
 * comparison: the behaviour of NullMoth's 47ec8d3 / 6372823 (NVVidMemory's park, debug.nvaccel_vm_sticky),
 * written for this driver. debug.rdna3accel_vm_sticky / boot-arg rdna3vmsticky (default 1; 0 = every
 * deallocPhysical frees, as before; a park left from before is still reused or released). */
static volatile SInt64 gCntVmParked, gCntVmUnparked, gCntVmParkReleased, gCntVmParkGrow, gCntVmReparked,
                       gCntVmFreeLive, gCntVmReleased, gCntVmReleasedBytes, gCntVmParkedBytes,
                       gCntVmRestored, gCntVmRestoreMiss, gCntVmDirtyMoved, gCntVmReleaseFailed,
                       gCntVmNoImport, gCntVmTrimPressure, gCntVmTrimCap, gCntVmTrimmed, gCntVmTrimmedBytes,
                       gCntVmTrimHeld, gCntVmTrimUnknown, gCntVmRetryOk, gCntVmRetryFail;
/* security review of N41: parked bytes kept at most (MiB, 0 no cap): past it the oldest parks no BO holds go */
static int gVmParkCapMb = (int)RD3_VP_CAP_MB_DEFAULT;
/* the vidmems holding a park (RD3VidMemory::fParkPrev / fParkNext), oldest first; their fPark changes only
 * under this lock (an object leaves the list in free() before it goes, so one found here is alive) */
static IOLock *volatile gVmParkLock;
static RD3VidMemory *gVmParkHead, *gVmParkTail;
static void rd3VmParkUnlinkLocked(RD3VidMemory *m)
{
    if (!m->fParkListed) return;
    if (m->fParkPrev) m->fParkPrev->fParkNext = m->fParkNext; else gVmParkHead = m->fParkNext;
    if (m->fParkNext) m->fParkNext->fParkPrev = m->fParkPrev; else gVmParkTail = m->fParkPrev;
    m->fParkPrev = m->fParkNext = nullptr;
    m->fParkListed = false;
    m->fParkSince = 0;
}
static void rd3VmParkLinkLocked(RD3VidMemory *m)   /* parked now: the newest */
{
    rd3VmParkUnlinkLocked(m);
    m->fParkPrev = gVmParkTail; m->fParkNext = nullptr;
    if (gVmParkTail) gVmParkTail->fParkNext = m; else gVmParkHead = m;
    gVmParkTail = m;
    m->fParkListed = true;
    m->fParkSince = mach_absolute_time();
}
/* the list follows the object's park (under gVmParkLock) */
static void rd3VmParkSyncLocked(RD3VidMemory *m, bool newPark)
{
    if (!rd3_vp_valid(&m->fPark)) rd3VmParkUnlinkLocked(m);
    else if (newPark || !m->fParkListed) rd3VmParkLinkLocked(m);
}
static struct rd3_vp rd3VmState(const RD3VidMemory *m)
{
    struct rd3_vp v;
    v.live.handle = m->fRmHandle; v.live.kva = m->fKva; v.live.phys = m->fPhys; v.live.bytes = m->fBytes;
    v.live.cookie = m->fAllocationCookie; v.live.gen = m->fAllocGen;
    v.park = m->fPark;
    return v;
}
static void rd3VmStore(RD3VidMemory *m, const struct rd3_vp *v)
{
    m->fRmHandle = v->live.handle; m->fKva = v->live.kva; m->fPhys = v->live.phys; m->fBytes = v->live.bytes;
    m->fAllocationCookie = v->live.cookie; m->fAllocGen = v->live.gen;
    m->fPark = v->park;
    *(volatile unsigned long long *)((unsigned char *)m + 0xc8) = v->live.phys;
}
/* gives 'a' back to the framebuffer. forget: the tables drop it first (a parked one stayed in them;
 * deallocPhysical's own free already forgot it): kVmForgetAll its address and every record of the vidmem,
 * kVmForgetAddr its address only (the vidmem's records belong to a newer park: ECC review of N41). park:
 * it was parked (traced VMFREE flag 32; trExtra adds flags, e.g. the object going). A release the
 * framebuffer refuses (or with no accelerator) is logged and counted (vm_release_failed), not as released */
enum { kVmForgetNone = 0, kVmForgetAll = 1, kVmForgetAddr = 2 };
static void rd3VmRelease(RD3VidMemory *m, const struct rd3_vp_alloc *a, unsigned forget, bool park, const char *why,
                         unsigned trExtra = 0, unsigned trV2 = 0)
{
    if (!rd3_vp_valid(a)) return;
    SInt64 pr = -1;
    if (park) {
        rd3AccelTraceVm(m, false, true, a->phys, a->bytes, 0, kIOReturnSuccess, false, kRd3VmTrParkGone | trExtra,
                        trV2);
        pr = OSAddAtomic64(1, &gCntVmParkReleased);
        OSAddAtomic64(-(SInt64)a->bytes, &gCntVmParkedBytes);
    }
    if (forget) rd3AccelVidmemGoneAt(m, a->phys, forget == kVmForgetAll);
    struct RD3KVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = RD3K_VRAM_ABI_VERSION;
    r.handle = a->handle; r.kva = a->kva; r.actualSize = a->bytes; r.allocationCookie = a->cookie;
    const IOReturn ret = RDNA3Accelerator::gAccel ? RDNA3Accelerator::gAccel->askFramebufferForVram(&r, false)
                                                  : kIOReturnNotReady;
    if (ret != kIOReturnSuccess) {
        const SInt64 k = OSAddAtomic64(1, &gCntVmReleaseFailed);
        if (rd3_log_due((uint64_t)k, 8, 1023))
            ALOG("RD3VidMemory(%p): VRAM phys 0x%llx (%llu bytes, generation %llu%s) NOT given back (%s; 0x%x%s) -- "
                 "LEAKED (vm_release_failed %lld)", m, a->phys, a->bytes, a->gen, park ? ", parked" : "", why,
                 (unsigned)ret, RDNA3Accelerator::gAccel ? "" : ": no accelerator", (long long)k + 1);
        return;
    }
    const SInt64 n = OSAddAtomic64(1, &gCntVmReleased);
    OSAddAtomic64((SInt64)a->bytes, &gCntVmReleasedBytes);
    if (park && rd3_log_due((uint64_t)pr, 8, 1023))
        ALOG("RD3VidMemory(%p): parked VRAM phys 0x%llx (%llu bytes, generation %llu) released: %s "
             "(vm_park_released %lld, vm_released %lld)", m, a->phys, a->bytes, a->gen, why, (long long)pr + 1,
             (long long)n + 1);
}

/* ECC review round 3 of N41: a park rd3VmParkTrim released (at 'phys', 0 none) left the records keyed by this
 * object (the present queue's / latch's object, the swap copy's front, A3, the copy forward's shown buffer):
 * forgotten here, while the object is alive and before it is backed again. Never by address: the trim forgot
 * that one, and the framebuffer may have handed it to another vidmem since */
static void rd3VmTrimmedGone(RD3VidMemory *m, unsigned long long phys, const char *where)
{
    if (!phys) return;
    rd3AccelVidmemGoneAt(m, 0, true);
    static volatile SInt32 gVmTrimmedGoneLogs;
    const SInt32 k = OSIncrementAtomic(&gVmTrimmedGoneLogs);
    if (rd3_log_due((uint64_t)k, 4, 1023))
        ALOG("RD3VidMemory(%p): its park at 0x%llx was released by the trim -- the records of the object forgotten "
             "at its %s (%d such)", m, phys, where, (int)k + 1);
}

/* security review of N41: parks no GEM BO holds any more are given back - every one when VRAM ran out
 * (pressure, an allocPhysical refusal), else the oldest while the parked bytes exceed
 * debug.rdna3accel_vm_park_cap_mb (common/rdna3_vram_park.h rd3_vp_pick_release). A park a BO still holds
 * (the core's count above 1, rdna3VramRefs) is kept: releasing it frees nothing and moves its surface off
 * the memory the plugin draws into. Called without the family's vidmem state locks held by the caller
 * (gVmParkLock is taken here, never across a call into the core). Returns the bytes given back */
enum { kVmTrimMax = 128, kVmTrimPages = 8 };   /* candidates kept; pages of parks looked at */
static IOService *rd3AccelCore();
/* a cap release that gave nothing back (every park held by a BO) is not tried again for a second: it would
 * ask the core about every park at each deallocPhysical */
static volatile uint64_t gVmTrimCapIdleUntil;
static unsigned long long rd3VmParkTrim(bool pressure, const char *why)
{
    IOLock *l = gVmParkLock;
    const unsigned long long cap = gVmParkCapMb > 0 ? (unsigned long long)gVmParkCapMb << 20 : 0ull;
    if (!l || (!pressure && (!cap || (unsigned long long)gCntVmParkedBytes <= cap))) return 0;
    if (!pressure && mach_absolute_time() < gVmTrimCapIdleUntil) return 0;
    struct rd3_vp_cand *vc = (struct rd3_vp_cand *)IOMalloc(sizeof(struct rd3_vp_cand) * kVmTrimMax);
    unsigned char *pick = (unsigned char *)IOMalloc(kVmTrimMax);
    struct rd3_vp_alloc *rel = (struct rd3_vp_alloc *)IOMalloc(sizeof(struct rd3_vp_alloc) * kVmTrimMax);
    RD3VidMemory **relVm = (RD3VidMemory **)IOMalloc(sizeof(RD3VidMemory *) * kVmTrimMax);
    if (!vc || !pick || !rel || !relVm) {
        ALOG("VRAM parks: no memory for the %s release (%s) -- parks kept", pressure ? "pressure" : "cap", why);
        if (vc) IOFree(vc, sizeof(struct rd3_vp_cand) * kVmTrimMax);
        if (pick) IOFree(pick, kVmTrimMax);
        if (rel) IOFree(rel, sizeof(struct rd3_vp_alloc) * kVmTrimMax);
        if (relVm) IOFree(relVm, sizeof(RD3VidMemory *) * kVmTrimMax);
        return 0;
    }
    /* 1+2: the parks in park order (oldest first), a page at a time, and who holds each (no lock: the core takes
     * its own). Held / unknown ones leave the page (rd3_vp_keep_releasable, ECC review round 3): releasable parks
     * behind kVmTrimMax held ones are found, up to kVmTrimPages pages. A page resumes after the last park an
     * earlier page listed, by its park time (rd3_vp_cursor; DRIVER-PLAN N41 open item 3: by count, a park
     * unlinked between two pages let one be skipped until the next trim); a park relinked meanwhile is listed
     * again and released once (step 4 checks its handle and time under the lock) */
    IOService *core = rd3AccelCore();
    const OSSymbol *sym = core ? OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_REFS) : nullptr;
    unsigned n = 0, listed = 0, looked = 0, pages = 0, held = 0, unknown = 0;
    IOReturn qerr = kIOReturnSuccess;
    struct rd3_vp_cursor cur = { 0, 0, 0 };
    for (bool more = true; more && n < kVmTrimMax && pages < kVmTrimPages; pages++) {
        const unsigned base = n;
        const struct rd3_vp_cursor prev = cur;
        unsigned ties = 0;
        more = false;
        listed = 0;
        IOLockLock(l);
        for (RD3VidMemory *m = gVmParkHead; m; m = m->fParkNext, listed++) {
            if (!rd3_vp_valid(&m->fPark) || rd3_vp_cursor_seen(&prev, m->fParkSince, &ties)) continue;
            if (n >= kVmTrimMax) { more = true; continue; }
            vc[n].since = m->fParkSince; vc[n].bytes = m->fPark.bytes; vc[n].refs = 0;
            vc[n].obj = m; vc[n].handle = m->fPark.handle;
            rd3_vp_cursor_note(&cur, m->fParkSince);
            n++;
        }
        IOLockUnlock(l);
        looked += n - base;
        for (unsigned i = base; i < n; i++) {
            unsigned int refs = 0;
            const IOReturn qr = sym ? core->callPlatformFunction(sym, false, vc[i].handle, &refs, nullptr, nullptr)
                                    : kIOReturnNotAttached;
            if (qr == kIOReturnSuccess) vc[i].refs = refs;
            else qerr = qr;
            if (vc[i].refs > 1) held++;
            else if (!vc[i].refs) unknown++;
        }
        n = base + rd3_vp_keep_releasable(vc + base, n - base);
    }
    if (sym) sym->release();
    /* 3: which go, 4: taken out of their objects (still parked, the same allocation) */
    const unsigned long long parked = (unsigned long long)gCntVmParkedBytes;
    (void)rd3_vp_pick_release(vc, n, parked, cap, pressure ? 1 : 0, pick);
    unsigned nrel = 0;
    IOLockLock(l);
    for (unsigned i = 0; i < n; i++) {
        if (!pick[i]) continue;
        RD3VidMemory *m = gVmParkHead;
        while (m && m != (RD3VidMemory *)vc[i].obj) m = m->fParkNext;   /* still listed: alive */
        /* the same park as listed: a reuse and a new park of the same handle (a BO may hold it now, its refs
         * were read for the old one) relinks it with a new fParkSince (ECC review of 721bb26) */
        if (!m || !rd3_vp_valid(&m->fPark) || m->fPark.handle != vc[i].handle || m->fParkSince != vc[i].since)
            continue;
        rel[nrel] = m->fPark; relVm[nrel] = m; nrel++;
        m->fParkTrimmedPhys = m->fPark.phys;   /* its own records: forgotten by the object (allocPhysical / free) */
        rd3_vp_clear(&m->fPark);
        rd3VmParkUnlinkLocked(m);
    }
    IOLockUnlock(l);
    /* 5: given back. The object may be re-backed or freed (and its address reused) meanwhile: it is a key only
     * (the trace), and only the released address leaves the tables (kVmForgetAddr, ECC review round 3): nothing
     * else can hold that address before this release, so a queued / in-flight present or latch record keyed by
     * it is this allocation's; the records keyed by the object go in its own allocPhysical / free() */
    unsigned long long bytes = 0;
    for (unsigned i = 0; i < nrel; i++) {
        bytes += rel[i].bytes;
        rd3VmRelease(relVm[i], &rel[i], kVmForgetAddr, true, pressure ? "VRAM ran out (no BO holds it)"
                                                                      : "past the park cap (no BO holds it)", 0,
                     RD3_TRVMV2_NOBO);
    }
    if (!pressure) {
        uint64_t sec = 0;
        nanoseconds_to_absolutetime(1000000000ull, &sec);
        gVmTrimCapIdleUntil = nrel ? 0 : mach_absolute_time() + sec;
    }
    OSAddAtomic64(1, pressure ? &gCntVmTrimPressure : &gCntVmTrimCap);
    OSAddAtomic64((SInt64)nrel, &gCntVmTrimmed);
    OSAddAtomic64((SInt64)bytes, &gCntVmTrimmedBytes);
    OSAddAtomic64((SInt64)held, &gCntVmTrimHeld);
    OSAddAtomic64((SInt64)unknown, &gCntVmTrimUnknown);
    const SInt64 k = pressure ? gCntVmTrimPressure : gCntVmTrimCap;
    /* an unknown count (an older core kext) is logged the first time, then every 1024th (ECC review round 3) */
    static volatile SInt64 gVmTrimUnknownRuns;
    const bool unk = unknown && qerr != kIOReturnSuccess;
    const SInt64 u = unk ? OSAddAtomic64(1, &gVmTrimUnknownRuns) : 0;
    if (rd3_log_due((uint64_t)(k - 1), 8, 1023) || (unk && rd3_log_due((uint64_t)u, 1, 1023)))
        ALOG("VRAM parks: %s (%s): %u of %u listed park(s) looked at (%u page(s)), %u given back (%llu bytes), %u "
             "kept (a BO holds them), %u kept (the core's count unknown: 0x%x%s); %lld bytes parked before, cap %d "
             "MiB (vm_park_trimmed %lld)", pressure ? "VRAM ran out -- parks no BO holds released" : "past the cap",
             why, looked, listed, pages, nrel, bytes, held, unknown, (unsigned)qerr,
             qerr == kIOReturnUnsupported ? ": an older core kext without rdna3VramRefs" : "", (long long)parked,
             gVmParkCapMb, (long long)gCntVmTrimmed);
    IOFree(vc, sizeof(struct rd3_vp_cand) * kVmTrimMax);
    IOFree(pick, kVmTrimMax);
    IOFree(rel, sizeof(struct rd3_vp_alloc) * kVmTrimMax);
    IOFree(relVm, sizeof(RD3VidMemory *) * kVmTrimMax);
    return bytes;
}

bool RD3VidMemory::allocPhysical()
{
    if (fScanout) {
        if (!fPhys || !fBytes) { ALOG("RD3VidMemory::allocPhysical(%p): scanout with no address", this); return false; }
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
        rd3AccelTraceVm(this, true, true, fPhys, fBytes, 0, kIOReturnSuccess, true);
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

    /* N41: the parked allocation first (the memory the plugin draws into) */
    {
        IOLock *pl = gVmParkLock;   /* set before anything was parked */
        if (pl) IOLockLock(pl);
        struct rd3_vp v = rd3VmState(this);
        struct rd3_vp_alloc rel;
        const enum rd3_vp_take_res t = rd3_vp_take(&v, want, &rel);
        rd3VmStore(this, &v);
        const unsigned long long trimmed = fParkTrimmedPhys;
        fParkTrimmedPhys = 0;
        if (pl) { rd3VmParkSyncLocked(this, false); IOLockUnlock(pl); }
        rd3VmTrimmedGone(this, trimmed, "allocPhysical");
        if (t == RD3_VPT_REUSE) {
            const SInt64 n = OSAddAtomic64(1, &gCntVmUnparked);
            OSAddAtomic64(-(SInt64)fBytes, &gCntVmParkedBytes);
            rd3AccelTraceVm(this, true, true, fPhys, fBytes, 0, kIOReturnSuccess, false, kRd3VmTrPark);
            if (rd3_log_due((uint64_t)n, 8, 1023))
                ALOG("RD3VidMemory::allocPhysical(%p): %llu bytes -- the PARKED VRAM handed back, phys 0x%llx "
                     "(%llu bytes, generation %llu kept; vm_unparked %lld)", this, want, fPhys, fBytes, fAllocGen,
                     (long long)n + 1);
            return true;
        }
        if (t == RD3_VPT_GROW) {
            const SInt64 n = OSAddAtomic64(1, &gCntVmParkGrow);
            if (rd3_log_due((uint64_t)n, 8, 1023))
                ALOG("RD3VidMemory::allocPhysical(%p): %llu bytes needed, the parked VRAM (phys 0x%llx) holds %llu "
                     "-- released, a new allocation follows (vm_park_grow %lld)", this, want, rel.phys, rel.bytes,
                     (long long)n + 1);
            rd3VmRelease(this, &rel, kVmForgetAll, true, "the resource grew");
        }
    }

    struct RD3KVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = RD3K_VRAM_ABI_VERSION;
    r.size    = want;
    IOReturn ret = RDNA3Accelerator::gAccel->askFramebufferForVram(&r, true);
    if ((ret != kIOReturnSuccess || !r.handle || !r.phys) && gCntVmParkedBytes > 0) {
        /* security review of N41: VRAM ran out while parks hold some: the ones no BO holds go, then once more */
        const IOReturn first = ret;
        const unsigned long long freed = rd3VmParkTrim(true, "an allocPhysical was refused");
        if (freed) {
            for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
            r.version = RD3K_VRAM_ABI_VERSION;
            r.size    = want;
            ret = RDNA3Accelerator::gAccel->askFramebufferForVram(&r, true);
            const bool ok = ret == kIOReturnSuccess && r.handle && r.phys;
            const SInt64 n = OSAddAtomic64(1, ok ? &gCntVmRetryOk : &gCntVmRetryFail);
            if (rd3_log_due((uint64_t)n, 8, 1023))
                ALOG("RD3VidMemory::allocPhysical(%p): %llu bytes refused (0x%x), %llu parked byte(s) no BO held "
                     "given back, asked again: %s (0x%x; vm_alloc_retry_%s %lld)", this, want, (unsigned)first, freed,
                     ok ? "GRANTED" : "refused again", (unsigned)ret, ok ? "ok" : "fail", (long long)n + 1);
        }
    }
    if (ret != kIOReturnSuccess || !r.handle || !r.phys) {
        const SInt64 k = OSAddAtomic64(1, &gCntVmRefused);
        rd3AccelTraceVm(this, true, false, 0, want, 0, ret, false);
        ALOG("RD3VidMemory::allocPhysical(%p): %llu bytes REFUSED by the framebuffer (0x%x)",
             this, want, (unsigned)ret);
        /* N41: how much the parks hold when VRAM runs out (first refusals, then every 1024th) */
        if (rd3_log_due((uint64_t)k, 4, 1023))
            ALOG("VRAM refused: %lld allocations (%lld bytes), %lld given back (%lld bytes); parked now %lld "
                 "allocation(s), %lld bytes (debug.rdna3accel_vm_sticky=%d)", (long long)gCntVmAlloc,
                 (long long)gCntVmAllocBytes, (long long)gCntVmReleased, (long long)gCntVmReleasedBytes,
                 (long long)(gCntVmParked - gCntVmUnparked - gCntVmParkReleased), (long long)gCntVmParkedBytes,
                 gVmSticky);
        return false;
    }
    fRmHandle = r.handle; fKva = r.kva; fPhys = r.phys; fBytes = r.actualSize; fAllocationCookie = r.allocationCookie;
    fAllocGen = (unsigned long long)OSAddAtomic64(1, &gVmAllocGen) + 1;   /* the IOP flip's marker (N38) */
    fBacked = true;   /* free() traces the object's end (N41) */
    OSAddAtomic64(1, &gCntVmAlloc); OSAddAtomic64((SInt64)fBytes, &gCntVmAllocBytes);
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
    rd3AccelVramBase(fPhys, fAllocationCookie);
    rd3AccelTraceVm(this, true, true, fPhys, fBytes, r.mappedTotal, kIOReturnSuccess, false);
    ALOG("RD3VidMemory::allocPhysical(%p): %llu bytes of REAL VRAM -- phys 0x%llx kva %p [BAR1 %llu]",
         this, fBytes, fPhys, fKva, r.mappedTotal);
    return true;
}

void RD3VidMemory::deallocPhysical()
{
    /* security review of N41: only the allocation a GEM BO was imported over is parked (the plugin draws into
     * it); every other one is freed as before N41 (the family pages its contents itself) */
    IOLock *pl = (gVmSticky && fRmHandle && !fScanout) ? rd3LockOnce(&gVmParkLock) : nullptr;
    const bool park = fRmHandle && pl && rd3_vp_should_park(gVmSticky, fScanout ? 1 : 0, fAllocGen, fBoGen);
    if (gVmSticky && fRmHandle && !fScanout && !park) {
        const SInt64 n = OSAddAtomic64(1, &gCntVmNoImport);
        if (rd3_log_due((uint64_t)n, 4, 4095))
            ALOG("RD3VidMemory::deallocPhysical(%p): phys 0x%llx (%llu bytes, generation %llu) freed, not parked: %s "
                 "(vm_park_noimport %lld)", this, fPhys, fBytes, fAllocGen,
                 !pl ? "no lock for the park list" : !fBoGen ? "no BO was imported over it"
                                                        : "the BO was imported over an older allocation",
                 (long long)n + 1);
    }
    if (fPhys) rd3AccelTraceVm(this, false, true, fPhys, fBytes, 0, kIOReturnSuccess, fScanout,
                               park ? kRd3VmTrPark : 0u);   /* before the records go */
    if (!park) rd3AccelVidmemGone(this);   /* a parked allocation stays in the tables: still allocated (N41) */
    if (fScanout) {
        ALOG("RD3VidMemory::deallocPhysical(%p): SCANOUT -- not ours to free, only forgetting it", this);
        fPhys = 0; fBytes = 0; fScanout = false;
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
        return;
    }
    if (!fRmHandle) return;
    OSAddAtomic64(1, &gCntVmDealloc); OSAddAtomic64((SInt64)fBytes, &gCntVmDeallocBytes);
    IOLock *ll = pl ? pl : gVmParkLock;
    if (ll) IOLockLock(ll);
    struct rd3_vp v = rd3VmState(this);
    struct rd3_vp_alloc rel;
    const enum rd3_vp_drop_res d = rd3_vp_drop(&v, park ? 1 : 0, &rel);
    rd3VmStore(this, &v);
    /* ECC review round 3 of N41: what was parked, copied under the lock (v.park) and counted before the
     * unlock: a trim may release and clear fPark right after it (the parked bytes would go negative) */
    const bool parkedNow = d == RD3_VPD_PARKED || d == RD3_VPD_REPARKED;
    if (parkedNow) OSAddAtomic64((SInt64)v.park.bytes, &gCntVmParkedBytes);
    if (ll) { rd3VmParkSyncLocked(this, parkedNow); IOLockUnlock(ll); }
    if (parkedNow) {
        const SInt64 n = OSAddAtomic64(1, &gCntVmParked);
        if (rd3_log_due((uint64_t)n, 8, 1023))
            ALOG("RD3VidMemory::deallocPhysical(%p): phys 0x%llx (%llu bytes, generation %llu) PARKED, not freed: "
                 "the next allocPhysical hands it back (vm_parked %lld; debug.rdna3accel_vm_sticky=0 frees)", this,
                 v.park.phys, v.park.bytes, v.park.gen, (long long)n + 1);
    }
    if (d == RD3_VPD_REPARKED) {   /* never expected: allocPhysical empties the park before anything is live */
        const SInt64 n = OSAddAtomic64(1, &gCntVmReparked);
        if (rd3_log_due((uint64_t)n, 4, 1023))
            ALOG("RD3VidMemory::deallocPhysical(%p): an older park (phys 0x%llx) was still held -- released "
                 "(vm_reparked %lld, never expected)", this, rel.phys, (long long)n + 1);
        rd3VmRelease(this, &rel, kVmForgetAddr, true, "replaced by a newer park");
    } else if (d == RD3_VPD_FREED) {
        rd3VmRelease(this, &rel, kVmForgetNone, false, "deallocPhysical");
    }
    if (parkedNow) (void)rd3VmParkTrim(false, "a park");   /* the cap */
}

/* N41: what this object still holds goes with it. The live part counts as vm_free_live: the family freed
 * the object without deallocPhysical (whether IOAccelVidMemory::free calls it is not known; before N41
 * that allocation was never given back). Every VMFREE traced here carries RD3_TRVM_OBJ_GONE (one with no
 * VRAM when the object held none but was backed once): the trace decoder forgets the object, whose
 * address a new vidmem may get (ECC review of N41, H-VMMOVE).
 * The base call: -fapple-kext compiles IOAccelVidMemory::free() into a call through IOAccelVidMemory's
 * exported vtable (__ZTV16IOAccelVidMemory) at OSObject::free's slot, i.e. Apple's own free of that class
 * (whatever IOAccelVidMemory / IOAccelMemory override), the same function the family's vtable patching put
 * into this class's slot before free() was overridden here; tools/check_accel_slots.py checks the
 * compiled call (ECC review of N41). */
void RD3VidMemory::free()
{
    if (!fScanout) {
        IOLock *pl = gVmParkLock;   /* off the park list before the object goes (rd3VmParkTrim) */
        if (pl) IOLockLock(pl);
        struct rd3_vp v = rd3VmState(this);
        struct rd3_vp_alloc rel[2];
        const unsigned m = rd3_vp_free(&v, rel);
        rd3VmStore(this, &v);
        const unsigned long long trimmed = fParkTrimmedPhys;
        fParkTrimmedPhys = 0;
        if (pl) { rd3VmParkUnlinkLocked(this); IOLockUnlock(pl); }
        rd3VmTrimmedGone(this, trimmed, "free");
        if (m & 1u) {
            const SInt64 n = OSAddAtomic64(1, &gCntVmFreeLive);
            if (rd3_log_due((uint64_t)n, 8, 1023))
                ALOG("RD3VidMemory::free(%p): VRAM phys 0x%llx (%llu bytes) still LIVE -- the family freed the object "
                     "without deallocPhysical; released here (vm_free_live %lld)", this, rel[0].phys, rel[0].bytes,
                     (long long)n + 1);
            rd3AccelTraceVm(this, false, true, rel[0].phys, rel[0].bytes, 0, kIOReturnSuccess, false,
                            kRd3VmTrFreeLive | kRd3VmTrObjGone);
            rd3VmRelease(this, &rel[0], kVmForgetAll, false, "the object was freed");
        }
        if (m & 2u) rd3VmRelease(this, &rel[1], kVmForgetAll, true, "the object was freed", kRd3VmTrObjGone);
        if (!m && fBacked) rd3AccelTraceVm(this, false, true, 0, 0, 0, kIOReturnSuccess, false, kRd3VmTrObjGone);
    }
    IOAccelVidMemory::free();
}

class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
    /* N59: the seeds a write lock / unlock moves (IOSurfaceLock without kIOSurfaceLockReadOnly) */
    unsigned int        getLockSeed() const;
    unsigned int        getUnlockSeed() const;
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
    /* the present queue (N38): complete once this pipe's queued and in-flight flips latched */
    return gRd3Iop != 0 && !rd3AccelIopAsyncBusy(this) && !rd3AccelPresentIopBusy(this);
}

static bool rd3AccelSurfCopyReady(IOSurface *s, unsigned head);
static void rd3AccelIopStaleGate(IOSurface *s, unsigned head, unsigned *lseed, unsigned *useed);
static void rd3AccelIopGateNote(unsigned head, uint64_t t0);   /* N61 */
static void rd3AccelIopCopyNote(unsigned head, uint64_t t0, uint64_t bytes, int accessBytes, unsigned w, unsigned h,
                                unsigned spitch, unsigned dpitch, bool fromRes);
static void rd3AccelIopStaleNote(IOSurface *s, unsigned head, unsigned lseed, unsigned useed);
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
    /* N41 security review: whose marks are taken - only a transaction that drives one of our heads (round 3) */
    rd3AccelSurfNoteDisplayClient("display-pipe transaction");

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
    unsigned staleL = 0, staleU = 0;
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
        /* N59: a buffer of WindowServer's present ring nothing wrote since this head last copied it holds an
         * older frame (the plugin's copy-out of the new one is still on its way): wait for it, bounded */
        const uint64_t gate0 = mach_absolute_time();
        rd3AccelIopStaleGate(s, head, &staleL, &staleU);
        const bool copyReady = rd3AccelSurfCopyReady(s, head);
        rd3AccelIopGateNote(head, gate0);   /* N61: what the gates cost this transaction */
        if (!copyReady) return kIOReturnSuccess;
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

    /* rdna3_iop_copy.h steps rows by bytes; the old loop stepped by pitch / 4 words: the same rows only for pitches of whole words
     * (the surface's is checked above, the VRAM resource's and the scanout's only >= 4: cpp-reviewer of 42c31bb) */
    if ((pitch & 3u) || (q.pitch & 3u))
        RD3IOP_FAIL(kIOReturnBadArgument, "source pitch %u or scanout pitch %u is not a whole number of 32-bit words", pitch, q.pitch);
    rd3AccelIopHome(fb, head);
    const unsigned rowBytes = ((pitch < q.pitch) ? pitch : q.pitch) & ~3u;
    const unsigned spr = pitch / 4;
    /* N61 (common/rdna3_iop_copy.h): the same bytes as the old 32-bit loop, 8 at a time when everything is 8-aligned; timed */
    const uint64_t copy0 = mach_absolute_time();
    const int accessBytes = rd3_iop_copy_rows(q.kva, q.pitch, src, pitch, h, rowBytes);
    rd3AccelIopCopyNote(head, copy0, (uint64_t)rowBytes * h, accessBytes, w, h, pitch, q.pitch, fromRes);
    if (!fromRes && s) rd3AccelIopStaleNote(s, head, staleL, staleU);
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
RD3ACCEL_CNT(rdna3accel_vm_alloc,         gCntVmAlloc,        "RD3VidMemory::allocPhysical new allocations (a parked one handed back counts as vm_unparked, N41)");
RD3ACCEL_CNT(rdna3accel_vm_alloc_bytes,   gCntVmAllocBytes,   "bytes of VRAM granted to RD3VidMemory");
RD3ACCEL_CNT(rdna3accel_vm_dealloc,       gCntVmDealloc,      "RD3VidMemory::deallocPhysical calls with a handle (parks included, N41: vm_released counts the real releases)");
RD3ACCEL_CNT(rdna3accel_vm_dealloc_bytes, gCntVmDeallocBytes, "bytes those deallocPhysical calls dropped (parked or freed; N41)");
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
/* N61 (common/rdna3_iop_copy.h): the display pipe's CPU copy into the scanout, and the gates before it */
static SInt64 gCntIopCopyN, gCntIopCopyBytes, gCntIopCopyUs, gCntIopCopyMaxUs, gCntIopCopy64, gCntIopGateUs;
static SInt64 gCntIopCopyNHead[kRd3MaxFB], gCntIopCopyUsHead[kRd3MaxFB], gCntIopGateUsHead[kRd3MaxFB];
RD3ACCEL_CNT(rdna3accel_iop_copy_n,       gCntIopCopyN,       "N61: IOP copies into a scanout (all heads)");
RD3ACCEL_CNT(rdna3accel_iop_copy_bytes,   gCntIopCopyBytes,   "N61: bytes the IOP copies moved");
RD3ACCEL_CNT(rdna3accel_iop_copy_us,      gCntIopCopyUs,      "N61: microseconds the IOP copies took (all heads)");
RD3ACCEL_CNT(rdna3accel_iop_copy_max_us,  gCntIopCopyMaxUs,   "N61: the longest IOP copy, microseconds");
RD3ACCEL_CNT(rdna3accel_iop_copy_64,      gCntIopCopy64,      "N61: IOP copies done 8 bytes per access (everything 8-aligned)");
RD3ACCEL_CNT(rdna3accel_iop_gate_us,      gCntIopGateUs,      "N61: microseconds transactions spent in the stale / page-off gates before their copy");
RD3ACCEL_CNT(rdna3accel_iop_copy_n_h0,    gCntIopCopyNHead[0], "N61: IOP copies into head 0");
RD3ACCEL_CNT(rdna3accel_iop_copy_n_h1,    gCntIopCopyNHead[1], "N61: IOP copies into head 1");
RD3ACCEL_CNT(rdna3accel_iop_copy_n_h2,    gCntIopCopyNHead[2], "N61: IOP copies into head 2");
RD3ACCEL_CNT(rdna3accel_iop_copy_n_h3,    gCntIopCopyNHead[3], "N61: IOP copies into head 3");
RD3ACCEL_CNT(rdna3accel_iop_copy_us_h0,   gCntIopCopyUsHead[0], "N61: microseconds of IOP copies into head 0");
RD3ACCEL_CNT(rdna3accel_iop_copy_us_h1,   gCntIopCopyUsHead[1], "N61: microseconds of IOP copies into head 1");
RD3ACCEL_CNT(rdna3accel_iop_copy_us_h2,   gCntIopCopyUsHead[2], "N61: microseconds of IOP copies into head 2");
RD3ACCEL_CNT(rdna3accel_iop_copy_us_h3,   gCntIopCopyUsHead[3], "N61: microseconds of IOP copies into head 3");
RD3ACCEL_CNT(rdna3accel_iop_gate_us_h0,   gCntIopGateUsHead[0], "N61: gate microseconds of head 0's transactions");
RD3ACCEL_CNT(rdna3accel_iop_gate_us_h1,   gCntIopGateUsHead[1], "N61: gate microseconds of head 1's transactions");
RD3ACCEL_CNT(rdna3accel_iop_gate_us_h2,   gCntIopGateUsHead[2], "N61: gate microseconds of head 2's transactions");
RD3ACCEL_CNT(rdna3accel_iop_gate_us_h3,   gCntIopGateUsHead[3], "N61: gate microseconds of head 3's transactions");
/* N59 (common/rdna3_iop_stale.h): IOP copies of a buffer whose IOSurface seeds did not move since the head last copied it */
static int gIopStaleWaitMaxUs;
static SInt64 gCntIopStaleChecked, gCntIopStale, gCntIopStaleMoved, gCntIopStaleTimeout, gCntIopStaleNotWaited, gCntIopStaleWaitUs,
              gCntIopStaleTorn, gCntIopStaleFresh, gCntIopStaleCapped;
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_iop_stale_wait_ms, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopStaleWaitMs, 0,
           "N59: an IOP copy of a buffer nothing wrote since the head last showed it waits up to this many ms for the write (0 = count only; boot-arg rdna3iopstalewait)");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_iop_stale_wait_maxus, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopStaleWaitMaxUs, 0, "N59: the longest stale wait, us");
RD3ACCEL_CNT(rdna3accel_iop_stale_checked, gCntIopStaleChecked, "N59: IOP copies whose buffer this head had copied before (seeds compared)");
RD3ACCEL_CNT(rdna3accel_iop_stale_fresh,   gCntIopStaleFresh,   "N59: of them with seeds that moved since (a new frame written)");
RD3ACCEL_CNT(rdna3accel_iop_stale,         gCntIopStale,        "N59: of them STALE: seeds unchanged since the head last copied it (an old frame, unless the wait saw the write)");
RD3ACCEL_CNT(rdna3accel_iop_stale_moved,   gCntIopStaleMoved,   "N59: stale copies whose wait saw the seeds move (the write came late: the step back avoided)");
RD3ACCEL_CNT(rdna3accel_iop_stale_timeout, gCntIopStaleTimeout, "N59: stale copies whose wait ended with no write (a re-present of an unchanged buffer, or a slower write)");
RD3ACCEL_CNT(rdna3accel_iop_stale_nowait,  gCntIopStaleNotWaited, "N59: stale copies not waited for (wait_ms 0, or the head's cap of timed-out waits for this second spent)");
RD3ACCEL_CNT(rdna3accel_iop_stale_capped,  gCntIopStaleCapped,  "N59: one-second windows in which a head's cap of timed-out waits stopped the waits");
RD3ACCEL_CNT(rdna3accel_iop_stale_waitus,  gCntIopStaleWaitUs,  "N59: time the stale waits took, us");
RD3ACCEL_CNT(rdna3accel_iop_stale_torn,    gCntIopStaleTorn,    "N59: copies during which the buffer's seeds moved (a write raced the copy: a torn frame)");
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
/* card run 58b: the swap and copy forward outcomes, per head (rdna3_swap_front.h) */
#define RD3ACCEL_CNT4(nm, arr, desc) \
    RD3ACCEL_CNT(nm##_h0, arr[0], desc " (head 0)"); RD3ACCEL_CNT(nm##_h1, arr[1], desc " (head 1)"); \
    RD3ACCEL_CNT(nm##_h2, arr[2], desc " (head 2)"); RD3ACCEL_CNT(nm##_h3, arr[3], desc " (head 3)")
#define RD3ACCEL_REG4(nm) do { \
    sysctl_register_oid(&sysctl__debug_##nm##_h0); sysctl_register_oid(&sysctl__debug_##nm##_h1); \
    sysctl_register_oid(&sysctl__debug_##nm##_h2); sysctl_register_oid(&sysctl__debug_##nm##_h3); } while (0)
RD3ACCEL_CNT4(rdna3accel_swaps_skipped, gCntSwapSkipHead, "swaps skipped: the back read empty (the front named this head)");
RD3ACCEL_CNT4(rdna3accel_swaps_frontskip, gCntSwapFrontSkip, "of those, the front had pixels the back did not (it never goes to the panel)");
RD3ACCEL_CNT4(rdna3accel_swaps_emptyshown, gCntSwapEmptyShown, "empty backs shown after 3 swaps of them in a row (a black or clear frame)");
RD3ACCEL_CNT4(rdna3accel_swaps_bufchg, gCntSwapBufChg, "swaps that showed another buffer than the swap before");
RD3ACCEL_CNT4(rdna3accel_bufs_new, gCntBufNew, "buffers the head showed for the first time (cumulative)");
RD3ACCEL_CNT4(rdna3accel_cf_shown, gCntCfShown, "copy forwards from the surface's buffer the head shows, every rect copied");
RD3ACCEL_CNT4(rdna3accel_cf_src, gCntCfSrc, "copy forwards from the front WindowServer named (it was shown last)");
RD3ACCEL_CNT4(rdna3accel_cf_backlast, gCntCfBackLast, "copy forwards into the buffer the head shows: nothing to copy (one back)");
RD3ACCEL_CNT4(rdna3accel_cf_nothingshown, gCntCfNothingShown, "copy forwards before the head showed anything recorded: nothing copied");
RD3ACCEL_CNT4(rdna3accel_cf_foreign, gCntCfForeign, "copy forwards while the head shows another surface's buffer: nothing copied");
RD3ACCEL_CNT4(rdna3accel_cf_miss, gCntCfMiss, "copy forwards that left rects stale (SDMA refused, too big for the CPU): pending in that buffer");
RD3ACCEL_CNT4(rdna3accel_cf_healed, gCntCfHealed, "copy forwards that also copied rects their target had missed before");
RD3ACCEL_CNT(rdna3accel_cf_pend_blocked, gCntCfPendBlocked, "card run 58b: copy forwards whose source had missed the target's pending rects too (they wait)");
RD3ACCEL_CNT(rdna3accel_cf_pend_forced, gCntCfPendForced, "card run 58b: pending rects copied anyway after 3 blocked calls (both buffers alike there)");
RD3ACCEL_CNT(rdna3accel_cf_pend_evicted, gCntCfPendEvicted, "card run 58b: pending-rect lists dropped from a full table (those rects may stay stale)");
RD3ACCEL_CNT(rdna3accel_cf_nohead_miss, gCntCfNoHeadMiss, "card run 58b: copy forwards with no head known that left rects stale (nowhere to keep them)");
RD3ACCEL_CNT(rdna3accel_cf_pend_whole, gCntCfPendWhole, "card run 58b: pending-rect lists that overflowed into the whole picture");
RD3ACCEL_CNT(rdna3accel_swaps_skipped_all, gCntSwapSkipped, "card run 58b: legacy swaps skipped, every head and reason (nothing drawn)");
RD3ACCEL_CNT(rdna3accel_swaps_nohead, gCntSwapNoHead, "card run 58b: legacy swaps that matched no head (not shown)");
RD3ACCEL_CNT(rdna3accel_cf_calls, gCntCfCalls, "card run 58b: copy forward calls");
RD3ACCEL_CNT(rdna3accel_cf_nohead, gCntCfNoHead, "card run 58b: copy forwards with no head known and a front never shown: nothing copied");
RD3ACCEL_CNT(rdna3accel_cf_shown_stale, gCntCfShownStale, "card run 58b: copy forwards whose shown buffer was not usable (changed, not copyable): nothing copied");
RD3ACCEL_CNT(rdna3accel_cf_refused, gCntCfRefused, "card run 58b: copy forwards refused to CoreDisplay (target or front not copyable, no rects)");
RD3ACCEL_CNT(rdna3accel_cf_same, gCntCfSame, "card run 58b: copy forwards whose front and target are one buffer");
RD3ACCEL_CNT(rdna3accel_cf_disabled, gCntCfDisabled, "card run 58b: copy forwards refused because rdna3accelcf=0");
/* card run 2026-10-17: the flip latch and the buffers the panel showed (rdna3_flip_latch.h) */
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_swap_latch, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gSwapLatch, 0,
           "card run 2026-10-17: 1 (default, boot-arg rdna3swaplatch) = a legacy swap returns once its flip latched; "
           "0 = at once (the old buffer may still be on the panel)");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_swap_torn, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gSwapTorn, 0,
           "card run 2026-10-17: the test of the buffers the panel showed - 0 off, 1 (default) the first 20000 checks "
           "per head, 2 always");
RD3ACCEL_CNT4(rdna3accel_swaps_latchpend, gCntLatchPend, "swaps whose entry found the previous flip not latched (the panel still read the buffer before it)");
RD3ACCEL_CNT4(rdna3accel_swaps_latchwait, gCntLatchWait, "flips seen latching while their swap waited after the flip (the only wait; the next swap's entry only looks)");
RD3ACCEL_CNT4(rdna3accel_swaps_latchus, gCntLatchUs, "microseconds from those flips to their latch, summed (average = latchus / latchwait)");
RD3ACCEL_CNT4(rdna3accel_swaps_latchmax, gCntLatchMax, "the longest flip-to-latch time seen, microseconds");
RD3ACCEL_CNT4(rdna3accel_swaps_latchtimeout, gCntLatchTimeout, "flips still pending 2 OTG frames after them, or 2.5 of the head's frames (35-100 ms) with the frame counter stopped");
RD3ACCEL_CNT4(rdna3accel_swaps_latchunknown, gCntLatchUnknown, "swaps whose flip state could not be read (no display status)");
RD3ACCEL_CNT4(rdna3accel_swaps_latchoff, gCntLatchOff, "flips not waited for with debug.rdna3accel_swap_latch on: the head's waits off (after 3 timeouts in a row)");
RD3ACCEL_CNT4(rdna3accel_swaps_tornchk, gCntTornChk, "shown buffers hashed again at the head's next swap (the torn test's denominator)");
RD3ACCEL_CNT4(rdna3accel_swaps_torn, gCntTorn, "buffers that changed while on the panel, then flipped away from (written while shown)");
RD3ACCEL_CNT4(rdna3accel_swaps_inplace, gCntInplace, "buffers rewritten while on the panel and shown again (one back buffer)");
RD3ACCEL_CNT4(rdna3accel_swaps_tornskip, gCntTornSkip, "shown buffers the torn test could not hash again (not the surface's own any more, re-allocated)");
RD3ACCEL_CNT4(rdna3accel_cf_onpanel, gCntCfOnPanel, "copy forwards into the buffer the panel still read (the flip away from it had not latched)");
RD3ACCEL_CNT4(rdna3accel_swaps_shownfreed, gCntShownFreed, "buffers freed while the head scanned them (shown, or the flip away not latched)");
RD3ACCEL_CNT4(rdna3accel_swaps_emptyfilled, gCntEmptyFilled, "sampled empty backs of skipped swaps that had pixels when read again 4 ms later (sampled only with swap_latch on)");
RD3ACCEL_CNT4(rdna3accel_swaps_emptystill, gCntEmptyStill, "sampled empty backs of skipped swaps still empty when read again 4 ms later");
RD3ACCEL_CNT4(rdna3accel_swaps_latchimmediate, gCntLatchImmediate, "flips already latched at the first look after them (not waited, not timed)");
RD3ACCEL_CNT4(rdna3accel_swaps_latchwaitsoff, gCntLatchWaitsOff, "1 = the head's swaps do not wait for their flips now (3 timeouts in a row; until a flip is seen latched)");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_swap_samenoflip, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gSwapSameNoFlip, 0,
           "card run 2026-10-18b: 1 (default, boot-arg rdna3swapsamenoflip) = a mode 0 swap to the buffer the panel "
           "already scans, its flip latched, is not flipped again and not waited for; 0 = flipped and waited as before");
RD3ACCEL_CNT4(rdna3accel_swaps_latchsame, gCntLatchSame, "mode 0 swaps whose back was the buffer the panel already scanned (its flip latched): not flipped again, no latch wait");
RD3ACCEL_CNT4(rdna3accel_swaps_flipfailed, gCntFlipFailed, "swaps whose flip the framebuffer refused (copied onto the scanout, or nothing)");
/* like every OID here these are registered by rdna3accelInstallSysctl and
 * never unregistered (the kext does not unload: _stop does nothing) */
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
    /* N61 */
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_n);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_bytes);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_us);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_max_us);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_64);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_gate_us);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_n_h0);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_n_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_n_h2);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_n_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_us_h0);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_us_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_us_h2);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_copy_us_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_gate_us_h0);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_gate_us_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_gate_us_h2);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_gate_us_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_wait_ms); sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_wait_maxus);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_checked); sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_fresh);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale);   sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_moved);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_timeout); sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_nowait);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_capped);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_waitus); sysctl_register_oid(&sysctl__debug_rdna3accel_iop_stale_torn);
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
    RD3ACCEL_REG4(rdna3accel_swaps_skipped);  RD3ACCEL_REG4(rdna3accel_swaps_frontskip);
    RD3ACCEL_REG4(rdna3accel_swaps_emptyshown); RD3ACCEL_REG4(rdna3accel_swaps_bufchg);
    RD3ACCEL_REG4(rdna3accel_bufs_new);       RD3ACCEL_REG4(rdna3accel_cf_shown);
    RD3ACCEL_REG4(rdna3accel_cf_src);         RD3ACCEL_REG4(rdna3accel_cf_backlast);
    RD3ACCEL_REG4(rdna3accel_cf_nothingshown); RD3ACCEL_REG4(rdna3accel_cf_foreign);
    RD3ACCEL_REG4(rdna3accel_cf_miss);        RD3ACCEL_REG4(rdna3accel_cf_healed);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_skipped_all); sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_nohead);
    sysctl_register_oid(&sysctl__debug_rdna3accel_cf_calls);   sysctl_register_oid(&sysctl__debug_rdna3accel_cf_nohead);
    sysctl_register_oid(&sysctl__debug_rdna3accel_cf_shown_stale); sysctl_register_oid(&sysctl__debug_rdna3accel_cf_refused);
    sysctl_register_oid(&sysctl__debug_rdna3accel_cf_same);    sysctl_register_oid(&sysctl__debug_rdna3accel_cf_disabled);
    sysctl_register_oid(&sysctl__debug_rdna3accel_cf_pend_blocked); sysctl_register_oid(&sysctl__debug_rdna3accel_cf_pend_forced);
    sysctl_register_oid(&sysctl__debug_rdna3accel_cf_pend_evicted); sysctl_register_oid(&sysctl__debug_rdna3accel_cf_pend_whole);
    sysctl_register_oid(&sysctl__debug_rdna3accel_cf_nohead_miss);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_latch);  sysctl_register_oid(&sysctl__debug_rdna3accel_swap_torn);
    RD3ACCEL_REG4(rdna3accel_swaps_latchpend);  RD3ACCEL_REG4(rdna3accel_swaps_latchwait);
    RD3ACCEL_REG4(rdna3accel_swaps_latchus);    RD3ACCEL_REG4(rdna3accel_swaps_latchmax);
    RD3ACCEL_REG4(rdna3accel_swaps_latchtimeout); RD3ACCEL_REG4(rdna3accel_swaps_latchunknown);
    RD3ACCEL_REG4(rdna3accel_swaps_latchoff);   RD3ACCEL_REG4(rdna3accel_swaps_tornchk);
    RD3ACCEL_REG4(rdna3accel_swaps_torn);       RD3ACCEL_REG4(rdna3accel_swaps_inplace);
    RD3ACCEL_REG4(rdna3accel_swaps_tornskip);   RD3ACCEL_REG4(rdna3accel_cf_onpanel);
    RD3ACCEL_REG4(rdna3accel_swaps_shownfreed); RD3ACCEL_REG4(rdna3accel_swaps_emptyfilled);
    RD3ACCEL_REG4(rdna3accel_swaps_emptystill);
    RD3ACCEL_REG4(rdna3accel_swaps_latchimmediate); RD3ACCEL_REG4(rdna3accel_swaps_latchwaitsoff);
    RD3ACCEL_REG4(rdna3accel_swaps_flipfailed);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_samenoflip); RD3ACCEL_REG4(rdna3accel_swaps_latchsame);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_h0);       sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_h2);       sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_h3);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_a3_h0);    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_a3_h1);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_a3_h2);    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_a3_h3);
    rd3AccelInstallSysctl2();   /* card run 2026-10-17b: the trace, the swap mode, the copy counters */
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
/* ---- DRIVER-PLAN N41: WindowServer's pending GPU writes (common/rdna3_surf_pending.h) ----
 * A normal IOSurfaceLock waits for GPU work in flight on the surface and WindowServer relies on it (a
 * window capture locks a surface it is still drawing). The Metal plugin marked VRAM newest only after
 * the GPU finished, so a lock that came first read the stale / zero system copy: black captures, stale
 * panes. WindowServer's plugin marks the surfaces a command buffer
 * writes pending at commit (RD3K_SS_PLANE_GPU_MARK | 1), marks them newest then, and ready (GPU_MARK |
 * 0) once the GPU finished the last part writing each; the page-off (and the IOP zero-copy flip) waits
 * here, bounded, while a surface is pending. Only WindowServer's marks are taken, and its pending marks
 * only for surfaces it can look up (no other process can hold its page-offs). The plugin learns the mode
 * from the answer to its pending marks (0 taken; ENXIO off; ENOSYS / ENODEV an older kext or
 * accelerator). NullMoth comparison: the behaviour of NullMoth's 79f47d0 / 40daaa6 / e75be1e
 * (nvSurfMark / nvSurfWaitReady), written for this driver; marks counted per surface across command
 * buffers, a late ready after a timeout never takes a newer write's count (rd3_sp forgiven marks).
 * debug.rdna3accel_surface_wait / boot-arg rdna3surfwait (default 1; 0: marks
 * answered RD3K_SS_OFF, nothing waits, the plugin keeps its old order), _surface_wait_ms (100, 1..1000) */
static int gSurfWaitMaxUs, gSurfIopWaitMaxUs;
static struct rd3_sp gSurfPend;
static IOLock *volatile gSurfPendLock;
static volatile SInt64 gCntSurfMarkPend, gCntSurfMarkReady, gCntSurfMarkLate, gCntSurfMarkEvict, gCntSurfMarkSat,
                       gCntSurfMarkOff, gCntSurfMarkNotWs, gCntSurfMarkNotOwn, gCntSurfWaits, gCntSurfTimeouts,
                       gCntSurfWaitUs, gCntSurfCleared, gCntSurfIopWaits, gCntSurfIopTimeouts, gCntSurfIopCleared,
                       gCntSurfIopWaitUs, gCntDirtyTryBusy, gCntSurfMarkNotReal, gCntSurfMarkMoot, gCntSurfSkipped;
/* security review of N41: the display client - the pid of the process named WindowServer whose legacy swaps
 * (rd3AccelSwapToPanel) or display-pipe transactions (RD3DisplayPipe::performTransaction) drive the panel; 0
 * none yet. With _windowserver's uid it decides whose GPU marks are taken (rd3_sp_mark_gate). The pid and uid
 * of the first mark taken, for the report (debug.rdna3accel_surface_wait_ws_pid / _mark_pid / _mark_uid) */
static volatile SInt32 gSurfWsPid;
static int gSurfMarkPid, gSurfMarkUid = -1;
/* ECC review round 3 of N41: the first display client is sticky while it lives (rd3_sp_display_client): it
 * is replaced only when no process has its pid any more, or that process is no longer named WindowServer;
 * a second WindowServer meanwhile is refused and counted (debug.rdna3accel_surface_wait_ws_pid_refused).
 * Called only once the swap / transaction was validated (it drives one of our heads / names our VRAM),
 * never with a spinlock held (proc_name takes the proc list lock) */
static volatile SInt64 gCntSurfWsPidRefused, gCntSurfWsPidSet;
static void rd3AccelSurfNoteDisplayClient(const char *path)
{
    const int pid = proc_selfpid();
    const SInt32 was = gSurfWsPid;
    if (pid <= 0 || pid == (int)was) return;
    char nm[32];
    nm[0] = 0;
    proc_selfname(nm, (int)sizeof nm);
    if (strcmp(nm, "WindowServer") != 0) return;
    /* proc_name finds the process (proc_find / proc_rele inside) and leaves the buffer empty when none has the
     * pid; a pid reused by another program is no longer WindowServer */
    char wnm[32];
    wnm[0] = 0;
    if (was > 0) proc_name((int)was, wnm, (int)sizeof wnm);
    const bool alive = was > 0 && strcmp(wnm, "WindowServer") == 0;
    const unsigned uid = (unsigned)kauth_cred_getuid(kauth_cred_get());
    switch (rd3_sp_display_client((int)was, pid, alive ? 1 : 0)) {
    case RD3_SPDC_KEEP: return;
    case RD3_SPDC_REFUSE: {
        const SInt64 n = OSAddAtomic64(1, &gCntSurfWsPidRefused);
        if (rd3_log_due((uint64_t)n, 8, 1023))
            ALOG("surface wait: a second process named WindowServer, pid %d uid %u, drives the panel (its %s) while "
                 "the display client pid %d lives: REFUSED as the display client (its marks are taken only with "
                 "_windowserver's uid %u; surface_wait_ws_pid_refused %lld)", pid, uid, path, (int)was,
                 RD3_SP_WS_UID, (long long)n + 1);
        return;
    }
    case RD3_SPDC_TAKE: break;
    }
    if (!OSCompareAndSwap((UInt32)was, (UInt32)pid, (volatile UInt32 *)&gSurfWsPid)) return;   /* another won */
    const SInt64 k = OSAddAtomic64(1, &gCntSurfWsPidSet);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("surface wait: WindowServer's display client is pid %d, uid %u (%s; pid before %d%s; seen in its %s): "
             "its GPU marks are taken (display client #%lld)", pid, uid,
             uid == RD3_SP_WS_UID ? "_windowserver" : "NOT _windowserver's 88: marks are taken from this pid only",
             (int)was, was > 0 ? ", no longer running" : "", path, (long long)k + 1);
}
static void *rd3AccelSurfaceRoot(void);
static unsigned rd3AccelSurfGpuMark(unsigned id, bool pending, task_t task)
{
    char nm[32];
    nm[0] = 0;
    if (gSurfWait) proc_selfname(nm, (int)sizeof nm);
    const int pid = proc_selfpid(), wsPid = gSurfWsPid;
    const unsigned uid = (unsigned)kauth_cred_getuid(kauth_cred_get());
    switch (rd3_sp_mark_gate(gSurfWait, pending ? 1 : 0, strcmp(nm, "WindowServer") == 0, uid, pid, wsPid,
                             gA3Ws || gA3Display || gIopFlip)) {
    case RD3_SPG_OFF: OSAddAtomic64(1, &gCntSurfMarkOff); return RD3K_SS_OFF;
    case RD3_SPG_NOTWS: {
        const SInt64 n = OSAddAtomic64(1, &gCntSurfMarkNotWs);
        if (rd3_log_due((uint64_t)n, 4, 1023))
            ALOG("surface wait: a GPU %s mark of surface %u from '%s' REFUSED: only WindowServer's are taken "
                 "(surface_wait_notws %lld)", pending ? "pending" : "ready", id, nm[0] ? nm : "?", (long long)n + 1);
        return RD3K_SS_OFF;
    }
    case RD3_SPG_NOTREAL: {   /* security review of N41: a name is not proof */
        const SInt64 n = OSAddAtomic64(1, &gCntSurfMarkNotReal);
        if (rd3_log_due((uint64_t)n, 4, 1023))
            ALOG("surface wait: a GPU %s mark of surface %u from a process named WindowServer REFUSED: pid %d uid %u "
                 "is neither _windowserver (uid %u) nor the display client (pid %d%s) (surface_wait_notreal %lld)",
                 pending ? "pending" : "ready", id, pid, uid, RD3_SP_WS_UID, wsPid,
                 wsPid ? "" : ": no legacy swap / display-pipe transaction seen yet", (long long)n + 1);
        return RD3K_SS_OFF;
    }
    case RD3_SPG_MOOT: {   /* the plugin backs off (ENXIO) as for the switch-off */
        const SInt64 n = OSAddAtomic64(1, &gCntSurfMarkMoot);
        if (rd3_log_due((uint64_t)n, 2, 4095))
            ALOG("surface wait: WindowServer's pending mark of surface %u REFUSED: MOOT - rdna3a3ws=0, "
                 "rdna3a3display=0 and no IOP flip keep its surfaces out of VRAM, no page-off waits for them "
                 "(surface_wait_moot %lld)", id, (long long)n + 1);
        return RD3K_SS_OFF;
    }
    default: break;
    }
    if (gSurfMarkUid < 0) {
        gSurfMarkPid = pid; gSurfMarkUid = (int)uid;
        ALOG("surface wait: the first GPU mark taken: WindowServer pid %d uid %u (%s; display client pid %d)", pid, uid,
             uid == RD3_SP_WS_UID ? "_windowserver" : "not uid 88: taken as the display client", wsPid);
    }
    if (!id) return RD3K_SS_NO_SURFACE;
    /* ECC review of N41 (M2): the process name alone is not proof. A pending mark is taken only for a surface
     * the calling task can look up (its own or a global one), so no other process can hold WindowServer's
     * page-offs by naming its surfaces. No family lock here: a page-off waiting for a mark holds it. Only the
     * pending mark: a ready only takes a count away (at worst a page-off copies early, as before N41), and a
     * surface released before its ready must still lose its marks */
    if (pending) {
        void *root = rd3AccelSurfaceRoot();
        void *surf = (root && task) ? _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(root, id, task) : nullptr;
        if (!surf) {
            const SInt64 n = OSAddAtomic64(1, &gCntSurfMarkNotOwn);
            if (rd3_log_due((uint64_t)n, 4, 1023))
                ALOG("surface wait: WindowServer's pending mark of surface %u REFUSED: %s (surface_wait_notown %lld)",
                     id, !root ? "no IOSurfaceRoot" : !task ? "no task" : "not a surface of the calling task",
                     (long long)n + 1);
            return RD3K_SS_NO_SURFACE;
        }
        ((OSObject *)surf)->release();
    }
    IOLock *l = rd3LockOnce(&gSurfPendLock);
    if (!l) {
        const SInt64 n = OSAddAtomic64(1, &gCntSurfMarkOff);
        if (rd3_log_due((uint64_t)n, 4, 1023))
            ALOG("surface wait: no lock for the pending table -- mark of surface %u REFUSED (surface_wait_off %lld)", id,
                 (long long)n + 1);
        return RD3K_SS_OFF;
    }
    uint32_t ev = 0;
    IOLockLock(l);
    const uint32_t starved = gSurfPend.starved;
    const enum rd3_sp_mark_res r = rd3_sp_mark(&gSurfPend, id, pending ? 1 : 0, mach_absolute_time(), &ev);
    if (r == RD3_SPM_READY || r == RD3_SPM_EVICTED) IOLockWakeup(l, &gSurfPend, false);
    const bool resumed = starved && !gSurfPend.starved;
    IOLockUnlock(l);
    if (resumed)
        ALOG("surface wait: a ready mark of surface %u came: page-offs WAIT again for WindowServer's pending GPU "
             "writes (surface_wait_skipped %lld so far)", id, (long long)gCntSurfSkipped);
    switch (r) {
    case RD3_SPM_PENDING: OSAddAtomic64(1, &gCntSurfMarkPend); return 0;
    case RD3_SPM_STILL:
    case RD3_SPM_READY: OSAddAtomic64(1, &gCntSurfMarkReady); return 0;
    case RD3_SPM_EVICTED: {
        OSAddAtomic64(1, &gCntSurfMarkPend);
        const SInt64 n = OSAddAtomic64(1, &gCntSurfMarkEvict);
        if (rd3_log_due((uint64_t)n, 4, 1023))
            ALOG("surface wait: %u surfaces pending at once -- surface %u's marks dropped for surface %u (its "
                 "page-offs stop waiting; its late readies are remembered, forgiven if it is marked again: "
                 "surface_wait_evict_kept; surface_wait_evicted %lld)", RD3_SP_SLOTS, ev, id, (long long)n + 1);
        return 0;
    }
    case RD3_SPM_SATURATED: {
        const SInt64 n = OSAddAtomic64(1, &gCntSurfMarkSat);
        if (rd3_log_due((uint64_t)n, 4, 1023))
            ALOG("surface wait: surface %u already holds %u pending marks -- this one REFUSED (readies lost? "
                 "surface_wait_saturated %lld)", id, RD3_SP_MAX_COUNT, (long long)n + 1);
        return RD3K_SS_BUSY;   /* not taken: the plugin neither marks it newest at commit nor sends its ready */
    }
    case RD3_SPM_LATE: {
        const SInt64 n = OSAddAtomic64(1, &gCntSurfMarkLate);
        if (rd3_log_due((uint64_t)n, 4, 1023))
            ALOG("surface wait: late ready mark of surface %u (a mark its wait timed out on or the table dropped; "
                 "never a newer write's count; surface_wait_late %lld)", id, (long long)n + 1);
        return 0;
    }
    default: return RD3K_SS_NO_SURFACE;
    }
}
/* waits, bounded, while WindowServer's GPU still writes surface 'id' (any thread without a spinlock; the
 * page-off holds the family's lock, which the ready marks never take). 0 nothing pending, 1 waited and
 * ready, 2 timed out (cleared by this wait), 3 cleared under this wait by another wait's timeout, an
 * eviction or the switch-off (its write did not finish: not counted as a successful wait), 4 pending but
 * not waited (the timeout budget is spent until a real ready: rd3_sp RD3_SPW_SKIP); *us the time
 * waited. iop: the IOP zero-copy flip's wait, counted apart from the page-offs' (ECC review of N41) */
static int rd3AccelSurfGpuWait(unsigned id, bool iop, unsigned long long *us)
{
    *us = 0;
    IOLock *l = gSurfPendLock;
    if (!l || !id || !gSurfWait) return 0;
    const int ms = gSurfWaitMs < 1 ? 1 : gSurfWaitMs > (int)RD3_SP_WAIT_MAX_MS ? (int)RD3_SP_WAIT_MAX_MS : gSurfWaitMs;
    const uint64_t start = mach_absolute_time();
    uint64_t deadline = 0, epoch = 0;
    clock_interval_to_deadline((uint32_t)ms, kMillisecondScale, &deadline);
    bool waited = false;
    uint32_t dropped = 0;
    enum rd3_sp_wait_res r;
    IOLockLock(l);
    const uint64_t starv0 = gSurfPend.starvations;
    for (;;) {
        r = rd3_sp_wait_step(&gSurfPend, id, mach_absolute_time(), deadline, &dropped, &epoch);
        if (r != RD3_SPW_WAIT) break;
        waited = true;
        IOLockSleepDeadline(l, &gSurfPend, deadline, THREAD_UNINT);
    }
    if (r == RD3_SPW_TIMEOUT) IOLockWakeup(l, &gSurfPend, false);   /* the surface's other waiters: CLEARED */
    const bool starvedNow = gSurfPend.starvations != starv0;
    IOLockUnlock(l);
    if (starvedNow)
        ALOG("surface wait: %u waits in a row timed out with no ready mark between them -- page-offs and IOP flips "
             "STOP waiting until WindowServer's next ready mark (the timeout budget; surface_wait_starved)",
             RD3_SP_BUDGET);
    if (r == RD3_SPW_SKIP) {   /* the budget is spent: not waited, nothing cleared */
        const SInt64 n = OSAddAtomic64(1, &gCntSurfSkipped);
        if (rd3_log_due((uint64_t)n, 4, 1023))
            ALOG("surface wait: %s of surface %u NOT waited for WindowServer's pending GPU write: the timeout budget "
                 "is spent until a ready mark comes (surface_wait_skipped %lld)", iop ? "IOP flip" : "page-off", id,
                 (long long)n + 1);
        return 4;
    }
    if (!waited && r == RD3_SPW_READY) return 0;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    *us = ns / 1000;
    const char *who = iop ? "IOP flip" : "page-off";
    int *maxus = iop ? &gSurfIopWaitMaxUs : &gSurfWaitMaxUs;
    if ((int)*us > *maxus) *maxus = (int)*us;
    OSAddAtomic64((SInt64)*us, iop ? &gCntSurfIopWaitUs : &gCntSurfWaitUs);
    if (r == RD3_SPW_TIMEOUT) {
        const SInt64 n = OSAddAtomic64(1, iop ? &gCntSurfIopTimeouts : &gCntSurfTimeouts);
        if (rd3_log_due((uint64_t)n, 8, 1023))
            ALOG("surface wait: %s of surface %u: WindowServer's GPU write still PENDING after %d ms -- going on "
                 "without it (%u mark(s) dropped, their readies counted late; surface_wait_%stimeouts %lld)", who, id,
                 ms, dropped, iop ? "iop_" : "", (long long)n + 1);
        return 2;
    }
    if (r == RD3_SPW_CLEARED) {
        const SInt64 n = OSAddAtomic64(1, iop ? &gCntSurfIopCleared : &gCntSurfCleared);
        if (rd3_log_due((uint64_t)n, 8, 1023))
            ALOG("surface wait: %s of surface %u: its pending write was CLEARED after %llu us by another wait's "
                 "timeout, a full table or the switch-off -- going on without it (surface_wait_%scleared %lld)", who,
                 id, *us, iop ? "iop_" : "", (long long)n + 1);
        return 3;
    }
    const SInt64 n = OSAddAtomic64(1, iop ? &gCntSurfIopWaits : &gCntSurfWaits);
    if (rd3_log_due((uint64_t)n, 4, 4095))
        ALOG("surface wait: %s of surface %u waited %llu us for WindowServer's GPU write (surface_wait_%swaits %lld)",
             who, id, *us, iop ? "iop_" : "", (long long)n + 1);
    return 1;
}
/* the trace flags of a wait's answer (common/rdna3_trace.h RD3_TRPO_*) */
static unsigned rd3AccelSurfWaitTrFlags(int w)
{
    return w == 1 ? RD3_TRPO_WAITED : w == 2 ? RD3_TRPO_TIMEOUT : w == 3 ? RD3_TRPO_CLEARED
         : w == 4 ? RD3_TRPO_SKIPPED : 0u;
}
/* the table's own statistics (arg2: 0 aged, 1 ghosts dropped, 2 starvations, 3 evicted marks forgiven) */
static int rd3AccelSurfStatSysctl SYSCTL_HANDLER_ARGS
{
    (void)arg1;
    uint64_t v = 0;
    IOLock *l = gSurfPendLock;
    if (l) {
        IOLockLock(l);
        v = arg2 == 0 ? gSurfPend.aged : arg2 == 1 ? gSurfPend.ghosts_dropped : arg2 == 2 ? gSurfPend.starvations
                                                                                : gSurfPend.evict_kept;
        IOLockUnlock(l);
    }
    return sysctl_handle_quad(oidp, (long long *)&v, 0, req);
}
static int rd3AccelSurfBusySysctl SYSCTL_HANDLER_ARGS
{
    int v = 0;
    IOLock *l = gSurfPendLock;
    if (l) { IOLockLock(l); v = (int)rd3_sp_busy(&gSurfPend); IOLockUnlock(l); }
    return sysctl_handle_int(oidp, &v, 0, req);
}
static int rd3AccelSurfWaitSysctl SYSCTL_HANDLER_ARGS
{
    int v = gSurfWait;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    gSurfWait = v ? 1 : 0;
    if (!gSurfWait && gSurfPendLock) {   /* switched off: no mark left from before holds a page-off */
        IOLockLock(gSurfPendLock);
        const unsigned had = rd3_sp_clear(&gSurfPend);
        IOLockWakeup(gSurfPendLock, &gSurfPend, false);
        IOLockUnlock(gSurfPendLock);
        ALOG("surface wait: switched OFF at runtime (%u surface(s) pending forgotten); the plugin keeps its old "
             "order once its marks are refused", had);
    } else if (gSurfWait) {
        ALOG("surface wait: switched ON at runtime");
    }
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_surface_wait, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
            rd3AccelSurfWaitSysctl, "I",
            "DRIVER-PLAN N41: 1 (default, boot-arg rdna3surfwait) = a page-off waits for WindowServer's pending GPU "
            "write of the surface (the plugin marks it at commit); 0 = marks refused, no wait");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_surface_wait_ms, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gSurfWaitMs, 0,
           "N41: the page-off's bound on that wait, ms (1..1000, default 100)");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_surface_wait_maxus, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfWaitMaxUs, 0,
           "N41: the longest page-off wait for a pending GPU write, us");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_surface_wait_iop_maxus, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfIopWaitMaxUs, 0,
           "N41: the longest IOP zero-copy flip wait for a pending GPU write, us");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_surface_wait_busy, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
            rd3AccelSurfBusySysctl, "I", "N41: surfaces with WindowServer's GPU writes pending now (a gauge)");
RD3ACCEL_CNT(rdna3accel_surface_wait_pending,   gCntSurfMarkPend,  "N41: WindowServer's pending marks taken (at commit)");
RD3ACCEL_CNT(rdna3accel_surface_wait_ready,     gCntSurfMarkReady, "N41: WindowServer's ready marks taken (GPU done)");
RD3ACCEL_CNT(rdna3accel_surface_wait_late,      gCntSurfMarkLate,  "N41: late ready marks: of a mark a timeout dropped, or of a surface not held");
RD3ACCEL_CNT(rdna3accel_surface_wait_evicted,   gCntSurfMarkEvict, "N41: surfaces whose marks were dropped: the table was full");
RD3ACCEL_CNT(rdna3accel_surface_wait_saturated, gCntSurfMarkSat,   "N41: pending marks refused: the surface held the most");
RD3ACCEL_CNT(rdna3accel_surface_wait_off,       gCntSurfMarkOff,   "N41: marks refused: surface_wait 0");
RD3ACCEL_CNT(rdna3accel_surface_wait_notws,     gCntSurfMarkNotWs, "N41: marks refused: not from WindowServer");
RD3ACCEL_CNT(rdna3accel_surface_wait_notown,    gCntSurfMarkNotOwn, "N41: pending marks refused: not a surface of the calling task");
RD3ACCEL_CNT(rdna3accel_surface_wait_notreal,   gCntSurfMarkNotReal, "N41 security review: marks refused: named WindowServer, neither uid 88 nor the display client's pid");
RD3ACCEL_CNT(rdna3accel_surface_wait_moot,      gCntSurfMarkMoot,  "N41 security review: pending marks refused: no WindowServer surface can be VRAM (rdna3a3ws=0, rdna3a3display=0, no IOP flip)");
RD3ACCEL_CNT(rdna3accel_surface_wait_skipped,   gCntSurfSkipped,   "N41 security review: page-offs / IOP flips not waited: the timeout budget was spent");
RD3ACCEL_CNT(rdna3accel_surface_wait_ws_pid_refused, gCntSurfWsPidRefused, "N41 review round 3: a second WindowServer refused as the display client while the first one lives");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_surface_wait_ws_pid, CTLFLAG_RD | CTLFLAG_LOCKED, (int *)&gSurfWsPid, 0,
           "N41 security review: WindowServer's display client pid (its legacy swaps / display-pipe transactions), 0 none");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_surface_wait_mark_pid, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfMarkPid, 0,
           "N41 security review: the pid of the first GPU mark taken");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_surface_wait_mark_uid, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfMarkUid, 0,
           "N41 security review: the uid of the first GPU mark taken (88 _windowserver; -1 none yet)");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_surface_wait_aged, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
            rd3AccelSurfStatSysctl, "Q", "N41 review round 2: forgiven marks aged out (their readies never came)");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_surface_wait_ghosts, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 1,
            rd3AccelSurfStatSysctl, "Q", "N41 review round 2: pending marks a timeout dropped for good (a lost ready's ghost)");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_surface_wait_starved, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 2,
            rd3AccelSurfStatSysctl, "Q", "N41 security review: times the timeout budget ran out (waits skipped until a ready)");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_surface_wait_evict_kept, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 3,
            rd3AccelSurfStatSysctl, "Q", "N41 item 4: marks of a surface dropped from the full table, forgiven when it was marked again");
RD3ACCEL_CNT(rdna3accel_surface_wait_waits,     gCntSurfWaits,     "N41: page-offs that waited for a pending GPU write and saw it done");
RD3ACCEL_CNT(rdna3accel_surface_wait_timeouts,  gCntSurfTimeouts,  "N41: page-offs whose write was still pending at the bound");
RD3ACCEL_CNT(rdna3accel_surface_wait_cleared,   gCntSurfCleared,   "N41: page-offs whose wait was cleared under them (another timeout, a full table, off)");
RD3ACCEL_CNT(rdna3accel_surface_wait_us,        gCntSurfWaitUs,    "N41: time all page-off waits took, us");
RD3ACCEL_CNT(rdna3accel_surface_wait_iop_waits, gCntSurfIopWaits,  "N41: IOP zero-copy flips that waited for a pending GPU write and saw it done");
RD3ACCEL_CNT(rdna3accel_surface_wait_iop_timeouts, gCntSurfIopTimeouts, "N41: IOP flips whose write was still pending at the bound");
RD3ACCEL_CNT(rdna3accel_surface_wait_iop_cleared, gCntSurfIopCleared, "N41: IOP flips whose wait was cleared under them");
RD3ACCEL_CNT(rdna3accel_surface_wait_iop_us,    gCntSurfIopWaitUs, "N41: time all IOP flip waits took, us");
RD3ACCEL_CNT(rdna3accel_surface_dirty_trybusy,  gCntDirtyTryBusy,  "N41: try-lock dirty marks that found the accelerator's lock taken");
RD3ACCEL_CNT(rdna3accel_vm_parked,        gCntVmParked,        "N41: deallocPhysical kept the VRAM (parked) instead of freeing it");
RD3ACCEL_CNT(rdna3accel_vm_unparked,      gCntVmUnparked,      "N41: allocPhysical answered with the parked VRAM");
RD3ACCEL_CNT(rdna3accel_vm_park_released, gCntVmParkReleased,  "N41: parked VRAM released (grown, replaced, the object freed)");
RD3ACCEL_CNT(rdna3accel_vm_park_grow,     gCntVmParkGrow,      "N41: parks released because the resource grew");
RD3ACCEL_CNT(rdna3accel_vm_reparked,      gCntVmReparked,      "N41: a park replaced another (never expected)");
RD3ACCEL_CNT(rdna3accel_vm_parked_bytes,  gCntVmParkedBytes,   "N41: bytes parked now");
RD3ACCEL_CNT(rdna3accel_vm_park_noimport, gCntVmNoImport,      "N41 security review: deallocPhysical freed, not parked: no BO was imported over the allocation");
RD3ACCEL_CNT(rdna3accel_vm_park_trim_pressure, gCntVmTrimPressure, "N41 security review: allocPhysical refusals that released the parks no BO holds");
RD3ACCEL_CNT(rdna3accel_vm_park_trim_cap, gCntVmTrimCap,       "N41 security review: parks past vm_park_cap_mb that released the oldest no BO holds");
RD3ACCEL_CNT(rdna3accel_vm_park_trimmed,  gCntVmTrimmed,       "N41 security review: parks released by those (pressure or cap)");
RD3ACCEL_CNT(rdna3accel_vm_park_trimmed_bytes, gCntVmTrimmedBytes, "N41 security review: bytes those released");
RD3ACCEL_CNT(rdna3accel_vm_park_trim_held, gCntVmTrimHeld,     "N41 security review: parks kept by those because a BO holds them");
RD3ACCEL_CNT(rdna3accel_vm_park_trim_unknown, gCntVmTrimUnknown, "N41 security review: parks kept by those because the core's count was unknown");
RD3ACCEL_CNT(rdna3accel_vm_alloc_retry_ok, gCntVmRetryOk,      "N41 security review: refused allocations granted after the parks were released");
RD3ACCEL_CNT(rdna3accel_vm_alloc_retry_fail, gCntVmRetryFail,  "N41 security review: refused allocations refused again after the parks were released");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_vm_park_cap_mb, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gVmParkCapMb, 0,
           "N41 security review: parked VRAM kept at most, MiB (default 512; 0 no cap): past it the oldest parks no "
           "BO holds are released");
RD3ACCEL_CNT(rdna3accel_vm_free_live,     gCntVmFreeLive,      "N41: vidmem objects freed with VRAM still live (released in free())");
RD3ACCEL_CNT(rdna3accel_vm_released,      gCntVmReleased,      "N41: VRAM allocations really given back to the framebuffer");
RD3ACCEL_CNT(rdna3accel_vm_released_bytes, gCntVmReleasedBytes, "N41: their bytes");
RD3ACCEL_CNT(rdna3accel_vm_release_failed, gCntVmReleaseFailed, "N41: VRAM the framebuffer did not take back (or no accelerator): leaked");
RD3ACCEL_CNT(rdna3accel_vm_restored,      gCntVmRestored,      "N41: dirty marks that made the parked VRAM live again first");
RD3ACCEL_CNT(rdna3accel_vm_restore_miss,  gCntVmRestoreMiss,   "N41: dirty marks of parked VRAM that could not restore it (refused)");
RD3ACCEL_CNT(rdna3accel_vm_dirty_moved,   gCntVmDirtyMoved,    "N41: dirty marks of an allocation other than the surface's last BO import");
RD3ACCEL_CNT(rdna3accel_vm_apdesc_moved,  gCntApDescMoved,     "N41: cached aperture descriptors dropped: the VRAM moved, changed size or was released");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_vm_sticky, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gVmSticky, 0,
           "DRIVER-PLAN N41: 1 (default, boot-arg rdna3vmsticky) = deallocPhysical parks a surface's VRAM and the next "
           "allocPhysical hands it back; 0 = every deallocPhysical frees (the old behaviour)");
static void rd3AccelInstallSysctlPark(void)
{
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_ms);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_maxus); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_iop_maxus);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_busy); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_notown);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_cleared); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_iop_waits);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_iop_timeouts); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_iop_cleared);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_iop_us); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_release_failed);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_pending); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_ready);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_late); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_evicted);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_saturated); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_off);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_notws); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_waits);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_timeouts); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_us);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_dirty_trybusy);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_sticky);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_parked);   sysctl_register_oid(&sysctl__debug_rdna3accel_vm_unparked);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_released); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_grow);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_reparked); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_parked_bytes);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_free_live); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_released);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_released_bytes); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_restored);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_restore_miss); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_dirty_moved);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_apdesc_moved);
    /* the security review and round-2 review of N41 */
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_notreal); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_moot);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_skipped); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_ws_pid);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_ws_pid_refused);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_mark_pid); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_mark_uid);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_aged); sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_ghosts);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_starved);
    sysctl_register_oid(&sysctl__debug_rdna3accel_surface_wait_evict_kept);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_noimport); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_trim_pressure);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_trim_cap); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_trimmed);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_trimmed_bytes); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_trim_held);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_trim_unknown); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_alloc_retry_ok);
    sysctl_register_oid(&sysctl__debug_rdna3accel_vm_alloc_retry_fail); sysctl_register_oid(&sysctl__debug_rdna3accel_vm_park_cap_mb);
}
/* the family's lock is held. The plugin keeps drawing into the allocation the family parked; a dirty
 * mark makes exactly that allocation live again (IOAccelVidMemory::wire -> allocPhysical's reuse) before
 * marking it newest, never a new one (it would hold nothing the plugin wrote). false: the dirty mark
 * is refused (the plugin copies out instead). NullMoth comparison: nvAccelRestoreParkedForDirty (6372823) */
static bool rd3AccelRestoreParkedForDirty(RD3Resource *res, RD3VidMemory *vm, unsigned surfaceID)
{
    if (!res || !vm || vm->fRmHandle || vm->fScanout || !rd3_vp_valid(&vm->fPark)) return true;   /* as before */
    const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)vm + 0x40);
    const struct rd3_vp before = rd3VmState(vm);
    const struct rd3_vp_alloc parked = vm->fPark;
    bool ok = false;
    if (rd3_vp_can_restore(&before, want)) {
        _ZN16IOAccelVidMemory4wireEv(vm);
        const struct rd3_vp after = rd3VmState(vm);
        ok = rd3_vp_same(&after.live, &parked) && !rd3_vp_valid(&after.park);
    }
    const SInt64 n = OSAddAtomic64(1, ok ? &gCntVmRestored : &gCntVmRestoreMiss);
    if (!ok ? rd3_log_due((uint64_t)n, 8, 1023) : rd3_log_due((uint64_t)n, 4, 4095))
        ALOG("surfvram dirty: surface %u: its VRAM was parked (phys 0x%llx, %llu bytes) -- %s (want %llu; "
             "vm_restored %lld, vm_restore_miss %lld)", surfaceID, parked.phys, parked.bytes,
             ok ? "made live again before the mark" : "could NOT be made live again: the mark is refused", want,
             (long long)gCntVmRestored, (long long)gCntVmRestoreMiss);
    return ok;
}

static void    *gNVSVRoot;
static unsigned gNVSVOk, gNVSVRefused;
static inline void rd3SVZero(void *p, unsigned long n) { volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static unsigned gNVSVDirty, gNVSVDirtyRefused, gNVSVPageoff, gNVSVPageoffFail;
static unsigned rd3AccelSurfDirty(void *cache, RD3Resource *res, unsigned plane)
{
    RD3VidMemory *dvm = rd3res_vidmem(res);
    volatile unsigned char *r = (volatile unsigned char *)res;
    unsigned char type = r[0x14];
    unsigned int fl = *(volatile unsigned int *)(r + 0xc);
    volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
    if (type != 0xc0 || (fl & 0x8000) || !valid || plane > 15) return RD3K_SS_NO_CACHE;
    if (!rd3SVPend(_ZNK9IOSurface12getSurfaceIDEv(*(void *volatile *)((unsigned char *)cache + 0x10)), 1, res)) return RD3K_SS_NO_CACHE;
    _ZN20IOSurfaceDeviceCache10setCurrentEv(cache);
    valid[6] |= (unsigned short)(1u << plane);
    valid[0] &= (unsigned short)~(1u << plane);
    /* the plugin wrote this allocation (after its GPU work was waited for): the IOP zero-copy flip's
     * marker (rd3_iop_flip_ok, N38) - its generation, which a later allocation never has */
    res->fPluginWroteGen = dvm ? dvm->fAllocGen : 0;
    /* N41: a mark of another allocation than the surface's last BO import: VRAM that import never drew into
     * (the re-backing the parking removes; two processes' imports of one surface at different times too) */
    if (dvm && res->fImportGen && dvm->fAllocGen != res->fImportGen) {
        const SInt64 n = OSAddAtomic64(1, &gCntVmDirtyMoved);
        if (rd3_log_due((uint64_t)n, 8, 1023))
            ALOG("surfvram dirty: resource %p marks allocation #%llu (phys 0x%llx) newest but its last BO import was "
                 "allocation #%llu -- the plugin may have drawn elsewhere (vm_dirty_moved %lld)", res, dvm->fAllocGen,
                 dvm->fPhys, res->fImportGen, (long long)n + 1);
    }
    return 0;
}
/* an IOSurface answer for the trace (card run 2026-10-17b, Q10): arg 0
 * import refused, 1 imported, 2 a dirty mark, 3 an RM share; the VRAM when
 * known (then also the IOSurface <-> VRAM pair) */
static void rd3AccelTraceSurf(unsigned arg, unsigned surfaceID, unsigned plane, unsigned status, unsigned reason,
                              void *surf, const void *res, const RD3VidMemory *vm)
{
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_SURFGEM);
    e.arg = (uint8_t)arg;
    e.obj = rd3TrObj(vm);
    e.v0 = surfaceID;
    e.v1 = status;
    e.v2 = reason | (plane << 16);
    if (surf) e.v3 = ((unsigned)((IOSurface *)surf)->getWidth() & 0xffffu) | ((unsigned)((IOSurface *)surf)->getHeight() << 16);
    e.pa = vm ? vm->fPhys : 0;
    rd3Trace(e);
    if (vm && vm->fPhys && res)
        rd3AccelTraceSurfRes(res, surfaceID, plane, arg == 1 ? 4u : arg == 2 ? 5u : 6u, vm->fPhys, vm->fBytes);
}
/* a plane above RD3K_SS_MAX_PLANE from a user client: refused before the device cache lookup (the
 * caller's 32-bit value reached deviceCacheForIOSurface unchecked) */
static volatile SInt32 gNVSVPlaneRefused;
static void rd3AccelSurfPlaneRefused(const char *what, unsigned surfaceID, unsigned plane)
{
    const SInt32 k = OSIncrementAtomic(&gNVSVPlaneRefused);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("%s: surface %u plane %u REFUSED: an IOSurface has planes 0..%u -- not looked up (%d such)", what,
             surfaceID, plane, RD3K_SS_MAX_PLANE, (int)k + 1);
}
static IOReturn rd3AccelSurfVram(struct RD3KSurfShareEsc *p, task_t task, unsigned cmd)
{
    p->hSrcClient = p->hSrcMemory = 0;
    if (p->version != RD3K_SURFSHARE_VERSION) { p->status = RD3K_SS_BAD_VERSION; return kIOReturnSuccess; }
    /* N41: WindowServer's pending / ready marks (no family lock: a page-off waiting for this very mark
     * holds that lock; a pending mark looks the surface up for the caller) and the try-lock dirty mark */
    if (cmd == RD3K_ESC_SURF_DIRTY && (p->plane & RD3K_SS_PLANE_GPU_MARK)) {
        p->status = (p->plane & ~(RD3K_SS_PLANE_GPU_MARK | 1u)) ? RD3K_SS_NO_CACHE
                                                                 : rd3AccelSurfGpuMark(p->surfaceID, (p->plane & 1u) != 0,
                                                                                       task);
        return kIOReturnSuccess;
    }
    /* the plane without the try-lock flag: a local, the caller's escape is not rewritten (ECC review of N41) */
    const bool tryOnly = cmd == RD3K_ESC_SURF_DIRTY && (p->plane & RD3K_SS_PLANE_TRYLOCK);
    const unsigned plane = tryOnly ? (p->plane & ~RD3K_SS_PLANE_TRYLOCK) : p->plane;
    if (!rd3k_ss_plane_ok(plane)) {
        p->status = RD3K_SS_NO_CACHE;
        rd3AccelSurfPlaneRefused(cmd == RD3K_ESC_SURF_DIRTY ? "surfvram dirty" : "surfvram", p->surfaceID, plane);
        return kIOReturnSuccess;
    }
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    struct RD3KRmHandles h; rd3SVZero(&h, sizeof h);
    if (!task || !a || !rd3AccelRmHandles(&h)) { p->status = RD3K_SS_OFF; return kIOReturnSuccess; }
    if (!rd3AccelSurfaceRoot()) { p->status = RD3K_SS_OFF; return kIOReturnSuccess; }
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
            rd3AccelTraceSurf(3, p->surfaceID, plane, st, reason, surf, nullptr, nullptr);
            ((OSObject *)surf)->release();
            p->status = st; gNVSVRefused++;
            if (rd3AccelA3RefusedFirst(p->surfaceID, n))
                ALOG("surfvram: surface %u plane %u refused: %s (%lld refused)", p->surfaceID, plane, why, (long long)n);
            return kIOReturnSuccess;
        }
    }
    if (!rd3FamLock(a, tryOnly)) {
        ((OSObject *)surf)->release();
        p->status = tryOnly ? RD3K_SS_BUSY : RD3K_SS_OFF;
        if (tryOnly) OSAddAtomic64(1, &gCntDirtyTryBusy);   /* expected: the plugin's mark at commit stands (N41) */
        return kIOReturnSuccess;
    }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, plane);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    RD3Resource *res = OSDynamicCast(RD3Resource, ctx);
    RD3VidMemory *vm = res ? rd3res_vidmem(res) : nullptr;
    if (cmd == RD3K_ESC_SURF_DIRTY) {
        /* N41: parked VRAM is made live again first: the allocation the plugin drew into */
        const bool sameBacking = rd3AccelRestoreParkedForDirty(res, vm, p->surfaceID);
        p->status = (sameBacking && cache && res && vm && vm->fRmHandle) ? rd3AccelSurfDirty(cache, res, plane)
                                                                          : RD3K_SS_NO_CACHE;
        if (p->status == 0) gNVSVDirty++; else gNVSVDirtyRefused++;
        rd3AccelTraceSurf(2, p->surfaceID, plane, p->status, 0, surf, res, vm);
        if (p->status == 0 && gCntA3DispImported) rd3AccelA3DispDirty(p->surfaceID, vm->fPhys);
        if (gNVSVDirty + gNVSVDirtyRefused <= 6 || ((gNVSVDirty + gNVSVDirtyRefused) & 4095) == 0)
            ALOG("surfvram dirty: surface %u plane %u type 0x%x -> %#x (dirty %u, refused %u)", p->surfaceID, plane,
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
    rd3AccelTraceSurf(3, p->surfaceID, plane, p->status, 0, surf, res, vm);
    if (gNVSVOk + gNVSVRefused <= 8 || ((gNVSVOk + gNVSVRefused) & 1023) == 0)
        ALOG("surfvram: surface %u plane %u -> cache %p res %p vidmem %p (%llu B) -> client %#x mem %#x status %#x (ok %u, refused %u)",
             p->surfaceID, plane, cache, res, vm, vm ? vm->fBytes : 0ull, p->hSrcClient, p->hSrcMemory, p->status, gNVSVOk, gNVSVRefused);
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
    if (!rd3k_ss_plane_ok(p->plane)) {
        p->status = RD3K_SS_NO_CACHE;
        rd3AccelSurfPlaneRefused("surfgem", p->surfaceID, p->plane);
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
        rd3AccelTraceSurf(0, p->surfaceID, p->plane, pst, reason, surf, nullptr, nullptr);
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
        if (rr == kIOReturnSuccess) {
            p->status = 0; p->kapiMemory = vm->fRmHandle; p->bytes = vm->fBytes;
            res->fImportGen = vm->fAllocGen;   /* N41: the allocation this BO draws into */
            vm->fBoGen = vm->fAllocGen;        /* N41 security review: only this allocation is parked */
        }
        else { p->status = RD3K_SS_NO_VRAM; why = "the core kext refused the reference (allocation unknown or freed)"; }
    }
    rd3AccelSurfGemLog(p, why ? why : "?", vm, vm ? vm->fBytes : 0ull);
    rd3AccelTraceSurf(p->status == 0 ? 1u : 0u, p->surfaceID, p->plane, p->status, 0, surf, res, vm);
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
    /* N41: a CPU lock while WindowServer's GPU still writes the surface waits for that write, as an
     * IOSurfaceLock does on Apple's drivers (the plugin marked it pending at commit), bounded */
    unsigned long long gpuWaitUs = 0;
    const int gpuWait = rd3AccelSurfGpuWait(_ZNK9IOSurface12getSurfaceIDEv(surf), false, &gpuWaitUs);
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
    {
        struct rd3_trace_ev e = rd3TrEv(RD3_TR_PAGEOFF);
        e.arg = ok ? 1 : 0;
        e.obj = rd3TrObj(this);
        e.v0 = rd3TrSat(len >> 10);
        e.v1 = rd3TrSat(usec + cpuUs);
        e.v2 = st;
        e.v3 = rd3TrSat(gpuWaitUs);                      /* N41: the wait for WindowServer's GPU write */
        e.flags = (uint8_t)rd3AccelSurfWaitTrFlags(gpuWait);
        e.pa = vm->fPhys;
        rd3Trace(e);
        rd3AccelTraceSurfRes(this, _ZNK9IOSurface12getSurfaceIDEv(surf), 0, 2, vm->fPhys, vm->fBytes);
    }
    if (gNVSVPageoff + gNVSVPageoffFail <= 8 || ((gNVSVPageoff + gNVSVPageoffFail) & 1023) == 0)
        ALOG("surfvram pageoff: res %p %llu B on the copy engine -> %#x in %llu us%s + cpu %llu us (ok %u, failed %u, staged %u)",
             this, len, st, usec, staged ? " (staged)" : " (per page)", cpuUs, gNVSVPageoff, gNVSVPageoffFail, gNVSVStaged);
}

static IOSurface *gIopFlipFront[kRd3MaxFB], *gIopFlipPrev[kRd3MaxFB];
static bool gIopFlipped[kRd3MaxFB];
static unsigned gIopFlipLogs;
/* an IOP flip done: 's' (a reference the caller hands over) is the front, the one before it leaves - its
 * reference dropped after the lock. Every path (the old async worker, the synchronous flip, the present
 * queue) moves these records under gPresLock: debug.rdna3accel_iop_queue / _iop_async toggled at runtime let
 * two writers race into a double release (ECC review of N38) */
static void rd3AccelIopFrontMove(unsigned head, IOSurface *s)
{
    IOLock *l = rd3LockOnce(&gPresLock);
    if (l) IOLockLock(l);
    IOSurface *drop = gIopFlipPrev[head];
    gIopFlipPrev[head] = gIopFlipFront[head];
    gIopFlipFront[head] = s;
    gIopFlipped[head] = true;
    if (l) IOLockUnlock(l);
    if (drop) ((OSObject *)drop)->release();
}
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
           rd3SimpleLockOnce(&gA3DispLock) && rd3SimpleLockOnce(&gCfPendLock) && rd3SimpleLockOnce(&gScStaleLock) &&
           rd3SimpleLockOnce(&gWgLock) && rd3SimpleLockOnce(&gIopStaleLock) &&
           rd3SurfCloseLockOnce() && (rd3TraceInit() || true);   /* no trace memory: the driver runs without it */
}

extern "C" {
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
}
struct RD3IopAsync { void *pipe; IOService *fb; IOSurface *s; struct RD3KFlipRequest f; volatile SInt32 busy; struct rd3_tcall *call;
                     unsigned long long seq; /* the shown record this flip set */ };
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
    const uint64_t issued = mach_absolute_time();
    IOReturn rr = sym ? e->fb->callPlatformFunction(sym, false, &e->f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr == kIOReturnSuccess) {
        gIopAsyncRefuseRun = 0;
        rd3AccelScPanelMoved(head, "an IOP flip", issued, true);
        rd3AccelIopFrontMove(head, s);
        const SInt64 n = OSAddAtomic64(1, &gCntIopFlip); OSAddAtomic64(1, &gCntIopAsync);
        if (n < 4 || n == 600 || (n & 0xffff) == 0)
            ALOG("iop: ASYNC swap #%lld head %u: surface 0x%x %ux%u pitch %u scanned out of its own VRAM (flipResult %d)",
                 (long long)n + 1, head, s->getSurfaceID(), e->f.width, e->f.height, e->f.pitch, e->f.flipResult);
    } else {
        OSAddAtomic64(1, &gCntIopAsyncRefused); OSAddAtomic64(1, &gCntIopFlipNo[7]);
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: async flip REFUSED (0x%x, flipResult %d) head %u -- that frame is not on the panel",
                                                     (unsigned)rr, e->f.flipResult, head); }
        rd3AccelForgetShownIf(head, e->seq);
        if (++gIopAsyncRefuseRun >= 3 && gIopAsync) {
            gIopAsync = 0;
            ALOG("iop: b89k 3 async refusals in a row -- debug.rdna3accel_iop_async=0 (synchronous flip, which copies on refusal)");
        }
        ((OSObject *)s)->release();
    }
    __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE);
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(e->pipe, nullptr);
}
static unsigned rd3AccelIopAsyncCancelled(RD3IopAsync *e);
static bool rd3AccelIopAsyncStart(void *pipe, IOService *fb, unsigned head, IOSurface *s, const struct RD3KFlipRequest *f,
                                  unsigned long long seq)
{
    RD3IopAsync *e = &gIopAs[head];
    for (unsigned i = 0; i < 50 && __atomic_load_n(&e->busy, __ATOMIC_ACQUIRE); i++) IOSleep(1);
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
        OSAddAtomic64(1, &gCntIopAsyncDrop);
        rd3AccelForgetShownIf(head, seq);   /* this frame never reaches the panel */
        return true;
    }
    if (!e->call) e->call = rd3_tcall_alloc("accel.iop_async", rd3AccelIopAsyncRun, e);
    if (!e->call) {
        rd3AccelForgetShownIf(head, seq);   /* the caller copies instead: this record is not on the panel */
        return false;
    }
    ((OSObject *)s)->retain();
    e->pipe = pipe; e->fb = fb; e->s = s; e->seq = seq;
    for (unsigned i = 0; i < sizeof *f; i++) ((volatile unsigned char *)&e->f)[i] = ((const unsigned char *)f)[i];
    __atomic_store_n(&e->busy, 1, __ATOMIC_RELEASE);
    const int how = rd3_tcall_enter3(e->call);
    if (how == RD3_TCALL_REFUSED) {
        /* the stop began (rd3_tcall_free logged it): no run will give the surface back or clear busy;
         * the caller copies instead, so this record is not on the panel (ECC review round 1) */
        rd3AccelIopAsyncCancelled(e);
        rd3AccelForgetShownIf(head, seq);
        return false;
    }
    if (how == RD3_TCALL_MERGED && gIopFlipLogs < 8) {
        /* busy kept a second flip out until the last run ended: a run still pending here means that
         * guard broke; the pending run flips the fields set above */
        gIopFlipLogs++;
        ALOG("iop: async flip head %u merged into a run still pending (busy guard broken?)", head);
    }
    return true;
}
/* an async IOP flip cancelled before it ran (rd3AccelIopStop): the surface reference it held given back */
static unsigned rd3AccelIopAsyncCancelled(RD3IopAsync *e)
{
    IOSurface *s = e->s;
    e->s = nullptr;
    if (s) ((OSObject *)s)->release();
    __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE);
    return 1;
}
/* the accelerator's stop, after the family's (no IOP transaction comes any more): an async IOP flip still
 * pending is cancelled (its surface reference given back; its pipe is not signalled, the family has stopped),
 * one running is waited for, the thread calls freed; the scanned surfaces' records (gIopFlipFront /
 * gIopFlipPrev) dropped under gPresLock, their references after it. ECC review round 2 of N38: neither was
 * released at the stop (a leak at unload only) */
static void rd3AccelIopStop(void)
{
    unsigned cancelled = 0, released = 0;
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        RD3IopAsync *e = &gIopAs[h];
        struct rd3_tcall *c = e->call;
        if (!c) continue;
        /* common/rdna3_tcall.h: a run still going is waited for, each one cancelled before it ran gives its
         * surface back (thread_call_cancel_wait is com.apple.kpi.private on macOS 26) */
        for (unsigned n = rd3_tcall_free(c); n; n--) cancelled += rd3AccelIopAsyncCancelled(e);
        e->call = nullptr;
    }
    IOSurface *drop[2 * kRd3MaxFB];
    unsigned nd = 0;
    IOLock *l = gPresLock;
    if (l) IOLockLock(l);
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        /* a head still on an IOP surface keeps its front's reference: the HUBP may scan that VRAM until a
         * modeset, and the release could hand it to another allocation (review of the N38 stop) */
        if (gIopFlipFront[h] && !gIopFlipped[h]) { drop[nd++] = gIopFlipFront[h]; gIopFlipFront[h] = nullptr; }
        if (gIopFlipPrev[h]) drop[nd++] = gIopFlipPrev[h];
        gIopFlipPrev[h] = nullptr;
    }
    if (l) IOLockUnlock(l);
    for (unsigned i = 0; i < nd; i++, released++) ((OSObject *)drop[i])->release();
    if (cancelled || released)
        ALOG("iop: stop: %u async flip(s) still pending cancelled, %u reference(s) of scanned IOSurfaces released",
             cancelled, released);
}
static bool rd3AccelPresentIop(unsigned head, IOService *fb, void *pipe, IOSurface *s,
                               const struct RD3KFlipRequest *f, RD3VidMemory *vm);
/* the IOP zero-copy flip's wait for WindowServer's pending GPU write (N41), counted apart from the page-offs'
 * (surface_wait_iop_*) and traced as PAGEOFF arg 2 with RD3_TRPO_IOP when it waited (v2 the surface, v3 us) */
static void rd3AccelIopGpuWait(unsigned id)
{
    unsigned long long us = 0;
    const int w = rd3AccelSurfGpuWait(id, true, &us);
    if (!w) return;
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_PAGEOFF);
    e.arg = 2;
    e.flags = (uint8_t)(RD3_TRPO_IOP | rd3AccelSurfWaitTrFlags(w));
    e.v2 = id;
    e.v3 = rd3TrSat(us);
    rd3Trace(e);
}
static bool rd3AccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)
{
    if (!gIopFlip || !fb || !s || head >= kRd3MaxFB) return false;
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a) return rd3AccelIopFlipNo(1);
    if (s->getPixelFormat() != 0x42475241u) return rd3AccelIopFlipNo(2);
    /* N41: the plugin marks VRAM newest at commit: never scan out a composite the GPU still writes. The wait
     * comes first, before anything about the surface's VRAM is read: a park, a grow or a free during it would
     * leave a looked-up handle, cookie or size stale (ECC review of N41, M1); it needs only the surface id */
    rd3AccelIopGpuWait(s->getSurfaceID());
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
    /* VRAM the family marks newest but the Metal plugin never wrote holds no frame: copied instead
     * (rd3_iop_flip_ok; the plugin's dirty mark comes after its GPU work was waited for) */
    if (gIopWrote && !rd3_iop_flip_ok(vramNewest, res ? res->fPluginWroteGen : 0, vm ? vm->fAllocGen : 0)) {
        const SInt64 n = OSAddAtomic64(1, &gCntPqIopUnwritten);
        if (n < 4 || (n & 0xfff) == 0)
            ALOG("iop: surface 0x%x: VRAM marked newest but the plugin never wrote this allocation -- copying its "
                 "system memory instead of flipping (%lld so far; debug.rdna3accel_iop_wrote=0 flips it)",
                 s->getSurfaceID(), (long long)n + 1);
        return false;
    }
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || P < 4 * W || H * P > bytes) return rd3AccelIopFlipNo(6);
    struct RD3KFlipRequest f; rd3SVZero(&f, sizeof f);
    f.version = RD3K_FLIP_ABI_VERSION; f.kapiMemory = kmem; f.allocationCookie = vm->fAllocationCookie; f.width = (unsigned)W; f.height = (unsigned)H; f.pitch = (unsigned)P;
    /* the head shows this IOSurface from now on (async: unless the flip is
     * dropped or refused, which forget it again) */
    const unsigned long long seq = rd3AccelNotePresented(vm, head);
    if (gIopAsync && pipe && gIopQueue && gPresReady) {
        /* the head's present queue (N38): Busy kept, the newest queued, the transaction complete at the latch */
        if (rd3AccelPresentIop(head, fb, pipe, s, &f, vm)) return true;
        rd3AccelForgetShownIf(head, seq);   /* refused: the caller copies, this record is not on the panel */
        return rd3AccelIopFlipNo(7);
    }
    if (gIopAsync && pipe) return rd3AccelIopAsyncStart(pipe, fb, head, s, &f, seq);
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("rd3FlipToSurfacePure");
    const uint64_t issued = mach_absolute_time();
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr != kIOReturnSuccess) {
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: flip REFUSED (0x%x, flipResult %d) head %u %llux%llu pitch %llu -- copying",
                                                     (unsigned)rr, f.flipResult, head, W, H, P); }
        rd3AccelForgetShown(head);
        return rd3AccelIopFlipNo(7);
    }
    ((OSObject *)s)->retain();
    rd3AccelScPanelMoved(head, "an IOP flip", issued, true);
    rd3AccelIopFrontMove(head, s);
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
/* rd3AccelIopHome: with the present queue (DRIVER-PLAN N38, below) */
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

/* ---- N61: what an IOP transaction costs (common/rdna3_iop_copy.h): the gates before the copy, the copy itself ---- */
static void rd3AccelIopGateNote(unsigned head, uint64_t t0)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    OSAddAtomic64((SInt64)(ns / 1000), &gCntIopGateUs);
    if (head < kRd3MaxFB) OSAddAtomic64((SInt64)(ns / 1000), &gCntIopGateUsHead[head]);
}
static void rd3AccelIopCopyNote(unsigned head, uint64_t t0, uint64_t bytes, int accessBytes, unsigned w, unsigned h,
                                unsigned spitch, unsigned dpitch, bool fromRes)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    const SInt64 us = (SInt64)(ns / 1000);
    const SInt64 k = OSAddAtomic64(1, &gCntIopCopyN);
    OSAddAtomic64((SInt64)bytes, &gCntIopCopyBytes);
    OSAddAtomic64(us, &gCntIopCopyUs);
    if (accessBytes == 8) OSAddAtomic64(1, &gCntIopCopy64);
    if (head < kRd3MaxFB) { OSAddAtomic64(1, &gCntIopCopyNHead[head]); OSAddAtomic64(us, &gCntIopCopyUsHead[head]); }
    SInt64 m = gCntIopCopyMaxUs;
    while (us > m && !OSCompareAndSwap64((UInt64)m, (UInt64)us, (volatile UInt64 *)&gCntIopCopyMaxUs)) m = gCntIopCopyMaxUs;
    if (rd3_iop_copy_log_due((uint64_t)k))
        ALOG("iop: copy #%lld (head %u): %ux%u from %s, pitch %u -> %u, %llu bytes in %lld us (%llu MB/s, %d-byte accesses); gates "
             "before the copies %lld us in all (N61)", (long long)k + 1, head, w, h, fromRes ? "our VRAM resource" : "the IOSurface's memory",
             spitch, dpitch, (unsigned long long)bytes, (long long)us, (unsigned long long)rd3_iop_mbps(bytes, (uint64_t)us), accessBytes,
             (long long)gCntIopGateUs);
}

/* ---- N59: an IOP copy of a buffer nothing wrote since this head last copied it (common/rdna3_iop_stale.h) ----
 * WindowServer presents each buffer of its ring because it drew a new frame into it; the Metal plugin puts that
 * frame into the IOSurface with the CPU after the GPU (an IOSurfaceLock that moves the seeds). Seeds unchanged
 * since the last copy = the copy-out has not landed: this copy would put the frame from a full ring ago on the
 * panel (card run 2026-10-26: animations step back). The wait polls the seeds every 1 ms, no lock held, bounded by
 * debug.rdna3accel_iop_stale_wait_ms (amdgpu: the implicit sync of a scanout BO, amdgpu_dm_commit_planes ->
 * dma_resv_wait_timeout before the flip). Not for a buffer with a VRAM page-off pending: its system memory is
 * filled by rd3AccelSurfCopyReady without a seed move. */
static rd3_is_head gIopStale[kRd3MaxFB];
static void rd3AccelIopStaleGate(IOSurface *s, unsigned head, unsigned *lseed, unsigned *useed)
{
    *lseed = s->getLockSeed(); *useed = s->getUnlockSeed();
    IOSimpleLock *l = gIopStaleLock;
    if (!l || head >= kRd3MaxFB) return;
    const unsigned id = s->getSurfaceID();
    if (rd3SVPend(id, 2)) return;
    uint64_t lastT = 0, nowNs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
    const uint32_t waitMs = rd3_is_wait_ms(gIopStaleWaitMs);
    IOSimpleLockLock(l);
    const int v = rd3_is_check(&gIopStale[head], id, *lseed, *useed, &lastT);
    const uint64_t allowUs = v == RD3_IS_STALE ? rd3_is_wait_us(&gIopStale[head], waitMs, nowNs) : 0;
    const bool capFirst = v == RD3_IS_STALE && waitMs && !allowUs && gIopStale[head].capped == 1;
    IOSimpleLockUnlock(l);
    if (v == RD3_IS_NEW) return;
    const SInt64 nc = OSAddAtomic64(1, &gCntIopStaleChecked);
    if (v == RD3_IS_FRESH) { OSAddAtomic64(1, &gCntIopStaleFresh); return; }
    const SInt64 n = OSAddAtomic64(1, &gCntIopStale);
    uint64_t agoNs = 0;
    if (lastT) absolutetime_to_nanoseconds(mach_absolute_time() - lastT, &agoNs);
    if (!allowUs) {
        const SInt64 k = OSAddAtomic64(1, &gCntIopStaleNotWaited);
        if (capFirst) {
            const SInt64 c = OSAddAtomic64(1, &gCntIopStaleCapped);
            if (rd3_log_due((uint64_t)c, 8, 1023))
                ALOG("iop stale: head %u has waited %u ms in vain within this second (timed-out waits, the cap): its stale "
                     "copies are NOT waited for until the next second (surface 0x%x; iop_stale_capped %lld windows)",
                     head, RD3_IS_CAP_MS, id, (long long)c + 1);
        }
        if (rd3_log_due((uint64_t)k, 8, 1023))
            ALOG("iop stale: head %u copies surface 0x%x with the seeds it had when this head last copied it %llu us ago "
                 "(lock %u unlock %u): nothing wrote it since -- an OLD frame goes to the panel, NOT waited (%s; "
                 "iop_stale %lld of %lld checked)", head, id, agoNs / 1000, *lseed, *useed,
                 waitMs ? "this second's cap of timed-out waits is spent" : "debug.rdna3accel_iop_stale_wait_ms 0", (long long)n + 1,
                 (long long)nc + 1);
        return;
    }
    /* done only when the unlock seed moved (the copy-out gave its write lock back: a lock seed that moved alone is
     * a writer still copying, ECC review of 5a8765b); bounded by elapsed time, not by a count of IOSleep(1) calls,
     * which a VM stretches to 1.5-2 ms each */
    const uint64_t t0 = mach_absolute_time();
    uint64_t deadline = 0, ns = 0;
    nanoseconds_to_absolutetime(allowUs * 1000ull, &deadline);
    deadline += t0;
    int moved = 0;
    bool lockMoved = false;
    while (!moved && mach_absolute_time() < deadline) {
        IOSleep(1);
        const unsigned l2 = s->getLockSeed(), u2 = s->getUnlockSeed();
        if (u2 != *useed) moved = 1;
        else if (l2 != *lseed) lockMoved = true;
    }
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    const unsigned us = (unsigned)(ns / 1000);
    OSAddAtomic64((SInt64)us, &gCntIopStaleWaitUs);
    if ((int)us > gIopStaleWaitMaxUs) gIopStaleWaitMaxUs = (int)us;
    IOSimpleLockLock(l);
    rd3_is_waited(&gIopStale[head], moved, us);   /* only a timed-out wait is charged to the head's cap */
    IOSimpleLockUnlock(l);
    const unsigned l1 = *lseed, u1 = *useed;
    *lseed = s->getLockSeed(); *useed = s->getUnlockSeed();
    if (moved) {
        const SInt64 k = OSAddAtomic64(1, &gCntIopStaleMoved);
        if (rd3_log_due((uint64_t)k, 8, 1023))
            ALOG("iop stale: head %u surface 0x%x was presented before its new frame was in it (seeds lock %u unlock %u, as "
                 "at its copy %llu us ago); the write came %u us into the wait (lock %u unlock %u): the new frame is "
                 "copied, the step back avoided (iop_stale_moved %lld of %lld stale)", head, id, l1, u1, agoNs / 1000,
                 us, *lseed, *useed, (long long)k + 1, (long long)n + 1);
    } else {
        const SInt64 k = OSAddAtomic64(1, &gCntIopStaleTimeout);
        if (rd3_log_due((uint64_t)k, 8, 1023))
            ALOG("iop stale: head %u surface 0x%x: no finished write within %u us (seeds lock %u unlock %u, as at its "
                 "copy %llu us ago%s) -- copied as it is: a re-present of an unchanged buffer, or an OLD frame "
                 "(iop_stale_timeout %lld of %lld stale; at most %u ms of such waits per head per second)", head, id, us,
                 *lseed, *useed, agoNs / 1000, lockMoved ? "; the write lock WAS taken: a writer still copying - torn "
                 "unless it ends first" : "", (long long)k + 1, (long long)n + 1, RD3_IS_CAP_MS);
    }
}
/* after the copy: the seeds the panel's frame came with; seeds that moved during the copy = a torn frame */
static void rd3AccelIopStaleNote(IOSurface *s, unsigned head, unsigned lseed, unsigned useed)
{
    IOSimpleLock *l = gIopStaleLock;
    if (!l || head >= kRd3MaxFB) return;
    const unsigned id = s->getSurfaceID(), l2 = s->getLockSeed(), u2 = s->getUnlockSeed();
    if (l2 != lseed || u2 != useed) {
        const SInt64 k = OSAddAtomic64(1, &gCntIopStaleTorn);
        if (rd3_log_due((uint64_t)k, 8, 1023))
            ALOG("iop stale: head %u surface 0x%x was written WHILE it was copied to the panel (seeds lock %u->%u unlock "
                 "%u->%u): a torn frame (iop_stale_torn %lld)", head, id, lseed, l2, useed, u2, (long long)k + 1);
    }
    IOSimpleLockLock(l);
    rd3_is_note(&gIopStale[head], id, lseed, useed, mach_absolute_time());
    IOSimpleLockUnlock(l);
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
/* whether a swap buffer holds a picture, and if not why: 8 x 8 samples
 * through the BAR (card run 58b review: 16 samples called a dark frame
 * empty), each inside the allocation */
enum RD3ResContent { kRcNoRes = 0, kRcNoVidmem, kRcNoKva, kRcNoGeometry, kRcEmpty, kRcPixels };
static const char *const kRcWhy[6] = { "no resource", "no vidmem", "no kernel mapping", "no geometry",
                                       "64 samples all zero", "pixels" };
static unsigned rd3AccelResContent(IOAccelResource2 *res)
{
    if (!res) return kRcNoRes;
    RD3VidMemory *vm = rd3res_vidmem(res);
    if (!vm) return kRcNoVidmem;
    if (!vm->fKva) return kRcNoKva;
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || pitch < 4u * w) return kRcNoGeometry;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    for (unsigned iy = 1; iy <= 8; iy++)
        for (unsigned ix = 1; ix <= 8; ix++) {
            const unsigned x = (w * ix) / 9, y = (h * iy) / 9;
            if (x >= w || y >= h || (unsigned long long)y * pitch + x * 4ull + 4 > vm->fBytes) continue;
            if (px[(unsigned long)y * ppr + x]) return kRcPixels;
        }
    return kRcEmpty;
}
bool rd3AccelResHasContent(IOAccelResource2 *res)
{
    return rd3AccelResContent(res) == kRcPixels;
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

/* the flip of a swap's buffer; why it was not done goes to the caller's
 * log line (swaps_flipfailed: RD3FLIPWHY's 20 lines were spent by the
 * first flips, review round 2); successes have their own 20 lines */
static unsigned rd3FlipOkLogs = 0;
/* the flip request for a legacy buffer (its vidmem, geometry), or why
 * there is none */
static bool rd3AccelFlipFill(IOAccelResource2 *res, IOService *fb, RD3FlipWhy *why, struct RD3KFlipRequest *f,
                             RD3VidMemory **vmOut)
{
    why->why = nullptr; why->ret = kIOReturnSuccess; why->flipResult = 0;
    if (!res) { why->why = "no resource"; return false; }
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a || !fb) { why->why = !a ? "no accelerator" : "no framebuffer for the head"; return false; }
    RD3VidMemory *vm = rd3res_vidmem(res);
    if (!vm || !vm->fRmHandle) { why->why = "the buffer has no RM VRAM to flip to"; return false; }
    if (vm->fScanout) { why->why = "the buffer is the framebuffer's own scanout"; return false; }

    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || !pitch) { why->why = "no geometry"; return false; }

    volatile unsigned char *z = (volatile unsigned char *)f;
    for (unsigned i = 0; i < sizeof *f; i++) z[i] = 0;
    f->version = RD3K_FLIP_ABI_VERSION;
    f->kapiMemory = vm->fRmHandle; f->allocationCookie = vm->fAllocationCookie;
    f->width = w; f->height = h; f->pitch = pitch;
    if (vmOut) *vmOut = vm;
    return true;
}
static bool rd3AccelFlipTo(IOAccelResource2 *res, IOService *fb, unsigned head, RD3FlipWhy *why)
{
    struct RD3KFlipRequest f;
    RD3VidMemory *vm = nullptr;
    if (!rd3AccelFlipFill(res, fb, why, &f, &vm)) return false;
    const unsigned w = f.width, h = f.height, pitch = f.pitch;

    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_FLIP_FN);
    if (!sym) { why->why = "OSSymbol allocation failed"; return false; }
    IOReturn rr = fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr);
    sym->release();
    if (rr != kIOReturnSuccess) {
        why->why = "the framebuffer refused it (a size other than its mode, or the core's flip failed)";
        why->ret = rr; why->flipResult = f.flipResult;
        return false;
    }
    if (rd3FlipOkLogs < 20) {
        rd3FlipOkLogs++;
        ALOG("rd3AccelFlipTo: *** FLIPPED *** head %u to %ux%u pitch %u kapiMemory %p (flipResult %d)", head, w, h,
             pitch, vm->fRmHandle, f.flipResult);
    }
    rd3AccelSampleFront(vm, w, h, pitch);
    return true;
}

/* ---- the flip latch and the buffers the panel showed (card run 2026-10-17) ----
 * The ~10th run of "windows and menus flicker on both screens": for a
 * moment a window shows, then the wallpaper where it was. rd3AccelFlipTo
 * programs the HUBP and returns; the flip latches at the next vblank, up to
 * a frame later, and until then the panel reads the buffer shown before -
 * the one WindowServer gets back from this swap and writes next (copy
 * forward, then the next frame). rdna3_flip_latch.h: the swap now waits for
 * the latch (amdgpu: the page-flip event comes from dm_pflip_high_irq, once
 * the flip latched), and every way a buffer on the panel can be written is
 * counted per head:
 *   - the previous flip still pending at a swap's entry, and how long flips
 *     take to latch (swaps_latchpend / _latchwait / _latchus / _latchmax,
 *     _latchimmediate for flips already latched when first read);
 *   - a copy forward into the buffer the panel still reads (cf_onpanel);
 *   - a shown buffer whose band hash at its swap differs from the one at the
 *     head's next swap: written while on the panel (swaps_torn, or
 *     swaps_inplace when the next swap shows it again: one back buffer);
 *   - a buffer freed while the head scans it (swaps_shownfreed);
 *   - an empty back read again 4 ms after its swap was skipped (sampled:
 *     swaps_emptyfilled / _emptystill: had the frame not landed yet?).
 * The wait sleeps in the swap, under the family's lock (the family hands
 * the old buffer back to WindowServer when submitSwapCopy returns); two
 * heads may take turns there. Completing the swap's IOAccelEvent from the
 * flip-done instead is a later redesign (DRIVER-PLAN N28).
 * The legacy buffers are written only by the family's page-on (the CPU,
 * through the aperture) and copy forward (SDMA, waited for: rdna3CopyVram),
 * both done before the swap is called; WindowServer's GPU composite reaches
 * them through the plugin's surface-out (vkQueueSubmit waited for, then a
 * read back into the IOSurface's system memory), so no GPU work writes a
 * legacy buffer at a swap: there is no fence to wait for there
 * (amdgpu_dm_commit_planes' dma_resv wait has no counterpart). Only an A3
 * display BO (rdna3a3display=1) would be GPU-written: swaps_a3_h<N>. */

/* why the head's display state could not be read (for the log lines) */
struct RD3StatusWhy { const char *why; IOReturn ret; };

/* the core's head number of a framebuffer, as RDNA3Framebuffer::start
 * finds its fHead: 0 on the PCI device, else its provider nub's
 * "RDNA3,Head" (read on every call: nothing cached across a framebuffer
 * that may go) */
static bool rd3AccelCoreHead(IOService *fb, unsigned *coreHead, RD3StatusWhy *w)
{
    IOService *p = fb ? fb->getProvider() : nullptr;
    if (!p) { w->why = "the framebuffer has no provider"; return false; }
    if (OSDynamicCast(IOPCIDevice, p)) { *coreHead = 0; return true; }
    OSNumber *num = OSDynamicCast(OSNumber, p->getProperty("RDNA3,Head"));
    if (!num) { w->why = "the framebuffer's provider has no RDNA3,Head"; return false; }
    *coreHead = num->unsigned32BitValue();
    return true;
}

/* the head's state from the core (dcn32_flip_pending through
 * rdna3DisplayStatus) */
static bool rd3AccelDispStatus(IOService *fb, unsigned head, RD3KDisplayStatus *st, RD3StatusWhy *w)
{
    w->why = nullptr; w->ret = kIOReturnSuccess;
    for (unsigned i = 0; i < sizeof *st; i++) ((volatile unsigned char *)st)[i] = 0;
    if (!fb || head >= kRd3MaxFB) { w->why = "no framebuffer for the head"; return false; }
    IOService *core = rd3AccelCore();
    if (!core) { w->why = "no core (RDNA3Accel)"; return false; }
    unsigned coreHead = 0;
    if (!rd3AccelCoreHead(fb, &coreHead, w)) return false;
    st->version = RD3K_DISPLAY_ABI_VERSION;
    st->head = coreHead;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_STATUS);
    if (!sym) { w->why = "no symbol"; return false; }
    w->ret = core->callPlatformFunction(sym, false, st, nullptr, nullptr, nullptr);
    sym->release();
    if (w->ret != kIOReturnSuccess) { w->why = "the core refused rdna3DisplayStatus"; return false; }
    return true;
}

/* the head's frame time from the core's measured refresh (rdna3DisplayInfo),
 * asked once per head and mode (0: unknown, rd3_latch_cap_us assumes 60 Hz) */
static uint32_t gLatchFrameUs[kRd3MaxFB];
static bool gLatchFrameAsked[kRd3MaxFB];      /* answered for this mode */
static unsigned gLatchFrameTries[kRd3MaxFB];  /* asks for this mode (rd3_latch_ask_frame) */
static uint64_t rd3AccelHeadFrameUs(IOService *fb, unsigned head)
{
    if (head >= kRd3MaxFB) return 0;
    if (!rd3_latch_ask_frame(gLatchFrameAsked[head], gLatchFrameTries[head])) return gLatchFrameUs[head];
    const unsigned tries = ++gLatchFrameTries[head];
    gLatchFrameUs[head] = 0;   /* never an older mode's answer */
    RD3StatusWhy w = { nullptr, kIOReturnSuccess };
    unsigned coreHead = 0;
    uint32_t mhz = 0;
    IOService *core = rd3AccelCore();
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_INFO);
    RD3KDisplayInfo *info = (RD3KDisplayInfo *)IOMalloc(sizeof *info);
    if (!core) w.why = "no core (RDNA3Accel)";
    else if (!sym) w.why = "no symbol";
    else if (!info) w.why = "no memory";
    else if (rd3AccelCoreHead(fb, &coreHead, &w)) {
        for (unsigned i = 0; i < sizeof *info; i++) ((volatile unsigned char *)info)[i] = 0;
        info->version = RD3K_DISPLAY_ABI_VERSION;
        w.ret = core->callPlatformFunction(sym, false, info, nullptr, nullptr, nullptr);
        if (w.ret != kIOReturnSuccess) w.why = "the core refused rdna3DisplayInfo";
        else if (coreHead >= info->nheads || coreHead >= RD3K_MAX_HEADS) w.why = "the core does not know the head";
        else if (!(mhz = info->head[coreHead].refreshMilliHz)) w.why = "the core has not measured its refresh";
    }
    if (info) IOFree(info, sizeof *info);
    if (sym) sym->release();
    gLatchFrameUs[head] = rd3_latch_frame_us(mhz);
    gLatchFrameAsked[head] = gLatchFrameUs[head] != 0;
    if (gLatchFrameAsked[head])
        ALOG("swap latch: head %u (core head %u): refresh %u mHz, frame %u us -> a swap waits at most %llu us after "
             "its flip", head, coreHead, mhz, gLatchFrameUs[head], rd3_latch_cap_us(gLatchFrameUs[head]));
    else
        ALOG("swap latch: head %u (core head %u): refresh unknown: %s (0x%x; ask %u of %u) -- 60 Hz assumed, a swap "
             "waits at most %llu us", head, coreHead, w.why ? w.why : "a refresh under 1 Hz", (unsigned)w.ret, tries,
             RD3_LATCH_FRAME_TRIES, rd3_latch_cap_us(0));
    return gLatchFrameUs[head];
}

static uint64_t rd3AccelUsSince(uint64_t abs0)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - abs0, &ns);
    return ns / 1000;
}

/* the flicker test's band hash of a buffer (8 bands, 48 columns x 2-4 rows
 * through the BAR): nullptr when hashed, else why not */
static const char *rd3AccelBandHash(IOAccelResource2 *res, uint32_t *hash)
{
    RD3VidMemory *vm = rd3res_vidmem(res);
    const unsigned char *r = (const unsigned char *)res;
    if (!res) return "no resource";
    const unsigned w = *(const volatile unsigned short *)(r + 0xb0), h = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int *)(r + 0xb8);
    const char *skip = !vm ? "no vidmem" : !vm->fKva ? "no kernel mapping of its VRAM" : !w || !h ? "no geometry"
                     : pitch < 4u * w ? "pitch under 4 x width"
                     : (unsigned long long)h * pitch > vm->fBytes ? "geometry past its allocation" : nullptr;
    if (skip) return skip;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
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
    return nullptr;
}

/* the torn test's budget: checks and skips both count (a head whose
 * checks keep being skipped must not hash forever); said once per head */
static bool rd3AccelTornWants(unsigned head)
{
    if (head >= kRd3MaxFB || !gSwapTorn) return false;
    if (gSwapTorn != 1 || gCntTornChk[head] + gCntTornSkip[head] < kTornBudget) return true;
    static bool said[kRd3MaxFB];
    if (!said[head]) {
        said[head] = true;
        ALOG("swap latch: head %u: the torn test stopped after %d checks (%lld checked, %lld torn, %lld in place, %lld "
             "skipped); debug.rdna3accel_swap_torn=2 keeps it on", head, (int)kTornBudget, (long long)gCntTornChk[head],
             (long long)gCntTorn[head], (long long)gCntInplace[head], (long long)gCntTornSkip[head]);
    }
    return false;
}

/* a transition of the head's waits (rd3_latch_state): logged (the first 8
 * per head, then every 256th: a flapping head cannot flood the log), and
 * the state in swaps_latchwaitsoff_h<N>; 'rearm': a forget (mode change,
 * flip refused), not a latch seen */
static unsigned gLatchEvLogs[kRd3MaxFB];
static void rd3AccelLatchEvent(unsigned head, enum rd3_latch_event e, const char *why, bool rearm = false)
{
    if (e == RD3_LATCH_EV_NONE || head >= kRd3MaxFB) return;
    gCntLatchWaitsOff[head] = e == RD3_LATCH_EV_OFF ? 1 : 0;
    if (!rd3_log_due(gLatchEvLogs[head]++, 8, 255)) return;
    if (e == RD3_LATCH_EV_OFF)
        ALOG("swap latch: head %u: %u flips in a row did not latch -- the head's swaps STOP WAITING for their flips "
             "until one is seen latched (the old buffer may be on the panel again)", head, RD3_LATCH_TIMEOUT_RUN);
    else if (rearm)
        ALOG("swap latch: head %u: the waits re-armed (%s) -- the head's swaps wait for their flips again", head, why);
    else
        ALOG("swap latch: head %u: a flip was seen latched (%s) -- the head's swaps wait for their flips again", head,
             why);
}

/* the log budget of each place that reads the display state */
enum { kUnkEntry = 0, kUnkWait, kUnkFlip, kUnkCf, kUnkSites };   /* the swap's entry, the wait, after the flip, copy forward */
static unsigned gLatchUnkLogs[kUnkSites];
static void rd3AccelLatchUnknown(unsigned head, unsigned site, const RD3StatusWhy &w, unsigned n, const char *what)
{
    const SInt64 k = OSAddAtomic64(1, &gCntLatchUnknown[head]);
    if (site < kUnkSites && rd3_log_due(gLatchUnkLogs[site]++, 4, 1023))
        ALOG("swap #%u: head %u: the flip state could not be read %s: %s (0x%x) -- %s (%lld such)", n, head,
             site == kUnkEntry ? "at the swap's entry" : site == kUnkWait ? "while waiting"
             : site == kUnkFlip ? "after the flip" : "for copy forward", w.why ? w.why : "?", (unsigned)w.ret, what,
             (long long)k + 1);
}

/* the panel reads nothing of a recorded buffer any more (a mode switch, a
 * copy onto the scanout instead of a flip): every wait armed again */
static void rd3AccelLatchForget(unsigned head, const char *why)
{
    if (head >= kRd3MaxFB) return;
    RD3HeadLatch &L = gLatch[head];
    L.vm = nullptr; L.prevVm = nullptr; L.phys = L.prevPhys = 0; L.hashed = false;
    rd3AccelLatchEvent(head, rd3_latch_rearm(&L.st), why, true);
}
/* every head's latch record forgotten (rd3AccelPresentForgetAll calls it under gPresLock: the present
 * queue's worker writes the same records, rd3AccelLatchProgrammed) */
static void rd3AccelLatchForgetHeads(const char *why)
{
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        gLatchFrameAsked[h] = false;   /* a new mode: its refresh asked again */
        gLatchFrameTries[h] = 0;
        if (gLatch[h].vm || gLatch[h].prevVm)
            ALOG("swap latch: head %u: the record of phys 0x%llx forgotten (%s)", h, gLatch[h].phys, why);
        rd3AccelLatchForget(h, why);
    }
}
/* rd3AccelLatchForget outside the present queue's own paths (a flip refused, the SDMA breaker opening, a swap
 * mode change): under gPresLock like rd3AccelPresentForgetAll, since the queue's worker rewrites the same record
 * (rd3AccelLatchProgrammed) and a forget between its reads and writes left a half-forgotten record behind - only
 * counter and log noise, but noise in the data the card runs are read from (ECC review round 2 of N38). The
 * caller holds no gPresLock (the order is the family's lock, then gPresLock) and no spinlock */
static void rd3AccelLatchForgetPres(unsigned head, const char *why)
{
    IOLock *l = gPresReady ? gPresLock : nullptr;
    if (l) IOLockLock(l);
    rd3AccelLatchForget(head, why);
    if (l) IOLockUnlock(l);
}
static void rd3AccelLatchForgetAll(const char *why)
{
    rd3AccelPresentForgetAll(why);   /* the queued presents were meant for a picture that is gone (N38), then the records */
}

/* a flip seen latched. 'waited': a swap slept for it (its time counts in
 * swaps_latchwait / _latchus / _latchmax); 'immediate': already latched at
 * the first look after the flip (swaps_latchimmediate); else seen later
 * (the next swap's entry, copy forward: not timed) */
enum { kSeenLater = 0, kSeenWaited, kSeenImmediate };
static void rd3AccelLatchSeen(unsigned head, RD3HeadLatch &L, unsigned how, const char *where)
{
    const uint64_t us = rd3AccelUsSince(L.flipAbs);
    rd3AccelLatchEvent(head, rd3_latch_seen(&L.st), where);
    if (how == kSeenLater) return;
    L.latchUs = us > 0xffffffffull ? 0xffffffffu : (uint32_t)us;
    if (how == kSeenImmediate) { OSAddAtomic64(1, &gCntLatchImmediate[head]); return; }
    OSAddAtomic64(1, &gCntLatchWait[head]);
    OSAddAtomic64((SInt64)us, &gCntLatchUs[head]);
    if ((SInt64)us > gCntLatchMax[head]) gCntLatchMax[head] = (SInt64)us;   /* one writer: the head's swaps */
}

/* wait (1 ms sleeps, no spinlock held: the swap path already takes IOLocks
 * in flipToLocked) until the head's last flip latched, as rd3_latch_step
 * says: from the flip only, at most 'cap' (ECC review round 2: a budget
 * shared with the swap's work before the flip turned stuck flips into
 * "budget spent" and the head never stopped waiting) */
static void rd3AccelLatchWait(unsigned head, IOService *fb, uint32_t frame0, uint64_t cap, unsigned n)
{
    RD3HeadLatch &L = gLatch[head];
    for (;;) {
        IOSleep(1);
        RD3KDisplayStatus st;
        RD3StatusWhy w;
        const bool ok = rd3AccelDispStatus(fb, head, &st, &w);
        const enum rd3_latch_step step = rd3_latch_step(ok, ok && st.flipPending, frame0, ok ? st.frame : frame0,
                                                        rd3AccelUsSince(L.flipAbs), cap);
        switch (step) {
        case RD3_LATCH_WAIT:
            continue;
        case RD3_LATCH_DONE:
            rd3AccelLatchSeen(head, L, kSeenWaited, "after the flip");
            return;
        case RD3_LATCH_UNKNOWN:
            rd3_latch_give_up(&L.st);
            rd3AccelLatchUnknown(head, kUnkWait, w, n, "not waited further");
            return;
        case RD3_LATCH_TIMEOUT:
        default: {
            const enum rd3_latch_event e = rd3_latch_timeout(&L.st);
            const SInt64 k = OSAddAtomic64(1, &gCntLatchTimeout[head]);
            if (rd3_log_due((uint64_t)k, 8, 1023))
                ALOG("swap #%u: head %u: the flip to phys 0x%llx had NOT latched %llu us after it (frame %u -> %u, "
                     "limit %llu us) -- the panel may still read phys 0x%llx; %u timeouts in a row (%lld such)", n,
                     head, L.phys, rd3AccelUsSince(L.flipAbs), frame0, ok ? st.frame : frame0, cap, L.prevPhys,
                     L.st.run, (long long)k + 1);
            rd3AccelLatchEvent(head, e, "after the flip");
            return;
        }
        }
    }
}

/* (a) at a swap's entry: the head's previous flip still pending? Then the
 * panel reads the buffer before it, the one WindowServer had back since
 * that swap. Only looked at and counted: the swap waited after that flip
 * already (or did not: swap_latch=0, waits off, unreadable), and
 * flipToLocked waits for it before the next flip as before. A flip seen
 * latched here ends a run of timeouts. */
static void rd3AccelLatchEntry(unsigned head, IOService *fb, unsigned n)
{
    RD3HeadLatch &L = gLatch[head];
    if (!L.phys || L.st.latched) return;
    RD3KDisplayStatus st;
    RD3StatusWhy w;
    if (!rd3AccelDispStatus(fb, head, &st, &w)) {
        rd3_latch_give_up(&L.st);
        rd3AccelLatchUnknown(head, kUnkEntry, w, n, "whether the previous flip latched is unknown");
        return;
    }
    if (!st.flipPending) { rd3AccelLatchSeen(head, L, kSeenLater, "at the next swap"); return; }
    const SInt64 k = OSAddAtomic64(1, &gCntLatchPend[head]);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("swap #%u: head %u: the previous flip (to phys 0x%llx, %llu us ago) had NOT latched at this swap -- the "
             "panel still read phys 0x%llx, which WindowServer had back since that swap (%lld such; %s)", n, head,
             L.phys, rd3AccelUsSince(L.flipAbs), L.prevPhys, (long long)k + 1,
             !gSwapLatch ? "debug.rdna3accel_swap_latch=0" : L.st.off ? "the head's waits are off"
                                                           : "its wait gave up");
}

/* (c) the buffer the head's last swap showed, hashed again now: changed
 * while on the panel? 'src' / 'srcHash' the buffer this swap shows (its
 * hash when taken), 'bufs' the swapping surface's own buffers (a buffer is
 * read only through one of them, alive) */
static void rd3AccelTornSkip(unsigned head, unsigned long long phys, const char *why, unsigned n)
{
    const SInt64 k = OSAddAtomic64(1, &gCntTornSkip[head]);
    if (rd3_log_due((uint64_t)k, 4, 1023))
        ALOG("swap #%u: head %u: the torn test skipped phys 0x%llx: %s (%lld such)", n, head, phys, why,
             (long long)k + 1);
}
static void rd3AccelTornCheck(unsigned head, IOAccelResource2 *src, const uint32_t *srcHash,
                              IOAccelResource2 *const *bufs, unsigned nbufs, unsigned n)
{
    RD3HeadLatch &L = gLatch[head];
    if (!L.hashed) return;
    L.hashed = false;
    if (!rd3AccelTornWants(head)) return;
    const void *vm = L.vm;
    RD3VidMemory *sv = rd3res_vidmem(src);
    const bool same = vm && (const void *)sv == vm && sv->fAllocationCookie == L.cookie;
    uint32_t now[kFlickBands];
    const char *skip = nullptr;
    if (same && srcHash) {
        for (unsigned b = 0; b < kFlickBands; b++) now[b] = srcHash[b];
    } else {
        IOAccelResource2 *own = same ? src : vm ? rd3AccelCfOwn(bufs, nbufs, vm, nullptr) : nullptr;
        const RD3VidMemory *ov = rd3res_vidmem(own);
        skip = !vm ? "freed" : !own ? "not one of the swapping surface's buffers"
             : !ov || ov->fAllocationCookie != L.cookie ? "re-allocated" : rd3AccelBandHash(own, now);
    }
    if (skip) { rd3AccelTornSkip(head, L.phys, skip, n); return; }
    OSAddAtomic64(1, &gCntTornChk[head]);
    const unsigned changed = rd3_torn_bands(L.hash, now, kFlickBands);
    const enum rd3_torn_kind kind = rd3_torn_classify(changed, same);
    if (kind == RD3_TORN_NONE) return;
    SInt64 *cnt = kind == RD3_TORN_SHOWN ? &gCntTorn[head] : &gCntInplace[head];
    const SInt64 k = OSAddAtomic64(1, cnt);
    if (!rd3_log_due((uint64_t)k, 8, 1023)) return;
    char lat[48];
    if (L.latchUs) snprintf(lat, sizeof lat, "latched %u us after it", L.latchUs);
    else snprintf(lat, sizeof lat, "%s", L.st.latched ? "latched (time not seen)" : "NOT seen latched");
    ALOG("swap #%u: head %u: the buffer it showed (phys 0x%llx) changed while on the panel: bands 0x%02x of %u "
         "(bit 0 the top 32 rows); its flip %s, %llu ms ago -- %s (%lld such, %lld checked)", n, head, L.phys, changed,
         (unsigned)kFlickBands, lat, rd3AccelUsSince(L.flipAbs) / 1000,
         kind == RD3_TORN_SHOWN ? "WRITTEN WHILE SHOWN, now flipped away (torn: the panel showed the writes)"
                                : "rewritten in place and shown again (one back buffer: the panel reads it while "
                                  "WindowServer draws)",
         (long long)k + 1, (long long)gCntTornChk[head]);
}

/* after a flip of 'res' (phys) on the head: the record, and the swap's one
 * wait for the latch (the fix, debug.rdna3accel_swap_latch) */
static void rd3AccelLatchFlipped(unsigned head, IOService *fb, IOAccelResource2 *res, unsigned long long phys,
                                 const uint32_t *hash, const char *hashSkip, uint64_t cap, unsigned n)
{
    RD3HeadLatch &L = gLatch[head];
    RD3VidMemory *vm = rd3res_vidmem(res);
    L.prevVm = L.vm; L.prevPhys = L.phys;
    L.vm = vm; L.phys = phys; L.cookie = vm ? vm->fAllocationCookie : 0;
    L.flipAbs = mach_absolute_time();
    L.latchUs = 0;
    rd3_latch_flipped(&L.st);
    const bool torn = rd3AccelTornWants(head);
    L.hashed = hash && torn;
    for (unsigned b = 0; L.hashed && b < kFlickBands; b++) L.hash[b] = hash[b];
    if (torn && !hash) rd3AccelTornSkip(head, phys, hashSkip ? hashSkip : "not hashed at its swap", n);
    RD3KDisplayStatus st;
    RD3StatusWhy w;
    if (!rd3AccelDispStatus(fb, head, &st, &w)) {
        rd3_latch_give_up(&L.st);
        rd3AccelLatchUnknown(head, kUnkFlip, w, n, "not waited for");
        return;
    }
    if (!st.flipPending) { rd3AccelLatchSeen(head, L, kSeenImmediate, "right after the flip"); return; }
    if (!rd3_latch_wait_flip(gSwapLatch, &L.st)) {
        if (gSwapLatch) OSAddAtomic64(1, &gCntLatchOff[head]);
        rd3_latch_give_up(&L.st);
        return;   /* the next swap's entry looks; flipToLocked waits */
    }
    rd3AccelLatchWait(head, fb, st.frame, cap, n);
}

/* a mode 0 swap whose back is the buffer the head already scans, its flip
 * latched (rd3_latch_same_shown, card run 2026-10-18b): no register write,
 * no wait. The record stays (same buffer, same flip); a latch seen only by
 * the scanned address is recorded; the torn test's hash is taken again so
 * the next swap still tells in-place rewrites apart (swaps_inplace) */
static void rd3AccelLatchSame(unsigned head, IOAccelResource2 *res, unsigned long long phys, const uint32_t *hash,
                              const char *hashSkip, bool scanKnown, unsigned n)
{
    RD3HeadLatch &L = gLatch[head];
    if (!L.st.latched) rd3AccelLatchSeen(head, L, kSeenLater, "a swap to the buffer on the panel");
    RD3VidMemory *vm = rd3res_vidmem(res);
    L.vm = vm;
    L.cookie = vm ? vm->fAllocationCookie : 0;
    const bool torn = rd3AccelTornWants(head);
    L.hashed = hash && torn;
    for (unsigned b = 0; L.hashed && b < kFlickBands; b++) L.hash[b] = hash[b];
    if (torn && !hash) rd3AccelTornSkip(head, phys, hashSkip ? hashSkip : "not hashed at its swap", n);
    const SInt64 k = OSAddAtomic64(1, &gCntLatchSame[head]);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("swap #%u: head %u: the back (phys 0x%llx) is the buffer the panel already scans -- not flipped again, "
             "no latch wait (%lld such; %s; debug.rdna3accel_swap_samenoflip=0 flips it)", n, head, phys,
             (long long)k + 1, scanKnown ? "the HUBP's address read" : "its flip seen latched");
}

/* (b) copy forward's target, still on the panel? Only the buffer the last
 * flip left matters (the shown buffer as target is copy forward's one-back
 * case, nothing copied); its flip state is read when not seen latched */
static void rd3AccelCfPanelCheck(unsigned head, const RD3VidMemory *dst, unsigned n)
{
    if (head >= kRd3MaxFB || !dst) return;
    RD3HeadLatch &L = gLatch[head];
    if (L.st.latched || !L.prevVm || L.prevVm != (const void *)dst) return;
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    RD3KDisplayStatus st;
    RD3StatusWhy w = { "no accelerator", kIOReturnSuccess };
    if (!a || !rd3AccelDispStatus(a->fFBs[head], head, &st, &w)) {
        rd3AccelLatchUnknown(head, kUnkCf, w, n, "copy forward's target not checked");
        return;
    }
    if (rd3_latch_panel(L.phys, L.prevPhys, L.st.latched, st.flipPending, dst->fPhys) != RD3_PANEL_LEAVING) {
        if (!st.flipPending) rd3AccelLatchSeen(head, L, kSeenLater, "by copy forward");
        return;
    }
    const SInt64 k = OSAddAtomic64(1, &gCntCfOnPanel[head]);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("copy forward #%u: head %u: the target (phys 0x%llx) is STILL ON THE PANEL -- the flip to phys 0x%llx "
             "(%llu us ago) had not latched: WindowServer writes into the buffer the HUBP reads (%lld such; "
             "debug.rdna3accel_swap_latch=%d)", n, head, dst->fPhys, L.phys, rd3AccelUsSince(L.flipAbs),
             (long long)k + 1, gSwapLatch);
}

/* a vidmem freed (deallocPhysical, any thread) while a head scans it */
/* vmToo false: only a record of exactly the address 'vmPhys' (N41: an older park of the vidmem released) */
static void rd3AccelLatchVidmemGone(const void *vm, unsigned long long vmPhys, bool vmToo)
{
    for (unsigned h = 0; vm && h < kRd3MaxFB; h++) {
        RD3HeadLatch &L = gLatch[h];
        const bool shown = L.vm == vm && (vmToo || L.phys == vmPhys),
                   leaving = !shown && L.prevVm == vm && (vmToo || L.prevPhys == vmPhys) && !L.st.latched;
        if (!shown && !leaving) continue;
        const unsigned long long phys = shown ? L.phys : L.prevPhys;
        if (shown) { L.vm = nullptr; L.hashed = false; }
        else L.prevVm = nullptr;
        const SInt64 k = OSAddAtomic64(1, &gCntShownFreed[h]);
        if (rd3_log_due((uint64_t)k, 8, 1023))
            ALOG("swap: head %u: the buffer %s (phys 0x%llx) was FREED while the head %s it -- its VRAM may be "
                 "reused under the panel (%lld such)", h, shown ? "it shows" : "its last flip left", phys,
                 shown ? "scans" : "still read (the flip away had not latched)", (long long)k + 1);
    }
}

/* ---- the present queue (DRIVER-PLAN N38, common/rdna3_flip_queue.h) ----
 * Card run 2026-10-18b: a mode 0 swap slept for its flip's latch (median
 * 14.8 ms) under the family's lock on WindowServer's thread; frames came
 * every third vblank. amdgpu's model instead: amdgpu_dm_commit_planes
 * programs the flip and returns, dm_pflip_high_irq completes the page-flip
 * event once the HUBP latched it, one flip in flight per CRTC. Here:
 *   - a present (a mode 0 swap, a flip-buffer present, an IOP transaction's
 *     zero-copy flip) with nothing in flight is programmed at once with
 *     RD3K_FLIP_FLAG_NOWAIT and the presenter returns;
 *   - with a flip in flight it waits in the head's one slot (a newer present
 *     replaces it: newest wins); the framebuffer answering Busy (another
 *     path's flip) keeps it there too;
 *   - the head's worker (a thread call, off every lock) sleeps on the
 *     framebuffer's latch wait (woken by its VBL thread; an older
 *     framebuffer: 1 ms polls of the core's status), sees the latch, then
 *     completes the in-flight present (an IOP transaction:
 *     signalTransactionInterrupt) and programs the queued one;
 *   - a legacy swap has no event of its own to complete later (the family's
 *     IOAccelEvent, submitSwapCopy's a0, is completed by the family's stamp
 *     machinery, RD3EventMachine::writeStamp writing the stamp page at once):
 *     what WindowServer may write after the swap returns is the buffer leaving
 *     the panel, and the writers the driver sees - copy forward's target, the
 *     family's page-on (addToAperture, getApertureMemoryDescriptor) - wait for
 *     the latch only when they target that buffer (rd3_fq_reuse LEAVING):
 *     the reuse point where userspace waits for the page-flip event under
 *     amdgpu. A buffer written while it stays on the panel (one back buffer)
 *     is counted, never waited for.
 * Every outcome is counted (debug.rdna3accel_swaps_pq_*_h<N>) and traced
 * (PRESENT events). Lock order: the family's lock, then gPresLock, then the
 * framebuffer's mode lock (the flip); the worker takes only gPresLock; IOP
 * surface references are dropped and pipes signalled after it is released. */
/* what a head's HUBP scans (rd3AccelTracePanel, below) */
struct RD3PanelScan { bool known, pending; unsigned long long scan; };
static void rd3AccelTracePanel(unsigned head, IOService *fb, unsigned long long expect, RD3PanelScan *out);
enum { kPqSwap = RD3_TRPK_SWAP, kPqFlipbuf = RD3_TRPK_FLIPBUF, kPqIop = RD3_TRPK_IOP, kPqKinds = 3 };
static const char *const kPqName[kPqKinds] = { "a mode 0 swap", "a flip-buffer present", "an IOP transaction" };
enum { kPqOutShown = 0, kPqOutQueued = 1, kPqOutReplaced = 2, kPqOutJoined = 3, kPqOutRefused = 4,
       kPqOutSame = 5 /* the buffer the head scans, nothing in flight or queued: not flipped (rd3_fq_present) */ };
struct RD3PresReq {
    struct RD3KFlipRequest f;
    const void *vm;                    /* compare-only: the free path drops a queued present of it */
    unsigned long long phys, cookie;
    IOSurface *surf;                   /* IOP: a reference while queued, the scanned record's from its flip on */
    void *volatile pipe;               /* IOP: the pipe whose transaction completes with it */
    uint32_t tok;
    uint32_t hash[kFlickBands];        /* the band hash at the swap (the torn test), when 'hashed' */
    bool hashed;
    uint8_t kind;
};
struct RD3Pres {
    struct rd3_fq q;
    RD3PresReq cur, next;              /* the in-flight present (q.inflight), the queued one (q.queued) */
    IOService *fb;                     /* the head's framebuffer (fFBs[head], held by the accelerator) */
    struct thread_call *call;
    bool running;
    volatile uint32_t gen;             /* bumped at each programming and latch: a wait for an older flip is stale */
    uint64_t progAbs, capUs;
    uint32_t frame0;
    bool frameKnown;
};
static RD3Pres gPres[kRd3MaxFB];
static void rd3AccelPresentRunTc(void *p0, void *);
static int gPresFbWait[kRd3MaxFB] = { -1, -1, -1, -1 };   /* the framebuffer has rdna3FlipLatchWait: 1 yes, 0 no */
/* N42: the latch wait's version the framebuffer answers (2: with the pflip interrupt's flags, 1: an older one) */
static unsigned gPresFbWaitVer[kRd3MaxFB] = { RD3K_FLIPWAIT_VERSION, RD3K_FLIPWAIT_VERSION, RD3K_FLIPWAIT_VERSION,
                                              RD3K_FLIPWAIT_VERSION };
/* N42: latches the framebuffer saw ended by the head's pflip interrupt (flip done), the interrupt -> wake time
 * summed / longest, whether the head's pflip interrupt is live (the last wait's flag), a v1-only framebuffer */
static SInt64 gCntPqIrqDone[kRd3MaxFB], gCntPqIrqLatUs[kRd3MaxFB], gCntPqIrqLatMax[kRd3MaxFB];
static SInt64 gCntPqIrqLive[kRd3MaxFB], gCntPqFbWaitV1[kRd3MaxFB];
static SInt64 gCntPqProg[kRd3MaxFB], gCntPqQueued[kRd3MaxFB], gCntPqReplaced[kRd3MaxFB], gCntPqJoined[kRd3MaxFB];
static SInt64 gCntPqBusy[kRd3MaxFB], gCntPqRefused[kRd3MaxFB], gCntPqLatched[kRd3MaxFB], gCntPqLatchUs[kRd3MaxFB];
static SInt64 gCntPqLatchMax[kRd3MaxFB], gCntPqNext[kRd3MaxFB], gCntPqTimeout[kRd3MaxFB], gCntPqUnknown[kRd3MaxFB];
static SInt64 gCntPqDropped[kRd3MaxFB], gCntPqEarly[kRd3MaxFB], gCntPqWait[kRd3MaxFB], gCntPqWaitUs[kRd3MaxFB];
static SInt64 gCntPqWaitMax[kRd3MaxFB], gCntPqWaitTimeout[kRd3MaxFB], gCntPqInplace[kRd3MaxFB], gCntPqInline[kRd3MaxFB];
static SInt64 gCntPqPolled[kRd3MaxFB], gCntPqIop[kRd3MaxFB], gCntPqDropMode[kRd3MaxFB];
/* ECC review of N38: a head whose flips stay pending (another path's flip given up, the queued present flipped
 * anyway), IOP transactions completed without a flip of their own, the framebuffer's latch waits, a buffer
 * freed while its flip was in flight, the IOP copy fallback's flips home */
static SInt64 gCntPqForeignTo[kRd3MaxFB], gCntPqGiveup[kRd3MaxFB], gCntPqIopDrop[kRd3MaxFB];
static SInt64 gCntPqFbWaitTo[kRd3MaxFB], gCntPqFbWaitFast[kRd3MaxFB], gCntPqFreedInflight[kRd3MaxFB];
static SInt64 gCntPqHome[kRd3MaxFB], gCntPqDoneMax;
static unsigned gPqLogs[kRd3MaxFB], gPqEarlyLogs, gPqDropLogs, gPqWaitLogs, gPqRefLogs;
static unsigned gPqStuckLogs, gPqIopDropLogs, gPqFastLogs, gPqHomeLogs, gPqFreedLogs;
static bool gPresStopping;               /* rd3AccelPresentStop: no worker armed, the waiters leave */

/* what a lock holder hands out once unlocked: IOP pipes to signal, IOSurface references to drop */
enum { kPqDoneMax = 16 };
struct RD3PresDone { void *pipe[kPqDoneMax]; IOSurface *surf[kPqDoneMax]; unsigned npipe, nsurf; };
static void rd3AccelPqDonePipe(RD3PresDone *d, void *pipe)
{
    if (!pipe) return;
    for (unsigned i = 0; i < d->npipe; i++) if (d->pipe[i] == pipe) return;
    if (d->npipe < kPqDoneMax) { d->pipe[d->npipe++] = pipe; return; }
    /* never expected (one pipe per head): its transaction then completes at that pipe's next signal */
    const SInt64 k = OSAddAtomic64(1, &gCntPqDoneMax);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("present queue: more than %u IOP pipes to signal at once -- pipe %p NOT signalled now, its transaction "
             "waits for the next signal (%lld such; debug.rdna3accel_swaps_pq_donemax)", (unsigned)kPqDoneMax, pipe,
             (long long)k + 1);
}
static void rd3AccelPqDoneSurf(RD3PresDone *d, IOSurface *s)
{
    if (!s) return;
    if (d->nsurf < kPqDoneMax) { d->surf[d->nsurf++] = s; return; }
    const SInt64 k = OSAddAtomic64(1, &gCntPqDoneMax);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("present queue: more than %u surface references to drop at once -- one LEAKED (never expected; %lld "
             "such; debug.rdna3accel_swaps_pq_donemax)", (unsigned)kPqDoneMax, (long long)k + 1);
}
static void rd3AccelPqDoneRun(RD3PresDone *d)
{
    for (unsigned i = 0; i < d->nsurf; i++) ((OSObject *)d->surf[i])->release();
    for (unsigned i = 0; i < d->npipe; i++) _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(d->pipe[i], nullptr);
    d->nsurf = d->npipe = 0;
}
static inline void rd3AccelPqZero(RD3PresReq *r)
{
    volatile unsigned char *z = (volatile unsigned char *)r;
    for (unsigned i = 0; i < sizeof *r; i++) z[i] = 0;
}

/* a PRESENT event (pb / pc: the queue after it) */
static void rd3AccelPqTrace(unsigned head, unsigned arg, unsigned flags, uint32_t tok, uint64_t us, uint32_t tok2,
                            int flipResult, IOReturn rr, unsigned long long pa)
{
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_PRESENT, head);
    e.arg = (uint8_t)arg;
    e.flags = (uint8_t)flags;
    e.v0 = tok;
    e.v1 = rd3TrSat(us);
    e.v2 = tok2;
    e.v3 = (uint32_t)flipResult;
    e.v4 = (uint32_t)rr;
    e.pa = pa;
    if (head < kRd3MaxFB) {
        e.pb = gPres[head].q.inflight;
        e.pc = gPres[head].q.queued;
    }
    rd3Trace(e);
}

static bool rd3AccelPresentInit(void)
{
    if (gPresReady) return true;
    if (!rd3LockOnce(&gPresLock)) return false;
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        if (!gPres[h].call) gPres[h].call = thread_call_allocate(rd3AccelPresentRunTc, (void *)(uintptr_t)h);
        if (!gPres[h].call) return false;
        rd3_fq_init(&gPres[h].q);
        gPres[h].running = false;
    }
    IOLockLock(gPresLock);
    gPresStopping = false;   /* a start after a stop */
    IOLockUnlock(gPresLock);
    gPresReady = true;
    return true;
}

/* the gLatch record of a legacy present just programmed (the torn test and
 * the PANEL / LATCH lines read it; under gPresLock) */
static void rd3AccelLatchProgrammed(unsigned head, const RD3PresReq &R, uint64_t issued)
{
    RD3HeadLatch &L = gLatch[head];
    L.prevVm = L.vm; L.prevPhys = L.phys;
    L.vm = R.vm; L.phys = R.phys; L.cookie = R.cookie;
    L.flipAbs = issued;
    L.latchUs = 0;
    rd3_latch_flipped(&L.st);
    L.hashed = R.hashed;
    for (unsigned b = 0; L.hashed && b < kFlickBands; b++) L.hash[b] = R.hash[b];
}

/* program P.cur (its address is q.inflight): 0 done, 1 Busy, 2 refused
 * (*rr / *fr the framebuffer's answer). 'force' (rd3_fq_verdict: the head's
 * flips stay pending): without RD3K_FLIP_FLAG_NOWAIT, the framebuffer waits
 * at most 20 ms for the pending flip and flips anyway. Under gPresLock */
enum { kPqProgOk = 0, kPqProgBusy = 1, kPqProgRefused = 2 };
static unsigned rd3AccelPqProgramLocked(unsigned head, RD3PresDone *d, IOReturn *rr, int *fr, bool force = false)
{
    RD3Pres &P = gPres[head];
    RD3PresReq &R = P.cur;
    IOService *fb = P.fb;
    if (force) R.f.flags &= ~RD3K_FLIP_FLAG_NOWAIT;
    else R.f.flags |= RD3K_FLIP_FLAG_NOWAIT;
    R.f.flipResult = 0;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_FLIP_FN);
    const uint64_t issued = mach_absolute_time();
    *rr = sym && fb ? fb->callPlatformFunction(sym, false, &R.f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    *fr = R.f.flipResult;
    struct rd3_trace_ev fe = rd3TrEv(RD3_TR_FLIP, head);
    fe.arg = 6;   /* the present queue's flip */
    fe.flags = *rr == kIOReturnSuccess ? 1 : 0;
    fe.v0 = (uint32_t)R.f.flipResult;
    fe.v4 = (uint32_t)*rr;
    fe.pa = R.phys;
    rd3Trace(fe);
    if (*rr == kIOReturnBusy) return kPqProgBusy;
    if (*rr != kIOReturnSuccess) return kPqProgRefused;
    P.progAbs = issued;
    P.gen++;
    RD3KDisplayStatus st;
    RD3StatusWhy w;
    P.frameKnown = rd3AccelDispStatus(fb, head, &st, &w);
    P.frame0 = P.frameKnown ? st.frame : 0;
    if (R.kind == kPqIop) {
        /* the scanned IOSurface stays referenced while the HUBP may read it: the queue's reference moves to
         * the front record, the one before it (leaving) is dropped at the next flip (under gPresLock, as on
         * every path: rd3AccelIopFrontMove) */
        rd3AccelScPanelMoved(head, "an IOP flip", issued, true);
        if (gIopFlipPrev[head]) rd3AccelPqDoneSurf(d, gIopFlipPrev[head]);
        gIopFlipPrev[head] = gIopFlipFront[head];
        gIopFlipFront[head] = R.surf;
        gIopFlipped[head] = true;
        R.surf = nullptr;
        const SInt64 n = OSAddAtomic64(1, &gCntIopFlip);
        OSAddAtomic64(1, &gCntIopAsync);
        if (n < 4 || n == 600 || (n & 0xffff) == 0)
            ALOG("iop: queued swap #%lld head %u: %ux%u pitch %u scanned out of its own VRAM (flipResult %d)",
                 (long long)n + 1, head, R.f.width, R.f.height, R.f.pitch, R.f.flipResult);
    } else {
        rd3AccelScPanelMoved(head, R.kind == kPqSwap ? "a mode 0 flip" : "a flip-buffer present", issued, false);
        rd3AccelLatchProgrammed(head, R, issued);
    }
    return kPqProgOk;
}

/* a present leaves the queue (its flip latched or given up, or it never reached the panel): its IOP surface
 * reference dropped, its transaction ended (the pipe signalled once the lock is released) */
static void rd3AccelPqLost(RD3PresDone *d, RD3PresReq *r)
{
    if (r->surf) rd3AccelPqDoneSurf(d, r->surf);
    if (r->kind == kPqIop) rd3AccelPqDonePipe(d, r->pipe);
    rd3AccelPqZero(r);
}

/* the framebuffer answered Busy to P.cur (q.inflight: another path's flip, or one of ours given up, still
 * pending): kept behind that flip (rd3_fq_busy), the worker waits for its latch from now. Under gPresLock */
static void rd3AccelPqBusyLocked(unsigned head, RD3PresDone *d)
{
    RD3Pres &P = gPres[head];
    uint32_t dropped = 0;
    OSAddAtomic64(1, &gCntPqBusy[head]);
    if (rd3_fq_busy(&P.q, &dropped)) {
        rd3AccelPqLost(d, &P.cur);   /* a newer present queued meanwhile wins (never from today's callers) */
    } else {
        P.next = P.cur;
        rd3AccelPqZero(&P.cur);
    }
    P.progAbs = mach_absolute_time();
    P.frameKnown = false;
    P.gen++;
}

/* the in-flight flip latched (step RD3_LATCH_DONE) or was given up
 * (timeout, unknown: its completion is early): complete it, program the
 * queued present (rd3_fq_verdict: without NOWAIT once the head's flips
 * stayed pending RD3_FQ_STUCK_RUN times in a row). Under gPresLock */
static void rd3AccelPqLatchedLocked(unsigned head, enum rd3_latch_step step, uint64_t us, bool inl, RD3PresDone *d,
                                    bool irq = false, uint32_t irqLatUs = 0)
{
    RD3Pres &P = gPres[head];
    int had = 0, fg = 0, force = 0;
    uint32_t tok = 0, ntok = 0;
    uint64_t na = 0;
    const bool early = step != RD3_LATCH_DONE;
    const unsigned long long was = P.q.inflight;
    const enum rd3_fq_act act = rd3_fq_verdict(&P.q, !early, &had, &tok, &na, &ntok, &fg, &force);
    P.gen++;
    if (had) {
        RD3PresReq &R = P.cur;
        if (early) {
            OSAddAtomic64(1, step == RD3_LATCH_TIMEOUT ? &gCntPqTimeout[head] : &gCntPqUnknown[head]);
            const SInt64 k = OSAddAtomic64(1, &gCntPqEarly[head]);
            if (rd3_log_due(gPqEarlyLogs++, 8, 1023))
                ALOG("present queue: head %u: the flip of %s #%u (phys 0x%llx) %s %llu us after it -- completed "
                     "EARLY, the panel may still read the buffer before it (%lld such)", head,
                     R.kind < kPqKinds ? kPqName[R.kind] : "?", tok, (unsigned long long)was,
                     step == RD3_LATCH_TIMEOUT ? "had NOT latched" : "could not be read", (unsigned long long)us,
                     (long long)k + 1);
            if (R.kind != kPqIop) rd3_latch_give_up(&gLatch[head].st);
        } else {
            OSAddAtomic64(1, &gCntPqLatched[head]);
            OSAddAtomic64((SInt64)us, &gCntPqLatchUs[head]);
            if ((SInt64)us > gCntPqLatchMax[head]) gCntPqLatchMax[head] = (SInt64)us;
            if (inl) OSAddAtomic64(1, &gCntPqInline[head]);
            if (R.kind != kPqIop && gLatch[head].phys == was)
                rd3AccelLatchSeen(head, gLatch[head], kSeenLater, "by the present queue");
        }
        /* N42: a latch the head's pflip interrupt ended is marked in the trace (v3 its latency), not only counted */
        const bool byIrq = !early && irq;
        rd3AccelPqTrace(head, early ? (step == RD3_LATCH_TIMEOUT ? RD3_TRP_TIMEOUT : RD3_TRP_UNKNOWN)
                                    : RD3_TRP_LATCHED, R.kind | (byIrq ? RD3_TRP_IRQ_DONE : 0u), tok, us, 0,
                        byIrq ? (int)(irqLatUs > 0x7fffffffu ? 0x7fffffffu : irqLatUs) : 0, kIOReturnSuccess, was);
        rd3AccelPqLost(d, &R);   /* completed: the transaction ends, the queue's reference goes */
    }
    if (fg) {
        /* another path's flip (Busy) given up: a head whose flips stay pending is bounded (ECC review of N38) */
        const SInt64 k = OSAddAtomic64(1, &gCntPqForeignTo[head]);
        rd3AccelPqTrace(head, RD3_TRP_FOREIGNTO, P.next.kind, ntok, us, P.q.stuck_run, 0, kIOReturnSuccess, na);
        if (rd3_log_due(gPqStuckLogs++, 8, 1023))
            ALOG("present queue: head %u: the flip pending before %s #%u (another path's, or one given up) %s "
                 "%llu us -- given up, %u in a row; the queued present is flipped anyway from %u (%lld such)", head,
                 P.next.kind < kPqKinds ? kPqName[P.next.kind] : "?", ntok,
                 step == RD3_LATCH_TIMEOUT ? "had NOT latched after" : "could not be read after",
                 (unsigned long long)us, P.q.stuck_run, (unsigned)RD3_FQ_STUCK_RUN, (long long)k + 1);
    }
    if (act != RD3_FQ_PROGRAM) return;
    P.cur = P.next;
    rd3AccelPqZero(&P.next);
    if (force) {
        const SInt64 k = OSAddAtomic64(1, &gCntPqGiveup[head]);
        rd3AccelPqTrace(head, RD3_TRP_GIVEUP, P.cur.kind, ntok, us, P.q.stuck_run, 0, kIOReturnSuccess, na);
        if (rd3_log_due(gPqStuckLogs++, 8, 1023))
            ALOG("present queue: head %u: its flips stay PENDING (%u given up in a row: blanked, DPMS off, a stuck "
                 "HUBP or a flip that never latches) -- %s #%u (phys 0x%llx) programmed WITHOUT waiting for them "
                 "(the framebuffer waits <= 20 ms, then flips anyway; %lld such)", head, P.q.stuck_run,
                 P.cur.kind < kPqKinds ? kPqName[P.cur.kind] : "?", ntok, (unsigned long long)na, (long long)k + 1);
    }
    IOReturn rr = kIOReturnSuccess;
    int fr = 0;
    const unsigned pr = rd3AccelPqProgramLocked(head, d, &rr, &fr, force != 0);
    if (pr == kPqProgOk) {
        OSAddAtomic64(1, &gCntPqNext[head]);
        rd3AccelPqTrace(head, RD3_TRP_NEXT, P.cur.kind, ntok, 0, 0, fr, rr, na);
    } else if (pr == kPqProgBusy) {
        rd3AccelPqBusyLocked(head, d);
        rd3AccelPqTrace(head, RD3_TRP_BUSY, P.next.kind, ntok, 0, 0, fr, rr, na);
    } else {
        (void)rd3_fq_refused(&P.q);
        OSAddAtomic64(1, &gCntPqRefused[head]);
        const SInt64 k = OSAddAtomic64(1, &gCntPqDropped[head]);
        if (rd3_log_due(gPqRefLogs++, 8, 1023))
            ALOG("present queue: head %u: the queued %s #%u (phys 0x%llx) was REFUSED by the framebuffer (0x%x, "
                 "flipResult %d) -- that frame is not on the panel (%lld lost)", head,
                 P.cur.kind < kPqKinds ? kPqName[P.cur.kind] : "?", ntok, (unsigned long long)na, (unsigned)rr, fr,
                 (long long)k + 1);
        rd3AccelPqTrace(head, RD3_TRP_REFUSED, P.cur.kind, ntok, 0, ntok, fr, rr, na);   /* v2: from the queue, lost */
        rd3AccelPqLost(d, &P.cur);
    }
}

/* the in-flight flip, read now: latched already? (a present or a writer
 * gets there before the worker's look). Under gPresLock */
static void rd3AccelPqLookLocked(unsigned head, RD3PresDone *d)
{
    RD3Pres &P = gPres[head];
    if (!P.q.inflight || !P.fb) return;
    RD3KDisplayStatus st;
    RD3StatusWhy w;
    if (!rd3AccelDispStatus(P.fb, head, &st, &w) || st.flipPending) return;
    rd3AccelPqLatchedLocked(head, RD3_LATCH_DONE, rd3AccelUsSince(P.progAbs), true, d);
    IOLockWakeup(gPresLock, &P.q, false);
}

/* the worker's wait for the in-flight flip of generation 'gen', off every
 * lock: the framebuffer's latch wait (woken at its vblank) or 1 ms polls */
enum { kPqFbWaitUs = 2000 };
static enum rd3_latch_step rd3AccelPqWait(unsigned head, IOService *fb, uint32_t gen, uint32_t frame0, bool frameKnown,
                                          uint64_t progAbs, uint64_t cap, uint64_t *usOut, bool *irqOut,
                                          uint32_t *irqLatOut)
{
    *irqOut = false;
    *irqLatOut = 0;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_FLIP_FN_LATCHWAIT);
    enum rd3_latch_step step = RD3_LATCH_WAIT;
    for (;;) {
        if (gPres[head].gen != gen || gPresStopping) { step = RD3_LATCH_WAIT; break; }   /* a present or a writer took it */
        bool napped = false;
        if (sym && fb && gPresFbWait[head] != 0) {
            struct RD3KFlipLatchWait lw;
            volatile unsigned char *z = (volatile unsigned char *)&lw;
            for (unsigned i = 0; i < sizeof lw; i++) z[i] = 0;
            lw.version = gPresFbWaitVer[head];
            lw.timeoutUs = kPqFbWaitUs;
            IOReturn r = fb->callPlatformFunction(sym, false, &lw, nullptr, nullptr, nullptr);
            if (r == kIOReturnBadArgument && lw.version > 1) {
                /* N42: a framebuffer older than the version 2 latch wait: version 1 from now on */
                gPresFbWaitVer[head] = 1;
                gCntPqFbWaitV1[head] = 1;
                ALOG("present queue: head %u: the framebuffer refused the version %u latch wait: an older "
                     "RDNA3Framebuffer -- version 1 (no flip-done interrupt flags)", head, lw.version);
                for (unsigned i = 0; i < sizeof lw; i++) z[i] = 0;
                lw.version = 1;
                lw.timeoutUs = kPqFbWaitUs;
                r = fb->callPlatformFunction(sym, false, &lw, nullptr, nullptr, nullptr);
            }
            if (lw.version >= 2 && (r == kIOReturnSuccess || r == kIOReturnTimeout)) {
                gCntPqIrqLive[head] = (lw.irqFlags & RD3K_FLIPWAIT_IRQ_LIVE) ? 1 : 0;
                if (r == kIOReturnSuccess && (lw.irqFlags & RD3K_FLIPWAIT_IRQ_WOKE)) {
                    *irqOut = true;   /* the trace's LATCHED event says so (RD3_TRP_IRQ_DONE) */
                    *irqLatOut = lw.irqLatUs;
                    OSAddAtomic64(1, &gCntPqIrqDone[head]);
                    OSAddAtomic64((SInt64)lw.irqLatUs, &gCntPqIrqLatUs[head]);
                    if ((SInt64)lw.irqLatUs > gCntPqIrqLatMax[head]) gCntPqIrqLatMax[head] = (SInt64)lw.irqLatUs;
                }
            }
            if (r == kIOReturnSuccess || r == kIOReturnTimeout || r == kIOReturnNotReady) {
                gPresFbWait[head] = 1;
                /* slept, unless it answered at once without a latch: a framebuffer stopping, without its latch
                 * lock, or a state it cannot read - then the 1 ms sleep below, never a busy spin to the cap */
                napped = r == kIOReturnSuccess || lw.waitedUs >= kPqFbWaitUs / 2;
                if (r == kIOReturnTimeout) OSAddAtomic64(1, &gCntPqFbWaitTo[head]);
                if (!napped) {
                    const SInt64 k = OSAddAtomic64(1, &gCntPqFbWaitFast[head]);
                    if (rd3_log_due(gPqFastLogs++, 4, 4095))
                        ALOG("present queue: head %u: rdna3FlipLatchWait answered 0x%x after %u us of %u without a "
                             "latch (the framebuffer stopping, no latch lock, or its state unreadable) -- the worker "
                             "sleeps 1 ms between looks (%lld such)", head, (unsigned)r, lw.waitedUs,
                             (unsigned)kPqFbWaitUs, (long long)k + 1);
                }
            } else if (gPresFbWait[head] < 0) {
                gPresFbWait[head] = 0;
                ALOG("present queue: head %u: the framebuffer has no rdna3FlipLatchWait (0x%x: an older "
                     "RDNA3Framebuffer) -- its latches are polled every 1 ms", head, (unsigned)r);
            }
        }
        if (!napped) {
            OSAddAtomic64(1, &gCntPqPolled[head]);
            IOSleep(1);
        }
        RD3KDisplayStatus st;
        RD3StatusWhy w;
        const bool ok = rd3AccelDispStatus(fb, head, &st, &w);
        if (ok && !frameKnown) { frame0 = st.frame; frameKnown = true; }
        *usOut = rd3AccelUsSince(progAbs);
        step = rd3_latch_step(ok, ok && st.flipPending, frame0, ok ? st.frame : frame0, *usOut, cap);
        if (step != RD3_LATCH_WAIT) break;
    }
    if (sym) sym->release();
    return step;
}

/* the head's flip-done worker: until nothing is in flight */
static void rd3AccelPresentRun(unsigned head)
{
    RD3Pres &P = gPres[head];
    for (;;) {
        RD3PresDone d = {};
        IOLockLock(gPresLock);
        if (!P.q.inflight || gPresStopping) {
            P.running = false;
            IOLockWakeup(gPresLock, &P.running, false);   /* rd3AccelPresentStop waits for it */
            IOLockUnlock(gPresLock);
            return;
        }
        IOService *fb = P.fb;
        const uint32_t gen = P.gen, frame0 = P.frame0;
        const bool frameKnown = P.frameKnown;
        const uint64_t progAbs = P.progAbs, cap = P.capUs ? P.capUs : RD3_LATCH_CAP_US;
        IOLockUnlock(gPresLock);
        uint64_t us = 0;
        bool irq = false;
        uint32_t irqLat = 0;
        const enum rd3_latch_step step = rd3AccelPqWait(head, fb, gen, frame0, frameKnown, progAbs, cap, &us, &irq,
                                                        &irqLat);
        IOLockLock(gPresLock);
        if (P.gen == gen && P.q.inflight && step != RD3_LATCH_WAIT) {
            rd3AccelPqLatchedLocked(head, step, us, false, &d, irq, irqLat);
            IOLockWakeup(gPresLock, &P.q, false);   /* the writers waiting for this latch */
        }
        IOLockUnlock(gPresLock);
        rd3AccelPqDoneRun(&d);
    }
}
static void rd3AccelPresentRunTc(void *p0, void *)
{
    rd3AccelPresentRun((unsigned)(uintptr_t)p0);
}
/* under gPresLock: the worker runs while something is in flight */
static void rd3AccelPqArmLocked(unsigned head)
{
    RD3Pres &P = gPres[head];
    if (P.running || !P.q.inflight || !P.call || gPresStopping) return;
    P.running = true;
    thread_call_enter(P.call);
}

/* an IOP present completed without a flip of its own (replaced by a newer one, or joining the flip in flight
 * of its own address): its pipe is signalled now - the family's isTransactionComplete asks the pipe, not the
 * transaction (ECC review of N38: a replaced or joined transaction of another pipe was never signalled) */
static void rd3AccelPqIopDone(unsigned head, RD3PresDone *d, const RD3PresReq &r, uint32_t tok, const char *why)
{
    if (r.kind != kPqIop) return;
    rd3AccelPqDonePipe(d, r.pipe);
    const SInt64 k = OSAddAtomic64(1, &gCntPqIopDrop[head]);
    if (rd3_log_due(gPqIopDropLogs++, 4, 4095))
        ALOG("present queue: head %u: IOP transaction #%u (phys 0x%llx) %s -- its pipe signalled without a flip of "
             "its own (%lld such)", head, tok, r.phys, why, (long long)k + 1);
}

/* a present: programmed now or queued; never waits for a latch. 'trySame'
 * (swap_samenoflip): a present of the buffer the head scans is not flipped
 * again (kPqOutSame) - decided here, under gPresLock, after the latch look
 * and only with nothing in flight or queued (rd3_fq_present: decided before
 * the look, a queued older present was programmed over the newer frame at
 * the next latch; ECC review of N38) */
static unsigned rd3AccelPresentSubmit(unsigned head, IOService *fb, RD3PresReq *rq, uint64_t capUs,
                                      bool trySame = false)
{
    if (head >= kRd3MaxFB || !gPresReady || !fb || gPresStopping) {
        if (rq->surf) ((OSObject *)rq->surf)->release();
        return kPqOutRefused;
    }
    RD3Pres &P = gPres[head];
    RD3PresDone d = {};
    unsigned out = kPqOutShown;
    IOLockLock(gPresLock);
    if (gPresStopping) {   /* rd3AccelPresentStop ran since the check above: nothing programmed any more */
        IOLockUnlock(gPresLock);
        if (rq->surf) ((OSObject *)rq->surf)->release();
        return kPqOutRefused;
    }
    if (capUs) P.capUs = capUs;
    if (P.fb != fb) {
        if (P.fb && P.q.inflight)
            ALOG("present queue: head %u: a new framebuffer (%p, was %p) with a flip in flight -- its latch is "
                 "seen through the new one", head, fb, P.fb);
        P.fb = fb;
        gPresFbWait[head] = -1;   /* whether it has rdna3FlipLatchWait: asked again */
    }
    rd3AccelPqLookLocked(head, &d);   /* a latch the worker has not seen yet: first, as rd3_fq_present wants */
    RD3PanelScan ps = { false, false, 0 };
    if (trySame && rd3_fq_same_wants(&P.q, rq->phys))
        rd3AccelTracePanel(head, fb, rq->phys, &ps);   /* the HUBP's address, now */
    uint32_t dropped = 0;
    const enum rd3_fq_act act = rd3_fq_present(&P.q, rq->phys, rq->tok, trySame, ps.known, ps.scan, ps.pending,
                                               &dropped);
    switch (act) {
    case RD3_FQ_SAME:
        if (rq->surf) rd3AccelPqDoneSurf(&d, rq->surf);
        out = kPqOutSame;
        break;
    case RD3_FQ_PROGRAM: {
        P.cur = *rq;
        IOReturn rr = kIOReturnSuccess;
        int fr = 0;
        const unsigned pr = rd3AccelPqProgramLocked(head, &d, &rr, &fr);
        if (pr == kPqProgOk) {
            OSAddAtomic64(1, &gCntPqProg[head]);
            rd3AccelPqTrace(head, RD3_TRP_PROGRAM, rq->kind, rq->tok, 0, 0, fr, rr, rq->phys);
            out = kPqOutShown;
        } else if (pr == kPqProgBusy) {
            /* another path's flip pending: Busy is kept, programmed at its latch */
            rd3AccelPqBusyLocked(head, &d);
            OSAddAtomic64(1, &gCntPqQueued[head]);
            rd3AccelPqTrace(head, RD3_TRP_BUSY, rq->kind, rq->tok, 0, 0, fr, rr, rq->phys);
            out = kPqOutQueued;
        } else {
            (void)rd3_fq_refused(&P.q);
            OSAddAtomic64(1, &gCntPqRefused[head]);
            rd3AccelPqTrace(head, RD3_TRP_REFUSED, rq->kind, rq->tok, 0, 0, fr, rr, rq->phys);
            if (rd3_log_due(gPqRefLogs++, 8, 1023))
                ALOG("present queue: head %u: %s #%u (phys 0x%llx) REFUSED by the framebuffer (0x%x, flipResult %d)",
                     head, rq->kind < kPqKinds ? kPqName[rq->kind] : "?", rq->tok, rq->phys, (unsigned)rr, fr);
            /* the caller decides (a copy onto the scanout): its own reference is dropped here */
            if (P.cur.surf) rd3AccelPqDoneSurf(&d, P.cur.surf);
            rd3AccelPqZero(&P.cur);
            out = kPqOutRefused;
        }
        break;
    }
    case RD3_FQ_QUEUED:
        P.next = *rq;
        OSAddAtomic64(1, &gCntPqQueued[head]);
        rd3AccelPqTrace(head, RD3_TRP_QUEUED, rq->kind, rq->tok, 0, 0, 0, kIOReturnSuccess, rq->phys);
        out = kPqOutQueued;
        break;
    case RD3_FQ_REPLACED:
        if (P.next.surf) rd3AccelPqDoneSurf(&d, P.next.surf);
        rd3AccelPqIopDone(head, &d, P.next, dropped, "replaced by a newer one (newest wins)");
        P.next = *rq;
        OSAddAtomic64(1, &gCntPqQueued[head]);
        OSAddAtomic64(1, &gCntPqReplaced[head]);
        rd3AccelPqTrace(head, RD3_TRP_REPLACED, rq->kind, rq->tok, 0, dropped, 0, kIOReturnSuccess, rq->phys);
        out = kPqOutReplaced;
        break;
    case RD3_FQ_JOINED:
        if (rq->surf) rd3AccelPqDoneSurf(&d, rq->surf);   /* the in-flight present holds that surface */
        rd3AccelPqIopDone(head, &d, *rq, rq->tok, "joins the flip in flight of its own address");
        OSAddAtomic64(1, &gCntPqJoined[head]);
        rd3AccelPqTrace(head, RD3_TRP_JOINED, rq->kind, rq->tok, 0, 0, 0, kIOReturnSuccess, rq->phys);
        out = kPqOutJoined;
        break;
    default: {
        /* RD3_FQ_IDLE: no VRAM address (never expected: the callers fill it from the vidmem) */
        if (rq->surf) rd3AccelPqDoneSurf(&d, rq->surf);
        OSAddAtomic64(1, &gCntPqRefused[head]);
        rd3AccelPqTrace(head, RD3_TRP_REFUSED, rq->kind, rq->tok, 0, 0, 0, kIOReturnNoResources, rq->phys);
        if (rd3_log_due(gPqRefLogs++, 8, 1023))
            ALOG("present queue: head %u: %s #%u REFUSED before the framebuffer: no VRAM address (phys 0x%llx)", head,
                 rq->kind < kPqKinds ? kPqName[rq->kind] : "?", rq->tok, rq->phys);
        out = kPqOutRefused;
        break;
    }
    }
    rd3AccelPqArmLocked(head);
    const SInt64 k = gCntPqProg[head] + gCntPqQueued[head] + gCntPqJoined[head];
    if (out != kPqOutRefused && out != kPqOutSame && rd3_log_due(gPqLogs[head]++, 6, 4095))
        ALOG("present queue: head %u: %s #%u (phys 0x%llx) %s (%lld presents: %lld programmed at once, %lld queued, "
             "%lld replaced, %lld latched, avg %lld us; debug.rdna3accel_swap_async)", head,
             rq->kind < kPqKinds ? kPqName[rq->kind] : "?", rq->tok, rq->phys,
             out == kPqOutShown ? "programmed, the presenter returns" : out == kPqOutJoined
             ? "joins the flip in flight" : "queued behind the flip in flight", (long long)k,
             (long long)gCntPqProg[head], (long long)gCntPqQueued[head], (long long)gCntPqReplaced[head],
             (long long)gCntPqLatched[head],
             gCntPqLatched[head] ? (long long)(gCntPqLatchUs[head] / gCntPqLatched[head]) : 0ll);
    IOLockUnlock(gPresLock);
    rd3AccelPqDoneRun(&d);
    return out;
}

/* a writer the driver sees is about to write the buffer at 'phys' (what: 1
 * copy forward's target, 2 the family's page-on): if it is still leaving
 * a head's panel, it waits for that latch (at most the head's cap); a
 * buffer on the panel with no flip away is written in place, counted */
static void rd3AccelPresentReuseGate(unsigned long long phys, unsigned what)
{
    if (!phys || !gPresReady) return;
    bool any = false;
    for (unsigned h = 0; h < kRd3MaxFB; h++)
        if (gPres[h].q.shown == phys || gPres[h].q.inflight == phys || gPres[h].q.queued == phys) any = true;
    if (!any) return;
    RD3PresDone d = {};
    IOLockLock(gPresLock);
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        RD3Pres &P = gPres[h];
        enum rd3_fq_use u = rd3_fq_reuse(&P.q, phys);
        if (u == RD3_FQ_SCANNED) { OSAddAtomic64(1, &gCntPqInplace[h]); continue; }
        if (u != RD3_FQ_LEAVING) continue;
        rd3AccelPqLookLocked(h, &d);
        if (rd3_fq_reuse(&P.q, phys) != RD3_FQ_LEAVING) continue;
        const uint64_t t0 = mach_absolute_time();
        const uint64_t cap = P.capUs ? P.capUs : RD3_LATCH_CAP_US;
        uint64_t deadline = 0;
        clock_interval_to_deadline((uint32_t)cap, kMicrosecondScale, &deadline);
        rd3AccelPqArmLocked(h);
        /* woken by the worker at the latch, and by a mode change, a buffer freed, a flip home or the
         * accelerator's stop, which change what the buffer is to the head */
        while (rd3_fq_reuse(&P.q, phys) == RD3_FQ_LEAVING && !gPresStopping && mach_absolute_time() < deadline)
            IOLockSleepDeadline(gPresLock, &P.q, deadline, THREAD_UNINT);
        const bool to = rd3_fq_reuse(&P.q, phys) == RD3_FQ_LEAVING;
        const uint64_t us = rd3AccelUsSince(t0);
        OSAddAtomic64(1, &gCntPqWait[h]);
        OSAddAtomic64((SInt64)us, &gCntPqWaitUs[h]);
        if ((SInt64)us > gCntPqWaitMax[h]) gCntPqWaitMax[h] = (SInt64)us;
        if (to) OSAddAtomic64(1, &gCntPqWaitTimeout[h]);
        rd3AccelPqTrace(h, RD3_TRP_WAIT, (what & 3u) | (to ? 4u : 0u), 0, us, 0, 0, kIOReturnSuccess, phys);
        if (rd3_log_due(gPqWaitLogs++, 8, 1023))
            ALOG("present queue: head %u: %s into phys 0x%llx waited %llu us for the flip away from it to latch%s "
                 "(%lld such)", h, what == 1 ? "a copy forward" : "a page-on", phys, (unsigned long long)us,
                 to ? " -- NOT latched in time, written while the panel may still read it" : "",
                 (long long)gCntPqWait[h]);
    }
    IOLockUnlock(gPresLock);
    rd3AccelPqDoneRun(&d);
}
static void rd3AccelPresentReuseRes(IOAccelResource2 *res, unsigned what)
{
    if (!gSwapAsync && !gIopQueue) return;
    const RD3VidMemory *vm = rd3res_vidmem(res);
    if (vm && !vm->fScanout) rd3AccelPresentReuseGate(vm->fPhys, what);
}

/* a queued present dropped with no newer one replacing it (under gPresLock);
 * 'modeChange': the picture it was meant for is gone (counted apart) */
static void rd3AccelPqDropLocked(unsigned head, uint32_t tok, const char *why, bool modeChange, RD3PresDone *d)
{
    RD3Pres &P = gPres[head];
    const SInt64 k = OSAddAtomic64(1, modeChange ? &gCntPqDropMode[head] : &gCntPqDropped[head]);
    rd3AccelPqTrace(head, RD3_TRP_DROPPED, P.next.kind | (modeChange ? 0x80u : 0u), tok, 0, tok, 0, kIOReturnSuccess,
                    P.next.phys);
    if (rd3_log_due(gPqDropLogs++, 8, 1023))
        ALOG("present queue: head %u: the queued %s #%u (phys 0x%llx) DROPPED: %s -- that frame never reached "
             "the panel (%lld such)", head, P.next.kind < kPqKinds ? kPqName[P.next.kind] : "?", tok,
             P.next.phys, why, (long long)k + 1);
    rd3AccelPqLost(d, &P.next);
}
/* a display mode change: nothing of ours is on the panel (under gPresLock;
 * the writers waiting for a buffer leaving the panel are woken: it is not
 * leaving any more) */
static void rd3AccelPresentForgetAllLocked(const char *why, RD3PresDone *d)
{
    if (!gPresReady) return;
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        uint32_t tok = 0;
        if (rd3_fq_forget(&gPres[h].q, &tok)) rd3AccelPqDropLocked(h, tok, why, true, d);
        IOLockWakeup(gPresLock, &gPres[h].q, false);
    }
}
static void rd3AccelPresentForgetAll(const char *why)
{
    IOLock *l = gPresReady ? gPresLock : nullptr;
    RD3PresDone d = {};
    if (l) IOLockLock(l);
    rd3AccelPresentForgetAllLocked(why, &d);
    rd3AccelLatchForgetHeads(why);   /* under the same hold: the worker writes these records too */
    if (l) IOLockUnlock(l);
    rd3AccelPqDoneRun(&d);
}
/* a vidmem freed: a queued present of it is dropped; one in flight cannot be
 * taken back (counted, logged; swaps_shownfreed counts the panel's record).
 * The queue's part and the latch record's (rd3AccelLatchVidmemGone) under
 * one hold of gPresLock: between them the worker could program the freed
 * buffer and write it back into gLatch (ECC review of N38) */
static void rd3AccelPresentForgetVm(const void *vm, unsigned long long vmPhys, bool vmToo)
{
    if (!vm) return;
    IOLock *l = gPresReady ? gPresLock : nullptr;
    RD3PresDone d = {};
    if (l) IOLockLock(l);
    for (unsigned h = 0; l && h < kRd3MaxFB; h++) {
        RD3Pres &P = gPres[h];
        uint32_t tok = 0;
        /* vmToo false: only a present of exactly that address is this allocation's */
        const bool queued = P.next.vm == vm && (vmToo || P.next.phys == vmPhys);
        if (rd3_fq_forget_addr(&P.q, queued ? P.next.phys : vmPhys, &tok) && queued)
            rd3AccelPqDropLocked(h, tok, "its buffer was freed", false, &d);
        if (P.cur.vm == vm && (vmToo || P.cur.phys == vmPhys) && P.q.inflight && P.q.inflight != RD3_FQ_FOREIGN) {
            const SInt64 k = OSAddAtomic64(1, &gCntPqFreedInflight[h]);
            if (rd3_log_due(gPqFreedLogs++, 8, 1023))
                ALOG("present queue: head %u: the buffer of %s #%u (phys 0x%llx) was FREED while its flip was in "
                     "flight -- the HUBP may scan freed VRAM until the next flip (%lld such)", h,
                     P.cur.kind < kPqKinds ? kPqName[P.cur.kind] : "?", P.q.inflight_tok, P.cur.phys,
                     (long long)k + 1);
            P.cur.vm = nullptr;   /* compare-only: not counted twice */
        }
        IOLockWakeup(gPresLock, &P.q, false);
    }
    rd3AccelLatchVidmemGone(vm, vmPhys, vmToo);
    if (l) IOLockUnlock(l);
    rd3AccelPqDoneRun(&d);
}

/* the IOP copy fallback copies onto the framebuffer's own scanout, so the
 * head flips home first (an IOP flip had moved it). With the present queue
 * (ECC review of N38): under gPresLock the latch look, the flip home (the
 * framebuffer waits at most 20 ms for a pending flip, then programs home),
 * then rd3_fq_moved - a queued IOP flip is dropped (its pipe signalled), it
 * would otherwise be programmed over the copied frame at the next latch;
 * the in-flight one completes; the flip home is what the worker waits for;
 * the latch record forgets the legacy buffer.
 * The head home already (no IOP flip since the last flip home) is not
 * flipped again, but an IOP flip queued behind that flip home is dropped
 * all the same: it is older than the frame copied now and the worker would
 * program it at the home flip's latch, a stale frame that stays if this was
 * the last update (ECC review round 2). No look first: a look that saw the
 * home flip's latch would program it */
static void rd3AccelIopHome(IOService *fb, unsigned head)
{
    if (!fb || head >= kRd3MaxFB) return;
    IOLock *l = rd3LockOnce(&gPresLock);
    RD3PresDone d = {};
    if (l) IOLockLock(l);
    const bool q = gPresReady && l;
    /* an IOP flip of ours in flight sets gIopFlipped under this lock (rd3AccelPqProgramLocked); should the two
     * ever disagree the head is flipped home as below rather than trusted to be home */
    const bool iopInflight = q && gPres[head].q.inflight && gPres[head].q.inflight != RD3_FQ_FOREIGN &&
                             gPres[head].cur.kind == kPqIop;
    if (!gIopFlipped[head] && !iopInflight) {
        RD3Pres &P = gPres[head];
        uint32_t tok = 0;
        if (q && P.q.queued && P.next.kind == kPqIop && rd3_fq_drop_queued(&P.q, &tok)) {
            OSAddAtomic64(1, &gCntPqIopDrop[head]);
            rd3AccelPqDropLocked(head, tok, "the IOP copy fallback copied a newer frame onto the panel, the head "
                                 "home already (no flip)", false, &d);
            IOLockWakeup(gPresLock, &P.q, false);
        }
        if (l) IOLockUnlock(l);
        rd3AccelPqDoneRun(&d);
        return;
    }
    if (q) rd3AccelPqLookLocked(head, &d);
    struct RD3KFlipRequest f; rd3SVZero(&f, sizeof f);
    f.version = RD3K_FLIP_ABI_VERSION; f.flags = RD3K_FLIP_FLAG_HOME;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("rd3FlipToSurfacePure");
    const uint64_t issued = mach_absolute_time();
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    OSAddAtomic64(1, &gCntIopFlipHome);
    if (rr == kIOReturnSuccess) {
        rd3AccelScPanelMoved(head, "an IOP flip home", issued, true);   /* only a flip that was done (round 2) */
        if (q) {
            RD3Pres &P = gPres[head];
            int had = 0;
            uint32_t tok = 0, dropped = 0;
            const unsigned long long was = P.q.inflight, wasq = P.q.queued;
            const bool drop = rd3_fq_moved(&P.q, &had, &tok, &dropped) != 0;
            if (had || drop) {
                const SInt64 k = OSAddAtomic64(1, &gCntPqHome[head]);
                rd3AccelPqTrace(head, RD3_TRP_HOME, had ? P.cur.kind : P.next.kind, had ? tok : 0, 0,
                                drop ? dropped : 0, f.flipResult, rr, had ? was : wasq);
                if (rd3_log_due(gPqHomeLogs++, 8, 1023))
                    ALOG("present queue: head %u: the IOP copy fallback flipped home with %s%s%s -- %s%s (%lld such)",
                         head, had ? "a flip in flight" : "", had && drop ? " and " : "", drop ? "a present queued" : "",
                         had ? "the in-flight one completed after the framebuffer's wait for its latch" : "",
                         drop ? (had ? ", the queued one dropped (the copied frame is newer)"
                                     : "the queued one dropped (the copied frame is newer)") : "",
                         (long long)k + 1);
            }
            if (had) rd3AccelPqLost(&d, &P.cur);
            if (drop) {
                if (P.next.kind == kPqIop) OSAddAtomic64(1, &gCntPqIopDrop[head]);
                rd3AccelPqLost(&d, &P.next);
            }
            P.progAbs = mach_absolute_time();
            P.frameKnown = false;
            P.gen++;
            rd3AccelPqArmLocked(head);   /* the flip home's latch, seen by the worker */
            IOLockWakeup(gPresLock, &P.q, false);
        }
        rd3AccelLatchForget(head, "the IOP copy fallback flipped home");
    } else if (gIopFlipLogs < 8) {
        gIopFlipLogs++;
        ALOG("iop: HOME flip refused (0x%x) head %u", (unsigned)rr, head);
    }
    gIopFlipped[head] = false;
    if (l) IOLockUnlock(l);
    rd3AccelPqDoneRun(&d);
}

/* the accelerator's stop: no worker armed any more, the running ones leave
 * at their next look, the waiters are woken, what is queued or in flight
 * completes (pipes signalled, references dropped), the thread calls freed
 * (ECC review of N38: they were never cancelled) */
static void rd3AccelPresentStop(void)
{
    if (!gPresLock) return;
    RD3PresDone d = {};
    IOLockLock(gPresLock);
    gPresStopping = true;
    gPresReady = false;
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        RD3Pres &P = gPres[h];
        IOLockWakeup(gPresLock, &P.q, false);
        while (P.running) {
            uint64_t dl = 0;
            clock_interval_to_deadline(200, kMillisecondScale, &dl);
            if (IOLockSleepDeadline(gPresLock, &P.running, dl, THREAD_UNINT) == THREAD_TIMED_OUT && P.running)
                ALOG("present queue: stop: head %u's worker still running after 200 ms -- waiting on", h);
        }
        if (P.q.queued || P.q.inflight)
            ALOG("present queue: stop: head %u: %s%s completed unshown", h, P.q.inflight ? "the flip in flight" : "",
                 P.q.queued ? (P.q.inflight ? " and the queued present" : "the queued present") : "");
        rd3AccelPqLost(&d, &P.cur);
        rd3AccelPqLost(&d, &P.next);
        rd3_fq_init(&P.q);
        P.fb = nullptr;
    }
    IOLockUnlock(gPresLock);
    rd3AccelPqDoneRun(&d);
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        struct thread_call *c = gPres[h].call;
        if (!c) continue;
        thread_call_cancel(c);
        while (!thread_call_free(c)) {   /* pending again: cancelled again (the fb's stop does the same) */
            thread_call_cancel(c);
            IOSleep(1);
        }
        gPres[h].call = nullptr;
    }
}

/* an IOP transaction of this pipe still in the queue (isTransactionComplete) */
static bool rd3AccelPresentIopBusy(const void *pipe)
{
    for (unsigned h = 0; gPresReady && h < kRd3MaxFB; h++)
        if ((gPres[h].cur.kind == kPqIop && gPres[h].cur.pipe == pipe) ||
            (gPres[h].next.kind == kPqIop && gPres[h].next.pipe == pipe))
            return true;
    return false;
}

/* a legacy buffer's present (a mode 0 swap, a flip-buffer present): its
 * request, then the queue. 'hash' its band hash at the swap (or null) */
static unsigned rd3AccelPresentLegacy(unsigned head, IOService *fb, IOAccelResource2 *res, unsigned kind, uint32_t tok,
                                      const uint32_t *hash, uint64_t capUs, RD3FlipWhy *fw, bool trySame)
{
    RD3PresReq rq;
    rd3AccelPqZero(&rq);
    RD3VidMemory *vm = nullptr;
    if (!rd3AccelFlipFill(res, fb, fw, &rq.f, &vm)) return kPqOutRefused;
    rq.vm = vm;
    rq.phys = vm->fPhys;
    rq.cookie = vm->fAllocationCookie;
    rq.kind = (uint8_t)kind;
    rq.tok = tok;
    rq.hashed = hash && rd3AccelTornWants(head);
    for (unsigned b = 0; rq.hashed && b < kFlickBands; b++) rq.hash[b] = hash[b];
    const unsigned out = rd3AccelPresentSubmit(head, fb, &rq, capUs, trySame);
    if (out == kPqOutRefused) {
        fw->why = "the framebuffer refused it (a size other than its mode, or the core's flip failed)";
        fw->ret = kIOReturnIOError;
    }
    return out;
}
/* an IOP transaction's zero-copy flip: queued with a reference on its
 * IOSurface; false when the framebuffer refused it (the caller copies) */
static bool rd3AccelPresentIop(unsigned head, IOService *fb, void *pipe, IOSurface *s,
                               const struct RD3KFlipRequest *f, RD3VidMemory *vm)
{
    if (!s || !vm || head >= kRd3MaxFB) return false;
    RD3PresReq rq;
    rd3AccelPqZero(&rq);
    rq.f = *f;
    rq.vm = vm;
    rq.phys = vm->fPhys;
    rq.cookie = vm->fAllocationCookie;
    ((OSObject *)s)->retain();
    rq.surf = s;
    rq.pipe = pipe;
    rq.kind = kPqIop;
    rq.tok = (uint32_t)OSAddAtomic64(1, &gCntPqIop[head]) + 1;
    return rd3AccelPresentSubmit(head, fb, &rq, 0) != kPqOutRefused;
}

SYSCTL_INT(_debug, OID_AUTO, rdna3accel_swap_async, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gSwapAsync, 0,
           "DRIVER-PLAN N38: 1 (default, boot-arg rdna3swapasync) = a mode 0 swap / flip-buffer present programs its "
           "flip and returns (one in flight per head, newer ones queued, the writers into a buffer leaving the panel "
           "wait for its latch); 0 = the swap waits for its latch (debug.rdna3accel_swap_latch)");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_iop_queue, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gIopQueue, 0,
           "DRIVER-PLAN N38: 1 (default, boot-arg rdna3iopqueue) = an IOP transaction's zero-copy flip goes through "
           "the head's present queue (Busy kept, the newest queued, complete at the latch); 0 = the old worker");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_iop_wrote, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gIopWrote, 0,
           "DRIVER-PLAN N38: 1 (default, boot-arg rdna3iopwrote) = the IOP zero-copy flip only of VRAM the Metal "
           "plugin wrote (its dirty mark, this allocation), else the copy");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_perfstats, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gPerfStats, 0,
           "DRIVER-PLAN N38: 1 (default, boot-arg rdna3perfstats) = the core's GPU activity into the accelerator's "
           "PerformanceStatistics (Device / Renderer / Tiler Utilization %) every 250 ms");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_prog, gCntPqProg, "N38: presents programmed at once (nothing was in flight)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_queued, gCntPqQueued, "N38: presents queued behind the flip in flight (Busy ones included)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_replaced, gCntPqReplaced, "N38: queued presents replaced by a newer one (never shown: newest wins)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_joined, gCntPqJoined, "N38: presents of the address already in flight (complete with it)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_busy, gCntPqBusy, "N38: flips the framebuffer answered Busy (another path's flip pending): kept, queued");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_refused, gCntPqRefused, "N38: flips the framebuffer refused (the frame not shown)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_latched, gCntPqLatched, "N38: in-flight flips seen latched (their presents completed)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_latchus, gCntPqLatchUs, "N38: microseconds from programming to the latch seen, summed");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_latchmax, gCntPqLatchMax, "N38: the longest programming-to-latch time seen, microseconds");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_next, gCntPqNext, "N38: queued presents programmed at the latch of the one before");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_timeout, gCntPqTimeout, "N38: flips not latched in time (given up, completed early)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_unknown, gCntPqUnknown, "N38: flips whose state could not be read (given up, completed early)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_dropped, gCntPqDropped, "N38: presents lost with no newer one (their buffer freed, refused when programmed from the queue)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_dropmode, gCntPqDropMode, "N38: queued presents dropped at a display mode change (the picture they were for is gone)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_early, gCntPqEarly, "N38: completions before the latch was seen (timeout or unreadable): must be 0");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_wait, gCntPqWait, "N38: copy forwards / page-ons that waited for the buffer leaving the panel");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_waitus, gCntPqWaitUs, "N38: microseconds those writers waited, summed");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_waitmax, gCntPqWaitMax, "N38: the longest such wait, microseconds");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_waittimeout, gCntPqWaitTimeout, "N38: writers whose wait ran out (written while the panel may read it)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_inplace, gCntPqInplace, "N38: writes into the buffer on the panel with no flip away (one back buffer, in place)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_inline, gCntPqInline, "N38: latches taken by a present or a writer before the worker's look");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_polled, gCntPqPolled, "N38: the worker's 1 ms polls (a framebuffer without rdna3FlipLatchWait)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_iop, gCntPqIop, "N38: IOP transactions presented through the queue");
RD3ACCEL_CNT(rdna3accel_swaps_pq_iopunwritten, gCntPqIopUnwritten, "N38: IOP zero-copy flips refused: VRAM newest but never written by the plugin (copied)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_foreigntimeout, gCntPqForeignTo, "N38: flips pending before ours (another path's, or one given up) not latched in time: given up");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_giveup, gCntPqGiveup, "N38: queued presents programmed without waiting (the head's flips stayed pending RD3_FQ_STUCK_RUN times)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_iopdrop, gCntPqIopDrop, "N38: IOP transactions completed without a flip of their own (replaced, joined, dropped at a flip home)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_fbwaitto, gCntPqFbWaitTo, "N38: the framebuffer's rdna3FlipLatchWait answered Timeout (2 ms each)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_irqdone, gCntPqIrqDone, "N42: latches the framebuffer saw ended by the head's pflip interrupt (flip done)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_irqlatus, gCntPqIrqLatUs, "N42: microseconds from the pflip interrupt to the latch wait waking, summed");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_irqlatmax, gCntPqIrqLatMax, "N42: the longest pflip interrupt -> wake time, microseconds");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_irqlive, gCntPqIrqLive, "N42: 1 = the framebuffer's last latch wait said the head's pflip interrupt is live");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_fbwaitv1, gCntPqFbWaitV1, "N42: 1 = the framebuffer only answers the version 1 latch wait (older than N42)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_fbwaitfast, gCntPqFbWaitFast, "N38: rdna3FlipLatchWait answered at once without a latch (stopping, no lock, unreadable): 1 ms sleeps");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_freedinflight, gCntPqFreedInflight, "N38: buffers freed while their flip was in flight (the HUBP may scan freed VRAM)");
RD3ACCEL_CNT4(rdna3accel_swaps_pq_home, gCntPqHome, "N38: IOP copy fallbacks that flipped home over the queue's flip in flight or queued");
RD3ACCEL_CNT(rdna3accel_swaps_pq_donemax, gCntPqDoneMax, "N38: more pipes / surface references at once than kPqDoneMax (never expected)");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_present_init_failed, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gPresInitFailed, 0,
           "DRIVER-PLAN N38: 1 = the present queue could not be set up at start (no lock or thread call): swap_async "
           "and iop_queue were forced to 0");

/* ---- GPU utilization into the accelerator's PerformanceStatistics (N38, N49) ----
 * Activity Monitor reads the IOAccelerator's PerformanceStatistics ("Device
 * Utilization %", SafeEjectGPU's getPercentUtilization) and, per process,
 * the "accumulatedGPUTime" / "lastSubmittedTime" of the IOAccelSubmitter2
 * entries under the accelerator (common/rdna3_gpu_util.h has the evidence).
 * Every 250 ms the accelerator takes the core's ring busy time
 * (RD3K_GPUBUSY_FN, common/rdna3_gpu_busy.h): the utilisation over the last
 * second goes into its own dictionary, read-copy-modify-write (the family
 * keeps its own keys there and replaces the dictionary every 0.25-2 s), and
 * each process's GPU time since the last take is added to its first
 * IOAccelSubmitter2's accumulatedGPUTime (the family's own OSNumber, the way
 * IOAccelBlockFence::notifyClient adds to it). Device / GPU Activity: any
 * kernel ring with work; Renderer / Tiler: the GFX ring (an immediate-mode
 * GPU has no separate tiler). A core that does not answer (older, no busy
 * accounting): the core's own Device Utilization % (the SMU's GFX activity,
 * 100 % at idle on this card, P2) as before N49, counted. */
static struct thread_call *gPerfCall;
static IOLock *volatile gPerfLock;      /* gPerfStop, gPerfRunning and the re-arm (ECC review of N38) */
static bool gPerfStop, gPerfRunning;
static SInt64 gCntPerfWrites, gCntPerfNoCore, gCntPerfNoKey, gCntPerfDictFail, gCntPerfSetFail;
/* N49 */
static SInt64 gCntPerfRings, gCntPerfSmu, gCntPerfNoAnswer, gCntPerfUtil, gCntPerfUtilGfx, gCntPerfJobs,
              gCntPerfFlushed, gCntPerfLostNs, gCntPerfGpuNs, gCntPerfNoSubNs, gCntPerfNoSub, gCntPerfCreditFail;
/* ECC round 1: the core's other totals, what it does not count, the walk's findings */
static SInt64 gCntPerfStale, gCntPerfMerged, gCntPerfLostJobs, gCntPerfInflight, gCntPerfCappedNs, gCntPerfRingNs[3],
              gCntPerfUnc[RD3K_GPUBUSY_UNC], gCntPerfSdmaEnd[RD3K_GPUBUSY_BY], gCntPerfWinReset,
              gCntPerfSubmitters, gCntPerfUccBad, gCntPerfNoLast;
/* N49 open item 1: the IOAccelCommandQueues among them (the Metal plugin opens one per process) */
static SInt64 gCntPerfCmdQueues;
/* ECC round 1 of item 1: a submitter that has no accumulatedGPUTime yet is one not started (an IOAccelCommandQueue
 * between IOServiceOpen and IOConnectAddClient: IOAccelSubmitter2::start creates the number): a transient miss,
 * counted apart from a lasting creditfail, its time apart from perf_nosubns */
static SInt64 gCntPerfCreditWait, gCntPerfPendingNs;
static SInt64 gCntPerfLastRet = 1;   /* the core's last answer to the take (1: none yet) */
static unsigned gPerfLogs, gPerfFailLogs, gPerfAnswerLogs, gPerfCreditLogs, gPerfNoSubLogs, gPerfWalkLogs;
static struct rd3_util_win gUtilDev, gUtilGfx;
static struct RD3KGpuBusy gPerfBusy;   /* only the tick uses it (gPerfRunning) */
/* the processes already logged with no submitter (-1: free; pid 0 is the kernel's) */
static int32_t gPerfNoSubPid[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static const unsigned kPerfNoSubLogMax = 64;   /* the no-submitter line's global cap, then every 1024th */
static const unsigned kPerfWalkDepth = 4;      /* the accelerator's subtree, as deep as SafeEjectGPU looks in practice */
static const uint32_t kPerfPeriodMs = 250;

/* the values into the accelerator's dictionary */
static void rd3AccelPerfPublish(RDNA3Accelerator *a, unsigned dev, unsigned gfx, const char *src)
{
    if (dev > 100) dev = 100;
    if (gfx > 100) gfx = 100;
    OSObject *mo = a->copyProperty("PerformanceStatistics");
    OSDictionary *cur = OSDynamicCast(OSDictionary, mo);
    OSDictionary *d = cur ? OSDynamicCast(OSDictionary, cur->copyCollection()) : nullptr;
    if (!d) d = OSDictionary::withCapacity(4);
    OSNumber *n = d ? OSNumber::withNumber((unsigned long long)dev, 32) : nullptr;
    OSNumber *g = d ? OSNumber::withNumber((unsigned long long)gfx, 32) : nullptr;
    const char *fail = !d ? "no dictionary (allocation failed)" : !n || !g ? "no OSNumber (allocation failed)" : nullptr;
    if (!fail && !(d->setObject("Device Utilization %", n) && d->setObject("Renderer Utilization %", g) &&
                   d->setObject("Tiler Utilization %", g) && d->setObject("GPU Activity(%)", n)))
        fail = "the dictionary refused a key";
    if (!fail && !a->setProperty("PerformanceStatistics", d))
        fail = "setProperty(PerformanceStatistics) refused";
    if (fail) {
        const SInt64 k = OSAddAtomic64(1, (!d || !n || !g) ? &gCntPerfDictFail : &gCntPerfSetFail);
        if (rd3_log_due(gPerfFailLogs++, 4, 1023))
            ALOG("perfstats: %s -- GPU utilization %u %% NOT published (%lld such)", fail, dev, (long long)k + 1);
    } else {
        const SInt64 k = OSAddAtomic64(1, &gCntPerfWrites);
        if (rd3_log_due(gPerfLogs++, 2, 2047))
            ALOG("perfstats: GPU utilization %u %% (GFX %u %%, %s) into PerformanceStatistics (%s; %lld writes)", dev,
                 gfx, src, cur ? "the family's keys kept" : "a new dictionary", (long long)k + 1);
    }
    if (g) g->release();
    if (n) n->release();
    if (d) d->release();
    if (mo) mo->release();
}

/* before N49 / a core without busy accounting: the core's Device Utilization % (the SMU's) */
static void rd3AccelPerfFromCore(RDNA3Accelerator *a, IOService *core)
{
    OSObject *co = core ? core->copyProperty("PerformanceStatistics") : nullptr;
    OSDictionary *cd = OSDynamicCast(OSDictionary, co);
    OSNumber *un = cd ? OSDynamicCast(OSNumber, cd->getObject("Device Utilization %")) : nullptr;
    if (!un) {
        const SInt64 k = OSAddAtomic64(1, core ? &gCntPerfNoKey : &gCntPerfNoCore);
        if (rd3_log_due((uint64_t)k, 2, 1023))
            ALOG("perfstats: %s -- the accelerator's GPU utilization not updated (%lld such)",
                 !core ? "no core (RDNA3Accel)" : !cd ? "the core has no PerformanceStatistics yet"
                                                   : "the core's PerformanceStatistics has no Device Utilization %",
                 (long long)k + 1);
        if (co) co->release();
        return;
    }
    const unsigned v = un->unsigned32BitValue();
    if (co) co->release();
    OSAddAtomic64(1, &gCntPerfSmu);
    rd3AccelPerfPublish(a, v, v, "the SMU's GFX activity");
}

/* N49: each process's GPU time since the last take into the
 * accumulatedGPUTime of its first IOAccelSubmitter2 in the accelerator's
 * subtree (SafeEjectGPU walks it recursively; the sum per pid is what
 * Activity Monitor shows), its newest submission into lastSubmittedTime. A
 * process with no IOAccelSubmitter2 (a Metal app on the plugin opens
 * IOAccelDevice2 / IOAccelSharedUserClient2 and the DRM bridge only) cannot
 * be shown: counted and logged once per pid, with what the walk found. */
struct rd3PerfWalk {
    const struct RD3KGpuBusy *q;
    bool done[RD3K_GPUBUSY_PIDS];
    bool pending[RD3K_GPUBUSY_PIDS];   /* a submitter of the pid not started yet */
    unsigned np, submitters, uccbad, cmdqueues;
};
static void rd3AccelPerfCreditOne(struct rd3PerfWalk *w, IORegistryEntry *e)
{
    OSObject *co = e->copyProperty("IOUserClientCreator");
    OSString *cs = OSDynamicCast(OSString, co);
    int32_t pid = -1;
    const bool named = cs && rd3_ucc_pid(cs->getCStringNoCopy(), &pid);
    w->submitters++;
    if (!named) {
        w->uccbad++;
        if (rd3_log_due((uint64_t)OSAddAtomic64(1, &gCntPerfUccBad), 4, 1023))
            ALOG("perfstats: a %s with %s IOUserClientCreator '%s' -- no pid to credit", e->getMetaClass()->getClassName(),
                 cs ? "an unparsable" : co ? "a non-string" : "no", cs ? cs->getCStringNoCopy() : "");
        if (co) co->release();
        return;
    }
    if (co) co->release();
    for (unsigned i = 0; i < w->np; i++) {
        const struct RD3KGpuBusyPid *pp = &w->q->pids[i];
        if (w->done[i] || pp->pid != pid) continue;
        OSObject *ao = e->copyProperty("accumulatedGPUTime");
        OSObject *lo = e->copyProperty("lastSubmittedTime");
        OSNumber *an = OSDynamicCast(OSNumber, ao), *ln = OSDynamicCast(OSNumber, lo);
        const char *bad = !an ? (ao ? "accumulatedGPUTime is not a number" : "no accumulatedGPUTime")
                        : an->numberOfBits() != 64 ? "accumulatedGPUTime is not 64 bits" : nullptr;
        if (!bad) {
            uint64_t abs = 0;
            nanoseconds_to_absolutetime(pp->gpuNs, &abs);   /* the family's host time: mach units */
            an->addValue((SInt64)abs);
            OSAddAtomic64((SInt64)pp->gpuNs, &gCntPerfGpuNs);
            w->done[i] = true;
            if (rd3_log_due(gPerfCreditLogs++, 4, 4095))
                ALOG("perfstats: GPU time: pid %d +%llu us (%u job(s)) into %s accumulatedGPUTime %llu", pid,
                     pp->gpuNs / 1000, pp->jobs, e->getMetaClass()->getClassName(), an->unsigned64BitValue());
            if (ln && ln->numberOfBits() == 64) {
                if (pp->lastSubmitAbs > ln->unsigned64BitValue()) ln->setValue(pp->lastSubmitAbs);
            } else {
                const SInt64 k = OSAddAtomic64(1, &gCntPerfNoLast);
                if (rd3_log_due((uint64_t)k, 4, 1023))
                    ALOG("perfstats: pid %d's %s: %s -- Activity Monitor may not see it as submitting (%lld such)", pid,
                         e->getMetaClass()->getClassName(), !ln ? (lo ? "lastSubmittedTime is not a number"
                                                                       : "no lastSubmittedTime")
                                                                 : "lastSubmittedTime is not 64 bits", (long long)k + 1);
            }
        } else if (!ao) {   /* not started yet (no number at all): only the time after its start is credited (later takes); this take's is lost */
            w->pending[i] = true;
            const SInt64 k = OSAddAtomic64(1, &gCntPerfCreditWait);
            if (rd3_log_due((uint64_t)k, 4, 1023))
                ALOG("perfstats: pid %d's %s has no accumulatedGPUTime yet (not started: opened, not yet connected) -- "
                     "this take's %llu us are lost (perf_pendingns), later takes credit it once started (%lld such)", pid, e->getMetaClass()->getClassName(),
                     pp->gpuNs / 1000, (long long)k + 1);
        } else {
            const SInt64 k = OSAddAtomic64(1, &gCntPerfCreditFail);
            if (rd3_log_due((uint64_t)k, 4, 1023))
                ALOG("perfstats: pid %d's %s: %s -- not credited (%lld such)", pid, e->getMetaClass()->getClassName(),
                     bad, (long long)k + 1);
        }
        if (ao) ao->release();
        if (lo) lo->release();
        break;
    }
}
/* the subtree through snapshots of each child list (getChildIterator copies
 * the list and its entries stay retained while it lives), depth-limited */
static void rd3AccelPerfWalk(struct rd3PerfWalk *w, IORegistryEntry *at, unsigned depth)
{
    OSIterator *it = at->getChildIterator(gIOServicePlane);
    if (!it) {
        const SInt64 k = OSAddAtomic64(1, &gCntPerfCreditFail);
        if (rd3_log_due((uint64_t)k, 4, 1023))
            ALOG("perfstats: no child iterator at depth %u -- part of the processes' GPU time NOT credited (%lld such)",
                 depth, (long long)k + 1);
        return;
    }
    while (OSObject *o = it->getNextObject()) {
        IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
        if (!e) continue;
        if (e->metaCast("IOAccelSubmitter2")) {
            if (e->metaCast("IOAccelCommandQueue")) w->cmdqueues++;
            rd3AccelPerfCreditOne(w, e);
        }
        else if (depth + 1 < kPerfWalkDepth)
            rd3AccelPerfWalk(w, e, depth + 1);
    }
    it->release();
}
static void rd3AccelPerfCredit(RDNA3Accelerator *a, const struct RD3KGpuBusy *q)
{
    static struct rd3PerfWalk w;   /* only the tick (gPerfRunning) */
    bzero(&w, sizeof w);
    w.q = q;
    w.np = q->npids < RD3K_GPUBUSY_PIDS ? q->npids : RD3K_GPUBUSY_PIDS;
    if (!w.np) return;
    rd3AccelPerfWalk(&w, a, 0);
    gCntPerfSubmitters = (SInt64)w.submitters;
    gCntPerfCmdQueues = (SInt64)w.cmdqueues;
    for (unsigned i = 0; i < w.np; i++) {
        const struct RD3KGpuBusyPid *pp = &q->pids[i];
        if (w.done[i] || !pp->gpuNs) continue;
        if (w.pending[i]) {   /* its only submitters are not started yet: counted apart, no 'has no submitter' line */
            OSAddAtomic64((SInt64)pp->gpuNs, &gCntPerfPendingNs);
            continue;
        }
        OSAddAtomic64((SInt64)pp->gpuNs, &gCntPerfNoSubNs);
        OSAddAtomic64(1, &gCntPerfNoSub);
        bool seen = false;
        for (unsigned k = 0; k < sizeof gPerfNoSubPid / sizeof gPerfNoSubPid[0]; k++)
            if (gPerfNoSubPid[k] == pp->pid) seen = true;
        if (seen) continue;
        const unsigned n = gPerfNoSubLogs++;
        gPerfNoSubPid[n % (sizeof gPerfNoSubPid / sizeof gPerfNoSubPid[0])] = pp->pid;
        if (n >= kPerfNoSubLogMax && (n & 1023)) continue;
        ALOG("perfstats: pid %d used the GPU (%llu us, %u job(s)) but has no IOAccelSubmitter2 (context / command "
             "queue) under the accelerator -- Activity Monitor shows 0 %% GPU for it (the walk saw %u submitter(s), "
             "%u of them command queues, %u without a readable pid; the plugin's 'GPU time queue:' line of that pid "
             "says why it has none)", pp->pid, pp->gpuNs / 1000, pp->jobs, w.submitters, w.cmdqueues, w.uccbad);
    }
}

static const char *rd3AccelPerfRetName(IOReturn r)
{
    return r == kIOReturnSuccess ? "answered" : r == kIOReturnUnsupported ? "an older core"
         : r == kIOReturnNotReady ? "no busy accounting / no GPU yet" : r == kIOReturnNotAttached ? "no core"
         : r == kIOReturnNoMemory ? "no OSSymbol (allocation failed)"
         : r == kIOReturnBadArgument ? "the core's ABI version differs" : "refused";
}

/* one update: the core's ring busy time (else its SMU value) into the accelerator's dictionary */
static void rd3AccelPerfUpdate(RDNA3Accelerator *a)
{
    IOService *core = rd3AccelCore();
    const OSSymbol *sym = core ? OSSymbol::withCStringNoCopy(RD3K_GPUBUSY_FN) : nullptr;
    struct RD3KGpuBusy *q = &gPerfBusy;
    bzero(q, sizeof(*q));
    q->version = RD3K_GPUBUSY_VERSION;
    const IOReturn r = sym ? core->callPlatformFunction(sym, false, q, nullptr, nullptr, nullptr)
                           : (core ? kIOReturnNoMemory : kIOReturnNotAttached);
    if (sym) sym->release();
    if ((SInt64)(uint32_t)r != gCntPerfLastRet) {   /* every change of the answer, both ways */
        ALOG("perfstats: the core's ring busy time: %s (0x%x, before 0x%x)", rd3AccelPerfRetName(r), r,
             (unsigned)gCntPerfLastRet);
        gCntPerfLastRet = (SInt64)(uint32_t)r;
    }
    if (r != kIOReturnSuccess) {
        const SInt64 k = OSAddAtomic64(1, &gCntPerfNoAnswer);
        if (rd3_log_due(gPerfAnswerLogs++, 2, 1023))
            ALOG("perfstats: the core did not give its ring busy time (0x%x: %s; %lld such) -- the SMU's activity "
                 "(100 %% at idle on this card, P2) published instead", r, rd3AccelPerfRetName(r), (long long)k + 1);
        gCntPerfUtil = gCntPerfUtilGfx = 0;   /* not the rings' any more */
        bzero(&gUtilDev, sizeof gUtilDev);    /* the window starts again when they come back */
        bzero(&gUtilGfx, sizeof gUtilGfx);
        rd3AccelPerfFromCore(a, core);
        return;
    }
    int vd = 0, vg = 0;
    const unsigned dev = rd3_util_feed(&gUtilDev, q->busyNs, q->nowNs, &vd);
    const unsigned gfx = rd3_util_feed(&gUtilGfx, q->ringNs[0], q->nowNs, &vg);
    gCntPerfJobs = (SInt64)q->jobs;
    gCntPerfFlushed = (SInt64)q->flushed;
    gCntPerfLostNs = (SInt64)q->lostNs;
    gCntPerfStale = (SInt64)q->stale;
    gCntPerfMerged = (SInt64)q->merged;
    gCntPerfLostJobs = (SInt64)q->lostJobs;
    gCntPerfInflight = (SInt64)q->inflight;
    gCntPerfCappedNs = (SInt64)q->cappedNs;
    for (unsigned i = 0; i < 3; i++) gCntPerfRingNs[i] = (SInt64)q->ringNs[i];
    for (unsigned i = 0; i < RD3K_GPUBUSY_UNC; i++) gCntPerfUnc[i] = (SInt64)q->uncounted[i];
    for (unsigned i = 0; i < RD3K_GPUBUSY_BY; i++) gCntPerfSdmaEnd[i] = (SInt64)q->sdmaEnds[i];
    rd3AccelPerfCredit(a, q);
    if (!vd || !vg) {   /* the first take, or the core's totals went back: the window starts, nothing published */
        const SInt64 k = OSAddAtomic64(1, &gCntPerfWinReset);
        if (k && rd3_log_due(gPerfWalkLogs++, 2, 1023))
            ALOG("perfstats: the core's busy totals went back (a core restart?) -- the 1 s window starts again "
                 "(%lld such)", (long long)k + 1);
        return;
    }
    gCntPerfUtil = dev;
    gCntPerfUtilGfx = gfx;
    OSAddAtomic64(1, &gCntPerfRings);
    rd3AccelPerfPublish(a, dev, gfx, "the rings' busy time");
}

static void rd3AccelPerfTick(void *, void *)
{
    IOLock *l = gPerfLock;
    if (!l) return;
    IOLockLock(l);
    if (gPerfStop) { IOLockUnlock(l); return; }
    gPerfRunning = true;
    IOLockUnlock(l);
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (a && gPerfStats) rd3AccelPerfUpdate(a);
    IOLockLock(l);
    gPerfRunning = false;
    if (gPerfStop) {
        IOLockWakeup(l, &gPerfRunning, false);   /* rd3AccelPerfStop waits for this tick */
    } else if (gPerfCall) {
        /* re-armed under the lock: a stop between the check and the enter cannot be missed */
        uint64_t dl = 0;
        clock_interval_to_deadline(kPerfPeriodMs, kMillisecondScale, &dl);
        thread_call_enter_delayed(gPerfCall, dl);
    }
    IOLockUnlock(l);
}
static void rd3AccelPerfStart(void)
{
    IOLock *l = rd3LockOnce(&gPerfLock);
    if (!l) { ALOG("perfstats: no lock -- the GPU utilization is not published"); return; }
    IOLockLock(l);
    if (!gPerfCall) gPerfCall = thread_call_allocate(rd3AccelPerfTick, nullptr);
    if (!gPerfCall) {
        IOLockUnlock(l);
        ALOG("perfstats: no thread call -- the GPU utilization is not published");
        return;
    }
    gPerfStop = false;   /* a start after a stop */
    uint64_t dl = 0;
    clock_interval_to_deadline(1000, kMillisecondScale, &dl);
    thread_call_enter_delayed(gPerfCall, dl);
    IOLockUnlock(l);
}
static void rd3AccelPerfStop(void)
{
    IOLock *l = gPerfLock;
    if (!l) return;
    IOLockLock(l);
    gPerfStop = true;
    while (gPerfRunning) IOLockSleep(l, &gPerfRunning, THREAD_UNINT);
    struct thread_call *c = gPerfCall;
    gPerfCall = nullptr;
    IOLockUnlock(l);
    if (!c) return;
    /* not re-armed any more (gPerfStop under the lock): cancel what is pending, then free it */
    thread_call_cancel(c);
    while (!thread_call_free(c)) {
        thread_call_cancel(c);
        IOSleep(1);
    }
}
RD3ACCEL_CNT(rdna3accel_perf_writes, gCntPerfWrites, "N38: PerformanceStatistics updates with the core's GPU utilization");
RD3ACCEL_CNT(rdna3accel_perf_nocore, gCntPerfNoCore, "N38: updates skipped: no core service");
RD3ACCEL_CNT(rdna3accel_perf_nokey, gCntPerfNoKey, "N38: updates skipped: the core published no Device Utilization %");
RD3ACCEL_CNT(rdna3accel_perf_dictfail, gCntPerfDictFail, "N38: updates lost: the dictionary or OSNumber allocation failed");
RD3ACCEL_CNT(rdna3accel_perf_setfail, gCntPerfSetFail, "N38: updates lost: a key or setProperty(PerformanceStatistics) refused");
RD3ACCEL_CNT(rdna3accel_perf_rings, gCntPerfRings, "N49: updates from the core's ring busy time");
RD3ACCEL_CNT(rdna3accel_perf_smu, gCntPerfSmu, "N49: updates from the SMU's GFX activity (the core gave no ring busy time)");
RD3ACCEL_CNT(rdna3accel_perf_noanswer, gCntPerfNoAnswer, "N49: takes the core did not answer (older core, no busy accounting)");
RD3ACCEL_CNT(rdna3accel_perf_util, gCntPerfUtil, "N49: the last Device Utilization % from the rings (any ring with work, 1 s)");
RD3ACCEL_CNT(rdna3accel_perf_utilgfx, gCntPerfUtilGfx, "N49: the last Renderer Utilization % (the GFX ring, 1 s)");
RD3ACCEL_CNT(rdna3accel_perf_jobs, gCntPerfJobs, "N49: jobs the core's busy accounting saw (total)");
RD3ACCEL_CNT(rdna3accel_perf_flushed, gCntPerfFlushed, "N49: jobs flushed with no time (a ring reset or a fence never seen)");
RD3ACCEL_CNT(rdna3accel_perf_lostns, gCntPerfLostNs, "N49: GPU time (ns) of processes the core had no slot for");
RD3ACCEL_CNT(rdna3accel_perf_gpuns, gCntPerfGpuNs, "N49: GPU time (ns) added to the processes' IOAccelSubmitter2 accumulatedGPUTime");
RD3ACCEL_CNT(rdna3accel_perf_nosubns, gCntPerfNoSubNs, "N49: GPU time (ns) of processes with no IOAccelSubmitter2 (Activity Monitor shows 0 %)");
RD3ACCEL_CNT(rdna3accel_perf_nosub, gCntPerfNoSub, "N49: takes of a process with GPU time and no IOAccelSubmitter2");
RD3ACCEL_CNT(rdna3accel_perf_stale, gCntPerfStale, "N49: jobs flushed as stale (no fence for 30 s without a reset the core saw)");
RD3ACCEL_CNT(rdna3accel_perf_merged, gCntPerfMerged, "N49: jobs merged into a full ring queue (more than 64 in flight)");
RD3ACCEL_CNT(rdna3accel_perf_lostjobs, gCntPerfLostJobs, "N49: jobs whose time had no process slot");
RD3ACCEL_CNT(rdna3accel_perf_inflight, gCntPerfInflight, "N49: jobs in flight at the last take");
RD3ACCEL_CNT(rdna3accel_perf_cappedns, gCntPerfCappedNs, "N49: process GPU time (ns) dropped above the wall time (several rings at once)");
RD3ACCEL_CNT(rdna3accel_perf_ringns_gfx, gCntPerfRingNs[0], "N49: GFX ring busy time (ns, total)");
RD3ACCEL_CNT(rdna3accel_perf_ringns_comp, gCntPerfRingNs[1], "N49: compute ring busy time (ns, total)");
RD3ACCEL_CNT(rdna3accel_perf_ringns_sdma, gCntPerfRingNs[2], "N49: SDMA ring busy time (ns, total)");
RD3ACCEL_CNT(rdna3accel_perf_unc_vcn, gCntPerfUnc[0], "N49: VCN encode jobs not in the busy time");
RD3ACCEL_CNT(rdna3accel_perf_unc_jpeg, gCntPerfUnc[1], "N49: JPEG jobs not in the busy time");
RD3ACCEL_CNT(rdna3accel_perf_unc_other, gCntPerfUnc[2], "N49: jobs on rings other than 0 not in the busy time");
RD3ACCEL_CNT(rdna3accel_perf_unc_kcopy, gCntPerfUnc[3], "N49: kernel SDMA copies / clears not in the busy time");
RD3ACCEL_CNT(rdna3accel_perf_unc_userq, gCntPerfUnc[4], "N49: user queues added (their jobs never reach the kernel)");
RD3ACCEL_CNT(rdna3accel_perf_sdmaend_irq, gCntPerfSdmaEnd[0], "N49: SDMA jobs ended at an interrupt's fence read");
RD3ACCEL_CNT(rdna3accel_perf_sdmaend_wait, gCntPerfSdmaEnd[1], "N49: SDMA jobs ended at a waiter's fence read");
RD3ACCEL_CNT(rdna3accel_perf_sdmaend_take, gCntPerfSdmaEnd[2], "N49: SDMA jobs ended at the 250 ms take (up to 250 ms late)");
RD3ACCEL_CNT(rdna3accel_perf_sdmaend_sub, gCntPerfSdmaEnd[3], "N49: SDMA jobs ended at the next submission's fence read");
RD3ACCEL_CNT(rdna3accel_perf_sdmaend_reset, gCntPerfSdmaEnd[4], "N49: SDMA jobs ended by a reset's forced fences");
RD3ACCEL_CNT(rdna3accel_perf_lastret, gCntPerfLastRet, "N49: the core's last answer to the take, an IOReturn as unsigned (0 = answered, 1 = none yet)");
RD3ACCEL_CNT(rdna3accel_perf_winreset, gCntPerfWinReset, "N49: takes not published: the first one, or the core's totals went back");
RD3ACCEL_CNT(rdna3accel_perf_submitters, gCntPerfSubmitters, "N49: IOAccelSubmitter2 entries the last credit walk saw");
RD3ACCEL_CNT(rdna3accel_perf_uccbad, gCntPerfUccBad, "N49: IOAccelSubmitter2 entries with no readable IOUserClientCreator pid");
RD3ACCEL_CNT(rdna3accel_perf_cmdqueues, gCntPerfCmdQueues, "N49: IOAccelCommandQueue entries the last credit walk saw (the Metal plugin opens one per process)");
RD3ACCEL_CNT(rdna3accel_perf_creditwait, gCntPerfCreditWait, "N49: credits missed because the pid's submitter was not started yet (no accumulatedGPUTime: a transient miss)");
RD3ACCEL_CNT(rdna3accel_perf_pendingns, gCntPerfPendingNs, "N49: GPU time (ns) of processes whose only submitters were not started yet at the take");
RD3ACCEL_CNT(rdna3accel_perf_nolast, gCntPerfNoLast, "N49: credited submitters without a 64-bit lastSubmittedTime");
RD3ACCEL_CNT(rdna3accel_perf_creditfail, gCntPerfCreditFail, "N49: GPU time not credited: no child iterator, or the submitter has no accumulatedGPUTime");

static void rd3AccelInstallSysctlPq(void)
{
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_async); sysctl_register_oid(&sysctl__debug_rdna3accel_iop_queue);
    sysctl_register_oid(&sysctl__debug_rdna3accel_iop_wrote);  sysctl_register_oid(&sysctl__debug_rdna3accel_perfstats);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_prog);    RD3ACCEL_REG4(rdna3accel_swaps_pq_queued);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_replaced); RD3ACCEL_REG4(rdna3accel_swaps_pq_joined);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_busy);    RD3ACCEL_REG4(rdna3accel_swaps_pq_refused);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_latched); RD3ACCEL_REG4(rdna3accel_swaps_pq_latchus);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_latchmax); RD3ACCEL_REG4(rdna3accel_swaps_pq_next);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_timeout); RD3ACCEL_REG4(rdna3accel_swaps_pq_unknown);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_dropped); RD3ACCEL_REG4(rdna3accel_swaps_pq_early);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_dropmode);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_wait);    RD3ACCEL_REG4(rdna3accel_swaps_pq_waitus);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_waitmax); RD3ACCEL_REG4(rdna3accel_swaps_pq_waittimeout);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_inplace); RD3ACCEL_REG4(rdna3accel_swaps_pq_inline);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_polled);  RD3ACCEL_REG4(rdna3accel_swaps_pq_iop);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_pq_iopunwritten);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_foreigntimeout); RD3ACCEL_REG4(rdna3accel_swaps_pq_giveup);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_iopdrop);  RD3ACCEL_REG4(rdna3accel_swaps_pq_fbwaitto);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_fbwaitfast);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_irqdone); RD3ACCEL_REG4(rdna3accel_swaps_pq_irqlatus);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_irqlatmax); RD3ACCEL_REG4(rdna3accel_swaps_pq_irqlive);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_fbwaitv1); RD3ACCEL_REG4(rdna3accel_swaps_pq_freedinflight);
    RD3ACCEL_REG4(rdna3accel_swaps_pq_home);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_pq_donemax);
    sysctl_register_oid(&sysctl__debug_rdna3accel_present_init_failed);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_writes); sysctl_register_oid(&sysctl__debug_rdna3accel_perf_nocore);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_nokey);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_dictfail);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_setfail);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_rings); sysctl_register_oid(&sysctl__debug_rdna3accel_perf_smu);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_noanswer); sysctl_register_oid(&sysctl__debug_rdna3accel_perf_util);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_utilgfx); sysctl_register_oid(&sysctl__debug_rdna3accel_perf_jobs);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_flushed); sysctl_register_oid(&sysctl__debug_rdna3accel_perf_lostns);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_gpuns); sysctl_register_oid(&sysctl__debug_rdna3accel_perf_nosubns);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_nosub);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_creditfail);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_stale);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_merged);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_lostjobs);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_inflight);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_cappedns);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_ringns_gfx);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_ringns_comp);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_ringns_sdma);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_unc_vcn);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_unc_jpeg);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_unc_other);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_unc_kcopy);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_unc_userq);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_sdmaend_irq);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_sdmaend_wait);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_sdmaend_take);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_sdmaend_sub);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_sdmaend_reset);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_lastret);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_winreset);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_submitters);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_uccbad);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_cmdqueues);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_creditwait);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_pendingns);
    sysctl_register_oid(&sysctl__debug_rdna3accel_perf_nolast);
}


/* (e) a swap skipped because its back read empty: read again 4 ms later,
 * sampled (rd3_empty_resample_due: only with swap_latch on, so 0 sleeps
 * nowhere; the first 8 of a head, then every 256th: the sleep is under the
 * family's lock, in the swap) - did the frame land late? */
static unsigned gEmptyResamples[kRd3MaxFB];
static void rd3AccelResScanFwd(IOAccelResource2 *res, unsigned *rc, uint8_t *band);
static void rd3AccelTraceScanFwd(unsigned head, const void *surf, IOAccelResource2 *res, unsigned rc,
                                 const uint8_t *band, unsigned flags, unsigned n, unsigned v1);
static void rd3AccelEmptyResample(unsigned head, IOAccelResource2 *back, bool wasEmpty, unsigned run, const void *surf,
                                  unsigned n)
{
    if (head >= kRd3MaxFB || !back || !wasEmpty) return;
    const unsigned s = gEmptyResamples[head]++;
    if (!rd3_empty_resample_due(gSwapLatch, s)) return;   /* swap_latch=0: no sleep from this feature */
    const uint64_t t0 = mach_absolute_time();
    IOSleep(4);
    unsigned rc = 0;
    uint8_t band[8];
    rd3AccelResScanFwd(back, &rc, band);
    const bool filled = rc == kRcPixels;
    const uint64_t us = rd3AccelUsSince(t0);
    /* the trace (Q3): the empty back's bands again, with the delay */
    rd3AccelTraceScanFwd(head, surf, back, rc, band, 4u | 16u, n, rd3TrSat(us));
    OSAddAtomic64(1, filled ? &gCntEmptyFilled[head] : &gCntEmptyStill[head]);
    const RD3VidMemory *bv = rd3res_vidmem(back);
    if (rd3_log_due((uint64_t)(gCntEmptyFilled[head] + gCntEmptyStill[head] - 1), 8, 15))
        ALOG("swap: head %u: the empty back (phys 0x%llx, %u skipped in a row) read again %llu us later: %s (%lld "
             "filled, %lld still empty of the sampled backs; %u skipped swaps of an empty back so far)", head,
             bv ? bv->fPhys : 0ull, run, us, filled ? "NOW HAS PIXELS -- the frame landed after the swap" : "still empty",
             (long long)gCntEmptyFilled[head], (long long)gCntEmptyStill[head], s + 1);
}

/* the head's latch and torn counts, at the 1st, 8th, 120th, 1200th and
 * every 4096th swap: what triage needs from a serial log alone */
static void rd3AccelLatchSummary(unsigned head, SInt64 n)
{
    if (n != 1 && n != 8 && n != 120 && n != 1200 && (n & 0xfff)) return;
    const SInt64 w = gCntLatchWait[head];
    ALOG("swap latch: head %u: %lld swaps; previous flip not latched at entry %lld; latched while waited %lld (avg %lld "
         "us, max %lld us), %lld timeouts, %lld unknown, %lld not waited; shown buffers hashed again %lld: %lld changed "
         "on the panel then flipped away (torn), %lld rewritten in place (one back), %lld not hashed; copy forwards into "
         "the buffer on the panel %lld; buffers freed on the panel %lld; empty backs read again: %lld filled, %lld "
         "still empty (swap_latch %d, card run 2026-10-17); latched at once %lld, flips failed %lld, waits %s; same "
         "buffer not flipped %lld (swap_samenoflip %d)", head, (long long)n, (long long)gCntLatchPend[head],
         (long long)w, w ? (long long)(gCntLatchUs[head] / w) : 0LL, (long long)gCntLatchMax[head],
         (long long)gCntLatchTimeout[head], (long long)gCntLatchUnknown[head], (long long)gCntLatchOff[head],
         (long long)gCntTornChk[head], (long long)gCntTorn[head], (long long)gCntInplace[head],
         (long long)gCntTornSkip[head], (long long)gCntCfOnPanel[head], (long long)gCntShownFreed[head],
         (long long)gCntEmptyFilled[head], (long long)gCntEmptyStill[head], gSwapLatch,
         (long long)gCntLatchImmediate[head], (long long)gCntFlipFailed[head],
         gLatch[head].st.off ? "OFF (timeouts)" : "on", (long long)gCntLatchSame[head], gSwapSameNoFlip);
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
/* the buffers each head's swaps have shown (card run 58b: two backs taking
 * turns after the login; the phys is the buffer's name, pointers read
 * <ptr> in the log), and how many new-buffer lines each head may log */
enum { kHeadBufs = 4 };
static unsigned long long gHeadBufPhys[kRd3MaxFB][kHeadBufs];
static unsigned gHeadBufLogs[kRd3MaxFB];
/* each head's swaps skipped in a row, whichever back they named
 * (rd3_swap_source: the third goes to the panel; two empty backs taking
 * turns count too), and the surface of its last swap shown */
static unsigned gSwapEmptyRun[kRd3MaxFB];
static const void *volatile gHeadSurf[kRd3MaxFB];
static unsigned    gSwapCount, gRouteLogs;
#define RD3ROUTELOG(...) do { if (gRouteLogs < 200) { gRouteLogs++; ALOG(__VA_ARGS__); } } while (0)

void rd3AccelSurfaceGone(const void *surf)
{
    for (unsigned i = 0; i < 8; i++)
        if (gSurfHead[i] == surf) {
            const unsigned h = gSurfHeadIdx[i];
            {
                struct rd3_trace_ev e = rd3TrEv(RD3_TR_GONE, h);
                e.arg = 0;
                e.obj = rd3TrObj(surf);
                rd3Trace(e);
            }
            RD3ROUTELOG("swap: surface %p (head %u) freed%s", surf, h,
                        h < kRd3MaxFB && gHeadSurf[h] != surf ? " (another surface swaps on that head now)" : "");
            gSurfHead[i] = nullptr;
            /* its buffers go with it: a new surface that gets the same VRAM
             * is not "head N's frame" (card run 58b's swap #84) - only when
             * it was the head's last swapper (at the login the new
             * WindowServer's surface swaps before the old one is freed) */
            if (h < kRd3MaxFB && gHeadSurf[h] == surf) {
                gHeadSurf[h] = nullptr;
                gHeadSrcPhys[h] = 0;
                for (unsigned k = 0; k < kHeadBufs; k++) gHeadBufPhys[h][k] = 0;
                gHeadBufLogs[h] = 0;          /* the next surface's buffers are logged again */
                gSwapEmptyRun[h] = 0;
                rd3AccelForgetShown(h);       /* what it showed is no source for the next surface */
                rd3AccelScStaleReset(h, "its surface was freed");   /* its backs' records go with it */
                /* the next surface's first swap presents (the write generations: its buffers may be drawn by
                 * WindowServer before any page-on of ours is seen) */
                rd3AccelWgHeadReset(h, "its swapping surface was freed");
            }
        }
}

void rd3AccelResourceGone(const void *res)
{
    unsigned gone = 0;
    /* its VRAM is no front holding the last frame any more (ECC review round 1) */
    if (res) rd3AccelScFrontGone(rd3res_vidmem((IOAccelResource2 *)res));
    if (!gHeadFbResLock) return;   /* nothing was ever entered */
    IOSimpleLockLock(gHeadFbResLock);
    for (unsigned h = 0; h < kRd3MaxFB; h++)
        if (gHeadFbRes[h] == res) {
            gHeadFbRes[h] = nullptr;
            gone |= 1u << h;
        }
    IOSimpleLockUnlock(gHeadFbResLock);
    for (unsigned h = 0; h < kRd3MaxFB; h++)
        if (gone & (1u << h)) {
            RD3ROUTELOG("swap: head %u's pipe resource %p freed", h, res);
            {
                struct rd3_trace_ev e = rd3TrEv(RD3_TR_GONE, h);
                e.arg = 1;
                e.obj = rd3TrObj(res);
                rd3Trace(e);
            }
            rd3AccelForgetShown(h);   /* a new front, a new mode: nothing recorded holds its frame */
            rd3AccelScStaleReset(h, "its framebuffer resource was freed");
        }
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
/* whether the flicker test hashes this head's next swap (its budget) */
static bool rd3AccelFlickerWants(unsigned head)
{
    return head < kRd3MaxFB && gFlicker && !(gFlicker == 1 && gCntSwapHashed[head] >= kFlickBudget);
}
/* 'hash': the band hash of the swap's buffer (rd3AccelBandHash, taken once
 * for this test and the torn test), nullptr with 'skip' saying why not */
static void rd3AccelFlickerNote(unsigned head, IOAccelResource2 *src, unsigned long long phys, SInt64 n,
                                const uint32_t *hash, const char *skip)
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
    const unsigned char *r = (const unsigned char *)src;
    const unsigned w = src ? *(const volatile unsigned short *)(r + 0xb0) : 0;
    const unsigned h = src ? *(const volatile unsigned short *)(r + 0xb2) : 0;
    if (!hash && !skip) skip = "not hashed";
    if (skip) {
        const SInt64 k = OSAddAtomic64(1, &gCntSwapHashSkip[head]);
        gSwapHashRun[head] = 0;
        if (k < 4 || (k & 0xffff) == 0)
            ALOG("flicker: head %u: swap %lld not hashed: %s (%lld skipped) -- not counted in swaps_hashed_h%u",
                 head, (long long)n, skip, (long long)k + 1, head);
        return;
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

static void rd3AccelA3SwapNote(unsigned head, IOAccelResource2 *src, unsigned long long phys, unsigned how,
                               const uint32_t *hash, const char *hashSkip)
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
    rd3AccelFlickerNote(head, src, phys, n, hash, hashSkip);
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

/* the summary of a head's swaps (card run 58b): the buffers it showed, the
 * swaps skipped for an empty back, copy forward's outcomes and the flicker
 * test, at the head's 8th, 120th, 1200th and every 65536th swap: one line
 * carries what triage needs from a serial log alone */
static void rd3AccelSwapSummary(unsigned head, SInt64 n, unsigned long long front)
{
    if (n != 8 && n != 120 && n != 1200 && (n & 0xffff)) return;
    unsigned nb = 0;
    for (unsigned k = 0; k < kHeadBufs; k++) nb += gHeadBufPhys[head][k] != 0;
    ALOG("swap: head %u: %lld swaps over %u buffer(s) now (0x%llx 0x%llx 0x%llx 0x%llx), %lld distinct so far, %lld "
         "buffer changes; front (framebuffer resource) %s0x%llx; %lld skipped (empty back), %lld of them with pixels in "
         "the front, %lld empty backs shown; copy forward: %lld from the shown buffer, %lld from the front, %lld not "
         "needed (one back), %lld nothing shown yet, %lld another surface's, %lld missed, %lld healed; all heads: %lld "
         "no head, %lld shown buffer not usable, %lld refused, %lld swaps with no head; flicker %lld A B A B of %lld "
         "hashed, %lld in more than one band (rdna3_swap_front.h, card run 58b)", head, (long long)n, nb,
         gHeadBufPhys[head][0], gHeadBufPhys[head][1], gHeadBufPhys[head][2], gHeadBufPhys[head][3],
         (long long)gCntBufNew[head], (long long)gCntSwapBufChg[head], front ? "" : "not named by this swap, ", front,
         (long long)gCntSwapSkipHead[head], (long long)gCntSwapFrontSkip[head], (long long)gCntSwapEmptyShown[head],
         (long long)gCntCfShown[head], (long long)gCntCfSrc[head], (long long)gCntCfBackLast[head],
         (long long)gCntCfNothingShown[head], (long long)gCntCfForeign[head], (long long)gCntCfMiss[head],
         (long long)gCntCfHealed[head], (long long)gCntCfNoHead, (long long)gCntCfShownStale, (long long)gCntCfRefused,
         (long long)gCntSwapNoHead, (long long)gCntSwapAbab[head], (long long)gCntSwapHashed[head],
         (long long)gCntSwapAbabWide[head]);
}

/* a head's new buffer (one not in its current set): counted, and logged
 * with both arguments' VRAM, which one is the front, and the route, 12 per
 * head and surface (the route log cap of 200 ran out at swap #289 in card
 * run 58b) */
static void rd3AccelSwapNewBuffer(unsigned head, unsigned n, unsigned long long phys, unsigned long long p1,
                                  unsigned long long p2, int front, unsigned pick, const char *how)
{
    unsigned k = kHeadBufs;
    for (unsigned i = 0; i < kHeadBufs && k == kHeadBufs; i++)
        if (gHeadBufPhys[head][i] == phys) k = i;
    if (k < kHeadBufs) return;
    for (unsigned i = 0; i < kHeadBufs && k == kHeadBufs; i++)
        if (!gHeadBufPhys[head][i]) k = i;
    if (k == kHeadBufs) {   /* full: start the set again */
        for (unsigned i = 1; i < kHeadBufs; i++) gHeadBufPhys[head][i] = 0;
        k = 0;
    }
    gHeadBufPhys[head][k] = phys;
    const SInt64 nn = OSAddAtomic64(1, &gCntBufNew[head]) + 1;
    if (gHeadBufLogs[head] >= 12) return;
    gHeadBufLogs[head]++;
    ALOG("swap #%u: head %u shows a new buffer phys 0x%llx (a1 0x%llx a2 0x%llx, front %s, source a%u, %s; %lld "
         "distinct so far)", n, head, phys, p1, p2, front == 1 ? "a1" : front == 2 ? "a2" : "not named", pick, how,
         (long long)nn);
}

/* a swap that shows nothing: counted (all heads, and per head when the
 * front named it) and logged with why, the first 8 and every 1024th */
static void rd3AccelSwapSkipped(unsigned frontHead, int front, unsigned rc1, unsigned rc2, unsigned run,
                                unsigned long long p1, unsigned long long p2, bool backMapped)
{
    const SInt64 k = OSAddAtomic64(1, &gCntSwapSkipped);
    const unsigned rcBack = front == 1 ? rc2 : rc1, rcFront = front == 1 ? rc1 : rc2;
    if (frontHead < kRd3MaxFB) {
        OSAddAtomic64(1, &gCntSwapSkipHead[frontHead]);
        if (rcFront == kRcPixels) OSAddAtomic64(1, &gCntSwapFrontSkip[frontHead]);
    }
    if (k < 8 || (k & 1023) == 0) {
        if (front)
            ALOG("swap: head %u: the back (phys 0x%llx) shows nothing (%s), %u skipped in a row -- skipped, the front "
                 "(phys 0x%llx, %s) never goes to the panel; %s (%lld skipped so far)", frontHead,
                 front == 1 ? p2 : p1, kRcWhy[rcBack < 6 ? rcBack : 0], run, front == 1 ? p1 : p2,
                 kRcWhy[rcFront < 6 ? rcFront : 0],
                 backMapped ? "an empty back is shown once 3 swaps in a row were skipped (a black or clear frame)"
                            : "a back without a mapping or geometry is never shown", (long long)k + 1);
        else
            ALOG("swap: no front named, neither buffer shows anything (a1 phys 0x%llx: %s, a2 phys 0x%llx: %s) -- "
                 "skipped (%lld so far)", p1, kRcWhy[rc1 < 6 ? rc1 : 0], p2, kRcWhy[rc2 < 6 ? rc2 : 0], (long long)k + 1);
    }
}

/* ---- the swap as a copy (card run 2026-10-17b, rdna3_swap_copy.h) ----
 * Each head's state in modes 1/2: the accelerator's second scanout buffer
 * D (VRAM of the accelerator's own, freed only off the panel once the flip
 * away latched), what the panel shows, the last flip and whether it was
 * seen latched (with rdna3_flip_latch.h's backoff), the front the last swap
 * copied into (copy forward's source), whether the panel holds a frame the
 * accelerator copied (a picture an empty back must not replace), home's
 * address (the PANEL events' expectation).
 * Written only under the family's lock: by the swaps, copy forward and the
 * swap mode's change. Whoever moves the panel without that lock (the
 * display mode change, IOP flips) posts what it did (rd3AccelScPost); the
 * next holder of the lock applies it (rd3AccelScApply, ECC review round 1).
 * The front record alone is cleared from the free paths, by a
 * compare-and-swap of the freed allocation. */
struct RD3ScHead {
    void *dHandle, *dKva;
    unsigned long long dPhys, dCookie, dBytes;
    unsigned dW, dH, dPitch;
    int shows;                          /* enum rd3_sc_shows */
    bool flipped, latched;              /* the head's last flip; seen latched */
    bool flipFrameKnown;                /* flipFrame is that flip's OTG frame (not for posted flips) */
    uint64_t flipAbs;
    uint32_t flipFrame;
    struct rd3_latch_state lat;         /* the waits' backoff (3 timeouts in a row: only looks) */
    struct rd3_sc_front front;          /* the front our last copy reached, and its allocation */
    bool scanoutFrame;                  /* the panel shows a frame this mode copied */
    bool lastPixels;                    /* ... and it had pixels */
    uint8_t lastBand[8];                /* ... and its source's band bytes at the swap (the landed buffer's "shown") */
    unsigned dRetry;                    /* swaps until D's allocation is tried again */
    unsigned long long homePhys;        /* the framebuffer's own scanout (home), as last described */
    bool seen;                          /* the last flip was SEEN latched (not a wait that gave up): D's free */
    uint64_t ownFlipAbs;                /* this head's last flip of the swaps' own (posts older are stale) */
};
static RD3ScHead gSc[kRd3MaxFB];
/* what moved a head's panel outside the family's lock, the last one wins */
enum { kScPostNone = 0, kScPostModeSet = 1, kScPostAway = 2 };
static volatile UInt32 gScPost[kRd3MaxFB];
static volatile uint64_t gScPostAbs[kRd3MaxFB];
/* the SDMA breaker (rd3_sc_brk): under the family's lock (the swaps) */
static struct rd3_sc_brk gScBrk;
enum { kRd3Etime = -62 };   /* RDNA3_ETIME (hw/rdna3_hw.h): the copy's fence did not come */
/* every outcome counted per head (debug.rdna3accel_swaps_copy*_h<N>,
 * cf_sc*_h<N>): copied and shown, empty backs held over a picture (mode 1),
 * empty backs copied (black frames), bytes, the copies' and the waits'
 * time, waits that timed out, failures, copies straight into the buffer on
 * the panel (no D: may tear), front copies and their failures; copy
 * forward's source in modes 1/2; swaps not waited for (waits off after 3
 * timeouts), swaps flipped while the SDMA breaker is open, old D's whose
 * free waited for a latch; the breaker's trips */
static SInt64 gCntScCopy[kRd3MaxFB], gCntScEmpty[kRd3MaxFB], gCntScBlack[kRd3MaxFB], gCntScBytes[kRd3MaxFB];
static SInt64 gCntScUs[kRd3MaxFB], gCntScUsMax[kRd3MaxFB], gCntScWait[kRd3MaxFB], gCntScWaitUs[kRd3MaxFB];
static SInt64 gCntScLatchTo[kRd3MaxFB], gCntScFail[kRd3MaxFB], gCntScDirect[kRd3MaxFB], gCntScFront[kRd3MaxFB];
static SInt64 gCntScFrontFail[kRd3MaxFB], gCntCfScSrc[kRd3MaxFB], gCntCfScScanout[kRd3MaxFB], gCntCfScStale[kRd3MaxFB];
static SInt64 gCntScNoWait[kRd3MaxFB], gCntScBrkFlip[kRd3MaxFB], gCntScFreeWait[kRd3MaxFB], gCntScBrkTrips;
static SInt64 gCntScLanded[kRd3MaxFB];   /* the landed buffer presented instead of the named back (2026-10-17c) */
static SInt64 gCntScLandedWould[kRd3MaxFB];   /* ... found but not presented (debug.rdna3accel_swap_landed=0) */
static SInt64 gCntScStale[kRd3MaxFB];   /* backs WindowServer did not redraw since presented, held (2026-10-17c) */
static void rd3AccelSwapCopy(unsigned head, IOService *fb, IOAccelResource2 *back, IOAccelResource2 *frontRes,
                             bool backEmpty, unsigned n, struct rd3_trace_ev *tr, const uint8_t *srcBand);
static void rd3AccelScBrkEvent(enum rd3_sc_brk_ev ev, unsigned n, unsigned head, int status, IOReturn r);
static void rd3AccelScLatchEvent(unsigned head, enum rd3_latch_event e, const char *why);
static inline uint64_t rd3AccelUsNow(void)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
/* the swaps copy now: modes 1/2 and the SDMA breaker not open (copy
 * forward's choice of source follows the swaps') */
static inline bool rd3AccelScCopying(void)
{
    return gSwapMode != RD3_SWAPMODE_FLIP && !(gScBrk.off && !gScBrk.probing);
}

/* 'at': when the move was issued (mach_absolute_time taken BEFORE the flip
 * call, ECC review round 3: a time taken after it returned could postdate
 * a swap's own later flip and undo it) */
static void rd3AccelScPost(unsigned head, UInt32 what, uint64_t at)
{
    if (head >= kRd3MaxFB) return;
    gScPostAbs[head] = at;
    __atomic_store_n(&gScPost[head], what, __ATOMIC_RELEASE);
}
static void rd3AccelScModeChanged(void)
{
    /* the framebuffer's mode set programmed its own scanout (home) */
    const uint64_t at = mach_absolute_time();
    for (unsigned h = 0; h < kRd3MaxFB; h++) rd3AccelScPost(h, kScPostModeSet, at);
}
static void rd3AccelScPanelMoved(unsigned head, const char *why, uint64_t issuedAt, bool foreign)
{
    rd3AccelScPost(head, kScPostAway, issuedAt);   /* not D (a copy into D is safe once this flip latched) */
    /* someone else's picture on the panel (an IOP surface): the head's next swap presents whatever it holds
     * (the write generations, rdna3_write_gen.h); a mode 0 flip of the swaps' own is no such move */
    if (foreign) rd3AccelWgHeadReset(head, why);
}
/* under the family's lock: what moved the panels since the last holder;
 * a post older than the head's own last flip is stale (it was in flight
 * while a swap flipped: rd3_sc_post_applies, ECC review round 2) */
static unsigned gScStalePosts;
static void rd3AccelScApply(unsigned head)
{
    if (head >= kRd3MaxFB) return;
    const UInt32 what = __atomic_exchange_n(&gScPost[head], (UInt32)kScPostNone, __ATOMIC_ACQ_REL);
    if (what == kScPostNone) return;
    RD3ScHead &S = gSc[head];
    const uint64_t at = gScPostAbs[head];
    if (!rd3_sc_post_applies(at, S.ownFlipAbs)) {
        if (rd3_log_due(gScStalePosts++, 4, 1023))
            ALOG("swap copy: head %u: a %s posted before the swaps' own last flip -- ignored (that flip is on the "
                 "panel)", head, what == kScPostModeSet ? "display mode change" : "panel move");
        return;
    }
    if (what == kScPostModeSet) {
        S.shows = RD3_SCS_HOME;
        S.flipped = false;
        S.latched = true;
        S.seen = true;             /* the mode set itself waited for the new surface */
        S.scanoutFrame = false;
        S.homePhys = 0;            /* the new mode's home is learned at the next copy (ECC review round 2) */
        rd3AccelScStaleReset(head, "a display mode change");
        rd3AccelWgHeadReset(head, "a display mode change");
        rd3AccelScLatchEvent(head, rd3_latch_rearm(&S.lat), "a display mode change re-armed them");
    } else if (what == kScPostAway) {
        S.shows = RD3_SCS_OTHER;
        S.scanoutFrame = false;
        S.flipped = true;          /* someone's flip: D may still be read until it latched */
        S.latched = false;
        S.seen = false;
        S.flipFrameKnown = false;
        S.flipAbs = at;
        rd3_latch_flipped(&S.lat);
    }
}
static void rd3AccelScApplyAll(void)
{
    for (unsigned h = 0; h < kRd3MaxFB; h++) rd3AccelScApply(h);
}
/* what the head's HUBP should scan as far as modes 1/2 know (the PANEL
 * events' expectation, Q6): D, home, 0 when someone else's buffer */
static unsigned long long rd3AccelScExpect(unsigned head)
{
    if (head >= kRd3MaxFB) return 0;
    const RD3ScHead &S = gSc[head];
    return S.shows == RD3_SCS_D ? S.dPhys : S.shows == RD3_SCS_HOME ? S.homePhys : 0;
}
/* a freed allocation leaves every head's front record (any thread: a
 * compare-and-swap, the swaps write the record under the family's lock and
 * hold the front they copy into, so it is never the one being freed) */
static unsigned gScFrontGoneLogs;
static void rd3AccelScFrontGone(const void *vm)
{
    for (unsigned h = 0; vm && h < kRd3MaxFB; h++) {
        const void *expect = vm;
        if (__atomic_compare_exchange_n(&gSc[h].front.vm, &expect, (const void *)nullptr, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            if (rd3_log_due(gScFrontGoneLogs++, 4, 1023))
                ALOG("swap copy: head %u: the front the last swap copied into was freed -- copy forward reads the "
                     "panel's buffer until the next swap's copy", h);
        }
    }
}

/* ---- the stale back (card run 2026-10-17c, rdna3_swap_copy.h) ----
 * Evidence-driven, no amdgpu analogue (macOS legacy surface semantics).
 * Each head's table (gScStale[h]) is touched only under gScStaleLock, a
 * spinlock held for the table operations alone (no BAR reads, no logs, no
 * sleep under it): by the swaps (the check, the presentations), copy
 * forward (its target marked in every head's table), the swap / display
 * mode changes and D's reallocation (resets), the surface's and the pipe
 * resource's free paths (resets) and the VRAM free path (the address
 * forgotten). Every reset, eviction, free and reason is counted per head
 * (debug.rdna3accel_swaps_stale*_h<N>) and the first ones logged. */
static struct rd3_sc_stale gScStale[kRd3MaxFB];
static SInt64 gCntScStaleWould[kRd3MaxFB], gCntScStaleMiss[kRd3MaxFB], gCntScStaleNocf[kRd3MaxFB];
static SInt64 gCntScStaleChanged[kRd3MaxFB], gCntScStaleReset[kRd3MaxFB], gCntScStaleEvict[kRd3MaxFB];
static SInt64 gCntScStaleGone[kRd3MaxFB];
/* why a mode 1 back with pixels was not a hold candidate (rd3_sc_stale_why), and no head known */
static SInt64 gCntScStaleNoRec[kRd3MaxFB], gCntScStaleChg[kRd3MaxFB], gCntScStaleCfd[kRd3MaxFB];
static SInt64 gCntScStaleSame[kRd3MaxFB], gCntScStaleNoPanel[kRd3MaxFB], gCntScStaleNoHead[kRd3MaxFB];
static unsigned gStaleResamples[kRd3MaxFB];
static void rd3AccelScStaleReset(unsigned head, const char *why)
{
    if (head >= kRd3MaxFB || !gScStaleLock) return;
    IOSimpleLockLock(gScStaleLock);
    const unsigned had = rd3_sc_stale_reset(&gScStale[head]);
    IOSimpleLockUnlock(gScStaleLock);
    if (!had) return;
    const SInt64 k = OSAddAtomic64(1, &gCntScStaleReset[head]);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("swap copy: head %u: the stale-back records forgotten (%s; %u records) -- every back is copied at its "
             "next swap (%lld resets)", head, why, had, (long long)k + 1);
}
/* a freed VRAM address leaves every head's table (any thread) */
static void rd3AccelScStaleGone(unsigned long long phys)
{
    if (!phys || !gScStaleLock) return;
    unsigned gone = 0;
    IOSimpleLockLock(gScStaleLock);
    for (unsigned h = 0; h < kRd3MaxFB; h++)
        if (rd3_sc_stale_forget_addr(&gScStale[h], phys)) gone |= 1u << h;
    IOSimpleLockUnlock(gScStaleLock);
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        if (!(gone & (1u << h))) continue;
        const SInt64 k = OSAddAtomic64(1, &gCntScStaleGone[h]);
        if (rd3_log_due((uint64_t)k, 4, 1023))
            ALOG("swap copy: head %u: the back at 0x%llx was freed -- its stale-back record is forgotten (a new "
                 "buffer there is copied at its first swap; %lld such)", h, phys, (long long)k + 1);
    }
}
/* a copy forward targeted 'phys': marked in every head's table */
static void rd3AccelScStaleCf(unsigned long long phys)
{
    if (!phys || !gScStaleLock) return;
    IOSimpleLockLock(gScStaleLock);
    rd3_sc_stale_cf(gScStale, kRd3MaxFB, phys);
    IOSimpleLockUnlock(gScStaleLock);
}
/* swap n presented 'phys' on the head: its record (an eviction counted, logged) */
static void rd3AccelScStaleShown(unsigned head, unsigned long long phys, unsigned long long sig,
                                 unsigned long long sig2, unsigned n)
{
    if (head >= kRd3MaxFB || !gScStaleLock) return;
    IOSimpleLockLock(gScStaleLock);
    const unsigned long long ev = rd3_sc_stale_shown(&gScStale[head], phys, sig, sig2, n);
    IOSimpleLockUnlock(gScStaleLock);
    if (!ev) return;
    const SInt64 k = OSAddAtomic64(1, &gCntScStaleEvict[head]);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("swap #%u: head %u: the stale-back table is full (%u buffers): 0x%llx's record evicted for 0x%llx -- "
             "0x%llx is copied at its next swap whatever it holds (%lld such)", n, head, RD3_SC_STALE_SLOTS, ev, phys,
             ev, (long long)k + 1);
}
static enum rd3_sc_stale_why rd3AccelScStaleCheck(unsigned head, int panelFrame, unsigned long long phys,
                                                  unsigned long long sig, unsigned *at, int *nocf,
                                                  unsigned long long *after)
{
    *after = 0;
    if (head >= kRd3MaxFB || !gScStaleLock) return RD3_SCST_OFF;
    IOSimpleLockLock(gScStaleLock);
    const enum rd3_sc_stale_why why =
        rd3_sc_stale_check(&gScStale[head], RD3_SWAPMODE_COPY, 1, panelFrame, phys, sig, at, nocf);
    *after = gScStale[head].lastShown;
    IOSimpleLockUnlock(gScStaleLock);
    return why;
}
static bool rd3AccelScStaleConfirm(unsigned head, unsigned long long phys, unsigned long long sig2)
{
    if (head >= kRd3MaxFB || !gScStaleLock) return false;
    IOSimpleLockLock(gScStaleLock);
    const bool ok = rd3_sc_stale_confirm(&gScStale[head], phys, sig2);
    IOSimpleLockUnlock(gScStaleLock);
    return ok;
}
/* the reasons, counted per head; a change without a copy forward (the
 * rule's assumption broken) logged, the first 8 then every 1024th */
static void rd3AccelScStaleWhy(unsigned head, enum rd3_sc_stale_why why, int nocf, unsigned n,
                               unsigned long long phys, unsigned at)
{
    if (head >= kRd3MaxFB) return;
    SInt64 *const c[8] = { nullptr, nullptr, &gCntScStaleNoRec[head], &gCntScStaleChg[head], &gCntScStaleCfd[head],
                           &gCntScStaleSame[head], &gCntScStaleNoPanel[head], nullptr };
    if ((unsigned)why < 8 && c[why]) OSAddAtomic64(1, c[why]);
    if (!nocf) return;
    const SInt64 k = OSAddAtomic64(1, &gCntScStaleNocf[head]);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("swap #%u: head %u: the back 0x%llx changed since it was shown at swap #%u although no copy forward "
             "targeted it -- copied (WindowServer redrew a back without copy forward: the stale-back rule's "
             "assumption; %lld such)", n, head, phys, at, (long long)k + 1);
}

/* ---- the write generations (card run 2026-10-17d, rdna3_write_gen.h) ----
 * Evidence-driven, no amdgpu analogue (macOS legacy surface semantics). One
 * table of buffer addresses and the generation of their last write (the
 * family's page-on: addToAperture / getApertureMemoryDescriptor; a copy
 * forward's target, when the copy went through) and each head's last
 * presented frame, all under gWgLock, a spinlock held for the table
 * operations alone (no logs, no BAR reads, no sleep under it). A swap
 * presents its back only when it was written after the head's frame;
 * otherwise mode 1 first presents the surface's buffer written after that
 * frame, if one is (rd3_wg_landed_pick), and else holds: the panel keeps its
 * frame. A hold never freezes the panel: after 250 ms and 3 holds in a row,
 * or while a newer generation was evicted, the back presents (ECC review
 * round 1). Every judged swap is counted per head so that
 *   swaps_gen_judged = swaps_genpresent + swaps_genhold + swaps_genhold_would + swaps_gen_landed
 * and the swaps not judged too (swaps_gennotjudged_h<N>), traced (SWAP v1
 * bits 25-31, v2 >> 16), the first ones of each kind logged. */
static struct rd3_wg gWg;
static struct rd3_wg_head gWgHead[kRd3MaxFB];
static SInt64 gCntWgAperture, gCntWgApCached, gCntWgCf, gCntWgCfNoStamp, gCntWgEvicted, gCntWgFreed, gCntWgResets;
static SInt64 gCntWgPresent[kRd3MaxFB], gCntWgHold[kRd3MaxFB], gCntWgWould[kRd3MaxFB], gCntWgFirst[kRd3MaxFB];
static SInt64 gCntWgContent[kRd3MaxFB], gCntWgUnwritten[kRd3MaxFB], gCntCfGenNewer[kRd3MaxFB];
static SInt64 gCntWgJudged[kRd3MaxFB], gCntWgAged[kRd3MaxFB], gCntWgEvictPresent[kRd3MaxFB], gCntWgLanded[kRd3MaxFB];
static SInt64 gCntWgNotJudged[kRd3MaxFB], gCntWgResetsH[kRd3MaxFB];
static unsigned gWgHoldLogs[kRd3MaxFB], gWgPresentLogs[kRd3MaxFB], gWgWouldLogs[kRd3MaxFB], gWgGuardLogs[kRd3MaxFB];
static unsigned gWgLandedLogs[kRd3MaxFB], gWgNotJudgedLogs[kRd3MaxFB];
/* each head's last swapping surface and when it swapped (compare-only: never followed) - a reset only when another
 * surface takes over after the last one went quiet (ECC review round 1: surfaces taking turns must not reset) */
static const void *volatile gWgSurf[kRd3MaxFB];
static uint64_t gWgSurfAbs[kRd3MaxFB];
#define RD3_WG_SURF_QUIET_US 500000ull
static const char *const kWgWhy[RD3_WGW_MAX + 1] = {
    "not judged", "written after the head's last frame", "the first swap since the head's reset",
    "NOT written since, but its samples changed since this head presented it (a writer the accelerator does not see)",
    "the buffer the panel shows, not written since", "written before the head's last frame",
    "never written (no page-on, no copy forward into it) and never presented on this head",
    "the AGE GUARD: no present for 250 ms and 3 holds in a row (a hold never freezes the panel)",
    "the EVICTION GUARD: no generation, and a newer one was evicted from the full table (it may be this one)" };
static unsigned long long rd3AccelWgBumpPhys(unsigned long long phys, unsigned why)
{
    if (!phys || !gWgLock) return 0;
    unsigned long long ev = 0;
    IOSimpleLockLock(gWgLock);
    const unsigned long long g = rd3_wg_bump(&gWg, phys, &ev);
    IOSimpleLockUnlock(gWgLock);
    OSAddAtomic64(1, why == kWgCf ? &gCntWgCf : why == kWgApCached ? &gCntWgApCached : &gCntWgAperture);
    if (ev) {
        const SInt64 k = OSAddAtomic64(1, &gCntWgEvicted);
        if (rd3_log_due((uint64_t)k, 8, 1023))
            ALOG("write generations: the table is full (%u buffers): 0x%llx's generation evicted for 0x%llx -- "
                 "0x%llx reads 'never written' until its next write; while its generation is newer than a head's "
                 "frame a swap of a buffer without one presents (the eviction guard) (%lld such)",
                 RD3_WG_SLOTS, ev, phys, ev, (long long)k + 1);
    }
    return g;
}
/* a write into a resource's VRAM ('why': kWgAperture, kWgApertureDesc, kWgApCached, kWgCf); any thread */
static void rd3AccelWgWrite(const void *res, unsigned why)
{
    const RD3VidMemory *m = rd3res_vidmem((IOAccelResource2 *)res);
    if (m && m->fPhys) (void)rd3AccelWgBumpPhys(m->fPhys, why);
}
static unsigned long long rd3AccelWgGen(unsigned long long phys)
{
    if (!phys || !gWgLock) return 0;
    IOSimpleLockLock(gWgLock);
    const unsigned long long g = rd3_wg_gen(&gWg, phys);
    IOSimpleLockUnlock(gWgLock);
    return g;
}
static unsigned long long rd3AccelWgHeadGen(unsigned head)
{
    if (head >= kRd3MaxFB || !gWgLock) return 0;
    IOSimpleLockLock(gWgLock);
    const unsigned long long g = gWgHead[head].gen;
    IOSimpleLockUnlock(gWgLock);
    return g;
}
/* the VRAM at 'phys' was freed (any thread): its generation and every head's samples of it go */
static void rd3AccelWgFreed(unsigned long long phys)
{
    if (!phys || !gWgLock) return;
    IOSimpleLockLock(gWgLock);
    int had = rd3_wg_forget(&gWg, phys);
    for (unsigned h = 0; h < kRd3MaxFB; h++) had |= rd3_wg_head_forget(&gWgHead[h], phys);
    IOSimpleLockUnlock(gWgLock);
    if (had) OSAddAtomic64(1, &gCntWgFreed);
}
/* the head's panel moved without a swap: its next swap presents (any thread) */
static unsigned gWgResetLogs;
static void rd3AccelWgHeadReset(unsigned head, const char *why)
{
    if (head >= kRd3MaxFB || !gWgLock) return;
    IOSimpleLockLock(gWgLock);
    const int had = rd3_wg_head_reset(&gWgHead[head]);
    IOSimpleLockUnlock(gWgLock);
    if (!had) return;
    OSAddAtomic64(1, &gCntWgResetsH[head]);
    const SInt64 k = OSAddAtomic64(1, &gCntWgResets);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("write generations: head %u reset (%s) -- its next swap presents whatever its back holds (%lld resets)",
             head, why, (long long)k + 1);
}
/* a swap of 'surf' on the head: another surface taking over after the last one was quiet for 500 ms resets the
 * head (its buffers' writes were never compared with this head's frame); surfaces taking turns do not */
static void rd3AccelWgSurfSwap(unsigned head, const void *surf)
{
    if (head >= kRd3MaxFB || !surf) return;
    const void *prev = gWgSurf[head];
    if (prev && prev != surf && rd3AccelUsSince(gWgSurfAbs[head]) > RD3_WG_SURF_QUIET_US)
        rd3AccelWgHeadReset(head, "another surface swaps on the head, the last one quiet for 500 ms");
    gWgSurf[head] = surf;
    gWgSurfAbs[head] = mach_absolute_time();
}
/* the rule on the back at 'phys' for the head: why, and its generation and
 * the head's (for the trace and the log) */
static enum rd3_wg_why rd3AccelWgDecide(unsigned head, unsigned long long phys, bool sampled,
                                        unsigned long long sig, unsigned long long *gen, unsigned long long *headGen)
{
    *gen = 0;
    *headGen = 0;
    if (head >= kRd3MaxFB || !gWgLock || !phys) return RD3_WGW_OFF;
    const unsigned long long now = rd3AccelUsNow();
    IOSimpleLockLock(gWgLock);
    *gen = rd3_wg_gen(&gWg, phys);
    *headGen = gWgHead[head].gen;
    const enum rd3_wg_why w = rd3_wg_decide(&gWgHead[head], phys, *gen, sampled ? 1 : 0, sig, gWg.evictedMax, now);
    IOSimpleLockUnlock(gWgLock);
    return w;
}
/* the head presented the buffer at 'phys' with generation 'gen' (as captured at the decision: a write between the
 * decision and the copy is not this frame's) and its samples */
static void rd3AccelWgPresented(unsigned head, unsigned long long phys, unsigned long long gen, bool sampled,
                                unsigned long long sig)
{
    if (head >= kRd3MaxFB || !gWgLock || !phys) return;
    const unsigned long long now = rd3AccelUsNow();
    IOSimpleLockLock(gWgLock);
    rd3_wg_presented(&gWgHead[head], phys, gen, sampled ? 1 : 0, sig, now);
    IOSimpleLockUnlock(gWgLock);
}
/* a swap the rule could not judge (no head known at its entry, no VRAM address): counted, the first logged */
static void rd3AccelWgNotJudged(unsigned head, unsigned n, const char *why)
{
    if (head >= kRd3MaxFB) return;
    const SInt64 k = OSAddAtomic64(1, &gCntWgNotJudged[head]);
    if (rd3_log_due(gWgNotJudgedLogs[head]++, 8, 1023))
        ALOG("swap #%u: head %u: the write generations did not judge this swap: %s -- presented as before (%lld "
             "such; debug.rdna3accel_swaps_gennotjudged_h%u)", n, head, why, (long long)k + 1, head);
}
/* mode 1, before a hold: the surface's buffer written after the head's frame (the newest), not the back nor the
 * head's framebuffer resource - WindowServer's frame in a buffer the swap did not name. Its array index and
 * generation; nullptr none. bufs are the swapping surface's own (live through the family's lock). */
static IOAccelResource2 *rd3AccelWgLanded(unsigned head, IOAccelResource2 *back, IOAccelResource2 *fbRes,
                                          IOAccelResource2 *const *bufs, unsigned nbufs, unsigned long long headGen,
                                          unsigned *idx, unsigned long long *gen)
{
    enum { kMax = 8 };   /* a display surface has 2-3 buffers; the rest is not searched (kernel stack) */
    unsigned long long g[kMax];
    IOAccelResource2 *r[kMax];
    unsigned n = 0;
    for (unsigned i = 0; bufs && i < nbufs && i < kMax; i++) {
        r[i] = OSDynamicCast(IOAccelResource2, (OSObject *)bufs[i]);
        g[i] = r[i] && r[i] != back && r[i] != fbRes ? rd3AccelWgGen(rd3res_phys(r[i])) : 0;
        n = i + 1;
    }
    const int pk = rd3_wg_landed_pick(g, n, headGen);
    if (pk < 0) return nullptr;
    *idx = (unsigned)pk;
    *gen = g[pk];
    (void)head;
    return r[pk];
}
/* the decision's counters, trace bits and log lines. 'apply': the rule
 * acts (swap_gen 1, modes 0 and 1); 'landPhys' / 'landGen': mode 1's
 * newer buffer of the surface presented instead of a hold (0: none);
 * returns true when the swap holds */
static bool rd3AccelWgNote(unsigned head, enum rd3_wg_why why, bool apply, unsigned n, unsigned long long phys,
                           unsigned long long gen, unsigned long long headGen, const char *what,
                           unsigned long long landPhys, unsigned long long landGen, struct rd3_trace_ev *tr)
{
    if (why == RD3_WGW_OFF || head >= kRd3MaxFB) return false;
    OSAddAtomic64(1, &gCntWgJudged[head]);
    tr->v1 |= ((unsigned)why & RD3_TRV1_GEN_WHY_MASK) << RD3_TRV1_GEN_WHY_SHIFT;
    tr->v2 = (tr->v2 & 0xffffu) | (rd3_wg_gen16(gen) << 16);
    if (rd3_wg_presents(why)) {
        const SInt64 k = OSAddAtomic64(1, &gCntWgPresent[head]);
        if (why == RD3_WGW_FIRST) OSAddAtomic64(1, &gCntWgFirst[head]);
        if (why == RD3_WGW_CONTENT || why == RD3_WGW_AGED || why == RD3_WGW_EVICTED) {
            SInt64 *const c = why == RD3_WGW_CONTENT ? &gCntWgContent[head]
                            : why == RD3_WGW_AGED ? &gCntWgAged[head] : &gCntWgEvictPresent[head];
            const SInt64 kc = OSAddAtomic64(1, c);
            if (rd3_log_due(gWgGuardLogs[head]++, 16, 1023))
                ALOG("swap #%u: head %u: %s 0x%llx presented by a guard: %s (generation %llu, the head's %llu; %lld "
                     "such of this guard)", n, head, what, phys, kWgWhy[why], gen, headGen, (long long)kc + 1);
        } else if (rd3_log_due(gWgPresentLogs[head]++, 8, 4095)) {
            ALOG("swap #%u: head %u: %s 0x%llx presented: %s (generation %llu, the head's %llu; %lld such)", n, head,
                 what, phys, kWgWhy[why], gen, headGen, (long long)k + 1);
        }
        return false;
    }
    if (why == RD3_WGW_UNWRITTEN) OSAddAtomic64(1, &gCntWgUnwritten[head]);
    if (!apply) {
        tr->v1 |= RD3_TRV1_GEN_WOULD;
        const SInt64 k = OSAddAtomic64(1, &gCntWgWould[head]);
        if (rd3_log_due(gWgWouldLogs[head]++, 8, 1023))
            ALOG("swap #%u: head %u: would hold %s 0x%llx: %s (generation %llu, the head's %llu) -- %s, so it goes on "
                 "as before (%lld such)", n, head, what, phys, kWgWhy[why], gen, headGen,
                 gSwapGen ? "swap mode 2 (the copy-everything control)" : "debug.rdna3accel_swap_gen=0",
                 (long long)k + 1);
        return false;
    }
    if (landPhys) {
        const SInt64 k = OSAddAtomic64(1, &gCntWgLanded[head]);
        if (rd3_log_due(gWgLandedLogs[head]++, 8, 1023))
            ALOG("swap #%u: head %u: %s 0x%llx would be held (%s) -- the surface's buffer 0x%llx, written after the "
                 "head's frame (generation %llu, the head's %llu), presented instead (%lld such; "
                 "debug.rdna3accel_swaps_gen_landed_h%u)", n, head, what, phys, kWgWhy[why], landPhys, landGen,
                 headGen, (long long)k + 1, head);
        return false;
    }
    tr->v1 |= RD3_TRV1_GEN_HOLD;
    tr->arg = RD3_TRS_HOLD;
    tr->v3 = (uint32_t)headGen;
    if (gWgLock) {
        IOSimpleLockLock(gWgLock);
        rd3_wg_held(&gWgHead[head]);   /* the run the age guard counts */
        IOSimpleLockUnlock(gWgLock);
    }
    const SInt64 k = OSAddAtomic64(1, &gCntWgHold[head]);
    if (rd3_log_due(gWgHoldLogs[head]++, 8, 1023))
        ALOG("swap #%u: head %u: %s 0x%llx held: %s (generation %llu, the head's %llu) -- no flip, no copy, the panel "
             "keeps its frame (%lld such)", n, head, what, phys, kWgWhy[why], gen, headGen, (long long)k + 1);
    return true;
}

/* the 8 x 8 samples of a buffer (rd3AccelResContent's grid), all of them:
 * its content state (kRc*) and one band byte per row of samples, the
 * trace's signature (rdna3_trace.h). Read through the BAR mapping after
 * the GPU (SDMA, the plugin's draws) wrote the buffer: amdgpu invalidates
 * the HDP read cache before CPU reads of GPU-written VRAM with
 * amdgpu_device_invalidate_hdp -> amdgpu_asic_invalidate_hdp, which on
 * Navi 32 is a no-op (soc21_asic_funcs has no invalidate_hdp and
 * hdp_v6_0_funcs only flush_hdp; hw/rdna3_hw.c's rdna3_hdp_invalidate is the
 * same no-op), so nothing is due here (ECC review round 1; the trace's
 * headers carry RD3_TRACE_HDR_HDP_NOINV). */
struct RD3Scan {
    unsigned rc;
    uint8_t band[8];
    unsigned long long sig;   /* the 64 samples' hash (rd3_sc_sig_add: the stale back's "unchanged", 2026-10-17c) */
};
/* one grid's 64 samples (rdna3_swap_copy.h rd3_sc_grid_index: grid 0 the
 * trace's, grid 1 shifted by half a cell); a point outside the allocation reads 0 */
static void rd3AccelPxGrid(const volatile void *kva, unsigned long long bytes, unsigned w, unsigned h, unsigned pitch,
                           unsigned grid, uint32_t *v)
{
    const volatile unsigned int *px = (const volatile unsigned int *)kva;
    for (unsigned k = 0; k < RD3_SC_GRID_N; k++) {
        unsigned long i = 0;
        v[k] = rd3_sc_grid_index(grid, w, h, pitch, bytes, k, &i) ? px[i] : 0;
    }
}
static void rd3AccelPxScan(const volatile void *kva, unsigned long long bytes, unsigned w, unsigned h, unsigned pitch,
                           RD3Scan *o)
{
    uint32_t v[RD3_SC_GRID_N];
    bool any = false;
    rd3AccelPxGrid(kva, bytes, w, h, pitch, 0, v);
    for (unsigned k = 0; k < RD3_SC_GRID_N; k++) any |= v[k] != 0;
    for (unsigned r = 0; r < 8; r++) o->band[r] = rd3_trace_band(&v[r * 8], 8);
    o->rc = any ? kRcPixels : kRcEmpty;
    o->sig = rd3_sc_sig64(v);
}
static void rd3AccelResScan(IOAccelResource2 *res, RD3Scan *o)
{
    bzero(o, sizeof *o);
    if (!res) { o->rc = kRcNoRes; return; }
    RD3VidMemory *vm = rd3res_vidmem(res);
    if (!vm) { o->rc = kRcNoVidmem; return; }
    if (!vm->fKva) { o->rc = kRcNoKva; return; }
    unsigned w, h, pitch;
    if (!rd3res_geom(res, &w, &h, &pitch)) { o->rc = kRcNoGeometry; return; }
    rd3AccelPxScan(vm->fKva, vm->fBytes, w, h, pitch, o);
}
/* the second grid's hash of a resource (the stale back's confirmation,
 * card run 2026-10-17c); false: no mapping or geometry */
static bool rd3AccelResSig2(IOAccelResource2 *res, unsigned long long *sig)
{
    *sig = 0;
    RD3VidMemory *vm = res ? rd3res_vidmem(res) : nullptr;
    unsigned w, h, pitch;
    if (!vm || !vm->fKva || !rd3res_geom(res, &w, &h, &pitch)) return false;
    uint32_t v[RD3_SC_GRID_N];
    rd3AccelPxGrid(vm->fKva, vm->fBytes, w, h, pitch, 1, v);
    *sig = rd3_sc_sig64(v);
    return true;
}
/* a stale back held at swap n read again ~4 ms later (the empty back's
 * re-sample, same budget: first 8, then every 256th, only with
 * swap_latch on): changed = a frame landed in it after the hold (counted,
 * logged; it reaches the panel at the back's next swap) */
static void rd3AccelStaleResample(unsigned head, IOAccelResource2 *back, unsigned long long sig, const void *surf,
                                  unsigned n, unsigned shownAt)
{
    if (head >= kRd3MaxFB || !back) return;
    const unsigned s = gStaleResamples[head]++;
    if (!rd3_empty_resample_due(gSwapLatch, s)) return;
    const uint64_t t0 = mach_absolute_time();
    IOSleep(4);
    RD3Scan sc;
    rd3AccelResScan(back, &sc);
    const uint64_t us = rd3AccelUsSince(t0);
    rd3AccelTraceScanFwd(head, surf, back, sc.rc, sc.band, 4u | 16u, n, rd3TrSat(us));
    if (sc.rc < kRcEmpty || sc.sig == sig) return;
    const SInt64 k = OSAddAtomic64(1, &gCntScStaleChanged[head]);
    if (rd3_log_due((uint64_t)k, 8, 1023))
        ALOG("swap #%u: head %u: the stale back 0x%llx held at this swap (unchanged since swap #%u) CHANGED %llu us "
             "later -- a frame landed after the hold; it reaches the panel at that back's next swap (%lld such)", n,
             head, rd3res_phys(back), shownAt, us, (long long)k + 1);
}

/* a vidmem without a resource (a free): 64 samples spread over its bytes */
static void rd3AccelVmScan(const RD3VidMemory *vm, RD3Scan *o)
{
    bzero(o, sizeof *o);
    if (!vm || !vm->fKva || vm->fBytes < 256) { o->rc = kRcNoKva; return; }
    const unsigned long long words = vm->fBytes / 4;
    bool any = false;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    for (unsigned b = 0; b < 8; b++) {
        uint32_t row[8];
        for (unsigned i = 0; i < 8; i++) {
            row[i] = px[(words * (b * 8 + i + 1)) / 65];
            any |= row[i] != 0;
        }
        o->band[b] = rd3_trace_band(row, 8);
    }
    o->rc = any ? kRcPixels : kRcEmpty;
}

/* each head's buffers as last sampled (by VRAM address): "changed since
 * this head last sampled it" for the trace's SCAN events and the landing
 * log (where did WindowServer's frame go?) */
enum { kSigSlots = 8 };
struct RD3BufSig { unsigned long long phys; uint8_t band[8]; };
static RD3BufSig gBufSig[kRd3MaxFB][kSigSlots];
static unsigned gBufSigNext[kRd3MaxFB];
/* stores 'band' for phys; returns 1 changed, 0 the same, -1 not seen before; 'prev' the old bands */
static int rd3AccelSigSwap(unsigned head, unsigned long long phys, const uint8_t *band, uint8_t *prev)
{
    if (head >= kRd3MaxFB || !phys) { bzero(prev, 8); return -1; }
    RD3BufSig *slot = nullptr;
    for (unsigned i = 0; i < kSigSlots && !slot; i++)
        if (gBufSig[head][i].phys == phys) slot = &gBufSig[head][i];
    int r = -1;
    if (slot) {
        memcpy(prev, slot->band, 8);
        r = memcmp(prev, band, 8) != 0;
    } else {
        bzero(prev, 8);
        slot = &gBufSig[head][gBufSigNext[head]++ % kSigSlots];
        slot->phys = phys;
    }
    memcpy(slot->band, band, 8);
    return r;
}
/* the index of 'res' in the surface's buffer array (shapeSurface's), 0xff none */
static unsigned rd3AccelBufIndex(IOAccelResource2 *const *bufs, unsigned nbufs, IOAccelResource2 *res)
{
    for (unsigned i = 0; bufs && res && i < nbufs && i < 0xff; i++)
        if (bufs[i] == res) return i;
    return 0xff;
}
/* one SCAN event; returns the change state (rd3AccelSigSwap) */
static int rd3AccelTraceScan(unsigned head, const void *surf, unsigned idx, unsigned long long phys, const RD3Scan &sc,
                             unsigned flags, unsigned n, unsigned v1 = 0, const void *res = nullptr)
{
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_SCAN, head);
    if (res) {   /* the join with the plugin's trace: the IOSurface id and the resource's type byte (round 2) */
        e.v3 = rd3AccelResSurfaceId(res);
        e.v4 = rd3AccelResType(res);
    }
    uint8_t prev[8];
    const int ch = rd3AccelSigSwap(head, phys, sc.band, prev);
    e.arg = (uint8_t)idx;
    e.flags = (uint8_t)(flags | (sc.rc == kRcPixels ? 1u : 0u) | (ch > 0 ? 2u : 0u));
    e.obj = rd3TrObj(surf);
    e.v0 = n;
    e.v1 = v1;
    e.v2 = sc.rc;
    e.pa = phys;
    memcpy(e.ba, sc.band, 8);
    memcpy(e.bb, prev, 8);
    rd3Trace(e);
    return ch;
}
static void rd3AccelResScanFwd(IOAccelResource2 *res, unsigned *rc, uint8_t *band)
{
    RD3Scan sc;
    rd3AccelResScan(res, &sc);
    *rc = sc.rc;
    memcpy(band, sc.band, 8);
}
static void rd3AccelTraceScanFwd(unsigned head, const void *surf, IOAccelResource2 *res, unsigned rc,
                                 const uint8_t *band, unsigned flags, unsigned n, unsigned v1)
{
    RD3Scan sc;
    sc.rc = rc;
    memcpy(sc.band, band, 8);
    rd3AccelTraceScan(head, surf, 0xff, rd3res_phys(res), sc, flags, n, v1, res);
}
/* the surface's buffers other than a1 / a2 as sampled at one swap: the
 * trace's SCAN events and the landed buffer's candidates come from one
 * sampling (at most RD3_SC_LANDED_MAX buffers, 64 BAR reads each) */
struct RD3Others {
    unsigned n;
    IOAccelResource2 *res[RD3_SC_LANDED_MAX];
    unsigned idx[RD3_SC_LANDED_MAX];
    unsigned long long phys[RD3_SC_LANDED_MAX];
    RD3Scan sc[RD3_SC_LANDED_MAX];
    int ch[RD3_SC_LANDED_MAX];
};
static void rd3AccelOthersSample(unsigned head, const void *surf, IOAccelResource2 *a1, IOAccelResource2 *a2,
                                 IOAccelResource2 *const *bufs, unsigned nbufs, unsigned n, RD3Others *o)
{
    o->n = 0;
    for (unsigned i = 0; bufs && i < nbufs && o->n < RD3_SC_LANDED_MAX; i++) {
        IOAccelResource2 *r = OSDynamicCast(IOAccelResource2, (OSObject *)bufs[i]);
        if (!r || r == a1 || r == a2) continue;
        const unsigned k = o->n++;
        o->res[k] = r;
        o->idx[k] = i;
        rd3AccelResScan(r, &o->sc[k]);
        o->phys[k] = rd3res_phys(r);
        o->ch[k] = rd3AccelTraceScan(head, surf, i, o->phys[k], o->sc[k], 0, n, 0, r);
    }
}
/* the surface's buffers other than a1 / a2, sampled (Q2: where did
 * WindowServer's frame land); 'landing' (the back read empty) also logs
 * the first ones of a head with every buffer's state; 'pre': this swap
 * already sampled them (the landed buffer's look), not sampled again */
static unsigned gLandingLogs[kRd3MaxFB];
static uint64_t gScanSwaps[kRd3MaxFB];   /* swaps that asked (rd3_trace_scan_due's count) */
static void rd3AccelScanOthers(unsigned head, const void *surf, IOAccelResource2 *a1, IOAccelResource2 *a2,
                               const RD3Scan &s1, const RD3Scan &s2, int ch1, int ch2, IOAccelResource2 *const *bufs,
                               unsigned nbufs, unsigned n, bool landing, const char *what,
                               const RD3Others *pre = nullptr)
{
    char line[400];
    size_t used = 0;
    const bool logIt = landing && head < kRd3MaxFB && rd3_log_due(gLandingLogs[head]++, 12, 1023);
    if (logIt)
        used = (size_t)snprintf(line, sizeof line, "a1 0x%llx %s%s, a2 0x%llx %s%s", rd3res_phys(a1),
                                s1.rc == kRcPixels ? "pixels" : kRcWhy[s1.rc < 6 ? s1.rc : 0],
                                ch1 > 0 ? " CHANGED" : ch1 == 0 ? " same" : " new", rd3res_phys(a2),
                                s2.rc == kRcPixels ? "pixels" : kRcWhy[s2.rc < 6 ? s2.rc : 0],
                                ch2 > 0 ? " CHANGED" : ch2 == 0 ? " same" : " new");
    const bool due = rd3_trace_scan_due(gTrScan, landing, head < kRd3MaxFB ? gScanSwaps[head]++ : 0, (unsigned)gSwapAb);
    RD3Others own;
    own.n = 0;
    if (!pre && due) rd3AccelOthersSample(head, surf, a1, a2, bufs, nbufs, n, &own);
    const RD3Others &o = pre ? *pre : own;
    for (unsigned k = 0; logIt && k < o.n && used < sizeof line; k++)
        used += (size_t)snprintf(line + used, sizeof line - used, ", buf[%u] 0x%llx %s%s", o.idx[k], o.phys[k],
                                 o.sc[k].rc == kRcPixels ? "pixels" : kRcWhy[o.sc[k].rc < 6 ? o.sc[k].rc : 0],
                                 o.ch[k] > 0 ? " CHANGED" : o.ch[k] == 0 ? " same" : " new");
    if (logIt)
        ALOG("swap #%u: head %u: %s -- where WindowServer's frame is (since this head's last look): %s", n, head,
             what, line);
}
/* The landed buffer (card run 2026-10-17c, rdna3_swap_copy.h). Evidence-
 * driven, no amdgpu analogue (macOS legacy surface semantics): head 0's
 * swaps named 0x8b100000 (IOSurface 37) and 0x8b8f0000 (type 0x40) in
 * turns, WindowServer's frame was in 0x8b100000 at all 159 swaps, the other
 * read empty at all of its 159. At a swap whose back is suspect
 * (rd3_sc_landed_wanted) the surface's other buffers - the array minus the
 * back and the head's framebuffer resource, plus a1 when it is neither -
 * are sampled now (o: the trace's SCAN events come from the same reads)
 * and rd3_sc_landed_pick chooses among them. Returns the buffer to copy,
 * nullptr: none (the old rule stands); *srcScan its samples, *idx its
 * index in the surface's array (RD3_TRV4_LANDED_A1: a1). */
static IOAccelResource2 *rd3AccelLanded(unsigned head, const void *surf, IOAccelResource2 *a1, IOAccelResource2 *a2,
                                        IOAccelResource2 *back, const RD3Scan &s1, int ch1, IOAccelResource2 *fbRes,
                                        IOAccelResource2 *const *bufs, unsigned nbufs, unsigned n, RD3Others *o,
                                        const RD3Scan **srcScan, unsigned *idx)
{
    rd3AccelOthersSample(head, surf, a1, a2, bufs, nbufs, n, o);
    const RD3ScHead &S = gSc[head];
    struct rd3_sc_cand c[RD3_SC_LANDED_MAX];
    IOAccelResource2 *cr[RD3_SC_LANDED_MAX];
    const RD3Scan *cs[RD3_SC_LANDED_MAX];
    unsigned ci[RD3_SC_LANDED_MAX];
    const bool a1Too = a1 && a1 != back && a1 != fbRes;
    unsigned m = 0;
    for (unsigned k = 0; k < o->n && m < RD3_SC_LANDED_MAX - (a1Too ? 1u : 0u); k++) {
        if (o->res[k] == back || o->res[k] == fbRes) continue;
        cr[m] = o->res[k];
        cs[m] = &o->sc[k];
        ci[m] = o->idx[k];
        c[m].changed = o->ch[k] > 0;
        m++;
    }
    if (a1Too) {   /* sampled at the swap's entry: its change state is ch1 (the reads are not repeated) */
        cr[m] = a1;
        cs[m] = &s1;
        ci[m] = RD3_TRV4_LANDED_A1;
        c[m].changed = ch1 > 0;
        m++;
    }
    for (unsigned k = 0; k < m; k++) {
        c[k].sampled = cs[k]->rc >= kRcEmpty;
        c[k].pixels = cs[k]->rc == kRcPixels;
        c[k].shown = S.scanoutFrame && memcmp(cs[k]->band, S.lastBand, 8) == 0;
        c[k].iosurface = rd3AccelResType(cr[k]) == 0xc0;
    }
    const int pk = rd3_sc_landed_pick(c, m);
    if (pk < 0) return nullptr;
    *srcScan = cs[pk];
    *idx = ci[pk];
    return cr[pk];
}
/* debug.rdna3accel_swap_landed=0: a landed buffer found but not presented -
 * counted, logged (first 8, then every 1024th) with what the swap did instead */
static void rd3AccelLandedWould(unsigned head, unsigned n, IOAccelResource2 *back, int backArg, unsigned backType,
                                bool backEmpty, IOAccelResource2 *l, unsigned idx, enum rd3_sc_act act)
{
    if (head >= kRd3MaxFB) return;
    const SInt64 k = OSAddAtomic64(1, &gCntScLandedWould[head]);
    if (!rd3_log_due((uint64_t)k, 8, 1023)) return;
    char where[24];
    if (idx == RD3_TRV4_LANDED_A1) strlcpy(where, "a1", sizeof where);
    else snprintf(where, sizeof where, "buf[%u]", idx);
    ALOG("swap #%u: head %u: would present landed buffer 0x%llx (%s, resource type 0x%02x, IOSurface %u) instead of "
         "the back (phys 0x%llx, %s, resource type 0x%02x, %s) -- debug.rdna3accel_swap_landed=0, so %s (%lld such)",
         n, head, rd3res_phys(l), where, rd3AccelResType(l), rd3AccelResSurfaceId(l), rd3res_phys(back),
         backArg == 2 ? "a2" : "a1", backType, backEmpty ? "read empty" : "has no IOSurface and did not change",
         act == RD3_SC_HOLD ? "held (the panel keeps the last frame)"
         : act == RD3_SC_COPY_EMPTY ? "the empty back copied (a black frame)" : "the back copied",
         (long long)k + 1);
}
/* what the head's HUBP scans now (Q6): the core's SURFACE_EARLIEST_INUSE
 * and the requested address as CPU addresses, the frame state */
/* PANEL's v3: why the HUBP's address is missing (0: it is there); 4 and 5
 * carry the MC addresses in pa / pb with flag 2 (not comparable with the
 * BAR addresses of the other events, ECC review round 2) */
enum { kPanelOk = 0, kPanelNoCore = 1, kPanelNoCoreHead = 2, kPanelCallFailed = 3, kPanelNoBase = 4,
       kPanelBelowFb = 5, kPanelWhys = 6 };
static unsigned gPanelLogs[kPanelWhys];
/* what the HUBP scanned, as the PANEL event recorded it ('known' only with
 * a BAR address comparable with the swaps' buffers) */
static void rd3AccelTracePanel(unsigned head, IOService *fb, unsigned long long expect, RD3PanelScan *out = nullptr)
{
    if (out) { out->known = false; out->pending = false; out->scan = 0; }
    if (head >= kRd3MaxFB || !fb) return;
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_PANEL, head);
    RD3StatusWhy w = { nullptr, kIOReturnSuccess };
    unsigned coreHead = 0, why = kPanelOk;
    IOReturn r = kIOReturnSuccess;
    IOService *core = rd3AccelCore();
    if (!core) {
        why = kPanelNoCore;
    } else if (!rd3AccelCoreHead(fb, &coreHead, &w)) {
        why = kPanelNoCoreHead;
        r = w.ret;
    } else {
        struct RD3KDisplayScanAddr q;
        bzero(&q, sizeof q);
        q.version = RD3K_DISPLAY_ABI_VERSION;
        q.head = coreHead;
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_SCANADDR);
        r = sym ? core->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
        if (sym) sym->release();
        const unsigned long long base = gVramPhysBase;
        if (r == kIOReturnSuccess && base && q.inUse >= q.fbStart) {
            e.pa = base + (q.inUse - q.fbStart);
            e.pb = q.requested >= q.fbStart ? base + (q.requested - q.fbStart) : 0;
            e.flags = q.flipPending ? 1 : 0;
            if (out) { out->known = true; out->pending = q.flipPending != 0; out->scan = e.pa; }
        } else if (r == kIOReturnSuccess) {
            e.pa = q.inUse;               /* MC addresses: no VRAM base learned yet, or below the FB aperture */
            e.pb = q.requested;
            e.flags = (q.flipPending ? 1 : 0) | 2;
            why = base ? kPanelBelowFb : kPanelNoBase;
        } else {
            why = kPanelCallFailed;
        }
    }
    e.v3 = why;
    e.v4 = r;
    if (why != kPanelOk && why != kPanelNoBase && rd3_log_due(gPanelLogs[why]++, 4, 1023))
        ALOG("trace: head %u: what its HUBP scans is unknown (the PANEL events carry 0 or MC addresses): %s "
             "(0x%x%s%s)", head,
             why == kPanelNoCore ? "no core kext (RDNA3Accel) found"
             : why == kPanelNoCoreHead ? "the core does not know this framebuffer's head"
             : why == kPanelBelowFb ? "the HUBP's address is below the framebuffer aperture (not VRAM the BAR shows)"
             : r == kIOReturnUnsupported ? "the core has no rdna3DisplayScanAddr (an older core kext)"
                                         : "the core's rdna3DisplayScanAddr failed",
             (unsigned)r, w.why ? ": " : "", w.why ? w.why : "");
    RD3KDisplayStatus st;
    if (rd3AccelDispStatus(fb, head, &st, &w)) {
        e.v0 = st.frame;
        e.v1 = st.scanline;
        e.v2 = st.inVblank;
    }
    e.pc = expect;
    rd3Trace(e);
}

/* a legacy swap's head (card runs 15, 17, 58b: the order in the comment
 * above gSurfHead); kRd3MaxFB when none, counted and logged */
static const char *const kHow[6] = { "?", "only head piped", "surface seen before", "argument is the pipe resource",
                                     "argument shares the pipe vidmem", "new surface -> next unclaimed head" };
static unsigned rd3AccelSwapRoute(RDNA3Accelerator *a, const void *surf, unsigned frontHead, RD3VidMemory *v1,
                                  RD3VidMemory *v2, unsigned n, unsigned long long p1, unsigned long long p2, int front,
                                  bool srcIsA2, IOAccelResource2 *const *pipeRes, unsigned *howOut)
{
    const unsigned piped = gRd3HeadsPiped;
    unsigned head = kRd3MaxFB, how = 0;
    if (piped && !(piped & (piped - 1))) {
        for (head = 0; !(piped & (1u << head)); head++) ;
        how = 1;
    }
    unsigned slot = 8;
    for (unsigned i = 0; i < 8; i++)
        if (gSurfHead[i] == surf) slot = i;
    if (how == 0 && slot < 8 && (piped & (1u << gSurfHeadIdx[slot]))) { head = gSurfHeadIdx[slot]; how = 2; }
    if (how == 0 && frontHead < kRd3MaxFB) { head = frontHead; how = 3; }
    if (how == 0 && gHeadFbResLock) {
        IOSimpleLockLock(gHeadFbResLock);
        for (unsigned h = 0; how == 0 && h < kRd3MaxFB; h++) {
            RD3VidMemory *hv = gHeadFbRes[h] ? rd3res_vidmem(gHeadFbRes[h]) : nullptr;   /* alive: still in the table */
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
    *howOut = how;
    if (how == 0 || head >= kRd3MaxFB || !a->fFBs[head]) {
        const SInt64 k = OSAddAtomic64(1, &gCntSwapNoHead);
        if (k < 8 || (k & 1023) == 0)
            ALOG("swap: surface %p (a1 phys 0x%llx a2 phys 0x%llx) matched no head (piped 0x%x, pipe resources %p %p %p %p) "
                 "-- not shown (%lld so far)", surf, p1, p2, piped, pipeRes[0], pipeRes[1], pipeRes[2], pipeRes[3],
                 (long long)k + 1);
        return kRd3MaxFB;
    }
    if (slot == 8) {   /* remember the surface -> head answer */
        for (unsigned i = 0; i < 8; i++)
            if (!gSurfHead[i]) { gSurfHead[i] = surf; gSurfHeadIdx[i] = head; break; }
    } else if (gSurfHeadIdx[slot] != head) {
        RD3ROUTELOG("swap #%u: surface %p moves from head %u to head %u", n, surf, gSurfHeadIdx[slot], head);
        gSurfHeadIdx[slot] = head;
    }
    if (gSwapHow[head] != how) {
        gSwapHow[head] = how;
        ALOG("swap: surface %p -> head %u (%s; a1 phys 0x%llx a2 phys 0x%llx, front %s, src %s)", surf, head, kHow[how],
             p1, p2, front == 1 ? "a1" : front == 2 ? "a2" : "not named", srcIsA2 ? "a2" : "a1");
    }
    return head;
}
/* the head a surface's swaps went to before (no routing, no log), kRd3MaxFB */
static unsigned rd3AccelSurfKnownHead(const void *surf)
{
    for (unsigned i = 0; i < 8; i++)
        if (gSurfHead[i] == surf && gSurfHeadIdx[i] < kRd3MaxFB) return gSurfHeadIdx[i];
    return kRd3MaxFB;
}
/* the bookkeeping of a swap whose buffer 'src' (phys) goes to the head:
 * buffer changes, new buffers, the flicker / A3 / summary lines; the band
 * hash of the flicker and torn tests in 'hash' (nullptr: *hashSkip says why) */
static void rd3AccelSwapNote(unsigned head, const void *surf, IOAccelResource2 *src, unsigned long long phys,
                             unsigned long long p1, unsigned long long p2, int front, unsigned pick, unsigned how,
                             unsigned n, unsigned frontHead, unsigned long long frontPhys, uint32_t *hash,
                             const char **hashSkip)
{
    if (phys != gHeadSrcPhys[head]) {
        if (gHeadSrcPhys[head]) OSAddAtomic64(1, &gCntSwapBufChg[head]);
        for (unsigned h = 0; h < kRd3MaxFB; h++)
            if (h != head && phys && gHeadSrcPhys[h] == phys)
                RD3ROUTELOG("swap #%u: head %u now gets head %u's frame (phys 0x%llx, surface %p, %s)",
                            n, head, h, phys, surf, kHow[how]);
        gHeadSrcPhys[head] = phys;
    }
    rd3AccelSwapNewBuffer(head, n, phys, p1, p2, front, pick, kHow[how]);
    *hashSkip = "not hashed";
    if (rd3AccelFlickerWants(head) || rd3AccelTornWants(head)) *hashSkip = rd3AccelBandHash(src, hash);
    rd3AccelA3SwapNote(head, src, phys, how, *hashSkip ? nullptr : hash, *hashSkip);
    rd3AccelSwapSummary(head, gCntSwapHead[head], frontHead == head ? frontPhys : 0);
}

void rd3AccelSwapToPanel(const void *surf, IOAccelResource2 *a1, IOAccelResource2 *a2, IOAccelResource2 *const *bufs,
                         unsigned nbufs)
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a) return;
    /* once per swap (the mode changes under the family's lock, between two
     * swaps); modes 1/2 flip while the SDMA breaker is open */
    int mode = gSwapMode;
    rd3AccelScApplyAll();   /* the panel moves posted since the last holder of the lock */
    rd3AccelCopyForwardCheck(a1);
    rd3AccelCopyForwardCheck(a2);
    /* the head framebuffer resources (the fronts): which argument is one */
    IOAccelResource2 *pipeRes[kRd3MaxFB] = {};
    RD3VidMemory *v1 = rd3res_vidmem(a1), *v2 = rd3res_vidmem(a2);
    /* N41 security review: whose GPU marks are taken - only a swap that names our VRAM (round 3), before the
     * head table's spinlock (proc_name sleeps) */
    if (v1 || v2) rd3AccelSurfNoteDisplayClient("legacy swap");
    unsigned frontHead = kRd3MaxFB;
    int front = 0;
    unsigned long long frontPhys = 0;
    if (gHeadFbResLock) {
        IOSimpleLockLock(gHeadFbResLock);
        for (unsigned h = 0; h < kRd3MaxFB; h++) {
            pipeRes[h] = gHeadFbRes[h];
            if (pipeRes[h] && (pipeRes[h] == a1 || pipeRes[h] == a2) && frontHead == kRd3MaxFB) {
                frontHead = h;
                front = pipeRes[h] == a1 ? 1 : 2;
                frontPhys = rd3res_phys(pipeRes[h]);   /* alive: still in the table */
            }
        }
        IOSimpleLockUnlock(gHeadFbResLock);
    }
    /* both arguments' 64 samples: content and the trace's band bytes */
    RD3Scan s1, s2;
    rd3AccelResScan(a1, &s1);
    rd3AccelResScan(a2, &s2);
    const unsigned rc1 = s1.rc, rc2 = s2.rc;
    const unsigned long long p1 = rd3res_phys(a1), p2 = rd3res_phys(a2);
    const unsigned n = ++gSwapCount;
    /* the join with the plugin's trace (Q10, ECC review round 2): each new
     * (resource, IOSurface id, address) of a1 / a2 once, an id of 0 too */
    if (a1) rd3AccelTraceSurfRes(a1, rd3AccelResSurfaceId(a1), 1, 3, p1, v1 ? v1->fBytes : 0, true);
    if (a2) rd3AccelTraceSurfRes(a2, rd3AccelResSurfaceId(a2), 2, 3, p2, v2 ? v2->fBytes : 0, true);
    const unsigned knownHead = frontHead < kRd3MaxFB ? frontHead : rd3AccelSurfKnownHead(surf);
    /* another surface takes the head over after the last one went quiet: its buffers' writes were never compared
     * with this head's frame - the write generations start over (the first swap presents); surfaces taking turns
     * keep the head's record (ECC review round 1) */
    if (knownHead < kRd3MaxFB) rd3AccelWgSurfSwap(knownHead, surf);
    bool brkFlip = false;
    if (mode != RD3_SWAPMODE_FLIP) {
        enum rd3_sc_brk_ev bev;
        if (!rd3_sc_brk_allow(&gScBrk, rd3AccelUsNow(), &bev)) {
            mode = RD3_SWAPMODE_FLIP;   /* the SDMA breaker is open (rd3_sc_brk) */
            brkFlip = true;
            if (knownHead < kRd3MaxFB) OSAddAtomic64(1, &gCntScBrkFlip[knownHead]);
        }
        rd3AccelScBrkEvent(bev, n, knownHead, 0, kIOReturnSuccess);
    }
    /* the trace's record of this swap (Q1), filled as it goes */
    struct rd3_trace_ev tr = rd3TrEv(RD3_TR_SWAP, knownHead);
    tr.obj = rd3TrObj(surf);
    tr.v0 = n;
    tr.v1 = (rc1 & 0xfu) | ((rc2 & 0xfu) << 4) | ((unsigned)mode << 12) | (brkFlip ? 1u << 20 : 0u);
    tr.v4 = rd3AccelBufIndex(bufs, nbufs, a1) | (rd3AccelBufIndex(bufs, nbufs, a2) << 8);
    tr.pa = p1;
    tr.pb = p2;
    tr.flags = (uint8_t)((front == 1 ? RD3_TRF_A1_FRONT : 0) | (front == 2 ? RD3_TRF_A2_FRONT : 0) |
                         (rc1 == kRcPixels ? RD3_TRF_A1_PIXELS : 0) | (rc2 == kRcPixels ? RD3_TRF_A2_PIXELS : 0));
    memcpy(tr.ba, s1.band, 8);
    memcpy(tr.bb, s2.band, 8);
    uint8_t prev[8];
    const int ch1 = knownHead < kRd3MaxFB ? rd3AccelSigSwap(knownHead, p1, s1.band, prev) : -1;
    const int ch2 = knownHead < kRd3MaxFB ? rd3AccelSigSwap(knownHead, p2, s2.band, prev) : -1;

    if (mode != RD3_SWAPMODE_FLIP) {
        /* ---- modes 1 / 2: the back is copied ---- */
        const struct rd3_sc_roles roles = rd3_sc_roles(front == 1, front == 2);
        IOAccelResource2 *back = roles.back == 2 ? a2 : a1, *frontRes = roles.front == 1 ? a1 : a2;
        const RD3Scan &sb = roles.back == 2 ? s2 : s1, &sf = roles.front == 1 ? s1 : s2;
        const int chb = roles.back == 2 ? ch2 : ch1;
        /* the write generations (card run 2026-10-17d, rdna3_write_gen.h): a back WindowServer did not write
         * (page-on, copy forward) after the head's frame is held - no copy into D / home, no front copy, the
         * panel keeps its frame (the stale buf[2] a copy forward burst left); mode 2, the copy-everything
         * control, and debug.rdna3accel_swap_gen=0 only count it. Before a hold, the surface's buffer written
         * after the head's frame, if one has pixels, is presented instead (ECC review round 1: WindowServer's
         * frame in a buffer the swap did not name) */
        bool wgJudged = false;
        unsigned long long wgGen = 0, wgLandGen = 0;
        IOAccelResource2 *wgLand = nullptr;
        unsigned wgLandIdx = 0xff;
        RD3Scan wgLandScan;
        if (knownHead < kRd3MaxFB) {
            unsigned long long wgHead = 0;
            const unsigned long long bp = rd3res_phys(back);
            const enum rd3_wg_why wg = rd3AccelWgDecide(knownHead, bp, sb.rc >= kRcEmpty, sb.sig, &wgGen, &wgHead);
            const bool apply = gSwapGen && mode == RD3_SWAPMODE_COPY;
            wgJudged = wg != RD3_WGW_OFF;
            if (!wgJudged) rd3AccelWgNotJudged(knownHead, n, "the back has no VRAM address");
            if (apply && rd3_wg_holds(wg)) {
                unsigned li = 0;
                unsigned long long lg = 0;
                IOAccelResource2 *l = rd3AccelWgLanded(knownHead, back, pipeRes[knownHead], bufs, nbufs, wgHead,
                                                       &li, &lg);
                if (l) {
                    rd3AccelResScan(l, &wgLandScan);
                    if (wgLandScan.rc == kRcPixels) {
                        wgLand = l;
                        wgLandIdx = li;
                        wgLandGen = lg;
                    }
                }
            }
            if (rd3AccelWgNote(knownHead, wg, apply, n, bp, wgGen, wgHead,
                               roles.back == 2 ? "the back (a2)" : "the back (a1)", wgLand ? rd3res_phys(wgLand) : 0,
                               wgLandGen, &tr)) {
                tr.pc = rd3AccelScExpect(knownHead);
                if (sb.rc == kRcEmpty) tr.flags |= RD3_TRF_BACK_EMPTY;
                rd3Trace(tr);
                rd3AccelScanOthers(knownHead, surf, a1, a2, s1, s2, ch1, ch2, bufs, nbufs, n, false,
                                   "the back was not written since the head's frame (held)");
                rd3AccelTracePanel(knownHead, a->fFBs[knownHead], tr.pc);
                return;
            }
        }
        const bool picture = sf.rc == kRcPixels || (knownHead < kRd3MaxFB && gSc[knownHead].scanoutFrame &&
                                                    gSc[knownHead].lastPixels);
        enum rd3_sc_act act = rd3_sc_action(mode, sb.rc >= kRcEmpty, sb.rc == kRcPixels, picture);
        /* the landed buffer (card run 2026-10-17c): a suspect back - empty,
         * or without an IOSurface and unchanged - and another buffer of the
         * surface holding a new frame: that one goes to the panel */
        RD3Others oth;
        oth.n = 0;
        bool othDone = false;
        IOAccelResource2 *src = back;
        const RD3Scan *ss = &sb;
        unsigned landedIdx = 0xff;
        const unsigned backType = rd3AccelResType(back);
        if (wgLand) {
            /* the write generations' landed buffer: the older content rule's search is not needed */
            src = wgLand;
            ss = &wgLandScan;
            act = RD3_SC_COPY_LANDED;
            landedIdx = wgLandIdx;
            tr.v1 |= RD3_TRV1_LANDED;
            tr.v4 |= (wgLandIdx & 0xffu) << 24;
        } else if (knownHead < kRd3MaxFB &&
            rd3_sc_landed_wanted(mode, sb.rc >= kRcEmpty, sb.rc == kRcPixels, chb != 0, backType == 0xc0)) {
            othDone = true;
            IOAccelResource2 *l = rd3AccelLanded(knownHead, surf, a1, a2, back, s1, ch1, pipeRes[knownHead], bufs,
                                                 nbufs, n, &oth, &ss, &landedIdx);
            if (l && gSwapLanded) {
                src = l;
                act = RD3_SC_COPY_LANDED;
                tr.v1 |= RD3_TRV1_LANDED;
                tr.v4 |= (landedIdx & 0xffu) << 24;
            } else if (l) {   /* off: the back's old rule stands, what it would have presented is recorded */
                tr.v1 |= RD3_TRV1_LANDED_WOULD;
                tr.v4 |= (landedIdx & 0xffu) << 24;
                rd3AccelLandedWould(knownHead, n, back, roles.back, backType, sb.rc == kRcEmpty, l, landedIdx, act);
            }
        }
        /* the stale back (card run 2026-10-17c): a mode 1 back with pixels
         * that WindowServer did not redraw since this head presented it (its
         * samples unchanged, no copy forward into it) while a newer frame was
         * presented after it - copied, it would put the older frame on the
         * panel and into the front. The second grid confirms (a redraw
         * between the first grid's points is copied); debug.rdna3accel_
         * swap_stale=0 only counts what it would have held */
        unsigned staleAt = 0;
        unsigned long long staleAfter = 0;
        bool staleNoHead = false;
        if (act == RD3_SC_COPY && mode == RD3_SWAPMODE_COPY && sb.rc == kRcPixels) {
            if (knownHead >= kRd3MaxFB) {
                staleNoHead = true;   /* counted under the head the swap is routed to */
            } else {
                const RD3ScHead &K = gSc[knownHead];
                const int panelFrame = K.scanoutFrame && (K.shows == RD3_SCS_D || K.shows == RD3_SCS_HOME);
                const unsigned long long bp = rd3res_phys(back);
                int nocf = 0;
                const enum rd3_sc_stale_why why =
                    rd3AccelScStaleCheck(knownHead, panelFrame, bp, sb.sig, &staleAt, &nocf, &staleAfter);
                rd3AccelScStaleWhy(knownHead, why, nocf, n, bp, staleAt);
                if (why == RD3_SCST_HOLD) {
                    unsigned long long sig2 = 0;
                    const bool ok2 = rd3AccelResSig2(back, &sig2);
                    if (!ok2 || !rd3AccelScStaleConfirm(knownHead, bp, sig2)) {
                        const SInt64 k = OSAddAtomic64(1, &gCntScStaleMiss[knownHead]);
                        if (rd3_log_due((uint64_t)k, 8, 1023))
                            ALOG("swap #%u: head %u: the back 0x%llx matched its first-grid samples of swap #%u but "
                                 "%s -- copied, not held (a redraw between the first grid's points; %lld such)", n,
                                 knownHead, bp, staleAt,
                                 ok2 ? "not its second grid (shifted by half a cell), or its record went"
                                     : "its second grid could not be read",
                                 (long long)k + 1);
                    } else if (gSwapStale) {
                        act = RD3_SC_HOLD_STALE;
                    } else {
                        tr.v1 |= RD3_TRV1_STALE_WOULD;
                        const SInt64 k = OSAddAtomic64(1, &gCntScStaleWould[knownHead]);
                        if (rd3_log_due((uint64_t)k, 8, 1023))
                            ALOG("swap #%u: head %u: would hold the stale back 0x%llx (unchanged since swap #%u, "
                                 "0x%llx shown after it, no copy forward into it) -- debug.rdna3accel_swap_stale=0, "
                                 "so copied (%lld such)", n, knownHead, bp, staleAt, staleAfter, (long long)k + 1);
                    }
                }
            }
        }
        if (act == RD3_SC_HOLD_STALE) {
            tr.arg = RD3_TRS_HOLD;
            tr.v1 |= RD3_TRV1_STALE;
            tr.v3 = staleAt;
            tr.pc = rd3AccelScExpect(knownHead);
            const SInt64 k = OSAddAtomic64(1, &gCntScStale[knownHead]);
            if (rd3_log_due((uint64_t)k, 8, 1023)) {
                const unsigned bi = rd3AccelBufIndex(bufs, nbufs, back);
                char where[32];
                if (bi == 0xff) strlcpy(where, "not in the surface's array", sizeof where);
                else snprintf(where, sizeof where, "buf[%u]", bi);
                ALOG("swap #%u: head %u: the back 0x%llx (%s, %s) is unchanged since it was shown at swap #%u and "
                     "0x%llx was shown after it, with no copy forward into it -- held, the panel keeps the newer "
                     "frame (%lld such)", n, knownHead, rd3res_phys(back), roles.back == 2 ? "a2" : "a1", where,
                     staleAt, staleAfter, (long long)k + 1);
            }
            rd3Trace(tr);
            rd3AccelScanOthers(knownHead, surf, a1, a2, s1, s2, ch1, ch2, bufs, nbufs, n, false,
                               "the back is stale (held)", othDone ? &oth : nullptr);
            rd3AccelTracePanel(knownHead, a->fFBs[knownHead], tr.pc);
            rd3AccelStaleResample(knownHead, back, sb.sig, surf, n, staleAt);
            return;
        }
        if (sb.rc == kRcEmpty && act != RD3_SC_COPY_LANDED) tr.flags |= RD3_TRF_BACK_EMPTY;
        if (act == RD3_SC_HOLD) {
            /* an empty back over a picture: the panel and the front keep the
             * last frame (card run 2026-10-17b: head 0's second back never
             * received a frame) */
            tr.arg = RD3_TRS_HOLD;
            if (knownHead < kRd3MaxFB) {
                const SInt64 k = OSAddAtomic64(1, &gCntScEmpty[knownHead]);
                tr.pc = rd3AccelScExpect(knownHead);
                if (rd3_log_due((uint64_t)k, 8, 1023))
                    ALOG("swap #%u: head %u: the back (phys 0x%llx, %s) read empty over a picture -- not copied, the "
                         "panel keeps the last frame (%lld such; swap mode 1; debug.rdna3accel_swap_mode=2 copies it)",
                         n, knownHead, rd3res_phys(back), roles.back == 2 ? "a2" : "a1", (long long)k + 1);
            }
            rd3Trace(tr);
            rd3AccelScanOthers(knownHead, surf, a1, a2, s1, s2, ch1, ch2, bufs, nbufs, n, true,
                               "the back read empty (held, mode 1; no other buffer had a new frame)",
                               othDone ? &oth : nullptr);
            if (knownHead < kRd3MaxFB) {
                rd3AccelTracePanel(knownHead, a->fFBs[knownHead], tr.pc);
                rd3AccelEmptyResample(knownHead, back, true, 1, surf, n);
            }
            return;
        }
        unsigned how = 0;
        const unsigned head = rd3AccelSwapRoute(a, surf, frontHead, v1, v2, n, p1, p2, front, src == a2, pipeRes, &how);
        tr.head = (uint8_t)(head < kRd3MaxFB ? head : 0xffu);
        tr.v1 |= how << 8;
        if (head >= kRd3MaxFB) {
            tr.arg = RD3_TRS_NOHEAD;
            rd3Trace(tr);
            return;
        }
        if (staleNoHead) OSAddAtomic64(1, &gCntScStaleNoHead[head]);   /* no head known at the check: copied */
        if (knownHead >= kRd3MaxFB) rd3AccelWgNotJudged(head, n, "no head known at the swap's entry (mode 1)");
        if (act == RD3_SC_COPY_LANDED && !wgLand) {   /* the write generations' landed one is counted and logged */
            const SInt64 k = OSAddAtomic64(1, &gCntScLanded[head]);
            if (rd3_log_due((uint64_t)k, 8, 1023)) {
                char where[24];
                if (landedIdx == RD3_TRV4_LANDED_A1) strlcpy(where, "a1", sizeof where);
                else snprintf(where, sizeof where, "buf[%u]", landedIdx);
                ALOG("swap #%u: head %u: the back (phys 0x%llx, %s, resource type 0x%02x) %s -- presented the landed "
                     "buffer 0x%llx instead (%s, resource type 0x%02x, IOSurface %u: new pixels since this head's "
                     "last look; %lld such; swap mode %d)", n, head, rd3res_phys(back), roles.back == 2 ? "a2" : "a1",
                     backType, sb.rc == kRcEmpty ? "read empty" : "has no IOSurface and did not change",
                     rd3res_phys(src), where, rd3AccelResType(src), rd3AccelResSurfaceId(src), (long long)k + 1, mode);
            }
        }
        if (act == RD3_SC_COPY_EMPTY) {
            const SInt64 k = OSAddAtomic64(1, &gCntScBlack[head]);
            if (rd3_log_due((uint64_t)k, 8, 1023))
                ALOG("swap #%u: head %u: the back (phys 0x%llx) read empty -- copied anyway, a black frame (%lld such; "
                     "swap mode %d%s)", n, head, rd3res_phys(back), (long long)k + 1, mode,
                     mode == RD3_SWAPMODE_COPY ? ", nothing on the panel to keep" : "");
        }
        uint32_t hash[kFlickBands];
        const char *hashSkip = nullptr;
        rd3AccelSwapNote(head, surf, src, rd3res_phys(src), p1, p2, front, (unsigned)roles.back, how, n, frontHead,
                         frontPhys, hash, &hashSkip);
        IOService *fb = a->fFBs[head];
        rd3AccelTracePanel(head, fb, rd3AccelScExpect(head));
        rd3AccelSwapCopy(head, fb, src, frontRes, ss->rc == kRcEmpty, n, &tr, ss->band);
        if (tr.arg == RD3_TRS_COPY) {
            /* on the panel: what it looked like (the stale back's record),
             * under the head the checks read: the one known at the swap's
             * entry, else the routed one */
            unsigned long long sig2 = 0;
            (void)rd3AccelResSig2(src, &sig2);   /* unreadable: 0, a later check then copies */
            rd3AccelScStaleShown(knownHead < kRd3MaxFB ? knownHead : head, rd3res_phys(src), ss->sig, sig2, n);
            /* the head's frame now (the write generations), with the generation captured at the decision: a
             * write between the decision and the copy is not this frame's (ECC review round 1) */
            const unsigned long long sp = rd3res_phys(src);
            const unsigned long long pg = src == wgLand ? wgLandGen
                                        : wgJudged && src == back ? wgGen : rd3AccelWgGen(sp);
            rd3AccelWgPresented(knownHead < kRd3MaxFB ? knownHead : head, sp, pg, ss->rc >= kRcEmpty, ss->sig);
        }
        rd3Trace(tr);
        rd3AccelScanOthers(head, surf, a1, a2, s1, s2, ch1, ch2, bufs, nbufs, n, sb.rc == kRcEmpty,
                           act == RD3_SC_COPY_LANDED ? "the back was suspect (the landed buffer presented)"
                                                     : "the back read empty (copied)",
                           othDone ? &oth : nullptr);
        gHeadSurf[head] = surf;
        return;
    }

    /* ---- mode 0: the panel flips to WindowServer's back (card run 2026-10-17) ---- */
    /* the head's swaps skipped in a row (a black or clear frame shows on the third) */
    const unsigned run = frontHead < kRd3MaxFB ? gSwapEmptyRun[frontHead] : 0;
    const bool backHasGeometry = (front == 1 ? rc2 : rc1) >= kRcEmpty;   /* a mapped buffer, no pixels or pixels */
    const enum rd3_swap_pick pick = rd3_swap_source(front == 1, front == 2, rc1 == kRcPixels, rc2 == kRcPixels,
                                                    backHasGeometry ? run : 0);
    if (pick == RD3_SWAP_SKIP) {
        if (frontHead < kRd3MaxFB) gSwapEmptyRun[frontHead] = run + 1;
        rd3AccelSwapSkipped(frontHead, front, rc1, rc2, run + 1, p1, p2, backHasGeometry);
        tr.arg = RD3_TRS_SKIP;
        if (front && (front == 1 ? rc2 : rc1) == kRcEmpty) tr.flags |= RD3_TRF_BACK_EMPTY;
        tr.pc = knownHead < kRd3MaxFB ? gLatch[knownHead].phys : 0;
        rd3Trace(tr);
        rd3AccelScanOthers(knownHead, surf, a1, a2, s1, s2, ch1, ch2, bufs, nbufs, n, true,
                           "the back read empty (skipped, mode 0)");
        if (knownHead < kRd3MaxFB) rd3AccelTracePanel(knownHead, a->fFBs[knownHead], tr.pc);
        /* did the empty back's frame land late? (card run 2026-10-17: 320 of
         * head 0's 1030 swaps skipped) */
        if (front)
            rd3AccelEmptyResample(frontHead, front == 1 ? a2 : a1, (front == 1 ? rc2 : rc1) == kRcEmpty, run + 1,
                                  surf, n);
        return;
    }
    IOAccelResource2 *src = pick == RD3_SWAP_A2 ? a2 : a1;
    const bool emptyShown = (pick == RD3_SWAP_A2 ? rc2 : rc1) != kRcPixels;
    const RD3Scan &ssrc = pick == RD3_SWAP_A2 ? s2 : s1;
    /* the write generations (card run 2026-10-17d, rdna3_write_gen.h): mode 0 flips only to a back WindowServer
     * wrote after the head's frame - never back to the stale buf[2] a copy forward burst left (17d: h0 flipped
     * to 0x89040000 about once a second from 309 s on); debug.rdna3accel_swap_gen=0 only counts it */
    bool wgJudged = false;
    unsigned long long wgGen = 0;
    if (knownHead < kRd3MaxFB) {
        unsigned long long wgHead = 0;
        const unsigned long long sp = rd3res_phys(src);
        const enum rd3_wg_why wg = rd3AccelWgDecide(knownHead, sp, ssrc.rc >= kRcEmpty, ssrc.sig, &wgGen, &wgHead);
        wgJudged = wg != RD3_WGW_OFF;
        if (!wgJudged) rd3AccelWgNotJudged(knownHead, n, "the back has no VRAM address (mode 0)");
        if (rd3AccelWgNote(knownHead, wg, gSwapGen != 0, n, sp, wgGen, wgHead,
                           pick == RD3_SWAP_A2 ? "the back (a2)" : "the back (a1)", 0, 0, &tr)) {
            tr.pc = gLatch[knownHead].phys;
            if (emptyShown) tr.flags |= RD3_TRF_BACK_EMPTY;
            rd3Trace(tr);
            rd3AccelScanOthers(knownHead, surf, a1, a2, s1, s2, ch1, ch2, bufs, nbufs, n, false,
                               "the back was not written since the head's frame (held, mode 0)");
            rd3AccelTracePanel(knownHead, a->fFBs[knownHead], tr.pc);
            return;
        }
    }
    if (frontHead < kRd3MaxFB) gSwapEmptyRun[frontHead] = 0;
    unsigned how = 0;
    const unsigned head = rd3AccelSwapRoute(a, surf, frontHead, v1, v2, n, p1, p2, front, src == a2, pipeRes, &how);
    tr.head = (uint8_t)(head < kRd3MaxFB ? head : 0xffu);
    tr.v1 |= how << 8;
    if (head >= kRd3MaxFB) {
        tr.arg = RD3_TRS_NOHEAD;
        rd3Trace(tr);
        return;
    }
    if (knownHead >= kRd3MaxFB) rd3AccelWgNotJudged(head, n, "no head known at the swap's entry (mode 0)");
    if (emptyShown) {
        tr.flags |= RD3_TRF_BACK_EMPTY;
        const SInt64 k = OSAddAtomic64(1, &gCntSwapEmptyShown[head]);
        if (k < 8 || (k & 1023) == 0)
            ALOG("swap #%u: head %u: the back (phys 0x%llx) read empty %u swaps in a row -- shown as a black or clear "
                 "frame (%lld so far)", n, head, rd3res_phys(src), RD3_SWAP_EMPTY_RUN, (long long)k + 1);
    }
    /* the frame each head shows: one head taking a buffer another head
     * shows now is what made the panels swap pictures (card run 2026-10-1x) */
    const unsigned long long phys = rd3res_phys(src);
    uint32_t hash[kFlickBands];
    const char *hashSkip = nullptr;
    rd3AccelSwapNote(head, surf, src, phys, p1, p2, front, (unsigned)pick, how, n, frontHead, frontPhys, hash,
                     &hashSkip);
    IOService *fb = a->fFBs[head];
    RD3PanelScan ps;
    rd3AccelTracePanel(head, fb, gLatch[head].phys, &ps);
    /* the limit of the wait after the flip, from the head's refresh */
    const uint64_t cap = rd3_latch_cap_us(rd3AccelHeadFrameUs(fb, head));
    /* DRIVER-PLAN N38: the present queue - programmed or queued, the swap returns at once */
    const bool async = gSwapAsync && gPresReady;
    /* the previous flip latched? (the queue's worker sees it when async) the buffer it showed, changed on the panel? */
    if (!async) rd3AccelLatchEntry(head, fb, n);
    rd3AccelTornCheck(head, src, hashSkip ? nullptr : hash, bufs, nbufs, n);
    /* the back already on the panel, its flip latched: not flipped again, no wait (card run 2026-10-18b).
     * With the present queue that is decided inside it, after its latch look and only with nothing in flight
     * or queued (rd3_fq_present; ECC review of N38: decided here first, an older queued present was
     * programmed over the newer frame at the next latch) */
    enum rd3_same_shown same = async ? RD3_SAME_NO
                                     : rd3_latch_same_shown(gSwapSameNoFlip && fb, gLatch[head].phys,
                                                            gLatch[head].st.latched, ps.known, ps.scan, ps.pending,
                                                            phys);
    RD3FlipWhy fw = { nullptr, kIOReturnSuccess, 0 };
    const uint64_t issued = mach_absolute_time();
    unsigned pq = kPqOutRefused;
    bool flipped = false;
    if (async) {
        pq = rd3AccelPresentLegacy(head, fb, src, kPqSwap, n, hashSkip ? nullptr : hash, cap, &fw,
                                   gSwapSameNoFlip && fb);
        if (pq == kPqOutSame) {
            same = RD3_SAME_SKIP;
            ps.known = true;   /* the queue read the HUBP's address */
        } else {
            if (hashSkip && rd3AccelTornWants(head)) rd3AccelTornSkip(head, phys, hashSkip, n);
            flipped = pq != kPqOutRefused;
        }
    } else if (same != RD3_SAME_SKIP) {
        flipped = rd3AccelFlipTo(src, fb, head, &fw);
        struct rd3_trace_ev fe = rd3TrEv(RD3_TR_FLIP, head);
        fe.arg = 0;
        fe.flags = flipped ? 1 : 0;
        fe.v0 = (uint32_t)fw.flipResult;
        fe.v4 = fw.ret;
        fe.pa = phys;
        rd3Trace(fe);
    }
    if (same == RD3_SAME_SKIP) {
        rd3AccelScPanelMoved(head, "a mode 0 swap to the buffer on the panel", issued, false);
        rd3AccelLatchSame(head, src, phys, hashSkip ? nullptr : hash, hashSkip, ps.known, n);
        tr.arg = RD3_TRS_FLIP;
        tr.pc = phys;
        tr.v3 = 0;
        tr.v1 |= RD3_TRV1_TARGET_SAME << 16;
    } else if (flipped && async) {
        /* programmed (the queue recorded it for the latch tests) or queued behind the flip in flight: the
         * panel gets it at a vblank, the swap does not wait (the reuse gate holds the writers back) */
        tr.arg = RD3_TRS_FLIP;
        tr.pc = phys;
        tr.v3 = 0;
        if (pq != kPqOutShown) tr.v1 |= RD3_TRV1_TARGET_QUEUED << 16;
    } else if (flipped) {
        rd3AccelScPanelMoved(head, "a mode 0 flip", issued, false);   /* the panel shows WindowServer's buffer, not D */
        /* returns once the flip latched (debug.rdna3accel_swap_latch) */
        rd3AccelLatchFlipped(head, fb, src, phys, hashSkip ? nullptr : hash, hashSkip, cap, n);
        tr.arg = RD3_TRS_FLIP;
        tr.pc = phys;
        tr.v3 = gLatch[head].latchUs;
        if (gLatch[head].latchUs) tr.flags |= RD3_TRF_WAITED;
    } else {
        const bool copied = rd3AccelCopyToScanout(src, fb, head);
        const SInt64 k = OSAddAtomic64(1, &gCntFlipFailed[head]);
        if (rd3_log_due((uint64_t)k, 8, 1023))
            ALOG("swap #%u: head %u: the flip to phys 0x%llx was refused: %s (0x%x, flipResult %d) -- %s (%lld such)", n,
                 head, phys, fw.why ? fw.why : "?", (unsigned)fw.ret, fw.flipResult,
                 copied ? "copied onto the framebuffer's scanout instead"
                        : "neither flipped nor copied, the panel keeps the last picture", (long long)k + 1);
        rd3AccelLatchForgetPres(head, "a flip refused");   /* the framebuffer's own scanout, not a recorded buffer */
        tr.arg = copied ? RD3_TRS_FLIP_COPIED : RD3_TRS_FLIP_FAILED;
        /* a copy into home is no flip: the panel stays where it was (ECC review round 2) */
    }
    /* the head's frame now (the write generations): flipped to, or copied onto its scanout */
    if (tr.arg == RD3_TRS_FLIP || tr.arg == RD3_TRS_FLIP_COPIED)
        rd3AccelWgPresented(knownHead < kRd3MaxFB ? knownHead : head, phys, wgJudged ? wgGen : rd3AccelWgGen(phys),
                            ssrc.rc >= kRcEmpty, ssrc.sig);
    rd3Trace(tr);
    rd3AccelScanOthers(head, surf, a1, a2, s1, s2, ch1, ch2, bufs, nbufs, n, emptyShown, "the back read empty (shown)");
    rd3AccelLatchSummary(head, gCntSwapHead[head]);
    /* the record copy forward reads: WindowServer's last frame on this head
     * is this buffer whether or not the panel got it (copy forward keeps
     * WindowServer's frames continuous, not the panel's) */
    rd3AccelNotePresented(rd3res_vidmem(src), head);
    gHeadSurf[head] = surf;
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
static unsigned gCfChecks, gCfKept, gCfReverted, gCfOther;
/* the buffer each head's last swap presented: WindowServer's last frame on
 * that head. A legacy swap is presented by flipping its back buffer itself
 * (zero-copy), never by the swap copy into the front buffer, so the front
 * never holds a frame: a copy forward from it would copy black over the
 * back (card run 56: "copied 0x00000000" on every check). The last frame
 * is in this buffer (rdna3_swap_front.h): with one back that is the back
 * itself, with two taking turns the other one (card run 58b). Copy forward
 * copies from it only when it is one of the calling surface's own buffers,
 * read through that live resource (ECC review of 60526e4: a record of
 * another surface's buffer must not become a source); here the pointer and
 * the allocation it had are only compared. A scanout vidmem is never
 * recorded (its cookie is the mode's, a mode switch frees it). */
static struct { const void *volatile vm; volatile unsigned long long cookie, seq; } gCfShown[kRd3MaxFB];
static volatile UInt64 gCfShownSeq;   /* each record's number: which presentation set it */
/* the rects each buffer of a head missed (rdna3_swap_front.h): copied into
 * it with the next copy forward that can; every access under gCfPendLock */
static struct rd3_cf_heal gCfPend[kRd3MaxFB];
/* returns the record's number (0: nothing recorded) */
static unsigned long long rd3AccelNotePresented(const void *vm, unsigned head)
{
    const RD3VidMemory *m = (const RD3VidMemory *)vm;
    if (head >= kRd3MaxFB) return 0;
    if (!m || m->fScanout) { rd3AccelForgetShown(head); return 0; }
    const unsigned long long seq = OSIncrementAtomic64((volatile SInt64 *)&gCfShownSeq) + 1;
    gCfShown[head].vm = nullptr;   /* never a pointer with another buffer's cookie */
    __asm__ volatile("" ::: "memory");
    gCfShown[head].cookie = m->fAllocationCookie;
    gCfShown[head].seq = seq;
    __asm__ volatile("" ::: "memory");
    gCfShown[head].vm = vm;
    return seq;
}
/* the head shows nothing recorded any more; the buffers' pending rects
 * stay (they are still stale) */
static void rd3AccelForgetShown(unsigned head)
{
    if (head >= kRd3MaxFB) return;
    gCfShown[head].vm = nullptr;
}
/* the same, only while the record is still the one that presentation set
 * (an async flip refused or dropped must not wipe a newer swap's record,
 * even one of the same buffer) */
static void rd3AccelForgetShownIf(unsigned head, unsigned long long seq)
{
    if (head >= kRd3MaxFB || !seq) return;
    if (gCfShown[head].vm && gCfShown[head].seq == seq) gCfShown[head].vm = nullptr;
}
/* a freed vidmem leaves the tables: its address may come back for a new,
 * never-shown buffer */
void rd3AccelVidmemGone(const void *vm)
{
    rd3AccelVidmemGoneAt(vm, vm ? ((const RD3VidMemory *)vm)->fPhys : 0, true);   /* still set: deallocPhysical calls first */
}
/* the same for the allocation at 'phys' of 'vm' (N41: a parked allocation is released while the
 * vidmem holds no address, or another one; a parked one stayed in every table: still allocated).
 * vmToo false: only what is keyed by that address (and a queued / in-flight present of exactly that
 * address) - the vidmem's own records describe a newer allocation of it (a park that replaced an older
 * one: ECC review of N41) */
static void rd3AccelVidmemGoneAt(const void *vm, unsigned long long phys, bool vmToo)
{
    /* a queued present of it never reaches the panel, then the latch record - one hold of the queue's lock
     * (DRIVER-PLAN N38, ECC review: the worker must not program it in between) */
    rd3AccelPresentForgetVm(vm, phys, vmToo);
    if (vm) rd3AccelScStaleGone(phys);
    if (vm) rd3AccelWgFreed(phys);       /* its write generation and samples go */
    if (!vmToo) return;
    rd3AccelScFrontGone(vm);
    rd3AccelA3DispGone(vm);
    for (unsigned h = 0; vm && h < kRd3MaxFB; h++)
        if (gCfShown[h].vm == vm) gCfShown[h].vm = nullptr;
    if (vm && gCfPendLock) {
        IOSimpleLockLock(gCfPendLock);
        for (unsigned h = 0; h < kRd3MaxFB; h++) rd3_cf_pend_forget(&gCfPend[h], vm);
        IOSimpleLockUnlock(gCfPendLock);
    }
}
/* a buffer some head's last swap presented (recorded only: card run 58b
 * review, a scanout is not "shown" by being one) */
static bool rd3AccelWasPresented(const RD3VidMemory *vm)
{
    if (!vm) return false;
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

/* why a copy forward copied nothing or was refused: its own log, apart from
 * RD3CFLOG's 60 lines (spent by the 13th call in card run 58b) - 6 lines per
 * reason, then every 1024th of it */
enum { kCfWhyDisabled = 0, kCfWhyNoTarget, kCfWhyNoRects, kCfWhyNoFront, kCfWhyNoHead, kCfWhyNothingShown,
       kCfWhyForeign, kCfWhyStale, kCfWhyMiss, kCfWhyNotLanded, kCfWhyHeadDiffers, kCfWhyFrontNotOwn, kCfWhyBlocked,
       kCfWhyForced, kCfWhyEvicted, kCfWhyOverflow, kCfWhyNoHeadMiss, kCfWhyCount };
static unsigned gCfWhyLogs[kCfWhyCount];
#define RD3CFWHY(why, ...) do { const unsigned _k = gCfWhyLogs[(why)]++; \
                                if (_k < 6 || (_k & 1023) == 0) ALOG(__VA_ARGS__); } while (0)

/* the head of a copy forward: the head whose framebuffer resource is its
 * front first (the family's own answer), else the head its surface's swaps
 * were routed to; a disagreement is logged. kRd3MaxFB when neither. */
static unsigned rd3AccelCfHead(const void *surf, IOAccelResource2 *src, unsigned n)
{
    unsigned byFront = kRd3MaxFB, bySurf = kRd3MaxFB;
    if (src && gHeadFbResLock) {
        IOSimpleLockLock(gHeadFbResLock);
        for (unsigned h = 0; h < kRd3MaxFB && byFront == kRd3MaxFB; h++)
            if (gHeadFbRes[h] == src) byFront = h;
        IOSimpleLockUnlock(gHeadFbResLock);
    }
    for (unsigned i = 0; i < 8 && bySurf == kRd3MaxFB; i++)
        if (surf && gSurfHead[i] == surf && gSurfHeadIdx[i] < kRd3MaxFB) bySurf = gSurfHeadIdx[i];
    if (byFront < kRd3MaxFB && bySurf < kRd3MaxFB && byFront != bySurf)
        RD3CFWHY(kCfWhyHeadDiffers, "copy forward #%u: its front is head %u's framebuffer resource, its surface's swaps "
                 "went to head %u -- head %u taken", n, byFront, bySurf, byFront);
    return byFront < kRd3MaxFB ? byFront : bySurf;
}

/* rdna3_swap_front.h's pending table, always under gCfPendLock (the vidmem
 * free path drops lists on any thread); nothing else is done under it */
static void rd3CfPendNote(unsigned head, const void *buf, unsigned flags)
{
    if (flags & RD3_PEND_EVICTED) {
        const SInt64 k = OSAddAtomic64(1, &gCntCfPendEvicted);
        RD3CFWHY(kCfWhyEvicted, "copy forward: head %u: the pending-rect table was full, the oldest list went (its "
                 "rects may stay stale) (%lld such)", head, (long long)k + 1);
    }
    if (flags & RD3_PEND_OVERFLOW) {
        const SInt64 k = OSAddAtomic64(1, &gCntCfPendWhole);
        RD3CFWHY(kCfWhyOverflow, "copy forward: head %u: more than %u rects pending in one buffer -- the whole picture is "
                 "pending there (%lld such)", head, RD3_CF_PEND_RECTS, (long long)k + 1);
    }
    (void)buf;
}
static void rd3CfPendAdd(unsigned head, const void *buf, const struct rd3_rect *r, unsigned n, const void *keep)
{
    if (head >= kRd3MaxFB || !buf || !gCfPendLock) return;
    IOSimpleLockLock(gCfPendLock);
    const unsigned flags = rd3_cf_pend_add(&gCfPend[head], buf, r, n, keep);
    IOSimpleLockUnlock(gCfPendLock);
    rd3CfPendNote(head, buf, flags);
}
static unsigned rd3CfPendTake(unsigned head, const void *dst, const void *src, unsigned w, unsigned h, bool engine,
                              struct rd3_rect *out, unsigned max, unsigned n)
{
    if (head >= kRd3MaxFB || !gCfPendLock) return 0;
    struct rd3_cf_take t = { 0, 0 };
    IOSimpleLockLock(gCfPendLock);
    const unsigned k = rd3_cf_pend_take(&gCfPend[head], dst, src, (int)w, (int)h, engine ? 1 : 0, out, max, &t);
    IOSimpleLockUnlock(gCfPendLock);
    if (t.blocked) {
        const SInt64 c = OSAddAtomic64(1, &gCntCfPendBlocked);
        RD3CFWHY(kCfWhyBlocked, "copy forward #%u: head %u: the source missed the target's pending rects too -- they wait "
                 "(%u blocked calls in a row copy anyway) (%lld such)", n, head, RD3_CF_PEND_BLOCKED_MAX, (long long)c + 1);
    }
    if (t.forced) {
        const SInt64 c = OSAddAtomic64(1, &gCntCfPendForced);
        RD3CFWHY(kCfWhyForced, "copy forward #%u: head %u: pending rects copied from a source that missed them too, after "
                 "%u blocked calls -- both buffers alike there (stale, no A B A B) (%lld such)", n, head,
                 RD3_CF_PEND_BLOCKED_MAX, (long long)c + 1);
    }
    return k;
}

/* the call's rects stay stale in its target (nothing copied into it this
 * time): pending there, copied with the next copy forward into it that can.
 * 'whole': the whole target (no rect array); else the rects, or the whole
 * target when there are more than the table holds; none: nothing. */
static void rd3AccelCfPendCall(unsigned head, const void *dstVm, const RD3CfBounds *b, unsigned count, bool whole)
{
    if (head >= kRd3MaxFB || !dstVm) return;
    if (whole) { rd3CfPendAdd(head, dstVm, nullptr, 0, nullptr); return; }
    if (!b || !count) return;
    if (count > RD3_CF_PEND_RECTS) { rd3CfPendAdd(head, dstVm, nullptr, 0, nullptr); return; }
    struct rd3_rect r[RD3_CF_PEND_RECTS];
    for (unsigned i = 0; i < count; i++) { r[i].x = b[i].x; r[i].y = b[i].y; r[i].w = b[i].w; r[i].h = b[i].h; }
    rd3CfPendAdd(head, dstVm, r, count, nullptr);
}

/* one of the calling surface's own buffers (shapeSurface's array): the one
 * over 'vm', or 'res' itself; scanned only when an answer is needed */
static IOAccelResource2 *rd3AccelCfOwn(IOAccelResource2 *const *bufs, unsigned nbufs, const void *vm,
                                       IOAccelResource2 *res)
{
    for (unsigned i = 0; bufs && (vm || res) && i < nbufs; i++) {
        IOAccelResource2 *r = OSDynamicCast(IOAccelResource2, (OSObject *)bufs[i]);
        if (!r) continue;
        if (res ? r == res : (const void *)rd3res_vidmem(r) == vm) return r;
    }
    return nullptr;
}

/* one rect of a copy forward: SDMA, else the CPU for small rects; false: not copied */
static bool rd3AccelCfOne(const RD3CfSide &S, const RD3CfSide &D, const RD3CfBounds &r, unsigned w, unsigned h, bool cpu,
                          unsigned long long *us, unsigned long long *bytes)
{
    int x0, y0, x1, y1;
    if (!rd3AccelCfClip(r, w, h, &x0, &y0, &x1, &y1)) return true;   /* nothing inside: nothing to copy */
    const unsigned rowb = (unsigned)(x1 - x0) * 4u, rows = (unsigned)(y1 - y0);
    const unsigned long long so = (unsigned long long)y0 * S.pitch + (unsigned)x0 * 4u;
    const unsigned long long dof = (unsigned long long)y0 * D.pitch + (unsigned)x0 * 4u;
    if (rd3AccelCfSdma(S, D, so, dof, rowb, rows, us)) gCfSdma++;
    else if (cpu && (unsigned long long)rowb * rows <= RD3CF_CPU_MAX) { rd3AccelCfCpu(S, D, so, dof, rowb, rows); gCfCpu++; }
    else { gCfFailed++; return false; }
    *bytes += (unsigned long long)rowb * rows;
    return true;
}

/* copy forward's front was written after the head's last frame (the write
 * generations, card run 2026-10-17d; debug.rdna3accel_swap_gen=0: never) */
static int rd3AccelCfGenNewer(unsigned head, const RD3VidMemory *srcVm)
{
    if (!gSwapGen || head >= kRd3MaxFB || !srcVm || !srcVm->fPhys) return 0;
    return rd3_wg_src_newer(rd3AccelWgGen(srcVm->fPhys), rd3AccelWgHeadGen(head));
}
/* ... and that changed the source: counted (cf_gennewer_h<N>), the first ones logged */
static unsigned gCfGenLogs[kRd3MaxFB];
static void rd3AccelCfGenNote(unsigned head, const RD3VidMemory *srcVm, const char *instead)
{
    if (head >= kRd3MaxFB) return;
    const SInt64 k = OSAddAtomic64(1, &gCntCfGenNewer[head]);
    if (rd3_log_due(gCfGenLogs[head]++, 8, 1023))
        ALOG("copy forward #%u: head %u: the front (phys 0x%llx, generation %llu) was written after the head's frame "
             "(generation %llu) -- copied from it instead of %s (a copy forward burst's chain; %lld such)", gCfCalls,
             head, srcVm ? srcVm->fPhys : 0ull, srcVm ? rd3AccelWgGen(srcVm->fPhys) : 0ull, rd3AccelWgHeadGen(head),
             instead, (long long)k + 1);
}

/* copy forward's source in modes 1/2 (rdna3_swap_copy.h): the front the
 * swap copied into, else the scanout buffer the panel shows; false: none
 * (the rects stay pending, counted) */
static bool rd3AccelCfScSource(unsigned head, IOAccelResource2 *src, RD3VidMemory *srcVm, const RD3CfSide &D,
                               RD3CfSide *S, unsigned *scPick, unsigned long long *usedPhys)
{
    bool srcIsHeadFront = false;
    if (src && gHeadFbResLock) {
        IOSimpleLockLock(gHeadFbResLock);
        for (unsigned h = 0; h < kRd3MaxFB; h++)
            if (gHeadFbRes[h] == src) srcIsHeadFront = true;
        IOSimpleLockUnlock(gHeadFbResLock);
    }
    rd3AccelScApply(head);   /* under the family's lock: the panel moves posted since the last swap */
    struct rd3_sc_cf_in in;
    in.srcIsDst = srcVm && srcVm == D.vm;
    in.srcIsHeadFront = srcIsHeadFront && head < kRd3MaxFB;
    in.frontCurrent = head < kRd3MaxFB && srcVm &&
                      rd3_sc_front_current(&gSc[head].front, srcVm, srcVm->fAllocationCookie);
    in.scanoutKnown = head < kRd3MaxFB && gSc[head].scanoutFrame &&
                      (gSc[head].shows == RD3_SCS_D || gSc[head].shows == RD3_SCS_HOME);
    in.srcNewer = 0;
    const enum rd3_sc_cf oldPick = rd3_sc_cf_source(&in);
    /* the write generations (card run 2026-10-17d): a front written after the head's frame holds a newer
     * picture than the panel (a copy forward burst's chain): the source */
    in.srcNewer = rd3AccelCfGenNewer(head, srcVm);
    const enum rd3_sc_cf pick = rd3_sc_cf_source(&in);
    if (pick != oldPick) rd3AccelCfGenNote(head, srcVm, oldPick == RD3_SCCF_SCANOUT ? "the panel's buffer"
                                                                                    : "nothing (rects pending)");
    *scPick = (unsigned)pick;
    if (pick == RD3_SCCF_SRC) {
        *usedPhys = srcVm ? srcVm->fPhys : 0;
        return src && rd3AccelCfSide(src, S);
    }
    if (pick != RD3_SCCF_SCANOUT) return false;
    RD3ScHead &C = gSc[head];
    bzero(S, sizeof *S);
    S->cpu = false;
    if (C.shows == RD3_SCS_D && C.dHandle) {
        S->cookie = C.dCookie; S->bytes = C.dBytes; S->w = C.dW; S->h = C.dH; S->pitch = C.dPitch;
        *usedPhys = C.dPhys;
        return true;
    }
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    IOService *fb = a ? a->fFBs[head] : nullptr;
    struct RD3KVramRequest q;
    bzero(&q, sizeof q);
    q.version = RD3K_VRAM_ABI_VERSION;
    if (!fb || a->askFramebufferForScanout(&q, fb) != kIOReturnSuccess || !q.pitch || !q.actualSize) return false;
    S->cookie = q.allocationCookie; S->bytes = q.actualSize; S->w = q.width; S->h = q.height; S->pitch = q.pitch;
    *usedPhys = q.phys;
    return true;
}

/* the trace's record of a copy forward call (Q4), filled by the call */
struct RD3CfTrace { struct rd3_trace_ev e; };
static IOReturn rd3AccelCopyForwardIn(const void *surf, unsigned fbIndex, IOAccelResource2 *src, IOAccelResource2 *dst,
                                      const void *rects, unsigned count, unsigned smode, IOAccelResource2 *const *bufs,
                                      unsigned nbufs, RD3CfTrace *t);
static unsigned gCfErrLogs;
IOReturn rd3AccelCopyForward(const void *surf, unsigned fbIndex, IOAccelResource2 *src, IOAccelResource2 *dst,
                             const void *rects, unsigned count, unsigned smode, IOAccelResource2 *const *bufs,
                             unsigned nbufs)
{
    RD3CfTrace t;
    t.e = rd3TrEv(RD3_TR_CF);
    t.e.obj = rd3TrObj(surf);
    t.e.v0 = count;
    t.e.pa = rd3res_phys(src);
    t.e.pb = rd3res_phys(dst);
    if (rd3AccelScCopying()) t.e.flags |= 1;
    {
        struct rd3_trace_box box;
        const RD3CfBounds *b = (const RD3CfBounds *)rects;
        bzero(&box, sizeof box);
        for (unsigned i = 0; b && i < count; i++) rd3_trace_box_add(&box, b[i].x, b[i].y, b[i].w, b[i].h);
        rd3_trace_put16(t.e.ba, 0, rd3_trace_u16(box.x0)); rd3_trace_put16(t.e.ba, 1, rd3_trace_u16(box.y0));
        rd3_trace_put16(t.e.ba, 2, rd3_trace_u16(box.x1)); rd3_trace_put16(t.e.ba, 3, rd3_trace_u16(box.y1));
    }
    const IOReturn r = rd3AccelCopyForwardIn(surf, fbIndex, src, dst, rects, count, smode, bufs, nbufs, &t);
    /* WindowServer refreshes the target for a new frame: never a stale back
     * until it is presented again (card run 2026-10-17c); whatever the call
     * did; in every head's table */
    rd3AccelScStaleCf(t.e.pb);
    /* a write into the target (the write generations, 2026-10-17d) - only when every rect went through: a target
     * the copy did not (wholly) write keeps its generation (ECC review round 1) */
    if (r == kIOReturnSuccess && t.e.v1 > 0 && !(t.e.flags & 4)) {
        rd3AccelWgBumpPhys(t.e.pb, kWgCf);
    } else if (count > 0 && t.e.pb) {
        const SInt64 k = OSAddAtomic64(1, &gCntWgCfNoStamp);
        if (rd3_log_due((uint64_t)k, 8, 1023))
            ALOG("copy forward #%u: the target 0x%llx keeps its write generation: the copy %s (0x%x, %u of %u rects "
                 "copied) (%lld such; debug.rdna3accel_swaps_gen_cfnostamp)", gCfCalls, t.e.pb,
                 r != kIOReturnSuccess ? "was answered with an error" : "did not write every rect", (unsigned)r,
                 t.e.v1, count, (long long)k + 1);
    }
    t.e.v4 = (uint32_t)r;
    rd3Trace(t.e);
    /* the target after the copy (Q3) */
    if (t.e.v1) {
        RD3Scan sc;
        rd3AccelResScan(dst, &sc);
        rd3AccelTraceScan(t.e.head < kRd3MaxFB ? t.e.head : kRd3MaxFB, surf, rd3AccelBufIndex(bufs, nbufs, dst),
                          rd3res_phys(dst), sc, 32u, gCfCalls, 0, dst);
    }
    /* anything but success makes CoreDisplay switch copy forward off for
     * good ("copy forward failed ... disabling"): said every time it happens
     * (only rdna3accelcf=0 answers so, on purpose) */
    if (r != kIOReturnSuccess && rd3_log_due(gCfErrLogs++, 8, 255))
        ALOG("copy forward #%u: answered 0x%x to CoreDisplay -- it switches copy forward OFF for the session "
             "(rdna3accelcf=%d)", gCfCalls, (unsigned)r, gBootCopyForward);
    return r;
}
static IOReturn rd3AccelCopyForwardIn(const void *surf, unsigned fbIndex, IOAccelResource2 *src, IOAccelResource2 *dst,
                                      const void *rects, unsigned count, unsigned smode, IOAccelResource2 *const *bufs,
                                      unsigned nbufs, RD3CfTrace *t)
{
    const unsigned n = ++gCfCalls;
    OSAddAtomic64(1, &gCntCfCalls);
    RD3CfSide S = {}, D = {};
    const bool okD = dst && rd3AccelCfSide(dst, &D);
    RD3VidMemory *srcVm = rd3res_vidmem(src), *dstVm = rd3res_vidmem(dst);
    const RD3CfBounds *b = (const RD3CfBounds *)rects;
    if (n <= 12) {
        RD3CfSide F = {};
        const bool okF = src && rd3AccelCfSide(src, &F);
        RD3CFLOG("copy forward #%u: surface %p fb index %u smode 0x%x, %u rect(s)%s; front phys 0x%llx (%ux%u pitch %u "
                 "off 0x%llx of %llu%s%s) -> target phys 0x%llx (%ux%u pitch %u off 0x%llx of %llu%s%s)", n, surf,
                 fbIndex, smode, count, gBootCopyForward ? "" : " [rdna3accelcf=0: refused]", rd3res_phys(src), F.w, F.h,
                 F.pitch, F.cookie, F.bytes, srcVm && srcVm->fScanout ? " scanout" : "", okF ? "" : " NOT COPYABLE",
                 rd3res_phys(dst), D.w, D.h, D.pitch, D.cookie, D.bytes, D.vm && D.vm->fScanout ? " scanout" : "",
                 okD ? "" : " NOT COPYABLE");
        for (unsigned i = 0; b && i < count && i < 4; i++)
            RD3CFLOG("   rect %u: %d,%d %dx%d", i, b[i].x, b[i].y, b[i].w, b[i].h);
    }
    if (!gBootCopyForward) {
        OSAddAtomic64(1, &gCntCfDisabled);
        RD3CFWHY(kCfWhyDisabled, "copy forward #%u: refused, rdna3accelcf=0 (CoreDisplay switches it off)", n);
        return kIOReturnUnsupported;
    }
    /* where the last frame is (rdna3_swap_front.h, card run 58b): the
     * buffer this surface's head showed last, when it is one of this
     * surface's own buffers (never another surface's, another process's) */
    const unsigned head = rd3AccelCfHead(surf, src, n);
    t->e.head = (uint8_t)(head < kRd3MaxFB ? head : 0xffu);
    /* states that pass (a target being re-allocated, no rect array): the
     * call's rects stay pending in the target, not refused to CoreDisplay,
     * which would switch copy forward off for good (review of e2bf78e) */
    if (!okD) {
        OSAddAtomic64(1, &gCntCfRefused); gCfFailed++;
        rd3AccelCfPendCall(head, dstVm, b, count, count && !b);
        RD3CFWHY(kCfWhyNoTarget, "copy forward #%u: the target (phys 0x%llx) is not copyable now (no vidmem, mapping or "
                 "4-byte geometry) -- nothing copied, %s%s", n, rd3res_phys(dst),
                 count && !b ? "the whole target is pending (no rect array)" : count ? "its rects stay pending"
                                                                             : "no rects to keep",
                 head < kRd3MaxFB && dstVm ? "" : " (no head or no vidmem: not even that)");
        return kIOReturnSuccess;
    }
    if (count && !b) {
        OSAddAtomic64(1, &gCntCfRefused); gCfFailed++;
        rd3AccelCfPendCall(head, D.vm, nullptr, 0, true);
        RD3CFWHY(kCfWhyNoRects, "copy forward #%u: %u rect(s) but no rect array -- nothing copied, the whole target is "
                 "pending", n, count);
        return kIOReturnSuccess;
    }
    /* modes 1/2 (card run 2026-10-17b): the front holds the last frame */
    const bool scMode = rd3AccelScCopying();   /* modes 1/2, the SDMA breaker not open */
    unsigned scPick = 0;
    enum rd3_cf_pick pick = RD3_CF_SKIP_SAME;
    if (scMode) {
        unsigned long long used = 0;
        const bool ok = rd3AccelCfScSource(head, src, srcVm, D, &S, &scPick, &used);
        t->e.arg = (uint8_t)(16u + scPick);
        t->e.pc = used;
        if (scPick == RD3_SCCF_SAME) {
            OSAddAtomic64(1, &gCntCfSame); gCfSkipped++;
            return kIOReturnSuccess;
        }
        if (!ok) {
            if (head < kRd3MaxFB) OSAddAtomic64(1, &gCntCfScStale[head]);
            else OSAddAtomic64(1, &gCntCfNoHead);
            rd3AccelCfPendCall(head, D.vm, b, count, false);
            RD3CFWHY(kCfWhyStale, "copy forward #%u: head %u: neither the front (phys 0x%llx) nor the panel's buffer "
                     "holds the last frame (swap mode %d: the swap's copy into the front failed or nothing was copied "
                     "yet) -- nothing copied, %u rect(s) pending in the target", n, head, rd3res_phys(src), gSwapMode,
                     count);
            return kIOReturnSuccess;
        }
        goto copy;
    }
    {
    /* the target still leaving a panel? then the copy waits for that latch (the present queue, N38) */
    if (D.vm && !D.vm->fScanout && (gSwapAsync || gIopQueue)) rd3AccelPresentReuseGate(D.vm->fPhys, 1);
    /* the target still on the panel? (card run 2026-10-17) */
    rd3AccelCfPanelCheck(head, D.vm, n);
    const void *shownVm = head < kRd3MaxFB ? gCfShown[head].vm : nullptr;
    const unsigned long long shownCookie = head < kRd3MaxFB ? gCfShown[head].cookie : 0;
    /* the surface's own buffer the head showed, looked for only when it is
     * neither the target nor the front */
    IOAccelResource2 *own = head < kRd3MaxFB && shownVm && shownVm != (const void *)D.vm &&
                            shownVm != (const void *)srcVm ? rd3AccelCfOwn(bufs, nbufs, shownVm, nullptr) : nullptr;
    struct rd3_cf_in in;
    in.srcIsDst = srcVm && srcVm == D.vm;
    in.headKnown = head < kRd3MaxFB;
    in.headShown = shownVm != nullptr;
    in.shownIsDst = shownVm && shownVm == (const void *)D.vm;
    in.shownIsSrc = shownVm && shownVm == (const void *)srcVm;
    in.shownOwned = own != nullptr;
    in.srcShown = rd3AccelWasPresented(srcVm);
    in.srcNewer = 0;
    const enum rd3_cf_pick oldPick = rd3_cf_source(&in);
    /* the write generations (card run 2026-10-17d): a front written after the head's frame is the newest
     * picture of WindowServer's chain (a copy forward burst) - never the older buffer the head shows over it
     * (17d: COPY_SHOWN copied the stale buf[2] mode 0 had flipped to back over the new buf[1]) */
    in.srcNewer = head < kRd3MaxFB ? rd3AccelCfGenNewer(head, srcVm) : 0;
    pick = rd3_cf_source(&in);
    if (pick != oldPick)
        rd3AccelCfGenNote(head, srcVm, oldPick == RD3_CF_COPY_SHOWN ? "the buffer the head showed"
                                       : oldPick == RD3_CF_SKIP_BACK_IS_LAST ? "nothing (the target is the buffer "
                                                                               "the head showed)"
                                       : "nothing (rects pending)");
    t->e.arg = (uint8_t)pick;
    switch (pick) {
    case RD3_CF_SKIP_SAME:
        OSAddAtomic64(1, &gCntCfSame); gCfSkipped++;
        return kIOReturnSuccess;
    case RD3_CF_SKIP_BACK_IS_LAST: {
        /* one back: it is WindowServer's last frame, nothing to bring forward */
        const SInt64 k = OSAddAtomic64(1, &gCntCfBackLast[head]);
        if (k < 4 || (k & 0xffff) == 0)
            ALOG("copy forward #%u: head %u: the target (phys 0x%llx) is the buffer the head showed last -- nothing to "
                 "copy (%lld such calls)", n, head, D.vm->fPhys, (long long)k + 1);
        return kIOReturnSuccess;
    }
    case RD3_CF_SKIP_NOTHING_SHOWN: {
        const SInt64 k = OSAddAtomic64(1, &gCntCfNothingShown[head]);
        rd3AccelCfPendCall(head, D.vm, b, count, false);
        RD3CFWHY(kCfWhyNothingShown, "copy forward #%u: head %u has shown nothing recorded yet (before its first swap, "
                 "after a free or a mode switch) -- nothing copied (the front, phys 0x%llx, is not the last frame "
                 "either), %u rect(s) pending in the target (%lld such calls)", n, head, rd3res_phys(src), count,
                 (long long)k + 1);
        return kIOReturnSuccess;
    }
    case RD3_CF_SKIP_NO_HEAD: {
        const SInt64 k = OSAddAtomic64(1, &gCntCfNoHead);
        RD3CFWHY(kCfWhyNoHead, "copy forward #%u: no head known for this surface and its front (phys 0x%llx) was never "
                 "shown -- nothing copied, the rects stay as WindowServer left them (%lld such calls)", n,
                 rd3res_phys(src), (long long)k + 1);
        return kIOReturnSuccess;
    }
    case RD3_CF_SKIP_FOREIGN: {
        const SInt64 k = OSAddAtomic64(1, &gCntCfForeign[head]);
        rd3AccelCfPendCall(head, D.vm, b, count, false);
        RD3CFWHY(kCfWhyForeign, "copy forward #%u: head %u shows a buffer that is not this surface's (another surface, "
                 "an IOSurface flip) -- never a source, nothing copied, %u rect(s) pending in the target (%lld such "
                 "calls)", n, head, count, (long long)k + 1);
        return kIOReturnSuccess;
    }
    case RD3_CF_COPY_SHOWN: {
        /* the surface's own buffer, read through its live resource */
        const RD3VidMemory *ov = rd3res_vidmem(own);
        if (!rd3AccelCfSide(own, &S) || !ov || ov->fAllocationCookie != shownCookie || S.vm == D.vm) {
            const SInt64 k = OSAddAtomic64(1, &gCntCfShownStale);
            rd3AccelCfPendCall(head, D.vm, b, count, false);
            RD3CFWHY(kCfWhyStale, "copy forward #%u: head %u: the buffer it showed (phys 0x%llx) is not usable now "
                     "(re-allocated or not copyable) -- nothing copied, %u rect(s) pending in the target (%lld such "
                     "calls)", n, head, rd3res_phys(own), count, (long long)k + 1);
            return kIOReturnSuccess;
        }
        break;
    }
    case RD3_CF_COPY_SRC:
    default:
        /* the front only when it is this surface's own buffer too */
        const bool srcOwn = src && rd3AccelCfOwn(bufs, nbufs, nullptr, src) != nullptr;
        if (!src || !srcOwn || !rd3AccelCfSide(src, &S)) {
            OSAddAtomic64(1, &gCntCfRefused); gCfFailed++;
            rd3AccelCfPendCall(head, D.vm, b, count, false);
            RD3CFWHY(srcOwn ? kCfWhyNoFront : kCfWhyFrontNotOwn, "copy forward #%u: the front (phys 0x%llx) holds the "
                     "last frame but %s -- nothing copied, %u rect(s) pending in the target", n, rd3res_phys(src),
                     srcOwn ? "is not copyable now" : "is not one of the surface's own buffers", count);
            return kIOReturnSuccess;
        }
        break;
    }
    }
copy:
    t->e.pc = t->e.pc ? t->e.pc : (S.vm ? S.vm->fPhys : 0);
    const unsigned w = S.w < D.w ? S.w : D.w, h = S.h < D.h ? S.h : D.h;
    const bool cpu = S.cpu && D.cpu;
    /* the rects the target missed before, those the source holds (it did
     * not miss them itself); the whole picture only with the copy engine on */
    struct rd3_rect extra[RD3_CF_PEND_RECTS];
    const unsigned ne = rd3CfPendTake(head, D.vm, S.vm, w, h, !gCfSdmaOff, extra, RD3_CF_PEND_RECTS, n);
    unsigned long long us = 0, bytes = 0;
    unsigned best = 0, bestArea = 0, done = 0, failed = 0, failedExtra = 0;
    int x0, y0, x1, y1;
    for (unsigned i = 0; i < count; i++) {
        if (!rd3AccelCfClip(b[i], w, h, &x0, &y0, &x1, &y1)) continue;
        const unsigned area = (unsigned)(x1 - x0) * (unsigned)(y1 - y0);
        if (area > bestArea) { bestArea = area; best = i; }
    }
    /* the check pixel: one of the biggest rect whose stale and new values differ */
    unsigned cx = 0, cy = 0, before = 0, want = 0;
    bool check = false;
    if (cpu && S.vm && bestArea && (n <= 400 || n % 200 == 0) && !gCfCheck.armed &&
        rd3AccelCfClip(b[best], w, h, &x0, &y0, &x1, &y1)) {
        for (unsigned gy = 1; gy <= 4 && !check; gy++)
            for (unsigned gx = 1; gx <= 4 && !check; gx++) {
                const unsigned x = (unsigned)(x0 + (x1 - x0) * (int)gx / 5), y = (unsigned)(y0 + (y1 - y0) * (int)gy / 5);
                before = rd3px(D.vm, D.pitch, x, y);
                want = rd3px(S.vm, S.pitch, x, y);
                if (before != want) { cx = x; cy = y; check = true; }
            }
    }
    struct rd3_rect missed[RD3_CF_PEND_RECTS];
    unsigned nm = 0;
    bool overflow = false;
    for (unsigned i = 0; i < count + ne; i++) {
        RD3CfBounds r;
        if (i < count) r = b[i];
        else {
            const struct rd3_rect &e = extra[i - count];
            r.x = (short)e.x; r.y = (short)e.y;
            r.w = (short)(e.w > 0x7fff ? 0x7fff : e.w); r.h = (short)(e.h > 0x7fff ? 0x7fff : e.h);
        }
        if (rd3AccelCfOne(S, D, r, w, h, cpu, &us, &bytes)) { done++; continue; }
        failed++;
        if (i >= count) failedExtra++;
        if (nm < RD3_CF_PEND_RECTS) { missed[nm].x = r.x; missed[nm].y = r.y; missed[nm].w = r.w; missed[nm].h = r.h; nm++; }
        else overflow = true;
    }
    __asm__ volatile("mfence" ::: "memory");   /* the CPU's writes through the write-combined BAR */
    gCfRects += done; gCfBytes += bytes; gCfUsec += us;
    t->e.v1 = done;
    t->e.v2 = rd3TrSat(bytes >> 10);
    t->e.v3 = rd3TrSat(us);
    t->e.bb[0] = (uint8_t)(ne > 255 ? 255 : ne);
    t->e.bb[1] = (uint8_t)(failed > 255 ? 255 : failed);
    if (failed) t->e.flags |= 4;
    if (scMode && !failed && head < kRd3MaxFB)
        OSAddAtomic64(1, scPick == RD3_SCCF_SCANOUT ? &gCntCfScScanout[head] : &gCntCfScSrc[head]);
    if (us > gCfUsecMax) gCfUsecMax = us;
    if (check) {
        const unsigned after = rd3px(D.vm, D.pitch, cx, cy);
        gCfCheck.vm = D.vm; gCfCheck.x = cx; gCfCheck.y = cy; gCfCheck.pitch = D.pitch;
        gCfCheck.before = before; gCfCheck.copied = after; gCfCheck.call = n;
        __asm__ volatile("" ::: "memory");
        gCfCheck.armed = true;
        if (after != want)
            RD3CFWHY(kCfWhyNotLanded, "copy forward #%u: pixel (%u,%u) reads 0x%08x after the copy, the source has "
                     "0x%08x -- the copy did NOT land", n, cx, cy, after, want);
    }
    if (n <= 12 || n % 600 == 0)
        ALOG("copy forward #%u: %u of %u rect(s) (%u pending before), %llu KiB in %llu us%s [total: %u calls, %u rects "
             "(%u SDMA, %u CPU, %u failed), %llu MiB, %llu us SDMA, max %llu us per call, %u same buffer]", n, done,
             count + ne, ne, bytes >> 10, us, gCfSdmaOff ? " (SDMA off)" : "", n, gCfRects, gCfSdma, gCfCpu, gCfFailed,
             gCfBytes >> 20, gCfUsec, gCfUsecMax, gCfSkipped);
    if (head >= kRd3MaxFB) {
        /* no head (the front was shown): no table to keep the miss in;
         * still not reported to CoreDisplay (copy forward off for good) */
        if (failed) OSAddAtomic64(1, &gCntCfNoHeadMiss);
        if (failed)
            RD3CFWHY(kCfWhyNoHeadMiss, "copy forward #%u: no head known: %u of %u rect(s) not copied (SDMA refused, too "
                     "big for the CPU) -- they stay stale, nowhere to keep them", n, failed, count);
        return kIOReturnSuccess;
    }
    if (pick == RD3_CF_COPY_SHOWN && !failed) OSAddAtomic64(1, &gCntCfShown[head]);
    if (pick == RD3_CF_COPY_SRC && !failed) OSAddAtomic64(1, &gCntCfSrc[head]);
    if (ne > failedExtra) {
        const SInt64 k = OSAddAtomic64(1, &gCntCfHealed[head]);
        if (k < 8 || (k & 1023) == 0)
            ALOG("copy forward #%u: head %u: %u rect(s) the target (phys 0x%llx) had missed copied too (%lld such calls)",
                 n, head, ne - failedExtra, D.vm->fPhys, (long long)k + 1);
    }
    if (failed) {
        /* rects left stale: pending in this target, copied with the next
         * copy forward into it that can; not reported to CoreDisplay, which
         * would switch copy forward off for good (rdna3_swap_front.h) */
        const SInt64 k = OSAddAtomic64(1, &gCntCfMiss[head]);
        rd3CfPendAdd(head, D.vm, overflow ? nullptr : missed, overflow ? ~0u : nm, S.vm);
        RD3CFWHY(kCfWhyMiss, "copy forward #%u: head %u: %u of %u rect(s) not copied (SDMA refused%s, too big for the "
                 "CPU) -- pending in the target (phys 0x%llx) (%lld such calls)", n, head, failed, count + ne,
                 gCfSdmaOff ? " and off" : "", D.vm->fPhys, (long long)k + 1);
    }
    return kIOReturnSuccess;
}


/* ---- the swap as a copy: the copy itself (card run 2026-10-17b) ---- */
/* rdna3CopyVram through the head's framebuffer (synchronous: the engine is
 * done when it returns) */
static IOReturn rd3AccelScSdma(IOService *fb, unsigned long long srcCookie, unsigned srcPitch,
                               unsigned long long dstCookie, unsigned dstPitch, unsigned rowb, unsigned rows,
                               unsigned long long *us, int *status)
{
    struct RD3KCopyRequest c;
    bzero(&c, sizeof c);
    c.version = RD3K_COPY_ABI_VERSION;
    c.srcCookie = srcCookie; c.dstCookie = dstCookie;
    c.srcPitch = srcPitch; c.dstPitch = dstPitch; c.rowBytes = rowb; c.rows = rows;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_COPY_FN);
    if (!sym) return kIOReturnNoMemory;
    const IOReturn r = fb->callPlatformFunction(sym, false, &c, nullptr, nullptr, nullptr);
    sym->release();
    *us = c.usec;
    *status = c.status;
    return r;
}
/* one look at the head's last flip (no wait): seen latched -> recorded */
static void rd3AccelScLatchEvent(unsigned head, enum rd3_latch_event e, const char *why);
static void rd3AccelScLook(unsigned head, IOService *fb, const char *why)
{
    RD3ScHead &S = gSc[head];
    if (!fb || !S.flipped || S.seen) return;
    RD3KDisplayStatus st;
    RD3StatusWhy w;
    if (rd3AccelDispStatus(fb, head, &st, &w) && !st.flipPending) {
        S.latched = true;
        S.seen = true;
        rd3AccelScLatchEvent(head, rd3_latch_seen(&S.lat), why);
    }
}
static unsigned gScDLogs, gScFreeLogs[kRd3MaxFB];
/* D (re)allocated for a w x h head; false: the swap copies into home
 * directly. An old D is freed only off the panel, once the flip away from
 * it was seen latched (rd3_sc_d_freeable, ECC review round 1). */
static bool rd3AccelScEnsureD(unsigned head, IOService *fb, unsigned w, unsigned h, unsigned n)
{
    RD3ScHead &S = gSc[head];
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a) return false;
    if (!rd3_sc_d_stale(S.dHandle != nullptr, S.dW, S.dH, w, h)) return true;
    if (S.dHandle) {
        rd3AccelScLook(head, fb, "the look before freeing the old second buffer");
        if (!rd3_sc_d_freeable((enum rd3_sc_shows)S.shows, S.flipped, S.seen)) {
            /* on the panel, or the flip away not latched yet: next swap */
            const SInt64 k = OSAddAtomic64(1, &gCntScFreeWait[head]);
            if (rd3_log_due(gScFreeLogs[head]++, 4, 1023))
                ALOG("swap #%u: head %u: the old second scanout buffer (phys 0x%llx, %ux%u) is %s -- not freed yet, "
                     "this swap copies into home (%lld such)", n, head, S.dPhys, S.dW, S.dH,
                     S.shows == RD3_SCS_D ? "on the panel" : "maybe still scanned (the flip away has not latched)",
                     (long long)k + 1);
            return false;
        }
        struct RD3KVramRequest r;
        bzero(&r, sizeof r);
        r.version = RD3K_VRAM_ABI_VERSION;
        r.handle = S.dHandle; r.kva = S.dKva; r.actualSize = S.dBytes; r.allocationCookie = S.dCookie;
        const IOReturn fr = a->askFramebufferForVram(&r, false);
        rd3AccelTraceVm(&S, false, fr == kIOReturnSuccess, S.dPhys, S.dBytes, 0, fr, true);
        ALOG("swap copy: head %u: second scanout buffer phys 0x%llx (%ux%u) freed for a %ux%u mode (0x%x)", head,
             S.dPhys, S.dW, S.dH, w, h, (unsigned)fr);
        S.dHandle = nullptr; S.dKva = nullptr; S.dPhys = S.dCookie = S.dBytes = 0; S.dW = S.dH = S.dPitch = 0;
    }
    if (S.dRetry) { S.dRetry--; return false; }
    const unsigned pitch = rd3_sc_pitch(w);
    struct RD3KVramRequest r;
    bzero(&r, sizeof r);
    r.version = RD3K_VRAM_ABI_VERSION;
    r.size = (unsigned long long)pitch * h;
    r.flags = RD3K_VRAM_OWNER_SWAP_D;   /* counted as swap_d in debug.rdna3_vram */
    const IOReturn ret = a->askFramebufferForVram(&r, true);
    if (ret != kIOReturnSuccess || !r.handle || !r.phys) {
        S.dRetry = 600;
        rd3AccelTraceVm(&S, true, false, 0, (unsigned long long)pitch * h, 0, ret, true);
        if (rd3_log_due(gScDLogs++, 8, 255))
            ALOG("swap copy: head %u: no VRAM for the second scanout buffer (%ux%u, 0x%x) -- the swaps copy into the "
                 "buffer on the panel (may tear, debug.rdna3accel_swaps_copydirect_h%u); tried again in 600 swaps",
                 head, w, h, (unsigned)ret, head);
        return false;
    }
    S.dHandle = r.handle; S.dKva = r.kva; S.dPhys = r.phys; S.dCookie = r.allocationCookie; S.dBytes = r.actualSize;
    S.dW = w; S.dH = h; S.dPitch = pitch;
    rd3AccelScStaleReset(head, "a new second scanout buffer");
    rd3AccelVramBase(r.phys, r.allocationCookie);
    rd3AccelTraceVm(&S, true, true, r.phys, r.actualSize, r.mappedTotal, kIOReturnSuccess, true);
    ALOG("swap copy: head %u: second scanout buffer phys 0x%llx %ux%u pitch %u (%llu bytes): the swaps copy into the "
         "buffer the panel does not show, then flip (tear-free)", head, r.phys, w, h, pitch, r.actualSize);
    return true;
}
static unsigned gScFailLogs[kRd3MaxFB], gScFrontLogs[kRd3MaxFB], gScWaitLogs[kRd3MaxFB], gScLatchEvLogs[kRd3MaxFB];
static void rd3AccelScLatchEvent(unsigned head, enum rd3_latch_event e, const char *why)
{
    if (e == RD3_LATCH_EV_NONE || head >= kRd3MaxFB) return;
    struct rd3_trace_ev le = rd3TrEv(RD3_TR_LATCH, head);
    le.arg = e == RD3_LATCH_EV_OFF ? 7 : 8;
    rd3Trace(le);
    if (!rd3_log_due(gScLatchEvLogs[head]++, 8, 255)) return;
    if (e == RD3_LATCH_EV_OFF)
        ALOG("swap copy: head %u: %u flips in a row did not latch in time -- the head's copies STOP WAITING for the "
             "last flip until one is seen latched (a copy may write the buffer the panel still reads: "
             "debug.rdna3accel_swaps_copynowait_h%u)", head, RD3_LATCH_TIMEOUT_RUN, head);
    else
        ALOG("swap copy: head %u: the head's copies wait for the last flip again (%s)", head, why);
}
/* the SDMA breaker's transitions: logged (the first 8, then every 64th),
 * traced (MODESET arg 2: v0 the old state, v1 the new: 0 closed, 1 open,
 * 2 a probe), counted (swaps_copybrktrips) */
static unsigned gScBrkLogs;
static void rd3AccelScBrkEvent(enum rd3_sc_brk_ev ev, unsigned n, unsigned head, int status, IOReturn r)
{
    if (ev == RD3_SC_BRK_NONE) return;
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_MODESET, head);
    e.arg = 2;
    e.v0 = ev == RD3_SC_BRK_OPENED ? 0 : ev == RD3_SC_BRK_PROBE ? 1 : 2;
    e.v1 = ev == RD3_SC_BRK_CLOSED ? 0 : ev == RD3_SC_BRK_PROBE ? 2 : 1;
    e.v2 = (uint32_t)status;
    e.v3 = gScBrk.trips;
    e.v4 = r;
    rd3Trace(e);
    if (ev == RD3_SC_BRK_OPENED || ev == RD3_SC_BRK_REOPENED) {
        if (ev == RD3_SC_BRK_OPENED) OSAddAtomic64(1, &gCntScBrkTrips);
        /* the swaps flip now: mode 0's records start empty, the front record too */
        for (unsigned h = 0; h < kRd3MaxFB; h++) {
            rd3AccelForgetShown(h);
            rd3AccelLatchForgetPres(h, "the SDMA breaker opened");
            gSwapEmptyRun[h] = 0;
            rd3_sc_front_set(&gSc[h].front, nullptr, 0);
        }
    }
    if (ev == RD3_SC_BRK_CLOSED)
        for (unsigned h = 0; h < kRd3MaxFB; h++) rd3_sc_front_set(&gSc[h].front, nullptr, 0);
    if (!rd3_log_due(gScBrkLogs++, 8, 63)) return;
    if (ev == RD3_SC_BRK_OPENED)
        ALOG("swap copy: SDMA BREAKER OPEN at swap #%u (head %u): %s (IOReturn 0x%x, status %d; %u failure(s) in a "
             "row) -- the swaps FLIP to WindowServer's back as in mode 0 (the flicker of card run 2026-10-17 may come "
             "back) instead of waiting on the engine at every swap; SDMA tried again in %llu s "
             "(debug.rdna3accel_swaps_copybrkflip_h<N>, swaps_copybrktrips)", n, head,
             status == kRd3Etime ? "the copy timed out" : "the copy was refused", (unsigned)r, status, gScBrk.run,
             RD3_SC_BRK_RETRY_US / 1000000ull);
    else if (ev == RD3_SC_BRK_REOPENED)
        ALOG("swap copy: SDMA breaker: the try at swap #%u failed (IOReturn 0x%x, status %d) -- the swaps keep "
             "flipping, next try in %llu s", n, (unsigned)r, status, RD3_SC_BRK_RETRY_US / 1000000ull);
    else if (ev == RD3_SC_BRK_PROBE)
        ALOG("swap copy: SDMA breaker: swap #%u tries the copy again", n);
    else
        ALOG("swap copy: SDMA breaker CLOSED at swap #%u: the copy went through -- the swaps copy again", n);
}
/* one SDMA copy's result into the breaker */
static void rd3AccelScSdmaNote(IOReturn r, int status, unsigned n, unsigned head)
{
    if (r == kIOReturnSuccess) rd3AccelScBrkEvent(rd3_sc_brk_ok(&gScBrk), n, head, 0, r);
    else rd3AccelScBrkEvent(rd3_sc_brk_fail(&gScBrk, rd3AccelUsNow(), status == kRd3Etime), n, head, status, r);
}
static void rd3AccelScFail(unsigned head, unsigned n, unsigned step, const char *why, unsigned long long phys,
                           unsigned v, struct rd3_trace_ev *tr)
{
    const SInt64 k = OSAddAtomic64(1, &gCntScFail[head]);
    tr->arg = RD3_TRS_COPY_FAILED;
    tr->v4 |= step << 16;
    if (rd3_log_due(gScFailLogs[head]++, 8, 1023))
        ALOG("swap #%u: head %u: the copy of the back (phys 0x%llx) did not reach the panel: %s (0x%x) -- the panel "
             "keeps its frame (%lld such)", n, head, phys, why, v, (long long)k + 1);
}
/* step 1 of a copy: the flip away from the target must have latched
 * (bounded by the cap; after RD3_LATCH_TIMEOUT_RUN timeouts in a row only
 * one look, rdna3_flip_latch.h) */
static void rd3AccelScWait(unsigned head, IOService *fb, enum rd3_sc_target tg, unsigned n, struct rd3_trace_ev *tr)
{
    RD3ScHead &S = gSc[head];
    const enum rd3_sc_wait plan = rd3_sc_wait_plan(tg, S.flipped, S.latched, S.lat.off);
    if (plan == RD3_SCW_NONE) return;
    if (plan == RD3_SCW_LOOK) {
        rd3AccelScLook(head, fb, "a look while the waits were off");
        if (!S.latched) {
            rd3_latch_give_up(&S.lat);
            S.latched = true;   /* not waited for: the copy goes on */
            OSAddAtomic64(1, &gCntScNoWait[head]);
            struct rd3_trace_ev le = rd3TrEv(RD3_TR_LATCH, head);
            le.arg = 6;
            le.v0 = rd3TrSat(rd3AccelUsSince(S.flipAbs));
            rd3Trace(le);
        }
        return;
    }
    const uint64_t cap = rd3_latch_cap_us(rd3AccelHeadFrameUs(fb, head));
    const uint64_t t0 = mach_absolute_time();
    enum rd3_latch_step step = RD3_LATCH_WAIT;
    for (;;) {
        RD3KDisplayStatus st;
        RD3StatusWhy w;
        const bool ok = rd3AccelDispStatus(fb, head, &st, &w);
        if (ok && !S.flipFrameKnown) {   /* a posted flip: its frame is the first one seen */
            S.flipFrame = st.frame;
            S.flipFrameKnown = true;
        }
        step = rd3_latch_step(ok, ok && st.flipPending, S.flipFrame, ok ? st.frame : S.flipFrame,
                              rd3AccelUsSince(S.flipAbs), cap);
        if (step != RD3_LATCH_WAIT) break;
        IOSleep(1);
    }
    const uint64_t us = rd3AccelUsSince(t0);
    S.latched = true;   /* seen, or given up: the copy goes on (bounded) */
    S.seen = step == RD3_LATCH_DONE;   /* only an observation frees D */
    if (step == RD3_LATCH_DONE) rd3AccelScLatchEvent(head, rd3_latch_seen(&S.lat), "the wait before a copy");
    else if (step == RD3_LATCH_TIMEOUT) rd3AccelScLatchEvent(head, rd3_latch_timeout(&S.lat), "");
    else rd3_latch_give_up(&S.lat);
    OSAddAtomic64(1, &gCntScWait[head]);
    OSAddAtomic64((SInt64)us, &gCntScWaitUs[head]);
    tr->flags |= RD3_TRF_WAITED;
    tr->v3 = rd3TrSat(us);
    struct rd3_trace_ev le = rd3TrEv(RD3_TR_LATCH, head);
    le.arg = step == RD3_LATCH_DONE ? 4 : 5;
    le.v0 = rd3TrSat(us);
    rd3Trace(le);
    if (step != RD3_LATCH_DONE) {
        const SInt64 k = OSAddAtomic64(1, &gCntScLatchTo[head]);
        if (rd3_log_due(gScWaitLogs[head]++, 8, 1023))
            ALOG("swap #%u: head %u: the last flip had not latched %llu us after it (%s) -- copied anyway, the "
                 "panel may still read the buffer the copy goes into (%lld such)", n, head,
                 rd3AccelUsSince(S.flipAbs), step == RD3_LATCH_TIMEOUT ? "timed out" : "state unreadable",
                 (long long)k + 1);
    }
}
/* modes 1/2: the back into the scanout buffer the panel does not show,
 * the flip to it, the back into the front (rdna3_swap_copy.h) */
static void rd3AccelSwapCopy(unsigned head, IOService *fb, IOAccelResource2 *back, IOAccelResource2 *frontRes,
                             bool backEmpty, unsigned n, struct rd3_trace_ev *tr, const uint8_t *srcBand)
{
    RD3ScHead &S = gSc[head];
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    const unsigned long long backPhys = rd3res_phys(back);
    RD3CfSide B = {};
    if (!a || !fb || !rd3AccelCfSide(back, &B)) {
        rd3AccelScFail(head, n, RD3_TRX_SRC, "the back is not copyable (no VRAM, mapping or 4-byte geometry)", backPhys,
                       0, tr);
        return;
    }
    struct RD3KVramRequest q;
    bzero(&q, sizeof q);
    q.version = RD3K_VRAM_ABI_VERSION;
    const IOReturn qr = a->askFramebufferForScanout(&q, fb);
    if (qr != kIOReturnSuccess || !q.pitch || !q.width || !q.height || !q.actualSize) {
        rd3AccelScFail(head, n, RD3_TRX_SCANOUT, "the framebuffer would not describe its scanout", backPhys,
                       (unsigned)qr, tr);
        return;
    }
    rd3AccelVramBase(q.phys, q.allocationCookie);
    S.homePhys = q.phys;
    const bool haveD = rd3AccelScEnsureD(head, fb, q.width, q.height, n);
    const enum rd3_sc_target tg = rd3_sc_target((enum rd3_sc_shows)S.shows, haveD);
    /* 1. the flip away from the target must have latched */
    rd3AccelScWait(head, fb, tg, n, tr);
    /* 2. the copy into the target */
    unsigned long long tCookie, tBytes, tPhys;
    unsigned tW, tH, tPitch;
    const volatile void *tKva;
    if (tg == RD3_SCT_D) {
        tCookie = S.dCookie; tBytes = S.dBytes; tPhys = S.dPhys; tW = S.dW; tH = S.dH; tPitch = S.dPitch; tKva = S.dKva;
    } else {
        tCookie = q.allocationCookie; tBytes = q.actualSize; tPhys = q.phys; tW = q.width; tH = q.height;
        tPitch = q.pitch; tKva = q.kva;
    }
    tr->v1 |= (unsigned)tg << 16;
    unsigned rowb = 0, rows = 0;
    if (!rd3_sc_extent(B.w, B.h, B.pitch, B.bytes, tW, tH, tPitch, tBytes, &rowb, &rows)) {
        rd3AccelScFail(head, n, RD3_TRX_SRC, "the back and the scanout have no common extent", backPhys, 0, tr);
        return;
    }
    unsigned long long us = 0;
    int cst = 0;
    const IOReturn cr = rd3AccelScSdma(fb, B.cookie, B.pitch, tCookie, tPitch, rowb, rows, &us, &cst);
    rd3AccelScSdmaNote(cr, cst, n, head);
    if (cr != kIOReturnSuccess) {
        tr->v2 = (tr->v2 & 0xffff0000u) | (us > 0xffffull ? 0xffffu : (uint32_t)us);   /* v2 >> 16: the generation */
        rd3AccelScFail(head, n, RD3_TRX_SDMA,
                       cst == kRd3Etime ? "rdna3CopyVram timed out (the front copy skipped too)"
                                        : "rdna3CopyVram refused the copy", backPhys, (unsigned)cr, tr);
        return;
    }
    tr->v2 = (tr->v2 & 0xffff0000u) | (us > 0xffffull ? 0xffffu : (uint32_t)us);
    OSAddAtomic64(1, &gCntScCopy[head]);
    OSAddAtomic64((SInt64)rowb * rows, &gCntScBytes[head]);
    OSAddAtomic64((SInt64)us, &gCntScUs[head]);
    if ((SInt64)us > gCntScUsMax[head]) gCntScUsMax[head] = (SInt64)us;   /* one writer: the head's swaps */
    if (tg == RD3_SCT_HOME_DIRECT) OSAddAtomic64(1, &gCntScDirect[head]);
    /* 3. the flip to it */
    struct RD3KFlipRequest f;
    bzero(&f, sizeof f);
    f.version = RD3K_FLIP_ABI_VERSION;
    if (tg == RD3_SCT_D) {
        f.kapiMemory = S.dHandle; f.allocationCookie = S.dCookie; f.width = S.dW; f.height = S.dH; f.pitch = S.dPitch;
    } else {
        f.flags = 1u;   /* the framebuffer's own scanout (home) */
    }
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_FLIP_FN);
    const IOReturn fr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    {
        struct rd3_trace_ev fe = rd3TrEv(RD3_TR_FLIP, head);
        fe.arg = tg == RD3_SCT_D ? 1 : 2;
        fe.flags = fr == kIOReturnSuccess ? 1 : 0;
        fe.v0 = (uint32_t)f.flipResult;
        fe.v4 = fr;
        fe.pa = tPhys;
        rd3Trace(fe);
    }
    if (fr != kIOReturnSuccess) {
        rd3AccelScFail(head, n, RD3_TRX_FLIP, "the framebuffer refused the flip", backPhys, (unsigned)fr, tr);
    } else {
        RD3KDisplayStatus st;
        RD3StatusWhy w;
        S.shows = rd3_sc_after_flip((enum rd3_sc_shows)S.shows, tg, 1);
        S.flipped = true;
        S.flipAbs = S.ownFlipAbs = mach_absolute_time();
        const bool ok = rd3AccelDispStatus(fb, head, &st, &w);
        S.flipFrame = ok ? st.frame : 0;
        S.flipFrameKnown = ok;
        S.latched = S.seen = ok && !st.flipPending;
        rd3_latch_flipped(&S.lat);
        if (S.latched) rd3AccelScLatchEvent(head, rd3_latch_seen(&S.lat), "right after the flip");
        S.scanoutFrame = true;
        S.lastPixels = !backEmpty;
        if (srcBand) memcpy(S.lastBand, srcBand, sizeof S.lastBand);
        else bzero(S.lastBand, sizeof S.lastBand);
        tr->arg = RD3_TRS_COPY;
        tr->pc = tPhys;
        if (rd3_sc_tear_free(tg)) tr->flags |= RD3_TRF_DOUBLE;
    }
    /* the scanout buffer's signature after the copy (Q6) */
    if (tKva) {
        RD3Scan sc;
        bzero(&sc, sizeof sc);
        rd3AccelPxScan(tKva, tBytes, tW, tH, tPitch, &sc);
        rd3AccelTraceScan(head, nullptr, 0xfe, tPhys, sc, 64u, n);
    }
    /* 4. the back into the front: copy forward's source, as Apple's family expects */
    if (!frontRes) return;
    if (frontRes == back) {
        /* the copied buffer is the front itself (a landed a1, card run
         * 2026-10-17c): it holds the frame on the panel, nothing to copy */
        if (B.vm) {
            rd3_sc_front_set(&S.front, B.vm, B.cookie);
            tr->flags |= RD3_TRF_FRONT_OK;
        }
        return;
    }
    RD3CfSide F = {};
    const char *fwhy = nullptr;
    IOReturn fcr = kIOReturnSuccess;
    if (!rd3AccelCfSide(frontRes, &F)) fwhy = "the front is not copyable";
    else if (F.vm && F.vm->fScanout) fwhy = "the front is the framebuffer's scanout itself (it would tear)";
    else if (!rd3_sc_extent(B.w, B.h, B.pitch, B.bytes, F.w, F.h, F.pitch, F.bytes, &rowb, &rows))
        fwhy = "the back and the front have no common extent";
    else if (gScBrk.off && !gScBrk.probing)
        fwhy = "the SDMA breaker opened at the copy before";
    else if ((fcr = rd3AccelScSdma(fb, B.cookie, B.pitch, F.cookie, F.pitch, rowb, rows, &us, &cst)) != kIOReturnSuccess) {
        rd3AccelScSdmaNote(fcr, cst, n, head);
        fwhy = cst == kRd3Etime ? "rdna3CopyVram timed out" : "rdna3CopyVram refused the copy";
    } else {
        rd3AccelScSdmaNote(fcr, cst, n, head);
    }
    if (fwhy) {
        rd3_sc_front_set(&S.front, nullptr, 0);
        const SInt64 k = OSAddAtomic64(1, &gCntScFrontFail[head]);
        if (rd3_log_due(gScFrontLogs[head]++, 8, 1023))
            ALOG("swap #%u: head %u: the back was not copied into the front (phys 0x%llx): %s (0x%x) -- copy forward "
                 "reads the panel's buffer instead (%lld such)", n, head, rd3res_phys(frontRes), fwhy, (unsigned)fcr,
                 (long long)k + 1);
        return;
    }
    rd3_sc_front_set(&S.front, F.vm, F.cookie);
    tr->flags |= RD3_TRF_FRONT_OK;
    OSAddAtomic64(1, &gCntScFront[head]);
    if (F.vm && F.vm->fKva) {
        RD3Scan sc;
        rd3AccelResScan(frontRes, &sc);
        rd3AccelTraceScan(head, nullptr, 0xfd, F.vm->fPhys, sc, 128u, n, 0, frontRes);
    }
}
/* leaving modes 1/2 (under the family's lock): a head showing D gets its
 * last frame copied into home and flips there, so mode 0 starts from the
 * framebuffer's own scanout and D is off the panel (ECC review round 1) */
static void rd3AccelScLeave(unsigned head, IOService *fb)
{
    RD3ScHead &S = gSc[head];
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a || !fb || S.shows != RD3_SCS_D || !S.dHandle) return;
    struct RD3KVramRequest q;
    bzero(&q, sizeof q);
    q.version = RD3K_VRAM_ABI_VERSION;
    const IOReturn qr = a->askFramebufferForScanout(&q, fb);
    unsigned rowb = 0, rows = 0;
    unsigned long long us = 0;
    int cst = 0;
    const char *why = nullptr;
    IOReturn r = qr;
    /* the flip to D must have latched: until then the HUBP may still read
     * home, which the copy writes (ECC review round 2); bounded by the cap,
     * a timeout is logged and counted by rd3AccelScWait */
    struct rd3_trace_ev wt = rd3TrEv(RD3_TR_SWAP, head);
    rd3AccelScWait(head, fb, RD3_SCT_HOME, gSwapCount, &wt);
    if (qr != kIOReturnSuccess || !q.pitch || !q.actualSize) why = "the framebuffer would not describe its scanout";
    else if (!rd3_sc_extent(S.dW, S.dH, S.dPitch, S.dBytes, q.width, q.height, q.pitch, q.actualSize, &rowb, &rows))
        why = "D and home have no common extent";
    else if (gScBrk.off && !gScBrk.probing)
        why = "the SDMA breaker is open";
    else {
        r = rd3AccelScSdma(fb, S.dCookie, S.dPitch, q.allocationCookie, q.pitch, rowb, rows, &us, &cst);
        rd3AccelScSdmaNote(r, cst, gSwapCount, head);   /* the breaker hears this copy too */
        if (r != kIOReturnSuccess) why = cst == kRd3Etime ? "rdna3CopyVram timed out" : "rdna3CopyVram refused the copy";
    }
    if (!why) {
        struct RD3KFlipRequest f;
        bzero(&f, sizeof f);
        f.version = RD3K_FLIP_ABI_VERSION;
        f.flags = 1u;
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(RD3K_FLIP_FN);
        r = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
        if (sym) sym->release();
        struct rd3_trace_ev fe = rd3TrEv(RD3_TR_FLIP, head);
        fe.arg = 4;   /* home, leaving modes 1/2 */
        fe.flags = r == kIOReturnSuccess ? 1 : 0;
        fe.v0 = (uint32_t)f.flipResult;
        fe.v4 = r;
        fe.pa = q.phys;
        rd3Trace(fe);
        if (r != kIOReturnSuccess) why = "the framebuffer refused the flip home";
    }
    if (why) {
        ALOG("swap mode: head %u: leaving the copy modes, the panel stays on the second scanout buffer (phys 0x%llx): "
             "%s (0x%x, status %d) -- mode 0's first flip moves it", head, S.dPhys, why, (unsigned)r, cst);
        return;
    }
    RD3KDisplayStatus st;
    RD3StatusWhy w;
    S.shows = RD3_SCS_HOME;
    S.flipped = true;
    S.flipAbs = S.ownFlipAbs = mach_absolute_time();
    const bool ok = rd3AccelDispStatus(fb, head, &st, &w);
    S.flipFrame = ok ? st.frame : 0;
    S.flipFrameKnown = ok;
    S.latched = S.seen = ok && !st.flipPending;
    rd3_latch_flipped(&S.lat);
    S.scanoutFrame = false;
    ALOG("swap mode: head %u: leaving the copy modes -- the last frame copied into home (phys 0x%llx, %llu us) and "
         "flipped there%s", head, q.phys, us, S.latched ? "" : " (latches at the next vblank)");
}

/* ---- the flip-present hooks (card run 2026-10-17d) ----
 * In bursts copy forward rotated a display surface's three buffers at
 * ~60 Hz with no submitSwapCopy (17d: 688.51-688.72 s, 803.1-806.6 s):
 * the family presents those frames by another path. Its candidates are
 * IOAccelLegacyDisplayPipe::submitFlipBuffer / submitScanoutFlipBuffer
 * (accel/iofam/IOAccelLegacyDisplayPipe.h) and the surface's
 * pickPresentType / didSubmitSwap / isBackBufferReady / submitUpdate
 * (IOAccelLegacySurface.h), none of which the driver overrode. Each is now
 * overridden by a naked wrapper (RD3_HOOK_ASM below) that
 *   1. saves the argument registers and calls rd3HookPre(kind, this, ...)
 *      (the flip-buffer hooks: counted, traced, the first 16 calls logged,
 *      and with debug.rdna3accel_flipbuf_present=1 the buffer presented),
 *   2. ALWAYS calls the base class's implementation with the same
 *      registers, through rd3Base<Method>: a C++ call qualified with the
 *      base class, which -fapple-kext compiles into a jump through the
 *      base's exported vtable at the slot the compiler takes from the
 *      reconstructed headers - the technique of tahoe_fwd.h's forwarders,
 *      without a slot number written by hand (checked against the compiled
 *      vtables: RD3Surface didSubmitSwap 2640, isBackBufferReady 2648,
 *      submitUpdate 2664 - tahoe_fwd.h's measured slot -, pickPresentType
 *      2672; RD3DisplayPipe submitScanoutFlipBuffer 2504, submitFlipBuffer
 *      2512, after setStereo 2480, tahoe_fwd.h's measured slot),
 *   3. calls rd3HookPost(kind, this, the first three arguments, rax) (the
 *      surface hooks: counted, traced with the base's return value, the
 *      first 16 logged),
 *   4. returns the base's rax:rdx untouched, so a return type the headers
 *      only know as void reaches the family whole.
 * Nothing but an atomic count happens at interrupt level or with interrupts
 * disabled (the trace's lock, the family's lock and the log are never taken
 * there); the first such call of each kind is logged by the next one in
 * thread context (ECC review round 1). */
enum { kHkFlipBuf = 0, kHkScanoutFlipBuf = 1, kHkPickPresent = 2, kHkDidSubmitSwap = 3, kHkBackReady = 4,
       kHkSubmitUpdate = 5, kHkKinds = 6 };
static const char *const kHkName[kHkKinds] = { "IOAccelLegacyDisplayPipe::submitFlipBuffer",
                                                "IOAccelLegacyDisplayPipe::submitScanoutFlipBuffer",
                                                "IOAccelLegacySurface::pickPresentType",
                                                "IOAccelLegacySurface::didSubmitSwap",
                                                "IOAccelLegacySurface::isBackBufferReady",
                                                "IOAccelLegacySurface::submitUpdate" };
static SInt64 gCntFlipbuf[kRd3MaxFB], gCntFlipbufScanout[kRd3MaxFB], gCntFlipbufPresented[kRd3MaxFB];
static SInt64 gCntFlipbufHeld[kRd3MaxFB], gCntFlipbufFailed[kRd3MaxFB];
static SInt64 gCntFlipbufBusy[kRd3MaxFB], gCntFlipbufUnlocked[kRd3MaxFB], gCntFlipbufEmptyHeld[kRd3MaxFB];
static SInt64 gCntFlipbufNoHead, gCntFlipbufNoRes, gCntFlipbufIntOff, gCntFlipbufBadPtr, gCntHookIntr, gCntHookIntOff;
static SInt64 gCntHook[kHkKinds], gCntHookBackNo, gCntHookIntrK[kHkKinds];
static volatile SInt32 gHookLogs[kHkKinds];
static unsigned gFlipbufFailLogs[kRd3MaxFB], gFlipbufBusyLogs, gFlipbufEmptyLogs[kRd3MaxFB], gFlipbufNoResLogs;
/* bit k: kind k called at interrupt level, bit 8 + k: with interrupts disabled (set there, logged by the next
 * call in thread context, once per bit) */
static volatile UInt32 gHookOddPending, gHookOddLogged;
extern "C" boolean_t ml_at_interrupt_context(void);
extern "C" boolean_t ml_get_interrupts_enabled(void);
/* the lowest x86_64 kernel / kext address (VM_MIN_KERNEL_AND_KEXT_ADDRESS, osfmk/mach/i386/vm_param.h, not in
 * the public headers): a "pointer" argument below it is no kernel object - never handed to OSDynamicCast */
static const uint64_t kRd3MinKernelVA = 0xffffff8000000000ull;

/* the buffer of a flip-buffer present, as the swap mode presents a back
 * (under the family's lock) */
static unsigned rd3AccelFlipbufPresentLocked(unsigned head, IOService *fb, RD3DisplayPipe *pipe, unsigned kind,
                                             IOAccelResource2 *res, IOAccelResource2 *other)
{
    const unsigned long long phys = rd3res_phys(res);
    rd3AccelScApplyAll();   /* the panel moves posted since the last holder of the lock */
    int mode = gSwapMode;
    bool brkFlip = false;
    const unsigned n = ++gSwapCount;
    if (mode != RD3_SWAPMODE_FLIP) {
        enum rd3_sc_brk_ev bev;
        if (!rd3_sc_brk_allow(&gScBrk, rd3AccelUsNow(), &bev)) {
            mode = RD3_SWAPMODE_FLIP;   /* the SDMA breaker is open */
            brkFlip = true;
            OSAddAtomic64(1, &gCntScBrkFlip[head]);
        }
        rd3AccelScBrkEvent(bev, n, head, 0, kIOReturnSuccess);
    }
    RD3Scan sc;
    rd3AccelResScan(res, &sc);
    struct rd3_trace_ev tr = rd3TrEv(RD3_TR_SWAP, head);
    tr.obj = rd3TrObj(pipe);
    tr.v0 = n;
    tr.v1 = (sc.rc & 0xfu) | ((unsigned)mode << 12) | (brkFlip ? 1u << 20 : 0u) | RD3_TRV1_FLIPBUF;
    tr.v4 = 0xffffu | (kind << 24);   /* in no surface's array; the hook's kind */
    tr.pa = phys;
    tr.pb = rd3res_phys(other);
    tr.flags = (uint8_t)(sc.rc == kRcPixels ? RD3_TRF_A1_PIXELS : 0);
    memcpy(tr.ba, sc.band, 8);
    if (mode == RD3_SWAPMODE_FLIP && gSwapAsync && gPresReady) {
        /* DRIVER-PLAN N38: through the head's present queue - the family named this buffer to be shown, so no
         * write-generation or empty hold; programmed now, or by the flip-done worker behind the flip in
         * flight (newest wins); the family's own submitFlipBuffer still runs after this hook */
        rd3AccelTracePanel(head, fb, gLatch[head].phys, nullptr);
        tr.arg = RD3_TRS_FLIP;
        tr.pc = phys;
        /* the buffer on the panel: decided inside the queue, after its latch look and only with nothing in
         * flight or queued (rd3_fq_present; ECC review of N38) */
        RD3FlipWhy fw = { nullptr, kIOReturnSuccess, 0 };
        const uint64_t cap = rd3_latch_cap_us(rd3AccelHeadFrameUs(fb, head));
        const unsigned pq = rd3AccelPresentLegacy(head, fb, res, kPqFlipbuf, n, nullptr, cap, &fw,
                                                  gSwapSameNoFlip != 0);
        if (pq == kPqOutSame) {
            tr.v1 |= RD3_TRV1_TARGET_SAME << 16;
            OSAddAtomic64(1, &gCntLatchSame[head]);
        } else {
            if (pq == kPqOutRefused) {
                tr.arg = RD3_TRS_FLIP_FAILED;
                rd3Trace(tr);
                const SInt64 k = OSAddAtomic64(1, &gCntFlipbufFailed[head]);
                if (rd3_log_due(gFlipbufFailLogs[head]++, 8, 1023))
                    ALOG("flipbuf #%u: head %u: %s's buffer (phys 0x%llx) NOT presented: %s (%lld such)", n, head,
                         kHkName[kind], phys, fw.why ? fw.why : "the flip was refused", (long long)k + 1);
                return RD3_TRFB_FAILED;
            }
            if (pq != kPqOutShown) tr.v1 |= RD3_TRV1_TARGET_QUEUED << 16;
        }
        rd3AccelWgPresented(head, phys, rd3AccelWgGen(phys), sc.rc >= kRcEmpty, sc.sig);
        rd3AccelNotePresented(rd3res_vidmem(res), head);
        rd3Trace(tr);
        OSAddAtomic64(1, &gCntFlipbufPresented[head]);
        return RD3_TRFB_PRESENTED;
    }
    /* the write generations: only a buffer written after the head's frame */
    unsigned long long wgGen = 0, wgHead = 0;
    const enum rd3_wg_why wg = rd3AccelWgDecide(head, phys, sc.rc >= kRcEmpty, sc.sig, &wgGen, &wgHead);
    const unsigned long long expect = mode != RD3_SWAPMODE_FLIP ? rd3AccelScExpect(head) : gLatch[head].phys;
    if (rd3AccelWgNote(head, wg, gSwapGen && mode != RD3_SWAPMODE_COPY_ALL, n, phys, wgGen, wgHead,
                       "the flip buffer", 0, 0, &tr)) {
        tr.pc = expect;
        rd3Trace(tr);
        OSAddAtomic64(1, &gCntFlipbufHeld[head]);
        return RD3_TRFB_HELD;
    }
    /* an empty buffer: held as mode 1 holds an empty back (rd3_sc_action) and mode 0 skips one */
    if (sc.rc == kRcEmpty && mode != RD3_SWAPMODE_COPY_ALL) {
        tr.arg = mode == RD3_SWAPMODE_FLIP ? RD3_TRS_SKIP : RD3_TRS_HOLD;
        tr.flags |= RD3_TRF_BACK_EMPTY;
        tr.pc = expect;
        rd3Trace(tr);
        OSAddAtomic64(1, &gCntFlipbufHeld[head]);
        const SInt64 k = OSAddAtomic64(1, &gCntFlipbufEmptyHeld[head]);
        if (rd3_log_due(gFlipbufEmptyLogs[head]++, 8, 1023))
            ALOG("flipbuf #%u: head %u: %s's buffer (phys 0x%llx) read empty -- held, the panel keeps its frame "
                 "(swap mode %d; %lld such; debug.rdna3accel_flipbuf_emptyheld_h%u)", n, head, kHkName[kind], phys,
                 mode, (long long)k + 1, head);
        return RD3_TRFB_HELD;
    }
    const char *fwhy = nullptr;
    rd3AccelTracePanel(head, fb, expect);
    if (mode != RD3_SWAPMODE_FLIP) {
        /* modes 1/2: copied into D / home (the latch wait first) and flipped there; no front copy (this path
         * names no front): copy forward then reads the panel's buffer, which holds this frame */
        rd3AccelSwapCopy(head, fb, res, nullptr, sc.rc == kRcEmpty, n, &tr, sc.band);
        rd3_sc_front_set(&gSc[head].front, nullptr, 0);
        if (tr.arg != RD3_TRS_COPY) fwhy = "the copy did not reach the panel (the swap copy's own line says why)";
    } else {
        /* mode 0: the flip and its latch wait (debug.rdna3accel_swap_latch) */
        const uint64_t cap = rd3_latch_cap_us(rd3AccelHeadFrameUs(fb, head));
        rd3AccelLatchEntry(head, fb, n);
        RD3FlipWhy fw;
        const uint64_t issued = mach_absolute_time();
        const bool flipped = rd3AccelFlipTo(res, fb, head, &fw);
        struct rd3_trace_ev fe = rd3TrEv(RD3_TR_FLIP, head);
        fe.arg = 5;   /* a flip-buffer present */
        fe.flags = flipped ? 1 : 0;
        fe.v0 = (uint32_t)fw.flipResult;
        fe.v4 = fw.ret;
        fe.pa = phys;
        rd3Trace(fe);
        if (flipped) {
            rd3AccelScPanelMoved(head, "a flip-buffer present", issued, false);
            rd3AccelLatchFlipped(head, fb, res, phys, nullptr, "a flip-buffer present (not hashed)", cap, n);
            rd3AccelNotePresented(rd3res_vidmem(res), head);
            tr.arg = RD3_TRS_FLIP;
            tr.pc = phys;
            tr.v3 = gLatch[head].latchUs;
        } else {
            tr.arg = RD3_TRS_FLIP_FAILED;
            fwhy = fw.why ? fw.why : "the flip was refused";
        }
    }
    /* the generation captured at the decision (ECC review round 1) */
    if (!fwhy)
        rd3AccelWgPresented(head, phys, wg != RD3_WGW_OFF ? wgGen : rd3AccelWgGen(phys), sc.rc >= kRcEmpty, sc.sig);
    rd3Trace(tr);
    if (fwhy) {
        const SInt64 k = OSAddAtomic64(1, &gCntFlipbufFailed[head]);
        if (rd3_log_due(gFlipbufFailLogs[head]++, 8, 1023))
            ALOG("flipbuf #%u: head %u: %s's buffer (phys 0x%llx) NOT presented: %s (%lld such)", n, head,
                 kHkName[kind], phys, fwhy, (long long)k + 1);
        return RD3_TRFB_FAILED;
    }
    OSAddAtomic64(1, &gCntFlipbufPresented[head]);
    return RD3_TRFB_PRESENTED;
}
/* debug.rdna3accel_flipbuf_present=1: which resource, the lock, the present */
static unsigned rd3AccelFlipbufPresent(unsigned head, IOService *fb, RD3DisplayPipe *pipe, unsigned kind,
                                       IOAccelResource2 *r1, IOAccelResource2 *r2, uint8_t *flags)
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    if (!a || !fb || head >= kRd3MaxFB) return RD3_TRFB_NOHEAD;
    bool fb1 = false, fb2 = false;
    if (gHeadFbResLock) {
        IOSimpleLockLock(gHeadFbResLock);
        fb1 = r1 && gHeadFbRes[head] == r1;
        fb2 = r2 && gHeadFbRes[head] == r2;
        IOSimpleLockUnlock(gHeadFbResLock);
    }
    const unsigned long long p1 = rd3res_phys(r1), p2 = rd3res_phys(r2);
    /* the head's framebuffer resource, or no VRAM of ours: never the one presented */
    const int pick = rd3_wg_flip_pick(fb1 || !p1, fb2 || !p2, rd3AccelWgGen(p1), rd3AccelWgGen(p2));
    IOAccelResource2 *res = pick == 1 ? r1 : r2, *other = pick == 1 ? r2 : r1;
    if (!res || !rd3res_phys(res)) {
        const SInt64 k = OSAddAtomic64(1, &gCntFlipbufNoRes);
        if (rd3_log_due(gFlipbufNoResLogs++, 8, 1023))
            ALOG("flipbuf: head %u: %s NOT presented: neither buffer is a VRAM resource of ours other than the head's "
                 "framebuffer (res %p phys 0x%llx, res %p phys 0x%llx) -- only traced (%lld such; "
                 "debug.rdna3accel_flipbuf_nores)", head, kHkName[kind], r1, p1, r2, p2, (long long)k + 1);
        return RD3_TRFB_NORES;
    }
    bool took = false;
    if (gFamOwner != (void *)current_thread()) {
        if (!rd3FamLock(a, true)) {
            const SInt64 k = OSAddAtomic64(1, &gCntFlipbufBusy[head]);
            if (rd3_log_due(gFlipbufBusyLogs++, 8, 1023))
                ALOG("flipbuf: head %u: %s's buffer NOT presented: the family's lock is held by another thread (or "
                     "by a lock path that does not call acceleratorDidLock) -- only traced (%lld such)", head,
                     kHkName[kind], (long long)k + 1);
            return RD3_TRFB_BUSY;
        }
        took = true;
        *flags |= 2;
        OSAddAtomic64(1, &gCntFlipbufUnlocked[head]);
    }
    const unsigned r = rd3AccelFlipbufPresentLocked(head, fb, pipe, kind, res, other);
    if (took) rd3FamUnlock(a);
    return r;
}
static const char *rd3AccelFlipbufWhat(unsigned what)
{
    switch (what) {
    case RD3_TRFB_TRACED:    return "traced";
    case RD3_TRFB_PRESENTED: return "PRESENTED";
    case RD3_TRFB_HELD:      return "held (not written since the head's frame, or empty)";
    case RD3_TRFB_FAILED:    return "present FAILED";
    case RD3_TRFB_NOHEAD:    return "no head";
    case RD3_TRFB_NORES:     return "no VRAM resource of ours";
    default:                 return "the family's lock busy";
    }
}
/* submitFlipBuffer / submitScanoutFlipBuffer, before the family's own */
static void rd3AccelFlipbufHook(unsigned kind, RD3DisplayPipe *pipe, const void *ev, unsigned idx, IOAccelResource2 *r1,
                                IOAccelResource2 *r2)
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    unsigned head = kRd3MaxFB;
    IOService *fb = a && pipe ? a->pipeFramebuffer(pipe, &head) : nullptr;
    if (!fb) head = kRd3MaxFB;
    const SInt64 k = head < kRd3MaxFB
                         ? OSAddAtomic64(1, kind == kHkFlipBuf ? &gCntFlipbuf[head] : &gCntFlipbufScanout[head])
                         : OSAddAtomic64(1, &gCntFlipbufNoHead);
    OSAddAtomic64(1, &gCntHook[kind]);
    /* the mangled names say IOAccelResource2 *: the cast checks the class, after a check that the value is a
     * kernel address at all (a register the reconstructed signature got wrong, ECC review round 1) */
    const bool bad1 = r1 && (uint64_t)r1 < kRd3MinKernelVA, bad2 = r2 && (uint64_t)r2 < kRd3MinKernelVA;
    if (bad1 || bad2) OSAddAtomic64(1, &gCntFlipbufBadPtr);
    IOAccelResource2 *c1 = r1 && !bad1 ? OSDynamicCast(IOAccelResource2, (OSObject *)r1) : nullptr;
    IOAccelResource2 *c2 = r2 && !bad2 ? OSDynamicCast(IOAccelResource2, (OSObject *)r2) : nullptr;
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_FLIPBUF, head);
    e.arg = (uint8_t)kind;
    e.obj = rd3TrObj(pipe);
    e.v0 = idx;
    e.v1 = rd3TrSat((uint64_t)k + 1);
    e.v2 = rd3TrObj(r1);
    e.v3 = rd3TrObj(r2);
    e.pa = rd3res_phys(c1);
    e.pb = rd3res_phys(c2);
    unsigned what = head < kRd3MaxFB ? (unsigned)RD3_TRFB_TRACED : (unsigned)RD3_TRFB_NOHEAD;
    if (gFlipbufPresent && head < kRd3MaxFB) {
        e.flags |= 1;
        what = rd3AccelFlipbufPresent(head, fb, pipe, kind, c1, c2, &e.flags);
    }
    e.v4 = what;
    e.pc = head < kRd3MaxFB ? (gSwapMode != RD3_SWAPMODE_FLIP ? rd3AccelScExpect(head) : gLatch[head].phys) : 0;
    rd3Trace(e);
    if (OSIncrementAtomic(&gHookLogs[kind]) < 16)
        ALOG("hook: %s(event %p, %u, res %p (phys 0x%llx%s), res %p (phys 0x%llx%s)) on head %d (pipe %p; call %lld "
             "of this kind on the head; family lock %s; interrupts %s; flipbuf_present %d: %s) -- the family's own "
             "runs next", kHkName[kind], ev, idx, r1, e.pa,
             bad1 ? ", NOT a kernel address" : r1 && !c1 ? ", NOT an IOAccelResource2" : "", r2, e.pb,
             bad2 ? ", NOT a kernel address" : r2 && !c2 ? ", NOT an IOAccelResource2" : "",
             head < kRd3MaxFB ? (int)head : -1, pipe, (long long)k + 1,
             gFamOwner == (void *)current_thread() ? "held by this thread" : "not this thread's",
             ml_get_interrupts_enabled() ? "enabled" : "DISABLED", gFlipbufPresent, rd3AccelFlipbufWhat(what));
}
/* the surface's hooks, after the family's own: its return value */
static void rd3AccelSurfHookPost(unsigned kind, const void *surf, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t ret)
{
    const SInt64 k = OSAddAtomic64(1, &gCntHook[kind]);
    if (kind == kHkBackReady && !(ret & 0xff)) OSAddAtomic64(1, &gCntHookBackNo);
    const unsigned head = rd3AccelSurfKnownHead(surf);
    if (k < 2048 || (k & 7) == 0) {
        struct rd3_trace_ev e = rd3TrEv(RD3_TR_HOOK, head);
        e.arg = (uint8_t)(kind - kHkPickPresent);
        e.obj = rd3TrObj(surf);
        e.v0 = (uint32_t)a0;
        e.v1 = kind == kHkSubmitUpdate ? (uint32_t)a2 : (uint32_t)a1;
        e.v2 = kind == kHkBackReady ? (uint32_t)(ret & 0xff) : (uint32_t)ret;
        e.v3 = rd3TrSat((uint64_t)k + 1);
        rd3Trace(e);
    }
    if (OSIncrementAtomic(&gHookLogs[kind]) >= 16) return;
    const int h = head < kRd3MaxFB ? (int)head : -1;
    const char *ints = ml_get_interrupts_enabled() ? "enabled" : "DISABLED";
    if (kind == kHkSubmitUpdate)
        ALOG("hook: %s(%u, bounds %p, %u) -> 0x%llx (surface %p, head %d; call %lld; interrupts %s)", kHkName[kind],
             (unsigned)a0, (const void *)a1, (unsigned)a2, (unsigned long long)ret, surf, h, (long long)k + 1, ints);
    else if (kind == kHkDidSubmitSwap)
        ALOG("hook: %s(%u, %u) -> 0x%llx (surface %p, head %d; call %lld; interrupts %s)", kHkName[kind],
             (unsigned)a0, (unsigned)a1, (unsigned long long)ret, surf, h, (long long)k + 1, ints);
    else
        ALOG("hook: %s(%u) -> 0x%llx%s (surface %p, head %d; call %lld; interrupts %s)", kHkName[kind], (unsigned)a0,
             (unsigned long long)ret, kind == kHkBackReady ? ((ret & 0xff) ? " (ready)" : " (NOT ready)") : "", surf,
             h, (long long)k + 1, ints);
}
/* the naked wrappers' C halves (asm names _rd3HookPre / _rd3HookPost) */
/* the first call of each kind at interrupt level / with interrupts disabled, logged from thread context */
static void rd3AccelHookOddReport(void)
{
    const UInt32 m = gHookOddPending & ~gHookOddLogged;
    if (!m) return;
    const UInt32 was = OSBitOrAtomic(m, &gHookOddLogged);
    for (unsigned b = 0; b < 16; b++) {
        if (!(m & (1u << b)) || (was & (1u << b))) continue;
        const unsigned kind = b & 7;
        if (kind >= kHkKinds) continue;
        if (b < 8)
            ALOG("hook: %s was called at INTERRUPT LEVEL (%lld such of this kind, %lld of all kinds) -- only counted "
                 "and forwarded to the family; nothing it holds is traced or presented there "
                 "(debug.rdna3accel_hook_intr)", kHkName[kind], (long long)gCntHookIntrK[kind], (long long)gCntHookIntr);
        else
            ALOG("hook: %s was called with INTERRUPTS DISABLED (%lld such of the %s kinds) -- only counted and "
                 "forwarded to the family; no lock, trace or present taken there (debug.rdna3accel_%s)",
                 kHkName[kind], kind <= kHkScanoutFlipBuf ? (long long)gCntFlipbufIntOff : (long long)gCntHookIntOff,
                 kind <= kHkScanoutFlipBuf ? "flip-buffer" : "surface",
                 kind <= kHkScanoutFlipBuf ? "flipbuf_intoff" : "hook_intoff");
    }
}
extern "C" __attribute__((used)) void rd3HookPre(unsigned kind, void *self, uint64_t a0, uint64_t a1, uint64_t a2,
                                                 uint64_t a3)
{
    if (kind >= kHkKinds) return;
    /* counted per kind before anything else (ECC review round 1); neither path takes a lock or logs */
    if (ml_at_interrupt_context()) {
        OSAddAtomic64(1, &gCntHookIntr);
        OSAddAtomic64(1, &gCntHookIntrK[kind]);
        OSBitOrAtomic(1u << kind, &gHookOddPending);
        return;
    }
    if (!ml_get_interrupts_enabled()) {
        OSAddAtomic64(1, kind <= kHkScanoutFlipBuf ? &gCntFlipbufIntOff : &gCntHookIntOff);
        OSBitOrAtomic(1u << (8 + kind), &gHookOddPending);
        return;
    }
    rd3AccelHookOddReport();
    if (kind == kHkFlipBuf || kind == kHkScanoutFlipBuf)
        rd3AccelFlipbufHook(kind, (RD3DisplayPipe *)self, (const void *)a0, (unsigned)a1, (IOAccelResource2 *)a2,
                            (IOAccelResource2 *)a3);
}
extern "C" __attribute__((used)) void rd3HookPost(unsigned kind, void *self, uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t ret)
{
    /* counted by rd3HookPre (the same call) */
    if (kind < kHkPickPresent || kind >= kHkKinds || ml_at_interrupt_context() || !ml_get_interrupts_enabled()) return;
    rd3AccelSurfHookPost(kind, self, a0, a1, a2, ret);
}
/* the base calls: qualified, so -fapple-kext jumps through the base's
 * vtable (no symbol of the base needed: submitUpdate is not exported on
 * macOS 26); void, the result is the base's rax as it leaves */
extern "C" __attribute__((used)) void rd3BaseSubmitFlipBuffer(RD3DisplayPipe *p, IOAccelEvent *a0, unsigned a1,
                                                              IOAccelResource2 *a2, IOAccelResource2 *a3)
{
    p->IOAccelLegacyDisplayPipe::submitFlipBuffer(a0, a1, a2, a3);
}
extern "C" __attribute__((used)) void rd3BaseSubmitScanoutFlipBuffer(RD3DisplayPipe *p, IOAccelEvent *a0, unsigned a1,
                                                                     IOAccelResource2 *a2, IOAccelResource2 *a3)
{
    p->IOAccelLegacyDisplayPipe::submitScanoutFlipBuffer(a0, a1, a2, a3);
}
extern "C" __attribute__((used)) void rd3BasePickPresentType(RD3Surface *s, unsigned a0)
{
    s->IOAccelLegacySurface::pickPresentType(a0);
}
extern "C" __attribute__((used)) void rd3BaseDidSubmitSwap(RD3Surface *s, unsigned a0, unsigned a1)
{
    s->IOAccelLegacySurface::didSubmitSwap(a0, a1);
}
extern "C" __attribute__((used)) void rd3BaseIsBackBufferReady(RD3Surface *s, unsigned a0)
{
    (void)s->IOAccelLegacySurface::isBackBufferReady(a0);
}
extern "C" __attribute__((used)) void rd3BaseSubmitUpdate(RD3Surface *s, unsigned a0, IOAccelBounds *a1, unsigned a2)
{
    s->IOAccelLegacySurface::submitUpdate(a0, a1, a2);
}
/* the wrapper: an rbp frame, the six argument registers saved at -8..-48
 * (rsp 16-aligned at every call), rd3HookPre(kind, this, a0..a3), the base
 * with the original registers, rax / rdx saved, rd3HookPost(kind, this,
 * a0, a1, a2, rax), rax / rdx back, return */
#define RD3_HOOK_ASM(kind, base)                                                                                 \
    "pushq %rbp\n\t" "movq %rsp, %rbp\n\t"                                                                       \
    "pushq %rdi\n\t" "pushq %rsi\n\t" "pushq %rdx\n\t" "pushq %rcx\n\t" "pushq %r8\n\t" "pushq %r9\n\t"         \
    "movq %r8, %r9\n\t" "movq %rcx, %r8\n\t" "movq %rdx, %rcx\n\t" "movq %rsi, %rdx\n\t" "movq %rdi, %rsi\n\t"  \
    "movl $" #kind ", %edi\n\t" "callq _rd3HookPre\n\t"                                                           \
    "movq -8(%rbp), %rdi\n\t" "movq -16(%rbp), %rsi\n\t" "movq -24(%rbp), %rdx\n\t"                             \
    "movq -32(%rbp), %rcx\n\t" "movq -40(%rbp), %r8\n\t" "movq -48(%rbp), %r9\n\t"                               \
    "callq _" base "\n\t"                                                                                        \
    "pushq %rax\n\t" "pushq %rdx\n\t"                                                                            \
    "movq %rax, %r9\n\t" "movl $" #kind ", %edi\n\t" "movq -8(%rbp), %rsi\n\t" "movq -16(%rbp), %rdx\n\t"         \
    "movq -24(%rbp), %rcx\n\t" "movq -32(%rbp), %r8\n\t" "callq _rd3HookPost\n\t"                                 \
    "popq %rdx\n\t" "popq %rax\n\t" "leave\n\t" "retq\n\t"
__attribute__((naked)) void RD3DisplayPipe::submitFlipBuffer(IOAccelEvent *a0, unsigned int a1, IOAccelResource2 *a2,
                                                             IOAccelResource2 *a3)
{
    __asm__(RD3_HOOK_ASM(0, "rd3BaseSubmitFlipBuffer"));
}
__attribute__((naked)) void RD3DisplayPipe::submitScanoutFlipBuffer(IOAccelEvent *a0, unsigned int a1,
                                                                    IOAccelResource2 *a2, IOAccelResource2 *a3)
{
    __asm__(RD3_HOOK_ASM(1, "rd3BaseSubmitScanoutFlipBuffer"));
}
__attribute__((naked)) void RD3Surface::pickPresentType(unsigned int a0)
{
    __asm__(RD3_HOOK_ASM(2, "rd3BasePickPresentType"));
}
__attribute__((naked)) void RD3Surface::didSubmitSwap(unsigned int a0, unsigned int a1)
{
    __asm__(RD3_HOOK_ASM(3, "rd3BaseDidSubmitSwap"));
}
__attribute__((naked)) bool RD3Surface::isBackBufferReady(unsigned int a0)
{
    __asm__(RD3_HOOK_ASM(4, "rd3BaseIsBackBufferReady"));
}
__attribute__((naked)) void RD3Surface::submitUpdate(unsigned int a0, IOAccelBounds *a1, unsigned int a2)
{
    __asm__(RD3_HOOK_ASM(5, "rd3BaseSubmitUpdate"));
}

/* ---- the trace's hooks (card run 2026-10-17b) ---- */
static void rd3AccelTraceMode(unsigned arg)
{
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_MODE);
    e.arg = (uint8_t)arg;
    rd3Trace(e);
}
/* an IOSurface id seen with a resource: one SURFRES event per new
 * (resource, id, address); at swaps also a resource without an id (id 0:
 * the join with the plugin's trace says which backs have none). v4 the
 * resource's type byte. The table under its own lock (any thread; round 2) */
enum { kSurfResSlots = 64 };
static struct { uint32_t obj, id; unsigned long long phys; } gSurfRes[kSurfResSlots];
static unsigned gSurfResNext;
static void rd3AccelTraceSurfRes(const void *res, unsigned surfaceID, unsigned plane, unsigned how,
                                 unsigned long long phys, unsigned long long bytes, bool idZeroToo)
{
    if (!res || (!surfaceID && !idZeroToo)) return;
    IOSimpleLock *lk = gSurfResLock;
    if (!lk) return;
    const uint32_t obj = rd3TrObj(res);
    bool known = false;
    IOSimpleLockLock(lk);
    for (unsigned i = 0; i < kSurfResSlots && !known; i++)
        known = gSurfRes[i].obj == obj && gSurfRes[i].id == surfaceID && gSurfRes[i].phys == phys;
    if (!known) {
        const unsigned k = gSurfResNext++ % kSurfResSlots;
        gSurfRes[k].obj = obj; gSurfRes[k].id = surfaceID; gSurfRes[k].phys = phys;
    }
    IOSimpleLockUnlock(lk);
    if (known) return;
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_SURFRES);
    e.obj = obj;
    e.v0 = surfaceID;
    e.v1 = plane;
    e.v2 = how;
    e.v3 = rd3TrSat(bytes >> 10);
    e.v4 = rd3AccelResType(res);
    e.pa = phys;
    rd3Trace(e);
}
/* a legacy / device-cache resource's type byte (+0x14: 0xc0 device cache) */
static unsigned rd3AccelResType(const void *res)
{
    return res ? ((const volatile unsigned char *)res)[0x14] : 0;
}
/* the IOSurface behind a legacy / device-cache resource, as pageoff finds it */
static unsigned rd3AccelResSurfaceId(const void *res)
{
    const volatile unsigned char *r = (const volatile unsigned char *)res;
    if (!r || r[0x14] != 0xc0) return 0;
    void *cache = *(void *volatile *)(r + 0xe0);
    void *surf = cache ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
    return surf ? _ZNK9IOSurface12getSurfaceIDEv(surf) : 0;
}
static void rd3AccelTraceAperture(const void *res, unsigned what, bool ok, unsigned why, unsigned count)
{
    RD3VidMemory *m = rd3res_vidmem((IOAccelResource2 *)res);
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_APERTURE);
    e.arg = (uint8_t)what;
    e.flags = ok ? 1 : 0;
    e.obj = rd3TrObj(res);
    e.v0 = m ? rd3TrSat(m->fBytes >> 10) : 0;
    e.v1 = why == kApWhyCached ? 0 : rd3AccelResSurfaceId(res);   /* the id on new / failed paths only (round 2) */
    e.v2 = why;
    e.v3 = count;
    e.pa = m ? m->fPhys : 0;
    rd3Trace(e);
    if (e.v1 && m) rd3AccelTraceSurfRes(res, e.v1, 0, 1, m->fPhys, m->fBytes);
}
static void rd3AccelTraceVm(const void *vm, bool alloc, bool ok, unsigned long long phys, unsigned long long bytes,
                            unsigned long long mapped, IOReturn ret, bool scanout, unsigned extra, unsigned v2)
{
    struct rd3_trace_ev e = rd3TrEv(alloc ? RD3_TR_VMALLOC : RD3_TR_VMFREE);
    e.arg = ok ? 1 : 0;
    e.flags = (uint8_t)((scanout ? 1u : 0u) | extra);
    e.obj = rd3TrObj(vm);
    e.v0 = rd3TrSat(bytes >> 10);
    e.v1 = rd3TrSat(mapped >> 20);
    e.v4 = (uint32_t)ret;
    e.pa = phys;
    e.v2 = v2;   /* VMFREE of a released park: why (RD3_TRVMV2_*) */
    for (unsigned h = 0; alloc && h < kRd3MaxFB; h++)
        if ((const void *)&gSc[h] == vm) { e.flags |= 8; e.head = (uint8_t)h; }   /* D: the decoder's stale reset */
    if (!alloc) {
        /* who scans it now (Q5): mode 0's record, the flip away not latched; mode 1's D */
        for (unsigned h = 0; h < kRd3MaxFB; h++) {
            if (gLatch[h].vm == vm) { e.flags |= 2; e.head = (uint8_t)h; }
            else if (gLatch[h].prevVm == vm && !gLatch[h].st.latched) { e.flags |= 4; e.head = (uint8_t)h; }
            if ((const void *)&gSc[h] == vm && gSc[h].shows == RD3_SCS_D) { e.flags |= 8; e.head = (uint8_t)h; }
        }
        /* its content as it goes (Q3) */
        bool ownD = false;   /* the accelerator's D is no OSObject: never cast */
        for (unsigned h = 0; h < kRd3MaxFB; h++) ownD |= (const void *)&gSc[h] == vm;
        /* a released park is not what the vidmem maps now; and a park the VRAM-pressure release took may be
         * of an object freed meanwhile (rd3VmParkTrim): its pointer is a key only, never cast */
        const bool notNow = (extra & (kRd3VmTrParkGone | kRd3VmTrFreeLive | kRd3VmTrObjGone)) != 0;
        const RD3VidMemory *m = (ownD || notNow) ? nullptr : OSDynamicCast(RD3VidMemory, (OSObject *)vm);
        if (m) {
            RD3Scan sc;
            rd3AccelVmScan(m, &sc);
            memcpy(e.ba, sc.band, 8);
            e.v2 = sc.rc;
        }
    }
    rd3Trace(e);
}
enum { kShapeScans = 256 };
void rd3AccelTraceShape(const void *surf, unsigned mask, unsigned w, unsigned h, unsigned smode, unsigned present,
                        unsigned shaped, IOAccelResource2 *const *bufs)
{
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_SHAPE, rd3AccelSurfKnownHead(surf));
    e.obj = rd3TrObj(surf);
    e.v0 = mask;
    e.v1 = (w & 0xffffu) | (h << 16);
    e.v2 = smode;
    e.v3 = (present & 0xffu) | ((shaped & 0xffu) << 8);
    for (unsigned i = 0; bufs && i < 3; i++) {
        IOAccelResource2 *r = OSDynamicCast(IOAccelResource2, (OSObject *)bufs[i]);
        (i == 0 ? e.pa : i == 1 ? e.pb : e.pc) = r ? rd3res_phys(r) : 0;
    }
    rd3Trace(e);
    /* the present buffers' content at the shape (Q3): the first
     * kShapeScans shapes, or every one with debug.rdna3accel_trace_scan=2
     * (64 uncached BAR reads per buffer; ECC review round 2) */
    static unsigned shapes;
    if (shapes++ >= kShapeScans && gTrScan < 2) return;
    for (unsigned i = 0; bufs && i < 32; i++) {
        if (!(mask & (1u << i))) continue;
        IOAccelResource2 *r = OSDynamicCast(IOAccelResource2, (OSObject *)bufs[i]);
        if (!r || !rd3res_phys(r)) continue;
        RD3Scan sc;
        rd3AccelResScan(r, &sc);
        rd3AccelTraceScan(e.head < kRd3MaxFB ? e.head : kRd3MaxFB, surf, i, rd3res_phys(r), sc, 0, 0, 0, r);
    }
}
void rd3AccelTraceSync(const void *surf, unsigned what, unsigned long long v0, unsigned long long v1)
{
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_SYNC, rd3AccelSurfKnownHead(surf));
    e.arg = (uint8_t)what;
    e.obj = rd3TrObj(surf);
    e.v0 = (uint32_t)v0;
    e.v1 = (uint32_t)v1;
    rd3Trace(e);
}

/* ---- the swap mode, the A/B label and the trace's sysctls ---- */
/* the mode changes between two swaps: under the family's lock (the swaps
 * and copy forwards run under it), mode 0's records forgotten (stale after
 * any copy), the head's front record too; the panel moves posted applied
 * first; leaving modes 1/2, a head on D goes home with its last frame
 * (rd3AccelScLeave). No lock (an accelerator without one): refused, a
 * swap could run in either mode (ECC review round 1). */
static int rd3AccelSetSwapMode(int v, const char *why)
{
    RDNA3Accelerator *a = RDNA3Accelerator::gAccel;
    const bool locked = a && rd3FamLock(a, false);
    const int old = gSwapMode;
    if (a && !locked) {
        ALOG("swap mode %d -> %d (%s) REFUSED: the family's lock could not be taken -- the mode stays %d", old, v, why,
             old);
        return EBUSY;
    }
    if (old != v) {
        rd3AccelScApplyAll();
        if (old != RD3_SWAPMODE_FLIP && v == RD3_SWAPMODE_FLIP)
            for (unsigned h = 0; a && h < kRd3MaxFB; h++) rd3AccelScLeave(h, a->fFBs[h]);
        gSwapMode = v;
        for (unsigned h = 0; h < kRd3MaxFB; h++) {
            rd3AccelForgetShown(h);
            rd3AccelLatchForgetPres(h, "swap mode change");
            gSwapEmptyRun[h] = 0;
            rd3_sc_front_set(&gSc[h].front, nullptr, 0);
            rd3AccelScStaleReset(h, "a swap mode change");
            rd3AccelWgHeadReset(h, "a swap mode change");
        }
    }
    if (locked) rd3FamUnlock(a);
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_MODESET);
    e.arg = 0;
    e.v0 = (uint32_t)old;
    e.v1 = (uint32_t)v;
    rd3Trace(e);
    ALOG("swap mode %d -> %d (%s; %s)", old, v, why,
         locked ? "between two swaps, under the family's lock" : "no accelerator yet: nothing swaps");
    return 0;
}
static int rdna3accel_swapmode_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gSwapMode;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (!rd3_swapmode_valid(v)) return EINVAL;
    return rd3AccelSetSwapMode(v, "sysctl debug.rdna3accel_swap_mode");
}
static int rdna3accel_swapab_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gSwapAb;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || v > 255) return EINVAL;
    const int old = gSwapAb;
    gSwapAb = v;
    struct rd3_trace_ev e = rd3TrEv(RD3_TR_MODESET);
    e.arg = 1;
    e.v0 = (uint32_t)old;
    e.v1 = (uint32_t)v;
    rd3Trace(e);
    ALOG("swap A/B phase %d -> %d (autoreport; swap mode %d)", old, v, gSwapMode);
    return 0;
}
/* one read of the ring (all kept, or the stream's new events) or of the
 * per-second table; root only */
enum { kTrReadAll = 0, kTrReadStream = 1, kTrReadSec = 2 };
static int rd3AccelTraceRead(struct sysctl_req *req, int kind)
{
    if (req->newptr) return EPERM;
    if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
    const size_t secRows = (size_t)RD3_TRACE_SECS * RD3_TRACE_SEC_ROWS;
    const size_t body = kind == kTrReadSec ? secRows * sizeof(struct rd3_trace_sec)
                                           : (size_t)RD3_TRACE_CAP * sizeof(struct rd3_trace_ev);
    const size_t maxLen = sizeof(struct rd3_trace_hdr) + body;
    if (!req->oldptr) return SYSCTL_OUT(req, nullptr, maxLen);
    uint8_t *buf = (uint8_t *)IOMalloc(maxLen);
    if (!buf) return ENOMEM;
    bzero(buf, sizeof(struct rd3_trace_hdr));
    struct rd3_trace_hdr *h = (struct rd3_trace_hdr *)buf;
    uint32_t n = 0;
    uint64_t written = 0, lost = 0, cursor0 = 0, to = 0;
    if (gTrLock && gTr.ev) {
        if (kind == kTrReadSec) {
            /* 80 rows (16 seconds) at a time, the lock dropped between (ECC review round 1) */
            struct rd3_trace_sec *out = (struct rd3_trace_sec *)(buf + sizeof *h);
            for (size_t i = 0; gTrSec && i < secRows; i += RD3_TRACE_SEC_ROWS * 16) {
                IOSimpleLockLock(gTrLock);
                for (size_t k = i; k < i + RD3_TRACE_SEC_ROWS * 16 && k < secRows; k++)
                    if (gTrSec[k].sec) out[n++] = gTrSec[k];
                IOSimpleLockUnlock(gTrLock);
            }
            IOSimpleLockLock(gTrLock);
            written = gTr.written;
            lost = rd3_trace_lost(&gTr);
            IOSimpleLockUnlock(gTrLock);
        } else {
            /* the window [from, to] fixed at the start, copied RD3_TRACE_CHUNK
             * events at a time with the lock dropped between: events
             * overwritten meanwhile are counted lost */
            uint64_t next;
            IOSimpleLockLock(gTrLock);
            to = written = gTr.written;
            cursor0 = gTrCursor;
            if (kind == kTrReadStream) next = rd3_trace_stream_from(&gTr, cursor0, &lost);
            else { next = rd3_trace_oldest(&gTr); lost = rd3_trace_lost(&gTr); }
            IOSimpleLockUnlock(gTrLock);
            struct rd3_trace_ev *ev = (struct rd3_trace_ev *)(buf + sizeof *h);
            while (next <= to && n < RD3_TRACE_CAP) {
                IOSimpleLockLock(gTrLock);
                const uint32_t want = RD3_TRACE_CAP - n < RD3_TRACE_CHUNK ? RD3_TRACE_CAP - n : RD3_TRACE_CHUNK;
                const uint32_t got = rd3_trace_copy_chunk(&gTr, &next, to, ev + n, want, &lost);
                IOSimpleLockUnlock(gTrLock);
                n += got;
                if (!got) break;
            }
        }
    }
    mach_timebase_info_data_t tb = { 1, 1 };
    clock_timebase_info(&tb);
    clock_sec_t ws = 0;
    clock_usec_t wus = 0;
    h->magic = RD3_TRACE_MAGIC;
    h->version = RD3_TRACE_VERSION;
    h->ev_size = kind == kTrReadSec ? sizeof(struct rd3_trace_sec) : sizeof(struct rd3_trace_ev);
    h->cap = kind == kTrReadSec ? (uint32_t)secRows : RD3_TRACE_CAP;
    h->count = n;
    h->swap_mode = (uint32_t)gSwapMode;
    h->written = written;
    h->lost = lost;
    h->now = mach_absolute_time();
    clock_get_calendar_microtime(&ws, &wus);
    h->wall_us = (uint64_t)ws * 1000000ull + (uint64_t)wus;
    h->nomem = (uint64_t)gCntTrNomem;
    h->boot_id = gTrBootId;
    h->source = RD3_TRACE_SRC_ACCEL;
    h->tb_numer = tb.numer;
    h->tb_denom = tb.denom;
    h->read_kind = (uint32_t)kind;
    h->ab = (uint32_t)gSwapAb;
    h->flags = RD3_TRACE_HDR_HDP_NOINV | RD3_TRACE_HDR_CHUNKED;
    h->hdr_size = sizeof *h;
    const int err = SYSCTL_OUT(req, buf, sizeof *h + (size_t)n * h->ev_size);
    IOFree(buf, maxLen);
    /* the stream's cursor moves only once the reader got the events (a
     * failed copyout reads them again), and only if no other reader moved
     * it meanwhile (ECC review round 1) */
    if (!err && kind == kTrReadStream && gTrLock) {
        IOSimpleLockLock(gTrLock);
        if (gTrCursor == cursor0) gTrCursor = to;
        IOSimpleLockUnlock(gTrLock);
    }
    return err;
}
static int rdna3accel_trace_sysctl SYSCTL_HANDLER_ARGS { (void)oidp; (void)arg1; (void)arg2; return rd3AccelTraceRead(req, kTrReadAll); }
static int rdna3accel_trace_stream_sysctl SYSCTL_HANDLER_ARGS { (void)oidp; (void)arg1; (void)arg2; return rd3AccelTraceRead(req, kTrReadStream); }
static int rdna3accel_trace_sec_sysctl SYSCTL_HANDLER_ARGS { (void)oidp; (void)arg1; (void)arg2; return rd3AccelTraceRead(req, kTrReadSec); }
static int gTrDumpAsk = 0;
static int rdna3accel_trace_dump_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gTrDumpAsk;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v <= 0) return EINVAL;
    if ((unsigned)v > RD3_TRACE_DUMP_MAX) v = (int)RD3_TRACE_DUMP_MAX;
    gTrDumpAsk = v;
    const uint64_t w = (uint64_t)gCntTrWritten;
    rd3TraceDumpQueue(w > (uint64_t)v ? w - (uint64_t)v + 1 : 1, w, "asked by sysctl debug.rdna3accel_trace_dump");
    return 0;
}
static SInt64 gCntTrLostRead;
static int rdna3accel_trace_lost_sysctl SYSCTL_HANDLER_ARGS
{
    SInt64 v = 0;
    if (gTrLock) { IOSimpleLockLock(gTrLock); v = (SInt64)rd3_trace_lost(&gTr); IOSimpleLockUnlock(gTrLock); }
    gCntTrLostRead = v;
    return sysctl_handle_quad(oidp, &v, 0, req);
}
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_trace, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED, nullptr, 0,
            rdna3accel_trace_sysctl, "S,rd3_trace", "card run 2026-10-17b: the per-frame trace, every event kept (common/rdna3_trace.h; root)");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_trace_stream, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED,
            nullptr, 0, rdna3accel_trace_stream_sysctl, "S,rd3_trace", "the per-frame trace since the last read of this one (root)");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_trace_sec, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED,
            nullptr, 0, rdna3accel_trace_sec_sysctl, "S,rd3_trace_sec", "the trace's per-second table (root)");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_trace_dump, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0,
            rdna3accel_trace_dump_sysctl, "I", "write n: the newest n trace events (at most 2000) into the kernel log");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_trace_scan, CTLFLAG_RW | CTLFLAG_LOCKED, &gTrScan, 0,
           "the trace samples the swapping surface's other buffers: 0 only at empty backs, 1 (default) every swap "
           "for a head's first 600, during autoreport's A/B and at empty backs, else every 8th, 2 every swap");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_swap_mode, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, nullptr, 0,
            rdna3accel_swapmode_sysctl, "I",
            "card run 2026-10-17b: 0 flip to WindowServer's back, 1 (default again since the write generations, boot-arg "
            "rdna3swapmode) copy it, an empty back never over a picture, 2 copy every back");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_swap_ab, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0,
            rdna3accel_swapab_sysctl, "I", "autoreport's A/B phase (a label in the trace: 0 none, 1 mode 1, 2 mode 0, 3 back)");
RD3ACCEL_CNT(rdna3accel_swap_trace_events, gCntTrWritten, "card run 2026-10-17b: trace events recorded since the boot");
RD3ACCEL_CNT(rdna3accel_swap_trace_nomem, gCntTrNomem, "trace events not recorded: no memory for the ring");
SYSCTL_PROC(_debug, OID_AUTO, rdna3accel_swap_trace_lost, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
            rdna3accel_trace_lost_sysctl, "Q", "trace events overwritten in the ring so far (the stream readers count their own)");
RD3ACCEL_CNT4(rdna3accel_swaps_copy, gCntScCopy, "swap mode 1/2: backs copied to the panel");
RD3ACCEL_CNT4(rdna3accel_swaps_copyempty, gCntScEmpty, "swap mode 1: empty backs NOT copied over a picture (the panel kept its frame)");
RD3ACCEL_CNT4(rdna3accel_swaps_copyblack, gCntScBlack, "swap mode 1/2: empty backs copied (black frames)");
RD3ACCEL_CNT4(rdna3accel_swaps_copylanded, gCntScLanded, "card run 2026-10-17c: swap mode 1/2: the named back empty (or a stale one without an IOSurface), the surface's buffer WindowServer's frame landed in presented instead");
RD3ACCEL_CNT4(rdna3accel_swaps_stalehold_would, gCntScStaleWould, "swap mode 1: stale backs that WOULD have been held (debug.rdna3accel_swap_stale=0): copied");
RD3ACCEL_CNT4(rdna3accel_swaps_stalehold_miss, gCntScStaleMiss, "swap mode 1: stale-back candidates whose second sample grid differed (a small redraw): copied");
RD3ACCEL_CNT4(rdna3accel_swaps_stalehold_changed, gCntScStaleChanged, "swap mode 1: held stale backs that changed in the ~4 ms re-sample (a frame landed after the hold)");
RD3ACCEL_CNT4(rdna3accel_swaps_stalenocf, gCntScStaleNocf, "swap mode 1: backs changed since presented with no copy forward into them (the stale rule's assumption broken)");
RD3ACCEL_CNT4(rdna3accel_swaps_stalereset, gCntScStaleReset, "swap mode 1: stale-back tables forgotten (surface / pipe resource freed, mode changes, D reallocated)");
RD3ACCEL_CNT4(rdna3accel_swaps_staleevict, gCntScStaleEvict, "swap mode 1: stale-back records evicted from the full 8-slot table (that buffer copied at its next swap)");
RD3ACCEL_CNT4(rdna3accel_swaps_stalegone, gCntScStaleGone, "swap mode 1: stale-back records forgotten because their VRAM was freed");
RD3ACCEL_CNT4(rdna3accel_swaps_stalecheck_norecord, gCntScStaleNoRec, "swap mode 1: backs with pixels not held: never presented on this head (or forgotten / evicted)");
RD3ACCEL_CNT4(rdna3accel_swaps_stalecheck_changed, gCntScStaleChg, "swap mode 1: backs with pixels not held: changed since presented");
RD3ACCEL_CNT4(rdna3accel_swaps_stalecheck_cf, gCntScStaleCfd, "swap mode 1: backs with pixels not held: unchanged, but a copy forward targeted them");
RD3ACCEL_CNT4(rdna3accel_swaps_stalecheck_sameshown, gCntScStaleSame, "swap mode 1: backs with pixels not held: nothing else presented after them");
RD3ACCEL_CNT4(rdna3accel_swaps_stalecheck_nopanel, gCntScStaleNoPanel, "swap mode 1: backs with pixels not held: the panel held no frame this mode copied");
RD3ACCEL_CNT4(rdna3accel_swaps_stalecheck_nohead, gCntScStaleNoHead, "swap mode 1: backs with pixels not judged: no head known at the swap's entry");
RD3ACCEL_CNT4(rdna3accel_swaps_stalehold, gCntScStale, "card run 2026-10-17c: swap mode 1: backs WindowServer did not redraw since this head presented them (no copy forward into them, the samples unchanged, another buffer presented after them) held: the panel kept the newer frame");
RD3ACCEL_CNT4(rdna3accel_swaps_copylanded_would, gCntScLandedWould, "swap mode 1/2: such a landed buffer found but NOT presented (debug.rdna3accel_swap_landed=0): the back's old rule applied");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_swap_stale, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gSwapStale, 0,
           "card run 2026-10-17c: 1 (default, boot-arg rdna3swapstale) swap mode 1 holds a back WindowServer did not "
           "redraw since the head presented it, 0 copies it and only counts and traces it");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_swap_landed, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gSwapLanded, 0,
           "card run 2026-10-17c: 1 swap modes 1/2 present the buffer WindowServer's frame landed in instead of an "
           "empty / stale back, 0 (default, boot-arg rdna3swaplanded) only count and trace it");
RD3ACCEL_CNT4(rdna3accel_swaps_copybytes, gCntScBytes, "swap mode 1/2: bytes copied to the panel");
RD3ACCEL_CNT4(rdna3accel_swaps_copyus, gCntScUs, "swap mode 1/2: microseconds of those copies, summed");
RD3ACCEL_CNT4(rdna3accel_swaps_copyusmax, gCntScUsMax, "swap mode 1/2: the longest copy, microseconds");
RD3ACCEL_CNT4(rdna3accel_swaps_copywait, gCntScWait, "swap mode 1/2: waits for the last flip to latch before a copy");
RD3ACCEL_CNT4(rdna3accel_swaps_copywaitus, gCntScWaitUs, "swap mode 1/2: microseconds of those waits, summed");
RD3ACCEL_CNT4(rdna3accel_swaps_copylatchto, gCntScLatchTo, "swap mode 1/2: waits that ran out (copied anyway: may tear)");
RD3ACCEL_CNT4(rdna3accel_swaps_copyfail, gCntScFail, "swap mode 1/2: swaps whose copy did not reach the panel");
RD3ACCEL_CNT4(rdna3accel_swaps_copydirect, gCntScDirect, "swap mode 1/2: copies into the buffer on the panel (no second buffer: may tear)");
RD3ACCEL_CNT4(rdna3accel_swaps_copyfront, gCntScFront, "swap mode 1/2: backs copied into the front too (copy forward's source)");
RD3ACCEL_CNT4(rdna3accel_swaps_copyfrontfail, gCntScFrontFail, "swap mode 1/2: backs NOT copied into the front");
RD3ACCEL_CNT4(rdna3accel_cf_scsrc, gCntCfScSrc, "swap mode 1/2: copy forwards from the front (Apple's model)");
RD3ACCEL_CNT4(rdna3accel_cf_scscanout, gCntCfScScanout, "swap mode 1/2: copy forwards from the panel's buffer (the front missed its copy)");
RD3ACCEL_CNT4(rdna3accel_cf_scstale, gCntCfScStale, "swap mode 1/2: copy forwards with no source holding the last frame (rects pending)");
RD3ACCEL_CNT4(rdna3accel_swaps_copynowait, gCntScNoWait, "swap mode 1/2: copies NOT waited for (the head's waits off after 3 latch timeouts: may tear)");
RD3ACCEL_CNT4(rdna3accel_swaps_copybrkflip, gCntScBrkFlip, "swap mode 1/2: swaps flipped as in mode 0, the SDMA breaker open");
RD3ACCEL_CNT4(rdna3accel_swaps_copyfreewait, gCntScFreeWait, "swap mode 1/2: an old second buffer kept (on the panel, or its flip away not latched)");
RD3ACCEL_CNT(rdna3accel_swaps_copybrktrips, gCntScBrkTrips, "swap mode 1/2: times the SDMA breaker opened (copies failed or timed out)");
/* card run 2026-10-17d: the write generations (rdna3_write_gen.h) and the flip-present hooks */
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_swap_gen, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gSwapGen, 0,
           "card run 2026-10-17d: 1 (default, boot-arg rdna3swapgen) swap modes 0 and 1 present a back only when "
           "WindowServer wrote it (page-on, copy forward) after the head's frame, else hold it; copy forward takes a "
           "front written after that frame as its source; 0 only counts and traces what would have been held");
SYSCTL_INT(_debug, OID_AUTO, rdna3accel_flipbuf_present, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gFlipbufPresent, 0,
           "card run 2026-10-17d: 1 IOAccelLegacyDisplayPipe::submitFlipBuffer / submitScanoutFlipBuffer also present "
           "their buffer as the swap mode does (copy into D / home in mode 1, flip in mode 0, the latch wait, the "
           "write generations); 0 (default, boot-arg rdna3flipbufpresent) only traced, counted and forwarded");
RD3ACCEL_CNT4(rdna3accel_swaps_genpresent, gCntWgPresent, "card run 2026-10-17d: swaps (and flip-buffer presents) the write generations presented: written after the head's frame, the first after a reset, or changed with no write seen");
RD3ACCEL_CNT4(rdna3accel_swaps_genhold, gCntWgHold, "card run 2026-10-17d: swaps held by the write generations: the back not written since the head's frame (no flip, no copy, the panel kept its frame)");
RD3ACCEL_CNT4(rdna3accel_swaps_genhold_would, gCntWgWould, "swaps the write generations WOULD have held (debug.rdna3accel_swap_gen=0, or swap mode 2): presented as before");
RD3ACCEL_CNT4(rdna3accel_swaps_genfirst, gCntWgFirst, "of the presented, the first swap after the head's reset (mode or swap mode change, an IOP flip)");
RD3ACCEL_CNT4(rdna3accel_swaps_gencontent, gCntWgContent, "of the presented, backs NOT written since this head presented them whose samples changed: a writer the accelerator does not see");
RD3ACCEL_CNT4(rdna3accel_swaps_genunwritten, gCntWgUnwritten, "of the held (or would-hold), backs never written and never presented on this head (the buf[2] WindowServer never draws)");
RD3ACCEL_CNT4(rdna3accel_cf_gennewer, gCntCfGenNewer, "card run 2026-10-17d: copy forwards whose source the write generations changed: the front written after the head's frame (a burst's chain)");
RD3ACCEL_CNT(rdna3accel_swaps_gen_aperture, gCntWgAperture, "card run 2026-10-17d: write generations handed out by the family's page-on (addToAperture, getApertureMemoryDescriptor)");
RD3ACCEL_CNT(rdna3accel_swaps_gen_cf, gCntWgCf, "write generations handed out by copy forward (its target)");
RD3ACCEL_CNT(rdna3accel_swaps_gen_evicted, gCntWgEvicted, "write generations evicted from the full 32-slot table (that buffer reads 'never written' until its next write)");
RD3ACCEL_CNT(rdna3accel_swaps_gen_freed, gCntWgFreed, "write generations and samples forgotten because their VRAM was freed");
RD3ACCEL_CNT(rdna3accel_swaps_gen_resets, gCntWgResets, "heads whose generation record was reset (mode or swap mode change, an IOP flip): their next swap presents");
RD3ACCEL_CNT4(rdna3accel_swaps_gen_resets, gCntWgResetsH, "ECC review round 1: of those, this head's (another surface taking over after 500 ms quiet, mode change, IOP flip)");
RD3ACCEL_CNT4(rdna3accel_swaps_gen_judged, gCntWgJudged, "ECC review round 1: swaps (and flip-buffer presents) the write generations judged = genpresent + genhold + genhold_would + gen_landed");
RD3ACCEL_CNT4(rdna3accel_swaps_gen_aged, gCntWgAged, "of the presented, by the age guard: no present for 250 ms and 3 holds in a row (a hold never freezes the panel)");
RD3ACCEL_CNT4(rdna3accel_swaps_gen_evictpresent, gCntWgEvictPresent, "of the presented, by the eviction guard: a back with no generation while a newer one was evicted");
RD3ACCEL_CNT4(rdna3accel_swaps_gen_landed, gCntWgLanded, "swap mode 1: swaps the write generations would have held whose surface had a buffer written after the head's frame: that one presented");
RD3ACCEL_CNT4(rdna3accel_swaps_gennotjudged, gCntWgNotJudged, "swaps the write generations did not judge (no head known at the swap's entry, no VRAM address): presented as before");
RD3ACCEL_CNT(rdna3accel_swaps_gen_apcached, gCntWgApCached, "ECC review round 1: write generations handed out by the page-on's cached descriptor (getApertureMemoryDescriptor's second and later calls)");
RD3ACCEL_CNT(rdna3accel_swaps_gen_cfnostamp, gCntWgCfNoStamp, "copy forwards whose target kept its generation: the copy failed, or not every rect was copied");
RD3ACCEL_CNT4(rdna3accel_flipbuf, gCntFlipbuf, "card run 2026-10-17d: IOAccelLegacyDisplayPipe::submitFlipBuffer calls (traced, forwarded to the family)");
RD3ACCEL_CNT4(rdna3accel_flipbuf_scanout, gCntFlipbufScanout, "IOAccelLegacyDisplayPipe::submitScanoutFlipBuffer calls (traced, forwarded to the family)");
RD3ACCEL_CNT4(rdna3accel_flipbuf_presented, gCntFlipbufPresented, "flip-buffer calls whose buffer was presented (debug.rdna3accel_flipbuf_present=1)");
RD3ACCEL_CNT4(rdna3accel_flipbuf_held, gCntFlipbufHeld, "flip-buffer presents held: not written since the head's frame, or empty (debug.rdna3accel_flipbuf_present=1)");
RD3ACCEL_CNT4(rdna3accel_flipbuf_failed, gCntFlipbufFailed, "flip-buffer presents whose copy or flip failed (debug.rdna3accel_flipbuf_present=1)");
RD3ACCEL_CNT(rdna3accel_flipbuf_nohead, gCntFlipbufNoHead, "flip-buffer calls on a pipe that drives no framebuffer of ours");
RD3ACCEL_CNT4(rdna3accel_flipbuf_busy, gCntFlipbufBusy, "flip-buffer presents not done: the family's lock held by another thread");
RD3ACCEL_CNT4(rdna3accel_flipbuf_unlocked, gCntFlipbufUnlocked, "flip-buffer presents that took the family's lock themselves (called without it)");
RD3ACCEL_CNT4(rdna3accel_flipbuf_emptyheld, gCntFlipbufEmptyHeld, "ECC review round 1: of the flip-buffer holds, the buffer read empty");
RD3ACCEL_CNT(rdna3accel_flipbuf_nores, gCntFlipbufNoRes, "ECC review round 1: flip-buffer presents not done: neither buffer a VRAM resource of ours other than the head's framebuffer");
RD3ACCEL_CNT(rdna3accel_flipbuf_intoff, gCntFlipbufIntOff, "ECC review round 1: flip-buffer calls with interrupts disabled (only counted, forwarded)");
RD3ACCEL_CNT(rdna3accel_flipbuf_badptr, gCntFlipbufBadPtr, "ECC review round 1: flip-buffer calls with a resource argument below the kernel's addresses (not cast, not presented)");
RD3ACCEL_CNT(rdna3accel_hook_intoff, gCntHookIntOff, "ECC review round 1: surface hook calls with interrupts disabled (only counted, forwarded)");
RD3ACCEL_CNT(rdna3accel_hook_intr, gCntHookIntr, "flip-present hook calls at interrupt level (only counted, forwarded)");
RD3ACCEL_CNT(rdna3accel_hook_intr_flipbuf, gCntHookIntrK[kHkFlipBuf], "ECC review round 1: of those, submitFlipBuffer");
RD3ACCEL_CNT(rdna3accel_hook_intr_scanout, gCntHookIntrK[kHkScanoutFlipBuf], "of those, submitScanoutFlipBuffer");
RD3ACCEL_CNT(rdna3accel_hook_intr_pickpresent, gCntHookIntrK[kHkPickPresent], "of those, pickPresentType");
RD3ACCEL_CNT(rdna3accel_hook_intr_didsubmitswap, gCntHookIntrK[kHkDidSubmitSwap], "of those, didSubmitSwap");
RD3ACCEL_CNT(rdna3accel_hook_intr_backready, gCntHookIntrK[kHkBackReady], "of those, isBackBufferReady");
RD3ACCEL_CNT(rdna3accel_hook_intr_submitupdate, gCntHookIntrK[kHkSubmitUpdate], "of those, submitUpdate");
RD3ACCEL_CNT(rdna3accel_hook_pickpresent, gCntHook[kHkPickPresent], "card run 2026-10-17d: IOAccelLegacySurface::pickPresentType calls (traced, forwarded)");
RD3ACCEL_CNT(rdna3accel_hook_didsubmitswap, gCntHook[kHkDidSubmitSwap], "IOAccelLegacySurface::didSubmitSwap calls (traced, forwarded)");
RD3ACCEL_CNT(rdna3accel_hook_backready, gCntHook[kHkBackReady], "IOAccelLegacySurface::isBackBufferReady calls (traced, forwarded)");
RD3ACCEL_CNT(rdna3accel_hook_backready_no, gCntHookBackNo, "of those, answered not ready by the family");
RD3ACCEL_CNT(rdna3accel_hook_submitupdate, gCntHook[kHkSubmitUpdate], "IOAccelLegacySurface::submitUpdate calls (traced, forwarded)");
/* the vtable offset of a virtual member function (Itanium ABI: its pointer's first word is 1 + the byte offset
 * from the vtable's address point); 0 when it is not a virtual one */
template <class M> static unsigned rd3VtOff(M m)
{
    uint64_t w = 0;
    memcpy(&w, &m, sizeof w);
    return (w & 1) ? (unsigned)(w - 1) : 0u;
}
/* the boot's line that the hooks are in (ECC review round 1): each override's vtable offset as compiled - the
 * base calls (rd3Base*) jump through the base's vtable symbol, 16 bytes before the address point, so their
 * operands are these + 16: 2512 2504 2672 2640 2648 2664 (tools/check_accel_slots.py checks the compiled ones) */
static void rd3AccelHookInstalledLog(void)
{
    ALOG("flip-present hooks installed: RD3DisplayPipe submitFlipBuffer vtable+%u, submitScanoutFlipBuffer +%u; "
         "RD3Surface pickPresentType +%u, didSubmitSwap +%u, isBackBufferReady +%u, submitUpdate +%u; "
         "RDNA3Accelerator acceleratorDidLock +%u, acceleratorWillUnlock +%u (the base calls' slots are these + 16) "
         "-- each logs its first 16 calls as 'hook:' lines (debug.rdna3accel_flipbuf_h<N>, _hook_*)",
         rd3VtOff(&RD3DisplayPipe::submitFlipBuffer), rd3VtOff(&RD3DisplayPipe::submitScanoutFlipBuffer),
         rd3VtOff(&RD3Surface::pickPresentType), rd3VtOff(&RD3Surface::didSubmitSwap),
         rd3VtOff(&RD3Surface::isBackBufferReady), rd3VtOff(&RD3Surface::submitUpdate),
         rd3VtOff(&RDNA3Accelerator::acceleratorDidLock), rd3VtOff(&RDNA3Accelerator::acceleratorWillUnlock));
}
static void rd3AccelInstallSysctlGen(void)
{
    rd3AccelHookInstalledLog();
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_gen);    sysctl_register_oid(&sysctl__debug_rdna3accel_flipbuf_present);
    RD3ACCEL_REG4(rdna3accel_swaps_genpresent); RD3ACCEL_REG4(rdna3accel_swaps_genhold);
    RD3ACCEL_REG4(rdna3accel_swaps_genhold_would); RD3ACCEL_REG4(rdna3accel_swaps_genfirst);
    RD3ACCEL_REG4(rdna3accel_swaps_gencontent); RD3ACCEL_REG4(rdna3accel_swaps_genunwritten);
    RD3ACCEL_REG4(rdna3accel_cf_gennewer);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_gen_aperture); sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_gen_cf);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_gen_evicted); sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_gen_freed);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_gen_resets);
    RD3ACCEL_REG4(rdna3accel_swaps_gen_resets); RD3ACCEL_REG4(rdna3accel_swaps_gen_judged);
    RD3ACCEL_REG4(rdna3accel_swaps_gen_aged);   RD3ACCEL_REG4(rdna3accel_swaps_gen_evictpresent);
    RD3ACCEL_REG4(rdna3accel_swaps_gen_landed); RD3ACCEL_REG4(rdna3accel_swaps_gennotjudged);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_gen_apcached);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_gen_cfnostamp);
    RD3ACCEL_REG4(rdna3accel_flipbuf);          RD3ACCEL_REG4(rdna3accel_flipbuf_scanout);
    RD3ACCEL_REG4(rdna3accel_flipbuf_presented); RD3ACCEL_REG4(rdna3accel_flipbuf_held);
    RD3ACCEL_REG4(rdna3accel_flipbuf_failed);
    sysctl_register_oid(&sysctl__debug_rdna3accel_flipbuf_nohead); sysctl_register_oid(&sysctl__debug_rdna3accel_hook_intr);
    RD3ACCEL_REG4(rdna3accel_flipbuf_busy); RD3ACCEL_REG4(rdna3accel_flipbuf_unlocked);
    RD3ACCEL_REG4(rdna3accel_flipbuf_emptyheld);
    sysctl_register_oid(&sysctl__debug_rdna3accel_flipbuf_nores); sysctl_register_oid(&sysctl__debug_rdna3accel_flipbuf_intoff);
    sysctl_register_oid(&sysctl__debug_rdna3accel_flipbuf_badptr); sysctl_register_oid(&sysctl__debug_rdna3accel_hook_intoff);
    sysctl_register_oid(&sysctl__debug_rdna3accel_hook_intr_flipbuf); sysctl_register_oid(&sysctl__debug_rdna3accel_hook_intr_scanout);
    sysctl_register_oid(&sysctl__debug_rdna3accel_hook_intr_pickpresent);
    sysctl_register_oid(&sysctl__debug_rdna3accel_hook_intr_didsubmitswap);
    sysctl_register_oid(&sysctl__debug_rdna3accel_hook_intr_backready);
    sysctl_register_oid(&sysctl__debug_rdna3accel_hook_intr_submitupdate);
    sysctl_register_oid(&sysctl__debug_rdna3accel_hook_pickpresent); sysctl_register_oid(&sysctl__debug_rdna3accel_hook_didsubmitswap);
    sysctl_register_oid(&sysctl__debug_rdna3accel_hook_backready); sysctl_register_oid(&sysctl__debug_rdna3accel_hook_backready_no);
    sysctl_register_oid(&sysctl__debug_rdna3accel_hook_submitupdate);
}
static void rd3AccelInstallSysctl2(void)
{
    sysctl_register_oid(&sysctl__debug_rdna3accel_trace);        sysctl_register_oid(&sysctl__debug_rdna3accel_trace_stream);
    sysctl_register_oid(&sysctl__debug_rdna3accel_trace_sec);    sysctl_register_oid(&sysctl__debug_rdna3accel_trace_dump);
    sysctl_register_oid(&sysctl__debug_rdna3accel_trace_scan);   sysctl_register_oid(&sysctl__debug_rdna3accel_swap_mode);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_ab);      sysctl_register_oid(&sysctl__debug_rdna3accel_swap_trace_events);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_trace_nomem); sysctl_register_oid(&sysctl__debug_rdna3accel_swap_trace_lost);
    RD3ACCEL_REG4(rdna3accel_swaps_copy);      RD3ACCEL_REG4(rdna3accel_swaps_copyempty);
    RD3ACCEL_REG4(rdna3accel_swaps_copylanded); RD3ACCEL_REG4(rdna3accel_swaps_copylanded_would);
    RD3ACCEL_REG4(rdna3accel_swaps_stalehold);
    RD3ACCEL_REG4(rdna3accel_swaps_stalehold_would); RD3ACCEL_REG4(rdna3accel_swaps_stalehold_miss);
    RD3ACCEL_REG4(rdna3accel_swaps_stalehold_changed); RD3ACCEL_REG4(rdna3accel_swaps_stalenocf);
    RD3ACCEL_REG4(rdna3accel_swaps_stalereset); RD3ACCEL_REG4(rdna3accel_swaps_staleevict);
    RD3ACCEL_REG4(rdna3accel_swaps_stalegone); RD3ACCEL_REG4(rdna3accel_swaps_stalecheck_norecord);
    RD3ACCEL_REG4(rdna3accel_swaps_stalecheck_changed); RD3ACCEL_REG4(rdna3accel_swaps_stalecheck_cf);
    RD3ACCEL_REG4(rdna3accel_swaps_stalecheck_sameshown); RD3ACCEL_REG4(rdna3accel_swaps_stalecheck_nopanel);
    RD3ACCEL_REG4(rdna3accel_swaps_stalecheck_nohead);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_landed);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_stale);
    RD3ACCEL_REG4(rdna3accel_swaps_copyblack); RD3ACCEL_REG4(rdna3accel_swaps_copybytes);
    RD3ACCEL_REG4(rdna3accel_swaps_copyus);    RD3ACCEL_REG4(rdna3accel_swaps_copyusmax);
    RD3ACCEL_REG4(rdna3accel_swaps_copywait);  RD3ACCEL_REG4(rdna3accel_swaps_copywaitus);
    RD3ACCEL_REG4(rdna3accel_swaps_copylatchto); RD3ACCEL_REG4(rdna3accel_swaps_copyfail);
    RD3ACCEL_REG4(rdna3accel_swaps_copydirect); RD3ACCEL_REG4(rdna3accel_swaps_copyfront);
    RD3ACCEL_REG4(rdna3accel_swaps_copyfrontfail); RD3ACCEL_REG4(rdna3accel_cf_scsrc);
    RD3ACCEL_REG4(rdna3accel_cf_scscanout);    RD3ACCEL_REG4(rdna3accel_cf_scstale);
    RD3ACCEL_REG4(rdna3accel_swaps_copynowait); RD3ACCEL_REG4(rdna3accel_swaps_copybrkflip);
    RD3ACCEL_REG4(rdna3accel_swaps_copyfreewait);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swaps_copybrktrips);
    rd3AccelInstallSysctlGen();   /* card run 2026-10-17d */
    rd3AccelInstallSysctlPq();    /* DRIVER-PLAN N38: the present queue, the GPU utilization */
    rd3AccelInstallSysctlPark();  /* DRIVER-PLAN N41: VRAM parking, WindowServer's pending GPU writes */
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
