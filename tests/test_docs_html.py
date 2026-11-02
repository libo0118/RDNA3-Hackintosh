"""The HTML working docs (docs/DRIVER-PLAN.html, AFTER-TESTS.html, CODEMAP.html and every other docs/*.html) stay
machine-readable: tools/docrows.py checks tag balance, unique ids, links between the pages (file and #anchor) and the
shape of every DRIVER-PLAN N row (id nNN, data-n, data-status, the badge that matches it). This test runs that check on
the repository and tests the checker itself on made-up pages. Run: python3 -I -B tests/test_docs_html.py"""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import docrows  # noqa: E402

fails = []


def expect(cond, what):
    if not cond:
        fails.append(what)


# 1. the repository's own pages
problems = ["DRIVER-PLAN.html: " + m for m in docrows.check_plan_text(open(docrows.PLAN, encoding="utf-8").read())]
problems += ["%s: %s" % p for p in docrows.lint_pages()]
for p in problems[:20]:
    fails.append(p)

plan = open(docrows.PLAN, encoding="utf-8").read()
rows = docrows.plan_rows(plan)
expect(len(rows) >= 76, "DRIVER-PLAN.html has only %d N rows" % len(rows))
expect('id="n-rows"' in plan, "DRIVER-PLAN.html lost the n-rows container")
expect(docrows.fix_plan_text(plan) == plan, "DRIVER-PLAN.html: a badge does not match its data-status (tools/docrows.py fix)")
for f in ("AFTER-TESTS.html", "CODEMAP.html"):
    expect(os.path.exists(os.path.join(ROOT, "docs", f)), "docs/%s is missing" % f)
for f in ("AFTER-TESTS.md", "CODEMAP.md", "DRIVER-PLAN.md"):
    expect(not os.path.exists(os.path.join(ROOT, "docs", f)), "docs/%s must not come back (the docs are HTML)" % f)

# 2. the row snippet an agent copies is itself valid, and next/fix behave
n = docrows.next_n(plan)
snip = docrows.snippet(n, "S")
expect(docrows.check_plan_text('<div class="rows" id="n-rows">\n' + snip + '</div>') == [],
       "the documented N row snippet fails the row check: %s" % docrows.check_plan_text(snip))
expect(docrows.next_n(snip) == n + 1, "next_n does not follow the highest row")
bad = snip.replace('data-status="open"', 'data-status="done"')
expect(any("badge says" in m for m in docrows.check_plan_text(bad)), "a badge that disagrees with data-status is not reported")
expect(docrows.check_plan_text(docrows.fix_plan_text(bad)) == [], "fix does not repair the badge")
expect(any("duplicate" in m for m in docrows.check_plan_text(snip + snip)), "a duplicate N row id is not reported")
expect(any("data-n" in m for m in docrows.check_plan_text(snip.replace('data-n="%d"' % n, 'data-n="1"'))), "a wrong data-n is not reported")
expect(any("data-status" in m for m in docrows.check_plan_text(snip.replace('data-status="open"', 'data-status="stuck"'))),
       "an unknown data-status is not reported")

# 3. the page linter on made-up pages
with tempfile.TemporaryDirectory() as d:
    def w(name, text):
        with open(os.path.join(d, name), "w") as f:
            f.write(text)
    w("a.html", '<h2 id="x">x</h2><a href="b.html#y">ok</a><a href="b.html#nope">bad</a><a href="gone.html">gone</a>'
                '<p id="x">dup</p><ul><li>open</ul>')
    w("b.html", '<h2 id="y">y</h2>')
    msgs = [m for _, m in docrows.lint_pages(d)]
    expect(any("missing anchor: b.html#nope" in m for m in msgs), "a dangling cross-page anchor is not reported: %s" % msgs)
    expect(any("missing file: gone.html" in m for m in msgs), "a link to a missing page is not reported")
    expect(any("duplicate id 'x'" in m for m in msgs), "a duplicate id is not reported")
    expect(any("does not close" in m or "unclosed" in m for m in msgs), "unbalanced tags are not reported")
    expect(not any("b.html#y" in m for m in msgs), "a good cross-page anchor was reported")

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("test_docs_html: ok (%d N rows)" % len(rows))
