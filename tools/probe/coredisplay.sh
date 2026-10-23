#!/bin/sh
# What CoreDisplay checks where WindowServer aborts with K5 answered (card
# runs 41, 45: __assert_rtn in CoreDisplay_CreateDisplayForCGXDisplayDevice).
# Runs on the self-hosted macOS runner (.github/workflows/probe.yml): the
# disassembly of the functions on the crash stack, read from the dyld
# shared cache with dyld_info, plus the strings __assert_rtn is called with.
# Output: $1 (a directory).
# Read-only on the runner: it reads the system libraries and writes only into $1.
# Present-mode part (30-/40-/41-/42-/43- files, card run 2026-10-17d: WindowServer
# logs "init_page_flip: page flip mode is on" and never uses IOPresentment
# transactions): which strings of CoreDisplay, SkyLight, IOPresentment and GPUWrangler
# name the mode switch, the function that logs it with its callers, and every
# string / CFSTR / sysctl / defaults key those functions read.
out=${1:-probe-out}
mkdir -p "$out"
sw_vers > "$out/00-system.txt"; uname -a >> "$out/00-system.txt"
xcode-select -p >> "$out/00-system.txt" 2>&1
command -v dyld_info >> "$out/00-system.txt" 2>&1
dyld_info -help 2>&1 | head -40 >> "$out/00-system.txt"

for lib in /System/Library/Frameworks/CoreDisplay.framework/Versions/A/CoreDisplay \
           /System/Library/PrivateFrameworks/CoreDisplay.framework/Versions/A/CoreDisplay \
           /System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight \
           /System/Library/PrivateFrameworks/IOPresentment.framework/Versions/A/IOPresentment \
           /System/Library/PrivateFrameworks/GPUWrangler.framework/Versions/A/GPUWrangler; do
    n=$(basename "$lib")
    echo "== $lib" >> "$out/00-system.txt"
    # every dyld_info error of this library goes into $n.dis.err (the workflow prints it)
    : > "$out/$n.dis.err"
    if ! dyld_info -platform "$lib" >> "$out/00-system.txt" 2>>"$out/$n.dis.err"; then
        echo "FAILED: $n dyld_info -platform failed (the library is not on this runner?), see $n.dis.err" >> "$out/00-system.txt"
        continue
    fi
    dyld_info -arch x86_64 -disassemble "$lib" > "$out/$n.dis" 2>>"$out/$n.dis.err" ||
        dyld_info -disassemble "$lib" > "$out/$n.dis" 2>>"$out/$n.dis.err"
    dyld_info -arch x86_64 -section __TEXT __cstring "$lib" > "$out/$n.cstring" 2>>"$out/$n.dis.err"
    dyld_info -arch x86_64 -exports "$lib" > "$out/$n.exports" 2>>"$out/$n.dis.err"
    wc -l "$out/$n.dis" "$out/$n.cstring" "$out/$n.exports" >> "$out/00-system.txt"
    # an empty disassembly or string table makes every later file empty without a word: say it here
    for part in dis cstring; do
        [ -s "$out/$n.$part" ] || echo "FAILED: $n .dis/.cstring empty ($n.$part), see $n.dis.err" >> "$out/00-system.txt"
    done
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
echo "== IOAccelerator: $(wc -l < "$out/IOAccelerator.dis") lines; $(head -3 "$out/IOAccelerator.dis.err")" >> "$out/00-system.txt"
grep -n 'SetSurfaceFramebufferShape' "$out/IOAccelerator.dis" | head -20 >> "$out/00-system.txt"
# probe run 8: what copy_forward_region's callers do with its false
# ("... disabling" was logged 813 times, so nothing stays disabled): every
# function that calls it, and every one that calls those
callers() {   # callers FILE SYMBOL-REGEX OUT
    awk -v re="$2" '
        /^[_A-Za-z0-9$.]+.*:$/ || /^_[^ ]*:$/ { if (hit) printf "%s", buf; buf = ""; hit = 0; name = $0 }
        { buf = buf $0 "\n" }
        $0 ~ re && $0 !~ /:$/ { hit = 1 }
        END { if (hit) printf "%s", buf }' "$1" > "$3"
    wc -l "$3" >> "$out/00-system.txt"
}
callers "$cd_dis" 'copy_forward_region' "$out/16-copy-forward-callers.s"
callers "$cd_dis" 'CGXDisplayCreateCopyForwardShape' "$out/17-copy-forward-shape-callers.s"
# every assertion string of CoreDisplay that names a display device, a
# display pipe or AGDC
grep -n -i 'CGXDisplayDevice\|DisplayPipe\|agdc\|linkconfig\|assert' "$out/CoreDisplay.cstring" > "$out/20-CoreDisplay-strings.txt" 2>&1
grep -n -i 'agdc\|linkconfig\|planescaler\|0x921\|0x711' "$out/IOPresentment.cstring" > "$out/21-IOPresentment-strings.txt" 2>&1

# ---- present mode: what makes CoreDisplay choose page flips over transactions
# The keywords of the question, in every library's __cstring (30-keyword-strings.txt)
kw='init_page_flip|page flip mode|MPAccessSurfaceForDisplayDevice|isBackBuffered|IOFBIntegrated|\(IG\)|isMobile|built-in|AAPL,slot-name|TransactionsSupported|DisplayPipeSupported|IOAccelDisplayPipeCapabilities|vendorID'
: > "$out/30-keyword-strings.txt"
for lib in CoreDisplay SkyLight IOPresentment GPUWrangler; do
    echo "== $lib: $kw" >> "$out/30-keyword-strings.txt"
    [ -f "$out/$lib.cstring" ] && grep -n -E "$kw" "$out/$lib.cstring" >> "$out/30-keyword-strings.txt" 2>&1
    [ -f "$out/$lib.exports" ] && grep -n -i -E 'page.?flip|transaction|integrated|DisplayPipe' "$out/$lib.exports" | head -60 >> "$out/30-keyword-strings.txt" 2>&1
