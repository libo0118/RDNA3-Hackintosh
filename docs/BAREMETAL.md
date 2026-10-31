# macOS 26 on the real PC: OpenCore installer USB, dual boot with Linux

How to install macOS 26 Tahoe on the owner's PC (MSI B550-A PRO, Ryzen 7 5800X,
RX 7800 XT) from a USB stick, next to Linux, and then run this driver on it.
The driver side (kexts, boot-args, the way back) is
[INSTALL-OPENCORE](INSTALL-OPENCORE.md); this page is the machine, the stick
and the order of things. The AMD CPU side follows Dortania's AMD (Zen) guide.

> **Status (2026-10-31):** macOS 26.7.2 is installed on this PC and boots with
> the driver (`config-driver.plist`). Getting there took three days of hand
> edits on the stick; every one is now in the generator, so a stick rebuilt
> from this page works first time. Part 6 lists what was learned and what is
> still open.

## 1. The hardware (read from the PC on 2026-10-29)

| Part | What | Consequence |
|---|---|---|
| Board / BIOS | MSI B550-A PRO (MS-7C56), BIOS A.M2 (2026-08-13) | SSDT-EC-USBX-DESKTOP (no EC device in ACPI), SSDT-CPUR (CPUs are `\_SB.PLTF.C000..C00F`) |
| CPU | Ryzen 7 5800X, 8 cores / 16 threads, Zen 3 | AMD_Vanilla kernel patches with core count **8**, `ProvideCurrentCpuInfo`, dummy power management |
| GPU | RX 7800 XT `1002:747E` (+ HDMI audio `1002:AB30`) | this driver; no WhateverGreen; BAR size handling (part 2) |
| Ethernet | RTL8111/8168 `10ec:8168` | RealtekRTL8111 3.0.0 |
| Audio | Realtek **ALC897** (`10ec:0897`, subsystem `1462:9c56`) on `1022:1487` | AppleALC, `alcid=13` (a guess, part 6) |
| NVMe | Crucial P3 / P3 Plus `c0a9:540a`, P310 `c0a9:5426` (DRAM-less) | NVMeFix |
| USB | AMD 500 Series XHCI `1022:43ee` (10 USB 2 + 4 USB 3 root ports), Matisse XHCI `1022:149c` (4 + 4) | 14 and 8 ports: both under macOS's limit of 15 per controller, **no USB map needed** |
| Disks | `nvme1n1` (931.5 GB): ESP 1 GB (`/boot`, Windows Boot Manager and GRUB `\EFI\GRUB\GRUBX64.EFI`), ext4 720 GB, APFS 210 GB; `nvme0n1` (1.8 TB) ext4 `/mnt/data` | the picker gets a GRUB entry; see part 5 for which partition macOS goes on |
| Stick | SanDisk 3.2Gen1, 57.3 GB, `/dev/sda` (as of 2026-10-29: check it again) | everything on it is erased |

## 2. BIOS settings (MSI Click BIOS 5, `A.M2`; menu names vary a little between versions)

Press Del at power-on. Set these before the first boot of the stick:

