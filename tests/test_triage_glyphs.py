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
SIN = ("vk: surfin: SUMMARY seq=7 t_ms=70000 cache=1 cimg_on=1 verify=%d calls=15430 sin_us_total=1000000 fast=9000 fast_us=20000 miss_unsynced=10 miss_seed=400 miss_gen=2 "
       "locked_upload=466 locked_skip_vram=3 locked_skip_pending=1 locked_skip_overwrite=2 locked_fail=0 bufcopy=5000 bufcopy_dedup=800 bufcopy_uploaded=500 bufcopy_up_us=40000 bufcopy_skipped=4500 "
       "bufcopy_skip_us=9000 bufcopy_noshadow=0 bufcopy_noshadow_us=0 bufcopy_failed=%d bufcopy_forgotten=3 bufcopy_skip_refused_layout=4 shadow_bytes=100000 shadow_refused=0 "
       "verify_checked=%d verify_mismatch=%d verify_failed=0 rsample_by_layout=0,16,100,283,0,1 cimg_new=40 cimg_recycled=400 cimg_parked=410 cimg_evicted=5 cimg_refused_size=1 cimg_refused_form=2 cimg_refused_full=3")
def sin(verify=0, failed=0, checked=0, bad=0): return SIN % (verify, failed, checked, bad)
r = rows_of(L("WindowServer", 208, "vk: ab switches: none set (no NVMTL_* diagnostic or optimisation switch is in the environment of this process)") + L("WindowServer", 208, sin()))
t = r["surface-in cache"][0][1]
check(r["surface-in cache"][0][0] == "INFO" and "9000 answered by the probe" in t and "4500 skipped unchanged" in t and "400 reused" in t, "surfin SUMMARY: INFO with the hit / skip numbers, %r" % r)
check("5 %" in t and "SUMMARY #7 after 70 s" in t and "4 skips refused by the image's layout" in t and "3 dropped by a writer" in t and "refused (size 1, shape 2, pool full 3)" in t
      and "shadows 97.7 KB" in t and "800 buffer-texture repeat" in t and "2 overwrite skips" in t, "the refresh share, the seq, forgotten, layout refusals and refusals are printed, %r" % t)
check(r["sampled layout"][0][0] == "INFO" and "400 render-encoder sample(s)" in r["sampled layout"][0][1] and "300 (75 %)" in r["sampled layout"][0][1] and "TRANSFER_SRC 283" in r["sampled layout"][0][1],
      "the layout the render samples saw is printed, %r" % r)
r = rows_of(L("WindowServer", 208, sin(failed=2)))
check(r["surface-in cache"][0][0] == "WARN" and "FAILED" in r["surface-in cache"][0][1], "a failed refresh is WARNed, %r" % r)
r = rows_of(L("WindowServer", 208, sin().replace("cache=1", "cache=0")))
check(r["surface-in cache"][0][0] == "WARN" and "NVMTL_NO_SURFIN_CACHE" in r["surface-in cache"][0][1], "the kill switch is WARNed, %r" % r)
r = rows_of(L("WindowServer", 208, sin().replace("bufcopy_noshadow=0", "bufcopy_noshadow=4000").replace("shadow_refused=0", "shadow_refused=9")))
check(r["surface-in cache"][0][0] == "WARN" and "no shadow" in r["surface-in cache"][0][1], "refreshes without a shadow are WARNed, %r" % r)
r = rows_of(L("WindowServer", 208, sin(verify=50, checked=90, bad=2)))
check(r["surface-in cache"][0][0] == "FAIL" and "2 STALE" in r["surface-in cache"][0][1], "a verified stale image is a FAIL, %r" % r)
r = rows_of(L("WindowServer", 208, sin(verify=50, checked=90)))
check(r["surface-in cache"][0][0] == "INFO" and "90 skip(s) read back, 0 stale" in r["surface-in cache"][0][1], "a clean verify is said, %r" % r)
r = rows_of(L("WindowServer", 208, sin(verify=50)))
check(r["surface-in cache"][0][0] == "WARN" and "no skip was verified" in r["surface-in cache"][0][1], "verify on with nothing verified is WARNed, %r" % r)
r = rows_of(L("Terminal", 907, 'vk: ab switches [1]: NVMTL_NO_SURFIN_CACHE="1"'))
check(r["surface-in cache"][0][0] == "INFO" and "no `vk: surfin: SUMMARY`" in r["surface-in cache"][0][1], "another process without the SUMMARY is INFO, %r" % r)
r = rows_of(L("WindowServer", 208, 'vk: ab switches: none set (no NVMTL_* diagnostic or optimisation switch is in the environment of this process)'))
check(r["surface-in cache"][0][0] == "WARN" and "timer" in r["surface-in cache"][0][1], "WindowServer without the SUMMARY is a WARN, %r" % r)

