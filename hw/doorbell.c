/*
 * doorbell.c - see doorbell.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "doorbell.h"
#include "gen/nbio_4_3_0_doorbell.h"

#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field)  reg##__##field##_MASK
#define REG_SET_FIELD(orig, reg, field, val) \
    (((orig) & ~(uint32_t)REG_FIELD_MASK(reg, field)) | \
     ((uint32_t)REG_FIELD_MASK(reg, field) & ((uint32_t)(val) << REG_FIELD_SHIFT(reg, field))))

#define RNB(d, reg) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_NBIO, 0, reg##_BASE_IDX, reg))
#define WNB(d, reg, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_NBIO, 0, reg##_BASE_IDX, reg), (v))

int nbio43_doorbell_init(rdna3_dev *d)
{
    uint32_t tmp;
    if (!d->doorbell_len || (!d->doorbell_cpu && !d->os.doorbell_wr)) {
        d->doorbell_ok = 0;
        return RDNA3_ENODEV;
    }
    /* nbio_v4_3_enable_doorbell_aperture */
    tmp = RNB(d, regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN);
    tmp = REG_SET_FIELD(tmp, RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN, BIF_DOORBELL_APER_EN, 1);
    WNB(d, regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN, tmp);
    /* nbio_v4_3_enable_doorbell_selfring_aperture */
    tmp = REG_SET_FIELD(0u, BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL, DOORBELL_SELFRING_GPA_APER_EN, 1);
    tmp = REG_SET_FIELD(tmp, BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL, DOORBELL_SELFRING_GPA_APER_MODE, 1);
    tmp = REG_SET_FIELD(tmp, BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL, DOORBELL_SELFRING_GPA_APER_SIZE, 0);
    WNB(d, regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW, (uint32_t)d->doorbell_bus);
    WNB(d, regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH, (uint32_t)(d->doorbell_bus >> 32));
    WNB(d, regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL, tmp);
    d->doorbell_ok = 1;
    RLOG(d, "doorbells: aperture %llu KiB at bus 0x%llx enabled",
         (unsigned long long)(d->doorbell_len / 1024), (unsigned long long)d->doorbell_bus);
    return RDNA3_OK;
}

void nbio43_gc_doorbell_init(rdna3_dev *d)
{
    WNB(d, regS2A_DOORBELL_ENTRY_0_CTRL, 0x30000007);
    WNB(d, regS2A_DOORBELL_ENTRY_3_CTRL, 0x3000000d);
}

void nbio43_sdma_doorbell_range(rdna3_dev *d, uint32_t inst, int use, uint32_t index,
                                uint32_t size)
{
    uint32_t r;
    if (inst != 0)
        return;
    r = RNB(d, regS2A_DOORBELL_ENTRY_2_CTRL);
    if (use) {
        r = REG_SET_FIELD(r, S2A_DOORBELL_ENTRY_2_CTRL, S2A_DOORBELL_PORT2_ENABLE, 1);
        r = REG_SET_FIELD(r, S2A_DOORBELL_ENTRY_2_CTRL, S2A_DOORBELL_PORT2_AWID, 0xe);
        r = REG_SET_FIELD(r, S2A_DOORBELL_ENTRY_2_CTRL, S2A_DOORBELL_PORT2_RANGE_OFFSET, index);
        r = REG_SET_FIELD(r, S2A_DOORBELL_ENTRY_2_CTRL, S2A_DOORBELL_PORT2_RANGE_SIZE, size);
        r = REG_SET_FIELD(r, S2A_DOORBELL_ENTRY_2_CTRL, S2A_DOORBELL_PORT2_AWADDR_31_28_VALUE, 0x3);
    } else {
        r = REG_SET_FIELD(r, S2A_DOORBELL_ENTRY_2_CTRL, S2A_DOORBELL_PORT2_RANGE_SIZE, 0);
    }
    WNB(d, regS2A_DOORBELL_ENTRY_2_CTRL, r);
}
