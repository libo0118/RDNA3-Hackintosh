/*
 * fixpt31_32.h - Display Core's 31.32 signed fixed point (the subset the DSC
 * policy and the MST payload math use), so the kext needs no floating point.
 * Ported from amdgpu Display Core (Linux v6.12, MIT):
 * dc/basics/fixpt31_32.c (dc_fixpt_from_fraction, dc_fixpt_mul) and
 * include/fixed31_32.h (dc_fixpt_from_int, dc_fixpt_add / _sub / _add_int /
 * _sub_int, dc_fixpt_mul_int, dc_fixpt_div / _div_int, dc_fixpt_shl,
 * dc_fixpt_floor, dc_fixpt_ceil), with the same rounding.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_FIXPT31_32_H
#define RDNA3_FIXPT31_32_H

#include <stdint.h>

#define RD_FIXPT_FRAC_BITS 32
#define RD_FIXPT_FRAC_MASK ((1ull << RD_FIXPT_FRAC_BITS) - 1)

typedef struct rd_fixpt { int64_t value; } rd_fixpt;   /* struct fixed31_32 */

/* dc_fixpt_from_fraction: integer part, 32 fraction bits by long division,
 * the LSB rounded up when the remainder is at least half the divisor */
static inline rd_fixpt dc_fixpt_from_fraction(int64_t numerator, int64_t denominator)
{
    rd_fixpt res;
    int neg1 = numerator < 0, neg2 = denominator < 0;
    uint64_t a1 = neg1 ? (uint64_t)-numerator : (uint64_t)numerator;
    uint64_t a2 = neg2 ? (uint64_t)-denominator : (uint64_t)denominator;
    uint64_t rem, v;
    unsigned i = RD_FIXPT_FRAC_BITS;
    if (!a2) {
        res.value = 0;
        return res;
    }
    v = a1 / a2;
    rem = a1 % a2;
    do {
        rem <<= 1;
        v <<= 1;
        if (rem >= a2) {
            v |= 1;
            rem -= a2;
        }
    } while (--i != 0);
    v += (rem << 1) >= a2;
    res.value = (int64_t)v;
    if (neg1 ^ neg2)
        res.value = -res.value;
    return res;
}

static inline rd_fixpt dc_fixpt_from_int(int64_t arg)
{
    rd_fixpt r;
    r.value = (int64_t)((uint64_t)arg << RD_FIXPT_FRAC_BITS);
    return r;
}

/* dc_fixpt_mul: integer and fraction parts multiplied separately, the
 * fraction x fraction product rounded */
static inline rd_fixpt dc_fixpt_mul(rd_fixpt arg1, rd_fixpt arg2)
{
    rd_fixpt res;
    int neg1 = arg1.value < 0, neg2 = arg2.value < 0;
    uint64_t v1 = neg1 ? (uint64_t)-arg1.value : (uint64_t)arg1.value;
    uint64_t v2 = neg2 ? (uint64_t)-arg2.value : (uint64_t)arg2.value;
    uint64_t i1 = v1 >> RD_FIXPT_FRAC_BITS, i2 = v2 >> RD_FIXPT_FRAC_BITS;
    uint64_t f1 = v1 & RD_FIXPT_FRAC_MASK, f2 = v2 & RD_FIXPT_FRAC_MASK, tmp, r;
    r = (i1 * i2) << RD_FIXPT_FRAC_BITS;
    r += i1 * f2;
    r += i2 * f1;
    tmp = f1 * f2;
    tmp = (tmp >> RD_FIXPT_FRAC_BITS) + (tmp >= (1ull << (RD_FIXPT_FRAC_BITS - 1)));   /* dc_fixpt_half */
    r += tmp;
    res.value = (int64_t)r;
    if (neg1 ^ neg2)
        res.value = -res.value;
    return res;
}

static inline rd_fixpt dc_fixpt_add(rd_fixpt a, rd_fixpt b) { a.value += b.value; return a; }
static inline rd_fixpt dc_fixpt_sub(rd_fixpt a, rd_fixpt b) { a.value -= b.value; return a; }
static inline rd_fixpt dc_fixpt_add_int(rd_fixpt a, int64_t b) { return dc_fixpt_add(a, dc_fixpt_from_int(b)); }
static inline rd_fixpt dc_fixpt_sub_int(rd_fixpt a, int64_t b) { return dc_fixpt_sub(a, dc_fixpt_from_int(b)); }
static inline rd_fixpt dc_fixpt_mul_int(rd_fixpt a, int64_t b) { return dc_fixpt_mul(a, dc_fixpt_from_int(b)); }
static inline rd_fixpt dc_fixpt_div(rd_fixpt a, rd_fixpt b) { return dc_fixpt_from_fraction(a.value, b.value); }
static inline rd_fixpt dc_fixpt_div_int(rd_fixpt a, int64_t b)
{
    return dc_fixpt_from_fraction(a.value, dc_fixpt_from_int(b).value);
}
static inline rd_fixpt dc_fixpt_shl(rd_fixpt a, unsigned shift)
{
    a.value = (int64_t)((uint64_t)a.value << shift);
    return a;
}

/* dc_fixpt_floor: toward zero, as DC's (it negates the magnitude's floor) */
static inline int64_t dc_fixpt_floor(rd_fixpt a)
{
    uint64_t v = a.value > 0 ? (uint64_t)a.value : (uint64_t)-a.value;
    return a.value >= 0 ? (int64_t)(v >> RD_FIXPT_FRAC_BITS) : -(int64_t)(v >> RD_FIXPT_FRAC_BITS);
}

/* dc_fixpt_ceil: the magnitude rounded up (one - epsilon added) */
static inline int64_t dc_fixpt_ceil(rd_fixpt a)
{
    uint64_t v = a.value > 0 ? (uint64_t)a.value : (uint64_t)-a.value;
    v += (1ull << RD_FIXPT_FRAC_BITS) - 1;
    return a.value >= 0 ? (int64_t)(v >> RD_FIXPT_FRAC_BITS) : -(int64_t)(v >> RD_FIXPT_FRAC_BITS);
}

#endif
