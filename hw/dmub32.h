/*
 * dmub32.h - DMCUB (Display Microcontroller Unit B) service for DCN 3.2.x,
 * ported from Linux amdgpu Display Core dmub_srv.c / dmub_dcn32.c and the
 * amdgpu_dm parts that drive them (dm_dmub_sw_init, dm_dmub_hw_init), MIT.
 *
 * On DCN 3.2 Display Core does not run the VBIOS command tables itself
 * (dcn32 debug.dmub_command_table = true): DIGxEncoderControl,
 * DIG1TransmitterControl, SetPixelClock and EnableDispPowerGating go to the
 * DMCUB as DMUB_CMD__VBIOS ring commands and its firmware executes the
 * tables from the copy of the VBIOS the driver placed in window CW3. The
 * DMCUB also owns PSR/ABM, FAMS (FW-assisted MCLK switching) and the
 * p-state handshake the PMFW needs for UCLK DPM.
 *
 * The instruction image is loaded by the PSP (LOAD_IP_FW GFX_FW_TYPE_DMUB,
 * already in the PSP load plan), which also programs windows CW0/CW1; this
 * code lays out the other regions in VRAM, resets the core, programs the
 * data / VBIOS / mailbox / trace / state windows and releases it. A
 * backdoor load (driver copies the image to CW0) is kept for completeness.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DMUB32_H
#define RDNA3_DMUB32_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* windows (enum dmub_window_id) */
enum {
    RD_DMUB_WIN_INST_CONST = 0,
    RD_DMUB_WIN_STACK,
    RD_DMUB_WIN_BSS_DATA,
    RD_DMUB_WIN_VBIOS,
    RD_DMUB_WIN_MAILBOX,
    RD_DMUB_WIN_TRACEBUFF,
    RD_DMUB_WIN_FW_STATE,
    RD_DMUB_WIN_SCRATCH_MEM,
    RD_DMUB_WIN_IB_MEM,
    RD_DMUB_WIN_SHARED_STATE,
    RD_DMUB_WIN_LSDMA_BUFFER,
    RD_DMUB_WIN_CURSOR_OFFLOAD,
    RD_DMUB_WIN_TOTAL
};

#define RD_DMUB_RB_CMD_SIZE   64
#define RD_DMUB_RB_MAX_ENTRY  128
#define RD_DMUB_RB_SIZE       (RD_DMUB_RB_CMD_SIZE * RD_DMUB_RB_MAX_ENTRY)

/* DMCUB address space bases of the windows (dmub_srv.c) */
#define RD_DMUB_CW0_BASE      0x60000000u
#define RD_DMUB_CW1_BASE      0x61000000u
#define RD_DMUB_CW3_BASE      0x63000000u
#define RD_DMUB_CW4_BASE      0x64000000u
#define RD_DMUB_CW5_BASE      0x65000000u
#define RD_DMUB_CW6_BASE      0x66000000u
#define RD_DMUB_REGION5_BASE  0xA0000000u
#define RD_DMUB_TRACE_ENTRY_OFFSET 16

/* SCRATCH0: union dmub_fw_boot_status */
#define RD_DMUB_BOOT_DAL_FW          (1u << 0)
#define RD_DMUB_BOOT_MAILBOX_RDY     (1u << 1)
#define RD_DMUB_BOOT_OPT_INIT_DONE   (1u << 2)
#define RD_DMUB_BOOT_RESTORE_REQ     (1u << 3)
#define RD_DMUB_BOOT_DEFER_LOAD      (1u << 4)
#define RD_DMUB_BOOT_FAMS_ENABLED    (1u << 5)
#define RD_DMUB_BOOT_DETECT_REQ      (1u << 6)
#define RD_DMUB_BOOT_HW_PWR_INIT     (1u << 7)

/* GPINT (DMCUB_GPINT_DATAIN1): param 0-15, command 16-27, status 28-31 */
#define RD_DMUB_GPINT(cmd, param)    (((uint32_t)(param) & 0xffff) | (((uint32_t)(cmd) & 0xfff) << 16) | (1u << 28))
#define RD_DMUB_GPINT_STOP_FW        2
#define RD_DMUB_GPINT_STOP_FW_RESPONSE 0xDEADDEADu

/* struct dmub_cmd_header: type 0-7, sub_type 8-15, ret_status 16,
 * multi_cmd_pending 17, is_reg_based 18, payload_bytes 24-29 */
#define RD_DMUB_HDR(type, sub, payload, ret) \
    ((uint32_t)(type) | ((uint32_t)(sub) << 8) | ((ret) ? (1u << 16) : 0) | \
     (((uint32_t)(payload) & 0x3f) << 24))
#define RD_DMUB_HDR_RET_STATUS       (1u << 16)

