# sohamalloc: project status

Last updated: 2026-10-22 17:40 UTC, branch `sohamalloc-dev`.

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

## Running now (2026-10-22 17:40 UTC)

**What happened since 2026-10-21 21:47 UTC:**

1. **Usage limit.** The session hit its usage limit. Both workflows then running failed, with no agent finished: review round 4 (`wf_a26b2874-42a`) and the research-log update (`wf_de0bc03b-12d`). The research-log writers left no files behind (`git status` was clean).
2. **Reboot.** The machine rebooted overnight. That killed the sohamalloc campaign rerun at 168 of 810 trials, all survived, and wiped the session scratch directory: the unfixed-code build, the frozen review libraries and the test binaries.
   - Nothing that matters was lost: the before/after records, the matrix record and campaign 1 were already in `docs/research-log/data/`.
   - The frozen review libraries were restored from the repository's build directories, built from `fd735dd`. Their sha256 match the earlier copies: release `77ab3a6b`, debug `60def547`, armhf `f052de62`, aarch64 `a0e72684`.

**sigstorm campaign 1 is finished** (6480 trials, 2026-10-21 17:41–21:44 UTC). Archived in `docs/research-log/data/sigstorm-campaign1/` with a README.

- sohamalloc, as the frozen `c1a9791` build: 0 failures in 540 signal trials, 95% CI [0, 0.007].
- Every other allocator failed in 50–100% of its signal trials.
- The 1620 control trials all survived.

**sohamalloc rerun on the fixed code `fd735dd`** (810 trials: the 8 handler kinds of campaign 1 plus `aligned`, × 1, 2 and 4 threads × 30):

- Directory `bench/out/sigstorm-c1-soham-fd735dd/`, which holds the frozen library `libsohamalloc-fd735dd.so` (sha256 `77ab3a6b…`). Log: `bench/out/sigstorm-c1-soham-fd735dd.log`.
- Resumed 17:33 UTC.
- **If interrupted:** `SOHAM_LIB=$PWD/bench/out/sigstorm-c1-soham-fd735dd/libsohamalloc-fd735dd.so python3 tools/sigstorm/run.py --resume bench/out/sigstorm-c1-soham-fd735dd --jobs 4`

**Review round 4 of `fd735dd`**, relaunched 17:3x UTC as workflow run `wf_f93f99a2-398`. It is leaner than the first attempt, to stay within the usage limit:

- the same five lenses, at reasoning effort "high";
- one combined refuter per finding, plus a second independent refuter for anything claimed or judged critical or major, and a tie-breaker on a split;
- the completeness critic and gap reviewers run only when no critical or major finding is confirmed.

Reviewers include each reproducer's source in their result, because scratch directories do not survive a reboot.

- **Journal and transcripts:** `~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/subagents/workflows/wf_f93f99a2-398/`.
- **If interrupted:**
  - **Same session:** `Workflow({scriptPath: "~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/workflows/scripts/sohamalloc-review-round4b-wf_f93f99a2-398.js", resumeFromRunId: "wf_f93f99a2-398"})`.
  - **New session:** salvage the findings from `journal.jsonl` into `docs/research-log/data/review4/`, which stays out of git as raw notes.

**Research-log update:** to be relaunched after review round 4. Its script: `~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/workflows/scripts/sohamalloc-research-log-wf_de0bc03b-12d.js`.

## Next

1. **Review round 4** on `fd735dd` (running, see above). Repeat rounds until none confirms a critical or major finding (gate 2).
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
