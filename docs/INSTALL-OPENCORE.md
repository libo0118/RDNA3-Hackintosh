# Installing on a bare-metal Hackintosh (OpenCore)

How to install the driver for real use on a Hackintosh with an
**RX 7800 XT (Navi 32, `1002:747E`)** and OpenCore, without the VM test
harness. The procedure for testing it (reports, ladder steps) is the last
section of [TESTING-HW](TESTING-HW.md#bare-metal-with-the-installer-no-vm-harness).

> **Status:** everything here follows the configuration that works in the
> test VM (macOS 15 with the card passed through, `tools/vm/rdna3vm`). A
> bare-metal machine has not run it yet. Prepare the way back (section 8)
> **before** the first boot.

The machine, BIOS and installer stick for the owner's own PC (macOS 26,
dual boot with Linux): [BAREMETAL](BAREMETAL.md).

Requirements: macOS **15 Sequoia** (x86_64), OpenCore, the RX 7800 XT,
SSH or a second machine to read a report if the screen stays dark.

## 1. Get the installer

**From CI:** the `build` workflow's artifact **`RDNA3-pkg`** holds
`RDNA3-Hackintosh-<version>-<commit>.pkg` and the same files as a
`.tar.gz` with `install.sh`. It contains the three kexts and the report
tools, **not** the Metal plugin or RADV (other CI jobs build those).

**Built yourself** (a Mac with the Xcode command line tools):

```sh
make kext && make -C fb && make -C accel && make -C userspace && make -C userspace probes
package/make_pkg.sh                       # -> build/pkg/RDNA3-Hackintosh-*.pkg + .tar.gz
# with the Metal plugin and RADV from CI artifacts (zips or unpacked):
package/make_pkg.sh --from build --from RDNA3MTL.zip --from radv-macos-logs.zip
```

**Firmware.** The AMD microcode is not part of the package
([FIRMWARE](FIRMWARE.md)). The installer downloads the 12 files the kext
loads from linux-firmware (tag pinned in `package/firmware.sh`, SHA-256 and
header checked) into `/Library/Application Support/RDNA3/firmware`, where
the kext reads them, together with `LICENSE.amdgpu`. Without the firmware the
installer installs nothing. Offline: unpack the `.tar.gz` and run
`sudo sh firmware.sh --from <linux-firmware>/amdgpu` first (or
`sudo ./install.sh --fw <linux-firmware>/amdgpu`), or build your own package
with `package/make_pkg.sh --fw <linux-firmware>/amdgpu` (the firmware
license allows that with `LICENSE.amdgpu`; keep such a package to yourself).

**Install:** double-click the `.pkg` (unsigned: right-click → Open, or
`sudo installer -pkg RDNA3-Hackintosh-*.pkg -target /`), or
`sudo ./install.sh` in the unpacked `.tar.gz`. Both run the same
preinstall/postinstall scripts.

## 2. What gets installed

| Path | What |
|---|---|
| `/Library/Extensions/RDNA3Accel.kext` | core: PCI, bring-up, display engine, amdgpu UAPI (`IOPCIFamily` only) |
| `/Library/Extensions/RDNA3Framebuffer.kext` | `IOFramebuffer` per display head (links `IOGraphicsFamily`) |
| `/Library/Extensions/RDNA3Accelerator.kext` | `IOAccelerator` for WindowServer and Metal (links `IOAcceleratorFamily2`, `IOGraphicsFamily`, `IOSurface`) |
| `/Library/GPUBundles/rdna3mtl/` | `libdrm.2.dylib`, `libdrm_amdgpu.1.dylib`, `amdgpu.ids`; RADV `libvulkan_radeon.dylib` + `radeon_icd.x86_64.json`, the AIR translator (when the package has them) |
| `/Library/GPUBundles/RDNA3MTLDriver.bundle`, `rdna3mtl-allow.txt` | Metal plugin (when the package has it) |
| `/Library/Application Support/RDNA3/firmware/` | AMD firmware + `LICENSE.amdgpu` (downloaded at install) |
| `/Library/Application Support/RDNA3/OpenCore/` | the core kext's copy for `EFI/OC/Kexts` and `Kernel-Add.plist` (the entries below) |
| `/Library/Application Support/RDNA3/` | `oc_install.sh`, `autoreport.sh`, `tools/macos/collect.sh`, `bin/` (test tools), `firmware.sh`, `uninstall.sh`, `VERSION`, licenses |

