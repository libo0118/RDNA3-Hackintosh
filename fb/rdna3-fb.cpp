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
 */

#include <IOKit/graphics/IOGraphicsTypes.h>
#ifndef detailedTimingModeID
#define detailedTimingModeID __reservedA[0]
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSAtomic.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include "../accel/rdna3k_abi.h"

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
    uint32_t v = 0;
    return PE_parse_boot_argn(name, &v, sizeof(v)) && v;
}

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
    IOSimpleLock *fLock = nullptr;
    volatile SInt32 fStopping = 0;

    /* flips */
    unsigned fFlips = 0, fFlipRejects = 0;
    bool     fFlipped = false;

    const OSSymbol *fSymInfo = nullptr, *fSymFlip = nullptr, *fSymStatus = nullptr;
    const OSSymbol *fSymCursor = nullptr, *fSymEdid = nullptr;
    struct RD3KDisplayEdid fEdid = {};
    bool     fEdidAsked = false;
    unsigned fEdidTries = 0;
    IOReturn fEdidLast = kIOReturnSuccess;

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

    IOService *findCore(IOService *provider);
    bool       readCore();
    bool       readConsole();
    uint64_t   vblPeriodAbs() const;
    bool       coreStatus(RD3KDisplayStatus *s);
    bool       flipTo(uint64_t vramOffset, uint32_t w, uint32_t h, uint32_t pitch, int *rc);
    void       scheduleVbl(uint64_t deadline);
    static void vblFire(thread_call_param_t p0, thread_call_param_t);

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOReturn enableController() override { return kIOReturnSuccess; }
    const char *getPixelFormats() override { return kPixelFormats; }
    IOItemCount getDisplayModeCount() override { return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override
    {
        if (modes) modes[0] = kModeId;
        return kIOReturnSuccess;
    }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture,
                                 IOPixelInformation *pi) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override
    {
        if (mode) *mode = kModeId;
        if (depth) *depth = 0;
        return kIOReturnSuccess;
    }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override
    {
        return (mode == kModeId && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupportedMode;
    }
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
    bool isConsoleDevice(void) override { return fHead == 0; }

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
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
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
    fPhys = (uint64_t)v.v_baseAddr;
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
    fSymInfo = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_INFO);
    fSymFlip = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_FLIP);
    fSymStatus = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_STATUS);
    fSymCursor = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_CURSOR);
    fSymEdid = OSSymbol::withCStringNoCopy(RD3K_DISPLAY_FN_EDID);
    if (!fLock || !fSymInfo || !fSymFlip || !fSymStatus || !fSymCursor || !fSymEdid)
        return false;

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
    /* the core probed the connectors before it registered: ask for the
     * EDID now, so IOGraphics' first HLDDC sense / hasDDCConnect question
     * (asked once, while the display is attached) already gets a yes */
    if (fDcn)
        fetchEdid();

    setProperty("IOFBDependentID", (unsigned long long)0x5244334600000001ull, 64);  /* 'RD3F' */
    setProperty("IOFBDependentIndex", (unsigned long long)fHead, 32);
    setProperty("IOFBMemorySize", (unsigned long long)fBytes, 64);
    setProperty("RDNA3,Mode", fDcn ? "dcn" : "console");
    setProperty("RDNA3,FBSource", FB_COMMIT);
    setName("RDNA3Framebuffer");

    if (!super::start(provider)) {
        FBLOG("IOFramebuffer::start failed");
        return false;
    }
    fVblTimer = thread_call_allocate(vblFire, this);
    if (fVblTimer) {
        uint64_t d;
        clock_interval_to_deadline(16667, kMicrosecondScale, &d);
        scheduleVbl(d);
    }
    registerPowerDriver(this, kFBPowerStates, 2);
    if (fDcn)
        cursorInit();
    FBLOG("started (head %u, VBL %s, %s cursor), build %s", fHead,
          fDcn ? "locked to the OTG frame counter" : "timer", fCurReady ? "hardware" : "software", FB_COMMIT);
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
    if (fVblTimer) {
        if (thread_call_cancel(fVblTimer))
            release();
        while (!thread_call_free(fVblTimer)) {
            if (thread_call_cancel(fVblTimer))
                release();
            IOSleep(1);
        }
        fVblTimer = nullptr;
    }
    if (fFlipped && fDcn) {
        int rc;
        flipTo(fBootVram, fW, fH, fBootPitch, &rc);   /* leave the boot surface on screen */
    }
    ownSurfaceFree();
    cursorFree();
    super::stop(provider);
}

