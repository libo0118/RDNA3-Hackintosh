#!/bin/sh
# tools/ci/fetch_fw.sh without network: a fake curl that answers 429 / 200 with a wrong file. The step must end fast,
# exit 0, leave no .complete and no half set in DEST, drop a corrupt cached file, keep nothing wrong in the cache, and
# reach the second source (git.kernel.org) when the first one is rate limited. POSIX sh.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_fetch_fw: FAIL $*"; [ -f "$T/out.txt" ] && sed 's/^/  | /' "$T/out.txt"; exit 1; }
mkdir -p "$T/bin"
cat > "$T/bin/curl" <<'EOF2'
#!/bin/sh
# fake curl: records the URL; FAKE_CODE 429 -> prints 429; FAKE_CODE 200 -> writes junk and prints 200
o=; url=
while [ $# -gt 0 ]; do case "$1" in -o) o=$2; shift ;; http*) url=$1 ;; esac; shift; done
echo "$url" >> "$FAKE_LOG"
if [ "$FAKE_CODE" = 200 ]; then echo "not a firmware" > "$o"; fi
printf '%s' "$FAKE_CODE"
EOF2
chmod +x "$T/bin/curl"
export FAKE_LOG=$T/calls.txt
: > "$FAKE_LOG"

run() {   # run <fake code> [budget]: fetch_fw.sh with the fake curl, a fresh DEST, the shared cache
    rm -rf "$T/dest"
    FAKE_CODE=$1 PATH="$T/bin:$PATH" RDNA3_FW_CACHE=$T/cache RDNA3_FW_BUDGET=${2:-20} sh "$ROOT/tools/ci/fetch_fw.sh" "$T/dest" "${3:-navi32}" > "$T/out.txt" 2>&1
}

# a corrupt file in the cache is dropped, not trusted
mkdir -p "$T/cache"
echo junk > "$T/cache/vcn_4_0_0.bin"

t0=$(date +%s)
run 429 || fail "exit status not 0 on a rate limit"
[ $(( $(date +%s) - t0 )) -lt 15 ] || fail "a 429 held the step for $(( $(date +%s) - t0 )) s"
grep -q 'NOT COMPLETE' "$T/out.txt" || fail "no NOT COMPLETE line"
grep -q 'cached vcn_4_0_0.bin fails its SHA-256' "$T/out.txt" || fail "a corrupt cached file was trusted"
[ ! -e "$T/dest/.complete" ] || fail ".complete without the files"
[ -z "$(ls "$T"/dest/*.bin 2>/dev/null)" ] || fail "a half set left in DEST"
[ -z "$(ls -A "$T/cache")" ] || fail "the cache holds something after only failures: $(ls -A "$T/cache")"
grep -q 'gitlab.com' "$FAKE_LOG" && grep -q 'git.kernel.org' "$FAKE_LOG" || fail "the second source was not tried after a 429"
[ "$(grep -c 'vcn_4_0_0.bin' "$FAKE_LOG")" = 2 ] || fail "a 429 was retried instead of moving on: $(grep -c vcn_4_0_0 "$FAKE_LOG") calls"

# 200 with a file whose SHA-256 is wrong: refused, never cached
: > "$FAKE_LOG"
run 200 4 || fail "exit status not 0 with wrong files"
grep -q 'SHA-256 differs' "$T/out.txt" || fail "a wrong file was not reported"
[ -z "$(ls -A "$T/cache")" ] || fail "a file with a wrong SHA-256 reached the cache: $(ls -A "$T/cache")"
[ ! -e "$T/dest/.complete" ] || fail ".complete with wrong files"

# The explicit Navi 33 set uses its own names and retains the same failure contract.
: > "$FAKE_LOG"
run 429 20 navi33 || fail "Navi 33 rate limit exit status"
grep -q 'psp_13_0_7_sos.bin' "$FAKE_LOG" || fail "Navi 33 PSP file not requested"
grep -q 'gc_11_0_2_mes_2.bin' "$FAKE_LOG" || fail "Navi 33 MES file not requested"
grep -q 'psp_13_0_10_sos.bin' "$FAKE_LOG" && fail "Navi 32 PSP requested for Navi 33"
[ ! -e "$T/dest/.complete" ] || fail ".complete after Navi 33 download failures"
sh "$ROOT/tools/ci/fetch_fw.sh" "$T/dest" unknown > "$T/out.txt" 2>&1 && fail "unknown chip accepted"
echo "test_fetch_fw: ok"
