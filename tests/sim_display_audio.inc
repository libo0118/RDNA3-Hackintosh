/*
 * sim_display_audio.inc - included by sim_navi32.c after sim_display_hpo.inc:
 * the model and tests of the display audio (stage 4 item N, rdna3audio)
 * and of the output pixel formats (rdna3fmt).
 *
 * Model:
 *   - the azalia endpoints AZF0ENDPOINT0..7: AZALIA_F0_CODEC_ENDPOINT_INDEX
 *     selects one of 256 endpoint registers per pin, _DATA reads / writes
 *     it; pins 0..3 have a connection, pin 4 reports PORT_CONNECTIVITY 1
 *     ("no physical connection": DC stops counting there)
 *   - the APGs 0..3: APG_RESET_DONE follows APG_RESET
 *   - the DPGs: DPG_DOUBLE_BUFFER_PENDING stays 0 (the update is taken at once)
 *   - the DDC GPIO pins of dp_is_sink_present: DC_GPIO_DDCn_Y reads the
 *     clock line's level (SA.ddc_clk_level[n]: 0 = a DP sink or a passive
 *     DP-HDMI dongle holds it low), sampled only when the pin is a GPIO
 *     input (MASK 1, EN 0) on a pad in I2C mode (AUX_PAD1_MODE 0,
 *     DDC_PADn_I2CMODE 1, AUXn_PAD_RXSEL 1); other reads count as bad
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_audio.h"
#include "dcn32_opp.h"
#include "gen/dcn_3_2_0_audio.h"
#include "gen/dcn_3_2_0_fmt.h"
#include "gen/dcn_3_2_0_color.h"
#include "gen/dcn_3_2_0_ddcgpio.h"
#include "gen/dcn_3_2_0_gpio.h"

#define SEGB(i) ((i) == 1 ? DCN_B1 : (i) == 2 ? DCN_B2 : DCN_B3)
#define AREG(reg) (SEGB(reg##_BASE_IDX) + (reg))
#define AZ_ST (regAZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_INDEX - regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX)
#define APG_ST (regAPG1_APG_CONTROL - regAPG0_APG_CONTROL)
#define FE_ST (regDIG1_AFMT_CNTL - regDIG0_AFMT_CNTL)
#define FMT_ST (regFMT1_FMT_CONTROL - regFMT0_FMT_CONTROL)

#define DDC_ST (regDC_GPIO_DDC2_MASK - regDC_GPIO_DDC1_MASK)

static struct {
    uint32_t az[8][256];
    unsigned az_writes;
    uint32_t ddc_clk_level[5];
    unsigned ddc_y_reads, ddc_y_bad;
} SA;

static int au_rd(uint32_t dw, uint32_t *out)
{
    uint32_t n;
    for (n = 0; n < 5; n++)
        if (dw == AREG(regDC_GPIO_DDC1_Y) + DDC_ST * n) {
            uint32_t mask = S.reg[AREG(regDC_GPIO_DDC1_MASK) + DDC_ST * n];
            uint32_t en = S.reg[AREG(regDC_GPIO_DDC1_EN) + DDC_ST * n];
            int input = (mask & DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_MASK_MASK) &&
                        !(en & DC_GPIO_DDC1_EN__DC_GPIO_DDC1CLK_EN_MASK) &&
                        !(mask & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK) &&
                        (S.reg[AREG(regDC_GPIO_AUX_CTRL_5)] & (DC_GPIO_AUX_CTRL_5__DDC_PAD1_I2CMODE_MASK << n)) &&
                        ((S.reg[AREG(regPHY_AUX_CNTL)] >> (PHY_AUX_CNTL__AUX1_PAD_RXSEL__SHIFT + 2 * n)) & 3) == 1;
            SA.ddc_y_reads++;
            if (!input)
                SA.ddc_y_bad++;
            *out = input && SA.ddc_clk_level[n] ? DC_GPIO_DDC1_Y__DC_GPIO_DDC1CLK_Y_MASK : 0;
            return 1;
        }
    for (n = 0; n < 8; n++)
        if (dw == AREG(regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA) + AZ_ST * n) {
            *out = SA.az[n][S.reg[AREG(regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX) + AZ_ST * n] & 0xff];
            return 1;
        }
    for (n = 0; n < RD_HPO_DP_STREAM_ENCS; n++)
        if (dw == AREG(regAPG0_APG_CONTROL) + APG_ST * n) {
            *out = follow(S.reg[dw], APG0_APG_CONTROL__APG_RESET_MASK, APG0_APG_CONTROL__APG_RESET_DONE_MASK);
            return 1;
        }
    return 0;
}

static int au_wr(uint32_t dw, uint32_t v)
{
    uint32_t n;
    for (n = 0; n < 8; n++)
        if (dw == AREG(regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA) + AZ_ST * n) {
            SA.az[n][S.reg[AREG(regAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX) + AZ_ST * n] & 0xff] = v;
            SA.az_writes++;
            return 1;
        }
    return 0;
}

/* the azalia pins as the card has them: 0..3 connected, 4 not */
static void au_reset(void)
{
    memset(&SA, 0, sizeof(SA));
    SA.az[4][RD_AZ_RESPONSE_CONFIGURATION_DEFAULT] = 1u << 30;
}

/* the synthetic HDMI sink of test_dc_hdmi with a monitor name ("SIM TV"),
 * an 8 channel LPCM descriptor and AC-3, 5.1 speakers, deep color 30 / 36
 * bit and YCbCr 4:4:4 */
static void au_test_edid(uint8_t *b)
{
    static const uint8_t name[18] = { 0, 0, 0, 0xfc, 0, 'S', 'I', 'M', ' ', 'T', 'V', 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };
    static const uint8_t cta[] = {
        0x43, 16, 97, 95,                                  /* video: VIC 16, 97, 95 */
        0x26, 0x0f, 0x7f, 0x07, 0x15, 0x07, 0x50,          /* audio: LPCM 8 ch 32-192 kHz 16-24 bit; AC-3 6 ch */
        0x83, 0x0f, 0x00, 0x00,                            /* speaker allocation: FL/FR, LFE, FC, RL/RR */
        0x67, 0x03, 0x0C, 0x00, 0x10, 0x00, 0x30, 0x3C,    /* HDMI VSDB: DC_30, DC_36, 300 MHz */
    };
    uint32_t i, s;
    hdmi_test_edid(b, 0x4D2D, 0x1234);   /* "SIM" */
    memcpy(b + 72, name, 18);
    b[19] = 3;     /* EDID 1.3: the CTA block's colour caps */
    for (i = 0, s = 0; i < 127; i++)
        s += b[i];
    b[127] = (uint8_t)(0x100 - (s & 0xff));
    memset(b + 128, 0, 128);
    b[128] = 0x02;
    b[129] = 0x03;
    b[130] = (uint8_t)(4 + sizeof(cta));
    b[131] = 0x70 | 0x20;   /* basic audio, YCbCr 4:4:4 (and 4:2:2) */
    memcpy(b + 132, cta, sizeof(cta));
    for (i = 128, s = 0; i < 255; i++)
        s += b[i];
    b[255] = (uint8_t)(0x100 - (s & 0xff));
}

static uint32_t afmt(uint32_t reg, uint32_t fe)
{
    return S.reg[DCN_B2 + reg + FE_ST * fe];
}

/* HDMI 1.4 7.2.3: 128 x fs x CTS = f_TMDS x N, exactly for the
 * integer-kHz clocks of the tables */
static int acr_exact(uint32_t pclk_10khz, uint32_t n, uint32_t cts, uint32_t fs)
{
    return (uint64_t)128 * fs * cts == (uint64_t)pclk_10khz * 10000 * n;
}

