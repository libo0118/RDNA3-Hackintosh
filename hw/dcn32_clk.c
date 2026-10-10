/*
 * dcn32_clk.c - see dcn32_clk.h. Ported from Linux amdgpu Display Core
 * (clk_mgr/dcn32/dcn32_clk_mgr.c, dcn32_clk_mgr_smu_msg.c,
 * dcn30_clk_mgr_smu_msg.c, dce100/dce_clk_mgr.c), MIT.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_clk.h"

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

void dcn32_clk_read(rdna3_dev *d, rdna3_dcn_clocks *c)
{
    uint32_t pll = rdna3_rreg(d, RD_CLK1_CLK_PLL_REQ);
    uint32_t *out[5] = { &c->dispclk_khz, &c->dppclk_khz, &c->dprefclk_khz, &c->dcfclk_khz,
                         &c->dtbclk_khz };
    /* FbMult_int [8:0] . FbMult_frac [31:16] (a 32-bit fraction) x DFS ref */
    uint64_t fx = ((uint64_t)(pll & 0x1ff) << 32) | (pll & 0xffff0000u);
    unsigned i;
    c->vco_khz = (uint32_t)((fx * RD_DCN32_DFS_REF_KHZ) >> 32);
    if (!c->vco_khz)
        c->vco_khz = RD_DCN32_VCO_DEFAULT_KHZ;
    for (i = 0; i < 5; i++) {
        uint32_t did = rdna3_rreg(d, RD_CLK1_CLK0_DFS_CNTL + RD_CLK1_DFS_STRIDE * i);
        /* DENTIST_DIVIDER_RANGE_SCALE_FACTOR (4) x VCO / divider */
        *out[i] = (uint32_t)(4ull * c->vco_khz / dcn32_dentist_divider(did & 0x7f));
    }
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

int dcn32_smu_send(rdna3_dev *d, uint32_t msg, uint32_t arg, uint32_t *out)
{
    uint32_t r;
    dal_wait(d);
    rdna3_wreg(d, RD_DAL_RESP_REG, 0);
    rdna3_wreg(d, RD_DAL_ARG_REG, arg);
    rdna3_wreg(d, RD_DAL_MSG_REG, msg);
    r = dal_wait(d);
    if (r != RD_DALSMC_RESULT_OK) {
        RERR(d, "DAL SMU message 0x%x(0x%x): %s 0x%x", msg, arg, r ? "answer" : "no answer", r);
        return r ? RDNA3_EIO : RDNA3_ETIME;
    }
    if (out)
        *out = rdna3_rreg(d, RD_DAL_ARG_REG);
    return RDNA3_OK;
}

int dcn32_smu_num_displays(rdna3_dev *d, uint32_t n)
{
    RLOG(d, "DAL SMU: %u display(s)", n);
    return dcn32_smu_send(d, RD_DALSMC_MSG_NumOfDisplays, n, NULL);
}

int dcn32_smu_set_hard_min(rdna3_dev *d, uint32_t ppclk, uint32_t mhz, uint32_t pmfw,
                           uint32_t *got_khz)
{
    uint32_t resp = 0, check = RD_CHECK_HARD_MIN_CLK_DPREFCLK, waited = 0;
    int r = dcn32_smu_send(d, RD_DALSMC_MSG_SetHardMinByFreq, (ppclk << 16) | (mhz & 0xffff), &resp);
    if (got_khz)
        *got_khz = resp;
    if (r)
        return r;
    /* dcn32_get_hard_min_status_supported (GC 11.0.3) and
     * dcn32_smu_wait_get_hard_min_status: the PMFW reports which hard
     * minimums are in place; give it up to 2 s */
    if (pmfw < 0x503900)
        return RDNA3_OK;
    if (ppclk == RD_PPCLK_DISPCLK)
        check |= RD_CHECK_HARD_MIN_CLK_DISPCLK;
    else if (ppclk == RD_PPCLK_DPPCLK)
        check |= RD_CHECK_HARD_MIN_CLK_DPPCLK;
    else if (ppclk == RD_PPCLK_DCFCLK)
        check |= RD_CHECK_HARD_MIN_CLK_DCFCLK;
    else
        return RDNA3_OK;
    for (;;) {
        uint32_t bits = 0;
        if (dcn32_smu_send(d, RD_DALSMC_MSG_ReturnHardMinStatus, 0, &bits) == RDNA3_OK &&
            (bits & check) == check)
            break;
        if (waited >= 2000) {
            RERR(d, "DAL SMU: hard min %u MHz on clock %u not confirmed (status 0x%x)", mhz, ppclk, bits);
            return RDNA3_ETIME;
        }
        d->os.msleep(d->os.ctx, 1);
        waited++;
    }
    RLOG(d, "DAL SMU: hard min clock %u = %u MHz, PMFW set %u kHz", ppclk, mhz, resp);
    return RDNA3_OK;
}

int dcn32_clk_fits(const rdna3_dcn_clocks *c, uint32_t pclk_khz)
{
    return c->dispclk_khz >= pclk_khz && c->dppclk_khz >= pclk_khz ? RDNA3_OK : RDNA3_EBUSY;
}
