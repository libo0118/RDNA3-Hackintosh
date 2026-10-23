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
   sudo package/oc_install.sh --efi /Volumes/EFI --off        # everything off again (-rdna3off)
   sudo package/oc_install.sh --efi /Volumes/EFI --remove     # our kexts, entries and boot-args out of OpenCore
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
| userq | same as 8c + `rdna3userq=1` (only after 8c passed) | user queues (DRM_AMDGPU_USERQ / _SIGNAL / _WAIT, amdgpu.user_queue=1): MES ADD_QUEUE / REMOVE_QUEUE, doorbell BOs, seq64 fences; then `build/userspace/rdna3_drminfo --userq-test` runs a compute queue from its own doorbell | `RDNA3,UserQueues` = the IP mask (7: GFX, compute, SDMA; 0 means the firmware is too old: ME 2390, PFP 2530, MEC 2600, MES 120, SDMA 25), log `user queue: ADD_QUEUE ... : 0`, `USERQ TEST: PASSED`; 8b still passes | low: off by default, kernel VMs keep VMIDs 1-7 |
| vcn | same as 8 + `rdna3vcn=1` (`rdna3vm step vcn`) | VCN 4.0 + JPEG 4.0 (`hw/vcn40.c`, ported from vcn_v4_0.c / jpeg_v4_0.c): the PSP also loads `vcn_4_0_0.bin` for VCN1, both instances start (PowerUpVcn, static power gating off, VCPU boot from the TMR image, unified ring with the fw_shared handshake), ring tests, the IB tests (encode session create/close, decode create/destroy message), the JPEG ring and its IB test; then `rdna3_drminfo --vcn-test` reads HW_IP_INFO / VIDEO_CAPS and runs a job on each video ring from a client VM; 1 s idle gates the block again (PowerDownVcn) | `RDNA3,VCN = VCN0 VCN1 JPEG up`, `RDNA3,VCNResult = 0`, `RDNA3,VCN0IBTest = 0`, `RDNA3,VCN1IBTest = 0`, `RDNA3,JPEGResult = 0`, `RDNA3,VCNFirmware = 0x09118022` (ENC 1.24 DEC 9), log `VCN0: unified ring ... ring test passed`, `VCN TEST: PASSED`, later `RDNA3,VCNGated = 1`; DMA/GFX tests still pass | medium: first VCN and JPEG register writes; a VCPU that does not boot costs ~10 s per instance and leaves the rings off |
| vcn2 | same as 8 + `rdna3vcn=2` (`rdna3vm step vcn2`) | step vcn with dynamic power gating as amdgpu runs it on this card (`vcn40_start_dpg_mode`, from vcn_v4_0_start_dpg_mode): per instance UVD_POWER_STATUS goes to DPG mode, the register image (VCPU clock, LMI, MPC, cache windows, master interrupt) is written into the DPG SRAM buffer and loaded by the PSP (`LOAD_IP_FW` GFX_FW_TYPE_VCN0_RAM / VCN1_RAM), then the unified ring with the fw_shared handshake; the idle gate is `vcn_v4_0_stop_dpg_mode` (PG mode off, then PowerDownVcn); every new job loads the SRAM image again. `rdna3vcn=3` (by hand) does the same through UVD_DPG_LMA_CTL/DATA without the PSP | as step vcn, plus `RDNA3,VCNPowerGating = DPG, SRAM through the PSP`, `RDNA3,VCNDPGStarts` >= 2, `RDNA3,VCNSRAMUploads` = `VCNDPGStarts`, `RDNA3,VCNSRAMUploadErrors = 0`, no `PSP: LOAD_IP_FW type 49/50 ... FAILED` line | medium: the PSP may refuse the SRAM image (then try `rdna3vcn=3`); a VCPU that does not boot leaves the rings off |
| 8g | same as 8 + `rdna3cg=1` (default since run 8) | GFX clock gating (gfx_v11_0_update_gfx_clock_gating: CGCG, CGLS, MGCG, repeater FGCG, in RLC safe mode) so the GFX clocks stop when idle | `RDNA3,GfxCGResult = 0`; the `PM:` log lines: activity well below 100 %, gfxclk and power down at idle (step 8: ~200 W, 2.7 GHz); DMA/GFX tests still pass | low: an idle GFX that does not wake up would fail the GFX test |
| 8o | same as 8 + `rdna3gfxoff=1` | GFXOFF (amdgpu_gfx_off_ctrl: AllowGfxOff 100 ms after the last DRM ioctl, DisallowGfxOff before the next): the GFX powers off when idle | `RDNA3,GfxOffResult = 0`, log `GFXOFF: AllowGfxOff -> 0` and `PM: GFX is off`; the `PM:` lines below step 8g's ~75 W; DMA/GFX tests still pass | medium: a GC register touched while the GFX is off hangs the access |
| 8u | same as 8 + `rdna3pm=2` | UCLK/FCLK not pinned at the top: the PMFW runs memory/fabric DPM as under Linux (idle there: UCLK 96 MHz, FCLK 601 MHz, ~12 W with both monitors) | `PM:` lines: uclk below 1218 MHz and fewer watts than step 8; no `UCLK pinned` line; the `rdna3 memory load` section (DRIVER-PLAN N43): under the 10 s fill/copy load `pp_dpm_mclk` leaves level 0 (456 / 772 / 1218 MHz, `umc` activity high), 4 s after it UCLK is back at 96 MHz | medium: a memory clock switch under the GOP scanout can flicker or blank the screen for a frame |
| pmctl | same as 8u + `rdna3pmctl=2` | power management from userspace (`build/userspace/rdna3_pmctl`, RDNA3Accel selector 1): fan mode and speed (smu_v13_0_set_fan_control_mode / _set_fan_speed_pwm), power limit (smu_v13_0_0_set_power_limit), overdrive table (smu_v13_0_0_od_edit_dpm_table: GFXCLK/UCLK limits, voltage offset, fan curve, zero RPM), power profiles (activity monitor table), pp_dpm clock levels, DF C-states; `rdna3_pmctl selftest` sets each once and back | `PMCTL: ALL PASSED`, `RDNA3,PMODResult = 0`, `RDNA3,PMPowerLimitMinW/MaxW` and `RDNA3,PMOD*` published, log `PM control: op ...` lines; the DMA/GFX tests still pass | medium: the fan stops following the temperature for ~4 s (manual PWM 50 %), the power limit goes 10 W down and back, GFXCLK max 100 MHz down and back |
| baco | `rdna3lvl=8 rdna3msi=1 rdna3pm=2 rdna3pmctl=1 rdna3baco=1` (only after pmctl passed) | `rdna3_pmctl baco-cycle`: BACO enter + exit through the PMFW (smu_v13_0_baco_enter / _exit), then levels 4-8 again as after a mode-1 reset | `BACO: cycle done`, `RDNA3,PMBACOResult = 0`, `RDNA3,PMBACOCycles = 1`, log `BACO cycle #1 done, rings back up`; the DMA/GFX tests after it pass | **high**: the chip powers off; a failed exit needs a reboot (the display goes dark during the cycle) |
| 9 | same as 8 + `rdna3fb=1` | our framebuffer instead of IONDRV on the GOP mode (the monitor the GOP lit; a second one stays dark until 9b) | desktop unchanged at the GOP resolution, `RDNA3-fb: started`, `RDNA3FB: head 0 EDID ... bytes from connector AUXn` in `20-log-rdna3.txt` (the serial log interleaves lines), `RDNA3,Head0,Route` names the connector, the monitor name in System Settings → Displays | **the screen may go dark** |
| 5c | `rdna3lvl=5 rdna3dmub=3` | attach to the DMCUB the GOP left running (its inbox, no reset, the PSP does not load DMCUB): the way around 5b | `RDNA3,DMUB = attached, mailbox up`, log `DMUB attach: the GOP's firmware ... behind CWn` | low |
| 9b | same as 9 + `rdna3dmub=1 rdna3mirror=1` (only after 5b passed; with 5c instead: `rdna3dmub=3`, `rdna3vm step 9b3`) | the second TMDS monitor shows the boot head's picture: a free pipe gets a copy of the lit one, pixel clock / encoder / transmitter through the DMCUB (`hw/dcn32_clone.c`) | `RDNA3,Mirror` names the connector, pipe and OTG; `RDNA3,MirrorResult = 0`; the log line `clone: pipe N lit`; both monitors show the desktop | **either screen may go dark** |
| 9c | same as 9b but `rdna3mirror=2` | extended desktop: the cloned head gets its own framebuffer (RDNA3DisplayHead nub) and surface | two displays in System Settings → Displays, `RDNA3-fb: head 1: own surface ...`, `RDNA3,ExtendHead = 1` | **either screen may go dark** |
| 9cu | same as 9c + `rdna3pm=2` | extended desktop with UCLK/FCLK DPM free (the clone forces the DCN p-state allow) | as 9c, plus lower `PM:` watts; **watch both screens for flicker** | either screen may flicker or go dark |
| 9d | 9b or 9c, then unplug and replug the second monitor's cable (needs `rdna3msi=1`) | the clone follows the cable through the HPD interrupt | log `the cloned monitor is gone: switching its pipe off`, then `mirror: ...` again on replug | medium |
| hotplug | the `metal` step + `rdna3hotplug=1` (`rdna3vm step hotplug`); **stay at the machine**: when the progress window says `unplug and replug the second monitor now`, pull the second monitor's cable, wait ~10 s, put it back | hotplug after boot (`kext/src/RDNA3Hotplug.cpp`, `hw/dchpd.c`): the HPD toggle filter as amdgpu's `program_hpd_filter` sets it, a 500 ms poll of `DC_HPD_SENSE_DELAYED` (a change counts after two equal looks), the EDID read again, the second monitor's pipe taken down and lit again with its cable, the framebuffer's connect change to IOGraphics | the report's `hotplug window` lines: `RDNA3,HPDState` with the pin going `0` then `1`; log `hotplug: HPDn disconnected ... take the second monitor's pipe down`, then `HPDn connected ... TMDS sink with EDID: light the second monitor` and `second monitor lit`; `RDNA3,HotplugClonesOff` = `RDNA3,HotplugClonesLit` = 1; macOS drops the second display and brings it back; the `look hotplug` pictures show both pipes lit | medium: the second screen may stay dark, WindowServer may restart when its display goes |
| dplink | the `modeset` step + `rdna3dplink=1` (`rdna3vm step dplink`); **needs a monitor on native DisplayPort** (no DP-to-HDMI/DVI adaptor or cable) that the GOP lights at boot | the driver's own DP SST mode set (`hw/dcn32_dp.c`, ported from DC's link training): the sink's DPCD caps, the link cap verified once by training at the sink's maximum with fallback (`dp_verify_link_cap`), the smallest link setting for the mode, 8b/10b clock recovery and channel equalization through the DMCUB's TransmitterControl (ENABLE, SETUP_VSEMPH), the DP DTO as pixel clock, MSA, M/N, unblank | log `dp: AUX n: DPCD 1.x, sink ...`, `dp: link training HBR2x4 pass VS=.., PE=..`, `dp: AUX n: verified link cap ...`, `dp: DIG f -> UNIPHY b: WxH ... trained`; ioreg `RDNA3,Head<n>,DPLink` (verified and running link, last result 0 = pass); `rdna3_modetest` switches the DP monitor and back like the TMDS ones | high: the DP monitor may show "no signal" until the old mode is back (a failed training puts the old timing back and trains again) |
| dsc | the `dplink` step + `rdna3dsc=1` (`rdna3vm step dsc`); **needs a native DP monitor that decodes DSC** (DSC in its specs, usually 4K 144 Hz or 5K/6K monitors) | FEC and DSC on the DP link (`hw/dcn32_dsc.c`, DC's `dc_dsc_compute_config` / `dsc2_get_packed_pps` / `link_set_dsc_enable`): FEC on a FEC capable monitor, a mode the link cannot carry uncompressed offered and set with DSC (the pipe's DSC engine ungated, the PPS sent every frame, DSC on in the monitor) | log `dp: AUX n: FEC yes, DSC yes ...`, `modeset: head n: WxH needs DSC: ...`, `dsc: DSC n: ... slices, .. bpp`, `dp: UNIPHY n: FEC enabled (active 1)`; ioreg `RDNA3,Head<n>,DSC`; `rdna3_modetest` sets the DSC mode and back with the picture right | high: the DP monitor may show "no signal" or a garbled picture until the old mode is back |
| mst | the `dplink` step + `rdna3mst=1` (`rdna3vm step mst`; `rdna3mstport=N` for another port); **needs an MST hub (or a daisy-chain DP monitor) on the card's DP output, with a monitor behind it lit by the GOP at boot** | DP MST (`hw/dp_mst.c`, `hw/dcn32_dp_mst.c`: DRM's sideband messages and payload table, DC's `allocate_mst_payload`): at the first mode switch the hub goes MST, its topology and every monitor's EDID are read, the head's stream goes to one monitor as an MST stream (VC payload, ACT, DP_MSE_RATE); back to SST when that does not run | log `mst: AUX n: MST on`, `mst: link address reply ..: n ports`, `mst: port n: EDID .. bytes`, `dp: MST allocate: 1 stream(s): VCP 1, n slots`, `dp: DIG f -> UNIPHY b (MST): ... trained`; ioreg `RDNA3,Head<n>,MSTPorts`, `RDNA3,Head<n>,MST` (ACT 1); after the first switch `rdna3_modetest list` shows the hub monitor's modes | high: the monitor behind the hub goes black at the takeover and may stay black until the step puts SST back |
| uhbr | the `dplink` step with `modeset2`'s clocks + `rdna3uhbr=1` (`rdna3vm step uhbr`); **needs a DP 2.1 (UHBR) monitor on a DP80 (UHBR20) or DP54 (UHBR13.5) certified cable straight into the card's DP output, lit by the GOP at boot, and a mode above HBR3 x4 in its EDID** (e.g. 4K 240 Hz, 5K/8K) | DP 2.1 UHBR SST (`hw/dcn32_hpo.c`, the 128b/132b part of `hw/dcn32_dp.c`): the UHBR rates from the VBIOS encoder caps, the sink (DPCD 0x2215), the cable id and LTTPRs; the link cap verified with DC's 128b/132b training (TPS1, channel EQ with the TX FFE presets, CDS) and the max-bandwidth fallback; a mode above HBR3 x4 set through the HPO stream and link encoders: DTBCLK raised (SetHardMinByFreq), the DTBCLK DTO, DPSTREAMCLK / SYMCLK32, the SST payload (VC 1, ACT) and the VC rate | log `dp: AUX n: 128b/132b: rates 07 (UHBR20), cable id ..`, `DAL SMU: DTBCLK .. reference .. kHz`, `dp: link training UHBR10x4 pass`, `dp: HPO 0 -> UNIPHY b: ... on UHBR.. x4: trained`, `dp: 128b/132b SST payload: n time slots, ..., ACT handled`; ioreg `RDNA3,Head<n>,UHBR` | high: the monitor may stay black until the step puts the old (8b/10b) mode back |
| odm | the `uhbr` step + `rdna3dsc=1 rdna3odm=1` (`rdna3vm step odm`); **needs an 8K (7680x4320) monitor, or a mode whose pixel rate is above what one pipe's DISPCLK reaches, on native DP** (a DSC monitor for 8K over DP 1.4; UHBR helps) | ODM combine 2:1 (`hw/dcn32_odm.c`, DC's `dcn32_update_odm`, `optc32_set_odm_combine` / `_bypass`, `dml32_CalculateODMMode`): a free pipe gets the right half of the picture (viewport, recout, OPP, MPC tree, DET split), the OTG reads two OPPs, the DSC slices are split over both pipes' DSC engines | log `modeset: head n: WxH on ..., ODM 2:1`, `odm: OTG o: pipe p / OPP .. + pipe q ...`, `odm: pipe p + q on OTG o: slices ..`, `dp: DSC .. 2 ODM segment(s)`; ioreg `RDNA3,Head<n>,ODM`; `rdna3_modetest` sets the 8K mode and back | high: the monitor may show half a picture or stay black until the old mode is back |
| mall | the `modeset2` step + `rdna3dpms=1 rdna3mall=1` (`rdna3vm step mall`); the two monitors as in `big2` | DC's display power (`hw/dcn32_pstate.c`, item O): at the bring-up the watermark range table goes to the PMFW (`dcn32_notify_wm_ranges`, TransferTableDram2Smu), the DPM levels and the voltage states are read; after the init, each mode set, display sleep / wake and the mirror lighting DC's DML decision runs on the lit heads (`dcn32_internal_validate_bw` subset: the lowest voltage state whose UCLK switch the heads hide, the four watermark sets, FCLK switching) and is programmed in DC's order (`dcn32_prepare_bandwidth`, MALL_SEL, `dcn20_optimize_bandwidth`: UCLK / FCLK hard minimums, SetFclkSwitchAllow, deep sleep DCFCLK); a second without flips turns the idle CAB on (DMCUB `CAB_FOR_SS`) | log `display power: init 0: PMFW .., n states, UCLK 96..1218 MHz ...`, `pstate: state s: UCLK switches (vactive ...)`, `display power: mode set: state s, UCLK switching at .. MHz (vactive), FCLK switching, ...`; ioreg `RDNA3,DCNPowerInit`, `RDNA3,DCNPower`, `RDNA3,Head<n>,PState` (active margin > 0 us); `PM:` lines: uclk at the low level and fewer watts than `big2` at idle; no flicker, `rdna3_drminfo --scanout` without HUBP underflow; `rdna3_modetest cycle` as in `modeset2` | medium: a wrong watermark shows as flicker or a black line at a UCLK switch; the next mode switch or a restart takes it back |
| subvp | the `mall` step + `rdna3subvp=1` with **one monitor** (`rdna3mirror=0`, `rdna3vm step subvp`); **needs a monitor with a mode that cannot hide the UCLK switch: 3840x2160, 3440x1440, 2560x1440 or 1920x1080 at 120-175 Hz** (or a 4K60 one whose DET is too small), set with `rdna3_modetest` | SubVP (`hw/dcn32_subvp.c`): DC's SubVP decision (`dcn32_assign_subvp_pipe`, phantom timing, static schedulability, only on a lower UCLK), a phantom pipe brought up on a free pipe (DP DTO, its OTG enabled and disabled again, HUBP blanked, MALL_SEL 1, DET 0), FORCE_ONE_ROW_FOR_FRAME on the main, the DMCUB's `HANDLE_SUBVP_CMD` under its HW lock; the main's flips give the firmware the new address | log `subvp: phantom of pipe p (OTG o, WxH) on pipe q: n lines ...`, `subvp: on, 1 phantom(s), 2 DMCUB lock commands: 0`, `display power: mode set: state s, UCLK switching at .. MHz (vactive + SubVP) ..., SubVP 1`; ioreg `RDNA3,Head<n>,PState` (`SubVP main`); at the high-refresh mode: no flicker, no underflow, UCLK at a low level in the `PM:` lines; the switch back turns the phantom off (`subvp: phantom pipe q off`, `subvp: off (was on)`) | high: a wrong phantom or schedule shows as a torn or flickering picture at every UCLK switch until the mode changes |
| otg | the `9c` step + `rdna3otg=1` (`rdna3vm step otg`) | DC's timing generator functions (`hw/dcn32_otg.c`: optc1/2/3/32) on every head the GOP lit, at display start: scan position and nominal line, active size, `optc1_wait_for_state` (vblank, then active), the vertical interrupt 0 aimed at the middle of the picture and its status latched (its registers put back), the OPTC underflow, double buffering and ODM segments; reads only apart from the interrupt position | ioreg `RDNA3,Head<n>,OTGCheck` = `0: WxH (head WxH), counter moving, vblank 1 vactive 1, vline0 line L latched (read L..L+2), underflow 0, db pending 0, ODM 1`; log `display: head n OTG o check ...` and `otg o: ...` | low: a wrong read changes nothing on screen |
| vblirq | the `play` step + `rdna3vblirq=1` (`rdna3vm step vblirq`), then `vblirq-off` (`rdna3vblirq=0`) for the A/B | DRIVER-PLAN N42: the DCN vblank (OTG VSTARTUP) and flip (HUBP) interrupts through the IH (`hw/dcirq.c`, `kext/src/RDNA3DisplayIrq.cpp`): IOGraphics' VBL from the vblank interrupt (a HIGH ONCE thread call), the accelerator's present queue completed at flip done by the pflip interrupt, the OTG poll as the fallback | log `display irq: head 0 (OTG n, HUBP n): the framebuffer registered for 0x3, enabled 0x3`, no `interrupt storm`, few or no `VBL: head n: no vblank interrupt for` lines; triage `fb N40, vblank irq` and `N42, display irq` INFO with the counts growing with the frames, `flip done from the pflip interrupt: N of M latch(es)`; the desktop at least as smooth as with `vblirq-off` | low: the poll stays as the fallback; a storm is cut |
| hdmi | the `modeset` step + `rdna3hdmi=1` (`rdna3vm step hdmi`); a monitor on HDMI (or DVI) that the GOP lights | DC's HDMI stream around each TMDS mode set (`hw/dcn32_hdmi.c`): `link_set_dpms_off`'s AV mute (two frames), SCDC back to HDMI 1.4 rates, info packets off and `enc1_reset_hdmi_stream_attribute`; after the VBIOS STREAM_SETUP `enc32_stream_encoder_hdmi_set_stream_attribute` (HDMI_CONTROL, GC / NULL packets, audio info frame line, AV mute off), `write_scdc_data` / `read_scdc_data` around the transmitter, the AVI infoframe (and an HDMI VIC vendor infoframe) through the VPG (`enc3_stream_encoder_update_hdmi_info_packets`); a DVI head gets `enc32_stream_encoder_dvi_set_stream_attribute` | log `display: head n HDMI: sink HDMI, SCDC s, LTE scramble l, max TMDS M MHz, HDMI 6G g, a audio modes`, `modeset: head n: HDMI_CONTROL 0x..., SCDC TMDS_CONFIG ...`; ioreg `RDNA3,Head<n>,HDMI` (`HDMI_CONTROL 0x....0019`: bit 0 keepout, bit 3 no extra null, bit 4 packet gen; bit 1 too with an LTE-scrambling sink; `SCDC ... (0)` on an HDMI 2.0 sink, `(-n)` NACK on an HDMI 1.4 one); the picture after every `rdna3_modetest` switch as in `modeset` (no overscan, right colours) | medium: a wrong infoframe can make the monitor show the picture over/underscanned or in wrong colours until the next switch |
| hdmi20 | the `modeset2` step + `rdna3hdmi=3` (`rdna3vm step hdmi20`); **needs a 4K HDMI 2.0 monitor or TV** and a high-speed cable | modes above a 340 MHz TMDS clock (4K60: 594 MHz) offered when the VBIOS gives the connector the HDMI 6G cap and the EDID allows the rate (`dcn10_link_encoder_validate_output_with_stream`); the HDMI scrambler and the 1/4 clock channel on, the monitor told through SCDC (TMDS_CONFIG 3), its scrambler status read back | `rdna3_modetest list` shows 3840x2160 60 Hz; ioreg `RDNA3,Head<n>,HDMI` = `HDMI_CONTROL ...` with bits 1 and 2 set, `SCDC TMDS_CONFIG 0x03 scrambler 0x01 flags 0x..0f`; the 4K60 picture | high: the monitor may show "no signal" until the old mode is back |
| dcseq | the `modeset2` step + `rdna3dcseq=1` (`rdna3vm step dcseq`) | DC's sequence extras of `hw/dcn32_hwseq.c` around each mode switch: on an HDMI head `apply_symclk_on_tx_off_wa` (TransmitterControl DISABLE, SetPixelClock at the running clock while the OTG still runs, OTG off, DISABLE again: `dcn20_reset_back_end_for_pipe`), then after the stream is up `dcn32_resync_fifo_dccg_dio` (`DENTIST_DISPCLK_WDIVIDER` rewritten with the RDIVIDER) | log `modeset: head n: DIO FIFO resync (DISPCLK divider 0x..)` after every switch; on the HDMI head 6 DMCUB VBIOS commands per switch instead of 4; the switches and pictures as in `modeset2`, no underflow | low: two extra VBIOS commands while the stream is down, one divider rewrite with the same value |
| audio | the `hdmi` step + `rdna3dplink=1 rdna3audio=1` (`rdna3vm step audio`); a monitor or TV with speakers (or an audio output) on HDMI, or on native DP; for the sound itself macOS needs an HDA driver attached to the card's audio function (1002:ab30 class; AppleHDA / AppleGFXHDA with the usual device-id spoofing) | DC's display audio around each mode set (`hw/dcn32_audio.c`): the azalia endpoints counted and initialised once (`read_dce_straps`, `update_num_audio`, `dce_aud_hw_init`), an endpoint per stream (`find_first_free_audio`), the EDID's audio modes, speakers and monitor name written as the ELD (`dce_aud_az_configure`), the audio DTO (`dce_aud_wall_dto_setup`: DTO0 from the pixel clock on HDMI, DTO1 from DPREFCLK on DP), HDMI ACR / audio infoframe through AFMT3, DP audio SDP (or the HPO stream encoder's APG on UHBR), `dce_aud_az_enable` and the stream's audio unmuted after the switch; `rdna3audio=3` also sends the PMFW's BACO audio PME message | log `audio: n azalia endpoints, straps 0x.. (HDMI audio native)`, `display: head n audio: "NAME", m modes, speakers 0x.., latency v/a ms`, per switch `display: head n audio: endpoint e, HDMI|DP (DIO)|DP (HPO), on, ...`; ioreg `RDNA3,Head<n>,Audio`; with an HDA driver: System Settings > Sound > Output lists the monitor (its EDID name) and a test sound plays through it | low: only the audio blocks change; the picture switches as in `hdmi` |
| fmt | the `hdmi` step + `rdna3fmt=1` (`rdna3vm step fmt`; `rdna3fmt=3` caps 8 bpc, `rdna3fmt=5` forces YCbCr 4:2:2); a monitor or TV on HDMI whose EDID offers YCbCr and/or deep colour (most TVs, many monitors) | amdgpu's output format on each HDMI mode set (`hw/dcn32_opp.c`): the encoding (`fill_stream_properties_from_drm_display_mode`: YCbCr 4:4:4 when the sink lists it), the bpc (`convert_color_depth_from_display_info`, `adjust_colour_depth_from_display_info`: 10 / 12 bpc within the TMDS limit), the output CSC, the OPP formatter (`opp1_program_fmt`: spatial dither at 8 bpc, truncation at 10 bpc, clamping, pixel encoding, dynamic expansion), the deep colour PHY PLL and its resync, and the DPG blank around the switch (`opp2_set_disp_pattern_generator`) | log and ioreg `RDNA3,Head<n>,Format` = `YCbCr 4:4:4 10 bpc, colour space n, sink bpc b formats 0x..`; the monitor's own info menu shows YCbCr / 10 bit; the picture after each `rdna3_modetest` switch has right colours (no pink / green cast, no banding beyond 8 bit's) | medium: a wrong format shows wrong colours until the next switch; the switch back to native restores RGB only through another switch (the format stays amdgpu's) |
| cursormall | the `9c` step + `rdna3cursormall=1` (`rdna3vm step cursormall`); move the mouse over both monitors | `hubp32_cursor_set_attributes`' DCN 3.2 part on top of the hardware cursor: `DCHUBP_MALL_CONFIG.USE_MALL_FOR_CURSOR` set for the framebuffer's 256 x 256 ARGB cursor (64 KiB > 16 KiB), as Linux sets it | the look's `HUBPn_DCHUBP_MALL_CONFIG` bit 2 set on each lit pipe; the pointer drawn and moving on both monitors without garbage | low: a bad MALL read shows as a garbled pointer only |
| dmubreset | the `9c` step + `rdna3dmubreset=1` (`rdna3vm step dmubreset`) | the DMCUB stopped and reset with amdgpu's exact `dmub_dcn32_reset` (GPINT STOP_FW, 30 reads for the ack and 30 for the answer, the reset forced either way, `MMHUBBUB_SOFT_RESET.DMUIF_SOFT_RESET` set and released by `dmub_dcn32_reset_release`) instead of the sequence step 5b verified (1 s wait, CW2-7 closed, DMUIF untouched); after the boot the DMCUB's diagnostic data (`dmub_dcn32_get_diagnostic_data`) and its first trace entries are logged | `RDNA3,DMUB = running, mailbox up`, `RDNA3,DMUBAmdReset` = 3 (bit 0 STOP_FW acked, bit 1 the firmware answered), log `DMUB: amdgpu reset (rdna3dmubreset=1): STOP_FW acked, firmware stopped`, `DMUB diag (after the amdgpu reset): ...`; the second monitor lit and extended as in 9c | medium: a DMCUB that does not come back leaves the second monitor dark (no mode sets, no clone) |
| 10 | same as 9 + `rdna3accel=1` | IOAcceleratorFamily2 accelerator | `ioreg -c RDNA3Accelerator`, `populateAccelConfig ... what the FAMILY pre-filled` in the log | high: WindowServer |
| 10s | same as 10 + `rdna3accelfb=3 rdna3acceliop=1 rdna3pm=2` | the accelerator's display machine gets head 0 and its pipe scans out what WindowServer composites (the sysctls `debug.rdna3accelfb=3` / `debug.rdna3accel_iop=1`, set from boot-args when the framebuffer publishes) | the desktop on head 0, log `boot-arg rdna3accelfb=3`, `iop: ... transaction hooks OPEN`, `debug.rdna3accel_iop_ok` counting | high: WindowServer may hang or show a black desktop |
| 10a | same as 10s but `rdna3accelfb=4` | also the aperture gate: WindowServer gets CPU mappings (getApertureMemoryDescriptor) of the surfaces it composites into | non-empty `SWAP PROBE` sums, the desktop on head 0, `23-windowserver-crash.txt` empty | high: WindowServer crash loop (black screen) |
| 10m | 9c + 10a: `rdna3dmub=1 rdna3mirror=2 rdna3accel=1 rdna3accelfb=4 rdna3acceliop=1 rdna3pm=2` | extended desktop through the accelerator: each head's framebuffer gets its gates when it publishes | the desktop on both monitors, `FRONT SURFACE ... LIVE WRITER` for both heads, `debug.rdna3accel_iop_ok_h1` counting | high: head 1 dark or a WindowServer crash loop |
| big | `rdna3lvl=8 rdna3msi=1 rdna3fb=1 rdna3dmub=1 rdna3mirror=2 rdna3accel=1 rdna3accelfb=4 rdna3acceliop=1 rdna3pm=2` | everything at once (one boot instead of ten): DMA/GFX tests, Vulkan, UCLK/FCLK DPM, extended desktop, the desktop composited through the accelerator on both heads | triage: tests PASS, `swap: surface ... -> head 0/1`, `FLIPPED head 0/1`, `LIVE WRITER`; the desktop on both monitors | high: any of the above |
| accelsdma | step `big` + `rdna3accelsdma=1` (also in `big4`) | A2: a swap the zero-copy flip cannot take (`rdna3acceliop` refused, a head without its own surface) is copied onto the scanout by SDMA0 in the core kext (`rdna3CopyVram`: the HDP flushed, then `amdgpu_copy_buffer`-style COPY_LINEAR packets per row, one fence) instead of the CPU through the BAR; no 120-frame cap | log `swap copy by SDMA (head n) #1: ... in N us` (every 600th again), no `swap copy by SDMA refused`; triage `PASS swap copy by SDMA`, or `INFO ... no copy was needed` when every swap flipped | low: a refused copy falls back to the CPU (three in a row switch SDMA off) |
| cts | step `metal` + `rdna3cts=1`, after `rdna3vm cts` (docs/TESTING-VM.md) | V2: a subset of the Vulkan CTS (`tools/cts/patterns.txt`: info, device creation, objects, memory, buffers, images, compute, SSBO layout, synchronization with timeline semaphores, render passes, basic draws, occlusion queries; about 2360 cases) through the Khronos loader and RADV, one `deqp-vk` run per group with its own time limit | the report's `CTS ...` lines and its `CTS: pass P, fail F, not supported N of T cases`; triage `Vulkan CTS` with the failing groups and cases | medium: a GPU hang inside a group: its run is killed at the limit, the groups after it may fail on the hung GPU |
| 11 | same as 10 | `vulkaninfo --summary` with RADV from `/Library/GPUBundles/rdna3mtl` | device `AMD Radeon RX 7800 XT (RADV NAVI32)` | medium |
| 12 | same as 10, then `sudo sysctl -w debug.rdna3accelfb=1` | Metal plugin for the processes `rdna3mtl-allow.txt` allows (names, and every application in apps mode: [INSTALL-OPENCORE](INSTALL-OPENCORE.md#which-apps-get-the-gpu-metal-plugin)) | `mtlprobe` sees the AMD device | high |
| sleep | step `metal` + `rdna3dpms=2 rdna3resume=1` | display sleep takes the link down on every TMDS monitor, and on DP SST ones with `rdna3dplink=1` (standby, no signal) and back (`hw/dcn32_state.c`); then `pmset relative wake 30; pmset sleepnow`: the heads saved and taken down, on the wake a power loss detected (PCI, `RCC_CONFIG_MEMSIZE`, ASIC_Init flag, PSP sOS) and, if so, the GPU posted and brought up again, the heads restored from the snapshot | `RDNA3,Head<n>,State` (`intact` / `restored (0), running`), `RDNA3,Wake` (what the wake found), `RDNA3,WakeHeadsLost = 0`, `RDNA3,WakeResumeResult = 1` after a power loss; report: `SYSTEM SLEEP: slept and woke, every head back`, the DMA/GFX tests after the wake pass | high: a monitor may stay dark after the wake; the rings may be dead after a power loss |
| vcnsleep | step `sleep` + `rdna3vcn=2` | step sleep with the video block: `--vcn-test` before the sleep; on the sleep `jpeg_v4_0_suspend` + `vcn_v4_0_suspend` (the VCPU BOs - stack, context, fw_shared - copied to system memory); on the wake `vcn_v4_0_resume` + `jpeg_v4_0_resume` (the copy back, the rings started again, VCN before JPEG and after GFX): after a power loss inside the GPU bring-up, otherwise from the wake work; `--vcn-test` again after the wake | step sleep's checks, plus `RDNA3,VCNSuspends = 1`, `RDNA3,VCNSuspendResult = 0`, `RDNA3,VCNResumes = 1`, `RDNA3,VCNResult = 0`, the IB tests 0 after the wake, log `VCN: suspended ... saved to system memory`, `VCN: resume (...): 0`; report: `VCN TEST: PASSED` twice | high, as step sleep |
| amdgpu | no rdna3 boot-arg but `rdna3amdgpu=1` (`rdna3vm step amdgpu`): the default profile with amdgpu's own sequence where the verified one differs | PSP: `psp_load_fw` order (SETUP_TMR before the PMFW, `psp_check_pmfw_centralized_cstate_management` false on 13.0.10), ASD, the register list (`psp_rl_load`), HDCP / DTM TAs, `psp_hw_fini` at unload; SMU: `smu_smc_hw_setup` order (`smu_v13_0_0_get_allowed_feature_mask`, every PCIe level after EnableAllSmuFeatures, `smu_v13_0_enable_thermal_alert`, `DALNotPresent` without the display core), AC source, `PrepareMp1ForUnload(0x55)` at shutdown; GPUVM: `amdgpu_vm_pt_clear` values, fragments and 2 MiB huge PDEs (`amdgpu_vm_ptes_update`), GART placed high, per-ring invalidation engines; IH: rings 0 and 1 in system memory through the GART (`MC_SNOOP`), rptr doorbells, page faults on ring 1; GFX: compute VMIDs 8-15 (`gfx_v11_0_init_compute_vmid`), graphics VMIDs 1-7; `sdma_v6_0_hw_fini` at unload and before a reset | `RDNA3,AmdgpuExact = 1`, `RDNA3,PSPResult = 0`, log `PSP: LOAD_ASD ... -> ok`, `PSP: LOAD_TA hdcp` / `dtm` ... `-> ok`, no `load RL failed`; `RDNA3,PMThermalAlert = 1`, `RDNA3,PMPcieLevels` (3 on this card), log `PM: rdna3amdgpu=1: ... PCIe levels sent`; `RDNA3,GARTStartMC = 0x7fff00000000`; `RDNA3,IHSystemMemory = 1`, log `IH: ring 0 + 1 in system memory`; `RDNA3,KCQVmInvEng = 1`; the report's DMA/GFX tests, Vulkan and the desktop as in step `default` | medium: a PSP or SMU refusal stops the bring-up at level 4/5 (boot without the boot-arg); the IH in system memory or the huge pages may show up as missing interrupts or VM faults |

Sleep: from level 4 on, without `rdna3resume=1`, the kext keeps macOS
from idle sleep (`RDNA3,IdleSleepBlocked = 1`): without the resume the
firmware and the GOP's picture are gone after a sleep. Do not put the
machine to sleep by hand during such tests; `rdna3sleep=1` lifts the
block. `rdna3resume=1` (step `sleep`, `big4`) is the suspend and resume:
the clients' VRAM saved to system memory and the IPs suspended
(`amdgpu_device_suspend`) before the sleep, the GPU posted and brought up
again on the wake, the VRAM and the display state restored; the machine
then idles to sleep like with any other GPU (see `docs/DRIVER-PLAN.md`,
D4, for what is still missing).

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
   `…-tests.txt`, `…-scanout*.ppm` and the kernel's frame trace streamed
   through the whole run (`…-trace-stream.bin`, `…-drmtrace-stream.bin`,
   `…-trace-sec.bin`, each with a `.size` file; the archive holds copies as
   `36-stream-*.bin`; the VM also sends WindowServer's plugin trace after each
   A/B phase as `…-plugin-ab1-WindowServer-<pid>.trace` and the like). Send
   all of them; they are what
   [AFTER-TESTS](AFTER-TESTS.md) expects from the VM.
   Only the collection, without running any test:
   `"$R/tools/macos/collect.sh" <label>` (writes `~/rdna3-report-<label>-<date>.tar.gz`).
   **Black screens, windows that do not appear or flash, flicker?** Instead of
   the report run the display bisect from a Terminal on the machine itself:
   `sudo "/Library/Application Support/RDNA3/autoreport.sh" --standalone --bisect`
   switches every new feature off and back on one at a time, 30 s each, while
   you look at the monitors (a notification, "step N" spoken, N beeps per step);
   press `y` (looks right), `n` (broken) or Enter (next) in the Terminal.
   `--bisect-plugin` does the Metal plugin's switches (WindowServer is
   restarted three times; log in again each time). `--bisect-agdc` is the
   kernel bisect over the switches that matter when WindowServer runs on the
   GPU (`swap_mode`, `swap_gen`, `flipbuf_present`, `swap_stale`, `iop`, `a3`,
   `swap_latch`, `iop_flip`; 13 combinations, not cumulative): use it on a
   machine booted with the `agdc`
   profile's boot-args, see
   [TESTING-VM](TESTING-VM.md#display-bisect-on-the-agdc-profile-step-bisect-agdc).
   The step list, the
   timetable and the reading of the result are in
   [TESTING-VM](TESTING-VM.md#display-bisect-step-bisect); the same report
   files land in `/Users/Shared/rdna3-reports/` (`…-bisect.txt` is new).
5. **A ladder step instead of the profile**, when the default profile fails:
   `sudo "$R/oc_install.sh" --efi /Volumes/EFI --step 2` (any step of the
   table above; `--step default` goes back to no `rdna3` boot-args), restart,
   report as in 4. Walk up from the last step that worked.
6. **Roll back:** `--off` (sets `-rdna3off`), the USB EFI, or from Recovery
   as INSTALL-OPENCORE §8 describes; `sudo "$R/uninstall.sh"` removes the install
   and, with the EFI mounted, its OpenCore part too (`--keep-opencore` skips that).

A dark screen with the system alive: SSH in, run step 4 there, then
`--off` and restart.
