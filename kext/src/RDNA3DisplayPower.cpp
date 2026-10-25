/*
 * RDNA3DisplayPower.cpp - item O: the DCN 3.2 display power features that
 * let UCLK / FCLK drop and the GPU idle with the displays on
 * (hw/dcn32_pstate.c, hw/dcn32_subvp.c). Nothing here runs without the
 * boot-args; both default off:
 *
 *   rdna3mall=1   watermark range table to the PMFW (dcn32_notify_wm_ranges),
 *                 DC's DML decision on the lit heads, the four watermark
 *                 sets, UCLK / FCLK p-state support and hard minimums
 *                 (dcn32_update_clocks), MALL_SEL, and the idle CAB
 *                 (dcn32_apply_idle_power_optimizations after a second
 *                 without flips). The DETs stay as the heads run them.
 *   rdna3mall=2   the same, with DC's DET / compressed-buffer split
 *                 (dcn32_set_det_allocations) programmed too.
 *   rdna3subvp=1  (with rdna3mall) SubVP: a phantom pipe for a head that
 *                 cannot hide the UCLK switch otherwise, the DMCUB's SubVP
 *                 command, the HW lock around full updates and immediate
 *                 flips, the flipped address for the firmware.
 *
 * Updates run after the bring-up's init (displayPowerInit), a mode set, a
 * display sleep / wake and the mirror lighting; each publishes
 * RDNA3,DCNPower and RDNA3,Head<n>,PState. The mirror's forced p-state
 * allow (rdna3pstate) stays when the mirror is lit: UCLK then drops only
 * for configurations whose every plane switches in active.
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
#include "dcn32_clk.h"
#include "dcn32_clone.h"
#include "dmub32.h"
#include "dcn32_pstate.h"
#include "dcn32_subvp.h"
}

#define LOG(fmt, ...) IOLog("RDNA3: " fmt "\n", ##__VA_ARGS__)
#define POWER_IDLE_MS 1000

void RDNA3Accel::displayPowerInit(rdna3_dev *d, rdna3_dmub *dm, uint64_t wmTableOff)
{
    uint32_t mall = rd3_arg("rdna3mall", 0);
    if (!mall || !fDcn || !fDispDev || !fDispLock || !d || !dm || !dm->hw_init)
        return;
    if (!fPower)
        fPower = (rdna3_dcn32_power *)IOMalloc(sizeof(rdna3_dcn32_power));
    if (!fSubvp)
        fSubvp = (rdna3_subvp_state *)IOMalloc(sizeof(rdna3_subvp_state));
    if (!fPowerCtx)
        fPowerCtx = (rdna3_pstate_ctx *)IOMalloc(sizeof(rdna3_pstate_ctx));
    if (!fPowerCtxMin)
        fPowerCtxMin = (rdna3_pstate_ctx *)IOMalloc(sizeof(rdna3_pstate_ctx));
    if (!fPower || !fSubvp || !fPowerCtx || !fPowerCtxMin) {
        setProperty("RDNA3,DCNPowerInit", "failed: no memory");
        return;
    }
    bzero(fSubvp, sizeof(*fSubvp));
    IOLockLock(fDispLock);
    /* the mirror forced the arbiter's allow (rdna3pstate): kept */
    bool keep = fMirrorOn && fMirror && (fMirror->pstate_force & 1);
    int r = dcn32_power_init(d, fPower, fDispVbios, wmTableOff, keep ? 1 : 0);
    fPowerOn = fPower->bw.smu_present && fPower->bw.nstates > 0;
    char val[200];
    snprintf(val, sizeof(val), "%d: PMFW 0x%x, %u states, UCLK %u..%u MHz, FCLK %u..%u MHz, DCFCLK %u..%u MHz, "
             "latency %u us, %u channels%s%s", r, fPower->bw.pmfw, fPower->bw.nstates, fPower->bw.memclk_mhz[0],
             fPower->bw.max_memclk_mhz, fPower->bw.fclk_mhz[0], fPower->bw.fclk_mhz[fPower->bw.n_fclk ? fPower->bw.n_fclk - 1 : 0],
             fPower->bw.dcfclk_mhz[0], fPower->bw.dcfclk_mhz[fPower->bw.n_dcfclk ? fPower->bw.n_dcfclk - 1 : 0],
             fPower->soc.dram_clock_change_latency_ns / 1000, fPower->soc.num_chans, fPower->soc.vbios_bb ? ", VBIOS" : "",
             keep ? ", p-state allow kept forced (mirror)" : "");
    if (fPowerOn)
        displayPowerUpdate("init");
    IOLockUnlock(fDispLock);
    setProperty("RDNA3,DCNPowerInit", val);
    LOG("display power: init %s", val);
    if (fPowerOn && !fPowerIdleCall) {
        fPowerStop = false;
        fPowerIdleCall = rd3_tcall_alloc("core.displayPowerTick", (thread_call_func_t)displayPowerTick, this);
        if (fPowerIdleCall) {
            uint64_t dl;
            clock_interval_to_deadline(POWER_IDLE_MS, kMillisecondScale, &dl);
            rd3_tcall_enter_delayed(fPowerIdleCall, dl);
        }
    }
}

