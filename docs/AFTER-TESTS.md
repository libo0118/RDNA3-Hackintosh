# Agent playbook: acting on hardware-test reports

**Audience: an AI coding agent** continuing this project after the user ran
the hardware tests. Follow it literally; it exists so that no planning round
is needed. The user is not the reader of this file.

You cannot touch the hardware. The user runs tests on their machine and sends
reports; you diagnose, fix, verify in the simulator and CI, and tell the user
exactly what to run next. One round = one report batch → one fix → one retest.

---

## 0. Context you need (read once per session)

- **Project:** macOS (Hackintosh) driver for AMD RX 7800 XT (Navi 32, PCI
  `1002:747E`), ported from Linux amdgpu (MIT). Branch **`stage1-kext`** of
  `voten641/rdna3-hackintosh`. Do not open a PR unless asked.
- **Nothing had run on real hardware before these tests.** Everything was
  verified only in the simulator `tests/sim_navi32.c` and in CI builds.
- **Test procedures the user follows:** `docs/TESTING-VM.md` (Linux host,
  macOS VM with the card passed through, driven by `tools/vm/rdna3vm`) and
  `docs/TESTING-HW.md` (bare-metal hackintosh). Ladder steps:
  `0 1 2 3 4 5 5b 6 6b 7 8 9 10`; boot-args per step are in
  `step_args()` in `tools/vm/rdna3vm`.
- **Kext layout:** `kext/src/RDNA3Accel.cpp` (levels 0–3: PCI, BAR5, IFWI
  wait, VRAM size, discovery), `RDNA3Display.cpp` (VBIOS paths, AUX/EDID,
  DMCUB glue), `RDNA3Drm.cpp` (amdgpu UAPI), `RDNA3Bringup.cpp`
  (`hwInit()`: levels 4–8 = PSP, SMU/PM, DMCUB, GMC, IH, SDMA, GFX/KIQ/KCQ;
  recovery). Hardware logic is portable C in `hw/` (one file per IP block,
  register headers generated in `hw/gen/` by `tools/gen_regs.py`).
- **Where amdgpu lives:** fetch the exact Linux file you compare against
  into your scratchpad, e.g.
  `curl -sfL https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/gpu/drm/amd/amdgpu/psp_v13_0.c`.
  Register headers: `drivers/gpu/drm/amd/include/asic_reg/...`.

### Hard rules

1. **Language:** talk to the user in **Polish**. Everything committed to the
   repo (code, comments, docs, commit messages) is **English only**.
