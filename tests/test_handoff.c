/*
 * test_handoff.c - metal/plugin/nvmtl_handoff.h on the host (DRIVER-PLAN N59): the hand-off counters and the
 * `vk: handoff: SUMMARY` body as NVMTLEventSync.m builds them.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "nvmtl_handoff.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    nvmtl_ho_stats s;
    char line[256];
    memset(&s, 0, sizeof s);
    CHECK(nvmtl_ho_format(&s, 0, line, sizeof line) == 0);
    nvmtl_ho_publish(&s, NVMTL_HO_NONE, 0, 0, 0);
    nvmtl_ho_publish(&s, NVMTL_HO_WAIT, 3000000, 0, 0);
    nvmtl_ho_publish(&s, NVMTL_HO_HANDLER, 1000000, 0, 0);
    nvmtl_ho_publish(&s, NVMTL_HO_NONE, 0, 1, 2000000);
    nvmtl_ho_publish(&s, NVMTL_HO_STATUS, 2000000, 0, 0);
    CHECK(nvmtl_ho_format(&s, 1, line, sizeof line) == 1);
    CHECK(!strcmp(line, "cbs=5 early=3 by_handler=1 by_wait=1 by_status=1 gap_us_avg=2000 gap_us_max=3000 held=1 held_us_avg=2000 hold=1"));
    CHECK(nvmtl_ho_hold_on("1") && !nvmtl_ho_hold_on("0") && !nvmtl_ho_hold_on(NULL) && !nvmtl_ho_hold_on(""));
    printf("test_handoff: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