static void test_dc_audio_units(rdna3_dev *dev)
{
    static rdna3_edid e;
    rdna3_audio_info ai;
    rdna3_audio_clock_info ci;
    rdna3_audio_crtc_info c;
    rdna3_audio_dp_link_info li;
    uint8_t edid[256];
    static const uint32_t exact[] = { 2520, 2700, 5400, 7425, 14850 };
    uint32_t i;

    /* get_audio_clock_info: the HDMI tables (N / CTS for 32 / 44.1 / 48 kHz) */
    dcn32_get_audio_clock_info(RD_COLOR_DEPTH_888, 742500, 742500, &ci);
    CHECK(ci.n_32khz == 4096 && ci.cts_32khz == 74250 && ci.n_44khz == 6272 && ci.cts_44khz == 82500 &&
          ci.n_48khz == 6144 && ci.cts_48khz == 74250);
    dcn32_get_audio_clock_info(RD_COLOR_DEPTH_888, 1483500, 1483500, &ci);   /* 148.5 / 1.001 */
    CHECK(ci.n_32khz == 11648 && ci.cts_32khz == 421875 && ci.n_44khz == 8918 && ci.cts_44khz == 234375 &&
          ci.n_48khz == 5824 && ci.cts_48khz == 140625);
    dcn32_get_audio_clock_info(RD_COLOR_DEPTH_888, 5940000, 5940000, &ci);   /* 594 MHz */
    CHECK(ci.n_32khz == 3072 && ci.cts_32khz == 445500 && ci.n_44khz == 9408 && ci.cts_44khz == 990000 &&
          ci.n_48khz == 6144 && ci.cts_48khz == 594000);
    dcn32_get_audio_clock_info(RD_COLOR_DEPTH_888, 252000, 0, &ci);
    CHECK(ci.n_48khz == 6144 && ci.cts_48khz == 25200 && ci.n_44khz == 6272 && ci.cts_44khz == 28000);
    /* the 36 bpc table at 148.5 MHz (222.75 MHz TMDS) */
    dcn32_get_audio_clock_info(RD_COLOR_DEPTH_121212, 1485000, 1485000, &ci);
    CHECK(ci.n_32khz == 4096 && ci.cts_32khz == 222750 && ci.n_48khz == 6144 && ci.cts_48khz == 222750 &&
          acr_exact(22275, ci.n_48khz, ci.cts_48khz, 48000) && acr_exact(22275, ci.n_44khz, ci.cts_44khz, 44100));
    /* 30 bpc has no table: the 24 bpc entries */
    dcn32_get_audio_clock_info(RD_COLOR_DEPTH_101010, 1485000, 1485000, &ci);
    CHECK(ci.cts_48khz == 148500);
    /* "Other" clocks: N 4096 / 6272 / 6144, CTS = the clock in kHz */
    dcn32_get_audio_clock_info(RD_COLOR_DEPTH_888, 2415000, 2415000, &ci);
    CHECK(ci.n_32khz == 4096 && ci.n_44khz == 6272 && ci.n_48khz == 6144 && ci.cts_32khz == 241500 &&
          ci.cts_48khz == 241500 && acr_exact(24150, 4096, 241500, 32000));
    for (i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        dcn32_get_audio_clock_info(RD_COLOR_DEPTH_888, exact[i] * 100, exact[i] * 100, &ci);
        CHECK(ci.pixel_clock_in_10khz == exact[i] && acr_exact(exact[i], ci.n_32khz, ci.cts_32khz, 32000) &&
              acr_exact(exact[i], ci.n_44khz, ci.cts_44khz, 44100) && acr_exact(exact[i], ci.n_48khz, ci.cts_48khz, 48000));
    }

    /* speakers_to_channels */
    CHECK(dcn32_audio_speakers_to_channels(RD_SPK_FL_FR) == 0x03);
    CHECK(dcn32_audio_speakers_to_channels(0x0f) == 0x3f);
    CHECK(dcn32_audio_speakers_to_channels(0x1f) == 0x3f);   /* RC moved to 7, then RLC_RRC's 0 wins */
    CHECK(dcn32_audio_speakers_to_channels(RD_SPK_FL_FR | RD_SPK_RC) == 0x13);
    CHECK(dcn32_audio_speakers_to_channels(0x4f) == 0xff);
    CHECK(dcn32_audio_speakers_to_channels(RD_SPK_FL_FR | RD_SPK_FLC_FRC) == 0xc3);

    /* dm_helpers_parse_edid_caps + dc_stream_construct */
    au_test_edid(edid);
    CHECK(edid_parse(edid, 256, &e) == 0 && e.audio_mode_count == 2 && e.speaker_flags == 0x0f && e.hdmi);
    dcn32_audio_info_from_edid(&e, &ai);
    CHECK(ai.manufacture_id == 0x2D4D && ai.product_id == 0x1234 && ai.mode_count == 2 && ai.flags == 0x0f &&
          ai.modes[0].format_code == RD_AUDIO_FORMAT_CODE_LINEARPCM && ai.modes[0].channel_count == 8 &&
          ai.modes[0].sample_rates == 0x7f && ai.modes[0].sample_size == 0x07 &&
          ai.modes[1].format_code == RD_AUDIO_FORMAT_CODE_AC3 && ai.modes[1].sample_size == 0x50 &&
          memcmp(ai.display_name, "SIM TV", 7) == 0 && ai.port_id[0] == 0x5558859e && ai.port_id[1] == 0xd989449);
    CHECK(dcn32_audio_wanted(RD_SIGNAL_HDMI_TYPE_A, &ai) && dcn32_audio_wanted(RD_SIGNAL_DISPLAY_PORT, &ai) &&
          !dcn32_audio_wanted(RD_SIGNAL_DVI_SINGLE_LINK, &ai));
    /* the colour caps: EDID 1.3, CTA 4:4:4 + 4:2:2, HDMI deep color 36 bit */
    CHECK(e.color_formats == (RD_COLOR_FORMAT_RGB444 | RD_COLOR_FORMAT_YCBCR444 | RD_COLOR_FORMAT_YCBCR422) &&
          e.bpc == 12 && e.hdmi_dc_modes == (RD_EDID_HDMI_DC_30 | RD_EDID_HDMI_DC_36) && !e.hdmi_dc_y444 &&
          e.max_tmds_clk_mhz == 300);
    /* EDID 1.4: the input's colour depth (10 bpc) overrides the VSDB's */
    edid[19] = 4;
    edid[20] = 0x80 | (3u << 4);
    edid[24] = 0x18 | 0x02;
    {
        uint32_t s2, k;
        for (k = 0, s2 = 0; k < 127; k++)
            s2 += edid[k];
        edid[127] = (uint8_t)(0x100 - (s2 & 0xff));
    }
    CHECK(edid_parse(edid, 256, &e) == 0 && e.bpc == 10 &&
          e.color_formats == (RD_COLOR_FORMAT_RGB444 | RD_COLOR_FORMAT_YCBCR444 | RD_COLOR_FORMAT_YCBCR422));

    /* check_audio_bandwidth_hdmi: 480p (27 MHz, 720 x 480) limits 8 ch to
     * 48 kHz (HDMI 1.3 table 7.5); 2 ch keeps what the sink has */
    memset(&c, 0, sizeof(c));
    c.h_total = 858; c.h_active = 720; c.v_active = 480; c.pixel_repetition = 1; c.refresh_rate = 60;
    c.requested_pixel_clock_100hz = c.calculated_pixel_clock_100hz = 270000; c.color_depth = RD_COLOR_DEPTH_888;
    CHECK(dcn32_audio_check_bandwidth(&c, NULL, 8, RD_SIGNAL_HDMI_TYPE_A, 0x7f) == 0x07);
    CHECK(dcn32_audio_check_bandwidth(&c, NULL, 2, RD_SIGNAL_HDMI_TYPE_A, 0x7f) == 0x07);   /* the blank's budget */
    c.h_total = 2200; c.h_active = 1920; c.v_active = 1080;
    c.requested_pixel_clock_100hz = c.calculated_pixel_clock_100hz = 1485000;
    CHECK(dcn32_audio_check_bandwidth(&c, NULL, 8, RD_SIGNAL_HDMI_TYPE_A, 0x7f) == 0x7f);
    /* DP SST 8b/10b: not checked; 128b/132b: the SDP budget of the blank */
    memset(&li, 0, sizeof(li));
    li.encoding = RD_DP_8b_10b_ENCODING; li.link_rate = RD_DP_LINK_RATE_HIGH2; li.lane_count = 4;
    CHECK(dcn32_audio_check_bandwidth(&c, &li, 8, RD_SIGNAL_DISPLAY_PORT, 0x7f) == 0x7f);
    li.encoding = RD_DP_128b_132b_ENCODING; li.link_rate = RD_DP_LINK_RATE_UHBR10; li.link_bandwidth_kbps = 38680000;
    CHECK(dcn32_audio_check_bandwidth(&c, &li, 8, RD_SIGNAL_DISPLAY_PORT, 0x7f) == 0x7f);
    li.lane_count = 1; li.link_bandwidth_kbps = 200000000;   /* a narrow share of the link symbols: nothing fits */
    CHECK((dcn32_audio_check_bandwidth(&c, &li, 8, RD_SIGNAL_DISPLAY_PORT, 0x7f) & RD_AUDIO_RATE_192) == 0);

    /* dccg31_set_audio_dtbclk_dto: phase / modulo = audio clock / DTBCLK */
    S.reg[AREG(regDCCG_AUDIO_DTO_SOURCE)] = 0;
    dcn32_dccg_set_audio_dtbclk_dto(dev, 600000, 24576);
    CHECK(S.reg[AREG(regDCCG_AUDIO_DTBCLK_DTO_MODULO)] == 600000000u && S.reg[AREG(regDCCG_AUDIO_DTBCLK_DTO_PHASE)] == 24576000u &&
          ((S.reg[AREG(regDCCG_AUDIO_DTO_SOURCE)] & DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL_MASK) >>
           DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL__SHIFT) == 4);
    dcn32_dccg_set_audio_dtbclk_dto(dev, 0, 0);
    CHECK(S.reg[AREG(regDCCG_AUDIO_DTBCLK_DTO_MODULO)] == 0 && S.reg[AREG(regDCCG_AUDIO_DTBCLK_DTO_PHASE)] == 0 &&
          ((S.reg[AREG(regDCCG_AUDIO_DTO_SOURCE)] & DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL_MASK) >>
           DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL__SHIFT) == 3);
    /* dccg31_init: every HPO stream encoder's SYMCLK32 back on REFCLK */
    S.reg[DCN_B1 + regSYMCLK32_SE_CNTL] = 0x3333;
    dcn32_dccg_init(dev);
    CHECK(S.reg[DCN_B1 + regSYMCLK32_SE_CNTL] == 0);
}

