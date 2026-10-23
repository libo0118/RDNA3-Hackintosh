/*
 * Derived from the NullMoth NVIDIA driver for macOS (kexts/NVRMFB/fb/nvrm-fb.cpp)
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * AMD (RDNA3-Hackintosh) port, used with the author's permission for
 * AMD-only work: the IOFramebuffer plumbing (attributes, connection
 * table, aperture/VRAM ranges, VBL delivery, power states, the
 * IONDRVFramebuffer claim) follows NVRMFB; the NVKMS mode setting is
 * replaced by the scanout the VBIOS / UEFI GOP left on DCN 3.2, which
 * RDNA3Accel.kext reads and flips (rdna3DisplayInfo / rdna3DisplayFlip,
 * accel/rdna3k_abi.h). See PROVENANCE.md.
 *
 * RDNA3Framebuffer - one head per instance, the firmware's mode (no mode
 * setting yet). Head 0 matches the IOPCIDevice; with rdna3mirror=2 the core
 * lights a second monitor and publishes an "RDNA3DisplayHead" nub for it,
 * on which another instance starts with a surface of its own in VRAM (an
 * extended desktop at the same mode):
 *   DCN mode       RDNA3Accel reported a usable head: BAR0 aperture of the
 *                  boot scanout, flips to accelerator surfaces, VBL locked
 *                  to the OTG frame counter, CRC.
 *   console mode   no core kext / no usable head: the boot console
 *                  framebuffer (PE_Video), a plain dumb framebuffer like
 *                  IONDRVFramebuffer, VBL from a timer, no flips.
 * Matches in the IOFramebuffer category above Apple's IONDRVFramebuffer
 * (probe 20000), so only one framebuffer publishes the boot VRAM (the
 * NVRMFBClaim lesson). Starts only with the boot-arg rdna3fb=1: probe()
 * declines otherwise and IONDRVFramebuffer keeps the display; if start()
 * fails, IOKit falls back to it too. -rdna3off disables it.
 *
 * DRIVER-PLAN N40 (card run 2026-10-17d: WindowServer stays in the legacy
 * page-flip present mode with us, NullMoth's setups get IOPresentment
 * transactions), each behind a boot-arg read at start and logged in the
 * "switches" line and RDNA3,Switches:
 *   rdna3dispflags=0   kConnectionDisplayFlags 0 (NullMoth 1baebac); 1 = the
 *                      old answer 1, kIODisplayNeedsCEAUnderscan
 *   rdna3glorder=1     head 1's open() opens head 0 first, so
 *                      IOFramebufferOpenGLIndex follows IOFBDependentIndex
 *   rdna3fbintegrated  0 = IOFBIntegrated as IOGraphics sets it (default),
 *                      1 = removed after open(), 2 = set Yes
 *   rdna3cursorasync=1 setCursorState / setCursorImage's moves applied by a
 *                      thread call, off WindowServer's thread (NullMoth)
 *   rdna3fbwake=1      the family's wake work after a system sleep (DidWake,
 *                      pagingState, WindowServer woken), as NullMoth does
 *   rdna3vblonce=1     the VBL thread call never runs twice at once
 *   rdna3fbdtmap=0     validateDetailedTiming answers Unsupported as before,
 *                      logged with whether the timing is one of our modes;
 *                      1 = accepts our modes' timings, UnsupportedMode else
 *   rdna3fbconsole=1   isConsoleDevice: this framebuffer holds the boot
 *                      console's address; 0 = head 0 as before
 */

#include <IOKit/graphics/IOGraphicsTypes.h>
#ifndef detailedTimingModeID
#define detailedTimingModeID __reservedA[0]
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include "../common/rdna3_bootargs.h"
#include "../common/rdna3_fb_policy.h"
#include <libkern/OSAtomic.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include "../accel/rdna3k_abi.h"
#include "../agdc/rdna3_agdc_k5.h"

extern "C" kern_return_t _start(kmod_info_t *ki, void *d);
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(io.github.voten641.RDNA3Framebuffer, "0.1.0", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *, void *) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *, void *) { return KERN_SUCCESS; }

#define FBLOG(fmt, ...) IOLog("RDNA3-fb: " fmt "\n", ##__VA_ARGS__)
#ifndef FB_COMMIT
#define FB_COMMIT "unknown"
#endif
#define FBSEL(a) char _s[5] = { (char)((a) >> 24), (char)((a) >> 16), (char)((a) >> 8), (char)(a), 0 }

static const char kPixelFormats[] = IO32BitDirectPixels "\0";
static const IODisplayModeID kModeId = 1;
static const char kFlipPureFn[] = "rd3FlipToSurfacePure";   /* the accelerator's async/iop flip */

static bool bootArgOn(const char *name)
{
    return rd3_arg(name, 0) != 0;   /* with the default profile (rdna3_bootargs.h) */
}

/* a thread call that re-arms itself (or that a racing kick may enter once
 * more), each invocation holding a reference of 'owner': the owner's stop
 * flag is set first, so a run that sees it re-arms nothing; every
 * invocation cancelled gives its reference back */
static void fbCallDrainFree(OSObject *owner, thread_call_t c)
{
    __sync_synchronize();
    while (thread_call_cancel_wait(c))
        owner->release();
    while (!thread_call_free(c))
        if (thread_call_cancel_wait(c))
            owner->release();
}

/* IOFramebuffer::open() runs under IOGraphics' system gate (newUserClient
 * takes it, IOFramebuffer.cpp ~4008; open() of the dependents runs inside
 * the first one's): head 0's open started by head 1's open() opens head 1
 * again as its dependent, which must not start over. Only touched in the
 * system gate: RDNA3Framebuffer::open() checks it holds the gate and takes
 * it when not (counted, logged), so the reads of 'opened' and this before
 * super::open() are serialised like IOGraphics' own. */
static int gOpenReenter = 0;

/* ------------------------------------------------------------------ */

class RDNA3Framebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(RDNA3Framebuffer)

    IOPCIDevice *fPci = nullptr;
    IOService   *fCore = nullptr;          /* RDNA3Accel */
    bool         fDcn = false;             /* DCN mode (flips, real VBL) */
    unsigned     fHead = 0;
    struct RD3KDisplayHead fInfo = {};
    /* heads > 0: the surface this instance allocated (the head shows the
     * boot surface, a mirror, until we flip it to ours) */
    struct RD3KVramRequest fOwnVram = {};
    bool         fOwnSurface = false;
    bool         ownSurfaceInit();
    void         ownSurfaceFree();

    /* modes: 0 = the timing the head ran at start; then the sink's other
     * timings the core can switch to (rdna3modeset=1, RD3K_DISPLAY_FN_MODES:
     * core = their index in the core's list); then smaller surfaces the DPP
     * scaler stretches over the start timing (rdna3modes=1, core = -1).
     * mode id = kModeId + index */
    struct FbMode { uint32_t w, h; int core; struct RD3KDisplayTiming t; };
    static const uint32_t kMaxModes = 6 + RD3K_MAX_MODES;
    FbMode   fModes[kMaxModes] = {};
    uint32_t fNModes = 1, fCurMode = 0;
    int      fNativeCore = -1;             /* the start timing's index in the core's list */
    uint32_t fNativeRefresh = 60000;
    const OSSymbol *fSymModes = nullptr, *fSymMode = nullptr;
    IOReturn setTiming(uint32_t idx);
    bool     isTiming(uint32_t idx) const { return idx && fModes[idx].core >= 0; }
    /* the native mode's surface, while a scaled one is shown */
    uint64_t fNativePhys = 0, fNativeBytes = 0, fNativeVram = 0;
    uint32_t fNativePitch = 0;
    struct RD3KVramRequest fScaledVram = {};
    bool     fScaledAlloc = false;
    const OSSymbol *fSymSource = nullptr;
    void     modesInit();
    void     scaledFree(struct RD3KVramRequest *v);
    IOReturn switchMode(uint32_t idx);
    uint32_t modePitch(uint32_t idx) const { return idx ? (fModes[idx].w * 4 + 255) & ~255u : fNativePitch; }
    uint32_t modeRefresh(uint32_t idx) const { return isTiming(idx) ? fModes[idx].t.refreshMilliHz : fNativeRefresh; }

    /* the scanout (boot surface): CPU physical, size, geometry */
    uint64_t fPhys = 0, fBytes = 0, fBootVram = 0;
    uint32_t fW = 0, fH = 0, fPitch = 0, fBootPitch = 0;
    uint32_t fRefreshMilliHz = 60000;
    IOMemoryMap *fScanMap = nullptr;       /* kernel mapping for rdna3ScanoutInfo */

    /* VBL */
    struct { IOFBInterruptProc proc; OSObject *target; void *ref; } fVbl = {}, fConnect = {};
    thread_call_t fVblTimer = nullptr;
    uint64_t fVblNext = 0;
    uint32_t fLastFrame = 0;
    unsigned fVblWaits = 0;
    /* N40: VBLs delivered, 250 us re-looks, frames the OTG counter did not
     * move for (12 looks), runs that found another run of vblFire inside */
    uint64_t fVblDelivered = 0, fVblPolls = 0, fVblStill = 0;
    volatile SInt32 fVblIn = 0;
    uint32_t fVblOverlap = 0, fStatTick = 0;
    uint32_t fVblCallFailed = 0;           /* 1: no VBL thread call (no VBLs at all; vblonce not in effect) */
    void     publishStats();

    /* DRIVER-PLAN N40 switches (see the top of the file), read at start */
    uint32_t fArgDispFlags = 0, fArgGlOrder = 1, fArgIntegrated = 0, fArgCurAsync = 1, fArgWake = 1;
    uint32_t fArgVblOnce = 1, fArgDtMap = 0, fArgConsole = 1;
    bool     fStarted = false;             /* super::start() succeeded: head 1's open() may open us first */
    bool     fBootConsole = false;
    bool     fDispFlagsLogged = false, fOpenLogged = false, fOpenFailed = false;
    uint32_t fOpenUngated = 0, fOpenFails = 0;
    uint32_t fIntegLogged = 0;
    uint32_t fDispFlagsAsked = 0;
    RDNA3Framebuffer *copyHead0();
    IOReturn openGated(void *, void *, void *, void *);
    /* IOFBIntegrated A/B: applied after open(); re-checked every ~2 s (the
     * family sets the property in each open) */
    bool     fIntegDone = false;
    uint32_t fIntegApplied = 0, fIntegReset = 0, fIntegLate = 0;
    void     integratedAB(const char *when);
    void     integratedRecheck();
    /* the family's wake work (rdna3fbwake): the WillSleep/DidWake count of
     * IOGraphics' notifications (rd3fb_wake, under fLock); a command gate on
     * the controller's workloop (made at start, taken under fLock only while
     * not stopping, removed at stop) and a thread call that retries a wake
     * work that could not run (drained at stop) */
    IONotifier   *fSleepTap = nullptr;
    IOCommandGate *fWakeGate = nullptr;
    thread_call_t fWakeRetry = nullptr;
    struct rd3fb_wake fWake = {};
    uint32_t fWakeNoGate = 0, fWakeOff = 0, fWakeStopping = 0;
    static IOReturn sleepTap(OSObject *self, void *ref, IOFramebuffer *fb, IOIndex event, void *info);
    void     familyDidWake(const char *why);
    void     wakeFailed(IOReturn r, const char *why, const void *sys, const void *gate);
    static IOReturn wakeSysGated(OSObject *target, void *gate, void *, void *, void *);
    IOReturn familyDidWakeGated(void *gate, void *, void *, void *);
    static void wakeRetryFire(thread_call_param_t p0, thread_call_param_t);
    /* validateDetailedTiming (rdna3fbdtmap) and, with 1, the detailed
     * timing ids setDetailedTimings mapped to our modes */
    uint32_t fDtValidates = 0, fDtMatched = 0, fDtRefused = 0, fDtSets = 0, fDtMapped = 0;
    static const uint32_t kMaxDt = 16;
    IODisplayModeID fDtId[kMaxDt] = {};
    uint32_t fDtIdx[kMaxDt] = {}, fNDt = 0;
    bool     modeIndex(IODisplayModeID mode, uint32_t *idx) const;
    int      dtMatch(const IODetailedTimingInformationV2 *d) const;
    IOSimpleLock *fLock = nullptr;
    /* a mode switch and a flip never interleave: a WindowServer frame of
     * the old size must not reach a head mid-switch (card run 2026-10-12:
     * stripes and a cut picture while the modes changed) */
    IOLock *fModeLock = nullptr;
    volatile SInt32 fStopping = 0;

    /* flips */
    unsigned fFlips = 0, fFlipRejects = 0, fFlipStillPending = 0;
    bool     fFlipped = false;
    /* DRIVER-PLAN N38: flips answered Busy (RD3K_FLIP_FLAG_NOWAIT) and the
     * accelerator's latch waits (rdna3FlipLatchWait), woken from vblFire */
    unsigned fFlipBusy = 0, fLatchWaits = 0, fLatchWaitTimeouts = 0;
    IOLock  *fLatchLock = nullptr;
    int      fLatchEv = 0;                 /* the wakeup's event address */
    IOReturn latchWait(struct RD3KFlipLatchWait *w);

    const OSSymbol *fSymInfo = nullptr, *fSymFlip = nullptr, *fSymStatus = nullptr;
    const OSSymbol *fSymBlank = nullptr;
    int      fPowerOn = 1;                 /* what the core was last told */
    void     powerBlank(bool on, const char *why);
    const OSSymbol *fSymCursor = nullptr, *fSymEdid = nullptr;
    struct RD3KDisplayEdid fEdid = {};
    bool     fEdidAsked = false;
    unsigned fEdidTries = 0;
    IOReturn fEdidLast = kIOReturnSuccess;
    /* rdna3DisplayHotplug, looked at every ~30 VBLs: with rdna3hotplug=1 a
     * new generation of this head's sink drops the cached EDID; that, and
     * a picture lost in VRAM (a wake or reset whose VRAM did not come
     * back), raise IOFramebuffer's connect-change interrupt */
    const OSSymbol *fSymHotplug = nullptr;
    bool     fHpOn = false, fHpPins = false, fConnected = true;
    uint32_t fHpGen = 0, fHpTick = 0, fHpChanges = 0, fLostGen = 0, fLostChanges = 0;
    void     hotplugPoll(bool first);
    void     publishK5Timing();

    /* hardware cursor: 256x256 ARGB8888 premultiplied in VRAM from the core */
    static const uint32_t kCurDim = 256;
    struct RD3KVramRequest fCurVram = {};
    volatile uint32_t *fCurKva = nullptr;
    bool     fCurReady = false, fCurHaveImage = false, fCurVisible = false;
    SInt32   fCurX = 0, fCurY = 0;
    unsigned fCurImages = 0;
    bool     cursorInit();
    void     cursorFree();
    bool     cursorCall(unsigned op);
    bool     cursorCallAt(unsigned op, int32_t x, int32_t y, bool visible);
    /* rdna3cursorasync: the newest state, applied by fCurCall (ONCE, high
     * priority) - the core's cursor call takes its display lock, which
     * flips, VBL status, EDID and the DP link take too (NullMoth's cursor
     * thread call); fCurLock guards fCurQ and the fCur* state */
    IOSimpleLock *fCurLock = nullptr;
    thread_call_t fCurCall = nullptr;
    struct rd3fb_cur_q fCurQ = {};
    uint32_t fCurSync = 0, fCurFailLogged = 0;
    uint32_t fCurImageFails = 0, fCurConvertFails = 0;   /* the core refused the first image; convertCursorImage failed */
    void     cursorWant(bool force);
    static void cursorFire(thread_call_param_t p0, thread_call_param_t);

    IOService *findCore(IOService *provider);
    bool       readCore();
    bool       readConsole();
    uint64_t   vblPeriodAbs() const;
    bool       coreStatus(RD3KDisplayStatus *s);
    bool       flipTo(uint64_t vramOffset, uint32_t w, uint32_t h, uint32_t pitch, int *rc);
    bool       flipToLocked(uint64_t vramOffset, uint32_t w, uint32_t h, uint32_t pitch, int *rc,
                            bool nowait = false);
    void       scheduleVbl(uint64_t deadline);
    static void vblFire(thread_call_param_t p0, thread_call_param_t);

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOReturn enableController() override { return kIOReturnSuccess; }
    const char *getPixelFormats() override { return kPixelFormats; }
    IOItemCount getDisplayModeCount() override { return fNModes; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override
    {
        for (uint32_t i = 0; modes && i < fNModes; i++)
            modes[i] = kModeId + (IODisplayModeID)i;
        return kIOReturnSuccess;
    }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture,
                                 IOPixelInformation *pi) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override
    {
        if (mode) *mode = kModeId + (IODisplayModeID)fCurMode;
        if (depth) *depth = 0;
        return kIOReturnSuccess;
    }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override
    {
        if (mode) *mode = kModeId;
        if (depth) *depth = 0;
        return kIOReturnSuccess;
    }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    IODeviceMemory *getVRAMRange() override;
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    /* the framebuffer whose scanout held the boot console at start
     * (rdna3fbconsole=0: head 0) */
    bool isConsoleDevice(void) override { return fArgConsole ? fBootConsole : fHead == 0; }
    IOReturn open(void) override;
    IOReturn validateDetailedTiming(void *description, IOByteCount descripSize) override;
    IOReturn setDetailedTimings(OSArray *array) override;

    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn getAttributeForConnection(IOIndex, IOSelect attribute, uintptr_t *value) override;
    IOReturn setAttributeForConnection(IOIndex, IOSelect attribute, uintptr_t value) override;
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override
    {
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    /* EDID from the core's DP AUX probe (rdna3DisplayEdid): IOKit builds
     * IODisplayEDID from these blocks, so the monitor gets its name and
     * the right display profile */
    bool hasDDCConnect(IOIndex) override
    {
        bool ok = fetchEdid();
        IOLog("RDNA3FB: hasDDCConnect -> %d\n", ok);
        return ok;
    }
    IOReturn getDDCBlock(IOIndex, UInt32 blockNumber, IOSelect blockType, IOOptionBits,
                         UInt8 *data, IOByteCount *length) override
    {
        if (blockType != kIODDCBlockTypeEDID || !data || !length || !fetchEdid())
            return kIOReturnUnsupported;
        if (!blockNumber || blockNumber * 128 > fEdid.length)
            return kIOReturnNotFound;
        IOByteCount n = *length < 128 ? *length : 128;
        memcpy(data, fEdid.data + (blockNumber - 1) * 128, n);
        *length = n;
        return kIOReturnSuccess;
    }
    bool fetchEdid();
    /* gamma: to the core's MPCC output gamma (on unless rdna3gamma=0); success either way */
    IOReturn setGammaTable(UInt32 channels, UInt32 count, UInt32 width, void *data) override
    {
        return gammaToCore(channels, count, width, data);
    }
    IOReturn setGammaTable(UInt32 channels, UInt32 count, UInt32 width, void *data, bool) override
    {
        return gammaToCore(channels, count, width, data);
    }
    const OSSymbol *fSymGamma = nullptr;
    struct RD3KDisplayGamma *fGamma = nullptr;
    bool     fGammaOff = false;            /* the core said unsupported: stop asking */
    unsigned fGammaCalls = 0;
    IOReturn gammaToCore(UInt32 channels, UInt32 count, UInt32 width, const void *data);
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    /* DCN hardware cursor (DCN mode); software cursor otherwise */
    IOReturn setCursorImage(void *cursorImage) override;
    IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override;

    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target,
                                      void *ref, void **interruptRef) override;
    IOReturn unregisterInterrupt(void *interruptRef) override;
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setPowerState(unsigned long ordinal, IOService *device) override;

    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3,
                                  void *p4) override;
};
OSDefineMetaClassAndStructors(RDNA3Framebuffer, IOFramebuffer)

