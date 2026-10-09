# After the hardware tests: action plan

What to do the moment reports from the first hardware runs arrive, so the
work starts without a planning round. The test procedure itself is in
[TESTING-VM](TESTING-VM.md) (Linux host, VM with the card passed through) and
[TESTING-HW](TESTING-HW.md) (bare-metal hackintosh). Steps below use the
ladder numbers from there (`0 1 2 3 4 5 5b 6 6b 7 8 9 10`).

Rules that do not change: port from amdgpu (MIT) with the source function
named in a comment, every fix first reproduced in the simulator
(`tests/sim_navi32.c`), commits with `[skip ci]`, one CI run with
`full: "false"` per batch (never `full: true` without asking: CI minutes).

---

## 1. Intake (every batch of reports, ~10 min)

1. Put the files in `reports/<yyyy-mm-dd>/` in the working copy. **Never
   commit them**: they contain EDID and serial numbers (`reports/` is in
   `.gitignore`).
2. Run the triage:

   ```sh
   python3 tools/triage_reports.py reports/<date> --all
   ```

   It prints per boot: boot-args, `RDNA3,Stage2`, every check as
   PASS / FAIL, the kext's ERROR lines, the DMA/GFX test results and the
   tails of the serial and host logs. Exit status 1 = something failed.
3. Compare the IP discovery blob with Linux (once, from step 2 on):

   ```sh
   make tools
   tar -xzf reports/<date>/rdna3-report-*lvl-2*.tar.gz -C /tmp
   build/host/amd_discovery_dump /tmp/rdna3-report-*/discovery.bin > /tmp/mac.txt
   build/host/amd_discovery_dump reports/<date>/linux-reference/discovery.bin > /tmp/linux.txt
   diff /tmp/linux.txt /tmp/mac.txt      # must be empty
   ```

4. In Notion create one card "HW test <date>" (stage 0, status by result)
   with the PASS/FAIL table, and move the cards of the passing parts to
   "🟢 Działa na sprzęcie".
5. Find the **first** FAIL in ladder order and go to its section in §3.
   Fix one step per round: a later failure may be a consequence of the
   first.

## 2. The fix loop (same for every failure)

1. **Evidence.** From the report: `RDNA3,Stage2`, the `*Result` value
   (negative errno: -5 EIO, -12 ENOMEM, -16 EBUSY, -19 ENODEV, -22 EINVAL,
   -62 ETIME), the first `ERROR` line and the register values it prints,
   the last serial lines before a hang.
2. **Compare with amdgpu.** Open the amdgpu function named in the comment
   of our code at the failing point (`git grep -n "<function>"`) and diff the
   register sequence with ours: offsets (our `hw/gen/*.h` vs Linux
   `asic_reg`), order, delays, polled bits.
3. **Not enough evidence → diagnostic build first.** Add `RLOG`/`publish`
   of the registers that decide between the hypotheses (status, version,
   pointer registers). One diagnostic round is cheaper than two wrong fixes.
   Mark such a build in the commit message and ask for exactly one step.
4. **Reproduce in the simulator.** Make `tests/sim_navi32.c` behave like
   the card did (the register value from the report, the missing ack...),
   watch the test fail, fix the driver, `make test` green.
5. **Ship.** Commit `[skip ci]`; when the batch is ready, one CI run
   (`full: "false"`); give the user the run link and the exact retest:
   *download the artifact → `rdna3vm prepare --artifact …` → `rdna3vm
   window` → `curl … | sudo sh` → shut down → `rdna3vm step N` (and the step
   before it as a regression check).*
6. **Record.** Notion card, `docs/ROADMAP.md`, and the step's section here
   if a new failure mode was learned.

---

## 3. Per step: symptom → likely cause → action

Hypotheses are ordered by likelihood. "VM:" marks what can only happen with
passthrough (a bare-metal hackintosh boots the card cold from its GOP).

### Step 0 (`rdna3lvl=0`): the kext loads, touches nothing

| Symptom | Likely cause | Action |
|---|---|---|
| no `RDNA3` lines at all, `03-kexts.txt` without RDNA3Accel | kext not in the Auxiliary KC: `kmutil` error, SIP, approval | read `/var/log/rdna3-vmreport.log` and the `setup.sh` output; check `02-csr.txt` (must be disabled). Fix `tools/vm/macos/setup.sh` (e.g. `kmutil load -p` fallback like `package/install.sh`) |
| loaded, not matched to the card | `IOPCIMatch` / class match; VM: the card behind a different topology | `04-pci.txt` (device-id, `IOName`); adjust `kext/Info.plist` matching |
| hang or black screen already at step 0 | not our driver: VM firmware / GOP / BAR placement | `rdna3vm step off` (kext disabled) must boot. If it does not: VM problem → `romfile` (re-dump VBIOS with amdgpu loaded), disable Resizable BAR, raise `X-PciMmio64Mb`, try `CPU_MODEL=Penryn,…` in `~/rdna3-vm/config` |

