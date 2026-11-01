/* DRIVER-PLAN N71 (c), the consumer: WindowServer's read-only shared imports (metal/plugin/nvmtl_ws_share.h):
 * eligibility, the plan at wrapper creation, the sample, the R1 hazard detector and demotion, R4 error counting, the generation value */
#include <stdio.h>
#include <string.h>
#include "nvmtl_ws_share.h"

static int fails;
static void check(long long got, long long want, const char *what)
{
    if (got != want) { printf("FAIL %s: %lld, want %lld\n", what, got, want); fails++; }
}

int main(void)
{
    const uint32_t R = NVMTL_WSS_USAGE_READ, W = NVMTL_WSS_USAGE_WRITE, RT = NVMTL_WSS_USAGE_RT, V = NVMTL_WSS_USAGE_VIEW;

    /* ---- eligibility: is_ws, kill, session_off, demoted, usage, cpu_write ---- */
    check(nvmtl_wss_eligible(1, 0, 1, 0, 0, R, 0), NVMTL_WSS_OK, "WindowServer, ShaderRead: eligible");
    check(nvmtl_wss_eligible(1, 0, 1, 0, 0, R | V, 0), NVMTL_WSS_OK, "ShaderRead + PixelFormatView: eligible");
    check(nvmtl_wss_eligible(0, 0, 1, 0, 0, R, 0), NVMTL_WSS_NOT_WS, "an app is not a consumer");
    check(nvmtl_wss_eligible(1, 1, 1, 0, 0, R, 0), NVMTL_WSS_KILL, "NVMTL_WS_SHARE=0");
    check(nvmtl_wss_eligible(1, 0, 0, 0, 0, R, 0), NVMTL_WSS_SWITCH_OFF, "a3share 0 (live): back to the private path");
    check(nvmtl_wss_eligible(1, 0, 1, 1, 0, R, 0), NVMTL_WSS_SESSION_OFF, "stopped for the session");
    check(nvmtl_wss_eligible(1, 0, 1, 0, 1, R, 0), NVMTL_WSS_DEMOTED, "demoted surface");
    check(nvmtl_wss_eligible(1, 0, 1, 0, 0, 0, 0), NVMTL_WSS_USAGE_UNKNOWN, "usage Unknown (0) may be written: private");
    check(nvmtl_wss_eligible(1, 0, 1, 0, 0, R | RT, 0), NVMTL_WSS_RENDER_TARGET, "RenderTarget: private");
    check(nvmtl_wss_eligible(1, 0, 1, 0, 0, RT, 0), NVMTL_WSS_RENDER_TARGET, "RenderTarget alone: private");
    check(nvmtl_wss_eligible(1, 0, 1, 0, 0, R | W, 0), NVMTL_WSS_SHADER_WRITE, "ShaderWrite: private");
    check(nvmtl_wss_eligible(1, 0, 1, 0, 0, R | 0x8u, 0), NVMTL_WSS_USAGE_OTHER, "a usage bit nobody knows: private");
    check(nvmtl_wss_eligible(1, 0, 1, 0, 0, R, 1), NVMTL_WSS_CPU_ACCESS, "write-combined CPU access: private");
    check(nvmtl_wss_eligible(1, 1, 1, 1, 1, 0, 1), NVMTL_WSS_KILL, "the first reason wins (kill)");
    check(strcmp(nvmtl_wss_why_name(NVMTL_WSS_RENDER_TARGET), "RenderTarget usage"), 0, "a reason has a name");
    check(strcmp(nvmtl_wss_why_name(99), "?"), 0, "an unknown reason has a name");

    /* ---- the plan at wrapper creation: have_cached, cached_gen, producer_gen, tag_current ---- */
    int why;
    check(nvmtl_wss_plan(0, 0, 0, 1, &why), NVMTL_WSS_PLAN_PRIVATE, "the producer never shared: private");
    check(why, NVMTL_WSS_PW_NOSHARE, "  ... no share");
    check(nvmtl_wss_plan(1, 5, 0, 1, &why), NVMTL_WSS_PLAN_PRIVATE, "a cached backing but the producer's record is gone: private");
    check(nvmtl_wss_plan(0, 0, 7, 0, &why), NVMTL_WSS_PLAN_PRIVATE, "tag stale: private, never an upload into the alias");
    check(why, NVMTL_WSS_PW_STALE, "  ... stale");
    check(nvmtl_wss_plan(1, 7, 7, 0, &why), NVMTL_WSS_PLAN_PRIVATE, "tag stale beats a cached backing");
    check(nvmtl_wss_plan(0, 0, 7, 1, &why), NVMTL_WSS_PLAN_IMPORT, "tag current, nothing cached: import");
    check(why, NVMTL_WSS_PW_NONE, "  ... no reason");
    check(nvmtl_wss_plan(1, 7, 7, 1, &why), NVMTL_WSS_PLAN_CACHED, "same generation: the cached backing, no import");
    check(nvmtl_wss_plan(1, 7, 8, 1, &why), NVMTL_WSS_PLAN_REIMPORT, "a new generation: import again");
    check(nvmtl_wss_plan(0, 0, 8, 1, NULL), NVMTL_WSS_PLAN_IMPORT, "why may be NULL");

    /* ---- the wrapper action: every (why, pgen) combination; "a3share 0 = as before" ---- */
    { const int whys[] = { NVMTL_WSS_NOT_WS, NVMTL_WSS_KILL, NVMTL_WSS_SWITCH_OFF };
      for (unsigned i = 0; i < 3; i++) {
          check(nvmtl_wss_reads_gen(whys[i]), 0, "an app / the kill switch / a3share 0 never read the surface's generation");
          check(nvmtl_wss_wrapper_action(whys[i], 0), NVMTL_WSS_WA_UNTOUCHED, "  ... untouched without a generation");
          check(nvmtl_wss_wrapper_action(whys[i], 77), NVMTL_WSS_WA_UNTOUCHED, "  ... untouched even when a stale generation sits on the surface");
      }
      for (int w = NVMTL_WSS_SESSION_OFF; w < NVMTL_WSS_N; w++) {
          check(nvmtl_wss_reads_gen(w), 1, "every other reason reads the generation");
          check(nvmtl_wss_wrapper_action(w, 0), NVMTL_WSS_WA_UNTOUCHED, "WindowServer's own surface (no generation): today's path, whatever the usage");
      }
      check(nvmtl_wss_wrapper_action(NVMTL_WSS_OK, 5), NVMTL_WSS_WA_PLAN, "shared and eligible: the plan");
      check(nvmtl_wss_wrapper_action(NVMTL_WSS_DEMOTED, 5), NVMTL_WSS_WA_PRIVATE_QUIET, "shared, demoted: private, quiet");
      check(nvmtl_wss_wrapper_action(NVMTL_WSS_SESSION_OFF, 5), NVMTL_WSS_WA_PRIVATE_QUIET, "shared, session stopped: private, quiet");
      check(nvmtl_wss_wrapper_action(NVMTL_WSS_USAGE_UNKNOWN, 5), NVMTL_WSS_WA_PRIVATE_INELIG, "shared, usage Unknown: private, counted");
      check(nvmtl_wss_wrapper_action(NVMTL_WSS_RENDER_TARGET, 5), NVMTL_WSS_WA_PRIVATE_INELIG, "shared, RenderTarget: private, counted");
      check(nvmtl_wss_wrapper_action(NVMTL_WSS_SHADER_WRITE, 5), NVMTL_WSS_WA_PRIVATE_INELIG, "shared, ShaderWrite: private, counted");
      check(nvmtl_wss_wrapper_action(NVMTL_WSS_CPU_ACCESS, 5), NVMTL_WSS_WA_PRIVATE_INELIG, "shared, CPU writer: private, counted");
      check(nvmtl_wss_wrapper_action(NVMTL_WSS_USAGE_OTHER, 5), NVMTL_WSS_WA_PRIVATE_INELIG, "shared, unknown usage bit: private, counted"); }
    check(nvmtl_wss_plan_outcome(NVMTL_WSS_PLAN_IMPORT, 0), NVMTL_WSS_PO_IMPORT, "plan import -> ask for an import");
    check(nvmtl_wss_plan_outcome(NVMTL_WSS_PLAN_REIMPORT, 0), NVMTL_WSS_PO_IMPORT, "plan re-import -> ask for an import");
    check(nvmtl_wss_plan_outcome(NVMTL_WSS_PLAN_PRIVATE, NVMTL_WSS_PW_STALE), NVMTL_WSS_PO_STALE_PRIVATE, "stale -> private, counted");
    check(nvmtl_wss_plan_outcome(NVMTL_WSS_PLAN_PRIVATE, NVMTL_WSS_PW_NOSHARE), NVMTL_WSS_PO_PRIVATE, "no share -> private");
    /* ECC review: a demoted producer's cached read-only alias is not sampled (the cache entry is dropped, the surface demoted) */
    check(nvmtl_wss_plan_demote(NVMTL_WSS_PLAN_CACHED, 1), NVMTL_WSS_PLAN_DEMOTED, "cached backing of a demoted producer: not used");
    check(nvmtl_wss_plan_demote(NVMTL_WSS_PLAN_REIMPORT, 1), NVMTL_WSS_PLAN_DEMOTED, "a re-import over the backing of a demoted producer: not asked");
    check(nvmtl_wss_plan_demote(NVMTL_WSS_PLAN_CACHED, 0), NVMTL_WSS_PLAN_CACHED, "cached backing of a healthy producer: used");
    check(nvmtl_wss_plan_demote(NVMTL_WSS_PLAN_REIMPORT, 0), NVMTL_WSS_PLAN_REIMPORT, "re-import of a healthy producer: asked");
    check(nvmtl_wss_plan_demote(NVMTL_WSS_PLAN_IMPORT, 1), NVMTL_WSS_PLAN_IMPORT, "a first import has no pid yet: decided after the kernel names it");
    check(nvmtl_wss_plan_demote(NVMTL_WSS_PLAN_PRIVATE, 1), NVMTL_WSS_PLAN_PRIVATE, "a private plan stays private");
    check(nvmtl_wss_plan_outcome(NVMTL_WSS_PLAN_DEMOTED, NVMTL_WSS_PW_NONE), NVMTL_WSS_PO_DEMOTED_PRIVATE, "demoted plan -> private without the kernel, surface demoted");
    { nvmtl_wss_state dp; nvmtl_wss_stats dps; memset(&dp, 0, sizeof dp); memset(&dps, 0, sizeof dps);
      for (unsigned k = 0; k < NVMTL_WSS_PID_HAZARD_MAX; k++) (void)nvmtl_wss_hazard(&dp, &dps, 4000 + k, 88, 1, 2);
      check(nvmtl_wss_plan_demote(NVMTL_WSS_PLAN_CACHED, nvmtl_wss_producer_demoted(&dp, 88)), NVMTL_WSS_PLAN_DEMOTED, "end to end: 4 hazards of pid 88, its cached alias is dropped");
      check(nvmtl_wss_plan_demote(NVMTL_WSS_PLAN_CACHED, nvmtl_wss_producer_demoted(&dp, 89)), NVMTL_WSS_PLAN_CACHED, "another producer's cached alias stays");
      check(nvmtl_wss_plan_demote(NVMTL_WSS_PLAN_CACHED, nvmtl_wss_producer_demoted(&dp, 0)), NVMTL_WSS_PLAN_CACHED, "pid 0 (older kernel): never demoted"); }

    /* ---- what the image creation does with a successful import ---- */
    check(nvmtl_wss_import_verdict(1, 1), NVMTL_WSS_IV_USE_RO, "planned, read-only: the alias");
    check(nvmtl_wss_import_verdict(0, 1), NVMTL_WSS_IV_DROP_RW, "planned but WRITABLE (record gone, a3ws 1): never used (card run 58)");
    check(nvmtl_wss_import_verdict(0, 0), NVMTL_WSS_IV_WRITABLE_AS_BEFORE, "not planned, writable: WindowServer's own surface, as before");
    check(nvmtl_wss_import_verdict(1, 0), NVMTL_WSS_IV_DROP_RO_UNPLANNED, "not planned but read-only: no generation on the surface");

    /* ---- the hazard also sees a seed-only move (the CPU copy-out fallback) ---- */
    { nvmtl_wss_state hs; nvmtl_wss_stats ht; memset(&hs, 0, sizeof hs); memset(&ht, 0, sizeof ht);
      check(nvmtl_wss_hazard(&hs, &ht, 9, 0, nvmtl_wss_tag_key(7, 3), nvmtl_wss_tag_key(7, 3)), 0, "same seed and generation: none");
      check(nvmtl_wss_hazard(&hs, &ht, 9, 0, nvmtl_wss_tag_key(7, 3), nvmtl_wss_tag_key(8, 3)) & NVMTL_WSS_H_HAZARD, 1, "the seed moved, the generation did not: a hazard");
      check(nvmtl_wss_hazard(&hs, &ht, 10, 0, nvmtl_wss_tag_key(7, 3), nvmtl_wss_tag_key(7, 4)) & NVMTL_WSS_H_HAZARD, 1, "the generation moved: a hazard");
      check(nvmtl_wss_tag_key(1, 2) != nvmtl_wss_tag_key(2, 1), 1, "(seed, gen) and (gen, seed) differ"); }

    /* ---- a sample ---- */
    check(nvmtl_wss_sample(1), NVMTL_WSS_SAMPLE_SKIP, "tag current: skip the upload");
    check(nvmtl_wss_sample(0), NVMTL_WSS_SAMPLE_STALE, "tag stale: no upload either");

    /* ---- the kernel's answer to the import ---- */
    check(nvmtl_wss_import_class(EPERM, 0, 1), NVMTL_WSS_IMP_ERR, "an import error");
    check(nvmtl_wss_import_class(EPERM, 1, 1), NVMTL_WSS_IMP_ERR, "an error beats flags");
    check(nvmtl_wss_import_class(0, 1, 1), NVMTL_WSS_IMP_RO, "read-only and usable");
    check(nvmtl_wss_import_class(0, 1, 0), NVMTL_WSS_IMP_RO_UNUSABLE, "read-only but the texture cannot use it (must not be written): drop it");
    check(nvmtl_wss_import_class(0, 0, 1), NVMTL_WSS_IMP_RW, "a writable import is WindowServer's own surface: no share");

    /* ---- R1: the hazard detector and demotion ---- */
    nvmtl_wss_state st; nvmtl_wss_stats s; memset(&st, 0, sizeof st); memset(&s, 0, sizeof s);
    check(nvmtl_wss_hazard(&st, &s, 40, 501, 100, 100), 0, "the generation did not move: no hazard");
    check(s.war == 0 && st.hazards == 0, 1, "no hazard counted");
    check(nvmtl_wss_hazard(&st, &s, 40, 501, 100, 101), NVMTL_WSS_H_HAZARD | NVMTL_WSS_H_DEMOTED, "it moved: hazard, the surface demoted");
    check(s.war == 1 && s.demoted == 1 && st.hazards == 1, 1, "counted sh_war, sh_demoted");
    check(nvmtl_wss_demoted(&st, 40), 1, "surface 40 is demoted");
    check(nvmtl_wss_demoted(&st, 41), 0, "surface 41 is not");
    check(nvmtl_wss_eligible(1, 0, 1, 0, nvmtl_wss_demoted(&st, 40), R, 0), NVMTL_WSS_DEMOTED, "a demoted surface takes the private path");
    check(nvmtl_wss_hazard(&st, &s, 40, 501, 101, 102), NVMTL_WSS_H_HAZARD, "the same surface again: a hazard, not demoted twice");
    check(s.demoted == 1, 1, "sh_demoted counts a surface once");
    check(nvmtl_wss_hazard(&st, &s, 41, 501, 5, 6), NVMTL_WSS_H_HAZARD | NVMTL_WSS_H_DEMOTED, "third hazard of producer 501");
    check(nvmtl_wss_producer_demoted(&st, 501), 0, "three hazards: the producer not yet");
    /* ECC security review: the 4th hazard of ONE producer demotes that producer, not the session */
    check(nvmtl_wss_hazard(&st, &s, 42, 501, 5, 6), NVMTL_WSS_H_HAZARD | NVMTL_WSS_H_DEMOTED | NVMTL_WSS_H_PRODUCER,
          "the 4th hazard of producer 501 demotes it");
    check(nvmtl_wss_producer_demoted(&st, 501) && s.pdem == 1 && !st.session_off, 1, "producer 501 demoted, sh_pdem 1, the session on");
    check(nvmtl_wss_hazard(&st, &s, 43, 501, 5, 6), NVMTL_WSS_H_HAZARD | NVMTL_WSS_H_DEMOTED, "later hazards do not demote it again");
    check(s.pdem == 1, 1, "sh_pdem counts a producer once");
    check(nvmtl_wss_producer_demoted(&st, 502), 0, "another producer keeps its share");
    check(nvmtl_wss_eligible(1, 0, 1, st.session_off, 0, R, 0), NVMTL_WSS_OK, "other producers' surfaces stay eligible");
    check(nvmtl_wss_producer_demoted(&st, 0), 0, "pid 0 (an older kernel) is never a demoted producer");
    /* pid 0: counted for the session only */
    { nvmtl_wss_state z0; nvmtl_wss_stats zs; memset(&z0, 0, sizeof z0); memset(&zs, 0, sizeof zs);
      for (uint32_t i = 0; i < 8; i++) check(nvmtl_wss_hazard(&z0, &zs, 60 + i, 0, 1, 2) & NVMTL_WSS_H_PRODUCER, 0, "pid 0 demotes no producer");
      check(z0.npid, 0, "pid 0 takes no producer entry"); }
    /* the session backstop: NVMTL_WSS_HAZARD_MAX hazards of all producers together */
    { nvmtl_wss_state bk; nvmtl_wss_stats bks; int said = 0; memset(&bk, 0, sizeof bk); memset(&bks, 0, sizeof bks);
      for (uint32_t i = 0; i < NVMTL_WSS_HAZARD_MAX - 1; i++) said |= nvmtl_wss_hazard(&bk, &bks, 1000 + i, (int32_t)(2000 + i), 1, 2) & NVMTL_WSS_H_SESSION;
      check(said == 0 && !bk.session_off, 1, "one short of the backstop: the session on");
      check(nvmtl_wss_hazard(&bk, &bks, 5000, 7000, 1, 2) & NVMTL_WSS_H_SESSION, NVMTL_WSS_H_SESSION, "the backstop stops the session");
      check(nvmtl_wss_eligible(1, 0, 1, bk.session_off, 0, R, 0), NVMTL_WSS_SESSION_OFF, "after the backstop every surface is private");
      check(nvmtl_wss_hazard(&bk, &bks, 5001, 7001, 1, 2) & NVMTL_WSS_H_SESSION, 0, "later hazards do not say 'session' again");
      check(bk.npid, NVMTL_WSS_PIDS, "the producer table stays at its size");
      check(bks.pdem, 0, "one hazard each: no producer demoted"); }
    /* a full producer table forgets a not-demoted producer first: a demoted one stays demoted */
    { nvmtl_wss_state pt; nvmtl_wss_stats ps; memset(&pt, 0, sizeof pt); memset(&ps, 0, sizeof ps);
      for (unsigned k = 0; k < NVMTL_WSS_PID_HAZARD_MAX; k++) (void)nvmtl_wss_hazard(&pt, &ps, 3000 + k, 77, 1, 2);
      check(nvmtl_wss_producer_demoted(&pt, 77), 1, "producer 77 demoted");
      for (int32_t q = 1; q <= (int32_t)NVMTL_WSS_PIDS * 3; q++) (void)nvmtl_wss_pid_slot(&pt, 100 + q, 1);
      check(nvmtl_wss_producer_demoted(&pt, 77), 1, "producer 77 still demoted after the table churned"); }
    /* the demotion log is rate limited per producer: its first 4, then every 64th */
    { nvmtl_wss_state lg; unsigned due = 0; memset(&lg, 0, sizeof lg);
      for (unsigned k = 0; k < 128; k++) due += (unsigned)nvmtl_wss_pid_log_due(&lg, 9);
      check(due, 6, "producer 9: 4 + the 64th and the 128th");
      check(nvmtl_wss_pid_log_due(&lg, 10), 1, "another producer has its own budget");
      check(nvmtl_wss_pid_log_due(&lg, 0), 1, "pid 0: always (no entry)"); }
    check(nvmtl_wss_demote(&st, &s, 0), 0, "surface 0 is never demoted");
    /* the table is bounded: the oldest is forgotten, nothing overruns */
    { nvmtl_wss_state big; nvmtl_wss_stats bs; memset(&big, 0, sizeof big); memset(&bs, 0, sizeof bs);
      for (uint32_t i = 1; i <= NVMTL_WSS_DEMOTE_MAX + 5; i++) check(nvmtl_wss_demote(&big, &bs, i), 1, "demote fills the table");
      check(big.n, NVMTL_WSS_DEMOTE_MAX, "the table stays at its size");
      check(nvmtl_wss_demoted(&big, NVMTL_WSS_DEMOTE_MAX + 5), 1, "the newest is remembered");
      check(nvmtl_wss_demoted(&big, 1), 0, "the oldest was forgotten"); }

    /* ---- R4: import errors, counted per errno ---- */
    nvmtl_wss_stats e; memset(&e, 0, sizeof e);
    nvmtl_wss_count_err(&e, EPERM); nvmtl_wss_count_err(&e, EPERM); nvmtl_wss_count_err(&e, ENOSPC); nvmtl_wss_count_err(&e, ENOENT);
    nvmtl_wss_count_err(&e, ENODEV); nvmtl_wss_count_err(&e, ENXIO); nvmtl_wss_count_err(&e, EIO);
    check(e.refused == 7 && e.e_perm == 2 && e.e_nospc == 1 && e.e_noent == 1 && e.e_nodev == 1 && e.e_nxio == 1 && e.e_other == 1, 1, "errno slots");

    /* ---- the producer's generation value ---- */
    { nvmtl_wss_gen_rec r = nvmtl_wss_gen_pack(0x123456789abcull);
      check((long long)nvmtl_wss_gen_unpack(&r, sizeof r), 0x123456789abcll, "the generation round-trips (64 bit)");
      check((long long)nvmtl_wss_gen_unpack(&r, sizeof r - 1), 0, "a short value is nothing");
      check((long long)nvmtl_wss_gen_unpack(NULL, sizeof r), 0, "no value is nothing");
      r.magic ^= 1u;
      check((long long)nvmtl_wss_gen_unpack(&r, sizeof r), 0, "a foreign value is nothing"); }

    /* ---- the SUMMARY tail ---- */
    { nvmtl_wss_stats z; char buf[512]; memset(&z, 0, sizeof z);
      check(nvmtl_wss_events(&z), 0, "no events");
      check(nvmtl_wss_format(&z, 0, buf, sizeof buf), 1, "the tail is written even with zeros (the consumer ran)");
      check(strstr(buf, "sh_imp=0 sh_skip=0 sh_stale=0 sh_reimp=0 sh_refused=0 sh_war=0 sh_demoted=0 ") == buf, 1, "the order the brief lists");
      z.imp = 3; z.skip = 100; z.stale = 2; z.reimp = 1; z.war = 1; z.demoted = 1; z.wr = 4; z.inelig = 6; z.rw = 2; z.e_perm = 2; z.refused = 2;
      check(nvmtl_wss_events(&z) > 0, 1, "events");
      nvmtl_wss_format(&z, 1, buf, sizeof buf);
      check(strstr(buf, "sh_imp=3 sh_skip=100 sh_stale=2 sh_reimp=1 sh_refused=2 sh_war=1 sh_demoted=1 sh_wr=4 sh_inelig=6 sh_rw=2 sh_eperm=2 ") == buf, 1, "values");
      check(strstr(buf, "sh_off=1") != NULL, 1, "sh_off=1 after the session stop");
      z.pdem = 2; nvmtl_wss_format(&z, 1, buf, sizeof buf);
      check(strstr(buf, " sh_pdem=2") != NULL, 1, "sh_pdem: producers demoted");
      check(nvmtl_wss_format(&z, 0, buf, 0), 0, "no room: nothing"); }

    printf("test_ws_share: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