/* the pixel clock of every lit head from its timing and refresh; false
 * when one is unknown (the decision would leave a head out) */
bool RDNA3Accel::displayPowerPclk(uint32_t *pclk, uint32_t *cursor)
{
    if (!fDispMeasured) {
        bool need = false;
        for (uint32_t i = 0; i < fDcn->nheads && i < 4; i++)
            need |= fDispRefresh[i] == 0;
        if (need)
            displayMeasureRefresh();
    }
    for (uint32_t i = 0; i < fDcn->nheads && i < RD_DCN_MAX_PIPES; i++) {
        const rdna3_dcn_head *h = &fDcn->head[i];
        uint32_t mhz = i < 4 ? fDispRefresh[i] : 0;
        pclk[i] = (uint32_t)((uint64_t)h->h_total * h->v_total * mhz / 1000000ull);
        if (fMirrorOn && fMirror && fCloneHead >= 0 && (uint32_t)fCloneHead == i)
            pclk[i] = fMirror->pclk_khz;   /* the clone's head: the plan's pixel clock */
        cursor[i] = fDcn->cursor_on[h->pipe] ? fDcn->cursor_size[h->pipe] * fDcn->cursor_size[h->pipe] * 4 : 0;
        if (!pclk[i])
            return false;
    }
    return true;
}

/* one update of the display power state for what runs now. Called with
 * fDispLock held. */
void RDNA3Accel::displayPowerUpdate(const char *why)
{
    if (!fPowerOn || !fPower || !fPowerCtx || !fPowerCtxMin || !fSubvp || !fDispDev || !fDcn || fDispAsleep)
        return;
    rdna3_dmub *dm = fDisplayDm;
    rdna3_pstate_ctx *ctx = fPowerCtx;
    uint32_t pclk[RD_DCN_MAX_PIPES] = {}, cursor[RD_DCN_MAX_PIPES] = {};
    char val[256];
    /* a clone pipe that is not a head of its own (rdna3mirror=1) scans
     * out unseen by the decision: nothing changes then */
    if (fMirrorOn && fCloneHead < 0) {
        setProperty("RDNA3,DCNPower", "not updated: the mirror's pipe is not a head (rdna3mirror=1)");
        return;
    }
    if (!displayPowerPclk(pclk, cursor)) {
        setProperty("RDNA3,DCNPower", "not updated: a head's refresh rate is unknown");
        return;
    }
    if (dcn32_pstate_planes_from_heads(fDispDev, fDcn, pclk, cursor, ctx) != fDcn->nheads || !ctx->nplanes) {
        setProperty("RDNA3,DCNPower", "not updated: a head is not described");
        return;
    }
    if (rd3_arg("rdna3mall", 0) >= 2)
        dcn32_set_det_allocations(ctx);
    fPower->keep_force = fMirrorOn && fMirror && (fMirror->pstate_force & 1);
    fPower->hubbub.usr_force_keep = fMirrorOn && fMirror && (fMirror->pstate_force & 2);
    int r = dcn32_pstate_validate(&fPower->soc, &fPower->bw, ctx);
    if (r) {
        snprintf(val, sizeof(val), "not updated (%s): no voltage state supports the heads (%d)", why, r);
        setProperty("RDNA3,DCNPower", val);
        LOG("display power: %s", val);
        return;
    }
    /* SubVP: GECC off in the DMCUB, no forced allow (the phantom needs the
     * firmware's p-state control) */
    rdna3_subvp_state next = {};
    *fPowerCtxMin = *ctx;   /* the configuration without SubVP */
    if (rd3_arg("rdna3subvp", 0) && dm && dm->hw_init && !fPower->keep_force) {
        int gecc = (int)((dm->feature_caps[1] >> 24) & 0xff);
        dcn32_subvp_validate(&fPower->soc, &fPower->bw, ctx, 0, gecc, &next);
    }
    /* a flip-free second later the idle CAB comes back */
    if (fPower->idle && dm) {
        dcn32_apply_idle_power_optimizations(dm, &fPower->soc, &fPower->cur, 0);
        fPower->idle = 0;
    }
    if (fSubvp->enabled || next.n) {
        rdna3_dcn_clocks ck;
        dcn32_clk_read(fDispDev, &ck);
        /* SubVP to another SubVP topology: a minimal transition first
         * (dc's commit_minimal_transition_state: the new heads without
         * SubVP), then the new phantoms */
        if (fSubvp->enabled && next.n && fPower->have_cur &&
            !dcn32_is_pipe_topology_transition_seamless(&fPower->cur, ctx)) {
            rdna3_subvp_state none = {};
            int m = dcn32_subvp_commit(fDispDev, dm, fPower, fPowerCtxMin, fSubvp, &none, ck.dprefclk_khz);
            LOG("display power: %s: minimal transition (SubVP off first): %d", why, m);
        }
        r = dcn32_subvp_commit(fDispDev, dm, fPower, ctx, fSubvp, &next, ck.dprefclk_khz);
    } else {
        r = dcn32_power_apply(fDispDev, dm, fPower, ctx);
    }
    fPowerUpdates++;
    snprintf(val, sizeof(val), "%s: state %u, UCLK %s at %u MHz (%s), FCLK %s, DCFCLK %u MHz, deep sleep %u MHz, "
             "WM A urgent %u us p-state %u us, MALL %u ways, SubVP %u, update %u: %d", why, ctx->vlevel,
             ctx->p_state_change_support ? "switching" : "pinned", ctx->p_state_change_support ? ctx->dramclk_khz / 1000
                                                                                                : fPower->bw.max_memclk_mhz,
             dcn32_dram_change_name(ctx->dram_change), ctx->fclk_p_state_change_support ? "switching" : "pinned",
             ctx->dcfclk_khz / 1000, ctx->dcfclk_deep_sleep_khz / 1000, ctx->wm[RD_WM_A].urgent_ns / 1000,
             ctx->wm[RD_WM_A].pstate_change_ns / 1000, dcn32_calculate_cab_allocation(&fPower->soc, ctx), fSubvp->n,
             fPowerUpdates, r);
    setProperty("RDNA3,DCNPower", val);
    LOG("display power: %s", val);
    for (uint32_t k = 0; k < ctx->nplanes; k++) {
        const rdna3_pstate_plane *p = &ctx->plane[k];
        char key[40];
        if (p->mall == RD_MALL_PSTATE_PHANTOM)
            continue;
        for (uint32_t i = 0; i < fDcn->nheads; i++)
            if (fDcn->head[i].pipe == p->pipe) {
                snprintf(key, sizeof(key), "RDNA3,Head%u,PState", i);
                snprintf(val, sizeof(val), "pipe %u, %u kHz, DET %u KiB, active margin %d us, FCLK margin %d us, "
                         "SubVP lines %u%s", p->pipe, p->pclk_khz, p->det_kb, ctx->dml.active_margin_ns[k] / 1000,
                         ctx->dml.fclk_margin_ns[k] / 1000, ctx->dml.subvp_lines[k],
                         p->mall == RD_MALL_PSTATE_SUB_VIEWPORT ? ", SubVP main" : "");
                setProperty(key, val);
            }
    }
}

