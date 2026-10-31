#!/bin/sh
# tests/run_one.sh LOGDIR NAME COMMAND [ARG...] - one test of `make test`. The output goes to LOGDIR/NAME.log and
# "<exit status> <seconds>" to LOGDIR/NAME.rc; one line says how it went. It always exits 0: a failing test must not
# stop the tests running beside it, tests/test_summary.sh turns the .rc files into the verdict and prints the logs.
# POSIX sh (macOS has bash 3.2 and BSD tools).
d=$1
n=$2
shift 2
mkdir -p "$d"
rm -f "$d/$n.rc" "$d/$n.log"
t0=$(date +%s)
"$@" > "$d/$n.log" 2>&1
rc=$?
t1=$(date +%s)
echo "$rc $((t1 - t0))" > "$d/$n.rc"
if [ "$rc" -eq 0 ]; then
    echo "ok   $n ($((t1 - t0)) s)"
else
    echo "FAIL $n (exit $rc, $((t1 - t0)) s), log $d/$n.log"
fi
exit 0
