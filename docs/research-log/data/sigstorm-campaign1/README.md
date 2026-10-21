# sigstorm campaign 1 (2026-10-21)

The raw data of the first full sigstorm campaign. It is copied here from `bench/out/sigstorm-campaign1/`, which is git-ignored.

**Design.** 9 allocators × 8 handler kinds × 1, 2 and 4 workload threads × 30 trials = 6480 trials of 5 s.

- The handler kinds include the two controls: `nosignal` (no signals) and `none` (signals, with a handler that does not allocate).
- The exact options are in `args.txt`.

**When.** The campaign started 2026-10-21T17:41:00Z and ended 21:44:35Z. It was interrupted twice and resumed with `--resume`; `env.txt` records each resumption with its load average.

**What was measured.** Allocator versions and sha256 checksums are in `env.txt`.

- **sohamalloc** is a frozen build of commit `c1a9791`, the code before the review-round-3 fixes (sha256 `303a23d1…`). Its rows are superseded by a rerun on the fixed code. The other allocators' rows stand.
- **Tool.** `sigstorm.c` is the tool source the campaign used (commit `85f3b78`). The `aligned` handler kind did not exist yet.

**Files:**

| File | Content |
|---|---|
| `results.csv` | One row per trial, as written by `tools/sigstorm/run.py` |
| `report.md` | Regenerated with `tools/sigstorm/run.py --report-only` at the commit that added this file. It adds the handler-results table (NULL and wrong results of handler calls). It also counts trials whose own counters are impossible as corruption: one snmalloc trial (trial 24, 1 thread, `large`) that exited "survived" with a heap address written over its counters |
| `report-as-written.md` | The report the runner wrote at the end of the campaign, before those two changes |
| `campaign.log` | The runner's progress log, one line per trial |

**Load.** The test matrix and review workflows ran at the same time, at load averages up to about 15 on 22 CPUs. The outcome measure, a thread making no progress for 2 s, is insensitive to CPU contention. The 1620 control trials all survived.
