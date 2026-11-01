# Testing on the bare-metal Mac from a second session (Raspberry Pi)

Since 2026-10-31 the driver runs on the owner's real PC (MSI B550-A PRO,
Ryzen 7 5800X, RX 7800 XT, two 1920x1080 Samsung C24F390 monitors), macOS
26.7.2 (25G332), SMBIOS MacPro7,1, dual boot with Linux. The main session runs
on that PC's Linux, so it is offline whenever macOS runs. A second Claude Code
session on the owner's Raspberry Pi 5 drives the Mac over SSH while it runs,
so a test round costs a Mac reboot at most, not a trip through Linux.

Division of work:

| Session | Does | Does not |
|---|---|---|
| Linux (main, on the PC) | driver code, reviews, CI, the USB stick generator (`tools/baremetal/`), DRIVER-PLAN | anything on the running Mac |
| Pi | installs CI builds on the Mac, switches sysctls, restarts WindowServer, collects reports, reads logs live, reports findings | edit driver code (kext/, fb/, accel/, agdc/, metal/, hw/, uapi/); push to stage1-kext |

The Pi session may commit notes under `docs/` (e.g. a dated section in this
file) and push them, after `git pull --rebase`. Reports never go into git.

## Reaching the Mac

- Remote Login is on (System Settings -> General -> Sharing); user `shrek`,
  the address from the owner. Key: `~/.ssh/mac_rdna3` on the Pi.
- Many steps need root. Ask the owner once for passwordless sudo for this
  user (`sudo visudo -f /etc/sudoers.d/rdna3`, line
  `shrek ALL=(ALL) NOPASSWD: ALL`); never type the owner's password yourself.
- The USB stick's EFI partition (OpenCore) mounts only with sudo:
  `sudo diskutil mount disk2s1` (check `diskutil list`: the 61.5 GB EFI
  partition labelled OPENCORE) -> `/Volumes/OPENCORE`.

## What is where on the Mac

- OpenCore (from the stick, not yet on the internal disk):
  `/Volumes/OPENCORE/EFI/OC/config.plist` is the active config. Copies:
  `config-driver.plist` (driver, core kext injected by OpenCore, no
  rdna3agdc), `config-metal.plist` (driver + `rdna3agdc=3`: WindowServer on
  the Metal plugin), `config-installer.plist` (no driver),
  `config-before-metal.plist` (what `metal.sh` saved). Switching = copy one
  over `config.plist`, then reboot the Mac.
- The core kext is OpenCore's: `/Volumes/OPENCORE/EFI/OC/Kexts/RDNA3Accel.kext`.
  The rest is in macOS: `/Library/Extensions/RDNA3{Framebuffer,Accelerator,AGDC}.kext`
  (and an unused RDNA3Accel copy), the Metal plugin
  `/Library/GPUBundles/RDNA3MTLDriver.bundle`, RADV and the translator in
  `/Library/GPUBundles/rdna3mtl/`, scripts and firmware in
  `/Library/Application Support/RDNA3/`.
- Stick helper scripts (`tools/baremetal/stick/`, copied by `make_usb.sh`):
  `/Volumes/OPENCORE/rdna3/install.sh` (driver from the release tarball:
  the `.pkg` refused Tahoe before 145ca65), `metal.sh` (plugin + RADV +
  `rdna3agdc=3` in config.plist), `report.sh` (report into
  `/Volumes/OPENCORE/rdna3/report-<date>/`: 3 h of logs, boot log, dmesg,
  ioreg, kexts, displays, rdna3 sysctls, crash and shutdown-stall reports).
  `report.sh` does NOT run `autoreport --standalone` (its display-sleep step
  crashed WindowServer without the plugin, fixed in f0789cb but keep it off
  until the plugin path is proven here).

## Installing a CI build without Linux

Artifacts of the Forgejo `build.yml` run (token `FORGEJO_TOKEN` in the Pi's
`.env`, API `https://forgejo.voten.me/api/v1/repos/Voten641/RDNA3-Hackintosh`,
`actions/artifacts?name=<name>` then `actions/artifacts/<id>/zip`):
`RDNA3-pkg` (the `.pkg` and the release tarball), `RDNA3Accel-kext` (all
kexts incl. the core), `RDNA3MTL` (plugin + translator, full runs only),
`radv-macos-logs` (`install/lib/libvulkan_radeon.dylib`, full or radv runs).
Use ONE run's artifacts together: the kexts, the core on the stick, the
plugin and RADV must come from the same commit (a mix ran on 2026-10-31).

1. scp the tarball to the Mac, `sudo sh install.sh <tarball>` (or put it next
   to `/Volumes/OPENCORE/rdna3/install.sh`).
2. Replace `/Volumes/OPENCORE/EFI/OC/Kexts/RDNA3Accel.kext` with the
   artifact's (keep the old one under `EFI/OC/Kexts-old/`).
3. Plugin + RADV: put `RDNA3MTLDriver.bundle`, `librdna3mtl_translate.dylib`,
   `libvulkan_radeon.dylib`, `rdna3mtl-allow.txt` (repo `metal/`) into
   `/Volumes/OPENCORE/rdna3/metal/`, `sudo sh /Volumes/OPENCORE/rdna3/metal.sh`.
   A plugin-only update needs no reboot: log out and in, or
   `sudo killall -HUP WindowServer` (the owner logs in again).
4. New kexts in /Library/Extensions need System Settings -> Privacy &
   Security -> Allow on the screen (the owner), then a reboot.

