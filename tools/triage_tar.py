#!/usr/bin/env python3
"""
triage_tar.py - a report archive read ONCE, for tools/triage_reports.py and tools/triage_bisect.py.

The card's report is a 5 MB .tar.gz of 600 MB (the plugin's traces are zeros mostly). The plain tarfile
decompresses all of it for every member the triage reads (getmembers, then extractfile): about 0.65 s each
and 75 times per triage, 50 s of the 64 s `rdna3vm result` spent in it (2026-10-26c). open_tar() reads the
archive in one pass and keeps the members up to CACHE_MAX in memory; the object answers the calls the triage
uses (getmembers(), extractfile(), iteration, 'with') with the same names, sizes and bytes the tarfile gives.
"""
import io
import os
import tarfile

CACHE_MAX = 48 << 20     # a member above this is not kept (the plugin's .trace files are 51 MB each)
_CACHE = {}              # absolute path -> (mtime_ns, size, CachedTar)


class Member:
    """what a member looks like to the callers: name, size, isfile()"""
    __slots__ = ("name", "size", "_file")

    def __init__(self, name, size, isfile):
        self.name, self.size, self._file = name, size, isfile

    def isfile(self):
        return self._file


class CachedTar:
    def __init__(self, path):
        self.path = path
        self.members = []
        self.data = {}
        with tarfile.open(path) as tar:
            for m in tar:
                self.members.append(Member(m.name, m.size, m.isfile()))
                # the first member of a name is the one read_member() finds
                if m.isfile() and m.size <= CACHE_MAX and not m.name.endswith(".trace") and m.name not in self.data:
                    self.data[m.name] = tar.extractfile(m).read()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def __iter__(self):
        return iter(self.members)

    def getmembers(self):
        return list(self.members)

    def extractfile(self, m):
        name = m.name if hasattr(m, "name") else m
        if name in self.data:
            return io.BytesIO(self.data[name])
        with tarfile.open(self.path) as tar:      # a member that was not kept: the slow way
            return io.BytesIO(tar.extractfile(name).read())


def open_tar(path):
    """tarfile.open(path) for the triage's reads: decompressed once per file and kept (a file that changed on
    disk is read again). Raises what tarfile.open would (OSError, tarfile.TarError)."""
    key = os.path.abspath(path)
    st = os.stat(path)
    hit = _CACHE.get(key)
    if hit and hit[0] == st.st_mtime_ns and hit[1] == st.st_size:
        return hit[2]
    tar = CachedTar(path)
    _CACHE[key] = (st.st_mtime_ns, st.st_size, tar)
    return tar
