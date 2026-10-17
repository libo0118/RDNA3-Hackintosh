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

## State per area (updated 2026-10-14, card run 56)

Legend: **card** = verified on the card (`docs/status/hw-verified.txt`),
**sim** = simulator/host tests only, **-** = missing. The amdgpu functions
that apply to this card are all ported (`docs/status/STATUS.md`: 1483 of
1483); what the table lists as sim waits for its card step.

| Area | card | sim only | missing |
|---|---|---|---|
| Bring-up (PSP, SMU, GMC/GART, IH+MSI, SDMA, GFX, KIQ on MES, KCQ, MES scheduler pipe) | all | kernel queues mapped by the MES scheduler (`rdna3amdgpu=1`), user queues, amdgpu's exact sequence | bare metal cold boot (only the VM so far) |
| Recovery | GFX and DMA hang recovery (`hanggfx`, `8h`) | mode-1 reset, VM faults, BACO | - |
| Power | GFX CG, GFXOFF allowed, UCLK DPM free, idle GFXCLK cap (14-15 W) | non-GFX clock gating, S3 suspend/resume with VRAM eviction, PM control (fan, power, OD, profiles) | idle power down to Linux's ~12 W (the PMFW counts GFX busy) |
| Display | GOP takeover, cursor, EDID names, extended desktop, own mode set with the DISPCLK raise, scaled modes, display sleep, hotplug | DP SST / MST / DSC (also over MST) / UHBR / ODM, HDMI 2.0 with scrambling, audio, output format, gamma, the display state across a reset and a sleep | the DP / DSC / MST / UHBR / 4K HDMI steps need such monitors on the bench |
| UAPI (amdgpu ioctls) | CS on DMA/GFX/compute, fences, BO mmap, USERPTR, PRIME | user queues, timeline syncobj fds; IOSurface BOs (`RD_NR_XNU_SURFACE_BO`, A3, host-tested, not on the card yet) | |
| Vulkan (RADV) | `rdna3_vktest` 13/13: compute, transfer, a triangle through a render pass, image copies, a timeline semaphore | the CTS subset in the report (V2, step `cts`) | WSI / present (V3) |
| Accelerator / WindowServer | desktop composited through it on both heads, flipped zero-copy; copy forward (`submitCopyForward`, from the buffer the head showed last; a no-op when that is the target: `common/rdna3_swap_front.h`, card run 58b's flicker with two backs) | swap copies by SDMA (A2, `rdna3accelsdma=1`) | gates on by default (A1), IOSurface <-> BO (A3: WindowServer composites into per-process copies, 8-10 ms per 1080p frame) |
| Metal | `mtlprobe` compute + render, a CAMetalLayer window on the screen (M4), WindowServer composites on the plugin (M5), Chess and Maps run on it (M6) | - | GPU access to the apps' read-only backing stores (GEM_USERPTR read-only, run 57), point/line draws (mesa 0009), Safari's GPU process (sandbox blocks `/Library/GPUBundles`) |
| Video (VCN 4.0) | - | VCN 4.0 + JPEG 4.0 rings, VIDEO_CAPS and CS, DPG power gating, suspend/resume | the card run (steps `vcn`, `vcn2`, `vcnsleep`); decode/encode from Mesa; VideoToolbox (optional) |
| Packaging | OpenCore in the VM | `package/oc_install.sh`, the installer `.pkg` and tarball from CI | bare metal run of the guide, Tahoe (26) layout on a 26 VM (R4) |

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
| A3 | IOSurface <-> GEM BO (same VRAM allocation visible to WindowServer and to RADV/Metal): `RD_NR_XNU_SURFACE_BO` gives a BO over the accelerator's allocation for a surface plane (looked up in the caller's task, referenced while the BO lives); libdrm_xnu's `drm_xnu_surface_fd` makes it a dma-buf fd for RADV's OPAQUE_FD import, the Metal plugin uses it where NVK used its RM hook. Card check: plugin log "item 7: ... live in the surface's family VRAM", kernel log "GEM: IOSurface ... (A3)". Open question for the card: if the accelerator ever re-backs a surface (deallocPhysical, then a new allocation) while a BO holds the old range, RADV would draw where WindowServer no longer reads - stale content, not corruption. Security (ECC review): isolation rests on IOSurfaceRoot's lookup in the caller's task, checked by `rdna3_drminfo --surface-xproc` in every report (SURFXPROC: another process's surface must give ENOENT); surface BOs cannot be exported, one file pins at most half the visible VRAM; still open: DIRTY is not tied to a surface the file imported (any process may mark a surface it can look up). Card run 58 (2026-10-15c, first card run): every process shared surfaces, but WindowServer's display framebuffers (the 1920x1080 compositing surfaces) got BOs over a second VRAM copy that no swap scans out (the legacy surface's buffers are other allocations filled by the family's page-on): the main screen kept an old frame. They are kept out of A3 now (`common/rdna3_a3_display.h`, `rdna3a3display=1` lets them in, N27) | L | card run 58: works for app surfaces; display surfaces kept out (N27) |

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

### N - not built at all (inventory 2026-10-15)

Three code-explorer agents went through the kernel, accel/metal and the
userspace/installer for what does not exist yet (not what is untested).
Order is by what a user misses most; "quick" = the first batch. Status:
open / in work / done (with the commit).

| # | Missing | Where it shows | Size | Status |
|---|---|---|---|---|
| N1 | Ordinary apps get no GPU: the Metal plugin loads only into the allow-list (`rdna3mtl-allow.txt`); Safari's WebKit.GPU sandbox refuses `/Library/GPUBundles` | metal/plugin, package | M | done 2026-10-15 (`%apps` mode, kill switch `rdna3mtlapps=0`; Safari stays out: its sandbox; card check: System Settings / Preview on the plugin) |
| N2 | Hardware video: VCN 4.0 decode/encode behind VideoToolbox (= R6); `RD3VideoContext` empty | accel, kext, plugin | XL | open |
| N3 | Sleep: S3 on the card (P4), S4 hibernate (no code), runtime idle / BACO-on-idle (P5) | kext | L | open |
| N4 | Other Navi 3x cards: matching is `0x747e1002` only (kext, fb, accel Info.plist, tools); 7900 (Navi 31), 7600 (Navi 33), Pro | all | M-L per chip | open |
| N5 | HDR / EDR / 10-bit: IOFramebuffer 8-bit only, no HDR static metadata infoframe, accel present path BGRA8 only, no XR pixel formats | fb, hw/dcn32, accel, plugin | L | open |
| N6 | VRR / FreeSync: `dcn32_set_drr` has no caller, no adaptive-sync / SPD infopackets | hw/dcn32_dp, hw/dcn32_hdmi, RDNA3Display | M-L | done 2026-10-15 behind `rdna3vrr=1` (merge 89ca848); macOS has no VRR API, the driver stretches vblank and each flip ends the frame; open: below-the-range frame doubling (needs a vblank IRQ hook), FreeSync video modes; card check: `RDNA3,Head<n>,VRRRange` and whether WindowServer flips often enough |
| N7 | Fan / power / sensors for the user: only root `rdna3_pmctl`; no daemon with profiles, no sensor provider for iStat-style tools, no app | kext, userspace | M | open |
| N8 | HDMI/DP audio (= S8) | hw, kext | M-L | open |
| N9 | Vulkan WSI / swapchain (= V3; RADV built with `-Dplatforms=`) | userspace/mesa | L | open |
| N10 | Metal gaps: GPU-side indirect command buffers, raster order groups, tile shading, MTLIO, cross-process shared events, sparse (resource state encoder), multiple viewports, MSAA depth resolve, profiler counters, Metal 4 | metal/plugin | S..L each | open |
| N11 | Pixel formats: BGR10_XR / BGRA10_XR, 16-bit packed (B5G6R5, ABGR4, A1BGR5, BGR5A1), GBGR422 / BGRG422; RG8 sRGB maps to a non-sRGB format | metal/plugin | S each | open |
| N12 | Translator: indirect calls through function pointers (visible function tables) | metal/translator | L | open |
| N13 | Display: overlays / more than one plane per head | hw/dcn32 | L | open |
| N14 | Display: YCbCr 4:2:0, MST over UHBR, DP HPD_RX short-pulse IRQ (polled every 2 s now), leaving the DISPCLK /127 divider (OTG add/drop pixel) | hw/dcn32 | S-M each | open |
| N15 | Gamma (Night Shift, calibration) off by default: `rdna3gamma=1` needed though card run 56 passed it | accel, fb | S | done 2026-10-15 (merge of quick/n15; card check: colours with Night Shift) |
| N16 | OpenGL beyond Apple's layer (Zink/EGL), OpenCL, Vulkan video | userspace/mesa | L-XL | open |
| N17 | GFX/SDMA mid-IB preemption (MCBP), per-queue reset (only soft reset / mode 1) | hw/gfx11, hw/sdma60 | M | open |
| N18 | `AMDGPU_INFO_VRAM_LOST_COUNTER` (and NUM_EVICTIONS etc.) always 0 although the kext handles VRAM loss | uapi | S | done 2026-10-15 (counter, ctx VRAMLOST, CS refusal `rdna3vramlostcs=0`; NUM_EVICTIONS etc. stay 0: nothing moves BOs) |
| N19 | GART dummy page in system memory, `GEM_OP_SET_PLACEMENT` migration, RESERVE_VMID | hw/gmc11, uapi | S-M | open |
| N20 | Release: updater / release feed, signed package, macOS 26 in the package (= R4, R5) | package, CI | M | open |
| N21 | Uninstall leaves the OpenCore EFI copy, its Kernel/Add entries and boot-args (only a NOTE) | package/uninstall.sh | S | done 2026-10-15 (6be15e1: `oc_install.sh --remove`, run by uninstall.sh) |
| N22 | End-user crash/report collection (only the manual `autoreport.sh --standalone`), kext log after a panic | tools, package | M | open |
| N24 | VMIDs held for a process's whole life: one of the 15 hardware VMIDs (7 with `rdna3userq`) per open DRM file, so with the Metal plugin serving every app (N1) the card runs out after about a dozen programs and the next one gets no GPU. amdgpu binds a VMID only while a job runs and takes it back when the job's fence signals (`amdgpu_vmid_grab`, `amdgpu_vmid_grab_idle`, `amdgpu_vmid_grab_reserved`, the VMID manager `amdgpu_vmid_mgr_init`, the page-table flush on a reused VMID `amdgpu_vm_flush`); port that so VMIDs are shared in time. Until then the open is refused cleanly when none is free, at most 8 per uid, the last 2 for root and _windowserver (N18); residuals (ECC security review): one app of the console user can take that uid's share and lock the user's other GPU apps out; after a fast user switch the previous console user keeps its VMIDs and the new one may get none | kext, hw/gpuvm | L | open |
| N25 | GPU hang recovery is triggered by whoever waits 10 s on a fence (`drmFenceWait` -> `recoverHang`), not by how long the job itself has run: a process can still make the kernel reset another process's long job by submitting a NOP behind it and waiting. Port amdgpu's job timeout (`drm_sched` job timeout per ring, `amdgpu_job_timedout`), so recovery follows the running job's own runtime | kext, uapi | M | open |
| N26 | Card run 57: the HDMI boot head's monitor said "no signal" while its OTG ran with the desktop (DP clone fine); driver log identical to run 56, where both worked. Diagnostic build: the TX watch (`hw/dcn32_txstate.c`, `RDNA3,Head<n>,TX`, the looks' `TX` lines). Next run: the `TX at the start` / `after the clone` / `TX change` lines and the look's `TX problems`; when the monitor went dark (from the boot screen, at WindowServer, after a test step). Latent, `rdna3vrr=1` only: `optc32_set_drr` without bounds selects V_TOTAL_MIN / MAX, which on a head the driver did not mode-set may be 0 (the TX watch names it `DRR bound 0`; `displayVrrCommit` runs only after a mode set, which programs them) | hw/dcn32_txstate, RDNA3Resume | M | open (waits for the card) |
| N27 | Card run 58 (2026-10-15c): the main screen kept an old frame (wallpaper and menu bar) while WindowServer's windows flashed in and out; root cause: A3 BOs over WindowServer's display framebuffers, a VRAM copy no swap scans out (flips only ever went to the legacy surface's buffers 0xb100000 / 0xc0e0000 / 0xc8d0000, the BOs sat at 0xd0f0000..0xe8d0000). Interim (done): those surfaces stay out of A3, composited through copies (`rdna3a3display=1` / `debug.rdna3accel_a3display=1` restores the run 58 behaviour; off while `debug.rdna3accel_iop_flip=1`); a display surface is a WindowServer surface of the size a framebuffer scans out at the import (asked live) or of the head's own swaps; the kernel knows the importing process only, not the creator, so WindowServer's import of an application's full-screen surface is refused too and read through copies (not counted apart: IOSurface exposes no creator to the kernel); `a3 display:` lines, `debug.rdna3accel_swaps_h<N>` / `swaps_a3_h<N>`, triage `A3 display` and `M6 screen` (a pipe frozen through the M6 looks). Open: (1) zero-copy for the display: map a legacy swap buffer to its IOSurface and flip the IOSurface's own VRAM when the dirty mark says it is newest, then let display surfaces back in; (2) VRAM is not cleared on allocation (the new WindowServer's buffer at 0xbc50000 showed the old WindowServer's last frame); (3) Terminal (apps mode) TCP reads at an unmapped VA 0x80018223f000 during and after the 960x540 mode switch, 9 times, still after its VM was marked guilty, each followed by 4 VMID 0 CPC writes at address 0 (the 45 VMFaults); WindowServer's GFX job hung at the same mode switch (soft reset, WindowServer restarted) ; card run 58b (2026-10-15d): with the display surfaces out, windows and menus flickered on both screens - WindowServer's 615 other inputs were A3 BOs it also wrote (747 surface-in uploads into shared VRAM) while their owners drew there; every WindowServer import is now kept out by default (`rdna3a3ws=1` / `rdna3a3=0`), A3 stays for applications' own and shared surfaces; (4) open: an ordering of owners' GPU / CPU writes with WindowServer's reads (dirty marks the reader honours, no reader uploads into shared VRAM) before WindowServer's inputs can join A3; the kernel's A B A B flicker counters say whether the copy path flickers too; (5) open: flicker confined to one band of one head (a small window or one status item) reads like a blinking caret and is only a WARN (`swaps_ababwide_h<N>` counts the swaps where more than one band alternated); the 8 bands x 48 columns sample can miss a small item altogether; card run 58b's second run (2026-10-16): the flicker stayed with every WindowServer input kept out - its cause was copy forward copying nothing while two back buffers took turns (fixed, `common/rdna3_swap_front.h`), so whether WindowServer's inputs can go back into A3 is open again once the flicker is gone; (6) open after the ECC review of 60526e4: a copy forward that missed rects keeps them pending for that buffer (8 rects, then the whole picture) and copies them with the next copy forward into it from a source that did not miss them itself (after 3 calls blocked by a source that missed them too, copied anyway: both backs then hold the same stale region until WindowServer redraws it), not reported to CoreDisplay (which would switch copy forward off for good, card run 54) - if the card shows rects stale for long, reporting after a run of misses is the alternative; the record copy forward reads is the buffer the head's last swap presented even when the flip failed (WindowServer's frames, not the panel's); the IOSurface IOP flips (`rdna3accel_iop_flip=1`) and the legacy swaps share the per-head record, a legacy copy forward then finds a foreign buffer and copies nothing | accel, plugin, kext | M | interim done, open (1)-(4) |
| N28 | Card run 2026-10-17 (~10th run of "windows and menus flicker on both screens": a window shows, then the wallpaper where it was): copy forward works now (h1 3080 from the shown buffer, 0 missed) and the A B A B test sees little (1 % / 15 %), but it hashes at swap time only. Code finding: `rd3AccelFlipTo` -> `flipToLocked` programs the HUBP and returns, the flip latches at the next vblank, so `submitSwapCopy` gave WindowServer the old buffer back while the panel still read it (H1). Diagnostic build + fix (`common/rdna3_flip_latch.h`, `tests/test_flip_latch.c`): the swap waits for the latch (`debug.rdna3accel_swap_latch` / boot-arg `rdna3swaplatch`, default 1; amdgpu: the flip event from `dm_pflip_high_irq`), the counters `swaps_latchpend/_latchwait/_latchus/_latchmax/_latchtimeout/_latchunknown/_latchoff_h<N>`, `cf_onpanel_h<N>`, the torn test (`swaps_tornchk/_torn/_inplace/_tornskip_h<N>`, `debug.rdna3accel_swap_torn`), `swaps_shownfreed_h<N>`, `swaps_emptyfilled/_emptystill_h<N>` (triage `flip latch`). H2 (the flip before the GPU finished the frame) has no mechanism in the default configuration: the legacy buffers are written only by the family's page-on (CPU) and copy forward (SDMA, waited for), WindowServer's GPU composite reaches them through the plugin's surface-out after `vkQueueSubmit` was waited for; no GPU-wait sysctl was added (`swaps_a3_h<N>` counts flips of GPU-written A3 BOs, 0 with `rdna3a3display=0`). Open: (1) with one back buffer the zero-copy flip shows the buffer WindowServer draws into (`swaps_inplace_h<N>`): a copy into the front (what `submitSwapCopy` asks for) or a third buffer if the card shows it; (2) the latch wait sleeps in the swap under the family's lock (the family hands the old buffer back when `submitSwapCopy` returns), up to two OTG frames or the cap (2.5 frames, 35-100 ms) per swap, and two heads may take turns there and halve the frame rate (A/B with `swap_latch=0`, the card run measures it); the alternative is completing the swap's `IOAccelEvent` (submitSwapCopy's a0) from the flip-done instead of waiting (a later redesign); a swap waits once, after its own flip, measured from that flip (2.5 of the head's frames from its measured refresh, 35-100 ms; the next swap's entry only looks: review round 2 removed the entry wait, which a shared budget made unable to time out a stuck flip), 3 timeouts in a row turn a head's waits off until a flip is seen latched (logged both ways, `swaps_latchwaitsoff_h<N>`); with `swap_latch=0` the feature sleeps nowhere (the empty-back re-read too); ECC review round 1 fixed the backoff that never recovered, the entry wait ignoring `swap_latch=0`, the 60 Hz-only cap, the silent status, hash and flip failures and triage for archive-only reports; (3) a buffer freed while scanned (`swaps_shownfreed_h<N>`) is only counted; (4) the 320 of 1030 skipped swaps on head 0 (empty back): `swaps_emptyfilled_h<N>` says whether the frame lands late; (5) WindowServer crashed after the mode switch (plugin fence wait 5000 ms, GPU hang) and Terminal's VMID 5 TCP read faults stay open (N27 (3)); (6) the IOP flips (`rdna3accel_iop_flip=1`) do not update the latch record | accel, triage | M | diagnostic + fix, card check open |
| N29 | Card run 2026-10-17b (both screens mostly black, content now and then, everything flickers; the flip latch from N28 works: 0 pending, 0 timeouts): head 0's swaps name the framebuffer resource as the front and two backs in turns; the second back read empty at every swap and the family's aperture access (page-on) never named it; 85 of 229 shown frames rewritten in place; 14 + 10 buffers freed while scanned; at the mode switch the core refused VRAM (0xe00002db, the CPU-visible heap) and CoreDisplay switched copy forward off (0xe00002bc: the family's, the driver answers only success). Root model mismatch (H-COPY): `submitSwapCopy(event, front, back)` is a copy into the front, the back WindowServer's again on return; the driver flipped to the back. Done: swap mode 1 (default, `debug.rdna3accel_swap_mode` / `rdna3swapmode`; `common/rdna3_swap_copy.h`, `tests/test_swap_copy.c`): the back copied by SDMA into the scanout buffer the panel does not show (home or the accelerator's D), flipped to at vblank (tear-free), the back copied into the front too (copy forward reads it), a wait for the last flip's latch before the next copy; an empty back is never copied over a picture (`swaps_copyempty_h<N>`), mode 2 copies all, mode 0 flips; the per-frame trace (`common/rdna3_trace.h`, `tools/rdna3_trace.py`, accelerator + core rings, per-second table, streams in autoreport's 2 x 60 s swap A/B, triage hypotheses H-B2 / H-COPY / H-FREE / CF-ERROR, the A/B side by side and the Q1-Q11 self-check, AFTER-TESTS "Card run 2026-10-17b"); `dcn32_scan_addr` + `rdna3DisplayScanAddr` (the HUBP's in-use address per swap, sim-checked); the core traces RADV's submissions, fences, waits, resets and GPU faults. Open: (1) why WindowServer names a back it never draws (H-B2): the next trace's SCAN / APERTURE / SURFRES events and the swap timing say it; (2) VRAM refusals of the CPU-visible heap during mode switches make the family's copy forward fail (CoreDisplay disables it for the session): a larger BAR (Resizable BAR) or accelerator buffers outside the CPU-visible heap (N30: a reserve for the accelerator, GEM BOs kept out of it, `RESIZE_BAR_LOG2` for the VM); (3) D costs one scanout-sized CPU-visible allocation per head (else copies go into the buffer on the panel: `swaps_copydirect_h<N>`); (4) the mode 1 wait before a copy still sleeps under the family's lock (at most the latch cap, usually 0: it waits only when the next swap comes within a frame); (5) WindowServer's crash after a mode switch (plugin fence wait 5 s, GPU hang) and Terminal's VMID 5 TCP faults stay open (N27 (3)); the core trace (Q9) now times them; (6) `didSubmitSwap` / `isBackBufferReady` (family virtuals around the swap) are not traced: overriding them needs their base symbols exported on both releases. ECC review round 1 fixed: an SDMA breaker (3 failures or a timeout: the swaps flip until a retry 600 swaps later), D freed only once the flip away latched, the front record cleared on frees, panel moves outside the family's lock posted to the swaps, the latch backoff in modes 1/2, a flip home when leaving modes 1/2, chunked trace reads with the cursor moved after the copyout, the trace header v2 (wall clock, HDP note), APERTURE / PANEL / refused CS reasons, bounded buffer sampling, the stream pumped through the whole run into the archive with size sidecars, the decoder's integrity checks (version, truncation, corruption, stream gaps, nomem, salt) and the A/B validity checks. Still open from it: (7) the breaker falls back to mode 0's flip, not a CPU copy (a 1080p frame through the uncached BAR costs tens of ms): a CPU path for small damage only would need the damage rects, which submitSwapCopy does not get; (8) fixed in round 2 (the table has its own lock). ECC review round 2 (completeness, security, C++): the panel's black / not-black per pipe and look judged (A/B side by side), COPY-BLACK, the kernel's backs joined with the plugin's IOSurfaces (SURFRES at swaps, SCAN ids), the plugin ring 256 K via rdna3mtl.conf and its wrap judged against the A/B, old-boot traces refused, plugin trace MISSING with the collector's reason, CRITICAL is a FAIL in the checklist, snapshots after each A/B phase, the first stream read kept, the A/B's preconditions logged; stale posted panel moves ignored, D freed only after a latch seen, the breaker by time, SipHash object names, rate-limited refused-CS and cached-aperture events; the standalone report folder made root's own, pending copies 0600. Open: (9) only one display redraws in the A/B (Activity Monitor on the main display): a second head without swaps is reported as not judged; (10) the 256 K plugin ring applies to every process that loads the plugin (32 + 16 MB per trace file in /var/tmp/rdna3-nvmtl) and reaches WindowServer only from its next start, so the first run after installing this autoreport still has the 16 K ring (round 3: written on every VM boot including the install boot, so the card boot has it; traces of earlier boots are deleted at each run's start; a standalone run takes the knob out after its report and keeps the 16 K ring) | accel, kext, hw, tools, triage | L | done, card check open |
| N30 | Card run 2026-10-17c (N29 open item 2): the CPU-visible VRAM heap (BAR0 256 MiB in the VM = 144 MiB) filled with RADV's BOs the CPU may map, then WindowServer's 8 MB framebuffers were refused (0xe00002db) and the main display froze. Done (`common/rdna3_vram_place.h`, `tests/test_vram_place.c`, `tests/test_triage_vram.sh`): BOs without CPU access never take visible space (they fell back into it when the part beyond BAR0 refused them); a reserve at the start of the visible heap belongs to the accelerator's allocations (`RD3K_VRAM_FN_ALLOC`; default two thirds of the heap, at most 128 MiB, `rdna3visreserve=<MiB>`) and GEM BOs never take it; physical alignment capped at the BO's own power of two (amdgpu_vram_mgr_new); `range_alloc` no longer loses the tail of a free range when its list is full (1024 -> 4096 ranges, a get that needs a slot skips the range, a put with no slot is counted); per-owner occupancy (`accel`, `fb-surface`, `fb-cursor`, `swap-D` through `RD3K_VRAM_OWNER_*` in the request's flags, `gem`, `gem-beyond-BAR0`) in `debug.rdna3_vram`, at every refusal (`VRAM REFUSED #n`) and in a 30 s summary (`VRAM heap:`), triage `VISIBLE VRAM`; `rdna3vm`: `RESIZE_BAR_LOG2` / `PCI_MMIO64_MB` in the config (default stays 256 MiB: commit 0ae3df7 put the BARs below 4 GiB because macOS left the card unconfigured with the 64-bit window, 5c1f617 because a 16 GiB BAR showed no picture; 2 GiB is an untried opt-in, window sized 4 x). ECC review round 1: kernel-internal BOs (`RD_BO_FLAG_INTERNAL`: seq64, user queue objects) take the general part first and then the reserve, owner `kernel`; a refused free (`free_fail`), a retired range (`retired_*`), refusals before the heaps were asked (`refused_other`) and a client BO that found neither VRAM nor GTT (`gtt_fail`, `GEM: .. no VRAM and no GTT either`) have counters, logs and triage lines; the dump's truncation and a log newer than the dump are called out. Open: (1) the swap copy's D buffer (accel/rdna3-accel.cpp) still sends owner 0 (counted as `accel`): tag it `RD3K_VRAM_OWNER_SWAP_D` where it calls `askFramebufferForVram`; (2) RADV's cpu_accessible heap budget (`vis_vram_size - vis_vram_usage`) still counts the reserve as free; (3) the 3 allocators of the visible part are first fit: a long run can fragment the reserve (the report has `largest_*`); (4) which of the free-list loss and a 15 GiB BO took the 15.4 GiB from the part beyond BAR0 in the card run is told by the next run's `lost_kib` / `gem_hi_peak_kib` / `GEM: a N MiB VRAM BO` | kext, fb, common, uapi, tools, triage | M | done, card check open |
| N31 | Card run 2026-10-17c, the side monitor (windows appear and disappear): head 1's swaps came in pairs per frame - CF -> B1, swap B1 (the frame), swap B2 that WindowServer had not redrawn since a window animation put pixels in it once; mode 1 copied B2's old frame over every new one, for most of each frame period, and into the front (from where copy forward spread it). 259 such copies by the decoder's count (130 of 0x8d0c0000, 102 of 0x8d8b0000), none on head 0, 14 A-B-A on head 1's scanout samples; no GPU work pending. Done (evidence-driven, no amdgpu analogue): mode 1 holds a back with pixels whose first sample grid hashes as at its last presentation on the head, that no copy forward targeted since (marked in every head's table), when another buffer was presented after it and the panel holds a frame mode 1 copied - confirmed by a second grid shifted by half a cell (`rd3_sc_stale_check` / `_confirm`, `rd3_sc_sig64`, `rd3_sc_grid_index` in `common/rdna3_swap_copy.h`, 8 records per head under a spinlock; `tests/test_swap_copy.c` `stale_*`); mode 2 stays the copy-everything control. `debug.rdna3accel_swap_stale` / `rdna3swapstale` (default 1; 0 counts `swaps_stalehold_would_h<N>`, SWAP `v1` bit 24). Records forgotten at surface / pipe resource gone, display and swap mode changes, D's reallocation (VMALLOC flags 8 in the trace) and per address at VMFREE (`rd3_sc_stale_forget_addr`). Counted: `swaps_stalehold`, `_would`, `_miss`, `_changed` (a 4 ms re-sample after the hold), `swaps_stalenocf`, `swaps_stalecheck_{norecord,changed,cf,sameshown,nopanel,nohead}`, `swaps_stalereset`, `swaps_staleevict`, `swaps_stalegone` (all `_h<N>`), logged with reasons; decoder and triage: one H-STALE line per head (copies decoded, stale copied, held, A-B-A), FAIL a stale copy or a held real frame (another buffer new, or an aperture write into the held back), WARN frozen (holds >= half the swaps), counter vs trace mismatch, missing counters; the A-B-A line of its own (the kernel's flicker test does not cover the panel). ECC review round 1 fixed (switch, mode 1 only, second grid, reasons, resets / evictions / frees, one table function for VMFREE, the record head rule, the re-sample, decoder / triage diagnosability, parity tests). Open (needs the card): (1) the side monitor steady with `swaps_stalehold_h1` > 0 and no H-STALE FAIL; (2) the rule's assumption - WindowServer redraws a back only through copy forward: `swaps_stalenocf_h<N>`, `swaps_stalehold_miss_h<N>` and `swaps_stalehold_changed_h<N>` say how often it does not, and a redraw that both grids miss would still be held until the back's next swap; (3) the decoder compares RGB band bytes, the kernel 64-bit hashes with alpha: an alpha-only change reads "unchanged" to the decoder | accel, common, tools, triage | M | fixed, card check open |
| N32 | Card run 2026-10-17c: head 0's swaps named 0x8b100000 (IOSurface 37, type 0xc0) and 0x8b8f0000 (type 0x40) in turns; the second read empty at all 159 of its swaps, WindowServer's frame was in the first at all 159; mode 1 held each (437). The trace shows the empty back's swap 0.2-0.3 ms after the swap of the drawn one, which had not changed again at any of them: no frame was lost. Done (evidence-driven, no amdgpu analogue): modes 1/2 present the landed buffer (`rd3_sc_landed_wanted` / `rd3_sc_landed_pick` in `common/rdna3_swap_copy.h`, `tests/test_swap_copy.c`): at an empty back, or a type 0x40 back that did not change, the surface's other buffers sampled at that swap (at most 6; the trace's SCAN reads) and the one with new pixels that is not on the panel copied, an IOSurface-backed one first (`swaps_copylanded_h<N>`, SWAP `v1` bit 21 / `v4 >> 24`, the decoder's `presented the landed buffer`) - OFF by default (`debug.rdna3accel_swap_landed` / `rdna3swaplanded` = 0): it then only counts, traces and logs what it would have presented (`swaps_copylanded_would_h<N>`, SWAP `v1` bit 22, `would present landed buffer`); triage: H-B2 INFO `handled` when that happened, WARN `no frame lost` when every swap of the empty back was held with nothing new elsewhere, FAIL when a frame was held back or black shown; the A/B counts held / held with a new frame elsewhere / landed; `plugin trace MISSING` names a stale VM plugin as the reason. Open: (1) a buffer WindowServer is drawing the next frame into also reads "changed": presenting it would show a half-drawn frame (needs the card: the next report's `swaps_copylanded_would_h<N>` says whether it matters; then `rdna3swaplanded=1` and `swaps_copylanded_h<N>` against the looks); (2) the 64-sample band hash misses changes between the samples (a small redraw is held as before); (3) why WindowServer swaps the empty type 0x40 back at all is still the family's / WindowServer's (H-B2) | accel, common, tools, triage | M | done, card check open |
| N33 | The display bug (black screens, windows that do not appear or only flash, flicker; since card run 58) has several suspects with runtime switches and no way to tell them apart in one run. Done (`tools/vm/macos/autoreport.sh` `bisect_run`, `rdna3vm step bisect` / `bisect-plugin`, `tools/triage_bisect.py`, `tests/test_bisect_step.sh`, `tests/test_triage_bisect.sh`): the sysctls `a3`, `flicker`, `swap_latch`, `swap_torn`, `swap_mode`, `trace_scan`, `swap_stale` off together, then on one at a time (13 steps of 30 s, a notification, a spoken step number and N beeps per step, two scanout looks, the switches read back, the counters, the owner's y / n from a Terminal), controls for `swap_landed`, mode 2 and `a3ws`; the plugin run with `NVMTL_NO_SURFACE_SHARE` / `_PAGEOFF` / `NVMTL_TRACE` and the `%apps` line (WindowServer restarted three times). Open: (1) not switchable at runtime: the copy forward fixes of 2026-10-17 (`rdna3accelcf=0` is the whole feature), the visible-VRAM reserve (`rdna3visreserve`), gamma, the VMID admission, a3display (new display surfaces only: after a mode switch); (2) `flicker` (3000 hashed swaps per head) and `swap_torn` (20000 checks) stop by themselves, so their 'on' steps may do nothing late in a boot (`swaps_hashed_h<N>` says); (3) the plugin run needs somebody to log in after each WindowServer restart unless the guest has auto-login (`autologin:` in bisect.txt); (4) a run that points at one switch is a suspect, not a proof (the steps are cumulative); the card has not run it | tools, triage | M | done, card check open |
| N23 | User docs: FAQ / troubleshooting, supported monitors and features, `rdna3_pmctl`; ROADMAP.md stale; DRIVER-PLAN vs STATUS disagree on `dcn32_init_hw` / `dcn32_init_clocks` | docs | S | open |

Left out on purpose (project decisions): HDCP / secure display TAs,
eDP / PSR / ABM, USB-C / DPIA, KFD / ROCm, RAS / ECC, XGMI.

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

## Done 2026-10-12 (sim / host verified, CI on the self-hosted runners; card: step `big4`)

| Item | Commit | Notes |
|---|---|---|
| S3: DP SST heads restored and in standby through their link | `1347b00` | `rdna3dplink=1`; MST / ODM heads stay blank |
| S3: HPD looks paused for the sleep, `link_resume` on the wake | `85e00d3` | `rdna3hotplug=1` (now in `big4`) |
| S3 gap 7: a picture lost in VRAM makes WindowServer redraw | `23e2e5e` | display ABI 2 (`RD3KDisplayHotplug.lost`) |
| Kernel queues mapped by the MES scheduler from MES 0x47 on | `87f3fd8` | `rdna3amdgpu=1 rdna3mes=1`, KIQ fallback |
| `amdgpu_userq_active`; VCN functions VCN 4.0 never calls classified | `0d8a24f` | |
| DP link loss retrained (`dp_handle_link_loss`), DP displays blanked before a resume | `0d31090` | link status polled every 2 s in place of the HPD RX interrupt |
| Report triage for the sleep | `8ca16a4` | |

| DSC over MST: the DM fairness, the DSC aux of the port, DSC enable through REMOTE_DPCD_WRITE, the Synaptics hub workarounds, a DSC stream update without a mode set (`link_update_dsc_config`, `link_increase/reduce_mst_payload`); `amdgpu_ring_max_ibs` in AMDGPU_INFO_MAX_IBS | (this batch) | `rdna3mst=1 rdna3dsc=1`; without `rdna3dsc` MST plans as before. The last 28 amdgpu functions: the port is at 100 % |

| A2: swap copies onto a scanout by SDMA (`rdna3CopyVram`: HDP flush, COPY_LINEAR per row, one fence) instead of the CPU through the BAR, no 120-frame cap | (A2 batch) | `rdna3accelsdma=1`, step `accelsdma` and `big4`; three refusals in a row hand the swaps back to the CPU |

| V2: Vulkan CTS subset in the report (`tools/cts/patterns.txt`, ~2360 cases), `deqp-vk` + the Khronos loader built by build.yml with `cts` on the macOS runner | (V2 batch) | step `cts` after `rdna3vm cts` |

| `big4` also collects data for the card-only items (`rdna3probe=1 rdna3pmexp=1`): P2 variants with the RLC state, R3 / R4 platform, V3 WSI (`rdna3_vktest --wsi`), A3 IOSurface textures and M4 CAMetalLayer (`mtlprobe --iosurface` / `--layer`), M5 WindowServer and the plugin, M6 Chess and Maps | (probes batch) | how to read them: docs/AFTER-TESTS.md, "Probes of big4"; the host already shows V3's answer: RADV built with `-Dplatforms=` offers no surface or swapchain |

DSC over MST needs an MST hub with a DSC monitor behind it on the card: a
bench with one runs step `mst` with `rdna3dsc=1` and a mode beyond the
port's bandwidth (4K60 on an HBR2 x2 port).

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
   recorded and applied afterwards. A picture lost in VRAM (a wake or a
   mode-1 reset whose VRAM did not all come back) moves
   `RD3KDisplayHotplug.lost`; every framebuffer then raises its
   connect-change interrupt, so IOGraphics probes the head again and
   WindowServer draws everything again (`RDNA3,DisplayPictureLost`,
   `RDNA3,PictureLost` on the framebuffer).
8. **Hotplug across the sleep.** Done: the HPD looks stop before the sleep
   (`amdgpu_dm_irq_suspend`; after a power loss the pins read unplugged
   until they are enabled again), the wake work programs the HPD pins and
   filters again (`link_resume`) and the looks start after the longest
   connect filter; a monitor plugged or pulled while asleep counts as a
   hotplug then.
