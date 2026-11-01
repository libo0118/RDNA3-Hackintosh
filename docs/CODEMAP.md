# Code map

Last updated: 2026-10-26 (branch `stage1-kext`). Read at the start of every task.

- The N-numbers (N38, N54, ...) and "card run <date>" in comments and docs are the
  history: `docs/DRIVER-PLAN.md` has one row per N, `docs/AFTER-TESTS.md` the
  procedure per symptom. They tell you why a file exists, not where it is.
- For a precise lookup use the code graph, not grep: `graphify query "<question>"
  --budget 1500`, `graphify explain "<symbol>"`, `graphify path "<A>" "<B>"`,
  `graphify affected "<symbol>"`. This map says which directory and file; the
  graph says which function calls what.
- Keep this file current: whoever adds a subsystem, a file family or a report
  member updates it in the same commit.
- Line counts below are rough (2026-10-26); the big files are one translation unit
  each, so search them by function name, do not read them whole.

## 1. Top-level layout

| Path | What it is |
|---|---|
| `kext/` | Core kext `RDNA3Accel.kext` (id `io.github.voten641.RDNA3Accel`): `kext/src/*.cpp` plus the portable C of `hw/`, `common/`, `uapi/` (`kext/Makefile` lists the sources). Bring-up, DRM bridge, display, PM. |
| `accel/` | `RDNA3Accelerator.kext`: IOGraphicsAccelerator2 subclass (NullMoth import, AMD adaptation, `PROVENANCE.md`). One 10k-line file `rdna3-accel.cpp`. |
| `fb/` | `RDNA3Framebuffer.kext`: IOFramebuffer, one head per instance (`rdna3-fb.cpp`; leaf class `AMDRDNA3Framebuffer`). |
| `agdc/` | `RDNA3AGDC.kext`: AppleGraphicsDeviceControl answers for IOPresentment (`rdna3-agdc.cpp`). |
| `hw/` | Portable C99 hardware layer, one file family per amdgpu IP block, ported from named amdgpu functions. No kernel headers. |
| `hw/gen/` | Generated register headers (`tools/gen_regs.py` from `tools/linux/regs`; `gc_11_0_0_*.h`, `dcn_3_2_*.h`, ...). Do not hand-edit. |
| `common/` | Portable C: discovery/firmware parsers (`amd_*.c`), allocators, and the `rdna3_*.h` decision headers (pure logic the kexts include, each with a host test). |
| `uapi/` | `rdna3_drm.c/.h`: the Linux DRM + amdgpu ioctl UAPI, OS-independent (BOs, VMs, CS, fences, user queues, scanout readback). |
| `userspace/` | Mac-side programs: `libdrm_xnu/` (DRM fd shim), `rdna3_drminfo.c`, `rdna3_modetest.c`, `rdna3_pmctl.c`, `rdna3_vktest.c`, `mtlprobe.m`, `mesa/` (RADV build script, `patches/0001..`). |
| `metal/plugin/` | The Metal driver bundle's code (Metal -> Vulkan -> RADV), NullMoth import. One translation unit (see 2.8). |
| `metal/translator/` | AIR -> SPIR-V translator (Rust, LGPL): `translator/` (library, `src/`, `tests/`) and `wrapper/` (cdylib `nvmtl_translate`, shipped as `librdna3mtl_translate.dylib`). |
| `metal/bundle/`, `metal/Makefile`, `metal/rdna3mtl-allow.txt` | Bundle `Info.plist`, plugin + translator build, default list of processes that get the Metal device. |
| `third_party/` | Vendored headers: `amd/` (amdgpu UAPI, ucode headers, clearstate, PSP interface), `libdrm/` (libdrm sources for libdrm_xnu). |
| `tools/` | Host tools (Python, shell, small C); see 3 and 4. `tools/vm/` (card VM on Linux), `tools/macos/` (on the Mac), `tools/ci/` (CI helpers), `tools/linux/` (amdgpu reference dumps), `tools/probe/` (one-off macOS probes run by `probe.yml`), `tools/progress/` (README status data), `tools/cts/` (Vulkan CTS patterns). |
| `tests/` | `make test` sources: simulator, host tests, parity/triage tests, fixtures (`tests/data`, `tests/stub/pexpert`). |
| `docs/` | `HANDOFF.md`, `DRIVER-PLAN.md`, `AFTER-TESTS.md`, `TESTING-VM.md`, `TESTING-HW.md`, `STAGE0..7.md`, `ROADMAP.md`, `PLAN-NEXT.md`, `FIRMWARE.md`, `status/` (generated README status), `INSTALL-OPENCORE.md`. |
| `tools/baremetal/` | The bare-metal macOS 26 installer USB for the owner's B550 / 5800X PC: `fetch.sh` (pinned downloads, `pins.txt`, `SOURCE`, macOS recovery), `build_efi.py` (OpenCore EFI + `config.plist` installer / `config-driver.plist`, AMD_Vanilla incl. the IOPCIFamily 10-bit tags patches via `tools/vm/vmtool.py amd_patches(iopci=True)`, the codeless `RDNA3MCEReporterDisabler.kext`, GRUB through `BlessOverride`, `--debug`), `make_usb.sh` (refuses non-USB disks, 7 numbered steps, progress; copies `stick/` and `DIR/rdna3/` to `<stick>/rdna3/`), `stick/` (`install.sh`, `report.sh`, `metal.sh`: the helper scripts the owner runs inside macOS, Polish messages). Procedure: `docs/BAREMETAL.md`; tests `tests/test_baremetal.sh` + `tests/test_baremetal_efi.py`. |
| `package/` | Installer: `make_pkg.sh` (.pkg + tarball), `install.sh`, `uninstall.sh`, `firmware.sh`, `oc_install.sh` (OpenCore injection, `--step`), `oc_snippet.py`, `pkg/`. |
| `.github/workflows/` | `build.yml` (CI, runs on Forgejo runners), `probe.yml`. |
| `reports/` | Git-ignored. Unpacked card/VM results, one directory per run (`2026-10-26c/`, ...), used for triage and as test fixtures by date. |
| `reference/` | Git-ignored. `macos-26.4.1/`: dumps of the target macOS (kernel collection, dyld, metallib, extensions) for reverse engineering. |
| `build/` | Git-ignored build output (`build/host` = `make test`). `graphify-out/` = the code graph. |
| Root | `Makefile` (host build + `make test`), `CLAUDE.md`, `CLAUDE_SUBAGENT.md`, `README.md`, `NOTICE`. |

## 2. Subsystems

### 2.1 Core kext (`kext/src/`)

Loaded levels `rdna3lvl=0..8` (boot-arg, see `RDNA3Accel.hpp` header comment): 0 PCI config, 1 BAR5 + VRAM size,
2 discovery, 3 GC read, 4 PSP + firmware, 5 SMU/PM, 6 GMC + IH, 7 SDMA, 8 GFX + KIQ/KCQ (+ MES with
`rdna3mes=1`). Levels 4-8 run on a kernel thread in `RDNA3Bringup`.

