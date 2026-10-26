/*
 * rdna3_hw.h - OS-independent hardware layer for Navi 3x bring-up.
 *
 * This is the equivalent of the slice of amdgpu's struct amdgpu_device
 * that early bring-up needs: register bases from IP discovery, MMIO and
 * SMN access, VRAM access through the BAR, HDP flush and a tiny VRAM
 * allocator. All OS services come in through rdna3_os so the same code
 * runs in the kext (kext/src/RDNA3Hw.cpp) and against the register
 * simulator in tests/ (tests/sim_navi32.c).
 *
 * Register offsets are in DWORDs like amdgpu (SOC15_REG_OFFSET).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_HW_H
#define RDNA3_HW_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "amd_discovery.h"
#include "rdna3_coh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes (negative errno style, independent of host errno) ---- */
enum {
    RDNA3_OK        = 0,
    RDNA3_EINVAL    = -22,
    RDNA3_ENOMEM    = -12,
    RDNA3_ENODEV    = -19,
    RDNA3_ETIME     = -62,
    RDNA3_EIO       = -5,
    RDNA3_ENOENT    = -2,
    RDNA3_EBUSY     = -16,
    RDNA3_EPERM     = -1,
    RDNA3_EOPNOTSUPP = -95,
    RDNA3_ENOSPC    = -28,
};

/* ---- hardware IP blocks we address registers of (subset of amd_hw_ip_block_type) ---- */
enum rdna3_hwip {
    HWIP_GC = 0, HWIP_HDP, HWIP_SDMA0, HWIP_SDMA1, HWIP_MMHUB, HWIP_ATHUB,
    HWIP_NBIO, HWIP_MP0, HWIP_MP1, HWIP_OSSSYS, HWIP_SMUIO, HWIP_DCE, HWIP_VCN,
    HWIP_THM, HWIP_CLK, HWIP_UMC,
    HWIP_MAX
};
#define RDNA3_MAX_INST 8
#define RDNA3_MAX_SEG  AMD_DISC_MAX_BASES

/* ---- OS services ---- */
typedef struct rdna3_os {
    void *ctx;
    void (*udelay)(void *ctx, uint32_t us);
    void (*msleep)(void *ctx, uint32_t ms);
    void (*vlog)(void *ctx, int level, const char *fmt, va_list ap);
    /* full memory barrier so CPU writes through the BAR are posted */
    void (*wmb)(void *ctx);
    /* MMIO accessors (byte offset into BAR5). The kext does plain volatile
     * accesses; the simulator models the registers. */
    uint32_t (*mmio_rd)(void *ctx, uint64_t byte_off);
    void     (*mmio_wr)(void *ctx, uint64_t byte_off, uint32_t val);
    /* optional: told about every CPU write into VRAM (tests track HDP flushes) */
    void     (*vram_written)(void *ctx, uint64_t off, uint64_t len);
    /* optional: 64-bit doorbell write (dword index into the doorbell BAR);
     * when NULL, rdna3_doorbell_wr64 writes through d->doorbell_cpu */
    void     (*doorbell_wr)(void *ctx, uint32_t index, uint64_t val);
    /* optional: 32-bit doorbell write (VCN / JPEG rings); when NULL,
     * rdna3_doorbell_wr32 writes through d->doorbell_cpu */
    void     (*doorbell_wr32)(void *ctx, uint32_t index, uint32_t val);
} rdna3_os;

enum { RDNA3_LOG_ERR = 0, RDNA3_LOG_INFO = 1, RDNA3_LOG_DBG = 2 };

typedef struct rdna3_ipver {
    uint8_t major, minor, rev;
    uint8_t present;
} rdna3_ipver;

/* VRAM allocator: first-fit over a list of free ranges, no holes reuse
 * needed for bring-up (allocations live as long as the device). */
#define RDNA3_VRAM_MAX_ALLOCS 128
typedef struct rdna3_vram_alloc {
    uint64_t offset, size;   /* offset from start of VRAM */
    const char *tag;
} rdna3_vram_block;