/* the endpoint pool, HDMI / DP / 128b/132b audio through the encoders */
static void test_dc_audio_streams(rdna3_dev *dev)
{
    static rdna3_edid e;
    static rdna3_audio_stream s;
    rdna3_audio_pool pool;
    uint8_t edid[256], enc_acq[RD_AUDIO_MAX_ENDPOINTS] = { 0 };
    rdna3_mode m1080, m4k;
    const rdna3_crtc_format rgb = { RD_PIXEL_ENCODING_RGB, RD_COLOR_DEPTH_888, 0, 0 };
    rdna3_dp_link_settings ls;
    uint32_t v, fe = 2, i;
    static uint32_t save[0x300];

    au_reset();
    for (i = 0; i < 0x300; i++)
        save[i] = S.reg[DCN_B2 + regDIG0_AFMT_CNTL - 0x60 + i];
    au_test_edid(edid);
    CHECK(edid_parse(edid, 256, &e) == 0);

    /* resource_construct: 4 pins (pin 4 has no connection), the straps */
    S.reg[AREG(regDC_PINSTRAPS)] = 2u << DC_PINSTRAPS__DC_PINSTRAPS_AUDIO__SHIFT;
    dcn32_audio_pool_init(dev, &pool);
    CHECK(pool.count == 4 && pool.hdmi_audio_native && pool.dp_audio);
    /* dce_aud_hw_init on pin 0: rates R5-R7, CLKSTOP / EPSS */
    CHECK((S.reg[AREG(regAZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES)] & 0xfff) == 0x70 &&
          (S.reg[AREG(regAZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES)] & 0xc0000000u) == 0xc0000000u &&
          !(SA.az[0][RD_AZ_HOT_PLUG_CONTROL] & RD_AZ_HPC_CLOCK_GATING_DISABLE));
    /* find_first_free_audio: the stream encoder's own pin, then the first free */
    enc_acq[2] = 1;
    CHECK(dcn32_audio_acquire(&pool, 2, enc_acq) == 2 && dcn32_audio_acquire(&pool, 2, enc_acq) == 0 &&
          dcn32_audio_acquire(&pool, 5, NULL) == 1);
    dcn32_audio_release(&pool, 0);
    dcn32_audio_release(&pool, 1);
    /* no audio straps: HDMI runs as DVI */
    S.reg[AREG(regDC_PINSTRAPS)] = 0;
    {
        rdna3_audio_pool p2;
        dcn32_audio_pool_init(dev, &p2);
        CHECK(!p2.hdmi_audio_native && p2.count == 4);
    }

    /* HDMI at 1080p60 on DIG 2 / OTG 1, endpoint 2 */
    memset(&m1080, 0, sizeof(m1080));
    m1080.h_active = 1920; m1080.v_active = 1080; m1080.h_total = 2200; m1080.v_total = 1125;
    m1080.h_front = 88; m1080.h_sync = 44; m1080.v_front = 4; m1080.v_sync = 5; m1080.pixel_khz = 148500;
    memset(&s, 0, sizeof(s));
    s.d = dev;
    s.signal = RD_SIGNAL_HDMI_TYPE_A;
    s.fe = fe;
    s.otg = 1;
    s.az_inst = 2;
    dcn32_audio_info_from_edid(&e, &s.info);
    dcn32_audio_build_output(&s, &m1080, &rgb, 148500, NULL, NULL, 0, 0);
    CHECK(s.out.crtc_info.refresh_rate == 60 && s.out.crtc_info.requested_pixel_clock_100hz == 1485000 &&
          s.out.pll_info.dto_source == RD_DTO_SOURCE_ID0 + 1);
    S.reg[AREG(regDCCG_AUDIO_DTBCLK_DTO_PHASE)] = 0x1234;
    dcn32_audio_attach(&s);
    /* the wall DTO: DTO0 from OTG 1's 148.5 MHz, the DTBCLK one off */
    v = S.reg[AREG(regDCCG_AUDIO_DTO_SOURCE)];
    CHECK((v & DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO0_SOURCE_SEL_MASK) == 1 &&
          (v & DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL_MASK) == 0 && S.reg[AREG(regDCCG_AUDIO_DTBCLK_DTO_PHASE)] == 0 &&
          S.reg[AREG(regDCCG_AUDIO_DTO0_MODULE)] == 1485000 && S.reg[AREG(regDCCG_AUDIO_DTO0_PHASE)] == 240000);
    /* the AFMT: the pin -> DIG mapping, 8 channels (5.1 allocation), IEC 60958 channel numbers */
    CHECK(afmt(regAFMT0_AFMT_AUDIO_SRC_CONTROL, fe) == 2 &&
          ((afmt(regAFMT0_AFMT_AUDIO_PACKET_CONTROL2, fe) & AFMT0_AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_CHANNEL_ENABLE_MASK) >>
           AFMT0_AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_CHANNEL_ENABLE__SHIFT) == 0x3f &&
          !(afmt(regAFMT0_AFMT_MEM_PWR, fe) & AFMT0_AFMT_MEM_PWR__AFMT_MEM_PWR_FORCE_MASK) &&
          ((afmt(regAFMT0_AFMT_60958_1, fe) & AFMT0_AFMT_60958_1__AFMT_60958_CS_CHANNEL_NUMBER_R_MASK) >>
           AFMT0_AFMT_60958_1__AFMT_60958_CS_CHANNEL_NUMBER_R__SHIFT) == 2 &&
          (afmt(regAFMT0_AFMT_AUDIO_PACKET_CONTROL, fe) & AFMT0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK) &&
          (afmt(regDIG0_AFMT_CNTL, fe) & DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK));
    /* ACR: auto send, N / CTS of 148.5 MHz; the audio infoframe on line 2 */
    v = afmt(regDIG0_HDMI_ACR_PACKET_CONTROL, fe);
    CHECK((v & DIG0_HDMI_ACR_PACKET_CONTROL__HDMI_ACR_AUTO_SEND_MASK) && !(v & DIG0_HDMI_ACR_PACKET_CONTROL__HDMI_ACR_SOURCE_MASK));
    CHECK((afmt(regDIG0_HDMI_ACR_32_0, fe) >> DIG0_HDMI_ACR_32_0__HDMI_ACR_CTS_32__SHIFT) == 148500 &&
          (afmt(regDIG0_HDMI_ACR_32_1, fe) & DIG0_HDMI_ACR_32_1__HDMI_ACR_N_32_MASK) == 4096 &&
          (afmt(regDIG0_HDMI_ACR_44_0, fe) >> DIG0_HDMI_ACR_44_0__HDMI_ACR_CTS_44__SHIFT) == 165000 &&
          (afmt(regDIG0_HDMI_ACR_44_1, fe) & DIG0_HDMI_ACR_44_1__HDMI_ACR_N_44_MASK) == 6272 &&
          (afmt(regDIG0_HDMI_ACR_48_0, fe) >> DIG0_HDMI_ACR_48_0__HDMI_ACR_CTS_48__SHIFT) == 148500 &&
          (afmt(regDIG0_HDMI_ACR_48_1, fe) & DIG0_HDMI_ACR_48_1__HDMI_ACR_N_48_MASK) == 6144 &&
          (afmt(regDIG0_HDMI_AUDIO_PACKET_CONTROL, fe) & DIG0_HDMI_AUDIO_PACKET_CONTROL__HDMI_AUDIO_DELAY_EN_MASK) &&
          (afmt(regDIG0_HDMI_INFOFRAME_CONTROL0, fe) & DIG0_HDMI_INFOFRAME_CONTROL0__HDMI_AUDIO_INFO_SEND_MASK));
    /* the endpoint (the ELD the codec answers with): enabled, HDMI, 5.1,
     * LPCM 8 ch 32-192 kHz (HBR: 192 kHz 8 ch fits 1080p), AC-3, the ids and the name */
    CHECK((SA.az[2][RD_AZ_HOT_PLUG_CONTROL] & (RD_AZ_HPC_AUDIO_ENABLED | RD_AZ_HPC_CLOCK_GATING_DISABLE)) ==
              RD_AZ_HPC_AUDIO_ENABLED &&
          (SA.az[2][RD_AZ_CHANNEL_SPEAKER] & (RD_AZ_CS_SPEAKER_ALLOCATION | RD_AZ_CS_HDMI_CONNECTION | RD_AZ_CS_DP_CONNECTION)) ==
              (0x0f | RD_AZ_CS_HDMI_CONNECTION));
    CHECK(SA.az[2][RD_AZ_AUDIO_DESCRIPTOR0] == (7u | 0x7fu << 8 | 0x07u << 16 | 0x7fu << 24) &&
          SA.az[2][RD_AZ_AUDIO_DESCRIPTOR0 + 1] == (5u | 0x07u << 8 | 0x50u << 16) && SA.az[2][RD_AZ_AUDIO_DESCRIPTOR0 + 2] == 0 &&
          SA.az[2][RD_AZ_RESPONSE_HBR] == 1 && S.reg[AREG(regAZALIA_F0_CODEC_FUNCTION_PARAMETER_STREAM_FORMATS)] == 5);
    CHECK(SA.az[2][RD_AZ_SINK_INFO0] == (0x2D4Du | 0x1234u << 16) && SA.az[2][RD_AZ_SINK_INFO1] == 7 &&
          SA.az[2][RD_AZ_SINK_INFO4] == ('S' | 'I' << 8 | 'M' << 16 | (uint32_t)' ' << 24) &&
          SA.az[2][RD_AZ_SINK_INFO4 + 1] == ('T' | 'V' << 8) && SA.az[2][RD_AZ_SINK_INFO2] == 0x5558859e && s.enabled);
    /* a second enable does nothing; disable: mute, the audio clock off; the back end: the pin off */
    dcn32_audio_enable_stream(&s);
    dcn32_audio_disable_stream(&s);
    CHECK(!s.enabled &&
          !(afmt(regAFMT0_AFMT_AUDIO_PACKET_CONTROL, fe) & AFMT0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK) &&
          !(afmt(regDIG0_AFMT_CNTL, fe) & DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK));
    dcn32_audio_reset_back_end(&s);
    CHECK(!(SA.az[2][RD_AZ_HOT_PLUG_CONTROL] & RD_AZ_HPC_AUDIO_ENABLED));
    /* 480p: 8 ch limited to 48 kHz, no HBR */
    {
        rdna3_mode m480;
        memset(&m480, 0, sizeof(m480));
        m480.h_active = 720; m480.v_active = 480; m480.h_total = 858; m480.v_total = 525; m480.pixel_khz = 27000;
        dcn32_audio_build_output(&s, &m480, &rgb, 27000, NULL, NULL, 0, 0);
        dcn32_audio_attach(&s);
        CHECK(SA.az[2][RD_AZ_AUDIO_DESCRIPTOR0] == (7u | 0x07u << 8 | 0x07u << 16 | 0x07u << 24) && SA.az[2][RD_AZ_RESPONSE_HBR] == 0 &&
              (afmt(regDIG0_HDMI_ACR_48_0, fe) >> DIG0_HDMI_ACR_48_0__HDMI_ACR_CTS_48__SHIFT) == 27000);
        dcn32_audio_disable_stream(&s);
    }

    /* DP SST 8b/10b on DIG 0, endpoint 0: DTO1 from DPREFCLK (512 fs), the
     * audio SDPs (ASP, ATP, AIP, then the master enable) */
    memset(&ls, 0, sizeof(ls));
    ls.lane_count = 4;
    ls.link_rate = RD_DP_LINK_RATE_HIGH2;
    memset(&s, 0, sizeof(s));
    s.d = dev;
    s.signal = RD_SIGNAL_DISPLAY_PORT;
    s.fe = 0;
    s.otg = 0;
    s.az_inst = 0;
    s.dprefclk_khz = 600000;
    dcn32_audio_info_from_edid(&e, &s.info);
    dcn32_audio_build_output(&s, &m1080, &rgb, 148500, NULL, &ls, 17280000, 1);
    CHECK(s.out.dp_link_info.encoding == RD_DP_8b_10b_ENCODING && s.out.dp_link_info.link_bandwidth_kbps == 17814432 &&
          s.out.pll_info.audio_dto_source_clock_in_khz == 600000);
    dcn32_audio_attach(&s);
    v = S.reg[AREG(regDCCG_AUDIO_DTO_SOURCE)];
    CHECK(((v & DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL_MASK) >> DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL__SHIFT) == 1 &&
          (v & DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO2_USE_512FBR_DTO_MASK) &&
          S.reg[AREG(regDCCG_AUDIO_DTO1_MODULE)] == 6000000 && S.reg[AREG(regDCCG_AUDIO_DTO1_PHASE)] == 240000);
    v = afmt(regDP0_DP_SEC_CNTL, 0);
    CHECK((v & (DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE_MASK |
                DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK)) ==
              (DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE_MASK |
               DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK) &&
          afmt(regDP0_DP_SEC_AUD_N, 0) == 0x8000 && (afmt(regDP0_DP_SEC_TIMESTAMP, 0) & 1) &&
          (SA.az[0][RD_AZ_CHANNEL_SPEAKER] & (RD_AZ_CS_HDMI_CONNECTION | RD_AZ_CS_DP_CONNECTION)) == RD_AZ_CS_DP_CONNECTION &&
          SA.az[0][RD_AZ_AUDIO_DESCRIPTOR0] == (7u | 0x7fu << 8 | 0x07u << 16 | 0x7fu << 24));
    /* off: the SDPs go, the master enable stays for another SDP (GSP 0 on) */
    S.reg[DCN_B2 + regDP0_DP_SEC_CNTL] |= 1u << 4 << 16;   /* a non-audio bit (DP_SEC_GSP4_ENABLE area) */
    dcn32_audio_disable_stream(&s);
    v = afmt(regDP0_DP_SEC_CNTL, 0);
    CHECK(!(v & (DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE_MASK |
                 DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK)) && (v & DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK));
    S.reg[DCN_B2 + regDP0_DP_SEC_CNTL] = 0;

    /* 128b/132b on HPO stream encoder 1 (APG 1), UHBR10 x4, 4K60:
     * hblank_min_symbol_width, no HBR (disable_hbr_audio_dp2) */
    memset(&m4k, 0, sizeof(m4k));
    m4k.h_active = 3840; m4k.v_active = 2160; m4k.h_total = 4400; m4k.v_total = 2250; m4k.pixel_khz = 594000;
    ls.link_rate = RD_DP_LINK_RATE_UHBR10;
    s.hpo = 1;
    s.hpo_se = 1;
    s.az_inst = 1;
    dcn32_audio_build_output(&s, &m4k, &rgb, 594000, NULL, &ls, 38564000, 0);
    CHECK(s.out.dp_link_info.encoding == RD_DP_128b_132b_ENCODING && s.out.dp_link_info.hblank_min_symbol_width == 26880 &&
          s.out.dp_link_info.link_bandwidth_kbps == 38660651);
    S.reg[HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_GSP_CONTROL0, 1)] = 0;
    dcn32_audio_attach(&s);
    CHECK((S.reg[HPOR(regDP_STREAM_ENC0_DP_STREAM_ENC_AUDIO_CONTROL, 1)] & 7) == 1 &&
          (S.reg[AREG(regAPG0_APG_CONTROL2) + APG_ST] & APG0_APG_CONTROL2__APG_ENABLE_MASK) &&
          !(S.reg[AREG(regAPG0_APG_CONTROL2) + APG_ST] & APG0_APG_CONTROL2__APG_DP_AUDIO_STREAM_ID_MASK) &&
          ((S.reg[AREG(regAPG0_APG_DBG_GEN_CONTROL) + APG_ST] & APG0_APG_DBG_GEN_CONTROL__APG_DBG_AUDIO_CHANNEL_ENABLE_MASK) >>
           APG0_APG_DBG_GEN_CONTROL__APG_DBG_AUDIO_CHANNEL_ENABLE__SHIFT) == 0xff &&
          !(S.reg[AREG(regAPG0_APG_CONTROL) + APG_ST] & APG0_APG_CONTROL__APG_RESET_MASK) &&
          (S.reg[HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0, 1)] & 7) == 7 &&
          (S.reg[HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, 1)] & 1) && SA.az[1][RD_AZ_RESPONSE_HBR] == 0 &&
          (SA.az[1][RD_AZ_HOT_PLUG_CONTROL] & RD_AZ_HPC_AUDIO_ENABLED));
    dcn32_audio_disable_stream(&s);
    CHECK((S.reg[HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0, 1)] & 0xf) == 0 &&
          !(S.reg[HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_CONTROL, 1)] & 1) &&
          !(S.reg[AREG(regAPG0_APG_CONTROL2) + APG_ST] & APG0_APG_CONTROL2__APG_ENABLE_MASK));
    for (i = 0; i < 0x300; i++)
        S.reg[DCN_B2 + regDIG0_AFMT_CNTL - 0x60 + i] = save[i];
    printf("sim_navi32: display audio tests ran (%u endpoint writes)\n", SA.az_writes);
}

