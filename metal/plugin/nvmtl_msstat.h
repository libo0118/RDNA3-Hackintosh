/*
 * Counters and SUMMARY lines of the multisample copy passes (nvmtl_msdl.h download, nvmtl_msupload.h upload; DRIVER-PLAN N52).
 *
 * Why: the plugin log keeps the first 8 calls of each FORMAT LITERAL (nvmtl_log_cap.h), so one literal shared by every aspect, format and
 * failure kind lets a benign kind (a colour copy, a format-97 refusal) use up the slots of the one the report needs (Chess's stencil
 * download, its R8_UINT upload). The totals therefore go out on a time schedule as `msdl: SUMMARY ...` / `msupload: SUMMARY ...`
 * (nvlog_uncapped, key=value, the newest line of a process counts; tools/triage_n48.py reads them): done per aspect / per view format
 * and layout, refusals and failures per kind. The distinct REASON of a refusal is logged at its 1st, 2nd, 4th, 8th ... occurrence.
 *
 * Pure C99 (the caller owns the atomics and passes plain arrays): tests/test_msdl.c. SPDX-License-Identifier: MIT
 */
#ifndef NVMTL_MSSTAT_H
#define NVMTL_MSSTAT_H
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* why a copy was not made, no failure of the plugin's own */
enum { NVMTL_MSS_R_PLAN, NVMTL_MSS_R_INRP, NVMTL_MSS_R_NODEV, NVMTL_MSS_R_NOADDR, NVMTL_MSS_R_NOUSAGE, NVMTL_MSS_R_PIPE, NVMTL_MSS_R_N };
/* what failed while building or recording the pass */
enum { NVMTL_MSS_F_SETLAYOUT, NVMTL_MSS_F_PIPELAYOUT, NVMTL_MSS_F_RENDERPASS, NVMTL_MSS_F_MODULE, NVMTL_MSS_F_PIPELINE, NVMTL_MSS_F_TABLE,
       NVMTL_MSS_F_VIEW, NVMTL_MSS_F_RETIRE, NVMTL_MSS_F_SET, NVMTL_MSS_F_FRAMEBUFFER, NVMTL_MSS_F_N };
/* aspects of a download: colour / depth / stencil */
enum { NVMTL_MSS_A_COLOUR, NVMTL_MSS_A_DEPTH, NVMTL_MSS_A_STENCIL, NVMTL_MSS_A_N };
/* the view formats the upload pass can ask for (nvmtl_msupload.h), 13 = R8_UINT is Chess's stencil-resolve texture */
#define NVMTL_MSS_NFMT 9
static const uint32_t nvmtl_mss_fmts[NVMTL_MSS_NFMT] = { 13, 20, 41, 74, 98, 44, 37, 9, 16 };
static inline int nvmtl_mss_fmt_index(uint32_t fmt)
{
    for (int i = 0; i < NVMTL_MSS_NFMT; i++) if (nvmtl_mss_fmts[i] == fmt) return i;
    return -1;
}
static inline int nvmtl_mss_pow2(unsigned k) { return k && !(k & (k - 1)); }
static inline unsigned nvmtl_mss_sum(const unsigned *v, int n) { unsigned s = 0; for (int i = 0; i < n; i++) s += v[i]; return s; }

/* The distinct reasons of plan refusals: a small table keyed by the reason text (the plans return static strings). Returns how many times
 * THIS reason has been seen including this one; a full table counts the surplus under "(other reasons)". Not thread-safe: the caller locks. */
typedef struct { const char *why; unsigned n; } nvmtl_mss_reason;
#define NVMTL_MSS_REASONS 16
static inline unsigned nvmtl_mss_reason_count(nvmtl_mss_reason *tab, const char *why)
{
    int i = 0;
    for (; i < NVMTL_MSS_REASONS - 1 && tab[i].why; i++)
        if (tab[i].why == why || !strcmp(tab[i].why, why)) return ++tab[i].n;
    if (i < NVMTL_MSS_REASONS - 1) { tab[i].why = why; return ++tab[i].n; }
    if (!tab[i].why) tab[i].why = "(other reasons)";
    return ++tab[i].n;
}

