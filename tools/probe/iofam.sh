#!/bin/sh
# Card run 50: a kernel panic at the shutdown of an install boot, a NULL
# read in IOAccelLegacySurface::clientClose+0x1cc while WindowServer exited
# (iokit_client_died). What that function reads there, from the
# IOAcceleratorFamily2 binary of the self-hosted macOS runner
# (.github/workflows/probe.yml). Output: $1 (a directory).
out=${1:-probe-out}
mkdir -p "$out"
k=/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2
{ sw_vers; uname -a; ls -la "$k"; file "$k"; } > "$out/30-iofam-system.txt" 2>&1
otool -arch x86_64 -tV "$k" > "$out/IOAcceleratorFamily2.dis" 2>"$out/IOAcceleratorFamily2.dis.err" ||
    otool -tV "$k" > "$out/IOAcceleratorFamily2.dis" 2>>"$out/IOAcceleratorFamily2.dis.err"
nm -arch x86_64 "$k" 2>/dev/null | c++filt > "$out/IOAcceleratorFamily2.nm"
wc -l "$out/IOAcceleratorFamily2.dis" "$out/IOAcceleratorFamily2.nm" >> "$out/30-iofam-system.txt"
fn() {   # fn SYMBOL-REGEX OUT: the whole function from otool's listing
    awk -v re="$1" '
        /^[_A-Za-z0-9$.]+:$/ { on = ($0 ~ re) }
        on { print }' "$out/IOAcceleratorFamily2.dis" > "$2"
    wc -l "$2" >> "$out/30-iofam-system.txt"
}
fn '^__ZN20IOAccelLegacySurface11clientCloseEv:$' "$out/31-LegacySurface-clientClose.s"
fn '^__ZN14IOAccelSurface11clientCloseEv:$' "$out/32-Surface-clientClose.s"
fn '^__ZN20IOAccelLegacySurface4freeEv:$' "$out/33-LegacySurface-free.s"
fn '^__ZN20IOAccelLegacySurface4initEv:$|^__ZN20IOAccelLegacySurface10initWithTaskE' "$out/34-LegacySurface-init.s"
grep -n 'IOAccelLegacySurface\|IOAccelSurface::' "$out/IOAcceleratorFamily2.nm" | head -400 > "$out/35-surface-symbols.txt"
