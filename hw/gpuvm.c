/*
 * gpuvm.c - see gpuvm.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gpuvm.h"
#include "gmc11.h"

#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ull

/* va bits consumed by each level, root first (amdgpu_vm_pt_level_shift) */
static const uint32_t level_shift[RD_VM_LEVELS] = { 39, 30, 21, 12 };

static uint32_t level_index(uint64_t va, int level)
{
    return (uint32_t)((va >> level_shift[level]) & (RD_VM_ENTRIES - 1));
}

/* ----------------------------- pool ----------------------------- */

int gpuvm_pool_init(rdna3_dev *d, rdna3_pt_pool *p, uint32_t npages)
{
    uint32_t i;
    if (!npages || npages > RD_PT_POOL_MAX)
        return RDNA3_EINVAL;
    p->base = rdna3_vram_alloc(d, (uint64_t)npages * 4096, 4096, "vm-pt-pool");
    if (p->base == UINT64_MAX)
        return RDNA3_ENOMEM;
    p->npages = p->nfree = npages;
    p->hint = 0;
    for (i = 0; i < RD_PT_POOL_MAX / 32; i++)
        p->bitmap[i] = 0;
    return RDNA3_OK;
}

static uint64_t pool_get(rdna3_dev *d, rdna3_pt_pool *p)
{
    uint32_t n, i;
    for (n = 0; n < p->npages; n++) {
        i = (p->hint + n) % p->npages;
        if (!(p->bitmap[i / 32] & (1u << (i % 32)))) {
            uint64_t off = p->base + (uint64_t)i * 4096;
            p->bitmap[i / 32] |= 1u << (i % 32);
            p->nfree--;
            p->hint = i + 1;
            rdna3_vram_fill(d, off, 0, 4096);
            return off;
        }
    }
    return UINT64_MAX;
}

/* amdgpu_vm_pt_free: one page-table page back to the pool */
static void pool_put(rdna3_pt_pool *p, uint64_t off)
{
    uint32_t i;
    if (off < p->base)
        return;
    i = (uint32_t)((off - p->base) / 4096);
    if (i >= p->npages || !(p->bitmap[i / 32] & (1u << (i % 32))))
        return;
    p->bitmap[i / 32] &= ~(1u << (i % 32));
    p->nfree++;
}

/* --------------------------- tables --------------------------- */

static uint64_t entry_read(rdna3_dev *d, uint64_t table, uint32_t idx)
{
    uint64_t e;
    rdna3_vram_read(d, table + 8ull * idx, &e, 8);
    return e;
}

static void entry_write(rdna3_dev *d, uint64_t table, uint32_t idx, uint64_t e)
{
    rdna3_vram_write(d, table + 8ull * idx, &e, 8);
}

/* VRAM offset of the table a PDE points to */
static uint64_t pde_table(rdna3_dev *d, uint64_t pde)
{
    return (pde & PTE_ADDR_MASK) - rdna3_vram_pa(d, 0);
}

int gpuvm_vm_init(rdna3_dev *d, rdna3_vm *vm, rdna3_pt_pool *p)
{
    vm->pool = p;
    vm->vmid = 0;
    vm->root = pool_get(d, p);
    if (vm->root == UINT64_MAX)
        return RDNA3_ENOMEM;
    vm->tables = 1;
    rdna3_hdp_flush(d);
    return RDNA3_OK;
}

uint64_t gpuvm_pd_addr(rdna3_dev *d, rdna3_vm *vm)
{
    return rdna3_vram_pa(d, vm->root) | RD_PTE_VALID;
}

/* returns the PTB covering 'va', creating directories when 'create' */
static uint64_t walk_to_ptb(rdna3_dev *d, rdna3_vm *vm, uint64_t va, int create)
{
    uint64_t table = vm->root;
    int level;
    for (level = 0; level < RD_VM_LEVELS - 1; level++) {
        uint32_t idx = level_index(va, level);
        uint64_t pde = entry_read(d, table, idx);
        if (!(pde & RD_PTE_VALID)) {
            uint64_t next;
            if (!create)
                return UINT64_MAX;
            next = pool_get(d, vm->pool);
            if (next == UINT64_MAX)
                return UINT64_MAX;
            vm->tables++;
            /* amdgpu_vm_pde_update -> amdgpu_gmc_get_pde_for_bo -> gmc_v11_0_get_vm_pde
             * (no translate_further: the table's physical address | VALID) */
            pde = rdna3_vram_pa(d, next) | RD_PTE_VALID;
            entry_write(d, table, idx, pde);
        }
        table = pde_table(d, pde);
    }
    return table;
}

