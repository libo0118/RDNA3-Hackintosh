/*
 * test_fb_policy.c - RDNA3Framebuffer's IOKit-free decisions
 * (common/rdna3_fb_policy.h, DRIVER-PLAN N40): the open order that makes
 * IOFramebufferOpenGLIndex follow IOFBDependentIndex (card run 2026-10-17d:
 * 0,1 against 1,0), the IOFBIntegrated A/B, the cursor state the thread call
 * applies, the WillSleep/DidWake counting of the family's wake work, the
 * timings validateDetailedTiming accepts and the boot console.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "rdna3_fb_policy.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* IOFramebuffer's open as far as the GL indices go (IOFramebuffer.cpp,
 * IOGraphics: open -> openAllDependents -> assignGLIndex -> next->open):
 * the ring follows the start order sorted by path (head 1's framebuffer,
 * under RDNA3Accel/RDNA3DisplayHead, sorts before head 0's under the PCI
 * device), the first framebuffer opened gets index 0 and the ring's next
 * ones the following indices. With 'order' the driver's open() asks
 * rd3fb_open_head0_first first. */
struct fbm { unsigned head; int opened; int gl; };
static struct fbm fbs[2];
static const unsigned ring[2] = { 1, 0 };   /* path order: head 1's, then head 0's */
static int ctrl_opened, reenter, order_on;

static unsigned ring_pos(unsigned head) { return ring[0] == head ? 0u : 1u; }

static void family_open(unsigned head);
static void driver_open(unsigned head)
{
    enum rd3fb_order_why why;
    if (rd3fb_open_head0_first(order_on, head, fbs[head].opened, reenter, 1, 1, fbs[0].opened, &why)) {
        reenter++;
        driver_open(0);
        reenter--;
    }
    family_open(head);
}
static void family_open(unsigned head)
{
    unsigned i, p;
    if (fbs[head].opened)
        return;
    fbs[head].opened = 1;
    if (!ctrl_opened) {   /* the controller's first open: assignGLIndex, then the dependents */
        ctrl_opened = 1;
        p = ring_pos(head);
        for (i = 0; i < 2; i++)
            fbs[ring[(p + i) % 2]].gl = (int)i;
        for (i = 1; i < 2; i++)
            driver_open(ring[(p + i) % 2]);   /* next->open() is the subclass's open */
    }
}
static void open_run(int order, unsigned ws_first)
{
    memset(fbs, 0, sizeof fbs);
    fbs[0].head = 0;
    fbs[1].head = 1;
    ctrl_opened = 0;
    reenter = 0;
    order_on = order;
    driver_open(ws_first);
    driver_open(1u - ws_first);
}

static void test_open_order(void)
{
    enum rd3fb_order_why why;
    /* card run 2026-10-17d: WindowServer opens head 1 first, no reorder: 0,1 against dependent 1,0 */
    open_run(0, 1);
    CHECK(fbs[1].gl == 0 && fbs[0].gl == 1);
    /* the change: head 0 opens first, each GL index is the dependent index */
    open_run(1, 1);
    CHECK(fbs[0].gl == 0 && fbs[1].gl == 1);
    CHECK(fbs[0].opened && fbs[1].opened);
    /* WindowServer opening head 0 first: the same either way */
    open_run(1, 0);
    CHECK(fbs[0].gl == 0 && fbs[1].gl == 1);
    open_run(0, 0);
    CHECK(fbs[0].gl == 0 && fbs[1].gl == 1);
    /* every reason */
    CHECK(!rd3fb_open_head0_first(0, 1, 0, 0, 1, 1, 0, &why) && why == RD3FB_ORDER_OFF);
    CHECK(!rd3fb_open_head0_first(1, 0, 0, 0, 1, 1, 0, &why) && why == RD3FB_ORDER_HEAD0);
    CHECK(!rd3fb_open_head0_first(1, 1, 1, 0, 1, 1, 0, &why) && why == RD3FB_ORDER_OPENED);
    CHECK(!rd3fb_open_head0_first(1, 1, 0, 1, 1, 1, 0, &why) && why == RD3FB_ORDER_REENTERED);
    CHECK(!rd3fb_open_head0_first(1, 1, 0, 0, 0, 1, 0, &why) && why == RD3FB_ORDER_NO_HEAD0);
    CHECK(!rd3fb_open_head0_first(1, 1, 0, 0, 1, 0, 0, &why) && why == RD3FB_ORDER_OTHER_CTRL);
    CHECK(!rd3fb_open_head0_first(1, 1, 0, 0, 1, 1, 1, &why) && why == RD3FB_ORDER_HEAD0_OPEN);
    CHECK(rd3fb_open_head0_first(1, 2, 0, 0, 1, 1, 0, &why) && why == RD3FB_ORDER_FIRST);
    CHECK(strcmp(rd3fb_order_text(RD3FB_ORDER_FIRST), "head 0 opened first") == 0);
    CHECK(rd3fb_open_head0_first(1, 1, 0, 0, 1, 1, 0, NULL) == 1);
}

