#!/usr/bin/env python3
"""Decode the Metal plugin's per-process frame trace (metal/plugin/nvmtl_trace.h).

Every process that loads the plugin writes /var/tmp/rdna3-nvmtl/<process>-<pid>.trace
(an mmap'd ring of 128-byte records, plus a per-second summary). A report holds them
as 31-plugin-trace/ (tools/macos/collect_plugin_trace.sh). This tool answers, per
process and per IOSurface:

  - which surface each frame (command buffer) rendered into, and what the pass was
    (drawn / cleared to a colour and nothing else / left untouched);
  - what the plugin then did with that surface: marked it dirty (no copy), copied it
    out to the IOSurface, or failed; whether the kernel refused the dirty mark (errno);
    whether the data it published was EMPTY (every sampled pixel zero);
  - surface-in: uploaded, or skipped because the shared VRAM was believed current;
  - the Vulkan boundary: every failed submit, fence timeout, backend switch-off, and
    every allocation / import failure.

  plugin_trace.py PATH...            PATH: a .trace file, a directory of them, an
                                     extracted report (its 31-plugin-trace/), or a
                                     report .tar.gz
    --proc NAME      only processes whose name contains NAME (WindowServer is listed first anyway)
    --surface ID     the timeline of one IOSurface id
    --frames N       print the last N frames (command buffers) per process (default 12; 0 none)
    --all            every frame the ring still holds, and every failure event
    --rates          the per-second summary as a table (counts per second)
    --big WxH        what 'display sized' means for clear-only passes (default 1024x600)
    --utc-offset H   show wall-clock times shifted by H hours (the card's time zone)
    --check          exit 1 when a FAIL finding exists
    --triage         print only 'LEVEL text' lines, for tools/triage_reports.py

Times are seconds since the process opened its trace (+S.mmm) and the wall clock the
trace recorded then plus that (UTC unless --utc-offset); compare with the process's
'NVMTL: ... in pid N' log lines.
"""
import argparse
import collections
import datetime
import io
import os
import re
import stat
import struct
import sys
import tarfile

HDR = struct.Struct("<8s6I3Q2I4Q4I64s")
REC = struct.Struct("<QII4BIi7IQ4I12I")
REC_SIZE = 128
assert HDR.size == 176 and REC.size == REC_SIZE
MAX_FILE = 256 << 20       # a trace is 4 MB; nothing bigger is read (a report comes from a machine others can write to)
MAX_RECORDS = 1 << 21      # per region, what the header may claim
_UNPRINTABLE = re.compile(r"[^\x20-\x7e]")


def clean(text):
    """text from a trace or a file name, made safe to print (no escape sequences, no control characters)"""
    return _UNPRINTABLE.sub("?", str(text))

# kinds (metal/plugin/nvmtl_trace.h NVT_K_*)
K_SURF_IN, K_SURF_OUT, K_PASS_BEGIN, K_PASS_END = 2, 3, 4, 5
K_CB_COMMIT, K_CB_DIRTY, K_CB_DONE = 6, 7, 8
K_VK_SUBMIT, K_VK_WAIT, K_VK_DISABLE, K_ALLOC_FAIL = 9, 10, 11, 12
K_KREQ_PROBE, K_KREQ_DIRTY, K_KREQ_IMPORT, K_SURF_MAP = 13, 14, 15, 16
K_ENC_SPLIT = 17   # one per render encoder that drew: where its encode time went (nvt_ev_enc_split)
KIND_NAMES = {1: "START", 2: "SURF_IN", 3: "SURF_OUT", 4: "PASS_BEGIN", 5: "PASS_END", 6: "CB_COMMIT", 7: "CB_DIRTY",
              8: "CB_DONE", 9: "VK_SUBMIT", 10: "VK_WAIT", 11: "VK_DISABLE", 12: "ALLOC_FAIL", 13: "KREQ_PROBE",
              14: "KREQ_DIRTY", 15: "KREQ_IMPORT", 16: "SURF_MAP", 17: "ENC_SPLIT"}
IN_NAMES = {1: "UPLOAD", 2: "SKIP(shared VRAM current)", 3: "LOCK-FAILED", 4: "UPLOAD-FAILED", 5: "NOT-COPYABLE"}
OUT_NAMES = {1: "DIRTY(no copy)", 2: "COPY-OUT(private image)", 3: "COPY-OUT after dirty refused", 4: "LOCK-FAILED",
             5: "READBACK-FAILED", 6: "NOT-COPYABLE"}
PASS_NAMES = {1: "DRAWN", 2: "CLEAR-ONLY", 3: "empty pass (clears/resolves)", 4: "empty pass FAILED",
              5: "load pass with no draws (target untouched)", 6: "REFUSED", 7: "clear FAILED"}
SUB_NAMES = {1: "ok", 2: "vkQueueSubmit FAILED", 3: "failed before the submit", 4: "no backend (disabled)"}
WAIT_NAMES = {1: "ok", 2: "TIMEOUT", 3: "ERROR", 4: "completion witness missing"}
DIS_NAMES = {1: "queue submit failed", 2: "fence wait failed/timed out", 3: "completion witness missing"}
KD_NAMES = {1: "accepted", 2: "REFUSED", 3: "dirty path off", 4: "no Vulkan device"}
KI_NAMES = {1: "ok", 2: "no import fd", 3: "import refused", 4: "buffer bind refused", 5: "share off"}
WHY_NAMES = {1: "planar surface", 2: "texture wider than a row", 3: "texture taller than the surface", 4: "bytesPerRow not a multiple of 256",
             5: "surface sharing off", 6: "family VRAM import failed", 7: "image alias over the VRAM failed"}
LOAD = {0: "DontCare", 1: "Load", 2: "Clear"}
STORE = {0: "DontCare", 1: "Store", 2: "MultisampleResolve", 3: "StoreAndMultisampleResolve", 4: "Unknown", 5: "CustomSampleDepthStore"}
# MTLPixelFormat.h of the macOS 11.3 SDK
PIXFMT = {10: "R8Unorm", 70: "RGBA8Unorm", 71: "RGBA8Unorm_sRGB", 80: "BGRA8Unorm", 81: "BGRA8Unorm_sRGB", 90: "RGB10A2Unorm",
          92: "RG11B10Float", 94: "BGR10A2Unorm", 110: "RGBA16Unorm", 115: "RGBA16Float", 125: "RGBA32Float", 554: "BGR10_XR",
          555: "BGR10_XR_sRGB"}
VKRES = {0: "VK_SUCCESS", 1: "VK_NOT_READY", 2: "VK_TIMEOUT", -1: "VK_ERROR_OUT_OF_HOST_MEMORY", -2: "VK_ERROR_OUT_OF_DEVICE_MEMORY",
         -3: "VK_ERROR_INITIALIZATION_FAILED", -4: "VK_ERROR_DEVICE_LOST", -5: "VK_ERROR_MEMORY_MAP_FAILED",
         -7: "VK_ERROR_EXTENSION_NOT_PRESENT", -8: "VK_ERROR_FEATURE_NOT_PRESENT", -12: "VK_ERROR_TOO_MANY_OBJECTS",
         -13: "VK_ERROR_FORMAT_NOT_SUPPORTED", -1000072003: "VK_ERROR_INVALID_EXTERNAL_HANDLE",
         -1000158000: "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT"}
