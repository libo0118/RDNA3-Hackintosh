/*
 * dcn32_color.c - see dcn32_color.h. Ported from Linux amdgpu Display Core
 * (mpc/dcn30/dcn30_mpc.c, mpc/dcn10/dcn10_mpc.c, dcn10/dcn10_cm_common.c,
 * basics/conversion.c, core/dc_hw_sequencer.c), MIT, Copyright Advanced
 * Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_color.h"
#include "gen/dcn_3_2_0_color.h"

#define GAMUT_STRIDE (regMPCC_OGAM1_MPCC_GAMUT_REMAP_MODE - regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE)
#define MUX_STRIDE   (regMPC_OUT1_MUX - regMPC_OUT0_MUX)      /* MUX and DENORM_* */
#define CSC_STRIDE   (regMPC_OUT1_CSC_MODE - regMPC_OUT0_CSC_MODE)
#define MPCC_STRIDE  (regMPCC1_MPCC_TOP_SEL - regMPCC0_MPCC_TOP_SEL)

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define FLD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)
#define SET(v, reg, field, x) (((v) & ~(uint32_t)reg##__##field##_MASK) | FLD(reg, field, x))
/* instance n of a register at 'stride' (reg##_BASE_IDX pasted here) */
#define AR(d, reg, stride, n) (rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + (stride) * (n))

/* the 12 coefficients of one matrix, two per register, C11_C12 .. C33_C34 */
#define MATRIX_REGS 6

/* ---- conversion.c / dc_hw_sequencer.c ---- */

#define DIVIDER   10000
#define S2D13_MIN (-3 * DIVIDER)
#define S2D13_MAX (3 * DIVIDER)

static rd_fixpt fixpt_abs(rd_fixpt a)
{
    if (a.value < 0)
        a.value = -a.value;
    return a;
}

/* dc_fixpt_round */
static int fixpt_round(rd_fixpt a)
{
    uint64_t v = a.value > 0 ? (uint64_t)a.value : (uint64_t)-a.value;
    v += 1ull << (RD_FIXPT_FRAC_BITS - 1);   /* dc_fixpt_half */
    return a.value >= 0 ? (int)(v >> RD_FIXPT_FRAC_BITS) : -(int)(v >> RD_FIXPT_FRAC_BITS);
}

/* dc_fixpt_clamp */
static rd_fixpt fixpt_clamp(rd_fixpt a, rd_fixpt lo, rd_fixpt hi)
{
    if (a.value <= lo.value)
        return lo;
    if (hi.value <= a.value)
        return hi;
    return a;
}

/* fixed_point_to_int_frac */
static uint16_t fixed_point_to_int_frac(rd_fixpt arg, uint8_t integer_bits, uint8_t fractional_bits)
{
    int32_t numerator, divisor = 1 << fractional_bits;
    uint16_t result;
    uint16_t dd = (uint16_t)dc_fixpt_floor(fixpt_abs(arg));

    /* (1 / (uint16_t)divisor) is 0 in integer arithmetic, as in DC */
    if (dd <= (uint16_t)(1 << integer_bits))
        numerator = (uint16_t)fixpt_round(dc_fixpt_mul_int(arg, divisor));
    else
        numerator = (int32_t)dc_fixpt_floor(
            dc_fixpt_sub(dc_fixpt_from_int(1ll << integer_bits),
                         dc_fixpt_from_fraction(1ll << RD_FIXPT_FRAC_BITS, dc_fixpt_from_int(divisor).value)));
    if (numerator >= 0)
        result = (uint16_t)numerator;
    else
        result = (uint16_t)((1 << (integer_bits + fractional_bits + 1)) + numerator);
    if (result != 0 && arg.value < 0)
        result |= (uint16_t)(1u << (integer_bits + fractional_bits));
    return result;
}

void dcn32_convert_float_matrix(uint16_t *matrix, const rd_fixpt *flt, uint32_t n)
{
    const rd_fixpt min_2_13 = dc_fixpt_from_fraction(S2D13_MIN, DIVIDER);
    const rd_fixpt max_2_13 = dc_fixpt_from_fraction(S2D13_MAX, DIVIDER);
    uint32_t i;
    for (i = 0; i < n; ++i)
        matrix[i] = fixed_point_to_int_frac(fixpt_clamp(flt[i], min_2_13, max_2_13), 2, 13);
}

