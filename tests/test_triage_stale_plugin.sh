#!/bin/bash
# tools/triage_reports.py's "STALE PLUGIN/RADV" line on synthetic reports:
#  - no stamp: the plugin log has the G14 line but no extended_dynamic_state
#    line -> printed; with the ENABLED (or ABSENT) line -> not printed
#  - stamps: the build's last metal/ (userspace/mesa) commit older than the
#    repo's -> printed; equal or newer -> not printed; unknown to the clone
#    or no history -> an INFO line, no FAIL
#  - card run 2026-10-17c: WindowServer loaded a stale plugin that predates
#    the plugin's frame trace -> "plugin trace MISSING" names that as the
#    reason (not the collector's generic list)
# The stamp cases need git history, so the triage runs from a copy inside a
# throw-away repo built here (it finds its repo from its own location).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_triage_stale_plugin: FAIL $*"; exit 1; }

# a repo: c1 metal, c2 mesa, c3 metal, c4 other, c5 mesa
R=$T/repo
mkdir -p "$R/tools" "$R/metal" "$R/userspace/mesa" "$R/docs"
cp "$ROOT/tools/triage_reports.py" "$R/tools/"
g() { git -C "$R" -c user.name=t -c user.email=t@t -c commit.gpgsign=false -c core.hooksPath=/dev/null "$@"; }
g init -q
commit() {   # commit <file> <message>: prints the new short id
    echo "$2" >> "$R/$1"; g add -A; g commit -q -m "$2"; g rev-parse --short HEAD
}
C1=$(commit metal/a.c "c1 metal")
C2=$(commit userspace/mesa/b.sh "c2 mesa")
C3=$(commit metal/a.c "c3 metal")
C4=$(commit docs/x.md "c4 other")
C5=$(commit userspace/mesa/b.sh "c5 mesa")

G14="NVMTL: vk: G14 primitive topology is dynamic; points/lines on a triangle-class pipeline: NO"
EDS="NVMTL: vk: VK_EXT_extended_dynamic_state ENABLED (dynamic depth/stencil state, PS epilogs)"

mk() {   # mk <dir> <host sha or -> <26-log lines> <35-build-stamps contents or -> [24-log-coredisplay]
    mkdir -p "$T/$1"
    [ "$2" = - ] || echo "repo $2 something" > "$T/$1/00-host.txt"
    python3 -I - "$T/$1/rdna3-report-x.tar.gz" "$3" "$4" "${5:-}" <<'PY'
import io, sys, tarfile
path, plog, stamps, cdlog = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
members = {
    "01-boot-args.txt": "boot-args rdna3lvl=8 rdna3fb=1 rdna3accel=1 rdna3agdc=2\n",
    "03-kexts.txt": "RDNA3Accel\n",
    "11-ioreg-props.txt": "",
    "20-log-rdna3.txt": "RDNA3: hello\n",
    "24-log-coredisplay.txt": cdlog,
    "26-log-metal-plugin.txt": plog.replace("\\n", "\n"),
}
if stamps != "-":
    members["35-build-stamps.txt"] = stamps.replace("\\n", "\n")
with tarfile.open(path, "w:gz") as t:
    for n, body in members.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
PY
}
run() { python3 -I "$R/tools/triage_reports.py" "$T/$1" 2>&1 || true; }
stamp() {   # stamp <plugin metal sha or -> <radv mesa sha or ->
    local p r
    if [ "$1" = - ]; then p="no stamp (built before stamps existed)"; else p="built=2026-10-15 10:00:00\\nhead=$1\\nmetal=$1"; fi
    if [ "$2" = - ]; then r="no stamp (built before stamps existed)"; else r="built=2026-10-15 10:00:00\\nhead=$2\\nmesa=$2"; fi
    printf '== plugin (/Library/GPUBundles/rdna3mtl/metal.stamp)\\n%s\\n== radv (/x/radv.stamp)\\n%s\\n' "$p" "$r"
}
STALE='FAIL STALE PLUGIN/RADV'

# -- no stamp: the log signature
mk s1 "$C5" "$G14" -
o=$(run s1); grep -q "$STALE: PLUGIN log has the 'vk: G14 primitive topology' line but no" <<<"$o" || fail "s1 stale signature: $o"
grep -q 'run build_radv.sh and build_metal.sh in the VM (docs/TESTING-VM.md)' <<<"$o" || fail "s1 hint: $o"
mk s2 "$C5" "$G14\\n$EDS" -
o=$(run s2); ! grep -q "$STALE" <<<"$o" || fail "s2 eds ENABLED printed STALE: $o"
mk s3 "$C5" "$G14\\nNVMTL: vk: VK_EXT_extended_dynamic_state ABSENT" -
o=$(run s3); ! grep -q "$STALE" <<<"$o" || fail "s3 eds ABSENT printed STALE: $o"
mk s4 - "" -
o=$(run s4); ! grep -q "$STALE" <<<"$o" || fail "s4 no plugin at all printed STALE: $o"

