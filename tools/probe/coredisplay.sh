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
# The switch itself (50-/51-/52-/53-/54- files, probe run 9): "page flip mode is on"
# is a constant, CoreDisplay::UseIOPresentment() is the real switch between page
# flips and IOPresentment transactions. Its body and callees, the users of the
# capability strings, the display pipe setup, the IOPresentment capability path and
# isMobileSystem. Every one of these files exists; a symbol that is missing leaves a
# one-line "not found: ..." file (the workflow prints them under bash -e).
# Probe run 10 (55-/56-/57-/58- files): how ___GetSystemGPUVendors_block_invoke classifies a
# framebuffer (CoreDisplay logs "GetSystemGPUVendors_block_invoke: 0x1000002d1 1 RDNA3Framebuffer":
# vendor bit 1 is never in the allowed mask of UseIOPresentment), who writes useIOP and
# useIOP_on*, the CFSTR keys those functions use (a CFSTR reference is a bare "; " in the
# disassembly: it points to the __cfstring object, resolved here through a dump of that
# section) and the IOP / vendor strings of the cstring tables.
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
    if [ "$n" = CoreDisplay ]; then
        # probe run 10: the CFSTR objects (isa, flags, cstring pointer at +16, length) and the
        # constant data (block literals: invoke pointer at +16) for the resolvers below
        dyld_info -arch x86_64 -section __DATA_CONST __cfstring "$lib" > "$out/$n.cfstring" 2>>"$out/$n.dis.err"
        [ -s "$out/$n.cfstring" ] || dyld_info -arch x86_64 -section __DATA __cfstring "$lib" > "$out/$n.cfstring" 2>>"$out/$n.dis.err"
        dyld_info -arch x86_64 -section __DATA_CONST __const "$lib" > "$out/$n.const" 2>>"$out/$n.dis.err"
        dyld_info -arch x86_64 -section __DATA __const "$lib" >> "$out/$n.const" 2>>"$out/$n.dis.err"
        wc -l "$out/$n.cfstring" "$out/$n.const" >> "$out/00-system.txt"
    fi
    # an empty disassembly or string table makes every later file empty without a word: say it here
    for part in dis cstring; do
        [ -s "$out/$n.$part" ] || echo "FAILED: $n .dis/.cstring empty ($n.$part), see $n.dis.err" >> "$out/00-system.txt"
    done
done

