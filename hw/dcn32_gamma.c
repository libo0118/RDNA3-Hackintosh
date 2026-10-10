/*
 * dcn32_gamma.c - see dcn32_gamma.h.
 *
 * Ported from (Linux v6.12, MIT): dcn32_set_output_transfer_func,
 * mpc3_set_output_gamma, mpc3_get_ogam_current, mpc3_power_on_ogam_lut,
 * mpc3_configure_ogam_lut, mpc3_program_luta, mpc3_program_lutb,
 * mpc3_program_ogam_pwl, mpc3_ogam_get_reg_field (RAM A's blue fields for
 * every register), cm_helper_program_gamcor_xfer_func,
 * cm3_helper_translate_curve_to_hw_format, cm3_helper_convert_to_custom_float,
 * is_rgb_equal (dcn30_mpc.c, dcn30_cm_common.c, dcn32_hwseq.c),
 * convert_to_custom_float_format / build_custom_float / setup_custom_float
 * (custom_float.c), dc_fixpt_from_fraction (fixpt31_32.c), __is_lut_linear
 * (amdgpu_dm_color.c) and optc3_lock / optc1_unlock.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_gamma.h"
#include "gen/dcn_3_2_1.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "gen/dcn_3_2_0_clone.h"
#include "gen/dcn_3_2_0_scale.h"
#include "gen/dcn_3_2_0_gamma.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))
#define FIELD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)

#define RI(d, reg, n, stride) rd((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n))
#define WI(d, reg, n, stride, v) wr((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n), (v))

#define OGAM_S (regMPCC_OGAM1_MPCC_OGAM_CONTROL - regMPCC_OGAM0_MPCC_OGAM_CONTROL)   /* 0x5e */
#define MPCC_S RD_CLONE_MPCC_STRIDE                                                    /* 0x15 */
#define ODM_S  RD_CLONE_ODM_STRIDE
#define OTG_S  RD_CLONE_OTG_STRIDE
#define MUX_S  (regMPC_OUT1_MUX - regMPC_OUT0_MUX)
/* RAM B's registers sit at the same distance from RAM A's for every one */
#define RAMB_S (regMPCC_OGAM0_MPCC_OGAM_RAMB_START_CNTL_B - regMPCC_OGAM0_MPCC_OGAM_RAMA_START_CNTL_B)

static uint32_t rd(rdna3_dev *d, uint32_t seg, uint32_t off)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off));
}

static void wr(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off), v);
}

/* ------------------------------------------------------------ fixed point */

/* struct fixed31_32: 32 fraction bits in a signed 64-bit value */
typedef int64_t fixpt;
#define FIX_ONE ((fixpt)1 << 32)

/* dc_fixpt_from_fraction: bit by bit, the last bit rounded */
static fixpt fix_from_fraction(int64_t num, int64_t den)
{
    int neg = (num < 0) != (den < 0);
    uint64_t a = (uint64_t)(num < 0 ? -num : num), b = (uint64_t)(den < 0 ? -den : den);
    uint64_t res = a / b, rem = a % b;
    unsigned i;
    for (i = 0; i < 32; i++) {
        rem <<= 1;
        res <<= 1;
        if (rem >= b) {
            res |= 1;
            rem -= b;
        }
    }
    res += (rem << 1) >= b;
    return neg ? -(fixpt)res : (fixpt)res;
}

/* convert_to_custom_float_format: build_custom_float + setup_custom_float,
 * unsigned formats (format->sign = false) */
