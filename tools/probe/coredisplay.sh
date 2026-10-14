#!/bin/sh
# What CoreDisplay checks where WindowServer aborts with K5 answered (card
# runs 41, 45: __assert_rtn in CoreDisplay_CreateDisplayForCGXDisplayDevice).
# Runs on the self-hosted macOS runner (.github/workflows/probe.yml): the
# disassembly of the functions on the crash stack, read from the dyld
# shared cache with dyld_info, plus the strings __assert_rtn is called with.
# Output: $1 (a directory).
out=${1:-probe-out}
mkdir -p "$out"
sw_vers > "$out/00-system.txt"; uname -a >> "$out/00-system.txt"
xcode-select -p >> "$out/00-system.txt" 2>&1
command -v dyld_info >> "$out/00-system.txt" 2>&1
dyld_info -help 2>&1 | head -40 >> "$out/00-system.txt"

for lib in /System/Library/Frameworks/CoreDisplay.framework/Versions/A/CoreDisplay \
           /System/Library/PrivateFrameworks/CoreDisplay.framework/Versions/A/CoreDisplay \
           /System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight \
           /System/Library/PrivateFrameworks/IOPresentment.framework/Versions/A/IOPresentment; do
    n=$(basename "$lib")
    echo "== $lib" >> "$out/00-system.txt"
    dyld_info -platform "$lib" >> "$out/00-system.txt" 2>&1 || continue
    dyld_info -arch x86_64 -disassemble "$lib" > "$out/$n.dis" 2>"$out/$n.dis.err" ||
        dyld_info -disassemble "$lib" > "$out/$n.dis" 2>>"$out/$n.dis.err"
    dyld_info -arch x86_64 -section __TEXT __cstring "$lib" > "$out/$n.cstring" 2>&1
    dyld_info -arch x86_64 -exports "$lib" > "$out/$n.exports" 2>&1
    wc -l "$out/$n.dis" "$out/$n.cstring" "$out/$n.exports" >> "$out/00-system.txt"
done

# the functions on the crash stack, whole
fn() {   # fn FILE SYMBOL-REGEX OUT
    awk -v re="$2" '
        /^[_A-Za-z0-9$.]+.*:$/ || /^_[^ ]*:$/ { on = ($0 ~ re) }
        on { print }' "$1" > "$3"
    wc -l "$3" >> "$out/00-system.txt"
}
cd_dis=$out/CoreDisplay.dis
fn "$cd_dis" 'CoreDisplay_CreateDisplayForCGXDisplayDevice' "$out/10-CreateDisplayForCGXDisplayDevice.s"
fn "$cd_dis" 'ZN11CoreDisplay11FramebufferC[12]E' "$out/11-Framebuffer-ctor.s"
fn "$cd_dis" 'ZN11CoreDisplay3GPUC[12]E' "$out/12-GPU-ctor.s"
fn "$out/SkyLight.dis" 'UpdateCGXDisplayDevice|AddCGXDisplayDeviceToDeviceList' "$out/13-SkyLight-CGXDisplayDevice.s"
# card run 54: 813x "copy_forward_region: copy forward failed (0xe00002c7)
# ... disabling" in WindowServer, stale regions until a full redraw: the
# function and what it calls, plus the strings near it
fn "$cd_dis" 'copy_forward|CopyForward|copyForward' "$out/14-copy-forward.s"
grep -n -i 'copy.forward\|copyforward' "$out/CoreDisplay.cstring" "$out/SkyLight.cstring" > "$out/22-copy-forward-strings.txt" 2>&1
grep -n -i 'copy_forward\|copyforward\|CopyForward' "$out/CoreDisplay.exports" "$out/SkyLight.exports" "$out/IOPresentment.exports" >> "$out/22-copy-forward-strings.txt" 2>&1
# copy_forward_region calls IOAccelSetSurfaceFramebufferShape(surface,
# rects, 0x1000, framebuffer index) and that returns kIOReturnUnsupported:
# the user-side function (which surface selector it calls) from IOAccelerator
iolib=/System/Library/PrivateFrameworks/IOAccelerator.framework/Versions/A/IOAccelerator
dyld_info -arch x86_64 -disassemble "$iolib" > "$out/IOAccelerator.dis" 2>"$out/IOAccelerator.dis.err"
fn "$out/IOAccelerator.dis" 'IOAccelSetSurfaceFramebufferShape|IOAccelSetSurfaceShape' "$out/15-IOAccelSetSurfaceFramebufferShape.s"
# every assertion string of CoreDisplay that names a display device, a
# display pipe or AGDC
grep -n -i 'CGXDisplayDevice\|DisplayPipe\|agdc\|linkconfig\|assert' "$out/CoreDisplay.cstring" > "$out/20-CoreDisplay-strings.txt" 2>&1
grep -n -i 'agdc\|linkconfig\|planescaler\|0x921\|0x711' "$out/IOPresentment.cstring" > "$out/21-IOPresentment-strings.txt" 2>&1
ls -la "$out" >> "$out/00-system.txt"
