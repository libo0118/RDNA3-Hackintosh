/*
 * vbios.h - the card's ATOM VBIOS, read through the SMUIO ROM window the way
 * amdgpu does on SOC21 (amdgpu_soc15_read_bios_from_rom), and the data-table
 * directory lookup of atom.c (amdgpu_atom_parse_data_header). The driver only
 * needs a few data tables from it: firmwareinfo (boot-up clocks and
 * voltages), smu_info, smc_dpm_info (the PMFW BoardTable) and
 * vram_usagebyfirmware. No command tables are ever executed.
 *
 * Ported from Linux amdgpu amdgpu_bios.c / atom.c / atomfirmware.h (MIT).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_VBIOS_H
#define RDNA3_VBIOS_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_VBIOS_MAX_LEN   (256u * 1024u)   /* ROM images are 64-128 KiB */

/* atomfirmware.h: struct atom_common_table_header */
typedef struct rdna3_atom_header {
    uint16_t structuresize;
    uint8_t  format_revision;
    uint8_t  content_revision;
} rdna3_atom_header;

/* indices into atom_master_list_of_data_tables_v2_1 */
enum {
    RD_ATOM_DT_UTILITYPIPELINE = 0,
    RD_ATOM_DT_MULTIMEDIA_INFO = 1,
    RD_ATOM_DT_SMC_DPM_INFO    = 2,
    RD_ATOM_DT_FIRMWAREINFO    = 4,
    RD_ATOM_DT_LCD_INFO        = 6,
    RD_ATOM_DT_SMU_INFO        = 8,
    RD_ATOM_DT_VRAM_USAGEBYFIRMWARE = 11,
    RD_ATOM_DT_GPIO_PIN_LUT    = 12,
    RD_ATOM_DT_GFX_INFO        = 14,
    RD_ATOM_DT_POWERPLAYINFO   = 15,
    RD_ATOM_DT_DISPLAYOBJECTINFO = 22,
    RD_ATOM_DT_UMC_INFO        = 24,
    RD_ATOM_DT_DCE_INFO        = 27,
    RD_ATOM_DT_VRAM_INFO       = 28,
    RD_ATOM_DT_VOLTAGEOBJECT_INFO = 32,
    RD_ATOM_DT_COUNT           = 35,
};

typedef struct rdna3_vbios {
    uint8_t *data;              /* caller's buffer */
    uint32_t cap;               /* its size */
    uint32_t len;               /* image length (header[2] * 512) */
    uint32_t rom_offset;        /* REGS_ROM_OFFSET_CTRL.ROM_OFFSET << 17 */
    uint16_t rom_header;        /* offset of atom_rom_header_v2_2 */
    uint16_t data_table;        /* offset of atom_master_data_table_v2_1 */
    char     part_number[32];   /* from the ROM header's string, when present */
} rdna3_vbios;

/* amdgpu_atom_parse's identification strings (atom_get_vbios_name / _pn /
 * _date / _version) for AMDGPU_INFO_VBIOS_INFO, and the firmware revision
 * (atom_firmware_info.firmware_revision). */
typedef struct rdna3_vbios_strings {
    char     name[64], pn[64], ver_str[32], date[32];
    uint32_t version;
} rdna3_vbios_strings;
void vbios_strings(const rdna3_vbios *v, rdna3_vbios_strings *s);

/* Read the image through ROM_INDEX/ROM_DATA into v->data (v->cap bytes).
 * Validates 0x55AA, the " 761295520" signature at 0x30 and the "ATOM"
 * ROM header (check_atom_bios), then locates the data table directory.
 * Returns RDNA3_ENODEV when the window does not hold an ATOM BIOS. */
int vbios_read(rdna3_dev *d, rdna3_vbios *v, uint8_t *buf, uint32_t cap);

/* Validate an image already in memory (tests, or a ROM obtained another
 * way) and fill in the offsets. */
int vbios_parse(rdna3_dev *d, rdna3_vbios *v, uint8_t *buf, uint32_t len);

/* amdgpu_atombios_get_data_table: pointer to data table 'index' (its
 * atom_common_table_header first), with size and revisions; NULL when the
 * directory has no such table or it does not fit the image. */
const uint8_t *vbios_data_table(const rdna3_vbios *v, unsigned index, uint16_t *size,
                                uint8_t *frev, uint8_t *crev);

/* Boot-up values the SMU code needs (smu_v13_0_get_vbios_bootup_values):
 * clocks in 10 kHz, voltages in mV. Zero when the table lacks them. */