Live switches (no reboot), read back with `sysctl -a | grep rdna3`:
`debug.rdna3accel_swap_gen`, `debug.rdna3accel_swap_gen_cpu`,
`debug.rdna3accel_swap_stale`, `debug.rdna3accel_swap_mode`,
`debug.rdna3accel_flipbuf_present`, `debug.rdna3accelfb` (see
docs/INSTALL-OPENCORE.md section 7 and DRIVER-PLAN N65).

## The first round (build 6f55f46, CI run 91)

What 124d309 + 6f55f46 changed and what the run must show (DRIVER-PLAN N65,
docs/AFTER-TESTS.md "Bare metal: two heads, 10-bit surfaces, the framebuffer
before the core"):

1. `config-driver` (no plugin): Terminal text appears while typing (was
   held: `swaps_genhold_h0` 805/1980); `debug.rdna3accel_swap_gen_wsmetal`
   0, `swaps_gencpu_h<N>` counting; no `flip ... REFUSED`. A/B:
   `sudo sysctl debug.rdna3accel_swap_gen_cpu=0` must bring the stale text
   back (proves the rule); watch for A/B/A/B flicker (`swaps_abab_h<N>`).
2. Display sleep without the plugin (`pmset displaysleepnow`, wake with the
   mouse): no WindowServer crash (`/Library/Logs/DiagnosticReports`),
   `vm_pageoff_sys` counts, `vm_pageoff_sys_fail_*` stay 0.
3. `config-metal` after `metal.sh`: the desktop comes up on the plugin (was:
   the boot logo for ever, every transaction 0xe00002c7 because WindowServer
   presents 'l10r' 10-bit surfaces); `iop_copy_fmt_l10r` == `_seen`,
   `iop_copy_us_h<N>` per head (the CPU conversion's real cost), smoothness
   by eye (the owner) and by the frame counters.
4. Then the owner's own use: apps, Geekbench 6 Metal (Linux Vulkan baseline
   145267).

Report to the owner in Polish, briefly: what was tested, what worked, what
did not, with the counter or log line that shows it. Findings for the Linux
session go into a dated section at the end of this file (pushed), with the
report folder's name on the stick (`/Volumes/OPENCORE/rdna3/report-*`), which
the Linux session reads from the stick or the Pi.

## Known open items (2026-10-31)

- Safe Mode (Shift+Enter in the picker) shows the prohibited sign; the core
  stands down on `-x` since 7821cae, but the boot itself fails before it.
- `tools/triage_reports.py` reads the VM report tarballs, not the stick
  report's layout: read `sysctl.txt`, `dmesg.txt`, `rdna3-3h.log` directly.
- DCN scanout of 10-bit surfaces (N66) instead of the CPU conversion.
- The CI `macos-tests` job has failed since run 86 (not investigated yet).

## Round 1 findings (2026-10-31, Pi session, build 6f55f46, CI run 91)

Installed from run 91's artifacts only (tarball via `install.sh`, the core
`RDNA3Accel.kext` on the stick, plugin + translator + RADV via `metal.sh`).
Old core kept as `EFI/OC/Kexts-old/RDNA3Accel.kext-pre6f55f46-*`. Reports on
the stick: `rdna3/report-20261031-162542` (config-driver, after the hang) and
`rdna3/report-20261031-163520` (config-metal).

### config-driver (no plugin): broken in three ways

1. **WindowServer watchdog kill when a window is dragged** (16:24:11,
   `WindowServer-2026-10-31-162411.ips` + `.userspace_watchdog_timeout.spin`):
   main thread waited 40 s on a `CompositorSW::CompositeLayersToDestination`
   worker that was in `CA::gaussian_blur` / `box_filter_8`
   (CA::OGL::render_layers, CPU only; the kernel frames there were interrupts,
   no thread in our kexts). The software compositor's blur (vibrancy, window
   backgrounds) is too slow on the CPU. `reduceTransparency` set to true for
   user shrek as a workaround (`defaults write com.apple.universalaccess
   reduceTransparency -bool true`; still set).
2. **`swap_gen_cpu=1` (default) shows a stale frame**: the screen stayed on
   one old frame, a window drag showed the new one for a fraction of a second,
   then the old came back. `swaps_abab_h0` 1 -> 10, `h1` 0 -> 9,
   `ababwide_h<N>` 3, `swaps_gencpu_h0` 357. This is N65 open item (c).
3. **`swap_gen_cpu=0` (live)**: no A/B any more (abab unchanged), the menu-bar
   clock updates, but Terminal text waits for a window move: 122 holds in
   ~1 min, **all `genhold_older_h0`, `genhold_same` 0**. So the backs judged
   OLDER sometimes carry the newest CPU-drawn content and sometimes are really
   stale: the write generation alone cannot tell them apart without the
   plugin. Needs a different criterion (content, or which back WindowServer
   last drew into) - Linux session.
4. Display sleep (`pmset displaysleepnow`): WindowServer survived (same pid),
   `vm_pageoff_sys` 2, `vm_pageoff_sys_us` 5598, every `_fail_*` 0 (P4 works).
   But **moving the mouse did not wake the displays** (IODisplayWrangler stayed
   1/4); `caffeinate -u` woke them at once (4/4 USEABLE, gamma reprogrammed on
   both MPCCs). HID wake path or USB (dmesg has `IOHIDInterface ...
   SetElementValues failed: 0xe0005000` / `Set LED` failures) - not checked.

### config-metal (`rdna3agdc=3`, the plugin): works

- The desktop comes up on both heads and refreshes by itself; the owner: a
  YouTube video plays much smoother than without the plugin (~3 fps before).