/* ---- output pixel formats (rdna3fmt) ---- */

#define FMTR(r_, o) S.reg[DCN_B2 + (r_) + FMT_ST * (o)]
#define FMTF(v, reg, field) (((v) & reg##__##field##_MASK) >> reg##__##field##__SHIFT)

static void test_dc_fmt_units(rdna3_dev *dev)
{
    rdna3_bit_depth_params bd;
    rdna3_clamping_params cl;
    rdna3_tg_color black;
    rdna3_sink_color_caps caps;
    rdna3_crtc_format f;
    rdna3_mode m;
    const uint32_t o = 3;
    uint32_t v, cs;

    /* resource_build_bit_depth_reduction_params, DITHER_OPTION_DEFAULT */
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_DEFAULT, RD_COLOR_DEPTH_888, RD_PIXEL_ENCODING_RGB, &bd);
    CHECK(!bd.truncate_enabled && bd.spatial_dither_enabled && bd.spatial_dither_depth == 1 && bd.highpass_random &&
          bd.rgb_random && !bd.frame_random && !bd.frame_modulation_enabled);
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_DEFAULT, RD_COLOR_DEPTH_888, RD_PIXEL_ENCODING_YCBCR444, &bd);
    CHECK(bd.spatial_dither_enabled && !bd.rgb_random);
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_DEFAULT, RD_COLOR_DEPTH_101010, RD_PIXEL_ENCODING_RGB, &bd);
    CHECK(bd.truncate_enabled && bd.truncate_depth == 2 && bd.truncate_mode == 1 && !bd.spatial_dither_enabled &&
          bd.frame_random);
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_DEFAULT, RD_COLOR_DEPTH_121212, RD_PIXEL_ENCODING_RGB, &bd);
    CHECK(!bd.truncate_enabled && !bd.spatial_dither_enabled && !bd.frame_random);
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_SPATIAL6, RD_COLOR_DEPTH_888, RD_PIXEL_ENCODING_RGB, &bd);
    CHECK(bd.truncate_enabled && bd.truncate_depth == 2 && bd.truncate_mode && bd.spatial_dither_depth == 0);
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_TRUN10_FM8, RD_COLOR_DEPTH_121212, RD_PIXEL_ENCODING_RGB, &bd);
    CHECK(bd.truncate_depth == 2 && !bd.truncate_mode && bd.frame_modulation_enabled && bd.frame_modulation_depth == 1);

    /* opp1_program_fmt: 8 bpc RGB, spatial dither 8 (frame random off: counter 0), full range clamp */
    FMTR(regFMT0_FMT_CONTROL, o) = 0xffffffffu & ~FMT0_FMT_CONTROL__FMT_DOUBLE_BUFFER_REG_UPDATE_PENDING_MASK;
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_DEFAULT, RD_COLOR_DEPTH_888, RD_PIXEL_ENCODING_RGB, &bd);
    dcn32_build_clamping_params(RD_COLOR_DEPTH_888, RD_PIXEL_ENCODING_RGB, &cl);
    dcn32_opp_program_fmt(dev, o, &bd, &cl);
    v = FMTR(regFMT0_FMT_BIT_DEPTH_CONTROL, o);
    CHECK(FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_EN) == 1 &&
          FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_DEPTH) == 1 &&
          FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_HIGHPASS_RANDOM_ENABLE) == 1 &&
          FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_RGB_RANDOM_ENABLE) == 1 &&
          FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_FRAME_RANDOM_ENABLE) == 0 &&
          FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_EN) == 0 &&
          FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_EN) == 0);
    v = FMTR(regFMT0_FMT_CONTROL, o);
    CHECK(FMTF(v, FMT0_FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_MAX) == 0 && FMTF(v, FMT0_FMT_CONTROL, FMT_PIXEL_ENCODING) == 0 &&
          FMTF(v, FMT0_FMT_CONTROL, FMT_SUBSAMPLING_MODE) == 0 && FMTF(v, FMT0_FMT_CONTROL, FMT_CBCR_BIT_REDUCTION_BYPASS) == 0);
    v = FMTR(regFMT0_FMT_CLAMP_CNTL, o);
    CHECK(FMTF(v, FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_DATA_EN) == 1 && FMTF(v, FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_COLOR_FORMAT) == 0);
    /* frame random spatial 6: the counter (15, swap 2), the seeds */
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_SPATIAL6_FRAME_RANDOM, RD_COLOR_DEPTH_888, RD_PIXEL_ENCODING_RGB, &bd);
    bd.r_seed_value = 0x12;
    dcn32_opp_program_bit_depth_reduction(dev, o, &bd);
    v = FMTR(regFMT0_FMT_CONTROL, o);
    CHECK(FMTF(v, FMT0_FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_MAX) == 15 &&
          FMTF(v, FMT0_FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_BIT_SWAP) == 2 &&
          (FMTR(regFMT0_FMT_DITHER_RAND_R_SEED, o) & FMT0_FMT_DITHER_RAND_R_SEED__FMT_RAND_R_SEED_MASK) == 0x12 &&
          FMTF(FMTR(regFMT0_FMT_BIT_DEPTH_CONTROL, o), FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_DEPTH) == 2);
    /* 10 bpc: truncation to 30 bpp with rounding; YCbCr 4:2:2 (12 bpc): no
     * reduction, encoding 1, subsampling 2; 4:2:0: encoding 2, CbCr bypass, the 4:2:0 memory on */
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_DEFAULT, RD_COLOR_DEPTH_101010, RD_PIXEL_ENCODING_RGB, &bd);
    dcn32_opp_program_bit_depth_reduction(dev, o, &bd);
    v = FMTR(regFMT0_FMT_BIT_DEPTH_CONTROL, o);
    CHECK(FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_EN) == 1 && FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_DEPTH) == 2 &&
          FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_MODE) == 1 && FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_EN) == 0);
    dcn32_build_bit_depth_params(RD_DITHER_OPTION_DEFAULT, RD_COLOR_DEPTH_121212, RD_PIXEL_ENCODING_YCBCR422, &bd);
    dcn32_build_clamping_params(RD_COLOR_DEPTH_121212, RD_PIXEL_ENCODING_YCBCR422, &cl);
    dcn32_opp_program_fmt(dev, o, &bd, &cl);
    v = FMTR(regFMT0_FMT_CONTROL, o);
    CHECK(FMTF(v, FMT0_FMT_CONTROL, FMT_PIXEL_ENCODING) == 1 && FMTF(v, FMT0_FMT_CONTROL, FMT_SUBSAMPLING_MODE) == 2 &&
          FMTF(FMTR(regFMT0_FMT_BIT_DEPTH_CONTROL, o), FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_EN) == 0);
    FMTR(regFMT0_FMT_MAP420_MEMORY_CONTROL, o) = FMT0_FMT_MAP420_MEMORY_CONTROL__FMT_MAP420MEM_PWR_FORCE_MASK;
    dcn32_build_clamping_params(RD_COLOR_DEPTH_888, RD_PIXEL_ENCODING_YCBCR420, &cl);
    dcn32_opp_program_fmt(dev, o, &bd, &cl);
    v = FMTR(regFMT0_FMT_CONTROL, o);
    CHECK(FMTF(v, FMT0_FMT_CONTROL, FMT_PIXEL_ENCODING) == 2 && FMTF(v, FMT0_FMT_CONTROL, FMT_CBCR_BIT_REDUCTION_BYPASS) == 1 &&
          !(FMTR(regFMT0_FMT_MAP420_MEMORY_CONTROL, o) & FMT0_FMT_MAP420_MEMORY_CONTROL__FMT_MAP420MEM_PWR_FORCE_MASK));
    cl.clamping_level = RD_CLAMPING_LIMITED_RANGE_10BPC;
    dcn32_opp_program_clamping_and_pixel_encoding(dev, o, &cl);
    CHECK(FMTF(FMTR(regFMT0_FMT_CLAMP_CNTL, o), FMT0_FMT_CLAMP_CNTL, FMT_CLAMP_COLOR_FORMAT) == 2);
    /* opp1_set_dyn_expansion: 8 -> 12 bit on HDMI / DP, 10 -> 12, none on DVI */
    dcn32_opp_set_dyn_expansion(dev, o, RD_COLOR_DEPTH_888, RD_SIGNAL_HDMI_TYPE_A);
    CHECK(FMTR(regFMT0_FMT_DYNAMIC_EXP_CNTL, o) == (FMT0_FMT_DYNAMIC_EXP_CNTL__FMT_DYNAMIC_EXP_EN_MASK |
                                                    FMT0_FMT_DYNAMIC_EXP_CNTL__FMT_DYNAMIC_EXP_MODE_MASK));
    dcn32_opp_set_dyn_expansion(dev, o, RD_COLOR_DEPTH_101010, RD_SIGNAL_DISPLAY_PORT);
    CHECK(FMTR(regFMT0_FMT_DYNAMIC_EXP_CNTL, o) == FMT0_FMT_DYNAMIC_EXP_CNTL__FMT_DYNAMIC_EXP_EN_MASK);
    dcn32_opp_set_dyn_expansion(dev, o, RD_COLOR_DEPTH_888, RD_SIGNAL_DVI_SINGLE_LINK);
    CHECK(FMTR(regFMT0_FMT_DYNAMIC_EXP_CNTL, o) == 0);

    /* the DPG: YCbCr black (0x1f4 / 0x40 / 0x1f4, MSB aligned), blanked;
     * video mode clears it; bars and ramps */
    dcn32_color_space_to_black_color(RD_CS_YCBCR709, &black);
    CHECK(black.color_r_cr == 0x1f4 && black.color_g_y == 0x40 && black.color_b_cb == 0x1f4);
    dcn32_color_space_to_black_color(RD_CS_SRGB_LIMITED, &black);
    CHECK(black.color_g_y == 0x40 && black.color_r_cr == 0x40);
    dcn32_blank_pixel_data(dev, o, 1, RD_CS_YCBCR709, RD_COLOR_DEPTH_888, 1920, 1080);
    v = FMTR(regDPG0_DPG_CONTROL, o);
    CHECK(FMTF(v, DPG0_DPG_CONTROL, DPG_EN) == 1 && FMTF(v, DPG0_DPG_CONTROL, DPG_MODE) == RD_TEST_PATTERN_MODE_HORIZONTALBARS &&
          FMTR(regDPG0_DPG_COLOUR_R_CR, o) == (0x1f4u << 6 | 0x1f4u << 6 << 16) &&
          FMTR(regDPG0_DPG_COLOUR_G_Y, o) == (0x40u << 6 | 0x40u << 6 << 16) &&
          FMTR(regDPG0_DPG_DIMENSIONS, o) == (1080u | 1920u << DPG0_DPG_DIMENSIONS__DPG_ACTIVE_WIDTH__SHIFT) &&
          dcn32_opp_dpg_is_blanked(dev, o) && !dcn32_opp_dpg_is_pending(dev, o) && dcn32_wait_for_blank_complete(dev, o));
    FMTR(regDPG0_DPG_STATUS, o) = DPG0_DPG_STATUS__DPG_DOUBLE_BUFFER_PENDING_MASK;
    CHECK(dcn32_opp_dpg_is_pending(dev, o) && !dcn32_opp_dpg_is_blanked(dev, o));
    FMTR(regDPG0_DPG_STATUS, o) = 0;
    dcn32_blank_pixel_data(dev, o, 0, RD_CS_YCBCR709, RD_COLOR_DEPTH_888, 1920, 1080);
    CHECK(FMTR(regDPG0_DPG_CONTROL, o) == 0 && FMTR(regDPG0_DPG_COLOUR_R_CR, o) == 0 && !dcn32_opp_dpg_is_blanked(dev, o));
    dcn32_opp_set_disp_pattern_generator(dev, o, RD_CONTROLLER_DP_TEST_PATTERN_VERTICALBARS, RD_CONTROLLER_DP_COLOR_SPACE_RGB,
                                         RD_COLOR_DEPTH_888, NULL, 640, 480, 0);
    v = FMTR(regDPG0_DPG_CONTROL, o);
    CHECK(FMTF(v, DPG0_DPG_CONTROL, DPG_MODE) == RD_TEST_PATTERN_MODE_VERTICALBARS && FMTF(v, DPG0_DPG_CONTROL, DPG_BIT_DEPTH) == 1 &&
          FMTR(regDPG0_DPG_COLOUR_R_CR, o) == 0xff00u << 16);
    dcn32_opp_set_disp_pattern_generator(dev, o, RD_CONTROLLER_DP_TEST_PATTERN_COLORRAMP, RD_CONTROLLER_DP_COLOR_SPACE_RGB,
                                         RD_COLOR_DEPTH_101010, NULL, 640, 480, 0);
    v = FMTR(regDPG0_DPG_CONTROL, o);
    CHECK(FMTF(v, DPG0_DPG_CONTROL, DPG_MODE) == RD_TEST_PATTERN_MODE_DUALRAMP_RGB && FMTF(v, DPG0_DPG_CONTROL, DPG_VRES) == 5 &&
          FMTF(v, DPG0_DPG_CONTROL, DPG_HRES) == 8 &&
          FMTR(regDPG0_DPG_RAMP_CONTROL, o) == ((384u << 6) | 6u << DPG0_DPG_RAMP_CONTROL__DPG_INC0__SHIFT |
                                                8u << DPG0_DPG_RAMP_CONTROL__DPG_INC1__SHIFT));
    dcn32_opp_set_disp_pattern_generator(dev, o, RD_CONTROLLER_DP_TEST_PATTERN_COLORSQUARES_CEA,
                                         RD_CONTROLLER_DP_COLOR_SPACE_YCBCR709, RD_COLOR_DEPTH_121212, NULL, 640, 480, 0);
    v = FMTR(regDPG0_DPG_CONTROL, o);
    CHECK(FMTF(v, DPG0_DPG_CONTROL, DPG_MODE) == RD_TEST_PATTERN_MODE_COLORSQUARES_YCBCR709 &&
          FMTF(v, DPG0_DPG_CONTROL, DPG_DYNAMIC_RANGE) == 1 && FMTF(v, DPG0_DPG_CONTROL, DPG_BIT_DEPTH) == 3 &&
          FMTF(v, DPG0_DPG_CONTROL, DPG_VRES) == 6);
    dcn32_opp_set_disp_pattern_generator(dev, o, RD_CONTROLLER_DP_TEST_PATTERN_VIDEOMODE, 0, RD_COLOR_DEPTH_888, NULL, 640, 480, 0);

    /* dce112_program_pixel_clk_resync on PHY PLL E (0x4c) and A */
    dcn32_program_pixel_clk_resync(dev, 4, RD_SIGNAL_HDMI_TYPE_A, RD_COLOR_DEPTH_101010, 0);
    dcn32_program_pixel_clk_resync(dev, 0, RD_SIGNAL_HDMI_TYPE_A, RD_COLOR_DEPTH_161616, 0);
    CHECK(FMTF(S.reg[AREG(regPHYPLLE_PIXCLK_RESYNC_CNTL)], PHYPLLA_PIXCLK_RESYNC_CNTL, PHYPLLA_DCCG_DEEP_COLOR_CNTL) == 1 &&
          FMTF(S.reg[AREG(regPHYPLLA_PIXCLK_RESYNC_CNTL)], PHYPLLA_PIXCLK_RESYNC_CNTL, PHYPLLA_DCCG_DEEP_COLOR_CNTL) == 3);
    dcn32_program_pixel_clk_resync(dev, 0, RD_SIGNAL_DVI_SINGLE_LINK, RD_COLOR_DEPTH_161616, 0);
    dcn32_program_pixel_clk_resync(dev, 4, RD_SIGNAL_HDMI_TYPE_A, RD_COLOR_DEPTH_888, 0);
    CHECK(S.reg[AREG(regPHYPLLA_PIXCLK_RESYNC_CNTL)] == 0 && S.reg[AREG(regPHYPLLE_PIXCLK_RESYNC_CNTL)] == 0);
    CHECK(dcn32_hdmi_pll_clk_khz(148500, RD_COLOR_DEPTH_101010) == 185625 &&
          dcn32_hdmi_pll_clk_khz(148500, RD_COLOR_DEPTH_121212) == 222750 && dcn32_hdmi_pll_clk_khz(148500, RD_COLOR_DEPTH_888) == 148500);

    /* amdgpu_dm's format: convert_color_depth_from_display_info */
    CHECK(dcn32_convert_color_depth(0, 16) == RD_COLOR_DEPTH_888 && dcn32_convert_color_depth(12, 16) == RD_COLOR_DEPTH_121212 &&
          dcn32_convert_color_depth(12, 10) == RD_COLOR_DEPTH_101010 && dcn32_convert_color_depth(10, 9) == RD_COLOR_DEPTH_888 &&
          dcn32_convert_color_depth(6, 16) == RD_COLOR_DEPTH_666);
    /* a 4:4:4 / 4:2:2 sink of 12 bpc and 300 MHz TMDS */
    memset(&caps, 0, sizeof(caps));
    caps.color_formats = RD_COLOR_FORMAT_RGB444 | RD_COLOR_FORMAT_YCBCR444 | RD_COLOR_FORMAT_YCBCR422;
    caps.bpc = 12;
    caps.max_tmds_clock_khz = 300000;
    memset(&m, 0, sizeof(m));
    m.h_active = 1920; m.v_active = 1080; m.h_total = 2200; m.v_total = 1125; m.pixel_khz = 148500;
    cs = dcn32_output_format(&caps, &m, RD_SIGNAL_HDMI_TYPE_A, 16, 0, &f);
    CHECK(f.pixel_encoding == RD_PIXEL_ENCODING_YCBCR444 && f.color_depth == RD_COLOR_DEPTH_121212 && cs == RD_CS_YCBCR709);
    m.pixel_khz = 297000;   /* 4K30: 12 and 10 bpc above 300 MHz TMDS -> 8 bpc */
    cs = dcn32_output_format(&caps, &m, RD_SIGNAL_HDMI_TYPE_A, 16, 0, &f);
    CHECK(f.color_depth == RD_COLOR_DEPTH_888);
    m.pixel_khz = 27000;    /* SDTV: BT.601 */
    cs = dcn32_output_format(&caps, &m, RD_SIGNAL_HDMI_TYPE_A, 8, 0, &f);
    CHECK(f.color_depth == RD_COLOR_DEPTH_888 && cs == RD_CS_YCBCR601);
    cs = dcn32_output_format(&caps, &m, RD_SIGNAL_HDMI_TYPE_A, 16, 1, &f);
    CHECK(f.pixel_encoding == RD_PIXEL_ENCODING_YCBCR422 && f.color_depth == RD_COLOR_DEPTH_121212);
    cs = dcn32_output_format(&caps, &m, RD_SIGNAL_DVI_SINGLE_LINK, 16, 0, &f);
    CHECK(f.pixel_encoding == RD_PIXEL_ENCODING_RGB && cs == RD_CS_SRGB && f.color_depth == RD_COLOR_DEPTH_121212);
    caps.color_formats = RD_COLOR_FORMAT_RGB444;
    caps.max_tmds_clock_khz = 0;   /* no max TMDS clock: no depth fits, the depth stays */
    m.pixel_khz = 148500;
    cs = dcn32_output_format(&caps, &m, RD_SIGNAL_HDMI_TYPE_A, 16, 0, &f);
    CHECK(f.pixel_encoding == RD_PIXEL_ENCODING_RGB && f.color_depth == RD_COLOR_DEPTH_121212 && cs == RD_CS_SRGB);
    CHECK(dcn32_color_space_type(RD_CS_YCBCR709) == RD_COLOR_SPACE_YCBCR709_TYPE &&
          dcn32_color_space_type(RD_CS_SRGB_LIMITED) == RD_COLOR_SPACE_RGB_LIMITED_TYPE &&
          dcn32_color_space_type(RD_CS_2020_YCBCR) == RD_COLOR_SPACE_YCBCR2020_TYPE &&
          dcn32_color_space_type(RD_CS_SRGB) == RD_COLOR_SPACE_RGB_TYPE);
    printf("sim_navi32: output format (FMT / DPG) tests ran\n");
}

