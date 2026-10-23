/*
 * RDNA3Resume.cpp - D4: the display state survives what wipes it, and
 * monitor standby on any TMDS head (hw/dcn32_state.c does the registers).
 *
 *   snapshot   each lit head when the display is set up or changed (boot
 *              heads, the clone, a mode set, a scaled source, a cursor
 *              image) and before a system sleep: dm_suspend's cached state
 *   standby    rdna3dpms=2 takes the link down on the boot heads too
 *              (link_set_dpms_off / link_set_dpms_on through the DMCUB);
 *              DP heads stay on the HUBP blank
 *   GPU reset  after a GFX / SDMA soft reset the heads are only checked
 *              (those resets do not reach DCN); after the mode-1 reset the
 *              display registers are gone and, with rdna3resume=1, come
 *              back from the snapshot (dm_resume's commit of the cached
 *              state)
 *   sleep      setPowerState(0): snapshot; with rdna3resume=1 the streams
 *              go down first (amdgpu_dm_commit_zero_streams)
 *   wake       setPowerState(1): did the card keep its power (PCI config,
 *              RCC_CONFIG_MEMSIZE, BIOS scratch ASIC_INIT_COMPLETE, PSP
 *              sOS, the heads)? With rdna3resume=1: a card without power
 *              is posted and brought up again (amdgpu_device_resume ->
 *              amdgpu_device_asic_init + the IP resume = hwInit), then the
 *              heads are restored. Without it the findings are logged and
 *              published (RDNA3,Wake*).
 *
 *   TX watch   what each head sends after its OTG (hw/dcn32_txstate.c):
 *              logged in full at the start and after the clone, then
 *              every 2 s only what changed (rdna3txwatch=0: off), and
 *              read by the report's screen looks (RD_NR_XNU_TXSTATE)
 *
 * New behaviour on the card only with rdna3resume=1 (restore, zero
 * streams, resume) or rdna3dpms=2 (standby); the snapshot and the checks
 * only read registers.
 *
 * SPDX-License-Identifier: MIT
 */
#include "RDNA3Accel.hpp"
#include "RDNA3Bringup.hpp"
#include "RDNA3Regs.h"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/kauth.h>

extern "C" {
#include "rdna3_hw.h"
#include "rdna3_bootargs.h"
#include "dcn32.h"
#include "dcn32_state.h"
#include "dcn32_txstate.h"
#include "dcn32_clone.h"
#include "dcn32_mode.h"
#include "dcaux.h"
#include "edid.h"
#include "vbios.h"
#include "atom.h"
#include "rdna3_vbl_irq.h"
}

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)

/* ------------------------------ snapshot ------------------------------ */

/* the VBIOS display path of head 'i': the one whose transmitter is the
 * head's DIG back end (the route dcn32_otg_dig read at start) */
const rdna3_display_path *RDNA3Accel::displayHeadPath(uint32_t head)
{
    const rdna3_dcn_head *h = &fDcn->head[head];
    if (!fPaths || h->dig_be < 0)
        return nullptr;
    for (uint32_t i = 0; i < fNPaths; i++)
        if (fPaths[i].transmitter == h->dig_be && fPaths[i].ddc_line >= 0)
            return &fPaths[i];
    return nullptr;
}

/* the pixel clock of head 'i': the sink's EDID mode it runs (exact); 0 lets
 * the snapshot take it from the DLG ratio */
uint32_t RDNA3Accel::displayHeadPclk(uint32_t head, const rdna3_display_path *p)
{
    uint32_t khz = 0;
    if (!p || !fConnectors)
        return 0;
    rdna3_edid *e = (rdna3_edid *)IOMalloc(sizeof(rdna3_edid));
    if (!e)
        return 0;
    for (uint32_t c = 0; c < fNConnectors && !khz; c++) {
        if (fConnectors[c].aux != (uint32_t)p->ddc_line || fConnectors[c].edid_len <= 0 ||
            edid_parse(fConnectors[c].edid, (uint32_t)fConnectors[c].edid_len, e))
            continue;
        int k = dcn32_match_head_mode(&fDcn->head[head], 0, e);
        if (k >= 0)
            khz = e->mode[k].pixel_khz;
    }
    IOFree(e, sizeof(rdna3_edid));
    return khz;
}

/* the DP link of a saved DP SST head for the state code (fDispLock held).
 * With 'sink' (the link goes up: a restore, back from standby) the sink's
 * capabilities are read again through displayDpLink (rdna3dplink=1; it
 * makes the link of a head no mode set touched; another sink after the
 * sleep: its link cap verified again); without, the link the mode set
 * made (taking it down needs no sink). nullptr: TMDS, or no link. */
