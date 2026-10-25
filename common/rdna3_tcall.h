/*
 * rdna3_tcall.h - a thread call that can be stopped with public KPIs only
 * (card run 2026-10-25b, DRIVER-PLAN N47).
 *
 * macOS 26 (Darwin 25) exports thread_call_cancel_wait only from
 * com.apple.kpi.private: kmutil refused RDNA3Framebuffer and
 * RDNA3Accelerator ("Failed to bind '_thread_call_cancel_wait' ... could
 * not find a kext which exports this symbol"; checked with `kmutil
 * libraries -p <kext> --undef-symbols` on 26.4.1). The core only linked
 * because OpenCore's prelinker does not enforce the KPI exports. Everything
 * here uses com.apple.kpi.mach (thread_call_allocate[_with_options],
 * thread_call_enter[_delayed], thread_call_cancel, thread_call_free,
 * current_thread) and com.apple.kpi.iokit (IOMalloc, IOFree, IOSleep, IOLog).
 *
 * How "wait for the run still going" works without thread_call_cancel_wait:
 * the thread call's function is a trampoline around the caller's, and a
 * counter, 'outstanding', holds every run that was armed (queued) and has
 * not yet finished or been cancelled:
 *   arm      outstanding += 1 before thread_call_enter*; when the enter
 *            returns TRUE (merged into a run already pending, or for a ONCE
 *            call a re-run already scheduled), -= 1 again;
 *   run      the trampoline calls the function, then -= 1 as its very last
 *            access to the state;
 *   cancel   thread_call_cancel returning TRUE removed one pending run: -= 1.
 * Counting at arm time, not at entry, closes the window in which a run was
 * dequeued but has not yet reached the trampoline (thread_call_cancel finds
 * nothing queued, a counter bumped at entry would still be 0). The count is
 * never below the real number of runs: a merged enter's -= 1 comes after
 * its += 1. A drain cancels and polls (IOSleep) until 'outstanding' is 0,
 * cancelling again on every look, so a run that re-arms itself while it is
 * waited for is cancelled like thread_call_cancel_wait's callers' loops did.
 * The call sites keep their own stop flags (set before the drain, so a run
 * that sees one re-arms nothing); rd3_tcall_free additionally refuses every
 * arm from then on (the Dekker pair: an arm bumps the count, then reads
 * 'stopping'; the free sets it, then reads the count; a refused arm keeps
 * its count until its last access, so the free cannot release the state
 * under it).
 *
 * No bound, as thread_call_cancel_wait: a stop returns only when no run of
 * the call is left, because the callers free what the function uses right
 * after it (ECC review round 1: a give-up turned a hang into a
 * use-after-free). A stop still waiting after RD3_TCALL_SLOW_MS is logged
 * and counted (slow), then logged again every RD3_TCALL_REPORT_MS with the
 * call's name, the time waited and the runs outstanding, so a hang names
 * its thread call in the kernel log. A drain from inside the call's own
 * run (thread_call_cancel_wait panics there) waits for the other runs only,
 * logged; a free from inside its own run is refused, logged as a FAIL (no
 * call site does that; freeing the call under its running trampoline is not
 * supported).
 *
 * The state machine below is portable C99 (tests/test_tcall.c drives it
 * against a model of xnu's thread calls); the kernel part is under KERNEL.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_TCALL_H
#define RDNA3_TCALL_H

#include <stdint.h>

#define RD3_TCALL_SLOW_MS    2000u    /* a stop still waiting: logged, counted */
#define RD3_TCALL_REPORT_MS  30000u   /* ... and logged again this often while it waits */
#define RD3_TCALL_NAP_FAST   50u      /* the first naps are 1 ms, then 10 ms */

struct rd3_tcall_state {
    int32_t  outstanding;   /* armed runs not yet finished or cancelled */
    int32_t  stopping;      /* rd3_tcall_free: every arm refused */
    uint32_t armed, merged, refused, runs, cancelled;
    uint32_t drains, slow, self_drains;
};

/* an arm: 1 = go on with thread_call_enter*; 0 = refused (stopping), and
 * the caller still holds a count that keeps the state alive (a free in
 * progress waits for it): it may log, then must call rd3_tcall_st_unhold.
 * The count comes first, so no arm ever touches the state without one. */
static inline int rd3_tcall_st_arm(struct rd3_tcall_state *s)
{
    __atomic_add_fetch(&s->outstanding, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s->stopping, __ATOMIC_SEQ_CST)) {
        __atomic_add_fetch(&s->refused, 1, __ATOMIC_SEQ_CST);
        return 0;
    }
    __atomic_add_fetch(&s->armed, 1, __ATOMIC_RELAXED);
    return 1;
}

