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

#ifdef __cplusplus
}
#endif
#endif
