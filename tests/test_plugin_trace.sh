#!/bin/bash
# The Metal plugin's frame trace (metal/plugin/nvmtl_trace.h) against its decoder (tools/plugin_trace.py):
#  - tests/test_plugin_trace_writer.c drives the very header the plugin compiles, writing real trace files
#    (a WindowServer-like run with the failure modes of the flicker runs: one back buffer always cleared to
#    transparent black and copied out empty after the kernel refused its dirty mark, one rendered; a command
#    buffer whose surface is never published; Vulkan device loss and a fence timeout; allocation failures)
#  - the decoder must name every one of them, handle a ring that wrapped, a cut-off file and a garbage file,
#    read a report .tar.gz, and exit non-zero under --check when a FAIL finding exists
#  - the band hash equals the kernel's (accel/rdna3-accel.cpp rd3AccelBandHash) on several geometries
#  - NVMTL_TRACE=0 creates nothing; the trace file is 0644, the directory sticky and world writable
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_plugin_trace: FAIL $*"; exit 1; }
need() {   # need <file> <pattern>: the decoder output must contain it
    grep -q -e "$2" "$1" || { echo "--- $1"; cat "$1"; fail "missing: $2"; }
}

W=$T/writer
${CC:-cc} -std=c99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I"$ROOT/metal/plugin" -I"$ROOT/common" \
    -o "$W" "$ROOT/tests/test_plugin_trace_writer.c"
PT="python3 -I -B $ROOT/tools/plugin_trace.py"

# --- the kernel's band hash ---
"$W" parity > "$T/parity.txt" 2>&1 || { cat "$T/parity.txt"; fail "band hash differs from the kernel's"; }

# --- off switch ---
NVMTL_TRACE=0 NVMTL_TRACE_DIR="$T/off" NVMTL_TRACE_NAME=Off "$W" off 2>/dev/null
[ ! -e "$T/off" ] || [ -z "$(ls -A "$T/off")" ] || fail "NVMTL_TRACE=0 still created a trace"

# --- a WindowServer-like run ---
mkdir -p "$T/ws"
NVMTL_TRACE_DIR="$T/ws/nvmtl" NVMTL_TRACE_NAME=WindowServer "$W" scenario 2>/dev/null
f=$(ls "$T"/ws/nvmtl/WindowServer-*.trace)
[ "$(stat -c %a "$f")" = 644 ] || fail "trace file mode $(stat -c %a "$f"), want 644"
[ "$(stat -c %a "$T/ws/nvmtl")" = 1777 ] || fail "trace directory mode $(stat -c %a "$T/ws/nvmtl"), want 1777"
$PT "$T/ws/nvmtl" --frames 20 > "$T/ws.txt"
need "$T/ws.txt" '^WindowServer  pid'
need "$T/ws.txt" 'events: 76 written, 76 kept'
need "$T/ws.txt" 'FAIL never-published surface 103 2560x1440 (shared VRAM): 1 command buffer'
need "$T/ws.txt" 'FAIL empty-publish   surface 102 2560x1440 (shared VRAM): 4 of 4 copy-out'
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
need "$T/s102.txt" 'surface map surf 102 .* -> private image (bytesPerRow not a multiple of 32)'
need "$T/s102.txt" 'kernel dirty surf 102 plane 0 -> REFUSED errno 19 ENODEV'
need "$T/s102.txt" 'failure events:'
need "$T/s102.txt" 'VULKAN BACKEND OFF'
$PT "$T/ws/nvmtl" --rates > "$T/rates.txt"
need "$T/rates.txt" 'per second'
# --check: the FAIL findings above fail the run; --triage prints LEVEL lines
if $PT --check "$T/ws/nvmtl" > /dev/null; then fail "--check passed a trace with FAIL findings"; fi
$PT --triage "$T/ws/nvmtl" > "$T/tri.txt"
need "$T/tri.txt" '^FAIL plugin trace WindowServer\[[0-9]*\] never-published: '
need "$T/tri.txt" '^INFO plugin trace WindowServer\[[0-9]*\]: 9 frames in the ring, 9 passes on surfaces (4 clear-only)'

# --- a report archive: 31-plugin-trace/ inside a tar.gz, and an extracted report dir ---
mkdir -p "$T/rep/rdna3-report-x/31-plugin-trace"
cp "$f" "$T/rep/rdna3-report-x/31-plugin-trace/"
tar -C "$T/rep" -czf "$T/rep.tar.gz" rdna3-report-x
$PT "$T/rep.tar.gz" > "$T/tar.txt"
need "$T/tar.txt" 'FAIL never-published surface 103'
$PT "$T/rep/rdna3-report-x" > "$T/dir.txt"
need "$T/dir.txt" 'FAIL never-published surface 103'
python3 -I -B "$ROOT/tools/plugin_trace.py" "$T/empty.tar.gz" 2>/dev/null && fail "a missing archive was accepted" || true
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
PATH="$T/bin:$PATH" NVMTL_TRACE_DIR="$T/ws/nvmtl" sh "$ROOT/tools/macos/collect_plugin_trace.sh" "$T/col" || fail "collect_plugin_trace.sh failed"
[ -f "$T/col/31-plugin-trace/$(basename "$f")" ] || fail "the trace was not copied"
cmp -s "$f" "$T/col/31-plugin-trace/$(basename "$f")" || fail "the copied trace differs"
need "$T/col/31-plugin-trace/info.txt" 'plugin frame traces collected: 1'
need "$T/col/32-log-nvmtl-windowserver.txt" 'NVMTL: trace: frame trace'
need "$T/col/32-log-nvmtl-windowserver.txt" '\[exit 0\]'
$PT "$T/col" > "$T/col.txt"
need "$T/col.txt" 'FAIL never-published surface 103'
# nothing to collect: a note, the log file anyway, and exit 0
PATH="$T/bin:$PATH" NVMTL_TRACE_DIR="$T/nowhere" sh "$ROOT/tools/macos/collect_plugin_trace.sh" "$T/col2" 2>/dev/null || true
mkdir -p "$T/col2"
PATH="$T/bin:$PATH" NVMTL_TRACE_DIR="$T/nowhere" sh "$ROOT/tools/macos/collect_plugin_trace.sh" "$T/col2" || fail "collect_plugin_trace.sh failed on an empty machine"
need "$T/col2/31-plugin-trace/info.txt" 'plugin frame traces collected: 0'
need "$T/col2/31-plugin-trace/info.txt" 'no traces:'

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

echo "test_plugin_trace: ok"
