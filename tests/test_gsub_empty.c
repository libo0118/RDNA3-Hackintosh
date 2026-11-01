/* The ahead pass's decision for an empty submission part (metal/plugin/nvmtl_gsub_empty.h, DRIVER-PLAN N70).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "nvmtl_gsub_empty.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    /* EVFOLD off: every empty part stops the pass, last or not, whatever the N70 switch says */
    for (int last = 0; last < 2; last++) for (int sw = 0; sw < 2; sw++)
        CHECK(nvmtl_gsub_empty_verdict(0, last, sw) == NVMTL_GSUB_EMPTY_STOP_EVFOLD);
    /* a part that is not the last is retired ahead, the N70 switch does not matter */
    CHECK(nvmtl_gsub_empty_verdict(1, 0, 1) == NVMTL_GSUB_EMPTY_RETIRE);
    CHECK(nvmtl_gsub_empty_verdict(1, 0, 0) == NVMTL_GSUB_EMPTY_RETIRE);
    /* N70: the last part is retired too (default); NVMTL_GSUB_LAST_EMPTY=0 restores the old stop */
    CHECK(nvmtl_gsub_empty_verdict(1, 1, 1) == NVMTL_GSUB_EMPTY_RETIRE_LAST);
    CHECK(nvmtl_gsub_empty_verdict(1, 1, 0) == NVMTL_GSUB_EMPTY_STOP_LAST);
    /* the verdicts that let the pass go on are exactly the two RETIRE ones (callers test `<= RETIRE_LAST`) */
    CHECK(NVMTL_GSUB_EMPTY_RETIRE < NVMTL_GSUB_EMPTY_RETIRE_LAST && NVMTL_GSUB_EMPTY_RETIRE_LAST < NVMTL_GSUB_EMPTY_STOP_EVFOLD);
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    puts("gsub empty: ok");
    return 0;
}
