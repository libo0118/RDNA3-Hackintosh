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

## Update 2026-10-13: upstream since our import (`b9a5a2b` → `cd8e2bb`)

`accel/`, `fb/` and `metal/` were imported from `b9a5a2b`; upstream has 217
commits since. What matters for the open RDNA3 problems (card runs 37-40):

### 1. AppleGraphicsDeviceControl (`kexts/NVRMAGDC`) - we have none

Upstream arms the display in this order (`NVRM.cpp`, auto-go):
`debug.nvaccelfb=1` → `=4` → `debug.nvrmfb_agdc_k5=1` + `debug.nvrmfb_agdc=1`
→ `debug.nvaccel_iop=1` (→ zero-copy flip 60 s later; **outdated 2026-10-23**:
upstream `75e2e28` flips right after the release, the 60 s sleep is gone,
`NVRM.cpp` auto-go). The AGDC object is an
`AppleGraphicsDeviceControl` subclass attached to the GPU's PCI device that
answers IOPresentment's vendor queries:

| cmd | answer |
|---|---|
| `1` | vendor info: version `0x30000`, vendor name, PCI vendor id, class 2 |
| `0x980` | GPU capability: port mask, counts, `maxFramebuffers`, the PCI device and the framebuffers by `IOFBDependentIndex` |
| `0x921` (K5) | `kAGDCLinkConfig` per framebuffer index: the head's timing (pixel clock, h/v active, blanking, sync), 1 link, RGB 8 bpc, port `1 + index` |
| `0x711` (K5) | extended capability type `0x10` (plane scaler): source and destination limits = the head's raster |

The framebuffer publishes its timing for it (`NVRMTiming`: 13 words, tag
`0x4b350001`) and `IOFBDependentIndex`.

Our card runs show every display-pipe transaction arriving **without a
plane** (`debug.rdna3accel_iop_empty` ~1000 per boot, `iop_ok` 0,
`iop_src_surf` 0): WindowServer never hands a composited surface to the
pipe, frames reach the panel only through the swap path, gamma arrives as
plane-less transactions the base class refuses (0xe00002c7), and Metal
layer presents (M4) are never shown. Upstream gets plane-0 surfaces in
`performTransaction` once AGDC answers. (**Outdated 2026-10-23**: we have
the AGDC object now (`agdc/`), and on the agdc profile transactions still do
not arrive with us - `iop_ok 0`, `iop_empty 0` in 2026-10-17b..18b -:
WindowServer runs the legacy page-flip mode (`init_page_flip: page flip
mode is on`), so the desktop reaches the panel through `submitSwapCopy` /
`submitFlipBuffer`, which upstream treats as a capped probe.) Plan: an AMD-only AGDC object in
`RDNA3Accelerator` (or its own kext), fed by a timing property from
`RDNA3Framebuffer`, armed between `rdna3accelfb=4` and `rdna3acceliop=1`;
the report records `iop_ok` / `iop_src_surf` / the AGDC counters.

### 2. Accelerator fixes to port (`accel/`)