/* enum dmub_cmd_type (the ones used here) */
#define RD_DMUB_CMD_QUERY_FEATURE_CAPS  6
#define RD_DMUB_CMD_VBIOS               128
/* enum dmub_cmd_vbios_type */
#define RD_DMUB_VBIOS_DIGX_ENCODER_CONTROL     0
#define RD_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL 1
#define RD_DMUB_VBIOS_SET_PIXEL_CLOCK          2
#define RD_DMUB_VBIOS_ENABLE_DISP_POWER_GATING 3

/* struct dmub_fw_meta_info (64-byte union dmub_fw_meta at the end of the
 * instruction image, before the PSP footer) */
#define RD_DMUB_FW_META_MAGIC 0x444D5542u   /* "DMUB" */
typedef struct rdna3_dmub_meta {
    uint32_t fw_region_size, trace_buffer_size, fw_version;
    uint8_t  dal_fw;
    uint32_t shared_state_size;
    uint32_t feature_bits;
    int      found;
} rdna3_dmub_meta;

typedef struct rdna3_dmub_region {
    uint32_t base, top;             /* offsets inside the DMUB VRAM block */
} rdna3_dmub_region;

typedef struct rdna3_dmub {
    rdna3_dev *d;
    /* firmware (dmcub_firmware_header_v1_0) */
    const uint8_t *fw_inst_const;   /* after the 256-byte PSP header */
    uint32_t inst_const_size;       /* minus PSP header and footer */
    const uint8_t *fw_bss_data;
    uint32_t bss_data_size;
    uint32_t fw_version;            /* ucode_version */
    rdna3_dmub_meta meta;
    /* layout */
    uint32_t vbios_size;
    rdna3_dmub_region region[RD_DMUB_WIN_TOTAL];
    uint32_t fb_size;
    uint64_t fb_off;                /* VRAM offset of the block */
    /* state */
    int      sw_init, hw_init, psp_loaded;
    uint32_t inbox1_wptr, inbox1_rptr, inbox1_last_wptr;
    uint32_t submitted, reported;
    uint32_t boot_status;           /* SCRATCH0 after the auto-load wait */
    uint32_t feature_caps[4];       /* QUERY_FEATURE_CAPS reply (raw) */
} rdna3_dmub;

/* dm_dmub_sw_init: parse the firmware blob (dcn_3_2_0_dmcub.bin), find
 * its meta info, lay out the windows and allocate them in VRAM.
 * 'vbios_size' is the image the hw_init will copy into CW3. */
int  dmub32_sw_init(rdna3_dmub *dm, rdna3_dev *d, const uint8_t *fw, uint32_t fw_len,
                    uint32_t vbios_size);

/* DMCUB enabled in this ASIC (CC_DC_PIPE_DIS.DC_DMCUB_ENABLE) */
int  dmub32_is_supported(rdna3_dev *d);
/* DMCUB running with DAL firmware (DMCUB_CNTL.ENABLE and SCRATCH0.dal_fw) */
int  dmub32_is_hw_init(rdna3_dev *d);

/* dm_dmub_hw_init: reset (asks running firmware to stop first), BSS and
 * VBIOS copies, clear mailbox/trace/state, windows, mailboxes, boot
 * options, release, then wait (<= timeout_us) for dal_fw + mailbox_rdy.
 * psp_loaded: the PSP already put the image in CW0 (no backdoor load). */
int  dmub32_hw_init(rdna3_dmub *dm, const uint8_t *vbios, uint32_t vbios_len, int psp_loaded,
                    uint32_t timeout_us);

/* dmub_srv_hw_reset (also what unload does) */
/* CNTL/SEC_CNTL/scratch, the PSP-programmed CW0/CW1 and the fault
 * addresses into the log */
void dmub32_log_state(rdna3_dmub *dm, const char *when);
/* the same without a dmub_srv: when the PSP-programmed code windows
 * (CW0/CW1) appear and disappear during bring-up (step 5b) */
void dmub32_log_regs(rdna3_dev *d, const char *when);
/* dmub_srv_wait_for_auto_load again, after a timed-out dmub32_hw_init */
int  dmub32_wait_boot(rdna3_dmub *dm, uint32_t timeout_us);
void dmub32_hw_reset(rdna3_dmub *dm);

/* Queue one 64-byte command into inbox1, ring the doorbell (WPTR), wait
 * until the firmware consumed it (<= timeout_us). With 'reply' the slot
 * is read back into cmd (dmub_rb_get_return_data). 0, RDNA3_ETIME,
 * RDNA3_EBUSY (ring full), RDNA3_EIO (firmware not up / bad pointers). */
int  dmub32_cmd_execute(rdna3_dmub *dm, uint8_t cmd[RD_DMUB_RB_CMD_SIZE], int reply,
                        uint32_t timeout_us);