The postinstall script ad-hoc signs the kexts (kernelmanagerd refuses
unsigned ones), sets `root:wheel`, and hands them to kernelmanagerd
(`kmutil load -p`), which asks for approval and rebuilds the Auxiliary
Kernel Collection for the next boot. It prints warnings into
`/var/log/install.log` (SIP on, `amfi=0x80` missing, a conflicting kext loaded).

## 3. Kernel → Add

Which kext loads from where (their `OSBundleLibraries`):

| Kext | Links | Loaded by |
|---|---|---|
| `RDNA3Accel` | `IOPCIFamily`, KPIs | **OpenCore** (recommended) or the Auxiliary KC |
| `RDNA3Framebuffer` | `IOGraphicsFamily`, `IOPCIFamily` | the Auxiliary KC (`/Library/Extensions`) only |
| `RDNA3Accelerator` | `IOAcceleratorFamily2`, `IOGraphicsFamily`, `IOSurface` | the Auxiliary KC only |

`IOGraphicsFamily` and `IOAcceleratorFamily2` exist only in Apple's System
kernel collection, so OpenCore cannot link the framebuffer or the
accelerator ([STAGE1](STAGE1.md#loading)). None of the three links another;
the framebuffer and the accelerator wait for the `RDNA3Accel` service at
start, so the core loading first (from OpenCore) is the natural order. No
Lilu dependency: the entry can go anywhere in `Kernel/Add`.

The tested configuration (the VM) injects the core with OpenCore and loads
the other two from `/Library/Extensions`. The `.pkg` installs all three
there; with the OpenCore entry the injected core wins and its copy only
logs "a prelinked copy … is already present". Without the entry all three
load from the Auxiliary KC, which should work with the settings of section
4 but has not been tried.

The easy way (copies the kext, adds the entry, backs up `config.plist`;
needs `python3`, i.e. the Xcode command line tools: `xcode-select --install`):

```sh
sudo diskutil mount disk0s1          # your EFI partition: diskutil list
sudo "/Library/Application Support/RDNA3/oc_install.sh" --efi /Volumes/EFI
```

By hand: copy `/Library/Application Support/RDNA3/OpenCore/RDNA3Accel.kext`
to `EFI/OC/Kexts/` and paste this into `Kernel → Add` (generated from the
kexts' `Info.plist` by `package/oc_snippet.py --kexts build`; the installed
`OpenCore/Kernel-Add.plist` holds the same, plus the other two disabled):

```xml
			<dict>
				<key>Arch</key>
				<string>x86_64</string>
				<key>BundlePath</key>
				<string>RDNA3Accel.kext</string>
				<key>Comment</key>
				<string>RDNA3-Hackintosh io.github.voten641.RDNA3Accel 0.1.0</string>
				<key>Enabled</key>
				<true/>
				<key>ExecutablePath</key>
				<string>Contents/MacOS/RDNA3Accel</string>
				<key>MaxKernel</key>
				<string>24.99.99</string>
				<key>MinKernel</key>
				<string>24.0.0</string>
				<key>PlistPath</key>
				<string>Contents/Info.plist</string>
			</dict>
```

**MinKernel / MaxKernel:** `24.0.0` – `24.99.99` = macOS 15 (Darwin 24).
CI builds the kexts with the self-hosted macOS runner's SDK and
`-mmacosx-version-min=11.0`, but only Sequoia has run them, the
accelerator's class layouts are per release (macOS 26 Tahoe needs the
`make -C accel NM_TAHOE=1` build, which the package does not ship), and the
userspace tools need 15.0. The `.pkg` refuses other macOS versions, and
`RDNA3Accelerator` itself refuses to start on a Darwin release other than
the one its build was verified on (24, or 25 for `NM_TAHOE=1`) unless
`rdna3accelanyos=1` is set.

**After every update** run `oc_install.sh` again: the EFI copy of the core
must be the same build as the framebuffer in `/Library/Extensions`
(a report says `framebuffer older than the core` otherwise).

## 4. Security settings (unsigned kexts)

The kexts are ad-hoc signed, not by a Developer ID. What the VM runs with:

| Setting | Value | Why |
|---|---|---|
| `NVRAM → Add → 7C436110-… → csr-active-config` | `EF0F0000` (SIP off, 0x0FEF) | ad-hoc signed kexts in the Auxiliary KC; NullMoth reports `430A0000` (0x0A43) working, untested here |
| `NVRAM → Delete → 7C436110-…` | lists `csr-active-config` and `boot-args` | otherwise OpenCore does not overwrite the stored values |
| `Misc → Security → SecureBootModel` | `Disabled` | the tested setting |
| boot-arg `amfi=0x80` | set | without it kernelmanagerd called the ad-hoc framebuffer an "invalid codeful kernel extension" and kept an old copy |

Then, once per new build: System Settings → Privacy & Security → **Allow**
next to the blocked system software, and restart. Until then the
framebuffer and the accelerator do not load (the core from OpenCore does).

The Metal plugin decides per process whether it hands out its Metal device
(`/Library/GPUBundles/rdna3mtl-allow.txt`, see "Which apps get the GPU"
below). NullMoth also uses `amfi_get_out_of_my_way=0x1` so WindowServer accepts an unsigned
bundle; not needed for the default profile, and it weakens the system's
code-signing checks further.

## 5. Devices, BIOS, OpenCore UEFI

* **DeviceProperties:** none needed. Remove any `device-id` spoof for the
  card: the kexts match `IOPCIPrimaryMatch 0x747e1002` against the ids in
  the IORegistry, so a spoofed id hides the card from them.
* **BIOS:** CSM off, Above 4G Decoding on, Resizable BAR on or off (see
  below). The GPU always uses all of its VRAM; the BAR0 window decides only
  how much of it the CPU can reach directly (the CPU-visible heap: buffers
  macOS maps, uploads, the framebuffer). **macOS takes no PCI BAR bigger than
  1 GiB** (Apple's IOPCIFamily: `MAX_BAR_SIZE`; a 16 GiB BAR0 is cut to
  nothing and the card has no VRAM aperture at all, DRIVER-PLAN N62). With
  Resizable BAR on (BAR0 = 16 GiB on the RX 7800 XT) set
  `Booter → Quirks → ResizeAppleGpuBars = 10` (1 GiB, the most macOS takes;
  OpenCore resizes BAR0 for macOS only, Windows/Linux keep 16 GiB) or `8`
  (256 MiB, what the VM proved; OpenCore's own docs call only 0 fully
  supported and warn that values above 8 may be unstable across sleep).
  With Resizable BAR off BAR0 is 256 MiB and the quirk can stay `-1`. Do not
  use `UEFI → Quirks → ResizeGpuBars` for this (OpenCore: it is not for the
  macOS limit). The kext maps up to 1 GiB of BAR0 into the kernel
  (`rdna3vramwin=<MiB>`); what it got is in the IORegistry
  (`RDNA3,VRAMBar0MB`, `RDNA3,VRAMWindowMB`, `RDNA3,VRAMVisibleHeapMB`,
  `RDNA3,VRAMApertureMB`) and in the boot log (`VRAM aperture: BAR0 ... MiB,
  mapped ... MiB, visible heap ... MiB`, or `VRAM aperture: NOT MAPPED: ...`
  with the reason: `BAR0 is bigger than 1 GiB` means the quirk is missing).
* **`UEFI → Output → Resolution`:** `Max` (what the VM uses). The
  framebuffer starts on the mode the GOP left on head 0, so this is the
  boot resolution of the desktop.
* **Do not block `IONDRVSupport`** (`Kernel → Block`): it drives the GOP
  picture when our framebuffer declines (`-rdna3off`, a failed start).

## 6. What to remove

* **WhateverGreen:** it has no RDNA3 support and was never run together with
  these kexts; with the RX 7800 XT as the only GPU remove it. At the very
  least drop `-radvesa`, `-wegnoegpu` (hides the card) and `agdpmod=`.
* **Other kexts that touch `1002:747E`:** RadeonSensor / SMCRadeonGPU
  (read the SMU behind our back), fake-id attempts with Apple's
  `AMDRadeonX6000*` (a `device-id` spoof in `DeviceProperties`), NootRX or
  forks of it. Only one driver may own the card.
* **Earlier copies** from `package/install.sh` or the VM's `setup.sh`: the
  `.pkg` replaces them (same paths).

## 7. Boot-args

**None are needed.** Without `rdna3lvl` the kexts run their default
profile (`common/rdna3_bootargs.h`, the configuration verified on the
card): every bring-up level, MSI, DMCUB, the framebuffer with the
accelerator's display pipe, the second monitor as an extended desktop,
PMFW DPM, the idle GFXCLK cap, display sleep and the scaled mode list.
Useful but optional: `-v keepsyms=1` (readable boot log and panics).

The profile's values (a boot-arg always wins; `rdna3profile=0` drops them
all, `rdna3profile=1` adds them to an explicit `rdna3lvl`):
`rdna3lvl=8 rdna3msi=1 rdna3dmub=1 rdna3fb=1 rdna3mirror=2 rdna3accel=1
rdna3accelfb=4 rdna3acceliop=1 rdna3pm=2 rdna3idlecap=1 rdna3dpms=1 rdna3modes=1`.
The output gamma (Night Shift, calibration, the ColorSync curve macOS loads
at login) is on outside the profile too, at every `rdna3lvl` (card run 56);
`rdna3gamma=0` turns it off.

