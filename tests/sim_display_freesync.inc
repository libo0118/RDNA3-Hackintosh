/*
 * sim_display_freesync.inc - included by sim_navi32.c before sim_wr: the
 * DMCUB's EDID CEA parser (DMUB_CMD__EDID_CEA) as the model of the
 * firmware, and the tests of hw/dcn32_freesync.c (test_dc_freesync, called
 * from run()): the sink's VRR range from the EDID, the FreeSync
 * infopackets byte for byte as amdgpu's freesync.c lays them out, DRR on
 * the OTG as dcn10_set_drr / optc32_set_drr program it, the DP stream's
 * SPD and adaptive sync SDP.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_freesync.h"

static struct {
    uint8_t  cea[128];       /* the CTA block the driver sent */
    unsigned cea_cmds, cea_results;
} SF;

/* the firmware's AMD VSDB search over the CTA data block collection: a
 * vendor-specific block (tag 3) with OUI 00-00-1A; version, feature caps,
 * min and max refresh after the OUI (the layout edid-decode documents) */
static void sf_cea_parse(uint8_t *c)
{
    uint32_t d = SF.cea[2], i, found = 0, ver = 0, lo = 0, hi = 0;
    for (i = 4; i < d && i < 127;) {
        uint32_t tag = SF.cea[i] >> 5, n = SF.cea[i] & 0x1f;
        if (tag == 3 && n >= 7 && i + n < 128 && SF.cea[i + 1] == 0x1A && SF.cea[i + 2] == 0 && SF.cea[i + 3] == 0) {
            found = 1;
            ver = SF.cea[i + 4];
            lo = SF.cea[i + 6];
            hi = SF.cea[i + 7];
            break;
        }
        i += 1 + n;
    }
    c[4] = RD_DMUB_EDID_CEA_AMD_VSDB;
    c[5] = (uint8_t)found;
    c[6] = (uint8_t)found;   /* freesync_supported */
    c[7] = (uint8_t)ver; c[8] = 0;
    c[9] = (uint8_t)lo; c[10] = 0;
    c[11] = (uint8_t)hi; c[12] = 0;
    SF.cea_results++;
}

static int fs_dmub_cmd(uint8_t *c, uint32_t hdr)
{
    uint32_t off, len, total;
    if ((hdr & 0xff) != RD_DMUB_CMD_EDID_CEA || ((hdr >> 24) & 0x3f) != RD_DMUB_EDID_CEA_PAYLOAD)
        return 0;
    off = (uint32_t)c[4] | (uint32_t)c[5] << 8;
    len = c[6];
    total = (uint32_t)c[7] | (uint32_t)c[8] << 8;
    SF.cea_cmds++;
    if (len > RD_DMUB_EDID_CEA_DATA_CHUNK || off + len > sizeof(SF.cea) || total > sizeof(SF.cea)) {
        c[4] = RD_DMUB_EDID_CEA_ACK;
        c[5] = (uint8_t)off; c[6] = (uint8_t)(off >> 8);
        c[7] = 0;
        return 1;
    }
    memcpy(SF.cea + off, c + 9, len);
    if (off + len >= total) {
        sf_cea_parse(c);
    } else {
        c[4] = RD_DMUB_EDID_CEA_ACK;
        c[5] = (uint8_t)off; c[6] = (uint8_t)(off >> 8);
        c[7] = 1;
    }
    return 1;
}

static void sf_sum(uint8_t *b)
{
    uint32_t i, s = 0;
    for (i = 0; i < 127; i++)
        s += b[i];
    b[127] = (uint8_t)(0x100 - (s & 0xff));
}

/* an EDID with one display range descriptor (min / max V Hz, pad2 the
 * EDID 1.4 offset flags, flags = byte 10) in the second slot; with
 * 'vsdb_ver' a CTA extension with an AMD VSDB of that version and range */
