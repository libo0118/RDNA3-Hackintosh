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
#    the window otherwise) so the next step does not ask twice, but keeps
#    $WORK/approve-unverified (no proof Allow was clicked): cmd__once clears it
#    only for a boot without an approve marker and with the current bundle; a
#    boot with the marker again says "Allow was not clicked / did not take"
#  - the notes of cmd__once (it runs under systemd-run: no terminal) go to the
#    newest host-*.log and $WORK/last-run-note; install_first prints and clears
#    the note, 'rdna3vm result' prints it and puts it into 00-host.txt
#  - a failing approval window does not abort install_first silently
#  - the queue sets the marker aside (approve-seen-*.txt) and says the step ran
#    on the old driver
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf -- "$T"' EXIT
fail() { echo "test_approve_gate: FAIL $*"; [ -f "$T/out.txt" ] && cat "$T/out.txt"; exit 1; }
RV=$ROOT/tools/vm/rdna3vm
export ROOT

FUNCS="approve_markers approve_marker_any approve_marker_text approve_marker_clear run_note old_driver_note
bundle_current vm_bundle_seen install_first cmd__once cmd_approve cmd__queue cmd_result approve_cmd host_opt"
for f in $FUNCS; do sed -n "/^$f() {/,/^}/p" "$RV"; done > "$T/fn.sh"
[ "$(grep -c '^[a-z_]*() ' "$T/fn.sh")" = 15 ] || fail "functions not found: $(grep -o '^[a-z_]*()' "$T/fn.sh")"

export WORK=$T/work
mkdir -p "$WORK/reports"
# a fake rdna3vm for 'bash $SELF _install / _passthrough': counts the runs,
# writes the markers the case asks for ($WORK/pass-<n> = approve and/or rerun)
cat > "$T/self.sh" <<'SC'
#!/bin/bash
case "$1" in
    _install) echo install >> "$WORK/calls" ;;
    window) echo "window $*" >> "$WORK/calls"; exit "${WINDOW_RC:-0}" ;;
    _passthrough)
        echo passthrough >> "$WORK/calls"
        echo "boot-args: x" > "$WORK/reports/host-run.log"
        [ -z "${BUNDLE_SEEN:-}" ] || echo "$BUNDLE_SEEN" > "$WORK/reports/bundle-x.txt"
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
die() { echo "die: $*" >&2; exit 1; }
sleep() { :; }
step_args() { echo "rdna3lvl=2"; }
load_conf() { :; }
auto_limit() { echo 5; }
QUEUE=$WORK/queue
RUN_MARK=$WORK/run-start
BASE_ARGS=b; DEBUG_ARGS=d
systemctl() { :; }
checklist() { :; }
src_rev() { :; }
git() { echo "repo abc1234 stub"; }
journalctl() { :; }
launch() { [ ! -e "$WORK"/reports/approve-x.txt ] || echo "LAUNCH WITH MARKER" >> "$WORK/calls"; echo "launch" >> "$WORK/calls"; }
# sudo env RDNA3VM_DIR=.. bash $SELF _install: the install boot may delete the markers
sudo() {
    if [ "$1" = env ]; then bash "$SELF" _install; [ -z "${INSTALL_DELETES:-}" ] || rm -f "$WORK"/reports/approve-*.txt; else "$@"; fi
}
. "$T/fn.sh"
ready() {
    mkdir -p "$WORK/share"; echo "$BUNDLE_OK" > "$WORK/share/rdna3-bundle.sha256"
    echo "$MARK" > "$WORK/approve-unverified"
}
SC
run() {   # run <body>: a fresh WORK, the stubs, then the body
    rm -rf "$WORK"; mkdir -p "$WORK/reports"; : > "$WORK/calls"
    ( export T; . "$T/h.sh"; eval "$1" ) > "$T/out.txt" 2>&1
}
MARK="RDNA3Accelerator: running e35e78c275, installed 4fcf2bc0b7 (not approved yet)"
BUNDLE_OK=$(printf 'b%.0s' $(seq 64))
export MARK BUNDLE_OK
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

# ---- cmd_approve keeps approve-unverified (no proof Allow was clicked)
run "echo '$MARK' > \$WORK/reports/approve-x.txt; cmd_approve"
grep -qF "$MARK" "$WORK/approve-unverified" || fail "approve: approve-unverified lacks the marker text: $(cat "$WORK/approve-unverified" 2>&1)"
run "cmd_approve"
[ -s "$WORK/approve-unverified" ] || fail "approve without a marker: no approve-unverified"
run "echo '$MARK' > \$WORK/reports/approve-x.txt; cmd_approve --card"
grep -qF "$MARK" "$WORK/approve-unverified" || fail "approve --card: approve-unverified lacks the marker text"