static void test_integrated(void)
{
    CHECK(rd3fb_integrated_action(0, 1) == RD3FB_INTEG_KEEP);
    CHECK(rd3fb_integrated_action(0, 0) == RD3FB_INTEG_KEEP);
    CHECK(rd3fb_integrated_action(1, 1) == RD3FB_INTEG_REMOVE);
    CHECK(rd3fb_integrated_action(1, 0) == RD3FB_INTEG_KEEP);
    CHECK(rd3fb_integrated_action(2, 0) == RD3FB_INTEG_SET);
    CHECK(rd3fb_integrated_action(2, 1) == RD3FB_INTEG_KEEP);
    CHECK(rd3fb_integrated_action(7, 1) == RD3FB_INTEG_KEEP);
}

static void test_cursor(void)
{
    struct rd3fb_cur_q q;
    struct rd3fb_cur_state s;
    int i, kicks = 0;
    memset(&q, 0, sizeof q);
    /* the first state kicks the thread call */
    CHECK(rd3fb_cur_set(&q, 10, 20, 1, 0) == 1);
    /* 100 more positions while that run is owed: none kicks, the newest is taken */
    for (i = 0; i < 100; i++)
        kicks += rd3fb_cur_set(&q, 11 + i, 20, 1, 0);
    CHECK(kicks == 0 && q.coalesced == 100);
    CHECK(rd3fb_cur_take(&q, &s) == 1 && s.x == 110 && s.y == 20 && s.visible == 1);
    CHECK(rd3fb_cur_take(&q, &s) == 0);   /* nothing more owed */
    rd3fb_cur_done(&q, &s, 1);
    CHECK(q.applies == 1 && q.applied.x == 110);
    /* the state on the screen again: no kick */
    CHECK(rd3fb_cur_set(&q, 110, 20, 1, 0) == 0 && q.same == 1);
    /* a wish while the thread call runs (taken, not done): kicks a rerun (ONCE folds it into one more run) */
    CHECK(rd3fb_cur_set(&q, 200, 30, 1, 0) == 1);
    CHECK(rd3fb_cur_take(&q, &s) == 1 && s.x == 200);
    CHECK(rd3fb_cur_set(&q, 201, 30, 1, 0) == 1);
    rd3fb_cur_done(&q, &s, 1);
    CHECK(rd3fb_cur_take(&q, &s) == 1 && s.x == 201);
    /* a failed apply: the same wish again is applied again */
    rd3fb_cur_done(&q, &s, 0);
    CHECK(q.fails == 1 && !q.applied_valid);
    CHECK(rd3fb_cur_set(&q, 201, 30, 1, 0) == 1);
    CHECK(rd3fb_cur_take(&q, &s) == 1);
    rd3fb_cur_done(&q, &s, 1);
    /* hide, then a forced re-apply of the same state (the image changed) */
    CHECK(rd3fb_cur_set(&q, 201, 30, 0, 0) == 1);
    CHECK(rd3fb_cur_take(&q, &s) == 1 && s.visible == 0);
    rd3fb_cur_done(&q, &s, 1);
    CHECK(rd3fb_cur_set(&q, 201, 30, 0, 1) == 1);
    /* no position is lost: every take had the newest wish of its time */
    CHECK(q.kicks + q.coalesced + q.same == q.sets);
}