| Setting (where) | Value | Why |
|---|---|---|
| Boot -> CSM / "Windows 10 WHQL Support" | CSM off, **UEFI only** | OpenCore is a UEFI loader |
| Boot -> Secure Boot | **Disabled** (Settings -> Security -> Secure Boot, "Secure Boot Mode"/"Image Execution Policy": Always Execute) | OpenCore is not signed for it |
| Boot -> Fast Boot | Disabled | USB and GPU must be initialised for the picker |
| Settings -> Advanced -> PCI Subsystem Settings -> **Above 4G memory / Crypto Mining** | **Enabled** | the card's 64-bit BARs |
| Settings -> Advanced -> PCI Subsystem Settings -> **Re-Size BAR Support** | **Disabled** (what every working boot had) | the BIOS then leaves BAR0 at 256 MiB, the EFI built here has `ResizeAppleGpuBars = -1`. With it **Enabled** BAR0 is 16 GiB and macOS takes a GPU BAR only up to 1 GiB (DRIVER-PLAN N62, [INSTALL-OPENCORE section 5](INSTALL-OPENCORE.md#5-devices-bios-opencore-uefi)): rebuild with `--resize-bars 10` (BAR0 cut to 1 GiB for macOS only, Linux keeps 16 GiB). It was **not** the cause of any boot failure |
| Settings -> Advanced -> USB Configuration -> **XHCI Hand-off** | **Enabled** | the stick keeps working after OpenCore starts (`ReleaseUsbOwnership` is on too) |
| Settings -> Advanced -> AMD CBS -> NBIO Common Options -> **IOMMU** | Auto or Disabled, either works | it was **not** the cause of the chipset failure (part 6); the config also sets `DisableIoMapper`. Linux does not need it |
| Settings -> Advanced -> Integrated Peripherals -> HD Audio | Enabled | onboard audio |
| Settings -> Advanced -> Wake Up Event Setup, ErP, Windows OS Configuration | defaults | - |
| Boot -> Boot Option Priorities | leave Linux's GRUB first; use **F11** (boot menu) to start the stick | the stick is a one-off until macOS is installed |

If the screen is black at the picker: primary display PCIe slot 1, the monitor
on the RX 7800 XT (the 5800X has no graphics of its own).

## 3. Build the stick (on this Linux PC)

```sh
tools/baremetal/fetch.sh                      # ~/rdna3-baremetal: pinned downloads + macOS recovery (~1 GB)
python3 -I tools/baremetal/build_efi.py --detect-linux   # ~/rdna3-baremetal/efi, checked with ocvalidate
sudo tools/baremetal/make_usb.sh /dev/sda     # the stick; asks you to type the device name
```

Re-run `build_efi.py` whenever there is a newer `RDNA3Accel-kext` (it reads
`~/Downloads/RDNA3Accel-kext.zip`, the artifact of the Forgejo `build.yml` job
`kext`; `--kext-zip` / `--kext-dir` for another place). The Mac's serial,
board serial, UUID and ROM are made once by OpenCore's `macserial` and kept
in `~/rdna3-baremetal/identity.json` (mode 600; never commit it).
`--detect-linux` reads `efibootmgr -v` and puts the loader paths (GRUB) into
`Misc/BlessOverride`; `--bless '\EFI\GRUB\GRUBX64.EFI'` does the same by hand.
`--debug` switches OpenCore's own log on (`Misc/Debug` Target 67: console and an
`opencore-*.txt` on the ESP, AppleDebug, ApplePanic; the boot-args already carry
`msgbuf=1048576`); it is off by default. `--resize-bars`, `--allow-ahci` and
`--entry` are in `--help`.

`make_usb.sh` shows `[1/7]` ... `[7/7]` and a percentage for the copy. It
refuses a disk that is not removable and on USB, a partition, a disk with a
system partition mounted, and anything smaller than 4 GiB, and wants the
device name typed. It erases the **whole** stick without reading it, makes a
GPT with one FAT32 partition `OPENCORE`, copies `EFI/` and
`com.apple.recovery.boot/` and reads the copy back. About 5 to 10 minutes on
USB 3. It also puts the helper scripts for macOS into `rdna3/` on the stick
(`tools/baremetal/stick/`: `install.sh`, `report.sh`, `metal.sh`; part 5) and
whatever is in `~/rdna3-baremetal/rdna3/` next to them. Put there, before
running it, from the CI artifacts: `RDNA3-Hackintosh-*.tar.gz` (artifact
`RDNA3-pkg`) and `metal/` with `RDNA3MTLDriver.bundle`,
`librdna3mtl_translate.dylib` (artifact `RDNA3MTL`), `libvulkan_radeon.dylib`
(artifact `radv-macos-logs`) and `rdna3mtl-allow.txt` (`metal/` in the repo).

What is on the stick (`~/rdna3-baremetal/efi/EFI/OC`):

| Part | Contents |
|---|---|
| Kexts | Lilu 1.7.2, **`RDNA3MCEReporterDisabler`** (generated, see part 6), VirtualSMC 1.3.8, RestrictEvents 1.1.6, AppleALC 1.9.8, RealtekRTL8111 3.0.0, NVMeFix 1.1.3, `RDNA3Accel.kext` (core kext of this driver) |
| ACPI | `SSDT-EC-USBX-DESKTOP.aml`, `SSDT-CPUR.aml` (Dortania, pinned) |
| Drivers | `OpenRuntime`, `OpenHfsPlus`, `ResetNvramEntry`; text picker, no OpenCanopy |
| `config.plist` | the **installer** configuration: `RDNA3Accel.kext` is listed but **disabled**, no `rdna3*` boot-args. The installer then runs on the card's GOP framebuffer, so a driver problem cannot stop the installation |
| `config-driver.plist` | the same with the core kext enabled and `amfi=0x80` (part 5). `rdna3agdc=3` is added later by `metal.sh` |

The versions and why each was chosen: `tools/baremetal/SOURCE`.

Common to both configs: Booter quirks for a Ryzen (`AvoidRuntimeDefrag`,
`EnableSafeModeSlide`, `ProvideCustomSlide`, `RebuildAppleMemoryMap`,
`SyncRuntimePermissions`; `EnableWriteUnprotector`, `DevirtualiseMmio` and
**`SetupVirtualMap` off**, Dortania's setting for 2020+ boards such as this
B550); `Kernel`: 27 AMD_Vanilla patches from the file the card VM runs (24
kernel ones, the four `cpuid_cores_per_package` with 8, **plus the three
IOPCIFamily ones**: the two "Disable 10 bit tags" enabled, the AM5
`IOPCIIsHotplugPort` one disabled), `ProvideCurrentCpuInfo`,
`DummyPowerManagement`, no CPUID spoof, `DisableIoMapper`, `XhciPortLimit`
off, `AppleAHCIPort` blocked (`Kernel/Block`; the owner's working stick has
it and this PC has no SATA disk; `--allow-ahci` leaves it out); `Misc`:
`SecureBootModel = Disabled`, `DmgLoading = Any` (the recovery dmg),
`ScanPolicy = 0` (every disk is listed), `ShowPicker`, `HideAuxiliary` off
(the installer dmg is an auxiliary entry), `PollAppleHotKeys` on (Shift+Enter,
Cmd+V...), `Timeout = 0` for the installer, GRUB through `BlessOverride`;
`csr-active-config = EF0F0000` (SIP relaxed, for the ad-hoc signed kexts,
INSTALL-OPENCORE section 4); SMBIOS `MacPro7,1` (what the card VM's
`play-desktop` step runs); boot-args `-v keepsyms=1 debug=0x100
msgbuf=1048576 alcid=13`.

## 4. Install macOS

1. Plug the stick into a **rear** USB port (the board's own ports, not a hub).
   Power on, **F11**, choose the stick's UEFI entry (`UEFI: SanDisk ...`, the
   one with a partition), not the plain one.
2. OpenCore's text picker lists `macOS Base System (External)` (the recovery
   from the stick; it is an auxiliary entry, shown because `HideAuxiliary` is
   off), `Reset NVRAM` and Linux's GRUB. Choose the recovery.
   Verbose boot text scrolls, then the macOS Utilities window appears
   (several minutes; the stick is slow). Ethernet works if RealtekRTL8111 loaded;
   the installer needs the network (it downloads macOS, about 15 GB).