static uint32_t custom_float(fixpt value, uint32_t exp_bits, uint32_t mant_bits)
{
    uint32_t exp_offset = (1u << (exp_bits - 1)) - 1;
    const fixpt max_mant = fix_from_fraction(((int64_t)1 << (mant_bits + 1)) - 1, (int64_t)1 << mant_bits);
    uint32_t mantissa, exponent, i;
    fixpt mant;

    if (value == 0)
        return 0;
    if (value < 0)
        value = -value;                 /* the sign bit is dropped: unsigned format */
    if (value < FIX_ONE) {
        i = 1;
        do {
            value <<= 1;
            ++i;
        } while (value < FIX_ONE);
        --i;
        if (exp_offset <= i)
            return 0;                   /* below the smallest exponent */
        exponent = exp_offset - i;
    } else if (max_mant <= value) {
        i = 1;
        do {
            value >>= 1;
            ++i;
        } while (max_mant < value);
        exponent = exp_offset + i - 1;
    } else {
        exponent = exp_offset;
    }
    mant = value - FIX_ONE;
    if (mant < 0 || FIX_ONE < mant)
        mant = 0;
    else
        mant <<= mant_bits;
    mantissa = (uint32_t)(mant >> 32);

    /* setup_custom_float: the verification clamps, then mantissa | exponent */
    if (mantissa & ~((1u << (mant_bits + 1)) - 1))
        mantissa = (1u << (mant_bits + 1)) - 1;
    if (exponent & ~((1u << (exp_bits + 1)) - 1))
        exponent = (1u << (exp_bits + 1)) - 1;
    return (mantissa & ((1u << mant_bits) - 1)) | (exponent & ((1u << exp_bits) - 1)) << mant_bits;
}

/* ------------------------------------------------------------ the curve */

/* cm3_helper_translate_curve_to_hw_format, the else (SDR) branch */
#define MAX_REGIONS_NUMBER 34
#define MAX_LOW_POINT      25
#define NUMBER_SW_SEGMENTS 16
static const uint32_t sdr_seg_distr[] = { 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 1 };
#define SDR_REGION_START (-10)
#define SDR_REGION_END   1
#define SEG_NONE 0xffffffffu

/* x in units of 2^-14 (every SDR point is a multiple): the table at x,
 * linearly interpolated, as a fraction of 65535 */
#define X_ONE 16384u
static fixpt table_at(const uint16_t *t, uint32_t n, uint32_t x)
{
    uint64_t pos, idx, rem;
    if (x >= X_ONE)
        return fix_from_fraction(t[n - 1], 65535);
    pos = (uint64_t)x * (n - 1);
    idx = pos / X_ONE;
    rem = pos % X_ONE;
    return fix_from_fraction((int64_t)(t[idx] * (X_ONE - rem) + t[idx + 1] * rem), (int64_t)X_ONE * 65535);
}

/* __is_lut_linear: every entry within +-1 of i * 0xffff / (size - 1).
 * The tables macOS hands down have 12 bits (IOFBGammaWidth 12), scaled to
 * 16: its identity is off by up to half a 12-bit step (card run 30: the
 * restored ColorSync identity stayed on the PWL instead of the bypass), so
 * the tolerance is one 12-bit step. */
#define LINEAR_TOLERANCE 16
static int table_linear(const uint16_t *t, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int64_t delta = (int64_t)t[i] - (int64_t)((uint64_t)i * 0xffffu / (n - 1));
        if (delta < -LINEAR_TOLERANCE || delta > LINEAR_TOLERANCE)
            return 0;
    }
    return 1;
}

