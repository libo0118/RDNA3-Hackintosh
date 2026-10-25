/*
 * test_tcall.c - common/rdna3_tcall.h on the host (card run 2026-10-25b,
 * DRIVER-PLAN N47: stopping a thread call without thread_call_cancel_wait,
 * which macOS 26 exports only from com.apple.kpi.private).
 *
 * The model of xnu's thread call (osfmk/kern/thread_call.c): one pending
 * entry at most (thread_call_enter on a pending call returns TRUE and
 * leaves one entry); a run is dequeued first and enters the function a
 * little later; a ONCE call entered while it runs is rescheduled (TRUE
 * only when a reschedule was already set) and queued again when the run
 * ends; thread_call_cancel removes a pending entry or a reschedule. The
 * drain's naps advance a clock on which the model's runs start and end.
 *
 * Checked: every run is counted from its arm to its end; a merged enter
 * does not count twice; a run that is dequeued but has not reached the
 * trampoline is still waited for (thread_call_cancel finds nothing then);
 * a run going on while the drain cancels is waited for; a run that re-arms
 * itself during the drain (plain and ONCE) has that run cancelled and
 * reported (its reference goes back); a drain from inside the call's own
 * run does not wait for itself; a run that never ends is logged slow at
 * RD3_TCALL_SLOW_MS and given up at RD3_TCALL_GIVEUP_MS; after the free's
 * stop every arm is refused.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_tcall.h"

static int fails;
#define CHECK(c, ...)                                                    \
    do {                                                                 \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("test_tcall: FAIL line %d: ", __LINE__);              \
            printf(__VA_ARGS__);                                         \
            printf("\n");                                                \
        }                                                                \
    } while (0)

/* ---- the model ---- */
struct sim {
    struct rd3_tcall_state st;
    int once;
    int queued;          /* one pending entry */
    int resched;         /* ONCE: entered while running */
    int dequeued;        /* taken off the queue, the trampoline not yet entered */
    int running;         /* inside the function */
    unsigned now;        /* ms */
    unsigned dispatch_at, end_at, rearm_at;   /* events (0 = none) */
    int rearm_on_end;    /* the run re-arms itself as it ends (read the stop flag before it was set) */
    int notes[8], nnotes;
    unsigned note_ms[8];
};

static int sim_enter(struct sim *s)
{
    if (s->once && (s->running || s->dequeued)) {
        const int was = s->resched;
        s->resched = 1;
        return was;
    }
    const int was = s->queued;
    s->queued = 1;
    return was;
}

/* the wrapper's arm: rd3_tcall_enter */
static int sim_arm(struct sim *s)
{
    if (!rd3_tcall_st_arm(&s->st))
        return 1;
    const int was = sim_enter(s);
    rd3_tcall_st_entered(&s->st, was);
    return was;
}

static int op_cancel(void *ctx)
{
    struct sim *s = (struct sim *)ctx;
    if (s->resched) {
        s->resched = 0;
        return 1;
    }
    if (s->queued) {
        s->queued = 0;
        return 1;
    }
    return 0;
}

static void sim_dequeue(struct sim *s)
{
    if (s->queued) {
        s->queued = 0;
        s->dequeued = 1;
    }
}

static void sim_tramp_enter(struct sim *s)
{
    s->dequeued = 0;
    s->running = 1;
    rd3_tcall_st_run_begin(&s->st);
}

static void sim_run_end(struct sim *s)
{
    if (s->rearm_on_end) {
        s->rearm_on_end = 0;
        sim_arm(s);
    }
    s->running = 0;
    rd3_tcall_st_run_end(&s->st);
    if (s->once && s->resched) {   /* xnu queues a rescheduled ONCE call again */
        s->resched = 0;
        s->queued = 1;
    }
}

static void op_nap(void *ctx, unsigned ms)
{
    struct sim *s = (struct sim *)ctx;
    for (unsigned i = 0; i < ms; i++) {
        s->now++;
        if (s->dispatch_at && s->now >= s->dispatch_at && s->dequeued) {
            s->dispatch_at = 0;
            sim_tramp_enter(s);
        }
        if (s->rearm_at && s->now >= s->rearm_at && s->running) {
            s->rearm_at = 0;
            sim_arm(s);   /* a re-arm while the run still goes on */
        }
        if (s->end_at && s->now >= s->end_at && s->running) {
            s->end_at = 0;
            sim_run_end(s);
        }
    }
}

