# Stage 7: system integration (IOAcceleratorFamily2 accelerator)

Metal and WindowServer find the card through an **IOAccelerator**: a service of class
`IOGraphicsAccelerator2` with the properties `MetalPluginName` /
`MetalPluginClassName`. It is provided by a separate kext, `RDNA3Accelerator.kext`
(`accel/`), ported from the NullMoth accelerator (see `accel/PROVENANCE.md`).

```
Metal.framework ── MetalPluginName ──▶ /Library/GPUBundles/RDNA3MTLDriver.bundle (Stage 6)
      │                                        │ RADV (Stage 5) → libdrm_xnu → RDNA3UserClient
IOAcceleratorFamily2
      │
RDNA3Accelerator.kext (accel/)   ── callPlatformFunction (accel/rdna3k_abi.h) ──▶ RDNA3Accel.kext
      │  VRAM, device handle, GPU VA ranges                                       (kext/src/RDNA3Platform.cpp)
      └─ scanout, flip, CRC ──▶ RDNA3Framebuffer (Stage 3, fb/) ──▶ RDNA3Accel (DCN)
```

```sh
make -C accel                 # build/RDNA3Accelerator.kext (Sequoia)
make -C accel NM_TAHOE=1      # vtable layout for macOS 26
make -C accel check           # checks that no NV* metaclass is left
```

## Gates (as in NullMoth)

| What | How |
|---|---|
| accelerator start | boot-arg `rdna3accel=1` (without it `start()` returns false: protection against a boot loop) |
| turning everything off | `-rdna3off` |
| ordering | waits up to 30 s for `RDNA3Accel` to set `rdna3-claimed` on the IOPCIDevice (it does this after a successful `drmInit`, i.e. `rdna3lvl>=2`) |
| Metal plugin | `MetalPluginName` is hidden at start; `sudo sysctl -w debug.rdna3accelfb=1` restores it (only after the framebuffer is registered) |
| display pipe | `debug.rdna3accelfb=3`, `debug.rdna3accel_iop=1`, as in NullMoth |

## Accelerator ↔ RDNA3Accel interface

| Function | Status |
|---|---|
| `rdna3AllocVram` / `rdna3FreeVram` | ✅ the same VRAM heap as GEM, only within the BAR0 range (kva + phys) |
| `rdna3DeviceHandles` | ✅ device cookie |
| `rdna3GpuVaAlloc` / `Free` | ✅ 1 TiB range starting at 64 TiB |
| `rdna3GpuVaMap` | ✅ the accelerator's own GPUVM context (separate VMID), linear PTEs to VRAM allocations; unmap/free clears the PTEs |
| `rdna3NewAccelClient` | ⬜ `kIOReturnUnsupported`; RADV opens `RDNA3Accel` directly |
| `rdna3ScanoutInfo`, `rdna3FlipToSurface`, `rdna3DisplayCrc` | 🟡 `RDNA3Framebuffer` ([STAGE3](STAGE3.md)); flip through `rdna3DisplayFlip` in the core, tested only in the simulator |
| `rdna3SurfPageoff` | ✅ from `rdna3lvl=7` SDMA0 through the GART window (16 MiB at the end of GART, like `amdgpu_ttm_map_buffer`); before that, the CPU through the BAR |
| `rdna3SurfShare` | ⬜ no equivalent of RM handles shared between clients; RADV shares through GEM |

## Next steps

1. First hardware test: `rdna3lvl=2 rdna3enum=1 rdna3accel=1`, second
   card driving a display. Expected: `ioreg -c RDNA3Accelerator` shows the service,
   log `IOGraphicsAccelerator2::start -> 1`.
2. `populateAccelConfig`: the values (flags, texture limits) come from NVIDIA.
   RDNA3 supports 16384×16384, so the limits fit. The meaning of the other
   `IOAccelConfig` fields is not documented (Apple does not publish it), and these
   values worked on hardware in NullMoth, so they stay until a test on the
   card shows a difference. The accelerator logs what the family wrote before
   us (`cfg+0x..`), for comparison.
4. Metal plugin in the AMD build: `vendorName` returns `AMD`, the device name
   is the part of the RADV name before ` (RADV ` (e.g. "AMD Radeon RX 7800 XT
   (RDNA3MTL over AMD Radeon RX 7800 XT (RADV NAVI32))"), and registration of
   the NVDEC/NVENC encoders in VideoToolbox is disabled
   (`RDNA3MTL_NV_VIDEO_CODECS 0`). The equivalent would be VCN 4.0, which is not
   ported.
3. `RDNA3Framebuffer` (Stage 3): without it the display machine has no heads,
   so `debug.rdna3accelfb=1` has nothing to register and WindowServer does not
   composite on this card.
