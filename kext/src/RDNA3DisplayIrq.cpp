/*
 * RDNA3DisplayIrq.cpp - DRIVER-PLAN N42: the DCN vblank / page-flip
 * interrupts of the display heads, delivered to the framebuffer.
 *
 * amdgpu's model (amdgpu_dm.c, amdgpu_dm_irq.c): dcn10_register_irq_handlers
 * registers the OTG VSTARTUP (DC_IRQ_SOURCE_VBLANKn), VUPDATE_NO_LOCK
 * (VUPDATEn) and HUBP flip (PFLIPn) sources on SOC15_IH_CLIENTID_DCE;
 * amdgpu_dm_irq_handler acks each IV (dc_interrupt_ack) and runs the high-irq
 * handler at once: dm_crtc_high_irq -> drm_crtc_handle_vblank, and
 * dm_pflip_high_irq, which sends the page-flip event at flip done. The
 * sources are enabled while a CRTC needs them (dm_set_vblank /
 * amdgpu_dm_set_pflip_irq_state -> dc_interrupt_set) and set again after a
 * reset or resume (amdgpu_irq_gpu_reset_resume_helper,
 * amdgpu_dm_irq_resume_early).
 *
 * Here: the framebuffer of a head registers a function (rdna3DisplayIrq,
 * accel/rdna3k_abi.h) and the sources it wants are enabled on its OTG /
 * HUBP (hw/dcirq.c); the bring-up's IH handler (its MSI work loop, fIhLock
 * held) calls displayIrq for every DCN IV: ack, count, timestamp, the storm
 * guard (common/rdna3_vbl_irq.h), then the framebuffer's function - which
 * only records and schedules its HIGH ONCE thread call (VBL) or wakes its
 * latch waiters (flip done). Nothing heavier runs at interrupt level.
 *
 * Switches: rdna3vblirq (default 1, rd3_vblirq_on): 0 = no source is ever
 * enabled, the framebuffer polls the OTG counter as before N42;
 * debug.rdna3_vblirq (read / write): 0 disables the sources at once (the
 * framebuffers fall back to their poll within 3 frames), 1 enables them
 * again and forgets the storms. Counters: debug.rdna3_dispirq (key=value,
 * tools/triage_reports.py 'DISPLAY IRQ') and the RDNA3,DisplayIrq property.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"
#include "RDNA3Bringup.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <sys/sysctl.h>
#include <sys/errno.h>

extern "C" {
#include "rdna3_hw.h"
#include "rdna3_bootargs.h"
#include "dcn32.h"
#include "ih60.h"
#include "dcirq.h"
#include "rdna3_vbl_irq.h"
}
#include "rdna3k_abi.h"

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

enum { kDiVblank = 0, kDiPflip = 1, kDiVupdate = 2, kDiKinds = 3 };
static const uint32_t kDiDcKind[kDiKinds] = { RD_DCIRQ_VBLANK, RD_DCIRQ_PFLIP, RD_DCIRQ_VUPDATE };
static const char *const kDiName[kDiKinds] = { "VBLANK", "PFLIP", "VUPDATE" };

struct RDNA3DispIrqHead {
    RD3KDisplayIrqFn fn;
    void            *target;
    uint32_t         want;          /* RD3K_DISPIRQ_* asked */
    uint32_t         enabled;       /* enabled in hardware at otg / pipe */
    uint32_t         why;           /* RD3K_DISPIRQ_WHY_* of a wanted source left off */
    uint32_t         otg, pipe;     /* where they are enabled (~0u: nowhere yet) */
    uint64_t         cnt[kDiKinds];
    uint64_t         storms;
    uint32_t         regs;          /* registrations */
};
struct RDNA3DispIrq {
    IOLock          *lock;          /* the table, the sync, the framebuffers' functions run under it */
    IOSimpleLock    *reg;           /* the read-modify-writes of the enable / ack registers */
    uint32_t         arg;           /* rdna3vblirq */
    volatile uint32_t on;           /* debug.rdna3_vblirq */
    RDNA3DispIrqHead h[RD3K_MAX_HEADS];
    struct rd3_virq_storm storm[kDiKinds][RD_DCIRQ_INSTS];
    uint32_t         stormed[kDiKinds];   /* bit n: instance n cut after a storm */
    uint64_t         total, unrouted, badInst, regFails, syncs;
    uint32_t         unroutedLogs, regFailLogs;
    bool             sysctl;
};

static RDNA3Accel *gDirqOwner;
static SInt32 gDirqSysctlBusy;

static uint32_t diInst(const RDNA3DispIrqHead *H, int k)
{
    return k == kDiPflip ? H->pipe : H->otg;
}

