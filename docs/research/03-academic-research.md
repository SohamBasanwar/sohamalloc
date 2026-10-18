# Allocator research 2017–2026: what sohamalloc should take from it

**Scope and method.** This survey covers peer-reviewed and preprint work from 2017 to 2026 on dynamic memory allocation, plus industrial sources such as TCMalloc's source code and docs, LLVM docs, the Apple Security blog and kernel docs. I ran about 30 web searches. I read the full text, extracted with pdftotext, of: Temeraire, ASPLOS'24 TCMalloc, the LLAMA CACM paper, Mesh, MineSweeper, DangZero, BUDAlloc, Cornucopia Reloaded, Memento, NUMAlloc, BatchIt, Reconsidering-Reconsidering, Exgen-Malloc, the 2026 tight-bounds paper and three MTE papers. Anything I saw only as an abstract, search snippet or slide is labelled. Anything I could not check is marked **UNVERIFIED**. All numbers are the authors' own unless I say otherwise.

---

## 1. Fragmentation theory

**Bender, Conway, Farach-Colton, Komlós, Kuszmaul, Wein, "Tight Bounds for Memory Allocation With and Without Request Fragmentation", arXiv 2608.28462, 28 Aug 2026 (marked "in submission", not yet peer-reviewed).** [S1]

- **Key idea.** Classical allocation, where each request gets contiguous slots, has an optimal deterministic competitive ratio of Θ(log M). Robson and Woodall showed this; M is the volume high-water mark.
  - The paper notes that real systems split requests into pieces: the OS maps a virtual range onto non-contiguous physical pages.
  - It defines **k-aggregate fragmentation**: the peak number of live fragments may be at most k times the peak number of live requests.
- **Results** (all tight, with matching upper and lower bounds):

  | Model | Optimal competitive ratio |
  |---|---|
  | No fragmentation, deterministic or randomized | Θ(log M). This closes the old Ω(log M / log log M) randomized gap: randomization gives no asymptotic advantage. |
  | k-per-request fragmentation | Θ(log_{k+1} M) |
  | k-aggregate, k ≥ 2 | Θ(log_k log M) |
  | (1+ε)-aggregate | Θ(log log M + log ε⁻¹) |
  | M known in advance, k = Θ(1) | O(1) |

  - The authors also note that "Best Fit … can incur … Ω(M); while First-Fit … achieves … O(log M)" [S1].
  - Their stated motivation: the fragment index is the page table, which "should ideally also fit in a relatively small hardware cache" [S1].
- **Limitations.**
  - It is worst-case, competitive-ratio theory.
  - It does not model sub-page objects, which cannot be split across pages, and it does not model TLB cost beyond fragment count.
- **What it means for sohamalloc.** This is the most useful theory result for the design.
  - Fragmentation through virtual memory is the real escape hatch from Robson's bound. Its "budget" k is page-table and TLB pressure.
  - That is exactly the hugepage-versus-subrelease trade-off. Breaking a 2 MiB page into 4 KiB pieces raises k. Packing into whole hugepages lowers k and pushes the allocator back toward Robson behaviour.
  - Randomized placement alone, without remapping, cannot beat Θ(log M).
  - My inference, not a claim in the paper: Mesh can beat the bound only because meshing is remapping, i.e. fragmentation in physical space.

**Lamprakos et al., "The Unexpected Efficiency of Bin Packing Algorithms for Dynamic Storage Allocation in the Wild", ISMM'23.** [S2]

- On 8 real application traces with 4 allocators, the allocators beat Robson's worst-case lower bound in 93.75% of cases.
- GNU malloc matched or beat an *offline* 2D bin-packing algorithm on makespan in 87.5% of cases. The bin-packing algorithm did give up to 2.46× better fragmentation.
- **Lesson.** Worst-case bounds rarely bind on real traces. Real waste comes from mixing lifetimes and from caching policy, which is the subject of section 2.

## 2. Lifetime prediction

