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

1. Collect the user's files into `reports/<yyyy-mm-dd>/` (usually one
   `rdna3-result.tar.gz` from `rdna3vm result`: unpack it there; its
   `00-triage.txt` is the host's triage, `00-host.txt` the commit and CI run). Expected:
   `rdna3-report-*.tar.gz`, `*-tests.txt`, `serial-*.log`, `host-*.log`,
   and once `linux-reference/`. If something needed is missing, ask for those
   exact files (in Polish) and stop.
2. Run `python3 tools/triage_reports.py reports/<date> --all`.
   From step 2 on each report comes with `<report>-scanout-pipe<N>.ppm`:
   what that display pipe really scanned out ~2 min after boot
   (`rdna3_drminfo --scanout`, HUBP read back + a 320-wide picture). Triage
   prints the per-pipe state ("screens") and writes the PNGs next to them:
   **look at them** (the user is often not at the screen) before asking
   what the monitors showed.
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

### Step userq (`rdna3userq=1`): user queues through the MES

| Symptom | Likely cause | Action |
|---|---|---|
| `RDNA3,UserQueues = 0`, log `user queues: IP mask 0x0` | firmware older than amdgpu's gate (ME 2390, PFP 2530, MEC 2600, MES 120, SDMA 25) | the log line names the versions; newer linux-firmware |
| log `rdna3userq=1, but the MES scheduler or the doorbells are not up` | step 8c fails, or no doorbell BAR | fix 8c first |
| `USERQ CREATE: Invalid argument` | a buffer not mapped where the queue says (`amdgpu_userq_input_va_validate`), no doorbell BO, the wptr BO larger than a page | `uapi/rdna3_drm.c` `userq_create` / `mes_userq_mqd_create` |
| `user queue: ADD_QUEUE ... : -62` (timeout) | the MES refused the frame: no API status fence | `MESAPI__ADD_QUEUE` layout vs the firmware's API version (`RDNA3,MESSchedVersion` bits 12-23), the MQD (`gfx11_compute_mqd_init`), `vm_context_cntl`, `page_table_base_addr` |
| queue created, `dst` stays 0 | the queue never ran: doorbell index (BO offset / 4 + offset * 2), the doorbell write did not reach BAR2, or the wptr poll address (GART slot of the wptr BO) | `RDNA3,LastFaultVMID` 8-15 is the queue's process faulting: the VM flush of VMIDs 8-15 |
| `dst` right, fence `did NOT signal` | the firmware did not write the rptr to the MQD's fence address, or no EOP interrupt reached the waiter | the fence slot is in the seq64 BO (VRAM); the model in `tests/sim_navi32.c` (`uq_eop`) is an assumption until this step passes |

### Step pmctl (`rdna3pmctl=2`): power management from userspace

| Symptom | Likely cause | Action |
|---|---|---|
| `rdna3_pmctl info`: `IOConnectCallMethod: 0x...` | the kext predates selector 1, or the user client did not open | rebuild; `RDNA3,PMCtl` must be in the IORegistry |
| `RDNA3,PMODResult` not 0, log `SMU: Failed to get overdrive table` | TransferTableSmu2Dram(TABLE_OVERDRIVE) refused | the OverDriveTable_t size vs `smu13_driver_if_v13_0_0.h` of the PMFW's driver interface (`RDNA3,SMUDriverIF`) |
| selftest `FAIL power limit`, `GetPptLimit` unchanged | SetPptLimit refused or THROTTLERS not running | `RDNA3,PMFeaturesRunning` bit 24; the limits `RDNA3,PMPowerLimitMinW/MaxW` |
| `fan pwm 128` ok but `info` shows the old speed | the PMFW keeps its own fan control on 13.0.10 even with FAN_CONTROL stopped (the reason amdgpu moved to the OD fan curve) | expected; the OD fan curve check is what counts |
| `OD ... -> -5 (EIO)` | the PMFW refused the OverDriveTable (a value past its limits, a feature it does not have) | compare the values with `OD_RANGE` of `rdna3_pmctl info` and `RDNA3,PMODFeatures`; the PMFW model's checks in `tests/sim_navi32.c` (`sim_od_accept`) |
| `profile COMPUTE` mask wrong | PMFW older than 80.69 does not get POWER_SAVING added | `RDNA3,SMUVersion` |

### Step baco (`rdna3baco=1`): BACO cycle

| Symptom | Likely cause | Action |
|---|---|---|
| `BACO: not supported` | platform caps without BACO, or FEATURE_BACO not running | `rdna3_pmctl info` BACO support; `RDNA3,PMFeaturesRunning` bit 20 |
| `BACO enter -> -5` | EnterBaco refused | log the PMFW version; amdgpu only uses BACO for runtime PM |
| enter ok, `BACO exit -> -62` (timeout) | the PMFW does not answer while the chip is off, or the exit needs the PCI config space saved / restored around it | reboot; next try: save/restore config space as in `resetIfPspStuck` |
| exit ok, the GPU does not come back (`GPU reset ... FAILED` style lines, `PSP bootloader did not come back`) | the re-init after BACO differs from the one after a mode-1 reset | compare the `PSP:` lines with a mode-1 recovery (step hanggfx) |

### Step vcn (`rdna3vcn=1`): VCN 4.0 + JPEG 4.0

Inputs: `RDNA3,VCN`, `RDNA3,VCNResult`, `RDNA3,VCN0IBTest`, `RDNA3,VCN1IBTest`,
`RDNA3,JPEGResult`, `RDNA3,VCNTmrMC0/1`, `RDNA3,VCNFirmware`, `RDNA3,VCNGated`,
`RDNA3,IRQFenceVCN/JPEG`, the `VCN` / `JPEG` log lines and the report's
`rdna3_drminfo --vcn-test` section. Linux on the card (dmesg): two instances,
`Found VCN firmware Version ENC: 1.24 DEC: 9 VEP: 0 Revision: 34`, rings
`vcn_unified_0`, `vcn_unified_1`, `jpeg_dec`.

