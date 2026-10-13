/*
 * Derived from the NullMoth NVIDIA driver for macOS (kexts/NVRMAGDC/nvrm-agdc.cpp)
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * AMD (RDNA3-Hackintosh) port, used with the author's permission for
 * AMD-only work. See PROVENANCE.md.
 *
 * RDNA3AGDC - an AppleGraphicsDeviceControl object on the card's PCI device:
 * it answers the display-topology queries IOPresentment (WindowServer's
 * display pipe stack) sends the GPU vendor:
 *   1      vendor info (AMD, 0x1002)
 *   0x980  GPU capability: the framebuffers by IOFBDependentIndex and their
 *          count (maxFramebuffers)
 *   0x921  kAGDCLinkConfig of a framebuffer index: its timing, one link     } "K5",
 *   0x711  extended capability 0x10 (plane scaler) of a framebuffer index   } debug.rdna3agdc_k5
 * The NVIDIA driver arms it between the accelerator's framebuffer
 * registration (level 4) and its display-pipe transactions; without it our
 * card runs got only plane-less transactions (rdna3accel_iop_empty, never
 * iop_ok). Each RDNA3Framebuffer publishes its current timing as
 * RDNA3Timing (rdna3_agdc_k5.h).
 *
 * Its own kext: it links Apple's AppleGraphicsDeviceControl, whose layout is
 * reconstructed (AppleGraphicsDeviceControl_re.h); if that does not match a
 * macOS release only this kext fails, the framebuffer keeps the display.
 * Boot-arg rdna3agdc=1 arms it when the first framebuffer publishes
 * (rdna3agdc=2: without the K5 answers); without it, the sysctl
 * debug.rdna3agdc=1 does. -rdna3off keeps it off.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>
#include <libkern/OSAtomic.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include "../common/rdna3_bootargs.h"
#include "AppleGraphicsDeviceControl_re.h"
#include "rdna3_agdc_k5.h"

extern "C" kern_return_t _start(kmod_info_t *ki, void *d);
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(io.github.voten641.RDNA3AGDC, "0.1.0", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *, void *) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *, void *) { return KERN_SUCCESS; }

#define AGLOG(fmt, ...) IOLog("RDNA3-agdc: " fmt "\n", ##__VA_ARGS__)
#ifndef AGDC_SRC
#define AGDC_SRC "unknown"
#endif
#define kFBClass "RDNA3Framebuffer"

static int    gAgdcK5 = 0;
static SInt64 gAgdcK5Link = 0, gAgdcK5Caps = 0;
static int    gAgdcK5Why = 0;
static SInt64 gAgdcCmds = 0, gAgdcUnsupported = 0;
static int    gAgdcLastRefused = 0;
static SInt32 gAgdcLogged = 0;

enum { kAgdcMaxFB = 4 };
/* the framebuffers 0x980 reports, by IOFBDependentIndex (slots 0..n-1, no
 * hole); gTabLock guards the table, never held across a call into Apple's
 * code (start() asks us back) */
static IOLock    *gTabLock = nullptr;
static IOService *gAgdcFB[kAgdcMaxFB];
static unsigned   gAgdcNFB = 0;
/* framebuffers that left the table: IOPresentment may still hold the
 * pointer an earlier 0x980 answer gave it, so they stay retained until the
 * object goes away */
enum { kAgdcRetired = 16 };
static IOService *gAgdcRetired[kAgdcRetired];
enum { kAgdcFBMapBytes = 3 + kAgdcMaxFB * 25 + 52 + 1 };
static_assert(kAgdcMaxFB >= 1 && kAgdcMaxFB <= 8, "IOPresentment's map holds 8, and kAgdcFBMapBytes assumes one digit per index");
static char   gAgdcFBMap[kAgdcFBMapBytes] = "never scanned";
static int    gAgdcMaxFBOut = 0;
static int    gAgdcK5Asked = 0, gAgdcK5LinkMask = 0, gAgdcK5CapsMask = 0;
static int    gAgdcK5LastRefusedIdx = -1;