/* rdna3fmt + rdna3audio on the HDMI head of test_dc_hdmi (DIG fe, OTG 2,
 * pipe 1 at 2560 x 1440): 1080p in YCbCr 4:4:4 at 10 bpc with audio, then
 * back to 1440p in RGB 8 bpc */
static void test_dc_fmt_modeset(rdna3_dev *dev, rdna3_dmub *dm, rdna3_dcn *dc2, rdna3_dcn_scale *sc,
                                const rdna3_display_path *own)
{
    static rdna3_dcn_modeset ms;
    static rdna3_audio_stream au;
    static rdna3_edid e;
    rdna3_hdmi_cfg hc;
    rdna3_mode m1080, up;
    uint8_t edid[256];
    uint32_t fe = (uint32_t)dc2->head[0].dig_fe, v, i, opp;
    static uint32_t dig_save[0x300];

    for (i = 0; i < 0x300; i++)
        dig_save[i] = S.reg[DCN_B2 + regDIG0_AFMT_CNTL - 0x60 + i];
    au_reset();
    au_test_edid(edid);
    CHECK(edid_parse(edid, 256, &e) == 0);
    memset(&hc, 0, sizeof(hc));
    hc.flags = RD_HDMI_STREAM | RD_HDMI_FMT;
    hc.ddc = -1;
    hc.enc_caps = RD_ATOM_ENC_CAP_HDMI6G;
    hc.hdmi_sink = 1;
    hc.max_tmds_clk_mhz = 600;
    dcn32_sink_color_caps(&e, &hc.color);
    hc.color.bpc = 10;
    hc.color.max_tmds_clock_khz = 600000;
    hc.max_bpc = 16;
    memset(&m1080, 0, sizeof(m1080));
    m1080.h_active = 1920; m1080.v_active = 1080; m1080.h_total = 2200; m1080.v_total = 1125;
    m1080.h_front = 88; m1080.h_sync = 44; m1080.v_front = 4; m1080.v_sync = 5; m1080.pixel_khz = 148500;
    m1080.h_sync_pos = m1080.v_sync_pos = 1; m1080.refresh_mhz = 60000;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 241500, &m1080, NULL, &ms) == RD_MODESET_OK &&
          ms.fmt_want.pixel_encoding == RD_PIXEL_ENCODING_YCBCR444 && ms.fmt_want.color_depth == RD_COLOR_DEPTH_101010 &&
          ms.cs_want == RD_CS_YCBCR709 && ms.fmt_cur.pixel_encoding == RD_PIXEL_ENCODING_RGB &&
          ms.fmt_cur.color_depth == RD_COLOR_DEPTH_888);
    /* the link encoder refuses 16 bpc (max deep color 12): the retry lands lower */
    hc.color.bpc = 16;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 241500, &m1080, NULL, &ms) == RD_MODESET_OK &&
          ms.fmt_want.color_depth == RD_COLOR_DEPTH_121212);
    hc.color.bpc = 10;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 241500, &m1080, NULL, &ms) == RD_MODESET_OK);
    opp = ms.opp;
    /* the audio of the stream: endpoint 3 */
    memset(&au, 0, sizeof(au));
    au.d = dev;
    au.signal = RD_SIGNAL_HDMI_TYPE_A;
    au.fe = fe;
    au.otg = ms.otg;
    au.az_inst = 3;
    dcn32_audio_info_from_edid(&e, &au.info);
    ms.audio = &au;
    SK.otg_model = 1;
    S.dcn_frames = 1;
    S.dmub.nseq = 0;
    CHECK(dcn32_modeset_apply(dev, dm, dc2, sc, &ms, FB_START + 0x8000000, 1920, FB_START, 2560, 50) == RDNA3_OK);
    /* the PHY PLL at 148.5 x 1.25 MHz with the 5:4 ratio, the DIG stream at the TMDS clock */
    CHECK(S.dmub.pixclk[0] == (1856250 & 0xff) && S.dmub.pixclk[1] == ((1856250 >> 8) & 0xff) && S.dmub.pixclk[9] == 1 &&
          (S.dmub.enc[4] | S.dmub.enc[5] << 8) == 18562);
    CHECK(FMTF(S.reg[AREG(regPHYPLLA_PIXCLK_RESYNC_CNTL) + (ms.be < 4 ? ms.be : 0)], PHYPLLA_PIXCLK_RESYNC_CNTL,
               PHYPLLA_DCCG_DEEP_COLOR_CNTL) == 1);
    /* HDMI deep color 30 bit; the AVI frame says YCbCr 4:4:4, BT.709 */
    v = S.reg[DIGK(regDIG0_HDMI_CONTROL, fe)];
    CHECK((v & DIG0_HDMI_CONTROL__HDMI_DEEP_COLOR_ENABLE_MASK) &&
          ((v & DIG0_HDMI_CONTROL__HDMI_DEEP_COLOR_DEPTH_MASK) >> DIG0_HDMI_CONTROL__HDMI_DEEP_COLOR_DEPTH__SHIFT) == 1);
    CHECK(((S.vpg[fe].mem[1] >> 8) & 0xff) == 0x5e && ((S.vpg[fe].mem[1] >> 16) & 0xc0) == 0x80);
    /* the MPC's output CSC (YCbCr 709, set A), the OPP's formatter: 10-bit
     * truncation, 4:4:4, dynamic expansion 10 -> 12; the DPG back to video at 1080p */
    CHECK(S.reg[AREG(regMPC_OUT0_CSC_MODE) + (regMPC_OUT1_CSC_MODE - regMPC_OUT0_CSC_MODE) * opp] == 1 &&
          (S.reg[AREG(regMPC_OUT0_CSC_C11_C12_A) + (regMPC_OUT1_CSC_MODE - regMPC_OUT0_CSC_MODE) * opp] & 0xffff) == 0xE04);
    v = FMTR(regFMT0_FMT_BIT_DEPTH_CONTROL, opp);
    CHECK(FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_EN) == 1 && FMTF(v, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_DEPTH) == 2 &&
          FMTF(FMTR(regFMT0_FMT_CONTROL, opp), FMT0_FMT_CONTROL, FMT_PIXEL_ENCODING) == 0 &&
          FMTR(regFMT0_FMT_DYNAMIC_EXP_CNTL, opp) == FMT0_FMT_DYNAMIC_EXP_CNTL__FMT_DYNAMIC_EXP_EN_MASK &&
          FMTR(regDPG0_DPG_CONTROL, opp) == 0 &&
          FMTR(regDPG0_DPG_DIMENSIONS, opp) == (1080u | 1920u << DPG0_DPG_DIMENSIONS__DPG_ACTIVE_WIDTH__SHIFT));
    /* the audio: DTO0 from the 148.5 MHz pixel clock, ACR at 148.5 MHz
     * (no 30 bpc table), the endpoint on, the samples sent */
    CHECK(S.reg[AREG(regDCCG_AUDIO_DTO0_MODULE)] == 1485000 && au.enabled &&
          (afmt(regDIG0_HDMI_ACR_48_0, fe) >> DIG0_HDMI_ACR_48_0__HDMI_ACR_CTS_48__SHIFT) == 148500 &&
          (SA.az[3][RD_AZ_HOT_PLUG_CONTROL] & RD_AZ_HPC_AUDIO_ENABLED) && SA.az[3][RD_AZ_RESPONSE_HBR] == 1 &&
          (afmt(regAFMT0_AFMT_AUDIO_PACKET_CONTROL, fe) & AFMT0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK) &&
          afmt(regAFMT0_AFMT_AUDIO_SRC_CONTROL, fe) == 3 &&
          !(S.reg[DIGK(regDIG0_HDMI_GC, fe)] & DIG0_HDMI_GC__HDMI_GC_AVMUTE_MASK));
    /* back to 1440p, RGB 8 bpc (a sink without YCbCr / deep color) */
    hc.running = ms.fmt_want;
    hc.running_cs = ms.cs_want;
    hc.color.color_formats = RD_COLOR_FORMAT_RGB444;
    hc.color.bpc = 8;
    memset(&up, 0, sizeof(up));
    up.h_active = 2560; up.v_active = 1440; up.h_total = 2720; up.v_total = 1481;
    up.h_front = 48; up.h_sync = 32; up.v_front = 3; up.v_sync = 5; up.pixel_khz = 241500;
    up.h_sync_pos = up.v_sync_pos = 1;
    CHECK(dcn32_modeset_plan_hdmi(dev, dc2, 0, own, &hc, 148500, &up, NULL, &ms) == RD_MODESET_OK &&
          ms.fmt_cur.color_depth == RD_COLOR_DEPTH_101010 && ms.fmt_want.color_depth == RD_COLOR_DEPTH_888);
    ms.audio = &au;
    CHECK(dcn32_modeset_apply(dev, dm, dc2, sc, &ms, FB_START, 2560, FB_START + 0x8000000, 1920, 50) == RDNA3_OK &&
          dc2->head[0].width == 2560);
    v = S.reg[DIGK(regDIG0_HDMI_CONTROL, fe)];
    CHECK(!(v & DIG0_HDMI_CONTROL__HDMI_DEEP_COLOR_ENABLE_MASK) && S.dmub.pixclk[9] == 0 &&
          S.reg[AREG(regMPC_OUT0_CSC_C11_C12_A) + (regMPC_OUT1_CSC_MODE - regMPC_OUT0_CSC_MODE) * opp] == 0x2000 &&
          FMTF(FMTR(regFMT0_FMT_BIT_DEPTH_CONTROL, opp), FMT0_FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_EN) == 1 &&
          S.reg[AREG(regDCCG_AUDIO_DTO0_MODULE)] == 2415000 &&
          (afmt(regDIG0_HDMI_ACR_48_0, fe) >> DIG0_HDMI_ACR_48_0__HDMI_ACR_CTS_48__SHIFT) == 241500 && au.enabled);
    /* the audio stream off with the next stream down (no audio this time) */
    dcn32_audio_disable_stream(&au);
    dcn32_audio_reset_back_end(&au);
    CHECK(!(SA.az[3][RD_AZ_HOT_PLUG_CONTROL] & RD_AZ_HPC_AUDIO_ENABLED));
    for (i = 0; i < 0x300; i++)
        S.reg[DCN_B2 + regDIG0_AFMT_CNTL - 0x60 + i] = dig_save[i];
    SK.otg_model = 0;
    S.dcn_frames = 0;
    printf("sim_navi32: rdna3fmt / rdna3audio mode sets ran\n");
}