#define super IOFramebuffer

static IOPMPowerState kFBPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* --------------------------- discovery --------------------------- */

IOService *RDNA3Framebuffer::probe(IOService *provider, SInt32 *score)
{
    char off[8];
    if (PE_parse_boot_argn("-rdna3off", off, sizeof(off)))
        return nullptr;
    if (!bootArgOn("rdna3fb")) {
        FBLOG("boot-arg rdna3fb=1 absent: not starting (IONDRVFramebuffer keeps the display)");
        return nullptr;
    }
    return super::probe(provider, score);
}

IOService *RDNA3Framebuffer::findCore(IOService *provider)
{
    /* RDNA3Accel sits on the same IOPCIDevice; it may still be starting */
    OSDictionary *m = serviceMatching("RDNA3Accel");
    if (!m)
        return nullptr;
    IOService *c = waitForMatchingService(m, 10ull * 1000 * 1000 * 1000);   /* 10 s */
    m->release();
    if (c && c->getProvider() != provider) {
        FBLOG("RDNA3Accel found on another device, ignoring it");
        c->release();
        c = nullptr;
    }
    return c;   /* retained by waitForMatchingService */
}

bool RDNA3Framebuffer::readCore()
{
    if (!fCore || !fSymInfo)
        return false;
    RD3KDisplayInfo *r = (RD3KDisplayInfo *)IOMalloc(sizeof(*r));
    if (!r)
        return false;
    bzero(r, sizeof(*r));
    r->version = RD3K_DISPLAY_ABI_VERSION;
    IOReturn ret = fCore->callPlatformFunction(fSymInfo, false, r, nullptr, nullptr, nullptr);
    bool ok = ret == kIOReturnSuccess && r->nheads > fHead && r->head[fHead].usable &&
              r->head[fHead].format == RD3K_FMT_XRGB8888;
    if (ret == kIOReturnSuccess)
        FBLOG("core reports %u head(s)%s (this is head %u)", r->nheads,
              r->nheads > fHead ? (r->head[fHead].usable ? "" : ", not usable (tiled/DCC/format/outside BAR0)") : "",
              fHead);
    if (ok) {
        fInfo = r->head[fHead];
        fPhys = fInfo.phys;
        fW = fInfo.width;
        fH = fInfo.height;
        fPitch = fBootPitch = fInfo.pitch;
        fBytes = (uint64_t)fPitch * fH;
        fBootVram = fInfo.vramOffset;
        if (fInfo.refreshMilliHz)
            fRefreshMilliHz = fInfo.refreshMilliHz;
    }
    IOFree(r, sizeof(*r));
    return ok;
}

bool RDNA3Framebuffer::readConsole()
{
    PE_Video v;
    bzero(&v, sizeof(v));
    IOPlatformExpert *pe = getPlatform();
    if (!pe || pe->getConsoleInfo(&v) != kIOReturnSuccess || !v.v_baseAddr)
        return false;
    if (v.v_depth != 32 || !v.v_width || !v.v_height || v.v_rowBytes < v.v_width * 4) {
        FBLOG("boot console %lux%lu depth %lu: not a 32 bpp console", (unsigned long)v.v_width,
              (unsigned long)v.v_height, (unsigned long)v.v_depth);
        return false;
    }
    /* the low bits are not address: IOGraphics hands the console over
     * with bit 0 set (0x80000001 on the card's BAR), and a 32 bpp frame
     * starts on a pixel. Taken as is, every pixel lands one byte off:
     * alpha in blue, black shown blue (card run 2026-10-12, console mode). */
    fPhys = (uint64_t)v.v_baseAddr & ~(uint64_t)3;
    if (v.v_baseAddr & 3)
        FBLOG("boot console base 0x%llx carries flag bits 0x%llx: using 0x%llx", (unsigned long long)v.v_baseAddr,
              (unsigned long long)(v.v_baseAddr & 3), (unsigned long long)fPhys);
    fW = (uint32_t)v.v_width;
    fH = (uint32_t)v.v_height;
    fPitch = fBootPitch = (uint32_t)v.v_rowBytes;
    fBytes = (uint64_t)fPitch * fH;
    return true;
}

/* ----------------------------- start ----------------------------- */

bool RDNA3Framebuffer::start(IOService *provider)
{
    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) {
        /* a head nub of the core (RDNA3Accel -> RDNA3DisplayHead) */
        OSNumber *h = OSDynamicCast(OSNumber, provider->getProperty("RDNA3,Head"));
        IOService *core = provider->getProvider();
        if (!h || !core || !h->unsigned32BitValue())
            return false;
        fHead = h->unsigned32BitValue();
        fPci = OSDynamicCast(IOPCIDevice, core->getProvider());
        if (!fPci)
            return false;
        core->retain();
        fCore = core;
    }
    fLock = IOSimpleLockAlloc();
    fModeLock = IOLockAlloc();
    fLatchLock = IOLockAlloc();   /* NULL: rdna3FlipLatchWait answers at once (the accelerator polls) */
    fSymInfo = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_INFO);
    fSymFlip = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_FLIP);
    fSymBlank = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_BLANK);
    fSymSource = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_SOURCE);
    fSymModes = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_MODES);
    fSymMode = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_MODE);
    fSymStatus = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_STATUS);
    fSymCursor = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_CURSOR);
    fSymEdid = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_EDID);
    if (!fLock || !fModeLock || !fSymInfo || !fSymFlip || !fSymStatus || !fSymCursor || !fSymEdid)
        return false;
    fArgDispFlags = rd3_arg("rdna3dispflags", 0);
    fArgGlOrder = rd3_arg("rdna3glorder", 1);
    fArgIntegrated = rd3_arg("rdna3fbintegrated", 0);
    fArgCurAsync = rd3_arg("rdna3cursorasync", 1);
    fArgWake = rd3_arg("rdna3fbwake", 1);
    fArgVblOnce = rd3_arg("rdna3vblonce", 1);
    fArgDtMap = rd3_arg("rdna3fbdtmap", 0);
    fArgConsole = rd3_arg("rdna3fbconsole", 1);

    if (!fCore)
        fCore = findCore(provider);
    fDcn = readCore();
    if (fHead && (!fDcn || !ownSurfaceInit())) {
        FBLOG("head %u: no usable scanout or no VRAM for its surface: not starting", fHead);
        return false;
    }
    if (!fDcn && !readConsole()) {
        /* super::start() not called yet: nothing to unwind, and with no
         * framebuffer at all the IONDRV claim would leave a black screen,
         * so say it loudly */
        FBLOG("no usable scanout (core %s, boot console unusable): refusing to start",
              fCore ? "present" : "absent");
        return false;
    }
    FBLOG("%s mode: %ux%u pitch %u at phys 0x%llx, %u.%03u Hz", fDcn ? "DCN" : "console", fW, fH,
          fPitch, (unsigned long long)fPhys, fRefreshMilliHz / 1000, fRefreshMilliHz % 1000);
    {
        /* the boot console: the scanout that holds its address now, before
         * IOGraphics takes it over (heads > 0 show a surface of their own) */
        PE_Video v;
        bzero(&v, sizeof(v));
        IOPlatformExpert *pe = getPlatform();
        const bool known = pe && pe->getConsoleInfo(&v) == kIOReturnSuccess && v.v_baseAddr;
        fBootConsole = rd3fb_owns_console(known, known ? (uint64_t)v.v_baseAddr : 0, fPhys, fBytes, fHead) != 0;
        FBLOG("head %u: boot console %s: %s (rdna3fbconsole=%u, default 1; 0 = head 0 as before)", fHead,
              known ? "at" : "unknown", fBootConsole ? "this framebuffer holds it" : "not this framebuffer", fArgConsole);
        if (known)
            FBLOG("head %u: console base 0x%llx, scanout 0x%llx + 0x%llx", fHead, (unsigned long long)v.v_baseAddr,
                  (unsigned long long)fPhys, (unsigned long long)fBytes);
        if (fArgConsole && !fHead && !fBootConsole)
            FBLOG("head 0 does NOT hold the boot console: isConsoleDevice answers no (rdna3fbconsole=0 restores yes)");
    }
    /* the core probed the connectors before it registered: ask for the
     * EDID now, so IOGraphics' first HLDDC sense / hasDDCConnect question
     * (asked once, while the display is attached) already gets a yes */
    if (fDcn)
        fetchEdid();
    modesInit();
    fSymHotplug = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_HOTPLUG);
    if (fDcn)
        hotplugPoll(true);   /* the picture-lost count always, the pins with rdna3hotplug=1 */

    setProperty("IOFBDependentID", (unsigned long long)0x5244334600000001ull, 64);  /* 'RD3F' */
    setProperty("IOFBDependentIndex", (unsigned long long)fHead, 32);
    publishK5Timing();
    setProperty("IOFBMemorySize", (unsigned long long)fBytes, 64);
    setProperty("RDNA3,Mode", fDcn ? "dcn" : "console");
    setProperty("RDNA3,FBSource", FB_COMMIT);
    /* IOFramebuffer hands gamma tables to setGammaTable only when the
     * framebuffer names their format (card run 26: no call without these,
     * the table went nowhere): 256 entries of 12 bits per channel, what the
     * core resamples onto the MPCC OGAM curve; on unless rdna3gamma=0 */
    if (fDcn && rd3_gamma_on()) {
        setProperty(kIOFBGammaWidthKey, 12ull, 32);
        setProperty(kIOFBGammaCountKey, 256ull, 32);
        setProperty("IOFBGammaHeaderSize", 0ull, 32);
    }
    setName("RDNA3Framebuffer");

    if (!super::start(provider)) {
        FBLOG("IOFramebuffer::start failed");
        return false;
    }
    fStarted = true;
    /* ONCE: a re-arm while vblFire still runs (a slow status call under the
     * core's display lock) queues one more run instead of a second vblFire
     * beside it (NullMoth #295); both run at THREAD_CALL_PRIORITY_HIGH */
    fVblTimer = fArgVblOnce ? thread_call_allocate_with_options(vblFire, this, THREAD_CALL_PRIORITY_HIGH,
                                                                THREAD_CALL_OPTIONS_ONCE)
                            : thread_call_allocate(vblFire, this);
    if (!fVblTimer) {
        fVblCallFailed = 1;
        FBLOG("head %u: no VBL thread call (rdna3vblonce=%u): no VBL interrupts for IOGraphics at all", fHead,
              fArgVblOnce);
    }
    if (fVblTimer) {
        uint64_t d;
        clock_interval_to_deadline(16667, kMicrosecondScale, &d);
        scheduleVbl(d);
    }
    /* the family's wake work (rdna3fbwake): its gate on the controller's
     * workloop and its retry, ready before the first setPowerState */
    if (fArgWake) {
        IOWorkLoop *wl = getWorkLoop();
        IOCommandGate *g = wl ? IOCommandGate::commandGate(this) : nullptr;
        if (g && wl->addEventSource(g) != kIOReturnSuccess)
            OSSafeReleaseNULL(g);
        fWakeGate = g;
        fWakeRetry = thread_call_allocate(wakeRetryFire, this);
        if (!fWakeGate || !fWakeRetry)
            FBLOG("head %u: family wake work: controller workloop %p, command gate %p, retry call %p: a system wake "
                  "%s", fHead, wl, fWakeGate, fWakeRetry,
                  fWakeGate ? "is not retried when it fails" : "cannot do the family's wake work (WindowServer may wait for paging)");
    }
    registerPowerDriver(this, kFBPowerStates, 2);
    if (fDcn)
        cursorInit();
    /* the WillSleep / DidWake count for the family's wake work; passive */
    fSleepTap = addFramebufferNotification(&RDNA3Framebuffer::sleepTap, this, nullptr);
    if (!fSleepTap)
        FBLOG("head %u: no framebuffer notification tap: a system wake cannot do the family's wake work", fHead);
    FBLOG("started (head %u, VBL %s, %s cursor), build %s", fHead,
          fDcn ? "locked to the OTG frame counter" : "timer", fCurReady ? "hardware" : "software", FB_COMMIT);
    /* DRIVER-PLAN N40: every switch with its default, for the A/B */
    FBLOG("head %u switches: dispflags %u (default 0), glorder %u (default 1), fbintegrated %u (default 0), "
          "cursorasync %u (default 1%s), fbwake %u (default 1), vblonce %u (default 1%s), fbdtmap %u (default 0), "
          "fbconsole %u (default 1); VBL source: %s (no hardware vblank interrupt in the core yet)",
          fHead, fArgDispFlags, fArgGlOrder, fArgIntegrated, fArgCurAsync, !fCurReady ? ", no hardware cursor" : fCurCall ? "" : ", no thread call: synchronous",
          fArgWake, fArgVblOnce, fVblTimer ? "" : ", NO VBL thread call", fArgDtMap, fArgConsole,
          fDcn ? "OTG frame counter polled by a thread call" : "timer");
    /* the switches as asked (the boot-arg or its default; triage compares
     * them with the boot-args) and, where they can differ, what is in
     * effect: cursorasync_active (a thread call moves the cursor), hwcursor,
     * vbl_call / vblonce_active (the VBL thread call exists, ONCE), waketap /
     * wakegate (the family wake work can see sleeps and run) */
    OSDictionary *sw = OSDictionary::withCapacity(16);
    if (sw) {
        const struct { const char *k; uint32_t v; } kv[] = {
            { "dispflags", fArgDispFlags }, { "glorder", fArgGlOrder }, { "fbintegrated", fArgIntegrated },
            { "cursorasync", fArgCurAsync }, { "cursorasync_active", fCurCall ? 1u : 0u },
            { "hwcursor", fCurReady ? 1u : 0u }, { "fbwake", fArgWake }, { "waketap", fSleepTap ? 1u : 0u },
            { "wakegate", fWakeGate ? 1u : 0u }, { "vblonce", fArgVblOnce }, { "vbl_call", fVblTimer ? 1u : 0u },
            { "vblonce_active", fArgVblOnce && fVblTimer ? 1u : 0u }, { "fbdtmap", fArgDtMap },
            { "fbconsole", fArgConsole }, { "bootconsole", fBootConsole ? 1u : 0u },
        };
        for (unsigned i = 0; i < sizeof(kv) / sizeof(kv[0]); i++) {
            OSNumber *n = OSNumber::withNumber((unsigned long long)kv[i].v, 32);
            if (n) {
                sw->setObject(kv[i].k, n);
                n->release();
            }
        }
        setProperty("RDNA3,Switches", sw);
        sw->release();
    }
    setProperty("RDNA3,VBLSource", fDcn ? "otg-poll" : "timer");
    {
        /* replaced after open(); this one left means it was never opened */
        char txt[80];
        snprintf(txt, sizeof(txt), "never opened (rdna3fbintegrated=%u)", fArgIntegrated);
        setProperty("RDNA3,IntegratedAB", txt);
    }
    publishStats();
    return true;
}

