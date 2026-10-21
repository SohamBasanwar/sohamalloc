# sohamalloc research log, part 3: defects and faults

This ledger records every defect found in sohamalloc: when and how it was found, its root cause, the fix, and the regression test that guards it, with before/after evidence. Times are UTC.

**Scope of this version.** It covers the defects of review rounds 2 and 3 (2026-10-21) in full.

- **Earlier defects** (2026-10-19/20) are listed with their discovery and fix in `01-timeline.md` (§ defect table) and in the commit messages of `71f852c` and `ee1f70d`. They are still to be transcribed into the format used here.
- **Design-stage faults** (the 101-fault critique of v1 and the red-team rounds of the plan) are in `docs/research/00-v1-critique.md` and `docs/SOHAMALLOC_PLAN.md`.

**Method note.** Review round 2 was run as an AI-assisted adversarial code review:

- two reviewer agents, one for concurrency and signal reentrancy and one for correctness, API, security and kernel interaction;
- each followed by a verifier agent that tries to refute every finding;
- interrupted once and resumed;
- stopped early at the author's request, to save the usage budget.

**Every defect below was confirmed by a reproducer program before it was fixed.**

## Review round 2 (2026-10-21)

| ID | Severity | Found | How found | Symptom | Root cause | Fix | Regression test |
|---|---|---|---|---|---|---|---|
| R2-1 | critical | 2026-10-21 05:2x | Concurrency review; reproducer stalled 3/3; stack confirmed with gdb | A signal handler that allocates while its thread holds the page-heap lock deadlocks: the thread makes no progress | Handler allocations run in *trymode* on the shared heap, where every lock must be a trylock. `bin_refill()` calls `bin_trim_avail()`, then `page_empty_release()`, `page_release()`, `ph_free()`, and `ph_free()` takes `g_ph.lock` with the **blocking** `soham_lock()`. Introduced in `ee1f70d`, which added the trim to the refill path | Refills in trymode never trim. SOHAM_DEBUG builds now abort when a blocking lock is taken in trymode (`soham_debug_check_blocking`) | `tests/regress/signal_trim_deadlock.c` |
| R2-2 | major | 2026-10-21 05:3x | Concurrency review (reproducer `fork_notify.c`; scale variant measured) | After `fork()`, a thread that takes over a pooled heap does not reuse pages that other threads freed into it; memory grows instead | `heap_attach()` reset `h->notify` to NULL unconditionally, so a heap recycled as is (the fork child pools other threads' heaps) loses its pending notifications. The full pages are never moved back to the avail list | Reopen only a closed stack: CAS `NOTIFY_CLOSED` to NULL | `tests/regress/fork_notify_reuse.c` |
| R2-3 | critical (security) | 2026-10-21 05:4x | Correctness review (reproducer `orphan_aba.c`); verifier reproduced it and tested the fix | Remote double free A, B, A on a page of an exited thread is not detected; the page is released while block L is still live, and a later `malloc` returns L's address | The free count of an orphaned or pooled page reaches its capacity, and `resolve()` releases the page by count alone, **without walking the remote chain**. `pool_claim()` does walk it (`merge_chain`, exact length). The A, B, A cycle only shows up in that walk | `resolve()` calls `merge_chain()` (exact-length validation, abort on mismatch) before releasing | New cases in `tests/regress/invalid_frees.c`: `orphan page A,B,A` must abort, `orphan page full free` must pass |
| R2-4 | minor (UB) | 2026-10-21 05:3x | Concurrency review (TSan stress: 21 reports) | ThreadSanitizer data races between `soham_get_stats()` and `hb_take()` / `hb_give()` | `soham_get_stats()` read `g_ph.s_used` / `g_ph.s_dirty` without `g_ph.lock` | Read them under the lock | `tests/regress/stats_concurrent.c` (prefixed suite, run under TSan) |
| R2-5 | minor (UB) | 2026-10-21 16:4x | The new R2-4 test under TSan; present before R2-4's fix as well | TSan data race: `soham_free()` reads `g_geo.wshift` (and the new `gmap` pointer) while another thread initialises the allocator | The free fast path read `wshift` before its acquire load of `gmask`. When the mask is still 0 the value is irrelevant, but the access is still a C11 data race, and so is reading a just-published `gmap` with a relaxed load | Load the mask first, then `wshift` as a relaxed atomic (stored atomically by init). `gmap` is stored with release and loaded with acquire (free on x86, one `ldar` on arm64) | `tests/regress/stats_concurrent.c` |

### Before/after evidence

Raw records:

- before the fixes: `data/regress-before-after/2026-10-21-review2-before.txt`;
- review data: `data/review2/`.

| Test | Unfixed code (`ee1f70d` sources) | Fixed code |
|---|---|---|
| `invalid_frees`: orphan page A,B,A | WRONG (exited: double free accepted) | ok (aborted: "corrupt remote free chain") |
| `invalid_frees`: orphan page full free | ok | ok |
| `signal_trim_deadlock`, 3 runs | STALL after 513–532 handler calls, 3/3 | PASS 3/3 (release and debug builds) |
| `fork_notify_reuse`, 3 runs | 540 of 6000 blocks reused, FAIL 3/3 | 5175 of 6000 reused, PASS |
| `stats_concurrent` under TSan | 9 race reports (`api.c:788`, `hb_take`, `hb_give`, `soham_free`) | 0 reports, 5/5 runs |

### Detection power of randomized signal storms (negative result)

**R2-1 was not found by `tools/sigstorm`.** On the unfixed code, with 2 workload threads, 8 s per run and about 385,000 signals per run, all of these runs survived:

| Workload | Handler | Runs | Outcome |
|---|---|---|---|
| default mix | `churn` | 3 | survived |
| `--workload=page` | `churn` | 3 | survived |
| `--workload=page` | `malloc` | 3 | survived |

The deadlock needs three things at once:

1. the handler fills and empties shared-heap pages *while interrupting the allocator*;
2. a later refill in that state trims a page;
3. the interrupted thread holds the page-heap lock at that moment.

The regression test forces all three, because its main thread sits inside the allocator almost continuously. Randomized storms alone therefore do not certify signal safety. The method used here combines them with targeted adversarial tests and review, and the claims made from sigstorm results are stated with their trial counts.

## Review round 3 (2026-10-21)

**Method.** The same AI-assisted adversarial review as round 2, on commit `c1a9791` (the round-2 fixes):

- two reviewer agents, one for correctness, API, security and kernel interaction and one for concurrency and signal reentrancy, run as one workflow from 17:12 UTC;
- each reviewer built and ran its own reproducer programs;
- the verifier stage did not complete, because the session hit its usage limit and was then cut off repeatedly.

The findings were saved as the reviewers returned them. The raw notes are kept out of git; this ledger gives a neutral summary. The two reviewers found three of the defects independently. **Every critical and major finding was confirmed again in the next session by a regression test that fails on `c1a9791` and passes on the fixed code** (evidence below). That test is the confirmation of record, not the reviewers' reproducers.

| ID | Severity | Reviewer IDs | Symptom | Root cause | Fix | Regression test |
|---|---|---|---|---|---|---|
| R3-1 | critical | R3-CORRECTNESS-1, R3-CONCURRENCY-1 | Two threads call `fork()` at the same time. A thread that had never allocated then crashes (SIGSEGV) at its next `malloc`, in the parent or in the child | `fork_prepare` saved the forking thread's busy byte in **one process-wide variable**, and `fork_parent`/`fork_child` restored it from there. glibc runs the atfork handlers of concurrently forking threads at the same time. Measured here on glibc 2.43 with a minimal handler and two threads started together, the second thread's prepare began before the first thread's parent handler had run in 65–72% of the rounds (three runs of 1000 rounds, `data/review3/atfork-overlap.txt`). The heapless thread (busy = `BUSY_NOHEAP`) was handed the other thread's 0, and the malloc fast path then dereferenced its NULL heap | Each thread saves its own busy byte: `soham_tls_t.fork_busy` replaces the global. The child is a copy of the forking thread, so it inherits the right value | `tests/regress/fork_concurrent_busy.c` |
| R3-2 | major | R3-CONCURRENCY-2 | `posix_memalign`, `aligned_alloc`, `memalign` and aligned `operator new` called from a signal handler that interrupted the allocator fail with ENOMEM once about 384 KiB have been allocated that way, forever, even with one allocating thread | `alloc_aligned()` sent every handler-context call to the static arena, whose frees are counted no-ops. It never tried the shared heap under its trylock, which `alloc_slow()` has done for malloc since the earlier fix guarded by `regress/signal_reentrant_enomem.c`. The aligned family was the same defect class, left unfixed | Handler-context allocation, aligned or not, goes through one function, `handler_alloc()`: the shared heap under trylock in trymode, where aligned requests use the aligned size class, an aligned run or a solo. It falls back to the static arena (aligned) only when the trylock fails | `tests/regress/signal_aligned_enomem.c` |
| R3-3 | major | R3-CORRECTNESS-3, R3-CONCURRENCY-3 | On 32-bit targets, after about 4 GiB of failed handler-context requests, the static arena hands out pointers outside itself, and writes their size header there (crash or memory corruption) | `bss_alloc()` advanced its bump counter with an unconditional `fetch_add`, **also for requests that then failed** because the arena was full. On ILP32 the `size_t` counter wraps modulo 2^32; the bounds check `off + need > size` then passed again. On 64-bit only the counter's unbounded growth is visible | A bounded bump: a CAS loop moves the counter only when the block fits, and the check is written as `off > size - need`, which cannot overflow | `tests/regress/bss_bounded.c` (through the new test hooks `soham_internal_bss_alloc` and `soham_internal_bss_top`) |
| R3-4 | major | R3-CORRECTNESS-2 | Memory that signal handlers allocated stays resident for the life of the process after it is freed. `malloc_trim(0)` and `soham_collect(1)` do not release it (64 MiB stayed resident in the reviewer's program; glibc dropped to 2 MiB) | Handler-context allocations come from the shared heap, which **no thread owns**. Its pages are therefore never flushed at thread exit; its refills run in trymode, where trimming is skipped (R2-1); and `soham_collect()` flushed only the calling thread's heap. Freed by other threads, its pages stay owned by it with every block on their remote chains | Any thread trims the shared heap on every 64th refill of its own heap, at most once per purge interval across all threads (`shared_trim`), when there may be work. The flag `g_shared_trim` is set by allocations from the shared heap and by notifications to it. The trim takes the shared lock by trylock, moves notified full pages to the avail list, and unlinks the avail pages whose blocks are all free. It releases them after the unlock, each after an exact-length walk of its remote chain. `soham_collect(1)` (and so `malloc_trim`) flushes the shared heap completely (`shared_flush`) | `tests/regress/signal_shared_release.c` |
| R3-5 | minor | R3-CORRECTNESS-4 | Under a small `RLIMIT_AS` (for example 128 MiB), about 40% less memory is usable than with glibc (60 against 100 MiB). Allocation still fails cleanly with ENOMEM | Windows of at least 64 MiB, reserved `PROT_NONE`, count fully against `RLIMIT_AS`, and so does the granule map, sized for the whole 48-bit address space (4 MiB at 64 MiB windows) | Not fixed in this round: a resource-efficiency limitation under small address-space limits, recorded in IMPLEMENTATION_NOTES §9 | — |
| R3-6 | minor (hardening) | R3-CORRECTNESS-5 | The per-page free-list keys of one heap come from one invertible generator stream (xorshift64*, seeded through an invertible mix). Anyone who learns one key can compute the heap's other keys | A design weakness of the link-encoding hardening, found by code analysis only (no reproducer against the allocator). The documentation presents link encoding as raising the bar, not as a guarantee | Not fixed in this round: recorded as a hardening limitation (IMPLEMENTATION_NOTES §9). A keyed, non-invertible derivation of each page key is the planned remedy | — |

### Before/after evidence (round 3)

Raw records:

- `data/regress-before-after/2026-10-21-review3-before-after.txt`;
- an earlier, partly wrong before-run: `data/regress-before-after/2026-10-21-review3-before.txt` (see the note below).

The tests were built as `tests/run_tests.sh` builds them. The unfixed code is `c1a9791` plus the two arena test hooks. The sigstorm campaign and the test matrix were running at the same time.

| Test | Unfixed code (`c1a9791`) | Fixed code |
|---|---|---|
| `fork_concurrent_busy`, 3 runs | FAIL 3/3: the parent crashed (SIGSEGV) within the first rounds | PASS 3/3 (1000 rounds of two concurrent forks each) |
| `signal_aligned_enomem`, 3 runs | FAIL 3/3: 13,108–15,622 of 40,894–43,986 handler allocations returned NULL | PASS 3/3: 0 NULL of 43,663–44,632, all aligned |
| `signal_shared_release`, 3 runs | FAIL 3/3: 16,703 KiB cached after further activity, 16,384 KiB after `soham_collect(1)` | PASS 3/3: 383 KiB after activity, 0 after `soham_collect(1)` |
| `bss_bounded`, x86-64 | FAIL: counter 395,760 after filling (beyond the 393,216-byte arena), 6,001,995,760 after 20,000 failed requests | PASS: counter 391,680 before and after |
| `bss_bounded`, armhf (32-bit, qemu-arm) | FAIL: crashed in `bss_alloc` once the counter wrapped | PASS |

**Note on the first before-run (a negative result about the test, not the code).** The first version of `fork_concurrent_busy` did not reproduce R3-1 on the unfixed code: 0 failures in 3 × 1000 rounds (`2026-10-21-review3-before.txt`). There were two reasons:

1. **The test was compiled without `-fno-builtin-malloc`.** GCC then deleted the test's own `malloc`/`free` pairs, which were exactly the calls that crash. An instrumented copy of the unfixed library showed the busy byte being swapped in 6 of 300 rounds while the test passed.
2. **The two threads met at a sleeping barrier** (`pthread_barrier_wait`). On an idle laptop the second thread woke up after the first had finished its `fork()`.

The reviewers' reproducer crashed 3/3 under load and 0/3 on the idle machine. The final test starts both forks by spinning, with the heapless thread first. Built as the runner builds it, it fails 3/3 (5/5 in an earlier check).

**Coverage gap.** `tools/sigstorm` has no handler kind that makes aligned allocations, so no sigstorm campaign could have found R3-2. An `aligned` handler kind is to be added before the next campaign.