# the surfin SUMMARY is two lines (nvlog cuts at 1023 characters): SUMMARY2 of the same seq completes SUMMARY; an old seq's second half does not
two = (L("Terminal", 907, "vk: surfin: SUMMARY seq=5 t_ms=50000 cache=1 cimg_on=1 verify=0 calls=136 sin_us_total=14948 fast=109 miss_unsynced=4 miss_seed=0 miss_gen=0 locked_upload=3 locked_skip_vram=0 locked_skip_pending=0 locked_skip_overwrite=0 locked_fail=0")
       + L("Terminal", 907, "vk: surfin: SUMMARY2 seq=5 bufcopy=40 bufcopy_dedup=0 bufcopy_uploaded=7 bufcopy_up_us=700 bufcopy_skipped=33 bufcopy_skip_us=66 bufcopy_noshadow=0 bufcopy_noshadow_us=0 bufcopy_failed=0 cimg_new=2")
       + L("Terminal", 907, "vk: surfin: SUMMARY2 seq=4 bufcopy=1 bufcopy_uploaded=1"))
t2 = rows_of(two)["surface-in cache"][0][1]
check("40 buffer-texture refresh(es)" in t2 and "7 uploaded" in t2 and "33 skipped unchanged" in t2, "SUMMARY2 completes SUMMARY of the same seq only, %r" % (t2[:400],))

# ---- DRIVER-PLAN N50, the glyph hunt (variant 3), the automatic glyph verdict, the undeclared-write detector, the same-image copy and the surface census
check(len(tg.LADDER_GLYPHS_RUNGS) == 13 and tg.LADDER_GLYPHS_RUNGS[:3] == ("baseline", "managed-whole", "managed-check") and tg.LADDER_GLYPHS_RUNGS[-1] == "paranoid" == tg.GLYPHS_CONTROL,
      "the glyph hunt has 13 rungs, the strongest first, the negative control last")
check(set(tg.GLYPHS_MEANING) <= set(tg.LADDER_GLYPHS_RUNGS) - {"baseline", "paranoid"}, "every meaning belongs to a rung of the hunt")
KNOB = {"managed-whole": "NVMTL_MANAGED_WHOLE=1", "managed-check": "NVMTL_MANAGED_CHECK=1", "no-surface-share": "NVMTL_NO_SURFACE_SHARE=1", "no-surface-pageoff": "NVMTL_NO_SURFACE_PAGEOFF=1",
        "fbfetch-zero": "NVMTL_FBFETCH_ZERO=1", "managed-pages": "NVMTL_MANAGED_PAGES=1", "managed-eager-zero": "NVMTL_MANAGED_EAGER_ZERO=1", "no-texbuf": "NVMTL_NO_TEXBUF=1",
        "no-bindless": "NVMTL_NO_BINDLESS=1", "dontcare-clears": "NVMTL_DONTCARE_CLEARS=1", "skip-empty-0": "NVMTL_SKIP_EMPTY=0", "paranoid": "NVMTL_PARANOID=1"}
MGK = ("snapshot=0 flushes=40 uploads=80 prologue_uploads=80 sync_uploads=0 checked=0 changed_exact=0 changed_env=0 check_skipped=0 snapped=0 snap_failed=0 snap_skipped_big=0 snap_skipped_noshadow=0 "
       "pages=%d decl_bytes=9000 up_bytes=%d widened_uploads=%d ud_scans=%d ud_idle_scans=%d ud_hits=0 ud_idle_hits=0 ud_bytes=0 ud_samepage_hits=0 ud_late_bytes=0 ud_buffers=0 ud_gpu_off=0 "
       "ud_nomirror=0 ud_mirrors=3 ud_mirror_bytes=786432 ud_managed=3 ud_whole_marks=0")


