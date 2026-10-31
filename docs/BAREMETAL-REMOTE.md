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
- Geekbench 6 is not installed on the Mac yet.
