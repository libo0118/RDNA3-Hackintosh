/*
 * rdna3_gamma_judge.h - the verdict of rdna3_modetest's gamma test from
 * each lit pipe's CRCs. Plain C, no CoreGraphics: userspace/rdna3_modetest.c
 * uses it and tests/test_gamma_judge.c checks it on the host.
 *
 * Per pipe: stable (its two looks before the test had one CRC), moved (the
 * dark table's CRC was neither of them), back (a look after ColorSync's
 * restore matched one before), idBack (the identity table brought a CRC of
 * before back). Card run 58 (2026-10-15d): the screens kept changing, so a
 * "moved" CRC proved nothing and no restore could match; such a run is
 * UNJUDGED (exit 3) and triage judges it by the core's record of the
 * tables. A pipe that stayed still while another kept changing is still a
 * witness (card run 58b review): when it did not move with the dark table,
 * no gamma reached it, whatever the other pipes did.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_GAMMA_JUDGE_H
#define RDNA3_GAMMA_JUDGE_H

struct rd3_gamma_pipe {
    int stable, moved, back, idBack;
};

enum { RD3_GAMMA_PASS = 0, RD3_GAMMA_FAIL = 1, RD3_GAMMA_UNJUDGED = 3 };

/* the sums over the lit pipes the verdict is made of */
struct rd3_gamma_sum {
    int pipes, stable, moved, back, idBack;
    int stuck;   /* stable pipes whose CRC did not move with the dark table */
};

static inline struct rd3_gamma_sum rd3_gamma_sum(const struct rd3_gamma_pipe *p, int n)
{
    struct rd3_gamma_sum s = { 0, 0, 0, 0, 0, 0 };
    int i;
    for (i = 0; p && i < n; i++) {
        s.pipes++;
        s.stable += !!p[i].stable;
        s.moved += !!p[i].moved;
        s.back += !!p[i].back;
        s.idBack += !!p[i].idBack;
        s.stuck += p[i].stable && !p[i].moved;
    }
    return s;
}

/* RD3_GAMMA_PASS, _FAIL or _UNJUDGED (no pipe stayed still, and none came
 * back after the restore or with the identity table) */
static inline int rd3_gamma_judge(const struct rd3_gamma_sum *s)
{
    if (!s || !s->pipes)
        return RD3_GAMMA_FAIL;
    if (s->moved == s->pipes && (s->back == s->pipes || s->idBack > 0))
        return RD3_GAMMA_PASS;
    if (s->stuck)
        return RD3_GAMMA_FAIL;
    if (!s->stable && !s->back && !s->idBack)
        return RD3_GAMMA_UNJUDGED;
    return RD3_GAMMA_FAIL;
}

#endif
