# Plan: the hardest and most important parts (order of execution)

Starting point: commit `d333e25` on `stage1-kext`. Everything below can be
written and checked without the card (simulator `tests/sim_navi32.c` + CI); the hardware
will confirm it only at the first tests (`docs/STAGE3.md`).

Rules unchanged: port from amdgpu (MIT) with the name of the source function in
a comment, each part with a test in the simulator, NullMoth code only for AMD,
commits with `[skip ci]` and one CI run per batch of changes.

## 1. ✅ MSI interrupts and fences from IH (b1f8132)
* `RDNA3Bringup`: its own `IOWorkLoop`, `IOFilterInterruptEventSource` on
  the MSI index from `IOPCIDevice::getInterruptType`, action = `ih60_process`
  → `ih60_iv_fence` → `IOLockWakeup`.
* Enabling sources: `sdma60_set_trap_interrupt`, `gfx11_set_eop_interrupt`.
* `fenceWait`: `IOLockSleepDeadline` instead of a loop, 5 ms margin for a lost
  interrupt, without holding `fDrmLock` during sleep.
* Gate `rdna3msi=1`; simulator: IV generation model for EOP and TRAP.

## 2. 🟡 Doorbells + KIQ/KCQ done (without MES); MES and more queues later
* NBIO 4.3: doorbell aperture (`nbio_v4_3_enable_doorbell_aperture`,
  SDMA/IH/VCN ranges), BAR2 mapping in the kext, `WDOORBELL64`.
* GFX and SDMA rings through doorbell (like amdgpu), wptr in memory.
* MES 11 (`mes_v11_0.c`, `amdgpu_mes.c`): loading through PSP is already there;
  still to add: MES ring, `SET_HW_RESOURCES`, `ADD_QUEUE`/`REMOVE_QUEUE`,
  MQD for gfx/compute/sdma queues. Alternative for a start: KIQ + MEC
  (`gfx_v11_0_kiq_*`, `compute_mqd_init`, `MAP_QUEUES`), without MES.
* UAPI: `AMDGPU_HW_IP_COMPUTE` in `rdna3_drm.c`, separate rings per file.
* Simulator: KIQ/MES model (MAP_QUEUES packets, MQD → HQD).

## 3. 🟡 Display Core from amdgpu (the largest)
Goal: modesetting, EDID, multiple monitors, hotplug, resolution change.
* ✅ Step 1: DP AUX (DPCD, I2C-over-AUX), HPD sense, EDID + mode parser
  (`hw/dcaux.c`, `hw/edid.c`), connector probe in the kext, `IODisplayEDID`
  through the framebuffer.
* ✅ Step 2: AtomBIOS interpreter (`hw/atom.c`), VBIOS object table
  (connector → UNIPHY, DDC/AUX, HPD, encoder capabilities).
* ✅ Step 3: DMCUB service (`hw/dmub32.c`, `rdna3dmub=1`): reset from STOP_FW,
  windows, inbox1, QUERY_FEATURE_CAPS, VBIOS commands through DMCUB.
* ✅ Step 4a (calculations, no register writes): `hw/dcn32_mode.c` —
  OTG values from the EDID mode (`optc1_program_timing`), DP 8b/10b
  bandwidth, mode filter, GOP mode recognition (`RDNA3,Head0EdidMode`,
  `RDNA3,Connector<n>,ModesUsable`).
  Next (needs the card): DML (watermarks, DLG/TTU, vstartup), pixel clock
  (DCCG DTO), stream encoder MSA, HUBP/DPP sizes, DP training,
  `DIGxEncoderControl`/`SetPixelClock` through DMCUB, modeset.
* A `dm_services` layer for XNU (allocation, logs, clocks, registers, delay)
  and building `drivers/gpu/drm/amd/display/dc` for DCN 3.2.1 as
  a C library in the kext (MIT). First `dc_create` + reading state, then
  `dc_commit_streams`.
* DMCUB: loading `dcn_3_2_1_dmcub.bin` (`dmub_srv`); without it DC on
  DCN 3.x does not work.