void RDNA3Framebuffer::scheduleVbl(uint64_t deadline)
{
    if (!fVblTimer || !fLock)
        return;
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(fLock);
    if (!fStopping) {
        retain();
        if (thread_call_enter_delayed(fVblTimer, deadline))
            release();   /* was already queued: that one holds the reference */
    }
    IOSimpleLockUnlockEnableInterrupt(fLock, st);
}

void RDNA3Framebuffer::stop(IOService *provider)
{
    if (fLock) {
        IOInterruptState st = IOSimpleLockLockDisableInterrupt(fLock);
        fStopping = 1;
        IOSimpleLockUnlockEnableInterrupt(fLock, st);
    }
    fStarted = false;
    if (fSleepTap) {
        fSleepTap->remove();
        fSleepTap = nullptr;
    }
    if (fWakeRetry) {
        fbCallDrainFree(this, fWakeRetry);   /* a retry still running finishes; none re-arms (fStopping) */
        fWakeRetry = nullptr;
    }
    if (fWakeGate) {
        /* familyDidWake takes the gate (retained) under fLock only while
         * fStopping is clear: none starts now. removeEventSource closes the
         * controller gate, so a wake work inside it finishes first; a caller
         * still holding its reference gets NotReady from runAction */
        IOCommandGate *g = nullptr;
        IOInterruptState st = IOSimpleLockLockDisableInterrupt(fLock);
        g = fWakeGate;
        fWakeGate = nullptr;
        IOSimpleLockUnlockEnableInterrupt(fLock, st);
        IOWorkLoop *wl = g->getWorkLoop();
        if (wl)
            wl->removeEventSource(g);
        g->release();
    }
    if (fVblTimer) {
        fbCallDrainFree(this, fVblTimer);   /* waits for a vblFire still running */
        fVblTimer = nullptr;
    }
    if (fCurCall) {
        fbCallDrainFree(this, fCurCall);
        fCurCall = nullptr;
    }
    if (fCurMode) {
        IOLockLock(fModeLock);
        switchMode(0);   /* the native timing's own surface again, scaler off */
        IOLockUnlock(fModeLock);
    }
    if (fFlipped && fDcn) {
        int rc;
        flipTo(fBootVram, fW, fH, fBootPitch, &rc);   /* leave the boot surface on screen */
    }
    ownSurfaceFree();
    cursorFree();
    publishStats();   /* the last counters stay in the registry for a report taken after */
    super::stop(provider);
}

void RDNA3Framebuffer::free()
{
    OSSafeReleaseNULL(fScanMap);
    OSSafeReleaseNULL(fCore);
    OSSafeReleaseNULL(fSymInfo);
    OSSafeReleaseNULL(fSymFlip);
    OSSafeReleaseNULL(fSymBlank);
    OSSafeReleaseNULL(fSymSource);
    OSSafeReleaseNULL(fSymModes);
    OSSafeReleaseNULL(fSymMode);
    OSSafeReleaseNULL(fSymStatus);
    OSSafeReleaseNULL(fSymCursor);
    OSSafeReleaseNULL(fSymEdid);
    OSSafeReleaseNULL(fSymHotplug);
    OSSafeReleaseNULL(fSymGamma);
    if (fGamma) {
        IOFree(fGamma, sizeof(*fGamma));
        fGamma = nullptr;
    }
    if (fLock) {
        IOSimpleLockFree(fLock);
        fLock = nullptr;
    }
    if (fModeLock) {
        IOLockFree(fModeLock);
        fModeLock = nullptr;
    }
    if (fLatchLock) {
        IOLockFree(fLatchLock);
        fLatchLock = nullptr;
    }
    if (fCurLock) {
        IOSimpleLockFree(fCurLock);
        fCurLock = nullptr;
    }
    super::free();
}

/* ------------------------------ VBL ------------------------------ */

uint64_t RDNA3Framebuffer::vblPeriodAbs() const
{
    uint64_t ns = 1000000000000ull / (fRefreshMilliHz ? fRefreshMilliHz : 60000);
    if (ns < 4000000ull || ns > 50000000ull)
        ns = 16666667ull;
    uint64_t abs = 0;
    nanoseconds_to_absolutetime(ns, &abs);
    return abs;
}

bool RDNA3Framebuffer::coreStatus(RD3KDisplayStatus *s)
{
    if (!fDcn || !fCore)
        return false;
    bzero(s, sizeof(*s));
    s->version = RD3K_DISPLAY_ABI_VERSION;
    s->head = fHead;
    return fCore->callPlatformFunction(fSymStatus, false, s, nullptr, nullptr, nullptr) == kIOReturnSuccess;
}

/* DCN mode: deliver VBL when the OTG frame counter moves (polled around
 * the expected time, at most ~3 ms late); console mode: free-running timer */
void RDNA3Framebuffer::vblFire(thread_call_param_t p0, thread_call_param_t)
{
    RDNA3Framebuffer *me = (RDNA3Framebuffer *)p0;
    if (me->fStopping) {
        me->release();
        return;
    }
    /* two runs at once (rdna3vblonce=0 and a re-arm that fired while this
     * one waited for the core): counted, the evidence for the switch */
    if (OSIncrementAtomic(&me->fVblIn) > 0)
        me->fVblOverlap++;
    uint64_t now = 0, period = me->vblPeriodAbs();
    clock_get_uptime(&now);
    RD3KDisplayStatus st;
    if (me->coreStatus(&st)) {
        if (st.frame == me->fLastFrame && me->fVblWaits < 12) {
            /* not yet: look again in 250 us */
            me->fVblWaits++;
            me->fVblPolls++;
            uint64_t d;
            clock_interval_to_deadline(250, kMicrosecondScale, &d);
            OSDecrementAtomic(&me->fVblIn);
            me->scheduleVbl(d);
            me->release();
            return;
        }
        if (st.frame == me->fLastFrame)
            me->fVblStill++;   /* 12 looks and the counter did not move: delivered anyway */
        me->fLastFrame = st.frame;
        me->fVblWaits = 0;
        /* a new frame: a pending flip may have latched (rdna3FlipLatchWait) */
        if (me->fLatchLock)
            IOLockWakeup(me->fLatchLock, &me->fLatchEv, false);
        /* next look a little before the next frame */
        me->fVblNext = now + period - period / 8;
    } else {
        if (me->fVblNext == 0)
            me->fVblNext = now;
        me->fVblNext += period;
        if (me->fVblNext <= now)
            me->fVblNext = now + period;
    }
    if (me->fVbl.proc)
        me->fVbl.proc(me->fVbl.target, me->fVbl.ref);
    me->fVblDelivered++;
    if (me->fHpOn && ++me->fHpTick >= 30) {
        me->fHpTick = 0;
        me->hotplugPoll(false);
    }
    /* the N40 counters into the registry about every 2 s */
    if (++me->fStatTick >= 120) {
        me->fStatTick = 0;
        me->integratedRecheck();
        me->publishStats();
    }
    OSDecrementAtomic(&me->fVblIn);
    me->scheduleVbl(me->fVblNext);
    me->release();
}

IOReturn RDNA3Framebuffer::registerForInterruptType(IOSelect type, IOFBInterruptProc proc,
                                                    OSObject *target, void *ref, void **interruptRef)
{
    if (type == kIOFBVBLInterruptType) {
        fVbl.proc = proc; fVbl.target = target; fVbl.ref = ref;
        if (interruptRef) *interruptRef = &fVbl;
        return kIOReturnSuccess;
    }
    if (type == kIOFBConnectInterruptType) {
        fConnect.proc = proc; fConnect.target = target; fConnect.ref = ref;
        if (interruptRef) *interruptRef = &fConnect;
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

IOReturn RDNA3Framebuffer::unregisterInterrupt(void *interruptRef)
{
    if (interruptRef == &fVbl) fVbl.proc = nullptr;
    if (interruptRef == &fConnect) fConnect.proc = nullptr;
    return kIOReturnSuccess;
}

/* ----------------------------- modes ----------------------------- */

/* the native mode plus, with rdna3modes=1 on a DCN head, the sizes the
 * DPP scaler stretches to it: 5/6, 3/4, 2/3 and 1/2 of the native size
 * (multiples of 8 x 2, the aspect kept) */
void RDNA3Framebuffer::modesInit()
{
    uint32_t want = 0;
    fModes[0].w = fW;
    fModes[0].h = fH;
    fModes[0].core = -1;
    fNModes = 1;
    fNativeCore = -1;
    fNativePhys = fPhys;
    fNativeBytes = fBytes;
    fNativeVram = fBootVram;
    fNativePitch = fBootPitch;
    fNativeRefresh = fRefreshMilliHz;
    if (!fDcn)
        return;
    /* the sink's own timings (rdna3modeset=1): the core lists what it can
     * drive on this head, the running one flagged */
    if (fSymModes && fCore) {
        struct RD3KDisplayModes *ml = (struct RD3KDisplayModes *)IOMalloc(sizeof(*ml));
        if (ml) {
            bzero(ml, sizeof(*ml));
            ml->version = RD3K_DISPLAY_ABI_VERSION;
            ml->head = fHead;
            IOReturn r = fCore->callPlatformFunction(fSymModes, false, ml, nullptr, nullptr, nullptr);
            for (uint32_t i = 0; r == kIOReturnSuccess && i < ml->count && i < RD3K_MAX_MODES; i++) {
                const struct RD3KDisplayTiming *t = &ml->mode[i];
                if (t->flags & RD3K_MODE_CURRENT) {
                    fNativeCore = (int)i;
                    fModes[0].t = *t;
                    continue;
                }
                if (fNModes >= kMaxModes)
                    break;
                fModes[fNModes].w = t->width;
                fModes[fNModes].h = t->height;
                fModes[fNModes].core = (int)i;
                fModes[fNModes].t = *t;
                fNModes++;
            }
            FBLOG("modes from the core: %u timings (0x%x)%s", fNModes - 1, (unsigned)r,
                  fNativeCore < 0 && fNModes > 1 ? ", the running one not among them: no switching" : "");
            if (fNativeCore < 0)
                fNModes = 1;   /* no way back to the start timing: offer none */
            IOFree(ml, sizeof(*ml));
        }
    }
    want = rd3_arg("rdna3modes", want);
    if (!want || !fSymSource)
        return;
    static const uint32_t kNum[] = { 5, 3, 2, 1 }, kDen[] = { 6, 4, 3, 2 };
    for (uint32_t i = 0; i < 4 && fNModes < kMaxModes; i++) {
        uint32_t w = (fW * kNum[i] / kDen[i]) & ~7u, h = ((uint64_t)w * fH / fW) & ~1u, k, dup = 0;
        for (k = 1; k < fNModes; k++)
            dup |= fModes[k].w == w && fModes[k].h == h;   /* a real timing of that size wins */
        if (w >= 640 && h >= 360 && w < fW && !dup) {
            fModes[fNModes].w = w;
            fModes[fNModes].h = h;
            fModes[fNModes].core = -1;
            fNModes++;
        }
    }
    /* kIOFBScalerInfoKey: the OS may program scaled detailed timings too */
    IODisplayScalerInformation si;
    bzero(&si, sizeof(si));
    si.scalerFeatures = kIOScaleCanUpSamplePixels | kIOScaleStretchOnly;
    si.maxHorizontalPixels = fW;
    si.maxVerticalPixels = fH;
    setProperty(kIOFBScalerInfoKey, &si, sizeof(si));
    FBLOG("%u modes: native %ux%u + %ux%u .. %ux%u", fNModes, fW, fH, fModes[1].w, fModes[1].h,
          fModes[fNModes - 1].w, fModes[fNModes - 1].h);
}

void RDNA3Framebuffer::scaledFree(struct RD3KVramRequest *v)
{
    const OSSymbol *fr = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_FREE);
    if (fr && fCore)
        fCore->callPlatformFunction(fr, false, v, nullptr, nullptr, nullptr);
    OSSafeReleaseNULL(fr);
}

/* a scaled mode: a cleared surface of the mode's size, the core points the
 * head's HUBP at it and turns the DPP scaler on (rdna3DisplaySetSource);
 * back to mode 0 shows the native surface with the scaler bypassed */
IOReturn RDNA3Framebuffer::switchMode(uint32_t idx)
{
    struct RD3KVramRequest nv;
    struct RD3KDisplaySource src;
    const uint32_t w = fModes[idx].w, h = fModes[idx].h, pitch = modePitch(idx);
    /* timing <-> scaled goes through the start timing: the scaler works on
     * that one only */
    if (isTiming(idx) || isTiming(fCurMode)) {
        if (fCurMode && !isTiming(fCurMode)) {
            IOReturn r0 = switchMode(0);
            if (r0 != kIOReturnSuccess)
                return r0;
        }
        if (idx && !isTiming(idx)) {
            IOReturn r0 = setTiming(0);
            if (r0 != kIOReturnSuccess)
                return r0;
        } else {
            return setTiming(idx);
        }
    }
    bzero(&nv, sizeof(nv));
    if (idx) {
        const OSSymbol *alloc = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_ALLOC);
        nv.version = RD3K_VRAM_ABI_VERSION;
        nv.flags = RD3K_VRAM_OWNER_FB_SURFACE;
        nv.size = (unsigned long long)pitch * h;
        IOReturn r = alloc && fCore ? fCore->callPlatformFunction(alloc, false, &nv, nullptr, nullptr, nullptr)
                                    : kIOReturnNoResources;
        OSSafeReleaseNULL(alloc);
        if (r != kIOReturnSuccess || !nv.phys) {
            FBLOG("mode %ux%u: no VRAM for the surface (0x%x)", w, h, (unsigned)r);
            return kIOReturnNoMemory;
        }
        if (nv.kva)
            bzero(nv.kva, (size_t)nv.size);
    }
    bzero(&src, sizeof(src));
    src.version = RD3K_DISPLAY_ABI_VERSION;
    src.head = fHead;
    src.width = w;
    src.height = h;
    src.pitch = pitch;
    src.vramOffset = idx ? nv.allocationCookie : fNativeVram;
    IOReturn r = fCore->callPlatformFunction(fSymSource, false, &src, nullptr, nullptr, nullptr);
    if (r != kIOReturnSuccess) {
        FBLOG("mode %ux%u: the core refused the source (0x%x, %d)", w, h, (unsigned)r, src.result);
        if (idx)
            scaledFree(&nv);
        return kIOReturnUnsupportedMode;
    }
    /* the old scaled surface goes once the head shows the new one */
    if (fScaledAlloc) {
        scaledFree(&fScaledVram);
        fScaledAlloc = false;
    }
    if (idx) {
        fScaledVram = nv;
        fScaledAlloc = true;
        fPhys = nv.phys;
        fBootVram = nv.allocationCookie;
        fBytes = (uint64_t)pitch * h;
    } else {
        fPhys = fNativePhys;
        fBootVram = fNativeVram;
        fBytes = fNativeBytes;
    }
    fW = w;
    fH = h;
    fPitch = fBootPitch = pitch;
    fFlipped = false;
    fCurMode = idx;
    OSSafeReleaseNULL(fScanMap);   /* rdna3ScanoutInfo maps the new surface */
    FBLOG("mode %u: %ux%u pitch %u at VRAM 0x%llx (%s)", idx, w, h, pitch, (unsigned long long)fBootVram,
          idx ? "scaled to the native timing" : "native");
    return kIOReturnSuccess;
}

