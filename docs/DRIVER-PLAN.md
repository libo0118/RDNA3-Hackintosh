# What a finished driver needs (2026-10-10)

The roadmap from today's state to a finished driver: what exists, what is
missing, what can be written now without the card, and what only a card
run can settle. `docs/PLAN-NEXT.md` stays the log of the items already
done; this file decides the order from here.

## Two finish lines

| | **D: desktop driver** | **M: Metal driver** |
|---|---|---|
| What the user gets | macOS on the RX 7800 XT as the only GPU: both monitors, every resolution the monitor offers, hotplug, sleep/wake, idle power near Linux (~12-15 W), stable for days | D + Metal apps run on the GPU (Safari, Preview, Final Cut, games through Metal/MoltenVK), WindowServer composites on the GPU |
| Left (estimate) | ~30-40 % | ~70-80 % |
| Biggest risk | DCN mode setting without the full amdgpu DC/DML | the Metal plugin (NullMoth port) has never run here; how much of Metal it covers decides everything |

Horizon: macOS 26 is the last x86_64 release; the work only pays while it
is supported.

## State per area

Legend: **card** = verified on the card (`docs/status/hw-verified.txt`),
**sim** = simulator/host tests only, **-** = missing.

| Area | card | sim only | missing |
|---|---|---|---|
| Bring-up (PSP, SMU, GMC/GART, IH+MSI, SDMA, GFX, KIQ on MES, KCQ) | all | MES scheduler pipe | bare metal cold boot (only the VM so far) |
| Recovery (hang, soft reset, mode-1 reset, VM faults) | - | all | a card run that provokes a hang |
| Power | GFX CG, GFXOFF allowed, UCLK DPM free (52 W idle) | - | idle power: PMFW counts GFX 100 % busy; non-GFX clock gating; suspend/resume (S3); runtime idle |
| Display | GOP takeover 1080p, cursor, EDID names, clone and extended desktop on a 2nd monitor | HPD interrupts, own timing values | own mode setting (other resolutions/refresh), mode list, hotplug to the framebuffer, DP link training, gamma LUT, DPMS blank, HDMI audio |
| UAPI (amdgpu ioctls) | CS on DMA/GFX/compute, fences, BO mmap, USERPTR, PRIME | timeline/syncobj fds | IOSurface sharing with the accelerator |
| Vulkan (RADV) | compute + transfer 8/8 (`rdna3_vktest`) | - | graphics pipeline (draw), images/formats, WSI (present), CTS subset |
| Accelerator / WindowServer | desktop composited (CPU) into VRAM surfaces, flipped on head 0; two-head routing logged | - | screens proven on both heads (pictures since `944d983`), gates on by default, GPU blits, IOSurface <-> BO |
| Metal | - | plugin + translator build in CI | everything on the card: device found, shader compile, compute, render, CAMetalLayer present, WindowServer on Metal |
| Video (VCN 4.0) | - | VCN 4.0 + JPEG 4.0 rings, VIDEO_CAPS and CS behind `rdna3vcn=1` (`hw/vcn40.c`, step vcn, simulator only) | verified on the card; DPG power gating; suspend/resume; decode/encode from Mesa (RADV video / radeonsi VA); VideoToolbox plugin (optional) |
| Packaging | OpenCore in the VM | `package/oc_install.sh` | bare metal guide, installer, release builds, Tahoe (26) layout |

## Work items

Size: S < 300 lines, M 300-1500, L 1500-5000, XL > 5000 (or unknown).
**now** = can be written and verified on the host/simulator today;
**1 boot** = written now, one card run confirms (batched into the round's
single big step); **iterate** = depends on what the card / macOS does,
needs several rounds, cannot be written far ahead.

### P - power (finish line D)