### Step 1 (`rdna3lvl=1`): BAR5, IFWI wait, VRAM size

| Symptom | Likely cause | Action |
|---|---|---|
| `device not in D0, refusing to touch MMIO` | VM: the card left in D3hot by vfio / host | host: `vfio-pci.disable_idle_d3=1` (add to `rdna3vm` modprobe options); kext: put the device in D0 like `pci_set_power_state(D0)` before mapping (`RDNA3Accel::readConfigSpace`) |
| `MMIO reads all-ones` | BAR5 not decoded (memory space bit off), wrong BAR, device in reset | log line `BAR5 (MMIO) raw …, memory decode …`; compare the address with `linux-reference/lspci.txt` Region 5; enable memory decode in `mapMmio()` |
| `RCC_CONFIG_MEMSIZE = 0 … not posted` or `C2PMSG33` 0 | the card was not posted (VM: OVMF did not run the GOP ROM) | first `romfile` check in `host-*.log`; then port `amdgpu_device_asic_init` for GC ≥ 11: wait for the PSP bootloader (`psp13_wait_for_bootloader`), then `amdgpu_atomfirmware_asic_init(adev, true)` — we have the interpreter (`hw/atom.c`, `atom_asic_init`); call it from `readVramSize()` when MEMSIZE is 0 |
| `RDNA3,VRAMSizeMB` ≠ 16384 | wrong register / units | compare `linux-reference/sysfs-pm.txt` `mem_info_vram_total`; fix `readVramSize()` |

### Step 2 (`rdna3lvl=2`): discovery, display paths, AUX/EDID

| Symptom | Likely cause | Action |
|---|---|---|
| `no discovery signature` / `scratch offset outside VRAM` | the table is not at `VRAM top − 64 KiB` on this board | port `amdgpu_discovery_get_tmr_info` / the scratch-register path of `amdgpu_discovery_read_binary_from_mem`; compare with Linux `dmesg-amdgpu.txt` |
| `amd_discovery_dump` diff mac vs Linux non-empty | parser bug | add the Linux `discovery.bin` as a test vector to `tests/test_discovery.c`, fix `common/amd_discovery.c` |
| `GC is x.y.z, expected 11.0.3` | another Navi 3x board | check device-id; most code keys off discovery, only the firmware names (`amd_navi32_*`) and SMU tables are 11.0.3-specific |
| `DisplayPaths` 0, `AtomCommandTables` empty | VBIOS not found / object table parse | `RDNA3,VBIOSSize`; compare the ROM with `~/rdna3-vm/vbios.rom`; extend `tests/sim_navi32.c` with this VBIOS's table header versions |
| `Connector<n>,Monitor` empty, EDID missing | AUX: wrong instance, DDC line, timeout | `RDNA3,Connector<n>,AUX` / `,DPCD`; compare `vbios_display_paths` output with Linux `dmesg` connector list; fix the AUX engine index map in `hw/dcaux.c` |

### Step 3 (`rdna3lvl=3`): GC registers, harvesting

| Symptom | Likely cause | Action |
|---|---|---|
| `ActiveCUs` ≠ Linux | harvesting registers / SE-SA select | compare `linux-reference/radv-info.txt` (`num_cu`, `num_rb`, `gb_addr_config`); fix `hw/gfx11_info.c` (`gfx11_get_cu_info`) and its sim model |
| `GBAddrConfig` ≠ Linux | golden value / register | same comparison; `gfx_v11_0_gpu_early_init` |

### Step 4 (`rdna3lvl=4`): PSP and firmware — the most likely first wall

Read `RDNA3,Stage2`, `PSPResult`, `SOSVersion` and the PSP ERROR line.

