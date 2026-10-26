"""Triage rows of DRIVER-PLAN N57 (card run 2026-10-26g: Terminal's glyphs break at draw time, a contiguous run of 18 instances of one draw gets
garbage varyings). Hypothesis: the GFX11 attribute ring, a DISCARDABLE VRAM BO that RADV declares with SPI_ATTRIBUTE_RING_SIZE.BIG_PAGE = 1 (the SPI
translates it in 64 KiB pages). The kext logs every such BO where it lands (`GEM DISCARDABLE #n: ...`, RDNA3Accel::discardableNote) and how it is
mapped (`GEM_VA DISCARDABLE #n: ...`, RDNA3Bringup::discardableMapNote); the plugin logs whether RADV got RADV_ATTR_RING_BIG_PAGE=0 (the ladder's
`attrring` rung, `vk: RADV_ATTR_RING_BIG_PAGE=...`). The verdict is recomputed here from the numbers (common/rdna3_attr_ring.h): VRAM, a 64 KiB
aligned VA congruent with the physical address modulo 64 KiB, and a PTE fragment of at least 64 KiB (4); anything else is FAIL.
Kept in a module of its own so that tools/triage_reports.py only gains one call; tests/test_triage_attr_ring.py."""
import re

BIGPAGE = 64 << 10
BIGPAGE_FRAG = 4

_CREATE = re.compile(r"GEM DISCARDABLE #(\d+): (\d+) KiB, alignment (\d+) KiB, (.*?)(?: at 0x([0-9a-f]+) \(64 KiB aligned (yes|no), 2 MiB aligned (yes|no)\)| - WRONG)")
_MAP_VRAM = re.compile(r"GEM_VA DISCARDABLE #(\d+): VA 0x([0-9a-f]+) \+ (\d+) KiB -> contiguous VRAM 0x([0-9a-f]+) \(PA 0x([0-9a-f]+)\), PTE fragment (\d+) "
                       r"\(\d+ KiB, ([^)]*)\): BIG_PAGE (ok|WRONG:.*)$")
_MAP_GTT = re.compile(r"GEM_VA DISCARDABLE #(\d+): VA 0x([0-9a-f]+) \+ (\d+) KiB -> GTT system pages")
_PLUG = re.compile(r'^vk: RADV_ATTR_RING_BIG_PAGE=(unset|"(?:[^"\\]|\\.)*") \(NVMTL_ATTR_RING_BIG_PAGE=(unset|"(?:[^"\\]|\\.)*")\)')
_PLUG_REJ = re.compile(r'^vk: NVMTL_ATTR_RING_BIG_PAGE=("(?:[^"\\]|\\.)*") REJECTED: (.*)$')


def bigpage_problems(vram, va, pa, frag):
    """common/rdna3_attr_ring.h rdna3_bigpage_problems, as words"""
    p = []
    if not vram:
        p.append("system pages")
    if frag < BIGPAGE_FRAG:
        p.append("fragment below 64 KiB")
    if va % BIGPAGE:
        p.append("VA not 64 KiB aligned")
    if vram and (va - pa) % BIGPAGE:
        p.append("VA and PA not congruent modulo 64 KiB")
    return p


def parse(klog):
    """(creates, maps): creates [{n, kib, align_kib, where, vram, off}], maps [{n, va, kib, vram, off, pa, frag, how, kernel}]"""
    creates, maps = [], []
    for line in (klog or "").splitlines():
        m = _CREATE.search(line)
        if m:
            vram = m.group(5) is not None
            creates.append(dict(n=int(m.group(1)), kib=int(m.group(2)), align_kib=int(m.group(3)), where=m.group(4), vram=vram,
                                off=int(m.group(5), 16) if vram else None))
            continue
        m = _MAP_VRAM.search(line)
        if m:
            maps.append(dict(n=int(m.group(1)), va=int(m.group(2), 16), kib=int(m.group(3)), vram=True, off=int(m.group(4), 16),
                             pa=int(m.group(5), 16), frag=int(m.group(6)), how=m.group(7), kernel=m.group(8)))
            continue
        m = _MAP_GTT.search(line)
        if m:
            maps.append(dict(n=int(m.group(1)), va=int(m.group(2), 16), kib=int(m.group(3)), vram=False, off=None, pa=0, frag=0,
                             how="4 KiB system pages", kernel="WRONG"))
    return creates, maps


def plugin_switch(pluginlog):
    """({process[pid]: RADV_ATTR_RING_BIG_PAGE value or None}, [rejections])"""
    from triage_glyphs import _lines   # the plugin's log line format (process names may contain blanks)
    got, rej = {}, []
    for proc, pid, _tid, msg in _lines(pluginlog):
        m = _PLUG.match(msg)
        if m:
            got["%s[%d]" % (proc, pid)] = None if m.group(1) == "unset" else m.group(1)[1:-1]
            continue
        m = _PLUG_REJ.match(msg)
        if m:
            rej.append("%s (%s[%d]): %s" % (m.group(1), proc, pid, m.group(2))[:160])
    return got, rej


