/*
 * dcn32_dp.c - see dcn32_dp.h. A port of the DP SST / 8b/10b part of
 * amdgpu Display Core (Linux v6.12, MIT); every function names the one it
 * ports. DC's objects (dc_link, link_encoder, stream_encoder, pipe_ctx)
 * collapse into rdna3_dp_link; the link hwss is the DIO one; the VBIOS
 * transmitter commands go to the DMCUB as DC does on DCN 3.2
 * (debug.dmub_command_table).
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_dp.h"
#include "vbios.h"
#include "gen/dcn_3_2_0_dig.h"
#include "gen/dcn_3_2_0_dp.h"
#include "gen/dcn_3_2_0_pipe.h"
#include "dcn32_dp_mst.h"
#include "dcn32_hdmi.h"
#include "dcn32_dsc.h"
#include "gen/dcn_3_2_0_dpsec.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define SET(v, reg, field, x) \
    (((v) & ~(uint32_t)reg##__##field##_MASK) | \
     (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK))

#define RI(d, reg, n, stride) rd((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n))
#define WI(d, reg, n, stride, v) wr((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n), (v))
#define AI(d, reg, n, stride) \
    rdna3_reg_offset((d), HWIP_DCE, 0, reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n))

#define DP_S   (regDP1_DP_LINK_CNTL - regDP0_DP_LINK_CNTL)    /* DPn / DIGn: 0x100 */
#define DIG_S  DP_S
#define DTO_S  (regDP_DTO1_PHASE - regDP_DTO0_PHASE)           /* DP_DTOn, OTGn_PIXEL_RATE_CNTL: 4 */

static uint32_t rd(rdna3_dev *d, uint32_t seg, uint32_t off)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off));
}

static void wr(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off), v);
}

static void upd(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t mask, uint32_t shift, uint32_t x)
{
    uint32_t v = rd(d, seg, off);
    wr(d, seg, off, (v & ~mask) | ((x << shift) & mask));
}
/* REG_UPDATE(reg, field, x) on DIG / DP instance n */
#define UPDATE(d, reg, n, stride, regf, field, x) \
    upd((d), reg##_BASE_IDX, (uint32_t)(reg) + (uint32_t)(stride) * (n), \
        (uint32_t)regf##__##field##_MASK, regf##__##field##__SHIFT, (uint32_t)(x))

/* fsleep */
static void fsleep(rdna3_dev *d, uint32_t us)
{
    if (us >= 1000)
        d->os.msleep(d->os.ctx, us / 1000);
    if (us % 1000)
        d->os.udelay(d->os.ctx, us % 1000);
}

static const char *rate_name(uint32_t r)
{
    switch (r) {
    case RD_DP_LINK_RATE_LOW: return "RBR";
    case RD_DP_LINK_RATE_HIGH: return "HBR";
    case RD_DP_LINK_RATE_RBR2: return "RBR2";
    case RD_DP_LINK_RATE_HIGH2: return "HBR2";
    case RD_DP_LINK_RATE_HIGH3: return "HBR3";
    default: return "Unknown";
    }
}

/* ---- DPCD (core_link_read_dpcd / core_link_write_dpcd) ---- */

static int core_link_read_dpcd(rdna3_dp_link *l, uint32_t a, uint8_t *b, uint32_t n)
{
    int r = dcaux_dpcd_read(l->d, l->aux, a, b, n);
    return r == (int)n ? RDNA3_OK : (r < 0 ? r : RDNA3_EIO);
}

static int core_link_write_dpcd(rdna3_dp_link *l, uint32_t a, const uint8_t *b, uint32_t n)
{
    int r = dcaux_dpcd_write(l->d, l->aux, a, b, n);
    return r == (int)n ? RDNA3_OK : (r < 0 ? r : RDNA3_EIO);
}

static int dpcd_write1(rdna3_dp_link *l, uint32_t a, uint8_t v)
{
    return core_link_write_dpcd(l, a, &v, 1);
}

/* ---- bandwidth (dc_dsc.c, link_validation.c) ---- */

/* dc_bandwidth_in_kbps_from_timing: no DSC, RGB (not Y-only, not 4:2:x) */
uint32_t dcn32_dp_stream_kbps(uint32_t pixel_khz, uint32_t bpc)
{
    uint32_t kbps = pixel_khz;
    kbps *= bpc ? bpc : 8;
    kbps *= 3;
    return kbps;
}

/* dp_link_bandwidth_kbps: 8b/10b, link rate in LINK_RATE_REF_FREQ_IN_KHZ
 * per DP byte per lane, DATA_EFFICIENCY_8b_10b_x10000 = 8000, times
 * DATA_EFFICIENCY_8b_10b_FEC_EFFICIENCY_x100 = 97 % with FEC */
static uint32_t dp_link_bandwidth_kbps(const rdna3_dp_link_settings *s, int fec)
{
    uint32_t per_lane, eff;
    if (s->link_rate < RD_DP_LINK_RATE_LOW || s->link_rate > RD_DP_LINK_RATE_HIGH3)
        return 0;   /* link_dp_get_encoding_format: not 8b/10b */
    per_lane = s->link_rate * RD_DP_LINK_RATE_REF_FREQ_IN_KHZ * 10;   /* BITS_PER_DP_BYTE */
    eff = 8000;
    if (fec) {
        eff /= 100;
        eff *= 97;
    }
    return per_lane * s->lane_count / 10000 * eff;
}

uint32_t dcn32_dp_link_kbps_settings(const rdna3_dp_link_settings *s)
{
    return dp_link_bandwidth_kbps(s, 0);
}

/* dp_is_fec_supported: the DCN 3.2 link encoder has fec_supported */
static int dp_is_fec_supported(const rdna3_dp_link *l)
{
    return (l->dpcd_caps.fec_cap & RD_DP_FEC_CAPABLE) != 0;
}

/* dp_should_enable_fec (no panel patch, not eDP), behind the link's FEC
 * policy (off: FEC never, as before rdna3dsc) */
int dcn32_dp_should_enable_fec(const rdna3_dp_link *l)
{
    if (!l->fec_policy)
        return 0;
    if (l->fec_state == RD_FEC_ENABLED)
        return 1;
    return dp_is_fec_supported(l);
}

/* dp_link_bandwidth_kbps for this link (FEC as dp_should_enable_fec says) */
uint32_t dcn32_dp_link_bw_kbps(const rdna3_dp_link *l, const rdna3_dp_link_settings *s)
{
    return dp_link_bandwidth_kbps(s, dcn32_dp_should_enable_fec(l));
}

/* ---- capabilities (link_dp_capability.c) ---- */

/* dp_parse_lttpr_repeater_count */
static uint32_t dp_parse_lttpr_repeater_count(uint8_t cnt)
{
    switch (cnt) {
    case 0x80: return 1;
    case 0x40: return 2;
    case 0x20: return 3;
    case 0x10: return 4;
    case 0x08: return 5;
    case 0x04: return 6;
    case 0x02: return 7;
    case 0x01: return 8;
    default: return 0;
    }
}

/* dp_is_lttpr_present */
static int dp_is_lttpr_present(const rdna3_dp_link *l)
{
    return dp_parse_lttpr_repeater_count(l->dpcd_caps.lttpr.phy_repeater_cnt) != 0 &&
           l->dpcd_caps.lttpr.max_lane_count > 0 && l->dpcd_caps.lttpr.max_lane_count <= 4;
}

/* get_link_rate_from_max_link_bw */
static uint32_t get_link_rate_from_max_link_bw(uint8_t bw)
{
    if (bw >= RD_DP_LINK_RATE_HIGH3)
        return RD_DP_LINK_RATE_HIGH3;
    if (bw >= RD_DP_LINK_RATE_HIGH2)
        return RD_DP_LINK_RATE_HIGH2;
    if (bw >= RD_DP_LINK_RATE_HIGH)
        return RD_DP_LINK_RATE_HIGH;
    if (bw >= RD_DP_LINK_RATE_LOW)
        return RD_DP_LINK_RATE_LOW;
    return RD_DP_LINK_RATE_UNKNOWN;
}

/* get_lttpr_max_link_rate (the UHBR rates are not supported here) */
static uint32_t get_lttpr_max_link_rate(const rdna3_dp_link *l)
{
    switch (l->dpcd_caps.lttpr.max_link_rate) {
    case RD_DP_LINK_RATE_LOW:
    case RD_DP_LINK_RATE_HIGH:
    case RD_DP_LINK_RATE_HIGH2:
    case RD_DP_LINK_RATE_HIGH3:
        return l->dpcd_caps.lttpr.max_link_rate;
    default:
        return RD_DP_LINK_RATE_UNKNOWN;
    }
}

/* wake_up_aux_channel */
static int wake_up_aux_channel(rdna3_dp_link *l)
{
    int status = RDNA3_EIO;
    uint32_t retry = 0;
    uint8_t state = 0;
    while (status != RDNA3_OK && retry < 10) {
        status = core_link_read_dpcd(l, RD_DPCD_SET_POWER, &state, 1);
        /* the AUX CH may be powered down (600h = 2): up to 1 ms to answer */
        if (status != RDNA3_OK || state == RD_DP_SET_POWER_D3) {
            fsleep(l->d, 1000);
            retry++;
        }
    }
    if (status != RDNA3_OK) {
        dpcd_write1(l, RD_DPCD_SET_POWER, RD_DP_SET_POWER_D0);
        dpcd_write1(l, RD_DPCD_SET_POWER, RD_DP_SET_POWER_D3);
        RLOG(l->d, "dp: AUX %u: failed to power up sink", l->aux);
        return status;
    }
    return RDNA3_OK;
}

/* dp_retrieve_lttpr_cap */
static int dp_retrieve_lttpr_cap(rdna3_dp_link *l)
{
    uint8_t b[8] = { 0 };
    int status;
    rdna3_dp_lttpr_caps *c = &l->dpcd_caps.lttpr;
    if (!l->lttpr_aware)
        return RDNA3_ENOENT;   /* DC_NOT_SUPPORTED */
    status = core_link_read_dpcd(l, RD_DPCD_LTTPR_REV, b, sizeof(b));
    c->revision = b[0];
    c->max_link_rate = b[1];
    c->phy_repeater_cnt = b[2];
    c->mode = b[3];
    c->max_lane_count = b[4];
    c->max_ext_timeout = b[5];
    c->main_link_channel_coding = b[6];
    c->supported_128b_132b_rates = b[7];
    if (dp_is_lttpr_present(l))
        RLOG(l->d, "dp: AUX %u: LTTPR caps %02x %02x %02x %02x %02x %02x %02x %02x", l->aux, b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7]);
    return status;
}

/* configure_lttpr_mode_transparent */
static int configure_lttpr_mode_transparent(rdna3_dp_link *l)
{
    return dpcd_write1(l, RD_DPCD_PHY_REPEATER_MODE, RD_DP_PHY_REPEATER_MODE_TRANSPARENT);
}

/* dcn10_link_encoder_get_max_link_cap with the features
 * dcn32_link_encoder_construct takes from the VBIOS encoder caps record
 * (link_enc_feature: HBR2, HBR3 when there is none); UHBR left out */
static void dcn10_link_encoder_get_max_link_cap(const rdna3_dp_link *l, rdna3_dp_link_settings *s)
{
    int hbr2 = 1, hbr3 = 1;
    if (l->enc_caps) {
        hbr2 = (l->enc_caps & RD_ATOM_ENC_CAP_HBR2) != 0;
        hbr3 = (l->enc_caps & RD_ATOM_ENC_CAP_HBR3) != 0;
    }
    s->lane_count = 4;
    s->link_rate = RD_DP_LINK_RATE_HIGH;
    s->link_spread = RD_DP_LINK_SPREAD_05_DOWNSPREAD_30KHZ;
    if (hbr2)
        s->link_rate = RD_DP_LINK_RATE_HIGH2;
    if (hbr3)
        s->link_rate = RD_DP_LINK_RATE_HIGH3;
}

/* dp_get_max_link_cap (no cable id: DP2 only; no UHBR) */
static void dp_get_max_link_cap(rdna3_dp_link *l, rdna3_dp_link_settings *max)
{
    dcn10_link_encoder_get_max_link_cap(l, max);
    if (l->reported_link_cap.lane_count < max->lane_count)
        max->lane_count = l->reported_link_cap.lane_count;
    if (l->reported_link_cap.link_rate < max->link_rate)
        max->link_rate = l->reported_link_cap.link_rate;
    if (l->reported_link_cap.link_spread < max->link_spread)
        max->link_spread = l->reported_link_cap.link_spread;
    /* repeaters do not snoop the DPRX caps: their own limits count */
    if (dp_is_lttpr_present(l) && l->dpcd_caps.lttpr.revision >= 0x14) {
        uint32_t r = get_lttpr_max_link_rate(l);
        if (l->dpcd_caps.lttpr.max_lane_count < max->lane_count)
            max->lane_count = l->dpcd_caps.lttpr.max_lane_count;
        if (r < max->link_rate)
            max->link_rate = r;
    }
}

void dcn32_dp_link_init(rdna3_dp_link *l, rdna3_dev *d, rdna3_dmub *dm, uint32_t aux, uint32_t be, uint32_t fe,
                        uint32_t otg, int hpd, uint8_t connector_id, uint8_t encoder_id, uint32_t enc_caps,
                        uint32_t dprefclk_khz)
{
    uint32_t i;
    for (i = 0; i < sizeof(*l); i++)
        ((uint8_t *)l)[i] = 0;
    l->d = d;
    l->dm = dm;
    l->aux = aux;
    l->be = be;
    l->fe = fe;
    l->otg = otg;
    l->hpd = hpd;
    l->connector_id = connector_id;
    l->encoder_id = encoder_id;
    l->enc_caps = enc_caps;
    l->dprefclk_khz = dprefclk_khz;
    /* dc->caps.extended_aux_timeout_support is set on DCN 3.2; the VBIOS
     * LTTPR interop flag is assumed (this port does not parse it) */
    l->lttpr_aware = 1;
}

/* is_dp_active_dongle: a VGA / DVI / HDMI converter */
int dcn32_dp_is_active_dongle(const rdna3_dp_link *l)
{
    return l->dpcd_caps.dongle_type >= RD_DONGLE_DP_VGA && l->dpcd_caps.dongle_type <= RD_DONGLE_DP_HDMI;
}

/* is_dp_branch_device */
int dcn32_dp_is_branch_device(const rdna3_dp_link *l)
{
    return l->dpcd_caps.is_branch_dev;
}

/* translate_dpcd_max_bpc (enum dpcd_downstream_port_max_bpc, 2 bits) */
static uint32_t translate_dpcd_max_bpc(uint32_t bpc)
{
    switch (bpc) {
    case 0: return 8;
    case 1: return 10;
    case 2: return 12;
    case 3: return 16;
    default: return 0;   /* -1 in DC: never reached with 2 bits */
    }
}

/* link_bw_kbps_from_raw_frl_link_rate_data */
static uint32_t link_bw_kbps_from_raw_frl_link_rate_data(uint8_t bw)
{
    switch (bw) {
    case 1: return 9000000;
    case 2: return 18000000;
    case 3: return 24000000;
    case 4: return 32000000;
    case 5: return 40000000;
    case 6: return 48000000;
    default: return 0;
    }
}

/* intersect_frl_link_bw_support: the PCON's post FRL training bandwidth
 * (HDMI_ENCODED_LINK_BW, valid with FRL_MODE) when it reports one */
static uint32_t intersect_frl_link_bw_support(uint32_t max_supported_frl_bw_in_kbps, uint8_t hdmi_encoded_link_bw)
{
    uint32_t bw = max_supported_frl_bw_in_kbps;
    if (hdmi_encoded_link_bw & 0x01) {             /* FRL_MODE */
        if (hdmi_encoded_link_bw & 0x40)
            bw = 48000000;
        else if (hdmi_encoded_link_bw & 0x20)
            bw = 40000000;
        else if (hdmi_encoded_link_bw & 0x10)
            bw = 32000000;
        else if (hdmi_encoded_link_bw & 0x08)
            bw = 24000000;
        else if (hdmi_encoded_link_bw & 0x04)
            bw = 18000000;
        else if (hdmi_encoded_link_bw & 0x02)
            bw = 9000000;
    }
    return bw;
}

/* get_active_converter_info: the branch / converter type from
 * DOWNSTREAMPORT_PRESENT and, from DPCD 1.1, the detailed caps of
 * downstream port 0 (a DP-to-HDMI converter's TMDS clock, bpc, 4:2:x
 * pass-through, FRL bandwidth: dc->caps.dp_hdmi21_pcon_support is set on
 * DCN 3.2), the branch revisions, and from DPCD 1.4 the DFP capability
 * extension */
