# sohamalloc: project status

Last updated: 2026-10-19 (UTC), commit after `ead7c98`, branch `sohamalloc-dev`.

## Where things stand

**Benchmarks** (24 workloads against glibc 2.39, jemalloc, mimalloc and tcmalloc; 4-vCPU VM, 3 repetitions; report `bench/out/report.md`, data copied to `docs/research-log/data/` once the research log is complete):

| allocator | time geomean | RSS geomean | combined score (lower is better) |
|---|---:|---:|---:|
| **sohamalloc** | 1.124 | **1.162** | **1.142** (tied first) |
| mimalloc | **1.063** | 1.227 | 1.142 |
| glibc | 1.491 | 1.092 | 1.276 |
| jemalloc | 1.333 | 1.355 | 1.344 |
| tcmalloc | 1.370 | 1.537 | 1.451 |

- That campaign measured commit `273bcb9` plus 64-byte hot-path alignment. The single busy-byte fast path (`13bcf17`) has not been benchmarked yet.
- sohamalloc is the only allocator that is never the worst on memory in any benchmark.
- At the start of this round the score was 1.241 (commit `5ff3c75`).

**Tests** (all passing on the current commit):

| target | result |
|---|---|
| x86-64 glibc | 15/15 |
| debug build | 15/15 |
| aarch64 (qemu) | 15/15 |
| armhf (qemu) | 15/15 |
| musl | 14/14 + 1 skipped |
| Windows (mingw + wine) | 8/8 |
| ThreadSanitizer, AddressSanitizer, UBSan | 8/8 each |
| real programs under LD_PRELOAD | 9/9 |

## Open threads

1. **Adversarial review (gate G4)** of the code at `13bcf17`, run as a four-lens review with refutation-based verification. Its confirmed findings still need fixing and re-verifying.
2. **Research record** for a possible paper, in `docs/research-log/`:
   - `01-timeline`, `02-experiments`, `03-defects-and-faults`, `04-design-decisions`;
   - `05-related-work-and-novelty` and `06-paper-assessment`.

   They are reconstructed from the session record, git history and benchmark data. If a file is missing, that step did not finish.
3. **Remaining performance gaps:**
   - larson is about 1.1–1.2× slower than the fastest allocator;
   - xmalloc-testN is 1.23×;
   - alloc-test is 1.15–1.2×;
   - sh6bench is 1.3× slower, with 1.23× its RSS;
   - rptest uses 1.55× the RSS of the leanest allocator.
4. **Benchmark the current commit**, then build the presentation (architecture diagrams, a step-by-step workflow diagram, the results).