/* the flip path (fDispLock held): idle optimizations off before the new
 * frame, the SubVP main's address for the firmware, the HW lock around an
 * immediate flip of a SubVP pipe (dcn32_subvp_pipe_control_lock_fast).
 * 'when' 0 before the flip, 1 after it. */
void RDNA3Accel::displayPowerFlip(uint32_t head, uint64_t addr, bool immediate, int when)
{
    if (!fPowerOn || !fPower || !fSubvp || head >= fDcn->nheads)
        return;
    rdna3_dmub *dm = fDisplayDm;
    int idx = dcn32_subvp_index_of(fSubvp, fDcn->head[head].pipe);
    if (when == 0) {
        fPowerFlips++;
        if (fPower->idle && dm) {
            dcn32_apply_idle_power_optimizations(dm, &fPower->soc, &fPower->cur, 0);
            fPower->idle = 0;
        }
        if (idx >= 0) {
            dmub_dcn32_save_surf_addr(fDispDev, addr, 0, (uint32_t)idx);
            dcn32_subvp_pipe_control_lock_fast(dm, fSubvp, immediate, 1);
        }
    } else if (idx >= 0) {
        dcn32_subvp_pipe_control_lock_fast(dm, fSubvp, immediate, 0);
    }
}

/* every second: no flip since the last look -> idle optimizations on
 * (dc_allow_idle_optimizations(true) when amdgpu_dm turns vblank
 * interrupts off) */
