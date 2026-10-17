"""common/rdna3_trace.h's SWAP constants and tools/rdna3_trace.py's must be
the same values (ECC review round 1 of the stale back, card run
2026-10-17c): every RD3_TRV1_<name> bit is the decoder's V1_<name>, every
rd3_trace_swap value an arg the decoder names (RD3_TRS_HOLD "hold",
RD3_TRS_COPY "copy"), RD3_TRV4_LANDED_A1 its LANDED_A1, and the trace
version its VERSION. Run: python3 -I -B tests/test_trace_parity.py"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rdna3_trace  # noqa: E402

h = open(os.path.join(ROOT, "common", "rdna3_trace.h")).read()
fails = []
bits = dict((n, 1 << int(b)) for n, b in re.findall(r"^#define RD3_TRV1_(\w+)\s+\(1u << (\d+)\)", h, re.M))
if len(bits) < 4:
    fails.append("only %d RD3_TRV1_* bits parsed from the header" % len(bits))
for name, val in sorted(bits.items()):
    py = getattr(rdna3_trace, "V1_" + name, None)
    if py != val:
        fails.append("RD3_TRV1_%s = 0x%x, the decoder's V1_%s = %s" % (name, val, name,
                                                                      "missing" if py is None else hex(py)))
if len(set(bits.values())) != len(bits):
    fails.append("two RD3_TRV1_* bits share a value: %s" % bits)
m = re.search(r"enum rd3_trace_swap \{(.*?)\};", h, re.S)
trs = dict((n, int(v)) for n, v in re.findall(r"RD3_TRS_(\w+)\s*=\s*(\d+)", m.group(1) if m else ""))
if set(trs.values()) != set(rdna3_trace.SWAP_ARG):
    fails.append("rd3_trace_swap values %s, the decoder's SWAP_ARG keys %s" % (sorted(trs.values()),
                                                                               sorted(rdna3_trace.SWAP_ARG)))
for name, word in (("HOLD", "hold"), ("COPY", "copy"), ("SKIP", "skip"), ("FLIP", "flip")):
    if rdna3_trace.SWAP_ARG.get(trs.get(name)) != word:
        fails.append("RD3_TRS_%s = %s is '%s' in the decoder, not '%s'" % (name, trs.get(name),
                                                                          rdna3_trace.SWAP_ARG.get(trs.get(name)), word))
a1 = re.search(r"#define RD3_TRV4_LANDED_A1 0x([0-9a-f]+)u", h)
if not a1 or int(a1.group(1), 16) != rdna3_trace.LANDED_A1:
    fails.append("RD3_TRV4_LANDED_A1 %s, the decoder's LANDED_A1 0x%x" % (a1 and a1.group(1), rdna3_trace.LANDED_A1))
ver = re.search(r"#define RD3_TRACE_VERSION\s+(\d+)u", h)
if not ver or int(ver.group(1)) != rdna3_trace.VERSION:
    fails.append("RD3_TRACE_VERSION %s, the decoder's VERSION %d" % (ver and ver.group(1), rdna3_trace.VERSION))
# the write generations' reason field (card run 2026-10-17d) and the flip-buffer event's outcomes
sh = re.search(r"#define RD3_TRV1_GEN_WHY_SHIFT (\d+)u", h)
mk = re.search(r"#define RD3_TRV1_GEN_WHY_MASK\s+(\d+)u", h)
if not sh or int(sh.group(1)) != rdna3_trace.V1_GEN_WHY_SHIFT or not mk or int(mk.group(1)) != rdna3_trace.V1_GEN_WHY_MASK:
    fails.append("RD3_TRV1_GEN_WHY_SHIFT / _MASK %s / %s, the decoder's %d / %d" % (
        sh and sh.group(1), mk and mk.group(1), rdna3_trace.V1_GEN_WHY_SHIFT, rdna3_trace.V1_GEN_WHY_MASK))
wg = open(os.path.join(ROOT, "common", "rdna3_write_gen.h")).read()
m2 = re.search(r"enum rd3_wg_why \{(.*?)\};", wg, re.S)
why = dict((int(v), n) for n, v in re.findall(r"RD3_WGW_(\w+)\s*=\s*(\d+)", m2.group(1) if m2 else ""))
if sorted(why) != sorted(rdna3_trace.GEN_WHY) or max(why) > rdna3_trace.V1_GEN_WHY_MASK:
    fails.append("enum rd3_wg_why %s, the decoder's GEN_WHY %s" % (sorted(why), sorted(rdna3_trace.GEN_WHY)))
m3 = re.search(r"enum rd3_trace_flipbuf \{(.*?)\};", h, re.S)
fbw = sorted(int(v) for v in re.findall(r"RD3_TRFB_\w+\s*=\s*(\d+)", m3.group(1) if m3 else ""))
if fbw != sorted(rdna3_trace.FLIPBUF_WHAT):
    fails.append("enum rd3_trace_flipbuf %s, the decoder's FLIPBUF_WHAT %s" % (fbw, sorted(rdna3_trace.FLIPBUF_WHAT)))
types = dict((int(v), n) for n, v in re.findall(r"RD3_TR_(\w+)\s*=\s*(\d+),", h))
for v, n in sorted((v, n) for v, n in types.items() if v):
    if rdna3_trace.TYPES.get(v) != n:
        fails.append("RD3_TR_%s = %d, the decoder's TYPES[%d] = %s" % (n, v, v, rdna3_trace.TYPES.get(v)))
# the bits the decoder uses that the header documents only in prose (the SDMA breaker, 1 << 20)
if rdna3_trace.V1_BRK_FLIP != 1 << 20 or "1 << 20 when modes 1/2" not in h:
    fails.append("the SDMA breaker's SWAP v1 bit is not 1 << 20 in both")
if fails:
    sys.exit("test_trace_parity: FAIL " + "; ".join(fails))
print("test_trace_parity: all checks passed (%d v1 bits, %d swap args)" % (len(bits), len(trs)))
