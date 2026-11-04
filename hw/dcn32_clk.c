/*
 * dcn32_clk.c - see dcn32_clk.h. Ported from Linux amdgpu Display Core
 * (clk_mgr/dcn32/dcn32_clk_mgr.c, dcn32_clk_mgr_smu_msg.c,
 * dcn30_clk_mgr_smu_msg.c, dce100/dce_clk_mgr.c, dccg/dcn20/dcn20_dccg.c,
 * dml/dcn32/display_mode_vba_util_32.c), MIT: dcn32_init_clocks (its
 * DISPCLK / DPPCLK part), dcn32_update_clocks (its raise half, without the
 * divider-127 pixel drop of dcn32_update_clocks_update_dentist),
 * dcn32_update_dppclk_dispclk_freq, dccg2_update_dpp_dto,
 * dml32_CalculateRequiredDispclk, dml32_CalculateDPPCLK.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_clk.h"
#include "gen/dcn_3_2_0_pipe.h"

static void udelay(rdna3_dev *d, uint32_t us) { d->os.udelay(d->os.ctx, us); }

uint32_t dcn32_dentist_divider(uint32_t did)
{
    if (did < 0x08)
        did = 0x08;
    if (did > 0x7f)
        did = 0x7f;
    if (did < 0x40)
        return 8 + 1 * (did - 0x08);       /* 2.00 .. 15.75, step 0.25 */
    if (did < 0x60)
        return 64 + 2 * (did - 0x40);      /* 16.0 .. 31.5, step 0.5 */
    if (did < 0x7e)
        return 128 + 4 * (did - 0x60);     /* 32 .. 61, step 1 */
    return 248 + 264 * (did - 0x7e);       /* 62, 128 */
}

/* Linux v6.12 dcn32_clk_mgr_construct / dcn32_dump_clk_registers. */
static int clk0_reg(rdna3_dev *d, uint32_t *reg)
{
    const rdna3_ipver *gc = &d->ipver[HWIP_GC];
    if (gc->present && gc->major == 11 && gc->minor == 0 &&
        (gc->rev == 0 || gc->rev == 2 || gc->rev == 3)) {
        *reg = gc->rev == 2 ? RD_CLK01_CLK0_CLK0_DFS_CNTL : RD_CLK1_CLK0_DFS_CNTL;
        return RDNA3_OK;
    }
    RERR(d, "DCN clocks: unsupported GC discovery (present %u, IP %u.%u.%u)",
         gc->present, gc->major, gc->minor, gc->rev);
    return RDNA3_ENODEV;
}

int dcn32_clk_read(rdna3_dev *d, rdna3_dcn_clocks *c)
{
    uint32_t pll, base;
    int ret = clk0_reg(d, &base);
    uint32_t *out[5] = { &c->dispclk_khz, &c->dppclk_khz, &c->dprefclk_khz, &c->dcfclk_khz,
                         &c->dtbclk_khz };
    uint64_t fx;
    unsigned i;
    if (ret) return ret;
    pll = rdna3_rreg(d, RD_CLK1_CLK_PLL_REQ);
    /* FbMult_int [8:0] . FbMult_frac [31:16] (a 32-bit fraction) x DFS ref */
    fx = ((uint64_t)(pll & 0x1ff) << 32) | (pll & 0xffff0000u);
    c->vco_khz = (uint32_t)((fx * RD_DCN32_DFS_REF_KHZ) >> 32);
    if (!c->vco_khz)
        c->vco_khz = RD_DCN32_VCO_DEFAULT_KHZ;
    for (i = 0; i < 5; i++) {
        uint32_t did = rdna3_rreg(d, base + RD_CLK1_DFS_STRIDE * i);
        /* DENTIST_DIVIDER_RANGE_SCALE_FACTOR (4) x VCO / divider */
        *out[i] = (uint32_t)(4ull * c->vco_khz / dcn32_dentist_divider(did & 0x7f));
    }
    return RDNA3_OK;
}

/* dcn32_smu_wait_for_response: until RESP is non-zero, 10 us x 200000 */
static uint32_t dal_wait(rdna3_dev *d)
{
    uint32_t r = 0, n;
    for (n = 0; n <= 200000; n++) {
        r = rdna3_rreg(d, RD_DAL_RESP_REG);
        if (r)
            break;
        udelay(d, 10);
    }
    return r;
}