| Symptom | Likely cause | Action |
|---|---|---|
| `failed: no PSP firmware` / `firmware set incomplete` | packaging: files missing in `RDNA3Accel.kext/Contents/Resources` | `firmware plan:` log line; check names in `kext/Makefile` `FW_FILES` vs `common/amd_fw.c` `amd_navi32_fw_names`; fix `rdna3vm` `fw_copy` |
| log `sOS already alive, skipping bootloader`, then any PSP failure | **VM: the card arrives initialised by Linux amdgpu.** amdgpu never reuses that state: `soc21_need_reset_on_init` (sOS sign of life in `MP0 C2PMSG_81`) → full ASIC reset on load, then `amdgpu_device_asic_init` | port it: at the start of `hwInit()`, if sign of life is set and it is not our own reinit → `smu13_mode1_reset` (exists, used by recovery) + wait for the bootloader + `atom_asic_init`, then the cold PSP path. Simulator: start the PSP model with SOL set and stale ring registers |
| `PSP: load <component> failed (C2PMSG_35=…)` during bootloader | bootloader command order / component IDs / wait bits for 13.0.7 | diff with `psp_v13_0_bootloader_load_{kdb,spl,sysdrv,sosdrv,intf_drv,dbg_drv,ras_drv}` and `psp_v13_0_bootloader_load_sos`; `firmware_info.txt` from Linux shows the versions the same files produce |
| `trust OS not ready for ring creation` / `ring create failed (C2PMSG_64=…)` | sOS did not come up, or ring address/size | value of C2PMSG_64; `psp_v13_0_ring_create`; the ring buffer must be in VRAM below our 512 MiB window — check `RDNA3,FBStartMC` vs Linux |
| `failed to load TOC` / TMR errors | TMR size/alignment/placement | `psp_tmr_init`: amdgpu puts the TMR at the **end** of VRAM (`amdgpu_bo_create_kernel` with `PSP_TMR_ALIGNMENT`); ours is inside the low window → move it |
| `LOAD_IP_FW <x> … failed` for one ucode | wrong fw type id / skipped component | `psp_get_fw_type` vs `common/amd_fw.c` table; `fw_load_skip_check` |
| `failed to start RLC autoload` | autoload buffer / TOC layout | `gfx_v11_0_rlc_backdoor_autoload_*` vs our layout in `hw/psp13.c` |
| freeze, serial stops in PSP | a PSP command pointed at a wrong MC address → bus hang | diagnostic build: log every command's MC addresses; compare `fb_start` / `vram_mc` with Linux |

### Step 5 (`rdna3lvl=5`): SMU / power management

| Symptom | Likely cause | Action |
|---|---|---|
| `SMU: MP1 firmware flags …, PMFW not running` | PMFW not started (it comes from the IFWI/PSP) | check the SMU blob was in the plan; `smu_v13_0_check_fw_status`; register offsets `MP1_FIRMWARE_FLAGS` |
| `SMU: not ready for msg … (resp 0)` | mailbox registers (C2PMSG_66/82/90) wrong | diff `hw/smu13.c` offsets with `smu_v13_0_7_ppt.c` `smu_v13_0_set_smu_mailbox_registers` |
| `SMUDriverIF` ≠ our header's version | PMFW newer/older than `smu13_driver_if_v13_0_7.h` | take the header matching the PMFW from Linux (`SMU13_0_7_DRIVER_IF_VERSION`), regenerate, re-check `PPTable_t` / `SmuMetrics_t` sizes in the simulator |
| `TransferTable…` / `EnableAllSmuFeatures failed` | table address / feature mask | retry with `rdna3pmmask=` (start from a minimal mask), `rdna3pm=1`; compare with Linux `pp_features` |
| values nonsense (temp 0 or 255, fan 0 while spinning) | metrics layout version | compare with `linux-reference/sysfs-pm.txt` / `gpu_metrics.bin`; fix `SmuMetrics_t` parsing |
| PASS | — | `PMGfxClkMin/MaxMHz`, `PMUclkMaxMHz` must equal the first/last lines of Linux `pp_dpm_sclk` / `pp_dpm_mclk` |

### Step 5b (`rdna3dmub=1`): DMCUB

| Symptom | Likely cause | Action |
|---|---|---|
| `DMUB: firmware did not stop (SCRATCH7 …)` | STOP_FW GPINT protocol differs on this firmware | `dmub_dcn32_reset` vs `hw/dmub32.c`; consider **not** resetting a running DMCUB and only attaching the mailbox (decision for Display Core) |
| `command … not consumed` | inbox1 registers / region addresses | `RDNA3,DMUBBootStatus`; `dmub_dcn32_setup_windows` |
| screen went black but log is fine | expected risk: the boot scanout depends on the DMCUB we restarted | note it; stay without `rdna3dmub` until the modeset work (§4) |

### Step 6 / 6b (`rdna3lvl=6`, `rdna3msi=1`): GMC, IH, interrupts

| Symptom | Likely cause | Action |
|---|---|---|
| `GMCResult` ≠ 0, `GFXHUB/MMHUB TLB flush timeout` | invalidation engine / ack registers | `gmc_v11_0_flush_gpu_tlb`, `mmhub_v3_0_2`; hub register offsets |
| `VMFaults` > 0 right after GMC | GART table / page directory address wrong | `RDNA3,LastFaultAddr/Status`; `gmc_v11_0_gart_enable` |
| `IHResult` ≠ 0 or `IHEarlyIVs` huge | ring base / wptr writeback / leftover interrupts (VM: state from Linux) | `ih_v6_0_irq_init`; the reset-on-init fix of step 4 usually also cures the leftovers |
| `MSILive` 0 | no MSI index / registration failed | `RDNA3,MSIIndex`; `IOPCIDevice::getInterruptType`; VM: QEMU vfio MSI is standard, so check the log line `MSI: cannot register interrupt` |