/* a real timing (or back to the start one, idx 0): a surface of the new
 * size, the core switches the head's timing to it (rdna3DisplaySetMode)
 * and puts the old one back itself when the new one does not run */
IOReturn RDNA3Framebuffer::setTiming(uint32_t idx)
{
    struct RD3KVramRequest nv;
    struct RD3KDisplayMode m;
    const uint32_t w = fModes[idx].w, h = fModes[idx].h, pitch = modePitch(idx);
    int core = idx ? fModes[idx].core : fNativeCore;
    if (!fSymMode || !fCore || core < 0)
        return kIOReturnUnsupportedMode;
    bzero(&nv, sizeof(nv));
    if (idx) {
        const OSSymbol *alloc = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_ALLOC);
        nv.version = RD3K_VRAM_ABI_VERSION;
        nv.flags = RD3K_VRAM_OWNER_FB_SURFACE;
        nv.size = (unsigned long long)pitch * h;
        IOReturn r = alloc ? fCore->callPlatformFunction(alloc, false, &nv, nullptr, nullptr, nullptr)
                           : kIOReturnNoResources;
        OSSafeReleaseNULL(alloc);
        if (r != kIOReturnSuccess || !nv.phys) {
            FBLOG("timing %ux%u: no VRAM for the surface (0x%x)", w, h, (unsigned)r);
            return kIOReturnNoMemory;
        }
        if (nv.kva)
            bzero(nv.kva, (size_t)nv.size);
    }
    bzero(&m, sizeof(m));
    m.version = RD3K_DISPLAY_ABI_VERSION;
    m.head = fHead;
    m.index = (unsigned)core;
    m.pitch = pitch;
    m.vramOffset = idx ? nv.allocationCookie : fNativeVram;
    m.oldPitch = fBootPitch;
    m.oldVramOffset = fBootVram;
    IOReturn r = fCore->callPlatformFunction(fSymMode, false, &m, nullptr, nullptr, nullptr);
    if (r != kIOReturnSuccess) {
        FBLOG("timing %ux%u: the core could not switch (0x%x, %d): the old one stays", w, h, (unsigned)r, m.result);
        if (idx)
            scaledFree(&nv);
        return kIOReturnUnsupportedMode;
    }
    if (fScaledAlloc) {
        scaledFree(&fScaledVram);
        fScaledAlloc = false;
    }
    if (idx) {
        fScaledVram = nv;
        fScaledAlloc = true;
        fPhys = nv.phys;
        fBootVram = nv.allocationCookie;
        fBytes = (uint64_t)pitch * h;
    } else {
        fPhys = fNativePhys;
        fBootVram = fNativeVram;
        fBytes = fNativeBytes;
    }
    fW = w;
    fH = h;
    fPitch = fBootPitch = pitch;
    fRefreshMilliHz = modeRefresh(idx);
    fFlipped = false;
    fCurMode = idx;
    OSSafeReleaseNULL(fScanMap);
    FBLOG("mode %u: %ux%u at %u.%03u Hz, pitch %u at VRAM 0x%llx (own timing)", idx, w, h, fRefreshMilliHz / 1000,
          fRefreshMilliHz % 1000, pitch, (unsigned long long)fBootVram);
    return kIOReturnSuccess;
}

/* our mode ids (kModeId + index) and, with rdna3fbdtmap=1, the detailed
 * timing ids setDetailedTimings mapped to one of our modes */
bool RDNA3Framebuffer::modeIndex(IODisplayModeID mode, uint32_t *idx) const
{
    if (mode >= kModeId && (uint32_t)(mode - kModeId) < fNModes) {
        *idx = (uint32_t)(mode - kModeId);
        return true;
    }
    for (uint32_t i = 0; i < fNDt; i++)
        if (fDtId[i] == mode && fDtIdx[i] < fNModes) {
            *idx = fDtIdx[i];
            return true;
        }
    return false;
}

IOReturn RDNA3Framebuffer::setDisplayMode(IODisplayModeID mode, IOIndex depth)
{
    uint32_t idx = 0;
    if (!modeIndex(mode, &idx) || depth != 0)
        return kIOReturnUnsupportedMode;
    if (idx == fCurMode)
        return kIOReturnSuccess;
    IOLockLock(fModeLock);
    IOReturn r = switchMode(idx);
    if (r == kIOReturnSuccess)
        publishK5Timing();
    IOLockUnlock(fModeLock);
    return r;
}

/* the current mode's timing for RDNA3AGDC (its kAGDCLinkConfig and
 * plane-scaler answers, agdc/rdna3_agdc_k5.h); none in console mode */
void RDNA3Framebuffer::publishK5Timing()
{
    IOTimingInformation ti;
    uint32_t t[RD3K5_TIMING_WORDS];
    if (getTimingInfoForDisplayMode(kModeId + fCurMode, &ti) != kIOReturnSuccess || !(ti.flags & kIODetailedTimingValid)) {
        removeProperty(RD3K5_TIMING_PROP);
        return;
    }
    const IODetailedTimingInformationV2 &d = ti.detailedInfo.v2;
    int why = rd3k5_timing_from(t, d.pixelClock, d.horizontalActive, d.horizontalBlanking, d.horizontalSyncOffset,
                                d.horizontalSyncPulseWidth, d.verticalActive, d.verticalBlanking, d.verticalSyncOffset,
                                d.verticalSyncPulseWidth, modeRefresh(fCurMode), d.horizontalSyncConfig != 0,
                                d.verticalSyncConfig != 0);
    if (why) {
        FBLOG("head %u: mode %u has no usable timing for AGDC (%d)", fHead, fCurMode, why);
        removeProperty(RD3K5_TIMING_PROP);
        return;
    }
    OSData *o = OSData::withBytes(t, sizeof(t));
    if (o) {
        setProperty(RD3K5_TIMING_PROP, o);
        o->release();
    }
}

IOReturn RDNA3Framebuffer::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
    uint32_t idx = 0;
    if (!info || !modeIndex(mode, &idx))
        return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->nominalWidth = fModes[idx].w;
    info->nominalHeight = fModes[idx].h;
    info->refreshRate = (UInt32)(((UInt64)modeRefresh(idx) << 16) / 1000);
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | (idx ? 0 : kDisplayModeDefaultFlag);
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info)
{
    uint32_t idx = 0;
    if (!info || !modeIndex(mode, &idx))
        return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    info->appleTimingID = kIOTimingIDInvalid;
    if (isTiming(idx)) {   /* a timing of its own, from the sink's EDID */
        const struct RD3KDisplayTiming &m = fModes[idx].t;
        IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
        d.pixelClock = d.minPixelClock = d.maxPixelClock = (UInt64)m.pclkKhz * 1000;
        d.horizontalActive = m.width;
        d.horizontalBlanking = m.hTotal - m.width;
        d.horizontalSyncOffset = m.hFront;
        d.horizontalSyncPulseWidth = m.hSync;
        d.verticalActive = m.height;
        d.verticalBlanking = m.vTotal - m.height;
        d.verticalSyncOffset = m.vFront;
        d.verticalSyncPulseWidth = m.vSync;
        d.horizontalSyncConfig = m.hSyncNeg ? 0 : 1;
        d.verticalSyncConfig = m.vSyncNeg ? 0 : 1;
        d.detailedTimingModeID = (UInt32)mode;
        info->flags = kIODetailedTimingValid;
        return kIOReturnSuccess;
    }
    const RD3KDisplayHead &t = fInfo;
    if (fDcn && t.hTotal > t.width && t.vTotal > t.height && fNativeRefresh) {
        IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
        uint64_t pclk = (uint64_t)t.hTotal * t.vTotal * fNativeRefresh / 1000;
        d.pixelClock = d.minPixelClock = d.maxPixelClock = pclk;
        d.horizontalActive = t.width;
        d.horizontalBlanking = t.hTotal - t.width;
        d.horizontalSyncOffset = t.hFront;
        d.horizontalSyncPulseWidth = t.hSync;
        d.verticalActive = t.height;
        d.verticalBlanking = t.vTotal - t.height;
        d.verticalSyncOffset = t.vFront;
        d.verticalSyncPulseWidth = t.vSync;
        d.horizontalSyncConfig = t.hSyncNeg ? 0 : 1;
        d.verticalSyncConfig = t.vSyncNeg ? 0 : 1;
        d.detailedTimingModeID = (UInt32)mode;
        info->flags = kIODetailedTimingValid;
        if (idx) {   /* the native timing, a smaller image stretched over it */
            d.horizontalScaled = fModes[idx].w;
            d.verticalScaled = fModes[idx].h;
            d.scalerFlags = kIOScaleStretchToFit;
            info->flags |= kIOScalingInfoValid;
        }
    }
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                               IOPixelAperture aperture, IOPixelInformation *pi)
{
    uint32_t idx = 0;
    if (!modeIndex(mode, &idx) || depth != 0 || aperture != kIOFBSystemAperture || !pi)
        return kIOReturnUnsupportedMode;
    bzero(pi, sizeof(*pi));
    pi->activeWidth = fModes[idx].w;
    pi->activeHeight = fModes[idx].h;
    pi->bytesPerRow = idx == fCurMode ? fBootPitch : modePitch(idx);
    pi->bitsPerPixel = 32;
    pi->pixelType = kIORGBDirectPixels;
    pi->componentCount = 3;
    pi->bitsPerComponent = 8;
    pi->componentMasks[0] = 0x00ff0000;
    pi->componentMasks[1] = 0x0000ff00;
    pi->componentMasks[2] = 0x000000ff;
    strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
    return kIOReturnSuccess;
}

/* the BAR that contains 'phys', or nullptr */
static IODeviceMemory *barFor(IOPCIDevice *pci, uint64_t phys, uint64_t bytes, uint64_t *barPa)
{
    IOItemCount n = pci->getDeviceMemoryCount();
    for (IOItemCount i = 0; i < n; i++) {
        IODeviceMemory *m = pci->getDeviceMemoryWithIndex(i);
        if (!m)
            continue;
        uint64_t pa = m->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
        uint64_t len = m->getLength();
        if (phys >= pa && bytes <= len && phys - pa <= len - bytes) {
            *barPa = pa;
            return m;
        }
    }
    return nullptr;
}

IODeviceMemory *RDNA3Framebuffer::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture || !fPhys || !fBytes)
        return nullptr;
    uint64_t barPa = 0;
    IODeviceMemory *bar = barFor(fPci, fPhys, fBytes, &barPa);
    if (bar) {
        IOMemoryDescriptor *sub = IOSubMemoryDescriptor::withSubRange(bar, (IOByteCount)(fPhys - barPa),
                                                                      (IOByteCount)fBytes, kIODirectionNone);
        if (sub)
            return (IODeviceMemory *)sub;
    }
    return (IODeviceMemory *)IOMemoryDescriptor::withAddressRange(
        fPhys, fBytes, kIODirectionNone | kIOMemoryMapperNone, nullptr);
}

IODeviceMemory *RDNA3Framebuffer::getVRAMRange()
{
    uint64_t barPa = 0;
    IODeviceMemory *bar = fPhys ? barFor(fPci, fPhys, fBytes, &barPa) : nullptr;
    if (bar) {
        bar->retain();
        return bar;
    }
    return getApertureRange(kIOFBSystemAperture);
}

/* ---------------------- open order, integrated ---------------------- */