static void get_active_converter_info(rdna3_dp_link *l, uint8_t data)
{
    rdna3_dp_caps *c = &l->dpcd_caps;
    rdna3_dp_dongle_caps *dg = &c->dongle_caps;
    uint8_t det[16] = { 0 }, rev[3] = { 0 };
    uint32_t i;

    for (i = 0; i < sizeof(*dg); i++)
        ((uint8_t *)dg)[i] = 0;
    if (!(data & 1)) {                     /* PORT_PRESENT */
        c->dongle_type = RD_DONGLE_NONE;
        c->is_branch_dev = 0;
        return;
    }
    c->is_branch_dev = 1;
    switch ((data >> 1) & 3) {             /* PORT_TYPE */
    case 1: c->dongle_type = RD_DONGLE_DP_VGA; break;    /* DOWNSTREAM_VGA */
    case 2: c->dongle_type = RD_DONGLE_DP_DVI; break;    /* DOWNSTREAM_DVI_HDMI_DP_PLUS_PLUS */
    default: c->dongle_type = RD_DONGLE_NONE; break;
    }
    if (c->dpcd_rev >= 0x11 && core_link_read_dpcd(l, RD_DPCD_DOWNSTREAM_PORT_0, det, sizeof(det)) == RDNA3_OK) {
        switch (det[0] & 7) {              /* DWN_STRM_PORTX_TYPE */
        case 0: c->dongle_type = RD_DONGLE_NONE; break;      /* DP: as DONGLE_NONE */
        case 1: c->dongle_type = RD_DONGLE_DP_VGA; break;
        case 2: c->dongle_type = RD_DONGLE_DP_DVI; break;
        case 3: case 5:
            /* DP++ active converters as HDMI (DP 1.4) */
            c->dongle_type = RD_DONGLE_DP_HDMI;
            dg->dongle_type = c->dongle_type;
            if (data & 0x10) {             /* DETAILED_CAPS */
                uint8_t hdmi_caps = det[3], color = det[2], post = 0;
                dg->dp_hdmi_max_pixel_clk_in_khz = det[1] * 2500u;
                dg->is_dp_hdmi_s3d_converter = hdmi_caps & 1;            /* FRAME_SEQ_TO_FRAME_PACK */
                if ((det[0] & 7) == 3) {   /* YCbCr caps only for HDMI */
                    dg->is_dp_hdmi_ycbcr422_pass_through = (hdmi_caps >> 1) & 1;
                    dg->is_dp_hdmi_ycbcr420_pass_through = (hdmi_caps >> 2) & 1;
                    dg->is_dp_hdmi_ycbcr422_converter = (hdmi_caps >> 3) & 1;
                    dg->is_dp_hdmi_ycbcr420_converter = (hdmi_caps >> 4) & 1;
                }
                dg->dp_hdmi_max_bpc = translate_dpcd_max_bpc(color & 3);   /* MAX_BITS_PER_COLOR_COMPONENT */
                dg->dp_hdmi_frl_max_link_bw_in_kbps = link_bw_kbps_from_raw_frl_link_rate_data((uint8_t)((color >> 2) & 7));
                if (core_link_read_dpcd(l, RD_DPCD_PCON_HDMI_POST_FRL_STATUS, &post, 1) == RDNA3_OK)
                    dg->dp_hdmi_frl_max_link_bw_in_kbps =
                        intersect_frl_link_bw_support(dg->dp_hdmi_frl_max_link_bw_in_kbps, post);
                if (dg->dp_hdmi_frl_max_link_bw_in_kbps > 0)
                    dg->extended_cap_valid = 1;
                if (dg->dp_hdmi_max_pixel_clk_in_khz != 0)
                    dg->extended_cap_valid = 1;
            }
            break;
        default: break;
        }
    }
    core_link_read_dpcd(l, RD_DPCD_BRANCH_REVISION_START, rev, sizeof(rev));
    c->branch_hw_revision = rev[0];
    c->branch_fw_revision[0] = rev[1];
    c->branch_fw_revision[1] = rev[2];
    if (c->dpcd_rev >= 0x14 && c->dongle_type != RD_DONGLE_NONE) {
        uint8_t x[12] = { 0 };
        core_link_read_dpcd(l, RD_DPCD_DFP_CAPABILITY_EXTENSION_SUPPORT, x, sizeof(x));
        dg->dfp_cap_ext.supported = x[0];
        dg->dfp_cap_ext.max_pixel_rate_in_mps = x[1] + ((uint32_t)x[2] << 8);
        dg->dfp_cap_ext.max_video_h_active_width = x[3] + ((uint32_t)x[4] << 8);
        dg->dfp_cap_ext.max_video_v_active_height = x[5] + ((uint32_t)x[6] << 8);
        dg->dfp_cap_ext.encoding_format_caps = x[7];
        dg->dfp_cap_ext.rgb_color_depth_caps = x[8];
        dg->dfp_cap_ext.ycbcr444_color_depth_caps = x[9];
        dg->dfp_cap_ext.ycbcr422_color_depth_caps = x[10];
        dg->dfp_cap_ext.ycbcr420_color_depth_caps = x[11];
    }
}

/* read_dp_device_vendor_id: the branch OUI and device name (DPCD 0x500) */
static void read_dp_device_vendor_id(rdna3_dp_link *l)
{
    uint8_t id[9] = { 0 };
    uint32_t i;
    core_link_read_dpcd(l, RD_DPCD_BRANCH_OUI, id, sizeof(id));
    l->dpcd_caps.branch_dev_id = ((uint32_t)id[0] << 16) + ((uint32_t)id[1] << 8) + id[2];
    for (i = 0; i < 6; i++)
        l->dpcd_caps.branch_dev_name[i] = id[3 + i];
}

/* dp_wa_power_up_0010FA: a sink reporting DPCD rev 0 gets D0 and a re-read;
 * the 0010FA / 0080E1 / 00E04C VGA converters keep their receiver powered */
static void dp_wa_power_up_0010FA(rdna3_dp_link *l, uint8_t *dpcd_data, uint32_t length)
{
    int retry = 0;
    if (!l->dpcd_caps.dpcd_rev) {
        do {
            dpcd_write1(l, RD_DPCD_SET_POWER, RD_DP_SET_POWER_D0);   /* dpcd_write_rx_power_ctrl(true) */
            core_link_read_dpcd(l, RD_DPCD_REV, dpcd_data, length);
            l->dpcd_caps.dpcd_rev = dpcd_data[RD_DPCD_REV];
        } while (retry++ < 4 && !l->dpcd_caps.dpcd_rev);
    }
    if (l->dpcd_caps.dongle_type == RD_DONGLE_DP_VGA) {
        switch (l->dpcd_caps.branch_dev_id) {
        case 0x0010fa:   /* DP_BRANCH_DEVICE_ID_0010FA */
        case 0x0080e1:   /* DP_BRANCH_DEVICE_ID_0080E1 */
        case 0x00e04c:   /* DP_BRANCH_DEVICE_ID_00E04C */
            l->keep_receiver_powered = 1;
            break;
        default:
            l->keep_receiver_powered = 0;
            break;
        }
    } else {
        l->keep_receiver_powered = 0;
    }
}

/* dpcd_read_sink_ext_caps: DP_SOURCE_SINK_CAP and, from eDP's general cap
 * 2, panel luminance control */
static int dpcd_read_sink_ext_caps(rdna3_dp_link *l)
{
    uint8_t dpcd_data = 0, edp_general_cap2 = 0;
    if (core_link_read_dpcd(l, RD_DPCD_SOURCE_SINK_CAP, &dpcd_data, 1) != RDNA3_OK)
        return 0;
    l->dpcd_sink_ext_caps = dpcd_data;
    if (core_link_read_dpcd(l, RD_DPCD_EDP_GENERAL_CAP_2, &edp_general_cap2, 1) != RDNA3_OK)
        return 0;
    l->dpcd_caps.panel_luminance_control = (edp_general_cap2 & 0x10) != 0;   /* DP_EDP_PANEL_LUMINANCE_CONTROL_CAPABLE */
    return 1;
}

/* try_to_configure_aux_timeout -> dce_aux_configure_timeout (DCN 3.2 has

 * extended_aux_timeout_support; the connector is a PHY endpoint) */
static void try_to_configure_aux_timeout(rdna3_dp_link *l, uint32_t timeout_us)
{
    dcaux_configure_timeout(l->d, l->aux, timeout_us);
}

/* dpcd_set_source_specific_data (no vendor signature override): the AMD
 * IEEE OUI 00-00-1A unless the sink holds it already, the device id (PCI
 * id, dce_version DCN_VERSION_3_2), DCN 3.2's minimum h blank (80) */
static void dpcd_set_source_specific_data(rdna3_dp_link *l)
{
    uint8_t sig[3] = { 0 }, id[9] = { 0 }, hblank = 80;   /* dc->caps.min_horizontal_blanking_period */
    core_link_read_dpcd(l, RD_DPCD_SOURCE_OUI, sig, sizeof(sig));
    if (!(sig[0] == 0x00 && sig[1] == 0x00 && sig[2] == 0x1a)) {
        sig[0] = 0x00;
        sig[1] = 0x00;
        sig[2] = 0x1a;
        core_link_write_dpcd(l, RD_DPCD_SOURCE_OUI, sig, sizeof(sig));
    }
    id[0] = (uint8_t)l->chip_id;          /* device_id_byte1 */
    id[1] = (uint8_t)(l->chip_id >> 8);   /* device_id_byte2; zero[4] */
    id[6] = 26;                           /* dce_version: DCN_VERSION_3_2 */
    core_link_write_dpcd(l, RD_DPCD_SOURCE_OUI + 3, id, sizeof(id));
    core_link_write_dpcd(l, RD_DPCD_SOURCE_MINIMUM_HBLANK_SUPPORTED, &hblank, 1);
}

/* retrieve_link_cap (subset; detect_dp_sink_caps is this) + dp_get_max_link_cap */
int dcn32_dp_read_caps(rdna3_dp_link *l)
{
    uint8_t dpcd[16], ext[16], b;
    uint8_t oui[3] = { 0 };
    rdna3_dp_caps *c = &l->dpcd_caps;
    int status = RDNA3_EIO, i;

    l->caps_valid = 0;
    for (i = 0; i < 16; i++)
        dpcd[i] = 0;
    /* the LTTPR timeout (3.2 ms) first: DP link layer CTS 4.2.1.1 */
    try_to_configure_aux_timeout(l, RD_LINK_AUX_DEFAULT_LTTPR_TIMEOUT_PERIOD);
    status = dp_retrieve_lttpr_cap(l);
    if (status != RDNA3_OK) {
        status = wake_up_aux_channel(l);
        if (status == RDNA3_OK)
            dp_retrieve_lttpr_cap(l);
        else
            return RDNA3_ENODEV;
    }
    if (dp_is_lttpr_present(l)) {
        configure_lttpr_mode_transparent(l);
        /* echo TOTAL_LTTPR_CNT back downstream */
        core_link_write_dpcd(l, RD_DPCD_TOTAL_LTTPR_CNT, &c->lttpr.phy_repeater_cnt, 1);
    }
    dpcd_set_source_specific_data(l);
    /* the sink may configure itself for the vendor: 30 ms before more */
    l->d->os.msleep(l->d->os.ctx, 30);
    for (i = 0; i < 3; i++) {
        status = core_link_read_dpcd(l, RD_DPCD_REV, dpcd, sizeof(dpcd));
        if (status == RDNA3_OK)
            break;
    }
    if (status != RDNA3_OK) {
        RERR(l->d, "dp: AUX %u: read receiver caps dpcd data failed (%d)", l->aux, status);
        return RDNA3_EIO;
    }
    if (!dp_is_lttpr_present(l))
        try_to_configure_aux_timeout(l, RD_LINK_AUX_DEFAULT_TIMEOUT_PERIOD);
    c->ext_receiver_cap_field_present = (dpcd[RD_DPCD_TRAINING_AUX_RD_INTERVAL] & 0x80) != 0;
    if (c->ext_receiver_cap_field_present) {
        for (i = 0; i < 3; i++) {
            status = core_link_read_dpcd(l, RD_DPCD_DP13_DPCD_REV, ext, sizeof(ext));
            if (status == RDNA3_OK) {
                uint32_t k;
                for (k = 0; k < sizeof(dpcd); k++)
                    dpcd[k] = ext[k];
                break;
            }
        }
        if (status != RDNA3_OK)
            RERR(l->d, "dp: AUX %u: read extend caps data failed, use cap from dpcd 0", l->aux);
    }
    for (i = 0; i < 16; i++)
        c->rx[i] = dpcd[i];
    c->dpcd_rev = dpcd[RD_DPCD_REV];
    c->dprx_feature = 0;
    if (c->ext_receiver_cap_field_present) {
        b = 0;
        for (i = 0; i < 3; i++)
            if (core_link_read_dpcd(l, RD_DPCD_DPRX_FEATURE_ENUMERATION_LIST, &b, 1) == RDNA3_OK)
                break;
        c->dprx_feature = b;
        /* AdaptiveSyncCapability */
        b = 0;
        for (i = 0; i < 3; i++)
            if (core_link_read_dpcd(l, RD_DPCD_DPRX_FEATURE_ENUMERATION_LIST_CONT_1, &b, 1) == RDNA3_OK)
                break;
        c->adaptive_sync_caps = b;
    }
    /* a sink cannot report MAX_LANE_COUNT 0 */
    if (!(dpcd[RD_DPCD_MAX_LANE_COUNT] & RD_DP_LANE_COUNT_MASK))
        return RDNA3_EIO;
    read_dp_device_vendor_id(l);
    c->allow_invalid_msa_timing_param = (dpcd[RD_DPCD_DOWN_STREAM_PORT_COUNT] >> 6) & 1;
    c->max_ln_count = dpcd[RD_DPCD_MAX_LANE_COUNT];
    c->max_down_spread = dpcd[RD_DPCD_MAX_DOWNSPREAD];
    c->channel_coding_cap = dpcd[RD_DPCD_MAIN_LINK_CHANNEL_CODING];
    l->reported_link_cap.lane_count = c->max_ln_count & RD_DP_LANE_COUNT_MASK;
    l->reported_link_cap.link_rate = get_link_rate_from_max_link_bw(dpcd[RD_DPCD_MAX_LINK_RATE]);
    l->reported_link_cap.link_spread = (c->max_down_spread & RD_DP_MAX_DOWNSPREAD_0_5)
                                           ? RD_DP_LINK_SPREAD_05_DOWNSPREAD_30KHZ : RD_DP_LINK_SPREAD_DISABLED;
    get_active_converter_info(l, dpcd[RD_DPCD_DOWNSTREAMPORT_PRESENT]);
    dp_wa_power_up_0010FA(l, dpcd, sizeof(dpcd));
    /* dpcd_read_sink_ext_caps (0 when it fails) */
    if (!dpcd_read_sink_ext_caps(l))
        l->dpcd_sink_ext_caps = 0;
    b = 0;
    core_link_read_dpcd(l, RD_DPCD_SINK_COUNT, &b, 1);
    c->sink_count = b;
    {
        /* the sink's IEEE OUI and device id string, HW / FW revision */
        uint8_t sid[9] = { 0 }, rev[3] = { 0 };
        core_link_read_dpcd(l, RD_DPCD_SINK_OUI, sid, sizeof(sid));
        for (i = 0; i < 3; i++)
            oui[i] = sid[i];
        for (i = 0; i < 6; i++)
            c->sink_dev_id_str[i] = sid[3 + i];
        core_link_read_dpcd(l, RD_DPCD_SINK_HW_REVISION_START, rev, sizeof(rev));
        c->sink_hw_revision = rev[0];
        c->sink_fw_revision[0] = rev[1];
        c->sink_fw_revision[1] = rev[2];
    }
    c->sink_dev_id = ((uint32_t)oui[0] << 16) | ((uint32_t)oui[1] << 8) | oui[2];
    {
        /* quirk for Retina panels: wrong DP_MAX_LINK_RATE (the 16,1 models
         * share the device id: the firmware revision decides) */
        static const uint8_t str_mbp_2018[6] = { 101, 68, 21, 103, 98, 97 };
        static const uint8_t fw_2018[2] = { 7, 4 }, fw_2018_vega[2] = { 8, 4 };
        int k, same = 1;
        for (k = 0; k < 6; k++)
            if (c->sink_dev_id_str[k] != str_mbp_2018[k])
                same = 0;
        if (c->sink_dev_id == 0x0010fa && same &&
            ((c->sink_fw_revision[0] == fw_2018[0] && c->sink_fw_revision[1] == fw_2018[1]) ||
             (c->sink_fw_revision[0] == fw_2018_vega[0] && c->sink_fw_revision[1] == fw_2018_vega[1])))
            l->reported_link_cap.link_rate = RD_DP_LINK_RATE_RBR2;
    }
    /* DSC and FEC sink caps from DPCD 1.4 */
    c->fec_cap = 0;
    for (i = 0; i < RD_DSC_DPCD_BASIC; i++)
        c->dsc_basic[i] = 0;
    for (i = 0; i < RD_DSC_DPCD_BRANCH; i++)
        c->dsc_branch[i] = 0;
    if (c->dpcd_rev >= 0x14) {
        core_link_read_dpcd(l, RD_DPCD_FEC_CAPABILITY, &c->fec_cap, 1);
        core_link_read_dpcd(l, RD_DPCD_DSC_SUPPORT, c->dsc_basic, RD_DSC_DPCD_BASIC);
        if (c->dongle_type != RD_DONGLE_NONE)
            core_link_read_dpcd(l, RD_DPCD_DSC_BRANCH_OVERALL_THROUGHPUT_0, c->dsc_branch, RD_DSC_DPCD_BRANCH);
    }
    /* read_is_mst_supported (the cap byte kept: DP_MST_CAP bit 0) */
    c->mstm_cap = 0;
    if (c->dpcd_rev >= 0x12)
        core_link_read_dpcd(l, RD_DPCD_MSTM_CAP, &c->mstm_cap, 1);
    /* 128b/132b (DP_128b_132b_SUPPORTED) is not supported by this port */
    dp_get_max_link_cap(l, &l->max_link_cap);
    l->caps_valid = 1;
    RLOG(l->d, "dp: AUX %u: DPCD %x.%x, sink %s x%u%s%s%s, OUI %06x, LTTPR %u, max link %s x%u", l->aux,
         c->dpcd_rev >> 4, c->dpcd_rev & 15, rate_name(l->reported_link_cap.link_rate),
         l->reported_link_cap.lane_count, (c->max_ln_count & RD_DP_TPS3_SUPPORTED) ? " TPS3" : "",
         (c->max_down_spread & RD_DP_TPS4_SUPPORTED) ? " TPS4" : "",
         (c->max_down_spread & RD_DP_MAX_DOWNSPREAD_0_5) ? " SSC" : "", c->sink_dev_id,
         dp_parse_lttpr_repeater_count(c->lttpr.phy_repeater_cnt), rate_name(l->max_link_cap.link_rate),
         l->max_link_cap.lane_count);
    if (c->fec_cap || (c->dsc_basic[0] & RD_DP_DSC_DECOMPRESSION_IS_SUPPORTED) || c->mstm_cap || c->is_branch_dev)
        RLOG(l->d, "dp: AUX %u: FEC %s, DSC %s (rev %02x, slices %02x, max slice width %u, bpc %02x), MST %s, branch %d (dongle %d)",
             l->aux, (c->fec_cap & RD_DP_FEC_CAPABLE) ? "yes" : "no",
             (c->dsc_basic[0] & RD_DP_DSC_DECOMPRESSION_IS_SUPPORTED) ? "yes" : "no", c->dsc_basic[1],
             c->dsc_basic[4], c->dsc_basic[0xc] * 320u, c->dsc_basic[0xa], (c->mstm_cap & 1) ? "yes" : "no",
             c->is_branch_dev, c->dongle_type);
    return RDNA3_OK;
}

