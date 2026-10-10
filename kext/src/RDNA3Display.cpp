/*
 * RDNA3Display.cpp - Stage 3, core side: the display heads the VBIOS /
 * UEFI GOP lit, read from DCN 3.2 at rdna3lvl>=2 (reads only), and the
 * calls RDNA3Framebuffer makes over callPlatformFunction
 * (accel/rdna3k_abi.h):
 *
 *   rdna3DisplayInfo     heads, geometry, timing, BAR0 address, refresh
 *   rdna3DisplayFlip     scan out another VRAM surface (HUBP address/pitch)
 *   rdna3DisplayStatus   OTG frame counter, scanline, vblank, flip pending
 *   rdna3DisplayCrc      OTG CRC of the last frame
 *   rdna3DisplayCursor   hardware cursor image (VRAM) and position
 *
 * Nothing here writes a register until the framebuffer asks for a flip or
 * a CRC, and the framebuffer only starts with the boot-arg rdna3fb=1.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>

extern "C" {
#include "rdna3_hw.h"
#include "dcn32.h"
#include "dcaux.h"
#include "edid.h"
#include "vbios.h"
#include "atom.h"
#include "dcn32_mode.h"
}
#include "rdna3k_abi.h"

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

static void dispUdelay(void *, uint32_t us) { IODelay(us); }
static void dispMsleep(void *, uint32_t ms) { IOSleep(ms); }
static void dispWmb(void *) { OSSynchronizeIO(); }
static void dispVlog(void *, int level, const char *fmt, va_list ap)
{
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    IOLog("RDNA3: %s%s\n", level == RDNA3_LOG_ERR ? "ERROR: " : "", buf);
}

uint32_t RDNA3Accel::dispMmioRd(void *ctx, uint64_t off)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (!a->regValid(off))
        return ~0u;
    return *(volatile uint32_t *)(a->fMmio + off);
}

void RDNA3Accel::dispMmioWr(void *ctx, uint64_t off, uint32_t v)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    if (a->regValid(off))
        *(volatile uint32_t *)(a->fMmio + off) = v;
}

bool RDNA3Accel::displayInit(const amd_discovery *disc)
{
    rdna3_os os = {};
    os.ctx = this;
    os.udelay = dispUdelay;
    os.msleep = dispMsleep;
    os.vlog = dispVlog;
    os.wmb = dispWmb;
    os.mmio_rd = dispMmioRd;
    os.mmio_wr = dispMmioWr;

    fDispLock = IOLockAlloc();
    fDispDev = (rdna3_dev *)IOMalloc(sizeof(rdna3_dev));
    fDcn = (rdna3_dcn *)IOMalloc(sizeof(rdna3_dcn));
    if (!fDispLock || !fDispDev || !fDcn) {
        displayFree();
        return false;
    }
    bzero(fDispDev, sizeof(rdna3_dev));
    bzero(fDcn, sizeof(rdna3_dcn));
    rdna3_dev_init(fDispDev, &os, fMmioLen);
    fDispDev->vram_size = fVramSize;
    if (rdna3_dev_set_discovery(fDispDev, disc) != RDNA3_OK ||
        rdna3_read_fb_location(fDispDev) != RDNA3_OK) {
        LOG("display: no GC/DCN register map, heads unknown");
        displayFree();
        return false;
    }
    int n = dcn32_read_state(fDispDev, fDcn);
    setNumber("RDNA3,DisplayHeads", (uint64_t)(n > 0 ? n : 0));

    /* keep allocators off every boot scanout in the low GiB */
    for (uint32_t i = 0; i < fDcn->nheads; i++) {
        const rdna3_dcn_head *h = &fDcn->head[i];
        uint64_t off = h->address - fDispDev->fb_start;
        uint64_t end = off + (uint64_t)h->pitch_px * 4 * h->height;
        if (h->address >= fDispDev->fb_start && off < (1ull << 30) && end > fDispReserveEnd)
            fDispReserveEnd = end;
        LOG("display: head %u %ux%u pitch %u B at VRAM 0x%llx, %s", i, h->width, h->height,
            h->pitch_px * 4, (unsigned long long)off,
            dcn32_head_usable(fDispDev, h) ? "usable" : "NOT usable (tiled/DCC/format)");
    }
    fDispReserveEnd = (fDispReserveEnd + (2ull << 20) - 1) & ~((2ull << 20) - 1);
    setNumber("RDNA3,DisplayReserveEnd", fDispReserveEnd, 64);
    displayReadVbios();
    displayProbeConnectors();
    return true;
}

