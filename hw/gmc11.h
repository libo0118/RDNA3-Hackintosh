/*
 * gmc11.h - GMC 11 memory controller layout + GFXHUB 3.0 / 3.0.3
 * VMID0 GART, ported from Linux amdgpu gmc_v11_0.c, amdgpu_gmc.c and
 * gfxhub_v3_0.c / gfxhub_v3_0_3.c (MIT).
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
#define RD_PTE_FRAG(x)     (((uint64_t)(x) & 0x1full) << 7)
#define RD_PTE_PRT         (1ull << 51)
#define RD_PDE_PTE         (1ull << 54)   /* a PDE that maps (huge page) */
#define RD_PTE_TF          (1ull << 56)
#define RD_VM_NORETRY_FLAGS    (RD_PTE_EXECUTABLE | RD_PDE_PTE | RD_PTE_TF)
#define RD_VM_NORETRY_FLAGS_TF (RD_PTE_VALID | RD_PTE_SYSTEM | RD_PTE_PRT)
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

/* Validate GC discovery (11.0.2 / 11.0.3, segment 0), then place/allocate
 * GART. Must succeed before any GFXHUB access, including fault interrupts. */
int  gmc11_init(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_size);
/* Write PTEs for 'pages' 4 KiB pages starting at GART offset 'gart_off'.
 * 'pa' is a bus address (system memory, RD_PTE_SYSTEM) or a VRAM physical
 * address (rdna3_vram_pa). */
int  gmc11_gart_map(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, uint64_t pa,
                    uint32_t pages, uint64_t flags);
void gmc11_gart_unmap(rdna3_dev *d, rdna3_gmc *g, uint64_t gart_off, uint32_t pages);
/* amdgpu_gart_invalidate_tlb: VMID0's TLB on every hub the GART is on (the
 * MMHUB, and the GFXHUB once gfx11_gfxhub_enable ran): MMIO, engine 17 */
void gmc11_gart_tlb_flush(rdna3_dev *d, rdna3_gmc *g);

/* ---- GPU page faults (gmc_v11_0_process_interrupt) ----
 * A fault IV comes from client VMC (MMHUB) or GFX/UTCL2 (GFXHUB) with
 * source 0; src_data[0..1] carry the faulting page, the hub's
 * L2_PROTECTION_FAULT_STATUS the rest. Without retry faults (noretry on
 * dGPUs) the offending wave is dead; the owner of the VMID is guilty. */
#include "ih60.h"
typedef struct rdna3_vm_fault {
    int      hub;               /* 0 = GFXHUB, 1 = MMHUB */
    uint32_t vmid, pasid, client, src_id, ring_id;
    uint64_t addr;              /* faulting virtual address (page granular) */
    uint32_t status;            /* L2_PROTECTION_FAULT_STATUS */
    uint32_t cid;               /* UTCL2 client id */
    int      write, retry, more_faults, walker_error, permission_faults, mapping_error;
} rdna3_vm_fault;
int  gmc11_iv_is_vm_fault(const rdna3_iv *iv);
/* decode + read and clear the hub's fault status (RW the status cache) */
void gmc11_decode_vm_fault(rdna3_dev *d, const rdna3_iv *iv, rdna3_vm_fault *f);
void gmc11_log_vm_fault(rdna3_dev *d, const rdna3_vm_fault *f);
/* amdgpu_gmc_set_vm_fault_masks for both hubs (gmc_v11_0_vm_fault_interrupt_state) */
void gmc11_set_vm_fault_interrupts(rdna3_dev *d, int enable);
/* MMHUB side helpers (mmhub302.c) */
uint32_t mmhub302_fault_status(rdna3_dev *d, int clear);
void     mmhub302_set_vm_fault_interrupts(rdna3_dev *d, int enable);

/* gfxhub_v3_0 / gfxhub_v3_0_3: selected by validated GC discovery. */
int  gfxhub11_gart_enable(rdna3_dev *d, rdna3_gmc *g);
void gfxhub11_set_fault_enable_default(rdna3_dev *d, int value);
/* gmc_v11_0_flush_gpu_tlb() for the GFXHUB, MMIO path (before KIQ/MES) */
int  gfxhub11_flush_tlb(rdna3_dev *d, uint32_t vmid, uint32_t flush_type);
/* register offsets + request word a ring needs to switch VMID n's page
 * directory and invalidate it on engine 'eng' (gmc_v11_0_emit_flush_gpu_tlb) */
