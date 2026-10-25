"""tools/triage_glyphs.py (DRIVER-PLAN N49) on synthetic plugin logs: the line formats are the plugin's (metal/plugin/nvmtl_vk.c, NVMTLObjects.m).
Cases: a clean default log (INFO only), a RADV debug profile (WARN), the paranoid profile (WARN), a plugin older than N49 (WARN: no RADV line although the
driver opened), load pass layouts (INFO with the counts), the fence that returned early (WARN, old one-line and new counted format), a managed shadow
that changed (WARN) and one that did not (INFO), a rejected flag (WARN), and the hook in triage_reports.py.
Run: python3 -I -B tests/test_triage_glyphs.py"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage_glyphs as tg  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def L(proc, pid, msg, tid="1e7a"):
    return "2026-10-25 10:04:46.1 Df %s[%d:%s] (RDNA3MTLDriver) NVMTL: %s\n" % (proc, pid, tid, msg)


def rows_of(log):
    out = {}
    for lvl, name, text in tg.n49_plugin_rows(log):
        out.setdefault(name, []).append((lvl, text))
    return out


check(tg.n49_plugin_rows("") == [], "empty log: nothing")
check(tg.n49_plugin_rows(L("Chess", 1, "newTextureWithDescriptor: 971x680 -> 0x1")) == [], "an unrelated line: nothing")

# the default driver: INFO
log = (L("Terminal", 907, "vk: driver opened directly: /Library/GPUBundles/rdna3mtl/libvulkan_radeon.dylib (image X)") +
       L("Terminal", 907, "vk: RADV_DEBUG=unset RADV_PERFTEST=unset (NVMTL_RADV_DEBUG=unset, NVMTL_RADV_PERFTEST=unset)"))
r = rows_of(log)
check(list(r) == ["radv debug"] and r["radv debug"][0][0] == "INFO", "default flags: one INFO row, %r" % r)

# a debug profile: WARN, names the process and the flags
log = (L("Chess", 814, "vk: driver opened directly: x") +
       L("Chess", 814, "vk: RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears RADV_PERFTEST=unset (NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears, NVMTL_RADV_PERFTEST=unset) - A DEBUG PROFILE IS ON: this run does not show the default driver") +
       L("WindowServer", 208, "vk: RADV_DEBUG=unset RADV_PERFTEST=unset (NVMTL_RADV_DEBUG=unset, NVMTL_RADV_PERFTEST=unset)"))
r = rows_of(log)
check(r["radv debug"][0][0] == "WARN" and "Chess" in r["radv debug"][0][1] and "nodcc,nohiz,nofmask,nofastclears" in r["radv debug"][0][1], "profile on: WARN naming Chess and the flags, %r" % r)
check("WindowServer" not in r["radv debug"][0][1].split("(")[0], "the unset process is not blamed")

# a plugin older than N49: the driver opened, no RADV line
r = rows_of(L("Terminal", 907, "vk: driver opened directly: x"))
check(r.get("radv debug", [("", "")])[0][0] == "WARN" and "older than DRIVER-PLAN N49" in r["radv debug"][0][1], "old plugin: WARN, %r" % r)

# a rejected flag
r = rows_of(L("Terminal", 907, "vk: NVMTL_RADV_DEBUG=nodcc;rm REJECTED (only lowercase letters, digits, '_' and ',' are taken, at most 200 characters) - RADV_DEBUG unchanged"))
check(r["radv debug"][0][0] == "WARN" and "REJECTED" in r["radv debug"][0][1], "a rejected flag: WARN")

# the paranoid profile
r = rows_of(L("Terminal", 907, "conf: NVMTL_PARANOID=1 - the diagnostic profile is ON (15 switch(es) set: no async commit, no recycling or reuse, no barrier folding, no residency paging, RADV compression metadata off): this run does NOT show the default plugin"))
check(r["paranoid profile"][0][0] == "WARN" and "15 switches" in r["paranoid profile"][0][1], "paranoid: WARN with the count, %r" % r)

# load passes by layout: the newest count wins, per process and kind
lp = ("vk: load pass colour of an image resting in layout %d, the pass declares 6 [%d such pass(es) so far; per layout (UNDEFINED GENERAL COLOR DS DSRO SRO SRC DST PREINIT other): "
      "%s]")
log = (L("Chess", 814, lp % (5, 1, "0 0 0 0 0 1 3 0 0 0")) + L("Chess", 814, lp % (5, 4, "2 1 0 0 0 4 90 0 0 0")) +
       L("Chess", 814, "vk: load pass depth of an image resting in layout 5, the pass declares 3 [2 such pass(es) so far; per layout (UNDEFINED GENERAL COLOR DS DSRO SRO SRC DST PREINIT other): 0 0 0 7 0 2 0 0 0 0]"))
r = rows_of(log)
check(len(r["load pass layouts"]) == 2 and all(x[0] == "INFO" for x in r["load pass layouts"]), "two INFO rows (colour, depth), %r" % r)
check(any("4 load pass(es) began" in x[1] and "UNDEFINED 2" in x[1] and "SRO 4" in x[1] and "SRC 90" in x[1] for x in r["load pass layouts"]), "the newest colour counts are shown, %r" % r["load pass layouts"])
r = rows_of(L("Chess", 814, "vk: load passes: the declared initial layout is MADE TRUE by a barrier from the tracked layout (experiment) (NVMTL_LOAD_BARRIER=1)") + L("Chess", 814, lp % (5, 1, "0 0 0 0 0 1 3 0 0 0")))
check("were transitioned" in r["load pass layouts"][0][1], "the barrier experiment is named")

# the fence that returns early: the old once-per-process line and the new counted one
r = rows_of(L("Chess", 814, "vk: keep witness 3313 landed only 14 us AFTER the flush returned - the flush is not synchronous any more"))
check(r["fence early"][0][0] == "WARN" and "1 time(s)" in r["fence early"][0][1] and "14 us late" in r["fence early"][0][1], "the old line: WARN, %r" % r)
log = (L("Chess", 814, "vk: keep witness 3313 landed only 14 us AFTER the flush returned - the flush is not synchronous any more [1 time(s) so far in 3314 witnessed submit(s), worst 14 us]") +
       L("Chess", 814, "vk: keep witness 9000 landed only 40 us AFTER the flush returned - the flush is not synchronous any more [4 time(s) so far in 9001 witnessed submit(s), worst 52 us]"))
r = rows_of(log)
check(len(r["fence early"]) == 1 and "4 time(s) in 9001 witnessed submits" in r["fence early"][0][1] and "worst 52 us" in r["fence early"][0][1], "the counted line: the newest wins, %r" % r)

# the managed shadow
chg = ("managed: the CPU shadow of a 262144-byte buffer CHANGED at bytes 0..4096 between the flush and the GPU's completion (%s) [%d exact, %d envelope of several marks, in %d uploads checked]")
r = rows_of(L("Terminal", 907, chg % ("one didModifyRange: the GPU copy may have read a mix of two states", 3, 5, 120)))
check(r["managed shadow"][0][0] == "WARN" and "3 time(s) under a single didModifyRange" in r["managed shadow"][0][1] and "5 merged" in r["managed shadow"][0][1], "a changed shadow: WARN, %r" % r)
r = rows_of(L("Terminal", 907, "managed: shadow check ON - each upload's shadow range is hashed at the flush and again when the GPU is done") +
            L("Terminal", 907, "managed: shadow check: 64 upload(s) checked, 0 changed under a single didModifyRange, 0 merged ranges changed"))
check(r["managed shadow"][0][0] == "INFO" and "64 managed upload(s) checked, none changed" in r["managed shadow"][0][1], "an unchanged shadow: INFO, %r" % r)
r = rows_of(L("Terminal", 907, "managed: shadow check ON - each upload's shadow range is hashed at the flush and again when the GPU is done"))
check(r["managed shadow"][0][0] == "INFO" and "no managed upload was checked" in r["managed shadow"][0][1], "the check on and nothing checked: INFO")

# the plain diagnostic switches: one WARN row each, naming the processes
r = rows_of(L("Terminal", 907, "managed: upload snapshot ON - the GPU copy reads a private copy of the shadow range made at the flush (ranges up to 1 MiB) (NVMTL_MANAGED_SNAPSHOT=1)") +
            L("Chess", 814, "commit: synchronous (NVMTL_ASYNC_COMMIT=0) (NVMTL_ASYNC_COMMIT=0)"))
check(len(r["plugin switch"]) == 2 and all(x[0] == "WARN" for x in r["plugin switch"]), "two switch rows, %r" % r)
check(any("NVMTL_MANAGED_SNAPSHOT=1 was on in Terminal" in x[1] for x in r["plugin switch"]) and any("NVMTL_ASYNC_COMMIT=0 was on in Chess" in x[1] for x in r["plugin switch"]), "the rows name switch and process")
r = rows_of(L("Chess", 814, "managed: upload snapshot off - the GPU copy reads the shadow when it runs (NVMTL_MANAGED_SNAPSHOT=unset)") + L("Chess", 814, "commit: ASYNC (default) - execution worker (NVMTL_ASYNC_COMMIT=unset)"))
check("plugin switch" not in r, "the defaults raise no row, %r" % r)
r = rows_of(L("Chess", 814, "vk: load passes: the declared initial layout is MADE TRUE by a barrier from the tracked layout (experiment) (NVMTL_LOAD_BARRIER=1)"))
check(r["plugin switch"][0][0] == "WARN" and "NVMTL_LOAD_BARRIER=1 was on in Chess" in r["plugin switch"][0][1], "LOAD_BARRIER is a switch row, %r" % r)

# the hook in triage_reports.py
src = open(os.path.join(ROOT, "tools", "triage_reports.py")).read()
check("from triage_glyphs import n49_plugin_rows" in src and "n49_plugin_rows(" in src, "triage_reports.py calls n49_plugin_rows")
check("tools/triage_glyphs.py is missing" in src, "triage_reports.py says so loudly when the module is missing")

if fails:
    print("FAIL: %d check(s)" % len(fails))
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("ok: tools/triage_glyphs.py")
