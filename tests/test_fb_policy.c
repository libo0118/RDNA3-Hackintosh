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
/* the driver's family wake work: what it delivers passes its own tap too */
static int32_t power_on(struct rd3fb_wake *w, int enabled, int paging)
{
    int32_t owed = rd3fb_wake_power_on(w, enabled, paging), i;
    if (!owed)
        return 0;
    rd3fb_wake_event(w, RD3FB_EV_DIDWAKE, 1);
    for (i = 0; i < owed; i++)
        rd3fb_wake_event(w, RD3FB_EV_DIDWAKE, 0);
    rd3fb_wake_done(w);
    return owed;
}

static void test_wake(void)
{
    struct rd3fb_wake w;
    memset(&w, 0, sizeof w);
    /* boot: power on with paging clear and no sleep seen: no family wake (it would send a DidWake at boot) */
    CHECK(power_on(&w, 1, 0) == 0 && w.plain_power_ons == 1);
    /* display sleep only (no WillSleep): plain DidPowerOn */
    CHECK(power_on(&w, 1, 1) == 0);
    /* one system sleep: DidWake(true) + one DidWake(false) */
    sys_sleep(&w);
    CHECK(power_on(&w, 1, 0) == 1 && w.family_wakes == 1 && w.will_sleep_off == 0 && !w.sys_slept);
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
    /* paging already on (the family did its work): nothing */
    sys_sleep(&w);
    CHECK(power_on(&w, 1, 1) == 0);
    /* off (rdna3fbwake=0): never; the sleep stays recorded for a later power-on with it on */
    CHECK(power_on(&w, 0, 0) == 0 && w.sys_slept);
    CHECK(power_on(&w, 1, 0) == 1);
    /* a mux-style sleep (WillSleep(true) only) still wakes once */
    rd3fb_wake_event(&w, RD3FB_EV_WILLSLEEP, 1);
    CHECK(power_on(&w, 1, 0) == 1);
    CHECK(w.sleeps == 5);
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
    test_timing();
    test_console();
    if (fails) {
        printf("test_fb_policy: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_fb_policy: all checks passed\n");
    return 0;
}