/* `msdl: SUMMARY ...`: returns the length written (never more than n - 1) */
static inline int nvmtl_msdl_summary_text(char *out, size_t n, const unsigned done[NVMTL_MSS_A_N], const unsigned ref[NVMTL_MSS_R_N],
                                          const unsigned fail[NVMTL_MSS_F_N], unsigned resolve_r8u, unsigned resolve_other)
{
    return snprintf(out, n,
        "msdl: SUMMARY done=%u d_colour=%u d_depth=%u d_stencil=%u refused=%u r_plan=%u r_inrp=%u r_nodev=%u r_noaddr=%u r_nousage=%u r_pipe=%u "
        "failed=%u f_setlayout=%u f_pipelayout=%u f_module=%u f_pipeline=%u f_view=%u f_retire=%u f_set=%u resolve_r8u=%u resolve_other=%u",
        nvmtl_mss_sum(done, NVMTL_MSS_A_N), done[NVMTL_MSS_A_COLOUR], done[NVMTL_MSS_A_DEPTH], done[NVMTL_MSS_A_STENCIL],
        nvmtl_mss_sum(ref, NVMTL_MSS_R_N), ref[NVMTL_MSS_R_PLAN], ref[NVMTL_MSS_R_INRP], ref[NVMTL_MSS_R_NODEV], ref[NVMTL_MSS_R_NOADDR],
        ref[NVMTL_MSS_R_NOUSAGE], ref[NVMTL_MSS_R_PIPE],
        nvmtl_mss_sum(fail, NVMTL_MSS_F_N), fail[NVMTL_MSS_F_SETLAYOUT], fail[NVMTL_MSS_F_PIPELAYOUT], fail[NVMTL_MSS_F_MODULE], fail[NVMTL_MSS_F_PIPELINE],
        fail[NVMTL_MSS_F_VIEW], fail[NVMTL_MSS_F_RETIRE], fail[NVMTL_MSS_F_SET], resolve_r8u, resolve_other);
}
/* `msupload: SUMMARY ...`: up_inter_F / up_flat_F per view format F, only the non-zero ones */
static inline int nvmtl_msupload_summary_text(char *out, size_t n, const unsigned up_inter[NVMTL_MSS_NFMT], const unsigned up_flat[NVMTL_MSS_NFMT],
                                              unsigned up_uint, unsigned up_wide, const unsigned ref[NVMTL_MSS_R_N], const unsigned fail[NVMTL_MSS_F_N])
{
    const unsigned inter = nvmtl_mss_sum(up_inter, NVMTL_MSS_NFMT), flat = nvmtl_mss_sum(up_flat, NVMTL_MSS_NFMT);
    int len = snprintf(out, n,
        "msupload: SUMMARY done=%u done_int=%u inter=%u flat=%u wide=%u refused=%u r_plan=%u r_inrp=%u r_nodev=%u r_noaddr=%u r_pipe=%u "
        "failed=%u f_pipelayout=%u f_renderpass=%u f_module=%u f_pipeline=%u f_table=%u f_view=%u f_framebuffer=%u f_retire=%u",
        inter + flat, up_uint, inter, flat, up_wide, nvmtl_mss_sum(ref, NVMTL_MSS_R_N), ref[NVMTL_MSS_R_PLAN], ref[NVMTL_MSS_R_INRP], ref[NVMTL_MSS_R_NODEV],
        ref[NVMTL_MSS_R_NOADDR], ref[NVMTL_MSS_R_PIPE], nvmtl_mss_sum(fail, NVMTL_MSS_F_N), fail[NVMTL_MSS_F_PIPELAYOUT], fail[NVMTL_MSS_F_RENDERPASS],
        fail[NVMTL_MSS_F_MODULE], fail[NVMTL_MSS_F_PIPELINE], fail[NVMTL_MSS_F_TABLE], fail[NVMTL_MSS_F_VIEW], fail[NVMTL_MSS_F_FRAMEBUFFER], fail[NVMTL_MSS_F_RETIRE]);
    for (int i = 0; i < NVMTL_MSS_NFMT && len > 0 && (size_t)len < n; i++) {
        if (up_inter[i]) len += snprintf(out + len, n - (size_t)len, " up_inter_%u=%u", nvmtl_mss_fmts[i], up_inter[i]);
        if (len > 0 && (size_t)len < n && up_flat[i]) len += snprintf(out + len, n - (size_t)len, " up_flat_%u=%u", nvmtl_mss_fmts[i], up_flat[i]);
    }
    return len;
}
#endif