typedef struct rdna3_vbios_boot {
    uint32_t firmware_revision;
    uint32_t gfxclk, uclk, socclk, dcefclk, vclk, dclk, fclk;
    uint16_t vddc, vddci, mvddc, vdd_gfx;
    uint8_t  cooling_id;
    uint32_t pp_table_id;       /* != 0: PMFW's own table instead of VBIOS */
    uint32_t firmware_capability;
    uint8_t  fw_frev, fw_crev;  /* firmwareinfo revisions seen */
    uint32_t fw_reserved_kb;    /* VRAM the firmware keeps at the top */
} rdna3_vbios_boot;
int vbios_bootup_values(rdna3_dev *d, const rdna3_vbios *v, rdna3_vbios_boot *b);

/* vram_usagebyfirmware v2.1: start (in 16 KiB units) and size (KiB) of the
 * firmware's reservation at the top of VRAM. 0 when absent. */
int vbios_fw_vram_reservation(const rdna3_vbios *v, uint64_t *start, uint64_t *size);

/* amdgpu_atomfirmware_get_fw_reserved_fb_size: firmwareinfo v3.4/v3.5
 * fw_reserved_size_in_kb, the block amdgpu_ttm_reserve_tmr keeps at the top
 * of VRAM (IP discovery, memory training, PSP runtime data). 0 if absent. */
uint64_t vbios_fw_reserved_fb_size(const rdna3_vbios *v);

/* amdgpu_atomfirmware_allocate_fb_scratch (+ _allocate_fb_v2_1 / _v2_2):
 * the size of the ATOM interpreter's FB scratch - vram_usagebyfirmware
 * v2.1's used_by_driver_in_kb unless the block is the SR-IOV message
 * reservation, else (and for v2.2 on bare metal) amdgpu's 20 KiB */
uint32_t vbios_atom_scratch_bytes(const rdna3_vbios *v);

/* amdgpu_atomfirmware_dynamic_boot_config_supported /
 * amdgpu_atomfirmware_mem_training_supported on firmwareinfo's
 * firmware_capability (rdna3_vbios_boot.firmware_capability) */
#define RD_ATOM_FW_CAP_2STAGE_BIST_TRAINING  0x00000400u
#define RD_ATOM_FW_CAP_DYNAMIC_BOOT_CFG      0x00020000u
static inline int vbios_dynamic_boot_config_supported(uint32_t fw_cap)
{
    return (fw_cap & RD_ATOM_FW_CAP_DYNAMIC_BOOT_CFG) != 0;
}
static inline int vbios_mem_training_supported(uint32_t fw_cap)
{
    return (fw_cap & RD_ATOM_FW_CAP_2STAGE_BIST_TRAINING) != 0;
}

/* amdgpu_atomfirmware_get_clock_info: firmwareinfo's boot sclk / mclk
 * (adev->clock.default_sclk / _mclk, 10 kHz), the system PLL reference
 * (smu_info, then gfx_info's RLC timer / golden TSC reference on Navi+:
 * soc21_get_xclk) and the memory PLL reference (umc_info). RDNA3_EINVAL
 * when no table answered (amdgpu fails amdgpu_device_init there). */
typedef struct rdna3_vbios_clocks {
    uint32_t default_sclk, default_mclk;
    uint32_t spll_ref, mpll_ref;
} rdna3_vbios_clocks;
int vbios_clock_info(const rdna3_vbios *v, rdna3_vbios_clocks *c);

/* amdgpu_atomfirmware_get_vram_info for a dGPU before GC 12: vram_info
 * v2.3 - v2.6 (the module the BIOS scratch register 4 names) or v3.0.
 * 'module_id' = (BIOS_SCRATCH_4 & 0x00ff0000) >> 16. Type in
 * AMDGPU_VRAM_TYPE_* (convert_atom_mem_type_to_vram_type), width in bits,
 * vendor = the low nibble of the module's vendor id. */
#define RD_VRAM_TYPE_UNKNOWN 0
#define RD_VRAM_TYPE_GDDR5   5
#define RD_VRAM_TYPE_HBM     6
#define RD_VRAM_TYPE_GDDR6   9
typedef struct rdna3_vbios_vram {
    uint32_t type, width, vendor;
} rdna3_vbios_vram;
int vbios_vram_info(const rdna3_vbios *v, uint32_t module_id, rdna3_vbios_vram *out);

