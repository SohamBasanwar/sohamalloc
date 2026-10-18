# sohamalloc: research notes and design proposal

> Goal: a general-purpose `malloc` for Linux (portable later) that is fast,
> memory-efficient, kernel-friendly, secure by default, and easy to use and
> understand.
>
> This document covers what the best allocators of 2026 do, where they fall
> short, and how sohamalloc could do better. Bracketed numbers like [3] refer
> to the reference list at the end.

---

## 0. The honest starting point

There is no single "best possible malloc", and two results explain why:

1. **Robson's bound.** If an allocator never moves objects, an adversarial
   program can force its fragmentation up to Θ(log n) times the live memory,
   where n is the ratio of the largest to the smallest object size [14]. No
   size-class or fit policy escapes this. You can only make the bad case rare,
   or compact memory some other way (see Mesh, §2).
2. **The axes conflict.** Speed, memory footprint, tail latency, security and
   scalability pull against each other. Caching more memory makes the fast
   path faster and the footprint larger. Hardening adds cycles. Hugepages
   speed up the application but make memory harder to give back.

So "the best" really means two things:

- **Pick a good default point** on that trade-off curve, judged by what the
  whole *application* gains, not by nanoseconds per `malloc`.
- **Adapt at runtime**, so the allocator drifts toward the right point for
  the workload and machine it is actually running on.

Google's fleet data makes the same point. Malloc is around 4–7% of cycles
[2][11], but the allocator's effect on the *application's* cache and TLB
behaviour is larger than its own cycles [1][2]. TCMalloc deliberately made
one service spend *more* time in malloc (2.7% → 3.5%), and that service still
got more requests per second and used less peak memory [12].

---

## 1. What an allocator is actually optimising

| Metric | What it means | Why it matters |
|---|---|---|
| Fast-path latency | Cycles for a `malloc`/`free` that hits a cache (~20 cycles in a good allocator [13]) | Small objects dominate: 98% of allocations are under 1 KB [2] |
| Application throughput | Requests per second of the real program | Allocator placement drives cache, TLB and NUMA locality |
| Peak and steady RSS | Physical memory used | Determines cost, container OOMs, and density |
| Fragmentation | External: free memory cached but unusable. Internal: rounding slack | 22% of the heap fleet-wide at Google (18.8% external + 3.4% internal) [2] |
| Tail latency | p99/p999 of `malloc`, including slow paths and syscalls | A slow-path `mmap` or page fault shows up in p99 latency |
| Scalability | Behaviour with 1 → 256+ threads and cores, chiplets, NUMA | Locks, atomics and cross-core cache-line traffic |
| Kernel friendliness | Syscall count, VMA count, TLB shootdowns, page faults, honesty of RSS, cgroup awareness | The allocator is a guest in the kernel's memory manager |
| Security | Resistance to heap exploitation (overflow, use-after-free, double free) | About half of Chrome's exploitable bugs were use-after-frees [9] |
| Usability | Drop-in use, sensible defaults, diagnosability | Most users never tune anything |

---

## 2. The landscape (2026)

