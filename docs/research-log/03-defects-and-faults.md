# sohamalloc research log, part 3: defects and faults

This ledger records every defect found in sohamalloc: when and how it was found, its root cause, the fix, and the regression test that guards it, with before/after evidence. Times are UTC.

**Scope of this version.** It covers the defects of review rounds 2, 3 and 4 (2026-10-21 to 2026-10-23) in full.

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

## Found by measurement (2026-10-22)

| ID | Severity | Found | How found | Symptom | Root cause | Fix | Regression test |
|---|---|---|---|---|---|---|---|
| C1-1 | major (within a documented limit) | 2026-10-21 21:5x | sigstorm campaign 1, handler-results table (`data/sigstorm-campaign1/report.md`), confirmed on `fd735dd` | Large allocations (300 KB) from a signal handler that interrupted the allocator return NULL. In the campaign this happened in 2–3% of the `large` handler calls that interrupted an allocator call; in the targeted test, about 45% (the main thread holds the page-heap lock most of the time) | A run of 300 KB needs the page-heap lock, which the interrupted thread may hold. The only fallback was the static arena (384 KiB, never reused): the first large fallback used most of it, and every later one failed. This was within limit 1 of IMPLEMENTATION_NOTES §10, so not a guarantee violation, but it limited practicality | Requests above 16 KiB never use the static arena. A request that neither the shared heap nor the arena can serve gets a mapping of its own, a solo whose table is taken by trylock (`handler_solo`). The arena stays for small requests | `tests/regress/signal_large_fallback.c` |

**Before/after** (`data/regress-before-after/2026-10-22-large-fallback-before-after.txt`; prototype measurements in `data/proto-handler-fallback/`):

| Test | `fd735dd` | Fixed |
|---|---|---|
| `signal_large_fallback`, 3 runs | FAIL 3/3: 18,796–19,069 NULL of 41,456–41,740 handler calls | PASS 3/3 release and 3/3 debug: 0 NULL |
| sigstorm `--handler=large`, 5 s, 1/2/4 threads | 95 / 143 / 115 NULL | 0 / 0 / 0 |

## Review round 4 (2026-10-22/23)

**Method.** Commit `ae95875` was reviewed: the round-3 fixes plus C1-1. The review ran as workflow `wf_75415e22-635` with five lens reviewers at reasoning effort "high": signal reentrancy, concurrency, lifecycle/fork/init, API and memory return, portability and checks.

- Each finding got one combined refuter, plus a second independent one for critical or major findings.
- 12 of the 38 agents failed at the usage limit, so some findings were never verified by a refuter. They are marked as such below.
- Raw results stay out of git (`data/review4/`).
- Result: 24 findings, 15 confirmed, 7 of them critical or major.

Every fix below has a regression test that fails on `ae95875` and passes after the fix, except where the table says otherwise.