/* head 0's framebuffer on the same PCI device, started, retained; or null */
RDNA3Framebuffer *RDNA3Framebuffer::copyHead0()
{
    RDNA3Framebuffer *found = nullptr;
    OSIterator *it = fPci ? fPci->getChildIterator(gIOServicePlane) : nullptr;
    if (!it)
        return nullptr;
    while (OSObject *o = it->getNextObject()) {
        RDNA3Framebuffer *f = OSDynamicCast(RDNA3Framebuffer, o);
        if (f && f != this && f->fHead == 0 && f->fStarted && !f->isInactive()) {
            f->retain();
            found = f;
            break;
        }
    }
    it->release();
    return found;
}

/* DRIVER-PLAN N40 (2): IOFramebuffer::open() of a controller's first
 * framebuffer gives it IOFramebufferOpenGLIndex 0 and its dependents the
 * next ones (assignGLIndex). WindowServer opened head 1 first in card run
 * 2026-10-17d (GL 0,1 against IOFBDependentIndex 1,0; AGDC slots and
 * IOAccelIndex follow the dependent index): with rdna3glorder=1 head 1's
 * open() opens head 0 first, which opens head 1 as its dependent
 * (rd3fb_open_head0_first). Then (3) the IOFBIntegrated A/B. */
IOReturn RDNA3Framebuffer::open(void)
{
    /* IOGraphics calls open() in its system gate (newUserClient, and the
     * first open's dependents); the reads of 'opened' and gOpenReenter
     * below rely on it. A caller without it is counted, logged and served
     * inside the gate (IOWorkLoop::runAction; the gate is recursive, so
     * IOFramebuffer::open's own SYSGATEGUARD nests) */
    IOWorkLoop *sys = getGraphicsSystemWorkLoop();
    if (sys && !sys->inGate()) {
        fOpenUngated++;
        if (fOpenUngated <= 4 || (fOpenUngated & 1023) == 0)
            FBLOG("head %u open: called OUTSIDE IOGraphics' system gate (%u such): run inside it", fHead, fOpenUngated);
        return sys->runAction(OSMemberFunctionCast(IOWorkLoop::Action, this, &RDNA3Framebuffer::openGated), this);
    }
    if (!sys)
        FBLOG("head %u open: no IOGraphics system workloop: the open order runs unchecked", fHead);
    return openGated(nullptr, nullptr, nullptr, nullptr);
}

IOReturn RDNA3Framebuffer::openGated(void *, void *, void *, void *)
{
    enum rd3fb_order_why why = RD3FB_ORDER_OFF;
    IOReturn r0 = kIOReturnSuccess;
    const bool wasOpen = opened != 0;
    RDNA3Framebuffer *h0 = (fArgGlOrder && fHead && !wasOpen && !gOpenReenter) ? copyHead0() : nullptr;
    bool sameDep = false;
    if (h0) {
        OSObject *a = getProperty("IOFBDependentID"), *b = h0->getProperty("IOFBDependentID");
        sameDep = a && b && a->isEqualTo(b);
    }
    if (rd3fb_open_head0_first(fArgGlOrder != 0, fHead, wasOpen, gOpenReenter != 0, h0 != nullptr, sameDep,
                               h0 && h0->opened, &why)) {
        gOpenReenter++;
        r0 = h0->open();
        gOpenReenter--;
    }
    OSSafeReleaseNULL(h0);
    const bool openBefore = opened != 0;   /* head 0's open may have opened us as its dependent */
    IOReturn r = super::open();
    /* the first open, a retry after a failed one, and every head-0-first */
    const bool retry = !wasOpen && fOpenFailed;
    if ((!wasOpen && (!fOpenLogged || fOpenFailed)) || why == RD3FB_ORDER_FIRST) {
        fOpenLogged = true;
        OSNumber *gl = OSDynamicCast(OSNumber, getProperty("IOFramebufferOpenGLIndex"));
        FBLOG("head %u open%s: %s%s -> 0x%x; IOFramebufferOpenGLIndex %d, IOFBDependentIndex %u (rdna3glorder=%u, "
              "default 1)", fHead, retry ? " (retry after a FAILED open)" : "", rd3fb_order_text(why),
              why == RD3FB_ORDER_FIRST ? (r0 ? " (its open FAILED)" : " (ok)") : "",
              r, gl ? (int)gl->unsigned32BitValue() : -1, fHead, fArgGlOrder);
        if (why == RD3FB_ORDER_FIRST && r0)
            FBLOG("head %u open: head 0's open answered 0x%x: the GL indices are IOGraphics' own", fHead, r0);
        char txt[128];
        if (r == kIOReturnSuccess)
            snprintf(txt, sizeof(txt), "%s%s", rd3fb_order_text(why), retry ? " (after a failed open)" : "");
        else
            snprintf(txt, sizeof(txt), "%s; open FAILED 0x%x (%u failure(s))", rd3fb_order_text(why), (unsigned)r,
                     fOpenFails + 1);
        setProperty("RDNA3,OpenOrder", txt);
    }
    if (r != kIOReturnSuccess && !wasOpen) {
        fOpenFails++;
        fOpenFailed = true;
    } else if (r == kIOReturnSuccess) {
        fOpenFailed = false;
    }
    /* once per open by IOGraphics (it sets the property in each open); open
     * already before super::open() without the A/B done: a path that did not
     * come through this override (the periodic re-check would catch it too) */
    if (r == kIOReturnSuccess && opened && (!openBefore || !fIntegDone))
        integratedAB(openBefore ? "late: already open before this open(), not through it" : "open");
    return r;
}

/* DRIVER-PLAN N40 (3): IOGraphics publishes IOFBIntegrated in open() when
 * the card's IOPCIDevice is on PCI bus 0 (IOFBController::init); in
 * IOFramebuffer.cpp fIntegrated is used beyond the property only in close()
 * with an NVIDIA device present. The GPUWrangler "(IG)" flag comes from its
 * own look at the device, which a kext cannot change: rdna3fbintegrated
 * (0 keep, 1 remove, 2 set) changes what readers of the property see. */
void RDNA3Framebuffer::integratedAB(const char *when)
{
    const bool present = getProperty("IOFBIntegrated") != nullptr;
    const enum rd3fb_integ_act a = rd3fb_integrated_action(fArgIntegrated, present);
    if (a == RD3FB_INTEG_REMOVE)
        removeProperty("IOFBIntegrated");
    else if (a == RD3FB_INTEG_SET)
        setProperty("IOFBIntegrated", kOSBooleanTrue);
    const char *what = a == RD3FB_INTEG_REMOVE ? "removed" : a == RD3FB_INTEG_SET ? "set Yes" : "kept";
    const unsigned bus = fPci ? (unsigned)fPci->getBusNumber() : 0xffu;
    char txt[160];
    snprintf(txt, sizeof(txt), "IOGraphics %s, %s (rdna3fbintegrated=%u), PCI bus %u%s%s", present ? "Yes" : "not set",
             what, fArgIntegrated, bus, strcmp(when, "open") ? "; " : "", strcmp(when, "open") ? when : "");
    fIntegDone = true;
    fIntegApplied++;
    if (!fIntegLogged++ || (strcmp(when, "open") && fIntegApplied <= 8))
        FBLOG("head %u: IOFBIntegrated: %s (default 0 = as IOGraphics sets it; IOGraphics' rule: bus 0 = integrated)",
              fHead, txt);
    setProperty("RDNA3,IntegratedAB", txt);
}

/* every ~2 s (vblFire): the A/B still holds - IOGraphics sets the property
 * again in each open(), and an open that did not come through our override
 * would leave it as IOGraphics set it */
void RDNA3Framebuffer::integratedRecheck()
{
    if (!opened)
        return;
    if (!fIntegDone) {
        fIntegLate++;
        integratedAB("late: open without our open() seeing it, applied by the periodic check");
        return;
    }
    const bool present = getProperty("IOFBIntegrated") != nullptr;
    if (rd3fb_integrated_action(fArgIntegrated, present) == RD3FB_INTEG_KEEP)
        return;
    fIntegReset++;
    if (fIntegReset <= 4 || (fIntegReset & 1023) == 0)
        FBLOG("head %u: IOFBIntegrated is %s again after our A/B (rdna3fbintegrated=%u): IOGraphics re-set it, "
              "applied again (%u such)", fHead, present ? "present" : "absent", fArgIntegrated, fIntegReset);
    integratedAB("re-applied: IOGraphics had re-set it");
}

/* ------------------------ detailed timings ------------------------ */

/* which of our modes a detailed timing is (rd3fb_dt_match), -1 none */
int RDNA3Framebuffer::dtMatch(const IODetailedTimingInformationV2 *d) const
{
    struct rd3fb_dt want, modes[kMaxModes];
    unsigned n = 0;
    bzero(&want, sizeof(want));
    want.pclk = d->pixelClock;
    want.hact = d->horizontalActive;
    want.hblank = d->horizontalBlanking;
    want.vact = d->verticalActive;
    want.vblank = d->verticalBlanking;
    want.hscaled = d->horizontalScaled;
    want.vscaled = d->verticalScaled;
    for (uint32_t i = 0; i < fNModes && i < kMaxModes; i++) {
        IOTimingInformation ti;
        bzero(&modes[n], sizeof(modes[n]));
        /* getTimingInfoForDisplayMode is const in effect; the cast keeps
         * IOFramebuffer's signature */
        if (const_cast<RDNA3Framebuffer *>(this)->getTimingInfoForDisplayMode(kModeId + (IODisplayModeID)i, &ti) ==
                kIOReturnSuccess &&
            (ti.flags & kIODetailedTimingValid) && !(ti.flags & kIOScalingInfoValid)) {
            const IODetailedTimingInformationV2 &m = ti.detailedInfo.v2;
            modes[n].pclk = m.pixelClock;
            modes[n].hact = m.horizontalActive;
            modes[n].hblank = m.horizontalBlanking;
            modes[n].vact = m.verticalActive;
            modes[n].vblank = m.verticalBlanking;
        }
        n++;   /* modes[k] is mode k: an empty entry (no clock) never matches */
    }
    return rd3fb_dt_match(&want, modes, n);
}

/* CoreDisplay's InstallTiming (card run 2026-10-17d: 57 x 0xe00002c7, the
 * base class's unsupported, for the EDID's 720x480 .. 1920x1080@72):
 * rdna3fbdtmap=0 (default) answers as before, logged with whether the timing
 * is one of our modes; 1 accepts our modes' timings (setDetailedTimings maps
 * their ids to the mode) and refuses the rest with UnsupportedMode */
