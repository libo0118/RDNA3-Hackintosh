"""`rdna3vm result` speed (2026-10-26c: 64 s of triage, 115 s of journalctl, 25 s of xz for one card report): the triage's
fast paths must give the answers the slow ones gave.
 - tools/triage_tar.py: open_tar() gives the same members, sizes and bytes as tarfile, reads an archive once (a changed
   file is read again), and a member above CACHE_MAX is still readable (the slow way);
 - tools/triage_par.py: pmap() equals the serial map in the same order, with 1 job, with several, when the pool cannot
   start (serial fallback), and an exception of the function is raised;
 - tools/triage_glyphs.py glyph_verdicts() on synthetic looks: the same verdicts for 1 job and for 4 (RDNA3_TRIAGE_JOBS);
 - tools/lookimg.py --to-png: the same PNGs for 1 job and for 4;
 - tools/triage_reports.py on a synthetic report (an archive and its looks): the whole output is byte for byte the same
   for 1 job and for 4;
 - tools/vm/queue_journal.py: keeps the lines of the rdna3vm-queue-* units (the unit's own and systemd's about it) in the
   short-iso / short shape, skips what is not JSON.
Run: python3 -I -B tests/test_triage_parallel.py"""
import hashlib
import io
import json
import os
import random
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_par  # noqa: E402
import triage_tar  # noqa: E402
import triage_glyphs as tg  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


td = tempfile.mkdtemp(prefix="triagepar")

# ---- triage_tar
arc = os.path.join(td, "a.tar.gz")
members = {"top/01-boot-args.txt": b"rdna3lvl=2 rdna3accel=1\n", "top/31-plugin-trace/info.txt": b"boottime 5\n", "top/big.bin": os.urandom(5000),
           "top/31-plugin-trace/W-1.trace": b"T" * 3000, "top/empty.txt": b""}
with tarfile.open(arc, "w:gz") as t:
    d = tarfile.TarInfo("top/")
    d.type = tarfile.DIRTYPE
    t.addfile(d)
    for n, b in members.items():
        ti = tarfile.TarInfo(n)
        ti.size = len(b)
        t.addfile(ti, io.BytesIO(b))
with tarfile.open(arc) as ref:
    want = [(m.name, m.size, m.isfile()) for m in ref.getmembers()]
with triage_tar.open_tar(arc) as ct:
    got = [(m.name, m.size, m.isfile()) for m in ct.getmembers()]
    check(got == want and [(m.name, m.size, m.isfile()) for m in ct] == want, "open_tar: the same members, %r vs %r" % (got, want))
    for m in ct.getmembers():
        if m.isfile():
            check(ct.extractfile(m).read() == members[m.name], "open_tar: the bytes of %s" % m.name)
check(triage_tar.open_tar(arc) is triage_tar.open_tar(arc), "open_tar: one read per file")
triage_tar.CACHE_MAX = 4000     # big.bin (5000) is no longer kept: read the slow way, the same bytes
ct = triage_tar.CachedTar(arc)
check("top/big.bin" not in ct.data and "top/01-boot-args.txt" in ct.data and "top/31-plugin-trace/W-1.trace" not in ct.data, "open_tar: what is kept, %r" % (sorted(ct.data),))
check(ct.extractfile([m for m in ct.getmembers() if m.name.endswith("big.bin")][0]).read() == members["top/big.bin"], "open_tar: an unkept member is still read")
triage_tar.CACHE_MAX = 48 << 20
# a changed file is read again (same path, other content)
with tarfile.open(arc, "w:gz") as t:
    ti = tarfile.TarInfo("top/01-boot-args.txt")
    ti.size = 3
    t.addfile(ti, io.BytesIO(b"new"))
check(triage_tar.open_tar(arc).extractfile("top/01-boot-args.txt").read() == b"new", "open_tar: a changed archive is read again")
# what tarfile.open raised, open_tar raises
bad = os.path.join(td, "bad.tar.gz")
open(bad, "wb").write(b"not an archive at all")
try:
    triage_tar.open_tar(bad)
    check(False, "open_tar: a bad archive must raise")
except (OSError, tarfile.TarError):
    pass
try:
    triage_tar.open_tar(os.path.join(td, "missing.tar.gz"))
    check(False, "open_tar: a missing archive must raise")
except OSError:
    pass


# ---- triage_par
def sq(x):
    return (x, x * x)


def boom(x):
    if x == 3:
        raise KeyError("boom")
    return x


items = list(range(40))
serial = [sq(x) for x in items]
os.environ["RDNA3_TRIAGE_JOBS"] = "1"
check(triage_par.jobs() == 1 and triage_par.pmap(sq, items) == serial, "pmap: 1 job")
os.environ["RDNA3_TRIAGE_JOBS"] = "4"
check(triage_par.jobs() == 4 and triage_par.pmap(sq, items) == serial, "pmap: 4 jobs, the same order")
check(triage_par.pmap(sq, []) == [] and triage_par.pmap(sq, [7]) == [(7, 49)], "pmap: nothing, one item")
os.environ["RDNA3_TRIAGE_JOBS"] = "junk"
check(triage_par.jobs() >= 1, "pmap: a bad RDNA3_TRIAGE_JOBS is the CPU count")
os.environ["RDNA3_TRIAGE_JOBS"] = "4"
try:
    triage_par.pmap(boom, items)
    check(False, "pmap: the function's exception must reach the caller")
