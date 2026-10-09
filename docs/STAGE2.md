# Stage 2: hardware bring-up

Status: **PSP + firmware + SMU + GMC (GART, MMHUB 3.0.2, GFXHUB 3.0.3)
+ IH 6.0 + SDMA 6.0 written and tested in the simulator**; GFX/MES
not yet.

## What is done

| Part | File | amdgpu counterpart | Test |
|---|---|---|---|
| Registers from IP discovery, MMIO + SMN, HDP flush | `hw/rdna3_hw.c` | `amdgpu_discovery_reg_base_init`, `amdgpu_device_rreg`, `nbio_v4_3`, `amdgpu_hdp_generic_flush` | `tests/sim_navi32.c` |
| FB location (VRAM MC address) | `hw/rdna3_hw.c` | `gfxhub_v3_0_3_get_fb_location` | sim |
| Firmware: SOS split, LOAD_IP_FW plan | `common/amd_fw.c` | `psp_init_sos_microcode`, `amdgpu_ucode_init_single_fw` | `tests/test_fw.c` + real files from linux-firmware in CI |
| PSP 13.0.7: bootloader (KDB/SPL/SYS/SOS), KM ring, TOC/TMR, LOAD_IP_FW (29 ucode), RLC autoload | `hw/psp13.c` | `psp_v13_0.c`, `amdgpu_psp.c` | sim (synthetic and real firmware) |
| SMU 13.0.7: mailbox, TestMessage/GetSmuVersion/GetDriverIfVersion | `hw/smu13.c` | `smu_cmn.c` | sim |
| VBIOS through the SMUIO ROM window, ATOM tables (firmwareinfo, smu_info, smc_dpm_info, vram_usagebyfirmware) | `hw/vbios.c` | `amdgpu_bios.c`, `atom.c`, `amdgpu_atomfirmware.c` | sim (synthetic image) |
| Robustness: state dump (GRBM/CP/SDMA), killing VMID waves (`SQ_CMD`), GFX soft reset (`GRBM_SOFT_RESET` + cp_resume), mode-1 reset through the PMFW debug mailbox + bring-up again from level 4, VM fault from IH (address, VMID, L2 status) → "guilty" context, `AMDGPU_CTX_OP_QUERY_STATE2` flags, `-ECANCELED` | `hw/gfx11.c`, `hw/gmc11.c`, `hw/smu13.c`, `kext/src/RDNA3Bringup.cpp` | `gfx_v11_0_soft_reset`, `gfx_v10_0_ring_soft_recovery`, `smu_v13_0_7_mode1_reset`, `gmc_v11_0_process_interrupt`, `amdgpu_job_timedout`, `amdgpu_ctx_query2` | sim (CP hang, IB fault, mode-1) |
| SMU 13.0.7 PM: driver/tool tables in VRAM, combo PPTable from PMFW + BoardTable from VBIOS, `TransferTableDram2Smu`, `RunDcBtc`, `SetAllowedFeaturesMask`, `OverridePcieParameters`, `EnableAllSmuFeatures`, DPM tables, PPT limit, workload, metrics (`SMU_TABLE_SMU_METRICS`) | `hw/smu13.c` | `amdgpu_smu.c` (`smu_smc_hw_setup`), `smu_v13_0.c`, `smu_v13_0_7_ppt.c` | sim (PMFW model: tables, features, DPM levels) |
| Startup in the kext (thread, firmware from Resources) | `kext/src/RDNA3Bringup.cpp` | `amdgpu_device_init` (part) | CI build |

## Running on hardware

1. Build the kext and add the firmware (it does not go into git, see `docs/FIRMWARE.md`):

   ```sh
   make kext
   make -C kext firmware FW_DIR=/path/to/linux-firmware/amdgpu
   ```