/* ---- link settings (link_dp_capability.c) ---- */

/* reduce_lane_count */
static uint32_t reduce_lane_count(uint32_t n)
{
    return n == 4 ? 2 : n == 2 ? 1 : 0;
}

/* reduce_link_rate (no eDP 1.5 rates) */
static uint32_t reduce_link_rate(uint32_t r)
{
    switch (r) {
    case RD_DP_LINK_RATE_HIGH3: return RD_DP_LINK_RATE_HIGH2;
    case RD_DP_LINK_RATE_RATE_8: return RD_DP_LINK_RATE_HIGH2;
    case RD_DP_LINK_RATE_HIGH2: return RD_DP_LINK_RATE_HIGH;
    case RD_DP_LINK_RATE_RATE_6:
    case RD_DP_LINK_RATE_RBR2: return RD_DP_LINK_RATE_HIGH;
    case RD_DP_LINK_RATE_HIGH: return RD_DP_LINK_RATE_LOW;
    case RD_DP_LINK_RATE_RATE_3:
    case RD_DP_LINK_RATE_RATE_2: return RD_DP_LINK_RATE_LOW;
    default: return RD_DP_LINK_RATE_UNKNOWN;
    }
}

/* increase_lane_count */
static uint32_t increase_lane_count(uint32_t n)
{
    return n == 1 ? 2 : n == 2 ? 4 : 0;
}

/* increase_link_rate (HBR3 is the top: no UHBR) */
static uint32_t increase_link_rate(uint32_t r)
{
    switch (r) {
    case RD_DP_LINK_RATE_LOW: return RD_DP_LINK_RATE_HIGH;
    case RD_DP_LINK_RATE_HIGH: return RD_DP_LINK_RATE_HIGH2;
    case RD_DP_LINK_RATE_HIGH2: return RD_DP_LINK_RATE_HIGH3;
    default: return RD_DP_LINK_RATE_UNKNOWN;
    }
}

/* the link cap link_decide_link_settings searches: dp_get_verified_link_cap
 * (verified_link_cap); before any training verified one, the max link cap
 * (link_detection.c sets verified_link_cap = dp_get_max_link_cap when it
 * does not verify) */
static const rdna3_dp_link_settings *verified_cap(const rdna3_dp_link *l)
{
    return l->verified ? &l->verified_link_cap : &l->max_link_cap;
}

const rdna3_dp_link_settings *dcn32_dp_link_cap(const rdna3_dp_link *l)
{
    return verified_cap(l);
}

/* decide_dp_link_settings: the smallest setting that carries req_bw */
static int decide_dp_link_settings(const rdna3_dp_link *l, rdna3_dp_link_settings *out, uint32_t req_bw)
{
    const rdna3_dp_link_settings *cap = verified_cap(l);
    rdna3_dp_link_settings cur = { 1, RD_DP_LINK_RATE_LOW, RD_DP_LINK_SPREAD_DISABLED };
    if (req_bw > dcn32_dp_link_bw_kbps(l, cap))
        return 0;
    while (cur.link_rate && cur.link_rate <= cap->link_rate) {
        if (req_bw <= dcn32_dp_link_bw_kbps(l, &cur)) {
            *out = cur;
            return 1;
        }
        if (cur.lane_count < cap->lane_count) {
            cur.lane_count = increase_lane_count(cur.lane_count);
        } else {
            cur.link_rate = increase_link_rate(cur.link_rate);
            cur.lane_count = 1;
        }
    }
    return 0;
}

/* link_decide_link_settings for an SST DP stream (no preferred setting, not eDP) */
int dcn32_dp_decide_link_settings(const rdna3_dp_link *l, uint32_t req_kbps, rdna3_dp_link_settings *out)
{
    out->lane_count = 0;
    out->link_rate = RD_DP_LINK_RATE_UNKNOWN;
    out->link_spread = RD_DP_LINK_SPREAD_DISABLED;
    decide_dp_link_settings(l, out, req_kbps);
    return out->lane_count && out->link_rate ? RDNA3_OK : RDNA3_EINVAL;
}

/* decide_fallback_link_setting (8b/10b: not the max-bandwidth policy) */
int dcn32_dp_fallback_link_setting(const rdna3_dp_link *l, rdna3_dp_link_settings *max,
                                   rdna3_dp_link_settings *cur, int training_result)
{
    (void)l;
    switch (training_result) {
    case RD_LT_CR_FAIL_LANE0:
    case RD_LT_CR_FAIL_LANE1:
    case RD_LT_CR_FAIL_LANE23:
    case RD_LT_LQA_FAIL:
        if (cur->link_rate > RD_DP_LINK_RATE_LOW) {           /* !reached_minimum_link_rate */
            cur->link_rate = reduce_link_rate(cur->link_rate);
        } else if (cur->lane_count > 1) {                      /* !reached_minimum_lane_count */
            cur->link_rate = max->link_rate;
            if (training_result == RD_LT_CR_FAIL_LANE0)
                return 0;
            else if (training_result == RD_LT_CR_FAIL_LANE1)
                cur->lane_count = 1;
            else if (training_result == RD_LT_CR_FAIL_LANE23)
                cur->lane_count = 2;
            else
                cur->lane_count = reduce_lane_count(cur->lane_count);
        } else {
            return 0;
        }
        break;
    case RD_LT_EQ_FAIL_EQ:
    case RD_LT_EQ_FAIL_CR_PARTIAL:
        if (cur->lane_count > 1) {
            cur->lane_count = reduce_lane_count(cur->lane_count);
        } else if (cur->link_rate > RD_DP_LINK_RATE_LOW) {
            cur->link_rate = reduce_link_rate(cur->link_rate);
            /* no later CR_FAIL fallback may go back above this rate */
            max->link_rate = cur->link_rate;
            cur->lane_count = max->lane_count;
        } else {
            return 0;
        }
        break;
    case RD_LT_EQ_FAIL_CR:
        if (cur->link_rate > RD_DP_LINK_RATE_LOW) {
            cur->link_rate = reduce_link_rate(cur->link_rate);
            max->link_rate = cur->link_rate;
            cur->lane_count = max->lane_count;
        } else {
            return 0;
        }
        break;
    default:
        return 0;
    }
    return 1;
}

/* ---- link encoder (dcn10_link_encoder.c, VBIOS through the DMCUB) ---- */

/* link_transmitter_control -> transmitter_control_v1_7 (command_table2.c),
 * sent as DMUB_CMD__VBIOS_DIG1_TRANSMITTER_CONTROL; hpdsel / connector /
 * front-end fields filled as the TMDS paths of this driver fill them */
static int link_transmitter_control(rdna3_dp_link *l, uint8_t action, uint8_t mode_laneset, uint32_t lanes,
                                    uint32_t pixel_clock_khz, int with_fe)
{
    rdna3_dig_tx_v1_7 t = { 0 };
    int r;
    if (!l->dm || !l->dm->hw_init)
        return RDNA3_ENODEV;
    t.phyid = (uint8_t)l->be;
    t.action = action;
    t.digmode = mode_laneset;   /* ATOM_TRANSMITTER_DIGMODE_V6_DP = 0, or the DP lane set */
    t.lanenum = (uint8_t)lanes;
    t.hpdsel = (uint8_t)(l->hpd >= 0 ? l->hpd + 1 : 0);
    t.connobj_id = l->connector_id;
    if (with_fe)
        t.digfe_sel = (uint8_t)(1u << l->fe);
    t.symclk_10khz = pixel_clock_khz / 10;
    r = dmub32_transmitter_control(l->dm, &t);
    if (r)
        RERR(l->d, "dp: TransmitterControl action %u on UNIPHY %u failed (%d)", action, l->be, r);
    return r;
}

/* enc1_configure_encoder */
static void enc1_configure_encoder(rdna3_dp_link *l, const rdna3_dp_link_settings *s)
{
    WI(l->d, regDP0_DP_CONFIG, l->be, DP_S,
       SET(0, DP0_DP_CONFIG, DP_UDI_LANES, s->lane_count - 1));
    UPDATE(l->d, regDP0_DP_DPHY_SCRAM_CNTL, l->be, DP_S, DP0_DP_DPHY_SCRAM_CNTL, DPHY_SCRAMBLER_ADVANCE, 1);
}

/* dcn10_link_encoder_enable_dp_output (dcn32_link_encoder_enable_dp_output
 * without avoid_vbios_exec_table) */
static void dcn10_link_encoder_enable_dp_output(rdna3_dp_link *l, const rdna3_dp_link_settings *s)
{
    enc1_configure_encoder(l, s);
    link_transmitter_control(l, RD_ATOM_TX_ENABLE, 0, s->lane_count,
                             s->link_rate * RD_DP_LINK_RATE_REF_FREQ_IN_KHZ, 1);
}

/* dcn10_link_encoder_enable_dp_mst_output: TransmitterControl ENABLE with
 * ATOM_TRANSMITTER_DIGMODE_V6_DP_MST, no engine (dig_encoder_sel_to_atom
 * gives 0: the DIG FE is connected by connect_dig_be_to_fe) */
static void dcn10_link_encoder_enable_dp_mst_output(rdna3_dp_link *l, const rdna3_dp_link_settings *s)
{
    enc1_configure_encoder(l, s);
    link_transmitter_control(l, RD_ATOM_TX_ENABLE, 5, s->lane_count,
                             s->link_rate * RD_DP_LINK_RATE_REF_FREQ_IN_KHZ, 0);
}

/* setup_panel_mode (DP_PANEL_MODE_DEFAULT) */
static void setup_panel_mode(rdna3_dp_link *l)
{
    WI(l->d, regDP0_DP_DPHY_INTERNAL_CTRL, l->be, DP_S, 0);
}

/* link_encoder_disable */
static void link_encoder_disable(rdna3_dp_link *l)
{
    WI(l->d, regDP0_DP_DPHY_TRAINING_PATTERN_SEL, l->be, DP_S, 0);
    UPDATE(l->d, regDP0_DP_LINK_CNTL, l->be, DP_S, DP0_DP_LINK_CNTL, DP_LINK_TRAINING_COMPLETE, 0);
    setup_panel_mode(l);
}

/* dcn10_is_dig_enabled */
static int dcn10_is_dig_enabled(rdna3_dp_link *l)
{
    return GET(RI(l->d, regDIG0_DIG_BE_EN_CNTL, l->be, DIG_S), DIG0_DIG_BE_EN_CNTL, DIG_ENABLE) != 0;
}

/* dcn10_link_encoder_disable_output */
static void dcn10_link_encoder_disable_output(rdna3_dp_link *l)
{
    if (!dcn10_is_dig_enabled(l))
        return;   /* OF_SKIP_POWER_DOWN_INACTIVE_ENCODER */
    if (link_transmitter_control(l, RD_ATOM_TX_DISABLE, 0, 0, 0, 1))
        return;
    link_encoder_disable(l);
}

/* dcn10_link_encoder_dp_set_lane_settings: one SETUP_VSEMPH per lane;
 * transmitter_control_v1_7 carries the DPCD lane set in the mode field
 * (it has no lane select: the lane loop is DC's) */
static void dcn10_link_encoder_dp_set_lane_settings(rdna3_dp_link *l, const rdna3_dp_link_settings *s,
                                                    const rdna3_dp_lane_settings *lanes)
{
    uint32_t lane;
    for (lane = 0; lane < s->lane_count; lane++) {
        uint8_t set = (uint8_t)((lanes[lane].voltage_swing & 3) | ((lanes[lane].pre_emphasis & 3) << 3));
        /* post cursor 2 only at HBR2 (POST_CURSOR2_SET, bits 6-7) */
        if (s->link_rate == RD_DP_LINK_RATE_HIGH2)
            set = (uint8_t)(set | ((lanes[lane].post_cursor2 & 3) << 6));
        link_transmitter_control(l, RD_ATOM_TX_SETUP_VSEMPH, set, s->lane_count,
                                 s->link_rate * RD_DP_LINK_RATE_REF_FREQ_IN_KHZ, 0);
    }
}

/* set_link_training_complete */
static void set_link_training_complete(rdna3_dp_link *l, int complete)
{
    UPDATE(l->d, regDP0_DP_LINK_CNTL, l->be, DP_S, DP0_DP_LINK_CNTL, DP_LINK_TRAINING_COMPLETE, complete ? 1 : 0);
}

/* enable_phy_bypass_mode */
static void enable_phy_bypass_mode(rdna3_dp_link *l, int enable)
{
    UPDATE(l->d, regDP0_DP_DPHY_CNTL, l->be, DP_S, DP0_DP_DPHY_CNTL, DPHY_BYPASS, enable ? 1 : 0);
}

/* disable_prbs_mode */
static void disable_prbs_mode(rdna3_dp_link *l)
{
    UPDATE(l->d, regDP0_DP_DPHY_PRBS_CNTL, l->be, DP_S, DP0_DP_DPHY_PRBS_CNTL, DPHY_PRBS_EN, 0);
}

/* dcn10_link_encoder_set_dp_phy_pattern_training_pattern */
static void dcn10_link_encoder_set_dp_phy_pattern_training_pattern(rdna3_dp_link *l, uint32_t index)
{
    WI(l->d, regDP0_DP_DPHY_TRAINING_PATTERN_SEL, l->be, DP_S, index);
    set_link_training_complete(l, 0);
    enable_phy_bypass_mode(l, 0);
    disable_prbs_mode(l);
}

