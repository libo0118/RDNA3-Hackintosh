/*
 * GENERATED subset of Linux v6.12 gc_11_0_0_{offset,sh_mask,default}.h,
 * via tools/gen_regs.py; GFXHUB30_ namespace avoids the 11.0.3 register names.
 * Used common field masks and strides match gc_11_0_3_gmc.h.
 * (drivers/gpu/drm/amd/include/asic_reg, MIT, Copyright Advanced Micro Devices).
 * Do not edit; regenerate.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GC_11_0_0_GMC_H
#define GC_11_0_0_GMC_H

#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32               0x16f3
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32_BASE_IDX      0
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32               0x16f4
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32_BASE_IDX      0
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32               0x16f5
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32_BASE_IDX      0
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32              0x1713
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32_BASE_IDX     0
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32              0x1714
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32_BASE_IDX     0
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32                0x1733
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32_BASE_IDX       0
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32                0x1734
#define GFXHUB30_regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32_BASE_IDX       0
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32              0x1715
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32_BASE_IDX     0
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32              0x1716
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32_BASE_IDX     0
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32                0x1735
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32_BASE_IDX       0
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32                0x1736
#define GFXHUB30_regGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32_BASE_IDX       0
#define GFXHUB30_regGCMC_VM_AGP_BASE                                      0x167c
#define GFXHUB30_regGCMC_VM_AGP_BASE_BASE_IDX                             0
#define GFXHUB30_regGCMC_VM_AGP_BOT                                       0x167b
#define GFXHUB30_regGCMC_VM_AGP_BOT_BASE_IDX                              0
#define GFXHUB30_regGCMC_VM_AGP_TOP                                       0x167a
#define GFXHUB30_regGCMC_VM_AGP_TOP_BASE_IDX                              0
#define GFXHUB30_regGCMC_VM_SYSTEM_APERTURE_LOW_ADDR                      0x167d
#define GFXHUB30_regGCMC_VM_SYSTEM_APERTURE_LOW_ADDR_BASE_IDX             0
#define GFXHUB30_regGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR                     0x167e
#define GFXHUB30_regGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR_BASE_IDX            0
#define GFXHUB30_regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB              0x15a8
#define GFXHUB30_regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB_BASE_IDX     0
#define GFXHUB30_regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB              0x15a9
#define GFXHUB30_regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB_BASE_IDX     0
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32            0x15cb
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32_BASE_IDX   0
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32            0x15cc
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32_BASE_IDX   0
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_CNTL                         0x15c4
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_CNTL_BASE_IDX                0
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_CNTL2                        0x15c5
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_CNTL2_BASE_IDX               0
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_STATUS                       0x15c8
#define GFXHUB30_regGCVM_L2_PROTECTION_FAULT_STATUS_BASE_IDX              0
#define GFXHUB30_regGCMC_VM_MX_L1_TLB_CNTL                                0x167f
#define GFXHUB30_regGCMC_VM_MX_L1_TLB_CNTL_BASE_IDX                       0
#define GFXHUB30_regGCVM_L2_CNTL                                          0x15bc
#define GFXHUB30_regGCVM_L2_CNTL_BASE_IDX                                 0
#define GFXHUB30_regGCVM_L2_CNTL2                                         0x15bd
#define GFXHUB30_regGCVM_L2_CNTL2_BASE_IDX                                0
#define GFXHUB30_regGCVM_L2_CNTL3                                         0x15be
#define GFXHUB30_regGCVM_L2_CNTL3_BASE_IDX                                0
#define GFXHUB30_regGCVM_L2_CNTL4                                         0x15d4
#define GFXHUB30_regGCVM_L2_CNTL4_BASE_IDX                                0
#define GFXHUB30_regGCVM_L2_CNTL5                                         0x15da
#define GFXHUB30_regGCVM_L2_CNTL5_BASE_IDX                                0
#define GFXHUB30_regGCVM_CONTEXT0_CNTL                                    0x1688
#define GFXHUB30_regGCVM_CONTEXT0_CNTL_BASE_IDX                           0
#define GFXHUB30_regGCVM_CONTEXT1_CNTL                                    0x1689
#define GFXHUB30_regGCVM_CONTEXT1_CNTL_BASE_IDX                           0
#define GFXHUB30_regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32      0x15ce
#define GFXHUB30_regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32_BASE_IDX 0
#define GFXHUB30_regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32      0x15cf
#define GFXHUB30_regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32_BASE_IDX 0
#define GFXHUB30_regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32     0x15d0
#define GFXHUB30_regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32_BASE_IDX 0
#define GFXHUB30_regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32     0x15d1
#define GFXHUB30_regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32_BASE_IDX 0
#define GFXHUB30_regGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32         0x15d2
#define GFXHUB30_regGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32_BASE_IDX 0
#define GFXHUB30_regGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32         0x15d3
#define GFXHUB30_regGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32_BASE_IDX 0
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32                  0x16cf
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32_BASE_IDX         0
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_ADDR_RANGE_HI32                  0x16d0
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_ADDR_RANGE_HI32_BASE_IDX         0
#define GFXHUB30_regGCVM_INVALIDATE_ENG1_ADDR_RANGE_LO32                  0x16d1
#define GFXHUB30_regGCVM_INVALIDATE_ENG1_ADDR_RANGE_LO32_BASE_IDX         0
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_REQ                              0x16ab
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_REQ_BASE_IDX                     0
#define GFXHUB30_regGCVM_INVALIDATE_ENG1_REQ                              0x16ac
#define GFXHUB30_regGCVM_INVALIDATE_ENG1_REQ_BASE_IDX                     0
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_ACK                              0x16bd
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_ACK_BASE_IDX                     0
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_SEM                              0x1699
#define GFXHUB30_regGCVM_INVALIDATE_ENG0_SEM_BASE_IDX                     0
#define GFXHUB30_regGCMC_VM_FB_LOCATION_BASE                              0x1678
#define GFXHUB30_regGCMC_VM_FB_LOCATION_BASE_BASE_IDX                     0
#define GFXHUB30_regGCMC_VM_FB_LOCATION_TOP                               0x1679
#define GFXHUB30_regGCMC_VM_FB_LOCATION_TOP_BASE_IDX                      0
#define GFXHUB30_regGCMC_VM_FB_OFFSET                                     0x15a7
#define GFXHUB30_regGCMC_VM_FB_OFFSET_BASE_IDX                            0
#define GFXHUB30_regCP_DEBUG                                              0x1e1f
#define GFXHUB30_regCP_DEBUG_BASE_IDX                                     0
#define GFXHUB30_regGCVM_L2_CNTL3_DEFAULT                                 0x80120007
#define GFXHUB30_regGCVM_L2_CNTL4_DEFAULT                                 0x000000c1
#define GFXHUB30_regGCVM_L2_CNTL5_DEFAULT                                 0x00003fe0
#define GFXHUB30_CP_DEBUG__CPG_UTCL1_ERROR_HALT_DISABLE__SHIFT            0xf
#define GFXHUB30_CP_DEBUG__CPG_UTCL1_ERROR_HALT_DISABLE_MASK              0x00008000L

#endif /* GC_11_0_0_GMC_H */