- `iop_copy_fmt_l10r` 367 == `iop_copy_fmt_l10r_seen` 367, `_fmt_other` 0,
  `iop_flip_fmt` 0, `iop_copy_sdma_fb_format` 367 (SDMA skipped for l10r as
  designed), no `REFUSED` / `not BGRA` in dmesg, `swaps_abab_*` 0,
  `genhold_*` 0, no new WindowServer crash.
- CPU conversion cost: `iop_copy_us_h0` 5954923 / 500 copies (avg 11.9 ms,
  dominated by the first frames after boot: the next 4 copies added 1.3 ms in
  total), `iop_copy_us_h1` 5110992 / 1978, then +607092 us for +1848 copies =
  **0.33 ms per frame** in steady state on head 1. N66 is not urgent for
  speed; measure h0 again under load.
- `swap_gen_wsmetal` stays 0 with the plugin and `swaps_h<N>` /
  `hook_didsubmitswap` 0: WindowServer presents through IOPresentment only,
  the swap path is unused here.
- VRAM heap (dmesg 101 s): visible 147456 KiB, free 21260 + 924 KiB, `gem`
  refused 1, surfgem `refused 2149` (ok 8).

### For the Linux session

- (no plugin) replace / refine the CPU write-generation rule (3 above); the
  blur hang (1) makes the no-plugin path unusable for window drags even with
  a correct rule unless reduceTransparency stays on.