#define RD_ATOM_SCRATCH_BYTES (20u * 1024u)   /* amdgpu's minimum FB scratch */

/* The VBIOS display object table (connector -> UNIPHY, DDC line = AUX
 * engine, HPD pin) and an AtomBIOS context for the command tables Display
 * Core calls (transmitter control, pixel clock). Nothing is executed here:
 * the GOP already ran ASIC_Init and lit the boot display. */
void RDNA3Accel::displayReadVbios()
{
    int ret;
    fDispRom = (uint8_t *)IOMalloc(RD_VBIOS_MAX_LEN);
    fDispVbios = (rdna3_vbios *)IOMalloc(sizeof(rdna3_vbios));
    fPaths = (rdna3_display_path *)IOMalloc(sizeof(rdna3_display_path) * RD_VBIOS_MAX_PATHS);
    if (!fDispRom || !fDispVbios || !fPaths)
        return;
    bzero(fDispVbios, sizeof(rdna3_vbios));
    ret = vbios_read(fDispDev, fDispVbios, fDispRom, RD_VBIOS_MAX_LEN);
    if (ret != RDNA3_OK) {
        LOG("display: VBIOS not readable (%d), no display paths", ret);
        return;
    }
    ret = vbios_display_paths(fDispVbios, fPaths, RD_VBIOS_MAX_PATHS);
    fNPaths = ret > 0 ? (uint32_t)ret : 0;
    setNumber("RDNA3,DisplayPaths", fNPaths);
    for (uint32_t i = 0; i < fNPaths; i++) {
        const rdna3_display_path *p = &fPaths[i];
        char key[48], val[96];
        snprintf(key, sizeof(key), "RDNA3,DisplayPath%u", i);
        snprintf(val, sizeof(val), "%s-%u UNIPHY%c DDC%d HPD%d caps 0x%x max %u MHz",
                 vbios_connector_name(p->connector), p->connector_enum,
                 p->transmitter >= 0 ? 'A' + p->transmitter : '?', p->ddc_line + 1, p->hpd + 1,
                 p->encoder_caps, p->max_link_mhz);
        setProperty(key, val);
        LOG("display: path %u: %s", i, val);
    }

    fAtom = (rdna3_atom *)IOMalloc(sizeof(rdna3_atom));
    fAtomScratch = (uint32_t *)IOMalloc(RD_ATOM_SCRATCH_BYTES);
    if (!fAtom || !fAtomScratch)
        return;
    bzero(fAtomScratch, RD_ATOM_SCRATCH_BYTES);
    ret = atom_init(fAtom, fDispDev, fDispVbios, fAtomScratch, RD_ATOM_SCRATCH_BYTES);
    if (ret != RDNA3_OK) {
        IOFree(fAtom, sizeof(rdna3_atom));
        fAtom = nullptr;
        return;
    }
    {
        static const struct { unsigned idx; const char *name; } t[] = {
            { RD_ATOM_CMD_DIGX_ENCODER_CONTROL, "DIGxEncoderControl" },
            { RD_ATOM_CMD_DIG1_TRANSMITTER_CONTROL, "DIG1TransmitterControl" },
            { RD_ATOM_CMD_SET_PIXEL_CLOCK, "SetPixelClock" },
            { RD_ATOM_CMD_ENABLE_DISP_POWER_GATING, "EnableDispPowerGating" },
            { RD_ATOM_CMD_SET_DCE_CLOCK, "SetDCEClock" },
        };
        char buf[192];
        size_t n = 0;
        buf[0] = 0;
        for (size_t k = 0; k < sizeof(t) / sizeof(t[0]) && n < sizeof(buf); k++) {
            uint8_t fr = 0, cr = 0;
            if (atom_cmd_revision(fAtom, t[k].idx, &fr, &cr))
                n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s v%u.%u", n ? ", " : "",
                                      t[k].name, fr, cr);
        }
        setProperty("RDNA3,AtomCommandTables", buf);
    }
}

