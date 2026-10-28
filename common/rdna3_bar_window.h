/*
 * rdna3_bar_window.h - how much of BAR0 (the VRAM aperture) the core kext
 * maps into the kernel, and why it got none (DRIVER-PLAN N62). Plain C99,
 * no kernel headers: kext/src/RDNA3BarMap.h uses it, tests/test_bar_window.c
 * checks it on the host.
 *
 * Card run 2026-10-28c: BAR0 resized to 1 GiB, OVMF put it at 0x900000000
 * (its 64-bit window) and both of the core's maps of it failed ("cannot map
 * BAR0 (VRAM aperture)", "VRAM 16368 MiB (0 MiB visible)"), so the bring-up
 * stopped and WindowServer never reached the GPU. What macOS does with a
 * BAR (IOPCIFamily-726.0.5, IOPCIConfigurator.cpp):
 *
 *  - addHostBridge clears kIOPCIConfiguratorPFM64 when CPUID says VMM (a
 *    VM). probeBaseAddressRegister then leaves a 64-bit BAR's maxAddress at
 *    0xFFFFFFFF (IOPCIRangeInit's default) and IOPCIRangeListAllocateSubRange
 *    clamps every free range to it: in a VM a BAR above 4 GiB is never kept
 *    where the firmware put it, and a 1 GiB-aligned 1 GiB hole below 4 GiB
 *    does not exist in q35, so BAR0 gets no "assigned-addresses" entry and
 *    the IOPCIDevice no IODeviceMemory for it (getDeviceMemoryWithRegister
 *    returns NULL, so does mapDeviceMemoryWithRegister). The same happened
 *    to the 64-bit window in commit 0ae3df7 ("at 40 s macOS had them
 *    unmapped").
 *  - MAX_BAR_SIZE is 1 GiB: a bigger BAR's size is cut to its low 32 bits
 *    (16 GiB -> 0) and the BAR treated as 32-bit, on bare metal too. That is
 *    the "theoretical 1 GB maximum" of OpenCore's ResizeAppleGpuBars (its
 *    ocvalidate calls values above 10 = 1 GiB unsupported by macOS).
 *
 * Kernel VA is not the limit: IOMemoryDescriptorMapAlloc maps into
 * kernel_map's KMEM_RANGE_ID_DATA, which kmem_range_startup_init sizes to
 * what is left of the kernel VA after the pointer ranges (tens of GiB on
 * x86_64). Apple's AMD driver maps windows of a BAR, not the whole aperture:
 * ATIController::mapFramebuffer(bar, offset, length) -> mapPCISubRange ->
 * getDeviceMemoryWithRegister, IODeviceMemory::withSubRange, map(
 * kIOMapAnywhere | kIOMapWriteCombineCache) for the framebuffer BAR
 * (AMDSupport, macOS 26.4.1). So the core maps a window of BAR0 the same
 * way: at most rdna3vramwin=<MiB> (default 1 GiB, the largest BAR macOS
 * assigns), the whole BAR when it is smaller (256 MiB stays as it was),
 * halved on a failed map down to 256 MiB.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_BAR_WINDOW_H
#define RDNA3_BAR_WINDOW_H

#include <stdint.h>

#define RD3_BARWIN_MIB (1ull << 20)
/* IOPCIConfigurator's MAX_BAR_SIZE: no BAR macOS assigns is bigger */
#define RD3_BARWIN_DEFAULT_MIB 1024u
/* rdna3vramwin below this is raised to it (the kernel's own low window
 * and the display's first buffers live in the first MiBs of VRAM) */
#define RD3_BARWIN_MIN_MIB 64u
/* a failed map is retried with half the window down to this (the size
 * every card run before N62 mapped) */
#define RD3_BARWIN_FLOOR (256ull * RD3_BARWIN_MIB)
#define RD3_BARWIN_4G (1ull << 32)

