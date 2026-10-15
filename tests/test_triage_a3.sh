#!/bin/bash
# tools/triage_reports.py's A3 line (IOSurface VRAM) on synthetic reports:
# none, shared with kernel BOs, shared without any (a WARN), refusals.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_triage_a3: FAIL $*"; exit 1; }

mk() {   # mk <name> <29-surface-vram.txt contents or "-" for none>
    python3 -I - "$T/$1" "$2" <<'PY'
import io, sys, tarfile
path, sv = sys.argv[1], sys.argv[2]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=2\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": "",
    "20-log-rdna3.txt": "RDNA3: hello\n",
    "24-log-coredisplay.txt": "",
}
if sv != "-":
    members["29-surface-vram.txt"] = sv.replace("\\n", "\n")
with tarfile.open(path + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
PY
}
mkdir -p "$T/r1" "$T/r2" "$T/r3" "$T/r4"
LIVE="NVMTL: item 7: IOSurface textures live in the surface's family VRAM (one per surface, every process)"
mk r1/rdna3-report-none -
mk r2/rdna3-report-ok "== 20-log-rdna3.txt\\nRDNA3: GEM: IOSurface 12 plane 0 -> 8100 KiB of the accelerator's VRAM at 0x1000000 (A3, import 1)\\n== 26-log-metal-plugin.txt\\n$LIVE"
mk r3/rdna3-report-nobo "== 26-log-metal-plugin.txt\\n$LIVE"
mk r4/rdna3-report-refused "== 20-log-rdna3.txt\\nRDNA3: GEM: IOSurface 9 plane 0 import refused: no such surface for this process (status 0x80000003, call 0, 1 of this cause)\\n== 26-log-metal-plugin.txt\\n$LIVE\\nNVMTL: item 7: surface 9 plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 2 No such file or directory, 8294400 B) - private image + copies (1 so far)"

run() { python3 "$ROOT/tools/triage_reports.py" "$T/$1" 2>&1 || true; }
o=$(run r1); grep -q 'INFO IOSurface VRAM (A3)  *no 29-surface-vram.txt' <<<"$o" || fail "r1: $o"
o=$(run r2); grep -q 'INFO IOSurface VRAM (A3)  *1 process(es) shared, 0 private; 1 kernel BO' <<<"$o" || fail "r2: $o"
o=$(run r3); grep -q 'WARN IOSurface VRAM (A3)  *1 process(es) shared, 0 private; 0 kernel BO' <<<"$o" || fail "r3: $o"
o=$(run r4)
grep -q 'WARN IOSurface VRAM (A3)' <<<"$o" || fail "r4 state: $o"
grep -q 'kernel refused: no such surface for this process' <<<"$o" || fail "r4 kernel: $o"
grep -q 'plugin import refused: VkResult -1000072003, kernel errno 2' <<<"$o" || fail "r4 plugin: $o"
! grep -q Traceback <<<"$(run r1)$(run r2)$(run r3)$(run r4)" || fail "a traceback"