# a window that fails keeps the marker and says so (cmd_approve returns its status)
WINDOW_RC=3 run "echo '$MARK' > \$WORK/reports/approve-x.txt; rc=0; cmd_approve || rc=\$?; echo rc=\$rc"
grep -q 'rc=3' "$T/out.txt" || fail "approve, window rc 3: cmd_approve hid the status"
grep -q 'the approval window ended with status 3' "$T/out.txt" || fail "approve, window rc 3: no message"
[ -e "$WORK/reports/approve-x.txt" ] || fail "approve, window rc 3: the marker was cleared"

# install_first: a failing window is reported, not a silent abort; marker and needs-allow stay
WINDOW_RC=3 run "echo '$MARK' > \$WORK/reports/approve-1.txt; echo 'framebuffer ' > \$WORK/needs-allow; install_first; echo AFTER"
grep -q '^AFTER' "$T/out.txt" || fail "install_first, window rc 3: aborted without going on"
grep -q 'the approval window ended with status 3: the new build may NOT be allowed' "$T/out.txt" || fail "install_first, window rc 3: no message"
[ -e "$WORK/reports/approve-1.txt" ] && [ -e "$WORK/needs-allow" ] || fail "install_first, window rc 3: marker / needs-allow dropped"
! grep -q 'Window closed' "$T/out.txt" || fail "install_first, window rc 3: claimed the window closed fine"
run "echo '$MARK' > \$WORK/reports/approve-1.txt; install_first"
grep -q 'Window closed (whether Allow was clicked shows in the next run)' "$T/out.txt" || fail "install_first: no honest closing line"

# ---- cmd__once: the approve-unverified lifecycle and the notes
# ready (in h.sh): the published bundle is b*64, approve-unverified is present
# boot without a marker, VM has the current bundle: Allow took
BUNDLE_SEEN=$BUNDLE_OK run "ready; cmd__once"
[ ! -e "$WORK/approve-unverified" ] || fail "once, clean boot + current bundle: approve-unverified stays"
# ... with an older bundle in the VM: not proven
BUNDLE_SEEN=$(printf 'c%.0s' $(seq 64)) run "ready; cmd__once"
[ -e "$WORK/approve-unverified" ] || fail "once, clean boot + old bundle: approve-unverified cleared"
# ... no bundle report at all: not proven either
run "ready; cmd__once"
[ -e "$WORK/approve-unverified" ] || fail "once, clean boot without a bundle report: approve-unverified cleared"
# a boot with the approve marker again: Allow did not take, said plainly, in the log and the note
BUNDLE_SEEN=$BUNDLE_OK run "ready; echo approve > \$WORK/pass-1; cmd__once"
[ -e "$WORK/approve-unverified" ] || fail "once, marker again: approve-unverified cleared"
for f in "$T/out.txt" "$WORK/last-run-note" "$WORK/reports/host-run.log"; do
    grep -q 'Allow was not clicked / did not take: macOS still runs RDNA3Accelerator: running e35e78c275, installed 4fcf2bc0b7' "$f" ||
        fail "once, marker again: no 'Allow was not clicked' in $f"
done
# the same through a rerun
BUNDLE_SEEN=$BUNDLE_OK run "ready; echo rerun > \$WORK/pass-1; echo approve > \$WORK/pass-2; cmd__once"
grep -q 'Allow was not clicked / did not take' "$WORK/last-run-note" || fail "once, rerun marker again: no note"
[ -e "$WORK/approve-unverified" ] || fail "once, rerun marker again: approve-unverified cleared"

# ---- M1: the notes of a run with the old driver reach the host log and last-run-note
run "echo approve > \$WORK/pass-1; cmd__once"
grep -q 'tested the OLD driver, not rerunning' "$WORK/last-run-note" || fail "note: last-run-note lacks the message"
grep -q 'tested the OLD driver, not rerunning' "$WORK/reports/host-run.log" || fail "note: the host log lacks the message"
grep -q "run 'rdna3vm approve', then the step again" "$WORK/reports/host-run.log" || fail "note: the host log lacks the action"
run "echo rerun > \$WORK/pass-1; echo approve > \$WORK/pass-2; cmd__once"
grep -q 'the rerun ran an older framebuffer/accelerator' "$WORK/reports/host-run.log" || fail "note: the rerun message is not in the host log"
# a run without a problem leaves no note, and clears the note of the run before
run "echo 'stale note' > \$WORK/last-run-note; cmd__once"
[ ! -e "$WORK/last-run-note" ] || fail "note: a clean run kept the note of the run before"

