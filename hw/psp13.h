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
 *  - no TAs (ASD/HDCP/DTM/RAS/SECUREDISPLAY) and no register list (RL)
 *    by default; with rdna3amdgpu=1 (d->amdgpu_exact) psp_load_fw loads
 *    ASD, RL, HDCP and DTM as amdgpu does on MP0 13.0.10.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_PSP13_H
#define RDNA3_PSP13_H

#include "rdna3_hw.h"
#include "amd_fw.h"
#include "psp_gfx_if.h"

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
#define PSP_ASD_SHARED_MEM_SIZE  0x0u   /* amdgpu_psp.h ta_mem_size */
#define PSP_HDCP_SHARED_MEM_SIZE 0x4000u
#define PSP_DTM_SHARED_MEM_SIZE  0x4000u

/* struct ta_context: a trusted application the PSP runs (psp_ta_load) */
typedef struct rdna3_psp_ta_ctx {
    const uint8_t *bin;          /* bin_desc.start_addr, NULL = not in the TA file */
    uint32_t bin_size;           /* bin_desc.size_bytes */
    uint32_t ta_load_type;       /* GFX_CMD_ID_LOAD_TA / GFX_CMD_ID_LOAD_ASD */
    uint64_t shared;             /* VRAM offset of mem_context.shared_buf, UINT64_MAX = none */
    uint64_t shared_mc;          /* mem_context.shared_mc_addr (0 for ASD) */
    uint32_t shared_mem_size;
    uint32_t session_id;
    uint32_t resp_status;
    int      initialized;
} rdna3_psp_ta_ctx;

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
    /* psp_cmd_submit_buf: ucode->tmr_mc_addr_lo/hi, where LOAD_IP_FW put
     * each image in the TMR (resp.fw_addr), by AMDGPU_UCODE_ID_* (the VCN
     * firmware is started from there) */
    uint64_t ucode_tmr_mc[64];

    /* psp_early_init / psp_check_pmfw_centralized_cstate_management */
    int autoload_supported, boot_time_tmr, pmfw_centralized_cstate_management;
    /* acquire_psp_cmd_buf / release_psp_cmd_buf: psp->cmd, zeroed for each
     * command; the PSP is driven by one thread at a time (the caller
     * serializes, as psp->mutex does in amdgpu) */
    int cmd_busy;
    struct psp_gfx_cmd_resp cmd_buf;
    /* the TA file (psp_init_ta_microcode) and the TAs psp_load_fw starts */
    const uint8_t *ta_fw;
    size_t ta_len;
    amd_psp_ta ta;
    rdna3_psp_ta_ctx asd, hdcp, dtm;
    int rl_loaded;               /* psp_rl_load ran (the sOS file's RL binary) */
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
/* psp_execute_ip_fw_load for an image that is not a ucode of the plan: the
 * VCN DPG SRAM (amdgpu_vcn_psp_update_sram, fw_type GFX_FW_TYPE_VCN0_RAM /
 * GFX_FW_TYPE_VCN1_RAM), 'size' bytes at MC 'mc' */
int  psp13_load_ip_fw_buf(rdna3_psp *psp, uint32_t fw_type, uint64_t mc, uint32_t size);

/* Full sequence of psp_hw_init() for an RDNA3 dGPU (psp_load_fw):
 *   bootloader (KDB, SPL, SYS, SOS) -> KM ring -> TOC/TMR alloc ->
 *   SMC -> SETUP_TMR -> other ucodes, starting RLC autoload after RLC_G.
 * The verified default loads the PMFW before SETUP_TMR; with
 * d->amdgpu_exact the order follows psp_early_init's flags for the MP0
 * version (13.0.10: SETUP_TMR first, then the PMFW as the first of
 * psp_load_non_psp_fw), and ASD, the register list (RL) and the HDCP /
 * DTM TAs follow the ucodes (psp13_set_ta first for the TAs). */
int  psp13_hw_init(rdna3_psp *psp, const amd_ucode_plan *plan,
                   const amd_fw_blob_ref blobs[AMD_BLOB_COUNT]);

/* psp_early_init for MP0 13.0.x: autoload_supported, boot_time_tmr and
 * psp_check_pmfw_centralized_cstate_management by IP version (psp13_init
 * runs it) */
void psp13_early_init(rdna3_psp *psp);
/* psp_check_pmfw_centralized_cstate_management for an MP0 version */
int  psp13_pmfw_centralized_cstate_management(const rdna3_ipver *mp0);

/* the TA file (psp_13_0_10_ta.bin): psp_init_ta_microcode -> the contexts
 * of ASD, HDCP and DTM; 'fw' must stay valid while the PSP runs */
int  psp13_set_ta(rdna3_psp *psp, const uint8_t *fw, size_t len);
/* psp_ta_load / psp_ta_unload / psp_ta_invoke on one TA context */
int  psp13_ta_load(rdna3_psp *psp, rdna3_psp_ta_ctx *ctx);
int  psp13_ta_unload(rdna3_psp *psp, rdna3_psp_ta_ctx *ctx);
int  psp13_ta_invoke(rdna3_psp *psp, uint32_t ta_cmd_id, rdna3_psp_ta_ctx *ctx);
/* psp_asd_initialize / psp_asd_terminate, psp_hdcp_* and psp_dtm_*: the
 * TA command for HDCP / DTM goes into the context's shared buffer (VRAM,
 * CPU visible) before the invoke */
int  psp13_asd_initialize(rdna3_psp *psp);
int  psp13_asd_terminate(rdna3_psp *psp);
int  psp13_hdcp_initialize(rdna3_psp *psp);
int  psp13_hdcp_invoke(rdna3_psp *psp, uint32_t ta_cmd_id);
int  psp13_hdcp_terminate(rdna3_psp *psp);
int  psp13_dtm_initialize(rdna3_psp *psp);
int  psp13_dtm_invoke(rdna3_psp *psp, uint32_t ta_cmd_id);
int  psp13_dtm_terminate(rdna3_psp *psp);
/* psp_rl_load: the sOS file's register list (LOAD_IP_FW REG_LIST) */
int  psp13_rl_load(rdna3_psp *psp);
/* psp_tmr_terminate -> psp_tmr_unload (DESTROY_TMR) */
int  psp13_tmr_terminate(rdna3_psp *psp);
/* psp_v13_0_ring_destroy: ring stop, the ring buffer is forgotten */
int  psp13_ring_destroy(rdna3_psp *psp);
/* psp_hw_fini: the TAs and ASD unloaded, DESTROY_TMR, ring destroyed */
int  psp13_hw_fini(rdna3_psp *psp);
/* psp_suspend: as psp_hw_fini, but the ring is only stopped */
int  psp13_suspend(rdna3_psp *psp);

#ifdef __cplusplus
}
#endif
#endif
