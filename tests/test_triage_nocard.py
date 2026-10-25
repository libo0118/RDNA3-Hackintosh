"""Card run 2026-10-25c (the first on macOS 26.4.1, all our kexts loaded): tools/triage_reports.py and
tools/macos/collect.sh.
 - no_card_boot(): a report whose 04-pci.txt lists devices but no AMD 1002:747e, from a boot whose boot-args do not ask
   for the driver (the VM's default profile), is a "no-card boot": its rows print as INFO (not FAIL / WARN), the
   summary's rows too, and it does not fail the run. A boot that asked for the driver (rdna3...=) and lost the card is
   NOT demoted; no usable PCI dump, no archive: not judged.
 - gpu_vendor_rows(): no 'GetSystemGPUVendors_block_invoke' line is a WARN, INFO when planes arrive in
   IOPresentment transactions (the line is Info level and ages out of the log buffer).
 - crash_rows(): "app crashes" from collect.sh's 40-app-crashes.txt: INFO for none / an older report, WARN per crashed
   process (exception + top frame + count), WARN for an unfinished listing or an unreadable report, INFO on a no-card boot.
 - collect.sh crash_reports(): *.ips newer than the boot, newest first, copies capped, summaries from osascript (JXA)
   or the grep fallback, "crashes: N"; the JXA program itself (run under node with a stand-in for $.NSString, and under
   the real osascript on a Mac); its output feeds crash_rows.
Run: python3 -I -B tests/test_triage_nocard.py"""
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_reports as tr  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def levels(rows):
    return [r[0] for r in rows]


def archive(path, members):
    with tarfile.open(path, "w:gz") as t:
        for n, body in members.items():
            data = body.encode()
            ti = tarfile.TarInfo("rdna3-report/" + n)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    return path


PCI_NOCARD = """$ sh -c ioreg ...
+-o S00@0  <class IOPCIDevice, id 0x1000001a1, registered, matched, active, busy 0 (0 ms), retain 9>
      "class-code" = <00000600>
      "vendor-id" = <86800000>
      "device-id" = <c0290000>
+-o S07@0  <class IOPCIDevice, id 0x1000001a2, registered, matched, active, busy 0 (0 ms), retain 9>
  |   "vendor-id" = <ad150000>
  |   "device-id" = <b0070000>

== AMD devices
[exit 0]
"""


def pci_card(device="7e740000"):
    return PCI_NOCARD.replace("== AMD devices\n", """+-o S80@10  <class IOPCIDevice, id 0x1000001a9, registered, matched, active, busy 0 (0 ms), retain 20>
  |   "vendor-id" = <02100000>
  |   "device-id" = <%s>

== AMD devices
""" % device)


SLEEP = """== rdna3_drminfo --scanout (look sleep)
[exit 1]
== rdna3_drminfo --scanout (look wake)
[exit 1]
"""
# one tests-file with display looks that FAIL when judged: in a no-card boot they are INFO
TESTS = "boot-args: %s\n== rdna3_drminfo --dma-test\n[exit 1]\n" + SLEEP

IPS_BLOCKS = """boot: 1000
== crash: /Users/kevin/Library/Logs/DiagnosticReports/Chess-2026-10-25-043000.ips
mtime: 5300 (4300 s after the boot)
size: 537 bytes
copy: 40-crash-ips/1-Chess-2026-10-25-043000.ips
process: Chess
pid: 412
exception: EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x0000000000000010
termination: {"namespace":"SIGNAL","indicator":"Segmentation fault: 11"}
crashed thread: 0 (com.apple.main-thread)
frame 0: libobjc.A.dylib  objc_msgSend + 32
frame 1: Chess  0x1234
== crash: /Users/kevin/Library/Logs/DiagnosticReports/Chess-2026-10-25-042000.ips
mtime: 5200 (4200 s after the boot)
size: 537 bytes
copy: 40-crash-ips/2-Chess-2026-10-25-042000.ips
process: Chess
pid: 380
exception: EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x0000000000000008
frame 0: RDNA3MTLDriver  NVMTLDevice_newBuffer + 100
== crash: /Library/Logs/DiagnosticReports/Maps-2026-10-25-041000.ips
mtime: 5100 (4100 s after the boot)
size: 900 bytes
copy: 40-crash-ips/3-Maps-2026-10-25-041000.ips
process: Maps
pid: 301
exception: EXC_CRASH (SIGABRT)
termination: {"namespace":"SIGNAL","indicator":"Abort trap: 6"}
crashed thread: 3
frame 0: libsystem_kernel.dylib  __pthread_kill + 8
crashes: 3
"""