| Allocator | Core idea | Strengths | Weaknesses |
|---|---|---|---|
| **glibc ptmalloc2** | Arenas (up to 8×cores), boundary-tag chunks with *inline* metadata, bins, per-thread tcache (since 2.26) | Everywhere; good enough for many programs | Arena bloat. Poor return of memory to the OS: `free()` only trims the top of the heap, and `malloc_trim` must be called by hand and has had arena bugs [6]. Inline metadata is the classic exploitation target. |
| **jemalloc** | Many arenas plus thread caches, extents, *time-decay* purging (dirty → muzzy → clean), rich statistics and profiling | Mature, observable, good at fragmentation | Many tuning knobs. Its future was uncertain: archived in June 2025, then revived by Meta in 2026 with release 5.3.1 [7]. |
| **TCMalloc (Google)** | Per-CPU caches via `rseq`, transfer cache, central free list, *hugepage-aware* page heap (Temeraire) | Best published warehouse-scale results: +7.7% RPS, 26% less fragmentation, 6% fewer TLB stalls [1] | Built for big Linux servers. Bazel/Abseil build. Per-CPU caches cost memory on many-core machines (cut from 3 MB to 1.5 MB per CPU in [2]). |
| **mimalloc** | *Free-list sharding*: many small free lists, one per 64 KiB page. Separate local and thread-free lists. Eager purging. | Small, very fast, strong benchmarks, secure mode costs about 10% [4] | v2's thread-local segments retained memory, so v3 reworked ownership to share pages between threads [4]. Its benchmarks are sometimes hard to reproduce [15]. |
| **snmalloc** | *Message passing*: remote frees are batched and sent to the owning allocator over lock-free queues. Out-of-band metadata. | Excellent for producer/consumer patterns, where thread caches fail [5]. CHERI support. | Less widely deployed; the design has changed a lot since its paper. |
| **Mesh** | *Compaction without moving*: merges two half-empty virtual pages onto one physical page | Works where nothing else can: Firefox −16% memory, Redis −39% [3] | Works on 4 KiB pages, which conflicts with hugepages. Randomised allocation costs locality. Research-grade. |
| **hardened_malloc / Scudo / PartitionAlloc** | Security first: out-of-line metadata, guard regions, quarantine, randomisation, MTE, MiraclePtr/BackupRefPtr | Turns many memory bugs into crashes. MiraclePtr mitigated 57% of the use-after-free bugs in its scope [9]. | Measurable CPU and memory cost. MiraclePtr cost 5.5–8% of browser-process memory on desktop [9]. |
| **Hoard / rpmalloc / others** | Hoard bounds "blowup" from cross-thread frees; rpmalloc is a lean, fast thread-cache design | Influential ideas | Older, or narrow in scope |

What these tell us:

- The best ideas already exist, but **no allocator combines all of them**.
  Hugepage awareness lives in TCMalloc, sharded free lists in mimalloc,
  message passing in snmalloc, decay purging and observability in jemalloc,
  meshing in Mesh, and hardening in Scudo and hardened_malloc.
- Very little of this is **adaptive to the environment**: cgroup limits,
  memory pressure, core count versus thread count, hugepage availability.
  Most of it is fixed tuning.

---

## 3. The biggest limitations of current allocators

### L1. Fragmentation from object lifetimes, not sizes

Size classes control *internal* fragmentation well. Most waste is
*external*: one long-lived object pins a whole span or hugepage. At Google,
51% of external fragmentation sits in the page heap and 29% in the central
free list [2].

Lifetimes vary enormously even within one size class: 46% of small objects
live under 1 ms, while 65% of objects over 1 GB live longer than a day [2].
Allocators group objects **by size**, but memory is freed **by lifetime**.
LLAMA showed that predicting lifetime cuts hugepage fragmentation by up to
78%, but it needed machine learning and offline profiles [10].

### L2. Memory is not given back, or not given back honestly

- glibc keeps freed memory inside its arenas, and RSS stays high [6].
- Caches multiply with the number of threads or CPUs.
- `MADV_FREE` is cheap, but RSS does not drop until the kernel reclaims the
  page. Monitoring, containers and OOM logic all read RSS. Go switched its
  default back to `MADV_DONTNEED` (Go 1.16) for exactly this reason.
- `MADV_DONTNEED` reports RSS accurately, but costs page faults and zeroing
  when the memory is reused [8].

### L3. Hugepages fight memory return

A 2 MiB hugepage gives a large TLB win, but giving back even 4 KiB of it
breaks it into small pages. Temeraire's fix is to pack allocations onto the
fullest hugepages so that empty ones can be released whole [1][16]. Most
allocators are still hugepage-oblivious. Newer kernels add mTHP (multi-size
THP, 16 KiB–1 MiB folios) and `MADV_COLLAPSE` [17], which almost no
allocator uses deliberately yet.

