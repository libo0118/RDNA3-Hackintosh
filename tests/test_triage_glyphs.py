"""tools/triage_glyphs.py (DRIVER-PLAN N50) on synthetic plugin logs: the line formats are the plugin's (metal/plugin/nvmtl_vk.c, NVMTLObjects.m).
Cases: a clean default log (INFO only), a RADV debug profile (WARN, quoted values, an empty value is not a profile, process names with blanks), a
rejected flag (WARN with the reason), the paranoid profile and the switch list (WARN), a plugin older than N50 (WARN), conf lines applied and skipped,
the load pass / fence counters of `vk: ab: SUMMARY` (INFO / WARN, pool reuse OFF = no evidence), the managed SUMMARY (changed, inert, skipped,
snapshot), the per-rung rows of the ladder, and the hook in triage_reports.py.
Run: python3 -I -B tests/test_triage_glyphs.py"""
import os
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
    for lvl, name, text in tg.n50_plugin_rows(log):
        out.setdefault(name, []).append((lvl, text))
    return out


check(tg.n50_plugin_rows("") == [], "empty log: nothing")
check(tg.n50_plugin_rows(L("Chess", 1, "newTextureWithDescriptor: 971x680 -> 0x1")) == [], "an unrelated line: nothing")

RADV0 = 'vk: RADV_DEBUG=unset RADV_PERFTEST=unset (NVMTL_RADV_DEBUG=unset, NVMTL_RADV_PERFTEST=unset)'
# the default driver; a process name with a blank is read whole
log = L("Terminal", 907, "vk: driver opened directly: x") + L("Activity Monitor", 786, "vk: driver opened directly: x") + L("Terminal", 907, RADV0) + L("Activity Monitor", 786, RADV0)
r = rows_of(log)
check(r["radv debug"][0][0] == "INFO" and "Activity Monitor" in r["radv debug"][0][1] and "Terminal" in r["radv debug"][0][1], "default flags: INFO naming both processes, %r" % r)

# a profile: WARN naming the process and the flags, and the processes where it is unset
on = 'vk: RADV_DEBUG="nodcc,nohiz,nofmask,nofastclears" RADV_PERFTEST=unset (NVMTL_RADV_DEBUG="nodcc,nohiz,nofmask,nofastclears", NVMTL_RADV_PERFTEST=unset) - A DEBUG PROFILE IS ON: this run does not show the default driver'
r = rows_of(L("Chess", 814, on) + L("WindowServer", 208, RADV0))
check(r["radv debug"][0][0] == "WARN" and "Chess" in r["radv debug"][0][1] and "nodcc,nohiz,nofmask,nofastclears" in r["radv debug"][0][1] and "unset in WindowServer" in r["radv debug"][0][1], "profile on: WARN, %r" % r)
# an empty value is not a profile
r = rows_of(L("Chess", 814, 'vk: RADV_DEBUG="" RADV_PERFTEST="" (NVMTL_RADV_DEBUG=unset, NVMTL_RADV_PERFTEST=unset)'))
check(r["radv debug"][0][0] == "INFO", "an empty RADV_DEBUG is not ON, %r" % r)
# an older plugin: the driver opened, no RADV line
r = rows_of(L("Terminal", 907, "vk: driver opened directly: x"))
check(r["radv debug"][0][0] == "WARN" and "older than DRIVER-PLAN N50" in r["radv debug"][0][1], "old plugin: WARN, %r" % r)
check(any("no `vk: ab switches` line" in t for _, t in r["plugin switch"]), "old plugin: no switch line either, %r" % r)
# a rejected value with a blank is NOT read as an old plugin; the reason is shown
r = rows_of(L("Terminal", 907, "vk: driver opened directly: x") +
            L("Terminal", 907, 'vk: NVMTL_RADV_DEBUG="nodcc nohiz" REJECTED: character 0x20 at position 5 (only RADV\'s own flag names, comma separated, at most 200 characters) - RADV_DEBUG unchanged') +
            L("Terminal", 907, 'vk: RADV_DEBUG=unset RADV_PERFTEST=unset (NVMTL_RADV_DEBUG="nodcc nohiz", NVMTL_RADV_PERFTEST=unset)'))
check(any("0x20" in t and "nodcc nohiz" in t for _, t in r["radv debug"]), "the rejected value and the character are shown, %r" % r)
check(not any("older than" in t for _, t in r["radv debug"]), "a rejected value is not an old plugin, %r" % r)