* Link: AUX/I2C (EDID), DP training, HDMI; HPD through IH (DCN sources).
* `RDNA3Framebuffer`: real mode list from EDID, `setDisplayMode`,
  multiple heads (one instance per OTG), hotplug through connect interrupt.
* Risk: size (~150k lines) and dependencies on `drm_*`; plan B is
  our own mini-DC only for DP+HDMI SST on DCN 3.2.

## 4. 🟡 SMU 13.0.7: power management (bring-up done, sleep later)
* ✅ `hw/vbios.c`: VBIOS through the SMUIO ROM window, ATOM tables; `hw/smu13.c`:
  `smu_smc_hw_setup` — PPTable from PMFW (combo) + BoardTable from VBIOS,
  `RunDcBtc`, `SetAllowedFeaturesMask`, `OverridePcieParameters`,
  `EnableAllSmuFeatures`, DPM tables, PPT limit, workload, metrics
  (`SMU_TABLE_SMU_METRICS`). Kext: level 5, `rdna3pm=0/1/2`,
  `rdna3pmmask`; the simulator models PMFW (tables, features, DPM levels).
* By default no UCLK/FCLK/DCN DPM and no GFXOFF — until Display Core
  (see STAGE2 “PM without Display Core”).
* Later: BACO/sleep, `IOPMPowerState` in the kext, wake-up, thermal
  interrupts (THM) through IH.

## 5. ✅ Robustness: hang detection and GPU reset
* ✅ Fence timeout (10 s) → dump of `GRBM_STATUS`/`CP_STAT`/`SDMA0_STATUS`
  to the log, job VM marked “guilty”, `SQ_CMD` kills waves (soft recovery), GFX soft
  reset (`gfx_v11_0_soft_reset` + ring 0/KIQ/KCQ from scratch), mode-1 reset
  through PMFW and `hwInit()` from level 4; `VRAMLOST` based on a check word.
* ✅ VM fault from IH (VMC/GFX source 0): address, VMID, L2 status, guilty
  context, `AMDGPU_CTX_OP_QUERY_STATE2` and `-ECANCELED` as in amdgpu.
* Remaining: single-queue reset through MES (`RESET_QUEUE`), once MES
  is in; SDMA soft reset; `drop fDrmLock` while waiting.

## 6. 🟡 UAPI for RADV for real
* ✅ Timeline syncobj, `SYNCOBJ_TRANSFER`, syncobj-fd within a process
  (`drm_xnu`), `AMDGPU_CTX` with priorities and reset state, `BO_LIST`,
  CS chunks (user fence written by the ring, GDS/GWS/OA BOs from the BO list
  switched per VMID), `AMDGPU_INFO`
  with `SENSOR` (SMU metrics) and `GPUVM_FAULT`, `GEM_USERPTR` through
  `IOMemoryDescriptor::withAddressRange`.
* ✅ `vulkaninfo` on a macOS runner without the card: `libdrm_loopback.dylib`
  in the `radv-macos` job (run with `full: true`).
* ✅ syncobj sync_file between processes (file with the fence by value,
  `RD_NR_XNU_SYNCOBJ_FENCE`); opaque fd stays within the process.
* Remaining: PRIME/IOSurface (BO export/import, shared with the accelerator),
  dEQP compute through loopback (needs
  shader execution — only on the card).

## 7. WindowServer / Metal path
* Accelerator: IOSurface surfaces in VRAM, composition through flips
  (the skeleton from NullMoth is already there), `populateAccelConfig` for RDNA3.
* Metal plugin: review of the NullMoth translator for AMD differences (formats,
  limits, RDNA3 tiling), running simple Metal samples.

## Order
1 → 2 → 4 → 5 → 6 → 3 → 7. DC is the largest, but without hardware it is hard
to verify, and the mode takeover from GOP (`RDNA3Framebuffer`) is enough for
the first tests. Each item ends with a card moved on the board
in Notion and an entry in `docs/ROADMAP.md`.
