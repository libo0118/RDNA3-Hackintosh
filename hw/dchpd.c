/*
 * dchpd.c - see dchpd.h. Port of program_hpd_filter / link_enable_hpd_filter
 * / query_hpd_status (link_hpd.c), hw_hpd.c set_config and get_value,
 * dcn10_link_encoder_enable_hpd, and the detection decisions of
 * handle_hpd_irq_helper / dc_link_detect.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dchpd.h"
#include "gen/dcn_3_2_1_aux.h"

#define HPD_STRIDE (regHPD1_DC_HPD_CONTROL - regHPD0_DC_HPD_CONTROL)

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))
#define RHPD(d, reg, n) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + HPD_STRIDE * (n))
#define WHPD(d, reg, n, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, reg) + HPD_STRIDE * (n), (v))

int dchpd_program_filter(rdna3_dev *d, uint32_t pin, uint8_t connector, int enable)
{
    uint32_t on_ms, off_ms, v;
    if (!d->ipver[HWIP_DCE].present || pin >= RD_AUX_ENGINES)   /* HPD0..4 on DCN 3.2 */
        return RDNA3_EINVAL;
    switch (connector) {
    case RD_ATOM_CONN_DVI_D:
    case RD_ATOM_CONN_DUAL_DVI_D:
    case RD_ATOM_CONN_HDMI_A:
        on_ms = RD_HPD_TMDS_CONNECT_MS;
        off_ms = RD_HPD_TMDS_DISCONNECT_MS;
        break;
    case RD_ATOM_CONN_DISPLAYPORT:
    case RD_ATOM_CONN_USBC:      /* DP alt mode: a DisplayPort signal */
        /* "to allow DP signal to settle": 500 ms hides an MST <-> SST
         * switch, less than 80 lets passive dongles bounce */
        on_ms = RD_HPD_DP_CONNECT_MS;
        off_ms = RD_HPD_DP_DISCONNECT_MS;
        break;
    default:                     /* eDP / LVDS / unknown: don't program */
        return RDNA3_EINVAL;
    }
    if (!enable)                 /* link_enable_hpd_filter(false) */
        on_ms = off_ms = 0;
    /* link_enable_hpd -> dcn10_link_encoder_enable_hpd: the pin senses
     * only when enabled */
    v = RHPD(d, regHPD0_DC_HPD_CONTROL, pin);
    if (!GET(v, HPD0_DC_HPD_CONTROL, DC_HPD_EN))
        WHPD(d, regHPD0_DC_HPD_CONTROL, pin, SET(v, HPD0_DC_HPD_CONTROL, DC_HPD_EN, 1));
    /* hw_hpd.c set_config: REG_UPDATE_2(toggle_filt_cntl, ... / 10) */
    v = RHPD(d, regHPD0_DC_HPD_TOGGLE_FILT_CNTL, pin);
    v = SET(v, HPD0_DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_CONNECT_INT_DELAY, on_ms / 10);
    v = SET(v, HPD0_DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_DISCONNECT_INT_DELAY, off_ms / 10);
    WHPD(d, regHPD0_DC_HPD_TOGGLE_FILT_CNTL, pin, v);
    return RDNA3_OK;
}

int dchpd_sense_delayed(rdna3_dev *d, uint32_t pin)
{
    uint32_t v;
    if (!d->ipver[HWIP_DCE].present || pin >= RD_AUX_ENGINES)
        return -1;
    v = RHPD(d, regHPD0_DC_HPD_INT_STATUS, pin);
    if (v == ~0u)
        return -1;
    return GET(v, HPD0_DC_HPD_INT_STATUS, DC_HPD_SENSE_DELAYED) ? 1 : 0;
}

void dchpd_filter_info(rdna3_dev *d, uint32_t pin, uint32_t *connect_ms, uint32_t *disconnect_ms,
                       uint32_t *toggle_filt, uint32_t *fast_train)
{
    uint32_t t = 0, f = 0;
    if (d->ipver[HWIP_DCE].present && pin < RD_AUX_ENGINES) {
        t = RHPD(d, regHPD0_DC_HPD_TOGGLE_FILT_CNTL, pin);
        f = RHPD(d, regHPD0_DC_HPD_FAST_TRAIN_CNTL, pin);
    }
    if (connect_ms)
        *connect_ms = GET(t, HPD0_DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_CONNECT_INT_DELAY) * 10;
    if (disconnect_ms)
        *disconnect_ms = GET(t, HPD0_DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_DISCONNECT_INT_DELAY) * 10;
    if (toggle_filt)
        *toggle_filt = t;
    if (fast_train)
        *fast_train = f;
}

void dchpd_track_init(rdna3_hpd_track *t, rdna3_dev *d, uint32_t npins, uint32_t debounce)
{
    uint32_t n;
    for (n = 0; n < sizeof(*t); n++)
        ((uint8_t *)t)[n] = 0;
    t->npins = npins < RD_AUX_ENGINES ? npins : RD_AUX_ENGINES;
    t->debounce = debounce ? debounce : 1;
    for (n = 0; n < RD_AUX_ENGINES; n++) {
        int s = n < t->npins ? dchpd_sense_delayed(d, n) : -1;
        t->state[n] = t->raw[n] = t->pend[n] = (int8_t)s;
    }
}

