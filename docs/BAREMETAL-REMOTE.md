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
