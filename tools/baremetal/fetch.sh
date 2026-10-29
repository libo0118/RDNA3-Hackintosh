#!/bin/bash
# fetch.sh [OUTDIR] - everything the bare-metal OpenCore USB needs, downloaded and checked.
#
#   OUTDIR/dl/        the pinned zips and .aml files (tools/baremetal/pins.txt, sha256 checked)
#   OUTDIR/src/NAME/  each zip unpacked
#   OUTDIR/recovery/  the macOS recovery (BaseSystem.dmg + .chunklist) from Apple, by OpenCore's
#                     macrecovery.py; it goes to com.apple.recovery.boot on the USB
#
# OUTDIR defaults to ~/rdna3-baremetal. Safe to run again: a file with the right sha256 and
# a finished recovery download are kept. Needs curl, unzip, sha256sum and python3.
# Environment: RECOVERY_BOARD (default Mac-27AD2F918AE68F61 = MacPro7,1, the SMBIOS the
# config uses), RECOVERY_SKIP=1 (no recovery download, for tests / when it is already there).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HOME/rdna3-baremetal}
PINS=${PINS_FILE:-$HERE/pins.txt}
RECOVERY_BOARD=${RECOVERY_BOARD:-Mac-27AD2F918AE68F61}
step=0
total=3
say()  { step=$((step + 1)); printf '\n[%d/%d] %s\n' "$step" "$total" "$*"; }
die()  { echo "fetch.sh: $*" >&2; exit 1; }
for t in curl unzip sha256sum python3; do command -v "$t" >/dev/null || die "$t is needed"; done
mkdir -p "$OUT/dl" "$OUT/src"

sha_of() { sha256sum "$1" | cut -d' ' -f1; }

say "Pinned releases -> $OUT/dl (sha256 checked)"
while read -r name ver sha url; do
    case "$name" in ''|'#'*) continue ;; esac
    f=$OUT/dl/${url##*/}
    if [ -f "$f" ] && [ "$(sha_of "$f")" = "$sha" ]; then
        echo "  ok     $name $ver (already there)"
    else
        echo "  fetch  $name $ver"
        curl -fsSL --retry 3 -o "$f.part" "$url" || die "download of $url failed"
        got=$(sha_of "$f.part")
        if [ "$got" != "$sha" ]; then
            rm -f "$f.part"
            die "$name $ver: sha256 $got, pins.txt says $sha. NOT used. Look at the release before trusting a new file."
        fi
        mv "$f.part" "$f"
        echo "  ok     $name $ver"
    fi
    case "$f" in
    *.zip)
        d=$OUT/src/$name
        rm -rf "$d"; mkdir -p "$d"
        unzip -q -o "$f" -d "$d"
        ;;
    esac
done < "$PINS"

say "Unpacked into $OUT/src (OpenCore: $(ls "$OUT/src/OpenCorePkg" | tr '\n' ' '))"
for t in ocvalidate/ocvalidate.linux macserial/macserial.linux; do
    [ -f "$OUT/src/OpenCorePkg/Utilities/$t" ] || die "OpenCorePkg zip has no Utilities/$t"
    chmod +x "$OUT/src/OpenCorePkg/Utilities/$t"
done

say "macOS recovery (board $RECOVERY_BOARD, latest) -> $OUT/recovery"
if [ -n "${RECOVERY_SKIP:-}" ]; then
    echo "  skipped (RECOVERY_SKIP)"
elif [ -f "$OUT/recovery/.complete" ] && [ -s "$OUT/recovery/BaseSystem.dmg" ] && [ -s "$OUT/recovery/BaseSystem.chunklist" ]; then
    echo "  already downloaded ($(du -m "$OUT/recovery/BaseSystem.dmg" | cut -f1) MB)"
else
    mkdir -p "$OUT/recovery"
    rm -f "$OUT/recovery/.complete"
    mac=$OUT/src/OpenCorePkg/Utilities/macrecovery
    # -os latest: the newest macOS this board gets; the script checks every chunk's hash itself
    python3 -I "$mac/macrecovery.py" -o "$OUT/recovery" -b "$RECOVERY_BOARD" -m 00000000000000000 \
        -os latest download 2>&1 | tr '\r' '\n' | grep -v "downloaded$" || true
    [ -s "$OUT/recovery/BaseSystem.dmg" ] && [ -s "$OUT/recovery/BaseSystem.chunklist" ] ||
        die "macrecovery.py did not leave BaseSystem.dmg + BaseSystem.chunklist in $OUT/recovery"
    : > "$OUT/recovery/.complete"
fi
if [ -s "$OUT/recovery/BaseSystem.dmg" ]; then
    sha_of "$OUT/recovery/BaseSystem.dmg" > "$OUT/recovery/BaseSystem.dmg.sha256"
    echo "  BaseSystem.dmg sha256 $(cat "$OUT/recovery/BaseSystem.dmg.sha256")"
fi
echo
echo "Done. Next: python3 -I $HERE/build_efi.py --out $OUT"
