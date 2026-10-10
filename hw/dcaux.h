/*
 * dcaux.h - DisplayPort AUX channel and hot-plug sense on DCN 3.2: native
 * DPCD reads/writes, I2C-over-AUX (EDID at 0x50) and the HPD pins, ported
 * from Linux amdgpu Display Core dce_aux.c (dce110 AUX engine: acquire,
 * submit_channel_request, get_channel_status, read_channel_reply) and the
 * HPD GPIO (DC_HPD_INT_STATUS.DC_HPD_SENSE). MIT.
 *
 * DCN 3.2.0 / 3.2.1 have five AUX engines and five HPD pins (instance n = AUX0 +
 * n * (AUX1 - AUX0)). Which connector they belong to is in the VBIOS object
 * table; until that is parsed the caller probes: HPD sense, then DPCD, then
 * EDID.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCAUX_H
#define RDNA3_DCAUX_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_AUX_ENGINES   5
#define RD_AUX_MAX_BYTES 16          /* one AUX transaction */
#define RD_EDID_BLOCK    128
#define RD_EDID_MAX      (4 * RD_EDID_BLOCK)

/* i2caux_transaction_action */
#define RD_AUX_I2C_WRITE      0x00
#define RD_AUX_I2C_READ       0x10
#define RD_AUX_I2C_WRITE_MOT  0x40
#define RD_AUX_I2C_READ_MOT   0x50
#define RD_AUX_DP_WRITE       0x80
#define RD_AUX_DP_READ        0x90

/* reply result (AUX_SW_DATA byte 0 >> 4): aux_transaction_reply */
#define RD_AUX_REPLY_ACK        0x0
#define RD_AUX_REPLY_NACK       0x1
#define RD_AUX_REPLY_DEFER      0x2
#define RD_AUX_REPLY_I2C_NACK   0x4
#define RD_AUX_REPLY_I2C_DEFER  0x8

/* AUX_RET_* of one transaction */
enum {
    RD_AUX_OK = 0,
    RD_AUX_ERR_TIMEOUT = -1,
    RD_AUX_ERR_INVALID_REPLY = -2,
    RD_AUX_ERR_HPD_DISCON = -3,
    RD_AUX_ERR_ACQUIRE = -4,
    RD_AUX_ERR_NACK = -5,        /* the sink answered NACK / I2C NACK */
    RD_AUX_ERR_DEFER = -6,       /* still DEFER after the retries */
};

/* DC_HPD_INT_STATUS.DC_HPD_SENSE of pin 'n' (enables the pin first) */
int  dcaux_hpd_sense(rdna3_dev *d, uint32_t n);

/* HPD interrupts (irq_service_dcn32.c): long pulses of pin 'n' arrive on
 * IH client DCE, src DCN_1_0__SRCID__DC_HPD1_INT with the pin in
 * src_data[0] (DCN_1_0__CTXID__DC_HPDn_INT = n, DP short pulses n + 6). */
#define RD_IH_CLIENT_DCE      0x04
#define RD_IH_SRC_DC_HPD1_INT 0x2a
#define RD_HPD_PINS           6     /* context ids; DCN 3.2 wires HPD1..5 */

/* dc_interrupt_set(DC_IRQ_SOURCE_HPDn): polarity set to catch the next
 * change of the current sense, then DC_HPD_INT_EN */
void dchpd_irq_set(rdna3_dev *d, uint32_t n, int enable);
/* to_dal_irq_source_dcn32: the pin an IV reports, or -1; *rx = DP short pulse */
int  dchpd_iv_pin(uint32_t client_id, uint32_t src_id, uint32_t ctx, int *rx);
/* hpd_ack: DC_HPD_INT_ACK, then the polarity flipped to the opposite of
 * DC_HPD_SENSE_DELAYED. Returns the (delayed) sense: 1 connected. */
int  dchpd_irq_ack(rdna3_dev *d, uint32_t n);

/* One raw transaction on engine 'n': 'action' RD_AUX_*, 20-bit 'address',
 * 'len' bytes (<= 16; 0 = address-only). Writes send buf, reads fill it.
 * Returns the bytes transferred (reads: received) or RD_AUX_ERR_*; *reply
 * gets the reply code. No retries. */
int  dcaux_transfer(rdna3_dev *d, uint32_t n, uint32_t action, uint32_t address, uint8_t *buf,
                    uint32_t len, uint32_t *reply);

/* dce_aux_transfer_with_retries for native DPCD: any length, split in 16s */
int  dcaux_dpcd_read(rdna3_dev *d, uint32_t n, uint32_t address, uint8_t *buf, uint32_t len);
int  dcaux_dpcd_write(rdna3_dev *d, uint32_t n, uint32_t address, const uint8_t *buf, uint32_t len);

/* I2C-over-AUX: write 'wlen' bytes then read 'rlen' bytes from 7-bit
 * device 'dev7' (MOT keeps the bus between the two) */
int  dcaux_i2c_xfer(rdna3_dev *d, uint32_t n, uint32_t dev7, const uint8_t *wbuf, uint32_t wlen,
                    uint8_t *rbuf, uint32_t rlen);

/* EDID of the sink on engine 'n': base block + extensions (up to 4 blocks),
 * checksummed. Returns the byte count, or RD_AUX_ERR_* / RDNA3_EINVAL for
 * a bad header or checksum. */
int  dcaux_read_edid(rdna3_dev *d, uint32_t n, uint8_t *edid, uint32_t cap);

/* Hardware I2C engine on DDC line n+1 (dce_i2c_hw.c, DCN 3.x): the DDC of
 * HDMI/DVI sinks and of DP++ adaptors (the AUX engine then reports
 * NON_AUX_MODE). One command of up to 4 payloads, each a write or a read
 * on a 7-bit address; all but the last keep the bus (MOT). Returns 0 or
 * RD_AUX_ERR_* (NACK: no device; TIMEOUT; ACQUIRE: owned by HW/DMCU). */
typedef struct rd_i2c_payload {
    uint8_t  addr7;
    int      write;
    uint8_t *data;
    uint32_t len;
} rd_i2c_payload;
int  dcddc_i2c_cmd(rdna3_dev *d, uint32_t n, rd_i2c_payload *p, uint32_t np);

/* EDID over the DDC (E-DDC segment pointer for blocks 2-3), checksummed */
int  dcddc_read_edid(rdna3_dev *d, uint32_t n, uint8_t *edid, uint32_t cap);

/* what one probe of all engines found */
typedef struct rdna3_connector {
    uint32_t aux;               /* engine / HPD pin index */
    int      hpd;               /* DC_HPD_SENSE */
    int      dp;                /* DPCD answered */
    uint8_t  dpcd_rev, max_link_rate, max_lane_count;   /* DPCD 0x000..0x002 */
    int      edid_len;          /* 0 = none */
    int      edid_ddc;          /* read over the DDC (TMDS sink) instead of AUX */
    uint8_t  edid[RD_EDID_MAX];
} rdna3_connector;

/* Probe every engine: HPD, DPCD 0x000..0x00f, EDID. Fills up to
 * RD_AUX_ENGINES entries (HPD or DPCD or EDID seen), returns the count. */
int  dcaux_probe(rdna3_dev *d, rdna3_connector *out, uint32_t max);

#ifdef __cplusplus
}
#endif
#endif
