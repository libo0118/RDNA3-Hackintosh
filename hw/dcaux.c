/*
 * dcaux.c - see dcaux.h. Port of dce_aux.c (acquire_engine, submit_channel_
 * request, get_channel_status, read_channel_reply, dce_aux_transfer_with_
 * retries) and the HPD pin read.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcaux.h"
#include "gen/dcn_3_2_1_aux.h"
#include "gen/dcn_3_2_0_gpio.h"
#include "gen/dcn_3_2_0_i2c.h"
#include "gen/dcn_3_2_0_ddcgpio.h"

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

void dchpd_irq_set(rdna3_dev *d, uint32_t n, int enable)
{
    uint32_t v, sense;
    if (!d->ipver[HWIP_DCE].present || n >= RD_AUX_ENGINES)   /* HPD0..4 on DCN 3.2 */
        return;
    v = RHPD(d, regHPD0_DC_HPD_INT_CONTROL, n);
    if (enable) {
        /* interrupt when the sense becomes the polarity: wait for the
         * opposite of what the pin reads now, as hpd_ack leaves it */
        sense = GET(RHPD(d, regHPD0_DC_HPD_INT_STATUS, n), HPD0_DC_HPD_INT_STATUS, DC_HPD_SENSE_DELAYED);
        v = SET(v, HPD0_DC_HPD_INT_CONTROL, DC_HPD_INT_POLARITY, sense ? 0 : 1);
        WHPD(d, regHPD0_DC_HPD_INT_CONTROL, n, v);
    }
    v = SET(v, HPD0_DC_HPD_INT_CONTROL, DC_HPD_INT_EN, enable ? 1 : 0);
    WHPD(d, regHPD0_DC_HPD_INT_CONTROL, n, v);
}

int dchpd_iv_pin(uint32_t client_id, uint32_t src_id, uint32_t ctx, int *rx)
{
    if (client_id != RD_IH_CLIENT_DCE || src_id != RD_IH_SRC_DC_HPD1_INT || ctx >= 2 * RD_HPD_PINS)
        return -1;
    if (rx)
        *rx = ctx >= RD_HPD_PINS;
    return (int)(ctx % RD_HPD_PINS);
}

int dchpd_irq_ack(rdna3_dev *d, uint32_t n)
{
    uint32_t v, sense;
    if (!d->ipver[HWIP_DCE].present || n >= RD_AUX_ENGINES)   /* HPD0..4 on DCN 3.2 */
        return 0;
    sense = GET(RHPD(d, regHPD0_DC_HPD_INT_STATUS, n), HPD0_DC_HPD_INT_STATUS, DC_HPD_SENSE_DELAYED);
    /* dal_irq_service_ack_generic: the ACK bit of the ack register */
    v = RHPD(d, regHPD0_DC_HPD_INT_CONTROL, n);
    WHPD(d, regHPD0_DC_HPD_INT_CONTROL, n, SET(v, HPD0_DC_HPD_INT_CONTROL, DC_HPD_INT_ACK, 1));
    v = RHPD(d, regHPD0_DC_HPD_INT_CONTROL, n);
    v = SET(v, HPD0_DC_HPD_INT_CONTROL, DC_HPD_INT_ACK, 0);
    WHPD(d, regHPD0_DC_HPD_INT_CONTROL, n, SET(v, HPD0_DC_HPD_INT_CONTROL, DC_HPD_INT_POLARITY, sense ? 0 : 1));
    return sense ? 1 : 0;
}

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

/* DC_GPIO_DDCn_MASK of AUX engine n's DDC line (DDC1..DDC5, stride 4) */
static uint32_t ddc_mask_reg(rdna3_dev *d, uint32_t n)
{
    return rdna3_reg_offset(d, HWIP_DCE, 0, regDC_GPIO_DDC1_MASK_BASE_IDX, regDC_GPIO_DDC1_MASK) +
           (regDC_GPIO_DDC2_MASK - regDC_GPIO_DDC1_MASK) * n;
}

/* hw_ddc.c set_config: GPIO_DDC_CONFIG_TYPE_MODE_AUX sets AUX_PAD1_MODE,
 * MODE_I2C clears it; returns the previous mode. With the pad in I2C mode
 * the engine reports AUX_SW_NON_AUX_MODE and sends nothing. */
static int ddc_pad_aux(rdna3_dev *d, uint32_t n, int aux)
{
    uint32_t reg = ddc_mask_reg(d, n), v = rdna3_rreg(d, reg);
    int was = GET(v, DC_GPIO_DDC1_MASK, AUX_PAD1_MODE) != 0;
    if (was != (aux != 0))
        rdna3_wreg(d, reg, SET(v, DC_GPIO_DDC1_MASK, AUX_PAD1_MODE, aux ? 1 : 0));
    return was;
}