/* IOGraphics' system sleep for one framebuffer, as the tap sees it */
static void sys_sleep(struct rd3fb_wake *w)
{
    rd3fb_wake_event(w, RD3FB_EV_WILLSLEEP, 0);
    rd3fb_wake_event(w, RD3FB_EV_WILLSLEEP, 1);
}
/* the driver's family wake work: what it delivers passes its own tap too;
 * inside the gates it looks again (rd3fb_wake_recheck) */
static int32_t power_on(struct rd3fb_wake *w, int enabled, int paging)
{
    int32_t owed = rd3fb_wake_power_on(w, enabled, paging), i;
    if (!owed)
        return 0;
    owed = rd3fb_wake_recheck(w, paging);
    rd3fb_wake_event(w, RD3FB_EV_DIDWAKE, 1);
    for (i = 0; i < owed; i++)
        rd3fb_wake_event(w, RD3FB_EV_DIDWAKE, 0);
    rd3fb_wake_done(w);
    return owed;
}

static void test_wake(void)
{
    struct rd3fb_wake w;
    int i;
    memset(&w, 0, sizeof w);
    /* boot: power on with paging clear and no sleep seen: no family wake (it would send a DidWake at boot) */
    CHECK(power_on(&w, 1, 0) == 0 && w.plain_power_ons == 1);
    /* display sleep only (no WillSleep): plain DidPowerOn */
    CHECK(power_on(&w, 1, 1) == 0);
    CHECK(w.tap_events == 0);
    /* one system sleep: DidWake(true) + one DidWake(false) */
    sys_sleep(&w);
    CHECK(power_on(&w, 1, 0) == 1 && w.family_wakes == 1 && w.will_sleep_off == 0 && !w.sys_slept);
    CHECK(w.tap_events == 4);   /* two WillSleep, our DidWake(true) and DidWake(false) */
    /* four maintenance dark wakes: four WillSleep(false), no DidWake between: four owed */
    sys_sleep(&w);
    rd3fb_wake_event(&w, RD3FB_EV_WILLSLEEP, 0);
    rd3fb_wake_event(&w, RD3FB_EV_WILLSLEEP, 0);
    rd3fb_wake_event(&w, RD3FB_EV_WILLSLEEP, 0);
    CHECK(w.will_sleep_off == 4);
    CHECK(power_on(&w, 1, 0) == 4 && w.owed_max == 4 && w.will_sleep_off == 0);
    /* a DidWake(false) someone else sent pays one back */
    sys_sleep(&w);
    rd3fb_wake_event(&w, RD3FB_EV_WILLSLEEP, 0);
    rd3fb_wake_event(&w, RD3FB_EV_DIDWAKE, 0);
    CHECK(w.will_sleep_off == 1);
    CHECK(power_on(&w, 1, 0) == 1);
    /* paging already on at the first power-on after a sleep (the family did its work): nothing, and the sleep
     * and its WillSleep(false) count are cleared - no stale owed count for a later power-on (ECC review) */
    sys_sleep(&w);
    rd3fb_wake_event(&w, RD3FB_EV_WILLSLEEP, 0);
    CHECK(power_on(&w, 1, 1) == 0);
    CHECK(!w.sys_slept && w.will_sleep_off == 0 && w.paging_already == 1 && w.dropped == 2);
    /* the display sleeps and wakes later with paging cleared by someone: no stale family wake */
    CHECK(power_on(&w, 1, 0) == 0);
    /* paging on with the switch off clears it too */
    sys_sleep(&w);
    CHECK(power_on(&w, 0, 1) == 0 && !w.sys_slept && w.paging_already == 2);
    /* off (rdna3fbwake=0): never; the sleep stays recorded for a later power-on with it on */
    sys_sleep(&w);
    CHECK(power_on(&w, 0, 0) == 0 && w.sys_slept);
    CHECK(power_on(&w, 1, 0) == 1);
    /* a mux-style sleep (WillSleep(true) only) still wakes once */
    rd3fb_wake_event(&w, RD3FB_EV_WILLSLEEP, 1);
    CHECK(power_on(&w, 1, 0) == 1);
    CHECK(w.sleeps == 7);
    /* inside the gates: paging went on between the power-on's look and the gates: nothing sent, cleared */
    sys_sleep(&w);
    CHECK(rd3fb_wake_power_on(&w, 1, 0) == 1);
    CHECK(rd3fb_wake_recheck(&w, 1) == 0 && !w.sys_slept);
    CHECK(rd3fb_wake_recheck(&w, 0) == 0);   /* nothing recorded: nothing owed */
    /* the work could not run: retried RD3FB_WAKE_RETRIES times, the sleep stays recorded, then given up */
    sys_sleep(&w);
    CHECK(rd3fb_wake_power_on(&w, 1, 0) == 1);
    for (i = 0; i < RD3FB_WAKE_RETRIES; i++)
        CHECK(rd3fb_wake_failed(&w) == 1);
    CHECK(rd3fb_wake_failed(&w) == 0 && w.gave_up == 1 && w.sys_slept);
    CHECK(w.fails == RD3FB_WAKE_RETRIES + 1 && w.retries == RD3FB_WAKE_RETRIES);
    /* the next power-on tries again, a success resets the streak */
    CHECK(power_on(&w, 1, 0) == 1 && w.fail_streak == 0 && !w.sys_slept);
    CHECK(rd3fb_wake_failed(&w) == 1 && w.fail_streak == 1);
    /* no command gate: given up at once, the streak cleared, no retry counted, the sleep kept */
    sys_sleep(&w);
    CHECK(rd3fb_wake_power_on(&w, 1, 0) == 1);
    {
        const uint32_t retries = w.retries, gave = w.gave_up, f = w.fails;
        rd3fb_wake_give_up(&w);
        CHECK(w.retries == retries && w.gave_up == gave + 1 && w.fails == f + 1 && w.fail_streak == 0);
        CHECK(w.sys_slept);
    }
}