- the mouse not waking the displays (4).
- `surfgem refused 2149` and the nearly full visible VRAM heap with the plugin.
- Geekbench 7.0.0 (the owner's Linux baseline is GB7) is in
  /Applications; its free CLI has only `--cpu`, so the GPU (Metal) run is
  the owner's, from the app.

### GPU checks on config-metal (run 91's userspace tools, over SSH)

- `rdna3_drminfo`: amdgpu 3.61.0, 0x747e rev 0xc8, 60 CUs, GTT and VRAM BO
  write/read-back ok, "Stage 4 UAPI smoke test passed".
- `mtlprobe`: one Metal device "AMD Radeon RX 7800 XT" (NVMTLDevice),
  families Common1-3 Mac2 Metal3, recommendedMaxWorkingSetSize 11148 MiB,
  compute kernel and rendered triangle correct ("METAL: ALL PASSED").
- `rdna3_vktest /Library/GPUBundles/rdna3mtl/libvulkan_radeon.dylib`:
  "ALL PASSED: 14 submit(s) tested" (all queue families, host-pointer import,
  OPAQUE_FD, DRM-modifier images, SDMA tiled copies, timeline semaphore,
  mutable descriptors).
- The tools need `libdrm.2.dylib` / `libdrm_amdgpu.1.dylib` next to them;
  the artifact ships them as `libdrm.dylib` / `libdrm_amdgpu.dylib` (symlinks
  made by hand).

### Smoothness and Geekbench 7 (config-metal, 17:00-17:25)

- The owner sees ~20-30 fps everywhere (window drags, animations, YouTube).
  The driver side measures a steady 60 Hz: `iop_ok_h1` sampled every ~1.5 ms
  for 3 s rose 180 times, every gap 16.7 ms; during a window drag 479 presents
  in 8 s (378 gaps ~17 ms, one 50 ms gap). SDMA copy 0.32 ms per frame
  (max 3 ms), vblank IRQs ~60/s per head (`virq_lat_avg_us` 6), the plugin's
  `ws frame` ~120/s for both heads (rec 1.2 ms CPU, 0.5 ms GPU per frame).
  So if the eye sees 30 fps, WindowServer presents repeated content: not
  yet explained. Next checks: whether consecutive presented surfaces differ
  (content duplicates), the cursor path, and N59 (frames stepping back).
- testufo.com in Safari shows **SYNC FAILURE** (no frame-rate reading): Safari
  is `-Safari` in the allow list (N1, WebKit.GPU sandbox), so WebKit renders
  on the CPU (two WebKit processes at 54% + 39% CPU) - Safari and YouTube in
  Safari say nothing about the plugin's speed.
- **Geekbench 7.0.0 GPU (Metal) does not run**:
  1. The benchmark worker is `Geekbench 7.app/Contents/Resources/geekbench_avx2`,
     not in Contents/MacOS, so `%apps` refuses it ("not allow-listed: no
     device") and the run ends at once. Added `geekbench_avx2` to
     `/Library/GPUBundles/rdna3mtl-allow.txt` on the Mac by hand (backup
     `/private/tmp/rdna3mtl-allow.txt.bak`); the repo's `metal/rdna3mtl-allow.txt`
     should get it too (or `%apps` should cover executables under
     Contents/Resources of an app).
  2. Then it crashes in MPS: `MPSNDArrayConvolutionDeviceBehaviorAMD::
     EncodeNDArrayConvolution` -> `MPSLibrary::MPSKey_Compile`; plugin log:
     `link FAILED: "AMDConvolution::ndArrayConvolution2DAMD_NHWC" ... native
     emitter: unsupported Metal visible function reference; dynamic linked
     functions are not expressible in Logical SPIR-V`, then MPSLibrary.mm
     line 639 error. The translator needs visible-function / linked-function
     support (inline the 10 linked functions at pipeline creation) before
     Geekbench's GPU run (MPS NDArray convolution) can work.
     Crash `~/Library/Logs/DiagnosticReports/geekbench_avx2-2026-10-31-172324.ips`
     and the log stream are on the Pi in `~/rdna3-bm/gb/`.
- `rdna3_drminfo` crashed once (16:42, before the libdrm symlinks: dyld).
- **What the panel shows changes at 60 Hz** during a window drag on head 0:
  `debug.rdna3accel_crc` (CRC32 of the head's scanout, `crc_head=0`) sampled
  every ~2 ms for 7 s changed 421 times (60.1/s), 419 gaps ~17 ms, presents
  420 (60.0/s): every present carries new pixels. The mouse (SteelSeries
  Rival 3 Wireless Gen 2, USB, ReportInterval 1000) delivered ~988 reports/s
  (`InputReportCount` 139436 -> 146084 in 6.7 s). Nothing measured explains
  the owner's ~30 fps impression; left: latency (the hardware cursor moves
  at once, the window trails it by the WindowServer -> plugin -> copy ->
  flip chain) or uneven step sizes. Next: a 240 fps phone video of a drag
  (cursor and window), cut into frames.

### The 240 fps drag videos (Linux session, 2026-10-31)

`IMG_7132.MOV` (slow drag) and `IMG_7133.MOV` (fast drag), 240 fps, the
Terminal window tracked frame by frame against the static Dock (camera shake
removed; the script stayed in the Linux session's scratchpad):

- The window moves on the panel's 60 Hz grid: in the slow drag 196 of the
  gaps between position changes are exactly 4 camera frames (one refresh);
  only 18 of 342 refreshes repeat the previous position.
- **The steps are uneven.** Fast drag, the window's movement per 60 Hz
  refresh (px at 480-wide scale): `10 1 8 10 4 8 11 0 21 9 44 10 12 11 19 4
  13 5 19 19 46 17 ...` (median 13, coefficient of variation 0.68). Small
  steps followed by catch-up jumps of 3-4x read as ~30 fps judder, which is
  what the owner sees, although every refresh carries new pixels (the CRC
  measurement above).
- So WindowServer's composited frame lags the mouse by a varying amount:
  some presents carry a frame composited from an older mouse position. Next
  for the Pi session, during fast drags: per-head `iop_*` wait/stale
  counters and the plugin's `ws frame` timing (does a frame's GPU work miss
  the refresh and the IOP copy take the previous surface?), A/B with
  `debug.rdna3accel_iop_sdma=0` (CPU copy) and with the plugin's surface
  wait off, each with a new 240 fps video.

## Round 2 plan (Linux session, 2026-11-01): build fe04d3c

What changed since round 1 (all merged in `stage1-kext` fe04d3c; take every
artifact from the ONE full `build.yml` run on fe04d3c: `RDNA3-pkg` tarball,
`RDNA3Accel-kext` core for the stick, `RDNA3MTL`, `radv-macos-logs`):

- N68: the window-drag judder. Instrumentation K1-K4 (`iop_txn_*`, `iop_vbl_*`,
  `iop_txn_overlap*`, trace event `RD3_TR_IOP`, plugin `held_us_max`) and an
  opt-in latched copy: `sudo sysctl -w debug.rdna3accel_iop_latch=1` (live,
  default 0; boot-arg `rdna3ioplatch`). Details and the expected numbers:
  docs/AFTER-TESTS.md "Presents at 60 Hz but a dragged window steps unevenly".
- N67: the Metal translator links visible functions (Geekbench 7 GPU / MPS).
- `metal/rdna3mtl-allow.txt` carries `geekbench_avx2`.
- `report.sh` now also saves `trace-stream.bin`, `trace-sec.bin`,
  `31-plugin-trace/` and dmesg after `debug.rdna3accel_trace_dump`; copy the
  new `tools/baremetal/stick/report.sh` to `/Volumes/OPENCORE/rdna3/`.

Checks, in this order (one Mac boot if possible):

1. **Judder A/B** (config-metal). Fast window drag for ~8 s, reading the
   counters listed in AFTER-TESTS N68 before and after each, with a 240 fps
   phone video each (the owner; videos land in Linux `~/Downloads`):
   a. `iop_latch=0` (baseline, measures the cause: `iop_vbl_same/skip/mid`).
   b. `iop_latch=1` (the fix: `iop_latch_n` rises with `swaps_pq_latched`,
      `iop_latch_fb_*` stay 0, `same`/`skip` near 0).
   c. optional: `debug.rdna3accel_iop_stale_wait_ms=0` with the better of a/b.
   Then `report.sh`, and the join:
   `python3 tools/plugin_trace.py <report>/31-plugin-trace --kernel <report>/trace-stream.bin`
   (present age per frame). The owner says which run felt smooth.
2. **Geekbench 7 GPU (Metal)** from the app (the owner). Plugin log lines to
   find (`log show --last 10m --predicate 'eventMessage CONTAINS "link:"'`):
   `link: "AMDConvolution::ndArrayConvolution2DAMD_NHWC" visible functions:
   linked N ...` and no MPSLibrary.mm:639 error. If it still fails: set
   `NVMTL_LINK_DUMP` and bring back the `.kernel.ll`, `.linked.ll`, `mod.*.ll`,
   `.err.txt` files (DRIVER-PLAN N67). Score vs the Linux baseline
   (Geekbench 6 Vulkan 145267; ask the owner for the GB7 Linux number).
3. **Mouse does not wake the displays.** After `pmset displaysleepnow`: press
   a key, click, and move the mouse, each on its own, and after each read
   `ioreg -r -c IOHIDSystem -d1 | grep HIDIdleTime`,
   `ioreg -r -c IODisplayWrangler | grep CurrentPowerState`, `pmset -g log |
   tail`, `log show --last 2m --predicate 'process=="hidd" OR
   process=="powerd"'`; repeat with another USB mouse or port. If HIDIdleTime
   resets but the wrangler stays at 1, report it: the suspect is
   fb/rdna3-fb.cpp swallowing `kIOWindowServerActiveAttribute`.
4. **Bigger BAR0** (visible VRAM heap was 21 of 144 MiB free). The owner sets
   BIOS Settings -> Advanced -> PCI Subsystem Settings -> Re-Size BAR Support:
   Enabled (Above 4G stays on). In `/Volumes/OPENCORE/EFI/OC/config.plist`
   set `Booter/Quirks/ResizeAppleGpuBars` to 10 (1 GiB); reboot; check the
   dmesg line `VRAM aperture: BAR0 ... MiB, mapped ...` and the heap numbers.
   If macOS does not come up or BAR0 is not mapped, use 9 (512 MiB, proven in
   the VM, N62). Linux keeps its 16 GiB BAR (the quirk is macOS-only).
5. Note: the run's userspace tools need `libdrm.2.dylib` /
   `libdrm_amdgpu.1.dylib` next to them (the artifact ships `libdrm.dylib` /
   `libdrm_amdgpu.dylib`): make the symlinks as in round 1 (not fixed yet).

Write the findings as "## Round 2 findings" below, push to Forgejo.

## Round 2 findings (2026-10-31 evening, Pi session; build bf2c3fa = fe04d3c + docs, CI run 103)

Run 102 (fe04d3c) was incomplete (macos-tests log cut mid-build, radv-macos skipped: a
Forgejo outage); run 103 on bf2c3fa (same code) is green and is what was installed.
Install note: the stick's `install.sh` takes the tarball NEXT TO IT and ignores an
argument; move the old tarball to `rdna3/old/` first (the first attempt reinstalled
6f55f46). Copies: `~/rdna3-bm/` on the Pi (r2-*, report-20261031-211339/, r2-gb/).

1. **Judder A/B, phase a only (`iop_latch=0`)**, video `IMG_7135.MOV` (Pi `~/`):
   during the drag every copy went by SDMA, 0.33 ms a transaction, stale wait 0;
   `iop_vbl_same` 13 of 1529, `mid` 1434 of 1529 (copies end mid-scan: torn,
   into the live scanout). The IOP join (report-20261031-211339): the 195 joined
   frames had present age median 0.4 ms (max 0.5): the scanout is not late; the
   plugin trace ring wrapped (only 195 of 2596 joined: read the report right
   after a drag). Before a0 (login): 511 l10r frames converted by the CPU at
   27 ms each. Phase b (`iop_latch=1`) NOT run yet (the owner moved to the game).
2. **Geekbench 7 GPU**: N67 works (`link: "AMDConvolution::ndArrayConvolution2DAMD_NHWC"
   visible functions: linked 2 ... unresolved 0`, translated). It now fails one
   kernel later: `link FAILED: "depthwiseConv_3x3" linked (676122 B AIR) but did
   not translate: native emitter: Private pointer phi %metal2vulkan.ct.slot.48 has
   differing structural sources; raw construction failed: native emitter: pointer
   merge storage mismatch Function vs Private at Local("%1425") across
   [Local("%72"), Local("%1425")]`, after 297-311 s of translation (spindump: the
   translator hashing/cloning Strings), then MPSLibrary.mm:639 and the abort.
   `NVMTL_LINK_DUMP` dump (kernel.ll, linked.ll, mod.*.ll, err.txt, fc.txt,
   stitch-*.mtlb) + the .ips: Pi `~/rdna3-bm/r2-gb/gb-linkdump.tgz`.
3. **Horizon (a wgpu Minecraft client)**: `horizon-client` lives in
   `~/Library/Application Support/horizon-launcher/versions/.../`, not in an
   .app, so `%apps` gave it no device ("not allow-listed: no device") and it
   quit in 0.3 s with no .ips. `horizon-client` was added by hand to
   `/Library/GPUBundles/rdna3mtl-allow.txt`; add it to `metal/rdna3mtl-allow.txt`.
   It then renders correctly (the owner: no artefacts), ~180 fps where Linux
   gives ~2000. Found (DTrace, passtime, plugin fps log): 13 commits and ~5
   copy-outs of the 1920x1080 drawable (8.3 MB into the IOSurface's host pages,
   0.44 ms GPU each) per frame; render passes 69-75 us. Fixed by **N69**
   (merge 243bc28, CI run 107): a presented drawable is copied once, by the
   presenting buffer. Measured after: copy-outs 702 for 703 presents, ~230-300
   fps. Left: ~0.8 ms GPU a frame but 4.3 ms a frame; the plugin's execution
   thread waits on a fence after each of ~6 submits a frame (gpuwait 30-60 %),
   ~13 commits a frame; `[CAMetalLayer nextDrawable]` 2-4 ms waits (DTrace).
   rdna3_pmctl reads gfx activity 100 % at 2229 MHz even at 0.8 ms GPU a frame
   (the SMU's activity is not a GPU-time measure here).
4. **Live plugin updates: never `cp` over a mapped dylib.** Copying the new
   `librdna3mtl_translate.dylib` over the old file (same inode) left the
   kernel's cached code signature of the old one: every process that mapped it
   afterwards got "code identity has been invalidated" (`codesign -v <pid>`),
   securityd then "suppressing keychain prompt for invalidly signed client" and
   the Horizon launcher could not read its Microsoft token
   (CSSMERR_CSP_OPERATION_AUTH_DENIED, "platform secure storage failure ...
   already exists in keychain"). `rm` first, then copy (new inode). metal.sh
   also copies in place but is followed by a reboot.
5. Not run yet: judder phase b/c, mouse wake (3), bigger BAR0 (4).
6. **N70 (submit ahead past an empty last part), branch quick/gsub-last 4436584, NOT merged.**
   It works mechanically (horizon-client, 3 s: 2910 submits ahead, 600 in turn; was 35 / 3396; the
   plugin's execution thread CPU 2.6 s -> 0.68 s per 4 s) but presented frames stayed ~185/s, A/B by
   NVMTL_GSUB_LAST_EMPTY=0 (the owner's in-game counter read 220 on, 300 off: noise of the view). The
   game's main thread waits 50-60 % in `[CAMetalLayer nextDrawable]`; drawables come back by
   WindowServer's `_XImageQueueCollectable` mach message (DTrace stack). Reviewed (code-reviewer: nothing
   confirmed). CI run 109: host-tests failed in test_baremetal "a damaged copy was not caught"
   (unrelated, passes 3/3 on the Pi with bash: a flaky test).
7. **WindowServer and the game, numbers** (before the BAR change): WS commits all 500 of its command
   buffers in turn (`commitAndWaitUntilSubmitted` -> the plugin's waitUntilCompleted, 366 calls / 3 s,
   0.6-1 ms each: CoreDisplay AccessComplete, CompositorMetal MetalContext::Flush); ws_main_thread 40 %
   CPU, half its samples idle in mach_msg; 17 % in nvmtlSurfaceIn (the game's surface uploaded by the
   CPU, 167/s, 377 MB/s). IOP transactions on the game's head ~134/s (more than the refresh). With
   `debug.rdna3accel_iop_latch=1` NOTHING latched: `iop_latch_fb_vram` 791 (the visible heap had no
   8 MiB), set back to 0.
8. **Bigger BAR0 (plan item 4) done**: BIOS Re-Size BAR Support Enabled (the owner),
   `Booter/Quirks/ResizeAppleGpuBars` 10 in `EFI/OC/config.plist` (old file `config-before-bar.plist`;
   config-metal.plist / config-driver.plist still -1). macOS came up; `debug.rdna3_vram visible_kib=933888`
   (was 147456), refused 0. **But the game got slower** (~123-145 presented fps, WindowServer 187 % CPU):
   with room in the visible heap the game's drawables became VRAM surfaces ("item 7", `_vramOn`, copy-out
   "dirty, no copy"), and WindowServer's import of the family VRAM is refused by the kernel:
   `item 7: surface N plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 1 Operation
   not permitted, 8294400 B) - private image + copies` (WS's nvmtl.log; it stops logging after a few), so
   WS reads every frame through the BAR with the CPU (sin_us ~57 % of its thread, ~4.5 ms a frame,
   locked_skip_vram 0). Also after the boot WS's `com.apple.coreanimation.pipelines` thread ran 100 %
   translating CA's shaders (nvmtl_translate_with_fc) for minutes. **Workaround now (not persistent):**
   `launchctl setenv NVMTL_NO_SURFACE_SHARE 1` in the owner's session -> drawables private again, N61 DMA
   copy-out, WS 80 % CPU, ~215 presented fps (the owner's counter 300-400). **The fix is in the kernel:**
   why the family-VRAM import (nvmtl_vk_surface_vram -> libdrm_xnu surface fd -> the accel's surface/GEM
   import) gives EPERM to WindowServer for a surface another process placed in VRAM; until then a VRAM
   surface whose consumer cannot import it is slower than a private one.
9. Mac state at 23:35: plugin = N69 build (run 107), RADV/kexts run 103, BAR 1 GiB, iop_latch 0,
   NVMTL_NO_SURFACE_SHARE=1 set by launchctl (gone at logout), horizon-client in the allow list,
   reduceTransparency on.
10. **The EPERM is the A3 policy, not a bug** (kernel log): `surfgem: surface N refused: a WindowServer
    input ... its writers ... are not ordered with the VRAM WindowServer would sample (card run 58)` and
    `GEM: IOSurface N import refused: kept out of A3: a WindowServer input (rdna3a3ws=1 lets it in)`
    (`debug.rdna3accel_a3ws_refused` 18230). Live A/B `debug.rdna3accel_a3ws=1` + no
    NVMTL_NO_SURFACE_SHARE: WS imported some family VRAM (`-> the family's VRAM, already current (no
    upload)`, WS CPU 95 %) but the game still ~150 presented fps: WS keeps uploading the game's frames
    (locked_upload ~128/s, locked_skip_vram did not move), 29 % of ws_main_thread in IOSurfaceClientLock
    (the lock of a VRAM surface) and it takes the game's frames by `MetalCompositeDetachedIOSurface`
    with a `newTextureWithDescriptor:iosurface:` per frame. Set back to 0. Best config tonight:
    a3ws 0 + NVMTL_NO_SURFACE_SHARE=1 in the session (`launchctl setenv`, gone at logout; a
    LaunchAgent for it was not made: the owner decides). Next for N71: (a) the plugin should not put an
    app's drawable in family VRAM while WindowServer cannot import it (a3ws 0), so the BAR change needs
    no env; (b) WindowServer's detached-surface path: a texture per frame + a CPU lock + upload of a
    surface it already has (cache by IOSurface ID + seed).
11. **N71 (a) merged (9c16a43, CI run 111 metal green)**: with BAR0 1 GiB and NO env, horizon-client
    300-400 fps (owner's counter), 232 presented/s, copy-outs by the GPU 695/695, WindowServer 73 % CPU.
    The NVMTL_NO_SURFACE_SHARE session env was removed. Mac state now: plugin run 111 (N69 + N71; N70
    not in it), RADV/kexts run 103, BAR 1 GiB (`config.plist` ResizeAppleGpuBars 10), a3ws 0, iop_latch 0.
    Flaky host tests on the macOS-side CI: test_baremetal "a damaged copy was not caught" (run 109),
    test_bootwatch "bash late: no snapshot" (run 111); both pass on the Pi.
    Still open from round 2: judder 1b/1c (iop_latch=1 now has VRAM room: retest), mouse wake (3);
    the game's remaining cap is nextDrawable waits (~50-60 %, drawables handed back by WindowServer).
12. **The owner (2026-11-01 00:0x): "okna chodza plynnie, wszystko jest teraz plynne"** (windows move smoothly,
    everything is smooth), and corrected: it was smooth BEFORE the bigger BAR too, i.e. already on this
    round's build (bf2c3fa, CI run 103) with iop_latch 0 - the round-1 complaint (~20-30 fps feel, N68) did
    not come back in round 2. What fixed it is not pinned (round 1 ran 6f55f46); not yet measured by a
    240 fps video. The BAR change is still worth keeping (no VRAM refusals, room for iop_latch); make
    ResizeAppleGpuBars 10 the default in the stick's config-*.plist (config-metal.plist /
    config-driver.plist still -1; only config.plist has 10).

## Round 3 checks for N73 (the game's nextDrawable wait; Linux session, 2026-11-01)

Install the plugin with N73 (CI `full: true`), reboot or log out (WindowServer reads rdna3mtl.conf at its start).

1. horizon-client in a world, 30 s, VSync off; then `sudo sh /Volumes/OPENCORE/rdna3/report.sh` and
   `grep "vk: drawable: SUMMARY" rdna3-3h.log | tail` (or `python3 tools/triage_present_cost.py sysctl.txt rdna3-3h.log`).
   The game's line: `next_pct` (share of its wall in nextDrawable), `pres_us_avg` (commit -> present, ours in the app),
   `away_us_avg` (present -> the same surface back: WindowServer's hold). WindowServer's line: `st_new` (new 8 MB images:
   with `keep=8` a few per surface, not one per frame), `st_alias`, `st_evict`, `st_new_us_avg` (what an image costs).
2. A/B: `NVMTL_SURFTEX_KEEP=0` in `/Library/GPUBundles/rdna3mtl/rdna3mtl.conf`, log out and in, the same 30 s; compare
   presented fps (DTrace `presentDrawable` count or the plugin's `fps:` lines), `away_us_avg`, WindowServer CPU. Remove the line.
3. Also record WindowServer's `vk: ws frame: SUMMARY` (up_us_avg = the CPU upload of the game's frame, wait_us_avg) and the
   game's `nvmtl-fps-<pid>.log` (gpuwait, submits/frame) for the same window: they say which half to cut next (DRIVER-PLAN N73 OPEN).
4. Desktop sanity with the keep ring on: drag windows, resize the game's window, close it; no stale or wrong window contents
   (a reused image keeps the seed check; a wrong picture = set NVMTL_SURFTEX_KEEP=0 and report).

## Round 3 plan (Linux session, 2026-11-01): build 2a0525d

All from the ONE green full `build.yml` run on 41ca260 or later (run 115 on
2a0525d failed only a flaky host test, fixed in 41ca260; the driver code is
the same). Kexts are unchanged since round 2, but take the core, tarball, `RDNA3MTL` and `radv-macos-logs` from
the same run). Plugin installs: `rm` the old dylib/bundle first, never `cp`
over a mapped file (round-2 lesson); log out after installing the plugin.

New since round 2:
- N73 (merged, CI run 113 green): WindowServer keeps IOSurface texture images
  (`NVMTL_SURFTEX_KEEP`, 8 in WindowServer) instead of rebuilding an 8 MB
  image and re-uploading it every frame; `vk: drawable: SUMMARY` lines with
  the apps' nextDrawable wait; triage row `drawable`. Checks: the section
  "Round 3 checks for N73" above (horizon-client 30 s, A/B with
  `NVMTL_SURFTEX_KEEP=0`, `ws frame` + `nvmtl-fps-<pid>.log`, desktop sanity).
- N72 (merged): the translator splits a pointer phi that mixes Function and
  Private storage and is ~2x faster on large kernels; translator notes now
  reach the release syslog as `translator note: <fn>: ...`. Check: Geekbench
  7 GPU from the app; look for `translator note: depthwiseConv_3x3: ...
  demoted to a per-storage value select (DRIVER-PLAN N72)`, the link time,
  and the next `link FAILED` line if any (bring back `NVMTL_LINK_DUMP` files
  to the Pi if it still fails). Score vs Linux (ask the owner for GB7 Linux).
- `metal/rdna3mtl-allow.txt` now carries `horizon-client` and `geekbench_avx2`.

Also in this round:
1. Set `Booter/Quirks/ResizeAppleGpuBars` to 10 in `config-metal.plist` and
   `config-driver.plist` on the stick too (only config.plist has it).
2. Mouse wake (round-2 plan item 3, still not done).
3. One 240 fps video of a fast window drag on the desktop (confirms the
   round-2 "smooth" by numbers; the Linux session analyses it).
4. N70 (branch quick/gsub-last, unmerged): decide keep or drop from the N73
   numbers (`pres_us_avg` large -> the app's execution thread, N70 matters).

Write "## Round 3 findings" below and push to Forgejo.

## Round 3 findings (2026-11-01 afternoon, Pi session; build 02b2210, CI run 117)

Installed from run 117 (all jobs green): tarball, RDNA3MTL, radv-macos-logs; the stick's
`install.sh` / `metal.sh` replaced by the repo's (the stick had the round-1 copies) and
`metal/rdna3mtl-allow.txt` from the repo (adds `geekbench_avx2`); round-2 files in
`rdna3/old/`. `install.sh` recreates `/Library/GPUBundles/rdna3mtl` (no conf survives it).
`debug.rdna3accel_a3ws` 0, `iop_latch` 0, BAR 1 GiB, no rdna3mtl.conf unless said.

1. **Plan correction: logging out does not restart WindowServer on macOS 26.** After log out /
   log in WindowServer kept its pid (195) and its knobs; an rdna3mtl.conf change for
   WindowServer needs a reboot (or a WindowServer restart). Apps read the conf at their start.
2. **N73 A/B (horizon-client in a world, VSync off, 35 s each, the plugin's `fps:` lines).**
   The owner's observation decided the setup: whenever the Claude app on the Mac redraws
   (streaming text), the game loses ~100-150 fps; so each side was measured with that app
   hidden and with it visible.

   | | KEEP=0 | KEEP=8 (default) |
   |---|---|---|
   | Claude app hidden | ~390 fps (325-400) | ~398 fps (386-404) |
   | Claude app visible, redrawing | ~240 fps | ~290 fps |
   | game next_pct (hidden) | 34.1 | 36.2 |
   | game away_us_avg (hidden) | 5607 | 5438 |
   | game pres_us_avg (hidden) | 1670 | 1685 |
   | CPU WindowServer / game (hidden) | 84 % / 126 % | 89 % / 128 % |

   Keep it: little gain on an empty desktop, ~+50 fps when WindowServer has other windows
   to draw. WindowServer with KEEP=8: `st_new` ~1 per 1000+ game frames, `st_alias` ~1 per
   game frame (the ring does what it should). DTrace objc probes on the game cost ~40 % of
   its fps (228/s under the probe), so the 30 s count is not a clean number.
3. **What caps the game now: WindowServer's CPU upload of every game frame.** WindowServer's
   `ws frame: SUMMARY` `ups` / `up_kb` and `surfin` `in_mb`: ~230 uploads/s of a full
   1920x1080 frame (7-8 MB), ~1.6 GB/s, `in_us_avg` ~860 us. The game's drawables are copied
   out by the GPU into the IOSurface's host pages (N61) and WindowServer uploads them again
   (G13 surface-in), because N71 keeps app surfaces private while `a3ws` is 0. The drawable
   comes back after ~5.5 ms (`away_us_avg`), and the game waits 34-36 % of its wall in
   nextDrawable. Next cut (DRIVER-PLAN N73 OPEN): share the game's surface with WindowServer
   in VRAM, or a GPU upload in WindowServer, i.e. N71 (b)/(c).
4. **N70** (`quick/gsub-last`): `pres_us_avg` ~1.7 ms (commit -> present in the app) against a
   2.5 ms frame at 400 fps. Not measured with N70 this round; the Linux session decides.
5. **Desktop with the keep ring:** no stale, foreign, black or torn window content while
   dragging windows or resizing the game's window. The game drops from ~300-400 to ~130-160
   fps while its window is dragged or resized and is back at ~400 right after; the owner says
   the window may refresh slowly and not look smooth during the drag (240 fps video still to do).
6. **Geekbench 7 GPU (Metal): still fails, N72 did not reach the failing phi.** Twice: after
   ~270-280 s of linking (`link cache: miss ... 281515 ms`; round 2 was ~300 s, so no 2x),
   `link FAILED: "depthwiseConv_3x3" ... native emitter: Private pointer phi
   %metal2vulkan.ct.slot.48 has differing structural sources; raw construction failed:
   native emitter: pointer merge storage mismatch Function vs Private at Local("%1425")
   across [Local("%72"), Local("%1425")]`, byte for byte the round-2 text; then the MPS
   assert (SIGABRT in `MPSLibrary::MPSKey_Compile`). No `translator note` line at all, so
   `emit_mixed_storage_pointer_phi` (phi_mixed_storage.rs) never demoted it. The failing phi
   is the translator's own `%metal2vulkan.ct.slot.48` (function-constant slot path, `function
   constants: re-translate FAILED`), one of its sources is the phi itself (a loop-carried
   edge, `%1425` across `[%72, %1425]`); the N72 fixtures are plain two-edge LLVM phis. Two
   more re-translate lines in the same run: `visible function reference to 2 function(s)
   [postfixPrimary_f, postfixPrimary_4xf] not linked into this module` and the same for 3
   functions (the linked retry then found them). An `NVMTL_LINK_DUMP` of this build
   (depthwiseConv_3x3 .kernel.ll / .linked.ll / .fc.txt / .err.txt, mod.*.ll, stitch-00*)
   is on the Pi in `~/rdna3-bm/r3/gb7/` (Apple MPS IR: evidence only, never committed).
   No score; the owner's Linux GB7 GPU number is about 145k.
7. **Mouse wake: the mouse, not the driver.** `pmset displaysleepnow` with the SteelSeries
   Rival 3 Wireless Gen 2 (2.4 GHz dongle, USB 1038:1872): used a moment before, moving it
   wakes the displays within ~2 s (`HIDIdleTime` back to 0, `IODisplayWrangler`
   CurrentPowerState 1 -> 4). After 6 min without input (HIDIdleTime 360 s), 30 s of moving it
   delivered no event at all (HIDIdleTime kept counting, no USB/HID line in the kernel log);
   the first click reset HIDIdleTime and the wrangler went 1 -> 4 in the same second. The
   mouse's own power save drops motion until a click; once an event arrives the card and
   both displays wake at once, so `fb/rdna3-fb.cpp` / `kIOWindowServerActiveAttribute` is
   cleared. Asked the owner whether Linux behaves the same after a long idle.
   Side note: `bluetoothd` restarts its bluetool script every ~32 s (no Bluetooth controller;
   the owner's Realtek RTL8761BU dongle 0bda:a728 has no driver yet).
8. **Stick:** `Booter/Quirks/ResizeAppleGpuBars` is 10 in config-metal.plist and
   config-driver.plist too now (copies `*-before-r3bar.plist` next to them).