2. boot-args: `rdna3lvl=4` (PSP + firmware), `rdna3lvl=5` (+ SMU; by default
   `rdna3pm=1` reads the VBIOS and starts PMFW power management — a
   "safe" set without memory/fabric/DCN DPM, because without Display Core nobody tells
   PMFW when a p-state switch will not break scanout; `rdna3pm=0`
   keeps the boot clocks, `rdna3pm=2` the full amdgpu set,
   `rdna3pmmask=<hex>` an explicit feature mask; results: `RDNA3,PM`,
   `RDNA3,PMResult`, `RDNA3,PMFeaturesRunning`, `RDNA3,PMGfxClkMaxMHz`,
   `RDNA3,PMPowerLimitW`, `RDNA3,VBIOSPartNumber`, and after bring-up the metrics
   `RDNA3,PMGfxClkMHz`, `RDNA3,PMTempHotspotC`, `RDNA3,PMPowerW`,
   `RDNA3,PMFanRPM`) or
   `rdna3lvl=6` (+ GMC: GART in VRAM, MMHUB, TLB flush; `RDNA3,GMCResult`;
   + IH 6.0 ring, `RDNA3,IHResult`, `RDNA3,IHEarlyIVs`) or `rdna3lvl=7`
   (+ SDMA0: ring, ring test, 64 KiB VRAM→VRAM copy checked by the CPU;
   `RDNA3,SDMAResult`, `RDNA3,SDMACopyMismatches`). **Level 7 is the first
   one where the GPU executes commands by itself.** `rdna3lvl=8` adds GFX 11:
   RLC autoload check, RS64 start addresses, GFXHUB, golden registers,
   SH_MEM, CSB + SRM, GFX0 ring with clear state and a `SCRATCH_REG0` test
   (`RDNA3,GFXResult`). From level 6 on the kext maps BAR2 and enables the doorbell
   aperture (`RDNA3,Doorbells`); then SDMA0 and GFX0 write wptr through the
   doorbell, and at level 8 KIQ (MEC1 pipe 1) and the first
   compute queue KCQ0 (pipe 0), mapped with `MAP_QUEUES`, start (`RDNA3,KIQResult`,
   `RDNA3,KCQResult`; CS on `AMDGPU_HW_IP_COMPUTE`). `rdna3doorbell=0`
   keeps the wptr registers and skips compute. `rdna3msi=1` (from level 6) registers the card's MSI
   in IOKit: the IH ring is drained from the interrupt, and waiting on a fence
   sleeps until the handler wakes it (5 ms margin for a lost interrupt).
   Level 7 checks the first trap interrupt from SDMA (`RDNA3,MSIVerified`);
   if it does not arrive within 200 ms, the driver falls back to polling. Counters:
   `RDNA3,IRQs`, `RDNA3,IRQIVs`, `RDNA3,IRQFenceGFX`, `RDNA3,IRQFenceSDMA`.
3. Results in the IORegistry (`ioreg -l -r -c RDNA3Accel`): `RDNA3,Stage2`,
   `RDNA3,PSPResult`, `RDNA3,SOSVersion`, `RDNA3,SMUVersion`, `RDNA3,FBStartMC`.
   Log: `log show --last boot --predicate 'eventMessage CONTAINS "RDNA3:"'`.

Expected on the RX 7800 XT: `sOS running, version 0x00330024` (depending on
the linux-firmware version), 29× `LOAD_IP_FW ... ok`, `RLC autoload started`,
`SMU: PMFW 82.97.0` (or similar).

## Hangs, reset, page faults

`fenceWait` gives each fence 10 s. After that (`amdgpu_job_timedout`):
a state dump to the log (`GFX timeout: GRBM_STATUS ...`), the VM of the last job on that
ring becomes "guilty" (its `CS` calls end with `-ECANCELED`, `AMDGPU_CTX_OP_QUERY_STATE2`
returns `GUILTY|RESET`), `SQ_CMD` kills that VMID's waves and waits 100 ms for the
fence. If that does not help: the fences of all rings are completed, GFX
goes through a soft reset (`gfx_v11_0_soft_reset` + ring 0, KIQ, KCQ from scratch),
a hung SDMA gets its own soft reset (`sdma_v6_0_soft_reset`: FREEZE,
HALT + TH1_RESET, `GRBM_SOFT_RESET.SDMA0`, queue from scratch on the same
ring, pending fences treated as done, VM of the last IB guilty), and
if that fails too — a mode-1 reset through PMFW and
`hwInit()` from level 4 (PSP, firmware, SMU, GMC, IH, SDMA, GFX). The VM page
tables stay (VRAM survives mode-1; the `RDN3` check word verifies this,
otherwise clients get `VRAMLOST`). GPUVM page faults (VMC/GFX client,
source 0) are decoded from the IV (`gmc11_decode_vm_fault`: hub, VMID, address,
`L2_PROTECTION_FAULT_STATUS`) and also mark the VM as guilty. Counters in the
IORegistry: `RDNA3,VMFaults`, `RDNA3,LastFaultAddr`, `RDNA3,SoftRecoveries`,
`RDNA3,Resets`, `RDNA3,SDMAResets`, `RDNA3,GPUResets`, `RDNA3,VRAMLost`.