int dcn32_gamma_build(const uint16_t *data, uint32_t channels, uint32_t count, rdna3_gamma_pwl *p)
{
    uint32_t seg_distr[MAX_REGIONS_NUMBER], offset[MAX_REGIONS_NUMBER] = { 0 }, segs[MAX_REGIONS_NUMBER] = { 0 };
    const int32_t region_start = SDR_REGION_START, region_end = SDR_REGION_END;
    fixpt y, y0, yend;
    uint32_t c, k, i, j, hw_points = 0;
    /* corner x: dc_fixpt_pow(2, region_start / region_end), exact powers of two */
    const fixpt x0 = FIX_ONE >> -SDR_REGION_START, x1 = FIX_ONE << SDR_REGION_END;

    if (!data || !p || (channels != 1 && channels != 3) || count < 2 || count > RD_GAMMA_MAX_ENTRIES)
        return RDNA3_EINVAL;
    for (k = 0; k < sizeof(*p); k++)
        ((uint8_t *)p)[k] = 0;

    for (k = 0; k < MAX_REGIONS_NUMBER; k++)
        seg_distr[k] = k < sizeof(sdr_seg_distr) / sizeof(sdr_seg_distr[0]) ? sdr_seg_distr[k] : SEG_NONE;
    for (k = 0; k < MAX_REGIONS_NUMBER; k++)
        if (seg_distr[k] != SEG_NONE)
            hw_points += 1u << seg_distr[k];
    if (hw_points + 1 != RD_GAMMA_HW_POINTS)
        return RDNA3_EINVAL;

    for (c = 0; c < 3; c++) {
        const uint16_t *t = data + (channels == 3 ? c * count : 0);
        /* the points: region k covers [2^e, 2^(e+1)), e = region_start + k;
         * tf_pts index i of the region sits at 2^e * (1 + i / 16) */
        j = 0;
        y0 = 0;
        for (k = 0; k < (uint32_t)(region_end - region_start); k++) {
            int32_t e = region_start + (int32_t)k;
            uint32_t increment = NUMBER_SW_SEGMENTS / (1u << seg_distr[k]);
            for (i = 0; i < NUMBER_SW_SEGMENTS && j < hw_points; i += increment) {
                /* 2^e * (1 + i / 16) in units of 2^-14 */
                y = table_at(t, count, (NUMBER_SW_SEGMENTS + i) << (e + 10));
                if (!j)
                    y0 = y;
                p->lut[c][j++] = custom_float(y, 6, 12);
            }
        }
        /* last point: x = 2^region_end */
        yend = table_at(t, count, X_ONE << region_end);
        p->lut[c][hw_points] = custom_float(yend, 6, 12);

        /* corner points: start slope y0 / x0 (dc_fixpt_div), end base y(2),
         * end slope 0; cm3_helper_convert_to_custom_float */
        p->start_x[c] = custom_float(x0, 6, 12);
        p->start_slope[c] = custom_float(fix_from_fraction(y0, x0), 6, 12);
        p->end_base[c] = custom_float(yend, 6, 12);
        p->end_x[c] = custom_float(x1, 6, 10);
        p->end_slope[c] = custom_float(0, 6, 10);
    }
    p->num = hw_points + 1;

    /* arr_curve_points: offsets and segment counts as the amdgpu loop sets them */
    k = 0;
    for (i = 1; i < MAX_REGIONS_NUMBER; i++) {
        if (seg_distr[k] != SEG_NONE) {
            segs[k] = seg_distr[k];
            offset[i] = offset[k] + (1u << seg_distr[k]);
        }
        k++;
    }
    if (seg_distr[k] != SEG_NONE)
        segs[k] = seg_distr[k];
    for (i = 0; i < RD_GAMMA_REGIONS; i++)
        p->region[i] =
            FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_REGION_0_1, MPCC_OGAM_RAMA_EXP_REGION0_LUT_OFFSET, offset[2 * i]) |
            FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_REGION_0_1, MPCC_OGAM_RAMA_EXP_REGION0_NUM_SEGMENTS, segs[2 * i]) |
            FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_REGION_0_1, MPCC_OGAM_RAMA_EXP_REGION1_LUT_OFFSET, offset[2 * i + 1]) |
            FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_REGION_0_1, MPCC_OGAM_RAMA_EXP_REGION1_NUM_SEGMENTS, segs[2 * i + 1]);

    /* is_rgb_equal */
    p->rgb_equal = 1;
    for (j = 0; j < p->num; j++)
        if (p->lut[0][j] != p->lut[1][j] || p->lut[2][j] != p->lut[0][j])
            p->rgb_equal = 0;
    p->linear = 1;
    for (c = 0; c < channels; c++)
        if (!table_linear(data + c * count, count))
            p->linear = 0;
    return RDNA3_OK;
}

/* ------------------------------------------------------------ registers */

int dcn32_gamma_otg_mpccs(rdna3_dev *d, uint32_t otg, uint32_t mpcc[2])
{
    uint32_t sel, opp[2], n, i;
    if (!d || !mpcc || otg >= RD_DCN_MAX_PIPES)
        return RDNA3_EINVAL;
    sel = RI(d, regODM0_OPTC_DATA_SOURCE_SELECT, otg, ODM_S);
    n = GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_NUM_OF_INPUT_SEGMENT) ? 2 : 1;
    opp[0] = GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG0_SRC_SEL);
    opp[1] = GET(sel, ODM0_OPTC_DATA_SOURCE_SELECT, OPTC_SEG1_SRC_SEL);
    for (i = 0; i < n; i++) {
        if (opp[i] >= RD_DCN_MAX_PIPES)
            return RDNA3_ENODEV;
        mpcc[i] = GET(RI(d, regMPC_OUT0_MUX, opp[i], MUX_S), MPC_OUT0_MUX, MPC_OUT_MUX);
        if (mpcc[i] >= RD_DCN_MAX_PIPES)
            return RDNA3_ENODEV;
    }
    return (int)n;
}