typedef struct rdna3_dev {
    rdna3_os os;

    uint64_t mmio_len;          /* BAR5 size in bytes */

    /* VRAM */
    uint64_t vram_size;         /* from RCC_CONFIG_MEMSIZE */
    volatile uint8_t *vram_cpu; /* BAR0 mapping (CPU-visible part), may be NULL */
    uint64_t vram_cpu_len;
    uint64_t fb_start;          /* MC address of VRAM offset 0 (MMMC_VM_FB_LOCATION_BASE) */
    uint64_t fb_end;
    uint64_t fb_offset;         /* GCMC_VM_FB_OFFSET: physical base for *_pa addresses */

    /* bump allocator window inside the CPU-visible part of VRAM */
    uint64_t alloc_lo, alloc_hi, alloc_next;
    /* top-down window for what the CPU never touches (amdgpu's NO_CPU_ACCESS
     * kernel BOs get TTM_PL_FLAG_TOPDOWN), e.g. the PSP TMR; 0 = none */
    uint64_t alloc_top_lo, alloc_top_hi, alloc_top_next;
    uint32_t nallocs;
    rdna3_vram_block allocs[RDNA3_VRAM_MAX_ALLOCS];

    /* register bases from discovery: reg_base[ip][inst][seg] (DWORDs) */
    uint32_t reg_base[HWIP_MAX][RDNA3_MAX_INST][RDNA3_MAX_SEG];
    rdna3_ipver ipver[HWIP_MAX];

    /* HDP remap hole (nbio_v4_3_set_reg_remap) */
    uint32_t rmmio_remap_offset; /* bytes */

    /* doorbell BAR (BAR2): CPU mapping, bus address, size; doorbell_ok is
     * set by nbio43_doorbell_init() once the aperture is enabled */
    volatile uint8_t *doorbell_cpu;
    uint64_t doorbell_bus, doorbell_len;
    int      doorbell_ok;

    uint32_t usec_timeout;      /* amdgpu default: 100000 */

    /* boot-arg rdna3amdgpu=1: follow amdgpu's own sequence where the
     * profile verified on the card does something else (the PMFW after
     * SETUP_TMR, the IH ring in system memory, page-table fragments, ...).
     * 0 (default) keeps the verified behaviour; set by the OS layer right
     * after rdna3_dev_init. */
    int      amdgpu_exact;
    /* boot-arg rdna3vmfrag=0: the default GPUVM path (amdgpu_exact 0)
     * writes VRAM mappings as 4 KiB PTEs without a fragment, as before
     * DRIVER-PLAN N57's fix; 0 (default) = amdgpu_vm_pte_fragment
     * fragments in the PTEs (hw/gpuvm.c ptb_fragment) */
    int      vm_frag_off;
    /* with amdgpu_exact: a display core drives the DCN
     * (amdgpu_device_has_dc_support; smu_v13_0_notify_display_change) */
    int      amdgpu_dc;
    /* DRIVER-PLAN N54: the coherence switches (RD3_COH_*, common/rdna3_coh.h)
     * the ring emitters read; the OS layer copies them in before each
     * submission (0: the behaviour before N54) */
    uint32_t coh;
    /* the work they did (RD3_COH_CNT_* / bit index; owned by the OS layer,
     * NULL = not counted) */
    uint64_t *coh_count;
} rdna3_dev;

