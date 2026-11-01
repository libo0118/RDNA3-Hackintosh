/* DRIVER-PLAN N71 (c), the producer: the gate (an app's IOSurface textures in family VRAM) and the share record decision table
 * (metal/plugin/nvmtl_surface_share.h) */
#include <stdio.h>
#include <string.h>
#include "nvmtl_surface_share.h"

static int fails;
static void check(long long got, long long want, const char *what)
{
    if (got != want) { printf("FAIL %s: %lld, want %lld\n", what, got, want); fails++; }
}

#define NS 1000000000ull

int main(void)
{
    /* ---- the gate: nvmtl_surface_share_kept_private(is_ws, a3ws, forced, a3share) ---- */
    /* a3share absent (-1) or 0: the N71 (a) table, unchanged */
    check(nvmtl_surface_share_kept_private(0, 0, 0, -1), 1, "app, a3ws 0, no a3share sysctl: private");
    check(nvmtl_surface_share_kept_private(0, 0, 0, 0), 1, "app, a3ws 0, a3share 0: private");
    check(nvmtl_surface_share_kept_private(0, 1, 0, 0), 0, "app, a3ws 1: shared as before");
    check(nvmtl_surface_share_kept_private(0, -1, 0, -1), 0, "app, no sysctls (older accelerator): shared as before");
    check(nvmtl_surface_share_kept_private(1, 0, 0, 0), 0, "WindowServer: its own policy, unchanged");
    check(nvmtl_surface_share_kept_private(0, 0, 1, 0), 0, "NVMTL_SURFACE_SHARE=1 forces sharing");
    check(nvmtl_surface_share_kept_private(1, 1, 1, 1), 0, "WindowServer, forced: shared");
    /* a3share 1: the app's drawables live in VRAM although the kernel keeps WindowServer's inputs out (a3ws 0) */
    check(nvmtl_surface_share_kept_private(0, 0, 0, 1), 0, "app, a3ws 0, a3share 1: shared");
    check(nvmtl_surface_share_kept_private(0, 1, 0, 1), 0, "app, a3ws 1, a3share 1: shared");
    check(nvmtl_surface_share_kept_private(0, -1, 0, 1), 0, "app, a3ws absent, a3share 1: shared");
    /* the switch is live: the same inputs with a3share going 1 -> 0 flip the answer back */
    check(nvmtl_surface_share_kept_private(0, 0, 0, 0), 1, "a3share back to 0: private again");

    /* ---- the re-read of the switch: at most once a second ---- */
    check(nvmtl_ps_reread_due(0, 5 * NS), 1, "never read: due");
    check(nvmtl_ps_reread_due(5 * NS, 5 * NS + 999999999ull), 0, "read 0.999 s ago: not due");
    check(nvmtl_ps_reread_due(5 * NS, 6 * NS), 1, "read 1 s ago: due");
    check(nvmtl_ps_reread_due(9 * NS, 5 * NS), 1, "clock went back: due (never stuck)");

    /* ---- the record decision (nvmtl_ps_decide) ---- */
    nvmtl_ps_tex t; nvmtl_ps_proc p; memset(&t, 0, sizeof t); memset(&p, 0, sizeof p);
    t.gen = 77;
    check(nvmtl_ps_decide(&t, &p, 1, 0, 1, NS), NVMTL_PS_NO_WS, "WindowServer never records");
    check(nvmtl_ps_decide(&t, &p, 0, 1, 1, NS), NVMTL_PS_NO_KILL, "kill switch: no record");
    check(nvmtl_ps_decide(&t, &p, 0, 0, 0, NS), NVMTL_PS_NO_SWITCH, "a3share 0: no record");
    check(nvmtl_ps_decide(&t, &p, 0, 0, -1, NS), NVMTL_PS_NO_SWITCH, "a3share absent: no record");
    check(nvmtl_ps_decide(&t, &p, 0, 0, 1, NS), NVMTL_PS_CALL, "a3share 1, gen known: ask the kernel");
    { nvmtl_ps_tex z; memset(&z, 0, sizeof z);
      check(nvmtl_ps_decide(&z, &p, 0, 0, 1, NS), NVMTL_PS_NO_NOGEN, "no generation from the import (older kext): no call");
      check(nvmtl_ps_decide(&z, &p, 0, 0, 0, NS), NVMTL_PS_NO_SWITCH, "no generation, switch 0: silent (no 'no generation' line either)"); }

    /* 0: recorded once per generation */
    check(nvmtl_ps_result(&t, &p, 0, NS), NVMTL_PSR_RECORDED, "ok -> recorded");
    check(t.shared_gen == 77, 1, "the recorded generation is kept");
    check(nvmtl_ps_decide(&t, &p, 0, 0, 1, 2 * NS), NVMTL_PS_NO_DONE, "recorded: not asked again for this generation");
    t.gen = 78;   /* the texture was imported again: a new generation */
    check(nvmtl_ps_decide(&t, &p, 0, 0, 1, 2 * NS), NVMTL_PS_CALL, "a new generation is asked once more");
    memset(&t, 0, sizeof t); t.gen = 5;

    /* ENXIO: retry after the next re-read, not before */
    check(nvmtl_ps_result(&t, &p, ENXIO, 10 * NS), NVMTL_PSR_RETRY, "ENXIO -> retry later");
    check(nvmtl_ps_decide(&t, &p, 0, 0, 1, 10 * NS + 1), NVMTL_PS_NO_WAIT, "ENXIO: not asked within the second");
    check(nvmtl_ps_decide(&t, &p, 0, 0, 1, 11 * NS), NVMTL_PS_CALL, "ENXIO: asked again a second later");
    check(nvmtl_ps_result(&t, &p, 0, 11 * NS), NVMTL_PSR_RECORDED, "ENXIO then ok -> recorded");
    check(t.retry_ns, 0, "the retry time is cleared by a record");

    /* EPERM: the surface was re-backed - no record for this import, other textures unaffected */
    nvmtl_ps_tex a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); a.gen = 9; b.gen = 10;
    check(nvmtl_ps_result(&a, &p, EPERM, NS), NVMTL_PSR_REBACKED, "EPERM -> re-backed");
    check(nvmtl_ps_decide(&a, &p, 0, 0, 1, 5 * NS), NVMTL_PS_NO_DEAD, "EPERM: that import is not asked again");
    check(nvmtl_ps_decide(&b, &p, 0, 0, 1, 5 * NS), NVMTL_PS_CALL, "EPERM: another texture is unaffected");
    /* ENOENT: no writable import on the connection - dead for this import */
    check(nvmtl_ps_result(&b, &p, ENOENT, NS), NVMTL_PSR_NOIMPORT, "ENOENT -> no import here");
    check(nvmtl_ps_decide(&b, &p, 0, 0, 1, 5 * NS), NVMTL_PS_NO_DEAD, "ENOENT: not asked again");
    /* ENODEV / ENOSYS: an older kernel - the whole process stops */
    { nvmtl_ps_tex c; memset(&c, 0, sizeof c); c.gen = 3;
      check(nvmtl_ps_result(&c, &p, ENODEV, NS), NVMTL_PSR_STOP, "ENODEV -> stop");
      check(p.stop, 1, "ENODEV stops the process");
      nvmtl_ps_tex d; memset(&d, 0, sizeof d); d.gen = 4;
      check(nvmtl_ps_decide(&d, &p, 0, 0, 1, 5 * NS), NVMTL_PS_NO_STOP, "after ENODEV no texture asks");
      memset(&p, 0, sizeof p);
      check(nvmtl_ps_result(&d, &p, ENOSYS, NS), NVMTL_PSR_STOP, "ENOSYS -> stop");
      check(p.stop, 1, "ENOSYS stops the process"); }
    memset(&p, 0, sizeof p);
    /* an unexpected errno: asked again a second later, three times, then dead */
    { nvmtl_ps_tex e; memset(&e, 0, sizeof e); e.gen = 6;
      check(nvmtl_ps_result(&e, &p, EINVAL, NS), NVMTL_PSR_OTHER, "EINVAL -> other (1)");
      check(nvmtl_ps_decide(&e, &p, 0, 0, 1, NS + 1), NVMTL_PS_NO_WAIT, "other: waits a second");
      check(nvmtl_ps_result(&e, &p, EINVAL, 2 * NS), NVMTL_PSR_OTHER, "other (2)");
      check(e.dead, 0, "still alive after two");
      check(nvmtl_ps_result(&e, &p, EINVAL, 3 * NS), NVMTL_PSR_OTHER, "other (3)");
      check(e.dead, 1, "dead after the third");
      check(nvmtl_ps_decide(&e, &p, 0, 0, 1, 9 * NS), NVMTL_PS_NO_DEAD, "other: not asked again"); }
    /* the kill switch beats everything, WindowServer beats the kill switch (nothing is asked either way) */
    check(nvmtl_ps_decide(&b, &p, 1, 1, 1, NS), NVMTL_PS_NO_WS, "WindowServer, killed: still no call");

    /* ---- the prefilter before the call (nvmtl_ps_wants) ---- */
    { nvmtl_ps_tex w; memset(&w, 0, sizeof w); w.gen = 4;
      check(nvmtl_ps_wants(&w, 1, 0, 1), 1, "a VRAM texture with a generation, not shared yet: ask");
      check(nvmtl_ps_wants(&w, 0, 0, 1), 0, "not a VRAM texture");
      check(nvmtl_ps_wants(&w, 1, 1, 1), 0, "a read-only alias never records");
      check(nvmtl_ps_wants(&w, 1, 0, 0), 0, "no IOSurface");
      w.shared_gen = 4;
      check(nvmtl_ps_wants(&w, 1, 0, 1), 0, "recorded for this generation: skip");
      w.shared_gen = 0; w.dead = 1;
      check(nvmtl_ps_wants(&w, 1, 0, 1), 0, "dead: skip");
      nvmtl_ps_tex n; memset(&n, 0, sizeof n);
      check(nvmtl_ps_wants(&n, 1, 0, 1), 1, "no generation, not said: ask once (to log it)");
      n.said = 1;
      check(nvmtl_ps_wants(&n, 1, 0, 1), 0, "no generation, said: skip"); }

    /* ---- counters and their SUMMARY tail ---- */
    nvmtl_ps_stats s; memset(&s, 0, sizeof s);
    char buf[256];
    check(nvmtl_ps_events(&s), 0, "nothing counted");
    check(nvmtl_ps_format(&s, buf, sizeof buf), 0, "nothing counted: no tail");
    nvmtl_ps_count(&s, NVMTL_PSR_RECORDED); nvmtl_ps_count(&s, NVMTL_PSR_RECORDED); nvmtl_ps_count(&s, NVMTL_PSR_RETRY);
    nvmtl_ps_count(&s, NVMTL_PSR_STOP); nvmtl_ps_count(&s, NVMTL_PSR_REBACKED); nvmtl_ps_count(&s, NVMTL_PSR_NOIMPORT);
    nvmtl_ps_count(&s, NVMTL_PSR_OTHER); s.nogen = 3;
    check(nvmtl_ps_format(&s, buf, sizeof buf), 1, "counted: a tail");
    check(strcmp(buf, "pr_shared=2 pr_enxio=1 pr_stop=1 pr_eperm=1 pr_enoent=1 pr_other=1 pr_nogen=3"), 0, "the tail's exact text");
    check(strcmp(nvmtl_ps_errno_name(ENXIO), "ENXIO"), 0, "name ENXIO");
    check(strcmp(nvmtl_ps_errno_name(EPERM), "EPERM"), 0, "name EPERM");
    check(strcmp(nvmtl_ps_errno_name(12345), "other"), 0, "an unknown errno has a name");

    printf("test_surface_share: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
