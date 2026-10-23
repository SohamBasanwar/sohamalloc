# sohamalloc: project status

Last updated: 2026-10-23 14:30 UTC, branch `sohamalloc-dev`.

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

## Review round 4 result (2026-10-23)

**Review round 4** of `ae95875` (workflow run `wf_75415e22-635`) finished with 24 findings, 15 confirmed, 7 of them critical or major. **Gate 2 does not hold yet.** 12 refuters failed at the usage limit, so a few findings are still unverified.

- **Raw results:** `docs/research-log/data/review4/` (git-excluded raw notes).
- **Ledger:** a neutral summary goes into `03-defects-and-faults.md` with the fixes.

**Confirmed critical or major, to fix with fail-before/pass-after tests:**

1. **critical:** `fork()` while another thread is inside lazy initialisation leaves the child unable to initialise. Fix: fork-safe init.
2. **critical:** a gap in double-free detection on pages of exited threads. Fix: check the block accounting before such a page is released or claimed.
3. **major:** run and solo frees deferred by signal handlers are not drained by threads that only take the fast paths or only free, nor after the drain at thread exit. Fix: drain in more places; free at once when a trylock succeeds.
4. **major:** allocations in atfork handlers registered before sohamalloc's first use fail inside the fork window. Fix: let the forking thread allocate normally while it holds the locks.
5. **major:** a module that embeds the prefixed archive cannot be unloaded. Fix: delete the thread key in a destructor.

**Unverified, refuter lost:** a missing store-buffering fence in the pending-page handoff, claimed major. Fix anyway, mirroring the notify protocol.

**Confirmed minor:** documentation of §10 after C1-1; a `g_shared_trim` flag race; fork-child stranding of pages with a resolver in flight; data races on configuration read before init completes; double frees into spare pages and of solos not reported; `realloc` keeping a block without the checks that `free` applies; handler-context frees of foreign pointers.

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

1. **Round-4 fixes** (above), then `tools/verify-matrix.sh`, then review round 5. Fix what it confirms, run `tools/verify-matrix.sh`, and review again; repeat until a round confirms no critical or major finding (gate 2).
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
