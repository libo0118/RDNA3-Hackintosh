#!/bin/bash
# The Metal plugin's frame trace (metal/plugin/nvmtl_trace.h) against its decoder (tools/plugin_trace.py):
#  - tests/test_plugin_trace_writer.c drives the very header the plugin compiles, writing real trace files
#    (a WindowServer-like run with the failure modes of the flicker runs: one back buffer always cleared to
#    transparent black and copied out empty after the kernel refused its dirty mark, one rendered; a command
#    buffer whose surface is never published; Vulkan device loss and a fence timeout; allocation failures)
#  - the decoder must name every one of them, handle a ring that wrapped, a cut-off file and a garbage file,
#    read a report .tar.gz, and exit non-zero under --check when a FAIL finding exists
#  - the band hash equals the kernel's (accel/rdna3-accel.cpp rd3AccelBandHash) on several geometries
#  - NVMTL_TRACE=0 creates nothing; the trace file is 0640; the shared directory is made 1777 only by the call that
#    created it, a symlinked directory or a foreign owner is refused, a squatted file name is not followed or
#    replaced, the pruner keeps WindowServer's traces for the count limit
#  - the collector takes no symlink, hard link, foreign-owned or magic-less file, and no oversized one
#  - the decoder survives a hostile trace (impossible clock, escape sequences, absurd sizes, links, 300 MB)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
# the collector's system directory: never the host's own /var/tmp/rdna3-nvmtl (a Mac runner with the plugin
# installed has real traces there - CI run #247)
export NVMTL_COLLECT_SYSDIR="$T/no-system-traces"
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_plugin_trace: FAIL $*"; exit 1; }
need() {   # need <file> <pattern>: the decoder output must contain it
    grep -q -e "$2" "$1" || { echo "--- $1"; cat "$1"; fail "missing: $2"; }
}

W=$T/writer
ME=$(id -u)
${CC:-cc} -std=c99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I"$ROOT/metal/plugin" -I"$ROOT/common" \
    -o "$W" "$ROOT/tests/test_plugin_trace_writer.c"
PT="python3 -I -B $ROOT/tools/plugin_trace.py"

# --- the file and directory handling of the header (pure C) ---
"$W" dir "$T/dirs" > /dev/null 2>&1 || fail "nvt_open_dir"
mkdir "$T/sq"
NVMTL_TRACE_DIR="$T/sq" NVMTL_TRACE_NAME=Squat "$W" squat > /dev/null 2>&1 || fail "a squatted trace name was followed or replaced"
"$W" prune "$T/pr" > /dev/null 2>&1 || fail "nvt_prune_fd"
# a set-uid style process takes no directory from its environment: covered by nvt_secure(), not testable without
# set-uid, so look at the code path instead
grep -q 'nvt_env("NVMTL_TRACE_DIR")' "$ROOT/metal/plugin/nvmtl_trace.h" || fail "NVMTL_TRACE_DIR must go through nvt_env"
if grep -n 'getenv("NVMTL_TRACE_' "$ROOT/metal/plugin/nvmtl_trace.h" | grep -v 'getenv("NVMTL_TRACE")' | grep -v 'nvt_env' | grep -q .; then
    fail "a NVMTL_TRACE_* knob is read with plain getenv"
fi

