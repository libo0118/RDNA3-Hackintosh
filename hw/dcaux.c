/*
 * dcaux.c - see dcaux.h. Port of dce_aux.c (acquire_engine, submit_channel_
 * request, get_channel_status, read_channel_reply, dce_aux_transfer_with_
 * retries) and the HPD pin read.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcaux.h"
#include "gen/dcn_3_2_1_aux.h"

#define AUX_STRIDE (regDP_AUX1_AUX_CONTROL - regDP_AUX0_AUX_CONTROL)
#define HPD_STRIDE (regHPD1_DC_HPD_CONTROL - regHPD0_DC_HPD_CONTROL)

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))

#define RA(d, reg, n) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + AUX_STRIDE * (n))
#define WA(d, reg, n, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + AUX_STRIDE * (n), (v))
#define RHPD(d, reg, n) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + HPD_STRIDE * (n))
#define WHPD(d, reg, n, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + HPD_STRIDE * (n), (v))

#define SW_CAN_ACCESS_AUX   1
#define DMCU_CAN_ACCESS_AUX 2
#define AUX_TIMEOUT_US      2000     /* SW_AUX_TIMEOUT_PERIOD_MULTIPLIER * 400 us, generous */
#define AUX_MAX_RETRIES     7
#define AUX_DEFER_DELAY_US  400

int dcaux_hpd_sense(rdna3_dev *d, uint32_t n)
{
    uint32_t v;
    if (!d->ipver[HWIP_DCE].present || n >= RD_AUX_ENGINES)
        return 0;
    /* dal_hw_hpd_... : the pin has to be enabled to sense */
    v = RHPD(d, regHPD0_DC_HPD_CONTROL, n);
    if (!GET(v, HPD0_DC_HPD_CONTROL, DC_HPD_EN)) {
        v = SET(v, HPD0_DC_HPD_CONTROL, DC_HPD_EN, 1);
        WHPD(d, regHPD0_DC_HPD_CONTROL, n, v);
        d->os.udelay(d->os.ctx, 100);
    }
    v = RHPD(d, regHPD0_DC_HPD_INT_STATUS, n);
    return GET(v, HPD0_DC_HPD_INT_STATUS, DC_HPD_SENSE) ? 1 : 0;
}

/* acquire_engine: SW asks the arbiter for the AUX registers */
static int aux_acquire(rdna3_dev *d, uint32_t n)
{
    uint32_t v = RA(d, regDP_AUX0_AUX_ARB_CONTROL, n), i;
    if (GET(v, DP_AUX0_AUX_ARB_CONTROL, AUX_REG_RW_CNTL_STATUS) == DMCU_CAN_ACCESS_AUX)
        return RD_AUX_ERR_ACQUIRE;
    v = RA(d, regDP_AUX0_AUX_CONTROL, n);
    if (!GET(v, DP_AUX0_AUX_CONTROL, AUX_EN)) {
        /* enable with a reset pulse (DP_AUX block as part of the enable sequence) */
        v = SET(v, DP_AUX0_AUX_CONTROL, AUX_EN, 1);
        v = SET(v, DP_AUX0_AUX_CONTROL, AUX_RESET, 1);
        WA(d, regDP_AUX0_AUX_CONTROL, n, v);
        for (i = 0; i < 11; i++) {
            if (GET(RA(d, regDP_AUX0_AUX_CONTROL, n), DP_AUX0_AUX_CONTROL, AUX_RESET_DONE))
                break;
            d->os.udelay(d->os.ctx, 1);
        }
        v = SET(v, DP_AUX0_AUX_CONTROL, AUX_RESET, 0);
        WA(d, regDP_AUX0_AUX_CONTROL, n, v);
        for (i = 0; i < 11; i++) {
            if (!GET(RA(d, regDP_AUX0_AUX_CONTROL, n), DP_AUX0_AUX_CONTROL, AUX_RESET_DONE))
                break;
            d->os.udelay(d->os.ctx, 1);
        }
    }
    v = RA(d, regDP_AUX0_AUX_ARB_CONTROL, n);
    v = SET(v, DP_AUX0_AUX_ARB_CONTROL, AUX_SW_USE_AUX_REG_REQ, 1);
    WA(d, regDP_AUX0_AUX_ARB_CONTROL, n, v);
    v = RA(d, regDP_AUX0_AUX_ARB_CONTROL, n);
    return GET(v, DP_AUX0_AUX_ARB_CONTROL, AUX_REG_RW_CNTL_STATUS) == SW_CAN_ACCESS_AUX
               ? RD_AUX_OK : RD_AUX_ERR_ACQUIRE;
}

