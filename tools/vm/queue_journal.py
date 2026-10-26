#!/usr/bin/env python3
"""
queue_journal.py - the journal lines of the queue runner (the systemd units rdna3vm-queue-*), from `journalctl -o json`.

    journalctl --no-pager -q -o json --since -6h | python3 queue_journal.py [--short]

Why not `journalctl -u 'rdna3vm-queue-*'`: a glob in -u makes journalctl list every unit name of every journal
file first, 115 s on a 4 GB journal (2026-10-26, `rdna3vm result` and `rdna3vm queue status` waited for it);
`--since` alone reads the last hours in 1.5 s and this filter keeps the lines of the runner's units: the same
rule as -u (the unit's own lines, and systemd's about it: the UNIT / OBJECT_SYSTEMD_UNIT fields). The output
looks like `-o short-iso` (default) or `-o short` (--short); a line that cannot be read is skipped, never an error.
"""
import datetime
import json
import sys

PREFIX = "rdna3vm-queue-"
FIELDS = ("_SYSTEMD_UNIT", "UNIT", "OBJECT_SYSTEMD_UNIT", "COREDUMP_UNIT")


def text(v):
    if isinstance(v, list):   # a binary message is a list of bytes
        return bytes(v).decode("utf-8", "replace")
    return str(v)


def keep(d):
    return any(text(d.get(f, "")).startswith(PREFIX) for f in FIELDS)


def fmt(d, short):
    us = int(d["__REALTIME_TIMESTAMP"])
    t = datetime.datetime.fromtimestamp(us // 1000000).astimezone()
    when = t.strftime("%b %d %H:%M:%S") if short else t.isoformat(timespec="seconds")
    ident = text(d.get("SYSLOG_IDENTIFIER") or d.get("_COMM") or "?")
    pid = d.get("_PID") or d.get("SYSLOG_PID")
    return "%s %s %s%s: %s" % (when, text(d.get("_HOSTNAME", "?")), ident, "[%s]" % text(pid) if pid else "",
                               text(d.get("MESSAGE", "")))


def main(argv):
    short = "--short" in argv
    for line in sys.stdin:
        try:
            d = json.loads(line)
            if keep(d):
                print(fmt(d, short))
        except (ValueError, KeyError, TypeError, OverflowError, OSError):
            continue
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
