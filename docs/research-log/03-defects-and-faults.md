# sohamalloc research log, part 3: defects and faults

This ledger records every defect found in sohamalloc: when and how it was found, its root cause, the fix, and the regression test that guards it, with before/after evidence. Times are UTC.

**Scope of this version.** It covers the defects of review round 2 (2026-10-21) in full.

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