/* the cap in bytes for rdna3vramwin=<MiB> (0 or absent = the default) */
static inline uint64_t rd3_barwin_cap(uint32_t arg_mib)
{
    if (arg_mib == 0)
        arg_mib = RD3_BARWIN_DEFAULT_MIB;
    if (arg_mib < RD3_BARWIN_MIN_MIB)
        arg_mib = RD3_BARWIN_MIN_MIB;
    return (uint64_t)arg_mib * RD3_BARWIN_MIB;
}

/* the first window to map: the whole BAR up to the cap, whole MiB */
static inline uint64_t rd3_barwin_first(uint64_t bar_len, uint64_t cap)
{
    uint64_t w = bar_len < cap ? bar_len : cap;
    return w & ~(RD3_BARWIN_MIB - 1);
}

/* the window to try after a failed map of 'cur': half of it while that is
 * still at least RD3_BARWIN_FLOOR, else 0 (give up; a BAR of 256 MiB or
 * less is tried once, as before N62) */
static inline uint64_t rd3_barwin_next(uint64_t cur)
{
    uint64_t h = cur / 2;
    return h >= RD3_BARWIN_FLOOR ? h & ~(RD3_BARWIN_MIB - 1) : 0;
}

/* the PCI resizable BAR capability's control register of BAR0 (PCIe 6.0
 * 7.8.6.3: bits 2:0 the BAR index, bits 13:8 the size, 2^n MiB): the size
 * of BAR0 in MiB, 0 when this control register is not BAR0's */
static inline uint64_t rd3_rebar_ctrl_mib(uint32_t ctrl)
{
    if ((ctrl & 7u) != 0)
        return 0;
    return 1ull << ((ctrl >> 8) & 0x3fu);
}

/* why BAR0 has no IODeviceMemory (getDeviceMemoryWithRegister NULL) */
enum rd3_barwin_why {
    RD3_BARWIN_WHY_VM_ABOVE_4G = 1,  /* in a VM, the firmware put it above 4 GiB */
    RD3_BARWIN_WHY_TOO_BIG,          /* bigger than IOPCIFamily's 1 GiB (resizable BAR) */
    RD3_BARWIN_WHY_UNKNOWN,          /* none of the two: macOS's log with pci_log= says */
};

/* raw_bar: BAR0's address from config space (0x10/0x14, flags cleared);
 * vmm: CPUID.1:ECX[31]; rebar_mib: BAR0's size from the resizable BAR
 * capability (0 = no capability / unknown) */
static inline enum rd3_barwin_why rd3_barwin_unassigned_why(uint64_t raw_bar, int vmm,
                                                            uint64_t rebar_mib)
{
    if (rebar_mib > RD3_BARWIN_DEFAULT_MIB)
        return RD3_BARWIN_WHY_TOO_BIG;
    if (vmm && raw_bar >= RD3_BARWIN_4G)
        return RD3_BARWIN_WHY_VM_ABOVE_4G;
    return RD3_BARWIN_WHY_UNKNOWN;
}

static inline const char *rd3_barwin_why_text(enum rd3_barwin_why w)
{
    switch (w) {
    case RD3_BARWIN_WHY_VM_ABOVE_4G:
        return "the firmware put it above 4 GiB and macOS in a VM keeps PCI BARs below 4 GiB "
               "(IOPCIConfigurator drops PFM64 when CPUID says VMM): give the VM no 64-bit PCI "
               "window (rdna3vm: RESIZE_BAR_LOG2=9 or 8, PCI_MMIO64_MB=0)";
    case RD3_BARWIN_WHY_TOO_BIG:
        return "BAR0 is bigger than 1 GiB, the most IOPCIFamily takes (MAX_BAR_SIZE): resize it "
               "for macOS (OpenCore Booter/Quirks/ResizeAppleGpuBars=10 or less, or Resizable BAR "
               "off)";
    default:
        return "IOPCIFamily did not assign it (the boot-arg pci_log=... prints its allocator)";
    }
}

#endif /* RDNA3_BAR_WINDOW_H */
