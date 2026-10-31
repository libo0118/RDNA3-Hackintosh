#!/bin/sh
# tools/ci/fetch_fw.sh DEST - the 13 Navi 32 firmware files the host tests read (RDNA3_FW_DIR of `make test`), for CI.
# The firmware is not redistributed, so it is fetched: the tag and the SHA-256 of every file are the ones of
# package/firmware.sh (the installer's), nothing unpinned or unchecked reaches DEST.
#  - a cache first ($RDNA3_FW_CACHE, default ~/.cache/rdna3-fw; the Linux runner's jobs run in a fresh container, so it
#    only helps where the directory is restored, e.g. by actions/cache); a cached file that fails its SHA-256 is dropped
#  - then gitlab.com, then git.kernel.org (the official repository; both serve the tag), a few tries each with a short
#    back-off; an HTTP 429, 5xx or 404 moves to the next source at once instead of waiting (the old 6 retries x 10 s
#    on 13 files held the job for 14 minutes on gitlab's 429)
#  - a time budget for the whole step ($RDNA3_FW_BUDGET s, default 150): when it is used up the rest is not tried
#  - a file lands in the cache only whole and verified (written to a temporary name inside the cache, then renamed; a
#    cancelled job leaves only .tmp.* there, which the next run removes)
# Exit status is always 0. DEST/.complete exists only when all files are there and verified; the caller runs the
# firmware-dependent checks only then (the tests print a "skip" note when they get no firmware directory).
# POSIX sh (also runs on macOS).
set -u
DEST=${1:?usage: fetch_fw.sh DEST}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FWSH=$ROOT/package/firmware.sh
CACHE=${RDNA3_FW_CACHE:-$HOME/.cache/rdna3-fw}
BUDGET=${RDNA3_FW_BUDGET:-150}
PIN=$(sed -n 's/^PIN=//p' "$FWSH")
FILES=$(sh "$FWSH" --list)
SUMS=$(grep -oE '[a-z0-9_]+\.bin [0-9a-f]{64}' "$FWSH")
if [ -z "$PIN" ] || [ -z "$FILES" ] || [ -z "$SUMS" ]; then
    echo "fetch_fw: cannot read the pin / files / sums from $FWSH"
    exit 0
fi

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
verified() {   # verified FILE NAME: SHA-256 equals the pinned one
    [ -s "$1" ] && [ "$(sha256 "$1")" = "$(echo "$SUMS" | awk -v f="$2" '$1 == f { print $2 }')" ]
}

mkdir -p "$CACHE" "$DEST" || { echo "fetch_fw: cannot create $CACHE / $DEST"; exit 0; }
find "$CACHE" -maxdepth 1 -name '.tmp.*' -mmin +30 -exec rm -rf {} + 2>/dev/null   # a killed run's leftovers
TMP=$CACHE/.tmp.$$
mkdir -p "$TMP" || exit 0
trap 'rm -rf "$TMP"' EXIT
trap 'exit 1' INT TERM
rm -f "$DEST/.complete"
T0=$(date +%s)
got=0
cached=0
missing=""

fetch_one() {   # fetch_one NAME: 0 when $CACHE/NAME is there and verified afterwards
    f=$1
    for base in "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$PIN/amdgpu" \
                "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/amdgpu"; do
        case "$base" in *git.kernel.org*) url="$base/$f?id=$PIN" ;; *) url="$base/$f" ;; esac
        host=$(echo "$url" | cut -d/ -f3)
        try=0
        while [ $try -lt 3 ]; do
            if [ $(( $(date +%s) - T0 )) -ge "$BUDGET" ]; then echo "fetch_fw: time budget ($BUDGET s) used up"; return 1; fi
            try=$((try + 1))
            code=$(curl -sSL --connect-timeout 10 --max-time 60 -o "$TMP/$f" -w '%{http_code}' "$url" 2>"$TMP/err") || code=000
            if [ "$code" = 200 ] && verified "$TMP/$f" "$f"; then
                mv -f "$TMP/$f" "$CACHE/$f" && return 0
            fi
            why=""
            [ "$code" = 200 ] && why=", SHA-256 differs"
            [ -s "$TMP/err" ] && why="$why: $(head -c 120 "$TMP/err")"
            echo "fetch_fw: $f from $host: HTTP $code$why (try $try)"
            rm -f "$TMP/$f"
            case "$code" in 429|5??|404) break ;; esac   # rate limited / down / not there: the next source, no waiting here
            sleep $((try * 3))
        done
    done
    return 1
}

for f in $FILES; do
    if [ -s "$CACHE/$f" ] && ! verified "$CACHE/$f" "$f"; then
        echo "fetch_fw: cached $f fails its SHA-256, dropped"
        rm -f "$CACHE/$f"
    fi
    if [ -s "$CACHE/$f" ]; then cached=$((cached + 1))
    elif fetch_one "$f"; then got=$((got + 1))
    else missing="$missing $f"; continue; fi
    cp "$CACHE/$f" "$DEST/$f"
done

if [ -z "$missing" ]; then
    : > "$DEST/.complete"
    echo "fetch_fw: all $(echo "$FILES" | wc -w | tr -d ' ') files of linux-firmware $PIN verified ($cached from the cache, $got fetched, $(( $(date +%s) - T0 )) s)"
else
    rm -f "$DEST"/*.bin   # never a half set: the tests would fail on it instead of skipping
    echo "fetch_fw: NOT COMPLETE, missing:$missing - the checks that need the real firmware are skipped"
fi
exit 0
