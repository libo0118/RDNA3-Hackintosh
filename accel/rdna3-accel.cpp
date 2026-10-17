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
static void               rd3AccelWriteStamp(int index, unsigned int value);
static bool               rd3AccelLocksInit(void);
/* card run 2026-10-17b: the per-frame trace (rdna3_trace.h) and the swap as
 * a copy (rdna3_swap_copy.h); hooks the classes above their definitions call */
void                      rd3AccelTraceShape(const void *surf, unsigned mask, unsigned w, unsigned h, unsigned smode,
                                             unsigned present, unsigned shaped, IOAccelResource2 *const *bufs);
void                      rd3AccelTraceSync(const void *surf, unsigned what, unsigned long long v0,
                                            unsigned long long v1);
static void               rd3AccelTraceMode(unsigned arg);
static void               rd3AccelScModeChanged(void);
static void               rd3AccelScPanelMoved(unsigned head, const char *why, uint64_t issuedAt);
static void               rd3AccelScFrontGone(const void *vm);
/* APERTURE's v2: the path (getApertureMemoryDescriptor's every return, ECC review round 1) */
enum { kApWhyNew = 0, kApWhyGate = 1, kApWhyCached = 2, kApWhyNoVram = 3, kApWhyRange = 4 };
static void               rd3AccelTraceAperture(const void *res, unsigned what, bool ok, unsigned why,
                                                unsigned count = 0);
static void               rd3AccelTraceVm(const void *vm, bool alloc, bool ok, unsigned long long phys,
                                          unsigned long long bytes, unsigned long long mapped, IOReturn ret,
                                          bool scanout);
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
/* card run 2026-10-17 (rdna3_flip_latch.h): the swap waits until its flip
 * latched before it returns (sysctl debug.rdna3accel_swap_latch, boot-arg
 * rdna3swaplatch, default 1; 0 = return at once as before, the next flip
 * waits in flipToLocked), and the test of the buffers the panel showed
 * (debug.rdna3accel_swap_torn / rdna3swaptorn: 0 off, 1 (default) the first
 * 20000 checks per head, 2 always; one more band hash per swap) */
static int gSwapLatch = 1, gSwapTorn = 1;
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
static int gTrScan = 1;
/* debug.rdna3accel_swap_landed / boot-arg rdna3swaplanded (card run
 * 2026-10-17c, rdna3_swap_copy.h): 1 modes 1/2 present the landed buffer
 * instead of a suspect back; 0 (the default) they only count, trace and log
 * what they would have presented (the rule fired 0 times in that run's
 * pattern and can show a half-drawn frame: the next report says whether it
 * matters) */
static int gSwapLanded = 0;
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
        gSwapLatch = (int)rd3_arg("rdna3swaplatch", 1);
        gSwapTorn = (int)rd3_arg("rdna3swaptorn", 1);
        gSwapLanded = rd3_arg("rdna3swaplanded", 0) ? 1 : 0;
        ALOG("swap landed: %s (card run 2026-10-17c; boot-arg rdna3swaplanded, sysctl debug.rdna3accel_swap_landed)",
             gSwapLanded ? "1 = modes 1/2 present the buffer WindowServer's frame landed in instead of an empty / "
                           "stale back"
                         : "0 = only counted and traced (swaps_copylanded_would_h<N>), the back's old rule stands");
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
        /* the cached descriptor: every page-on of a buffer comes here; the
         * first 4 per resource traced, then every 256th (v3 the count) */
        const unsigned c = ++fApCached;
        if (c <= 4 || (c & 255) == 0) rd3AccelTraceAperture(this, 2, true, kApWhyCached, c);
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
    rd3AccelTraceAperture(this, 2, true, kApWhyNew);
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

    struct RD3KVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = RD3K_VRAM_ABI_VERSION;
    r.size    = want;
    IOReturn ret = RDNA3Accelerator::gAccel->askFramebufferForVram(&r, true);
    if (ret != kIOReturnSuccess || !r.handle || !r.phys) {
        OSAddAtomic64(1, &gCntVmRefused);
        rd3AccelTraceVm(this, true, false, 0, want, 0, ret, false);
        ALOG("RD3VidMemory::allocPhysical(%p): %llu bytes REFUSED by the framebuffer (0x%x)",
             this, want, (unsigned)ret);
        return false;
    }
    fRmHandle = r.handle; fKva = r.kva; fPhys = r.phys; fBytes = r.actualSize; fAllocationCookie = r.allocationCookie;
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
    if (fPhys) rd3AccelTraceVm(this, false, true, fPhys, fBytes, 0, kIOReturnSuccess, fScanout);   /* before the records go */
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
            rd3AccelTraceSurf(3, p->surfaceID, p->plane, st, reason, surf, nullptr, nullptr);
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
        rd3AccelTraceSurf(2, p->surfaceID, p->plane, p->status, 0, surf, res, vm);
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
    rd3AccelTraceSurf(3, p->surfaceID, p->plane, p->status, 0, surf, res, vm);
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
        if (rr == kIOReturnSuccess) { p->status = 0; p->kapiMemory = vm->fRmHandle; p->bytes = vm->fBytes; }
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
           rd3SimpleLockOnce(&gA3DispLock) && rd3SimpleLockOnce(&gCfPendLock) &&
           rd3SurfCloseLockOnce() && (rd3TraceInit() || true);   /* no trace memory: the driver runs without it */
}

