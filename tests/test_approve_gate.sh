#!/bin/bash
# tools/vm/rdna3vm: a card step must not run an unapproved accelerator without
# saying so (card runs 2026-10-18b and 2026-10-19b: macOS kept loading the old
# RDNA3Accelerator, the new build was never allowed, three 'step play' runs
# tested the old driver). The functions are cut out of the script and run with
# stubs for sudo, the VM and the approval window:
#  - install_first: an approve marker left by the last card boot -> says the
#    last run tested the OLD build, prints the marker, calls cmd_approve once
#    and removes the marker (also when the install boot deleted it meanwhile);
#    needs-allow alone still calls cmd_approve; nothing pending -> nothing
#  - cmd__once: an approve marker after the run -> no rerun, the marker stays
#    (rerun marker dropped); a plain rerun still happens; an approve marker
#    after the rerun stays too
#  - cmd_approve: clears the marker (before the card launch with --card, after
#    the window otherwise) so the next step does not ask twice
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_approve_gate: FAIL $*"; [ -f "$T/out.txt" ] && cat "$T/out.txt"; exit 1; }
RV=$ROOT/tools/vm/rdna3vm

{ grep -E '^approve_marker_(text|clear)\(\)' "$RV"
  for f in install_first cmd__once cmd_approve; do sed -n "/^$f() {/,/^}/p" "$RV"; done; } > "$T/fn.sh"
[ "$(grep -c '^[a-z_]*() ' "$T/fn.sh")" = 5 ] || fail "functions not found: $(grep -o '^[a-z_]*()' "$T/fn.sh")"

export WORK=$T/work
mkdir -p "$WORK/reports"
# a fake rdna3vm for 'bash $SELF _install / _passthrough': counts the runs,
# writes the markers the case asks for ($WORK/pass-<n> = approve and/or rerun)
cat > "$T/self.sh" <<'SC'
#!/bin/bash
case "$1" in
    _install) echo install >> "$WORK/calls" ;;
    _passthrough)
        echo passthrough >> "$WORK/calls"
        n=$(grep -c passthrough "$WORK/calls")
        w=$(cat "$WORK/pass-$n" 2>/dev/null || true)
        case "$w" in *approve*) echo "RDNA3Accelerator: running e35e78c275, installed 4fcf2bc0b7 (not approved yet)" > "$WORK/reports/approve-x.txt" ;; esac
        case "$w" in *rerun*) echo r > "$WORK/reports/rerun-x.txt" ;; esac ;;
esac
SC
cat > "$T/h.sh" <<'SC'
set -euo pipefail
SELF=$T/self.sh
say() { echo "== $*"; }
sleep() { :; }
step_args() { echo "rdna3lvl=2"; }
cmd_window() { echo "window $*" >> "$WORK/calls"; }
launch() { [ ! -e "$WORK"/reports/approve-x.txt ] || echo "LAUNCH WITH MARKER" >> "$WORK/calls"; echo "launch" >> "$WORK/calls"; }
# sudo env RDNA3VM_DIR=.. bash $SELF _install: the install boot may delete the markers
sudo() {
    if [ "$1" = env ]; then bash "$SELF" _install; [ -z "${INSTALL_DELETES:-}" ] || rm -f "$WORK"/reports/approve-*.txt; else "$@"; fi
}
. "$T/fn.sh"
SC
run() {   # run <body>: a fresh WORK, the stubs, then the body
    rm -rf "$WORK"; mkdir -p "$WORK/reports"; : > "$WORK/calls"
    ( export T; . "$T/h.sh"; eval "$1" ) > "$T/out.txt" 2>&1
}
MARK="RDNA3Accelerator: running e35e78c275, installed 4fcf2bc0b7 (not approved yet)"
calls() { tr '\n' ' ' < "$WORK/calls"; }

# ---- install_first
STUB='cmd_approve() { echo approve >> "$WORK/calls"; }'
run "$STUB; echo '$MARK' > \$WORK/reports/approve-1.txt; install_first"
[ "$(calls)" = "install approve " ] || fail "marker: calls [$(calls)]"
[ ! -e "$WORK/reports/approve-1.txt" ] || fail "marker: the approve marker is still there"
grep -q 'the last card run tested the OLD framebuffer/accelerator' "$T/out.txt" || fail "marker: no OLD-driver warning"
grep -q 'RDNA3Accelerator: running e35e78c275, installed 4fcf2bc0b7 (not approved yet)' "$T/out.txt" || fail "marker: the marker text is not printed"

