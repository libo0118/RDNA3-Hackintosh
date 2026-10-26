#!/usr/bin/env python3
"""
triage_par.py - ordered parallel map for the triage's slow, pure steps (the glyph check of the looks).

pmap(fn, items) answers exactly [fn(x) for x in items], in the same order, computed by forked worker
processes when there are several items and several CPUs. RDNA3_TRIAGE_JOBS=N sets the workers (1 = serial,
the default is the CPUs this process may use). Any trouble with the pool (no fork on this platform, no
processes left, a worker killed) is not an error: the items are then computed serially in this process,
so the output never depends on whether the pool worked. An exception raised by fn itself is raised here
too (the callers catch inside fn what must not stop the triage).
"""
import multiprocessing
import os


def jobs():
    try:
        n = int(os.environ.get("RDNA3_TRIAGE_JOBS", "") or 0)
    except ValueError:
        n = 0
    if n >= 1:
        return n
    try:
        return max(1, len(os.sched_getaffinity(0)))
    except (AttributeError, OSError):
        return max(1, os.cpu_count() or 1)


def pmap(fn, items, workers=None):
    items = list(items)
    n = min(workers or jobs(), len(items))
    if n <= 1:
        return [fn(x) for x in items]
    try:
        ctx = multiprocessing.get_context("fork")   # fn and the data are inherited, nothing is pickled but the answers
        pool = ctx.Pool(n)
        try:
            res = pool.map(fn, items, chunksize=1)
        except BaseException:
            pool.terminate()
            pool.join()
            raise
        pool.close()   # close + join, not terminate() (what 'with' does at the end): it waited 1.2 s for the result thread
        pool.join()
        return res
    except (OSError, ValueError, ImportError, multiprocessing.ProcessError, EOFError):
        return [fn(x) for x in items]