/* The gate order of the wake work against WindowServer's wait for paging
 * (IOFramebuffer::_extEntry): a system entry takes the system gate, then the
 * controller's, tests pagingState, opens the controller gate (FBUNLOCK) and
 * sleeps on fServerConnect in the system gate (sleepGate releases it and
 * sleeps at once); a controller entry sleeps in the controller gate. The
 * wake work sets pagingState and wakes fServerConnect (not gate specific)
 * holding the controller gate only (the first version) or the system gate
 * and then the controller's (the family's order, computeState). Every
 * interleaving is explored; a lost wakeup is the sleeper asleep with the
 * wake work done. */
struct gm { int sys, ctl, paging, asleep, spc, wpc, sys_entry, waker_sys; };
#define GM_DONE 99
static int gm_sleeper(struct gm *g)
{
    switch (g->spc) {
    case 0:   /* a system entry: the system gate first */
        if (g->sys_entry) {
            if (g->sys)
                return 0;
            g->sys = 1;
        }
        g->spc = 1;
        return 1;
    case 1:
        if (g->ctl)
            return 0;
        g->ctl = 1;
        g->spc = 2;
        return 1;
    case 2:   /* the test */
        g->ctl = 0;
        if (g->paging) {
            if (g->sys_entry)
                g->sys = 0;
            g->spc = GM_DONE;
        } else if (g->sys_entry) {
            g->spc = 3;   /* FBUNLOCK done; sleepGate on the system gate next */
        } else {
            g->asleep = 1;   /* sleepGate on the controller gate: released and asleep at once */
            g->spc = 4;
        }
        return 1;
    case 3:
        g->sys = 0;
        g->asleep = 1;
        g->spc = 4;
        return 1;
    case 4:   /* woken: the gates again, the test again */
        if (g->asleep)
            return 0;
        g->spc = 0;
        return 1;
    }
    return 0;
}
static int gm_waker(struct gm *g)
{
    switch (g->wpc) {
    case 0:
        if (g->waker_sys) {
            if (g->sys)
                return 0;
            g->sys = 2;
        }
        g->wpc = 1;
        return 1;
    case 1:
        if (g->ctl)
            return 0;
        g->ctl = 2;
        g->wpc = 2;
        return 1;
    case 2:
        g->paging = 1;
        g->wpc = 3;
        return 1;
    case 3:
        g->asleep = 0;   /* wakeupGate(&fServerConnect): whoever sleeps on it */
        g->wpc = 4;
        return 1;
    case 4:
        g->ctl = 0;
        if (g->waker_sys)
            g->sys = 0;
        g->wpc = 5;
        return 1;
    }
    return 0;
}
static unsigned gm_lost, gm_ends;
static void gm_explore(struct gm g)
{
    struct gm a = g, b = g;
    const int ps = gm_sleeper(&a), pw = gm_waker(&b);
    if (ps)
        gm_explore(a);
    if (pw)
        gm_explore(b);
    if (!ps && !pw) {
        gm_ends++;
        if (g.spc != GM_DONE)
            gm_lost++;
    }
}
static unsigned gm_run(int sys_entry, int waker_sys)
{
    struct gm g;
    memset(&g, 0, sizeof g);
    g.sys_entry = sys_entry;
    g.waker_sys = waker_sys;
    gm_lost = gm_ends = 0;
    gm_explore(g);
    CHECK(gm_ends > 0);
    return gm_lost;
}
static void test_wake_gates(void)
{
    /* the controller gate alone: a system entry between its FBUNLOCK and its sleep misses the wakeup */
    CHECK(gm_run(1, 0) > 0);
    /* the family's order: no interleaving loses it, for either kind of entry */
    CHECK(gm_run(1, 1) == 0);
    CHECK(gm_run(0, 1) == 0);
    CHECK(gm_run(0, 0) == 0);
}