# the profile and the switch list
r = rows_of(L("Terminal", 907, "conf: NVMTL_PARANOID=1 - the diagnostic profile is ON: 14 switch(es) set by it (NVMTL_RADV_DEBUG=nodcc NVMTL_ASYNC_COMMIT=0), already set and kept: NVMTL_NO_RES2; no async commit, no recycling or reuse, no barrier folding, no residency paging, RADV compression metadata off: this run does NOT show the default plugin"))
check(r["paranoid profile"][0][0] == "WARN" and "14 set by it" in r["paranoid profile"][0][1] and "kept: NVMTL_NO_RES2" in r["paranoid profile"][0][1], "paranoid: WARN, %r" % r)
r = rows_of(L("Terminal", 907, "conf: NVMTL_PARANOID=yes is not 1 - the profile is off"))
check(r["paranoid profile"][0][0] == "WARN" and "NOT 1" not in r["paranoid profile"][0][1] and "OFF" in r["paranoid profile"][0][1], "a PARANOID value that is not 1 is said, %r" % r)
r = rows_of(L("Terminal", 907, 'vk: ab switches [1]: NVMTL_ASYNC_COMMIT="0" NVMTL_NO_ALIGN_GUARD="1"') + L("Chess", 814, 'vk: ab switches [1]: NVMTL_ASYNC_COMMIT="0" NVMTL_MANAGED_CHECK=" 0"') +
            L("WindowServer", 208, "vk: ab switches: none set (no NVMTL_* diagnostic or optimisation switch is in the environment of this process)"))
sw = r["plugin switch"][0][1]
check(r["plugin switch"][0][0] == "WARN" and "NVMTL_ASYNC_COMMIT=0 in Chess, Terminal" in sw and "NVMTL_NO_ALIGN_GUARD=1 in Terminal" in sw and 'NVMTL_MANAGED_CHECK= 0 in Chess' in sw, "switches by knob and value: ' 0' is visible, %r" % r)
r = rows_of(L("WindowServer", 208, "vk: ab switches: none set (no NVMTL_* diagnostic or optimisation switch is in the environment of this process)"))
check(r["plugin switch"][0][0] == "INFO", "none set: INFO, %r" % r)

# the conf file: what was applied, what was skipped
r = rows_of(L("Terminal", 907, 'conf: set NVMTL_RADV_DEBUG="nodcc" NVMTL_ASYNC_COMMIT="0"') + L("Terminal", 907, 'conf: line 3 skipped: NVMTL_ASYNC_COMMIT is already set (an earlier line of the file, or the environment, wins)') +
            L("Terminal", 907, 'conf: line 5 skipped: "RADV_DEBUG" is not an NVMTL_ / NVK_ / NVRM_ knob'))
check(r["plugin conf"][0][0] == "INFO" and 'NVMTL_ASYNC_COMMIT="0"' in r["plugin conf"][0][1], "conf set: INFO, %r" % r)
check(sum(1 for lvl, t in r["plugin conf"] if lvl == "WARN") == 2 and any("already set" in t for _, t in r["plugin conf"]) and any("not an NVMTL_" in t for _, t in r["plugin conf"]), "skipped lines: WARN each, %r" % r)

# the counters
ab = ("vk: ab: SUMMARY load_colour_all=1200 load_colour_mismatch=40 load_colour_inrp=0 load_colour_barriers=0 load_depth_all=10 load_depth_mismatch=2 load_depth_inrp=3 load_depth_barriers=0 "
      "colour_by_layout=2,1,0,0,0,37,0,0,0,0 depth_by_layout=0,0,0,0,0,2,0,0,0,0 wit_early=%d wit_ok=%d wit_blind=1 wit_worst_us=%d")