/* a refused arm's last access to the state */
static inline void rd3_tcall_st_unhold(struct rd3_tcall_state *s)
{
    __atomic_sub_fetch(&s->outstanding, 1, __ATOMIC_SEQ_CST);
}

/* thread_call_enter*'s result for an arm that went on: TRUE = merged into
 * a run already pending (that run is the one counted); the counter goes
 * last */
static inline void rd3_tcall_st_entered(struct rd3_tcall_state *s, int was_pending)
{
    if (!was_pending)
        return;
    __atomic_add_fetch(&s->merged, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&s->outstanding, 1, __ATOMIC_SEQ_CST);
}

static inline void rd3_tcall_st_run_begin(struct rd3_tcall_state *s)
{
    __atomic_add_fetch(&s->runs, 1, __ATOMIC_RELAXED);
}

/* the run's last access to the state: a drain may free it right after */
static inline void rd3_tcall_st_run_end(struct rd3_tcall_state *s)
{
    __atomic_sub_fetch(&s->outstanding, 1, __ATOMIC_SEQ_CST);
}

static inline int rd3_tcall_st_cancelled(struct rd3_tcall_state *s, int did)
{
    if (!did)
        return 0;
    __atomic_add_fetch(&s->cancelled, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&s->outstanding, 1, __ATOMIC_SEQ_CST);
    return 1;
}

static inline void rd3_tcall_st_stop(struct rd3_tcall_state *s)
{
    __atomic_store_n(&s->stopping, 1, __ATOMIC_SEQ_CST);
}

enum { RD3_TCALL_NOTE_SLOW = 1, RD3_TCALL_NOTE_STILL = 2, RD3_TCALL_NOTE_SELF = 3 };

struct rd3_tcall_ops {
    int  (*cancel)(void *ctx);                 /* thread_call_cancel: 1 = a pending run removed */
    void (*nap)(void *ctx, unsigned ms);       /* IOSleep */
    void (*note)(void *ctx, int what, uint64_t waited_ms, int32_t outstanding);   /* IOLog */
};

struct rd3_tcall_drain_res {
    unsigned cancelled;   /* pending runs removed (each never ran: a reference it held goes back) */
    uint64_t waited_ms;   /* naps taken, at least this long */
    unsigned notes;       /* slow / still-waiting notes given */
};

/* cancel, then look (cancelling again each time) until no run is armed or
 * running any more, however long that takes; 'self': called from inside
 * one of the call's runs, which cannot end before this returns (not waited
 * for). Noted at slow_ms, then every report_ms. */
static inline void rd3_tcall_st_drain(struct rd3_tcall_state *s, int self, const struct rd3_tcall_ops *ops,
                                      void *ctx, unsigned slow_ms, unsigned report_ms,
                                      struct rd3_tcall_drain_res *r)
{
    const int32_t floor = self ? 1 : 0;
    uint64_t next_note = slow_ms;
    r->cancelled = 0;
    r->waited_ms = 0;
    r->notes = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the caller's stop flag before the first cancel */
    __atomic_add_fetch(&s->drains, 1, __ATOMIC_RELAXED);
    if (self) {
        __atomic_add_fetch(&s->self_drains, 1, __ATOMIC_RELAXED);
        if (ops->note)
            ops->note(ctx, RD3_TCALL_NOTE_SELF, 0, __atomic_load_n(&s->outstanding, __ATOMIC_SEQ_CST));
    }
    for (;;) {
        /* every pending run goes first (a run that re-armed itself while it
         * was waited for is one; each cancelled one never runs) */
        while (rd3_tcall_st_cancelled(s, ops->cancel(ctx)))
            r->cancelled++;
        const int32_t left = __atomic_load_n(&s->outstanding, __ATOMIC_SEQ_CST);
        if (left <= floor)
            return;
        if (r->waited_ms >= next_note) {
            if (r->notes == 0)
                __atomic_add_fetch(&s->slow, 1, __ATOMIC_RELAXED);
            if (ops->note)
                ops->note(ctx, r->notes == 0 ? RD3_TCALL_NOTE_SLOW : RD3_TCALL_NOTE_STILL, r->waited_ms, left);
            r->notes++;
            next_note += report_ms;
        }
        const unsigned ms = r->waited_ms < RD3_TCALL_NAP_FAST ? 1u : 10u;
        ops->nap(ctx, ms);
        r->waited_ms += ms;
    }
}

/* ------------------------------------------------------------------ */
#if defined(KERNEL) && defined(__cplusplus)

#include <kern/thread_call.h>
#include <kern/thread.h>
#include <IOKit/IOLib.h>

