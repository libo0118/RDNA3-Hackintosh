/*
 * rdna3_fb_policy.h - RDNA3Framebuffer's decisions that do not need IOKit:
 * which framebuffer opens first, the IOFBIntegrated A/B, the cursor state
 * a thread call applies, the family's wake work after a system sleep, the
 * detailed timings validateDetailedTiming accepts and the boot console.
 * Plain C, no kernel headers: fb/rdna3-fb.cpp uses it and
 * tests/test_fb_policy.c checks it on the host (DRIVER-PLAN N40).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_FB_POLICY_H
#define RDNA3_FB_POLICY_H

#include <stdint.h>

/* ---- open order (IOFramebufferOpenGLIndex) ----
 * IOFramebuffer::open() of the first framebuffer of a controller links all
 * of them (the ring follows gStartedFramebuffers, sorted by registry path)
 * and assignGLIndex() gives the opener GL index 0 and each next dependent
 * the next one. Card run 2026-10-17d: WindowServer opened head 1's
 * framebuffer first (its user client 0x...3d2 before head 0's 0x...3e0), so
 * head 1 got GL index 0 while its IOFBDependentIndex, AGDC slot and
 * IOAccelIndex are 1. Opening head 0 first (it then opens head 1 itself as
 * its dependent) makes the GL indices follow the dependent index.
 * Returns 1 when head 0's framebuffer must be opened before this one; *why
 * says what decided it (logged). */
enum rd3fb_order_why {
    RD3FB_ORDER_OFF = 0,          /* rdna3glorder=0 */
    RD3FB_ORDER_HEAD0 = 1,        /* this is head 0: nothing to do */
    RD3FB_ORDER_OPENED = 2,       /* this framebuffer is open already */
    RD3FB_ORDER_REENTERED = 3,    /* inside head 0's open of its dependents */
    RD3FB_ORDER_NO_HEAD0 = 4,     /* head 0's framebuffer not found or not started */
    RD3FB_ORDER_OTHER_CTRL = 5,   /* head 0's framebuffer has another IOFBDependentID */
    RD3FB_ORDER_HEAD0_OPEN = 6,   /* head 0 is open already: its open assigned the indices */
    RD3FB_ORDER_FIRST = 7         /* head 0 opens first (the change) */
};
static inline int rd3fb_open_head0_first(int enabled, unsigned head, int self_opened, int reentered,
                                         int head0_started, int same_dep_id, int head0_opened,
                                         enum rd3fb_order_why *why)
{
    enum rd3fb_order_why w;
    if (!enabled)
        w = RD3FB_ORDER_OFF;
    else if (head == 0)
        w = RD3FB_ORDER_HEAD0;
    else if (self_opened)
        w = RD3FB_ORDER_OPENED;
    else if (reentered)
        w = RD3FB_ORDER_REENTERED;
    else if (!head0_started)
        w = RD3FB_ORDER_NO_HEAD0;
    else if (!same_dep_id)
        w = RD3FB_ORDER_OTHER_CTRL;
    else if (head0_opened)
        w = RD3FB_ORDER_HEAD0_OPEN;
    else
        w = RD3FB_ORDER_FIRST;
    if (why)
        *why = w;
    return w == RD3FB_ORDER_FIRST;
}
static inline const char *rd3fb_order_text(enum rd3fb_order_why w)
{
    switch (w) {
    case RD3FB_ORDER_OFF:        return "off (rdna3glorder=0)";
    case RD3FB_ORDER_HEAD0:      return "head 0";
    case RD3FB_ORDER_OPENED:     return "already open";
    case RD3FB_ORDER_REENTERED:  return "opened by head 0 as its dependent";
    case RD3FB_ORDER_NO_HEAD0:   return "head 0's framebuffer not started: opened as asked";
    case RD3FB_ORDER_OTHER_CTRL: return "head 0's framebuffer is on another controller: opened as asked";
    case RD3FB_ORDER_HEAD0_OPEN: return "head 0 already open";
    case RD3FB_ORDER_FIRST:      return "head 0 opened first";
    }
    return "?";
}

