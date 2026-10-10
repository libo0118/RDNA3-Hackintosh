/*
 * dchpd.h - hotplug of the DCN 3.2 connectors after boot: the HPD pins'
 * hardware toggle filter, a debounced view of the filtered sense, the sink
 * read again on a change, and what the display side does about it. Ported
 * from Linux amdgpu Display Core:
 *
 *   link_hpd.c   program_hpd_filter (TMDS 500/100 ms, DP 80/0 ms),
 *                link_enable_hpd_filter, query_hpd_status / link_get_hpd_state
 *   hw_hpd.c     set_config (DC_HPD_TOGGLE_FILT_CNTL in units of 10 ms),
 *                get_value in GPIO_MODE_INTERRUPT (DC_HPD_SENSE_DELAYED)
 *   link_enable_hpd -> dcn10_link_encoder.c dcn10_link_encoder_enable_hpd
 *                (DC_HPD_EN)
 *   amdgpu_dm.c  handle_hpd_irq_helper -> dc_link_detect (link_detection.c
 *                detect_link_and_local_sink: an HDMI/DVI sink without an
 *                EDID is no sink), dm_helpers_read_local_edid's 3 tries
 *
 * amdgpu learns of a change from the HPD interrupt, which fires after the
 * toggle filter (DC_HPD_INT_STATUS / dchpd_irq_ack in dcaux.c). Here a poll
 * every RD_HPD_POLL_MS reads DC_HPD_SENSE_DELAYED of each pin (the IH
 * interrupt, when MSI is live, only makes the next poll come at once), and
 * a new level counts after RD_HPD_DEBOUNCE equal polls in a row: a cable
 * that bounces longer than the hardware filter is not a hotplug.
 *
 * DC_HPD_FAST_TRAIN_CNTL (AUX TX / fast training delays after a connect) is
 * left as the VBIOS set it: Display Core never writes it on DCN.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DCHPD_H
#define RDNA3_DCHPD_H

#include "rdna3_hw.h"
#include "dcaux.h"
#include "dcn32.h"
#include "vbios.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_HPD_POLL_MS            500
#define RD_HPD_DEBOUNCE           2      /* equal polls before a new level counts */
#define RD_HPD_EDID_TRIES         3      /* dm_helpers_read_local_edid: retry = 3 */
#define RD_HPD_EDID_RETRY_MS      100

/* program_hpd_filter: delays in ms by connector signal */
#define RD_HPD_TMDS_CONNECT_MS    500
#define RD_HPD_TMDS_DISCONNECT_MS 100
#define RD_HPD_DP_CONNECT_MS      80
#define RD_HPD_DP_DISCONNECT_MS   0

/* program_hpd_filter + dcn10_link_encoder_enable_hpd for HPD pin 'pin'
 * (HPD1 = 0) of a connector of type 'connector' (RD_ATOM_CONN_*): DVI and
 * HDMI 500/100 ms, DisplayPort and USB-C 80/0 ms; eDP / unknown: not
 * programmed (RDNA3_EINVAL), as amdgpu. 'enable' 0 = link_enable_hpd_filter
 * (false): both delays 0. */
int  dchpd_program_filter(rdna3_dev *d, uint32_t pin, uint8_t connector, int enable);

/* hw_hpd.c get_value in interrupt mode: DC_HPD_SENSE_DELAYED of the pin,
 * 1 = a sink. -1 when the register reads all-ones (the device is gone or in
 * D3: no sample). */
int  dchpd_sense_delayed(rdna3_dev *d, uint32_t pin);

/* the filter as programmed (for the log): delays in ms, the raw registers */
void dchpd_filter_info(rdna3_dev *d, uint32_t pin, uint32_t *connect_ms, uint32_t *disconnect_ms,
                       uint32_t *toggle_filt, uint32_t *fast_train);

/* debounced HPD state of every pin */
typedef struct rdna3_hpd_track {
    uint32_t npins;
    uint32_t debounce;
    int8_t   state[RD_AUX_ENGINES];     /* debounced level; -1 not seen yet */
    int8_t   raw[RD_AUX_ENGINES];       /* the last sample */
    int8_t   pend[RD_AUX_ENGINES];      /* the level being confirmed */
    uint8_t  count[RD_AUX_ENGINES];     /* polls it has held */
    uint32_t generation;                /* +1 for every accepted change */
    uint32_t events;                    /* accepted changes */
    uint32_t glitches;                  /* a level that went back before it counted */
    uint32_t polls;
} rdna3_hpd_track;

/* start from what the pins sense now (no events for them) */
void     dchpd_track_init(rdna3_hpd_track *t, rdna3_dev *d, uint32_t npins, uint32_t debounce);
/* one poll: returns the pins whose debounced level changed (bit n = pin n);
 * *raw_changed (may be NULL) gets the pins whose sample differs from the
 * previous one, debounced or not */
uint32_t dchpd_track_poll(rdna3_hpd_track *t, rdna3_dev *d, uint32_t *raw_changed);

/* dc_link_detect for the connector on AUX engine / DDC line 'aux': the sink
 * read again (dcaux_probe_one) and, while the pin senses a sink without an
 * EDID, read again up to 'tries' times RD_HPD_EDID_RETRY_MS apart. */
enum { RD_HPD_NO_SINK = 0, RD_HPD_SINK = 1, RD_HPD_SINK_NO_EDID = 2 };
int  dchpd_detect(rdna3_dev *d, uint32_t aux, rdna3_connector *c, uint32_t tries);

/* routing: the HPD pin of the VBIOS path a head's DIG back end drives, and
 * the AUX engine / DDC line of the connector on HPD pin 'pin' (this board
 * pairs DDCn with HPDn; without a path the pin's own number) */
int  dchpd_head_pin(const rdna3_dcn_head *h, const rdna3_display_path *p, uint32_t n);
int  dchpd_pin_aux(uint32_t pin, const rdna3_display_path *p, uint32_t n);
const rdna3_display_path *dchpd_pin_path(uint32_t pin, const rdna3_display_path *p, uint32_t n);

/* what a debounced change on one pin means for the display side */
typedef struct rdna3_hp_view {
    int      boot_pin;      /* head 0's (the GOP's) HPD pin, -1 unknown */
    int      clone_pin;     /* the lit clone's HPD pin, -1 no clone lit */
    uint32_t mirror;        /* rdna3mirror: 0 off, 1 mirror, 2 extended */
    int      can_clone;     /* a DMCUB to drive the PHY */
} rdna3_hp_view;
enum {
    RD_HP_NONE      = 0,
    RD_HP_BOOT      = 1,    /* the boot head's sink: the pipe stays, report only */
    RD_HP_CLONE_ON  = 2,    /* a TMDS sink on a free connector: light the clone */
    RD_HP_CLONE_OFF = 4,    /* the clone's sink is gone: take the pipe down */
};
/* 'c': the connector after dchpd_detect (NULL: not found) */
int  dchpd_decide(const rdna3_hp_view *v, uint32_t pin, int connected, const rdna3_connector *c);
const char *dchpd_action_name(int action);

#ifdef __cplusplus
}
#endif
#endif