IOReturn RDNA3Framebuffer::validateDetailedTiming(void *description, IOByteCount descripSize)
{
    IODetailedTimingInformationV2 *d = nullptr;
    if (description && descripSize == sizeof(IOFBDisplayModeDescription))
        d = &((IOFBDisplayModeDescription *)description)->timingInfo.detailedInfo.v2;
    else if (description && descripSize == sizeof(IODetailedTimingInformationV2))
        d = (IODetailedTimingInformationV2 *)description;
    fDtValidates++;
    const int m = d ? dtMatch(d) : -1;
    /* rdna3fbdtmap=0: the base class's Unsupported for every shape */
    const enum rd3fb_dt_ans ans = rd3fb_dt_answer(fArgDtMap, d != nullptr, m);
    const bool accept = ans == RD3FB_DT_ACCEPT;
    if (m >= 0)
        fDtMatched++;
    if (!accept)
        fDtRefused++;
    if (!d) {
        if (fDtValidates <= 8 || (fDtValidates & 1023) == 0)
            FBLOG("head %u validateDetailedTiming #%u: %llu bytes, not a timing -> %s (rdna3fbdtmap=%u)", fHead,
                  fDtValidates, (unsigned long long)descripSize,
                  ans == RD3FB_DT_BADARG ? "Bad argument" : "Unsupported (as before)", fArgDtMap);
        return ans == RD3FB_DT_BADARG ? kIOReturnBadArgument : kIOReturnUnsupported;
    }
    if (fDtValidates <= 24 || (fDtValidates & 1023) == 0 || accept) {
        const uint64_t tot = (uint64_t)(d->horizontalActive + d->horizontalBlanking) * (d->verticalActive + d->verticalBlanking);
        const uint32_t mhz = tot ? (uint32_t)(d->pixelClock * 1000 / tot) : 0;
        FBLOG("head %u validateDetailedTiming #%u id 0x%x %ux%u@%u.%03u: %s%d -> %s (rdna3fbdtmap=%u)", fHead,
              fDtValidates, (unsigned)d->detailedTimingModeID, (unsigned)d->horizontalActive,
              (unsigned)d->verticalActive, mhz / 1000, mhz % 1000, m >= 0 ? "our mode " : "not one of our modes ", m,
              accept ? "accepted" : ans == RD3FB_DT_UNSUPPORTED_MODE ? "UnsupportedMode" : "Unsupported (as before)",
              fArgDtMap);
    }
    if (!accept)
        return ans == RD3FB_DT_UNSUPPORTED_MODE ? kIOReturnUnsupportedMode : kIOReturnUnsupported;
    if (!d->minPixelClock)
        d->minPixelClock = d->pixelClock;
    if (!d->maxPixelClock)
        d->maxPixelClock = d->pixelClock;
    if (descripSize == sizeof(IOFBDisplayModeDescription)) {
        IODisplayModeInformation *info = &((IOFBDisplayModeDescription *)description)->info;
        IODisplayModeInformation ours;
        if (getInformationForDisplayMode(kModeId + (IODisplayModeID)m, &ours) == kIOReturnSuccess) {
            info->nominalWidth = ours.nominalWidth;
            info->nominalHeight = ours.nominalHeight;
            info->refreshRate = ours.refreshRate;
            info->maxDepthIndex = 0;
        }
    }
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::setDetailedTimings(OSArray *array)
{
    fDtSets++;
    if (!fArgDtMap) {
        if (fDtSets <= 4)
            FBLOG("head %u setDetailedTimings(%u timing(s)): Unsupported as before (rdna3fbdtmap=0)", fHead,
                  array ? (unsigned)array->getCount() : 0u);
        return kIOReturnUnsupported;
    }
    uint32_t n = 0, bad = 0;
    for (unsigned i = 0; array && i < array->getCount(); i++) {
        OSData *o = OSDynamicCast(OSData, array->getObject(i));
        const IODetailedTimingInformationV2 *d =
            o && o->getLength() >= sizeof(IODetailedTimingInformationV2)
                ? (const IODetailedTimingInformationV2 *)o->getBytesNoCopy()
                : nullptr;
        const int m = d ? dtMatch(d) : -1;
        if (m < 0 || n >= kMaxDt) {
            bad++;
            continue;
        }
        fDtId[n] = (IODisplayModeID)d->detailedTimingModeID;
        fDtIdx[n] = (uint32_t)m;
        n++;
    }
    fNDt = n;
    fDtMapped = n;
    FBLOG("head %u setDetailedTimings: %u timing(s) mapped to our modes, %u not ours%s (rdna3fbdtmap=1)", fHead, n,
          bad, array ? "" : " (cleared)");
    return bad ? kIOReturnUnsupportedMode : kIOReturnSuccess;
}

/* the DRIVER-PLAN N40 counters, RDNA3,FBStats in the registry (the report's
 * 12-ioreg-fb.txt; tools/triage_reports.py "fb N40") */
void RDNA3Framebuffer::publishStats()
{
    struct rd3fb_cur_q q;
    struct rd3fb_wake w;
    bzero(&q, sizeof(q));
    if (fCurLock) {
        IOSimpleLockLock(fCurLock);
        q = fCurQ;
        IOSimpleLockUnlock(fCurLock);
    }
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(fLock);
    w = fWake;
    IOSimpleLockUnlockEnableInterrupt(fLock, st);
    OSDictionary *d = OSDictionary::withCapacity(48);
    if (!d)
        return;
    const struct { const char *k; uint64_t v; } kv[] = {
        { "cursor_sets", q.sets }, { "cursor_kicks", q.kicks }, { "cursor_coalesced", q.coalesced },
        { "cursor_same", q.same }, { "cursor_applies", q.applies }, { "cursor_fails", q.fails },
        { "cursor_sync", fCurSync }, { "cursor_images", fCurImages },
        { "wake_sleeps", w.sleeps }, { "wake_family", w.family_wakes }, { "wake_owed_max", w.owed_max },
        { "wake_plain_on", w.plain_power_ons }, { "wake_didwake_seen", w.didwakes_seen },
        { "wake_unanswered", (uint64_t)(w.will_sleep_off > 0 ? w.will_sleep_off : 0) },
        { "wake_nogate", fWakeNoGate }, { "wake_off", fWakeOff },
        { "vbl_delivered", fVblDelivered }, { "vbl_polls", fVblPolls }, { "vbl_still", fVblStill },
        { "vbl_overlap", fVblOverlap },
        { "wake_tap_events", w.tap_events }, { "wake_slept_pending", w.sys_slept },
        { "wake_paging_already", w.paging_already }, { "wake_dropped", w.dropped }, { "wake_fails", w.fails },
        { "wake_retries", w.retries }, { "wake_gave_up", w.gave_up }, { "wake_stopping", fWakeStopping },
        { "vbl_call_failed", fVblCallFailed },
        { "cursor_image_fails", fCurImageFails }, { "cursor_convert_fails", fCurConvertFails },
        { "integ_applied", fIntegApplied }, { "integ_reset", fIntegReset }, { "integ_late", fIntegLate },
        { "open_ungated", fOpenUngated }, { "open_fails", fOpenFails },
        { "dt_validate", fDtValidates }, { "dt_matched", fDtMatched }, { "dt_refused", fDtRefused },
        { "dt_sets", fDtSets }, { "dt_mapped", fDtMapped }, { "dispflags_asked", fDispFlagsAsked },
    };
    for (unsigned i = 0; i < sizeof(kv) / sizeof(kv[0]); i++) {
        OSNumber *n = OSNumber::withNumber((unsigned long long)kv[i].v, 64);
        if (n) {
            d->setObject(kv[i].k, n);
            n->release();
        }
    }
    setProperty("RDNA3,FBStats", d);
    d->release();
}

/* --------------------------- attributes --------------------------- */

IOReturn RDNA3Framebuffer::getAttribute(IOSelect attribute, uintptr_t *value)
{
    if (attribute == kIOWindowServerActiveAttribute) {
        if (value) *value = 1;
        return kIOReturnSuccess;
    }
    if (attribute == kIOHardwareCursorAttribute) {
        if (value) *value = fCurReady ? 1 : 0;
        return kIOReturnSuccess;
    }
    return super::getAttribute(attribute, value);
}

IOReturn RDNA3Framebuffer::setAttribute(IOSelect attribute, uintptr_t value)
{
    if (attribute == kIOWindowServerActiveAttribute)
        return kIOReturnSuccess;
    if (attribute == kIOPowerAttribute) {
        powerBlank(value != 0, "power attribute");
        if (value)
            handleEvent(kIOFBNotifyDidPowerOn);
        else
            handleEvent(kIOFBNotifyWillPowerOff);
        IOReturn r = super::setAttribute(attribute, value);
        return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
    }
    return super::setAttribute(attribute, value);
}

IOReturn RDNA3Framebuffer::getAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t *value)
{
    switch (attribute) {
    case kConnectionEnable:                    if (value) *value = fConnected ? 1 : 0; return kIOReturnSuccess;
    case kConnectionFlags:                     if (value) *value = 0; return kIOReturnSuccess;
    case kConnectionColorModesSupported:       if (value) *value = 0x00000001; return kIOReturnSuccess;
    case kConnectionColorDepthsSupported:      if (value) *value = 0x00000002; return kIOReturnSuccess;
    case kConnectionControllerColorDepth:      if (value) *value = 0x00000002; return kIOReturnSuccess;
    case kConnectionControllerDepthsSupported: if (value) *value = 0x00000002; return kIOReturnSuccess;
    case kConnectionColorMode:                 if (value) *value = 0x00000001; return kIOReturnSuccess;
    case kConnectionCheckEnable:               if (value) *value = fConnected ? 1 : 0; return kIOReturnSuccess;
    case kConnectionDisplayFlags:
        /* 0 (NullMoth 1baebac); 1 was kIODisplayNeedsCEAUnderscan, which
         * no DP/HDMI sink of ours asked for (rdna3dispflags=1 restores it) */
        if (value) *value = fArgDispFlags ? 1 : 0;
        fDispFlagsAsked++;
        if (!fDispFlagsLogged) {
            fDispFlagsLogged = true;
            FBLOG("head %u: kConnectionDisplayFlags -> %u (rdna3dispflags=%u, default 0)", fHead,
                  fArgDispFlags ? 1u : 0u, fArgDispFlags);
        }
        return kIOReturnSuccess;
    case kConnectionSupportsHLDDCSense: {
        /* IOGraphics asks hasDDCConnect / getDDCBlock only when this
         * succeeds: the EDID the core read over AUX or the DDC */
        bool ok = fetchEdid();
        IOLog("RDNA3FB: kConnectionSupportsHLDDCSense -> %s\n", ok ? "yes" : "no");
        return ok ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    case kConnectionSupportsAppleSense:
    case kConnectionSupportsLLDDCSense:
    case kConnectionDisplayParameterCount:
    case kConnectionDisplayParameters:
        return kIOReturnUnsupported;
    default:
        return super::getAttributeForConnection(idx, attribute, value);
    }
}

IOReturn RDNA3Framebuffer::setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value)
{
    switch (attribute) {
    case kConnectionChanged:
        /* IOGraphics re-probes after a connect change: the EDID again */
        if (fHpOn) {
            fEdidAsked = false;
            fEdidTries = 0;
            FBLOG("head %u: connection changed, %s", fHead, fConnected ? "the EDID is read again" : "no sink");
        }
        return kIOReturnSuccess;
    case kConnectionPower:
    case kConnectionColorModesSupported:
    case kConnectionColorDepthsSupported:
    case kConnectionControllerColorDepth:
    case kConnectionControllerDepthsSupported:
    case kConnectionColorMode:
    case kConnectionDisplayFlags:
    case kConnectionFlags:
        return kIOReturnSuccess;
    default:
        return super::setAttributeForConnection(idx, attribute, value);
    }
}

/* display sleep: the core blanks the head (rdna3dpms=1; it answers
 * unsupported otherwise and nothing changes). Both the power attribute and
 * the power state lead here; the core's blank is idempotent. */
void RDNA3Framebuffer::powerBlank(bool on, const char *why)
{
    if (!fCore || !fSymBlank || fPowerOn == (int)on)
        return;
    IOReturn b = fCore->callPlatformFunction(fSymBlank, false, (void *)(uintptr_t)fHead,
                                             (void *)(uintptr_t)(on ? 0 : 1), nullptr, nullptr);
    if (b == kIOReturnUnsupported)
        return;
    /* a refused wake (or sleep) keeps the old state, so the next power
     * request tries again (NullMoth 79e1c2f: a failed wake recorded as done
     * left the head black with nothing left to retry it) */
    if (b == kIOReturnSuccess)
        fPowerOn = on;
    FBLOG("%s: head %u %s (0x%x)%s", why, fHead, on ? "unblanked" : "blanked", (unsigned)b,
          b == kIOReturnSuccess ? "" : ": FAILED, the next power request retries");
}

IOReturn RDNA3Framebuffer::setPowerState(unsigned long ordinal, IOService *)
{
    FBLOG("setPowerState %lu", ordinal);
    powerBlank(ordinal != 0, "power state");
    if (!ordinal) {
        handleEvent(kIOFBNotifyWillPowerOff);
        return kIOPMAckImplied;
    }
    /* IOGraphics' system sleep cleared pagingState and told every client
     * WillSleep; its wake work runs from IOFramebuffer::setPowerState, which
     * this one replaces, and handleEvent() does nothing off the controller's
     * power thread. So after a system sleep the framebuffer does that work
     * itself (rdna3fbwake=1; NullMoth's familyDidWake), else nobody sends
     * DidWake and WindowServer waits for paging. At boot pagingState is clear
     * too, but no WillSleep came: no wake work then. */
    const bool paging = pagingState != 0;   /* a look outside the gates: familyDidWakeGated looks again inside */
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(fLock);
    const bool slept = fWake.sys_slept != 0;
    const int32_t pend = fWake.will_sleep_off;
    const int32_t owed = rd3fb_wake_power_on(&fWake, fArgWake != 0, paging);
    IOSimpleLockUnlockEnableInterrupt(fLock, st);
    if (owed) {
        familyDidWake("power-on");
    } else {
        if (slept)
            FBLOG("system wake: head %u, pagingState %d, %d WillSleep(false) unanswered: the family wake work %s", fHead,
                  paging ? 1 : 0, (int)pend, paging ? "is not needed (paging on; the sleep is cleared)"
                                                    : "is OFF (rdna3fbwake=0): WindowServer may wait for paging");
        if (slept && !fArgWake && !paging)
            fWakeOff++;
        handleEvent(kIOFBNotifyDidPowerOn);
        if (slept)
            publishStats();
    }
    return kIOPMAckImplied;
}

/* IOGraphics' notifications to every client of this framebuffer, ours
 * included: WillSleep(false) is counted until a DidWake(false) answers it,
 * WillSleep(true) marks a system sleep (rdna3_fb_policy.h) */
IOReturn RDNA3Framebuffer::sleepTap(OSObject *self, void *, IOFramebuffer *, IOIndex event, void *info)
{
    RDNA3Framebuffer *fb = OSDynamicCast(RDNA3Framebuffer, self);
    if (!fb || !fb->fLock || (event != kIOFBNotifyWillSleep && event != kIOFBNotifyDidWake))
        return kIOReturnSuccess;
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(fb->fLock);
    rd3fb_wake_event(&fb->fWake, (int)event, info != nullptr);
    const int32_t off = fb->fWake.will_sleep_off;
    const uint32_t sleeps = fb->fWake.sleeps;
    IOSimpleLockUnlockEnableInterrupt(fb->fLock, st);
    if (event == kIOFBNotifyWillSleep && info)
        FBLOG("system sleep #%u: head %u told WillSleep, %d WillSleep(false) unanswered", sleeps, fb->fHead, (int)off);
    return kIOReturnSuccess;
}

/* What IOFramebuffer's own wake does for a framebuffer that powers on after
 * a system sleep (IOFBController's power work, then handleEvent(DidWake)):
 * clients told DidWake(true), then DidWake(false) as many times as
 * WillSleep(false) came (IOGraphicsAccelerator2 enables itself again only
 * then), DidPowerOn(false), pagingState set and WindowServer, asleep on
 * fServerConnect until paging is on, woken.
 *
 * Gates (ECC review): the family's order, the system gate and then the
 * controller's (IOFBController::computeState delivers DidWake/DidPowerOn so,
 * systemWork its WillSleep; the other order risks a deadlock against them).
 * WindowServer's system entries (IOFramebuffer::_extEntry) test pagingState
 * holding both, open the controller gate and sleep on fServerConnect in the
 * system gate, which sleepGate releases as the thread sleeps: with the
 * system gate held here the test cannot fall between our wakeup and its
 * sleep. The controller entries sleep in the controller gate, held here
 * too. The wakeup is not gate specific (IOLockWakeup on the event; the
 * family wakes system sleepers through the controller's workloop the same
 * way, IOFramebuffer.cpp ~6589), so it goes through our command gate on the
 * controller's workloop and no event source is added to the family's
 * system workloop. tests/test_fb_policy.c (test_wake_gates) explores every
 * interleaving of both orders: no second, delayed wakeup is needed. */
void RDNA3Framebuffer::familyDidWake(const char *why)
{
    IOWorkLoop *sys = getGraphicsSystemWorkLoop();
    IOCommandGate *g = nullptr;
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(fLock);
    const bool stopping = fStopping != 0;
    if (!stopping && fWakeGate) {
        g = fWakeGate;
        g->retain();   /* stop() drops fWakeGate under fLock: ours stays valid */
    }
    if (stopping)
        fWakeStopping++;
    IOSimpleLockUnlockEnableInterrupt(fLock, st);
    if (stopping) {
        FBLOG("system wake (%s): head %u is stopping: the family wake work is skipped", why, fHead);
        return;
    }
    IOReturn r = kIOReturnNotReady;
    if (sys && g)
        r = sys->runAction(&RDNA3Framebuffer::wakeSysGated, this, g);
    if (r == kIOReturnSuccess) {
        OSSafeReleaseNULL(g);
        publishStats();
        return;
    }
    wakeFailed(r, why, sys, g);
    OSSafeReleaseNULL(g);
}

/* the system gate is held: the controller gate next (our command gate) */
IOReturn RDNA3Framebuffer::wakeSysGated(OSObject *target, void *gate, void *, void *, void *)
{
    RDNA3Framebuffer *me = OSDynamicCast(RDNA3Framebuffer, target);
    IOCommandGate *g = (IOCommandGate *)gate;
    if (!me || !g)
        return kIOReturnBadArgument;
    return g->runAction(OSMemberFunctionCast(IOCommandGate::Action, me, &RDNA3Framebuffer::familyDidWakeGated), g);
}

/* the wake work could not run: the sleep stays recorded, a thread call
 * tries again RD3FB_WAKE_RETRIES times ~50 ms apart, then the next power-on */
void RDNA3Framebuffer::wakeFailed(IOReturn r, const char *why, const void *sys, const void *gate)
{
    bool armed = false;
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(fLock);
    const int again = rd3fb_wake_failed(&fWake);
    const uint32_t fails = fWake.fails;
    if (!gate)
        fWakeNoGate++;
    if (again && !fStopping && fWakeRetry) {
        uint64_t d;
        clock_interval_to_deadline(50, kMillisecondScale, &d);
        retain();
        if (thread_call_enter_delayed(fWakeRetry, d))
            release();   /* already queued: that one holds the reference */
        armed = true;
    }
    IOSimpleLockUnlockEnableInterrupt(fLock, st);
    FBLOG("system wake (%s): head %u family wake work did NOT run (0x%x; system workloop %p, controller gate %p, "
          "stopping %d): %s (%u failure(s)); WindowServer may wait for paging", why, fHead, r, sys, gate,
          (int)fStopping, armed ? "retried in 50 ms" : again ? "NOT retried (no retry thread call)"
                                                         : "given up until the next power-on", fails);
    publishStats();
}

void RDNA3Framebuffer::wakeRetryFire(thread_call_param_t p0, thread_call_param_t)
{
    RDNA3Framebuffer *me = (RDNA3Framebuffer *)p0;
    if (!me->fStopping)
        me->familyDidWake("retry");
    me->release();
}

