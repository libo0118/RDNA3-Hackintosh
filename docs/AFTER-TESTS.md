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
   A `FAIL STALE PLUGIN/RADV` line means the VM ran an old Metal plugin or
   RADV: see its row in the failure table below before reading any Metal result.
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
   is removed from that file. A big step (`big3`, `big4`) runs many steps'
   features in one boot: judge each feature's tests in its report by that
   step's table in §3, and every step whose checks all pass goes in on its
   own line with the note `(big4)`. A failing feature is bisected with its
   own step (`rdna3vm step <name>`).
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
| hang / black screen at step 0 | VM firmware, not the driver | ask for `rdna3vm step off`. If that hangs too: VM setup — re-dump VBIOS with amdgpu loaded, Resizable BAR off, larger `X-PciMmio64Mb` (`PCI_MMIO64_MB`), `CPU_MODEL=Penryn,…` in `~/rdna3-vm/config`; a bigger BAR0 (`RESIZE_BAR_LOG2` above 8) is the first thing to set back to 8 |

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

### Probes of `big4` (`rdna3probe=1 rdna3pmexp=1`): the card-only plan items

These steps do not pass or fail the round: they are what the next round of
each plan item (docs/DRIVER-PLAN.md) starts from. Triage prints one line per
item in the SUMMARY; the full output is in the tests file under `== probe:`.