| File | Holds | Key entry points |
|---|---|---|
| `RDNA3Accel.cpp/.hpp` | The IOService: matching, PCI/BAR mapping, levels 0-3, power state, IORegistry identity. | `probe`, `start`, `setPowerState`, `mapMmio`, `readVramSize`, `readDiscovery`, `publishDiscovery`; BAR0 GTT backing class in the header |
| `RDNA3Bringup.cpp/.hpp` | Everything from level 4: the biggest file (4.9k lines). | `run` (thread body), `hwInit` (levels 4-8, also re-run after a reset), `postIfNeeded`, `loadFirmware`, `pmInit`/`pmCtl*`, `dmubInit`, `vcn*`, `vmCreate`/`vmMap`/`vmFlush`, `submit` -> `submitRing`, `fenceWait`/`fenceSignaled`/`forceFences`, `recover` (hang), `gpuReset`/`bacoCycle`/`mode1Reset`, `reinitFromLevel4`, `systemSuspend`/`resumeAfterPowerLoss`, `userq*`, `ivCallback`/`handleInterrupt`/`setupInterrupt` |
| coherence (N54) in `RDNA3Bringup.cpp` | Kernel GPU-memory coherence switches, selftest, fence system copy, skew counters (decisions in `common/rdna3_coh.h`). | `cohInit`, `cohSetup`, `cohSelftest`, `cohFence`, `cohSync`, `cohWaitIdle`, `cohPte`, `cohRingsForced`, `cohVmFlushForced`; sysctls `debug.rdna3_coh*` |
| GPU busy credit in `RDNA3Bringup.cpp` | Per-ring / per-process busy time for Activity Monitor (logic in `common/rdna3_gpu_busy.h`). | `busySubmit`, `busyObserve`, `busyEndAll`, `busyTake`; served to the accelerator by `platformGpuBusy` |
| `RDNA3Drm.cpp` | DRM bridge: `rdna3_drm_ops` callbacks into Bringup, BO memory, CPU mapping (`IOMemoryMap`, N47 `common/rdna3_bo_map.h`), the drm trace ring. | `drmInit`/`drmFree`, `drmOpen`/`drmClose`/`drmIoctl`, `drmBoAlloc`/`drmBoFree`/`drmBoUserptr`/`gttAlloc`, `drmVmCreate`/`drmVmMap`, `drmSubmit`, `drmFenceWait`, `drmBoMap*`/`drmMemoryForOffset`, `drmScanout` (readback); header `RDNA3Trace.h` |
| `RDNA3BarMap.h` | N62: the kernel window of BAR0 (the VRAM aperture), mapped like Apple's `ATIController::mapPCISubRange`; the reason when BAR0 has none. | `rd3MapBar0Window` (struct `Rd3BarWindow`), `rd3ReBarBar0Mib`, `rd3CpuIsVmm`; rules in `common/rdna3_bar_window.h` |
| `RDNA3UserClient.cpp/.hpp` | `IOAccelRDNA3Bridge` user client: one DRM file per open; selectors for ioctl and pmctl. | `externalMethod`, `ioctlMethod`, `pmMethod`, `clientMemoryForType`, `clientClose` |
| `RDNA3Platform.cpp` | Core's answers to the other kexts over `callPlatformFunction` (ABI = `accel/rdna3k_abi.h`): VRAM heap, GPU VA, surface copies, GPU busy, display calls, new client. | `callPlatformFunction` (dispatch by function name), `platformVramAlloc/Free/Ref`, `platformGpuVa`, `platformPageoff`, `platformCopyVram`, `platformIopSdma` (N63 `rdna3IopSdma`: bind / copy / unbind a presented surface's GART range), `platformGpuBusy`, `vramSysctl*` (`debug.rdna3_vram`) |
| `RDNA3Display.cpp` | DCN 3.2 display side: heads the GOP lit, mode set, flips, cursor, gamma, EDID, clone/mirror, MST, VRR, audio. | `displayInit`, `displayInfo`, `displayFlip`, `displayStatus`, `displaySetMode`, `displayModes`, `displayCursor`, `displayGamma`, `displayEdid`, `displayMirror*`, `displayDpLink`, `displayMstTakeover` |
| `RDNA3DisplayIrq.cpp` | N42: vblank / page-flip interrupts to the framebuffer. | `displayIrqInit`, `displayIrq`, `displayIrqSync`; sysctls `debug.rdna3_dispirq`, `debug.rdna3_vblirq` |
| `RDNA3DisplayPower.cpp` | Item O: p-state, MALL, SubVP, idle optimisations (`rdna3mall`, `rdna3pstate`); N74, without `rdna3mall` (the default): UCLK's hard minimum at the top while two or more heads are lit (`hw/dcn32_pstate.c` `dcn32_uclk_pin_*`; boot-arg `rdna3uclkpin`, sysctl `debug.rdna3_uclk_pin`, ioreg `RDNA3,DCNPower` / `RDNA3,UclkPinned`). | `displayPowerInit`, `displayPowerUpdate`, `displayPowerFlip`, `displayPowerTick`, `displayUclkPinInit`, `displayUclkPinUpdate` |
| `RDNA3Hotplug.cpp` | D6: HPD polling/filter, monitor plug after boot (`rdna3hotplug=1`). | `hotplugInit`, `hotplugTick`, `hotplugHandle`, `displayHotplugQuery` |
| `RDNA3Resume.cpp` | D4: display state snapshot, DPMS, restore after reset / sleep. | `displayStateSave*`, `displayStateRestore`, `displayAfterReset`, `displayWillSleep`, `displayDidWake`, `displayTxCheck` |
| `RDNA3Evict.cpp` | VRAM -> system pages before S3, back after (`rdna3resume=1`). | `vramEvict`, `vramRestore` |
| `RDNA3Regs.h`, `RDNA3Trace.h`, `kmod_info.c` | Register helpers, drm trace API, kmod stub. | - |

PM/SMU: `RDNA3Bringup::pmInit` (PPTable, DPM, fan, thermal), `pmCtl*`/`pmctl`/`sensor`
(userspace control, `common/rdna3_pmctl.h`), `gfxOff*`, `idleCap*`, `smuIrq`, `bacoCycle`; the
register layer is `hw/smu13.c`.

Boot-args: read with `rd3_arg()` from `common/rdna3_bootargs.h` (default profile, shared by core,
accelerator and framebuffer); test `tests/test_bootargs.c`.

### 2.2 `hw/` IP blocks (portable C99, ported from amdgpu)

