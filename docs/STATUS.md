# sohamalloc: project status

Last updated: 2026-11-20, branch `sohamalloc-dev`.

## Where things stand

**Benchmarks** (24 workloads against glibc 2.39, jemalloc, mimalloc and tcmalloc; old 4-vCPU cloud VM, 3 repetitions; the raw data and `bench/out/report.md` of that campaign are not on this machine, see `docs/research-log/01-timeline.md` §5):

| allocator | time geomean | RSS geomean | combined score (lower is better) |
|---|---:|---:|---:|
| **sohamalloc** | 1.124 | **1.162** | **1.142** (tied first) |
| mimalloc | **1.063** | 1.227 | 1.142 |
| glibc | 1.491 | 1.092 | 1.276 |
| jemalloc | 1.333 | 1.355 | 1.344 |
| tcmalloc | 1.370 | 1.537 | 1.451 |

That campaign measured commit `273bcb9` plus 64-byte hot-path alignment. No later code has been benchmarked: the tuned campaign on this laptop (gate 4) is still to run.

**The harness is ready for that campaign** (`d9b37ae`, 2026-11-13): 95% confidence intervals in the report (checked by `bench/lib/selftest_stats.py`: 96–98% coverage on 1000 simulated campaigns), `--resume`, core pinning, shuffled order, environment padding, and the host state recorded per campaign. It needs the machine tuned, which takes `sudo`:

```sh
sudo tools/bench-host.sh tune
bench/run_bench.sh --reps 10 --cpus auto --seed 1 --env-pad --strict-host
```

The statistics code has a self-test but has not had an independent review yet; do that before quoting its intervals in a paper.

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

## Review round 4 fixed (2026-10-23)

**Review round 4** of `ae95875` confirmed 7 critical or major findings. All are fixed in the commit after `e490087`, each with a regression test that fails on `ae95875` and passes now:

- **R4-1:** fork during lazy init;
- **R4-2:** a double-free detection gap on pages of exited threads;
- **R4-3:** deferred handler frees not drained;
- **R4-4:** allocations in atfork handlers inside the fork window;
- **R4-5:** a crash after unloading a module that embeds the prefixed archive.

R4-6 (pending-handoff fences, unverified) is fixed by pattern. Of the minors, R4-7 to R4-12 are fixed and R4-13 to R4-16 are corrected in the docs or documented.

- **Details:** `docs/research-log/03-defects-and-faults.md`, "Review round 4".
- **Before/after:** `data/regress-before-after/2026-10-23-review4-before-after.txt`.
- **Matrix:** `data/verify/2026-10-23-review4-fixes-matrix.txt`:
  - native, debug, aarch64 and armhf 26/26 each;
  - prefixed 11/11;
  - musl 25/25 + 1 skipped;
  - TSan, ASan and UBSan 9/9 + 2 skipped by design;
  - preload 9/9;
  - mingw 9/9 + 2 skipped;
  - sigstorm 66/66.

**Gate 2 still needs a clean round. Review round 5 of `d2add24` now runs in waves** (plan of 2026-11-13). Three earlier attempts (2026-10-23, 2026-10-29, 2026-11-05) launched every reviewer at once and were cut off by usage limits or stopped with nothing finished.

- **Why waves.** Measured on 2026-11-13: an agent in this setup starts with about 60,000 tokens of fixed context and re-reads it on every turn, and 27 small test agents used up a whole usage window. Seven deep reviewers plus their refuters cannot fit in one window.
- **Design.** Seven lenses, two per wave, each wave a workflow of its own: reviewers at effort high; one refuter per finding at medium; a second, independent refuter at high for a finding claimed or judged critical or major; a tie-breaker at xhigh when those two disagree. Reviewers keep a `NOTES.md` in their scratch directory. After the last wave, and only if nothing blocking was confirmed, a completeness critic names gap areas.

  | Wave | Lenses | Status |
  |---|---|---|
  | 1 (pilot) | signal-reentrancy | running since 2026-11-20, fourth launch, workflow run `wf_5344e75c-add`, on Opus 5.5; it continues from `bench/out/review5-work/signal-reentrancy/NOTES.md`. Earlier launches: `wf_14912398-7ae` and `wf_2dbcefe8-aef`, cut off by the session limit after 10 and 20 minutes; `wf_1cc28c75-b3f` (2026-11-15), cut off by the Fable model's usage limit after 9.5 minutes (216,251 tokens, 13 tool calls, no result returned) |
  | 2 | concurrency, lifecycle-fork-init | not started |
  | 3 | api-and-memory, portability-and-failure-paths | not started |
  | 4 | hardening-checks, page-heap-and-release | not started |
  | 5 | completeness critic and gap reviewers | not started |

  Wave 1 is a single reviewer on purpose: its token cost, read from the workflow result, decides how many reviewers fit in one usage window.