### L4. Cross-thread frees (producer/consumer)

Thread caches assume that the thread that allocates an object also frees
it. Pipelines break that assumption. Memory piles up in the consumer's cache
("blowup", as the Hoard paper called it), or every free pays for an atomic
operation. snmalloc batches thousands of remote frees per atomic operation
[5]; mimalloc uses one atomic compare-and-swap (CAS) per remote free into a
per-page list [4]. Neither strategy is best in all cases.

### L5. The fast path is solved; locality is not

A cache hit is about 20 cycles [13]. TCMalloc spends 16% of its malloc
cycles on *prefetching* the next object, because cache misses in the
application, not instructions in malloc, are the real cost [2].

Other things that matter more than allocator instruction count:

- The order in which free objects are handed out.
- Whether consecutive allocations land on the same cache line and page.
- Whether memory crosses chiplet boundaries (NUCA). NUCA-aware caches gave
  up to 3.8% throughput [2].

### L6. The C API is information-poor

`malloc(size)` tells the allocator nothing about lifetime, thread affinity,
NUMA node, or whether the memory needs zeroing. `free(p)` makes the allocator
look the size up again. C23 added `free_sized` and `free_aligned_sized`, and
C++ has sized delete. Size feedback, meaning how much space the program
*actually* got, is still only a proposal (P0901) [18], so programs misuse
`malloc_usable_size` instead. The allocator is forced to guess.

### L7. Security vs speed

- Inline metadata, as in glibc, is the root of whole exploitation families.
- Full hardening (guard pages per slab, quarantine, randomisation) costs
  10% or more in CPU, plus extra memory [4][9].
- Users choose either "fast" or "safe"; nothing offers sensible graduated
  tiers.
- Old-style guard pages are separate `PROT_NONE` mappings that consume VMAs.
  Linux 6.13 added `MADV_GUARD_INSTALL`, which avoids that [19].

### L8. Kernel interaction costs

- `mmap` and `munmap` contend on the process's `mmap_lock`.
- `munmap` and `MADV_DONTNEED` trigger **TLB-shootdown inter-processor
  interrupts (IPIs)** to every CPU running the process. This is expensive on
  large machines.
- Each first touch costs a page fault plus zeroing.
- `vm.max_map_count` (default 65530) limits allocators that use many
  mappings.
- Allocators rarely respect cgroup `memory.high`/`memory.max` or memory
  pressure (PSI).

### L9. Per-thread vs per-CPU caches

- **Per-thread caches** waste memory when a program has thousands of mostly
  idle threads.
- **Per-CPU caches** (`rseq`) waste memory when a process has 4 threads on a
  256-CPU machine, and they are Linux-only.
- glibc ≥ 2.35 registers `rseq` itself, so the allocator has to share that
  registration through `__rseq_offset`.

### L10. Tuning, observability and benchmarking

- jemalloc and TCMalloc expose dozens of knobs that almost nobody sets.
- When RSS is high, users cannot easily tell *where* the memory is: in use,
  cached, fragmented, or released-but-counted.
- Published allocator benchmarks are often irreproducible [15].

### L11. Operational edge cases

- **Thread exit with live objects**: abandoned heaps must be reclaimed by
  other threads.
- **`fork()`**: locks must be held across fork, or the child deadlocks.
- **Early initialisation**: `malloc` can be called before thread-local
  storage (TLS) exists.
- **Signal safety**: a signal can arrive in the middle of an allocation.
- **`dlopen` and interposition**, including mixing allocators.
- **`calloc`**: it zeroes memory the kernel already zeroed.
- **`realloc` on large blocks**: it copies even though `mremap` exists.

---

## 4. Requirements for sohamalloc

1. **Optimise application throughput and RSS together.** Never report
   allocator nanoseconds alone.
