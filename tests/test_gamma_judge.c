/*
 * test_gamma_judge.c - common/rdna3_gamma_judge.h on the host: the gamma
 * test's verdict from each pipe's CRCs (card run 58: changing screens are
 * UNJUDGED, a still pipe that did not move is a failure).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include "rdna3_gamma_judge.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int judge(const struct rd3_gamma_pipe *p, int n)
{
    const struct rd3_gamma_sum s = rd3_gamma_sum(p, n);
    return rd3_gamma_judge(&s);
}

int main(void)
{
    /* two still pipes: moved and came back */
    const struct rd3_gamma_pipe pass2[2] = { { 1, 1, 1, 0 }, { 1, 1, 1, 0 } };
    /* card run 49: ColorSync's restore loads its own curve, the identity table came back */
    const struct rd3_gamma_pipe idback[2] = { { 1, 1, 0, 1 }, { 1, 1, 0, 0 } };
    /* card run 58: both screens kept changing, nothing came back */
    const struct rd3_gamma_pipe flick[2] = { { 0, 1, 0, 0 }, { 0, 1, 0, 0 } };
    /* the review's case: a still pipe that did not move next to a changing one */
    const struct rd3_gamma_pipe stuck[2] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 } };
    /* a still pipe that moved but did not come back, next to a changing one */
    const struct rd3_gamma_pipe noback[2] = { { 1, 1, 0, 0 }, { 0, 1, 0, 0 } };
    /* one still pipe, no gamma at all */
    const struct rd3_gamma_pipe none1[1] = { { 1, 0, 0, 0 } };
    struct rd3_gamma_sum s;

    CHECK(judge(pass2, 2) == RD3_GAMMA_PASS);
    CHECK(judge(idback, 2) == RD3_GAMMA_PASS);
    CHECK(judge(flick, 2) == RD3_GAMMA_UNJUDGED);
    CHECK(judge(stuck, 2) == RD3_GAMMA_FAIL);
    s = rd3_gamma_sum(stuck, 2);
    CHECK(s.stuck == 1 && s.stable == 1 && s.moved == 1 && s.pipes == 2);
    CHECK(judge(noback, 2) == RD3_GAMMA_FAIL);
    CHECK(judge(none1, 1) == RD3_GAMMA_FAIL);
    CHECK(judge(flick, 0) == RD3_GAMMA_FAIL);   /* no lit pipe */
    CHECK(judge(NULL, 2) == RD3_GAMMA_FAIL);
    CHECK(rd3_gamma_judge(NULL) == RD3_GAMMA_FAIL);
    if (fails) {
        printf("test_gamma_judge: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_gamma_judge: all checks passed\n");
    return 0;
}