/* output_csc_matrix[] of dc_hw_sequencer.c, by enum dc_color_space_type */
static const uint16_t output_csc_matrix[][12] = {
    [RD_COLOR_SPACE_RGB_TYPE] = { 0x2000, 0, 0, 0, 0, 0x2000, 0, 0, 0, 0, 0x2000, 0 },
    [RD_COLOR_SPACE_RGB_LIMITED_TYPE] = { 0x1B67, 0, 0, 0x201, 0, 0x1B67, 0, 0x201, 0, 0, 0x1B67, 0x201 },
    [RD_COLOR_SPACE_YCBCR601_TYPE] = { 0xE04, 0xF444, 0xFDB9, 0x1004, 0x831, 0x1016, 0x320, 0x201,
                                       0xFB45, 0xF6B7, 0xE04, 0x1004 },
    [RD_COLOR_SPACE_YCBCR709_TYPE] = { 0xE04, 0xF345, 0xFEB7, 0x1004, 0x5D3, 0x1399, 0x1FA, 0x201,
                                       0xFCCA, 0xF533, 0xE04, 0x1004 },
    [RD_COLOR_SPACE_YCBCR601_LIMITED_TYPE] = { 0xE00, 0xF447, 0xFDB9, 0x1000, 0x991, 0x12C9, 0x3A6, 0x200,
                                               0xFB47, 0xF6B9, 0xE00, 0x1000 },
    [RD_COLOR_SPACE_YCBCR709_LIMITED_TYPE] = { 0xE00, 0xF349, 0xFEB7, 0x1000, 0x6CE, 0x16E3, 0x24F, 0x200,
                                               0xFCCB, 0xF535, 0xE00, 0x1000 },
    [RD_COLOR_SPACE_YCBCR2020_TYPE] = { 0x1000, 0xF149, 0xFEB7, 0x1004, 0x0868, 0x15B2, 0x01E6, 0x201,
                                        0xFB88, 0xF478, 0x1000, 0x1004 },
    [RD_COLOR_SPACE_YCBCR709_BLACK_TYPE] = { 0x0000, 0x0000, 0x0000, 0x1000, 0x0000, 0x0000, 0x0000, 0x0200,
                                             0x0000, 0x0000, 0x0000, 0x1000 },
};

/* find_color_matrix */
const uint16_t *dcn32_find_color_matrix(uint32_t color_space_type)
{
    if (color_space_type >= sizeof(output_csc_matrix) / sizeof(output_csc_matrix[0]))
        return NULL;
    return output_csc_matrix[color_space_type];
}

/* cm_helper_program_color_matrices: C11/C12 in each register of the
 * consecutive C11_C12 .. C33_C34 run (the field layout of the first) */
static void cm_helper_program_color_matrices(rdna3_dev *d, const uint16_t *regval, uint32_t c11_c12,
                                             uint32_t mask11, uint32_t shift11, uint32_t mask12, uint32_t shift12)
{
    uint32_t i;
    for (i = 0; i < MATRIX_REGS; i++)
        rdna3_wreg(d, c11_c12 + i, (((uint32_t)regval[2 * i] << shift11) & mask11) |
                                   (((uint32_t)regval[2 * i + 1] << shift12) & mask12));
}

/* ---- dcn30_mpc.c ---- */

static void program_ocsc(rdna3_dev *d, uint32_t opp, const uint16_t *regval, uint32_t ocsc_mode)
{
    uint32_t base = ocsc_mode == RD_MPC_OUTPUT_CSC_COEF_A ? AR(d, regMPC_OUT0_CSC_C11_C12_A, CSC_STRIDE, opp)
                                                         : AR(d, regMPC_OUT0_CSC_C11_C12_B, CSC_STRIDE, opp);
    cm_helper_program_color_matrices(d, regval, base, MPC_OUT0_CSC_C11_C12_A__MPC_OCSC_C11_A_MASK,
                                     MPC_OUT0_CSC_C11_C12_A__MPC_OCSC_C11_A__SHIFT,
                                     MPC_OUT0_CSC_C11_C12_A__MPC_OCSC_C12_A_MASK,
                                     MPC_OUT0_CSC_C11_C12_A__MPC_OCSC_C12_A__SHIFT);
}

/* mpc3_set_output_csc */
int dcn32_mpc_set_output_csc(rdna3_dev *d, uint32_t opp, const uint16_t *regval, uint32_t ocsc_mode)
{
    rdna3_wreg(d, AR(d, regMPC_OUT_CSC_COEF_FORMAT, 0, 0), 0);
    rdna3_wreg(d, AR(d, regMPC_OUT0_CSC_MODE, CSC_STRIDE, opp), FLD(MPC_OUT0_CSC_MODE, MPC_OCSC_MODE, ocsc_mode));
    if (ocsc_mode == RD_MPC_OUTPUT_CSC_DISABLE)
        return RDNA3_OK;
    if (!regval)
        return RDNA3_EINVAL;
    program_ocsc(d, opp, regval, ocsc_mode);
    return RDNA3_OK;
}