/* dc_interrupt_set for one source, under the register lock */
static int diSet(RDNA3DispIrq *d, rdna3_dev *dev, int k, uint32_t inst, int enable)
{
    rdna3_dc_irq_source s = { kDiDcKind[k], inst };
    IOSimpleLockLock(d->reg);
    int r = dcirq_set(dev, s, enable);
    int rb = r == RDNA3_OK ? dcirq_enabled(dev, s) : -1;
    IOSimpleLockUnlock(d->reg);
    if (r != RDNA3_OK)
        return r;
    return rb == (enable ? 1 : 0) ? RDNA3_OK : RDNA3_EIO;
}

/* ---------------------------- the handler ---------------------------- */

bool RDNA3Accel::displayIrq(const rdna3_iv *iv)
{
    rdna3_dc_irq_source s;
    RDNA3DispIrq *d = fDirq;
    if (!dcirq_to_irq_source(iv->client_id, iv->src_id, iv->src_data[0], &s) || s.kind == RD_DCIRQ_HPD ||
        s.kind == RD_DCIRQ_HPDRX)
        return false;
    if (!d || !fDispDev)
        return true;   /* a DCN IV with no display side: claimed, nothing to do */
    const uint64_t now = mach_absolute_time();
    const int k = s.kind == RD_DCIRQ_VBLANK ? kDiVblank : s.kind == RD_DCIRQ_PFLIP ? kDiPflip : kDiVupdate;
    d->total++;
    if (s.inst >= RD_DCIRQ_INSTS) {
        d->badInst++;   /* OTG / HUBP 4, 5: no such instance on DCN 3.2 (a dummy entry) */
        return true;
    }
    /* amdgpu_dm_irq_handler: dc_interrupt_ack before the handler */
    IOSimpleLockLock(d->reg);
    dcirq_ack(fDispDev, s);
    IOSimpleLockUnlock(d->reg);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(now, &ns);
    const bool storm = rd3_virq_storm_note(&d->storm[k][s.inst], ns) != 0;
    if (storm) {
        /* no amdgpu counterpart: a source raising without end would keep
         * this work loop (fences, faults) busy; cut it, the poll takes over */
        diSet(d, fDispDev, k, s.inst, 0);
        OSBitOrAtomic(1u << s.inst, &d->stormed[k]);
        LOG("display irq: %s %u interrupt storm: more than %u in %u ms -- the source is disabled, the framebuffer's "
            "poll delivers (debug.rdna3_vblirq=1 enables it again)", kDiName[k], s.inst, RD3_VIRQ_STORM_MAX,
            (unsigned)(RD3_VIRQ_STORM_WINDOW_NS / 1000000ull));
    }
    IOLockLock(d->lock);
    int head = -1;
    for (uint32_t h = 0; h < RD3K_MAX_HEADS; h++) {
        RDNA3DispIrqHead *H = &d->h[h];
        if ((storm || (H->enabled & (1u << k))) && diInst(H, k) == s.inst) {
            head = (int)h;
            break;
        }
    }
    if (head < 0) {
        d->unrouted++;
        if (d->unroutedLogs < 4 || (d->unrouted & 1023) == 0) {
            d->unroutedLogs++;
            LOG("display irq: %s %u with no framebuffer registered for it (%llu such): acked, dropped", kDiName[k],
                s.inst, (unsigned long long)d->unrouted);
        }
    } else {
        RDNA3DispIrqHead *H = &d->h[head];
        H->cnt[k]++;
        if (storm) {
            H->storms++;
            H->enabled &= ~(1u << k);
            H->why = RD3K_DISPIRQ_WHY_STORM;
        } else if (H->fn && d->on) {
            /* dm_crtc_high_irq / dm_pflip_high_irq: the framebuffer records
             * and schedules; the frame counter for its VBL bookkeeping */
            const uint32_t frame = k == kDiVblank && fDcn && (uint32_t)head < fDcn->nheads
                                       ? dcn32_frame_count(fDispDev, fDcn, (uint32_t)head) : 0;
            H->fn(H->target, (unsigned)head, 1u << k, frame, now);
        }
    }
    IOLockUnlock(d->lock);
    return true;
}

/* --------------------------- enable / disable --------------------------- */