| Files | Block | Typical entry points |
|---|---|---|
| `rdna3_hw.c/.h`, `navi3x_regs.h` | Device context, MMIO/SMN access, VRAM window and VRAM allocator, HDP, NBIO remap | `rdna3_dev_init`, `rdna3_rreg`, `rdna3_vram_alloc`, `rdna3_vram_fill`, `rdna3_hdp_flush` |
| `psp13.c/.h` | PSP 13.0.x: bootloader, sOS, TMR, ring, firmware load | `psp13_hw_init` |
| `smu13.c/.h` | SMU 13.0.10 (PMFW): mailbox, PPTable, features, metrics | `smu13_send_msg`, `smu13_pm_init`, `smu13_read_metrics` |
| `gmc11.c/.h`, `mmhub302.c` | GMC 11, GFXHUB 3.0.3 + MMHUB GART, VM faults | `gmc11_init`, `gmc11_gart_map`, `gmc11_gart_tlb_flush`, `gmc11_decode_vm_fault` |
| `gpuvm.c/.h` | Per-process GPU page tables (amdgpu_vm / vm_pt) | `gpuvm_vm_init`, `gpuvm_map`, `gpuvm_unmap`, `gpuvm_lookup`, `gpuvm_map_fragment` (the PTE fragment a VRAM mapping gets, N57); default path: `ptb_fragment` / `ptb_write` (amdgpu_vm_pte_fragment steps in PTB PTEs, boot-arg `rdna3vmfrag`), `ptb_refrag_edges` (a cut fragment re-fragmented) |
| `ih60.c/.h` | IH 6.0 interrupt ring and IV decode | `ih60_init`, `ih60_process`, `ih60_decode_iv` |
| `sdma60.c/.h` | SDMA 6.0 ring, copy / fill / fence packets, soft reset | `sdma60_start`, `sdma60_commit`, `sdma60_emit_copy`, `sdma60_test_ring` |
| `gfx11.c/.h`, `gfx11_info.c/.h` | GFX 11: RLC/RS64, KGQ/KCQ/KIQ, MES, reset, ring tests, CU/RB info after harvest | `gfx11_hw_init`, `gfx11_kgq_start`, `gfx11_kiq_start`, `gfx11_mes_*`, `gfx11_soft_reset`, `gfx11_ring_test_ib` |
| `xfer.c/.h` | VRAM <-> scattered system pages through a GART window (TTM copy); N63: system pages bound to a GART range and the pitched copy from it into VRAM (the IOP copy by SDMA) | `rdna3_copy_vram_to_pages`, `rdna3_copy_pages_to_vram`, `rdna3_gart_bind_pages`, `rdna3_gart_unbind_pages`, `rdna3_copy_gart_rect_to_vram` |
| `doorbell.c/.h` | NBIO 4.3 doorbell aperture, SOC21 doorbell indices | - |
| `soc21_cg.c/.h` | SOC21 common init, ASPM, clock gating outside GC | `soc21_program_aspm`, `soc21_set_clock_gating` |
| `vcn40.c/.h` | VCN 4.0 + JPEG 4.0 incl. DPG | `vcn40_hw_init`, `vcn40_suspend`, `vcn40_resume` |
| `vbios.c/.h`, `atom.c/.h` | VBIOS read, data tables, AtomBIOS interpreter | `vbios_read`, `vbios_parse` |
| `dmub32.c/.h` | DMCUB service (DCN 3.2) | `dmub32_hw_init`, `dmub32_attach` |
| `dcn32.c`, `dcn32_*.c/.h` | DCN 3.2 display core pieces: mode/clone/modeset, clk, otg, odm, opp, color, gamma, hdmi, hpo, dp, dp_mst, dsc, audio, hwseq, pstate, subvp, freesync, state, txstate | `dcn32_scanout_info`, `dcn32_flip`, `dcn32_modeset_plan/apply`, `dcn32_notify_wm_ranges` |
| `dcaux.c`, `dchpd.c`, `dcirq.c`, `dp_mst.c`, `edid.c` | AUX/DPCD/I2C, HPD filter, DC interrupt sources, MST topology helper, EDID parser | `dchpd_program_filter`, `dcirq_set`, `dcirq_ack` |

Rules: each function carries the amdgpu name it ports; add a file to `HW` in the root `Makefile` and to
`CSRC` in `kext/Makefile`; the register model lives in `tests/sim_navi32.c` (+ `tests/sim_*.inc` for
display). New amdgpu sources to track: `tools/progress/sources.txt`.

### 2.3 `common/` and `uapi/`

`common/amd_discovery.c`, `amd_fw.c`, `range_alloc.c`, `amd_mqd.h`: parsers and allocators (tests
`test_discovery`, `test_fw`, `test_range_alloc`). The `rdna3_*.h` headers are kernel-free decision logic
with one host test each (`tests/test_<name>.c`):