/* ---- display object table (displayobjectinfo v1.4 / v1.5, bios_parser2) ----
 * One entry per display path: connector -> encoder (UNIPHY A..G), the DDC
 * line that feeds its AUX engine and the HPD pin. Which OTG drives a path
 * is not in the VBIOS: Display Core assigns pipes at modeset time. */
#define RD_ATOM_OBJ_TYPE(id)  (((id) >> 12) & 0x7)
#define RD_ATOM_OBJ_ENUM(id)  (((id) >> 8) & 0x7)
#define RD_ATOM_OBJ_ID(id)    ((id) & 0xff)
#define RD_ATOM_OBJ_TYPE_ENCODER   2
#define RD_ATOM_OBJ_TYPE_CONNECTOR 3

#define RD_ATOM_CONN_DVI_D        0x03
#define RD_ATOM_CONN_DUAL_DVI_D   0x04
#define RD_ATOM_CONN_HDMI_A       0x0C
#define RD_ATOM_CONN_DISPLAYPORT  0x13
#define RD_ATOM_CONN_EDP          0x14
#define RD_ATOM_CONN_USBC         0x17

#define RD_ATOM_ENC_UNIPHY   0x1E   /* A (enum 1) / B (enum 2) */
#define RD_ATOM_ENC_UNIPHY1  0x20   /* C / D */
#define RD_ATOM_ENC_UNIPHY2  0x21   /* E / F */
#define RD_ATOM_ENC_UNIPHY3  0x25   /* G */

/* atom_encoder_caps_record.encodercaps */
#define RD_ATOM_ENC_CAP_MST       0x01
#define RD_ATOM_ENC_CAP_HBR2      0x02
#define RD_ATOM_ENC_CAP_HDMI6G    0x04
#define RD_ATOM_ENC_CAP_HBR3      0x08
#define RD_ATOM_ENC_CAP_DP2       0x10
#define RD_ATOM_ENC_CAP_UHBR10    0x20
#define RD_ATOM_ENC_CAP_UHBR13_5  0x40
#define RD_ATOM_ENC_CAP_UHBR20    0x80
#define RD_ATOM_ENC_CAP_USB_C     0x100

#define RD_VBIOS_MAX_PATHS 8

typedef struct rdna3_display_path {
    uint16_t connector_objid;   /* raw ATOM object ids */
    uint16_t encoder_objid;
    uint16_t device_tag;
    uint8_t  connector;         /* RD_ATOM_CONN_* */
    uint8_t  connector_enum;    /* 1-based instance of that connector type */
    int8_t   transmitter;       /* 0 = UNIPHY A .. 6 = G, -1 unknown */
    int8_t   ddc_line;          /* DDC1 = 0 .. DDC5 = 4: the AUX engine; -1 none */
    int8_t   hpd;               /* HPD1 = 0 .. HPD6 = 5; -1 none */
    uint8_t  i2c_id;            /* atom_i2c_record.i2c_id */
    uint8_t  hpd_pin_id;        /* atom_hpd_int_record.pin_id (GPIO_PIN_LUT id) */
    uint8_t  hpd_active;        /* plugin_pin_state */
    uint32_t encoder_caps;      /* RD_ATOM_ENC_CAP_*, 0 when no record */
    uint32_t connector_caps;
    uint32_t max_link_mhz;      /* ATOM_CONNECTOR_SPEED_UPTO, 0 when absent */
} rdna3_display_path;

/* Parse displayobjectinfo into 'out' (up to 'max' paths, connectors only).
 * Returns the number of paths, 0 when the table is absent, RDNA3_EINVAL
 * for a malformed one. */
int vbios_display_paths(const rdna3_vbios *v, rdna3_display_path *out, unsigned max);

const char *vbios_connector_name(uint8_t connector);

/* bios_parser_get_lttpr_caps / bios_parser_get_lttpr_interop
 * (get_disp_caps_v4_1 .. v4_5): the low byte of the dce_info table's
 * display_caps (atom_display_controller_info_v4_x, right after the
 * header), as DC reads it into a uint8_t. RDNA3_ENOENT without the table
 * or for another revision than 4.1..4.5. */
#define RD_DCE_INFO_CAPS_LTTPR_SUPPORT_ENABLE          0x20
#define RD_DCE_INFO_CAPS_VBIOS_LTTPR_TRANSPARENT_ENABLE 0x40
int vbios_dce_info_caps(const rdna3_vbios *v, uint8_t *dce_caps);

#ifdef __cplusplus
}
#endif
#endif