3. **Disk Utility** first. View -> Show All Devices. Find the **partition**
   macOS is to go on (you replace the old Windows partition; this PC's
   second disk also showed a 210 GB APFS partition, so check size and name,
   do not trust this paragraph). Select **that partition, not the physical
   disk above it**, Erase, format **APFS**, name e.g. `macOS`. Erasing the
   physical disk would delete Linux and its boot partition.
   Do not touch the 1 GB EFI partition: Linux's GRUB (and the Windows boot
   files) live there.
4. Quit Disk Utility, **Install macOS Tahoe**, pick that volume. The Mac
   restarts several times; each time press F11 again, choose the stick, and in
   the picker pick the entry that says `Install macOS Tahoe` (or, at the end,
   `macOS`) on the new volume, **not** the recovery again. With
   `Timeout = 0` the picker waits for you every time.
5. At the first setup assistant, create the user, skip Apple ID, **Remote
   Login on** (System Settings -> Sharing), so a dark screen still allows ssh
   and a report.

If the installer or the first boot stops: try the picker's `Reset NVRAM`
once; the verbose text before the stop is what to look for (the usual ones: `[PCI Configuration
Begin]` or `EB|#LOG:EXITBS:START` = a Booter quirk, `AppleACPICPU` = SSDT-CPUR
or the core count, a panic mentioning `non-monotonic time` = the AMD_Vanilla
file does not match this macOS build).