rdna3_dp_link *RDNA3Accel::displayStateDpLink(uint32_t head, bool sink)
{
    if (!fDispState || head >= 4 || !fDispState->head[head].valid || !fDispState->head[head].dp)
        return nullptr;
    if (!sink)
        return fDpLink[head];
    const rdna3_display_path *p = displayHeadPath(head);
    return p ? displayDpLink(head, p) : nullptr;
}

/* fDispLock held */
void RDNA3Accel::displayStateSave(uint32_t head)
{
    if (!fDcn || !fDispDev || head >= fDcn->nheads || head >= 4)
        return;
    if (!fDispState) {
        fDispState = (rdna3_dcn_state *)IOMalloc(sizeof(rdna3_dcn_state));
        if (!fDispState)
            return;
        bzero(fDispState, sizeof(rdna3_dcn_state));
    }
    const rdna3_display_path *p = displayHeadPath(head);
    dcn32_state_save(fDispDev, fDcn, head, p, displayHeadPclk(head, p), fDispState);
    setNumber("RDNA3,DisplayStateSaves", fDispState->saves);
}

void RDNA3Accel::displayStateSaveAll()
{
    if (!fDcn || !fDispLock)
        return;
    IOLockLock(fDispLock);
    for (uint32_t i = 0; i < fDcn->nheads && i < 4; i++)
        if (!fHeadLinkDown[i])   /* a head in standby keeps its last snapshot */
            displayStateSave(i);
    IOLockUnlock(fDispLock);
}

/* ------------------------------ TX watch ------------------------------ */

/* Card run of 2026-10-15: the HDMI monitor of the boot head said "no
 * signal" while its OTG ran, its CRC moved and its pipe scanned the
 * desktop; the report could not tell when it went dark nor what changed
 * after the OTG. The DTN log's read-backs (hw/dcn32_txstate.c), kept per
 * watched chain: the boot heads, and the clone of rdna3mirror=1 (which is
 * no head of its own; with rdna3mirror=2 it is one). A watch that cannot
 * read a chain says so once, with the reason, and never stays quiet. */
#define RD_TX_ENTITIES 5          /* heads 0..3, then the clone */
#define RD_TX_CLONE    4
#define RD_TX_CHANGES_LOGGED 32   /* routine changes; then every 64th (problem / important ones always) */

struct RDNA3TxWatch {
    rdna3_tx_state boot[RD_TX_ENTITIES], last[RD_TX_ENTITIES];
    uint32_t       changes[RD_TX_ENTITIES], lastProblems[RD_TX_ENTITIES];
    bool           skipLogged[RD_TX_ENTITIES];
    uint32_t       ticks;
};

typedef struct {
    char key[32], val[384];
} TxProp;

static uint64_t txUptimeMs()
{
    uint64_t abs, ns;
    clock_get_uptime(&abs);
    absolutetime_to_nanoseconds(abs, &ns);
    return ns / 1000000ull;
}

static void txName(uint32_t e, char *buf, size_t len)
{
    if (e == RD_TX_CLONE)
        snprintf(buf, len, "clone");
    else
        snprintf(buf, len, "head %u", e);
}

static void txKey(uint32_t e, char *buf, size_t len)
{
    if (e == RD_TX_CLONE)
        snprintf(buf, len, "RDNA3,CloneTX");
    else
        snprintf(buf, len, "RDNA3,Head%u,TX", e);
}

/* the chain of entity e: a head's OTG and the DIG it was found or lit
 * with (a head whose DIG reads off now keeps the route of its snapshot),
 * or the clone's. false: no such entity. fDispLock held. */
bool RDNA3Accel::displayTxChain(uint32_t e, uint32_t *otg, int *fe, int *be)
{
    if (!fDcn || !fDispDev)
        return false;
    if (e == RD_TX_CLONE) {
        if (!fMirrorOn || !fMirror || fCloneHead >= 0)
            return false;   /* no clone, or it is a head of fDcn */
        *otg = fMirror->dst_otg;
        *fe = (int)fMirror->dst_fe;
        *be = (int)fMirror->dst_be;
        return true;
    }
    if (e >= fDcn->nheads || e >= 4)
        return false;
    const rdna3_dcn_head *h = &fDcn->head[e];
    *otg = h->otg;
    *fe = h->dig_fe;
    *be = h->dig_be;
    if ((*fe < 0 || *be < 0) && fDispState && fDispState->head[e].valid && fDispState->head[e].fe < RD_TX_MAX_DIG &&
        fDispState->head[e].be < RD_TX_MAX_DIG) {
        *fe = (int)fDispState->head[e].fe;
        *be = (int)fDispState->head[e].be;
    }
    return true;
}

