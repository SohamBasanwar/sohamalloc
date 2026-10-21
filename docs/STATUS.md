# sohamalloc: project status

Last updated: 2026-10-21 21:40 UTC, branch `sohamalloc-dev`.

## Where things stand

**Benchmarks** (24 workloads against glibc 2.39, jemalloc, mimalloc and tcmalloc; old 4-vCPU cloud VM, 3 repetitions; report `bench/out/report.md`):

| allocator | time geomean | RSS geomean | combined score (lower is better) |
|---|---:|---:|---:|
| **sohamalloc** | 1.124 | **1.162** | **1.142** (tied first) |
| mimalloc | **1.063** | 1.227 | 1.142 |
| glibc | 1.491 | 1.092 | 1.276 |
| jemalloc | 1.333 | 1.355 | 1.344 |
| tcmalloc | 1.370 | 1.537 | 1.451 |

That campaign measured commit `273bcb9` plus 64-byte hot-path alignment. No later code has been benchmarked: the tuned campaign on this laptop (gate 4) is still to run.

**Tests** on the commit with the review-round-3 fixes (record: `docs/research-log/data/verify/2026-10-21-review3-fixes-matrix.txt`):

| Suite | Result |
|---|---|
| native | 23/23 |
| debug (SOHAM_DEBUG) | 23/23 |
| TSan, ASan, UBSan | 9/9 each |
| real programs under LD_PRELOAD | 9/9 |
| aarch64, armhf (qemu) | 23/23 each |
| musl | 22/22 + 1 skipped (C++) |
| mingw (wine) | 9/9 |
| sigstorm on the debug build | 54/54 survived |

## Review rounds

| Round | Code | Confirmed defects | Status |
|---|---|---|---|
| 2 | `ee1f70d` (sources unchanged up to `f5e37a2`) | R2-1 … R2-5 (2 critical, 1 major, 2 minor) | fixed in `c1a9791` |
| 3 | `c1a9791` | R3-1 critical, R3-2 / R3-3 / R3-4 major, R3-5 / R3-6 minor | critical and major fixed in this commit; the minors are recorded as limitations |

Details: `docs/research-log/03-defects-and-faults.md`. Each fix has a regression test that fails on the old code and passes on the new (`data/regress-before-after/2026-10-21-review3-before-after.txt`).

Round 3 fixes:

- **R3-1:** the busy byte is saved per thread across `fork`.
- **R3-2:** aligned allocations in handler context use the shared heap.
- **R3-3:** the static-arena counter is bounded.
- **R3-4:** the shared heap is trimmed by other threads' refills and flushed by `malloc_trim` / `soham_collect(1)`.

The limits of the signal-safety guarantee are documented in `docs/IMPLEMENTATION_NOTES.md` §10 and in the README.

## Running now

**sigstorm campaign 1** (`tools/sigstorm/run.py`, 6480 trials):

- 9 allocators × 8 handler kinds (including the controls) × 1, 2 and 4 threads × 30 trials.
- Started 17:41 UTC. It was interrupted and resumed at 21:2x UTC with 3519 trials done.
- sohamalloc is the frozen copy of the `c1a9791` code (before the round-3 fixes). Its rows are rerun on the fixed code afterwards; the other allocators' rows stand.
- **Data:** `bench/out/sigstorm-campaign1/` (git-ignored; copy to `docs/research-log/data/sigstorm-campaign1/` when it finishes). Log: `bench/out/sigstorm-campaign1.log`.
- **If interrupted:** `SOHAM_LIB=$PWD/bench/out/sigstorm-campaign1/libsohamalloc-064d208.so python3 tools/sigstorm/run.py --resume bench/out/sigstorm-campaign1 --jobs 5`

**Review round 4** of `fd735dd`, started 21:4x UTC as workflow run `wf_a26b2874-42a`:

- **Design.** Five lens reviewers (signal reentrancy, concurrency, lifecycle/fork/init, API and memory return, portability and checks). Each finding gets two refuters (reproduction, code reasoning), plus a tie-breaker on a split. A completeness critic then assigns up to four gap reviewers, whose findings are verified the same way.
- **Where its work is.** Frozen libraries: `<session scratchpad>/review4/libs/`. Reviewers' working directories: `<session scratchpad>/review4/<lens>/`, under `/tmp/claude-1000/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/scratchpad/`. Journal (completed agents) and transcripts: `~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/subagents/workflows/wf_a26b2874-42a/`.
- **If interrupted:**
  - **Same session:** `Workflow({scriptPath: "~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/workflows/scripts/sohamalloc-review-round4-wf_a26b2874-42a.js", resumeFromRunId: "wf_a26b2874-42a"})`.
  - **New session:** salvage the findings from `journal.jsonl` and the agent transcripts. Save them to `docs/research-log/data/review4/` (raw notes stay out of git, as in round 3).

## Next

1. **Review round 4** on this commit (running, see above). Repeat rounds until none confirms a critical or major finding (gate 2).
2. **Campaign.** Finish campaign 1. Rerun sohamalloc's rows on the fixed code. Add an `aligned` handler kind to sigstorm (round 3 showed the gap) and run it for all allocators. Copy the data to `docs/research-log/data/`.
3. **Busy-byte cost** measurement, then the tuned benchmark campaign: ≥10 repetitions with confidence intervals, on a quiet machine with `sudo tools/bench-host.sh tune`.
4. **Research log.** Complete `02-experiments`, `04-design-decisions` and `06-paper-assessment`, and extend `01-timeline` past 2026-10-19.
5. **Paper and presentation** only after the four gates hold.

## How to build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
tests/run_tests.sh --lib build/libsohamalloc.so      # full suite
tests/preload_smoke.sh --lib build/libsohamalloc.so  # real programs
```

The full verification matrix is the `/sohamalloc-verify` skill (`~/.claude/skills/sohamalloc-verify`).