/* optc3_lock / optc1_unlock (as hw/dcn32.c) */
static void otg_lock(rdna3_dev *d, uint32_t otg, int lock)
{
    uint32_t v, t;
    if (otg >= RD_DCN_MAX_PIPES)
        return;
    if (lock) {
        v = RI(d, regOTG0_OTG_GLOBAL_CONTROL2, otg, OTG_S);
        WI(d, regOTG0_OTG_GLOBAL_CONTROL2, otg, OTG_S, SET(v, OTG0_OTG_GLOBAL_CONTROL2, OTG_MASTER_UPDATE_LOCK_SEL, otg));
    }
    WI(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg, OTG_S, lock ? OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK : 0);
    for (t = 0; lock && t < 10 && !GET(RI(d, regOTG0_OTG_MASTER_UPDATE_LOCK, otg, OTG_S),
                                       OTG0_OTG_MASTER_UPDATE_LOCK, UPDATE_LOCK_STATUS); t++)
        d->os.udelay(d->os.ctx, 1);
}

/* mpc3_get_ogam_current */
static uint32_t ogam_current(uint32_t ctl)
{
    if (GET(ctl, MPCC_OGAM0_MPCC_OGAM_CONTROL, MPCC_OGAM_MODE_CURRENT) != 2)
        return RD_GAMMA_LUT_BYPASS;
    return GET(ctl, MPCC_OGAM0_MPCC_OGAM_CONTROL, MPCC_OGAM_SELECT_CURRENT) ? RD_GAMMA_LUT_RAM_B : RD_GAMMA_LUT_RAM_A;
}

/* mpc3_power_on_ogam_lut(true): force the memory on, REG_WAIT 10 x 10 us */
static int ogam_power_on(rdna3_dev *d, uint32_t mpcc)
{
    uint32_t v = RI(d, regMPCC0_MPCC_MEM_PWR_CTRL, mpcc, MPCC_S), t;
    WI(d, regMPCC0_MPCC_MEM_PWR_CTRL, mpcc, MPCC_S, SET(v, MPCC0_MPCC_MEM_PWR_CTRL, MPCC_OGAM_MEM_PWR_DIS, 1));
    for (t = 0; t < 10; t++) {
        if (!GET(RI(d, regMPCC0_MPCC_MEM_PWR_CTRL, mpcc, MPCC_S), MPCC0_MPCC_MEM_PWR_CTRL, MPCC_OGAM_MEM_PWR_STATE))
            return RDNA3_OK;
        d->os.udelay(d->os.ctx, 10);
    }
    return RDNA3_ETIME;
}

/* mpc3_program_luta / mpc3_program_lutb -> cm_helper_program_gamcor_xfer_func;
 * B, G, R registers are consecutive, RAM B's at RAMB_S from RAM A's */
static void ogam_program_lut_regs(rdna3_dev *d, uint32_t mpcc, uint32_t ram_b, const rdna3_gamma_pwl *p)
{
    const uint32_t s = ram_b ? RAMB_S : 0;
    uint32_t c, i;
#define WR_RAM(reg, k, v) \
    wr(d, reg##_BASE_IDX, (uint32_t)(reg) + s + (k) + OGAM_S * mpcc, (v))
    for (c = 0; c < 3; c++) {          /* register order B, G, R; p is R, G, B */
        uint32_t ch = 2 - c;
        WR_RAM(regMPCC_OGAM0_MPCC_OGAM_RAMA_START_CNTL_B, c,
               FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_START_CNTL_B, MPCC_OGAM_RAMA_EXP_REGION_START_B, p->start_x[ch]) |
               FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_START_CNTL_B, MPCC_OGAM_RAMA_EXP_REGION_START_SEGMENT_B, 0));
        WR_RAM(regMPCC_OGAM0_MPCC_OGAM_RAMA_START_SLOPE_CNTL_B, c,
               FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_START_SLOPE_CNTL_B, MPCC_OGAM_RAMA_EXP_REGION_START_SLOPE_B,
                     p->start_slope[ch]));
    }
    for (c = 0; c < 3; c++) {          /* END_CNTL1_B, END_CNTL2_B, END_CNTL1_G, ... */
        uint32_t ch = 2 - c;
        WR_RAM(regMPCC_OGAM0_MPCC_OGAM_RAMA_END_CNTL1_B, 2 * c,
               FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_END_CNTL1_B, MPCC_OGAM_RAMA_EXP_REGION_END_BASE_B, p->end_base[ch]));
        WR_RAM(regMPCC_OGAM0_MPCC_OGAM_RAMA_END_CNTL2_B, 2 * c,
               FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_END_CNTL2_B, MPCC_OGAM_RAMA_EXP_REGION_END_SLOPE_B, p->end_slope[ch]) |
               FIELD(MPCC_OGAM0_MPCC_OGAM_RAMA_END_CNTL2_B, MPCC_OGAM_RAMA_EXP_REGION_END_B, p->end_x[ch]));
    }
    for (i = 0; i < RD_GAMMA_REGIONS; i++)
        WR_RAM(regMPCC_OGAM0_MPCC_OGAM_RAMA_REGION_0_1, i, p->region[i]);
