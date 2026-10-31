#!/bin/sh
# tests/test_summary.sh LOGDIR NAME... - the verdict of `make test`: reads LOGDIR/NAME.rc of every test written by
# tests/run_one.sh, prints the log of each failed test, then one summary line. Exit status 1 if any test failed
# or left no result (killed / never started). A test that passed but said "skip" in its log counts as skipped.
# POSIX sh (macOS has bash 3.2 and BSD tools).
d=$1
shift
pass=0
fail=0
skip=0
failed=
for n in "$@"; do
    if [ ! -f "$d/$n.rc" ]; then
        echo "---- $n: no result (the test did not run to its end)"
        fail=$((fail + 1))
        failed="$failed $n"
        continue
    fi
    read -r rc secs < "$d/$n.rc"
    if [ "$rc" -ne 0 ]; then
        echo "---- $n: exit status $rc after $secs s, $d/$n.log"
        cat "$d/$n.log"
        fail=$((fail + 1))
        failed="$failed $n"
    elif grep -qi 'skip' "$d/$n.log"; then
        skip=$((skip + 1))
    else
        pass=$((pass + 1))
    fi
done
# the slowest ones, to keep the schedule in the Makefile sorted (longest first)
for n in "$@"; do
    [ -f "$d/$n.rc" ] && { read -r rc secs < "$d/$n.rc"; echo "$secs $n"; }
done | sort -rn | head -5 | tr '\n' ',' | sed 's/,$//; s/,/, /g; s/^/slowest (s): /'
echo
echo "make test: $pass passed, $fail failed, $skip skipped (with a skip note in the log), $# tests; logs in $d/"
[ -z "$failed" ] || echo "failed:$failed"
[ "$fail" -eq 0 ]