2. **CI minutes are scarce (user's explicit instruction).**
   - Every commit message carries **`[skip ci]`**.
   - Verify with **one** manual run per batch, always with
     `inputs: {"full": "false"}` — the workflow input defaults to `true`, so
     never dispatch without it. Never run `full: true` (Metal + RADV) without
     asking the user first.
   - A push of `metal/**` without `[skip ci]` starts the expensive Metal job.
3. **Port, don't invent.** Each hardware change follows a named amdgpu
   function; name it in a comment. NullMoth-derived code stays AMD-only.
4. **Simulator first.** Every hardware fix must be reproduced in
   `tests/sim_navi32.c` (make the model behave like the card in the report)
   and pass `make test` before it is pushed.
5. **One step per round.** Fix the first failing ladder step only; later
   failures are often consequences.
6. **Commit trailers:** follow the session's attribution instructions.
   Never put a model identifier in the repo.
7. **Never commit reports** (`reports/` is git-ignored: EDID, serials).

### Tools and state outside the repo

- **Notion kanban:** data source `collection://2dad47f8-8a55-4222-96e5-39b401eb72ed`
  (title property `Zadanie`; `Status`: `📋 Do zrobienia`, `🔨 W toku`,
  `✅ Zrobione (CI)`, `🧪 Przetestowane (symulator)`, `🟢 Działa na sprzęcie`,
  `🔴 Zepsute / zablokowane`; `Etap`: `0 Stanowisko/CI` … `8 Pakowanie`;
  `Priorytet`: Wysoki/Średni/Niski; text `Pliki`, `Notatki`). Status page
  with per-stage percentages: `3f4560305d0d81058363de48e7e2efbd`.
  The board is in Polish (it is the user's, not the repo).
- **CI:** `.github/workflows/build.yml`. Artifact **`RDNA3Accel-kext`**
  (kexts, `rdna3_drminfo`, `libdrm.dylib`) is what the user installs.
- **Local tests:** `make test RDNA3_FW_DIR=<dir>`; fetch the firmware into
  the scratchpad first with the same loop as the `host-tests` job in
  `build.yml` (linux-firmware `amdgpu/*.bin`).

---

## 1. Intake (start of every round)

1. Collect the user's files into `reports/<yyyy-mm-dd>/`. Expected:
   `rdna3-report-*.tar.gz`, `*-tests.txt`, `serial-*.log`, `host-*.log`,
   and once `linux-reference/`. If something needed is missing, ask for those
   exact files (in Polish) and stop.
2. Run `python3 tools/triage_reports.py reports/<date> --all`.
   Exit 1 = a check failed. The first `FAIL` (in ladder order) is your target.
3. From step 2 on, check discovery once:
   ```sh
   make tools
   mkdir -p /tmp/rep && tar -xzf reports/<date>/rdna3-report-*lvl-2*.tar.gz -C /tmp/rep
   build/host/amd_discovery_dump /tmp/rep/*/discovery.bin > /tmp/mac.txt
   build/host/amd_discovery_dump reports/<date>/linux-reference/discovery.bin > /tmp/linux.txt
   diff /tmp/linux.txt /tmp/mac.txt    # must be empty, else fix step 2 first
   ```
4. For every step that passed: add `<step> <date> <vm|metal>` to
   `docs/status/hw-verified.txt` and run `python3 tools/progress.py`
   (the README map turns those blocks green). A step that fails again later
   is removed from that file.
5. Notion: create a card `HW test <date>` (Etap `0 Stanowisko/CI`) with the
   PASS/FAIL list; move cards of verified parts to `🟢 Działa na sprzęcie`,
   the failing part to `🔴 Zepsute / zablokowane`.
6. Go to the failing step in §3.

## 2. Fix loop (every failure)

1. **Evidence.** `RDNA3,Stage2`, the `*Result` value (negative errno:
   -5 EIO, -12 ENOMEM, -16 EBUSY, -19 ENODEV, -22 EINVAL, -62 ETIME),
   the first `ERROR` line with its register values, the last serial lines.
2. **Compare with amdgpu.** Find the amdgpu function named in the comment at
   the failing spot (`git grep -n`), fetch it, diff the register sequence:
   offsets (`hw/gen/*.h` vs `asic_reg`), order, delays, polled bits.
3. **Evidence too thin → diagnostic build, not a guess.** Add `RLOG` /
   `publish("RDNA3,…")` for the registers that separate the hypotheses,
   say "diagnostic build" in the commit message, and ask for that one
   step only.
4. **Reproduce in the simulator**, watch it fail, fix, `make test` green.
5. **Ship:** commit `[skip ci]`, push, one CI run `full: "false"`, wait for
   green. Fix and repeat if red (never hand the user a red build).
6. **Tell the user** (Polish) with the template in §5.
7. **Record:** Notion cards and the status page percentages, `docs/ROADMAP.md`,
   and add the new failure mode to §3 of this file.

---

## 3. Decision tables: symptom → likely cause → action

Hypotheses are ordered by likelihood. "VM:" = only possible with passthrough
(bare metal boots the card cold from its GOP).

### Step 0 (`rdna3lvl=0`): kext loads, touches nothing

| Symptom | Likely cause | Action |
|---|---|---|
| no `RDNA3` log lines, `03-kexts.txt` without RDNA3Accel | VM: no AMD device in `04-pci.txt` (triage prints the AMD ids) | the card must sit on `pcie.0` (`GPU_BUS=pcie0`, default): behind a `pcie-root-port` macOS leaves it unconfigured. Check `host-*.log` "guest PCI bus": BAR0 mapped at 40 s |
| same, AMD device present | not loaded: kernelmanagerd refuses ad-hoc kexts in the Auxiliary KC ("invalid codeful kernel extension") | the VM injects the kexts with OpenCore (`rdna3vm prepare`, `EFI/OC/Kexts`); serial log must show "a prelinked copy … is already present" for the `/Library/Extensions` copy |
| loaded, not matched | `IOPCIMatch` / topology | `04-pci.txt`; fix matching in `kext/Info.plist` |
| hang / black screen at step 0 | VM firmware, not the driver | ask for `rdna3vm step off`. If that hangs too: VM setup — re-dump VBIOS with amdgpu loaded, Resizable BAR off, larger `X-PciMmio64Mb`, `CPU_MODEL=Penryn,…` in `~/rdna3-vm/config` |

### Step 1 (`rdna3lvl=1`): BAR5, IFWI wait, VRAM size

| Symptom | Likely cause | Action |
|---|---|---|
| `device not in D0, refusing to touch MMIO` | VM: card left in D3hot | host: add `vfio-pci disable_idle_d3=1` handling in `rdna3vm`; kext: bring the device to D0 (`pci_set_power_state(D0)` equivalent) in `RDNA3Accel::readConfigSpace` |
| `MMIO reads all-ones` | BAR5 not decoded / wrong BAR / in reset | log `BAR5 (MMIO) raw …, memory decode …`; compare with `linux-reference/lspci.txt` Region 5; enable memory decode in `mapMmio()` |
| `RCC_CONFIG_MEMSIZE = 0 … not posted`, `C2PMSG33` 0 | card not posted (VM: GOP ROM not run) | check `romfile` in `host-*.log`; port `amdgpu_device_asic_init` for GC ≥ 11 (`psp13_wait_for_bootloader`, then `atom_asic_init` from `hw/atom.c`) and call it from `readVramSize()` when MEMSIZE is 0 |
| `VRAMSizeMB` ≠ Linux's `mem_info_vram_total` (16368, not 16384: amdgpu reports RCC_CONFIG_MEMSIZE as is) | register/units | `linux-reference/sysfs-pm.txt` `mem_info_vram_total`; fix `readVramSize()` |

### Step 2 (`rdna3lvl=2`): discovery, display paths, AUX/EDID

| Symptom | Likely cause | Action |
|---|---|---|
| `no discovery signature` / `scratch offset outside VRAM` | table not at VRAM top − 64 KiB | port `amdgpu_discovery_get_tmr_info` / `amdgpu_discovery_read_binary_from_mem`; compare `dmesg-amdgpu.txt` |
| discovery diff non-empty | parser bug | add Linux `discovery.bin` as a vector in `tests/test_discovery.c`; fix `common/amd_discovery.c` |
| `GC is x.y.z, expected 11.0.3` | other Navi 3x | stop and ask the user which card; firmware names `amd_navi32_*` and SMU tables are 11.0.3-specific |
| `DisplayHeads` 0 | VM: OVMF lit no head (no picture) - expected there; metal: the GOP's head not found | metal only: `RDNA3,Head<n>,*`, compare `dcn32_read_heads` with the OTG registers |
| `AtomCommandTables` empty (info only) | expected: the RX 7800 XT VBIOS has only command tables 0, 6, 22, 53, 66 (`tools/vbios_info.py`); like amdgpu's `*_fallback` in command_table2.c, DCN 3.2 runs encoder/transmitter/pixel-clock through DMCUB (`hw/dmub32.c`) | none |
| `DisplayPaths` 0 | VBIOS not read / object table versions | `RDNA3,VBIOSSize`; ask for `~/rdna3-vm/vbios.rom`; teach `hw/vbios.c` the header versions; add them to the simulator |
| `Connector<n>,Monitor` empty | AUX instance / DDC map / timeout | `RDNA3,Connector<n>,AUX`/`,DPCD`; compare `vbios_display_paths` with Linux connectors; fix the AUX map in `hw/dcaux.c` |

### Step 3 (`rdna3lvl=3`): GC registers, harvesting

| Symptom | Likely cause | Action |
|---|---|---|
| `ActiveCUs` / `GBAddrConfig` ≠ Linux | harvesting / SE-SA select / golden value | compare `linux-reference/radv-info.txt`; fix `hw/gfx11_info.c` (`gfx11_get_cu_info`) and its sim model; `gfx_v11_0_gpu_early_init` |

### Step 4 (`rdna3lvl=4`): PSP and firmware — most likely first wall

Inputs: `RDNA3,Stage2`, `PSPResult`, `SOSVersion`, `NeedPost`, `PostResult`,
the PSP ERROR line.

**IP versions of the real card** (discovery, 2026-10-09): GC 11.0.3, MP0/MP1
**13.0.10**, DCN **3.2.0**, VCN **4.0.0**, MMHUB 3.0.0, SDMA 6.0.3, NBIO 4.3.0,
SMUIO 13.0.6. Earlier code assumed Navi 33's 13.0.7 / 3.2.1 / 4.0.4; fixed:
firmware names, MMHUB 3.0.0 registers, and `hw/smu13.c` now follows
`smu_v13_0_0_ppt.c` (driver_if v13_0_0, SCPM, MP1 firmware flags at
0x3010024). The DCN register headers come from `dcn_3_2_1_*`; every
define they hold (display, AUX/HPD, DMCUB: 1088) has the same value in
`dcn_3_2_0_*`, and amdgpu uses the same DMUB code (dmub_dcn32.c) and pipe /
AUX / HPD counts for both, so nothing changes for steps 5b and 9 apart
from the firmware (`dcn_3_2_0_dmcub.bin`).

| Symptom | Likely cause | Action |
|---|---|---|
| `Stage2: starting` for > 90 s, no `firmware …` line | the firmware read never returned (was: `OSKextRequestResource` unanswered for an injected kext) | now read with the vnode KPI from `/Library/Extensions/RDNA3Accel.kext/Contents/Resources`; check that `setup.sh` ran after the last `prepare` |
| `FB MC 0x0-0xffffff`, `NeedPost` 0, KDB times out after `C2PMSG_35` was 0x80000000 | fixed 2026-10-09: the FB base was read from GC (`GCMC_VM_FB_LOCATION_*`, 0 until the gfxhub is programmed) instead of MMHUB like `gmc_v11_0_vram_gtt_location`, and with MMHUB 3.0.2 offsets (Navi 32 is 3.0.0), so the PSP got a wrong fw_pri MC address | `FB MC` must now show the MMHUB base (typically 0x8000000000) |
| `FB MC 0x0-0xffffff`, `NeedPost` 1 | VM: OVMF did not run the option ROM's init, GPU not posted | `postIfNeeded()` runs `atom_asic_init` (amdgpu_device_asic_init); `PostResult` must be 0 and the FB location non-zero afterwards. `PostResult` ≠ 0: the ATOM interpreter (`hw/atom.c`) on the real ASIC_Init v2.1 table — find the opcode in the log |
| `PSP: load KDB failed (-62), C2PMSG_35=0`, `SOSVersion` 0 | VM, 2026-10-09: the bus reset after the host's amdgpu leaves the PSP with no sOS (`C2PMSG_81` 0) and the bootloader not ready (`C2PMSG_35` 0) | `resetIfPspStuck()` (default `rdna3reset=1`) does a mode-1 reset first; read `PSPStuckAtInit`, `InitResetResult` and the two `PSP state (…)` log lines. No change after the reset: the PMFW did not take the debug message — try the host side (`RESTORE_MODE=suspend` before the step, so the card enters the VM fresh from a power cycle) |
| `failed: no PSP firmware` / `firmware set incomplete` | packaging | `firmware plan:` log; `kext/Makefile` `FW_FILES` vs `common/amd_fw.c` `amd_navi32_fw_names`; fix `fw_copy` in `tools/vm/rdna3vm` |
| log `sOS already alive, skipping bootloader`, then any PSP failure | **VM: the card arrives initialised by Linux amdgpu.** amdgpu never reuses that state: `soc21_need_reset_on_init` (sign of life `MP0 C2PMSG_81` ≠ 0) → ASIC reset on load, then `amdgpu_device_asic_init` (`amdgpu_device_init`, Linux v6.12 `amdgpu_device.c` ~4265 and ~4300) | at the start of `RDNA3Bringup::hwInit()` (first init only, not `fReinit`): if sign of life is set → `smu13_mode1_reset` (exists; used by recovery) → `psp13_wait_for_bootloader` → `atom_asic_init` → cold PSP path. Simulator: start the PSP model with SOL set and stale ring registers |
| `PSP: load <component> failed (C2PMSG_35=…)` in bootloader | command order / component ids / ready bits for 13.0.7 | diff with `psp_v13_0_bootloader_load_{kdb,spl,sysdrv,sosdrv,intf_drv,dbg_drv,ras_drv}`, `psp_v13_0_bootloader_load_sos`; `linux-reference/firmware_info.txt` |
| `trust OS not ready for ring creation` / `ring create failed (C2PMSG_64=…)` | sOS not up / ring address | `psp_v13_0_ring_create`; compare `RDNA3,FBStartMC` with Linux |
| `failed to load TOC` / TMR errors | TMR placement | the TMR (167 MiB on the card) is allocated top-down (`rdna3_vram_alloc_top`, amdgpu's NO_CPU_ACCESS kernel BO) in `[VRAM - 256 MiB, VRAM - fw_reserved_size)`; check `RDNA3,VRAMFwReservedKB` and the `psp-tmr … top-down` log line |
| `LOAD_IP_FW <x> … failed` | fw type id / skip list | `psp_get_fw_type`, `fw_load_skip_check` vs `common/amd_fw.c` |
| `failed to start RLC autoload` | autoload TOC/buffer layout | `gfx_v11_0_rlc_backdoor_autoload_*` vs `hw/psp13.c` |
| freeze, serial stops inside PSP | a command with a wrong MC address | diagnostic build logging every command's MC addresses |

### Step 5 (`rdna3lvl=5`): SMU / PM

| Symptom | Likely cause | Action |
|---|---|---|
| `MP1 firmware flags …, PMFW not running` | PMFW not started | SMU blob in the plan; `smu_v13_0_check_fw_status` |
| `not ready for msg … (resp 0)` | mailbox offsets | `smu_v13_0_0_init_msg_ctl` vs `hw/smu13.c` |
| `SMUDriverIF` ≠ header version | PMFW vs `hw/gen/smu13_driver_if_v13_0_0.h` | take the matching Linux header, regenerate, re-check struct sizes in the sim |
| `TransferTable…` / `EnableAllSmuFeatures failed` | table address / mask | ask for a retest with `rdna3vm args "rdna3lvl=5 rdna3pmmask=<minimal>"`, then widen |
| `SCPM` 1 and `TransferTableDram2Smu(PPTABLE)` / `SetAllowedFeatures…` rejected | SCPM not detected (runtime DB read) | `psp13_scpm_enabled`: the database is 1 MiB below the top of VRAM, read through MM_INDEX/MM_DATA |
| `SCPM` 1, `PMFeaturesRunning` has UCLK/FCLK DPM | expected: SCPM ignores the allowed mask; log must show `UCLK pinned at … MHz`, `FCLK pinned …` | none; watch for scanout flicker on metal |
| `powerplay table format 20.0` | card, 2026-10-09: PMFW 80.92 (driver IF 0x40, header 0x3D) reports 20.0 for the 13.0.0 layout | only logged now; amdgpu never checks it. `SCPM` reads 0x40000000 there: any non-zero = enabled, as `psp_sw_init` |
| implausible temps/fan | metrics layout | compare `linux-reference/sysfs-pm.txt`, `gpu_metrics.bin` |
| pass criterion | — | `PMGfxClkMin/MaxMHz`, `PMUclkMaxMHz` = first/last lines of Linux `pp_dpm_sclk`/`pp_dpm_mclk` |

### Step 5b (`rdna3dmub=1`): DMCUB

| Symptom | Likely cause | Action |
|---|---|---|
| `firmware did not stop (SCRATCH7 …)` | STOP_FW protocol | `dmub_dcn32_reset` vs `hw/dmub32.c`; option: attach to the running DMCUB without reset |
| `DMUBResult` -62, `auto-load timed out`, CW0/CW1 offsets 0 in `DMUB state` | card, 2026-10-09: the PSP-programmed code windows are empty when the driver releases the DMCUB (`SCRATCH7 0xdeaddead` is only STOP_FW_RESPONSE from the GOP's firmware) | the three `DMUB state (after the PSP load / after the SMU / before reset)` lines show which stage leaves CW0 empty; if the SMU: test `rdna3pm=0`, then the DCN features in the allowed mask |
| `command … not consumed` | inbox/regions | `RDNA3,DMUBBootStatus`, `dmub_dcn32_setup_windows` |
| 5c: `DMUB attach: no running DMCUB` | the GOP's DMCUB was stopped (an earlier `rdna3dmub=1` boot in the same power cycle, or the PSP) | cold boot, then 5c again |
| 5c: `inbox1 ... behind no enabled code window` / `not in VRAM` | the GOP firmware uses other windows | send the `DMUB state` lines: CW0-CW7 offsets/bases, INBOX1_BASE_ADDRESS |
| 5c: attached, `mailbox silent` | the GOP firmware does not answer QUERY_FEATURE_CAPS | may still run VBIOS commands: try 9b3 |
| screen black, log fine | expected risk | record; keep `rdna3dmub` off until modeset work (§4) |

### Step 8c (`rdna3mes=1`): MES scheduler pipe

| Symptom | Likely cause | Action |
|---|---|---|
| `MES: the KIQ did not process MAP_QUEUES for the scheduler pipe` | MAP_QUEUES me 2 / ENGINE_SEL 5 refused | compare with `gfx_v11_0_kiq_map_queues` (AMDGPU_RING_TYPE_MES); KIQ ring test lines |
| `MES: no answer to opcode 0` (SET_HW_RESOURCES), fence 0 | the scheduler does not run the ring: MQD (`mes_v11_0_mqd_init`) or the doorbell (MES_RING0, 0x00B) | `RDNA3,MESSchedVersion` 0 means the pipe never started; check `CP_MES_CNTL` |
| `no answer to opcode 0`, fence advanced but status 0 | the firmware rejected the frame | packet layout vs `mes_v11_api_def.h` of the firmware's API version |
| 8b fails only with `rdna3mes=1` | MES grabbed the kernel queues (hqd masks) | `compute_hqd_mask` 0xc / `gfx_hqd_mask` 0xfffffffe leave queue 0 to the kernel |

### Hotplug (any step from 6b with `rdna3msi=1`)

Unplug and plug a monitor cable while macOS runs: the log shows `HPD: hotplug interrupts enabled`, then `display: HPDn disconnected` / `connected` and `hotplug on pins ...`; `RDNA3,HotplugEvents` counts them.

| Symptom | Likely cause | Action |
|---|---|---|
| no `display: HPDn` line on unplug | DCN interrupts do not reach the IH (`DC_HPD_INT_EN` set but no IV) | `RDNA3,IHEarlyIVs`, MSI counters; read `HPDn_DC_HPD_INT_STATUS` (bit 0 latched?) |
| many `HPDn` lines for one unplug | polarity / ack order (`hpd_ack` in irq_service_dcn32.c) | send the log; check `DC_HPD_INT_POLARITY` after the ack |
| `connected` but the connector has no EDID afterwards | re-probe too early (sink not ready) | delay the re-probe (amdgpu waits through `dc_link_detect` retries) |

### Step 9b (`rdna3dmub=1 rdna3mirror=1`): second monitor (clone)

| Symptom | Likely cause | Action |
|---|---|---|
| `RDNA3,Mirror` missing | the DMCUB did not come up (5b) | fix 5b first: the PHY side has no other way on this VBIOS |
| `failed: no other TMDS monitor with the boot mode` | the other connector: no HPD, a DP sink, no DDC EDID, or its EDID lacks the boot timing | `RDNA3,ConnectorN,*`; the `mirror: the monitor on AUXn has no ...` line |
| `clone: no free pipe / OTG / DIG front end` | GOP left more state lit | the `clone: head ...` plan line and `DCN: head` lines; read MPCC_TOP_SEL / OTG_CONTROL of all four |
| `clone: DISPCLK/DPPCLK below the N kHz pixel rate` | the GOP left the display clocks at a low DPM level | `RDNA3,DCNClocks` and the `clone: DCN clocks` line; next: `dcn32_smu_set_hard_min` (DISPCLK, DPPCLK) before the clone, as `dcn32_update_clocks` does |
| after `DAL SMU: N display(s)`: `SMU: msg 0x12 ... timed out`, then `not ready` for ever; the host cannot take the card back | not the DAL message and not one VBIOS command: the PMFW answered after each of them (run 4) and failed at the first metrics transfer (TransferTableSmu2Dram) with the clone lit | FIXED (run 6): forcing the arbiter's p-state allow (9bp) kept the PMFW up for the whole run, USR retraining alone (9bu) did not; the clone now does it by default (`rdna3pstate=0` turns it off). History: 9bw (SetAlwaysWaitDmcubResp 0) did not help (run 5); then the `DCHUBBUB ... USR_RETRAINING_CNTL` log (retraining REQUESTED = the PMFW waits for the arbiter) and 9bp / 9bu / 9bpu (`rdna3pstate=1/2/3`: force p-state allow as dcn30_hardware_release, USR retraining allow as hubbub32_force_usr_retraining_allow); make the one that keeps `RDNA3,CloneMetricsResult = 0` the default |
| `framebuffer build` FAIL (framebuffer older than the core) | OpenCore injects only RDNA3Accel; RDNA3Framebuffer links IOGraphicsFamily and loads from the Auxiliary KC, i.e. the copy of the last setup.sh | autoreport.sh installs a new bundle by itself; kernelmanagerd then needs the user's approval of the new ad-hoc build (`KMErrorDomain Code=27 ... not approved to load`, found 2026-10-10): `rdna3vm approve` (System Settings -> Privacy & Security -> Allow), once per framebuffer build |
| panic `Taking non-sleepable RW lock with preemption enabled` in `namei` from `RDNA3Bringup::loadFirmware`, < 1 s after boot | the firmware was read before the root file system existed (run 6, once) | loadFirmware waits for the IOBSD resource and `vfs_rootvnode()` |
| `PM:` lines: ~197 W, gfxclk ~2700 MHz, activity 100 % with nothing submitted | GRBM_STATUS idle (0x0000302c) while the PMFW counts 100 %: no GFX clock gating, the GFX clocks never stop (amdgpu gates them in late init) | FIXED in part (run 8): GFX clock gating (gfx11_set_clock_gating, default on, `rdna3cg=0` off) took the idle from ~200 W to ~75 W, hotspot 78 -> 55 C, fan stops; the PMFW still reports 100 % activity and ~2.7 GHz target. Step 8o (run 10, GFXOFF allowed): AllowGfxOff accepted, DMA/GFX tests pass, no hang, but no change (~75 W, 100 % activity): the PMFW never sees the GFX idle, so it never enters GFXOFF either. Open: what keeps the PMFW's GFX activity at 100 % while GRBM_STATUS is idle (RLC / MES state); the rest of the idle power is UCLK/FCLK pinned at their top level for the GOP's scanout |
| `clone: pipe N power on timed out` | `DOMAINn_PG_CONFIG` refused (IP_REQUEST_EN) | `dcn32_hubp_pg_control`; try with the pipe the GOP left powered |
| `DET… timed out` | DET split not taken (COMPBUF in the way) | log line has `COMPBUF`; shrink COMPBUF first (`dcn32_program_compbuf_size`) |
| `SetPixelClock / STREAM_SETUP / TransmitterControl through the DMCUB failed` | DMCUB rejected or hung on the VBIOS command | `DMUB: command ... not consumed`; check payload sizes vs dmub_cmd.h |
| `MirrorCheck`: NOT running | the clone OTG never started: pixel clock (DCCG `OTGn_PIXEL_RATE_CNTL`, PHYPLL) or VTG | the `clone: pipe N lit` line; compare `OTG_CONTROL` of both OTGs |
| `MirrorCheck`: running, CRC DIFFERENT | the pipe scans out something else: HUBP surface/viewport, DPP/MPC/OPP copy | send the log; a black clone has a constant CRC |
| result 0, `MirrorCheck` running + same CRC, second screen black or "no signal" | PHY/PLL side: `PHYPLL` / `PIXEL_RATE` in the `clone: pipe N lit` line still 0, or the HDMI sink wants infoframes | send the log; compare the source and clone `OTGn_PIXEL_RATE_CNTL`, `PHYxSYMCLK_CLOCK_CNTL`; next: write PHYPLL/SYMCLK source like the GOP's |
| first screen flickers or goes black after the clone | DET split too small, or watermarks | send the log; the DET line shows the split |
| 9c: only one display in System Settings | the second RDNA3Framebuffer did not start on the nub | `ioreg -n RDNA3DisplayHead`; `RDNA3-fb: head 1: ...` lines (VRAM for the surface, flip) |
| 9c: second display black, first fine | the flip of head 1 to its own surface | `RDNA3-fb: head 1: own surface` then `flip #` lines; `RDNA3,Mirror` still names the clone |

### Step 6 / 6b (`rdna3lvl=6`, `rdna3msi=1`): GMC, IH, MSI

| Symptom | Likely cause | Action |
|---|---|---|
| `GMCResult` ≠ 0, TLB flush timeout | invalidation engine | `gmc_v11_0_flush_gpu_tlb`, `mmhub_v3_0_2` offsets |
| `VMFaults` > 0 right after GMC | GART/PD address | `LastFaultAddr/Status`; `gmc_v11_0_gart_enable` |
| `IHResult` ≠ 0, `IHEarlyIVs` huge | ring setup / VM: leftovers from Linux | `ih_v6_0_irq_init`; the step-4 reset-on-init fix also clears leftovers |
| `MSILive` 0 | registration | `RDNA3,MSIIndex`, log `MSI: cannot register interrupt` |

### Step 7 (`rdna3lvl=7 rdna3msi=1`): SDMA

| Symptom | Likely cause | Action |
|---|---|---|
| `rdna3_drminfo`: `mmap: Cannot allocate memory` | VRAM BO outside the BAR window (card, 2026-10-09: user BOs started at 512 MiB with a 256 MiB BAR) | fixed: user BOs prefer the visible heap (`fVramHeap`), the TMR left the BAR window; `DRM/amdgpu UAPI ready` log line shows both heap sizes |
| `DMA job submitted, fence N` then `DMA job results WRONG` | SDMA 6.0 walks the GFXHUB, which only level 8 enabled: the user VMID had page tables on MMHUB alone (card, 2026-10-09) | fixed: level 7 runs `gfx11_gfxhub_enable` before `sdma60_start` (`GFXHUBResult` 0); drminfo now prints the dwords it read |
| level 8: `GFX: ring test timeout (SCRATCH_REG0 0xcafedead, CP_STAT 0x80000000)` with a GFXHUB fault on VMID 0 at 0x0, UTCL2 client 0x6 (CPG), `ME_CNTL 0x15000000` | card, 2026-10-10: PSP set the RS64 caches (PFP_IC/ME_IC/DC0 non-zero, PCs at 0xc00), RB0 registers right, but `CP_GFX_MQD_BASE_ADDR` 0: the RS64 gfx firmware reads the queue's MQD, which the RB0 (amdgpu_async_gfx_ring=0) path never sets | fixed: the gfx ring is a v11_gfx_mqd mapped by the KIQ (MAP_QUEUES ENGINE_SEL 4) as amdgpu does by default; `GFXAsync` 1, log `GFX: ring 0 ... mapped by the KIQ`. Still failing: the timeout dump prints `GFX MQD`/`HQD active`/`base`; HQD active 0 means the KIQ ignored the MAP_QUEUES |
| level 8: `KIQ: ... ring test passed`, then `KIQ: ring test timeout` and `KCQ: the KIQ did not process MAP_QUEUES` | card, 2026-10-10: the MEC KIQ (me 1 pipe 1) runs WRITE_DATA but not SET_RESOURCES / MAP_QUEUES. amdgpu_discovery always sets enable_mes_kiq for GC 11: the KIQ is MES pipe 1 | fixed: the KIQ runs on MES pipe 1 (`mes_v11_0_kiq_hw_init`: MES pipes started from the mes/mes1 firmware headers, KIQ MQD and HQD on me 3, doorbell MES_RING1); `KIQOnMES` 1, log `KIQ: MES me 3 pipe 1 up` |
| `AUXn: HPD 1, no DPCD, EDID 0 bytes` on a DP monitor; `SW_STATUS 0x00000801` in the `HPD high, DPCD read` line | card, 2026-10-10: AUX_SW_NON_AUX_MODE, the DDC pad was in I2C mode so the engine sent nothing | fixed: the AUX acquire puts the pad in AUX mode (`hw_ddc.c` MODE_AUX, `AUX_PADn_MODE` in `DC_GPIO_DDCn_MASK`); the probe gives non-DP pads their I2C mode back. Still NON_AUX_MODE with the pad in AUX mode (`DDCn_MASK` bit 16 set, 2nd run): `AUX_CONTROL` had `AUX_HPD_SEL` 0 on every engine; the acquire now runs `dcn10_aux_initialize` (HPD_SEL = the engine's HPD, LS_READ_EN 0, RX receive window 0). HDMI sinks still need the DDC/I2C engine |
| `AUXn: HPD 1, no DPCD` with NON_AUX_MODE on a port with a DP->HDMI adaptor or an HDMI port | card, 2026-10-10: both monitors are TMDS sinks (HDMI, and HDMI through a DP++ adaptor on DP-3); AUX never answers them | fixed: the probe reads their EDID over the DDC with the HW I2C engine (`dce_i2c_hw.c`): log `EDID 256 bytes (DDC, TMDS sink)`, `RDNA3,ConnectorN,EDIDSource` = DDC; head 0 takes the EDID that has its current mode. If it fails: the `no EDID on the DDC` line gives the I2C status |
| `SDMA0: ring test timeout (…, status …)` | engine not running / RB regs / doorbell | `sdma_v6_0_gfx_resume_instance`; SDMA ucode loaded in step 4? |
| `SDMACopyMismatches` > 0 | MC address / HDP | `rdna3_vram_mc` vs `FBStartMC`; HDP flush before readback |
| `MSIVerified` 0 | trap routing | `SDMA0_CNTL.TRAP_ENABLE`, IH client id |
| `--dma-test` fails, faults | user page tables | `LastFaultAddr` vs test VA; `hw/gpuvm.c` vs `amdgpu_vm_bo_update` |

### Step 8 (`rdna3lvl=8 rdna3msi=1`): GFX, KIQ, KCQ

| Symptom | Likely cause | Action |
|---|---|---|
| `rlc autoload timeout` | step 4 marginal | `gfx_v11_0_wait_for_rlc_autoload_complete` |
| `GFX: ring test timeout` | RS64 start / ring regs | `gfx_v11_0_cp_gfx_start`; `gfx11_rs64_from_fw` |
| `KIQ`/`KCQ` failures | MQD / doorbell range | `gfx_v11_0_kiq_init_register`, `compute_mqd_init`, `nbio43_doorbell_init` |
| `--gfx-test` fails | PM4 / GFXHUB VM | `LastFaultAddr`, `GRBM_STATUS` |
| `rdna3_vktest`: `physical devices: 0` | RADV refused the device: the `RADV_DEBUG=startup` lines above it name the query (DEV_INFO, HW_IP_INFO, FW_VERSION, MEMORY) | reproduce on the host first: `tools/radv_host.sh` (RADV on the loopback UAPI) |
| `rdna3_vktest`: `vkCreateDevice` fails | the preamble / init-state IBs RADV submits on the GFX ring, or a BO / VA call | `LastFaultAddr`, `RDNA3,CSResult`, `hang:` lines in the log |
| `rdna3_vktest`: a queue `FAIL ... fence not signalled` | the compute meta shader (64 KiB) or CP DMA (256 B) never finished | which size failed tells shader vs CP DMA; `hang:` dump, `GRBM_STATUS` |
| `rdna3_vktest`: words wrong | cache flush / VA mapping of the host buffer (GTT) | first wrong word index: page boundary = mapping, all = no flush |
| `rdna3_vktest`: host pointer import FAIL | GEM_USERPTR (`gem_userptr` in uapi/, `RDNA3Gtt` wiring the user pages) | `LastFaultAddr`; compare with the loopback (`tools/radv_host.sh` passes it) |
| `rdna3_vktest`: OPAQUE_FD import FAIL | PRIME export/import tokens (`RD_NR_XNU_BO_EXPORT/IMPORT`), or an ioctl struct size mismatch (uapi/ zero-extends shorter structs like `drm_ioctl` since 2026-10-10) | `DRM_XNU_TRACE=1` shows the failing ioctl nr |

### Step 9 / 10 (`rdna3fb=1`, `rdna3accel=1`)

| Symptom | Likely cause | Action |
|---|---|---|
| step 9 black screen, system alive | scanout address/pitch vs GOP | `12-ioreg-fb.txt`, `Head0EdidMode`; `fb/rdna3-fb.cpp` |
| step 10 WindowServer crash loop | `IOAccelConfig` / missing methods | `22-log-windowserver.txt`; `populateAccelConfig`; `accel/gen_stubs.py` |

---

## 4. When steps 0–8 pass: next work, in order

1. Default `rdna3lvl` to the highest level that passed twice (boot-args stay
   overrides); update the testing docs and Notion percentages.
2. **RADV on the card:** add `rdna3vm step 11` (RADV from the `radv-macos`
   CI job into the bundle; `vulkaninfo --summary`; a dEQP-VK
   `compute.basic.*` subset). Needs one `full: true` run — ask first.
3. **Display Core:** diagnostic build dumping DCN state at the GOP mode,
   compared with Linux; then DCCG pixel clock, DP training, DML, modeset,
   hotplug (`docs/PLAN-NEXT.md` §3).
4. **Metal:** `mtlprobe`, first sample.
5. **Power:** UCLK/FCLK/DCN DPM after Display Core; BACO/sleep.
6. **MES** only when needed. 7. **Packaging:** OpenCore guide, release zip.

## 5. Message to the user (Polish template)

Keep it short, no internals beyond one sentence of cause.

```
Wynik: kroki 0–N przeszły, krok M nie (<jedno zdanie: co i dlaczego>).
Poprawka: <jedno zdanie>. CI: <link do przebiegu> (zielone).

Co zrobić:
1. Pobierz artefakt RDNA3Accel-kext z <link>.
2. rdna3vm prepare --artifact ~/Downloads/RDNA3Accel-kext.zip
3. rdna3vm window → w Terminalu macOS: curl -s http://10.0.2.2:8000/setup.sh | sudo sh
   → sudo shutdown -h now
4. rdna3vm step M (potem step M-1 jako kontrola)
5. Prześlij nowe pliki z ~/rdna3-vm/reports.
```

For a diagnostic build say so explicitly ("to wersja diagnostyczna, nic nie
naprawia, potrzebuję tylko raportu z kroku M").

## 6. Bare metal vs VM

VM failures that bare metal would not have: warm PSP (step 4 "sOS already
alive"), leftover interrupts (step 6), unposted card (step 1), VM setup
(step 0). A failure in both environments is ours. A stage counts as done
only after steps 4–8 also passed once on bare metal (or the user decides
the VM is enough).

## 7. Known pitfalls of this environment

- Shell heredocs: a line `EOF` inside a Python heredoc ends it — write longer
  scripts to a scratch file instead.
- `grep -P '[ąę…]'` and `\b` match arrows/dashes byte-wise here; check Polish
  text with a Unicode-aware Python scan.
- The kext cannot be compiled locally (no macOS SDK): kext-side changes are
  verified only by the CI run — read them twice before pushing.

## Linux reference dump (`tools/linux/card_dump.sh`)

`sudo bash tools/linux/card_dump.sh` on the Linux host (amdgpu, idle desktop)
gives `~/Downloads/rdna3-card-dump.tar.gz`; `~/Downloads/rdna3-card-dump.progress`
names the last phase if the machine hangs. Use it as ground truth:

| File | Compare with |
|---|---|
| `regs/gc_11_0_0.txt` (only with `--regs`) | our `PM: RLC_STAT ...` / `GRBM_STATUS` lines: RLC_GPM_STAT, RLC_PG_CNTL, RLC_CGCG_CGLS_CTRL, CGTT override under Linux idle |
| `pm/idle-samples.txt`, `pm/gfxoff-residency.txt` | `PM: activity` (100 % on macOS): Linux idle activity and GFXOFF residency |
| `pm/sysfs.txt` (`pp_features`) | `RDNA3,PMFeaturesRunning` |
| `display/amdgpu_dm_dtn_log.txt` (never sweep DCN registers raw: a power-gated block hung the host) | DCHUBBUB watermarks / DRAM state, DET, DCCG, OTG timing for B5 and a real modeset |
| `display/*.edid`, `display/card*-*.txt` | `RDNA3,Connector*` EDIDs and routing |
| `vulkan/rdna3_vktest.txt` | the same test on macOS: a pass on Linux means a macOS failure is ours |

### Linux reference of this card (dump 2026-10-10, kernel amdgpu, idle, both monitors)

- SMU feature mask `0x0003ebbc71ffffff`: **the same as ours** (SCPM runs the
  same features whatever we allow).
- Idle: ~12 W, GFX activity 7-10 % (gpu_metrics), GFXCLK 135-226 MHz (SCLK
  deep sleep 40 MHz), **UCLK 96 MHz, FCLK 601 MHz** with two 1080p60 heads.
- GFX clock gating flags: MGCG, CGCG, CGLS, REPEATER_FGCG (= ours) plus
  BIF MGCG, HDP shutdown, IH CG, ATHUB MGCG/LS (not ported yet).
- DC watermarks (us): set A urgent 17.32, sr_enter 59.72, sr_exit 52.74,
  dram_clk_change 417.32; set C dram_clk_change 67.32. DCFCLK 199 MHz,
  DISPCLK 215 MHz, DPPCLK 212 MHz. OTG 2591x1127 total (1080p60 CVT-RB).
- amdgpu runs the MES (`uni_mes=1`); our default is the legacy RB0 ring.
- Our activity was already 46-49 % at level 5 (before any GFX bring-up of
  ours), so the PMFW's busy reading comes from the RLC/GFX state, not work.
- `rdna3_vktest` on Linux RADV: ALL PASSED 8/8 (the test itself is right).

### Card run 11 (build #90, 2026-10-10)

- Step 8: `rdna3_vktest` ALL PASSED 8/8 on the card (RADV built in the VM).
- 8u / 9cu (`rdna3pm=2`): UCLK 96 MHz, 52-54 W instead of 77 W; the user
  reported no flicker on 9cu.
- Step 10: the accelerator starts; WindowServer picks it, makes three
  1920x1080 surfaces in VRAM, but `RD3DisplayPipe::initFramebufferResource:
  scanout gate CLOSED` - nothing reaches the panel: first head off, second
  black with the hardware cursor. Next: step 10s opens the gates from
  boot-args (`rdna3accelfb=3 rdna3acceliop=1`).
- PM trend: `RLC_GPM_STAT 0x00940016` (GFX power/clock on) at every sample,
  activity 100 %. The sample itself reads GC registers (which wakes the
  clock), so it does not prove CGCG never engages; still open.

