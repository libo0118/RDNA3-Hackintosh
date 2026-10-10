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
| `9b` | the second monitor with the same picture (`rdna3vm args "rdna3lvl=8 rdna3msi=1 rdna3fb=1 rdna3dmub=1 rdna3mirror=1"`) | only after step 5b passed; **either screen may go dark** |
| `10` | accelerator (WindowServer) | risky; send the reports from 0–9 first |

If you want to look longer, add `--stay`, for example `rdna3vm step 9 --stay`.

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
rdna3vm reports
```

From each step send the files from `~/rdna3-vm/reports`:
- `rdna3-report-*.tar.gz` (report from macOS);
- `*-tests.txt` (DMA and GFX test results, from step 7);
- `serial-*.log` (macOS kernel log; it is kept even after a hang or a panic);
- `host-*.log` (what the script did on Linux).

With the first submission also include the `linux-reference` directory.

Want a first look yourself? `python3 ~/RDNA3-Hackintosh/tools/triage_reports.py ~/rdna3-vm/reports`
prints PASS / FAIL per step. What happens with the reports next is in
[AFTER-TESTS](AFTER-TESTS.md).

## New driver version

1. Download the new artifact (step C, item 1).
2. Run `rdna3vm prepare --artifact ~/Downloads/RDNA3Accel-kext.zip`.
3. Run `rdna3vm window` and in the macOS Terminal paste the same
   `curl ... | sudo sh` command as in step D. Then `sudo shutdown -h now`.
4. Go back to step E.

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