| Line | Plan item | What it tells | Next |
|---|---|---|---|
| `P2 variant 0..5` (from `PM experiment` + `PM: RLC_STAT` in the kernel log) | P2 idle power | activity and watts per variant with the kernel queues on the MES (`rdna3amdgpu=1 rdna3mes=1`), and whether the RLC gated the GFX clock after each (`GPM_STAT` clk 0 / ls 1) | activity below 100 % with MES: make MES-mapped queues the default; still 100 % with GPM_STAT clk 0: the busy signal is not the clock - compare `RLC_PG_CNTL` / `CGCG_CGLS` with the Linux dump (`tools/linux/card_dump.sh --regs`) |
| `R3 platform`, `R4 macOS` | R3 bare metal, R4 macOS 26 | VM or bare metal, the macOS / Darwin version, the card's state at load (`NeedPost`, `PostResult`, `PSPStuckAtInit`), whether the accelerator runs on this Darwin | bare metal only from a bare-metal boot (`package/oc_install.sh --step big4`); R4 only from a VM with macOS 26 and the NM_TAHOE build |
| `V3 Vulkan WSI` | V3 present | the WSI extensions RADV offers; with a headless surface, a swapchain cleared and presented | `no present path`: RADV is built without WSI (`-Dplatforms=`): the next step is Mesa's WSI common with a headless or Metal-layer platform |
| `A3 IOSurface` | A3 IOSurface <-> BO | an IOSurface as a Metal texture: CPU -> GPU and GPU -> CPU pixel counts | `newTextureWithDescriptor:iosurface: returned nil`: the plugin has no IOSurface textures (its log after the probe); counts wrong: the surface's backing is not the memory the GPU used |
| `M4 CAMetalLayer`, `M4 on the screen` | M4 present | drawables, rendering, presents; magenta pixels in the scanout picture taken while the window showed | drawables NONE: the layer refused the plugin's device; presented 0 or no magenta on the screen: the drawable never reaches WindowServer's composition |
| `M5 WindowServer` | M5 | which processes loaded the plugin, whether WindowServer did, system_profiler's Metal line, WindowServer's GPU messages; the allow list and its mode (`allow mode: apps` / `list`) | WindowServer does not load it (not on the allow list): an M5 step with WindowServer allowed is the next experiment |
| `M6 apps`, `M6 Chess` / `Maps` | M6 apps | running, plugin loaded, crash report | a crash report: its first 40 lines in the tests file name the Metal call that failed |
| `M6 screens`, `FAIL M6 screen: pipe N (main display) showed the same picture in all M6 looks` | M6 apps on the panel | whether each pipe's hi-res picture changed (more than 1 % of the pixels between two consecutive looks; a clock tick is not enough) while Chess, Maps and Settings opened. Judged only on the main display's pipe (head 0's OTG from `RDNA3,Head0,Route`: the boot head, main unless the menu bar was moved), only when every M6 app ran and with 3 looks or more; the M6 looks keep their `pipe N:` state lines | a frozen main screen (card run 58): WindowServer drew where the head does not scan out. Read `A3 display` and the `a3 display: head N:` lines: `kept out of A3` and the screen still frozen is a new cause (the copies or the swap); `WINDOWSERVER DRAWS WHERE THE PANEL DOES NOT READ` means rdna3a3display=1 was set |
| `A3 display` (`a3 display: head N: ... legacy swaps`, `surfgem: ... refused: WindowServer's display framebuffer`), `A3 display counters` | A3 and the display | card run 58: WindowServer's display framebuffer IOSurfaces (the size a framebuffer scans out at the import, or of the head's own swaps) stay out of A3 (`common/rdna3_a3_display.h`); the legacy swap scans out the legacy surface's buffers (`it scans out phys`). Counters dumped before the probes, after M6 and in `29-surface-vram.txt`: `debug.rdna3accel_a3display_refused` / `_imported` / `_other` / `_task_mismatch` / `_noname`, `swaps_h<N>`, `swaps_a3_h<N>` | `FAIL A3 display: the refusal never fired`: WindowServer got BOs of a scanout size and nothing was refused (the kernel's marker counts only when it looked at WindowServer imports: `other` + `unchecked` > 0; `debug.rdna3accel_iop_flip=1` is INFO) - the `surfgem: WindowServer surface ... is not a display framebuffer (heads, scanout/swap size: ...)` and `display check did not run` lines say why. `FAIL A3 display` with `rdna3a3display=1`: no legacy swap ever scanned out an A3 BO - the swap must flip the IOSurface's own VRAM before display surfaces can join A3. `debug.rdna3accel_iop_flip=1` turns the rule off (that path flips the IOSurface's own VRAM) |
| `flicker` (`FAIL` / `WARN flicker: head N: alternating pictures (candidate flicker)`, kernel `flicker: head N: ... A B A B in K of 8 bands`, `debug.rdna3accel_swaps_abab_h<N>` / `swaps_ababwide_h<N>` / `swaps_hashed_h<N>` / `swaps_hashskip_h<N>`) | the panel | card run 58 (2026-10-15d): per band of the picture (the top 32 rows - menu bar, status items - and 7 bands below, 48 columns each) the swaps where any band showed its picture of two swaps before and not the one before, against the swaps hashed (`swaps_ababwide_h<N>`: those where more than one band did). Measured from the dump `== A3 display counters before the probes` (taken right before the probes, which run with `debug.rdna3accel_flicker=2` and put it back after M6) to `== A3 display counters after M6`; a run without the probes: since boot. 25 % or more of 60+ hashed swaps is FAIL when more than one band alternated in 25 % of them, or when two heads or more are over the line; one band on one head is WARN (a blinking caret, one status item - or flicker in a small area; a kernel that does not count the bands is judged by the heads alone). Fewer than 60 is not judged, no hashed swaps is NOT MEASURED (`debug.rdna3accel_flicker`: 0 off, 1 the first 3000 hashed swaps per head, 2 always) | with `A3 WindowServer ... kept out`: the copy path itself (the plugin's per-process copies of the display surfaces, copy forward); with `rdna3a3ws=1`: WindowServer's A3 inputs, as in 2026-10-15d |
| `A3 WindowServer` (`A3: WindowServer's inputs are kept out`, `surfgem: ... refused: a WindowServer input`) | A3 | card run 58 (2026-10-15d): every IOSurface WindowServer imports (window backing stores, menus) stays out of A3 by default and goes through copies; applications keep A3 for their own and shared surfaces. A caller the kernel cannot name is refused too (it might be WindowServer). `rdna3a3ws=1` lets them in, `rdna3a3=0` turns A3 off for every process; counters `debug.rdna3accel_a3ws_refused` / `a3off_refused` / `a3display_task_mismatch` / `_noname`; each refusal's line names its own cause (`a WindowServer input`, `imported for another task than the caller's`, `a process the kernel could not name`, `A3 is off`), all of them the policy, not a warning. `FAIL ... WindowServer input leaked into A3`: a WindowServer `item 7 ... -> the family's VRAM` line, or more non-display imports than refusals | flicker gone with them kept out: A3 for WindowServer's inputs needs an ordering of the owner's GPU / CPU writes with WindowServer's reads first (DRIVER-PLAN N27) |
| `display sleep: not judged by the look`, `FAIL display sleep: never blanked a head`, summary `display sleep` | display sleep | card run 58 (2026-10-15d): the VM went on into system sleep after the display slept; triage takes the kernel's `display: head N blanked (display sleep)` lines (archive log or serial log) instead, FAIL without any, INFO when the report has no kernel log at all (`not judged: ... no kernel log`). autoreport sets system sleep to never for the look only when it read the old value, saves it to `/var/db/rdna3-sleep.bak` first and restores it after the look, at the start of every later run and (standalone) at exit (`system sleep restored to N min` / `NOT restored`: `pmset -a sleep <minutes>` by hand), holds `caffeinate -s -i`, and prints the assertions and the power log (`== power before / after`) | the power log names what slept the system |
| `swap/flicker` (serial log: `flicker: head N: ...`, `a3 display: head N: ...`, `swap: head N: ... swaps over K buffer(s) ...`, `copy forward / swaps: head N at swap M ...`), `copy forward` (summary: `hN X from the shown buffer / Y not needed / Z from the front / M missed (H healed) / ...`) | the panel | card run 58b, second run (2026-10-16): a head whose swaps take turns between two back buffers needs copy forward from the surface's own buffer it showed last (`cf_shown_h<N>`); `WARN ... two back buffers took turns while copy forward copied nothing` is a kernel before `rdna3_swap_front.h`. Flicker from a serial log alone (the newest serial log only; a head the newest report's counters judged, 60+ hashed swaps, gets its serial flicker line as INFO `(serial said ...)`, the report decides; copy forward and swap warnings stay): the snapshot with the most hashed swaps per head (`at swap N`), FAIL with 25 % of 60+ hashed swaps A B A B in more than one band or on two heads, under the line only INFO (later swaps are not in the log). Copy forward WARN: copies impossible (`no head`, `shown buffer not usable`, `refused`), missed and not healed (`cf_miss_h<N>` > `cf_healed_h<N>`: the rects a buffer missed stay pending for that buffer and are copied with the next copy forward into it, from a source that did not miss them itself; too many become the whole picture, copied only with the copy engine on; a source that missed the same region blocks them, after 3 blocked calls they are copied anyway so both backs agree: `cf_pend_blocked` / `cf_pend_forced`; `cf_pend_evicted`: a list dropped from a full table, WARN; `cf_pend_whole`: lists that overflowed), the head showed another surface's buffer (`cf_foreign_h<N>`, never a source), more than 10 calls with nothing shown yet. Swaps: WARN when a head skipped 10 % or more (an empty back; the third in a row is shown as a black or clear frame, `swaps_emptyshown_h<N>`) or a swap matched no head (`swaps_nohead`), FAIL when a head skipped 10 or more swaps and showed none, WARN when swaps were skipped with no head named (`swaps_skipped_all` above the heads' sum) | the reason lines: `copy forward #n: ... not copied (SDMA refused ...)`, `... is not usable now`, `... has shown nothing recorded yet`, `... shows a buffer that is not this surface's`; `swap: head N: the back ... shows nothing (<why>)`; `swap: surface ... matched no head` |
| `flip latch` (report counters `swaps_latch*_h<N>` (`pend`, `wait`/`us`/`max` for waited flips, `immediate`, `timeout`, `unknown`, `off`, `waitsoff`: 1 while the head's waits are off), `swaps_flipfailed_h<N>`, `swaps_torn*_h<N>`, `swaps_inplace_h<N>`, `cf_onpanel_h<N>`, `swaps_shownfreed_h<N>`, `swaps_empty{filled,still}_h<N>`, `swap_latch`, `swap_torn`; from the tests file, else the archive's `29-surface-vram.txt`; serial `swap latch: head N: ...`, shown under `swap/flicker`) | the panel | card run 2026-10-17: windows vanish for a moment and the wallpaper shows (both screens) while copy forward and the A B A B test look fine. `N of M swaps: the previous flip had not latched` (WARN): the swap returned before the panel left the old buffer and WindowServer wrote into it - with `swap_latch 0` that is the cause, with 1 it must be 0 (timeouts: `flip(s) never latched in time` - 2 OTG frames after the flip, or 2.5 of the head's frames, 35-100 ms, when the frame counter stops; a head stops waiting after 3 in a row, `its swaps were NOT waiting for their flips at the end` while still off, until a flip is seen latched; with the frame counter stopped the framebuffer also says `flip to VRAM ...: the previous flip still pending after 20 ms`). `copy forward(s) wrote into the buffer on the panel` (WARN): the same seen from copy forward. WARN too: `swap_latch was 0`, flips not waited for (`off`: waits off after 3 timeouts until a flip is seen latched, or a flip given up), `the flip state never read pending - the wait did nothing`, refused flips, `torn test not measured` (INFO with `swap_torn 0`), `counters missing` (a key absent is unknown, never 0; with the kernel built from these sources and no latch counters at all). `N of K shown frames changed while on the panel, then flipped away (torn)`: FAIL from 5 % of 60+ checks - something writes the buffer the panel shows after its swap (not H1: look at `swaps_shownfreed`, the plugin's surface-out and the family's page-on timing); `rewritten in place` (WARN from 25 %): one back buffer, the zero-copy flip shows the buffer WindowServer draws into (DRIVER-PLAN N28 (1)). `empty backs ... read again 4 ms later: N had pixels`: the frame landed after the swap asked for it (skips that should have waited) | the reason lines: `swap #n: head N: the previous flip (to phys ...) had NOT latched at this swap`, `the flip to phys ... had NOT latched ... us after it`, `the buffer it showed (phys ...) changed while on the panel: bands 0x..`, `copy forward #n: head N: the target ... is STILL ON THE PANEL`, `swap: head N: the buffer ... was FREED while the head scans it`, `swap: head N: the empty back ... read again ... us later` |
| `swap copy` (counters `swaps_copy*_h<N>`, `cf_sc*_h<N>`, `swap_mode`), `frame trace` (H-B2 / H-COPY / H-FREE / CF-ERROR / VRAM / PANEL / GPU, `A/B mode N ...`, `A/B verdict`, `Qn` / `trace incomplete: Qn`, the timeline), `copy forward: CoreDisplay disabled copy forward` (CRITICAL) | the swap as a copy and the per-frame trace | card run 2026-10-17b: the swap copies the back into the scanout buffer the panel does not show and flips to it (mode 1), the trace records every frame; see "Card run 2026-10-17b" for the coverage matrix (Q1-Q11) and the symptom -> trace -> action table | the A/B verdict, then the hypothesis lines; any `trace incomplete` first (the next run must answer it) |
| `GAMMA: UNJUDGED`, `gamma (open)`, progress `UNJUDGED (exit 3)` | gamma | no pipe stayed still between the looks (`CRC before NOT stable`, `0 of N pipe(s) still`, card run 58: the flicker), so the CRCs prove nothing either way (exit 3, `[exit 3: UNJUDGED, not a failure]`, never cached as passed); a still pipe next to a changing one is judged on its own and fails when it did not move with the dark table (`GAMMA: FAIL - N still pipe(s) did not move`); the core's `RDNA3,Head<n>,Gamma` record before and after the test decides: every head took 3+ tables is WARN (open), else FAIL | judge the flicker first, then the gamma on still screens |
| `VM faults`, `VMID N` (SUMMARY, from the serial log) | VM faults | the faults grouped by VMID and address with the UTCL2 client (`gfxhub_client_ids`) and the owner | card run 58: Terminal's VMID 5 TCP reads at an unmapped VA, each followed by 4 CPC writes at 0 on VMID 0 (not a second bug: they come with every client fault) |

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
| a user queue dead after a GPU reset or a system sleep | the restarted scheduler did not get it back: after a reset `user queues after the reset: mapped again` (`RDNA3,UserqResetFailures` 0; `hung (VRAM lost)` is amdgpu's behaviour), after a sleep `amdgpu_userq_suspend` / `_resume` (`wake: N user queue(s) not back`) | the `ADD_QUEUE` / `REMOVE_QUEUE` lines around `suspend #N` / `resume #N`; the queue's MQD came back with the evicted VRAM (`resume: N VRAM range(s) back`) |
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
| `STALE KEXTS` FAIL; user: everything blue where black should be, the screen redrawn top to bottom about once a second, one monitor | the build was not approved: macOS ran the framebuffer/accelerator approved earlier. One older than the core's display ABI (`RD3K_DISPLAY_ABI_VERSION`) got every call refused and fell back to console mode (`RDNA3,Mode` = `console`, `Unknown Display`, flips refused, CPU copies); its console base `0x80000001` carried IOGraphics' flag bit, so every pixel sat one byte off (alpha in blue; scanout mean rgb ~84 95 254). Found 2026-10-12 | the core answers display ABI 1 and 2 (`RD3K_DISPLAY_ABI_OK`), the framebuffer masks the console base's low bits, the queue stops for `rdna3vm approve` (`approve-*.txt` from autoreport's `stale_kexts`). Results of such a run about the display, the accelerator and Metal on screen do not count: `rdna3vm approve`, then the step again |
| `STALE PLUGIN/RADV` FAIL | the Metal plugin and/or RADV in the VM are older than the code in the repo while the kext is current: `rdna3vm update` installs only the kext; the plugin and RADV are built inside the VM (card run 2026-10-15 tested the Chess crash fix eb5e3e8 and the N1/A3 plugin work with an old plugin and old RADV, and nothing said so). Decided from `35-build-stamps.txt` (written by `build_metal.sh` / `build_radv.sh`: the last `metal/` resp. `userspace/mesa` commit of the source built) against the same in this repo at the commit `00-host.txt` names, when the stamp's commit is provably older; with no stamp (reports before stamps, RADV from a CI artifact) from the plugin log: the `vk: G14 primitive topology` line without the `vk: VK_EXT_extended_dynamic_state ENABLED/ABSENT` line | the plugin / RADV results of this run (Metal, Chess, M-steps, `rdna3_vktest`, CTS) do not test the current code: do not debug them. Ask the user (Polish) to `git pull`, `rdna3vm window` and in the VM Terminal run `build_radv.sh`, then `build_metal.sh` (docs/TESTING-VM.md, "With Vulkan" and "Metal plugin"), shut the VM down and run the steps again |
| `AGDC build` / `AGDC object` FAIL (step `agdc`, `rdna3agdc=1`) | RDNA3AGDC.kext not approved or not loaded (`AGDC not loaded`), or the object was not made: `RDNA3-agdc:` lines say why - `not making one` (Apple's AppleGraphicsDeviceControl has another size on this macOS: the reconstructed header needs that release's layout), `start failed`, `no RDNA3Framebuffer` | approve and run again; a size refusal: note the macOS build and the size from the log, then compare `agdc/AppleGraphicsDeviceControl_re.h` with that release's AppleGraphicsDeviceControl |
| `planes in transactions` FAIL with the AGDC object up | IOPresentment still sends only plane-less transactions (`rdna3accel_iop_empty` grows, `iop_ok` 0): read `== display pipe and AGDC counters` - `rdna3agdc_cmds` 0 = nobody asked; `rdna3agdc_k5_link`/`_caps` 0 with `_k5_asked` set = a K5 query refused (`rdna3agdc_k5_why`, `_last_refused_idx`); `rdna3agdc_fbmap` must list every lit head from index 0 | `k5_why` 0x31: the framebuffer published no `RDNA3Timing` (console head); 0x11/0x21: an index past the table; others: the timing words (`rd3k5_timing_bad`) |
| `planes in transactions` PASS | WindowServer hands each frame to the display pipe (`iop_ok`, `iop_src_surf`): the copy / flip path of `RD3DisplayPipe::performTransaction` now carries the desktop, not the swap path | compare flicker, the second monitor and gamma with the step without AGDC; if they are better, `rdna3agdc=1` goes into the profile (`common/rdna3_bootargs.h`) |
| step `relight` / `big4` (`rdna3hotplug=2`): the replugged monitor black or the other one moved | `RDNA3,HotplugRelights` / `HotplugRelightFails` and the core's `hotplug: head N's monitor is back: link down and up again -> r`; the framebuffer's `framebuffer kept, IOGraphics not told`. r != 0: the link bring-up failed (`displayHeadDpms` / `dcn32_clone_link`: a DP head without its snapshot, no DMCUB); no relight line: the pin was not the boot head's or the clone's | a failed relight on a DP monitor: the link training lines (`dp: link`) around it; on HDMI the pipe never went down, so a black picture with r = 0 means the sink did not sync to the TMDS signal again |
| panic `Taking non-sleepable RW lock with preemption enabled` in `namei` from `RDNA3Bringup::loadFirmware`, < 1 s after boot | the firmware was read before the root file system existed (run 6, once) | loadFirmware waits for the IOBSD resource and `vfs_rootvnode()` |
| `PM:` lines: ~197 W, gfxclk ~2700 MHz, activity 100 % with nothing submitted | GRBM_STATUS idle (0x0000302c) while the PMFW counts 100 %: no GFX clock gating, the GFX clocks never stop (amdgpu gates them in late init) | FIXED in part (run 8): GFX clock gating (gfx11_set_clock_gating, default on, `rdna3cg=0` off) took the idle from ~200 W to ~75 W, hotspot 78 -> 55 C, fan stops; the PMFW still reports 100 % activity and ~2.7 GHz target. Step 8o (run 10, GFXOFF allowed): AllowGfxOff accepted, DMA/GFX tests pass, no hang, but no change (~75 W, 100 % activity): the PMFW never sees the GFX idle, so it never enters GFXOFF either. Open: what keeps the PMFW's GFX activity at 100 % while GRBM_STATUS is idle (RLC / MES state); the rest of the idle power is UCLK/FCLK pinned at their top level for the GOP's scanout |
| `clone: pipe N power on timed out` | `DOMAINn_PG_CONFIG` refused (IP_REQUEST_EN) | `dcn32_hubp_pg_control`; try with the pipe the GOP left powered |
| `DET… timed out` | DET split not taken (COMPBUF in the way) | log line has `COMPBUF`; shrink COMPBUF first (`dcn32_program_compbuf_size`) |
| `SetPixelClock / STREAM_SETUP / TransmitterControl through the DMCUB failed` | DMCUB rejected or hung on the VBIOS command | `DMUB: command ... not consumed`; check payload sizes vs dmub_cmd.h |
| `MirrorCheck`: NOT running | the clone OTG never started: pixel clock (DCCG `OTGn_PIXEL_RATE_CNTL`, PHYPLL) or VTG | the `clone: pipe N lit` line; compare `OTG_CONTROL` of both OTGs |
| `MirrorCheck`: running, CRC DIFFERENT | the pipe scans out something else: HUBP surface/viewport, DPP/MPC/OPP copy | send the log; a black clone has a constant CRC |
| result 0, `MirrorCheck` running + same CRC, second screen black or "no signal" | PHY/PLL side: `PHYPLL` / `PIXEL_RATE` in the `clone: pipe N lit` line still 0, or the HDMI sink wants infoframes | send the log; compare the source and clone `OTGn_PIXEL_RATE_CNTL`, `PHYxSYMCLK_CLOCK_CNTL`; next: write PHYPLL/SYMCLK source like the GOP's |
| first screen flickers or goes black after the clone | DET split too small, or watermarks | send the log; the DET line shows the split |
| a monitor says "no signal" while the look says its pipe is `LIT`, the OTG CRC moves and the picture is right (card run 57: the HDMI boot head) | the pixels leave the OTG but not the DIG / PHY: back end or symbol clock off, a DIO / DIG FIFO error, AV mute, DRR bounds of 0 under `V_TOTAL_MIN/MAX_SEL`, an HPO encoder on the PHY | the look's `TX` lines (`TX problems: ...`, the registers per block) and the serial log's TX watch: `display: head N TX at the start` (as the firmware left it), `... after the clone` (the clone of `rdna3mirror=1` is watched as `clone`), `TX change #n at T ms uptime: problems: ...; registers` (what changed, when). A change names the step that did it (compare T with the test steps). `DIG chain not found`: the DIG was off or not routed to the OTG - the `every DIG` line says which DIG is on and what it takes. Trust "no change" only with `RDNA3,TXWatchTicks` > 0 (triage says `NEVER TICKED`, or `NO TICK` when every tick found the display lock busy, otherwise); `OTG enabled but not running` / `pixel clock DTO enabled but not running`: a requested enable whose status never came; `TX not read: ...` / `TX unavailable` say why a head was not read. `problems: none` and no change with a dark monitor: the PHY / PLL side the DCN does not show, or the sink - ask whether it was dark from the boot screen on |
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
| step `gamma`: `GAMMA: FAIL - 0 of N pipe(s) moved with the dark table` | the table never reached the MPCC OGAM: `rdna3gamma=0` (gamma is on by default since card run 56), fb not asking, core refusing, wrong MPCC, or MODE/SELECT not latched | fb log `head N: gamma ...` (no line: IOFramebuffer never called `setGammaTable`), core log `display: head N gamma ...` and ioreg `RDNA3,Head<n>,Gamma` (expect `table ... mode 2 RAM A/B`); `gamma: MPCC n OGAM memory does not power up` -> `MPCC_MEM_PWR_CTRL`; mode 2 and still no change: the head's MPCC walk (`dcn32_gamma_otg_mpccs`: OPTC_DATA_SOURCE_SELECT -> MPC_OUT_MUX) or the OTG lock in `dcn32_gamma_apply` |
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
| `display: head N: the DP link lost its lock: trained again (0)`, `RDNA3,DPLinkLosses` > 0 (with `rdna3hotplug=1`) | the sink dropped the link (monitor input switch, power save, a cable bump); the status DPCD 0x202..0x204 is read every 2 s in place of the HPD RX interrupt and the stream trained again (`dp_handle_link_loss`) | pass if the picture came back; `(-5)` three times in a row stops the tries until a hotplug: send the `dp: link training` lines |
| `wake: N DP display(s) blanked before the restore` | `dcn32_init_hw`'s `link_blank_all_dp_displays`: a DP sink still awake after the sleep goes to D3 before the restore trains it again | expected after a power loss with a DP monitor |
| `dp: link training HBR3x4 CR failed lane0 VS=3` then a pass at HBR2 | the fallback works: the cable / sink does not do HBR3 | pass; `RDNA3,Head<n>,DPLink` names the verified cap |
| every training `CR failed lane0 VS=0`, no VS change | the sink never saw the pattern: the PHY is not running at the link rate (TransmitterControl ENABLE through the DMCUB failed: `dp: TransmitterControl action 1 ... failed`), wrong transmitter, or `DP_DPHY_TRAINING_PATTERN_SEL` not reaching the PHY | the DMUB lines of the log; the head's `DIG_BE_CNTL` / `DIG_BE_EN_CNTL` in the look |
| CR done, `EQ failed` at every rate and lane count | pre-emphasis requests not applied: SETUP_VSEMPH's lane set (TransmitterControl v1.7 mode field) is ignored by the VBIOS table | the `lane status` / `adjust` debug lines; compare with the GOP's DPCD 0x103..0x106 |
| trained, `dp: DIG f -> UNIPHY b ... trained`, monitor black or "no signal" | stream side: M/N, MSA or the DP DTO (DPREFCLK) | look: `DP_DTOn_PHASE / MODULO` (phase = pixel clock in Hz, modulo = DPREFCLK in Hz), `DP_VID_M`, `DP_VID_STREAM_CNTL` status; `dp: DIG n: SYMCLK_FE_ON stays off` = the FE is not connected to the BE |
| `modeset: the DP link failed (-5): back to ...` and the old mode trains | the new mode's link setting does not train (see above) | record the rate / lanes; the old mode back is the expected recovery |
| modes `refused (10)` on a DP-to-HDMI / DVI / VGA converter | `link_validate_mode_timing`: the converter's TMDS clock (DPCD 0x081 x 2.5 MHz), its max bpc, or its DFP capability extension (DPCD 0x0a3..: max pixel rate, active size, RGB 8 bpc) is below the mode | `RDNA3,Connector<n>,DPCD` 0x005 / 0x080-0x083; expected for a mode above the converter |
| the monitor reacts differently since the stage 4 item K build (other OSD, FreeSync menu) | `retrieve_link_cap` now writes AMD's source OUI (00-00-1A), the device id and DCN 3.2's minimum h blank to DPCD 0x300 / 0x303 / 0x340 as amdgpu does, and the AUX reply timeout is 3.2 ms during the LTTPR probe | expected (Linux does the same); report anything that stopped working |
| a repeater (dock, active cable, retimer) between card and monitor: `dp: AUX n: LTTPR mode at the max link: non-transparent (VBIOS LTTPR support enabled)`, training fails | DC's non-transparent 8b/10b training hop by hop (`dp_perform_8b_10b_link_training`: each repeater with TPS4 at its own AUX_RD_INTERVAL, `repeater_training_done`, then the sink; the VBIOS's `DCE_INFO_CAPS_LTTPR_SUPPORT_ENABLE` decides it as in `dp_decide_8b_10b_lttpr_mode`) | the `dp: LTTPR r lane status ...` debug lines; compare with the Linux dump's link training log; without the repeater the sink trains as before |

### DSC and FEC (`dsc`)

`dsc` = `dplink` + `rdna3dsc=1`. On a native DP monitor that decodes DSC
(DPCD 0x060 bit 0) a mode the link cannot carry uncompressed is offered
and set with DSC; FEC runs on a FEC capable monitor whatever the mode.
Lines: `dp: AUX n: FEC .., DSC .. (rev .., slices .., ...)` (the caps),
`modeset: head n: WxH needs DSC: a x b slices, c bpp`, `dsc: DSC n: ...`
(the engine's config), `dp: DSC n on OPP .. -> OTG .. -> DIG ..`,
`dp: AUX n: SST_DSC: DSC enable on the RX`, `dp: UNIPHY n: FEC enabled
(active 1)`; ioreg `RDNA3,Head<n>,DSC` (slices, bpp, FEC state: 2 =
enabled).

| Symptom | Likely cause | Action |
|---|---|---|
| no mode needs DSC (`rdna3_modetest list` has the same modes as `dplink`) | the monitor's modes all fit the trained link uncompressed, or its DPCD has no DSC (`DSC no` in the `dp: AUX n: FEC ..` line, or no such line) | expected with such a monitor: test with a mode above the link (4K 120+ Hz on HBR3, or a cable / hub that limits the link) |
| `FEC enabled (active 0)` | the PHY did not start FEC after the training (`DP_DPHY_CNTL` FEC_ACTIVE_STATUS) | look: `DP_DPHY_CNTL`, DPCD 0x120; the training lines before it |
| DSC mode set, `trained`, monitor black or "no signal" | the monitor did not get or accept the PPS: GSP11 off, the PPS bytes, DSC not on in the RX (DPCD 0x160) | look: `DP_SEC_CNTL` / `DP_SEC_CNTL2` / `DP_GSP11_CNTL`, `DP_DSC_CNTL`, `OPTC_DATA_FORMAT_CONTROL` DSC mode, the `dsc: DSC n:` line (slice size, bpp) |
| DSC mode set, picture garbled or shifted | rate control / slice config the encoder and the monitor disagree on, or the DSC engine not fed by the head's OPP | the `dsc:` line and the look's `DSCC_PPS_CONFIG*`, `DSCRM_DSC_FORWARD_CONFIG` (OPP source) |
| `dsc: DSC n power up: PGFSM status ..` | DOMAIN16+n did not ungate | look: `DOMAIN16..19_PG_STATUS`, `DC_IP_REQUEST_CNTL` |
| `modeset: ... underflows / does not run: back to` only at DSC modes | the DLG rescale of the GOP's values at the compressed mode | as for `modeset2`: the look's DLG line; full DML for that mode |

### Timing generator read-back (`otg`)

`otg` = `9c` + `rdna3otg=1`. At display start `dcn32_otg_check`
(hw/dcn32_otg.c) runs DC's optc functions on every head the GOP lit and
publishes `RDNA3,Head<n>,OTGCheck`. Pass: `0: ...`, the active size equal
to the head's, `counter moving`, `vblank 1 vactive 1`, `vline0 ... latched`
with the line read within a few lines of the one aimed at, `underflow 0`,
`ODM 1`. Then mark `otg` passed: the optc functions the check reads count
as working (tools/progress.py ITEM_STEPS).

| Symptom | Likely cause | Action |
|---|---|---|
| no `OTGCheck` property | the boot-arg is missing, or no head was lit at display start (`RDNA3,DisplayHeads 0`) | the boot-args line of the report |
| `-19` (RDNA3_ENODEV) / `0x0` size | the OTG reads as off (OTG_MASTER_EN 0) | the look's `OTG_CONTROL`; the head list in `20-log-rdna3.txt` |
| active size differs from the head's | OTG_H/V_BLANK_START_END read differently by `optc1_get_otg_active_size` than by `read_timing` (dcn32.c) | both register values from the look; fix whichever subtraction is off |
| `counter STOPPED` | two OTG_STATUS_POSITION reads were equal: the OTG is not counting (or the read is cached) | `RDNA3,Head<n>,...` frame counter from `rdna3_drminfo --scanout`; the OTG clock (`OTG_CLOCK_CONTROL`) |
| `vblank 0` or `vactive 0` | OTG_STATUS never showed the state within 100 ms | the look's `OTG_STATUS`; the V_BLANK bits' layout (OTG_V_BLANK / OTG_V_ACTIVE_DISP) |
| `vline0 ... NOT latched` | OTG_VERTICAL_INTERRUPT0_STATUS did not set within 60 ms with the line inside the range | the line aimed at vs `OTG_V_TOTAL`; the look's `OTG_VERTICAL_INTERRUPT0_CONTROL` (OUTPUT_POLARITY, INT_TYPE) |
| `underflow 1` | OPTC_UNDERFLOW_OCCURRED_STATUS is sticky: an underflow at any time since boot | compare with the HUBP underflow of `rdna3_drminfo --scanout`; not a failure of this step |

### amdgpu's DMCUB reset (`dmubreset`)

`dmubreset` = `9c` + `rdna3dmubreset=1`: the DMCUB is stopped and reset by
the exact `dmub_dcn32_reset` (hw/dmub32.c) before its windows are set up.
ioreg `RDNA3,DMUBAmdReset`: bit 0 = STOP_FW acked (GPINT status cleared by
the firmware), bit 1 = the firmware answered 0xDEADDEAD in SCRATCH7. The
log has `DMUB diag (after the amdgpu reset)`: SCRATCH0-16, fault
addresses, inbox / outbox pointers, and up to 16 `DMUB trace:` entries.
Pass: `RDNA3,DMUB = running, mailbox up`, the clone and the extended
desktop as in 9c. Then mark `dmubreset` passed.

| Symptom | Likely cause | Action |
|---|---|---|
| `RDNA3,DMUBAmdReset = 0` or `2`, DMUB running anyway | the firmware acks later than 30 register reads (amdgpu forces the reset then) | harmless if the boot and the clone pass; note the value: the verified path waits up to 1 s |
| `DMUB: auto-load timed out` after the amdgpu reset | DMUIF_SOFT_RESET left set, or the forced reset caught the firmware mid PHY sequence | the `DMUB diag` lines (soft-reset 1? fault addresses?), `MMHUBBUB_SOFT_RESET` in the look; step 5b's path (no boot-arg) still works |
| DMUB running, second monitor dark | the clone's VBIOS commands after the reset: as step 9b's triage | `clone:` lines, `RDNA3,MirrorResult` |

### DC's HDMI stream (`hdmi`, `hdmi20`)

`hdmi` = `modeset` + `rdna3hdmi=1`, `hdmi20` = `modeset2` + `rdna3hdmi=3`.
Every TMDS mode set then also runs the HDMI half of DC's stream setup
(hw/dcn32_hdmi.c, called from `program()` in hw/dcn32_modeset.c). The log
has, per mode list, `display: head n HDMI: sink HDMI|DVI, SCDC s, LTE
scramble l, max TMDS M MHz, HDMI 6G g, a audio modes` (the EDID's HDMI
VSDB / HF-VSDB, the VBIOS encoder cap), and per switch `modeset: head n:
HDMI_CONTROL 0x..., SCDC TMDS_CONFIG ...`; ioreg `RDNA3,Head<n>,HDMI`
keeps the last one. Pass for `hdmi`: the switches of `rdna3_modetest
cycle` pass as in `modeset` with the picture right (no over/underscan, no
colour cast, no "no signal" after the switch back). Pass for `hdmi20`:
additionally a mode above 340 MHz (4K60) was offered, set, and
`RDNA3,Head<n>,HDMI` shows HDMI_CONTROL bits 1 and 2 (scrambler, 1/4
clock channel) with `SCDC TMDS_CONFIG 0x03 scrambler 0x01`. Then mark the
step passed (the ITEM_STEPS rules map the ported enc / SCDC / validation
functions to these steps).

| Symptom | Likely cause | Action |
|---|---|---|
| picture overscanned / underscanned or colours washed out after a switch | the AVI infoframe (`set_avi_info_frame`: VIC, aspect, S = underscan, Q default) read differently by the TV | the mode's timing against the CTA VIC table (`edid_match_vic`); reproduce the packet with `dcn32_hdmi_build_info_frames` in tests/sim_display_dc.inc and compare byte by byte with CTA-861 |
| `SCDC ... (-n)` with n = NACK on an HDMI 1.4 monitor | no SCDC at I2C 0x54: expected, DC ignores the error | nothing; on an HDMI 2.0 sink check the DDC line (`hc->ddc`, the VBIOS path) |
| `SCDC ... (-n)` ACQUIRE | the DDC engine owned by the DMCUB/HW | note; the SCDC write is skipped, which only matters above 340 MHz |
| hdmi20: 4K60 not in `rdna3_modetest list` | `HDMI 6G 0` (no ATOM_ENCODER_CAP_RECORD_HDMI6Gbps for the connector), the EDID's max TMDS below 594 MHz, or DISPCLK limits (`RD_MODESET_CLOCKS` in `display: head n mode ... refused (4)`) | the refusal code in the mode list log: 5 = TMDS limit, 4 = clocks |
| hdmi20: "no signal" at 4K60, old mode back | scrambling not matched by the sink (`scrambler 0x00`), or the PHY PLL at 594 MHz | SCDC values in the log; `HDMICHARCLK` / PHY registers in the look; retry with a certified cable |
| audio device appears but silent | without `rdna3audio=1` the azalia endpoint is never enabled (only the audio info frame line is set) | run step `audio` |

### Display audio (`audio`)

`audio` = `hdmi` + `rdna3dplink=1 rdna3audio=1`. The first mode list logs
`display: head n audio: "NAME", m modes, speakers 0x.., latency v/a ms`
(the EDID's SADs and speaker allocation, `dcn32_audio_info_from_edid`), the
first switch `audio: n azalia endpoints, straps 0x.. (HDMI audio native)`
(`dcn32_audio_pool_init`), every switch `display: head n audio: endpoint e,
HDMI|DP (DIO)|DP (HPO), on|off, m modes, speakers 0x.., DTO source d, c
channels at P kHz pixel clock, hot plug 0x.., PME p`; ioreg
`RDNA3,Head<n>,Audio` keeps the last one. Pass: `on`, `m modes` > 0, the
picture switches as in `hdmi`; with an HDA driver attached to the card's
audio function, System Settings > Sound > Output lists the monitor by its
EDID name and a test sound plays. Without such a driver the card side
alone passes the step (`on` and `hot plug 0x8.......`: bit 31 =
AUDIO_ENABLED of the endpoint, set by `dce_aud_az_enable`); then mark
`audio` passed (the ITEM_STEPS rules
map the enc / AFMT / APG / audio DTO functions to it).

| Symptom | Likely cause | Action |
|---|---|---|
| no `display: head n audio:` line at the switch | `rdna3audio` not set, a DVI sink (no HDMI VSDB), no audio data block in the EDID (`0 modes`), or `HDMI audio off` in the straps line | the mode list log's `audio modes` count; the straps value (`DC_PINSTRAPS_AUDIO` bit 1 = HDMI audio) |
| `no free azalia endpoint` | fewer endpoints than streams (`n azalia endpoints`, `PORT_CONNECTIVITY` of the pins) | the count in the log; compare with the Linux dump's `/proc/asound/cards` |
| `on`, macOS lists no output | no HDA driver for the card's audio function, or it sees no presence / ELD | `ioreg -l | grep -i hda`; the hot plug value (bit 31 AUDIO_ENABLED, bit 0 CLOCK_GATING_DISABLE); compare the ELD with Linux `cat /proc/asound/card*/eld*` |
| listed, no sound | the audio DTO / ACR (`DTO source`, pixel clock) does not match the stream, or the stream muted | the look's `DCCG_AUDIO_DTO0_PHASE/MODULE` (HDMI: module = pixel clock in 100 Hz, phase 24 MHz), `AFMT_AUDIO_PACKET_CONTROL`, `HDMI_ACR_PACKET_CONTROL`; reproduce with `test_dc_audio_streams` |
| DP: `DP (HPO)`, no sound | 128b/132b audio through APG with HBR audio off (`config.disable_hbr_audio_dp2`) | `APGn_APG_CONTROL`, `HPO_DP_STREAM_ENCn_DP_SYM32_ENC_SDP_AUDIO_CONTROL0` in the look |
| `RDNA3,Head<n>,DPSinkPresent` = `0 (... clock pin 1)` on a DP connector | the DP++ adaptor or cable in front of an HDMI / DVI monitor, as DC's `dp_is_sink_present` (DDC clock pin as GPIO input) tells DP from TMDS at detection | information only (the head runs TMDS; HDMI audio through a passive adaptor follows the straps' `hdmi_audio_on_dongle`) |
| picture lost at the switch with `rdna3audio=3` | the PMFW's BACO audio PME message (`DALSMC_MSG_BacoAudioD3PME`) | run with `rdna3audio=1`, report the PME value |

### Output pixel formats (`fmt`)

`fmt` = `hdmi` + `rdna3fmt=1` (`=3`: max 8 bpc, `=5`: YCbCr 4:2:2 forced).
Every HDMI switch logs and sets `RDNA3,Head<n>,Format` = `ENC N bpc,
colour space c, sink bpc b formats 0x..` (amdgpu_dm's choice for the
mode: `dcn32_output_format`). Pass: the switches of `rdna3_modetest
cycle` as in `hdmi` with the right colours (no cast, no extra banding)
and, where the sink offers it, `YCbCr 4:4:4` and more than 8 bpc in the
property (the monitor's info menu agrees). Then mark `fmt` passed.

| Symptom | Likely cause | Action |
|---|---|---|
| pink / green picture after a switch | the output CSC (RGB -> YCbCr matrix, `dcn32_program_output_csc`) or the AVI infoframe's colour fields do not match the encoding | the look's `MPCC_OGAM` / OCSC registers and `FMT_CONTROL.FMT_PIXEL_ENCODING`; reproduce with `test_dc_fmt_modeset` |
| "no signal" at 10 / 12 bpc | the deep colour TMDS clock (pixel clock x 1.25 / 1.5) or `PHYPLLx_PIXCLK_RESYNC_CNTL` | the log's pixel clock line; retry with `rdna3fmt=3` (8 bpc) to split encoding from depth |
| banding at 8 bpc | dither off (`FMT_BIT_DEPTH_CONTROL` should have spatial dither on) | the look's `FMT_BIT_DEPTH_CONTROL` |
| `RGB 8 bpc` although the TV takes YCbCr | the EDID's CTA byte 3 / HDMI VSDB deep colour bits (`color_formats`, `bpc` in the property) | the EDID in the report; `edid_parse` |

### Cursor through MALL (`cursormall`)

`cursormall` = `9c` + `rdna3cursormall=1`. Pass: the pointer is drawn and
moves on both monitors as in 9c, and the look has
`HUBPn_DCHUBP_MALL_CONFIG` with bit 2 (USE_MALL_FOR_CURSOR) on the lit
pipes. Then mark `cursormall` passed.

| Symptom | Likely cause | Action |
|---|---|---|
| pointer garbled, flickering or missing only with the boot-arg | the MALL (Infinity Cache) does not serve display reads in this configuration (no MALL allocation from the PMFW / DCHUBBUB) | the look's `DCHUBBUB_ARB_MALL_CNTL`; keep the boot-arg off, report |
| bit 2 not set | the framebuffer never set an image (software cursor: `rdna3cursor=0` or no VRAM) | fb log `hardware cursor: ...` |

### DC's sequence extras (`dcseq`)


`dcseq` = `modeset2` + `rdna3dcseq=1`. Pass: the switches of
`rdna3_modetest cycle` as in `modeset2` (pictures, no underflow, back to
native) with `modeset: head n: DIO FIFO resync (DISPCLK divider 0x..)`
after each. Then mark `dcseq` passed.

| Symptom | Likely cause | Action |
|---|---|---|
| HDMI head: `SYMCLK on with TX off (... kHz) failed` | the VBIOS SetPixelClock refused the running clock with the transmitter off | the DMUB lines; the switch still goes on (DC ignores it too); report the value |
| picture shifts / flickers once right after a switch, then settles | the DIO FIFO resync with a divider the DENTIST was still ramping to | `DENTIST_DISPCLK_CNTL` in the look (WDIVIDER = RDIVIDER?); compare without the boot-arg |
| `modeset: ... does not run` only with the boot-arg | the second TransmitterControl DISABLE after the OTG stopped left the PHY off for the new timing | the DMUB sequence in the log (DISABLE, PIXCLK, DISABLE, PIXCLK, ENCODER, ENABLE expected) |

### DP MST (`mst`)




`mst` = `dplink` + `rdna3mst=1`; needs an MST hub (or a daisy-chain
monitor) on the card's native DP output with at least one monitor behind
it. At the first mode switch on that head the driver turns MST on in the
hub, reads the topology and every monitor's EDID over sideband messages
(`mst:` lines, ioreg `RDNA3,Head<n>,MSTPorts` = `port:peer type:full
PBN:name` per output port), and sends the head's stream to one monitor
(`rdna3mstport=N` picks port N; default the first). After that switch the
mode list is that monitor's. ioreg `RDNA3,Head<n>,MST` = port, VCPI, PBN,
time slots, `X.Y`, ACT handled.

| Symptom | Likely cause | Action |
|---|---|---|
| no `mst:` lines | the hub's DPCD has no MST cap (`MST no` in the `dp: AUX n: FEC ..` line, or no such line), the boot-arg is missing, or no mode switch ran | `RDNA3,Connector<n>,DPCD` 0x021; run `rdna3_modetest` on that head |
| `mst: timed out msg send` / `NAK reply` / `link address NAK` | sideband: the hub did not answer DOWN_REQ, or the request was malformed (the hub NAKs with a reason) | the serial `mst:` lines in order; `takeover failed (r, n messages, n NAKs, n timeouts, n bad replies)` then `back to DP SST`: the SST picture must come back |
| ports listed, `EDID 0 bytes` | REMOTE_I2C_READ failed for that port | the port's `pdt` (3 = SST sink; 4 = legacy converter: not handled) |
| `mst: failed to get ACT after 3000 ms` | the hub did not apply the payload table: SAT rows vs the hub's table, or MST_EN / the link not up | `dp: MST allocate: ... VCP v, s slots`, the look's `DP_MSE_SAT0`, DPCD 0x1c0..0x1c2 / 0x2c0 |
| ACT handled, `ALLOCATE_PAYLOAD for port n failed` | the hub refused the PBN on its downstream link | `mst: enum path resources port n: full PBN ..`: the mode needs more than that port carries; pick a smaller mode |
| everything passes, the monitor behind the hub stays black | the VC rate or the stream inside the MST link | `dp: MST Update Payload: ... X: .. Y: ..` (X.Y = PBN / PBN per slot), the look's `DP_MSE_RATE_CNTL`, `DIG_BE_CNTL` DIG_MODE 5 |
| `refused as MST stream (8): back to SST` | the requested mode does not fit the chosen port's PBN | expected for big modes; the SST picture comes back and the mode is set as SST |

### DP 2.1 UHBR (`uhbr`)

`uhbr` = `dplink` + `modeset2`'s clocks + `rdna3uhbr=1`; needs a DP 2.1
monitor on a DP80 (UHBR20) or DP54 (UHBR13.5) cable straight into the
card, and a mode above HBR3 x4 (25.9 Gbps) in its EDID. Lines:
`dp: AUX n: 128b/132b: rates rr (UHBRxx), cable id cc, LTTPR rates ll, UHBR
on` (the caps: 0x2215 bits 0/1/2 = UHBR10/20/13.5), `dp: AUX n: ... max
link UHBRxx x4`, the training `dp: 128b_132b chose LTTPR_MODE ..`,
`dp: 128b/132b channel EQ done`, `dp: 128b/132b CDS done`, `dp: link
training UHBRxx x4 pass` or the failures `LT_FAILED received` / `max loop
count reached` / `channel EQ done timeout` / `CDS done timeout` (each
followed by the max-bandwidth fallback), `DAL SMU: DTBCLK n levels up to
.. kHz: reference .. kHz`, `dp: HPO 0 -> UNIPHY b: WxH .. kHz on UHBRxx x4:
trained`, `dp: 128b/132b SST payload: n time slots, VC rate X.Y x.y,
hblank min symbol width w, ACT handled`; ioreg `RDNA3,Head<n>,UHBR`.
Pass: the mode above HBR3 runs with the picture right and goes back to
the old mode cleanly. Then mark `uhbr` passed.

| Symptom | Likely cause | Action |
|---|---|---|
| max link stays `HBR3` | no UHBR in the VBIOS encoder caps (`RDNA3,Connector<n>` encoder caps without 0x20/0x40/0x80), the sink without 128b/132b (DPCD 0x006 bit 1, 0x2215), the cable id (0x2217) naming a slower cable, or a repeater (LTTPR rates) | the caps line; DPCD 0x006 / 0x2215 / 0x2217 / 0xf0000.. in the report; try a certified DP80 cable |
| every UHBR rate `LT_FAILED received` / `max loop count reached`, the cap falls back to HBR3 | the PHY did not lock at the UHBR rate: TransmitterControl for the HPO link encoder (digmode 5, hpo_instance), the FFE preset, the cable | the DMUB `VBIOS 0x4 ...` lines (symclk 100000 / 135000 / 200000 x 10 kHz, hpo_instance 0), the look's `DP_DPHY_SYM32_CONTROL` (DPHY_ENABLE, NUM_LANES, MODE) and `SYMCLK32_LE_CNTL`; DPCD 0x202..0x207 at the failure |
| `channel EQ done timeout` / `CDS done timeout` | EQ / CDS interlane align never set (DPCD 0x204 bits 2 / 3) | the lane status lines before it; the aux read interval (DPCD 0x2216) |
| `DTBCLK not set (..)`: stays at the old mode | the PMFW's DAL mailbox refused PPCLK_DTBCLK (12) | the `DAL SMU message 0x9(0xc....)` line; `RDNA3,DCNClocks` DTBCLK |
| trained, monitor black | the stream: HPO stream encoder not mapped / not enabled, the DTBCLK DTO, DPSTREAMCLK, the payload (no ACT) or the VC rate | the look's `DP_STREAM_MAPPER_CONTROL0`, `DP_SYM32_ENC_CONTROL` / `VID_STREAM_CONTROL`, `OTG_PIXEL_RATE_CNTL` (DTBCLK_DTO_ENABLE, PIPE_DTO_SRC_SEL 2), `DTBCLK_DTO0_PHASE/MODULO`, `DPSTREAMCLK_CNTL`, `SYMCLK32_SE_CNTL`, `DP_DPHY_SYM32_SAT_VC0` / `VC_RATE_CNTL0`, DPCD 0x1c0..0x1c2 / 0x2c0 |
| `ACT NOT handled` | the sink did not see the allocation change trigger (SAT_UPDATE) or lost the link | the look's `DP_DPHY_SYM32_STATUS` (SAT_UPDATE_PENDING), DPCD 0x2c0 |
| picture torn / shifted horizontally | the hblank minimum symbol width or the VC rate X.Y | `hblank min symbol width w` vs the timing; the X.Y in the log |
| MST hub on a UHBR link | MST over 128b/132b is not ported: `RD_MODESET_DP_LINK` | expected; run `mst` without `rdna3uhbr` |

### ODM combine (`odm`)

`odm` = `uhbr` + `rdna3dsc=1 rdna3odm=1`; needs an 8K monitor (or a mode
whose pixel rate needs more DISPCLK than one pipe gets) on native DP.
Lines: `modeset: head n: WxH on 8b/10b (DIO) | 128b/132b (HPO), ODM 2:1
(runs 1:1), ODM pipe acquired`, `odm: OTG o: pipe p / OPP o + pipe q /
OPP q / DSC q, DET a/b -> c/d`, `odm: pipe p + q on OTG o: slices w0 + w1
of WxH, refcyc_h_blank_end ..`, with DSC `dsc: DSC p: (W/2)xH .. 2 x ..
slices` for both engines and `dp: DSC p on OPP .. 2 ODM segment(s)`;
ioreg `RDNA3,Head<n>,ODM`. Pass: the 8K mode runs with the whole picture
(both halves, no seam), the cursor crosses the middle, and the switch
back to a one-pipe mode turns the second pipe off (`odm: pipe q off`).
Then mark `odm` passed.

| Symptom | Likely cause | Action |
|---|---|---|
| 8K refused (6, size) | `rdna3odm` missing, or ODM not chosen | the boot-args; `modeset: ... ODM` line |
| refused (11, ODM) | no free pipe (both heads plus a clone use them), the OTG not on its own OPP, no DET to split, or the DSC slices do not split over two segments | the `odm:` / `modeset:` lines; unplug the other monitors or turn `rdna3mirror` off |
| left half right, right half black | the ODM pipe: power, its HUBP blanked, MPC tree or OPP not into the OTG | the look's `OPTC_DATA_SOURCE_SELECT` (2 segments, SEG1 = q), `MPC_OUT_MUX[q]`, `MPCC_TOP_SEL[q]`, `OPP_PIPE_CONTROL[q]`, `DCHUBP_CNTL[q]` (BLANK_EN, VTG_SEL), `DOMAINq_PG_STATUS` |
| both halves show the left half / the halves swapped | the ODM pipe's viewport x (`HUBP_DCSURF_PRI_VIEWPORT_START` = W/2) | the look's viewport registers of both pipes |
| a seam or a stripe in the middle, or underflow | `OPTC_WIDTH_CONTROL` segment width, `OPTC_MEMORY_CONFIG`, the second pipe's DLG (`refcyc_h_blank_end`), DET split | the `odm: ... refcyc_h_blank_end a / b` line; `DCHUBBUB_DET*_CTRL` |
| with DSC: garbage in one half | the second DSC engine (slices per segment, its OPP) | the two `dsc:` lines (W/2 wide, slices / 2), `DSCRM_DSC_FORWARD_CONFIG` of both engines |
| the switch back leaves the second monitor / pipe dark | the ODM pipe was another head's | report: `dcn32_odm_acquire` should never take a head's pipe |

### Display power (`mall`)

`mall` = `modeset2` + `rdna3dpms=1 rdna3mall=1` (hw/dcn32_pstate.c).
Lines: `pstate: PMFW 0x.., DCFCLK a..b MHz (n), UCLK 96..1218 MHz (4), FCLK
.. (n), DTBCLK .., s states, c x 2-byte channels, latencies 400000 / ..`,
`pstate: watermark range table at MC 0x.. to the PMFW: 0 (A 0/0 B 1/0 C
2/1 D 3/2)`, `display power: init 0: ...`, then per update `pstate: state
s: UCLK switches (vactive, prefetch mode 0) min .. kHz, FCLK switches,
DCFCLK .. kHz deep sleep .., WM A urgent u p-state p fclk f sr x/y ns, C
p-state .. ns (dummy d), MALL .. B (w ways), compbuf ..`, one `pstate: pipe
p OTG o WxH .. kHz DET .. KiB: active margin m us, FCLK margin ..` per head
and `display power: <why>: state s, UCLK switching at .. MHz (vactive) ...`;
ioreg `RDNA3,DCNPowerInit`, `RDNA3,DCNPower`, `RDNA3,Head<n>,PState`. The
Linux reference with two 1080p60 heads: state 0, UCLK 96 MHz, FCLK 601
MHz, set A urgent 17.32 us, p-state 417.32 us, sr_enter 59.72 us. Read
the `PM:` lines of the trend (uclk, W) against `big2`'s, and
`rdna3_drminfo --scanout` for HUBP underflow. Pass: no flicker or black
line for some minutes of idle and use, no underflow, UCLK at its low level
at idle, `rdna3_modetest cycle` as in `modeset2`. Then mark `mall` passed.

| Symptom | Likely cause | Action |
|---|---|---|
| `the PMFW does not answer the DAL mailbox`, nothing else | GetSmuVersion over the DAL mailbox (0x1628A) unanswered | the step 5 lines; the feature stays off by itself |
| `watermark range table ...: -n` | SetDalDramAddrHigh / Low or TransferTableDram2Smu refused (`DAL SMU message 0x8(0x2): answer ..`) | the DAL lines; the decision still runs, the PMFW keeps its own ranges: report |
| `UCLK PINNED at max (unsupported ...)` with two 1080p / 1440p heads | the active margins negative: the DETs the GOP split leave too few lines | `RDNA3,Head<n>,PState` margins and DET; try `rdna3mall=2` (DC's DET split) |
| `p-state allow forced and the switch needs vblank: UCLK stays pinned` | the mirror's `rdna3pstate` force with a head that only switches in vblank | expected (the force lets the PMFW switch at any time); `PM:` uclk stays high |
| flicker / a black line now and then at idle, HUBP underflow | the UCLK switch outlasts a head's buffer: the set A p-state watermark (`p-state` in the `pstate: state` line, register `DCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A` = ns / 10) below the real latency, or the VBIOS latency (`latencies` line) wrong | compare the watermarks with the Linux reference; the look's `DCHUBBUB_ARB_*` and `DRAM_STATE_CNTL`; report with the `pstate:` lines |
| flicker only after a second without movement | the idle CAB (`CAB_FOR_SS` fit, n ways) or MALL_SEL 2 | the DMCUB lines; `DCHUBP_MALL_CONFIG` of the pipes in the look; report |
| black screen at display sleep / wake | the update at display sleep (`display power: display sleep`) | the lines around it; the step without `rdna3dpms` |
| `not updated: a head's refresh rate is unknown` / `a head is not described` | a head the decision does not see (no measured refresh) | the `display: head n refresh` lines; nothing was changed: UCLK stays as the GOP / PMFW left it |
| `not updated: the mirror's pipe is not a head` | `rdna3mirror=1` (clone without its own head) | expected; use `rdna3mirror=2` |
| `not updated (..): no voltage state supports the heads` | DML found no state (bandwidth / prefetch) | the `pstate:` lines; report with the modes |

### SubVP (`subvp`)

`subvp` = `mall` + `rdna3subvp=1` with one monitor (`rdna3mirror=0`), its
high-refresh mode set with `rdna3_modetest`. Lines: `subvp: phantom of
pipe p (OTG o, WxH) on pipe q: n lines (vtotal t, back porch b, VSTARTUP
v), MALL index 0`, `subvp: on, 1 phantom(s), 2 DMCUB lock commands: 0`,
`display power: mode set: state s, UCLK switching at .. MHz (vactive +
SubVP) ..., SubVP 1`; ioreg `RDNA3,Head<n>,PState` (`, SubVP main`). Pass:
the high-refresh mode runs without flicker or tearing for a few minutes
(move windows: every flip sends the new address to the firmware), UCLK at
a low level in the `PM:` lines, no HUBP underflow; the switch back to 60
Hz turns the phantom off (`subvp: phantom pipe q off`, `subvp: off (was
on)`). Then mark `subvp` passed.

| Symptom | Likely cause | Action |
|---|---|---|
| no `subvp:` line, `UCLK PINNED` | the decision found no SubVP configuration (refresh / size not in DC's lists, scaled mode, no free pipe, no lower UCLK) | the `pstate:` lines: `SubVP lines`, margins; expected for 60 Hz 1080p/1440p (they switch in active) |
| `subvp: DMCUB HW lock: -n` / `HANDLE_SUBVP_CMD: -n` | the DMCUB does not ack inbox0 or refused the command (firmware without SubVP, GECC on) | the DMUB lines and `RDNA3,DMUB*`; report the firmware version |
| `subvp: phantom bring-up failed (-n): SubVP off` | the phantom pipe's power gate or DET did not settle | the look's `DOMAINq_PG_STATUS`, `DCHUBBUB_DETq_CTRL` |
| flicker / tear at every UCLK switch, HUBP underflow on the main | the phantom's MALL region too short or the schedule wrong (phantom lines, VSTARTUP, prefetch lines in the command) | the `subvp: phantom` line; the look's phantom OTG `V_TOTAL` / `V_BLANK_START_END`, `DCHUBP_MALL_CONFIG` (1 on the phantom), `DCHUBP_VMPG_CONFIG` FORCE_ONE_ROW_FOR_FRAME on the main; report |
| the picture freezes on the old frame after a flip | the firmware programs an old address: `dmub_dcn32_save_surf_addr` | DMCUB_SCRATCH9/12/15 in the look; report |
| the switch back leaves a pipe powered / MALL_SEL 1 | `dcn32_disable_phantom_streams` | the look's phantom pipe registers; report |

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
`resume #N ...` (`RDNA3Bringup::resumeAfterPowerLoss`). With
`rdna3resume=1` the sleep also runs `amdgpu_device_suspend`
(`suspend #N: rings idle, then the IPs down in reverse order`,
`RDNA3,SystemSuspends`), so every wake runs the resume, also when the card
kept its power (`resume #N: after amdgpu_device_suspend`). Before the
suspend the VRAM clients use is copied to system memory
(`suspend: N VRAM range(s), M MiB saved`, `RDNA3,EvictedMiB`); after a
power loss it goes back before the display restore
(`resume: N VRAM range(s) back from system memory`, then `every evicted
VRAM range is back`: no VRAMLOST for the clients). With `rdna3hotplug=1`
the HPD looks pause for the sleep and come back with
`hotplug: wake: N HPD filter(s) programmed again (link_resume)`
(`RDNA3,HotplugResumes`); a `hotplug:` unplug of a monitor that stayed
connected right after a wake means the pins were looked at too early.
`display: the picture in VRAM was lost (...)` after a wake means the
eviction did not bring everything back (see the `suspend:` / `resume:`
VRAM lines); the framebuffers then log `the picture in VRAM was lost:
connect change` and WindowServer should redraw within a second - a desktop
that stays black or garbled after that line is a WindowServer question.

| Symptom | Likely cause | Action |
|---|---|---|
| monitor keeps its signal in display sleep (only black), `display: head N blanked` | `dcn32_head_link` refused: no snapshot, neither TMDS nor DP SST (`state: head N saved ... blank only`), a DP head without its link (`state: head N: a DP head without its link`: `rdna3dplink=1` missing or the sink gave no caps), DP MST / ODM, or no DMCUB (`rdna3dmub=1`) | the `saved` line names the DIG mode and whether a VBIOS path was found (`displayHeadPath`: transmitter = DIG back end); MST and ODM heads stay on the blank by design |
| a DP monitor dark after the wake, `state: head N restore: ... DP ...` then `dp: link training failed` or `restored (-5)` | the link did not train again after the power loss (`dcn32_dp_link_set_dpms_on` in the restore) | the `dp: link training ...` lines of the restore against those of the boot's mode set; `RDNA3,Head<n>,DPLink`; `restored (-22)` with `no link resources`: the sink now offers less than the mode needs |
| standby works, the monitor wakes without a picture | TransmitterControl ENABLE alone does not bring the stream back on the boot head | compare `state: head N link up (r)` with the clone's `clone: link up`; next: `enable_link_hdmi`'s `setup_stream_encoder` (STREAM_SETUP) before ENABLE in `dcn32_head_link`, modelled in the sim first |
| `SYSTEM SLEEP: the system did not sleep` | VM: guest S3 off, or an assertion | `pmset -g log` lines in the test output, `host-*.log` QEMU line with `ICH9-LPC.disable_s3=0`; `pmset -g assertions` in the VM |
| report ends at `system sleep`, `host-*.log` `still suspended after 90 s: system_wakeup` | the guest did not wake (RTC alarm lost, system_wakeup refused) | QEMU version / `info status` output in `host-*.log`; next round without `pmset sleepnow` (display part only) |
| `RDNA3,Wake ... the card kept its power`, `WakeResumeResult 1`, every head `restored (0), running`, pictures and CRC like before, DMA/GFX tests after the wake pass | pass | `sleep <date> vm` into `docs/status/hw-verified.txt` |
| `unload: ring X.Y stuck at fence N: forced` before `suspend #N` | a job still ran when the sleep came (amdgpu_fence_driver_hw_fini forces it too) | harmless if the resume passes; else the ring named |
| card kept its power, `WakeResumeResult 0` | the resume with a live sOS: `psp_resume` path (`PSP: sOS already alive` must appear) or `hwInit` after the IPs' suspend | serial from `resume #N` on: the first `failed:` `Stage2` value names the block |
| `RDNA3,Wake ... the card lost its power`, `WakeResumeResult 0` | `resumeAfterPowerLoss` stopped: `ResumeResult` 1 ASIC_Init, 2 FB location moved, 3 `hwInit` | serial `resume #1` and the `PSP` / `post` / `Stage2` lines after it; in the VM a wake brings a bus reset: `resetIfPspStuck` must log the mode-1 reset |
| heads `restored (-16)` after a power loss | DISPCLK / DPPCLK below the pixel rate and the raise through the DAL mailbox failed (`state: head N: ... raising them failed`); a raise that worked logs `state: head N: clocks raised for ...` | the `DAL SMU: hard min` lines before it; divider 127 (`DCN clocks: DISPCLK at divider 127`) needs the OTG pixel drop of `dcn32_update_clocks_update_dentist` |
| heads `restored (-19)` | no DMCUB after the resume: with `rdna3dmub=3` the re-init loads our own once the GOP's is gone (`DMUB: the GOP's firmware did not survive`, `RDNA3,DMUBReloaded`); that load failed | the `DMUB:` lines after `resume` (`RDNA3,DMUBResult`) |
| restored, `running`, the picture black or garbage | VRAM did not survive and the eviction missed the surface (`EvictFailedMiB` > 0, `some NOT`), or the restore copy failed (`EvictRestoreFailures`) | the `suspend:` / `resume:` eviction lines; a picture a minute later (WindowServer redraws); `sysctx 0` in the head state = the DCHUBBUB system aperture not restored |
| DMA/GFX tests after the wake fail, ring tests passed | GPUVM page tables lost with VRAM and not rebuilt: `VRAM lost: page tables rebuilt for N VM(s)` and `the clients' GPUVM ranges mapped again` must follow `resume #N` (`RDNA3,VMReplayFailures` 0) | a fresh process (new VM) passing = the old VMs' replay failed: the `VMReplayFailures` count and the client's mappings |

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

---

### Step `amdgpu` (default profile + `rdna3amdgpu=1`): amdgpu's own sequence

The boot-arg switches every place where the profile verified on the card
does something else than amdgpu to amdgpu's own code (stage 4 item J;
`rdna3_dev.amdgpu_exact`). Read the report like step `default` first;
`RDNA3,AmdgpuExact = 1` says the boot-arg was seen. Then, area by area:

| Symptom | Likely cause | Action |
|---|---|---|
| `RDNA3,PSPResult` nonzero, `PSP: load ... SMC` failed or the PMFW never answers (`SMU: not ready`) | the PMFW loaded after SETUP_TMR (`psp_load_fw` order without centralized C-state management) is refused | the `PSP:` lines around `SETUP_TMR` and `LOAD_IP_FW SMU`; compare with step `default` (PMFW before the TMR) |
| `PSP: load asd failed!`, `PSP: LOAD_TA hdcp/dtm ... status` nonzero, `load RL failed!` | the TA file (`psp_13_0_10_ta.bin`) missing from the bundle, or the PSP refuses a TA / the register list | the bring-up stops there (amdgpu does the same); the `PSP:` lines; boot without the boot-arg |
| `SMU: OverridePcieParameters failed`, `Failed to enable thermal alert`, `Failed to notify display change` | `smu_smc_hw_setup`'s order refused by the PMFW (every PCIe level after EnableAllSmuFeatures, THM / MP1 interrupt registers, `DALNotPresent`) | `RDNA3,PMResult`; the `SMU:` lines with the message and response; `RDNA3,PMPcieLevels` |
| `GPU SW CTF` / `GPU HW CTF` / `thermal emergency` in the log, the machine powers down | the THM alert fired: either a real over-temperature or `DIG_THERM_INTH` programmed wrong | `RDNA3,PMSmuInterrupts`, the hotspot in the report; if the card was cool, send the log (the IV's client / source) |
| `IH: no system memory rings, back to VRAM` | the system pages or their GART binding failed | `RDNA3,IHSystemMemory = 0`; the rest of the step still runs on the VRAM ring |
| `RDNA3,IHSystemMemory = 1` but fences time out, `RDNA3,IRQIVs` stays flat or IVs arrive late | IH writes into snooped system memory not seen, or the rptr doorbell not routed (`S2A_DOORBELL_ENTRY_1_CTRL`) and the ring fills up (`IH ring buffer overflow`) | `RDNA3,IRQIVs`, the overflow lines; compare with step `default` |
| VM faults (`RDNA3,VMFaults` > 0) in the DMA/GFX/Vulkan tests that step `default` does not show | the page tables with fragments / huge PDEs (`amdgpu_vm_ptes_update`), or a compute VMID (8-15) handed to graphics | `RDNA3,LastFaultAddr` / `LastFaultVMID`; a VMID >= 8 there is a driver bug |
| `RDNA3,IBTestGFX` / `IBTestCOMP` nonzero, `GFX: IB test failed` / `KCQ: IB test failed` | an IB in VMID 0 (`gfx_v11_0_ring_test_ib`) not fetched, or the 256-dword NOP padding of the rings (`gfx_v11_ring_insert_nop`) not accepted by the CP | the `GFX:` status dump after it; compare `RDNA3,ComputeRings` (should read `comp_1.0.0 comp_1.1.0 ... comp_1.3.1`) |
| `Illegal opcode / register access / instruction in command stream` in the log, `hang:` recovery right after | a CP fault interrupt (`RDNA3,CPFaultIrqs = 1`, gfx_v11_0_late_init) named the ring and recovery started at once (drm_sched_fault) | the job the client ran; if the line repeats without a client running, send the log (ring_id) |
| compute jobs slower or GFX starved while a compute job runs | KCQ 0 now runs at amdgpu's high priority (pipe HIGH, queue 15, tunneling) | expected with this step; note it in the report |
| `GMC: failed to write reg ... wait reg ...`, a VM map or unmap stalls for seconds | the TLB invalidation through the KIQ (`gmc_v11_0_flush_gpu_tlb` -> `amdgpu_gmc_fw_reg_write_reg_wait`) did not finish: the KIQ (MES pipe 1) does not run the WAIT_REG_MEM write-and-wait | the KIQ lines; compare with step `default` (MMIO invalidation) |
| VM faults whose PASID is 0 or another process's | the IH LUT (`IH_VMID_n_LUT`) not written by the job's PASID mapping (`gmc_v11_0_emit_pasid_mapping`) | `RDNA3,LastFaultVMID`; send the log |
| `GPU reset ... FAILED` where step `default` recovers | the suspend amdgpu does before a full reset (GFX hw_fini, `psp_suspend`, IH and GMC off) left something the bring-up does not expect | the `hang:` lines; boot without the boot-arg |
| with `rdna3mes=1` too: `MES: failed to reg_write_reg_wait`, `failed to map legacy queue`, `ring reset failed` | the MES firmware refuses a MISC / legacy-queue frame (TLB invalidations go through the MES once it runs; a hung GFX or compute ring is reset through it first) | `RDNA3,MESSchedVersion`, the `MES:` and `hang:` lines; boot without `rdna3mes=1` to separate the two |
| with `rdna3mes=1` too: `RDNA3,KernelQueuesByMES = 1` (MES firmware 0x47 on: the scheduler started with the KIQ, `KCQ: ... mapped by the MES scheduler`, `GFX: ring 0 mapped by the MES scheduler`) | amdgpu's `enable_legacy_queue_map` path (`mes_v11_0_kiq_hw_init`, `amdgpu_gfx_mes_enable_kcq`, `amdgpu_gfx_enable_kgq`) | expected; `RDNA3,KernelQueuesByMESFallback = 1` with `the MES scheduler did not map it` means the scheduler refused the map and the KIQ mapped the queue as in step `default`: send the `MES:` lines |
| `RDNA3,IBTestSDMA` nonzero, `SDMA: IB test failed`, or DMA jobs (libdrm / Vulkan transfer) time out where step `default` runs them | the DMA ring as amdgpu drives it: 16-dword padding with burst NOPs (`RDNA3,SDMABurstNop`), COND_EXE on the ring's write-back slot, or the VM flush on the ring's own invalidation engine (`RDNA3,SDMAVmInvEng`, should read 12) not acked | the `SDMA` status dump after the hang; `RDNA3,SDMAResets`; send the log |
| `firmware ...: failed to validate`, `RDNA3,FirmwareNameMismatch` nonzero, or `RDNA3,XclkKHz` / `VRAMType` / `VRAMWidth` / `MALLSizeMB` not 100000 / 9 / 256 / 64 | a firmware file whose header size is not the file size (amdgpu_ucode_validate), the bundle's file names not the ones the discovered IP versions give, or the VBIOS / discovery tables read differently from Linux (card dump: GDDR6, 16 channels, 100 MHz reference, MALL 16 MiB x 4 UMC) | reinstall the firmware; send the log and `ioreg` lines |
| `reset on init: the PSP sOS already runs` and no picture afterwards, `RDNA3,ResetOnInit` nonzero, or `RDNA3,ASPMProgrammed = 1` with link errors / a hang in the first seconds | a kext reload found the sOS running and did amdgpu's mode-1 reset on init (expected only after a reload, never after a cold boot), or ASPM / LTR programmed because the link reports ASPM enabled (Linux on the card: "ASPM Disabled") | send the log; boot without the boot-arg to compare |
| black screen or no picture after the driver unloads | `gmc_v11_0_hw_fini` / `gfxhub_v3_0_3_gart_disable` at unload switch the hubs off as amdgpu does | expected only at unload; reboot |
| everything as in step `default` | pass | `amdgpu <date> vm` into `docs/status/hw-verified.txt`, regenerate |

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

## Plugin frame trace

Every process that loads the Metal plugin writes a binary trace of what it did
with IOSurfaces, command buffers and Vulkan: `/var/tmp/rdna3-nvmtl/<process>-<pid>.trace`
(format: `metal/plugin/nvmtl_trace.h`). `tools/macos/collect_plugin_trace.sh`
puts them in the report as `31-plugin-trace/` and saves WindowServer's NVMTL
log whole as `32-log-nvmtl-windowserver.txt` (once `collect.sh` calls it; until
then run it by hand on the card machine, as root: `sudo sh tools/macos/collect_plugin_trace.sh <dir>`;
the files it adds are handed back to the invoking user). The text log is rate limited;
the trace is the record. Decode on the host:

    python3 tools/plugin_trace.py rdna3-report-*.tar.gz          # all processes, WindowServer first
    python3 tools/plugin_trace.py REPORT --proc WindowServer --frames 40
    python3 tools/plugin_trace.py REPORT --surface 1234 --all    # one IOSurface's whole timeline + failures
    python3 tools/plugin_trace.py REPORT --rates                 # counts per second of process life

Questions it answers (the "findings:" block names each, per IOSurface):

* *Which surface did WindowServer render into, frame by frame?* The frame view
  lists, per command buffer, the pass (`surf`, load/store action, clear colour)
  and what it turned out to be: DRAWN, **CLEAR-ONLY** (cleared and nothing
  drawn; `clear-only` finding when the surface is display sized and the colour
  is transparent black), or untouched.
* *Was it published?* `never-published`: a command buffer wrote the surface and
  no dirty mark and no copy-out ran for it (FAIL). The `per IOSurface` table
  counts `o:dirt` (dirty mark, no copy), `o:copy` (copy-out), `o:refu` (copy-out
  because the kernel refused the dirty mark), `o:fail`, `EMPTY`.
* *Did the kernel refuse?* `dirty-refused` lists the errno (macOS numbers; 19 =
  ENODEV: no VRAM behind the surface); `kreq-import` the refused surface imports.
* *Was the published picture empty?* (FAIL only when the same command buffer's
  pass DREW into the surface and the copy-out is still empty; an empty picture
  after a clear-only pass is INFO, one whose pass is not in the trace WARN.) `empty-publish`: the band hash of the data
  written into the IOSurface (the same 8 bands x 48 columns FNV hash as the
  kernel's `rd3AccelBandHash`, so a hash can be matched with the kext's `swap
  #N` band lines) has every sampled pixel zero. A back buffer that is always
  CLEAR-ONLY + EMPTY while its twin is DRAWN + dirty is the flicker.
* *Surface-in skipped while the VRAM was stale?* `skip-stale`: a "shared VRAM
  current" skip right after a failed, empty or refused-then-copied publish of
  that surface; `skip-foreign` (INFO): a skip that relies on a VRAM tag written
  by another process. `NVMTL_TRACE_HASH=2` additionally hashes the unlocked CPU
  copy of skipped / dirty surfaces (not a default: it reads memory the plugin
  did not lock).
* *Did Vulkan fail?* `vk-submit` (vkQueueSubmit not VK_SUCCESS, with the
  function that submitted), `vk-wait` (fence timeout / error, wait time),
  `vk-disable` ("disabling backend" decisions), `alloc-fail` (vkCreate*/
  vkAllocateMemory/newBufferWithBytesNoCopy), `cb-error`.

Reading a block: `+S.mmm` is seconds since the process opened its trace, next
to it the wall clock (UTC; `--utc-offset H` for the card's zone; the process's
`NVMTLDevice +load` log line gives the same instant). `events: N written, M
kept` says how much the rings still hold (main ring 16384 records, vk ring 8192:
about the last 10 to 30 s of WindowServer); `lifetime totals` come from the
per-second summary (8192 s) and never wrap, so a rate is trustworthy even when
the events are gone. SURF_OUT `rc` is the dirty request's errno (negative: -2
dirty path off, -3 no Vulkan device); seeds are `IOSurfaceGetSeed` before ->
after the lock. A frame with `NOT FINISHED` was still in flight when the
report was taken. If `31-plugin-trace/info.txt` says 0 traces: the plugin is
older than the trace, or `NVMTL_TRACE=0` is set (also readable from the
`trace:` line in the plugin log). Switch off: `NVMTL_TRACE=0` in
`/Library/GPUBundles/rdna3mtl/rdna3mtl.conf`. The directory `/var/tmp/rdna3-nvmtl`
is created root:admin, mode 1777, by `tools/macos/fix_tracedir.sh` (run by the
package's postinstall, by `build_metal.sh` and the VM's `autoreport.sh --sync-metal`: a
symlink, a file or a foreign directory planted there is removed first, never
followed; an existing directory of root's or WindowServer's is kept with its
live traces and only chown'ed, so WindowServer cannot make it `_windowserver:wheel`
and hide the traces from the admin);
the files are mode 0640, so the admin group (and root) can read them and other
users cannot. If an app created the directory first, root, WindowServer (uid 88)
and that app's user are accepted as owner (a root process accepts only root's),
anyone else's is refused (`trace: cannot use the trace directory ...` in the
plugin log); without the postinstall's group the files carry the creator's
group, so collect as root (`sudo`) then; a file the collector cannot read is
listed as `REJECT ... unreadable: run with sudo`. The collector takes only regular files
with one link, the NVMTLTR1 magic, owned by root/uid 88 or by the uid their own
header names (never a link, never a file named WindowServer* of another user),
and says what it refused in `31-plugin-trace/info.txt` (`REJECT ... reason`).
The decoder warns (`ws-uid`) about a WindowServer trace not written by uid 88.
Privacy: a trace holds timestamps, IOSurface ids and unsalted hashes of sampled
pixels (needed to match the kext's band hash), i.e. a coarse record of screen
activity: keep reports private. `NVMTL_TRACE_*` knobs are ignored by set-uid
processes; there is no /private/tmp fallback any more.
Residual risks: the trace is an mmap'd file, so a process that truncates it
(as root, by hand) kills the traced process with SIGBUS (deleting it is safe);
`NVMTL_TRACE_HASH=2` reads IOSurface memory the plugin did not lock, so only
use it on a run where that is the question. Triage: `plugin_trace.triage_lines(report)`.

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
| "first setGammaTable" logged, no "gamma ... -> 0x0" | the core refused (`rdna3gamma=0` in the boot-args? DCN head?) | core log `display: gamma` |
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

### Card run 31 (steps userq, pmctl, vcn, vcn2; build 90734eb)

These steps boot without `rdna3fb`, so macOS drives only the head the GOP
lit, through its generic framebuffer: one monitor, named "Unknown" in
Settings. That is expected for them, not a display regression.

- **userq:** `ADD_QUEUE type 1 doorbell 0xc00 PASID 6: 0`, the compute user
  queue never fetched (`rptr 0 of 14`, `dst` 0), no VM fault. Diagnostic
  build: the ADD_QUEUE log line carries the PD, MQD, wptr (VA and GART MC),
  process and gang context addresses, and `userq diag:` lines dump the
  doorbell ranges and every active compute HQD of ME 1 before the
  REMOVE_QUEUE (no active HQD = the MES never mapped the queue; an active
  one with WPTR 0 = the wptr poll address; RPTR = WPTR > 0 = it ran but the
  fence path failed).
- **pmctl:** `rdna3_pmctl` was missing from the CI artifact and from the VM's
  `bin/`, so the step tested nothing. Both fixed; run it again.
- **vcn (`rdna3vcn=1`, static PG):** `VCN[0] is not responding, giving up!!!
  (UVD_STATUS 0x00000004)`: the VCPU does not boot on the static path. Linux
  runs DPG on this card; `vcn` stays as a comparison only.
- **vcn2 (`rdna3vcn=2`, DPG):** the VCPUs boot, both unified ring tests and
  the JPEG ring and IB tests pass, and `--vcn-test`'s JPEG job from a client
  VM signals. Both encode IB tests time out (`fence 2 timeout (last 0)`), so
  no VCN_ENC ring is offered (`--vcn-test` reported PASSED on JPEG alone; it
  now fails without a VCN_ENC ring). Diagnostic build: a fence-only
  submission before the IB test (`VCNn: fence-only test passed/FAILED`) tells
  a fence the VCPU cannot write apart from an IB it does not finish, and a
  failed IB test logs rptr / wptr, UVD_STATUS, UVD_POWER_STATUS and the
  first session-buffer dwords.
- **vcn2 power-off:** after `system power off` a nested panic ("Ticket lock
  ... unexpectedly owned") left the VM on a black screen. The VCN idle tick
  kept rescheduling itself (a fence never came back) into the halt; the
  power-off now stops VCN / JPEG first (`amdgpu_pci_shutdown` ->
  `amdgpu_device_ip_suspend`), log `VCN: stopped for the power-off`.

### Card run 33 (steps userq, pmctl, vcn2, amdgpu; build 852bbef)

- **userq:** the MES mapped the compute user queue (`userq diag: ME1 pipe 0
  queue 2 active: VMID 8, DOORBELL_CONTROL 0xe0003000, PQ_BASE 0x00004000,
  RPTR 0xe, WPTR 0x0_0000000e, WPTR_POLL ... 0x00401000`): the doorbell, the
  wptr poll through the process VM and the fetch all work, the CP consumed
  all 14 dwords. Yet `dst` stayed 0 and the fence did not signal, with no VM
  fault. The test's RELEASE_MEM had no GL2 / GLM write-back, so its writes
  could stay in the GPU caches: it now uses the GCR bits of
  `gfx_v11_0_ring_emit_fence`. Diagnostic: before REMOVE_QUEUE the kext logs
  the interrupt counters and the last unclaimed IVs (`userq diag: IRQs user
  queue N ...`, `unclaimed IV client ...`): no CP EOP IV with the queue's
  doorbell means the EOP interrupt never came (or is decoded differently).
- **pmctl:** 22 of 25. Power limit, profiles, activity monitor tables, OD
  sclk, OD restore all pass. Fails were the test's: on PMFW 80.92 the
  FAN_CONTROL feature off is answered OK but the PMFW keeps the fan (mode
  stays auto, PWM refused) - amdgpu uses the OD fan curve on these boards;
  now reported as info. The fan curve point was 0 C / 5 %, below
  OD_RANGE (25..100 C, 15..100 %) and rightly refused; it now uses the middle
  of the range.
- **vcn2:** `VCNn: fence-only test passed`: the VCPU writes fences. The first
  encode IB never completes (rptr stays at its start, 0x80; wptr 0x100).
  Diagnostic: instance 1 now runs the decode IB test first, instance 0 the
  encode one first, and the whole session buffer is zeroed (the
  `ffffffff` in the dump was never-written memory).
- **amdgpu (`rdna3amdgpu=1` on the default profile):** everything passed
  (displays, sleep / wake, mode switches, DMA, GFX, Vulkan, Metal), but the
  log had 662 `GFX: ring full (rptr 0x0, wptr 0x700)` / `GMC: failed to write
  reg ... wait reg ...`: the KIQ (on MES pipe 1) runs the TLB-flush packets
  but writes no rptr report, and with amdgpu's 256-dword padding the ring
  looked full after four flushes. The ring space now also counts from the
  last completed KIQ fence. The report archive did not arrive in time
  (collect.sh slowed by the 5 s waits): run the step again.

### Card run 34 (step big4, build 0977b02)

- **Boot stalled** (the user stopped it after ~1.5 min, no report): the
  serial log had 142 `GFX: ring full (rptr 0x0, wptr 0x380)` / `MES ring
  buffer is full.` / `MES: failed to reg_write_reg_wait` from the moment
  WindowServer started. With `rdna3amdgpu=1` and `rdna3mes=1` together the
  TLB flushes go through the MES scheduler ring
  (`amdgpu_mes_reg_write_reg_wait`), and the MES scheduler pipe, like the
  KIQ in run 33, writes no rptr report: each API call takes 2 x 64 dwords,
  so the 4 KiB ring looked full after seven calls and every later flush
  waited out its timeout. The ring space now also counts from the last
  answered QUERY_SCHEDULER_STATUS fence; the simulator reproduces it.
  Everything before (PSP, SMU, DMCUB with `rdna3dmubreset=1`, the clone,
  display power, GMC, IH, SDMA, KIQ / KCQ / GFX ring tests, MES up, VCN
  rings) came up. `VCN1: decode IB test failed (-62)` as in run 33
  (diagnostic). Run `big4` again.

### Card run 35 (step big4, build c5aab01)

- The MES fix held: no `ring full` / `MES ring buffer is full` in the
  serial log; the boot reached the Apple progress bar and WindowServer.
- **Kernel panic** ~17 s into the desktop: NX page fault at a kernel data
  address called from `IOAccelDisplayPipe::event_interrupt_gated`, right
  after the first plane-less display-pipe transaction (`iop: empty #0`).
  With `rdna3gamma=1` the accelerator hands such a transaction to the base
  class's `performTransaction` through weak imports; on 24H32 neither
  `IOAccelLegacyDisplayPipe::` nor `IOAccelDisplayPipe::performTransaction`
  is exported, the Auxiliary KC bound them to a kernel data address and
  the `gOSKextUnresolved` test let the call through. A weak candidate must
  now also lie inside IOAcceleratorFamily2 (within 1 MiB of the strongly
  imported `signalTransactionInterrupt`); otherwise the transaction is
  answered as before and the log names the rejected addresses
  (`iop: rdna3gamma=1 but IOAcceleratorFamily2 exports no
  performTransaction (weak 0x... / 0x..., unresolved 0x...)`). The gamma
  test then fails without a panic: gamma through the accelerator needs
  another route. Run `big4` again.

### Card run 36 (step big4, build 7e883c9)

- Both earlier blockers held: no MES `ring full`, no panic; macOS reached
  the desktop on both monitors and the report started (tests file only,
  the archive did not come). Looks 1-3: both pipes lit 1920x1080, pictures
  not black. Display sleep: both pipes blanked, monitors in standby.
- **The report stopped at the display wake** and the VM ran into the
  25 min limit. Serial: right after `pmset displaysleepnow` macOS put the
  whole system to sleep by itself (`display: system sleep #1`), woke
  (`wake #1: ... the card kept its power`, both heads `restored (0),
  running`), then slept again a minute later (`system sleep #2`) and the
  report never went on. `big4` is the first step that lets the guest
  suspend (QEMU `ICH9-LPC.disable_s3=0` for `rdna3resume=1`), and with the
  MacBookPro SMBIOS macOS idle-sleeps a minute after the displays. The
  report now holds `caffeinate -i` while it runs (idle sleep only; the
  display sleep and the planned `pmset sleepnow` still work).
- VCN: `VCN0: encode IB test failed (-62)` and `VCN1: decode IB test
  failed (-62)` at the bring-up and again after wake #1: with the
  diagnostic order of run 33 both instances fail whichever IB runs first,
  so it is not the encode path alone (fences alone pass). Cause found in
  the port: the IB tests submitted `ib_size_dw` dwords (the zero padding
  included) where `amdgpu_job_submit_direct` runs `ib->length_dw`; the
  VCPU stops at the zero-size package. The simulator now stalls the ring
  on such a package and reproduces both messages; fixed in `hw/vcn40.c`.
- Not reached: userq, pmctl, VCN tests from userspace, Vulkan, the planned
  system sleep, BACO. Run `big4` again.

### Card runs 37-38 (step big4, builds 150 and 152, 2026-10-12)

- Run 37 ran the framebuffer and accelerator approved on 2026-10-10/11
  (the new ones waited for "Allow"): built against display ABI 1, refused
  by the core, console mode; black shown blue (console base `0x80000001`,
  IOGraphics' flag bit taken as address), frames copied by the CPU about
  once a second. Fixed: the core answers ABI 1, the console base is
  masked, the queue stops for `rdna3vm approve` (`stale_kexts`).
- Run 38 (framebuffer / accelerator of build 150, the same code as 152
  for this run): colours right, desktop through the accelerator, the
  monitor's name; modeset 3 of 3, display sleep / wake, system sleep with
  every head back, BACO, DMA/GFX, Vulkan 13/13, Metal compute + render,
  A3 (IOSurface <-> Metal texture both ways) and **M6: Chess and Maps on
  the Metal plugin without a crash**.
- **Second monitor**: lit at boot, then HPD3 low for > 0.5 s (the monitor
  waking up on the new signal), the clone taken down, and every DMCUB
  command after that timed out (`DMUB: command ... not consumed (rptr
  0x240 ...)`, relight `SetPixelClock ... failed (-62)`). Cause: the
  clone's power down sent SetPixelClock with 0 for the CRTC just
  disabled, which the DMCUB never finished; amdgpu sends nothing there
  (`dce110_clock_source_power_down`: controller UNDEFINED,
  `set_pixel_clock_v7` returns before the DMCUB). Removed in the clone and
  the sleep path; the simulator hangs its DMCUB on that command. After a
  wake with the power lost the bring-up no longer tries a new clone before
  the boot head is restored (`source OPP 0 / MPCC 15 unknown`).
- Mode switches showed stripes / a cut picture for a moment on the HDMI
  monitor: the framebuffer now holds a mode lock across a switch and the
  accelerator's flips, so no old-size frame reaches the head mid-switch.
- After the wake `GEM_VA: Invalid argument`: `rdna3_drminfo --dma-test
  --gfx-test` mapped both jobs at the same VA in one process (amdgpu
  refuses that too); each job has its own VA now.
- OD fan curve point 2 -> -5: the PMFW refuses a table with one manual
  point among the all-zero auto curve; the selftest sets all five points.
- Open: gamma (macOS sends it in plane-less display-pipe transactions,
  IOAcceleratorFamily2's base class answers unsupported), VCN
  encode/decode and the user queue fence (diagnostics added: the ring /
  VCPU / MMHUB state on a timeout, the client's last GPUVM fault), M4
  presents never on screen, an underflow after the display wake.

### Card run 39 (step big4, build 154; framebuffer / accelerator of build 152)

- The second monitor lit at boot (the DMCUB fix held) and the extended
  desktop came up, but one monitor flickered whenever something moved and
  the other stayed black with only the cursor: the family had registered
  head 0's framebuffer itself (pipe 0 active, `count went 1 -> 1`), the
  accelerator never marked head 0 piped (`piped 0x2`), and "only head
  piped" sent both displays' frames to head 1. Fixed: a head whose
  framebuffer already drives a pipe is marked piped
  (`pipeOfFramebuffer`).
- Unplugging the HDMI monitor (head 0) blanked the other for ~20 s:
  recheck with the routing fixed.
- System sleep #1: QEMU kept the card powered through the guest's S3; the
  wake found the PSP sOS running with its TMR freed and `LOAD_IP_FW SMC`
  timed out (`resume #1 FAILED`); sleep #2 lost the card (config space all
  1s), QEMU `vfio: Unable to power on device, stuck in D3`, and the host
  hung handing it back (amdgpu `trn=2 ACK should not assert`). amdgpu
  resets a dGPU at the end of every S3 suspend
  (`amdgpu_pmops_suspend_noirq` -> `amdgpu_acpi_should_gpu_reset` ->
  `amdgpu_asic_reset`, mode 1): ported into `systemSuspend`
  (`RDNA3,SuspendResetResult`). `rdna3vm` reboots the host instead of
  suspending it when QEMU reports the card stuck in D3.
- The run used the framebuffer of build 152 although 154 was installed
  and the queue did not stop: the report now prints the running and the
  installed hashes (`== running vs installed`).

### Card run 40 (step big4, build 156)

- Both monitors show the desktop, no flicker (the pipe marking fix);
  mode switches clean (a blink, then the lower resolution); unplugging
  HDMI no longer blanks the other monitor. After replugging HDMI it lit
  but stayed black: open (the re-created display pipe gets no frames?).
- System sleep #1 with the new suspend reset: `mode-1 reset before the
  sleep -> 0`, the wake brought both monitors back (`resume #1 done`).
- Later a kernel-ring fault (`[gfxhub] page fault ... vmid:0 at 0x0 from
  client 10`, CP stuck, `CPF 0x94000003`), PTE writes to
  `0x900000000000 ... outside the BAR`, and at sleep #2 the MES did not
  answer; the suspend reset on that GPU took the host down (serial and host
  log end mid-suspend). Our mode-1 resets read MEMSIZE through the BARs
  before writing the PCI config back (or never wrote it back);
  `amdgpu_device_mode1_reset` restores it first
  (`amdgpu_device_load_pci_state`). All mode-1 resets now go through
  `mode1Reset()` with amdgpu's order. Open: the vmid-0 fault and the
  `0x900000000000` table after the first wake.

### Card run 41 (step agdc, build 169, 2026-10-14)

- User: after the verbose boot text both monitors stayed black with a
  signal. Both pipes scanned out black (CRC 0), every display-pipe counter
  0 (not even plane-less transactions): WindowServer aborted at every start
  (six PIDs, `23-windowserver-crash.txt`: `__assert_rtn` in
  `CoreDisplay_CreateDisplayForCGXDisplayDevice`, after a CoreDisplay error
  in `CoreDisplay::Framebuffer::Framebuffer` / `DisplayPipe`).
- RDNA3AGDC was up and answered (709 vendor calls, K5 link / plane scaler
  28 each for index 0), but its slot table held one framebuffer
  (`rdna3agdc_fbmap n=1`, `maxfb 1`) while GPUWrangler listed two
  (`fb0:1 .../RDNA3DisplayHead/RDNA3Framebuffer`, `fb1:0 .../RDNA3Framebuffer`):
  the table was scanned a moment before the second head's framebuffer
  registered and never again.
- Fix: the table is scanned again at every 0x980 query and for a K5 query
  naming an index it does not have; a publish or terminate looks 500 ms
  later, one worker at a time (THREAD_CALL_OPTIONS_ONCE); the left-out
  reasons are logged whenever they change. Triage: the AGDC build hash
  parsed without the trailing colon, a `WindowServer starts` row (more than
  two WindowServer PIDs = a restart loop).

### Card run 42 (step agdc, build 171)

- The desktop on both monitors again, no WindowServer restart: AGDC's
  table held both framebuffers (`n=2`, `maxfb 2`). METAL ALL PASSED.
- Still only plane-less transactions (`iop_empty` 199, `iop_ok` 0), gamma
  FAIL as before. AGDC got 13 vendor calls and no K5 question
  (`k5_asked 0`); refused 0x3, 0x941, 0x923, 0x2, 0x2001.
- The object came up 600 ms after WindowServer had opened the accelerator
  (the 500 ms delayed kick, re-armed by each publish). In run 41 it was up
  before WindowServer and IOPresentment asked the K5 questions. NullMoth
  holds the registry busy until its display stack is armed.
- Fix: the object is made at once when the first framebuffer publishes (a
  100 ms retry while the registry does not show it yet); only table
  updates wait 500 ms (the table is also rescanned at every 0x980).

### Card runs 43-44 (step agdc, build 171)

- Run 43: macOS still ran the AGDC allowed before (`approve-*.txt`), the
  step stopped before testing. The user allowed it in System Settings.
- Run 44: the new AGDC ran (`UP on S80` early in the boot, both
  framebuffers in the table, vendor calls 0x3 and 0x941 before
  WindowServer), but the report script installed the host's bundle again
  and stopped without a report: `rdna3vm prepare` packs a new tarball each
  time, its hash changed while its kexts did not, and every reinstall
  re-signs them (another "Allow" for the same build).
- Fix (tooling only): autoreport.sh leaves a kext alone when it equals the
  copy installed before (`diff -r` against `$D/kexts`); only a changed
  kext counts as an update. `rdna3vm step`/`args` run the same boot-args
  once more after a rerun marker, like the queue.

### Card run 45 (step agdc, build 174)

- The current AGDC ran (sources = installed = running); the triage's STALE
  flag came from kernelmanagerd waiting for a relinked copy of the same
  sources (a CI rebuild reinstalled it): now only a note when `stale:` is
  empty, and autoreport.sh compares kexts by RDNA3SourceHash.
- The object was up early and IOPresentment asked K5 for both framebuffers:
  `k5_link 56`, `k5_caps 56`, masks 3, nothing refused for a known index.
  WindowServer aborted at every start, 16 PIDs (`__assert_rtn` in
  `CoreDisplay_CreateDisplayForCGXDisplayDevice`), both pipes black, no
  transaction at all. 996 refused calls, mostly 0x711 for capability types
  other than the plane scaler, 0x925, 0x2001.
- Runs 41 and 45 (K5 answered) both crash-looped; run 42 (no K5 question)
  had the desktop. The slot table (run 41's suspect) was full in run 45.
  NullMoth keeps its K5 off after every boot (runtime sysctl only).
- Fix: K5 is off with `rdna3agdc=1`/`2`; `=3` answers from the start (the
  crash, kept for experiments); `=4` turns K5 on 90 s after the object is
  up, after WindowServer started (step `agdc`). Refused 0x711 types are
  logged and collected in `debug.rdna3agdc_k5_types`. Triage counts
  WindowServer PIDs in the report's WindowServer log (it read only the
  tests file and showed 0).

| Symptom | Likely cause | Action |
|---|---|---|
| `agdc`: `k5_link`/`k5_caps` > 0, WindowServer restarts (assert in `CoreDisplay_CreateDisplayForCGXDisplayDevice`), both screens black | K5 answered while CoreDisplay builds its displays (`rdna3agdc=3`, or `=4` with a slow WindowServer start) | `rdna3agdc=4` (or a longer delay, `kAgdcK5LateMs`); compare with `rdna3agdc=1` |

### Card run 46 (step agdc, `rdna3agdc=4`, build 175)

- No reinstall this time (the bundle's kexts unchanged): the tooling fix
  works. The object came up with K5 off, both framebuffers in the table;
  K5 went on at 90 s, just before the report script started.
- Then nothing: no tests file, no report, the VM stopped at the 12 min
  limit. The serial log shows no panic and no RDNA3 error; userland
  services kept logging "unexpected session uid -1" (no GUI session).
- WindowServer never opened the accelerator in run 46 (0 surfaces, 0 GPU
  tasks, 0 display-pipe clients), while in run 45 it did at each of its 17
  starts before aborting: run 46 stopped earlier.

### Root cause of runs 41, 45, 46 (analysis of all four AGDC runs)

- **The abort (41, 45) is not about K5's contents.** Disassembled on the
  self-hosted macOS runner (same build, 15.8.1 24H32;
  `tools/probe/coredisplay.sh`): `CoreDisplay_CreateDisplayForCGXDisplayDevice`
  builds a `CoreDisplay::Display`, which first looks for a display pipe
  for the framebuffer (`FindDisplayPipeForService`, remembered at +0x14c)
  and then calls `CreateMetalDevice(accelerator)`. When that throws, the
  catch logs "Failed to create MetalDevice for accelerator" and goes on
  if there is no display pipe, but asserts **"Failed to create MetalDevice
  (%s)"** if there is one. A display pipe exists only with an AGDC object
  up when WindowServer starts.
- Our Metal plugin gives WindowServer no device: it is not on
  `rdna3mtl-allow.txt`. NullMoth's installer documents the same abort for
  an allow list WindowServer could not read.
- **Run 42 had the desktop only because AGDC came up after WindowServer**
  (GPUWrangler: `agdc 0x000000000`); in 41/45 it shows `/RD3DeviceControl`.
  CoreDisplay's `[ERROR]` backtrace (Framebuffer ctor) is in run 42 too:
  harmless.
- **The hang (46):** the object came up while WindowServer was starting
  (5 log lines after IOConsoleUsers), K5 off; WindowServer stopped after
  9 vendor calls. NullMoth holds the registry busy (`NVRM::fBootHold`)
  until all heads published, the accelerator armed, K5 set, AGDC up and
  iop on, so WindowServer's IOKitWaitQuiet returns on a finished stack.
  We had no such hold, so where AGDC lands relative to WindowServer was
  luck (before: 41/45, during: 46, after: 42).
- My earlier claim that NullMoth keeps K5 off was wrong: its NVRM writes
  `debug.nvrmfb_agdc_k5=1` before `debug.nvrmfb_agdc=1`.
- The report hid the cause: the crash file was cut at 24 KB before the
  `asi` (assertion text), and the WindowServer log filter dropped
  CoreDisplay's info lines.
- Fixes: `@WindowServer` in the allow list (the plugin gives WindowServer
  a device only while `debug.rdna3agdc` reads 1), the plugin and list
  readable by `_windowserver` (build_metal.sh); the AGDC loader holds the
  registry busy until the object is up and its slot table settled 5 s
  (cap 40 s, `RDNA3,AGDCBootHold`); step `agdc` back to K5 from the start
  (`rdna3agdc=3`); collect.sh prints the crash's `asi` and
  `24-log-coredisplay.txt` (CoreDisplay, IOPresentment, GPUWrangler,
  NVMTL, info level); triage shows the assertion and the plugin's
  WindowServer lines.

| Symptom | Likely cause | Action |
|---|---|---|
| `agdc`: WindowServer restarts, assert "Failed to create MetalDevice" | WindowServer got no Metal device from the plugin while a display pipe exists | the allow list has `@WindowServer`, mode 644; the plugin rebuilt with it (build_metal.sh); `NVMTL:` lines of WindowServer in `24-log-coredisplay.txt` say why |
| apps mode (the `%apps` line of `rdna3mtl-allow.txt`, N1): an application misbehaves or the GPU address spaces run out | every application on the desktop now gets the device while the card is armed and WindowServer is on the plugin | `34-metal-allow.txt` (triage "Metal apps mode"): which processes got it and why the others did not; the kernel log's `VM: VMID n for <process>` lines and `VM: no VMID` (15 VMIDs, one per open file; 7 with `rdna3userq`). Deny the app with `-name`, or `rdna3mtlapps=0` / no `%apps` line for the old explicit list. Safari's WebKit.GPU cannot load the plugin at all (sandbox, card run 56): not a driver bug |
| `agdc`: WindowServer never opens the accelerator, no report | AGDC appeared while WindowServer started | `RDNA3,AGDCBootHold` must say "display stack armed"; "CAP" = the object or the table was late |

### Card run 47 (step agdc, `rdna3agdc=3`, build 177)

- The new diagnostics named the next layer at once: WindowServer is
  allow-listed and the plugin's device initialises, but RADV inside
  WindowServer enumerates no GPU (`vkEnumeratePhysicalDevices -> count 0`,
  "NVK did not come up in this process"), so CoreDisplay still asserts
  "Failed to create MetalDevice" (16 WindowServer starts).
- Why: the serial log has `IOUC RDNA3UserClient failed MACF in process
  pid 157, WindowServer`: WindowServer's sandbox refuses our DRM client
  class. NullMoth's NVK reaches its GPU from WindowServer through the
  accelerator: `newUserClient` type 0x4E56xxxx returns `IOAccelNVRMBridge`
  attached to a carrier, a class name the sandbox lets through. Our
  accelerator already had that path (type `RD3K_UC_TYPE_BASE`, carriers)
  but the core answered `rdna3NewAccelClient` with Unsupported.
- Fix: the core makes an `IOAccelRDNA3Bridge` (an RDNA3UserClient with the
  device set explicitly, its provider is a carrier); libdrm_xnu opens
  RDNA3Accelerator with type 0x52440000 when opening RDNA3Accel fails.
  Only the core kext (OpenCore-injected) and libdrm changed: no approval.

| Symptom | Likely cause | Action |
|---|---|---|
| `NVMTL: vk: zero physical devices ... in WindowServer`, serial `IOUC RDNA3UserClient failed MACF ... WindowServer` | the sandbox refuses the DRM client class | libdrm falls back to the accelerator bridge; serial `newUserClient(0x52440000) -> IOAccelRDNA3Bridge on the ACCELERATOR` shows it was used |

### Card run 48 (step agdc, build 179): WindowServer on Metal (M5)

- The install boot crash-looped as expected (the old libdrm in the
  plugin), synced the plugin's libdrm and asked for the rerun.
- The rerun: the boot hold released after 6.5 s ("display stack armed",
  both framebuffers), K5 answered for both, **one** WindowServer start.
  WindowServer's open of RDNA3UserClient failed MACF, the libdrm fallback
  got `IOAccelRDNA3Bridge` from the accelerator, RADV came up inside
  WindowServer and the plugin is its Metal device: **WindowServer
  composites on our Metal plugin** (thousands of submissions), the user
  saw the mode-switch fade. Desktop on both monitors, presented by
  surface swaps and zero-copy flips (40 `FLIPPED`); display-pipe
  transactions stay 0 on this path (triage: "WindowServer on Metal").
- Scaled modes 4 of 4. Gamma: CoreDisplay now sends tables continuously
  (MPCC RAM A/B ~450 times); the dark table moved both pipes' CRC and the
  restore put the OGAM back to bypass, but the screen had changed between
  "before" and the test, so the verdict compared the wrong looks. The
  test now also accepts restored = the identity look when ColorSync's
  table is the identity.
- The report's CoreDisplay log kept only its last 600 lines and lost the
  plugin's start in WindowServer: now the first 400 and the last 200,
  without the plugin's per-texture lines.
- Step `agdc` gets the Metal probes (`rdna3probe=1`: M4 CAMetalLayer on
  screen, M5, M6 Chess and Maps) now that WindowServer is on Metal.

### Card run 49 (step agdc + probes, build 180)

- WindowServer on Metal for 9 minutes through modes (4/4), display sleep
  and wake, gamma, A3 and **M4: the CAMetalLayer window was on the screen**
  (the user saw it magenta, 8470 magenta pixels on pipe 1). mtlprobe said
  FAILED only because no presented callback arrives on the flip path; the
  triage now decides M4 by the screen.
- Gamma: on pipe 0 the identity table brought the CRC of before back
  exactly; ColorSync's restore loads its own curve (a RAM table), so its
  CRC differs although the screen looks as before (the user saw it come
  back). PASS now also when the identity table returns a pipe's CRC.
- **M6 (Chess) broke the desktop**: Chess opened the device through the
  bridge, then `[gfxhub] page fault vmid 3 at 0x800100958000, client 10`
  (SQC data, MAPPING_ERROR, read) and "VMID 3's client is guilty";
  WindowServer aborted ("COREANIMATION ... pipeline: Vulkan is not up in
  this process") and restarted six more times about every 10 s; Maps
  could not launch. No GPU hang or reset. The report kept neither which
  process owned VMID 3 nor why the later WindowServers died (the log's
  middle was cut, one crash file kept).
- Diagnostics: the kernel logs each VMID with its process and pid, the
  guilty line names it, a freed VM says so; collect.sh keeps the whole
  CoreDisplay / plugin log and `25-windowserver-exits.txt` (every
  WindowServer crash of the boot: pid, launch, termination, asi; launchd's
  exits).
- IH ring overflows (3 this run, 1 in run 48) precede the fault: the ring
  fills faster than it is read; to look at with the VMID owners known.

### Card run 50 (step agdc + probes, build 181)

- Install boot (new bundle, then shutdown): **kernel panic at shutdown**,
  NULL read in `IOAccelLegacySurface::clientClose+0x1cc` (RAX 0xffffffdf)
  from `iokit_client_died` while WindowServer (pid 158) exited, after
  "VMID 2 of WindowServer freed". The test boot's shutdown passed the same
  path without a panic. tools/probe/iofam.sh disassembles the function on
  the macOS runner; the installs now run without the card anyway
  (`rdna3vm _install`).
- Test boot: modes 4/4, display sleep/wake, **gamma PASS on both pipes**,
  Vulkan 13/13, Metal compute + render, A3, M4 on the screen (8470 magenta
  pixels), WindowServer on Metal for 9 minutes with 40 zero-copy flips.
- **The M6 cause, found with the VMID owners**: Chess (pid 1571) got
  VMIDs 3 and 4, crashed 3 s after its launch, and its file was closed with
  a job still on the GFX ring: "VMID 3 of Chess freed", then the GFXHUB
  fault at 0x8001006a8000 on VMID 3 "which no client owns" (GRBM and CP
  busy). The job ran on into torn-down page tables; WindowServer's
  Vulkan device went down ("MTLTexture creation failed - vulkan down"),
  it aborted and every later WindowServer died with "Vulkan is not up".
  amdgpu never frees a VM under its jobs (amdgpu_flush waits for the
  contexts' entities, amdgpu_vm_fini for the VM's last fences, TTM frees
  the page tables only once the fences in their reservations signal).
  Fix: `rdna3_drm_file_close` waits for the file's last submission on
  every ring before it unmaps, destroys the VM and drops the BOs (the OS's
  fence wait: its 10 s job timeout recovers a ring that never gets there),
  and takes no new ioctls meanwhile (test_uapi).
- Why Chess crashed in the first place is still open (its .ips was cut
  after the header in the tests file; the next run has the full exits).

### Card run 51 (step agdc + probes, build 182, first install without the card)

- The bundle went in through the headless boot (`bundle-*.txt` came back),
  the card step ran the new build straight away.
- **The M6 fix holds**: no page fault, one WindowServer for the whole boot
  (run 50: seven), **Maps ran on the plugin** without a crash.
- Chess (an OpenGL app on Metal) aborted in
  `GLDDeviceRec::getBuiltinRenderPipelineStateFromDescriptor`
  (MTLReleaseAssertionFailure): the plugin returned no render pipeline for
  the GL layer's present blit. Its plugin log stopped after four lines: the
  report kept only plugin lines that named RDNA3; collect.sh now keeps the
  plugin's log of every process (26-log-metal-plugin.txt).
- mtlprobe FAILED in `vkCreateComputePipelines`: RADV's descriptor pool
  (VRAM with CPU access) got VK_ERROR_OUT_OF_DEVICE_MEMORY. Only 144 MiB of
  VRAM are CPU-visible (BAR0 cut to 256 MiB for the VM), and a VRAM-only
  request had no fallback. amdgpu_gem_create_ioctl retries with GTT added;
  the UAPI does that now (its other retry, dropping CPU_ACCESS_REQUIRED,
  relies on TTM moving the BO on a CPU fault, which nothing does here). The
  kext logs how full both VRAM heaps were when one did not fit. Chess's
  pipeline likely failed the same way.
- False FAILs fixed: M4 is decided by the screen (8238 magenta pixels),
  mtlprobe never gets presented callbacks on the flip path; M5 read lsof's
  name cut to "WindowSer" as "not loaded".

### Card run 52 (step agdc + probes, build 183)

- Metal compute + render PASS again (the GTT fallback), M4 and M5 PASS,
  Maps on the plugin, one WindowServer, gamma and modes as before.
- Chess still aborted: the whole plugin log (26-log-metal-plugin.txt) shows
  the GL layer's shaders lowered and translated, then
  `vkCreateDescriptorPool -> -2` before every pipeline, and the kernel
  logged no VRAM-full line. RADV makes its descriptor pools VRAM BOs with
  neither CPU_ACCESS_REQUIRED nor NO_CPU_ACCESS and maps them at once; the
  kext let such a BO land outside the BAR when the 144 MiB visible part was
  full, and a BO there cannot be mapped (amdgpu moves it into the BAR on
  the CPU fault, amdgpu_bo_fault_reserve_notify; nothing does here). Only
  NO_CPU_ACCESS BOs go outside the BAR now; the rest falls back to GTT.
  A refused map of a VRAM BO is logged.
- Shutdown panic (run 50): probe run 4 showed clientClose loading the
  surface's accelerator (+0x12c8) at +0x1e and finding it NULL at +0x1c5;
  probe run 5 lists its writers: IOAccelSurface::stopLocked, ::stop and
  ::detach_surface. Probe run 6: stop takes lock_busy like clientClose (no
  race); detach_surface does not and runs from IOAccelSurface::didTerminate,
  i.e. when IOKit terminates the surface. Run 50's install boot had run
  `kmutil load` on the new accelerator just before its shutdown, which
  terminates the running instance while WindowServer's surfaces close.
  Fix: RD3Surface runs clientClose and didTerminate under one recursive
  lock (rd3AccelSurfaceGuard, logged when one waits), and autoreport no
  longer kmutil-loads a kext that is running (the new build loads from the
  next boot anyway; installs run without the card now).

### Card run 53 (step agdc + probes, build 184)

- **M6: both apps ran on the plugin without a crash** - Chess (OpenGL on
  Metal) too, after the GTT placement of CPU-mappable BOs; Metal compute +
  render, A3, M4 on the screen (8238 magenta pixels), M5, one WindowServer.
- The result still said "something failed" for two probe exits that are
  not failures: mtlprobe's M4 verdict waits for presented callbacks the
  flip path never sends, and V3 records that RADV has no present path on
  macOS. autoreport now labels the M4 line "NO PRESENT CALLBACK ... the m4
  scanout look decides" and exits 0 for both; triage ignores the probes'
  exit codes (M4 is decided by the screen in the summary).

### Card run 54 (step agdc + probes, build 187)

- **Every reached check passed** (M6 2/2), but the user saw three faults:
  System Settings' sidebar did not update while it scrolled (only after a
  resize), Chess's window was see-through with no title bar, Maps' text was
  misplaced.
- WindowServer logged `copy_forward_region: copy forward failed
  (0xe00002c7) ... disabling` 813 times. Probe runs 7-10 (CoreDisplay,
  IOAcceleratorFamily2 on the runner): CoreDisplay calls
  `IOAccelSetSurfaceFramebufferShape(surface, rects, 0x1000, fb)`; shape bit
  12 goes to `IOAccelLegacySurface::copy_forward`, which calls the virtual
  `submitCopyForward(event, fb index, buffer 0 = front, buffer 1 = back,
  rects, count)`. The family's own is a bare `return kIOReturnUnsupported`,
  and CoreDisplay ignores the failure: the back buffer keeps regions two
  frames old. RD3Surface now implements it (`rd3AccelCopyForward`: SDMA by
  `rdna3CopyVram`, the CPU for small rects; `rdna3accelcf=0` restores the
  refusal).
- Open question for the next run: WindowServer composites into per-process
  copies of its framebuffer IOSurfaces (G13 surface-in/out, 8-10 ms per
  1920x1080 frame), so the next surface-out may write the stale copy over
  the kernel's copy. `copy forward check #n ... KEPT / REVERTED` in
  `27-copy-forward.txt` answers it: KEPT means the fix holds, REVERTED means
  the copy forward has to reach the plugin's copy (or the IOSurfaces have to
  live in VRAM, item 7 for RADV).
- The report gains 1280-wide scanout looks (`--scanout-hires`) of Chess,
  Maps, System Settings and the desktop, `27-copy-forward.txt`,
  `28-plugin-warnings.txt`, and logs collected for up to 180 s (45 s cut
  the plugin log off before Maps).

### Card run 56 (step agdc + probes, build 191)

- **The screen went black** but for what changed. `27-copy-forward.txt`:
  every check read `copied 0x00000000 ... KEPT`. The copy reached the
  flip, but its source was empty: a legacy swap is presented by flipping
  its back buffer itself (zero-copy, `rd3AccelFlipTo` on `a2`), never by
  the swap copy into the front buffer, so buffer 0 (the front) never holds
  a frame and buffer 1 (the back) already holds the last one. Copy forward
  from a buffer that was never shown is now a no-op; it copies only from a
  buffer that went to a panel. Copy forward was therefore not what kept
  the System Settings sidebar stale.
- **Chess crashed** in RADV: `radv_shader_part_cache_get` (NULL) from
  `lookup_ps_epilog` during `glReadPixels`' blit. The plugin makes depth
  and stencil state dynamic (Vulkan 1.3 core) without enabling
  `extendedDynamicState`, and RADV creates its PS epilog cache only with
  that feature. The plugin now enables `VK_EXT_extended_dynamic_state`, and
  mesa patch 0008 creates the cache always. The run before it drew Chess
  almost right (a few cyan slivers on the knights).
- **Point and line draws were skipped** in WindowServer ("G14 draw:
  point/line primitives ... NOT drawn"): RADV reports
  `dynamicPrimitiveTopologyUnrestricted = false`. Mesa patch 0009 compiles
  pipelines with a dynamic topology for an unknown topology class (as with
  graphics pipeline libraries) and reports the feature.
- **Every host-pointer import was refused** ("vkAllocateMemory refused
  the imported host pointer", including 14.7 MB buffers). RADV imports
  read-write, and WindowServer gets the apps' CoreAnimation backing stores
  mapped read-only. GEM_USERPTR now wires such pages read-only after a
  read-write refusal (the VA mapping drops write), and logs refusals with
  their IOReturn. This is the lead for the stale sidebar: the CPU-drawn
  layers reached WindowServer only as copies.
- **Buffer textures came back nil** (glyph / icon textures: "bytesPerRow
  ... not a multiple of 32", "refused on memory types"): RADV cannot alias
  them as the plugin's NVK path did. A read-only buffer texture over
  CPU-visible bytes is now a private image refreshed from the buffer once
  per command buffer that uses it.
- Known limit: Safari's GPU process (`com.apple.WebKit.GPU`) cannot load
  the plugin: its sandbox blocks mapping code from `/Library/GPUBundles`
  ("file system sandbox blocked mmap()"), SIP off or not. Apple's own GPU
  bundles live on the sealed system volume; M6 for Safari needs the bundle
  there or WebKit's GPU process off.

### Card run 57 (step agdc + probes, build 8175274, 2026-10-15)

- **The HDMI monitor (head 0, the boot head: OTG0 -> DIGD -> UNIPHYD)
  said "no signal"**; the DP-3 one (clone / extended head through a DP++
  adaptor) worked and macOS made it the secondary display. Every look
  read pipe 0 `LIT`, its OTG CRC moving and the desktop in its picture;
  mode switches, display sleep / wake and gamma worked by the registers.
  The run before it (same build) showed the boot screen, then WindowServer
  never started (one-off).
- The core's display log is the same as card run 56's (build a96c9a1,
  both monitors fine): the same route, clone (`PIXEL_RATE 0x0 PHYPLL
  0x1`), snapshots, gamma tables, DPP-scaler mode switches (no timing
  change), no hotplug, the head-0 wake check `intact, 0 regs differ`
  (the snapshot holds the DIG front end's HDMI / packet registers, the
  TMDS controls and the OTG's V_TOTAL_MIN / MAX / CONTROL). The FreeSync
  batch (N6) changes nothing without `rdna3vrr=1`: no caps read (the mode
  list came back unsupported), no DRR, the HDMI SPD only from a zeroed
  stream. No cause in the driver's code or logs; the report could not say
  when the monitor went dark nor what left the DIG.
- Diagnostic build: `hw/dcn32_txstate.c` reads what each head sends after
  its OTG (DC's DTN-log read-backs), the core logs it at the start, after
  the clone and every change (2 s, `rdna3txwatch=0` off), publishes
  `RDNA3,Head<n>,TX`, and the screen looks print it (`TX` lines,
  `RD_NR_XNU_TXSTATE`); triage prints the lines, the warnings and a
  `TX watch` summary. The simulator showed the states it names (DRR
  bounds of 0, a DRR range, a DIO FIFO error, AV mute, the back end off,
  a DIG chain that cannot be followed). After the ECC review: a chain not
  followed is never "none" (every DIG's routing is read instead), the
  watch runs without the SMU metrics, skips a tick when the display lock
  is held (counted), counts its ticks (`RDNA3,TXWatchTicks`), always logs
  a change of the problems or of the routing / enables / clock selects,
  ignores status and self-clearing fields, and watches the clone of
  `rdna3mirror=1` too.


### Card run 58 (step agdc + probes, build 98582a0, 2026-10-15c)

- The first run with the current Metal plugin and RADV (A3 and N1 on the
  card). **The main (HDMI) screen kept an old frame** - the wallpaper and
  the menu bar - while the windows on it (Terminal, Settings, Chess, the
  menus) flashed in for a moment; the DP screen looked fine (it only
  showed the static desktop). The looks agree: pipe 0 scanned out one
  buffer (0x800b100000, later 0x800bc50000) whose picture lacked the open
  Terminal and never changed in 2 s; all four M6 hi-res pictures of pipe 0
  were byte-identical.
- Cause: A3 made WindowServer's 1920x1080 compositing IOSurfaces GEM BOs
  over their own device-cache VRAM (`GEM: IOSurface 1..4 ... at 0xd0f0000
  / 0xd8e0000 / 0xe0d0000 / 0xe8d0000`, each allocated by the import's
  wire). The legacy surface swap scans out other allocations - the
  IOAccelSurface's buffers (0xb100000, 0xb8f0000, 0xc0e0000, 0xc8d0000),
  which the family fills from the IOSurface's system memory (page-on
  through the aperture). No flip ever went to an A3 BO. The plugin's
  "surface-out -> dirty, no copy" left that system memory stale, so the
  panel showed whatever a CPU lock had paged off last (three 8 MB
  page-offs at 17:46:48, the early desktop).
- Interim fix: WindowServer surfaces of a head's scanout size stay out of
  A3 (`common/rdna3_a3_display.h`, status `RD3K_SS_DISPLAY`, errno EPERM
  to the plugin, which composites them through copies as before A3);
  `rdna3a3display=1` / `sysctl debug.rdna3accel_a3display=1` lets them in
  again. The next run must show `surfgem: ... refused: WindowServer's
  display framebuffer` for the 1920x1080 surfaces, `a3 display: head 0:
  ... kept out of A3`, the plugin's `import refused (... kernel errno 1 ...)
  - private image + copies` for them, and pipe 0's M6 pictures differing
  (triage `M6 screens`, `FAIL M6 screen` otherwise).
- The 45 VMFaults: Terminal (VMID 5, apps mode) TCP reads at an unmapped
  VA 0x80018223f000, 9 times from the 960x540 mode switch on (and after
  its VM was marked guilty), each followed by 4 CPC writes at address 0 on
  VMID 0 ("which no client owns"). At the first one WindowServer's GFX job
  hung for 10 s (soft reset, WindowServer restarted). Open (DRIVER-PLAN
  N27); triage now groups the faults (`VM faults` in the summary).

### Card run 58b (step agdc + probes, build 751f5b8, 2026-10-15d)

- With WindowServer's display framebuffers kept out of A3 both screens
  updated again, but **windows and menus flickered on both screens**. The
  result came without the report archive or pictures: the VM's 16-minute
  limit stopped the run inside M6 (no `M6:` line); the serial log and the
  tests file are all there is.
- Evidence: WindowServer imported 615 of its other inputs as BOs
  (`debug.rdna3accel_a3display_other 615`: the menu bar strips 1920x24,
  status items 44x24, window backing stores), and its plugin wrote into
  those shared VRAM copies itself: `surface-in SKIPPED (shared VRAM current
  at seed 3): 2 skipped, 747 copied` - 747 uploads of the IOSurfaces'
  system memory into VRAM the owners also draw into through A3 (their
  "surface-out -> dirty, no copy" leaves that system memory stale), plus
  `dirty refused: errno 19` copy-outs for surfaces the accelerator never
  drew. Nothing orders an owner's GPU or CPU write with WindowServer's
  upload, so a window shows its newest frame and an older one in turns.
  In run 58 (2026-10-15c) the same imports existed, but the main screen
  was frozen and the second static, which hid it.
- Fix: every WindowServer import stays out of A3 by default
  (`rd3_a3_ws_policy`, status `RD3K_SS_POLICY`, EPERM to the plugin, which
  goes through copies as before A3); applications keep A3 for their own
  and shared surfaces. `rdna3a3ws=1` lets WindowServer's inputs in again,
  `rdna3a3=0` turns A3 off for every process. The kernel now measures
  flicker: per head and band of the picture the swaps that show the
  picture of two swaps before (`flicker:` lines,
  `debug.rdna3accel_swaps_abab_h<N>` / `swaps_hashed_h<N>`, triage
  `flicker`; second review: the baseline is a dump right before the
  probes, which run with the test on for every swap, and one band taking
  turns on one head - a blinking caret - is a WARN, not a FAIL). The run's
  VM limit grows by 5 min for the probes (M6), and the queue says when a
  step was cut by it.
- GAMMA FAIL was the flicker (`CRC before NOT stable`, the dark table
  reached both pipes): rdna3_modetest now says `GAMMA: UNJUDGED` then.
  Display sleep FAIL was the system going to sleep after the display: the
  kernel blanked both heads first (`head 0/1 blanked (display sleep)`,
  `system sleep #1`); autoreport keeps the system awake through the look.
- The main display is the DP head (pipe 1: CG display 0's mode switch
  resized pipe 1 in both runs); triage takes the main pipe from that look.

### Card run 58b, second run (step agdc + probes, build c010ebb, 2026-10-16)

- **The flicker stayed on both screens** with every WindowServer input kept
  out of A3 (`surfgem: ... refused: a WindowServer input`, the display
  framebuffers refused): A3 was not the cause. The user stopped the VM
  about 1.5 min after the login (Esc x3), so the serial log is all there
  is; triage now reads its swap and flicker lines (`swap/flicker`).
- Evidence: the flicker test fired on head 1 (`120 of 204 hashed swaps`,
  `A B A B in 6 of 8 bands`, 53 in more than one band) and later on head 0
  (`7 of 8 bands`). After the login the user's WindowServer made its
  display surfaces with two back buffers (`shapeSurface(mode 0x6)`: buffers
  1 and 2; buffer 0, the front, is the head's framebuffer resource with its
  own VRAM, 0x87040000 / 0x88060000), and head 1's swaps took turns
  between 0x8b100000 and 0x8b8f0000 from swap #113 on; head 0 stayed on
  one buffer (few A B A B) until it too took turns. Card runs 15b-15d show
  the same turns; card run 56 (before eb5e3e8) used one back per head.
- Cause: the accelerator flips the panel to the back (zero-copy), so the
  front never holds a frame, and since eb5e3e8 copy forward copied nothing
  when its source, the front, had never been shown ("the back buffer
  already holds the last frame"). That is true with one back only. With
  two, the back WindowServer draws next holds the frame of two swaps
  before; WindowServer redraws only this frame's damage and relies on copy
  forward for the regions the last frame changed, so each back kept them
  two frames old and the panel showed the two backs in turns. The cross
  head line at swap #84 ("head 0 now gets head 1's frame") was not a
  mis-route: the old WindowServer's surfaces were freed and the new ones
  got the same VRAM.
- Fix (`common/rdna3_swap_front.h`, tested by `tests/test_swap_front.c`,
  which reproduces the A B A B with the old rule): copy forward takes the
  last frame from the buffer the head's last swap presented, only when it
  is one of the calling surface's own buffers, read through that live
  resource (never another surface's or process's frame), nothing when that
  buffer is its target (one back), nothing before the head showed anything
  (never the front then), and the front only when it was shown. A copy
  that left rects stale (or a call that copied nothing) leaves them
  pending for that buffer; the next copy forward into it copies them too,
  from a source that did not miss them itself (not reported to
  CoreDisplay, which would switch copy forward off for good). A swap never
  sends the front to the panel (it did when the back read empty); an empty
  back is shown once the head's last 3 swaps in a row were skipped (a black
  or clear frame; two empty backs taking turns count too). Per head the buffers shown are
  logged by phys (`swap #n: head N shows a new buffer`, pointers read
  `<ptr>`) and summed up with copy forward's outcomes and the flicker
  counts (`swap: head N: ... swaps over K buffer(s) ...`); every skip and
  refusal has its own reason line and counter: `swaps_skipped_h<N>`,
  `swaps_frontskip_h<N>`, `swaps_emptyshown_h<N>`, `swaps_bufchg_h<N>`,
  `bufs_new_h<N>`, `swaps_skipped_all`, `swaps_nohead`, `cf_shown_h<N>`,
  `cf_src_h<N>`, `cf_backlast_h<N>`, `cf_nothingshown_h<N>`,
  `cf_foreign_h<N>`, `cf_miss_h<N>`, `cf_healed_h<N>`, `cf_calls`,
  `cf_nohead`, `cf_shown_stale`, `cf_refused`, `cf_same`, `cf_disabled`
  (triage `copy forward`, `swap/flicker`).
- The next run must show `copy forward ... from the buffer it shows` with
  `cf_shown_h<N>` growing on a head whose swaps take turns, `cf_miss_h<N>`
  no larger than `cf_healed_h<N>`, `cf_foreign_h<N>` / `cf_nohead` /
  `cf_shown_stale` at 0, no `flicker` FAIL (triage `flicker` /
  `swap/flicker`), and no flicker on the screens.


### Card run 2026-10-17 (step agdc + probes, build 98b1abe)

- About the 10th run where **windows and menus flicker on both screens**:
  a window shows for a moment, then the wallpaper where it was; the
  desktop is unusable. Copy forward works (head 1: 3080 from the shown
  buffer, 0 missed), the A B A B test sees 1 % (head 1) / 15 % (head 0):
  the old alternation is not what the user sees, and that test hashes a
  buffer only at its swap. Head 0 skipped 320 of 1030 swaps for an empty
  back. WindowServer crashed at 11:37 after a mode switch (the plugin's
  fence wait gave up after 5000 ms, GPU hang): a separate bug (N27/N28).
- Code finding: `rd3AccelFlipTo` -> `RDNA3Framebuffer::flipToLocked`
  programs the HUBP address and returns; the flip latches at the next
  vblank, up to a frame later. `submitSwapCopy` returned at once, so
  WindowServer got the old buffer back (copy forward into it, the next
  frame paged on) while the panel still read it - a half-drawn frame,
  the wallpaper first. amdgpu sends the flip event from
  `dm_pflip_high_irq` once the flip latched and userspace reuses the old
  buffer only then.
- Fix (diagnostic build, `common/rdna3_flip_latch.h`, `tests/test_flip_latch.c`
  reproduces the writes into the scanned buffer without the wait and none
  with it): the swap waits once, after its own flip, until it latched (1 ms
  sleeps, up to two OTG frames or 2.5 of the head's frames clamped to
  35-100 ms; `debug.rdna3accel_swap_latch` / boot-arg
  `rdna3swaplatch`, default 1, 0 = the old behaviour for an A/B run).
  Counted per head: the previous flip pending at a swap's entry and the
  flip-to-latch times, copy forwards into the buffer on the panel, the
  shown buffer's band hash at its swap against the head's next swap
  (torn, or rewritten in place with one back buffer;
  `debug.rdna3accel_swap_torn`), buffers freed while scanned, empty backs
  read again 4 ms after a skipped swap. Triage: `flip latch`.
- H2 (the flip before the GPU finished the frame): no mechanism in the
  default configuration. WindowServer's display framebuffers stay out of
  A3; its Metal composite goes through the plugin's private image,
  `nvmtl_vk_submit_wait` (the GPU job waited for), then surface-out reads
  it back into the IOSurface's system memory; the family pages that into
  the legacy buffer through the aperture (CPU) and copy forward copies by
  SDMA synchronously, both before the swap. No GPU-wait sysctl.
- The next run must show `flip latch` with `0 of M swaps: the previous
  flip had not latched` (swap_latch 1), no torn FAIL, and the user's
  verdict on the flicker. If it stays: the torn / in-place counts say
  which buffer is written on the panel; a run with
  `sysctl debug.rdna3accel_swap_latch=0` (or `rdna3swaplatch=0`) gives the
  comparison.

### Card run 2026-10-17b (step agdc + probes, build ad55679)

- **Both monitors mostly black, content now and then, everything
  flickers; the side monitor is fine for a while, then breaks too.** The
  flip latch works (0 flips pending at a swap's entry, 0 timeouts): it was
  not the cause.
- Evidence: head 0's legacy swaps name the head's framebuffer resource as
  a1 (the front, 0x87040000) and two backs as a2 in turns: 0x8b100000
  (pixels) and 0x8b8f0000 (64 samples all zero at every swap, also 4 ms
  later). The family's aperture access - `addToAperture`, its page-on, the
  only CPU path into a legacy buffer - was logged for the first back only,
  never for the second (four log lines per resource allowed). 85 of 229
  shown frames on head 0 were rewritten in place (WindowServer drew into
  the buffer on the panel), 14 + 10 buffers were freed while scanned. At
  the mode switch back to 1920x1080 the core refused VRAM (`allocPhysical
  ... REFUSED (0xe00002db)`, the CPU-visible heap) and CoreDisplay logged
  `copy forward failed (0xe00002bc) ... disabling` - the driver's
  `submitCopyForward` answers only success (or 0xe00002c7 with
  `rdna3accelcf=0`): the family failed before calling it.
- Code findings: `IOAccelLegacySurface::submitSwapCopy(event, front, back)`
  asks for a COPY of the back into the front (Apple's front is the
  scanout) and hands the back to WindowServer when it returns; the driver
  flipped to the back instead (H-COPY: the in-place rewrites, copy
  forward's own "shown buffer" rules, buffers freed under the panel). The
  front resource is not the scanout on this card: the family maps it into
  the GPU page table, which the scanout vidmem cannot be, frees it and
  gives it ordinary VRAM. Nothing in the driver writes a legacy back
  (copy forward targeted only the first back; no GPU engine renders into
  a legacy buffer), so the second back's emptiness is WindowServer's /
  the family's (H-B2: certain that no driver path fills it; why
  WindowServer names it needs the card - the trace below answers it).
- Fix: `common/rdna3_swap_copy.h` (`tests/test_swap_copy.c`), swap mode
  1 (default; `debug.rdna3accel_swap_mode`, boot-arg `rdna3swapmode`): the
  back is copied by SDMA into the scanout buffer the panel does NOT show
  (the framebuffer's own, "home", or the accelerator's second buffer D),
  the head flips to it (latched at vblank: tear-free), the back is copied
  into the front too (copy forward's source, as Apple's family expects);
  before the next copy the swap waits until that flip latched (bounded by
  the latch cap). An empty back is not copied over a picture
  (`swaps_copyempty_h<N>`, the first ones logged with where WindowServer's
  frame did land); mode 2 copies everything, mode 0 is the flip.
- The per-frame trace (`common/rdna3_trace.h`, `tools/rdna3_trace.py`):
  every swap, copy forward, shape, VRAM allocation / free, aperture access,
  page-off, IOSurface answer, mode change, flip and what each HUBP
  scanned, in a ring of 32768 events (2.5 MiB) plus a per-second table;
  the core kext keeps a second ring of RADV's submissions, fences, waits,
  resets and GPU faults. `tools/macos/collect.sh` puts them in the archive
  (`36-frame-trace.bin`, `36-frame-trace-sec.bin`, `36-drm-trace.bin`,
  `36-frame-trace.txt`); autoreport.sh streams both rings every 3 s
  through the whole run (`<report>-trace-stream.bin`, `-drmtrace-stream.bin`,
  `-trace-sec.bin`; snapshots go up after each A/B phase). The kernel log
  gets the first 300 events of a boot, 600 more at the A/B's end and 600
  at the end of the tests (`debug.rdna3accel_trace_dump`; "tr" lines,
  triage decodes them when the files are missing).
- The A/B (autoreport.sh, every accelerator run): 60 s in mode 1, a look
  (`look ab1`) and the counters, 60 s in mode 0 (`look ab0`), back.
  Triage prints both side by side (`frame trace: A/B mode N ...`) with a
  verdict.

#### Coverage: every open question -> where the next report answers it

| Q | Question | Trace event / field / counter | What the values mean |
|---|---|---|---|
| Q1 | each swap: a1 / a2, their index in the surface's buffer array (0x11C0), VRAM, 8-band signature, content; what the driver did | `SWAP`: `pa`/`pb` a1/a2, `v4` low bytes the array indexes (0xff none), `ba`/`bb` the band bytes, `v1` bits 0-7 the content (4 empty, 5 pixels), `arg` (flip / skip / copy / hold / failed), `v1` bits 16-19 the target (home / D / home directly), `pc` what the panel shows after it, `v2` copy us, `v3` wait us; counters `swaps_copy*_h<N>` | an `arg` of hold with an empty back is mode 1 keeping the picture; `copy FAILED` names the step in `v4 >> 16` (1 source, 2 scanout, 3 SDMA, 4 flip) |
| Q2 | where WindowServer's frame landed: every buffer of the surface at each swap vs the previous swap | `SCAN` per other buffer (`arg` its index), flag 2 = changed since this head last sampled it, `ba` now / `bb` before; the SWAP's own a1/a2 bands; kernel log `swap #n: head N: ... where WindowServer's frame is` | the buffer whose bands change is where the frame went; a back with 0 changes and empty content is H-B2 |
| Q3 | when the "empty" back is written, if ever | `SCAN` at every swap, after each copy forward (flag 32), at shapes, `VMFREE`'s `ba`; delayed re-sample: `SCAN` flag 16 with `v1` the microseconds after the swap | flag 16 + pixels: the frame lands late; never pixels: never written |
| Q4 | every copy forward | `CF`: `pa` the front named, `pb` the target, `pc` the source used, `v0`/`v1` rects / copied, `ba` the bounding box (x0 y0 x1 y1), `arg` the decision, `v4` the IOReturn answered | any `v4` != 0 is CRITICAL (`CF-ERROR`); CoreDisplay's `copy forward failed` without a CF error means the family failed before the driver |
| Q5 | every vidmem alloc / free, scanned at the free?; shapes | `VMALLOC` (`arg` 0 = refused, `v4` the IOReturn, `v1` MiB handed out), `VMFREE` (flags 2/4/8: shown by mode 0 / leaving the panel / mode 1's D on the panel, `head`), `SHAPE` (`v0` mask, `v1` w/h, `v3` present / shaped, `pa`-`pc` buffers 0-2) | a `VMFREE` with flags 2-8 is H-FREE |
| Q6 | what each panel really scanned | `PANEL` at each swap: `pa` SURFACE_EARLIEST_INUSE, `pb` the requested address, `pc` what the driver expects, flags 1 pending; `SCAN` arg 0xfe the scanout buffer after the copy, 0xfd the front | `pa` != `pc` with no flip pending: the panel is not where the driver thinks; triage `PANEL` WARN |
| Q7 | mode switches and display mode changes | `MODESET` (`arg` 0 swap mode, 1 A/B phase: `v0` old, `v1` new), `MODE` (`arg` 0/1 will/did change, 2 initFramebufferResource with the scanout) | the A/B table segments by these |
| Q8 | nothing dropped in the run | stream headers' `lost` (events overwritten between two reads, accelerator and core), the full read's `lost`, `nomem`; each stream file's reads must follow one another (`written` k - `written` k-1 = `count` + `lost`), its size must match its `.size` file; the per-second table (`36-frame-trace-sec.bin`: the last 4096 s, counts stop at 65535); counters `swap_trace_events / _lost / _nomem`; a kernel log dump must hold its `dump done, N line(s)` | any `lost` in the streams, a read missing from a file, a truncated / corrupt blob, an unknown version, `nomem` > 0, a size mismatch, blobs of two boots (`salt_check`): FAIL `trace incomplete: Q8 (...)` naming the file and byte |
| Q9 | RADV's submissions, completions, waits, resets, faults | core ring: `CS` (pid, VMID, IP / ring, seq, IBs / BOs, result), `FENCE` (first seen signalled), `GPUWAIT` (pid, us, seq, cap ran out, result), `RESET`, `VMFAULT` (VMID, address, client, pid) | a plugin VkResult / timeout lines up with these by time |
| Q10 | every IOSurface the kext sees, and its VRAM | `SURFGEM` (`arg` 0 refused, 1 imported, 2 dirty, 3 RM share; `v0` id, `v1` status, `v2` reason / plane, `pa` VRAM), `SURFRES` (resource, id, how: 1 aperture, 2 page-off, 4 import, 5 dirty; `pa` its VRAM) | joins the plugin's IOSurface ids to the legacy buffers' addresses |
| Q11 | one clock | every `t` is mach_absolute_time; each header (version 2, 96 bytes) carries `tb_numer` / `tb_denom`, the read's `now` and its wall clock `wall_us` (us since 1970, taken next to `now`) | ns = t * numer / denom (the plugin's traces use the same clock); wall = wall_us - (now_ns - t_ns) / 1000; triage prints the anchor; a version 2 header without it: FAIL Q11 |

Triage's self-check prints `FAIL trace incomplete: Qn (...)` for any
question the report cannot answer (no trace at all on a kernel that has
it, no HUBP address from a core without `rdna3DisplayScanAddr`, events
lost in the A/B streams, no core trace when RADV ran).

#### ECC review round 1 of the swap copy and the trace

What could make the next report come back empty or wrong, and what says so:

| Seen | Counter / log / triage line | Meaning, action |
|---|---|---|
| `swap copy: SDMA BREAKER OPEN at swap #n` (log), `swaps_copybrktrips`, `swaps_copybrkflip_h<N>`, SWAP `v1` bit 20, MODESET arg 2 | three SDMA copies failed in a row, or one timed out (~100 ms each) | modes 1/2 flip as in mode 0 until one swap 600 swaps later tries SDMA again (`... breaker CLOSED`); the A/B's phase 1 then WARNs; the SDMA ring is the fault (see the core's `copy:` lines) |
| `swap copy: head N: 3 flips in a row did not latch in time -- the head's copies STOP WAITING`, `swaps_copynowait_h<N>`, LATCH arg 6/7/8 | rdna3_flip_latch.h's backoff in modes 1/2 | a head that never latches costs 3 capped waits, then only a look per swap; a latch seen turns the waits on again |
| `swaps_copyfreewait_h<N>`, `the old second scanout buffer ... is on the panel / maybe still scanned` | D is freed only off the panel once the flip away latched | a mode change waits for the latch, the swap copies into home meanwhile |
| `swap mode: head N: leaving the copy modes -- the last frame copied into home ... flipped there` | `debug.rdna3accel_swap_mode=0` while the panel showed D | mode 0 starts from home; a failure keeps D on the panel until mode 0's first flip (logged) |
| `swap mode a -> b ... REFUSED: the family's lock could not be taken` | the mode never changes without the lock | the A/B's `ab_set` line says `NOT APPLIED`; triage `A/B inconclusive: mode 0 never applied in phase 2` |
| `trace: head N: what its HUBP scans is unknown ...: <reason> (0x...)`, PANEL `v3` / `v4` | Q6 names the reason (no core, unknown head, an older core, the call failed) | |
| APERTURE `v2` 1-4, CS `arg` 0xff | every getApertureMemoryDescriptor return (gate, cached, no VRAM, withRange) and every refused AMDGPU_CS (`drm: AMDGPU_CS from pid p refused (e)`) | Q3 / Q9 |
| `== frame trace: <oid> NOT BINARY` (tests file) | `sysctl -b` gave text: the stream files cannot be decoded | triage `trace incomplete: Q8 (... NOT a trace ...)` |
| `FAIL frame trace: A/B did not run` / `A/B inconclusive: phase X had n swaps on head h (need 20)` / `no MODESET to phase X` / `mode 0 never applied in phase 2` | the A/B is judged only from phases 1-2 events, with at least 20 swaps per head and phase | an inconclusive A/B prints its verdict as `INFO A/B verdict (NOT valid ...)`; `== swap A/B: activity: Activity Monitor is NOT running` explains a phase without swaps |
| `trace incomplete: Q1 (... FEWER than the counters: head N: a in the trace, b counted over the A/B)` | the trace lost swaps the counters saw (`swaps_h<N>` before phase 1 and after mode 0) | |
| `trace incomplete: Q1-Q11 (debug.rdna3accel_trace NOT read by collect.sh ...)` | `36-frame-trace.txt` says a root-only read failed | run collect.sh as root |
| `plugin trace: ...` | tools/plugin_trace.py's verdicts (the Metal plugin's own trace, collected by tools/macos/collect_plugin_trace.sh into the archive) | INFO `not in this report` when the plugin loaded but its trace is missing |

The kernel stream is pumped every 3 s through the whole run now (not only
the A/B), into `$HOME/rdna3-trace/` on the machine, the archive
(`36-stream-*.bin` and `.size`), the host (`<report>-trace-stream.bin`,
`-drmtrace-stream.bin`, `-trace-sec.bin` and their `.size`) and
`/var/db/rdna3-pending-*` until sent. Reads copy 256 events at a time with
the ring's lock dropped between; the stream's cursor moves only after the
copyout. Content samples are read through the BAR with no HDP invalidate,
as amdgpu does on Navi 32 (`amdgpu_device_invalidate_hdp` is a no-op on
HDP 6.0); the headers say so (flag `HDP_NOINV`). The other buffers of a
swapping surface are sampled at every swap for a head's first 600 swaps,
during the A/B and at empty backs, else every 8th
(`debug.rdna3accel_trace_scan`: 0 only empty backs, 2 always).

#### ECC review round 2: one run must answer "why black / flicker"

| Seen | Where | Meaning, action |
|---|---|---|
| `screens <look> ... pipe N P% not black, BLACK pipe(s) N` (FAIL), `A/B screens FAIL mode 1 left pipe(s) N BLACK (pipe N: mode 1 a%, mode 0 b%)` | the rdna3_drminfo looks of the run and of the A/B (`look ab1` / `look ab0`), at most 3 % not black = black | the panel itself, not a counter: start here |
| `frame trace FAIL COPY-BLACK: n copy(ies) of a back WITH pixels left the scanout buffer / front without any` | SCAN 0xfe / 0xfd after a mode 1 copy | the copy lost the picture (SDMA, a wrong source / pitch); compare with the core's `copy:` lines |
| `plugin trace INFO join: head N back 0x... = IOSurface X (...): plugin DRAWN a, CLEAR-ONLY b, published p (dirty d, copy-out c, k EMPTY)` / `plugin NEVER saw this IOSurface` / `WARN join: ... has NO IOSurface id` | the kernel's SURFRES (how 3: a1 / a2 at every new address, id 0 too) and SCAN (`v3` the id, `v4` the resource's type byte) joined with WindowServer's plugin trace | which buffer the panel got, and what the plugin did to it: a back the plugin only cleared or never published is black on the panel whatever the swap does |
| `plugin trace WindowServer[pid] (opened ... UTC): main ring WRAPPED (...): only a .. b UTC kept - the A/B (from ...) is NOT covered` | the plugin's ring wrapped before collection | autoreport writes `NVMTL_TRACE_RING=262144` into `/Library/GPUBundles/rdna3mtl/rdna3mtl.conf` on every VM boot, the install boot (`rdna3vminstall=1`, also through sync_metal) included, so the card boot's WindowServer starts with it; WindowServer reads it only when it loads the plugin, so a conf written during a run counts from the next boot (the INFO line prints the ring size each trace had). Disk: about 51 MB preallocated per process that loads the plugin; autoreport deletes the traces of earlier boots at each run's start, and a standalone run takes the knob out again after its report (so a standalone run keeps the 16 K ring); snapshots after each A/B phase (`<report>-plugin-ab1-WindowServer-<pid>.trace`, `-ab2-`, `-ab-end-`, `-end-`) keep the A/B even when it wraps later |
| `plugin trace: ... OLD - opened <date> UTC, before this boot` | a trace of an earlier boot left in `/var/tmp/rdna3-nvmtl` | not judged; collect_plugin_trace.sh REJECTs them (kern.boottime against the mtime and the header's open time) |
| `WARN plugin trace MISSING (<the collector's reason>) although WindowServer loaded the plugin`, `plugin trace collector: REJECT ...` | M5 / CoreDisplay say WindowServer loaded the plugin, the report has no trace of it | the reason is the collector's (`31-plugin-trace/info.txt`, `37-plugin-trace-collect.txt`) |
| `CRITICAL` anywhere | triage, `rdna3vm result` | a FAIL in the checklist (it was INFO before) |
| `frame trace WARN GPU: n GPU wait(s) ran out the 10 s cap` | GPUWAIT `v3` 1 | a hang; the plugin's fence timeouts line up with it |
| `Q6 ... no VRAM base yet (MC addresses)` / `below the framebuffer aperture` | PANEL `v3` 4 / 5, flag 2 | MC addresses are never compared with the BAR addresses (no false `PANEL` mismatch) |
| `Q8 ...; the boot before the pump: N events overwritten before its first read` | the stream files' first blob is the boot (kept now) | not a loss of the run |
| `== swap A/B: console user ...`, `power: ...`, `NOBODY LOGGED IN`, `activity: ...`, `(exit n)` | the A/B's preconditions in the tests file; the SUMMARY row `swap A/B` sums them up: FAIL for a failed A/B (its exit), a setting NOT APPLIED / FAILED, nobody logged in; WARN for no activity, pump read errors, failed uploads, plugin snapshots without a trace or with rejects, rdna3mtl.conf not written, fix_tracedir.sh failed | a head that had no swap in the A/B is listed as not judged |
| `== frame trace: pump read errors`, `... reached 536870912 bytes` | the pump's failed reads and its 512 MB cap per file | |

Round 3 (end-to-end, kernel, security): the plugin trace's copies of one
process (the A/B snapshots, the archive's final copy) are merged by
(ring, seq), so the join keeps what an earlier snapshot saw and a wrap the
snapshots cover is no WARN (`main ring N records (k copies merged)`); a lit
pipe whose picture is not readable is `NOT READABLE (not judged)`, never a
PASS, `A/B screens WARN NOT JUDGED` / `look ab1 missing although the A/B
ran`, and `A/B main display <verdict> mode 1 vs mode 0 on the main display
(pipe P, how it was found): a% vs b% not black`; join lines carry the A/B
phase split and the EMPTY swaps, and a back the plugin never saw although
its trace covers the A/B is a WARN when it had empty swaps; backs to join
without any WindowServer plugin trace are `plugin trace MISSING (<reason>) -
the join lines below carry no plugin verdict`; the SUMMARY row `swap A/B`
(see the table above); both "before this boot" checks allow 120 s (a clock
set after the boot); the collector decides from the directory entry before
it opens anything and reads the header from its own copy; autoreport makes
the standalone folder with mkdir (no mv, no install -d through a link; a
fresh mktemp directory otherwise), deletes plugin traces of earlier boots at
each run's start, keeps every snapshot's pending copy; a posted panel move
is timed before its flip is issued.

Kernel side of round 2: a panel move posted from outside the family's lock
is ignored when it is older than the swaps' own last flip (it would undo
that flip's record); D is freed only after a latch was SEEN (a wait that
gave up does not count); leaving modes 1/2 waits for the flip to D before
copying D home, and that copy feeds the SDMA breaker; the breaker retries
after 10 s of the clock (not 600 swaps: a quiet desktop swaps rarely);
object names are SipHash-2-4 under a per-boot key (the round 1 salt could be
inverted), the headers carry a random boot id instead of the salt check;
refused AMDGPU_CS are traced 64 times per error, then every 64th (v1 the
count, v2 those skipped); the aperture's cached path 4 times per resource,
then every 256th; shapes sample their buffers for the first 256 shapes.

#### Symptom -> trace -> action

| Seen | Trace / triage line | Action |
|---|---|---|
| black frames in mode 1 | A/B: `BLACK frames` > 0 in mode 1, `swaps_copyblack_h<N>` | mode 1 only copies an empty back with no picture to lose: look at Q2 for where the frame went; `debug.rdna3accel_swap_mode=0` to compare |
| `H-STALE` FAIL (2026-10-17c) | a mode 1/2 copy of a back with no copy forward into it since it was shown, unchanged, < 20 ms after another back's copy; `A-B-A` on the scanout samples | mode 1 holds such a back (`swaps_stalehold_h<N>`, `debug.rdna3accel_swap_stale`); see "The stale back" under card run 2026-10-17c |
| `H-B2` FAIL | a back named by swaps never receives a frame (no aperture write, empty at every swap) and a frame was held back or black shown at its swaps (since card run 2026-10-17c: WARN `no frame lost` / INFO `handled`, see that run) | modes 1/2 present the landed buffer; find with SURFRES / SURFGEM which IOSurface WindowServer draws (H-B2 is the family's / WindowServer's buffer rotation, not a driver write) |
| `H-COPY` FAIL in mode 0, none in mode 1 | in-place rewrites only while flipping | the copy is right: keep mode 1 |
| `H-FREE` | VMFREE flags 2-8 | in mode 1 the panel never scans WindowServer's buffers; in mode 0 expected |
| `CF-ERROR` CRITICAL | a CF with `v4` != 0 | only `rdna3accelcf=0` answers an error: a bug if it shows otherwise |
| CoreDisplay `copy forward failed` CRITICAL | no CF error in the trace, `VRAM` WARN (allocations refused) | the family failed on a buffer without VRAM: the CPU-visible heap ran out (BAR size); see "Card run 2026-10-17c" below, N30; the triage's `VISIBLE VRAM` line says who held it |
| `PANEL` WARN | HUBP address != expected | the framebuffer or an IOP flip moved the panel: see the FLIP events around it |
| tearing in mode 1 | `swaps_copydirect_h<N>` (no second buffer) or `swaps_copylatchto_h<N>` | D could not be allocated (VRAM) or flips did not latch in time |

### Card run 2026-10-17c (step agdc + probes, build 1311b22)

- Seen: head 0's swaps name the framebuffer resource as the front and two
  backs in turns: 0x8b100000 (IOSurface 37, resource type 0xc0) and
  0x8b8f0000 (type 0x40, no IOSurface). The second read EMPTY at all 159
  swaps that named it (16 aperture writes into it, 272 into the first);
  WindowServer's frame was in 0x8b100000 at 159 of 159 swaps. Mode 1 held
  the panel at each of them (`swaps_copyempty_h0` 437).
- The trace says when (certain from the run's stream): each swap of
  0x8b8f0000 came 0.2-0.3 ms (median 0.30) after the swap of 0x8b100000,
  which mode 1 had just copied; at none of them had 0x8b100000 (or any
  other buffer) changed again (2 changes of it between two swaps in the
  whole run, both copy forward's writes). So the empty back's swap is
  WindowServer's second swap of a frame the swap before presented: the
  hold lost no frame. Mode 0 skipped those swaps the same way.
- Change (evidence-driven, no amdgpu analogue: macOS legacy surface
  semantics; `common/rdna3_swap_copy.h` `rd3_sc_landed_wanted` /
  `rd3_sc_landed_pick`, `tests/test_swap_copy.c`): in modes 1/2, at a
  swap whose back reads empty - or has no IOSurface (type 0x40) and did
  not change since the head last sampled it - the surface's other
  buffers (its 0x11C0 array minus the back and the head's framebuffer
  resource, plus a1 when it is neither) are sampled at that swap (at most
  6, 64 BAR reads each, the same reads the trace's SCAN events use), and
  the one that changed since the head's last look, has pixels and is not
  the picture on the panel is copied instead (an IOSurface-backed one
  first). Without one the old rule stands (hold over a picture). In the
  pattern of this run it does not fire (nothing new at those swaps); it
  presents a frame that really sits in a buffer the swap did not name.
  The risk: a buffer WindowServer is drawing the next frame into also
  "changed" - a half-drawn frame would show. So it is OFF by default
  (`debug.rdna3accel_swap_landed` / boot-arg `rdna3swaplanded`, default
  0): the swap keeps the old rule and only counts, traces and logs what it
  would have presented (`swaps_copylanded_would_h<N>`, SWAP `v1` bit 22,
  `would present landed buffer ...`). A next report with that counter at 0
  says the rule does not matter; above 0, try `rdna3swaplanded=1` and
  watch `swaps_copylanded_h<N>` against the looks.
- The plugin's trace was missing because the VM's plugin was stale (built
  from metal/ at 8540be5, before the plugin's frame trace,
  `metal/plugin/NVMTLTrace.m`, came with e168385): rebuild it in the VM
  (`tools/vm/macos/build_metal.sh`) before the next run.

| Seen | Trace / counter / triage line | Meaning, action |
|---|---|---|
| `swap #n: head N: the back (phys 0x..., a2, resource type 0x40) read empty -- presented the landed buffer 0x... instead (buf[i], resource type 0xc0, IOSurface s: ...)` (log, first 8 then every 1024th) | `swaps_copylanded_h<N>`; SWAP `v1` bit 21, `v4 >> 24` the buffer's index (0xfe: a1), the decoder prints `presented the landed buffer 0x...` | modes 1/2 copied the buffer WindowServer's frame landed in, not the empty / stale back |
| `swap #n: head N: would present landed buffer 0x... (buf[i], ...) instead of the back (...) -- debug.rdna3accel_swap_landed=0, so held / the back copied` (log) | `swaps_copylanded_would_h<N>`, SWAP `v1` bit 22 (`v4 >> 24` the index), decoder `would present (swap_landed 0) the landed buffer 0x...`; triage `WARN swap copy: head N: the landed buffer WOULD have been presented N times`, H-B2 `found but NOT presented at N` | with the default 0 a frame sat in another buffer and the swap did not show it: set `rdna3swaplanded=1` for the next run and compare the looks |
| `frame trace INFO H-B2: ... - handled: landed buffer presented N times` / `swap copy INFO head N: H-B2 handled: landed buffer presented N times` | the swaps naming the empty back presented the landed buffer | nothing to do; compare the looks (no half-drawn frames) |
| `frame trace WARN H-B2: ... - but no frame lost: all N of its swaps were held/skipped and no other buffer had a new frame at any of them (median x ms after the head's swap of another back)` | SCAN of the other buffers at each held swap: none CHANGED with pixels | WindowServer's second swap of an already presented frame: the hold is right, H-B2 is not the cause of a black or flickering screen |
| `frame trace FAIL H-B2: ...; n held/skipped, k of them with a new frame in another buffer, b shown black` | a held swap while another buffer had a new frame (a frame waited for the next swap), or the empty back shown | a kernel before the landed buffer, or the landed rule missed it (its SCAN not taken: `held swaps without samples`) |
| A/B `shown (k landed), held/skipped (n with a new frame elsewhere)`, verdict `mode 1: h held (n with a new frame in another buffer: frames held back), k landed buffers presented` | the A/B per mode | n > 0 in mode 1: frames held back |
| `plugin trace MISSING although WindowServer loaded the plugin: the VM's plugin is STALE: PLUGIN built from metal at X, ... - a plugin without the frame trace (metal/plugin/NVMTLTrace.m came with Y after it)` | 35-build-stamps.txt against 00-host.txt (the same check as `STALE PLUGIN/RADV`) | rebuild the plugin in the VM; the collector's generic reasons are printed only when the plugin is current |

#### The stale back: the side monitor flickers, windows appear and disappear (2026-10-17c)

- Seen (certain from the run's stream, `tools/rdna3_trace.py`): head 1's
  swaps come in pairs per frame - a copy forward into B1, the swap of B1
  (the frame), then 5-14 ms later the swap of B2, which WindowServer did not
  draw this frame. B2 (0x8d0c0000, later 0x8d8b0000) received pixels once,
  from a window animation that rotated three buffers, and never again. Mode
  1 copied it at every frame: the panel showed the old frame for most of
  each frame period, and the copy into the front spread it into the next
  frame through copy forward. 259 such copies on head 1 by the decoder's
  count (130 of 0x8d0c0000, 102 of 0x8d8b0000, the rest the same pattern
  on other buffer sets), none on head 0; 14 A-B-A patterns on head 1's
  scanout samples, 0 on head 0. No GPU work of WindowServer was pending at
  any of those swaps (not a render race); SYNC's dirty bits are 0.
- Change (evidence-driven, no amdgpu analogue: macOS legacy surface
  semantics; `common/rdna3_swap_copy.h` `rd3_sc_stale_check` /
  `rd3_sc_stale_confirm`, `tests/test_swap_copy.c` `stale_*`): mode 1
  remembers per head and per buffer address the hashes of two grids of 64
  samples of the back when the head last presented it (grid 0 the trace's,
  grid 1 shifted by half a cell) and whether a copy forward targeted it
  since (marked in every head's table). A back with pixels is held - no
  copy to the panel, none into the front, the panel keeps the newer frame
  - when its grid 0 is unchanged, no copy forward targeted it, another
  buffer was presented after it, the panel holds a frame mode 1 copied,
  and grid 1, read again then, is unchanged too (else: copied, counted as a
  miss). Never presented: copied. Mode 2 stays the copy-everything control
  (never holds), mode 0 is unchanged. `debug.rdna3accel_swap_stale` /
  boot-arg `rdna3swapstale` (default 1) set to 0 copies such a back as
  before and only counts and traces it. The records are forgotten when the
  surface or the head's framebuffer resource goes, at a display or swap
  mode change, when D is reallocated, and per address when the VRAM is
  freed. A held back is read again ~4 ms later (the empty back's
  re-sample budget): a change there is a frame that landed after the hold.
- The decoder's bands are RGB only, the kernel's hashes include alpha: an
  alpha-only change reads "unchanged" to the decoder (it may count a stale
  copy the kernel rightly made).

| Seen | Trace / counter / triage line | Meaning, action |
|---|---|---|
| side monitor flickers, windows appear and disappear (mode 1) | `frame trace FAIL H-STALE: head N: c mode 1/2 copies decoded, s stale copied, h held, a A-B-A on the scanout samples - stale backs COPIED (0x... nx, ...; first swap #m, shown before at #p): ...; <why>` | stale backs reached the panel. `<why>` says which kext: `no stale hold in the trace: a kext before the stale-back hold` (update it), `the kext has the hold` or, with the counters, `kernel: ... not held: norecord, changed, cf, sameshown, nopanel, nohead; evicted, second-grid misses, ...` (the reason the kernel copied them) |
| `frame trace WARN A-B-A: head N: a A-B-A patterns on the scanout samples ... its PASS does not cover this` | SCAN 0xfe after the copies: a picture back after one other | the flicker the panel showed; the kernel's `flicker` test hashes the backs and can PASS while this happens |
| `swap #n: head N: the back 0x... (a2, buf[i]) is unchanged since it was shown at swap #m and 0x... was shown after it, with no copy forward into it -- held, the panel keeps the newer frame (k such)` (log, first 8 then every 1024th) | `swaps_stalehold_h<N>`; SWAP arg 5 with `v1` bit 23, `v3` = m; decoder `held: stale back (shown at #m)`; triage `INFO swap copy: head N: H-STALE handled: k stale backs held`, `frame trace INFO H-STALE: head N: ... 0 stale copied, k held` | the fix working: nothing to do; compare the looks |
| `swap #n: head N: would hold the stale back 0x... -- debug.rdna3accel_swap_stale=0, so copied` | `swaps_stalehold_would_h<N>`, SWAP `v1` bit 24, decoder `stale: would hold (swap_stale 0)`; `WARN H-STALE ... copied on purpose` | the switch is off: set `rdna3swapstale=1` (the default) unless this run is the A/B of it |
| `swap #n: head N: the back 0x... matched its first-grid samples of swap #m but not its second grid ... -- copied, not held` | `swaps_stalehold_miss_h<N>` | a small redraw the first grid missed: the second grid saved it; many of them mean the redraws are small - the rule still works |
| `swap #n: head N: the stale back 0x... held at this swap ... CHANGED x us later -- a frame landed after the hold` | `swaps_stalehold_changed_h<N>`; SCAN flags 4 and 16 of the back; `WARN swap copy: head N: stale back ...; k held backs CHANGED in the 4 ms re-sample` | WindowServer drew the held back after its swap: that frame waits for the back's next swap. Many: the hold is too eager - open item N31 |
| `swap #n: head N: the back 0x... changed since it was shown at swap #m although no copy forward targeted it -- copied` | `swaps_stalenocf_h<N>` | WindowServer redraws a back without copy forward: the rule's assumption does not always hold. It is copied (safe); a redraw that both grids miss would be held - N31 (needs the card) |
| `frame trace FAIL H-STALE: head N: held a REAL frame at k stale hold(s) (first swap #n: a new frame in 0x... / aperture write(s) into the back since swap #m)` | a stale hold while another buffer had a new frame, or after the CPU wrote the back | the hold hid a frame: compare `swaps_stalehold_miss_h<N>` / `_changed_h<N>`; set `rdna3swapstale=0` for the next run if the looks show missing updates |
| `WARN H-STALE: head N: k of its n swaps were stale holds (half or more): the panel may look frozen` | holds against the head's swaps | compare the looks; WindowServer's two-swap pattern alone gives just under half |
| `WARN H-STALE: head N: the kernel counted k stale holds, the trace holds h (l events lost)` | `swaps_stalehold_h<N>` against SWAP `v1` bit 23 | the stream did not cover the counters' window (it started late or lost reads), or a hold was not traced |
| `WARN H-STALE: head N: c mode 1/2 swaps but no copy decoded` / `WARN frame trace: H-STALE not judged: no trace in the report` | no copies in the trace / no trace at all | the stale back is not judged: get the stream (`tools/vm/macos/autoreport.sh`) |
| `WARN swap copy: head N: counter missing: swaps_stalehold_h<N>, swaps_copylanded_h<N> (kext older than 2026-10-17c)`, `counter missing: swap_landed`, `counter missing: swaps_copy_h<N> although swaps_stalehold_h<N> is k` | the counters the report has | an older kext (`?` printed, never zeros), or a partial dump |
| `swap copy: head N: the stale-back records forgotten (why; k records)`, `the stale-back table is full (8 buffers): 0x...'s record evicted`, `the back at 0x... was freed -- its stale-back record is forgotten` | `swaps_stalereset_h<N>`, `swaps_staleevict_h<N>`, `swaps_stalegone_h<N>` | the records' life: evictions mean more than 8 buffers per head (each evicted one is copied once) |

### Card run 2026-10-17c (the CPU-visible VRAM ran out; step agdc + probes)

- Evidence (`serial-*.log`): the VM's BAR0 is 256 MiB (`rdna3vm` shrinks it,
  `OVMF 64-bit PCI window: 0 MiB (BAR0 256 MiB)`), so the CPU-visible heap
  is 144 MiB (`DRM/amdgpu UAPI ready: ... 144 MiB visible`). RADV's BOs the
  CPU may map (36992 KiB and 4096 KiB each) filled it (`GEM: ... did not
  fit (visible heap 9568 KiB free ...): GTT`); then `RD3VidMemory::
  allocPhysical: 2658304 / 3145728 / 8294400 bytes REFUSED by the
  framebuffer (0xe00002db)` for WindowServer's framebuffers: the main
  display went black and froze (gamma "pipe 1 2 % not black", M6 FROZEN).
- Code findings (certain from code): (1) the accelerator's allocations
  (`rdna3AllocVram`: WindowServer's framebuffers, the framebuffer kext's
  surfaces and cursor, the swap copy's D) and GEM BOs shared one first-fit
  visible heap, GEM first come first served; (2) a BO created without CPU
  access went beyond BAR0 first but fell back into the visible heap when
  that refused it (`4 KiB of VRAM without CPU access did not fit (visible
  heap 8872 KiB free, the rest 252472 KiB free)`); (3) `range_alloc_get`
  cut a free range to `[s,a)` before it found its free list (1024 ranges)
  had no slot for the tail: the failed allocation lost the tail for good -
  the 15.4 GiB drop of the "rest" between two log lines is that or a
  15 GiB BO nobody logged; which of the two the next run says (below);
  (4) a 4 KiB BO with a large alignment hint needed a large aligned hole.
- Fixed (`common/rdna3_vram_place.h`, `tests/test_vram_place.c`):
  BOs without CPU access never use the visible heap
  (amdgpu_bo_placement_from_domain); the visible heap has a reserve at its
  start for the accelerator's allocations (default two thirds of it, at most
  128 MiB; `rdna3visreserve=<MiB>`, 0 = none) that GEM BOs never take; the
  alignment never exceeds the allocation's own power of two
  (amdgpu_vram_mgr_new); the free lists hold 4096 ranges, a split with no
  slot changes nothing, a range that cannot be given back is counted.
- Log lines (kernel): `VRAM REFUSED #n: <who>, <size> KiB by <owner>
  (alignment .., placed at ..): free reserve .. visible heap .. beyond BAR0
  .. (largest ..); free ranges a/g/h, lost .. KiB; held: <owner> <KiB> xN
  (peak, refused)` (first 16, then every 64th), `VRAM heap: ...` (at most
  every 30 s while allocations happen), `GEM: a N MiB VRAM BO ... placed at`
  (BOs of 1 GiB and more), `VRAM: could not give N KiB .. back`.
  Counters: `debug.rdna3_vram` (one line of key=value; autoreport adds it to
  every counter dump of the tests file, collect.sh writes
  `38-vram-heap.txt`).

| Seen (triage `VISIBLE VRAM`) | Cause | Action |
|---|---|---|
| `FAIL VISIBLE VRAM exhausted: N refusals (K of the accelerator's own ...)` | the display's allocations did not fit the reserve plus the general heap | read `owners`: who holds the visible heap at the end (`accel` = WindowServer's framebuffers + legacy surfaces, `swap-D`, `fb-*`); more reserve (`rdna3visreserve=<MiB>`) or a bigger BAR0 (`RESIZE_BAR_LOG2=11` in `~/rdna3-vm/config`, docs/TESTING-VM.md, risky) |
| `WARN VISIBLE VRAM exhausted: N refusals (0 of the accelerator ...; M of GEM BOs)` | RADV's BOs the CPU may map filled the general part and went to GTT (expected with 256 MiB); `reserve_blocked` counts those the reserve alone could have held | nothing to fix for the display; if RADV slows down, a bigger BAR0 |
| `WARN .. BO(s) without CPU access found no room beyond BAR0` | the part beyond BAR0 was full or fragmented (see `largest`, the `VRAM REFUSED` line's alignment) | the line says free vs largest: free but not largest = fragmentation (alignment hint, free-list size) |
| `FAIL the heap lost N KiB` | a free list (4096 ranges) was full when a range came back | fragmentation the allocator cannot hold: raise `RANGE_ALLOC_MAX_FREE` or merge ranges |
| `WARN .. skipped a free range only because the free list was full` | the same, no range lost yet | as above |
| `FAIL .. kernel-internal BO(s) (seq64, user queue objects) refused` | no room in the general part or the reserve for a BO the kernel needs (they spill into the reserve; no GTT fallback) | the owners show who holds the heap; raise the reserve or the BAR0 |
| `FAIL .. free(s) of N KiB were refused by the heap` | a BO or accelerator allocation was given back twice or from outside the heap (`free_fail`): the owner still counts it | a refcount bug; the kernel log's `VRAM: could not give` lines name the first ones |
| `WARN .. range(s) retired for good` | a VRAM clear timed out (`VRAM clear .. range retired`): the SDMA fill may still land, so the range is never reused and no owner holds it | SDMA hang: see the ring/reset lines before it |
| `WARN .. client BO(s) found no VRAM and no GTT either` | `gtt_fail`: VRAM refused and GTT was over budget or its allocation failed; the client got an error (`GEM: a N KiB BO found no VRAM and no GTT either` has the GTT usage) | RADV then fails with out of memory: GTT is 8 GiB wired; look for a leak of GTT BOs |
| `WARN log shows N refusals, newest dump says M` | the dump was read before the run ended | the log lines are the later truth |
| `WARN dump truncated` / `visible_kib=0` / `not measured: 38-vram-heap.txt has no .. dump` | the report is incomplete (sysctl buffer, collector) or BAR0 was not mapped | `sysctl debug.rdna3_vram` in the VM; `RDNA3,VRAMApertureMB` |
| `WARN not measured: debug.rdna3_vram is in neither ...` | the kext logs the heap but the collector did not read the sysctl | `collect.sh` ran as another user or too early: `sysctl debug.rdna3_vram` in the VM |
