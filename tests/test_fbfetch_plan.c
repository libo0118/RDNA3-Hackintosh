/* Metal framebuffer fetch decisions (metal/plugin/nvmtl_fbfetch_plan.h): which pipelines are served, which attachments can be inputs,
 * the render-pass variant key, the pInputAttachments array, the restart rule.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "nvmtl_fbfetch_plan.h"
#include "nvmtl_log_cap.h"
#include "nvmtl_log_failure.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- the plugin log's per-site cap (nvmtl_log_cap.h), as nvlog applies it ---- */
static nvmtl_log_site g_sites[NVLOG_SITES];
static char g_log[1 << 16];
static void sim_nvlog(const char *fmt, ...)
{
    uint64_t rep;
    if (nvmtl_log_cap_dropped(g_sites, NVLOG_SITES, fmt, &rep)) return;
    va_list a; va_start(a, fmt);
    size_t n = strlen(g_log); vsnprintf(g_log + n, sizeof g_log - n, fmt, a); va_end(a);
    strncat(g_log, "\n", sizeof g_log - strlen(g_log) - 1);
}
/* what the code did BEFORE the review: every reason through one literal */
static void shared_site(unsigned why, uint32_t z, uint32_t sv)
{ sim_nvlog("vk: fbfetch: reason %u: reads of colour attachment(s) 0x%x are zeroed, 0x%x are served", why, z, sv); }
#define SIM_REASON(why) NVMTL_FBF_LOG_REASON(sim_nvlog, why, 1u, 0u, 1u)

