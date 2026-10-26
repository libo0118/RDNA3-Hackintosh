#!/usr/bin/env python3
"""tools/triage_bootwatch.py: the boot watchdog's files (tools/vm/macos/autoreport.sh BOOTWATCH block) as triage rows.
The files are written by the shell block (tests/test_bootwatch.sh runs it with stubs); here the parser reads hand-made ones in the
real formats of `sample` (call graph + top of stack) and spindump, and tools/triage_reports.py prints the top-level FAIL for a
result directory that has no report at all (the owner's emergency exit), and result_checklist.py turns it into a FAIL row."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_bootwatch as tb  # noqa: E402
import result_checklist as rc  # noqa: E402

fails = []

SAMPLE = """Sampling process 412 for 5 seconds with 1 millisecond of run time between samples
Call graph:
    4980 Thread_1001   DispatchQueue_1: com.apple.main-thread  (serial)
    + 4980 start  (in dyld) + 6400  [0x7ff8100a141f]
    +   4980 WindowServer_main  (in WindowServer) + 120  [0x10a2b3c40]
    +     4980 CGXServer  (in SkyLight) + 88  [0x7ff80f111111]
    +       4980 IOAccelOpen  (in IOKit) + 33  [0x7ff80e222222]
    +         4980 IOServiceOpen  (in IOKit) + 187  [0x7ff80e333333]
    +           4980 mach_msg2_trap  (in libsystem_kernel.dylib) + 10  [0x7ff8100b4f2a]
    4980 Thread_1002: com.apple.CFRunLoop
    + 4980 mach_msg2_trap  (in libsystem_kernel.dylib) + 10  [0x7ff8100b4f2a]

Total number in stack (recursive counted multiple, when >=5):

Sort by top of stack, same collapsed (when >= 5):
        mach_msg2_trap  (in libsystem_kernel.dylib)        9960

Binary Images:
"""

SPINDUMP = """Heaviest stack for the main thread of the target process:
  4 start + 6400 (dyld + 5000) [0x1]
  4 WindowServer_main + 120 (WindowServer + 100) [0x2]
  4 IOServiceOpen + 187 (IOKit + 9) [0x3]
  4 mach_msg2_trap + 10 (libsystem_kernel.dylib + 1) [0x4]

Process: x
"""

TOP_ONLY = """Sort by top of stack, same collapsed (when >= 5):
        semaphore_wait_trap  (in libsystem_kernel.dylib)        4000
        mach_msg2_trap  (in libsystem_kernel.dylib)        1000