# --- tools/macos/fix_tracedir.sh (package/pkg/postinstall and the VM harness run it): never follows or trusts a plant ---
mode_of() { python3 -I -B -c 'import os,sys;print(oct(os.stat(sys.argv[1]).st_mode&0o7777)[2:])' "$1"; }
FIX="$ROOT/tools/macos/fix_tracedir.sh"
mkdir -p "$T/pi/private/var/tmp" "$T/pi/victim"
chmod 755 "$T/pi/victim"
TD=$T/pi/private/var/tmp/rdna3-nvmtl
ln -s "$T/pi/victim" "$TD"
sh "$FIX" "$T/pi" || fail "fix_tracedir.sh (symlink)"
[ ! -L "$TD" ] && [ -d "$TD" ] || fail "the planted symlink is still there"
[ "$(mode_of "$T/pi/victim")" = 755 ] || fail "fix_tracedir chmod'ed the symlink's target"
[ "$(mode_of "$TD")" = 1777 ] || fail "trace directory is not 1777"
rm -rf "$TD"; : > "$TD"                      # a plain file at the name
sh "$FIX" "$T/pi" || fail "fix_tracedir.sh (file)"
[ -d "$TD" ] || fail "a file at the name was not replaced"
echo live > "$TD/WindowServer-1.trace"
sh "$FIX" "$T/pi" || fail "fix_tracedir.sh (second run)"
[ -f "$TD/WindowServer-1.trace" ] || fail "an existing directory of ours lost its traces"
if [ "$ME" = 0 ]; then
    chown 88 "$TD"                           # WindowServer made it first: kept, with its live trace
    sh "$FIX" "$T/pi" || fail "fix_tracedir.sh (uid 88)"
    [ -f "$TD/WindowServer-1.trace" ] || fail "a directory of _windowserver's lost its traces"
    chown 1234 "$TD"                         # somebody else's: replaced
    sh "$FIX" "$T/pi" || fail "fix_tracedir.sh (foreign)"
    [ ! -e "$TD/WindowServer-1.trace" ] && [ -d "$TD" ] && [ "$(mode_of "$TD")" = 1777 ] || fail "a foreign directory was not replaced"
fi
mkdir -p "$T/pi2"                            # a volume root without /private/var/tmp: made, quietly
sh "$FIX" "$T/pi2" 2> "$T/pi2.err" || fail "fix_tracedir.sh (new volume)"
[ ! -s "$T/pi2.err" ] || { cat "$T/pi2.err"; fail "fix_tracedir.sh is noisy"; }
[ "$(mode_of "$T/pi2/private/var/tmp/rdna3-nvmtl")" = 1777 ] || fail "new volume: trace directory is not 1777"
grep -q 'tools/macos/fix_tracedir.sh' "$ROOT/package/pkg/postinstall" || fail "postinstall does not run fix_tracedir.sh"
grep -q 'fix_tracedir.sh' "$ROOT/tools/vm/macos/build_metal.sh" || fail "build_metal.sh does not run fix_tracedir.sh"

# --- append-only log files (nvlog): no symlink, FIFO, hard link, or (as root) foreign file ---
"$W" append "$T/appendtest" > /dev/null 2>&1 || fail "nvt_open_append"

# --- the kernel's band hash ---
"$W" parity > "$T/parity.txt" 2>&1 || { cat "$T/parity.txt"; fail "band hash differs from the kernel's"; }

# --- off switch ---
NVMTL_TRACE=0 NVMTL_TRACE_DIR="$T/off" NVMTL_TRACE_NAME=Off "$W" off 2>/dev/null
[ ! -e "$T/off" ] || [ -z "$(ls -A "$T/off")" ] || fail "NVMTL_TRACE=0 still created a trace"

