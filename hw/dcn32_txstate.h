/*
 * dcn32_txstate.h - what a lit head sends after its OTG: the timing
 * generator's totals and DRR bounds, the pixel-rate / PHY PLL selects, the
 * DIG front end (stream encoder: HDMI control, GC / AV mute, infoframe and
 * generic packet slots, audio packets, the DIG FIFO), the DIG back end
 * (link encoder: enable, symbol clock, TMDS control) and the PHY's clock
 * and encoder selects. Reads only.
 *
 * DC reads the same registers for its hardware state log (the DTN log:
 * dcn10_log_hw_state with optc1_read_otg_state, enc1_read_state,
 * dcn10_link_encoder_is_dig_enabled, dcn10_get_dig_frontend). Card run of
 * 2026-10-15: the HDMI monitor of the boot head showed "no signal" while its
 * OTG ran, its OTG CRC moved and its picture was the desktop; nothing in
 * the report said what left the DIG. The core logs this state at boot,
 * after the clone and whenever it changes, and the report's screen looks
 * print it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCN32_TXSTATE_H
#define RDNA3_DCN32_TXSTATE_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_TX_NREGS 31
#define RD_TX_NONE  0xffu           /* a DIG instance not known: its registers are not read */
#define RD_TX_GROUPS 4              /* OTG / clocks, DIG FE, FE packets, DIG BE / PHY */
#define RD_TX_LINE  320             /* one group, formatted */

typedef struct rdna3_tx_state {
    uint32_t valid;
    uint32_t otg, fe, be;           /* fe / be RD_TX_NONE when not known */
    uint32_t read_mask;             /* bit i: v[i] was read */
    uint32_t v[RD_TX_NREGS];
} rdna3_tx_state;

/* what the state says is wrong with the output (dcn32_tx_state_problems) */
#define RD_TX_P_OTG_OFF    (1u << 0)    /* OTG_MASTER_EN 0 */
#define RD_TX_P_DIG_OFF    (1u << 1)    /* DIG_BE_EN_CNTL.DIG_ENABLE 0 */
#define RD_TX_P_BE_SYMCLK  (1u << 2)    /* back end on, DIG_SYMCLK_BE_ON 0 */
#define RD_TX_P_FE_SYMCLK  (1u << 3)    /* back end on, DIG_SYMCLK_FE_ON 0 */
#define RD_TX_P_ROUTE      (1u << 4)    /* FE source is not the OTG, or BE source not the FE */
#define RD_TX_P_FORCED_OFF (1u << 5)    /* FORCE_DIG_DISABLE */
#define RD_TX_P_DIG_FIFO   (1u << 6)    /* DIG_FIFO_ERROR */
#define RD_TX_P_DIO_FIFO   (1u << 7)    /* OTG<n>_DIO_FIFO_ERROR / DIO_ERROR_COUNT */
#define RD_TX_P_AVMUTE     (1u << 8)    /* HDMI_GC_AVMUTE or HDMI_ACTIVE_AVMUTE */
#define RD_TX_P_HDMI_ERROR (1u << 9)    /* HDMI_ERROR_INT */
#define RD_TX_P_DRR        (1u << 10)   /* V_TOTAL_MIN / MAX selected and not the nominal V_TOTAL */
#define RD_TX_P_DRR_ZERO   (1u << 11)   /* V_TOTAL_MIN / MAX selected with a bound of 0 */
#define RD_TX_P_NOT_DIO    (1u << 12)   /* DIO_LINK<x>_CNTL.ENC_TYPE_SEL not the DIG (TMDS needs it) */
#define RD_TX_P_NBITS      13

/* Read the chain of OTG 'otg' -> DIG FE 'fe' -> DIG BE / PHY 'be' (fe / be
 * < 0: not known, skipped). RDNA3_EINVAL for an instance out of range. */
int dcn32_tx_state_read(rdna3_dev *d, uint32_t otg, int fe, int be, rdna3_tx_state *t);

/* RD_TX_P_* of a state read */
uint32_t dcn32_tx_state_problems(const rdna3_tx_state *t);

/* "none" or the names of the bits in 'p', comma separated; the length */
size_t dcn32_tx_problems_str(uint32_t p, char *buf, size_t len);

/* group g (0..RD_TX_GROUPS-1) as "OTG1: OTG_CONTROL 0x... V_TOTAL ..."
 * ("-" for a register not read); the length (0 for g out of range) */
size_t dcn32_tx_state_format(const rdna3_tx_state *t, uint32_t g, char *buf, size_t len);

/* the registers read in both that differ, "NAME 0xold -> 0xnew, ...";
 * returns how many (the text may be cut, the count is not). Different
 * instances count as every register changed. */
uint32_t dcn32_tx_state_diff(const rdna3_tx_state *was, const rdna3_tx_state *now, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