extern "C" {
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
}
struct RD3IopAsync { void *pipe; IOService *fb; IOSurface *s; struct RD3KFlipRequest f; volatile SInt32 busy; struct thread_call *call;
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
        rd3AccelScPanelMoved(head, "an IOP flip", issued);
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
    if (!e->call) e->call = thread_call_allocate(rd3AccelIopAsyncRun, e);
    if (!e->call) {
        rd3AccelForgetShownIf(head, seq);   /* the caller copies instead: this record is not on the panel */
        return false;
    }
    ((OSObject *)s)->retain();
    e->pipe = pipe; e->fb = fb; e->s = s; e->seq = seq;
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
    /* the head shows this IOSurface from now on (async: unless the flip is
     * dropped or refused, which forget it again) */
    const unsigned long long seq = rd3AccelNotePresented(vm, head);
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
    rd3AccelScPanelMoved(head, "an IOP flip", issued);
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
    const uint64_t issued = mach_absolute_time();
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    OSAddAtomic64(1, &gCntIopFlipHome);
    if (rr == kIOReturnSuccess) rd3AccelScPanelMoved(head, "an IOP flip home", issued);   /* only a flip that was done (round 2) */
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
static bool rd3AccelFlipTo(IOAccelResource2 *res, IOService *fb, unsigned head, RD3FlipWhy *why)
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