# card run 58: WindowServer's display framebuffers kept out of A3 (the
# policy, not a warning), the refusal that never fired, the accelerator's
# verdict when they were let in, the counters, and a main screen frozen
# through the M6 looks (judged on the main pipe only, when every app ran)
mk58() {   # mk58 <name> <20-log-rdna3.txt> <29-surface-vram.txt> <m6 pictures> [extra tests text]
    # m6 pictures: differ | same (pipe 0 frozen) | same1 (pipe 1 frozen) | tick (pipe 0: one pixel) | two | none
    python3 -I - "$T/$1" "$2" "$3" "$4" "${5:-}" <<'PY'
import io, sys, tarfile
path, log, sv, m6, extra = sys.argv[1:6]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=2\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": '    "RDNA3,Head0,Route" = "OTG0 -> DIGD -> UNIPHYD (mode 3) -> HDMI-4 DDC1"\n',
    "20-log-rdna3.txt": log.replace("\\n", "\n"),
    "24-log-coredisplay.txt": "",
    "29-surface-vram.txt": sv.replace("\\n", "\n"),
}
with tarfile.open(path + ".tar.gz", "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
open(path + "-tests.txt", "w").write(
    "== rdna3_drminfo --scanout-hires (look m6Chess)\npipe 0: LIT otg 0 1920x1080\npipe 1: LIT otg 1 1920x1080\n"
    "2 pipe(s) lit\n== probe: Metal apps (M6)\nM6: 2 of 2 apps ran on the plugin without a crash\n" +
    extra.replace("\\n", "\n"))
looks = ("Chess", "Maps", "Settings", "desktop")[:2 if m6 == "two" else 4]
if m6 != "none":
    for i, look in enumerate(looks):
        for pipe in (0, 1):
            frozen = (pipe == 0 and m6 in ("same", "tick")) or (pipe == 1 and m6 == "same1")
            px = bytearray([40 if frozen else 40 + 50 * i] * (32 * 32 * 3))
            if m6 == "tick" and pipe == 0:
                px[0:3] = bytes([i * 60] * 3)   # the menu bar clock: one pixel of 1024
            with open("%s-scanoutm6%s-pipe%d.ppm" % (path, look, pipe), "wb") as f:
                f.write(b"P6\n32 32\n255\n" + bytes(px))
PY
}
for r in r5 r6 r7 r8 r9 r10 r11 r12 r13 r14 r15 r16 r17 r18; do mkdir -p "$T/$r"; done
KEPTBOOT="RDNA3Accelerator: A3: WindowServer's display framebuffers are kept out of A3: WindowServer composites them through copies the legacy swap pages on (card run 58)"
SCAN="RDNA3Accelerator: RD3DisplayPipe::initFramebufferResource(head 0, index 0, res 0x1) -> vidmem 0x2 over the LIVE SCANOUT phys 0x80000000 1920 x 1080 pitch 7680 (8294400 bytes)"
KEPT="RDNA3Accelerator: surfgem: surface 1 plane 0 refused: WindowServer's display framebuffer of head mask 0x3 (1920x1080): the legacy swaps scan out the legacy surface's own VRAM, never a BO over this one (card run 58) -- copies instead; rdna3a3display=1 lets it in (status 0x80000007, 1 of this cause; ok 0, refused 1)"
PLUGREF="NVMTL: item 7: surface 1 plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 1 Operation not permitted, 8294400 B) - private image + copies (1 so far)"
OTHERPERM="NVMTL: item 7: surface 77 plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 1 Operation not permitted, 65536 B) - private image + copies (2 so far)"
GEM23="RDNA3: GEM: IOSurface 23 plane 0 -> 772 KiB of the accelerator's VRAM at 0xd0f0000 (A3, import 1)"
WSBO="NVMTL: item 7: surface 2 1920x1080 pitch 7680 -> the family's VRAM (vram 1, private 0)"
A3OK="RDNA3Accelerator: a3 display: head 0: 120 legacy swaps, it scans out phys 0x8b100000 (1920x1080); WindowServer's display framebuffers are kept out of A3, composited through copies (4 refused, 9 other WindowServer surfaces, 0 unchecked)"
A3NEVER="RDNA3Accelerator: a3 display: head 0: 8 legacy swaps, it scans out phys 0x8b100000 (1920x1080); WindowServer's display framebuffers are kept out of A3, composited through copies (0 refused, 9 other WindowServer surfaces, 0 unchecked) *** the refusal never fired ***"
A3BAD="RDNA3Accelerator: a3 display: head 0: 120 legacy swaps, 0 scanned out an A3 BO; it shows phys 0x8b100000, WindowServer drew display surface 1 at phys 0x8d0f0000 last (2 BO(s) that may be this head's, e.g. 0x8d0f0000) -- *** WINDOWSERVER DRAWS WHERE THE PANEL DOES NOT READ (card run 58) ***"
mk58 r5/rdna3-report-kept "RDNA3: hello\\n$KEPTBOOT\\n$SCAN\\n$KEPT\\n$A3OK" "== 20-log-rdna3.txt\\n$KEPT\\n$GEM23\\n== 26-log-metal-plugin.txt\\n$LIVE\\n$PLUGREF" differ
mk58 r6/rdna3-report-apart "RDNA3: hello\\nRDNA3Accelerator: A3: WindowServer's display framebuffers MAY become BOs over their own VRAM\\n$A3BAD" "== 26-log-metal-plugin.txt\\n$LIVE" none
mk58 r7/rdna3-report-frozen "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" same
mk58 r8/rdna3-report-moving "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ
mk58 r9/rdna3-report-missed "RDNA3: hello\\n$KEPTBOOT\\n$SCAN" "== 20-log-rdna3.txt\\n$GEM23\\n== 24-log-coredisplay.txt\\n$LIVE\\n$WSBO" none
mk58 r10/rdna3-report-never "RDNA3: hello\\n$KEPTBOOT\\n$A3NEVER" "== 26-log-metal-plugin.txt\\n$LIVE" none
mk58 r11/rdna3-report-quiet "RDNA3: hello\\n$KEPTBOOT\\n$SCAN" "== 20-log-rdna3.txt\\n$GEM23\\n== 26-log-metal-plugin.txt\\n$LIVE" none
mk58 r12/rdna3-report-norun "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" same "M6 Chess: running NO, plugin NOT loaded, crash report: none\\n"
mk58 r13/rdna3-report-side "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" same1
mk58 r14/rdna3-report-tick "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" tick
mk58 r15/rdna3-report-two "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" two
mk58 r16/rdna3-report-ctr "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "debug.rdna3accel_a3display: 1\\ndebug.rdna3accel_a3display_imported: 4\\ndebug.rdna3accel_swaps_h0: 500\\ndebug.rdna3accel_swaps_a3_h0: 0\\n"
mk58 r17/rdna3-report-oldk "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ "(no rdna3accel_a3display sysctl: kext older than card run 58)\\n"
mk58 r18/rdna3-report-perm "RDNA3: hello\\n$KEPTBOOT\\n$SCAN\\n$KEPT" "== 20-log-rdna3.txt\\n$KEPT\\n$GEM23\\n== 26-log-metal-plugin.txt\\n$LIVE\\n$PLUGREF\\n$OTHERPERM" differ
o=$(run r5)
grep -q 'INFO IOSurface VRAM (A3)' <<<"$o" || fail "r5: display refusals are the policy, not a WARN: $o"
grep -q 'INFO A3 display  *WindowServer.s display framebuffers kept out of A3: 1 surface(s) refused' <<<"$o" || fail "r5 A3 display: $o"
grep -q 'a3 display: head 0: 120 legacy swaps' <<<"$o" || fail "r5 verdict line: $o"
! grep -qE 'FAIL (A3 display|M6 screen)' <<<"$o" || fail "r5 no A3 display / M6 screen FAIL: $o"
o=$(run r6)
grep -q 'FAIL A3 display  *WindowServer draws display surfaces into A3 BOs no legacy swap scans out' <<<"$o" || fail "r6: $o"
grep -q 'DOES NOT READ' <<<"$o" || fail "r6 the line: $o"
o=$(run r7)
grep -q 'FAIL M6 screen: pipe 0 (main display) showed the same picture in all 4 M6 looks (Chess, Maps, Settings, desktop' <<<"$o" || fail "r7: $o"
grep -q 'PASS M6 screen: pipe 1 changed' <<<"$o" || fail "r7 pipe 1 changed: $o"
grep -q 'M6 screens  *pipe 0 (main) FROZEN in all 4 looks' <<<"$o" || fail "r7 summary: $o"
o=$(run r8)
! grep -q 'FAIL M6 screen' <<<"$o" || fail "r8: $o"
grep -q "M6 screens  *pipe 0 changed" <<<"$o" || fail "r8 summary: $o"
o=$(run r9)
grep -q 'FAIL A3 display  *the refusal never fired: 1 WindowServer surface(s) of a scanout size became BOs (2)' <<<"$o" || fail "r9: $o"
o=$(run r10)
grep -q 'FAIL A3 display  *the refusal never fired' <<<"$o" || fail "r10: $o"
o=$(run r11)
grep -q 'INFO A3 display  *kept-out mode, but no display framebuffer import seen' <<<"$o" || fail "r11: $o"
o=$(run r12)
grep -q 'INFO M6 screen: pipe 0 (main display) did not change in the M6 looks, not judged: Chess did not run' <<<"$o" || fail "r12: $o"
! grep -q 'FAIL M6 screen' <<<"$o" || fail "r12 no FAIL: $o"
o=$(run r13)
grep -q 'INFO M6 screen: pipe 1 did not change in the M6 looks .*not the main display' <<<"$o" || fail "r13: $o"
! grep -q 'FAIL M6 screen' <<<"$o" || fail "r13 no FAIL: $o"
o=$(run r14)
grep -q 'FAIL M6 screen: pipe 0 (main display) showed the same picture' <<<"$o" || fail "r14: one clock pixel must not clear a frozen pipe: $o"
o=$(run r15)
grep -q 'INFO M6 screen: pipe 0 (main display): 2 M6 look(s), not judged' <<<"$o" || fail "r15: $o"
o=$(run r16)
grep -q 'FAIL A3 display: 4 display surface(s) imported (rdna3a3display=1) and no legacy swap ever scanned one out' <<<"$o" || fail "r16: $o"
grep -q 'A3 display counters  *refused ?, imported 4' <<<"$o" || fail "r16 summary: $o"
o=$(run r17)
grep -q 'INFO A3 display counters: none (the kext predates card run 58)' <<<"$o" || fail "r17: $o"
o=$(run r18)
grep -q 'WARN IOSurface VRAM (A3)' <<<"$o" || fail "r18: another EPERM refusal stays a warning: $o"
grep -q 'plugin import refused: VkResult -1000072003, kernel errno 1' <<<"$o" || fail "r18 line: $o"
all=""
for r in r5 r6 r7 r8 r9 r10 r11 r12 r13 r14 r15 r16 r17 r18; do all="$all$(run $r)"; done
! grep -q Traceback <<<"$all" || fail "a traceback (run 58 cases)"
echo "test_triage_a3: all checks passed"