| Symptom | Likely cause | Action |
|---|---|---|
| step 4 now fails at `LOAD_IP_FW VCN1` | the PSP refuses the second instance's image (`amd_vcn_setup_ucode`, GFX_FW_TYPE_VCN1) | compare the ucode list with `amdgpu_vcn_setup_ucode` / `psp_get_fw_type`; send the PSP lines |
| `VCN: the PSP did not load vcn_4_0_0.bin for every instance`, `RDNA3,VCNTmrMC1 = 0` | the LOAD_IP_FW answer carried no `fw_addr` (the TMR address cache window 0 needs) | `psp13_load_ip_fw` / `ucode_tmr_mc`; amdgpu takes it from `resp.fw_addr_lo/hi` |
| `VCN[n] is not responding, giving up!!!` (UVD_STATUS in the line), `RDNA3,VCNResult = -62` | the VCPU did not boot: PMFW did not power the instance (`PowerUpVcn` failed in the log), PGFSM status never reached the power-on pattern, or cache window 0 is not on the firmware | `RDNA3,VCNTmrMC0/1`; `smu13_set_vcn_enable`; `vcn40_disable_static_power_gating` / `vcn40_mc_resume` against `vcn_v4_0.c`; amdgpu runs the DPG path on this card: if the static path never boots, run step vcn2 (`rdna3vcn=2`) |
| ring test timeout (`VCN0: ring test timeout (rptr ...)`) | the VCPU booted but does not fetch: doorbell not routed (S2A entry 4/5, `nbio43_vcn_doorbell_range`), `VCN_RB1_DB_CTRL`, or the fw_shared unified-queue flags | boot with `rdna3doorbell=0` (the ring then uses `UVD_RB_WPTR`) to tell doorbell from ring setup |
| `encode IB test failed` / `decode IB test failed`, ring test fine | the IB's checksum / engine-info packages, or the message buffer address (VMID 0 MC address in VRAM) | `amdgpu_vcn_unified_ring_ib_header` vs `hw/vcn40.c`; the model in `tests/sim_navi32.c` checks the checksum |
| `JPEG: ring test timeout (pitch 0x...)` | JPEG power (`PowerUpJpeg`, UVDJ PGFSM), JRBC ring registers, or VCN_JPEG_DB_CTRL | `jpeg40_start` vs `jpeg_v4_0_start` |
| `--vcn-test`: `AMDGPU_CS failed` | the UAPI refused the IB (`amdgpu_cs_patch_ibs`: not inside one mapping; JPEG: a packet outside `jpeg_v2_dec_ring_parse_cs`'s set) | `uapi/rdna3_drm.c` `cs_patch_ibs` |
| `--vcn-test`: fence did NOT signal | the job did not run in the client's VMID: MMHUB page tables (`RDNA3,LastFaultVMID`, an MMHUB fault in the log), or the trap / fence | `RDNA3,IRQFenceVCN` counts traps; a hang gates the block (`RDNA3,VCNHangs`) |
| `RDNA3,VCNGated` stays 0 | the idle work never ran or found work pending | `vcnIdleTick`; jobs still emitted (`fence_count_emitted`) |
| DMA/GFX tests fail only with `rdna3vcn=1` | the VCN bring-up broke a shared block (MMHUB, IH, doorbells) | send the report; run step 8 again without it |
| step vcn passes, `vcn2` fails | the DPG path (next table) | compare the two reports |

### Step vcn2 (`rdna3vcn=2`; `rdna3vcn=3` by hand): VCN dynamic power gating

Inputs: step vcn's, plus `RDNA3,VCNPowerGating`, `RDNA3,VCNDPGStarts`,
`RDNA3,VCNSRAMUploads`, `RDNA3,VCNSRAMUploadErrors`, the `PSP: LOAD_IP_FW
type 49` / `type 50` lines (logged only when the PSP answers with a status)
and `VCN<n>: PSP load of the DPG SRAM failed`. Linux on the card runs this
path (soc21 pg_flags VCN | VCN_DPG | JPEG, `adev->vcn.indirect_sram`); its
`amdgpu_vcn_psp_update_sram` loads 27 (offset, value) pairs per instance
(216 bytes), as `tests/sim_navi32.c` checks.

| Symptom | Likely cause | Action |
|---|---|---|
| `VCN<n>: PSP load of the DPG SRAM failed (-62 ...)` or a nonzero status, `RDNA3,VCNSRAMUploadErrors > 0` | the PSP refuses GFX_FW_TYPE_VCN0_RAM / VCN1_RAM (size, alignment of `dpg_sram`, or VCN not powered: `PowerUpVcn` failed) | the PSP lines; try `rdna3vcn=3` (the same registers through UVD_DPG_LMA_CTL/DATA, no PSP) to tell PSP from register content |
| SRAM loads fine, ring test timeout | the VCPU did not boot from the image: cache windows 0/1 (the PSP fills them, the image has 0), or `UVD_VCPU_CNTL` order | `rdna3vcn=3` writes window 0 with the TMR address itself: if it passes, the PSP did not fill the windows - compare with `vcn_v4_0_mc_resume_dpg_mode` |
| `rdna3vcn=3` fails too, `rdna3vcn=1` passes | the DPG register image itself (`vcn40_start_dpg_mode` vs `vcn_v4_0_start_dpg_mode`), `UVD_POWER_STATUS` PG mode | send both reports; dump `UVD_POWER_STATUS`, `UVD_DPG_PAUSE` |
| first jobs pass, later ones time out after an idle gate | `vcn_v4_0_stop_dpg_mode`'s waits (POWER_STATUS 1, RPTR = WPTR) or the restart (`RDNA3,VCNDPGStarts` grows, uploads too?) | `RDNA3,VCNGated`, the `VCN` log lines around the gate; `rdna3vcn=1` for comparison |
| step passes | pass | `vcn2 <date> vm` into `docs/status/hw-verified.txt`, regenerate |

### Hotplug (any step from 6b with `rdna3msi=1`)

Unplug and plug a monitor cable while macOS runs: the log shows `HPD: hotplug interrupts enabled`, then `display: HPDn disconnected` / `connected` and `hotplug on pins ...`; `RDNA3,HotplugEvents` counts them.

| Symptom | Likely cause | Action |
|---|---|---|
| no `display: HPDn` line on unplug | DCN interrupts do not reach the IH (`DC_HPD_INT_EN` set but no IV) | `RDNA3,IHEarlyIVs`, MSI counters; read `HPDn_DC_HPD_INT_STATUS` (bit 0 latched?) |
| many `HPDn` lines for one unplug | polarity / ack order (`hpd_ack` in irq_service_dcn32.c) | send the log; check `DC_HPD_INT_POLARITY` after the ack |
| `connected` but the connector has no EDID afterwards | re-probe too early (sink not ready) | delay the re-probe (amdgpu waits through `dc_link_detect` retries) |

### Step `hotplug` (`rdna3hotplug=1`): hotplug to the framebuffer

The report's `hotplug window` section has `RDNA3,HPDState` (`HPD1 1 HPD2 0 ...`, debounced; `(now n)` = a look that has not counted yet) each time it changed, the kext / framebuffer lines (`hotplug:`, `connect change`, `mirror:`, `extend:`) and `HOTPLUG: N HPD change(s)`; then a `look hotplug` scanout picture. At start the log has one `hotplug: HPDn (...) filter set: connect 500 ms, disconnect 100 ms` line per VBIOS path (DP: 80 / 0 ms) and `RDNA3,Hotplug` says it polls.

| Symptom | Likely cause | Action |
|---|---|---|
| `HOTPLUG: 0` and `HPDState` never moved | the user missed the window, or `DC_HPD_SENSE_DELAYED` does not follow the cable | ask whether the cable was pulled; else compare `DC_HPD_SENSE` (raw) with the delayed bit: log `TOGGLE_FILT_CNTL` from the start line (a delay of 0xff x 10 ms would hide it) |
| `(now 0)` appears but no `hotplug: HPDn disconnected` line | the level went back before the second look (a glitch: `RDNA3,HotplugGlitches`) | a cable that bounces longer than the filter; if every unplug looks like this, raise `RD_HPD_DEBOUNCE` / check the 500 ms tick ran (`RDNA3,HotplugPolledEvents`) |
| several `disconnected`/`connected` pairs for one pull | the filter delays not taken (connect 500 / disconnect 100 ms for HDMI, 80 / 0 for DP) | the start line's `TOGGLE_FILT_CNTL`; `program_hpd_filter` / hw_hpd.c `set_config` units (10 ms) |
| `disconnected ... nothing to do` for the second monitor's pin | the clone's HPD pin is not the pin that moved (`fMirror->hpd` vs the VBIOS path) | `RDNA3,DisplayPath<n>` HPD numbers vs `RDNA3,Mirror`; `dchpd_decide` gets `clone_pin` from the clone plan |
| `take the second monitor's pipe down`, then a panic or WindowServer restart loop | the second framebuffer / accelerator head torn down under WindowServer (`displayMirrorDisable` terminates the RDNA3DisplayHead nub synchronously) | `23-windowserver-crash.txt`, the panic; next: hide the head first (connect change on head 1 with `kConnectionEnable` 0) and terminate after IOGraphics let go |
| `connected ... no EDID` | the DDC did not answer in 3 tries 100 ms apart (`dm_helpers_read_local_edid`) | `RDNA3,HotplugNoEdid`; the `AUXn/DDCn: HPD high, no DPCD ... no EDID on the DDC` line; raise `RD_HPD_EDID_TRIES` / `RD_HPD_EDID_RETRY_MS` |
| `light the second monitor` then `second monitor NOT lit` | the clone path refused (same as 9b/9c) | `RDNA3,Mirror` / `RDNA3,MirrorResult`: the 9b table below |
| lit again but macOS shows one display | the new RDNA3Framebuffer on the head nub did not start or did not publish | `RDNA3-fb: head 1` lines after the replug; `ioreg -c RDNA3Framebuffer` count in the report |
| the boot monitor's cable pulled: `boot head: pipe kept, reported` and macOS loses its only display | expected: head 0 reports `kConnectionEnable` 0 while its cable is out; the pipe keeps scanning | none if it comes back on replug (`RDNA3,HotplugBootHead`, fb line `the monitor on HPDn is back`) |

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
| `rdna3_vktest`: `draw` FAIL, pixels wrong | the graphics pipeline: all blue = the draw never wrote (PS export / CB), all 0x5a5a5a5a = the copy-out never ran, garbage = tiling/DCC of the image | first wrong pixel and its value; `hang:` dump if the fence timed out |
| `rdna3_vktest`: `images` FAIL | image tiling (swizzle modes, optimal layout) on GFX (compute/CP copy) or, for the transfer-queue line, SDMA tiled copies | the pattern encodes x, y in R, G: the first wrong pixel's value shows where it came from |
| `rdna3_vktest`: timeline semaphore FAIL | syncobj timeline UAPI (`SYNCOBJ_TIMELINE_SIGNAL/WAIT`, `QUERY`) | `DRM_XNU_TRACE=1`; the loopback passes it |
| `mtlprobe`: `METAL: FAILED no AMD device` | Metal did not load the plugin: `MetalPluginName: 0` in the section header = the accelerator did not publish it (registration of the heads, `rdna3accelfb`); 1 = the plugin refused (its log: allow list, `vk:` lines = RADV did not come up) | plugin log under the section; `20-log-rdna3.txt` for `MetalPluginName restored` |
| `mtlprobe`: FAILED `newLibraryWithSource` / pipeline | Apple's compiler service or the translator (`translate:` lines in the plugin log) | the plugin log; reproduce the AIR in the VM without the card |
| `mtlprobe`: FAILED command buffer / kernel result | the plugin's submit path into RADV, then the same as a vktest compute failure | `hang:` dump, vktest result of the same boot |
| `rdna3_vktest`: OPAQUE_FD import FAIL | PRIME export/import tokens (`RD_NR_XNU_BO_EXPORT/IMPORT`), or an ioctl struct size mismatch (uapi/ zero-extends shorter structs like `drm_ioctl` since 2026-10-10) | `DRM_XNU_TRACE=1` shows the failing ioctl nr |

### Step 9 / 10 (`rdna3fb=1`, `rdna3accel=1`)

| Symptom | Likely cause | Action |
|---|---|---|
| step 9 black screen, system alive | scanout address/pitch vs GOP | `12-ioreg-fb.txt`, `Head0EdidMode`; `fb/rdna3-fb.cpp` |
| step 10 WindowServer crash loop | `IOAccelConfig` / missing methods | `22-log-windowserver.txt`; `populateAccelConfig`; `accel/gen_stubs.py` |
| triage `!! two lit pipes scan the same surface` | a swap routed to the wrong head | serial log `swap #N: head X now gets head Y's frame` / `surface ... moves from head` / `... freed`: which rule (`kHow`) answered; `rd3AccelSwapToPanel` |
| `display sleep: FAIL` (no BLANKED pipe) | IOFramebuffer never set power 0, or the core refused | fb log `power 0: head N blanked`; core `display: head N blanked`; `rdna3dpms=1` in the boot args? |
| `display wake: FAIL` (still BLANKED) | power 1 not delivered / unblank lost | same lines with `unblanked`; `dcn32_set_blank` |
| step `gamma`: `GAMMA: FAIL - 0 of N pipe(s) moved with the dark table` | the table never reached the MPCC OGAM: boot-arg, fb not asking, core refusing, wrong MPCC, or MODE/SELECT not latched | fb log `head N: gamma ...` (no line: IOFramebuffer never called `setGammaTable`), core log `display: head N gamma ...` and ioreg `RDNA3,Head<n>,Gamma` (expect `table ... mode 2 RAM A/B`); `gamma: MPCC n OGAM memory does not power up` -> `MPCC_MEM_PWR_CTRL`; mode 2 and still no change: the head's MPCC walk (`dcn32_gamma_otg_mpccs`: OPTC_DATA_SOURCE_SELECT -> MPC_OUT_MUX) or the OTG lock in `dcn32_gamma_apply` |
| `GAMMA: FAIL - ... the restore did not reach the pipe` (moved, not back) | the ColorSync table did not come back, or the RAM ping-pong latched the wrong RAM | `== gamma pipe` lines: `identity` CRC vs `before` (saved table identity -> equal); `RDNA3,Head<n>,Gamma` call count went up after the restore? `mpc3_get_ogam_current` / `MPCC_OGAM_SELECT` in `hw/dcn32_gamma.c` |
| `GAMMA: FAIL - the picture in VRAM darkened` | WindowServer applied the table in the compositor (software gamma), so the OGAM is not proven | fb log: was `setGammaTable` called at all; IOFramebuffer gamma properties (`IOFBGammaWidth` / `IOFBGammaCount`) |
| `GAMMA: PASS`, but colours / Night Shift look wrong on the monitor | curve mapping: OGAM input taken as the encoded pixel value (DPP colour management in bypass, as the GOP leaves it); pixel value 0 always maps to 0 (start slope from the origin) | photo of the screen; same boot with the ColorSync profile set to sRGB; check the DPP's degamma / gamut remap (`CM_DGAM_*`, `CM_GAMUT_REMAP_*`) left by the GOP |

### Mode set and scaled modes (`modeset`, `modeset2`)

Every `rdna3_drminfo --scanout` look prints per powered pipe: `DET n x 64
KiB (compbuf m), DISPCLK .. kHz, DPPCLK .. kHz (pipe), DSCL mode, LB, mem
pwr, DLG after scaler y/x req .. line ..`. It reads the sticky underflow,
clears it, and the second look 2 s later says `UNDERFLOW again within 2 s`
(steady) or `no underflow in the 2 s after clearing it` (it came from a
switch). `rdna3_modetest cycle` first tries each display's fastest timing
of its own size (look `mode-d<D>-<W>x<H>-<Hz>hz`), then the smaller modes.
`modeset2` = `modeset` + `rdna3dispclk=1` + `rdna3scaledlg=1`.

| Symptom | Likely cause | Action |
|---|---|---|
| `UNDERFLOW again` at a scaled mode with `modeset`, not with `modeset2` | the 1:1 LB / DLG (card run 20, pipe 1 at 960x540) | verified: hw-verified, then `rdna3scaledlg` into the default profile |
| `UNDERFLOW again` with `modeset2` too, its pipe shows DSCL mode 1, LB `0x00001f01`, req delivery x (native / source width) | not the DLG. Compare the pipes: `DET` (the clone gets the smaller half, `dcn32_clone_plan`), `mem pwr` (LB_Gx force bits; the clone does not copy DSCL_MEM_PWR_CTRL), `DPPCLK (pipe)` below the pixel clock (DTO) | the differing one: equal DET split (`dcn32_determine_det_override`), copy DSCL_MEM_PWR_CTRL / `dpp1_power_on_dscl`, or `dcn32_clk_raise`'s DTO for the scaled pipe |
| `no underflow in the 2 s after clearing it` after a switch | transient while the scaler / viewport latched | picture fine: record, no fix needed |
| `modeset2`: `rdna3_modetest list` has no faster mode of the native size | the plan refused it (log `display: head N mode ... refused (4)`) or the limits read failed | `RDNA3,DCNClockLimits` (0 kHz = `GetDpmFreqByIndex` over the DAL mailbox failed; log `DAL SMU: PMFW ...`); a mode above the max is refused by design |
| `DCN clocks: asked DISPCLK .. the PMFW set ..`, mode set `-16` | the PMFW acked SetHardMinByFreq but did not move the DFS (DCN DPM off: `RD_SMU_FEATURES_SAFE` drops `FEATURE_DPM_DCN` unless SCPM runs it) | `PMFeaturesRunning` bit DPM_DCN; port the DCN DPM enable (`smu_v13_0_0` allowed mask) behind its own boot-arg |
| `DAL SMU message 0x9(...)`: no answer, PMFW mailbox dead afterwards | the DAL mailbox wedged as NumOfDisplays did in card runs 1-6 | drop `rdna3dispclk`; the serial log shows the first DAL message that hung |
| faster mode `now WxH 72.00 Hz`, picture right, the look's `DISPCLK` above the pixel clock x 1.015 | pass | hw-verified (`modeset2`), then `rdna3dispclk` into the profile |
| faster mode set, then `modeset: ... underflows / does not run: back to` | clocks fine, DLG not (rescaled GOP values) | the look's DLG line + serial `modeset:` lines; port the full `dml32_rq_dlg_get_dlg_reg` for that mode |

### DP link (`dplink`)

`dplink` = `modeset` + `rdna3dplink=1`. A DP SST head (DIG_MODE 0, the GOP
lit a native DP monitor) is offered modes and switched like a TMDS head,
through `hw/dcn32_dp.c`: link down (stream blank, `SET_POWER` D3,
TransmitterControl DISABLE), the DP DTO at the new pixel clock, the link
cap verified once (training at the sink's maximum, falling back), then the
link up at the smallest setting the mode needs. Serial / log lines start
with `dp:`; ioreg `RDNA3,Head<n>,DPLink` sums the link up.

| Symptom | Likely cause | Action |
|---|---|---|
| no `dp:` lines, the DP head offers only `current` | the boot-arg is missing, the head is not DP SST (`DCN: head n ... mode 0`?), the VBIOS path has no DDC line for the transmitter (`RDNA3,DisplayPath<n>`), or the sink gave no caps (`dp: ... gives no capabilities`) | `RDNA3,Connector<n>,DPCD`; a DP++ adaptor makes the head TMDS (mode 2/3): the `modeset` step covers it |
| every mode `refused (8)` | no link setting of the (verified) link cap carries the mode | `dp: AUX n: DPCD ..., max link ...`; the VBIOS encoder caps (`RDNA3,DisplayPath<n>` HBR2/HBR3) limit it too |
| `dp: link training HBR3x4 CR failed lane0 VS=3` then a pass at HBR2 | the fallback works: the cable / sink does not do HBR3 | pass; `RDNA3,Head<n>,DPLink` names the verified cap |
| every training `CR failed lane0 VS=0`, no VS change | the sink never saw the pattern: the PHY is not running at the link rate (TransmitterControl ENABLE through the DMCUB failed: `dp: TransmitterControl action 1 ... failed`), wrong transmitter, or `DP_DPHY_TRAINING_PATTERN_SEL` not reaching the PHY | the DMUB lines of the log; the head's `DIG_BE_CNTL` / `DIG_BE_EN_CNTL` in the look |
| CR done, `EQ failed` at every rate and lane count | pre-emphasis requests not applied: SETUP_VSEMPH's lane set (TransmitterControl v1.7 mode field) is ignored by the VBIOS table | the `lane status` / `adjust` debug lines; compare with the GOP's DPCD 0x103..0x106 |
| trained, `dp: DIG f -> UNIPHY b ... trained`, monitor black or "no signal" | stream side: M/N, MSA or the DP DTO (DPREFCLK) | look: `DP_DTOn_PHASE / MODULO` (phase = pixel clock in Hz, modulo = DPREFCLK in Hz), `DP_VID_M`, `DP_VID_STREAM_CNTL` status; `dp: DIG n: SYMCLK_FE_ON stays off` = the FE is not connected to the BE |
| `modeset: the DP link failed (-5): back to ...` and the old mode trains | the new mode's link setting does not train (see above) | record the rate / lanes; the old mode back is the expected recovery |

### Idle power (`rdna3pmexp=1`)

Triage prints one line per variant (10 s each): baseline, GFXOFF allowed,
workload POWER_SAVING, HDP/ATHUB gating, GFXCLK soft max lowest. Read
activity and watts per line: the first variant that drops activity below
~20 % or the watts below ~25 W is the lever - make it the default (port the
matching amdgpu default if it was a workaround). If only the GFXCLK cap
lowers the watts while activity stays 100 %, the PMFW's busy signal is the
problem (RLC/CP idle reporting), not the clock policy.

### Hang test (`rdna3hangtest=1`)

`HANG TEST: PASSED` = both the SDMA and the GFX ring recovered within the
10 s cap and ran a verified job afterwards. A `NOT recovered` line: the
`hang:` lines in the serial log name the path taken (soft recovery, SDMA
soft reset, GFX soft reset, mode-1 reset).
After each recovery the display is checked (`RDNA3,Head<n>,State`):
a GFX/SDMA soft reset must leave every head `intact`
(`display: GFX/SDMA reset does not reach DCN: ... LOST (unexpected)` =
the reset reaches DCN after all: send the serial log); after the mode-1
reset the heads are `LOST` unless `rdna3resume=1` restored them.

### Step `sleep` (`rdna3dpms=2 rdna3resume=1`): monitor standby, system sleep

Serial lines: `state: head N saved/link down/link up/restore/restored`
(`hw/dcn32_state.c`), `display: head N <why>: ...` (the check after each
event), `wake #N: PCI ..., MEMSIZE ..., ASIC_Init ..., PSP sOS ...`,
`resume #N ...` (`RDNA3Bringup::resumeAfterPowerLoss`).

| Symptom | Likely cause | Action |
|---|---|---|
| monitor keeps its signal in display sleep (only black), `display: head N blanked` | `dcn32_head_link` refused: no snapshot, not TMDS (`state: head N saved ... blank only`) or no DMCUB (`rdna3dmub=1`) | the `saved` line names the DIG mode and whether a VBIOS path was found (`displayHeadPath`: transmitter = DIG back end); DP heads stay on the blank by design |
| standby works, the monitor wakes without a picture | TransmitterControl ENABLE alone does not bring the stream back on the boot head | compare `state: head N link up (r)` with the clone's `clone: link up`; next: `enable_link_hdmi`'s `setup_stream_encoder` (STREAM_SETUP) before ENABLE in `dcn32_head_link`, modelled in the sim first |
| `SYSTEM SLEEP: the system did not sleep` | VM: guest S3 off, or an assertion | `pmset -g log` lines in the test output, `host-*.log` QEMU line with `ICH9-LPC.disable_s3=0`; `pmset -g assertions` in the VM |
| report ends at `system sleep`, `host-*.log` `still suspended after 90 s: system_wakeup` | the guest did not wake (RTC alarm lost, system_wakeup refused) | QEMU version / `info status` output in `host-*.log`; next round without `pmset sleepnow` (display part only) |
| `RDNA3,Wake ... the card kept its power`, every head `restored (0), running`, pictures and CRC like before | pass | `sleep <date> vm` into `docs/status/hw-verified.txt` |
| `RDNA3,Wake ... the card lost its power`, `WakeResumeResult 0` | `resumeAfterPowerLoss` stopped: `ResumeResult` 1 ASIC_Init, 2 FB location moved, 3 `hwInit` | serial `resume #1` and the `PSP` / `post` / `Stage2` lines after it; in the VM a wake brings a bus reset: `resetIfPspStuck` must log the mode-1 reset |
| heads `restored (-16)` after a power loss | DISPCLK / DPPCLK below the pixel rate: the clock manager (`dcn32_init_clocks`, `dcn32_update_clocks`) is not ported | `state: head N: DISPCLK ... no longer carry` line; port the hard minimums through `dcn32_smu_set_hard_min` (DAL mailbox) |
| heads `restored (-19)` | no DMCUB after the resume (`rdna3dmub=3` attaches to the GOP's firmware, gone after a power loss) | `rdna3dmub=1`; `DMUB:` lines after `resume` |
| restored, `running`, the picture black or garbage | VRAM did not survive (`VRAMLost`): the surfaces stay empty until WindowServer redraws | a picture a minute later; `sysctx 0` in the head state = the DCHUBBUB system aperture not restored |
| DMA/GFX tests after the wake fail, ring tests passed | GPUVM page tables lost with VRAM (the pool is not saved; see `docs/DRIVER-PLAN.md`, D4) | expected after a power loss; a fresh process (new VM) passing = only old VMs are broken |

---

### Step `vcnsleep` (`sleep` + `rdna3vcn=2`): the video block through a system sleep

Step `sleep`'s table first. The video part: the report's `--vcn-test`
before the sleep and `--vcn-test (after the wake)` with the VCN properties
before it; `RDNA3,VCNSuspends`, `RDNA3,VCNSuspendResult`,
`RDNA3,VCNResumes`, the log lines `VCN: suspended (JPEG 0, VCN 0), VCPU BOs
saved to system memory` and `VCN: resume (...): 0`. Order as amdgpu: the
display suspends first, then JPEG and VCN; on the wake VCN and JPEG come
back after GFX (in the bring-up after a power loss, from the wake work
when the card kept its power). Plain step `sleep` with any `rdna3vcn=` set
by hand runs the same path.

| Symptom | Likely cause | Action |
|---|---|---|
| `VCN: suspended (...)` missing in the log | setPowerState(0) came before the bring-up finished, or the block was not up | `RDNA3,VCN` before the sleep; the order of the `display: system sleep` and `VCN:` lines |
| `VCNSuspendResult = -12` (ENOMEM), `VCPU BOs NOT saved` | the copy buffers (2 x 644 KiB) were not allocated | resume then clears the BOs and sets fw_shared up again: the rings may still work; send the log |
| `VCN: the card lost its power in the sleep: the video block waits` and no `resume` line | power lost and `rdna3resume=1` missing / the GPU bring-up failed | step sleep's power-loss rows |
| `VCN: resume (...): -62`, `RDNA3,VCNResult` nonzero after the wake | the VCPU did not boot again: the PSP lost the VCN images (TMR) in the sleep, PowerUpVcn refused | `RDNA3,VCNTmrMC0/1` after the wake (0 = the PSP did not load them again); compare with step vcn2 |
| `--vcn-test (after the wake)` fence did NOT signal, before the sleep fine | fw_shared / stack restored wrong (the copy came back over a different BO layout) or the ring write-back page | `RDNA3,VCNResumes`, the IB test results after the wake |
| both `--vcn-test` pass, every head back | pass | `vcnsleep <date> vm` into `docs/status/hw-verified.txt`, regenerate |

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

### Card run 12 (build #91): step 10s

Same black desktop - but the run used the OLD accelerator (`accelerator
build FAIL 1e8fedbbfc`): the queue's first boot installed the new bundle,
after `rdna3vm approve` had already run, so nothing was waiting for
approval yet. autoreport now opens Privacy & Security with a dialog right
after it installs a new bundle on an interactive boot (approve / window).

### Card run 13 (build #91, step 10s with the new accelerator)

The boot-arg gates work: `iop ... hooks OPEN`, the display machine has head
0's pipe (`initFramebufferResource ... over the LIVE SCANOUT`), WindowServer
swaps frames - but every swapped frame is empty (`SWAP PROBE ... nonzero
0/32400`), `getApertureMemoryDescriptor: aperture gate CLOSED`, and
WindowServer restarts in a loop (PID 153, 299, ...): black screen, no
cursor. Next: 10a (`rdna3accelfb=4`, aperture open); the report now carries
the WindowServer crash reports (`23-windowserver-crash.txt`). The Metal
plugin (RDNA3MTLDriver.bundle, Stage 6) is not installed in the VM yet.

### Card run 14 (step 10a): the desktop through the accelerator

With the aperture gate open (`rdna3accelfb=4`): `getApertureMemoryDescriptor
-> over BAR1`, `SWAP PROBE 3 ... HAS CONTENT`, `rd3AccelFlipTo: FLIPPED`,
`FRONT SURFACE ... A LIVE WRITER IS PAINTING` - the user saw the desktop on
the GOP's monitor; no WindowServer crash in this boot (the .ips reports in
it were from run 13). Next: 10m, both heads; boot gates now apply per head.

### Card run 15 (step 10m): two heads piped, swaps refused

Both framebuffers got a display pipe (`heads piped 0x3`), WindowServer
composited (`swap a2 ... HAS CONTENT`), but the legacy swap path
(`RD3Surface::submitSwapCopy`) names no display, and with two heads the
flip/copy refused (`no way to tell whose surface this is`); WindowServer
never used `performTransaction` (which knows the pipe). Both screens black
with the cursor. Fix: `rd3AccelSwapToPanel` resolves the head (only head
piped / argument is the pipe resource / shares its vidmem / surface seen
before / next unclaimed head) and logs `swap: surface ... -> head N (how)`.
From now on one step per round: `big`.

### Card run 16-17 (step `big`, scanout pictures)

Both heads routed and flipped (run 16). Run 17's pictures: both pipes lit
with the macOS login screen, but at the end both scanned the first
monitor's surface, and the user saw the second monitor switch between its
own picture and the first one's. WindowServer re-creates pipe resources and
swap surfaces; the router matched stale pointers. Fixed in `2bcd676`
(surface keeps its head, freed objects leave the tables, route changes
logged); the report now looks three times, 20 s apart.

### Card run 18 (step `big2`, CI build 95, Metal plugin installed)

Every part ran (serial log: scanout looks, modetest, DMA/GFX, vktest,
mtlprobe reaching `MTLCompilerService`, soak, hang test), the VM shut down
by itself, but no report reached the host: the tests file was uploaded only
after collect.sh's archive, and collect.sh did not finish. Fixed in
`93cd123` (tests file and pictures first, collect.sh 360 s, `rdna3vm
result` shows the upload server's log). Power from the serial log:
baseline 47 W at 2.7 GHz, GFXOFF / POWER_SAVING / HDP+ATHUB CG change
nothing (48-56 W), GFXCLK soft max 500 MHz gives 21 W, the idle cap holds
15 W at 539 MHz (Linux: 12 W); activity reads 100 % throughout. Hang test:
the DMA hang was detected and SDMA0 soft-reset, but the follow-up jobs ran
on the guilty client's connection and got -ECANCELED, so the GFX hang never
ran; each job now has its own connection.

### Card run 19 (step `big2`, CI build 96): the GFX hang froze the host

Scaled modes (1600x900 and 960x540 on each head, then native), display
sleep/wake, DMA/GFX, vktest, mtlprobe and the soak all ran. The hang test,
now with one connection per job, detected and recovered the DMA hang, ran
the next DMA job, then submitted the GFX hang: no `hang: GFX` line followed
and the whole computer froze (the serial log ends in zeros: never written
to disk). The GFX hang is now its own step (`hanggfx`, `rdna3hangtest=2`);
`big2`'s hang test is DMA only. Every recovery step is logged with 100 ms
for the log to drain before it runs, and the host fsyncs the serial log
every 2 s, so the next attempt shows the step that takes the bus down.

### Card run 20 (step `big2`, CI build 97, Metal plugin installed)

Everything but Metal passed: three looks and display sleep/wake (both pipes
blanked, lit again), four scaled-mode switches and back to native (pipe 1
flags UNDERFLOW at 960x540, picture intact: watermarks for the scaled
source are not programmed), DMA/GFX jobs, rdna3_vktest 12/12 (triangle,
GFX and SDMA image copies, timeline semaphore), soak 283 rounds 0 failed,
DMA hang recovered, idle 14 W. Metal lists the card ("AMD Radeon RX 7800 XT
(RDNA3MTL over ...)", families up to Metal3); newLibraryWithSource failed in
the plugin: no air-runtime/libLLVM.dylib in the bundle and no air-opt. The
plugin now falls back to Apple's libLLVM in GPUCompiler.framework (the
plugin's GL path already uses its C API).

### Card run 21 (step `metal`, plugin with the system libLLVM)

The kernel compiled: Apple's front end, AIR read through GPUCompiler's
libLLVM, translated. Pipeline creation then hit `Assertion failed:
(mutable_size && mutable_align), radv_CreateDescriptorSetLayout`: RADV
indexes pMutableDescriptorTypeLists with the binding's position after
sorting by number, the spec ties list i to pBindings[i]; the plugin's
layout has binding 640 before 480..607. Reproduced by rdna3_vktest on the
host loopback; fixed in Mesa (patches/0007) and worked around in the
plugin (its bindings sorted by number), so the VM's RADV need not be
rebuilt for the next try.

### Card run 22 (step `metal`): Metal compute passes

`METAL: ALL PASSED`: the kernel compiled from source ran on the card
through the plugin (AIR read with GPUCompiler's libLLVM, translated to
SPIR-V, RADV pipeline with the bindings sorted), 4096 words right, GPU time
0.012 ms. Vulkan 12/12, DMA/GFX verified in the same boot.

### Card run 23 (step `metal`, build 98): Metal render passes

`METAL: ALL PASSED` with the rendered triangle (M3): vertex + fragment
functions compiled from source, render pipeline, a draw into a 64x64
texture blitted back, 2016 red pixels, 0 wrong off the edge. rdna3_vktest
this time hit its own mutable-layout check (added after run 21) and died
on the same RADV assert after 8 PASS: the VM's RADV was built before
patches/0007. Not a driver failure; rebuild RADV in the VM
(`build_radv.sh`, which applies every patch) to clear it. The check now
runs last in rdna3_vktest, so a RADV without 0007 only loses that line,
and triage reports "CRASHED after N PASS" instead of "not run".

### Card run 24 (queue `modeset modeset2 gamma sleep hotplug`, build 101)

Step `modeset` passed: each head offered 19 of its EDID timings (1080p72
at 210 MHz refused: DISPCLK 165 MHz, step `modeset2` raises it), the cycle
set 1680x1050@59.954 and 640x480@59.940 on both heads with their own
timings ("own timing" in the fb log), the desktop in every picture, no
underflow, back to native. The DLG reference clock the GOP's
REF_FREQ_TO_PIX_FREQ implies is 24245 kHz, not 100 MHz: the scaling from
the GOP's values absorbed it. Vulkan 13/13 (RADV rebuilt with 0007),
Metal passed. The queue then stopped on the host: before `modeset2`
vfio-pci did not take the card (`driver/unbind: No such file`): amdgpu was
still probing it after the previous step's restore. `detach` now waits for
udev, sets the override first and retries; the queue retries a step that
did not start once; `rdna3vm result` adds the queue runner's journal.

### Card run 25 (queue `modeset2 gamma sleep hotplug`, build 101)

Step `modeset2` passed: the DAL mailbox answered (PMFW 0x505c00, DISPCLK
and DPPCLK up to 1950 MHz), the hard mins raised DISPCLK 165 -> 220 MHz
and DPPCLK -> 215 MHz, 1080p@71.91 ran on both heads, the 60 Hz modes and
native as before, Vulkan 13/13, Metal passed. The queue stopped again on
the host: the passthrough of `modeset2` was SIGKILLed 6 s after the
restore's suspend/resume, `gamma`'s detach found the card still not
handed to vfio-pci after 5 tries, and the queue runner's journal ends
with the next suspend. `rdna3vm result` now adds the host kernel's
amdgpu/vfio/suspend lines of this boot and the one before; detach says
which driver holds the card; the pause between steps is 60 s.

Cause (the host's kernel log): at the first detach amdgpu logged
"finishing device" but its release ("ttm finalized") came 11 s later, after
vfio-pci had the card - a DRM file was still open (a killed compositor
waiting for its GPU work). The late release ran on vfio-pci's device; the
vfio-pci unbind after the VM then oopsed in vfio_pci_core_sriov_configure,
which SIGKILLed the passthrough, left the device locked (no vfio-pci for
`gamma`) and froze the host on the next suspend. `detach` now waits for
amdgpu's release (the kernel log, 30 s at most, killing every process
that still holds /dev/dri or /dev/kfd) before vfio-pci may bind, and gives
up rather than bind early.

### Card run 26 (queue `gamma sleep hotplug`, build 101)

The host held (detach waited for amdgpu's release). Step `gamma` FAILED:
no `gamma` line from the framebuffer or the core at all - IOFramebuffer
never called setGammaTable, because the framebuffer did not publish
IOFBGammaWidth / IOFBGammaCount. Pipe 1's CRC moved once and stayed (the
cursor; the picture did not change), pipe 0's never moved. The framebuffer
now publishes 12-bit x 256 with rdna3gamma=1 and logs the first call. The
result was collected in the pause between two steps (queue still running,
`sleep hotplug` left): `rdna3vm result` now says so, `rdna3vm queue status`
shows the queue.

| Symptom | Likely cause | Action |
|---|---|---|
| GAMMA: FAIL, no "first setGammaTable" line in the fb log | IOFramebuffer does not hand gamma down | check IOFBGammaWidth/IOFBGammaCount on RDNA3Framebuffer in ioreg |
| "first setGammaTable" logged, no "gamma ... -> 0x0" | the core refused (rdna3gamma=1? DCN head?) | core log `display: gamma` |
| the call reaches the core, the CRC does not move | OGAM not reaching the pipe (MPCC routing, memory power, MODE_CURRENT) | `RDNA3,Head<n>,Gamma` state |

### Card run 27 (queue `gamma sleep hotplug`, build 102)

- `gamma`: ran on the stale framebuffer (498e4d3fdc; the new one loads
  from the Auxiliary KC only from the next boot), no result. autoreport now
  stops a step that installed new kexts and the host's queue runs it again.
- `sleep`: display sleep/wake passed (both pipes blanked, both lit again).
  The system sleep woke on its RTC alarm with the card powered off (PSP
  sOS gone, both heads lost); with rdna3resume=1 the bring-up ran PSP,
  firmware and SMU and then hung for good: resumeAfterPowerLoss runs under
  fDrmLock, and pmInit's drmSetClocks took it again (IOLock, not
  recursive). gpuReset (from recover(), also under fDrmLock) had the same
  deadlock - the likely reason the GFX hang test froze the VM. The drm*
  setters now skip the lock when the reinit's caller holds it
  (drmHeldHere/drmHeldByMe).
- `hotplug`: passed - DP-3 pulled, its pipe down; plugged back, EDID re-read
  and the pipe lit again; the framebuffer told; both pipes lit at the end.

### Card run 28 (queue `gamma sleep hanggfx`, build 103)

- `gamma`: the new framebuffer ran (IOFBGammaWidth 12 / IOFBGammaCount 256
  in ioreg) and still no setGammaTable call. With the accelerator's
  display pipe (rdna3acceliop=1) the tables probably come in display-pipe
  transactions without a plane. Diagnostic build: the report shows the
  pipe counters around the gamma test, IOAcceleratorFamily2's transaction
  and gamma symbols and the first empty transactions dumped; step `gamma0`
  runs the same test without the accelerator.
- `sleep`: system sleep with the card powered off; the resume bring-up ran
  to the end ("resume #1 done, rings back up", the reinit deadlock fixed),
  head 0 restored and running, head 1 restored but its check says LOST
  (routing). The tests after the wake ran (drminfo, vktest) but nothing
  reached the host: the VM's vmxnet3 did not come back from S3. autoreport
  now restarts the interface after the wake, and results that could not go
  up are kept and sent at the next boot.
- `hanggfx`: passed - GFX hang noticed, waves killed, GFX soft reset, the
  next GFX job verified (this step froze the VM before the deadlock fix).

### Card run 29 (queue `gamma gamma0 sleep`, build 104)

- The new RDNA3Accelerator (gamma diagnostics) waited for approval in
  macOS: the old one ran, no transaction dump. Needs `rdna3vm approve`.
- `gamma` and `gamma0`: rdna3_modetest saw 0 displays (both pipes lit,
  WindowServer had both framebuffers open); run 28 saw 2. The report's
  daemon now runs rdna3_modetest in the console user's session
  (`launchctl asuser`) and the tool asks CoreGraphics again for 30 s.
  Display pipe counters: 65 plane-less transactions at boot, none during
  the (not run) gamma test.
- `sleep`: two system sleeps in the boot, one with the card powered, one
  without; both times the GPU came back (resume with the full bring-up),
  head 0 restored and running, head 1 restored but its check says LOST
  (routing). Again no network after the wake: when nothing went up, the VM
  now restarts once and that boot only sends the waiting results.

Second monitor after the system sleep: no signal. Cause: the clone's
standby (rdna3dpms=2, dcn32_clone_link) did not mark its link down, so the
snapshot before the system sleep caught the clone with its transmitter off
and the wake restored exactly that. The clone path now snapshots the
running head before the link goes down and marks it (fHeadLinkDown), the
wake brings back the link of every head macOS unblanked while asleep
(displayLinkUp), and hwInit's displayMirror leaves a lit clone alone on a
reset or resume.

### Step `gamma` with the accelerator: plane-less transactions to the base class

Card run 30: with the accelerator's display pipe, macOS sends the gamma
tables as display-pipe transactions without a plane
(`debug.rdna3accel_iop_empty` +6: 3 tables x 2 heads) and never calls
`setGammaTable`; without the accelerator (`gamma0`) `setGammaTable`
reaches the framebuffer and the OGAM works. With `rdna3gamma=1`,
`RD3DisplayPipe::performTransaction` now hands a plane-less transaction to
IOAcceleratorFamily2's own `performTransaction` (IOAccelLegacyDisplayPipe's
if it exports one, else IOAccelDisplayPipe's; weak imports, so a missing
symbol cannot stop the kext loading) and returns its result. The next run
of step `gamma` should show the accelerator log line `iop: plane-less
transaction handed to <class>::performTransaction -> 0x0`,
`debug.rdna3accel_iop_empty_base` going up with `iop_empty`, and then the
framebuffer's `first setGammaTable` / core `display: head N gamma` lines
and the CRCs moving as in `gamma0`. `iop_empty_base_err` going up (or the
log line with a non-zero result) means the base refused the transaction;
`IOAcceleratorFamily2 exports no performTransaction` means neither symbol
resolved (the transaction is then answered as before). A refusal goes back
to WindowServer as the base's error; booting without `rdna3gamma=1` brings
back the old answer.
