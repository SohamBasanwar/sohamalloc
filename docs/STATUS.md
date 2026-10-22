# sohamalloc: project status

Last updated: 2026-10-22 17:55 UTC, branch `sohamalloc-dev`.

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

## Stopped at the author's request (2026-10-22 ~17:50 UTC)

Nothing is running. Everything is saved in git.

**Review round 4 (gate 2) has not been done.** Two attempts on `fd735dd` produced no findings:

- `wf_a26b2874-42a` (2026-10-21) failed at the usage limit;
- `wf_f93f99a2-398` (2026-10-22) was stopped at the author's request before any reviewer finished (its journal has no results).

**To run it:**

- **Same session:** resume with `Workflow({scriptPath: "~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/workflows/scripts/sohamalloc-review-round4b-wf_f93f99a2-398.js", resumeFromRunId: "wf_f93f99a2-398"})`.
- **New session:** relaunch that script, which is self-contained.

It expects frozen `fd735dd` libraries in `<session scratchpad>/review4/libs/`. Copy them from `build/`, `build-debug/`, `build-thread/`, `build-armhf/` and `build-aarch64/`, rebuilt from `fd735dd` if needed, and check the sha256 prefixes: release `77ab3a6b`, debug `60def547`, armhf `f052de62`, aarch64 `a0e72684`.

**sigstorm campaign 1 is finished and archived** (`docs/research-log/data/sigstorm-campaign1/`).

- sohamalloc, as the frozen `c1a9791` build: 0 failures in 540 signal trials, 95% CI [0, 0.007].
- Every other allocator failed in 50–100% of its signal trials.
- The 1620 control trials all survived.

**sohamalloc rerun on `fd735dd`: stopped at 456 of 810 trials, all survived.**

- Interim data and the resume command: `docs/research-log/data/sigstorm-c1-soham-fd735dd-partial/README.md`.
- Its frozen library is in `bench/out/sigstorm-c1-soham-fd735dd/` (git-ignored).
- Rerun it on the final code once the round-4 fixes and the prototype below are in.

**Prototype, not applied:** handler-context fallback for large requests. Requests above 16 KiB skip the static arena and take a mapping of their own under trylock.

- It removes the handler NULLs that campaign 1 measured: `large` 95–143 NULLs per 5 s run on `fd735dd`, 0 with the patch, on the release and debug builds.
- Patch, regression test (written, not yet run), measurements and next steps: `docs/research-log/data/proto-handler-fallback/`.
- Apply it with `git apply docs/research-log/data/proto-handler-fallback/handler-solo-fallback.patch`, then follow the README's next steps.

**Research-log update:** not done; its workflow failed at the usage limit with no files written. Script to relaunch: `~/.claude/projects/-home-sohambasanwar-sohamalloc/8c5fee8d-8e37-4816-96c1-7ff48ca35722/workflows/scripts/sohamalloc-research-log-wf_de0bc03b-12d.js`.

**Lost in the reboot of 2026-10-21/22:** the session scratch directory (`/tmp/claude-1000/…`), which held builds and test binaries. Everything needed is in git or rebuildable from it.

## Next

1. **Review round 4** on `fd735dd` (not done yet, see above). Then fix what it confirms, together with the prototype, run the verification matrix, and review again until no critical or major finding is confirmed. Repeat rounds until none confirms a critical or major finding (gate 2).
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