def term_log(name, pid=907, **over):
    """what the plugin logs in the Terminal of a rung: the effective-state lines of the default plugin, changed the way the rung's knob changes them, and the SUMMARY lines"""
    k = KNOB.get(name, "")
    st = dict(share="live in the surface's family VRAM", pageoff="marked newest", zero="clear their VRAM copy", tex="ON", bind="ON", skip="are NOT submitted", forced=0, widened=0, scans=0, dcc=0, whole=0, pages=0, para=0)
    if name == "no-surface-share": st["share"] = "private per process"
    if name == "no-surface-pageoff": st["pageoff"] = "copied out eagerly (old)"
    if name == "managed-eager-zero": st["zero"] = "memset their CPU copy at creation and upload it (old)"
    if name == "no-texbuf": st["tex"] = "OFF"
    if name == "no-bindless": st["bind"] = "OFF"
    if name == "skip-empty-0": st["skip"] = "are submitted as before b78"
    if name == "fbfetch-zero": st["forced"] = 3
    if name == "managed-whole": st["whole"], st["widened"] = 1, 70
    if name == "managed-pages": st["pages"], st["widened"] = 1, 50
    if name == "managed-check": st["scans"] = 380
    if name == "dontcare-clears": st["dcc"] = 9
    st.update(over)
    m = lambda msg: L("Terminal", pid, msg)
    out = m('vk: ab switches [1]: ' + " ".join('%s="%s"' % tuple(k.split("=", 1)) for _ in [0] if k)) if k else m("vk: ab switches: none set (x)")
    out += m("item 7: IOSurface textures %s (NVK import ABSENT, NVMTL_NO_SURFACE_SHARE=unset)" % st["share"])
    out += m("after a GPU write, IOSurface VRAM is %s (NVK dirty export ABSENT, NVMTL_NO_SURFACE_PAGEOFF=unset)" % st["pageoff"])
    out += m("zero-fill: managed buffers %s (NVMTL_MANAGED_EAGER_ZERO=unset)" % st["zero"])
    out += m("vk: par3 VK_EXT_mutable_descriptor_type present \u2014 texture buffers %s (mutable texture/storage bands)" % st["tex"])
    out += m("vk: descriptor indexing present - bindless textures %s (set 2)" % st["bind"])
    out += m("commit: empty submission parts %s (NVMTL_SKIP_EMPTY=unset)" % st["skip"])
    if st["skip"].startswith("are NOT"): out += m("commit: 3 empty submission parts NOT submitted (no encoder, no resources) - each WAS a full submit + wait")
    if st["para"] or name == "paranoid": out += m("conf: NVMTL_PARANOID=1 - the diagnostic profile is ON: 15 switch(es) set by it (NVMTL_RADV_DEBUG=nodcc), already set and kept: none; x")
    out += m("vk: ab: SUMMARY wit_ok=3 wit_early=0 dontcare_seen=9 dontcare_cleared=%d same_image_copies=0" % st["dcc"])
    out += m("managed: SUMMARY check=%d whole=%d " % (1 if name == "managed-check" else 0, st["whole"]) + MGK % (st["pages"], 9000 if not st["widened"] else 200000, st["widened"], st["scans"], 0))
    out += m("vk: fbfetch: SUMMARY served=4 modules=4 read=4 zmod=0 zr_forced=%d draws=100" % st["forced"])
    out += m("census: SUMMARY seq=2 t_ms=20000 surfaces=1 overflow=0 sin_up_vram=0 sin_up_priv=0 sin_up_mb=0 sin_skip=0 sin_fail=0 pass=10 pass_clear=0 draws=50 draw_blit=20 draw_fbf=30 blitenc=0 replace=0 "
            "compute=0 present=0 out_dirty=9 out_copy=0 out_fail=0 top=325:640x353:V:P10,D50,B20,F30,O9")
    return out


def fresh_rungs(**overlog):
    r = {n: {"conf": [KNOB[n]] if n in KNOB else [], "scope": "apps", "looks": ["ladder-%sa" % n, "ladder-%sh" % n, "ladder-%sh2" % n], "verdict": None, "cut": False, "warn": [],
             "pids": "Terminal=907 Chess=908 ", "terminal_pid": 907, "log": term_log(n)} for n in tg.LADDER_GLYPHS_RUNGS}
    for n, lg in overlog.items(): r[n]["log"] = lg
    return r