static void aux_release(rdna3_dev *d, uint32_t n)
{
    uint32_t v = RA(d, regDP_AUX0_AUX_ARB_CONTROL, n);
    v = SET(v, DP_AUX0_AUX_ARB_CONTROL, AUX_SW_DONE_USING_AUX_REG, 1);
    v = SET(v, DP_AUX0_AUX_ARB_CONTROL, AUX_SW_USE_AUX_REG_REQ, 0);
    WA(d, regDP_AUX0_AUX_ARB_CONTROL, n, v);
}

static int wait_sw_done(rdna3_dev *d, uint32_t n, int want)
{
    uint32_t i;
    for (i = 0; i < AUX_TIMEOUT_US / 10; i++) {
        uint32_t v = RA(d, regDP_AUX0_AUX_SW_STATUS, n);
        if ((GET(v, DP_AUX0_AUX_SW_STATUS, AUX_SW_DONE) != 0) == (want != 0))
            return RD_AUX_OK;
        d->os.udelay(d->os.ctx, 10);
    }
    return RD_AUX_ERR_TIMEOUT;
}

int dcaux_transfer(rdna3_dev *d, uint32_t n, uint32_t action, uint32_t address, uint8_t *buf,
                   uint32_t len, uint32_t *reply)
{
    int is_write = action == RD_AUX_DP_WRITE || action == RD_AUX_I2C_WRITE ||
                   action == RD_AUX_I2C_WRITE_MOT;
    uint32_t v, length, i, status, replied;
    int ret;

    if (reply)
        *reply = 0xff;
    if (!d->ipver[HWIP_DCE].present || n >= RD_AUX_ENGINES || len > RD_AUX_MAX_BYTES ||
        (len && !buf))
        return RDNA3_EINVAL;
    ret = aux_acquire(d, n);
    if (ret)
        return ret;

    /* submit_channel_request */
    v = RA(d, regDP_AUX0_AUX_INTERRUPT_CONTROL, n);
    v = SET(v, DP_AUX0_AUX_INTERRUPT_CONTROL, AUX_SW_DONE_ACK, 1);
    WA(d, regDP_AUX0_AUX_INTERRUPT_CONTROL, n, v);
    (void)wait_sw_done(d, n, 0);

    /* 4-bit command + 20-bit address = 3 bytes, a 4th carries length - 1 */
    length = len ? 4 : 3;
    if (is_write)
        length += len;
    v = RA(d, regDP_AUX0_AUX_SW_CONTROL, n);
    v = SET(v, DP_AUX0_AUX_SW_CONTROL, AUX_SW_START_DELAY, 0);
    v = SET(v, DP_AUX0_AUX_SW_CONTROL, AUX_SW_WR_BYTES, length);
    WA(d, regDP_AUX0_AUX_SW_CONTROL, n, v);

    v = 0;
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_INDEX, 0);
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA_RW, 0);
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_AUTOINCREMENT_DISABLE, 1);
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, action | ((address & 0xf0000) >> 16));
    WA(d, regDP_AUX0_AUX_SW_DATA, n, v);
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_AUTOINCREMENT_DISABLE, 0);
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, (address & 0xff00) >> 8);
    WA(d, regDP_AUX0_AUX_SW_DATA, n, v);
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, address & 0xff);
    WA(d, regDP_AUX0_AUX_SW_DATA, n, v);
    if (len) {
        v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, len - 1);
        WA(d, regDP_AUX0_AUX_SW_DATA, n, v);
    }
    if (is_write)
        for (i = 0; i < len; i++) {
            v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, buf[i]);
            WA(d, regDP_AUX0_AUX_SW_DATA, n, v);
        }
    v = RA(d, regDP_AUX0_AUX_SW_CONTROL, n);
    v = SET(v, DP_AUX0_AUX_SW_CONTROL, AUX_SW_GO, 1);
    WA(d, regDP_AUX0_AUX_SW_CONTROL, n, v);

    /* get_channel_status */
    ret = wait_sw_done(d, n, 1);
    status = RA(d, regDP_AUX0_AUX_SW_STATUS, n);
    if (status & DP_AUX0_AUX_SW_STATUS__AUX_SW_HPD_DISCON_MASK) {
        aux_release(d, n);
        return RD_AUX_ERR_HPD_DISCON;
    }
    if (ret || !(status & DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK)) {
        aux_release(d, n);
        return RD_AUX_ERR_TIMEOUT;
    }
    if (status & (DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_STATE_MASK |
                  DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_MASK)) {
        aux_release(d, n);
        return RD_AUX_ERR_TIMEOUT;
    }
    if (status & (DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_INVALID_STOP_MASK |
                  DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_NO_DET_MASK |
                  DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_INVALID_H_MASK |
                  DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_INVALID_L_MASK)) {
        aux_release(d, n);
        return RD_AUX_ERR_INVALID_REPLY;
    }
    replied = GET(status, DP_AUX0_AUX_SW_STATUS, AUX_SW_REPLY_BYTE_COUNT);
    if (!replied) {
        aux_release(d, n);
        return RD_AUX_ERR_INVALID_REPLY;
    }

    /* read_channel_reply: byte 0 is the reply code, the rest the data */
    v = 0;
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_INDEX, 0);
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_AUTOINCREMENT_DISABLE, 1);
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA_RW, 1);
    WA(d, regDP_AUX0_AUX_SW_DATA, n, v);
    v = SET(v, DP_AUX0_AUX_SW_DATA, AUX_SW_AUTOINCREMENT_DISABLE, 0);
    WA(d, regDP_AUX0_AUX_SW_DATA, n, v);
    v = GET(RA(d, regDP_AUX0_AUX_SW_DATA, n), DP_AUX0_AUX_SW_DATA, AUX_SW_DATA) >> 4;
    if (reply)
        *reply = v;
    replied--;
    if (v != RD_AUX_REPLY_ACK) {
        aux_release(d, n);
        return (v == RD_AUX_REPLY_DEFER || v == RD_AUX_REPLY_I2C_DEFER) ? RD_AUX_ERR_DEFER
                                                                         : RD_AUX_ERR_NACK;
    }
    if (is_write) {
        aux_release(d, n);
        return (int)len;
    }
    if (replied > len)
        replied = len;
    for (i = 0; i < replied; i++)
        buf[i] = (uint8_t)GET(RA(d, regDP_AUX0_AUX_SW_DATA, n), DP_AUX0_AUX_SW_DATA, AUX_SW_DATA);
    aux_release(d, n);
    return (int)replied;
}