/* DP audio through link_set_dpms_on / _off (test_dp_uhbr): the stream's
 * audio on endpoint 0 */
static rdna3_audio_stream dp_au;

static void au_dp_attach(rdna3_dp_link *dp, rdna3_dev *dev)
{
    static rdna3_edid e;
    uint8_t edid[256];
    au_reset();
    au_test_edid(edid);
    CHECK(edid_parse(edid, 256, &e) == 0);
    memset(&dp_au, 0, sizeof(dp_au));
    dp_au.d = dev;
    dp_au.az_inst = 0;
    dcn32_audio_info_from_edid(&e, &dp_au.info);
    dp->audio = &dp_au;
}

/* HPO stream encoder 'se': the audio SDPs and the APG on, the pin a DP one */
static int au_dp_hpo_on(uint32_t se)
{
    return dp_au.enabled && dp_au.hpo && (S.reg[HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0, se)] & 7) == 7 &&
           (S.reg[AREG(regAPG0_APG_CONTROL2) + APG_ST * se] & APG0_APG_CONTROL2__APG_ENABLE_MASK) &&
           dp_au.out.dp_link_info.encoding == RD_DP_128b_132b_ENCODING && SA.az[0][RD_AZ_RESPONSE_HBR] == 0 &&
           (SA.az[0][RD_AZ_CHANNEL_SPEAKER] & RD_AZ_CS_DP_CONNECTION) && (SA.az[0][RD_AZ_HOT_PLUG_CONTROL] & RD_AZ_HPC_AUDIO_ENABLED);
}

