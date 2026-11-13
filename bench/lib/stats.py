#!/usr/bin/env python3
"""Confidence intervals for the benchmark report (used by report.py).

Two kinds of interval, both at 95%:

* For the median of one benchmark under one allocator: the distribution-free interval from
  order statistics.  With n repetitions, [x(k), x(n+1-k)] covers the median with probability
  1 - 2 P(Bin(n, 1/2) < k); k is the largest rank that keeps the coverage at 95% or more
  (n = 10 gives [x(2), x(9)], coverage 97.9%).

* For everything derived from several medians (ratios between allocators, normalised factors,
  geometric means, the combined score): a block bootstrap over repetitions.  A repetition runs
  every benchmark under every allocator once, so repetitions are the independent blocks.  Each
  resample draws whole repetitions with replacement and recomputes every median, normalisation
  and geometric mean from them, which keeps whatever is common to a repetition (machine state,
  time of day) paired across allocators.  The interval is the percentile interval: the 2.5th
  and 97.5th percentiles of the resampled values.

bench/lib/selftest_stats.py measures the coverage of both on simulated campaigns with 10
repetitions: 95% to 98% for the percentile interval.  The expanded percentile interval
(T. Hesterberg, "What Teachers Should Know About the Bootstrap", The American Statistician 69(4),
2015), which widens the percentiles to Phi(-sqrt(n/(n-1)) * t(0.975, n-1)) for small n, was tried
first and rejected: its coverage was 97% to 100%, wider than needed, because the bootstrap of a
median already overstates its spread.  It is kept as an option (expanded=True) for that comparison.
"""
import math
import random
import statistics
from collections import defaultdict

CI_MIN_REPS = 6          # fewer repetitions give no interval
BOOT_B = 4000
BOOT_SEED = 20261113
# t(0.975, df) for df = 1..30
T975 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131,
        2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]


