/* common/rdna3_bar_window.h on the host: how much of BAR0 the core maps
 * into the kernel and why it got none (DRIVER-PLAN N62, card run
 * 2026-10-28c: BAR0 1 GiB at 0x900000000 in the VM, not mapped).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "rdna3_bar_window.h"

#define MiB (1ull << 20)
#define GiB (1ull << 30)
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    /* the cap: rdna3vramwin absent = 1 GiB (IOPCIFamily's MAX_BAR_SIZE), raised to 64 MiB at least */
    CHECK(rd3_barwin_cap(0) == 1024 * MiB);
    CHECK(rd3_barwin_cap(512) == 512 * MiB);
    CHECK(rd3_barwin_cap(8) == 64 * MiB);
    CHECK(rd3_barwin_cap(16384) == 16 * GiB);

    /* 256 MiB: the whole BAR, tried once (as before N62) */
    CHECK(rd3_barwin_first(256 * MiB, rd3_barwin_cap(0)) == 256 * MiB);
    CHECK(rd3_barwin_next(256 * MiB) == 0);
    /* 512 MiB and 1 GiB: the whole BAR first */
    CHECK(rd3_barwin_first(512 * MiB, rd3_barwin_cap(0)) == 512 * MiB);
    CHECK(rd3_barwin_first(GiB, rd3_barwin_cap(0)) == GiB);
    /* a failed 1 GiB map: 512, then 256, then give up */
    CHECK(rd3_barwin_next(GiB) == 512 * MiB);
    CHECK(rd3_barwin_next(512 * MiB) == 256 * MiB);
    CHECK(rd3_barwin_next(256 * MiB) == 0);
    /* a 16 GiB BAR (should IOPCIFamily ever take one): the cap, then halves */
    CHECK(rd3_barwin_first(16 * GiB, rd3_barwin_cap(0)) == GiB);
    CHECK(rd3_barwin_first(16 * GiB, rd3_barwin_cap(4096)) == 4 * GiB);
    /* rdna3vramwin smaller than the BAR: the window, no retries below 256 MiB */
    CHECK(rd3_barwin_first(GiB, rd3_barwin_cap(128)) == 128 * MiB);
    CHECK(rd3_barwin_next(128 * MiB) == 0);
    /* a BAR that is not a whole MiB (never on this card): whole MiB only; none -> 0 */
    CHECK(rd3_barwin_first(256 * MiB + 4096, rd3_barwin_cap(0)) == 256 * MiB);
    CHECK(rd3_barwin_first(0, rd3_barwin_cap(0)) == 0);
    /* the loop the kext runs: 1 GiB failing every time tries exactly 1024, 512, 256 */
    {
        uint64_t tried[8];
        int n = 0;
        for (uint64_t w = rd3_barwin_first(GiB, rd3_barwin_cap(0)); w && n < 8; w = rd3_barwin_next(w))
            tried[n++] = w;
        CHECK(n == 3 && tried[0] == GiB && tried[1] == 512 * MiB && tried[2] == 256 * MiB);
    }

    /* resizable BAR control register: BAR index 0, size 2^n MiB in bits 13:8 */
    CHECK(rd3_rebar_ctrl_mib(0x0800u | (1u << 5)) == 256);   /* 8 = 256 MiB, 1 resizable BAR */
    CHECK(rd3_rebar_ctrl_mib(0x0e00u) == 16384);             /* 14 = 16 GiB */
    CHECK(rd3_rebar_ctrl_mib(0x0a00u) == 1024);
    CHECK(rd3_rebar_ctrl_mib(0x0102u) == 0);                 /* BAR2's register */

    /* why BAR0 has no IODeviceMemory */
    /* card run 2026-10-28c: VM, BAR0 at 0x900000000, no resizable BAR capability seen */
    CHECK(rd3_barwin_unassigned_why(0x900000000ull, 1, 0) == RD3_BARWIN_WHY_VM_ABOVE_4G);
    CHECK(strstr(rd3_barwin_why_text(RD3_BARWIN_WHY_VM_ABOVE_4G), "below 4 GiB") != NULL);
    /* bare metal with Resizable BAR on: 16 GiB, more than IOPCIFamily takes */
    CHECK(rd3_barwin_unassigned_why(0x7c00000000ull, 0, 16384) == RD3_BARWIN_WHY_TOO_BIG);
    CHECK(strstr(rd3_barwin_why_text(RD3_BARWIN_WHY_TOO_BIG), "ResizeAppleGpuBars") != NULL);
    /* the size wins over the address: a 16 GiB BAR in a VM above 4 GiB is too big first */
    CHECK(rd3_barwin_unassigned_why(0x900000000ull, 1, 16384) == RD3_BARWIN_WHY_TOO_BIG);
    /* 1 GiB above 4 GiB on bare metal (PFM64 on) is fine for IOPCIFamily: not this reason */
    CHECK(rd3_barwin_unassigned_why(0x7c00000000ull, 0, 1024) == RD3_BARWIN_WHY_UNKNOWN);
    /* below 4 GiB in a VM: not the 4 GiB reason */
    CHECK(rd3_barwin_unassigned_why(0x80000000ull, 1, 0) == RD3_BARWIN_WHY_UNKNOWN);
    CHECK(rd3_barwin_why_text(RD3_BARWIN_WHY_UNKNOWN)[0] != '\0');

    if (failures) {
        fprintf(stderr, "test_bar_window: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_bar_window: ok\n");
    return 0;
}
