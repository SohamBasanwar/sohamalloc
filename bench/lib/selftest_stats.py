#!/usr/bin/env python3
"""Measure the coverage of the intervals in stats.py on simulated campaigns.

  python3 bench/lib/selftest_stats.py [CAMPAIGNS]

A simulated campaign has known true ratios, so the share of campaigns whose 95% interval
contains the truth (the coverage) can be counted.  The noise has what real runs have:
independent noise per run, an effect common to all runs of a repetition, and occasional slow
outliers.  A deliberately narrow interval is scored as well, to show that this test can fail.
Exit status 1 if a coverage is outside its accepted range.
"""
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stats  # noqa: E402

BENCHES = ["b%d" % i for i in range(6)]
ALLOCS = ["other1", "other2", "sohamalloc"]
ME = "sohamalloc"
REPS = 10


def campaign(rnd):
    """One simulated campaign: (cell, truth)."""
    ft = {(b, a): math.exp(rnd.uniform(-0.2, 0.2)) for b in BENCHES for a in ALLOCS}   # true time factors
    fr = {(b, a): math.exp(rnd.uniform(-0.2, 0.2)) for b in BENCHES for a in ALLOCS}   # true rss factors
    base = {b: math.exp(rnd.uniform(-1.0, 2.0)) for b in BENCHES}
    cell = {(b, a): {"byrep": {}} for b in BENCHES for a in ALLOCS}
    for rep in range(1, REPS + 1):
        drift = math.exp(rnd.gauss(0.0, 0.02))                 # common to the repetition
        for b in BENCHES:
            for a in ALLOCS:
                noise = math.exp(rnd.gauss(0.0, 0.03)) * (1.3 if rnd.random() < 0.05 else 1.0)
                t = base[b] * ft[(b, a)] * drift * noise
                r = 100000.0 * fr[(b, a)] * math.exp(rnd.gauss(0.0, 0.005))
                cell[(b, a)]["byrep"][rep] = (t, r)
    nt = {(b, a): ft[(b, a)] / min(ft[(b, x)] for x in ALLOCS) for b in BENCHES for a in ALLOCS}
    nr = {(b, a): fr[(b, a)] / min(fr[(b, x)] for x in ALLOCS) for b in BENCHES for a in ALLOCS}
    truth = {
        ("vs_time", "other1"): stats.gmean([ft[(b, ME)] / ft[(b, "other1")] for b in BENCHES]),
        ("vs_rss", "other1"): stats.gmean([fr[(b, ME)] / fr[(b, "other1")] for b in BENCHES]),
        ("time", ME): stats.gmean([nt[(b, ME)] for b in BENCHES]),
        ("score", ME): math.sqrt(stats.gmean([nt[(b, ME)] * nr[(b, ME)] for b in BENCHES])),
        ("bench", "b0", "other1"): ft[("b0", ME)] / ft[("b0", "other1")],
    }
    return cell, truth


def main():
    n_camp = int(sys.argv[1]) if len(sys.argv) > 1 else 600
    rnd = random.Random(1)
    tails = {"percentile (used)": stats.tail_probability(REPS, False),
             "expanded percentile (rejected: too wide)": stats.tail_probability(REPS, True),
             "narrow control (25%..75%)": 0.25}
    hits = {name: {} for name in tails}
    for i in range(n_camp):
        cell, truth = campaign(rnd)
        acc = stats.bootstrap_stats(cell, BENCHES, ALLOCS, ME, B=400, seed=1000 + i, raw=True)
        for name, tail in tails.items():
            for key, true_value in truth.items():
                lo, hi = stats.percentile_ci(acc[key], tail)
                hits[name][key] = hits[name].get(key, 0) + (lo <= true_value <= hi)

    # single medians: order-statistic interval, true median 1.0
    med_hits, med_n = 0, 4000
    for _ in range(med_n):
        xs = [math.exp(rnd.gauss(0.0, 0.05)) * (1.3 if rnd.random() < 0.05 else 1.0) for _ in range(REPS)]
        lo, hi = stats.median_ci(xs)
        med_hits += lo <= 1.0 <= hi          # the 5% outliers move the true median by under 0.4%
    med_cov = med_hits / med_n

    print("coverage of the 95%% intervals over %d simulated campaigns (%d repetitions each):" % (n_camp, REPS))
    ok = True
    for name in tails:
        print("  %s" % name)
        for key in sorted(hits[name], key=str):
            cov = hits[name][key] / n_camp
            if name.startswith("percentile"):
                good = 0.925 <= cov <= 0.995
            elif name.startswith("narrow"):
                good = cov < 0.80          # the control has to fail the standard
            else:
                good = True                # reported for comparison only
            ok = ok and good
            print("    %-28s %.3f%s" % ("/".join(key), cov, "" if good else "   <-- outside the accepted range"))
    good = 0.95 <= med_cov <= 0.995
    ok = ok and good
    print("  order-statistic interval of a single median (n = %d): %.3f (theory 0.979)%s"
          % (REPS, med_cov, "" if good else "   <-- outside the accepted range"))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