r = rows_of(L("Chess", 814, ab % (0, 5000, 0)) + L("Chess", 814, ab % (4, 9000, 52)))
check(len(r["load pass layouts"]) == 2 and all(x[0] == "INFO" for x in r["load pass layouts"]), "two INFO rows (colour, depth), %r" % r)
check(any("1200 load pass(es), 40 began" in x[1] and "UNDEFINED 2" in x[1] and "SRO 37" in x[1] for x in r["load pass layouts"]), "colour counts, %r" % r["load pass layouts"])
check(any("3 seen with a render pass already open" in x[1] for x in r["load pass layouts"]), "the in_rp count is shown")
check(r["fence early"][0][0] == "WARN" and "4 time(s) in 9004 witnessed submits" in r["fence early"][0][1] and "worst 52 us" in r["fence early"][0][1], "the newest SUMMARY wins, %r" % r)
r = rows_of(L("Chess", 814, ab % (0, 5000, 0)))
check(r["fence early"][0][0] == "INFO" and "5000 witnessed submits, no early fence" in r["fence early"][0][1], "no early fence: INFO with the count, %r" % r)
r = rows_of(L("Chess", 814, ab % (0, 0, 0)) + L("Chess", 814, "vk: pool reuse OFF by environment - command pools and address pages are created and destroyed per commit"))
check(r["fence early"][0][0] == "INFO" and "NOT evidence" in r["fence early"][0][1], "pool reuse OFF: no evidence either way, %r" % r)
r = rows_of(L("Chess", 814, "vk: keep witness 3313 landed only 14 us AFTER the flush returned - the flush is not synchronous any more"))
check(r["fence early"][0][0] == "WARN" and "older plugin" in r["fence early"][0][1], "an old one-line report: WARN, %r" % r)

# the managed upload check / snapshot
mg = ("managed: SUMMARY check=%d snapshot=%d flushes=100 uploads=%d prologue_uploads=%d sync_uploads=%d checked=%d changed_exact=%d changed_env=%d check_skipped=%d snapped=%d "
      "snap_failed=%d snap_skipped_big=%d snap_skipped_noshadow=%d")
r = rows_of(L("Terminal", 907, mg % (1, 0, 300, 300, 0, 300, 3, 5, 0, 0, 0, 0, 0)))
check(r["managed shadow"][0][0] == "WARN" and "3 time(s) under a single didModifyRange" in r["managed shadow"][0][1] and "5 merged" in r["managed shadow"][0][1], "a changed shadow: WARN, %r" % r)
r = rows_of(L("Terminal", 907, mg % (1, 1, 64, 64, 0, 64, 0, 0, 0, 64, 0, 0, 0)))
check([x[0] for x in r["managed shadow"]] == ["INFO", "INFO"] and "64 managed upload(s) checked, none changed" in r["managed shadow"][0][1] and "64 upload(s) read a private snapshot" in r["managed shadow"][1][1], "clean check + snapshot: INFO, %r" % r)
r = rows_of(L("Terminal", 907, mg % (1, 1, 100, 40, 60, 40, 0, 0, 0, 40, 0, 0, 0)))
check(any(x[0] == "WARN" and "60 of 100" in x[1] and "INERT" in x[1] for x in r["managed shadow"]), "uploads as a synchronous submit: INERT is WARNed, %r" % r)
r = rows_of(L("Terminal", 907, mg % (1, 1, 100, 100, 0, 90, 0, 0, 10, 0, 2, 5, 3)))
check(any(x[0] == "WARN" and "10 upload(s) not checked" in x[1] and "2 snapshot allocation(s) failed" in x[1] and "5 range(s) over 1 MiB" in x[1] and "NO upload was snapshotted" in x[1] for x in r["managed shadow"]), "skips and failures are WARNed, %r" % r)
r = rows_of(L("Terminal", 907, mg % (0, 0, 100, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0)))
check("managed shadow" not in r, "both off: nothing, %r" % r)
r = rows_of(L("Terminal", 907, "managed: shadow check ON - each upload's shadow range is hashed at the flush and again when the GPU is done"))
check(r["managed shadow"][0][0] == "INFO" and "no managed upload was seen" in r["managed shadow"][0][1], "the check on and nothing seen: INFO")

# the ladder, per rung
conf = {"radv": ["NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears"], "sync": ["NVMTL_ASYNC_COMMIT=0"]}
S0 = L("Terminal", 907, "vk: ab: SUMMARY load_colour_all=5 load_colour_mismatch=0 wit_ok=3 wit_early=0 wit_blind=0 wit_worst_us=0")
rungs = {"baseline": {"conf": [], "scope": "apps", "log": L("Terminal", 907, 'vk: ab switches: none set (x)') + S0, "looks": ["a.png"], "verdict": "n"},
         "radv": {"conf": conf["radv"], "scope": "apps", "log": L("Terminal", 907, on) + L("Terminal", 907, 'vk: ab switches [1]: NVMTL_RADV_DEBUG="nodcc,nohiz,nofmask,nofastclears"') + S0, "looks": ["b.png", "c.png"], "verdict": "y"},
         "sync": {"conf": conf["sync"], "scope": "apps", "log": L("Terminal", 907, 'vk: ab switches: none set (x)') + S0, "looks": [], "verdict": None}}