| # | Item | amdgpu source | Size | Test |
|---|---|---|---|---|
| P1 | Non-GFX clock gating: MMHUB, ATHUB, HDP, BIF/NBIO, IH, SDMA, MP0/MP1 light sleep | `soc21_common_set_clockgating_state`, `mmhub_v3_0_set_clockgating`, `athub_v3_0_set_clockgating`, `hdp_v6_0_set_clockgating_state`, `nbio_v4_3_update_medium_grain_*`, `sdma_v6_0_set_clockgating_state` | M | now -> 1 boot (watts in the PM trend) |
| P2 | Why the PMFW counts GFX busy: compare every register amdgpu writes after `gfx_v11_0_hw_init` that we skip (RLC CGTT, `RLC_PG_*`, IMU, `CP_*_IDLE`), log them; GFXOFF entry/exit counters (`smu_v13_0_0_get_gfx_off_residency`) | `gfx_v11_0_set_powergating_state`, `gfx_v11_0_update_gfx_clock_gating`, `amdgpu_gfx_off_ctrl` | M | now (diagnostics) -> iterate |
| P3 | Power profile + fan as amdgpu's defaults (BOOTUP_DEFAULT workload, fan curve from the PPTable), `rdna3pm=2` default | `smu_v13_0_0_set_power_profile_mode` | S | 1 boot |
| P4 | Suspend/resume (S3) and shutdown: `setPowerState` -> the reinit path the reset already has (fini in reverse, VRAM kept, rings re-created, display restored) | `amdgpu_device_suspend` / `_resume`, `amdgpu_device_ip_suspend_phase1/2` | L | now (sim: suspend+resume cycle) -> iterate (VM sleep with passthrough is unreliable; bare metal) |
| P5 | Runtime idle: no `PreventSystemIdleSleep` assertion once P4 works | - | S | with P4 |

### S - display (finish line D)

| # | Item | amdgpu source | Size | Test |
|---|---|---|---|---|
| S1 | Mode list from the EDID (detailed timings + CTA/DisplayID), filtered to what the link/clock can carry | `drm_add_edid_modes`, `dc_link_validate_mode` | M | now |
| S2 | Own mode set on a lit TMDS head: OTG timing, pixel clock (DCCG DTO / VBIOS `SetPixelClock` / DMCUB), HUBP surface, OPP/MPC, encoder | `dcn20_enable_stream_timing`, `dcn20_program_pipe`, `dce110_enable_stream`, `optc1_program_timing` | L | now (sim model) -> iterate (a wrong value = black screen; the boot step tries the variant table and keeps the GOP mode as fallback) |
| S3 | Bandwidth: DISPCLK/DPPCLK/DCFCLK minima through the SMU, watermarks/DLG for the modes S1 allows (a table for <= 4K60, not full DML) | `dcn32_prepare_bandwidth`, `dcn32_calculate_wm_and_dlg` | L | now -> 1 boot per mode class |
| S4 | Framebuffer: `getDisplayModes`/`setDisplayMode` -> S2, connect interrupt on HPD (S7 of the old plan done in sim), one fb per head always | IOFramebuffer | M | 1 boot (the report switches modes itself and grabs pictures) |
| S5 | Gamma/regamma LUT (`setGammaTable`, Night Shift, calibration) | `dpp3_program_blnd_lut`, `dpp20_program_regamma` | M | now -> 1 boot |
| S6 | DPMS: blank/unblank and display sleep | `dcn10_blank_pixel_data`, `dce110_blank_stream` | S | now -> 1 boot |
| S7 | DP SST link training (native DP monitors) | `dp_perform_link_training`, `dpcd` | L | iterate (needs a native DP monitor) |
| S8 | HDMI/DP audio | `dce_aud` | L | later (optional) |

### V - Vulkan (feeds M)

| # | Item | Size | Test |
|---|---|---|---|
| V1 | `rdna3_vktest`: draw a triangle into an image and read it back, image layout/format copies, timeline semaphores, two queues at once | M | now (host loopback runs the submit path) -> 1 boot |
| V2 | CTS subset in the report (`deqp-vk` with a fixed case list: api.*, memory.*, compute.*, a few draw cases), results summarised by triage | M | 1 boot per round (runs unattended) |
| V3 | Present: VK_EXT_metal_surface comes from MoltenVK's world; for RADV on macOS: present through the accelerator (IOSurface -> flip) | L | iterate |