/* stop() against a wake work in flight (ECC review round 2). The wake work
 * W (a setPowerState callout): enters, tests 'stopping' and counts itself
 * in flight, takes the system gate (runAction), gets NotReady from the
 * removed command gate, opens the system gate, leaves. stop() S: sets
 * 'stopping' (under the same lock), waits for the in-flight count to reach
 * 0 (the fix), then IOFramebuffer::stop: the system gate, PMstop (waits for
 * a callout that started and has not returned; none starts after it), the
 * gate opened. A deadlock is both blocked before the end. 'mode' 0 = no
 * wait, 1 = the wait with the test and the count as two steps, 2 = the wait
 * with both under the lock (rd3fb_wake_enter). */
struct sm { int stopping, sys, pm_stopped, wpc, spc, mode; int32_t inflight; };
#define SM_DONE 99
static int sm_w(struct sm *m)
{
    switch (m->wpc) {
    case 0:   /* the callout starts */
        if (m->pm_stopped)
            return 0;
        m->wpc = 1;
        return 1;
    case 1:
        if (m->mode == 1) {   /* the test alone; the count a step later */
            if (m->stopping) {
                m->wpc = SM_DONE;
            } else {
                m->wpc = 5;
            }
            return 1;
        }
        m->wpc = rd3fb_wake_enter(&m->inflight, m->stopping) ? 2 : SM_DONE;
        return 1;
    case 5:
        m->inflight++;
        m->wpc = 2;
        return 1;
    case 2:   /* getGraphicsSystemWorkLoop()->runAction */
        if (m->sys)
            return 0;
        m->sys = 1;
        m->wpc = 3;
        return 1;
    case 3:   /* the removed command gate answers NotReady; the system gate opens */
        m->sys = 0;
        m->wpc = 4;
        return 1;
    case 4:
        rd3fb_wake_leave(&m->inflight);
        m->wpc = SM_DONE;
        return 1;
    }
    return 0;
}
static int sm_s(struct sm *m)
{
    switch (m->spc) {
    case 0:
        m->stopping = 1;
        m->spc = m->mode ? 1 : 2;
        return 1;
    case 1:   /* the wait, no gate held */
        if (rd3fb_stop_wake_wait(m->inflight, 0) != 1)
            return 0;
        m->spc = 2;
        return 1;
    case 2:   /* IOFramebuffer::stop: SYSGATEGUARD */
        if (m->sys)
            return 0;
        m->sys = 2;
        m->spc = 3;
        return 1;
    case 3:   /* PMstop: waits for a callout in progress */
        if (m->wpc != 0 && m->wpc != SM_DONE)
            return 0;
        m->pm_stopped = 1;
        m->spc = 4;
        return 1;
    case 4:
        m->sys = 0;
        m->spc = SM_DONE;
        return 1;
    }
    return 0;
}
static unsigned sm_dead, sm_ends;
static void sm_explore(struct sm m)
{
    struct sm a = m, b = m;
    const int pw = sm_w(&a), ps = sm_s(&b);
    if (pw)
        sm_explore(a);
    if (ps)
        sm_explore(b);
    if (!pw && !ps) {
        sm_ends++;
        if (m.spc != SM_DONE || (m.wpc != SM_DONE && !(m.wpc == 0 && m.pm_stopped)))
            sm_dead++;
        else if (m.inflight != 0)
            sm_dead++;   /* a count left behind would make the next stop wait in vain */
    }
}
static unsigned sm_run(int mode)
{
    struct sm m;
    memset(&m, 0, sizeof m);
    m.mode = mode;
    sm_dead = sm_ends = 0;
    sm_explore(m);
    CHECK(sm_ends > 0);
    return sm_dead;
}
static void test_stop_wake(void)
{
    int32_t n = 0;
    /* the in-flight count: no entry while stopping, every leave drops it, never below 0 */
    CHECK(rd3fb_wake_enter(&n, 1) == 0 && n == 0);
    CHECK(rd3fb_wake_enter(&n, 0) == 1 && n == 1);
    CHECK(rd3fb_wake_enter(&n, 0) == 1 && n == 2);
    rd3fb_wake_leave(&n);
    rd3fb_wake_leave(&n);
    CHECK(n == 0);
    rd3fb_wake_leave(&n);
    CHECK(n == 0);
    /* the wait: done at 0, bounded otherwise */
    CHECK(rd3fb_stop_wake_wait(0, 0) == 1);
    CHECK(rd3fb_stop_wake_wait(0, RD3FB_STOP_WAKE_WAIT_MS * 2) == 1);
    CHECK(rd3fb_stop_wake_wait(1, 0) == 0);
    CHECK(rd3fb_stop_wake_wait(1, RD3FB_STOP_WAKE_WAIT_MS - RD3FB_STOP_WAKE_STEP_MS) == 0);
    CHECK(rd3fb_stop_wake_wait(1, RD3FB_STOP_WAKE_WAIT_MS) == -1);
    /* no wait: stop holds the system gate in PMstop while the callout waits for it */
    CHECK(sm_run(0) > 0);
    /* a wait whose count is not taken with the stopping test still deadlocks */
    CHECK(sm_run(1) > 0);
    /* the count taken with the test under the lock: no interleaving deadlocks */
    CHECK(sm_run(2) == 0);
}

