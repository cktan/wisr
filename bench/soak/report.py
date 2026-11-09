#!/usr/bin/env python3
"""report.py -- the tables of a soak run, as markdown.

    soak/report.py build/soak/dg [warmup_seconds]

Reads the samples and the pgbench logs that soak.sh left in the
directory, and judges the pass criteria of SOAK.md section 1.

1. Read the samples, and keep those after the warm-up.
2. Fit a line, over the second half of the scored part, to each of
   memory, threads and descriptors.
3. Compare the first tenth of the scored part with the last tenth:
   tps (from the progress lines), and p50 and p99 latency (from the
   2% sample of the transaction logs).
4. Compare the data directory's peak size in the two halves.
5. Print the mix of the transactions, and the retries.
"""
import csv
import glob
import os
import re
import statistics
import sys


def median(v):
    """Median of a list, 0 if empty."""
    return statistics.median(v) if v else 0


def slope_per_hour(xs, ys):
    """Least-squares slope of ys over xs (seconds), in units an hour."""
    if len(xs) < 3:
        return 0.0
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    den = sum((x - mx) ** 2 for x in xs)
    if den == 0:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den * 3600


def pct(v, p):
    """The p-th percentile (0 to 100) of a list, 0 if empty."""
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p / 100))]


def read_samples(d, warmup):
    """Rows of samples.csv after the warm-up, as dicts of numbers."""
    rows = []
    with open(os.path.join(d, "samples.csv")) as f:
        for r in csv.DictReader(f):
            r = {k: float(v) for k, v in r.items()}
            if r["t"] >= warmup:
                rows.append(r)
    return rows


def read_progress(d):
    """Per group: a list of (seconds, tps, latency ms) progress lines."""
    out = {}
    rx = re.compile(r"progress: ([\d.]+) s, ([\d.]+) tps, lat ([\d.]+) ms")
    for p in sorted(glob.glob(os.path.join(d, "pgbench-*.log"))):
        g = os.path.basename(p)[len("pgbench-"):-len(".log")]
        out[g] = []
        with open(p) as f:
            for line in f:
                m = rx.search(line)
                if m:
                    out[g].append(tuple(float(x) for x in m.groups()))
    return out


def read_latencies(d, group, t0):
    """(seconds since t0, latency ms) of the logged transactions."""
    out = []
    for p in glob.glob(os.path.join(d, "tx-%s.*" % group)):
        with open(p) as f:
            for line in f:
                c = line.split()
                try:
                    out.append((int(c[4]) - t0, int(c[2]) / 1000.0))
                except (IndexError, ValueError):
                    pass  # a line cut short while the run goes on
    return out


def read_mix(d):
    """Transactions by script name, retries and failures, in all groups."""
    mix, retries, failed = {}, 0, 0
    for p in glob.glob(os.path.join(d, "pgbench-*.log")):
        name = None
        for line in open(p):
            m = re.match(r"SQL script \d+: soak/(\w+)\.sql", line)
            if m:
                name = m.group(1)
            m = re.match(r" - (\d+) transactions \(", line)
            if m and name:
                mix[name] = mix.get(name, 0) + int(m.group(1))
            m = re.match(r"number of failed transactions: (\d+)", line)
            if m and name is None:
                failed += int(m.group(1))
            m = re.match(r"number of transactions retried: (\d+)", line)
            if m and name is None:
                retries += int(m.group(1))
    return mix, retries, failed