/* dce_aux_transfer_with_retries: DEFER / timeout / short reads retried */
static int xfer_retry(rdna3_dev *d, uint32_t n, uint32_t action, uint32_t address, uint8_t *buf,
                      uint32_t len)
{
    int i, r = RD_AUX_ERR_TIMEOUT;
    uint32_t reply;
    int is_write = action == RD_AUX_DP_WRITE || action == RD_AUX_I2C_WRITE ||
                   action == RD_AUX_I2C_WRITE_MOT;
    for (i = 0; i < AUX_MAX_RETRIES; i++) {
        r = dcaux_transfer(d, n, action, address, buf, len, &reply);
        if (r >= 0 && (is_write || (uint32_t)r == len))
            return r;
        if (r == RD_AUX_ERR_NACK || r == RD_AUX_ERR_HPD_DISCON || r == RD_AUX_ERR_ACQUIRE ||
            r == RDNA3_EINVAL)
            return r;
        d->os.udelay(d->os.ctx, AUX_DEFER_DELAY_US);
    }
    return r < 0 ? r : RD_AUX_ERR_TIMEOUT;
}

int dcaux_dpcd_read(rdna3_dev *d, uint32_t n, uint32_t address, uint8_t *buf, uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        uint32_t chunk = len - done > RD_AUX_MAX_BYTES ? RD_AUX_MAX_BYTES : len - done;
        int r = xfer_retry(d, n, RD_AUX_DP_READ, address + done, buf + done, chunk);
        if (r < 0)
            return r;
        done += (uint32_t)r;
    }
    return (int)done;
}

