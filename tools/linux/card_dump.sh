#!/bin/bash
# card_dump.sh - everything Linux/amdgpu knows about the RX 7800 XT, in one
# archive: the ground truth the macOS driver is compared with.
#
#   sudo bash tools/linux/card_dump.sh [--regs] [outdir]   (default ~/Downloads/rdna3-card-dump)
#   -> <outdir>.tar.gz
#
# Only reads what amdgpu itself reports (sysfs, debugfs). --regs also reads
# a short list of GC registers through debugfs amdgpu_regs (GFXOFF held off
# meanwhile). Never sweep register files blindly: the first version read
# every DCN 3.2 register, and touching a power-gated DCN block hung the
# whole machine (2026-10-10). Each phase is written to
# ~/Downloads/rdna3-card-dump.progress (synced) so a hang shows where.
#
# Run it on the Linux host with amdgpu driving the card and the desktop
# idle (no VM, no games): about a minute. Read-only, except that GFXOFF is
# switched off for the moment the GC registers are read and then back on.
#
# What it collects:
#   pci/      lspci -vvv -xxxx (GPU, audio, bridges), IOMMU group, BARs
#   vbios/    the ROM, vbios_version
#   ip/       discovery blob, ip_discovery sysfs, firmware versions
#   pm/       pp_* files, pp_features, pp_table, gpu_metrics samples,
#             gpu_busy_percent over 20 s idle, GFXOFF status/residency, hwmon
#   regs/     (--regs) GC status, RLC power/clock gating, golden settings
#   display/  DC's DTN log (every pipe's HUBP/DPP/MPC/OPP/OTG state), DMUB
#             state, atomic state, EDID + modes of every connector
#   vulkan/   vulkaninfo, RADV_DEBUG=info, rdna3_vktest on the real card
#   sys/      kernel, modules, amdgpu parameters, dmesg
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
user_home=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)
want_regs=0
[ "${1:-}" = --regs ] && { want_regs=1; shift; }
out=${1:-$user_home/Downloads/rdna3-card-dump}
progress=$out.progress
# runs as root: only ever replace a directory an earlier dump made
# (sudo ... card_dump.sh ~/Downloads deleted the whole folder - PR #2 review)
if [ -e "$out" ] && [ ! -e "$out/.rdna3-card-dump" ] && ! { [ -d "$out/pci" ] && [ -d "$out/vbios" ]; }; then
    echo "$out exists and is not an earlier card dump: give a new directory"; exit 1
fi
rm -rf "$out" && mkdir -p "$out"/{pci,vbios,ip,pm,regs,display,vulkan,sys} && : > "$out/.rdna3-card-dump"
log() { printf '== %s\n' "$*"; printf '%s %s\n' "$(date +%T)" "$*" >> "$progress"; sync; }
: > "$progress"
run() { local f=$1; shift; { echo "\$ $*"; "$@"; echo "[exit $?]"; } > "$f" 2>&1; }

card=""
for d in /sys/class/drm/card*/device; do
    [ "$(cat "$d/vendor" 2>/dev/null)" = 0x1002 ] && [ "$(cat "$d/device" 2>/dev/null)" = 0x747e ] && card=$d && break