/* both gates held (system, then controller) */
IOReturn RDNA3Framebuffer::familyDidWakeGated(void *gate, void *, void *, void *)
{
    IOCommandGate *g = (IOCommandGate *)gate;
    if (fStopping)
        return kIOReturnAborted;
    const bool paging = pagingState != 0;
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(fLock);
    const int32_t owed = rd3fb_wake_recheck(&fWake, paging ? 1 : 0);
    IOSimpleLockUnlockEnableInterrupt(fLock, st);
    if (!owed) {
        FBLOG("system wake: head %u, in the gates: %s: no wake work", fHead,
              paging ? "paging is on already (the sleep is cleared)" : "no system sleep recorded any more");
        return kIOReturnSuccess;
    }
    IOReturn r1 = deliverFramebufferNotification(kIOFBNotifyDidWake, (void *)true);
    IOReturn r2 = kIOReturnSuccess;
    for (int32_t i = 0; i < owed; i++)
        r2 = deliverFramebufferNotification(kIOFBNotifyDidWake, (void *)false);
    IOReturn r3 = deliverFramebufferNotification(kIOFBNotifyDidPowerOn, (void *)false);
    pagingState = true;
    g->commandWakeup(&fServerConnect, false);   /* every WindowServer thread waiting for paging */
    st = IOSimpleLockLockDisableInterrupt(fLock);
    rd3fb_wake_done(&fWake);
    const uint32_t n = fWake.family_wakes;
    IOSimpleLockUnlockEnableInterrupt(fLock, st);
    FBLOG("system wake #%u: head %u clients told DidWake true and %d x false (0x%x/0x%x), DidPowerOn (0x%x), "
          "paging on, WindowServer woken in the system and controller gates (rdna3fbwake=1)", n, fHead, (int)owed, r1,
          r2, r3);
    return kIOReturnSuccess;
}

/* ----------------------------- cursor ----------------------------- */

/* heads > 0: a surface of our own (zeroed by the core), shown at once */
bool RDNA3Framebuffer::ownSurfaceInit()
{
    const OSSymbol *alloc = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_ALLOC);
    uint32_t pitch = (fW * 4 + 255) & ~255u;
    int rc = -1;
    if (!alloc)
        return false;
    bzero(&fOwnVram, sizeof(fOwnVram));
    fOwnVram.version = RD3K_VRAM_ABI_VERSION;
    fOwnVram.flags = RD3K_VRAM_OWNER_FB_SURFACE;
    fOwnVram.size = (unsigned long long)pitch * fH;
    IOReturn r = fCore->callPlatformFunction(alloc, false, &fOwnVram, nullptr, nullptr, nullptr);
    alloc->release();
    if (r != kIOReturnSuccess || !fOwnVram.phys) {
        FBLOG("head %u: no CPU-visible VRAM for a %ux%u surface (0x%x)", fHead, fW, fH, r);
        return false;
    }
    fOwnSurface = true;
    /* the boot surface is head 0's: ours replaces it as "home" */
    fPhys = fOwnVram.phys;
    fBootVram = fOwnVram.allocationCookie;
    fPitch = fBootPitch = pitch;
    fBytes = (uint64_t)pitch * fH;
    if (!flipTo(fBootVram, fW, fH, fBootPitch, &rc)) {
        FBLOG("head %u: flip to its own surface failed (%d)", fHead, rc);
        ownSurfaceFree();
        return false;
    }
    FBLOG("head %u: own surface %ux%u pitch %u at VRAM 0x%llx", fHead, fW, fH, pitch,
          (unsigned long long)fBootVram);
    return true;
}

void RDNA3Framebuffer::ownSurfaceFree()
{
    if (!fOwnSurface)
        return;
    int rc;
    /* back to the mirror of the boot surface before the memory goes */
    flipTo(fInfo.vramOffset, fW, fH, fInfo.pitch, &rc);
    const OSSymbol *fr = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_FREE);
    if (fr && fCore)
        fCore->callPlatformFunction(fr, false, &fOwnVram, nullptr, nullptr, nullptr);
    OSSafeReleaseNULL(fr);
    fOwnSurface = false;
}

bool RDNA3Framebuffer::cursorInit()
{
    uint32_t gate = 1;
    PE_parse_boot_argn("rdna3cursor", &gate, sizeof(gate));
    if (!gate || !fCore) {
        FBLOG("hardware cursor off (%s)", gate ? "no core kext" : "rdna3cursor=0");
        return false;
    }
    const OSSymbol *alloc = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_ALLOC);
    if (!alloc)
        return false;
    bzero(&fCurVram, sizeof(fCurVram));
    fCurVram.version = RD3K_VRAM_ABI_VERSION;
    fCurVram.flags = RD3K_VRAM_OWNER_FB_CURSOR;
    fCurVram.size = (unsigned long long)kCurDim * kCurDim * 4;
    IOReturn r = fCore->callPlatformFunction(alloc, false, &fCurVram, nullptr, nullptr, nullptr);
    alloc->release();
    if (r != kIOReturnSuccess || !fCurVram.kva) {
        FBLOG("hardware cursor: no CPU-visible VRAM (0x%x), software cursor", r);
        bzero(&fCurVram, sizeof(fCurVram));
        return false;
    }
    fCurKva = (volatile uint32_t *)fCurVram.kva;   /* zeroed by the core */
    fCurReady = true;
    if (fArgCurAsync) {
        fCurLock = IOSimpleLockAlloc();
        fCurCall = fCurLock ? thread_call_allocate_with_options(cursorFire, this, THREAD_CALL_PRIORITY_HIGH,
                                                                THREAD_CALL_OPTIONS_ONCE)
                            : nullptr;
        if (!fCurCall)
            FBLOG("hardware cursor: no lock or thread call (%p): the cursor is moved on WindowServer's thread", fCurLock);
    }
    FBLOG("hardware cursor: %ux%u at VRAM 0x%llx, moved %s (rdna3cursorasync=%u, default 1)", kCurDim, kCurDim,
          (unsigned long long)fCurVram.allocationCookie,
          fCurCall ? "by a thread call off WindowServer's thread" : "on WindowServer's thread", fArgCurAsync);
    return true;
}

void RDNA3Framebuffer::cursorFree()
{
    if (!fCurReady)
        return;
    fCurVisible = false;
    cursorCall(RD3K_CURSOR_OP_MOVE);   /* hide before the memory goes */
    fCurReady = false;
    const OSSymbol *fr = OSSymbol::withCStringNoCopy(RD3K_VRAM_FN_FREE);
    if (fr && fCore)
        fCore->callPlatformFunction(fr, false, &fCurVram, nullptr, nullptr, nullptr);
    OSSafeReleaseNULL(fr);
    fCurKva = nullptr;
}

/* rdna3DisplayHotplug: the first call takes the state as it is; later ones
 * act when the head's generation moved */
void RDNA3Framebuffer::hotplugPoll(bool first)
{
    if (!fCore || !fSymHotplug)
        return;
    struct RD3KDisplayHotplug q;
    bzero(&q, sizeof(q));
    q.version = RD3K_DISPLAY_ABI_VERSION;
    q.head = fHead;
    IOReturn r = fCore->callPlatformFunction(fSymHotplug, false, &q, nullptr, nullptr, nullptr);
    if (r != kIOReturnSuccess) {
        if (first)
            FBLOG("head %u: no hotplug from the core (0x%x)", fHead, r);
        return;
    }
    if (first) {
        fHpOn = true;
        fHpPins = q.hotplug != 0;
        fHpGen = q.generation;
        fLostGen = q.lost;
        fConnected = q.connected != 0;
        setProperty("RDNA3,Connected", fConnected);
        if (fHpPins)
            FBLOG("head %u: hotplug on, HPD%d %s", fHead, q.pin == ~0u ? 0 : (int)q.pin + 1,
                  fConnected ? "connected" : "disconnected");
        return;
    }
    if (q.lost != fLostGen) {
        /* the monitor stayed, its picture did not: IOGraphics probes the
         * head again and WindowServer draws everything again */
        fLostGen = q.lost;
        fLostChanges++;
        setProperty("RDNA3,PictureLost", (unsigned long long)fLostChanges, 32);
        FBLOG("head %u: the picture in VRAM was lost: connect change to IOGraphics so WindowServer draws again%s",
              fHead, fConnect.proc ? "" : " (no handler registered)");
        if (fConnect.proc && (!fHpPins || q.generation == fHpGen))
            fConnect.proc(fConnect.target, fConnect.ref);
    }
    if (!fHpPins || q.generation == fHpGen)
        return;
    fHpGen = q.generation;
    fHpChanges++;
    setProperty("RDNA3,HotplugChanges", (unsigned long long)fHpChanges, 32);
    /* rdna3hotplug=2 (NullMoth 08c5bba): IOGraphics is not told. The
     * framebuffer and its mode stay for the monitor's return, when the core
     * brings the same head's link up again; card run 40's connect change
     * made macOS move both monitors to another mode and the HDMI one stayed
     * black after the replug */
    if (rd3_arg("rdna3hotplug", 0) == 2) {
        FBLOG("head %u: the monitor on HPD%d is %s: framebuffer kept, IOGraphics not told (rdna3hotplug=2)", fHead,
              q.pin == ~0u ? 0 : (int)q.pin + 1, q.connected ? "back" : "away");
        setProperty("RDNA3,MonitorAway", q.connected == 0);
        return;
    }
    fConnected = q.connected != 0;
    fEdidAsked = false;
    fEdidTries = 0;
    if (fConnected)
        fetchEdid();
    setProperty("RDNA3,Connected", fConnected);
    FBLOG("head %u: the monitor on HPD%d is %s: connect change %u to IOGraphics%s", fHead,
          q.pin == ~0u ? 0 : (int)q.pin + 1, fConnected ? "back" : "gone", fHpChanges,
          fConnect.proc ? "" : " (no handler registered)");
    if (fConnect.proc)
        fConnect.proc(fConnect.target, fConnect.ref);
}

/* IOFramebuffer::setGammaTable: 'channels' (1 or 3) tables of 'count'
 * entries of 'width' bits, channel-planar (all of R, then G, then B); 8 bit
 * and narrower entries are bytes, wider ones UInt16. Scaled to 16 bits and,
 * above RD3K_GAMMA_MAX entries, resampled; the core programs them as the
 * head's MPCC output gamma (RDNA3Accel::displayGamma). The answer to
 * IOFramebuffer stays success when the core does not do gamma. */
IOReturn RDNA3Framebuffer::gammaToCore(UInt32 channels, UInt32 count, UInt32 width, const void *data)
{
    if (!fGammaCalls)
        FBLOG("head %u: first setGammaTable: %u channel(s) x %u entries of %u bits%s", fHead, (unsigned)channels,
              (unsigned)count, (unsigned)width, fGammaOff ? " (core said unsupported)" : !fDcn ? " (no DCN head)" : "");
    if (fGammaOff || !fDcn || !fCore || !data || (channels != 1 && channels != 3) || count < 2 || !width ||
        width > 16) {
        fGammaCalls++;
        return kIOReturnSuccess;
    }
    if (!fSymGamma)
        fSymGamma = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_GAMMA);
    if (!fGamma)
        fGamma = (struct RD3KDisplayGamma *)IOMalloc(sizeof(*fGamma));
    if (!fSymGamma || !fGamma)
        return kIOReturnSuccess;
    uint32_t n = count > RD3K_GAMMA_MAX ? RD3K_GAMMA_MAX : count, max = (1u << width) - 1;
    bzero(fGamma, sizeof(*fGamma));
    fGamma->version = RD3K_DISPLAY_ABI_VERSION;
    fGamma->head = fHead;
    fGamma->channels = channels;
    fGamma->count = n;
    for (uint32_t c = 0; c < channels; c++)
        for (uint32_t i = 0; i < n; i++) {
            /* entry i of n <- entry i * (count - 1) / (n - 1) of count, rounded */
            uint32_t k = (uint32_t)(((uint64_t)i * (count - 1) * 2 + (n - 1)) / (2 * (uint64_t)(n - 1)));
            uint32_t v = width <= 8 ? ((const UInt8 *)data)[c * count + k] : ((const UInt16 *)data)[c * count + k];
            if (v > max)
                v = max;
            fGamma->data[c * n + i] = (unsigned short)(((uint64_t)v * 65535 + max / 2) / max);
        }
    IOReturn r = fCore->callPlatformFunction(fSymGamma, false, fGamma, nullptr, nullptr, nullptr);
    if (++fGammaCalls <= 4 || r != kIOReturnSuccess)
        FBLOG("head %u: gamma %ux%u of %u bits -> 0x%x (%d)", fHead, (unsigned)channels, (unsigned)count,
              (unsigned)width, (unsigned)r, fGamma->result);
    if (r == kIOReturnUnsupported)
        fGammaOff = true;
    return kIOReturnSuccess;
}

bool RDNA3Framebuffer::fetchEdid()
{
    /* asked once when it worked; a failure (the core not ready yet, no
     * sink) is retried a few times and logged whenever the answer changes */
    if (fHpOn && !fConnected)
        return false;   /* the head's cable is out (hotplug): no EDID, not another sink's */
    if (fEdidAsked)
        return fEdid.length > 0;
    if (!fCore || !fSymEdid)
        return false;
    bzero(&fEdid, sizeof(fEdid));
    fEdid.version = RD3K_DISPLAY_ABI_VERSION;
    fEdid.head = fHead;
    IOReturn r = fCore->callPlatformFunction(fSymEdid, false, &fEdid, nullptr, nullptr, nullptr);
    if (r != kIOReturnSuccess)
        fEdid.length = 0;
    if (fEdid.length) {
        fEdidAsked = true;
        IOLog("RDNA3FB: head %u EDID %u bytes from connector AUX%u\n", fHead, fEdid.length,
              fEdid.connector);
        return true;
    }
    if (r != fEdidLast || !fEdidTries)
        IOLog("RDNA3FB: head %u no EDID from the core (0x%x, try %u)\n", fHead, r, fEdidTries + 1);
    fEdidLast = r;
    if (++fEdidTries >= 8)
        fEdidAsked = true;
    return false;
}

bool RDNA3Framebuffer::cursorCall(unsigned op)
{
    return cursorCallAt(op, fCurX, fCurY, fCurVisible && fCurHaveImage);
}

/* the core's cursor call (rdna3DisplayCursor): takes its display lock */
bool RDNA3Framebuffer::cursorCallAt(unsigned op, int32_t x, int32_t y, bool visible)
{
    if (!fCore || !fSymCursor)
        return false;
    RD3KDisplayCursor c;
    bzero(&c, sizeof(c));
    c.version = RD3K_DISPLAY_ABI_VERSION;
    c.head = fHead;
    c.op = op;
    c.size = kCurDim;
    c.vramOffset = fCurVram.allocationCookie;
    c.x = x;
    c.y = y;
    c.visible = visible ? 1 : 0;
    return fCore->callPlatformFunction(fSymCursor, false, &c, nullptr, nullptr, nullptr) == kIOReturnSuccess;
}

/* rdna3cursorasync: the state as it is now becomes the newest wish; the
 * thread call is entered when a run is not already owed. 'force' applies it
 * even when it is the state shown. */
