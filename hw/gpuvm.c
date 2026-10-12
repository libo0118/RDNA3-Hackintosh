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

/* VRAM was lost: every table in the pool is garbage and no VM keeps its
 * pages; the pool starts empty over the same VRAM range */
void gpuvm_pool_reset(rdna3_pt_pool *p)
{
    uint32_t i;
    p->nfree = p->npages;
    p->hint = 0;
    for (i = 0; i < RD_PT_POOL_MAX / 32; i++)
        p->bitmap[i] = 0;
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

/* ======================================================================
 * rdna3amdgpu=1: amdgpu_vm_pt.c / amdgpu_vm.c as amdgpu runs them on
 * GFX 11. New tables are cleared like amdgpu_vm_pt_clear (PD entries
 * AMDGPU_PDE_PTE, PTB entries AMDGPU_PTE_EXECUTABLE), updates go through
 * amdgpu_vm_ptes_update with the fragment field (AMDGPU_PTE_FRAG) and huge
 * PDEs (a PDE with AMDGPU_PDE_PTE maps 2 MiB / 1 GiB directly, the tables
 * below it are freed), unmapping writes the fault-priority workaround
 * (EXECUTABLE). The cursor walks our tables: an entry "has a bo" when its
 * PDE is VALID and points to a table (no PDE_PTE).
 * ====================================================================== */

#define VM_PTB        (RD_VM_LEVELS - 1)            /* AMDGPU_VM_PTB */
#define VM_BLOCK_SIZE 9                             /* vm_manager.block_size */
#define VM_MAX_PFN    (1ull << (RD_VM_VA_BITS - 12))
#define NO_TABLE      UINT64_MAX

typedef struct vm_cursor {
    uint64_t pfn;
    uint32_t level;
    uint64_t table[RD_VM_LEVELS];   /* the bo of the entry at each level on the path */
} vm_cursor;

typedef struct vm_update_params {
    rdna3_dev *d;
    rdna3_vm *vm;
    rdna3_page_pa_fn pages_addr;    /* NULL: dst is an address, else a byte offset */
    void *ctx;
} vm_update_params;

/* Ported from amdgpu_vm_pt_level_shift (pfn bits a level consumes) */
static unsigned int amdgpu_vm_pt_level_shift(unsigned int level)
{
    if (level < VM_PTB)
        return 9 * (VM_PTB - 1 - level) + VM_BLOCK_SIZE;
    return 0;
}

/* Ported from amdgpu_vm_pt_num_entries */
static unsigned int amdgpu_vm_pt_num_entries(unsigned int level)
{
    unsigned int shift = amdgpu_vm_pt_level_shift(0);
    if (level == 0)   /* the root directory */
        return (unsigned int)(((VM_MAX_PFN + (1ull << shift) - 1) & ~((1ull << shift) - 1)) >> shift);
    if (level != VM_PTB)
        return 512;
    return 1u << VM_BLOCK_SIZE;   /* AMDGPU_VM_PTE_COUNT */
}

/* Ported from amdgpu_vm_pt_entries_mask */
static uint32_t amdgpu_vm_pt_entries_mask(unsigned int level)
{
    if (level == 0)
        return 0xffffffff;
    if (level != VM_PTB)
        return 0x1ff;
    return (1u << VM_BLOCK_SIZE) - 1;
}

/* Ported from amdgpu_vm_pt_size: every table is one 4 KiB pool page */
static unsigned int amdgpu_vm_pt_size(unsigned int level)
{
    return (amdgpu_vm_pt_num_entries(level) * 8 + 4095) & ~4095u;
}

/* the child table an entry of 'table' points to (entry->bo) */
static uint64_t pde_child(rdna3_dev *d, uint64_t table, uint32_t idx)
{
    uint64_t e;
    if (table == NO_TABLE)
        return NO_TABLE;
    e = entry_read(d, table, idx);
    if (!(e & RD_PTE_VALID) || (e & RD_PDE_PTE))
        return NO_TABLE;
    return pde_table(d, e);
}

/* Ported from amdgpu_vm_pt_start */
static void amdgpu_vm_pt_start(rdna3_vm *vm, uint64_t start, vm_cursor *c)
{
    unsigned int i;
    c->pfn = start;
    c->level = 0;
    c->table[0] = vm->root;
    for (i = 1; i < RD_VM_LEVELS; i++)
        c->table[i] = NO_TABLE;
}

/* Ported from amdgpu_vm_pt_descendant */
static int amdgpu_vm_pt_descendant(rdna3_dev *d, vm_cursor *c)
{
    uint32_t mask, shift, idx;
    if (c->level == VM_PTB || c->table[c->level] == NO_TABLE)
        return 0;
    mask = amdgpu_vm_pt_entries_mask(c->level);
    shift = amdgpu_vm_pt_level_shift(c->level);
    idx = (uint32_t)((c->pfn >> shift) & mask);
    c->level++;
    c->table[c->level] = pde_child(d, c->table[c->level - 1], idx);
    return 1;
}

/* Ported from amdgpu_vm_pt_parent: the table one level up the path */
static uint64_t amdgpu_vm_pt_parent(const vm_cursor *c)
{
    return c->level ? c->table[c->level - 1] : NO_TABLE;
}

/* Ported from amdgpu_vm_pt_sibling */
static int amdgpu_vm_pt_sibling(rdna3_dev *d, vm_cursor *c)
{
    uint32_t shift, num_entries, idx;
    if (!c->level)   /* Root doesn't have a sibling */
        return 0;
    shift = amdgpu_vm_pt_level_shift(c->level - 1);
    num_entries = amdgpu_vm_pt_num_entries(c->level - 1);
    idx = (uint32_t)((c->pfn >> shift) & amdgpu_vm_pt_entries_mask(c->level - 1));
    if (idx == num_entries - 1)
        return 0;
    c->pfn += 1ull << shift;
    c->pfn &= ~((1ull << shift) - 1);
    c->table[c->level] = pde_child(d, amdgpu_vm_pt_parent(c), idx + 1);
    return 1;
}

/* Ported from amdgpu_vm_pt_ancestor */
static int amdgpu_vm_pt_ancestor(vm_cursor *c)
{
    if (!c->level)
        return 0;
    c->level--;
    return 1;
}

/* Ported from amdgpu_vm_pt_next */
static void amdgpu_vm_pt_next(rdna3_dev *d, vm_cursor *c)
{
    /* First try a newborn child */
    if (amdgpu_vm_pt_descendant(d, c))
        return;
    /* If that didn't worked try to find a sibling */
    while (!amdgpu_vm_pt_sibling(d, c)) {
        /* No sibling, go to our parents and grandparents */
        if (!amdgpu_vm_pt_ancestor(c)) {
            c->pfn = ~0ull;
            return;
        }
    }
}

/* the entry a cursor stands on: its level and the pfn its slot starts at */
typedef struct vm_entry {
    int      valid;
    uint32_t level;
    uint64_t slot, table;
} vm_entry;

static vm_entry cursor_entry(const vm_cursor *c)
{
    vm_entry e;
    e.valid = 1;
    e.level = c->level;
    e.slot = c->level ? c->pfn & ~((1ull << amdgpu_vm_pt_level_shift(c->level - 1)) - 1) : 0;
    e.table = c->table[c->level];
    return e;
}

/* Ported from amdgpu_vm_pt_first_dfs */
static void amdgpu_vm_pt_first_dfs(rdna3_dev *d, rdna3_vm *vm, const vm_cursor *start, vm_cursor *c)
{
    if (start)
        *c = *start;
    else
        amdgpu_vm_pt_start(vm, 0, c);
    while (amdgpu_vm_pt_descendant(d, c))
        ;
}

/* Ported from amdgpu_vm_pt_continue_dfs */
static int amdgpu_vm_pt_continue_dfs(const vm_cursor *start, const vm_entry *entry)
{
    vm_entry s;
    if (!entry->valid)
        return 0;
    if (!start)
        return 1;
    s = cursor_entry(start);
    return !(entry->level == s.level && entry->slot == s.slot);
}

/* Ported from amdgpu_vm_pt_next_dfs ('done' is the entry = NULL of the root) */
static void amdgpu_vm_pt_next_dfs(rdna3_dev *d, vm_cursor *c, int *done)
{
    if (*done)
        return;
    if (!c->level)
        *done = 1;
    else if (amdgpu_vm_pt_sibling(d, c))
        while (amdgpu_vm_pt_descendant(d, c))
            ;
    else
        amdgpu_vm_pt_ancestor(c);
}

/* for_each_amdgpu_vm_pt_dfs_safe: 'fn' on every entry below 'start' (the
 * whole tree for NULL), children first */
static void vm_pt_dfs(rdna3_dev *d, rdna3_vm *vm, const vm_cursor *start,
                      void (*fn)(rdna3_dev *, rdna3_vm *, const vm_entry *))
{
    vm_cursor c;
    vm_entry e;
    int done = 0;
    amdgpu_vm_pt_first_dfs(d, vm, start, &c);
    e = cursor_entry(&c);
    amdgpu_vm_pt_next_dfs(d, &c, &done);
    while (amdgpu_vm_pt_continue_dfs(start, &e)) {
        fn(d, vm, &e);
        if (done) {
            e.valid = 0;
        } else {
            e = cursor_entry(&c);
            amdgpu_vm_pt_next_dfs(d, &c, &done);
        }
    }
}

/* Ported from amdgpu_gmc_set_pte_pde (CPU update of one entry) */
static void amdgpu_gmc_set_pte_pde(rdna3_dev *d, uint64_t table, uint32_t gpu_page_idx, uint64_t addr,
                                   uint64_t flags)
{
    uint64_t value = addr & 0x0000FFFFFFFFF000ull;
    value |= flags;
    entry_write(d, table, gpu_page_idx, value);
}

static void amdgpu_vm_pt_add_list(rdna3_dev *d, rdna3_vm *vm, const vm_cursor *c);

/* The bo of an entry is its PDE here, so the tables below a PD entry about
 * to be overwritten (huge entry, or cleared on unmap) are freed now: amdgpu
 * frees the same ones right after the write (the descendant loop of
 * amdgpu_vm_ptes_update) */
static void free_below(vm_update_params *p, uint64_t table, uint32_t idx, unsigned int level,
                       uint64_t slot_pfn)
{
    vm_cursor c;
    unsigned int l;
    uint64_t old = entry_read(p->d, table, idx);
    if (!(old & RD_PTE_VALID) || (old & RD_PDE_PTE))
        return;
    for (l = 0; l < RD_VM_LEVELS; l++)
        c.table[l] = NO_TABLE;
    c.pfn = slot_pfn;
    c.level = level + 1;
    c.table[level] = table;
    c.table[level + 1] = pde_table(p->d, old);
    amdgpu_vm_pt_add_list(p->d, p->vm, &c);
}

/* amdgpu_vm_cpu_update: 'count' entries from byte offset 'pe'; 'pfn' is
 * where the first entry's range starts */
static void vm_cpu_update(vm_update_params *p, uint64_t table, unsigned int level, uint64_t pfn,
                          uint64_t pe, uint64_t addr, unsigned int count, uint64_t incr,
                          uint64_t flags)
{
    unsigned int i;
    for (i = 0; i < count; i++) {
        uint64_t value = addr;
        if (level != VM_PTB)
            free_below(p, table, (uint32_t)(pe / 8) + i, level,
                       pfn + ((uint64_t)i << amdgpu_vm_pt_level_shift(level)));
        if (p->pages_addr)   /* amdgpu_vm_map_gart */
            value = (p->pages_addr(p->ctx, addr >> 12) | (addr & 4095)) & 0xFFFFFFFFFFFFF000ull;
        amdgpu_gmc_set_pte_pde(p->d, table, (uint32_t)(pe / 8) + i, value, flags);
        addr += incr;
    }
}

/* Ported from amdgpu_vm_pt_clear: a new table's entries before use */
static void amdgpu_vm_pt_clear(rdna3_dev *d, uint64_t table, unsigned int level)
{
    uint64_t chunk[64], value = 0, flags;
    unsigned int i, entries = amdgpu_vm_pt_size(level) / 8;
    if (level != VM_PTB) {
        /* Handle leaf PDEs as PTEs (AMDGPU_PDE_PTE_FLAG; gmc_v11_0_get_vm_pde
         * leaves the address of a PDE_PTE entry alone) */
        flags = RD_PDE_PTE;
    } else {
        /* Workaround for fault priority problem on GMC9 */
        flags = RD_PTE_EXECUTABLE;
    }
    for (i = 0; i < 64; i++)
        chunk[i] = (value & 0x0000FFFFFFFFF000ull) | flags;
    for (i = 0; i < entries; i += 64)
        rdna3_vram_write(d, table + 8ull * i, chunk, sizeof(chunk));
}

/* Ported from amdgpu_vm_pt_create: one table from the pool */
static uint64_t amdgpu_vm_pt_create(rdna3_dev *d, rdna3_vm *vm, unsigned int level)
{
    uint64_t t = pool_get(d, vm->pool);
    (void)level;   /* amdgpu_vm_pt_size(level) is one pool page */
    if (t != NO_TABLE)
        vm->tables++;
    return t;
}

/* Ported from amdgpu_vm_pt_alloc (with amdgpu_vm_pde_update for the new
 * table's PDE: gmc_v11_0_get_vm_pde, physical address | VALID) */
static int amdgpu_vm_pt_alloc(rdna3_dev *d, rdna3_vm *vm, vm_cursor *c)
{
    uint64_t pt;
    uint32_t idx;
    if (c->table[c->level] != NO_TABLE)
        return 0;
    pt = amdgpu_vm_pt_create(d, vm, c->level);
    if (pt == NO_TABLE)
        return RDNA3_ENOMEM;
    amdgpu_vm_pt_clear(d, pt, c->level);
    idx = (uint32_t)((c->pfn >> amdgpu_vm_pt_level_shift(c->level - 1)) &
                     amdgpu_vm_pt_entries_mask(c->level - 1));
    entry_write(d, amdgpu_vm_pt_parent(c), idx, rdna3_vram_pa(d, pt) | RD_PTE_VALID);
    c->table[c->level] = pt;
    return 1;
}

/* Ported from amdgpu_vm_pt_free */
static void amdgpu_vm_pt_free(rdna3_dev *d, rdna3_vm *vm, const vm_entry *entry)
{
    (void)d;
    if (entry->table == NO_TABLE)
        return;
    pool_put(vm->pool, entry->table);
    vm->tables--;
}

/* Ported from amdgpu_vm_pt_add_list + amdgpu_vm_pt_free_list (a locked CPU
 * update frees the tables as soon as the update is done; nothing points
 * at them any more, the huge PDE or the cleared entry replaced them) */
static void amdgpu_vm_pt_add_list(rdna3_dev *d, rdna3_vm *vm, const vm_cursor *c)
{
    vm_entry start = cursor_entry(c);
    vm_pt_dfs(d, vm, c, amdgpu_vm_pt_free);
    amdgpu_vm_pt_free(d, vm, &start);   /* enter start node now */
}

/* Ported from amdgpu_vm_pte_update_noretry_flags (gmc_v11_0:
 * noretry_flags = AMDGPU_VM_NORETRY_FLAGS_TF) */
static void amdgpu_vm_pte_update_noretry_flags(uint64_t *flags)
{
    if ((*flags & RD_VM_NORETRY_FLAGS) == RD_VM_NORETRY_FLAGS) {
        *flags &= ~RD_VM_NORETRY_FLAGS;
        *flags |= RD_VM_NORETRY_FLAGS_TF;
    }
}

/* Ported from amdgpu_vm_pte_update_flags */
static void amdgpu_vm_pte_update_flags(vm_update_params *p, uint64_t pt, unsigned int level,
                                       uint64_t pfn, uint64_t pe, uint64_t addr, unsigned int count,
                                       uint64_t incr, uint64_t flags)
{
    if (level != VM_PTB) {
        flags |= RD_PDE_PTE;   /* AMDGPU_PDE_PTE_FLAG; get_vm_pde: no translate_further */
    } else if (!(flags & RD_PTE_VALID) && !(flags & RD_PTE_PRT)) {
        /* Workaround for fault priority problem on GMC9 */
        flags |= RD_PTE_EXECUTABLE;
    }
    if (level == VM_PTB)
        amdgpu_vm_pte_update_noretry_flags(&flags);
    vm_cpu_update(p, pt, level, pfn, pe, addr, count, incr, flags);
}

static unsigned int ffs64(uint64_t v)
{
    unsigned int i;
    for (i = 0; i < 64; i++)
        if (v & (1ull << i))
            return i + 1;
    return 0;
}

static unsigned int fls64(uint64_t v)
{
    unsigned int n = 0;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

/* Ported from amdgpu_vm_pte_fragment */
static void amdgpu_vm_pte_fragment(const vm_update_params *p, uint64_t start, uint64_t end,
                                   unsigned int *frag, uint64_t *frag_end)
{
    const unsigned int max_frag = 31;
    unsigned int a, b;
    /* system pages are non continuously */
    if (p->pages_addr) {
        *frag = 0;
        *frag_end = end;
        return;
    }
    /* This intentionally wraps around if no bit is set */
    a = ffs64(start) - 1;
    b = fls64(end - start) - 1;
    *frag = a < b ? a : b;
    if (*frag >= max_frag) {
        *frag = max_frag;
        *frag_end = end & ~((1ull << max_frag) - 1);
    } else {
        *frag_end = start + (1ull << *frag);
    }
}

/* Ported from amdgpu_vm_ptes_update (locked, GMC v9+: huge pages) */
static int amdgpu_vm_ptes_update(vm_update_params *p, uint64_t start, uint64_t end, uint64_t dst,
                                 uint64_t flags)
{
    rdna3_dev *d = p->d;
    vm_cursor cursor;
    uint64_t frag_start = start, frag_end;
    unsigned int frag;
    int r;

    /* figure out the initial fragment */
    amdgpu_vm_pte_fragment(p, frag_start, end, &frag, &frag_end);

    /* walk over the address space and update the PTs */
    amdgpu_vm_pt_start(p->vm, start, &cursor);
    while (cursor.pfn < end) {
        unsigned int shift, parent_shift;
        uint64_t incr, entry_end, pe_start, mask, pt;

        /* make sure that the page tables covering the address range are
         * actually allocated */
        r = amdgpu_vm_pt_alloc(d, p->vm, &cursor);
        if (r < 0)
            return r;

        shift = amdgpu_vm_pt_level_shift(cursor.level);
        parent_shift = cursor.level ? amdgpu_vm_pt_level_shift(cursor.level - 1) : ~0u;
        if (frag < shift) {
            /* We can't use this level when the fragment size is smaller
             * than the address shift. Go to the next child entry and try
             * again. */
            if (amdgpu_vm_pt_descendant(d, &cursor))
                continue;
        } else if (frag >= parent_shift) {
            /* If the fragment size is even larger than the parent shift
             * we should go up one level and check it again. */
            if (!amdgpu_vm_pt_ancestor(&cursor))
                return RDNA3_EINVAL;
            continue;
        }

        pt = cursor.table[cursor.level];
        if (pt == NO_TABLE) {
            /* We need all PDs and PTs for mapping something, */
            if (flags & RD_PTE_VALID)
                return RDNA3_ENOENT;
            /* but unmapping something can happen at a higher level. */
            if (!amdgpu_vm_pt_ancestor(&cursor))
                return RDNA3_EINVAL;
            pt = cursor.table[cursor.level];
            shift = parent_shift;
            {
                uint64_t al = (frag_start + 1 + (1ull << shift) - 1) & ~((1ull << shift) - 1);
                frag_end = frag_end > al ? frag_end : al;
            }
        }

        /* Looks good so far, calculate parameters for the update */
        incr = 4096ull << shift;
        mask = amdgpu_vm_pt_entries_mask(cursor.level);
        pe_start = ((cursor.pfn >> shift) & mask) * 8;
        entry_end = (mask + 1) << shift;
        entry_end += cursor.pfn & ~(entry_end - 1);
        entry_end = entry_end < end ? entry_end : end;

        do {
            uint64_t upd_end = entry_end < frag_end ? entry_end : frag_end;
            unsigned int nptes = (unsigned int)((upd_end - frag_start) >> shift);
            uint64_t upd_flags = flags | RD_PTE_FRAG(frag);

            /* This can happen when we set higher level PDs to silent to
             * stop fault floods. */
            if (!nptes)
                nptes = 1;
            amdgpu_vm_pte_update_flags(p, pt, cursor.level, frag_start & ~((1ull << shift) - 1),
                                       pe_start, dst, nptes, incr, upd_flags);

            pe_start += nptes * 8ull;
            dst += nptes * incr;

            frag_start = upd_end;
            if (frag_start >= frag_end) {
                /* figure out the next fragment */
                amdgpu_vm_pte_fragment(p, frag_start, end, &frag, &frag_end);
                if (frag < shift)
                    break;
            }
        } while (frag_start < entry_end);

        if (amdgpu_vm_pt_descendant(d, &cursor)) {
            /* Free all child entries: the tables below a huge entry or a
             * freed area that the range covers completely. */
            while (cursor.pfn < frag_start) {
                /* Make sure previous mapping is freed */
                if (cursor.table[cursor.level] != NO_TABLE)
                    amdgpu_vm_pt_add_list(d, p->vm, &cursor);
                amdgpu_vm_pt_next(d, &cursor);
            }
        } else if (frag >= shift) {
            /* or just move on to the next on the same level. */
            amdgpu_vm_pt_next(d, &cursor);
        }
    }
    return RDNA3_OK;
}

/* Ported from amdgpu_vm_update_range for one mapping: VRAM in one piece,
 * system pages split into contiguous runs (mapped like VRAM, with
 * fragments) and scattered runs (4 KiB PTEs through pages_addr) */
static int vm_update_range(rdna3_dev *d, rdna3_vm *vm, uint64_t va, uint64_t pages,
                           rdna3_page_pa_fn pa_fn, void *ctx, int linear, uint64_t flags)
{
    vm_update_params p;
    uint64_t start = va >> 12, done = 0;
    int r;
    p.d = d;
    p.vm = vm;
    p.ctx = ctx;
    while (done < pages) {
        uint64_t num_entries = pages - done, addr;
        int contiguous = 1;
        if (!(flags & RD_PTE_VALID)) {
            addr = 0;
            p.pages_addr = NULL;
        } else if (linear) {
            addr = pa_fn(ctx, done);
            p.pages_addr = NULL;
        } else {
            if (num_entries > 1) {
                uint64_t count;
                contiguous = pa_fn(ctx, done + 1) == pa_fn(ctx, done) + 4096;
                for (count = 2; count < num_entries; ++count) {
                    uint64_t idx = done + count;
                    if (contiguous != (pa_fn(ctx, idx) == pa_fn(ctx, idx - 1) + 4096))
                        break;
                }
                if (!contiguous)
                    count--;
                num_entries = count;
            }
            if (!contiguous) {
                /* byte offset into the mapping, resolved per page */
                addr = done * 4096;
                p.pages_addr = pa_fn;
                p.ctx = ctx;
            } else {
                addr = pa_fn(ctx, done);
                p.pages_addr = NULL;
            }
        }
        r = amdgpu_vm_ptes_update(&p, start, start + num_entries, addr, flags);
        if (r)
            return r;
        start += num_entries;
        done += num_entries;
    }
    return RDNA3_OK;
}

/* amdgpu_vm_pt_free_root: every table, depth first */
static void vm_free_root_exact(rdna3_dev *d, rdna3_vm *vm)
{
    vm_pt_dfs(d, vm, NULL, amdgpu_vm_pt_free);
}

/* the hardware walk with huge PDEs (AMDGPU_PDE_PTE in a PDB0 / PDB1 entry) */
static uint64_t vm_lookup_exact(rdna3_dev *d, rdna3_vm *vm, uint64_t va)
{
    uint64_t table = vm->root, e;
    unsigned int level;
    for (level = 0; level < VM_PTB; level++) {
        uint32_t bits = 12 + amdgpu_vm_pt_level_shift(level);
        e = entry_read(d, table, (uint32_t)((va >> bits) & amdgpu_vm_pt_entries_mask(level)));
        if (!(e & RD_PTE_VALID))
            return 0;
        if (e & RD_PDE_PTE)
            return (e & PTE_ADDR_MASK & ~((1ull << bits) - 1)) | (va & ((1ull << bits) - 1));
        table = pde_table(d, e);
    }
    e = entry_read(d, table, (uint32_t)((va >> 12) & amdgpu_vm_pt_entries_mask(VM_PTB)));
    if (!(e & RD_PTE_VALID))
        return 0;
    return (e & PTE_ADDR_MASK) | (va & 4095);
}

int gpuvm_vm_init(rdna3_dev *d, rdna3_vm *vm, rdna3_pt_pool *p)
{
    vm->pool = p;
    vm->vmid = 0;
    vm->root = pool_get(d, p);
    if (vm->root == UINT64_MAX)
        return RDNA3_ENOMEM;
    vm->tables = 1;
    if (d->amdgpu_exact)
        amdgpu_vm_pt_clear(d, vm->root, 0);
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

static uint64_t linear_pa(void *ctx, uint64_t i);

int gpuvm_map(rdna3_dev *d, rdna3_vm *vm, uint64_t va, uint64_t pages,
              rdna3_page_pa_fn pa_fn, void *ctx, uint64_t pte_flags)
{
    uint64_t i, ptb = UINT64_MAX, cur_block = UINT64_MAX;
    va &= RD_VM_VA_MASK;
    if ((va & 4095) || !pages || va + pages * 4096 > (1ull << RD_VM_VA_BITS))
        return RDNA3_EINVAL;
    if (d->amdgpu_exact) {
        int r = vm_update_range(d, vm, va, pages, pa_fn, ctx, pa_fn == linear_pa, pte_flags);
        rdna3_hdp_flush(d);
        if (r)
            RERR(d, "VM: mapping 0x%llx failed (%d)", (unsigned long long)va, r);
        return r;
    }
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
    if (d->amdgpu_exact) {
        /* amdgpu_vm_clear_freed: amdgpu_vm_update_range with flags 0 */
        if (pages && !(va & 4095) && va + pages * 4096 <= (1ull << RD_VM_VA_BITS))
            vm_update_range(d, vm, va, pages, NULL, NULL, 0, 0);
        rdna3_hdp_flush(d);
        return;
    }
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
    if (d->amdgpu_exact)
        return vm_lookup_exact(d, vm, va);
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
    if (d->amdgpu_exact)
        vm_free_root_exact(d, vm);
    else
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