int dcaux_dpcd_write(rdna3_dev *d, uint32_t n, uint32_t address, const uint8_t *buf, uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        uint32_t chunk = len - done > RD_AUX_MAX_BYTES ? RD_AUX_MAX_BYTES : len - done;
        uint8_t tmp[RD_AUX_MAX_BYTES];
        uint32_t i;
        int r;
        for (i = 0; i < chunk; i++)
            tmp[i] = buf[done + i];
        r = xfer_retry(d, n, RD_AUX_DP_WRITE, address + done, tmp, chunk);
        if (r < 0)
            return r;
        done += chunk;
    }
    return (int)done;
}

int dcaux_i2c_xfer(rdna3_dev *d, uint32_t n, uint32_t dev7, const uint8_t *wbuf, uint32_t wlen,
                   uint8_t *rbuf, uint32_t rlen)
{
    uint32_t done = 0;
    int r;
    if (wlen) {
        uint8_t tmp[RD_AUX_MAX_BYTES];
        uint32_t i;
        if (wlen > RD_AUX_MAX_BYTES)
            return RDNA3_EINVAL;
        for (i = 0; i < wlen; i++)
            tmp[i] = wbuf[i];
        /* MOT: keep the I2C transaction open for the read that follows */
        r = xfer_retry(d, n, rlen ? RD_AUX_I2C_WRITE_MOT : RD_AUX_I2C_WRITE, dev7, tmp, wlen);
        if (r < 0)
            return r;
    }
    while (done < rlen) {
        uint32_t chunk = rlen - done > RD_AUX_MAX_BYTES ? RD_AUX_MAX_BYTES : rlen - done;
        int last = done + chunk >= rlen;
        r = xfer_retry(d, n, last ? RD_AUX_I2C_READ : RD_AUX_I2C_READ_MOT, dev7, rbuf + done, chunk);
        if (r < 0)
            return r;
        done += (uint32_t)r;
    }
    return (int)(wlen + done);
}