Binary Images:
"""

who, fr = tb.sample_frames(SAMPLE)
if not fr or not fr[0].startswith("mach_msg2_trap (in libsystem_kernel.dylib)") or "IOServiceOpen (in IOKit)" not in fr[1] \
        or any("0x" in f for f in fr) or "main thread" not in who:
    fails.append("call graph chain: %r %r" % (who, fr))
if any("CFRunLoop" in f for f in fr):
    fails.append("the second thread leaked into the main thread's chain: %r" % (fr,))
who, fr = tb.sample_frames(SPINDUMP)
if not fr or not fr[0].startswith("mach_msg2_trap") or "heaviest" not in who or any("0x" in f for f in fr):
    fails.append("spindump: %r %r" % (who, fr))
who, fr = tb.sample_frames(TOP_ONLY)
if fr != ["semaphore_wait_trap (in libsystem_kernel.dylib) x4000", "mach_msg2_trap (in libsystem_kernel.dylib) x1000"]:
    fails.append("top of stack list: %r" % (fr,))
if tb.sample_frames("no stacks here\n") != ("", []):
    fails.append("empty input")


def body(n, after, sample=SAMPLE, running=True):
    return ("BOOTWATCH: n=%d trigger_s=75 after_s=%d uptime_s=%d console_user=root stamp=20261026-120000\n" % (n, after, after + 20) +
            "date: x\n== ps (WindowServer, loginwindow, AGPM)\n" +
            ("  412 1 Ss 0.0 01:20 - /System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer\n" if running
             else "WindowServer: NOT RUNNING\n") +
            "== sample WindowServer\n" + sample + "BOOTWATCH-DONE: n=%d\n" % n)


def write(d, name, text):
    with open(os.path.join(d, name), "w") as f:
        f.write(text)


# a boot that never got there: two snapshots, no END -> one FAIL row with the frames
with tempfile.TemporaryDirectory() as d:
    write(d, "bootwatch-20261026-120000-1.txt", body(1, 78))
    write(d, "bootwatch-20261026-120000-2.txt", body(2, 140))
    r = tb.bootwatch_rows(d)
    if len(r) != 1 or r[0][0] != "FAIL" or "did not reach the GPU within 75 s" not in r[0][2] or "2 snapshot(s)" not in r[0][2] \
            or "140 s after the start" not in r[0][2] or "mach_msg2_trap" not in r[0][2]:
        fails.append("never reached: %r" % (r,))
    elif not any("IOServiceOpen" in l for l in r[0][3]):
        fails.append("detail lines lack the frames: %r" % (r[0][3],))
    # the whole triage: no report at all, the FAIL is a top-level row, and the exit status says failed
    p = subprocess.run([sys.executable, "-I", "-B", os.path.join(ROOT, "tools", "triage_reports.py"), d],
                       capture_output=True, text=True, env=dict(os.environ, RDNA3_NO_PICTURES="1"))
    out = p.stdout
    if "FAIL BOOTWATCH: WindowServer did not reach the GPU within 75 s" not in out or p.returncode != 1 or "Traceback" in p.stderr:
        fails.append("triage_reports.py: rc %d\n%s\n%s" % (p.returncode, out[-1500:], p.stderr[-800:]))
    # the checklist makes it a FAIL row of its own
    rows, result, note = rc.parse(out.splitlines())
    if not any(nm.startswith("triage: BOOTWATCH") and forced == rc.FAIL and "did not reach the GPU" in tx for nm, tx, forced in rows):
        fails.append("checklist rows: %r" % (rows,))
    # an unpacked 'rdna3vm result' (rdna3-result/ below the directory)
with tempfile.TemporaryDirectory() as d:
    os.mkdir(os.path.join(d, "rdna3-result"))
    write(os.path.join(d, "rdna3-result"), "bootwatch-20261026-120000-1.txt", body(1, 78, running=False, sample="WindowServer: NOT RUNNING\n"))
    r = tb.bootwatch_rows(d, os.path.join(d, "rdna3-result"))   # the same file twice (d and d0 are one directory): one row
    r2 = tb.bootwatch_rows(os.path.join(d, "rdna3-result"))
    if len(r2) != 1 or r2[0][0] != "FAIL" or "NOT RUNNING" not in r2[0][2] or not r2[0][3]:
        fails.append("no WindowServer: %r" % (r2,))
    if len(r) != 1:
        fails.append("rows doubled: %r" % (r,))

# card run 2026-10-26j (DRIVER-PLAN N58): the boot's spindump says the kernel deadlocked, with our thread call in the cycle
SPIN_DEADLOCK = """Date/Time:        2026-10-26 13:55:10.000 -0700
Deadlock:         kernel_task [0] thread 0x6a7 "AppleGPUWrangler" -> kernel_task [0] thread 0x136 "thread call high #9" -> kernel_task [0] thread 0x6a7 "AppleGPUWrangler"
Blocked by Deadlock: 10 tasks - kernel_task [0], WindowServer [204]

Process:          kernel_task [0]
Deadlocked:       2 threads - Thread 0x6a7 "AppleGPUWrangler", Thread 0x136 "thread call high #9"

  Thread 0x136    Thread name "thread call high #9"    305 samples (1-305)    priority 93 (base 93)
 *305  ??? (kernel + 12702) [0xffffff80002eb19e]
   *305  ??? (RDNA3Accelerator + 179057) [0xffffff7f9664eb71]
     *305  ??? (IOAcceleratorFamily2 + 3052) [0xffffff7f95261bec]
       *305  ??? (IOGraphicsFamily + 131240) [0xffffff7f953ff0a8]
         *305  ??? (kernel + 1989367) [0xffffff80004cdaf7] (blocked by turnstile waiting for kernel_task [0] thread 0x6a7 - part of a deadlock)

  Thread 0x137    Thread name "thread call high #10"    305 samples (1-305)    priority 93 (base 93)
 *305  ??? (IOGraphicsFamily + 101536) [0xffffff7f953f7ca0]
   *305  ??? (kernel + 1989367) [0xffffff80004cdaf7] (blocked by turnstile waiting for kernel_task [0] thread 0x6a7 - ends in a deadlock)

  Thread 0x6a7    Thread name "AppleGPUWrangler"    305 samples (1-305)    priority 93 (base 81)
 *305  ??? (AppleGPUWrangler + 23046) [0xffffff7f94a16a06]
   *305  ??? (IOGraphicsFamily + 115533) [0xffffff7f953fb34d]
     *305  ??? (IOAcceleratorFamily2 + 251657) [0xffffff7f9529e709]
       *305  ??? (kernel + 1989367) [0xffffff80004cdaf7] (blocked by turnstile with priority 93 waiting for kernel_task [0] thread 0x136 - part of a deadlock)