/* RDNA3_OK, or why not; 'why' names it. fDispLock held. */
int RDNA3Accel::displayTxRead(uint32_t e, rdna3_tx_state *t, char *why, size_t whyLen)
{
    uint32_t otg = 0;
    int fe = -1, be = -1, r;
    if (!displayTxChain(e, &otg, &fe, &be)) {
        snprintf(why, whyLen, "no such head");
        bzero(t, sizeof(*t));
        return RDNA3_EINVAL;
    }
    r = dcn32_tx_state_read(fDispDev, otg, fe, be, t);
    if (r == RDNA3_ENODEV)
        snprintf(why, whyLen, "OTG%u_OTG_CONTROL reads all ones (DCN registers unreadable), DIG %d/%d", otg, fe, be);
    else if (r != RDNA3_OK)
        snprintf(why, whyLen, "OTG %u / DIG %d/%d out of range (%d)", otg, fe, be, r);
    else
        why[0] = 0;
    return r;
}

void RDNA3Accel::displayTxLogFull(const char *name, const char *when, const rdna3_tx_state *t)
{
    char line[RD_TX_LINE], probs[320], chain[48];
    dcn32_tx_problems_str(dcn32_tx_state_problems(t), probs, sizeof(probs));
    dcn32_tx_chain_str(t, chain, sizeof(chain));
    LOG("display: %s TX %s (%llu ms uptime), %s: problems: %s", name, when, (unsigned long long)txUptimeMs(), chain,
        probs);
    for (uint32_t g = 0; g < RD_TX_GROUPS; g++)
        if (dcn32_tx_state_format(t, g, line, sizeof(line)))
            LOG("display: %s TX   %s", name, line);
}

/* at the start: the watch's buffers and whether it runs */
void RDNA3Accel::displayTxInit()
{
    if (!rd3_arg("rdna3txwatch", 1)) {
        LOG("display: TX watch: off (rdna3txwatch=0)");
        setProperty("RDNA3,TXWatch", "off (rdna3txwatch=0)");
        return;
    }
    fTx = (RDNA3TxWatch *)IOMalloc(sizeof(RDNA3TxWatch));
    if (!fTx) {
        LOG("display: TX watch: NOT started: no memory for its state");
        setProperty("RDNA3,TXWatch", "not started: no memory");
        return;
    }
    bzero(fTx, sizeof(*fTx));
    LOG("display: TX watch: on (each head's output after its OTG: in full now and after the clone, then every 2 s "
        "from the statistics tick only what changed; reads only)");
    setProperty("RDNA3,TXWatch", "on (every 2 s from the statistics tick)");
}

/* when: logged in full ("at the start", "after the clone"); NULL (the 2 s
 * statistics tick): only a change, with the time, what it means and the
 * registers. A tick that finds fDispLock held skips (counted). Reads
 * registers only. */
