#!/bin/bash
# DRIVER-PLAN N44: autoreport.sh's coredisplay_iop_prefs (CoreDisplay's useIOP / useIOP_onAMD in
# /Library/Preferences/com.apple.CoreDisplay, written on every boot of the VM, taken out with the
# boot-arg rdna3iopprefs=0) run on the host with a stub `defaults` and `sysctl`, and what it prints read
# by tools/triage_reports.py's cd_prefs_rows:
#  - the first boot: the domain did not exist (WindowServer started without them), both keys written,
#    "the next boot" said; a second call in the same boot keeps the state of the boot's start
#  - the next boot: WindowServer started with both = 1 (triage INFO with useIOP_onAMD 1)
#  - rdna3iopprefs=0 takes both out; a write that does not take is "could NOT" (triage WARN)
#  - kern.boottime with microseconds (the second counts), an unparseable one ("could NOT tell": the record is
#    rewritten every call, triage WARN, useIOP_onAMD unknown), an unwritable boot record ("could NOT keep", the
#    start state is "(unknown)"), a delete that does not take, a write that takes for one key only, the boot-arg
#    as a whole token (=01, =1, a prefix do not count), rdna3vm's pass-through of the token
#  - rdna3iopprefs=0 while WindowServer started with the keys of an earlier boot: triage WARN, not the A/B
#  - a standalone run changes nothing; collect.sh packs 19-coredisplay-prefs.txt; rdna3vm passes
#    the off switch to the install boot
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf -- "$T"' EXIT
fail() { echo "test_coredisplay_prefs: FAIL $*"; [ -f "$T/out" ] && sed 's/^/  | /' "$T/out"; exit 1; }
AR="$ROOT/tools/vm/macos/autoreport.sh"

{ sed -n '/^CD_PREFS=/p; /^CD_PREFS_BOOT=/p' "$AR"; sed -n '/^cd_prefs_read() {/,/^}/p' "$AR"
  sed -n '/^coredisplay_iop_prefs() {/,/^}/p' "$AR"; } > "$T/fn.sh"
[ "$(wc -l < "$T/fn.sh")" -gt 25 ] || fail "coredisplay_iop_prefs not found in autoreport.sh"
grep -q '^\[ -z "\$STANDALONE" \] && coredisplay_iop_prefs >/dev/null 2>&1$' "$AR" || fail "not called on every boot"
grep -q '^    coredisplay_iop_prefs$' "$AR" || fail "not printed into the tests file"
grep -q "run 19-coredisplay-prefs.txt sh -c 'defaults read /Library/Preferences/com.apple.CoreDisplay;" \
    "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not pack 19-coredisplay-prefs.txt"
grep -q 'case " ${RDNA3VM_BOOTARGS:-} " in \*" rdna3iopprefs=0 "\*) iop=" rdna3iopprefs=0"' "$ROOT/tools/vm/rdna3vm" ||
    fail "rdna3vm does not pass rdna3iopprefs=0 to the install boot"

mkdir -p "$T/bin" "$T/store"
cat > "$T/bin/defaults" <<'STUB'
#!/bin/sh
# a domain = a directory of key files; the output looks like macOS's defaults
s=$STORE
case $1 in
read)
    if [ -n "${3:-}" ]; then [ -f "$s/$3" ] && cat "$s/$3" && exit 0; echo "does not exist" >&2; exit 1; fi
    [ -n "$(ls "$s")" ] || { echo "Domain $2 does not exist" >&2; exit 1; }
    echo "{"; for k in $(ls "$s"); do case $k in *_*) q="\"$k\"" ;; *) q=$k ;; esac; echo "    $q = $(cat "$s/$k");"; done; echo "}" ;;
write) [ -n "${DEFAULTS_FAIL:-}" ] && exit 1; [ -n "${PARTIAL_FAIL:-}" ] && [ "$3" = useIOP_onAMD ] && exit 1
       [ "$4" = -bool ] && [ "$5" = true ] && echo 1 > "$s/$3" ;;
delete) [ -n "${DELETE_FAIL:-}" ] && exit 1; rm -f "$s/$3" ;;
esac
STUB
cat > "$T/bin/sysctl" <<'STUB'
#!/bin/sh
case $2 in
kern.boottime) if [ -n "${BTRAW+x}" ]; then echo "$BTRAW"; else echo "{ sec = $BOOTSEC, usec = ${USEC:-123456} } Thu Oct 24 10:00:00 2026"; fi ;;
kern.bootargs) echo "$BOOTARGS" ;;
esac
STUB
chmod +x "$T/bin/defaults" "$T/bin/sysctl"