RC_DRM_PITCH = -1000158000
# macOS errno numbers (the card), not this host's
ERRNO = {1: "EPERM", 2: "ENOENT", 3: "ESRCH", 4: "EINTR", 5: "EIO", 6: "ENXIO", 9: "EBADF", 12: "ENOMEM", 13: "EACCES", 14: "EFAULT",
         16: "EBUSY", 17: "EEXIST", 19: "ENODEV", 20: "ENOTDIR", 22: "EINVAL", 24: "EMFILE", 28: "ENOSPC", 35: "EAGAIN",
         45: "ENOTSUP", 78: "ENOSYS"}
# summary counters (NVT_C_*)
COUNTERS = ["events", "in_upload", "in_skip", "in_synced", "in_fail", "out_copy", "out_dirty", "out_refused", "out_fail",
            "passes", "passes_surf", "clear_only", "clear_only_big", "cb_commit", "cb_done", "cb_err", "vk_submit",
            "vk_submit_err", "vk_wait_timeout", "vk_wait_err", "vk_disable", "alloc_fail", "kreq", "kreq_err", "map_vram",
            "map_private", "kb_in", "kb_out"]
F_VRAM, F_RO, F_HASHED, F_UNLOCKED, F_VIEW, F_CURRENT = 1, 2, 4, 8, 16, 32
SEVERITY = {"FAIL": 0, "WARN": 1, "INFO": 2}


def errno_name(e):
    if e < 0:
        return {-2: "dirty path switched off", -3: "no Vulkan device"}.get(e, "status %d" % e)
    return "errno %d %s" % (e, ERRNO.get(e, "?")) if e else "0"


def vk_name(r):
    return VKRES.get(r, "VkResult %d" % r)


def pixfmt(f):
    return PIXFMT.get(f, "fmt%d" % f)


def band_rows(band, height, rows):
    """nvt_band_rows == rd3_flicker_band_rows(band, 8, height, rows)"""
    if band >= 8 or not height or not rows:
        return []
    if band == 0:
        top, ln = 0, min(height, 32)
    else:
        rest = height - 32 if height > 32 else 0
        top = 32 + (rest * (band - 1)) // 7
        ln = 32 + (rest * band) // 7 - top
    out = []
    for i in range(rows):
        if not ln:
            break
        y = top + (ln * (2 * i + 1)) // (2 * rows)
        if y < height:
            out.append(y)
    return out


_ZERO = {}


def zero_bands(height):
    """the band hash of a picture whose sampled pixels are all zero (an empty buffer)"""
    if height not in _ZERO:
        out = []
        for b in range(8):
            v = 2166136261
            for _ in range(len(band_rows(b, height, 2 if b else 4)) * 48):
                v = (v * 16777619) & 0xffffffff
            out.append(v)
        _ZERO[height] = tuple(out)
    return _ZERO[height]


class Rec:
    __slots__ = ("abs", "seq", "tid", "kind", "path", "plane", "flags", "surf", "rc", "w", "h", "fmt", "bpr", "seed0", "seed1",
                 "dur_us", "bytes", "id", "a0", "a1", "a2", "words", "text", "ring", "t")

    def __init__(self, raw, ring):
        v = REC.unpack(raw)
        (self.abs, self.seq, self.tid, self.kind, self.path, self.plane, self.flags, self.surf, self.rc, self.w, self.h,
         self.fmt, self.bpr, self.seed0, self.seed1, self.dur_us, self.bytes, self.id, self.a0, self.a1, self.a2) = v[:21]
        self.words = v[21:]
        self.text = clean(raw[80:128].split(b"\0", 1)[0].decode("utf-8", "replace"))
        self.ring = ring
        self.t = 0.0

    @property
    def bands(self):
        return self.words[:8]

    def clear(self):
        return struct.unpack("<4f", struct.pack("<4I", *self.words[:4]))

    def empty(self):
        """a published picture whose sampled pixels are all zero"""
        return bool(self.flags & F_HASHED) and self.h > 0 and tuple(self.bands) == zero_bands(self.h)