void RDNA3Accel::displayIrqSync(const char *why, bool force)
{
    RDNA3DispIrq *d = fDirq;
    if (!d || !fDispDev)
        return;
    const bool live = fBringup && fBringup->irqLive();
    IOLockLock(d->lock);
    d->syncs++;
    for (uint32_t h = 0; h < RD3K_MAX_HEADS; h++) {
        RDNA3DispIrqHead *H = &d->h[h];
        const bool headOk = fDcn && h < fDcn->nheads && fDcn->head[h].otg < RD_DCIRQ_INSTS &&
                            fDcn->head[h].pipe < RD_DCIRQ_INSTS;
        const uint32_t otg = headOk ? fDcn->head[h].otg : ~0u, pipe = headOk ? fDcn->head[h].pipe : ~0u;
        uint32_t desired = H->fn ? H->want : 0, whyNot = RD3K_DISPIRQ_WHY_NONE;
        if (desired && !d->on) {
            desired = 0;
            whyNot = RD3K_DISPIRQ_WHY_SYSCTL;
        } else if (desired && !live) {
            desired = 0;
            whyNot = RD3K_DISPIRQ_WHY_NOIRQ;
        } else if (desired && !headOk) {
            desired = 0;
            whyNot = RD3K_DISPIRQ_WHY_HEAD;
        }
        for (int k = 0; k < kDiKinds; k++) {
            const uint32_t bit = 1u << k, inst = k == kDiPflip ? pipe : otg;
            if ((desired & bit) && (d->stormed[k] & (1u << inst))) {
                desired &= ~bit;
                whyNot = RD3K_DISPIRQ_WHY_STORM;
            }
            /* off where it was, when it moved or is no longer wanted */
            if ((H->enabled & bit) && (!(desired & bit) || diInst(H, k) != inst)) {
                diSet(d, fDispDev, k, diInst(H, k), 0);
                H->enabled &= ~bit;
            }
        }
        H->otg = otg;
        H->pipe = pipe;
        for (int k = 0; k < kDiKinds; k++) {
            const uint32_t bit = 1u << k;
            if (!(desired & bit) || ((H->enabled & bit) && !force))
                continue;
            const int r = diSet(d, fDispDev, k, diInst(H, k), 1);
            if (r == RDNA3_OK) {
                H->enabled |= bit;
            } else {
                H->enabled &= ~bit;
                whyNot = RD3K_DISPIRQ_WHY_REG;
                d->regFails++;
                if (d->regFailLogs++ < 8)
                    LOG("display irq: head %u: %s on %s %u did not take (%d) at %s -- the framebuffer polls", h,
                        kDiName[k], k == kDiPflip ? "HUBP" : "OTG", diInst(H, k), r, why);
            }
        }
        H->why = whyNot;
    }
    IOLockUnlock(d->lock);
    displayIrqPublish();
}

/* ------------------------- the framebuffer's call ------------------------- */

IOReturn RDNA3Accel::displayIrqCall(RD3KDisplayIrq *r)
{
    RDNA3DispIrq *d = fDirq;
    if (!r || r->version != RD3K_DISPIRQ_VERSION || r->head >= RD3K_MAX_HEADS)
        return kIOReturnBadArgument;
    if (!d || !d->arg)
        return kIOReturnUnsupported;   /* rdna3vblirq=0: the framebuffer polls */
    RDNA3DispIrqHead *H = &d->h[r->head];
    switch (r->op) {
    case RD3K_DISPIRQ_OP_REGISTER: {
        if (!r->fn || !(r->want & (RD3K_DISPIRQ_VBLANK | RD3K_DISPIRQ_PFLIP | RD3K_DISPIRQ_VUPDATE)))
            return kIOReturnBadArgument;
        if (!fDcn || !fDispDev || r->head >= fDcn->nheads)
            return kIOReturnNotReady;
        if (!fBringup || !fBringup->irqLive()) {
            r->enabled = 0;
            r->why = RD3K_DISPIRQ_WHY_NOIRQ;
            return kIOReturnNotReady;   /* the bring-up's MSI handler is not up yet: asked again later */
        }
        IOLockLock(d->lock);
        H->fn = r->fn;
        H->target = r->target;
        H->want = r->want & (RD3K_DISPIRQ_VBLANK | RD3K_DISPIRQ_PFLIP | RD3K_DISPIRQ_VUPDATE);
        H->regs++;
        IOLockUnlock(d->lock);
        displayIrqSync("register", false);
        LOG("display irq: head %u (OTG %u, HUBP %u): the framebuffer registered for 0x%x, enabled 0x%x%s (why %u)",
            r->head, H->otg, H->pipe, H->want, H->enabled, H->enabled == H->want ? "" : " -- NOT all", H->why);
        break;
    }
    case RD3K_DISPIRQ_OP_UNREGISTER:
        IOLockLock(d->lock);   /* a function running now finishes first */
        H->fn = nullptr;
        H->target = nullptr;
        H->want = 0;
        IOLockUnlock(d->lock);
        displayIrqSync("unregister", false);
        LOG("display irq: head %u: the framebuffer unregistered (VBLANK %llu, PFLIP %llu interrupts)", r->head,
            (unsigned long long)H->cnt[kDiVblank], (unsigned long long)H->cnt[kDiPflip]);
        break;
    case RD3K_DISPIRQ_OP_STATS:
        displayIrqPublish();
        break;
    default:
        return kIOReturnBadArgument;
    }
    IOLockLock(d->lock);
    r->enabled = H->enabled;
    r->why = H->why;
    r->otg = H->otg;
    r->pipe = H->pipe;
    r->vblank = H->cnt[kDiVblank];
    r->pflip = H->cnt[kDiPflip];
    r->vupdate = H->cnt[kDiVupdate];
    r->storms = H->storms;
    IOLockUnlock(d->lock);
    return kIOReturnSuccess;
}