#undef WR_RAM
}

/* mpc3_program_ogam_pwl: one pass for all three colours when they agree,
 * else red (mask 4), green (2), blue (1), the index back to 0 between */
static void ogam_program_pwl(rdna3_dev *d, uint32_t mpcc, const rdna3_gamma_pwl *p)
{
    static const uint32_t mask[3] = { 4, 2, 1 };
    uint32_t c, i, v;
    if (p->rgb_equal) {
        for (i = 0; i < p->num; i++)
            WI(d, regMPCC_OGAM0_MPCC_OGAM_LUT_DATA, mpcc, OGAM_S, p->lut[0][i]);
        return;
    }
    for (c = 0; c < 3; c++) {
        if (c)
            WI(d, regMPCC_OGAM0_MPCC_OGAM_LUT_INDEX, mpcc, OGAM_S, 0);
        v = RI(d, regMPCC_OGAM0_MPCC_OGAM_LUT_CONTROL, mpcc, OGAM_S);
        WI(d, regMPCC_OGAM0_MPCC_OGAM_LUT_CONTROL, mpcc, OGAM_S, SET(v, MPCC_OGAM0_MPCC_OGAM_LUT_CONTROL, MPCC_OGAM_LUT_WRITE_COLOR_MASK, mask[c]));
        for (i = 0; i < p->num; i++)
            WI(d, regMPCC_OGAM0_MPCC_OGAM_LUT_DATA, mpcc, OGAM_S, p->lut[c][i]);
    }
}