### A - accelerator / WindowServer (D, prerequisite of M)

| # | Item | Size | Test |
|---|---|---|---|
| A1 | Two heads confirmed by the scanout pictures, then `rdna3accel=1 rdna3accelfb=4 rdna3acceliop=1` default (boot-loop guard kept: `-rdna3off`) | S | 1 boot |
| A2 | Swap copies by SDMA/GPU instead of the CPU (`submitSwapCopy` -> SDMA linear copy), BAR reads avoided | M | now -> 1 boot |
| A3 | IOSurface <-> GEM BO (same VRAM allocation visible to WindowServer and to RADV/Metal) | L | iterate |

### M - Metal (finish line M)

| # | Item | Size | Test |
|---|---|---|---|
| M1 | Install the plugin + translator + RADV into `/Library/GPUBundles/rdna3mtl` from `rdna3vm prepare`, `debug.rdna3accelfb=1` by boot-arg; report runs `mtlprobe` (`MTLCopyAllDevices`, device name, families) | M | 1 boot |
| M2 | Compute: a Metal compute kernel (shipped as a precompiled `.metallib`, built in CI on the macOS runner) through translator -> RADV, result checked | M | iterate |
| M3 | Render pass offscreen, texture formats, blits | L | iterate |
| M4 | CAMetalLayer / drawable present (IOSurface from A3, flip) | L | iterate |
| M5 | WindowServer on Metal (SkyLight's Metal compositor instead of the CPU path) | XL | iterate |
| M6 | App coverage (Safari, Preview, a game), allow-list widened | XL | iterate |

### R - robustness and release

| # | Item | Size | Test |
|---|---|---|---|
| R1 | Hang/recovery on the card: the report submits a deliberately hanging IB and checks the soft reset path | S | 1 boot |
| R2 | Soak: the report loops vktest/DMA for N minutes, PM trend, no leaks (`ioreg` BO counts) | S | 1 boot (longer step) |
| R3 | Bare metal: `docs/TESTING-HW.md` run on the real machine (GOP cold boot, no Linux before) | - | user, once D is near |
| R4 | Tahoe (macOS 26): `NM_TAHOE=1` accelerator layout, kext checks | M | VM with 26 |
| R5 | Release: signed-as-possible bundle, `package/` installer, OpenCore snippet, versioned artifacts | M | now |
| R6 | Video VCN 4.0 + VideoToolbox | XL | optional, last |

## How much can be written without the card

- **Simulator-verifiable ports** (register sequences from named amdgpu
  functions: P1, P4, S1-S3, S5, S6, A2, V1, R1, R2, R5) can be written in
  large batches - several thousand lines before a card run - because the
  simulator checks the order and values against amdgpu. What stays unknown is
  how the card reacts; each batch therefore ships behind a boot-arg and the
  round's single big step turns all of them on, with variant fallbacks in code
  (as the accelerator's head strategies did).
- **Behaviour-dependent work** (P2, S2's exact clock path, A3, M2-M6, V3)
  depends on what macOS / the card do and cannot be written far ahead: one
  round per discovery. Here the gain is in making each round report more
  (pictures, power trend, mtlprobe, CTS) so no round is spent on a question
  the report could have answered.

## Test rounds

One boot per round (`rdna3vm queue pending`, step `big`), unattended, then
`rdna3vm result`. The report already holds: DMA/GFX tests, Vulkan, PM
trend, accelerator swap/flip log, and (from `944d983`) a picture of what
each display pipe scans out. Planned additions, each cheap: `mtlprobe` (M1),
CTS subset (V2), hang test (R1), soak (R2), mode switching with a picture
per mode (S4).

## Done since the plan was written (round 18 batch, sim/host verified, step `big2`)

| Item | Commit | Notes |
|---|---|---|
| P1 HDP/ATHUB clock gating | `2bcd676` | `rdna3socg=1`; MC/IH/BIF CG flags are not set on GC 11.0.3 |
| P2 idle-power experiment | `b4acec0` | `rdna3pmexp=1`, six variants in one boot |
| P2 workaround: idle GFXCLK cap | `9616cdf` | `rdna3idlecap=1` |
| R1 hang test | `f555390` | `rdna3hangtest=1`, run last |
| R2 soak | `8e0f267` | `rdna3soak=<min>` |
| S6 display sleep (HUBP blank) | `c1c9126` | `rdna3dpms=1`; the link stays up (no monitor standby yet) |
| S1+S2 (scaled modes only) | `e4d49bf` | `rdna3modes=1`: DPP scaler on the native timing, `rdna3_modetest` |
| A swap routing fix | `2bcd676` | stale pointer matches after WindowServer re-created its resources |
| screen read-back | `944d983`, `8e0f267` | pictures + underflow per pipe in every report |
| V1 Vulkan draw, images, timeline | `83c8872` | triangle read back, GFX + SDMA image copies; loopback submits only |
| M1 + M2 Metal probe, compute | `6a64d94` | `build_metal.sh` in the VM installs the plugin; the report runs `mtlprobe` (devices + a compute kernel) |
| UAPI: firmware versions, VBIOS info | `eceebce`, `d815309` | RADV saw 0 for every firmware before |

Open from this batch: S5 gamma, real mode setting (other timings), monitor
standby (link off), display restore after a mode-1 GPU reset.

## Order

1. A1 (pictures confirm two heads) -> defaults on (A1, P3).
2. P1 + P2 diagnostics (idle power), S1 + S5 + S6, V1, R1 - one batch, one boot.
3. M1 (does Metal see the card at all?) - early, because it sizes finish line M.
4. S2-S4 (mode setting), A2.
5. P4/P5 (sleep), R2, R3 (bare metal).
6. M2 -> M6 iteratively; V2/V3 alongside.
7. R4, R5; R6 only if everything else holds.

## D4: display state across resets and sleep (2026-10-10, sim/host verified, step `sleep`)

What exists (`hw/dcn32_state.c`, `kext/src/RDNA3Resume.cpp`):

| Piece | amdgpu model | Behind |
|---|---|---|
| Snapshot of every lit head: the clone's per-instance register lists, DIG back end TMDS setup, routing, DCCG, DET, cursor, gamma mode, VBIOS ids, pixel clock; global DCHUBBUB system context / arbiter / DCCG part | `dm_suspend`'s cached state | always (reads only) |
| Check: power, OTG, blank, DIG, timing, system aperture, routing, registers that differ, frames | - | always (reads only) |
| Restore of a head (also a power-gated pipe with nothing left) in the `dcn32_modeset_apply` / `dcn32_clone_enable` order | `dm_resume` -> `dc_commit_streams` (`dcn20_enable_stream_timing`, `link_set_dpms_on`, `dcn32_hubp_pg_control`, `dcn32_program_det_size`, `dcn10_link_encoder_hw_init`) | `rdna3resume=1` |
| Monitor standby on any TMDS head (the boot heads too) | `link_set_dpms_off` / `link_set_dpms_on` (`dcn10_link_encoder_disable_output`, `dcn10_link_encoder_enable_tmds_output`) | `rdna3dpms=2` |
| After a GFX / SDMA soft reset: the heads checked (those resets never touch DCN) | - | always |
| After the mode-1 reset: the heads restored | `amdgpu_device_gpu_recover` -> `dm_resume` | `rdna3resume=1` |
| System sleep: snapshot, streams down | `amdgpu_dm_commit_zero_streams` | `rdna3resume=1` (snapshot always) |
| Wake: power loss detected (PCI config, `RCC_CONFIG_MEMSIZE`, BIOS scratch 7 ASIC_INIT_COMPLETE, PSP sOS, heads), published as `RDNA3,Wake*` | `amdgpu_device_need_post` | always |
| Wake after a power loss: `resetIfPspStuck` (a VM wake comes with a bus reset), ASIC_Init, the whole `hwInit()` again (PSP, firmware, SMU/PM, DMCUB, GMC, IH, SDMA, GFX, MES), then the heads restored | `amdgpu_device_resume` -> `amdgpu_device_asic_init`, `amdgpu_device_ip_resume` | `rdna3resume=1` |

The S3 gap - what a full resume still lacks, in the order it will bite:

1. **Suspend side.** Done (`a3b2ec6`, `rdna3resume=1`): `systemSuspend`
   runs `amdgpu_fence_driver_hw_fini` and `amdgpu_device_ip_suspend_phase2`
   (VCN/JPEG, MES, SDMA, GFX, PSP, IH, GMC, SoC); every wake then runs the
   resume. `smu_suspend` has nothing to send on 13.0.7 (mp1_state NONE).
2. **VRAM contents.** amdgpu evicts every VRAM BO to system memory before
   the sleep (`amdgpu_device_evict_resources`) and moves them back; here
   VRAM is simply lost: user BOs, the accelerator's and the framebuffer's
   surfaces (black until WindowServer redraws), and the **GPUVM page-table
   pool** - live VMs get their MMHUB/GFXHUB contexts back from `fVmPd` but
   the tables behind them are garbage, so old contexts fault (clients see
   VRAM lost). Page tables: done - after a VRAM loss the pool starts over,
   each live VM gets a new root and the UAPI maps every client's ranges
   again (`rdna3_drm_device_vm_replay`). BO contents: done - every DRM BO
   in VRAM, the seq64 slots, the accelerator's surfaces and the boot
   scanout are copied to wired system pages through SDMA before the
   suspend and back after a power loss (`kext/src/RDNA3Evict.cpp`).
3. **Display init.** `dcn32_init_hw` is not ported: the clock manager's
   raise half runs in the restore now (DISPCLK / DPPCLK hard minimums
   through the DAL mailbox, back to the saved clocks; `restored (-16)` only
   when that fails or DISPCLK sits at divider 127); DCFCLK and the rest of
   `dcn32_init_clocks` are not ported; DIO / PHY init of the unused
   links; the replay of the saved DCHUBBUB / DCCG globals stands in for
   `hubbub32_init`, `dccg32_init` and `hubbub3_init_dchub_sys_ctx`.
4. **Lost display contents.** The gamma / degamma LUT RAMs (a restore after
   a power loss sets them to bypass; the table macOS last sent is loaded
   again now, `displayGammaReload`), the VPG infoframe contents (infoframes off, as after a mode set),
   HDMI audio.
5. **DMCUB.** `rdna3dmub=3` attaches to the GOP's firmware; after a power
   loss or a mode-1 reset that firmware is gone and the re-init now loads
   our own, as `rdna3dmub=1` does (`RDNA3,DMUBReloaded`).
6. **DP heads.** Done for DP SST on one pipe with `rdna3dplink=1`: the
   sleep takes the link down (`link_set_dpms_off`), the restore brings
   the stream resources back (the link cap verified for a new sink,
   DTBCLK again for 128b/132b) and trains the link again
   (`link_set_dpms_on`); monitor standby goes through the link too. DP MST
   and ODM combined heads are still refused (blank only).
7. **Ordering with macOS.** The wake work runs on a thread call after
   `setPowerState(1)`; framebuffer power requests that arrive earlier are
   recorded and applied afterwards. Nothing yet tells IOFramebuffer that
   its VRAM contents are gone.
8. **Hotplug across the sleep.** Done: the HPD looks stop before the sleep
   (`amdgpu_dm_irq_suspend`; after a power loss the pins read unplugged
   until they are enabled again), the wake work programs the HPD pins and
   filters again (`link_resume`) and the looks start after the longest
   connect filter; a monitor plugged or pulled while asleep counts as a
   hotplug then.