void RDNA3Accel::displayTxCheck(const char *when)
{
    TxProp props[RD_TX_ENTITIES];
    uint32_t nprops = 0, ticks = 0;
    if (!fTx || !fDcn || !fDispDev || !fDispLock || fDispAsleep || fShuttingDown)
        return;
    if (when) {
        IOLockLock(fDispLock);
    } else if (!IOLockTryLock(fDispLock)) {
        /* a mode set, a flip, a restore holds it: the next tick. Said on
         * the first skip and every 30th, so a watch that never got the
         * lock is not taken for one that never ran */
        SInt32 n = OSIncrementAtomic(&fTxBusySkips) + 1;
        if (n == 1 || n % 30 == 0)
            LOG("display: TX watch: skipped, display lock busy (%d skips)", (int)n);
        setNumber("RDNA3,TXWatchBusySkips", (uint32_t)n);
        return;
    }
    /* a system sleep, the shutdown or the stop (fTx freed) may have begun
     * while we waited; fTx is only touched under the lock from here on */
    RDNA3TxWatch *tx = fTx;
    if (!tx || fDispAsleep || fShuttingDown) {
        IOLockUnlock(fDispLock);
        return;
    }
    if (!when && ++tx->ticks == 1)
        LOG("display: TX watch: first tick at %llu ms uptime", (unsigned long long)txUptimeMs());
    ticks = tx->ticks;
    for (uint32_t e = 0; e < RD_TX_ENTITIES; e++) {
        rdna3_tx_state now;
        char diff[640], probs[320], why[128], name[16];
        uint32_t otg = 0;
        int fe = -1, be = -1;
        bool publish = false;
        if (!displayTxChain(e, &otg, &fe, &be))
            continue;
        txName(e, name, sizeof(name));
        if (displayTxRead(e, &now, why, sizeof(why)) != RDNA3_OK) {
            if (!tx->skipLogged[e]) {
                LOG("display: %s TX not read: %s", name, why);
                tx->skipLogged[e] = true;
                txKey(e, props[nprops].key, sizeof(props[nprops].key));
                snprintf(props[nprops].val, sizeof(props[nprops].val), "not read: %s", why);
                nprops++;
            }
            continue;
        }
        if (tx->skipLogged[e]) {
            LOG("display: %s TX readable again", name);
            tx->skipLogged[e] = false;
            publish = true;   /* the property still says "not read" */
        }
        uint32_t p = dcn32_tx_state_problems(&now);
        rdna3_tx_state *last = &tx->last[e];
        if (!tx->boot[e].valid)
            tx->boot[e] = now;
        if (when || !last->valid) {
            displayTxLogFull(name, when ? when : "first look", &now);
            if (when && dcn32_tx_state_diff(&tx->boot[e], &now, diff, sizeof(diff), nullptr))
                LOG("display: %s TX %s, changed since the start: %s", name, when, diff);
            publish = true;
        } else {
            int imp = 0;
            uint32_t n = dcn32_tx_state_diff(last, &now, diff, sizeof(diff), &imp);
            if (n == RD_TX_NREGS && imp && (last->otg != now.otg || last->fe != now.fe || last->be != now.be)) {
                /* another chain (a mode set, the DIG found again): it is the new start */
                uint32_t c = ++tx->changes[e];
                dcn32_tx_problems_str(p, probs, sizeof(probs));
                LOG("display: %s TX change #%u at %llu ms uptime: problems: %s; %s (re-baselined)", name, c,
                    (unsigned long long)txUptimeMs(), probs, diff);
                displayTxLogFull(name, "new chain", &now);
                tx->boot[e] = now;
                publish = true;
            } else if (n || p != tx->lastProblems[e]) {
                uint32_t c = ++tx->changes[e];
                dcn32_tx_problems_str(p, probs, sizeof(probs));
                if (p != tx->lastProblems[e] || imp || c <= RD_TX_CHANGES_LOGGED || c % 64 == 0)
                    LOG("display: %s TX change #%u at %llu ms uptime: problems: %s; %u register(s): %s%s", name, c,
                        (unsigned long long)txUptimeMs(), probs, n, n ? diff : "(only status fields)",
                        e < 4 && fHeadLinkDown[e] ? " (monitor standby)" : "");
                if (c == RD_TX_CHANGES_LOGGED)
                    LOG("display: %s TX: %u changes logged; from now on every 64th, and every change of the "
                        "problems, the routing, the enables or the clock selects", name, c);
                publish = true;
            }
        }
        *last = now;
        tx->lastProblems[e] = p;
        if (publish && nprops < RD_TX_ENTITIES) {
            dcn32_tx_problems_str(p, probs, sizeof(probs));
            txKey(e, props[nprops].key, sizeof(props[nprops].key));
            snprintf(props[nprops].val, sizeof(props[nprops].val), "%u change(s) since the start; problems: %s",
                     tx->changes[e], probs);
            nprops++;
        }
    }
    IOLockUnlock(fDispLock);
    /* the registry outside the display lock */
    for (uint32_t i = 0; i < nprops; i++)
        if (!setProperty(props[i].key, props[i].val) && !fTxPublishFailLogged) {
            fTxPublishFailLogged = true;
            LOG("display: TX watch: could not publish %s", props[i].key);
        }
    if (!when)
        setNumber("RDNA3,TXWatchTicks", ticks);
}

/* RD_NR_XNU_TXSTATE: the chain on pipe t->pipe (a head, or the clone of
 * rdna3mirror=1) as text for the report's screen looks (rdna3_drminfo
 * --scanout). Root only, reads registers. valid 0 with a "not read: ..."
 * line when it cannot be read; nothing for a pipe without a chain. */
