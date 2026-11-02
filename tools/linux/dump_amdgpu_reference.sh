#!/bin/sh
# Collect reference data from the same RX 7800 XT booted under Linux/amdgpu.
# This is the "ground truth" every stage of the kext gets compared against.
#   sudo tools/linux/dump_amdgpu_reference.sh [outdir]
set -eu
out=${1:-navi32-reference}
mkdir -p "$out"

card=""
for d in /sys/class/drm/card*/device; do
    [ "$(cat "$d/device" 2>/dev/null)" = "0x747e" ] && card=$d && break
done
[ -n "$card" ] || { echo "no 0x747e (Navi 32) card bound to amdgpu" >&2; exit 1; }
pci=$(basename "$(readlink -f "$card")")
echo "card: $card ($pci)"

lspci -nnvvv -s "$pci" > "$out/lspci.txt" 2>&1 || true
cat "$card/resource" > "$out/pci_resource.txt"

# Raw discovery blob (debugfs is per DRM minor; find the one for this card)
for dbg in /sys/kernel/debug/dri/*; do
    if [ -f "$dbg/name" ] && grep -q "$pci" "$dbg/name" 2>/dev/null; then
        cp "$dbg/amdgpu_discovery" "$out/discovery.bin" 2>/dev/null && \
            echo "discovery blob -> $out/discovery.bin"
        cp "$dbg/amdgpu_firmware_info" "$out/firmware_info.txt" 2>/dev/null || true
    fi
done

# Decoded view amdgpu exposes in sysfs: ip_discovery/die/<die>/<hwid>/<inst>/
if [ -d "$card/ip_discovery" ]; then
    find "$card/ip_discovery/die" -name major | sort | while read -r m; do
        d=$(dirname "$m")
        printf '%s %s.%s.%s base=%s\n' "${d#"$card"/ip_discovery/die/}" \
            "$(cat "$d/major")" "$(cat "$d/minor")" "$(cat "$d/revision")" \
            "$(tr '\n' ' ' < "$d/base_addr")"
    done > "$out/ip_discovery_sysfs.txt"
fi

dmesg | grep -iE "amdgpu|drm" > "$out/dmesg-amdgpu.txt" 2>/dev/null || true

# what the kext's bring-up gets compared with (docs/AFTER-TESTS.html):
# DPM levels per clock domain, current link, VBIOS, fan/temps
for f in pp_dpm_sclk pp_dpm_mclk pp_dpm_fclk pp_dpm_socclk pp_dpm_dcefclk pp_dpm_pcie \
         pp_power_profile_mode power_dpm_force_performance_level vbios_version \
         current_link_speed current_link_width mem_info_vram_total; do
    [ -r "$card/$f" ] && { echo "== $f"; cat "$card/$f"; } >> "$out/sysfs-pm.txt" 2>/dev/null
done
for f in "$card"/hwmon/hwmon*/temp*_input "$card"/hwmon/hwmon*/fan1_input "$card"/hwmon/hwmon*/power1_cap; do
    [ -r "$f" ] && echo "$(basename "$f") $(cat "$f")" >> "$out/sysfs-pm.txt" 2>/dev/null
done
[ -r "$card/gpu_metrics" ] && cat "$card/gpu_metrics" > "$out/gpu_metrics.bin" 2>/dev/null
# RADV's device info: gb_addr_config, CU/RB counts, clocks, firmware - the
# same numbers DEV_INFO reports on macOS
if command -v vulkaninfo >/dev/null; then
    RADV_DEBUG=info vulkaninfo --summary > "$out/radv-info.txt" 2>&1 || true
fi
if command -v umr >/dev/null; then
    umr -O bits -r *.gfx1100.mmGB_ADDR_CONFIG > "$out/umr-gb_addr_config.txt" 2>&1 || true
fi
echo "done: $out"
