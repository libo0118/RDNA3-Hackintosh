"""Triage rows of the boot watchdog (tools/vm/macos/autoreport.sh, "BOOTWATCH": card runs 2026-10-26e/f, the kext and the AGDC
came up, WindowServer never opened the accelerator, and the VM was killed with the unified log in it).

The VM uploads bootwatch-<YYYYMMDD-HHMMSS>-<n>.txt while WindowServer has no user client of the accelerator and nobody is logged
in (first at rdna3bootwatch=N s, default 75, then every 60 s): `BOOTWATCH:` header, ps, ioreg essentials, the plugin conf,
`sample WindowServer 5` (or spindump), `log show --last 3m`. When WindowServer opens the GPU (or somebody logs in) after all, the
last file is `BOOTWATCH-END: state=gpu|login ...`; state gave-up / timeout means the watch ended with WindowServer still without
the GPU. Kept in a module of its own so that tools/triage_reports.py only gains one call; docs/AFTER-TESTS.md "WindowServer
never opened the GPU" explains the rows."""
import glob
import os
import re

HEADER = re.compile(r"^BOOTWATCH: (.*)$", re.M)
END = re.compile(r"^BOOTWATCH-END: (.*)$", re.M)
NAME = re.compile(r"^bootwatch-(\d{8}-\d{6})-(\d+)\.txt$")
_KV = re.compile(r"(\w+)=(\S*)")
TOP_FRAMES = 6


def _kv(text):
    return dict(_KV.findall(text))


def _clean(frame):
    """a sample / spindump frame without its address and offset: 'mach_msg2_trap  (in libsystem_kernel.dylib)'"""
    frame = re.sub(r"\[0x[0-9a-fA-F]+\]", "", frame)
    frame = re.sub(r"\)\s*\+\s*\d+", ")", frame)
    frame = re.sub(r"\s+\+\s+\d+\s*$", "", frame)
    return re.sub(r"\s+", " ", frame).strip()


def _call_graph_chain(lines, start):
    """frames of the first thread's call graph that carry all of its samples (the stack it sat in for the whole sample),
    root first; lines[start] is the 'Call graph:' line"""
    chain, total = [], None
    for l in lines[start + 1:]:
        if not l.strip():
            if chain:
                break
            continue
        if l.startswith("Total number in stack") or l.startswith("Sort by top"):
            break
        m = re.match(r"^\s*[+!:| ]*(\d+)\s+(.*)$", l)
        if not m:
            continue
        n, rest = int(m.group(1)), m.group(2)
        if total is None:
            if not re.match(r"Thread_\d+", rest):
                continue
            total = n          # the thread's header line (the main thread comes first)
            continue
        if re.match(r"Thread_\d+", rest):
            break              # the next thread
        if n == total:
            chain.append(_clean(rest))
    return chain


def sample_frames(text):
    """(who, [frames, deepest first]) of the sample / spindump text in a bootwatch file: the main thread's stack that held every
    sample of `sample`'s call graph, else spindump's heaviest stack of the main thread, else the top of stack list; [] when the
    file has none"""
    lines = text.splitlines()
    for i, l in enumerate(lines):
        if l.strip() == "Call graph:":
            chain = _call_graph_chain(lines, i)
            if chain:
                return "main thread, in this stack for the whole sample", list(reversed(chain))[:TOP_FRAMES]
    for i, l in enumerate(lines):
        if l.startswith("Heaviest stack for the main thread"):
            fr = []
            for x in lines[i + 1:]:
                if not x.strip():
                    break
                m = re.match(r"^\s*(?:\d+\s+)?(\S.*)$", x)
                if m:
                    fr.append(_clean(m.group(1)))
            if fr:
                return "main thread, heaviest stack", list(reversed(fr))[:TOP_FRAMES]
    for i, l in enumerate(lines):
        if l.startswith("Sort by top of stack"):
            fr = []
            for x in lines[i + 1:]:
                if not x.strip():
                    if fr:
                        break
                    continue
                if x.startswith("Binary Images"):
                    break
                m = re.match(r"^\s+(\S.*?)\s{2,}(\d+)\s*$", x)
                if m:
                    fr.append("%s x%s" % (_clean(m.group(1)), m.group(2)))
            if fr:
                return "top of stack", fr[:TOP_FRAMES]
    return "", []