done

# Every function of FILE whose body matches REGEX, whole, in file order. The disassembly
# annotates string references (a "literal" comment) or only prints the address, so the
# string is looked for both ways below. `callers` above does the same for calls.
blocks_with() { callers "$@"; }

# 1. the function that logs "page flip mode is": by the string, then by the address of
# the string in __cstring (first hex number on its line there)
for n in CoreDisplay SkyLight; do
    blocks_with "$out/$n.dis" 'page flip mode is|init_page_flip' "$out/40-page-flip-fn-$n.s"
    : > "$out/40-page-flip-fn-$n-by-address.s"      # always there, empty when the address is not found
    addr=$(grep -m1 'page flip mode is' "$out/$n.cstring" 2>/dev/null | grep -o -i '0x[0-9a-f]*' | head -1 | sed 's/^0x0*//' | tr 'A-F' 'a-f')
    [ -n "$addr" ] && blocks_with "$out/$n.dis" "0x$addr" "$out/40-page-flip-fn-$n-by-address.s"
    echo "== $n page flip string address: ${addr:-not found in $n.cstring}" >> "$out/00-system.txt"
done
# otool / llvm-objdump read a Mach-O file; the system libraries live in the dyld shared
# cache from macOS 11 on, so this works only on a runner that still has them on disk
for n in CoreDisplay; do
    f=/System/Library/Frameworks/$n.framework/Versions/A/$n
    [ -f "$f" ] && { otool -arch x86_64 -tV "$f" > "$out/$n.otool" 2>&1 ||
        xcrun llvm-objdump --macho --arch=x86_64 -d "$f" > "$out/$n.otool" 2>&1; }
done

# 2. the function names found (the label lines), their callers, and the callers' callers
callers_of() {   # callers_of NAMES-FILE OUT: every function calling one of the names, per name
    : > "$2"
    while IFS= read -r nm; do
        [ -n "$nm" ] || continue
        sym=$(printf '%s' "${nm%:}" | sed 's#[][\.*^$+?(){}|/]#\\&#g')
        for dis in "$out/CoreDisplay.dis" "$out/SkyLight.dis"; do
            echo "== callers of $nm in $(basename "$dis")" >> "$2"
            callers "$dis" "$sym" "$out/.callers.tmp"
            cat "$out/.callers.tmp" >> "$2"
        done
    done < "$1"
    rm -f "$out/.callers.tmp"
}
cat "$out"/40-page-flip-fn-*.s 2>/dev/null | grep -E '^[^[:space:]].*:$' | sort -u | head -20 > "$out/41-page-flip-function-names.txt"
callers_of "$out/41-page-flip-function-names.txt" "$out/41-page-flip-callers.s"
grep -E '^[^[:space:]].*:$' "$out/41-page-flip-callers.s" | sort -u | head -30 > "$out/41-page-flip-caller-names.txt"
callers_of "$out/41-page-flip-caller-names.txt" "$out/41-page-flip-callers2.s"

# 3. everything those functions read: quoted strings (CFSTR keys, log texts, sysctl and
# defaults names), the functions they call, and the lines that read a preference, a
# sysctl, an environment variable, an IORegistry property or a boolean key
keys() {   # keys FILE...: the unique strings and call targets of these disassembly files
    echo "-- quoted strings and CFSTRs"
    cat "$@" 2>/dev/null | grep -o '"[^"]*"' | sort -u
    echo "-- called functions"
    cat "$@" 2>/dev/null | grep -E 'call|bl[[:space:]]' | grep -o '_[A-Za-z0-9_$.]*' | sort -u
    echo "-- reads (preferences, sysctl, defaults, getenv, IORegistry, boolean keys)"
    cat "$@" 2>/dev/null | grep -i -E 'CFPreferences|sysctl|defaults|getenv|IORegistry|GetBoolean|GetInteger|CGXGetBooleanForKey|CFDictionaryGetValue|os_variant|csr_|CFBundle|NSUserDefaults' | sort -u
}
{
    echo "## the function(s) that log the page flip mode"
    keys "$out"/40-page-flip-fn-*.s
    echo
    echo "## their callers"
    keys "$out/41-page-flip-callers.s"
    echo
    echo "## the callers' callers"
    keys "$out/41-page-flip-callers2.s"
} > "$out/42-page-flip-keys.txt"
# the same question for the strings of the integrated flag and the mobile paths
for pat in 'isMobile' 'IOFBIntegrated|\(IG\)' 'AAPL,slot-name|built-in'; do
    tag=$(printf '%s' "$pat" | tr -c 'A-Za-z0-9\n' '_' | cut -c1-24)
    for n in CoreDisplay GPUWrangler; do
        [ -f "$out/$n.dis" ] && blocks_with "$out/$n.dis" "$pat" "$out/43-fn-$n-$tag.s"
    done
done
{
    echo "== present-mode probe"
    wc -l "$out"/40-* "$out"/41-* "$out"/42-* "$out"/43-* 2>/dev/null
    echo "keyword strings found:"; grep -c . "$out/30-keyword-strings.txt"
} >> "$out/00-system.txt"
ls -la "$out" >> "$out/00-system.txt"