int RDNA3Accel::drmTxState(void *ctx, struct rd_xnu_txstate *t)
{
    RDNA3Accel *a = (RDNA3Accel *)ctx;
    uint32_t pipe = t->pipe;
    size_t n = 0;
    if (!kauth_cred_issuser(kauth_cred_get()))
        return -RD_EPERM;
    if (!a->fDcn || !a->fDispDev || !a->fDispLock)
        return -RD_ENODEV;
    if (pipe >= RD_DCN_MAX_PIPES)
        return -RD_EINVAL;
    bzero(t, sizeof(*t));
    t->pipe = pipe;
#define TXT(...)                                                                         \
    do {                                                                                 \
        if (n < sizeof(t->text)) {                                                       \
            int w_ = snprintf(t->text + n, sizeof(t->text) - n, __VA_ARGS__);            \
            n = w_ < 0 ? sizeof(t->text) : n + (size_t)w_;                               \
        }                                                                                \
    } while (0)
    IOLockLock(a->fDispLock);
    if (a->fDispAsleep || a->fShuttingDown) {
        IOLockUnlock(a->fDispLock);
        TXT("not read: %s\n", a->fShuttingDown ? "the system is shutting down" : "the display is asleep (system sleep)");
        return 0;
    }
    for (uint32_t e = 0; e < RD_TX_ENTITIES; e++) {
        rdna3_tx_state now;
        char line[RD_TX_LINE], diff[640], why[128], name[16];
        uint32_t otg = 0;
        int fe = -1, be = -1;
        if (!a->displayTxChain(e, &otg, &fe, &be))
            continue;
        if (e == RD_TX_CLONE ? a->fMirror->dst_pipe != pipe : a->fDcn->head[e].pipe != pipe)
            continue;
        txName(e, name, sizeof(name));
        if (a->displayTxRead(e, &now, why, sizeof(why)) != RDNA3_OK) {
            TXT("%s not read: %s\n", name, why);
            break;
        }
        t->valid = 1;
        t->problems = dcn32_tx_state_problems(&now);
        t->changes = a->fTx ? a->fTx->changes[e] : 0;
        dcn32_tx_chain_str(&now, line, sizeof(line));
        TXT("%s: %s\n", name, line);
        dcn32_tx_problems_str(t->problems, line, sizeof(line));
        TXT("problems: %s\n", line);
        for (uint32_t g = 0; g < RD_TX_GROUPS; g++)
            if (dcn32_tx_state_format(&now, g, line, sizeof(line)))
                TXT("%s\n", line);
        if (!a->fTx)
            TXT("watch: %s\n", rd3_arg("rdna3txwatch", 1) ? "not started (no memory)" : "off (rdna3txwatch=0)");
        else if (!a->fTx->ticks && a->fTxBusySkips)
            TXT("watch: no tick yet, %u skipped: display lock busy\n", (uint32_t)a->fTxBusySkips);
        else if (!a->fTx->ticks)
            TXT("watch: never ticked (it runs from the statistics tick)\n");
        else
            TXT("watch: %u tick(s), %u change(s), %u busy skip(s)\n", a->fTx->ticks, t->changes,
                (uint32_t)a->fTxBusySkips);
        if (a->fTx && a->fTx->boot[e].valid) {
            uint32_t d = dcn32_tx_state_diff(&a->fTx->boot[e], &now, diff, sizeof(diff), nullptr);
            TXT("since the start: %s\n", d ? diff : "unchanged");
        }
        break;
    }
    IOLockUnlock(a->fDispLock);
    /* a cut text says so */
    if (n >= sizeof(t->text))
        memcpy(t->text + sizeof(t->text) - 5, "...\n", 5);
#undef TXT
    return 0;
}

void RDNA3Accel::displayStateFree()
{
    if (fWakeCall) {
        thread_call_cancel_wait(fWakeCall);
        thread_call_free(fWakeCall);
        fWakeCall = nullptr;
    }
    /* under the display lock: a screen look (RD_NR_XNU_TXSTATE) reads both;
     * the statistics tick that watches is stopped by now */
    if (fDispLock)
        IOLockLock(fDispLock);
    if (fDispState)
        IOFree(fDispState, sizeof(rdna3_dcn_state));
    fDispState = nullptr;
    if (fTx)
        IOFree(fTx, sizeof(RDNA3TxWatch));
    fTx = nullptr;
    if (fDispLock)
        IOLockUnlock(fDispLock);
}

/* --------------------------- standby, blank --------------------------- */

/* what a restore brings back: the framebuffer's last power request */
void RDNA3Accel::displayNoteBlank(uint32_t head, bool blank)
{
    if (head < 4)
        fHeadBlank[head] = blank;
}

/* rdna3dpms=2 on a head we did not light ourselves (fDispLock held):
 * RDNA3_EINVAL when it cannot (a DP head without its link, MST, no
 * snapshot, no DMCUB): the caller blanks the HUBP instead */
int RDNA3Accel::displayHeadDpms(uint32_t head, bool blank)
{
    rdna3_dmub *dm = fDisplayDm;
    if (!dm || head >= 4 || !fDcn || head >= fDcn->nheads)
        return RDNA3_EINVAL;
    if (blank) {
        if (fHeadLinkDown[head])
            return RDNA3_OK;
        displayStateSave(head);   /* the head as it runs, for the way back */
        int r = dcn32_head_link(fDispDev, dm, fDcn, fDispState, head, 0, displayStateDpLink(head, false));
        if (r == RDNA3_OK) {
            fHeadLinkDown[head] = true;
            displayStateRestore("monitor standby", false);   /* RDNA3,Head<n>,State */
        }
        return r;
    }
    if (!fHeadLinkDown[head])
        return RDNA3_EINVAL;   /* the link never went down: a plain unblank */
    int r = dcn32_head_link(fDispDev, dm, fDcn, fDispState, head, 1, displayStateDpLink(head, true));
    if (r == RDNA3_OK) {
        fHeadLinkDown[head] = false;
        displayStateRestore("back from standby", false);
    }
    return r;
}

/* the link of a head in standby up again (fDispLock held): the clone
 * through its own path, the other heads through dcn32_head_link */
