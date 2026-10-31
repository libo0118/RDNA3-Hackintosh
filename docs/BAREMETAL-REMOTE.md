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