/* ---- IOFBIntegrated A/B ----
 * IOFBController::init sets fIntegrated when the card's IOPCIDevice is on
 * PCI bus 0 and IOFramebuffer::open() publishes it as IOFBIntegrated. The
 * kernel side uses fIntegrated for nothing else of ours (IOFramebuffer::close
 * only with an NVIDIA device present), so this A/B changes only what readers
 * of the property (CoreDisplay, GPUWrangler if it reads it) see.
 * rdna3fbintegrated: 0 = as IOGraphics sets it, 1 = removed, 2 = set Yes. */
enum rd3fb_integ_act { RD3FB_INTEG_KEEP = 0, RD3FB_INTEG_REMOVE = 1, RD3FB_INTEG_SET = 2 };
static inline enum rd3fb_integ_act rd3fb_integrated_action(uint32_t mode, int present)
{
    if (mode == 1 && present)
        return RD3FB_INTEG_REMOVE;
    if (mode == 2 && !present)
        return RD3FB_INTEG_SET;
    return RD3FB_INTEG_KEEP;
}

/* ---- cursor state applied off WindowServer's thread ----
 * setCursorState runs on WindowServer's thread; the core's cursor call takes
 * its display lock, which flips, VBL status, EDID and DP link work hold too.
 * The framebuffer records the newest state and a THREAD_CALL_OPTIONS_ONCE
 * thread call applies it (NullMoth's design): positions that arrive while
 * one is applied fold into the next run, the newest wins. Caller holds a
 * lock around set/take/done. */
struct rd3fb_cur_state {
    int32_t x, y;
    uint32_t visible;      /* 0/1: shown (WindowServer's wish and an image loaded) */
};
struct rd3fb_cur_q {
    struct rd3fb_cur_state want, applied;
    uint32_t gen_want, gen_taken;  /* a new wish raises gen_want; the thread call takes up to it */
    uint32_t applied_valid;        /* 'applied' is what the hardware shows */
    uint32_t sets, same, kicks, takes, applies, coalesced, fails;
};
/* a new wish: 1 when the thread call must be entered (a change not taken yet
 * and no run already owed), 0 when it is the state shown or already queued */
static inline int rd3fb_cur_set(struct rd3fb_cur_q *q, int32_t x, int32_t y, uint32_t visible, int force)
{
    const int was_owed = q->gen_want != q->gen_taken;
    q->sets++;
    if (!force && q->want.x == x && q->want.y == y && q->want.visible == visible &&
        (was_owed || (q->applied_valid && q->applied.x == x && q->applied.y == y && q->applied.visible == visible))) {
        q->same++;
        return 0;
    }
    q->want.x = x;
    q->want.y = y;
    q->want.visible = visible;
    if (was_owed) {
        q->coalesced++;   /* folds into the run already owed */
        return 0;
    }
    q->gen_want++;
    q->kicks++;
    return 1;
}
/* the thread call: 1 and the newest wish in *out, 0 when nothing is owed */
static inline int rd3fb_cur_take(struct rd3fb_cur_q *q, struct rd3fb_cur_state *out)
{
    if (q->gen_want == q->gen_taken)
        return 0;
    q->gen_taken = q->gen_want;
    q->takes++;
    *out = q->want;
    return 1;
}
/* after the core's call for the state taken: 'ok' it took it */
static inline void rd3fb_cur_done(struct rd3fb_cur_q *q, const struct rd3fb_cur_state *s, int ok)
{
    if (ok) {
        q->applied = *s;
        q->applied_valid = 1;
        q->applies++;
    } else {
        q->applied_valid = 0;   /* unknown: the next wish is applied even if equal */
        q->fails++;
    }
}