# --- a WindowServer-like run ---
mkdir -p "$T/ws"
NVMTL_TRACE_DIR="$T/ws/nvmtl" NVMTL_TRACE_NAME=WindowServer "$W" scenario 2>/dev/null
f=$(ls "$T"/ws/nvmtl/WindowServer-*.trace)
mode() { python3 -I -B -c 'import os, sys; print(oct(os.stat(sys.argv[1]).st_mode & 0o7777)[2:])' "$1"; }   # stat(1) differs on macOS
[ "$(mode "$f")" = 640 ] || fail "trace file mode $(mode "$f"), want 640"
[ "$(mode "$T/ws/nvmtl")" = 700 ] || fail "a named trace directory is mode $(mode "$T/ws/nvmtl"), want 700"
$PT "$T/ws/nvmtl" --frames 20 > "$T/ws.txt"
need "$T/ws.txt" '^WindowServer  pid'
need "$T/ws.txt" 'WARN ws-uid'
need "$T/ws.txt" 'events: 87 written, 87 kept'
need "$T/ws.txt" 'encode-time split (1 render encoder(s), 40 draw(s)): per draw pipeline lookup 7.5us, descriptor writes 22.5us, surface-in 11.2us'
need "$T/ws.txt" 'worst encoder: cb [0-9]*, 40 draw(s), pipeline 0.30ms, descriptors 0.90ms, surface-in 0.45ms (12 call(s)), slowest draw: lookup 60us, descriptors 140us'
# the one-line form of a kind 17 record (not on the frame listings; the failure listing prints it with the other events)
python3 -I -B -c '
import sys, types
sys.path.insert(0, sys.argv[1])
import plugin_trace as pt
r = types.SimpleNamespace(id=7, seed0=0, a0=40, rc=1, dur_us=2500, a1=300, a2=900, words=[0] * 8 + [450, 12, 60, 140])
got = pt.enc_split_line(r)
want = "encoder cb 7: 40 draw(s) in 2.50ms = pipeline lookup 0.30ms (worst draw 60us), descriptor writes 0.90ms (worst 140us), surface-in checks 0.45ms (12 call(s)), rest 0.85ms, 1 pipeline variant(s) built on a draw"
assert got == want, got
assert pt.KIND_NAMES[17] == "ENC_SPLIT" and pt.K_ENC_SPLIT == 17
' "$ROOT/tools" || fail "the kind 17 (ENC_SPLIT) line"
need "$T/ws.txt" 'FAIL never-published surface 103 2560x1440 (shared VRAM): 1 command buffer'
need "$T/ws.txt" 'FAIL empty-publish   surface 107 2560x1440 (shared VRAM): 1 copy-out(s) published an EMPTY picture although the same command buffer DREW'
need "$T/ws.txt" 'INFO empty-publish   surface 102 2560x1440 (shared VRAM): 4 of 4 copy-out(s) published an empty picture after a clear-only pass'
if grep -q 'FAIL empty-publish.*surface 102' "$T/ws.txt"; then fail "a cleared surface's empty copy-out is not a FAIL"; fi
need "$T/ws.txt" 'WARN clear-only      surface 102 2560x1440 .*4 of 4 pass(es) only cleared it (4 to transparent black): (0 0 0 0) x4'
need "$T/ws.txt" 'WARN dirty-refused   surface 102 .*refused 4 time(s)'
need "$T/ws.txt" 'errno 19 ENODEV x4'
need "$T/ws.txt" 'WARN skip-stale      1 surface-in skip(s)'
need "$T/ws.txt" 'right after a doubtful publish'
need "$T/ws.txt" 'last surface-out published an EMPTY picture x1'
need "$T/ws.txt" 'INFO skip-foreign    1 surface-in skip(s) rely on a VRAM tag this process never wrote (surfaces 105)'
need "$T/ws.txt" 'FAIL vk-submit       1 vkQueueSubmit failure'
need "$T/ws.txt" 'surface upload: vkQueueSubmit FAILED VK_ERROR_DEVICE_LOST x1'
need "$T/ws.txt" 'FAIL vk-wait         1 fence wait(s) timed out'
need "$T/ws.txt" 'nvmtl_vk_image_read: TIMEOUT VK_TIMEOUT x1'
need "$T/ws.txt" 'FAIL vk-disable      Vulkan backend switched OFF'
need "$T/ws.txt" 'vkAllocateMemory(image) rc=-2 x1'
# DRIVER-PLAN N43: the buffer texture pitch refusal of an older plugin is INFO with its count, never part of the alloc-fail FAIL
need "$T/ws.txt" 'INFO buftex-pitch    3 buffer texture(s) refused by vkCreateImage with VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT'
need "$T/ws.txt" 'FAIL alloc-fail      2 allocation / import failure(s)'
if grep -q 'vkCreateImage(buffer texture) rc=' "$T/ws.txt"; then fail "the buffer texture pitch refusal is still listed as an allocation failure"; fi
need "$T/ws.txt" 'WARN kreq-import     1 kernel surface import(s) refused'
need "$T/ws.txt" 'import refused VK_ERROR_OUT_OF_DEVICE_MEMORY kernel errno 12 ENOMEM x1'
# the per-surface table: 101 rendered and marked dirty, 102 cleared and copied out empty
grep -E '^  101 +2560x1440 +vram +4 +4 +0 +4 +0 +0 +0 +0' "$T/ws.txt" >/dev/null || { cat "$T/ws.txt"; fail "surface 101 row"; }
grep -E '^  102 +2560x1440 +vram +4 +0 +4 +0 +4 +4 +0 +4' "$T/ws.txt" >/dev/null || { cat "$T/ws.txt"; fail "surface 102 row"; }
# the frame view names the surface, the pass and what was done
need "$T/ws.txt" 'pass [0-9]* att0  surf 102 .*2560x1440 BGRA8Unorm load=Clear store=Store clear=(0 0 0 0)'
need "$T/ws.txt" '-> CLEAR-ONLY to (0 0 0 0)'
need "$T/ws.txt" 'surface-out surf 102 COPY-OUT after dirty refused .*\[errno 19 ENODEV\].*hash=EMPTY'
need "$T/ws.txt" 'surface-out surf 101 DIRTY(no copy)'
need "$T/ws.txt" '!! surf 103 was written but never published'
need "$T/ws.txt" 'lifetime totals .*clear_only 4, clear_only_big 4'
# the timeline of one surface and the failure list
$PT "$T/ws/nvmtl" --surface 102 --all > "$T/s102.txt"
need "$T/s102.txt" 'timeline of surface 102 2560x1440'
need "$T/s102.txt" 'surface map surf 102 .* -> private image (bytesPerRow not a multiple of 256)'
need "$T/s102.txt" 'kernel dirty surf 102 plane 0 -> REFUSED errno 19 ENODEV'
need "$T/s102.txt" 'failure events:'
need "$T/s102.txt" 'VULKAN BACKEND OFF'
$PT "$T/ws/nvmtl" --rates > "$T/rates.txt"
need "$T/rates.txt" 'per second'
# --check: the FAIL findings above fail the run; --triage prints LEVEL lines
if $PT --check "$T/ws/nvmtl" > /dev/null; then fail "--check passed a trace with FAIL findings"; fi
$PT --triage "$T/ws/nvmtl" > "$T/tri.txt"
need "$T/tri.txt" '^FAIL plugin trace WindowServer\[[0-9]*\] (opened [0-9-]* [0-9:]* UTC) never-published: '
need "$T/tri.txt" '^INFO plugin trace WindowServer\[[0-9]*\] (opened [0-9-]* [0-9:]* UTC): 10 frames in the ring, 10 passes on surfaces (4 clear-only)'
need "$T/tri.txt" 'kept [0-9-]* [0-9:]* .. [0-9:]* UTC'

