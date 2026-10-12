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
 *   rdna3DisplayGamma    output gamma table (MPCC OGAM, rdna3gamma=1)
 *
 * Nothing here writes a register until the framebuffer asks for a flip or
 * a CRC, and the framebuffer only starts with the boot-arg rdna3fb=1.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/OSAtomic.h>
#include <sys/kauth.h>

extern "C" {
#include "rdna3_hw.h"
#include "rdna3_bootargs.h"
#include "dcn32.h"
#include "dcn32_clk.h"
#include "dcaux.h"
#include "edid.h"
#include "vbios.h"
#include "atom.h"
#include "dcn32_mode.h"
#include "dcn32_clone.h"
#include "dcn32_modeset.h"
#include "dcn32_dp.h"
#include "dcn32_audio.h"
#include "dcn32_hpo.h"
#include "dp_mst.h"
#include "dcn32_gamma.h"
#include "dcn32_otg.h"
#include "dcn32_hwseq.h"
}
#include "rdna3k_abi.h"

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

static void displayOtgCheck(IOService *svc, rdna3_dev *d, const rdna3_dcn *dc);

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
    displayPublishRoutes();
    displayStateSaveAll();   /* D4: what a reset or a sleep must bring back */
    displayOtgCheck(this, fDispDev, fDcn);   /* rdna3otg=1: step otg */

    fHotplugCall = thread_call_allocate((thread_call_func_t)displayHotplugWork, this);
    hotplugInit();   /* rdna3hotplug=1: RDNA3Hotplug.cpp polls and handles */
    return true;
}

/* rdna3otg=1 (step otg): the timing generator functions of hw/dcn32_otg.c
 * on every lit head - position, active size, vblank / active states, the
 * vertical interrupt 0 latched mid-frame (its registers put back),
 * underflow, double buffering, ODM segments. Reads only, apart from the
 * vertical interrupt position, which nothing else uses. */