P3 = fresh_rungs()
glyphs = {n: {"verdict": "broken", "text": "BROKEN: 2 of 23 tick lines have a twin (9 %) in the live window"} for n in tg.LADDER_GLYPHS_RUNGS}
glyphs["baseline"] = {"verdict": "broken", "text": "BROKEN: 5 of 23 tick lines have a twin (22 %) in the live window"}
glyphs["managed-whole"] = {"verdict": "clean", "text": "CLEAN: 22 of 23 tick lines have a twin (96 %) in the live window"}
glyphs["no-texbuf"] = {"verdict": "none", "text": "no LIVE tick stack"}
rows = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(P3, 3, (), glyphs)}
check(all(rows["ladder " + n][0] == "INFO" or n == "no-texbuf" for n in tg.LADDER_GLYPHS_RUNGS), "a complete set of logs: every rung INFO, got %r" % ([(n, rows["ladder " + n][1][-300:]) for n in tg.LADDER_GLYPHS_RUNGS if rows["ladder " + n][0] != "INFO" and n != "no-texbuf"][:2],))
check("glyphs (automatic, tools/glyph_verdict.py): CLEAN: 22 of 23" in rows["ladder managed-whole"][1] and "knob engaged (70 of 80 uploads widened)" in rows["ladder managed-whole"][1], "the rung says what the verdict was and that the knob engaged, %r" % (rows["ladder managed-whole"],))
check("surface census: Terminal[907] (SUMMARY #2" in rows["ladder baseline"][1] and "fence early" in rows["ladder baseline"][1], "the rung row carries the census and the fence rows of the Terminal, %r" % (rows["ladder baseline"],))
check("CLEAN with: managed-whole" in rows["ladder glyphs"][1] and "BROKEN and" in rows["ladder glyphs"][1], "the ladder glyph row names the clean rung, %r" % (rows.get("ladder glyphs"),))
check("ladder glyphs managed-whole" in rows and "hypothesis A" in rows["ladder glyphs managed-whole"][1], "a clean rung says what it points at, %r" % (rows.get("ladder glyphs managed-whole"),))
check("ladder glyphs none" in rows and "no-texbuf" in rows["ladder glyphs none"][1], "a rung with no live window is named, %r" % (rows.get("ladder glyphs none"),))
check("ladder glyphs control" not in rows, "the control stayed broken: no control row")
# the control clean = the run is not comparable
g2 = dict(glyphs, paranoid={"verdict": "clean", "text": "CLEAN: 20 of 20 tick lines have a twin (100 %)"})
rows = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(P3, 3, (), g2)}
check(rows["ladder glyphs control"][0] == "WARN" and "not comparable" in rows["ladder glyphs control"][1], "a clean negative control is a WARN, %r" % (rows.get("ladder glyphs control"),))
# nothing cleans the text
g3 = dict(glyphs, **{"managed-whole": {"verdict": "broken", "text": "BROKEN: 3 of 22 tick lines have a twin (14 %)"}})
rows = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(P3, 3, (), g3)}
check("no rung made them clean" in rows["ladder glyphs"][1] and "no ENGAGED knob of this glyph hunt is the cure (engaged: managed-whole" in rows["ladder glyphs"][1] and "undeclared writes" in rows["ladder glyphs"][1],
      "a hunt with no cure says what is engaged and what to read next, %r" % (rows["ladder glyphs"],))

# --- every rung's Terminal must have logged (the baseline too); an empty slice is a WARN with or without a conf
P = fresh_rungs(baseline="", **{"managed-pages": term_log("managed-pages", pid=555)})
P["paranoid"]["terminal_pid"] = None
rows = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(P, 3, (), glyphs)}
check(rows["ladder baseline"][0] == "WARN" and "no plugin log slice for this rung" in rows["ladder baseline"][1], "an empty baseline slice is a WARN although the baseline sets nothing, %r" % (rows["ladder baseline"],))
check(rows["ladder managed-pages"][0] == "WARN" and "Terminal pid 907 logged nothing" in rows["ladder managed-pages"][1], "a slice with other pids but not the Terminal's is a WARN, %r" % (rows["ladder managed-pages"],))
check("Terminal did not start" in rows["ladder paranoid"][1], "no Terminal pid: said")

# --- the engaged predicate: a key that reached the plugin can still be a no-op
def eng(name, **over):
    P = fresh_rungs(); P[name]["log"] = term_log(name, **over)
    return {n: (l, t) for l, n, t in tg.ladder_rows(P, 3, (), glyphs)}["ladder " + name]
for name, over, why in (("managed-whole", dict(widened=0), "no upload was widened"), ("managed-pages", dict(pages=0), "pages flag is 0"), ("managed-check", dict(scans=0), "scanned nothing"),
                        ("fbfetch-zero", dict(forced=0), "zr_forced=0"), ("dontcare-clears", dict(dcc=0), "no render pass attachment had a don't-care"),
                        ("paranoid", dict(para=0), None)):
    if name == "paranoid":
        continue
    lvl, t = eng(name, **over)
    check(lvl == "WARN" and "KNOB NOT ENGAGED" in t and why in t, "%s without the counter that proves it: KNOB NOT ENGAGED (%s), %r" % (name, why, t[-250:]))
