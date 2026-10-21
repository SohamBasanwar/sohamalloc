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