struct rd3_tcall {
    thread_call_t          call;
    thread_call_func_t     fn;
    thread_call_param_t    p0;
    const char            *name;     /* a string literal: the log lines name the call */
    void *volatile         runner;   /* the thread inside fn (the last one to enter) */
    struct rd3_tcall_state st;
};

enum { RD3_TCALL_QUEUED = 0, RD3_TCALL_MERGED = 1, RD3_TCALL_REFUSED = 2 };

static inline void rd3_tcall_tramp(thread_call_param_t p0, thread_call_param_t p1)
{
    struct rd3_tcall *t = (struct rd3_tcall *)p0;
    rd3_tcall_st_run_begin(&t->st);
    t->runner = (void *)current_thread();
    t->fn(t->p0, p1);
    t->runner = nullptr;
    rd3_tcall_st_run_end(&t->st);   /* the last access: a drain may free t now */
}

static inline struct rd3_tcall *rd3_tcall_alloc_opts(const char *name, thread_call_func_t fn,
                                                     thread_call_param_t p0, int with_options,
                                                     thread_call_priority_t pri, thread_call_options_t opts)
{
    struct rd3_tcall *t = (struct rd3_tcall *)IOMalloc(sizeof(struct rd3_tcall));
    if (!t) {
        IOLog("RDNA3-tcall: %s: thread call allocation failed (no memory for its state, %lu bytes): the work it "
              "runs will not happen\n", name, (unsigned long)sizeof(struct rd3_tcall));
        return nullptr;
    }
    bzero(t, sizeof(*t));
    t->fn = fn;
    t->p0 = p0;
    t->name = name;
    t->call = with_options ? thread_call_allocate_with_options(rd3_tcall_tramp, t, pri, opts)
                           : thread_call_allocate(rd3_tcall_tramp, t);
    if (!t->call) {
        IOLog("RDNA3-tcall: %s: thread call allocation failed (%s returned NULL, priority %d, options 0x%x): the "
              "work it runs will not happen\n", name,
              with_options ? "thread_call_allocate_with_options" : "thread_call_allocate", (int)pri,
              (unsigned)opts);
        IOFree(t, sizeof(*t));
        return nullptr;
    }
    return t;
}

/* thread_call_allocate(fn, p0) */
static inline struct rd3_tcall *rd3_tcall_alloc(const char *name, thread_call_func_t fn, thread_call_param_t p0)
{
    return rd3_tcall_alloc_opts(name, fn, p0, 0, THREAD_CALL_PRIORITY_HIGH, 0);
}

/* thread_call_allocate_with_options(fn, p0, pri, opts) */
static inline struct rd3_tcall *rd3_tcall_alloc_with(const char *name, thread_call_func_t fn, thread_call_param_t p0,
                                                     thread_call_priority_t pri, thread_call_options_t opts)
{
    return rd3_tcall_alloc_opts(name, fn, p0, 1, pri, opts);
}

/* an arm refused because rd3_tcall_free began (the state still held) */
static inline int rd3_tcall_refused(struct rd3_tcall *t)
{
    const uint32_t n = __atomic_load_n(&t->st.refused, __ATOMIC_RELAXED);
    if (n == 1 || (n & 1023) == 0)
        IOLog("RDNA3-tcall: %s: an enter during its stop was refused (#%u): no run queued\n", t->name, n);
    rd3_tcall_st_unhold(&t->st);   /* the last access */
    return RD3_TCALL_REFUSED;
}

/* RD3_TCALL_QUEUED, _MERGED (into a run already pending: no new run) or
 * _REFUSED (the free began: no run); never call it after rd3_tcall_free
 * returned (t is gone) */
static inline int rd3_tcall_enter3(struct rd3_tcall *t)
{
    if (!rd3_tcall_st_arm(&t->st))
        return rd3_tcall_refused(t);
    const boolean_t was = thread_call_enter(t->call);
    rd3_tcall_st_entered(&t->st, was ? 1 : 0);
    return was ? RD3_TCALL_MERGED : RD3_TCALL_QUEUED;
}

static inline int rd3_tcall_enter_delayed3(struct rd3_tcall *t, uint64_t deadline)
{
    if (!rd3_tcall_st_arm(&t->st))
        return rd3_tcall_refused(t);
    const boolean_t was = thread_call_enter_delayed(t->call, deadline);
    rd3_tcall_st_entered(&t->st, was ? 1 : 0);
    return was ? RD3_TCALL_MERGED : RD3_TCALL_QUEUED;
}

/* thread_call_enter / thread_call_enter_delayed: true = no new run (merged
 * into one already pending, or refused), so a reference taken for it goes
 * back, as with thread_call_enter's TRUE */
static inline bool rd3_tcall_enter(struct rd3_tcall *t)
{
    return rd3_tcall_enter3(t) != RD3_TCALL_QUEUED;
}

