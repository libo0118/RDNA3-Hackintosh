/*
 * gmc11.h - GMC 11 memory controller layout + GFXHUB 3.0.3 (GC 11.0.3)
 * VMID0 GART, ported from Linux amdgpu gmc_v11_0.c, amdgpu_gmc.c and
 * gfxhub_v3_0_3.c (MIT).
 *
 * Scope today: the system-domain GART that kernel-side engines use
 * (rings, fences, MES/KIQ buffers), with its flat page table in VRAM,
 * plus the per-VMID context setup userspace VMs will use. MMHUB 3.0.2
 * (SDMA/IH/DCN/VCN) is in mmhub302.c; gmc11_gart_enable() enables MMHUB
 * like gmc_v11_0_gart_enable, while GFXHUB follows in gfx hw_init.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_GMC11_H
#define RDNA3_GMC11_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* amdgpu_vm.h PTE bits */
#define RD_PTE_VALID       (1ull << 0)
#define RD_PTE_SYSTEM      (1ull << 1)
#define RD_PTE_SNOOPED     (1ull << 2)
#define RD_PTE_EXECUTABLE  (1ull << 4)
#define RD_PTE_READABLE    (1ull << 5)
#define RD_PTE_WRITEABLE   (1ull << 6)
#define RD_PTE_MTYPE_NV10(m) ((uint64_t)(m) << 48)
#define RD_MTYPE_NC 0
#define RD_MTYPE_UC 3

#define RD_GMC_HOLE_START  0x0000800000000000ull   /* 48-bit VA, max_level 3 */
#define RD_GART_SIZE_DEFAULT (512ull << 20)         /* amdgpu_gart_size default */

typedef struct rdna3_gmc {
    uint64_t gart_start, gart_end, gart_size;   /* MC addresses */
    uint64_t agp_start, agp_end;                /* AGP disabled: start > end */
    uint64_t gart_table;                        /* VRAM offset of the flat PTE table */
    uint64_t scratch;                           /* VRAM offset, 4 KiB (mem_scratch) */
    uint64_t dummy_page_pa;                     /* fault default page (bus address) */
    uint64_t max_pfn;                           /* 1 << (48 - 12) */
    uint32_t num_level, block_size;             /* amdgpu_vm_adjust_size(.., 9, 3, 48) */
    uint32_t vm_cntx_cntl, mm_vm_cntx_cntl;
    int      gfxhub_enabled, mmhub_enabled;
} rdna3_gmc;

/* Place GART (BEST_FIT, 4 GiB aligned), allocate table + scratch in VRAM. */
int  gmc11_init(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_size);
/* Write PTEs for 'pages' 4 KiB pages starting at GART offset 'gart_off'.
 * 'pa' is a bus address (system memory, RD_PTE_SYSTEM) or a VRAM physical
 * address (rdna3_vram_pa). */
int  gmc11_gart_map(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, uint64_t pa,
                    uint32_t pages, uint64_t flags);
void gmc11_gart_unmap(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, uint32_t pages);

/* gfxhub_v3_0_3_gart_enable() + set_fault_enable_default() */
int  gfxhub303_gart_enable(rdna3_dev *d, rdna3_gmc *g);
void gfxhub303_set_fault_enable_default(rdna3_dev *d, int value);
/* gmc_v11_0_flush_gpu_tlb() for the GFXHUB, MMIO path (before KIQ/MES) */
int  gfxhub303_flush_tlb(rdna3_dev *d, uint32_t vmid, uint32_t flush_type);
/* program VMID n's page directory base (gfxhub_v3_0_3_setup_vm_pt_regs) */
void gfxhub303_setup_vm_pt_regs(rdna3_dev *d, uint32_t vmid, uint64_t pd_addr);

/* MMHUB 3.0.2 (mmhub302.c, mmhub_v3_0_2.c): SDMA/IH/VCN/DCN/PSP side */
int      mmhub302_gart_enable(rdna3_dev *d, rdna3_gmc *g);
void     mmhub302_gart_disable(rdna3_dev *d);
void     mmhub302_set_fault_enable_default(rdna3_dev *d, int value);
int      mmhub302_flush_tlb(rdna3_dev *d, uint32_t vmid, uint32_t flush_type);
void     mmhub302_setup_vm_pt_regs(rdna3_dev *d, uint32_t vmid, uint64_t pd_addr);
uint64_t mmhub302_get_fb_location(rdna3_dev *d);
/* gmc_v11_0_gart_enable(): MMHUB GART + fault defaults + VMID0 flush */
int      gmc11_gart_enable(rdna3_dev *d, rdna3_gmc *g);

#ifdef __cplusplus
}
#endif
#endif
