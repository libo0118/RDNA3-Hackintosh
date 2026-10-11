/*
 * sim_display_hpo.inc - included by sim_navi32.c before sim_wr: the model of
 * the DCN 3.2 HPO DP encoders and the DCCG pieces of a 128b/132b stream
 * (stage 4 item L), and of the ODM combine checks.
 *
 *   - the HPO DP stream encoders 0..3: SYM32 RESET_DONE follows RESET, the
 *     pixel-to-symbol and the clock ramp FIFO reset done follow their
 *     resets, VID_STREAM_STATUS follows VID_STREAM_ENABLE
 *   - the HPO DP link encoders 0..1: DPHY STATUS / RESET_STATUS follow
 *     DPHY_ENABLE / DPHY_RESET; SAT_UPDATE is taken at once (no pending)
 *     and sends the ACT: an SST sink whose VC payload table was just
 *     updated (DPCD 0x2c0 bit 0) sets ACT_HANDLED (bit 1)
 *   - OTGn_PIXEL_RATE_CNTL: DTBCLKDTO0_ENABLE_STATUS follows the enable
 *
 * The 128b/132b link layer of the sink is dp_sink_eval128 in sim_navi32.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_hpo.h"
#include "dcn32_odm.h"
#include "gen/dcn_3_2_0_hpo.h"
#include "gen/dcn_3_2_0_odm.h"

#define HPO_ST (regDP_STREAM_ENC1_DP_STREAM_ENC_CLOCK_CONTROL - regDP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_CONTROL)
#define HPOR(reg, n) (DCN_B2 + (reg) + HPO_ST * (n))
#define PRCR(reg, o) (DCN_B1 + (reg) + (regOTG1_PIXEL_RATE_CNTL - regOTG0_PIXEL_RATE_CNTL) * (o))

static struct {
    unsigned sat_updates, acts;
} SH;

/* a status bit that follows a control bit of the same register */
static uint32_t follow(uint32_t v, uint32_t ctl_mask, uint32_t st_mask)
{
    return (v & ctl_mask) ? (v | st_mask) : (v & ~st_mask);
}

static int hpo_rd(uint32_t dw, uint32_t *out)
{
    uint32_t n;
    for (n = 0; n < RD_HPO_DP_STREAM_ENCS; n++) {
        if (dw == HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_CONTROL, n)) {
            *out = follow(S.reg[dw], DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL__DP_SYM32_ENC_RESET_MASK,
                          DP_SYM32_ENC0_DP_SYM32_ENC_CONTROL__DP_SYM32_ENC_RESET_DONE_MASK);
            return 1;
        }
        if (dw == HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_VID_FIFO_CONTROL, n)) {
            *out = follow(S.reg[dw], DP_SYM32_ENC0_DP_SYM32_ENC_VID_FIFO_CONTROL__PIXEL_TO_SYMBOL_FIFO_RESET_MASK,
                          DP_SYM32_ENC0_DP_SYM32_ENC_VID_FIFO_CONTROL__PIXEL_TO_SYMBOL_FIFO_RESET_DONE_MASK);
            return 1;
        }
        if (dw == HPOR(regDP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_RAMP_ADJUSTER_FIFO_STATUS_CONTROL0, n)) {
            *out = follow(S.reg[dw],
                          DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_RAMP_ADJUSTER_FIFO_STATUS_CONTROL0__FIFO_RESET_MASK,
                          DP_STREAM_ENC0_DP_STREAM_ENC_CLOCK_RAMP_ADJUSTER_FIFO_STATUS_CONTROL0__FIFO_RESET_DONE_MASK);
            return 1;
        }
        if (dw == HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_VID_STREAM_CONTROL, n)) {
            *out = follow(S.reg[dw], DP_SYM32_ENC0_DP_SYM32_ENC_VID_STREAM_CONTROL__VID_STREAM_ENABLE_MASK,
                          DP_SYM32_ENC0_DP_SYM32_ENC_VID_STREAM_CONTROL__VID_STREAM_STATUS_MASK);
            return 1;
        }
    }
    for (n = 0; n < RD_HPO_DP_LINK_ENCS; n++)
        if (dw == HPOR(regDP_DPHY_SYM320_DP_DPHY_SYM32_STATUS, n)) {
            uint32_t c = S.reg[HPOR(regDP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL, n)];
            *out = ((c & DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL__DPHY_ENABLE_MASK)
                        ? DP_DPHY_SYM320_DP_DPHY_SYM32_STATUS__STATUS_MASK : 0) |
                   ((c & DP_DPHY_SYM320_DP_DPHY_SYM32_CONTROL__DPHY_RESET_MASK)
                        ? DP_DPHY_SYM320_DP_DPHY_SYM32_STATUS__RESET_STATUS_MASK : 0);
            return 1;
        }
    for (n = 0; n < RD_DCN_MAX_PIPES; n++)
        if (dw == PRCR(regOTG0_PIXEL_RATE_CNTL, n)) {
            *out = follow(S.reg[dw], OTG0_PIXEL_RATE_CNTL__DTBCLK_DTO0_ENABLE_MASK,
                          OTG0_PIXEL_RATE_CNTL__DTBCLKDTO0_ENABLE_STATUS_MASK);
            return 1;
        }
    return 0;
}

static int hpo_wr(uint32_t dw, uint32_t v)
{
    uint32_t n;
    for (n = 0; n < RD_HPO_DP_LINK_ENCS; n++)
        if (dw == HPOR(regDP_DPHY_SYM320_DP_DPHY_SYM32_SAT_UPDATE, n)) {
            S.reg[dw] = 0;   /* taken: SAT_UPDATE_PENDING never shows */
            if (v & DP_DPHY_SYM320_DP_DPHY_SYM32_SAT_UPDATE__SAT_UPDATE_MASK) {
                SH.sat_updates++;
                /* the ACT on the link: an SST sink acknowledges a table it
                 * just updated */
                if (!S.mst.on && (S.dpcd[0x2c0] & RD_DP_VC_PAYLOAD_TABLE_UPDATED)) {
                    S.dpcd[0x2c0] |= RD_DP_ACT_HANDLED;
                    SH.acts++;
                }
            }
            return 1;
        }
    return 0;
}