class RD3DeviceControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(RD3DeviceControl)
public:
    IOService *fPci = nullptr;
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                              unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(RD3DeviceControl, AppleGraphicsDeviceControl)

static IOReturn rd3K5Answer(unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out, unsigned long *outSize)
{
    uint32_t t[kAgdcMaxFB][RD3K5_TIMING_WORDS];
    struct rd3k5_head h[kAgdcMaxFB];
    unsigned nh;
    IOLockLock(gTabLock);
    nh = gAgdcNFB;
    for (unsigned i = 0; i < nh; i++) {
        OSObject *o = gAgdcFB[i] ? gAgdcFB[i]->copyProperty(RD3K5_TIMING_PROP) : nullptr;
        OSData *d = OSDynamicCast(OSData, o);
        h[i].t = nullptr;
        h[i].tBytes = 0;
        if (d) {
            h[i].tBytes = d->getLength();
            bzero(t[i], sizeof t[i]);
            if (h[i].tBytes == sizeof t[i])
                memcpy(t[i], d->getBytesNoCopy(), sizeof t[i]);
            h[i].t = t[i];
        }
        OSSafeReleaseNULL(o);
    }
    IOLockUnlock(gTabLock);
    int why = 0;
    int32_t idx = -1;
    int r = rd3k5_answer(cmd, in, inSize, out, outSize ? *outSize : 0, h, nh, &why, &idx);
    const UInt32 bit = (idx >= 0 && idx < 32) ? (1u << idx) : 0;
    if (bit) OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5Asked);
    if (r == RD3K5_R_LINK) { OSAddAtomic64(1, &gAgdcK5Link); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5LinkMask); return kIOReturnSuccess; }
    if (r == RD3K5_R_CAPS) { OSAddAtomic64(1, &gAgdcK5Caps); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5CapsMask); return kIOReturnSuccess; }
    if (r == RD3K5_R_UNSUPPORTED) return kIOReturnUnsupported;
    if (r == RD3K5_R_NOTFOUND && idx >= 0 && (unsigned)idx < nh && !h[idx].t) why = 0x30;
    gAgdcK5Why = why;
    gAgdcK5LastRefusedIdx = idx;
    if (OSIncrementAtomic(&gAgdcLogged) < 40)
        AGLOG("K5: command %#x index %d NOT answered (why %#x, %u slot(s))", cmd, idx, why, nh);
    return r == RD3K5_R_BADARG ? kIOReturnBadArgument : kIOReturnNotFound;
}

IOReturn RD3DeviceControl::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                                    unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args)
{
    OSAddAtomic64(1, &gAgdcCmds);
    if (cmd == RD3K5_CMD_VENDOR) {
        if (!out || !outSize || *outSize != sizeof(struct rd3k5_vendor_info)) return kIOReturnBadArgument;
        struct rd3k5_vendor_info *v = (struct rd3k5_vendor_info *)out;
        bzero(v, sizeof *v);
        v->version = 0x30000;
        strlcpy(v->vendor, "AMD", sizeof v->vendor);
        v->vendorID = 0x1002;
        v->vendorClass = 2;
        return kIOReturnSuccess;
    }
    if (cmd == RD3K5_CMD_GPUCAPS) {
        /* AGDCGPUCapability_t */
        struct __attribute__((packed)) caps {
            uint64_t portMask, portFeatureA, portFeatureB, portFeatureC;
            uint32_t countA, countB, countC, countD;
            uint32_t maxFramebuffers;
            IOService *pciDevice;
            IOService *framebuffer[20];
        };
        static_assert(sizeof(struct caps) == 0xdc, "AGDCGPUCapability_t is 0xdc");
        if (!out || !outSize || *outSize != sizeof(struct caps)) return kIOReturnBadArgument;
        struct caps *c = (struct caps *)out;
        bzero(c, sizeof *c);
        IOLockLock(gTabLock);
        unsigned n = gAgdcNFB;
        for (unsigned i = 0; i < n; i++) { c->framebuffer[i] = gAgdcFB[i]; c->portMask |= 1ull << (1 + i); }
        IOLockUnlock(gTabLock);
        if (!n) {   /* no framebuffer in the table yet: the one-port answer */
            c->portMask = 1ull << 1;
            n = 1;
        }
        c->countA = c->countB = c->countC = c->countD = n;
        c->maxFramebuffers = n;
        c->pciDevice = fPci;
        gAgdcMaxFBOut = (int)n;
        return kIOReturnSuccess;
    }
    if (gAgdcK5 && (cmd == RD3K5_CMD_LINKCONFIG || cmd == RD3K5_CMD_FBCAPEX)) {
        IOReturn k5 = rd3K5Answer(cmd, in, inSize, out, outSize);
        if (k5 != kIOReturnUnsupported) return k5;
    }
    OSAddAtomic64(1, &gAgdcUnsupported);
    gAgdcLastRefused = (int)cmd;
    if (OSIncrementAtomic(&gAgdcLogged) < 40)
        AGLOG("command %#x (in %lu bytes) -> Unsupported", cmd, inSize);
    return kIOReturnUnsupported;
}