def _state_of(text):
    """a short phrase for what the file says about WindowServer: from the ps section"""
    if re.search(r"^WindowServer: NOT RUNNING", text, re.M):
        return "WindowServer was NOT RUNNING"
    return ""


def bootwatch_rows(*dirs):
    """[(level, label, text, detail_lines)] one per boot that left bootwatch files in the result directories: FAIL when
    WindowServer had no GPU client at the last look, WARN when it got one (or a user logged in) after the watch had fired,
    WARN again for a file that cannot be read (never silence)."""
    files = {}
    for d in dirs:
        if d and os.path.isdir(d):
            for p in glob.glob(os.path.join(d, "bootwatch-*.txt")):
                m = NAME.match(os.path.basename(p))
                if m:
                    files[(m.group(1), int(m.group(2)))] = p
    stamps = sorted({k[0] for k in files})
    rows = []
    for stamp in stamps:
        snaps, end, first_hdr, bad = [], None, {}, []
        for (st, n), p in sorted(files.items()):
            if st != stamp:
                continue
            try:
                txt = open(p, errors="replace").read()
            except OSError as e:
                bad.append("%s: %s" % (os.path.basename(p), e))
                continue
            e = END.search(txt)
            if e:
                end = _kv(e.group(1))
                continue
            h = HEADER.search(txt)
            if not h:
                bad.append("%s has no BOOTWATCH header (cut off?)" % os.path.basename(p))
                continue
            snaps.append((n, _kv(h.group(1)), txt))
            if not first_hdr:
                first_hdr = snaps[-1][1]
        if bad:
            rows.append(("WARN", "BOOTWATCH", "boot %s: %s" % (stamp, "; ".join(bad)), []))
        if not snaps and not end:
            continue
        trig = (first_hdr or end or {}).get("trigger_s", "75")
        recovered = end is not None and end.get("state") in ("gpu", "login")
        detail = []
        frames_who, frames = "", []
        # the newest snapshot that has a sample says where WindowServer sat
        for n, kv, txt in reversed(snaps):
            frames_who, frames = sample_frames(txt)
            if frames:
                break
        notes = _state_of(snaps[-1][2]) if snaps else ""
        if recovered:
            what = "got there only after %s s" % end.get("after_s", "?") if end.get("state") == "gpu" else \
                "had no GPU client when somebody logged in after %s s" % end.get("after_s", "?")
            text = ("boot %s: WindowServer did not reach the GPU within %s s, but %s (%d snapshot(s))"
                    % (stamp, trig, what, len(snaps)))
            level = "WARN"
        else:
            last = snaps[-1][1] if snaps else {}
            ended = "; the watch ended: %s" % end.get("state") if end else ""
            text = ("WindowServer did not reach the GPU within %s s (boot %s: %d snapshot(s), the last at %s s after the start, "
                    "console user %s%s%s)"
                    % (trig, stamp, len(snaps), last.get("after_s", "?"), last.get("console_user", "?") or "none",
                       ", " + notes if notes else "", ended))
            level = "FAIL"
        if frames:
            text += "; top of stack: " + " < ".join(f[:70] for f in frames[:3])
            detail.append("   bootwatch-%s: %s, deepest frame first:" % (stamp, frames_who))
            detail.extend("       " + f[:150] for f in frames)
        elif snaps:
            detail.append("   bootwatch-%s: no stack of WindowServer in the files (%s)" %
                          (stamp, notes or "sample and spindump gave nothing: read the file's '== sample' section"))
        if snaps:
            detail.append("   bootwatch-%s-N.txt: ps, ioreg, plugin conf, sample, log show of WindowServer / loginwindow / RDNA3 / NVMTL "
                          "(docs/AFTER-TESTS.md, WindowServer never opened the GPU)" % stamp)
        rows.append((level, "BOOTWATCH", text, detail))
    return rows
