# sohamalloc: Definitive Architecture and Build Plan (v3, red-team revised)

*Date: 2026-10-18. This document replaces docs/RESEARCH_AND_DESIGN.md (v1) entirely. It is v2 of the plan with every critical and major finding from two red-team reviews resolved (see §7.2).*

**How claims are marked**

- `[tag]` points to the reference list in §10. Every tag resolves to a URL taken from the research reports, the critiques, the judges' notes or the red-team reports.
- **[est]** is my own estimate of instruction or cycle counts. Nothing has been measured, because no sohamalloc code exists yet.
- **[derived]** is my own arithmetic on published facts or on this plan's own constants.
- **PARAM** is an initial design constant. Each one has a validating experiment (E-n, §9).
- **UNVERIFIED** means I could not confirm the claim from a primary source in the material.
- **INFERENCE** is reasoning from a mechanism that no source states outright.

---

## 1. Executive summary

sohamalloc is a drop-in, page-based dynamic memory allocator for Linux (x86-64 and arm64, glibc and musl). It covers `malloc`, `free`, `calloc`, `realloc`, `aligned_alloc`, `posix_memalign`, `memalign` and the full C++ `new`/`delete` surface.

Its thesis rests on fleet evidence that *where* blocks land matters more than how many instructions `malloc` takes:

- malloc accounts for 4.3% of fleet cycles, yet a hugepage- and NUCA-aware heap moved fleet throughput by +1.4% [asplos24].
- Temeraire gained +7.7% requests per second while cutting RAM by 2.4% [osdi21].

Those gains came from hugepage backing. Under the most common kernel setting (`enabled=madvise`, `defrag=madvise`) sohamalloc v1.0 deliberately does **not** request hugepages, because of documented stall risks (§5.8.3). So the thesis applies in full on `THP=always` hosts, and only partly elsewhere until experiment E9 settles whether a dense-area opt-in is safe. Every throughput and memory gate is measured on both THP configurations (§3).

sohamalloc aims at:

- application throughput comparable to mimalloc v3 and per-CPU TCMalloc;
- lower and more *honest* RSS than any of them;
- kernel citizenship by construction: few VMAs, no runtime VMA flag changes, batched release, and `mmap_lock` writes in steady state only for listed, counted events;
- a near-free default security tier;
- zero required tuning.

**Lineage.** The two judges disagreed (Judge 1 picked Design A, Judge 2 picked Design B) but converged on one synthesis:

- **Data path from A:** page-owning slots, a packed single-commit free word, a remote buffer flushed with one CAS per destination page, arithmetic descriptor lookup, and a countdown that drives the periodic jobs.
- **Policy layer from B:** falsifiable RSS and VMA targets, the demand-window retention formula, cgroup pressure levels, an inline release trigger, a lazily started futex-idle background thread, and the THP matrix.
- **Lifecycle and validation discipline from C:** thread-heap mode ships first, with a slot pool, a Hoard-style bound and an asymmetric-Dekker lease, plus the foreign-pointer policy and the drop-in checklist.

The red teams then found nine critical defects in v2's data path. Among them: link encode and decode that were not inverses; an ownership check outside the critical section; a tag collision on unowned pages; signal reentrancy; and a futex on a byte. They also found about forty major problems. All are fixed in this version.

### 1.1 Headline design decisions

1. **One object flow: pages are the only unit of transfer.** There are no object transfer caches.
2. **v1.0 front end: TLS-exclusive slots.**
   - Exited threads return their slots to a warm-first *slot pool*.
   - A Hoard-style bound with hysteresis applies to each slot [hoard].
   - Idle slots are reclaimed by a **non-blocking lease**: an asymmetric Dekker handshake over `membarrier(PRIVATE_EXPEDITED)` [membarrier], using release/acquire on `busy` and `lease`.
   - The owner never waits: it either cancels a pending lease or abandons the slot and attaches another.
3. **rseq `mm_cid` slot mode is a P4 option.** It ships only if a per-kernel benchmark gate passes. Prior art: TCMalloc's `RseqVcpuMode::kMM` [tc-percpu] and the kernel's mm_cid feature [k-cid].
4. **Packed `count:16 | head:48` free word**, with links encoded as `bswap(next ^ key ^ (addr>>12))` and decoded as `bswap(E) ^ key ^ (addr>>12)`. Round-trip property tests are a P0 exit gate.
5. **Three 64 B lines per slice record**: owner-written, read-mostly and remote. The records sit in a **separate `MADV_NOHUGEPAGE` metadata area**, below the data and separated from it by a `PROT_NONE` gap.
6. **Remote frees ≤ 1 KiB are buffered per slot** (64 entries, PARAM).
   - The buffer is replaced by a single-commit swap with the *thread's* spare buffer, then sorted and pushed with one CAS per destination page, as in BatchIt [batchit].
   - Experiment E6 decides whether batching is on by default.
7. **One heap window per process.**
   - The window is a 64 GiB-aligned, `PROT_NONE`, `MAP_NORESERVE` VA window (≤ 64 GiB, scaled down under RLIMIT_AS).
   - It has a committed prefix that grows inside it, which avoids the clash with top-down mmap placement.
   - Pointer→record lookup is one granule-map byte load in parallel with one arithmetic record load.
8. **64 size classes.**
   - Small pages (64 KiB) for ≤ 8 KiB.
   - Medium pages that are any slice multiple up to 1 MiB, generated for ≤ 3.125% tail waste where possible.
   - 192 and 256 KiB served as exact slice runs.
   - Solos for ≥ 32 MiB.
9. **Uniform commit model** across overcommit modes 0/1/2 and `mlockall`: the prefix grows by `mprotect` inside the `PROT_NONE` window. Solos are mapped *without* `MAP_NORESERVE`, so glibc's failure behaviour for absurd requests is preserved. Mode-2 decommit of the top extent comes in v1.1.
10. **Release policy.**
    - `MADV_DONTNEED`, sent through `process_madvise(PIDFD_SELF)` when the kernel supports it (6.15+), else a coalesced `madvise` loop [pmadvise][k-pidfd].
    - Pressure-driven cgroup levels with hysteresis.
    - A byte-budgeted inline trigger.
    - A lazily created background thread that exits when the process becomes single-threaded again [unshare].
11. **THP policy.**
    - The bootstrap hugeblock and all metadata are `MADV_NOHUGEPAGE`.
    - Per-size (mTHP) controls are read.
    - No `MADV_HUGEPAGE` under `defrag=madvise` [go-61718].
    - `MADV_COLLAPSE` is opt-in only, because it takes `mmap_write_lock` [k-khp].
12. **Security tiers on one page format, plus signal-safe reentrancy.**
    - S0 (default): encoded links, range checks on head and next, and a GWP-ASan-style pool.
    - S1: boundary check on every free, shuffle, zero-on-free ≤ 1 KiB, quarantine and call-site buckets.
    - S2 (MTE): P4+.
    - A malloc from a signal handler that interrupted the allocator gets an emergency bump arena. A free there goes to a thread-local deferred-free list, so no handler path ever takes a lock.

### 1.2 Quantified targets (detail in §3)

| Target | Value | Baseline |
|---|---|---|
| Application throughput geomean (both THP configs) | gate ≥ 0.97×, goal ≥ 1.00× | mimalloc v3.4.5 |
| Application throughput geomean | ≥ 1.05× | glibc 2.44 |
| `/bin/true`-class RssAnon | ≤ glibc 2.44 + 96 KiB (goal + 48 KiB) | glibc 2.44 |
| Thread-churn RSS-time integral | ≤ 0.7× | mimalloc v3.4.5 |
| RSS 15 s after a burst (THP=madvise) | ≤ live + FE + max(4 MiB, 12.5%·live) | — |
| Steady-state VMAs | ≤ 6 + 4 per extra window + 1 per solo | — |
| `mmap_lock` write acquisitions in steady state | 0/s apart from listed, counted exclusions | — |
| S0 cost | ≤ 2% vs tier −1, also reported against glibc and mimalloc | "tier −1" build |
| S1 cost | ≤ 10% | "tier −1" build |

---

## 2. Self-critique of v1, and how the judges' fatal flaws are fixed

### 2.1 Faults in v1

| # | Where in v1 | Problem | Fix adopted |
|---|---|---|---|
| 1 | §5 (whole) | No pointer→metadata lookup. It is the hottest lookup on the free path, and its foreign, interior and tagged-pointer behaviour was undefined | §5.2: granule map plus window arithmetic, a foreign-pointer policy, TBI stripping |
| 2 | §5.1 | "4 classes per doubling = ≤12.5% waste" is wrong. The real worst case is ~20% of the block, 25% of the request [je-man] | §5.3: explicit 64-class table with waste per band |
| 3 | §5.1 / §5.4 | Page size per class missing. A 40 KiB class in a 64 KiB page wastes 37.5% | Generated slice-multiple page sizes (§5.3) |
| 4 | §5.1 | mm_cid sold as novel. It is TCMalloc kMM [tc-percpu] and a purpose-built kernel feature [k-cid] | Credited; TLS mode ships first; cid mode gated (§5.5) |
| 5 | §5.1 | mm_cid semantics wrong. The bound is `min(1.25·min(cpus_allowed, users), possible)` [k-sched], ownership modes changed in 6.19 [k-cidrw], and cross-slot work needs process-wide fences [tc-percpu] | Bounds-checked slot table, fenced reclaim, kernel-matrix gate |
| 6 | §5.1 + §5.2 | Page-local sharded lists combined with TCMalloc transfer caches and "elastic capacity" do not compose | Single page-based flow (§5.4) |
| 7 | §5.1, §5.4, §5.5 | Hard-coded 4 KiB / 2 MiB geometry. arm64 PMD is 32 or 512 MiB on 16K/64K kernels [k-arm64hp]; such hard-coding broke other allocators [asahi] | All geometry read at runtime (§5.1) |
| 8 | §5.2 | Adaptive switch to message passing: racy, two channels, idle slots never collect | One channel (page `xfree`) plus sender batching |
| 9 | §5.3 | Return-address prediction on the fast path. LLAMA's cached lookup costs 22.5 ns against an 8.3 ns TCMalloc fast path [llama] | No small-object prediction; large-object predictor is counterfactual-only (P5) |
| 10 | §5.3 | Lifetime classes multiply partial pages per slot | Dropped; zero-cost density split instead [asplos24] |
| 11 | §5.3 | Prior art not credited | Credited [tc-lifetime][llama] |
| 12 | §5.4 | "Fullest first" hurts refill yield; Temeraire's order misdescribed [tc-hpf] | Fullest page with ≥ 1/8 free; Temeraire's actual order |
| 13 | §5.4 | Large objects inside the region, `mremap` and "no mmap" contradict each other | `mremap` only for solos that own their VMA [lwn-mremap] |
| 14 | §5.4 | "Out-of-line metadata" contradicts inline encoded links; snmalloc 64-bit figure misapplied [sn-paper] | Threat model stated: links inline and encoded, all else in a separate metadata area |
| 15 | §5.5 | 64 GiB read-write reservation breaks under overcommit mode 2 and RLIMIT_AS [k-overcommit] | `PROT_NONE` window (not accountable), sized by RLIMIT_AS, committed by prefix growth (§5.8.1) |
| 16 | §5.5 | "<10 VMAs" contradicted by `mprotect` commit, `mbind`, sub-range madvise, `mremap` and `PROT_NONE` guards [k-madvise] | Per-feature VMA budget enforced in CI (K1) |
| 17 | §5.5 | THP policy fights the kernel: defrag stalls, khugepaged `max_ptes_none`, the underused shrinker, `MADV_COLLAPSE` ignoring sysfs [k-thp] | THP matrix (§5.8.3) |
| 18 | §5.5 | Guard pages conflict with THP and mlock [madvise]; `PROT_NONE` fallback splits VMAs | Guards only in the GWP pool by default; S1 guards only when not mlocked |
| 19 | §5.5 | `MADV_FREE` tied to a profile; no batching; `process_madvise` missed [pmadvise] | DONTNEED by default, vectors, every return code checked |
| 20 | §5.5 | Decay without a timer never runs in idle programs [je-1398] | Inline trigger, free-driven ticks, lazy futex-idle thread |
| 21 | §5.5 | PSI from system-wide `/proc/pressure`; 2 s unprivileged window ignored [k-psi] | The process's own cgroup files (§5.9); PSI opt-in in v1.x |
| 22 | §5.5 | Fork ignored in-flight lock-free operations and leaked other threads' caches | PreventFork drain before locks [sn-fork], slot re-homing in the child |
| 23 | §5.6 | Safe-linking described as using a secret; it uses `pos>>12` [glibc-malloc] | Per-page key plus address mixing plus bswap |
| 24 | §5.6 | Tier-0 bitmap duplicated the free list | No bitmap; one page format |
| 25 | §5.6 | 1-in-10⁵ GWP sampling, uncapped zero-on-free (+8.38% wall on a kernel build [k-initfree]), MTE underspecified, no quarantine | §5.12 |
| 26 | §5.7 | Throwing `operator new` cannot be written in C | Separate C++ translation unit [cxx-new] |
| 27 | §5.7 | `getenv` exposed to setuid programs | `AT_SECURE` rules [getenv] |
| 28 | §5.7 | Report produced in a signal handler, which is not async-signal-safe [sigsafety] | Signal only sets a flag; report never allocates |
| 29 | §5.7 | API semantics missing: overflow, `PTRDIFF_MAX`, `realloc(p,0)`, errno, alignment | Compatibility table (§5.13) |
| 30 | §5.7 | Interposition rules missing: IE TLS, raw syscalls, `reallocarray`, musl `aligned_alloc` [glibc-replace][musl-glue] | Drop-in checklist (§5.13, §8) |
| 31 | §5.7 | Size feedback assumed to speed up free | Size is a validation hint only [tc-msd] |
| 32 | §5.7 | `backtrace()` inside malloc dlopens libgcc [unwind-link] | Bounds-checked frame-pointer walker (P4) |
| 33 | §5.7 | Fabricated RSS sum in the explain report | Two views; difference labelled [k-proc] |
| 34 | §3 L3 | "Hugepage-oblivious peers" is false [tc-sysalloc][je-hpa][mi-readme] | Benchmark against Temeraire, jemalloc HPA and mimalloc v3 |
| 35 | §3 L5 | "NUCA +3.8%" overstated; the fleet figure is +0.32% [asplos24] | Corrected |
| 36 | §3 L8 | TLB IPIs presented as universal; this is x86-specific [k-arm64tlb][lwn-invlpgb] | Stated per architecture |
| 37 | §2, §3 L11 | glibc, jemalloc, mimalloc facts stale (glibc 2.44 arena cap [glibc-arena], jemalloc 5.4.0 [je-changelog], mimalloc v3.4.5 [mi-readme]) | Baselines pinned in §8.5 |
| 38 | §0 | Robson stated with a different parameter; Θ(log log M) result ignored [bender26] | §4.2, §9 |
| 39 | §6 | Several "differentiators" already exist | §4.3 narrows the claims |
| 40 | §7, §8 | Unfalsifiable goals; Redis ignores `LD_PRELOAD`; gperftools mistaken for TCMalloc; roadmap order wrong; weak Phase-0 exit | §3, §8 |
| 41 | §8 Phase 5 | Meshing conflicts with DONTNEED, fork and THP [mesh] | Removed |
| 42 | Absent | No licence, testing chapter, startup budget, runtime-language support, real-time non-goal, deterministic mode, heterogeneous cores, tiering or swap | §3, §5, §8 |
| 43 | Refs [15], [17], [19] | Login-walled source, mismatched citations | Removed or replaced |

### 2.2 The judges' fatal flaws and their resolution

| Source | Fatal flaw | Resolution in this plan |
|---|---|---|
| A | Idle-slot reclaim race: the free push did not check the lease | §5.5.6: every CS re-checks lease with acquire semantics. Ownership is compared *inside* the CS. The reclaimer takes the slot by CAS (`REQ→TAKEN`) only after it observes `busy == 0` through an acquire load after `membarrier`. Modelled before P2 code |
| A | mm_cid unstable before 6.19 and in per-CPU mode | cid mode is a P4 option, gated per kernel (6.6, 6.12, 6.18, 6.19, 7.1); E14 measures how often a same-thread free goes remote. The instability claim is the judge's analysis, UNVERIFIED |
| A | 64 MiB arena justified by a wrong overcommit argument | Arenas replaced by a `PROT_NONE` window. NORESERVE/`PROT_NONE` reservations are not accountable [k-mmapc][k-vmac][k-overcommit] |
| A | Fat fast path on rseq | TLS mode is the default. The P1 prototype is measured against mimalloc v3.4.x and per-CPU TCMalloc on Zen 4 and Neoverse V2 |
| A | gmap pointed at a 4 KiB zero page but was indexed up to 2^21 | gmap lives in the window-1 metadata area. It is indexed only below a runtime bound `gmap_n`, which is 0 before init |
| A, B, C | Map covered only 47 bits; arm64 48-bit kernels return addresses above 2^47 [k-arm64mem] | gmap sized for 2^48: 2^22 entries, 4 MiB of VA, committed lazily |
| A, B, C | Mode-2 decommit via `mprotect(PROT_NONE)` keeps the commit charge once pages have an anon_vma [k-mprotect] | `mmap(MAP_FIXED|MAP_NORESERVE, PROT_NONE)` over the top extent (v1.1) |
| A | Cached cgroup fds and pidfd | No fds are cached. No pidfd is ever opened: `PIDFD_SELF` or plain `madvise` (§5.8.2) |
| A | RSEQ_SIG probed by forcing an abort; a mismatch is SIGSEGV | Per-architecture constant from glibc's `<sys/rseq.h>`. No probe |
| A | `mm_cid & 8191` mask aliases two live CIDs | Bounds check; fall back to TLS mode |
| A, B | `MADV_COLLAPSE` assumed lock-free; `collapse_huge_page` takes `mmap_write_lock` [k-khp] | Off by default; opt-in from the background thread only, rate-limited, counted in K2 |
| B | Remote-batch eviction outside rseq | Rejected. Commit-swap with a per-thread spare buffer (§5.5.5) |
| B | Ownership transitions unspecified | Resumable single-commit detach and claim plus ORPHAN/QUEUED/PENDING in `xfree` (§5.4) |
| B | `xfree` shares the 64 B line with `free_head` | Three separate lines per record |
| B | 3 dependent loads on free | Arithmetic record address; the gmap byte load runs in parallel |
| B | GWP sampled at refill (biased); no heap profiler | Allocation-byte countdown drives GWP and (P4) the profiler; frees drive a separate tick |
| B | "0 mmap_lock writes" contradicted | Target restated with explicit, counted exclusions, including khugepaged (§3 K2) |
| B | FULL / pending lost wakeups | PENDING responsibility bit; the responsible thread re-checks under the pool lock, and its clearing CAS fails if `xfree` changed (§5.4) |
| C | 45-instruction free, address-ordered reuse, 2 atomics per remote free | Encoded LIFO lists; one CAS per destination page; no shared counter |
| C | No rseq means heaps scale with threads | Slot ceiling of 2× effective CPUs, shared slots above it; cid mode in P4 |
| C | Double-free detection overclaimed | Best-effort, stated (§5.12) |
| C | Fork child kept stale `busy` flags | The child resets `busy`, `lease` and PreventFork counters on every slot |
| C | Bitmap claim order, calloc `fresh_groups` | N/A: no bitmap pages. calloc skips zeroing only for bump-cursor blocks of zero-known pages and zero-known runs (§5.7.4) |
| C | Huge-object guard in unmapped slack | S1 guard is a retained, mapped tail page, installed only when not mlocked |
| C | Mode-0 "refuses requests larger than RAM" claim | Deleted. Solos now use glibc's mapping flags so the mode-0 heuristic applies as it does for glibc |
| C | Novelty overclaims | hardened_malloc, kalloc_type, TCMalloc and liburcu credited [hm-readme][kalloc-type][liburcu] |

---

## 3. Goals, non-goals and measurable success criteria

**Goals:** application throughput, low and honest RSS, kernel citizenship, a secure default, drop-in with no tuning, explainability.

**Non-goals for v1.0:**

- Hard real-time (TLSF or o1heap fit better).
- 32-bit and wasm.
- Moving or meshing objects.
- ML prediction on the fast path.
- Windows and macOS: designed for (§5.15), not built.
- NUMA-specific placement and the rseq cid mode: designed, P4.
- Making `RTLD_DEEPBIND` mixed-allocator programs safe, which is unsolved upstream [glibc-30186].

**Success criteria.** All are measured with the harness in §8.5. "Gate" blocks the release; "goal" is the aspiration. Every value is a design target, not a measured result.

Unless a row says otherwise, every T and M gate must pass on **both** THP configurations: (a) `enabled=always, defrag=madvise` and (b) `enabled=madvise, defrag=madvise`.