/* dcn32_smu_wait_for_response_delay: dal_wait adding up the time it waited */
static uint32_t dal_wait_delay(rdna3_dev *d, uint32_t *total_delay_us)
{
    uint32_t r = 0, n;
    *total_delay_us = 0;
    for (n = 0; n <= 200000; n++) {
        r = rdna3_rreg(d, RD_DAL_RESP_REG);
        if (r)
            break;
        udelay(d, 10);
        *total_delay_us += 10;
    }
    return r;
}

/* Linux v6.12 dcn32_smu_send_msg_with_param is shared by DCN320/321.
 * Its DAL IF 0x18 is independent from the MP1 driver IF (0x3d / 0x35). */
static int dal_send(rdna3_dev *d, uint32_t msg, uint32_t arg, uint32_t *out, uint32_t *total_delay_us)
{
    const rdna3_ipver *mp1 = &d->ipver[HWIP_MP1];
    uint32_t r, delay1_us = 0, delay2_us = 0;
    if (total_delay_us)
        *total_delay_us = 2000000; /* a refusal must also end the status wait */
    if (!mp1->present || !mp1->num_base || mp1->major != 13 || mp1->minor != 0 ||
        (mp1->rev != 0 && mp1->rev != 7 && mp1->rev != 10)) {
        RERR(d, "DAL SMU: unsupported MP1 discovery (present %u, segments %u, IP %u.%u.%u)",
             mp1->present, mp1->num_base, mp1->major, mp1->minor, mp1->rev);
        return RDNA3_ENODEV;
    }
    if (total_delay_us)
        r = dal_wait_delay(d, &delay1_us);
    else
        r = dal_wait(d);
    if (mp1->rev == 7 && !r) {
        RERR(d, "DAL SMU: not ready for message 0x%x", msg);
        return RDNA3_ETIME;
    }
    rdna3_wreg(d, RD_DAL_RESP_REG, 0);
    rdna3_wreg(d, RD_DAL_ARG_REG, arg);
    rdna3_wreg(d, RD_DAL_MSG_REG, msg);
    r = total_delay_us ? dal_wait_delay(d, &delay2_us) : dal_wait(d);
    if (total_delay_us)
        *total_delay_us = delay1_us + (r == RD_DALSMC_RESULT_OK ? delay2_us : 2000000);
    if (r != RD_DALSMC_RESULT_OK) {
        RERR(d, "DAL SMU message 0x%x(0x%x): %s 0x%x", msg, arg, r ? "answer" : "no answer", r);
        return r ? RDNA3_EIO : RDNA3_ETIME;
    }
    if (out)
        *out = rdna3_rreg(d, RD_DAL_ARG_REG);
    return RDNA3_OK;
}

int dcn32_smu_send(rdna3_dev *d, uint32_t msg, uint32_t arg, uint32_t *out)
{
    return dal_send(d, msg, arg, out, NULL);
}

/* dcn32_smu_send_msg_with_param_delay: 1 when the PMFW answered OK; the
 * time spent (2 s added when it did not) */
static int dcn32_smu_send_delay(rdna3_dev *d, uint32_t msg, uint32_t arg, uint32_t *out, uint32_t *total_delay_us)
{
    return dal_send(d, msg, arg, out, total_delay_us) == RDNA3_OK;
}

/* dcn32_smu_get_hard_min_status: the clocks whose hard minimum is in place */
static uint32_t dcn32_smu_get_hard_min_status(rdna3_dev *d, int *no_timeout, uint32_t *total_delay_us)
{
    uint32_t response = 0;
    *no_timeout = dcn32_smu_send_delay(d, RD_DALSMC_MSG_ReturnHardMinStatus, 0, &response, total_delay_us);
    return response;
}

int dcn32_smu_num_displays(rdna3_dev *d, uint32_t n)
{
    RLOG(d, "DAL SMU: %u display(s)", n);
    return dcn32_smu_send(d, RD_DALSMC_MSG_NumOfDisplays, n, NULL);
}

int dcn32_smu_set_hard_min(rdna3_dev *d, uint32_t ppclk, uint32_t mhz, uint32_t pmfw,
                           uint32_t *got_khz)
{
    return dcn32_smu_set_hard_min_ex(d, ppclk, mhz, pmfw, got_khz, NULL);
}