int dcn32_gamma_apply(rdna3_dev *d, uint32_t otg, uint32_t mpcc, const rdna3_gamma_pwl *p)
{
    uint32_t ctl0, cur, next, v;
    int r;

    if (!d || mpcc >= RD_DCN_MAX_PIPES || (p && (p->num != RD_GAMMA_HW_POINTS)))
        return RDNA3_EINVAL;
    otg_lock(d, otg, 1);
    ctl0 = RI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, mpcc, OGAM_S);
    if (!p) {                          /* disable OGAM */
        WI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, mpcc, OGAM_S, FIELD(MPCC_OGAM0_MPCC_OGAM_CONTROL, MPCC_OGAM_MODE, 0));
        otg_lock(d, otg, 0);
        RLOG(d, "gamma: MPCC %u bypass (was %u)", mpcc, ogam_current(ctl0));
        return RDNA3_OK;
    }
    /* enable OGAM (REG_SET: SELECT 0 until the new RAM is chosen; under
     * the lock nothing of it latches before the unlock) */
    WI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, mpcc, OGAM_S, FIELD(MPCC_OGAM0_MPCC_OGAM_CONTROL, MPCC_OGAM_MODE, 2));
    cur = ogam_current(RI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, mpcc, OGAM_S));
    next = cur == RD_GAMMA_LUT_RAM_A ? RD_GAMMA_LUT_RAM_B : RD_GAMMA_LUT_RAM_A;

    r = ogam_power_on(d, mpcc);
    if (r) {
        WI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, mpcc, OGAM_S,
           ctl0 & (MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_MODE_MASK | MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_SELECT_MASK |
                   MPCC_OGAM0_MPCC_OGAM_CONTROL__MPCC_OGAM_PWL_DISABLE_MASK));
        otg_lock(d, otg, 0);
        RERR(d, "gamma: MPCC %u OGAM memory does not power up (MEM_PWR_CTRL 0x%x)", mpcc,
             RI(d, regMPCC0_MPCC_MEM_PWR_CTRL, mpcc, MPCC_S));
        return r;
    }
    /* mpc3_configure_ogam_lut */
    v = RI(d, regMPCC_OGAM0_MPCC_OGAM_LUT_CONTROL, mpcc, OGAM_S);
    v = SET(v, MPCC_OGAM0_MPCC_OGAM_LUT_CONTROL, MPCC_OGAM_LUT_WRITE_COLOR_MASK, 7);
    v = SET(v, MPCC_OGAM0_MPCC_OGAM_LUT_CONTROL, MPCC_OGAM_LUT_HOST_SEL, next == RD_GAMMA_LUT_RAM_A ? 0 : 1);
    WI(d, regMPCC_OGAM0_MPCC_OGAM_LUT_CONTROL, mpcc, OGAM_S, v);
    WI(d, regMPCC_OGAM0_MPCC_OGAM_LUT_INDEX, mpcc, OGAM_S, 0);

    ogam_program_lut_regs(d, mpcc, next == RD_GAMMA_LUT_RAM_B, p);
    ogam_program_pwl(d, mpcc, p);

    v = RI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, mpcc, OGAM_S);
    WI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, mpcc, OGAM_S,
       SET(v, MPCC_OGAM0_MPCC_OGAM_CONTROL, MPCC_OGAM_SELECT, next == RD_GAMMA_LUT_RAM_A ? 0 : 1));
    /* enable_mem_low_power.bits.mpc is false on DCN 3.2: the memory stays on */
    otg_lock(d, otg, 0);
    RLOG(d, "gamma: MPCC %u RAM %c (%s, end 0x%x/0x%x/0x%x)", mpcc, next == RD_GAMMA_LUT_RAM_A ? 'A' : 'B',
         p->rgb_equal ? "one table" : "per channel", p->end_base[0], p->end_base[1], p->end_base[2]);
    return RDNA3_OK;
}

int dcn32_gamma_set_head(rdna3_dev *d, const rdna3_dcn *dc, uint32_t head, const rdna3_gamma_pwl *p)
{
    uint32_t mpcc[2];
    int n, i, r;
    if (!d || !dc || head >= dc->nheads)
        return RDNA3_EINVAL;
    n = dcn32_gamma_otg_mpccs(d, dc->head[head].otg, mpcc);
    if (n < 0) {
        RERR(d, "gamma: head %u (OTG %u): no MPCC feeds it (%d)", head, dc->head[head].otg, n);
        return n;
    }
    for (i = 0; i < n; i++) {
        r = dcn32_gamma_apply(d, dc->head[head].otg, mpcc[i], p);
        if (r)
            return r;
    }
    return RDNA3_OK;
}

int dcn32_gamma_state(rdna3_dev *d, uint32_t mpcc, rdna3_gamma_state *s)
{
    uint32_t ctl;
    if (!d || !s || mpcc >= RD_DCN_MAX_PIPES)
        return RDNA3_EINVAL;
    ctl = RI(d, regMPCC_OGAM0_MPCC_OGAM_CONTROL, mpcc, OGAM_S);
    s->mode = GET(ctl, MPCC_OGAM0_MPCC_OGAM_CONTROL, MPCC_OGAM_MODE);
    s->select = GET(ctl, MPCC_OGAM0_MPCC_OGAM_CONTROL, MPCC_OGAM_SELECT);
    s->current = ogam_current(ctl);
    s->mem_pwr_state = GET(RI(d, regMPCC0_MPCC_MEM_PWR_CTRL, mpcc, MPCC_S), MPCC0_MPCC_MEM_PWR_CTRL, MPCC_OGAM_MEM_PWR_STATE);
    s->dpp_degamma = GET(RI(d, regCM0_CM_GAMCOR_CONTROL, mpcc, RD_CLONE_DPP_STRIDE), CM0_CM_GAMCOR_CONTROL,
                         CM_GAMCOR_MODE_CURRENT);
    return RDNA3_OK;
}