typedef struct rdna3_vm_flush_regs {
    uint32_t pt_lo, pt_hi;   /* GCVM_CONTEXTn_PAGE_TABLE_BASE_ADDR_LO32/HI32 */
    uint32_t req, ack;       /* GCVM_INVALIDATE_ENGe_REQ/ACK */
    uint32_t req_val;        /* get_invalidate_req(vmid, 0) */
} rdna3_vm_flush_regs;
void gfxhub11_flush_regs(rdna3_dev *d, uint32_t vmid, uint32_t eng, rdna3_vm_flush_regs *o);
/* the same for the MMHUB (mmhub_v3_0: vm_inv_eng0_req / ack, get_invalidate_req) */
void mmhub302_flush_regs(rdna3_dev *d, uint32_t vmid, uint32_t eng, rdna3_vm_flush_regs *o);
/* gmc_v11_0_emit_pasid_mapping: the register a ring writes the job's PASID
 * to, IH_VMID_0_LUT + vmid on the GFXHUB, IH_VMID_0_LUT_MM + vmid on the
 * MMHUB (the IH fills an IV's PASID from it) */
uint32_t gmc11_pasid_mapping_reg(rdna3_dev *d, uint32_t vmid, int mmhub);
/* gmc_v11_0_get_vbios_fb_size: the pre-OS scanout (AMDGPU_VBIOS_VGA_ALLOCATION
 * in VGA mode, else HUBP0's viewport height x pitch x 4) */
#define RD_VBIOS_VGA_ALLOCATION (9u * 1024 * 1024)
uint32_t gmc11_get_vbios_fb_size(rdna3_dev *d);
/* amdgpu_gmc_get_vbios_allocations (display hardware, no VF): the stolen
 * VGA and extended sizes amdgpu reserves at the bottom of VRAM */
void gmc11_get_vbios_allocations(rdna3_dev *d, uint64_t real_vram_size, uint64_t *stolen_vga,
                                 uint64_t *stolen_extended);
/* gfxhub_v3_0 / gfxhub_v3_0_3_setup_vm_pt_regs */
void gfxhub11_setup_vm_pt_regs(rdna3_dev *d, uint32_t vmid, uint64_t pd_addr);

/* MMHUB 3.0.2 (mmhub302.c, mmhub_v3_0_2.c): SDMA/IH/VCN/DCN/PSP side */
int      mmhub302_gart_enable(rdna3_dev *d, rdna3_gmc *g);
void     mmhub302_gart_disable(rdna3_dev *d);
void     mmhub302_set_fault_enable_default(rdna3_dev *d, int value);
int      mmhub302_flush_tlb(rdna3_dev *d, uint32_t vmid, uint32_t flush_type);
void     mmhub302_setup_vm_pt_regs(rdna3_dev *d, uint32_t vmid, uint64_t pd_addr);
uint64_t mmhub302_get_fb_location(rdna3_dev *d);
uint64_t mmhub302_get_mc_fb_offset(rdna3_dev *d);
/* gfxhub_v3_0 / gfxhub_v3_0_3_gart_disable, _get_mc_fb_offset */
void     gfxhub11_gart_disable(rdna3_dev *d, rdna3_gmc *g);
uint64_t gfxhub11_get_mc_fb_offset(rdna3_dev *d);
/* gmc_v11_0_gart_disable (MMHUB) / gmc_v11_0_hw_fini (VM fault interrupts
 * off, then the GART) */
void     gmc11_gart_disable(rdna3_dev *d);
void     gmc11_hw_fini(rdna3_dev *d);
/* amdgpu_gmc_allocate_vm_inv_eng: the invalidation engine of each ring
 * (ring_hub[i] = RD_VMHUB_*, in amdgpu's ring order) */
#define RD_VMHUB_GFX   0
#define RD_VMHUB_MM    1
#define RD_VMHUB_COUNT 2
#define RD_VM_INV_ENG_NONE 0xffffffffu
int      gmc11_allocate_vm_inv_eng(const uint32_t *ring_hub, const int *ring_is_mes, uint32_t n,
                                   int enable_mes, uint32_t *eng);
/* gmc_v11_0_gart_enable(): MMHUB GART + fault defaults + VMID0 flush */
int      gmc11_gart_enable(rdna3_dev *d, rdna3_gmc *g);

#ifdef __cplusplus
}
#endif
#endif
