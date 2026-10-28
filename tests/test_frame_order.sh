#!/bin/sh
# tests/test_frame_order.sh BUILD_DIR - DRIVER-PLAN N59: the frame-order strips end to end on the host. The loopback's
# picture (RDNA3_LB_STRIP=1) holds a css and a canvas strip whose counter shows the frame three back every 40th frame;
# rdna3_drminfo --scanout-strip must find both and tools/frame_order.py must call those steps back, and nothing else.
set -u
B=${1:-build/host}
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
fail=0
no() { echo "FAIL test_frame_order: $*"; fail=1; }
RDNA3_LB_STRIP=1 "$B/rdna3_drminfo_host" --scanout-strip "$T/frameorder-host.log" 1 > "$T/out" 2>&1 || no "the sampler failed: $(cat "$T/out")"
grep -q 'scanout strip: 2 strip(s) found' "$T/out" || no "the sampler did not find the two strips: $(cat "$T/out")"
grep -q '^L canvas 0 206 300$' "$T/frameorder-host.log" || no "the canvas strip is not at row 206, x 300"
grep -q '^E [0-9]* [0-9]* 0$' "$T/frameorder-host.log" || no "no end line, or failed reads"
python3 -I -B tools/frame_order.py --check "$T/frameorder-host.log" > "$T/dec" && no "--check passed a burst with steps back"
grep -q '^FAIL frame order: frameorder-host: [1-9][0-9]* regressions in [0-9]* samples (layer: canvas, pipe 0)' "$T/dec" || no "no canvas regression row: $(head -3 "$T/dec")"
grep -q '^FAIL frame order: frameorder-host: [1-9][0-9]* regressions in [0-9]* samples (layer: css, pipe 0)' "$T/dec" || no "no css regression row"
grep -q 'layer(s) webgl not found' "$T/dec" || no "the missing webgl strip is not named"
# a clean burst: the counter only moves forward (wrapping at 240 for css), one torn sample, the kernel's counters
cat > "$T/frameorder-clean.log" <<'END'
# frameorder clean 12:00:00 iop_stale_wait_ms=12
B debug.rdna3accel_iop_stale: 10
B debug.rdna3accel_iop_stale_moved: 4
L css 0 106 300
S 0 css 0 111101110000000010001111 3
S 100 css 0 0?0000000000111111111111 1
S 200 css 0 000000000000111111111111 5
S 300 css 0 100000000000011111111111 2
E 11 400 0
A debug.rdna3accel_iop_stale: 13
A debug.rdna3accel_iop_stale_moved: 7
END
python3 -I -B tools/frame_order.py --check "$T/frameorder-clean.log" > "$T/dec2" || no "--check failed a clean burst: $(cat "$T/dec2")"
grep -q '^PASS frame order: frameorder-clean (kernel stale wait 12 ms): 0 regressions in 11 samples (layer: css, pipe 0): 3 frames seen, 0 skipped, 1 torn sample(s); kernel during it: iop_stale +3, iop_stale_moved +3' "$T/dec2" || no "clean row wrong: $(cat "$T/dec2")"
printf '# frameorder none\nE 0 10 0\n' > "$T/frameorder-none.log"
python3 -I -B tools/frame_order.py "$T/frameorder-none.log" | grep -q '^WARN frame order: frameorder-none: NO strip found' || no "a burst with no strip is not a WARN"
# the kernel's row (29-surface-vram.txt) and the triage entry point
python3 -I -B - <<'PY' || no "kernel_rows / report_rows"
import sys; sys.path.insert(0, "tools")
import frame_order as f
sv = "debug.rdna3accel_iop_ok: 900\ndebug.rdna3accel_iop_stale_checked: 800\ndebug.rdna3accel_iop_stale_fresh: 780\ndebug.rdna3accel_iop_stale: 20\n" \
     "debug.rdna3accel_iop_stale_moved: 18\ndebug.rdna3accel_iop_stale_timeout: 2\ndebug.rdna3accel_iop_stale_wait_ms: 12\n"
r = f.kernel_rows(sv, True)
assert r[0][0] == "WARN" and "20 STALE" in r[0][2] and "18 waited" in r[0][2] and "PROVEN" in r[0][2], r
assert f.kernel_rows("", True)[0][0] == "WARN" and f.kernel_rows("", False) == []
assert f.kernel_rows(sv.replace("stale: 20", "stale: 18"), True)[0][0] == "INFO"
rows = f.report_rows({"frameorder-a": "# frameorder a\nE 0 1 0\n", "notes": "x"}, sv, True)
assert [x[1] for x in rows] == ["iop stale", "frame order"] and rows[1][0] == "WARN", rows
pl = ("2026-10-26 16:22:46.1 Df WindowServer[195:2ca] (RDNA3MTLDriver) NVMTL: vk: handoff: SUMMARY cbs=5 early=0 by_handler=0 by_wait=0 gap_us_avg=0 gap_us_max=0 held=0 held_us_avg=0 hold=0\n"
      "2026-10-26 16:22:56.1 Df WindowServer[195:2ca] (RDNA3MTLDriver) NVMTL: vk: handoff: SUMMARY cbs=9 early=3 by_handler=1 by_wait=2 by_status=0 gap_us_avg=2100 gap_us_max=4000 held=0 held_us_avg=0 hold=0\n"
      "2026-10-26 16:22:57.1 Df Notes[3545:4237] (RDNA3MTLDriver) NVMTL: vk: handoff: SUMMARY cbs=2 early=0 by_handler=0 by_wait=0 gap_us_avg=0 gap_us_max=0 held=2 held_us_avg=900 hold=1\n")
h = f.handoff_rows(pl)
assert [x[0] for x in h] == ["INFO", "WARN"] and h[1][2].startswith("WindowServer[195]: 3 of 9 ") and "ON, held 2" in h[0][2], h
PY
[ "$fail" = 0 ] && echo "test_frame_order: ok"
exit "$fail"
