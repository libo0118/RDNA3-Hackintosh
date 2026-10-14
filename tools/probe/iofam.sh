#!/bin/sh
# Card run 50: a kernel panic at the shutdown of an install boot, a NULL
# read in IOAccelLegacySurface::clientClose+0x1cc while WindowServer exited
# (iokit_client_died). What that function reads there, from the
# IOAcceleratorFamily2 of the self-hosted macOS runner (the same macOS
# build as the VM, .github/workflows/probe.yml). macOS 11+ keeps the kexts'
# code only in the kernel collections (MH_FILESET); neither otool nor lldb
# splits them (probe run 3: lldb saw one image), so kc_func.py cuts the
# functions out by the fileset entry's own symbol table. Output: $1.
out=${1:-probe-out}
mkdir -p "$out"
sys=$out/30-iofam-system.txt
{ sw_vers; uname -a; ls -la /System/Library/KernelCollections/; } > "$sys" 2>&1

kc=
for k in /System/Library/KernelCollections/*.kc; do
    otool -l "$k" 2>/dev/null | grep -A4 LC_FILESET_ENTRY | grep -q 'com.apple.iokit.IOAcceleratorFamily2' && kc=$k
done
echo "== IOAcceleratorFamily2 is in ${kc:-no collection}" >> "$sys"
[ -n "$kc" ] || exit 0
here=$(dirname "$0")
python3 "$here/kc_func.py" "$kc" com.apple.iokit.IOAcceleratorFamily2 \
    'IOAccelLegacySurface::clientClose\(' 'IOAccelSurface::clientClose\(' \
    > "$out/31-clientClose.txt" 2>&1
python3 "$here/kc_func.py" "$kc" com.apple.iokit.IOAcceleratorFamily2 \
    'IOAccelLegacySurface::free\(' 'IOAccelLegacySurface::detach' 'IOAccelLegacySurface::release_' \
    > "$out/32-surface.txt" 2>&1
wc -l "$out/31-clientClose.txt" "$out/32-surface.txt" >> "$sys"