# -- stamps (host commit C5: the repo's last metal/ commit is C3, mesa C5)
mk t1 "$C5" "$G14\\n$EDS" "$(stamp "$C1" "$C5")"
o=$(run t1); grep -q "$STALE: PLUGIN built from metal at $C1, the repo's last commit there is $C3" <<<"$o" || fail "t1 old plugin: $o"
! grep -q 'RADV built' <<<"$o" || fail "t1 current RADV flagged: $o"
mk t2 "$C5" "$G14\\n$EDS" "$(stamp "$C3" "$C2")"
o=$(run t2); grep -q "$STALE: RADV built from userspace/mesa at $C2, the repo's last commit there is $C5" <<<"$o" || fail "t2 old RADV: $o"
! grep -q 'PLUGIN built' <<<"$o" || fail "t2 current plugin flagged: $o"
mk t3 "$C5" "$G14\\n$EDS" "$(stamp "$C3" "$C5")"
o=$(run t3); ! grep -q "$STALE" <<<"$o" || fail "t3 current stamps printed STALE: $o"
grep -q "INFO build stamps" <<<"$o" || fail "t3 no stamps line: $o"
# the host commit older than the build: the build is newer, fine
mk t4 "$C2" "$G14\\n$EDS" "$(stamp "$C3" "$C5")"
o=$(run t4); ! grep -q "$STALE" <<<"$o" || fail "t4 newer build printed STALE: $o"
# a stamp commit this clone does not have: cannot tell, no FAIL
mk t5 "$C5" "$G14\\n$EDS" "$(stamp deadbeef0 deadbeef1)"
o=$(run t5); ! grep -q "$STALE" <<<"$o" || fail "t5 unknown stamp printed STALE: $o"
grep -q 'cannot tell which is older' <<<"$o" || fail "t5 no explanation: $o"
# a stamp the build could not fill in ("unknown")
mk t6 "$C5" "$G14\\n$EDS" "$(stamp unknown unknown)"
o=$(run t6); ! grep -q "$STALE" <<<"$o" || fail "t6 unknown printed STALE: $o"
# no 00-host.txt: HEAD of the clone is the reference
mk t7 - "$G14\\n$EDS" "$(stamp "$C1" "$C5")"
o=$(run t7); grep -q "$STALE: PLUGIN built from metal at $C1" <<<"$o" || fail "t7 without 00-host: $o"
# stamp says current but the log has no eds line: the log wins
mk t8 "$C5" "$G14" "$(stamp "$C3" "$C5")"
o=$(run t8); grep -q "$STALE: PLUGIN log has the" <<<"$o" || fail "t8 log vs stamp: $o"
# card run 2026-10-17c: WindowServer loaded a plugin built before the plugin's frame trace existed:
# the MISSING line names the stale build as the reason
mkdir -p "$R/metal/plugin"
C6=$(commit metal/plugin/NVMTLTrace.m "c6 plugin trace")
WSLOG="2026-10-17 13:20:15.056 E  WindowServer[158:29f] NVMTL: loaded"
mk t10 "$C6" "$G14\\n$EDS" "$(stamp "$C3" "$C5")" "$WSLOG"
o=$(run t10)
grep -qF "plugin trace MISSING although WindowServer loaded the plugin: the VM's plugin is STALE: PLUGIN built from metal at $C3, the repo's last commit there is $C6" <<<"$o" || fail "t10 stale plugin not the reason: $o"
grep -qF "a plugin without the frame trace (metal/plugin/NVMTLTrace.m came with $C6 after it)" <<<"$o" || fail "t10 no trace source named: $o"
# a current plugin without a trace: the collector's reason as before, no STALE in the MISSING line
mk t11 "$C6" "$G14\\n$EDS" "$(stamp "$C6" "$C5")" "$WSLOG"
o=$(run t11)
grep -qF "plugin trace MISSING" <<<"$o" || fail "t11 no MISSING line: $o"
! grep -q "plugin trace MISSING.*STALE" <<<"$o" || fail "t11 a current plugin called stale: $o"
# no git history at all: only the host's own repo-metal / repo-mesa lines
mk t9 - "$G14\\n$EDS" "$(stamp "$C3" "$C5")"
rm -rf "$R/.git"
printf 'repo 1234567 x\nrepo-metal %s\nrepo-mesa %s\n' "$C5" "$C5" > "$T/t9/00-host.txt"
o=$(run t9); ! grep -q "$STALE" <<<"$o" || fail "t9 no git printed STALE: $o"
grep -q 'cannot tell which is older\|no git history' <<<"$o" || fail "t9 no explanation: $o"
! grep -q Traceback <<<"$(for d in s1 s2 s3 s4 t1 t2 t3 t4 t5 t6 t7 t8 t9 t10 t11; do run $d; done)" || fail "a traceback"
echo "test_triage_stale_plugin: all checks passed"