/* mpc3_set_ocsc_default */
int dcn32_mpc_set_ocsc_default(rdna3_dev *d, uint32_t opp, uint32_t color_space_type, uint32_t ocsc_mode)
{
    const uint16_t *regval;
    rdna3_wreg(d, AR(d, regMPC_OUT_CSC_COEF_FORMAT, 0, 0), 0);
    rdna3_wreg(d, AR(d, regMPC_OUT0_CSC_MODE, CSC_STRIDE, opp), FLD(MPC_OUT0_CSC_MODE, MPC_OCSC_MODE, ocsc_mode));
    if (ocsc_mode == RD_MPC_OUTPUT_CSC_DISABLE)
        return RDNA3_OK;
    regval = dcn32_find_color_matrix(color_space_type);
    if (!regval)
        return RDNA3_EINVAL;
    program_ocsc(d, opp, regval, ocsc_mode);
    return RDNA3_OK;
}

/* mpc3_set_denorm: de-normalize the U1.13 color to the target depth, 0 bypass */
void dcn32_mpc_set_denorm(rdna3_dev *d, uint32_t opp, uint32_t color_depth)
{
    uint32_t mode = 0, a = AR(d, regMPC_OUT0_DENORM_CONTROL, MUX_STRIDE, opp);
    switch (color_depth) {
    case RD_COLOR_DEPTH_666: mode = 1; break;
    case RD_COLOR_DEPTH_888: mode = 2; break;
    case RD_COLOR_DEPTH_999: mode = 3; break;
    case RD_COLOR_DEPTH_101010: mode = 4; break;
    case RD_COLOR_DEPTH_111111: mode = 5; break;
    case RD_COLOR_DEPTH_121212: mode = 6; break;
    default: break;   /* 14 / 16 bpc: "not valid used case" */
    }
    rdna3_wreg(d, a, SET(rdna3_rreg(d, a), MPC_OUT0_DENORM_CONTROL, MPC_OUT_DENORM_MODE, mode));
}

/* mpc3_set_denorm_clamp */
void dcn32_mpc_set_denorm_clamp(rdna3_dev *d, uint32_t opp, const rdna3_denorm_clamp *c)
{
    uint32_t a = AR(d, regMPC_OUT0_DENORM_CONTROL, MUX_STRIDE, opp), v;
    v = rdna3_rreg(d, a);
    v = SET(v, MPC_OUT0_DENORM_CONTROL, MPC_OUT_DENORM_CLAMP_MAX_R_CR, c->clamp_max_r_cr);
    v = SET(v, MPC_OUT0_DENORM_CONTROL, MPC_OUT_DENORM_CLAMP_MIN_R_CR, c->clamp_min_r_cr);
    rdna3_wreg(d, a, v);
    a = AR(d, regMPC_OUT0_DENORM_CLAMP_G_Y, MUX_STRIDE, opp);
    v = rdna3_rreg(d, a);
    v = SET(v, MPC_OUT0_DENORM_CLAMP_G_Y, MPC_OUT_DENORM_CLAMP_MAX_G_Y, c->clamp_max_g_y);
    v = SET(v, MPC_OUT0_DENORM_CLAMP_G_Y, MPC_OUT_DENORM_CLAMP_MIN_G_Y, c->clamp_min_g_y);
    rdna3_wreg(d, a, v);
    a = AR(d, regMPC_OUT0_DENORM_CLAMP_B_CB, MUX_STRIDE, opp);
    v = rdna3_rreg(d, a);
    v = SET(v, MPC_OUT0_DENORM_CLAMP_B_CB, MPC_OUT_DENORM_CLAMP_MAX_B_CB, c->clamp_max_b_cb);
    v = SET(v, MPC_OUT0_DENORM_CLAMP_B_CB, MPC_OUT_DENORM_CLAMP_MIN_B_CB, c->clamp_min_b_cb);
    rdna3_wreg(d, a, v);
}

