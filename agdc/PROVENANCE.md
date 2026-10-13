# Provenance of agdc/

Derived from the **NullMoth NVIDIA Driver for macOS**
<https://github.com/nullmoth/nvidia-macos-driver>, commit
`cd8e2bb4` (2026-10-13), `kexts/NVRMAGDC/` (`nvrm-agdc.cpp`,
`nvrm-agdc-k5.h`, `AppleGraphicsDeviceControl_re.h`).
Licence: PolyForm Noncommercial 1.0.0 (`../metal/LICENSE.PolyForm-Noncommercial-1.0.0`),
© NullMoth Systems. Used under the permission the NullMoth author gave this
project (see `/NOTICE`): **AMD GPUs only**, without interfering with the
NullMoth NVIDIA driver.

## Kept from NVRMAGDC

* the reconstructed `AppleGraphicsDeviceControl` base class (0x110 bytes)
  and the vendor command answers: 1 (vendor info), 0x980 (GPU capability:
  the framebuffers by `IOFBDependentIndex`), 0x921 `kAGDCLinkConfig` and
  0x711 plane scaler ("K5"), with their structure layouts;
* the slot table rules (no hole, indices below the maximum, one per index)
  and the debug sysctls.

## Changed for AMD

* Names (`RDNA3AGDC`, `RD3DeviceControl`, `debug.rdna3agdc*`, the bundle
  id), vendor `AMD` / 0x1002; the framebuffers are `RDNA3Framebuffer`s
  and their timing property is `RDNA3Timing` (`fb/rdna3-fb.cpp`,
  `publishK5Timing`).
* Armed by the boot-arg `rdna3agdc=1` when the first framebuffer publishes
  (NullMoth's NVRM writes the sysctls in its bring-up thread); the
  accelerator opens its display pipes only after it is up
  (`rd3AccelBootGates`). The table is scanned again whenever a framebuffer
  publishes or terminates (our second head comes later than the first);
  framebuffers that leave it stay retained while the object lives.
* The object is made only when the running `AppleGraphicsDeviceControl`
  has the size this build knows (`getClassSize`).
* `rdna3_agdc_k5.h` also builds the timing words from a mode's detailed
  timing; `tests/test_agdc_k5.c` checks the answers on the host.
