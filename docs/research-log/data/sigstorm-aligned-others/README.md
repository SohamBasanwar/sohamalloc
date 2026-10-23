# sigstorm: the `aligned` handler kind for the other eight allocators (2026-10-22/23)

**What it is.** The `aligned` handler kind (posix_memalign and aligned_alloc from a signal handler, alignments 32 B .. 64 KiB, added in `f3809c0`) run against glibc, jemalloc, mimalloc, tcmalloc, tbbmalloc, scudo, snmalloc and hardened_malloc.

- 1, 2 and 4 workload threads × 30 trials of 5 s each: 720 trials.
- **When:** started 2026-10-22 ~23:3x UTC, ended 2026-10-23 03:08:10 UTC.

**Controls.** This run has none, by design. The controls (`nosignal`, `none`) of campaign 1 (`../sigstorm-campaign1/`) stand: same allocator builds (the sha256 sums in both `env.txt` files), same machine, same tool, apart from the added handler kind. All 1620 control trials of campaign 1 survived.

**sohamalloc.** Its rows for this kind are part of the sohamalloc rerun on the final code, which is still to run.

**Result** (failed trials of 90 per allocator; full tables in `report.md`):

| Allocator | Failed |
|---|---|
| glibc, jemalloc, tcmalloc, tbbmalloc, hardened_malloc | 90 each |
| mimalloc | 85 |
| snmalloc | 76 |
| scudo | 39 |

`report.md` was regenerated with `run.py --report-only`, which adds the handler-results table and the harness-counter check.
