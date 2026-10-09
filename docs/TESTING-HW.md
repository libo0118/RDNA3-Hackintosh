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
| 1 | `rdna3lvl=1` | maps BAR5, waits for PSP, reads the VRAM size | `RDNA3,VRAMSizeMB = 16384`, no `MMIO reads all-ones` | low |
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
| 9 | same as 8 + `rdna3fb=1` | our framebuffer instead of IONDRV on the GOP mode | desktop unchanged, `RDNA3FB: ... EDID ... from AUX` in the log and the monitor name in System Settings → Displays | **the screen may go dark** |
| 10 | same as 9 + `rdna3accel=1` | IOAcceleratorFamily2 accelerator | `ioreg -c RDNA3Accelerator`, `populateAccelConfig ... what the FAMILY pre-filled` in the log | high: WindowServer |
| 11 | same as 10 | `vulkaninfo --summary` with RADV from `/Library/GPUBundles/rdna3mtl` | device `AMD Radeon RX 7800 XT (RADV NAVI32)` | medium |
| 12 | same as 10, then `sudo sysctl -w debug.rdna3accelfb=1` | Metal plugin for processes listed in `rdna3mtl-allow.txt` | `mtlprobe` sees the AMD device | high |

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