**LLAMA (Maas et al., ASPLOS'20), and its CACM Research Highlight, DOI 10.1145/3611018 (CACM issue date UNVERIFIED).** [S3][S4]

- **Key idea.**
  - An LSTM, trained on sampled stack traces from earlier runs, predicts lifetime classes that are 10× apart.
  - The heap is organised by lifetime class, not size class. Hugepages are split into 8 KB blocks and lines.
  - Free blocks in long-lived pages are refilled with shorter-lived objects.
  - Mispredictions are handled with deadlines (deadline = now + K × LC, with K = 2), escalation of the lifetime class, and reclassification.
  - Predictions are cached in a thread-local hash keyed on return address, stack height and object size.
- **Results** [S4].
  - Fragmentation reductions versus TCMalloc: 43% (image server), 19% (TensorFlow), 78% (data pipeline), 73% (Redis).
  - Weighted prediction accuracy was 96–100%.
  - 95% of predictions hit the cache. Cached values disagreed with fresh predictions 14% of the time, but only 1.6% of allocation sites needed escalation.
- **Costs** [S4].
  - TCMalloc's fast path is 8.3 ns. Capturing a full stack takes 396 ± 364 ns, and the stack-hash lookup 22.5 ns.
  - LLAMA's fast path is 48.8 ns, against 8.3 ns for TCMalloc. The image server ran about 12.5% slower per query.
  - One model inference takes 100–500 µs.
  - Exact stack traces match poorly across binary versions: 58.31% for revisions one week apart, 0.38% five months apart. Stack height alone identifies stacks with 68% accuracy.
- **Limitations.**
  - The TCMalloc baseline was *not hugepage-aware* when the paper was written. Its occupancy was reconstructed as a lower bound [S4].
  - The prototype used a central lock.
  - Offline training is needed.
- **Follow-ups.**
  - Google applied the insights to hugepage break-up decisions instead, for an estimated 1% fleet throughput gain [S4][S6].
  - TCMalloc documents an experimental **lifetime-based allocator**: an online `LifetimeDatabase` keyed by stack trace and binary short/long classes with T = 0.5 s. It applies only to *large* allocations. It has a "counterfactual" mode that measures benefit without changing placement [S7].
    - I found no source file with "lifetime" in its name in TCMalloc HEAD (commit 1c7cf24, checked 2026-10-18). `huge_page_aware_allocator.h` contains no predictor [S8].
    - The feature therefore looks removed or never open-sourced. **UNVERIFIED**, and I could not find the reason.
- **Lightweight non-ML approaches.**
  1. **Span capacity as a lifetime proxy** (ASPLOS'24) [S5].
     - Span capacity (objects per span) correlates with span return rate at Spearman −0.75.
     - Spans of capacity ≤ 16 and > 16 go to separate hugepage sets.
     - Fleet result: +1.02% throughput, −0.82% memory, hugepage coverage up from 54.4% to 56.2%, dTLB misses down 8.1%.
     - It costs nothing at runtime. TCMalloc HEAD still separates "sparsely-accessed" from "densely-accessed" hugepages through `AccessDensityPrediction` [S9].
  2. **LLVM MemProf.** Profile-guided hot/cold/notcold labels per allocation context, rewritten into `operator new(…, hot_cold_t)` calls (0 = cold, 255 = hot), aimed at TCMalloc's API. Seeing through allocation wrappers requires ThinLTO. The docs cite no performance numbers [S10].
  3. **Clang AllocToken** (`-fsanitize=alloc-token`, section 9). It gives each allocation a static per-type or per-site ID at compile time [S11].
- **Adopt?** Yes, in this order:
  - First, the zero-cost span-capacity split.
  - Then hint APIs (MemProf hot/cold, tokens).
  - Online stack-keyed prediction only for medium and large allocations, where its cost is amortised, and always shipped with a counterfactual mode.

## 3. Compaction without moving objects

**Mesh (Powers, Tench, Berger, McGregor, PLDI'19).** [S12]

- **Key idea.**
  - Randomized allocation through shuffle vectors makes spans likely to be "meshable": two spans whose live objects do not overlap.
  - SplitMesher probes random pairs and copies one span into the other. The two virtual pages are then remapped onto one physical page.
  - The arena is backed by `memfd_create`. Concurrent writes are blocked with `mprotect` plus a SIGSEGV write barrier.
  - Meshing is rate-limited, by default to at most once per tenth of a second.
- **Results.**
  - Firefox: −16% memory with less than 1% change in Speedometer. Redis: −39%.
  - SPECint: geomean −2.4% memory and +0.7% time versus glibc.
  - With randomization disabled: 4% overhead and only a 3% heap reduction.
  - The evaluation ran on a MacBook Pro with 16 GiB [S12].
- **Limitations.**
  - It works at 4 KiB granularity.
  - The heap must be a shared file mapping. That brings shmem THP policy, which is controlled separately through `shmem_enabled` [S13]. It also raises fork semantics questions (**UNVERIFIED** how Mesh handles fork).
  - The SIGSEGV handler can collide with an application's own signal handlers.
  - My analysis: each remap can split VMAs.
- **Follow-ups.** I found no 2020–2026 peer-reviewed follow-up on meshing in C/C++ allocators (a negative search result, so **UNVERIFIED**). The GitHub repo says it "runs on Linux and macOS" [S14].
- **Adopt?** Only as an opt-in for long-running servers, and only on spans that already live in broken-up (4 KiB-backed) hugepages, so it never fights densely packed hugepage memory.

## 4. Hugepage-aware allocation

**Temeraire (Hunter et al., OSDI'21).** [S15]

- **Key idea.** The page heap is split into a HugeFiller (best-fit, densest-first), a HugeCache, and HugeRegions for awkward sizes.
- **Results.**
  - Eight applications: mean +7.7% throughput, −2.4% RAM.
  - Fleet: TLB-stall cycles fell from 21.6% to 20.3% (a 6% reduction). Page-heap overhead fell from 14.3% to 10.6% (a 26% reduction).
  - Hugepage coverage rose from 11.8% to 23% for applications that release memory, and from 44.3% to 67.3% for those that do not.
  - One service spent more time in malloc (2.7% → 3.5%) and still gained 3.42% RPS and 6.5% peak memory.
  - Only 8.8% of programs trigger HugeRegions.
- **Limitation.** Coverage is "still only 67%" [S15].

**Adaptive Hugepage Subrelease (Maas et al., ISMM'21).** [S6][S16]

- **Key idea.** A "realized fragmentation" metric. Skip subrelease when free pages are at or below recent demand, defined as a short-term fluctuation peak plus a long-term trend.
- TCMalloc's `huge_page_subrelease.h` also tracks whether each skipped release later proved correct [S16].
- **Result.** About 1% fleet throughput "at negligible memory overhead" [S6].

**ASPLOS'24 TCMalloc characterization (Zhou et al.).** [S5]

- **Fleet profile.** Malloc takes 4.3% of fleet cycles. Fragmentation is 22.2% of the heap (18.8% external, 3.4% internal), with 51% of it in the page heap and 29% in the central free list. The HugeFiller holds 83.6% of in-use page-heap memory and 94.4% of page-heap fragmentation.
- **Optimizations and measured gains.**

  | Change | Measured result |
  |---|---|
  | Per-CPU cache default cut from 3 MB to 1.5 MB, made usage-sized | −1.94% fleet memory, no performance loss |
  | NUCA-aware transfer caches | −4.37% LLC load misses, +0.32% throughput, +0.10% memory |
  | Span prioritization: L = 8 lists, list index max(0, L − log₂ A) | −1.41% fleet memory |
  | Lifetime-aware filler | See section 2 |
  | All of the above combined | +1.4% throughput, −3.4% RAM |

- Note that the paper itself is inconsistent on the combined memory figure: 3.4% in the abstract, 3.5% in section 4.5 [S5].

**Kernel reality (not an allocator paper).**

- At LSFMM 2024 (reported by LWN), unpatched mTHP allocations succeeded about 50% of the time after one hour, and failed more than 90% of the time after two hours [S17].
- `MADV_COLLAPSE` (Linux 6.1+) collapses ranges into PMD hugepages regardless of sysfs settings [S13][S18].
- **Lesson.** Hugepages are a scarce shared resource that decays over time. Returning *whole* hugepages helps other processes as well.

**Adopt.** All of it: densest-first filling, span prioritization, the capacity-based lifetime split, and skip-subrelease. TCMalloc (Apache-2.0) is a working reference implementation.

## 5. Remote frees, NUMA and NUCA

**BatchIt (Filardo and Parkinson, ISMM'24).** [S19]

- **Key idea.** A small per-thread cache of remote frees, keyed by destination slab, so that each atomic operation hands back a batch of objects. Prototyped in both mimalloc and snmalloc.
- **Results.**
  - xmalloc-test: −9.9% time (mimalloc), −27.6% (secure mimalloc), +14.7% speed (snmalloc).
  - A back-pressured message-passing benchmark: up to 20% faster.
  - Regressions: glibc-thread −7.3% (mimalloc), sh8bench −8.3% (snmalloc), mstress −6.0% (secure mimalloc).
  - The authors warn that xmalloc-test has no back-pressure, so its memory numbers mislead.
- **Adopt?** Yes. It is simpler than switching the v1 doc's remote-free strategy per heap.

**NUMAlloc (Yang et al., ISMM'23).** [S20]

- **Key idea.**
  - Bind each thread to a node, and bind one VA region per node with `mbind`.
  - The origin node of any object is then heap offset ÷ region size.
  - "Origin-aware" frees send each object back to its origin node.
  - "Incremental sharing" of THP between threads on the same node.
- **Results.** 15.7% faster than mimalloc and 20.9% faster than glibc, scaling to 128 threads. The paper says a syscall to query a page's node costs about 10,000 cycles.
- **Limitation.** The allocator *changes thread affinity*, which a drop-in default must not do.
- **Adopt?** Take the address-derived origin node and the origin-aware frees. Get the current node from rseq instead of binding threads. TCMalloc also has a NUMA-aware mode (`want_numa_aware.cc`) [S8].

## 6. Hardware acceleration

| Work | Idea | Results | Limitations |
|---|---|---|---|
| **Mallacc** (ASPLOS'17) [S21] | In-core malloc cache for size-class lookup and free-list pops | Up to 50% lower malloc latency for under 1500 µm² of silicon (abstract via search snippet; full-program speedup **UNVERIFIED**) | New hardware |
| **Memento** (MICRO'23) [S22] | Hardware object allocator plus page allocator, with a main-memory bypass for newly allocated objects | Serverless functions: 8–28% faster (16% average), −30% memory traffic, −29% cost. User/kernel split of memory-management cycles shown as 48/52% for one workload class (Table 2) | Full-system simulation; ISA extensions |
| **NextGen-Malloc** (HotOS'23) [S23] | Run the allocator on its own core | Vision paper: up to 72% performance variation from the choice of allocator | No prototype |
| **SpeedMalloc** (arXiv 2508.20253, not peer-reviewed) [S24] | Lightweight support core keeps all allocator metadata | 1.15× over mimalloc, 1.18× over TCMalloc, 1.75× over jemalloc (gem5). L2 miss cycles −22.8% versus mimalloc at 16 threads | Simulation only. Needs attachment above the LLC, which current chips lack. 4 new ISA instructions. "Memento+" is the authors' own port. 1.5% area and 2.25% power on 16 cores |
| **Valinor** (arXiv 2607.14789, not peer-reviewed) [S25] | Programmable engine for *physical* page allocation (the minor-fault path) | Minor faults are up to 54% of runtime in short-lived workloads. 17× faster allocation, +16% end-to-end, up to −8% energy, on a BOOM RISC-V soft core | Kernel and hardware, not malloc |
| **PIM-malloc** (HPCA'26 per arXiv listing; not opened) [S26] | Allocation for processing-in-memory | 66× faster allocation (abstract) | Processing-in-memory hardware only |

- **Adoption.** None of these can be used directly. Three lessons carry over:
  1. Metadata pollutes the cache. Keep it compact, out-of-line and dense, as the v1 doc already proposes.
  2. In short-lived and bursty processes the kernel fault path dominates. Batched prefaulting with `MADV_POPULATE_WRITE` (Linux 5.14+) [S18], gated on pressure, is the software analogue.
  3. Specializing for one thread pays. Exgen-Malloc [S27] reports 1.17×, 1.10× and 1.93× over *dlmalloc* on SPEC2017, Redis and mimalloc-bench, and 6.2%, 0.1% and 25.2% memory savings over *mimalloc*.
     - Note the different baselines for speed and for memory.
     - Adoptable form: a lock-free, atomic-free mode until the first thread is created.

## 7. CXL, tiered and far memory

- **OBASE** (OSDI'26) [S28].
  - **Idea.** "Hotness fragmentation": in Google traces, up to 97% of the bytes in active pages are cold.
  - **Mechanism.** Pointer instrumentation plus lock-free migration of objects into hot and cold pages, feeding unmodified kernel tiering such as TPP and Memtis.
  - **Results.** 2–4× better page utilization and up to 70% smaller footprint, at 2–5% overhead.
  - **Limitation.** It needs a compiler and moves objects, so it is not drop-in.
- **AIM** (ISMM'26) [S29]. Bandwidth-driven dynamic interleaving across local DRAM and CXL. Bandwidth-bound applications ran up to 26% faster than the best previous bandwidth-aware tiering system (abstract).
- **Kernel support.** `MPOL_WEIGHTED_INTERLEAVE` interleaves pages by per-node weights set in sysfs [S30].
- **Carbink** (OSDI'22) [S31]. Far memory organized in spans, with erasure coding and remote compaction. Against Hydra: 29% lower tail latency, 48% higher application performance, at most 35% more memory.
- **Adopt?** Partly. Keep cold allocation sites, identified by MemProf hints or tokens, on separate spans and hugepages so the kernel can demote whole pages. Allow optional weighted interleave for large, bandwidth-heavy arenas. Do not move objects.

## 8. GPUs (low relevance)

Gallatin (PPoPP'24) uses a wait-free van Emde Boas tree. The PPoPP page claims it is up to 568× faster than the next-best GPU allocator; another abstract copy gives smaller numbers [S32]. There is no direct relevance to a CPU malloc.

## 9. Security research

**MTE tagging strategies.**

- **Partap and Boneh** (arXiv 2209.00307) [S33] surveyed how glibc, Scudo, SLUB, Chrome and LLVM tag memory.
  - Increment-by-one tagging can be exploited.
  - Scudo's odd/even scheme detects use-after-free with only about 7/8 probability.
  - glibc's metadata tagging had an exploitable weakness.
- **"ARM MTE Performance in Practice"** (arXiv 2601.11786) [S34] measured Pixel 8 and 9, AmpereOne and Apple M5.
  - Overheads are often modest, but slowdowns reach 6.64× on 456.hmmer on the Pixel Performance core in SYNC mode. That is a microarchitectural hazard which ASYNC mode does not show.
  - Tagging only allocations under 32 KiB, with guard pages for larger ones, cut 502.gcc from 1.25× to 1.17× on AmpereOne and from 1.29× to 1.08× on Apple M5.
- **AmpereOne paper** (arXiv 2511.17773) [S35].
  - Reports "single-digit" SYNC overhead, with application memory management as "the primary remaining source of overhead".
  - Eager tag initialization of large reservations "removes the benefits of lazy population".
  - Linking jemalloc, which does no tagging, recovered much of the loss.
- **TikTag** (arXiv 2406.08719) [S36] leaks MTE tags speculatively with more than 95% success in under 4 seconds.
- **Adopt** for an arm64 secure tier:
  - Random tags that exclude the neighbours' tags.
  - Retag on *free*.
  - Lazy tag initialization on first touch, never over a whole span.
  - A 32 KiB cutoff, with guard pages above it.
  - SYNC mode in the secure profile.
  - Treat MTE as probabilistic, never as a boundary.

**Type segregation.**

- **kalloc_type** (iOS 15) [S37].
  - The compiler gives each type a signature (pointer, data or padding per 8-byte granule).
  - 1,482 signature groups were assigned at random, per boot, to 200 zones.
  - Pure-data allocations live in a separate heap, and zones' virtual address ranges are sequestered.
  - The stated budget was "0% CPU and 0% memory". There are no measured numbers.
- **Memory Integrity Enforcement** (Sept 2025) [S38] adds **xzone malloc** (iOS 17) in user space and synchronous EMTE.
  - Apple claims "the same or better performance" but publishes no numbers.
  - xzone's internals are not public (**UNVERIFIED** beyond the blog post).
- **Clang AllocToken** [S11] makes this possible for C/C++ on Linux.
  - `malloc(n)` becomes `__alloc_token_malloc(n, id)`.
  - The default mode, `typehashpointersplit`, puts pointer-containing types in the top half of the ID space.
  - A "fast ABI" encodes the token in the function name.
  - Other modes give per-site IDs (`typefunchash`, `increment`).
  - It landed around October 2025, per cfe-commits [S39]. The first release that includes it is **UNVERIFIED**.
- **Adopt?** Yes, as a pointer-versus-data split with no runtime cost, and as stable keys for per-site tables. The limit: it only works for programs rebuilt with Clang, not binaries loaded through LD_PRELOAD.

**Use-after-free prevention.** Figures below are from MineSweeper's own comparison unless noted [S40].

| System | Approach | Time / memory overhead | Notes |
|---|---|---|---|
| **MarkUs** (S&P'20) | Quarantine plus Boehm-style marking | SPEC2006: +15.5% time (worst 2.97×), +12.3% memory | |
| **MineSweeper** (ASPLOS'22) | Quarantine, zero-on-free, concurrent linear sweeps, on jemalloc | SPEC2006: +5.4% time, +11.1% memory, +9.6% extra CPU. SPEC2017: +10.8% / +7.9%. mimalloc-bench stress: 2.7× time, 4.0× memory (worst 31× / 27×) | Its slowdowns come mostly from lost cache reuse, not from sweeping |
| **FFmalloc** (USENIX Sec'21) | One-time allocation | SPEC2017: +5.3% time, +22.2% memory, with memory growing over a run | Authors' slides: +2.3% SPEC, +22% PARSEC (MarkUs 43%), limited by mmap locking [S41]. BUDAlloc cites bloat up to 800% [S43] |
| **DangZero** (CCS'22) | Direct page-table access in a guest-mode "virtual process" | SPEC2006: +16% geomean, SPEC2017: +22% (alias-based state of the art: 40%). Nginx: −18% saturated throughput [S42] | Needs virtualization |
| **BUDAlloc** (USENIX Sec'24) | User-space allocator manages VA, with eBPF batching of unmaps in the page-fault handler | 15% faster than DangZero and 61% less memory than FFmalloc on SPEC2017; about 30% memory overhead [S43] | Needs kernel co-design |

- **Adopt?** Offer MineSweeper-style quarantine plus sweeping as an optional "UAF-safe" profile. Never make it the default.

**CHERI.**

- **Cornucopia Reloaded** (ASPLOS'24) [S44] adds a per-page capability load barrier on Morello. It nearly eliminates stop-the-world revocation pauses at no extra cost over Cornucopia, and uses a median 87% of Cornucopia's DRAM-traffic overhead.
  - It was evaluated with snmalloc. Worst cases: xalancbmk 29.4%, omnetpp 23.1%.
- **Adopt?** Requires CHERI hardware. The only practical step now is to keep metadata provenance-clean so a later CHERI port is possible.

**FRP, "Fully Randomized Pointers"** (ISMM'25) [S45]. Randomizes pointer bits. A hardware proof of concept shows under 10% overhead. It needs hardware, so it is out of scope.

## 10. Custom allocators and benchmarking methodology

- **van Kempen and Berger, ISMM'26** [S46].
  - Per-class allocators still give no real benefit.
  - Region allocators remain the only win, but their advantage shrank from up to 44% to at most 15% (mudlle).
  - **Adversarial allocation**: the heap is first preconditioned to M × the program's peak allocations at a chosen occupancy. Under that preconditioning, general-purpose allocation slows down by up to 2×, while region-based code is unaffected.
- **Fleetbench** (ISPASS'24) [S47] provides a TCMalloc "empirical driver" built from distributions derived from production data. It defines three fidelity levels, the strongest being that a change's effect on the benchmark matches its effect in production.
- **mimalloc-bench** [S48] is the standard stress suite: larson, xmalloc-test, mstress, sh6/sh8bench, cache-scratch, and real programs such as lean, redis and z3.
- **Methodology pitfalls seen in this survey:**
  - Fresh-heap bias (ISMM'26).
  - No back-pressure in xmalloc-test (BatchIt).
  - Different baselines for speed and memory (Exgen).
  - Simulated systems with self-ported baselines (SpeedMalloc).
  - Reconstructed lower-bound baselines (LLAMA).
  - Results available only from fleet A/B tests, with no public replication (ASPLOS'24, Temeraire).

---

## 11. Problems this research finds in the v1 design document

1. **§0 and reference [14] misstate the newest theory.** The v1 doc cites arXiv 2608.28462 as a "Robson-bound discussion" and says no policy escapes Θ(log n).
   - The paper's actual result is that 1+o(1) aggregate fragmentation collapses the ratio to Θ(log log M), and that randomization gives no asymptotic gain [S1].
   - v2 should frame the hugepage-versus-release trade-off as choosing k.
2. **§5.3 predictor cost is underestimated.**
   - The v1 doc says the return-address lookup costs "a few cycles". LLAMA measured 22.5 ns for a stack-hash lookup and a 48.8 ns fast path, against TCMalloc's 8.3 ns [S4].
   - `__builtin_return_address(0)` alone cannot tell apart allocations made through wrappers such as zmalloc, g_malloc or xmalloc. That is my analysis, but it is consistent with LLAMA adding stack height and size to the key, and with MemProf needing LTO context disambiguation [S10].
   - Sampling one allocation per 2 MiB gives very few samples for rare sites.
3. **§5.3 bets on the costly option first.** Google itself shipped the *zero-cost* span-capacity proxy (+1.02% fleet throughput) [S5]. TCMalloc's online stack-trace lifetime allocator was restricted to large allocations, and its source appears to be gone from HEAD (**UNVERIFIED**) [S7][S8].
4. **LLAMA's 78% figure is quoted without context.** The baseline was not hugepage-aware, and the prototype cost about 12.5% throughput [S4].
5. **§5.2 adaptive switching is more complex than necessary.** BatchIt-style per-slab batching gets most of the producer/consumer benefit within a single design [S19].
6. **§5.6 tier 2 "MTE where available" is not yet a design.** It needs a tagging policy (Partap [S33]), lazy tag initialization and a size cutoff ([S34][S35]), and an explicit threat model in light of TikTag [S36].
7. **L6 ("the C API is information-poor") is out of date.** AllocToken and MemProf hot/cold hints now carry type and site information into the allocator [S10][S11].
8. **§7 benchmarking lacks:** adversarial preconditioning [S46], back-pressured producer/consumer tests [S19], Fleetbench [S47], and a rule that speed and memory use the same baseline.
9. **§5.5 NUMA says "optional mbind" but omits origin-aware frees** [S20]. It should also state that the allocator will not bind threads.
10. **Missing entirely:** the cost of the kernel fault path for short-lived processes ([S22][S25]), and hotness fragmentation for tiered memory [S28].

## 12. The ten most promising research ideas for a 2026 allocator, ranked

| # | Idea | Evidence | Feasibility (honest) |
|---|---|---|---|
| 1 | **Zero-cost lifetime proxies in the page heap**: span-capacity split (C = 16), occupancy-prioritized span lists, densest-first hugepage fill | +1.02% throughput and −0.82% memory (split), −1.41% memory (prioritization), Temeraire's +7.7% RPS [S5][S15] | **High.** Published algorithms and Apache-2.0 reference code. Gains outside Google-like servers are unproven. |
| 2 | **Adaptive subrelease plus pressure feedback**: realized fragmentation and demand-peak tracking, combined with PSI/cgroup signals and k-aware release | About 1% fleet throughput [S6]; theoretical framing [S1] | **High** for the algorithm. The PSI coupling is new and unmeasured. |
| 3 | **BatchIt-style remote-free batching** | +9.9% to +27.6% on xmalloc-test, up to 20% on back-pressured message passing; some 6–8% regressions [S19] | **High.** It is small. Regressions must be gated. |
| 4 | **Compiler tokens as allocator input**: AllocToken pointer/data split (kalloc_type-like) and per-site tables keyed by token | Apple's design and Clang support [S11][S37] | **Medium.** No runtime cost, but it only helps code rebuilt with Clang. The fallback is size-only. |
| 5 | **Batched prefault and keep-warm for short-lived and bursty processes** (`MADV_POPULATE_WRITE`, pressure-gated) | Minor faults up to 54% of runtime [S25]; large kernel share in Memento [S22] | **Medium-high.** Cheap to build. Risk of RSS inflation; must be measured. |
| 6 | **Hint ABIs**: MemProf hot/cold, region/arena API with bulk free | Regions up to 15% faster and immune to the 2× fragmentation slowdown [S46]; MemProf [S10] | **High** to implement. Benefit needs PGO or source changes. |
| 7 | **Sampled online lifetime prediction for medium and large objects only**, with LLAMA's deadline and escalation and TCMalloc's counterfactual mode | 19–78% less fragmentation, against a weak baseline [S4]; TCMalloc design [S7] | **Medium-low.** Mixed evidence, and Google apparently dropped it. Ship it in counterfactual mode first. |
| 8 | **MTE policy done right** (random excluding neighbours, retag on free, lazy initialization, 32 KiB cutoff, SYNC in the secure profile) | [S33][S34][S35][S36][S38] | **Medium.** arm64-only hardware, costs depend on the microarchitecture, probabilistic security. |
| 9 | **MineSweeper-style UAF-safe profile** | +5.4% time / +11.1% memory on SPEC2006 [S40] | **Medium.** Needs sweeper threads and has bad worst cases. Opt-in only. |
| 10 | **Hotness segregation and NUMA origin-aware frees**: cold sites on cold hugepages, origin derived from address, optional weighted interleave | OBASE's 97% cold-byte finding [S28]; NUMAlloc +15.7% [S20]; AIM up to 26% [S29] | **Medium-low.** Benefit depends on CXL or NUMA hardware and on kernel tiering. Thread binding is rejected. |

**Not recommended as default features:**

- Meshing: opt-in, and only on broken-up hugepages.
- Hardware offload (Mallacc, Memento, SpeedMalloc, Valinor): no hardware exists.
- One-time allocators (FFmalloc, DangZero, BUDAlloc): memory bloat, virtualization or kernel changes.
- CHERI: hardware-gated.
- GPU allocator techniques: not relevant to a CPU malloc.

---

## Sources

- [S1] https://arxiv.org/abs/2608.28462 · https://arxiv.org/pdf/2608.28462
- [S2] https://arxiv.org/abs/2305.01497
- [S3] https://cacm.acm.org/research-highlights/combining-machine-learning-and-lifetime-based-resource-management-for-memory-allocation-and-beyond
- [S4] https://colinraffel.com/publications/cacm2022combining.pdf
- [S5] https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf
- [S6] https://research.google/pubs/pub50436/
- [S7] https://raw.githubusercontent.com/google/tcmalloc/master/docs/lifetime-based-allocator.md
- [S8] https://github.com/google/tcmalloc · https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/huge_page_aware_allocator.h
- [S9] https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/span.h · https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/huge_page_filler.h
- [S10] https://www.llvm.org/docs/MemProf.html · https://llvm.googlesource.com/llvm-project/+/a35206d78280e0ebcf48cd21bc5fff5c3b9c73fa%5E%21
- [S11] https://clang.llvm.org/docs/AllocToken.html
- [S12] https://arxiv.org/pdf/1902.04738
- [S13] https://docs.kernel.org/admin-guide/mm/transhuge.html
- [S14] https://github.com/plasma-umass/Mesh
- [S15] https://www.usenix.org/system/files/osdi21-hunter.pdf
- [S16] https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/huge_page_subrelease.h
- [S17] https://lwn.net/Articles/974826/
- [S18] https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/madvise.2
- [S19] https://microsoft.com/en-us/research/uploads/prodnew/2024/05/preprint_batchit.pdf
- [S20] https://par.nsf.gov/servlets/purl/10418579
- [S21] https://vlsiarch.eecs.harvard.edu/publications/mallacc-accelerating-memory-allocation
- [S22] https://www.pdl.cmu.edu/PDL-FTP/CloudComputing/Memento-MICRO23.pdf
- [S23] https://utw10244.utweb.utexas.edu/pubs/Li_HotOS_2023.pdf
- [S24] https://arxiv.org/abs/2508.20253v1 · https://arxiv.org/html/2508.20253
- [S25] https://arxiv.org/abs/2607.14789
- [S26] https://arxiv.org/abs/2505.13002v1
- [S27] https://arxiv.org/abs/2510.10219
- [S28] https://arxiv.org/abs/2603.00378
- [S29] https://conf.researchr.org/details/ismm-2026/ismm-2026-papers/8/Bandwidth-Speaks-We-Listen-Dynamic-Memory-Interleaving-for-Tiering
- [S30] https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html
- [S31] https://www.usenix.org/conference/osdi22/presentation/zhou-yang
- [S32] https://ppopp24.sigplan.org/details/PPoPP-2024-papers/24/Gallatin-A-General-Purpose-GPU-Memory-Manager
- [S33] https://arxiv.org/pdf/2209.00307
- [S34] https://arxiv.org/abs/2601.11786
- [S35] https://arxiv.org/abs/2511.17773
- [S36] https://arxiv.org/abs/2406.08719
- [S37] https://security.apple.com/blog/towards-the-next-generation-of-xnu-memory-safety/
- [S38] https://security.apple.com/blog/memory-integrity-enforcement/
- [S39] https://lists.llvm.org/pipermail/cfe-commits/Week-of-Mon-20251006/761601.html
- [S40] https://www.cl.cam.ac.uk/~tmj32/papers/docs/erdos22-asplos.pdf
- [S41] https://gts3.org/assets/papers/2021/wickman:ffmalloc-slides.pdf · https://www.usenix.org/conference/usenixsecurity21/presentation/wickman
- [S42] https://download.vusec.net/papers/dangzero_ccs22.pdf
- [S43] https://usenix.org/system/files/usenixsecurity24-ahn.pdf
- [S44] https://www.microsoft.com/en-us/research/uploads/prod/2024/02/revocation3-paper.pdf
- [S45] https://conf.researchr.org/details/ismm-2025/ismm-2025-papers/4/Fully-Randomized-Pointers
- [S46] https://arxiv.org/abs/2605.17119 · https://arxiv.org/pdf/2605.17119
- [S47] https://github.com/google/fleetbench
- [S48] https://raw.githubusercontent.com/daanx/mimalloc-bench/master/README.md
- Conference listings: https://conf.researchr.org/track/ismm-2026/ismm-2026-papers · https://conf.researchr.org/track/ismm-2025/ismm-2025-papers