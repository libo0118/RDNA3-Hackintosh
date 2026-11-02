#!/usr/bin/env python3
"""docrows.py - the machine side of the HTML working docs (docs/DRIVER-PLAN.html, AFTER-TESTS.html, CODEMAP.html).

    python3 tools/docrows.py next            # the next free N number for a DRIVER-PLAN row
    python3 tools/docrows.py check           # every docs/*.html: tags balanced, ids unique, links resolve;
                                             # DRIVER-PLAN rows well formed
    python3 tools/docrows.py fix             # rewrite each N row's badge and the CSS class from its data-status
    python3 tools/docrows.py snippet N [S]   # print an empty N row (S = size, default M)

An N row of DRIVER-PLAN.html is one section (the single source of truth is its data-status):

  <section class="row" id="n77" data-n="77" data-status="open" data-size="M">
  <div class="rowhead"><a class="rid" href="#n77">N77</a> <span class="badge st-open gen">open</span></div>
  <p class="rowtext">what is missing, all the detail; &lt; &gt; &amp; escaped, <code>code</code> allowed</p>
  <dl class="rowmeta"><dt class="gen">Where it shows</dt><dd>dirs</dd><dt class="gen">Size</dt><dd>M</dd><dt class="gen">Status</dt><dd>open</dd></dl>
  </section>

data-status is one of open, card (done, the card run is open), partial, done, superseded, unknown; `fix` makes the
badge match it. The rows live in <div class="rows" id="n-rows"> of the "N - not built at all" section.
"""
import html
import os
import re
import sys
from html.parser import HTMLParser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")
PLAN = os.path.join(DOCS, "DRIVER-PLAN.html")

STATUS_LABEL = {"open": "open", "card": "done, card open", "partial": "partly done", "done": "done",
                "superseded": "superseded", "unknown": "see text"}
VOID = {"meta", "br", "hr", "img", "input", "link"}
ROW_RE = re.compile(r'<section class="row" id="(?P<id>[^"]*)"(?P<attrs>[^>]*)>(?P<body>.*?)</section>', re.S)
BADGE_RE = re.compile(r'<span class="badge st-(?P<cls>\w+) gen">(?P<label>[^<]*)</span>')


class Scan(HTMLParser):
    """tag balance, ids, hrefs of one page"""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.ids = []
        self.hrefs = []
        self.errors = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.append(a["id"])
        if tag == "a" and a.get("href"):
            self.hrefs.append(a["href"])
        if tag not in VOID:
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1] != tag:
            self.errors.append("</%s> does not close <%s> (line %d)" % (tag, self.stack[-1] if self.stack else "-", self.getpos()[0]))
            if tag in self.stack:
                while self.stack and self.stack[-1] != tag:
                    self.stack.pop()
                self.stack.pop()
            return
        self.stack.pop()


def scan(path):
    s = Scan()
    with open(path, encoding="utf-8") as f:
        s.feed(f.read())
    s.close()
    if s.stack:
        s.errors.append("unclosed tags: %s" % s.stack[-5:])
    return s


def lint_pages(docs_dir=DOCS):
    """problems found in every docs/**/*.html: [(file, message)]"""
    pages = {}
    for dirpath, _, names in os.walk(docs_dir):
        for n in sorted(names):
            if n.endswith(".html"):
                pages[os.path.join(dirpath, n)] = scan(os.path.join(dirpath, n))
    out = []
    for path, s in sorted(pages.items()):
        rel = os.path.relpath(path, docs_dir)
        for e in s.errors:
            out.append((rel, e))
        seen = set()
        for i in s.ids:
            if i in seen:
                out.append((rel, "duplicate id %r" % i))
            seen.add(i)
        for h in s.hrefs:
            if re.match(r"^([a-z][a-z0-9+.-]*:|/)", h, re.I):
                continue
            target, _, frag = h.partition("#")
            tpath = path if not target else os.path.normpath(os.path.join(os.path.dirname(path), target))
            if target and not os.path.exists(tpath):
                if tpath.startswith(docs_dir):
                    out.append((rel, "link to a missing file: %s" % h))
                continue
            if frag and tpath.endswith(".html"):
                t = pages.get(tpath)
                if t is None:
                    continue
                if frag not in t.ids:
                    out.append((rel, "link to a missing anchor: %s" % h))
    return out