static void op_note(void *ctx, int what, unsigned waited, int32_t left)
{
    struct sim *s = (struct sim *)ctx;
    (void)left;
    if (s->nnotes < 8) {
        s->note_ms[s->nnotes] = waited;
        s->notes[s->nnotes++] = what;
    }
}

static const struct rd3_tcall_ops OPS = { op_cancel, op_nap, op_note };

static int drain(struct sim *s, int self, struct rd3_tcall_drain_res *r)
{
    return rd3_tcall_st_drain(&s->st, self, &OPS, s, RD3_TCALL_SLOW_MS, RD3_TCALL_GIVEUP_MS, r);
}

static void reset(struct sim *s, int once)
{
    memset(s, 0, sizeof(*s));
    s->once = once;
}

int main(void)
{
    struct sim s;
    struct rd3_tcall_drain_res r;

    /* an arm, a merged arm, the run: counted once */
    reset(&s, 0);
    CHECK(sim_arm(&s) == 0, "first enter not pending");
    CHECK(sim_arm(&s) == 1, "second enter merged");
    CHECK(s.st.outstanding == 1 && s.st.merged == 1 && s.st.armed == 2, "merged arm counted once (out %d)",
          s.st.outstanding);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    sim_run_end(&s);
    CHECK(s.st.outstanding == 0 && s.st.runs == 1, "run ended: out %d", s.st.outstanding);

    /* nothing armed: drained at once, no nap */
    CHECK(drain(&s, 0, &r) == 1 && r.waited_ms == 0 && r.cancelled == 0, "idle drain");

    /* a pending run: cancelled, never runs, reported */
    reset(&s, 0);
    sim_arm(&s);
    CHECK(drain(&s, 0, &r) == 1 && r.cancelled == 1 && r.waited_ms == 0 && s.st.outstanding == 0,
          "pending run cancelled (cancelled %u)", r.cancelled);

    /* a run going on while the drain cancels: thread_call_cancel finds
     * nothing, the drain waits for its end */
    reset(&s, 0);
    sim_arm(&s);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    s.end_at = 7;
    CHECK(drain(&s, 0, &r) == 1, "running run drained");
    CHECK(r.cancelled == 0 && r.waited_ms >= 7 && !s.running && s.st.outstanding == 0,
          "waited for the running run (waited %u, cancelled %u)", r.waited_ms, r.cancelled);
    CHECK(!r.slow && s.nnotes == 0, "a short wait is not slow");

    /* dequeued, not yet in the trampoline: a counter bumped at entry would
     * read 0 here; the arm-time count waits for it */
    reset(&s, 0);
    sim_arm(&s);
    sim_dequeue(&s);
    s.dispatch_at = 3;
    s.end_at = 9;
    CHECK(drain(&s, 0, &r) == 1 && r.waited_ms >= 9 && s.st.runs == 1 && s.st.outstanding == 0,
          "the run between dequeue and trampoline waited for (waited %u, runs %u)", r.waited_ms, s.st.runs);

    /* a run that re-arms itself as it ends during the drain (it read the
     * stop flag before the caller set it): that new run is cancelled */
    reset(&s, 0);
    sim_arm(&s);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    s.end_at = 4;
    s.rearm_on_end = 1;
    CHECK(drain(&s, 0, &r) == 1 && r.cancelled == 1 && !s.queued && s.st.outstanding == 0,
          "self re-arm during the drain cancelled (cancelled %u, queued %d, out %d)", r.cancelled, s.queued,
          s.st.outstanding);

    /* plain call re-armed while still running: queued beside the run;
     * cancelled, the run waited for */
    reset(&s, 0);
    sim_arm(&s);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    s.rearm_at = 2;
    s.end_at = 6;
    CHECK(drain(&s, 0, &r) == 1 && r.cancelled == 1 && r.waited_ms >= 6 && s.st.outstanding == 0,
          "plain re-arm while running (cancelled %u waited %u)", r.cancelled, r.waited_ms);

    /* ONCE re-armed while running: a reschedule, cancelled */
    reset(&s, 1);
    sim_arm(&s);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    s.rearm_at = 2;
    s.end_at = 6;
    CHECK(drain(&s, 0, &r) == 1 && r.cancelled == 1 && !s.queued && !s.resched && s.st.outstanding == 0,
          "ONCE reschedule cancelled (cancelled %u, queued %d, resched %d)", r.cancelled, s.queued, s.resched);
    /* ONCE: the first enter while running is not pending, the second merges */
    reset(&s, 1);
    sim_arm(&s);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    CHECK(sim_arm(&s) == 0 && sim_arm(&s) == 1 && s.st.outstanding == 2, "ONCE re-arms while running: out %d",
          s.st.outstanding);
    sim_run_end(&s);
    CHECK(s.queued && s.st.outstanding == 1, "ONCE reschedule queued after the run");
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    sim_run_end(&s);
    CHECK(s.st.outstanding == 0 && s.st.runs == 2, "ONCE: two runs, none left (out %d)", s.st.outstanding);

    /* a drain from inside the call's own run: not waited for; a pending
     * re-arm of it still cancelled */
    reset(&s, 0);
    sim_arm(&s);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    sim_arm(&s);
    CHECK(drain(&s, 1, &r) == 1 && r.cancelled == 1 && r.waited_ms == 0 && s.st.outstanding == 1,
          "self drain (cancelled %u, waited %u, out %d)", r.cancelled, r.waited_ms, s.st.outstanding);
    CHECK(s.nnotes == 1 && s.notes[0] == RD3_TCALL_NOTE_SELF && s.st.self_drains == 1, "self drain noted");

    /* a run that never ends: slow at RD3_TCALL_SLOW_MS (once), given up at
     * RD3_TCALL_GIVEUP_MS */
    reset(&s, 0);
    sim_arm(&s);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    CHECK(drain(&s, 0, &r) == 0 && r.gaveup && r.slow, "a hung run: gave up");
    CHECK(r.waited_ms >= RD3_TCALL_GIVEUP_MS && r.waited_ms < RD3_TCALL_GIVEUP_MS + 20, "gave up at %u ms",
          r.waited_ms);
    CHECK(s.nnotes == 2 && s.notes[0] == RD3_TCALL_NOTE_SLOW && s.notes[1] == RD3_TCALL_NOTE_GAVEUP,
          "notes: slow then gave up (%d notes)", s.nnotes);
    CHECK(s.note_ms[0] >= RD3_TCALL_SLOW_MS && s.note_ms[0] < RD3_TCALL_SLOW_MS + 20, "slow noted at %u ms",
          s.note_ms[0]);
    CHECK(s.st.slow == 1 && s.st.gaveup == 1 && s.st.outstanding == 1, "hung run still counted");
    CHECK(s.now >= RD3_TCALL_GIVEUP_MS, "the naps took the time (%u ms)", s.now);

    /* a run slower than the slow bound but ending: slow, drained */
    reset(&s, 0);
    sim_arm(&s);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    s.end_at = RD3_TCALL_SLOW_MS + 500;
    CHECK(drain(&s, 0, &r) == 1 && r.slow && !r.gaveup && s.st.outstanding == 0, "slow run drained");

    /* the free's stop: every arm refused, nothing counted; a drain after it
     * still waits for the run going on */
    reset(&s, 0);
    sim_arm(&s);
    sim_dequeue(&s);
    sim_tramp_enter(&s);
    rd3_tcall_st_stop(&s.st);
    CHECK(sim_arm(&s) == 1 && !s.queued && s.st.refused == 1 && s.st.outstanding == 1,
          "arm after stop refused (refused %u, out %d)", s.st.refused, s.st.outstanding);
    s.end_at = 5;
    s.rearm_on_end = 1;   /* the run's own re-arm is refused too */
    CHECK(drain(&s, 0, &r) == 1 && r.cancelled == 0 && s.st.refused == 2 && s.st.outstanding == 0,
          "drain after stop: re-arm refused, nothing to cancel (refused %u)", s.st.refused);

    if (fails) {
        printf("test_tcall: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_tcall: ok\n");
    return 0;
}