/* the display path whose DDC line feeds AUX engine 'aux' */
static const rdna3_display_path *path_for_aux(const rdna3_display_path *p, uint32_t n, uint32_t aux)
{
    for (uint32_t i = 0; i < n; i++)
        if (p[i].ddc_line >= 0 && (uint32_t)p[i].ddc_line == aux)
            return &p[i];
    return nullptr;
}

/* DP AUX engines 0-4: HPD sense, DPCD, EDID (over AUX, or the DDC for
 * HDMI / DP++ sinks). Read-only on the display
 * state (the AUX channel is a side band); what it finds goes into the
 * IORegistry as RDNA3,Connector<n>,* and to the framebuffer (displayEdid). */
void RDNA3Accel::displayProbeConnectors()
{
    fConnectors = (rdna3_connector *)IOMalloc(sizeof(rdna3_connector) * RD_AUX_ENGINES);
    if (!fConnectors)
        return;
    bzero(fConnectors, sizeof(rdna3_connector) * RD_AUX_ENGINES);
    int n = dcaux_probe(fDispDev, fConnectors, RD_AUX_ENGINES);
    fNConnectors = n > 0 ? (uint32_t)n : 0;
    setNumber("RDNA3,Connectors", fNConnectors);
    for (uint32_t i = 0; i < fNConnectors; i++) {
        const rdna3_connector *c = &fConnectors[i];
        char key[48];
        snprintf(key, sizeof(key), "RDNA3,Connector%u,AUX", i);
        setNumber(key, c->aux);
        snprintf(key, sizeof(key), "RDNA3,Connector%u,HPD", i);
        setNumber(key, (uint64_t)c->hpd);
        if (const rdna3_display_path *p = path_for_aux(fPaths, fNPaths, c->aux)) {
            snprintf(key, sizeof(key), "RDNA3,Connector%u,Type", i);
            setProperty(key, vbios_connector_name(p->connector));
            snprintf(key, sizeof(key), "RDNA3,Connector%u,Transmitter", i);
            setNumber(key, (uint64_t)(int64_t)p->transmitter);
        }
        if (c->dp) {
            snprintf(key, sizeof(key), "RDNA3,Connector%u,DPCD", i);
            setNumber(key, ((uint64_t)c->dpcd_rev << 16) | ((uint64_t)c->max_link_rate << 8) |
                               c->max_lane_count);
        }
        if (c->edid_len > 0) {
            rdna3_edid e;
            OSData *d = OSData::withBytes(c->edid, (unsigned)c->edid_len);
            snprintf(key, sizeof(key), "RDNA3,Connector%u,EDID", i);
            if (d) {
                setProperty(key, d);
                d->release();
            }
            snprintf(key, sizeof(key), "RDNA3,Connector%u,EDIDSource", i);
            setProperty(key, c->edid_ddc ? "DDC" : "AUX");
            if (edid_parse(c->edid, (uint32_t)c->edid_len, &e) == 0) {
                snprintf(key, sizeof(key), "RDNA3,Connector%u,Monitor", i);
                setProperty(key, e.name[0] ? e.name : e.manufacturer);
                snprintf(key, sizeof(key), "RDNA3,Connector%u,Modes", i);
                setNumber(key, e.nmodes);
                uint32_t usable = 0;
                for (uint32_t m = 0; m < e.nmodes; m++) {
                    /* the DP link the sink offers (DPCD max rate/lanes, downspread) */
                    int why = dcn32_mode_check(&e.mode[m], c->dp ? c->max_link_rate : 0,
                                               c->dp ? (c->max_lane_count & 0x1f) : 0, 1);
                    if (why == RD_MODE_OK)
                        usable++;
                    LOG("display: AUX%u %s %ux%u%s @ %u.%03u Hz%s%s", c->aux, e.name,
                        e.mode[m].h_active, e.mode[m].v_active, e.mode[m].interlaced ? "i" : "",
                        e.mode[m].refresh_mhz / 1000, e.mode[m].refresh_mhz % 1000,
                        e.mode[m].preferred ? " (preferred)" : "",
                        why == RD_MODE_OK ? "" : why == RD_MODE_NO_TIMING ? " [no timing]"
                                               : why == RD_MODE_LINK_BW ? " [link too slow]" : " [refused]");
                }
                snprintf(key, sizeof(key), "RDNA3,Connector%u,ModesUsable", i);
                setNumber(key, usable);
                /* the EDID mode the boot display is running, if it is this sink */
                if (fDcn && fDcn->nheads) {
                    int k = dcn32_match_head_mode(&fDcn->head[0], 0, &e);
                    if (k >= 0) {
                        char val[64];
                        snprintf(val, sizeof(val), "%ux%u@%u.%03u%s on AUX%u", e.mode[k].h_active,
                                 e.mode[k].v_active, e.mode[k].refresh_mhz / 1000,
                                 e.mode[k].refresh_mhz % 1000, e.mode[k].preferred ? " preferred" : "",
                                 c->aux);
                        setProperty("RDNA3,Head0EdidMode", val);
                    }
                }
            }
        }
    }
}