### Step 7 (`rdna3lvl=7 rdna3msi=1`): SDMA, the first GPU work

| Symptom | Likely cause | Action |
|---|---|---|
| `SDMA0: ring test timeout (…, status …)` | SDMA not running: firmware not loaded, RB registers, doorbell vs wptr poll | `SDMA0_STATUS` value from the log; `sdma_v6_0_gfx_resume_instance`; check the SDMA ucode was in `LOAD_IP_FW` |
| `SDMACopyMismatches` > 0 | MC address / HDP coherence | `rdna3_vram_mc` vs `FBStartMC`; HDP flush before the CPU readback |
| `MSIVerified` 0 | SDMA trap not routed | `SDMA0_CNTL.TRAP_ENABLE`, IH client id for SDMA0 (`SOC21_IH_CLIENTID_GFX`) |
| `--dma-test` fails, `VMFaults` > 0 | user VM page tables / GTT pages | `LastFaultAddr` vs the test's VA; `hw/gpuvm.c` PTE flags vs `amdgpu_vm_bo_update` |

### Step 8 (`rdna3lvl=8 rdna3msi=1`): GFX, KIQ, KCQ

| Symptom | Likely cause | Action |
|---|---|---|
| `GFX: rlc autoload timeout` | RLC autoload did not finish (PSP step 4 marginal) | `BOOTLOAD_STATUS`; `gfx_v11_0_wait_for_rlc_autoload_complete` |
| `GFX: ring test timeout (SCRATCH_REG0 …, CP_STAT …)` | CP RS64 start / ring registers / doorbell | `gfx_v11_0_cp_gfx_start`, RS64 program addresses from `gfx11_rs64_from_fw` |
| `KIQ: ring test timeout` / `KCQ: … MAP_QUEUES` | MQD fields / doorbell range | `gfx_v11_0_kiq_init_register`, `compute_mqd_init`; `nbio43_doorbell_init` ranges |
| `--gfx-test` fails | PM4 / VM on GFXHUB | `LastFaultAddr`, `GRBM_STATUS` in the log |

### Step 9 (`rdna3fb=1`): our framebuffer

| Symptom | Likely cause | Action |
|---|---|---|
| black screen, system alive | scanout surface address / pitch differs from the GOP's | `12-ioreg-fb.txt`; compare `RDNA3,Head0EdidMode` with the GOP mode; `fb/rdna3-fb.cpp` |
| wrong resolution / no monitor name | EDID path | step 2 AUX results |

### Step 10 (`rdna3accel=1`): accelerator

| Symptom | Likely cause | Action |
|---|---|---|
| WindowServer crash loop | `IOAccelConfig` / missing methods | `22-log-windowserver.txt`; `populateAccelConfig`; `accel/gen_stubs.py` stub list; go back to step 9 boot-args while fixing |

---

## 4. When everything up to step 8 passes: next development, in order

1. **Make it default.** Default `rdna3lvl` to the highest level that passed
   twice, keep the boot-args as overrides; update TESTING docs.
2. **RADV on the card (stage 5).** Add `rdna3vm step 11`: installs the RADV
   build from the `radv-macos` CI job into the bundle and runs
   `vulkaninfo --summary`, then a compute smoke test (a dEQP-VK
   `compute.basic.*` subset). Fix the winsys gaps it shows. This needs one
   `full: true` CI run — ask first (CI minutes).
3. **Display Core (stage 3, plan §3).** With real DCN registers: dump DCN
   state on Linux and on macOS at the same mode (diagnostic build), then
   DCCG pixel clock, DP link training, DML, modeset; hotplug through IH.
4. **Metal (stage 6).** `mtlprobe` against the device; first Metal sample.
5. **Power (stage 2/7).** UCLK/FCLK/DCN DPM once Display Core owns the
   display, then BACO/sleep, `IOPMPowerState`.
6. **MES (queue management)** only when user queues or more rings are needed.
7. **Packaging (stage 8).** OpenCore guide, release zip with
   `LICENSE.amdgpu`, versioning.

## 5. Bare metal vs VM

The bare-metal hackintosh boots the card cold from its GOP; the VM gets it
after Linux amdgpu (warm PSP, leftover interrupts, possibly no post). A
failure seen only in the VM points at §3 step 4 "sOS already alive" or at
the VM setup (step 0). A failure in both is ours. When the VM passes,
bare metal still needs one run of steps 4–8 before calling a stage done.