# --- a report archive: 31-plugin-trace/ inside a tar.gz, and an extracted report dir ---
mkdir -p "$T/rep/rdna3-report-x/31-plugin-trace"
cp "$f" "$T/rep/rdna3-report-x/31-plugin-trace/"
tar -C "$T/rep" -czf "$T/rep.tar.gz" rdna3-report-x
$PT "$T/rep.tar.gz" > "$T/tar.txt"
need "$T/tar.txt" 'FAIL never-published surface 103'
$PT "$T/rep/rdna3-report-x" > "$T/dir.txt"
need "$T/dir.txt" 'FAIL never-published surface 103'
python3 -I -B "$ROOT/tools/plugin_trace.py" "$T/empty.tar.gz" >/dev/null 2>&1 && fail "a missing archive was accepted" || true
tar -C "$T/rep" -czf "$T/none.tar.gz" --files-from /dev/null
$PT "$T/none.tar.gz" > "$T/none.txt" || true
need "$T/none.txt" 'no plugin traces in the archive'

# --- the collection script: traces and WindowServer's log into a report directory ---
mkdir -p "$T/bin" "$T/col"
cat > "$T/bin/log" <<'LOG'
#!/bin/sh
echo "2026-10-17 13:15:12.471 Df WindowServer[158:29f] (RDNA3MTLDriver) NVMTL: trace: frame trace /var/tmp/rdna3-nvmtl/WindowServer-158.trace"
LOG
chmod +x "$T/bin/log"
mkdir -m 700 "$T/colsrc"
NVMTL_TRACE_DIR="$T/colsrc" NVMTL_TRACE_NAME=Finder "$W" scenario 2>/dev/null
good=$(ls "$T"/colsrc/Finder-*.trace)
cp "$good" "$T/hard-src.trace"; mv "$T/hard-src.trace" "$T/colsrc/hardsrc.trace"; ln "$T/colsrc/hardsrc.trace" "$T/colsrc/hardlink-2.trace"
ln -s /etc/hostname "$T/colsrc/symlink-3.trace"
ln -s "$good" "$T/colsrc/symlink-to-trace-4.trace"
head -c 8000 /dev/zero > "$T/colsrc/nomagic-5.trace"
mkfifo "$T/colsrc/fifo-9.trace"
if [ "$ME" = 0 ]; then
    cp "$good" "$T/colsrc/foreign-6.trace"; chown 1234 "$T/colsrc/foreign-6.trace"          # owner 1234, header uid 0
    cp "$good" "$T/colsrc/WindowServer-7.trace"; chown 1234 "$T/colsrc/WindowServer-7.trace" # a forged WindowServer
