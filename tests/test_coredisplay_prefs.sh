#!/bin/bash
# DRIVER-PLAN N44: autoreport.sh's coredisplay_iop_prefs (CoreDisplay's useIOP / useIOP_onAMD in
# /Library/Preferences/com.apple.CoreDisplay (AnyHost) and in /Library/Preferences/ByHost/
# com.apple.CoreDisplay.<IOPlatformUUID> (ByHost; Navi48-MacOS, Almosst-DEV, MIT, found CoreDisplay reading it),
# written on every boot of the VM, taken out with the boot-arg rdna3iopprefs=0) run on the host with a stub
# `defaults`, `sysctl` and `ioreg`, and what it prints read by tools/triage_reports.py's cd_prefs_rows:
#  - both stores written / taken out, each reported on its own; one store failing is "could NOT" for that store
#    only (the other is still written); the host UUID unknown ("could NOT", AnyHost still written)
#  - only AnyHost holding the keys at WindowServer's start: triage WARN "may read the ByHost store"
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
  sed -n '/^CD_BYHOST_DIR=/p' "$AR"; sed -n '/^cd_host_uuid() {/,/^}/p; /^cd_prefs_state() {/,/^}/p; /^cd_prefs_on() {/,/^}/p; /^cd_prefs_off() {/,/^}/p' "$AR"
  sed -n '/^coredisplay_iop_prefs() {/,/^}/p' "$AR"; } > "$T/fn.sh"
[ "$(wc -l < "$T/fn.sh" | tr -d " ")" -gt 40 ] || fail "coredisplay_iop_prefs not found in autoreport.sh"
grep -q '^\[ -z "\$STANDALONE" \] && coredisplay_iop_prefs >/dev/null 2>&1$' "$AR" || fail "not called on every boot"
grep -q '^    coredisplay_iop_prefs$' "$AR" || fail "not printed into the tests file"
grep -q "run 19-coredisplay-prefs.txt sh -c 'defaults read /Library/Preferences/com.apple.CoreDisplay;" \
    "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not pack 19-coredisplay-prefs.txt"
grep -q "19-coredisplay-prefs.txt .*defaults read /Library/Preferences/ByHost/com.apple.CoreDisplay.\$u" "$ROOT/tools/macos/collect.sh" ||
    fail "collect.sh does not read the ByHost store into 19-coredisplay-prefs.txt"
grep -q 'ls -l /Library/Preferences/ByHost/ | grep CoreDisplay' "$ROOT/tools/macos/collect.sh" || fail "collect.sh does not list the ByHost files"
grep -q 'case " ${RDNA3VM_BOOTARGS:-} " in \*" rdna3iopprefs=0 "\*) iop=" rdna3iopprefs=0"' "$ROOT/tools/vm/rdna3vm" ||
    fail "rdna3vm does not pass rdna3iopprefs=0 to the install boot"

