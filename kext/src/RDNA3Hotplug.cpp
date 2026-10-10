/*
 * RDNA3Hotplug.cpp - D6: monitors plugged and unplugged after boot
 * (rdna3hotplug=1; nothing here runs without it).
 *
 * amdgpu: program_hpd_filter on every link at creation (link_factory.c),
 * then handle_hpd_irq -> handle_hpd_irq_helper -> dc_link_detect ->
 * amdgpu_dm_update_connector_after_detect -> drm_kms_helper_connector_
 * hotplug_event. Here (hw/dchpd.c):
 *
 *   start      the toggle filter of every VBIOS path's HPD pin
 *              (program_hpd_filter: TMDS 500/100 ms, DP 80/0 ms), the
 *              pins' filtered sense as the start state
 *   poll       every RD_HPD_POLL_MS a thread call reads DC_HPD_SENSE_DELAYED
 *              of each pin (link_get_hpd_state); a new level counts after
 *              RD_HPD_DEBOUNCE equal looks. The HPD interrupt (B7, with
 *              rdna3msi=1) only brings the next look forward.
 *   change     the sink read again (dchpd_detect: dc_link_detect with
 *              dm_helpers_read_local_edid's tries), the connector table and
 *              RDNA3,Connector<n>,* rebuilt (stale keys removed), then:
 *                the boot head's cable   its pipe stays as the GOP left it;
 *                                        the head's generation moves
 *                a TMDS sink on a free   rdna3mirror on, no clone lit: the
 *                connector               clone path lights it (displayMirror;
 *                                        rdna3mirror=2: its own framebuffer)
 *                the clone's cable       the clone's teardown
 *                                        (displayMirrorDisable)
 *   framebuffer  RDNA3Framebuffer polls rdna3DisplayHotplug (per-head
 *              generation, accel/rdna3k_abi.h) and raises IOFramebuffer's
 *              connect-change interrupt.
 *
 * Polling, not the IH, decides: the DCN HPD interrupt needs MSI (rdna3msi=1)
 * and has not been seen on the card yet; the poll costs five register reads
 * per half second and works from rdna3lvl=2.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>

extern "C" {
#include "rdna3_hw.h"
#include "rdna3_bootargs.h"
#include "dcn32.h"
#include "dcaux.h"
#include "vbios.h"
#include "dcn32_clone.h"
#include "dchpd.h"
}
#include "rdna3k_abi.h"

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

struct RDNA3HotplugState {
    rdna3_hpd_track track;
    thread_call_t   call;
    volatile bool   stop;
    uint64_t        lastPoll;                 /* mach absolute time */
    uint32_t        headGen[RD3K_MAX_HEADS];
    uint32_t        actions, clonesOn, clonesOff, bootReports, noEdid;
};

static uint64_t hpPollAbs()
{
    uint64_t abs = 0;
    nanoseconds_to_absolutetime((uint64_t)RD_HPD_POLL_MS * 1000000ull, &abs);
    return abs;
}