void RDNA3Framebuffer::free()
{
    OSSafeReleaseNULL(fScanMap);
    OSSafeReleaseNULL(fCore);
    OSSafeReleaseNULL(fSymInfo);
    OSSafeReleaseNULL(fSymFlip);
    OSSafeReleaseNULL(fSymStatus);
    OSSafeReleaseNULL(fSymCursor);
    OSSafeReleaseNULL(fSymEdid);
    if (fLock) {
        IOSimpleLockFree(fLock);
        fLock = nullptr;
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
    uint64_t now = 0, period = me->vblPeriodAbs();
    clock_get_uptime(&now);
    RD3KDisplayStatus st;
    if (me->coreStatus(&st)) {
        if (st.frame == me->fLastFrame && me->fVblWaits < 12) {
            /* not yet: look again in 250 us */
            me->fVblWaits++;
            uint64_t d;
            clock_interval_to_deadline(250, kMicrosecondScale, &d);
            me->scheduleVbl(d);
            me->release();
            return;
        }
        me->fLastFrame = st.frame;
        me->fVblWaits = 0;
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

IOReturn RDNA3Framebuffer::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
    if (!info || mode != kModeId)
        return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->nominalWidth = fW;
    info->nominalHeight = fH;
    info->refreshRate = (UInt32)(((UInt64)fRefreshMilliHz << 16) / 1000);
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info)
{
    if (!info || mode != kModeId)
        return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    info->appleTimingID = kIOTimingIDInvalid;
    const RD3KDisplayHead &t = fInfo;
    if (fDcn && t.hTotal > t.width && t.vTotal > t.height && fRefreshMilliHz) {
        IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
        uint64_t pclk = (uint64_t)t.hTotal * t.vTotal * fRefreshMilliHz / 1000;
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
        d.detailedTimingModeID = (UInt32)kModeId;
        info->flags = kIODetailedTimingValid;
    }
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                               IOPixelAperture aperture, IOPixelInformation *pi)
{
    if (mode != kModeId || depth != 0 || aperture != kIOFBSystemAperture || !pi)
        return kIOReturnUnsupportedMode;
    bzero(pi, sizeof(*pi));
    pi->activeWidth = fW;
    pi->activeHeight = fH;
    pi->bytesPerRow = fBootPitch;
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
    case kConnectionEnable:                    if (value) *value = 1; return kIOReturnSuccess;
    case kConnectionFlags:                     if (value) *value = 0; return kIOReturnSuccess;
    case kConnectionColorModesSupported:       if (value) *value = 0x00000001; return kIOReturnSuccess;
    case kConnectionColorDepthsSupported:      if (value) *value = 0x00000002; return kIOReturnSuccess;
    case kConnectionControllerColorDepth:      if (value) *value = 0x00000002; return kIOReturnSuccess;
    case kConnectionControllerDepthsSupported: if (value) *value = 0x00000002; return kIOReturnSuccess;
    case kConnectionColorMode:                 if (value) *value = 0x00000001; return kIOReturnSuccess;
    case kConnectionCheckEnable:               if (value) *value = 1; return kIOReturnSuccess;
    case kConnectionDisplayFlags:              if (value) *value = 1; return kIOReturnSuccess;
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

IOReturn RDNA3Framebuffer::setPowerState(unsigned long ordinal, IOService *)
{
    FBLOG("setPowerState %lu", ordinal);
    handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
    return kIOPMAckImplied;
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
    FBLOG("hardware cursor: %ux%u at VRAM 0x%llx", kCurDim, kCurDim,
          (unsigned long long)fCurVram.allocationCookie);
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

bool RDNA3Framebuffer::fetchEdid()
{
    /* asked once when it worked; a failure (the core not ready yet, no
     * sink) is retried a few times and logged whenever the answer changes */
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
    if (!fCore || !fSymCursor)
        return false;
    RD3KDisplayCursor c;
    bzero(&c, sizeof(c));
    c.version = RD3K_DISPLAY_ABI_VERSION;
    c.head = fHead;
    c.op = op;
    c.size = kCurDim;
    c.vramOffset = fCurVram.allocationCookie;
    c.x = fCurX;
    c.y = fCurY;
    c.visible = (fCurVisible && fCurHaveImage) ? 1 : 0;
    return fCore->callPlatformFunction(fSymCursor, false, &c, nullptr, nullptr, nullptr) == kIOReturnSuccess;
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
        fCurHaveImage = false;
        cursorCall(RD3K_CURSOR_OP_MOVE);
        return kIOReturnUnsupported;
    }
    bool first = !fCurHaveImage;
    fCurHaveImage = true;
    if (fCurImages++ < 4)
        FBLOG("cursor image %u: %ux%u", fCurImages, (unsigned)ci.cursorWidth, (unsigned)ci.cursorHeight);
    /* the image address never changes; program it once, then only move */
    if (first && !cursorCall(RD3K_CURSOR_OP_IMAGE)) {
        fCurHaveImage = false;
        return kIOReturnUnsupported;
    }
    cursorCall(RD3K_CURSOR_OP_MOVE);
    return kIOReturnSuccess;
}

IOReturn RDNA3Framebuffer::setCursorState(SInt32 x, SInt32 y, bool visible)
{
    if (!fCurReady)
        return kIOReturnUnsupported;
    fCurX = x;
    fCurY = y;
    fCurVisible = visible;
    return (cursorCall(RD3K_CURSOR_OP_MOVE) || !fCurHaveImage) ? kIOReturnSuccess : kIOReturnError;
}

/* ------------------------------ flips ------------------------------ */

bool RDNA3Framebuffer::flipTo(uint64_t vramOffset, uint32_t w, uint32_t h, uint32_t pitch, int *rc)
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
    for (unsigned i = 0; i < 40 && coreStatus(&st) && st.flipPending; i++)
        IODelay(500);
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
        bool ok = (f->flags & 1u) ? flipTo(fBootVram, fW, fH, fBootPitch, &rc)   /* home */
                                  : flipTo(f->allocationCookie, f->width, f->height, f->pitch, &rc);
        f->flipResult = rc;
        return ok ? kIOReturnSuccess : kIOReturnIOError;
    }
    /* VRAM, handles, CRC: the core kext answers */
    if (fCore && (fn->isEqualTo(RD3K_FN_DISPLAY_CRC) || fn->isEqualTo(RD3K_VRAM_FN_ALLOC) ||
                  fn->isEqualTo(RD3K_VRAM_FN_FREE) || fn->isEqualTo(RD3K_GPUVA_FN_HANDLES))) {
        if (fn->isEqualTo(RD3K_FN_DISPLAY_CRC))
            p1 = (void *)(uintptr_t)fHead;
        return fCore->callPlatformFunction(fn, wait, p1, p2, p3, p4);
    }
    return super::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}