2. **Bounded memory overhead with no tuning.** Cache sizes scale with
   *active* threads and CPUs, and free memory goes back to the OS
   automatically, driven by both time and memory pressure.
3. **Group objects by size and expected lifetime.**
4. **Hugepage-aware by default**, falling back gracefully when THP is
   disabled.
5. **Kernel-friendly**:
   - Few large virtual memory reservations (VMAs).
   - Batched `madvise` calls, made off the fast path.
   - Lightweight guard pages.
   - Respect for cgroup limits and PSI.
   - No busy background thread.
6. **Secure by default at near-zero cost**, with stronger tiers available
   when wanted.
7. **Drop-in and explainable**:
   - Works with `LD_PRELOAD` or static linking.
   - Covers the full libc and C++ allocation surface.
   - One env var picks a profile.
   - A human-readable "where is my memory" report.
8. **Correct in the edge cases**: fork, signals, early init, thread exit,
   `dlopen`.

---

## 5. Proposed architecture

```
          malloc / free / new / delete  (+ extended API: sized free, size feedback, hints)
                                   │
 ┌─────────────────────────────────▼──────────────────────────────────────┐
 │ FRONT END   per-"slot" caches                                          │
 │             slot = rseq mm_cid (Linux ≥ 6.3) │ CPU id │ thread (fallback) │
 │             • size class from lookup table (branch-light)              │
 │             • pop/push on a sharded free list of the current page      │
 │             • prefetch the next object                                 │
 └───────────────┬───────────────────────────────────────▲────────────────┘
                 │ refill / flush (batched)              │
 ┌───────────────▼───────────────────────────────────────┴────────────────┐
 │ MIDDLE      per-LLC-domain transfer caches (NUCA-aware)                │
 │             remote frees: per-page atomic list  ⇄  batched message      │
 │             queue (switches adaptively per heap)                       │
 └───────────────┬────────────────────────────────────────────────────────┘
                 │ spans (pages of one size class)
 ┌───────────────▼────────────────────────────────────────────────────────┐
 │ SPAN LAYER  occupancy-prioritised span lists (allocate from fullest)   │
 │             lifetime classes: SHORT │ NORMAL │ LONG (hints + sampled    │
 │             call-site predictor)                                       │
 └───────────────┬────────────────────────────────────────────────────────┘
                 │
 ┌───────────────▼────────────────────────────────────────────────────────┐
 │ PAGE HEAP   hugepage-aware: 2 MiB blocks, filled densest-first         │
 │             large objects: dedicated runs, mremap for realloc          │
 │             out-of-line metadata (separate region, never next to data) │
 └───────────────┬────────────────────────────────────────────────────────┘
                 │
 ┌───────────────▼────────────────────────────────────────────────────────┐
 │ OS LAYER    few huge VA reservations; commit by touching                │
 │             release policy: decay timer + PSI/cgroup pressure           │
 │             MADV_DONTNEED │ MADV_FREE (by mode) │ whole-hugepage release │
 │             MADV_HUGEPAGE / MADV_COLLAPSE for hot blocks               │
 │             MADV_GUARD_INSTALL guard pages (6.13+, else PROT_NONE)     │
 └────────────────────────────────────────────────────────────────────────┘
```

### 5.1 Front end

- **Size classes**: about 4 per power of two, so internal waste is at most
  about 12.5%. Classes are exact for 8 KiB and below, and a 1-byte-indexed
  table maps sizes up to 1 KiB. Anything above about 256 KiB goes straight
  to the page heap.
- **Free-list sharding, as in mimalloc**: a cache holds a *current page* per
  size class and pops from that page's local free list.
  - Consecutive allocations stay close together in memory (L5).
  - A page that becomes empty is found immediately, without scanning.