static void test_timing(void)
{
    /* the native 1920x1080@60 (148.5 MHz, 2200x1125) and 1280x720@60 (74.25 MHz, 1650x750) */
    const struct rd3fb_dt modes[2] = {
        { 148500000ull, 1920, 280, 1080, 45, 0, 0 },
        { 74250000ull, 1280, 370, 720, 30, 0, 0 },
    };
    struct rd3fb_dt t = modes[1];
    CHECK(rd3fb_dt_match(&modes[0], modes, 2) == 0);
    CHECK(rd3fb_dt_match(&t, modes, 2) == 1);
    t.pclk = 74175824ull;   /* 59.94 Hz: within 0.5 % */
    CHECK(rd3fb_dt_match(&t, modes, 2) == 1);
    t.pclk = 61875000ull;   /* 1280x720@50 (1980 total): another blanking */
    t.hblank = 700;
    CHECK(rd3fb_dt_match(&t, modes, 2) == -1);
    /* 1920x1080@50: same actives, another horizontal blanking (2640 total) */
    t = modes[0];
    t.hblank = 720;
    CHECK(rd3fb_dt_match(&t, modes, 2) == -1);
    /* 720x480, 720x576: none */
    t.hact = 720; t.vact = 480; t.hblank = 138; t.vblank = 45; t.pclk = 27000000ull;
    CHECK(rd3fb_dt_match(&t, modes, 2) == -1);
    /* a scaled timing is never one of ours; scaled to its own size is */
    t = modes[0];
    t.hscaled = 1280; t.vscaled = 720;
    CHECK(rd3fb_dt_match(&t, modes, 2) == -1);
    t.hscaled = 1920; t.vscaled = 1080;
    CHECK(rd3fb_dt_match(&t, modes, 2) == 0);
    /* nothing, no clock */
    CHECK(rd3fb_dt_match(NULL, modes, 2) == -1);
    t.pclk = 0;
    CHECK(rd3fb_dt_match(&t, modes, 2) == -1);
    CHECK(rd3fb_dt_match(&modes[0], modes, 0) == -1);
    /* the answer: rdna3fbdtmap=0 is the base class's Unsupported for every shape (ECC review) */
    CHECK(rd3fb_dt_answer(0, 0, -1) == RD3FB_DT_UNSUPPORTED);
    CHECK(rd3fb_dt_answer(0, 1, -1) == RD3FB_DT_UNSUPPORTED);
    CHECK(rd3fb_dt_answer(0, 1, 0) == RD3FB_DT_UNSUPPORTED);
    CHECK(rd3fb_dt_answer(1, 0, -1) == RD3FB_DT_BADARG);
    CHECK(rd3fb_dt_answer(1, 1, -1) == RD3FB_DT_UNSUPPORTED_MODE);
    CHECK(rd3fb_dt_answer(1, 1, 2) == RD3FB_DT_ACCEPT);
}

