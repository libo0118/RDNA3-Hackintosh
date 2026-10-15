/*
 * dcn32_freesync.c - see dcn32_freesync.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_freesync.h"
#include "dcn32_otg.h"

#define EDID_LENGTH 128
/* struct edid offsets */
#define EDID_VERSION    18
#define EDID_REVISION   19
#define EDID_FEATURES   24
#define EDID_DETAILED   54
#define EDID_EXTENSIONS 126
#define EDID_DETAILED_TIMINGS 4
#define EDID_DETAILED_TIMING_LENGTH 18
#define DRM_EDID_FEATURE_CONTINUOUS_FREQ (1u << 0)
/* struct detailed_non_pixel + detailed_data_monitor_range */
#define DT_PAD1      2
#define DT_TYPE      3
#define DT_PAD2      4
#define DT_MIN_VFREQ 5
#define DT_MAX_VFREQ 6
#define DT_FLAGS     10
#define EDID_DETAIL_MONITOR_RANGE 0xfd
#define DRM_EDID_RANGE_LIMITS_ONLY_FLAG 0x01
#define DRM_EDID_RANGE_OFFSET_MIN_VFREQ (1u << 0)
#define DRM_EDID_RANGE_OFFSET_MAX_VFREQ (1u << 1)
#define CEA_EXT       0x02
#define DISPLAYID_EXT 0x70
/* DC_HDMI_INFOFRAME_TYPE_SPD */
#define INFOFRAME_TYPE_SPD 0x83
/* enum as_sdp_ver */
#define AS_SDP_VER_1 0x1
/* dc_wake_and_execute_dmub_cmd's wait */
#define EDID_CEA_TIMEOUT_US 100000

static int dc_is_hdmi_signal(uint32_t signal)
{
    return signal == RD_SIGNAL_HDMI_TYPE_A;
}

static int dc_is_dp_signal(uint32_t signal)
{
    return signal == RD_SIGNAL_DISPLAY_PORT || signal == RD_SIGNAL_DISPLAY_PORT_MST;
}

/* the extension blocks the buffer holds (edid->extensions, bounded) */
static uint32_t edid_extensions(const uint8_t *edid, uint32_t len)
{
    uint32_t n;
    if (!edid || len < EDID_LENGTH)
        return 0;
    n = edid[EDID_EXTENSIONS];
    if (n > len / EDID_LENGTH - 1)
        n = len / EDID_LENGTH - 1;
    return n;
}

/* ---- drm_edid.c ---- */

/* is_display_descriptor */
static int is_display_descriptor(const uint8_t *t, uint8_t type)
{
    return t[0] == 0 && t[1] == 0 && t[DT_PAD1] == 0 && t[DT_TYPE] == type;
}

/* get_monitor_range */
static void get_monitor_range(const uint8_t *edid, const uint8_t *t, uint32_t *min_vfreq, uint32_t *max_vfreq)
{
    if (!is_display_descriptor(t, EDID_DETAIL_MONITOR_RANGE))
        return;
    /* only the "range limits only" variant guarantees any timing in range */
    if (t[DT_FLAGS] != DRM_EDID_RANGE_LIMITS_ONLY_FLAG)
        return;
    *min_vfreq = t[DT_MIN_VFREQ];
    *max_vfreq = t[DT_MAX_VFREQ];
    if (edid[EDID_REVISION] >= 4) {
        if (t[DT_PAD2] & DRM_EDID_RANGE_OFFSET_MIN_VFREQ)
            *min_vfreq += 255;
        if (t[DT_PAD2] & DRM_EDID_RANGE_OFFSET_MAX_VFREQ)
            *max_vfreq += 255;
    }
}

void dcn32_drm_get_monitor_range(const uint8_t *edid, uint32_t len, uint32_t *min_vfreq, uint32_t *max_vfreq)
{
    uint32_t i, k, n;
    if (!edid || len < EDID_LENGTH)
        return;
    if (edid[EDID_REVISION] < 4)
        return;
    if (!(edid[EDID_FEATURES] & DRM_EDID_FEATURE_CONTINUOUS_FREQ))
        return;
    /* drm_for_each_detailed_block: the base block's four, then each CTA
     * extension's detailed timing area (cea_for_each_detailed_block) */
    for (i = 0; i < EDID_DETAILED_TIMINGS; i++)
        get_monitor_range(edid, edid + EDID_DETAILED + EDID_DETAILED_TIMING_LENGTH * i, min_vfreq, max_vfreq);
    n = edid_extensions(edid, len);
    for (k = 1; k <= n; k++) {
        const uint8_t *ext = edid + EDID_LENGTH * k;
        uint32_t d = ext[2];
        if (ext[0] != CEA_EXT || d < 4 || d > 127)
            continue;
        for (i = 0; i < (127 - d) / EDID_DETAILED_TIMING_LENGTH; i++)
            get_monitor_range(edid, ext + d + EDID_DETAILED_TIMING_LENGTH * i, min_vfreq, max_vfreq);
    }
}

