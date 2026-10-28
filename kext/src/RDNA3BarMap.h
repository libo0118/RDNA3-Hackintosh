/*
 * RDNA3BarMap.h - the core kext's kernel mapping of a window of BAR0 (the
 * VRAM aperture), shared by RDNA3Accel::drmInit and RDNA3Bringup::mapVram
 * (DRIVER-PLAN N62; the rules and the evidence in common/rdna3_bar_window.h).
 *
 * Apple's AMD driver does the same (ATIController::mapPCISubRange):
 * getDeviceMemoryWithRegister, IODeviceMemory::withSubRange, map. The map
 * KPI returns no IOReturn, so a failure is told apart by its stage: no
 * IODeviceMemory (IOPCIFamily did not assign BAR0), no sub-range (no
 * memory), or the map of N MiB failed (the VM layer), with every size tried.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_BAR_MAP_H
#define RDNA3_BAR_MAP_H

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSDebug.h>
#include "rdna3_bar_window.h"

struct Rd3BarWindow {
    IODeviceMemory *dev;   /* BAR0 as IOPCIFamily assigned it (retained), or NULL */
    IOMemoryMap *map;      /* the kernel window [0, mapped) of BAR0, or NULL */
    uint64_t barPhys;      /* dev's physical address (0 without dev) */
    uint64_t barLen;       /* dev's length (0 without dev) */
    uint64_t mapped;       /* the window's length (0 = not mapped) */
    uint64_t cap;          /* rdna3vramwin in bytes */
    uint32_t tries;        /* map attempts */
    uint64_t failed[4];    /* the window sizes whose map failed, in order */
    char why[400];         /* the reason when nothing was mapped, "" otherwise */
};

/* CPUID.1:ECX[31], the hypervisor bit (IOPCIConfigurator::addHostBridge's
 * CPUID_FEATURE_VMM test) */
static inline int rd3CpuIsVmm()
{
    uint32_t a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    (void)b;
    (void)d;
    return (int)((c >> 31) & 1u);
}

/* BAR0's size from the PCIe resizable BAR capability, MiB (0 = none / not BAR0's) */
static inline uint64_t rd3ReBarBar0Mib(IOPCIDevice *pci)
{
    IOByteCount off = 0;
    /* extended capability IDs are passed negated (kIOPCIExpressCapabilityIDResizableBAR) */
    if (!pci->extendedFindPCICapability((UInt32)-0x15, &off) || !off)
        return 0;
    uint32_t n = (pci->configRead32((UInt16)(off + 8)) >> 5) & 7u;   /* number of resizable BARs */
    if (n == 0 || n > 6)
        n = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t mib = rd3_rebar_ctrl_mib(pci->configRead32((UInt16)(off + 8 + 8 * i)));
        if (mib)
            return mib;
    }
    return 0;
}

/*
 * Map the window. argMib: rdna3vramwin (0 = default). Returns true when a
 * window is mapped; out->dev may be set either way (the caller owns the
 * references in out and releases dev and map).
 */
static inline bool rd3MapBar0Window(IOPCIDevice *pci, uint32_t argMib, Rd3BarWindow *out)
{
    bzero(out, sizeof(*out));
    out->cap = rd3_barwin_cap(argMib);
    IODeviceMemory *dev = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!dev || !dev->getLength()) {
        uint64_t raw = pci->configRead32(kIOPCIConfigBaseAddress0);
        if ((raw & 6u) == 4u)
            raw |= (uint64_t)pci->configRead32(kIOPCIConfigBaseAddress1) << 32;
        raw &= ~0xfull;
        const int vmm = rd3CpuIsVmm();
        const uint64_t rebar = rd3ReBarBar0Mib(pci);
        snprintf(out->why, sizeof(out->why),
                 "BAR0 has %s (config space: BAR0 at 0x%llx, resizable BAR size %s%llu MiB, %s): %s",
                 dev ? "a zero-length IODeviceMemory" : "no IODeviceMemory from IOPCIFamily",
                 (unsigned long long)raw, rebar ? "" : "unknown/", (unsigned long long)rebar,
                 vmm ? "CPUID says VMM" : "no VMM bit",
                 rd3_barwin_why_text(rd3_barwin_unassigned_why(raw, vmm, rebar)));
        if (dev) {
            dev->retain();
            out->dev = dev;
            out->barPhys = dev->getPhysicalAddress();
        }
        return false;
    }
    dev->retain();
    out->dev = dev;
    out->barPhys = dev->getPhysicalAddress();
    out->barLen = dev->getLength();
    for (uint64_t w = rd3_barwin_first(out->barLen, out->cap); w; w = rd3_barwin_next(w)) {
        IOMemoryMap *m = nullptr;
        if (w == out->barLen) {
            m = dev->map(kIOMapAnywhere | kIOMapWriteCombineCache);
        } else {
            IODeviceMemory *sub = IODeviceMemory::withSubRange(dev, 0, (IOPhysicalLength)w);
            if (!sub) {
                snprintf(out->why, sizeof(out->why),
                         "IODeviceMemory::withSubRange(BAR0, 0, %llu MiB) failed (no memory)",
                         (unsigned long long)(w >> 20));
                return false;
            }
            m = sub->map(kIOMapAnywhere | kIOMapWriteCombineCache);
            sub->release();   /* the map holds the descriptor */
        }
        out->tries++;
        if (m && m->getVirtualAddress() && m->getLength() >= w) {
            out->map = m;
            out->mapped = w;
            out->why[0] = '\0';
            return true;
        }
        OSSafeReleaseNULL(m);
        if (out->tries <= sizeof(out->failed) / sizeof(out->failed[0]))
            out->failed[out->tries - 1] = w;
    }
    char sizes[96];
    size_t o = 0;
    sizes[0] = '\0';
    for (uint32_t i = 0; i < out->tries && i < sizeof(out->failed) / sizeof(out->failed[0]); i++) {
        if (o + 1 >= sizeof(sizes))
            break;
        int k = snprintf(sizes + o, sizeof(sizes) - o, "%s%llu", i ? ", " : "",
                         (unsigned long long)(out->failed[i] >> 20));
        if (k < 0)
            break;
        o += (size_t)k;
    }
    snprintf(out->why, sizeof(out->why),
             "the kernel map of BAR0 (phys 0x%llx, %llu MiB) failed for every window tried "
             "(%s MiB, write-combined; the map KPI returns no IOReturn)",
             (unsigned long long)out->barPhys, (unsigned long long)(out->barLen >> 20), sizes);
    return false;
}

#endif /* RDNA3_BAR_MAP_H */
