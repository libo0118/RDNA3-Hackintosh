# Test in a virtual machine (Linux + a single RX 7800 XT card)

Instructions for a Linux computer in which **the only graphics card is the
RX 7800 XT** (no iGPU and no second card). macOS runs in a QEMU virtual
machine, and the card is passed through to it (passthrough). One script
does everything: `tools/vm/rdna3vm`.

What it looks like in practice:

1. You type `rdna3vm step 2`. The Linux desktop disappears, because Linux hands over the card.
2. macOS appears on the monitor. It boots with our driver at the
   level for the given step.
3. After about 2 minutes macOS collects a report by itself, sends it to Linux and
   shuts down. The Linux desktop comes back.
4. The report is in `~/rdna3-vm/reports`. You type the next step.

**Time:** one-time setup, about 1–1.5 hours (almost all of that time is
the macOS installation). After that each step takes about 3 minutes.

> During a step Linux has no display, and the keyboard and mouse belong to macOS.
> **Every step logs you out of the Linux desktop**: the desktop session holds the
> card, so the script stops the display manager to hand the card over and starts
> it again afterwards. You come back to the login screen; that is expected.
> Save your open work beforehand. You will not get stuck without a display, see
> [Emergency exit](#emergency-exit): **both Ctrl keys at once, then Esc 3×**.

---

## Step A. BIOS and Linux (once, 10 min)

**Motherboard BIOS:**
- enabled: **SVM** (AMD) or **VT-x** (Intel);
- enabled: **IOMMU** (AMD-Vi) or **VT-d** (Intel);
- enabled: **Above 4G Decoding**;
- disabled: **CSM**.

Resizable BAR can stay enabled.

**Kernel parameter** `iommu=pt` (on Intel also `intel_iommu=on`):

| Distribution | Command |
|---|---|
| Ubuntu / Debian / Mint | in `/etc/default/grub` add the text `iommu=pt` to `GRUB_CMDLINE_LINUX_DEFAULT`, then `sudo update-grub` |
| Fedora | `sudo grubby --update-kernel=ALL --args="iommu=pt"` |
| Arch (GRUB) | like Ubuntu, then `sudo grub-mkconfig -o /boot/grub/grub.cfg` |

Then restart the computer.

**Packages:**

| Distribution | Command |
|---|---|
| Ubuntu / Debian | `sudo apt install qemu-system-x86 qemu-utils python3 git dmg2img zstd psmisc` |
| Fedora | `sudo dnf install qemu-kvm qemu-img python3 git dmg2img zstd psmisc` |
| Arch | `sudo pacman -S qemu-full python git zstd psmisc`; `dmg2img` is in the AUR |

Finally, add yourself to the `kvm` group: `sudo usermod -aG kvm $USER`. Then
log out and log back in.

**Check:**

```sh
git clone -b stage1-kext https://github.com/Voten641/RDNA3-Hackintosh ~/RDNA3-Hackintosh
~/RDNA3-Hackintosh/tools/vm/rdna3vm check
```

Every line should show `OK`. The script also shows what will go to the VM: the card (with its
HDMI audio function) and the USB keyboard and mouse.

For convenience, set an alias. After that typing `rdna3vm` is enough:

```sh
echo "alias rdna3vm=~/RDNA3-Hackintosh/tools/vm/rdna3vm" >> ~/.bashrc && . ~/.bashrc
```

## Step B. Downloading macOS (once, 10 min)

We use the existing [OSX-KVM](https://github.com/kholia/OSX-KVM) project:

```sh
git clone --depth 1 --recursive https://github.com/kholia/OSX-KVM.git ~/OSX-KVM
cd ~/OSX-KVM
./fetch-macOS-v2.py                      # choose Sequoia (macOS 15)
dmg2img -i BaseSystem.dmg BaseSystem.img
```

## Step C. Driver package (once and after every new version)

1. Open [GitHub → Actions](https://github.com/Voten641/RDNA3-Hackintosh/actions).
   Go to the latest green **build** run on the `stage1-kext` branch and
   download the **RDNA3Accel-kext** artifact (a `.zip` file).
2. Run:

```sh
rdna3vm prepare --osx-kvm ~/OSX-KVM --artifact ~/Downloads/RDNA3Accel-kext.zip
```

The script does, in order:
- a copy of OpenCore with SIP disabled (required for an unsigned kext);
- a package for macOS: kexts with firmware taken from `/lib/firmware/amdgpu` of your
  Linux;
- a dump of the card's VBIOS;
- reference data from amdgpu (`reports/linux-reference`), which we
  compare the results with.

It asks for the `sudo` password a few times.

## Step D. Installing macOS in a window (once, ~1 h)

This step works **without passing through the card**: macOS opens in a normal
window on the Linux desktop.

```sh
rdna3vm window
```

1. In the OpenCore menu choose **macOS Base System**.
2. **Disk Utility** → choose *QEMU HARDDISK* (about 128 GB) → **Erase**:
   name `macOS`, format **APFS**. Close Disk Utility.
3. **Reinstall macOS Sequoia** → choose the `macOS` disk. The installer
   restarts the VM several times by itself. If you see the OpenCore menu, choose
   the installer entry (*macOS Installer*), and at the end simply `macOS`.
4. Create an account. You can skip Apple ID, location and other options.
5. In macOS open **Terminal** (Cmd+Space, type "Terminal") and paste:

```sh
curl -s http://10.0.2.2:8000/setup.sh | sudo sh
```

This installs the driver and the automatic reports. When it finishes, shut down the VM
with `sudo shutdown -h now`.

If macOS shows "System Extension Blocked": go to System
Settings → Privacy & Security → **Allow**, restart and shut down the VM.

## Step E. Tests on the card (about 3 min each)

Run the steps **in order**. Start the next one only when the previous one
has come back with a report:

```sh
rdna3vm step 0
```

The desktop goes dark, OpenCore appears on the monitor, and then macOS with white
text on a black background (verbose mode). About 2 minutes after the desktop
appears, macOS shuts down by itself and the Linux desktop comes back. Then run the next
steps:

| Step | What it tests | What you watch on the screen |
|---|---|---|
| `0` | the kext loads, does nothing | macOS boots normally |
| `1` | register reads, VRAM size | same as above |
| `2` | IP discovery table, VBIOS, monitor (EDID) | same as above |
| `3` | GC block registers, CU count | same as above |
| `4` | PSP: firmware loading | same as above; the first step that writes to the card |
| `5` | SMU: clocks, fan, temperature | the fan speed may change |
| `5b` | DMCUB (display firmware) | **the screen may flicker or go dark** |
| `6` | GPU memory (GART), interrupts | same as above |
| `6b` | MSI interrupts | same as above |
| `7` | SDMA: first commands executed by the GPU (+ DMA test) | same as above |
| `8` | GFX: graphics ring (+ GFX test) | same as above |
| `9` | our framebuffer instead of the UEFI image | the image should stay unchanged; **it may go dark** |
| `8c` | MES scheduler (`rdna3mes=1`) | low risk, the kernel rings stay on the KIQ |
| `userq` | user queues through the MES (`rdna3userq=1`, + user queue test) | low risk; after 8c |
| `pmctl` | power management from userspace (`rdna3pmctl=2`): fan, power limit, overdrive, profiles; the report sets each once and back | the fan may change speed for a few seconds |
| `baco` | the chip through BACO and back (`rdna3baco=1`), then the GPU is brought up again | **risky**: only after `pmctl` passed; the screens go dark during the cycle, a failed one needs a reboot |
| `vcn` | the video engines: VCN 4.0 decode/encode and the JPEG decoder (`rdna3vcn=1`, + `rdna3_drminfo --vcn-test`) | medium risk; after 8 passed. Nothing shows on screen; the report has the result |
| `vcn2` | the video engines as in `vcn`, with the power saving Linux uses on this card (dynamic power gating, `rdna3vcn=2`) | medium risk; after `vcn` passed. Nothing shows on screen; the report has the result |
| `9b` | the second monitor with the same picture (`rdna3dmub=1 rdna3mirror=1`) | only after step 5b passed; **either screen may go dark** |
| `9c` | the second monitor as its own display (`rdna3mirror=2`) | after 9b; **either screen may go dark** |
| `10` | accelerator (WindowServer) | risky; send the reports from 0–9 first |
| `hotplug` | a monitor unplugged and plugged back while macOS runs (`rdna3hotplug=1`) | **stay at the machine**: when the progress window says *unplug and replug the second monitor now*, pull the second monitor's cable, wait about 10 seconds, put it back; the second screen goes dark and comes back, macOS drops that display and adds it again |
| `dplink` | the mode switches of step `modeset` on a monitor connected over native DisplayPort (`rdna3dplink=1`): the driver trains the DP link itself | only with a monitor on a DP cable straight into the card (no adaptor) that shows the boot picture; that monitor goes black for a moment at each switch and comes back in the new mode. **It may stay on "no signal"** until the step puts the old mode back |
| `dsc` | step `dplink` with DSC and FEC (`rdna3dsc=1`): a mode too big for the DP link is sent compressed | only with a DP monitor that supports DSC (most 4K 144 Hz and 5K/6K monitors) on a DP cable straight into the card; the monitor goes black at each switch; **it may stay black or show a garbled picture** until the step puts the old mode back |
| `mst` | step `dplink` through an MST hub or a daisy-chain monitor (`rdna3mst=1`): the driver reads the monitors behind the hub and drives one of them | only with an MST hub (or a monitor with DP out) on a DP cable into the card and a monitor behind it showing the boot picture; that monitor goes black at the first switch and should come back; **it may stay black** until the step goes back to the normal (SST) mode. `rdna3mstport=2` picks the hub's second port |
| `uhbr` | step `dplink` with the DisplayPort 2.1 speeds (UHBR10 / 13.5 / 20, `rdna3uhbr=1`): a mode too fast for DP 1.4 runs over the new 128b/132b link | only with a DP 2.1 monitor ("UHBR" or "DP80" in its specs) on a DP80 or DP54 certified cable straight into the card, and a mode beyond 4K 120 Hz in its list (4K 240 Hz, 5K, 8K); the monitor goes black at each switch; **it may stay black** until the step goes back to the old mode |
| `odm` | step `uhbr` with two display pipes per picture (`rdna3odm=1`, and DSC): an 8K mode is split in a left and a right half | only with an 8K monitor (or a very high refresh-rate one) on native DP; **it may show half a picture or stay black** until the step goes back to the old mode |
| `mall` | step `modeset2` with the display power features (`rdna3mall=1`): the memory clock and the fabric clock may go down while the monitors stay on, the idle screen is kept in the GPU's cache | both monitors as usual; watch for flicker, a black line or a short black screen, especially in the first minute and when nothing moves on screen. Then do the mode switches of `modeset2` |
| `subvp` | step `mall` with one monitor and "sub-viewport" memory switching (`rdna3subvp=1`) for high refresh rates | only with a monitor that runs 120-175 Hz at 4K, 3440x1440, 2560x1440 or 1080p; only that monitor is used. Switch it to its high-refresh mode with `rdna3_modetest`; **it may flicker or show a torn picture** while the mode is set: note what it showed, then switch back |
| `otg` | the second monitor as its own display (step `9c`) and the driver reading every lit picture's timing generator through Linux's own functions (`rdna3otg=1`) | low risk: nothing changes on screen; the result is in the report |
| `dmubreset` | step `9c` with the display microcontroller (DMCUB) reset exactly as Linux resets it (`rdna3dmubreset=1`) | medium risk: the second monitor may stay dark if the DMCUB does not come back |
| `hdmi` | the mode switches of step `modeset` with Linux's HDMI setup on top (`rdna3hdmi=1`): the picture-format packet (AVI infoframe) the TV/monitor reads, audio/video mute around each switch, the HDMI 2.0 control channel (SCDC) | for a monitor or TV on HDMI (or DVI); it goes black for a moment at each switch as in `modeset`. If it shows wrong colours, overscan or "no signal" after a switch, note what it showed |
| `cursormall` | step `9c` with the mouse pointer read through the card's big cache as Linux does it (`rdna3cursormall=1`) | low risk; **move the mouse over both monitors** during the step and note if the pointer flickers, shows garbage or vanishes |
| `dcseq` | step `modeset2` with two small steps Linux adds around each switch (`rdna3dcseq=1`): the HDMI clock kept running a moment longer while the picture goes off, and the display FIFOs re-synchronised after the switch | same as `modeset2`: the monitors go black for a moment at each switch and come back |
| `audio` | the mode switches of step `hdmi` with the monitor's sound turned on as Linux does it (`rdna3audio=1`, also on a DisplayPort monitor): the card tells the monitor's speakers about the sound it can send and starts the sound stream | for a monitor or TV **with speakers or an audio output** on HDMI (or native DisplayPort). After the step, if macOS has an HDMI audio driver for the card, open System Settings > Sound > Output: note whether the monitor is listed and whether a test sound plays. If nothing is listed, that is expected without such a driver: the report still shows whether the card side worked |
| `fmt` | the mode switches of step `hdmi` in the colour format Linux picks (`rdna3fmt=1`): YCbCr instead of RGB and 10 or 12 bits per colour when the monitor/TV supports it | for a monitor or TV on HDMI. Look at the colours after every switch: note any pink / green / washed-out picture or visible banding, and if the monitor's own menu shows the input format ("YCbCr", "10 bit"), note what it says |
| `hdmi20` | step `modeset2` plus modes that need HDMI 2.0 speed (`rdna3hdmi=3`), e.g. 3840x2160 at 60 Hz | only with a 4K monitor/TV on an HDMI 2.0 (or newer) port and a "high speed" HDMI cable. **It may stay on "no signal"** until the step puts the old mode back. If the automatic run does not pick 4K60, run `rdna3_modetest set 0 3840 2160` once by hand |
| `sleep` | monitor standby on every monitor, then a short system sleep: the display is saved, taken down and brought back (`rdna3dpms=2 rdna3resume=1`) | both monitors go to standby (no signal) and come back; then macOS sleeps for ~30 s and wakes by itself (the host wakes the VM after 90 s otherwise). **A screen may stay dark after the wake** |
| `vcnsleep` | the `sleep` step with the video engines on (`rdna3vcn=2`): the video test runs before the system sleep and again after the wake | as `sleep`; after `sleep` and `vcn2` passed. **A screen may stay dark after the wake** |
| `amdgpu` | the default profile, but the driver follows Linux amdgpu's own order where the tested one differs (`rdna3amdgpu=1`): firmware load order, power management setup, page tables, the interrupt ring in system memory | medium risk; after `default` passed. Looks like step `default`; if the screen stays on the boot picture and the report says PSP or SMU failed, boot once without it |

If you want to look longer, add `--stay`, for example `rdna3vm step 9 --stay`.

Step `hotplug` is the one step that needs you at the screens: about two minutes after the desktop appears, the progress window and a notification say *unplug and replug the second monitor now*. You have two minutes from then: pull the second monitor's cable (the one the GOP did not light at boot), wait about 10 seconds, plug it back in. The report records what the driver saw (`RDNA3,HPDState` over time, the hotplug lines of the log) and takes a picture of both pipes afterwards. If you miss the window, the report says `HOTPLUG: 0 HPD change(s)`; run the step again.

### A new framebuffer build: allow it once

OpenCore injects the core kext itself, but `RDNA3Framebuffer` and
`RDNA3Accelerator` load from macOS's own kext cache, and macOS takes a new
ad-hoc signed build only after you allowed it. After `rdna3vm prepare`
with a build that changed them (the reports say so: `waits for approval`):

```sh
rdna3vm approve
```

macOS starts and stays up: System Settings → Privacy & Security, scroll
down, **Allow** next to the blocked system software, enter the password,
then **Not Now** at the restart question and Apple menu → Shut Down.

`rdna3vm window` works as well (the card stays with Linux): about 2
minutes after the login the boot-time report script has installed the
new kexts, and the **Allow** button appears in the same place.

### Several steps in one go

```sh
rdna3vm queue pending
```

runs the step(s) still waiting for the card - since card run 34 one big
step, `big3`: the default profile with every new feature the card can test
without extra hardware (amdgpu's exact sequence, user queues, PM control,
VCN, mode set, HDMI stream / format / audio endpoint, display power); before
it `big2`: macOS boots once and the report itself runs everything
(DMA/GFX/Vulkan tests - buffers, a triangle, images, timeline semaphores -,
Metal when the plugin is installed, the idle-power experiment, three looks at what
each monitor shows, display sleep and wake, a switch through scaled
modes, a 2-minute soak, the hang test last), then shuts down. About 12
minutes, nobody needs to watch the screens; both monitors connected.
While it runs, a Terminal window on the macOS desktop shows each test and
its result (OK / FAIL), with notifications at the start and the end
(`/Users/Shared/rdna3-progress.log`); closing it changes nothing.
You can also name steps yourself: `rdna3vm queue 5b 8c 9`.
The [emergency exit](#emergency-exit) stops the VM **and** the rest of
the queue. If the computer reboots on the way (amdgpu could not take the
card back), log in and run `rdna3vm queue` without steps: it goes on
with what is left. At the end, `rdna3vm result` packs them into one file to send.

Step 9 in the VM: OpenCore sets the largest GOP mode (`OC_RESOLUTION`), so
the desktop comes up at the monitor's native mode; only the monitor the GOP
lit shows a picture. EDID comes over AUX for DisplayPort sinks and over the
DDC (I2C engine) for HDMI and DP->HDMI adaptors. Read the framebuffer lines
from the report (`20-log-rdna3.txt`); `tools/triage_reports.py` checks them.
The VM will then not shut down by itself. Shut down macOS normally (Apple menu → Shut Down)
or with the power button. The report is sent anyway.

The values we expect at each step are described in [TESTING-HW](TESTING-HW.md).
You do not need to check them, sending the reports is enough.

## Emergency exit

Each of the methods below closes **only the VM**. The script then gives the card,
keyboard and mouse back to Linux and restores the desktop:

1. **Both Ctrl keys at once, then Esc 3×** (within 3 seconds). Both Ctrl keys take
   the keyboard away from macOS and give it back to Linux, and Esc 3× closes the VM. It works even
   when macOS hangs or has a kernel panic, because the keyboard is read by Linux,
   not macOS.
2. **A short press of the computer's power button.** While the VM is running Linux
   does not shut down from it, it only closes the VM.
3. **You do nothing:** an automatic step closes the VM by itself after 8 minutes
   (it usually finishes after about 3). With `--stay` the limit is 30 minutes. You can change both
   limits in `~/rdna3-vm/config` (`AUTO_TIMEOUT_MIN`, `TIMEOUT_MIN`).

The script gives the card back to Linux even when QEMU itself crashes.
The restore is hooked to the script's exit, not to macOS.

After the VM an RDNA3 card does not come back to Linux by itself (amdgpu
logs `DMCUB error` / `failed to blank crtc` and the screen stays black). So
after every step **the computer sleeps for about 5 seconds and wakes up by
itself**: that powers the card off and on, and the desktop comes back. If
amdgpu still cannot drive the card, the script **reboots cleanly** (the
reports are already saved): wait a minute and log in. Settings in
`~/rdna3-vm/config`: `RESTORE_MODE=suspend` (default), `auto` (no sleep),
`reboot` (always reboot).

Last resort, when nothing happens for 2 minutes after the VM closed: briefly
press the power button (Linux shuts down normally), or hold it for 5 seconds.
It does not harm the card.

## When something goes wrong

| Symptom | What to do |
|---|---|
| black screen or a hang longer than 5 min, kernel panic | [emergency exit](#emergency-exit) (both Ctrl keys, Esc 3×). Send the reports and **do not go further** |
| the Linux desktop does not come back after the VM closes | short press of the power button: Linux shuts down normally (see above) |
| `step` ends immediately, the desktop does not disappear | read `~/rdna3-vm/reports/host-*.log` (the newest) and send it |
| macOS did not send a report | run the same step with `--stay` and in the macOS Terminal run `sudo sh "/Library/Application Support/RDNA3VM/autoreport.sh"` |
| the keyboard does not work in macOS | the VM started without input capture: send `host-*.log`; exit with the power button |

A computer without a login manager (booting to a console) also works. The script itself
stops `display-manager` and then starts it again.

## What to send me

```sh
rdna3vm result
```

It packs every report written since the last `step` / `args` / `queue`
started (plus a triage summary) into **one** file,
`~/Downloads/rdna3-result.tar.gz`, replaced each time it runs. Send that file.

`rdna3vm reports` only lists them. What one step writes to `~/rdna3-vm/reports`:
- `rdna3-report-*.tar.gz` (report from macOS);
- `*-tests.txt` (DMA and GFX test results, from step 7);
- `serial-*.log` (macOS kernel log; it is kept even after a hang or a panic);
- `host-*.log` (what the script did on Linux).

With the first submission also include the `linux-reference` directory (not in the result file).

Want a first look yourself? `python3 ~/RDNA3-Hackintosh/tools/triage_reports.py ~/rdna3-vm/reports`
prints PASS / FAIL per step. What happens with the reports next is in
[AFTER-TESTS](AFTER-TESTS.md).

## New driver version

`rdna3vm update` does steps 1-2 in one go: `git pull` in the checkout, the
newest successful CI build's artifact over `~/Downloads/RDNA3Accel-kext.zip`,
then `prepare`. It needs GitHub access once: `gh auth login` (GitHub CLI), or
a token with *Actions: read* for the repository in
`~/.config/rdna3vm/github-token`. By hand:

1. Download the new artifact (step C, item 1).
2. Run `rdna3vm prepare --artifact ~/Downloads/RDNA3Accel-kext.zip`.
3. Run `rdna3vm window` and in the macOS Terminal paste the same
   `curl ... | sudo sh` command as in step D. Then `sudo shutdown -h now`.
4. Go back to step E.

### With Vulkan (RADV)

A build made with `full` or `radv` on also has the artifact
`radv-macos-logs.zip` (RADV + `rdna3_vktest`). Pass it to prepare too:

```sh
rdna3vm prepare --artifact ~/Downloads/RDNA3Accel-kext.zip --radv ~/Downloads/radv-macos-logs.zip
```

Or build RADV inside the VM, no CI needed (no card either): `git pull`, then
`rdna3vm window`, and in the macOS Terminal:

```sh
bash <(curl -s http://10.0.2.2:8000/tools/build_radv.sh)
```

The first run installs the Xcode Command Line Tools (click Install in the
dialog, then run the command again), Python 3.12 from python.org (asks for
the password), the build tools from PyPI and glslang from source - no
Homebrew, which no longer installs on Intel Macs; Mesa takes 10-20 minutes. It ends with a loopback check and installs RADV
and `rdna3_vktest` where the report runs them.

From step 8 on the report then has a `== rdna3_vktest` section: RADV creates
a Vulkan device on the card and fills and copies buffers on every queue.

### Metal plugin (optional, once)

After `build_radv.sh`, in the same `rdna3vm window` session:

```sh
bash <(curl -s http://10.0.2.2:8000/tools/build_metal.sh)
```

Installs Rust through rustup on the first run, builds the Metal plugin and
the AIR->SPIR-V translator (5-10 minutes) and installs them with RADV into
`/Library/GPUBundles`. From then on the report has a `== mtlprobe` section:
does Metal list the AMD device, and does a compute kernel compiled from
source run on it (`METAL: ALL PASSED`). Without the plugin it only lists
the Metal devices. Only processes in `rdna3mtl-allow.txt` (and `mtlprobe`)
get the device; everything else, WindowServer included, sees nothing new.

## How it works (for the curious)

- `rdna3vm step N` writes the step's boot-args to the OpenCore `config.plist` (a copy
  in `~/rdna3-vm/OpenCore-rdna3.img`). Then, as a systemd service (`rdna3vm-*`,
  which keeps running after the desktop goes dark), it:
  - stops the desktop and detaches the console;
  - rebinds the card from `amdgpu` to `vfio-pci`;
  - starts QEMU with the card and the USB keyboard and mouse;
  - after the VM shuts down, gives everything back to Linux.
- Before the VM starts, the computer also sleeps for 5 seconds
  (`PRE_SUSPEND=1`): that powers the card off, so the VM gets it cold, as
  after a fresh boot. Without it the card's boot screen (OpenCore menu)
  stays dark: the monitor reports no signal.
- `prepare` sets OpenCore's `UEFI/Output/Resolution` to `Max`
  (`OC_RESOLUTION` in the config; empty = leave it): OVMF hands the
  card's GOP 800x600, and macOS keeps whatever mode the boot screen had.
- QEMU reads the keyboard and mouse from Linux (`input-linux`, the
  "both Ctrl" toggle) and passes them to macOS as USB devices. Linux itself watches for Esc 3×
  and the power button (`vmtool.py stopkeys`, `systemd-inhibit`). After the VM
  the button works normally again.
- `prepare` also copies the kexts into the OpenCore image (`EFI/OC/Kexts`,
  `Kernel/Add`), and OpenCore injects them at boot. macOS itself refuses our
  ad-hoc signed kexts ("invalid codeful kernel extension"). The copy in
  `/Library/Extensions` (from `setup.sh`) only provides the firmware files,
  which the kext reads from disk.
- macOS sees Linux at the address `10.0.2.2`. At that address
  `tools/vm/vmtool.py serve` serves the package and receives reports (only
  locally, `127.0.0.1:8000`).
- In macOS the `org.rdna3.vmreport` daemon collects a report 90 s after boot, i.e.
  `tools/macos/collect.sh` plus the `rdna3_drminfo` tests. It sends it and, if the host
  asked for it (`rdna3vmauto=1`), shuts down the system.
- The kernel log goes to the VM serial port (`serial=3`) into `serial-*.log`, and
  `debug=0x100` halts macOS on a panic instead of restarting.