def plan_rows(text):
    """[(id, attrs dict, body)] of the N rows"""
    rows = []
    for m in ROW_RE.finditer(text):
        attrs = dict(re.findall(r'(data-[\w-]+)="([^"]*)"', m.group("attrs")))
        rows.append((m.group("id"), attrs, m.group("body")))
    return rows


def check_plan_text(text):
    """problems in the N rows of a DRIVER-PLAN page: [message]"""
    out = []
    rows = plan_rows(text)
    if not rows:
        out.append("no N rows (<section class=\"row\" ...>) found")
    if text.count("<section class=\"row\"") != len(rows):
        out.append("a <section class=\"row\"> does not match the row pattern (id first, then data-* attributes)")
    seen = set()
    for rid, attrs, body in rows:
        if rid in seen:
            out.append("%s: duplicate id" % rid)
        seen.add(rid)
        m = re.match(r"^n(\d+)(-\d+)?$", rid)
        if not m:
            out.append("%s: the id must be n<number>" % rid)
            continue
        n = m.group(1)
        if attrs.get("data-n") != n:
            out.append("%s: data-n=%r, expected %r" % (rid, attrs.get("data-n"), n))
        st = attrs.get("data-status")
        if st not in STATUS_LABEL:
            out.append("%s: data-status %r is not one of %s" % (rid, st, "/".join(sorted(STATUS_LABEL))))
            continue
        b = BADGE_RE.search(body)
        if not b:
            out.append('%s: no <span class="badge st-%s gen">%s</span> in the row head' % (rid, st, STATUS_LABEL[st]))
        elif b.group("cls") != st or b.group("label") != STATUS_LABEL[st]:
            out.append("%s: badge says st-%s %r but data-status is %s (python3 tools/docrows.py fix)" % (
                rid, b.group("cls"), b.group("label"), st))
        if 'href="#%s"' % rid not in body or ">N%s<" % n not in body:
            out.append('%s: the head must hold <a class="rid" href="#%s">N%s</a>' % (rid, rid, n))
        if '<p class="rowtext">' not in body:
            out.append('%s: no <p class="rowtext"> (what is missing)' % rid)
    return out


def fix_plan_text(text):
    def repl(m):
        attrs = dict(re.findall(r'(data-[\w-]+)="([^"]*)"', m.group("attrs")))
        st = attrs.get("data-status")
        if st not in STATUS_LABEL:
            return m.group(0)
        body = BADGE_RE.sub('<span class="badge st-%s gen">%s</span>' % (st, STATUS_LABEL[st]), m.group("body"), count=1)
        return m.group(0).replace(m.group("body"), body, 1)
    return ROW_RE.sub(repl, text)


def next_n(text):
    nums = [int(m.group(1)) for m in re.finditer(r'<section class="row" id="n(\d+)"', text)]
    return max(nums) + 1 if nums else 1


def snippet(n, size="M"):
    return (
        '<section class="row" id="n%(n)d" data-n="%(n)d" data-status="open" data-size="%(s)s">\n'
        '<div class="rowhead"><a class="rid" href="#n%(n)d">N%(n)d</a> <span class="badge st-open gen">open</span></div>\n'
        '<p class="rowtext">WHAT IS MISSING: the evidence, the cause, what was done, what stays open.</p>\n'
        '<dl class="rowmeta"><dt class="gen">Where it shows</dt><dd>dirs</dd><dt class="gen">Size</dt><dd>%(s)s</dd>'
        '<dt class="gen">Status</dt><dd>open</dd></dl>\n'
        '</section>\n' % {"n": n, "s": html.escape(size)})


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "check"
    with open(PLAN, encoding="utf-8") as f:
        text = f.read()
    if cmd == "next":
        print(next_n(text))
        return 0
    if cmd == "snippet":
        print(snippet(int(argv[2]), argv[3] if len(argv) > 3 else "M"), end="")
        return 0
    if cmd == "fix":
        new = fix_plan_text(text)
        if new != text:
            with open(PLAN, "w", encoding="utf-8") as f:
                f.write(new)
            print("DRIVER-PLAN.html: badges rewritten")
        else:
            print("DRIVER-PLAN.html: badges already match")
        return 0
    if cmd == "check":
        problems = ["DRIVER-PLAN.html: " + m for m in check_plan_text(text)]
        problems += ["%s: %s" % p for p in lint_pages()]
        for p in problems:
            print("FAIL", p)
        print("docs: %s" % ("%d problem(s)" % len(problems) if problems else "ok"))
        return 1 if problems else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
