/*
 * test_coh.c - common/rdna3_coh.h on the host (DRIVER-PLAN N54: the
 * kernel's GPU-memory coherence switches).
 *
 * Checked: the switch names and the flags string; the system-memory fence
 * copy's check (the VRAM answer by default, the system answer with
 * fence_sys; a VRAM value ahead of the system one opens ONE skew per
 * lag, a later seq keeps its clock, the skew closes with its length when
 * the system copy catches up, a copy that never lands goes stale after
 * RD3_COH_SYS_STALE_NS and the VRAM fence answers again, a ring reset under
 * a skew closes it without a length, the 32-bit wrap); the MTYPE of a user
 * mapping (gmc_v11_0_get_vm_pte's UC override for COHERENT / UNCACHED /
 * EXT_COHERENT BOs, the uc switch, nothing else touched).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rdna3_coh.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_names(void)
{
    char b[128];
    CHECK(!strcmp(rd3_coh_name(0), "fence_sys") && !strcmp(rd3_coh_name(7), "sdma_gcr"));
    CHECK(!strcmp(rd3_coh_name(8), "?"));
    CHECK(!strcmp(rd3_coh_flags_str(0, b, sizeof b), "none"));
    CHECK(!strcmp(rd3_coh_flags_str(RD3_COH_FENCE_SYS | RD3_COH_PIPE_SYNC, b, sizeof b), "fence_sys,pipe_sync"));
    CHECK(!strcmp(rd3_coh_flags_str(RD3_COH_ALL, b, sizeof b),
                  "fence_sys,fence_inv,pipe_sync,wait_idle,uc,hdp_end,vm_flush,sdma_gcr"));
    CHECK(!strcmp(rd3_coh_flags_str(RD3_COH_ALL, b, 6), "fence"));   /* cut, terminated */
    CHECK(RD3_COH_DEFAULT == 0);
}

static void test_fence(void)
{
    rd3_coh_ring_state s;
    rd3_coh_fence_out o;
    const uint64_t ms = 1000000ull;

    /* no system copy: the VRAM value, nothing measured */
    memset(&s, 0, sizeof s);
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 0, 10, 10, 0, 0, &o);
    CHECK(o.passed && !o.from_sys && !o.skew_new && !s.skew_open);

    /* both landed: passed from either source */
    rd3_coh_fence_check(&s, 0, 1, 10, 10, 10, 0, &o);
    CHECK(o.passed && !o.from_sys && !o.skew_new);
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 10, 10, 10, 0, &o);
    CHECK(o.passed && o.from_sys && !o.skew_new);
    /* neither */
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 11, 10, 10, 0, &o);
    CHECK(!o.passed && !o.skew_new);

    /* the system copy first (it is written first): no skew */
    rd3_coh_fence_check(&s, 0, 1, 11, 10, 11, 0, &o);
    CHECK(!o.passed && !o.skew_new && !s.skew_open);
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 11, 10, 11, 0, &o);
    CHECK(o.passed && o.from_sys);

    /* VRAM ahead: by default the VRAM answers (the old behaviour) and a skew opens once */
    rd3_coh_fence_check(&s, 0, 1, 12, 12, 11, 1 * ms, &o);
    CHECK(o.passed && !o.from_sys && o.skew_new && s.skew_open && s.skew_seq == 12);
    rd3_coh_fence_check(&s, 0, 1, 12, 12, 11, 2 * ms, &o);
    CHECK(o.passed && !o.skew_new && !o.skew_closed);
    /* with fence_sys the same check says not yet */
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 12, 12, 11, 2 * ms, &o);
    CHECK(!o.passed && o.from_sys && !o.skew_new);
    /* the system copy lands: the skew closes with its length */
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 12, 12, 12, 3 * ms + 500, &o);
    CHECK(o.passed && o.skew_closed && o.skew_ns == 2 * ms + 500 && !s.skew_open);

    /* a later seq while the copy is still behind: the same skew, its clock kept */
    rd3_coh_fence_check(&s, 0, 1, 13, 13, 12, 10 * ms, &o);
    CHECK(o.skew_new && s.skew_seq == 13);
    rd3_coh_fence_check(&s, 0, 1, 14, 14, 12, 20 * ms, &o);
    CHECK(!o.skew_new && s.skew_seq == 14 && s.skew_t0 == 10 * ms);
    /* an older seq's check does not move it back */
    rd3_coh_fence_check(&s, 0, 1, 13, 14, 12, 21 * ms, &o);
    CHECK(!o.skew_new && s.skew_seq == 14);
    /* the system copy reaches 13 only: still open (it is behind 14) */
    rd3_coh_fence_check(&s, 0, 1, 14, 14, 13, 22 * ms, &o);
    CHECK(s.skew_open && !o.skew_closed);
    rd3_coh_fence_check(&s, 0, 1, 14, 14, 14, 25 * ms, &o);
    CHECK(o.skew_closed && o.skew_ns == 15 * ms && !s.skew_open);

    /* a copy that never lands: stale after RD3_COH_SYS_STALE_NS, then the VRAM answers even with fence_sys */
    memset(&s, 0, sizeof s);
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 100, 100, 0, 1 * ms, &o);
    CHECK(!o.passed && o.skew_new && !o.went_stale);
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 101, 101, 0, 1 * ms + RD3_COH_SYS_STALE_NS - 1, &o);
    CHECK(!o.passed && !o.went_stale && !s.stale);
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 101, 101, 0, 1 * ms + RD3_COH_SYS_STALE_NS, &o);
    CHECK(o.passed && !o.from_sys && o.went_stale && s.stale);
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 101, 101, 0, 2 * RD3_COH_SYS_STALE_NS, &o);
    CHECK(o.passed && !o.went_stale);   /* said once */

    /* a reset under a skew (the VRAM value goes back): closed, no length */
    memset(&s, 0, sizeof s);
    rd3_coh_fence_check(&s, 0, 1, 50, 50, 49, 1 * ms, &o);
    CHECK(o.skew_new);
    rd3_coh_fence_check(&s, 0, 1, 1, 0, 0, 2 * ms, &o);
    CHECK(!s.skew_open && !o.skew_closed && !o.passed);

    /* the 32-bit wrap */
    memset(&s, 0, sizeof s);
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 2u, 3u, 0xfffffffeu, 0, &o);
    CHECK(!o.passed && o.skew_new);
    rd3_coh_fence_check(&s, RD3_COH_FENCE_SYS, 1, 2u, 3u, 2u, 5, &o);
    CHECK(o.passed && o.skew_closed && o.skew_ns == 5);
    CHECK(rd3_coh_seq_passed(1u, 0xffffffffu) && !rd3_coh_seq_passed(0xffffffffu, 1u));
}