static inline bool rd3_tcall_enter_delayed(struct rd3_tcall *t, uint64_t deadline)
{
    return rd3_tcall_enter_delayed3(t, deadline) != RD3_TCALL_QUEUED;
}

/* thread_call_cancel: no wait; true = a pending run removed */
static inline bool rd3_tcall_cancel(struct rd3_tcall *t)
{
    return rd3_tcall_st_cancelled(&t->st, thread_call_cancel(t->call) ? 1 : 0) != 0;
}

static inline int rd3_tcall_op_cancel(void *ctx)
{
    return thread_call_cancel(((struct rd3_tcall *)ctx)->call) ? 1 : 0;
}

static inline void rd3_tcall_op_nap(void *ctx, unsigned ms)
{
    (void)ctx;
    IOSleep(ms);
}

static inline void rd3_tcall_op_note(void *ctx, int what, uint64_t waited_ms, int32_t left)
{
    const struct rd3_tcall *t = (const struct rd3_tcall *)ctx;
    if (what == RD3_TCALL_NOTE_SELF) {
        const uint32_t n = t->st.self_drains;
        if (n <= 8 || (n & 1023) == 0)
            IOLog("RDNA3-tcall: %s: stopped from inside its own run: that run is not waited for (#%u)\n", t->name, n);
        return;
    }
    /* the first note of a slow stop: rate limited per call; the later ones
     * of the same stop (every RD3_TCALL_REPORT_MS: a hang) always */
    const uint32_t n = t->st.slow;
    if (what == RD3_TCALL_NOTE_SLOW && n > 8 && (n & 1023) != 0)
        return;
    IOLog("RDNA3-tcall: %s: the stop has waited %llu ms for %d run(s) still outstanding%s (slow stop #%u)\n",
          t->name, (unsigned long long)waited_ms, (int)left,
          what == RD3_TCALL_NOTE_STILL ? ", still waiting (as thread_call_cancel_wait would)" : "", n);
}

/* thread_call_cancel_wait's callers' loop: every pending run cancelled,
 * every running one waited for, however long; the call stays usable (a
 * later enter arms it again). Returns how many pending runs were cancelled. */
static inline unsigned rd3_tcall_drain(struct rd3_tcall *t)
{
    const struct rd3_tcall_ops ops = { rd3_tcall_op_cancel, rd3_tcall_op_nap, rd3_tcall_op_note };
    struct rd3_tcall_drain_res r;
    const int self = t->runner == (void *)current_thread();
    rd3_tcall_st_drain(&t->st, self, &ops, t, RD3_TCALL_SLOW_MS, RD3_TCALL_REPORT_MS, &r);
    if (r.notes)
        IOLog("RDNA3-tcall: %s: drained after %llu ms\n", t->name, (unsigned long long)r.waited_ms);
    return r.cancelled;
}

/* every later arm refused, the drain, then thread_call_free and the state
 * freed; t is gone afterwards. Refused (logged FAIL, nothing freed) from
 * inside the call's own run. Returns how many pending runs were cancelled. */
static inline unsigned rd3_tcall_free(struct rd3_tcall *t)
{
    const struct rd3_tcall_ops ops = { rd3_tcall_op_cancel, rd3_tcall_op_nap, rd3_tcall_op_note };
    struct rd3_tcall_drain_res r;
    if (t->runner == (void *)current_thread()) {
        IOLog("RDNA3-tcall: %s: FAIL: freed from inside its own run: refused, the thread call stays allocated "
              "(a driver bug: free it from the stop path)\n", t->name);
        return 0;
    }
    rd3_tcall_st_stop(&t->st);
    rd3_tcall_st_drain(&t->st, 0, &ops, t, RD3_TCALL_SLOW_MS, RD3_TCALL_REPORT_MS, &r);
    if (r.notes)
        IOLog("RDNA3-tcall: %s: drained after %llu ms\n", t->name, (unsigned long long)r.waited_ms);
    /* nothing is queued now (arms are refused, the drain cancelled the
     * last); thread_call_free refuses only a queued call or a pending ONCE
     * reschedule: cancel and try again, never give up */
    for (unsigned i = 1; !thread_call_free(t->call); i++) {
        if (rd3_tcall_cancel(t))
            r.cancelled++;
        if (i == 1000 || i % 30000 == 0)
            IOLog("RDNA3-tcall: %s: thread_call_free refused %u times, still trying\n", t->name, i);
        IOSleep(1);
    }
    IOFree(t, sizeof(*t));
    return r.cancelled;
}

#endif /* KERNEL && __cplusplus */
#endif /* RDNA3_TCALL_H */