/* ------------------------------------------------------------------ */

static int gAgdcState = 0;   /* 1 up, 2 terminated, <0 the step that failed */
static RD3DeviceControl *gAgdc = nullptr;
static thread_call_t gAgdcCall = nullptr;
static int gAgdcWant = 0;
static SInt32 gAgdcBusy = 0;
static IOService *gAgdcLoader = nullptr;

static IOService *rd3PciOf(IOService *f)
{
    IOService *p = f ? f->getProvider() : nullptr;
    int hops = 0;
    while (p && !p->metaCast("IOPCIDevice") && hops++ < 8)
        p = p->getProvider();
    return (p && p->metaCast("IOPCIDevice")) ? p : nullptr;
}

/* moves every table entry to the retired list (gTabLock held) */
static void rd3RetireTableLocked(void)
{
    for (unsigned i = 0; i < kAgdcMaxFB; i++) {
        IOService *f = gAgdcFB[i];
        gAgdcFB[i] = nullptr;
        if (!f) continue;
        bool kept = false;
        for (unsigned j = 0; j < kAgdcRetired && !kept; j++) {
            if (gAgdcRetired[j] == f) { f->release(); kept = true; }
            else if (!gAgdcRetired[j]) { gAgdcRetired[j] = f; kept = true; }
        }
        /* a full list: the reference stays (a few bytes) rather than a
         * pointer IOPresentment might still use going stale */
    }
    gAgdcNFB = 0;
}

static void rd3ReleaseAll(void)
{
    IOLockLock(gTabLock);
    rd3RetireTableLocked();
    for (unsigned j = 0; j < kAgdcRetired; j++)
        if (gAgdcRetired[j]) { gAgdcRetired[j]->release(); gAgdcRetired[j] = nullptr; }
    snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "released");
    IOLockUnlock(gTabLock);
}

/* the table from the running RDNA3Framebuffers on 'pci', by IOFBDependentIndex */
static void rd3Scan(IOService *pci)
{
    IOService *tab[kAgdcMaxFB] = {};
    unsigned seen = 0, noidx = 0, range = 0, dup = 0, other = 0, hole = 0, n = 0;
    OSDictionary *m = IOService::serviceMatching(kFBClass);
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    OSSafeReleaseNULL(m);
    if (it) {
        while (OSObject *o = it->getNextObject()) {
            if (++seen > 16) break;
            IOService *f = OSDynamicCast(IOService, o);
            if (!f || f->isInactive()) continue;
            if (rd3PciOf(f) != pci) { other++; continue; }
            OSObject *io = f->copyProperty("IOFBDependentIndex");
            OSNumber *num = OSDynamicCast(OSNumber, io);
            long long ix = num ? (long long)num->unsigned32BitValue() : -1;
            OSSafeReleaseNULL(io);
            if (ix < 0) { noidx++; continue; }
            if (ix >= kAgdcMaxFB) { range++; continue; }
            if (tab[ix]) { dup++; continue; }
            f->retain();
            tab[ix] = f;
        }
        it->release();
    }
    while (n < kAgdcMaxFB && tab[n]) n++;
    for (unsigned i = n; i < kAgdcMaxFB; i++)
        if (tab[i]) { hole++; tab[i]->release(); tab[i] = nullptr; }

    IOLockLock(gTabLock);
    bool same = n == gAgdcNFB;
    for (unsigned i = 0; same && i < n; i++) same = tab[i] == gAgdcFB[i];
    if (same) {
        for (unsigned i = 0; i < n; i++) tab[i]->release();
    } else {
        rd3RetireTableLocked();
        for (unsigned i = 0; i < n; i++) gAgdcFB[i] = tab[i];
        gAgdcNFB = n;
    }
    int w = snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=%u", gAgdcNFB);
    for (unsigned i = 0; i < gAgdcNFB && w > 0 && (unsigned)w < sizeof gAgdcFBMap; i++)
        w += snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " [%u]id %#llx", i,
                      (unsigned long long)gAgdcFB[i]->getRegistryEntryID());
    if (w > 0 && (unsigned)w < sizeof gAgdcFBMap && (noidx | range | dup | other | hole))
        snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " (left out: noidx %u range %u dup %u gpu %u hole %u)",
                 noidx, range, dup, other, hole);
    IOLockUnlock(gTabLock);
    if (!same)
        AGLOG("slot table %s (%u framebuffer(s) seen)", gAgdcFBMap, seen);
}

