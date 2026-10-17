#!/bin/sh
# Make the Metal plugin's frame trace directory (metal/plugin/nvmtl_trace.h): /var/tmp/rdna3-nvmtl, root:admin, mode
# 1777 (sticky, world writable), so every process the plugin runs in can create its trace whatever starts first and
# the admin group (and root) can read the 0640 files for the report. Shared by package/pkg/postinstall (every
# install) and tools/vm/macos/build_metal.sh / autoreport.sh --sync-metal (the VM, where WindowServer would otherwise
# create it first, as _windowserver:wheel, and the user could not read the traces).
#   fix_tracedir.sh [target volume root]       run as root; default the running system
# /var/tmp is world writable, so what stands at that name is never trusted or followed:
#   - a symlink or a file there is removed (rm -f, not followed);
#   - a directory of root's or _windowserver's (uid 88) is KEPT with its traces (WindowServer's live one among them)
#     and only chown'ed root:admin; a directory of anybody else's is removed (it holds nothing of the desktop's);
#   - a missing directory is made by mkdir -m 1777 (no -p for it);
#   - afterwards it must be a real directory, and root's on a real install.
# Exit 1 with a "RDNA3: WARNING" line when it cannot.
set -u
DST=${1:-}
[ "$DST" = / ] && DST=
PARENT="$DST/private/var/tmp"
TD="$PARENT/rdna3-nvmtl"
me=$(id -u)
sys=0; [ "$(uname -s)" = Darwin ] && [ "$me" = 0 ] && sys=1
fail() { echo "RDNA3: WARNING: $TD: $* (the Metal plugin's frame trace may not be written, or not be readable)" >&2; exit 1; }
owner() { ls -ldn "$1" 2>/dev/null | awk '{print $3}'; }

# the parent exists on every macOS; only a volume root under test or install lacks it
[ -d "$PARENT" ] || mkdir -p "$PARENT" || fail "cannot create $PARENT"
if [ -L "$TD" ] || { [ -e "$TD" ] && [ ! -d "$TD" ]; }; then
    rm -f "$TD" || fail "cannot remove what is planted at that name"
fi
if [ -d "$TD" ]; then
    case "$(owner "$TD")" in
    0|88) ;;                                         # keep it, and its traces
    "$me") ;;                                        # (a test run as an ordinary user keeps its own)
    *) rm -rf "$TD" || fail "cannot remove a directory owned by somebody else" ;;
    esac
fi
[ -d "$TD" ] || mkdir -m 1777 "$TD" || fail "cannot create it"
# a real directory now: not a link, and of an owner we accept
[ ! -L "$TD" ] && [ -d "$TD" ] || fail "not a directory"
o=$(owner "$TD")
case "$o" in 0|88|"$me") ;; *) fail "owned by uid $o" ;; esac
# never through a link: a uid-88 owner could swap the entry for a symlink
# after the check above (-h acts on the link itself; GNU chmod has no -h)
hflag=; [ "$(uname)" = Darwin ] && hflag=-h
if [ $sys = 1 ]; then
    chown -h root:admin "$TD" || fail "cannot chown root:admin"
fi
chmod $hflag 1777 "$TD" || fail "cannot chmod"
[ ! -L "$TD" ] && [ -d "$TD" ] || fail "not a directory after chmod"
if [ $sys = 1 ]; then
    [ "$(owner "$TD")" = 0 ] || fail "not root's"
fi
exit 0
