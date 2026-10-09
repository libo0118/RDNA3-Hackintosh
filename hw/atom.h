/*
 * atom.h - AtomBIOS command-table interpreter, ported from Linux amdgpu
 * atom.c (Stanislaw Skowronek / AMD, MIT). The VBIOS carries bytecode
 * "command tables" (ASIC_Init, DIGxEncoderControl, DIG1TransmitterControl,
 * SetPixelClock, EnableDispPowerGating, ...) that Display Core calls
 * through bios_parser2 / command_table2 to program PHYs and PLLs whose
 * sequences AMD does not otherwise publish.
 *
 * Differences from the Linux file:
 *  - no allocation: the workspace of nested tables comes from a pool inside
 *    the context, and the caller hands in the scratch ("FB") area;
 *  - every byte the bytecode reads from the image is bounds-checked, an
 *    out-of-range access aborts the table instead of reading past the ROM;
 *  - the 20 s "stuck in loop" guard counts the delays the table asked for
 *    plus an opcode budget instead of jiffies.
 *
 * Register operands are absolute MMIO dword indices, as in amdgpu's
 * cail_reg_read/cail_reg_write. PLL and MC spaces do not exist on SOC21;
 * like amdgpu the interpreter logs and returns 0 for them.
 *
 * Not thread-safe: the caller serialises (amdgpu holds ctx->mutex).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_ATOM_H
#define RDNA3_ATOM_H

#include "rdna3_hw.h"
#include "vbios.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_ATOM_MAX_DEPTH     32          /* ATOM_EXECUTE_MAX_DEPTH */
#define RD_ATOM_WS_POOL       2048        /* dwords of workspace for all nesting levels */
#define RD_ATOM_TIMEOUT_US    20000000u   /* ATOM_CMD_TIMEOUT_SEC */
#define RD_ATOM_OP_BUDGET     50000000u   /* opcodes per top-level call */

/* atom_master_list_of_command_functions_v2_1 indices DC uses */
enum {
    RD_ATOM_CMD_ASIC_INIT              = 0,
    RD_ATOM_CMD_DIGX_ENCODER_CONTROL   = 4,
    RD_ATOM_CMD_SET_ENGINE_CLOCK       = 10,
    RD_ATOM_CMD_SET_MEMORY_CLOCK       = 11,
    RD_ATOM_CMD_SET_PIXEL_CLOCK        = 12,
    RD_ATOM_CMD_ENABLE_DISP_POWER_GATING = 13,
    RD_ATOM_CMD_BLANK_CRTC             = 34,
    RD_ATOM_CMD_ENABLE_CRTC            = 35,
    RD_ATOM_CMD_GET_SMU_CLOCK_INFO     = 41,
    RD_ATOM_CMD_SELECT_CRTC_SOURCE     = 42,
    RD_ATOM_CMD_SET_DCE_CLOCK          = 46,
    RD_ATOM_CMD_SET_CRTC_USING_DTD     = 49,
    RD_ATOM_CMD_EXTERNAL_ENCODER_CONTROL = 50,
    RD_ATOM_CMD_PROCESS_I2C_CHANNEL    = 54,
    RD_ATOM_CMD_DIG1_TRANSMITTER_CONTROL = 76,
    RD_ATOM_CMD_PROCESS_AUX_CHANNEL    = 78,
    RD_ATOM_CMD_COUNT                  = 81,
};

typedef struct rdna3_atom {
    rdna3_dev     *d;
    const uint8_t *bios;
    uint32_t       bios_size;
    uint16_t       cmd_table, data_table;
    uint16_t       iio[256];           /* indirect IO methods (atom_index_iio) */

    /* interpreter state shared by nested tables (struct atom_context) */
    uint16_t data_block;
    uint32_t fb_base;
    uint32_t divmul[2];
    uint16_t io_attr;
    uint16_t reg_block;
    uint8_t  shift;
    int      cs_equal, cs_above;
    int      io_mode;
    uint32_t *scratch;                 /* "FB" space: firmware scratch, caller-owned */
    uint32_t  scratch_bytes;

    unsigned depth;
    uint32_t ws_pool[RD_ATOM_WS_POOL];
    uint32_t ws_top;                   /* dwords in use */
    uint64_t ops, delay_us;            /* budget of the current top-level call */
    int      bad_access;               /* an operand fell outside the image */
    int      trace;                    /* log every opcode (RLOG) */

    /* statistics */
    uint32_t tables_run, unknown_ops, unimplemented_io;
} rdna3_atom;

/* amdgpu_atom_parse: validates the image (vbios_parse must have accepted
 * it), finds the command and data tables and indexes the IIO methods.
 * 'scratch' may be NULL (FB operands then read 0 and writes are dropped). */
int  atom_init(rdna3_atom *a, rdna3_dev *d, const rdna3_vbios *v, uint32_t *scratch,
               uint32_t scratch_bytes);

/* amdgpu_atom_execute_table: resets the per-call state and runs command
 * table 'index' with 'params' (params_dwords dwords, read and written in
 * place). 0, RDNA3_ENOENT (no such table), RDNA3_EINVAL (abort: bad opcode
 * stream, out-of-image access, stuck loop) or RDNA3_ENOMEM (workspace). */
int  atom_execute(rdna3_atom *a, unsigned index, uint32_t *params, unsigned params_dwords);

/* amdgpu_atom_parse_cmd_header: 1 when table 'index' exists, with its
 * format/content revisions (DC picks the parameter layout by them). */
int  atom_cmd_revision(const rdna3_atom *a, unsigned index, uint8_t *frev, uint8_t *crev);

/* amdgpu_atomfirmware_asic_init: ASIC_Init v2.1 with the firmwareinfo boot clocks. */
int  atom_asic_init(rdna3_atom *a);
/* amdgpu_device_need_post for atomfirmware dGPUs: 1 = not posted (BIOS
 * scratch 7 lacks ASIC_INIT_COMPLETE), 0 = posted, < 0 = no firmware_info */
int  atom_need_post(rdna3_atom *a);

#ifdef __cplusplus
}
#endif
#endif
