# Hardware test: step-by-step ladder

A single procedure for a hackintosh with an RX 7800 XT. Do you have Linux and this card as
the only one? The same ladder runs in a virtual machine, automated:
[TESTING-VM](TESTING-VM.md). Each step adds one
thing, and after each one you collect a report with a single script. If a step hangs
the system or turns off the screen, go back to the previous one and send the report from
it together with a description of the symptom. Module details are in `STAGE1`–`STAGE7`.

## Before the first step

1. **A way back.** In OpenCore keep a second entry or a second copy of the EFI in
   which the RDNA3 kexts are disabled. The boot-arg `-rdna3off` also disables
   everything. Check once that you can edit boot-args in the picker (space,
   then the entry with boot-args, or `Cmd+V` for verbose mode).
2. **Remote access.** System Settings → General → Sharing → Remote
   Login (SSH). When the screen goes dark, you collect the report over SSH from another
   computer.
3. **BIOS.** CSM disabled, Above 4G Decoding enabled. If you have trouble with
   Resizable BAR: disable it or set `ResizeAppleGpuBars = -1` in OpenCore.
4. **Build and install.** On the Mac, in the repository directory:

   ```sh
   make kext && make -C userspace && make tools
   make -C accel && make -C fb          # accelerator and framebuffer (from step 9)
   sudo package/install.sh --fw /path/to/linux-firmware/amdgpu
   ```

   Firmware is not in the repository (`docs/FIRMWARE.md`). The installer
   rebuilds the Auxiliary Kernel Collection; a restart is required.

   **Through OpenCore instead (recommended):** kernelmanagerd may refuse
   ad-hoc signed kexts in the Auxiliary KC ("invalid codeful kernel
   extension", seen in the VM). Then let OpenCore inject them:

   ```sh
   sudo diskutil mount disk0s1                   # your EFI partition (diskutil list)
   sudo package/oc_install.sh --efi /Volumes/EFI --fw /path/to/linux-firmware/amdgpu
   sudo package/oc_install.sh --efi /Volumes/EFI --step 2     # boot-args of a step
   sudo package/oc_install.sh --efi /Volumes/EFI --off        # everything off again
   ```

   It copies the kexts to `EFI/OC/Kexts`, adds them to `Kernel/Add`, puts
   the firmware in `/Library/Application Support/RDNA3/firmware`, and sets
   the step's boot-args in `config.plist` (yours other than `rdna3*` stay;
   a backup `config.plist.rdna3-<date>` is written first). With this route
   skip step 5 below: `--step N` does it.
5. **Set boot-args once per step**, for example:

   ```sh
   sudo nvram boot-args="-v keepsyms=1 rdna3lvl=2"
   ```

   Keep your existing boot-args and append the ones from the table.
6. **Report after each step** (after the restart, from a normal terminal or SSH):

   ```sh
   tools/macos/collect.sh step3
   ```

   The script changes nothing. It creates `~/rdna3-report-step3-<date>.tar.gz`
   with logs, IORegistry, boot-args and a panic report, if any.

## The ladder