# the install boot deletes the markers: install_first read it before
INSTALL_DELETES=1 run "$STUB; echo '$MARK' > \$WORK/reports/approve-1.txt; install_first"
[ "$(calls)" = "install approve " ] || fail "marker deleted by the install boot: calls [$(calls)]"

run "$STUB; install_first"
[ "$(calls)" = "install " ] || fail "nothing pending: calls [$(calls)]"
! grep -q 'OLD' "$T/out.txt" || fail "nothing pending: warned"

run "$STUB; echo 'framebuffer ' > \$WORK/needs-allow; install_first"
[ "$(calls)" = "install approve " ] || fail "needs-allow: calls [$(calls)]"
[ ! -e "$WORK/needs-allow" ] || fail "needs-allow not removed"
! grep -q 'OLD' "$T/out.txt" || fail "needs-allow alone claimed the last run tested the OLD build"

run "$STUB; echo 'x ' > \$WORK/needs-allow; echo '$MARK' > \$WORK/reports/approve-1.txt; install_first"
[ "$(calls)" = "install approve " ] || fail "both: one approval expected, calls [$(calls)]"

# ---- cmd__once
run "echo approve > \$WORK/pass-1; cmd__once"
[ "$(calls)" = "install passthrough " ] || fail "once+approve: no rerun expected, calls [$(calls)]"
[ -e "$WORK/reports/approve-x.txt" ] || fail "once+approve: the marker must stay for install_first / result"
grep -q 'tested the OLD driver, not rerunning' "$T/out.txt" || fail "once+approve: no message"

run "echo 'approve rerun' > \$WORK/pass-1; cmd__once"
[ "$(calls)" = "install passthrough " ] || fail "once+approve+rerun marker: calls [$(calls)]"
[ ! -e "$WORK/reports/rerun-x.txt" ] || fail "once+approve+rerun marker: the rerun marker stays"
[ -e "$WORK/reports/approve-x.txt" ] || fail "once+approve+rerun marker: the approve marker is gone"

run "echo rerun > \$WORK/pass-1; cmd__once"
[ "$(calls)" = "install passthrough passthrough " ] || fail "once+rerun: calls [$(calls)]"
[ ! -e "$WORK/reports/rerun-x.txt" ] || fail "once+rerun: the rerun marker stays"
! grep -q 'OLD driver' "$T/out.txt" || fail "once+rerun: false OLD-driver message"

run "echo rerun > \$WORK/pass-1; echo approve > \$WORK/pass-2; cmd__once"
[ "$(calls)" = "install passthrough passthrough " ] || fail "once+rerun+approve: calls [$(calls)]"
[ -e "$WORK/reports/approve-x.txt" ] || fail "once+rerun+approve: the marker must stay"
grep -q 'the rerun ran an older framebuffer/accelerator' "$T/out.txt" || fail "once+rerun+approve: no message"

# a marker of an earlier run is install_first's: only this boot's count
run "echo '$MARK' > \$WORK/reports/approve-x.txt; cmd__once"
[ ! -e "$WORK/reports/approve-x.txt" ] || fail "once: an old marker was taken for this boot's"
! grep -q 'OLD driver' "$T/out.txt" || fail "once: old marker reported as this run's"

# ---- cmd_approve clears the marker
run "echo '$MARK' > \$WORK/reports/approve-x.txt; cmd_approve"
[ ! -e "$WORK/reports/approve-x.txt" ] || fail "approve (window): the marker stays"
grep -q 'window --approve' "$WORK/calls" || fail "approve (window): no window"
run "echo '$MARK' > \$WORK/reports/approve-x.txt; cmd_approve --card"
[ ! -e "$WORK/reports/approve-x.txt" ] || fail "approve --card: the marker stays"
! grep -q 'LAUNCH WITH MARKER' "$WORK/calls" || fail "approve --card: launch (install_first) would ask for the approval again"

echo "test_approve_gate: ok"