## How to resume

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
tests/run_tests.sh --lib build/libsohamalloc.so      # full suite
tests/preload_smoke.sh --lib build/libsohamalloc.so  # real programs
bench/build_bench.sh && bench/run_bench.sh --reps 3  # full benchmark (~15 min)
```

## Review round 2 (2026-10-21): fixed and validated

The round was interrupted once, resumed, and then stopped early at the author's request to save the usage budget. The four agents were:

- the correctness review, which finished;
- the concurrency review, which reproduced its findings before it stopped;
- two verifiers, one interrupted and one restarted.

All five confirmed defects are fixed, each with a regression test that fails on the old code and passes on the new. The ledger with root causes and evidence is `docs/research-log/03-defects-and-faults.md`.

| Defect | Severity | Fix |
|---|---|---|
| R2-1: handler-context deadlock | critical | The refill's trim is skipped in trymode. New invariant H3: no blocking lock in trymode, and SOHAM_DEBUG builds abort if one is taken |
| R2-2: notify-stack stranding after fork | major | heap_attach reopens only a closed stack |
| R2-3: orphan-page remote double free A,B,A | critical, security | resolve() walks the chain before releasing |
| R2-4: TSan race in soham_get_stats | minor | Counters read under g_ph.lock |
| R2-5: TSan race on g_geo.wshift / gmap during init | minor | Acquire mask first, atomic wshift, release/acquire gmap |

**Validation of the committed code:**

| Suite | Result |
|---|---|
| Native | 19/19 |
| Debug | 19/19 |
| Preload of real programs | 9/9 |
| TSan, ASan, UBSan | 9/9 each |
| aarch64 | 19/19 |
| armhf | 19/19 |
| musl | 18/18 + 1 skipped (C++) |
| mingw | 9/9 |

**sigstorm on the debug build** (the H3 assertion is active): 0 failures in 54 runs. The runs covered 7 handler kinds × 1, 2 and 4 threads × 2 seeds × 4 s, plus 12 runs with the page-heavy workload. sigstorm did *not* detect R2-1 on the unfixed code (ledger, "detection power"), so these runs are evidence, not proof.

## Next (directive of 2026-10-21)

1. A new review round on this code. Repeat until no critical or major finding is confirmed.
2. The full sigstorm campaign:
   - 9 allocators × 7 handler kinds × 1, 2 and 4 threads;
   - at least 30 trials each, plus the controls;
   - raw data in `docs/research-log/data/`.
3. Document the limits of the signal-safety guarantee in IMPLEMENTATION_NOTES and README.
4. Measure the busy-byte cost.
5. Survey real software that allocates in signal handlers.
6. A tuned benchmark campaign with ten or more repetitions and confidence intervals.
7. Complete the research log.
8. The paper and the presentation, only after the gates.

### Checkpoint (2026-10-21 12:12 CDT): review round 3 running

- **Run.** Workflow run `wf_107e841a-152` (4 agents) reviews HEAD c1a9791.
- **Where its work is.** Agent transcripts and the journal (completed results):
  `~/.claude/projects/-home-sohambasanwar-sohamalloc/d02fd376-691d-4e43-9867-b889f9bfad80/subagents/workflows/wf_107e841a-152/`.
  Repros: `/tmp/claude-1000/-home-sohambasanwar-sohamalloc/d02fd376-691d-4e43-9867-b889f9bfad80/scratchpad/review3-*`.
- **If interrupted**, two ways to recover:
  - **Same session:** resume with
    `Workflow({scriptPath: ".../workflows/scripts/sohamalloc-review-round3-wf_107e841a-152.js", resumeFromRunId: "wf_107e841a-152"})`.
    Completed agents are reused.
  - **New session:** salvage the findings from journal.jsonl and the agent transcripts.

### Checkpoint (2026-10-21 12:45 CDT): sigstorm campaign 1 running

- **Run.** `tools/sigstorm/run.py --trials 30 --seconds 5 --threads 1,2,4 --jobs 5 --out bench/out/sigstorm-campaign1`, started 17:41 UTC under `nice -n 10`.
  - 9 allocators × 3 thread counts × 8 handler kinds (including the controls `nosignal` and `none`) × 30 trials = 6480 trials.
  - Tools at commit 85f3b78.
  - sohamalloc is the frozen copy `bench/out/sigstorm-campaign1/libsohamalloc-064d208.so` of the c1a9791 code, sha256 `303a23d1…`.
  - Its rows are rerun on the final code after the review-round-3 fixes. The other allocators' rows stand.
- **Where its data is.** `bench/out/sigstorm-campaign1/` (results.csv, appended per trial; env.txt; args.txt), with the log in `bench/out/sigstorm-campaign1.log`. `bench/out/` is git-ignored: copy the text files to `docs/research-log/data/sigstorm-campaign1/` when it finishes, or at a checkpoint.
- **If interrupted:** `SOHAM_LIB=$PWD/bench/out/sigstorm-campaign1/libsohamalloc-064d208.so python3 tools/sigstorm/run.py --resume bench/out/sigstorm-campaign1 --jobs 5`. It skips completed trials and records the resumption in env.txt.
- **Concurrent load.** Review round 3 runs at the same time. The outcome measure (stall = no progress for 2 s) is insensitive to CPU contention. The `inside` exposure counts and the load averages in env.txt show the load.

### Checkpoint (2026-10-21 13:00 CDT): usage limit at 99%, work saved

- **Review round 3** (`wf_107e841a-152`).
  - The correctness review finished with 5 findings, **not yet verified**; its verifier is running. They are saved in `docs/research-log/data/review3/correctness-findings-unverified.json`:

    | Severity | Finding |
    |---|---|
    | critical | `fork()` from several threads: the single global `g_fork_saved_busy` |
    | major | The shared (handler) heap is never released |
    | major | `bss_alloc` offset wrap on ILP32 |
    | minor | Address-space waste under `RLIMIT_AS` |
    | minor | Link keys predictable from one leaked key |

  - The concurrency reviewer is still running.
  - **Recovery:** same session, `Workflow({scriptPath: ".../workflows/scripts/sohamalloc-review-round3-wf_107e841a-152.js", resumeFromRunId: "wf_107e841a-152"})`. New session: salvage from `journal.jsonl` and the agent transcripts in `~/.claude/projects/-home-sohambasanwar-sohamalloc/d02fd376-691d-4e43-9867-b889f9bfad80/subagents/workflows/wf_107e841a-152/`.
- **sigstorm campaign 1** is running (1264 of 6480 trials at 12:55). To resume, see the checkpoint above.
- **Done this session:**
  - sigstorm end-of-trial stall fix (85f3b78);
  - the survey `docs/research/06-signal-handler-allocation-survey.md` (55e6900);
  - personal skills `~/.claude/skills/sohamalloc-{verify,benchmark,research,security-audit}` (outside git);
  - 12 unrelated plugins disabled for this project in `.claude/settings.local.json`.
- **Next:**
  1. Verify the round-3 findings.
  2. Fix them with fail-before/pass-after tests, then run the matrix (`/sohamalloc-verify`), commit and push.
  3. Rerun sohamalloc's campaign rows on the fixed code.
  4. Copy the campaign data to `docs/research-log/data/`.

### Checkpoint (2026-10-21 16:10 CDT): review-round-3 fixes in progress (uncommitted)

The session was repeatedly cut off by the model's automated safeguards. **Continue in a fresh session.** The work in progress is in the working tree, not committed, because allocator changes are committed only after `/sohamalloc-verify` passes.

**Applied, not yet built or tested:**
- **F1:** each thread saves its own busy byte across `fork()` (`soham_tls_t.fork_busy` replaces the global `g_fork_saved_busy`).
- **F3:** the static-arena counter moves only when a block fits (CAS loop in `bss_alloc`).
- **Test hooks:** `soham_internal_bss_alloc` and `soham_internal_bss_top`.

**Still to do:**
- **F2:** aligned allocations in handler context should use the shared heap under its trylock, as `alloc_slow` does.
- **F4:** flush the shared heap from `soham_collect` and from the periodic refill tick.
- Register the four new tests in `tests/run_tests.sh`, then run `/sohamalloc-verify` and commit.

**New tests**, uncommitted, in `tests/regress/`:

| Test | On the unfixed code |
|---|---|
| `fork_concurrent_busy.c` | Did **not** reproduce the race on this machine (0 failures in 3×1000 rounds): it is kept as a guard |
| `signal_aligned_enomem.c` | Fails: ~16k ENOMEM per run |
| `signal_shared_release.c` | Fails: 16 MiB stays cached |
| `bss_bounded.c` | Fails: the counter passes the arena |

The before-results are in `docs/research-log/data/regress-before-after/2026-10-21-review3-before.txt`.

**Leave out of git:** `docs/research-log/data/review3/repros/` and `review3-results.json`, which hold the reviewers' raw notes. The ledger gets a neutral summary.

**Campaign:** the sigstorm campaign may need `--resume` (see the earlier checkpoint).
