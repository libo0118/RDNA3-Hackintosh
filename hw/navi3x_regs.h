/*
 * navi3x_regs.h - register offsets used by the bring-up code.
 *
 * Every value is copied from the Linux amdgpu register headers (MIT, AMD);
 * the source header is named next to each group. Offsets are DWORD
 * indices relative to reg_base[ip][inst][BASE_IDX].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NAVI3X_REGS_H
#define NAVI3X_REGS_H

/* ---- fixed, pre-discovery (amdgpu_discovery.c) ---- */
#define mmMM_INDEX                      0x0000
#define mmMM_DATA                       0x0001
#define mmMM_INDEX_HI                   0x0006
#define mmRCC_CONFIG_MEMSIZE            0x0de3
#define mmMP0_SMN_C2PMSG_33_FIXED       0x16061
#define mmDRIVER_SCRATCH_0              0x0094
#define mmDRIVER_SCRATCH_1              0x0095
#define mmDRIVER_SCRATCH_2              0x0096

/* ---- MP0 / PSP 13.0.x (asic_reg/mp/mp_13_0_2_offset.h, used by psp_v13_0.c) ---- */
#define regMP0_SMN_C2PMSG_33            0x0061
#define regMP0_SMN_C2PMSG_33_BASE_IDX   0
#define regMP0_SMN_C2PMSG_35            0x0063
#define regMP0_SMN_C2PMSG_35_BASE_IDX   0
#define regMP0_SMN_C2PMSG_36            0x0064
#define regMP0_SMN_C2PMSG_36_BASE_IDX   0
#define regMP0_SMN_C2PMSG_58            0x007a
#define regMP0_SMN_C2PMSG_58_BASE_IDX   0
#define regMP0_SMN_C2PMSG_64            0x0080
#define regMP0_SMN_C2PMSG_64_BASE_IDX   0
#define regMP0_SMN_C2PMSG_67            0x0083
#define regMP0_SMN_C2PMSG_67_BASE_IDX   0
#define regMP0_SMN_C2PMSG_69            0x0085
#define regMP0_SMN_C2PMSG_69_BASE_IDX   0
#define regMP0_SMN_C2PMSG_70            0x0086
#define regMP0_SMN_C2PMSG_70_BASE_IDX   0
#define regMP0_SMN_C2PMSG_71            0x0087
#define regMP0_SMN_C2PMSG_71_BASE_IDX   0
#define regMP0_SMN_C2PMSG_81            0x0091
#define regMP0_SMN_C2PMSG_81_BASE_IDX   0

/* ---- MP1 / SMU 13.0.x (asic_reg/mp/mp_13_0_0_offset.h, smu_v13_0.c mailbox) ---- */
#define regMP1_SMN_C2PMSG_66            0x0282   /* message */
#define regMP1_SMN_C2PMSG_66_BASE_IDX   0
#define regMP1_SMN_C2PMSG_82            0x0292   /* argument */
#define regMP1_SMN_C2PMSG_82_BASE_IDX   0
#define regMP1_SMN_C2PMSG_90            0x029a   /* response */
#define regMP1_SMN_C2PMSG_90_BASE_IDX   0

/* ---- GC 11.0.3 (asic_reg/gc/gc_11_0_3_offset.h / _sh_mask.h) ---- */
#define regGRBM_STATUS                  0x0da4
#define regGRBM_STATUS_BASE_IDX         0
#define regGCMC_VM_FB_OFFSET            0x15a7
#define regGCMC_VM_FB_OFFSET_BASE_IDX   0
#define regGCMC_VM_FB_LOCATION_BASE     0x1688
#define regGCMC_VM_FB_LOCATION_BASE_BASE_IDX 0
#define regGCMC_VM_FB_LOCATION_TOP      0x1689
#define regGCMC_VM_FB_LOCATION_TOP_BASE_IDX  0
#ifndef GCMC_VM_FB_LOCATION_BASE__FB_BASE_MASK
#define GCMC_VM_FB_LOCATION_BASE__FB_BASE_MASK 0x00FFFFFFL
#define GCMC_VM_FB_LOCATION_TOP__FB_TOP_MASK   0x00FFFFFFL
#endif

/* ---- NBIO 4.3 (asic_reg/nbio/nbio_4_3_0_offset.h) ---- */
#define regBIF_BX_PF0_RSMU_INDEX        0x0000
#define regBIF_BX_PF0_RSMU_INDEX_BASE_IDX 1
#define regBIF_BX_PF0_RSMU_DATA         0x0001
#define regBIF_BX_PF0_RSMU_DATA_BASE_IDX  1
/* the HDP flush engine CP/SDMA packets kick (nbio_v4_3_get_hdp_flush_*_offset) */
#define regBIF_BX_PF0_GPU_HDP_FLUSH_REQ            0x0106
#define regBIF_BX_PF0_GPU_HDP_FLUSH_REQ_BASE_IDX   2
#define regBIF_BX_PF0_GPU_HDP_FLUSH_DONE           0x0107
#define regBIF_BX_PF0_GPU_HDP_FLUSH_DONE_BASE_IDX  2
#define BIF_BX_PF_GPU_HDP_FLUSH_DONE__CP0_MASK     0x00000001u
#define BIF_BX_PF_GPU_HDP_FLUSH_DONE__CP2_MASK     0x00000004u
#define BIF_BX_PF_GPU_HDP_FLUSH_DONE__CP9_MASK     0x00000200u
#define BIF_BX_PF_GPU_HDP_FLUSH_DONE__SDMA0_MASK   0x00000400u
#define regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL          0x012d
#define regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL_BASE_IDX 2
#define regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL          0x012e
#define regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL_BASE_IDX 2
#define regRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE          0x00c3
#define regRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE_BASE_IDX 2

/* nbio_v4_3.c / kfd_ioctl.h */
#define RDNA3_MMIO_REG_HOLE_OFFSET      (0x80000u - 4096u)
#define KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL 0
#define KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL 4

#endif /* NAVI3X_REGS_H */