IOReturn RDNA3Accel::displayEdid(RD3KDisplayEdid *e)
{
    if (!e || e->version != RD3K_DISPLAY_ABI_VERSION)
        return kIOReturnBadArgument;
    if (!fDcn || !fDispDev)
        return kIOReturnNotReady;
    e->length = 0;
    e->connector = ~0u;
    /* head -> connector, until the OTG -> DIG -> PHY routing is read back:
     * the sink whose EDID has the mode head 0 is scanning out (its preferred
     * mode first), else the first connected sink with an EDID */
    if (e->head != 0)
        return kIOReturnNotFound;
    int best = -1, bestScore = -1;
    for (uint32_t i = 0; i < fNConnectors; i++) {
        const rdna3_connector *c = &fConnectors[i];
        rdna3_edid ed;
        int score = 0;
        if (c->edid_len <= 0 || !(c->hpd || c->dp))
            continue;
        if (fDcn->nheads && edid_parse(c->edid, (uint32_t)c->edid_len, &ed) == 0) {
            int k = dcn32_match_head_mode(&fDcn->head[0], 0, &ed);
            if (k >= 0)
                score = ed.mode[k].preferred ? 2 : 1;
        }
        if (score > bestScore) {
            best = (int)i;
            bestScore = score;
        }
    }
    if (best < 0)
        return kIOReturnNotFound;
    const rdna3_connector *c = &fConnectors[best];
    uint32_t n = (uint32_t)c->edid_len < RD3K_EDID_MAX ? (uint32_t)c->edid_len : RD3K_EDID_MAX;
    memcpy(e->data, c->edid, n);
    e->length = n;
    e->connector = c->aux;
    return kIOReturnSuccess;
}

void RDNA3Accel::displayFree()
{
    if (fDcn && fDispDev && fDispLock) {
        for (uint32_t i = 0; i < fDcn->nheads && i < 4; i++)
            if (fDcnCrcOn[i])
                dcn32_crc_enable(fDispDev, fDcn, i, 0);
    }
    if (fAtom) IOFree(fAtom, sizeof(rdna3_atom));
    if (fAtomScratch) IOFree(fAtomScratch, RD_ATOM_SCRATCH_BYTES);
    if (fPaths) IOFree(fPaths, sizeof(rdna3_display_path) * RD_VBIOS_MAX_PATHS);
    if (fDispVbios) IOFree(fDispVbios, sizeof(rdna3_vbios));
    if (fDispRom) IOFree(fDispRom, RD_VBIOS_MAX_LEN);
    fAtom = nullptr;
    fAtomScratch = nullptr;
    fPaths = nullptr;
    fDispVbios = nullptr;
    fDispRom = nullptr;
    fNPaths = 0;
    if (fConnectors) IOFree(fConnectors, sizeof(rdna3_connector) * RD_AUX_ENGINES);
    fConnectors = nullptr;
    fNConnectors = 0;
    if (fDcn) IOFree(fDcn, sizeof(rdna3_dcn));
    if (fDispDev) IOFree(fDispDev, sizeof(rdna3_dev));
    if (fDispLock) IOLockFree(fDispLock);
    fDcn = nullptr;
    fDispDev = nullptr;
    fDispLock = nullptr;
}