# the effective-state lines: a feature that is already off in the baseline, a knob that did not take
P = fresh_rungs(); P["baseline"]["log"] = term_log("baseline", share="private per process")
r_ = {n: (l, t) for l, n, t in tg.ladder_rows(P, 3, (), glyphs)}
check("KNOB NOT ENGAGED" in r_["ladder no-surface-share"][1] and "already 'private per process' in the baseline" in r_["ladder no-surface-share"][1], "sharing already off in the baseline, %r" % (r_["ladder no-surface-share"],))
P = fresh_rungs(); P["baseline"]["log"] = term_log("baseline", tex="OFF", bind="OFF", zero="memset their CPU copy at creation and upload it (old)")
r_ = {n: (l, t) for l, n, t in tg.ladder_rows(P, 3, (), glyphs)}
check(all("KNOB NOT ENGAGED" in r_["ladder " + n][1] for n in ("no-texbuf", "no-bindless", "managed-eager-zero")), "features absent in the baseline: not engaged")
P = fresh_rungs(); P["skip-empty-0"]["log"] = term_log("skip-empty-0", skip="are NOT submitted")
check("KNOB NOT ENGAGED" in {n: t for l, n, t in tg.ladder_rows(P, 3, (), glyphs)}["ladder skip-empty-0"] and "did not take" in {n: t for l, n, t in tg.ladder_rows(P, 3, (), glyphs)}["ladder skip-empty-0"], "skip-empty=0 that did not take")
P = fresh_rungs(); P["baseline"]["log"] = P["baseline"]["log"].replace("commit: 3 empty submission parts NOT submitted", "commit: 0 empty")
check("never had an empty submission part" in {n: t for l, n, t in tg.ladder_rows(P, 3, (), glyphs)}["ladder skip-empty-0"], "no empty part in the baseline: the knob changes nothing")
P = fresh_rungs(); P["fbfetch-zero"]["log"] = "".join(l for l in P["fbfetch-zero"]["log"].splitlines(True) if "fbfetch" not in l)
check("engagement could not be shown" in {n: t for l, n, t in tg.ladder_rows(P, 3, (), glyphs)}["ladder fbfetch-zero"], "a missing counter line: unknown, said")
# a clean rung whose knob did not engage is not a discriminator; the "no cure" sentence leaves such rungs out
P = fresh_rungs(); P["no-bindless"]["log"] = term_log("no-bindless", bind="ON")
g4 = dict(glyphs, **{"no-bindless": {"verdict": "clean", "text": "CLEAN: 20 of 20 tick lines have a twin (100 %)"}, "managed-whole": {"verdict": "broken", "text": "BROKEN: 3 of 22 tick lines have a twin (14 %)"}})
r_ = {n: (l, t) for l, n, t in tg.ladder_rows(P, 3, (), g4)}
check(r_["ladder glyphs no-bindless"][0] == "WARN" and "NOT shown to be engaged" in r_["ladder glyphs no-bindless"][1] and "CLEAN with" not in r_["ladder glyphs"][1] and "did NOT engage" in r_["ladder glyphs"][1],
      "clean without engagement is not a cure, %r / %r" % (r_.get("ladder glyphs no-bindless"), r_["ladder glyphs"]))

# the glyphs of a rung that has a hires look must always be judged or WARNed
P = fresh_rungs()
r_ = {n: (l, t) for l, n, t in tg.ladder_rows(P, 3, (), {})}
check("the glyphs were not judged" in r_["ladder baseline"][1] and r_["ladder baseline"][0] == "WARN", "no verdict for a rung with a hires look is a WARN, %r" % (r_["ladder baseline"],))
gnone = {"baseline": {"verdict": "none", "text": "the glyphs were NOT judged: no hires look file next to the report (x)"}}
r_ = {n: (l, t) for l, n, t in tg.ladder_rows(P, 3, (), gnone)}
check("the glyphs were NOT judged: no hires look file" in r_["ladder baseline"][1] and r_["ladder baseline"][0] == "WARN", "a verdict of 'not judged' is a WARN on the rung")

# ladder.txt lines outside the rungs reach the rows: an abort by a signal, ppm files that were not sent; the looks' exit codes
txt = ("# rdna3 ladder v1\nvariant: 3 (ladder-glyphs)\nWARN: a tick window existed before the run\n== rung baseline\nscope: apps\nconf: (nothing)\npids: Terminal=907 Chess=908 \n"
       "-- look ladder-baselineh (hires, 12:00:00)\nexit: 124\nlooks: ladder-baselinea ladder-baselineh\nlog: baseline.log (10 lines, pids 907 908, slice truncated: 600000 bytes kept)\n== end rung baseline\n"
       "push: 3 ppm file(s) not sent to the host (curl failed): their rungs have no glyph verdict\nABORTED by SIGTERM at rung managed-whole\n")