| ID | Severity | Reviewer finding(s) | Symptom | Root cause | Fix | Regression test |
|---|---|---|---|---|---|---|
| R4-1 | critical | lifecycle-1 | A child forked while another thread was inside lazy init could never finish init: its first allocation waited forever | Init marked itself running at its start and registered the fork handlers at its end. A child forked in between inherited "running" with no thread to finish | The fork handlers are registered at load (ELF constructor), and `fork_prepare` waits for an init in progress. Init is owned by a process id, so a child that still inherits an unfinished init takes it over; its steps are rerunnable. A first version skipped locking when `fork_prepare` saw init not yet started. The test then failed on the fixed code (1 of 40 trials hung): another thread finished init and took a lock just before the fork. Corrected before commit: `fork_prepare` initialises the allocator itself if needed and always takes every lock. Afterwards 0 hangs in 1000 trials, 800 of them under CPU load | `tests/regress/fork_during_init.c` (prefixed suite) |
| R4-2 | critical | portability-1 | A double free whose two frees landed on the two different free lists of a page whose owner had exited was not detected. The page could be released by count while a block was still in use | Release and claim decisions for such pages relied on block counts and on the chain's own length; nothing compared the local list with the remote chain | Before such a page is released or claimed, `free_lists_unique` marks every block of both lists in a bitmap and aborts on a repeat. An owner's collect runs the same check when every block looks free | new cases in `tests/regress/invalid_frees.c` |
| R4-3 | major | signal-1, lifecycle-3, signal-2, concurrency-2 (the last unverified) | Runs and solos freed by a signal handler that interrupted the allocator stayed in use without bound when the thread only took fast paths or only freed; those freed during thread exit leaked | Every such free was put on the thread's deferred list, which only the allocation slow paths, `soham_collect` and the start of thread exit drained | In handler context, runs and solos are freed at once when their lock can be taken by trylock. Anything deferred is drained at the end of the slow paths and of thread exit. Pointers whose solo lookup must wait go into a side table instead of being written into | `tests/regress/signal_deferred_free.c` |
| R4-4 | major | lifecycle-2 | Allocations in atfork handlers registered before sohamalloc's first use failed: small ones used up the static arena, larger ones returned NULL at once | Those handlers run inside the fork window while `fork_prepare` holds every lock, and were served like signal handlers | The forking thread is marked as holding every lock (`t_fork_owner`): its blocking locks are no-ops in the window and it allocates normally. Trylocks are unchanged | `tests/regress/fork_atfork_alloc.c` |
| R4-5 | major | lifecycle-4 | A module that embeds the prefixed archive crashed the next exiting thread after `dlclose()` | The pthread key's destructor stayed registered after the module was unmapped | An ELF destructor deletes the key (`soham_os_thread_key_fini`) | `tests/regress/dlclose_unload.c` (prefixed suite, new plugin test kind) |
| R4-6 | major (claimed; refuters failed at the usage limit) | concurrency-1 | A pending-page handoff could be lost to store-buffering, stranding an orphan page | The pending push and the drain formed a store-buffering pattern with release/acquire only | seq_cst fences on both sides, as in the notify protocol | none deterministic: a memory-ordering window. Fixed by the pattern argument |
| R4-7 | minor | signal-3, concurrency-3, api-5 | The shared-heap trim flag could be cleared just after a notification set it | Check-then-clear without the lock | Clear, fence, then look at the notify stack again (seq_cst push and flag) | — (a race window; `signal_shared_release` still passes) |
| R4-8 | minor | portability-2 | A double free into a spare page was accepted silently | A spare keeps its geometry, and its owner word did not stop frees | A free into a page whose owner is `OWNER_SPARE` aborts | — |
| R4-9 | minor | portability-4, api-4 (unverified) | `realloc` kept a block without the boundary checks `free` applies | The keep-in-place path checked only the kind | Alignment, run base and exact block boundary are checked before keeping | — |
| R4-10 | minor | portability-5 | A handler-context free of a foreign pointer wrote into that memory | Every non-window pointer went onto the deferred list, which links through the block | Solo lookup by trylock first; a foreign pointer is leaked untouched (S1 aborts); an undecided one goes into the side table | — |
| R4-11 | minor | lifecycle-6, concurrency-4 (unverified) | Data races: configuration and geometry read before init completed (`g_cfg.verbose`, `g_geo.secure`, `g_geo.B`), and `g_solo_n` read without its lock | Reads not ordered after the init publication | Reads gated on the init state (acquire); the solo count read under its lock | — |
| R4-12 | minor (unverified) | api-1 | `malloc_trim` released at most 16,384 ranges per call | `ph_purge` capped every mode at 256 rounds | The cap no longer applies to `PURGE_ALL` | — |
| R4-13 | minor | signal-4, api-3 (unverified) | Documentation: §10 after C1-1; 8× instead of 32× for hugeblock-aligned small requests | — | Corrected | — |
| R4-14 | minor | lifecycle-5 | A fork child keeps pages whose responsible resolver was a thread of the parent: PENDING with nobody responsible | The resolver thread does not exist in the child | Documented (IMPLEMENTATION_NOTES §7); not fixed | — |
| R4-15 | minor | portability-3 | A repeated free of a solo in the default tier is ignored | A solo that is not in the table is treated like a foreign pointer | Documented (README, IMPLEMENTATION_NOTES §9) | — |
| R4-16 | minor (unverified) | api-2 | Slice records of released memory are resident but not counted in the statistics | — | Documented (IMPLEMENTATION_NOTES §9) | — |