| Header | Decides |
|---|---|
| `rdna3_bootargs.h` | boot-arg defaults |
| `rdna3_coh.h` | N54 coherence switches (`debug.rdna3_coh_<name>`, mask, all) |
| `rdna3_attr_ring.h` | N57: does a DISCARDABLE BO (the GFX11 attribute ring) hold what SPI_ATTRIBUTE_RING_SIZE.BIG_PAGE assumes (VRAM, 64 KiB aligned and congruent, PTE fragment >= 4); the kext's `GEM DISCARDABLE` / `GEM_VA DISCARDABLE` lines |
| `rdna3_gpu_busy.h`, `rdna3_gpu_util.h` | GPU busy time per ring / process; Activity Monitor utilisation window |
| `rdna3_bo_map.h`, `rdna3_vram_place.h`, `rdna3_vram_park.h`, `rdna3_surf_pending.h` | CPU mapping holds its BO (N47); where VRAM goes; surface VRAM kept across dealloc/alloc (N41); WindowServer's pending GPU writes |
| `rdna3_bar_window.h` | N62: how much of BAR0 the kernel maps (`rdna3vramwin`, default 1 GiB, halved on a failed map) and why BAR0 got no IODeviceMemory (VM above 4 GiB, BAR above 1 GiB); used by `kext/src/RDNA3BarMap.h` `rd3MapBar0Window` (`RDNA3Accel::drmInit`, `RDNA3Bringup::mapVram`) |
| `rdna3_a3_display.h`, `rdna3_swap_front.h`, `rdna3_swap_copy.h`, `rdna3_write_gen.h`, `rdna3_flip_latch.h`, `rdna3_flip_queue.h` | which surfaces A3 keeps out; swap buffer / copy forward source; swap as copy; write generation (N65: `rd3_wg_cpu_rule`, a CPU-compositing WindowServer's SAME / OLDER backs present: `swap_gen_cpu`, `swap_gen_wsmetal`, `swaps_genhold_{same,older,unwritten}_h<N>`, `swaps_gencpu_h<N>`); flip latch; present queue (N38) |
| `rdna3_vbl_irq.h`, `rdna3_fb_policy.h`, `rdna3_tcall.h` | N42 vblank path; framebuffer policy (N65: `rd3fb_core_wait` / `rd3fb_core_late`, head 0 started before the core: `RDNA3Framebuffer::findCore` / `armCoreLate` / `coreLate`); stoppable thread call (N47, public KPIs only) |
| `rdna3_fbgate.h` | N58: the framebuffers' IOGraphics controller gates the accelerator closes (once per workloop, head order) before it calls the display machine itself (`RDNA3Accelerator::runInFbGates`; lock order fRegLock -> controller gates -> accelerator lock) |
| `rdna3_iop_stale.h` | N59: an IOP copy of a buffer whose IOSurface seeds did not move since the head last copied it (STALE), the bounded wait and its budget (`rd3AccelIopStaleGate` / `rd3AccelIopStaleNote` in `accel/rdna3-accel.cpp`, `debug.rdna3accel_iop_stale*`) |
| `rdna3_iop_latch.h` | N68: the IOP transactions' timing (K1 `rd3_il_txn_slow`, K2 `rd3_il_vbl_class` / `rd3_il_scan_bin`: the head's OTG frame / scanline at the copy's end) and the latched copy (`rd3_il_pick`: home or the second buffer D, whichever the panel does not scan; `rd3_il_d_freeable`; fallback reasons `rd3_il_fb`); kext side `rd3AccelIopTxnEnd`, `rd3AccelIopVblNote`, `rd3AccelIopLatchCopy`, `rd3AccelIopLbEnsureD` / `rd3AccelIopLbFree` in `accel/rdna3-accel.cpp`, switch `debug.rdna3accel_iop_latch` (boot-arg `rdna3ioplatch`), counters `debug.rdna3accel_iop_{txn,vbl,latch}_*`, trace event `RD3_TR_IOP` (`tools/rdna3_trace.py`, joined by `tools/plugin_trace.py --kernel`), triage rows `IOP timing` / `IOP latch` (`tools/triage_present_cost.py`, runnable on a sysctl dump) |
| `rdna3_iop_copy.h` | N61: the IOP copy of a presented surface into the scanout (8 bytes per access when everything is 8-aligned) and its cost (`rd3AccelIopCopyNote` / `rd3AccelIopGateNote` in `accel/rdna3-accel.cpp`, `debug.rdna3accel_iop_copy_*`, `iop_gate_us*`, log `iop: copy #N`). N63, the copy by SDMA: the core's GART range table `rd3_ig_*` (`RDNA3Bringup::iopGart*`), the page list `rd3_iop_pages_add`, fallback reasons, read-back samples (accelerator `rd3AccelIopBySdma` / `rd3AccelIopSdmaBind` / `rd3AccelIopSdmaUnbind`, `debug.rdna3accel_iop_sdma`, `iop_copy_sdma_*`, logs `iop: sdma copy #N`, `iop: sdma -> CPU`). N65: the surface formats (`rd3_iop_fmt_kind`) and the CPU conversion of 10-bit `l10r` into the 8-bit scanout (`rd3_iop_convert_l10r_rows`; `iop_copy_fmt_l10r` / `_fmt_l10r_seen` / `_fmt_other` / `_fmt_other_last`, `iop_flip_fmt`, fallback reason `format`; triage `IOP formats`) |
| `rdna3_strip.h` | N59: the frame-order strip (marker, 12 bits, inverted bits, magenta end): found and read by `rdna3_drminfo --scanout-strip`, painted by the loopback (`RDNA3_LB_STRIP=1`), decoded by `tools/frame_order.py` |
| `rdna3_trace.h` | binary per-frame trace format (decoder `tools/rdna3_trace.py`, parity test `tests/test_trace_parity.py`) |
| `rdna3_pmctl.h`, `rdna3_gamma_judge.h` | pmctl ABI; modetest gamma verdict |

`uapi/rdna3_drm.c` (4.6k lines): `rdna3_drm_ioctl` dispatches the amdgpu ioctls; `rdna3_drm_device_init`
and `rdna3_drm_device_set_*` take what the OS layer knows (`rdna3_drm_ops` = submit, VM, fences);
`rdna3_drm_file_open/close`, `rdna3_drm_bo_get/put`, `rdna3_drm_bomap_charge`, `rdna3_vmid_admit`,
`rdna3_drm_device_vm_replay`, `rdna3_drm_device_userq_*`. Private ioctls (`RD_NR_XNU_*`, root only), e.g.
scanout readback `RD_NR_XNU_SCANOUT` (`struct rd_xnu_scanout`, filled by `drmScanout` in `RDNA3Display.cpp`,
read by `rdna3_drminfo --scanout`). Tests: `tests/test_uapi.c`.

### 2.4 Accelerator (`accel/`)

`accel/rdna3-accel.cpp` is one file. Classes: `RDNA3Accelerator : IOGraphicsAccelerator2` (probe/start,
`newUserClient` -> core's `rdna3NewAccelClient`, `callPlatformFunction`), `IOAccelRDNA3Carrier`,
`RD3EventMachine`, `RD3DisplayMachine`, `RD3Task`, `RD3MemoryMap`; the family's Resource / SysMemory /
VidMemory / Surface / DisplayPipe / contexts are generated vendor subclasses
(`accel/gen_stubs.py` -> `rdna3accel_stubs.h`). Function families (`rd3Accel<Prefix>*`):

| Prefix | What |
|---|---|
| `Pq` | present queue (N38): off-thread flips, `rd3AccelInstallSysctlPq` |
| `Latch`, `Wg`, `Swap`, `Cf`, `Copy` | flip latch, write generation, legacy swap, copy forward, SDMA copy of surfaces |
| `Iop` | IOPresentment flip path (`rdna3accel_iop*`) |
| `Surf`, `Res`, `Vm` (`rd3Vm*`) | surfaces / resources, VRAM parking (N41) |
| `Perf` | GPU busy credit: `rd3AccelPerfUpdate` writes PerformanceStatistics (Device Utilization %), `rd3AccelPerfCreditOne` adds GPU time to each process's `IOAccelSubmitter2` `accumulatedGPUTime` (N38, N49) |
| `Trace`, `Sc` (`ScPost`/`ScApply`), `Scan` | per-frame trace (`debug.rdna3accel_trace*`); the swap-as-copy head state posted by non-swap paths and applied under the family lock; scans of other surfaces |
| `rd3AccelInstallSysctl*` | registers the `debug.rdna3accel_*` counters (`RD3ACCEL_CNT`) |
| `RD3_HOOK_ASM` (end of file) | naked wrappers that call the family's own methods; slots checked by `tools/check_accel_slots.py` |

Support: `rdna3k_abi.h` (cross-kext ABI: function names, structs, `RD3K_UC_TYPE_BASE`),
`iofam/`, `re/` (reconstructed IOAccelerator family headers), `tahoe_fwd.h` (generated by
`gen_tahoe_fwd.py`, macOS 26 vtable forwarding). The user client is the core's `RDNA3UserClient`.

### 2.5 Framebuffer and AGDC

`fb/rdna3-fb.cpp`: `RDNA3Framebuffer` (IOFramebuffer plumbing, connection table, aperture, VBL from the
core's interrupt, power states) and `RDNA3_FB_CLASS` = `AMDRDNA3Framebuffer`; talks to the core only through
`rdna3Display*` calls (`accel/rdna3k_abi.h`); decisions in `common/rdna3_fb_policy.h`,
`common/rdna3_vbl_irq.h`. Counters are IORegistry properties (`RDNA3,FBStats`).
`agdc/rdna3-agdc.cpp`: `RDNA3AGDC` + `RD3DeviceControl`; `rdna3_agdc_k5.h` (0x921 link config, 0x711
plane scaler), `rdna3_agdc_n48.h` (Navi48-MacOS answers), `AppleGraphicsDeviceControl_re.h`; sysctls
`debug.rdna3agdc*`. Build-id hashes: `FB_SRC_HASH` / `AGDC_SRC_HASH` / `ACCEL_SRC_HASH` in the three
Makefiles must list the same files as `tools/triage_reports.py` (`*_HASH_FILES`; tests
`test_accel_hash_parity.py`, `test_fb_agdc_hash_parity.py`). A new header included by a kext goes into both.

### 2.6 libdrm_xnu and userspace

`userspace/libdrm_xnu/`: `drm_xnu.c/.h` (a DRM fd = pipe read end; `drm_xnu_open/ioctl/mmap`),
`xf86drm_xnu.c`, transports `backend_iokit.c` (to `RDNA3UserClient`) and `backend_loopback.c` (runs
`uapi/rdna3_drm.c` in-process: host tests, `tools/radv_host.sh`), `override/libdrm_macros.h`. Real
libdrm sources are in `third_party/libdrm`. Programs: `rdna3_drminfo` (UAPI acceptance, `--dma-test`,
`--gfx-test`, `--scanout[-hires]`, `--scanout-strip FILE SECONDS` (N59), `--vcn-test`), `rdna3_modetest`, `rdna3_pmctl`, `rdna3_vktest`
(RADV smoke test), `mtlprobe` (Metal compute/render). RADV build + patches: `userspace/mesa/` (`patches/0010`: `RADV_ATTR_RING_BIG_PAGE=0`, N57).

### 2.7 Translator (`metal/translator/`)

`translator/src/lib.rs` + `main.rs` (CLI `in -> out.spv`), passes in `src/passes/`, reflection in
`src/reflect/`, SPIR-V in `src/spirv_*`, `mesh_lower.rs`, `fc_specialize.rs`, `air_intrinsics.rs`;
visible functions linked at pipeline creation in `linked_functions.rs` (references, tables, dylib
externs) and `visible_stubs.rs` (stubs found by suffix, the per-link report the plugin logs as
`link: "<kernel>" visible functions: ...` via `nvmtl_link_report`); integration tests
`translator/tests/*.rs` (CI runs only these, not the `src/` unit tests). `wrapper/src/*.rs`: the C-callable dylib the plugin loads
(`align_guard`, `buffer_addresses`, `linearize`, `table_ubo`, `wide_loads`, `g2_inline`). Built by
`make -C metal translator`; CI job `translator` (Linux) and `metal` (macOS dylib).

### 2.8 Metal plugin (`metal/plugin/`)

Single translation unit: `NVMTLDevice.m` `#import`s `NVMTLObjects.h/.m`, `NVMTLTrace.m`,
`NVMTLDevice_contract.m`, `NVMTLForward.m`, `NVMTLLibraryLoad.m`, `NVMTLGL.m`, `NVMTLEmbedded.m`,
`NVMTLVendorCompiler.m` and `#include`s `nvmtl_vk.c`; `NVMTLObjects.m` pulls in `NVMTLEventSync.m`,
`nvmtl_ab_switch.h`, `nvmtl_capture.h`, `nvmtl_ci_refl.h` (CoreImage's reading of a visible function: `nvmtl_ci_resolved` in `NVMTLObjects.m`, N60), `nvconv_lib.h` (`NVMTLObjects.h` includes `nvmtl_surfin_cache.h`; `NVMTLTrace.m` includes `nvmtl_surf_census.h`). Built by `make -C metal plugin` into
`RDNA3MTLDriver.bundle`. Everything is named `NVMTL*` / `nvmtl_*` (NullMoth heritage), including `nvlog`.

| File | Holds |
|---|---|
| `NVMTLDevice.m` | `NVMTLDevice` (MTLDevice), `nvlog` / `nvlog_uncapped`, process allow decision (`rdna3mtl_allow_policy.h`), arms the shared 10 s SUMMARY timer |
| `NVMTLObjects.m` (6k lines) | Metal objects: buffers, textures, heaps, fences, libraries, pipeline states, render/compute/blit encoders, command buffer and queue; `rdna3mtl.conf` knob loading (`RDNA3MTL_CONF`), managed-buffer logic, surface-in cache, capture hook, the SUMMARY timer |
| `nvmtl_vk.c` (8.6k lines) | The Vulkan backend: `nvmtl_vk_*` (init, buffers, images, pipelines, passes, copies, submit, surface dirty/mark, ms copies, fbfetch) |
| `NVMTLEventSync.m` | Command-buffer submission, MTLEvent / shared-event sync, pending surface writes |
| `NVMTLLibraryLoad.m` | AIR library loading, function lookup, translation via the translator dylib, dynamic libraries, binary archives, mesh, intersection tables |
| `NVMTLTrace.m` | ObjC half of the frame trace and the surface census (`nvmtl_cen_note`) |
| `NVMTLEmbedded.m`, `NVMTLForward.m`, `NVMTLGL.m`, `NVMTLVendorCompiler.m`, `NVMTLDevice_contract.m` | embedded plans, forwarding installer, Apple GL over the plugin, vendor compiler plug-in, MTLDevice contract answers |
| `nvconv.metal`, `nvconv_lib.h`, `nvmtl_*_spv.h`, `nvmtl_visibility_sum_spv.h` | Compiled helper shaders / SPIR-V arrays (generated, do not edit) |
| `rdna3mtl_config.h`, `rdna3mtl_allow_policy.h` | AMD constants and paths (`/Library/GPUBundles/rdna3mtl/rdna3mtl.conf`); which processes get the device (test `test_mtl_allow`) |

Helper headers (pure C, each with a host test) by feature:

| Feature | Headers |
|---|---|
| Drawable copy-out deferred to the present (N69) | `nvmtl_drawable_defer.h`; `nvmtl_seal_surfaces` in `NVMTLEventSync.m`; `_soDrawable`, `_presentRoots` |
| A/B switches (N50) | `nvmtl_ab_switch.h` (`nvmtl_ab_knobs[]`: the switches a process logs in `vk: ab switches`; managed-buffer page logic) |
| Framebuffer fetch (N48) | `nvmtl_fbfetch_plan.h` (N64: `nvmtl_fbf_barrier_mode`, the image barrier before a reading draw, `NVMTL_FBF_MEMBAR`), `nvmtl_spirv_fbfetch.h` |
| What a frame costs (N64) | `nvmtl_ws_frame.h` (`vk: ws frame: SUMMARY`, `nvmtl_wf_note` / `nvmtl_vk_wf_upload` in `nvmtl_vk.c`) |
| A drawable's way back (N73) | `nvmtl_drawable_life.h` (`vk: drawable: SUMMARY`; the `-[CAMetalLayer nextDrawable]` hook and the IOSurface keep ring `nvmtl_keep_put` / `nvmtl_keep_prune` in `NVMTLDevice.m`, the present note in `NVMTLEventSync.m` `nvmtlFinishExecution`, `nvmtl_dl_summary` in `NVMTLObjects.m`) |
| Multisample copies (N48/N52) | `nvmtl_msupload.h`, `nvmtl_msdl.h`, `nvmtl_msbuf.h`, `nvmtl_msstat.h`, `*_spv.h`; plain copies `nvmtl_copy_pack.h` |
| Surface-in cache (N51), census (N50) | `nvmtl_surfin_cache.h`, `nvmtl_surf_census.h` |
| Draw capture (N55) | `nvmtl_capture.h` (`NVMTL_CAPTURE_DRAW`), decoder `tools/capture_decode.py` |
| Hand-offs (N59) | `nvmtl_handoff.h` (command buffers whose scheduled point was seen before their IOSurface write was published; `vk: handoff: SUMMARY`; knob `NVMTL_SCHED_AFTER_PUBLISH`) |
| Present cost, GPU copy-out (N61) | `nvmtl_present_cost.h` (copy-out / surface-in counters, `vk: present cost: SUMMARY`, the GPU copy-out geometry `nvmtl_so_plan`; knob `NVMTL_SURFOUT_DMA`); in `NVMTLObjects.m` `nvmtl_surface_dma_record` (at a part's seal: the copy into the IOSurface's imported pages, `nvmtl_vk_cmd_surface_dma`), `-nvmtlSurfaceOutDMA` (the publish), `nvmtl_so_import` |
| Logging | `nvmtl_log_cap.h` (first N per format literal), `nvmtl_log_failure.h` (what reaches syslog in release) |
| Trace | `nvmtl_trace.h` (format + emitters, `/var/tmp/rdna3-nvmtl/<proc>-<pid>.trace`) |
| Misc | `nvmtl_gpu_time_queue.h` (one queue per process for GPU time), `nvmtl_linear_pitch.h`, `nvmtl_sampler_*.h`, `nvmtl_sample_*.h`, `nvmtl_physical_bounds.h`, `nvmtl_fd_token.h`, `nvmtl_readonly_metadata.h`, `nvmtl_independent_helpers.h` |

## 3. Diagnostics: where to look

Log prefixes (kernel log / `log show`):

| Prefix | Source |
|---|---|
| `RDNA3: ` | core kext (`BLOG`/`LOG`; `RDNA3: hang: ...` recovery, `RDNA3: coherence: ...` N54) |
| `RDNA3Accelerator: ` (`iop:`, `perfstats:`, `present queue:`) | `accel/rdna3-accel.cpp` (`ALOG`; capped by `RD3IOPLOG` etc.) |
| `RDNA3-fb: ` / `RDNA3FB: ` | `fb/rdna3-fb.cpp` |
| `RDNA3-agdc: ` | `agdc/rdna3-agdc.cpp` |
| `NVMTL: ` (process `Name[pid:tid] (RDNA3MTLDriver)`) | Metal plugin `nvlog`; capped per format literal, so kinds that matter need their own literal |

Counters and state:

- Core: `debug.rdna3_coh*` (N54 switches + counters), `debug.rdna3_vram` (visible-VRAM heap),
  `debug.rdna3_dispirq`, `debug.rdna3_vblirq`, `debug.rdna3_drmtrace[_stream]` (binary), `debug.rdna3_uclk_pin` (N74 switch).
- Accelerator: `debug.rdna3accel_*` (`swap*`, `swaps_*`, `perf_*`, `vm_*`, `surface_wait*`, `iop_*`,
  `cf_*`, `present_*`, `flipbuf*`, `trace[_sec|_stream]`); AGDC: `debug.rdna3agdc*`.
- IORegistry: `RDNA3,*` properties on the core, `RDNA3,FBStats` on the framebuffer.
- Plugin SUMMARY lines (`nvlog_uncapped`, key=value, newest line per process counts): `managed: SUMMARY`,
  `census: SUMMARY`, `vk: ab: SUMMARY`, `vk: fbfetch: SUMMARY`, `vk: surfin: SUMMARY[2]`, `msdl: SUMMARY`,
  `msupload: SUMMARY`, `vk: handoff: SUMMARY` (N59), `vk: present cost: SUMMARY` (N61), `vk: ws frame: SUMMARY` (N64), `vk: drawable: SUMMARY` (N73). Plugin traces: `tools/plugin_trace.py` (decoder), `NVMTL_TRACE=0` turns off.
- Kernel frame trace decoder: `tools/rdna3_trace.py`.

Report pipeline:

| Step | File |
|---|---|
| Collect on the Mac | `tools/macos/collect.sh [label]` -> `~/rdna3-report-<label>-<stamp>.tar.gz`; members `NN-name.txt`: `00`-`06` system/boot-args/kexts/PCI, `10`-`19` ioreg + sysctls (`17` agdc, `18` irq, `16` pairing), `20`-`28` logs (`20-log-rdna3`, `24-log-coredisplay`, `26-log-metal-plugin`), `29-surface-vram` (accelerator sysctl dump), `31`-`33` drminfo/vulkaninfo/pmctl, `34`-`35` plugin conf/build stamps, `36` frame trace, `37`/`31-plugin-trace/` plugin traces (`collect_plugin_trace.sh`), `38-vram-heap`, `39-bisect`, `40-*` crashes, `41-ladder*` A/B ladder, `42-coherence` |
| Drive a test boot | `tools/vm/macos/autoreport.sh` (LaunchDaemon on the VM, `--standalone` on bare metal): runs collect, DMA/GFX tests, uploads to the host. `bisect_run` (display bisect: `--bisect[-plugin|-agdc]`), `ladder_run` (A/B ladder). |
| Boot watchdog | `bootwatch_start` / `bootwatch_run` / `bw_collect` (`# BOOTWATCH-BEGIN` block of `tools/vm/macos/autoreport.sh`, background from the start of a VM boot): WindowServer without a GPU user client in the ioreg and nobody logged in after `rdna3bootwatch=N` s (default 75, 0 = off) -> uploads `bootwatch-<stamp>-<n>.txt` (ps, ioreg, plugin conf, `sample WindowServer`, `log show`, dmesg) every 60 s; `BOOTWATCH-END` when it recovered. Host: the share keeps them, `rdna3vm result` packs them; also `bootwatch-<stamp>-<n>.spindump` (every process, user and kernel stacks); triage: `tools/triage_bootwatch.py` (`bootwatch_rows`, a top-level FAIL with the stack's top frames; `spindump_deadlocks`: the kernel's `Deadlock:` verdicts and the images of the deadlocked threads, N58). Tests `test_bootwatch.sh`, `test_triage_bootwatch.py`; `docs/AFTER-TESTS.md` "WindowServer never opened the GPU". |
| Ladder variants | 1 `ladder` (`LADDER_RUNGS`: app rungs), 2 `ladder-ws` (+ WindowServer rungs), 3 `ladder-glyphs` (plugin knobs, N50), 4 `ladder-coh` (kernel coherence sysctls, N54), 5 `apps` (the app survey: the APPS block of `autoreport.sh`, `APPS_RUNGS`, `apps_rung` / `apps_stage` / `apps_do`; default driver, one real app per rung; rows from `tools/triage_apps.py`). Rung -> knob: `ladder_rung_env` (`NAME=value` env or `sysctl:NAME=VALUE`). Looks: `rdna3_drminfo --scanout[-hires]`. |
| VM host | `tools/vm/rdna3vm` (bash, 2.4k lines): `check`, `prepare`, `window`, `step N`, `args "BOOT-ARGS"`, `queue`, `approve` (once per framebuffer build), `reports`, `result` (all of a run in `~/Downloads/rdna3-result.tar.xz` + per-test PASS/FAIL, `--show`), `update` (git pull + newest CI artifact + prepare), `cts`. Steps and boot-args: `tools/ladder-steps.txt` (`@smbios=`, `@gpubus=` host-only tokens). Helpers: `vmtool.py` (OpenCore plist edits), `queue_journal.py`, `ocpatchsim.py`. |
| CPU mode | `CPU_MODEL=host` in the VM config (N53): QEMU `-cpu host` on the Ryzen; `vmtool.py plist --amd-cpu CORES` adds the AMD Vanilla patches (`tools/vm/data/amd_vanilla-*.plist`); `CPUS`, `CPU_THREADS`; the same mode injects RestrictEvents + Lilu for the CPU name in About This Mac (`vmtool.py ockexts` fetches the pinned releases, `oc_cpu_kexts` in `rdna3vm` puts them in / takes them out, pins in `tools/vm/data/oc_kexts.SOURCE`); tests `test_vm_amd_cpu.sh`, `test_ocpatchsim.py`. |
| Triage | `tools/triage_reports.py` (5.7k lines; `report()` builds rows `(LEVEL, name, text)`; `read_member(tar, "NN-...")` reads a member). Helpers it imports: `triage_tar.py` (archive read once), `triage_par.py` (`pmap`, parallel looks), `triage_glyphs.py` (N50 rows, ladder rows), `triage_apps.py` (`app <name>` rows of the app survey, `apps_rows`), `frame_order.py` (N59: `iop stale`, `handoff` and `frame order` rows with the displayed / page fps (N61); the survey's `41-ladder-frameorder-<burst>.log`), `triage_present_cost.py` (N61: `present cost` and `IOP copy` rows; N64: `ws frame`; N73: `drawable`), `triage_n48.py` (N48/N52 plugin copy rows), `triage_coherence.py` (N54 rows, `coh_ladder_rows`), `triage_attr_ring.py` (N57 `attribute ring` / `attr ring switch` rows; reads the newest `serial-*.log` next to the report too, `report_serial`), `triage_uclk_pin.py` (N74 `UCLK pin (N74)` row from ioreg `RDNA3,DCNPower`), `triage_bar.py` (N62 `VRAM aperture` row: BAR0, the kernel window, the visible heap; also from a serial log alone), `triage_bisect.py`, `triage_bootwatch.py` (boot watchdog files), `glyph_verdict.py` (automatic glyph verdict of looks), `lookimg.py` (PPM/PNG looks), `plugin_trace.py`, `rdna3_trace.py`, `capture_decode.py`, `result_checklist.py` (turns triage text into PASS/FAIL lines: `classify()` rules). |
| Procedure | `docs/AFTER-TESTS.md` (sections per step and per symptom: N42 VBL, N48 copies, N50 ladder, N51 surf-in, N54 coherence, N55 capture, plugin frame trace) |

## 4. Tests and CI

`make test` (root `Makefile`; builds into `build/host`, ASan/UBSan, `-Werror`; `RDNA3_FW_DIR` adds real
firmware checks). Each test is its own target `t.<name>` (lists `C_TESTS`/`PY_TESTS`/`SH_TESTS` and the
`TEST_RULE` calls in the Makefile; a new test goes into one of them), run in parallel: one job per hardware
thread unless `-jN` or `JOBS=N` says otherwise (`make -j1 test` / `JOBS=1` is serial). Each test's output is
`build/test-logs/<name>.log` (`tests/run_one.sh`); failed logs and one summary line come last
(`tests/test_summary.sh`). A test must use its own `mktemp -d` (or a `build/host` name only it uses): tests run
in any order beside each other; steps that depend on each other are one target (`tests/test_scanout.sh`,
`tests/test_capture_decode.sh`). The three slow step tests (`test_bisect_step.sh`, `test_ladder_step.sh`,
`test_apps_step.sh`) run in parts, one target `t.<script>.<part>` each (`PARTS` in the script, sourced helper
`tests/split_parts.sh`; `tests/test_x.sh --parts` lists them, `tests/test_x.sh PART` runs one, no argument runs
all); a new scenario goes into an existing part or a new part name. It runs, by area:

| Area | Tests |
|---|---|
| Hardware model | `sim_navi32` (Navi 32 register model: PSP/SMU/GMC/IH/SDMA/GFX/DCN; `tests/sim_*.inc`), `test_discovery`, `test_fw`, `test_range_alloc` |
| UAPI / libdrm | `test_uapi`, `test_bo_map`, `test_bo_map_race`, `test_libdrm_amdgpu` (real libdrm over `backend_loopback`), `rdna3_drminfo_host` (`--scanout`, DMA/GFX tests) |
| Accelerator / display logic | `test_a3_display`, `test_swap_front`, `test_attr_ring`, `test_swap_copy`, `test_write_gen`, `test_flip_latch`, `test_iop_stale`, `test_iop_copy`, `test_strip`, `test_flip_queue`, `test_vbl_irq`, `test_tcall`, `test_fbgate`, `test_gpu_busy`, `test_vram_park`, `test_vram_place`, `test_bar_window`, `test_surf_pending`, `test_trace`, `test_coh`, `test_fb_policy`, `test_bootargs`, `test_gamma_judge`, `test_agdc_k5`, `test_agdc_n48` |
| Metal plugin helpers | `test_mtl_allow`, `test_handoff`, `test_present_cost`, `test_ws_frame`, `test_linear_pitch`, `test_gpu_time_queue`, `test_ab_switch`, `test_drawable_defer`, `test_gsub_empty`, `test_surf_census`, `test_surfin_cache`, `test_copy_pack`, `test_capture` (+ `tools/capture_decode.py --check`), `test_msupload`, `test_msdl`, `test_spirv_fbfetch`, `test_fbfetch_plan` |, `test_ci_refl`
| Python / source checks | `test_accel_hash_parity.py`, `test_fb_agdc_hash_parity.py`, `test_trace_parity.py`, `test_accel_slots.py` (`tools/check_accel_slots.py`), `test_accel_locks.py`, `test_kext_kpi_versions.py`, `test_kmutil_kpi_guard.py` |
| Triage / tools | `test_triage_*.py` / `.sh` (fixtures in `reports/<date>` and `tests/trace_fixture.py`), `test_bootwatch.sh`, `test_glyph_verdict.py`, `test_lookimg.py`, `test_plugin_trace.sh`, `test_result_checklist.sh`, `test_bisect_step.sh`, `test_ladder_step.sh`, `test_apps_step.sh`, `test_triage_apps.py`, `test_frame_order.sh` (N59: strips end to end on the loopback, `tools/frame_order.py`), `test_play_step.sh`, `test_approve_gate.sh`, `test_vm_*.sh`, `test_forgejo_update.sh` |
| Packaging | `test_oc_install.sh`, `test_make_pkg.sh`, `test_coredisplay_prefs.sh`, `test_prune_traces.sh`, `test_membw_load.sh` |

Not in `make test`: probes that need a Vulkan device (`tests/vk_*_probe.c`: `vk_fbfetch_probe`,
`vk_meta_probe`, `vk_msdl_probe`, `vk_msupload_probe`, `vk_same_image_probe`, `vk_cpu_write_probe`; build
line in each header comment), `tools/radv_host.sh` (RADV on the host over loopback), and the card runs.
`tools/kext_check.sh`: `MACOS_SDK=<MacOSX11.3.sdk> tools/kext_check.sh [files]` type-checks kext, fb, agdc
and the C they build on Linux (no link). `python3 tools/progress.py [--check]` regenerates `docs/status/`.

CI: `.github/workflows/build.yml` (dispatch on Forgejo, labels `linux` / `macOS` only):

| Job | Runs on | What |
|---|---|---|
| `changes` | linux | sets `metal` (a `metal/` change or `full`) and `radv` (`userspace/`, `third_party/libdrm/`) flags that gate `metal` / `radv-macos` |
| `host-tests` | linux | `make test`, firmware load plan, host tools, README status check |
| `kext` | macOS | Xcode build of the four kexts (Sequoia + Tahoe layouts), `kextlibs`, KPI guard, userspace tools, installer `.pkg` artifact |
| `macos-tests` | macOS | `make test` on macOS |
| `metal` | macOS | plugin bundle + translator dylib (when `metal/` changed or `full`) |
| `translator` | linux | Rust tests of `metal/translator` |
| `radv-macos` | macOS | RADV against libdrm_xnu, `rdna3_vktest`, loopback `vulkaninfo` |
| `cts-macos` | macOS | Vulkan CTS subset build (only with `cts: true`) |

`probe.yml`: one-off macOS probes, triggered by changes under `tools/probe/`.

## 5. Where to change X

| Task | Files to touch |
|---|---|
| Add a core kext boot-arg | default in `common/rdna3_bootargs.h` (+ `tests/test_bootargs.c`); read with `rd3_arg()`; document in `docs/TESTING-HW.md` / `AFTER-TESTS.md` |
| Add an accelerator sysctl counter and its triage row | `RD3ACCEL_CNT(rdna3accel_<name>, gVar, "desc")` in `accel/rdna3-accel.cpp` + `sysctl_register_oid(&sysctl__debug_rdna3accel_<name>)` in the matching `rd3AccelInstallSysctl*` (the accelerator has no unregister); a new name prefix must match the `grep -E` in `tools/macos/collect.sh` (29-surface-vram) and `a3_sysctls` in `tools/vm/macos/autoreport.sh`; row in `tools/triage_reports.py` (or a helper module), classify rule in `tools/result_checklist.py` if the row text needs one, fixture + test in `tests/test_triage_*`, `docs/AFTER-TESTS.md` |
| Add a core kext counter / sysctl | next to the `debug.rdna3_coh*` / `rdna3_vram` code in `kext/src/RDNA3Bringup.cpp` / `RDNA3Platform.cpp`; collect it in `tools/macos/collect.sh` (a new `NN-` member or an existing one) and read it in triage |
| Add a plugin A/B switch (`NVMTL_*`) | read it with `getenv` (and the conf-file path) in `metal/plugin`; append the name to `nvmtl_ab_knobs[]` in `nvmtl_ab_switch.h` (+ `tests/test_ab_switch.c`); to make triage name it, `tools/triage_glyphs.py` |
| Add a ladder rung | variant 1/2/3: add to `LADDER_*_RUNGS` and a `case` in `ladder_rung_env` (`tools/vm/macos/autoreport.sh`); `LADDER_GLYPHS_RUNGS` + hint text in `tools/triage_glyphs.py` (variant 4: `LADDER_COH_RUNGS` in `tools/triage_coherence.py`); expected knob in `tests/test_ladder_step.sh` and `tests/test_triage_glyphs.py` / `test_triage_coherence.py`; `docs/AFTER-TESTS.md` ladder section |
| Add an app to the survey | `APPS_RUNGS` and the `case` tables `apps_proc` / `apps_stages` / `apps_do` / `apps_settle` (and `apps_helpers` / `apps_extra`) in the APPS block of `tools/vm/macos/autoreport.sh`; content it opens goes into `apps_assets` (a binary: `tools/apps_assets.py`); `APPS_RUNGS` / `APPS_TITLE` in `tools/triage_apps.py`; the stage list in `tests/test_apps_step.sh`; the table in `docs/AFTER-TESTS.md` "The app survey" |
| Add a ladder step (`rdna3vm step X`) | line in `tools/ladder-steps.txt`; table row in `docs/TESTING-HW.md`; section in `docs/AFTER-TESTS.md`; `tests/test_ladder_step.sh` |
| Add a report member | `run NN-name.txt ...` or a `{ ... } > "$dir/NN-name.txt"` block in `tools/macos/collect.sh` (unused number, keep the order comment); read it with `read_member(tar, "NN-name.txt")` in `tools/triage_reports.py` or a helper; tests; `docs/AFTER-TESTS.md` |
| Port an amdgpu function into `hw/` | function in the matching `hw/<ip>.c/.h` with the amdgpu name in its comment; add new files to `HW` in `Makefile` and `CSRC` in `kext/Makefile`; model the registers in `tests/sim_navi32.c` first (`make test` must pass); call it from `RDNA3Bringup.cpp`; register source in `tools/progress/sources.txt` + `python3 tools/progress/update_index.py`, then `python3 tools/progress.py`; a passed ladder step goes into `docs/status/hw-verified.txt` |
| Add a generated register header | `tools/gen_regs.py --out hw/gen/<name>.h ...` from `tools/linux/regs` |
| Add a UAPI ioctl | `uapi/rdna3_drm.h/.c` (`rdna3_drm_ioctl` switch), OS callbacks via `rdna3_drm_ops` -> `RDNA3Drm.cpp` / `RDNA3Bringup.cpp`; `tests/test_uapi.c`; libdrm side in `userspace/libdrm_xnu` |
| Add a cross-kext call | name + struct in `accel/rdna3k_abi.h`; answer in `RDNA3Platform.cpp::callPlatformFunction`; caller in `accel/` or `fb/`; if `rdna3k_abi.h` changes the build ids change (it is in the hash lists) |
| Add a pure-logic header shared by a kext | `common/rdna3_<x>.h` + `tests/test_<x>.c` + a rule in `Makefile` and its `test:` line; if accel / fb / agdc include it, add it to that `*_SRC_HASH` in its Makefile and to `*_HASH_FILES` in `tools/triage_reports.py` |
| Add a host test | `$(BUILD)/test_<x>: tests/test_<x>.c <headers> | $(BUILD)` rule in `Makefile`, the binary in the `test:` prerequisites and a run line in its recipe (C); Python / shell tests are a recipe line (`python3 -I -B tests/test_<x>.py`, `tests/test_<x>.sh`) |
| Add a Metal plugin helper header | `metal/plugin/nvmtl_<x>.h` (pure C), include it from `nvmtl_vk.c` / `NVMTLObjects.m`, test as above with `-Imetal/plugin` |
| Change what runs in CI | `.github/workflows/build.yml` (jobs stay on `runs-on: linux` / `macOS`) |
| Add a source file to a kext | `CXXSRC` / `CSRC` in `kext/Makefile` (or `accel/`, `fb/`, `agdc/` Makefile); also `tools/kext_check.sh` defaults and, for accel/fb/agdc, the hash lists |