void RDNA3Framebuffer::cursorWant(bool force)
{
    IOSimpleLockLock(fCurLock);
    const int kick = rd3fb_cur_set(&fCurQ, fCurX, fCurY, (fCurVisible && fCurHaveImage) ? 1u : 0u, force ? 1 : 0);
    IOSimpleLockUnlock(fCurLock);
    if (!kick)
        return;
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(fLock);
    if (!fStopping) {
        retain();
        if (thread_call_enter(fCurCall))
            release();   /* already queued: that one holds the reference */
    }
    IOSimpleLockUnlockEnableInterrupt(fLock, st);
}

/* the cursor thread call (ONCE, high priority): the newest wish to the core */
void RDNA3Framebuffer::cursorFire(thread_call_param_t p0, thread_call_param_t)
{
    RDNA3Framebuffer *me = (RDNA3Framebuffer *)p0;
    struct rd3fb_cur_state cs;
    if (!me->fStopping && me->fCurReady && me->fCurLock) {
        IOSimpleLockLock(me->fCurLock);
        const int have = rd3fb_cur_take(&me->fCurQ, &cs);
        IOSimpleLockUnlock(me->fCurLock);
        if (have) {
            const bool ok = me->cursorCallAt(RD3K_CURSOR_OP_MOVE, cs.x, cs.y, cs.visible != 0);
            IOSimpleLockLock(me->fCurLock);
            rd3fb_cur_done(&me->fCurQ, &cs, ok ? 1 : 0);
            const uint32_t fails = me->fCurQ.fails;
            IOSimpleLockUnlock(me->fCurLock);
            if (!ok && (me->fCurFailLogged++ < 8 || (fails & 1023) == 0)) {
                FBLOG("cursor: head %u move to %d,%d %s FAILED in the core (%u such)", me->fHead, (int)cs.x, (int)cs.y,
                      cs.visible ? "shown" : "hidden", fails);
                me->publishStats();   /* the counters reach the registry with the log line */
            }
        }
    }
    me->release();
}

IOReturn RDNA3Framebuffer::setCursorImage(void *cursorImage)
{
    if (!fCurReady)
        return kIOReturnUnsupported;
    size_t bytes = (size_t)kCurDim * kCurDim * 4;
    uint32_t *staging = (uint32_t *)IOMalloc(bytes);
    if (!staging)
        return kIOReturnNoMemory;
    bzero(staging, bytes);
    IOHardwareCursorDescriptor d;
    bzero(&d, sizeof(d));
    d.majorVersion = kHardwareCursorDescriptorMajorVersion;
    d.minorVersion = kHardwareCursorDescriptorMinorVersion;
    d.width = kCurDim;
    d.height = kCurDim;
    d.bitDepth = 32;
    d.supportedSpecialEncodings = kTransparentEncodedPixel;
    IOHardwareCursorInfo ci;
    bzero(&ci, sizeof(ci));
    ci.majorVersion = kHardwareCursorInfoMajorVersion;
    ci.minorVersion = kHardwareCursorInfoMinorVersion;
    ci.hardwareCursorData = (UInt8 *)staging;
    bool ok = convertCursorImage(cursorImage, &d, &ci) && ci.cursorWidth && ci.cursorHeight &&
              ci.cursorWidth <= kCurDim && ci.cursorHeight <= kCurDim;
    if (ok) {
        /* convertCursorImage gives straight alpha, packed cursorWidth wide;
         * DCN wants premultiplied rows of kCurDim pixels */
        for (uint32_t y = 0; y < kCurDim; y++)
            for (uint32_t x = 0; x < kCurDim; x++) {
                uint32_t p = 0;
                if (y < ci.cursorHeight && x < ci.cursorWidth) {
                    uint32_t s = staging[y * ci.cursorWidth + x], a = s >> 24;
                    uint32_t r = (((s >> 16) & 255) * a + 127) / 255;
                    uint32_t g = (((s >> 8) & 255) * a + 127) / 255;
                    uint32_t b = ((s & 255) * a + 127) / 255;
                    p = (a << 24) | (r << 16) | (g << 8) | b;
                }
                fCurKva[y * kCurDim + x] = p;
            }
        __asm__ volatile("sfence" ::: "memory");
    }
    IOFree(staging, bytes);
    if (!ok) {
        fCurConvertFails++;
        if (fCurConvertFails <= 8 || (fCurConvertFails & 1023) == 0) {
            FBLOG("cursor: head %u image not converted (%ux%u, limit %u): software cursor for it (%u such)", fHead,
                  (unsigned)ci.cursorWidth, (unsigned)ci.cursorHeight, kCurDim, fCurConvertFails);
            publishStats();
        }
        if (fCurCall) {
            IOSimpleLockLock(fCurLock);
            fCurHaveImage = false;
            IOSimpleLockUnlock(fCurLock);
            cursorWant(false);   /* hidden by the thread call */
        } else {
            fCurHaveImage = false;
            cursorCall(RD3K_CURSOR_OP_MOVE);
        }
        return kIOReturnUnsupported;
    }
    bool first = !fCurHaveImage;
    if (fCurImages++ < 4)
        FBLOG("cursor image %u: %ux%u", fCurImages, (unsigned)ci.cursorWidth, (unsigned)ci.cursorHeight);
    /* the image address never changes; program it once (here, the answer
     * matters to WindowServer), then only move */
    if (first && !cursorCallAt(RD3K_CURSOR_OP_IMAGE, fCurX, fCurY, false)) {
        /* the core refused the cursor's surface: WindowServer draws a
         * software cursor; the next image tries again (fCurHaveImage clear) */
        fCurImageFails++;
        if (fCurImageFails <= 8 || (fCurImageFails & 1023) == 0) {
            FBLOG("cursor: head %u first image: the core REFUSED the cursor surface at VRAM 0x%llx: Unsupported, "
                  "software cursor (%u such)", fHead, (unsigned long long)fCurVram.allocationCookie, fCurImageFails);
            publishStats();
        }
        return kIOReturnUnsupported;
    }
    if (fCurCall) {
        IOSimpleLockLock(fCurLock);
        fCurHaveImage = true;
        IOSimpleLockUnlock(fCurLock);
        cursorWant(first);
        return kIOReturnSuccess;
    }
    fCurHaveImage = true;
    cursorCall(RD3K_CURSOR_OP_MOVE);
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::setCursorState(SInt32 x, SInt32 y, bool visible)
{
    if (!fCurReady)
        return kIOReturnUnsupported;
    if (fCurCall) {
        /* WindowServer's thread only records it: the core's cursor call
         * waits for its display lock (flips, VBL status, EDID, DP link) */
        IOSimpleLockLock(fCurLock);
        fCurX = x;
        fCurY = y;
        fCurVisible = visible;
        IOSimpleLockUnlock(fCurLock);
        cursorWant(false);
        return kIOReturnSuccess;
    }
    fCurSync++;
    fCurX = x;
    fCurY = y;
    fCurVisible = visible;
    return (cursorCall(RD3K_CURSOR_OP_MOVE) || !fCurHaveImage) ? kIOReturnSuccess : kIOReturnError;
}

/* ------------------------------ flips ------------------------------ */

bool RDNA3Framebuffer::flipTo(uint64_t vramOffset, uint32_t w, uint32_t h, uint32_t pitch, int *rc)
{
    *rc = -1;
    if (!fDcn || !fCore || !fModeLock)
        return false;
    IOLockLock(fModeLock);
    bool ok = flipToLocked(vramOffset, w, h, pitch, rc);
    IOLockUnlock(fModeLock);
    return ok;
}

bool RDNA3Framebuffer::flipToLocked(uint64_t vramOffset, uint32_t w, uint32_t h, uint32_t pitch, int *rc,
                                    bool nowait)
{
    *rc = -1;
    if (!fDcn || !fCore)
        return false;
    if (w != fW || h != fH || pitch < w * 4 || (pitch & 3)) {
        if (fFlipRejects++ < 8)
            FBLOG("flip REFUSED %ux%u pitch %u: the mode is %ux%u", w, h, pitch, fW, fH);
        return false;
    }
    /* one flip in flight: the previous one must have latched, else the
     * surface it replaced may still be read by the HUBP */
    RD3KDisplayStatus st;
    /* DRIVER-PLAN N38: the accelerator's present queue keeps its frame and
     * programs it from the flip-done path (amdgpu: one flip in flight per
     * CRTC, the next commit after flip_done) - no wait here */
    if (nowait && coreStatus(&st) && st.flipPending) {
        *rc = RD3K_FLIP_RESULT_BUSY;
        fFlipBusy++;
        if (fFlipBusy <= 8 || (fFlipBusy & 1023) == 0)
            FBLOG("flip to VRAM 0x%llx: the previous flip still pending -- answered Busy, nothing programmed "
                  "(the caller keeps its frame; %u such)", (unsigned long long)vramOffset, fFlipBusy);
        return false;
    }
    for (unsigned i = 0; i < 40 && coreStatus(&st) && st.flipPending; i++)
        IODelay(500);
    /* 20 ms and the previous flip still pending (a blanked head, a stuck
     * HUBP): flipped anyway, said (card run 2026-10-17, ECC review round 2:
     * the accelerator's latch counters cannot see this wait) */
    if (coreStatus(&st) && st.flipPending) {
        fFlipStillPending++;
        if (fFlipStillPending <= 8 || (fFlipStillPending & 1023) == 0)
            FBLOG("flip to VRAM 0x%llx: the previous flip still pending after 20 ms -- flipped anyway (%u such)",
                  (unsigned long long)vramOffset, fFlipStillPending);
    }
    RD3KDisplayFlip f;
    bzero(&f, sizeof(f));
    f.version = RD3K_DISPLAY_ABI_VERSION;
    f.head = fHead;
    f.pitch = pitch;
    f.vramOffset = vramOffset;
    IOReturn r = fCore->callPlatformFunction(fSymFlip, false, &f, nullptr, nullptr, nullptr);
    *rc = f.result;
    if (r != kIOReturnSuccess)
        return false;
    fFlipped = vramOffset != fBootVram;
    if (fFlips++ < 4 || (fFlips % 3600) == 0)
        FBLOG("flip #%u to VRAM 0x%llx pitch %u", fFlips, (unsigned long long)vramOffset, pitch);
    return true;
}

/* DRIVER-PLAN N38: the accelerator's flip-done path sleeps here until the
 * head's flip latched (dcn32_flip_pending through the core's
 * rdna3DisplayStatus); vblFire wakes it at each new frame, a 500 us deadline
 * covers a wakeup that came between the look and the sleep. Never called
 * with a spinlock held (a thread call of the accelerator) */
IOReturn RDNA3Framebuffer::latchWait(struct RD3KFlipLatchWait *w)
{
    const uint32_t to = w->timeoutUs > RD3K_FLIPWAIT_MAX_US ? RD3K_FLIPWAIT_MAX_US : w->timeoutUs;
    uint64_t start = 0, deadline = 0;
    clock_get_uptime(&start);
    clock_interval_to_deadline(to, kMicrosecondScale, &deadline);
    w->pending = 0;
    w->frame = 0;
    w->waitedUs = 0;
    fLatchWaits++;
    for (;;) {
        RD3KDisplayStatus st;
        uint64_t now = 0, ns = 0;
        const bool ok = coreStatus(&st);
        clock_get_uptime(&now);
        absolutetime_to_nanoseconds(now - start, &ns);
        w->waitedUs = (unsigned int)(ns / 1000 > 0xffffffffull ? 0xffffffffull : ns / 1000);
        if (!ok)
            return kIOReturnNotReady;
        w->pending = st.flipPending;
        w->frame = st.frame;
        if (!st.flipPending)
            return kIOReturnSuccess;
        if (now >= deadline || !fLatchLock || fStopping) {
            fLatchWaitTimeouts++;
            if (fLatchWaitTimeouts <= 8 || (fLatchWaitTimeouts & 1023) == 0)
                FBLOG("latch wait: head %u's flip still pending after %u us (frame %u; %u such of %u waits)", fHead,
                      w->waitedUs, st.frame, fLatchWaitTimeouts, fLatchWaits);
            return kIOReturnTimeout;
        }
        uint64_t nap = 0;
        clock_interval_to_deadline(500, kMicrosecondScale, &nap);
        if (nap > deadline)
            nap = deadline;
        IOLockLock(fLatchLock);
        IOLockSleepDeadline(fLatchLock, &fLatchEv, nap, THREAD_UNINT);
        IOLockUnlock(fLatchLock);
    }
}

IOReturn RDNA3Framebuffer::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2,
                                                void *p3, void *p4)
{
    if (!fn)
        return super::callPlatformFunction(fn, wait, p1, p2, p3, p4);

    if (fn->isEqualTo(RD3K_VRAM_FN_SCANOUT)) {
        RD3KVramRequest *r = (RD3KVramRequest *)p1;
        if (!r || r->version != RD3K_VRAM_ABI_VERSION)
            return kIOReturnBadArgument;
        if (!fScanMap) {
            IODeviceMemory *m = getApertureRange(kIOFBSystemAperture);
            if (m) {
                fScanMap = m->map(kIOMapWriteCombineCache);
                m->release();
            }
        }
        if (!fScanMap)
            return kIOReturnNotReady;
        r->phys = fPhys;
        r->kva = (void *)fScanMap->getVirtualAddress();
        r->actualSize = fBytes;
        r->width = fW;
        r->height = fH;
        r->pitch = fBootPitch;
        r->handle = nullptr;
        r->allocationCookie = fBootVram;
        return kIOReturnSuccess;
    }
    if (fn->isEqualTo(RD3K_FLIP_FN) || fn->isEqualTo(kFlipPureFn)) {
        RD3KFlipRequest *f = (RD3KFlipRequest *)p1;
        if (!f || f->version != RD3K_FLIP_ABI_VERSION)
            return kIOReturnBadArgument;
        int rc = -1;
        bool ok = false;
        const bool nowait = (f->flags & RD3K_FLIP_FLAG_NOWAIT) != 0;
        if (fDcn && fModeLock) {
            /* home is read under the mode lock too: a switch moves it */
            IOLockLock(fModeLock);
            ok = (f->flags & RD3K_FLIP_FLAG_HOME)
                     ? flipToLocked(fBootVram, fW, fH, fBootPitch, &rc, nowait)
                     : flipToLocked(f->allocationCookie, f->width, f->height, f->pitch, &rc, nowait);
            IOLockUnlock(fModeLock);
        }
        f->flipResult = rc;
        return ok ? kIOReturnSuccess : rc == RD3K_FLIP_RESULT_BUSY ? kIOReturnBusy : kIOReturnIOError;
    }
    if (fn->isEqualTo(RD3K_FLIP_FN_LATCHWAIT)) {
        RD3KFlipLatchWait *w = (RD3KFlipLatchWait *)p1;
        if (!w || w->version != RD3K_FLIPWAIT_VERSION)
            return kIOReturnBadArgument;
        return latchWait(w);
    }
    /* VRAM, handles, CRC: the core kext answers */
    if (fCore && (fn->isEqualTo(RD3K_FN_DISPLAY_CRC) || fn->isEqualTo(RD3K_VRAM_FN_ALLOC) ||
                  fn->isEqualTo(RD3K_VRAM_FN_FREE) || fn->isEqualTo(RD3K_GPUVA_FN_HANDLES) ||
                  fn->isEqualTo(RD3K_COPY_FN))) {
        if (fn->isEqualTo(RD3K_FN_DISPLAY_CRC))
            p1 = (void *)(uintptr_t)fHead;
        return fCore->callPlatformFunction(fn, wait, p1, p2, p3, p4);
    }
    return super::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}
