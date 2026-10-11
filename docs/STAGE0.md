# Stage 0: preparing the test setup

Before you load anything on the 7800 XT, build a setup where a driver bug
does not leave you without a screen and without logs.

## 1. Collaboration instead of writing everything from scratch

The NVIDIA project is the **NullMoth NVIDIA Driver**:
<https://github.com/nullmoth/nvidia-macos-driver> (v1.1.0, Sequoia 15.7/15.8,
RTX 5060). Full analysis: [NULLMOTH-ANALYSIS.md](NULLMOTH-ANALYSIS.md). Key points:

* Their Metal plugin loads the Vulkan driver through `dlopen()` and mostly uses
  standard Vulkan, so **it may work on top of RADV once the NVK hooks are disabled**.
* The AIR→SPIR-V translator is LGPL and can be used unchanged.
* Kernel↔userspace is done by **emulating Linux ioctls on IOKit**. For us this
  means: the kext implements the amdgpu UAPI, and RADV works with the existing winsys.
* The plugin and the kexts are under **PolyForm Noncommercial**, so they must not
  be pasted into our MIT repo without the author's permission.

To do by hand (needs your account): join the NullMoth Discord (the link is not
in the README; it is mentioned in the bug reporting section of app 1401)
and write to the author. Template:

> Hi, I am writing an open macOS driver for RDNA3 (RX 7800 XT):
> https://github.com/voten641/rdna3-hackintosh. The kext already recognizes the card and reads
> the IP discovery table. I plan to do what you did: a kext that emulates the Linux UAPI
> (amdgpu in my case) through IOConnectCallMethod, with Mesa's RADV on top. Your
> Metal plugin loads the ICD through dlopen, so it could work on top of RADV.
> 1) Would you consider a "generic Vulkan" mode for the plugin (NVK hooks optional)?
> 2) Is a license possible that allows combining the plugin with MIT code, or
>    a shared, neutral part of the plugin?
> 3) What minimal set of Vulkan features does the plugin require? I will check them
>    on RADV under Linux beforehand.
> I will gladly contribute fixes back to the plugin and the translator.

## 2. Hardware

| Role | Card | Purpose |
|---|---|---|
| Display | natively supported (e.g. RX 6600 / RX 580) | macOS has a working picture regardless of our kext |
| Test | RX 7800 XT | no monitors connected at the start of stages 1–2 |

* In BIOS/UEFI set the display card as primary (primary PCIe slot / "Primary
  Display"), enable *Above 4G decoding*. ReBAR can stay enabled, but the kext in
  stage 1 does not use BAR0.
* **Do not** spoof the 7800 XT as another device-id in OpenCore `DeviceProperties`
  and do not load WhateverGreen fake-IDs for it, because then Apple's driver
  matches it instead of ours.
* Ideally also have a Linux disk in the same computer: amdgpu on the same
  card is the reference we compare every read against
  (`tools/linux/dump_amdgpu_reference.sh`).

## 3. System and debugging

* **macOS Sequoia 15.x** (like the NVIDIA project). Write down the exact build (`sw_vers`),
  because the Kernel Debug Kit must match it exactly.
* **SIP disabled**: in OpenCore `NVRAM > Add > 7C436110-AB2A-4BBB-A880-FE41995C9F82 >
  csr-active-config = EF0F0000` (0xFEF) and `SecureBootModel = Disabled`.
* **boot-args** during development:

  ```
  -v keepsyms=1 debug=0x144 rdna3lvl=0
  ```

  `debug=0x144` = NMI from the power button (0x4) + ARP for the KDP debugger (0x40) +
  panic log on screen (0x100). `keepsyms=1` gives symbols in backtraces.
* **Kernel Debug Kit** for exactly this build from developer.apple.com: kernel
  symbols for LLDB (`lldb /Library/Developer/KDKs/<KDK>/System/Library/Kernels/kernel`).
* **LLDB over the network (KDP) and panic dumps (netdump)**:

  ```
  debug=0x144 kdp_match_name=en0                      # LLDB: kdp-remote <IP>
  debug=0x144 kdp_match_name=en0 _panicd_ip=<IP>      # core dump to kdumpd
  ```

  Note: KDP only works with network cards whose Apple driver supports
  polling mode. Popular hackintosh drivers (Realtek, Intel kexts from
  community repositories) usually **do not work** with KDP. Check this at the very
  start; the alternative is a USB-Ethernet adapter supported by Apple or a
  PCIe Intel I210/I225 with the Apple driver.
* Logs without a debugger: `log show --last boot --predicate 'sender == "RDNA3Accel"'`
  or `sudo dmesg | grep RDNA3`.

## 4. Build tools

* A Mac with Xcode (full Xcode, not just the CLT, so that `Kernel.framework` is in the SDK).
* We build the kext with `make kext`; CI does the same on the self-hosted macOS runner, so the artifact
  from GitHub Actions is also fine for testing.
* Linux is enough for work on portable code (`common/`, tests, tools).

## 5. Checklist before the first kext load

- [ ] The second card shows a picture, 7800 XT has no monitor
- [ ] `ioreg -l | grep -i 747e` shows the 7800 XT PCI device without a driver
- [ ] SIP disabled (`csrutil status`)
- [ ] boot-args as above, the OpenCore picker can boot a second configuration
      **without** the kext (rescue)
- [ ] Reference dump from Linux done (`tools/linux/dump_amdgpu_reference.sh`)