void RDNA3Accel::displayLinkUp(uint32_t head)
{
    int r;
    if (head >= 4 || !fHeadLinkDown[head])
        return;
    if (fMirrorOn && fMirror && fMirrorDm && fCloneHead >= 0 && (uint32_t)fCloneHead == head)
        r = dcn32_clone_link(fDispDev, fMirrorDm, fMirror, 1);
    else
        r = displayHeadDpms(head, false);
    if (r == RDNA3_OK)
        fHeadLinkDown[head] = false;
    LOG("display: head %u link back after the wake (%d)", head, r);
}

/* rdna3hotplug=2: the monitor of 'head' is back (fDispLock held). Its link
 * goes down and up again the way the monitor-standby path does it (the
 * NullMoth driver relights such a head with a full mode set); a head in
 * standby waits for macOS's unblank (displayLinkUp) */
int RDNA3Accel::displayHeadRelight(uint32_t head)
{
    int r;
    if (head >= 4)
        return RDNA3_EINVAL;
    if (fHeadLinkDown[head])
        return RDNA3_OK;
    if (fMirrorOn && fMirror && fMirrorDm && fCloneHead >= 0 && (uint32_t)fCloneHead == head) {
        r = dcn32_clone_link(fDispDev, fMirrorDm, fMirror, 0);
        if (r == RDNA3_OK)
            r = dcn32_clone_link(fDispDev, fMirrorDm, fMirror, 1);
        return r;
    }
    r = displayHeadDpms(head, true);
    if (r == RDNA3_OK)
        r = displayHeadDpms(head, false);
    return r;
}

/* ------------------------------ restore ------------------------------ */

/* Check every saved head; with 'restore' the lost ones are programmed
 * from their snapshot. fDispLock held. Returns the heads still lost. */
uint32_t RDNA3Accel::displayStateRestore(const char *why, bool restore)
{
    uint32_t lost = 0, back = 0, n = fDcn ? fDcn->nheads : 0;
    char val[160];
    if (!fDispState || !fDispDev)
        return 0;
    for (uint32_t i = 0; i < n && i < 4; i++) {
        rdna3_dcn_state_check k;
        char key[40];
        int r = dcn32_state_check(fDispDev, fDispState, i, 50, &k);
        if (r < 0)
            continue;
        if (r && restore) {
            int rr = dcn32_state_restore(fDispDev, fDisplayDm, fDcn, fDispState, i, fHeadBlank[i],
                                         fHeadLinkDown[i], displayStateDpLink(i, !fHeadLinkDown[i]));
            if (!rr)
                displayGammaReload(i);
            r = rr ? 1 : dcn32_state_check(fDispDev, fDispState, i, 50, &k);
            snprintf(val, sizeof(val), "%s: restored (%d), %s, %u frames in 50 ms", why, rr,
                     r ? "still LOST" : "running", k.frames);
            if (!r)
                back++;
        } else {
            snprintf(val, sizeof(val), "%s: %s (power %d OTG %d DIG %d timing %d sysctx %d routing %d, %u regs differ)",
                     why, r ? "LOST" : "intact", k.powered, k.otg_on, k.dig_on, k.timing_ok, k.sys_ok, k.routed,
                     k.ndiff);
        }
        lost += r ? 1 : 0;
        snprintf(key, sizeof(key), "RDNA3,Head%u,State", i);
        setProperty(key, val);
        LOG("display: head %u %s", i, val);
    }
    setNumber("RDNA3,DisplayStateRestores", fDispState->restores);
    if (back)
        LOG("display: %s: %u head(s) back from the snapshot", why, back);
    return lost;
}

/* the picture in VRAM is gone while the monitors stayed (the restore
 * shows whatever VRAM holds now): every framebuffer sees
 * RD3KDisplayHotplug.lost move and raises its connect change, so
 * WindowServer draws everything again */
void RDNA3Accel::displayPictureLost(const char *why)
{
    fDispLostGen++;
    setNumber("RDNA3,DisplayPictureLost", fDispLostGen);
    LOG("display: the picture in VRAM was lost (%s): the framebuffers ask WindowServer to draw again (#%u)", why,
        fDispLostGen);
}

/* RDNA3Bringup after a GPU recovery: a GFX / SDMA soft reset leaves DCN
 * alone (verify only); the mode-1 reset takes it down (restore with
 * rdna3resume=1). Called under the DRM lock. */