lr = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(rungs)}
check(lr["ladder baseline"][0] == "INFO" and "picture broken" in lr["ladder baseline"][1], "baseline row, %r" % (lr["ladder baseline"],))
check(lr["ladder radv"][0] == "INFO" and "PICTURE RIGHT" in lr["ladder radv"][1] and "2 look(s)" in lr["ladder radv"][1], "radv row, %r" % (lr["ladder radv"],))
check(lr["ladder sync"][0] == "WARN" and "did not reach the plugin" in lr["ladder sync"][1], "a rung whose switch is not in its log slice is WARNed, %r" % (lr["ladder sync"],))
check(lr["ladder paranoid"][0] == "WARN" and "did not run" in lr["ladder paranoid"][1], "a missing rung is WARNed")
check([k for k in lr if k != "ladder"] == ["ladder " + n for n in tg.LADDER_RUNGS], "one row per rung, in order")
check(lr["ladder"][0] == "INFO" and "RIGHT with: radv" in lr["ladder"][1] and "discriminator" in lr["ladder"][1], "the ladder's verdict names the rung that fixed it, %r" % (lr["ladder"],))

# review round 2: a rung slice with a WARN counter row (a skipped conf line, an early fence) used to raise NameError
bad = dict(rungs)
bad["sync"] = dict(rungs["sync"], conf=["NVMTL_ASYNC_COMMIT=0"],
                   log=L("Terminal", 907, 'conf: line 3 skipped: "FOO" is not an NVMTL_ / NVK_ / NVRM_ knob') + L("Terminal", 907, 'vk: ab switches [1]: NVMTL_ASYNC_COMMIT="0"') + S0 +
                       L("Terminal", 907, "vk: ab: SUMMARY wit_early=4 wit_ok=9000 wit_blind=0 wit_worst_us=52"))
try:
    lr2 = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(bad)}
    check(lr2["ladder sync"][0] == "WARN" and "counters:" in lr2["ladder sync"][1] and "conf line" in lr2["ladder sync"][1], "a WARN counter row in a slice is shown, %r" % (lr2["ladder sync"],))
except Exception as e:  # noqa: BLE001
    check(False, "ladder_rows raised %r on a slice with a WARN counter row" % (e,))