int gpuvm_map(rdna3_dev *d, rdna3_vm *vm, uint64_t va, uint64_t pages,
              rdna3_page_pa_fn pa_fn, void *ctx, uint64_t pte_flags)
{
    uint64_t i, ptb = UINT64_MAX, cur_block = UINT64_MAX;
    va &= RD_VM_VA_MASK;
    if ((va & 4095) || !pages || va + pages * 4096 > (1ull << RD_VM_VA_BITS))
        return RDNA3_EINVAL;
    for (i = 0; i < pages; i++) {
        uint64_t a = va + i * 4096, pa = pa_fn(ctx, i);
        if (a >> 21 != cur_block) {
            ptb = walk_to_ptb(d, vm, a, 1);
            if (ptb == UINT64_MAX) {
                rdna3_hdp_flush(d);
                RERR(d, "VM: out of page-table pages mapping 0x%llx", (unsigned long long)a);
                return RDNA3_ENOMEM;
            }
            cur_block = a >> 21;
        }
        entry_write(d, ptb, level_index(a, RD_VM_LEVELS - 1), (pa & PTE_ADDR_MASK) | pte_flags);
    }
    rdna3_hdp_flush(d);
    return RDNA3_OK;
}

struct linear_ctx { uint64_t pa; };
static uint64_t linear_pa(void *ctx, uint64_t i)
{
    return ((struct linear_ctx *)ctx)->pa + i * 4096;
}

int gpuvm_map_linear(rdna3_dev *d, rdna3_vm *vm, uint64_t va, uint64_t pa, uint64_t pages,
                     uint64_t pte_flags)
{
    struct linear_ctx c;
    c.pa = pa;
    return gpuvm_map(d, vm, va, pages, linear_pa, &c, pte_flags);
}

void gpuvm_unmap(rdna3_dev *d, rdna3_vm *vm, uint64_t va, uint64_t pages)
{
    uint64_t i, ptb = UINT64_MAX, cur_block = UINT64_MAX;
    va &= RD_VM_VA_MASK;
    for (i = 0; i < pages; i++) {
        uint64_t a = va + i * 4096;
        if (a >> 21 != cur_block) {
            ptb = walk_to_ptb(d, vm, a, 0);
            cur_block = a >> 21;
        }
        if (ptb != UINT64_MAX)
            entry_write(d, ptb, level_index(a, RD_VM_LEVELS - 1), 0);
    }
    rdna3_hdp_flush(d);
}

uint64_t gpuvm_lookup(rdna3_dev *d, rdna3_vm *vm, uint64_t va)
{
    uint64_t ptb, pte;
    va &= RD_VM_VA_MASK;
    ptb = walk_to_ptb(d, vm, va, 0);
    if (ptb == UINT64_MAX)
        return 0;
    pte = entry_read(d, ptb, level_index(va, RD_VM_LEVELS - 1));
    if (!(pte & RD_PTE_VALID))
        return 0;
    return (pte & PTE_ADDR_MASK) | (va & 4095);
}

static void free_level(rdna3_dev *d, rdna3_vm *vm, uint64_t table, int level)
{
    uint32_t i;
    if (level < RD_VM_LEVELS - 1) {
        for (i = 0; i < RD_VM_ENTRIES; i++) {
            uint64_t pde = entry_read(d, table, i);
            if (pde & RD_PTE_VALID)
                free_level(d, vm, pde_table(d, pde), level + 1);
        }
    }
    pool_put(vm->pool, table);
    vm->tables--;
}

/* amdgpu_vm_pt_free_root: every table, depth first */
void gpuvm_vm_fini(rdna3_dev *d, rdna3_vm *vm)
{
    if (vm->root == UINT64_MAX || !vm->pool)
        return;
    free_level(d, vm, vm->root, 0);
    vm->root = UINT64_MAX;
}

/* amdgpu_gem_va_map_flags + gmc_v11_0_get_vm_pte (MTYPE_NC default) */
uint64_t gpuvm_pte_flags(uint32_t f, int system_memory)
{
    uint64_t pte = RD_PTE_VALID;
    uint32_t mtype = 0;   /* MTYPE_NC */
    if (f & (1u << 1)) pte |= RD_PTE_READABLE;     /* AMDGPU_VM_PAGE_READABLE */
    if (f & (1u << 2)) pte |= RD_PTE_WRITEABLE;    /* AMDGPU_VM_PAGE_WRITEABLE */
    if (f & (1u << 3)) pte |= RD_PTE_EXECUTABLE;   /* AMDGPU_VM_PAGE_EXECUTABLE */
    /* gmc_v11_0_map_mtype */
    switch ((f >> 5) & 0xf) {                      /* AMDGPU_VM_MTYPE_* */
    case 2: mtype = 1; break;                      /* WC */
    case 3: mtype = 2; break;                      /* CC */
    case 4: mtype = RD_MTYPE_UC; break;            /* UC */
    default: mtype = 0; break;                     /* DEFAULT, NC, RW */
    }
    pte |= RD_PTE_MTYPE_NV10(mtype);
    if (system_memory)
        pte |= RD_PTE_SYSTEM | RD_PTE_SNOOPED;
    return pte;
}