void RDNA3Accel::displayAfterReset(bool chipReset)
{
    if (!fDispState || !fDispLock || !fDcn)
        return;
    bool restore = chipReset && rd3_arg("rdna3resume", 0) != 0;
    IOLockLock(fDispLock);
    uint32_t lost = displayStateRestore(chipReset ? "after the mode-1 reset" : "after a GFX/SDMA reset", restore);
    IOLockUnlock(fDispLock);
    setNumber("RDNA3,DisplayLostAfterReset", lost);
    if (!chipReset)
        LOG("display: GFX/SDMA reset does not reach DCN: %u of %u heads %s", lost ? lost : fDcn->nheads, fDcn->nheads,
            lost ? "LOST (unexpected)" : "still lit");
    else if (lost && !restore)
        LOG("display: the mode-1 reset took %u head(s) down; rdna3resume=1 restores them from the snapshot", lost);
    if (chipReset && fBringup && fBringup->lastVramLost())
        displayPictureLost("mode-1 reset");
    /* N42, amdgpu_irq_gpu_reset_resume_helper: the vblank / pflip sources
     * again - only on a DCN that is back (restored, or the heads stayed) */
    if (chipReset)
        displayIrqResync("mode-1 reset", rd3_virq_reset_dcn_up(restore ? 1 : 0, lost) != 0);
}

/* --------------------------- sleep and wake --------------------------- */

void RDNA3Accel::displayWillSleep()
{
    if (!fDcn || !fDispLock || fShuttingDown)
        return;
    hotplugSuspend();   /* amdgpu_dm_irq_suspend: no HPD looks until the wake work */
    displayStateSaveAll();
    IOLockLock(fDispLock);
    fDispAsleep = true;
    fSleeps++;
    /* dm_suspend: amdgpu_dm_commit_zero_streams before the power goes */
    displayPowerSuspend();   /* item O: SubVP / CAB off before the streams */
    if (rd3_arg("rdna3resume", 0) && fDisplayDm && fDispState)
        for (uint32_t i = 0; i < fDcn->nheads && i < 4; i++)
            dcn32_state_head_off(fDispDev, fDisplayDm, fDcn, fDispState, i, displayStateDpLink(i, false));
    IOLockUnlock(fDispLock);
    statsStop();   /* no SMU traffic to a card that may lose power */
    setNumber("RDNA3,Sleeps", fSleeps);
    LOG("display: system sleep #%u: %u head(s) saved%s", fSleeps, fDcn->nheads,
        rd3_arg("rdna3resume", 0) ? ", streams down (rdna3resume=1)" : "");
}

void RDNA3Accel::displayDidWake()
{
    if (!fDispAsleep)
        return;   /* the first power-on at registration, or no sleep */
    if (!fWakeCall)
        fWakeCall = thread_call_allocate((thread_call_func_t)displayWakeWork, this);
    if (fWakeCall)
        thread_call_enter(fWakeCall);
}