# the keys of the slice's `vk: ab switches` against the rung's own: a missing one did not reach, an extra one is not default
extra = dict(rungs)
extra["baseline"] = dict(rungs["baseline"], log=L("Terminal", 907, 'vk: ab switches [1]: NVMTL_ASYNC_COMMIT="0"') + S0)
extra["sync"] = dict(rungs["sync"], log=L("Terminal", 907, 'vk: ab switches [1]: NVMTL_ASYNC_COMMIT="0" NVMTL_NO_POOL_REUSE="1"') + S0)
extra["radv"] = dict(rungs["radv"], log=L("Terminal", 907, 'vk: ab switches [1]: NVMTL_RADV_DEBUG="nodcc"') + L("Chess", 910, 'vk: ab switches: none set (x)') + S0 + L("Chess", 910, "vk: ab: SUMMARY wit_ok=1"))
lr3 = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(extra)}
check(lr3["ladder baseline"][0] == "WARN" and "not default: Terminal: NVMTL_ASYNC_COMMIT" in lr3["ladder baseline"][1], "a leftover switch in the baseline is WARNed, %r" % (lr3["ladder baseline"],))
check(lr3["ladder sync"][0] == "WARN" and "not default: Terminal: NVMTL_NO_POOL_REUSE" in lr3["ladder sync"][1] and "did not reach" not in lr3["ladder sync"][1], "an extra key is WARNed, %r" % (lr3["ladder sync"],))
check(lr3["ladder radv"][0] == "WARN" and "did not reach the plugin" in lr3["ladder radv"][1] and "Chess: NVMTL_RADV_DEBUG" in lr3["ladder radv"][1], "a process without the rung's key is WARNed by name, %r" % (lr3["ladder radv"],))
# paranoid: its 14 switches are expected, anything else is not
par = {"conf": ["NVMTL_PARANOID=1"], "scope": "apps", "looks": [], "verdict": None}
allk = " ".join('%s="1"' % k for k in ("NVMTL_PARANOID",) + tg.PARANOID_KEYS)
par["log"] = L("Terminal", 907, "vk: ab switches [1]: " + allk) + S0
pr = tg.ladder_rows({"paranoid": par})
check([x for x in pr if x[1] == "ladder paranoid"][0][0] == "INFO", "the paranoid rung expects the profile's own switches, %r" % (pr,))
par["log"] = L("Terminal", 907, "vk: ab switches [1]: " + allk + ' NVMTL_LOAD_BARRIER="1"') + S0
pr = tg.ladder_rows({"paranoid": par})
check("not default: Terminal: NVMTL_LOAD_BARRIER" in [x for x in pr if x[1] == "ladder paranoid"][0][2], "a switch beyond the profile is WARNed, %r" % (pr,))
# the SUMMARY is missing: counters unknown (plugin rows and the rung's row)
r = rows_of(L("Terminal", 907, 'vk: ab switches: none set (x)'))
check(r["ab summary"][0][0] == "INFO" and "UNKNOWN" in r["ab summary"][0][1], "no SUMMARY: counters unknown, %r" % r)
r = rows_of(L("Terminal", 907, 'vk: ab switches: none set (x)') + S0)
check("ab summary" not in r, "a SUMMARY: no such row, %r" % r)
nosum = tg.ladder_rows({"baseline": dict(rungs["baseline"], log=L("Terminal", 907, 'vk: ab switches: none set (x)'))})
check([x for x in nosum if x[1] == "ladder baseline"][0][0] == "WARN" and "no SUMMARY line from Terminal" in [x for x in nosum if x[1] == "ladder baseline"][0][2], "a rung without a SUMMARY is WARNed, %r" % (nosum,))
# the profile's list is the plugin's
src_m = open(os.path.join(ROOT, "metal", "plugin", "NVMTLObjects.m")).read()
import re as _re  # noqa: E402
blk = src_m[src_m.index("static const char *const kv[][2]"):]
blk = blk[:blk.index("};")]
check(tuple(_re.findall(r'\{ "(NVMTL_\w+)",', blk)) == tg.PARANOID_KEYS, "PARANOID_KEYS equals the plugin's profile list, %r" % (_re.findall(r'\{ "(NVMTL_\w+)",', blk),))

# ladder.txt as autoreport.sh's ladder_run writes it
LT = '''# rdna3 ladder v1
variant: 2 (ladder-ws)
started: 12:00:00
== rung baseline
scope: apps
conf: (nothing)
env: (nothing)
pids: Terminal=101 Chess=102
looks: ladder-baselinea ladder-baselineh
answer: n
log: baseline.log (12 lines, pids 101 102 , since 2026-10-25 12:00:01)
== end rung baseline
== rung radv
scope: apps
conf: NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears
WARN: an app did not start (Terminal: 201 Chess: no): the rung proves little
looks: ladder-radva ladder-radvh
answer: y
== end rung radv
== rung sync
scope: apps
conf: NVMTL_ASYNC_COMMIT=0
looks: x
'''
v, pr, probs = tg.parse_ladder(LT, {"baseline": "BL", "radv": "RV"})
check(v == 2 and list(pr) == ["baseline", "radv", "sync"], "parse_ladder: variant and rung order, %r %r" % (v, list(pr)))
check(pr["baseline"]["conf"] == [] and pr["baseline"]["verdict"] == "n" and pr["baseline"]["looks"] == ["ladder-baselinea", "ladder-baselineh"] and pr["baseline"]["log"] == "BL" and not pr["baseline"]["cut"], "parse_ladder: baseline, %r" % (pr["baseline"],))
check(pr["radv"]["conf"] == ["NVMTL_RADV_DEBUG=nodcc,nohiz,nofmask,nofastclears"] and pr["radv"]["verdict"] == "y" and pr["radv"]["warn"], "parse_ladder: radv with its WARN line, %r" % (pr["radv"],))
check(pr["sync"]["cut"] and pr["sync"]["verdict"] is None, "parse_ladder: a rung without its end line is cut")
check(probs and "no closing" in probs[0], "parse_ladder: no '== end' is a problem, %r" % (probs,))
rows = tg.ladder_rows(pr, v, probs)
d = {}
for lvl, name, text in rows:
    d.setdefault(name, []).append((lvl, text))