int dcaux_read_edid(rdna3_dev *d, uint32_t n, uint8_t *edid, uint32_t cap)
{
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    uint8_t off = 0;
    uint32_t blocks, b, i, sum;
    int r;
    if (cap < RD_EDID_BLOCK)
        return RDNA3_EINVAL;
    r = dcaux_i2c_xfer(d, n, 0x50, &off, 1, edid, RD_EDID_BLOCK);
    if (r < 0)
        return r;
    for (i = 0; i < 8; i++)
        if (edid[i] != hdr[i])
            return RDNA3_EINVAL;
    for (i = 0, sum = 0; i < RD_EDID_BLOCK; i++)
        sum += edid[i];
    if (sum & 0xff) {
        RERR(d, "AUX%u: EDID base block checksum wrong", n);
        return RDNA3_EINVAL;
    }
    blocks = 1u + edid[126];
    if (blocks > cap / RD_EDID_BLOCK)
        blocks = cap / RD_EDID_BLOCK;
    if (blocks > RD_EDID_MAX / RD_EDID_BLOCK)
        blocks = RD_EDID_MAX / RD_EDID_BLOCK;
    for (b = 1; b < blocks; b++) {
        /* blocks 2 and 3 live in segment 1 (E-DDC segment pointer at 0x30) */
        uint8_t seg = (uint8_t)(b / 2);
        off = (uint8_t)((b & 1) * RD_EDID_BLOCK);
        if (seg) {
            r = xfer_retry(d, n, RD_AUX_I2C_WRITE_MOT, 0x30, &seg, 1);
            if (r < 0)
                return r;
        }
        r = dcaux_i2c_xfer(d, n, 0x50, &off, 1, edid + b * RD_EDID_BLOCK, RD_EDID_BLOCK);
        if (r < 0)
            return r;
        for (i = 0, sum = 0; i < RD_EDID_BLOCK; i++)
            sum += edid[b * RD_EDID_BLOCK + i];
        if (sum & 0xff) {
            RERR(d, "AUX%u: EDID extension %u checksum wrong, dropped", n, b);
            blocks = b;
            break;
        }
    }
    return (int)(blocks * RD_EDID_BLOCK);
}

int dcaux_probe(rdna3_dev *d, rdna3_connector *out, uint32_t max)
{
    uint32_t n, found = 0;
    if (!d->ipver[HWIP_DCE].present)
        return 0;
    for (n = 0; n < RD_AUX_ENGINES && found < max; n++) {
        rdna3_connector *c = &out[found];
        uint8_t dpcd[16];
        int r;
        unsigned i;
        for (i = 0; i < sizeof(*c); i++)
            ((uint8_t *)c)[i] = 0;
        c->aux = n;
        c->hpd = dcaux_hpd_sense(d, n);
        r = dcaux_dpcd_read(d, n, 0x000, dpcd, 16);
        if (r == 16 && dpcd[0]) {
            c->dp = 1;
            c->dpcd_rev = dpcd[0];
            c->max_link_rate = dpcd[1];
            c->max_lane_count = dpcd[2] & 0x1f;
        }
        if (c->hpd && !c->dp) {
            /* a sink is plugged but DPCD does not answer: what the engine saw
             * (an HDMI sink has no DPCD; a DP one should) */
            uint32_t reply = 0xff;
            int r1 = dcaux_transfer(d, n, RD_AUX_DP_READ, 0x000, dpcd, 1, &reply);
            RERR(d, "AUX%u: HPD high, DPCD read %d (reply 0x%x): CONTROL 0x%08x ARB 0x%08x "
                 "SW_STATUS 0x%08x SW_CONTROL 0x%08x DPHY_TX 0x%08x RX0 0x%08x RX1 0x%08x", n, r1,
                 reply, RA(d, regDP_AUX0_AUX_CONTROL, n), RA(d, regDP_AUX0_AUX_ARB_CONTROL, n),
                 RA(d, regDP_AUX0_AUX_SW_STATUS, n), RA(d, regDP_AUX0_AUX_SW_CONTROL, n),
                 RA(d, regDP_AUX0_AUX_DPHY_TX_CONTROL, n), RA(d, regDP_AUX0_AUX_DPHY_RX_CONTROL0, n),
                 RA(d, regDP_AUX0_AUX_DPHY_RX_CONTROL1, n));
        }
        r = dcaux_read_edid(d, n, c->edid, sizeof(c->edid));
        if (c->hpd && r <= 0)
            RERR(d, "AUX%u: HPD high, EDID over I2C-over-AUX failed (%d)", n, r);
        c->edid_len = r > 0 ? r : 0;
        if (c->hpd || c->dp || c->edid_len) {
            RLOG(d, "AUX%u: HPD %d, %s, EDID %d bytes", n, c->hpd,
                 c->dp ? "DisplayPort sink" : "no DPCD", c->edid_len);
            found++;
        }
    }
    return (int)found;
}