/* ---- the family's wake work after a system sleep ----
 * IOGraphics' system sleep (IOFramebuffer::systemWork) sends every client
 * kIOFBNotifyWillSleep(false), then WillSleep(true), and clears pagingState.
 * Its wake work runs from IOFramebuffer::setPowerState, which this driver
 * overrides, and handleEvent() does nothing off the controller's power
 * thread: nobody sent DidWake, pagingState stayed clear and WindowServer
 * waits for it. The framebuffer does that work itself at power-on: DidWake
 * (true), then one DidWake(false) per WillSleep(false) since the last full
 * wake (IOGraphicsAccelerator2 counts each WillSleep(false) as a mode change
 * in progress; a sleep from a dark wake sends another one with no DidWake in
 * between), DidPowerOn(false), pagingState set and WindowServer woken.
 * The event values are IOFramebuffer's kIOFBNotifyWillSleep / DidWake.
 * Gates (ECC review): the work runs in the family's order, the system gate
 * (getGraphicsSystemWorkLoop) and then the controller's, as computeState and
 * systemWork deliver these events. WindowServer's system entries test
 * pagingState under both and sleep on fServerConnect in the system gate
 * (IOFramebuffer::_extEntry): holding that gate while paging is set and the
 * wakeup sent leaves no window between its test and its sleep; the other
 * entries sleep in the controller gate, which the work holds too
 * (tests/test_fb_policy.c models both orders). */
#define RD3FB_EV_WILLSLEEP 3
#define RD3FB_EV_DIDWAKE   4
/* a failed wake work (no gate, stopping) is tried again by a thread call,
 * this many times in a row, then given up (logged; the next power-on tries) */
#define RD3FB_WAKE_RETRIES 5
struct rd3fb_wake {
    int32_t will_sleep_off;   /* WillSleep(false) not yet answered by a DidWake(false) */
    uint32_t sys_slept;       /* a WillSleep(true) came: a system sleep since the last power-on */
    uint32_t sleeps, didwakes_seen, family_wakes, plain_power_ons, owed_max;
    uint32_t tap_events;      /* every WillSleep / DidWake the tap saw (0 with sleeps behind: a deaf tap) */
    uint32_t paging_already;  /* power-ons after a system sleep that found paging on: the sleep is cleared */
    uint32_t dropped;         /* WillSleep(false) dropped then (no stale DidWake(false) owed later) */
    uint32_t fails, fail_streak, retries, gave_up;   /* wake work that could not run */
};
/* every framebuffer notification (the framebuffer's own deliveries too) */
static inline void rd3fb_wake_event(struct rd3fb_wake *w, int event, int info)
{
    if (event == RD3FB_EV_WILLSLEEP) {
        w->tap_events++;
        if (info) {
            w->sys_slept = 1;
            w->sleeps++;
        } else {
            w->will_sleep_off++;
        }
    } else if (event == RD3FB_EV_DIDWAKE) {
        w->tap_events++;
        if (!info) {
            w->didwakes_seen++;
            if (w->will_sleep_off > 0)
                w->will_sleep_off--;
        }
    }
}
/* paging on after a system sleep: the family's wake (or someone's) is done,
 * the sleep and its WillSleep(false) count are history (else the next power-on
 * after a plain display sleep would send stale DidWakes) */
static inline void rd3fb_wake_paging_on(struct rd3fb_wake *w)
{
    w->paging_already++;
    w->dropped += w->will_sleep_off > 0 ? (uint32_t)w->will_sleep_off : 0u;
    w->will_sleep_off = 0;
    w->sys_slept = 0;
    w->fail_streak = 0;
}
/* the wake work still owed (inside the gates, and the retry): paging on
 * clears it (0), no system sleep recorded: 0, else the DidWake(false) count */
static inline int32_t rd3fb_wake_recheck(struct rd3fb_wake *w, int paging_state)
{
    int32_t owed;
    if (!w->sys_slept)
        return 0;
    if (paging_state) {
        rd3fb_wake_paging_on(w);
        return 0;
    }
    owed = w->will_sleep_off > 1 ? w->will_sleep_off : 1;
    if ((uint32_t)owed > w->owed_max)
        w->owed_max = (uint32_t)owed;
    return owed;
}
/* setPowerState(on): how many DidWake(false) the family wake work sends (at
 * least 1), or 0 = no system sleep to undo (or off, or paging already on):
 * a plain DidPowerOn as before. The caller runs the work in the family's
 * gates (rd3fb_wake_recheck there), then rd3fb_wake_done, or
 * rd3fb_wake_failed when it could not run. */