v, rr, pb = tg.parse_ladder(txt, {})
check(any("tick window existed before the run" in x for x in pb) and any("ABORTED by SIGTERM at rung managed-whole" in x for x in pb) and any("3 ppm file(s) not sent" in x for x in pb), "global ladder.txt lines reach the problems, %r" % (pb,))
check(any("exited 124" in w for w in rr["baseline"]["warn"]) and any("TRUNCATED" in w for w in rr["baseline"]["warn"]) and rr["baseline"]["terminal_pid"] == 907, "a look's exit code and a truncated slice are warned, %r" % (rr["baseline"],))
# the verdict text lists the pipes
# a rung's own detector result rides on its row
P3["managed-check"]["log"] = P3["managed-check"]["log"] + L("Terminal", 907, "managed: SUMMARY check=1 snapshot=0 flushes=40 uploads=80 prologue_uploads=80 sync_uploads=0 checked=80 changed_exact=0 changed_env=0 check_skipped=0 snapped=0 "
                                    "snap_failed=0 snap_skipped_big=0 snap_skipped_noshadow=0 whole=0 pages=0 decl_bytes=9000 up_bytes=9000 widened_uploads=0 ud_scans=80 ud_idle_scans=300 ud_hits=3 ud_idle_hits=1 "
                                    "ud_bytes=60 ud_samepage_hits=2 ud_late_bytes=10 ud_buffers=1 ud_gpu_off=0 ud_nomirror=0 ud_mirrors=3 ud_mirror_bytes=786432")
rows = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(P3, 3, (), glyphs)}
check("undeclared writes: Terminal[907]: the CPU wrote managed-buffer bytes it did NOT declare" in rows["ladder managed-check"][1], "the rung row carries the detector, %r" % (rows["ladder managed-check"],))

# the detector rows themselves
def mgsum(**kw):
    base = dict(check=1, snapshot=0, flushes=40, uploads=80, prologue_uploads=80, sync_uploads=0, checked=80, changed_exact=0, changed_env=0, check_skipped=0, snapped=0, snap_failed=0, snap_skipped_big=0,
                snap_skipped_noshadow=0, whole=0, pages=0, decl_bytes=9000, up_bytes=9000, widened_uploads=0, ud_scans=80, ud_idle_scans=300, ud_hits=0, ud_idle_hits=0, ud_bytes=0, ud_samepage_hits=0,
                ud_late_bytes=0, ud_buffers=0, ud_gpu_off=0, ud_nomirror=0, ud_mirrors=3, ud_mirror_bytes=786432)
    base.update(kw)
    return L("Terminal", 907, "managed: SUMMARY " + " ".join("%s=%d" % kv for kv in base.items()))
r = rows_of(mgsum())
check(r["undeclared writes"][0][0] == "INFO" and "not supported for them" in r["undeclared writes"][0][1] and "80 flush scan(s) + 300 idle" in r["undeclared writes"][0][1], "no undeclared write: INFO, %r" % (r.get("undeclared writes"),))
r = rows_of(mgsum(ud_hits=4, ud_idle_hits=1, ud_bytes=96, ud_samepage_hits=3, ud_late_bytes=16, ud_buffers=2, ud_nomirror=1, ud_gpu_off=2))
t_ = r["undeclared writes"][0]
check(t_[0] == "WARN" and "96 changed byte(s) in 2 buffer(s)" in t_[1] and "16 of them declared at the NEXT flush" in t_[1] and "80 never" in t_[1] and "3 hit(s) in a page" in t_[1]
      and "1 buffer(s) had no mirror" in t_[1] and "2 buffer(s) the GPU writes back" in t_[1] and "NVMTL_MANAGED_WHOLE" in t_[1], "undeclared writes: WARN with the numbers, %r" % (t_,))
