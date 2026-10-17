#!/bin/bash
# tools/result_checklist.py (what 'rdna3vm result' prints): every row of a
# triage SUMMARY classified PASS / FAIL / WARN / INFO / NOT RUN by the rules
# in the script, on a synthetic SUMMARY with one row per rule (a sub-item, a
# row longer than the name column, an unknown row), the counts and RESULT
# line, a triage without a SUMMARY, and the exit status (always 0).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_result_checklist: FAIL $*"; [ -f "$T/out.txt" ] && cat "$T/out.txt"; exit 1; }

cat > "$T/triage.txt" <<'FIX'
some triage output above the summary
   !! two lit pipes scan the same surface: both monitors show one picture
==============================================================================
SUMMARY of the newest report (rdna3-report-test.tar.gz)
  boot-args              rdna3lvl=8 rdna3fb=1
  DMA/GFX jobs           2 verified, 0 wrong
  DMA/GFX jobs bad       2 verified, 1 wrong
  DMA/GFX jobs none      0 verified, 0 wrong
  Vulkan                 ALL PASSED: 13 submit(s) tested
  Vulkan bad             FAILED: 13 submit(s) tested
  Metal                  METAL: ALL PASSED device "AMD Radeon RX 7800 XT"
  Metal bad              METAL: FAILED command buffer status: GPU event segment submission failed
  A3 IOSurface           PASSED IOSurface texture, CPU -> GPU 65536/65536
  A3 IOSurface bad       FAILED IOSurface texture
  screens 1              2 lit
  screens sleep          NOT READ: the display was asleep (system sleep or shutdown)
  screens sleep dpms     0 lit, 2 blanked
  screens dark           0 lit
  screens mode-d0-1600x900 2 lit, UNDERFLOW
  screens same           2 lit, SAME SURFACE on two pipes
  screens notx           2 lit, NO TX LINES
  screens dig            2 lit, TX: DIG FIFO error
  screens wake           2 lit, 2 blanked
  TX watch               no change on any head after the start (488 tick(s))
  TX watch bad           head 0: 1 change(s); first with problems at 61234 ms: problems: DIG back end off
  TX watch never         NEVER TICKED: no change seen proves nothing
  R3 platform            macOS 15.8.1 (24H32), Darwin 24.6.0
  R4 macOS               the accelerator runs on Darwin 24 (15.8.1)
  V3 Vulkan WSI          no present path to try: headless surface no, swapchain no, Metal surface no
  M4 CAMetalLayer        NO PRESENT CALLBACK CAMetalLayer: drawables yes, rendering right, presents NEVER shown
  M4 CAMetalLayer ok     PASS on the screen (mtlprobe: presents NEVER shown)
  M4 on the screen       magenta pixels: pipe0 0 of 57600, pipe1 0 of 57600
  M4 on the screen ok    magenta pixels: pipe0 0 of 57600, pipe1 8238 of 57600 -> PASS: the CAMetalLayer window is on the screen
  M4 on the screen none  magenta pixels: no picture
  M5 WindowServer        WindowServer LOADED the plugin; it is on the allow list
  M5 WindowServer bad    WindowServer did not load the plugin; it is on the allow list
  M6 apps                1 of 2 apps ran on the plugin without a crash
    Chess                running NO, plugin NOT loaded, crash report: /Users/x/Library/Logs/DiagnosticReports/Chess.ips
    Maps                 running yes, plugin loaded, crash report: none
  M6 apps ok             2 of 2 apps ran on the plugin without a crash
  M6 screens             pipe 0 changed (60%); pipe 1 changed (66%)
  M6 screens frozen      pipe 0 changed (60%); pipe 1 (main) FROZEN in all 4 looks
  M6 screens not judged  pipe 0 not judged; pipe 1 not judged
  flicker                INFO head 0: NOT MEASURED between 'a' and 'b': no hashed swaps
  flicker                PASS head 1: 3 of 400 hashed swaps A B A B (1%, under 25%)
  copy forward           WARN head 0: 190 of 433 swaps skipped
  A3 display counters    refused 10, imported 0, other 1162 (kept out 1162)
  display sleep          the look found the system asleep; heads blanked first (kernel log): 0, 1
  scaled modes           MODES: 4 switch(es) tried, 0 failed
  scaled modes bad       MODES: 4 switch(es) tried, 2 failed
  gamma                  GAMMA: FAIL - the picture in VRAM darkened
  gamma ok               GAMMA: PASS - 2 pipe(s): the CRC moved
  IOSurface isolation    SURFXPROC: PASS - another process's surface 52: No such file or directory
  power (last)           PM: activity 100%, gfxclk 539 MHz, 15 W
  idle cap               4 cap(s), e.g. PM: idle, GFXCLK capped at 500 MHz (0)
  something new          a row no rule knows
==============================================================================
RESULT: something failed - start at the first FAIL (docs/AFTER-TESTS.md)
FIX