static inline int32_t rd3fb_wake_power_on(struct rd3fb_wake *w, int enabled, int paging_state)
{
    if (paging_state && w->sys_slept) {
        rd3fb_wake_paging_on(w);
        w->plain_power_ons++;
        return 0;
    }
    if (!enabled || paging_state || !w->sys_slept) {
        w->plain_power_ons++;
        return 0;
    }
    return rd3fb_wake_recheck(w, 0);
}
static inline void rd3fb_wake_done(struct rd3fb_wake *w)
{
    w->will_sleep_off = 0;
    w->sys_slept = 0;
    w->fail_streak = 0;
    w->family_wakes++;
}
/* the wake work could not run (the sleep stays recorded): 1 = try again
 * (a thread call a little later), 0 = given up until the next power-on */
static inline int rd3fb_wake_failed(struct rd3fb_wake *w)
{
    w->fails++;
    if (++w->fail_streak <= RD3FB_WAKE_RETRIES) {
        w->retries++;
        return 1;
    }
    w->gave_up++;
    w->fail_streak = 0;
    return 0;
}

/* ---- validateDetailedTiming ----
 * CoreDisplay's InstallTiming hands the framebuffer the EDID's timings
 * (card run 2026-10-17d: 57 x 0xe00002c7, IOFramebuffer's unsupported). A
 * timing is one of the framebuffer's modes when the actives and the totals
 * are equal and the pixel clock is within 0.5 %; a scaled timing (another
 * scaled size than the active one) never is. Returns the mode's index in
 * 'modes', -1 none. */
struct rd3fb_dt {
    uint64_t pclk;               /* Hz */
    uint32_t hact, hblank, vact, vblank;
    uint32_t hscaled, vscaled;   /* 0 = not scaled */
};
static inline int rd3fb_dt_match(const struct rd3fb_dt *want, const struct rd3fb_dt *modes, unsigned n)
{
    unsigned i;
    if (!want || !want->pclk || !want->hact || !want->vact)
        return -1;
    if ((want->hscaled && want->hscaled != want->hact) || (want->vscaled && want->vscaled != want->vact))
        return -1;
    for (i = 0; i < n; i++) {
        const struct rd3fb_dt *m = &modes[i];
        uint64_t d;
        if (!m->pclk || m->hact != want->hact || m->vact != want->vact || m->hblank != want->hblank ||
            m->vblank != want->vblank)
            continue;
        d = m->pclk > want->pclk ? m->pclk - want->pclk : want->pclk - m->pclk;
        if (d * 200 <= m->pclk)
            return (int)i;
    }
    return -1;
}

/* validateDetailedTiming's answer: rdna3fbdtmap=0 answers what the base
 * class does for every shape (Unsupported, ECC review: a malformed size
 * answered BadArgument there); 1 refuses a malformed description with
 * BadArgument and a timing not ours with UnsupportedMode. Every answer but
 * ACCEPT counts as refused (dt_refused). */
enum rd3fb_dt_ans { RD3FB_DT_ACCEPT = 0, RD3FB_DT_UNSUPPORTED = 1, RD3FB_DT_UNSUPPORTED_MODE = 2, RD3FB_DT_BADARG = 3 };
static inline enum rd3fb_dt_ans rd3fb_dt_answer(uint32_t dtmap, int is_timing, int match)
{
    if (!dtmap)
        return RD3FB_DT_UNSUPPORTED;
    if (!is_timing)
        return RD3FB_DT_BADARG;
    return match >= 0 ? RD3FB_DT_ACCEPT : RD3FB_DT_UNSUPPORTED_MODE;
}

/* ---- isConsoleDevice ----
 * The framebuffer owns the boot console when the console's base (the flag
 * bits IOGraphics sets in its low bits cleared) is inside its scanout at
 * start. Console info unavailable: head 0 (the answer before). */
static inline int rd3fb_owns_console(int console_known, uint64_t console_base, uint64_t phys, uint64_t bytes,
                                     unsigned head)
{
    uint64_t b = console_base & ~(uint64_t)3;
    if (!console_known || !b)
        return head == 0;
    return phys && bytes && b >= phys && b - phys < bytes;
}

#endif