/* refresh = frames counted over ~250 ms of wall time (no pixel clock read
 * from DCCG needed); exact enough for IOFramebuffer's timing info */
void RDNA3Accel::displayMeasureRefresh()
{
    uint32_t f0[4], f1[4], i;
    uint64_t t0, t1, ns;
    for (i = 0; i < fDcn->nheads && i < 4; i++)
        f0[i] = dcn32_frame_count(fDispDev, fDcn, i);
    clock_get_uptime(&t0);
    IOSleep(250);
    for (i = 0; i < fDcn->nheads && i < 4; i++)
        f1[i] = dcn32_frame_count(fDispDev, fDcn, i);
    clock_get_uptime(&t1);
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    for (i = 0; i < fDcn->nheads && i < 4; i++) {
        uint64_t frames = (f1[i] - f0[i]) & 0xffffff;
        uint64_t mhz = ns ? frames * 1000000000000ull / ns : 0;
        /* round to the nearest common rate when within 0.5 % */
        static const uint32_t common[] = { 23976, 24000, 25000, 29970, 30000, 50000, 59940,
                                           60000, 72000, 75000, 90000, 100000, 119880, 120000,
                                           144000, 165000, 240000 };
        for (uint32_t k = 0; k < sizeof(common) / sizeof(common[0]); k++)
            if (mhz * 1000 > (uint64_t)common[k] * 995 && mhz * 1000 < (uint64_t)common[k] * 1005)
                mhz = common[k];
        fDispRefresh[i] = mhz > 1000 && mhz < 1000000 ? (uint32_t)mhz : 0;
    }
    fDispMeasured = true;
}

IOReturn RDNA3Accel::displayInfo(RD3KDisplayInfo *r)
{
    if (!r || r->version != RD3K_DISPLAY_ABI_VERSION)
        return kIOReturnBadArgument;
    if (!fDcn || !fDispDev)
        return kIOReturnNotReady;
    IOLockLock(fDispLock);
    if (!fDispMeasured)
        displayMeasureRefresh();
    uint64_t barPhys = fVramDev ? fVramDev->getPhysicalAddress() : 0;
    uint64_t barLen = fVramDev ? fVramDev->getLength() : 0;
    r->nheads = fDcn->nheads < RD3K_MAX_HEADS ? fDcn->nheads : RD3K_MAX_HEADS;
    for (uint32_t i = 0; i < r->nheads; i++) {
        const rdna3_dcn_head *h = &fDcn->head[i];
        RD3KDisplayHead *o = &r->head[i];
        bzero(o, sizeof(*o));
        uint64_t off = h->address - fDispDev->fb_start;
        uint64_t bytes = (uint64_t)h->pitch_px * 4 * h->height;
        bool inBar = h->address >= fDispDev->fb_start && barPhys && off + bytes <= barLen;
        o->usable = dcn32_head_usable(fDispDev, h) && inBar;
        o->width = h->width;
        o->height = h->height;
        o->pitch = h->pitch_px * 4;
        o->format = h->format == RD_DCN_FMT_ABGR8888 ? RD3K_FMT_XBGR8888 : RD3K_FMT_XRGB8888;
        o->vramOffset = off;
        o->phys = inBar ? barPhys + off : 0;
        o->hTotal = h->h_total;
        o->vTotal = h->v_total;
        o->hFront = h->h_front;
        o->hSync = h->h_sync;
        o->vFront = h->v_front;
        o->vSync = h->v_sync;
        o->hSyncNeg = h->h_sync_neg;
        o->vSyncNeg = h->v_sync_neg;
        o->refreshMilliHz = fDispRefresh[i];
    }
    IOLockUnlock(fDispLock);
    return kIOReturnSuccess;
}

