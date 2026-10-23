# sohamalloc: project status

Last updated: 2026-10-23 16:20 UTC, branch `sohamalloc-dev`.

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

**Gate 2 still needs a clean round. Review round 5 of `d2add24`** is running as workflow run `wf_dba46797-17e`, with the same design as round 4.

- **Frozen libraries:** `<session scratchpad>/review5/libs/`, with a copy in `bench/out/review5-libs-d2add24/`. sha256 prefixes: release `8da10337`, debug `418db9aa`, armhf `0e1b1d6a`, aarch64 `5d127fab`.
- **Journal:** `~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/subagents/workflows/wf_dba46797-17e/`.
- **If interrupted:**
  - **Same session:** `Workflow({scriptPath: "~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/workflows/scripts/sohamalloc-review-round5.js", resumeFromRunId: "wf_dba46797-17e"})`.
  - **New session:** relaunch that script after copying `bench/out/review5-libs-d2add24/*` into the scratchpad's `review5/libs/`. Save finished findings into `docs/research-log/data/review5/` (git-excluded).

**sigstorm, `aligned` handler kind for the other eight allocators** (720 trials; started 23:3x UTC). It complements campaign 1, whose controls stand for the same allocator builds on this machine.

- Output: `bench/out/sigstorm-aligned-others/`. Log: `bench/out/sigstorm-aligned-others.log`.
- **If interrupted:** `python3 tools/sigstorm/run.py --resume bench/out/sigstorm-aligned-others --jobs 4`

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
2. **Campaign.**
   - Rerun sohamalloc's rows of campaign 1 on the final code: all 9 handler kinds, including `aligned`.
   - Run the `aligned` kind for the other eight allocators.
   - Copy the data to `docs/research-log/data/`.
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