/* set_dp_phy_pattern_passthrough_mode (DP_PANEL_MODE_DEFAULT) */
static void set_dp_phy_pattern_passthrough_mode(rdna3_dp_link *l)
{
    uint32_t v;
    setup_panel_mode(l);
    /* restore LINK_FRAMING_CNTL and DPHY_SCRAMBLER_BS_COUNT */
    v = RI(l->d, regDP0_DP_LINK_FRAMING_CNTL, l->be, DP_S);
    v = SET(v, DP0_DP_LINK_FRAMING_CNTL, DP_IDLE_BS_INTERVAL, 0x2000);
    v = SET(v, DP0_DP_LINK_FRAMING_CNTL, DP_VBID_DISABLE, 0);
    v = SET(v, DP0_DP_LINK_FRAMING_CNTL, DP_VID_ENHANCED_FRAME_MODE, 1);
    WI(l->d, regDP0_DP_LINK_FRAMING_CNTL, l->be, DP_S, v);
    UPDATE(l->d, regDP0_DP_DPHY_SCRAM_CNTL, l->be, DP_S, DP0_DP_DPHY_SCRAM_CNTL, DPHY_SCRAMBLER_BS_COUNT, 0x1ff);
    set_link_training_complete(l, 1);
    enable_phy_bypass_mode(l, 0);
    disable_prbs_mode(l);
}

/* dcn10_link_encoder_dp_set_phy_pattern: training patterns and video mode
 * (the test patterns of compliance are not ported). 'pattern' is
 * RD_DP_TRAINING_PATTERN_SEQUENCE_n, or RD_DP_TRAINING_PATTERN_VIDEOIDLE
 * for DP_TEST_PATTERN_VIDEO_MODE. */
static void dcn10_link_encoder_dp_set_phy_pattern(rdna3_dp_link *l, uint32_t pattern)
{
    if (pattern <= RD_DP_TRAINING_PATTERN_SEQUENCE_4)
        dcn10_link_encoder_set_dp_phy_pattern_training_pattern(l, pattern);
    else
        set_dp_phy_pattern_passthrough_mode(l);
}

/* dcn10_link_encoder_setup: DIG_MODE 0 = DP SST, 5 = DP MST */
static void dcn10_link_encoder_setup(rdna3_dp_link *l)
{
    UPDATE(l->d, regDIG0_DIG_BE_CNTL, l->be, DIG_S, DIG0_DIG_BE_CNTL, DIG_MODE, l->mst ? 5 : 0);
}

/* enc2_fec_set_enable */
static void enc2_fec_set_enable(rdna3_dp_link *l, int enable)
{
    RDBG(l->d, "dp: %s FEC at link encoder %u", enable ? "enabling" : "disabling", l->be);
    UPDATE(l->d, regDP0_DP_DPHY_CNTL, l->be, DP_S, DP0_DP_DPHY_CNTL, DPHY_FEC_EN, enable ? 1 : 0);
}

/* enc2_fec_set_ready */
static void enc2_fec_set_ready(rdna3_dp_link *l, int ready)
{
    UPDATE(l->d, regDP0_DP_DPHY_CNTL, l->be, DP_S, DP0_DP_DPHY_CNTL, DPHY_FEC_READY_SHADOW, ready ? 1 : 0);
}

/* enc2_fec_is_active */
static int enc2_fec_is_active(rdna3_dp_link *l)
{
    return GET(RI(l->d, regDP0_DP_DPHY_CNTL, l->be, DP_S), DP0_DP_DPHY_CNTL, DPHY_FEC_ACTIVE_STATUS) != 0;
}

/* dp_set_fec_ready: FEC is set ready before the training (the policy:
 * always train with FEC when the sink supports it) */
static int dp_set_fec_ready(rdna3_dp_link *l, int ready)
{
    uint8_t fec_config;
    int status = RDNA3_OK;
    if (ready && dcn32_dp_should_enable_fec(l)) {
        fec_config = RD_DP_FEC_READY;
        status = core_link_write_dpcd(l, RD_DPCD_FEC_CONFIGURATION, &fec_config, 1);
        if (status == RDNA3_OK) {
            enc2_fec_set_ready(l, 1);
            l->fec_state = RD_FEC_READY;
        }
    } else if (l->fec_state == RD_FEC_READY) {
        fec_config = 0;
        core_link_write_dpcd(l, RD_DPCD_FEC_CONFIGURATION, &fec_config, 1);
        enc2_fec_set_ready(l, 0);
        l->fec_state = RD_FEC_NOT_READY;
    }
    return status;
}

/* dp_set_fec_enable: FEC may start 1000 LL codes after the training (6.2 us
 * on one RBR lane: 7 us) */
static void dp_set_fec_enable(rdna3_dp_link *l, int enable)
{
    if (enable && dcn32_dp_should_enable_fec(l)) {
        if (l->fec_state == RD_FEC_READY) {
            l->d->os.udelay(l->d->os.ctx, 7);
            enc2_fec_set_enable(l, 1);
            l->fec_state = RD_FEC_ENABLED;
            RLOG(l->d, "dp: UNIPHY %u: FEC enabled (active %d)", l->be, enc2_fec_is_active(l));
        }
    } else if (l->fec_state == RD_FEC_ENABLED) {
        enc2_fec_set_enable(l, 0);
        l->fec_state = RD_FEC_READY;
    }
}

/* dcn10_link_encoder_connect_dig_be_to_fe; get_frontend_source: DIG n is
 * bit n of DIG_FE_SOURCE_SELECT (DCN10_DIG_FE_SOURCE_SELECT_DIGA..) */
static void dcn10_link_encoder_connect_dig_be_to_fe(rdna3_dp_link *l, int connect)
{
    uint32_t v = RI(l->d, regDIG0_DIG_BE_CNTL, l->be, DIG_S);
    uint32_t field = GET(v, DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT);
    if (connect)
        field |= 1u << l->fe;
    else
        field &= ~(1u << l->fe);
    WI(l->d, regDIG0_DIG_BE_CNTL, l->be, DIG_S, SET(v, DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT, field));
}

/* ---- stream encoder (dcn10 / dcn20 / dcn32 stream encoders) ---- */

/* enc32_set_dig_input_mode */
static void enc32_set_dig_input_mode(rdna3_dp_link *l, uint32_t pix_per_container)
{
    UPDATE(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S, DIG0_DIG_FIFO_CTRL0, DIG_FIFO_OUTPUT_PIXEL_MODE,
           pix_per_container == 2 ? 1 : 0);
}

/* enc32_reset_fifo */
static void enc32_reset_fifo(rdna3_dp_link *l, int reset)
{
    uint32_t val = reset ? 1 : 0;
    UPDATE(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S, DIG0_DIG_FIFO_CTRL0, DIG_FIFO_RESET, val);
    if (GET(RI(l->d, regDIG0_DIG_FE_CNTL, l->fe, DIG_S), DIG0_DIG_FE_CNTL, DIG_SYMCLK_FE_ON))
        rdna3_wait_reg(l->d, AI(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S),
                       val << DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE__SHIFT,
                       DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE_MASK, 0, 50000);
    else
        l->d->os.udelay(l->d->os.ctx, 10);
}

/* enc32_enable_fifo */
static void enc32_enable_fifo(rdna3_dp_link *l)
{
    UPDATE(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S, DIG0_DIG_FIFO_CTRL0, DIG_FIFO_READ_START_LEVEL, 0x7);
    enc32_reset_fifo(l, 1);
    enc32_reset_fifo(l, 0);
    UPDATE(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S, DIG0_DIG_FIFO_CTRL0, DIG_FIFO_ENABLE, 1);
}

/* setup_dio_stream_encoder (DCN 3.2 stream encoders have no enable_stream /
 * map_stream_to_link) */
static void setup_dio_stream_encoder(rdna3_dp_link *l)
{
    dcn10_link_encoder_connect_dig_be_to_fe(l, 1);
    enc32_set_dig_input_mode(l, l->pix_per_cycle);
    enc32_enable_fifo(l);
}

/* reset_dio_stream_encoder (no disable_fifo on DCN 3.2) */
static void reset_dio_stream_encoder(rdna3_dp_link *l)
{
    enc32_set_dig_input_mode(l, 0);
    dcn10_link_encoder_connect_dig_be_to_fe(l, 0);
}

/* enc1_stream_encoder_dp_set_stream_attribute: RGB 4:4:4, 8 bpc, sRGB,
 * no VSC SDP, no borders, progressive */
static void enc1_stream_encoder_dp_set_stream_attribute(rdna3_dp_link *l, const rdna3_mode *m)
{
    uint32_t misc0, misc1, h_blank, h_back_porch, h_active_start, v_active_start, v;
    /* DP_PIXEL_ENCODING_TYPE_RGB444 = 0, DP_COMPONENT_PIXEL_DEPTH_8BPC = 1 */
    v = RI(l->d, regDP0_DP_PIXEL_FORMAT, l->fe, DP_S);
    v = SET(v, DP0_DP_PIXEL_FORMAT, DP_PIXEL_ENCODING, 0);
    v = SET(v, DP0_DP_PIXEL_FORMAT, DP_COMPONENT_DEPTH, 1);
    WI(l->d, regDP0_DP_PIXEL_FORMAT, l->fe, DP_S, v);
    misc1 = RI(l->d, regDP0_DP_MSA_MISC, l->fe, DP_S);
    misc1 &= ~0x40u;               /* no VSC SDP for colorimetry */
    misc0 = 1u << 5;               /* colorimetry_bpc 8 bpc; asynchronous clock */
    misc1 &= ~0x80u;               /* COLOR_SPACE_SRGB */
    WI(l->d, regDP0_DP_MSA_COLORIMETRY, l->fe, DP_S, SET(0, DP0_DP_MSA_COLORIMETRY, DP_MSA_MISC0, misc0));
    WI(l->d, regDP0_DP_MSA_MISC, l->fe, DP_S, misc1);
    WI(l->d, regDP0_DP_MSA_TIMING_PARAM1, l->fe, DP_S,
       SET(SET(0, DP0_DP_MSA_TIMING_PARAM1, DP_MSA_HTOTAL, m->h_total), DP0_DP_MSA_TIMING_PARAM1, DP_MSA_VTOTAL,
           m->v_total));
    h_blank = m->h_total - m->h_active;
    h_back_porch = h_blank - m->h_front - m->h_sync;
    h_active_start = m->h_sync + h_back_porch;
    v_active_start = m->v_total - m->v_active - m->v_front;
    WI(l->d, regDP0_DP_MSA_TIMING_PARAM2, l->fe, DP_S,
       SET(SET(0, DP0_DP_MSA_TIMING_PARAM2, DP_MSA_HSTART, h_active_start), DP0_DP_MSA_TIMING_PARAM2,
           DP_MSA_VSTART, v_active_start));
    v = SET(0, DP0_DP_MSA_TIMING_PARAM3, DP_MSA_HSYNCWIDTH, m->h_sync);
    v = SET(v, DP0_DP_MSA_TIMING_PARAM3, DP_MSA_HSYNCPOLARITY, m->h_sync_pos ? 0 : 1);
    v = SET(v, DP0_DP_MSA_TIMING_PARAM3, DP_MSA_VSYNCWIDTH, m->v_sync);
    v = SET(v, DP0_DP_MSA_TIMING_PARAM3, DP_MSA_VSYNCPOLARITY, m->v_sync_pos ? 0 : 1);
    WI(l->d, regDP0_DP_MSA_TIMING_PARAM3, l->fe, DP_S, v);
    WI(l->d, regDP0_DP_MSA_TIMING_PARAM4, l->fe, DP_S,
       SET(SET(0, DP0_DP_MSA_TIMING_PARAM4, DP_MSA_HWIDTH, m->h_active), DP0_DP_MSA_TIMING_PARAM4, DP_MSA_VHEIGHT,
           m->v_active));
}

/* enc2_stream_encoder_dp_set_stream_attribute (no SDP splitting) */
static void enc2_stream_encoder_dp_set_stream_attribute(rdna3_dp_link *l, const rdna3_mode *m)
{
    enc1_stream_encoder_dp_set_stream_attribute(l, m);
    UPDATE(l->d, regDP0_DP_SEC_FRAMING4, l->fe, DP_S, DP0_DP_SEC_FRAMING4, DP_SST_SDP_SPLITTING, 0);
}

/* enc1_stream_encoder_dp_blank */
static void enc1_stream_encoder_dp_blank(rdna3_dp_link *l)
{
    uint32_t v = RI(l->d, regDP0_DP_VID_STREAM_CNTL, l->fe, DP_S);
    if (!GET(v, DP0_DP_VID_STREAM_CNTL, DP_VID_STREAM_ENABLE))
        return;   /* stream not enabled */
    /* disable at the start of the next vertical blank */
    v = SET(v, DP0_DP_VID_STREAM_CNTL, DP_VID_STREAM_DIS_DEFER, 2);
    WI(l->d, regDP0_DP_VID_STREAM_CNTL, l->fe, DP_S, v);
    UPDATE(l->d, regDP0_DP_VID_STREAM_CNTL, l->fe, DP_S, DP0_DP_VID_STREAM_CNTL, DP_VID_STREAM_ENABLE, 0);
    /* up to 102 ms: one frame of a 10 Hz mode */
    if (rdna3_wait_reg(l->d, AI(l->d, regDP0_DP_VID_STREAM_CNTL, l->fe, DP_S), 0,
                       DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK, 0, 102000))
        RERR(l->d, "dp: DIG %u: video stream status stays on after the blank", l->fe);
    /* ignore the OTG's timing only after the poll */
    UPDATE(l->d, regDP0_DP_STEER_FIFO, l->fe, DP_S, DP0_DP_STEER_FIFO, DP_STEER_FIFO_RESET, 1);
}

/* enc32_dp_set_odm_combine */
static void enc32_dp_set_odm_combine(rdna3_dp_link *l, int two_pixel_per_cycle)
{
    UPDATE(l->d, regDP0_DP_PIXEL_FORMAT, l->fe, DP_S, DP0_DP_PIXEL_FORMAT, DP_PIXEL_PER_CYCLE_PROCESSING_MODE,
           two_pixel_per_cycle ? 1 : 0);
}

/* enc32_stream_encoder_dp_unblank */
static void enc32_stream_encoder_dp_unblank(rdna3_dp_link *l, uint32_t pixel_khz, uint32_t link_rate,
                                            uint32_t pix_per_cycle)
{
    if (link_rate != RD_DP_LINK_RATE_UNKNOWN) {
        uint32_t n_vid = 0x8000, m_vid, n_multiply = 0, ppc = 0, v;
        uint64_t m_vid_l = n_vid;
        if (pix_per_cycle > 1) {
            n_multiply = 1;
            ppc = 1;
        }
        /* M / N = Fstream / Flink */
        m_vid_l *= pixel_khz;
        m_vid_l /= (uint64_t)link_rate * RD_DP_LINK_RATE_REF_FREQ_IN_KHZ;
        m_vid = (uint32_t)m_vid_l;
        UPDATE(l->d, regDP0_DP_VID_TIMING, l->fe, DP_S, DP0_DP_VID_TIMING, DP_VID_M_N_GEN_EN, 0);
        /* auto measurement needs one 0x8000 symbol cycle: initial values */
        UPDATE(l->d, regDP0_DP_VID_N, l->fe, DP_S, DP0_DP_VID_N, DP_VID_N, n_vid);
        UPDATE(l->d, regDP0_DP_VID_M, l->fe, DP_S, DP0_DP_VID_M, DP_VID_M, m_vid);
        v = RI(l->d, regDP0_DP_VID_TIMING, l->fe, DP_S);
        v = SET(v, DP0_DP_VID_TIMING, DP_VID_M_N_GEN_EN, 1);
        v = SET(v, DP0_DP_VID_TIMING, DP_VID_N_MUL, n_multiply);
        WI(l->d, regDP0_DP_VID_TIMING, l->fe, DP_S, v);
        UPDATE(l->d, regDP0_DP_PIXEL_FORMAT, l->fe, DP_S, DP0_DP_PIXEL_FORMAT, DP_PIXEL_PER_CYCLE_PROCESSING_MODE, ppc);
    }
    /* the stream off before the steer FIFO reset */
    UPDATE(l->d, regDP0_DP_VID_STREAM_CNTL, l->fe, DP_S, DP0_DP_VID_STREAM_CNTL, DP_VID_STREAM_ENABLE, 0);
    rdna3_wait_reg(l->d, AI(l->d, regDP0_DP_VID_STREAM_CNTL, l->fe, DP_S), 0,
                   DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK, 0, 50000);
    /* switch the encoder to the OTG's data, the FIFO reset first */
    UPDATE(l->d, regDP0_DP_STEER_FIFO, l->fe, DP_S, DP0_DP_STEER_FIFO, DP_STEER_FIFO_RESET, 1);
    l->d->os.udelay(l->d->os.ctx, 10);
    UPDATE(l->d, regDP0_DP_STEER_FIFO, l->fe, DP_S, DP0_DP_STEER_FIFO, DP_STEER_FIFO_RESET, 0);
    if (rdna3_wait_reg(l->d, AI(l->d, regDIG0_DIG_FE_CNTL, l->fe, DIG_S), DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK,
                       DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK, 0, 50000))
        RERR(l->d, "dp: DIG %u: SYMCLK_FE_ON stays off", l->fe);
    /* read start level 1/2 full before the reset */
    UPDATE(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S, DIG0_DIG_FIFO_CTRL0, DIG_FIFO_READ_START_LEVEL, 0x7);
    UPDATE(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S, DIG0_DIG_FIFO_CTRL0, DIG_FIFO_RESET, 1);
    rdna3_wait_reg(l->d, AI(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S), DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE_MASK,
                   DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE_MASK, 0, 50000);
    UPDATE(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S, DIG0_DIG_FIFO_CTRL0, DIG_FIFO_RESET, 0);
    rdna3_wait_reg(l->d, AI(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S), 0,
                   DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE_MASK, 0, 50000);
    UPDATE(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S, DIG0_DIG_FIFO_CTRL0, DIG_FIFO_ENABLE, 1);
    /* 100 us for the DIG / DP logic to prime */
    l->d->os.udelay(l->d->os.ctx, 100);
    /* video starts at the next DP frame */
    UPDATE(l->d, regDP0_DP_VID_STREAM_CNTL, l->fe, DP_S, DP0_DP_VID_STREAM_CNTL, DP_VID_STREAM_ENABLE, 1);
}