python3 -I -B "$ROOT/tools/result_checklist.py" --no-color --width 200 "$T/triage.txt" > "$T/out.txt" || fail "exit status"
want() {   # want <verdict> <name> [text part]: one line with that verdict and name
    grep -qE "^$1 +$2 +.*$3" "$T/out.txt" || fail "want $1 '$2' $3"
}
want 'INFO' 'boot-args' 'rdna3lvl=8'
want 'PASS' 'DMA/GFX jobs' '2 verified, 0 wrong'
want 'FAIL' 'DMA/GFX jobs bad' '1 wrong'
want 'NOT RUN' 'DMA/GFX jobs none' '0 verified'
want 'PASS' 'Vulkan' 'ALL PASSED'
want 'FAIL' 'Vulkan bad' 'FAILED'
want 'PASS' 'Metal' 'ALL PASSED'
want 'FAIL' 'Metal bad' 'METAL: FAILED'
want 'PASS' 'A3 IOSurface' 'PASSED'
want 'FAIL' 'A3 IOSurface bad' 'FAILED'
want 'PASS' 'screens 1' '2 lit'
want 'NOT RUN' 'screens sleep' 'NOT READ'
want 'PASS' 'screens sleep dpms' '0 lit, 2 blanked'
want 'FAIL' 'screens dark' '0 lit'
want 'WARN' 'screens mode-d0-1600x900' 'UNDERFLOW'      # a name longer than the column
want 'WARN' 'screens same' 'SAME SURFACE'
want 'WARN' 'screens notx' 'NO TX LINES'
want 'WARN' 'screens dig' 'DIG FIFO error'
want 'WARN' 'screens wake' '2 blanked'
want 'INFO' 'TX watch' 'no change on any head'
want 'WARN' 'TX watch bad' 'with problems'
want 'NOT RUN' 'TX watch never' 'NEVER TICKED'
want 'INFO' 'R3 platform' 'macOS 15.8.1'
want 'INFO' 'R4 macOS' 'runs on Darwin 24'
want 'NOT RUN' 'V3 Vulkan WSI' 'no present path'
want 'FAIL' 'M4 CAMetalLayer' 'NO PRESENT CALLBACK'
want 'PASS' 'M4 CAMetalLayer ok' 'on the screen'
want 'INFO' 'M4 on the screen' 'pipe0 0 of 57600'
want 'PASS' 'M4 on the screen ok' '-> PASS'
want 'NOT RUN' 'M4 on the screen none' 'no picture'
want 'PASS' 'M5 WindowServer' 'LOADED'
want 'FAIL' 'M5 WindowServer bad' 'did not load'
want 'FAIL' 'M6 apps' '1 of 2'
want 'FAIL' 'Chess' 'running NO'                          # a sub-item, shown indented
want 'PASS' 'Maps' 'crash report: none'
want 'PASS' 'M6 apps ok' '2 of 2'
want 'PASS' 'M6 screens' 'pipe 1 changed'
want 'FAIL' 'M6 screens frozen' 'FROZEN'
want 'NOT RUN' 'M6 screens not judged' 'not judged'
want 'INFO' 'flicker' 'NOT MEASURED'                     # NOT MEASURED inside an INFO row: the row's own word wins
want 'PASS' 'flicker' 'A B A B'
want 'WARN' 'copy forward' 'head 0'
want 'INFO' 'A3 display counters' 'refused 10'
want 'INFO' 'display sleep' 'blanked first'
want 'PASS' 'scaled modes' '4 switch\(es\) tried, 0 failed'
want 'FAIL' 'scaled modes bad' '2 failed'
want 'FAIL' 'gamma' 'GAMMA: FAIL'
want 'PASS' 'gamma ok' 'GAMMA: PASS'
want 'PASS' 'IOSurface isolation' 'SURFXPROC: PASS'
want 'INFO' 'power \(last\)' 'activity 100%'
want 'INFO' 'idle cap' 'GFXCLK capped'
want 'INFO' 'something new' 'a row no rule knows'        # unknown: INFO with its text, not dropped
# the verdict word is not repeated in the text of "WARN head 0: ..." rows
grep -qE '^WARN +copy forward +head 0: 190' "$T/out.txt" || fail "verdict word not stripped from the text"
# the "!!" line above the SUMMARY is not a row; every SUMMARY row is
[ "$(grep -cE '^(PASS|FAIL|WARN|INFO|NOT RUN) ' "$T/out.txt")" -eq "$(sed -n '/^SUMMARY/,/^====/p' "$T/triage.txt" | grep -cE '^  [^=S ]|^    ')" ] ||
    fail "row count differs from the SUMMARY's"
grep -q 'scan the same surface' "$T/out.txt" && fail "a line outside the SUMMARY was printed"
grep -qE '^[0-9]+ PASS, [0-9]+ FAIL, [0-9]+ WARN, [0-9]+ INFO, [0-9]+ NOT RUN$' "$T/out.txt" || fail "count line"
grep -qE '^FAIL: .*Chess' "$T/out.txt" || fail "FAIL recap line"
tail -n 1 "$T/out.txt" | grep -q '^RESULT: something failed' || fail "RESULT line last"

# truncation to the width, colour only with a tty / not with --no-color
python3 -I -B "$ROOT/tools/result_checklist.py" --no-color --width 60 "$T/triage.txt" | awk 'length($0) > 60 && !/^RESULT|^FAIL:/ {bad=1} END {exit bad}' || fail "width 60"
grep -q $'\033' "$T/out.txt" && fail "colour with --no-color"

# a triage without a SUMMARY (macOS never sent a report): the "!!" lines, still exit 0
printf '!! no rdna3-report-*.tar.gz: macOS never sent a report\n====\nRESULT: something failed - x\n' > "$T/none.txt"
python3 -I -B "$ROOT/tools/result_checklist.py" --no-color "$T/none.txt" > "$T/out2.txt" || fail "exit status without a SUMMARY"
grep -qE '^FAIL +triage +no rdna3-report' "$T/out2.txt" || fail "no-SUMMARY row"
grep -q '^RESULT: something failed' "$T/out2.txt" || fail "no-SUMMARY RESULT line"

# a missing file: said on stdout, exit 0 (a display tool)
python3 -I -B "$ROOT/tools/result_checklist.py" --no-color "$T/nope.txt" | grep -q 'cannot read' || fail "missing file"
echo "test_result_checklist: OK"