/* DIG 'fe': DP_SEC audio on, the HPO's off */
static int au_dp_dio_on(uint32_t fe)
{
    const uint32_t m = DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE_MASK |
                       DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK;
    return dp_au.enabled && !dp_au.hpo && (afmt(regDP0_DP_SEC_CNTL, fe) & m) == m &&
           (S.reg[HPOR(regDP_SYM32_ENC0_DP_SYM32_ENC_SDP_AUDIO_CONTROL0, 0)] & 7) == 0 &&
           !(S.reg[AREG(regAPG0_APG_CONTROL2)] & APG0_APG_CONTROL2__APG_ENABLE_MASK) &&
           dp_au.out.dp_link_info.encoding == RD_DP_8b_10b_ENCODING && afmt(regAFMT0_AFMT_AUDIO_SRC_CONTROL, fe) == 0 &&
           (SA.az[0][RD_AZ_HOT_PLUG_CONTROL] & RD_AZ_HPC_AUDIO_ENABLED);
}

/* dp_is_sink_present (dcaux_dp_is_sink_present): the DDC clock pin read
 * as a GPIO input on a pad in I2C mode, 3 retries; everything put back */
static void test_dc_dp_sink_present(rdna3_dev *dev)
{
    const uint32_t mreg = AREG(regDC_GPIO_DDC1_MASK) + DDC_ST * 1, areg = AREG(regDC_GPIO_DDC1_A) + DDC_ST * 1;
    const uint32_t ereg = AREG(regDC_GPIO_DDC1_EN) + DDC_ST * 1;
    const uint32_t c5 = AREG(regDC_GPIO_AUX_CTRL_5), pa = AREG(regPHY_AUX_CNTL);
    uint32_t save[5], clk = 9;
    save[0] = S.reg[mreg]; save[1] = S.reg[areg]; save[2] = S.reg[ereg]; save[3] = S.reg[c5]; save[4] = S.reg[pa];
    /* DDC2 in AUX mode, the pins driven by the HW, output enables set */
    S.reg[mreg] = DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK;
    S.reg[areg] = 0;
    S.reg[ereg] = DC_GPIO_DDC1_EN__DC_GPIO_DDC1CLK_EN_MASK | DC_GPIO_DDC1_EN__DC_GPIO_DDC1DATA_EN_MASK;
    S.reg[c5] = 0x5;
    S.reg[pa] = 0x2u << (PHY_AUX_CNTL__AUX1_PAD_RXSEL__SHIFT + 2);
    SA.ddc_y_reads = SA.ddc_y_bad = 0;
    /* a DP sink: the clock line low, one read */
    SA.ddc_clk_level[1] = 0;
    CHECK(dcaux_dp_is_sink_present(dev, 1, 1, &clk) == 1 && clk == 0 && SA.ddc_y_reads == 1 && SA.ddc_y_bad == 0);
    /* restored: the pin fields, the pad back in AUX mode, I2CMODE / RXSEL;
     * the pull-downs stay on as DC leaves them */
    CHECK(S.reg[mreg] == (DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK | DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_PD_EN_MASK |
                          DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_PD_EN_MASK) &&
          S.reg[areg] == 0 &&
          S.reg[ereg] == (DC_GPIO_DDC1_EN__DC_GPIO_DDC1CLK_EN_MASK | DC_GPIO_DDC1_EN__DC_GPIO_DDC1DATA_EN_MASK) &&
          S.reg[c5] == 0x5 && S.reg[pa] == 0x2u << (PHY_AUX_CNTL__AUX1_PAD_RXSEL__SHIFT + 2));
    /* a TMDS sink on a DP connector (or nothing with a pull-up): the clock
     * line high on all four reads -> no DP sink */
    SA.ddc_clk_level[1] = 1;
    SA.ddc_y_reads = 0;
    CHECK(dcaux_dp_is_sink_present(dev, 1, 1, &clk) == 0 && clk == 1 && SA.ddc_y_reads == 4 && SA.ddc_y_bad == 0);
    /* an engine without a DDC: what the connector says */
    CHECK(dcaux_dp_is_sink_present(dev, RD_AUX_ENGINES, 1, NULL) == 1 &&
          dcaux_dp_is_sink_present(dev, RD_AUX_ENGINES, 0, NULL) == 0);
    SA.ddc_clk_level[1] = 0;
    S.reg[mreg] = save[0]; S.reg[areg] = save[1]; S.reg[ereg] = save[2]; S.reg[c5] = save[3]; S.reg[pa] = save[4];
}