IOReturn RDNA3Accel::displayFlip(RD3KDisplayFlip *f)
{
    if (!f || f->version != RD3K_DISPLAY_ABI_VERSION || (f->pitch & 3))
        return kIOReturnBadArgument;
    if (!fDcn || !fDispDev)
        return kIOReturnNotReady;
    IOLockLock(fDispLock);
    int r = RDNA3_EINVAL;
    if (f->head < fDcn->nheads && dcn32_head_usable(fDispDev, &fDcn->head[f->head]))
        r = dcn32_flip(fDispDev, fDcn, f->head, fDispDev->fb_start + f->vramOffset, f->pitch / 4,
                       (f->flags & RD3K_DISPLAY_FLIP_IMMEDIATE) != 0);
    IOLockUnlock(fDispLock);
    f->result = r;
    return r ? kIOReturnBadArgument : kIOReturnSuccess;
}

IOReturn RDNA3Accel::displayStatus(RD3KDisplayStatus *s)
{
    if (!s || s->version != RD3K_DISPLAY_ABI_VERSION)
        return kIOReturnBadArgument;
    if (!fDcn || !fDispDev || s->head >= fDcn->nheads)
        return kIOReturnNotReady;
    /* reads only: no lock needed beyond the head table, which never shrinks */
    s->frame = dcn32_frame_count(fDispDev, fDcn, s->head);
    s->scanline = dcn32_scanline(fDispDev, fDcn, s->head);
    s->inVblank = (unsigned)dcn32_in_vblank(fDispDev, fDcn, s->head);
    IOLockLock(fDispLock);
    s->flipPending = (unsigned)dcn32_flip_pending(fDispDev, fDcn, s->head);
    IOLockUnlock(fDispLock);
    return kIOReturnSuccess;
}

IOReturn RDNA3Accel::displayCrc(uint32_t head, uint32_t *crc)
{
    uint32_t r = 0, g = 0, b = 0;
    if (!crc)
        return kIOReturnBadArgument;
    if (!fDcn || !fDispDev || head >= fDcn->nheads || head >= 4)
        return kIOReturnNotReady;
    IOLockLock(fDispLock);
    if (!fDcnCrcOn[head]) {
        dcn32_crc_enable(fDispDev, fDcn, head, 1);
        fDcnCrcOn[head] = true;
    }
    int ret = dcn32_crc_read(fDispDev, fDcn, head, &r, &g, &b);
    IOLockUnlock(fDispLock);
    /* one number for the sysctl: R ^ G<<8 ^ B<<16 */
    *crc = ret ? 0 : (r ^ (g << 8) ^ (b << 16));
    return ret ? kIOReturnNotReady : kIOReturnSuccess;
}

IOReturn RDNA3Accel::displayCursor(RD3KDisplayCursor *c)
{
    if (!c || c->version != RD3K_DISPLAY_ABI_VERSION)
        return kIOReturnBadArgument;
    if (!fDcn || !fDispDev || c->head >= fDcn->nheads)
        return kIOReturnNotReady;
    int r = RDNA3_EINVAL;
    IOLockLock(fDispLock);
    if (c->op == RD3K_CURSOR_OP_IMAGE)
        r = dcn32_cursor_set_image(fDispDev, fDcn, c->head, fDispDev->fb_start + c->vramOffset, c->size);
    else if (c->op == RD3K_CURSOR_OP_MOVE)
        r = dcn32_cursor_set_position(fDispDev, fDcn, c->head, c->x, c->y, c->visible != 0);
    IOLockUnlock(fDispLock);
    c->result = r;
    return r ? kIOReturnBadArgument : kIOReturnSuccess;
}