class Trace:
    def __init__(self, name, data):
        self.name = name
        self.problems = []
        self.recs = []
        self.sums = []
        if len(data) < HDR.size or data[:8] != b"NVMTLTR1":
            raise ValueError("%s: not a plugin trace (bad magic or shorter than the header)" % name)
        (_, self.version, self.rec_size, self.nmain, self.nvk, self.nsum, self.pid, self.off_main, self.off_vk, self.off_sum,
         self.tb_n, self.tb_d, self.start_abs, self.start_wall_ns, self.widx_main, self.widx_vk, self.seq_next, self.flags,
         self.uid, self.ncnt, proc) = HDR.unpack_from(data, 0)
        self.proc = clean(proc.split(b"\0", 1)[0].decode("utf-8", "replace"))
        if self.version != 1 or self.rec_size != REC_SIZE or not self.tb_d:
            raise ValueError("%s: unsupported trace version %d / record size %d" % (name, self.version, self.rec_size))
        if max(self.nmain, self.nvk, self.nsum) > MAX_RECORDS:
            raise ValueError("%s: the header claims %d / %d / %d records (more than %d)" % (name, self.nmain, self.nvk, self.nsum, MAX_RECORDS))
        for ring, off, n in ((0, self.off_main, self.nmain), (1, self.off_vk, self.nvk)):
            if off + n * REC_SIZE > len(data):
                self.problems.append("ring %d is cut off: the file holds %d of %d bytes" % (ring, max(0, len(data) - off), n * REC_SIZE))
                n = max(0, (len(data) - off) // REC_SIZE)
            for i in range(n):
                raw = data[off + i * REC_SIZE: off + (i + 1) * REC_SIZE]
                if len(raw) < REC_SIZE or raw[8:12] == b"\0\0\0\0":
                    continue   # empty slot (seq 0) or torn by the process dying mid-write
                r = Rec(raw, ring)
                if r.kind:
                    self.recs.append(r)
        self.recs.sort(key=lambda r: r.seq)
        for r in self.recs:
            r.t = self.secs(r.abs)
        if off_ok(self.off_sum, self.nsum, len(data)):
            for i in range(self.nsum):
                o = self.off_sum + i * REC_SIZE
                sec1, _flags, first = struct.unpack_from("<IIQ", data, o)
                if sec1 in (0, 0xffffffff):
                    continue
                cnt = struct.unpack_from("<%dI" % min(self.ncnt, len(COUNTERS)), data, o + 16)
                self.sums.append((sec1 - 1, first, dict(zip(COUNTERS, cnt))))
            self.sums.sort()
        elif self.nsum:
            self.problems.append("the per-second summary is cut off")
        self.total_events = self.widx_main + self.widx_vk
        self.kept = len(self.recs)

    def secs(self, ticks):
        return max(0, ticks - self.start_abs) * self.tb_n / self.tb_d / 1e9

    def wall(self, r, utc_offset=0.0):
        try:
            ns = self.start_wall_ns + max(0, r.abs - self.start_abs) * self.tb_n // self.tb_d
            d = datetime.datetime.fromtimestamp(ns / 1e9, datetime.timezone.utc) + datetime.timedelta(hours=utc_offset)
            return d.strftime("%H:%M:%S.%f")[:-3]
        except (OverflowError, ValueError, OSError):
            return "??:??:??.???"   # a header with an impossible clock

    def complete_history(self):
        """the main ring never wrapped (or a snapshot merged in saw it before it did): every main-ring event
        since the trace opened is still here"""
        return self.widx_main <= self.nmain or getattr(self, "_complete", False)

    def merge(self, other):
        """another copy of the same file (an A/B snapshot, the archive's final copy): its records the ring has
        overwritten since are added, by (ring, seq) (ECC review round 3: the newest copy alone lost the A/B)"""
        have = set((r.ring, r.seq) for r in self.recs)
        add = [r for r in other.recs if (r.ring, r.seq) not in have]
        for r in add:
            r.t = self.secs(r.abs)
        self.recs.extend(add)
        self.recs.sort(key=lambda r: r.seq)
        self.kept = len(self.recs)
        self._complete = self.complete_history() or other.complete_history()
        self.merged = getattr(self, "merged", 1) + 1
        self.problems.extend(p for p in other.problems if p not in self.problems)
        return len(add)

    def opened(self):
        try:
            return datetime.datetime.fromtimestamp(self.start_wall_ns / 1e9, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
        except (OverflowError, ValueError, OSError):
            return "?"

    def wall_s(self, ticks):
        """a record's wall clock in seconds since 1970 (float), None for an impossible clock"""
        try:
            v = (self.start_wall_ns + max(0, ticks - self.start_abs) * self.tb_n // self.tb_d) / 1e9
            return v if 0 < v < 1e11 else None
        except (OverflowError, ValueError, ZeroDivisionError):
            return None

    def kept_window(self):
        """(first, last) wall seconds of the main ring's kept records, or None"""
        main = [r for r in self.recs if r.ring == 0]
        if not main:
            return None
        a, b = self.wall_s(main[0].abs), self.wall_s(main[-1].abs)
        return (a, b) if a is not None and b is not None else None

    def totals(self):
        t = collections.Counter()
        for _s, _f, c in self.sums:
            t.update(c)
        return t


def off_ok(off, n, size):
    return off > 0 and off + n * REC_SIZE <= size


# ---------------------------------------------------------------------------------------------- loading

def read_regular(path):
    """the bytes of a regular file of a sane size; ValueError otherwise (a symlink, a device, a 300 MB file)"""
    st = os.lstat(path)
    if not stat.S_ISREG(st.st_mode):
        raise ValueError("%s: not a regular file (a link or special file is not read)" % clean(path))
    if st.st_size > MAX_FILE:
        raise ValueError("%s: %d bytes, more than %d (not read)" % (clean(path), st.st_size, MAX_FILE))
    with open(path, "rb") as fh:
        return fh.read(MAX_FILE + 1)


def utc(sec):
    try:
        return datetime.datetime.fromtimestamp(sec, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    except (OverflowError, ValueError, OSError, TypeError):
        return "?"


def load_paths(paths, boottime=None):
    """(traces, problems). A report .tar.gz also takes the snapshots autoreport.sh sent next to it
    (<report>-plugin-<when>-<process>-<pid>.trace: a later cut must not lose the A/B); one trace per
    process (the one with the most events wins). boottime (seconds since 1970, else the archive's
    31-plugin-trace/info.txt 'boottime N' line): traces opened before it are another boot's, not judged."""
    traces, problems = [], []
    found_boot = [boottime]

    def add(name, data):
        try:
            traces.append(Trace(clean(name), data))
        except (ValueError, struct.error) as e:
            problems.append(clean(e))

    for p in paths:
        try:
            if os.path.isdir(p) and not os.path.islink(p):
                sub = os.path.join(p, "31-plugin-trace")
                d = sub if os.path.isdir(sub) and not os.path.islink(sub) else p
                names = sorted(f for f in os.listdir(d) if f.endswith(".trace"))
                if not names:
                    problems.append("%s: no .trace files" % clean(d))
                for f in names:
                    try:
                        add(f, read_regular(os.path.join(d, f)))
                    except (ValueError, OSError) as e:
                        problems.append(clean(e))
            elif p.endswith((".tar.gz", ".tgz", ".tar")):
                n0 = len(traces)
                with tarfile.open(p) as tar:
                    for m in tar:
                        if m.isfile() and m.name.endswith("31-plugin-trace/info.txt") and m.size < (1 << 20):
                            bt = re.search(r"^boottime (\d+)", tar.extractfile(m).read().decode("ascii", "replace"), re.M)
                            if bt and found_boot[0] is None:
                                found_boot[0] = int(bt.group(1))
                        if not (m.isfile() and m.name.endswith(".trace")):
                            continue
                        if m.size > MAX_FILE:
                            problems.append("%s: %d bytes, more than %d (not read)" % (clean(m.name), m.size, MAX_FILE))
                            continue
                        add(os.path.basename(m.name), tar.extractfile(m).read())
                in_archive = len(traces) - n0
                base = p[:-len(".tar.gz")] if p.endswith(".tar.gz") else p.rsplit(".", 1)[0]
                import glob as _glob
                for f in sorted(_glob.glob(_glob.escape(base) + "-plugin-*.trace")):
                    try:
                        add(os.path.basename(f), read_regular(f))
                    except (ValueError, OSError) as e:
                        problems.append(clean(e))
                if len(traces) == n0:
                    problems.append("%s: no plugin traces in the archive (collect_plugin_trace.sh did not run, or the plugin is older)" % clean(p))
                elif not in_archive:
                    problems.append("%s: no plugin traces in the archive; the A/B snapshots sent next to it are read" % clean(p))
            else:
                add(os.path.basename(p), read_regular(p))
        except (ValueError, OSError, tarfile.TarError) as e:
            problems.append(clean(e))
    # one trace per process: the archive's final copy and the A/B snapshots of the same file, merged (the
    # newest copy's header and summary, every record any copy still held)
    groups = {}
    for t in traces:
        groups.setdefault((t.proc, t.pid, t.start_wall_ns), []).append(t)
    traces = []
    for ts in groups.values():
        ts.sort(key=lambda t: -t.total_events)
        base = ts[0]
        for o in ts[1:]:
            base.merge(o)
        traces.append(base)
    bt = found_boot[0]
    if bt:
        old = [t for t in traces if t.start_wall_ns / 1e9 < bt - 120]   # the collector's tolerance (NTP after the boot)
        for t in old:
            problems.append("%s[%d] (%s): OLD - opened %s UTC, before this boot (%s UTC): not judged" %
                            (t.proc, t.pid, t.name, t.opened(), utc(bt)))
        traces = [t for t in traces if t not in old]
    traces.sort(key=lambda t: (t.proc != "WindowServer", t.proc, t.pid))
    return traces, problems


# ---------------------------------------------------------------------------------------------- analysis

class Frame:
    """one command buffer: its passes, the surfaces it dirtied, what was published for them"""
    __slots__ = ("cb", "commit", "dirty", "passes", "outs", "ins", "done", "first")

    def __init__(self, cb):
        self.cb = cb
        self.commit = self.done = None
        self.dirty, self.outs, self.ins = [], [], []
        self.passes = collections.OrderedDict()   # pass id -> {"begin": [rec], "end": rec|None}
        self.first = None


class Surf:
    __slots__ = ("sid", "w", "h", "vram", "plane", "frames", "drawn", "clear_only", "other_pass", "out_dirty", "out_copy",
                 "out_refused", "out_fail", "empty_out", "in_upload", "in_skip", "in_fail", "map", "events", "refused_errno",
                 "clears", "published", "dirtied")

    def __init__(self, sid):
        self.sid = sid
        self.w = self.h = 0
        self.vram = None
        self.plane = 0
        self.frames = self.drawn = self.clear_only = self.other_pass = 0
        self.out_dirty = self.out_copy = self.out_refused = self.out_fail = self.empty_out = 0
        self.in_upload = self.in_skip = self.in_fail = 0
        self.map = None
        self.events = []
        self.refused_errno = collections.Counter()
        self.clears = collections.Counter()
        self.published = self.dirtied = 0


class Analysis:
    def __init__(self, tr, big=(1024, 600)):
        self.tr = tr
        self.big = big
        self.frames = collections.OrderedDict()
        self.surfs = {}
        self.findings = []   # (level, code, text, [detail lines])
        self.vk = [r for r in tr.recs if r.kind in (K_VK_SUBMIT, K_VK_WAIT, K_VK_DISABLE)]
        self._build()
        self._find()

    def surf(self, sid):
        s = self.surfs.get(sid)
        if s is None:
            s = self.surfs[sid] = Surf(sid)
        return s

    def frame(self, cb):
        f = self.frames.get(cb)
        if f is None:
            f = self.frames[cb] = Frame(cb)
        return f

    def _geometry(self, s, r):
        if r.w and r.h and (r.w * r.h >= s.w * s.h):
            s.w, s.h = r.w, r.h
        if r.flags & F_VRAM:
            s.vram = True

    def _build(self):
        for r in self.tr.recs:
            k = r.kind
            if k in (K_SURF_IN, K_SURF_OUT, K_PASS_BEGIN, K_PASS_END, K_CB_DIRTY, K_SURF_MAP, K_KREQ_DIRTY, K_KREQ_IMPORT) and r.surf:
                s = self.surf(r.surf)
                s.events.append(r)
                self._geometry(s, r)
            if k == K_SURF_MAP:
                s = self.surf(r.surf)
                s.map = r
                s.vram = r.path == 1
            if k == K_CB_COMMIT:
                f = self.frame(r.id)
                f.commit = r
                f.first = f.first or r
            elif k == K_CB_DIRTY:
                f = self.frame(r.id)
                f.dirty.append(r)
                f.first = f.first or r
                self.surf(r.surf).dirtied += 1
            elif k == K_CB_DONE:
                self.frame(r.id).done = r
            elif k == K_PASS_BEGIN:
                f = self.frame(r.id)
                f.first = f.first or r
                f.passes.setdefault(r.a0, {"begin": [], "end": None})["begin"].append(r)
            elif k == K_PASS_END:
                f = self.frame(r.id)
                f.first = f.first or r
                f.passes.setdefault(r.a0, {"begin": [], "end": None})["end"] = r
                s = self.surf(r.surf)
                s.frames += 1
                if r.path == 1:
                    s.drawn += 1
                elif r.path == 2:
                    s.clear_only += 1
                    s.clears[tuple(round(c, 3) for c in r.clear())] += 1
                else:
                    s.other_pass += 1
            elif k == K_SURF_OUT:
                s = self.surf(r.surf)
                if r.id:
                    self.frame(r.id).outs.append(r)
                if r.path == 1:
                    s.out_dirty += 1; s.published += 1
                elif r.path in (2, 3):
                    s.out_copy += 1; s.published += 1
                    if r.path == 3:
                        s.out_refused += 1
                        s.refused_errno[r.rc] += 1
                    if r.empty():
                        s.empty_out += 1
                else:
                    s.out_fail += 1
            elif k == K_SURF_IN:
                s = self.surf(r.surf)
                if r.id:
                    self.frame(r.id).ins.append(r)
                if r.path == 1:
                    s.in_upload += 1
                elif r.path == 2:
                    s.in_skip += 1
                else:
                    s.in_fail += 1

    # -- findings --------------------------------------------------------------------------------
    def add(self, level, code, text, detail=()):
        self.findings.append((level, code, text, list(detail)))

    def sdesc(self, s):
        return "surface %d %dx%d%s" % (s.sid, s.w, s.h, "" if s.vram is None else (" (shared VRAM)" if s.vram else " (private image)"))

    def _find(self):
        tr = self.tr
        # 1. rendered / dirtied in a command buffer and never published
        never = collections.defaultdict(list)
        unfinished = collections.defaultdict(list)
        for f in self.frames.values():
            if not f.commit or not f.dirty:
                continue
            pubs = {o.surf for o in f.outs if o.path in (1, 2, 3)}
            for d in f.dirty:
                if d.surf in pubs:
                    continue
                (unfinished if f.done is None else never)[d.surf].append(f)
        for sid, fl in sorted(never.items()):
            s = self.surf(sid)
            bad = [o for f in fl for o in f.outs if o.surf == sid and o.path > 3]
            det = ["cb %d at +%.3fs: %s" % (f.cb, f.commit.t, "; ".join(
                "%s rc=%d" % (OUT_NAMES.get(o.path, o.path), o.rc) for o in f.outs if o.surf == sid) or "no surface-out ran") for f in fl[:3]]
            self.add("FAIL", "never-published",
                     "%s: %d command buffer(s) wrote it but nothing was published (no dirty mark, no copy-out)%s" %
                     (self.sdesc(s), len(fl), " - %d surface-out failure(s)" % len(bad) if bad else ""), det)
        for sid, fl in sorted(unfinished.items()):
            if len(fl) > 3:
                self.add("WARN", "unfinished", "%s: %d command buffers committed but not finished by the end of the trace" %
                         (self.sdesc(self.surf(sid)), len(fl)))
        # 2. dirty marks the kernel refused
        for sid, s in sorted(self.surfs.items()):
            if s.out_refused:
                det = ["%s x%d" % (errno_name(e), n) for e, n in s.refused_errno.most_common(4)]
                self.add("WARN", "dirty-refused", "%s: dirty mark refused %d time(s), copied out instead" % (self.sdesc(s), s.out_refused), det)
        kd = collections.Counter((r.path, r.a0) for r in tr.recs if r.kind == K_KREQ_DIRTY and r.path != 1)
        if kd:
            self.add("INFO", "kreq-dirty", "kernel dirty requests not accepted: " +
                     ", ".join("%s %s x%d" % (KD_NAMES.get(p, p), errno_name(e), n) for (p, e), n in kd.most_common(6)))
        # 3. empty publications. FAIL only when the same command buffer's pass on that surface DREW and the
        # copy-out hashed empty (the drawing was lost); a surface that was only cleared is expected to be empty.
        for sid, s in sorted(self.surfs.items()):
            if not s.empty_out:
                continue
            drawn_empty, cleared_empty = [], 0
            for r in s.events:
                if r.kind != K_SURF_OUT or not r.empty() or r.path not in (2, 3):
                    continue
                f = self.frames.get(r.id)
                ends = [p["end"] for p in f.passes.values() if p["end"] and p["end"].surf == sid] if f else []
                if any(e.path == 1 for e in ends):
                    drawn_empty.append(r)
                elif ends and all(e.path == 2 for e in ends):
                    cleared_empty += 1
            other = s.empty_out - len(drawn_empty) - cleared_empty
            if drawn_empty:
                self.add("FAIL", "empty-publish", "%s: %d copy-out(s) published an EMPTY picture although the same command buffer DREW into it "
                         "(all sampled pixels zero; first at +%.3fs, cb %d)" % (self.sdesc(s), len(drawn_empty), drawn_empty[0].t, drawn_empty[0].id))
            if other:
                self.add("WARN", "empty-publish", "%s: %d copy-out(s) published an EMPTY picture and the pass that wrote it is not in the trace" %
                         (self.sdesc(s), other))
            if cleared_empty:
                self.add("INFO", "empty-publish", "%s: %d of %d copy-out(s) published an empty picture after a clear-only pass (expected)" %
                         (self.sdesc(s), cleared_empty, s.out_copy))
        # 4. clear-only passes on display sized surfaces
        bw, bh = self.big
        for sid, s in sorted(self.surfs.items()):
            if s.clear_only and s.w >= bw and s.h >= bh:
                cols = ", ".join("(%g %g %g %g) x%d" % (c + (n,)) for c, n in s.clears.most_common(3))
                tb = sum(n for c, n in s.clears.items() if not any(c))
                self.add("WARN", "clear-only", "%s: %d of %d pass(es) only cleared it (%d to transparent black): %s" %
                         (self.sdesc(s), s.clear_only, s.frames, tb, cols))
        # 5. surface-in skipped while the VRAM may have been stale
        self._skips()
        # 6. Vulkan / allocation / kernel failures
        sub = [r for r in self.vk if r.kind == K_VK_SUBMIT and r.path != 1]
        if sub:
            c = collections.Counter((r.text, r.path, r.rc) for r in sub)
            self.add("FAIL", "vk-submit", "%d vkQueueSubmit failure(s)" % len(sub),
                     ["%s: %s %s x%d" % (t or "?", SUB_NAMES.get(p, p), vk_name(rc), n) for (t, p, rc), n in c.most_common(5)])
        to = [r for r in self.vk if r.kind == K_VK_WAIT and r.path != 1]
        if to:
            c = collections.Counter((r.text, r.path, r.rc) for r in to)
            worst = max(to, key=lambda r: r.dur_us)
            self.add("FAIL", "vk-wait", "%d fence wait(s) timed out or failed (worst %.0f ms at +%.3fs)" % (len(to), worst.dur_us / 1000.0, worst.t),
                     ["%s: %s %s x%d" % (t or "?", WAIT_NAMES.get(p, p), vk_name(rc), n) for (t, p, rc), n in c.most_common(5)])
        dis = [r for r in tr.recs if r.kind == K_VK_DISABLE]
        for r in dis:
            self.add("FAIL", "vk-disable", "Vulkan backend switched OFF at +%.3fs (%s, %s, in %s)" %
                     (r.t, DIS_NAMES.get(r.path, r.path), vk_name(r.rc), r.text or "?"))
        al = collections.Counter((r.text, r.rc) for r in tr.recs if r.kind == K_ALLOC_FAIL)
        # DRIVER-PLAN N43: a buffer texture whose bytesPerRow is not a multiple of 256 B is refused by RADV's linear
        # custom pitch (vkCreateImage -> -1000158000). A plugin from N43 on refuses it before Vulkan (no record at
        # all, the log's "buffer texture copy (pitch)" counts it); an older one still files an alloc-fail per call
        # (~32/s from WindowServer) - the cause is known and the private copy draws it, so INFO, not FAIL
        pitch = sum(n for (t, rc), n in al.items() if t == "vkCreateImage(buffer texture)" and rc == RC_DRM_PITCH)
        if pitch:
            al = collections.Counter({k: n for k, n in al.items()
                                      if not (k[0] == "vkCreateImage(buffer texture)" and k[1] == RC_DRM_PITCH)})
            self.add("INFO", "buftex-pitch", "%d buffer texture(s) refused by vkCreateImage with %s (bytesPerRow not a multiple of "
                     "256 B, RADV's linear custom pitch): a plugin older than DRIVER-PLAN N43; each was drawn from a private "
                     "copy (the ring holds the last events only, the plugin's log counts them all)" % (pitch, vk_name(RC_DRM_PITCH)))
        if al:
            self.add("FAIL", "alloc-fail", "%d allocation / import failure(s)" % sum(al.values()),
                     ["%s rc=%d x%d" % (t, rc, n) for (t, rc), n in al.most_common(6)])
        ki = [r for r in tr.recs if r.kind == K_KREQ_IMPORT and r.path != 1]
        if ki:
            c = collections.Counter((r.path, r.rc, r.a0) for r in ki)
            self.add("WARN", "kreq-import", "%d kernel surface import(s) refused (those surfaces stay private: copies)" % len(ki),
                     ["%s %s kernel %s x%d" % (KI_NAMES.get(p, p), vk_name(rc), errno_name(e), n) for (p, rc, e), n in c.most_common(5)])
        errs = [f for f in self.frames.values() if f.done is not None and f.done.rc]
        if errs:
            self.add("FAIL", "cb-error", "%d command buffer(s) finished with an error" % len(errs),
                     ["cb %d at +%.3fs: error code %d" % (f.cb, f.done.t, f.done.rc) for f in errs[:3]])
        if tr.problems:
            self.add("WARN", "trace-damaged", "; ".join(tr.problems))
        if tr.proc == "WindowServer" and tr.uid != 88:
            self.add("WARN", "ws-uid", "a trace named WindowServer written by uid %d, not _windowserver (88): another WindowServer "
                     "(root, a test) or a forged file - do not trust it" % tr.uid)

    def _skips(self):
        stale, foreign = [], []
        for sid, s in self.surfs.items():
            last_pub = None
            known = False
            for r in s.events:
                if r.kind == K_SURF_OUT:
                    last_pub = r
                    known = known or r.path in (1, 2, 3)
                elif r.kind == K_SURF_IN:
                    if r.path == 1:
                        known = True
                        last_pub = None
                    elif r.path == 2:
                        why = None
                        if last_pub is not None and last_pub.path > 3:
                            why = "the last surface-out failed (%s)" % OUT_NAMES.get(last_pub.path)
                        elif last_pub is not None and last_pub.empty():
                            why = "the last surface-out published an EMPTY picture"
                        elif last_pub is not None and last_pub.path == 3:
                            why = "the last surface-out was a copy after a refused dirty mark"
                        if why:
                            stale.append((s, r, why))
                        elif not known and self.tr.complete_history():
                            foreign.append((s, r))
        if stale:
            c = collections.Counter((s.sid, w) for s, _r, w in stale)
            self.add("WARN", "skip-stale", "%d surface-in skip(s) 'shared VRAM current' right after a doubtful publish" % len(stale),
                     ["%s: %s x%d (first at +%.3fs)" % (self.sdesc(self.surfs[sid]), w, n, next(r.t for s, r, ww in stale if s.sid == sid and ww == w))
                      for (sid, w), n in c.most_common(5)])
        if foreign:
            sids = sorted({s.sid for s, _r in foreign})
            self.add("INFO", "skip-foreign", "%d surface-in skip(s) rely on a VRAM tag this process never wrote (surfaces %s)" %
                     (len(foreign), ", ".join(map(str, sids[:8])) + (" ..." if len(sids) > 8 else "")))

    def worst(self):
        return min((SEVERITY[f[0]] for f in self.findings), default=3)


# ---------------------------------------------------------------------------------------------- output

def fmt_clear(c):
    return "(%g %g %g %g)" % tuple(round(x, 3) for x in c)


def describe(tr, r, off, prefix=True):
    """one line for one record"""
    pre = "+%9.3fs %s " % (r.t, tr.wall(r, off)) if prefix else ""
    k = r.kind
    geo = "%dx%d %s" % (r.w, r.h, pixfmt(r.fmt)) if r.w else ""
    hv = ""
    if r.flags & F_HASHED:
        hv = " hash=" + ("EMPTY" if r.empty() else "%08x/%08x/..%08x" % (r.bands[0], r.bands[1], r.bands[7])) + \
             ("(unlocked)" if r.flags & F_UNLOCKED else "")
    if k == K_SURF_IN:
        return pre + "surface-in  surf %d %s %s seed %d->%d%s %s%s%s" % (
            r.surf, IN_NAMES.get(r.path, r.path), geo, r.seed0, r.seed1, " vram" if r.flags & F_VRAM else "",
            "%d MB " % (r.bytes >> 20) if r.bytes else "", "%.1fms" % (r.dur_us / 1000.0) if r.dur_us else "", hv) + \
            (" rc=%d" % r.rc if r.rc else "")
    if k == K_SURF_OUT:
        return pre + "surface-out surf %d %s %s seed %d->%d%s%s%s %s%s" % (
            r.surf, OUT_NAMES.get(r.path, r.path), geo, r.seed0, r.seed1, " vram" if r.flags & F_VRAM else "",
            " ro-lock" if r.flags & F_RO else "", (" [%s]" % errno_name(r.rc)) if r.rc else "",
            "%.1fms" % (r.dur_us / 1000.0) if r.dur_us else "", hv)
    if k == K_PASS_BEGIN:
        return pre + "pass %d att%d/%d surf %d %s load=%s store=%s clear=%s%s" % (
            r.a0, r.a1, r.a2, r.surf, geo, LOAD.get(r.words[8], r.words[8]), STORE.get(r.words[9], r.words[9]), fmt_clear(r.clear()),
            " view" if r.flags & F_VIEW else "")
    if k == K_PASS_END:
        return pre + "pass %d end  surf %d %dx%d -> %s%s ~%d sets %.2fms" % (
            r.a0, r.surf, r.w, r.h, PASS_NAMES.get(r.path, r.path), (" clear=" + fmt_clear(r.clear())) if r.path == 2 else "", r.a1, r.dur_us / 1000.0)
    if k == K_CB_COMMIT:
        return pre + "commit cb %d (%d encoder(s), %d present(s), %d surface(s) written)" % (r.id, r.a0, r.a1, r.a2)
    if k == K_CB_DIRTY:
        return pre + "cb %d wrote surf %d %s%s" % (r.id, r.surf, geo, " vram" if r.flags & F_VRAM else "")
    if k == K_CB_DONE:
        return pre + "cb %d done %s after %.2f ms (%d part(s))" % (r.id, "ok" if not r.rc else "ERROR %d" % r.rc, r.dur_us / 1000.0, r.a1)
    if k == K_VK_SUBMIT:
        return pre + "vkQueueSubmit #%d %s %s %.2fms (%s)" % (r.id, SUB_NAMES.get(r.path, r.path), vk_name(r.rc), r.dur_us / 1000.0, r.text)
    if k == K_VK_WAIT:
        return pre + "fence wait #%d %s %s %.2fms (budget %d ms) (%s)" % (r.id, WAIT_NAMES.get(r.path, r.path), vk_name(r.rc), r.dur_us / 1000.0, r.a1, r.text)
    if k == K_VK_DISABLE:
        return pre + "VULKAN BACKEND OFF #%d %s %s (%s)" % (r.id, DIS_NAMES.get(r.path, r.path), vk_name(r.rc), r.text)
    if k == K_ALLOC_FAIL:
        return pre + "allocation failed: %s rc=%d" % (r.text, r.rc)
    if k == K_KREQ_PROBE:
        return pre + "kext surface BO probe: %s rc=%d %s" % ({1: "supported", 0: "not offered", 2: "retry"}.get(r.path, r.path), r.rc, errno_name(r.a0))
    if k == K_KREQ_DIRTY:
        return pre + "kernel dirty surf %d plane %d -> %s %s %.2fms" % (r.surf, r.plane, KD_NAMES.get(r.path, r.path), errno_name(r.a0), r.dur_us / 1000.0)
    if k == K_KREQ_IMPORT:
        return pre + "kernel surface import surf %d plane %d %d B -> %s %s kernel %s" % (
            r.surf, r.plane, r.bytes, KI_NAMES.get(r.path, r.path), vk_name(r.rc), errno_name(r.a0))
    if k == K_SURF_MAP:
        return pre + "surface map surf %d %s -> %s%s" % (
            r.surf, geo, "family VRAM" if r.path == 1 else "private image (%s)" % WHY_NAMES.get(r.a0, r.a0), " (VRAM already current)" if r.flags & F_CURRENT else "")
    if k == K_ENC_SPLIT:
        return pre + "%s" % enc_split_line(r)
    return pre + "%s" % KIND_NAMES.get(k, "kind %d" % k)


def enc_split_fields(r):
    """K_ENC_SPLIT (nvmtl_trace.h nvt_ev_enc_split): the encode time of one render encoder, split. All times in us."""
    return {"cb": r.id, "pass": r.seed0, "draws": r.a0, "builds": r.rc, "total_us": r.dur_us, "pipe_us": r.a1, "desc_us": r.a2,
            "sin_us": r.words[8], "sin_calls": r.words[9], "pipe_max_us": r.words[10], "desc_max_us": r.words[11],
            # N51 (zero in a trace of an older plugin): what the surface-in calls were
            "bc_up_n": r.words[0], "bc_up_us": r.words[1], "bc_skip_n": r.words[2], "bc_skip_us": r.words[3],
            "fast_n": r.words[4], "fast_us": r.words[5], "wait_us": r.words[6], "up_n": r.words[7]}


def enc_split_kinds(f):
    """'' for a trace without the N51 fields, else what the surface-in calls were: buffer-texture refreshes (uploaded / skipped), probe hits, uploads, monitor wait"""
    if not any(f[k] for k in ("bc_up_n", "bc_skip_n", "fast_n", "wait_us", "up_n")):
        return ""
    return "buffer-texture refresh %d uploaded %.2fms + %d unchanged %.2fms, %d probe hit(s) %.2fms, %d surface upload(s), monitor wait %.2fms" % (
        f["bc_up_n"], f["bc_up_us"] / 1000.0, f["bc_skip_n"], f["bc_skip_us"] / 1000.0, f["fast_n"], f["fast_us"] / 1000.0, f["up_n"], f["wait_us"] / 1000.0)


def enc_split_line(r):
    f = enc_split_fields(r)
    other = f["total_us"] - f["pipe_us"] - f["desc_us"] - f["sin_us"]
    return ("encoder cb %d%s: %d draw(s) in %.2fms = pipeline lookup %.2fms (worst draw %.0fus), descriptor writes %.2fms (worst %.0fus), "
            "surface-in checks %.2fms (%d call(s)), rest %.2fms%s" % (
                f["cb"], (" pass %d" % f["pass"]) if f["pass"] else "", f["draws"], f["total_us"] / 1000.0, f["pipe_us"] / 1000.0, f["pipe_max_us"],
                f["desc_us"] / 1000.0, f["desc_max_us"], f["sin_us"] / 1000.0, f["sin_calls"], other / 1000.0,
                (", %d pipeline variant(s) built on a draw" % f["builds"]) if f["builds"] else "") +
            ((" [" + enc_split_kinds(f) + "]") if enc_split_kinds(f) else ""))


def print_enc_split(tr, out):
    """a summary of the kind 17 records still in the ring: which of the three suspects the draws spend their encode time in"""
    rows = [enc_split_fields(r) for r in tr.recs if r.kind == K_ENC_SPLIT]
    if not rows:
        return
    tot = collections.Counter()
    for f in rows:
        for k in ("draws", "builds", "total_us", "pipe_us", "desc_us", "sin_us", "sin_calls", "bc_up_n", "bc_up_us", "bc_skip_n", "bc_skip_us",
                  "fast_n", "fast_us", "wait_us", "up_n"):
            tot[k] += f[k]
    d = max(tot["draws"], 1)
    out.write("\nencode-time split (%d render encoder(s), %d draw(s)): per draw pipeline lookup %.1fus, descriptor writes %.1fus, surface-in %.1fus "
              "(of %.1fus per draw in the encoders; the rest is the app's own time between calls)%s\n" % (
                  len(rows), tot["draws"], tot["pipe_us"] / d, tot["desc_us"] / d, tot["sin_us"] / d, tot["total_us"] / d,
                  ("; %d pipeline variant(s) built while drawing" % tot["builds"]) if tot["builds"] else ""))
    kinds = enc_split_kinds(tot)
    if not kinds:
        out.write("  (no N51 ENC_SPLIT data: the trace is from a plugin older than DRIVER-PLAN N51, or none of its %d surface-in call(s) was a refresh, a probe hit, an upload or a monitor wait - "
                  "what the surface-in time was cannot be split)\n" % tot["sin_calls"])
    if kinds:
        known = tot["bc_up_us"] + tot["bc_skip_us"] + tot["fast_us"] + tot["wait_us"]
        out.write("  surface-in calls (%d, %.1fms): %s; the rest of the surface-in time (%.1fms: surface uploads, tag queries, locked skips) is in neither\n" % (
            tot["sin_calls"], tot["sin_us"] / 1000.0, kinds, max(tot["sin_us"] - known, 0) / 1000.0))
    for f in sorted(rows, key=lambda f: -(f["pipe_us"] + f["desc_us"] + f["sin_us"]))[:3]:
        out.write("  worst encoder: cb %d, %d draw(s), pipeline %.2fms, descriptors %.2fms, surface-in %.2fms (%d call(s)), slowest draw: lookup %.0fus, descriptors %.0fus\n" % (
            f["cb"], f["draws"], f["pipe_us"] / 1000.0, f["desc_us"] / 1000.0, f["sin_us"] / 1000.0, f["sin_calls"], f["pipe_max_us"], f["desc_max_us"]))


def print_header(tr, an, out):
    out.write("=" * 100 + "\n")
    out.write("%s  pid %d  (%s, uid %d)\n" % (tr.proc, tr.pid, clean(tr.name), tr.uid))
    first = tr.recs[0].t if tr.recs else 0.0
    last = tr.recs[-1].t if tr.recs else 0.0
    out.write("trace opened %s UTC; events: %d written, %d kept (main ring %d, vk ring %d of %d); kept window +%.3fs .. +%.3fs%s\n" % (
        tr.opened(),
        tr.total_events, tr.kept, tr.widx_main, tr.widx_vk, tr.nmain, first, last,
        "" if tr.complete_history() else "  (main ring WRAPPED: older events only in the summary)"))
    t = tr.totals()
    if t:
        out.write("lifetime totals (per-second summary, %d s with activity): " % len(tr.sums) +
                  ", ".join("%s %d" % (k, t[k]) for k in ("passes_surf", "clear_only", "clear_only_big", "cb_commit", "in_upload", "in_skip",
                                                          "in_synced", "out_dirty", "out_copy", "out_refused", "out_fail", "vk_submit",
                                                          "vk_submit_err", "vk_wait_timeout", "vk_wait_err", "vk_disable", "alloc_fail", "kreq_err")
                            if t[k]) + "\n")


def print_surfaces(an, out):
    rows = [s for s in an.surfs.values() if s.frames or s.published or s.in_upload or s.in_skip or s.dirtied]
    if not rows:
        out.write("\nno surface activity recorded\n")
        return
    rows.sort(key=lambda s: (-(s.w * s.h >= an.big[0] * an.big[1]), -(s.frames + s.published), s.sid))
    out.write("\nper IOSurface (big ones first):\n")
    out.write("  %-8s %-11s %-7s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n" % (
        "surface", "size", "backing", "frames", "drawn", "clear", "o:dirt", "o:copy", "o:refu", "o:fail", "EMPTY", "i:up", "i:skip"))
    for s in rows[:40]:
        out.write("  %-8d %-11s %-7s %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d\n" % (
            s.sid, "%dx%d" % (s.w, s.h), "-" if s.vram is None else ("vram" if s.vram else "private"), s.frames, s.drawn, s.clear_only,
            s.out_dirty, s.out_copy, s.out_refused, s.out_fail, s.empty_out, s.in_upload, s.in_skip))
    if len(rows) > 40:
        out.write("  ... %d more surface(s)\n" % (len(rows) - 40))


def print_findings(an, out):
    out.write("\nfindings:\n")
    if not an.findings:
        out.write("  none\n")
    for level, code, text, detail in sorted(an.findings, key=lambda f: SEVERITY[f[0]]):
        out.write("  %-4s %-15s %s\n" % (level, code, text))
        for d in detail:
            out.write("       %s\n" % d)


def print_frames(tr, an, n, off, out):
    frames = [f for f in an.frames.values() if f.commit or f.passes]
    if n is not None:
        frames = frames[-n:]
    if not frames:
        return
    out.write("\nframes (command buffers), oldest first - which surface each rendered into and what was done with it:\n")
    for f in frames:
        c = f.commit or f.first
        out.write("cb %-6d +%.6fs %s  tid %#x  %s\n" % (f.cb, c.t, tr.wall(c, off), c.tid,
                  ("done %s in %.2f ms" % ("ok" if not f.done.rc else "ERROR %d" % f.done.rc, f.done.dur_us / 1000.0)) if f.done else "NOT FINISHED in the trace"))
        for pid, p in f.passes.items():
            for b in p["begin"]:
                out.write("    pass %d att%d  surf %-5d %dx%d %s load=%s store=%s clear=%s\n" % (
                    pid, b.a1, b.surf, b.w, b.h, pixfmt(b.fmt), LOAD.get(b.words[8], b.words[8]), STORE.get(b.words[9], b.words[9]), fmt_clear(b.clear())))
            if p["end"]:
                e = p["end"]
                out.write("      -> %s%s (~%d sets, %.2f ms)\n" % (PASS_NAMES.get(e.path, e.path), (" to " + fmt_clear(e.clear())) if e.path == 2 else "", e.a1, e.dur_us / 1000.0))
        for d in f.dirty:
            out.write("    wrote surf %d\n" % d.surf)
        for r in f.ins + f.outs:
            out.write("    " + describe(tr, r, off, False) + "\n")
        pubs = {o.surf for o in f.outs if o.path in (1, 2, 3)}
        for d in f.dirty:
            if d.surf not in pubs and f.done is not None:
                out.write("    !! surf %d was written but never published\n" % d.surf)


def print_surface_timeline(tr, an, sid, off, out):
    s = an.surfs.get(sid)
    if not s:
        out.write("\nsurface %d: no events\n" % sid)
        return
    out.write("\ntimeline of %s:\n" % an.sdesc(s))
    for r in s.events:
        out.write("  " + describe(tr, r, off) + "\n")


def print_failures(tr, off, out):
    ev = [r for r in tr.recs if (r.kind in (K_VK_SUBMIT, K_VK_WAIT) and r.path != 1) or r.kind in (K_VK_DISABLE, K_ALLOC_FAIL)
          or (r.kind == K_KREQ_IMPORT and r.path != 1) or (r.kind == K_KREQ_PROBE) or (r.kind == K_CB_DONE and r.rc)]
    if ev:
        out.write("\nfailure events:\n")
        for r in ev:
            out.write("  " + describe(tr, r, off) + "\n")


def print_rates(tr, out):
    if not tr.sums:
        return
    cols = ["passes_surf", "clear_only", "cb_commit", "in_upload", "in_skip", "in_synced", "out_dirty", "out_copy", "out_refused",
            "out_fail", "vk_submit", "vk_submit_err", "vk_wait_timeout"]
    out.write("\nper second (counts within that second of process life):\n  %6s " % "sec" + " ".join("%9s" % c[:9] for c in cols) + "\n")
    prev = None
    for sec, _first, c in tr.sums:
        if prev is not None and sec > prev + 1:
            out.write("  ... %d idle second(s)\n" % (sec - prev - 1))
        out.write("  %6d " % sec + " ".join("%9d" % c.get(k, 0) for k in cols) + "\n")
        prev = sec


def surface_summary(an):
    """{IOSurface id: what this process's trace says of it} (tools/rdna3_trace.py's join)"""
    out = {}
    for sid, sf in an.surfs.items():
        if not sid:
            continue
        bits = []
        if sf.drawn:
            bits.append("DRAWN %d" % sf.drawn)
        if sf.clear_only:
            bits.append("CLEAR-ONLY %d" % sf.clear_only)
        if sf.other_pass:
            bits.append("other passes %d" % sf.other_pass)
        if not sf.frames:
            bits.append("no pass")
        pub = "published %d (dirty %d, copy-out %d" % (sf.published, sf.out_dirty, sf.out_copy)
        if sf.empty_out:
            pub += ", %d EMPTY" % sf.empty_out
        pub += ")"
        if not sf.published and sf.dirtied:
            pub = "NEVER published (written by %d command buffer(s))" % sf.dirtied
        elif not sf.published:
            pub = "never published"
        bits.append(pub)
        if sf.out_fail:
            bits.append("%d copy-out FAILURE(s)" % sf.out_fail)
        if sf.in_upload or sf.in_skip:
            bits.append("surface-in %d upload / %d skip" % (sf.in_upload, sf.in_skip))
        out[sid] = ", ".join(bits)
    return out


def triage(path, big=(1024, 600), ab_window=None, boottime=None):
    """(lines, surfaces): triage_ex() without its coverage facts"""
    r = triage_ex(path, big, ab_window, boottime)
    return r["lines"], r["surfaces"]


def triage_ex(path, big=(1024, 600), ab_window=None, boottime=None):
    """(lines, surfaces): [(level, text)] for tools/triage_reports.py - the findings of a report's traces,
    WindowServer first, each with when its trace was opened, the time the main ring still covers, and a
    WARN when it wrapped (and does not cover the A/B: ab_window = (start, end) wall seconds) - and
    {IOSurface id: summary} of WindowServer's surfaces for the join with the kernel's trace"""
    traces, problems = load_paths([path], boottime)
    out = []
    surfaces = {}
    ws_found = False
    covers = None    # WindowServer's kept records reach back to the A/B's start (None: no A/B window known)
    for p in problems:
        out.append(("WARN" if "OLD -" in p else "INFO", "plugin trace: %s" % p))
    for tr in traces:
        an = Analysis(tr, big)
        ws = tr.proc == "WindowServer"
        tag = "plugin trace %s[%d] (opened %s UTC)" % (tr.proc, tr.pid, tr.opened())
        for level, code, text, detail in an.findings:
            if level == "INFO" and not ws:
                continue
            if not ws and level == "WARN":
                continue   # an app's refused dirty marks are not the desktop's
            out.append((level, "%s %s: %s%s" % (tag, code, text, ("  [" + "; ".join(detail[:2]) + "]") if detail else "")))
        if not ws:
            continue
        surfaces.update(surface_summary(an))
        ws_found = True
        t = tr.totals()
        win = tr.kept_window()
        complete = tr.complete_history()
        if ab_window and ab_window[0]:
            c = complete or bool(win and win[0] <= ab_window[0])
            covers = c if covers is None else (covers or c)
        merged = getattr(tr, "merged", 1)
        out.append(("INFO", "%s: %d frames in the ring, %d passes on surfaces (%d clear-only), %d dirty / %d copy-out "
                    "(%d after a refused dirty mark) / %d skipped uploads; main ring %d records%s; kept %s; decode: "
                    "tools/plugin_trace.py" %
                    (tag, len([f for f in an.frames.values() if f.commit]), t["passes_surf"], t["clear_only"], t["out_dirty"],
                     t["out_copy"], t["out_refused"], t["in_skip"], tr.nmain,
                     " (%d copies merged)" % merged if merged > 1 else "",
                     "%s .. %s UTC (%.0f s)" % (utc(win[0]), utc(win[1])[11:], win[1] - win[0]) if win else "nothing")))
        if not complete:
            cov, lvl = "", "WARN"
            if ab_window and ab_window[0] and win:
                if win[0] > ab_window[0]:
                    cov = " - the A/B (from %s UTC) is NOT covered: it starts before the oldest kept event" % utc(ab_window[0])
                else:
                    cov = " - the A/B (from %s UTC) is covered%s" % (utc(ab_window[0]), " by the snapshots" if merged > 1 else "")
                    lvl = "INFO"
            out.append((lvl, "%s: main ring WRAPPED (%d events written, %d kept, ring %d): only %s .. %s UTC kept%s "
                        "(NVMTL_TRACE_RING in rdna3mtl.conf enlarges it from the next WindowServer start)" %
                        (tag, tr.widx_main, tr.kept, tr.nmain, utc(win[0]) if win else "?",
                         utc(win[1])[11:] if win else "?", cov)))
    return {"lines": out, "surfaces": surfaces, "ws": ws_found, "covers_ab": covers}


def triage_lines(path, big=(1024, 600), ab_window=None, boottime=None):
    """[(level, text)] for tools/triage_reports.py (triage() without the surfaces)"""
    return triage(path, big, ab_window, boottime)[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description="decode the Metal plugin's frame trace (see the module docstring)")
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--proc")
    ap.add_argument("--surface", type=int, action="append")
    ap.add_argument("--frames", type=int, default=12)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--rates", action="store_true")
    ap.add_argument("--big", default="1024x600")
    ap.add_argument("--utc-offset", type=float, default=0.0)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--triage", action="store_true")
    a = ap.parse_args(argv)
    try:
        bw, bh = (int(x) for x in a.big.lower().split("x"))
    except ValueError:
        ap.error("--big wants WxH")
    if a.triage:
        for lvl, text in triage_lines(a.paths[0], (bw, bh)):
            print("%s %s" % (lvl, text))
        return 0
    traces, problems = load_paths(a.paths)
    out = sys.stdout
    for p in problems:
        out.write("!! %s\n" % p)
    if a.proc:
        traces = [t for t in traces if a.proc.lower() in t.proc.lower()]
    if not traces:
        out.write("no plugin traces found\n")
        return 1
    failed = False
    for tr in traces:
        an = Analysis(tr, (bw, bh))
        print_header(tr, an, out)
        print_surfaces(an, out)
        print_enc_split(tr, out)
        print_findings(an, out)
        for sid in a.surface or []:
            print_surface_timeline(tr, an, sid, a.utc_offset, out)
        if a.rates:
            print_rates(tr, out)
        if a.all:
            print_failures(tr, a.utc_offset, out)
        print_frames(tr, an, None if a.all else a.frames, a.utc_offset, out)
        failed = failed or any(f[0] == "FAIL" for f in an.findings)
    return 1 if (a.check and failed) else 0


if __name__ == "__main__":
    sys.exit(main())