- **Cache slot selection, the new part.** Index caches by the kernel's
  `rseq` **`mm_cid`** (memory-map concurrency ID, Linux 6.3+). Its values are
  dense and bounded by min(#threads, #CPUs).
  - A 4-thread process on a 256-CPU machine gets 4 caches, not 256.
  - A 10,000-thread process gets at most #CPUs caches.
  - This addresses L9 directly.
  - Fallbacks: CPU id, then per-thread caches on other OSes or older
    kernels.
  - Shares the `rseq` registration with glibc through `__rseq_offset`.
- **Elastic cache capacity**: a cache that misses often steals capacity from
  idle caches, like TCMalloc's dynamic per-CPU caches [2]. The total front-end
  budget is a fixed fraction of the heap, with a floor and a ceiling.

### 5.2 Remote frees, done adaptively

- **Default**: push onto the page's atomic thread-free list (one CAS, as in
  mimalloc). The owner collects these lazily.
- If a heap's remote-free rate passes a threshold (a producer/consumer
  pattern), switch that heap to **batched message passing** as in snmalloc:
  the freeing thread buffers objects by owner and sends one batch per atomic
  operation.
- Measure both against the "xmalloc-test" and "larson" benchmarks.

### 5.3 Lifetime-aware placement (main research bet, addresses L1)

1. **Explicit hints**, for the API:
   `soham_malloc_hint(size, SOHAM_SHORT_LIVED | SOHAM_LONG_LIVED)`.
2. **A cheap automatic predictor.** On each *sampled* allocation (about 1 in
   every 2 MiB allocated, the same sampler as the heap profiler), record
   `hash(__builtin_return_address(0))` and, at free time, the object's
   lifetime. Keep a small, fixed-size table:

   | call-site hash | lifetime class |
   |---|---|

   On the fast path, look up the return address in that table. If it is not
   there, use NORMAL.
3. Each lifetime class gets its own spans and hugepages. Short-lived objects
   share pages that empty out quickly and can be released whole. Long-lived
   objects are packed densely so they never pin a mostly-empty page.
4. **Safety valve**: if predictions turn out wrong (a SHORT span is still
   live after T seconds), demote that call-site. Predictions only affect
   *placement*, never correctness.

This is LLAMA's idea [10] without the neural network and without offline
profiling.

It must be validated, not assumed. The return-address lookup costs a few
cycles and has to pay for itself in RSS and TLB savings. If it does not,
compile it out.

### 5.4 Span and page heap: hugepage-aware (L1, L3)

- **Occupancy-prioritised span lists**: about 8 lists by the number of live
  objects. Allocate from the *fullest* span first, so nearly-empty spans
  drain and can be released. In the Google fleet this saved 1.41% of fleet
  memory [2].
- **2 MiB "hugeblocks"** are filled densest-first, as in Temeraire [1].
  Releasing memory **prefers whole hugeblocks**, and only falls back to
  sub-releasing 4 KiB runs under real pressure (adaptive subrelease [16]).
- **Large objects** (above about 256 KiB) get their own page runs, but
  still inside the reserved region, with no `mmap` per object. `realloc` of
  large blocks uses `mremap` to grow without copying.
- **Metadata always out-of-line**, in a separate reserved region:
  - A heap overflow cannot corrupt allocator state.
  - Data pages stay clean, so releasing them needs no metadata
    housekeeping.
  - It is cheap: snmalloc fits each 64 KiB slab's metadata in 64 bits [5].

### 5.5 OS layer: the "kernel-friendly" part (L2, L8)