done
[ -n "$card" ] || { echo "no 1002:747e card bound to amdgpu (is the VM running?)"; exit 1; }
card=$(readlink -f "$card")
pci=$(basename "$card")
dbg=""
for d in /sys/kernel/debug/dri/*; do
    grep -q "$pci" "$d/name" 2>/dev/null && dbg=$d && break
done
log "card $pci, debugfs ${dbg:-none}"
[ -n "$dbg" ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null
[ -n "$dbg" ] || for d in /sys/kernel/debug/dri/*; do grep -q "$pci" "$d/name" 2>/dev/null && dbg=$d && break; done

# ---------------------------------------------------------------- sys
log "system"
run "$out/sys/uname.txt" uname -a
cp /etc/os-release "$out/sys/" 2>/dev/null
cat /proc/cmdline > "$out/sys/cmdline.txt"
run "$out/sys/lsmod.txt" sh -c "lsmod | grep -E 'amdgpu|drm|vfio|kvm'"
for p in /sys/module/amdgpu/parameters/*; do echo "$(basename "$p") = $(cat "$p" 2>/dev/null)"; done > "$out/sys/amdgpu-params.txt"
run "$out/sys/modinfo-amdgpu.txt" modinfo amdgpu
dmesg > "$out/sys/dmesg-full.txt" 2>&1
grep -iE "amdgpu|drm|\[drm|vfio|0000:$pci" "$out/sys/dmesg-full.txt" > "$out/sys/dmesg-amdgpu.txt"
run "$out/sys/mesa.txt" sh -c "glxinfo -B 2>/dev/null || true; dpkg -l 2>/dev/null | grep -E 'mesa|libdrm|linux-firmware|firmware-amd' || rpm -qa 2>/dev/null | grep -E 'mesa|libdrm|firmware' || pacman -Q 2>/dev/null | grep -E 'mesa|libdrm|firmware'"
ls -la /lib/firmware/amdgpu/ 2>/dev/null | grep -E "gc_11_0_3|psp_13_0_10|smu_13_0_10|sdma_6_0_3|dcn_3_2_0|vcn_4_0" > "$out/sys/firmware-files.txt"
run "$out/sys/sensors.txt" sensors

# ---------------------------------------------------------------- pci
log "PCI"
run "$out/pci/gpu-lspci.txt" lspci -nnvvvxxx -s "$pci"
fn=${pci%.*}
for f in 1 2 3; do lspci -s "$fn.$f" >/dev/null 2>&1 && [ -n "$(lspci -s "$fn.$f")" ] && run "$out/pci/func$f-lspci.txt" lspci -nnvvvxxx -s "$fn.$f"; done
# the bridges above the card (the RX 7800 XT has its own switch)
p=$card; i=0
while p=$(dirname "$p"); [ "$(basename "$p")" != devices ] && [ $i -lt 6 ]; do
    b=$(basename "$p"); case $b in pci*) break ;; esac
    run "$out/pci/up$i-$b.txt" lspci -nnvvv -s "$b"; i=$((i + 1))
done
run "$out/pci/tree.txt" lspci -tvnn
cat "$card/resource" > "$out/pci/resource.txt"
for f in current_link_speed current_link_width max_link_speed max_link_width resource0_resize resource2_resize power_state d3cold_allowed; do
    [ -r "$card/$f" ] && echo "$f: $(cat "$card/$f" 2>&1)"
done > "$out/pci/sysfs.txt"
g=$(basename "$(readlink -f "$card/iommu_group" 2>/dev/null)")
[ -n "$g" ] && ls -la "/sys/kernel/iommu_groups/$g/devices/" > "$out/pci/iommu-group.txt" 2>&1

# ---------------------------------------------------------------- vbios / ip
log "VBIOS, IP discovery, firmware"
cat "$card/vbios_version" > "$out/vbios/vbios_version.txt" 2>/dev/null
# the driver's copy only (enabling the ROM BAR on a live card is not worth it)
[ -n "$dbg" ] && [ -r "$dbg/amdgpu_vbios" ] && cat "$dbg/amdgpu_vbios" > "$out/vbios/vbios.rom" 2>/dev/null
[ -n "$dbg" ] && cp "$dbg/amdgpu_discovery" "$out/ip/discovery.bin" 2>/dev/null
[ -n "$dbg" ] && cat "$dbg/amdgpu_firmware_info" > "$out/ip/firmware_info.txt" 2>/dev/null
if [ -d "$card/ip_discovery" ]; then
    find "$card/ip_discovery/die" -name major | sort | while read -r m; do
        d=$(dirname "$m")
        printf '%s %s.%s.%s base=%s\n' "${d#"$card"/ip_discovery/die/}" \
            "$(cat "$d/major")" "$(cat "$d/minor")" "$(cat "$d/revision")" \
            "$(tr '\n' ' ' < "$d/base_addr")"
    done > "$out/ip/ip_discovery_sysfs.txt"
fi

# ---------------------------------------------------------------- pm
log "power management (20 s of idle sampling - leave the desktop alone)"
for f in "$card"/pp_* "$card"/power_dpm_* "$card"/gpu_busy_percent "$card"/mem_busy_percent \
         "$card"/mem_info_* "$card"/unique_id "$card"/serial_number "$card"/product_name \
         "$card"/product_number "$card"/board_info "$card"/thermal_throttling_logging \
         "$card"/apu_thermal_cap "$card"/current_compute_partition; do
    [ -r "$f" ] || continue
    case $(basename "$f") in pp_table) cp "$f" "$out/pm/pp_table.bin" 2>/dev/null; continue ;; esac
    { echo "== $(basename "$f")"; cat "$f" 2>&1; } >> "$out/pm/sysfs.txt"
done
for h in "$card"/hwmon/hwmon*; do
    for f in "$h"/*; do [ -f "$f" ] && [ -r "$f" ] && echo "$(basename "$f") $(cat "$f" 2>/dev/null)"; done
done > "$out/pm/hwmon.txt"
[ -n "$dbg" ] && cat "$dbg/amdgpu_pm_info" > "$out/pm/amdgpu_pm_info.txt" 2>/dev/null
python3 -I - "$card" "$dbg" "$out/pm" <<'EOF'
import os, struct, sys, time
card, dbg, out = sys.argv[1:4]
def rd(p, n=None, binary=False):
    try:
        if n is not None:              # debugfs u32 files want exactly n bytes, unbuffered
            fd = os.open(p, os.O_RDONLY)
            try:
                d = os.read(fd, n)
            finally:
                os.close(fd)
        else:
            with open(p, 'rb') as f:
                d = f.read()
        return d if binary else d.decode(errors='replace').strip()
    except OSError:
        return None
lines = []
for i in range(40):                      # 20 s
    t = time.time()
    busy = rd(card + '/gpu_busy_percent')
    gfxoff = rd(dbg + '/amdgpu_gfxoff_status', 4, True) if dbg else None
    gm = rd(card + '/gpu_metrics', binary=True)
    if gm and i % 8 == 0:
        open('%s/gpu_metrics-%02d.bin' % (out, i), 'wb').write(gm)
    lines.append('%.1f busy=%s gfxoff_status=%s' % (
        i * 0.5, busy, struct.unpack('<I', gfxoff)[0] if gfxoff and len(gfxoff) == 4 else '?'))
    time.sleep(max(0, 0.5 - (time.time() - t)))
open(out + '/idle-samples.txt', 'w').write('\n'.join(lines) + '\n')
# GFXOFF residency over 5 s (debugfs: write 1 starts logging, read gives %)
if dbg and os.path.exists(dbg + '/amdgpu_gfxoff_residency'):
    try:
        with open(dbg + '/amdgpu_gfxoff_residency', 'wb') as f: f.write(struct.pack('<I', 1))
        time.sleep(5)
        r = rd(dbg + '/amdgpu_gfxoff_residency', 4, True)
        with open(dbg + '/amdgpu_gfxoff_residency', 'wb') as f: f.write(struct.pack('<I', 0))
        open(out + '/gfxoff-residency.txt', 'w').write(
            'residency (x0.01 %%): %d\n' % struct.unpack('<I', r)[0] if r and len(r) == 4
            else 'residency: not readable on this card/kernel\n')
    except OSError as e:
        open(out + '/gfxoff-residency.txt', 'w').write('failed: %s\n' % e)
for n in ('amdgpu_gfxoff_count', 'amdgpu_gfxoff_entrycount'):
    v = rd(dbg + '/' + n, 8, True) if dbg else None
    if v and len(v) == 8:
        open(out + '/gfxoff-count.txt', 'a').write('%s %d\n' % (n, struct.unpack('<Q', v)[0]))
EOF

# ---------------------------------------------------------------- registers
if [ "$want_regs" = 1 ] && [ -n "$dbg" ] && [ -r "$dbg/amdgpu_regs" ]; then
    log "GC registers (--regs, GFXOFF held off)"
    python3 -I - "$card" "$dbg" "$ROOT/tools/linux/regs" "$out/regs" <<'EOF'
import glob, os, re, struct, sys, time
card, dbg, regdir, out = sys.argv[1:5]
skip = re.compile(r'(I2C.*DATA|AUX.*DATA|FIFO|_ACK$|INTERRUPT_STATUS.*CLEAR)')
# segment bases per IP from sysfs ip_discovery: die/0/<hwid>/<inst>/base_addr
def bases(hwid):
    for d in sorted(glob.glob('%s/ip_discovery/die/0/%d/0' % (card, hwid))):
        try:
            return [int(x, 0) for x in open(d + '/base_addr').read().split()]
        except OSError:
            pass
    return None
ips = {'gc_11_0_0': 11}      # GC_HWID only: never sweep DCN (power-gated blocks hang the bus)
fd = os.open(dbg + '/amdgpu_regs', os.O_RDONLY)
gfxoff = dbg + '/amdgpu_gfxoff'
held = False
try:
    with open(gfxoff, 'wb') as f: f.write(struct.pack('<I', 0))   # GFX on while GC is read
    held = True
    time.sleep(0.2)
except OSError:
    pass
for name, hwid in ips.items():
    b = bases(hwid)
    res = []
    if not b:
        res.append('# no ip_discovery bases for hwid %d' % hwid)
    else:
        res.append('# bases ' + ' '.join('0x%x' % x for x in b))
        for l in open('%s/%s.txt' % (regdir, name)):
            if l.startswith('#'):
                continue
            n, seg, off = l.split()
            # reads with side effects: I2C/AUX data ports advance their index,
            # FIFOs pop; DC may be using them right now
            if skip.search(n):
                continue
            seg, off = int(seg), int(off, 16)
            if seg >= len(b):
                continue
            a = b[seg] + off
            try:
                v = struct.unpack('<I', os.pread(fd, 4, a * 4))[0]
                res.append('%s 0x%05x 0x%08x' % (n, a, v))
            except OSError as e:
                res.append('%s 0x%05x error %s' % (n, a, e.strerror))
    open('%s/%s.txt' % (out, name), 'w').write('\n'.join(res) + '\n')
if held:
    with open(gfxoff, 'wb') as f: f.write(struct.pack('<I', 1))
EOF
else
    echo "not read (run with --regs; or no debugfs amdgpu_regs)" > "$out/regs/missing.txt"
fi

# ---------------------------------------------------------------- display
log "display"
if [ -n "$dbg" ]; then
    for f in amdgpu_dm_dtn_log amdgpu_dm_dmub_fw_state amdgpu_dm_dmub_tracebuffer amdgpu_dm_capabilities \
             amdgpu_dm_ips_status state clients framebuffer amdgpu_fence_info; do
        [ -r "$dbg/$f" ] && timeout 10 cat "$dbg/$f" > "$out/display/$f.txt" 2>&1
    done
    for c in "$dbg"/*-A-* "$dbg"/DP-* "$dbg"/HDMI-* "$dbg"/DVI-*; do
        [ -d "$c" ] || continue
        for n in link_settings output_bpc dp_dsc_clock_en dp_dsc_slice_width hdcp_sink_capability is_mst_connector; do
            f=$c/$n; [ -f "$f" ] && [ -r "$f" ] && { echo "== $n"; timeout 3 cat "$f" 2>&1 | head -50; }; done \
            > "$out/display/debugfs-$(basename "$c").txt"
    done
fi
for c in /sys/class/drm/card*-*; do
    [ -e "$c/status" ] || continue
    n=$(basename "$c")
    { echo "status $(cat "$c/status")"; echo "enabled $(cat "$c/enabled" 2>/dev/null)"; echo "dpms $(cat "$c/dpms" 2>/dev/null)"
      echo "modes:"; cat "$c/modes" 2>/dev/null; } > "$out/display/$n.txt"
    [ -s "$c/edid" ] && cp "$c/edid" "$out/display/$n.edid"
done
command -v edid-decode >/dev/null && for e in "$out"/display/*.edid; do [ -f "$e" ] && edid-decode "$e" > "$e.txt" 2>&1; done
[ -n "${SUDO_USER:-}" ] && command -v xrandr >/dev/null && \
    sudo -u "$SUDO_USER" env DISPLAY="${DISPLAY:-:0}" xrandr --verbose > "$out/display/xrandr.txt" 2>&1

# ---------------------------------------------------------------- vulkan
log "Vulkan"
vk_user() { if [ -n "${SUDO_USER:-}" ]; then sudo -u "$SUDO_USER" env "$@"; else env "$@"; fi; }
if command -v vulkaninfo >/dev/null; then
    vk_user RADV_DEBUG=info vulkaninfo --summary > "$out/vulkan/summary.txt" 2>&1
    vk_user vulkaninfo > "$out/vulkan/vulkaninfo.txt" 2>&1
else
    echo "vulkaninfo not installed (vulkan-tools)" > "$out/vulkan/summary.txt"
fi
lib=$(ls /usr/lib/x86_64-linux-gnu/libvulkan_radeon.so /usr/lib64/libvulkan_radeon.so /usr/lib/libvulkan_radeon.so 2>/dev/null | head -1)
if [ -n "$lib" ] && [ -f /usr/include/vulkan/vulkan_core.h ] && command -v cc >/dev/null; then
    cc -O1 -std=gnu11 -o "$out/vulkan/rdna3_vktest" "$ROOT/userspace/rdna3_vktest.c" -ldl 2> "$out/vulkan/vktest-build.txt" &&
        vk_user "$out/vulkan/rdna3_vktest" "$lib" > "$out/vulkan/rdna3_vktest.txt" 2>&1
    rm -f "$out/vulkan/rdna3_vktest"
else
    echo "skipped: need $lib + /usr/include/vulkan (libvulkan-dev / vulkan-headers) + cc" > "$out/vulkan/rdna3_vktest.txt"
fi

# ---------------------------------------------------------------- pack
chown -R "${SUDO_USER:-root}" "$out" 2>/dev/null
tar -czf "$out.tar.gz" -C "$(dirname "$out")" "$(basename "$out")" && chown "${SUDO_USER:-root}" "$out.tar.gz"
log "done: $out.tar.gz ($(du -h "$out.tar.gz" | cut -f1))"
grep -h "ALL PASSED\|FAILED\|physical devices" "$out/vulkan/rdna3_vktest.txt" 2>/dev/null | head -3
