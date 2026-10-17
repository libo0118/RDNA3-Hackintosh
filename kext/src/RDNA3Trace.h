/*
 * RDNA3Trace.h - the core kext's half of the per-frame trace (card run
 * 2026-10-17b, common/rdna3_trace.h): RADV's command submissions, the
 * fences they produce, the waits on them, hang recoveries and GPU page
 * faults, each with mach_absolute_time, so the Metal plugin's trace and the
 * accelerator's swaps line up with what the GPU did. Read as one blob with
 * `sysctl -b debug.rdna3_drmtrace` (root only; debug.rdna3_drmtrace_stream:
 * the events since the last stream read). Implemented in RDNA3Drm.cpp.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_TRACE_KEXT_H
#define RDNA3_TRACE_KEXT_H

#include "rdna3_trace.h"

/* records one event (its seq and time are set here); any thread, also
 * with interrupts off */
void rdna3TraceCore(const struct rd3_trace_ev *e);
/* the ring and its sysctls: made by drmInit, taken down by drmFree */
bool rdna3TraceCoreInit(void);
void rdna3TraceCoreFini(void);

#endif