def main():
    d = sys.argv[1]
    warmup = float(sys.argv[2]) if len(sys.argv) > 2 else 900
    s = read_samples(d, warmup)
    if len(s) < 10:
        print("report: too few samples after the warm-up:", len(s))
        return 1
    ts = [r["t"] for r in s]
    n10 = max(1, len(s) // 10)

    print("| measure | start | end | slope an hour | limit | result |")
    print("| --- | --: | --: | --: | --: | --- |")
    ok = True
    for key, label, limit in (("pss_kb", "PSS (MB)", 1024),
                              ("threads", "threads", 1),
                              ("fds", "descriptors", 1)):
        ys = [r[key] for r in s]
        scale = 1024.0 if key == "pss_kb" else 1.0
        # The slope of the second half: the first half still settles.
        sl = slope_per_hour(ts[len(ts) // 2:], ys[len(ys) // 2:])
        a, b = median(ys[:n10]), median(ys[-n10:])
        res = "pass" if abs(sl) <= limit else "FAIL"
        ok &= res == "pass"
        print("| %s | %.1f | %.1f | %.2f | %.2f | %s |" %
              (label, a / scale, b / scale, sl / scale, limit / scale, res))
    # The size is a sawtooth: it grows until a compaction, then falls.
    # Compare the peaks of the first and the second half.
    data = [r["data_kb"] for r in s]
    half = len(data) // 2
    pa, pb = max(data[:half]), max(data[half:])
    res = "pass" if pb <= 1.10 * pa else "FAIL"
    ok &= res == "pass"
    print("| data directory peak (MB) | %.0f | %.0f | | 10%% over the first half | %s |" %
          (pa / 1024, pb / 1024, res))
    files = [r["files"] for r in s]
    print("| data files | %d | %d | | | |" %
          (median(files[:n10]), median(files[-n10:])))
    print()

    # The progress lines run on the clients' clock, from 0 at their start.
    prog = read_progress(d)
    ticks = sorted({round(t) for g in prog.values() for t, _, _ in g
                    if t >= warmup})
    tps_by_t = {}
    for g in prog.values():
        for t, tps, _ in g:
            if t >= warmup:
                tps_by_t[round(t)] = tps_by_t.get(round(t), 0) + tps
    tv = [tps_by_t[t] for t in ticks]
    k = max(1, len(tv) // 10)
    a, b = median(tv[:k]), median(tv[-k:])
    res = "pass" if b >= 0.95 * a else "FAIL"
    ok &= res == "pass"
    print("Total tps of the four client groups, median: first tenth %.0f,"
          " last tenth %.0f (%+.1f%%): %s" % (a, b, 100 * (b - a) / a if a else 0, res))
    print()

    print("| group | first tenth p50 ms | p99 ms | last tenth p50 ms | p99 ms | result |")
    print("| --- | --: | --: | --: | --: | --- |")
    names = {"rs": "reconnecting, student", "rp": "reconnecting, prof",
             "ps": "persistent, student", "pp": "persistent, prof"}
    for g in ("rs", "rp", "ps", "pp"):
        files = glob.glob(os.path.join(d, "tx-%s.*" % g))
        if not files:
            continue
        t0 = min(t for t, _ in read_latencies(d, g, 0)) \
            if read_latencies(d, g, 0) else 0
        lat = [(t, v) for t, v in read_latencies(d, g, t0) if t >= warmup]
        if not lat:
            continue
        lo = min(t for t, _ in lat)
        hi = max(t for t, _ in lat)
        w = (hi - lo) / 10.0
        first = [v for t, v in lat if t < lo + w]
        last = [v for t, v in lat if t >= hi - w]
        p99a, p99b = pct(first, 99), pct(last, 99)
        res = "pass" if p99b <= 1.05 * p99a else "FAIL"
        if g in ("ps", "pp"):
            ok &= res == "pass"
        else:
            res += " (not scored: includes connecting)"
        print("| %s | %.2f | %.2f | %.2f | %.2f | %s |" %
              (names[g], pct(first, 50), p99a, pct(last, 50), p99b, res))
    print()

    mix, retries, failed = read_mix(d)
    tot = sum(mix.values()) or 1
    cat = {"read": ["student_sched", "student_grades", "prof_sched", "roster"],
           "update": ["set_grade"], "delete": ["drop_class"],
           "insert": ["add_class"]}
    print("Transactions: %d (%.0f/s over the run). Mix: %s." % (
        tot, tot / (ts[-1] + warmup),
        ", ".join("%s %.1f%%" % (c, 100 * sum(mix.get(n, 0) for n in ns) / tot)
                  for c, ns in cat.items())))
    print("Retried after a serialization failure: %d. Failed for good: %d." %
          (retries, failed))
    print()
    print("Overall: %s" % ("PASS" if ok else "FAIL"))
    return 0


sys.exit(main())