# ---- no_card_boot
with tempfile.TemporaryDirectory() as d:
    p = archive(os.path.join(d, "a.tar.gz"), {"04-pci.txt": PCI_NOCARD, "01-boot-args.txt": "$ nvram boot-args\nboot-args\tkeepsyms=1\n[exit 0]\n"})
    r = tr.no_card_boot(p)
    check(r and "1002:747e" in r, "no-card boot: %r" % r)
    p = archive(os.path.join(d, "b.tar.gz"), {"04-pci.txt": pci_card(), "01-boot-args.txt": "boot-args\tkeepsyms=1\n"})
    check(tr.no_card_boot(p) is None, "the card is there")
    p = archive(os.path.join(d, "c.tar.gz"), {"04-pci.txt": pci_card("30ab0000"), "01-boot-args.txt": "boot-args\tkeepsyms=1\n"})
    check(tr.no_card_boot(p), "another AMD device (the audio function) is not the card")
    p = archive(os.path.join(d, "d.tar.gz"), {"04-pci.txt": PCI_NOCARD, "01-boot-args.txt": "boot-args\trdna3lvl=8 rdna3fb=1\n"})
    check(tr.no_card_boot(p) is None, "a boot that asked for the driver and lost the card is a failure, not a no-card boot")
    p = archive(os.path.join(d, "e.tar.gz"), {"04-pci.txt": "$ sh -c ioreg\n[exit 1]\n", "01-boot-args.txt": "keepsyms=1\n"})
    check(tr.no_card_boot(p) is None, "no PCI dump: not judged")
    check(tr.no_card_boot(os.path.join(d, "none.tar.gz")) is None, "no archive: not judged")
    check(tr.demote_nocard("   FAIL a\n  WARN b\nCRITICAL c\n   PASS d\n  x FAIL y\n") == "   INFO a\n  INFO b\nINFO c\n   PASS d\n  x FAIL y\n",
          "demote_nocard: %r" % tr.demote_nocard("   FAIL a\n  WARN b\n"))

# ---- the whole triage on a no-card result and on the same with the driver asked for
KEXTS = "$ kmutil\n  121 1 0xffffff7f95433000 com.apple.iokit.IONDRVSupport (600)\n[exit 0]\n"


def triage(d):
    res = subprocess.run([sys.executable, "-I", "-B", os.path.join(ROOT, "tools", "triage_reports.py"), d],
                         capture_output=True, text=True)
    return res.returncode, res.stdout + res.stderr


def result_dir(d, args, pci, extra=None):
    members = {"01-boot-args.txt": "$ nvram boot-args\nboot-args\t%s\n[exit 0]\n" % args, "03-kexts.txt": KEXTS, "04-pci.txt": pci,
               "24-log-coredisplay.txt": "$ log show\n2026-10-25 02:40:00.000 I WindowServer[1:2] something else\n[exit 0]\n"}
    members.update(extra or {})
    archive(os.path.join(d, "rdna3-report-default-profile-20261025-023935.tar.gz"), members)
    with open(os.path.join(d, "rdna3-report-default-profile-20261025-023935-tests.txt"), "w") as f:
        f.write(TESTS % args)


