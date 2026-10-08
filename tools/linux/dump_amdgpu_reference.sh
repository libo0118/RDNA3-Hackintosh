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
echo "done: $out"