call() {   # call <out> <env...>
    ( export PATH="$T/bin:$PATH" STORE="$T/store" RDNA3_CD_PREFS=/Library/Preferences/com.apple.CoreDisplay \
             RDNA3_CD_PREFS_BOOT="$T/boot.txt" STANDALONE= BOOTARGS="rdna3fb=1" BOOTSEC=100 DEFAULTS_FAIL= PARTIAL_FAIL= DELETE_FAIL=
      unset BTRAW
      export "${@:2}"
      set +e +o pipefail   # autoreport.sh is a plain sh script: no pipefail
      . "$T/fn.sh"; coredisplay_iop_prefs; echo "[exit $?]" ) > "$T/$1" 2>&1
    cp "$T/$1" "$T/out"
}
rows() {   # rows <file> <args>: cd_prefs_rows of that output as "LEVEL text" lines
    python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1] + "/tools"); import triage_reports as tr
r, amd = tr.cd_prefs_rows(open(sys.argv[2]).read(), sys.argv[3])
print("\n".join("%s %s" % (x[0], x[2]) for x in r)); print("amd=%s" % amd)' "$ROOT" "$T/$1" "$2"
}

# the first boot: nothing in the domain
call b1 BOOTSEC=100
grep -qF "this boot's WindowServer started with: (none)" "$T/b1" || fail "b1: started with (none)"
! grep -qF "could NOT" "$T/b1" || fail "b1: a healthy first boot says could NOT"
grep -qF "== CoreDisplay prefs: useIOP / useIOP_onAMD true (rdna3iopprefs=0 takes them out); now: { useIOP = 1; \"useIOP_onAMD\" = 1; }; WindowServer takes the change at its next start" "$T/b1" || fail "b1: written"
rows b1 "rdna3fb=1" > "$T/r1"; cp "$T/r1" "$T/out"
grep -qx "INFO this boot's WindowServer started with useIOP_onAMD unset (CoreDisplay's static 0), useIOP unset (static 1) ((none)); the switch counts from the next boot (WindowServer started before autoreport wrote it)" "$T/r1" || fail "b1: triage row"
grep -qx "amd=None" "$T/r1" || fail "b1: amd"
# the same boot again (the tests file's call after the early one): the boot's start is kept
call b1b BOOTSEC=100
grep -qF "started with: (none)" "$T/b1b" || fail "b1b: the boot's start state was overwritten"
grep -qF "next start" "$T/b1b" || fail "b1b: WindowServer still runs without them: the next-boot note"

# the next boot: WindowServer started with them
call b2 BOOTSEC=200
grep -qF "this boot's WindowServer started with: { useIOP = 1; \"useIOP_onAMD\" = 1; }" "$T/b2" || fail "b2: started with both"
! grep -qF "next start" "$T/b2" || fail "b2: nothing changed, no next-boot note"
rows b2 "rdna3fb=1" > "$T/r2"; cp "$T/r2" "$T/out"
grep -q "^INFO this boot's WindowServer started with useIOP_onAMD 1, useIOP 1 " "$T/r2" || fail "b2: triage row"
grep -qx "amd=1" "$T/r2" || fail "b2: amd"

# the A/B: rdna3iopprefs=0 takes them out
call b3 BOOTSEC=300 BOOTARGS="rdna3fb=1 rdna3iopprefs=0"
grep -qF "== CoreDisplay prefs: rdna3iopprefs=0: useIOP / useIOP_onAMD taken out; now: (none)" "$T/b3" || fail "b3: taken out"
[ -z "$(ls "$T/store")" ] || fail "b3: keys left"
call b4 BOOTSEC=400 BOOTARGS="rdna3fb=1 rdna3iopprefs=0"
rows b4 "rdna3fb=1 rdna3iopprefs=0" > "$T/r4"; cp "$T/r4" "$T/out"
grep -q "^INFO .*useIOP_onAMD unset .*; rdna3iopprefs=0: the A/B without them$" "$T/r4" || fail "b4: A/B row"

# a write that does not take
call b5 BOOTSEC=500 DEFAULTS_FAIL=1
grep -qF "useIOP / useIOP_onAMD could NOT be written" "$T/b5" || fail "b5: could NOT"
rows b5 "rdna3fb=1" > "$T/r5"; cp "$T/r5" "$T/out"
grep -q "^WARN autoreport.sh: useIOP / useIOP_onAMD could NOT be written" "$T/r5" || fail "b5: triage WARN"