static void rd3SetStateProp(const char *s)
{
    if (gAgdcLoader) gAgdcLoader->setProperty("RDNA3,AGDC", s);
}

static void rd3AgdcWorkBody(void)
{
    if (gAgdcWant == 1 && !gAgdc) {
        OSDictionary *m = IOService::serviceMatching(kFBClass);
        IOService *fb = m ? IOService::copyMatchingService(m) : nullptr;
        OSSafeReleaseNULL(m);
        IOService *pci = rd3PciOf(fb);
        OSSafeReleaseNULL(fb);
        if (!pci) { gAgdcState = -1; AGLOG("no RDNA3Framebuffer on a PCI device yet"); rd3SetStateProp("waiting for a framebuffer"); return; }
        /* the reconstructed layout must be the running one, or the object
         * would overrun Apple's (or Apple's code ours) */
        const unsigned size = AppleGraphicsDeviceControl::metaClass ? AppleGraphicsDeviceControl::metaClass->getClassSize() : 0;
        if (size != sizeof(AppleGraphicsDeviceControl)) {
            gAgdcState = -6;
            AGLOG("AppleGraphicsDeviceControl is %u bytes on this macOS, not the %u this build knows: not making one",
                  size, (unsigned)sizeof(AppleGraphicsDeviceControl));
            rd3SetStateProp("refused: AppleGraphicsDeviceControl layout differs");
            return;
        }
        rd3Scan(pci);
        RD3DeviceControl *d = OSTypeAlloc(RD3DeviceControl);
        if (!d) { gAgdcState = -2; AGLOG("alloc failed"); return; }
        if (!d->init(nullptr)) { gAgdcState = -3; AGLOG("init failed"); d->release(); return; }
        d->fPci = pci;
        if (!d->attach(pci)) { gAgdcState = -4; AGLOG("attach(%s) failed", pci->getName()); d->release(); return; }
        if (!d->start(pci)) {
            gAgdcState = -5;
            AGLOG("AppleGraphicsDeviceControl::start failed (vendor calls so far %lld)", gAgdcCmds);
            d->detach(pci);
            d->release();
            rd3SetStateProp("start failed");
            return;
        }
        gAgdc = d;
        gAgdcState = 1;
        AGLOG("UP on %s: %u framebuffer slot(s), K5 %s; vendor calls %lld, refused %lld (last %#x)", pci->getName(),
              gAgdcNFB, gAgdcK5 ? "on" : "off", gAgdcCmds, gAgdcUnsupported, gAgdcLastRefused);
        rd3SetStateProp("up");
    } else if (gAgdcWant == 1 && gAgdc) {
        rd3Scan(gAgdc->fPci);   /* a framebuffer came or went */
    } else if (gAgdcWant == 0 && gAgdc) {
        RD3DeviceControl *d = gAgdc;
        gAgdc = nullptr;
        bool ok = d->terminate(kIOServiceRequired | kIOServiceSynchronous);
        d->release();
        rd3ReleaseAll();
        gAgdcState = 2;
        AGLOG("terminate -> %d", ok);
        rd3SetStateProp("terminated");
    }
}
static void rd3AgdcWork(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gAgdcBusy);
    rd3AgdcWorkBody();
    OSDecrementAtomic(&gAgdcBusy);
}
static void rd3AgdcKick(void)
{
    if (gAgdcCall) thread_call_enter(gAgdcCall);
}

