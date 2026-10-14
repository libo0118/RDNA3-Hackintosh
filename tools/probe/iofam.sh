#!/bin/sh
# Card run 50: a kernel panic at the shutdown of an install boot, a NULL
# read in IOAccelLegacySurface::clientClose+0x1cc while WindowServer exited
# (iokit_client_died). What that function reads there, from the
# IOAcceleratorFamily2 of the self-hosted macOS runner (the same macOS
# build as the VM, .github/workflows/probe.yml). macOS 11+ keeps the kexts'
# code only in the kernel collections (MH_FILESET), not in
# /System/Library/Extensions: lldb opens those. Output: $1 (a directory).
out=${1:-probe-out}
mkdir -p "$out"
sys=$out/30-iofam-system.txt
{ sw_vers; uname -a
  echo "== kernel collections"; ls -la /System/Library/KernelCollections/
  echo "== the kext bundle"; find /System/Library/Extensions/IOAcceleratorFamily2.kext -maxdepth 3 | head -20
  echo "== KDKs"; ls /Library/Developer/KDKs 2>&1
  echo "== tools"; command -v lldb otool llvm-objdump 2>&1; xcrun --find lldb 2>&1
} > "$sys" 2>&1

# which collection holds the kext (LC_FILESET_ENTRY names)
kc=
for k in /System/Library/KernelCollections/*.kc; do
    if otool -l "$k" 2>/dev/null | grep -A4 LC_FILESET_ENTRY | grep -q 'com.apple.iokit.IOAcceleratorFamily2'; then
        kc=$k
        echo "== IOAcceleratorFamily2 is in $k" >> "$sys"
        otool -l "$k" 2>/dev/null | grep -B2 -A4 'com.apple.iokit.IOAcceleratorFamily2' >> "$sys"
    fi
done
[ -n "$kc" ] || { echo "== no collection names IOAcceleratorFamily2; trying all" >> "$sys"; kc=$(ls /System/Library/KernelCollections/*.kc | tr '\n' ' '); }

lldb_dis() {   # lldb_dis KC OUT COMMAND...: one lldb batch on a collection
    k=$1; o=$2; shift 2
    set -- --batch -o "target create \"$k\"" "$@"
    xcrun lldb "$@" > "$o" 2>&1 || lldb "$@" > "$o" 2>&1
}
for k in $kc; do
    n=$(basename "$k" .kc)
    lldb_dis "$k" "$out/31-$n-clientClose.txt" \
        -o "image lookup -r -n 'IOAccelLegacySurface::clientClose'" \
        -o "disassemble -n 'IOAccelLegacySurface::clientClose()'" \
        -o "disassemble -n 'IOAccelSurface::clientClose()'"
    lldb_dis "$k" "$out/32-$n-surface.txt" \
        -o "image lookup -r -n 'IOAccelLegacySurface::'" \
        -o "disassemble -n 'IOAccelLegacySurface::free()'"
    wc -l "$out/31-$n-clientClose.txt" "$out/32-$n-surface.txt" >> "$sys"
done