"""
cyc, thr = tb.spindump_deadlocks(SPIN_DEADLOCK)
if len(cyc) != 1 or "AppleGPUWrangler" not in cyc[0] or [t[0] for t in thr] != ["0x136", "0x6a7"] \
        or thr[0][2] != ["RDNA3Accelerator", "IOAcceleratorFamily2", "IOGraphicsFamily"]:
    fails.append("spindump deadlock parse: %r %r" % (cyc, thr))
if tb.spindump_deadlocks(SPINDUMP) != ([], []):
    fails.append("a spindump without a deadlock gave one")
with tempfile.TemporaryDirectory() as d:
    write(d, "bootwatch-20261026-120000-1.txt", body(1, 78))
    write(d, "bootwatch-20261026-120000-1.spindump", SPIN_DEADLOCK)
    write(d, "bootwatch-20261026-120000-2.spindump", SPIN_DEADLOCK)   # the second look: the same cycle, said once
    r = tb.bootwatch_rows(d)
    if len(r) != 1 or r[0][0] != "FAIL" or "KERNEL DEADLOCK" not in r[0][2] or "OUR RDNA3Accelerator in the cycle" not in r[0][2] \
            or r[0][2].count("KERNEL DEADLOCK") != 1 or not any("thread 0x136" in l for l in r[0][3]):
        fails.append("deadlock row: %r" % (r,))
    # without the spindump: no deadlock words
    os.remove(os.path.join(d, "bootwatch-20261026-120000-1.spindump"))
    os.remove(os.path.join(d, "bootwatch-20261026-120000-2.spindump"))
    r = tb.bootwatch_rows(d)
    if "DEADLOCK" in r[0][2]:
        fails.append("deadlock without a spindump: %r" % (r,))

# it got there late: WARN, not FAIL; a user logging in the same; a gave-up END stays FAIL
for state, level in (("gpu", "WARN"), ("login", "WARN"), ("gave-up", "FAIL"), ("timeout", "FAIL")):
    with tempfile.TemporaryDirectory() as d:
        write(d, "bootwatch-20261026-120000-1.txt", body(1, 80))
        write(d, "bootwatch-20261026-120000-2.txt",
              "BOOTWATCH-END: state=%s after_s=190 uptime_s=210 snapshots=1 trigger_s=75 stamp=20261026-120000 console_user=root\n" % state)
        r = tb.bootwatch_rows(d)
        if len(r) != 1 or r[0][0] != level or "75 s" not in r[0][2] or (level == "WARN" and "190 s" not in r[0][2]):
            fails.append("END %s: %r" % (state, r))

# a boot with only the END (the first file never arrived) and two boots in one directory
with tempfile.TemporaryDirectory() as d:
    write(d, "bootwatch-20261026-110000-1.txt", body(1, 80))
    write(d, "bootwatch-20261026-120000-1.txt", body(1, 80))
    write(d, "bootwatch-20261026-130000-3.txt",
          "BOOTWATCH-END: state=gpu after_s=300 uptime_s=320 snapshots=2 trigger_s=75 stamp=20261026-130000 console_user=root\n")
    r = tb.bootwatch_rows(d)
    if [x[0] for x in r] != ["FAIL", "FAIL", "WARN"]:
        fails.append("three boots: %r" % ([(x[0], x[2][:60]) for x in r],))

# a header-less (cut off) file is a WARN, a directory without files gives nothing, a stray name is ignored
with tempfile.TemporaryDirectory() as d:
    write(d, "bootwatch-20261026-120000-1.txt", "garbage")
    write(d, "bootwatch-notes.txt", "x")
    r = tb.bootwatch_rows(d)
    if len(r) != 1 or r[0][0] != "WARN" or "no BOOTWATCH header" not in r[0][2]:
        fails.append("cut-off file: %r" % (r,))
with tempfile.TemporaryDirectory() as d:
    if tb.bootwatch_rows(d, None, os.path.join(d, "nope")):
        fails.append("empty directory gave rows")

if fails:
    print("test_triage_bootwatch: FAIL")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print("test_triage_bootwatch: ok")