fi
cp "$f" "$T/colsrc/WindowServer-8.trace"   # ours: accepted only when we are root (or _windowserver)
PATH="$T/bin:$PATH" NVMTL_TRACE_DIR="$T/colsrc" sh "$ROOT/tools/macos/collect_plugin_trace.sh" "$T/col" || fail "collect_plugin_trace.sh failed"
I=$T/col/31-plugin-trace/info.txt
cmp -s "$good" "$T/col/31-plugin-trace/$(basename "$good")" || { cat "$I"; fail "the good trace was not copied intact"; }
need "$I" 'ACCEPT .* Finder-[0-9]*\.trace'
need "$I" 'REJECT .* hardsrc.trace: 2 hard links'
need "$I" 'REJECT .* hardlink-2.trace: 2 hard links'
need "$I" 'REJECT .* symlink-3.trace: a symlink'
need "$I" 'REJECT .* symlink-to-trace-4.trace: a symlink'
need "$I" 'REJECT .* nomagic-5.trace: no NVMTLTR1 magic'
need "$I" 'REJECT .* fifo-9.trace: not a regular file'
if [ "$ME" = 0 ]; then
    need "$I" 'REJECT .* foreign-6.trace: owned by uid 1234, its header says uid 0'
    need "$I" 'REJECT .* WindowServer-7.trace: named WindowServer but owned by uid 1234'
    need "$I" 'ACCEPT .* WindowServer-8.trace'
else
    need "$I" "REJECT .* WindowServer-8.trace: named WindowServer but owned by uid $ME"
fi
[ -e "$T/col/31-plugin-trace/symlink-3.trace" ] && fail "a symlink was copied"
[ -e "$T/col/31-plugin-trace/hardlink-2.trace" ] && fail "a hard link was copied"
[ -e "$T/col/31-plugin-trace/nomagic-5.trace" ] && fail "a file without the magic was copied"
need "$I" 'plugin frame traces collected: [0-9]'
need "$T/col/32-log-nvmtl-windowserver.txt" 'NVMTL: trace: frame trace'
need "$T/col/32-log-nvmtl-windowserver.txt" '\[exit 0\]'
# the copy itself is bounded, exclusive and cannot be blocked by a FIFO
sed -n '/^bounded_copy() {/,/^}/p' "$ROOT/tools/macos/collect_plugin_trace.sh" > "$T/bc.sh"
head -c 5000000 /dev/zero > "$T/bc-src"; echo keep > "$T/bc-dst"
( cap=1000; . "$T/bc.sh"; bounded_copy "$T/bc-src" "$T/bc-dst" ) && fail "the copy overwrote an existing file"
[ "$(cat "$T/bc-dst")" = keep ] || fail "noclobber did not protect the destination"
( cap=1000; . "$T/bc.sh"; bounded_copy "$T/bc-src" "$T/bc-new" ) || fail "bounded copy"
[ "$(wc -c < "$T/bc-new" | tr -d " ")" = 1000 ] || fail "the copy is not bounded by the cap"
# a size cap
mkdir -p "$T/col3"
PATH="$T/bin:$PATH" NVMTL_COLLECT_MAXBYTES=1000000 NVMTL_TRACE_DIR="$T/colsrc" sh "$ROOT/tools/macos/collect_plugin_trace.sh" "$T/col3" || fail "collector (cap)"
need "$T/col3/31-plugin-trace/info.txt" 'REJECT .* Finder-[0-9]*\.trace: over 1000000 bytes'
need "$T/col3/31-plugin-trace/info.txt" 'plugin frame traces collected: 0'
# a symlinked or foreign directory is refused
ln -s "$T/colsrc" "$T/linkdir"; mkdir -p "$T/col4"
PATH="$T/bin:$PATH" NVMTL_TRACE_DIR="$T/linkdir" sh "$ROOT/tools/macos/collect_plugin_trace.sh" "$T/col4" || fail "collector (symlinked dir)"
need "$T/col4/31-plugin-trace/info.txt" 'REJECT dir .*linkdir: a symlink'
need "$T/col4/31-plugin-trace/info.txt" 'plugin frame traces collected: 0'
# the good one decodes
$PT "$T/col" > "$T/col.txt"
need "$T/col.txt" 'FAIL never-published surface 103'
# nothing to collect: a note, the log file anyway, and exit 0
mkdir -p "$T/col2"
PATH="$T/bin:$PATH" NVMTL_TRACE_DIR="$T/nowhere" sh "$ROOT/tools/macos/collect_plugin_trace.sh" "$T/col2" || fail "collect_plugin_trace.sh failed on an empty machine"
need "$T/col2/31-plugin-trace/info.txt" 'plugin frame traces collected: 0'
need "$T/col2/31-plugin-trace/info.txt" 'no traces:'