int dcn32_smu_set_hard_min_ex(rdna3_dev *d, uint32_t ppclk, uint32_t mhz, uint32_t pmfw,
                              uint32_t *got_khz, int *taken)
{
    uint32_t resp = 0, check = RD_CHECK_HARD_MIN_CLK_DPREFCLK, total_delay_us = 0, read_delay_us, bits = 0;
    int no_timeout = 0;
    int r = dcn32_smu_send(d, RD_DALSMC_MSG_SetHardMinByFreq, (ppclk << 16) | (mhz & 0xffff), &resp);
    if (got_khz)
        *got_khz = resp;
    if (taken)
        *taken = r == RDNA3_OK;
    if (r)
        return r;
    /* dcn32_get_hard_min_status_supported (Linux v6.12) and
     * dcn32_smu_wait_get_hard_min_status: the PMFW reports which hard
     * minimums are in place; give it up to 2 s */
    {
        const rdna3_ipver *gc = &d->ipver[HWIP_GC];
        uint32_t minimum;
        if (!gc->present || gc->major != 11 || gc->minor != 0) return RDNA3_OK;
        if (gc->rev == 0) minimum = 0x4e6a00;
        else if (gc->rev == 2) minimum = 0x524e00;
        else if (gc->rev == 3) minimum = 0x503900;
        else return RDNA3_OK;
        if (pmfw < minimum) return RDNA3_OK;
    }
    if (ppclk == RD_PPCLK_DISPCLK)
        check |= RD_CHECK_HARD_MIN_CLK_DISPCLK;
    else if (ppclk == RD_PPCLK_DPPCLK)
        check |= RD_CHECK_HARD_MIN_CLK_DPPCLK;
    else if (ppclk == RD_PPCLK_DCFCLK)
        check |= RD_CHECK_HARD_MIN_CLK_DCFCLK;
    else if (ppclk == RD_PPCLK_DTBCLK)
        check |= RD_CHECK_HARD_MIN_CLK_DTBCLK;
    else if (ppclk == 2 /* PPCLK_UCLK */)
        check |= RD_CHECK_HARD_MIN_CLK_UCLK;
    else
        return RDNA3_OK;
    for (;;) {
        bits = dcn32_smu_get_hard_min_status(d, &no_timeout, &read_delay_us);
        total_delay_us += read_delay_us;
        if ((bits & check) == check)
            break;
        if (total_delay_us >= 2000000) {
            RERR(d, "DAL SMU: hard min %u MHz on clock %u not confirmed (status 0x%x, answered %d)", mhz, ppclk,
                 bits, no_timeout);
            return RDNA3_ETIME;
        }
        d->os.msleep(d->os.ctx, 1);
        total_delay_us += 1000;
    }

    RLOG(d, "DAL SMU: hard min clock %u = %u MHz, PMFW set %u kHz", ppclk, mhz, resp);
    return RDNA3_OK;
}

int dcn32_clk_fits(const rdna3_dcn_clocks *c, uint32_t pclk_khz)
{
    return c->dispclk_khz >= pclk_khz && c->dppclk_khz >= pclk_khz ? RDNA3_OK : RDNA3_EBUSY;
}

/* --- the raise path of dcn32_update_clocks (rdna3dispclk=1) --- */

#define DTO_S (regDPPCLK1_DTO_PARAM - regDPPCLK0_DTO_PARAM)

static uint32_t dccg_rd(rdna3_dev *d, uint32_t off)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, regDPPCLK_DTO_CTRL_BASE_IDX, off));
}

static void dccg_wr(rdna3_dev *d, uint32_t off, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, regDPPCLK_DTO_CTRL_BASE_IDX, off), v);
}

/* dcn32_init_single_clock: level 0xff gives the count (bit 31: fine
 * grained, min and max only), then each level's MHz */
static int single_clock(rdna3_dev *d, uint32_t ppclk, uint32_t *levels, uint32_t *max_khz)
{
    uint32_t v = 0, n, i, f;
    int r = dcn32_smu_send(d, RD_DALSMC_MSG_GetDpmFreqByIndex, (ppclk << 16) | 0xff, &v);
    *levels = 0;
    *max_khz = 0;
    if (r)
        return r;
    n = (v & (1u << 31)) ? 2 : (v & 0xff);
    for (i = 0; i < n; i++) {
        r = dcn32_smu_send(d, RD_DALSMC_MSG_GetDpmFreqByIndex, (ppclk << 16) | i, &v);
        if (r)
            return r;
        f = (v & 0xffff) * 1000;
        if (f > RD_DCN32_DISPCLK_LIMIT_KHZ)
            f = RD_DCN32_DISPCLK_LIMIT_KHZ;
        if (f > *max_khz)
            *max_khz = f;
    }
    *levels = n;
    return RDNA3_OK;
}

