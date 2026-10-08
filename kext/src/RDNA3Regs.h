/*
 * RDNA3Regs.h - the handful of registers Stage 1 is allowed to touch.
 *
 * Every value here is cross-checked against Linux amdgpu (MIT):
 *   amdgpu_discovery.c            mmRCC_CONFIG_MEMSIZE, mmMM_INDEX/_HI/_DATA,
 *                                 mmMP0_SMN_C2PMSG_33, mmDRIVER_SCRATCH_0..2
 *   nbio_4_3_0_offset.h           cfgRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE = 0x...378c
 *   gc_11_0_3_offset.h            regGRBM_STATUS 0x0da4, BASE_IDX 0
 *   amdgpu_device.c               MMIO = PCI BAR 5 for every ASIC >= Bonaire
 *
 * Linux uses DWORD indices; we keep them as such and convert with REG_BYTES().
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_REGS_H
#define RDNA3_REGS_H

#include <stdint.h>

#define RDNA3_PCI_VENDOR_AMD        0x1002u
#define RDNA3_PCI_DEVICE_NAVI32     0x747Eu /* RX 7800 XT / RX 7700 XT */

/* BAR layout of Navi 3x (64-bit BARs take two slots):
 *   BAR0/1  VRAM aperture (prefetchable; 256 MiB, or full VRAM with ReBAR)
 *   BAR2/3  doorbells
 *   BAR4    I/O ports (legacy, unused)
 *   BAR5    MMIO registers  <-- the only one Stage 1 maps */
#define RDNA3_MMIO_BAR_CFG          0x24u   /* kIOPCIConfigBaseAddress5 */

#define REG_BYTES(dw)               ((uint64_t)(dw) * 4u)

/* Fixed (pre-discovery) registers, DWORD indices */
#define mmMM_INDEX                  0x0000u
#define mmMM_DATA                   0x0001u
#define mmMM_INDEX_HI               0x0006u
#define mmDRIVER_SCRATCH_0          0x0094u
#define mmDRIVER_SCRATCH_1          0x0095u
#define mmDRIVER_SCRATCH_2          0x0096u
#define mmRCC_CONFIG_MEMSIZE        0x0de3u /* VRAM size in MiB */
#define mmMP0_SMN_C2PMSG_33         0x16061u /* bit31: IFWI init complete */

#define MM_INDEX_APERTURE_VRAM      0x80000000u
#define C2PMSG_33_IFWI_DONE         0x80000000u
#define RDNA3_IFWI_WAIT_MS          2000u   /* same budget as amdgpu */

/* GC 11.0.3 (after discovery: GC base[0] + offset) */
#define regGRBM_STATUS              0x0da4u
#define regGRBM_STATUS_BASE_IDX     0u
#define GRBM_STATUS__GUI_ACTIVE     0x80000000u

#endif /* RDNA3_REGS_H */