/* amdgpu_device_resume, as far as this driver gets: detection first */
void RDNA3Accel::displayWakeWork(void *owner, void *)
{
    RDNA3Accel *a = (RDNA3Accel *)owner;
    bool resume = rd3_arg("rdna3resume", 0) != 0;
    uint16_t vendor = a->fPci->configRead16(kIOPCIConfigVendorID);
    uint16_t cmd = a->fPci->configRead16(kIOPCIConfigCommand);
    uint32_t memsize = a->fMmio ? a->rreg(mmRCC_CONFIG_MEMSIZE) : ~0u;
    int needPost = -1, sos = -1;
    uint32_t lost = 0;
    bool resumed = false;
    char val[256];

    a->fWakes++;
    if (a->fDispLock)
        IOLockLock(a->fDispLock);
    if (a->fAtom)
        needPost = atom_need_post(a->fAtom);   /* BIOS scratch 7: ASIC_INIT_COMPLETE */
    if (a->fDispState)
        lost = a->displayStateRestore("wake (check)", false);
    if (a->fDispLock)
        IOLockUnlock(a->fDispLock);
    if (a->fBringup && a->fBringup->finished() && a->fDrmLock) {
        IOLockLock(a->fDrmLock);
        sos = a->fBringup->sosAlive();
        IOLockUnlock(a->fDrmLock);
    }
    bool powerLost = vendor != 0x1002 || memsize == ~0u || memsize == 0 || needPost == 1 || sos == 0;
    /* P4: after amdgpu_device_suspend the IPs are down whether or not the
     * card kept its power: amdgpu_device_resume runs either way */
    bool suspended = a->fBringup && a->fBringup->suspended();
    snprintf(val, sizeof(val), "wake #%u: PCI %04x cmd 0x%04x, MEMSIZE %u MiB, ASIC_Init %s, PSP sOS %s, %u head(s) lost: %s",
             a->fWakes, vendor, cmd, memsize, needPost < 0 ? "?" : needPost ? "NEEDED" : "done",
             sos < 0 ? "?" : sos ? "running" : "GONE", lost,
             powerLost ? "the card lost its power"
             : !lost ? "the card kept its power and the display"
             : resume ? "the card kept its power (the streams went down for the sleep)"
                      : "the card kept its power, the display did not");
    a->setProperty("RDNA3,Wake", val);
    a->setNumber("RDNA3,WakePowerLost", powerLost ? 1 : 0);
    a->setNumber("RDNA3,Wakes", a->fWakes);
    LOG("%s", val);

    if (powerLost || suspended) {
        if (resume && a->fBringup && a->fBringup->finished() && a->fDrmLock) {
            IOLockLock(a->fDrmLock);
            bool ok = a->fBringup->resumeAfterPowerLoss();
            resumed = ok;
            a->drmVmReplayIfNeeded();
            /* the evicted VRAM back before the display restore shows it */
            a->vramRestore(ok && a->fBringup->lastResumeLostVram());
            a->drmVramLostSync();   /* what vramRestore could not bring back */
            if (ok && a->fDrm) {   /* amdgpu_userq_resume, with their MQDs back */
                int uf = rdna3_drm_device_userq_resume(a->fDrm);
                if (uf)
                    LOG("wake: %d user queue(s) not back on the scheduler (hung)", uf);
                a->setNumber("RDNA3,WakeUserQueuesActive", (uint64_t)rdna3_drm_device_userq_active(a->fDrm));
            }
            IOLockUnlock(a->fDrmLock);
            a->setNumber("RDNA3,WakeResumeResult", ok ? 1 : 0);
            LOG("wake: GPU bring-up after the %s %s", powerLost ? "power loss" : "suspend",
                ok ? "done" : "FAILED");
        } else {
            LOG("wake: the GPU needs ASIC_Init (atom_asic_init), the PSP / SMU / firmware bring-up from level 4 "
                "(hwInit) and the display restore (dcn32_state_restore); rdna3resume=1 tries that. The display "
                "stays dark and the rings dead until a restart.");
        }
    }
    if (a->fDispLock) {
        IOLockLock(a->fDispLock);
        a->fDispAsleep = false;
        a->fScanoutAsleepLogged = false;   /* a look in the next sleep logs again */
        if (resume && (powerLost || suspended)) {
            /* dcn32_init_hw: "we want to turn off all dp displays before
             * doing detection": a DP sink still in D0 is blanked and put
             * to D3; the restore trains it again */
            int nb = dcn32_dp_link_blank_all_dp_displays(a->fDpLink, 4);
            if (nb)
                LOG("wake: %d DP display(s) blanked before the restore (link_blank_all_dp_displays)", nb);
        }
        if (resume) {
            lost = a->displayStateRestore("wake", true);
        } else if (!powerLost && a->fDcn) {
            /* power requests that came while asleep */
            for (uint32_t i = 0; i < a->fDcn->nheads && i < 4; i++)
                if (!a->fHeadLinkDown[i])
                    dcn32_set_blank(a->fDispDev, a->fDcn, i, a->fHeadBlank[i] ? 1 : 0);
        }
        /* a head whose link went down for the monitor's standby and that
         * macOS unblanked while asleep: its link comes back now (the
         * unblank itself was deferred) */
        if (a->fDcn)
            for (uint32_t i = 0; i < a->fDcn->nheads && i < 4; i++)
                if (a->fHeadLinkDown[i] && !a->fHeadBlank[i])
                    a->displayLinkUp(i);
        a->displayPowerWake(powerLost || suspended);   /* item O: after the heads are back (hwInit reloaded the DMCUB) */
        IOLockUnlock(a->fDispLock);
    }
    a->setNumber("RDNA3,WakeHeadsLost", lost);
    /* the evicted VRAM came back unless the resume did not run, failed, or
     * the eviction was incomplete (lastResumeLostVram stays set) */
    if (powerLost && (!resume || !resumed || (a->fBringup && a->fBringup->lastResumeLostVram())))
        a->displayPictureLost("wake after a power loss");
    a->hotplugResume();   /* link_resume: the HPD filters again, then the looks */
    /* N42, amdgpu_dm_irq_resume_early: the vblank / pflip sources again -
     * only on a DCN that kept its power or was resumed (ECC review) */
    a->displayIrqResync("wake", rd3_virq_wake_dcn_up(powerLost ? 1 : 0, suspended ? 1 : 0, resume ? 1 : 0,
                                                     resumed ? 1 : 0) != 0);
    /* VCN / JPEG (rdna3vcn): amdgpu_device_ip_resume_phase2 when the card
     * kept its power; after a power loss resumeAfterPowerLoss's hwInit
     * resumed them already (vcnStart), or they wait for the next bring-up */
    if (a->fBringup && a->fBringup->finished() && a->fBringup->vcnWanted() && a->fDrmLock) {
        IOLockLock(a->fDrmLock);
        a->fBringup->vcnResume();
        IOLockUnlock(a->fDrmLock);
    }
    if (a->fBringup)
        a->statsStart();
}
