# Provenance of accel/

Imported from **NullMoth NVIDIA Driver for macOS**
<https://github.com/nullmoth/nvidia-macos-driver>, commit
`b9a5a2b35c6475b1df0e7438916df25ad5c6d1d8`, path `kexts/NVRM/accel/`
plus the ABI headers `kexts/NVRM/nvrm_{vram,gpuva,surfshare}_abi.h`.
Licence: PolyForm Noncommercial 1.0.0 (`../metal/LICENSE.PolyForm-Noncommercial-1.0.0`),
© NullMoth Systems. Used under the permission the NullMoth author gave this
project (see `/NOTICE`): **AMD GPUs only**, without interfering with the
NullMoth NVIDIA driver.

The commit "accel: import NullMoth accelerator kext sources verbatim"
holds the upstream files byte-for-byte; the AMD adaptation is the next
commit (`git log -- accel/`).

## What changed for AMD

* Every class / metaclass, sysctl, boot-arg, bundle id and platform
  function name is renamed (`NV*` → `RD3*`, `NVAccel` → `RDNA3Accelerator`,
  `debug.nvaccel*` → `debug.rdna3accel*`, `nvaccel=1` → `rdna3accel=1`,
  `-nvoff` → `-rdna3off`). Metaclass names are global in the kernel, so
  this is what lets both drivers be installed side by side.
  `make -C accel check` fails if an `NV*` metaclass survives.
* `rdna3k_abi.h` (MIT, ours) replaces the three NVRM ABI headers: same call
  shapes, our names, answered by `RDNA3Accel.kext`
  (`kext/src/RDNA3Platform.cpp`).
* VRAM and device handles come from the core kext instead of the NVIDIA
  framebuffer; scanout and flips stay with the framebuffer
  (`RDNA3Framebuffer`, Stage 3).
* `probe()` accepts Navi 3x ids instead of NVIDIA ones; `start()` waits for
  `rdna3-claimed` (set by `RDNA3Accel`) instead of `nvrm-claimed`.
* The display CRC sysctl asks the framebuffer (`rdna3DisplayCrc`) instead of
  NVKMS.
* Kept as is: the reconstructed `re/` and `iofam/` headers, the family
  plumbing, `populateAccelConfig` values, the display pipe / IOSurface
  paths. Values there were reverse-engineered against NVIDIA's needs and
  may need AMD-specific tuning once this runs on a card.