mkdir -p "$T/bin" "$T/store" "$T/store_bh" "$T/ByHost"
cat > "$T/bin/defaults" <<'STUB'
#!/bin/sh
# a domain = a directory of key files; the output looks like macOS's defaults
# the ByHost domain (a path under /ByHost/) is a second directory; a stub failure can hit one store only
[ -n "${DLOG:-}" ] && echo "$*" >> "$DLOG"
case $2 in */ByHost/*) s=$STORE_BH; bh=1 ;; *) s=$STORE; bh= ;; esac
case $1 in
read)
    if [ -n "${3:-}" ]; then [ -f "$s/$3" ] && cat "$s/$3" && exit 0; echo "does not exist" >&2; exit 1; fi
    [ -n "$(ls "$s")" ] || { echo "Domain $2 does not exist" >&2; exit 1; }
    echo "{"; for k in $(ls "$s"); do case $k in *_*) q="\"$k\"" ;; *) q=$k ;; esac; echo "    $q = $(cat "$s/$k");"; done; echo "}" ;;
write) [ -n "${DEFAULTS_FAIL:-}" ] && exit 1; [ -n "${PARTIAL_FAIL:-}" ] && [ "$3" = useIOP_onAMD ] && exit 1
       [ -n "$bh" ] && [ -n "${FAIL_BH:-}" ] && exit 1; [ -z "$bh" ] && [ -n "${FAIL_ANY:-}" ] && exit 1
       [ "$4" = -bool ] && [ "$5" = true ] && echo 1 > "$s/$3" ;;
delete) [ -n "${DELETE_FAIL:-}" ] && exit 1; [ -n "$bh" ] && [ -n "${DELETE_FAIL_BH:-}" ] && exit 1; rm -f "$s/$3" ;;
esac
STUB
cat > "$T/bin/sysctl" <<'STUB'
#!/bin/sh
case $2 in
kern.boottime) if [ -n "${BTRAW+x}" ]; then echo "$BTRAW"; else echo "{ sec = $BOOTSEC, usec = ${USEC:-123456} } Thu Oct 24 10:00:00 2026"; fi ;;
kern.bootargs) echo "$BOOTARGS" ;;
esac
STUB
cat > "$T/bin/ioreg" <<'STUB'
#!/bin/sh
# ioreg -rd1 -c IOPlatformExpertDevice: the UUID line only when UUID is set (an empty one = ioreg without it)
echo '+-o Root  <class IORegistryEntry, id 0x100000100, retain 20>'
echo '  | "IOPlatformSerialNumber" = "SERIAL123"'
[ -n "${UUID:-}" ] && echo "  | \"IOPlatformUUID\" = \"$UUID\""
exit 0
STUB
chmod +x "$T/bin/defaults" "$T/bin/sysctl" "$T/bin/ioreg"
UUID1=11111111-2222-3333-4444-555555555555
ANY0='AnyHost: (none)'
BH0="ByHost($UUID1): (none)"
BOTH="{ useIOP = 1; \"useIOP_onAMD\" = 1; }"       # one store's domain as the stub prints it
BOTH_ST="AnyHost: $BOTH; ByHost($UUID1): $BOTH"

call() {   # call <out> <env...>
    ( export PATH="$T/bin:$PATH" STORE="$T/store" STORE_BH="$T/store_bh" RDNA3_CD_PREFS=/Library/Preferences/com.apple.CoreDisplay \
             RDNA3_CD_BYHOST_DIR="$T/ByHost" DLOG="$T/dlog" UUID=$UUID1 FAIL_BH= FAIL_ANY= DELETE_FAIL_BH= \
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
grep -qF "this boot's WindowServer started with: $ANY0; $BH0" "$T/b1" || fail "b1: started with (none) in both stores"
! grep -qF "could NOT" "$T/b1" || fail "b1: a healthy first boot says could NOT"
grep -qF "== CoreDisplay prefs: useIOP / useIOP_onAMD true: AnyHost written; ByHost($UUID1) written (rdna3iopprefs=0 takes them out); now: $BOTH_ST; WindowServer takes the change at its next start" "$T/b1" || fail "b1: written"
[ -f "$T/store/useIOP" ] && [ -f "$T/store/useIOP_onAMD" ] || fail "b1: AnyHost keys not written"
[ -f "$T/store_bh/useIOP" ] && [ -f "$T/store_bh/useIOP_onAMD" ] || fail "b1: ByHost keys not written"
grep -qxF "write $T/ByHost/com.apple.CoreDisplay.$UUID1 useIOP_onAMD -bool true" "$T/dlog" || fail "b1: the ByHost file is not <dir>/com.apple.CoreDisplay.<IOPlatformUUID>"
rows b1 "rdna3fb=1" > "$T/r1"; cp "$T/r1" "$T/out"
grep -qx "INFO this boot's WindowServer started with AnyHost: useIOP_onAMD unset (CoreDisplay's static 0), useIOP unset (static 1); ByHost($UUID1): useIOP_onAMD unset (static 0), useIOP unset (static 1) ($ANY0; $BH0); the switch counts from the next boot (WindowServer started before autoreport wrote it)" "$T/r1" || fail "b1: triage row"
! grep -q "^WARN" "$T/r1" || fail "b1: a healthy first boot has a triage WARN"
grep -qx "amd=None" "$T/r1" || fail "b1: amd"
# the same boot again (the tests file's call after the early one): the boot's start is kept
call b1b BOOTSEC=100
grep -qF "started with: $ANY0; $BH0" "$T/b1b" || fail "b1b: the boot's start state was overwritten"
grep -qF "next start" "$T/b1b" || fail "b1b: WindowServer still runs without them: the next-boot note"

# the next boot: WindowServer started with them
call b2 BOOTSEC=200
grep -qF "this boot's WindowServer started with: $BOTH_ST" "$T/b2" || fail "b2: started with both, in both stores"
! grep -qF "next start" "$T/b2" || fail "b2: nothing changed, no next-boot note"
rows b2 "rdna3fb=1" > "$T/r2"; cp "$T/r2" "$T/out"
grep -q "^INFO this boot's WindowServer started with AnyHost: useIOP_onAMD 1, useIOP 1; ByHost($UUID1): useIOP_onAMD 1, useIOP 1 " "$T/r2" || fail "b2: triage row"
! grep -q "^WARN" "$T/r2" || fail "b2: both stores hold the keys: no WARN"
grep -qx "amd=1" "$T/r2" || fail "b2: amd"

# the A/B: rdna3iopprefs=0 takes them out
call b3 BOOTSEC=300 BOOTARGS="rdna3fb=1 rdna3iopprefs=0"
grep -qF "== CoreDisplay prefs: rdna3iopprefs=0: useIOP / useIOP_onAMD: AnyHost taken out; ByHost($UUID1) taken out; now: $ANY0; $BH0" "$T/b3" || fail "b3: taken out of both"
[ -z "$(ls "$T/store")" ] || fail "b3: AnyHost keys left"
[ -z "$(ls "$T/store_bh")" ] || fail "b3: ByHost keys left"
call b4 BOOTSEC=400 BOOTARGS="rdna3fb=1 rdna3iopprefs=0"
rows b4 "rdna3fb=1 rdna3iopprefs=0" > "$T/r4"; cp "$T/r4" "$T/out"
grep -q "^INFO .*AnyHost: useIOP_onAMD unset .*ByHost($UUID1): useIOP_onAMD unset .*; rdna3iopprefs=0: the A/B without them$" "$T/r4" || fail "b4: A/B row"

# a write that does not take
call b5 BOOTSEC=500 DEFAULTS_FAIL=1
grep -qF "AnyHost could NOT be written; ByHost($UUID1) could NOT be written" "$T/b5" || fail "b5: could NOT, each store on its own"
rows b5 "rdna3fb=1" > "$T/r5"; cp "$T/r5" "$T/out"
grep -q "^WARN autoreport.sh: useIOP / useIOP_onAMD true: AnyHost could NOT be written; ByHost" "$T/r5" || fail "b5: triage WARN"

# standalone: reports, changes nothing
call b6 BOOTSEC=600 STANDALONE=1
grep -qF "left as they are (standalone run); now: $ANY0; $BH0" "$T/b6" || fail "b6: standalone"
[ -z "$(ls "$T/store")" ] && [ -z "$(ls "$T/store_bh")" ] || fail "b6: standalone wrote keys"

# 19-coredisplay-prefs.txt as the fallback (a tests file without the lines)
printf '$ sh -c defaults read ...\n{\n    useIOP = 1;\n}\n== at the start of the boot (autoreport.sh):\nboot 100\n{ useIOP = 1; "useIOP_onAMD" = 1; }\n[exit 0]\n' > "$T/m19"
python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1] + "/tools"); import triage_reports as tr
r, amd = tr.cd_prefs_rows("", "", open(sys.argv[2]).read()); assert amd == "1" and r[0][0] == "INFO", (r, amd)
r, amd = tr.cd_prefs_rows("", "", ""); assert amd is False and r[0][0] == "WARN", (r, amd)' "$ROOT" "$T/m19" ||
    fail "19-coredisplay-prefs.txt fallback / missing data"
# ... in the two-store format (autoreport.sh since N45 (1)): both stores read, and a ByHost store that is unknown (ioreg gave
# no IOPlatformUUID) is a WARN here too, as the tests-file path WARNs through autoreport.sh's "could NOT be written" line
m19b() {   # m19b <state line> -> the member text as collect.sh packs it
    printf '$ sh -c defaults read ...\n{\n    useIOP = 1;\n}\n== ByHost store (IOPlatformUUID ):\n== at the start of the boot (autoreport.sh):\nboot 100\n%s\n[exit 0]\n' "$1" > "$T/m19b"
}
m19b "AnyHost: (none); ByHost(unknown): (unknown)"
python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1] + "/tools"); import triage_reports as tr
r, amd = tr.cd_prefs_rows("", "", open(sys.argv[2]).read())
assert [x[0] for x in r] == ["INFO", "WARN"] and "ByHost store is NOT known" in r[1][2] and "not known (ioreg" in r[0][2], r
assert amd is None, amd' "$ROOT" "$T/m19b" || fail "19-coredisplay-prefs.txt fallback: two-store line with an unknown ByHost store must WARN"
m19b "AnyHost: $BOTH; ByHost($UUID1): $BOTH"
python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1] + "/tools"); import triage_reports as tr
r, amd = tr.cd_prefs_rows("", "", open(sys.argv[2]).read())
assert [x[0] for x in r] == ["INFO"] and amd == "1" and "ByHost(" in r[0][2], (r, amd)' "$ROOT" "$T/m19b" ||
    fail "19-coredisplay-prefs.txt fallback: two-store line, both stores holding the keys"
m19b "AnyHost: $BOTH; ByHost($UUID1): (none)"
python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1] + "/tools"); import triage_reports as tr
r, amd = tr.cd_prefs_rows("", "", open(sys.argv[2]).read())
assert [x[0] for x in r] == ["INFO", "WARN"] and "AnyHost store only" in r[1][2], r' "$ROOT" "$T/m19b" ||
    fail "19-coredisplay-prefs.txt fallback: AnyHost-only line must WARN"

# the boot time as macOS prints it has microseconds (usec != 0): the second is what counts, not the usec
call u1 BOOTSEC=700 USEC=999999
head -n 1 "$T/boot.txt" | grep -qx "boot 700" || fail "u1: the boot record is not 'boot <sec>': $(head -n 1 "$T/boot.txt")"
call u2 BOOTSEC=700 USEC=1
head -n 1 "$T/boot.txt" | grep -qx "boot 700" || fail "u2: another usec of the same boot changed the record"
grep -qF "started with: $ANY0; $BH0" "$T/u2" || fail "u2: the same boot (another usec) overwrote the boot's start state"
rm -f "$T"/store/* "$T"/store_bh/*

# the boot time cannot be parsed: the record is rewritten on every call and the line says it could NOT tell;
# triage WARNs and does not trust the state
call t1 BTRAW=garbage
grep -qF "== CoreDisplay prefs: boot time unknown: could NOT tell this boot's start state, it may be stale" "$T/t1" || fail "t1: unknown boot time not said"
grep -qF "started with: $ANY0; $BH0" "$T/t1" || fail "t1: first call: the state before the write"
call t2 BTRAW=garbage
grep -qF "could NOT tell this boot's start state" "$T/t2" || fail "t2: not said again"
grep -qF "started with: $BOTH_ST" "$T/t2" || fail "t2: the record must be rewritten on every call when the boot is unknown"
rows t2 "rdna3fb=1" > "$T/rt2"; cp "$T/rt2" "$T/out"
grep -q "^WARN .*NOT known" "$T/rt2" && grep -q "^WARN autoreport.sh: boot time unknown: could NOT tell" "$T/rt2" || fail "t2: triage WARN"
grep -qx "amd=False" "$T/rt2" || fail "t2: useIOP_onAMD must be unknown"
call t3 BTRAW=""
grep -qF "could NOT tell this boot's start state" "$T/t3" || fail "t3: an empty kern.boottime"
call t4 BTRAW="{ usec = 5 }"
grep -qF "could NOT tell this boot's start state" "$T/t4" || fail "t4: no sec = in kern.boottime"
rm -f "$T"/store/* "$T"/store_bh/*

# the boot's record cannot be written: "could NOT keep", the old file's content is not this boot's state
mkdir -p "$T/boots"
call k1 RDNA3_CD_PREFS_BOOT="$T/nodir/boot.txt"
grep -qF "== CoreDisplay prefs: could NOT keep this boot's state in $T/nodir/boot.txt" "$T/k1" || fail "k1: could NOT keep"
grep -qF "started with: (unknown)" "$T/k1" || fail "k1: the state of a lost record must be (unknown), not (none)"
rows k1 "rdna3fb=1" > "$T/rk1"; cp "$T/rk1" "$T/out"
grep -q "^WARN autoreport.sh: could NOT keep this boot's state" "$T/rk1" || fail "k1: triage WARN"
grep -qx "amd=False" "$T/rk1" || fail "k1: useIOP_onAMD must be unknown"
rm -f "$T"/store/* "$T"/store_bh/*
call k2 RDNA3_CD_PREFS_BOOT="$T/boots"     # a directory at the record's name
grep -qF "could NOT keep this boot's state" "$T/k2" && grep -qF "started with: (unknown)" "$T/k2" || fail "k2: a directory as the record"
rm -f "$T"/store/* "$T"/store_bh/*

# rdna3iopprefs=0 but the keys do not go away
call d1 BOOTSEC=800
call d2 BOOTSEC=801 BOOTARGS="rdna3fb=1 rdna3iopprefs=0" DELETE_FAIL=1
grep -qF "rdna3iopprefs=0: useIOP / useIOP_onAMD: AnyHost could NOT be taken out; ByHost($UUID1) could NOT be taken out" "$T/d2" || fail "d2: delete failure not said for both stores"
grep -qF "started with: $BOTH_ST" "$T/d2" || fail "d2: this boot's start state (the keys of d1)"
rows d2 "rdna3fb=1 rdna3iopprefs=0" > "$T/rd2"; cp "$T/rd2" "$T/out"
grep -q "^WARN autoreport.sh: rdna3iopprefs=0: useIOP / useIOP_onAMD: AnyHost could NOT be taken out" "$T/rd2" || fail "d2: triage WARN"
# ... and WindowServer had started with the keys although rdna3iopprefs=0: not the A/B without them (triage WARN)
grep -q "^WARN rdna3iopprefs=0 but WindowServer started with useIOP_onAMD 1" "$T/rd2" || fail "d2: triage WARN (=0 with the keys at the start)"
! grep -q "A/B without them$" "$T/rd2" || fail "d2: the row calls the run the A/B"
rm -f "$T"/store/* "$T"/store_bh/*

# a write that takes for one key only
call p1 BOOTSEC=810 PARTIAL_FAIL=1
grep -qF "AnyHost could NOT be written; ByHost($UUID1) could NOT be written" "$T/p1" || fail "p1: partial write not said"
rows p1 "rdna3fb=1" > "$T/rp1"; cp "$T/rp1" "$T/out"
grep -q "^WARN autoreport.sh: useIOP / useIOP_onAMD true: AnyHost could NOT be written" "$T/rp1" || fail "p1: triage WARN"
rm -f "$T"/store/* "$T"/store_bh/*

# the boot-arg is a whole token: =0 alone or last or first takes the keys out; =01, =1, a prefix or suffix do not
n=820
for ba in "rdna3iopprefs=0" "rdna3fb=1 rdna3iopprefs=0" "rdna3iopprefs=0 rdna3fb=1" "-v rdna3iopprefs=0 rdna3fb=1"; do
    n=$((n + 1)); call w$n BOOTSEC=$n; call w${n}o BOOTSEC=$((n + 100)) BOOTARGS="$ba"
    [ -z "$(ls "$T/store")" ] && [ -z "$(ls "$T/store_bh")" ] || fail "'$ba' did not take the keys out of both stores"
    grep -qF "rdna3iopprefs=0: useIOP / useIOP_onAMD: AnyHost taken out; ByHost($UUID1) taken out" "$T/w${n}o" || fail "'$ba': taken out not said"
done
for ba in "rdna3iopprefs=01" "rdna3iopprefs=1" "xrdna3iopprefs=0" "rdna3iopprefs=0x" "rdna3iopprefs=" "rdna3iopprefs"; do
    n=$((n + 1)); rm -f "$T"/store/* "$T"/store_bh/*; call w$n BOOTSEC=$n BOOTARGS="$ba"
    [ -f "$T/store/useIOP_onAMD" ] && [ -f "$T/store/useIOP" ] && [ -f "$T/store_bh/useIOP_onAMD" ] && [ -f "$T/store_bh/useIOP" ] ||
        fail "'$ba' must write the keys into both stores"
    grep -qF "useIOP / useIOP_onAMD true: AnyHost written; ByHost($UUID1) written" "$T/w$n" || fail "'$ba': written not said"
done
rm -f "$T"/store/* "$T"/store_bh/*

# --- the two stores on their own (DRIVER-PLAN N45 (1)) ---
# one store refuses the write: that one is "could NOT", the other is still written; triage WARNs
call s1 BOOTSEC=900 FAIL_BH=1
grep -qF "AnyHost written; ByHost($UUID1) could NOT be written" "$T/s1" || fail "s1: ByHost failure said for ByHost only"
[ -f "$T/store/useIOP_onAMD" ] && [ -f "$T/store/useIOP" ] || fail "s1: AnyHost not written when ByHost fails"
[ -z "$(ls "$T/store_bh")" ] || fail "s1: ByHost written although the stub refused"
rows s1 "rdna3fb=1" > "$T/rs1"; cp "$T/rs1" "$T/out"
grep -q "^WARN autoreport.sh: useIOP / useIOP_onAMD true: AnyHost written; ByHost($UUID1) could NOT be written" "$T/rs1" || fail "s1: triage WARN"
rm -f "$T"/store/* "$T"/store_bh/*
call s2 BOOTSEC=901 FAIL_ANY=1
grep -qF "AnyHost could NOT be written; ByHost($UUID1) written" "$T/s2" || fail "s2: AnyHost failure said for AnyHost only"
[ -f "$T/store_bh/useIOP_onAMD" ] && [ -f "$T/store_bh/useIOP" ] || fail "s2: ByHost not written when AnyHost fails"
rm -f "$T"/store/* "$T"/store_bh/*

# taking out: one store refuses, the other is cleaned
call s3 BOOTSEC=910
call s3o BOOTSEC=911 BOOTARGS="rdna3iopprefs=0" DELETE_FAIL_BH=1
grep -qF "AnyHost taken out; ByHost($UUID1) could NOT be taken out" "$T/s3o" || fail "s3: the ByHost delete failure said for ByHost only"
[ -z "$(ls "$T/store")" ] || fail "s3: AnyHost keys left"
[ -n "$(ls "$T/store_bh")" ] || fail "s3: the stub should have kept the ByHost keys"
rm -f "$T"/store/* "$T"/store_bh/*

# no IOPlatformUUID (ioreg without the line, or an implausible one that would end up in a path): AnyHost is
# still written, ByHost is "could NOT" with the reason, the state says ByHost(unknown): (unknown); triage WARNs
for u in "" "../../etc" "a b" 'x;y'; do
    rm -f "$T"/store/* "$T"/store_bh/*; : > "$T/dlog"
    call n1 BOOTSEC=920 UUID="$u"
    grep -qF "AnyHost written; ByHost(unknown) could NOT be written (host UUID unknown: ioreg gave no IOPlatformUUID)" "$T/n1" || fail "n1 '$u': the unknown UUID not said"
    grep -qF "started with: AnyHost: (none); ByHost(unknown): (unknown)" "$T/n1" || fail "n1 '$u': state line"
    [ -f "$T/store/useIOP_onAMD" ] || fail "n1 '$u': AnyHost not written without a UUID"
    [ -z "$(ls "$T/store_bh")" ] && ! grep -q "ByHost" "$T/dlog" || fail "n1 '$u': something written under ByHost without a UUID"
    rows n1 "rdna3fb=1" > "$T/rn1"; cp "$T/rn1" "$T/out"
    grep -q "^WARN autoreport.sh: useIOP / useIOP_onAMD true: AnyHost written; ByHost(unknown) could NOT be written (host UUID unknown" "$T/rn1" || fail "n1 '$u': triage WARN"
    grep -q "^INFO .*ByHost(unknown): not known (ioreg gave no IOPlatformUUID)" "$T/rn1" || fail "n1 '$u': triage INFO"
done
rm -f "$T"/store/* "$T"/store_bh/*
call n2 BOOTSEC=925 UUID="" BOOTARGS="rdna3iopprefs=0"
grep -qF "AnyHost taken out; ByHost(unknown) could NOT be taken out (host UUID unknown" "$T/n2" || fail "n2: =0 without a UUID"
rm -f "$T"/store/* "$T"/store_bh/*

# the ByHost directory does not exist yet: it is created; a UUID in lower case is a plausible one
rmdir "$T/ByHost"; call m1 BOOTSEC=930 UUID=abcdef01-2345-6789-abcd-ef0123456789
[ -d "$T/ByHost" ] || fail "m1: the ByHost directory was not created"
grep -qF "ByHost(abcdef01-2345-6789-abcd-ef0123456789) written" "$T/m1" || fail "m1: lower-case UUID"
rm -f "$T"/store/* "$T"/store_bh/*

# triage: only AnyHost held the keys at WindowServer's start -> WARN "may read the ByHost store" (the evidence is
# macOS 26's), amd still "1"; only ByHost, or both: no WARN; ByHost with other keys only counts as unset
st() {   # st <AnyHost domain> <ByHost domain> -> the tests file's "started with" line
    printf "== CoreDisplay prefs (/Library/Preferences/com.apple.CoreDisplay, DRIVER-PLAN N44): this boot's WindowServer started with: AnyHost: %s; ByHost(%s): %s\n[exit 0]\n" "$1" "$UUID1" "$2" > "$T/st"
}
st "$BOTH" "(none)"
rows st "rdna3fb=1" > "$T/rst1"; cp "$T/rst1" "$T/out"
grep -q "^WARN useIOP_onAMD is in the AnyHost store only, not in ByHost($UUID1): CoreDisplay may read the ByHost store" "$T/rst1" || fail "st1: AnyHost-only WARN"
grep -q "^INFO .*AnyHost: useIOP_onAMD 1, useIOP 1; ByHost($UUID1): useIOP_onAMD unset (static 0), useIOP unset (static 1) " "$T/rst1" || fail "st1: each store on its own in the INFO row"
grep -qx "amd=1" "$T/rst1" || fail "st1: amd"
st "(none)" "$BOTH"
rows st "rdna3fb=1" > "$T/rst2"; cp "$T/rst2" "$T/out"
! grep -q "^WARN" "$T/rst2" || fail "st2: ByHost-only holds the keys: no WARN"
grep -q "^INFO .*AnyHost: useIOP_onAMD unset (CoreDisplay's static 0), useIOP unset (static 1); ByHost($UUID1): useIOP_onAMD 1, useIOP 1 " "$T/rst2" || fail "st2: INFO row"
grep -qx "amd=1" "$T/rst2" || fail "st2: amd"
st "$BOTH" '{ "useIOP_onAMD" = 0; useIOP = 1; }'
rows st "rdna3fb=1" > "$T/rst3"; cp "$T/rst3" "$T/out"
grep -q "^WARN useIOP_onAMD is in the AnyHost store only" "$T/rst3" || fail "st3: ByHost holds useIOP_onAMD 0: AnyHost-only WARN"
st "$BOTH" '{ SomethingElse = 1; }'
rows st "rdna3fb=1" > "$T/rst4"; cp "$T/rst4" "$T/out"
grep -q "^WARN useIOP_onAMD is in the AnyHost store only" "$T/rst4" || fail "st4: ByHost without the keys: WARN"
st "$BOTH" "(unknown)"
rows st "rdna3fb=1" > "$T/rst5"; cp "$T/rst5" "$T/out"
! grep -q "AnyHost store only" "$T/rst5" || fail "st5: ByHost not known: no 'AnyHost only' claim"
grep -q "ByHost($UUID1): not known" "$T/rst5" || fail "st5: not known said"
# the same, rdna3iopprefs=0 and AnyHost-only: no "AnyHost only" noise beyond the earlier-boot WARN
st "(none)" "(none)"
rows st "rdna3fb=1 rdna3iopprefs=0" > "$T/rst6"; cp "$T/rst6" "$T/out"
grep -q "rdna3iopprefs=0: the A/B without them$" "$T/rst6" && ! grep -q "^WARN" "$T/rst6" || fail "st6: =0, both stores empty: the A/B, no WARN"
st "(none)" "$BOTH"
rows st "rdna3fb=1 rdna3iopprefs=0" > "$T/rst7"; cp "$T/rst7" "$T/out"
grep -q "^WARN rdna3iopprefs=0 but WindowServer started with useIOP_onAMD 1" "$T/rst7" || fail "st7: =0 with ByHost keys at the start is not the A/B"

# collect.sh: the log filter of 24-log-coredisplay.txt carries IOPresentment's capability strings (triage's IOPresentment log row)
for str in "Invalid end point size" "capabilities with no devices" "device with no planes" "Failed to get capabilities"; do
    grep -q "run 24-log-coredisplay.txt .*eventMessage CONTAINS .*$str" "$ROOT/tools/macos/collect.sh" || fail "collect.sh: 24-log-coredisplay.txt does not select '$str'"
done

# bash 3.2 / BSD: the shell code this test covers (autoreport.sh's prefs functions, collect.sh's line) uses none of
# the GNU-only forms (the Mac runner runs this test with /bin/bash 3.2 and BSD tools)
for f in "$T/fn.sh"; do
    ! grep -n -E 'mapfile|readarray|find .*-printf|grep -[a-zA-Z]*P|date -d|sed -i[^.]|stat -c' "$f" || fail "GNU-only form in the prefs functions"
done

# rdna3vm passes the off switch to the install boot only as a whole token (the case of cmd__install, run as it is)
sed -n '/^    case " ${RDNA3VM_BOOTARGS:-} " in \*" rdna3iopprefs=0 "\*) iop=/p' "$ROOT/tools/vm/rdna3vm" > "$T/rv.sh"
[ "$(wc -l < "$T/rv.sh" | tr -d " ")" = 1 ] || fail "rdna3vm: the iop case line not found"
passes() { ( iop=; RDNA3VM_BOOTARGS="$1"; . "$T/rv.sh"; printf '%s' "$iop" ); }
[ "$(passes "rdna3iopprefs=0")" = " rdna3iopprefs=0" ] || fail "rdna3vm: =0 alone is not passed"
[ "$(passes "-v rdna3iopprefs=0 rdna3fb=1")" = " rdna3iopprefs=0" ] || fail "rdna3vm: =0 in the middle is not passed"
[ "$(passes "rdna3fb=1 rdna3iopprefs=0")" = " rdna3iopprefs=0" ] || fail "rdna3vm: =0 last is not passed"
for ba in "" "rdna3iopprefs=01" "rdna3iopprefs=1" "xrdna3iopprefs=0" "rdna3iopprefs=0x"; do
    [ -z "$(passes "$ba")" ] || fail "rdna3vm: '$ba' must not be passed as the off switch"
done

for f in b1 b1b b2 b3 b4 b5 b6 u1 u2 t1 t2 k1 d2 p1 s1 s2 s3o n1 n2 m1; do grep -qx "\[exit 0\]" "$T/$f" || fail "$f: the function did not return 0"; done
echo "test_coredisplay_prefs: ok"