# standalone: reports, changes nothing
call b6 BOOTSEC=600 STANDALONE=1
grep -qF "left as they are (standalone run); now: (none)" "$T/b6" || fail "b6: standalone"
[ -z "$(ls "$T/store")" ] || fail "b6: standalone wrote keys"

# 19-coredisplay-prefs.txt as the fallback (a tests file without the lines)
printf '$ sh -c defaults read ...\n{\n    useIOP = 1;\n}\n== at the start of the boot (autoreport.sh):\nboot 100\n{ useIOP = 1; "useIOP_onAMD" = 1; }\n[exit 0]\n' > "$T/m19"
python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1] + "/tools"); import triage_reports as tr
r, amd = tr.cd_prefs_rows("", "", open(sys.argv[2]).read()); assert amd == "1" and r[0][0] == "INFO", (r, amd)
r, amd = tr.cd_prefs_rows("", "", ""); assert amd is False and r[0][0] == "WARN", (r, amd)' "$ROOT" "$T/m19" ||
    fail "19-coredisplay-prefs.txt fallback / missing data"

# the boot time as macOS prints it has microseconds (usec != 0): the second is what counts, not the usec
call u1 BOOTSEC=700 USEC=999999
head -n 1 "$T/boot.txt" | grep -qx "boot 700" || fail "u1: the boot record is not 'boot <sec>': $(head -n 1 "$T/boot.txt")"
call u2 BOOTSEC=700 USEC=1
head -n 1 "$T/boot.txt" | grep -qx "boot 700" || fail "u2: another usec of the same boot changed the record"
grep -qF "started with: (none)" "$T/u2" || fail "u2: the same boot (another usec) overwrote the boot's start state"
rm -f "$T"/store/*

# the boot time cannot be parsed: the record is rewritten on every call and the line says it could NOT tell;
# triage WARNs and does not trust the state
call t1 BTRAW=garbage
grep -qF "== CoreDisplay prefs: boot time unknown: could NOT tell this boot's start state, it may be stale" "$T/t1" || fail "t1: unknown boot time not said"
grep -qF "started with: (none)" "$T/t1" || fail "t1: first call: the state before the write"
call t2 BTRAW=garbage
grep -qF "could NOT tell this boot's start state" "$T/t2" || fail "t2: not said again"
grep -qF 'started with: { useIOP = 1; "useIOP_onAMD" = 1; }' "$T/t2" || fail "t2: the record must be rewritten on every call when the boot is unknown"
rows t2 "rdna3fb=1" > "$T/rt2"; cp "$T/rt2" "$T/out"
grep -q "^WARN .*NOT known" "$T/rt2" && grep -q "^WARN autoreport.sh: boot time unknown: could NOT tell" "$T/rt2" || fail "t2: triage WARN"
grep -qx "amd=False" "$T/rt2" || fail "t2: useIOP_onAMD must be unknown"
call t3 BTRAW=""
grep -qF "could NOT tell this boot's start state" "$T/t3" || fail "t3: an empty kern.boottime"
call t4 BTRAW="{ usec = 5 }"
grep -qF "could NOT tell this boot's start state" "$T/t4" || fail "t4: no sec = in kern.boottime"
rm -f "$T"/store/*

# the boot's record cannot be written: "could NOT keep", the old file's content is not this boot's state
mkdir -p "$T/boots"
call k1 RDNA3_CD_PREFS_BOOT="$T/nodir/boot.txt"
grep -qF "== CoreDisplay prefs: could NOT keep this boot's state in $T/nodir/boot.txt" "$T/k1" || fail "k1: could NOT keep"
grep -qF "started with: (unknown)" "$T/k1" || fail "k1: the state of a lost record must be (unknown), not (none)"
rows k1 "rdna3fb=1" > "$T/rk1"; cp "$T/rk1" "$T/out"
grep -q "^WARN autoreport.sh: could NOT keep this boot's state" "$T/rk1" || fail "k1: triage WARN"
grep -qx "amd=False" "$T/rk1" || fail "k1: useIOP_onAMD must be unknown"
rm -f "$T"/store/*
call k2 RDNA3_CD_PREFS_BOOT="$T/boots"     # a directory at the record's name
grep -qF "could NOT keep this boot's state" "$T/k2" && grep -qF "started with: (unknown)" "$T/k2" || fail "k2: a directory as the record"
rm -f "$T"/store/*

# rdna3iopprefs=0 but the keys do not go away
call d1 BOOTSEC=800
call d2 BOOTSEC=801 BOOTARGS="rdna3fb=1 rdna3iopprefs=0" DELETE_FAIL=1
grep -qF "rdna3iopprefs=0: useIOP / useIOP_onAMD could NOT be taken out" "$T/d2" || fail "d2: delete failure not said"
grep -qF 'started with: { useIOP = 1; "useIOP_onAMD" = 1; }' "$T/d2" || fail "d2: this boot's start state (the keys of d1)"
rows d2 "rdna3fb=1 rdna3iopprefs=0" > "$T/rd2"; cp "$T/rd2" "$T/out"
grep -q "^WARN autoreport.sh: rdna3iopprefs=0: useIOP / useIOP_onAMD could NOT be taken out" "$T/rd2" || fail "d2: triage WARN"
# ... and WindowServer had started with the keys although rdna3iopprefs=0: not the A/B without them (triage WARN)
grep -q "^WARN rdna3iopprefs=0 but WindowServer started with useIOP_onAMD 1" "$T/rd2" || fail "d2: triage WARN (=0 with the keys at the start)"
! grep -q "A/B without them$" "$T/rd2" || fail "d2: the row calls the run the A/B"
rm -f "$T"/store/*

# a write that takes for one key only
call p1 BOOTSEC=810 PARTIAL_FAIL=1
grep -qF "useIOP / useIOP_onAMD could NOT be written" "$T/p1" || fail "p1: partial write not said"
rows p1 "rdna3fb=1" > "$T/rp1"; cp "$T/rp1" "$T/out"
grep -q "^WARN autoreport.sh: useIOP / useIOP_onAMD could NOT be written" "$T/rp1" || fail "p1: triage WARN"
rm -f "$T"/store/*

# the boot-arg is a whole token: =0 alone or last or first takes the keys out; =01, =1, a prefix or suffix do not
n=820
for ba in "rdna3iopprefs=0" "rdna3fb=1 rdna3iopprefs=0" "rdna3iopprefs=0 rdna3fb=1" "-v rdna3iopprefs=0 rdna3fb=1"; do
    n=$((n + 1)); call w$n BOOTSEC=$n; call w${n}o BOOTSEC=$((n + 100)) BOOTARGS="$ba"
    [ -z "$(ls "$T/store")" ] || fail "'$ba' did not take the keys out"
    grep -qF "rdna3iopprefs=0: useIOP / useIOP_onAMD taken out" "$T/w${n}o" || fail "'$ba': taken out not said"
done
for ba in "rdna3iopprefs=01" "rdna3iopprefs=1" "xrdna3iopprefs=0" "rdna3iopprefs=0x" "rdna3iopprefs=" "rdna3iopprefs"; do
    n=$((n + 1)); rm -f "$T"/store/*; call w$n BOOTSEC=$n BOOTARGS="$ba"
    [ -f "$T/store/useIOP_onAMD" ] && [ -f "$T/store/useIOP" ] || fail "'$ba' must write the keys"
    grep -qF "useIOP / useIOP_onAMD true" "$T/w$n" || fail "'$ba': written not said"
done
rm -f "$T"/store/*

# rdna3vm passes the off switch to the install boot only as a whole token (the case of cmd__install, run as it is)
sed -n '/^    case " ${RDNA3VM_BOOTARGS:-} " in \*" rdna3iopprefs=0 "\*) iop=/p' "$ROOT/tools/vm/rdna3vm" > "$T/rv.sh"
[ "$(wc -l < "$T/rv.sh")" = 1 ] || fail "rdna3vm: the iop case line not found"
passes() { ( iop=; RDNA3VM_BOOTARGS="$1"; . "$T/rv.sh"; printf '%s' "$iop" ); }
[ "$(passes "rdna3iopprefs=0")" = " rdna3iopprefs=0" ] || fail "rdna3vm: =0 alone is not passed"
[ "$(passes "-v rdna3iopprefs=0 rdna3fb=1")" = " rdna3iopprefs=0" ] || fail "rdna3vm: =0 in the middle is not passed"
[ "$(passes "rdna3fb=1 rdna3iopprefs=0")" = " rdna3iopprefs=0" ] || fail "rdna3vm: =0 last is not passed"
for ba in "" "rdna3iopprefs=01" "rdna3iopprefs=1" "xrdna3iopprefs=0" "rdna3iopprefs=0x"; do
    [ -z "$(passes "$ba")" ] || fail "rdna3vm: '$ba' must not be passed as the off switch"
done

for f in b1 b1b b2 b3 b4 b5 b6 u1 u2 t1 t2 k1 d2 p1; do grep -qx "\[exit 0\]" "$T/$f" || fail "$f: the function did not return 0"; done
echo "test_coredisplay_prefs: ok"