def gmean(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")


def median_ci(xs):
    """Distribution-free 95% interval for the median, or None with fewer than CI_MIN_REPS values."""
    n = len(xs)
    if n < CI_MIN_REPS:
        return None
    xs = sorted(xs)
    cum, k = 0.0, 0
    for i in range(n // 2):
        cum += math.comb(n, i) / 2.0 ** n
        if cum > 0.025:
            break
        k = i + 1
    return (xs[k - 1], xs[n - k]) if k else None


def tail_probability(n, expanded=False):
    """Lower-tail probability of the percentile interval for n blocks."""
    if not expanded:
        return 0.025
    df = n - 1
    t = T975[df - 1] if df <= len(T975) else 1.96 + 2.372 / df
    return 0.5 * math.erfc(math.sqrt(n / (n - 1.0)) * t / math.sqrt(2.0))


def percentile_ci(samples, tail):
    s = sorted(samples)
    if not s:
        return None
    m = len(s) - 1
    return s[int(math.floor(tail * m))], s[int(math.ceil((1.0 - tail) * m))]


def bootstrap_stats(cell, benches, allocs, me="sohamalloc", B=BOOT_B, seed=BOOT_SEED, expanded=False, raw=False):
    """Block bootstrap over repetitions.

    cell[(bench, alloc)]["byrep"] maps a repetition number to (time, rss).  Returns None unless
    every benchmark has at least CI_MIN_REPS repetitions under every allocator.  Otherwise a
    dict of intervals keyed by
      ("time", a), ("rss", a), ("score", a)   normalised geometric means of allocator a
      ("vs_time", o), ("vs_rss", o)           geometric mean of the ratio me / o
      ("bench", b, o)                         time ratio me / o on benchmark b
    plus "_meta".  With raw=True the resampled values are returned instead of intervals.
    """
    if not benches or any(len(cell[(b, a)]["byrep"]) < CI_MIN_REPS for b in benches for a in allocs):
        return None
    reps = sorted({rep for b in benches for a in allocs for rep in cell[(b, a)]["byrep"]})
    others = [o for o in allocs if o != me] if me in allocs else []
    rnd = random.Random(seed)
    acc = defaultdict(list)
    for _ in range(B):
        draw = [reps[rnd.randrange(len(reps))] for _ in reps]
        mt, mr, complete = {}, {}, True
        for b in benches:
            for a in allocs:
                br = cell[(b, a)]["byrep"]
                ts = [br[r][0] for r in draw if r in br]
                rs = [br[r][1] for r in draw if r in br and br[r][1]]
                if not ts or not rs:
                    complete = False
                    break
                mt[(b, a)] = max(statistics.median(ts), 1e-3)
                mr[(b, a)] = statistics.median(rs)
            if not complete:
                break
        if not complete:      # a repetition with a failed run was drawn for every slot of a cell
            continue
        nt, nr = {}, {}
        for b in benches:
            best_t = min(mt[(b, a)] for a in allocs)
            best_r = min(mr[(b, a)] for a in allocs)
            for a in allocs:
                nt[(b, a)] = mt[(b, a)] / best_t
                nr[(b, a)] = mr[(b, a)] / best_r
        for a in allocs:
            acc[("time", a)].append(gmean([nt[(b, a)] for b in benches]))
            acc[("rss", a)].append(gmean([nr[(b, a)] for b in benches]))
            acc[("score", a)].append(math.sqrt(gmean([nt[(b, a)] * nr[(b, a)] for b in benches])))
        for o in others:
            acc[("vs_time", o)].append(gmean([mt[(b, me)] / mt[(b, o)] for b in benches]))
            acc[("vs_rss", o)].append(gmean([mr[(b, me)] / mr[(b, o)] for b in benches]))
            for b in benches:
                acc[("bench", b, o)].append(mt[(b, me)] / mt[(b, o)])
    if raw:
        return acc
    tail = tail_probability(len(reps), expanded)
    out = {k: percentile_ci(v, tail) for k, v in acc.items()}
    out["_meta"] = {"reps": len(reps), "B": B, "tail": tail, "used": len(acc[("time", allocs[0])])}
    return out


def fmt_ci(x, ci, nd=3):
    f = "%%.%df" % nd
    return f % x + (" [" + f % ci[0] + ", " + f % ci[1] + "]" if ci else "")


def verdict(ci, below, above):
    if not ci:
        return ""
    return below if ci[1] < 1 else (above if ci[0] > 1 else "no difference")


def ci_sections(w, rows, cell, med, common, allocs, scores, me="sohamalloc"):
    """Append the confidence-interval sections of the report through w(line)."""
    w("## Confidence intervals (95%)\n")
    ci = bootstrap_stats(cell, common, allocs, me) if common else None
    if ci is None:
        w("Not computed: every benchmark needs at least %d successful repetitions under every allocator.\n"
          % CI_MIN_REPS)
        return
    m = ci["_meta"]
    w("Intervals for ratios and geometric means come from a block bootstrap over the %d repetitions "
      "(%d resamples, percentile interval). Intervals for single medians come from order "
      "statistics. The method and its reference are in `bench/lib/stats.py`; "
      "`bench/lib/selftest_stats.py` measures their coverage.\n" % (m["reps"], m["used"]))

    w("### Geometric means\n")
    w("| allocator | time | rss | score |")
    w("|---|---|---|---|")
    for a in allocs:
        w("| %s | %s | %s | %s |" % (a, fmt_ci(scores[a][0], ci[("time", a)]),
                                    fmt_ci(scores[a][1], ci[("rss", a)]), fmt_ci(scores[a][2], ci[("score", a)])))
    w("")

    if me in allocs:
        others = [o for o in allocs if o != me]
        t_of = lambda b, a: max(med[(b, a)][0], 1e-3)   # noqa: E731
        w("### %s relative to each allocator\n" % me)
        w("Geometric mean over the %d benchmarks of the ratio of medians (%s / other). Below 1 means %s "
          "is better. The verdict says whether the interval excludes 1.\n" % (len(common), me, me))
        w("| vs | time ratio | time | rss ratio | rss |")
        w("|---|---|---|---|---|")
        for o in others:
            tr = gmean([t_of(b, me) / t_of(b, o) for b in common])
            rr = gmean([med[(b, me)][1] / med[(b, o)][1] for b in common])
            w("| %s | %s | %s | %s | %s |" % (
                o, fmt_ci(tr, ci[("vs_time", o)]), verdict(ci[("vs_time", o)], "faster", "slower"),
                fmt_ci(rr, ci[("vs_rss", o)]), verdict(ci[("vs_rss", o)], "leaner", "fatter")))
        w("")
        w("### Time ratio per benchmark (%s / other)\n" % me)
        w("`*` marks an interval that excludes 1.\n")
        w("| benchmark | " + " | ".join("vs " + o for o in others) + " |")
        w("|---|" + "---|" * len(others))
        tally = {o: [0, 0, 0] for o in others}
        for b in common:
            cells = []
            for o in others:
                c = ci[("bench", b, o)]
                tally[o][0 if c[1] < 1 else (2 if c[0] > 1 else 1)] += 1
                cells.append(fmt_ci(t_of(b, me) / t_of(b, o), c, 2) + (" *" if (c[1] < 1 or c[0] > 1) else ""))
            w("| %s | %s |" % (b, " | ".join(cells)))
        w("")
        for o in others:
            w("- vs %s: faster on %d, no difference on %d, slower on %d." % (o, tally[o][0], tally[o][1], tally[o][2]))
        w("")

    w("### Stability across repetitions\n")
    w("For each repetition: the geometric mean of (run time / median of its benchmark and allocator). "
      "1.000 means the repetition ran at the typical speed; `load` is the mean 1-minute load at the start "
      "of its runs.\n")
    rel, loads = defaultdict(list), defaultdict(list)
    for r in rows:
        k = (r["bench"], r["alloc"])
        if r["status"] == "ok" and r["time_s"] and k in med:
            rel[int(r["rep"])].append(max(float(r["time_s"]), 1e-3) / max(med[k][0], 1e-3))
            if r.get("load1"):
                loads[int(r["rep"])].append(float(r["load1"]))
    w("| repetition | relative time | load |")
    w("|---:|---:|---:|")
    for rep in sorted(rel):
        w("| %d | %.3f | %s |" % (rep, gmean(rel[rep]),
                                 "%.2f" % (sum(loads[rep]) / len(loads[rep])) if loads[rep] else "n/a"))
    w("")

    w("### Median time with its interval\n")
    w("| benchmark | " + " | ".join(allocs) + " |")
    w("|---|" + "---|" * len(allocs))
    for b in common:
        w("| %s | %s |" % (b, " | ".join(fmt_ci(med[(b, a)][0], median_ci(cell[(b, a)]["t"])) for a in allocs)))
    w("")