| Concern | Policy |
|---|---|
| VMA count | Reserve a few large regions (for example 64 GiB of virtual address space) at start-up; commit by touching. Ideally fewer than 10 VMAs for the whole heap. |
| Syscall cost | No `mmap`/`munmap` on any steady-state path. All `madvise` calls are batched and coalesced into large ranges. |
| TLB shootdowns | Release in batches from one maintenance point, not one `madvise` per `free`. Prefer releasing whole hugepages, which is one IPI round for 2 MiB. |
| RSS honesty | **Mode-dependent**: `MADV_DONTNEED` by default (RSS drops, container-safe). `MADV_FREE` in the `throughput` profile or when there is no cgroup limit. |
| When to release | *Two triggers*: (a) a time-decay curve for idle free memory, as in jemalloc; (b) **pressure feedback**: read the cgroup's `memory.high`/`memory.max`/`memory.events` and `/proc/pressure/memory` (PSI). Near the limit or under pressure, release aggressively and shrink caches. With plenty of headroom, keep memory warm. |
| Background work | No always-on thread. Maintenance runs piggy-backed on slow paths (amortised), plus one optional, mostly-sleeping thread that wakes on a PSI trigger fd. |
| Hugepages | `MADV_HUGEPAGE` on the heap region. Use `MADV_COLLAPSE` (6.1+) for hot, dense hugeblocks when THP is set to `madvise`. Detect THP mode at start-up and adapt. |
| Guard pages | `MADV_GUARD_INSTALL` (6.13+) gives guard regions without extra VMAs [19]; fall back to `PROT_NONE` on older kernels. |
| `calloc` | Track **known-zero** pages. Fresh pages and pages released with `DONTNEED` are already zero, so skip `memset`. Pages released with `MADV_FREE` are not guaranteed zero. |
| NUMA / NUCA | Transfer caches per last-level-cache domain [2]. Node from `rseq`'s `node_id`. Optional `mbind` per node arena. |
| `fork` | `pthread_atfork` handlers take all allocator locks. The child resets per-slot caches. |

### 5.6 Security tiers (L7)