void RDNA3Accel::displayPowerTick(void *owner, void *)
{
    RDNA3Accel *a = (RDNA3Accel *)owner;
    if (!a->fPowerOn || a->fPowerStop)
        return;
    if (a->fDispLock && a->fPower && !a->fDispAsleep) {
        IOLockLock(a->fDispLock);
        if (a->fPowerFlips == a->fPowerFlipsSeen && !a->fPower->idle && a->fPower->have_cur && a->fDisplayDm) {
            int r = dcn32_apply_idle_power_optimizations(a->fDisplayDm, &a->fPower->soc, &a->fPower->cur, 1);
            a->fPower->idle = r == 1;
            a->fPowerIdles += r == 1;
        }
        a->fPowerFlipsSeen = a->fPowerFlips;
        IOLockUnlock(a->fDispLock);
    }
    uint64_t dl;
    clock_interval_to_deadline(POWER_IDLE_MS, kMillisecondScale, &dl);
    if (a->fPowerIdleCall && !a->fPowerStop)
        rd3_tcall_enter_delayed(a->fPowerIdleCall, dl);
}

/* idle optimizations off, SubVP's phantoms off (the firmware told under
 * its lock, the mains' p-state no longer held by it), UCLK pinned at the
 * top when 'pin': what runs needs no decision any more. fDispLock held. */
void RDNA3Accel::displayPowerQuiesce(bool pin)
{
    if (!fPowerOn || !fPower || !fSubvp || !fPowerCtx)
        return;
    rdna3_dmub *dm = fDisplayDm;
    if (fPower->idle && dm)
        dcn32_apply_idle_power_optimizations(dm, &fPower->soc, &fPower->cur, 0);
    fPower->idle = 0;
    if (fSubvp->enabled) {
        dcn32_subvp_pipe_control_lock(dm, fSubvp, 0, 1, 1);
        dcn32_disable_phantom_streams(fDispDev, fSubvp);
        bzero(fPowerCtx, sizeof(*fPowerCtx));
        if (dm && dm->hw_init)
            dcn32_commit_subvp_config(dm, fPowerCtx, fSubvp, fPower->refclk_mhz);
        dcn32_subvp_pipe_control_lock(dm, fSubvp, 0, 1, 0);
        for (uint32_t i = 0; i < fSubvp->n; i++) {
            hubp32_update_force_pstate_disallow(fDispDev, fSubvp->ph[i].main_pipe, 0);
            hubp32_update_force_cursor_pstate_disallow(fDispDev, fSubvp->ph[i].main_pipe, 0);
        }
        uint32_t locks = fSubvp->locks;
        bzero(fSubvp, sizeof(*fSubvp));
        fSubvp->locks = locks;
    }
    fPower->have_cur = 0;
    if (pin && fPower->bw.smu_present) {
        dcn32_smu_set_hard_min(fDispDev, RD_PPCLK_UCLK, fPower->bw.max_memclk_mhz, fPower->bw.pmfw, nullptr);
        fPower->clks.p_state_change_support = 0;
    }
}

/* system sleep (fDispLock held): back to the plain state before the
 * streams go down */
void RDNA3Accel::displayPowerSuspend()
{
    displayPowerQuiesce(true);
}

/* wake (fDispLock held, fDispAsleep cleared): the update again, unless
 * the card lost its power (the PMFW's tables with it: no more updates) */
void RDNA3Accel::displayPowerWake(bool powerLost)
{
    if (!fPowerOn)
        return;
    if (powerLost) {
        fPowerOn = false;
        setProperty("RDNA3,DCNPower", "off: the card lost its power in the sleep");
        return;
    }
    displayPowerUpdate("wake");
}

/* stop / free: the DMCUB goes with the bring-up (displayMirrorOff), the
 * driver with stop() */
void RDNA3Accel::displayPowerFree()
{
    fPowerStop = true;
    if (fPowerIdleCall) {
        rd3_tcall_free(fPowerIdleCall);
        fPowerIdleCall = nullptr;
    }
    /* the flag and the pointers change under fDispLock, which the flip
     * path (displayPowerFlip) holds while it reads them; the memory is
     * freed after (PR #2 review) */
    rdna3_dcn32_power *power;
    rdna3_subvp_state *subvp;
    rdna3_pstate_ctx *ctx, *ctxMin;
    if (fDispLock)
        IOLockLock(fDispLock);
    if (fPowerOn)
        displayPowerQuiesce(true);
    fPowerOn = false;
    power = fPower;
    subvp = fSubvp;
    ctx = fPowerCtx;
    ctxMin = fPowerCtxMin;
    fPower = nullptr;
    fSubvp = nullptr;
    fPowerCtx = nullptr;
    fPowerCtxMin = nullptr;
    if (fDispLock)
        IOLockUnlock(fDispLock);
    if (power) IOFree(power, sizeof(rdna3_dcn32_power));
    if (subvp) IOFree(subvp, sizeof(rdna3_subvp_state));
    if (ctx) IOFree(ctx, sizeof(rdna3_pstate_ctx));
    if (ctxMin) IOFree(ctxMin, sizeof(rdna3_pstate_ctx));
}