| Step | Boot-args (append) | What it does | Check (`11-ioreg-props.txt`, `20-log-rdna3.txt`) | Risk |
|---|---|---|---|---|
| 1 | `rdna3lvl=1` | maps BAR5, waits for PSP, reads the VRAM size | `RDNA3,VRAMSizeMB` = Linux's `mem_info_vram_total` (16368 on the RX 7800 XT), no `MMIO reads all-ones` | low |
| 2 | `rdna3lvl=2` | IP discovery table, reading DCN heads, VBIOS, display paths, DP AUX/EDID | `IP discovery ... ok`, GC 11.0.3; `RDNA3,DisplayHeads = 1`; `RDNA3,DisplayPath0` (e.g. `DP-1 UNIPHYA DDC1 HPD1`); `RDNA3,Connector0,Monitor` = monitor name; `RDNA3,Head0EdidMode`; `RDNA3,AtomCommandTables` | low: writes only to read windows and AUX registers |
| 3 | `rdna3lvl=3` | first GC register from discovery | `GRBM_STATUS`, `RDNA3,GBAddrConfig`, `RDNA3,ActiveCUs = 60` | low |
| 4 | `rdna3lvl=4` | PSP: bootloader, sOS, TMR, loading all firmware | `RDNA3,Stage2 = PSP up, firmware loaded`, `sOS running`, 29× `LOAD_IP_FW ... ok`, `RLC autoload started` | medium: first writes to PSP |
| 5 | `rdna3lvl=5` | SMU: PMFW, VBIOS, PPTable, safe features, metrics | `RDNA3,SMUVersion`, `RDNA3,PM`, `RDNA3,PMFeaturesRunning`, `RDNA3,PMTempHotspotC`, `RDNA3,PMFanRPM` | medium: clocks and fan |
| 5b | `rdna3lvl=5 rdna3dmub=1` | DMCUB: stopping the firmware started by the VBIOS, windows, restart, command mailbox | `RDNA3,DMUB = running, mailbox up`, `RDNA3,DMUBVersion = 0x07003300` (or close), `RDNA3,DMUBBootStatus` with bits 0 and 1 | **the screen may flicker or go dark**; then SSH, report, step without `rdna3dmub` |
| 6 | `rdna3lvl=6` | GMC (GART, MMHUB), IH ring, doorbells | `RDNA3,GMCResult = 0`, `RDNA3,IHResult = 0`, `RDNA3,Doorbells` | medium |
| 6b | `rdna3lvl=6 rdna3msi=1` | MSI interrupts instead of polling | `RDNA3,IRQs` increases | low |
| 7 | `rdna3lvl=7 rdna3msi=1` | SDMA0: ring test, 64 KiB VRAM→VRAM copy | `RDNA3,SDMAResult = 0`, `RDNA3,SDMACopyMismatches = 0`, `RDNA3,MSIVerified = 1` | medium: **first commands executed by the GPU** |
| 7b | same as 7 | from a terminal: `build/userspace/rdna3_drminfo --dma-test` | `DMA job results verified (WRITE_LINEAR + CONST_FILL)` | medium |
| 8 | `rdna3lvl=8 rdna3msi=1` | GFX 11: RLC, RS64, GFX ring, KIQ, KCQ | `RDNA3,GFXResult = 0`, `RDNA3,KIQResult = 0`, `RDNA3,KCQResult = 0` | medium |
| 8b | same as 8 | `build/userspace/rdna3_drminfo --gfx-test` | `GFX job results verified (PM4 WRITE_DATA)` | medium |
| 8c | same as 8 + `rdna3mes=1` | MES scheduler pipe (mes_v11_0_hw_init): mapped by the KIQ, SET_HW_RESOURCES, status query; needed later for user queues and queue resets | `RDNA3,MESResult = 0`, `RDNA3,MESSchedVersion` (firmware), log `MES: scheduler pipe up`; 8b still passes | low: the kernel rings stay on the KIQ |
| 8g | same as 8 + `rdna3cg=1` (default since run 8) | GFX clock gating (gfx_v11_0_update_gfx_clock_gating: CGCG, CGLS, MGCG, repeater FGCG, in RLC safe mode) so the GFX clocks stop when idle | `RDNA3,GfxCGResult = 0`; the `PM:` log lines: activity well below 100 %, gfxclk and power down at idle (step 8: ~200 W, 2.7 GHz); DMA/GFX tests still pass | low: an idle GFX that does not wake up would fail the GFX test |
| 8o | same as 8 + `rdna3gfxoff=1` | GFXOFF (amdgpu_gfx_off_ctrl: AllowGfxOff 100 ms after the last DRM ioctl, DisallowGfxOff before the next): the GFX powers off when idle | `RDNA3,GfxOffResult = 0`, log `GFXOFF: AllowGfxOff -> 0` and `PM: GFX is off`; the `PM:` lines below step 8g's ~75 W; DMA/GFX tests still pass | medium: a GC register touched while the GFX is off hangs the access |
| 8u | same as 8 + `rdna3pm=2` | UCLK/FCLK not pinned at the top: the PMFW runs memory/fabric DPM as under Linux (idle there: UCLK 96 MHz, FCLK 601 MHz, ~12 W with both monitors) | `PM:` lines: uclk below 1218 MHz and fewer watts than step 8; no `UCLK pinned` line | medium: a memory clock switch under the GOP scanout can flicker or blank the screen for a frame |
| 9 | same as 8 + `rdna3fb=1` | our framebuffer instead of IONDRV on the GOP mode (the monitor the GOP lit; a second one stays dark until 9b) | desktop unchanged at the GOP resolution, `RDNA3-fb: started`, `RDNA3FB: head 0 EDID ... bytes from connector AUXn` in `20-log-rdna3.txt` (the serial log interleaves lines), `RDNA3,Head0,Route` names the connector, the monitor name in System Settings → Displays | **the screen may go dark** |
| 5c | `rdna3lvl=5 rdna3dmub=3` | attach to the DMCUB the GOP left running (its inbox, no reset, the PSP does not load DMCUB): the way around 5b | `RDNA3,DMUB = attached, mailbox up`, log `DMUB attach: the GOP's firmware ... behind CWn` | low |
| 9b | same as 9 + `rdna3dmub=1 rdna3mirror=1` (only after 5b passed; with 5c instead: `rdna3dmub=3`, `rdna3vm step 9b3`) | the second TMDS monitor shows the boot head's picture: a free pipe gets a copy of the lit one, pixel clock / encoder / transmitter through the DMCUB (`hw/dcn32_clone.c`) | `RDNA3,Mirror` names the connector, pipe and OTG; `RDNA3,MirrorResult = 0`; the log line `clone: pipe N lit`; both monitors show the desktop | **either screen may go dark** |
| 9c | same as 9b but `rdna3mirror=2` | extended desktop: the cloned head gets its own framebuffer (RDNA3DisplayHead nub) and surface | two displays in System Settings → Displays, `RDNA3-fb: head 1: own surface ...`, `RDNA3,ExtendHead = 1` | **either screen may go dark** |
| 9cu | same as 9c + `rdna3pm=2` | extended desktop with UCLK/FCLK DPM free (the clone forces the DCN p-state allow) | as 9c, plus lower `PM:` watts; **watch both screens for flicker** | either screen may flicker or go dark |
| 9d | 9b or 9c, then unplug and replug the second monitor's cable (needs `rdna3msi=1`) | the clone follows the cable through the HPD interrupt | log `the cloned monitor is gone: switching its pipe off`, then `mirror: ...` again on replug | medium |
| hotplug | the `metal` step + `rdna3hotplug=1` (`rdna3vm step hotplug`); **stay at the machine**: when the progress window says `unplug and replug the second monitor now`, pull the second monitor's cable, wait ~10 s, put it back | hotplug after boot (`kext/src/RDNA3Hotplug.cpp`, `hw/dchpd.c`): the HPD toggle filter as amdgpu's `program_hpd_filter` sets it, a 500 ms poll of `DC_HPD_SENSE_DELAYED` (a change counts after two equal looks), the EDID read again, the second monitor's pipe taken down and lit again with its cable, the framebuffer's connect change to IOGraphics | the report's `hotplug window` lines: `RDNA3,HPDState` with the pin going `0` then `1`; log `hotplug: HPDn disconnected ... take the second monitor's pipe down`, then `HPDn connected ... TMDS sink with EDID: light the second monitor` and `second monitor lit`; `RDNA3,HotplugClonesOff` = `RDNA3,HotplugClonesLit` = 1; macOS drops the second display and brings it back; the `look hotplug` pictures show both pipes lit | medium: the second screen may stay dark, WindowServer may restart when its display goes |
| dplink | the `modeset` step + `rdna3dplink=1` (`rdna3vm step dplink`); **needs a monitor on native DisplayPort** (no DP-to-HDMI/DVI adaptor or cable) that the GOP lights at boot | the driver's own DP SST mode set (`hw/dcn32_dp.c`, ported from DC's link training): the sink's DPCD caps, the link cap verified once by training at the sink's maximum with fallback (`dp_verify_link_cap`), the smallest link setting for the mode, 8b/10b clock recovery and channel equalization through the DMCUB's TransmitterControl (ENABLE, SETUP_VSEMPH), the DP DTO as pixel clock, MSA, M/N, unblank | log `dp: AUX n: DPCD 1.x, sink ...`, `dp: link training HBR2x4 pass VS=.., PE=..`, `dp: AUX n: verified link cap ...`, `dp: DIG f -> UNIPHY b: WxH ... trained`; ioreg `RDNA3,Head<n>,DPLink` (verified and running link, last result 0 = pass); `rdna3_modetest` switches the DP monitor and back like the TMDS ones | high: the DP monitor may show "no signal" until the old mode is back (a failed training puts the old timing back and trains again) |
| 10 | same as 9 + `rdna3accel=1` | IOAcceleratorFamily2 accelerator | `ioreg -c RDNA3Accelerator`, `populateAccelConfig ... what the FAMILY pre-filled` in the log | high: WindowServer |
| 10s | same as 10 + `rdna3accelfb=3 rdna3acceliop=1 rdna3pm=2` | the accelerator's display machine gets head 0 and its pipe scans out what WindowServer composites (the sysctls `debug.rdna3accelfb=3` / `debug.rdna3accel_iop=1`, set from boot-args when the framebuffer publishes) | the desktop on head 0, log `boot-arg rdna3accelfb=3`, `iop: ... transaction hooks OPEN`, `debug.rdna3accel_iop_ok` counting | high: WindowServer may hang or show a black desktop |
| 10a | same as 10s but `rdna3accelfb=4` | also the aperture gate: WindowServer gets CPU mappings (getApertureMemoryDescriptor) of the surfaces it composites into | non-empty `SWAP PROBE` sums, the desktop on head 0, `23-windowserver-crash.txt` empty | high: WindowServer crash loop (black screen) |
| 10m | 9c + 10a: `rdna3dmub=1 rdna3mirror=2 rdna3accel=1 rdna3accelfb=4 rdna3acceliop=1 rdna3pm=2` | extended desktop through the accelerator: each head's framebuffer gets its gates when it publishes | the desktop on both monitors, `FRONT SURFACE ... LIVE WRITER` for both heads, `debug.rdna3accel_iop_ok_h1` counting | high: head 1 dark or a WindowServer crash loop |
| big | `rdna3lvl=8 rdna3msi=1 rdna3fb=1 rdna3dmub=1 rdna3mirror=2 rdna3accel=1 rdna3accelfb=4 rdna3acceliop=1 rdna3pm=2` | everything at once (one boot instead of ten): DMA/GFX tests, Vulkan, UCLK/FCLK DPM, extended desktop, the desktop composited through the accelerator on both heads | triage: tests PASS, `swap: surface ... -> head 0/1`, `FLIPPED head 0/1`, `LIVE WRITER`; the desktop on both monitors | high: any of the above |
| 11 | same as 10 | `vulkaninfo --summary` with RADV from `/Library/GPUBundles/rdna3mtl` | device `AMD Radeon RX 7800 XT (RADV NAVI32)` | medium |
| 12 | same as 10, then `sudo sysctl -w debug.rdna3accelfb=1` | Metal plugin for processes listed in `rdna3mtl-allow.txt` | `mtlprobe` sees the AMD device | high |
| sleep | step `metal` + `rdna3dpms=2 rdna3resume=1` | display sleep takes the link down on every TMDS monitor (standby, no signal) and back (`hw/dcn32_state.c`); then `pmset relative wake 30; pmset sleepnow`: the heads saved and taken down, on the wake a power loss detected (PCI, `RCC_CONFIG_MEMSIZE`, ASIC_Init flag, PSP sOS) and, if so, the GPU posted and brought up again, the heads restored from the snapshot | `RDNA3,Head<n>,State` (`intact` / `restored (0), running`), `RDNA3,Wake` (what the wake found), `RDNA3,WakeHeadsLost = 0`, `RDNA3,WakeResumeResult = 1` after a power loss; report: `SYSTEM SLEEP: slept and woke, every head back`, the DMA/GFX tests after the wake pass | high: a monitor may stay dark after the wake; the rings may be dead after a power loss |

Sleep: from level 4 on the kext keeps macOS from idle sleep
(`RDNA3,IdleSleepBlocked = 1`): there is no GPU suspend/resume yet, and
after a sleep the firmware and the GOP's picture are gone. Do not put the
machine to sleep by hand during tests; `rdna3sleep=1` lifts the block.
`rdna3resume=1` (step `sleep`) is the first try at a resume: the display
state is saved and restored, and a GPU that lost its power is posted and
brought up again (see `docs/DRIVER-PLAN.md`, D4, for what is missing).

Sensors: once the SMU is up (level 6 and higher) `RDNA3Accel` publishes
`PerformanceStatistics` every 2 s, with the keys AMD's macOS drivers use
(`Temperature(C)`, `Junction Temperature(C)`, `Fan Speed(RPM)`,
`Core Clock(MHz)`, `Memory Clock(MHz)`, `Total Power(W)`,
`Device Utilization %`). Check with
`ioreg -rc RDNA3Accel -d1 | grep -A9 PerformanceStatistics`; the values
must change under load and the temperature must look plausible (30-90).

Rule: **do not move on until a step gives the expected values.**
When a result differs from the table, send the report. That is exactly the
information that cannot be obtained in the simulator.

## When something goes wrong

| Symptom | What to do |
|---|---|
| hang at boot on step N | restart with the boot-args of step N−1 (OpenCore picker) and a report from that boot; describe at which point of the verbose log it stopped |
| kernel panic | after the restart `collect.sh` attaches the two latest panic reports by itself |
| black screen, system alive | SSH, `tools/macos/collect.sh screen`, then `sudo nvram boot-args="..."` without the last flag and `sudo reboot` |
| no `RDNA3` entries at all in the log | the kext did not load: `03-kexts.txt` and `kmutil showloaded`; check that the installer rebuilt the collection and that SIP allows kexts from `/Library/Extensions` |
| `RDNA3,PSPResult` other than 0 | firmware is missing from `RDNA3Accel.kext/Contents/Resources` or it is a different version than expected; send the report |

## What cannot be checked without the card

The simulator models the behaviour from the Linux code, but it does not know how the card
will really behave. The first test will tell us above all:

* whether PSP accepts our firmware loading order (step 4);
* whether PMFW accepts the BoardTable from the VBIOS and the safe feature set (step 5);
* whether resetting DMCUB under a live GOP image is safe (step 5b);
* whether the GPU executes our SDMA and PM4 packets (steps 7 and 8);
* how IOAcceleratorFamily2 fills `IOAccelConfig` before us (step 10).

Further modeset work (DML, pixel clock, DP training) starts after
steps 2, 5b and 9, because it needs real DCN register values.

## Bare metal with the installer (no VM harness)

For a hackintosh where the driver is installed with the `.pkg` (or the
tarball's `install.sh`) instead of a build tree:
[INSTALL-OPENCORE](INSTALL-OPENCORE.md) covers the package, the OpenCore
settings and the way back. Everything below runs from
`/Library/Application Support/RDNA3` (written `$R` here); nothing needs the
repository or a Linux host.

1. **Way back first** ([INSTALL-OPENCORE §8](INSTALL-OPENCORE.md#8-the-way-back)):
   a USB copy of the EFI with `RDNA3Accel.kext` disabled and `-rdna3off`.
   SSH on (Remote Login), so a dark screen still gives a report.
2. **Install:** the `.pkg`, then
   `sudo "$R/oc_install.sh" --efi /Volumes/EFI` (EFI mounted) for the
   OpenCore entry of the core kext. Check the security settings of
   INSTALL-OPENCORE §4 (`csr-active-config`, `SecureBootModel`, `amfi=0x80`).
   Restart, **Allow** in Privacy & Security if asked, restart again.
3. **Boot with the default profile:** no `rdna3` boot-args at all. Expected:
   the desktop on the RX 7800 XT at the GOP resolution, a second monitor as
   an extended display, `ioreg -rc RDNA3Accel -d1 | grep -E 'Stage2|GFXResult|PMFeatures'`
   shows the bring-up done.
4. **Report** (after the desktop is up, from Terminal or SSH):

   ```sh
   sudo "/Library/Application Support/RDNA3/autoreport.sh" --standalone
   ```

   The same tests and report the VM runs after each boot (`tools/vm/macos/autoreport.sh`):
   scanout pictures, display sleep and wake, scaled mode switches, the DMA
   and GFX tests, Vulkan and Metal when installed, then `collect.sh`. It
   takes 3 to 8 minutes, a Terminal window shows the progress, and the files
   land in `/Users/Shared/rdna3-reports/`: `rdna3-report-<label>-<date>.tar.gz`,
   `…-tests.txt` and `…-scanout*.ppm`. Send all of them; they are what
   [AFTER-TESTS](AFTER-TESTS.md) expects from the VM.
   Only the collection, without running any test:
   `"$R/tools/macos/collect.sh" <label>` (writes `~/rdna3-report-<label>-<date>.tar.gz`).
5. **A ladder step instead of the profile**, when the default profile fails:
   `sudo "$R/oc_install.sh" --efi /Volumes/EFI --step 2` (any step of the
   table above; `--step default` goes back to no `rdna3` boot-args), restart,
   report as in 4. Walk up from the last step that worked.
6. **Roll back:** `--off` (sets `-rdna3off`), the USB EFI, or from Recovery
   as INSTALL-OPENCORE §8 describes; `sudo "$R/uninstall.sh"` removes the install.

A dark screen with the system alive: SSH in, run step 4 there, then
`--off` and restart.