static int rdna3agdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAgdcState;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    gAgdcWant = v;
    if (!gAgdcCall) return ENOMEM;
    rd3AgdcKick();
    return 0;
}
/* the SDK's SYSCTL_* initializers leave oid_link to the registration */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
SYSCTL_PROC(_debug, OID_AUTO, rdna3agdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, rdna3agdc_sysctl, "I",
            "write 1 = make our AppleGraphicsDeviceControl object, 0 = terminate it; reads the state (1 up, <0 the failed step)");
SYSCTL_QUAD(_debug, OID_AUTO, rdna3agdc_cmds,    CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcCmds,        "AGDC vendor calls received");
SYSCTL_QUAD(_debug, OID_AUTO, rdna3agdc_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcUnsupported, "AGDC vendor calls answered Unsupported");
SYSCTL_INT (_debug, OID_AUTO, rdna3agdc_last_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcLastRefused, 0, "the last AGDC command refused");
SYSCTL_INT (_debug, OID_AUTO, rdna3agdc_k5,      CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcK5, 0, "K5: 1 = answer kAGDCLinkConfig + plane-scaler capabilities");
SYSCTL_QUAD(_debug, OID_AUTO, rdna3agdc_k5_link, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Link,     "K5: kAGDCLinkConfig queries answered");
SYSCTL_QUAD(_debug, OID_AUTO, rdna3agdc_k5_caps, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Caps,     "K5: plane-scaler capability queries answered");
SYSCTL_INT (_debug, OID_AUTO, rdna3agdc_k5_why,  CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Why, 0,   "K5: why the last K5 query was not answered");
SYSCTL_STRING(_debug, OID_AUTO, rdna3agdc_fbmap, CTLFLAG_RD | CTLFLAG_LOCKED, gAgdcFBMap, sizeof gAgdcFBMap, "the slot table 0x980 reports: n, then [i]id per IOFBDependentIndex");
SYSCTL_INT (_debug, OID_AUTO, rdna3agdc_maxfb,   CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcMaxFBOut, 0, "maxFramebuffers in the last 0x980 answer (0 = never asked)");
SYSCTL_INT (_debug, OID_AUTO, rdna3agdc_k5_asked, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Asked, 0, "bit i = a 0x921/0x711 named index i");
SYSCTL_INT (_debug, OID_AUTO, rdna3agdc_k5_link_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LinkMask, 0, "bit i = kAGDCLinkConfig answered for index i");
SYSCTL_INT (_debug, OID_AUTO, rdna3agdc_k5_caps_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5CapsMask, 0, "bit i = the plane-scaler capability answered for index i");
SYSCTL_INT (_debug, OID_AUTO, rdna3agdc_k5_last_refused_idx, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LastRefusedIdx, 0, "the index the last refused K5 query named (-1 none)");
#pragma clang diagnostic pop

static struct sysctl_oid *const kOids[] = {
    &sysctl__debug_rdna3agdc, &sysctl__debug_rdna3agdc_cmds, &sysctl__debug_rdna3agdc_refused,
    &sysctl__debug_rdna3agdc_last_refused, &sysctl__debug_rdna3agdc_k5, &sysctl__debug_rdna3agdc_k5_link,
    &sysctl__debug_rdna3agdc_k5_caps, &sysctl__debug_rdna3agdc_k5_why, &sysctl__debug_rdna3agdc_fbmap,
    &sysctl__debug_rdna3agdc_maxfb, &sysctl__debug_rdna3agdc_k5_asked, &sysctl__debug_rdna3agdc_k5_link_mask,
    &sysctl__debug_rdna3agdc_k5_caps_mask, &sysctl__debug_rdna3agdc_k5_last_refused_idx,
};