# --- hostile input to the decoder ---
mkdir "$T/evil"
python3 -I -B - "$T/evil" <<'PY'
import struct, sys
d = sys.argv[1]
HDR = struct.Struct("<8s6I3Q2I4Q4I64s")
REC = struct.Struct("<QII4BIi7IQ4I12I")
def trace(path, proc=b"WindowServer", wall=1, nmain=256, tb_d=1, text=None, uid=0, tbn=1, tick=5):
    n = 256
    off_main, off_vk = 4096, 4096 + n * 128
    h = HDR.pack(b"NVMTLTR1", 1, 128, nmain, n, n, 1, off_main, off_vk, off_vk + n * 128, tbn, tb_d, 0, wall, 0, 0, 0, 0, uid, 28, proc)
    body = bytearray(h.ljust(4096, b"\0")) + bytearray(3 * n * 128)
    if text is not None:
        words = struct.unpack("<12I", text.ljust(48, b"\0"))
        body[4096:4096 + 128] = REC.pack(tick, 1, 1, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, *words)
    open(path, "wb").write(bytes(body))
trace(d + "/clock-1.trace", wall=2**63, text=b"x", tbn=2**32 - 1, tick=2**62)           # a clock no date can show
trace(d + "/escape-2.trace", proc=b"\x1b[31mEVIL\x07\x1b]0;pwn\x07", text=b"\x1b]0;pwn\x07\x1b[2J")
trace(d + "/huge-3.trace", nmain=0xffffffff)                                        # a header claiming 4 G records
trace(d + "/cut-4.trace")
open(d + "/cut-4.trace", "r+b").truncate(5000)
PY
ln -s "$T/evil/escape-2.trace" "$T/evil/link-5.trace"
truncate -s 300M "$T/evil/big-6.trace"
$PT --all "$T/evil" > "$T/evil.txt" 2>&1 || true
grep -q Traceback "$T/evil.txt" && { cat "$T/evil.txt"; fail "the decoder crashed on a hostile trace"; }
grep -q $'\033' "$T/evil.txt" && fail "an escape sequence reached the output"
grep -q $'\007' "$T/evil.txt" && fail "a control character reached the output"
need "$T/evil.txt" '??:??:??'
need "$T/evil.txt" 'more than 2097152'
need "$T/evil.txt" 'link-5.trace: not a regular file'
need "$T/evil.txt" 'big-6.trace: 314572800 bytes, more than'
need "$T/evil.txt" 'EVIL'
$PT --triage "$T/evil" > /dev/null 2>&1 || fail "--triage crashed on a hostile trace"

# --- a ring that wrapped keeps the newest and says so ---
NVMTL_TRACE_RING=256 NVMTL_TRACE_DIR="$T/wrap" NVMTL_TRACE_NAME=Safari "$W" wrap 2>/dev/null
$PT --check "$T/wrap" > "$T/wrap.txt"
need "$T/wrap.txt" 'events: 1700 written, 512 kept (main ring 1000, vk ring 700 of 256)'
need "$T/wrap.txt" 'main ring WRAPPED'
need "$T/wrap.txt" 'findings:'
need "$T/wrap.txt" '  none'