/* dc_dmub_srv_query_caps_cmd: the first round trip through the mailbox */
int  dmub32_query_caps(rdna3_dmub *dm);

/* DMUB_CMD__VBIOS with the table's parameter block as payload (<= 60
 * bytes): what command_table2 sends instead of running the table */
int  dmub32_vbios_cmd(rdna3_dmub *dm, uint32_t sub_type, const void *payload, uint32_t len);

/* struct dmub_dig_transmitter_control_data_v1_7 (16 bytes used + reserved) */
typedef struct rdna3_dig_tx_v1_7 {
    uint8_t  phyid;           /* 0 = UNIPHYA .. */
    uint8_t  action;          /* ATOM_TRANSMITTER_ACTION_* */
    uint8_t  digmode;         /* atom_encode_mode_def, or DP lane set */
    uint8_t  lanenum;
    uint32_t symclk_10khz;
    uint8_t  hpdsel;          /* 1 = HPD1 .. 0 = none */
    uint8_t  digfe_sel;       /* bit n = DIG n front end */
    uint8_t  connobj_id;
    uint8_t  hpo_instance;
    uint8_t  txffe_lane_sel;
    uint8_t  skip_phy_ssc_reduction;
    uint8_t  reserved2[2];
    uint32_t reserved3[11];
} rdna3_dig_tx_v1_7;

/* ATOM_TRANSMITTER_ACTION_* */
#define RD_ATOM_TX_DISABLE              0
#define RD_ATOM_TX_ENABLE               1
#define RD_ATOM_TX_INIT                 7
#define RD_ATOM_TX_SETUP_VSEMPH         11
#define RD_ATOM_TX_POWER_ON             12
#define RD_ATOM_TX_POWER_OFF            13

/* transmitter_control_v1_7 through the DMCUB */
int  dmub32_transmitter_control(rdna3_dmub *dm, const rdna3_dig_tx_v1_7 *p);

/* struct set_pixel_clock_parameter_v1_7 (atomfirmware.h), the payload of
 * DMUB_CMD__VBIOS_SET_PIXEL_CLOCK (set_pixel_clock_dmcub) */
typedef struct rdna3_pixclk_v1_7 {
    uint32_t pixclk_100hz;    /* target pixel clock in 100 Hz */
    uint8_t  pll_id;          /* RD_ATOM_COMBOPHY_PLL0 + n, RD_ATOM_PPLL0 */
    uint8_t  encoderobjid;    /* encoder object id (low byte of the ATOM id) */
    uint8_t  encoder_mode;    /* RD_ATOM_ENCODER_MODE_* */
    uint8_t  miscinfo;        /* PIXEL_CLOCK_V7_MISC_* */
    uint8_t  crtc_id;         /* ATOM_CRTC1 = 0: the OTG instance */
    uint8_t  deep_color_ratio;/* 0 = 8 bpc */
} rdna3_pixclk_v1_7;

#define RD_ATOM_PPLL0              2
#define RD_ATOM_COMBOPHY_PLL0      20
#define RD_ATOM_ENCODER_MODE_DP    0
#define RD_ATOM_ENCODER_MODE_DVI   2
#define RD_ATOM_ENCODER_MODE_HDMI  3

/* set_pixel_clock_v7 through the DMCUB */
int  dmub32_set_pixel_clock(rdna3_dmub *dm, const rdna3_pixclk_v1_7 *p);

/* struct dig_encoder_stream_setup_parameters_v1_5, the payload of
 * DMUB_CMD__VBIOS_DIGX_ENCODER_CONTROL (encoder_control_dmcub); the
 * command carries the whole 12-byte union dig_encoder_control_parameters_v1_5 */
typedef struct rdna3_dig_enc_v1_5 {
    uint8_t  digid;           /* DIG front end, 0 = DIGA */
    uint8_t  action;          /* RD_ATOM_ENCODER_CMD_* */
    uint8_t  digmode;         /* RD_ATOM_ENCODER_MODE_* */
    uint8_t  lanenum;
    uint32_t pclk_10khz;
    uint8_t  bitpercolor;     /* RD_ATOM_PANEL_8BIT_PER_COLOR */
    uint8_t  dplinkrate_270mhz;
} rdna3_dig_enc_v1_5;

#define RD_ATOM_ENCODER_CMD_STREAM_SETUP 0x0F
#define RD_ATOM_ENCODER_CMD_LINK_SETUP   0x11
#define RD_ATOM_PANEL_8BIT_PER_COLOR     0x02

/* encoder_control_digx_v1_5 through the DMCUB */
int  dmub32_encoder_control(rdna3_dmub *dm, const rdna3_dig_enc_v1_5 *p);

/* the trace buffer (outbox0): entries the firmware logged since 'from' */
uint32_t dmub32_trace_wptr(rdna3_dmub *dm);

#ifdef __cplusplus
}
#endif
#endif