r = rows_of(mgsum(ud_scans=0, ud_idle_scans=0))
check(r["undeclared writes"][0][0] == "INFO" and "no managed buffer was scanned" in r["undeclared writes"][0][1], "nothing scanned, %r" % (r.get("undeclared writes"),))
old_mg = L("Terminal", 907, "managed: SUMMARY check=1 snapshot=0 flushes=4 uploads=8 prologue_uploads=8 sync_uploads=0 checked=8 changed_exact=0 changed_env=0 check_skipped=0")
check(rows_of(old_mg)["undeclared writes"][0][0] == "WARN" and "older plugin" in rows_of(old_mg)["undeclared writes"][0][1], "an older plugin's SUMMARY with the check on says the detector did not run")
r = rows_of(mgsum(check=0, whole=1, decl_bytes=1000, up_bytes=262144, widened_uploads=70))
check(r["managed upload"][0][0] == "WARN" and "NVMTL_MANAGED_WHOLE is on" in r["managed upload"][0][1] and "70 of 80 uploads widened" in r["managed upload"][0][1], "a widened upload is a WARN, %r" % (r.get("managed upload"),))
check("undeclared writes" not in rows_of(mgsum(check=0, ud_scans=0, ud_idle_scans=0)), "the detector off: no row")
# buffers the detector could not watch: the label says what was missed and the wording is softened when most were blind
r = rows_of(mgsum(ud_managed=10, ud_mirrors=2, ud_nomirror=1))
t_ = r["undeclared writes"][0][1]
check("7 of the 10 managed buffers made in this process were never watched" in t_ and "NOT excluded" in t_, "an unwatched majority softens the verdict, %r" % (t_,))
r = rows_of(mgsum(ud_managed=3, ud_mirrors=3))
check("not supported for them" in r["undeclared writes"][0][1] and "never watched" not in r["undeclared writes"][0][1], "all watched: the plain verdict")
r = rows_of(mgsum(check=0, whole=1, ud_scans=5, ud_idle_scans=5, ud_whole_marks=7, widened_uploads=7))
check("7 buffer(s) the app wrote WITHOUT any didModifyRange were uploaded whole" in r["managed upload"][0][1] and "undeclared writes" in r, "WHOLE alone: the scans still report")

# the same-image copy counters
ABS = "vk: ab: SUMMARY load_colour_all=0 wit_early=0 wit_ok=2 wit_blind=0 wit_worst_us=0 same_image_copies=%d same_image_overlap=%d same_image_general=%d same_image_tmp=%d same_image_tmpfail=%d same_image_unsafe=%d"
r = rows_of(L("Terminal", 907, ABS % (30, 22, 8, 22, 0, 0)))
check(r["same-image copy"][0][0] == "INFO" and "30 copy(ies)" in r["same-image copy"][0][1] and "22 overlapping, 22 of them through a scratch image" in r["same-image copy"][0][1], "same-image copies: INFO, %r" % (r.get("same-image copy"),))
r = rows_of(L("Terminal", 907, ABS % (30, 22, 8, 19, 1, 2)))
check(r["same-image copy"][0][0] == "WARN" and "3 overlapping copy(ies) could NOT use the scratch image" in r["same-image copy"][0][1], "a copy that could not use the scratch image is a WARN, %r" % (r.get("same-image copy"),))
check("same-image copy" not in rows_of(L("Terminal", 907, ABS % (0, 0, 0, 0, 0, 0))), "no same-image copy: no row")

# the surface census
CEN = ("census: SUMMARY seq=4 t_ms=41234 surfaces=2 overflow=0 sin_up_vram=1 sin_up_priv=0 sin_up_mb=0 sin_skip=4 sin_fail=0 pass=40 pass_clear=2 draws=90 draw_blit=40 draw_fbf=22 blitenc=0 replace=0 "
       "compute=0 present=0 out_dirty=38 out_copy=0 out_fail=0 top=325:640x353:V:u1,s4,P40,c2,D90,B40,F22,O38;7:100x50:P:R1")
r = rows_of(L("Terminal", 907, "census: SUMMARY seq=1 t_ms=1000 surfaces=1 overflow=0 sin_up_vram=0 sin_up_priv=0 sin_up_mb=0 sin_skip=0 sin_fail=0 pass=1 pass_clear=0 draws=0 draw_blit=0 draw_fbf=0 blitenc=0 replace=0 compute=0 present=0 out_dirty=0 out_copy=0 out_fail=0 top=325:1x1:V:P1")
            + L("Terminal", 907, CEN))
t_ = r["surface census"]
check(len(t_) == 1 and t_[0][0] == "INFO" and "SUMMARY #4 after 41 s" in t_[0][1] and "render passes 40 (2 cleared), draws 90 (40 buffer_image_blit, 22 framebuffer fetch)" in t_[0][1]
      and "325:640x353:V:u1,s4,P40,c2,D90,B40,F22,O38" in t_[0][1] and "38 dirty mark(s)" in t_[0][1], "the census row: the newest line, the numbers and the busiest surface, %r" % (t_,))