void RDNA3Accel::hotplugInit()
{
    if (!rd3_arg("rdna3hotplug", 0) || !fDispDev || !fDispLock || fHp)
        return;
    fHp = (RDNA3HotplugState *)IOMalloc(sizeof(RDNA3HotplugState));
    if (!fHp)
        return;
    bzero(fHp, sizeof(*fHp));
    fHp->call = thread_call_allocate_with_options((thread_call_func_t)hotplugTick, this,
                                                  THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (!fHp->call) {
        IOFree(fHp, sizeof(*fHp));
        fHp = nullptr;
        return;
    }
    IOLockLock(fDispLock);
    /* program_hpd_filter for every link the VBIOS lists */
    for (uint32_t i = 0; fPaths && i < fNPaths; i++) {
        const rdna3_display_path *p = &fPaths[i];
        uint32_t on = 0, off = 0, tf = 0, ft = 0;
        if (p->hpd < 0)
            continue;
        int r = dchpd_program_filter(fDispDev, (uint32_t)p->hpd, p->connector, 1);
        dchpd_filter_info(fDispDev, (uint32_t)p->hpd, &on, &off, &tf, &ft);
        LOG("hotplug: HPD%d (%s-%u) filter %s: connect %u ms, disconnect %u ms "
            "(TOGGLE_FILT_CNTL 0x%08x, FAST_TRAIN_CNTL 0x%08x as the VBIOS left it)",
            p->hpd + 1, vbios_connector_name(p->connector), p->connector_enum,
            r ? "not programmed" : "set", on, off, tf, ft);
    }
    dchpd_track_init(&fHp->track, fDispDev, RD_AUX_ENGINES, RD_HPD_DEBOUNCE);
    IOLockUnlock(fDispLock);
    hotplugPublish(0, 0);
    setProperty("RDNA3,Hotplug", "polling HPD every 500 ms (HPD interrupts only hurry the next look)");
    LOG("hotplug: polling %u HPD pins every %u ms, a change counts after %u looks", fHp->track.npins,
        RD_HPD_POLL_MS, RD_HPD_DEBOUNCE);
    clock_get_uptime(&fHp->lastPoll);
    thread_call_enter_delayed(fHp->call, fHp->lastPoll + hpPollAbs());
}

/* the bring-up is going or the system shuts down: no more looks, none
 * running (a running one may be using the DMCUB) */
void RDNA3Accel::hotplugStop()
{
    if (!fHp)
        return;
    fHp->stop = true;
    if (fHp->call)
        thread_call_cancel_wait(fHp->call);
}

void RDNA3Accel::hotplugFree()
{
    if (!fHp)
        return;
    hotplugStop();
    if (fHp->call)
        thread_call_free(fHp->call);
    IOFree(fHp, sizeof(*fHp));
    fHp = nullptr;
}

/* an HPD interrupt (displayHotplugWork): look now rather than at the next
 * tick, unless the last look was less than half a period ago (a bouncing
 * cable must not count as several looks) */
bool RDNA3Accel::hotplugKick()
{
    if (!fHp || fHp->stop)
        return false;
    uint64_t now;
    clock_get_uptime(&now);
    if (now - fHp->lastPoll >= hpPollAbs() / 2)
        thread_call_enter(fHp->call);
    return true;
}

void RDNA3Accel::hotplugTick(void *owner, void *)
{
    RDNA3Accel *a = (RDNA3Accel *)owner;
    RDNA3HotplugState *hp = a->fHp;
    uint32_t raw = 0, changed;
    if (!hp || hp->stop || !a->fDispDev)
        return;
    clock_get_uptime(&hp->lastPoll);
    changed = dchpd_track_poll(&hp->track, a->fDispDev, &raw);
    if (raw || changed)
        a->hotplugPublish(raw, changed);
    if (changed)
        a->hotplugHandle(changed);
    if (!hp->stop) {
        uint64_t now;
        clock_get_uptime(&now);
        thread_call_enter_delayed(hp->call, now + hpPollAbs());
    }
}

/* RDNA3,HPDState: "HPD1 1 HPD2 0 ..." (debounced), a pin whose last look
 * differs marked "(now n)"; the report samples it over time */
void RDNA3Accel::hotplugPublish(uint32_t raw, uint32_t changed)
{
    char val[160];
    size_t n = 0;
    val[0] = 0;
    for (uint32_t p = 0; p < fHp->track.npins && n < sizeof(val); p++) {
        int s = fHp->track.state[p], r = fHp->track.raw[p];
        n += (size_t)snprintf(val + n, sizeof(val) - n, "%sHPD%u %d", p ? " " : "", p + 1, s);
        if (r != s && n < sizeof(val))
            n += (size_t)snprintf(val + n, sizeof(val) - n, "(now %d)", r);
    }
    setProperty("RDNA3,HPDState", val);
    setNumber("RDNA3,HotplugGeneration", fHp->track.generation);
    setNumber("RDNA3,HotplugPolledEvents", fHp->track.events);
    setNumber("RDNA3,HotplugGlitches", fHp->track.glitches);
    if (raw && !changed)
        LOG("hotplug: %s (pins 0x%x moved, waiting for a second look)", val, (unsigned)raw);
}

/* displayProbeConnectors only sets keys: drop the ones of connectors that
 * are gone or lost their sink. Called with fDispLock held. */
void RDNA3Accel::hotplugTidyProps()
{
    static const char *all[] = { "AUX", "HPD", "Type", "Transmitter", "DPCD", "EDID", "EDIDSource",
                                 "Monitor", "Modes", "ModesUsable" };
    for (uint32_t i = 0; i < RD_AUX_ENGINES; i++) {
        const rdna3_connector *c = fConnectors && i < fNConnectors ? &fConnectors[i] : nullptr;
        for (uint32_t k = 0; k < sizeof(all) / sizeof(all[0]); k++) {
            bool keep = c != nullptr;
            if (c && k >= 5)                       /* EDID and what came from it */
                keep = c->edid_len > 0;
            if (c && k == 4)
                keep = c->dp != 0;
            if (c && (k == 2 || k == 3)) {         /* only with a VBIOS path on its DDC line */
                keep = false;
                for (uint32_t j = 0; fPaths && j < fNPaths; j++)
                    keep |= fPaths[j].ddc_line >= 0 && (uint32_t)fPaths[j].ddc_line == c->aux;
            }
            if (keep)
                continue;
            char key[48];
            snprintf(key, sizeof(key), "RDNA3,Connector%u,%s", i, all[k]);
            removeProperty(key);
        }
    }
}

/* handle_hpd_irq_helper for the pins in 'changed' */
void RDNA3Accel::hotplugHandle(uint32_t changed)
{
    RDNA3HotplugState *hp = fHp;
    rdna3_connector *c = (rdna3_connector *)IOMalloc(sizeof(rdna3_connector));
    uint32_t mirror = rd3_arg("rdna3mirror", 0);
    if (!c)
        return;
    for (uint32_t pin = 0; pin < hp->track.npins && !hp->stop; pin++) {
        if (!(changed & (1u << pin)))
            continue;
        int on = hp->track.state[pin] > 0;
        int aux = dchpd_pin_aux(pin, fPaths, fNPaths);
        const rdna3_display_path *p = dchpd_pin_path(pin, fPaths, fNPaths);
        int det;
        char line[192];
        /* dc_link_detect: the sink again (the EDID with its tries), then
         * the table and the RDNA3,Connector<n>,* keys from scratch */
        IOLockLock(fDispLock);
        bzero(c, sizeof(*c));
        det = on ? dchpd_detect(fDispDev, (uint32_t)aux, c, RD_HPD_EDID_TRIES) : RD_HPD_NO_SINK;
        displayProbeConnectors();
        hotplugTidyProps();
        /* the heads this pin feeds: their framebuffers re-ask */
        for (uint32_t h = 0; fDcn && h < fDcn->nheads && h < RD3K_MAX_HEADS; h++)
            if (dchpd_head_pin(&fDcn->head[h], fPaths, fNPaths) == (int)pin)
                hp->headGen[h]++;
        rdna3_hp_view v;
        v.boot_pin = fDcn && fDcn->nheads ? dchpd_head_pin(&fDcn->head[0], fPaths, fNPaths) : -1;
        v.clone_pin = fMirrorOn && fMirror ? fMirror->hpd : -1;
        v.mirror = mirror;
        v.can_clone = fMirrorDm != nullptr && !fMirrorOn;   /* one clone at a time */
        int act = dchpd_decide(&v, pin, on, det == RD_HPD_SINK ? c : nullptr);
        IOLockUnlock(fDispLock);
        if (det == RD_HPD_SINK_NO_EDID)
            hp->noEdid++;
        snprintf(line, sizeof(line), "HPD%u %s on %s-%u (AUX%d%s): %s", pin + 1, on ? "connected" : "disconnected",
                 p ? vbios_connector_name(p->connector) : "?", p ? p->connector_enum : 0, aux,
                 !on ? "" : det == RD_HPD_SINK ? (c->dp ? ", DP sink" : ", TMDS sink with EDID")
                                               : ", no EDID", dchpd_action_name(act));
        LOG("hotplug: %s", line);
        hp->actions++;
        rdna3_dmub *dm = fMirrorDm;
        if (act == RD_HP_CLONE_OFF && dm) {
            LOG("hotplug: the second monitor is gone: switching its pipe off");
            displayMirrorDisable(dm);
            hp->clonesOff++;
        } else if (act == RD_HP_CLONE_ON && dm) {
            displayMirror(dm);
            hp->clonesOn += fMirrorOn ? 1 : 0;
            LOG("hotplug: second monitor %s", fMirrorOn ? "lit" : "NOT lit (see RDNA3,Mirror)");
        } else if (act == RD_HP_BOOT) {
            hp->bootReports++;
            setProperty("RDNA3,HotplugBootHead", on ? "connected (pipe as the GOP left it)"
                                                    : "disconnected (pipe kept lit)");
        }
        setProperty("RDNA3,HotplugLast", line);
    }
    IOFree(c, sizeof(*c));
    setNumber("RDNA3,HotplugActions", hp->actions);
    setNumber("RDNA3,HotplugClonesLit", hp->clonesOn);
    setNumber("RDNA3,HotplugClonesOff", hp->clonesOff);
    setNumber("RDNA3,HotplugBootReports", hp->bootReports);
    setNumber("RDNA3,HotplugNoEdid", hp->noEdid);
}

IOReturn RDNA3Accel::displayHotplugQuery(RD3KDisplayHotplug *q)
{
    if (!q || q->version != RD3K_DISPLAY_ABI_VERSION)
        return kIOReturnBadArgument;
    if (!fHp)
        return kIOReturnUnsupported;
    if (!fDcn || q->head >= fDcn->nheads || q->head >= RD3K_MAX_HEADS)
        return kIOReturnNotReady;
    /* reads only (a byte and a word the tick writes) */
    int pin = dchpd_head_pin(&fDcn->head[q->head], fPaths, fNPaths);
    q->generation = fHp->headGen[q->head];
    q->connected = pin < 0 || fHp->track.state[pin] != 0;   /* unknown route: assume a sink */
    q->pin = pin < 0 ? ~0u : (unsigned)pin;
    q->events = fHp->track.events;
    return kIOReturnSuccess;
}
