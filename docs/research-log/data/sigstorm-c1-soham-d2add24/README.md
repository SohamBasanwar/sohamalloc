# sohamalloc rerun of campaign 1 on the round-4-fix code `d2add24`

**What it is.** Campaign 1's design for sohamalloc only:

- the 8 handler kinds of campaign 1 plus `aligned`, × 1, 2 and 4 threads × 30 trials of 5 s, which is 810 trials (630 with an allocating handler, 180 controls);
- the frozen library `bench/out/sigstorm-c1-soham-d2add24/libsohamalloc-d2add24.so`, sha256 `8da10337…` (full sum in `env.txt`), built from `d2add24`, the code with the review-round-4 fixes;
- the tool `tools/sigstorm` as in `sigstorm.c`, with the `aligned` kind.

It supersedes the sohamalloc rows of campaign 1 (`../sigstorm-campaign1/`, code `c1a9791`) and the partial rerun on `fd735dd` (`../sigstorm-c1-soham-fd735dd-partial/`). The other allocators' rows of campaign 1, and `../sigstorm-aligned-others/`, stand.

**Timeline:**

| When (UTC) | Event |
|---|---|
| 2026-10-29 23:27 | started |
| 2026-10-29/30 | stopped with the session at 652 of 810 trials |
| 2026-11-04 17:05 | resumed with `--resume` |
| 2026-11-04 17:08 | ended |

`env.txt` records each start with its load average. The outcome measure, a thread making no progress for 2 s, is insensitive to CPU contention.

**Result** (full tables in `report.md`):

| | Trials | Failed | P(fail) [95% Wilson CI] |
|---|---:|---:|---|
| allocating handler kinds, pooled | 630 | 0 | 0.000 [0.000, 0.006] |
| controls (`nosignal`, `none`) | 180 | 0 | 0.000 [0.000, 0.021] |

- 140,285,748 handler calls completed; 2,676,467 of them interrupted an allocator call of the workload.
- **Handler allocations that returned NULL** (not counting the `free` handler's empty-pool counts): `large` 31 of 19,941,425 calls, `churn` 2 of 20,097,150, every other kind 0. No wrong results.

**Compared with the earlier runs** (handler NULL results):

| Run | Code | `large` | `churn` | `aligned` |
|---|---|---:|---:|---:|
| campaign 1, 540 trials | `c1a9791` | 13,838 of 20,305,876 | 12 | (kind not yet in the tool) |
| partial rerun, 456 trials | `fd735dd` | 6,743 | 70 | 39 |
| this run, 810 trials | `d2add24` | 31 of 19,941,425 | 2 | 0 |

The drop in `large` comes from the C1-1 fix (`ae95875`): handler requests above 16 KiB skip the static arena, and what the shared heap and the arena cannot serve gets a solo mapping under trylock. A handler allocation may still return NULL when a lock it needs is held (documented limit, `docs/IMPLEMENTATION_NOTES.md` §10).

**Files:**

| File | Content |
|---|---|
| `results.csv` | One row per trial, as written by `tools/sigstorm/run.py` |
| `report.md` | The report the runner wrote at the end |
| `campaign.log` | The runner's progress log, one line per trial |
| `args.txt`, `env.txt` | Options; machine, compiler, library path and sha256, starts and end |
| `sigstorm.c` | The tool source the run used |

**To reproduce:** build `d2add24` (check the sha256 against `env.txt`), then

```sh
SOHAM_LIB=$PWD/build/libsohamalloc.so python3 tools/sigstorm/run.py --allocs sohamalloc \
  --handlers nosignal,none,malloc,free,realloc,calloc,large,churn,aligned \
  --trials 30 --seconds 5 --threads 1,2,4 --out bench/out/<dir> --jobs 4
```