/* ---- amdgpu_dm.c: the sink's range ---- */

void dcn32_parse_edid_displayid_vrr(const uint8_t *edid, uint32_t len, uint32_t *min_vfreq, uint32_t *max_vfreq)
{
    const uint8_t *ext = NULL;
    uint32_t i, j, n = edid_extensions(edid, len), lo, hi;
    if (!n)
        return;
    /* the DisplayID extension (the last block when there is none, as amdgpu) */
    for (i = 0; i < n; i++) {
        ext = edid + EDID_LENGTH * (i + 1);
        if (ext[0] == DISPLAYID_EXT)
            break;
    }
    for (j = 0; j < EDID_LENGTH; j++) {
        /* the dynamic video timing range of DisplayID */
        if (EDID_LENGTH - j > 13 && ext[j] == 0x25 && (ext[j + 1] & 0xFE) == 0 && ext[j + 2] == 9) {
            lo = ext[j + 9];
            if (ext[j + 1] & 7)
                hi = ext[j + 10] + ((uint32_t)(ext[j + 11] & 3) << 8);
            else
                hi = ext[j + 10];
            if (hi && lo) {
                *max_vfreq = hi;
                *min_vfreq = lo;
                return;
            }
        }
    }
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/* dm_edid_parser_send_cea: one chunk of the CTA block to the DMCUB, its
 * ack, or (after the last) the AMD VSDB it found. 1 = go on, 0 = no AMD
 * VSDB (amdgpu's false), -1 = the firmware did not run it or answered
 * something unknown (also false in amdgpu; told apart here so the caller
 * stops asking a hung DMCUB) */
static int dm_edid_parser_send_cea(rdna3_dmub *dm, uint32_t offset, uint32_t total_length, const uint8_t *data,
                                   uint32_t length, rdna3_vsdb_info *vsdb)
{
    uint8_t cmd[RD_DMUB_RB_CMD_SIZE];
    uint32_t i, hdr;
    int ret;
    if (length > RD_DMUB_EDID_CEA_DATA_CHUNK)
        return 0;
    for (i = 0; i < sizeof(cmd); i++)
        cmd[i] = 0;
    hdr = RD_DMUB_HDR(RD_DMUB_CMD_EDID_CEA, 0, RD_DMUB_EDID_CEA_PAYLOAD, 0);
    cmd[0] = (uint8_t)hdr; cmd[1] = (uint8_t)(hdr >> 8); cmd[2] = (uint8_t)(hdr >> 16); cmd[3] = (uint8_t)(hdr >> 24);
    /* struct dmub_cmd_send_edid_cea (packed): offset, length, cea_total_length, payload */
    put16(cmd + 4, offset);
    cmd[6] = (uint8_t)length;
    put16(cmd + 7, total_length);
    for (i = 0; i < length; i++)
        cmd[9 + i] = data[i];
    ret = dmub32_cmd_execute(dm, cmd, 1, EDID_CEA_TIMEOUT_US);
    if (ret != RDNA3_OK) {
        RERR(dm->d, "freesync: EDID CEA parser failed at offset %u (%d)", offset, ret);
        return -1;
    }
    /* struct dmub_cmd_edid_cea_output: type, then the ack or the VSDB */
    if (cmd[4] == RD_DMUB_EDID_CEA_ACK) {
        if (!cmd[7])
            RERR(dm->d, "freesync: EDID CEA ack failed at offset %u", get16(cmd + 5));
    } else if (cmd[4] == RD_DMUB_EDID_CEA_AMD_VSDB) {
        if (!cmd[5])
            return 0;
        vsdb->freesync_supported = cmd[6];
        vsdb->amd_vsdb_version = get16(cmd + 7);
        vsdb->min_refresh_rate_hz = get16(cmd + 9);
        vsdb->max_refresh_rate_hz = get16(cmd + 11);
    } else {
        RERR(dm->d, "freesync: unknown EDID CEA parser result %u at offset %u", cmd[4], offset);
        return -1;
    }
    return 1;
}

/* parse_edid_cea_dmub (8 bytes per command): 1 FreeSync, 0 none, -1 the
 * firmware failed */
static int parse_edid_cea_dmub(rdna3_dmub *dm, const uint8_t *edid_ext, uint32_t len, rdna3_vsdb_info *v)
{
    uint32_t i;
    int r;
    for (i = 0; i < len; i += RD_DMUB_EDID_CEA_DATA_CHUNK)
        if ((r = dm_edid_parser_send_cea(dm, i, len, edid_ext + i, RD_DMUB_EDID_CEA_DATA_CHUNK, v)) <= 0)
            return r;
    return v->freesync_supported ? 1 : 0;
}

int dcn32_parse_hdmi_amd_vsdb(rdna3_dmub *dm, const uint8_t *edid, uint32_t len, rdna3_vsdb_info *v)
{
    uint32_t i, n = edid_extensions(edid, len);
    const uint8_t *ext = NULL;
    if (!n)
        return -1;
    for (i = 0; i < n; i++) {
        ext = edid + EDID_LENGTH * (i + 1);
        if (ext[0] == CEA_EXT)
            break;
    }
    if (i == n)
        return -1;
    /* parse_edid_cea: the DMCUB parses it (no DMCU on DCN 3.2) */
    if (!dm || !dm->hw_init)
        return -1;
    switch (parse_edid_cea_dmub(dm, ext, EDID_LENGTH, v)) {
    case 1:  return (int)i;
    case 0:  return -1;
    default: return RD_AMD_VSDB_PARSER_FAILED;
    }
}

void dcn32_update_freesync_caps(rdna3_freesync_caps *c, const uint8_t *edid, uint32_t len, uint32_t signal,
                                int allow_invalid_msa_timing_param, int as_type, rdna3_dmub *dm)
{
    uint32_t i;
    int k;
    rdna3_vsdb_info vsdb_info = { 0, 0, 0, 0 };
    for (i = 0; i < sizeof(*c); i++)
        ((uint8_t *)c)[i] = 0;
    c->as_type = RD_ADAPTIVE_SYNC_TYPE_NONE;
    c->hdmi_vsdb = -1;
    if (!edid || len < EDID_LENGTH)
        return;
    /* drm_edid_connector_update: DRM's monitor_range first */
    dcn32_drm_get_monitor_range(edid, len, &c->monitor_min_vfreq, &c->monitor_max_vfreq);
    /* some eDP panels have the range in DisplayID only */
    if (c->monitor_min_vfreq == 0 || c->monitor_max_vfreq == 0)
        dcn32_parse_edid_displayid_vrr(edid, len, &c->monitor_min_vfreq, &c->monitor_max_vfreq);

    if (signal == RD_SIGNAL_DISPLAY_PORT) {
        int edid_check_required = 0;
        if (allow_invalid_msa_timing_param) {
            if (edid[EDID_FEATURES] & DRM_EDID_FEATURE_CONTINUOUS_FREQ) {
                c->min_vfreq = c->monitor_min_vfreq;
                c->max_vfreq = c->monitor_max_vfreq;
                c->from = RD_VRR_FROM_MONITOR_RANGE;
                if ((int)c->max_vfreq - (int)c->min_vfreq > RD_FREESYNC_MIN_REFRESH_RANGE)
                    c->freesync_capable = 1;
            } else {
                edid_check_required = edid[EDID_VERSION] > 1 ||
                                      (edid[EDID_VERSION] == 1 && edid[EDID_REVISION] > 1);
            }
        }
        if (edid_check_required) {
            for (i = 0; i < EDID_DETAILED_TIMINGS; i++) {
                const uint8_t *t = edid + EDID_DETAILED + EDID_DETAILED_TIMING_LENGTH * i;
                /* a monitor range descriptor with "range limits only" (no
                 * GTF / CVT formula) */
                if (t[DT_TYPE] != EDID_DETAIL_MONITOR_RANGE)
                    continue;
                if (t[DT_FLAGS] != 1)
                    continue;
                c->monitor_min_vfreq = t[DT_MIN_VFREQ];
                c->monitor_max_vfreq = t[DT_MAX_VFREQ];
                if (edid[EDID_REVISION] >= 4) {
                    if (t[DT_PAD2] & DRM_EDID_RANGE_OFFSET_MIN_VFREQ)
                        c->monitor_min_vfreq += 255;
                    if (t[DT_PAD2] & DRM_EDID_RANGE_OFFSET_MAX_VFREQ)
                        c->monitor_max_vfreq += 255;
                }
                c->min_vfreq = c->monitor_min_vfreq;
                c->max_vfreq = c->monitor_max_vfreq;
                c->from = RD_VRR_FROM_RANGE_DESCRIPTOR;
                break;
            }
            if ((int)c->max_vfreq - (int)c->min_vfreq > RD_FREESYNC_MIN_REFRESH_RANGE)
                c->freesync_capable = 1;
        }
        /* parse_amd_vsdb: eDP replay only, not ported */
    } else if (signal == RD_SIGNAL_HDMI_TYPE_A) {
        k = dcn32_parse_hdmi_amd_vsdb(dm, edid, len, &vsdb_info);
        c->hdmi_vsdb = k;
        if (k >= 0 && vsdb_info.freesync_supported) {
            c->min_vfreq = vsdb_info.min_refresh_rate_hz;
            c->max_vfreq = vsdb_info.max_refresh_rate_hz;
            c->from = RD_VRR_FROM_AMD_VSDB;
            if ((int)c->max_vfreq - (int)c->min_vfreq > RD_FREESYNC_MIN_REFRESH_RANGE)
                c->freesync_capable = 1;
            c->monitor_min_vfreq = vsdb_info.min_refresh_rate_hz;
            c->monitor_max_vfreq = vsdb_info.max_refresh_rate_hz;
        }
    }

    if (as_type == RD_FREESYNC_TYPE_PCON_IN_WHITELIST) {
        k = dcn32_parse_hdmi_amd_vsdb(dm, edid, len, &vsdb_info);
        c->hdmi_vsdb = k;
        if (k >= 0 && vsdb_info.freesync_supported && vsdb_info.amd_vsdb_version > 0) {
            c->pack_sdp_v1_3 = 1;
            c->as_type = as_type;
            c->vsdb_info = vsdb_info;
            c->min_vfreq = vsdb_info.min_refresh_rate_hz;
            c->max_vfreq = vsdb_info.max_refresh_rate_hz;
            c->from = RD_VRR_FROM_PCON_VSDB;
            if ((int)c->max_vfreq - (int)c->min_vfreq > RD_FREESYNC_MIN_REFRESH_RANGE)
                c->freesync_capable = 1;
            c->monitor_min_vfreq = vsdb_info.min_refresh_rate_hz;
            c->monitor_max_vfreq = vsdb_info.max_refresh_rate_hz;
        }
    }
}

/* ---- the stream ---- */

uint32_t dcn32_drm_mode_vrefresh(const rdna3_mode *m)
{
    uint64_t num = 1000, den;
    if (!m->h_total || !m->v_total)
        return 0;
    if (m->interlaced)
        num *= 2;
    den = (uint64_t)m->h_total * m->v_total;
    /* DIV_ROUND_CLOSEST_ULL */
    return (uint32_t)(((uint64_t)m->pixel_khz * num + den / 2) / den);
}

void dcn32_vrr_stream_from_mode(rdna3_vrr_stream *s, const rdna3_mode *m, uint32_t signal)
{
    uint32_t i;
    for (i = 0; i < sizeof(*s); i++)
        ((uint8_t *)s)[i] = 0;
    s->pix_clk_100hz = m->pixel_khz * 10;
    s->h_total = m->h_total;
    s->v_total = m->v_total;
    s->v_addressable = m->v_active;
    s->v_front_porch = m->v_front;
    s->signal = signal;
}

uint64_t dcn32_freesync_calc_nominal_field_rate(const rdna3_vrr_stream *s)
{
    uint64_t total = (uint64_t)s->h_total * s->v_total;
    if (!total)
        return 0;
    return (uint64_t)s->pix_clk_100hz * 100000000ull / total;
}

/* calc_duration_in_us_from_refresh_in_uhz */
static uint32_t calc_duration_in_us_from_refresh_in_uhz(uint32_t refresh_in_uhz)
{
    return refresh_in_uhz ? (uint32_t)(1000000000ull * 1000 / refresh_in_uhz) : 0;
}

uint32_t dcn32_freesync_calc_v_total_from_refresh(const rdna3_vrr_stream *s, uint32_t refresh_in_uhz)
{
    uint32_t v_total, frame_duration_in_ns;
    if (!refresh_in_uhz || !s->h_total)
        return s->v_total;
    frame_duration_in_ns = (uint32_t)(1000000000ull * 1000000 / refresh_in_uhz);
    v_total = (uint32_t)(((uint64_t)frame_duration_in_ns * (s->pix_clk_100hz / 10) / s->h_total + 500000) / 1000000);
    /* v_total cannot be less than nominal */
    if (v_total < s->v_total)
        v_total = s->v_total;
    return v_total;
}

/* vrr_settings_require_update */
static int vrr_settings_require_update(const rdna3_freesync_config *in_config, uint32_t min_refresh_in_uhz,
                                       uint32_t max_refresh_in_uhz, const rdna3_vrr_params *in_vrr)
{
    if (in_vrr->state != in_config->state)
        return 1;
    if (in_vrr->state == RD_VRR_STATE_ACTIVE_FIXED &&
        in_vrr->fixed.target_refresh_in_uhz != in_config->fixed_refresh_in_uhz)
        return 1;
    if (in_vrr->min_refresh_in_uhz != min_refresh_in_uhz)
        return 1;
    if (in_vrr->max_refresh_in_uhz != max_refresh_in_uhz)
        return 1;
    return 0;
}

static void vrr_adjust_nominal(const rdna3_vrr_stream *s, rdna3_vrr_params *vrr)
{
    vrr->adjust.v_total_min = s->v_total;
    vrr->adjust.v_total_max = s->v_total;
}

void dcn32_freesync_build_vrr_params(const rdna3_vrr_stream *s, const rdna3_freesync_config *in_config,
                                     rdna3_vrr_params *vrr)
{
    uint64_t nominal_field_rate_in_uhz, rounded_nominal_in_uhz;
    uint64_t min_refresh_in_uhz, max_refresh_in_uhz, min_hardware_refresh_in_uhz = 0;
    uint32_t refresh_range = 0;

    nominal_field_rate_in_uhz = dcn32_freesync_calc_nominal_field_rate(s);
    if (s->h_total != 0)
        min_hardware_refresh_in_uhz = (uint64_t)s->pix_clk_100hz * 100000000ull /
                                      ((uint64_t)s->h_total * RD_DCN32_MAX_V_TOTAL);
    /* the minimum refresh the hardware can do */
    min_refresh_in_uhz = min_hardware_refresh_in_uhz > in_config->min_refresh_in_uhz ? min_hardware_refresh_in_uhz
                                                                                      : in_config->min_refresh_in_uhz;
    max_refresh_in_uhz = in_config->max_refresh_in_uhz;
    /* the full range may be larger than the timing: cap at nominal */
    if (max_refresh_in_uhz > nominal_field_rate_in_uhz)
        max_refresh_in_uhz = nominal_field_rate_in_uhz;
    if (min_refresh_in_uhz > max_refresh_in_uhz)
        min_refresh_in_uhz = max_refresh_in_uhz;
    /* a monitor with max exactly 2x min: enforce it on the nominal */
    rounded_nominal_in_uhz = (nominal_field_rate_in_uhz + 50000) / 100000 * 100000;
    if (in_config->max_refresh_in_uhz == 2ull * in_config->min_refresh_in_uhz &&
        in_config->max_refresh_in_uhz == rounded_nominal_in_uhz)
        min_refresh_in_uhz = nominal_field_rate_in_uhz / 2;

    if (!vrr_settings_require_update(in_config, (uint32_t)min_refresh_in_uhz, (uint32_t)max_refresh_in_uhz, vrr))
        return;

    vrr->state = in_config->state;
    vrr->send_info_frame = in_config->vsif_supported;

    if (in_config->state == RD_VRR_STATE_UNSUPPORTED) {
        vrr->state = RD_VRR_STATE_UNSUPPORTED;
        vrr->supported = 0;
        vrr_adjust_nominal(s, vrr);
        return;
    }
    vrr->min_refresh_in_uhz = (uint32_t)min_refresh_in_uhz;
    vrr->max_duration_in_us = calc_duration_in_us_from_refresh_in_uhz((uint32_t)min_refresh_in_uhz);
    vrr->max_refresh_in_uhz = (uint32_t)max_refresh_in_uhz;
    vrr->min_duration_in_us = calc_duration_in_us_from_refresh_in_uhz((uint32_t)max_refresh_in_uhz);
    if (in_config->state == RD_VRR_STATE_ACTIVE_FIXED)
        vrr->fixed_refresh_in_uhz = in_config->fixed_refresh_in_uhz;
    else
        vrr->fixed_refresh_in_uhz = 0;
    refresh_range = (vrr->max_refresh_in_uhz + 500000) / 1000000 - (vrr->min_refresh_in_uhz + 500000) / 1000000;
    vrr->supported = 1;

    vrr->fixed.ramping_active = in_config->ramping;
    vrr->btr.btr_enabled = in_config->btr;
    if (vrr->max_refresh_in_uhz < 2ull * vrr->min_refresh_in_uhz) {
        vrr->btr.btr_enabled = 0;
    } else {
        vrr->btr.margin_in_us = vrr->max_duration_in_us - 2 * vrr->min_duration_in_us;
        if (vrr->btr.margin_in_us > 2500)   /* BTR_MAX_MARGIN */
            vrr->btr.margin_in_us = 2500;
    }
    vrr->btr.btr_active = 0;
    vrr->btr.inserted_duration_in_us = 0;
    vrr->btr.frames_to_insert = 0;
    vrr->btr.frame_counter = 0;
    vrr->fixed.fixed_active = 0;
    vrr->fixed.target_refresh_in_uhz = 0;
    vrr->btr.mid_point_in_us = (vrr->min_duration_in_us + vrr->max_duration_in_us) / 2;

    if (vrr->state == RD_VRR_STATE_DISABLED || vrr->state == RD_VRR_STATE_INACTIVE) {
        vrr_adjust_nominal(s, vrr);
    } else if (vrr->state == RD_VRR_STATE_ACTIVE_VARIABLE && refresh_range >= RD_FREESYNC_MIN_REFRESH_RANGE) {
        vrr->adjust.v_total_min = dcn32_freesync_calc_v_total_from_refresh(s, vrr->max_refresh_in_uhz);
        vrr->adjust.v_total_max = dcn32_freesync_calc_v_total_from_refresh(s, vrr->min_refresh_in_uhz);
    } else if (vrr->state == RD_VRR_STATE_ACTIVE_FIXED) {
        vrr->fixed.target_refresh_in_uhz = vrr->fixed_refresh_in_uhz;
        /* fixed_active was cleared above, so amdgpu's "ramping already
         * active" branch cannot be taken here: the v_totals always follow */
        vrr->fixed.fixed_active = 1;
        vrr->adjust.v_total_min = dcn32_freesync_calc_v_total_from_refresh(s, vrr->fixed.target_refresh_in_uhz);
        vrr->adjust.v_total_max = vrr->adjust.v_total_min;
    } else {
        vrr->state = RD_VRR_STATE_INACTIVE;
        vrr_adjust_nominal(s, vrr);
    }
    vrr->adjust.allow_otg_v_count_halt = in_config->state == RD_VRR_STATE_ACTIVE_FIXED;
}

/* ---- the infopackets ---- */

/* build_vrr_infopacket_data_v1 / _v3: the AMD OUI and the FreeSync bits */
static void build_vrr_infopacket_data(const rdna3_vrr_params *vrr, rdna3_info_packet *p, int freesync_on_desktop,
                                      int v3)
{
    int active = vrr->state == RD_VRR_STATE_ACTIVE_VARIABLE || vrr->state == RD_VRR_STATE_ACTIVE_FIXED;
    int enabled = vrr->state != RD_VRR_STATE_DISABLED && vrr->state != RD_VRR_STATE_UNSUPPORTED;
    uint32_t min_refresh, max_refresh, fixed_refresh, min_programmed;
    p->sb[1] = 0x1A;   /* PB1-3: AMD IEEE OUI 0x00001A */
    p->sb[2] = 0x00;
    p->sb[3] = 0x00;
    if (vrr->state != RD_VRR_STATE_UNSUPPORTED)
        p->sb[6] |= 0x01;   /* FreeSync supported */
    if (enabled)
        p->sb[6] |= 0x02;   /* FreeSync enabled */
    if (freesync_on_desktop ? enabled : active)
        p->sb[6] |= 0x04;   /* FreeSync active */
    max_refresh = (vrr->max_refresh_in_uhz + 500000) / 1000000;
    if (!v3) {
        /* v1 / v2: the full range when active, else the nominal */
        p->sb[7] = (uint8_t)(active ? (vrr->min_refresh_in_uhz + 500000) / 1000000 : max_refresh);
        p->sb[8] = (uint8_t)max_refresh;
        return;
    }
    min_refresh = (vrr->min_refresh_in_uhz + 500000) / 1000000;
    fixed_refresh = (vrr->fixed_refresh_in_uhz + 500000) / 1000000;
    min_programmed = vrr->state == RD_VRR_STATE_ACTIVE_FIXED      ? fixed_refresh
                   : vrr->state == RD_VRR_STATE_ACTIVE_VARIABLE   ? min_refresh
                   : vrr->state == RD_VRR_STATE_INACTIVE          ? min_refresh
                                                                  : max_refresh;
    p->sb[7] = (uint8_t)(min_programmed & 0xFF);
    p->sb[8] = (uint8_t)(max_refresh & 0xFF);
    p->sb[11] = (uint8_t)((min_programmed >> 8) & 0x03);
    p->sb[12] = (uint8_t)((max_refresh >> 8) & 0x03);
    p->sb[16] = vrr->state == RD_VRR_STATE_ACTIVE_FIXED ? 1 : 0;
}

/* build_vrr_infopacket_header_v1 / _v2 / _v3 */
static void build_vrr_infopacket_header(uint32_t signal, uint32_t version, rdna3_info_packet *p, uint32_t *payload_size)
{
    static const uint8_t hdmi_len[4] = { 0, 0x08, 0x09, 0x10 };
    if (dc_is_hdmi_signal(signal)) {
        p->hb0 = INFOFRAME_TYPE_SPD;
        p->hb1 = (uint8_t)version;
        p->hb2 = hdmi_len[version];
        *payload_size = hdmi_len[version];
    } else if (dc_is_dp_signal(signal)) {
        p->hb0 = 0x00;                /* secondary-data packet ID */
        p->hb1 = INFOFRAME_TYPE_SPD;
        p->hb2 = 0x1B;                /* INFOFRAME: 1Bh */
        p->hb3 = (uint8_t)((version & 0x3F) << 2);   /* SDP version */
        *payload_size = 0x1B;
    }
}

/* build_vrr_infopacket_checksum */
static void build_vrr_infopacket_checksum(uint32_t payload_size, rdna3_info_packet *p)
{
    uint32_t idx;
    uint8_t checksum = (uint8_t)(p->hb0 + p->hb1 + p->hb2 + p->hb3);
    for (idx = 1; idx <= payload_size && idx < sizeof(p->sb); idx++)
        checksum = (uint8_t)(checksum + p->sb[idx]);
    p->sb[0] = (uint8_t)(0x100 - checksum);
    p->valid = 1;
}

/* build_vrr_infopacket_sdp_v1_3 */
static void build_vrr_infopacket_sdp_v1_3(uint32_t packet_type, rdna3_info_packet *p)
{
    uint8_t idx, size = packet_type == RD_PACKET_TYPE_FS_V1 ? 0x08 : packet_type == RD_PACKET_TYPE_FS_V3 ? 0x10 : 0x09;
    for (idx = p->hb2; idx > 1; idx--)   /* data byte count 0x1B */
        p->sb[idx] = p->sb[idx - 1];
    p->sb[1] = size;                          /* length */
    p->sb[0] = (uint8_t)((p->hb3 >> 2) & 0x3F);   /* version */
    p->hb3 = 0x13 << 2;                       /* header, SDP 1.3 */
    p->hb2 = 0x1D;
}

void dcn32_freesync_build_vrr_infopacket(const rdna3_vrr_stream *s, const rdna3_vrr_params *vrr, uint32_t packet_type,
                                         rdna3_info_packet *p, int pack_sdp_v1_3)
{
    uint32_t payload_size = 0, version;
    if (!vrr->send_info_frame)
        return;
    /* v2 / v3 add the FreeSync 2 bits of app_tf: TRANSFER_FUNC_UNKNOWN here */
    version = packet_type == RD_PACKET_TYPE_FS_V3 ? 3 : packet_type == RD_PACKET_TYPE_FS_V2 ? 2 : 1;
    build_vrr_infopacket_header(s->signal, version, p, &payload_size);
    build_vrr_infopacket_data(vrr, p, s->freesync_on_desktop, version == 3);
    build_vrr_infopacket_checksum(payload_size, p);
    if (pack_sdp_v1_3 && dc_is_dp_signal(s->signal) && packet_type != RD_PACKET_TYPE_VRR &&
        packet_type != RD_PACKET_TYPE_VTEM)
        build_vrr_infopacket_sdp_v1_3(packet_type, p);
}

void dcn32_build_adaptive_sync_infopacket(int as_type, rdna3_info_packet *p)
{
    uint32_t i;
    for (i = 0; i < sizeof(*p); i++)
        ((uint8_t *)p)[i] = 0;
    /* mod_build_adaptive_sync_infopacket_v1: header {00, type 22h, version, 0} */
    if (as_type == RD_FREESYNC_TYPE_PCON_IN_WHITELIST || as_type == RD_ADAPTIVE_SYNC_TYPE_EDP) {
        p->valid = 1;
        p->hb0 = 0x00;
        p->hb1 = 0x22;
        p->hb2 = AS_SDP_VER_1;
        p->hb3 = 0x00;
    }
}

void dcn32_adaptive_sync_sdp_line_num(const rdna3_vrr_stream *s, uint32_t vstartup_start, int *valid, uint32_t *line)
{
    /* blank_start = frame end - front porch; blank_end = blank_start - active (no borders) */
    uint32_t asic_blank_start = s->v_total - s->v_front_porch;
    uint32_t asic_blank_end = asic_blank_start - s->v_addressable;
    if (vstartup_start > asic_blank_end) {
        uint32_t v_update = s->v_total - (vstartup_start - asic_blank_end);
        *valid = 1;
        *line = s->v_total - v_update - 1;
    } else {
        *valid = 0;
        *line = 0;
    }
}

/* ---- amdgpu_dm.c: the CRTC's state ---- */

void dcn32_freesync_update_stream(rdna3_freesync_crtc *crtc, rdna3_vrr_stream *s, const rdna3_freesync_caps *caps,
                                  const rdna3_mode *m, int vrr_enabled)
{
    rdna3_freesync_config config;
    uint32_t i, vrefresh = dcn32_drm_mode_vrefresh(m);
    for (i = 0; i < sizeof(config); i++)
        ((uint8_t *)&config)[i] = 0;
    /* get_freesync_config_for_crtc (no FreeSync video mode) */
    crtc->vrr_supported = caps && caps->freesync_capable && vrefresh >= caps->min_vfreq && vrefresh <= caps->max_vfreq;
    s->ignore_msa_timing_param = 0;
    if (crtc->vrr_supported) {
        s->ignore_msa_timing_param = 1;
        /* (an EDID / VSDB rate above 4294 Hz would wrap the uHz: clamped) */
        config.min_refresh_in_uhz = caps->min_vfreq < 4294 ? caps->min_vfreq * 1000000 : 4294000000u;
        config.max_refresh_in_uhz = caps->max_vfreq < 4294 ? caps->max_vfreq * 1000000 : 4294000000u;
        config.vsif_supported = 1;
        config.btr = 1;
        config.state = vrr_enabled ? RD_VRR_STATE_ACTIVE_VARIABLE : RD_VRR_STATE_INACTIVE;
    }
    crtc->config = config;

    /* update_stream_irq_parameters */
    if (!s->h_total || !s->v_total)
        return;
    if (crtc->vrr_supported && config.min_refresh_in_uhz && config.max_refresh_in_uhz)
        config.state = vrr_enabled ? RD_VRR_STATE_ACTIVE_VARIABLE : RD_VRR_STATE_INACTIVE;
    else
        config.state = RD_VRR_STATE_UNSUPPORTED;
    dcn32_freesync_build_vrr_params(s, &config, &crtc->vrr);
    crtc->config = config;

    /* update_freesync_state_on_stream (no surface: no BTR in preflip) */
    crtc->packet_type = RD_PACKET_TYPE_VRR;
    for (i = 0; i < sizeof(s->adaptive_sync_infopacket); i++)
        ((uint8_t *)&s->adaptive_sync_infopacket)[i] = 0;
    for (i = 0; i < sizeof(s->vrr_infopacket); i++)
        ((uint8_t *)&s->vrr_infopacket)[i] = 0;
    if (caps && caps->as_type == RD_FREESYNC_TYPE_PCON_IN_WHITELIST) {
        if (caps->vsdb_info.amd_vsdb_version == 1)
            crtc->packet_type = RD_PACKET_TYPE_FS_V1;
        else if (caps->vsdb_info.amd_vsdb_version == 2)
            crtc->packet_type = RD_PACKET_TYPE_FS_V2;
        else if (caps->vsdb_info.amd_vsdb_version == 3)
            crtc->packet_type = RD_PACKET_TYPE_FS_V3;
        dcn32_build_adaptive_sync_infopacket(caps->as_type, &s->adaptive_sync_infopacket);
    }
    dcn32_freesync_build_vrr_infopacket(s, &crtc->vrr, crtc->packet_type, &s->vrr_infopacket,
                                        caps ? caps->pack_sdp_v1_3 : 0);
    /* mod_freesync_get_freesync_enabled */
    s->allow_freesync = crtc->vrr.state != RD_VRR_STATE_UNSUPPORTED && crtc->vrr.state != RD_VRR_STATE_DISABLED;
}

int dcn32_freesync_vrr_active(const rdna3_freesync_crtc *crtc)
{
    return crtc->config.state == RD_VRR_STATE_ACTIVE_VARIABLE || crtc->config.state == RD_VRR_STATE_ACTIVE_FIXED;
}

int dcn32_freesync_timing_adjust_needed(const rdna3_freesync_crtc *old_crtc, const rdna3_freesync_crtc *new_crtc)
{
    if (new_crtc->config.state == RD_VRR_STATE_ACTIVE_FIXED)
        return 1;
    return dcn32_freesync_vrr_active(old_crtc) != dcn32_freesync_vrr_active(new_crtc);
}

/* ---- DRR ---- */

void dcn32_set_drr(rdna3_dev *d, uint32_t otg, const rdna3_crtc_timing_adjust *adjust)
{
    rdna3_drr_params params;
    params.vertical_total_max = adjust->v_total_max;
    params.vertical_total_min = adjust->v_total_min;
    params.vertical_total_mid = adjust->v_total_mid;
    params.vertical_total_mid_frame_num = adjust->v_total_mid_frame_num;
    dcn32_otg_set_drr(d, otg, &params);
    if (adjust->v_total_max != 0 && adjust->v_total_min != 0)
        dcn32_otg_set_static_screen_control(d, otg, RD_DRR_EVENT_TRIGGERS, RD_DRR_NUM_FRAMES);
}

int dcn32_stream_adjust_vmin_vmax(rdna3_dev *d, uint32_t otg, const rdna3_crtc_timing_adjust *adjust)
{
    if (adjust->v_total_max > RD_DCN32_MAX_V_TOTAL || adjust->v_total_min > RD_DCN32_MAX_V_TOTAL) {
        /* set_long_vtotal (allow_otg_v_count_halt) is not ported */
        RERR(d, "freesync: OTG %u: v_total %u..%u above the OTG's %u, DRR not set", otg, adjust->v_total_min,
             adjust->v_total_max, RD_DCN32_MAX_V_TOTAL);
        return 0;
    }
    dcn32_set_drr(d, otg, adjust);
    return 1;
}