# --- damage: a file cut off in the middle, and one that is not a trace ---
mkdir -p "$T/dmg"
head -c 60000 "$f" > "$T/dmg/WindowServer-1.trace"
echo "not a trace" > "$T/dmg/junk.trace"
$PT "$T/dmg" > "$T/dmg.txt" || true
need "$T/dmg.txt" 'is cut off'
need "$T/dmg.txt" 'junk.trace: not a plugin trace'

# --- ECC review round 2: old boots, the snapshots next to a report, a wrapped ring against the A/B ---
mkdir -p "$T/col5" "$T/col6"
PATH="$T/bin:$PATH" NVMTL_BOOTTIME=4000000000 NVMTL_TRACE_DIR="$T/colsrc" sh "$ROOT/tools/macos/collect_plugin_trace.sh" "$T/col5" || fail "collector (boottime)"
need "$T/col5/31-plugin-trace/info.txt" 'REJECT .* Finder-[0-9]*\.trace: written before this boot'
need "$T/col5/31-plugin-trace/info.txt" 'boottime 4000000000'
PATH="$T/bin:$PATH" NVMTL_BOOTTIME=1 NVMTL_TRACE_DIR="$T/colsrc" sh "$ROOT/tools/macos/collect_plugin_trace.sh" --traces-only --only Finder "$T/col6" || fail "collector (traces only)"
need "$T/col6/31-plugin-trace/info.txt" 'ACCEPT .* Finder-[0-9]*\.trace'
grep -q 'nomagic-5' "$T/col6/31-plugin-trace/info.txt" && fail "--only took another name"
[ -e "$T/col6/32-log-nvmtl-windowserver.txt" ] && fail "--traces-only ran log show"
# the clock set 60 s ahead after the boot (NTP): this boot's traces are not "older" (120 s tolerance)
mkdir -p "$T/col7"
PATH="$T/bin:$PATH" NVMTL_BOOTTIME=$(( $(date +%s) + 60 )) NVMTL_TRACE_DIR="$T/colsrc" sh "$ROOT/tools/macos/collect_plugin_trace.sh" --traces-only --only Finder "$T/col7" || fail "collector (tolerance)"
need "$T/col7/31-plugin-trace/info.txt" 'ACCEPT .* Finder-[0-9]*\.trace'
# kern.boottime as macOS prints it ("{ sec = N, usec = M } <date>", usec != 0) and no NVMTL_BOOTTIME: the collector
# takes the boot SECOND, not the microseconds (a greedy parse took usec: every old trace passed)
mkdir -p "$T/bin-bt" "$T/btsrc" "$T/col8" "$T/col9"
chmod 700 "$T/btsrc"
cat > "$T/bin-bt/sysctl" <<'SYSCTL'
#!/bin/sh
[ "$2" = kern.boottime ] || exit 1
case "$BT_STUB" in
real) echo "{ sec = $BT_SEC, usec = 123456 } $(date)" ;;
*) echo "$BT_STUB" ;;
esac
SYSCTL
chmod +x "$T/bin-bt/sysctl"
cp "$good" "$T/btsrc/Finder-new.trace"
cp "$good" "$T/btsrc/Finder-old.trace"
touch -t 200001010000 "$T/btsrc/Finder-old.trace"
BT_NOW=$(date +%s)
( unset NVMTL_BOOTTIME; PATH="$T/bin-bt:$T/bin:$PATH" BT_STUB=real BT_SEC=$((BT_NOW - 3600)) NVMTL_TRACE_DIR="$T/btsrc" \
  sh "$ROOT/tools/macos/collect_plugin_trace.sh" --traces-only --only Finder "$T/col8" ) || fail "collector (kern.boottime)"