with tempfile.TemporaryDirectory() as d:
    result_dir(d, "keepsyms=1", PCI_NOCARD, {tr.CRASH_MEMBER: IPS_BLOCKS})
    rc, out = triage(d)
    check(rc == 0, "no-card result: exit %d\n%s" % (rc, out))
    check("RESULT: every reached check passed" in out, "no-card result: RESULT")
    check("INFO no-card boot: no AMD 1002:747e device in 04-pci.txt" in out, "no-card label")
    check(not re.search(r"(?m)^\s*(FAIL|WARN|CRITICAL)\b", out), "a FAIL / WARN row of a no-card boot: %s" % re.findall(r"(?m)^\s*(?:FAIL|WARN|CRITICAL).*$", out)[:3])
    check("INFO display sleep:" in out or "INFO display wake:" in out, "the display looks are INFO rows: %s" % out[-1500:])
    check("INFO app crashes: Chess crashed 2 time(s) since the boot" in out and "no-card boot: not about the driver" in out,
          "crash rows on a no-card boot are INFO: %s" % re.findall(r"(?m)^.*app crashes.*$", out))
    summ = out[out.index("SUMMARY of the newest report"):]
    check(not re.search(r"(?m)^  .{22} (FAIL|WARN|CRITICAL)\b", summ), "summary rows of a no-card boot are INFO: %s" % summ[:1500])
    check(re.search(r"(?m)^  no-card boot +INFO ", summ) is not None, "summary names the no-card boot")

with tempfile.TemporaryDirectory() as d:
    result_dir(d, "rdna3lvl=8 rdna3fb=1", PCI_NOCARD, {tr.CRASH_MEMBER: IPS_BLOCKS})
    rc, out = triage(d)
    check(rc == 1 and "RESULT: something failed" in out, "the driver asked for and the card lost: exit %d" % rc)
    check("!! RDNA3Accel not loaded" in out and "no-card boot" not in out, "not labelled no-card: %s" % out[:600])
    check(re.search(r"(?m)^\s*FAIL display (sleep|wake)", out) is not None, "the display looks stay FAIL: %s" % out[:2500])
    check(re.search(r"(?m)^\s*WARN app crashes: Chess crashed 2 time\(s\)", out) is not None, "crash row WARN: %s" % re.findall(r"(?m)^.*app crashes.*$", out))

with tempfile.TemporaryDirectory() as d:     # the card is there: a normal report is not a no-card boot
    result_dir(d, "keepsyms=1", pci_card())
    rc, out = triage(d)
    check("no-card boot" not in out, "card present: not labelled")

# ---- the GPU vendor row
cd = "$ log\n2026-10-25 02:40:00.000 I WindowServer[1:2] something else\n[exit 0]\n"
r = tr.gpu_vendor_rows(cd, planes=True)
check(levels(r) == ["INFO"] and "iop_ok > 0" in r[0][2], "no vendor line, planes arrive: %r" % r)
for planes in (False, None):
    r = tr.gpu_vendor_rows(cd, planes=planes)
    check(levels(r) == ["WARN"], "no vendor line, planes %r: %r" % (planes, r))
r = tr.gpu_vendor_rows("$ x\n[exit 0]\n", planes=True)
check(levels(r) == ["WARN"], "an empty log stays WARN: %r" % r)

# ---- crash_rows
r = tr.crash_rows(None)
check(levels(r) == ["INFO"] and "older than card run 2026-10-25c" in r[0][2], "absent: %r" % r)
r = tr.crash_rows("$ crash_reports\n[exit 0]\n")
check(levels(r) == ["INFO"], "wrapper only: %r" % r)
r = tr.crash_rows("$ crash_reports\nboot: 1000\n== crash: /x/Chess.ips\nprocess: Chess\n[exit 137]\n")
check(levels(r) == ["WARN"] and "did not finish" in r[0][2], "killed listing: %r" % r)
r = tr.crash_rows("$ crash_reports\nboot: 1000\ncrashes: 0\n[exit 0]\n")
check(levels(r) == ["INFO"] and r[0][1] == "app crashes" and "no crash report" in r[0][2], "none: %r" % r)
r = tr.crash_rows(IPS_BLOCKS)
check(levels(r) == ["WARN", "WARN"] and all(x[1] == "app crashes" for x in r), "two processes: %r" % r)
chess = r[0][2]
check("Chess crashed 2 time(s) since the boot (newest pid 412): EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x0000000000000010; "
      "Segmentation fault: 11; top frame of the crashed thread: libobjc.A.dylib  objc_msgSend + 32; copy 40-crash-ips/1-Chess-" in chess,
      "Chess row: %r" % chess)
check("Maps crashed 1 time(s)" in r[1][2] and "EXC_CRASH (SIGABRT)" in r[1][2] and "Abort trap: 6" in r[1][2] and "__pthread_kill" in r[1][2],
      "Maps row: %r" % r[1][2])
