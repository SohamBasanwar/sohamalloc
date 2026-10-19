#!/usr/bin/env python3
"""Build bench/out/report.md from a results CSV written by runner.py.

  python3 bench/lib/report.py bench/out/results-<stamp>.csv [> report.md]

Per benchmark and allocator: median time and median peak RSS over the
successful repetitions, each normalised to the best allocator on that
benchmark (1.00 = best).  Per allocator: geometric means of the normalised
time and RSS over the benchmarks where every allocator succeeded, and a
combined score sqrt(geomean(time_norm * rss_norm)) (lower is better).
"""
import csv
import math
import os
import statistics
import sys
from collections import OrderedDict, defaultdict

APPS = {"python", "python-malloc", "gxx", "sqlite", "redis"}
TIE = 0.02   # head-to-head: differences under 2% count as a tie
ORDER = ["glibc", "jemalloc", "mimalloc", "tcmalloc", "sohamalloc"]


def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def gmean(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")


def summarize(rows):
    benches = list(OrderedDict.fromkeys(r["bench"] for r in rows))
    seen = list(OrderedDict.fromkeys(r["alloc"] for r in rows))
    allocs = [a for a in ORDER if a in seen] + [a for a in seen if a not in ORDER]
    cell = defaultdict(lambda: {"t": [], "r": [], "fail": [], "n": 0, "metric": "wall"})
    for r in rows:
        c = cell[(r["bench"], r["alloc"])]
        c["n"] += 1
        c["metric"] = r["metric"]
        if r["status"] == "ok" and r["time_s"]:
            c["t"].append(float(r["time_s"]))
            if r["rss_kb"]:
                c["r"].append(float(r["rss_kb"]))
        else:
            c["fail"].append(r)
    med = {}
    for k, c in cell.items():
        if c["t"]:
            t = statistics.median(c["t"])
            rs = statistics.median(c["r"]) if c["r"] else None
            spread = ((max(c["t"]) - min(c["t"])) / t if t > 0 else 0.0) if len(c["t"]) > 1 else None
            med[k] = (t, rs, spread)
    return benches, allocs, cell, med


def build_report(csv_path, env_info=None):
    rows = load(csv_path)
    if not rows:
        return "# sohamalloc benchmark report\n\n(no results)\n"
    benches, allocs, cell, med = summarize(rows)
    reps = max(int(r["rep"]) for r in rows)
    out = []
    w = out.append
    w("# sohamalloc benchmark report\n")
    w("Results file: `%s` (%d runs, %d benchmarks, %d allocators, up to %d repetitions).\n"
      % (os.path.basename(csv_path), len(rows), len(benches), len(allocs), reps))
    if env_info:
        w("```\n" + "\n".join(env_info) + "\n```\n")
    w("Lower is better everywhere. `time` is wall-clock seconds, except for the fixed-duration "
      "benchmarks marked †, where it is a relative time derived from the reported throughput "
      "(as in mimalloc-bench). `rss` is the peak resident set size (GNU time `%M`). Each cell is "
      "the median over the successful repetitions; the factor in parentheses is normalised to the "
      "best allocator for that benchmark. **Bold** = best. `spread` is the largest "
      "(max-min)/median of the time over repetitions among the allocators, a noise indicator.\n")

    norm = {}
    best_t, best_r = {}, {}
    for b in benches:
        ts = [med[(b, a)][0] for a in allocs if (b, a) in med]
        rs = [med[(b, a)][1] for a in allocs if (b, a) in med and med[(b, a)][1]]
        # Guard against a 0.000 s median (sub-resolution run): floor at 1 ms.
        best_t[b] = max(min(ts), 1e-3) if ts else None
        best_r[b] = min(rs) if rs else None
        for a in allocs:
            if (b, a) in med:
                t, r, _ = med[(b, a)]
                norm[(b, a)] = (max(t, 1e-3) / best_t[b] if best_t[b] else None,
                                r / best_r[b] if (r and best_r[b]) else None)

    def failtxt(b, a):
        c = cell.get((b, a))
        if not c or not c["fail"]:
            return ""
        sts = sorted({f["status"] for f in c["fail"]})
        return " ⚠%d/%d %s" % (len(c["fail"]), c["n"], ",".join(sts))

    def is_rel(b):
        return any(cell[(b, a)]["metric"] == "relative" for a in allocs if (b, a) in cell)

    # ---- time table
    w("## Time per benchmark\n")
    w("| benchmark | " + " | ".join(allocs) + " | spread |")
    w("|---|" + "---:|" * len(allocs) + "---:|")
    for b in benches:
        cells, spreads = [], []
        for a in allocs:
            if (b, a) in med:
                t, _, sp = med[(b, a)]
                if sp is not None:
                    spreads.append(sp)
                s = "%.3f (%.2f)" % (t, norm[(b, a)][0])
                if max(t, 1e-3) == best_t[b]:
                    s = "**%s**" % s
                cells.append(s + failtxt(b, a))
            else:
                cells.append("FAIL" + failtxt(b, a) if (b, a) in cell else "–")
        w("| %s%s | %s | %s |" % (b, " †" if is_rel(b) else "", " | ".join(cells),
                                 ("%.0f%%" % (100 * max(spreads))) if spreads else ""))
    w("")

    # ---- rss table
    w("## Peak RSS per benchmark (MiB)\n")
    w("| benchmark | " + " | ".join(allocs) + " |")
    w("|---|" + "---:|" * len(allocs))
    for b in benches:
        cells = []
        for a in allocs:
            if (b, a) in med and med[(b, a)][1]:
                r = med[(b, a)][1]
                s = "%.1f (%.2f)" % (r / 1024.0, norm[(b, a)][1])
                if r == best_r[b]:
                    s = "**%s**" % s
                cells.append(s + failtxt(b, a))
            else:
                cells.append("FAIL" + failtxt(b, a) if (b, a) in cell else "–")
        w("| %s | %s |" % (b, " | ".join(cells)))
    w("")

    # ---- geomeans
    common = [b for b in benches if all((b, a) in norm and norm[(b, a)][0] and norm[(b, a)][1]
                                        for a in allocs)]
    excluded = [b for b in benches if b not in common]
    w("## Geometric means (normalised; 1.00 = best on every benchmark)\n")
    w("Computed over the %d benchmarks where every allocator succeeded%s. "
      "`score` = sqrt(geomean(time_norm × rss_norm)); lower is better.\n"
      % (len(common), (" (excluded: %s)" % ", ".join(excluded)) if excluded else ""))
    subsets = [("all", common), ("mimalloc-bench", [b for b in common if b not in APPS]),
               ("apps", [b for b in common if b in APPS])]
    w("| allocator | time | rss | score | time (mb) | rss (mb) | time (apps) | rss (apps) |")
    w("|---|---:|---:|---:|---:|---:|---:|---:|")
    scores = {}
    for a in allocs:
        vals = []
        for _, bs in subsets:
            tg = gmean([norm[(b, a)][0] for b in bs])
            rg = gmean([norm[(b, a)][1] for b in bs])
            vals.append((tg, rg))
        sc = math.sqrt(gmean([norm[(b, a)][0] * norm[(b, a)][1] for b in common])) if common else float("nan")
        scores[a] = (vals[0][0], vals[0][1], sc)
        w("| %s | %.3f | %.3f | **%.3f** | %.3f | %.3f | %.3f | %.3f |" % (
            a, vals[0][0], vals[0][1], sc, vals[1][0], vals[1][1], vals[2][0], vals[2][1]))
    w("")
    ranked = sorted(allocs, key=lambda a: scores[a][2] if not math.isnan(scores[a][2]) else 1e9)
    w("Ranking by combined score: " + " < ".join("%s (%.3f)" % (a, scores[a][2]) for a in ranked) + "\n")

    # ---- wins / losses
    w("## Wins and losses\n")
    w("Per allocator over the common benchmarks: *best* = lowest median (ties within %d%% of the "
      "best also count as best), *worst* = highest.\n" % int(TIE * 100))
    w("| allocator | time best | time worst | rss best | rss worst |")
    w("|---|---:|---:|---:|---:|")
    wl = {}
    for a in allocs:
        tb = [b for b in common if norm[(b, a)][0] <= 1 + TIE]
        tw = [b for b in common if med[(b, a)][0] == max(med[(b, x)][0] for x in allocs)]
        rb = [b for b in common if norm[(b, a)][1] <= 1 + TIE]
        rw = [b for b in common if med[(b, a)][1] == max(med[(b, x)][1] for x in allocs)]
        wl[a] = (tb, tw, rb, rw)
        w("| %s | %d | %d | %d | %d |" % (a, len(tb), len(tw), len(rb), len(rw)))
    w("")
    for a in allocs:
        tb, tw, rb, rw = wl[a]
        w("- **%s**: fastest on %s; slowest on %s; leanest on %s; fattest on %s." % (
            a, ", ".join(tb) or "none", ", ".join(tw) or "none", ", ".join(rb) or "none",
            ", ".join(rw) or "none"))
    w("")
    me = "sohamalloc"
    if me in allocs:
        w("### Head-to-head: sohamalloc vs each allocator\n")
        w("Counts of benchmarks where sohamalloc is better / tied (±%d%%) / worse.\n" % int(TIE * 100))
        w("| vs | time better | tie | time worse | rss better | tie | rss worse | worst time ratio | worst rss ratio |")
        w("|---|---:|---:|---:|---:|---:|---:|---|---|")
        for o in allocs:
            if o == me:
                continue
            tr = [(med[(b, me)][0] / med[(b, o)][0], b) for b in common]
            rr = [(med[(b, me)][1] / med[(b, o)][1], b) for b in common]
            def cnt(xs):
                return (sum(1 for x, _ in xs if x < 1 - TIE), sum(1 for x, _ in xs if abs(x - 1) <= TIE),
                        sum(1 for x, _ in xs if x > 1 + TIE))
            ct, cr = cnt(tr), cnt(rr)
            wt, wr = max(tr), max(rr)
            w("| %s | %d | %d | %d | %d | %d | %d | %.2f× (%s) | %.2f× (%s) |" % (
                o, ct[0], ct[1], ct[2], cr[0], cr[1], cr[2], wt[0], wt[1], wr[0], wr[1]))
        w("")

    # ---- failures
    fails = [r for r in rows if r["status"] != "ok"]
    w("## Failures\n")
    if not fails:
        w("None: every run exited 0 and every output digest matched the reference run.\n")
    else:
        w("| rep | benchmark | allocator | status | detail |")
        w("|---:|---|---|---|---|")
        for r in fails:
            d = r["detail"].replace("|", "\\|")
            w("| %s | %s | %s | %s | %s |" % (r["rep"], r["bench"], r["alloc"], r["status"], d[:600]))
        w("")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.stdout.write(build_report(sys.argv[1]))