int main(void)
{
    unsigned why; uint32_t sv, zr;
    nvmtl_fbf_in in = { .read_mask = 1, .ncol = 1, .cfmt_present = 1, .cfmt_int = 0, .samples = 1, .layout_ok = 1 };
#define PLAN() nvmtl_fbf_plan(&in, &sv, &zr, &why)
    PLAN(); CHECK(sv == 1 && zr == 0 && why == NVMTL_FBF_OK);
    /* nothing read: nothing to serve or zero, no reason */
    in.read_mask = 0; PLAN(); CHECK(sv == 0 && zr == 0 && why == NVMTL_FBF_OK); in.read_mask = 1;
    /* MRT: reads of slots 0 and 2 */
    in.ncol = 3; in.cfmt_present = 7; in.read_mask = 5; PLAN(); CHECK(sv == 5 && zr == 0 && why == NVMTL_FBF_OK);
    /* the card run's Terminal: the module reads colour(0..2) but the pipeline has ONE colour attachment: slot 0 served, 1 and 2 zeroed */
    in.ncol = 1; in.cfmt_present = 1; in.read_mask = 7; PLAN(); CHECK(sv == 1 && zr == 6 && why == NVMTL_FBF_NO_ATTACHMENT);
    /* a hole in the pipeline's formats; beyond ncol; beyond 8 */
    in.ncol = 3; in.cfmt_present = 5; in.read_mask = 3; PLAN(); CHECK(sv == 1 && zr == 2 && why == NVMTL_FBF_NO_ATTACHMENT);
    in.cfmt_present = 7; in.ncol = 2; in.read_mask = 4; PLAN(); CHECK(sv == 0 && zr == 4 && why == NVMTL_FBF_NO_ATTACHMENT);
    in.ncol = 3; in.read_mask = 1u << 9; PLAN(); CHECK(sv == 0 && zr == 0);                      /* bit 9 is not a binding of the band: ignored */
    /* an integer slot is zeroed, the float one is served */
    in.read_mask = 3; in.cfmt_int = 2; PLAN(); CHECK(sv == 1 && zr == 2 && why == NVMTL_FBF_INTEGER_FORMAT);
    in.cfmt_int = 0;
    /* global reasons zero every read: multisample, no layout, forced zero, tessellation */
    in.read_mask = 3; in.samples = 4; PLAN(); CHECK(sv == 0 && zr == 3 && why == NVMTL_FBF_MULTISAMPLE); in.samples = 1;
    in.layout_ok = 0; PLAN(); CHECK(sv == 0 && zr == 3 && why == NVMTL_FBF_NO_LAYOUT); in.layout_ok = 1;
    in.forced_zero = 1; PLAN(); CHECK(sv == 0 && zr == 3 && why == NVMTL_FBF_FORCED_ZERO); in.forced_zero = 0;
    in.tess = 1; PLAN(); CHECK(sv == 0 && zr == 3 && why == NVMTL_FBF_TESS); in.tess = 0;
    in.read_mask = 1; nvmtl_fbf_plan(&in, NULL, NULL, NULL); PLAN(); CHECK(sv == 1);
    for (unsigned r = 0; r < NVMTL_FBF_REASONS; r++) CHECK(nvmtl_fbf_reason_text(r)[0]);
    CHECK(!strcmp(nvmtl_fbf_reason_text(999), "?"));

    /* attachment facts */
    CHECK(nvmtl_fbf_attachment_reason(1, 1, 2, 1) == NVMTL_FBF_OK);
    CHECK(nvmtl_fbf_attachment_reason(1, 0, 3, 1) == NVMTL_FBF_OK);              /* a slice of a 2D array: a 2D view */
    CHECK(nvmtl_fbf_attachment_reason(4, 1, 4, 1) == NVMTL_FBF_MULTISAMPLE);
    CHECK(nvmtl_fbf_attachment_reason(1, 6, 3, 1) == NVMTL_FBF_LAYERED);
    CHECK(nvmtl_fbf_attachment_reason(1, 1, 7, 1) == NVMTL_FBF_VOLUME);
    CHECK(nvmtl_fbf_attachment_reason(1, 1, 2, 0) == NVMTL_FBF_NO_USAGE);

    /* variant key */
    CHECK(nvmtl_fbf_rp_key(0, 0) == 0 && nvmtl_fbf_rp_key(3, 0) == 3);           /* no input attachment: the plain key (legacy slots keep working) */
    CHECK(nvmtl_fbf_rp_key(0x3ff, 0x81) == (0x3ffu | (0x81u << 16)));
    CHECK(nvmtl_fbf_key_load(nvmtl_fbf_rp_key(0x3ff, 0x81)) == 0x3ff && nvmtl_fbf_key_fbin(nvmtl_fbf_rp_key(0x3ff, 0x81)) == 0x81);
    CHECK(nvmtl_fbf_rp_key(0, 1) != nvmtl_fbf_rp_key(0, 2) && nvmtl_fbf_rp_key(1, 1) != nvmtl_fbf_rp_key(1, 0));
    CHECK(nvmtl_fbf_rp_key(0x10000u | 5u, 0) == 5u);                              /* load bits never leak into the fbin field */

    /* pInputAttachments: slot 1 has no attachment (index UNUSED = ~0) */
    { const uint32_t U = ~0u, att[4] = { 0, U, 1, 2 }; uint32_t out[8];
      CHECK(nvmtl_fbf_input_refs(0, 4, att, U, out) == 0 && out[0] == U && out[2] == U);
      CHECK(nvmtl_fbf_input_refs(1, 4, att, U, out) == 1 && out[0] == 0 && out[1] == U);
      CHECK(nvmtl_fbf_input_refs(0xf, 4, att, U, out) == 4 && out[0] == 0 && out[1] == U && out[2] == 1 && out[3] == 2);
      CHECK(nvmtl_fbf_input_refs(2, 4, att, U, out) == 0 && out[1] == U);             /* bit set for a slot without attachment: nothing */
      CHECK(nvmtl_fbf_input_refs(0x100, 4, att, U, out) == 0);                       /* bit beyond ncol ignored */
      CHECK(nvmtl_fbf_input_refs(4, 4, att, U, out) == 3 && out[2] == 1); }

    /* restart rule and the pass's mask */
    CHECK(nvmtl_fbf_missing(0, 1) == 1 && nvmtl_fbf_missing(1, 1) == 0 && nvmtl_fbf_missing(3, 1) == 0 && nvmtl_fbf_missing(1, 3) == 2 && nvmtl_fbf_missing(5, 0) == 0);
    CHECK(nvmtl_fbf_pass_fbin(0, 1, 1, 1) == 1 && nvmtl_fbf_pass_fbin(2, 1, 3, 1) == 3 && nvmtl_fbf_pass_fbin(2, 1, 1, 1) == 1);
    CHECK(nvmtl_fbf_pass_fbin(3, 3, 3, 4) == 0);                                  /* multisample: never */
    CHECK(nvmtl_fbf_pass_fbin(0x1ff, 0, 0x1ff, 1) == 0xff);
    /* a restart never ping-pongs: the union covers both pipelines */
    { uint32_t fbin = 1; const uint32_t a = 1, b = 2;
      CHECK(nvmtl_fbf_missing(fbin, b) == 2); fbin = nvmtl_fbf_pass_fbin(fbin, b, 3, 1);
      CHECK(!nvmtl_fbf_missing(fbin, a) && !nvmtl_fbf_missing(fbin, b)); }
    /* the cap: 8 per literal, then every 4096th */
    { uint64_t rep; nvmtl_log_site t[4]; memset(t, 0, sizeof t); static const char A[] = "a", B[] = "b"; int passed = 0;
      for (int i = 0; i < 10000; i++) { if (!nvmtl_log_cap_dropped(t, 4, A, &rep)) { passed++; CHECK(i < 8 || (i + 1) % 4096 == 0); } }
      CHECK(passed == 8 + 2); CHECK(!nvmtl_log_cap_dropped(t, 4, B, &rep) && rep == 0);   /* another literal is unaffected */
      nvmtl_log_site one[1] = { { 0, 0 } }; static const char C[] = "c";
      CHECK(!nvmtl_log_cap_dropped(one, 1, C, &rep)); CHECK(!nvmtl_log_cap_dropped(one, 1, B, &rep)); } /* a full table never drops */
    /* the reviewed failure: the benign reason floods its (shared) literal and the real one is gone ... */
    memset(g_sites, 0, sizeof g_sites); g_log[0] = 0;
    for (int i = 0; i < 40; i++) shared_site(NVMTL_FBF_NO_ATTACHMENT, 6, 1);
    shared_site(NVMTL_FBF_MULTISAMPLE, 1, 0);
    CHECK(!strstr(g_log, "reason 2:"));
    /* ... with a literal per reason (the macro the plugin uses) it is not */
    memset(g_sites, 0, sizeof g_sites); g_log[0] = 0;
    for (int i = 0; i < 40; i++) SIM_REASON(NVMTL_FBF_NO_ATTACHMENT);
    SIM_REASON(NVMTL_FBF_MULTISAMPLE); SIM_REASON(NVMTL_FBF_INTEGER_FORMAT); SIM_REASON(NVMTL_FBF_TESS);
    SIM_REASON(NVMTL_FBF_NO_LAYOUT); SIM_REASON(NVMTL_FBF_FORCED_ZERO); SIM_REASON(99);
    { int benign = 0, unsup = 0; for (const char *q = g_log; (q = strstr(q, "reads of attachments the pipeline lacks")); q++) benign++;
      for (const char *q = g_log; (q = strstr(q, "UNSUPPORTED (")); q++) unsup++;
      CHECK(benign == 8); CHECK(unsup == 6);
      CHECK(strstr(g_log, "UNSUPPORTED (a multisample target") && strstr(g_log, "UNSUPPORTED (an integer colour format") && strstr(g_log, "UNSUPPORTED (a tessellation pipeline")
            && strstr(g_log, "UNSUPPORTED (no input-attachment descriptor layout") && strstr(g_log, "UNSUPPORTED (NVMTL_FBFETCH_ZERO=1") && strstr(g_log, "reason code 99")); }
    /* every literal of the macro is its own site (a compiler may merge equal literals): 7 reasons -> 7 sites */
    { memset(g_sites, 0, sizeof g_sites); g_log[0] = 0;
      SIM_REASON(NVMTL_FBF_NO_ATTACHMENT); SIM_REASON(NVMTL_FBF_MULTISAMPLE); SIM_REASON(NVMTL_FBF_INTEGER_FORMAT); SIM_REASON(NVMTL_FBF_TESS);
      SIM_REASON(NVMTL_FBF_NO_LAYOUT); SIM_REASON(NVMTL_FBF_FORCED_ZERO); SIM_REASON(99);
      int sites = 0; for (int i = 0; i < NVLOG_SITES; i++) if (g_sites[i].fmt) { sites++; CHECK(g_sites[i].n == 1); }
      CHECK(sites == 7); }
    /* reasons are ranked: the real limit wins over the normal one in any order */
    { nvmtl_fbf_in x = { .read_mask = 3, .ncol = 2, .cfmt_present = 3, .cfmt_int = 1, .samples = 1, .layout_ok = 1 };
      uint32_t s2, z2; unsigned w2;
      x.cfmt_present = 1; x.cfmt_int = 0; nvmtl_fbf_plan(&x, &s2, &z2, &w2); CHECK(s2 == 1 && z2 == 2 && w2 == NVMTL_FBF_NO_ATTACHMENT);
      x.cfmt_present = 3; x.cfmt_int = 2; x.read_mask = 3; nvmtl_fbf_plan(&x, &s2, &z2, &w2); CHECK(w2 == NVMTL_FBF_INTEGER_FORMAT);
      x.ncol = 2; x.cfmt_present = 2; x.cfmt_int = 2; x.read_mask = 3;                /* slot 0 missing (benign) first in mask order, slot 1 integer (real) */
      nvmtl_fbf_plan(&x, &s2, &z2, &w2); CHECK(s2 == 0 && z2 == 3 && w2 == NVMTL_FBF_INTEGER_FORMAT);
      CHECK(nvmtl_fbf_reason_rank(NVMTL_FBF_NO_ATTACHMENT) < nvmtl_fbf_reason_rank(NVMTL_FBF_FORCED_ZERO)
            && nvmtl_fbf_reason_rank(NVMTL_FBF_FORCED_ZERO) < nvmtl_fbf_reason_rank(NVMTL_FBF_MULTISAMPLE)
            && nvmtl_fbf_reason_rank(NVMTL_FBF_MULTISAMPLE) < nvmtl_fbf_reason_rank(NVMTL_FBF_NO_LAYOUT)); }
    /* N64: the barrier before a reading draw: an image barrier unless the switch asks for the old one or an image is missing */
    CHECK(nvmtl_fbf_barrier_mode(0, 0x1, 0x1) == NVMTL_FBF_BAR_IMAGE);
    CHECK(nvmtl_fbf_barrier_mode(0, 0x5, 0xff) == NVMTL_FBF_BAR_IMAGE);
    CHECK(nvmtl_fbf_barrier_mode(1, 0x1, 0x1) == NVMTL_FBF_BAR_MEM_KNOB);
    CHECK(nvmtl_fbf_barrier_mode(0, 0x3, 0x1) == NVMTL_FBF_BAR_MEM_NOIMAGE);
    CHECK(nvmtl_fbf_barrier_mode(0, 0, 0xff) == NVMTL_FBF_BAR_MEM_NOIMAGE);
    CHECK(nvmtl_fbf_barrier_mode(0, 0x100, 0x1ff) == NVMTL_FBF_BAR_MEM_NOIMAGE);   /* only slots 0..7 exist */
    CHECK(!nvmtl_fbf_membar_knob(NULL) && !nvmtl_fbf_membar_knob("") && !nvmtl_fbf_membar_knob("0") && !nvmtl_fbf_membar_knob("0x"));
    CHECK(nvmtl_fbf_membar_knob("1") && nvmtl_fbf_membar_knob("yes"));
    /* the release-build syslog filter keeps the new lines */
    CHECK(nvmtl_log_failure("vk: fbfetch: UNSUPPORTED (a multisample target): reads"));
    CHECK(nvmtl_log_failure("vk: fbfetch: a draw that reads [[color(n)]] 0x1 is SKIPPED: no set"));
    CHECK(nvmtl_log_failure("vk: fbfetch: a pipeline that reads [[color(n)]] 0x1 was bound ... RESTARTED (LOAD)"));
    CHECK(nvmtl_log_failure("vk: fbfetch: ZEROING FAILED for a fragment module"));
    CHECK(nvmtl_log_failure("reads of colour attachment(s) 0x6 are zeroed, 0x1 are served"));
    CHECK(!nvmtl_log_failure("vk: fbfetch: draw 1 reads [[color(n)]] 0x1: barrier + set 3 bound (pass inputs 0x1)"));
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("test_fbfetch_plan: ok");
    return 0;
}