Waiting on a fence does not hold the global DRM lock: `drmFenceWait` releases
it while sleeping (other clients keep running), and recovery after 10 s is done
under the lock and only once. The UAPI does not keep pointers across that window (fences
by value, syncobj looked up again), and closing a file waits for
its in-flight ioctls.

## Decisions and risks (to check on hardware)

* **All PSP buffers in VRAM** (written through BAR0 + HDP flush). amdgpu
  supports this (`debug_use_vram_fw_buf`), so PSP starts without GART.
* **Allocation window 16 MiB–512 MiB of VRAM**, in the part visible through BAR0.
  We skip the first 16 MiB (pre-OS framebuffer), and the top of VRAM too (discovery,
  IFWI reservations). amdgpu reads the exact reservations from the ATOM tables
  (`vram_usagebyfirmware`); `vbios_fw_vram_reservation()` already reads them,
  the allocator does not use them yet.
* **PM without Display Core**: amdgpu enables UCLK/FCLK/DCN DPM, because its DC
  programs the watermarks and tells PMFW (through DMCUB) when the p-state may
  switch. We leave the GOP scanout untouched, so by default these
  features are disabled (`RD_SMU_FEATURES_SAFE`), and GFXOFF never — nobody
  sends `AllowGfxOff`. The full set (`rdna3pm=2`) is a test at your own risk:
  possible flicker/underflow of the picture.
* **No long DRAM training** (`psp_v13_0_memory_training`). IFWI trains
  memory at power-on. If memory errors show up, this is the first
  suspect.
* **SPL loads the KDB bytes**: this is what `psp_v13_0_bootloader_load_spl` does in Linux.
  Kept 1:1.
* **sOS takes >20 ms to start**: Linux waits 20 ms and only then watches for a change in
  C2PMSG_81. The simulator models this with a virtual clock.
* **MES1 (KIQ)**: we load it if the file is present. amdgpu for GC 11 may
  skip it depending on `enable_mes_kiq`.

## Next (order as in `amdgpu_device_ip_init`)

1. ✅ **GMC 11 / MMHUB 3.0.2 / GFXHUB 3.0.3**: GART in VRAM, VMID0 and
   contexts 1–15, TLB flush (MMHUB with a semaphore and private invalidation).
   ✅ Per-VM page tables (`hw/gpuvm.c`): PDB2→PDB1→PDB0→PTB in VRAM from a
   16 MiB pool, VMID 1–15 per open DRM file, `GEM_VA` writes PTEs (VRAM: physical
   VRAM address, GTT: system pages + SNOOPED), TLB flush after every
   change. Works from `rdna3lvl=6`; below that, mappings are only recorded.
2. 🟡 **IH 6.0** (`hw/ih60.c`): ring 0 in VRAM, wptr writeback, IV decoding,
   overflow handling; polled or with MSI (`rdna3msi=1`,
   `RPTR_REARM`, GFX EOP / SDMA trap fences → `ih60_iv_fence`). Remaining:
   ring 1 (page faults), rptr doorbell, VM fault → killing the context.
3. 🟡 **GFX 11** (`hw/gfx11.c`): GFX0 ring through the `CP_RB0_*` registers (like
   amdgpu with `amdgpu_mes=0`), clear state, ring test, jobs in the
   `amdgpu_ib_schedule` order (VM flush, ACQUIRE_MEM, HDP, frames, fence); KIQ and
   KCQ0 on MEC (MQD in VRAM, `kiq_init_register`, `SET_RESOURCES` +
   `MAP_QUEUES`, `UNMAP_QUEUES` on close); CS on
   `AMDGPU_HW_IP_GFX`/`COMPUTE` from `rdna3lvl=8`. Remaining: MES, more
   compute queues, priorities.
4. 🟡 **SDMA 6.0** (`hw/sdma60.c`): QUEUE0 ring in VRAM, ring test, copy,
   fill, fence, IB (`INDIRECT`), TRAP after fence with MSI; wptr through the
   doorbell (`S2A_DOORBELL_ENTRY_2` through RSMU) or register. Remaining: SDMA1.
5. Then `AMDGPU_CS` in `uapi/` gets a real implementation, and the
   `rdna3enum=1` mode is no longer needed.