def attr_ring_rows(klog="", pluginlog=""):
    """[(level, name, text)]: `attribute ring` (where the DISCARDABLE BOs landed and how they are mapped) and `attr ring switch` (RADV_ATTR_RING_BIG_PAGE
    in the plugin's processes)."""
    rows = []
    creates, maps = parse(klog)
    sw, rej = plugin_switch(pluginlog)
    off = sorted(k for k, v in sw.items() if v == "0")
    on = sorted(k for k, v in sw.items() if v != "0")
    note = ""
    if off:
        note = "; BIG_PAGE was OFF (RADV_ATTR_RING_BIG_PAGE=0, the `attrring` rung) in %s: their rings do not depend on this mapping" % ", ".join(off)[:200]
    if sw:
        rows.append(("WARN" if off else "INFO", "attr ring switch",
                     ("RADV_ATTR_RING_BIG_PAGE=0 in %s - this run does not show the default driver there%s" % (", ".join(off)[:240], ("; default (BIG_PAGE on) in " + ", ".join(on)[:200]) if on else ""))
                     if off else "BIG_PAGE as upstream (RADV_ATTR_RING_BIG_PAGE unset) in %s" % ", ".join(on)[:300]))
    for r in rej[:3]:
        rows.append(("WARN", "attr ring switch", "NVMTL_ATTR_RING_BIG_PAGE=%s: the switch did not reach RADV" % r))
    if not creates and not maps:
        if (klog or "").strip():
            rows.append(("WARN", "attribute ring", "no `GEM DISCARDABLE` / `GEM_VA DISCARDABLE` line in the kernel log: a kext older than DRIVER-PLAN N57, or no RADV queue "
                         "made its GFX11 attribute ring (no client drew); the ring's placement and PTEs are UNKNOWN"))
        return rows
    gtt = [c for c in creates if not c["vram"]]
    bad = []
    for m in maps:
        p = bigpage_problems(m["vram"], m["va"], m["pa"], m["frag"])
        if p:
            bad.append((m, p))
    good = [m for m in maps if not bigpage_problems(m["vram"], m["va"], m["pa"], m["frag"])]
    first = (bad[0][0] if bad else (maps[0] if maps else None))
    desc = ""
    if first:
        desc = " (e.g. #%d: VA 0x%x + %d KiB -> %s, PTE fragment %d = %d KiB, %s)" % (
            first["n"], first["va"], first["kib"], ("VRAM 0x%x, PA 0x%x" % (first["off"], first["pa"])) if first["vram"] else "GTT",
            first["frag"], 4 << first["frag"], first["how"])
    cdesc = ", ".join(sorted({"%d KiB align %d KiB in %s" % (c["kib"], c["align_kib"], c["where"]) for c in creates}))[:240]
    if gtt:
        rows.append(("FAIL", "attribute ring", "%d DISCARDABLE BO(s) landed in GTT (%s): BIG_PAGE translates the attribute ring in 64 KiB pages, system pages are "
                     "4 KiB and scattered - garbage varyings (DRIVER-PLAN N57)%s" % (len(gtt), ", ".join(sorted({c["where"] for c in gtt})), note)))
    if bad:
        why = sorted({w for _m, p in bad for w in p})
        rows.append(("FAIL", "attribute ring", "%d of %d DISCARDABLE mapping(s) do NOT hold what SPI_ATTRIBUTE_RING_SIZE.BIG_PAGE assumes: %s%s; created: %s - "
                     "amdgpu maps the ring with fragments >= 64 KiB (amdgpu_vm_pte_fragment); a clean `attrring` rung confirms it, the fix is the kernel's "
                     "placement / PTE fragments (DRIVER-PLAN N57)%s" % (len(bad), len(maps), ", ".join(why), desc, cdesc or "no create line", note)))
    elif maps and not gtt:
        rows.append(("PASS", "attribute ring", "%d DISCARDABLE mapping(s), all contiguous VRAM, 64 KiB aligned and congruent, PTE fragment >= 64 KiB%s; created: %s%s" % (
            len(good), desc, cdesc or "no create line", note)))
    if not maps:
        rows.append(("WARN", "attribute ring", "%d DISCARDABLE BO(s) created (%s) but no `GEM_VA DISCARDABLE` line: the mapping and its PTE fragment are UNKNOWN "
                     "(the log lost it, or the VM is record-only)" % (len(creates), cdesc)))
    disagree = [m["n"] for m in maps if (m["kernel"] == "ok") != (not bigpage_problems(m["vram"], m["va"], m["pa"], m["frag"]))]
    if disagree:
        rows.append(("WARN", "attribute ring", "the kernel's own BIG_PAGE verdict differs from the triage's for #%s: common/rdna3_attr_ring.h and tools/triage_attr_ring.py disagree"
                     % ", #".join(str(x) for x in disagree[:6])))
    return rows
