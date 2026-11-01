/*
 * nvmtl_gsub_empty.h - what the G-SUB submit-ahead pass does with a submission part that has nothing to run (DRIVER-PLAN N70).
 *
 * -[NVMTLCommandBuffer nvmtlSubmitAhead] walks a command buffer's parts in order; a part with no encoder, no resource, no surface and no
 * scratch (an event-split remainder) is not submitted: with NVMTL_EVFOLD it is retired on the spot and the pass goes on. Upstream retired it
 * only when it was not the last part, but every command buffer that ends after a split has an empty last part, so the pass returned NO there
 * and -nvmtlSubmitAhead's caller (nvmtl_gsub_ahead_pass1) stopped at that buffer: later buffers ran submit + fence wait in turn. Plain C, so
 * tests/test_gsub_empty.c checks the decision exactly as NVMTLEventSync.m uses it.
 */
#ifndef NVMTL_GSUB_EMPTY_H
#define NVMTL_GSUB_EMPTY_H

enum {
    NVMTL_GSUB_EMPTY_RETIRE = 0,        /* retire the part ahead (not the last), go on with the next part */
    NVMTL_GSUB_EMPTY_RETIRE_LAST,       /* retire the LAST part ahead: the buffer is fully in flight, the pass goes on to the next buffer (N70) */
    NVMTL_GSUB_EMPTY_STOP_EVFOLD,       /* stop the pass: NVMTL_EVFOLD=0 (a signal is only seen by the CPU, so nothing may run ahead of it) */
    NVMTL_GSUB_EMPTY_STOP_LAST          /* stop the pass: last part and NVMTL_GSUB_LAST_EMPTY=0 (the old behaviour) */
};

/* `evfold_on` NVMTL_EVFOLD, `is_last` this part is the buffer's last, `last_empty_on` NVMTL_GSUB_LAST_EMPTY (default on). */
static inline int nvmtl_gsub_empty_verdict(int evfold_on, int is_last, int last_empty_on)
{
    if (!evfold_on) return NVMTL_GSUB_EMPTY_STOP_EVFOLD;
    if (is_last) return last_empty_on ? NVMTL_GSUB_EMPTY_RETIRE_LAST : NVMTL_GSUB_EMPTY_STOP_LAST;
    return NVMTL_GSUB_EMPTY_RETIRE;
}

#endif
