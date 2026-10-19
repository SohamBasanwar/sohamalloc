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
