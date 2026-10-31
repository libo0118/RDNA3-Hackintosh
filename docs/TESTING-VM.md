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

1. Open [Forgejo → Actions](https://forgejo.voten.me/Voten641/RDNA3-Hackintosh/actions)
   (CI runs only there; GitHub just mirrors the code). Go to the latest
   green **build** run on the `stage1-kext` branch and download the
   **RDNA3Accel-kext** artifact (a `.zip` file). `rdna3vm update` does
   this for you (see "New driver version").
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

macOS runs at 1440x900 there (`WINDOW_RESOLUTION` in the config, or
`rdna3vm window --res 1280x800` for one run), not at the monitor's full
size, and the picture is scaled to the window (QEMU's GTK `zoom-to-fit`):
resize the window freely, the whole desktop stays visible. `--no-zoom`
shows it 1:1. The steps with the card set the card's own mode back
(`OC_RESOLUTION`).

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
| `agdc` | the desktop with RDNA3AGDC (`rdna3agdc=3`; WindowServer waits until it is up) and WindowServer on our Metal plugin, which CoreDisplay requires then (card runs 41-48; run 48: works), plus the Metal probes M4-M6 (an app window on the Metal plugin, Chess and Maps briefly). **First** rebuild the plugin in the VM: `rdna3vm window`, then in its Terminal `bash <(curl -s http://10.0.2.2:8000/tools/build_metal.sh)`. macOS learns the monitors' timings from it and should send each frame through the display pipe; the gamma test runs too | new kext: `rdna3vm approve` first (it asks to allow RDNA3AGDC); watch both monitors: if the picture freezes, goes black or flickers, say so |
| `relight` | step `hotplug` the NullMoth way (`rdna3hotplug=2`): the unplugged monitor keeps its place, macOS is not told; plugged back, the card lights the same picture again | **stay at the machine** and do the same as in `hotplug` (cable out ~10 s, back in): say whether the other monitor stayed as it was and whether the replugged one came back with the desktop (not black) |
| `cursormall` | step `9c` with the mouse pointer read through the card's big cache as Linux does it (`rdna3cursormall=1`) | low risk; **move the mouse over both monitors** during the step and note if the pointer flickers, shows garbage or vanishes |
| `dcseq` | step `modeset2` with two small steps Linux adds around each switch (`rdna3dcseq=1`): the HDMI clock kept running a moment longer while the picture goes off, and the display FIFOs re-synchronised after the switch | same as `modeset2`: the monitors go black for a moment at each switch and come back |
| `audio` | the mode switches of step `hdmi` with the monitor's sound turned on as Linux does it (`rdna3audio=1`, also on a DisplayPort monitor): the card tells the monitor's speakers about the sound it can send and starts the sound stream | for a monitor or TV **with speakers or an audio output** on HDMI (or native DisplayPort). After the step, if macOS has an HDMI audio driver for the card, open System Settings > Sound > Output: note whether the monitor is listed and whether a test sound plays. If nothing is listed, that is expected without such a driver: the report still shows whether the card side worked |
| `fmt` | the mode switches of step `hdmi` in the colour format Linux picks (`rdna3fmt=1`): YCbCr instead of RGB and 10 or 12 bits per colour when the monitor/TV supports it | for a monitor or TV on HDMI. Look at the colours after every switch: note any pink / green / washed-out picture or visible banding, and if the monitor's own menu shows the input format ("YCbCr", "10 bit"), note what it says |
| `hdmi20` | step `modeset2` plus modes that need HDMI 2.0 speed (`rdna3hdmi=3`), e.g. 3840x2160 at 60 Hz | only with a 4K monitor/TV on an HDMI 2.0 (or newer) port and a "high speed" HDMI cable. **It may stay on "no signal"** until the step puts the old mode back. If the automatic run does not pick 4K60, run `rdna3_modetest set 0 3840 2160` once by hand |
| `sleep` | monitor standby on every monitor, then a short system sleep: the display is saved, taken down and brought back (`rdna3dpms=2 rdna3resume=1`) | both monitors go to standby (no signal) and come back; then macOS sleeps for ~30 s and wakes by itself (the host wakes the VM after 90 s otherwise). **A screen may stay dark after the wake** |
| `vcnsleep` | the `sleep` step with the video engines on (`rdna3vcn=2`): the video test runs before the system sleep and again after the wake | as `sleep`; after `sleep` and `vcn2` passed. **A screen may stay dark after the wake** |
| `amdgpu` | the default profile, but the driver follows Linux amdgpu's own order where the tested one differs (`rdna3amdgpu=1`): firmware load order, power management setup, page tables, the interrupt ring in system memory | medium risk; after `default` passed. Looks like step `default`; if the screen stays on the boot picture and the report says PSP or SMU failed, boot once without it |
| `bisect` | the display bug (black screens, windows that do not appear or only flash, flicker): every new feature that has a runtime switch off, then back on one at a time, 30 s each (13 steps, 6.5 min; [Display bisect](#display-bisect-step-bisect)) | **sit at the monitors for the whole run**: a notification, a spoken "step N" and N beeps announce every step; note the step numbers where the picture goes wrong |
| `bisect-plugin` | the same for the Metal plugin's switches (`rdna3mtl.conf`, the allow list); **restarts WindowServer three times** | run it last, after `bisect`; log in again at the login window each time unless the guest logs in by itself |
| `bisect-agdc` | the same bisect on the **agdc profile** (WindowServer on our Metal plugin, the card busy): `swap_mode`, `swap_gen`, `flipbuf_present`, `swap_stale`, `iop`, `a3`, `swap_latch`, `iop_flip` in 13 combinations of 30 s each, 6.5 min ([Display bisect on the agdc profile](#display-bisect-on-the-agdc-profile-step-bisect-agdc)) | the plain `bisect` runs the default profile, where the AGDC is idle and WindowServer draws on the CPU: use this one for the real desktop. **Sit at the monitors for the whole run**; same announcements as `bisect`. Step 7 turns `iop` off while WindowServer runs: the picture may stay wrong afterwards (shut macOS down from the report's end as usual) |
| `ladder` | DRIVER-PLAN N50, the broken glyphs / tile blocks: the Metal plugin's A/B switches one rung at a time in ONE run (8 rungs, about 29 min; [The A/B ladder](#the-ab-ladder-steps-ladder-and-ladder-ws)) | nothing to type, nothing to answer: Terminal and Chess open and close by themselves on the desktop, look at the screen; the report has one row per rung. `ladder-ws` (about 45 min) adds the WindowServer rungs, which restart the desktop: **log in at the login window each time** |
| `apps` | the app survey: fifteen real apps (Chess, Terminal, TextEdit, Calculator, Notes, Safari with a text / canvas / WebGL page, Preview with a PDF and a JPEG, Maps, System Settings, App Store, Photos, Image Capture, QuickTime Player, Mission Control, Launchpad) opened one after another on the DEFAULT driver, a look of both screens each (about 25 min; [The app survey](#the-app-survey-step-apps)) | read the `app <name>` rows and look at the `scanoutapps-*` pictures; [AFTER-TESTS](AFTER-TESTS.md) "The app survey" |

If you want to look longer, add `--stay`, for example `rdna3vm step 9 --stay`.

Step `hotplug` is the one step that needs you at the screens: about two minutes after the desktop appears, the progress window and a notification say *unplug and replug the second monitor now*. You have two minutes from then: pull the second monitor's cable (the one the GOP did not light at boot), wait about 10 seconds, plug it back in. The report records what the driver saw (`RDNA3,HPDState` over time, the hotplug lines of the log) and takes a picture of both pipes afterwards. If you miss the window, the report says `HOTPLUG: 0 HPD change(s)`; run the step again.

### Display bisect (step `bisect`)

The screens have been wrong since about card run 58: black, windows that do
not appear or only flash every few seconds, flicker. Since then several
things were added; most have a switch that works while macOS runs. This step
turns every one of them off, then on again one at a time (the oldest first,
each one on top of the ones before it), and holds each state for 30 seconds
while **you** look at both monitors. The report then says at which step the
picture went wrong.

What you run:

```sh
rdna3vm update
rdna3vm step bisect
```

The VM boots as for any step. A few minutes after the desktop appears (the
progress window says when) the 13 steps start; the whole run is 6.5 minutes
and the report arrives a few minutes later. **Nothing else runs in this step**
(no swap A/B, no Metal apps, no mode switches except the controls): the screens show the desktop
and Activity Monitor (it redraws every few seconds, so a frozen picture shows).
Only look at the monitors; do not touch the mouse or keyboard.

What you see and hear at every step: a macOS notification "step N of 12: ...",
the voice saying "step N", and N short beeps (step 5: five beeps). If the
virtual machine has no sound you still get the notification and the progress
window; the timetable below tells you the step by the clock as well. Write down
the numbers of the steps where a monitor goes black, a window does not appear
or only flashes, or the picture flickers, and which monitor. The run also
looks at the monitors itself (two pictures of every pipe per step) and sends
what each step switched, so the report answers even without your notes.

Timetable (the time after the start; the progress window and the first
notification say the clock time of the start). Each state is held for 30 s.
"Off" is the value a switch has to restore the behaviour before it was added;
the switches read back are in the report (`39-bisect.txt`).

| Step | Time | What is switched | Since |
|---|---|---|---|
| 0 | 00:00 | nothing: the defaults as the boot left them (the reference) | |
| 1 | 00:30 | **every new feature off**: `a3` 0, `flicker` 0, `swap_latch` 0, `swap_torn` 0, `swap_mode` 0 (the flip), `trace_scan` 0, `swap_stale` 0 (closest to before the bug) | |
| 2 | 01:00 | `a3` 1: applications' IOSurfaces live in the card's VRAM (A3) | 2026-10-15 |
| 3 | 01:30 | `flicker` 1: the flicker test hashes the swaps' pictures (reads the card's memory through the BAR) | 2026-10-15d |
| 4 | 02:00 | `swap_latch` 1: a swap waits until its flip latched | 2026-10-17 |
| 5 | 02:30 | `swap_torn` 1: a second hash of every shown buffer | 2026-10-17 |
| 6 | 03:00 | `swap_stale` 1: the stale-back hold. This is the defaults again (the same switches as step 0; it only acts in mode 1/2) | 2026-10-17c |
| 7 | 03:30 | control, off by default: `swap_mode` 1 (the back is copied into a scanout buffer instead of flipped to; the former default, it broke both monitors in the 2026-10-17d bisect) | 2026-10-17b |
| 8 | 04:00 | control, off by default: `trace_scan` 1 (the trace samples the swap's other buffers) | 2026-10-17b |
| 9 | 04:30 | control, off by default: `swap_mode` 1 with `swap_landed` 1 | 2026-10-17c |
| 10 | 05:00 | control, off by default: `swap_mode` 2 (every back copied) | 2026-10-17b |
| 11 | 05:30 | control, off by default: `a3ws` 1 (WindowServer's own inputs in A3, the card run 58b flicker) | 2026-10-15d |
| 12 | 06:00 | the defaults again (the end state; the switches are put back even if the run dies) | |

Steps 7 to 11 are controls: they show what the states that are off by default
look like (swap mode 1 and 2, `trace_scan`, `a3ws`); a bad picture there is
expected and does not count. Since card run 2026-10-17d the defaults are the
flip (`swap_mode` 0) and `trace_scan` 0, i.e. the good state of that bisect, so
step 0, step 6 and step 12 have the same switches (a different picture in them
is reported as flaky). A step 1 that looks fine and a step 0 that does not means
the cause is one of the switches above; the first of steps 2 to 6 that is bad
again is the one that brings it back. If step 1 is bad too, none of these
switches is the cause.

`a3` and `a3ws` only reach surfaces created after the switch, so the run
opens Activity Monitor again at steps 0, 1, 2, 11 and 12; every other switch
acts at the next swap.

How to answer. By default the run is on a timer and records no answers
(the triage uses its own looks: a lit pipe with almost no picture is black, a
picture that stood still between two looks while it moved in other steps is
frozen). To answer with the keyboard, start it by hand in a Terminal inside
macOS instead:

```sh
rdna3vm args "rdna3bisect=3" --stay          # on the host: boots macOS, runs nothing
sudo sh "/Library/Application Support/RDNA3VM/autoreport.sh" --bisect    # in the macOS Terminal
```

Then press `y` (the picture looks right), `n` (broken) or Enter (next step)
once a step is on; a key moves on to the next step after the run has taken its
pictures (about 20 s into the step), no key = the 30 s timer. On bare metal
(docs/TESTING-HW.md) the same with `sudo sh "/Library/Application Support/RDNA3/autoreport.sh" --standalone --bisect`.

The Metal plugin's own switches are the optional second run:

```sh
rdna3vm step bisect-plugin
```

Its steps: 0 the plugin as it is; 1 `NVMTL_NO_SURFACE_SHARE=1`,
`NVMTL_NO_SURFACE_PAGEOFF=1` and `NVMTL_TRACE=0` in
`/Library/GPUBundles/rdna3mtl/rdna3mtl.conf` and the `%apps` line out of the
allow list (apps mode off); 2 the two `NVMTL_NO_SURFACE_*` lines out again (A3's
plugin half, 2026-10-15); 3 apps mode back (2026-10-15); 4 the frame trace back
(2026-10-17, the defaults again). The plugin reads its conf when a process
loads it, so WindowServer is **restarted at steps 1, 2 and 4**: your session
ends, the login window shows and you log in again (the run waits up to 5
minutes; if the guest logs in by itself nothing is needed). The risk is
that of any WindowServer restart: the picture may stay black and the VM may
panic (card run 50). The run uploads its table after every step, and the
plugin's files are put back at the end and at the next boot should the run
die. Run it last, and only when `bisect` points at nothing in the kernel.

This run is the default profile, where the AGDC is idle and WindowServer does
no GPU work (it renders on the CPU). Its answer (swap mode 0 fine, mode 1
broken) therefore does not hold for the real desktop, which is the `agdc`
profile: use [`bisect-agdc`](#display-bisect-on-the-agdc-profile-step-bisect-agdc)
for that.

Not switchable at runtime (not tested by the steps above): the copy
forward fixes of 2026-10-17 (`rdna3accelcf=0` switches copy forward off for
the whole boot), the visible-VRAM reserve (`rdna3visreserve=<MiB>`, 0 = none),
output gamma on by default (`rdna3gamma=0`), the VRAM-lost CS refusal
(`rdna3vramlostcs=0`), FreeSync (`rdna3vrr=1`, off by default), the VMID
admission and every other change in the kext without a switch. Try those with
`rdna3vm args "<the boot-arg>"` (no `rdna3lvl` in it: the default profile) and
compare with step 0. Older than the bug and left alone: `iop_async`,
`crc_head` and the plugin's `NVMTL_NO_DIRTYONLY`, `NVMTL_NO_TEXBUF`,
`NVMTL_SKIP_EMPTY`, `NVMTL_NO_BUFTEX_RECYCLE` (all imported with the plugin on
2026-10-09; their defaults did not change).

The result: `rdna3vm result` prints the table (step, what changed, your
answer, pipe 0 / 1 % not black, whether each picture moved between the step's
two looks, the swap and stale-hold counter changes) and names the first step
you marked `n`, the first whose looks went black or stood still, and where
the run says the bug starts (`BISECT: BREAKS AT step N`). Anything missing
(a look, the counters, a step that never ran) is a `WARN`.

### Display bisect on the agdc profile (step `bisect-agdc`)

Card run 2026-10-17f (`rdna3vm step play`, swap mode 0): the desktop is broken
again. The serial log says `swap #83: head 1: the buffer it showed (phys ...)
changed while on the panel ... rewritten in place and shown again` and
`FRONT SURFACE sample: *** MOVED -- A LIVE WRITER IS PAINTING THE SCANNED-OUT
SURFACE ***`, and the second back buffer reads empty at every swap. The plain
`bisect` could not have seen that: it boots the default profile, where the
AGDC is idle and WindowServer renders on the CPU. `bisect-agdc` is the same
30-seconds-per-step bisect on the profile you really use.

```sh
rdna3vm update
rdna3vm step bisect-agdc
```

The boot-args are exactly those of step `agdc` (WindowServer on our Metal
plugin: `rdna3agdc=3 rdna3acceliop=1 rdna3accelfb=4` ...) plus
`rdna3bisect=4`. (`rdna3bisect=3` is "no tests this boot, start by hand", see
above.) The run takes about 6.5 minutes of steps, the rest is the usual report
(trace pump, plugin trace, `collect.sh`); the host stops the VM after about 23
minutes at the latest. Everything else is as in `bisect`: the notification
"step N of 12: ...", the voice, N beeps, two pictures of each monitor per step
(8 s and 20 s in) and the table in `39-bisect.txt` / `<report>-bisect.txt`.
Without keys the triage judges from the pictures (black, frozen). To answer
with `y` / `n` / Enter, boot the agdc profile without tests and start the run
in a macOS Terminal (the plain `rdna3bisect=3` has no agdc profile, the AGDC
would stay idle):

```sh
rdna3vm args "$(awk '$1 == "agdc" { $1 = ""; print }' tools/ladder-steps.txt) rdna3bisect=3" --stay
sudo sh "/Library/Application Support/RDNA3VM/autoreport.sh" --bisect-agdc    # in the macOS Terminal
```

Unlike `bisect` the steps are **not cumulative**: every step sets its whole
combination from the values the boot left, so a step is only what its row
says. Each holds 30 s; the time is after the start (the first notification
says the clock time).

| Step | Time | What is set (everything else as the boot left it; `trace_scan` 0, `swap_stale` 1, `iop` 1 unless the row says otherwise) |
|---|---|---|
| 0 | 00:00 | the reference: `swap_mode` 1 (the copy, the default again), `swap_gen` 1 (the write generations), `flipbuf_present` 0, `a3` as booted |
| 1 | 00:30 | `swap_mode` 0 (the flip), `swap_gen` 1 |
| 2 | 01:00 | `swap_mode` 1, `swap_gen` 0 (the generations only counted: the copy of card run 2026-10-17d) |
| 3 | 01:30 | `swap_mode` 0, `swap_gen` 0 (the flip of card runs 2026-10-17f / g) |
| 4 | 02:00 | `swap_mode` 1, `flipbuf_present` 1 (the family's flip-buffer frames copied to the panel too) |
| 5 | 02:30 | `swap_mode` 0, `flipbuf_present` 1 (the family's flip-buffer frames flipped to) |
| 6 | 03:00 | `swap_mode` 2 (every back copied: the control, nothing held) |
| 7 | 03:30 | `swap_mode` 1, `iop` 0 (no display-pipe transactions) |
| 8 | 04:00 | `swap_mode` 1, `a3` 0 (Activity Monitor opened again) |
| 9 | 04:30 | `swap_mode` 1, `swap_latch` 0 (Activity Monitor opened again: `a3` is back, and only new surfaces see it) |
| 10 | 05:00 | `swap_mode` 1, `swap_stale` 0 (stale backs left to the write generations alone) |
| 11 | 05:30 | `swap_mode` 0, `iop_flip` toggled from its boot value (0 -> 1) |
| 12 | 06:00 | the defaults again (Activity Monitor opened again) |

All the switches can change at runtime (read from `accel/rdna3-accel.cpp`:
`swap_mode`, `swap_gen`, `swap_stale`, `trace_scan`, `swap_latch` and `iop_flip`
are read at every swap or import, `flipbuf_present` at every flip-buffer call,
`a3` at every IOSurface import, `iop` is a handler
that publishes or removes `IOAccelDisplayPipeCapabilities` and makes the
transaction hooks answer as the base class), so none was dropped. Three things
the run cannot promise: `a3` only reaches surfaces created after the switch
(hence the reopened Activity Monitor); WindowServer may have read the pipe
capabilities once at its start, so `iop` 0 may change only what the hooks
answer; and after step 7 the later steps assume `iop` back at 1 really
puts WindowServer back (step 12 has the same switches as step 0: a different
picture there is reported as flaky or an order effect). `iop` is never written
to the value it already has. The unswitched boot-args stay: `rdna3gamma`,
`rdna3accelcf` and the rest are in the report's boot-args.

Evidence per step, on top of what `bisect` records: the swap counters at the
step's start (the table subtracts them, so a delta is exactly one step), the
trace position (`trace_events_start` / `trace_events`, `stream_bytes_start` /
`stream_bytes`) so the decoder cuts the accelerator / DRM trace stream and
the Metal plugin's trace per step (`tools/rdna3_trace.py` on the stream, events
`trace_events_start`..`trace_events`), and one line `trace: events W..W (asked
by sysctl debug.rdna3accel_trace_dump)` in the kernel log at each step's start
(the serial log has no clock; the triage cuts the log by these lines). The
trace pump runs for the whole bisect; the full report follows the steps: the
trace stream, the plugin trace snapshot at the end, `collect.sh`. A plugin
trace snapshot after every step is not on by default (about 50 MB uploaded per
step, and the copy loads the guest while you look); by hand
`BISECT_PLUGIN_SNAP=1 sudo -E sh .../autoreport.sh --bisect-agdc` does it.

The result: `rdna3vm result` prints the same table as `bisect`, and a second
one with the counters of each step: swaps per head, copies, stale holds, zero-copy
`iop` flips, buffers **rewritten in place** (the flicker you saw), torn
buffers, frames copied by the display pipe, and the `FRONT SURFACE` lines of
that step (`live writer lines / sampler lines`). It names the steps that
**looked right** (your answers; black %, motion of the pictures) and the
ones that looked broken, and the same by swap mode. The FRONT SURFACE column
needs the kernel log: the serial log next to the report or the archive's
`20-log-rdna3.txt`. The kext samples the front surface only 40 times per boot, so
if those were used up before the bisect the column says so (a WARN) and the
`inplace` / `torn` counters are the evidence.

### Play (step `play`): the desktop is yours

When you want to drive the desktop by hand (drag windows, open apps) and still
get a full report:

```sh
rdna3vm update
rdna3vm step play
```

The profile is the `agdc` step's (WindowServer runs on our Metal plugin, so the
card works and its clocks are lifted) plus `rdna3play=1`. Do not use
`rdna3vm args "rdna3bisect=3" --stay` for this: it has no agdc profile, the
AGDC stays idle, WindowServer does no GPU work and no report is sent.

What happens: no test runs (no swap A/B, no mode switches, no apps, no gamma,
no sleep). A notification says *use the desktop; the report is taken at
minute 5 and again at minute 10*. The trace pump (accelerator and DRM trace
streams, the plugin's trace snapshots) runs as in every step. At minute 5 (after
the desktop has settled, about 90 s after boot) the run takes three scanout
looks, the counters and `rdna3_pmctl info` and the full collection
(`collect.sh`), and uploads it like any report (`rdna3vm result` picks it up).
At minute 10 the same again, as a second report with its own name (the label
is `play`). A snapshot of the trace stream is taken at each moment while the
pump keeps running. Then the VM stays up: shut macOS down yourself (Apple
menu), or the host stops it after 30 minutes. A report at shutdown is not
possible (the shutdown gives the report script seconds, a collection needs
minutes), so the second one is at minute 10.

The swap A/B is no longer part of a normal step (card run 2026-10-17d): add the
boot-arg `rdna3swapab=1` (`rdna3vm args "<the step's args> rdna3swapab=1"`) to
run it; it breaks the picture for a minute on purpose (mode 1).

CoreDisplay's IOPresentment switches (DRIVER-PLAN N44): every boot of the VM
(the install boot included) writes `useIOP` and `useIOP_onAMD` = true into
`/Library/Preferences/com.apple.CoreDisplay` (AnyHost) and into the ByHost store
`/Library/Preferences/ByHost/com.apple.CoreDisplay.<IOPlatformUUID>` (the one
Navi48-MacOS found CoreDisplay reading on macOS 26; each store is reported on
its own); WindowServer reads them when it
starts, so a change counts from the next boot. For the A/B without them add
`rdna3iopprefs=0` (`rdna3vm args "<the step's args> rdna3iopprefs=0"`): that
boot takes both keys out of both stores, and the boot after it starts without them. The
tests file says what this boot's WindowServer started with (`== CoreDisplay
prefs ...`), triage prints it as `present mode, CoreDisplay prefs`.

### Present-mode A/B (steps `play-macpro`, `play-rootport`, `play-desktop`)

Why: with our driver WindowServer presents with the legacy page-flip mode
(CoreDisplay logs `init_page_flip: page flip mode is on`) and never sends
IOPresentment transactions, while the NullMoth NVIDIA driver gets
transactions. Two things in our VM differ from that setup, and a card run
can tell whether either decides the mode:

- **The card is "integrated".** IOGraphics sets `IOFBIntegrated` when the
  card's PCI bus number is 0 (`fIntegrated = (0 == pciDevice->getBusNumber())`),
  and the VM puts the card on the root bus (`GPU_BUS=pcie0`, `00:10.0`).
  GPUWrangler then logs `flags 0x10 (IG) ... b:d:f=0:16:0`. Behind a PCIe
  root port the card is bus 1.
- **The Mac model is a muxed laptop.** The OpenCore image says
  `MacBookPro16,1`; CoreDisplay logs `isBDW=YES ... isMobile=YES` and
  `mux_initialize: Couldn't find any matches`. A Mac Pro has none of that.

The three steps are the `play` step (the desktop is yours, reports at minute 5
and 10, the VM stays up) with one or both changed for that run only:

| step | Mac model | card |
|---|---|---|
| `play` (as before) | MacBookPro16,1 | root bus (bus 0) |
| `play-macpro` | **MacPro7,1** | root bus |
| `play-rootport` | as in the config | **behind a PCIe root port (bus 1)** |
| `play-desktop` | **MacPro7,1** | **behind a PCIe root port** |

Cheapest order: `rdna3vm step play-desktop` first. If the present mode changes
there, run `play-macpro` and `play-rootport` to see which of the two did it
(or whether it takes both); if it does not change, neither is the cause and
the next suspect is CoreDisplay's own decision (`tools/probe/coredisplay.sh`,
run by CI on the macOS runner, lists what the function that logs the mode
reads). Each step is a normal card run: `rdna3vm update`, `rdna3vm step ...`,
use the desktop for ~10 minutes (or just wait), shut macOS down,
`rdna3vm result`, send the file.

Both can also be set for every run in `~/rdna3-vm/config` (`rdna3vm prepare`
keeps them): `SMBIOS_MODEL="MacPro7,1"` (also `iMacPro1,1`) and
`GPU_BUS="rootport"` (default `pcie0`), or for one custom run
`rdna3vm args "<the step's rdna3 args> @smbios=MacPro7,1 @gpubus=rootport"`.
The `@` words are for the host: they are taken out of the boot-args macOS gets,
and `rdna3vm check` prints what the config selects. An unknown `@` word or an
empty value (`@smbois=...`, `@smbios=`) stops the run with a message.

**Nothing asked, nothing touched.** `rdna3vm` edits the OpenCore config's
`SystemProductName` only when a model was asked for (`SMBIOS_MODEL` other than
`MacBookPro16,1`, or a step's `@smbios=`). With nothing asked, `prepare`,
`window`, the install boot and every card step leave the OpenCore config as it
is, exactly as before this feature (a config whose `PlatformInfo/Automatic` is
false, which `--smbios` refuses, still works). If an earlier run wrote another
model (`play-macpro`), the next run that asks for nothing writes
`MacBookPro16,1` back, so the model never outlives the step that asked for it;
the host log says `SMBIOS model:` with the model, or `SMBIOS model: not asked`.

**What changes in the VM with the model.** Only `PlatformInfo/Generic/SystemProductName`
of the OpenCore config. The serial number, board serial, UUID and ROM stay as
they are, so the serial belongs to a MacBook model while the VM says it is a
Mac Pro; that only matters for Apple's online services (iMessage, iCloud
activation), which this VM does not use. macOS derives the board id
(`Mac-27AD2F918AE68F61` for MacPro7,1) from the model; the report records
`hw.model` and `board-id` in `00-system.txt`. Both desktop models are the
kind the OpenCore sample configs use; `iMacPro1,1` is only for the case
`MacPro7,1` boots badly (macOS 26 supports the Mac Pro, not the iMac Pro: an
update from the installer may refuse it, a disk that is already installed
boots).

**Does a model change need `rdna3vm approve` again? Unknown.** macOS records
the "Allow" for a third-party kext by team and bundle identifier
(`/var/db/SystemPolicyConfiguration/KextPolicy`) and builds the Auxiliary KC
for the architecture, not for the model, so the approval should survive; nobody
has tried it on this VM. Nothing is silent either way: the install boot and the
approval window of a step run with that step's model, and if macOS then ran
an old framebuffer/accelerator the run says `STALE KEXTS old driver ran`
(triage, first line) and the host log names the model change
(`SMBIOS model changed since the last card run: A -> B`). In that case run
`rdna3vm approve --step play-desktop` (the window then runs that step's
model; plain `rdna3vm approve` uses the config's; `approve --card --step` is
refused, the card boot would use step 0's boot-args) and send the result so the
open question in DRIVER-PLAN N39 can be closed. The messages that tell you to
approve (the run's note, the queue stop, the triage's `STALE KEXTS` line) name
`rdna3vm approve --step <n>` by themselves when the step used `@smbios=`.

**The root port.** `GPU_BUS=rootport` puts the card (VGA function 0 and the
HDMI audio function 1, together) on `pcie-root-port,id=rp0,hotplug=off`. Earlier
runs found the card unconfigured behind an unsized port (commits 0a80e3c,
c21eed6), so the port now carries the windows the card needs: `mem-reserve`
for the 32-bit BARs (2 MiB or more), `pref32-reserve` (512 MiB for the default
256 MiB BAR0, all BARs stay below 4 GiB as on the root bus) or, with a 64-bit
window (`RESIZE_BAR_LOG2` above 10, `PCI_MMIO64_MB`), `pref64-reserve` (a power
of two above BAR0 + BAR2: 32 GiB for a 16 GiB BAR0) together with
`-global q35-pcihost.pci-hole64-size` as large as OVMF's window. The sizes
are read from the card's BARs after BAR0 was resized and printed in the host
log (`root port windows: ...`). It is **not proven on the card**: OVMF only
pads ports it treats as hot-plug capable, so with `hotplug=off` the real
windows come from the BARs OVMF finds behind the port. If the report shows no
AMD device or an unconfigured card, `host-*.log` has the guest's PCI bus at 8
and 40 s (`info pci`): send it, set `GPU_BUS=pcie0` again, and judge
`play-macpro` alone.

**What to look for in the result.** The triage prints one row each, for every
run (`present: ...` in the SUMMARY and the checklist, `present mode, ...` per report):

| row | before (card run 2026-10-17d) | what a change means |
|---|---|---|
| `page flip mode` | `init_page_flip: page flip mode is on` | `is off` or no such line: CoreDisplay chose another mode; the changed variable is the cause |
| `transactions` | `iop_ok 0, iop_empty 0` | `iop_ok > 0`: planes arrive in IOPresentment transactions (what the NVIDIA setups have); `iop_empty > 0` and `iop_ok 0`: transactions are sent but without planes (AGDC/IOPresentment next) |
| `integrated` | `IOFBIntegrated = Yes on 2 of 2` | `not set` after `play-rootport`/`play-desktop`: IOGraphics sees a discrete card |
| `GPUWrangler` | `flags 0x10 (IG) ... b:d:f=0:16:0` | no `(IG)`, `b:d:f=1:0:0` |
| `SMBIOS` | `MacBookPro16,1 ... isMobile=YES ... mux_initialize found no match` | `MacPro7,1 (hw.model, board-id ...)`, `isMobile=NO`; a `WARN ... BUT macOS reports X` means OpenCore did not apply the model |
| `card bus` (WARN only) | - | rdna3vm asked for pcie0/rootport but macOS sees the other bus |
| `fb index` | INFO: `IOFramebufferOpenGLIndex 0,1, IOFBDependentIndex 1,0: they disagree ... (unchanged)` | independent of the two variables above; INFO while it is the state of card run 2026-10-17d, WARN when the mismatch differs, INFO with a note when the indices now agree (a separate suspect, DRIVER-PLAN N39) |
| `host log not matched` / `request unknown` (WARN) | - | the host log is found by its `boot-args:` line against the report's boot-args, not by clock stamps. `not matched`: no log has them (or two logs with them ask for different things): the newest is used as a guess and the rows from it say so. `request unknown`: no host log, or one without the `SMBIOS model:` / `GPU on:` lines. A TESTS-ONLY report and a result without any report get `asked model`, `asked bus` and `guest PCI` (the card's bus and BARs in `info pci` at 8 and 40 s) from the host log alone |

### Driver-side A/B (DRIVER-PLAN N40)

The steps above change the VM; these boot-args change what our driver tells
macOS, so each one can be tried alone on top of any of them. The defaults are
in every run's log (`RDNA3-fb: head N switches: ...`, `RDNA3-agdc: vendor info
answer ...`) and the triage prints them (`fb N40, switches`).

| boot-arg | default | other value | what it changes |
|---|---|---|---|
| `rdna3agdcvendor` | 0: AMD 0x1002 | 1: NVIDIA 0x10de (what NullMoth's AGDC answers), 2: Intel 0x8086 | the AGDC vendor info IOPresentment asks for; also `sudo sysctl debug.rdna3agdc_vendor=1` at runtime (WindowServer may ask only at its start) |
| `rdna3fbintegrated` | 0: as IOGraphics sets it | 1: `IOFBIntegrated` removed, 2: set | the property only; GPUWrangler's `(IG)` stays (only `play-rootport` changes the bus) |
| `rdna3dispflags` | 0 (NullMoth's answer) | 1: the old answer (CEA underscan) | `kConnectionDisplayFlags` |
| `rdna3glorder` | 1: head 0 opens first | 0: as before | `IOFramebufferOpenGLIndex` (the new default makes it equal `IOFBDependentIndex`) |
| `rdna3cursorasync` | 1: the cursor moves on a thread call | 0: on WindowServer's thread, as before | cursor smoothness while flips run |
| `rdna3fbwake` | 1: the framebuffer does the family's wake work | 0: as before | the desktop after a system sleep (Apple menu > Sleep, then a key) |
| `rdna3vblonce` | 1 | 0: as before | never two VBL runs at once |
| `rdna3fbdtmap` | 0: as before (logged) | 1: our modes' timings accepted | the `InstallTiming ... FAILED` lines in the CoreDisplay log |
| `rdna3fbconsole` | 1: the framebuffer that holds the boot console | 0: head 0 | `isConsoleDevice` |
| `rdna3vblirq` (N42) | 1: VBL from the DCN vblank interrupt, flip done from the pflip interrupt, the OTG poll as fallback (needs `rdna3msi=1`, in the profile) | 0: the OTG poll alone, as before N42 | VBL timing and the present queue's completion; also `sudo sysctl debug.rdna3_vblirq=0` / `=1` at runtime (the framebuffers fall back within 3 frames) |
| `rdna3hpdirq` (N42) | 0: HPD interrupts counted and logged (`RDNA3,HPDIrqs`), the hotplug poll decides | 1: an HPD interrupt probes the connectors (B7) | what a cable pull / a DP monitor's standby does |

One run per value: take the step's line and add the boot-arg, e.g.

```sh
rdna3vm args "$(awk '$1 == "play-desktop" { $1 = ""; print }' tools/ladder-steps.txt) rdna3agdcvendor=1"
rdna3vm args "$(awk '$1 == "play" { $1 = ""; print }' tools/ladder-steps.txt) rdna3fbintegrated=1"
```

(the step's `@smbios=` / `@gpubus=` words come along). Cheapest order after
`play-desktop`: `rdna3agdcvendor=1` on `play`, then `rdna3fbintegrated=1`,
then `rdna3dispflags=1` and `rdna3glorder=0` (both only undo this round's
defaults). What decides it is the same pair of rows as above:
`present: page flip mode` and `present: transactions`. For `rdna3fbwake`, put
macOS to sleep once during a `play` run and wake it; the `fb N40, wake` row
must show as many family wakes as system sleeps and the desktop must come
back.

N42's A/B: one `play` run as the step is (`rdna3vblirq=1`) and one with
`rdna3vblirq=0` added. The triage rows `fb N40, vblank irq` and `N42, display
irq` of the first must be INFO with the interrupt counts growing with the
frames (no fallback, no storm) and the present queue's `flip done from the
pflip interrupt` rows present; the second shows `rdna3vblirq=0` rows. Compare
how smooth windows move in both.

N45's AGDC A/B (the answers after Navi48-MacOS, `agdc/rdna3_agdc_n48.h`):
`play` runs them all at their defaults (as before N45); each of these steps
is `play` with one switch changed, judged against a plain `play` run of the
same build:

| Step | Switch | What changes |
|---|---|---|
| `play-agdcib` | `rdna3agdcib=3` | 0x711 type 0x40 (inline buffer) answered, the other capability types zero-filled with success |
| `play-agdclink` | `rdna3agdclinkflags=1` | the 0x921 flags without bit 0 (0xa) |
| `play-agdcscaler` | `rdna3agdcscaler=1` | the plane scaler with AMD's markers (0x50005 / 4) |

`rdna3agdcib=1` (type 0x40 only, no zero fill) and `rdna3agdcscaler=2`
(Navi48's width-first entry) are `rdna3vm args` lines:

```sh
rdna3vm args "$(awk '$1 == "play" { $1 = ""; print }' tools/ladder-steps.txt) rdna3agdcib=1"
```

Run `play-agdcib` first. What decides it: the triage rows `AGDC N45` (the
switch values this boot ran, the per-type `asked/answered` text) and
`planes in transactions` (`iop_ok` > 0). The sysctls
(`sudo sysctl debug.rdna3agdc_ib=3`) count only from the next WindowServer
start, so a boot-arg step is the clean A/B.

### The A/B ladder (steps `ladder`, `ladder-ws` and `ladder-glyphs`)

DRIVER-PLAN N50 (broken glyphs in Terminal / Maps, tile-shaped blocks in Chess; card run 2026-10-25e). One run replaces five reports with a switch each:

```sh
rdna3vm step ladder       # the app rungs: about 29 minutes
rdna3vm step ladder-ws    # and the two WindowServer rungs: about 45 minutes, you log in after each restart
rdna3vm step ladder-glyphs     # the glyph hunt: 13 rungs, about 25 minutes (managed uploads whole / in pages, the undeclared-write detector, the shared window surface, page-off, framebuffer fetch, ...)
rdna3vm step ladder-coh        # the kernel coherence switches (DRIVER-PLAN N54): 10 rungs, about 17 minutes (baseline, all, then one sysctl debug.rdna3_coh_<name> each; docs/AFTER-TESTS.md "Kernel coherence switches")
```

The step is the `play` profile (the desktop on the Metal plugin) with `rdna3ladder=1` / `2` instead of `rdna3play=1`; no other test runs. Per rung
(`baseline`, `attrring` (DRIVER-PLAN N57: `NVMTL_ATTR_RING_BIG_PAGE=0`, RADV's attribute ring without BIG_PAGE), `radv`, `sync`, `managed`, `alignguard`, `paranoid`, `loadbarrier`) the guest opens Terminal (colour text, a tick line every second) and Chess as
new instances with that rung's `NVMTL_*` switches in their environment (`open -n -a APP --env ...`; the plugin reads the environment as well as `rdna3mtl.conf`),
takes a 320-wide and a hi-res (native size) scanout look, keeps the plugin log of those two processes, and quits them. `rdna3mtl.conf` is not touched by the app rungs.
Terminal's text is a colour listing, then a tick line a second in ONE fixed colour (bright white) ending in the same `abcdefghijklmnopqrstuvwxyz`, and every 4th second a
line of mixed glyphs (upper case, digits, punctuation). The Terminal is opened with `-ApplePersistenceIgnoreState YES`: without it the new instance restores the windows
the previous rung's Terminal had saved, which show the text of the rung before, static and drawn clean, next to the live one (card run 2026-10-25f: that is what looked
like a clean `paranoid` rung). The report's triage reads each rung's hi-res look and prints an automatic glyph verdict
([The whole ladder in one card run](AFTER-TESTS.md#the-whole-ladder-in-one-card-run-rdna3vm-step-ladder)).
In the guest's Terminal, by hand (a tty: it also asks y / n / Enter per rung):

```sh
sudo sh "/Library/Application Support/RDNA3/autoreport.sh" --standalone --ladder      # or --ladder-ws or --ladder-glyphs
```

`ladder-ws` edits `rdna3mtl.conf` and restarts WindowServer for `radv-ws` and `paranoid-ws`, and puts the conf back exactly (also on Ctrl-C and, after a dead run,
at the next start of `autoreport.sh`). The report: `41-ladder.txt`, `41-ladder-<rung>.log`, the looks `scanoutladder-<rung>[a|h]-pipe*.ppm`; the triage prints a row per
rung (docs/AFTER-TESTS.md, "The whole ladder in one card run"). The host tests are `tests/test_ladder_step.sh` and `tests/test_glyph_verdict.py`.

`ladder-glyphs` (`rdna3ladder=3`, `--ladder-glyphs`, 13 rungs of about 95 s) is the glyph hunt after card run 2026-10-25f, where NVMTL_PARANOID did not cure Terminal's text
(it replaced the earlier `ladder-paranoid`, which was for the day PARANOID did). Rungs, the strongest suspect first so that a run cut short keeps them: `baseline`;
`managed-whole` (`NVMTL_MANAGED_WHOLE=1`: every managed-buffer flush uploads the whole buffer, not the declared range); `managed-check` (`NVMTL_MANAGED_CHECK=1`: the undeclared-write
detector, no behaviour change); `no-surface-share` (the window is a private image, not the shared family VRAM); `no-surface-pageoff` (a GPU write is copied out eagerly, no VRAM-newest
mark); `fbfetch-zero` (framebuffer fetch returns zero); `managed-pages` (ranges widened to 4 KiB pages); `managed-eager-zero` (`=1` is the OLD behaviour: a new managed buffer is memset and
uploaded); `no-texbuf`; `no-bindless`; `dontcare-clears`; `skip-empty-0` (`NVMTL_SKIP_EMPTY=0`); `paranoid`, the NEGATIVE control. Each rung takes a second hi-res look three seconds after the
first (`...h2`): the glyph verdict only counts the tick-line window that CHANGED between them, the rung's own live text.
Apps are quit with SIGINT, then SIGTERM, then SIGKILL (no System Events, no Automation prompt); `41-ladder.txt` says how each instance ended.

### The app survey (step `apps`)

```sh
rdna3vm step apps         # about 25 minutes; nothing to answer, the apps open and close by themselves
```

The `play` profile with `rdna3ladder=5` (`autoreport.sh --apps` by hand, in the guest's Terminal): the default driver and plugin settings, no switch of any kind. One after another,
the guest opens Chess, Terminal (the tick text), TextEdit (an RTF), Calculator, Notes, Safari (stages: a text / CSS page, a canvas animation, a WebGL cube; local files the run writes),
Preview (a PDF and a JPEG), Maps, System Settings (appearance, wallpaper, displays), App Store, Photos, Image Capture, QuickTime Player (a 12 s movie the run carries), Mission Control
(with Calculator and TextEdit open) and Launchpad (`Apps` on macOS 26). It needs no network. Per app and stage it takes a 320-wide and a hi-res look of both pipes, then checks whether the
app is still running (a death is a crash when a report appeared), whether the Metal plugin is loaded in it, whether WindowServer kept its pid, collects its log slice, and quits it (SIGINT,
TERM, KILL; an instance you had open before the run is left alone). The report: `41-ladder.txt` (variant 5), `41-ladder-<app>.log`, `41-ladder-<app>-sys.log`,
`41-ladder-<app>-crash.log`, the looks `scanoutapps-<app>[-<stage>]a|h-pipe*.ppm`; the triage prints a row per app. What it cannot do headless: type or click in an app (System Events
needs Accessibility / Automation consent), record the screen (Screen Recording consent), sign in (App Store, Photos, Notes may show a welcome or sign-in sheet: that is recorded by the
look). Whether an app drew is for the pictures. How to read it: [AFTER-TESTS](AFTER-TESTS.md), "The app survey". The host tests are `tests/test_apps_step.sh` and
`tests/test_triage_apps.py`; the PDF and the movie the run carries are made by `tools/apps_assets.py`.

### Tests that already passed

The report does not run a long test again (scaled modes, gamma, the
Vulkan test and CTS, mtlprobe, M6, the PM selftest, soak) when it passed
before with the same builds of everything it tests: the kexts' source
hashes, the test tool, the Metal plugin and RADV/libdrm, and the step's
boot-args. Its saved result goes into the report marked `SKIPPED - passed
on <date>`, and the triage lists them. Any change to what a test uses runs
it again. To run everything regardless: add `rdna3nocache=1`, e.g.
`rdna3vm args "<the step's boot-args> rdna3nocache=1"`.

### A new framebuffer build: allow it once

OpenCore injects the core kext itself, but `RDNA3Framebuffer` and
`RDNA3Accelerator` load from macOS's own kext cache, and macOS takes a new
ad-hoc signed build only after you allowed it. After `rdna3vm prepare`
with a build that changed them (the reports say so: `waits for approval`):

```sh
rdna3vm approve
```

macOS starts in a window (the card and the screen stay with Linux) and
stays up; about 2 minutes after the login System Settings → Privacy &
Security opens by itself: scroll down, **Allow** next to the blocked
system software, enter the password, then **Not Now** at the restart
question and Apple menu → Shut Down. No **Allow** button there = already
allowed, just shut down. `rdna3vm approve --card` does the same with the
card passed through (the old way).

A queue that reaches a boot where macOS still runs the build allowed
before stops by itself instead of testing it (`rdna3vm queue status`:
`stopped before step ...: macOS runs an older framebuffer/accelerator`):
`rdna3vm approve`, then `rdna3vm queue` goes on with the same step.
Without that (builds before 2026-10-12) such a run shows the old
framebuffer in its fallback console mode: everything redrawn about once a
second, and before the fix of that mode black shown blue.

### A new build reaches macOS without the card

A step or queue whose bundle (`rdna3vm prepare` / `update`) the VM does
not have yet first boots macOS **headless, without the card**: Linux keeps
the card and the screen for those ~3 minutes (`== installing the new
bundle in macOS without the card` in the host log), macOS copies the new
kexts and the Metal plugin's libdrm/RADV, and shuts down. Then the step
starts with the card and already runs the new build: no install boot with
the card that stops and runs the step again. Only a kext that really
changed is installed again; a new framebuffer, accelerator or AGDC build
needs the **Allow** above, and `rdna3vm step` / `queue` then opens the
`rdna3vm approve` window by itself before the card step (Allow, Not Now,
Shut Down: the card step starts after the window closes). `rdna3vm
prepare` with the same build changes nothing in macOS. Should the headless
boot not confirm the install (10 min limit, `INSTALL_TIMEOUT_MIN` in the
config), the card step installs the bundle itself and runs once more, as
before.

### Several steps in one go

```sh
rdna3vm queue pending
```

runs the step(s) still waiting for the card - now one big step, `big4`:
everything left that only the card can test, in one boot, about 25 minutes.
It is `big3` (the default profile with amdgpu's exact sequence, user
queues, PM control, VCN, mode set, HDMI stream / format / audio endpoint,
display power) plus the output gamma, amdgpu's display-firmware reset, DC's
extra steps around each mode switch, monitor standby and a short system
sleep with the video engines on, and BACO last. What needs no card (builds,
the simulator, the UAPI and RADV on the loopback, the Metal translator's
tests) runs in CI on the self-hosted runners instead. Watch for: both
monitors go to standby and come back, macOS sleeps ~30 s and wakes by
itself, and at the very end BACO turns the chip off and on: **the screens
may stay dark after it** - every result is already on the host by then, so
just wait for the VM to stop (or use the [emergency exit](#emergency-exit)).
Not in it (they need hardware or hands this bench does not have): `dplink`,
`dsc`, `mst`, `uhbr`, `odm` (native DisplayPort monitors), `subvp` (a
120 Hz+ monitor), `hdmi20` (a 4K HDMI 2.0 monitor), `hotplug` (someone at
the cables); run them by name when you have that hardware. Before it,
`big2`: macOS boots once and the report itself runs everything
(DMA/GFX/Vulkan tests - buffers, a triangle, images, timeline semaphores -,
Metal when the plugin is installed, the idle-power experiment, three looks at what
each monitor shows, display sleep and wake, a switch through scaled
modes, a 2-minute soak, the hang test last), then shuts down (about 12
minutes). Nobody needs to watch the screens; both monitors connected.
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
| macOS does not boot with `CPU_MODEL="host"` | [back to the Skylake model](#the-vm-on-this-computers-real-cpu-cpu_modelhost-amd-hosts-driver-plan-n52): one `sed` line; send `serial-*.log`, `host-*.log` |

A computer without a login manager (booting to a console) also works. The script itself
stops `display-manager` and then starts it again.

## What to send me

```sh
rdna3vm result
```

It packs every report written since the last `step` / `args` / `queue`
started (plus a triage summary) into **one** file,
`~/Downloads/rdna3-result.tar.xz` (`.tar.gz` when the host has no `xz`), replaced each time it runs.
Send that file. It prints its size: a ladder run is a few MB as `.xz` (the looks of one run are the same desktop, xz stores it once; as gzip it would be
about 85 MB even with the looks as PNGs).

It also prints every test on its own line, `PASS` / `FAIL` / `WARN` /
`INFO` / `NOT RUN`, with the counts and the triage's verdict at the end
(the same list is saved in the file as `00-checklist.txt`). `WARN` is a
finding that needs reading (a lit pipe without TX lines, two pipes on one
surface, an underflow), `INFO` only describes the run (boot-args, power,
counters), `NOT RUN` is a test that gave no verdict (a sleeping display,
no present path). To print the list again later, without collecting
anything: `rdna3vm result --show`.

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
then `prepare`. The builds come from the owner's Forgejo
(`https://forgejo.voten.me`, repository `Voten641/RDNA3-Hackintosh`); the
repository is public, so no login is needed. A token is used when there is one
(`FORGEJO_TOKEN` in the environment, else the first line of
`~/.config/rdna3vm/forgejo-token`, else `FORGEJO_TOKEN=...` in the checkout's
git-ignored `.env`); it may only contain letters, digits, `_` and `-`, goes
only to an `https://` Forgejo and only as a request header, and when the
server refuses it (401) `rdna3vm` says which source it came from and asks
again without it. The three config keys (in `~/rdna3-vm/config`, or the
environment) are optional: `CI_FORGE=forgejo` (default) or `github` for the
old GitHub Actions setup (`gh auth login`, or a token with *Actions: read*
in `~/.config/rdna3vm/github-token`), `FORGEJO_URL`, `FORGEJO_REPO`. The same
holds for `rdna3vm cts` (the `rdna3-cts` artifact). When the build of the pulled commit is
still running, `update` shows its jobs live (each job's current step and
time, refreshed every 15 s) and carries on by itself once its `kext` job
(the artifact) finished, without waiting for the longer RADV/Metal jobs; a
failed build falls back to the last good one. Right after a push it looks
for the build for up to 2 minutes (not for `[skip ci]` or docs-only
commits). `rdna3vm update --no-wait` stops instead of waiting. By hand:

1. Download the new artifact (step C, item 1).
2. Run `rdna3vm prepare --artifact ~/Downloads/RDNA3Accel-kext.zip`.
3. Run `rdna3vm window` and in the macOS Terminal paste the same
   `curl ... | sudo sh` command as in step D. Then `sudo shutdown -h now`.
4. Go back to step E.

### The VM on this computer's real CPU (`CPU_MODEL="host"`, AMD hosts; DRIVER-PLAN N53)

By default the VM shows macOS an emulated Intel Skylake CPU. For the planned
bare-metal hackintosh on the same Ryzen the card can be tested with the real
CPU instead: QEMU's `-cpu host`, and OpenCore gets the AMD_Vanilla kernel
patches that every AMD hackintosh needs.

Switch (one line, nothing else to edit; `rdna3vm` changes the OpenCore image
itself before every boot):

```sh
sed -i 's/^CPU_MODEL=.*/CPU_MODEL="host"/' ~/rdna3-vm/config      # the real CPU
rdna3vm check                                                     # the "VM CPU" part must be green
rdna3vm window                                                    # first try: macOS in a window, the card stays with Linux
```

Back to the Intel model (the patches are removed and what they replaced
is restored the same way, no manual edit):

```sh
sed -i 's/^CPU_MODEL=.*/CPU_MODEL="Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt,check"/' ~/rdna3-vm/config
```

What `host` does:

- QEMU gets `-cpu host,kvm=on,-kvm-pv-unhalt,+invtsc,vmware-cpuid-freq=on,check`
  (more flags after `host,` in `CPU_MODEL` are appended). `-kvm-pv-unhalt` is
  required: without it macOS 26.4+ turns on paravirtual ticket locks and the
  first contended lock panics the VM on an AMD host (see the table below). The vendor stays AuthenticAMD:
  the patches expect it. `+invtsc` is the invariant TSC (macOS since Monterey
  panics when the host's TSC is not stable: `rdna3vm check` wants the host
  clocksource to be `tsc`). `ignore_msrs` is set by the VM start as before.
- The topology is `CPUS` vCPUs = `CPUS / CPU_THREADS` cores x `CPU_THREADS`
  threads (`CPU_THREADS` in the config, default 1; `+topoext` is added for
  2 or more, QEMU warns without it). The AMD core-count patch gets the same
  number of cores, so the topology macOS reads and the patch agree. With
  `CPUS=12` that is 12 cores / 12 threads, patch `0C`; the 5800X as it will
  be on bare metal is `CPUS=16` and `CPU_THREADS=2` (8 cores, patch `08`).
- Before every boot (`window`, `step`, the install boot) `vmtool.py plist
  --amd-cpu <cores>` writes 24 Kernel/Patch entries into the image's
  `config.plist` (their Comment starts with `rdna3vm-amd:`), sets
  `Kernel/Quirks/ProvideCurrentCpuInfo` and `Kernel/Emulate/DummyPowerManagement`
  on and the Cpuid1 spoof empty. What it replaced is kept in the key
  `#rdna3vm-amd-restore` of `Kernel` (OpenCore ignores `#` keys) and comes
  back with `--no-amd-cpu`, which every other `CPU_MODEL` runs. A second run changes
  nothing. The patch list is vendored in `tools/vm/data/` (source, commit,
  licence and sha256 in `amd_vanilla.SOURCE`; the patches are not downloaded).
  Only the kernel patches are used, not AMD_Vanilla's IOPCIFamily ones (the VM's
  chipset is QEMU's; the bare-metal EFI of `tools/baremetal/build_efi.py` needs the
  "Disable 10 bit tags" ones for the B550 chipset, `docs/BAREMETAL.md` part 6).
- **The CPU name in About This Mac** (RestrictEvents). macOS shows the Mac
  model's CPU ("8-Core Intel Xeon W" for MacBookPro16,1) whatever the VM has.
  In the same mode `rdna3vm` puts Acidanthera's RestrictEvents 1.1.6 and Lilu
  1.7.2 into OpenCore (`EFI/OC/Kexts` + `Kernel/Add`, Lilu first); RestrictEvents
  patches that string for a non-Intel CPU and takes the name from CPUID, so no
  boot-arg is needed (`revcpuname=` would override it; it is left unset). The
  two RELEASE zips are the only thing `rdna3vm` downloads for the VM: once, into
  `~/rdna3-vm/oc-kexts`, with the sha256 pinned in `tools/vm/vmtool.py` and
  `tools/vm/data/oc_kexts.SOURCE` (no network after that). The image's own Lilu
  is 1.6.8, which does not run on macOS 26; it is kept as
  `Lilu.kext.rdna3vm-orig` and put back, with RestrictEvents removed, by every
  run of another `CPU_MODEL` (or a host that is not AMD). A failed download is
  a `!! RestrictEvents is not injected` line in the terminal and `host-*.log`:
  the VM boots and only About This Mac keeps the Mac model's name. Check in
  macOS: About This Mac, and `kextstat | grep -i -e lilu -e restrict` (both
  listed; with `-liludbgall`/`-revdbg` a DEBUG build would log, the RELEASE one
  does not). **Needs the VM: not run yet.**
- The run says what it did: `VM CPU: ...` and `amd-cpu: on, 24 AMD_Vanilla
  kernel patches, core count 12 (0x0C)` in the terminal and in `host-*.log`,
  and the result file's `00-host.txt` has `vm-cpu:` lines (CPU, topology, the
  exact `-cpu` / `-smp`). In macOS: `sysctl machdep.cpu.brand_string
  machdep.cpu.core_count machdep.cpu.thread_count` shows the Ryzen and the
  counts above.

What a failed boot looks like (**needs the card / the VM: not run yet**).
`-v` is on in every step, so the window or `serial-*.log` shows it:

| What you see | Meaning |
|---|---|
| a restart loop at the Apple logo or right after `EXITBS:START`; a panic naming `cpuid_set_info`, `commpage`, `Non-monotonic time` or `thread_invoke` | an AMD patch did not match this macOS (Apple changed the kernel; macOS 26.4 needed a new one, which the vendored file has). Send `serial-*.log`, `host-*.log` and `00-host.txt`; the Darwin number is in the panic text. The fix is a newer AMD_Vanilla `patches.plist` (see `amd_vanilla.SOURCE`), not a change here |
| `Kernel trap ... type = 14=page fault`, `CR2` equal to `RIP`, `Error code: 0x...3`, a backtrace with `_copypv` (really an unnamed static function after it), `lck_ticket`, `IOCatalogue::findDrivers` or any other contended lock | macOS 26.4+ pv ticket locks: the kernel kicks a sleeping waiter with the Intel `VMCALL`; KVM on an AMD host rewrites it to `VMMCALL` in the guest's read-only kernel text and the write faults. Cause: QEMU exposed `kvm-pv-unhalt` (`-cpu host` does by default). `-kvm-pv-unhalt` must be in the `-cpu` line (`last-cpu` in `~/rdna3-vm`, `vm-cpu:` in `00-host.txt`); a `+kvm-pv-unhalt` in `CPU_MODEL` brings the panic back. Second switch inside macOS: boot-arg `pvticket=0` in `BASE_ARGS` of `~/rdna3-vm/config` |
| the log ends at `EXITBS:START` (no `EXITBS:END`, no kernel line) | the hang is inside `ExitBootServices` (OVMF / OpenCore), before macOS runs one instruction, so no kernel patch and no AMD_Vanilla entry can be the cause. Seen once with `CPUS=12`; `CPUS=8` (the 5800X's 8 cores, a power of two, the tested setting) reached the kernel. Use `CPUS=8`; for the full chip `CPUS=16 CPU_THREADS=2`. If a count other than 8 matters, send `serial-*.log` and `host-*.log` of the hang |
| a panic about the topology or a CPU count | `CPUS` / `CPU_THREADS` / core count disagree or macOS dislikes the number; try `CPUS=8` or `CPUS=16 CPU_THREADS=2` |
| `kernel_task` panics about the TSC or the clock | the host TSC; `dmesg \| grep -i -e tsc -e clocksource` on Linux, a power cycle of the computer helps sometimes |
| QEMU prints `warning: host doesn't support requested feature` | read the feature: `+invtsc` missing means the host reports no invariant TSC |

Going back is the sed line above, then any `rdna3vm window` or `step` (the
OpenCore image is cleaned at its start). If macOS does not boot even in
the Skylake model afterwards, copy the original again:
`qemu-img convert -O raw /mnt/data/OSX-KVM/OpenCore/OpenCore.qcow2 ~/rdna3-vm/OpenCore-rdna3.img`
and `rdna3vm prepare ...` once more (it re-applies SIP off and the kexts).

Does the patch list still fit a new macOS? Run it against the kernel the VM boots
(the files, read only: `kernel` = `/System/Library/Kernels/kernel`, the Boot KC =
`/System/Library/KernelCollections/BootKernelExtensions.kc`; Darwin = `uname -r`):

```sh
python3 tools/vm/ocpatchsim.py BootKernelExtensions.kc tools/vm/data/amd_vanilla-a174cca.plist 25.4.0
```

It applies the entries exactly as OpenCore 1.0.6 does (Base lookup, Find / Mask /
Replace / ReplaceMask, Count, Skip, Limit, Min/MaxKernel; `tests/test_ocpatchsim.py`) and
prints per entry the matches against its Count with function+offset, and `--avoid
START-END` (file offsets) flags a match inside a range. For Darwin 25.4.0 every
applicable entry matches its Count (the kernel and the Boot KC give the same sites)
except one: AMD_Vanilla's `Force CPUFAMILY_INTEL_PENRYN 11.3+` has the Base
`_cpuid_set_info ` with a trailing space, so OpenCore never applies it (same on bare
metal; macOS 26.4.1 and 26.7.1 boot reported with the same file). The quirks that run
before the list (`ProvideCurrentCpuInfo`, `DisableLinkeditJettison`) are not simulated.

What was checked on this computer (Ryzen 7 5800X, Zen 3, host kernel 7.2): QEMU 11.1.2 accepts
the `-cpu` and `-smp` lines (no warning; `threads=2` needs `+topoext`); the
OpenCore 1.0.6 image (`ocvalidate` of the same version: no issues on the
edited config and on the one switched back, which is byte for byte the
original); `make test` runs `tests/test_vm_amd_cpu.sh`. Not checked: that macOS
26.4.1 boots with these patches in a KVM guest. Known risks: a future Darwin
that breaks a patch; how macOS finds the TSC frequency in this guest
(the `vmware-cpuid-freq` leaf, or what OpenCore's `ProvideCurrentCpuInfo`
gives; OSX-KVM's flags were chosen for an Intel guest) is untested; the PAT patch is AMD_Vanilla's default (Algrey's), the
alternative of Shaneee changes how GPU memory is mapped and is off.

### macOS 26 (Tahoe) on a second disk

The same OpenCore boots either macOS disk: it injects the accelerator's
Sequoia build up to Darwin 24 and its Tahoe build (`RDNA3AcceleratorTahoe`,
from CI builds after 2026-10-12) from Darwin 25 on, and the VM installs the
one for its own Darwin into the Auxiliary KC. To run a step on macOS 26:
swap the disk file (`MACDISK` in `~/rdna3-vm/config`), `rdna3vm update`, the
first time `rdna3vm window` + the setup command of step D, then
`rdna3vm approve` and the steps as usual (`rdna3vm queue pending`). The
report's `R4 macOS` line says which Darwin ran and whether the accelerator
started there.

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

### Vulkan CTS subset (step cts)

With RADV in the VM, the report can run a fixed part of the Vulkan
conformance tests (`tools/cts/patterns.txt`, about 2360 cases, up to 45
minutes). The `deqp-vk` build comes from CI: Actions -> build -> Run
workflow with `cts` on (the first build takes about an hour on the macOS
runner, later ones minutes). Then, once per CTS build:

```sh
rdna3vm cts          # the newest rdna3-cts artifact into the VM's share
rdna3vm step cts
```

The report's `== Vulkan CTS subset` section has one line per group (pass,
fail, not supported) and the failing cases; a group that hangs is killed at
its time limit and named with the case it was in. Triage prints the total.

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
the Metal devices. Only the processes `rdna3mtl-allow.txt` allows (and `mtlprobe`) get the
device: WindowServer, and every application in apps mode (the `%apps` line;
`rdna3mtlapps=0` in the boot-args or no such line gives the explicit names
only). Everything else sees nothing new.

### Keeping the plugin and RADV current

`rdna3vm update` installs only the kext bundle. The
Metal plugin and a RADV built in the VM stay as they were built, so after a
`git pull` that touches `metal/` or `userspace/mesa/` they are older than the
kext the report tests. Both build scripts therefore write a stamp (the last
commit that touched `metal/`, resp. `userspace/mesa/`, in the sources they
built) and the report carries it in `35-build-stamps.txt`. When the triage
prints `FAIL STALE PLUGIN/RADV`, the plugin / RADV results of that run do
not test the current code: `git pull`, `rdna3vm window`, then in the VM
Terminal `build_radv.sh` and `build_metal.sh` again (the commands above),
shut the VM down and run the steps again. Reports from before the stamps
(and a RADV from a CI artifact) have none; for the plugin the triage then
looks for the `vk: VK_EXT_extended_dynamic_state` line in its log.

### A bigger BAR0 for the VM (optional)

`rdna3vm` gives the VM the card with BAR0 cut to 256 MiB. That window is
all the CPU can reach of the card's memory, and macOS's CPU-visible VRAM
heap is what is left of it (144 MiB): WindowServer's framebuffers, the
cursor and the swap copies share it with the buffers RADV maps. Card run
2026-10-17c ran it dry (the main display froze); the driver now keeps a
reserve of it for the display (`rdna3visreserve`, docs/INSTALL-OPENCORE.md)
and keeps buffers that need no CPU access out of it, so 256 MiB stays the
default.

What macOS takes (DRIVER-PLAN N62, from Apple's IOPCIFamily source and card
run 2026-10-28c): in a VM it keeps a PCI BAR only **below 4 GiB** (its PCI
configurator drops 64-bit placements when CPUID says "hypervisor"), and on
any machine it takes **no BAR bigger than 1 GiB**. Run 2026-10-28c had a
1 GiB BAR0 that OVMF put at 0x900000000 (its 64-bit window): macOS gave the
card no BAR0 at all, the kext could not map it, the bring-up stopped and
WindowServer never reached the GPU. So `rdna3vm` now gives the VM no 64-bit
PCI window for a BAR0 up to 1 GiB, and OVMF has to fit it below 4 GiB. To
try a larger window, put this in `~/rdna3-vm/config`:

```sh
RESIZE_BAR_LOG2=9      # 2^9 MiB = 512 MiB (about 400 MiB visible heap); 8 = 256 MiB, the default
```

and leave `PCI_MMIO64_MB` unset (or 0). The host log then says `OVMF 64-bit
PCI window: 0 MiB (BAR0 512 MiB)` and, in the guest's PCI bus at 8 and 40 s,
`BAR0: 64 bit prefetchable memory at 0x80000000 [0x9fffffff]` (below 4 GiB).
When that run is good, `RESIZE_BAR_LOG2=10` (1 GiB) is the next try: it fits
only if OVMF's 32-bit window has a free 1 GiB-aligned GiB, which q35 may not
have. Values above 10 are useless to macOS (and the host log says so).

What the report must show (triage row `VRAM aperture`, the kernel's line
`VRAM aperture: BAR0 512 MiB at phys 0x80000000, mapped 512 MiB (kernel
window, ...), visible heap N MiB`): the same BAR0 size as the host's
`BAR0 ... -> 512 MiB`, `mapped` equal to it, and `VISIBLE VRAM` with fewer or
no refusals. `VRAM aperture: NOT MAPPED: ...` names why (for example `the
firmware put it above 4 GiB`); a picture-less VM or an unconfigured card means
OVMF could not fit the BAR below 4 GiB: set `RESIZE_BAR_LOG2=8` again (or
delete the line) and send `host-*.log`. The kext maps at most 1 GiB of BAR0
into the kernel (`rdna3vramwin=<MiB>` lowers it); a map that fails is retried
with half the window down to 256 MiB, and the row then says WARN.

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
- `CPU_MODEL` decides the VM's CPU (an emulated Skylake by default, the real one with `host`);
  `rdna3vm` adjusts the OpenCore image for it before every boot (see above).
- The kernel log goes to the VM serial port (`serial=3`) into `serial-*.log`, and
  `debug=0x100` halts macOS on a panic instead of restarting.