/* acquire_engine: SW asks the arbiter for the AUX registers */
static int aux_acquire(rdna3_dev *d, uint32_t n)
{
    uint32_t v, i;
    /* dce_aux acquire(): dal_ddc_open(..., GPIO_DDC_CONFIG_TYPE_MODE_AUX) first */
    (void)ddc_pad_aux(d, n, 1);
    /* dcn10_aux_initialize (link encoder hw_init): the engine follows its own
     * HPD pin (hpd_source; DDCn and HPDn pair up on this board's VBIOS
     * paths), no LS reads, receive window 0. With HPD_SEL left at 0 every
     * engine watched HPD1 and reported AUX_SW_NON_AUX_MODE on the card. */
    v = RA(d, regDP_AUX0_AUX_CONTROL, n);
    if (GET(v, DP_AUX0_AUX_CONTROL, AUX_HPD_SEL) != n || GET(v, DP_AUX0_AUX_CONTROL, AUX_LS_READ_EN)) {
        v = SET(v, DP_AUX0_AUX_CONTROL, AUX_HPD_SEL, n);
        v = SET(v, DP_AUX0_AUX_CONTROL, AUX_LS_READ_EN, 0);
        WA(d, regDP_AUX0_AUX_CONTROL, n, v);
        v = RA(d, regDP_AUX0_AUX_DPHY_RX_CONTROL0, n);
        WA(d, regDP_AUX0_AUX_DPHY_RX_CONTROL0, n, SET(v, DP_AUX0_AUX_DPHY_RX_CONTROL0, AUX_RX_RECEIVE_WINDOW, 0));
    }
    v = RA(d, regDP_AUX0_AUX_ARB_CONTROL, n);
    if (GET(v, DP_AUX0_AUX_ARB_CONTROL, AUX_REG_RW_CNTL_STATUS) == DMCU_CAN_ACCESS_AUX)   /* is_engine_available */
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

/* dce_aux_configure_timeout's polling_timeout_period per engine (us);
 * 0 = AUX_TIMEOUT_US. Shared by all devices: one card per system here. */
static uint32_t aux_poll_us[RD_AUX_ENGINES];

#define TIME_OUT_INCREMENT        1016
#define TIME_OUT_MULTIPLIER_8     8
#define TIME_OUT_MULTIPLIER_16    16
#define TIME_OUT_MULTIPLIER_32    32
#define TIME_OUT_MULTIPLIER_64    64
#define MAX_TIMEOUT_LENGTH        127
#define DEFAULT_AUX_ENGINE_MULT   0
#define DEFAULT_AUX_ENGINE_LENGTH 69
#define SW_AUX_TIMEOUT_PERIOD_MULTIPLIER 6

uint32_t dcaux_configure_timeout(rdna3_dev *d, uint32_t n, uint32_t timeout_in_us)
{
    uint32_t multiplier = 0, length = 0, prev_length, prev_mult, prev_timeout_val, v;

    if (n >= RD_AUX_ENGINES)
        return 0;
    /* 1-Update polling timeout period */
    aux_poll_us[n] = timeout_in_us * SW_AUX_TIMEOUT_PERIOD_MULTIPLIER;
    /* 2-Update aux timeout period length and multiplier */
    if (timeout_in_us == 0) {
        multiplier = DEFAULT_AUX_ENGINE_MULT;
        length = DEFAULT_AUX_ENGINE_LENGTH;
    } else if (timeout_in_us <= TIME_OUT_INCREMENT) {
        multiplier = 0;
        length = timeout_in_us / TIME_OUT_MULTIPLIER_8;
        if (timeout_in_us % TIME_OUT_MULTIPLIER_8 != 0)
            length++;
    } else if (timeout_in_us <= 2 * TIME_OUT_INCREMENT) {
        multiplier = 1;
        length = timeout_in_us / TIME_OUT_MULTIPLIER_16;
        if (timeout_in_us % TIME_OUT_MULTIPLIER_16 != 0)
            length++;
    } else if (timeout_in_us <= 4 * TIME_OUT_INCREMENT) {
        multiplier = 2;
        length = timeout_in_us / TIME_OUT_MULTIPLIER_32;
        if (timeout_in_us % TIME_OUT_MULTIPLIER_32 != 0)
            length++;
    } else {
        multiplier = 3;
        length = timeout_in_us / TIME_OUT_MULTIPLIER_64;
        if (timeout_in_us % TIME_OUT_MULTIPLIER_64 != 0)
            length++;
    }
    length = length < MAX_TIMEOUT_LENGTH ? length : MAX_TIMEOUT_LENGTH;
    v = RA(d, regDP_AUX0_AUX_DPHY_RX_CONTROL1, n);
    prev_length = GET(v, DP_AUX0_AUX_DPHY_RX_CONTROL1, AUX_RX_TIMEOUT_LEN);
    prev_mult = GET(v, DP_AUX0_AUX_DPHY_RX_CONTROL1, AUX_RX_TIMEOUT_LEN_MUL);
    switch (prev_mult) {
    case 0: prev_timeout_val = prev_length * TIME_OUT_MULTIPLIER_8; break;
    case 1: prev_timeout_val = prev_length * TIME_OUT_MULTIPLIER_16; break;
    case 2: prev_timeout_val = prev_length * TIME_OUT_MULTIPLIER_32; break;
    case 3: prev_timeout_val = prev_length * TIME_OUT_MULTIPLIER_64; break;
    default: prev_timeout_val = DEFAULT_AUX_ENGINE_LENGTH * TIME_OUT_MULTIPLIER_8; break;
    }
    v = SET(v, DP_AUX0_AUX_DPHY_RX_CONTROL1, AUX_RX_TIMEOUT_LEN, length);
    v = SET(v, DP_AUX0_AUX_DPHY_RX_CONTROL1, AUX_RX_TIMEOUT_LEN_MUL, multiplier);
    WA(d, regDP_AUX0_AUX_DPHY_RX_CONTROL1, n, v);
    return prev_timeout_val;
}

static int wait_sw_done(rdna3_dev *d, uint32_t n, int want)
{
    uint32_t i, poll = n < RD_AUX_ENGINES && aux_poll_us[n] ? aux_poll_us[n] : AUX_TIMEOUT_US;
    for (i = 0; i < poll / 10; i++) {

        uint32_t v = RA(d, regDP_AUX0_AUX_SW_STATUS, n);
        if ((GET(v, DP_AUX0_AUX_SW_STATUS, AUX_SW_DONE) != 0) == (want != 0))
            return RD_AUX_OK;
        d->os.udelay(d->os.ctx, 10);
    }
    return RD_AUX_ERR_TIMEOUT;
}

/* dce_aux_transfer_raw: acquire, submit_channel_request, get_channel_status,
 * read_channel_reply, release_engine */
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

/* the transaction actions are i2caux_action_from_payload's: I2C write / read,
 * with MOT on every chunk but the last */
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

/* ---------------- hardware I2C engine (dce_i2c_hw.c, dcn2 construct) ---------------- */

#define RI(d, reg) rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg)
#define I2C_SETUP(d, n)  (RI(d, regDC_I2C_DDC1_SETUP) + (regDC_I2C_DDC2_SETUP - regDC_I2C_DDC1_SETUP) * (n))
#define I2C_SPEED(d, n)  (RI(d, regDC_I2C_DDC1_SPEED) + (regDC_I2C_DDC2_SPEED - regDC_I2C_DDC1_SPEED) * (n))
#define I2C_HWST(d, n)   (RI(d, regDC_I2C_DDC1_HW_STATUS) + (regDC_I2C_DDC2_HW_STATUS - regDC_I2C_DDC1_HW_STATUS) * (n))
#define I2C_STATUS_IDLE       0
#define I2C_STATUS_USED_BY_SW 1
#define I2C_STATUS_USED_BY_HW 2
#define I2C_RW_CNTL_DMCU_ONLY 2
#define I2C_SETUP_TIME_LIMIT_DCN 3
#define I2C_SEND_RESET_LENGTH_9  9
#define I2C_HW_BUFFER_SIZE       538     /* I2C_HW_BUFFER_SIZE_DCE100 */
#define I2C_SPEED_KHZ            100     /* dc->caps.i2c_speed_in_khz (dcn32) */
#define I2C_TIMEOUT_CLOCKS       32      /* TRANSACTION_TIMEOUT_IN_I2C_CLOCKS */

typedef struct {
    uint32_t n, transaction_count, buffer_used_bytes, buffer_used_write;
} i2c_hw;

static uint32_t rmw(rdna3_dev *d, uint32_t reg, uint32_t mask, uint32_t val)
{
    uint32_t v = (rdna3_rreg(d, reg) & ~mask) | (val & mask);
    rdna3_wreg(d, reg, v);
    return v;
}
#define FV(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)
#define FM(reg, field)    ((uint32_t)reg##__##field##_MASK)

static int i2c_engine_available(rdna3_dev *d, uint32_t n)
{
    if (GET(rdna3_rreg(d, I2C_HWST(d, n)), DC_I2C_DDC1_HW_STATUS, DC_I2C_DDC1_HW_STATUS) ==
        I2C_STATUS_USED_BY_HW)
        return 0;
    return GET(rdna3_rreg(d, RI(d, regDC_I2C_ARBITRATION)), DC_I2C_ARBITRATION,
               DC_I2C_REG_RW_CNTL_STATUS) != I2C_RW_CNTL_DMCU_ONLY;
}

static int i2c_hw_busy(rdna3_dev *d, uint32_t n)
{
    if (GET(rdna3_rreg(d, RI(d, regDC_I2C_SW_STATUS)), DC_I2C_SW_STATUS, DC_I2C_SW_STATUS) ==
        I2C_STATUS_IDLE)
        return 0;
    return !i2c_engine_available(d, n);
}

/* set_speed: prescale from the microsecond time base (or the 100 MHz crystal) */
static void i2c_set_speed(rdna3_dev *d, uint32_t n, uint32_t khz)
{
    uint32_t tb = rdna3_rreg(d, RI(d, regMICROSECOND_TIME_BASE_DIV));
    uint32_t xtal_div = GET(tb, MICROSECOND_TIME_BASE_DIV, XTAL_REF_DIV);
    uint32_t base_div = GET(tb, MICROSECOND_TIME_BASE_DIV, MICROSECOND_TIME_BASE_DIV);
    uint32_t ref = base_div ? base_div * 1000 : 100000, prescale;
    if (!xtal_div)
        xtal_div = 2;
    prescale = (ref / xtal_div) / khz;
    rmw(d, I2C_SPEED(d, n),
        FM(DC_I2C_DDC1_SPEED, DC_I2C_DDC1_PRESCALE) | FM(DC_I2C_DDC1_SPEED, DC_I2C_DDC1_THRESHOLD) |
            FM(DC_I2C_DDC1_SPEED, DC_I2C_DDC1_START_STOP_TIMING_CNTL),
        FV(DC_I2C_DDC1_SPEED, DC_I2C_DDC1_PRESCALE, prescale) |
            FV(DC_I2C_DDC1_SPEED, DC_I2C_DDC1_THRESHOLD, 2) |
            FV(DC_I2C_DDC1_SPEED, DC_I2C_DDC1_START_STOP_TIMING_CNTL, khz > 50 ? 2 : 1));
}

/* hw_ddc.c set_config GPIO_DDC_CONFIG_TYPE_MODE_I2C: pull-downs on, pad in I2C mode */
static void ddc_pad_i2c(rdna3_dev *d, uint32_t n)
{
    uint32_t reg = ddc_mask_reg(d, n), v = rdna3_rreg(d, reg);
    if (!GET(v, DC_GPIO_DDC1_MASK, DC_GPIO_DDC1DATA_PD_EN) || !GET(v, DC_GPIO_DDC1_MASK, DC_GPIO_DDC1CLK_PD_EN)) {
        v = SET(v, DC_GPIO_DDC1_MASK, DC_GPIO_DDC1DATA_PD_EN, 1);
        v = SET(v, DC_GPIO_DDC1_MASK, DC_GPIO_DDC1CLK_PD_EN, 1);
        rdna3_wreg(d, reg, v);
    }
    if (GET(v, DC_GPIO_DDC1_MASK, AUX_PAD1_MODE)) {
        d->os.udelay(d->os.ctx, 2000);   /* let the pins de-assert first */
        (void)ddc_pad_aux(d, n, 0);
    }
}

/* acquire_i2c_hw_engine + setup_engine */
static int i2c_acquire(rdna3_dev *d, i2c_hw *e)
{
    uint32_t n = e->n;
    if (!i2c_engine_available(d, n))
        return RD_AUX_ERR_ACQUIRE;
    ddc_pad_i2c(d, n);
    rmw(d, RI(d, regDC_I2C_ARBITRATION), FM(DC_I2C_ARBITRATION, DC_I2C_SW_USE_I2C_REG_REQ),
        FV(DC_I2C_ARBITRATION, DC_I2C_SW_USE_I2C_REG_REQ, 1));
    i2c_set_speed(d, n, I2C_SPEED_KHZ);
    rmw(d, RI(d, regDC_I2C_CONTROL),
        FM(DC_I2C_CONTROL, DC_I2C_GO) | FM(DC_I2C_CONTROL, DC_I2C_SOFT_RESET) |
            FM(DC_I2C_CONTROL, DC_I2C_SEND_RESET) | FM(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET) |
            FM(DC_I2C_CONTROL, DC_I2C_TRANSACTION_COUNT) | FM(DC_I2C_CONTROL, DC_I2C_DDC_SELECT),
        FV(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET, 1) | FV(DC_I2C_CONTROL, DC_I2C_DDC_SELECT, n));
    rmw(d, I2C_SETUP(d, n),
        FM(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_TIME_LIMIT) | FM(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_SEND_RESET_LENGTH) |
            FM(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_ENABLE),
        FV(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_TIME_LIMIT, I2C_SETUP_TIME_LIMIT_DCN) |
            FV(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_SEND_RESET_LENGTH, I2C_SEND_RESET_LENGTH_9) |
            FV(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_ENABLE, 1));
    rmw(d, RI(d, regDC_I2C_ARBITRATION), FM(DC_I2C_ARBITRATION, DC_I2C_NO_QUEUED_SW_GO), 0);
    e->transaction_count = e->buffer_used_bytes = e->buffer_used_write = 0;
    return 0;
}

/* release_engine (engine_keep_power_up_count 1: the line stays enabled) */
static void i2c_release(rdna3_dev *d, i2c_hw *e)
{
    uint32_t st = GET(rdna3_rreg(d, RI(d, regDC_I2C_SW_STATUS)), DC_I2C_SW_STATUS, DC_I2C_SW_STATUS);
    if (st == I2C_STATUS_USED_BY_SW)
        rmw(d, RI(d, regDC_I2C_CONTROL), FM(DC_I2C_CONTROL, DC_I2C_SOFT_RESET) | FM(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET),
            FV(DC_I2C_CONTROL, DC_I2C_SOFT_RESET, 1) | FV(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET, 1));
    else
        rmw(d, RI(d, regDC_I2C_CONTROL), FM(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET),
            FV(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET, 1));
    i2c_set_speed(d, e->n, I2C_SPEED_KHZ);   /* i2c_speed_in_khz_hdcp */
    rmw(d, RI(d, regDC_I2C_ARBITRATION),
        FM(DC_I2C_ARBITRATION, DC_I2C_SW_DONE_USING_I2C_REG) | FM(DC_I2C_ARBITRATION, DC_I2C_SW_USE_I2C_REG_REQ),
        FV(DC_I2C_ARBITRATION, DC_I2C_SW_DONE_USING_I2C_REG, 1));
}

/* process_transaction: queue one payload; 1 when it closes the batch */
static int i2c_queue(rdna3_dev *d, i2c_hw *e, const rd_i2c_payload *p, int mot)
{
    int rd = !p->write;
    int last = e->transaction_count == 3 || (p->write && !mot) || rd;
    uint32_t reg = RI(d, regDC_I2C_TRANSACTION0) + e->transaction_count, data, i;
    uint8_t addr = (uint8_t)((p->addr7 << 1) | (rd ? 1 : 0));
    rmw(d, reg,
        FM(DC_I2C_TRANSACTION0, DC_I2C_STOP_ON_NACK0) | FM(DC_I2C_TRANSACTION0, DC_I2C_START0) |
            FM(DC_I2C_TRANSACTION0, DC_I2C_RW0) | FM(DC_I2C_TRANSACTION0, DC_I2C_COUNT0) |
            FM(DC_I2C_TRANSACTION0, DC_I2C_STOP0),
        FV(DC_I2C_TRANSACTION0, DC_I2C_STOP_ON_NACK0, 1) | FV(DC_I2C_TRANSACTION0, DC_I2C_START0, 1) |
            FV(DC_I2C_TRANSACTION0, DC_I2C_RW0, rd) | FV(DC_I2C_TRANSACTION0, DC_I2C_COUNT0, p->len) |
            FV(DC_I2C_TRANSACTION0, DC_I2C_STOP0, last));
    if (e->transaction_count == 0) {
        data = FV(DC_I2C_DATA, DC_I2C_DATA, addr) | FV(DC_I2C_DATA, DC_I2C_INDEX_WRITE, 1);
        e->buffer_used_write = 0;
    } else {
        data = FV(DC_I2C_DATA, DC_I2C_DATA, addr);
    }
    rdna3_wreg(d, RI(d, regDC_I2C_DATA), data);
    e->buffer_used_write++;
    if (p->write)
        for (i = 0; i < p->len; i++) {
            data = SET(data, DC_I2C_DATA, DC_I2C_INDEX_WRITE, 0);
            data = SET(data, DC_I2C_DATA, DC_I2C_DATA, p->data[i]);
            rdna3_wreg(d, RI(d, regDC_I2C_DATA), data);
            e->buffer_used_write++;
        }
    e->transaction_count++;
    e->buffer_used_bytes += (p->write ? 0 : p->len) + 1;
    return last;
}

/* reset_hw_engine + execute_transaction */
static void i2c_go(rdna3_dev *d, i2c_hw *e)
{
    rmw(d, RI(d, regDC_I2C_CONTROL), FM(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET),
        FV(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET, 1));
    rmw(d, I2C_SETUP(d, e->n),
        FM(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_DATA_DRIVE_EN) | FM(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_CLK_DRIVE_EN) |
            FM(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_DATA_DRIVE_SEL) |
            FM(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_INTRA_TRANSACTION_DELAY) |
            FM(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_INTRA_BYTE_DELAY), 0);
    rmw(d, RI(d, regDC_I2C_CONTROL),
        FM(DC_I2C_CONTROL, DC_I2C_SOFT_RESET) | FM(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET) |
            FM(DC_I2C_CONTROL, DC_I2C_SEND_RESET) | FM(DC_I2C_CONTROL, DC_I2C_GO) |
            FM(DC_I2C_CONTROL, DC_I2C_TRANSACTION_COUNT),
        FV(DC_I2C_CONTROL, DC_I2C_TRANSACTION_COUNT, e->transaction_count - 1));
    rmw(d, RI(d, regDC_I2C_CONTROL), FM(DC_I2C_CONTROL, DC_I2C_GO), FV(DC_I2C_CONTROL, DC_I2C_GO, 1));
    e->transaction_count = 0;
    e->buffer_used_bytes = 0;
}

/* get_channel_status polled while USED_BY_SW */
static int i2c_wait(rdna3_dev *d, uint32_t timeout_us)
{
    uint32_t i, v = 0;
    for (i = 0; i <= timeout_us; i++) {
        v = rdna3_rreg(d, RI(d, regDC_I2C_SW_STATUS));
        if (GET(v, DC_I2C_SW_STATUS, DC_I2C_SW_STATUS) != I2C_STATUS_USED_BY_SW)
            break;
        d->os.udelay(d->os.ctx, 1);
    }
    if (GET(v, DC_I2C_SW_STATUS, DC_I2C_SW_STATUS) == I2C_STATUS_USED_BY_SW)
        return RD_AUX_ERR_TIMEOUT;
    if (v & DC_I2C_SW_STATUS__DC_I2C_SW_STOPPED_ON_NACK_MASK)
        return RD_AUX_ERR_NACK;
    if (v & DC_I2C_SW_STATUS__DC_I2C_SW_TIMEOUT_MASK)
        return RD_AUX_ERR_TIMEOUT;
    if (v & DC_I2C_SW_STATUS__DC_I2C_SW_ABORTED_MASK)
        return RD_AUX_ERR_INVALID_REPLY;
    return 0;
}

/* dce_i2c_submit_command_hw */
int dcddc_i2c_cmd(rdna3_dev *d, uint32_t n, rd_i2c_payload *p, uint32_t np)
{
    i2c_hw e = { n, 0, 0, 0 };
    uint32_t k, i;
    int r = 0;
    if (!d->ipver[HWIP_DCE].present || n >= RD_AUX_ENGINES || !np || np > 4)
        return RDNA3_EINVAL;
    r = i2c_acquire(d, &e);
    if (r)
        return r;
    for (k = 0; k < np && !r; k++) {
        int mot = k != np - 1;
        uint32_t timeout;
        if (p[k].len >= I2C_HW_BUFFER_SIZE - e.buffer_used_bytes) {
            r = RDNA3_EINVAL;
            break;
        }
        /* get_transaction_timeout_hw, before the request is queued */
        timeout = (1000 * I2C_TIMEOUT_CLOCKS / I2C_SPEED_KHZ) *
                  (1 + ((p[k].len + 1) << 3) + 1 + (e.buffer_used_bytes << 3) + (e.transaction_count << 1));
        if (i2c_hw_busy(d, n)) {
            r = RD_AUX_ERR_ACQUIRE;
            break;
        }
        if (!i2c_queue(d, &e, &p[k], mot))
            continue;
        if (i2c_hw_busy(d, n)) {
            r = RD_AUX_ERR_ACQUIRE;
            break;
        }
        i2c_go(d, &e);
        r = i2c_wait(d, timeout);
        if (!r && !p[k].write) {
            /* process_channel_reply: the read data follows what was written */
            rdna3_wreg(d, RI(d, regDC_I2C_DATA),
                       FV(DC_I2C_DATA, DC_I2C_INDEX, e.buffer_used_write) |
                           FV(DC_I2C_DATA, DC_I2C_DATA_RW, 1) | FV(DC_I2C_DATA, DC_I2C_INDEX_WRITE, 1));
            for (i = 0; i < p[k].len; i++)
                p[k].data[i] = (uint8_t)GET(rdna3_rreg(d, RI(d, regDC_I2C_DATA)), DC_I2C_DATA, DC_I2C_DATA);
        }
    }
    i2c_release(d, &e);
    return r;
}

int dcddc_read_edid(rdna3_dev *d, uint32_t n, uint8_t *edid, uint32_t cap)
{
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    uint32_t blocks = 1, b, i, sum;
    if (cap < RD_EDID_BLOCK)
        return RDNA3_EINVAL;
    for (b = 0; b < blocks; b++) {
        uint8_t seg = (uint8_t)(b / 2), off = (uint8_t)((b & 1) * RD_EDID_BLOCK);
        rd_i2c_payload p[3];
        uint32_t np = 0;
        int r;
        if (seg)
            p[np++] = (rd_i2c_payload){ 0x30, 1, &seg, 1 };
        p[np++] = (rd_i2c_payload){ 0x50, 1, &off, 1 };
        p[np++] = (rd_i2c_payload){ 0x50, 0, edid + b * RD_EDID_BLOCK, RD_EDID_BLOCK };
        r = dcddc_i2c_cmd(d, n, p, np);
        if (r)
            return b ? (int)(b * RD_EDID_BLOCK) : r;
        for (i = 0, sum = 0; i < RD_EDID_BLOCK; i++)
            sum += edid[b * RD_EDID_BLOCK + i];
        if (!b) {
            for (i = 0; i < 8; i++)
                if (edid[i] != hdr[i])
                    return RDNA3_EINVAL;
            if (sum & 0xff) {
                RERR(d, "DDC%u: EDID base block checksum wrong", n + 1);
                return RDNA3_EINVAL;
            }
            blocks = 1u + edid[126];
            if (blocks > cap / RD_EDID_BLOCK)
                blocks = cap / RD_EDID_BLOCK;
            if (blocks > RD_EDID_MAX / RD_EDID_BLOCK)
                blocks = RD_EDID_MAX / RD_EDID_BLOCK;
        } else if (sum & 0xff) {
            RERR(d, "DDC%u: EDID extension %u checksum wrong, dropped", n + 1, b);
            return (int)(b * RD_EDID_BLOCK);
        }
    }
    return (int)(blocks * RD_EDID_BLOCK);
}

int dcaux_probe_one(rdna3_dev *d, uint32_t n, rdna3_connector *c)
{
    uint8_t dpcd[16];
    uint32_t aux_status;
    int r, pad_was_aux;
    unsigned i;
    if (!d->ipver[HWIP_DCE].present || n >= RD_AUX_ENGINES)
        return 0;
    pad_was_aux = ddc_pad_aux(d, n, 1);
    for (i = 0; i < sizeof(*c); i++)
        ((uint8_t *)c)[i] = 0;
    c->aux = n;
    c->hpd = dcaux_hpd_sense(d, n);
    r = dcaux_dpcd_read(d, n, 0x000, dpcd, 16);
    aux_status = RA(d, regDP_AUX0_AUX_SW_STATUS, n);
    if (r == 16 && dpcd[0]) {
        c->dp = 1;
        c->dpcd_rev = dpcd[0];
        c->max_link_rate = dpcd[1];
        c->max_lane_count = dpcd[2] & 0x1f;
        r = dcaux_read_edid(d, n, c->edid, sizeof(c->edid));
        c->edid_len = r > 0 ? r : 0;
        if (c->hpd && r <= 0)
            RERR(d, "AUX%u: DisplayPort sink, EDID over I2C-over-AUX failed (%d)", n, r);
    } else {
        /* no DPCD: an HDMI/DVI sink, or a DP++ adaptor (NON_AUX_MODE);
         * its EDID is on the DDC, through the I2C engine */
        (void)pad_was_aux;
        if (c->hpd) {
            r = dcddc_read_edid(d, n, c->edid, sizeof(c->edid));
            c->edid_len = r > 0 ? r : 0;
            c->edid_ddc = r > 0;
            if (r <= 0)
                RERR(d, "AUX%u/DDC%u: HPD high, no DPCD (AUX_SW_STATUS 0x%08x) and no EDID on "
                     "the DDC (%d), DDC%u_MASK 0x%08x, I2C SW_STATUS 0x%08x", n, n + 1,
                     aux_status, r, n + 1, rdna3_rreg(d, ddc_mask_reg(d, n)),
                     rdna3_rreg(d, RI(d, regDC_I2C_SW_STATUS)));
        } else {
            (void)ddc_pad_aux(d, n, 0);   /* nothing there: back to I2C mode */
        }
    }
    if (c->hpd || c->dp || c->edid_len) {
        RLOG(d, "AUX%u: HPD %d, %s, EDID %d bytes%s", n, c->hpd,
             c->dp ? "DisplayPort sink" : "no DPCD", c->edid_len,
             c->edid_ddc ? " (DDC, TMDS sink)" : "");
        return 1;
    }
    return 0;
}

int dcaux_probe(rdna3_dev *d, rdna3_connector *out, uint32_t max)
{
    uint32_t n, found = 0;
    if (!d->ipver[HWIP_DCE].present)
        return 0;
    for (n = 0; n < RD_AUX_ENGINES && found < max; n++)
        if (dcaux_probe_one(d, n, &out[found]))
            found++;
    return (int)found;
}

/* ---- dp_is_sink_present (link_dp_capability.c) ---- */

/* the MASK / A / EN fields of one pin of DDC line n+1 (hw_gpio.c
 * store_registers / restore_registers) */
typedef struct ddc_gpio_store {
    uint32_t mask, a, en;
} ddc_gpio_store;

#define DDC_GPIO_A(d, n)  (ddc_mask_reg(d, n) + (regDC_GPIO_DDC1_A - regDC_GPIO_DDC1_MASK))
#define DDC_GPIO_EN(d, n) (ddc_mask_reg(d, n) + (regDC_GPIO_DDC1_EN - regDC_GPIO_DDC1_MASK))
#define DDC_GPIO_Y(d, n)  (ddc_mask_reg(d, n) + (regDC_GPIO_DDC1_Y - regDC_GPIO_DDC1_MASK))

static void ddc_gpio_field(rdna3_dev *d, uint32_t reg, uint32_t m, uint32_t x)
{
    uint32_t v = rdna3_rreg(d, reg);
    rdna3_wreg(d, reg, (v & ~m) | (x ? m : 0));
}

/* dal_hw_gpio_open: store_registers, then dal_hw_gpio_config_mode
 * (GPIO_MODE_INPUT: output enable off, the pin masked from the HW) */
static void dal_hw_gpio_open(rdna3_dev *d, uint32_t n, uint32_t mask_m, uint32_t a_m, uint32_t en_m,
                             ddc_gpio_store *s)
{
    s->mask = rdna3_rreg(d, ddc_mask_reg(d, n)) & mask_m;
    s->a = rdna3_rreg(d, DDC_GPIO_A(d, n)) & a_m;
    s->en = rdna3_rreg(d, DDC_GPIO_EN(d, n)) & en_m;
    ddc_gpio_field(d, DDC_GPIO_EN(d, n), en_m, 0);
    ddc_gpio_field(d, ddc_mask_reg(d, n), mask_m, 1);
}

/* dal_hw_gpio_close: restore_registers */
static void dal_hw_gpio_close(rdna3_dev *d, uint32_t n, uint32_t mask_m, uint32_t a_m, uint32_t en_m,
                              const ddc_gpio_store *s)
{
    ddc_gpio_field(d, ddc_mask_reg(d, n), mask_m, s->mask);
    ddc_gpio_field(d, DDC_GPIO_A(d, n), a_m, s->a);
    ddc_gpio_field(d, DDC_GPIO_EN(d, n), en_m, s->en);
}

#define DDC_CLK_PIN                                                                                  \
    (uint32_t)DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_MASK_MASK, (uint32_t)DC_GPIO_DDC1_A__DC_GPIO_DDC1CLK_A_MASK, \
        (uint32_t)DC_GPIO_DDC1_EN__DC_GPIO_DDC1CLK_EN_MASK
#define DDC_DATA_PIN                                                                                   \
    (uint32_t)DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_MASK_MASK, (uint32_t)DC_GPIO_DDC1_A__DC_GPIO_DDC1DATA_A_MASK, \
        (uint32_t)DC_GPIO_DDC1_EN__DC_GPIO_DDC1DATA_EN_MASK

int dcaux_dp_is_sink_present(rdna3_dev *d, uint32_t n, int dp_connector, uint32_t *clock_pin_out)
{
    ddc_gpio_store sd, sc;
    uint32_t v, clock_pin = 0, retry = 0, i2cmode_m, rxsel_m, ctrl5, phy_aux;
    int aux_pad_mode;
    if (!d->ipver[HWIP_DCE].present || n >= RD_AUX_ENGINES)
        return dp_connector;   /* no DDC: what the connector id says */
    /* dal_ddc_open(ddc, GPIO_MODE_INPUT, GPIO_DDC_CONFIG_TYPE_MODE_I2C):
     * dal_gpio_open_ex on the data pin, then on the clock pin */
    dal_hw_gpio_open(d, n, DDC_DATA_PIN, &sd);
    dal_hw_gpio_open(d, n, DDC_CLK_PIN, &sc);
    /* hw_ddc.c set_config, GPIO_DDC_CONFIG_TYPE_MODE_I2C with
     * GPIO_CONFIG_TYPE_I2C_AUX_DUAL_MODE (GPIO_MODE_INPUT): the internal
     * pull-downs on, 3 ms to discharge the plug-in transient */
    v = rdna3_rreg(d, ddc_mask_reg(d, n));
    aux_pad_mode = GET(v, DC_GPIO_DDC1_MASK, AUX_PAD1_MODE) != 0;
    if (!GET(v, DC_GPIO_DDC1_MASK, DC_GPIO_DDC1DATA_PD_EN) || !GET(v, DC_GPIO_DDC1_MASK, DC_GPIO_DDC1CLK_PD_EN)) {
        v = SET(v, DC_GPIO_DDC1_MASK, DC_GPIO_DDC1DATA_PD_EN, 1);
        v = SET(v, DC_GPIO_DDC1_MASK, DC_GPIO_DDC1CLK_PD_EN, 1);
        rdna3_wreg(d, ddc_mask_reg(d, n), v);
        d->os.msleep(d->os.ctx, 3);
    }
    if (aux_pad_mode) {
        /* the pins de-assert before the pad goes to I2C mode
         * (data_en_bit_present / clock_en_bit_present: the stored EN) */
        if (sd.en || sc.en)
            d->os.msleep(d->os.ctx, 2);
        (void)ddc_pad_aux(d, n, 0);
    }
    /* DDC_REG_LIST_DCN2: DC_GPIO_AUX_CTRL_5.DDC_PADn_I2CMODE = 1 and
     * PHY_AUX_CNTL.AUXn_PAD_RXSEL = 1 (DC_IO_aux_rxsel = 2'b01) */
    i2cmode_m = (uint32_t)DC_GPIO_AUX_CTRL_5__DDC_PAD1_I2CMODE_MASK << n;
    rxsel_m = (uint32_t)PHY_AUX_CNTL__AUX1_PAD_RXSEL_MASK << (2 * n);
    ctrl5 = rdna3_rreg(d, RI(d, regDC_GPIO_AUX_CTRL_5));
    phy_aux = rdna3_rreg(d, RI(d, regPHY_AUX_CNTL));
    rdna3_wreg(d, RI(d, regDC_GPIO_AUX_CTRL_5), ctrl5 | i2cmode_m);
    rdna3_wreg(d, RI(d, regPHY_AUX_CNTL),
               (phy_aux & ~rxsel_m) | ((1u << PHY_AUX_CNTL__AUX1_PAD_RXSEL__SHIFT) << (2 * n)));
    /* a DP sink (or a passive DP-HDMI dongle) holds the clock pin at zero;
     * a short pulse on the pin after a plug / unplug: 3 retries 1 ms apart */
    do {
        clock_pin = GET(rdna3_rreg(d, DDC_GPIO_Y(d, n)), DC_GPIO_DDC1_Y, DC_GPIO_DDC1CLK_Y);
        if (clock_pin)
            d->os.msleep(d->os.ctx, 1);
        else
            break;
    } while (retry++ < 3);
    /* dal_ddc_close: the clock pin, then the data pin */
    dal_hw_gpio_close(d, n, DDC_CLK_PIN, &sc);
    dal_hw_gpio_close(d, n, DDC_DATA_PIN, &sd);
    /* DC leaves the pad in I2C mode until the next dal_ddc_open; this
     * port's AUX acquire only sets AUX_PAD1_MODE, so the pad modes it
     * relies on are put back as they were */
    rdna3_wreg(d, RI(d, regDC_GPIO_AUX_CTRL_5), ctrl5);
    rdna3_wreg(d, RI(d, regPHY_AUX_CNTL), phy_aux);
    if (aux_pad_mode)
        (void)ddc_pad_aux(d, n, 1);
    if (clock_pin_out)
        *clock_pin_out = clock_pin;
    return !clock_pin;
}