# the functions on the crash stack, whole
fn() {   # fn FILE SYMBOL-REGEX OUT
    awk -v re="$2" '
        /^[_A-Za-z0-9$.]+.*:$/ || /^_[^ ]*:$/ || /^[-+]\[.*\]:$/ { on = ($0 ~ re) }
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
        /^[_A-Za-z0-9$.]+.*:$/ || /^_[^ ]*:$/ || /^[-+]\[.*\]:$/ { if (hit) printf "%s", buf; buf = ""; hit = 0; name = $0 }
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
# ---- the switch itself (probe run 9): CoreDisplay::UseIOPresentment() and what feeds it
# "init_page_flip: page flip mode is on" is a constant (_CGXDisplayDriverInitialize
# stores 1 unconditionally); UseIOPresentment() is what MPHWCopyPixels and
# CGXAcquireDisplayDeviceSurface branch on. Every file below exists: a symbol or
# string that is not found leaves a "not found: ..." line, never a missing file.
nf() {   # nf FILE WHAT: an empty file says why, so the workflow can always show it
    [ -s "$1" ] || echo "not found: $2" > "$1"
}
fn_nf() {   # fn_nf DIS LABEL-REGEX OUT WHAT: the whole function(s) whose label matches
    fn "$1" "$2" "$3" 2>/dev/null
    nf "$3" "$4 (no label matching $2 in $(basename "$1"))"
}
string_users() {   # string_users LIB STRING OUT: functions using STRING, by its text and by its address in LIB.cstring
    addr=$(grep -m1 -F -- "$2" "$out/$1.cstring" 2>/dev/null | grep -o -i '0x[0-9a-f]*' | head -1 | sed 's/^0x0*//' | tr 'A-F' 'a-f')
    re=$(printf '%s' "$2" | sed 's/[^A-Za-z0-9_ ]/./g')
    if [ -n "$addr" ]; then re="$re|0x0*$addr([^0-9a-fA-F]|\$)"; fi
    echo "== $1 string \"$2\" address: ${addr:-not found in $1.cstring}" >> "$out/00-system.txt"
    callers "$out/$1.dis" "$re" "$3" 2>/dev/null
    nf "$3" "no function of $1.dis uses \"$2\" (address in $1.cstring: ${addr:-not found})"
}
call_targets() {   # call_targets FILE: the symbols named on call / jmp lines
    grep -E 'call|jmp' "$1" 2>/dev/null | grep -o '_[A-Za-z0-9_$.]*' | sort -u
}
label_names() {   # label_names FILE...: the function labels (without the colon)
    cat "$@" 2>/dev/null | grep -E '^[^[:space:]].*:$' | sed 's/:$//' | sort -u
}
callee_bodies() {   # callee_bodies SRC DIS OUT MAXFN MAXLINES [SKIPFILE...]
    src=$1; dis=$2; dst=$3; maxfn=$4; maxln=$5; shift 5
    call_targets "$src" > "$out/.want.tmp"
    label_names "$src" "$@" > "$out/.skip.tmp"
    awk -v want="$out/.want.tmp" -v skip="$out/.skip.tmp" -v maxn="$maxfn" -v maxl="$maxln" '
        BEGIN { while ((getline l < want) > 0) w[l] = 1
                while ((getline l < skip) > 0) seen[l] = 1 }
        /^[_A-Za-z0-9$.]+.*:$/ || /^_[^ ]*:$/ || /^[-+]\[.*\]:$/ {
            nm = $0; sub(/:[ \t]*$/, "", nm); on = 0
            if ((nm in w) && !(nm in seen) && nfn < maxn) { on = 1; seen[nm] = 1; nfn++; lines = 0; print "== " nm }
        }
        on { lines++; if (lines <= maxl) print; else if (lines == maxl + 1) print "   ... truncated at " maxl " lines" }
    ' "$dis" > "$dst" 2>/dev/null
    nf "$dst" "no callee of $(basename "$src") is a function of $(basename "$dis")"
    rm -f "$out/.want.tmp" "$out/.skip.tmp"
}
small_blocks() {   # small_blocks FILE MAXLINES: the "== name" blocks of at most MAXLINES lines
    awk -v max="$2" '
        function flush() { if (n > 0 && n <= max) printf "%s", buf; buf = ""; n = 0 }
        /^== / { flush() }
        { buf = buf $0 "\n"; n++ }
        END { flush() }' "$1"
}
io_dis=$out/IOPresentment.dis
# 1. the switch, its callees (one level), and the callees of the small ones (two levels)
fn_nf "$cd_dis" 'ZN11CoreDisplay16UseIOPresentmentEv' "$out/50-UseIOPresentment.s" "CoreDisplay::UseIOPresentment"
callee_bodies "$out/50-UseIOPresentment.s" "$cd_dis" "$out/50-UseIOPresentment-callees.s" 25 300
small_blocks "$out/50-UseIOPresentment-callees.s" 62 > "$out/.small.tmp"
callee_bodies "$out/.small.tmp" "$cd_dis" "$out/50-UseIOPresentment-callees2.s" 25 80 \
    "$out/50-UseIOPresentment.s" "$out/50-UseIOPresentment-callees.s"
rm -f "$out/.small.tmp"
keys "$out/50-UseIOPresentment.s" "$out/50-UseIOPresentment-callees.s" "$out/50-UseIOPresentment-callees2.s" \
    > "$out/50-UseIOPresentment-keys.txt"
# 2. who uses the capability strings in CoreDisplay, and the display pipe setup
string_users CoreDisplay displayPipeIsAvailable "$out/51-CoreDisplay-str-displayPipeIsAvailable.s"
string_users CoreDisplay IOPRESENTMENT_CAPABILITIES_AGDCEXTENDED_MISSING "$out/51-CoreDisplay-str-IOPRESENTMENT_CAPABILITIES_AGDCEXTENDED_MISSING.s"
fn_nf "$cd_dis" 'InitializeDisplayPipes' "$out/52-CoreDisplay-InitializeDisplayPipes.s" "CoreDisplay::InitializeDisplayPipes"
fn_nf "$cd_dis" 'DisplayPipeForService' "$out/52-CoreDisplay-DisplayPipeForService.s" "CoreDisplay::DisplayPipeForService"
# 3. the IOPresentment capability path
fn_nf "$io_dis" 'IOPresentmentCopyCapabilities|IOPresentmentDisplayPipe.*apabilit' "$out/53-IOPresentment-CopyCapabilities.s" "IOPresentmentCopyCapabilities (IOPresentmentDisplayPipe)"
fn_nf "$io_dis" '_IOPresentmentInitE|IOPresentmentInit($|[^A-Za-z])' "$out/53-IOPresentment-Init.s" "_IOPresentmentInit"
# probe run 9: ioPresentmentFindAGDCMatch is a string, not a function
string_users IOPresentment ioPresentmentFindAGDCMatch "$out/53-IOPresentment-str-ioPresentmentFindAGDCMatch.s"
string_users IOPresentment DisplayPipeSupported "$out/53-IOPresentment-str-DisplayPipeSupported.s"
string_users IOPresentment TransactionsSupported "$out/53-IOPresentment-str-TransactionsSupported.s"
string_users IOPresentment "AGDC Extended Capabilities not found" "$out/53-IOPresentment-str-AGDC_Extended_Capabilities_not_found.s"
# 4. isMobileSystem (CoreDisplay), whole, and its callees
fn_nf "$cd_dis" 'isMobileSystem' "$out/54-isMobileSystem.s" "isMobileSystem"
callee_bodies "$out/54-isMobileSystem.s" "$cd_dis" "$out/54-isMobileSystem-callees.s" 10 120
# ---- probe run 10: how GetSystemGPUVendors classifies a framebuffer, who writes useIOP*
# dyld_info prints labels at column 0 ("__ZN11CoreDisplay16UseIOPresentmentEv:"), globals in a
# trailing "; __ZN11CoreDisplayL12useIOP_onAMDE" comment and string literals as
# "; string literal: ...". A CFSTR reference is a bare "; " (the code points to the __cfstring
# object, not to the cstring), so the resolver below computes the target of every
# `leaq disp(%rip)` with an empty comment (next instruction address + disp), reads the cstring
# pointer at entry+16 of the CoreDisplay.cfstring dump and looks the text up in CoreDisplay.cstring.
# Blocks passed to dispatch_once are only referenced (`leaq ___block_literal_global...` into rsi):
# the block literal is read from CoreDisplay.const, its invoke pointer (+16) names the function.
# awk functions shared by the two passes (POSIX / BSD awk: no strtonum, no intervals)
RES_FN='
function hex2dec(s,    i, n, d, v) {
    sub(/^0[xX]/, "", s); v = 0; n = length(s)
    if (n == 0) return -1
    for (i = 1; i <= n; i++) {
        d = index("0123456789abcdef", tolower(substr(s, i, 1))) - 1
        if (d < 0) return -1
        v = v * 16 + d
    }
    return v
}
function key(v) { return sprintf("%.0f", v) }
function dec2hex(v,    s, d) {
    if (v <= 0) return "0"
    s = ""
    while (v > 0) { d = v % 16; s = substr("0123456789ABCDEF", d + 1, 1) s; v = (v - d) / 16 }
    return s
}
function ishexaddr(t) {
    sub(/:$/, "", t)
    if (t ~ /^0[xX][0-9a-fA-F]+$/) return 1
    return (length(t) >= 8 && t ~ /^[0-9a-fA-F]+$/)
}
function addrof(t) { sub(/:$/, "", t); return hex2dec(t) }
'
# pass 1: the lea targets of the disassembly files (mode cfstr: empty comment -> text; mode blk: block literal -> invoke)
RES_MAIN='
# memory image of a section dump: rows "ADDR  w0 w1 ..." with words of 2, 4, 8 or 16 hex digits
# (little-endian numbers, or single bytes), so a pointer at entry+16 can be read back
function loadmem(file,    line, n, t, i, tok, w, k, off, a, rows) {
    rows = 0
    while ((getline line < file) > 0) {
        sub(/^[ \t]+/, "", line)
        n = split(line, t, /[ \t]+/)
        if (n < 2 || !ishexaddr(t[1])) continue
        a = addrof(t[1]); if (a < 0) continue
        rows++
        off = a
        for (i = 2; i <= n; i++) {
            tok = t[i]; sub(/^0[xX]/, "", tok); w = length(tok)
            if (tok !~ /^[0-9a-fA-F]+$/ || (w != 2 && w != 4 && w != 8 && w != 16)) break
            for (k = 0; k < w / 2; k++) mem[key(off + k)] = tolower(substr(tok, w - 2 * k - 1, 2))
            off += w / 2
        }
    }
    close(file)
    return rows
}
function loadstr(file,    line, tok, rest, a) {
    while ((getline line < file) > 0) {
        sub(/^[ \t]+/, "", line)
        tok = line; sub(/[ \t].*$/, "", tok)
        if (!ishexaddr(tok)) continue
        a = addrof(tok); if (a < 0) continue
        rest = line; sub(/^[^ \t]+[ \t]*/, "", rest)
        if (rest ~ /^".*"$/) rest = substr(rest, 2, length(rest) - 2)
        str[key(a)] = rest; nstr++
    }
    close(file)
}
function readptr(a,    k, s, b) {
    s = ""
    for (k = 7; k >= 0; k--) { b = key(a + k); if (!(b in mem)) return -1; s = s mem[b] }
    return hex2dec(substr(s, 5))
}
function emptycomment(line,    p) {
    p = index(line, ";")
    if (p == 0) { p = index(line, "##"); if (p == 0) return 1; return (substr(line, p + 2) ~ /^[ \t]*$/) }
    return (substr(line, p + 1) ~ /^[ \t]*$/)
}
function lea(line,    s, neg, d) {
    leaok = 0
    if (line !~ /(^|[ \t])lea[a-z]?[ \t]/) return
    if (match(line, /-?0[xX][0-9a-fA-F]+[(]%rip[)]/)) {
        s = substr(line, RSTART, RLENGTH); sub(/[(]%rip[)]/, "", s)
        neg = (substr(s, 1, 1) == "-"); sub(/^-/, "", s); d = hex2dec(s)
    } else if (match(line, /[(]%rip[)]/)) { d = 0; neg = 0 }
    else if (match(line, /\[rip *[-+] *0[xX][0-9a-fA-F]+\]/)) {
        s = substr(line, RSTART, RLENGTH); neg = (index(s, "-") > 0)
        sub(/^\[rip *[-+] */, "", s); sub(/\]$/, "", s); d = hex2dec(s)
    } else return
    if (d < 0) return
    disp = neg ? -d : d; leaok = 1
}
function emit(tok, text,    f) {
    if (tok in seen) return
    seen[tok] = 1
    if (pfn != lastfn) { f = pfn; sub(/:[ \t]*$/, "", f); print "== " f; lastfn = pfn }
    print tok "  " text
}
function resolve(nextad,    t, p, ps) {
    t = nextad + pdisp
    if (mode == "blk") {
        p = readptr(t + 16)
        if (p >= 0) { nblk++; print "BLOCK " ptok " " dec2hex(t) " " dec2hex(p) " " key(p) }
        else { nmiss++; print "BLOCKFAIL " ptok " " dec2hex(t) }
        return
    }
    p = readptr(t + 16)
    if (p >= 0) {
        ps = key(p)
        if (ps in str) { nres++; emit(ptok, str[ps]) }
        else { nnostr++; emit(ptok, "<cfstring " dec2hex(t) " -> cstring " dec2hex(p) " is not in the cstring dump>") }
    } else if (key(t) in str) { ndirect++; emit(ptok, str[key(t)]) }
    else nmiss++
}
BEGIN {
    nrows = loadmem(memfile)
    if (mode == "cfstr") loadstr(cst)
}
/^== / || /^[^ \t].*:[ \t]*$/ { pend = 0; pfn = $0; next }
{
    tok = $1
    if (!ishexaddr(tok)) next
    ad = addrof(tok)
    if (pend) { resolve(ad); pend = 0 }
    lea($0)
    if (!leaok) next
    if (mode == "blk") want1 = (index($0, "__block_literal_global") > 0)
    else want1 = emptycomment($0)
    if (want1) { ncand++; pend = 1; ptok = tok; pdisp = disp }
}
END {
    print "CFSTR resolver (" mode "): " nrows + 0 " dump rows, " nstr + 0 " cstring entries, " ncand + 0 " lea candidates: " \
        nres + 0 " resolved, " nnostr + 0 " cfstring entries whose cstring is not in the dump, " ndirect + 0 " direct cstring refs, " \
        nblk + 0 " blocks, " nmiss + 0 " unresolved" > stats
}
'
# pass 2: the functions whose first instruction is one of the wanted addresses (block invoke functions)
RES_BODY='
BEGIN { while ((getline l < want) > 0) w[l] = 1; close(want) }
/^== / || /^[^ \t].*:[ \t]*$/ { lab = $0; first = 1; on = 0; next }
{
    if (first && ishexaddr($1)) {
        first = 0
        if ((key(addrof($1)) in w) && nfn < maxn) { on = 1; nfn++; lines = 0; print "== block invoke function at 0x" dec2hex(addrof($1)); print lab }
    }
    if (on) { lines++; if (lines <= maxl) print; else if (lines == maxl + 1) print "   ... truncated at " maxl " lines" }
}
'
# 1. GetSystemGPUVendors: every function whose label matches (the block invoke included), the
# blocks it passes to dispatch_once (resolved through the block literal), and the callees one level
fn_nf "$cd_dis" 'GetSystemGPUVendors' "$out/55-GetSystemGPUVendors.s" "GetSystemGPUVendors"
: > "$out/.blk.tmp"; : > "$out/.want.tmp"; : > "$out/.stats.tmp"
if [ -s "$out/CoreDisplay.const" ]; then
    awk -v mode=blk -v memfile="$out/CoreDisplay.const" -v stats="$out/.stats.tmp" "$RES_FN $RES_MAIN" \
        "$out/55-GetSystemGPUVendors.s" > "$out/.blk.tmp" 2>>"$out/CoreDisplay.dis.err"
    awk '$1 == "BLOCK" { print $5 }' "$out/.blk.tmp" > "$out/.want.tmp"
    cat "$out/.stats.tmp" >> "$out/00-system.txt"
else
    echo "GetSystemGPUVendors blocks: dyld_info could not print __const of CoreDisplay (CoreDisplay.dis.err); only the labels matching GetSystemGPUVendors are dumped" >> "$out/00-system.txt"
fi
if [ -s "$out/.want.tmp" ]; then
    { echo "# block literals passed to dispatch_once (literal address, invoke address read at literal+16):"
      sed 's/^/# /' "$out/.blk.tmp"
      awk -v want="$out/.want.tmp" -v maxn=10 -v maxl=300 "$RES_FN $RES_BODY" "$cd_dis" 2>>"$out/CoreDisplay.dis.err"; } > "$out/55-GetSystemGPUVendors-blocks.s"
else
    echo "not found: no block literal of GetSystemGPUVendors resolved through CoreDisplay.const (labels matching GetSystemGPUVendors are in 55-GetSystemGPUVendors.s)" > "$out/55-GetSystemGPUVendors-blocks.s"
fi
cat "$out/55-GetSystemGPUVendors.s" "$out/55-GetSystemGPUVendors-blocks.s" > "$out/.gv.tmp"
callee_bodies "$out/.gv.tmp" "$cd_dis" "$out/55-GetSystemGPUVendors-callees.s" 25 300
rm -f "$out/.gv.tmp" "$out/.blk.tmp" "$out/.want.tmp"
keys "$out/55-GetSystemGPUVendors.s" "$out/55-GetSystemGPUVendors-blocks.s" "$out/55-GetSystemGPUVendors-callees.s" \
    > "$out/55-GetSystemGPUVendors-keys.txt"
# 2. who writes useIOP, useIOP_onIG / _onAMD / _onNVIDIA / _onAPV (comments name the globals)
callers "$cd_dis" 'useIOP' "$out/56-useIOP-users.s"
nf "$out/56-useIOP-users.s" "no function of CoreDisplay.dis references useIOP*"
callee_bodies "$out/56-useIOP-users.s" "$cd_dis" "$out/56-useIOP-callees.s" 12 200
{
    keys "$out/56-useIOP-users.s" "$out/56-useIOP-callees.s"
    echo "-- useIOP globals named in comments"
    cat "$out/56-useIOP-users.s" "$out/56-useIOP-callees.s" | grep -o '[A-Za-z0-9_]*useIOP[A-Za-z0-9_]*' | sort -u
} > "$out/56-useIOP-keys.txt"
# 3. the CFSTR resolver over the functions dumped in 50-, 55- and 56-
if [ -s "$out/CoreDisplay.cfstring" ]; then
    awk -v mode=cfstr -v memfile="$out/CoreDisplay.cfstring" -v cst="$out/CoreDisplay.cstring" -v stats="$out/.stats.tmp" \
        "$RES_FN $RES_MAIN" "$out"/50-*.s "$out"/55-*.s "$out"/56-*.s > "$out/57-cfstr-resolved.txt" 2>>"$out/CoreDisplay.dis.err"
    cat "$out/.stats.tmp" >> "$out/00-system.txt"
    nf "$out/57-cfstr-resolved.txt" "no lea with an empty comment resolved to a CFSTR (see the CFSTR resolver line in 00-system.txt)"
else
    echo "CFSTR resolver: dyld_info cannot print __DATA_CONST __cfstring (nor __DATA __cfstring) of CoreDisplay, see CoreDisplay.dis.err; 57-cfstr-resolved.txt is empty" >> "$out/00-system.txt"
    echo "not found: dyld_info cannot print the __cfstring section of CoreDisplay (see CoreDisplay.dis.err), the CFSTR keys stay unresolved" > "$out/57-cfstr-resolved.txt"
fi
rm -f "$out/.stats.tmp"
# 4. the IOP / vendor strings of the cstring tables
iop='useIOP|IOP_on|vendor-id|IOVendor|GPUVendor|Paravirt|IOPresentment|UseIOP'
: > "$out/58-cstring-iop.txt"
for n in CoreDisplay SkyLight; do
    echo "== $n: $iop" >> "$out/58-cstring-iop.txt"
    c=$(grep -c -E "$iop" "$out/$n.cstring" 2>/dev/null)
    grep -n -E "$iop" "$out/$n.cstring" >> "$out/58-cstring-iop.txt" 2>/dev/null
    [ "${c:-0}" -gt 0 ] || echo "not found: no cstring of $n matches (is $n.cstring empty?)" >> "$out/58-cstring-iop.txt"
done
{
    echo "== GetSystemGPUVendors, useIOP, CFSTR keys (probe run 10)"
    wc -l "$out"/55-* "$out"/56-* "$out"/57-* "$out"/58-* 2>/dev/null
} >> "$out/00-system.txt"
{
    echo "== the switch (probe run 9)"
    wc -l "$out"/50-* "$out"/51-* "$out"/52-* "$out"/53-* "$out"/54-* 2>/dev/null
} >> "$out/00-system.txt"
{
    echo "== present-mode probe"
    wc -l "$out"/40-* "$out"/41-* "$out"/42-* "$out"/43-* 2>/dev/null
    echo "keyword strings found:"; grep -c . "$out/30-keyword-strings.txt"
} >> "$out/00-system.txt"
ls -la "$out" >> "$out/00-system.txt"