/* ------------------------------------------------------------------ */

class RDNA3AGDC : public IOService {
    OSDeclareDefaultStructors(RDNA3AGDC)
    IONotifier *fPub = nullptr, *fTerm = nullptr;
    bool fSysctl = false;
    static bool fbChanged(void *target, void *ref, IOService *s, IONotifier *n);
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(RDNA3AGDC, IOService)

/* a framebuffer published or went away: make the object (boot-arg) or
 * rescan its table, off the notification thread */
bool RDNA3AGDC::fbChanged(void *, void *, IOService *, IONotifier *)
{
    if (gAgdcWant == 1)
        rd3AgdcKick();
    return true;
}

bool RDNA3AGDC::start(IOService *provider)
{
    char off[8];
    if (PE_parse_boot_argn("-rdna3off", off, sizeof(off)))
        return false;
    if (!IOService::start(provider))
        return false;
    gTabLock = IOLockAlloc();
    gAgdcCall = thread_call_allocate(rd3AgdcWork, nullptr);
    if (!gTabLock || !gAgdcCall) {
        AGLOG("no lock / thread call: not starting");
        if (gAgdcCall) { thread_call_free(gAgdcCall); gAgdcCall = nullptr; }
        if (gTabLock) { IOLockFree(gTabLock); gTabLock = nullptr; }
        return false;
    }
    gAgdcLoader = this;
    setProperty("RDNA3,AGDCSource", AGDC_SRC);
    for (unsigned i = 0; i < sizeof kOids / sizeof kOids[0]; i++)
        sysctl_register_oid(kOids[i]);
    fSysctl = true;
    const uint32_t arg = rd3_arg("rdna3agdc", 0);
    if (arg) {
        gAgdcK5 = arg == 2 ? 0 : 1;
        gAgdcWant = 1;
        rd3SetStateProp("armed: waiting for a framebuffer");
    } else {
        rd3SetStateProp("idle (sysctl debug.rdna3agdc=1)");
    }
    OSDictionary *m = serviceMatching(kFBClass);
    if (m) {
        fPub = addMatchingNotification(gIOFirstPublishNotification, m, &RDNA3AGDC::fbChanged, this, nullptr, 0);
        m->release();
    }
    m = serviceMatching(kFBClass);
    if (m) {
        fTerm = addMatchingNotification(gIOTerminatedNotification, m, &RDNA3AGDC::fbChanged, this, nullptr, 0);
        m->release();
    }
    AGLOG("build %s: %s", AGDC_SRC, arg ? (gAgdcK5 ? "rdna3agdc: up when the first framebuffer publishes, K5 on"
                                                  : "rdna3agdc=2: up when the first framebuffer publishes, K5 off")
                                        : "idle until `sysctl debug.rdna3agdc=1`");
    registerService();
    return true;
}

void RDNA3AGDC::stop(IOService *provider)
{
    if (fPub) { fPub->remove(); fPub = nullptr; }
    if (fTerm) { fTerm->remove(); fTerm = nullptr; }
    if (fSysctl) {
        for (unsigned i = 0; i < sizeof kOids / sizeof kOids[0]; i++)
            sysctl_unregister_oid(kOids[i]);
        fSysctl = false;
    }
    if (gAgdcCall) {
        thread_call_cancel(gAgdcCall);
        int spins = 0;
        while (gAgdcBusy && spins++ < 200) IOSleep(10);
        if (gAgdcBusy) AGLOG("stop: the worker still runs after 2 s: its thread call stays allocated");
        else { thread_call_free(gAgdcCall); gAgdcCall = nullptr; }
    }
    if (!gAgdcBusy) {
        gAgdcWant = 0;
        if (gAgdc) rd3AgdcWorkBody();
        rd3ReleaseAll();
        if (!gAgdc && gTabLock) { IOLockFree(gTabLock); gTabLock = nullptr; }
    }
    gAgdcLoader = nullptr;
    IOService::stop(provider);
}