except KeyError:
    pass
real = triage_par.multiprocessing.get_context


def nofork(_m):
    raise OSError("no fork here")


triage_par.multiprocessing.get_context = nofork
check(triage_par.pmap(sq, items) == serial, "pmap: no pool = serial, the same answer")
triage_par.multiprocessing.get_context = real

# ---- synthetic looks (the way tests/test_glyph_verdict.py draws them: a noisy wallpaper and a text window of tick lines)
CELL_W, CELL_H, PITCH = 7, 10, 14
BG = (28, 28, 28)
_FONT = {}


def glyph(ch):
    if ch not in _FONT:
        r = random.Random(ord(ch) * 7919)
        _FONT[ch] = [[1 if r.random() < 0.45 else 0 for _ in range(6)] for _ in range(CELL_H)] if ch != " " else [[0] * 6 for _ in range(CELL_H)]
    return _FONT[ch]


def look(x0, y0, nlines, broken, seed, w=1280, h=720, cols=80, rows=24):
    r = random.Random(seed)
    base = bytes(r.randrange(40, 200) for _ in range(w * 3))
    pix = bytearray()
    for y in range(h):
        k = (y * 37) % w
        pix += bytes((c * (180 + (y % 7)) // 255) for c in base[k * 3:] + base[:k * 3])
    texts = [" 13:58:%02d  rdna3 ladder tick  abcdefghijklmnopqrstuvwxyz" % (42 + k) for k in range(nlines)]
    hit = [{c: [[1 if r.random() < 0.4 else 0 for _ in range(6)] for _ in range(CELL_H)] for c in range(37, len(t)) if r.random() < 0.8} if broken else {} for t in texts]
    ww, wh = cols * CELL_W + 12, rows * PITCH + 12
    for yo in range(h):
        sy = int(yo * 1.5)
        ly = sy - y0
        if not 0 <= ly < wh:
            continue
        k, ry = divmod(ly - 6, PITCH)
        for xo in range(w):
            lx = int(xo * 1.5) - x0
            if not 0 <= lx < ww:
                continue
            col = BG
            c, rx = divmod(lx - 6, CELL_W)
            if ly >= 6 and k < nlines and ry < CELL_H and lx >= 6 and c < len(texts[k]) and rx < 6:
                bits = hit[k].get(c)
                col = (230, 120, 40) if (bits[ry][rx] if bits is not None else glyph(texts[k][c])[ry][rx]) else BG
            o = (yo * w + xo) * 3
            pix[o:o + 3] = bytes(col)
    return b"P6\n%d %d\n255\n" % (w, h) + bytes(pix)


clean, broken = look(1100, 330, 22, False, 3), look(1100, 330, 22, True, 4)
rep = os.path.join(td, "rep")
os.makedirs(rep)
prefix = os.path.join(rep, "rdna3-report-test-1")
rungs = {"baseline": broken, "radv": clean, "sync": clean, "paranoid": broken, "noise": b"P6\n3 3\n255\n" + bytes(27)}
for n, data in rungs.items():
    for pipe in (0, 1):
        open("%s-scanoutladder-%sh-pipe%d.ppm" % (prefix, n, pipe), "wb").write(data)
rg = {n: {"looks": ["ladder-%sa" % n, "ladder-%sh" % n]} for n in rungs}
rg["missing"] = {"looks": ["ladder-missinga", "ladder-missingh"]}   # no file: a `none` with the reason
rg["nolook"] = {"looks": []}                                       # left out

os.environ["RDNA3_TRIAGE_JOBS"] = "1"
g1 = tg.glyph_verdicts(rg, prefix)
os.environ["RDNA3_TRIAGE_JOBS"] = "4"
g4 = tg.glyph_verdicts(rg, prefix)
check(g1 == g4 and list(g1) == list(g4), "glyph_verdicts: 4 jobs = 1 job")
check(list(g1) == ["baseline", "radv", "sync", "paranoid", "noise", "missing"], "glyph_verdicts: the order of the rungs, %r" % (list(g1),))
check(g1["baseline"]["verdict"] == "broken" and g1["radv"]["verdict"] == "clean", "glyph_verdicts: the synthetic verdicts are what they should be: %r / %r" % (
    g1["baseline"]["text"], g1["radv"]["text"]))
check(g1["noise"]["verdict"] == "none" and g1["missing"]["verdict"] == "none" and "NOT judged" in g1["missing"]["text"], "glyph_verdicts: none with a reason")

# ---- lookimg --to-png: the same PNGs, 1 job and 4
res = {}
for jobs in ("1", "4"):
    d = os.path.join(td, "png" + jobs)
    os.makedirs(d)
    for n in ("a", "b", "c"):
        open(os.path.join(d, "x-%s.ppm" % n), "wb").write(clean if n != "b" else broken)
    open(os.path.join(d, "x-bad.ppm"), "wb").write(b"P6\n3 3\n255\nxx")   # not a picture: stays, named
    p = subprocess.run([sys.executable, "-I", "-B", os.path.join(ROOT, "tools", "lookimg.py"), "--to-png", d], capture_output=True, text=True,
                       env=dict(os.environ, RDNA3_TRIAGE_JOBS=jobs))
    res[jobs] = (p.stdout, sorted((f, hashlib.sha1(open(os.path.join(d, f), "rb").read()).hexdigest()) for f in os.listdir(d)))
check(res["1"] == res["4"], "lookimg --to-png: 4 jobs = 1 job: %r vs %r" % (res["1"][0], res["4"][0]))
check("3 PPM -> PNG" in res["1"][0] and "WARN look not converted: x-bad.ppm" in res["1"][0] and [f for f, _ in res["1"][1]] == ["x-a.png", "x-b.png", "x-bad.ppm", "x-c.png"],
      "lookimg --to-png: the answer, %r" % (res["1"],))

# ---- the whole triage on a synthetic report: byte for byte the same for 1 job and 4
LT = """# rdna3 ladder v1
variant: 2 (ladder-ws)
started: 12:00:00
%s"""
rungs_txt = "".join("== rung %s\nscope: apps\nconf: (nothing)\npids: Terminal=101 Chess=102\nlooks: ladder-%sa ladder-%sh\nanswer: n\n== end rung %s\n" % (n, n, n, n) for n in rungs)
files = {"01-boot-args.txt": "rdna3lvl=2 rdna3accel=1\n", "03-kexts.txt": "RDNA3Accelerator 1.0 loaded\n", "41-ladder.txt": LT % rungs_txt, "20-log-rdna3.txt": "RDNA3Accelerator: build deadbeef00\n"}
with tarfile.open(prefix + ".tar.gz", "w:gz") as t:
    for n, body in files.items():
        data = body.encode()
        ti = tarfile.TarInfo("rdna3-report-AbC/" + n)
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
open(prefix + "-tests.txt", "w").write("== tests\n")
outs = {}
for jobs in ("1", "4"):
    p = subprocess.run([sys.executable, "-I", "-B", os.path.join(ROOT, "tools", "triage_reports.py"), rep], capture_output=True, text=True,
                       env=dict(os.environ, RDNA3_TRIAGE_JOBS=jobs, RDNA3_NO_PICTURES="1"))
    outs[jobs] = (p.returncode, p.stdout, p.stderr)
check(outs["1"] == outs["4"], "triage_reports: 4 jobs = 1 job (byte for byte)")
check("BROKEN" in outs["1"][1] and "tick lines have a twin" in outs["1"][1], "triage_reports: the glyph verdicts reach the output: %r" % (outs["1"][1][-600:],))

# ---- queue_journal.py
j = [{"__REALTIME_TIMESTAMP": "1793014169495945", "_HOSTNAME": "h", "SYSLOG_IDENTIFIER": "rdna3vm", "_PID": "12", "MESSAGE": "== queue: step 5", "_SYSTEMD_UNIT": "rdna3vm-queue-101010.service"},
     {"__REALTIME_TIMESTAMP": "1793014169495945", "_HOSTNAME": "h", "_COMM": "systemd", "_PID": "1", "MESSAGE": "Started rdna3vm-queue-101010.service.", "UNIT": "rdna3vm-queue-101010.service", "_SYSTEMD_UNIT": "init.scope"},
     {"__REALTIME_TIMESTAMP": "1793014169495945", "_HOSTNAME": "h", "_COMM": "bash", "MESSAGE": [104, 105], "_SYSTEMD_UNIT": "rdna3vm-queue-1.service"},
     {"__REALTIME_TIMESTAMP": "1793014169495945", "_HOSTNAME": "h", "SYSLOG_IDENTIFIER": "cron", "MESSAGE": "unrelated", "_SYSTEMD_UNIT": "cron.service"}]
inp = "\n".join(json.dumps(x) for x in j[:2]) + "\nnot json\n" + json.dumps(j[2]) + "\n" + json.dumps(j[3]) + "\n{}\n"
qj = os.path.join(ROOT, "tools", "vm", "queue_journal.py")
iso = subprocess.run([sys.executable, "-I", "-B", qj], input=inp, capture_output=True, text=True).stdout.splitlines()
short = subprocess.run([sys.executable, "-I", "-B", qj, "--short"], input=inp, capture_output=True, text=True).stdout.splitlines()
check(len(iso) == 3 and iso[0].endswith(" h rdna3vm[12]: == queue: step 5") and iso[1].endswith(" h systemd[1]: Started rdna3vm-queue-101010.service.") and iso[2].endswith(" h bash: hi"),
      "queue_journal: the runner's lines only, %r" % (iso,))
check(len(short) == 3 and "T" not in short[0].split(" h ")[0] and iso[0][:4] == "2026", "queue_journal: --short, %r / %r" % (short, iso))

if fails:
    for f in fails:
        print("test_triage_parallel: FAIL " + f)
    sys.exit(1)
print("test_triage_parallel: ok")