- **Script:** `~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/workflows/scripts/sohamalloc-review-round5.js`, launched as `Workflow({scriptPath: "<script>", args: {lenses: ["<lens>", "<lens>"]}})`. A copy of the script is kept in `docs/research-log/data/review5/` (git-excluded).
- **Frozen libraries:** `bench/out/review5-libs-d2add24/` (the script reads them there). sha256 prefixes: release `8da10337`, debug `418db9aa`, armhf `0e1b1d6a`, aarch64 `5d127fab`.
- **Results:** saved per wave into `docs/research-log/data/review5/wave<N>.json` as soon as a wave finishes.
- **Interim findings of wave 1, from the reviewer's notes; not yet checked by a refuter:**
  - S1, reproduced by the reviewer: when a process's first use of the allocator is `fork()`, `fork_prepare` runs the lazy init without the busy byte set. A signal handler on that thread that allocates inside two windows of a few instructions each then waits forever.
  - S2, reproduced by the third launch in 2 of 6 runs: a run freed by a handler that interrupted a fast path is deferred when another thread holds the page-heap lock, and no fast path drains it. Kept 1,600 KiB and 43,520 KiB until the thread's next slow path. The variant with a single thread that only frees passed 6 of 6. The R4-3 fix may be incomplete.
  - S5, minor: the pending drain runs in refills and `soham_collect`, not in "the next slow path of any thread" as the notes say.
  - Not diagnosed: the reviewer's single-step harness, run on the debug library, died of SIGTRAP after its last section. It is not yet known whether the trap comes from the harness or from the library.
- **Why the launches died.** The first two (2026-11-15): the main session's conversation had grown to about 624,000 tokens, re-read at every step. That day its ten steps used 1.24 million fresh input tokens and 4.3 million cached ones, against the reviewer's 0.43 and 2.8 million. The third: the Fable model's own usage limit, which also blocked `/compact`. On 2026-11-20 the author switched the main model to Opus 5.5, which workflow agents inherit. Keep the main session small (`/compact`, or a new session started from this file); the review itself is not the main cost.
- **Working folders:** `bench/out/review5-work/<lens>/` (git-ignored, survives a reboot). Each reviewer keeps a `NOTES.md` there and continues from it when its wave is launched again.
- **If a wave is interrupted:** launch it again with the same `args`. In the same session add `resumeFromRunId: "<run id of that wave>"`, and finished agents replay from the journal; in a new session the reviewers pick up their `NOTES.md`.

**sohamalloc rerun of campaign 1 on `d2add24`: finished 2026-11-04** and archived in `docs/research-log/data/sigstorm-c1-soham-d2add24/`.

- 0 of 630 signal trials failed, 95% CI [0, 0.006]; 0 of 180 controls failed.
- 140 million handler calls, 2.7 million of them inside an allocator call.
- Handler NULL results: `large` 31 of 19.9 million (13,838 on `c1a9791`), `churn` 2, every other kind 0.

**sigstorm, `aligned` handler kind for the other eight allocators** (720 trials): finished and archived in `docs/research-log/data/sigstorm-aligned-others/` (`bc13653`). Failed trials of 90: glibc, jemalloc, tcmalloc, tbbmalloc and hardened_malloc 90 each, mimalloc 85, snmalloc 76, scudo 39.

**Done since the last checkpoint:**

- **C1-1, committed in `ae95875`.** Handler requests above 16 KiB skip the static arena; anything the shared heap and the arena cannot serve gets a solo mapping under trylock.
  - Regression test `signal_large_fallback`: FAIL 3/3 on `fd735dd` (about 45% NULL), PASS 6/6 fixed.
  - Matrix all green: native, debug, aarch64 and armhf 24/24; musl 23/23 + 1 skipped; TSan, ASan and UBSan 9/9; preload 9/9; mingw 9/9; sigstorm 66/66, now with the `aligned` kind.
  - Record: `docs/research-log/data/verify/2026-10-22-c1-1-fix-matrix.txt`.
  - The matrix is now a script: `tools/verify-matrix.sh OUTDIR`.
- **Tooling audit, committed in `6e4527e`.** `docs/TOOLING_AUDIT.md`. Its recommendations wait for the author's decision; nothing has been changed:
  - remove the claude-mem stack, the headroom-ai MCP server and OmniRoute;
  - never install FreeLLMAPI;
  - make Task Observer manual-only and disable Claude Code Setup;
  - back up the local-only evidence;
  - add deny rules and a commit-msg guard.
- **sigstorm campaign 1** is finished and archived. sohamalloc on `c1a9791`: 0 failures in 540 signal trials, 95% CI [0, 0.007].
- **The partial rerun on `fd735dd`** (456 of 810 trials, all survived) is superseded by `ae95875`. Rerun the campaign's sohamalloc rows on the final code after review round 4.

## Next

1. **Review round 5** on the round-4-fix commit; repeat rounds until none confirms a critical or major finding (gate 2). Fix what it confirms, run `tools/verify-matrix.sh`, and review again; repeat until a round confirms no critical or major finding (gate 2).
2. **Campaign.** Done on `d2add24`: the sohamalloc rerun with all 9 handler kinds and the `aligned` kind for the other eight allocators are archived. Rerun sohamalloc's rows if review round 5 changes allocator code.
3. **Busy-byte cost** measurement, then the tuned benchmark campaign: ≥10 repetitions with confidence intervals, on a quiet machine with `sudo tools/bench-host.sh tune`.
4. **Research log.** Complete `02-experiments`, `04-design-decisions` and `06-paper-assessment`, and extend `01-timeline` past 2026-10-19.
5. **Paper and presentation** only after the four gates hold.

## How to build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
tests/run_tests.sh --lib build/libsohamalloc.so      # full suite
tests/preload_smoke.sh --lib build/libsohamalloc.so  # real programs
```

The full verification matrix: `tools/verify-matrix.sh OUTDIR` (the steps of the `/sohamalloc-verify` skill as one script).
