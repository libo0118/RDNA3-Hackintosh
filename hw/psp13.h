/*
 * psp13.h - PSP 13.0.x (MP0) bring-up for Navi 3x, ported from Linux
 * amdgpu psp_v13_0.c and amdgpu_psp.c (MIT).
 *
 * Differences from amdgpu, all deliberate for early bring-up:
 *  - every PSP buffer (fw_pri, fence, cmd, KM ring, TMR, ucode buffer)
 *    lives in VRAM, addressed by MC address = FB base + offset, and is
 *    written by the CPU through BAR0 followed by an HDP flush. amdgpu
 *    supports this layout too (debug_use_vram_fw_buf); it means no GART
 *    is needed before the PSP is up;
 *  - no two-stage DRAM training (the IFWI trains memory at power-on;
 *    amdgpu's optional long training is skipped);
 *  - no TAs (ASD/HDCP/DTM/RAS/SECUREDISPLAY): not needed for GFX/DCN.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_PSP13_H
#define RDNA3_PSP13_H

#include "rdna3_hw.h"
#include "amd_fw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSP_1_MEG             0x100000u
#define PSP_FENCE_BUFFER_SIZE 0x1000u
#define PSP_CMD_BUFFER_SIZE   0x1000u
#define PSP_RING_SIZE         0x1000u
#define PSP_TMR_SIZE_DEFAULT  0x400000u
#define PSP_TMR_ALIGNMENT     0x100000u
#define PSP_CMD_TIMEOUT_ITERS 20000u   /* adev->psp_timeout */

typedef struct rdna3_psp {
    rdna3_dev *d;

    const uint8_t *sos_fw;
    size_t sos_len;
    amd_psp_sos sos;
    uint32_t sos_fw_version;   /* C2PMSG_58 after SOS boots */

    /* VRAM offsets */
    uint64_t fw_pri, fence, cmd, ring, tmr, fw_buf;
    uint32_t tmr_size, fw_buf_size;

    uint32_t fence_value;
    uint32_t last_resp_status;
    uint32_t last_tmr_size_from_toc;
    int      ring_created;
} rdna3_psp;

/* Allocates PSP buffers in VRAM and parses the SOS container. */
int psp13_init(rdna3_psp *psp, rdna3_dev *d, const uint8_t *sos_fw, size_t sos_len);

int  psp13_is_sos_alive(rdna3_psp *psp);
/* C2PMSG_35 bit 31: the bootloader is up (also what a mode-1 reset ends in) */
int  psp13_wait_for_bootloader(rdna3_psp *psp);
/* neither sOS alive (C2PMSG_81) nor the bootloader ready (C2PMSG_35 bit 31):
 * a PSP that a PCI bus reset left half-initialised (passthrough after the
 * host driver); only a mode-1 reset brings the bootloader back */
int  psp13_bootloader_stuck(rdna3_psp *psp);
/* psp_get_runtime_db_entry(PSP_RUNTIME_ENTRY_TYPE_PPTABLE_ERR_STATUS), as
 * psp_sw_init uses it: the PSP runtime database 1 MiB below the top of
 * VRAM (read through MM_INDEX/MM_DATA). Returns 1 when SCPM is enabled
 * (*status = SCPM_ENABLE / SCPM_ENABLE_WITH_SCPM_ERR), 0 when disabled or
 * there is no database. */
#define RD_PSP_RUNTIME_DB_OFFSET     0x100000u
#define RD_PSP_RUNTIME_DB_COOKIE_ID  0x0ed5u
#define RD_PSP_RUNTIME_DB_MAX        0x40u
#define RD_PSP_RUNTIME_ENTRY_SCPM    0x6u
int  psp13_scpm_enabled(rdna3_dev *d, uint32_t *status);
/* C2PMSG 33/35/36/58/64/81 into the log */
void psp13_log_state(rdna3_psp *psp, const char *when);
int  psp13_bootloader_load_all(rdna3_psp *psp);   /* KDB..SOS (psp_hw_start part 1) */
int  psp13_ring_create(rdna3_psp *psp);
int  psp13_ring_stop(rdna3_psp *psp);
int  psp13_load_toc(rdna3_psp *psp, uint32_t *tmr_size);
int  psp13_tmr_init(rdna3_psp *psp);
int  psp13_tmr_load(rdna3_psp *psp);
int  psp13_autoload_rlc(rdna3_psp *psp);

/* Copy ucodes of the plan into a VRAM buffer (amdgpu_ucode_init_bo). */
int  psp13_stage_ucodes(rdna3_psp *psp, const amd_ucode_plan *plan,
                        const amd_fw_blob_ref blobs[AMD_BLOB_COUNT],
                        uint64_t mc_out[AMD_UCODE_MAX]);
int  psp13_load_ip_fw(rdna3_psp *psp, const amd_ucode *u, uint64_t mc);

/* Full sequence of psp_hw_init() for an RDNA3 dGPU:
 *   bootloader (KDB, SPL, SYS, SOS) -> KM ring -> TOC/TMR alloc ->
 *   SMC (pmfw centralized cstate) -> SETUP_TMR -> other ucodes, starting
 *   RLC autoload after RLC_G. */
int  psp13_hw_init(rdna3_psp *psp, const amd_ucode_plan *plan,
                   const amd_fw_blob_ref blobs[AMD_BLOB_COUNT]);

#ifdef __cplusplus
}
#endif
#endif