## 5. Run the driver

Everything is decided by which `config.plist` OpenCore reads, and the helper
scripts on the stick (`<stick>/rdna3/`) do the macOS side. Messages in the
scripts are Polish. The order:

1. **`install.sh`, inside macOS.** `sudo sh /Volumes/OPENCORE/rdna3/install.sh`
   unpacks the release tarball (`RDNA3-Hackintosh-*.tar.gz` next to it) and
   runs its `install.sh`: the same preinstall / postinstall as the `.pkg`
   (framebuffer, accelerator, AGDC kexts; the accelerator's Tahoe build on
   Darwin 25; ad-hoc signatures; the AMD firmware into
   `/Library/Application Support/RDNA3/firmware`, which needs the network).
   The tarball is the way because it needs no Installer.app (the `.pkg`'s
   check now accepts macOS 15 and 26 as well). **Do not run `oc_install.sh`**
   on this EFI: it is for an EFI without our entry and its default kernel
   range is Darwin 24.
   **The EFI partition and the stick need `sudo` to mount in macOS**: the
   stick's `OPENCORE` partition is an EFI System partition, which macOS does not
   mount by itself. `diskutil list` shows it (`EFI OPENCORE`, e.g. `disk2s1`),
   then `sudo diskutil mount disk2s1` (without sudo it says "Unable to mount").
2. Restart; System Settings -> Privacy & Security -> **Allow** the blocked
   system software, restart again (INSTALL-OPENCORE section 4).
3. **Switch on the core kext.** Shut down, plug the stick into Linux, mount
   `OPENCORE`, and
   ```sh
   cd <stick>/EFI/OC && cp config.plist config-installer.plist.bak && cp config-driver.plist config.plist
   ```
   (the way back is the same with `config-installer.plist`). Boot from the stick
   again, pick `macOS`. First thing to look at: `ioreg -rc RDNA3Accel -d1 |
   grep -E 'Stage2|GFXResult|PMFeatures'` and the report (below).
4. **`metal.sh`, inside macOS**, once the driver boots: `sudo sh
   /Volumes/OPENCORE/rdna3/metal.sh` installs the Metal plugin, the translator
   and RADV from `rdna3/metal/` (the way `tools/vm/macos/build_metal.sh` does in
   the VM: `autoreport.sh --sync-metal`, root:wheel, `fix_tracedir.sh`) and sets
   `rdna3agdc=3` in the stick's `config.plist` with `plutil` (WindowServer on
   the plugin). It first copies the file to `config-before-metal.plist` (only
   the first time: that copy is the way back and a second run keeps it).
   Restart from the stick. If the screen goes dark: Linux,
   `cp config-before-metal.plist config.plist`.