/* ------------------------------ counters ------------------------------ */

int RDNA3Accel::displayIrqReport(char *buf, size_t len)
{
    RDNA3DispIrq *d = fDirq;
    if (!d || !buf || len < 64)
        return ENXIO;
    size_t o = 0;
    IOLockLock(d->lock);
    o += (size_t)snprintf(buf + o, len - o,
                          "vblirq=%u on=%u msi=%u hpdirq=%u hpd=%u total=%llu unrouted=%llu badinst=%llu "
                          "regfails=%llu stormed=0x%x/0x%x/0x%x syncs=%llu",
                          d->arg, d->on, fBringup && fBringup->irqLive() ? 1u : 0u, rd3_arg("rdna3hpdirq", 0),
                          fHpdIrqs, (unsigned long long)d->total, (unsigned long long)d->unrouted,
                          (unsigned long long)d->badInst, (unsigned long long)d->regFails, d->stormed[kDiVblank],
                          d->stormed[kDiPflip], d->stormed[kDiVupdate], (unsigned long long)d->syncs);
    for (uint32_t h = 0; h < RD3K_MAX_HEADS && o < len; h++) {
        const RDNA3DispIrqHead *H = &d->h[h];
        if (!H->regs && !H->cnt[kDiVblank] && !H->cnt[kDiPflip])
            continue;
        o += (size_t)snprintf(buf + o, len - o,
                              " | h%u reg=%u want=0x%x en=0x%x why=%u otg=%d pipe=%d vblank=%llu pflip=%llu "
                              "vupdate=%llu storms=%llu",
                              h, H->fn ? 1u : 0u, H->want, H->enabled, H->why, H->otg == ~0u ? -1 : (int)H->otg,
                              H->pipe == ~0u ? -1 : (int)H->pipe, (unsigned long long)H->cnt[kDiVblank],
                              (unsigned long long)H->cnt[kDiPflip], (unsigned long long)H->cnt[kDiVupdate],
                              (unsigned long long)H->storms);
    }
    IOLockUnlock(d->lock);
    return 0;
}

void RDNA3Accel::displayIrqPublish()
{
    char buf[640];
    if (displayIrqReport(buf, sizeof(buf)) == 0)
        setProperty("RDNA3,DisplayIrq", buf);
}

int RDNA3Accel::displayIrqSysctl(int *on, bool set)
{
    RDNA3DispIrq *d = fDirq;
    if (!d)
        return ENXIO;
    if (set) {
        const uint32_t v = *on ? 1u : 0u;
        if (v && !d->on)
            for (int k = 0; k < kDiKinds; k++)
                d->stormed[k] = 0;   /* by hand: the storms forgotten */
        d->on = v;
        LOG("display irq: debug.rdna3_vblirq=%u: the vblank / pflip sources %s", v,
            v ? "enabled again for the registered framebuffers" : "disabled (the framebuffers poll)");
        displayIrqSync("sysctl", false);
    }
    *on = (int)d->on;
    return 0;
}

static int rdna3_vblirq_sysctl SYSCTL_HANDLER_ARGS
{
    (void)arg1; (void)arg2;
    int err = ENXIO, v = 0;
    OSIncrementAtomic(&gDirqSysctlBusy);
    RDNA3Accel *a = gDirqOwner;
    if (a && !(err = a->displayIrqSysctl(&v, false))) {
        err = sysctl_handle_int(oidp, &v, 0, req);
        if (!err && req->newptr)
            err = a->displayIrqSysctl(&v, true);
    }
    OSDecrementAtomic(&gDirqSysctlBusy);
    return err;
}

