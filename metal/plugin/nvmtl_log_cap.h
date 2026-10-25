/*
 * The plugin log's per-site cap, as a pure function (NVMTLDevice.m nvlog; tests/test_fbfetch_plan.c simulates it).
 *
 * nvlog() lets the first NVLOG_VERBATIM calls of each FORMAT-STRING SITE (the pointer of the literal, not the text) through, then only
 * every NVLOG_EVERY-th. Two consequences for code that logs several kinds of event: (1) every kind needs a format literal of its own, or
 * a flood of one kind (a benign one) uses up the slots of a rare, serious one; (2) an app-level gate such as `k <= 8 || k % 256 == 0`
 * cannot log more than the cap allows. Totals that must reach the report go through nvlog_uncapped() on a time schedule.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NVMTL_LOG_CAP_H
#define NVMTL_LOG_CAP_H
#include <stdint.h>
#include <stddef.h>

#define NVLOG_SITES    256
#define NVLOG_VERBATIM 8
#define NVLOG_EVERY    4096

typedef struct { const char *fmt; uint64_t n; } nvmtl_log_site;

/* Returns 1 when this call is dropped; *rep is set to the running count when it is one of the periodic ones (0 otherwise).
 * The caller holds the lock around the table. A full table lets everything through (a site that cannot be tracked is never dropped). */
static inline int nvmtl_log_cap_dropped(nvmtl_log_site *sites, size_t nsites, const char *fmt, uint64_t *rep)
{
    *rep = 0;
    size_t i = 0;
    for (; i < nsites; i++) {
        if (sites[i].fmt == fmt) break;
        if (!sites[i].fmt) { sites[i].fmt = fmt; break; }
    }
    if (i == nsites) return 0;
    const uint64_t n = ++sites[i].n;
    if (n <= NVLOG_VERBATIM) return 0;
    if (n % NVLOG_EVERY == 0) { *rep = n; return 0; }
    return 1;
}
#endif
