# macOS 26 on the real PC: OpenCore installer USB, dual boot with Linux

How to install macOS 26 Tahoe on the owner's PC (MSI B550-A PRO, Ryzen 7 5800X,
RX 7800 XT) from a USB stick, next to Linux, and then run this driver on it.
The driver side (kexts, boot-args, the way back) is
[INSTALL-OPENCORE](INSTALL-OPENCORE.md); this page is the machine, the stick
and the order of things. The AMD CPU side follows Dortania's AMD (Zen) guide.

> **Status:** nothing here has run on this PC yet. The OpenCore settings for
> the driver are what the card VM runs (macOS 26.4.1, AMD_Vanilla patches,
> `RDNA3Accel.kext` injected by OpenCore); the Ryzen and B550 settings are the
> guide's. Part 6 lists what is a guess.

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
| Boot -> CSM / "Windows 10 WHQL Support" | CSM off, **UEFI only** | OpenCore is a UEFI loader; Resizable BAR also needs it |
| Boot -> Secure Boot | **Disabled** (Settings -> Security -> Secure Boot, "Secure Boot Mode"/"Image Execution Policy": Always Execute) | OpenCore is not signed for it |
| Boot -> Fast Boot | Disabled | USB and GPU must be initialised for the picker |
| Settings -> Advanced -> PCI Subsystem Settings -> **Above 4G memory / Crypto Mining** | **Enabled** | the card's 64-bit BARs |
| Settings -> Advanced -> PCI Subsystem Settings -> **Re-Size BAR Support** | **Enabled** (recommended) or Disabled | macOS takes a GPU BAR only up to 1 GiB (DRIVER-PLAN N62, [INSTALL-OPENCORE section 5](INSTALL-OPENCORE.md#5-devices-bios-opencore-uefi)). Enabled: BAR0 is 16 GiB, the EFI built here has `ResizeAppleGpuBars = 10` (1 GiB for macOS only, Linux keeps 16 GiB). Disabled: BAR0 is 256 MiB; rebuild with `--resize-bars -1` (or leave 10, it only reduces) |
| Settings -> Advanced -> USB Configuration -> **XHCI Hand-off** | **Enabled** | the stick keeps working after OpenCore starts (`ReleaseUsbOwnership` is on too) |
| Settings -> Advanced -> AMD CBS -> NBIO Common Options -> **IOMMU** | **Disabled** (or Auto) | macOS knows no AMD-Vi; the config also sets `DisableIoMapper`. Turn it off first if the boot stops early. Linux does not need it |
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

Already done on 2026-10-29: `fetch.sh` and `build_efi.py` ran, the recovery is
macOS 26.6.2 (25G83), and only `make_usb.sh` is left. Re-run `build_efi.py`
whenever there is a newer `RDNA3Accel-kext` (it reads
`~/Downloads/RDNA3Accel-kext.zip`, the artifact of the Forgejo `build.yml` job
`kext`; `--kext-zip` / `--kext-dir` for another place). The Mac's serial,
board serial, UUID and ROM are made once by OpenCore's `macserial` and kept
in `~/rdna3-baremetal/identity.json` (mode 600; never commit it).

`make_usb.sh` shows `[1/7]` ... `[7/7]` and a percentage for the copy. It
refuses a disk that is not removable and on USB, a partition, a disk with a
system partition mounted, and anything smaller than 4 GiB, and wants the
device name typed. It erases the **whole** stick without reading it, makes a
GPT with one FAT32 partition `OPENCORE`, copies `EFI/` and
`com.apple.recovery.boot/` and reads the copy back. About 5 to 10 minutes on
USB 3.

What is on the stick (`~/rdna3-baremetal/efi/EFI/OC`):

| Part | Contents |
|---|---|
| Kexts | Lilu 1.7.2, VirtualSMC 1.3.8, RestrictEvents 1.1.6, AppleALC 1.9.8, RealtekRTL8111 3.0.0, NVMeFix 1.1.3, `RDNA3Accel.kext` (core kext of this driver) |
| ACPI | `SSDT-EC-USBX-DESKTOP.aml`, `SSDT-CPUR.aml` (Dortania, pinned) |
| Drivers | `OpenRuntime`, `OpenHfsPlus`, `ResetNvramEntry`; text picker, no OpenCanopy |
| `config.plist` | the **installer** configuration: `RDNA3Accel.kext` is listed but **disabled**, no `rdna3*` boot-args. The installer then runs on the card's GOP framebuffer, so a driver problem cannot stop the installation |
| `config-driver.plist` | the same with the core kext enabled and `amfi=0x80 rdna3agdc=3` (part 5) |

The versions and why each was chosen: `tools/baremetal/SOURCE`.

Common to both configs: Booter quirks for a Ryzen (`AvoidRuntimeDefrag`,
`EnableSafeModeSlide`, `ProvideCustomSlide`, `RebuildAppleMemoryMap`,
`SetupVirtualMap`, `SyncRuntimePermissions`; `EnableWriteUnprotector` and
`DevirtualiseMmio` off); `Kernel`: 24 AMD_Vanilla patches (the four
`cpuid_cores_per_package` ones with 8) from the file the card VM runs,
`ProvideCurrentCpuInfo`, `DummyPowerManagement`, no CPUID spoof,
`DisableIoMapper`, `XhciPortLimit` off; `Misc`: `SecureBootModel = Disabled`,
`ScanPolicy = 0` (every disk is listed), `ShowPicker`, `Timeout = 0` for the
installer, GRUB as a picker entry; `csr-active-config = EF0F0000` (SIP
relaxed, for the ad-hoc signed kexts, INSTALL-OPENCORE section 4);
SMBIOS `MacPro7,1` (what the card VM's `play-desktop` step runs); boot-args
`-v keepsyms=1 debug=0x100 msgbuf=1048576 alcid=13`.

## 4. Install macOS

1. Plug the stick into a **rear** USB port (the board's own ports, not a hub).
   Power on, **F11**, choose the stick's UEFI entry (`UEFI: SanDisk ...`, the
   one with a partition), not the plain one.
2. OpenCore's text picker lists `macOS Base System (External)` (the recovery
   from the stick) and Linux's GRUB (Space shows the auxiliary entries, among
   them `Reset NVRAM`). Choose the recovery.
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
(Space shows it) once; the verbose text before the stop is what to look for (the usual ones: `[PCI Configuration
Begin]` or `EB|#LOG:EXITBS:START` = a Booter quirk, `AppleACPICPU` = SSDT-CPUR
or the core count, a panic mentioning `non-monotonic time` = the AMD_Vanilla
file does not match this macOS build).

## 5. Run the driver

Everything is decided by which `config.plist` OpenCore reads.

1. **Install the rest of the driver inside macOS.** The EFI already holds the
   core kext; the framebuffer, accelerator and AGDC kexts and the Metal plugin
   come from the package (CI `build.yml` artifact `RDNA3-pkg`; the
   postinstall script chooses the accelerator's Tahoe build on Darwin 25),
   see [INSTALL-OPENCORE section 1](INSTALL-OPENCORE.md#1-get-the-installer):
   `sudo installer -pkg RDNA3-Hackintosh-*.pkg -target /` (or `install.sh`).
   It downloads the AMD firmware into `/Library/Application Support/RDNA3/firmware`
   (needs the network). **Do not run `oc_install.sh`** on this EFI: it is for
   an EFI without our entry and its default kernel range is Darwin 24.
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
4. Once macOS boots well from the stick, **put OpenCore on the disk**: from
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
`rdna3probe` / `rdna3ladder`): the driver's default profile
(`common/rdna3_bootargs.h`) plus `rdna3agdc=3` (WindowServer on the Metal
plugin) and `amfi=0x80` (INSTALL-OPENCORE section 4). If the screen goes dark
after the driver comes up: Linux, `cp config-installer.plist.bak config.plist`
(or `-rdna3off` in the boot-args), see
[INSTALL-OPENCORE section 8](INSTALL-OPENCORE.md#8-the-way-back).

### What to check first, and the report

Over ssh or on the desktop:

```sh
sudo "/Library/Application Support/RDNA3/autoreport.sh" --standalone
```

The files land in `/Users/Shared/rdna3-reports/`; send them like the VM's
(`docs/AFTER-TESTS.md`). Before that, the lines that decide the first
questions (kernel log: `log show --last boot --predicate 'eventMessage contains "RDNA3"'`
or the verbose boot):

| Look for | Meaning |
|---|---|
| `VRAM aperture: BAR0 ... MiB, mapped ... MiB` | BAR0 reached the kernel; `NOT MAPPED: BAR0 is bigger than 1 GiB` = `ResizeAppleGpuBars` did not apply (Resizable BAR on and the quirk missing) |
| the bring-up levels up to 8, `GFXResult` in ioreg | the card initialised on a **real** PCIe slot (the VM never ran it) |
| a panic or hang after `RDNA3Accel` | this is the bare-metal difference: photograph the verbose text (no serial port here) |
| `RDNA3,Head<n>,...` in ioreg, the picture on both outputs | display side up |
| About This Mac: "AMD Ryzen 7 5800X 8-Core Processor" | RestrictEvents + AMD_Vanilla core count work |

Dual boot notes:

* **Linux/GRUB.** The EFI built with `--detect-linux` has a picker entry
  `GRUB` pointing at `HD(1,GPT,a46e195a-...,0x800,0x200000)/\EFI\GRUB\GRUBX64.EFI`
  (the PARTUUID of the ESP, read by `efibootmgr -v` on 2026-10-29). If OpenCore
  cannot start it, use F11 and the firmware's own `GRUB` entry. Rebuild with
  `--entry 'Name=<device path>'` after the ESP changes.
* macOS writes nothing into the ESP by itself except what OpenCore/`bless`
  would; GRUB keeps working. Do not let macOS's Startup Disk change the
  firmware boot order if you want GRUB first.
* Never mount macOS's APFS volume read-write from Linux, and do not put
  macOS's volume on a partition Linux uses.
* The RX 7800 XT under Linux keeps its 16 GiB BAR (the quirk reduces it for
  macOS only), so amdgpu is unaffected.

## 6. What is not certain

| Setting | How it can fail | What to try |
|---|---|---|
| `alcid=13` | no sound (or the wrong jack) | `alcid=69`, then `31`, `97` (ALC897 layouts in AppleALC 1.9.8: 11 12 13 21 22 23 31 66 69 77 97 98 99); sound is not needed for the driver |
| No USB map | a port missing in the installer | none expected (14 and 8 ports < 15). If a keyboard is dead in the picker, use another rear port; map ports later with USBToolBox (Windows/Linux tool) only if needed |
| `SetupVirtualMap`, `DevirtualiseMmio` | hang at `EB|#LOG:EXITBS` or a MMIO panic | toggle `SetupVirtualMap`; try `DevirtualiseMmio` on (needs `MmioWhitelist` on some boards) |
| `ResizeAppleGpuBars = 10` | ocvalidate warns about it (accepted by `build_efi.py`; OpenCore's own text calls values above 8 unstable across sleep) | `--resize-bars 8` (VM-proven) or Resizable BAR off + `-1` |
| `DisableIoMapper` | none expected | AMD boards have no DMAR; it is only a guard |
| GRUB picker entry as a device path | OpenCore cannot load it | F11 and the firmware's GRUB entry |
| Core kext range `25.0.0..25.99.99` | kext not injected on a later Darwin | change `CORE_KERNEL_MAX` in `tools/baremetal/build_efi.py` |
| AMD_Vanilla file `a174cca` | `non-monotonic time` panic / hang on a macOS newer than 26.7.1 | a newer file (`tools/vm/data/amd_vanilla.SOURCE`) |

Not included on purpose: WhateverGreen (it breaks this driver), SMCProcessor /
SMCSuperIO (Intel sensors), a CPU power-management kext for Ryzen
(`SMCAMDProcessor`, unmaintained; temperatures only), Bluetooth or Wi-Fi
kexts (this PC has none checked).