static void displayOtgCheck(IOService *svc, rdna3_dev *d, const rdna3_dcn *dc)
{
    if (!rd3_arg("rdna3otg", 0) || !d || !dc)
        return;
    for (uint32_t i = 0; i < dc->nheads && i < RD_DCN_MAX_PIPES; i++) {
        rdna3_otg_check c;
        char key[40], val[200];
        int r = dcn32_otg_check(d, dc->head[i].otg, &c);
        snprintf(key, sizeof(key), "RDNA3,Head%u,OTGCheck", i);
        snprintf(val, sizeof(val), "%d: %ux%u (head %ux%u), counter %s, vblank %d vactive %d, vline0 line %u %s "
                 "(read %u), underflow %d, db pending %d, ODM %d", r, c.width, c.height, dc->head[i].h_active,
                 dc->head[i].v_active, c.moving ? "moving" : "STOPPED", c.vblank_seen, c.vactive_seen, c.vline0_line,
                 c.vline0_seen ? "latched" : "NOT latched", c.vline0_at, c.underflow, c.db_pending, c.odm_segments);
        svc->setProperty(key, val);
        LOG("display: head %u OTG %u check %s", i, dc->head[i].otg, val);
    }
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

/* the AUX engine / DDC line of the connector a head drives: the head's DIG
 * back end is the transmitter (dcn32_otg_dig), the VBIOS path with that
 * transmitter names the DDC line; -1 when the route is unknown */
static int head_aux(const rdna3_dcn_head *h, const rdna3_display_path *p, uint32_t n)
{
    if (!h || h->dig_be < 0 || !p)
        return -1;
    for (uint32_t i = 0; i < n; i++)
        if (p[i].transmitter == h->dig_be && p[i].ddc_line >= 0)
            return p[i].ddc_line;
    return -1;
}

/* DP AUX engines 0-4: HPD sense, DPCD, EDID (over AUX, or the DDC for
 * HDMI / DP++ sinks). Read-only on the display
 * state (the AUX channel is a side band); what it finds goes into the
 * IORegistry as RDNA3,Connector<n>,* and to the framebuffer (displayEdid). */
void RDNA3Accel::displayProbeConnectors()
{
    if (!fConnectors)   /* kept across hotplug re-probes */
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
                    if (k >= 0 && !getProperty("RDNA3,Head0EdidMode")) {   /* the first match */
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

/* RD_NR_XNU_SCANOUT: what pipe s->pipe scans out, and a scaled slice of
 * it, for the report (rdna3_drminfo --scanout): proof of what reaches each
 * panel without anyone at the screen. Screen contents: root only. */
int RDNA3Accel::drmScanout(void *ctx, struct rd_xnu_scanout *s)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    rdna3_scanout_info si;
    uint32_t pipe = s->pipe, row0 = s->row0, nrows = 0, out_h = 0;
    int r;
    if (!kauth_cred_issuser(kauth_cred_get()))
        return -RD_EPERM;
    if (!a->fDispDev || !a->fDispLock)
        return -RD_ENODEV;
    /* the OTG CRC of what leaves the pipe (after the scaler and the blank),
     * on the first call of a picture: windows over the active area, two
     * frames to settle (nothing borrowed while the lock is dropped) */
    if (row0 == 0) {
        IOLockLock(a->fDispLock);
        r = dcn32_scanout_info(a->fDispDev, pipe, &si);
        IOLockUnlock(a->fDispLock);
        if (r == RDNA3_OK && (si.lit || si.blanked)) {
            uint32_t rr = 0, gg = 0, bb = 0;
            rdna3_dcn one;
            bzero(&one, sizeof(one));
            one.nheads = 1;
            one.head[0].otg = si.otg;
            IOLockLock(a->fDispLock);
            dcn32_crc_enable(a->fDispDev, &one, 0, 1);
            IOLockUnlock(a->fDispLock);
            IOSleep(40);
            IOLockLock(a->fDispLock);
            if (dcn32_crc_read(a->fDispDev, &one, 0, &rr, &gg, &bb) == RDNA3_OK) {
                s->crc_valid = 1;
                s->crc_rg = rr | gg << 16;
                s->crc_b = bb;
            }
            IOLockUnlock(a->fDispLock);
        }
    }
    IOLockLock(a->fDispLock);
    /* the display's register device has no BAR0 view of its own: lend it
     * the DRM's for this read only */
    volatile uint8_t *oldCpu = a->fDispDev->vram_cpu;
    uint64_t oldLen = a->fDispDev->vram_cpu_len;
    if (!oldCpu && a->fVramMap) {
        a->fDispDev->vram_cpu = (volatile uint8_t *)a->fVramMap->getVirtualAddress();
        a->fDispDev->vram_cpu_len = a->fVramMap->getLength();
    }
    r = dcn32_scanout_info(a->fDispDev, pipe, &si);
    /* hubp2_clear_underflow once read: the next look shows if it comes back */
    if (r == RDNA3_OK && row0 == 0 && (s->flags & RD_XNU_SCANOUT_CLEAR_UNDERFLOW) && si.underflow)
        dcn32_clear_underflow(a->fDispDev, pipe);
    if (r == RDNA3_OK && si.lit && si.width && si.height) {
        out_h = (uint32_t)((uint64_t)si.height * RD_XNU_SCANOUT_W / si.width);
        if (out_h > 4 * RD_XNU_SCANOUT_W)
            out_h = 4 * RD_XNU_SCANOUT_W;
        if (row0 < out_h) {
            nrows = out_h - row0 < RD_XNU_SCANOUT_ROWS ? out_h - row0 : RD_XNU_SCANOUT_ROWS;
            if (dcn32_scanout_sample(a->fDispDev, &si, RD_XNU_SCANOUT_W, out_h, row0, nrows,
                                     s->rgb) != RDNA3_OK)
                nrows = 0;
        }
    }
    a->fDispDev->vram_cpu = oldCpu;
    a->fDispDev->vram_cpu_len = oldLen;
    IOLockUnlock(a->fDispLock);
    if (r != RDNA3_OK)
        return -RD_EINVAL;
    s->powered = si.powered;
    s->lit = si.lit;
    s->blanked = si.blanked;
    s->underflow = si.underflow;
    s->otg = si.otg;
    s->format = si.format;
    s->sw_mode = si.sw_mode;
    s->dcc = si.dcc;
    s->width = si.width;
    s->height = si.height;
    s->pitch_px = si.pitch_px;
    s->flip_pending = si.flip_pending;
    s->out_w = RD_XNU_SCANOUT_W;
    s->out_h = out_h;
    s->nrows = nrows;
    s->address = si.address;
    s->inuse = si.inuse;
    s->det_segs = si.det_segs;
    s->compbuf_segs = si.compbuf_segs;
    s->dispclk_khz = si.dispclk_khz;
    s->dppclk_khz = si.dppclk_khz;
    s->dscl_mode = si.dscl_mode;
    s->lb_memory_ctrl = si.lb_memory_ctrl;
    s->dscl_mem_pwr = si.dscl_mem_pwr;
    s->dlg_after_scaler = si.dlg_after_scaler;
    s->dlg_req_delivery = si.dlg_req_delivery;
    s->dlg_line_delivery = si.dlg_line_delivery;
    return 0;
}

/* RDNA3,Head<n>,Route: what the head drives, from the DIG routing */
void RDNA3Accel::displayPublishRoutes()
{
    if (!fDcn)
        return;
    for (uint32_t i = 0; i < fDcn->nheads && i < RD_DCN_MAX_PIPES; i++) {
        const rdna3_dcn_head *h = &fDcn->head[i];
        char key[48], val[96];
        int aux = head_aux(h, fPaths, fNPaths);
        const rdna3_display_path *p = aux >= 0 ? path_for_aux(fPaths, fNPaths, (uint32_t)aux) : nullptr;
        snprintf(key, sizeof(key), "RDNA3,Head%u,Route", i);
        if (h->dig_be < 0)
            snprintf(val, sizeof(val), "OTG%u -> no enabled DIG", h->otg);
        else if (p)
            snprintf(val, sizeof(val), "OTG%u -> DIG%c -> UNIPHY%c (mode %u) -> %s-%u DDC%d",
                     h->otg, 'A' + h->dig_fe, 'A' + h->dig_be, h->dig_mode,
                     vbios_connector_name(p->connector), p->connector_enum, aux + 1);
        else
            snprintf(val, sizeof(val), "OTG%u -> DIG%c -> UNIPHY%c (mode %u) -> no VBIOS path",
                     h->otg, 'A' + h->dig_fe, 'A' + h->dig_be, h->dig_mode);
        setProperty(key, val);
        if (aux >= 0) {
            snprintf(key, sizeof(key), "RDNA3,Head%u,AUX", i);
            setNumber(key, (uint64_t)aux);
        }
        LOG("display: head %u route %s, AUX/DDC %d", i, val, aux);
    }
    /* the display clocks the GOP left (dcn32_dump_clk_registers): what a
     * second head has to fit into */
    {
        rdna3_dcn_clocks ck;
        char val[128];
        dcn32_clk_read(fDispDev, &ck);
        snprintf(val, sizeof(val), "VCO %u DISPCLK %u DPPCLK %u DPREFCLK %u DCFCLK %u DTBCLK %u kHz",
                 ck.vco_khz, ck.dispclk_khz, ck.dppclk_khz, ck.dprefclk_khz, ck.dcfclk_khz, ck.dtbclk_khz);
        setProperty("RDNA3,DCNClocks", val);
        LOG("display: clocks %s", val);
    }
}

/* rdna3mirror=1 with rdna3dmub=1: the boot head's picture on the other
 * TMDS monitor (hw/dcn32_clone.c). The connector must have a sink (HPD),
 * a DDC EDID (TMDS: HDMI or a DP++ adaptor) and the boot mode in it. fDcn
 * keeps describing the boot heads only: the framebuffer's head numbers do
 * not move. */
void RDNA3Accel::displayMirror(rdna3_dmub *dm, bool probe)
{
    uint32_t want = rd3_arg("rdna3mirror", 0);
    fDisplayDm = dm;   /* mode sets drive the PHY through it too */
    if (!want)
        return;
    fMirrorDm = dm;   /* hotplug lights it later, or switches it off */
    if (fMirrorOn)
        return;   /* a GPU reset / resume runs hwInit again: the clone is the restore's */
    if (!fDcn || !fDispDev || !fDispLock || !fPaths || !fConnectors || !fDcn->nheads) {
        setProperty("RDNA3,Mirror", "failed: no boot head / connectors");
        return;
    }
    if (!fMirror)
        fMirror = (rdna3_dcn_clone *)IOMalloc(sizeof(rdna3_dcn_clone));
    if (!fMirror)
        return;
    IOLockLock(fDispLock);
    const rdna3_dcn_head *h = &fDcn->head[0];
    int boot_aux = head_aux(h, fPaths, fNPaths);
    int ret = RDNA3_ENODEV;
    char val[128];
    snprintf(val, sizeof(val), "failed: no other TMDS monitor with the boot mode");
    for (uint32_t i = 0; i < fNConnectors && !fMirrorOn; i++) {
        const rdna3_connector *c = &fConnectors[i];
        const rdna3_display_path *p = path_for_aux(fPaths, fNPaths, c->aux);
        rdna3_edid e;
        int k;
        if ((int)c->aux == boot_aux || !c->hpd || c->dp || c->edid_len <= 0 || !p)
            continue;
        if (edid_parse(c->edid, (uint32_t)c->edid_len, &e) != 0 ||
            (k = dcn32_match_head_mode(h, 0, &e)) < 0) {
            LOG("display: mirror: the monitor on AUX%u has no %ux%u mode like the boot head", c->aux,
                h->h_active, h->v_active);
            continue;
        }
        ret = dcn32_clone_plan(fDispDev, fDcn, 0, p, e.mode[k].pixel_khz, fMirror);
        fMirror->smu_probe = probe;
        /* rdna3pstate: force the DCHUBBUB arbiter to allow p-state changes
         * (1, the default: without it the PMFW hung at its next table
         * transfer, card runs 1-6), USR retraining (2) or both (3); 0 = off */
        {
            uint32_t pf = 1;
            PE_parse_boot_argn("rdna3pstate", &pf, sizeof(pf));
            fMirror->pstate_force = pf & 3;
        }
        if (ret == RDNA3_OK)
            ret = dcn32_clone_enable(fDispDev, dm, fMirror);
        if (ret == RDNA3_OK) {
            /* does the new pipe make the boot head's picture? (frame
             * counter + OTG CRC); a pass with a black monitor = PHY side */
            rdna3_clone_check ck;
            int vr = dcn32_clone_verify(fDispDev, fMirror, 100, &ck);
            char chk[96];
            snprintf(chk, sizeof(chk), "%s, %u frames in 100 ms, CRC %s",
                     ck.running ? "running" : "NOT running", ck.frames_dst,
                     !ck.crc_valid ? "not compared (CRC reads 0)" :
                     ck.crc_match ? "same as the boot head" : "DIFFERENT");
            setProperty("RDNA3,MirrorCheck", chk);
            setNumber("RDNA3,MirrorCheckResult", (uint64_t)(int64_t)vr, 64);
            fMirrorOn = true;
            if (want == 2)
                displayAddCloneHead();
            displayPowerUpdate("mirror on");   /* item O (rdna3mall) */
            snprintf(val, sizeof(val), "%s-%u UNIPHY%c, pipe %u OTG %u DIG%c, %u kHz",
                     vbios_connector_name(p->connector), p->connector_enum, 'A' + fMirror->dst_be,
                     fMirror->dst_pipe, fMirror->dst_otg, 'A' + fMirror->dst_fe, fMirror->pclk_khz);
        } else {
            snprintf(val, sizeof(val), "failed (%d) on %s-%u", ret, vbios_connector_name(p->connector),
                     p->connector_enum);
        }
        break;
    }
    IOLockUnlock(fDispLock);
    setProperty("RDNA3,Mirror", val);
    setNumber("RDNA3,MirrorResult", (uint64_t)(int64_t)ret, 64);
    LOG("display: mirror: %s", val);
}

void RDNA3Accel::displayHotplug(uint32_t pin, int connected)
{
    OSBitOrAtomic(1u << pin, &fHotplugPending);
    fHotplugEvents++;
    LOG("display: HPD%u %s", pin + 1, connected ? "connected" : "disconnected");
    if (fHotplugCall)
        thread_call_enter(fHotplugCall);
}

/* handle_hpd_irq -> dc_link_detect: read the sinks again (DPCD, EDID over
 * AUX or the DDC) and publish what changed */
void RDNA3Accel::displayHotplugWork(void *owner, void *)
{
    RDNA3Accel *a = (RDNA3Accel *)owner;
    UInt32 pins = OSBitAndAtomic(0, &a->fHotplugPending);
    if (!a->fDispDev || !a->fDispLock)
        return;
    if (a->hotplugKick())   /* rdna3hotplug=1: the poll decides (RDNA3Hotplug.cpp) */
        return;
    IOLockLock(a->fDispLock);
    a->displayProbeConnectors();
    IOLockUnlock(a->fDispLock);
    a->setNumber("RDNA3,HotplugEvents", a->fHotplugEvents);
    a->setNumber("RDNA3,HotplugPins", pins);
    LOG("display: hotplug on pins 0x%x: %u connectors with a sink or DPCD", (unsigned)pins, a->fNConnectors);
    /* rdna3mirror: the second monitor follows its cable */
    if (rdna3_dmub *dm = a->fMirrorDm) {
        if (a->fMirrorOn && !a->displayCloneSinkPresent()) {
            LOG("display: the cloned monitor is gone: switching its pipe off");
            a->displayMirrorDisable(dm);
        } else if (!a->fMirrorOn) {
            a->displayMirror(dm);
        }
    }
}

/* the connector the clone drives still has a sink (HPD) */
bool RDNA3Accel::displayCloneSinkPresent()
{
    if (!fMirror || !fPaths || !fConnectors)
        return false;
    for (uint32_t i = 0; i < fNPaths; i++) {
        if (fPaths[i].transmitter != (int8_t)fMirror->dst_be || fPaths[i].ddc_line < 0)
            continue;
        for (uint32_t c = 0; c < fNConnectors; c++)
            if (fConnectors[c].aux == (uint32_t)fPaths[i].ddc_line)
                return fConnectors[c].hpd != 0;
    }
    return false;
}

/* rdna3mirror=2: the clone becomes head <n> of fDcn (the boot heads keep
 * their numbers) and gets a nub, RDNA3DisplayHead, on which a second
 * RDNA3Framebuffer starts with its own surface: an extended desktop.
 * Called with fDispLock held. */
void RDNA3Accel::displayAddCloneHead()
{
    rdna3_dcn *all = (rdna3_dcn *)IOMalloc(sizeof(rdna3_dcn));
    if (!all)
        return;
    bzero(all, sizeof(*all));
    dcn32_read_state(fDispDev, all);
    for (uint32_t i = 0; i < all->nheads; i++) {
        if (all->head[i].pipe != fMirror->dst_pipe || fDcn->nheads >= RD_DCN_MAX_PIPES)
            continue;
        fCloneHead = fDcn->nheads;
        fDcn->head[fCloneHead] = all->head[i];
        fDcn->requested[all->head[i].pipe] = all->head[i].address;
        fDcn->nheads++;
        displayStateSave((uint32_t)fCloneHead);   /* D4 */
        break;
    }
    IOFree(all, sizeof(*all));
    if (fCloneHead < 0) {
        LOG("display: extend: the clone pipe %u does not read back as lit", fMirror->dst_pipe);
        return;
    }
    fHeadNub = new IOService;
    if (fHeadNub && fHeadNub->init()) {
        fHeadNub->setName("RDNA3DisplayHead");
        fHeadNub->setProperty("RDNA3,Head", (unsigned long long)fCloneHead, 32);
        if (fHeadNub->attach(this)) {
            fHeadNub->registerService();
            setNumber("RDNA3,ExtendHead", (uint64_t)fCloneHead);
            LOG("display: extend: head %d (pipe %u OTG %u) published for a second framebuffer",
                fCloneHead, fMirror->dst_pipe, fMirror->dst_otg);
            return;
        }
    }
    OSSafeReleaseNULL(fHeadNub);
    fDcn->nheads--;
    fCloneHead = -1;
}

/* amdgpu_pci_shutdown: no clone left lit for whoever drives the card next
 * (the host's amdgpu after the VM, the GOP after a restart). The second
 * framebuffer's head goes away with it; its nub stays until stop(). */
void RDNA3Accel::displayShutdown()
{
    rdna3_dmub *dm = fMirrorDm;
    fMirrorDm = nullptr;
    if (fHotplugCall)
        thread_call_cancel_wait(fHotplugCall);
    hotplugStop();
    /* item O: no phantom pipe or CAB left for the next driver, UCLK pinned */
    fPowerStop = true;
    if (fPowerOn && fDispLock) {
        IOLockLock(fDispLock);
        displayPowerQuiesce(true);
        IOLockUnlock(fDispLock);
    }
    if (!dm || !fMirrorOn || !fMirror || !fDispLock)
        return;
    IOLockLock(fDispLock);
    if (fCloneHead >= 0 && (uint32_t)fCloneHead == fDcn->nheads - 1)
        fDcn->nheads--;
    fCloneHead = -1;
    dcn32_clone_disable(fDispDev, dm, fMirror);
    fMirrorOn = false;
    IOLockUnlock(fDispLock);
    LOG("display: shutdown: the second monitor is off");
}

/* the bring-up is going (its DMCUB with it): no hotplug may use it */
void RDNA3Accel::displayMirrorOff(rdna3_dmub *dm)
{
    displayPowerFree();   /* item O: SubVP / CAB off while the DMCUB runs */
    fMirrorDm = nullptr;
    fDisplayDm = nullptr;
    if (fHotplugCall)
        thread_call_cancel_wait(fHotplugCall);
    hotplugStop();
    displayMirrorDisable(dm);
}

void RDNA3Accel::displayMirrorDisable(rdna3_dmub *dm)
{
    if (!fMirrorOn || !fMirror || !fDispLock)
        return;
    /* the second framebuffer goes first (its stop() calls back into the
     * display functions, which take fDispLock) */
    if (fHeadNub) {
        fHeadNub->terminate(kIOServiceSynchronous);
        OSSafeReleaseNULL(fHeadNub);
    }
    IOLockLock(fDispLock);
    if (fCloneHead >= 0 && (uint32_t)fCloneHead == fDcn->nheads - 1)
        fDcn->nheads--;
    fCloneHead = -1;
    displayPowerQuiesce(false);   /* item O: no phantom across the clone's teardown */
    dcn32_clone_disable(fDispDev, dm, fMirror);
    fMirrorOn = false;
    displayPowerUpdate("mirror off");
    IOLockUnlock(fDispLock);
}

IOReturn RDNA3Accel::displayEdid(RD3KDisplayEdid *e)
{
    if (!e || e->version != RD3K_DISPLAY_ABI_VERSION) {
        LOG("display: EDID request with ABI %u (want %u)", e ? e->version : 0, RD3K_DISPLAY_ABI_VERSION);
        return kIOReturnBadArgument;
    }
    if (!fDcn || !fDispDev || !fDispLock) {
        LOG("display: EDID request before the display core is up");
        return kIOReturnNotReady;
    }
    /* fConnectors is rebuilt from scratch by a hotplug re-probe */
    IOLockLock(fDispLock);
    IOReturn ret = displayEdidLocked(e);
    IOLockUnlock(fDispLock);
    return ret;
}

/* displayEdid with fDispLock held */
IOReturn RDNA3Accel::displayEdidLocked(RD3KDisplayEdid *e)
{
    e->length = 0;
    e->connector = ~0u;
    /* head -> connector, until the OTG -> DIG -> PHY routing is read back:
     * the sink whose EDID has the mode head 0 is scanning out (its preferred
     * mode first), else the first connected sink with an EDID */
    if (e->head >= fDcn->nheads)
        return kIOReturnNotFound;
    int best = -1, bestScore = -1;
    /* the connector the head's DIG route leads to wins outright */
    int routed = head_aux(&fDcn->head[e->head], fPaths, fNPaths);
    for (uint32_t i = 0; routed >= 0 && i < fNConnectors; i++)
        if (fConnectors[i].aux == (uint32_t)routed && fConnectors[i].edid_len > 0) {
            best = (int)i;
            bestScore = 3;
        }
    for (uint32_t i = 0; i < fNConnectors; i++) {
        const rdna3_connector *c = &fConnectors[i];
        rdna3_edid ed;
        int score = 0;
        if (bestScore == 3 || e->head != 0)
            break;   /* heads we lit ourselves have a known route only */
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
    if (best < 0) {
        LOG("display: EDID request for head %u: none of %u connectors has one", e->head, fNConnectors);
        return kIOReturnNotFound;
    }
    const rdna3_connector *c = &fConnectors[best];
    uint32_t n = (uint32_t)c->edid_len < RD3K_EDID_MAX ? (uint32_t)c->edid_len : RD3K_EDID_MAX;
    memcpy(e->data, c->edid, n);
    e->length = n;
    e->connector = c->aux;
    return kIOReturnSuccess;
}

void RDNA3Accel::displayFree()
{
    displayPowerFree();
    /* the HPD work first: a displayHotplugWork already queued reaches
     * fHp through hotplugKick (PR #2 review) */
    if (fHotplugCall) {
        rd3_thread_call_drain_free(fHotplugCall);
        fHotplugCall = nullptr;
    }
    hotplugFree();
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
    if (fMirror) IOFree(fMirror, sizeof(rdna3_dcn_clone));
    fMirror = nullptr;
    if (fScale) IOFree(fScale, sizeof(rdna3_dcn_scale));
    fScale = nullptr;
    if (fClkLim) IOFree(fClkLim, sizeof(rdna3_dcn_clk_limits));
    fClkLim = nullptr;
    for (uint32_t i = 0; i < 4; i++) {
        if (fGammaTab[i]) IOFree(fGammaTab[i], sizeof(rdna3_gamma_pwl));
        fGammaTab[i] = nullptr;
        if (fModeTab[i]) IOFree(fModeTab[i], sizeof(rdna3_mode) * RD3K_MAX_MODES);
        fModeTab[i] = nullptr;
        fModeCount[i] = 0;
        if (fDpLink[i]) IOFree(fDpLink[i], sizeof(rdna3_dp_link));
        fDpLink[i] = nullptr;
        if (fHdmiCfg[i]) IOFree(fHdmiCfg[i], sizeof(rdna3_hdmi_cfg));
        fHdmiCfg[i] = nullptr;
        if (fAudio[i]) IOFree(fAudio[i], sizeof(rdna3_audio_stream));
        fAudio[i] = nullptr;
        if (fMstMgr[i]) IOFree(fMstMgr[i], sizeof(rdna3_mst_mgr));
        fMstMgr[i] = nullptr;
    }
    if (fDispDev) IOFree(fDispDev, sizeof(rdna3_dev));
    if (fDispLock) IOLockFree(fDispLock);
    fDcn = nullptr;
    fDispDev = nullptr;
    fDispLock = nullptr;
}

/* refresh = whole frames between two frame-counter edges over ~400 ms of
 * wall time (no pixel clock read from DCCG needed). Counting frames in a
 * fixed sleep instead quantises to 1/window (16 frames in a 251 ms VM sleep
 * read as 63.7 Hz on the card); edge to edge the error is the polling step. */
static bool waitFrameEdge(rdna3_dev *d, rdna3_dcn *dc, uint32_t i, uint32_t *frame, uint64_t *t)
{
    uint32_t f0 = dcn32_frame_count(d, dc, i), f;
    for (uint32_t k = 0; k < 2500; k++) {          /* 50 ms: three frames at 60 Hz */
        f = dcn32_frame_count(d, dc, i);
        if (f != f0) {
            clock_get_uptime(t);
            *frame = f;
            return true;
        }
        IODelay(20);
    }
    return false;
}

void RDNA3Accel::displayMeasureRefresh()
{
    for (uint32_t i = 0; i < fDcn->nheads && i < 4; i++) {
        uint32_t f0, f1;
        uint64_t t0, t1, ns, mhz = 0;
        fDispRefresh[i] = 0;
        if (!waitFrameEdge(fDispDev, fDcn, i, &f0, &t0))
            continue;
        IOSleep(400);
        if (!waitFrameEdge(fDispDev, fDcn, i, &f1, &t1))
            continue;
        absolutetime_to_nanoseconds(t1 - t0, &ns);
        uint64_t frames = (f1 - f0) & 0xffffff;
        mhz = ns ? frames * 1000000000000ull / ns : 0;
        /* round to the nearest common rate when within 0.5 % */
        static const uint32_t common[] = { 23976, 24000, 25000, 29970, 30000, 50000, 59940,
                                           60000, 72000, 75000, 90000, 100000, 119880, 120000,
                                           144000, 165000, 240000 };
        for (uint32_t k = 0; k < sizeof(common) / sizeof(common[0]); k++)
            if (mhz * 1000 > (uint64_t)common[k] * 995 && mhz * 1000 < (uint64_t)common[k] * 1005)
                mhz = common[k];
        fDispRefresh[i] = mhz > 1000 && mhz < 1000000 ? (uint32_t)mhz : 0;
        LOG("display: head %u refresh %llu.%03llu Hz (%llu frames in %llu us)", i, mhz / 1000,
            mhz % 1000, frames, ns / 1000);
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
    if (f->head < fDcn->nheads && dcn32_head_usable(fDispDev, &fDcn->head[f->head])) {
        bool imm = (f->flags & RD3K_DISPLAY_FLIP_IMMEDIATE) != 0;
        displayPowerFlip(f->head, fDispDev->fb_start + f->vramOffset, imm, 0);   /* item O */
        r = dcn32_flip(fDispDev, fDcn, f->head, fDispDev->fb_start + f->vramOffset, f->pitch / 4, imm);
        displayPowerFlip(f->head, fDispDev->fb_start + f->vramOffset, imm, 1);
    }
    IOLockUnlock(fDispLock);
    f->result = r;
    return r ? kIOReturnBadArgument : kIOReturnSuccess;
}

/* IOFramebuffer power off (display sleep) -> dcn32_set_blank; opt-in until
 * a card run shows the panels come back */
IOReturn RDNA3Accel::displayBlank(uint32_t head, bool blank)
{
    static int wanted = -1;
    if (wanted < 0) {
        uint32_t v = rd3_arg("rdna3dpms", 0);
        wanted = (int)(v > 2 ? 2 : v);
    }
    if (!wanted)
        return kIOReturnUnsupported;
    if (!fDcn || !fDispDev || head >= fDcn->nheads)
        return kIOReturnNotReady;
    IOLockLock(fDispLock);
    int r;
    displayNoteBlank(head, blank);   /* D4: what a restore brings back */
    /* rdna3dpms=2: the head we lit ourselves also takes its link down, so
     * the monitor goes to standby (dcn32_clone_link) */
    if (fDispAsleep)
        r = RDNA3_OK;   /* D4: the wake work applies it */
    else if (wanted == 2 && fMirrorOn && fMirror && fMirrorDm && fCloneHead >= 0 && (uint32_t)fCloneHead == head) {
        /* card run 29: the clone's standby did not mark the link down, so
         * the snapshot before a system sleep caught it dark and the wake
         * restored it dark (no signal). Snapshot first, then mark it. */
        if (blank && !fHeadLinkDown[head])
            displayStateSave(head);
        r = dcn32_clone_link(fDispDev, fMirrorDm, fMirror, blank ? 0 : 1);
        if (r == RDNA3_OK && head < 4)
            fHeadLinkDown[head] = blank;
    }
    /* D4: the other heads too (dcn32_head_link: TMDS, DP through its link); the rest keeps the blank */
    else if (wanted != 2 || (r = displayHeadDpms(head, blank)) == RDNA3_EINVAL)
        r = dcn32_set_blank(fDispDev, fDcn, head, blank ? 1 : 0);
    if (r == RDNA3_OK && !fDispAsleep)
        displayPowerUpdate(blank ? "display sleep" : "display wake");   /* item O: the CAB without requests */
    IOLockUnlock(fDispLock);
    LOG("display: head %u %s (%d)", head, blank ? "blanked (display sleep)" : "unblanked", r);
    return r ? kIOReturnError : kIOReturnSuccess;
}

IOReturn RDNA3Accel::displaySetSource(RD3KDisplaySource *s)
{
    if (!s || s->version != RD3K_DISPLAY_ABI_VERSION || (s->pitch & 255))
        return kIOReturnBadArgument;
    if (!fDcn || !fDispDev || s->head >= fDcn->nheads)
        return kIOReturnNotReady;
    IOLockLock(fDispLock);
    if (!fScale) {
        fScale = (rdna3_dcn_scale *)IOMalloc(sizeof(rdna3_dcn_scale));
        if (fScale) {
            bzero(fScale, sizeof(*fScale));
            /* line buffer + DLG/TTU for scaled sources (dcn32_set_source) */
            fScale->dlg = rd3_arg("rdna3scaledlg", 0);
        }
    }
    int r = fScale ? dcn32_set_source(fDispDev, fDcn, fScale, s->head, fDispDev->fb_start + s->vramOffset,
                                      s->width, s->height, s->pitch / 4)
                   : RDNA3_ENOMEM;
    if (r == RDNA3_OK)
        displayStateSave(s->head);   /* D4 */
    IOLockUnlock(fDispLock);
    s->result = r;
    LOG("display: head %u source %ux%u pitch %u -> %d", s->head, s->width, s->height, s->pitch, r);
    return r ? kIOReturnBadArgument : kIOReturnSuccess;
}

/* the pixel clock head 'i' runs: its EDID mode, else the measured refresh */
static uint32_t head_pclk_khz(const rdna3_dcn_head *h, const rdna3_edid *ed, uint32_t refresh_mhz)
{
    int k = ed ? dcn32_match_head_mode(h, 0, ed) : -1;
    if (k >= 0)
        return ed->mode[k].pixel_khz;
    return (uint32_t)((uint64_t)h->h_total * h->v_total * refresh_mhz / 1000000ull);
}

/* rdna3dispclk=1: what the PMFW lets DISPCLK / DPPCLK reach (dcn32_init_clocks
 * over the DAL mailbox), read once; nullptr (or no limits) = the clocks the
 * GOP left are the limit, as before. Called with fDispLock held. */
const rdna3_dcn_clk_limits *RDNA3Accel::displayClkLimits()
{
    if (!rd3_arg("rdna3dispclk", 0) || !fDispDev)
        return nullptr;
    if (!fClkLim) {
        fClkLim = (rdna3_dcn_clk_limits *)IOMalloc(sizeof(rdna3_dcn_clk_limits));
        if (!fClkLim)
            return nullptr;
        bzero(fClkLim, sizeof(*fClkLim));
        int r = dcn32_clk_limits_read(fDispDev, fClkLim);
        LOG("display: rdna3dispclk: PMFW 0x%x, DISPCLK up to %u kHz (%u levels), DPPCLK up to %u kHz (%u levels) (%d)",
            fClkLim->pmfw, fClkLim->dispclk_max_khz, fClkLim->dispclk_levels, fClkLim->dppclk_max_khz,
            fClkLim->dppclk_levels, r);
        if (r)
            fClkLim->dispclk_max_khz = fClkLim->dppclk_max_khz = 0;
    }
    return fClkLim;
}

/* rdna3dplink=1: the link of a DP head (hw/dcn32_dp.c). The sink's
 * capabilities are read again on every call (AUX side band, the link is
 * not touched); the link cap a training verified (dp_verify_link_cap on the
 * first mode set) is kept until the sink reports other capabilities.
 * rdna3dsc=1 turns DC's FEC and DSC policies on for the link. rdna3uhbr=1
 * lets the link take DP 2.1 UHBR rates (128b/132b through the HPO
 * encoders; one stream: the HPO stream / link encoder 0, so one head at a
 * time holds the policy, the first that asks). rdna3odm=1 turns the ODM
 * combine policy on (2:1 for the 8K modes and pixel rates one pipe does not
 * reach). A DIG_MODE 5 head is one this driver took over as MST (rdna3mst=1). nullptr without
 * the boot-arg, for a head that is not DP, or when the sink does not
 * answer: such a head stays refused (RD_MODESET_NOT_TMDS).
 * Called with fDispLock held. */
rdna3_dp_link *RDNA3Accel::displayDpLink(uint32_t head, const rdna3_display_path *p)
{
    if (!rd3_arg("rdna3dplink", 0) || !fDcn || !fDispDev || head >= 4 || head >= fDcn->nheads || !p ||
        p->ddc_line < 0)
        return nullptr;
    const rdna3_dcn_head *h = &fDcn->head[head];
    rdna3_dp_link *l = fDpLink[head];
    if (h->dig_fe < 0 || h->dig_be < 0 || (h->dig_mode != 0 && !(h->dig_mode == 5 && l && l->mst)))
        return nullptr;
    if (!l) {
        l = (rdna3_dp_link *)IOMalloc(sizeof(rdna3_dp_link));
        if (!l)
            return nullptr;
        bzero(l, sizeof(*l));
        l->mst_port = -1;
        fDpLink[head] = l;
    }
    if (!l->d || l->aux != (uint32_t)p->ddc_line || l->be != (uint32_t)h->dig_be || l->fe != (uint32_t)h->dig_fe ||
        l->otg != h->otg) {
        dcn32_dp_link_init(l, fDispDev, fDisplayDm, (uint32_t)p->ddc_line, (uint32_t)h->dig_be, (uint32_t)h->dig_fe,
                           h->otg, p->hpd, (uint8_t)p->connector_objid, (uint8_t)p->encoder_objid, p->encoder_caps, 0);
        l->mst_port = -1;
        /* dpcd_set_source_specific_data names the ASIC (asic_id.chip_id) */
        l->chip_id = fPci ? fPci->configRead16(kIOPCIConfigDeviceID) : 0;
        /* dcn32_resource_construct: dc->caps.vbios_lttpr_enable from the
         * dce_info table (bios_parser_get_lttpr_caps) */
        uint8_t caps = 0;
        if (fDispVbios && vbios_dce_info_caps(fDispVbios, &caps) == RDNA3_OK)
            l->vbios_lttpr_enable = (caps & RD_DCE_INFO_CAPS_LTTPR_SUPPORT_ENABLE) ? 1 : 0;
    }
    l->dm = fDisplayDm;
    l->fec_policy = l->dsc_policy = rd3_arg("rdna3dsc", 0) ? 1 : 0;
    l->odm_policy = rd3_arg("rdna3odm", 0) ? 1 : 0;
    {
        /* the HPO encoders serve one stream: UHBR for one DP head only */
        int uhbr = rd3_arg("rdna3uhbr", 0) ? 1 : 0;
        for (uint32_t j = 0; uhbr && j < 4; j++)
            if (j != head && fDpLink[j] && fDpLink[j]->uhbr_policy)
                uhbr = 0;
        if (uhbr != l->uhbr_policy && l->caps_valid)
            l->verified = 0;   /* another max link cap: verified again */
        l->uhbr_policy = uhbr;
        if (uhbr && !fDccgInit) {
            dcn32_dccg_init(fDispDev);
            fDccgInit = true;
        }
    }
    uint8_t rx[16];
    memcpy(rx, l->dpcd_caps.rx, sizeof(rx));
    uint32_t oui = l->dpcd_caps.sink_dev_id;
    int had = l->caps_valid;
    int r = dcn32_dp_read_caps(l);
    if (r) {
        LOG("display: head %u: DP sink on AUX %u gives no capabilities (%d)", head, l->aux, r);
        return nullptr;
    }
    if (had && (memcmp(rx, l->dpcd_caps.rx, sizeof(rx)) || oui != l->dpcd_caps.sink_dev_id)) {
        l->verified = 0;
        LOG("display: head %u: another DP sink: its link cap is verified again on the next mode set", head);
    }
    return l;
}

/* rdna3mst=1: the MST takeover of an SST head whose sink is MST capable
 * (drm_dp_mst_topology_mgr_set_mst + the probe, as amdgpu does for such a
 * sink at detection). MST_EN in the branch ends the GOP's SST stream: the
 * caller sets the new mode as an MST stream next, or puts the SST stream
 * back (displayMstRevert, at 'cur'). 0, or an error with the SST stream
 * back. Called with fDispLock held. */
int RDNA3Accel::displayMstTakeover(uint32_t head, rdna3_dp_link *l, const rdna3_mode *cur)
{
    if (head >= 4 || !l)
        return RDNA3_EINVAL;
    if (dp_mst_read_mst_cap(fDispDev, l->aux, l->dpcd_caps.dpcd_rev) != RD_DRM_DP_MST)
        return RDNA3_ENODEV;
    if (!fMstMgr[head]) {
        fMstMgr[head] = (rdna3_mst_mgr *)IOMalloc(sizeof(rdna3_mst_mgr));
        if (!fMstMgr[head])
            return RDNA3_ENOMEM;
    }
    rdna3_mst_mgr *mgr = fMstMgr[head];
    bzero(mgr, sizeof(*mgr));
    dp_mst_mgr_init(mgr, fDispDev, l->aux);
    int r = dp_mst_set_mst(mgr, 1);
    if (!r)
        r = dp_mst_probe(mgr);
    /* the sinks: their EDIDs (REMOTE_I2C_READ), the one to drive */
    uint32_t want = rd3_arg("rdna3mstport", 0);
    int pick = -1, first = -1;
    char val[256];
    size_t o = 0;
    val[0] = 0;
    for (int i = 0; !r && i < RD_MST_MAX_PORTS; i++) {
        rdna3_mst_port *mp = &mgr->port[i];
        if (!mp->used || mp->input)
            continue;
        char name[14] = "";
        if (mp->ddps && mp->pdt == RD_DP_PEER_DEVICE_SST_SINK) {
            rdna3_edid *ed = (rdna3_edid *)IOMalloc(sizeof(rdna3_edid));
            if (dp_mst_read_edid(mgr, i) > 0 && ed && !edid_parse(mp->edid, mp->edid_len, ed))
                memcpy(name, ed->name, sizeof(name));
            if (ed)
                IOFree(ed, sizeof(*ed));
            if (first < 0)
                first = i;
            if (want && mp->port_num == want)
                pick = i;
        }
        LOG("display: head %u: MST port %u: peer type %u, %s, %u PBN, EDID %u bytes \"%s\"", head, mp->port_num,
            mp->pdt, mp->ddps ? "plugged" : "empty", mp->full_pbn, mp->edid_len, name);
        if (o < sizeof(val))
            o += (size_t)snprintf(val + o, sizeof(val) - o, "%s%u:%u:%u:%s", o ? ", " : "", mp->port_num, mp->pdt,
                                  mp->full_pbn, name);
    }
    if (pick < 0)
        pick = first;
    char key[32];
    snprintf(key, sizeof(key), "RDNA3,Head%u,MSTPorts", head);
    setProperty(key, val);
    if (r || pick < 0) {
        LOG("display: head %u: MST takeover failed (%d, %u messages, %u NAKs, %u timeouts, %u bad replies): back to SST",
            head, r, mgr->msgs, mgr->naks, mgr->timeouts, mgr->bad_replies);
        displayMstRevert(head, l, cur);
        return r ? r : RDNA3_ENODEV;
    }
    l->mst = 1;
    l->mgr = mgr;
    l->mst_port = pick;
    LOG("display: head %u: MST: the stream goes to port %u (%u messages)", head, mgr->port[pick].port_num, mgr->msgs);
    return RDNA3_OK;
}

/* back to SST: the MST stream (if any) off with its payload, MST off in
 * the manager and the branch, the head's stream on again at 'cur' as SST
 * (link_set_dpms_on with fallback). Called with fDispLock held. */
int RDNA3Accel::displayMstRevert(uint32_t head, rdna3_dp_link *l, const rdna3_mode *cur)
{
    if (head >= 4 || !l)
        return RDNA3_EINVAL;
    rdna3_mst_mgr *mgr = fMstMgr[head];
    if (!l->mst || !l->mst_alloc.stream_count)
        l->mst = 0;   /* no payload: taken down as the SST stream it was */
    dcn32_dp_link_set_dpms_off(l);
    if (mgr) {
        for (int i = 0; i < RD_MST_MAX_PORTS; i++)
            if (mgr->port[i].used && mgr->port[i].vcpi)
                dp_mst_release_time_slots(mgr, i);
        if (mgr->mst_state)
            dp_mst_set_mst(mgr, 0);
    }
    l->mst = 0;
    l->mgr = nullptr;
    l->mst_port = -1;
    int r = dcn32_dp_link_set_dpms_on(l, cur, 1);
    fDcn->head[head].dig_mode = 0;
    LOG("display: head %u: back to DP SST at %ux%u %u kHz (%d)", head, cur->h_active, cur->v_active, cur->pixel_khz, r);
    return r;
}

/* rdna3hdmi (steps hdmi, hdmi20): the HDMI half of DC's TMDS stream on a
 * DVI / HDMI head (hw/dcn32_hdmi.c through dcn32_modeset_plan_hdmi): bit 0
 * the stream attributes, AVI / vendor infoframes, AV mute and SCDC as
 * link_set_dpms_off / _on run them; bit 1 (rdna3hdmi=3) also modes above a
 * 340 MHz TMDS clock (scrambling, validated as the link encoder does: the
 * VBIOS's HDMI 6G cap, the EDID's max TMDS clock). The sink's caps come
 * from its EDID (dm_helpers_parse_edid_caps). */
static bool hdmi_cfg(const rdna3_dcn_head *h, const rdna3_display_path *p, const rdna3_edid *ed, rdna3_hdmi_cfg *hc)
{
    uint32_t a = rd3_arg("rdna3hdmi", 0);
    if (!(a & 1) || !p || !ed || (h->dig_mode != 2 && h->dig_mode != 3))
        return false;
    bzero(hc, sizeof(*hc));
    hc->flags = RD_HDMI_STREAM | ((a & 2) ? RD_HDMI_20 : 0);
    hc->ddc = p->ddc_line;
    hc->enc_caps = p->encoder_caps;
    hc->hdmi_sink = ed->hdmi;
    hc->lte_340_scramble = ed->lte_340_scramble;
    hc->max_tmds_clk_mhz = ed->max_tmds_clk_mhz;
    /* rdna3fmt (step fmt): amdgpu_dm's output format on HDMI - YCbCr 4:4:4
     * when the sink takes it, the deep colour the EDID allows (max bpc 16,
     * amdgpu's default; bit 1: 8), the OPP formatter, dither and the deep
     * colour PHY PLL with it; bit 2 forces YCbCr 4:2:2 (this port's switch) */
    uint32_t f = rd3_arg("rdna3fmt", 0);
    if (f & 1) {
        hc->flags |= RD_HDMI_FMT;
        dcn32_sink_color_caps(ed, &hc->color);
        hc->max_bpc = (f & 2) ? 8 : 16;
        hc->force_yuv422 = (f & 4) ? 1 : 0;
    }
    return true;
}

/* rdna3audio (step audio): the head's audio stream for the next mode set,
 * as DC's resource code pairs one with a stream (dc_resource.c: an audio
 * capable signal, an audio_info with modes, find_first_free_audio); bit 1
 * (rdna3audio=3) also sends the PMFW the BACO audio D3 PME message the DC
 * clock manager's enable_pme_wa sends. nullptr without the boot-arg, for a
 * DVI sink, with HDMI audio strapped off, or with no audio in the EDID.
 * Called with fDispLock held. */
rdna3_audio_stream *RDNA3Accel::displayAudio(uint32_t head, rdna3_dp_link *l, const rdna3_hdmi_cfg *hc, uint32_t fe,
                                             uint32_t otg)
{
    uint32_t a = rd3_arg("rdna3audio", 0);
    if (!(a & 1) || head >= 4 || !fAudio[head] || !fDispDev)
        return nullptr;
    if (!fAudioPool) {
        fAudioPool = (rdna3_audio_pool *)IOMalloc(sizeof(rdna3_audio_pool));
        if (!fAudioPool)
            return nullptr;
        dcn32_audio_pool_init(fDispDev, fAudioPool);
    }
    rdna3_audio_stream *au = fAudio[head];
    uint32_t signal = l ? (l->mst ? RD_SIGNAL_DISPLAY_PORT_MST : RD_SIGNAL_DISPLAY_PORT)
                    : hc && hc->hdmi_sink ? RD_SIGNAL_HDMI_TYPE_A : 0;
    bool ok = signal && dcn32_audio_wanted(signal, &au->info) &&
              (signal == RD_SIGNAL_HDMI_TYPE_A ? fAudioPool->hdmi_audio_native : fAudioPool->dp_audio);
    if (!ok) {
        if (au->az_inst >= 0 && !au->enabled) {
            dcn32_audio_release(fAudioPool, au->az_inst);
            au->az_inst = -1;
        }
        return nullptr;
    }
    if (au->az_inst < 0)
        au->az_inst = dcn32_audio_acquire(fAudioPool, fe, nullptr);
    if (au->az_inst < 0) {
        LOG("display: head %u audio: no free azalia endpoint (%u)", head, fAudioPool->count);
        return nullptr;
    }
    au->d = fDispDev;
    au->signal = signal;
    au->fe = fe;
    au->otg = otg;
    au->pme_wa = (a & 2) ? 1 : 0;
    return au;
}

/* rdna3DisplayModes: the sink's EDID timings the head can switch to
 * (dcn32_modeset_plan says yes), the running one flagged CURRENT. The list
 * is kept: rdna3DisplaySetMode names a mode by its index in it. */
IOReturn RDNA3Accel::displayModes(RD3KDisplayModes *r)
{
    if (!r || r->version != RD3K_DISPLAY_ABI_VERSION)
        return kIOReturnBadArgument;
    r->count = 0;
    if (!rd3_arg("rdna3modeset", 0))
        return kIOReturnUnsupported;
    if (!fDcn || !fDispDev || !fDispLock || r->head >= fDcn->nheads || r->head >= 4)
        return kIOReturnNotReady;
    RD3KDisplayEdid *e = (RD3KDisplayEdid *)IOMalloc(sizeof(RD3KDisplayEdid));
    rdna3_edid *ed = (rdna3_edid *)IOMalloc(sizeof(rdna3_edid));
    rdna3_dcn_modeset *ms = (rdna3_dcn_modeset *)IOMalloc(sizeof(rdna3_dcn_modeset));
    if (!fModeTab[r->head])
        fModeTab[r->head] = (rdna3_mode *)IOMalloc(sizeof(rdna3_mode) * RD3K_MAX_MODES);
    IOReturn ret = kIOReturnNoMemory;
    if (!e || !ed || !ms || !fModeTab[r->head])
        goto out;
    bzero(e, sizeof(*e));
    e->version = RD3K_DISPLAY_ABI_VERSION;
    e->head = r->head;
    {
        /* a head taken over as MST: the modes of the sink it drives */
        bool mstEdid = false;
        IOLockLock(fDispLock);
        const rdna3_dp_link *ml = fDpLink[r->head];
        if (ml && ml->mst && ml->mgr && ml->mst_port >= 0 && ml->mst_port < RD_MST_MAX_PORTS) {
            const rdna3_mst_port *mp = &ml->mgr->port[ml->mst_port];
            if (mp->edid_len && mp->edid_len <= sizeof(e->data)) {
                memcpy(e->data, mp->edid, mp->edid_len);
                e->length = mp->edid_len;
                mstEdid = true;
            }
        }
        IOLockUnlock(fDispLock);
        ret = mstEdid ? kIOReturnSuccess : displayEdid(e);
    }
    if (ret != kIOReturnSuccess || !e->length || edid_parse(e->data, e->length, ed)) {
        LOG("display: modes for head %u: no EDID (0x%x)", r->head, ret);
        ret = kIOReturnNotFound;
        goto out;
    }
    {
        IOLockLock(fDispLock);
        const rdna3_dcn_head *h = &fDcn->head[r->head];
        int aux = head_aux(h, fPaths, fNPaths);
        const rdna3_display_path *p = aux >= 0 ? path_for_aux(fPaths, fNPaths, (uint32_t)aux) : nullptr;
        uint32_t cur = head_pclk_khz(h, ed, fDispRefresh[r->head]), n = 0;
        int k = dcn32_match_head_mode(h, 0, ed);
        const rdna3_dcn_clk_limits *lim = displayClkLimits();
        rdna3_dp_link *dpl = displayDpLink(r->head, p);   /* rdna3dplink=1 and a DP head */
        rdna3_hdmi_cfg hcfg;
        const rdna3_hdmi_cfg *hc = !dpl && hdmi_cfg(h, p, ed, &hcfg) ? &hcfg : nullptr;   /* rdna3hdmi */
        if (hc && !fHdmiCfg[r->head])
            fHdmiCfg[r->head] = (rdna3_hdmi_cfg *)IOMalloc(sizeof(rdna3_hdmi_cfg));
        if (fHdmiCfg[r->head]) {
            /* the format the last mode set left running stays (rdna3fmt) */
            rdna3_crtc_format run = fHdmiCfg[r->head]->running;
            uint32_t run_cs = fHdmiCfg[r->head]->running_cs;
            if (hc)
                *fHdmiCfg[r->head] = *hc;
            else
                bzero(fHdmiCfg[r->head], sizeof(rdna3_hdmi_cfg));
            fHdmiCfg[r->head]->running = run;
            fHdmiCfg[r->head]->running_cs = run_cs;
        }
        /* rdna3audio: the sink's audio (dc_stream_construct's audio_info) */
        if (rd3_arg("rdna3audio", 0) & 1) {
            if (!fAudio[r->head]) {
                fAudio[r->head] = (rdna3_audio_stream *)IOMalloc(sizeof(rdna3_audio_stream));
                if (fAudio[r->head]) {
                    bzero(fAudio[r->head], sizeof(rdna3_audio_stream));
                    fAudio[r->head]->az_inst = -1;
                }
            }
            /* a TMDS head on a DP connector (a DP++ adaptor): DC's signal
             * decision (link_detection.c: dm_helpers_is_dp_sink_present ->
             * dp_is_sink_present) reads the DDC clock pin; DVI / HDMI
             * through the adaptor when it is not held low. Reported only. */
            if (p && p->connector == RD_ATOM_CONN_DISPLAYPORT && p->ddc_line >= 0 && !dpl &&
                (h->dig_mode == 2 || h->dig_mode == 3)) {
                uint32_t clk = 0;
                int present = dcaux_dp_is_sink_present(fDispDev, (uint32_t)p->ddc_line, 1, &clk);
                char key[40], val[64];
                snprintf(key, sizeof(key), "RDNA3,Head%u,DPSinkPresent", r->head);
                snprintf(val, sizeof(val), "%d (DDC%d clock pin %u)", present, p->ddc_line + 1, clk);
                setProperty(key, val);
                LOG("display: head %u: DP connector with a TMDS stream: dp_is_sink_present %s", r->head, val);
            }
            if (fAudio[r->head]) {
                dcn32_audio_info_from_edid(ed, &fAudio[r->head]->info);
                LOG("display: head %u audio: \"%s\", %u modes, speakers 0x%x, latency %u/%u ms", r->head,
                    (const char *)fAudio[r->head]->info.display_name, fAudio[r->head]->info.mode_count,
                    fAudio[r->head]->info.flags & 0x7f, fAudio[r->head]->info.video_latency,
                    fAudio[r->head]->info.audio_latency);
            }
        }
        if (hc)
            LOG("display: head %u HDMI: sink %s, SCDC %d, LTE scramble %d, max TMDS %u MHz, HDMI 6G %d, %u audio modes",
                r->head, hc->hdmi_sink ? "HDMI" : "DVI", ed->scdc_present, hc->lte_340_scramble, hc->max_tmds_clk_mhz,
                (hc->enc_caps & RD_ATOM_ENC_CAP_HDMI6G) ? 1 : 0, ed->audio_mode_count);
        for (uint32_t i = 0; i < ed->nmodes && n < RD3K_MAX_MODES; i++) {
            const rdna3_mode *m = &ed->mode[i];
            int why = (int)i == k ? RD_MODESET_OK
                    : !p ? RD_MODESET_BAD_HEAD
                    : hc ? dcn32_modeset_plan_hdmi(fDispDev, fDcn, r->head, p, hc, cur, m, lim, ms)
                         : dcn32_modeset_plan_dp(fDispDev, fDcn, r->head, p, dpl, cur, m, lim, ms);
            LOG("display: head %u mode %ux%u %u.%03u Hz %u kHz: %s (%d)", r->head, m->h_active, m->v_active,
                m->refresh_mhz / 1000, m->refresh_mhz % 1000, m->pixel_khz,
                (int)i == k ? "current" : why ? "refused" : "offered", why);
            if (why)
                continue;
            RD3KDisplayTiming *t = &r->mode[n];
            t->width = m->h_active;
            t->height = m->v_active;
            t->pclkKhz = m->pixel_khz;
            t->refreshMilliHz = m->refresh_mhz;
            t->hTotal = m->h_total;
            t->vTotal = m->v_total;
            t->hFront = m->h_front;
            t->hSync = m->h_sync;
            t->vFront = m->v_front;
            t->vSync = m->v_sync;
            t->hSyncNeg = m->h_sync_pos ? 0 : 1;
            t->vSyncNeg = m->v_sync_pos ? 0 : 1;
            t->flags = ((int)i == k ? RD3K_MODE_CURRENT : 0) | (m->preferred ? RD3K_MODE_PREFERRED : 0);
            fModeTab[r->head][n++] = *m;
        }
        fModeCount[r->head] = n;
        r->count = n;
        IOLockUnlock(fDispLock);
        ret = kIOReturnSuccess;
        if (lim) {
            char val[96];
            snprintf(val, sizeof(val), "DISPCLK %u DPPCLK %u kHz max, PMFW 0x%x", lim->dispclk_max_khz,
                     lim->dppclk_max_khz, lim->pmfw);
            setProperty("RDNA3,DCNClockLimits", val);
        }
    }
out:
    if (e) IOFree(e, sizeof(*e));
    if (ed) IOFree(ed, sizeof(*ed));
    if (ms) IOFree(ms, sizeof(*ms));
    return ret;
}

IOReturn RDNA3Accel::displaySetMode(RD3KDisplayMode *m)
{
    if (!m || m->version != RD3K_DISPLAY_ABI_VERSION || (m->pitch & 255) || (m->oldPitch & 3))
        return kIOReturnBadArgument;
    if (!rd3_arg("rdna3modeset", 0))
        return kIOReturnUnsupported;
    if (!fDcn || !fDispDev || !fDispLock || m->head >= fDcn->nheads || m->head >= 4 || !fModeTab[m->head] ||
        m->index >= fModeCount[m->head])
        return kIOReturnNotReady;
    rdna3_dmub *dm = fDisplayDm;
    if (!dm) {
        m->result = RDNA3_ENODEV;
        LOG("display: mode set on head %u: no DMCUB (rdna3dmub=1) to drive the PHY", m->head);
        return kIOReturnNotReady;
    }
    rdna3_dcn_modeset *ms = (rdna3_dcn_modeset *)IOMalloc(sizeof(rdna3_dcn_modeset));
    if (!ms)
        return kIOReturnNoMemory;
    IOLockLock(fDispLock);
    if (!fScale) {
        fScale = (rdna3_dcn_scale *)IOMalloc(sizeof(rdna3_dcn_scale));
        if (fScale) {
            bzero(fScale, sizeof(*fScale));
            /* line buffer + DLG/TTU for scaled sources (dcn32_set_source) */
            fScale->dlg = rd3_arg("rdna3scaledlg", 0);
        }
    }
    const rdna3_mode *want = &fModeTab[m->head][m->index];
    const rdna3_dcn_head *h = &fDcn->head[m->head];
    int aux = head_aux(h, fPaths, fNPaths);
    const rdna3_display_path *p = aux >= 0 ? path_for_aux(fPaths, fNPaths, (uint32_t)aux) : nullptr;
    /* the running pixel clock: what the last mode set left, else the
     * refresh measured at start */
    uint32_t cur = (uint32_t)((uint64_t)h->h_total * h->v_total * fDispRefresh[m->head] / 1000000ull);
    rdna3_dp_link *dpl = p ? displayDpLink(m->head, p) : nullptr;   /* rdna3dplink=1: DP heads */
    /* rdna3mst=1: an MST capable sink on an SST head: the takeover first */
    rdna3_mode curMode;
    dcn32_modeset_head_mode(h, cur, &curMode);
    bool takeover = false;
    if (dpl && !dpl->mst && rd3_arg("rdna3mst", 0) && (dpl->dpcd_caps.mstm_cap & RD_DP_MST_CAP) && cur)
        takeover = displayMstTakeover(m->head, dpl, &curMode) == RDNA3_OK;
    /* rdna3hdmi: the HDMI caps the mode list was made with */
    const rdna3_hdmi_cfg *hc = !dpl && fHdmiCfg[m->head] && (fHdmiCfg[m->head]->flags & RD_HDMI_STREAM) &&
                                       rd3_arg("rdna3hdmi", 0)
                                   ? fHdmiCfg[m->head]
                                   : nullptr;
    /* rdna3audio: the DP link's audio stream (link_set_dpms_off / _on run it) */
    if (dpl)
        dpl->audio = displayAudio(m->head, dpl, nullptr, dpl->fe, dpl->otg);
    int r = !p ? RD_MODESET_BAD_HEAD
          : hc ? dcn32_modeset_plan_hdmi(fDispDev, fDcn, m->head, p, hc, cur, want, displayClkLimits(), ms)
               : dcn32_modeset_plan_dp(fDispDev, fDcn, m->head, p, dpl, cur, want, displayClkLimits(), ms);
    /* rdna3audio: an HDMI stream's audio around the TMDS mode set */
    if (r == RD_MODESET_OK && hc && ms->dig_mode == 3)
        ms->audio = displayAudio(m->head, nullptr, hc, ms->fe, ms->otg);
    if (r != RD_MODESET_OK && takeover) {
        /* the port cannot carry the mode: the SST stream back, the mode as SST */
        LOG("display: head %u: %ux%u refused as MST stream (%d): back to SST", m->head, want->h_active,
            want->v_active, r);
        displayMstRevert(m->head, dpl, &curMode);
        takeover = false;
        r = dcn32_modeset_plan_dp(fDispDev, fDcn, m->head, p, dpl, cur, want, displayClkLimits(), ms);
    }
    bool dpHead = r == RD_MODESET_OK && ms->dp;
    /* rdna3dcseq=1 (step dcseq): DC's SYMCLK work-around on HDMI and the DIO
     * FIFO resync after the switch (hw/dcn32_hwseq.c) */
    if (r == RD_MODESET_OK)
        ms->dc_seq = rd3_arg("rdna3dcseq", 0) ? 1 : 0;
    if (r == RD_MODESET_OK)
        r = dcn32_modeset_apply(fDispDev, dm, fDcn, fScale, ms, fDispDev->fb_start + m->vramOffset, m->pitch / 4,
                                fDispDev->fb_start + m->oldVramOffset, m->oldPitch / 4, 200);
    else
        r = RDNA3_EINVAL;
    if (r != RDNA3_OK && takeover && dpl->mst) {
        /* the first MST stream did not run: the old timing came back as an
         * MST stream; SST again */
        displayMstRevert(m->head, dpl, dpHead ? &ms->dp_cur : &curMode);
    }
    if (r == RDNA3_OK) {
        fDispRefresh[m->head] = want->refresh_mhz;
        displayStateSave(m->head);   /* D4 */
        displayPowerUpdate("mode set");   /* item O (rdna3mall) */
        /* rdna3fmt: the format running from now on (the next set's "current") */
        if (hc && (hc->flags & RD_HDMI_FMT) && fHdmiCfg[m->head]) {
            fHdmiCfg[m->head]->running = ms->fmt_want;
            fHdmiCfg[m->head]->running_cs = ms->cs_want;
        }
    }
    /* steps audio / fmt: what the stream carries now */
    char audval[160] = "", fmtval[96] = "";
    {
        const rdna3_audio_stream *au = ms->audio ? ms->audio : dpl ? dpl->audio : nullptr;
        if (au && au->az_inst >= 0) {
            const uint32_t pin = dcn32_aud_read(fDispDev, (uint32_t)au->az_inst, RD_AZ_HOT_PLUG_CONTROL);
            snprintf(audval, sizeof(audval),
                     "endpoint %d, %s, %s, %u modes, speakers 0x%x, DTO source %u, %u channels at %u kHz pixel clock, "
                     "hot plug 0x%x, PME %d",
                     au->az_inst, au->signal == RD_SIGNAL_HDMI_TYPE_A ? "HDMI" : au->hpo ? "DP (HPO)" : "DP (DIO)",
                     au->enabled ? "on" : "off", au->info.mode_count, au->info.flags & 0x7f,
                     au->out.pll_info.dto_source, dcn32_audio_speakers_to_channels(au->info.flags & 0x7f),
                     au->out.crtc_info.requested_pixel_clock_100hz / 10, pin, au->pme_wa);
        }
        if (hc && (hc->flags & RD_HDMI_FMT) && r == RDNA3_OK) {
            static const char *const enc[] = { "?", "RGB", "YCbCr 4:2:2", "YCbCr 4:4:4", "YCbCr 4:2:0" };
            static const uint8_t bpc[] = { 0, 6, 8, 10, 12, 14, 16 };
            uint32_t pe = ms->fmt_want.pixel_encoding, cd = ms->fmt_want.color_depth;
            snprintf(fmtval, sizeof(fmtval), "%s %u bpc, colour space %u, sink bpc %u formats 0x%x",
                     pe < 5 ? enc[pe] : "?", cd < 7 ? bpc[cd] : 0, ms->cs_want, hc->color.bpc,
                     hc->color.color_formats);
        }
    }
    char hdmival[128] = "";
    if (hc && ms->hdmi.flags) {
        /* what the HDMI engine and the sink's SCDC say now (step hdmi / hdmi20) */
        rdna3_scdc_state scdc;
        int sr = hc->ddc >= 0 ? dcn32_hdmi_read_scdc(fDispDev, (uint32_t)hc->ddc, &scdc) : RDNA3_ENODEV;
        if (hc->ddc < 0)
            bzero(&scdc, sizeof(scdc));
        snprintf(hdmival, sizeof(hdmival),
                 "HDMI_CONTROL 0x%08x, SCDC TMDS_CONFIG 0x%02x scrambler 0x%02x flags 0x%02x%02x (%d), %u kHz",
                 dcn32_enc_hdmi_control(fDispDev, ms->fe), scdc.tmds_config, scdc.scrambler_status,
                 scdc.status_flags[1], scdc.status_flags[0], sr, want->pixel_khz);
    }
    rdna3_dcn_clocks ck;
    dcn32_clk_read(fDispDev, &ck);   /* raised or not: the report sees them */
    char dpval[128] = "";
    char dscval[96] = "", mstval[96] = "", uhbrval[160] = "", odmval[96] = "";
    if (dpHead) {
        snprintf(dpval, sizeof(dpval), "verified 0x%x x%u, running 0x%x x%u, last training %d, %u trainings, %u fallbacks",
                 dpl->verified_link_cap.link_rate, dpl->verified_link_cap.lane_count, dpl->cur_link_settings.link_rate,
                 dpl->cur_link_settings.lane_count, dpl->last_result, dpl->lt_runs, dpl->lt_fallbacks);
        /* step uhbr: the 128b/132b side (rates in 10 Mbps: 1000 / 1350 / 2000) */
        snprintf(uhbrval, sizeof(uhbrval),
                 "%s, sink rates 0x%02x, cable id 0x%02x, max %s, running %s%s, DTBCLK %u kHz, %u slots, X.Y %u.%u, ACT %d, "
                 "hblank sym %u",
                 dpl->uhbr_policy ? "on" : "off", dpl->dpcd_caps.dp_128b_132b_supported_link_rates,
                 dpl->dpcd_caps.cable_id, dcn32_dp_rate_name(dpl->max_link_cap.link_rate),
                 dcn32_dp_rate_name(dpl->cur_link_settings.link_rate), dpl->hpo_active ? " (HPO)" : " (DIO)",
                 dpl->dtbclk_khz, dpl->sst_slots, dpl->sst_vcp_x, dpl->sst_vcp_y, dpl->sst_act,
                 dpl->hblank_min_symbol_width);
        /* step odm: the segments and their OPPs */
        if (dpl->odm_opp_cnt > 1)
            snprintf(odmval, sizeof(odmval), "%u:1, OPP %u + %u, DSC %u + %u, pipe %u + %u", dpl->odm_opp_cnt,
                     dpl->odm_opp[0], dpl->odm_opp[1], dpl->odm_dsc[0], dpl->odm_dsc[1], ms->pipe, ms->odm.pipe);
        else
            snprintf(odmval, sizeof(odmval), "%s, one pipe", dpl->odm_policy ? "on" : "off");
        if (dpl->stream_dsc)
            snprintf(dscval, sizeof(dscval), "%u x %u slices, %u.%04u bpp, FEC %d", dpl->dsc_cfg.num_slices_h,
                     dpl->dsc_cfg.num_slices_v, dpl->dsc_cfg.bits_per_pixel / 16,
                     (dpl->dsc_cfg.bits_per_pixel % 16) * 625, dpl->fec_state);
        else
            snprintf(dscval, sizeof(dscval), "off, FEC %d", dpl->fec_state);
        if (dpl->mst && dpl->mgr && dpl->mst_port >= 0)
            snprintf(mstval, sizeof(mstval), "port %u, VCPI %d, PBN %u, %u slots, X.Y %u.%u, ACT %d",
                     dpl->mgr->port[dpl->mst_port].port_num, dpl->mgr->port[dpl->mst_port].vcpi, dpl->mst_pbn,
                     dpl->mst_slots, dpl->mst_vcp_x, dpl->mst_vcp_y, dpl->mst_act);
        else
            snprintf(mstval, sizeof(mstval), "SST");
    }
    IOLockUnlock(fDispLock);
    if (hdmival[0]) {
        char key[32];
        snprintf(key, sizeof(key), "RDNA3,Head%u,HDMI", m->head);
        setProperty(key, hdmival);
        LOG("display: head %u HDMI: %s", m->head, hdmival);
    }
    if (audval[0]) {
        char key[32];
        snprintf(key, sizeof(key), "RDNA3,Head%u,Audio", m->head);
        setProperty(key, audval);
        LOG("display: head %u audio: %s", m->head, audval);
    }
    if (fmtval[0]) {
        char key[32];
        snprintf(key, sizeof(key), "RDNA3,Head%u,Format", m->head);
        setProperty(key, fmtval);
        LOG("display: head %u format: %s", m->head, fmtval);
    }
    if (dpHead) {
        char key[32];
        snprintf(key, sizeof(key), "RDNA3,Head%u,DPLink", m->head);

        setProperty(key, dpval);
        LOG("display: head %u DP link: %s", m->head, dpval);
        snprintf(key, sizeof(key), "RDNA3,Head%u,DSC", m->head);
        setProperty(key, dscval);
        snprintf(key, sizeof(key), "RDNA3,Head%u,MST", m->head);
        setProperty(key, mstval);
        LOG("display: head %u DSC: %s; MST: %s", m->head, dscval, mstval);
        snprintf(key, sizeof(key), "RDNA3,Head%u,UHBR", m->head);
        setProperty(key, uhbrval);
        snprintf(key, sizeof(key), "RDNA3,Head%u,ODM", m->head);
        setProperty(key, odmval);
        LOG("display: head %u UHBR: %s; ODM: %s", m->head, uhbrval, odmval);
    }
    {
        char val[128];
        snprintf(val, sizeof(val), "VCO %u DISPCLK %u DPPCLK %u DPREFCLK %u DCFCLK %u DTBCLK %u kHz",
                 ck.vco_khz, ck.dispclk_khz, ck.dppclk_khz, ck.dprefclk_khz, ck.dcfclk_khz, ck.dtbclk_khz);
        setProperty("RDNA3,DCNClocks", val);
    }
    IOFree(ms, sizeof(*ms));
    m->result = r;
    {
        char key[32], val[64];
        snprintf(key, sizeof(key), "RDNA3,Head%u,ModeSet", m->head);
        snprintf(val, sizeof(val), "%ux%u %u kHz -> %d", want->h_active, want->v_active, want->pixel_khz, r);
        setProperty(key, val);
    }
    LOG("display: head %u mode %ux%u@%u.%03u Hz (%u kHz) -> %d", m->head, want->h_active, want->v_active,
        want->refresh_mhz / 1000, want->refresh_mhz % 1000, want->pixel_khz, r);
    return r ? kIOReturnUnsupportedMode : kIOReturnSuccess;
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
    if (c->op == RD3K_CURSOR_OP_IMAGE) {
        r = dcn32_cursor_set_image(fDispDev, fDcn, c->head, fDispDev->fb_start + c->vramOffset, c->size);
        /* rdna3cursormall=1 (step cursormall): hubp32_cursor_set_attributes'
         * MALL selection on top (a 256 x 256 cursor reads through MALL) */
        if (r == RDNA3_OK && rd3_arg("rdna3cursormall", 0))
            dcn32_hubp32_cursor_mall(fDispDev, fDcn->head[c->head].pipe, c->size, c->size, RD_DCN_CURSOR_MODE_PREMULT);
        if (r == RDNA3_OK)
            displayStateSave(c->head);   /* D4 */
    }
    else if (c->op == RD3K_CURSOR_OP_MOVE)
        r = dcn32_cursor_set_position(fDispDev, fDcn, c->head, c->x, c->y, c->visible != 0);
    IOLockUnlock(fDispLock);
    c->result = r;
    return r ? kIOReturnBadArgument : kIOReturnSuccess;
}

/* rdna3DisplayGamma: the framebuffer's gamma table as the head's MPCC
 * output gamma (hw/dcn32_gamma.c: dcn32_set_output_transfer_func ->
 * mpc3_set_output_gamma). An identity table (__is_lut_linear) and
 * channels 0 put the OGAM in bypass, which is what the GOP left. A
 * mirror clone (rdna3mirror=1) shows this head's picture through an MPCC
 * of its own and gets the same table; an extended clone head is a head of
 * fDcn and asks for itself. */
/* a restored head (power loss, mode-1 reset): the restore set the output
 * gamma to bypass; the table macOS last sent goes back (and to a mirror
 * clone of the head) */
void RDNA3Accel::displayGammaReload(uint32_t head)
{
    if (head >= 4 || !fGammaTab[head] || !fDcn || head >= fDcn->nheads)
        return;
    int r = dcn32_gamma_set_head(fDispDev, fDcn, head, fGammaTab[head]);
    if (r == RDNA3_OK && fMirrorOn && fMirror && fCloneHead < 0 && fMirror->src_otg == fDcn->head[head].otg)
        r = dcn32_gamma_apply(fDispDev, fMirror->dst_otg, fMirror->dst_mpcc, fGammaTab[head]);
    LOG("display: head %u gamma table loaded again after the restore (%d)", head, r);
}

IOReturn RDNA3Accel::displayGamma(RD3KDisplayGamma *g)
{
    if (!g || g->version != RD3K_DISPLAY_ABI_VERSION || (g->channels != 0 && g->channels != 1 && g->channels != 3) ||
        (g->channels && (g->count < 2 || g->count > RD3K_GAMMA_MAX)))
        return kIOReturnBadArgument;
    g->result = 0;
    if (!rd3_arg("rdna3gamma", 0))
        return kIOReturnUnsupported;
    if (!fDcn || !fDispDev || !fDispLock || g->head >= fDcn->nheads || g->head >= 4)
        return kIOReturnNotReady;
    rdna3_gamma_pwl *p = nullptr;
    int r = RDNA3_OK;
    if (g->channels) {
        p = (rdna3_gamma_pwl *)IOMalloc(sizeof(rdna3_gamma_pwl));
        if (!p)
            return kIOReturnNoMemory;
        r = dcn32_gamma_build(g->data, g->channels, g->count, p);
    }
    const rdna3_gamma_pwl *use = p && !p->linear ? p : nullptr;
    rdna3_gamma_state st = {};
    uint32_t mpcc[2] = {}, clone = 0;
    if (r == RDNA3_OK) {
        IOLockLock(fDispLock);
        r = dcn32_gamma_set_head(fDispDev, fDcn, g->head, use);
        if (r == RDNA3_OK && fMirrorOn && fMirror && fCloneHead < 0 && fMirror->src_otg == fDcn->head[g->head].otg) {
            r = dcn32_gamma_apply(fDispDev, fMirror->dst_otg, fMirror->dst_mpcc, use);
            clone = 1;
        }
        if (dcn32_gamma_otg_mpccs(fDispDev, fDcn->head[g->head].otg, mpcc) > 0)
            dcn32_gamma_state(fDispDev, mpcc[0], &st);
        /* kept for a restore after a power loss (amdgpu: the stream's
         * out_transfer_func, programmed again by dc_commit_streams) */
        if (r == RDNA3_OK) {
            if (use && !fGammaTab[g->head])
                fGammaTab[g->head] = (rdna3_gamma_pwl *)IOMalloc(sizeof(rdna3_gamma_pwl));
            if (use && fGammaTab[g->head]) {
                *fGammaTab[g->head] = *use;
            } else if (!use && fGammaTab[g->head]) {
                IOFree(fGammaTab[g->head], sizeof(rdna3_gamma_pwl));
                fGammaTab[g->head] = nullptr;
            }
        }
        IOLockUnlock(fDispLock);
    }
    g->result = r;
    uint32_t n = ++fGammaCalls[g->head];
    {
        static const char *const cur[] = { "bypass", "RAM A", "RAM B" };
        char key[32], val[128];
        snprintf(key, sizeof(key), "RDNA3,Head%u,Gamma", g->head);
        snprintf(val, sizeof(val), "%s %ux%u -> %d: MPCC %u mode %u %s%s, DPP degamma %u, %u calls",
                 !g->channels ? "bypass" : use ? "table" : "identity", g->channels, g->count, r, mpcc[0], st.mode,
                 st.current < 3 ? cur[st.current] : "?", clone ? " + mirror clone" : "", st.dpp_degamma, n);
        setProperty(key, val);
        /* Night Shift and ColorSync send tables often: the first ones, then every 256th */
        if (n <= 8 || !(n & 255) || r)
            LOG("display: head %u gamma %s", g->head, val);
    }
    if (p)
        IOFree(p, sizeof(rdna3_gamma_pwl));
    return r ? kIOReturnBadArgument : kIOReturnSuccess;
}