Opt-in features and switches (step names from `tools/ladder-steps.txt`,
what each step checks: [TESTING-HW](TESTING-HW.md#the-ladder)):

| Boot-arg | What |
|---|---|
| `-rdna3off` | everything off (all three kexts decline): the emergency switch |
| `rdna3lvl=0`…`8` | a test-ladder level instead of the profile (`oc_install.sh --step N` sets a whole step) |
| `rdna3modeset=1` | own display timings: the monitor's EDID modes through `hw/dcn32_modeset.c` (step `modeset`) |
| `rdna3vrr=1` | FreeSync / VRR with `rdna3modeset=1` (`hw/dcn32_freesync.c`): a mode inside the sink's range runs with DRR (v_total from the max to the min refresh, flips end the stretched blank), MSA timing ignored on DP and the FreeSync SPD sent; needs `rdna3dplink=1` (DP) or `rdna3hdmi=1` (HDMI, the AMD VSDB read through the DMCUB); off with `rdna3subvp=1` (SubVP assumes a fixed timing). The range is in ioreg `RDNA3,Head<n>,VRRRange` either way, the state in `RDNA3,Head<n>,VRR` |
| `rdna3txwatch=0` | no TX watch: by default the core logs what each head sends after its OTG (DIG, HDMI packets, PHY clock selects; `hw/dcn32_txstate.c`) at the start, after the clone and whenever it changes (every 2 s, reads only), and publishes `RDNA3,Head<n>,TX`, `RDNA3,CloneTX` and `RDNA3,TXWatchTicks` |
| `rdna3mes=1` | MES scheduler pipe (step 8c) |
| `rdna3userq=1` | user queues (DRM_AMDGPU_USERQ, amdgpu's `user_queue=1`) through the MES; turns `rdna3mes` on, kernel VMs keep VMIDs 1-7 (step userq) |
| `rdna3vcn=1` | the video engines: VCN 4.0 (two instances) and JPEG 4.0 for VCN_ENC / VCN_JPEG command submission and VIDEO_CAPS; the PSP also loads `vcn_4_0_0.bin` for VCN1 (step vcn) |
| `rdna3gfxoff=1` | GFXOFF: the GFX block powers off when idle (step 8o) |
| `rdna3pmctl=1` | power management writes from userspace (`rdna3_pmctl`, as root): fan mode / speed, power limit, profiles, clock levels, DF C-states; `=2` also overdrive (amdgpu's `ppfeaturemask` overdrive bit: GFXCLK / UCLK limits, voltage offset, fan curve, zero RPM, power limit above the default range) (step pmctl). Reads work without it |
| `rdna3baco=1` | with `rdna3pmctl`: `rdna3_pmctl baco-cycle` takes the chip through BACO and brings it up again (step baco); no sleep path uses BACO |
| `rdna3socg=1` | HDP/ATHUB clock gating (`hw/soc21_cg.c`) |
| `rdna3cg=0` | GFX clock gating off (on by default since card run 8) |
| `rdna3gamma=0` | macOS's gamma tables (Night Shift, calibration, ColorSync) no longer reach the display's output gamma; the screen keeps the linear curve (on by default since card run 56) |
| `rdna3sleep=1` | lift the idle-sleep block (no suspend/resume yet: the picture is gone after a sleep) |
| `rdna3mirror=1` | the second monitor mirrors the first instead of extending the desktop (step 9b) |
| `rdna3dmub=3` | attach to the DMCUB the GOP left running instead of restarting it (step 5c) |
| `rdna3dpms=2` | display sleep also takes the link down |
| `rdna3accelfb=3` | accelerator display pipe without the aperture gate (step 10s) |
| `rdna3pstate=0`…`3` | p-state handling of the cloned head (steps 9bp/9bu/9bpu; default 1) |
| `rdna3dmubwait=0` | DAL SMU without waiting for DMCUB answers (step 9bw) |
| `rdna3pm=0`/`1` | boot clocks only / the safe PMFW feature set (profile: 2, the amdgpu set) |
| `rdna3pmmask=<hex>` | the PMFW feature mask to allow (debug) |
| `rdna3pmexp=1` | the idle-power experiment, six variants in one boot (test only) |
| `rdna3reset=0`/`2` | mode-1 reset before init never / always (default 1: only when the PSP is stuck) |
| `rdna3post=0` | do not post an unposted card through the VBIOS |
| `rdna3doorbell=0` | no doorbell BAR: rings keep their write-pointer registers |
| `rdna3cursor=0` | no hardware cursor |
| `rdna3mtlapps=0` | read by the Metal plugin: ends apps mode (`%apps` in `rdna3mtl-allow.txt`), only the names in the list keep the GPU |
| `rdna3enum=1` | report the GPU as working for enumeration only; submissions fail (debug) |
| `rdna3visreserve=<MiB>` | how much of the CPU-visible VRAM heap (BAR0 minus the kernel's windows) is kept for the accelerator's allocations (WindowServer's framebuffers, cursor, swap copies); GEM BOs never take it. Default: two thirds of the heap, at most 128 MiB; `0` = no reserve (card run 2026-10-17c) |
| `rdna3vramwin=<MiB>` | the most of BAR0 the core kext maps into the kernel (the CPU-visible VRAM heap is this window minus the kernel's own low window); default 1024 (the largest BAR macOS assigns, so all of it), at least 64; a failed map is retried with half the window down to 256 MiB (DRIVER-PLAN N62) |
| `rdna3vmfrag=0` | user VRAM mappings with 4 KiB PTEs without a fragment, as before card run 2026-10-26h. Default (1): amdgpu_vm_pte_fragment PTE fragments (up to 1 MiB, capped at the physical alignment), which the GFX11 attribute ring's BIG_PAGE needs (DRIVER-PLAN N57); `0` only to tell new VM faults apart |
| `rdna3vramlostcs=0` | after a VRAM loss (a reset or a sleep whose VRAM did not all come back), contexts made before it keep submitting instead of failing with -ECANCELED (amdgpu's behaviour, the default); the counter and VRAMLOST are reported either way. With the default, RADV turns the refused submission into `VK_ERROR_DEVICE_LOST` ("The CS has been cancelled because the context is lost"), the Metal plugin then disables its Vulkan backend for that process and every later command buffer ends with `MTLCommandBufferErrorInternal`: the process (WindowServer too, while it has the RDNA3 Metal device) stops drawing with the GPU until it is restarted, rather than drawing from lost memory. `=0` keeps it drawing, possibly garbage (debug) |
| `rdna3soak=<min>`, `rdna3hangtest=1`/`2` | read by the report script only: soak loop, hang/recovery test |

### Which apps get the GPU (Metal plugin)

`/Library/GPUBundles/rdna3mtl-allow.txt` decides, in every process that
loads the plugin, whether that process gets the Metal device. Without a
device an app stays on Apple's software path (nothing breaks, it is just
not on the GPU). The shipped list has two parts:

- **Apps mode** (the `%apps` line): every application, meaning an
  executable in `Contents/MacOS` of an `.app` in `/Applications`,
  `/System/Applications` or the running user's own `~/Applications` (only
  `/Users/<name>`; homes elsewhere, other users' folders and translocated
  apps are refused; `Info.plist` is not read), but only while the accelerator is armed
  (`debug.rdna3accelfb`) and WindowServer runs on the plugin
  (`debug.rdna3agdc` is 1): the configuration card runs 48-56 ran Chess and
  Maps in. Not served: the session's agents (Dock, Finder, SystemUIServer,
  loginwindow: they run all session and each would hold one of the card's
  GPU address spaces), helpers and XPC services inside bundles, apps nested in another app
  (Xcode's Simulator), and command line tools. `-name` lines are a deny list
  (shipped: `-Safari`); they match the process name and the executable's file
  name, and `NVMTL_ENABLE` plus `mtlprobe`, `nvmtltest`, `nvmtlrender`
  bypass the list.
- **Names**: `mtlprobe`, `nvmtltest`, `vulkaninfo` and `@WindowServer`
  (WindowServer only while the display pipe is up: CoreDisplay asserts
  without a Metal device then, card runs 41 and 45). Leave these as they are.

Switch apps mode off without rebuilding anything: delete (or `#`) the
`%apps` line, which gives the old explicit-names list back, or add
`rdna3mtlapps=0` to the OpenCore boot-args when the desktop is unusable
(the list file needs a working session, the boot-arg does not). Add a helper
or an app that lives elsewhere by name (one line each); deny one with
`-name`. A package install replaces the file.

Each process that loads the plugin logs one line, `NVMTL: allow mode: <mode>;
<process>: <reason>` (report: `34-metal-allow.txt`); if a sandbox hides the
boot-args from a process, the line says `boot-args unreadable` and
`rdna3mtlapps=0` was not checked there (use the list file for those). Limits that are not the
driver's to fix: Safari's web content renders in `com.apple.WebKit.GPU`,
whose sandbox refuses to map code from `/Library/GPUBundles` ("file system
sandbox blocked mmap()", card run 56, SIP off or not), so Safari pages stay
on Apple's software path; Apple's own GPU bundles live on the sealed system
volume. Other sandboxed helpers (Chrome and Electron GPU processes) are in
the same position. Every process with the device takes one of the card's
15 GPU address spaces (VMIDs; 7 with `rdna3userq=1`) for as long as it runs,
so many GPU apps at once can exhaust them (the kernel log says `VM: no VMID`).

## 8. The way back

Prepare **one** of these before the first boot:

1. **A USB stick with a copy of your EFI** in which `RDNA3Accel.kext` has
   `Enabled = false` and the boot-args carry `-rdna3off`; boot it from the
   firmware's boot menu. This always works, even when the internal
   `config.plist` is the problem.
2. **`-rdna3off` in `config.plist`**: `oc_install.sh --efi /Volumes/EFI --off`
   writes it (with a backup `config.plist.rdna3-<date>`; restore a backup to
   undo). `--remove` instead takes the kext copies, their `Kernel/Add`
   entries and the `rdna3*` boot-args out for good (see Uninstall below).
   `sudo nvram boot-args=…` does not help when OpenCore sets
   boot-args from `config.plist` at every boot.
3. **From Recovery**, when macOS no longer starts: remove the kexts from
   the Data volume,
   `rm -rf "/Volumes/Macintosh HD - Data/Library/Extensions/RDNA3"*.kext`;
   if they still load, also move away
   `/Volumes/Macintosh HD - Data/Library/KernelCollections/AuxiliaryKernelExtensions.kc`
   (macOS rebuilds it). The OpenCore-injected core still loads: disable it
   in the EFI (`diskutil mount disk0s1`, then option 1 or 2).

Safe Mode (Shift in the picker) skips the Auxiliary KC (framebuffer,
accelerator) but not the core that OpenCore injects.

**Uninstall:** mount the EFI partition (`sudo diskutil mount disk0s1`), then
`sudo "/Library/Application Support/RDNA3/uninstall.sh"` and restart. It
removes the kexts in `/Library/Extensions`, the Metal plugin and RADV in
`/Library/GPUBundles` and `/Library/Application Support/RDNA3`, and takes
the OpenCore side out through `oc_install.sh --remove`, which first writes
the backup `config.plist.rdna3-<date>` and then deletes only what this
installer added: the `RDNA3*.kext` copies in `EFI/OC/Kexts`, their
`Kernel/Add` entries and the `rdna3*` / `-rdna3*` boot-args. Your other
kexts, entries and boot-args and the backups stay, and it prints what it
removed and what it left.

- The EFI is taken from `--efi DIR`, else `/Volumes/EFI`. If OpenCore booted
  the Mac but no EFI is mounted, the script stops **before removing
  anything** and says so; mount it, or add `--keep-opencore`.
- `--keep-opencore` leaves OpenCore alone. Its kexts then inject a core
  that is gone; remove them later with
  `sudo package/oc_install.sh --efi /Volumes/EFI --remove` from a source
  checkout (the installed copy is deleted by the uninstall), or by hand.
- `--keep-firmware` keeps `/Library/Application Support/RDNA3/firmware`.
- The same removal alone, keeping the install:
  `sudo "/Library/Application Support/RDNA3/oc_install.sh" --efi /Volumes/EFI --remove`.
  (`--off` is different: it only adds `-rdna3off`, the kexts stay.)

## Licenses

The kexts' core and `common/`, `hw/` are MIT. The accelerator, the
framebuffer and the Metal plugin derive from NullMoth and are under
PolyForm Noncommercial 1.0.0, the AIR translator under LGPL-3.0-or-later
(see `NOTICE`; the texts are installed in `/Library/Application Support/RDNA3`).
The AMD firmware is under `LICENSE.amdgpu` and is downloaded from
linux-firmware at install time, not distributed by this project.