    struct RD3KFlipRequest f;
    volatile unsigned char *z = (volatile unsigned char *)&f;
    for (unsigned i = 0; i < sizeof f; i++) z[i] = 0;
    f.version = RD3K_FLIP_ABI_VERSION;
    f.kapiMemory = vm->fRmHandle; f.allocationCookie = vm->fAllocationCookie;
    f.width = w; f.height = h; f.pitch = pitch;

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
static void rd3AccelLatchForgetAll(const char *why)
{
    for (unsigned h = 0; h < kRd3MaxFB; h++) {
        gLatchFrameAsked[h] = false;   /* a new mode: its refresh asked again */
        gLatchFrameTries[h] = 0;
        if (gLatch[h].vm || gLatch[h].prevVm)
            ALOG("swap latch: head %u: the record of phys 0x%llx forgotten (%s)", h, gLatch[h].phys, why);
        rd3AccelLatchForget(h, why);
    }
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
static void rd3AccelLatchVidmemGone(const void *vm)
{
    for (unsigned h = 0; vm && h < kRd3MaxFB; h++) {
        RD3HeadLatch &L = gLatch[h];
        const bool shown = L.vm == vm, leaving = !shown && L.prevVm == vm && !L.st.latched;
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
         "still empty (swap_latch %d, card run 2026-10-17); latched at once %lld, flips failed %lld, waits %s", head, (long long)n, (long long)gCntLatchPend[head],
         (long long)w, w ? (long long)(gCntLatchUs[head] / w) : 0LL, (long long)gCntLatchMax[head],
         (long long)gCntLatchTimeout[head], (long long)gCntLatchUnknown[head], (long long)gCntLatchOff[head],
         (long long)gCntTornChk[head], (long long)gCntTorn[head], (long long)gCntInplace[head],
         (long long)gCntTornSkip[head], (long long)gCntCfOnPanel[head], (long long)gCntShownFreed[head],
         (long long)gCntEmptyFilled[head], (long long)gCntEmptyStill[head], gSwapLatch,
         (long long)gCntLatchImmediate[head], (long long)gCntFlipFailed[head],
         gLatch[head].st.off ? "OFF (timeouts)" : "on");
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
static void rd3AccelScPanelMoved(unsigned head, const char *why, uint64_t issuedAt)
{
    (void)why;
    rd3AccelScPost(head, kScPostAway, issuedAt);   /* not D (a copy into D is safe once this flip latched) */
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
struct RD3Scan { unsigned rc; uint8_t band[8]; };
static void rd3AccelPxScan(const volatile void *kva, unsigned long long bytes, unsigned w, unsigned h, unsigned pitch,
                           RD3Scan *o)
{
    bool any = false;
    const volatile unsigned int *px = (const volatile unsigned int *)kva;
    for (unsigned iy = 1; iy <= 8; iy++) {
        uint32_t row[8];
        for (unsigned ix = 1; ix <= 8; ix++) {
            const unsigned x = (w * ix) / 9, y = (h * iy) / 9;
            uint32_t v = 0;
            if (x < w && y < h && (unsigned long long)y * pitch + x * 4ull + 4 <= bytes)
                v = px[(unsigned long)y * (pitch / 4) + x];
            row[ix - 1] = v;
            any |= v != 0;
        }
        o->band[iy - 1] = rd3_trace_band(row, 8);
    }
    o->rc = any ? kRcPixels : kRcEmpty;
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
static void rd3AccelTracePanel(unsigned head, IOService *fb, unsigned long long expect)
{
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
        if (knownHead < kRd3MaxFB &&
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
        if (act == RD3_SC_COPY_LANDED) {
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
    rd3AccelTracePanel(head, fb, gLatch[head].phys);
    /* the limit of the wait after the flip, from the head's refresh */
    const uint64_t cap = rd3_latch_cap_us(rd3AccelHeadFrameUs(fb, head));
    /* the previous flip latched? the buffer it showed, changed on the panel? */
    rd3AccelLatchEntry(head, fb, n);
    rd3AccelTornCheck(head, src, hashSkip ? nullptr : hash, bufs, nbufs, n);
    RD3FlipWhy fw;
    const uint64_t issued = mach_absolute_time();
    const bool flipped = rd3AccelFlipTo(src, fb, head, &fw);
    {
        struct rd3_trace_ev fe = rd3TrEv(RD3_TR_FLIP, head);
        fe.arg = 0;
        fe.flags = flipped ? 1 : 0;
        fe.v0 = (uint32_t)fw.flipResult;
        fe.v4 = fw.ret;
        fe.pa = phys;
        rd3Trace(fe);
    }
    if (flipped) {
        rd3AccelScPanelMoved(head, "a mode 0 flip", issued);   /* the panel shows WindowServer's buffer, not D */
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
        rd3AccelLatchForget(head, "a flip refused");   /* the framebuffer's own scanout, not a recorded buffer */
        tr.arg = copied ? RD3_TRS_FLIP_COPIED : RD3_TRS_FLIP_FAILED;
        /* a copy into home is no flip: the panel stays where it was (ECC review round 2) */
    }
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
    rd3AccelLatchVidmemGone(vm);
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
    const enum rd3_sc_cf pick = rd3_sc_cf_source(&in);
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
    pick = rd3_cf_source(&in);
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
            rd3AccelLatchForget(h, "the SDMA breaker opened");
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
        tr->v2 = rd3TrSat(us);
        rd3AccelScFail(head, n, RD3_TRX_SDMA,
                       cst == kRd3Etime ? "rdna3CopyVram timed out (the front copy skipped too)"
                                        : "rdna3CopyVram refused the copy", backPhys, (unsigned)cr, tr);
        return;
    }
    tr->v2 = rd3TrSat(us);
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
                            unsigned long long mapped, IOReturn ret, bool scanout)
{
    struct rd3_trace_ev e = rd3TrEv(alloc ? RD3_TR_VMALLOC : RD3_TR_VMFREE);
    e.arg = ok ? 1 : 0;
    e.flags = scanout ? 1 : 0;
    e.obj = rd3TrObj(vm);
    e.v0 = rd3TrSat(bytes >> 10);
    e.v1 = rd3TrSat(mapped >> 20);
    e.v4 = (uint32_t)ret;
    e.pa = phys;
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
        const RD3VidMemory *m = ownD ? nullptr : OSDynamicCast(RD3VidMemory, (OSObject *)vm);
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
            rd3AccelLatchForget(h, "swap mode change");
            gSwapEmptyRun[h] = 0;
            rd3_sc_front_set(&gSc[h].front, nullptr, 0);
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
            "card run 2026-10-17b: 0 flip to WindowServer's back, 1 (default, boot-arg rdna3swapmode) copy it, an "
            "empty back never over a picture, 2 copy every back");
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
RD3ACCEL_CNT4(rdna3accel_swaps_copylanded_would, gCntScLandedWould, "swap mode 1/2: such a landed buffer found but NOT presented (debug.rdna3accel_swap_landed=0): the back's old rule applied");
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
static void rd3AccelInstallSysctl2(void)
{
    sysctl_register_oid(&sysctl__debug_rdna3accel_trace);        sysctl_register_oid(&sysctl__debug_rdna3accel_trace_stream);
    sysctl_register_oid(&sysctl__debug_rdna3accel_trace_sec);    sysctl_register_oid(&sysctl__debug_rdna3accel_trace_dump);
    sysctl_register_oid(&sysctl__debug_rdna3accel_trace_scan);   sysctl_register_oid(&sysctl__debug_rdna3accel_swap_mode);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_ab);      sysctl_register_oid(&sysctl__debug_rdna3accel_swap_trace_events);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_trace_nomem); sysctl_register_oid(&sysctl__debug_rdna3accel_swap_trace_lost);
    RD3ACCEL_REG4(rdna3accel_swaps_copy);      RD3ACCEL_REG4(rdna3accel_swaps_copyempty);
    RD3ACCEL_REG4(rdna3accel_swaps_copylanded); RD3ACCEL_REG4(rdna3accel_swaps_copylanded_would);
    sysctl_register_oid(&sysctl__debug_rdna3accel_swap_landed);
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