static uint32_t sf_edid(uint8_t *e, uint8_t rev, uint8_t features, uint8_t min, uint8_t max, uint8_t pad2,
                        uint8_t flags, uint8_t vsdb_ver, uint8_t vmin, uint8_t vmax)
{
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    uint8_t *t = e + 72, *x = e + 128;
    memset(e, 0, 256);
    memcpy(e, hdr, 8);
    e[8] = 0x10; e[9] = 0xac;   /* "DEL" */
    e[18] = 1;
    e[19] = rev;
    e[20] = 0x80;
    e[24] = features;
    t[3] = 0xfd;
    t[4] = pad2;
    t[5] = min;
    t[6] = max;
    t[7] = 30; t[8] = 160; t[9] = 60;
    t[10] = flags;
    e[126] = vsdb_ver ? 1 : 0;
    sf_sum(e);
    if (!vsdb_ver)
        return 128;
    x[0] = 0x02; x[1] = 0x03;
    x[4] = 3u << 5 | 8;   /* vendor-specific, 8 bytes */
    x[5] = 0x1A; x[6] = 0x00; x[7] = 0x00;
    x[8] = vsdb_ver;
    x[9] = 0x01;
    x[10] = vmin;
    x[11] = vmax;
    x[12] = 0;
    x[2] = 13;            /* the DTDs from byte 13: none */
    sf_sum(x);
    return 256;
}

static uint8_t sf_check(const rdna3_info_packet *p, uint32_t payload)
{
    uint32_t i;
    uint8_t s = (uint8_t)(p->hb0 + p->hb1 + p->hb2 + p->hb3);
    for (i = 0; i <= payload; i++)
        s = (uint8_t)(s + p->sb[i]);
    return s;
}