/* program_gamut_remap */
static void program_gamut_remap(rdna3_dev *d, uint32_t mpcc, const uint16_t *regval, uint32_t select)
{
    uint32_t mode = AR(d, regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, GAMUT_STRIDE, mpcc), base, selection = 0;
    if (!regval || select == RD_GAMUT_REMAP_BYPASS) {
        rdna3_wreg(d, mode, FLD(MPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, MPCC_GAMUT_REMAP_MODE, RD_GAMUT_REMAP_BYPASS));
        return;
    }
    if (select == RD_GAMUT_REMAP_COEFF)
        selection = 1;
    else if (select == RD_GAMUT_REMAP_COMA_COEFF)
        selection = 2;   /* coefficient set B */
    base = select == RD_GAMUT_REMAP_COEFF ? AR(d, regMPCC_OGAM0_MPC_GAMUT_REMAP_C11_C12_A, GAMUT_STRIDE, mpcc)
                                         : AR(d, regMPCC_OGAM0_MPC_GAMUT_REMAP_C11_C12_B, GAMUT_STRIDE, mpcc);
    /* the field layout of set A for both, as DC does */
    cm_helper_program_color_matrices(d, regval, base, MPCC_OGAM0_MPC_GAMUT_REMAP_C11_C12_A__MPCC_GAMUT_REMAP_C11_A_MASK,
                                     MPCC_OGAM0_MPC_GAMUT_REMAP_C11_C12_A__MPCC_GAMUT_REMAP_C11_A__SHIFT,
                                     MPCC_OGAM0_MPC_GAMUT_REMAP_C11_C12_A__MPCC_GAMUT_REMAP_C12_A_MASK,
                                     MPCC_OGAM0_MPC_GAMUT_REMAP_C11_C12_A__MPCC_GAMUT_REMAP_C12_A__SHIFT);
    rdna3_wreg(d, mode, FLD(MPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, MPCC_GAMUT_REMAP_MODE, selection));
}

/* mpc3_set_gamut_remap */
void dcn32_mpc_set_gamut_remap(rdna3_dev *d, uint32_t mpcc, const rd_fixpt *matrix)
{
    uint16_t regval[12];
    uint32_t cur;
    if (!matrix) {
        program_gamut_remap(d, mpcc, NULL, RD_GAMUT_REMAP_BYPASS);
        return;
    }
    dcn32_convert_float_matrix(regval, matrix, 12);
    /* the set in use now: write the other one (A after bypass or B) */
    cur = GET(rdna3_rreg(d, AR(d, regMPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, GAMUT_STRIDE, mpcc)),
              MPCC_OGAM0_MPCC_GAMUT_REMAP_MODE, MPCC_GAMUT_REMAP_MODE_CURRENT);
    program_gamut_remap(d, mpcc, regval, cur == 0 ? 1 : cur == 1 ? 2 : 1);
}

/* mpc3_mpc_init_single_inst -> mpc1_mpc_init_single_inst + the OPP's
 * output rate / flow control off */
void dcn32_mpc_init_single_inst(rdna3_dev *d, uint32_t mpcc)
{
    uint32_t opp_id = GET(rdna3_rreg(d, AR(d, regMPCC0_MPCC_OPP_ID, MPCC_STRIDE, mpcc)), MPCC0_MPCC_OPP_ID, MPCC_OPP_ID);
    uint32_t a;
    rdna3_wreg(d, AR(d, regMPCC0_MPCC_TOP_SEL, MPCC_STRIDE, mpcc), FLD(MPCC0_MPCC_TOP_SEL, MPCC_TOP_SEL, 0xf));
    rdna3_wreg(d, AR(d, regMPCC0_MPCC_BOT_SEL, MPCC_STRIDE, mpcc), FLD(MPCC0_MPCC_BOT_SEL, MPCC_BOT_SEL, 0xf));
    rdna3_wreg(d, AR(d, regMPCC0_MPCC_OPP_ID, MPCC_STRIDE, mpcc), FLD(MPCC0_MPCC_OPP_ID, MPCC_OPP_ID, 0xf));
    rdna3_wreg(d, AR(d, regMPCC0_MPCC_UPDATE_LOCK_SEL, MPCC_STRIDE, mpcc),
               FLD(MPCC0_MPCC_UPDATE_LOCK_SEL, MPCC_UPDATE_LOCK_SEL, 0xf));
    if (opp_id < 4) {   /* MAX_OPP on this ASIC: 4 */
        a = AR(d, regMPC_OUT0_MUX, MUX_STRIDE, opp_id);
        rdna3_wreg(d, a, SET(rdna3_rreg(d, a), MPC_OUT0_MUX, MPC_OUT_MUX, 0xf));
    }
    /* "assuming mpc out mux is connected to opp with the same index" */
    if (mpcc < 4) {
        a = AR(d, regMPC_OUT0_MUX, MUX_STRIDE, mpcc);
        rdna3_wreg(d, a, SET(SET(rdna3_rreg(d, a), MPC_OUT0_MUX, MPC_OUT_RATE_CONTROL_DISABLE, 1), MPC_OUT0_MUX,
                             MPC_OUT_FLOW_CONTROL_COUNT, 0));
    }
}
