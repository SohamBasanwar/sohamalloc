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

**Tests** on the round-5 fixes with the refuters' follow-ups: the commit that added `docs/research-log/data/verify/2026-11-22-review5-refuter-fixes-matrix.txt` (2026-11-22). The counts are the same as on `668e128`:

| Suite | Result |
|---|---|
| native | 28/28 |
| debug (SOHAM_DEBUG) | 28/28 |
| prefixed | 11/11 |
| TSan, ASan, UBSan | 9/9 + 2 skipped by design, each |
| real programs under LD_PRELOAD | 9/9 |
| aarch64, armhf (qemu) | 26/26 + 2 skipped (the two x86-64-only tests), each |
| musl | 27/27 + 1 skipped (C++) |
| mingw (wine) | 9/9 + 2 skipped |
| sigstorm on the debug build | 66/66 survived |

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

**Gate 2 still needs a clean round.** Review round 5 of `d2add24` ran in waves: two lenses per wave, each wave a workflow of its own, so that it fits in one usage window (plan of 2026-11-13).

- **Wave 1 (pilot: the signal-reentrancy lens) finished on 2026-11-20.** Workflow `wf_5344e75c-add`, the fourth launch, ran on Opus 5.5; the first three were cut off by usage limits. It made 4 findings, all confirmed by refuters:
  - **R5-1, critical:** a handler that allocates during the lazy init that `fork()` runs (the process's first allocator use) hangs.
  - **R5-2, major:** runs freed by a handler that interrupted a fast path stay allocated until the thread's next slow path. The R4-3 fix was incomplete.
  - **R5-3 and R5-4, minor:** documentation (the pending drain; `nothrow` new with a `new_handler` installed).
  - Details: `docs/research-log/03-defects-and-faults.md`, "Review round 5". Raw result: `docs/research-log/data/review5/wave1.json` (git-excluded).
- **Cost of the pilot:** 7 agents, 714,094 subagent tokens, 81 tool calls, 17 minutes. The reviewer took 63 turns and 107,056 output tokens; each refuter 7 to 20 turns and 4,000 to 16,000 output tokens. A wave of two lenses should cost about 1.4 million.
- **Fixes of 2026-11-21, commit `668e128`** (pushed at the author's request before the matrix finished; the full matrix then passed on exactly that code, see the table above):
  - R5-1 and R5-2 are fixed in `src/api.c`; R5-3 and R5-4 are corrected in `docs/IMPLEMENTATION_NOTES.md`.
  - New regression tests `regress_sig_fork_init` and `regress_sig_fastpath` (x86-64 Linux) fail on both `d2add24` libraries and pass on the fixed ones: `docs/research-log/data/regress-before-after/2026-11-21-review5-before-after.txt`.
  - The fast-path check costs 3 instructions per malloc and per free (callgrind). Its time cost is for the gate-4 campaign to measure.
- **Decision (2026-11-21): round 5 stops after wave 1.** Its blocking findings are fixed first. The remaining six lenses, and the signal lens again, then review the fixed commit as round 6. Reason: round 5 can no longer be the clean round, and reviewing `d2add24` further would still leave a full round to run after the fixes.
- **Independent refuters of the two fixes** (script `~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/workflows/scripts/sohamalloc-verify-r5-fixes-wf_29c0820d-a59.js`; work folders `bench/out/review5-work/verify-r5-1/` and `verify-r5-2/`, each with a `NOTES.md`):
  - **R5-2 holds** (`wf_29c0820d-a59`). Its two minor gaps were closed on 2026-11-22, and the matrix passed on the result: a drain after `ensure_init`'s busy region, and a test call that exercises the drain after the solo lookup.
  - **R5-1: not finished.** The session ended under it twice (2026-11-21, and in the night to 2026-11-22, when the machine rebooted for a kernel update). Its results so far, from `verify-r5-1/NOTES.md`:
    - Handlers that allocate and free at every instruction of fork()'s prepare handler hang neither on the fixed release library nor on the fixed debug one. `d2add24` hangs in the R5-1 window.
    - `fork()` called from a handler inside the init windows hangs both before and after the fix. That case is §10 limit 6, and the fix did not make it worse.
    - Still to check: the takeover path, concurrent forks, `ensure_init`, and whether the test could pass on broken code.

    To resume, launch `Workflow({scriptPath: "<script>", args: {keys: ["r5-1-fork-init"]}})`; it continues from its notes.
- **Round 6 plan:** the round-5 script with the commit and the library folder changed (`bench/out/review6-libs-<commit>/`). Waves of two lenses:
  1. signal-reentrancy and concurrency;
  2. lifecycle-fork-init and api-and-memory;
  3. portability-and-failure-paths and hardening-checks;
  4. page-heap-and-release;
  5. the completeness critic and gap reviewers.
- **Script:** `~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/workflows/scripts/sohamalloc-review-round5.js`, launched as `Workflow({scriptPath: "<script>", args: {lenses: ["<lens>", "<lens>"]}})`. A copy of the script is kept in `docs/research-log/data/review5/` (git-excluded).
- **Frozen libraries of round 5:** `bench/out/review5-libs-d2add24/` (the script reads them there). sha256 prefixes: release `8da10337`, debug `418db9aa`, armhf `0e1b1d6a`, aarch64 `5d127fab`.
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

1. **When the R5-1 refuter finishes:** fix what it confirms, run the matrix on the working tree, commit and push. Then freeze that commit's libraries into `bench/out/review6-libs-<commit>/` and **review round 6** on it, in the waves above. Fix what it confirms, run `tools/verify-matrix.sh`, and review again, until a round confirms no critical or major finding (gate 2).
2. **Campaign.** Done on `d2add24`: the sohamalloc rerun with all 9 handler kinds and the `aligned` kind for the other eight allocators are archived. The round-5 fixes change allocator code, so sohamalloc's rows must be rerun on the final code.
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