| ID | Metric (benchmark) | Gate | Goal | Baseline |
|---|---|---|---|---|
| T1 | Throughput geomean, ≥ 10 runs, 95% bootstrap CI. Suite: Redis (`MALLOC=libc` build, memtier), RocksDB `db_bench` readwhilewriting (built with jemalloc disabled, verified with `ldd`/`nm`), clang self-build, CPython pyperformance with `PYTHONMALLOC=malloc` [py-mem], Envoy built with `--define tcmalloc=disabled` [envoy-bazel], fleetbench with `custom_malloc` set to each allocator [fleetbench]. A run is valid only if the allocator under test served > 99% of malloc calls (its own counter) | ≥ 0.97× | ≥ 1.00× | mimalloc v3.4.5 |
| T1b | Same suite | ≥ 1.05× | ≥ 1.10× | glibc 2.44 |
| T1c | Same suite, TCMalloc confirmed in per-CPU mode | ≥ 0.95× | ≥ 1.00× | TCMalloc |
| T1d | pyperformance with default pymalloc; Redis with its bundled jemalloc (reported only, not gated) | report | — | same builds |
| T2 | mimalloc-bench geomean time [mibench] | ≤ 1.10× | ≤ 1.05× | mimalloc v3.4.5 |
| T3 | Median latency of a `malloc`+`free` pair (P1 microbenchmark) | ≤ 1.3× | ≤ 1.1× | mimalloc v3.4.5 |
| T3b | p99 and p99.9 `malloc`/`free` latency under mixed multithreaded load | ≤ 1.5× | ≤ 1.2× | mimalloc v3.4.5 |
| T4 | p99 under constant-rate load (wrk2-style [wrk2]): Redis, Envoy | ≤ 1.05× | ≤ 1.0× | best of mimalloc and jemalloc |
| T5 | Longest single inline-release stall inside `free` | ≤ 1 ms | ≤ 0.3 ms | — |
| M1 | RssAnon delta of a `/bin/true`-like program touching 10 classes; RssFile reported separately (4K pages, both THP configs) | ≤ +96 KiB | ≤ +48 KiB | glibc 2.44 |
| M2 | Thread-churn RSS-time integral (1k thread create/exit per second, 60 s) | ≤ 0.7× | ≤ 0.5× | mimalloc v3.4.5 |
| M3a | RSS 15 s after a burst ends, multithreaded, in a container, THP config (b) | ≤ live + FE_meas + max(4 MiB, 12.5%·live), where FE_meas is the front-end bytes reported by stats and itself ≤ the §5.9 bound | — | — |
| M3b | Same, THP config (a) | at 15 s: ≤ live + FE_meas + max(8 MiB, 25%·live); at 330 s: the M3a bound | — | — |
| M3s | Same, single-threaded, burst frees ≥ 32 MiB | at 15 s: ≤ live + max(32 MiB, U/4) + 4 MiB | — | — |
| M4 | Peak RSS geomean on the T1 suite | ≤ 1.0× | ≤ 0.95× | mimalloc v3.4.5; jemalloc 5.4.0 with `background_thread` |
| M5 | Phased workload under `memory.max` = 1.2× working set, including a variant where a co-tenant streams page cache | 0 OOM kills; RSS-time ≤ 0.85×; no throughput loss in the co-tenant variant vs the no-co-tenant run beyond 2% | — | jemalloc 5.4.0 with `background_thread` |
| M6 | Metadata ÷ committed | ≤ 0.5% | ≤ 0.35% | — |
| K1 | Steady-state VMAs owned by sohamalloc | ≤ 6 + 4 per extra window + 1 per solo (+2 if the GWP pool falls back to `PROT_NONE` guards with headroom) | — | `/proc/self/maps` CI gate |
| K2 | `mmap_lock` write acquisitions in steady state | 0/s excluding: solo map/unmap, window prefix growth, mode-2 decommit, opt-in `MADV_COLLAPSE`, and khugepaged collapses of our VMAs. Each excluded event is counted (khugepaged via the tracepoint's mm). **Gated on THP=madvise and THP=never**; reported on THP=always | — | lock tracepoints (E12) |
| K3 | Syscalls issued by sohamalloc before `main` | ≤ glibc + 8 (provisional: re-baselined from an `strace` of the P0 prototype, E1) | ≤ glibc + 6 | glibc 2.44 |
| K4 | Threads created in a single-threaded process | 0 | — | — |
| K5 | membarrier calls | ≤ 1 per second; 0 when the process affinity intersects nohz_full or isolated CPUs (default policy) | — | — |
| S0 | Throughput cost of the default tier | ≤ 2% vs tier −1 (also reported vs glibc and mimalloc) | ≤ 1% | tier −1 build |
| S1 | Throughput cost of `secure` | ≤ 10% | ≤ 6% | tier −1 build |
| SE | Security effectiveness (E22): each attack primitive a tier claims to stop is stopped in the how2heap-style suite | 100% of claimed | — | claims table §5.12 |
| U1 | `LD_PRELOAD` smoke suite (§8.4) | 100% | — | — |
| U2 | Required tuning for every gate | none | — | — |

---

## 4. Verified limitations of current allocators and how sohamalloc responds

### 4.1 Limitations with evidence

| # | Limitation (evidence) | sohamalloc response | Honest residual |
|---|---|---|---|
| L1 | **glibc arena fragmentation.** Ruby: unlimited arenas cost 1.73× memory for 0.9× response time [speedshop]. Cloudflare: TCMalloc cut RocksDB memory 2.5× [cloudflare]. Node workers: ~1100 MB vs ~80 MB [node-51868]. glibc only capped arenas at max(8, ncores) in 2.44 [glibc-arena] | No arenas. Pages are owned by slots; slot count is bounded by effective CPUs; free memory moves between slots as whole pages | Benchmark against glibc 2.44, not the 2.31 in older data |
| L2 | **Thread-churn stranding.** mimalloc #351: ~150 MB per new pool thread, 580 MB held after exit [mi-351]. #1025: rspack grows past 1 GB [mi-1025] | Exited slots go to a warm-first pool and are reused; Hoard bound with hysteresis; idle slots leased after 1 s | Without membarrier, idle *live* threads keep up to the structural bound S_struct each (§5.9) |
| L3 | **Idle programs never purge.** jemalloc #1398: 599 MB held vs 14 MB on 4.0.3 [je-1398]. TCMalloc OSS release rate defaults to 0 and needs an application thread [tc-params][tc-ext] | Frees drive a tick as well as mallocs. Local frees detect fully free pages. Pages that empty through the remote path return to the page heap. A budgeted inline trigger plus a lazy futex-idle thread (§5.9) | A single-threaded program that frees slowly and stays under the trigger keeps up to `max(32 MiB, U/4)` plus ≤ 4 MiB pinned by its remote buffer |
| L4 | **`MADV_FREE` makes RSS dishonest.** Go reverted it in 1.16 [go116][go-42330] | DONTNEED by default; LazyFree reported | Refault cost; DONTNEED alone is 1.5–2× slower [go-42330] |
| L5 | **Fixed THP policies lose either way.** mimalloc's 2 MiB purge minimum: +81% RSS [mi-1282]. Meta THP=always: +7.7% memory [lwn-thp]. Go: 100+ ms defrag stalls [go-61718]. Redis fork spikes 339 ms vs 21 ms [antirez] | THP matrix keyed on sysfs, including mTHP sizes; whole-hugeblock release; NOHUGEPAGE metadata and bootstrap block | Hugepage coverage under `defrag=madvise` will trail Temeraire unless E9 approves the dense-area option |
| L6 | **rseq fragility.** TCMalloc's own registration conflicts with glibc and silently falls back [mongo-90036]. Its `cpu_id_start` overlay broke on 6.19 [lwn-hyrum] (confirmed by red team 2). Out-of-range CPU ids crashed it in Envoy [tc-188] | v1.0 needs no rseq. In P4, fields are read only, there is no self-registration, and CIDs are bounds-checked | cid mode depends on ABI stability (7.1 V2 [k-rseqdoc]) |
| L7 | **Caches scale with core count.** TCMalloc caches 1.5 MiB per CPU [tc-design]; jemalloc uses 4×ncpu arenas | Slots ≤ 2× *effective* CPUs (cpuset and `cpu.max` quota); per-slot budget scales with the live heap | Structural worst case S_struct ≈ 20 MiB per idle slot without leases (§5.9) |
| L8 | **Sleeping threads cannot be purged.** PartitionAlloc admits this [pa-tc] | Non-blocking Dekker lease reclaims idle slots | Needs membarrier; skipped by default when the affinity touches nohz_full or isolated CPUs |
| L9 | **mimalloc v3 regressions.** Multithreaded calloc 0.62 s → 4.96 s; `rep stosb` slow at ≥ 128 B; singleton pages shared [mi-1104] | libc `memset`; solos never shared through slot caches; known-zero bump-cursor calloc | — |
| L10 | **Inline-metadata exploitation keeps evolving** [how2heap] | Per-page key, bswap and address encoding; range checks on head and next; records in a separate area behind a `PROT_NONE` gap | The inline link stays a target; a read primitive recovers one page's key (§5.12) |
| L11 | **Hardening is all or nothing.** Derived calibration: hardened_malloc 3.67× time, Scudo 2.24× vs glibc on one machine [rpm-csv] | Tiers. S0 is cheap and priced; S1 is opt-in | — |
| L12 | **Interposition is fragile:** `free(NULL)` before the constructor [mi-1341]; glibc 2.39 dynamic-TLS recursion [glibc-news]; `--as-needed` drops the allocator [ruby-18409]; RTLD_DEEPBIND [glibc-30186] | `gmap_n = 0` before init routes everything, NULL included, to a safe slow path; IE TLS ≤ 48 B; bootstrap arena; `DT_NEEDED` CI test; full symbol set incl. `memalign` [glibc-replace] | DEEPBIND: our pointers freed by glibc still crash inside glibc |
| L13 | **Complexity collapse.** jemalloc HPA "cratered" from piled-on tweaks [je-postmortem] | Code budget, an honest list of 8 concurrency protocols in v1.0 (each modelled), kill criteria, scope cut (§8.1) | Combining three designs adds risk (§7) |
| L14 | **VMA blow-up from `PROT_NONE` guards.** hardened_malloc needs `max_map_count` = 1048576 [hm-readme]; kernel default 65530 [k-vm] | Guards only in the GWP pool via `MADV_GUARD_INSTALL` by default | Guard markers are invisible in `/proc/*/maps` [criu-guard]; CRIU test added |
| L15 | **musl's static malloc collapses under contention:** 93.07 s vs 4.311 s with glibc [tweag] | Same allocator under musl; static recipe | Strong musl symbols need `objcopy` surgery |
| L16 | **Benchmarks mislead.** Peak-RSS-only, gperftools labelled "tc", one machine [mibench][rpm-csv]; layout bias [mytkowicz]; suites that never call the allocator under test [fleetbench][envoy-bazel][py-mem] | §8.5 protocol, served-fraction check | — |
| L17 | **glibc tcache key catches only same-thread double frees** [glibc-malloc] | Head check on push; remote chains walked by exact count at collection; S1 boundary check | Racing cross-thread double frees are best-effort |

### 4.2 Limits sohamalloc cannot remove

- **Robson's bound.** For non-moving allocators that place each request contiguously, the optimal competitive ratio stays Θ(log M) [bender26].
  - The paper's escape hatch is "request fragmentation": letting a request span k = 1+o(1) fragments lowers the bound to Θ(log log M) [bender26]. Page-granular release and whole-hugeblock release are how sohamalloc trades this off, at the price of more page-table fragments.
  - On real traces, allocators beat the worst-case bound 93.75% of the time [lamprakos]. Lifetime mixing, not worst-case placement, is the practical problem.
- **Statically bundled allocators.** Redis's bundled jemalloc ignores `LD_PRELOAD`; fleetbench and Envoy bundle TCMalloc by default [fleetbench][envoy-bazel]. They must be rebuilt.
- **MTE tag leakage** through speculation [tiktag].

### 4.3 What is actually new (claims kept narrow)

1. **Membarrier-based asymmetric lease for idle threads' caches.** It reclaims the per-thread caches of *idle live* threads, without rseq and without ever blocking the owner. The fast path is reused by a later cid mode.
   - The synchronisation pattern itself is prior art. liburcu uses `sys_membarrier` the same way [liburcu][urcu].
   - The red team also cites folly's asymmetric thread fence and Dice, Moir and Scherer's asymmetric Dekker work. I could not verify either; no source URL is in the material.
   - The claim is limited to applying this to allocator-cache reclaim, which we have not found in production allocators.
2. **A combination of release mechanisms**, not a first:
   - cgroup *pressure* levels with hysteresis;
   - a demand-window retention target;
   - a budgeted inline trigger;
   - a futex-idle thread that exits when the process becomes single-threaded.

   Activity-driven purging without a timer already exists in jemalloc's decay [je-man] and in mimalloc's purge delay [mi-readme]. The tick mechanism in each is an INFERENCE from source familiarity (red team 1).
3. **A fixed VMA layout** (one window, flags set once) with a CI-enforced VMA budget and an `mmap_lock`-write target.
4. **An "explain my memory" report that never invents a sum.**

Everything else is credited reuse.

---

## 5. Architecture

### 5.1 Runtime geometry and address-space layout

Every value is computed once at init and kept in one read-mostly cache line (`geo`). A CI lint bans page-size literals.

| Symbol | Value | Source |
|---|---|---|
| B (base page) | `getauxval(AT_PAGESZ)`: 4, 16 or 64 KiB | — |
| PMD | `/sys/kernel/mm/transparent_hugepage/hpage_pmd_size`, read lazily at first prefix growth beyond the bootstrap block; fallback B | [k-thp] |
| H (hugeblock) | PMD if PMD ≤ 2 MiB, else 2 MiB (the contiguous-PTE unit on 16K/64K granules) | [k-arm64hp] |
| F (first-touch folio) | Largest anonymous mTHP size whose `hugepages-<size>kB/enabled` is `always` (or `inherit` with top-level `always`), else B; read with the THP state | [k-thp] |
| S (slice) | max(64 KiB, B) | — |
| WALIGN | 2^36 (64 GiB), compile-time constant. Window base alignment, which makes record arithmetic constant | — |
| Wsz (window size) | 64 GiB if RLIMIT_AS is unlimited; otherwise clamp(pow2_floor(RLIMIT_AS/4), 256 MiB, 64 GiB). The single rule for RLIMIT_AS | — |
| Solo threshold | ≥ 32 MiB, or alignment > H | — |
| VA ceiling | 2^48. x86-64 4-level is 2^47; arm64 48-bit returns addresses up to 2^48 [k-arm64mem]. No hint above 2^47 is ever passed [k-5lvl] | — |

**The heap window.** This replaces v2's growable 64 MiB reservations, which grew *upward* with `MAP_FIXED_NOREPLACE` and so collided with Linux's top-down placement (both red teams; [k-mmapc][mmap]).

1. **Placement.** At init, sohamalloc reserves one window of Wsz bytes with `mmap(PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE)`, hinted at a random WALIGN-aligned address below 2^47 (randomness from `getrandom`).
2. **Retries.** On EEXIST, retry up to 16 random hints. Kernels older than 4.17 treat the flag as a hint and may return another address [mmap]; then sohamalloc checks alignment and unmaps-and-retries.
3. **Fallback.** Only when RLIMIT_AS is unlimited, fall back to over-mapping Wsz + WALIGN and trimming.
4. **Commit.** All growth happens *inside* the window by moving committed-prefix boundaries (§5.8.1), so neighbouring mappings never matter.
5. **More windows.** When a window fills, another is created the same way (+4 VMAs).

**VSZ trade-off, stated plainly.** VSZ will show about 64 GiB for `/bin/true` on an unlimited-RLIMIT_AS system.

- `PROT_NONE` private mappings are not accountable in any overcommit mode [k-overcommit].
- Untouched VA costs no page tables.
- The explain report shows window VA separately.
- The `window=` key shrinks it.

**Window layout** (offsets from the window base):

```
+0                      window header (4 KiB): magic^secret, epoch, commit high-water marks, DATA_OFF
+4 KiB                  hugeblock meta[Wsz/H] x 192 B   (64 GiB -> 6 MiB)
+12 MiB (window 1 only) gmap: 2^22 bytes (one per 64 MiB granule of 2^48), then the slot array
+REC_OFF = 32 MiB       slice records[Wsz/S] x 192 B    (64 GiB -> 192 MiB)
   -- metadata area: MADV_NOHUGEPAGE once at creation; committed as a growing prefix --
REC_OFF+recsize .. DATA_OFF   PROT_NONE gap (>= 1 base page; rounding to H makes it larger)
DATA_OFF = round_up(REC_OFF + recsize + B, H)
DATA_OFF .. DATA_OFF+H        bootstrap hugeblock (window 1): RW, MADV_NOHUGEPAGE
.. committed data prefix       RW
.. window end                  PROT_NONE tail; the last base page of the window is never handed out
```

- Overflow off the committed top hits the `PROT_NONE` tail. Underflow off the first data slice hits the gap. Neither costs anything in S0.
- The records area for slices below DATA_OFF exists as VA but is never touched.
- Metadata ÷ data VA is 192 B / 64 KiB = 0.29% [derived]. Only records of slices in use become resident.

**VMA budget (K1):**

| Item | VMAs |
|---|---|
| Window 1: metadata RW prefix, metadata `PROT_NONE` remainder/gap, bootstrap hugeblock (RW, NOHUGEPAGE), data RW prefix, data `PROT_NONE` tail | 5 |
| Each extra window (no bootstrap block) | 4 |
| GWP pool (created lazily) | 1 |
| Each solo | 1 |
| Locked mode (`mlockall`) | no extra; same prefix model |
| THP `defrag=defer/never` row: `MADV_HUGEPAGE` on [bootstrap end, window end) once at first growth, which covers whole VMAs | 0 |

**Never done at runtime:**

- `MADV_[NO]HUGEPAGE`, `mprotect` or `mbind` on sub-ranges other than the committed-prefix boundaries. The others split VMAs; the madvise variants also take the mmap write lock [k-madvise][k-numa].
- `mremap` inside a window.

### 5.2 Pointer → record lookup

**Granule map (`gmap`).**

- One byte per 64 MiB granule, 2^22 entries, in the window-1 metadata area, which is NOHUGEPAGE. That fixes red-team findings that a 4 MiB `.bss` array is THP-eligible under `enabled=always`.
- `geo.gmap` and `geo.gmap_n` live in the read-mostly geometry line. Before init, `gmap_n == 0`.
- Values: 0 foreign; 1 window; 2 solo-present (per-granule refcount in the solo table, §5.7.3); 4 GWP pool; 5 other internal.

```
lookup(p):                                   // [est] 8 instr; 1 byte load ∥ 1 record load
  a = p & ADDR_MASK                          // strip arm64 TBI/MTE top byte; x86 LAM-safe
  g = a >> 26
  if (unlikely(g >= geo.gmap_n)) -> slow     // pre-init (gmap_n == 0), NULL handled there
  if (unlikely(geo.gmap[g] != 1)) -> nonarena // NULL lands here after init (gmap[0] == 0)
  r = (a & ~(WALIGN-1)) + REC_OFF + ((a >> 16) & (WALIGN/S - 1)) * 192
```

The record address depends only on `a` and constants, not on the gmap byte, so the CPU issues both loads in parallel [est]. The `* 192` is one LEA (×3) plus a shift on x86 [est]. The `geo` loads are independent of `p` and L1-hot.

**Non-arena path** (slow): `p == NULL` returns. Solo and GWP pointers are resolved through an exact interval lookup in a small locked table. Handler-context frees never reach it (§5.10). Next come the bootstrap and emergency `.bss` ranges; otherwise the pointer is foreign.

**Foreign-pointer policy:**

| Pointer | S0 | S1 and above |
|---|---|---|
| Outside every sohamalloc range | Log once, count, *leak* | Abort with diagnostic |
| Inside our ranges but failing validation (unused or metadata slice: record kind 0; misaligned; bad magic) | Abort | Abort |
| Library build (not preloaded) | Forward to `dlsym(RTLD_NEXT,"free")`, resolved **lazily on the first foreign free**, after init is done, with any recursion served from the bootstrap arena. Whether glibc's `dlsym` allocates is UNVERIFIED (red team 1) | Same |

In S0, leaking a foreign pointer is safe because we never write to memory we do not own.

**Interior pointers never reach `free` legitimately:**

- Aligned small requests use a class that is a multiple of the alignment, in slice-aligned pages.
- Aligned runs start at the returned pointer.
- Alignments above H go to solos.
- Multi-slice pages carry `head_delta` in every slice's read-mostly line, so a block in a non-head slice resolves to its page with one subtraction on the fast path (§5.5.4).

**Rejected alternatives:**

- mimalloc's flat byte-per-slice map: 4 GiB VA reserve at 48 bits, plus a dependent load [mi-pagemap].
- TCMalloc's 3-level radix tree [tc-design].
- B's rtab + delta + meta: 3 dependent loads.

### 5.3 Size classes and page kinds

There are 64 classes; an id fits in 6 bits. This is the bootstrap table; E2 decides between it and a 52-class variant.

| Band | Class ids | Spacing | Worst waste (fraction of block) [derived] |
|---|---|---|---|
| 16–128 B | 0–7 | 16 B | 46.9% (17 B → 32 B, forced by 16 B alignment) |
| 129 B–1 KiB | 8–31 | 8 per doubling (16/32/64 B steps) | 10.9% (513 → 576) |
| 1–8 KiB | 32–43 | 4 per doubling | 19.9% (1025 → 1280) |
| 8–128 KiB | 44–59 | 4 per doubling | ≈ 20% |
| 128–256 KiB | 60–63: 160, 192, 224, 256 KiB | 4 per doubling | 19.4% (129 → 160 KiB) |

- The ~20% bound for 4-per-doubling matches jemalloc's manual [je-man].
- The denser 129 B–1 KiB band reflects that objects under 1 KB are 98% of allocations, though only 28% of bytes [asplos24].

**Lookup:**

- n ≤ 1024: `c = lut[(n+15)>>4]`, a 65-byte table.
- n > 1024: `lg = 63−clz(n−1); c = 32 + 4(lg−10) + (((n−1) >> (lg−2)) & 3)` [est ~6 instructions]. Checks [derived]: n = 1025 → 32 (1280 B); n = 1281 → 33; n = 2048 → 35; n = 131072 → 59 (128 KiB); n = 131073 → 60 (160 KiB); n = 262144 → 63.
- `n == 0` maps to class 0 and returns a unique 16 B block.

**Page kinds.** The generator script is checked in; its output is a header.

- **Small pages (ids 0–43, ≤ 8 KiB): one 64 KiB slice.**
  - Worst tail waste 6.25% (5120 B and 6144 B classes); classes ≤ 1 KiB waste ≤ 1% [derived].
  - Every page holds ≥ 8 objects.
- **Medium pages (ids 44–59, plus 160 KiB and 224 KiB).** Any slice multiple. The generator picks the smallest page P with P ≥ 4·bsize and P ≤ 1 MiB whose tail waste is ≤ 3.125%; if none exists, it picks the P ≤ 1 MiB with minimum tail waste. Examples [derived]:

  | Class | Page | Objects | Tail waste |
  |---|---|---|---|
  | 10 KiB | 192 KiB | 19 | 1.04% |
  | 12 KiB | 192 KiB | 16 | 0% |
  | 20 KiB | 320 KiB | 16 | 0% |
  | 28 KiB | 256 KiB | 9 | 1.56% |
  | 56 KiB | 448 KiB | 8 | 0% |
  | 112 KiB | 448 KiB | 4 | 0% |
  | 128 KiB | 512 KiB | 4 | 0% |
  | 160 KiB | 640 KiB | 4 | 0% |
  | 224 KiB | 896 KiB | 4 | 0% |

  The generator asserts that every class ≤ 128 KiB gets a page ≤ 512 KiB; spot checks confirm this [derived]. Only 160 and 224 KiB exceed it.

  This fixes both red teams' finding that 160 and 224 KiB are not slice multiples (2.5 and 3.5 slices). They are now 4-object medium pages with zero tail waste, made possible by per-slice `head_delta`, which removes the need for pages aligned to their own size.
- **Run classes (192 KiB and 256 KiB).** Exactly 3 and 4 slices, served as slice runs through the per-slot run cache (§5.7.2) with zero tail waste beyond class rounding [derived]. The property test asserts `run size == class size` and `class size % S == 0` for every run class.
- **Large runs (> 256 KiB and < 32 MiB):** slice-granular, page-heap allocated.
- **Solo (≥ 32 MiB):** dedicated mapping.

**Lazy provisioning** (PartitionAlloc's approach [pa-enc]).

- A fresh page is a bump region. Blocks are threaded onto the free list max(B, F) bytes at a time, in address order in S0 (shuffled in S1).
- On 4K pages without mTHP, a process touching 10 classes has about 10 base pages of data resident [derived].
- If an mTHP size F > B is set to `always`, provisioning works in F-sized steps, and M1's budget is recomputed from F (§5.8.3). The bootstrap hugeblock is NOHUGEPAGE, so a small process is not affected; whether `MADV_NOHUGEPAGE` also suppresses mTHP folios is UNVERIFIED and is tested in CI.

**Property tests:**

- The size→class map is monotone.
- Every size in [requested, usable] maps to the same class, as sized delete and `__size_returning_new` require [tc-ext].
- `bsize × cap ≤ page`.
- Run classes are slice multiples.

### 5.4 Records, ownership and invariants

Each 64 KiB slice has a 192 B record: three cache lines with three distinct writers. This is red team 2's fix: remote freers no longer read the line the owner writes on every pop.

```
line O (owner-written; head slice only):
  +0  u64 free       // [63:48] blocks on local list, [47:0] head (tag-stripped)
  +8  u16 prov; u16 cap; u32 pad
  +16 u64 bump       // bump cursor | zero_fresh bit (page zero-known when initialised)
  +24 u64 alt_link; u64 last_refill_epoch; ...
line R (read-mostly; written only at page init, claim and detach; copied into every slice of the page):
  +0  u32 owner      // [15:0] slot_id+1 (0 = unowned); [31:16] flags:
                     //   CLAIMING SAMPLED GUARDED LARGE SHARED S1CHK KIND0 (never all-ones)
  +4  u32 head_delta // bytes back to the head record; 0 for the head slice
  +8  u64 key        // per-page random, re-drawn at every page (re)init
  +16 u64 pbase; u32 psize; u32 bsize
  +32 u32 recip      // exact 32-bit reciprocal for boundary checks [sn-sct]
  +36 u8 cls, kind, node, pad; u32 epoch; u32 llc
line X (remote; head slice only):
  +0  atomic u64 xfree  // [63:48] count, [47:4] head, bit2 PENDING, bit1 QUEUED, bit0 ORPHAN
  +8  u32 folded        // local-free + unprovisioned count folded in at detach
  +16 pool/orphan links, donor slot and donation epoch, claim epoch
```

**Why each choice:**

- Packing count with head lets each local pop and push commit with **one store**. A thread stopped anywhere (preemption, signal, fork) leaves consistent state.
- Line R is shared-clean in every cache that reads it. Remote frees read only R and write only X, so the owner's line O never bounces (validated with `perf c2c` HITM counts in E6).

**Invariants:**

- **I1.** `owner == s+1` implies the page is referenced by slot s's `cur`, `alt` or run-cache entry.
- **I2.** Line O is written only (a) inside a critical section (CS) of slot s that has re-checked `owner == s+1` *inside* the CS, or (b) by a thread holding the page exclusively while it is unreferenced and `owner ∈ {0, CLAIMING}`.
- **I3.** Every multi-step transition is a sequence of single-commit steps. Every intermediate state is valid and can be completed by the next refill on that slot.
- **I4.** While `ORPHAN` or `QUEUED` is set, `xfree.count` is the page's *total* free count: remote chain plus `folded`. "Fully free" is exactly `count == cap`.

**Domain abstraction.** "A CS of slot s" means:

- in **TLS mode** (v1.0): the window between `CS_BEGIN` and `CS_END` (§5.5.3), executed by the thread that owns s;
- in **cid mode** (P4): an rseq critical section running with `mm_cid == s`.

**Detach** (a page leaves slot s; by the owner in a slow path, or by a reclaimer holding the lease):

1. Commit `owner = CLAIMING|s` (tail copies first, head last).
2. Commit `bins[c].cur` or `alt` = replacement.
3. The page is now exclusive. Compute `folded = cnt(free) + (cap − prov)` and store it in line X.
4. Take the class pool lock and link the page onto the orphan list.
5. CAS `xfree`: count += folded, set ORPHAN. If the new count is ≥ cap/8 (or == cap), set PENDING in the same CAS and handle it immediately under the held lock (move to the pool, or return the page).
6. Set `owner = 0` and release the lock.

**Remote CAS rule** (one CAS per destination page, from a flush or `remote_direct`):

```
loop:
  old = load(xfree)
  write the tail link of my chain = enc(old.head, tail)
  new = {count: old.count + k, head: my_head, flags: old.flags}
  responsible = false
  if ((old.ORPHAN || old.QUEUED) && !old.PENDING &&
      ((old.ORPHAN && new.count >= cap/8) || new.count == cap)) { new.PENDING = 1; responsible = true }
  if (!CAS(xfree, old, new)) goto loop
  if (responsible) resolve(page)                 // never from handler context (§5.10)
resolve(page):                                   // under the class pool lock
  repeat:
    x = load(xfree)
    if (!x.PENDING) break                        // a claimer took the page; nothing to do
    if (x.count == cap)          { unlink from orphan list or pool; CAS(x -> {0,0,0}); return page to filler }
    else if (x.ORPHAN && x.count >= cap/8) { move orphan -> pool; CAS(x -> x - ORPHAN + QUEUED - PENDING) }
    else                         { CAS(x -> x - PENDING) }
  until the CAS succeeds                         // a concurrent free that changed count makes it fail -> re-evaluate
```

Only the PENDING holder moves the page between lists, and it re-checks under the lock. Its clearing CAS fails if any free arrived in the meantime. So no transition is lost, and no page is returned while it is still on a list. This fixes the red-team race where the CAS "decided" but the list move happened later.

**Claim** (a page joins slot s; in s's slow path, inside a PreventFork section):

1. Under the pool lock: unlink the page from the pool or orphan list, then `old = xchg(xfree, 0)`, which clears ORPHAN, QUEUED and PENDING together. A PENDING holder waiting for the lock will then find `!PENDING` and do nothing.
2. Release the lock. `chain_len = old.count − folded` if the page was unowned.
3. Walk exactly `chain_len` nodes. Abort with a diagnostic if the last node's link is not NULL, or if `local + chain_len + (cap − prov) > cap`; this catches cycles from racing double frees (red team 1). Merge with one commit to `free`.
4. Write owner copies = CLAIMING|s, commit `bins[c].cur = pg`, then commit `owner = s+1` only if `bins[c].cur == pg` and the lease word is 0.

**Owned-page collection** (refill on `cur`): `xchg(xfree, 0)` while the page is owned (no flags), then walk exactly `count` nodes and merge with one commit. This runs inside a PreventFork section (§5.10).

### 5.5 Front end: slots, fast paths and remote frees

#### 5.5.1 Slot indexing and binding modes

| Mode | Binding | When it is used |
|---|---|---|
| **tls-exclusive** (v1.0 default) | `slot = tls.slot`. One thread per slot; exited threads return slots to the pool | Always available |
| **tls-shared** | Threads beyond H_max attach to shared slots. Their `countdown` and `tick` are held ≤ 0, so every call takes the event path, which uses a per-slot trylock as in Scudo's shared TSDs [scudo-tsd]. While holding the trylock, a shared thread may free locally to pages of that shared slot | Too many threads |
| **rseq-cid** (P4 option) | `slot = slots[cid]` with `cid = rseq->mm_cid` and `cid < nslots`; on failure fall back to TLS. Slot array sized from possible CPUs; CIDs are below that [k-sched] | Only after gate E14 passes, `eff_rseq_size ≥ 28`, and `MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ` succeeds (since 5.10 [membarrier]) |

- **H_max** = clamp(2 × E, 8, 1024), where E = min(allowed CPUs, ceil(`cpu.max` quota / period)) [k-cg2].
- Until topology discovery runs (U > 4 MiB), E = allowed CPUs.
- E14 includes a 2000-thread / 64-CPU case for TLS mode.

**Slot** (4 KiB, in the window-1 metadata area, so NOHUGEPAGE and lazily touched):

```
line0: u8 busy; u8 pad; u16 flags; atomic u32 lease; u32 tag; u32 act_epoch;
       u64 rbuf*; u64 last_event_ns; atomic u32 pf_count; u32 rng; u64 free_est; u64 live_bytes
bins[64] x 16 B: { rec* cur; rec* alt }        // alt used only for small classes (ids 0-43)
runc[4]  x 16 B: { run*, class }               // total <= 1 MiB per slot (PARAM)
spare[2]: empty-page spares, at most 2 per slot
```

**TLS (initial-exec, exactly 48 B):** `i64 countdown; slot* slot; rbuf* spare; void* deferred; u32 slot_tag; i32 tick; u8 depth; u8 state; u8 pad[6]`.

- This fits glibc's 512 B surplus static TLS for late `dlopen` [glibc-tun].
- Replacement allocators must use initial-exec TLS [glibc-replace].
- glibc 2.39+ calls malloc during dynamic-TLS access [glibc-news], another reason to stay in IE.
- The RNG seed moved into the slot.
- `slot_tag == 0` means "no exclusive slot": a new, zombie or shared thread. No page owner word ever equals a valid tag except its owner's, because valid tags are ≥ 1 and the free path compares the full 32-bit owner word.

**rseq detection (P4):**

- `__rseq_offset` and `__rseq_size` are referenced weakly.
- If `__rseq_size ∈ {20, 32}`, effective size = `min(32, AT_RSEQ_FEATURE_SIZE)`; otherwise `__rseq_size` [lkml-rseqsize][glibc-rseq].
- `__rseq_size == 0` → TLS mode. No self-registration in v1.x.
- Kernel-owned fields are only read; only `rseq_cs` is written, because under 7.1 V2 writes to kernel fields kill the process [k-rseqdoc].
- `RSEQ_SIG` is the per-architecture constant from glibc's `<sys/rseq.h>`. No abort probe. That glibc uses exactly this constant is UNVERIFIED and is checked by a build-time test.

#### 5.5.2 Link encoding (fixed)

Both red teams showed that v2's encode `bswap(n) ^ K` and decode `bswap(E) ^ K` are not inverses. The XOR terms outside the bswap do not cancel; a numerical counter-example is in red team 1's report. The corrected pair is:

```
K(r, a)   = r.R.key ^ (a >> 12)
enc(n, a) = bswap(n ^ K(r, a))          // stored in the first 8 bytes of block a
dec(E, a) = bswap(E) ^ K(r, a)          // = n
```

- `dec(enc(n, a), a) = bswap(bswap(n ^ K)) ^ K = n` [derived].
- bswap stays the outermost operation on the stored value. A partial overwrite of the stored low bytes therefore corrupts the *high* bytes of the decoded pointer, which makes it non-canonical: the PartitionAlloc property [pa-enc].
- NULL is stored as `bswap(K)`.
- One `static inline` pair is used by provisioning, local free, remote-chain building and collection.
- P0 exit gate: a property test that `dec(enc(x)) == x` for random keys, addresses and pointers, and that every writer goes through `enc`.

#### 5.5.3 Critical-section primitives (TLS mode)

```
CS_BEGIN(s):  if (s->busy) goto reentrant;            // a signal handler interrupted a CS or slow path
              atomic_store_relaxed(&s->busy, 1);
              atomic_signal_fence(seq_cst);           // compiler barrier
              l = atomic_load_acquire(&s->lease);      // LDAPR/LDAR on arm64; plain load on x86
              if (l) { atomic_store_release(&s->busy, 0); goto leased(l); }
CS_END(s):    atomic_store_release(&s->busy, 0);      // STLRB on arm64; plain store on x86
```

- `busy`, `lease` and the free word are C11 atomics: relaxed where noted, so there is no data-race UB.
- Slow paths keep `busy = 1` for their whole duration. Any allocator code that holds an allocator lock runs with `tls.depth > 0`, and with `busy = 1` if the thread has a slot.
- Because `CS_BEGIN` refuses entry when `busy` is set, a nested CS never runs, and `CS_END` writing 0 is correct.

#### 5.5.4 Fast paths

**malloc, n ≤ 1024:**

```
// [est] ~34 instr, 0 atomics RMW; dependent chain: tls.slot -> bins[c].cur -> r.O.free -> *b
t = &tls
if ((t->countdown -= n) <= 0) goto event       // starts at 0, so the first call (even malloc(0)) attaches
c  = lut[(n + 15) >> 4]
s  = t->slot
CS_BEGIN(s)
r  = s->bins[c].cur                            // sentinel record when empty (free == 0)
h  = r->O.free
b  = h & PTR48; if (!b) goto refill
if (b - r->R.pbase >= r->R.psize) goto corrupt                // S0 head check (red team 2)
nx = bswap(*(u64*)b) ^ r->R.key ^ (b >> 12)                   // dec()
if (nx && nx - r->R.pbase >= r->R.psize) goto corrupt         // next is NULL or in this page
atomic_store_relaxed(&r->O.free, nx | ((h - CNT1) & CNTMASK)) // COMMIT: single store
CS_END(s)
prefetchw(nx)                                  // TCMalloc prefetches too; 16% of its cycles [asplos24]
return b
```

- **Cost** [est]: about 34 instructions, two predictable data-dependent branches plus four rarely taken checks, and 0 atomic read-modify-writes. Dependent latency is about 16–22 cycles with all-L1 hits, partly overlapped. mimalloc describes its own path as "about 7 to 10 instructions with a single test" [mi-alloc].
- The count went up from v2's ~24 because of the reentrancy check, the acquire load (free on x86, near-free on arm64) and the S0 head check. E11 may move the head check to S1 (kill criterion F26).
- The `<= 0` test fixes red team 1's finding: malloc(0) as a thread's first call would otherwise have reached the fast path with `tls.slot == NULL`.

**malloc, 1 KiB < n ≤ 8 KiB:** the same body, with the class from the `clz` formula (§5.3), about 6 more instructions [est]. Larger requests take the medium/run path through the slot (`bins` for medium classes, run cache for run classes) with the same CS discipline.

**free:**

```
// [est] ~36 instr; critical chain: arith -> r.R.owner (1 load) -> compare
a = p & ADDR_MASK; g = a >> 26
if (g >= geo.gmap_n || geo.gmap[g] != 1) goto nonarena   // NULL, solo, foreign, pre-init
r = (a & ~(WALIGN-1)) + REC_OFF + ((a >> 16) & (WALIGN/S - 1)) * 192
if (a & 15) goto bad                                      // S0 alignment check
t = &tls
tag = t->slot_tag
if (tag == 0) goto unattached_free                        // new, zombie or shared thread (§5.10)
s = t->slot
CS_BEGIN(s)
if (r->R.owner != tag) goto not_local                     // checked INSIDE the CS (both red teams)
rh = r - r->R.head_delta                                  // head record for multi-slice pages
h = rh->O.free
if ((h & PTR48) == a) goto double_free                    // S0 head check
*(u64*)a = bswap((h & PTR48) ^ r->R.key ^ (a >> 12))      // enc(); idempotent if repeated
atomic_store_relaxed(&rh->O.free, a | ((h + CNT1) & CNTMASK))   // COMMIT
if (cnt(h) + 1 == rh->O.prov) goto page_empty             // every provisioned block is back locally
CS_END(s)
if ((t->tick -= r->R.bsize) <= 0) goto free_event         // frees drive periodic jobs too (red team 2)
```

- **The TOCTOU fix.** In v2 the owner comparison ran before `busy = 1`. A complete lease cycle, or a handler's refill, could detach the page in that window, and the free then committed into a page another slot owned. Now the comparison happens after `CS_BEGIN` has observed `lease == 0` with acquire semantics. A reclaimer cannot take the slot while `busy == 1` (§5.5.6), and a handler cannot enter (reentrancy check). So between the check and the commit, the page cannot change owner.
- **Tag collision fix.** Unowned pages have owner 0 and unattached threads have tag 0. The explicit `tag == 0` branch keeps them from ever meeting on the local path.
- **Emptiness.** If `cnt(free) == prov`, all provisioned blocks are locally free. No remote block can be outstanding, since a block cannot be on both lists, so the page is fully free without reading line X.
  - `page_empty` (busy still 1): if the page is `cur`, keep it.
  - If it is `alt` or a run-cache page: move it to a slot spare if fewer than 2 spares exist; otherwise detach it and return it to the filler.
- **Sized free** (`free_sized`, sized `delete`) takes the same path. Size is a validation hint only: checked lazily at collection in S0 and on every call in S1 [tc-msd]. A sized delete can legally arrive unsized [cxx-new].
- **errno** is saved and restored around every syscall reachable from `free` [glibc-replace].

**`not_local` (still inside the CS):**

```
if (r->R.owner & FLAGS) { CS_END; goto generic }            // SAMPLED, LARGE, GUARDED, CLAIMING
if (r->R.bsize > 1024 || REMOTE_DIRECT_MODE) { CS_END; goto remote_direct }   // one CAS (§5.4)
rb = s->rbuf; n = rb->n
if (n == RB_CAP) goto flush                                 // RB_CAP = 64 (PARAM); busy stays 1
rb->p[n] = a; atomic_store_relaxed(&rb->n, n + 1)           // COMMIT
CS_END(s)
```

[est] about 22 instructions, 0 atomic RMWs. Buffered bytes ≤ 64 × 1 KiB = 64 KiB per slot. Pinned destination pages are ≤ 64, i.e. ≤ 4 MiB of small pages per slot [derived] (red team 2: state the pinning bound as pages).

#### 5.5.5 Flush (commit-swap with a per-thread spare)

The flush runs in a slow path with `busy = 1`, inside a PreventFork section:

1. `old = s->rbuf; s->rbuf = t->spare` (COMMIT, single store); `t->spare = NULL`. The empty spare is always present: it is created at attach from the internal metadata arena and returned at detach.
2. Sort the ≤ 64 pointers by record address. Build one chain per page with `enc()` using that page's key and address. Apply the §5.4 remote CAS rule once per destination page; the retry loop rewrites the tail link from the observed head.
3. `old->n = 0; t->spare = old`.

There is no shared spare-buffer stack. That removes red team 1's ABA and empty-pool concerns, and one lock-free protocol.

**Cost** [est]: about 8–15 cycles amortised per object, one CAS per destination page. mimalloc pays one CAS per object [mi-free].

**Flush triggers:** buffer full; every slow path of the slot; tick or countdown events where the buffer is older than 50 ms (PARAM); thread exit; fork prepare; lease reclaim. Because frees also drive `tick`, a free-only consumer thread ages out its buffer.

**Default (E6).** Batching is on if the producer/consumer subset improves ≥ 3% on the geomean and no mimalloc-bench benchmark regresses by more than 3%. Otherwise per-object CAS is the default and batching is opt-in. BatchIt measured −9.9% to −27.6% time on xmalloc-test, up to 20% on back-pressured message passing, and 6–8% regressions elsewhere [batchit].

#### 5.5.6 Idle-slot reclaim: the non-blocking lease

The lease word is a 4-byte-aligned `atomic u32` with states `0`, `REQ`, `TAKEN` and `ABANDONED`. No futex is ever used on it. That fixes red team 1's finding that `futex` on a `u8` returns EINVAL and could lose a wakeup [futex]; the owner simply never waits.

**Reclaimer** (the background thread only; it exists only in multithreaded processes):

```
candidates: slots whose act_epoch (bumped on every countdown/tick event and slow path) has not changed for >= 1 s,
            AND (free_est >= 64 KiB OR rbuf non-empty)                                           PARAM
first batch: membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED); any failure latches leases off
for each candidate v: CAS(v->lease, 0, REQ)
rc = membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0)           // one per batch, <= 1 batch/s
if (rc != 0) { for each v: CAS(v->lease, REQ, 0); latch leases off; return }   // EPERM => no fence happened
for each candidate v:
   if (atomic_load_acquire(&v->busy) == 0 && CAS_acq_rel(v->lease, REQ, TAKEN)) reclaim(v)
   else CAS(v->lease, REQ, 0)                                     // owner active or it cancelled: withdraw
reclaim(v):  flush v->rbuf (commit-swap into a reclaimer-owned buffer, then §5.5.5 steps 2-3);
             detach every page in bins cur/alt, the run cache and the spares (§5.4); set bins to the sentinel;
             if (!CAS_release(v->lease, TAKEN, 0)) { /* ABANDONED */ v->lease = 0; push v to the slot pool }
```

**Owner `leased(l)` path** (entered from `CS_BEGIN` with `busy` already reset to 0):

- `l == REQ`: `CAS(lease, REQ, 0)`. On success the owner has **cancelled** the lease and retries the operation. On failure the state is now TAKEN; continue below.
- `l == TAKEN`: `CAS(lease, TAKEN, ABANDONED)`. The owner attaches a fresh or pooled slot (slot-pool lock, or shared mode at H_max), updates `tls.slot`/`slot_tag` and retries. If the CAS fails because the reclaim already finished (lease is 0), it retries on the same slot.

**Why it is correct.**

- *Handshake.* The owner's `busy = 1` store precedes its acquire load of `lease`. The reclaimer's `REQ` store precedes `membarrier`, which acts as a full barrier on every running thread of the process; threads that were not running get one at the context switch [membarrier]. So either the owner's load sees `REQ`/`TAKEN`, or the reclaimer's post-fence load sees `busy == 1`. This is the asymmetric-Dekker argument liburcu relies on [urcu].
- *A CS that straddles the IPI.* The reclaimer either sees `busy == 1` and withdraws, or sees the 0 written by the owner's `CS_END`. That store is a store-release and the reclaimer's load is an acquire, so all of the CS's commits are visible.
- *After a reclaim.* The reclaimer's `CAS_release(TAKEN→0)` pairs with the owner's acquire load of `lease`, so the owner sees the sentinel bins.
- This answers red team 2's critical finding (x86-TSO hides missing release/acquire). A second membarrier, the liburcu pattern [urcu], is unnecessary *given* the release/acquire pairs. If GenMC or herd7 finds otherwise, the second fence is added.
- *Mutual exclusion* comes from the CAS on `lease`: either the owner cancels (`REQ→0`) or the reclaimer takes (`REQ→TAKEN`), never both.
- *No waiting.* The owner never blocks on the reclaimer, so there is no priority inversion with SCHED_FIFO owners (red team 2).
- *Idleness signal.* The signal is "no allocator activity at all for 1 s", not "no slow path", so a busy steady-state thread is not chosen. E16 gates leases per second on fully busy threads at 0.

**Fence hygiene:**

- At most 1 batch per second (K5), counted in stats.
- `MEMBARRIER_CMD_PRIVATE_EXPEDITED` exists since Linux 4.14 and returns EPERM unless the process registered first [membarrier] (verified by both red teams). Registration happens lazily at the first lease batch, never at init (K3).
- **Default skip:** when the process affinity (`sched_getaffinity(pid)`) intersects `/sys/devices/system/cpu/nohz_full` or `/sys/devices/system/cpu/isolated`. Both attributes exist [k-cpu].
  - Red team 2 notes that PRIVATE_EXPEDITED interrupts only CPUs currently running this process's threads [membarrier], so the skip is conservative. It is kept as the default because those threads are exactly the RT/DPDK threads an IPI would disturb.
  - `lease=always` overrides it.
- With virtual ids the target CPU cannot be narrowed, which is why TCMalloc's `FenceCpu` falls back to fencing all CPUs [tc-percpu].

**cid mode (P4)** keeps Judge 1's corrected ordering:

1. `CAS(v->lease, 0, ME)`.
2. **Before the fence:** write the sentinel into v's bins and rbuf, and `CLAIMING` into the owner words of every page v references (finite by I1).
3. `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ)`, which aborts every in-flight rseq CS of the process [membarrier].
4. **After the fence:** re-read v's bins, rewrite sentinels and owner tags, collect, process the old rbuf, detach, and clear the lease.

Every cid-mode CS that commits slot-owned state checks `lease` before its commit (pop, refill install, rbuf push, claim finalisation, free push). If E14 shows the free-push check costs more than 0.5%, a two-fence variant replaces it. Both variants are modelled in P4 before any cid code.

### 5.6 Slow paths and pools

**Refill** (`cur` is empty), with `busy = 1`, inside a PreventFork section:

1. Collect `xfree` on `cur` (§5.4 owned-page collection). In S0, validate each remote node with the exact reciprocal boundary check during the walk. If anything was collected, retry the pop.
2. If `prov < cap`, provision the next max(B, F) bytes of blocks, in address order in S0 and in shuffled 8-block windows in S1.
3. Swap in `alt` if it has free blocks (small classes only).
4. Detach `cur` and claim a page from the pools.
5. Process the TLS deferred-free list (§5.10) if it is non-empty.

Estimate [est]: 150–600 cycles without a lock; 1–3 µs with a pool lock.

**Pools (v1.0):** one per class, behind a mutex, in a single node shard. Per-(LLC, class) and per-node pools are P4. Three buckets by free fraction at insertion: [1/8, 1/4), [1/4, 1/2), [1/2, 1).

Refill order:

1. Own `alt`.
2. Class pool, fullest non-empty bucket: the "fullest page that still has ≥ 1/8 free" rule. It raises occupancy, as span prioritisation did (−1.41% fleet memory [asplos24]), without handing out scattered holes.
3. Orphans: scan at most 8 with count > 0.
4. A fresh page from the filler.

Pages this slot donated less than 100 ms ago are skipped unless nothing else is available (hysteresis, §5.9).

**Sweep** (on a countdown or tick event, at most once per 10 ms per slot, PARAM):

- Scan `cur`/`alt` of the 64 bins and the run cache, about 140 record reads [est ~300 cycles]. Compute `free_est` exactly.
- Apply the Hoard bound with hysteresis (§5.9).
- Detach fully free pages not refilled since the last sweep, beyond the 2 spares.
- Detach medium `cur` pages idle for a whole sweep interval.
- Flush an rbuf older than 50 ms.
- Bump `act_epoch`.
- Create the background thread if it is wanted, before `CS_BEGIN` (§5.9).

### 5.7 Page heap, large allocations and realloc

#### 5.7.1 Hugeblock filler (one shard and mutex in v1.0; no syscalls under the lock)

- **Placement of k slices** follows Temeraire's actual order: the smallest longest-free-range ≥ k, ties broken by most allocations, freshly donated tails ranked last. TCMalloc reports that a simpler order fragmented its page heap, and that allocation count beat allocated pages as a key [tc-hpf]. Implementation: a bitmap index over (longest free range × quantised allocation count) with `tzcnt`, giving O(1).
- **Density split, keyed on kind** (red team 1 found that the object-count rule contradicted the page table): small pages go to *dense* hugeblocks, medium pages and runs to *sparse* ones. This is ASPLOS'24's capacity proxy: +1.02% throughput, −0.82% memory, zero runtime cost [asplos24].
- **Whole hugeblocks** come from address-ordered first fit within the window, which keeps the committed prefix low. Red team 2 found no First-Fit/Best-Fit competitive-ratio statement in bender26's abstract, so that claim is UNVERIFIED until checked against the paper body.
- **Hugeblock meta (192 B):** used, released and zero-known bitmaps; longest free range; allocation count; state; last-used epoch; density class.
- **Lock hold:** bitmap work only, exported as lock-wait stats. The rseq time-slice extension for lock holders is experiment E13 (7.0+ and only when glibc's registration exceeds 32 bytes [k-rseqdoc]; P4).

#### 5.7.2 Large runs and run classes (192 KiB ≤ n < 32 MiB)

- Runs are slice-granular and recorded in the head slice's record (kind LARGE). Every slice of the run carries `head_delta`.
- Runs ≥ H take contiguous whole hugeblocks. The slack in the last hugeblock is donated to the filler, a simplified HugeRegion idea [tc-regions].
- **Per-slot run cache:** up to 4 entries, ≤ 1 MiB in total (PARAM), keyed by size and counted against the slot budget. Run classes 192 and 256 KiB use it as their "page". This answers the critique that I/O-buffer workloads would otherwise hit the shard lock on every operation.
- Remote large frees go straight to the shard.
- **Run tails:** when a reused run exceeds the request by ≥ 2·B, the dirty tail is queued for DONTNEED by the release engine, not released inline.

#### 5.7.3 Solo regions (≥ 32 MiB, or alignment > H)

- **Mapping.** A dedicated `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)` **without** `MAP_NORESERVE`, over-mapped by max(H, alignment) and trimmed.
  - In mode 0 this keeps glibc's behaviour: the kernel heuristic refuses obviously excessive requests and malloc returns NULL with ENOMEM [k-overcommit][k-util] (red team 1).
  - Conformance test: `malloc(RAM + swap + 1)` returns NULL in mode 0, as with glibc.
- **gmap.** The solo table keeps a per-granule solo count. A gmap byte becomes 2 when the count goes 0→1 and is cleared only at 1→0. That fixes red team 1's finding that unmapping one solo could hide a neighbour sharing a granule. gmap==2 is always resolved by an exact interval lookup.
- **Freeing.** `munmap` after a small cache: at most 2 mappings, at most 25% of the live heap, at most 2 s (PARAM; Scudo's secondary-cache precedent [scudo-flags]).
- **Guard (S1 only, and only when `mlockall` is not active).** One mapped base page after the object end is *retained* inside the mapping and `MADV_GUARD_INSTALL`ed (6.13+). It splits THP only in the last hugeblock. Guard install requires an unlocked mapping, and mlocking guard regions fails [madvise]. S0 solos have no guard.
- **realloc growth.** Solos are the *only* objects that `realloc` grows with `mremap`. They own their VMA, and moved anonymous VMAs "can almost never" merge [mremap][lwn-mremap].
  - Growth first reserves an aligned destination: `mmap(PROT_NONE)` of size + alignment, then trim.
  - Then it calls `mremap(MREMAP_MAYMOVE|MREMAP_FIXED)` onto that destination and updates the solo table and granule counts under the table lock.

#### 5.7.4 calloc, realloc, aligned allocation

**calloc:**

- `__builtin_mul_overflow`; anything above `PTRDIFF_MAX` → NULL with ENOMEM.
- Zeroing is skipped only for:
  - **bump-cursor blocks**, taken directly from the unprovisioned part of a page whose `zero_fresh` bit is set. Such blocks were never threaded, so no link word was ever written to them. This fixes red team 1's finding that provisioning writes a link into every threaded block.
  - **runs** whose slices are all set in the zero-known bitmap.
- `zero_fresh` and zero-known bits are set only for never-touched memory or after a DONTNEED call returns success. They are never set for `MADV_FREE`d memory, whose contents are indeterminate [madvise], and are cleared on reuse and on MTE retag.
- Every other calloc uses libc `memset`, avoiding mimalloc's `rep stosb` problem [mi-1104].
- Differential test: calloc returns all-zero memory for every class, right after fresh provisioning and after reuse.

**realloc:**

- Same class → return p.
- Runs grow in place when the following slices are free (metadata only, under the shard lock); otherwise allocate, copy, free.
- Shrinking a run donates the tail.
- Solos use `mremap` (§5.7.3).
- `realloc(p, 0)` frees p and returns NULL, as glibc does [glibc-malloc]. On failure the old block is left intact, with ENOMEM.

**Aligned allocation:**

- alignment ≤ 16: normal path.
- ≤ 64 KiB: the smallest class with `size % align == 0`, in slice-aligned pages. Classes 2^k·{1.25, 1.5, 1.75, 2} are multiples of 2^(k−2), and the power-of-two class 2^j exists for every j ≥ 4, so one always exists.
- ≤ H: aligned run.
- Above H: solo.
- `posix_memalign` returns EINVAL as specified [pmemalign]. `aligned_alloc` accepts a size that is not a multiple of the alignment, as glibc does [glibc-news]. `memalign` rounds a non-power-of-two alignment up to a power of two, as glibc's `_mid_memalign` does [glibc-malloc].

### 5.8 OS and kernel layer

#### 5.8.1 Commit model (one model for every overcommit mode and for `mlockall`)

Inside each window there are two committed prefixes: metadata and data. Each grows by `mprotect(PROT_READ|PROT_WRITE)` on the next extent above its current boundary. The newly RW range merges with the RW prefix below it, so a window stays at 4 VMAs (5 for window 1).

| Situation | Growth step | Charge | Decommit |
|---|---|---|---|
| Modes 0/1, not locked | data: clamp(current committed, 64 MiB, 1 GiB); metadata: proportional | None. The window is `MAP_NORESERVE`, so VM_NORESERVE is set and making it writable charges nothing. This is an INFERENCE from [k-mprotect][k-mmapc], tested in E11b | `MADV_DONTNEED`; the prefix never shrinks |
| Mode 2 | 4·H (PARAM) | `mprotect(RW)` charges the extent; ENOMEM → malloc returns NULL with ENOMEM. `PROT_NONE` reservations cost nothing [k-overcommit] | v1.0: DONTNEED only, so the charge never shrinks (documented). v1.1: DONTNEED, then `mmap(top_extent, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, PROT_NONE)`. This drops the charge, which `mprotect(PROT_NONE)` would not for faulted ranges [k-mprotect]. It merges with the tail; under the HUGEPAGE row, `MADV_HUGEPAGE` is reapplied to the new extent so flags still match |
| Locked (`mlockall(MCL_FUTURE)` detected through `VmLck` in `/proc/self/status` at each growth, or an EAGAIN/ENOMEM from growth) | 4·H | Each committed extent is locked and populated at commit time [mlock], which is why steps stay small | EINVAL from DONTNEED → `MADV_DONTNEED_LOCKED` (5.18 [k-dnl]) under pressure only; otherwise "not released" |

Further rules:

- **Growth count.** Data growth doubles up to 1 GiB, then goes linearly in 1 GiB steps. A 100 GiB heap therefore needs about 104 growth steps: 5 doubling steps reach ~1.94 GiB, then ~99 steps of 1 GiB [derived]. Red team 1 was right that v2's "logarithmic" wording and its 107-step count were wrong. Each step takes `mmap_lock` for writing once and is counted under K2's exclusions.
- **The overcommit mode** is read once, from `/proc/sys/vm/overcommit_memory` with raw `open`/`read`/`close`, at the first growth beyond the bootstrap block. If the read fails, mode 0 is assumed.
- **Naming:** `PR_SET_VMA_ANON_NAME "sohamalloc"` (5.17) is applied to whole VMAs at the first growth, which preserves merges [k-vmaname].

#### 5.8.2 Release engine (simplified to two paths)

1. **Gather.** Candidates (whole hugeblocks first, then subrelease runs ≥ 64 KiB) are marked RELEASING under the shard lock and gathered into an iovec of ≤ 1024 entries (`UIO_MAXIOV` [k-uio]). The lock is then dropped.
2. **Issue the release:**
   - `process_madvise(PIDFD_SELF, iov, n, MADV_DONTNEED, 0)`. `PIDFD_SELF` is defined in the 6.15 uapi [k-pidfd], and advice on the calling process is allowed since 6.13 [pmadvise]. The man page does not document `PIDFD_SELF` (red team 2).
   - The first real call doubles as the probe. EPERM, EINVAL, ENOSYS or EBADF latch the fallback, and the batch is reissued as a coalesced `madvise` loop.
   - Partial returns resume from the returned byte count.
   - v2's 6.13–6.14 per-batch `pidfd_open` path is **dropped**. It added a version rung for two releases, and a concurrent fork could copy the pidfd [pidfd-open] (both red teams).
3. **Locked pages.** EINVAL on locked pages → `MADV_DONTNEED_LOCKED` under pressure only.
4. **Commit the result.** Re-lock and mark the ranges released and zero-known.

**Version notes.**

- One TLB flush per vector from 6.16, and DONTNEED under a per-VMA read lock from 6.17, are attributed to [k-madvise] but are UNVERIFIED for those exact versions.
- jemalloc ships the same mechanism behind a gate [je-pages].
- DONTNEED never takes `mmap_lock` for writing, so this ladder exists for flush cost, not for K2.

**TLB note.** On x86 without INVLPGB every flush IPIs each CPU the mm has run on. INVLPGB (6.15, AMD) applies only to mms active on ≥ 3 CPUs [lwn-invlpgb]. arm64 uses broadcast TLBI [k-arm64tlb]. Batching pays most on large x86 processes.

#### 5.8.3 THP policy

State read at the first growth beyond the bootstrap block, not at init (K3):

- `enabled`, `defrag`, `khugepaged/max_ptes_none`, `shrink_underused` and every `hugepages-<size>kB/enabled` entry;
- `prctl(PR_GET_THP_DISABLE)`;
- `hpage_pmd_size` [k-thp].

**Fixed regardless of state:** the window-1 bootstrap hugeblock and every metadata area are `MADV_NOHUGEPAGE`, set once at window creation. Small processes never take a 2 MiB first-touch fault, and khugepaged never collapses sparse records or the gmap (both red teams).

| System state | Action |
|---|---|
| `never`, or THP disabled by prctl | No madvise; hugeblocks are packing and release units only; never collapse. `MADV_COLLAPSE` ignores sysfs, and user intent wins [madvise] |
| `madvise` with defrag `madvise`, `always` or `defer+madvise` (defrag's default is `madvise`) | No `MADV_HUGEPAGE`: first-touch faults would direct-reclaim, as Go saw [go-61718]. `MADV_COLLAPSE` is **off**: `collapse_huge_page` takes `mmap_write_lock` [k-khp], and Go removed collapse after stalls [go-collapse]. Opt-in `thp=collapse`: background thread only, hugeblocks ≥ 95% used for ≥ 10 s (jemalloc HPA defaults [je-hpa]), ≤ 4/s, 60 s back-off after any collapse > 10 ms, counted in K2. Experimental `thp=dense` (E9, F43): a separate dense data area inside the window gets `MADV_HUGEPAGE` once (+2 VMAs) and receives only dense, long-lived small-page hugeblocks |
| `madvise` with defrag `defer` or `never` | `MADV_HUGEPAGE` once on [bootstrap end, window end), covering whole VMAs, at first growth |
| `always` (including the 6.18 `PR_THP_DISABLE_EXCEPT_ADVISED` handling [k-thp]) | Nothing per range beyond the fixed NOHUGEPAGE areas |

**Per-size THP (mTHP)** (red team 1):

- F (§5.1) is the first-touch folio size for the data area and sets the provisioning granule. M1's budget is computed with it.
- On 16K/64K arm64 kernels the 2 MiB policy is keyed on `hugepages-2048kB/enabled`, not the top-level knob, because H is not the PMD size there [k-arm64hp].
- Partial DONTNEED inside a large folio splits it, so subrelease below F is avoided except under pressure.
- CI covers mTHP {16K, 64K, 2M} × {always, madvise, never}.

**Interactions with the kernel:**

- With `max_ptes_none > 255` (default 511 [k-khp], verified by red team 2), any subrelease inside a THP-eligible block is treated as temporary: whole-hugeblock release is preferred, and subreleased blocks are ranked first for refill.
- Partial DONTNEED inside a PMD THP only queues a deferred split [k-thp], so subrelease is reserved for pressure (§5.9).
- S0 never zeroes on free and S1 zeroes only blocks ≤ 1 KiB, so retained memory should not look "underused" to the 6.12 shrinker [k-thp]. This is an INFERENCE.
- khugepaged collapses of our data VMAs take `mmap_write_lock` [k-khp]. They are counted (via the tracepoint's mm, and system-wide `khugepaged/pages_collapsed`) and excluded from K2, which is gated only on THP=madvise/never.
- Coverage is measured on demand from `smaps_rollup` `AnonHugePages`.

#### 5.8.4 Kernel feature matrix

| Feature | Minimum kernel | Use | Fallback |
|---|---|---|---|
| `MAP_FIXED_NOREPLACE` | 4.17 [mmap] | Aligned window placement | Check the returned address; over-map and trim when RLIMIT_AS is unlimited |
| `PR_SET_VMA_ANON_NAME` | 5.17 | Naming | Skip |
| `MADV_POPULATE_WRITE` | 5.14 [madvise] | Latency-profile prefault (P4) | Touch pages |
| `MADV_DONTNEED_LOCKED` | 5.18 [k-dnl] | Locked heaps under pressure | "Not released" |
| `process_madvise` with `PIDFD_SELF` | 6.15 [k-pidfd][pmadvise] | Batched release | `madvise` loop |
| `MADV_GUARD_INSTALL` | 6.13 [madvise] | GWP pool; S1 solo guards when not mlocked | GWP pool with `PROT_NONE` guards only if `max_map_count` headroom > 10k; else no GWP |
| `MADV_COLLAPSE` | 6.1 | Opt-in only | — |
| `mseal` | 6.10 [k-mseal] | Seal the read-only config and secret page only, never heap | Skip |
| membarrier `PRIVATE_EXPEDITED` (+ registration) | 4.14 [membarrier] | TLS-mode lease | No lease |
| membarrier `EXPEDITED_RSEQ` | 5.10 [membarrier] | cid-mode lease (P4) | TLS mode |
| rseq `mm_cid` / `node_id` | 6.3 [k-rseqh] (needs glibc ≥ 2.41 to be visible through `__rseq_size`) | P4 cid mode; `cpu_id` for NUCA | TLS mode; `sched_getcpu` |
| Per-size mTHP controls | 6.8 (red team 1) | F computation | Top-level `enabled` only |
| Unprivileged PSI triggers | 6.4, 2 s window multiples [k-psi] | Opt-in `psi=1` (v1.x) | cgroup polling |
| rseq time-slice extension | 7.0 (V2 registration) [k-rseqdoc] | E13 (P4) | None |

### 5.9 Memory release policy (formulas)

**Per page-heap shard:**

| Symbol | Meaning |
|---|---|
| U | bytes in pages or runs handed to slots or users |
| F | free, committed, dirty bytes in the filler |
| D_s | max U over the trailing 10 s, from a ring of 10 one-second maxima |
| D_l | max U over 5 min, from a ring of 30 ten-second maxima |

**Cgroup pressure levels** (rewritten after both red teams):

- **Why the rewrite.** `memory.current` counts page cache as well as anonymous memory [k-cg2]. Driving the levels from raw headroom pinned I/O-heavy containers in p2.
- **Discovery.** From `/proc/self/cgroup`, find up to 4 ancestors (up to the cgroupns root) with finite `memory.high` or `memory.max`. Each ancestor j is compared with *its own* `memory.current`.
- **Per ancestor j:**
  - `L_j = min(memory.high_j, memory.max_j)`.
  - `C_j = memory.current_j`, re-opened on every read, at most every 100 ms.
  - `Fe_j` = `file` bytes from `memory.stat_j`. It is read at most every 10 s, and only while the raw headroom (L_j − C_j)/L_j < 25%. That rate keeps rstat flush cost negligible [lwn-rstat].
  - `h_j = (L_j − (C_j − Fe_j)) / L_j`: estimated anon headroom.
  - `E_j` = increase in `memory.events_j` `high + max + oom` since the last read. These are hierarchical and signal real pressure [k-cg2].
- **Combined:** `h = min_j h_j`. If no limit is found, or cgroup v1 is in use, the level stays p0.

```
Level transitions (hysteresis; minimum dwell 2 s, PARAM):
  p0 -> p1  when h < 10%            p1 -> p0  when h > 20%
  p1 -> p2  when h < 5% or any E_j > 0
  p0 -> p2  when any E_j > 0
  p2 -> p1  when h > 12% and no E_j increase for 10 s
```

Co-tenant page-cache streaming no longer moves the level unless it causes real `high`/`max` events, which genuinely affect this process too. M5 includes this case.

**Retention target:**

```
R_cap = max(2 MiB, U/8)                                        PARAM
p0:   R = min(max(D_s - U, 0), R_cap)
p1:   R = min(max(D_s - U, 0), 2 MiB)
p2:   R = 0
idle (no page-heap activity for 30 s): R = 0
X = F - R ; release iff X >= one hugeblock, or level >= p1
```

**Release order:**

1. Fully free hugeblocks, oldest last-used epoch first.
2. Free slices inside partly used hugeblocks, only if THP does not back the block, or level ≥ p1, or the block stayed < 25% used across the whole D_l window. This is skip-subrelease in spirit [ismm21][tc-sub].
3. Run tails.

**Front-end bound** (Hoard-style per slot, with hysteresis; red team 2):

```
S_act    = EWMA over 10 s of slots with an event or slow path in the last second
cap_slot = clamp(U / (16 * max(1, S_act)), 512 KiB, 8 MiB)
B_s      = min(cap_slot, max(K * S, f * A_s))   K = 2 pages, f = 1/4; A_s = live bytes in slot-owned pages [hoard]
at each sweep and at thread exit: free_est = sum over referenced pages of free bytes + run-cache bytes
  if free_est > B_s: donate partial/empty pages, least-recently-refilled class first, until free_est <= B_s/2
  a donated page records (donor slot, epoch); the donor skips it for 100 ms (PARAM) on refill
```

**Structural per-slot bound S_struct** [derived] (red team 1 showed v2's 128 KiB-per-slot figure ignored most referenced memory):

| Component | Bound |
|---|---|
| cur + alt for 44 small classes × 64 KiB | 5.5 MiB |
| cur for 16 medium classes ≤ 512 KiB, plus 640 KiB and 896 KiB | ≤ 9.5 MiB |
| Run cache | ≤ 1 MiB |
| Empty spares | ≤ 2 × 64 KiB |
| rbuf: 64 blocks pinning ≤ 64 small pages | ≤ 4 MiB |
| **Total** | **≈ 20 MiB** |

- An *active* slot holds ≤ B_s ≤ 8 MiB of free memory after each sweep.
- An idle slot with leases enabled keeps < 64 KiB after the next lease batch (candidate threshold).
- An idle slot without leases keeps up to S_struct. That is about 20 GiB in the pathological worst case of 1024 idle slots that each touched every class. A typical thread touching 10 small classes keeps ≈ 1.3 MiB plus up to 4 MiB of rbuf pinning [derived].
- M2 and L7 are gated against these corrected numbers.

```
FE  <= sum_active B_s + sum_idle_leased 64 KiB + sum_idle_unleased S_struct
RSS <= live + FE + pool waste (< 7/8 of each queued page; fully free pages returned at once) + R + metadata (~0.3%)
```

That is the informal blowup bound [derived]. Under p1 and p2: spare pages are dropped, B_s is halved, every rbuf is flushed at the next event, and the idle-slot threshold drops to 100 ms.

**Who runs the policy:**

1. **Inline trigger, in any process.** In the free and malloc slow paths, if X > max(32 MiB, U/4) (PARAM), release synchronously and outside locks, at most once per 100 ms.
   - Each call releases at most 16 MiB (PARAM) and continues on later slow paths, which bounds the stall (T5 ≤ 1 ms, red team 2).
   - It also flushes the caller's rbuf.
   - This covers single-threaded "spike, then idle" programs. Pages detached when full become orphans, so same-thread frees to them go through the remote path. When they become fully free they return to the filler and raise F. Red team 1's claim that such pages are never detected was partly wrong; its fix (free-driven ticks and the local emptiness check) is adopted anyway for cur/alt pages.
2. **Background thread** (`sohamalloc-bg`, named with `PR_SET_NAME`).
   - **Creation.** Wanted once the process has ≥ 2 live slots and F > 16 MiB for > 1 s. It is created from the next countdown/tick event **before `CS_BEGIN`**, with `depth == 0` and `busy == 0`, under a recursion guard. `pthread_create`'s own allocations therefore take normal paths (red team 1).
   - **Residual risk.** glibc internal locks held by a caller that malloc'd (for example inside `dlopen`) might interact with `pthread_create`; this is UNVERIFIED, tested with dlopen-heavy programs, and `bg=0` disables the thread.
   - **Signals.** It blocks signals with `pthread_sigmask`, which on glibc never blocks SIGCANCEL or SIGSETXID [glibc-sigmask][glibc-intsig]. A raw `rt_sigprocmask` would block SIGSETXID and hang `setuid()` in multithreaded processes, because glibc signals every thread and waits for each [glibc-setxid] (red team 1). Whether musl's `pthread_sigmask` protects its internal signals the same way is UNVERIFIED and tested.
   - **Behaviour.** It ticks every 1 s while X > 0 or leases are pending, and otherwise futex-waits; slow paths wake it when F crosses a threshold. An idle process sees no wakeups. Each tick polls the cgroup files with open/read/close; no fds are cached.
   - **Exit.** It exits when live application slots stay at 1 for > 1 s. The process is then single-threaded again, so `unshare(CLONE_NEWUSER)` and `setns` work [unshare] (both red teams). `soham_bg_stop()` stops it synchronously for programs that unshare sooner. Test: spawn 8 threads, join them, call `soham_bg_stop()` (or wait 1.5 s), then `unshare(CLONE_NEWUSER)` must succeed.
3. **No thread.** `memory.current` is sampled at most every 100 ms, checked with a vDSO `CLOCK_MONOTONIC_COARSE` read every 64 slow paths.
4. **Explicit:** `soham_release(level)`; `malloc_trim` maps onto it.

**Rate limit:** release syscall time ≤ 1% of wall time per second, following Go's scavenger budget [go-scav]; lifted under p2.

**MADV_FREE** (P4 `throughput` profile only): only while h ≥ 25%, only for blocks free for < 2 s, drained to DONTNEED at p1, reported as `LazyFree` [k-proc].

**Soft budget** (`SOHAMALLOC_BUDGET`, default off; `budget=auto` = 0.95·L): above it, release synchronously; if still over, run the `new_handler` path or return NULL *before* the kernel OOM-kills the process.

### 5.10 Thread lifecycle, fork, signals and early init

**Early init** (no constructor dependency):

- All globals are zero-valid: `gmap_n` is 0, TLS countdown and tick are 0, the sentinel record has `free = 0`.
- The first malloc goes to the event path, which runs `init()` under an atomic once-state (0 / in progress / done) with a recursion guard.
- No `fopen` or `opendir`, which allocate [glibc-replace]. No `backtrace()`, which dlopens libgcc [unwind-link].
- Nested allocations during init (for example `pthread_atfork` growing its array) are served from a 128 KiB `.bss` **bootstrap arena**. Freeing those pointers is a counted no-op; exhaustion aborts with a diagnostic.

**Init syscall budget (K3):**

| Step | Syscalls [derived] |
|---|---|
| `getauxval` (PAGESZ, SECURE, HWCAP2, RANDOM) | 0 |
| `getrandom(GRND_NONBLOCK)`; fallback `AT_RANDOM` mixed with TSC or `cntvct` | 1 |
| Parse `SOHAMALLOC` from `environ`, honouring `AT_SECURE` | 0 |
| `prlimit(RLIMIT_AS)` | 1 |
| Window `mmap` at an aligned hint (+1 per EEXIST retry) | 1 |
| Commit the metadata prefix (`mprotect`) | 1 |
| `MADV_NOHUGEPAGE` over metadata, gap and bootstrap hugeblock | 1 |
| Commit the bootstrap hugeblock (`mprotect`) | 1 |
| `pthread_atfork`, `pthread_key_create` | 0 |
| **Total** | **6** |

Deferred until needed:

- to the first growth beyond the bootstrap block: overcommit mode, THP and mTHP sysfs, `PR_GET_THP_DISABLE`, naming;
- to U > 4 MiB: cgroup and topology discovery;
- to U > 8 MiB: the GWP pool and its handler;
- to the first lease batch: membarrier registration;
- to ≥ 2 slots: the background thread.

The 6 is a derivation, not a measurement. K3 is re-baselined from an `strace` of the P0 prototype (E1), as both red teams asked.

**M1 page budget** (4K, THP config (b)) [derived/est]:

| Item | Size |
|---|---|
| 10 data base pages | 40 KiB |
| Window header and first hugeblock meta | 8 KiB |
| Slice records for the 10 slices | 4 KiB |
| gmap page | 4 KiB |
| Slot page | 4 KiB |
| sohamalloc's writable `.data`/`.bss`/GOT after relocation (UNVERIFIED until measured) | ~8 KiB |
| **Total** | **≈ 68 KiB** |

Against glibc's 1–2 brk pages for the same 10 allocations, that is about +56–64 KiB. Hence the gate is +96 KiB and the goal +48 KiB. The goal needs the nursery page (F42, E23), which is not adopted by default.

**Thread attach** (first event of a new thread):

1. Pop the warmest slot from the pool (most recently released), or create one if fewer than H_max live slots exist; otherwise use tls-shared.
2. Allocate the thread's spare rbuf from the metadata arena.
3. Set `tls.slot` and `tls.slot_tag` **before** `pthread_setspecific`, which may allocate [glibc-replace]. Whether a key index below 32 avoids that allocation is UNVERIFIED.

**Thread detach** (TSD destructor):

1. Flush the rbuf.
2. Process the deferred-free list.
3. Donate partial pages above B_s, with K = 0.
4. Push the slot, with its pages, onto the pool, and return the spare rbuf.
5. Set `tls.slot_tag = 0`, `tls.slot = NULL`, `state = ZOMBIE`, `countdown = 0`.

Later frees, for example C++ `thread_local` destructors, take the `tag == 0` path: `unattached_free` → `remote_direct`, or the deferred list if `depth > 0`. This is the class of late-free bug jemalloc 5.4.0 fixed [je-changelog]. Later mallocs use a shared slot.

Pooled slots older than 2 s are drained by the next tick or slow path: pages go to the class pools, empty pages retire. Thread churn becomes slot reuse, the fix for mimalloc #351 [mi-351].

**PreventFork (fixed order; red team 1):**

- **What is a PF section.** Every allocator slow path that takes an allocator lock or performs a multi-step lock-free sequence: refill collection, claim, detach, flush processing, the remote CAS with `resolve`, and release batches.
- **Entry:** `pf_count++` (seq_cst); `if (g_forking) { pf_count--; wait until !g_forking; retry }`. Slow paths can afford the full fence.
- **Counters:** threads with a slot use `slot->pf_count`; others use a global counter.
- **Rule:** a PF section is always outermost. No thread waits at PF entry while holding an allocator lock.

**Fork:**

- **prepare:**
  1. Flush the caller's rbuf and deferred list.
  2. Set `g_forking`.
  3. Wait, without a bound and with **no allocator lock held**, until every PF counter except the caller's is zero. This is snmalloc's order [sn-fork].
  4. Take locks in a fixed order: slot pool, class pools, filler shard, solo table, stats.
- If the caller's own counter is non-zero (fork from a signal handler that interrupted an allocator slow path), the child marks the heap "forked-in-handler" and serves only from the emergency arena until `exec`, documented as unsupported.
- A thread stopped (SIGSTOP) inside a PF section delays fork until it resumes. This is a residual risk shared with glibc's arena locks.
- **child:**
  1. Re-initialise every lock and clear `g_forking`.
  2. Reset `busy`, `lease` and `pf_count` on **every** slot.
  3. Move every slot other than the caller's to the pool as-is. This is O(#slots) and touches no page metadata, which matters for copy-on-write servers.
  4. New pages get keys from a re-seeded RNG.
  5. Mark the background thread absent (restarted lazily). No fds are inherited or cached.
- **parent:** release the locks, clear `g_forking`, wake waiters.
- **Tests:** fork from 64 threads in the middle of frees; fork during a lease; fork during flush and during `resolve`. The child must not leak or deadlock.

**Signals and reentrancy.** POSIX does not require malloc to be async-signal-safe [sigsafety]. sohamalloc guarantees no corruption and no deadlock for handlers that call malloc or free.

- A handler that interrupts a CS or slow path sees `s->busy == 1`. One that interrupts slotless allocator code sees `tls.depth > 0`.
  - **malloc** → the **emergency bump arena** (256 KiB `.bss`, atomic add). Exhaustion returns NULL with ENOMEM and logs once with raw `write`.
  - **free** of *any* pointer kind (arena, solo, GWP, emergency) → pushed onto the thread's **deferred-free list**, `tls.deferred`, using the block's first word.
- **The deferred list.** It is thread-local. The handler pushes with plain stores ordered by signal fences. The owner detaches the whole list with one TLS store before processing, so a handler interrupting processing pushes onto a fresh empty head. The list is processed at the next slow path, tick event or thread exit.
- So no handler path ever performs `resolve`, takes a pool, shard or solo-table lock, or touches another thread's state. That fixes both red teams' finding that `remote_direct` could take locks held by the interrupted code.
- Emergency frees are counted no-ops, and the bytes are reported.
- **cid mode (P4):** delivery aborts an interrupted rseq CS [k-rseqdoc]; slow paths use the same `busy`/`depth` routing.
- The explain report never runs in a handler. `report_signal=USR2` (opt-in) only sets a flag.

### 5.11 NUMA and NUCA (designed; P4)

- **Topology** is read lazily from `cpu*/cache/index*/shared_cpu_list` and `cpu_capacity` on asymmetric systems [k-sched-cap]. Any failure means one domain.
- Slots are not CPU-bound, so locality is chosen at **refill**: the LLC pool by current CPU, the shard by node. The current CPU comes from `rseq->cpu_id` when glibc registered rseq (read-only), else `sched_getcpu()`. On arm64 without glibc rseq this may cost a syscall; that there is no arm64 vDSO getcpu is UNVERIFIED.
- **NUCA:** ASPLOS'24's NUCA-aware caches gave +0.32% fleet throughput and −4.37% LLC load misses [asplos24].
- **NUMA:**
  - Per-node shards exist only when more than one node has memory.
  - Each node gets its own window (its own VMAs). `mbind(MPOL_PREFERRED)` is applied only to a whole window, and only when `get_mempolicy` reports the default policy; the process mempolicy, including weighted interleave, otherwise wins [k-numa].
  - Frees are origin-aware by construction: blocks return to their page, and orphans go to that page's node pool [numalloc].
  - Threads are never bound.
  - Google saw no material gain from per-NUMA vCPU ids [lkml-flatvcpu].
- **Tiering:** cold-hinted allocations (opt-in partition) keep whole pages cold, so kernel demotion moves whole pages [obase].

### 5.12 Security tiers and costs

**One page format for every tier** (Judge 1). S1 does not switch pages to a bitmap allocator, which would mean maintaining two allocators. C's out-of-line bitmap format comes back only if E11 shows its locality loss is under 2% on application benchmarks. Temeraire and ASPLOS'24 attribute their gains to placement, which makes that unlikely [osdi21][asplos24].

| Tier | Contents | Cost target / evidence |
|---|---|---|
| **S0** (default) | Encoded link `bswap(next ^ page_key ^ (addr>>12))` (§5.5.2): bswap makes partial overwrites non-canonical [pa-enc]; the address term binds the link to its slot, as safe-linking does [glibc-malloc]; the per-page key limits one leak to one page. Range checks of head and next against `pbase/psize` on pop. head ≠ block and 16 B alignment on push. Exact reciprocal boundary check on every remote-freed block at collection, and exact-count chain walks. Lazy sized-delete and class checks at collection. Foreign-pointer policy (§5.2). Records in a separate area behind a `PROT_NONE` gap, so linear heap overflows and underflows cannot reach metadata. GWP-ASan pool. Sealed config page | ≤ 2% (E11). Safe-linking was "not measurable" in glibc [cp-safelink]; snmalloc's checks cost < 5% [sn-sec] |
| **S1** (`secure`) | S0 plus: exact boundary check on **every** free (multiply by `recip`) [sn-sct]; shuffled 8-block provisioning windows; zero-on-free for blocks ≤ 1 KiB with a write-after-free check on reuse [xzone][hm-readme]; byte-budgeted FIFO quarantine of min(1% of U, 4 MiB), best-effort; 2–4 call-site buckets for classes ≤ 1 KiB, chosen by `hash(ret_addr ^ secret)` [xzone]; per-class VA sequestering; window header magic validated on every slow path; guarded solos when not mlocked; foreign pointers abort; `dealloc_type_mismatch`-style checks [scudo-flags] | ≤ 10%. Analogues: mimalloc secure ~10% [mi-readme], snmalloc < 5% [sn-sec]. Uncapped zero-on-free would cost +8.38% wall on a kernel build [k-initfree] |
| **S2** (MTE, P4+) | `PROT_MTE` at window creation. Tag ≤ 32 KiB, guards above. Exclude neighbours' and previous tags; tag 0 = free. Retag on free ≤ 4 KiB and on allocation 4–32 KiB. Retag after purge, because DONTNEED may clear tags [k-mte][xzone] | 1.08–1.18× async on Pixel 8; 1.56× sync on Cortex-X3; outlier 6.64× [mte-perf]. Treated as probabilistic [tiktag] |

In S0, VA *is* reused across classes; per-class sequestering is S1-only, because it costs RSS and packing.

**S0 threat model, stated honestly** (red team 2):

- The records are out of band but at addresses computable from any heap pointer. An attacker with an *arbitrary* write can forge a free head; the S0 head range check confines the forged head to the same page, but cannot do more.
- S0 provisions in address order, so a fresh block's next is `b + bsize`. One leaked encoded link plus knowledge of `b` recovers that page's key. S1's shuffling only raises this to roughly a 1-in-7 guess within an 8-block window [derived].
- So S0 stops blind partial overwrites and cross-page link redirection, and limits a key leak to one page. It does not withstand an attacker who has a read primitive and an address leak. E22 measures each claim.

**GWP-ASan pool:**

- 32 slots (PARAM) of ≤ B each, in a NOHUGEPAGE mapping created lazily (U > 8 MiB), with guards installed once via `MADV_GUARD_INSTALL` [madvise].
- If `mlockall` is active (`VmLck` > 0) or install returns EINVAL, the pool is created with `PROT_NONE` guards only if `max_map_count` headroom allows; otherwise GWP is disabled and logged once.
- Locking guard regions fails [madvise]. Whether a later `mlockall(MCL_CURRENT)` by the application fails because of our guards is UNVERIFIED. CI tests it, and if it fails, the default switches to `PROT_NONE` guards.
- **Placement** rides the allocation-byte countdown: the allocation that crosses the 2 MiB-mean countdown goes to the pool if it is ≤ B. Per-allocation probability is size/2 MiB, i.e. 1 in 32768 for a 64 B object [derived]. That is sparser than LLVM's 1/5000 [gwp-llvm], an honest trade for zero fast-path cost. `secure` lowers the mean to 256 KiB; E18 measures detection against cost.
- **Handler** (fixes red team 2's contradiction):
  - At pool creation sohamalloc installs its SIGSEGV handler and saves the previous disposition. Faults outside the pool are chained to it (re-raised with the default action if it was SIG_DFL).
  - If a later installer, such as a JVM, replaces our handler, GWP reports are lost but nothing breaks.
  - The explain report shows `GWP: handler installed | chained | displaced | disabled`.

**What S0 does not stop:**

- use-after-free after the slot is reused;
- a double free after the block was reallocated;
- a *local-path* interior-pointer free (caught in S1, or in S0 if E11 promotes the boundary check at ≤ 1% cost);
- racing cross-thread double frees (best-effort);
- in-object overflow;
- cross-type confusion after page reuse;
- an attacker with arbitrary read and write.

### 5.13 Observability and API

**Explain report** (`soham_explain(fd)`, `SOHAMALLOC=report=exit`, or the stats API):

- Allocation-free, built in a static buffer, written with raw `write`.
- **Allocator view:** requested, rounding waste, tail waste, slot-owned free, rbuf-buffered and rbuf-pinned pages, pooled, orphaned, retained R, released-but-committed, window VA (VSZ), committed prefix, metadata, emergency, bootstrap and deferred bytes.
- **Kernel view**, read on demand from `/proc/self/smaps_rollup`: Rss, AnonHugePages, LazyFree [k-proc]. The difference is labelled "not attributable". No sum is invented.
- **Modes and reasons**, for example: `front-end: tls-exclusive 37/96 slots, shared 0 (cid mode: off, reason=P4 feature); THP: madvise/defrag=madvise, mTHP F=4K, MADV_HUGEPAGE=no; release: process_madvise+PIDFD_SELF; overcommit=0; window=64GiB VA, 1.2GiB committed; leases: 12 (membarrier ok), 0 on busy slots; cgroup level p0 (h=41%); bg-thread: idle; GWP: chained`.

**Other observability:**

- `soham_stats_json` with a versioned schema. Counters are updated only on slow paths and summed on read.
- USDT probes on refill, release, reclaim and collapse.
- Valgrind `MALLOCLIKE`/`FREELIKE` client requests in a `-DSOHAM_VALGRIND` build [valgrind].
- A no-override build for ASan, TSan and MSan.
- Heap profiler (P4): the countdown sample records a stack via a bounds-checked frame-pointer walk (≤ 32 frames, within thread stack bounds), stored in an internal arena. It needs `-fno-omit-frame-pointer` for accuracy.

**API compatibility (glibc semantics):**

| Entry point | Behaviour |
|---|---|
| All sizes | Overflow builtins; size > `PTRDIFF_MAX` → NULL with ENOMEM |
| `malloc(0)` | Unique 16 B block; safe as a thread's first call |
| `realloc(p, 0)` | Free p, return NULL; failed realloc keeps the old block |
| `free` | Preserves errno; NULL is a no-op through the slow path |
| `free_sized`, `free_aligned_sized` | Size validated (lazily in S0, always in S1); NULL accepted |
| `memalign` | Exported; non-power-of-two alignment rounded up as glibc does [glibc-malloc]; listed among required replacements [glibc-replace] |
| `cfree` | Exported as `free` where the libc still declares it |
| `posix_memalign`, `aligned_alloc`, `valloc`, `pvalloc` | §5.7.4; `valloc`/`pvalloc` use runtime B |
| `malloc_usable_size` | Class or run size; writing into the slack is legal for us |
| `reallocarray` | **Not exported** on glibc: its own version calls our `realloc`, and replacing it is unsupported [glibc-replace] |
| `mallopt` | Accept, return 1, log once |
| `mallinfo` / `mallinfo2` | Filled from stats |
| `malloc_trim` | Calls `soham_release` |
| `malloc_stats`, `malloc_info` | Text and XML via `write` |
| `__libc_*` aliases | Not provided; required only before glibc 2.25 [glibc-replace], below the tested range (2.35+) |

A CI symbol-completeness check compares exported symbols against the glibc manual's list [glibc-replace].

**C++:** a separate C++17 translation unit, `libsohamalloc_cxx`:

- All replaceable `new`/`delete` forms: the throwing `new` that loops on `get_new_handler()` and then throws `bad_alloc` [cxx-new], nothrow, `align_val_t` and sized forms.
- `__size_returning_new` and the `__hot_cold_t` overloads, ABI-compatible with TCMalloc [tc-ext] and the MemProf rewrite targets [memprof] (P4). Hints are counted; the cold partition is opt-in.
- `new`/`delete` have no observable side effects, so they stay valid under GCC's sane-operators assumption.

**Extensions:**

- `soham_release(level)`, `soham_thread_idle()` (flushes the rbuf and the deferred list, and donates down to K·S), `soham_bg_stop()`, `soham_stats_json`, `soham_explain`, `soham_malloc_sized`.
- P4: `__alloc_token_*` entry points (opt-in pointer/data split) [alloctoken]; an official Rust `GlobalAlloc` crate (every Rust free is sized, and the allocator must not unwind [rust-ga]); first-class heaps with a visitor and a reuse-delay hook, which CPython free-threading needs [pep703].

### 5.14 Configuration and profiles

`SOHAMALLOC=<profile>[,key=value...]`. Unknown keys are reported once and are never fatal.

| Profile | Effect | Release |
|---|---|---|
| `default` | Everything in this document | v1.0 |
| `lowmem` | R_cap = 2 MiB, idle timeout 5 s, K = 1 page, rbuf 16 entries, run cache 256 KiB | v1.0 |
| `secure` | S1, GWP mean 256 KiB | P4 |
| `debug` | Junk fill, full checks, at-exit leak report from the sampler | v1.0 |
| `throughput` | `MADV_FREE` warm stage, R_cap = U/4, `MADV_HUGEPAGE` even under `defrag=madvise` | P4 |
| `latency` | Background `MADV_POPULATE_WRITE` prefault of the next hugeblock; no inline release; `thp=collapse` allowed | P4 |
| `deterministic` | Fixed seed, decay counted in allocations, no thread, TLS mode, fixed window hint | P4 |

Keys: `bg=0|1`, `thp=collapse|dense`, `budget`, `sample`, `seed`, `report`, `forward_foreign`, `remote_batch`, `lease=auto|always|off`, `window=<size>`, and (P4) `psi`, `mode=tls|cid`.

Under `getauxval(AT_SECURE)`, only options that strengthen security are honoured, and paths and fds are never taken from the environment [getenv].

### 5.15 Portability

**Primitive layer:** `reserve_window`, `commit_prefix`, `decommit_top`, `release(iov)`, `guard`, `name`, `geometry`, `thp_state`, `limits`, `slot_cs_begin/commit` (TLS stub or rseq asm), `asym_fence`, `now_coarse`, `futex_wait/wake` (on 32-bit-aligned words only), raw file reads.

| Target | Plan |
|---|---|
| arm64 16K and 64K pages | S = max(64 KiB, B) and H = 2 MiB (contiguous PTE) [k-arm64hp]; 2 MiB policy keyed on `hugepages-2048kB/enabled`. On 64K kernels a small page is one base page and provisioning happens at 64 KiB granularity; the M1 target becomes ≤ glibc + 1.5 MiB (PARAM). The window's last-page and gap guards are one base page (64 KiB on 64K kernels). CI runs QEMU 16K and 64K kernels; lease ordering is stress-tested on real Neoverse hardware |
| musl | TLS mode (no rseq dependency). Export `aligned_alloc`, or musl's returns ENOMEM [musl-glue]. Ship a static recipe that weakens `libc.a` malloc objects with `objcopy` [tweag] |
| glibc static | Link before `libc.a` with the full symbol set; CI checks that `DT_NEEDED` survives `--as-needed` [ruby-18409] |
| macOS, Windows | Later: zone registration and a redirect DLL (mimalloc's approach [mi-readme]). Windows reserve/commit maps directly onto the window/prefix model |
| 32-bit, wasm | Non-goals |

---

## 6. Feature catalogue

**Decision rule:** "Keep/kill" names the measurement that removes or demotes the feature. Experiments are listed in §9. "Rel." is v1.0 or P4.

| Feature | Rel. | Rationale | Cost | Risk | Rejected alternatives | Experiment | Keep/kill criterion |
|---|---|---|---|---|---|---|---|
| F1 TLS-exclusive slots + warm pool | v1.0 | No rseq ABI dependency; reuses warm slots after churn [mi-351] | ~6 instr for reentrancy, `busy` and acquire `lease` [est] | Memory scales with threads up to H_max | Pure per-thread heaps; cid-first (ABI churn) | E7, T1 | Kill pool reuse if M2 is not ≤ 0.7× mimalloc |
| F2 Non-blocking Dekker lease | v1.0 | Reclaims sleeping threads' memory, which PartitionAlloc cannot [pa-tc]; the owner never waits | ≤ 1 membarrier/s; IPIs | seccomp; nohz_full | No reclaim; futex-waiting owner (v2) | E16 | Default off if IPI cost > 0.1% CPU on the 256-CPU box, or any lease on a busy slot |
| F3 rseq-cid mode | P4 | Memory follows *running* threads [k-cid] | +3 instr; per-arch asm | 6.19 and 7.1 churn | Self-registration; cpu_id slots | E14 | Enabled per kernel only if T1 beats TLS mode by ≥ 2% and the remote-free rate rises by < 5 points |
| F4 Page-only transfer | v1.0 | Coherent flow; empty pages found at once | One spare page per class per slot | Same-thread frees to orphaned pages go remote | Transfer caches [tc-design] | E4, E21 | Switch to owned-full-pages (E21) if it wins T1 by ≥ 2% without breaking M2 |
| F5 Packed count+head | v1.0 | Single commit; exact emptiness | 48-bit pointer limit | 57-bit VA future | Two counters | Unit tests, model checks | Revisit if 57-bit heaps are needed |
| F6 Three-line records (O/R/X) | v1.0 | No owner-line invalidation by remote frees | 192 B per slice VA (0.29%) | — | Two lines (v2); shared line (B) | E6 `perf c2c` | Keep unless HITM on line O shows no difference |
| F7 Batched remote frees | v1.0 | One CAS per page [batchit] | ≤ 64 KiB delay, ≤ 64 pinned pages per slot | 6–8% regressions [batchit] | Per-object CAS; MPSC queues [sn-queue] | E6 | Default on only under the §5.5.5 rule |
| F8 ORPHAN/QUEUED/PENDING in `xfree` | v1.0 | Single responsible thread; re-check under lock; no lost transitions | — | Subtle protocol | FULL flag + pending list (B); decide-then-move (v2) | GenMC with list ops | — |
| F9 gmap + window arithmetic | v1.0 | 1 load ∥ 1 load; pre-init safe [sn-pagemap] | 4 MiB VA in metadata | — | Radix tree; flat slice map [mi-pagemap]; `.bss` gmap (THP) | E3 | Kill if > 1 cycle median slower than mimalloc's lookup on free-heavy microbenchmarks |
| F10 64-class table + generator | v1.0 | Dense where 98% of objects are [asplos24] | More partial pages | Fit for some programs | 52 classes; fleet table | E2 | Switch to 52 if partial-page RSS outweighs rounding savings on the trace set |
| F11 Lazy provisioning (max(B, F)) | v1.0 | M1 [pa-enc] | Slow-path work | mTHP | Whole-page threading | E1 | — |
| F12 Pool order: fullest with ≥ 1/8 free | v1.0 | Occupancy plus refill yield | Bucket upkeep | Locality | Fullest-first; FIFO | E4 | Pick the best of 4 orders by LLC misses, dTLB misses and RSS |
| F13 Temeraire filler + density split by kind | v1.0 | +1.02% / −0.82% fleet [asplos24]; order from [tc-hpf] | Shard lock | Contention | First fit; v1 densest-first | E9, lock-wait | — |
| F14 Slice-multiple medium pages; run classes 192/256 KiB + run cache | v1.0 | Zero tail waste for 160–256 KiB; no 2-object pages | ≤ 1 MiB run cache per slot | 640/896 KiB pages per slot | 2.5/3.5-slice "runs" (v2, wrong) | E2 | Kill the cache if hit rate < 50% |
| F15 Solos without NORESERVE + `mremap` only there | v1.0 | glibc failure semantics; no VMA splitting [lwn-mremap] | 1 VMA per solo | `max_map_count` | In-window `mremap` | E15 | — |
| F16 Heap window + uniform prefix commit | v1.0 | Survives modes 0/1/2, RLIMIT_AS, `mlockall`; independent of top-down placement | Large VSZ | Monitoring that alerts on VSZ | Upward-growing reservations (v2); fixed 64 GiB RW (v1) | E11b, E15 | — |
| F17 `process_madvise(PIDFD_SELF)` batching | v1.0 | Fewer syscalls [k-madvise] | — | Older kernels gain nothing | Per-batch pidfd (v2); per-range madvise | E5 | — |
| F18 Demand window + pressure levels with hysteresis + budgeted inline trigger | v1.0 | Honest RSS in containers [k-cg2] | Refaults | Wrong windows churn | Raw headroom levels (v2); fixed decay; application-driven [tc-ext] | E8 | Tune windows until refaults < 2% of allocations; M5 co-tenant variant must pass |
| F19 Lazy futex-idle thread that exits | v1.0 | Idle release [je-1398] without breaking `unshare` [unshare] | One thread while multithreaded | seccomp clone; glibc lock interactions | Always-on; none | E8 | — |
| F20 THP matrix + NOHUGEPAGE metadata/bootstrap + mTHP | v1.0 | No stalls [go-61718]; small processes lean | Lower coverage | dTLB loss | Blanket `MADV_HUGEPAGE`; 2 MiB purge minimum [mi-1282] | E9 | — |
| F21 `MADV_COLLAPSE` (opt-in) | v1.0 | Coverage under `defrag=madvise` | `mmap_write_lock` [k-khp] | Stalls [go-collapse] | On by default | E9 | Default on only if p99 fault latency rises < 5% and K2 holds outside the collapse budget |
| F22 Prefault (latency profile) | P4 | Minor faults can dominate short programs [valinor] (not peer-reviewed) | RSS inflation | — | Always | E1, E8 | — |
| F23 Allocation countdown + free tick | v1.0 | Periodic jobs on two branches [je-inl]; unbiased sampling stays allocation-only | 2 TLS RMWs | Bias | Single mixed countdown | Profile | — |
| F24 Heap profiler (2 MiB) | P4 | Unbiased byte sampling [tc-design] | Frame walk on samples only | Missing frame pointers | Refill sampling (B) | — | — |
| F25 GWP pool | v1.0 | Field detection [gwp-llvm] | ~256 KiB VA, lazy | Sparse rate; mlock interaction | 1/10⁵ (v1) | E18 | — |
| F26 S0 encoding + head/next range checks | v1.0 | Raises the bar on link forgery [cp-safelink][pa-enc] | ≈ +10 instr per pair [est] | Inline links remain [how2heap] | Bitmap pages (C) | E11, E22 | Move the head check to S1 if it costs > 0.5%; the next check if > 1% |
| F27 Reciprocal boundary check | v1.0 (remote), P4 (every free) | Interior and invalid frees never reach UB (S1) [sn-sct] | ~5 instr [est] | — | None | E11 | Promote to S0 local free if ≤ 1% |
| F28 S1 bundle | P4 | Hardened tier | ≤ 10% | Fragmentation from buckets | Bitmap allocator | E11, E22 | Drop sub-features costing > 3% each |
| F29 S2 MTE | P4+ | Hardware tagging | 1.08–1.56× [mte-perf] | Availability; leaks [tiktag] | — | P4 | — |
| F30 Token and hot/cold partitions (opt-in) | P4 | Type split [alloctoken][kalloc-type] | Up to 2× partial pages | RSS | Default on | E6b | Default stays off unless RSS + fragmentation cost < 1% |
| F31 Known-zero calloc (bump cursor + zero-known runs) | v1.0 | Skip `memset` | Bookkeeping | Zero-state bugs | Threaded fresh blocks (v2, wrong); `fresh_groups` (C) | Differential | — |
| F32 Two-view explain + JSON + USDT | v1.0 | Explainability [k-proc] | Code size | — | One fabricated RSS sum (v1) | User study | — |
| F33 Profiles + `AT_SECURE` | v1.0 | One knob [getenv] | — | — | Many knobs | — | — |
| F34 Emergency + bootstrap arenas + deferred-free list | v1.0 | Lock-free, thread-local handler safety; init safety | 384 KiB `.bss` VA | Bounded leak (emergency) | Locks or `remote_direct` in handlers (v2) | Signal-storm test | — |
| F35 PreventFork drain-then-lock + child re-home | v1.0 | Fork safety [sn-fork] | seq_cst on slow paths | Stopped thread delays fork | Lock-then-drain (v2) | Fork tests | — |
| F36 Per-node windows / LLC pools | P4 | NUCA +0.32% fleet [asplos24] | VMAs per node | Masked topology | `mbind` on slices [k-numa] | E19 | Per-node shards only if they win on 2-node hardware |
| F37 Time-slice extension | P4 | Shields lock holders [k-rseqdoc] | — | Needs V2 | — | E13 | Kill if lock-wait p99 does not improve ≥ 10% |
| F38 Large-object lifetime predictor | P5 | Fragmentation [llama][tc-lifetime] | Sampled path only | Google appears to have dropped it (UNVERIFIED) | Small-object prediction | E17 | Enable only on counterfactual RSS drop ≥ 5% |
| F39 Meshing | — | — | memfd, fork copies, breaks DONTNEED [mesh] | — | — | — | **Killed** |
| F40 Soft budget | v1.0 | Fail before the OOM kill | — | False NULLs | — | E8 | Off by default |
| F41 First-class heaps | P4 | CPython [pep703], regions [reconsider26] | API surface | Destroy races [mi-readme] | — | GenMC on destroy | — |
| F42 Nursery page (mixed small classes for the first 64 KiB of a process) | experiment | M1 goal | Generic-path frees for nursery blocks | Complexity | Per-class pages only | E23 | Adopt only if needed for the M1 goal and T3 is unchanged |
| F43 Dense `MADV_HUGEPAGE` area (`thp=dense`) | experiment | Hugepage wins under `defrag=madvise` without VMA splits | +2 VMAs | Direct-reclaim stalls | Blanket `MADV_HUGEPAGE` | E9 | Default on only if p99 first-touch latency rises < 5% and T1 gains ≥ 1% |

---

## 7. Devil's advocate

### 7.1 Strongest remaining objections

1. **"v1.0 is per-thread caching in 2026; Google abandoned it."**
   - *Answer:* the H_max ceiling from effective CPUs, shared slots above it, the B_s bound with hysteresis and non-blocking leases cap the damage. cid mode is designed and shares the fast path.
   - *Residual:* on a 256-CPU host with thousands of threads, TLS mode will trail per-CPU TCMalloc on memory until cid mode passes E14. This is accepted for v1.0 because of the 6.19 and 7.1 ABI churn [lwn-hyrum][k-rseqdoc].
2. **"~34 instructions against mimalloc's 7–10. You will lose every microbenchmark."**
   - *Answer:* the thesis is application-level; T1 is the gate, and T3 tolerates 1.3×.
   - *Mitigations if T1 fails:* move the head check to S1, then the next check; drop `prefetchw`. The reentrancy check, lease acquire and release store are correctness requirements and stay.
   - *Residual:* if T1 still fails, the locality bet was wrong and the design must adopt mimalloc-style owned full pages (E21) or trimmed checks.
3. **"Most same-thread frees go remote, because full pages are orphaned."**
   - *Answer:* buffered remote frees cost no atomics, and E21 compares an owned-full-pages variant in which the slot keeps detached full pages and local frees to them stay local.
   - *Residual:* this may be the largest throughput risk in the design. It is measured early, in P1/P2.
4. **"membarrier IPIs are the opposite of kernel-friendly."**
   - *Answer:* at most one batch per second, only for slots idle ≥ 1 s that hold ≥ 64 KiB or a non-empty rbuf. Never on nohz_full or isolated affinities by default. Counted; opt-out available.
   - *Residual:* when disabled, the S_struct bound applies (§5.9).
5. **"Encoded inline links are exactly what how2heap exploits; C's bitmap would remove the class."**
   - *Answer:* per-page keys, address binding, bswap and range checks mean a blind forger cannot redirect allocation outside the page, and a key leak compromises one page. One format halves the verification surface.
   - *Residual:* a read primitive plus an address leak defeats S0 for that page (§5.12). E11 and E22 re-test the trade.
6. **"Byte-rate GWP almost never samples 32 B objects."**
   - *Residual, accepted:* `secure` lowers the mean; E18 quantifies detection.
7. **"Metadata at computable addresses can be targeted."**
   - *Answer:* it is now separated from data by a `PROT_NONE` gap, so linear overflows cannot reach it.
   - *Residual:* arbitrary writes can, as in any allocator.
8. **"DONTNEED-by-default costs throughput."**
   - *Answer:* demand-window retention keeps recent peak demand resident; E5 measures refaults.
9. **"Refusing `MADV_HUGEPAGE` under `defrag=madvise` throws away the thesis."**
   - *Answer:* partly true, and now stated in §1. Both THP configs gate the release; F43 and opt-in collapse are the experiments that could recover the gains [go-61718][lwn-thp].
10. **"Three designs grafted together is how jemalloc's HPA died."**
    - *Answer:*
      - core budget ≤ 12k lines of C for v1.0;
      - scope cut (§8.1);
      - every feature carries a kill criterion;
      - an honest list of **eight** concurrency protocols in v1.0, each modelled:
        1. `xfree` CAS with ORPHAN/QUEUED/PENDING and the locked `resolve`;
        2. detach/claim single-commit sequences;
        3. the TLS lease (`busy`/`lease` + membarrier);
        4. the rbuf commit-swap with per-thread spare;
        5. PreventFork counters + `g_forking`;
        6. signal reentrancy routing with the deferred list (thread-local but asynchronous);
        7. the emergency bump;
        8. the init once-state.
    - *Residual:* the policy layer still has many interacting PARAMs. v2's claim of "≤ 4 protocols" was an undercount (red team 2).
11. **"The evidence is Google's fleet; it may not transfer."**
    - *Answer:* every borrowed result is an experiment here, and the gate is on public applications built so that the allocator under test actually runs.
12. **"Remote batching delays reuse and pins pages."**
    - *Answer:* E6 decides. The delay is ≤ 64 blocks per slot; free-driven ticks age the buffer out; `soham_thread_idle()` flushes on demand. Pinning ≤ 64 pages per slot is stated.
13. **"What is new? It is a remix."**
    - *Answer:* §4.3, narrowed further with prior art credited [liburcu].
14. **"Single-threaded programs below the inline trigger keep memory."**
    - *Residual, accepted:* up to max(32 MiB, U/4) + 4 MiB; `malloc_trim` is provided.
15. **"A 64 GiB VSZ for `/bin/true` will alarm admins."**
    - *Answer:* the window is `PROT_NONE`, uncommitted, has no page tables and no commit charge in any mode [k-overcommit]. It shrinks under RLIMIT_AS, can be set with `window=`, and is shown in the explain report.
    - *Residual:* tools and policies that key on VSZ will notice.
16. **"Pre-6.19 cid behaviour may make cid mode worse than TLS mode."**
    - *Answer:* that is why it is P4 and gated per kernel version.
17. **"14 months for 2–3 engineers is still optimistic."**
    - *Answer:* possibly. The roadmap is ordered so that each phase ships a measurable, usable artefact, and deferrals are pre-agreed (§8.1).

### 7.2 Red-team findings and resolutions

R1 = red team 1, R2 = red team 2; C = critical, M = major, m = minor. Duplicates across the teams are merged.

| ID | Sev. | Finding | Resolution | Where |
|---|---|---|---|---|
| R1-C1 / R2-C2 | C | Link encode `bswap(n)^K` and decode `bswap(E)^K` are not inverses; every pop returns garbage | Fixed: `enc = bswap(n ^ K)`, `dec = bswap(E) ^ K`. One inline pair for all writers; round-trip property test is a P0 exit gate | §5.5.2, §8.1 |
| R1-C2 / R2-C1 | C | Free checks `owner` before `busy = 1`; a full lease cycle or a handler's refill can detach the page in the window, so the free commits into a foreign page | Fixed: owner compared inside the CS after an acquire load of `lease == 0`; the reclaimer can only take a slot whose `busy` it saw as 0 after `membarrier`; handlers cannot enter. Interleaving is in the model and in ptrace single-step tests | §5.5.4, §5.5.6, §8.4 |
| R1-C3 | C | Unowned pages (owner 0) match unattached or zombie threads (tag 0) | Fixed: explicit `tag == 0` branch to `unattached_free` before any slot access; full 32-bit owner comparison | §5.5.4, §5.10 |
| R1-C4 | C | Fast paths never read `busy`, so a handler re-runs a pop (double allocation) and its `CS_END` clears `busy` under the outer CS | Fixed: `CS_BEGIN` refuses entry when `busy` is set and routes to the emergency arena or deferred list; recounted instructions | §5.5.3, §5.10 |
| R1-C5 | C | Futex on a `u8`; lost wakeup when the reclaimer backs off | Fixed by redesign: the lease word is an aligned `u32`, and **no futex** is used. The owner cancels (`REQ→0`) or abandons (`TAKEN→ABANDONED`) and never waits [futex] | §5.5.6 |
| R2-C3 / R1-M1 | C | Single membarrier with only compiler barriers is wrong on arm64; data races are UB; GenMC has no membarrier model | Fixed: `CS_END` store-release, acquire loads of `lease` (owner) and `busy` (reclaimer), release CAS on completion; all shared words are C11 atomics. Membarrier modelled as SC fences injected at nondeterministic points in other threads (a manual abstraction, stated), plus herd7 litmus tests (tool named by the red team; no URL in the material) and Neoverse stress. Red team 2's second-membarrier suggestion is not adopted *because* the release/acquire pairs cover the hand-off (§5.5.6). It is added if the models disagree [urcu] | §5.5.3, §5.5.6, §8.4 |
| R2-C4 / R1-M3 | C | Handler frees via `remote_direct` can take pool, shard or solo-table locks held by the interrupted code | Fixed: all reentrant frees go to the thread-local deferred list; no handler path takes a lock or runs `resolve` | §5.10 |
| R1-M2 | M | Fork prepare takes locks before draining PreventFork (deadlock); refill collection uncounted | Fixed: set `g_forking`, drain unboundedly with no locks held, then lock; PF sections are outermost; collection, claim, detach, flush, `resolve` and release are PF sections [sn-fork] | §5.10 |
| R1-M4 / R2-M6 | M | ORPHAN/QUEUED: claim races the returning freer; "count reaches cap" undefined remotely; list moves not atomic with the CAS | Fixed: `folded` count makes `xfree.count` the total while unowned (I4); a PENDING responsibility bit; `resolve` re-checks under the pool lock with a CAS that fails on concurrent change; claim `xchg` clears all flags under the lock | §5.4 |
| R1-M5 | M | Pages emptied by same-thread frees never detected; single-threaded release claim fails | **Partly wrong:** pages detached when full are orphans, so frees to them go remote and return the page to the filler, raising F and reaching the inline trigger. For cur/alt pages the fix is adopted: a local emptiness check plus a free-driven tick | §5.5.4, §5.9 |
| R1-M6 / R2-M10 | M | 128 MiB stranding bound ignores cur/alt, spares, run cache and rbuf (≈ 170× too small) and the 256 KiB floor | Restated: S_struct ≈ 20 MiB per idle unleased slot; active ≤ B_s; leased < 64 KiB; medium `alt` dropped, run cache ≤ 1 MiB, spares ≤ 2, lease threshold 64 KiB or non-empty rbuf. M2/L7 gated on the corrected bound | §5.9 |
| R1-M7 / R2-M1 | M | 160 and 224 KiB run classes are 2.5 and 3.5 slices | Fixed: 160/224 KiB are 4-object medium pages (640/896 KiB, 0% tail); 192/256 KiB remain runs; property test asserts slice divisibility | §5.3 |
| R1-M8 / R2-M3 | M | Arena headers and descriptors share THP-eligible hugeblocks; `.bss` gmap THP-eligible under `always`; khugepaged re-collapses | Fixed: all metadata, the gmap and the slot array sit in a separate NOHUGEPAGE metadata area per window, behind a `PROT_NONE` gap; the bootstrap hugeblock is always NOHUGEPAGE | §5.1, §5.2, §5.8.3 |
| R1-M9 | M | K2 ignores khugepaged's `mmap_write_lock` | Fixed: khugepaged collapses counted and excluded; K2 gated on THP=madvise/never | §3, §5.8.3 |
| R1-M10 | M | mTHP per-size controls ignored; wrong knob on 16K/64K | Fixed: all `hugepages-*kB/enabled` read; F sets the provisioning granule and M1 budget; 2 MiB keyed on `hugepages-2048kB` on 16K/64K; CI matrix | §5.1, §5.8.3 |
| R1-M11 / R2-M13 | M | K3 (+8) contradicted by ~25–30 init syscalls; M1 doubtful | Fixed: init cut to 6 syscalls [derived] by deferring THP, overcommit, naming, GWP, cgroup and membarrier; K3 provisional and re-baselined from strace; M1 redefined as RssAnon with a page budget (gate +96 KiB) | §3, §5.10 |
| R1-M12 | M | M1 nearly spent by data pages and metadata | Fixed by restating M1 with an explicit budget. The red team's nursery idea is kept as an experiment (F42/E23), not adopted, because it adds a page kind and slow-path frees | §3, §5.10, §6 |
| R1-M13 / R2-M2 | M | Upward growth with `MAP_FIXED_NOREPLACE` fights top-down placement; "logarithmic" wrong; count wrong | Fixed: a single aligned `PROT_NONE` window with internal prefix growth (red team 2's alternative); count corrected to ≈ 104 steps; VSZ trade-off stated | §5.1, §5.8.1 |
| R1-M14 | M | `mlockall(MCL_FUTURE)` would lock and populate whole RW reservations [mlock] | Fixed: the uniform `PROT_NONE`+prefix model commits small steps when locked; DONTNEED_LOCKED under pressure; gmap committed lazily; CI matrix includes `mlockall` variants | §5.8.1, §8.4 |
| R1-M15 / R2-M17 | M | Background thread: raw signal blocking hangs `setuid()`; created from inside a CS; outlives multithreading and breaks `unshare` | Fixed: `pthread_sigmask` [glibc-sigmask][glibc-intsig][glibc-setxid]; creation before `CS_BEGIN` with depth 0; exits when single-threaded for 1 s; `soham_bg_stop()`; named; unshare-after-join test | §5.9 |
| R1-M16 / R2-M7 | M | `memory.current` includes page cache; no hysteresis; gameable; ancestor compared with own usage | Fixed: levels driven by `memory.events` deltas and anon-headroom estimate (file from `memory.stat` at most every 10 s); per-ancestor comparison; hysteresis bands and 2 s dwell; co-tenant case in M5 | §5.9 |
| R1-M17 | M | Solo unmap or mremap clears gmap bytes shared with neighbours | Fixed: per-granule solo counts; exact interval lookup; mremap onto a pre-reserved aligned destination | §5.7.3 |
| R1-M18 | M | NORESERVE solos change mode-0 malloc failure semantics | Fixed: solos mapped without NORESERVE; conformance test | §5.7.3 |
| R1-M19 | M | calloc known-zero ignores the link word written at provisioning | Fixed: zero-skipping only for bump-cursor (never threaded) blocks and zero-known runs; differential test | §5.7.4 |
| R1-M20 | M | `memalign` (and `cfree`) missing | Fixed: exported with glibc rounding; symbol-completeness CI | §5.13 |
| R1-M21 | M | `malloc(0)` as first call reaches the fast path with NULL slot | Fixed: `countdown <= 0` test; unit test | §5.5.4 |
| R1-M22 | M | M3 at 10 s contradicts the 10 s D_s window and the THP subrelease rule | Fixed: measured at 15 s with FE stated; separate THP=always target (M3b) at 15 s and 330 s | §3 |
| R2-M4 | M | "No slow path for 1 s" picks busy threads; owner blocks on futex (priority inversion); reclaimer unspecified | Fixed: activity epoch bumped on all events; the owner never waits (cancel or abandon); the reclaimer is the background thread only; E16 gates leases on busy slots at 0 | §5.5.6 |
| R2-M5 | M | Remote frees still read the owner's hot line (HITM) | Fixed: three lines (O/R/X); owner, key, bsize and recip copied to the read-mostly line; E6 reports HITM on line O | §5.4 |
| R2-M8 | M | Hoard donation has no hysteresis; `S_active` jumps; `L_live` undefined | Fixed: donate to B_s/2; donor skip window 100 ms; `S_act` EWMA; defined in terms of U; sawtooth oscillation test (E25) | §5.9, §9 |
| R2-M9 | M | Time-based jobs ride an allocation countdown, so free-only or idle threads never age their rbuf | Fixed: a separate free-driven `tick`; leases accept slots with non-empty rbufs; pinning bound stated in pages | §5.5.4, §5.5.5, §5.5.6 |
| R2-M11 | M | Thesis relies on hugepages, which are off under the default THP config; THP not pinned in benchmarks | Accepted and stated in §1; all T/M gates run on two THP configs; dense-area experiment F43. The no-`MADV_HUGEPAGE` default is **kept**, because the stall evidence [go-61718] is stronger than the gain evidence for this config | §1, §3, §5.8.3 |
| R2-M12 | M | Three of six T1 workloads do not exercise the allocator (fleetbench, Envoy bundle TCMalloc; pymalloc ≤ 512 B); RocksDB may link jemalloc (UNVERIFIED) | Fixed: build recipes [fleetbench][envoy-bazel][py-mem]; RocksDB verified with ldd/nm; > 99% served-fraction check invalidates runs; T1d reports default builds | §3, §8.5 |
| R2-M14 | M | v1.0 exit omits throughput, S0 and U1 gates; P2 blocked on a P4 model; scope too large; protocol count understated | Fixed: P3 exit carries all v1.0 gates; cid modelling moved to P4; scope cut (NUMA, LLC pools, PSI, profiler, ABIs, deterministic and throughput profiles, mode-2 decommit deferred); eight protocols listed and modelled; durations re-estimated | §7.1 item 10, §8.1 |
| R2-M15 | M | S0 metadata in-band at computable offsets; head not validated; underflow hits descriptors; address-order provisioning leaks keys; no attack benchmark | Fixed: head range check on pop; metadata behind a `PROT_NONE` gap; key-recovery trade-off documented (S0 shuffle not adopted, since shuffling only yields a 1-in-7 guess); security-effectiveness gate SE / E22 | §5.12, §3, §9 |
| R2-M16 | M | Guard markers conflict with mlock; no CRIU test | Fixed: guards only in the GWP pool by default; S1 guards only when not mlocked; `PROT_NONE` fallback or disable; mlock/mlockall and CRIU tests [madvise][criu-guard] | §5.7.3, §5.12, §8.4 |
| R1-m1 | m | A racing double free makes a cycle in the xfree chain, and the walk never terminates | Fixed: walk exactly `count` nodes; abort on terminator or total mismatch; fuzz case | §5.4 |
| R1-m2 | m | Novelty claims have prior art (asymmetric Dekker, liburcu, folly; jemalloc and mimalloc activity-driven purge) | Fixed: claims narrowed and credited [liburcu][urcu]; folly and Dice et al. marked UNVERIFIED | §4.3 |
| R1-m3 | m | Density split contradicts object counts; TAIL frees take the generic path; 1–8 KiB path unspecified | Fixed: split by kind; per-slice `head_delta` resolves TAIL on the fast path; 1–8 KiB path specified | §5.3, §5.5.4, §5.7.1 |
| R1-m4 | m | tls-shared threads pay atomics on every op; H_max ignores `cpu.max` | Fixed: local free under the shared trylock; H_max from min(cpuset, cpu.max quota); E14 TLS-mode 2000-thread case | §5.5.1 |
| R1-m5 / R2-m2 | m | membarrier registration and EPERM unspecified; 4.14 verifiable; nohz_full skip too conservative | Fixed: lazy registration at the first lease batch; any non-zero return latches off; citations replace UNVERIFIED [membarrier][k-cpu]. The conservative nohz_full default is **kept** (protects RT threads), with a `lease=always` override | §5.5.6 |
| R1-m6 | m | Spare-buffer stack empty or ABA; LLC selection needs getcpu | Fixed: per-thread spare buffer, no shared stack; LLC selection only in P4 | §5.5.5 |
| R1-m7 / R2-m1 | m | Per-batch pidfd can leak into a forked child; release ladder over-engineered; 6.16/6.17 claims unverified | Fixed: 6.13–6.14 pidfd path dropped; two paths; 6.16/6.17 marked UNVERIFIED | §5.8.2 |
| R1-m8 | m | `dlsym` may allocate during init | Fixed: lazy resolution on the first foreign free, after init; recursion served from the bootstrap arena; claim remains UNVERIFIED | §5.2 |
| R2-m3 | m | Inconsistencies: RLIMIT_AS /16 vs /2; GWP handler both "only if default" and "chains"; First-Fit/Best-Fit claim not in bender26's abstract; bumper size per page | Fixed: one RLIMIT_AS rule (Wsz); GWP installs and chains, with displacement reported; FF/BF claim UNVERIFIED; guard sizes stated per base page | §5.1, §5.7.1, §5.12, §5.15 |
| R2-m4 | m | Security gates measure only cost vs tier −1; T3 median only; no inline-release stall gate | Fixed: SE gate; S0 reported vs glibc and mimalloc; T3b p99/p99.9; T5 stall ≤ 1 ms with a 16 MiB per-call release budget | §3, §5.9 |

Items both red teams marked as now verified: membarrier PRIVATE_EXPEDITED since 4.14 with registration [membarrier]; the nohz_full and isolated sysfs attributes [k-cpu]; TCMalloc breaking on 6.19 [lwn-hyrum]; `max_ptes_none` defaulting to 511 [k-khp]; the existence of the bender26 paper [bender26]. Still UNVERIFIED: the 6.16/6.17 madvise details, the 7.1 rseq claims beyond [k-rseqdoc], and RocksDB auto-linking jemalloc.

---

## 8. The build process

### 8.1 Phased roadmap

Durations assume 2–3 engineers and are [est]. Time to v1.0 is about 14 months, which is still aggressive; the deferral list below is agreed in advance.

| Phase | Content | Exit criteria |
|---|---|---|
| **P0 Foundations** (4 months) | Harness and trace recorder; geometry; heap window and uniform prefix commit; gmap; size-class generator; record format and `enc`/`dec`; single-thread core; full C and C++ API including `memalign`/`cfree`; bootstrap and early init; differential oracle and fuzzers; GenMC models of `xfree` + `resolve` (including list operations), detach and claim; LICENSE (recommended: dual MIT/Apache-2.0; owner decision), NOTICE, SECURITY.md. TCMalloc Apache-2.0 reference copies (`hpf.h`, `hps.h`) in `third_party/reference/` with attribution | glibc malloc test suite passes; encode/decode round-trip property passes; 24 h fuzzing clean; M1 met on x86 4K; init `strace` taken and K3 re-baselined; preload tier 1 (coreutils, Python, clang) passes; the P0 GenMC models pass; mode-0 large-malloc conformance passes |
| **P0.5 Simulation** (1 month) | Trace simulator for classes, pool orders and retention | E2 and E4 decided offline |
| **P1 Single-thread performance** (2 months) | Fast paths; **prototype measurement against mimalloc v3.4.x and per-CPU TCMalloc on Zen 4 and Neoverse V2, with dTLB and LLC counters** (Judge 1); first E21 measurement | T3 ≤ 1.3×; single-thread T1 subset ≥ 0.97× mimalloc |
| **P2 Multithreading, TLS mode** (4 months) | Slots, pool, Hoard bound, remote buffer, lease, PreventFork, signals and deferred list. **GenMC models (with injected-fence membarrier abstraction) and herd7 litmus tests of the lease, flush, PreventFork and reentrancy must pass before code** | All lifecycle tests pass (incl. ptrace single-step lease injection on arm64 hardware); M2 met; larson and xmalloc scale; E6 and E21 decided |
| **P3 OS policy** (3 months) | Release engine, THP matrix with mTHP, cgroup pressure levels, background thread, VMA and lock gates | **v1.0 release gates, on both THP configs:** T1, T1b, T1c, T2, T3, T3b, T4 (Zen 4 and Neoverse V2), T5, M1–M6, K1–K5, S0 cost, SE for S0 claims, U1 tiers 1–2 → **v1.0 (S0, TLS mode)** |
| **P4** (4 months) | S1; GWP tuning; mode-2 top decommit (v1.1); NUMA windows and LLC pools; PSI; heap profiler; first-class heaps; Rust crate; hot/cold and AllocToken ABIs; `throughput`, `latency`, `deterministic` profiles; cid mode with GenMC of the cid reclaim and the kernel gate (6.6, 6.12, 6.18, 6.19, 7.1); S2 begins | S1 ≤ 10%; SE for S1 claims; E14 decision recorded |
| **P5** (open) | Large-object lifetime counterfactual; macOS and Windows | E17 |

### 8.2 Repository layout

```
LICENSE NOTICE SECURITY.md CONTRIBUTING.md
include/        sohamalloc.h  sohamalloc_ext.h
src/core/       sizeclass.c record.c slot.c remote.c pool.c filler.c large.c solo.c
                release.c policy.c sample.c stats.c explain.c init.c fork.c signal.c emergency.c
src/prim/linux/ window.c thp.c cgroup.c membarrier.c bgthread.c (P4: rseq_x86_64.S rseq_aarch64.S)
src/override/   libc.c  cxx.cc
gen/            sizeclass_gen.py -> gen/sizeclass_table.h (+ waste report)
tests/          unit/ stress/ diff/ fuzz/ model/(GenMC, herd7) lifecycle/ preload/ conformance/ security/
bench/          harness/ workloads/ recipes/ schemas/ results/
tools/          trace-recorder/ simulator/
third_party/reference/  (non-built reference copies, with licences)
docs/
```

### 8.3 Coding standards

- **Language:** C11 with `<stdatomic.h>`. C++17 only in `cxx.cc`.
- **Warnings:** `-Wall -Wextra -Werror`; clang-tidy.
- **No allocating libc calls in the core.** Raw syscalls only, except `pthread_create`, `pthread_sigmask` and `pthread_atfork`, which are used deliberately (§5.9, §5.10). Every return code is checked; errno is saved and restored.
- **No page-size literals:** a CI grep bans `4096`, `0x1000` and `PAGE_SIZE` in `src/`.
- **Atomics:**
  - Every word accessed by more than one thread or by a signal handler is a C11 atomic.
  - Each carries a comment naming its pairing and memory order, mapped to a model.
  - Store-release and load-acquire are required wherever another thread hands off state.
- **Futex** only on naturally aligned 32-bit words [futex].
- **Single-commit rule:** any new multi-step transition needs a design note, a PreventFork classification and a model.
- **PreventFork rule:** PF sections are outermost; never wait at PF entry while holding an allocator lock.
- **Reentrancy rule:** any code that holds an allocator lock runs with `depth > 0` (and `busy = 1` when it has a slot).
- **TLS:** initial-exec only, exactly ≤ 48 B, enforced with a `static_assert`.
- **Generated tables** are checked in and verified in CI.
- **Hot paths:** `musttail` or `noinline` boundaries between fast and slow paths. Assembly listings of the fast paths are diffed in CI so instruction-count regressions are visible.
- **Code budget:** ≤ 12k lines of C for the v1.0 core, excluding tests, tools and `cxx.cc`.

### 8.4 Testing strategy

**Unit tests:**

- size-map properties;
- `enc`/`dec` round-trip for random keys and addresses (P0 gate);
- record arithmetic, and run-class slice divisibility;
- retention formula, hysteresis transitions and bound assertions;
- `malloc(0)` as a thread's first call.

**Stress tests:**

- randomised multithreaded alloc/free/realloc with content checksums;
- producer/consumer with back-pressure;
- 1M thread create/exit; 10k idle threads;
- a sawtooth working set around B_s, gated on pool-lock acquisitions per second and donate/claim cycles per page (E25).

**Randomised differential testing:** API sequences against a reference oracle (glibc semantics plus a shadow map). The oracle checks:

- no overlap;
- alignment;
- content preserved across realloc;
- `malloc_usable_size` ≥ the request;
- errno behaviour;
- calloc all-zero for every class after fresh provisioning and after reuse.

**Fuzzing:** libFuzzer and AFL++ on API sequences and on corrupted-metadata scenarios, including cyclic remote chains and forged heads. An S0/S1 abort is expected; UB never is. 24 h before each release.

**Concurrency verification:**

- GenMC [genmc] models of every protocol in §7.1 item 10.
  - membarrier is modelled as SC fences injected at nondeterministic points in each other thread. This is a manual abstraction (GenMC lists only C11-family memory models in its README [genmc]), and the docs say so.
- herd7 litmus tests on the AArch64 model for the lease hand-off.
- TSan on a no-override build.
- ptrace single-step injection: stop and signal a thread between each pair of fast-path instructions while forcing leases, refills and handler allocations.
- Stress on real Neoverse hardware, not only x86.

**Fork and signal tests:**

- fork from 64 threads mid-free;
- fork during a lease, a flush and a `resolve`;
- signal storms (`setitimer`, malloc and free in the handler), including while slow paths hold each pool, shard and solo-table lock;
- `setuid()` in a multithreaded process with the background thread running.

**Early init and linking tests:**

- `free(NULL)` before constructors;
- late `dlopen` from JNI and Python extensions;
- library mode: sohamalloc dlopened late, foreign frees forwarded;
- static glibc; static musl with the recipe;
- a `DT_NEEDED` check under `--as-needed`;
- `GLIBC_TUNABLES=glibc.pthread.rseq=0`;
- symbol completeness against the glibc manual's list.

**Lifecycle tests:**

- free from a C++ `thread_local` destructor into an orphaned page;
- free from a new thread before its first malloc;
- 8 threads joined, then `soham_bg_stop()`, then `unshare(CLONE_NEWUSER)` succeeds;
- a single thread allocates 1 GiB of small objects, frees all, sleeps 15 s; RSS checked against M3s.

**Conformance tests:** `malloc(RAM + swap + 1)` returns NULL in overcommit mode 0; `memalign` with non-power-of-two alignment.

**Environment matrix:**

- kernels 6.1, 6.6, 6.12, 6.18, 6.19, 7.1;
- glibc 2.35, 2.39, 2.41, 2.44; musl 1.2;
- arm64 4K, 16K, 64K;
- overcommit 0, 1, 2; `ulimit -v 2G`; low `max_map_count`;
- THP always/madvise/never × `max_ptes_none` 0/511 × mTHP {16K, 64K, 2M} × {always, madvise, never};
- `mlockall(MCL_CURRENT|MCL_FUTURE[|MCL_ONFAULT])` and range `mlock`, in S0 and S1;
- seccomp profiles that deny membarrier, `process_madvise` or `open`;
- a cgroup with `memory.max` and a co-tenant streaming page cache;
- CRIU dump/restore of a process with a GWP pool [criu-guard].

**LD_PRELOAD smoke suite:**

- tier 1: coreutils, Python test suite, clang build;
- tier 2: Redis (`MALLOC=libc`) tests, PostgreSQL regression, Node, JVM with JNI;
- tier 3: Firefox, Envoy (`--define tcmalloc=disabled`).

**Security suite (E22):** how2heap-style primitives [how2heap] and replayed allocator-CVE patterns against S0 and S1. Each claim in §5.12 is checked.

**Soak:** 72 h with RSS-drift and VMA-count alarms.

### 8.5 Benchmarking methodology

- **Pinned baselines:**
  - glibc 2.44 and 2.39;
  - jemalloc 5.4.0 with and without `background_thread`;
  - mimalloc v3.4.5;
  - Google TCMalloc with per-CPU mode *verified active* (gperftools is never accepted as TCMalloc [mibench]);
  - snmalloc;
  - Scudo and hardened_malloc for tier costs.
- **Build recipes:**
  - fleetbench with `custom_malloc` per allocator [fleetbench];
  - Envoy with `--define tcmalloc=disabled` and a preload [envoy-bazel];
  - pyperformance with `PYTHONMALLOC=malloc`, with default-pymalloc runs reported separately [py-mem];
  - RocksDB with jemalloc disabled, verified with `ldd` and `nm`;
  - Redis `MALLOC=libc`, with the bundled-jemalloc build reported separately (it is the production configuration and has active defrag).
- **Served-fraction check:** each run records the allocator's own malloc counter against an `ltrace`-free call count from a `LD_PRELOAD` shim. Runs below 99% are invalid.
- **THP configurations:** every run records `enabled`, `defrag`, `max_ptes_none` and the mTHP settings. Gates use configs (a) and (b) from §3.
- **Hardware:** Zen 4, Neoverse V2, a 256-CPU box, a hybrid-core machine, a 2-vCPU cgroup.
- **Statistics:** ≥ 10 runs with re-linking or layout randomisation [mytkowicz][stabilizer]; 95% bootstrap CIs; speed and memory against the same baseline; latency percentiles from HDR-style histograms.
- **Phases:** ramp, steady, burst, ≥ 60 s idle (≥ 330 s for M3b). RSS sampled every 100 ms from `smaps_rollup` and `memory.current`.
- **Counters:** `perf stat -d -d` [perf-stat]; `perf c2c` for HITM; `/proc/interrupts` TLB lines; VMA count; `mmap_lock` tracepoints; khugepaged counters.
- **Loads:** constant-rate load [wrk2]; adversarial preconditioning [reconsider26]. Trace replay is used only for packing, never for decay, because glibc traces carry no timestamps [glibc-trace].
- **Publication:** scripts, versions, kernel and THP settings, and raw data in a schema, so anyone can reproduce the numbers.

### 8.6 CI

| Cadence | Content |
|---|---|
| Per PR | gcc and clang on x86 and arm64; unit tests incl. the round-trip property; 10 min differential fuzzing; sanitizer builds; fast-path assembly diff; symbol completeness; M1, K1 and K3 gates; microbench smoke (a regression above 3% blocks the PR) |
| Nightly | Kernel, libc, THP/mTHP, overcommit and mlock matrix VMs; 16K and 64K QEMU; GenMC and herd7 models; 2 h fuzzing; preload tiers 1–2; ptrace injection suite on an arm64 runner |
| Weekly | Full T/M/K/SE benchmark suite on dedicated hardware in both THP configs; 72 h soak; CRIU test |

---

## 9. Open research questions and experiments

| ID | Question | Method | Decides |
|---|---|---|---|
| E1 | Startup RSS and syscalls | `/bin/true`, 10k fork+exec, vs glibc and mimalloc; `strace -c`; RssAnon and RssFile | M1, K3 |
| E2 | 64 vs 52 classes; medium page rule | Trace histograms (Redis, RocksDB, clang, CPython, Envoy): rounding + tail waste + partial-page RSS | F10, F14 |
| E3 | Lookup cost | Free-heavy microbenchmark vs mimalloc and TCMalloc; perf L1/LLC | F9 |
| E4 | Pool order | FIFO, LIFO, fullest-first and fullest-with-≥1/8: LLC misses, dTLB misses, RSS | F12 |
| E5 | DONTNEED refault cost | Fault rate and release syscalls per GiB, per profile, with and without INVLPGB | F17, F18 |
| E6 | Remote batching default; three-line record value | xmalloc-test, back-pressured producer/consumer [batchit]; `perf c2c` HITM on line O | F6, F7 |
| E6b | Token and hot/cold partitions | RSS and fragmentation with partitions on | F30 |
| E7 | Churn | 1k thread create/exit per second, RSS-time integral | M2 |
| E8 | Retention windows and pressure levels | Phased runs under `memory.max` = 1.2× WS; single-thread variant; co-tenant page-cache streaming; hysteresis transition counts | F18, F19 |
| E9 | THP matrix, opt-in collapse, dense area | dTLB walk cycles, AnonHugePages, p99 first-touch, T1 under `thp=dense` | F20, F21, F43 |
| E10 | mimalloc-bench geomean | Standard suite | T2 |
| E11 | Security ablation | Per-feature cost vs tier −1, glibc and mimalloc; head/next check costs; reciprocal-check promotion; bitmap-format locality (< 2% threshold) | F26–F28 |
| E11b | Overcommit, RLIMIT and mlock matrix | modes 0/1/2 × `ulimit -v` × swapless VM × `mlockall`; verify that NORESERVE prefix growth is uncharged in modes 0/1 | F16 |
| E12 | `mmap_lock` writes | Lock tracepoints in steady state, khugepaged attributed | K2 |
| E13 | Time-slice extension | Shard-lock hold and wait p99 | F37 |
| E14 | cid vs TLS mode | Kernels 6.6/6.12/6.18/6.19/7.1 at (4 threads, 256 CPUs), (10k threads, 64 CPUs), (2000 threads, 64 CPUs, TLS mode), (cpuset of 2); same-thread-free-remote rate; rseq abort rate | F3 |
| E15 | Window placement and growth | EEXIST rate of aligned hints under ASLR with interleaved dlopen/mmap; VSZ, mmap count, VMAs | §5.1 |
| E16 | Lease cost and correctness | IPIs/s and CPU on the 256-CPU box; stranded-memory reduction; leases on busy slots (target 0); owner p99 | F2 |
| E17 | Large-object lifetime | Counterfactual mode on RocksDB and CPython | F38 |
| E18 | GWP rate | Seeded-bug detection probability vs cost | F25 |
| E19 | NUMA shards | 2-node box, with and without per-node windows | F36 |
| E20 | Adversarial fragmentation | Preconditioned heaps [reconsider26] | Robustness |
| E21 | Owned full pages vs orphaned full pages | Variant where detached full pages stay owned (local frees stay non-atomic, requeued at ≥ 1/8 free) vs the default; T1, M2, same-thread-free-remote rate | F4 |
| E22 | Security effectiveness | how2heap-style primitives and replayed CVE patterns per tier | SE gate |
| E23 | Nursery page | M1 and T3 with and without F42 | F42 |
| E25 | Bound oscillation | Sawtooth working set around B_s; pool-lock acquisitions/s; donate/claim cycles per page | §5.9 |

**Open questions:**

- Can request fragmentation (k) be chosen online from TLB-miss feedback, given the Θ(log log M) result [bender26]?
- Does cid mode remove the need for leases entirely on 6.19+ per-task CID kernels?
- Can AllocToken-driven splitting run at zero RSS cost with lazy per-token pages?
- Does `MADV_NOHUGEPAGE` suppress mTHP folios in all kernels in the matrix (UNVERIFIED)?
- Do our GWP guard regions make an application's later `mlockall(MCL_CURRENT)` fail (UNVERIFIED)?

---

## 10. References

All URLs below appear in the research reports, critiques, designs, judges' notes or red-team reports.

- [asplos24] https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf
- [osdi21] https://www.usenix.org/system/files/osdi21-hunter.pdf
- [ismm21] https://research.google/pubs/pub50436/
- [mi-alloc] https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/alloc.c
- [mi-free] https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/free.c
- [mi-pagemap] https://github.com/microsoft/mimalloc/blob/dev3/include/mimalloc/heap/page-map.h
- [mi-readme] https://github.com/microsoft/mimalloc/blob/main/readme.md
- [mi-351] https://github.com/microsoft/mimalloc/issues/351
- [mi-1025] https://github.com/microsoft/mimalloc/issues/1025
- [mi-1104] https://github.com/microsoft/mimalloc/issues/1104
- [mi-1282] https://github.com/microsoft/mimalloc/issues/1282
- [mi-1341] https://github.com/microsoft/mimalloc/issues/1341
- [tc-percpu] https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/percpu.cc
- [tc-hpf] https://github.com/google/tcmalloc/blob/master/tcmalloc/huge_page_filler.h
- [tc-sub] https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/huge_page_subrelease.h
- [tc-design] https://github.com/google/tcmalloc/blob/master/docs/design.md
- [tc-params] https://github.com/google/tcmalloc/blob/master/tcmalloc/parameters.cc
- [tc-ext] https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h
- [tc-lifetime] https://github.com/google/tcmalloc/blob/master/docs/lifetime-based-allocator.md
- [tc-msd] https://github.com/google/tcmalloc/blob/master/docs/mismatched-sized-delete.md
- [tc-188] https://github.com/google/tcmalloc/issues/188
- [tc-regions] https://github.com/google/tcmalloc/blob/master/docs/regions-are-not-optional.md
- [tc-sysalloc] https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/system_allocator.h
- [lwn-hyrum] https://lwn.net/Articles/1070072/
- [mongo-90036] https://jira.mongodb.com/browse/SERVER-90036
- [k-cid] https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/patch/?id=af7f588d8f7355bc4298dd1962d7826358fc95f0
- [k-cidrw] https://lkml.iu.edu/2511.2/08641.html
- [k-sched] https://github.com/torvalds/linux/blob/master/kernel/sched/core.c
- [k-rseqh] https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/rseq.h
- [k-rseqdoc] https://docs.kernel.org/userspace-api/rseq.html
- [glibc-rseq] https://sourceware.org/glibc/manual/latest/html_node/Restartable-Sequences.html
- [lkml-rseqsize] https://lkml.iu.edu/2602.2/07723.html
- [glibc-news] https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=NEWS;hb=HEAD
- [glibc-replace] https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html
- [glibc-malloc] https://sourceware.org/git/?p=glibc.git;a=blob;f=malloc/malloc.c;hb=HEAD
- [glibc-tun] https://raw.githubusercontent.com/bminor/glibc/master/manual/tunables.texi
- [glibc-30186] https://sourceware.org/bugzilla/show_bug.cgi?id=30186
- [glibc-arena] https://sourceware.org/git/?p=glibc.git;a=commit;h=93e6135904eec91f6aaf5500175e7593d260dbe7
- [glibc-trace] https://sourceware.org/glibc/wiki/MallocTracing
- [glibc-sigmask] https://raw.githubusercontent.com/bminor/glibc/master/nptl/pthread_sigmask.c
- [glibc-intsig] https://raw.githubusercontent.com/bminor/glibc/master/sysdeps/unix/sysv/linux/internal-signals.h
- [glibc-setxid] https://raw.githubusercontent.com/bminor/glibc/master/nptl/nptl_setxid.c
- [unwind-link] https://sourceware.org/git/?p=glibc.git;a=blob;f=misc/unwind-link.c
- [membarrier] https://man7.org/linux/man-pages/man2/membarrier.2.html
- [futex] https://man7.org/linux/man-pages/man2/futex.2.html
- [mlock] https://man7.org/linux/man-pages/man2/mlock.2.html
- [pidfd-open] https://man7.org/linux/man-pages/man2/pidfd_open.2.html
- [liburcu] https://liburcu.org/
- [urcu] https://raw.githubusercontent.com/urcu/userspace-rcu/master/src/urcu.c
- [madvise] https://man7.org/linux/man-pages/man2/madvise.2.html
- [k-madvise] https://raw.githubusercontent.com/torvalds/linux/master/mm/madvise.c
- [pmadvise] https://man7.org/linux/man-pages/man2/process_madvise.2.html
- [k-pidfd] https://raw.githubusercontent.com/torvalds/linux/v6.15/include/uapi/linux/pidfd.h
- [k-mprotect] https://raw.githubusercontent.com/torvalds/linux/master/mm/mprotect.c
- [k-khp] https://raw.githubusercontent.com/torvalds/linux/master/mm/khugepaged.c
- [k-mmapc] https://github.com/torvalds/linux/blob/master/mm/mmap.c
- [k-vmac] https://github.com/torvalds/linux/blob/master/mm/vma.c
- [k-util] https://github.com/torvalds/linux/blob/master/mm/util.c
- [k-overcommit] https://docs.kernel.org/mm/overcommit-accounting.html
- [k-arm64mem] https://docs.kernel.org/arch/arm64/memory.html
- [k-arm64hp] https://docs.kernel.org/arch/arm64/hugetlbpage.html
- [k-5lvl] https://docs.kernel.org/arch/x86/x86_64/5level-paging.html
- [k-thp] https://docs.kernel.org/admin-guide/mm/transhuge.html
- [k-cpu] https://raw.githubusercontent.com/torvalds/linux/master/drivers/base/cpu.c
- [mmap] https://man7.org/linux/man-pages/man2/mmap.2.html
- [mremap] https://man7.org/linux/man-pages/man2/mremap.2.html
- [lwn-mremap] https://lwn.net/Articles/1015762/
- [k-vmaname] https://man7.org/linux/man-pages/man2/pr_set_vma.2const.html
- [k-numa] https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html
- [k-cg2] https://docs.kernel.org/admin-guide/cgroup-v2.html
- [k-psi] https://docs.kernel.org/accounting/psi.html
- [lwn-rstat] https://lwn.net/Articles/951695/
- [k-proc] https://docs.kernel.org/filesystems/proc.html
- [unshare] https://man7.org/linux/man-pages/man2/unshare.2.html
- [k-vm] https://docs.kernel.org/admin-guide/sysctl/vm.html
- [lwn-invlpgb] https://lwn.net/Articles/1008065/
- [k-arm64tlb] https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm64/include/asm/tlbflush.h
- [k-mseal] https://docs.kernel.org/userspace-api/mseal.html
- [k-mte] https://docs.kernel.org/arch/arm64/memory-tagging-extension.html
- [k-uio] https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/uio.h
- [k-dnl] https://lkml.iu.edu/hypermail/linux/kernel/2203.0/05125.html
- [k-sched-cap] https://docs.kernel.org/scheduler/sched-capacity.html
- [k-initfree] https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=6471384af2a6530696fc0203bafe4de41a23c9ef
- [criu-guard] https://lkml.rescloud.iu.edu/2503.2/05377.html
- [go116] https://go.dev/doc/go1.16
- [go-42330] https://github.com/golang/go/issues/42330
- [go-61718] https://github.com/golang/go/issues/61718
- [go-scav] https://raw.githubusercontent.com/golang/go/master/src/runtime/mgcscavenge.go
- [go-collapse] https://fuchsia.googlesource.com/third_party/go/+/ef6993f3270c95e1158b11d9fc1a047528b34da6
- [je-pages] https://github.com/jemalloc/jemalloc/blob/dev/src/pages.c
- [je-hpa] https://github.com/jemalloc/jemalloc/blob/dev/include/jemalloc/internal/hpa_opts.h
- [je-man] https://raw.githubusercontent.com/jemalloc/jemalloc/dev/doc/jemalloc.xml.in
- [je-1398] https://github.com/jemalloc/jemalloc/issues/1398
- [je-postmortem] https://jasone.github.io/2025/06/12/jemalloc-postmortem/
- [je-changelog] https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog
- [je-inl] https://github.com/jemalloc/jemalloc/blob/dev/include/jemalloc/internal/jemalloc_internal_inlines_c.h
- [sn-pagemap] https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds/pagemap.h
- [sn-fork] https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds_aal/prevent_fork.h
- [sn-queue] https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/mem/freelist_queue.h
- [sn-sct] https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds/sizeclasstable.h
- [sn-sec] https://github.com/microsoft/snmalloc/blob/main/docs/security/README.md
- [sn-paper] https://www.microsoft.com/en-us/research/uploads/prod/2020/04/snmalloc.pdf
- [batchit] https://microsoft.com/en-us/research/uploads/prodnew/2024/05/preprint_batchit.pdf
- [scudo-tsd] https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/scudo/standalone/tsd_shared.h
- [scudo-flags] https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/scudo/standalone/flags.inc
- [hm-readme] https://github.com/GrapheneOS/hardened_malloc/blob/main/README.md
- [pa-enc] https://github.com/chromium/chromium/blob/main/base/allocator/partition_allocator/src/partition_alloc/encoded_next_freelist.h
- [pa-tc] https://github.com/chromium/chromium/blob/main/base/allocator/partition_allocator/src/partition_alloc/thread_cache.cc
- [kalloc-type] https://security.apple.com/blog/towards-the-next-generation-of-xnu-memory-safety/
- [xzone] https://cs.iossec.tech/libmalloc/latest/source/doc/xzone_malloc.md
- [alloctoken] https://clang.llvm.org/docs/AllocToken.html
- [memprof] https://www.llvm.org/docs/MemProf.html
- [gwp-llvm] https://llvm.org/docs/GwpAsan.html
- [cp-safelink] https://research.checkpoint.com/2020/safe-linking-eliminating-a-20-year-old-malloc-exploit-primitive/
- [how2heap] https://github.com/shellphish/how2heap
- [mte-perf] https://arxiv.org/abs/2601.11786
- [tiktag] https://arxiv.org/abs/2406.08719
- [bender26] https://arxiv.org/abs/2608.28462
- [lamprakos] https://arxiv.org/abs/2305.01497
- [llama] https://cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf
- [hoard] https://people.cs.umass.edu/~emery/pubs/berger-asplos2000.pdf
- [numalloc] https://par.nsf.gov/servlets/purl/10418579
- [obase] https://arxiv.org/abs/2603.00378
- [valinor] https://arxiv.org/abs/2607.14789
- [reconsider26] https://arxiv.org/abs/2605.17119
- [mesh] https://arxiv.org/abs/1902.04738
- [fleetbench] https://github.com/google/fleetbench
- [envoy-bazel] https://github.com/envoyproxy/envoy/blob/main/bazel/README.md
- [py-mem] https://docs.python.org/3/c-api/memory.html
- [mibench] https://github.com/daanx/mimalloc-bench
- [rpm-csv] https://github.com/mjansson/rpmalloc/blob/develop/benchmark/results/mimalloc-bench-allt.csv
- [mytkowicz] https://sape.inf.usi.ch/publications/asplos09
- [stabilizer] https://people.cs.umass.edu/~emery/pubs/stabilizer-asplos13.pdf
- [wrk2] https://github.com/giltene/wrk2
- [genmc] https://raw.githubusercontent.com/MPI-SWS/genmc/master/README.md
- [perf-stat] https://man7.org/linux/man-pages/man1/perf-stat.1.html
- [speedshop] https://www.speedshop.co/blog/malloc-doubles-ruby-memory/
- [cloudflare] https://blog.cloudflare.com/the-effect-of-switching-to-tcmalloc-on-rocksdb-memory-use/
- [node-51868] https://github.com/nodejs/node/issues/51868
- [tweag] https://tweag.io/blog/2023-08-10-rust-static-link-with-mimalloc/
- [antirez] https://www.antirez.com/news/84
- [lwn-thp] https://lwn.net/Articles/986242/
- [ruby-18409] https://bugs.ruby-lang.org/issues/18409
- [musl-glue] https://git.musl-libc.org/cgit/musl/tree/src/malloc/mallocng/glue.h
- [pep703] https://peps.python.org/pep-0703/
- [rust-ga] https://doc.rust-lang.org/std/alloc/trait.GlobalAlloc.html
- [cxx-new] https://eel.is/c++draft/new.delete.single
- [getenv] https://man7.org/linux/man-pages/man3/getenv.3.html
- [sigsafety] https://man7.org/linux/man-pages/man7/signal-safety.7.html
- [pmemalign] https://man7.org/linux/man-pages/man3/posix_memalign.3.html
- [asahi] https://asahilinux.org/docs/sw/broken-software/
- [valgrind] https://valgrind.org/docs/manual/manual-core.html
- [lkml-flatvcpu] https://lkml.iu.edu/2208.0/01988.html