| Tier | Contents | Target cost |
|---|---|---|
| **0 – default** | Out-of-line metadata. Free-list pointers XOR-encoded with a per-process secret (like glibc's "safe-linking"). Cheap double-free detection through a per-span free bitmap. Sampled guard allocations (GWP-ASan style: about 1 in 10⁵ allocations is placed next to a guard page) to catch real bugs in production. | ≤ 1–2% |
| **1 – hardened** | Plus randomised free-list order, guard pages between spans, a small use-after-free quarantine, and zero-on-free. | about 10% |
| **2 – paranoid** | Plus memory tagging (Arm MTE) where available, a larger quarantine, and a guard page per large allocation. | debugging and high-risk use |

### 5.7 Usability and observability (L10)

- **Drop-in**:
  - `LD_PRELOAD=libsohamalloc.so`, or static linking.
  - Overrides all of: `malloc`, `free`, `calloc`, `realloc`,
    `reallocarray`, `aligned_alloc`, `posix_memalign`, `memalign`, `valloc`,
    `pvalloc`, `malloc_usable_size`, `free_sized`, `free_aligned_sized`, and
    every C++ `new`/`delete` overload (sized and aligned).
- **One knob**: `SOHAMALLOC=balanced|throughput|memory|latency|secure`.
  Advanced settings live in `SOHAMALLOC_OPTS`, but the defaults must be good.
- **Extended API**:
  - Size feedback: `soham_malloc_sized(size, &actual)`.
  - Lifetime hints.
  - **Heaps/arenas** with bulk destroy, for request-scoped memory.
  - `soham_release(bytes)`.
  - `soham_stats()`.
- **"Explain my memory" report** (on demand, on a signal, or at exit):

  ```
  RSS 812 MiB = in-use 640 | front-end caches 22 | middle 9 | spans partially used 71
              | page heap free-not-released 58 | metadata 12
  hugepage coverage 87% | top fragmenting call-sites: ...
  ```

- **Sampling heap profiler**, always available: one sample per about 2 MiB
  allocated, with stack traces, as in TCMalloc. Leak and peak-heap reports
  come from the same samples.

---

## 6. What would make sohamalloc genuinely different

Each idea below has precedent somewhere. The contribution is putting them
together, adapting them at runtime, and making the result explainable:

1. **Pressure-adaptive memory return.** Uses cgroup limits and PSI, not just
   timers, so the allocator behaves well in containers.
2. **`mm_cid`-indexed caches.** Per-CPU speed without paying for idle CPUs.
3. **Lifetime-aware placement without ML.** Explicit hints plus a sampled
   call-site predictor.
4. **Hugepage-first page heap plus occupancy-prioritised spans**, inside a
   small, portable C codebase rather than a large framework.
5. **Adaptive remote-free strategy**: per-page CAS or batched messages,
   chosen per heap.
6. **Security tier 0 that is basically free**, plus production-safe sampled
   guard allocations.
7. **Kernel citizenship as a design goal**: few VMAs, batched IPIs,
   lightweight guards, honest RSS.
8. **Explainability**: the RSS breakdown report.

Be realistic. Beating mimalloc or TCMalloc on *microbenchmarks* is very
hard, since their fast paths are already near the hardware limit. The
winnable goal is being on or near the best trade-off point for *real
programs*: comparable speed, lower and more predictable RSS, better
behaviour in containers, and cheaper security.

---

## 7. How to measure (and keep it honest)

- **Benchmarks**:
  - `mimalloc-bench`, a suite that bundles larson, xmalloc-test,
    cache-scratch, mstress, rptest, glibc-bench, espresso and others.
  - Real programs: Redis, RocksDB `db_bench`, a C++ HTTP server, the Lua or
    CPython test suites, and clang compiling a large file.
- **Metrics per run**:
  - Wall time and throughput.
  - Peak RSS and RSS over time.
  - p99 allocation latency.
  - `perf stat` counts: dTLB misses, LLC misses, page faults, context
    switches.
  - Syscall and `madvise` counts (`strace -c`).
  - VMA count (`/proc/self/maps`).
- **Comparison set**: glibc, jemalloc 5.3.1, TCMalloc, mimalloc v3, and
  snmalloc. All are pinned versions, built with the same flags, on the same
  pinned kernel, with the CPU governor set to `performance`. Run each
  benchmark at least 10 times and report confidence intervals.
- **Container scenario**: the same workloads under `memory.max` set to 1.2×
  the working set. Count OOM kills and measure throughput.
- Publish scripts so anyone can reproduce the numbers, because
  irreproducible allocator benchmarks are a known problem [15].

---

## 8. Roadmap

| Phase | Deliverable | Exit criterion |
|---|---|---|
| 0 | Single-threaded core: size classes, page/span layer, out-of-line metadata, large objects, full libc API, test suite (randomised stress, alignment, `realloc` edge cases, fork) | Passes stress tests; runs `ls`, `python`, `gcc` under `LD_PRELOAD` |
| 1 | Multithreading: per-thread caches, per-page remote-free lists, abandoned-heap reclamation | Larson and xmalloc-test scale linearly to core count |
| 2 | OS layer: hugeblocks, densest-first fill, decay plus PSI/cgroup release, batched `madvise`, known-zero `calloc` | RSS at or below jemalloc on Redis and RocksDB |
| 3 | `rseq` `mm_cid` caches, elastic capacity, NUCA transfer caches | Throughput within about 5% of TCMalloc and mimalloc |
| 4 | Security tiers 0–2, sampled guard allocations, statistics and the explain report, heap profiler | Tier 0 costs ≤ 2% |
| 5 | Research: lifetime predictor, adaptive remote-free switching, optional meshing for very long-running servers | Measurable fragmentation reduction, or remove the feature |

**Language**: C11 with `<stdatomic.h>`, no dependencies, and a CMake build.
This is the most portable and least surprising choice for a drop-in
allocator. Keep every OS-specific call behind a small `os_linux.c` /
`os_posix.c` layer.

---

## 9. Risks and open questions

- **Predictor overhead vs benefit.** Unknown until measured; it is the
  biggest research risk.
- **`MADV_DONTNEED` by default** trades some throughput for RSS honesty.
  The profile system has to make that trade visible.
- **`rseq` complexity.** Restartable sequences need hand-written assembly
  per architecture. Start with x86-64 and arm64, and keep per-thread caches
  as the reference implementation.
- **Meshing** requires shared file-backed (memfd) mappings, which interact
  badly with THP and `fork`. It should stay an opt-in experiment.
- **Lifetime grouping vs size-class density.** More span types mean more
  partially filled spans. This needs careful limits.

---

## References

1. Hunter et al., "Beyond malloc efficiency to fleet efficiency: a hugepage-aware memory allocator" (TCMalloc/Temeraire), OSDI 2021. https://research.google/pubs/beyond-malloc-efficiency-to-fleet-efficiency-a-hugepage-aware-memory-allocator/ · https://usenix.org/conference/osdi21/presentation/hunter
2. Zhou et al., "Characterizing a Memory Allocator at Warehouse Scale", ASPLOS 2024. https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf
3. Powers, Tench, Berger, McGregor, "Mesh: Compacting Memory Management for C/C++ Applications", PLDI 2019. https://arxiv.org/abs/1902.04738
4. mimalloc README and design notes. https://microsoft.github.io/mimalloc/index.html · https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md
5. Liétar et al., "snmalloc: A Message Passing Allocator", ISMM 2019. https://www.microsoft.com/en-us/research/uploads/prod/2020/04/snmalloc.pdf
6. glibc memory bloat and `malloc_trim`: Vertica docs https://docs.vertica.com/12.0.x/en/admin/managing-db/memory-trimming · glibc bug 30769 https://sourceware.org/bugzilla/show_bug.cgi?id=30769
7. jemalloc archive and revival: https://lists.freebsd.org/archives/cu/2025-June/007847.html · https://theconsensus.dev/p/2026/04/16/who-even-uses-jemalloc-anyway.html
8. MADV_FREE design discussion: https://lwn.net/Articles/590693 · https://lore.kernel.org/all/56399D66.5010606@gmail.com/
9. MiraclePtr: https://security.googleblog.com/2022/09/use-after-freedom-miracleptr.html · https://security.googleblog.com/2024/01/miracleptr-protecting-users-from-use.html
10. Maas et al., "Learning-based Memory Allocation for C++ Server Workloads" (LLAMA), ASPLOS 2020. https://cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf
11. Kanev et al., "Profiling a warehouse-scale computer", ISCA 2015. https://research.google/pubs/profiling-a-warehouse-scale-computer/
12. Google Cloud blog, "Trading off malloc costs and fleet efficiency". https://cloud.google.com/blog/topics/systems/trading-off-malloc-costs-and-fleet-efficiency/
13. Kanev et al., "Mallacc: Accelerating Memory Allocation", ASPLOS 2017. https://vlsiarch.eecs.harvard.edu/publications/mallacc-accelerating-memory-allocation
14. Robson-bound discussion: https://arxiv.org/pdf/2608.28462 · https://www.sqlite.org/matrix/ev/src/mem5.html
15. "Irreproducible malloc benchmarks". https://paper.dropbox.com/doc/Irreproducible-malloc-benchmarks--BW~~drKA3xsbBtf9XP2xtvSRAQ-xgvyGU2f6EJ14r5oqHrn5
16. "Adaptive Hugepage Subrelease for Non-moving Memory Allocators in Warehouse-Scale Computers". https://research.google/pubs/pub50436/
17. Multi-size THP for anonymous memory (Ryan Roberts patch series). https://lkml.iu.edu/hypermail/linux/kernel/2312.0/08512.html
18. P0901 "Size feedback in operator new". https://wg21.link/p0901r9
19. Lightweight guard pages (`MADV_GUARD_INSTALL`), Linux 6.13. https://lwn.net/Articles/1000908/
20. TCMalloc rseq per-CPU caches. https://google.github.io/tcmalloc/rseq.html