static void test_pte(void)
{
    const uint64_t nc = 0x61ull | (0ull << 48), wc = 0x61ull | (1ull << 48);
    int why = -1;
    CHECK(rd3_coh_pte(0, nc, 0, &why) == nc && why == 0);
    CHECK(rd3_coh_pte(0, wc, 1ull << 2, &why) == wc && why == 0);   /* CPU_GTT_USWC: not a UC flag */
    CHECK(rd3_coh_pte(0, wc, 1ull << 13, &why) == (0x61ull | (3ull << 48)) && why == 1);
    CHECK(rd3_coh_pte(0, nc, 1ull << 14, &why) == (0x61ull | (3ull << 48)) && why == 1);
    CHECK(rd3_coh_pte(0, nc, 1ull << 15, &why) == (0x61ull | (3ull << 48)) && why == 1);
    CHECK(rd3_coh_pte(RD3_COH_UC, wc, 0, &why) == (0x61ull | (3ull << 48)) && why == 2);
    CHECK(rd3_coh_pte(RD3_COH_UC, nc, 1ull << 14, &why) == (0x61ull | (3ull << 48)) && why == 1);
    /* bits outside the MTYPE stay (address, PRT, fragment) */
    {
        const uint64_t p = 0x0000123456789000ull | (1ull << 51) | (9ull << 7) | 0x67ull | (2ull << 48);
        CHECK(rd3_coh_pte(RD3_COH_UC, p, 0, NULL) == ((p & ~(7ull << 48)) | (3ull << 48)));
    }
    CHECK(rd3_coh_pte(RD3_COH_ALL & ~RD3_COH_UC, nc, 0, &why) == nc && why == 0);
}

int main(void)
{
    test_names();
    test_fence();
    test_pte();
    if (fails) {
        printf("test_coh: %d FAILED\n", fails);
        return 1;
    }
    printf("test_coh: all passed\n");
    return 0;
}