int dchpd_link_resume(rdna3_dev *d, const rdna3_display_path *p, uint32_t n, rdna3_hpd_track *t)
{
    uint32_t i;
    int k = 0;
    for (i = 0; p && i < n; i++)
        if (p[i].hpd >= 0 && dchpd_program_filter(d, (uint32_t)p[i].hpd, p[i].connector, 1) == RDNA3_OK)
            k++;
    for (i = 0; t && i < RD_AUX_ENGINES; i++) {
        t->count[i] = 0;
        t->pend[i] = t->state[i];
    }
    return k;
}

uint32_t dchpd_track_poll(rdna3_hpd_track *t, rdna3_dev *d, uint32_t *raw_changed)
{
    uint32_t n, changed = 0, raw = 0;
    t->polls++;
    for (n = 0; n < t->npins; n++) {
        int s = dchpd_sense_delayed(d, n);
        if (s < 0)
            continue;                    /* no sample (D3, surprise removal) */
        if (s != t->raw[n])
            raw |= 1u << n;
        t->raw[n] = (int8_t)s;
        if (t->state[n] < 0) {           /* first readable sample: the start state */
            t->state[n] = t->pend[n] = (int8_t)s;
            continue;
        }
        if (s == t->state[n]) {
            if (t->count[n])
                t->glitches++;           /* went back before it counted */
            t->count[n] = 0;
            t->pend[n] = (int8_t)s;
            continue;
        }
        if (s != t->pend[n] || !t->count[n]) {
            t->pend[n] = (int8_t)s;
            t->count[n] = 0;
        }
        if (++t->count[n] >= t->debounce) {
            t->state[n] = (int8_t)s;
            t->count[n] = 0;
            t->generation++;
            t->events++;
            changed |= 1u << n;
        }
    }
    if (raw_changed)
        *raw_changed = raw;
    return changed;
}

int dchpd_detect(rdna3_dev *d, uint32_t aux, rdna3_connector *c, uint32_t tries)
{
    uint32_t k;
    if (!tries)
        tries = 1;
    for (k = 0; k < tries; k++) {
        if (k)
            d->os.msleep(d->os.ctx, RD_HPD_EDID_RETRY_MS);
        (void)dcaux_probe_one(d, aux, c);
        if (!c->hpd && !c->dp)
            return RD_HPD_NO_SINK;
        if (c->edid_len > 0)
            return RD_HPD_SINK;
    }
    RERR(d, "hotplug: AUX%u senses a sink but gave no EDID in %u tries", aux, tries);
    return RD_HPD_SINK_NO_EDID;
}

const rdna3_display_path *dchpd_pin_path(uint32_t pin, const rdna3_display_path *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; p && i < n; i++)
        if (p[i].hpd >= 0 && (uint32_t)p[i].hpd == pin)
            return &p[i];
    return NULL;
}

int dchpd_head_pin(const rdna3_dcn_head *h, const rdna3_display_path *p, uint32_t n)
{
    uint32_t i;
    if (!h || h->dig_be < 0 || !p)
        return -1;
    for (i = 0; i < n; i++)
        if (p[i].transmitter == h->dig_be && p[i].hpd >= 0)
            return p[i].hpd;
    return -1;
}

int dchpd_pin_aux(uint32_t pin, const rdna3_display_path *p, uint32_t n)
{
    const rdna3_display_path *q = dchpd_pin_path(pin, p, n);
    return q && q->ddc_line >= 0 ? q->ddc_line : (int)pin;
}

int dchpd_decide(const rdna3_hp_view *v, uint32_t pin, int connected, const rdna3_connector *c)
{
    if (v->boot_pin >= 0 && (uint32_t)v->boot_pin == pin)
        return v->relight && connected ? RD_HP_BOOT_RELIGHT : RD_HP_BOOT;
    /* relight (the NullMoth NVIDIA driver's answer to a monitor switched to
     * another input and back): the clone's pipe and framebuffer stay while
     * its monitor is away, and its link comes up again when it is back */
    if (v->relight && v->clone_pin >= 0 && (uint32_t)v->clone_pin == pin) {
        if (!connected)
            return RD_HP_CLONE_KEEP;
        return c ? RD_HP_CLONE_RELIGHT : RD_HP_NONE;
    }
    if (!connected)
        return v->clone_pin >= 0 && (uint32_t)v->clone_pin == pin ? RD_HP_CLONE_OFF : RD_HP_NONE;
    /* a TMDS sink with an EDID read over the DDC (HDMI, or DP++ adaptor):
     * what hw/dcn32_clone.c can light; DP sinks need link training first */
    if (v->mirror && v->can_clone && v->clone_pin < 0 && c && c->hpd && !c->dp && c->edid_len > 0 &&
        c->edid_ddc)
        return RD_HP_CLONE_ON;
    return RD_HP_NONE;
}

const char *dchpd_action_name(int action)
{
    switch (action) {
    case RD_HP_BOOT:      return "boot head: pipe kept, reported";
    case RD_HP_CLONE_ON:  return "light the second monitor";
    case RD_HP_CLONE_OFF: return "take the second monitor's pipe down";
    case RD_HP_BOOT_RELIGHT:  return "boot head: its link brought up again";
    case RD_HP_CLONE_KEEP:    return "second monitor away: its pipe kept";
    case RD_HP_CLONE_RELIGHT: return "second monitor back: its link brought up again";
    default:              return "nothing to do";
    }
}