/* ---- DP secondary data packets (dcn10 / dcn30 stream encoder) ---- */

/* enc1_stream_encoder_stop_dp_info_packets: every GSP, the MPG and the
 * master enable off (REG_SET of 0); the master back on when something
 * else in the register (the audio packets) still runs */
static void enc1_stream_encoder_stop_dp_info_packets(rdna3_dp_link *l)
{
    uint32_t v;
    WI(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S, 0);
    v = RI(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S);
    if (v)
        UPDATE(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S, DP0_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE, 1);
}

void dcn32_enc_update_dp_info_packets_sdp_line_num(rdna3_dp_link *l, const rdna3_dp_info_frames *f)
{
    if (f->adaptive_sync.valid && f->adaptive_sync_line_num_valid) {
        /* 00: REFER_TO_DP_SOF, 01: REFER_TO_OTG_SOF */
        UPDATE(l->d, regDP0_DP_SEC_CNTL1, l->fe, DP_S, DP0_DP_SEC_CNTL1, DP_SEC_GSP5_LINE_REFERENCE, 1);
        UPDATE(l->d, regDP0_DP_SEC_CNTL5, l->fe, DP_S, DP0_DP_SEC_CNTL5, DP_SEC_GSP5_LINE_NUM,
               f->adaptive_sync_line_num);
    }
}

static void vpg_packet(rdna3_dp_link *l, uint32_t index, const rdna3_info_packet *p)
{
    uint8_t hb[4];
    hb[0] = p->hb0;
    hb[1] = p->hb1;
    hb[2] = p->hb2;
    hb[3] = p->hb3;
    dcn32_vpg_update_generic_info_packet(l->d, l->fe, index, hb, p->sb, 1);
}

void dcn32_enc_update_dp_info_packets(rdna3_dp_link *l, const rdna3_dp_info_frames *f)
{
    uint32_t v;
    /* VSC in packets 0 and 1 (DC writes packet 1 twice: PSR-SU's TODO) */
    if (f->vsc.valid)
        vpg_packet(l, 0, &f->vsc);
    if (f->vsc.valid)
        vpg_packet(l, 1, &f->vsc);
    if (f->vsc.valid)
        vpg_packet(l, 1, &f->vsc);
    if (f->spd.valid)
        vpg_packet(l, 2, &f->spd);
    if (f->hdrsmd.valid)
        vpg_packet(l, 3, &f->hdrsmd);
    /* packet 4 is for immediate SDP messages */
    if (f->adaptive_sync.valid)
        vpg_packet(l, 5, &f->adaptive_sync);
    /* transmission from the next frame */
    UPDATE(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S, DP0_DP_SEC_CNTL, DP_SEC_GSP0_ENABLE, f->vsc.valid ? 1 : 0);
    UPDATE(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S, DP0_DP_SEC_CNTL, DP_SEC_GSP2_ENABLE, f->spd.valid ? 1 : 0);
    UPDATE(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S, DP0_DP_SEC_CNTL, DP_SEC_GSP3_ENABLE, f->hdrsmd.valid ? 1 : 0);
    UPDATE(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S, DP0_DP_SEC_CNTL, DP_SEC_GSP5_ENABLE, f->adaptive_sync.valid ? 1 : 0);
    /* the master enable with any packet (shared with the audio info frame) */
    v = RI(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S);
    if (v)
        UPDATE(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S, DP0_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE, 1);
    /* dynamic metadata transmission needs it too */
    if (GET(RI(l->d, regDP0_DP_SEC_METADATA_TRANSMISSION, l->fe, DP_S), DP0_DP_SEC_METADATA_TRANSMISSION,
            DP_SEC_METADATA_PACKET_ENABLE))
        UPDATE(l->d, regDP0_DP_SEC_CNTL, l->fe, DP_S, DP0_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE, 1);
}

int dcn32_enc_dp_get_pixel_format(rdna3_dp_link *l, uint32_t *encoding, uint32_t *depth)
{
    uint32_t v, hw_encoding, hw_depth;
    if (!l || !encoding || !depth)
        return 0;
    v = RI(l->d, regDP0_DP_PIXEL_FORMAT, l->fe, DP_S);
    hw_encoding = GET(v, DP0_DP_PIXEL_FORMAT, DP_PIXEL_ENCODING);
    hw_depth = GET(v, DP0_DP_PIXEL_FORMAT, DP_COMPONENT_DEPTH);
    switch (hw_depth) {
    case 0: *depth = RD_COLOR_DEPTH_666; break;     /* DP_COMPONENT_PIXEL_DEPTH_6BPC */
    case 1: *depth = RD_COLOR_DEPTH_888; break;
    case 2: *depth = RD_COLOR_DEPTH_101010; break;
    case 3: *depth = RD_COLOR_DEPTH_121212; break;
    case 4: *depth = RD_COLOR_DEPTH_161616; break;
    default: *depth = RD_COLOR_DEPTH_UNDEFINED; break;
    }
    switch (hw_encoding) {
    case 0: *encoding = RD_PIXEL_ENCODING_RGB; break;        /* DP_PIXEL_ENCODING_TYPE_RGB444 */
    case 1: *encoding = RD_PIXEL_ENCODING_YCBCR422; break;
    case 2: case 3: *encoding = RD_PIXEL_ENCODING_YCBCR444; break;   /* YCbCr 4:4:4, Y only */
    case 4: *encoding = RD_PIXEL_ENCODING_YCBCR420; break;
    default: *encoding = 0; break;                               /* PIXEL_ENCODING_UNDEFINED */
    }
    return 1;
}

/* enable_stream_features (SST): DP_DOWNSPREAD_CTRL's MSA_TIMING_PAR_IGNORE
 * as the stream wants it (ignore_msa_timing_param: FreeSync only, so off);
 * MST: dm_helpers_mst_enable_stream_features, the same on the link's DPCD
 * (amdgpu's TODO: no virtual DPCD) */
static void enable_stream_features(rdna3_dp_link *l)
{
    uint8_t old_downspread = 0, new_downspread;
    if (core_link_read_dpcd(l, RD_DPCD_DOWNSPREAD_CTRL, &old_downspread, 1) != RDNA3_OK && l->mst)
        return;
    new_downspread = (uint8_t)((old_downspread & ~0x80u) | (l->ignore_msa_timing_param ? 0x80u : 0));
    if (new_downspread != old_downspread)
        core_link_write_dpcd(l, RD_DPCD_DOWNSPREAD_CTRL, &new_downspread, 1);
}

/* dcn20_blank_stream (DP 8b/10b) */
static void dcn20_blank_stream(rdna3_dp_link *l)
{
    enc1_stream_encoder_dp_blank(l);
}

/* dcn32_unblank_stream: no ODM, one pipe; pix_per_cycle is the stream
 * encoder's (dio_se_pix_per_cycle) */
static void dcn32_unblank_stream(rdna3_dp_link *l, uint32_t pixel_khz, uint32_t link_rate)
{
    enc32_dp_set_odm_combine(l, l->pix_per_cycle > 1);
    enc32_stream_encoder_dp_unblank(l, pixel_khz, link_rate, l->pix_per_cycle);
}

/* ---- clock source (dce_clock_source.c) ---- */

/* video_optimized_pixel_rates */
static const struct { uint32_t min_khz, max_khz, target_khz, mult, div; } video_optimized_pixel_rates[] = {
    /* /1.001 rates */
    { 25170, 25180, 25200, 1000, 1001 },     { 59340, 59350, 59400, 1000, 1001 },
    { 74170, 74180, 74250, 1000, 1001 },     { 89910, 90000, 90000, 1000, 1001 },
    { 125870, 125880, 126000, 1000, 1001 },  { 148350, 148360, 148500, 1000, 1001 },
    { 167830, 167840, 168000, 1000, 1001 },  { 222520, 222530, 222750, 1000, 1001 },
    { 257140, 257150, 257400, 1000, 1001 },  { 296700, 296710, 297000, 1000, 1001 },
    { 342850, 342860, 343200, 1000, 1001 },  { 395600, 395610, 396000, 1000, 1001 },
    { 409090, 409100, 409500, 1000, 1001 },  { 445050, 445060, 445500, 1000, 1001 },
    { 467530, 467540, 468000, 1000, 1001 },  { 519230, 519240, 519750, 1000, 1001 },
    { 525970, 525980, 526500, 1000, 1001 },  { 545450, 545460, 546000, 1000, 1001 },
    { 593400, 593410, 594000, 1000, 1001 },  { 623370, 623380, 624000, 1000, 1001 },
    { 692300, 692310, 693000, 1000, 1001 },  { 701290, 701300, 702000, 1000, 1001 },
    { 791200, 791210, 792000, 1000, 1001 },  { 890100, 890110, 891000, 1000, 1001 },
    { 1186810, 1186820, 1188000, 1000, 1001 },
    /* x1.001 rates */
    { 27020, 27030, 27000, 1001, 1000 },     { 54050, 54060, 54000, 1001, 1000 },
    { 108100, 108110, 108000, 1001, 1000 },
};

/* dcn31_program_pix_clk, the DP signal branch: the DP DTO without the
 * VBIOS (phase = target clock, modulo = DPREFCLK), PIPE_DTO_SRC_SEL 1
 * (8b/10b). look_up_in_video_optimized_rate_tlb keeps /1.001 rates exact. */
void dcn32_dp_program_pix_clk(rdna3_dp_link *l, uint32_t pixel_khz)
{
    uint32_t i, v, ref = l->dprefclk_khz;
    int found = 0;
    for (i = 0; i < sizeof(video_optimized_pixel_rates) / sizeof(video_optimized_pixel_rates[0]); i++) {
        if (video_optimized_pixel_rates[i].min_khz <= pixel_khz && pixel_khz <= video_optimized_pixel_rates[i].max_khz) {
            WI(l->d, regDP_DTO0_PHASE, l->otg, DTO_S,
               video_optimized_pixel_rates[i].target_khz * video_optimized_pixel_rates[i].mult);
            WI(l->d, regDP_DTO0_MODULO, l->otg, DTO_S, ref * video_optimized_pixel_rates[i].div);
            found = 1;
            break;
        }
    }
    if (!found) {
        WI(l->d, regDP_DTO0_PHASE, l->otg, DTO_S, pixel_khz * 1000);
        WI(l->d, regDP_DTO0_MODULO, l->otg, DTO_S, ref * 1000);
    }
    v = RI(l->d, regOTG0_PIXEL_RATE_CNTL, l->otg, DTO_S);
    v = SET(v, OTG0_PIXEL_RATE_CNTL, DP_DTO0_ENABLE, 1);
    v = SET(v, OTG0_PIXEL_RATE_CNTL, PIPE0_DTO_SRC_SEL, 1);
    WI(l->d, regOTG0_PIXEL_RATE_CNTL, l->otg, DTO_S, v);
}

/* ---- link hwss / link_dp_phy.c ---- */

/* dpcd_write_rx_power_ctrl */
static void dpcd_write_rx_power_ctrl(rdna3_dp_link *l, int on)
{
    dpcd_write1(l, RD_DPCD_SET_POWER, on ? RD_DP_SET_POWER_D0 : RD_DP_SET_POWER_D3);
}

/* dp_enable_link_phy: dce110_enable_dp_link_output -> enable_dio_dp_link_output
 * (the pixel clock is already the DP DTO; no DMCU on DCN 3.2) */
void dcn32_dp_enable_link_phy(rdna3_dp_link *l, const rdna3_dp_link_settings *s)
{
    l->cur_link_settings = *s;
    if (l->mst)
        dcn10_link_encoder_enable_dp_mst_output(l, s);
    else
        dcn10_link_encoder_enable_dp_output(l, s);
    dpcd_write_rx_power_ctrl(l, 1);
}

/* dp_disable_link_phy: dcn32_disable_link_output -> disable_dio_link_output */
void dcn32_dp_disable_link_phy(rdna3_dp_link *l)
{
    uint32_t i;
    if (!l->keep_receiver_powered)
        dpcd_write_rx_power_ctrl(l, 0);

    dcn10_link_encoder_disable_output(l);
    for (i = 0; i < sizeof(l->cur_link_settings); i++)
        ((uint8_t *)&l->cur_link_settings)[i] = 0;
}

/* dp_set_hw_lane_settings -> set_dio_dp_lane_settings (DPRX only: an LTTPR
 * here is transparent) */
static void dp_set_hw_lane_settings(rdna3_dp_link *l, const rdna3_dp_lt_settings *lt)
{
    uint32_t i;
    dcn10_link_encoder_dp_set_lane_settings(l, &lt->link_settings, lt->hw_lane_settings);
    for (i = 0; i < RD_DP_LANE_COUNT_DP_MAX; i++)
        l->cur_lane_setting[i] = lt->hw_lane_settings[i];
}

/* dp_set_hw_test_pattern -> set_dio_dp_link_test_pattern */
static void dp_set_hw_test_pattern(rdna3_dp_link *l, uint32_t pattern)
{
    dcn10_link_encoder_dp_set_phy_pattern(l, pattern);
}

/* dp_set_hw_training_pattern */
static void dp_set_hw_training_pattern(rdna3_dp_link *l, uint32_t pattern)
{
    dp_set_hw_test_pattern(l, pattern);
}

/* ---- training (link_dp_training.c, link_dp_training_8b_10b.c) ---- */

/* dp_training_pattern_to_dpcd_training_pattern */
static uint8_t dp_training_pattern_to_dpcd_training_pattern(uint32_t p)
{
    switch (p) {
    case RD_DP_TRAINING_PATTERN_SEQUENCE_1: return RD_DPCD_TRAINING_PATTERN_1;
    case RD_DP_TRAINING_PATTERN_SEQUENCE_2: return RD_DPCD_TRAINING_PATTERN_2;
    case RD_DP_TRAINING_PATTERN_SEQUENCE_3: return RD_DPCD_TRAINING_PATTERN_3;
    case RD_DP_TRAINING_PATTERN_SEQUENCE_4: return RD_DPCD_TRAINING_PATTERN_4;
    default: return RD_DPCD_TRAINING_PATTERN_VIDEOIDLE;
    }
}

/* dp_initialize_scrambling_data_symbols */
static uint8_t dp_initialize_scrambling_data_symbols(uint32_t p)
{
    return p <= RD_DP_TRAINING_PATTERN_SEQUENCE_3 ? 1 : 0;
}

/* dp_get_nibble_at_index */
static uint8_t dp_get_nibble_at_index(const uint8_t *buf, uint32_t index)
{
    uint8_t nibble = buf[index / 2];
    return (uint8_t)(index % 2 ? nibble >> 4 : nibble & 0x0f);
}

/* dp_wait_for_training_aux_rd_interval */
static void dp_wait_for_training_aux_rd_interval(rdna3_dp_link *l, uint32_t us)
{
    fsleep(l->d, us);
}

/* get_max_pre_emphasis_for_voltage_swing */
static uint8_t get_max_pre_emphasis_for_voltage_swing(uint8_t vs)
{
    static const uint8_t vs_to_pe[] = { 3, 2, 1, 0 };
    return vs <= RD_DP_VOLTAGE_SWING_MAX_LEVEL ? vs_to_pe[vs] : RD_DP_PRE_EMPHASIS_MAX_LEVEL;
}

/* maximize_lane_settings */
static void maximize_lane_settings(const rdna3_dp_lt_settings *lt, rdna3_dp_lane_settings *lanes)
{
    uint32_t lane;
    uint8_t vs = lanes[0].voltage_swing, pe = lanes[0].pre_emphasis;
    for (lane = 1; lane < lt->link_settings.lane_count; lane++) {
        if (lanes[lane].voltage_swing > vs)
            vs = lanes[lane].voltage_swing;
        if (lanes[lane].pre_emphasis > pe)
            pe = lanes[lane].pre_emphasis;
    }
    if (vs > RD_DP_VOLTAGE_SWING_MAX_LEVEL)
        vs = RD_DP_VOLTAGE_SWING_MAX_LEVEL;
    if (pe > RD_DP_PRE_EMPHASIS_MAX_LEVEL)
        pe = RD_DP_PRE_EMPHASIS_MAX_LEVEL;
    if (pe > get_max_pre_emphasis_for_voltage_swing(vs))
        pe = get_max_pre_emphasis_for_voltage_swing(vs);
    for (lane = 0; lane < RD_DP_LANE_COUNT_DP_MAX; lane++) {
        lanes[lane].voltage_swing = vs;
        lanes[lane].pre_emphasis = pe;
    }
}

