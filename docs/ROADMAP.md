# Plan

Legend: ✅ done · 🟡 in progress · ⬜ not started

| Stage | Goal | Status |
|---|---|---|
| 0 | Test bench, contact with the NVIDIA project | 🟡 documentation ready ([STAGE0](STAGE0.md)), NVIDIA project analyzed ([NULLMOTH-ANALYSIS](NULLMOTH-ANALYSIS.md)); hardware and contact are on your side |
| 1 | Kext recognizes the card, maps the BAR, reads registers and discovery | 🟡 code + tests ready ([STAGE1](STAGE1.md)); waiting for a test on hardware |
| 2 | Bring-up | 🟡 PSP + firmware + SMU, GMC (GART, MMHUB, GFXHUB), IH 6.0, GPUVM, SDMA 6.0, GFX 11 ring — in the simulator ([STAGE2](STAGE2.md)); `rdna3lvl` levels 4–8; doorbells, KIQ + KCQ0, MSI (`rdna3msi=1`), SMU PM (PPTable, GFXCLK/SOCCLK DPM, throttlers, fan, metrics; `rdna3pm`), hangs → soft recovery / GFX soft reset / mode-1 reset, VM fault → guilty ctx; MES, BACO/sleep still to do |
| 3 | Display: DCN 3.2.1, IOFramebuffer | 🟡 mode takeover from GOP, flips, VBL from the frame counter, CRC, DP AUX/HPD/EDID (monitor name, mode list) in the simulator; `RDNA3Framebuffer.kext` from NVRMFB ([STAGE3](STAGE3.md)); modesetting ⬜ |
| 4 | IOUserClient implementing the **amdgpu UAPI** | 🟡 CS on DMA/GFX/COMPUTE rings, fences, syncobj (+TRANSFER, fd within a process), GPUVM, USERPTR, SENSOR, GPUVM_FAULT, ctx reset state ([STAGE4](STAGE4.md)); unmodified libdrm_amdgpu submits CS end-to-end on the host; PRIME/IOSurface still to do |
| 5 | Vulkan: RADV with the existing amdgpu winsys | 🟡 `libvulkan_radeon.dylib` builds and links on macOS in CI with 3 patches ([STAGE5](STAGE5.md)) |
| 6 | Metal: plugin + translator from NullMoth | 🟡 imported, adapted to AMD, build in CI ([STAGE6](STAGE6.md)) |
| 7 | Integration: WindowServer, CA, PM, sleep | 🟡 IOAcceleratorFamily2 accelerator moved over from NullMoth, builds in CI ([STAGE7](STAGE7.md)); PM/sleep ⬜ |
| 8 | Packaging, OpenCore, releases | 🟡 `package/install.sh` / `uninstall.sh`; OpenCore and releases ⬜ |

## Stage 1: completion criteria

* the kext loads on Sequoia and matches the 7800 XT at all levels 0–3
  without a hang;
* `amd_discovery_dump` from the macOS blob == from the Linux blob (see STAGE1).

## Stage 2: order of work (port from amdgpu)

The order follows `amdgpu_device_ip_init()` for SOC21. Each step first
**reads** the state, compares it with Linux, and only then **writes**.

1. **Register layer**: the `reg_offset[hwip][inst][seg]` table from discovery,
   macros like `SOC15_REG_OFFSET`/`RREG32_SOC15`, SMN access (indirect through NBIO 4.3).
2. **Firmware**: loading from the kext's resources, parsing `amdgpu_firmware_header` headers
   (`amdgpu_ucode.h`, MIT).
3. **NBIO 4.3 / HDP 6.x**: doorbells, HDP flush.
4. **GMC 11 + MMHUB 3.0.x / GFXHUB 11** (exact versions from discovery): VRAM layout, aperture, GPUVM page tables
   (VMID0 for the kernel), guard buffer (dummy page).
5. **PSP 13.0.7**: SOS boot, PSP ring, TMR loading, and then, through PSP, firmware for GFX,
   SDMA, SMU, DMCUB (front-door loading, like amdgpu for RDNA3).
6. **SMU 13.0.7**: ✅ `smu_smc_hw_setup` (PPTable from PMFW + BoardTable from VBIOS,
   features, DPM, PPT, workload, metrics); no overdrive; memory/DCN DPM waits
   for Display Core.
7. **IH 6.0.x (OSSSYS)**: interrupt ring, MSI in IOKit (`IOInterruptEventSource` with MSI).
8. **GFX 11 / MES**: RLC, IMU, MES (on RDNA3 queues are managed by MES), KIQ ring;
   **SDMA 6.0**: first ring.
9. **Test**: `WRITE_DATA` on the GFX/SDMA ring to a buffer in VRAM and a read back through
   `MM_INDEX`, then fence + interrupt.

The hardware code in amdgpu is MIT, but it is written for Linux APIs (DRM, TTM,
`dma_fence`, kref). Plan: a thin compatibility layer (`linux_compat/`), only as much
as the ported files need, not an emulation of the whole Linux kernel.

## Stages 3–8: known decisions and risks

* **DC (display core)** is largely OS-independent
  (`dc/` + `dmub/`), so a port through a `dm_services` layer, modeled on amdgpu_dm,
  is realistic. IOFramebuffer is deprecated, but still works on x86 in Sequoia.
* **Stage 4: decision after the NullMoth analysis.** We do not design our own ABI.
  The IOUserClient accepts the Linux amdgpu ioctls (`amdgpu_drm.h`, MIT) through
  `IOConnectCallMethod`, mmap goes through `IOConnectMapMemory64`, and in userspace
  a thin layer replaces `open/ioctl/mmap`. The same setup works in NullMoth for NVK.
  Benefit: RADV and its tests work without rewriting the winsys, and each ioctl
  can be compared 1:1 with Linux.
* **Stage 5** starts with `vulkaninfo` and compute tests that pass on Linux
  on the same card: same ioctl stream, so differences point to
  a bug in the kext.
* **Stage 6**: NullMoth has already shown how Metal finds the plugin (`MetalPluginName`,
  `MetalPluginClassName` on the IOAccelerator service) and what an `MTLDevice` on top of Vulkan looks like.
  Their plugin is under PolyForm Noncommercial, so either a joint project with the author, or
  our own implementation. Reverse engineering of private Apple interfaces
  (AMD bundle, IOAccelerator) is needed to a smaller extent than the plan assumed.
  Legal uncertainty: interoperability usually supports RE of interfaces, but
  this is not legal advice.
* **Horizon**: macOS 26 is the last release on x86_64, so the project's usefulness
  ends together with its support.

Step-by-step hardware test: [TESTING-HW](TESTING-HW.md) (report: `tools/macos/collect.sh`).
In a VM on Linux: [TESTING-VM](TESTING-VM.md). What to do with the reports: [AFTER-TESTS](AFTER-TESTS.md).
