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

    /* ---- what the image creation does with a successful import ---- */
    check(nvmtl_wss_import_verdict(1, 1), NVMTL_WSS_IV_USE_RO, "planned, read-only: the alias");
    check(nvmtl_wss_import_verdict(0, 1), NVMTL_WSS_IV_DROP_RW, "planned but WRITABLE (record gone, a3ws 1): never used (card run 58)");
    check(nvmtl_wss_import_verdict(0, 0), NVMTL_WSS_IV_WRITABLE_AS_BEFORE, "not planned, writable: WindowServer's own surface, as before");
    check(nvmtl_wss_import_verdict(1, 0), NVMTL_WSS_IV_DROP_RO_UNPLANNED, "not planned but read-only: no generation on the surface");

    /* ---- the hazard also sees a seed-only move (the CPU copy-out fallback) ---- */
    { nvmtl_wss_state hs; nvmtl_wss_stats ht; memset(&hs, 0, sizeof hs); memset(&ht, 0, sizeof ht);
      check(nvmtl_wss_hazard(&hs, &ht, 9, nvmtl_wss_tag_key(7, 3), nvmtl_wss_tag_key(7, 3)), 0, "same seed and generation: none");
      check(nvmtl_wss_hazard(&hs, &ht, 9, nvmtl_wss_tag_key(7, 3), nvmtl_wss_tag_key(8, 3)) & NVMTL_WSS_H_HAZARD, 1, "the seed moved, the generation did not: a hazard");
      check(nvmtl_wss_hazard(&hs, &ht, 10, nvmtl_wss_tag_key(7, 3), nvmtl_wss_tag_key(7, 4)) & NVMTL_WSS_H_HAZARD, 1, "the generation moved: a hazard");
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
    check(nvmtl_wss_hazard(&st, &s, 40, 100, 100), 0, "the generation did not move: no hazard");
    check(s.war == 0 && st.hazards == 0, 1, "no hazard counted");
    check(nvmtl_wss_hazard(&st, &s, 40, 100, 101), NVMTL_WSS_H_HAZARD | NVMTL_WSS_H_DEMOTED, "it moved: hazard, the surface demoted");
    check(s.war == 1 && s.demoted == 1 && st.hazards == 1, 1, "counted sh_war, sh_demoted");
    check(nvmtl_wss_demoted(&st, 40), 1, "surface 40 is demoted");
    check(nvmtl_wss_demoted(&st, 41), 0, "surface 41 is not");
    check(nvmtl_wss_eligible(1, 0, 1, 0, nvmtl_wss_demoted(&st, 40), R, 0), NVMTL_WSS_DEMOTED, "a demoted surface takes the private path");
    check(nvmtl_wss_hazard(&st, &s, 40, 101, 102), NVMTL_WSS_H_HAZARD, "the same surface again: a hazard, not demoted twice");
    check(s.demoted == 1, 1, "sh_demoted counts a surface once");
    check(nvmtl_wss_hazard(&st, &s, 41, 5, 6), NVMTL_WSS_H_HAZARD | NVMTL_WSS_H_DEMOTED, "third hazard");
    check(st.session_off, 0, "three hazards: not yet");
    check(nvmtl_wss_hazard(&st, &s, 42, 5, 6), NVMTL_WSS_H_HAZARD | NVMTL_WSS_H_DEMOTED | NVMTL_WSS_H_SESSION, "the 4th hazard stops the session");
    check(st.session_off, 1, "session_off");
    check(nvmtl_wss_hazard(&st, &s, 43, 5, 6), NVMTL_WSS_H_HAZARD | NVMTL_WSS_H_DEMOTED, "later hazards do not say 'session' again");
    check(nvmtl_wss_eligible(1, 0, 1, st.session_off, 0, R, 0), NVMTL_WSS_SESSION_OFF, "after the 4th every surface is private");
    check(s.war == 5, 1, "five hazards counted");
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
      check(nvmtl_wss_format(&z, 0, buf, 0), 0, "no room: nothing"); }

    printf("test_ws_share: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