need "$T/col8/31-plugin-trace/info.txt" "^boottime $((BT_NOW - 3600))\$"
need "$T/col8/31-plugin-trace/info.txt" 'REJECT .* Finder-old\.trace: written before this boot'
need "$T/col8/31-plugin-trace/info.txt" 'ACCEPT .* Finder-new\.trace'
[ -e "$T/col8/31-plugin-trace/Finder-old.trace" ] && fail "an old trace was collected"
# unparseable kern.boottime: said, nothing filtered
for g in "garbage" "{ usec = 5 }" ""; do
    rm -rf "$T/col9"; mkdir -p "$T/col9"
    ( unset NVMTL_BOOTTIME; PATH="$T/bin-bt:$T/bin:$PATH" BT_STUB="$g" NVMTL_TRACE_DIR="$T/btsrc" \
      sh "$ROOT/tools/macos/collect_plugin_trace.sh" --traces-only --only Finder "$T/col9" ) || fail "collector (garbage kern.boottime '$g')"
    need "$T/col9/31-plugin-trace/info.txt" 'boottime unknown: traces of older boots are not filtered'
    grep -q '^boottime [0-9]' "$T/col9/31-plugin-trace/info.txt" && fail "a boot time was invented from '$g'"
    need "$T/col9/31-plugin-trace/info.txt" 'ACCEPT .* Finder-old\.trace'
done
# an archive whose info.txt says the boot began after the trace was opened: OLD, not judged
mkdir -p "$T/old/rdna3-report-o/31-plugin-trace"
cp "$f" "$T/old/rdna3-report-o/31-plugin-trace/"
echo "boottime 4000000000" > "$T/old/rdna3-report-o/31-plugin-trace/info.txt"
tar -C "$T/old" -czf "$T/old.tar.gz" rdna3-report-o
$PT --triage "$T/old.tar.gz" > "$T/old.txt"
need "$T/old.txt" 'WARN plugin trace: WindowServer\[[0-9]*\] .*OLD - opened .* before this boot'
grep -q 'never-published' "$T/old.txt" && fail "an old boot's trace was judged"
# no trace in the archive, the A/B snapshot sent next to it: read
tar -C "$T/rep" -czf "$T/snap.tar.gz" --files-from /dev/null
cp "$f" "$T/snap-plugin-ab1-WindowServer-1.trace"
$PT --triage "$T/snap.tar.gz" > "$T/snap.txt"
need "$T/snap.txt" 'the A/B snapshots sent next to it are read'
need "$T/snap.txt" 'never-published'
# a wrapped WindowServer ring: WARN with the kept window, and whether it covers the A/B
NVMTL_TRACE_RING=256 NVMTL_TRACE_DIR="$T/wrapws" NVMTL_TRACE_NAME=WindowServer "$W" wrap 2>/dev/null
python3 -I -B - "$ROOT/tools" "$T/wrapws" > "$T/wrapws.txt" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import plugin_trace as p
for lv, t in p.triage_lines(sys.argv[2], ab_window=(1.0, 2.0)):
    print(lv, t)
PY
need "$T/wrapws.txt" 'WARN plugin trace WindowServer\[[0-9]*\] (opened .*): main ring WRAPPED (1000 events written'
need "$T/wrapws.txt" 'the A/B (from 1970-01-01 00:00:01 UTC) is NOT covered'

# two copies of one WindowServer trace (an A/B snapshot and the final copy after the ring wrapped): merged by
# (ring, seq), so the snapshot's surfaces survive and the A/B counts as covered (ECC review round 3)
mkdir -p "$T/two"
python3 -I -B "$ROOT/tests/trace_fixture.py" plugin-join "$T/two/snap-WindowServer-158.trace"
python3 -I -B "$ROOT/tests/trace_fixture.py" plugin-late "$T/two/WindowServer-158.trace"
python3 -I -B - "$ROOT/tools" "$T/two" > "$T/two.txt" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import plugin_trace as p
r = p.triage_ex(sys.argv[2], ab_window=(1792310400.0000005, 1792310401.0))
for lv, t in r["lines"]:
    print(lv, t)
print("surfaces", sorted(r["surfaces"]), "covers", r["covers_ab"])
PY
need "$T/two.txt" '(2 copies merged)'
need "$T/two.txt" 'surfaces \[101, 102, 103\] covers True'
grep -q 'WARN .*WRAPPED' "$T/two.txt" && fail "a wrap the snapshot covers is still a WARN"
need "$T/two.txt" 'main ring 256 records'

echo "test_plugin_trace: ok"
