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
echo "test_triage_a3: all checks passed"
