# Provenance of fb/

`rdna3-fb.cpp` is derived from the **NullMoth NVIDIA Driver for macOS**
framebuffer, <https://github.com/nullmoth/nvidia-macos-driver>, commit
`b9a5a2b35c6475b1df0e7438916df25ad5c6d1d8`, `kexts/NVRMFB/fb/nvrm-fb.cpp`.
Licence: PolyForm Noncommercial 1.0.0, © NullMoth Systems, used under the
permission its author gave this project (see `/NOTICE`): **AMD GPUs only**,
without interfering with the NullMoth NVIDIA driver.

## Kept from NVRMFB

* the IOFramebuffer method set and its answers: connection attribute
  table, `wsrv` / power attributes, pixel information, aperture as a
  sub-range of the PCI BAR, `getVRAMRange` = the whole BAR, power states,
  VBL / connect interrupt registration, the retain-per-queued-thread-call
  scheduling and the stop() drain;
* matching in the `IOFramebuffer` category above IONDRVFramebuffer so only
  one framebuffer publishes the boot VRAM (their `NVRMFBClaim` finding);
* the accelerator-facing calls (`ScanoutInfo`, `FlipToSurface`,
  `FlipToSurfacePure` with flag 1 = home, display CRC).

## Changed for AMD

* NVKMS (mode set, surfaces, vblank callbacks, EDID) is gone. The head is
  the scanout the VBIOS / GOP left on DCN 3.2, read and flipped by
  `RDNA3Accel.kext` (`rdna3DisplayInfo` / `rdna3DisplayFlip` /
  `rdna3DisplayStatus`, `accel/rdna3k_abi.h`, implemented in
  `hw/dcn32.c` + `kext/src/RDNA3Display.cpp`, MIT).
* VBL follows the OTG frame counter instead of an NVKMS interrupt; without
  the core kext a timer, like NVRMFB's fallback.
* Without a usable DCN head the boot console (`PE_Video`) is driven as a
  dumb framebuffer.
* One mode (the firmware's), no detailed timings / EDID / hardware cursor
  yet, one head. Names, boot-args (`rdna3fb=1`, `-rdna3off`) and the bundle
  id are ours, so a NullMoth install never collides.
