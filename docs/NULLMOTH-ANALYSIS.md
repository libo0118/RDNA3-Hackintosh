# Analysis: the NullMoth NVIDIA driver and what it means for RDNA3

Source: <https://github.com/nullmoth/nvidia-macos-driver> (commit `b9a5a2b`,
8.10.2026, version 1.1.0). This document describes the architecture in our own words.
**We do not copy code from there**: see “Licenses” below.

## NullMoth architecture

```
Metal app ─► NVMTLDriver.bundle ─► translator (AIR → SPIR-V) ─► NVK (Mesa) ─► kexts ─► GPU
```

| Layer | Their implementation | License | Our counterpart |
|---|---|---|---|
| Metal plugin | `plugin/` (Obj-C, ~7k lines in `nvmtl_vk.c` alone) implements `MTLDevice` etc. on top of Vulkan | PolyForm Noncommercial 1.0.0 | to be agreed with the author (see below) |
| Shader translator | `translator/` (Rust, based on metal2vulkan): Apple AIR → SPIR-V | LGPL-3.0-or-later | the same, unchanged: SPIR-V is vendor-independent |
| Vulkan | NVK from Mesa + patch `nvk/nvk-macos.patch` (~12k lines) | MIT (Mesa) | **RADV** from Mesa + our patch |
| Kernel | port of the NVIDIA r610 open modules with an OS layer `os-xnu*.cpp` | PolyForm NC / MIT+GPL (NVIDIA) | amdgpu port + `linux_compat/` (Stage 2) |
| Framebuffer | `NVRMFB.kext` (`IOFramebuffer` on top of NVKMS) | PolyForm NC | DCN 3.2.1 + `IOFramebuffer` (Stage 3) |
| Accelerator | `NVAccel` (`IOAccelerator`, IOAcceleratorFamily2) | PolyForm NC | Stage 4/7 |
| Display policy | `NVRMAGDC.kext` (AppleGraphicsDeviceControl) | PolyForm NC | Stage 7 |

## Key technical findings

### 1. The Metal plugin loads any Vulkan driver

`nvmtl_vk.c` opens the ICD through `dlopen()` (path from `NVMTL_ICD_LIB` or
`nvk_icd.json`) and calls `vk_icdGetInstanceProcAddr`. The core uses
standard Vulkan: `VK_KHR_external_memory_fd`, `VK_EXT_image_drm_format_modifier`,
`VK_EXT_mutable_descriptor_type`, `VK_EXT_memory_budget`, ray query, mesh shaders.
RADV supports all of this on RDNA3.

There are, however, parts tied to NVK: hooks into NVK (the “SASS
dispatch” fast path, `nvk_blend_state`), workarounds for NVK limitations, and `NVIDIAShared.bundle`
(NVVM). These have to be disabled or replaced for RADV.

### 2. Kernel↔userspace: emulating Linux ioctls through IOKit

This is the most important idea to adopt. NVK did not get a new backend: the existing
`nvkmd/nvrm` backend (from the Haiku port) calls `ioctl()` exactly as on Linux,
and a thin layer `nvrm_xnu.c` turns that into IOKit:

| Linux | macOS (NullMoth) |
|---|---|
| `open("/dev/nvidia0")` | `IOServiceOpen()` on the kext's service; a `/dev/null` descriptor is returned as the “fd”, and fd↔`io_connect_t` is kept in a table |
| `ioctl(fd, cmd, arg)` | `IOConnectCallMethod(conn, SEL_ESC, {cmd}, arg, size → arg)` |
| `mmap(fd, …)` | `IOConnectMapMemory64()` |
| waiting for events | a separate selector with a timeout |

**Consequence for us:** Stage 4 should not invent its own ABI. The kext implements
the **Linux amdgpu UAPI** (`include/uapi/drm/amdgpu_drm.h`, MIT):
`DRM_IOCTL_AMDGPU_INFO`, `GEM_CREATE`, `GEM_MMAP`, `GEM_VA`, `CTX`, `CS`,
`WAIT_CS`, `BO_LIST`, `SYNCOBJ_*`. Then RADV with the existing amdgpu winsys
works almost unchanged; we only replace the system call layer.

In Mesa, all RADV ioctls go through a single layer (`src/amd/common/ac_linux_drm.c`,
`ac_drm_*`, added for virtio native context). *I could not confirm this from this
environment (the proxy blocks gitlab.freedesktop.org); check in current Mesa.* If that is the case,
the RADV patch is limited to this file + WSI/IOSurface.

### 3. How Metal finds the plugin

The `IOAccelerator` service publishes in the IORegistry `MetalPluginName`
(path to the bundle in `/Library/GPUBundles/`), `MetalPluginClassName`
(the `MTLDevice` class) and `IOGLBundleName = AppleMetalOpenGLRenderer` (OpenGL on top of
Metal). For us it is analogous: `RDNA3MTLDriver.bundle` + our own `IOAccelerator`.

### 4. Lessons from OpenCore and kext loading (Sequoia)

* Display kexts link against IOGraphicsFamily / IOAcceleratorFamily2, which exist
  only in Apple's kernel collections, so **from Stage 3 on the kexts must load from the Auxiliary
  Kernel Collection** (`/Library/Extensions` + `kmutil`), not through OpenCore
  injection. Our Stage 1 kext (IOPCIFamily only) can still be injected.
* `csr-active-config = 430A0000` (0x0A43) tested with unsigned kexts.
* `Kernel > Block: com.apple.iokit.IONDRVSupport (Exclude)`, otherwise the firmware
  framebuffer takes display index 0.
* `amfi_get_out_of_my_way=0x1 amfi=0x80`, so that WindowServer loads an unsigned bundle.
* The macOS installer needs the opposite BAR settings (small BAR, no IONDRV block).
* They have an emergency boot-arg `-nvoff` and an EFI tool that sets it. We have `-rdna3off`.

### 5. Where AMD is harder than NVIDIA

NVIDIA (Turing+) has **GSP**: firmware on the card does most of the initialization, and
the r610 open modules are designed with portability across operating systems in mind
(`os-interface`). amdgpu has no such layer: initialization (PSP, SMU, GMC, IH,
GFX/MES) happens in the host driver and is written for Linux APIs (DRM, TTM,
`dma_fence`). Our Stage 2 is therefore larger than their equivalent.

## Licenses: what is allowed

| NullMoth component | License | Can we use it in an MIT repo? |
|---|---|---|
| `translator/` | LGPL-3.0+ | **yes**, as a separate dynamic library (LGPL requirements: sources + ability to replace it) |
| `nvk/` (patch for Mesa) | MIT | yes, as a model for the RADV patch |
| `plugin/`, `kexts/`, `build/`, `package/` | PolyForm Noncommercial 1.0.0 | **no** in an MIT repo. Possible: (a) the author's consent to a different license for the shared part, (b) a separate module under PolyForm NC, which would make the whole thing noncommercial, (c) a shared, neutral plugin project run together |

The amdgpu code (MIT) and our kext stay MIT regardless of the choice.

## Proposed questions for the author (NullMoth Discord)

1. Could the plugin (`plugin/`) have a “generic Vulkan” mode in which the NVK hooks are
   optional, so that it can run on top of RADV?
2. Would you agree to a license that allows combining the plugin with MIT code, or to
   splitting out a shared vendor-neutral part?
3. What is the minimal set of Vulkan extensions / features the plugin requires
   (so we can check it early on RADV under Linux)?
4. Which IOAccelerator/IOSurface behaviors that WindowServer expects were
   the hardest to reproduce?
