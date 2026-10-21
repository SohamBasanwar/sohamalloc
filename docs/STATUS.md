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

## In progress (2026-10-21, laptop session) -- READ THIS FIRST WHEN RESUMING

Review round 2 (workflow run wf_d0487d5f-fa6) was interrupted once and resumed;
it may not have finished. Three defects are CONFIRMED with reproducers
(data: docs/research-log/data/review2/, before-results:
docs/research-log/data/regress-before-after/2026-10-21-review2-before.txt):

1. Handler-context deadlock (critical): bin_refill -> bin_trim_avail ->
   page_release -> ph_free takes the blocking g_ph.lock in trymode.
   Fix: in bin_refill, call bin_trim_avail only when !t_tls.trymode; audit
   every soham_lock reachable in trymode; SOHAM_DEBUG: abort in soham_lock
   when t_tls.trymode is set.  Test: tests/regress/signal_trim_deadlock.c
   (old code: STALL 3/3).
2. Notify-stack stranding after fork (major): heap_attach stores NULL into
   h->notify unconditionally.  Fix: CAS NOTIFY_CLOSED -> NULL only.
   Test: tests/regress/fork_notify_reuse.c (old code: 540/6000 reused, FAIL).
3. Orphan-page remote double free A,B,A (critical): resolve()'s cnt==cap
   branch releases the page without walking the chain.  Fix: after the CAS
   to 0 (and pool_unlink), merge_chain(r, xf_ptr(x), cnt - r->x.folded)
   before page_release.  Test: new cases in tests/regress/invalid_frees.c
   (old code: "orphan page A,B,A WRONG (exited)").
4. TSan: soham_get_stats reads page-heap counters without g_ph.lock
   (api.c:788) -- read them under the lock.

Reviewer's proposed diff for 1 and 2: docs/research-log/data/review2/reviewer-proposed-fixes.diff.
The three new tests are NOT yet registered in tests/run_tests.sh (they fail
until the fixes land).  Next steps follow the directive: apply fixes,
register the tests, full matrix, sigstorm for sohamalloc (add a "churn"
handler kind that exercises medium-page trim paths), then the campaigns.