check(d["ladder"][0][0] == "WARN" and "cut short" in d["ladder"][0][1], "the cut run is WARNed, %r" % (d.get("ladder"),))
check("ladder radv-ws" in d and "ladder paranoid-ws" in d and "did not run" in d["ladder radv-ws"][0][1], "variant 2 expects the WindowServer rungs, %r" % (list(d),))
check(d["ladder radv"][0][0] == "WARN" and "an app did not start" in d["ladder radv"][0][1], "a rung with an app that did not start is WARNed, %r" % (d["ladder radv"],))
check(d["ladder sync"][0][0] == "WARN" and "cut off" in d["ladder sync"][0][1], "a cut rung is WARNed, %r" % (d["ladder sync"],))
check(tg.ladder_log_name("radv-ws") == "41-ladder-radv-ws.log", "log member name")

# N51: the surface-in cache rows
SIN = ("vk: surfin: SUMMARY cache=1 calls=15430 fast=9000 miss_unsynced=10 miss_seed=400 miss_gen=2 locked_upload=466 locked_skip_vram=3 bufcopy=5000 bufcopy_uploaded=500 "
       "bufcopy_skipped=4500 bufcopy_noshadow=0 bufcopy_failed=%d bufcopy_forgotten=3 bufcopy_up_us=40000 bufcopy_skip_us=9000 shadow_bytes=100000 shadow_refused=0 "
       "cimg_new=40 cimg_recycled=400 cimg_parked=410 cimg_evicted=5 cimg_refused=0")
r = rows_of(L("WindowServer", 208, "vk: ab switches: none set (no NVMTL_* diagnostic or optimisation switch is in the environment of this process)") + L("WindowServer", 208, SIN % 0))
check(r["surface-in cache"][0][0] == "INFO" and "9000 answered by the probe" in r["surface-in cache"][0][1] and "4500 skipped unchanged" in r["surface-in cache"][0][1]
      and "400 reused" in r["surface-in cache"][0][1], "surfin SUMMARY: INFO with the hit / skip numbers, %r" % r)
r = rows_of(L("WindowServer", 208, SIN % 2))
check(r["surface-in cache"][0][0] == "WARN" and "FAILED" in r["surface-in cache"][0][1], "a failed refresh is WARNed, %r" % r)
r = rows_of(L("WindowServer", 208, (SIN % 0).replace("cache=1", "cache=0")))
check(r["surface-in cache"][0][0] == "WARN" and "NVMTL_NO_SURFIN_CACHE" in r["surface-in cache"][0][1], "the kill switch is WARNed, %r" % r)
r = rows_of(L("WindowServer", 208, (SIN % 0).replace("bufcopy_noshadow=0", "bufcopy_noshadow=4000").replace("shadow_refused=0", "shadow_refused=9")))
check(r["surface-in cache"][0][0] == "WARN" and "no shadow" in r["surface-in cache"][0][1], "refreshes without a shadow are WARNed, %r" % r)
r = rows_of(L("Terminal", 907, 'vk: ab switches [1]: NVMTL_NO_SURFIN_CACHE="1"'))
check(r["surface-in cache"][0][0] == "INFO" and "no `vk: surfin: SUMMARY`" in r["surface-in cache"][0][1], "a process without the SUMMARY is said, %r" % r)

# the hook in triage_reports.py
src = open(os.path.join(ROOT, "tools", "triage_reports.py")).read()
check("from triage_glyphs import n50_plugin_rows" in src and "n50_plugin_rows(" in src, "triage_reports.py calls n50_plugin_rows")
check("tools/triage_glyphs.py is missing" in src, "triage_reports.py says so loudly when the module is missing")
check("41-ladder.txt" in src and "parse_ladder" in src and "ladder_rows(" in src, "triage_reports.py reads the archive's 41-ladder members")
check("except Exception as e_:" in src[src.index("parse_ladder, ladder_rows"):][:900] and "ladder_rows_ = [(\"WARN\", \"ladder\", \"tools/triage_glyphs.py FAILED" in src, "triage_reports.py survives any error of the ladder module with a loud WARN")
check("n50_rows_ = [(\"WARN\", \"plugin A/B\", \"tools/triage_glyphs.py FAILED" in src, "triage_reports.py survives any error of n50_plugin_rows with a loud WARN")

if fails:
    print("FAIL: %d check(s)" % len(fails))
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("ok: tools/triage_glyphs.py")