/* dp_hw_to_dpcd_lane_settings */
static void dp_hw_to_dpcd_lane_settings(const rdna3_dp_lane_settings *hw, uint8_t *dpcd)
{
    uint32_t lane;
    for (lane = 0; lane < RD_DP_LANE_COUNT_DP_MAX; lane++)
        dpcd[lane] = (uint8_t)((hw[lane].voltage_swing & 3) |
                               (hw[lane].voltage_swing == RD_DP_VOLTAGE_SWING_MAX_LEVEL ? 1u << 2 : 0) |
                               ((hw[lane].pre_emphasis & 3) << 3) |
                               (hw[lane].pre_emphasis == RD_DP_PRE_EMPHASIS_MAX_LEVEL ? 1u << 5 : 0));
}

/* dp_get_cr_failure */
static int dp_get_cr_failure(uint32_t ln_count, const uint8_t *st)
{
    if (ln_count >= 1 && !(st[0] & RD_DP_LANE_CR_DONE))
        return RD_LT_CR_FAIL_LANE0;
    if (ln_count >= 2 && !(st[1] & RD_DP_LANE_CR_DONE))
        return RD_LT_CR_FAIL_LANE1;
    if (ln_count >= 4 && (!(st[2] & RD_DP_LANE_CR_DONE) || !(st[3] & RD_DP_LANE_CR_DONE)))
        return RD_LT_CR_FAIL_LANE23;
    return RD_LT_SUCCESS;
}

/* dp_is_max_vs_reached */
static int dp_is_max_vs_reached(const rdna3_dp_lt_settings *lt)
{
    uint32_t lane;
    for (lane = 0; lane < lt->link_settings.lane_count; lane++)
        if (RD_DP_LANE_VS(lt->dpcd_lane_settings[lane]) == RD_DP_VOLTAGE_SWING_MAX_LEVEL)
            return 1;
    return 0;
}

/* dp_is_cr_done, dp_is_ch_eq_done, dp_is_symbol_locked */
static int lanes_have(uint32_t ln_count, const uint8_t *st, uint8_t bit)
{
    uint32_t lane;
    for (lane = 0; lane < ln_count; lane++)
        if (!(st[lane] & bit))
            return 0;
    return 1;
}
static int dp_is_cr_done(uint32_t n, const uint8_t *st) { return lanes_have(n, st, RD_DP_LANE_CR_DONE); }
static int dp_is_ch_eq_done(uint32_t n, const uint8_t *st) { return lanes_have(n, st, RD_DP_LANE_CHANNEL_EQ_DONE); }
static int dp_is_symbol_locked(uint32_t n, const uint8_t *st) { return lanes_have(n, st, RD_DP_LANE_SYMBOL_LOCKED); }

/* dp_is_interlane_aligned / dp_check_interlane_aligned (no
 * skip_fallback_on_link_loss) */
static int dp_is_interlane_aligned(uint8_t align) { return (align & RD_DP_INTERLANE_ALIGN_DONE) != 0; }

/* dp_get_lane_status_and_lane_adjust (DPRX) */
static int dp_get_lane_status_and_lane_adjust(rdna3_dp_link *l, const rdna3_dp_lt_settings *lt, uint8_t *ln_status,
                                              uint8_t *ln_align, uint8_t *ln_adjust)
{
    uint8_t b[6] = { 0 };
    uint32_t lane;
    int status = core_link_read_dpcd(l, RD_DPCD_LANE0_1_STATUS, b, sizeof(b));
    if (status != RDNA3_OK) {
        RDBG(l->d, "dp: failed to read lane status: keep the current status and adjust");
        return status;
    }
    for (lane = 0; lane < lt->link_settings.lane_count; lane++) {
        ln_status[lane] = dp_get_nibble_at_index(&b[0], lane);
        ln_adjust[lane] = dp_get_nibble_at_index(&b[4], lane);
    }
    *ln_align = b[2];
    RDBG(l->d, "dp: lane status %02x %02x align %02x adjust %02x %02x", b[0], b[1], b[2], b[4], b[5]);
    return RDNA3_OK;
}

/* dp_decide_lane_settings (adjust nibble: VS bits 0-1, PE bits 2-3) */
static void dp_decide_lane_settings(rdna3_dp_lt_settings *lt, const uint8_t *ln_adjust)
{
    uint32_t lane;
    for (lane = 0; lane < RD_DP_LANE_COUNT_DP_MAX; lane++) {
        lt->hw_lane_settings[lane].voltage_swing = (uint8_t)(ln_adjust[lane] & 3);
        lt->hw_lane_settings[lane].pre_emphasis = (uint8_t)((ln_adjust[lane] >> 2) & 3);
    }
    dp_hw_to_dpcd_lane_settings(lt->hw_lane_settings, lt->dpcd_lane_settings);
    if (lt->disallow_per_lane_settings) {
        /* the maximum of the requests on every lane (override_lane_settings: no overrides) */
        maximize_lane_settings(lt, lt->hw_lane_settings);
        if (lt->always_match_dpcd_with_hw_lane_settings)
            dp_hw_to_dpcd_lane_settings(lt->hw_lane_settings, lt->dpcd_lane_settings);
    }
}

/* get_cr_training_aux_rd_interval */
static uint32_t get_cr_training_aux_rd_interval(rdna3_dp_link *l)
{
    uint8_t v = 0;
    uint32_t us = 100;
    if (l->dpcd_caps.dpcd_rev >= 0x12) {
        core_link_read_dpcd(l, RD_DPCD_TRAINING_AUX_RD_INTERVAL, &v, 1);
        if (v & 0x7f)
            us = (v & 0x7fu) * 4000;
    }
    return us;
}

/* get_eq_training_aux_rd_interval (dp_translate_training_aux_read_interval) */
static uint32_t get_eq_training_aux_rd_interval(rdna3_dp_link *l)
{
    uint8_t v = 0;
    if (l->dpcd_caps.dpcd_rev >= 0x12)
        core_link_read_dpcd(l, RD_DPCD_TRAINING_AUX_RD_INTERVAL, &v, 1);
    switch (v & 0x7f) {
    case 1: return 4000;
    case 2: return 8000;
    case 3: return 12000;
    case 4: return 16000;
    case 5: return 32000;
    case 6: return 64000;
    default: return 400;
    }
}

/* decide_cr_training_pattern */
static uint32_t decide_cr_training_pattern(void)
{
    return RD_DP_TRAINING_PATTERN_SEQUENCE_1;
}

/* decide_eq_training_pattern (the DCN 3.2 link encoder is TPS3 and TPS4 capable) */
static uint32_t decide_eq_training_pattern(const rdna3_dp_link *l)
{
    if (l->dpcd_caps.max_down_spread & RD_DP_TPS4_SUPPORTED)
        return RD_DP_TRAINING_PATTERN_SEQUENCE_4;
    if (l->dpcd_caps.max_ln_count & RD_DP_TPS3_SUPPORTED)
        return RD_DP_TRAINING_PATTERN_SEQUENCE_3;
    return RD_DP_TRAINING_PATTERN_SEQUENCE_2;
}

/* dp_decide_8b_10b_lttpr_mode: LTTPR-aware VBIOS, non-transparent not forced;
 * dp_decide_lttpr_mode for the 8b/10b links this port trains */

static int dp_decide_8b_10b_lttpr_mode(const rdna3_dp_link *l)
{
    if (!dp_is_lttpr_present(l))
        return RD_LTTPR_MODE_NON_LTTPR;
    return l->lttpr_aware ? RD_LTTPR_MODE_TRANSPARENT : RD_LTTPR_MODE_NON_LTTPR;
}

/* decide_8b_10b_training_settings (+ override_training_settings without
 * overrides: hw lane settings VS0 / PE0, DPCD lane settings to match) */
static void decide_8b_10b_training_settings(rdna3_dp_link *l, const rdna3_dp_link_settings *s,
                                            rdna3_dp_lt_settings *lt)
{
    uint32_t i;
    for (i = 0; i < sizeof(*lt); i++)
        ((uint8_t *)lt)[i] = 0;
    lt->link_settings.link_rate = s->link_rate;
    lt->link_settings.lane_count = s->lane_count;
    /* dp_ss_off is not set: 0.5 % downspread, 30 kHz */
    lt->link_settings.link_spread = RD_DP_LINK_SPREAD_05_DOWNSPREAD_30KHZ;
    lt->cr_pattern_time = get_cr_training_aux_rd_interval(l);
    lt->eq_pattern_time = get_eq_training_aux_rd_interval(l);
    lt->pattern_for_cr = decide_cr_training_pattern();
    lt->pattern_for_eq = decide_eq_training_pattern(l);
    lt->enhanced_framing = 1;
    lt->should_set_fec_ready = 1;
    lt->disallow_per_lane_settings = 1;
    lt->always_match_dpcd_with_hw_lane_settings = 1;
    lt->lttpr_mode = dp_decide_8b_10b_lttpr_mode(l);
    dp_hw_to_dpcd_lane_settings(lt->hw_lane_settings, lt->dpcd_lane_settings);
}

/* dpcd_set_training_pattern */
static int dpcd_set_training_pattern(rdna3_dp_link *l, uint32_t p)
{
    return dpcd_write1(l, RD_DPCD_TRAINING_PATTERN_SET, dp_training_pattern_to_dpcd_training_pattern(p));
}

/* dpcd_exit_training_mode (8b/10b) */
static void dpcd_exit_training_mode(rdna3_dp_link *l)
{
    dpcd_set_training_pattern(l, RD_DP_TRAINING_PATTERN_VIDEOIDLE);
}

/* dpcd_configure_lttpr_mode (transparent only) */
static void dpcd_configure_lttpr_mode(rdna3_dp_link *l, const rdna3_dp_lt_settings *lt)
{
    if (lt->lttpr_mode == RD_LTTPR_MODE_TRANSPARENT)
        configure_lttpr_mode_transparent(l);
}

/* dpcd_configure_channel_coding: DP_8b_10b_ENCODING = 1 */
static void dpcd_configure_channel_coding(rdna3_dp_link *l)
{
    dpcd_write1(l, RD_DPCD_MAIN_LINK_CHANNEL_CODING_SET, 1);
}

/* dpcd_set_link_settings */
static void dpcd_set_link_settings(rdna3_dp_link *l, const rdna3_dp_lt_settings *lt)
{
    uint8_t lane_count_set = (uint8_t)(lt->link_settings.lane_count & RD_DP_LANE_COUNT_MASK);
    if (lt->enhanced_framing)
        lane_count_set |= RD_DP_ENHANCED_FRAME;
    /* POST_LT_ADJ_REQ_GRANTED when the sink supports it and EQ is below TPS4 */
    if (lt->pattern_for_eq < RD_DP_TRAINING_PATTERN_SEQUENCE_4 &&
        (l->dpcd_caps.max_ln_count & RD_DP_POST_LT_ADJ_REQ))
        lane_count_set |= RD_DP_POST_LT_ADJ_REQ;
    dpcd_write1(l, RD_DPCD_DOWNSPREAD_CTRL, (uint8_t)lt->link_settings.link_spread);
    dpcd_write1(l, RD_DPCD_LANE_COUNT_SET, lane_count_set);
    /* get_dpcd_link_rate: the 8b/10b code itself */
    dpcd_write1(l, RD_DPCD_LINK_BW_SET, (uint8_t)lt->link_settings.link_rate);
    RDBG(l->d, "dp: dpcd_set_link_settings rate %x lanes %u framing %d spread %x", lt->link_settings.link_rate,
         lt->link_settings.lane_count, lt->enhanced_framing, lt->link_settings.link_spread);
}

/* dpcd_set_lane_settings */
static void dpcd_set_lane_settings(rdna3_dp_link *l, const rdna3_dp_lt_settings *lt)
{
    core_link_write_dpcd(l, RD_DPCD_TRAINING_LANE0_SET, lt->dpcd_lane_settings, lt->link_settings.lane_count);
}

/* dpcd_set_lt_pattern_and_lane_settings: pattern + lanes in one burst */
static void dpcd_set_lt_pattern_and_lane_settings(rdna3_dp_link *l, const rdna3_dp_lt_settings *lt, uint32_t p)
{
    uint8_t buf[5] = { 0 };
    uint32_t lane;
    buf[0] = (uint8_t)(dp_training_pattern_to_dpcd_training_pattern(p) |
                       (dp_initialize_scrambling_data_symbols(p) ? RD_DP_SCRAMBLING_DISABLE : 0));
    for (lane = 0; lane < lt->link_settings.lane_count; lane++)
        buf[1 + lane] = lt->dpcd_lane_settings[lane];
    core_link_write_dpcd(l, RD_DPCD_TRAINING_PATTERN_SET, buf, 1 + lt->link_settings.lane_count);
}

/* perform_8b_10b_clock_recovery_sequence (DPRX) */
static int perform_8b_10b_clock_recovery_sequence(rdna3_dp_link *l, rdna3_dp_lt_settings *lt)
{
    uint32_t retries_cr = 0, retry_count = 0, lane_count = lt->link_settings.lane_count;
    uint8_t st[4] = { 0 }, adj[4] = { 0 }, align = 0;

    dp_set_hw_training_pattern(l, lt->pattern_for_cr);
    while (retries_cr < RD_DP_LINK_TRAINING_MAX_RETRY_COUNT && retry_count < RD_DP_LINK_TRAINING_MAX_CR_RETRY) {
        /* 1. lane settings on the PHY */
        dp_set_hw_lane_settings(l, lt);
        /* 2. the receiver's DPCD: a 5-byte burst the first time */
        if (!retry_count)
            dpcd_set_lt_pattern_and_lane_settings(l, lt, lt->pattern_for_cr);
        else
            dpcd_set_lane_settings(l, lt);
        /* 3. wait for the receiver to lock on */
        dp_wait_for_training_aux_rd_interval(l, lt->cr_pattern_time);
        /* 4. lane status and the drive settings the sink asks for */
        dp_get_lane_status_and_lane_adjust(l, lt, st, &align, adj);
        /* 5. CR done */
        if (dp_is_cr_done(lane_count, st)) {
            RDBG(l->d, "dp: clock recovery OK");
            return RD_LT_SUCCESS;
        }
        /* 6. max VS reached */
        if (dp_is_max_vs_reached(lt))
            break;
        /* 7. same VS requested again (settings are equal on all lanes) */
        if (RD_DP_LANE_VS(lt->dpcd_lane_settings[0]) == (adj[0] & 3u))
            retries_cr++;
        else
            retries_cr = 0;
        /* 8. new VS / PE */
        dp_decide_lane_settings(lt, adj);
        retry_count++;
    }
    if (retry_count >= RD_DP_LINK_TRAINING_MAX_CR_RETRY)
        RERR(l->d, "dp: could not get CR after %d tries, possibly a voltage swing issue",
             RD_DP_LINK_TRAINING_MAX_CR_RETRY);
    return dp_get_cr_failure(lane_count, st);
}

/* perform_8b_10b_channel_equalization_sequence (DPRX) */
static int perform_8b_10b_channel_equalization_sequence(rdna3_dp_link *l, rdna3_dp_lt_settings *lt)
{
    uint32_t retries, lane_count = lt->link_settings.lane_count;
    uint8_t st[4] = { 0 }, adj[4] = { 0 }, align = 0;
    uint32_t p = lt->pattern_for_eq;

    dp_set_hw_training_pattern(l, p);
    for (retries = 0; retries <= RD_DP_LINK_TRAINING_MAX_RETRY_COUNT; retries++) {
        dp_set_hw_lane_settings(l, lt);
        if (!retries)
            dpcd_set_lt_pattern_and_lane_settings(l, lt, p);
        else
            dpcd_set_lane_settings(l, lt);
        /* dp_get_eq_aux_rd_interval (DPRX) */
        dp_wait_for_training_aux_rd_interval(l, lt->eq_pattern_time);
        dp_get_lane_status_and_lane_adjust(l, lt, st, &align, adj);
        if (!dp_is_cr_done(lane_count, st))
            return (st[0] & RD_DP_LANE_CR_DONE) ? RD_LT_EQ_FAIL_CR_PARTIAL : RD_LT_EQ_FAIL_CR;
        if (dp_is_ch_eq_done(lane_count, st) && dp_is_symbol_locked(lane_count, st) &&
            dp_is_interlane_aligned(align))
            return RD_LT_SUCCESS;
        dp_decide_lane_settings(lt, adj);
    }
    return RD_LT_EQ_FAIL_EQ;
}

