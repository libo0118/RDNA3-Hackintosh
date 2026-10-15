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
# policy, not a warning), the accelerator's verdict when they were let in,
# and a main screen frozen through the M6 looks
mk58() {   # mk58 <name> <20-log-rdna3.txt> <29-surface-vram.txt> <m6 pipe 0 pictures: same|differ|none>
    python3 -I - "$T/$1" "$2" "$3" "$4" <<'PY'
import io, sys, tarfile
path, log, sv, m6 = sys.argv[1:5]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=2\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": "",
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
open(path + "-tests.txt", "w").write("== probe: Metal apps (M6)\nM6: 2 of 2 apps ran on the plugin without a crash\n")
if m6 != "none":
    for i, look in enumerate(("Chess", "Maps", "Settings", "desktop")):
        for pipe in (0, 1):
            v = 40 if (pipe == 0 and m6 == "same") else 40 + 50 * i
            with open("%s-scanoutm6%s-pipe%d.ppm" % (path, look, pipe), "wb") as f:
                f.write(b"P6\n4 4\n255\n" + bytes([v, v, v]) * 16)
PY
}
mkdir -p "$T/r5" "$T/r6" "$T/r7" "$T/r8"
KEPT="RDNA3Accelerator: surfgem: surface 1 plane 0 refused: WindowServer's display framebuffer of head 0 (1920x1080): the swaps scan out the legacy surface's own VRAM, never a BO over this one (card run 58) -- copies instead; rdna3a3display=1 lets it in (status 0x80000007, 1 of this cause; ok 0, refused 1)"
PLUGREF="NVMTL: item 7: surface 1 plane 0: family VRAM import refused (VkResult -1000072003, kernel errno 1 Operation not permitted, 8294400 B) - private image + copies (1 so far)"
A3OK="RDNA3Accelerator: a3 display: head 0: 120 swaps, it scans out phys 0x8b100000 (1920x1080); WindowServer's display framebuffers are kept out of A3, composited through copies (4 refused)"
A3BAD="RDNA3Accelerator: a3 display: head 0: 120 swaps, 0 scanned out an A3 BO; it shows phys 0x8b100000, WindowServer drew display surface 1 at phys 0x8d0f0000 last (2 BO(s) of this head, e.g. 0x8d0f0000) -- *** WINDOWSERVER DRAWS WHERE THE PANEL DOES NOT READ (card run 58) ***"
mk58 r5/rdna3-report-kept "RDNA3: hello\\n$KEPT\\n$A3OK" "== 20-log-rdna3.txt\\n$KEPT\\nRDNA3: GEM: IOSurface 23 plane 0 -> 772 KiB of the accelerator's VRAM at 0xd0f0000 (A3, import 1)\\n== 26-log-metal-plugin.txt\\n$LIVE\\n$PLUGREF" differ
mk58 r6/rdna3-report-apart "RDNA3: hello\\nRDNA3Accelerator: A3: WindowServer's display framebuffers MAY become BOs over their own VRAM\\n$A3BAD" "== 26-log-metal-plugin.txt\\n$LIVE" none
mk58 r7/rdna3-report-frozen "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" same
mk58 r8/rdna3-report-moving "RDNA3: hello" "== 26-log-metal-plugin.txt\\n$LIVE" differ
o=$(run r5)
grep -q 'INFO IOSurface VRAM (A3)' <<<"$o" || fail "r5: display refusals are the policy, not a WARN: $o"
grep -q 'INFO A3 display  *WindowServer.s display framebuffers kept out of A3' <<<"$o" || fail "r5 A3 display: $o"
grep -q 'a3 display: head 0: 120 swaps' <<<"$o" || fail "r5 verdict line: $o"
! grep -qE 'FAIL (A3 display|M6 screen)' <<<"$o" || fail "r5 no A3 display / M6 screen FAIL: $o"
o=$(run r6)
grep -q 'FAIL A3 display  *WindowServer draws display surfaces into A3 BOs no swap scans out' <<<"$o" || fail "r6: $o"
grep -q 'DOES NOT READ' <<<"$o" || fail "r6 the line: $o"
o=$(run r7)
grep -q 'FAIL M6 screen: pipe 0 showed the same picture in all 4 M6 looks (Chess, Maps, Settings, desktop)' <<<"$o" || fail "r7: $o"
! grep -q 'M6 screen: pipe 1' <<<"$o" || fail "r7 pipe 1 changed: $o"
grep -q 'M6 screens  *pipe 0 FROZEN in all 4 looks' <<<"$o" || fail "r7 summary: $o"
o=$(run r8)
! grep -q 'FAIL M6 screen' <<<"$o" || fail "r8: $o"
grep -q "M6 screens  *every pipe's picture changed" <<<"$o" || fail "r8 summary: $o"
! grep -q Traceback <<<"$(run r5)$(run r6)$(run r7)$(run r8)" || fail "a traceback (run 58 cases)"
echo "test_triage_a3: all checks passed"