* `c2af33d`: a copied frame waits until the IOSurface's newest pixels are
  in system memory (page-off under the family lock, bounded wait) or keeps
  the previous frame, at most 4 in a row per head. Fixed full-screen
  flicker on machines whose frames take the copy path ("windows flicker
  while something moves", card runs 38-39).
* `79e1c2f`: a pending dirty-surface mark remembers the resource that set
  it; `Resource::free` drops only its own marks (no stale marks, no device
  cache read during destruction).
* `249ef8c`: only head 0 becomes the accelerator's primary framebuffer.

### 3. Framebuffer behaviour to adopt (`fb/`)

* `08c5bba` (issue #110): a known display that is seen disconnected and
  then connected again is relit with a full mode set of the same head; the
  framebuffer stays and IOGraphics is not told. Our hotplug sends a connect
  change instead and the re-created pipe stays black after a replug (run
  40). Candidate: the same relight as an `rdna3hotplug` mode.
* `5b0965e`: a display connected after boot gets its own framebuffer by
  matching the idle display nub again.
* `7c7b2a4`: only rasters larger than the boot raster are added beyond what
  macOS proposes (smaller ones made the login window black on a high
  refresh monitor).
* `79e1c2f` / `531faec`: display power requests share one gate; a refused
  wake keeps the sleeping state so the next request retries, and no
  power-on notification follows a failed wake.

### 4. Metal plugin and translator (`metal/`)

~40 vendor-neutral upstream commits (translator fixes, OpenCL, Blender,
WebGL in Chromium, the Unity lock-up, surface upload copies, texture
swizzle, pixel-sampler variants, the surface-writer map lock). The upstream
diff applies to `metal/` except `NVMTLDevice.m` and `nvmtl_vk.c`, where our
AMD changes sit; upstream's own vendor switch (`37e7c9d`, RADV for
`0x1002`) replaces most of them.

### 5. Upstream's own AMD path (`gpu-port/`)

`NMXGPU.kext` runs drm-kmod's amdgpu (display core included) on FreeBSD's
LinuxKPI over xnu, RADV through libdrm for Darwin, and `NMXAccel` (this
accelerator built with `-DNMX_ACCEL`) as an app-only Metal device with
frames copied to the DRM framebuffer. Not loaded on hardware yet, one mode,
no per-output framebuffers. Our port is further on display, power and
sleep; their tree is a reference to compare amdgpu behaviour against (for
example VCN power gating) once it runs.

## Update 2026-10-23: upstream `75e2e28` against our present path (DRIVER-PLAN N38)

Compared: their `kexts/NVRM/accel/nvrm-accel.cpp` (performTransaction,
the asynchronous flip worker, the flip gate) and
`kexts/NVRMFB/fb/nvrm-fb.cpp` (the flip and its latch) with ours. Design
studied, our own code written (`common/rdna3_flip_queue.h`, the
accelerator's present queue, N38):

| Their behaviour | Ours before | Ours now (N38) |
|---|---|---|
| performTransaction waits up to ~1.25 refresh for the flip in flight (an IOLock woken by the worker), then queues the newest frame for the worker (newest replaces queued) | 50 x `IOSleep(1)` on WindowServer's thread, then the frame **dropped** while answering "shown" | queued at once (no wait at all), newest wins, Busy kept |
| framebuffer Busy while a flip is in flight = "kept" | the framebuffer spun up to 20 ms in `IODelay`, then flipped anyway | `RD3K_FLIP_FLAG_NOWAIT` answers Busy, the present stays queued |
| the transaction completes (`signalTransactionInterrupt`) after the flip landed; the framebuffer's flip waits for its own latch | completed after the flip call returned (before the latch) | completed by the worker at the latch (`rdna3FlipLatchWait`, woken at the vblank) |
| the worker waits for the surface's GPU completion (`nvSurfWaitReady`, the plugin's pending/ready marks) | none | not ported: our plugin sends the dirty mark only after waiting for the GPU, so the mark is the GPU-ready gate (open: N38 (5)) |
| zero-copy flip only when the plugin wrote the VRAM (`fPluginWroteCookie` == the allocation's cookie) and it is the newest | VRAM-newest bit only | both (`rd3_iop_flip_ok`, `debug.rdna3accel_iop_wrote`) |
| PerformanceStatistics read-copy-modify-write (their temperature) | the core published utilization on its own service only | the accelerator's own, every 250 ms (`debug.rdna3accel_perfstats`) |

Upstream changes in this area not ported (yet): the plugin's GPU pending /
ready surface marks and `nvSurfWaitReady` (N38 (5)); the copy path's "page
off only VRAM the plugin wrote" rule as a separate check (ours pages off
only surfaces with a pending dirty mark, which only the plugin sets); the
parked-allocation restore before a dirty mark (`nvAccelRestoreParkedForDirty`);
the framebuffer's late-flip handling (waiting past a period, the stuck limit,
the present statistics HUD); the accelerator's temperature key; the headless
check that restores the plugin name without arming; auto-go without the 60 s
sleep (our boot gates are boot-args).