r = tr.crash_rows(IPS_BLOCKS, nocard=True)
check(levels(r) == ["INFO", "INFO"] and "no-card boot" in r[0][2], "no-card: %r" % r)
r = tr.crash_rows("boot: 1000\n== crash: /Users/other/Library/Logs/DiagnosticReports/X.ips\nmtime: 5000\n"
                  "unreadable: no permission (run collect.sh as the admin user, or with sudo)\ncrashes: 1\n")
check(levels(r) == ["WARN"] and "unreadable" in r[0][2] and "/Users/other/" in r[0][2], "unreadable: %r" % r)
FALLBACK = """boot: 1000
== crash: /Library/Logs/DiagnosticReports/Chess.ips
mtime: 5300
size: 537 bytes
copy: 40-crash-ips/1-Chess.ips
(no JSON summary: osascript missing or failed; header fields by grep)
"app_name":"Chess"
"procName":"Chess"
"pid":412
"exception":{"type":"EXC_BAD_ACCESS","signal":"SIGSEGV","subtype":"KERN_INVALID_ADDRESS at 0x10"}
"termination":{"namespace":"SIGNAL","indicator":"Segmentation fault: 11"}
crashes: 1
"""
r = tr.crash_rows(FALLBACK)
check(levels(r) == ["WARN"] and "Chess crashed 1 time(s)" in r[0][2] and "EXC_BAD_ACCESS (SIGSEGV)" in r[0][2]
      and "Segmentation fault: 11" in r[0][2] and "top frame of the crashed thread: not in the report" in r[0][2], "fallback format: %r" % r)

# ---- collect.sh
col = open(os.path.join(ROOT, "tools", "macos", "collect.sh")).read()
check(re.search(r"^run 40-app-crashes\.txt crash_reports$", col, re.M) is not None, "collect.sh does not run crash_reports into 40-app-crashes.txt")
check(tr.CRASH_MEMBER == "40-app-crashes.txt", "member name")
m = re.search(r"^IPS_JXA='.*?^(?=# the Metal plugin's traces first)", col, re.M | re.S)
check(m is not None, "collect.sh has no IPS_JXA .. crash_reports block")
funcs = m.group(0) if m else ""
check("sed -n 's/^{ *sec = \\([0-9]*\\).*/\\1/p'" in funcs, "boot time must be parsed with the BSD-safe sed")
for bad in ("mapfile", "readarray", "grep -P", "date -d", "find -printf", "sed -i "):
    check(bad not in funcs, "bash 3.2 / BSD trap in the crash block: " + bad)

IPS_BODY = ('{"app_name":"Chess","timestamp":"2026-10-25 04:30:00.00 -0700"}\n'
            '{"procName":"Chess","pid":412,"exception":{"type":"EXC_BAD_ACCESS","signal":"SIGSEGV","subtype":"KERN_INVALID_ADDRESS at 0x0000000000000010"},'
            '"termination":{"namespace":"SIGNAL","indicator":"Segmentation fault: 11"},"faultingThread":0,'
            '"threads":[{"id":1,"queue":"com.apple.main-thread","triggered":true,"frames":[{"imageIndex":0,"symbol":"objc_msgSend","symbolLocation":32},'
            '{"imageIndex":1,"imageOffset":4660}]}],"usedImages":[{"name":"libobjc.A.dylib"},{"name":"Chess"}]}\n')
JXA_WANT = ["process: Chess", "pid: 412", "exception: EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x0000000000000010",
            'termination: {"namespace":"SIGNAL","indicator":"Segmentation fault: 11"}', "crashed thread: 0 (com.apple.main-thread)",
            "frame 0: libobjc.A.dylib  objc_msgSend + 32", "frame 1: Chess  0x1234"]


