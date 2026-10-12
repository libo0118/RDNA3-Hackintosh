/*
 * gpuvm.h - per-process GPU page tables for GFX11 (Linux amdgpu_vm.c /
 * amdgpu_vm_pt.c layout, gmc_v11_0 PTE/PDE format; MIT).
 *
 * Layout (amdgpu_vm_adjust_size(.., 9, 3, 48), max_level 3):
 *   PDB2 va[47:39] -> PDB1 va[38:30] -> PDB0 va[29:21] -> PTB va[20:12]
 * 512 eight-byte entries per 4 KiB table, every table in VRAM. A PDE is the
 * physical address of the next table | VALID; a PTE is the page's address
 * | flags (RD_PTE_* in gmc11.h). VMIDs 1..15 run with PAGE_TABLE_DEPTH 3
 * (gfxhub303/mmhub302 setup_vmid_config), the root goes to
 * *_setup_vm_pt_regs(vmid, gpuvm_pd_addr()).
 *
 * Tables come from a fixed pool of 4 KiB VRAM pages tracked by a bitmap,
 * so the module needs no allocator from the OS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_GPUVM_H
#define RDNA3_GPUVM_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_VM_LEVELS      4
#define RD_VM_ENTRIES     512
#define RD_VM_VA_BITS     48
#define RD_VM_VA_MASK     ((1ull << RD_VM_VA_BITS) - 1)
#define RD_PT_POOL_MAX    16384             /* pages: 64 MiB of tables */

typedef struct rdna3_pt_pool {
    uint64_t base;                          /* VRAM offset of page 0 */
    uint32_t npages, nfree, hint;
    uint32_t bitmap[RD_PT_POOL_MAX / 32];   /* 1 = in use */
} rdna3_pt_pool;

typedef struct rdna3_vm {
    rdna3_pt_pool *pool;
    uint64_t root;                          /* VRAM offset of the PDB2 */
    uint32_t vmid;                          /* 0 = not bound */
    uint32_t tables;                        /* tables in use, root included */
} rdna3_vm;

/* page address lookup for gpuvm_map: physical/bus address of page 'i' */
typedef uint64_t (*rdna3_page_pa_fn)(void *ctx, uint64_t page_index);

int  gpuvm_pool_init(rdna3_dev *d, rdna3_pt_pool *p, uint32_t npages);
/* VRAM lost: all pages free again (the VMs' old tables are gone) */
void gpuvm_pool_reset(rdna3_pt_pool *p);
int  gpuvm_vm_init(rdna3_dev *d, rdna3_vm *vm, rdna3_pt_pool *p);
void gpuvm_vm_fini(rdna3_dev *d, rdna3_vm *vm);
/* map 'pages' 4 KiB pages at 'va'; page i lives at pa_fn(ctx, i) */
int  gpuvm_map(rdna3_dev *d, rdna3_vm *vm, uint64_t va, uint64_t pages,
               rdna3_page_pa_fn pa_fn, void *ctx, uint64_t pte_flags);
/* map a physically contiguous range (VRAM BOs) */
int  gpuvm_map_linear(rdna3_dev *d, rdna3_vm *vm, uint64_t va, uint64_t pa, uint64_t pages,
                      uint64_t pte_flags);
void gpuvm_unmap(rdna3_dev *d, rdna3_vm *vm, uint64_t va, uint64_t pages);
/* walk the tables like the hardware would; 0 if not mapped */
uint64_t gpuvm_lookup(rdna3_dev *d, rdna3_vm *vm, uint64_t va);
/* amdgpu_gmc_pd_addr(root) */
uint64_t gpuvm_pd_addr(rdna3_dev *d, rdna3_vm *vm);

/* gmc_v11_0_get_vm_pte flags for an AMDGPU_VM_PAGE_* request */
uint64_t gpuvm_pte_flags(uint32_t amdgpu_va_flags, int system_memory);

#ifdef __cplusplus
}
#endif
#endif
