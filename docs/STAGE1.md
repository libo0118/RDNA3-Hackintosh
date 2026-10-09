# Stage 1: kext that recognizes the card

`RDNA3Accel.kext` matches `1002:747E` (Navi 32) and step by step reads
more and more from the hardware. You enable each level separately with the `rdna3lvl=` boot-arg,
so if one of them hangs the system, you know exactly which one.

| `rdna3lvl` | What it does | Risk |
|---|---|---|
| `0` (default) | PCI matching, reads config space (ID, revision, PCIe link, power state). Does not touch the BARs | none |
| `1` | + checks that the card is in D0, enables memory decoding, maps **BAR5 (MMIO)**, waits (max 2 s) for the PSP firmware to report ready (`MP0_SMN_C2PMSG_33` bit 31), reads the VRAM size (`RCC_CONFIG_MEMSIZE`) | low: these are exactly the first reads amdgpu does |
| `2` | + reads 10 KiB of the IP discovery table from the end of VRAM through the `MM_INDEX`/`MM_DATA` window, verifies checksums, decodes it and publishes it in the IORegistry | low: the only writes are `MM_INDEX`/`MM_INDEX_HI` |
| `3` | + reads `GRBM_STATUS` from the GC block at the address from discovery | low, first register addressed through discovery |

`-rdna3off` disables the kext completely (probe fails).

The kext **never** enables bus mastering (no DMA) and does not load firmware.

## Building

```sh
make kext            # on a Mac; output: build/RDNA3Accel.kext
make -C kext check   # plutil, kmod_info and metaclass symbols, kextlibs
```

## Loading

Recommended: injection through OpenCore (works regardless of the kext security
in Sequoia):

1. Copy `RDNA3Accel.kext` to `EFI/OC/Kexts/`.
2. In `config.plist > Kernel > Add` add an entry:
   `BundlePath = RDNA3Accel.kext`, `ExecutablePath = Contents/MacOS/RDNA3Accel`,
   `PlistPath = Contents/Info.plist`, `Enabled = true`, `Arch = x86_64`,
   `MinKernel = 24.0.0` (Sequoia).
3. Keep a second entry in the OpenCore picker / a second EFI copy with `Enabled = false`
   as a way out.

Alternatively, with SIP disabled: `sudo kmutil load -p build/RDNA3Accel.kext`.

Injection through OpenCore only works as long as the kext links only against IOPCIFamily.
From Stage 3 on (IOFramebuffer, IOAccelerator) the kexts must load from the Auxiliary Kernel
Collection (`/Library/Extensions` + `kmutil`), because IOGraphicsFamily and IOAcceleratorFamily2
exist only in Apple's kernel collections. NullMoth does it this way, see [NULLMOTH-ANALYSIS](NULLMOTH-ANALYSIS.md).

## What to check after boot

```sh
log show --last boot --predicate 'eventMessage CONTAINS "RDNA3:"'
ioreg -l -w0 -r -c RDNA3Accel
tools/macos/ioreg_discovery.sh          # (rdna3lvl>=2) save + decode the blob
```

Expected at `rdna3lvl=2` for the RX 7800 XT:

* `RDNA3,VRAMSizeMB = 16384`
* `IP discovery ... : ok`
* in the table: GC **11.0.3**, along with MP0/MP1 (PSP/SMU), SDMA, DMU (DCN), VCN, NBIF.
  Compare the exact versions with Linux; `amd_discovery_dump` also hints which
  firmware files will be needed in stage 2.

### Comparison with Linux (stage completion criterion)

```sh
# Linux, same card:
sudo tools/linux/dump_amdgpu_reference.sh ref/
build/host/amd_discovery_dump ref/discovery.bin > linux.txt
# macOS:
tools/macos/ioreg_discovery.sh mac.bin && build/host/amd_discovery_dump mac.bin > mac.txt
diff linux.txt mac.txt     # expected: no differences
```

## When something goes wrong

| Symptom | Likely cause |
|---|---|
| kext does not load, no logs | another driver won the match (fake-ID in OpenCore?) or wrong `Arch`/`MinKernel` |
| `device not in D0` | card put to sleep by firmware; add it to `DeviceProperties` with `built-in`, check the ASPM/ASPM L1 settings in the BIOS |
| `MMIO reads all-ones` | BAR5 not assigned (enable Above 4G), card in D3cold, or wrong slot |
| `IFWI NOT ready` | the card was not initialized by firmware; check that it works under Linux/Windows |
| `RCC_CONFIG_MEMSIZE = 0` | VRAM not trained; discovery table in system memory (unusual for a dGPU) |
| `no discovery signature` | different VRAM layout than we assume; compare `DRIVER_SCRATCH_0..2` with Linux |
| hang at `rdna3lvl=N` | stay at `N-1`, file an issue with the log from the previous level |

## Offset sources

All constants in `kext/src/RDNA3Regs.h` and `common/amd_discovery.*` were
checked against Linux (master branch, October 2026):
`amdgpu_discovery.c`, `include/discovery.h`, `include/soc15_hw_ip.h`,
`asic_reg/gc/gc_11_0_3_offset.h`, `asic_reg/nbio/nbio_4_3_0_offset.h`,
`amdgpu_device.c` (BAR5 = MMIO), `amdgpu_device_mm_access()` (the
`MM_INDEX`/`MM_DATA` sequence).