static void test_console(void)
{
    /* the console at the boot scanout (flag bit 0 set, card run 2026-10-12) */
    CHECK(rd3fb_owns_console(1, 0x80000001ull, 0x80000000ull, 1920ull * 4 * 1080, 0) == 1);
    /* head 1's own surface elsewhere in the BAR */
    CHECK(rd3fb_owns_console(1, 0x80000001ull, 0x87040000ull, 1920ull * 4 * 1080, 1) == 0);
    /* head 0 whose scanout is not where the console is: not the console */
    CHECK(rd3fb_owns_console(1, 0x90000000ull, 0x80000000ull, 1920ull * 4 * 1080, 0) == 0);
    /* the end of the range */
    CHECK(rd3fb_owns_console(1, 0x80000000ull + 1920ull * 4 * 1080, 0x80000000ull, 1920ull * 4 * 1080, 0) == 0);
    /* no console info: head 0 as before */
    CHECK(rd3fb_owns_console(0, 0, 0x80000000ull, 4096, 0) == 1);
    CHECK(rd3fb_owns_console(0, 0, 0x80000000ull, 4096, 1) == 0);
}

int main(void)
{
    test_open_order();
    test_integrated();
    test_cursor();
    test_wake();
    test_wake_gates();
    test_stop_wake();
    test_timing();
    test_console();
    if (fails) {
        printf("test_fb_policy: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_fb_policy: all checks passed\n");
    return 0;
}