# install_first prints the note and clears it
run "$STUB; echo '10:00:00 == the VM ran an older thing' > \$WORK/last-run-note; install_first"
grep -q '== the VM ran an older thing' "$T/out.txt" || fail "install_first: does not print the note"
[ ! -e "$WORK/last-run-note" ] || fail "install_first: the note stays"

# rdna3vm result prints the note and puts it into 00-host.txt (not clearing it)
RDNA3VM_RESULT=$T/res.tar.gz run "echo '10:00:00 == the VM ran an older thing' > \$WORK/last-run-note
    touch \$WORK/run-start; sleep() { :; }; echo hi > \$WORK/reports/serial-1.log; cmd_result"
grep -q '== the VM ran an older thing' "$T/out.txt" || fail "result: does not print the note"
tar -xzOf "$T/res.tar.gz" rdna3-result/00-host.txt | grep -q '^run-note: 10:00:00 == the VM ran an older thing' || fail "result: 00-host.txt lacks the note"
[ -s "$WORK/last-run-note" ] || fail "result: cleared the note (only install_first does)"

# ---- L1: the queue sets the marker aside and says the step ran on the old driver
run "printf 'play\n5\n' > \$QUEUE; step_args() { echo a; }; echo approve > \$WORK/pass-1; cmd__queue"
ls "$WORK"/reports/approve-seen-*.txt >/dev/null 2>&1 || fail "queue: no approve-seen-*.txt"
[ ! -e "$WORK/reports/approve-x.txt" ] || fail "queue: the marker is still in place"
grep -qF "$MARK" "$WORK"/reports/approve-seen-*.txt || fail "queue: approve-seen lacks the marker text"
grep -q 'stopped after step play: it ran on the OLD driver' "$T/out.txt" || fail "queue: message: $(cat "$T/out.txt")"
[ "$(tr '\n' ' ' < "$WORK/queue")" = "play 5 " ] || fail "queue: step not kept: $(cat "$WORK/queue")"
# a set-aside marker is not a marker of the next boot (install_first would ask again)
run "$STUB; cp /dev/null \$WORK/reports/approve-seen-1.txt; echo '$MARK' > \$WORK/reports/approve-seen-1.txt; install_first; ! approve_marker_any"
[ "$(calls)" = "install " ] || fail "approve-seen counted as a marker: calls [$(calls)]"

# ---- ECC review of 501eae9: a step that asked for a Mac model is approved under it ('--step N')
RDNA3VM_STEP=play-desktop RDNA3VM_BOOTARGS="-v rdna3play=1 @smbios=MacPro7,1 rdna3vmauto=1" run "echo approve > \$WORK/pass-1; cmd__once"
grep -q "run 'rdna3vm approve --step play-desktop', then the step again" "$WORK/reports/host-run.log" || fail "note with @smbios=: $(cat "$WORK/reports/host-run.log")"
RDNA3VM_BOOTARGS="-v rdna3play=1" run "echo approve > \$WORK/pass-1; cmd__once"
grep -q "run 'rdna3vm approve', then the step again" "$WORK/reports/host-run.log" || fail "note without @smbios= must stay plain"
run "printf 'play-macpro\n5\n' > \$QUEUE; step_args() { echo 'rdna3play=1 @smbios=MacPro7,1'; }; echo approve > \$WORK/pass-1; cmd__queue"
grep -q "run 'rdna3vm approve --step play-macpro', then 'rdna3vm queue' runs step play-macpro again" "$T/out.txt" || fail "queue stop with @smbios=: $(cat "$T/out.txt")"
run "printf 'play\n5\n' > \$QUEUE; step_args() { echo 'rdna3play=1'; }; echo approve > \$WORK/pass-1; cmd__queue"
grep -q "run 'rdna3vm approve', then 'rdna3vm queue' runs step play again" "$T/out.txt" || fail "queue stop without @smbios=: $(cat "$T/out.txt")"
run "printf 'play-macpro\n' > \$QUEUE; step_args() { echo 'rdna3play=1 @smbios=MacPro7,1'; }; echo x > \$WORK/needs-allow; cmd__queue"
grep -q "run 'rdna3vm approve --step play-macpro', then 'rdna3vm queue' goes on with" "$T/out.txt" || fail "queue needs-allow stop with @smbios=: $(cat "$T/out.txt")"

echo "test_approve_gate: ok"