int dcn32_clk_dtbclk_enable(rdna3_dev *d, uint32_t pmfw, uint32_t *ref_khz)
{
    uint32_t levels = 0, max_khz = 0, got = 0;
    int r = single_clock(d, RD_PPCLK_DTBCLK, &levels, &max_khz);
    *ref_khz = 0;
    if (r)
        return r;
    if (!levels || !max_khz)
        return RDNA3_ENODEV;
    /* khz_to_mhz_ceil */
    r = dcn32_smu_set_hard_min(d, RD_PPCLK_DTBCLK, (max_khz + 999) / 1000, pmfw, &got);
    if (r)
        return r;
    *ref_khz = got ? got : max_khz;
    RLOG(d, "DAL SMU: DTBCLK %u levels up to %u kHz: reference %u kHz", levels, max_khz, *ref_khz);
    return RDNA3_OK;
}

int dcn32_clk_limits_read(rdna3_dev *d, rdna3_dcn_clk_limits *l)
{
    int r;
    l->pmfw = 0;
    l->dispclk_levels = l->dppclk_levels = l->dispclk_max_khz = l->dppclk_max_khz = 0;
    /* dcn32_clk_mgr_construct: smu_present when GetSmuVersion answers
     * (dcn32_is_smu_present); the limits need it */
    r = dcn32_smu_send(d, RD_DALSMC_MSG_GetSmuVersion, 0, &l->pmfw);
    if (!r)
        r = single_clock(d, RD_PPCLK_DISPCLK, &l->dispclk_levels, &l->dispclk_max_khz);
    if (!r)
        r = single_clock(d, RD_PPCLK_DPPCLK, &l->dppclk_levels, &l->dppclk_max_khz);
    RLOG(d, "DAL SMU: PMFW 0x%08x, DISPCLK %u levels up to %u kHz, DPPCLK %u levels up to %u kHz (%d)", l->pmfw,
         l->dispclk_levels, l->dispclk_max_khz, l->dppclk_levels, l->dppclk_max_khz, r);
    return r;
}

uint32_t dcn32_dfs_round(uint32_t vco_khz, uint64_t clk_khz_x1e6, int up)
{
    uint64_t num = 4ull * vco_khz * 1000000ull, div;
    if (!clk_khz_x1e6 || !vco_khz)
        return 0;
    div = up ? num / clk_khz_x1e6 : (num + clk_khz_x1e6 - 1) / clk_khz_x1e6;
    if (!div)
        div = 1;
    return (uint32_t)(4ull * vco_khz / div);
}

uint32_t dcn32_required_dispclk(uint32_t pclk_khz, uint32_t vco_khz, uint32_t max_khz)
{
    /* x 1e6: pclk x (1000 + 5) / 1000 x (100 + 1) / 100 */
    uint64_t with = (uint64_t)pclk_khz * (1000 + RD_DCN32_DOWNSPREAD_PER_MILLE) * (100 + RD_DCN32_RAMP_PERCENT) * 10;
    uint64_t without = (uint64_t)pclk_khz * (1000 + RD_DCN32_DOWNSPREAD_PER_MILLE) * 1000;
    uint32_t r_with = dcn32_dfs_round(vco_khz, with, 1), r_without = dcn32_dfs_round(vco_khz, without, 1), r_max;
    if (!max_khz)
        return r_with;
    r_max = dcn32_dfs_round(vco_khz, (uint64_t)max_khz * 1000000ull, 0);
    if (r_without > r_max)
        return 0;   /* dml32 returns it, mode support then fails */
    return r_with > r_max ? r_max : r_with;
}

uint32_t dcn32_required_dppclk(uint32_t pclk_khz, uint32_t vco_khz)
{
    return dcn32_dfs_round(vco_khz, (uint64_t)pclk_khz * (1000 + RD_DCN32_DOWNSPREAD_PER_MILLE) * 1000, 1);
}

uint32_t dcn32_pipe_dppclk(rdna3_dev *d, const rdna3_dcn_clocks *c, uint32_t pipe)
{
    uint32_t ctl = dccg_rd(d, regDPPCLK_DTO_CTRL), p, phase, modulo;
    if (pipe >= 4 || !((ctl >> (4 * pipe)) & DPPCLK_DTO_CTRL__DPPCLK0_DTO_ENABLE_MASK))
        return c->dppclk_khz;   /* DTO off: the pipe runs at the global DPPCLK */
    p = dccg_rd(d, regDPPCLK0_DTO_PARAM + DTO_S * pipe);
    phase = (p & DPPCLK0_DTO_PARAM__DPPCLK0_DTO_PHASE_MASK) >> DPPCLK0_DTO_PARAM__DPPCLK0_DTO_PHASE__SHIFT;
    modulo = (p & DPPCLK0_DTO_PARAM__DPPCLK0_DTO_MODULO_MASK) >> DPPCLK0_DTO_PARAM__DPPCLK0_DTO_MODULO__SHIFT;
    if (!modulo || phase >= modulo)
        return c->dppclk_khz;
    return (uint32_t)((uint64_t)c->dppclk_khz * phase / modulo);
}