/* dp_perform_8b_10b_link_training (no non-transparent LTTPR hops) */
static int dp_perform_8b_10b_link_training(rdna3_dp_link *l, rdna3_dp_lt_settings *lt)
{
    int status;
    /* 1. link rate, lane count, spread */
    dpcd_set_link_settings(l, lt);
    status = perform_8b_10b_clock_recovery_sequence(l, lt);
    if (status == RD_LT_SUCCESS) {
        status = perform_8b_10b_channel_equalization_sequence(l, lt);
        if (status == RD_LT_SUCCESS)
            RDBG(l->d, "dp: channel EQ done");
    }
    return status;
}

/* dp_check_link_loss_status */
static int dp_check_link_loss_status(rdna3_dp_link *l, uint32_t lane_count)
{
    uint8_t b[6] = { 0 };
    uint32_t lane;
    core_link_read_dpcd(l, RD_DPCD_SINK_COUNT, b, sizeof(b));
    for (lane = 0; lane < lane_count; lane++) {
        uint8_t s = dp_get_nibble_at_index(&b[2], lane);
        if (!(s & RD_DP_LANE_CHANNEL_EQ_DONE) || !(s & RD_DP_LANE_CR_DONE) || !(s & RD_DP_LANE_SYMBOL_LOCKED) ||
            !dp_is_interlane_aligned(b[4]))
            return RD_LT_LINK_LOSS;
    }
    return RD_LT_SUCCESS;
}

int dcn32_dp_link_ok(rdna3_dp_link *l)
{
    return l->cur_link_settings.lane_count &&
           dp_check_link_loss_status(l, l->cur_link_settings.lane_count) == RD_LT_SUCCESS;
}

/* perform_post_lt_adj_req_sequence */
static int perform_post_lt_adj_req_sequence(rdna3_dp_link *l, rdna3_dp_lt_settings *lt)
{
    uint32_t lane_count = lt->link_settings.lane_count, n, t, lane;
    uint8_t st[4] = { 0 }, adj[4] = { 0 }, align = 0;
    int changed = 0;
    for (n = 0; n < 6; n++) {                    /* POST_LT_ADJ_REQ_LIMIT */
        changed = 0;
        for (t = 0; t < 200; t++) {              /* POST_LT_ADJ_REQ_TIMEOUT */
            dp_get_lane_status_and_lane_adjust(l, lt, st, &align, adj);
            if (!(align & RD_DP_POST_LT_ADJ_REQ_IN_PROGRESS))
                return 1;
            if (!dp_is_cr_done(lane_count, st))
                return 0;
            if (!dp_is_ch_eq_done(lane_count, st) || !dp_is_symbol_locked(lane_count, st) ||
                !dp_is_interlane_aligned(align))
                return 0;
            for (lane = 0; lane < lane_count; lane++) {
                if (RD_DP_LANE_VS(lt->dpcd_lane_settings[lane]) != (adj[lane] & 3u) ||
                    RD_DP_LANE_PE(lt->dpcd_lane_settings[lane]) != ((adj[lane] >> 2) & 3u)) {
                    changed = 1;
                    break;
                }
            }
            if (changed) {
                dp_decide_lane_settings(lt, adj);
                /* dp_set_drive_settings */
                dp_set_hw_lane_settings(l, lt);
                dp_hw_to_dpcd_lane_settings(lt->hw_lane_settings, lt->dpcd_lane_settings);
                dpcd_set_lane_settings(l, lt);
                break;
            }
            l->d->os.msleep(l->d->os.ctx, 1);
        }
        if (!changed) {
            RLOG(l->d, "dp: post link training adjust request timed out");
            return 1;
        }
    }
    RLOG(l->d, "dp: post link training adjust request limit reached");
    return 1;
}

/* dp_transition_to_video_idle */
static int dp_transition_to_video_idle(rdna3_dp_link *l, rdna3_dp_lt_settings *lt, int status)
{
    uint8_t lane_count_set;
    /* 4. main link idle pattern */
    dp_set_hw_test_pattern(l, RD_DP_TRAINING_PATTERN_VIDEOIDLE);
    /* 5. post training adjust, unless TPS4 is used */
    if (!(l->dpcd_caps.max_ln_count & RD_DP_POST_LT_ADJ_REQ) ||
        lt->pattern_for_eq >= RD_DP_TRAINING_PATTERN_SEQUENCE_4) {
        if (status == RD_LT_SUCCESS) {
            l->d->os.msleep(l->d->os.ctx, 5);
            status = dp_check_link_loss_status(l, lt->link_settings.lane_count);
        }
        return status;
    }
    if (status == RD_LT_SUCCESS && !perform_post_lt_adj_req_sequence(l, lt))
        status = RD_LT_LQA_FAIL;
    lane_count_set = (uint8_t)(lt->link_settings.lane_count | (lt->enhanced_framing ? RD_DP_ENHANCED_FRAME : 0));
    dpcd_write1(l, RD_DPCD_LANE_COUNT_SET, lane_count_set);
    return status;
}

/* dp_log_training_result */
static void dp_log_training_result(rdna3_dp_link *l, const rdna3_dp_lt_settings *lt, int status)
{
    static const char *const res[] = { "pass", "CR failed lane0", "CR failed lane1", "CR failed lane23",
                                       "CR failed in EQ", "CR failed in EQ partially", "EQ failed",
                                       "LQA failed", "Link loss", "Aborted" };
    RLOG(l->d, "dp: link training %sx%u %s VS=%u, PE=%u, DS=%s", rate_name(lt->link_settings.link_rate),
         lt->link_settings.lane_count, status >= 0 && status <= RD_LT_ABORT ? res[status] : "Unknown",
         lt->hw_lane_settings[0].voltage_swing, lt->hw_lane_settings[0].pre_emphasis,
         lt->link_settings.link_spread ? "0.5% 30KHz" : "Disabled");
}

/* dp_perform_link_training (8b/10b) */
int dcn32_dp_perform_link_training(rdna3_dp_link *l, const rdna3_dp_link_settings *s, int skip_video_pattern)
{
    rdna3_dp_lt_settings lt;
    int status;
    /* dp_decide_training_settings + override_training_settings */
    decide_8b_10b_training_settings(l, s, &lt);
    /* reset previous training states */
    dpcd_exit_training_mode(l);
    /* configure the link before entering training mode */
    dpcd_configure_lttpr_mode(l, &lt);
    dp_set_fec_ready(l, lt.should_set_fec_ready);
    dpcd_configure_channel_coding(l);
    status = dp_perform_8b_10b_link_training(l, &lt);
    /* exit training mode */
    dpcd_exit_training_mode(l);
    /* switch to video idle */
    if (status == RD_LT_SUCCESS || !skip_video_pattern)
        status = dp_transition_to_video_idle(l, &lt, status);
    dp_log_training_result(l, &lt, status);
    l->last_result = status;
    l->lt_runs++;
    return status;
}

/* perform_link_training_with_retries */
int dcn32_dp_link_training_with_retries(rdna3_dp_link *l, const rdna3_dp_link_settings *set, int skip_video_pattern,
                                        int attempts, int do_fallback, uint32_t req_kbps)
{
    int j = 0, fail_count = 0, status = RD_LT_CR_FAIL_LANE0;
    uint32_t delay = RD_DP_LINK_TRAINING_RETRY_DELAY;
    rdna3_dp_link_settings cur = *set, max = *set;
    int is_link_bw_low = 0;
    int is_link_bw_min = cur.link_rate <= RD_DP_LINK_RATE_LOW && cur.lane_count <= 1;

    /* the idle pattern in SST must follow the training at once */
    setup_dio_stream_encoder(l);
    while (j < attempts && fail_count < attempts * 10) {
        RLOG(l->d, "dp: link training attempt %d of %d @ %s x%u spread %x", j + 1, attempts, rate_name(cur.link_rate),
             cur.lane_count, cur.link_spread);
        dcn32_dp_enable_link_phy(l, &cur);
        /* dp_set_panel_mode(DP_PANEL_MODE_DEFAULT) writes nothing */
        status = dcn32_dp_perform_link_training(l, &cur, skip_video_pattern);
        if (status == RD_LT_SUCCESS && !is_link_bw_low)
            return 1;
        fail_count++;
        /* the last attempt: keep the PHY on (DISPLAY_ENDPOINT_PHY) */
        if (j == attempts - 1 || status == RD_LT_ABORT)
            break;
        RLOG(l->d, "dp: link training attempt %d of %d failed @ %s x%u: %d", j + 1, attempts,
             rate_name(cur.link_rate), cur.lane_count, status);
        dcn32_dp_disable_link_phy(l);
        if (!do_fallback || status == RD_LT_ABORT || (status == RD_LT_SUCCESS && is_link_bw_low) || is_link_bw_min) {
            j++;
            cur = *set;
            delay += RD_DP_LINK_TRAINING_RETRY_DELAY;
            is_link_bw_low = 0;
            is_link_bw_min = cur.link_rate <= RD_DP_LINK_RATE_LOW && cur.lane_count <= 1;
        } else {
            uint32_t link_bw;
            dcn32_dp_fallback_link_setting(l, &max, &cur, status);
            l->lt_fallbacks++;
            link_bw = dcn32_dp_link_bw_kbps(l, &cur);
            is_link_bw_low = req_kbps > link_bw;
            is_link_bw_min = cur.link_rate <= RD_DP_LINK_RATE_LOW && cur.lane_count <= 1;
            if (is_link_bw_low) {
                RLOG(l->d, "dp: link bandwidth too low after fallback req_bw(%u) > link_bw(%u)", req_kbps, link_bw);
                return 0;
            }
        }
        l->d->os.msleep(l->d->os.ctx, delay);
    }
    return 0;
}

/* dp_verify_link_cap / dp_verify_link_cap_with_retries */
int dcn32_dp_verify_link_cap(rdna3_dp_link *l, int attempts)
{
    static const rdna3_dp_link_settings fail_safe = { 1, RD_DP_LINK_RATE_LOW, RD_DP_LINK_SPREAD_DISABLED };
    rdna3_dp_link_settings last = fail_safe;
    int i, success = 0, fail_count = 0;   /* failures of every attempt and fallback step */

    if (!l->caps_valid)
        return 0;
    for (i = 0; i < attempts; i++) {
        rdna3_dp_link_settings cur = l->max_link_cap, max = l->max_link_cap;
        int ok = 0, status;
        /* grant the extended timeout an LTTPR asks for */
        if (dp_is_lttpr_present(l) && l->dpcd_caps.lttpr.max_ext_timeout > 0)
            dpcd_write1(l, RD_DPCD_PHY_REPEATER_EXTENDED_WAIT_TIMEOUT,
                        (uint8_t)(l->dpcd_caps.lttpr.max_ext_timeout & 0x80));
        do {
            int skip_video_pattern = cur.link_rate != RD_DP_LINK_RATE_LOW;
            dcn32_dp_enable_link_phy(l, &cur);
            status = dcn32_dp_perform_link_training(l, &cur, skip_video_pattern);
            if (status == RD_LT_SUCCESS) {
                ok = 1;
                fsleep(l->d, 1000);
                /* the link loss check on the sink's IRQ status data */
                if (dp_check_link_loss_status(l, cur.lane_count) != RD_LT_SUCCESS)
                    fail_count++;
            } else if (status == RD_LT_LINK_LOSS) {
                ok = 1;
                fail_count++;
            } else {
                fail_count++;
            }
            dcn32_dp_disable_link_phy(l);
            if (ok)
                break;
            l->lt_fallbacks++;
        } while (dcn32_dp_fallback_link_setting(l, &max, &cur, status));
        l->verified_link_cap = ok ? cur : fail_safe;
        if (ok) {
            last = l->verified_link_cap;
            if (!fail_count) {
                success = 1;
                break;
            }
        } else {
            l->verified_link_cap = last;
        }
        fsleep(l->d, 10 * 1000);
    }
    l->verified = 1;
    RLOG(l->d, "dp: AUX %u: verified link cap %s x%u (%s)", l->aux, rate_name(l->verified_link_cap.link_rate),
         l->verified_link_cap.lane_count, success ? "trained" : "fail-safe / with link loss");
    return success;
}


/* ---- DSC on the link (link_dpms.c, amdgpu_dm.c, amdgpu_dm_helpers.c) ---- */

/* the dc_crtc_timing of mode 'm' (RGB, 'bpc' per component, no borders) */
void dcn32_dp_dsc_timing(const rdna3_mode *m, uint32_t bpc, rdna3_dsc_timing *t)
{
    uint32_t i;
    for (i = 0; i < sizeof(*t); i++)
        ((uint8_t *)t)[i] = 0;
    t->h_addressable = m->h_active;
    t->v_addressable = m->v_active;
    t->h_total = m->h_total;
    t->v_total = m->v_total;
    t->pix_clk_100hz = m->pixel_khz * 10;
    t->bpc = bpc;
    t->pixel_encoding = RD_PIXEL_ENCODING_RGB;
}

/* update_dsc_caps: dc_dsc_parse_dsc_dpcd for a sink without a converter or
 * behind a DP-HDMI converter (the branch decoder caps passed as read) */
int dcn32_dp_sink_dsc_caps(const rdna3_dp_link *l, rdna3_dsc_dec_caps *caps)
{
    uint32_t i;
    for (i = 0; i < sizeof(*caps); i++)
        ((uint8_t *)caps)[i] = 0;
    if (!l->caps_valid || (l->dpcd_caps.dongle_type != RD_DONGLE_NONE && l->dpcd_caps.dongle_type != RD_DONGLE_DP_HDMI))
        return 0;
    return dcn32_dsc_parse_dsc_dpcd(l->dpcd_caps.dsc_basic, l->dpcd_caps.dsc_branch, caps);
}

/* apply_dsc_policy_for_stream, SST sink without a converter: DSC (with the
 * default options, no EDID DSC bpp limit) when dc_dsc_compute_config finds
 * the mode needs more than dc_link_bandwidth_kbps of the link cap; behind
 * the link's DSC policy (rdna3dsc=1) */
int dcn32_dp_decide_dsc(const rdna3_dp_link *l, const rdna3_mode *m, uint32_t bpc, rdna3_dsc_config *cfg)
{
    rdna3_dsc_dec_caps caps;
    rdna3_dsc_options o;
    rdna3_dsc_timing t;
    uint32_t i, link_bandwidth_kbps;
    for (i = 0; i < sizeof(*cfg); i++)
        ((uint8_t *)cfg)[i] = 0;
    if (!l->dsc_policy || l->mst || l->dpcd_caps.dongle_type != RD_DONGLE_NONE || !dcn32_dp_sink_dsc_caps(l, &caps))
        return 0;
    dcn32_dsc_default_options(&o);
    link_bandwidth_kbps = dcn32_dp_link_bw_kbps(l, verified_cap(l));
    dcn32_dp_dsc_timing(m, bpc, &t);
    return dcn32_dsc_compute_config(&caps, &o, link_bandwidth_kbps, &t, cfg);
}

/* dc_bandwidth_in_kbps_from_timing of mode 'm' (with flags.DSC when 'cfg') */
uint32_t dcn32_dp_mode_kbps(const rdna3_mode *m, uint32_t bpc, const rdna3_dsc_config *cfg)
{
    rdna3_dsc_timing t;
    dcn32_dp_dsc_timing(m, bpc, &t);
    if (cfg) {
        t.dsc = 1;
        t.dsc_cfg = *cfg;
    }
    return dcn32_dsc_timing_kbps(&t);
}

/* dp_set_dsc_on_rx -> dm_helpers_dp_write_dsc_enable (SST: DP_DSC_ENABLE
 * on the RX itself, no converter or a DP-HDMI one) */
static int dp_set_dsc_on_rx(rdna3_dp_link *l, int enable)
{
    uint8_t enable_dsc = enable ? 0x01 : 0x00;   /* DSC_DECODING / DSC_DISABLE */
    if (l->dpcd_caps.dongle_type != RD_DONGLE_NONE && l->dpcd_caps.dongle_type != RD_DONGLE_DP_HDMI)
        return 0;
    RLOG(l->d, "dp: AUX %u: SST_DSC: DSC %s on the RX", l->aux, enable ? "enable" : "disable");
    return core_link_write_dpcd(l, RD_DPCD_DSC_ENABLE, &enable_dsc, 1) == RDNA3_OK;
}

static void dsc_hw_config(const rdna3_dp_link *l, const rdna3_mode *m, rdna3_dsc_hw_config *c)
{
    c->pic_width = m->h_active;
    c->pic_height = m->v_active;
    c->pixel_encoding = RD_PIXEL_ENCODING_RGB;
    c->bpc = 8;
    c->is_odm = 0;
    c->dc_dsc_cfg = l->dsc_cfg;
}