/* ---- logging ---- */
void rdna3_log(rdna3_dev *d, int level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
#define RLOG(d, ...)  rdna3_log((d), RDNA3_LOG_INFO, __VA_ARGS__)
#define RERR(d, ...)  rdna3_log((d), RDNA3_LOG_ERR, __VA_ARGS__)
#define RDBG(d, ...)  rdna3_log((d), RDNA3_LOG_DBG, __VA_ARGS__)

/* ---- setup ---- */
void rdna3_dev_init(rdna3_dev *d, const rdna3_os *os, uint64_t mmio_len);
/* Fill reg_base/ipver from a decoded discovery table (amdgpu_discovery_reg_base_init). */
int  rdna3_dev_set_discovery(rdna3_dev *d, const amd_discovery *disc);

/* ---- register access (DWORD index) ---- */
uint32_t rdna3_rreg(rdna3_dev *d, uint32_t reg);
void     rdna3_wreg(rdna3_dev *d, uint32_t reg, uint32_t v);
uint32_t rdna3_reg_offset(const rdna3_dev *d, int ip, int inst, int seg, uint32_t reg);
#define SOC15_OFF(d, ip, inst, reg) \
    rdna3_reg_offset((d), HWIP_##ip, (inst), reg##_BASE_IDX, reg)
/* paste here, not through SOC15_OFF, so 'reg' is not macro-expanded first */
#define RREG32_SOC15(d, ip, inst, reg) \
    rdna3_rreg((d), rdna3_reg_offset((d), HWIP_##ip, (inst), reg##_BASE_IDX, reg))
#define WREG32_SOC15(d, ip, inst, reg, v) \
    rdna3_wreg((d), rdna3_reg_offset((d), HWIP_##ip, (inst), reg##_BASE_IDX, reg), (v))

/* SMN (System Management Network) indirect access through NBIO 4.3 RSMU */
uint32_t rdna3_smn_rreg(rdna3_dev *d, uint32_t addr);
void     rdna3_smn_wreg(rdna3_dev *d, uint32_t addr, uint32_t v);

/* Poll reg until (val & mask) == expect (or val != expect if changed). */
int rdna3_wait_reg(rdna3_dev *d, uint32_t reg, uint32_t expect, uint32_t mask,
                   int wait_changed, uint32_t usec);

/* ---- VRAM ---- */
int  rdna3_read_fb_location(rdna3_dev *d);           /* gfxhub_v3_0_3_get_fb_location */
void rdna3_vram_alloc_init(rdna3_dev *d, uint64_t lo, uint64_t hi);
/* returns VRAM offset, or UINT64_MAX on failure */
uint64_t rdna3_vram_alloc(rdna3_dev *d, uint64_t size, uint64_t align, const char *tag);
/* [lo, hi) for rdna3_vram_alloc_top; rdna3_vram_alloc_init rewinds it too */
void rdna3_vram_alloc_top_init(rdna3_dev *d, uint64_t lo, uint64_t hi);
/* from the top of that window down; falls back to rdna3_vram_alloc when
 * there is no top window */
uint64_t rdna3_vram_alloc_top(rdna3_dev *d, uint64_t size, uint64_t align, const char *tag);
static inline uint64_t rdna3_vram_mc(const rdna3_dev *d, uint64_t off) { return d->fb_start + off; }
/* a ring's 32-bit fence number widened past 'base', the job number the UAPI
 * last handed out on that ring (amdgpu's 64-bit job->uf_sequence) */
static inline uint64_t rdna3_seq_widen(uint64_t base, uint32_t seq32)
{
    return base + (uint32_t)(seq32 - (uint32_t)base);
}
/* amdgpu_gmc_vram_pa: VRAM offset -> physical address (amdgpu_gmc_vram_mc2pa) */
static inline uint64_t rdna3_vram_pa(const rdna3_dev *d, uint64_t off) { return d->fb_offset + off; }
volatile uint8_t *rdna3_vram_cpu(rdna3_dev *d, uint64_t off, uint64_t len);
void rdna3_vram_write(rdna3_dev *d, uint64_t off, const void *src, uint64_t len);
void rdna3_vram_read(rdna3_dev *d, uint64_t off, void *dst, uint64_t len);
/* one aligned 32-bit load of a value the GPU writes (fences, wptr writebacks) */
uint32_t rdna3_vram_read32(rdna3_dev *d, uint64_t off);
void rdna3_vram_fill(rdna3_dev *d, uint64_t off, uint8_t v, uint64_t len);
/* Indirect access through MM_INDEX/MM_DATA (works for all of VRAM). */
void rdna3_vram_mm_read(rdna3_dev *d, uint64_t off, uint32_t *dst, uint32_t dwords);
/* the write direction of amdgpu_device_mm_access: VRAM beyond the BAR */
void rdna3_vram_mm_write(rdna3_dev *d, uint64_t off, const uint32_t *src, uint32_t dwords);

/* ---- doorbells (WDOORBELL64) ---- */
void rdna3_doorbell_wr64(rdna3_dev *d, uint32_t index, uint64_t v);
/* WDOORBELL32: the VCN and JPEG rings' 32-bit doorbells */
void rdna3_doorbell_wr32(rdna3_dev *d, uint32_t index, uint32_t v);

/* ---- NBIO / HDP ---- */
void rdna3_nbio_remap_hdp(rdna3_dev *d);
/* nbio_v4_3_get_rev_id: the chip revision from the NBIO strap (not the PCI
 * revision); DEV_INFO's external_rev is this + 0x20 on GC 11.0.3 */
uint32_t rdna3_nbio_rev_id(rdna3_dev *d);
/* the product name by PCI device and revision ID, as libdrm's amdgpu.ids
 * (amdgpu_get_marketing_name) names it; a generic Navi 3x name otherwise */
const char *rdna3_marketing_name(uint16_t device, uint8_t revision);
/* the same; *exact (may be NULL) 0 when the generic name was taken */
const char *rdna3_marketing_name_ex(uint16_t device, uint8_t revision, int *exact);
/* gfx_v11_0_get_gpu_clock_counter: GOLDEN_TSC, upper read twice so a carry
 * between the two halves cannot tear the value (AMDGPU_INFO_TIMESTAMP) */
uint64_t rdna3_gpu_clock_counter(rdna3_dev *d);   /* nbio_v4_3_set_reg_remap + remap_hdp_registers */
void rdna3_hdp_flush(rdna3_dev *d);        /* amdgpu_hdp_generic_flush */
void rdna3_hdp_invalidate(rdna3_dev *d);   /* no-op on HDP 6.0, kept for symmetry */

#ifdef __cplusplus
}
#endif
#endif /* RDNA3_HW_H */