static void test_dc_freesync(rdna3_dev *dev, rdna3_dmub *dm)
{
    static uint8_t e[256];
    static rdna3_dp_link l;
    rdna3_freesync_caps c;
    rdna3_freesync_crtc crtc, old;
    rdna3_vrr_stream s;
    rdna3_mode m;
    rdna3_info_packet p;
    uint32_t len, lo, hi, k, o = 1;
    unsigned cmds0;
    int valid;

    /* DRM's monitor_range: EDID 1.4, continuous frequency, range limits only */
    len = sf_edid(e, 4, 0x01, 48, 144, 0, 1, 0, 0, 0);
    lo = hi = 0;
    dcn32_drm_get_monitor_range(e, len, &lo, &hi);
    CHECK(lo == 48 && hi == 144);
    /* a GTF variant of the descriptor is not taken; nor EDID 1.3 */
    len = sf_edid(e, 4, 0x01, 48, 144, 0, 0, 0, 0, 0);
    lo = hi = 0;
    dcn32_drm_get_monitor_range(e, len, &lo, &hi);
    CHECK(lo == 0 && hi == 0);
    len = sf_edid(e, 3, 0x01, 48, 144, 0, 1, 0, 0, 0);
    dcn32_drm_get_monitor_range(e, len, &lo, &hi);
    CHECK(lo == 0 && hi == 0);
    /* EDID 1.4's +255 offset of the max */
    len = sf_edid(e, 4, 0x01, 48, 105, 2, 1, 0, 0, 0);
    dcn32_drm_get_monitor_range(e, len, &lo, &hi);
    CHECK(lo == 48 && hi == 360);

    /* amdgpu_dm_update_freesync_caps, DP: MSA_TIMING_PAR_IGNORED and the
     * continuous-frequency range */
    len = sf_edid(e, 4, 0x01, 48, 144, 0, 1, 0, 0, 0);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DISPLAY_PORT, 1, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    CHECK(c.freesync_capable && c.min_vfreq == 48 && c.max_vfreq == 144 && c.from == RD_VRR_FROM_MONITOR_RANGE &&
          c.monitor_min_vfreq == 48 && c.monitor_max_vfreq == 144 && !c.pack_sdp_v1_3);
    /* no MSA ignore: the monitor range only, not capable */
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DISPLAY_PORT, 0, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    CHECK(!c.freesync_capable && c.min_vfreq == 0 && c.monitor_max_vfreq == 144 && c.from == RD_VRR_FROM_NONE);
    /* EDID 1.3 without continuous frequency: amdgpu's own descriptor loop */
    len = sf_edid(e, 3, 0x00, 40, 75, 0, 1, 0, 0, 0);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DISPLAY_PORT, 1, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    CHECK(c.freesync_capable && c.min_vfreq == 40 && c.max_vfreq == 75 && c.from == RD_VRR_FROM_RANGE_DESCRIPTOR);
    len = sf_edid(e, 3, 0x00, 40, 75, 0, 2, 0, 0, 0);   /* secondary GTF: skipped */
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DISPLAY_PORT, 1, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    CHECK(!c.freesync_capable && c.max_vfreq == 0);
    /* a range of 10 Hz is not enough (max - min > 10) */
    len = sf_edid(e, 4, 0x01, 50, 60, 0, 1, 0, 0, 0);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DISPLAY_PORT, 1, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    CHECK(!c.freesync_capable && c.min_vfreq == 50 && c.max_vfreq == 60);
    /* DVI and MST sinks: no caps; no EDID: nothing */
    len = sf_edid(e, 4, 0x01, 48, 144, 0, 1, 0, 0, 0);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DVI_SINGLE_LINK, 1, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    CHECK(!c.freesync_capable && c.monitor_max_vfreq == 144);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DISPLAY_PORT_MST, 1, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    CHECK(!c.freesync_capable);
    dcn32_update_freesync_caps(&c, NULL, 0, RD_SIGNAL_DISPLAY_PORT, 1, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    CHECK(!c.freesync_capable && c.monitor_max_vfreq == 0 && c.hdmi_vsdb == -1);

    /* HDMI: the AMD VSDB through the DMCUB's EDID CEA parser, 16 commands
     * of 8 bytes for the 128-byte CTA block */
    len = sf_edid(e, 3, 0x00, 56, 75, 0, 0, 2, 48, 120);
    cmds0 = SF.cea_cmds;
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_HDMI_TYPE_A, 0, RD_ADAPTIVE_SYNC_TYPE_NONE, dm);
    CHECK(SF.cea_cmds == cmds0 + 16 && !memcmp(SF.cea, e + 128, 128));
    CHECK(c.freesync_capable && c.hdmi_vsdb == 0 && c.min_vfreq == 48 && c.max_vfreq == 120 &&
          c.from == RD_VRR_FROM_AMD_VSDB && c.monitor_min_vfreq == 48 && c.monitor_max_vfreq == 120 &&
          c.as_type == RD_ADAPTIVE_SYNC_TYPE_NONE);
    /* no DMCUB: no parser, no HDMI FreeSync; no AMD VSDB: none either */
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_HDMI_TYPE_A, 0, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    CHECK(!c.freesync_capable && c.hdmi_vsdb == -1);
    e[128 + 5] = 0x1B;   /* another OUI */
    sf_sum(e + 128);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_HDMI_TYPE_A, 0, RD_ADAPTIVE_SYNC_TYPE_NONE, dm);
    CHECK(!c.freesync_capable && c.hdmi_vsdb == -1 && SF.cea_cmds == cmds0 + 32);

    /* a whitelisted DP-HDMI PCON: the HDMI sink's AMD VSDB, SDP 1.3 packing */
    len = sf_edid(e, 3, 0x00, 56, 75, 0, 0, 3, 48, 144);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DISPLAY_PORT, 1, RD_FREESYNC_TYPE_PCON_IN_WHITELIST, dm);
    CHECK(c.freesync_capable && c.pack_sdp_v1_3 && c.as_type == RD_FREESYNC_TYPE_PCON_IN_WHITELIST &&
          c.vsdb_info.amd_vsdb_version == 3 && c.min_vfreq == 48 && c.max_vfreq == 144 &&
          c.from == RD_VRR_FROM_PCON_VSDB);

    /* the stream: 2560x1440 at exactly 144 Hz (h_total 2720, v_total 1525) */
    memset(&m, 0, sizeof(m));
    m.h_active = 2560; m.v_active = 1440; m.h_total = 2720; m.v_total = 1525;
    m.v_front = 3; m.v_sync = 5;
    m.pixel_khz = 2720u * 1525u * 144u / 1000u;
    CHECK(dcn32_drm_mode_vrefresh(&m) == 144);
    dcn32_vrr_stream_from_mode(&s, &m, RD_SIGNAL_HDMI_TYPE_A);
    CHECK(dcn32_freesync_calc_nominal_field_rate(&s) == 144000000ull);
    /* vmin at 144 Hz is the timing's own, vmax at 48 Hz three times it */
    CHECK(dcn32_freesync_calc_v_total_from_refresh(&s, 144000000) == 1525 &&
          dcn32_freesync_calc_v_total_from_refresh(&s, 48000000) == 4575 &&
          dcn32_freesync_calc_v_total_from_refresh(&s, 200000000) == 1525);

    /* HDMI FreeSync 48-120 at a 144 Hz mode: out of range, unsupported */
    len = sf_edid(e, 3, 0x00, 56, 75, 0, 0, 2, 48, 120);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_HDMI_TYPE_A, 0, RD_ADAPTIVE_SYNC_TYPE_NONE, dm);
    memset(&crtc, 0, sizeof(crtc));
    dcn32_freesync_update_stream(&crtc, &s, &c, &m, 1);
    /* (zeroed params are UNSUPPORTED already: vrr_settings_require_update
     * finds nothing to change, the adjust stays zero as in amdgpu) */
    CHECK(!crtc.vrr_supported && crtc.config.state == RD_VRR_STATE_UNSUPPORTED && !s.vrr_infopacket.valid &&
          !s.ignore_msa_timing_param && !s.allow_freesync && crtc.vrr.adjust.v_total_min == 0 &&
          !crtc.vrr.send_info_frame && !dcn32_freesync_vrr_active(&crtc));

    /* HDMI 48-144, VRR on: vmin 1525 (144 Hz), vmax 4575 (48 Hz), the SPD
     * v1 of an active FreeSync stream */
    len = sf_edid(e, 3, 0x00, 56, 75, 0, 0, 2, 48, 144);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_HDMI_TYPE_A, 0, RD_ADAPTIVE_SYNC_TYPE_NONE, dm);
    memset(&crtc, 0, sizeof(crtc));
    dcn32_freesync_update_stream(&crtc, &s, &c, &m, 1);
    CHECK(crtc.vrr_supported && crtc.config.state == RD_VRR_STATE_ACTIVE_VARIABLE && crtc.vrr.supported &&
          crtc.vrr.state == RD_VRR_STATE_ACTIVE_VARIABLE && crtc.vrr.min_refresh_in_uhz == 48000000 &&
          crtc.vrr.max_refresh_in_uhz == 144000000 && crtc.vrr.adjust.v_total_min == 1525 &&
          crtc.vrr.adjust.v_total_max == 4575 && !crtc.vrr.adjust.allow_otg_v_count_halt &&
          crtc.vrr.btr.btr_enabled && crtc.vrr.min_duration_in_us == 6944 && crtc.vrr.max_duration_in_us == 20833 &&
          crtc.vrr.btr.margin_in_us == 2500 && s.ignore_msa_timing_param && s.allow_freesync &&
          crtc.packet_type == RD_PACKET_TYPE_VRR);
    p = s.vrr_infopacket;
    CHECK(p.valid && p.hb0 == 0x83 && p.hb1 == 0x01 && p.hb2 == 0x08 && p.hb3 == 0 && p.sb[1] == 0x1A &&
          p.sb[2] == 0 && p.sb[3] == 0 && p.sb[4] == 0 && p.sb[5] == 0 && p.sb[6] == 0x07 && p.sb[7] == 48 &&
          p.sb[8] == 144 && p.sb[0] == 0x93 && sf_check(&p, 8) == 0 && p.sb[9] == 0);
    CHECK(dcn32_freesync_vrr_active(&crtc));
    memset(&old, 0, sizeof(old));
    CHECK(dcn32_freesync_timing_adjust_needed(&old, &crtc));
    /* the same again: nothing to update, nothing to adjust */
    old = crtc;
    dcn32_freesync_update_stream(&crtc, &s, &c, &m, 1);
    CHECK(!dcn32_freesync_timing_adjust_needed(&old, &crtc) && crtc.vrr.adjust.v_total_max == 4575);
    /* VRR off (vrr_enabled 0): INACTIVE, the nominal v_total, the SPD says
     * supported + enabled with the nominal range */
    dcn32_freesync_update_stream(&crtc, &s, &c, &m, 0);
    CHECK(crtc.vrr.state == RD_VRR_STATE_INACTIVE && crtc.vrr.adjust.v_total_min == 1525 &&
          crtc.vrr.adjust.v_total_max == 1525 && dcn32_freesync_timing_adjust_needed(&old, &crtc));
    p = s.vrr_infopacket;
    CHECK(p.valid && p.sb[6] == 0x03 && p.sb[7] == 144 && p.sb[8] == 144 && sf_check(&p, 8) == 0);

    /* DP: the SPD as an INFOFRAME SDP (hb 00 83 1B 04, 27 payload bytes) */
    dcn32_vrr_stream_from_mode(&s, &m, RD_SIGNAL_DISPLAY_PORT);
    len = sf_edid(e, 4, 0x01, 48, 144, 0, 1, 0, 0, 0);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DISPLAY_PORT, 1, RD_ADAPTIVE_SYNC_TYPE_NONE, NULL);
    memset(&crtc, 0, sizeof(crtc));
    dcn32_freesync_update_stream(&crtc, &s, &c, &m, 1);
    p = s.vrr_infopacket;
    CHECK(p.valid && p.hb0 == 0x00 && p.hb1 == 0x83 && p.hb2 == 0x1B && p.hb3 == 0x04 && p.sb[1] == 0x1A &&
          p.sb[6] == 0x07 && p.sb[7] == 48 && p.sb[8] == 144 && sf_check(&p, 0x1B) == 0 &&
          !s.adaptive_sync_infopacket.valid && s.ignore_msa_timing_param);

    /* a whitelisted PCON with an AMD VSDB v3: SPD v3, packed as SDP 1.3
     * (version, length, then the payload moved up one byte); the adaptive
     * sync SDP v1 header */
    len = sf_edid(e, 3, 0x00, 56, 75, 0, 0, 3, 48, 144);
    dcn32_update_freesync_caps(&c, e, len, RD_SIGNAL_DISPLAY_PORT, 1, RD_FREESYNC_TYPE_PCON_IN_WHITELIST, dm);
    memset(&crtc, 0, sizeof(crtc));
    dcn32_freesync_update_stream(&crtc, &s, &c, &m, 1);
    p = s.vrr_infopacket;
    CHECK(crtc.packet_type == RD_PACKET_TYPE_FS_V3 && p.valid && p.hb0 == 0 && p.hb1 == 0x83 && p.hb2 == 0x1D &&
          p.hb3 == (0x13 << 2) && p.sb[0] == 3 && p.sb[1] == 0x10 && p.sb[2] == 0x1A && p.sb[7] == 0x07 &&
          p.sb[8] == 48 && p.sb[9] == 144 && p.sb[12] == 0 && p.sb[13] == 0 && p.sb[17] == 0);
    p = s.adaptive_sync_infopacket;
    CHECK(p.valid && p.hb0 == 0 && p.hb1 == 0x22 && p.hb2 == 0x01 && p.hb3 == 0 && p.sb[0] == 0);
    /* FreeSync v3 on HDMI (no packing): hb 83 03 10, 16 payload bytes */
    {
        rdna3_vrr_stream h = s;
        memset(&p, 0, sizeof(p));
        h.signal = RD_SIGNAL_HDMI_TYPE_A;
        dcn32_freesync_build_vrr_infopacket(&h, &crtc.vrr, RD_PACKET_TYPE_FS_V3, &p, 1);
        CHECK(p.hb0 == 0x83 && p.hb1 == 3 && p.hb2 == 0x10 && p.sb[7] == 48 && p.sb[8] == 144 && p.sb[11] == 0 &&
              p.sb[12] == 0 && p.sb[16] == 0 && sf_check(&p, 0x10) == 0);
        /* v2: hb 83 02 09 */
        memset(&p, 0, sizeof(p));
        dcn32_freesync_build_vrr_infopacket(&h, &crtc.vrr, RD_PACKET_TYPE_FS_V2, &p, 0);
        CHECK(p.hb1 == 2 && p.hb2 == 0x09 && sf_check(&p, 9) == 0);
    }

    /* the adaptive sync SDP's line: VSTARTUP past the blank end (1440
     * active, 3 lines front porch of 1525) */
    dcn32_adaptive_sync_sdp_line_num(&s, 100, &valid, &k);
    CHECK(valid && k == 17);
    dcn32_adaptive_sync_sdp_line_num(&s, 50, &valid, &k);
    CHECK(!valid && k == 0);

    /* the DP stream's packets: SPD in VPG packet 2 (GSP2), the adaptive
     * sync SDP in packet 5 (GSP5) on its line */
    {
        static rdna3_dp_info_frames f;
        const uint32_t sec = S.reg[DPR(regDP0_DP_SEC_CNTL, 2)];
        memset(S.vpg, 0, sizeof(S.vpg));
        dcn32_dp_link_init(&l, dev, dm, 0, 1, 2, 2, 0, 0x13, 0x1e, 0, 0);
        l.vrr_infopacket = s.vrr_infopacket;
        l.adaptive_sync_infopacket = s.adaptive_sync_infopacket;
        l.vstartup_start = 100;
        dcn32_dp_build_info_frames(&l, &m, &f);
        CHECK(f.spd.valid && !memcmp(&f.spd, &s.vrr_infopacket, sizeof(f.spd)) && f.adaptive_sync.valid &&
              f.adaptive_sync_line_num_valid && f.adaptive_sync_line_num == 17 && !f.vsc.valid && !f.hdrsmd.valid);
        S.reg[DPR(regDP0_DP_SEC_CNTL, 2)] = 0;
        dcn32_enc_update_dp_info_packets_sdp_line_num(&l, &f);
        dcn32_enc_update_dp_info_packets(&l, &f);
        CHECK((S.reg[DPR(regDP0_DP_SEC_CNTL, 2)] & (DP0_DP_SEC_CNTL__DP_SEC_GSP2_ENABLE_MASK |
                                                  DP0_DP_SEC_CNTL__DP_SEC_GSP5_ENABLE_MASK |
                                                  DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK)) ==
                  (DP0_DP_SEC_CNTL__DP_SEC_GSP2_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_GSP5_ENABLE_MASK |
                   DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK) &&
              ((S.reg[DPR(regDP0_DP_SEC_CNTL5, 2)] & DP0_DP_SEC_CNTL5__DP_SEC_GSP5_LINE_NUM_MASK) >>
               DP0_DP_SEC_CNTL5__DP_SEC_GSP5_LINE_NUM__SHIFT) == 17 &&
              S.vpg[2].mem[18] == (0x83u << 8 | 0x1Du << 16 | 0x4Cu << 24) &&
              S.vpg[2].mem[45] == (0x22u << 8 | 0x01u << 16));
        /* no FreeSync: no SPD, no adaptive sync SDP */
        memset(&l.vrr_infopacket, 0, sizeof(l.vrr_infopacket));
        memset(&l.adaptive_sync_infopacket, 0, sizeof(l.adaptive_sync_infopacket));
        dcn32_dp_build_info_frames(&l, &m, &f);
        CHECK(!f.spd.valid && !f.adaptive_sync.valid && !f.adaptive_sync_line_num_valid);
        S.reg[DPR(regDP0_DP_SEC_CNTL, 2)] = sec;
        memset(S.vpg, 0, sizeof(S.vpg));
    }

    /* DRR: dc_stream_adjust_vmin_vmax -> dcn10_set_drr -> optc32_set_drr:
     * V_TOTAL_MIN / MAX = v_total - 1, MIN_SEL / MAX_SEL, TRIGA ends the
     * stretched blank; the static screen trigger on OTG_TRIG_A for 2 frames */
    S.reg[OTG(regOTG0_OTG_V_TOTAL_CONTROL, o)] = 0;
    S.reg[OTG(regOTG0_OTG_STATIC_SCREEN_CONTROL, o)] = 0;
    CHECK(dcn32_stream_adjust_vmin_vmax(dev, o, &old.vrr.adjust));
    CHECK(S.reg[OTG(regOTG0_OTG_V_TOTAL_MIN, o)] == 1524 && S.reg[OTG(regOTG0_OTG_V_TOTAL_MAX, o)] == 4574);
    k = S.reg[OTG(regOTG0_OTG_V_TOTAL_CONTROL, o)];
    CHECK((k & OTG0_OTG_V_TOTAL_CONTROL__OTG_V_TOTAL_MIN_SEL_MASK) && (k & OTG0_OTG_V_TOTAL_CONTROL__OTG_V_TOTAL_MAX_SEL_MASK) &&
          !(k & OTG0_OTG_V_TOTAL_CONTROL__OTG_VTOTAL_MID_REPLACING_MAX_EN_MASK) &&
          ((k & OTG0_OTG_V_TOTAL_CONTROL__OTG_SET_V_TOTAL_MIN_MASK_MASK) >> OTG0_OTG_V_TOTAL_CONTROL__OTG_SET_V_TOTAL_MIN_MASK__SHIFT) == 2);
    k = S.reg[OTG(regOTG0_OTG_STATIC_SCREEN_CONTROL, o)];
    CHECK(((k & OTG0_OTG_STATIC_SCREEN_CONTROL__OTG_STATIC_SCREEN_EVENT_MASK_MASK) >>
           OTG0_OTG_STATIC_SCREEN_CONTROL__OTG_STATIC_SCREEN_EVENT_MASK__SHIFT) == RD_DRR_EVENT_TRIGGERS &&
          ((k & OTG0_OTG_STATIC_SCREEN_CONTROL__OTG_STATIC_SCREEN_FRAME_COUNT_MASK) >>
           OTG0_OTG_STATIC_SCREEN_CONTROL__OTG_STATIC_SCREEN_FRAME_COUNT__SHIFT) == RD_DRR_NUM_FRAMES);
    /* back to the nominal (VRR off): min = max = v_total */
    {
        rdna3_crtc_timing_adjust a;
        memset(&a, 0, sizeof(a));
        a.v_total_min = a.v_total_max = 1525;
        CHECK(dcn32_stream_adjust_vmin_vmax(dev, o, &a) && S.reg[OTG(regOTG0_OTG_V_TOTAL_MIN, o)] == 1524 &&
              S.reg[OTG(regOTG0_OTG_V_TOTAL_MAX, o)] == 1524);
        /* above DCN 3.2's 15-bit v_total: refused (no set_long_vtotal), nothing written */
        a.v_total_max = RD_DCN32_MAX_V_TOTAL + 1;
        CHECK(!dcn32_stream_adjust_vmin_vmax(dev, o, &a) && S.reg[OTG(regOTG0_OTG_V_TOTAL_MAX, o)] == 1524);
        /* zero (DRR off): only the manual trigger set up, the static screen untouched */
        S.reg[OTG(regOTG0_OTG_STATIC_SCREEN_CONTROL, o)] = 0;
        memset(&a, 0, sizeof(a));
        dcn32_set_drr(dev, o, &a);
        CHECK(S.reg[OTG(regOTG0_OTG_STATIC_SCREEN_CONTROL, o)] == 0 && S.reg[OTG(regOTG0_OTG_V_TOTAL_MAX, o)] == 1524);
    }
    printf("sim_navi32: FreeSync tests ran (%u EDID CEA commands, %u parser results)\n", SF.cea_cmds, SF.cea_results);
}