r = rows_of(L("Terminal", 907, CEN.replace("sin_fail=0", "sin_fail=3")))
check(r["surface census"][0][0] == "WARN", "a failed surface-in is a WARN")

# baseline clean / nothing found
rows = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(P3, 3, (), {"baseline": {"verdict": "clean", "text": "CLEAN: 20 of 20 tick lines have a twin (100 %)"}})}
check("did not show" in rows["ladder glyphs"][1], "a clean baseline: the other rungs prove nothing, %r" % (rows["ladder glyphs"],))
rows = {name: (lvl, text) for lvl, name, text in tg.ladder_rows(P3, 3, (), {"baseline": {"verdict": "none", "text": "no stack of >= 8 tick lines in the looks"}})}
check(rows["ladder glyphs"][0] == "WARN" and "no tick lines" in rows["ladder glyphs"][1], "no tick lines anywhere is a WARN, %r" % (rows["ladder glyphs"],))
check("ladder glyphs" not in {name for lvl, name, text in tg.ladder_rows(P3, 3, (), None)}, "no looks, no glyph row")
# glyph_verdicts: the files next to the archive, a rung with no look is left out, a bad file is a `none`, never an exception
import tempfile  # noqa: E402
td = tempfile.mkdtemp(prefix="tgg")
open(os.path.join(td, "r-scanoutladder-baselineh-pipe0.ppm"), "wb").write(b"P6\n3 3\n255\n" + bytes(27))
gv = tg.glyph_verdicts({"baseline": {"looks": ["ladder-baselinea", "ladder-baselineh"]}, "radv": {"looks": ["ladder-radva", "ladder-radvh"]}, "sync": {"looks": []}}, os.path.join(td, "r"))
check(list(gv) == ["baseline", "radv"] and gv["baseline"]["verdict"] == "none" and "no look could be read" in gv["baseline"]["text"], "glyph_verdicts: a bad file is none, %r" % (gv,))
check(gv["radv"]["verdict"] == "none" and "NOT judged: no hires look file next to the report" in gv["radv"]["text"], "glyph_verdicts: a rung with an h look and no file is none WITH the reason, never left out, %r" % (gv["radv"],))
gv0 = tg.glyph_verdicts({"baseline": {"looks": ["ladder-baselinea", "ladder-baselineh"]}}, None)
check(gv0["baseline"]["verdict"] == "none" and "not given the report as <name>.tar.gz" in gv0["baseline"]["text"], "glyph_verdicts: a report that is not a .tar.gz says why nothing was judged, %r" % (gv0,))
# a rung with an h2 look is judged on the second look as well (the glyph hunt): a missing second look is said, not guessed
open(os.path.join(td, "r-scanoutladder-managed-wholeh-pipe0.ppm"), "wb").write(b"P6\n3 3\n255\n" + bytes(27))
gv = tg.glyph_verdicts({"managed-whole": {"looks": ["ladder-managed-wholea", "ladder-managed-wholeh", "ladder-managed-wholeh2"]}}, os.path.join(td, "r"))
check(gv["managed-whole"]["verdict"] == "none", "glyph_verdicts: a rung with an h2 look and no readable files is none, %r" % (gv,))

# the hook in triage_reports.py
src = open(os.path.join(ROOT, "tools", "triage_reports.py")).read()
check("from triage_glyphs import n50_plugin_rows" in src and "n50_plugin_rows(" in src, "triage_reports.py calls n50_plugin_rows")
check("tools/triage_glyphs.py is missing" in src, "triage_reports.py says so loudly when the module is missing")
check("41-ladder.txt" in src and "parse_ladder" in src and "ladder_rows(" in src, "triage_reports.py reads the archive's 41-ladder members")
check("glyph_verdicts(lr_," in src and "endswith('.tar.gz')" in src.replace('"', "'"), "triage_reports.py hands the rungs' looks to the glyph check")
check("except Exception as e_:" in src[src.index("parse_ladder, ladder_rows"):][:900] and "ladder_rows_ = [(\"WARN\", \"ladder\", \"tools/triage_glyphs.py FAILED" in src, "triage_reports.py survives any error of the ladder module with a loud WARN")
check("n50_rows_ = [(\"WARN\", \"plugin A/B\", \"tools/triage_glyphs.py FAILED" in src, "triage_reports.py survives any error of n50_plugin_rows with a loud WARN")

if fails:
    print("FAIL: %d check(s)" % len(fails))
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("ok: tools/triage_glyphs.py")
