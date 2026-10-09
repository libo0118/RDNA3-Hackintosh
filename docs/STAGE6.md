# Stage 6: Metal

We ported the Metal plugin and the AIR→SPIR-V translator from the NullMoth project
(`metal/`; origin and licenses in `metal/PROVENANCE.md`, the author's permission
to use it for AMD in `NOTICE`).

```
Metal app → RDNA3MTLDriver.bundle (metal/plugin) → librdna3mtl_translate.dylib (AIR→SPIR-V)
          → libvulkan_radeon.dylib (RADV, Stage 5) → libdrm_xnu → RDNA3Accel.kext
```

```sh
make -C metal plugin        # RDNA3MTLDriver.bundle (downloads Vulkan-Headers)
make -C metal translator    # librdna3mtl_translate.dylib (cargo)
```

## What we changed compared to NullMoth

* `metal/plugin/rdna3mtl_config.h`: all AMD paths and names in one
  place (`/Library/GPUBundles/rdna3mtl/`: RADV, loader, translator, configuration;
  `rdna3mtl-allow.txt`; sysctl `debug.rdna3accelfb`). Nothing conflicts with
  a NullMoth installation.
* Plugin code changed only where it has paths. NVIDIA-only parts
  (the `NVIDIAShared` compiler, the SASS hook in NVK, NVDEC/NVENC) already have
  clean fallback paths when their files do not exist.
* Translator unchanged (LGPL-3.0+), built as a separate dylib under a new name.

## What is still needed for Metal to see the card

1. **Accelerator (IOAccelerator)**: a service with `MetalPluginName` /
   `MetalPluginClassName`, through which `MTLCopyAllDevices` finds the plugin
   and WindowServer composites. NullMoth has it in `kexts/NVRM/accel`
   (IOAcceleratorFamily2 with reconstructed headers). The AMD port is a separate
   kext loaded from the AuxKC (Stage 7).
2. RADV with a working CS (Stage 2 + 5).
3. The process allow list (`metal/rdna3mtl-allow.txt`) starts narrow:
   `mtlprobe`, `nvmtltest`, `vulkaninfo`.

## Notes

* The translator's unit tests need `validation/fixtures`, which NullMoth
  does not publish. The library itself builds on Linux and macOS (CI).
* Obj-C class names (`NVMTLDevice` etc.) were kept as upstream to keep the diff
  small. A collision is only possible if both bundles end up in one process.