/* link_set_dsc_on_stream: one pipe (opp_cnt 1); DCN 3.2 has no
 * set_dto_dscclk / set_ref_dscclk. The stream can be unblanked only after
 * DSC is on in the RX (dp_set_dsc_on_rx). */
static void link_set_dsc_on_stream(rdna3_dp_link *l, const rdna3_mode *m, int enable)
{
    if (enable) {
        rdna3_dsc_hw_config c;
        rdna3_dsc_optc_config optc = { 0, 0, 0 };
        uint32_t optc_dsc_mode;
        dsc_hw_config(l, m, &c);
        if (dcn32_dsc_set_config(l->d, l->dsc_inst, &c, &optc))
            RERR(l->d, "dp: DSC %u: config refused", l->dsc_inst);
        dcn32_dsc_enable(l->d, l->dsc_inst, l->opp);
        optc_dsc_mode = optc.is_pixel_format_444 ? RD_OPTC_DSC_ENABLED_444 : RD_OPTC_DSC_ENABLED_NATIVE_SUBSAMPLED;
        /* DSC in the stream encoder; the PPS SDP comes after the DIG FE is
         * connected to the BE */
        dcn32_enc_dp_set_dsc_config(l->d, l->fe, optc_dsc_mode);
        /* DSC in the OPTC */
        dcn32_optc_set_dsc_config(l->d, l->otg, optc_dsc_mode, optc.bytes_per_pixel, optc.slice_width);
        RLOG(l->d, "dp: DSC %u on OPP %u -> OTG %u -> DIG %u: slice width %u, bytes/pixel 0x%08x", l->dsc_inst,
             l->opp, l->otg, l->fe, optc.slice_width, optc.bytes_per_pixel);
    } else {
        dcn32_optc_set_dsc_config(l->d, l->otg, RD_OPTC_DSC_DISABLED, 0, 0);
        dcn32_enc_dp_set_dsc_config(l->d, l->fe, RD_OPTC_DSC_DISABLED);
        dcn32_enc_dp_set_dsc_pps_info_packet(l->d, l->fe, 0, 0, 1);
        /* dsc_disconnect is double buffered: dsc_disable may run while the
         * DSC is still connected, as in DC */
        dcn32_dsc_disconnect(l->d, l->dsc_inst);
        dcn32_dsc_disable(l->d, l->dsc_inst);
    }
}

/* link_set_dsc_pps_packet */
static int link_set_dsc_pps_packet(rdna3_dp_link *l, const rdna3_mode *m, int enable, int immediate_update)
{
    if (!l->stream_dsc)
        return 0;
    if (enable) {
        rdna3_dsc_hw_config c;
        dsc_hw_config(l, m, &c);
        dcn32_dsc_get_packed_pps(&c, l->pps);
        dcn32_enc_dp_set_dsc_pps_info_packet(l->d, l->fe, 1, l->pps, immediate_update);
    } else {
        uint32_t i;
        for (i = 0; i < sizeof(l->pps); i++)
            l->pps[i] = 0;
        dcn32_enc_dp_set_dsc_pps_info_packet(l->d, l->fe, 0, 0, 1);
    }
    return 1;
}

/* link_set_dsc_enable, with dcn32_update_dsc_pg's power gate around it (the
 * DSC ungated while the stream has it, gated once it is released) */
static int link_set_dsc_enable(rdna3_dp_link *l, const rdna3_mode *m, int enable)
{
    if (!l->stream_dsc)
        return 0;
    if (enable) {
        if (!dcn32_dsc_pg_status(l->d, l->dsc_inst))
            dcn32_dsc_pg_control(l->d, l->dsc_inst, 1);
        link_set_dsc_on_stream(l, m, 1);
    } else {
        dp_set_dsc_on_rx(l, 0);
        link_set_dsc_on_stream(l, m, 0);
        if (dcn32_dsc_pg_status(l->d, l->dsc_inst))
            dcn32_dsc_pg_control(l->d, l->dsc_inst, 0);
    }
    return 1;
}

/* ---- dpms (link_dpms.c) ---- */

/* disable_link_dp: an MST link stays up until its last payload goes; the
 * sink back to SST after an MST link; FEC off */
static void disable_link_dp(rdna3_dp_link *l)
{
    if (l->mst && l->mst_alloc.stream_count > 0)
        return;
    dcn32_dp_disable_link_phy(l);
    if (l->mst)
        dcn32_dp_mst_enable_on_sink(l, 0);
    dp_set_fec_enable(l, 0);
    dp_set_fec_ready(l, 0);
}

/* disable_link */
static void disable_link(rdna3_dp_link *l)
{
    disable_link_dp(l);
    if (!l->mst || l->mst_alloc.stream_count == 0)
        l->link_active = 0;
}

/* link_set_dpms_off for DP 8b/10b. SST: the link goes before the stream so
 * the receiver never sees TPS1; MST: the payload is deallocated first and
 * the encoder stays video active until the link goes. DSC off at the end. */
void dcn32_dp_link_set_dpms_off(rdna3_dp_link *l)
{
    /* the stream encoder's pixel mode the boot left (dio_se_pix_per_cycle) */
    if (!l->pix_per_cycle)
        l->pix_per_cycle = GET(RI(l->d, regDIG0_DIG_FIFO_CTRL0, l->fe, DIG_S), DIG0_DIG_FIFO_CTRL0,
                               DIG_FIFO_OUTPUT_PIXEL_MODE) ? 2 : 1;
    dcn20_blank_stream(l);
    if (l->mst)
        dcn32_dp_mst_deallocate_payload(l);
    if (!l->mst) {
        /* disable_link -> disable_link_dp -> dp_disable_link_phy */
        disable_link(l);
        /* dce110_disable_stream: the DP info packets off, reset_dio_stream_encoder */
        enc1_stream_encoder_stop_dp_info_packets(l);
        reset_dio_stream_encoder(l);
    } else {
        enc1_stream_encoder_stop_dp_info_packets(l);
        reset_dio_stream_encoder(l);
        disable_link(l);
    }
    if (l->stream_dsc)
        link_set_dsc_enable(l, 0, 0);
    l->stream_dsc = 0;
}

/* enable_link_dp: the source OUI again ("during mode switch we do
 * DP_SET_POWER off then on, and OUI is lost", 30 ms after it for a sink
 * with extended caps), training with retries (the requested symbol clock
 * is the link rate: DCN 3.2 clk_mgr has no notification), FEC on after it */
static int enable_link_dp(rdna3_dp_link *l, const rdna3_dp_link_settings *s, int do_fallback, uint32_t req)
{
    int skip_video_pattern = s->link_rate != RD_DP_LINK_RATE_LOW, status;
    dpcd_set_source_specific_data(l);
    if (l->dpcd_sink_ext_caps)
        l->d->os.msleep(l->d->os.ctx, 30);
    status = dcn32_dp_link_training_with_retries(l, s, skip_video_pattern, RD_DP_LINK_TRAINING_ATTEMPTS, do_fallback,
                                                 req) ? RDNA3_OK : RDNA3_EIO;
    dp_set_fec_enable(l, 1);
    return status;
}

/* enable_link_dp_mst: the link is trained once for every MST stream; MST_EN
 * on in the sink before the training (the payload table clear and the
 * pending down reply poll of dm_helpers are empty in amdgpu) */
static int enable_link_dp_mst(rdna3_dp_link *l, const rdna3_dp_link_settings *s)
{
    if (l->link_active)
        return RDNA3_OK;
    dcn32_dp_mst_enable_on_sink(l, 1);
    return enable_link_dp(l, s, 0, 0);
}

/* link_set_dpms_on for DP 8b/10b with mode 'm' (the OTG already runs it),
 * the stream's DSC config in l->stream_dsc / l->dsc_cfg, MST to l->mst_port
 * when l->mst: link_enc setup, stream attribute (MSA), DSC on the stream,
 * enable_link (SST / MST), the second setup, dcn20_enable_stream, DSC on
 * the RX and its PPS, the MST payload, dcn32_unblank_stream */
int dcn32_dp_link_set_dpms_on(rdna3_dp_link *l, const rdna3_mode *m, int do_fallback)
{
    rdna3_dp_link_settings s;
    uint32_t req = dcn32_dp_mode_kbps(m, 8, l->stream_dsc ? &l->dsc_cfg : 0);
    int status;

    if (!l->pix_per_cycle)
        l->pix_per_cycle = 1;
    /* link_decide_link_settings (done at validation in DC): MST takes the
     * verified link cap (decide_mst_link_settings) */
    if (l->mst) {
        s = *verified_cap(l);
    } else if (dcn32_dp_decide_link_settings(l, req, &s)) {
        RERR(l->d, "dp: no link setting carries %u kbps (link cap %s x%u)", req, rate_name(verified_cap(l)->link_rate),
             verified_cap(l)->lane_count);
        return RDNA3_EINVAL;
    }
    dcn10_link_encoder_setup(l);
    /* setup_dio_stream_attribute (no stereo sync) */
    enc2_stream_encoder_dp_set_stream_attribute(l, m);
    /* DSC before the DIG FE and BE are connected (before the training), so
     * the bandwidth sent to the BE stays what the link carries */
    if (l->stream_dsc)
        link_set_dsc_enable(l, m, 1);
    /* enable_link (a link still up is taken down first) */
    if (l->link_active)
        disable_link(l);
    status = l->mst ? enable_link_dp_mst(l, &s) : enable_link_dp(l, &s, do_fallback, req);
    if (l->mst)
        l->d->os.msleep(l->d->os.ctx, 200);
    if (status == RDNA3_OK) {
        l->link_active = 1;
    } else if (l->mst) {
        /* MST displays cannot go on without a trained link */
        RERR(l->d, "dp: MST link training failed at %s x%u: stream not enabled", rate_name(s.link_rate), s.lane_count);
        if (!l->link_active)
            disable_link(l);
        return RDNA3_EIO;
    } else {
        RERR(l->d, "dp: link training failed at %s x%u: enabling the stream anyway, as DC does",
             rate_name(s.link_rate), s.lane_count);
    }
    /* the second setup: the transmitter command may have left another DIG_MODE */
    dcn10_link_encoder_setup(l);
    /* dcn20_enable_stream: link_hwss->setup_stream_encoder (the pixel rate
     * divider and the early control stay as they are on DCN 3.2), then
     * dcn30_update_info_frame: no VSC (RGB, no PSR), SPD (no FreeSync), HDR
     * or adaptive sync packet in this driver's streams */
    setup_dio_stream_encoder(l);
    {
        rdna3_dp_info_frames f;
        uint32_t k;
        for (k = 0; k < sizeof(f); k++)
            ((uint8_t *)&f)[k] = 0;
        dcn32_enc_update_dp_info_packets_sdp_line_num(l, &f);
        dcn32_enc_update_dp_info_packets(l, &f);
    }
    /* the PPS SDP ("info frames") */
    if (l->stream_dsc) {
        dp_set_dsc_on_rx(l, 1);
        link_set_dsc_pps_packet(l, m, 1, 1);
    }
    if (l->mst && dcn32_dp_mst_allocate_payload(l, m, req))
        status = RDNA3_EIO;
    dcn32_unblank_stream(l, m->pixel_khz, l->cur_link_settings.link_rate ? l->cur_link_settings.link_rate
                                                                         : s.link_rate);
    enable_stream_features(l);
    RLOG(l->d, "dp: DIG %u -> UNIPHY %u%s: %ux%u %u kHz on %s x%u%s%s: %s", l->fe, l->be, l->mst ? " (MST)" : "",
         m->h_active, m->v_active, m->pixel_khz, rate_name(l->cur_link_settings.link_rate),
         l->cur_link_settings.lane_count, l->fec_state == RD_FEC_ENABLED ? ", FEC" : "",
         l->stream_dsc ? ", DSC" : "", status == RDNA3_OK ? "trained" : "NOT trained");
    return status;
}

/* ---- link_validation.c ---- */

/* dp_active_dongle_validate_timing (no 3D) for a stream of 'enc'
 * (RD_PIXEL_ENCODING_*) at 'depth' (RD_COLOR_DEPTH_*) */
static int dp_active_dongle_validate_timing(const rdna3_dp_link *l, const rdna3_mode *m, uint32_t enc, uint32_t depth)
{
    const rdna3_dp_caps *c = &l->dpcd_caps;
    const rdna3_dp_dongle_caps *dg = &c->dongle_caps;
    rdna3_crtc_format f;
    uint8_t dc = 0;

    switch (c->dongle_type) {
    case RD_DONGLE_DP_VGA:
    case RD_DONGLE_DP_DVI:
        return enc == RD_PIXEL_ENCODING_RGB;
    default:
        break;
    }
    if (c->dongle_type == RD_DONGLE_DP_HDMI && dg->extended_cap_valid) {
        switch (enc) {
        case RD_PIXEL_ENCODING_RGB:
        case RD_PIXEL_ENCODING_YCBCR444:
            break;
        case RD_PIXEL_ENCODING_YCBCR422:
            if (!dg->is_dp_hdmi_ycbcr422_pass_through)
                return 0;
            break;
        case RD_PIXEL_ENCODING_YCBCR420:
            if (!dg->is_dp_hdmi_ycbcr420_pass_through)
                return 0;
            break;
        default:
            return 0;
        }
        switch (depth) {
        case RD_COLOR_DEPTH_666:
        case RD_COLOR_DEPTH_888:
            break;
        case RD_COLOR_DEPTH_101010:
            if (dg->dp_hdmi_max_bpc < 10)
                return 0;
            break;
        case RD_COLOR_DEPTH_121212:
            if (dg->dp_hdmi_max_bpc < 12)
                return 0;
            break;
        default:
            return 0;   /* 14 / 16 bpc not supported */
        }
        if (dg->dp_hdmi_frl_max_link_bw_in_kbps > 0) {
            /* DP to HDMI FRL converter: the output timing without DSC */
            if (dcn32_dp_mode_kbps(m, depth == RD_COLOR_DEPTH_121212 ? 12 : depth == RD_COLOR_DEPTH_101010 ? 10 : 8,
                                   NULL) > dg->dp_hdmi_frl_max_link_bw_in_kbps)
                return 0;
        } else {
            /* DP to HDMI TMDS converter */
            f.pixel_encoding = enc;
            f.color_depth = depth;
            f.dsc = 0;
            f.dsc_ycbcr422_simple = 0;
            if (dcn32_tmds_output_pixel_clock_100hz(m->pixel_khz * 10, &f) > dg->dp_hdmi_max_pixel_clk_in_khz * 10)
                return 0;
        }
    }
    /* the DFP capability extension, unless the sink has 128b/132b or DSC pass-through */
    if (!(c->channel_coding_cap & 0x02) && !(c->dsc_basic[0] & 0x02) && dg->dfp_cap_ext.supported) {
        if (dg->dfp_cap_ext.max_pixel_rate_in_mps < m->pixel_khz / 1000)
            return 0;
        if (dg->dfp_cap_ext.max_video_h_active_width < m->h_active)
            return 0;
        if (dg->dfp_cap_ext.max_video_v_active_height < m->v_active)
            return 0;
        if (!(dg->dfp_cap_ext.encoding_format_caps & 0x01))   /* support_rgb (DC checks it for every encoding) */
            return 0;
        switch (enc) {
        case RD_PIXEL_ENCODING_RGB: dc = dg->dfp_cap_ext.rgb_color_depth_caps; break;
        case RD_PIXEL_ENCODING_YCBCR444: dc = dg->dfp_cap_ext.ycbcr444_color_depth_caps; break;
        case RD_PIXEL_ENCODING_YCBCR422: dc = dg->dfp_cap_ext.ycbcr422_color_depth_caps; break;
        case RD_PIXEL_ENCODING_YCBCR420: dc = dg->dfp_cap_ext.ycbcr420_color_depth_caps; break;
        default: break;
        }
        if ((depth == RD_COLOR_DEPTH_666 && enc == RD_PIXEL_ENCODING_RGB && !(dc & 0x01)) ||
            (depth == RD_COLOR_DEPTH_888 && !(dc & 0x02)) || (depth == RD_COLOR_DEPTH_101010 && !(dc & 0x04)) ||
            (depth == RD_COLOR_DEPTH_121212 && !(dc & 0x08)) || (depth == RD_COLOR_DEPTH_161616 && !(dc & 0x10)))
            return 0;
    }
    return 1;
}

/* link_validate_mode_timing: no passive dongle on a DP connector
 * (dongle_max_pix_clk 0), the active dongle; the bandwidth half
 * (dp_validate_mode_timing) is the caller's link setting decision */
int dcn32_dp_validate_mode_timing(const rdna3_dp_link *l, const rdna3_mode *m)
{
    return dp_active_dongle_validate_timing(l, m, RD_PIXEL_ENCODING_RGB, RD_COLOR_DEPTH_888);
}