static int rdna3_dispirq_sysctl SYSCTL_HANDLER_ARGS
{
    (void)oidp; (void)arg1; (void)arg2;
    const size_t cap = 768;
    int err = ENXIO;
    OSIncrementAtomic(&gDirqSysctlBusy);
    RDNA3Accel *a = gDirqOwner;
    char *buf = a ? (char *)IOMalloc(cap) : nullptr;
    if (buf) {
        bzero(buf, cap);
        err = a->displayIrqReport(buf, cap);
        if (!err)
            err = SYSCTL_OUT(req, buf, strlen(buf) + 1);
        IOFree(buf, cap);
    } else if (a) {
        err = ENOMEM;
    }
    OSDecrementAtomic(&gDirqSysctlBusy);
    return err;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
SYSCTL_PROC(_debug, OID_AUTO, rdna3_vblirq, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, nullptr, 0,
            rdna3_vblirq_sysctl, "I",
            "DRIVER-PLAN N42: 1 = the DCN vblank / pflip interrupts drive the framebuffers' VBL and flip done, 0 = "
            "their OTG poll alone (rdna3vblirq=0 at boot: never)");
SYSCTL_PROC(_debug, OID_AUTO, rdna3_dispirq, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED, nullptr, 0,
            rdna3_dispirq_sysctl, "A",
            "DRIVER-PLAN N42: display interrupts per head (key=value; tools/triage_reports.py 'DISPLAY IRQ')");
#pragma clang diagnostic pop

/* ------------------------------ lifecycle ------------------------------ */

void RDNA3Accel::displayIrqInit()
{
    if (fDirq)
        return;
    RDNA3DispIrq *d = (RDNA3DispIrq *)IOMalloc(sizeof(RDNA3DispIrq));
    if (!d) {
        LOG("display irq: no memory: the framebuffers poll the OTG counter");
        return;
    }
    bzero(d, sizeof(*d));
    d->lock = IOLockAlloc();
    d->reg = IOSimpleLockAlloc();
    if (!d->lock || !d->reg) {
        if (d->lock) IOLockFree(d->lock);
        if (d->reg) IOSimpleLockFree(d->reg);
        IOFree(d, sizeof(*d));
        LOG("display irq: no locks: the framebuffers poll the OTG counter");
        return;
    }
    for (uint32_t h = 0; h < RD3K_MAX_HEADS; h++)
        d->h[h].otg = d->h[h].pipe = ~0u;
    d->arg = rd3_vblirq_on() ? 1u : 0u;
    d->on = 1;
    fDirq = d;
    gDirqOwner = this;
    sysctl_register_oid(&sysctl__debug_rdna3_vblirq);
    sysctl_register_oid(&sysctl__debug_rdna3_dispirq);
    d->sysctl = true;
    LOG("display irq: rdna3vblirq=%u (default 1), rdna3hpdirq=%u (default 0): the vblank / pflip interrupts %s; "
        "HPD interrupts %s", d->arg, rd3_arg("rdna3hpdirq", 0),
        d->arg ? "drive a framebuffer that registers (with MSI), its OTG poll as the fallback"
               : "are never enabled, the framebuffers poll the OTG counter",
        rd3_arg("rdna3hpdirq", 0) ? "start a connector probe" : "are counted and logged only");
    displayIrqPublish();
}

/* after the bring-up is gone (no handler can run): every source off */
void RDNA3Accel::displayIrqFree()
{
    RDNA3DispIrq *d = fDirq;
    if (!d)
        return;
    if (d->sysctl) {
        sysctl_unregister_oid(&sysctl__debug_rdna3_vblirq);
        sysctl_unregister_oid(&sysctl__debug_rdna3_dispirq);
        gDirqOwner = nullptr;
        for (int i = 0; i < 200 && gDirqSysctlBusy; i++)   /* a read in flight finishes first */
            IOSleep(10);
        if (gDirqSysctlBusy)
            LOG("display irq: a sysctl was still being read after 2 s; going on");
    }
    IOLockLock(d->lock);
    for (uint32_t h = 0; h < RD3K_MAX_HEADS; h++) {
        RDNA3DispIrqHead *H = &d->h[h];
        for (int k = 0; k < kDiKinds; k++)
            if ((H->enabled & (1u << k)) && fDispDev)
                diSet(d, fDispDev, k, diInst(H, k), 0);
        H->enabled = 0;
        H->fn = nullptr;
    }
    IOLockUnlock(d->lock);
    fDirq = nullptr;
    IOLockFree(d->lock);
    IOSimpleLockFree(d->reg);
    IOFree(d, sizeof(*d));
}