/* dcn32_update_dppclk_dispclk_freq: to the dentist's integer divider */
static uint32_t dentist_round(uint32_t vco_khz, uint32_t khz)
{
    uint32_t div;
    if (!khz)
        return 0;
    div = (uint32_t)(4ull * vco_khz / khz);
    return div ? (uint32_t)(4ull * vco_khz / div) : 4 * vco_khz;
}

int dcn32_clk_raise(rdna3_dev *d, uint32_t pmfw, uint32_t pipe, uint32_t dispclk_khz, uint32_t dppclk_khz,
                    rdna3_dcn_clocks *now)
{
    rdna3_dcn_clocks c0, c1;
    uint32_t disp, dpp, pipe_khz, base;
    int r = clk0_reg(d, &base);
    if (r) return r;

    r = dcn32_clk_read(d, &c0);
    if (r) return r;
    disp = dentist_round(c0.vco_khz, dispclk_khz);
    dpp = dentist_round(c0.vco_khz, dppclk_khz);
    /* dcn32_update_clocks_update_dentist: leaving divider 127 needs every
     * stream's OTG pixel drop dance (not ported): refused */
    if (disp > c0.dispclk_khz && (rdna3_rreg(d, base) & 0x7f) == 127) {
        RERR(d, "DCN clocks: DISPCLK at divider 127, not raised");
        return RDNA3_EBUSY;
    }
    /* "if clock is being raised, increase refclk before lowering DTO":
     * DPPCLK's hard min, the dentist (DISPCLK's), then the DTO */
    if (dpp > c0.dppclk_khz)
        r = dcn32_smu_set_hard_min(d, RD_PPCLK_DPPCLK, dpp / 1000, pmfw, NULL);
    if (!r && disp > c0.dispclk_khz)
        r = dcn32_smu_set_hard_min(d, RD_PPCLK_DISPCLK, disp / 1000, pmfw, NULL);
    {
        int cr = dcn32_clk_read(d, &c1);
        if (!cr && now) *now = c1;
        if (r) return r;
        if (cr) return cr;
    }
    if (c1.dispclk_khz < disp / 1000 * 1000 || c1.dppclk_khz < dpp / 1000 * 1000) {
        RERR(d, "DCN clocks: asked DISPCLK %u DPPCLK %u kHz, the PMFW set %u / %u kHz", disp, dpp, c1.dispclk_khz,
             c1.dppclk_khz);
        return RDNA3_EBUSY;
    }
    /* dccg2_update_dpp_dto: phase / modulo = pipe clock / global clock,
     * modulo 0xff, phase rounded up; the parameters, then the enable */
    pipe_khz = dcn32_pipe_dppclk(d, &c1, pipe);
    if (pipe < 4 && dppclk_khz && pipe_khz < dppclk_khz && c1.dppclk_khz) {
        uint32_t phase = (uint32_t)((255ull * dppclk_khz + c1.dppclk_khz - 1) / c1.dppclk_khz);
        if (phase > 255)
            phase = 255;
        dccg_wr(d, regDPPCLK0_DTO_PARAM + DTO_S * pipe, phase | 255u << DPPCLK0_DTO_PARAM__DPPCLK0_DTO_MODULO__SHIFT);
        dccg_wr(d, regDPPCLK_DTO_CTRL,
                dccg_rd(d, regDPPCLK_DTO_CTRL) | (DPPCLK_DTO_CTRL__DPPCLK0_DTO_ENABLE_MASK << (4 * pipe)));
        RLOG(d, "DCN clocks: pipe %u DPPCLK DTO %u/255 (%u -> %u kHz)", pipe, phase, pipe_khz,
             (uint32_t)((uint64_t)c1.dppclk_khz * phase / 255));
    }
    RLOG(d, "DCN clocks: DISPCLK %u -> %u kHz, DPPCLK %u -> %u kHz (asked %u / %u)", c0.dispclk_khz, c1.dispclk_khz,
         c0.dppclk_khz, c1.dppclk_khz, disp, dpp);
    return RDNA3_OK;
}