5. Once macOS boots well from the stick, **put OpenCore on the disk**: from
   Linux mount the ESP (`/boot`), `cp -r <stick>/EFI/OC /boot/EFI/OC` (and
   `BOOT`'s `BOOTx64.efi` is not needed), then
   ```sh
   sudo efibootmgr -c -d /dev/nvme1n1 -p 1 -L OpenCore -l '\EFI\OC\OpenCore.efi'
   ```
   and make OpenCore first in `BootOrder` (`efibootmgr -o`) or pick it with
   F11. The 1 GB ESP has room (this EFI is about 8 MB). Keep GRUB's own
   files where they are. In `config.plist` set `Timeout` to 5 or so and use
   Ctrl+Enter in the picker to set the default OS (`AllowSetDefault` is on).

The driver's boot-args in `config-driver.plist` are the card VM's `apps` step
without what is VM-only (`serial=3`, `gemini_check`, the report switches
`rdna3probe` / `rdna3ladder`) and without `rdna3agdc=3` (that comes with
`metal.sh`): the driver's default profile (`common/rdna3_bootargs.h`) plus
`amfi=0x80` (INSTALL-OPENCORE section 4). If the screen goes dark after the
driver comes up: Linux, `cp config-installer.plist.bak config.plist` (or
`-rdna3off` in the boot-args), see
[INSTALL-OPENCORE section 8](INSTALL-OPENCORE.md#8-the-way-back).

### What to check first, and the report

Inside macOS: `sudo sh /Volumes/OPENCORE/rdna3/report.sh` writes
`rdna3/report-<date>/` on the stick: the kernel and RDNA3/WindowServer logs of
the last 3 hours and of this boot, `ioreg`, `kmutil showloaded`, the displays,
`sysctl` rdna3, `boot-args`, the `*.ips` and WindowServer crash reports. It does
**not** run `autoreport.sh --standalone` (its GPU probes crashed WindowServer on
this Mac, 2026-10-31); `sudo RDNA3_FULL=1 sh report.sh` does. Send the folder
like the VM's reports (`docs/AFTER-TESTS.md`). The lines that decide the first
questions (kernel log: `log show --last boot --predicate 'eventMessage contains
"RDNA3"'` or the verbose boot):

| Look for | Meaning |
|---|---|
| `VRAM aperture: BAR0 ... MiB, mapped ... MiB` | BAR0 reached the kernel; `NOT MAPPED: BAR0 is bigger than 1 GiB` = Re-Size BAR is on and the config has `ResizeAppleGpuBars = -1` (rebuild with `--resize-bars 10`) |
| the bring-up levels up to 8, `GFXResult` in ioreg | the card initialised on a **real** PCIe slot (the VM never ran it) |
| a panic or hang after `RDNA3Accel` | this is the bare-metal difference: photograph the verbose text (no serial port here) |
| `RDNA3,Head<n>,...` in ioreg, the picture on both outputs | display side up |
| About This Mac: "AMD Ryzen 7 5800X 8-Core Processor" | RestrictEvents + AMD_Vanilla core count work |

Dual boot notes:

* **Linux/GRUB.** `--detect-linux` (or `--bless`) puts `\EFI\GRUB\GRUBX64.EFI`
  into `Misc/BlessOverride`, and OpenCore lists the `GRUB` entry of the ESP
  by itself. A `Misc/Entries` entry with the short device path
  `HD(1,GPT,<guid>,0x800,0x200000)/\EFI\GRUB\GRUBX64.EFI` made OpenCore say
  `LoadImage failed - Not Found` (2026-10-30), which is why the generator
  no longer writes one. If GRUB is still missing, use F11 and the firmware's
  own `GRUB` entry.
* macOS writes nothing into the ESP by itself except what OpenCore/`bless`
  would; GRUB keeps working. Do not let macOS's Startup Disk change the
  firmware boot order if you want GRUB first.
* Never mount macOS's APFS volume read-write from Linux, and do not put
  macOS's volume on a partition Linux uses.
* With Re-Size BAR on (`--resize-bars 10`) the RX 7800 XT under Linux keeps
  its 16 GiB BAR (the quirk reduces it for macOS only), so amdgpu is
  unaffected.

## 6. What was learned, and what is not certain

Learned 2026-10-29..31 (the first bare-metal days), now built in:

| What happened | Cause and fix |
|---|---|
| USB, LAN, the 1 TB NVMe and SATA were dead or the boot stopped early; IOMMU off and Re-Size BAR off changed nothing | **Root cause: the chipset.** Everything behind the B550 chipset (USB, LAN, the 1 TB NVMe, SATA) only works with AMD_Vanilla's `IOPCIFamily` "Disable 10 bit tags" patch (probeBusGated; macOS otherwise enables PCIe 10-bit tags the chipset does not handle). `vmtool.amd_patches()` used to keep only the `kernel` patches (its comment wrongly called the IOPCIFamily ones Zen 4-only); `build_efi.py` now adds both variants (12.0-15.x and 26.0+, enabled) and the AM5 `IOPCIIsHotplugPort` one disabled. The VM keeps the kernel-only list (its chipset is QEMU's) |
| Panic in `AppleIntelMCEReporter` | MacPro7,1 makes macOS load the Intel machine-check reporter, which panics on AMD. `RDNA3MCEReporterDisabler.kext` (generated by `build_efi.py`, only an `Info.plist`, listed right after Lilu) has two personalities with probe score 5000 and the reporter's bundle identifier, matching `IOPlatformExpertDevice` (board-id `Mac-27AD2F918AE68F61`) and `AppleIntelMCEInterruptNub` with a plain `IOService`, so the real driver never matches |
| Booter and console settings | `SetupVirtualMap` is **off** (Dortania: boards from 2020 on, such as this B550). The `npci` boot-arg is **not needed**, and `npci=0x2000` / `0x3000` kill the console when the GPU BAR is above 4 GiB |
| The picker showed the stick only | the recovery dmg is an auxiliary entry: `HideAuxiliary` off; `DmgLoading = Any` |
| OpenCore: `LoadImage failed - Not Found` for GRUB | a short `HD(...)` device path in `Misc/Entries`; `Misc/BlessOverride` `\EFI\GRUB\GRUBX64.EFI` instead |
| The `.pkg` refused macOS 26 | its installation check said 15.0 up to 16.0 although the payload has the Tahoe accelerator: now 15.0 up to 27.0; `install.sh` on the stick uses the tarball anyway |
| `autoreport.sh --standalone` crashed WindowServer | `report.sh` does not run it unless `RDNA3_FULL=1` |

Open, or a guess:

| Setting | How it can fail | What to try |
|---|---|---|
| Safe Mode (Shift+Enter in the picker, `PollAppleHotKeys` is on) | shows the prohibited sign (**open**; the driver already stands down in `-x`, commit `7821cae`, the cause is not found) | not investigated yet; the normal boot works |
| `alcid=13` | no sound (or the wrong jack) | `alcid=69`, then `31`, `97` (ALC897 layouts in AppleALC 1.9.8: 11 12 13 21 22 23 31 66 69 77 97 98 99); sound is not needed for the driver |
| No USB map | a port missing in the installer | none expected (14 and 8 ports < 15). If a keyboard is dead in the picker, use another rear port; map ports later with USBToolBox (Windows/Linux tool) only if needed |
| `DevirtualiseMmio` | a MMIO panic | try it on (needs `MmioWhitelist` on some boards) |
| `ResizeAppleGpuBars = -1` | with Re-Size BAR on in the BIOS the BAR stays 16 GiB: `NOT MAPPED: BAR0 is bigger than 1 GiB` | `--resize-bars 10` (ocvalidate warns; `build_efi.py` accepts that note, OpenCore's own text calls values above 8 unstable across sleep) or `8` (VM-proven) |
| `AppleAHCIPort` blocked | no SATA disk in macOS (none is attached) | `build_efi.py --allow-ahci`. The block came from the first chipset test, before the 10-bit tags patch; whether SATA works without it is untested |
| `DisableIoMapper` | none expected | AMD boards have no DMAR; it is only a guard |
| Core kext range `25.0.0..25.99.99` | kext not injected on a later Darwin | change `CORE_KERNEL_MAX` in `tools/baremetal/build_efi.py` |
| AMD_Vanilla file `a174cca` | `non-monotonic time` panic / hang on a macOS newer than 26.7.2 | a newer file (`tools/vm/data/amd_vanilla.SOURCE`) |
| MCE kext with another SMBIOS | `build_efi.py --smbios X` refuses a model whose board-id is not in `MCE_BOARD_IDS` | add the model's board-id there |

Not included on purpose: WhateverGreen (it breaks this driver), SMCProcessor /
SMCSuperIO (Intel sensors), a CPU power-management kext for Ryzen
(`SMCAMDProcessor`, unmaintained; temperatures only), Bluetooth or Wi-Fi
kexts (this PC has none checked).