def collect_run(work, dirs, osascript=None, boot=1000, extra_path=None):
    """crash_reports() of collect.sh on the directories, with a fake sysctl (boot time) and a fake osascript
    (None: one that fails, so the grep fallback runs)"""
    bindir = os.path.join(work, "bin")
    os.makedirs(bindir, exist_ok=True)
    with open(os.path.join(bindir, "sysctl"), "w") as f:
        f.write("#!/bin/sh\necho '{ sec = %d, usec = 5 } Thu Jan  1 00:16:40 1970'\n" % boot)
    with open(os.path.join(bindir, "osascript"), "w") as f:
        f.write(osascript if osascript is not None else "#!/bin/sh\nexit 1\n")
    for n in ("sysctl", "osascript"):
        os.chmod(os.path.join(bindir, n), 0o755)
    out = os.path.join(work, "out")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(work, "funcs.sh"), "w") as f:
        f.write(funcs)
    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"], dir=out, RDNA3_CRASH_DIRS=" ".join(dirs))
    res = subprocess.run(["sh", "-c", ". %s/funcs.sh; crash_reports" % work], env=env, capture_output=True, text=True)
    return res, out


with tempfile.TemporaryDirectory() as work:
    d1, d2 = os.path.join(work, "user"), os.path.join(work, "sys")
    os.makedirs(d1)
    os.makedirs(d2)

    def mk(path, body, mtime):
        with open(path, "w") as f:
            f.write(body)
        os.utime(path, (mtime, mtime))
    mk(os.path.join(d1, "Chess-new.ips"), IPS_BODY, 5300)
    mk(os.path.join(d1, "Chess-old.ips"), IPS_BODY, 5200)
    mk(os.path.join(d2, "System Settings-1.ips"), IPS_BODY.replace("Chess", "System Settings"), 5250)     # a space in the name
    mk(os.path.join(d2, "Before the boot.ips"), IPS_BODY, 500)                                          # an earlier boot's
    mk(os.path.join(d2, "Huge.ips"), IPS_BODY + "x" * 310000, 5100)
    res, out = collect_run(work, [d1, d2])
    o = res.stdout
    check(res.returncode == 0, "crash_reports exit %d: %s" % (res.returncode, res.stderr))
    check(o.startswith("boot: 1000\n"), "boot line: %r" % o[:60])
    check(o.rstrip().endswith("crashes: 4"), "four since the boot (the earlier boot's is left out): %r" % o[-200:])
    check("Before the boot" not in o, "an earlier boot's report listed")
    order = re.findall(r"(?m)^== crash: .*/(.*)$", o)
    check(order == ["Chess-new.ips", "System Settings-1.ips", "Chess-old.ips", "Huge.ips"], "newest first: %r" % order)
    check("mtime: 5300 (4300 s after the boot)" in o, "mtime line")
    check("size: %d bytes (copy cut at 300000)" % (len(IPS_BODY) + 310000) in o, "huge: size line")
    copies = sorted(os.listdir(os.path.join(out, "40-crash-ips")))
    check(len(copies) == 4 and "2-System_Settings-1.ips" in copies, "copies: %r" % copies)
    check(os.path.getsize(os.path.join(out, "40-crash-ips", "4-Huge.ips")) == 300000, "the copy is cut at 300000 bytes")
    check(not os.path.exists(os.path.join(out, ".40.list")) and not os.path.exists(os.path.join(out, ".40.list.s")), "temp lists removed")
    check("(no JSON summary: osascript missing or failed" in o and '"procName":"Chess"' in o, "grep fallback: %r" % o[:700])
    rows = tr.crash_rows("$ crash_reports\n" + o + "[exit 0]\n")
    check(sorted(levels(rows)) == ["WARN", "WARN"] and any("Chess crashed 3 time(s)" in x[2] and "Segmentation fault: 11" in x[2] for x in rows),
          "the fallback output through crash_rows: %r" % rows)

    # a fake osascript that prints the JXA summary: its output is used instead of the fallback
    work2 = os.path.join(work, "w2")
    os.makedirs(work2)
    fake = "#!/bin/sh\n" + "".join("echo '%s'\n" % l.replace("'", "") for l in JXA_WANT)
    res, out2 = collect_run(work2, [d1], osascript=fake)
    check("no JSON summary" not in res.stdout and "process: Chess\npid: 412\n" in res.stdout, "osascript output used: %r" % res.stdout[:500])
    rows = tr.crash_rows("$ crash_reports\n" + res.stdout + "[exit 0]\n")
    check(levels(rows) == ["WARN"] and "Chess crashed 2 time(s) since the boot (newest pid 412)" in rows[0][2], "osascript output through crash_rows: %r" % rows)

    # no crash directory, no boot time: still the "crashes:" line
    work3 = os.path.join(work, "w3")
    os.makedirs(work3)
    res, _ = collect_run(work3, [os.path.join(work, "nothing-here")])
    check(res.stdout.endswith("crashes: 0\n"), "no directory: %r" % res.stdout)
    rows = tr.crash_rows("$ crash_reports\n" + res.stdout + "[exit 0]\n")
    check(levels(rows) == ["INFO"], "no crash through crash_rows: %r" % rows)

    # twelve at most
    work4, d4 = os.path.join(work, "w4"), os.path.join(work, "many")
    os.makedirs(work4)
    os.makedirs(d4)
    for i in range(15):
        mk(os.path.join(d4, "P%02d.ips" % i), IPS_BODY, 6000 + i)
    res, out4 = collect_run(work4, [d4])
    check(res.stdout.rstrip().endswith("crashes: 12") and "P14.ips" in res.stdout and "P02.ips" not in res.stdout, "twelve newest: %r" % res.stdout[-120:])

    # the JXA program itself, under node with a stand-in for $.NSString.stringWithContentsOfFileEncodingError
    jxa = re.search(r"^IPS_JXA='(.*?)'$", col, re.M | re.S).group(1)
    node = shutil.which("node")
    f1 = os.path.join(d1, "Chess-new.ips")
    if node:
        js = ("var fs=require('fs');var $={NSString:{stringWithContentsOfFileEncodingError:function(p){return {js:fs.readFileSync(p,'utf8')}}}};\n"
              + jxa + "\nconsole.log(run([process.argv[2]]));")
        jp = os.path.join(work, "t.js")
        open(jp, "w").write(js)
        res = subprocess.run([node, jp, f1], capture_output=True, text=True)
        check(res.stdout.split("\n")[:len(JXA_WANT)] == JXA_WANT, "JXA under node: %r %s" % (res.stdout, res.stderr[:300]))
        # an .ips with another shape: no crashed thread flag (faultingThread index), no exception, header only app_name
        odd = ('{"app_name":"Maps"}\n{"pid":9,"termination":{"indicator":"Abort trap: 6"},"asi":{"libsystem_c.dylib":["abort() called"]},'
               '"faultingThread":1,"threads":[{"frames":[]},{"name":"worker","frames":[{"imageIndex":0,"symbol":"__pthread_kill","symbolLocation":8}]}],'
               '"usedImages":[{"name":"libsystem_kernel.dylib"}]}\n')
        op = os.path.join(work, "odd.ips")
        open(op, "w").write(odd)
        res = subprocess.run([node, jp, op], capture_output=True, text=True)
        check("process: Maps" in res.stdout and "crashed thread: 1 (worker)" in res.stdout and "frame 0: libsystem_kernel.dylib  __pthread_kill + 8" in res.stdout
              and "exception:" not in res.stdout and "asi: " in res.stdout, "JXA, odd shape: %r %s" % (res.stdout, res.stderr[:300]))
    real = shutil.which("osascript")
    if real and sys.platform == "darwin":
        res = subprocess.run([real, "-l", "JavaScript", "-e", jxa, f1], capture_output=True, text=True)
        check(res.stdout.split("\n")[:len(JXA_WANT)] == JXA_WANT, "JXA under the real osascript: %r %s" % (res.stdout, res.stderr[:300]))
        # collect.sh's crash_reports with the real osascript
        work5 = os.path.join(work, "w5")
        os.makedirs(work5)
        env_path = os.path.join(work5, "bin")
        res, _ = collect_run(work5, [d1], osascript="#!/bin/sh\nexec %s \"$@\"\n" % real)
        check("process: Chess\npid: 412\n" in res.stdout, "crash_reports with the real osascript: %r" % res.stdout[:400])
    if not node and not (real and sys.platform == "darwin"):
        print("test_triage_nocard: note: neither node nor a Mac's osascript here, the JXA program was not run")

if fails:
    for f in fails:
        print("test_triage_nocard: FAIL " + f)
    sys.exit(1)
print("test_triage_nocard: ok")
