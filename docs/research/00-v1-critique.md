# Critique of v1 (`RESEARCH_AND_DESIGN.md`)

**Totals: 12 critical, 62 major, 27 minor.**

Three independent reviewers attacked the v1 research and design document:

- one checked every fact and citation,
- one reviewed the design as a hostile allocator engineer,
- one looked for what was missing.

Every fault is listed below as the reviewer reported it.
The final plan (`../SOHAMALLOC_PLAN.md`) records how each one was resolved.

## Fact-check (every claim, number, version and citation)

I read all of /home/user/sohamalloc/docs/RESEARCH_AND_DESIGN.md (v1) and checked its claims against primary sources: kernel git and man-pages, glibc git and NEWS, TCMalloc, mimalloc, jemalloc and snmalloc source, and the ASPLOS'24, OSDI'21, Mallacc, LLAMA, snmalloc and Mesh papers.

Most of the cited numbers are accurate. These checked out: ASPLOS'24 (4.3% malloc cycles, 22.2% = 18.8% + 3.4% fragmentation, 51%/29%, 98% <1 KB, 46%/65% lifetimes, 16% prefetch, 1.41%, 3 MB → 1.5 MB); the Cloud blog's 2.7% → 3.5%; Mallacc's 18–20 cycles; Mesh's 16%/39%; LLAMA's 78%; MiraclePtr's 57%, 5.5–8% and 'half of exploitable bugs are UAF'; mimalloc's ~10% secure-mode cost; Go 1.16 MADV_DONTNEED; glibc 2.35 rseq registration; mm_cid and node_id in 6.3; MADV_COLLAPSE in 6.1; MADV_GUARD_INSTALL in 6.13; C23 free_sized; the jemalloc archive (June 2025), unarchive (March 2026) and 5.3.1 (April 2026); the 65530 kernel default; and Robson's Θ(log n) with n = size ratio.

The serious problems are about novelty claims and stale facts:
- **mm_cid indexing is not new (critical).** The kernel feature was built for allocators, Google's TCMalloc indexes per-CPU caches by kernel vCPU IDs in production, open-source TCMalloc has an mm_cid mode, and the document's own reference [2] describes it.
- **mm_cid semantics changed in Linux 6.19.** The rewrite introduced per-task and per-CPU ownership modes, an optimal space of min(nr_tasks, nr_cpus_allowed), and transient overshoot. CIDs also force process-wide fences.
- **The glibc rseq sharing story is incomplete.** __rseq_size was 32 in 2.35–2.39 and 20 in 2.40, and only from 2.41 does it reflect AT_RSEQ_FEATURE_SIZE and expose node_id and mm_cid. TCMalloc does not share glibc's registration at all.
- **The lifetime predictor has decades of prior art.** Barrett & Zorn 1993, dynamic pretenuring, LLAMA's own return-address hash, and TCMalloc's lifetime profiler all predate it. The design also has right-censoring and wrapper-call-site pitfalls.
- **The size-class waste figure is wrong.** Four classes per doubling bound waste at about 20–25%, not 12.5%.
- **'Hugepage-oblivious, MADV_COLLAPSE unused' is false.** TCMalloc calls MADV_COLLAPSE with a latency backoff, jemalloc has HPA, mimalloc v3 is THP-aware, and glibc has THP tunables.
- **MADV_GUARD_INSTALL conflicts with hugepages.** Installing a guard zaps and splits any THP in the range, and MADV_POPULATE fails on guarded ranges.
- **The TLB-IPI claim is x86-specific.** arm64 broadcasts TLB invalidations, and AMD INVLPGB (merged for 6.15) avoids IPIs. process_madvise on the calling process accepts any advice since 6.13 and batches TLB flushes, and jemalloc already uses it.
- **The PSI/cgroup plan has gaps.** /proc/pressure is system-wide, unprivileged triggers need windows of at least 2 s, MADV_FREE pages are reclaimed under cgroup pressure (since 4.12), there is prior art (TCMalloc limits, GOMEMLIMIT, TMO/Senpai), and an allocator-owned thread breaks unshare(CLONE_NEWUSER).
- **The <10 VMA goal conflicts with other features.** Strict overcommit, mprotect commits, mbind, madvise flag changes and mremap-based realloc all add VMAs.
- **P0901 is cited at an old revision.** R11 failed to reach consensus for C++26, and the document omits C++23 allocate_at_least and existing size-returning APIs.
- **The mimalloc facts are out of date.** Current v3.4.5 has out-of-line metadata, sampled guard pages and graduated MI_SECURE levels, which contradicts 'no graduated tiers'.
- **The Robson citation uses a different parameter.** arXiv 2608.28462 states the bound in terms of M and shows request fragmentation reduces it to Θ(log log M).

Smaller stale points: jemalloc 5.4.0 is out (Sep 2026); glibc 2.44 changed the arena limit and fixed the malloc_trim bug; glibc's calloc and realloc already avoid zeroing and copying for mmapped chunks; glibc safe-linking uses ASLR bits, not a secret; the snmalloc 64-bit figure is from 2019; and Fedora, Ubuntu and Arch now ship vm.max_map_count = 1048576.

Items I could not fully confirm are marked UNVERIFIED in the faults:
- the exact first release containing process_madvise TLB batching and PIDFD_SELF;
- the THP underused-split kernel version;
- whether khugepaged can collapse ranges containing guard markers;
- the content of the Dropbox Paper benchmark post (it requires a login);
- Ubuntu's shipped max_map_count value;
- snmalloc's current per-slab metadata size.

No files in /home/user/sohamalloc were modified. Downloaded source and papers are in a local scratch directory.

### 1. [critical] §5.1 'Cache slot selection, the new part'; §6 item 2 'mm_cid-indexed caches'

**Problem.** The document presents indexing per-CPU caches by rseq mm_cid as sohamalloc's novel contribution. It is not new. The kernel feature was built for this exact purpose. Google's TCMalloc already indexes its per-CPU caches by kernel-managed virtual CPU IDs in production, and open-source TCMalloc has an mm_cid mode. The document's own reference [2] says so.

**Evidence.** Kernel commit af7f588d8f73, 'sched: Introduce per-memory-map concurrency ID' (Desnoyers, merged for 6.3), says the feature's 'primary purpose' is 'to do the heavy-lifting needed by memory allocators to allow them to use per-cpu data structures efficiently'. It credits Paul Turner (Google) with the vcpu idea and notes that 'Google have been running a rseq vcpu_id extension internally in production' (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/patch/?id=af7f588d8f7355bc4298dd1962d7826358fc95f0). The ASPLOS'24 paper (doc ref [2]), section 4.1, says: 'TCMalloc makes use of virtual CPU (vCPU) IDs, which are managed by the kernel in process-private number space... By using a dense set of vCPU IDs to index the per-CPU cache, TCMalloc significantly reduces...' (https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf). LWN covered the idea in Feb 2022 in 'Extending restartable sequences with virtual CPU IDs' (https://lwn.net/Articles/885818). Open-source TCMalloc percpu.cc has RseqVcpuMode::kMM, which reads __rseq_abi.mm_cid when getauxval(AT_RSEQ_FEATURE_SIZE) covers the field. It is gated by Experiment::TEST_ONLY_MM_VCPU (https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/percpu.cc, https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/experiment_config.h).

**Fix.** Remove mm_cid indexing from the list of differentiators and cite TCMalloc and the kernel commit as prior art. Any remaining contribution has to be narrower and measurable. Examples: making it the default in a portable C allocator, sharing glibc's registration instead of requiring glibc.pthread.rseq=0, and handling the 6.19+ CID semantics (see the next fault). Also adopt the ASPLOS'24 finding that vCPU indexing skews load toward low-indexed caches, which is why TCMalloc needs heterogeneous, dynamically sized caches.

### 2. [major] §5.1 'Its values are dense and bounded by min(#threads, #CPUs)'; §6 item 2 'Per-CPU speed without paying for idle CPUs'

**Problem.** This describes mm_cid semantics loosely and in their pre-2026 form. Linux 6.19 rewrote mm_cid management, and the costs of using CIDs that do not map to a physical CPU are left out.

**Evidence.** In 6.3, a CID was 'temporarily (and uniquely) assigned while threads are actively running', within the possible-CPUs range (commit af7f588d8f73). Thomas Gleixner's rewrite reverted the 'complex CID management' because 'the compaction mechanism forces random tasks of a process into task work on exit to user space causing latency spikes' (https://lkml.iu.edu/2511.2/07557.html). The new mechanism has a per-task mode in which each task owns a CID while the number of tasks is at most the number of allowed CPUs, and a per-CPU mode otherwise. The optimal space is min(nr_tasks, nr_cpus_allowed), where nr_cpus_allowed is the union of all task affinities and only grows. CIDs may temporarily exceed the optimum but stay below the number of possible CPUs (https://lkml.iu.edu/2511.3/03604.html; cover letter https://lkml.iu.edu/2511.2/08641.html). It landed in the 6.19 cycle. The exact release tag was not checked; the search result points to a 6.19-tagged fixes merge, so treat 6.19 as PARTIALLY VERIFIED. Under per-task mode, a process with 200 mostly idle threads on a 256-CPU machine holds about 200 CIDs. TCMalloc's FenceCpu says: 'With virtual CPUs, we cannot identify the true physical core we need to interrupt' and calls FenceAllCpus() (https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/percpu.cc). That means every cross-slot drain or capacity steal becomes a process-wide rseq fence.

**Fix.** Design to the 6.19+ contract. Size the slot array by the number of possible CPUs and populate slots lazily. Reclaim idle slots. Budget for process-wide membarrier fences when stealing capacity or draining another slot. Keep slots that serve NUMA or NUCA locality separate from the slot index: check rseq node_id or cpu_id on each slow path, not the CID. State the behaviour per kernel range: 6.3–6.18 (lazy compaction) and 6.19+ (ownership modes).

### 3. [major] §3 L9 and §5.1: 'glibc ≥ 2.35 registers rseq itself, so the allocator has to share that registration through __rseq_offset'

**Problem.** This is correct but incomplete in ways that matter for reading mm_cid. The meaning of __rseq_size changed across glibc versions, glibc's registration can be disabled, and the leading rseq allocator (TCMalloc) does not share glibc's registration at all.

**Evidence.** glibc NEWS: 2.35 added automatic rseq registration with __rseq_flags, __rseq_offset and __rseq_size. In 2.40, '__rseq_size now denotes the size of the active rseq area (20 bytes initially), not the size of struct rseq (32 bytes initially)'. In 2.41, 'Support for the extensible rseq ABI introduced in the Linux kernel version 6.3 has been added... __rseq_size now exposing features past the original ABI which currently include node_id and mm_cid' (https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=NEWS;hb=refs/tags/glibc-2.44). Desnoyers (Feb 2026) gives the same history: __rseq_size was 32 in 2.35–2.39, 20 in 2.40, and equal to AT_RSEQ_FEATURE_SIZE from 2.41 (https://lkml.iu.edu/2602.2/03380.html). TCMalloc calls syscall(__NR_rseq, &__rseq_abi, ...) on its own area (percpu.cc). When glibc registers first, TCMalloc loses per-CPU mode unless GLIBC_TUNABLES=glibc.pthread.rseq=0 is set (https://jira.mongodb.com/browse/SERVER-90036, https://mongodb.com/zh-cn/docs/upcoming/administration/tcmalloc-performance). An LTTng user abandoned TCMalloc for this reason (https://lists.lttng.org/pipermail/lttng-dev/2026-July/031086.html).

**Fix.** Specify the detection logic. Use getauxval(AT_RSEQ_FEATURE_SIZE) >= offsetof(struct rseq, mm_cid) + 4, which is what TCMalloc does. Trust __rseq_size only on glibc 2.41 or later. Treat __rseq_size == 0 (tunable off or registration failed) as 'register our own area'. Add tests for glibc 2.35–2.40, 2.41+, musl, static linking, and GLIBC_TUNABLES=glibc.pthread.rseq=0.

### 4. [major] §5.3 'This is LLAMA's idea without the neural network and without offline profiling'; §6 item 3; §3 L1

**Problem.** The sampled call-site lifetime predictor is presented as new. Online, allocation-site-keyed lifetime prediction has decades of prior work. LLAMA's own runtime already uses the proposed return-address hash lookup. The design also has known measurement pitfalls that the document does not mention.

**Evidence.** Barrett & Zorn, 'Using Lifetime Predictors to Improve Memory Allocation Performance', PLDI 1993 (doi 10.1145/155090.155108), sent objects predicted to be short-lived to a separate allocator. Cohn & Singh, NIPS 1996 (https://papers.neurips.cc/paper_files/paper/1996/hash/a9078e8653368c9c291ae2f8b74012e7-Abstract.html). The LLAMA paper surveys this line of work: allocation-site predictors, dynamic pretenuring (Jump/Blackburn/McKinley 2004; Clifford et al. 2015) and ROLP (EuroSys'19). At runtime LLAMA uses 'values that are already in registers (the return address and stack pointer) to index a hash table'. It reports that a deep stack trace costs 400 ns against an 8.3 ns fast path (https://cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf). TCMalloc already ships a sampled per-callstack lifetime profiler (MallocExtension::StartLifetimeProfiling) that explicitly separates 'observed' lifetimes from right-censored 'partially observed' ones (https://raw.githubusercontent.com/google/tcmalloc/master/docs/sampling.md). Pitfalls: (a) recording lifetime only at free() biases the data toward short-lived objects, because long-lived objects are censored. (b) __builtin_return_address(0) inside malloc often lands in a wrapper such as operator new, xmalloc or g_malloc, which merges many call sites; LLAMA adds stack height for this reason. (c) Sampling once per 2 MiB is byte-weighted, but 98% of objects are under 1 KB and hold only 28% of bytes (ASPLOS'24), so statistics for small, short-lived sites accumulate slowly.

**Fix.** Cite this prior work and present the predictor as an engineering combination, not an invention. Key the predictor on (return address, stack depth or frame-pointer hash, size class), and do the frame walk only on the sampled path. Handle censoring, for example by counting 'still live at T'. Validate against TCMalloc lifetime-profile data on the same workloads before claiming RSS or TLB benefits.

### 5. [major] §5.1 'about 4 per power of two, so internal waste is at most about 12.5%'

**Problem.** The arithmetic is wrong. Four size classes per doubling bound internal waste at about 20% of the block, or 25% of the request. A 12.5% bound needs about 8 classes per doubling.

**Evidence.** With classes at 2^k × {1, 1.25, 1.5, 1.75}, a request of 2^k + 1 rounds up to 1.25 × 2^k. The waste is about 0.25 × 2^k, which is 20% of the block and 25% of the request. jemalloc's manual: size classes are 'spaced such that there are four size classes for each doubling in size, which limits internal fragmentation to approximately 20% for all but the smallest size classes' (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/doc/jemalloc.xml.in).

**Fix.** Either keep 4 classes per doubling and state a worst case of about 20–25%, with the expected average, or use 8 classes per doubling (worst case about 11–12.5%) and account for the extra partially filled spans that more classes create. That second cost is the trade-off §9 already flags.

### 6. [major] §3 L3 'Most allocators are still hugepage-oblivious... MADV_COLLAPSE, which almost no allocator uses deliberately yet'; §2 'Hugepage awareness lives in TCMalloc'; §6 item 4

**Problem.** This is stale and false. TCMalloc deliberately calls MADV_COLLAPSE, with a latency backoff. jemalloc has a hugepage allocator (HPA), mimalloc v3 is THP-aware, and glibc has THP tunables and defaults.

**Evidence.** TCMalloc system_allocator.h calls madvise(start, length, MADV_COLLAPSE) and names regions with prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, ...) (https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/system_allocator.h). huge_page_filler.h has ShouldBackoffFromCollapse(): 'In case of high collapse latency, this is used to reduce the frequency of collapse attempts' (https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/huge_page_filler.h). jemalloc's manual documents 'the HPA minimum purge interval and hugification delay' (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/doc/jemalloc.xml.in), and Meta's 2026 plan prioritises the huge-page allocator (https://engineering.fb.com/2026/03/02/data-infrastructure/investing-in-infrastructure-metas-renewed-commitment-to-jemalloc/). mimalloc v3.2.6 (2026-01-08): 'When THP is detected on v3, mimalloc will set the MIMALLOC_MINIMAL_PURGE_SIZE to 2MiB to avoid breaking up potential THP huge pages' (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). glibc added the glibc.malloc.hugetlb tunable in 2.35, enabled 2 MB THP by default in malloc on AArch64 in 2.43, and added glibc.elf.thp (which changes malloc's THP mode) in 2.44 (glibc NEWS, link above).

**Fix.** Rewrite L3 and the landscape summary. To claim hugepages as an advantage, sohamalloc must be measured against Temeraire, jemalloc HPA and mimalloc v3 on hugepage coverage, RSS and dTLB misses, not against 'oblivious' allocators.

### 7. [major] §5.5 'Guard pages' row and §5.6 tiers 1–2 ('guard pages between spans', 'guard page per large allocation'), combined with §5.4 'hugepage-first'

**Problem.** The document leaves out the semantics of MADV_GUARD_INSTALL that conflict with the hugepage-first design and with prefaulting.

**Evidence.** Man page: installing a guard replaces existing mappings, so data in the range is lost. Before 6.15 it worked only on writable anonymous private mappings; it rejects mlock'd and hugetlb mappings. Guards survive fork and are not removed by MADV_DONTNEED or MADV_FREE. 'Attempting to mlock(2) lightweight guard regions will fail, as will MADV_POPULATE_READ or MADV_POPULATE_WRITE' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/madvise.2). In kernel source, guard_install_pmd_entry() returns '>0 so we abort the operation + zap' when it finds a huge PMD, and madvise_guard_install then zaps and retries: 'If any non-guard pages or THP huge pages are encountered, give up and zap the range' (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/mm/madvise.c). So a single guard inside a 2 MiB hugeblock forces that block to be PTE-mapped. Whether khugepaged can later collapse a range that contains guard markers is UNVERIFIED. Doc ref [19] (lwn.net/Articles/1000908) is a man-pages patch posting, not an LWN article.

**Fix.** Place guards only at hugeblock boundaries, or in a separate non-THP pool, which is how GWP-ASan-style sampling usually works. Never issue MADV_POPULATE_* over ranges that contain guards. Version-gate: 6.13 for anonymous mappings, 6.15 for file-backed and read-only ones. Measure the hugepage-coverage cost of tier 1.

### 8. [major] §3 L8 (TLB-shootdown IPIs, mmap_lock); §5.5 'Syscall cost' and 'TLB shootdowns' rows

**Problem.** The IPI claim is specific to x86. The batching plan misses process_madvise, which is the kernel's batching interface, and which jemalloc already uses. The mmap_lock concern is outdated for the release path.

**Evidence.** arm64 flushes with broadcast TLBI ...IS instructions, not IPIs (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm64/include/asm/tlbflush.h). AMD INVLPGB broadcast invalidation was merged for Linux 6.15 and lets the kernel 'invalidate TLB entries on remote CPUs without needing to send IPIs' (https://lkml.rescloud.iu.edu/2412.2/08358.html, https://www.phoronix.com/news/AMD-INVLPGB-Ready-For-Linux). process_madvise(): 'Since Linux 6.13, when manipulating the calling process, any advice flag is permitted' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/process_madvise.2). The PIDFD_SELF sentinels were queued for 6.15 (https://lkml.iu.edu/hypermail/linux/kernel/2501.3/06907.html; release PARTIALLY VERIFIED). In current mainline, vector_madvise() uses a single mmu_gather for MADV_DONTNEED, DONTNEED_LOCKED and FREE across all iovec ranges, which means one flush for many discontiguous ranges (madvise_batch_tlb_flush in mm/madvise.c, link above; SeongJae Park's series https://lkml.iu.edu/hypermail/linux/kernel/2504.1/04107.html; first release UNVERIFIED). get_lock_mode() in the same file takes only a per-VMA read lock for MADV_DONTNEED, MADV_FREE and MADV_GUARD_*, and the mmap read lock for MADV_COLLAPSE and POPULATE. jemalloc 5.3.1 already does 'Enable process_madvise usage, add runtime option process_madvise_max_batch' (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog).

**Fix.** Say 'on x86 without INVLPGB/RAR'. Make process_madvise(PIDFD_SELF or a pidfd, iovec, MADV_DONTNEED or MADV_FREE) the primary release primitive, falling back to madvise. Measure flushes and IPIs per released MiB on x86 and arm64. Credit jemalloc as prior art for batched purging.

### 9. [major] §5.5 'When to release' and 'RSS honesty' rows; §5.5 'Background work'; §6 item 1 'Pressure-adaptive memory return'

**Problem.** The PSI and cgroup plan has factual gaps. /proc/pressure is system-wide. Unprivileged PSI triggers react slowly. The reason given for 'DONTNEED is container-safe' misreads how the kernel reclaims MADV_FREE pages. There is prior art the document does not credit. And an allocator-owned thread breaks some programs.

**Evidence.** PSI docs: /proc/pressure/ is system-wide, and each cgroup2 directory has its own memory.pressure. 'Unprivileged users can also create monitors, with the only limitation that the window size must be a multiple of 2s'; windows range from 500 ms to 10 s (https://docs.kernel.org/accounting/psi.html). MADV_FREE pages have been moved to the inactive file LRU since commit f7ad2a6cb9f7 ('mm: move MADV_FREE pages into LRU_INACTIVE_FILE list', https://coral.googlesource.com/linux-mtk/+/f7ad2a6cb9f7c4040004bedee84a70a9b985583e). The man page says that before Linux 4.12 on swapless systems they were freed instantly (madvise.2, link above). So they are reclaimed cheaply under cgroup memory.high or memory.max pressure before an OOM. The real drawback is metrics that read RSS, which matches Go 1.16's stated reason for switching (https://go.dev/doc/go1.16). Prior art: TCMalloc MallocExtension::SetMemoryLimit with LimitKind {kSoft, kHard} (https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/malloc_extension.h); Go 1.19 soft memory limit GOMEMLIMIT (https://go.dev/doc/go1.19); Meta TMO/Senpai, PSI-driven reclaim that saved 20–32% of memory fleet-wide (ASPLOS'22, https://par.nsf.gov/servlets/purl/10332477). Threads: 'CLONE_NEWUSER requires that the calling process is not threaded' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/unshare.2).

**Fix.** Read the process's own cgroup memory.pressure, memory.events and memory.max via /proc/self/cgroup, not /proc/pressure. Expect a reaction time of 2 s or more when unprivileged. Restate the DONTNEED-versus-FREE trade-off as accuracy of metrics, not OOM safety. Credit the prior art. Make the PSI thread opt-in, or create it lazily after the first trigger, and never in single-threaded processes.

### 10. [major] §4 item 5, §5.5 'VMA count' ('Reserve ... 64 GiB ... commit by touching ... fewer than 10 VMAs'), §5.4 'realloc of large blocks uses mremap', §5.5 NUMA 'Optional mbind per node arena'

**Problem.** The fewer-than-10-VMAs goal conflicts with several other features the document proposes, and with strict overcommit.

**Evidence.** Overcommit docs: in mode 2, 'MAP_NORESERVE flag is ignored', a private writable anonymous mapping costs its full size, and 'We account mprotect changes in commit' (https://docs.kernel.org/mm/overcommit-accounting.html). A 64 GiB read-write reservation therefore fails or drains CommitLimit under vm.overcommit_memory=2. The alternative, reserving PROT_NONE and calling mprotect on commit, splits VMAs. madvise flag changes such as MADV_HUGEPAGE and NOHUGEPAGE, and anonymous VMA naming, also split VMAs ('Update the vm_flags or anon_name on region of a vma, splitting it', mm/madvise.c). So does mbind, through vma_modify_policy (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/mm/mempolicy.c). mremap cannot grow a block into neighbouring address space that the reservation has already mapped, so it must move the block to a new VMA or MREMAP_FIXED it over part of the reservation. The PROT_NONE guard fallback for kernels before 6.13 adds up to 2 VMAs per guard.

**Fix.** Set a VMA budget per feature instead of a flat '<10'. Define the reservation strategy for each overcommit mode and for RLIMIT_AS. Put large mremap-able objects in a separate region. Use per-node reservations rather than mbind on sub-ranges. Test under vm.overcommit_memory=2 and ulimit -v.

### 11. [major] §3 L6 'Size feedback ... is still only a proposal (P0901) [18]'; ref [18] wg21.link/p0901r9; §5.7 extended API

**Problem.** The citation points to an old revision, and the claim leaves out that size feedback already exists in the C++ standard library and in shipping allocators. The document also does not say how its size-feedback API interacts with C23 free_sized.

**Evidence.** wg21.link/p0901 now resolves to P0901R11 (2023). The tracking issue shows that at Kona in November 2023 the EWG poll to forward R11 to CWG for C++26 got 1 SF, 3 F, 10 N, 2 A: 'Not consensus'. The label is needs-revision (https://github.com/cplusplus/papers/issues/18). So it is not in C++26. C++23 already has size feedback for allocators through allocate_at_least (P0401R6, https://wg21.link/P0401), and libstdc++ was adding it in 2026 (https://gcc.gnu.org/pipermail/libstdc++/2026-March/065676.html). TCMalloc ships tcmalloc_size_returning_operator_new / __size_returning_new and hot/cold allocation hints (malloc_extension.h, link above). mimalloc v3.2.6 added an 'API for returning allocated sizes on allocation and free' (mimalloc readme, link above). C23 free_sized must be given the size that was requested; an implementation may ignore it (N2801, https://open-std.org/jtc1/sc22/wg14/www/docs/n2801.htm). glibc 2.43 implements free_sized and free_aligned_sized (https://linuxiac.com/glibc-2-43-released-with-iso-c23-features-and-performance-improvements/), and so does jemalloc 5.3.1 (ChangeLog).

**Fix.** Cite P0901R11 and its status. Acknowledge allocate_at_least and the existing allocator APIs. Specify that sohamalloc's free_sized and sized delete accept both the requested size and the size returned by feedback, and validate them in hardened tiers.

### 12. [major] §2 mimalloc row; §3 L7 'nothing offers sensible graduated tiers'; §6 item 6

**Problem.** The mimalloc facts are out of date, and two of the document's claims are contradicted by current mimalloc.

**Evidence.** The mimalloc README lists 'Latest release: v3.4.5 (2026-08-05) recommended; Latest v2 release: v2.4.5 stable'. Among its changes: 'All metadata is now separated from heap objects in v3' (v3.4.1, 2026-07-14) and 'by default put page meta info separate from allocated objects (v3,security)' (v3.3.0). Its guarded mode with MIMALLOC_GUARDED_SAMPLE_RATE 'places a guard page behind every N suitable object allocations', added in v3.0.1/v2.1.9 (2025-01). MIMALLOC_PURGE_DELAY defaults to '1000 in v3' ms, and purging uses MADV_DONTNEED by default (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). types.h defines graduated MI_SECURE levels 1–5, from guard pages around metadata up to guard pages at the end of each page (https://raw.githubusercontent.com/microsoft/mimalloc/dev3/include/mimalloc/types.h). Verified as correct: 64 KiB pages, a single CAS for a remote free, and a secure-mode penalty of 'usually around 10% on average'.

**Fix.** Update the mimalloc row: v3.4.5, metadata out of line, sampled guard pages, THP-aware purging, 1 s purge delay. Change 'eager purging' to 'delayed purging (1 s default in v3)'. Drop or narrow 'nothing offers graduated tiers'; the remaining gap is graduated tiers selectable at runtime in one binary. Remove 'tier 0 + sampled guards' as a differentiator, or show that its cost and catch rate beat mimalloc guarded mode and GWP-ASan.

### 13. [major] §0 Robson's bound and ref [14]

**Problem.** The statement is roughly right, but the cited source uses a different parameter, and the paper's main result undercuts 'No size-class or fit policy escapes this'. The bound also concerns contiguous address space, not RSS.

**Evidence.** SQLite's write-up of Robson (JACM 1974) defines n as 'the ratio of the largest memory allocation to the smallest' and gives N = M × (1 + (log2 n)/2) − n + 1 for power-of-two first-fit. That is total memory Θ(M log n), so the document's Θ(log n) is right (https://www.sqlite.org/malloc.html). However, ref [14], arXiv 2608.28462 (Bender, Conway, Farach-Colton, Komlós, Kuszmaul, Wein, submitted 2026-08-28), states the classical ratio as 'Θ(log M), where M is the volume high-water mark', not the size ratio. Its finding is that with k = 1 + o(1) request fragmentation, which it says many real allocators effectively use, the optimal ratio 'collapses to Θ(log log M)' (https://arxiv.org/abs/2608.28462). Mesh's abstract claims it breaks 'the classical Robson bounds with high probability' (https://arxiv.org/abs/1902.04738). The second ref [14] URL (sqlite.org/matrix/ev/src/mem5.html) is a source listing of mem5.c, not the explanatory page.

**Fix.** Cite Robson JACM 1971 and 1974 directly, together with sqlite.org/malloc.html. Give both parameterisations. Limit the claim to non-moving allocators that place each object contiguously in one address space. Add that page-granular release and request fragmentation, as in arXiv 2608.28462, change the bound for physical memory, so the design target should be RSS, not address-space fragmentation.

### 14. [minor] §2 jemalloc row ('Many tuning knobs', 'dirty → muzzy → clean'); §7 comparison set 'jemalloc 5.3.1'

**Problem.** The jemalloc facts are out of date, and the default decay path is described inaccurately.

**Evidence.** jemalloc ChangeLog: '5.4.0 (Sep 17, 2026)'. It adapts 'tcache fill and retention targets per bin to demand' and removes seven legacy tcache knobs. 5.3.1 (Apr 13, 2026) added process_madvise batching, C23 free_sized and free_aligned_sized, and calloc_madvise_threshold (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog; tag 5.4.0 exists at raw.githubusercontent.com/jemalloc/jemalloc/5.4.0/ChangeLog). The manual says 'Muzzy decay is disabled by default (with decay time 0)', so the default path is dirty → clean (jemalloc.xml.in). Verified as correct: archived 2025-06-02 (https://lists.freebsd.org/archives/cu/2025-June/007847.html), unarchived per Meta's 2026-03-02 post, 5.3.1 released 2026-04-13 (https://www.phoronix.com/news/jemalloc-5.3.1).

**Fix.** Pin jemalloc 5.4.0 in §7. Note that muzzy is off by default and that jemalloc is moving toward adaptive tcache sizing, which narrows the 'no tuning' differentiator.

### 15. [minor] §2 glibc row ('Arenas (up to 8×cores)', malloc_trim arena bugs [6]); §3 L11 ('calloc: it zeroes memory the kernel already zeroed', 'realloc on large blocks: it copies even though mremap exists')

**Problem.** The glibc facts are out of date, and the two L11 claims overgeneralise.

**Evidence.** glibc 2.43's arena.c uses narenas_limit = NARENAS_FROM_NCORES(n), which is 8×cores on 64-bit. glibc 2.44's arena.c uses narenas_limit = __get_nprocs(), with arena_test (8 on 64-bit) as the floor (https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=malloc/arena.c;hb=refs/tags/glibc-2.44). Bug 30769, 'malloc_trim is not working correctly for arenas other than arena 0', is RESOLVED FIXED with target 2.44 and is listed in the 2.44 NEWS (https://sourceware.org/bugzilla/show_bug.cgi?id=30769). In malloc.c, __libc_calloc2 skips clearing for mmapped chunks and for freshly sbrk'd top memory ('clear only the bytes from non-freshly-sbrked memory'). Linux glibc defines HAVE_MREMAP 1, so realloc of mmapped chunks uses mremap_chunk (https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=malloc/malloc.c;hb=HEAD; sysdeps/unix/sysv/linux/malloc-sysdep.h).

**Fix.** Say '8×cores up to glibc 2.43; nprocs from 2.44'. Note that the malloc_trim bug is fixed in 2.44. Narrow L11 to heap-resident (non-mmapped) chunks. Pin the glibc version in benchmarks, because the arena change affects RSS results.

### 16. [minor] §3 L5 'NUCA-aware caches gave up to 3.8% throughput [2]'

**Problem.** The figure is the best benchmark result, not the fleet effect, so it overstates the gain.

**Evidence.** ASPLOS'24: NUCA-aware transfer caches reduce the LLC load miss rate by 4.37%, give '0.32% improvement in application throughput in the fleet', 0.28–1.72% for the top 5 apps, and 1.37–3.80% on benchmarks, at a cost of +0.10% fleet memory (https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf).

**Fix.** Write '0.32% fleet-wide (up to 3.8% on benchmarks), at +0.10% memory'.

### 17. [minor] §2 TCMalloc row '+7.7% RPS, 26% less fragmentation, 6% fewer TLB stalls [1]'

**Problem.** The sentence mixes results from different scopes, and omits the RAM figure.

**Evidence.** OSDI'21 abstract: the 8-application studies show 'improving requests-per-second (RPS) by 7.7% and reducing RAM usage 2.4%'. The fleet-scale results (a 1% experiment plus the rollout) show '6% fewer TLB miss stalls, and 26% reduction in memory wasted due to fragmentation' (https://www.usenix.org/conference/osdi21/presentation/hunter).

**Fix.** Report the two sets separately, with their scopes.

### 18. [minor] §1 table '98% of allocations are under 1 KB [2]'

**Problem.** The number is right, but the omitted half of the sentence changes where memory-efficiency work should focus.

**Evidence.** ASPLOS'24: 'Objects smaller than 1 KB make up 98% of the allocated objects, but occupy only 28% of the memory'. Objects over 8 KB account for 50% of fleet memory, and objects over 256 KB (which bypass the caches) for 22% (https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf).

**Fix.** Quote both numbers, and give large-object placement and release (§5.4) as much design weight as the small-object fast path.

### 19. [minor] §3 L9 'Per-CPU caches (rseq) waste memory when a process has 4 threads on a 256-CPU machine'

**Problem.** This overstates the problem for current TCMalloc, which already mitigates it in several ways besides vCPU IDs.

**Evidence.** TCMalloc cpu_cache.h tracks whether each CPU's cache 'has ever been populated' (lazy population), 'tries to drain/reclaim inactive per-CPU caches', and resizes slabs and per-class capacity dynamically (https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/cpu_cache.h). ASPLOS'24 also cut the default per-CPU cache from 3 MB to 1.5 MB.

**Fix.** State the residual problem precisely, for example as memory stranded in caches populated during load spikes, and compare against TCMalloc's idle-cache reclaim rather than a naive per-CPU design.

### 20. [minor] §5.6 tier 0 'Free-list pointers XOR-encoded with a per-process secret (like glibc's "safe-linking")'

**Problem.** glibc safe-linking does not use a secret.

**Evidence.** glibc malloc.c: 'Safe-Linking: Use randomness from ASLR (mmap_base)', with #define PROTECT_PTR(pos, ptr) ((((size_t) pos) >> 12) ^ ((size_t) ptr)). The mask is derived from the pointer's own storage address, so a single heap-address leak defeats it (https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=malloc/malloc.c;hb=HEAD).

**Fix.** Describe tier 0 as a per-process (or per-heap) random key, unlike glibc safe-linking. Consider mixing in the storage address as well, so that one leaked key-and-pointer pair does not decode every list.

### 21. [minor] §5.4 'snmalloc fits each 64 KiB slab's metadata in 64 bits [5]'; §2 snmalloc 'Out-of-band metadata'

**Problem.** The figure comes from the 2019 paper, and the project says its metadata layout has changed since. The 2019 design also kept superslab metadata inside the superslab.

**Evidence.** The paper says '64-bits of meta-data are sufficient for each 64 KiB slab', and that 'The first 64KiB of a superslab is special as it contains the meta-data' (https://www.microsoft.com/en-us/research/uploads/prod/2020/04/snmalloc.pdf). The current README: 'The mechanism for returning memory to remote threads has remained, but most of the meta-data layout has changed'. Its hardened build stores most metadata separately, behind guard pages (https://raw.githubusercontent.com/microsoft/snmalloc/main/README.md). Verified as correct: '1000s of remote deallocations ... with only a single atomic operation'; the paper's batch threshold is 1 MiB per destination.

**Fix.** Mark the 64-bit figure as the 2019 design. Take current metadata sizes from snmalloc's docs/security and AddressSpace.md; the current per-slab size is UNVERIFIED.

### 22. [minor] §3 L8 'vm.max_map_count (default 65530)'

**Problem.** 65530 is still the kernel default, but major distributions now ship 1048576.

**Evidence.** The Fedora change for F39 raised it to 1048576 and calls 65530 'the Linux default' (https://fedoraproject.org/wiki/Changes/IncreaseVmMaxMapCount). Arch raised it from 65530 to 1048576 in filesystem 2024.04.07-1 (https://archlinux.org/news/increasing-the-default-vmmax_map_count-value/). Ubuntu 24.04 did the same through procps, per Launchpad bug 2057792 and news coverage (https://bugs.edge.launchpad.net/ubuntu/+source/gamemode/+bug/2057792; the shipped value is PARTIALLY VERIFIED). Debian has a merge request proposing it (https://salsa.debian.org/kernel-team/linux-base/-/merge_requests/17).

**Fix.** Write 'kernel default 65530; Fedora 39+, Ubuntu 24.04+ and Arch use 1048576; containers and older distros may still be at 65530'.

### 23. [minor] §2 security row and §3 L7 (MiraclePtr/BackupRefPtr listed as allocator features; 'Full hardening ... 10% or more [4][9]')

**Problem.** The numbers check out, but MiraclePtr is not something a drop-in malloc can provide. It requires rewriting the application's source code.

**Evidence.** The 2024 Google blog says MiraclePtr 'effectively mitigated 57% of these use-after-free vulnerabilities in privileged processes' (168 reports). Browser-process memory rose '5.5-8% on desktop platforms and approximately 2% on Android', and whole-browser memory 1–3% (https://security.googleblog.com/2024/01/miracleptr-protecting-users-from-use.html). The 2022 post says 'Half of the known exploitable bugs in Chrome are use-after-free' and that MiraclePtr 'relies on rewriting the codebase to use a new smart pointer type, raw_ptr<T>' (https://security.googleblog.com/2022/09/use-after-freedom-miracleptr.html). Ref [9] reports memory overhead, not CPU overhead.

**Fix.** Label MiraclePtr as a compiler- and source-level technique that needs allocator support. Cite [4] only for the CPU cost.

### 24. [minor] References [6], [15], [16], [17], [19]; §3 L2 Go claim

**Problem.** Several citations are inaccessible, out of date, mismatched to the claim, or missing.

**Evidence.** [15] (paper.dropbox.com) returns HTTP 401 without a login. According to a search snippet it is a 2021 non-peer-reviewed post about mimalloc 2.0.3 on larsonN; its content is UNVERIFIED, and it is stale for v3 either way. [17] is the mTHP v9 patch cover letter (https://lkml.iu.edu/hypermail/linux/kernel/2312.0/08512.html), but it is also cited for MADV_COLLAPSE, which it does not cover. [19] is a man-pages patch posting. [16] lacks authors and venue: Maas, Kennelly, Nguyen, Gove, McKinley, Turner, ISMM 2021 (now at https://research.google/pubs/adaptive-hugepage-subrelease-for-non-moving-memory-allocators-in-warehouse-scale-computers/). The Go 1.16 claim is correct but has no citation. The release notes say the runtime 'now defaults to releasing memory... promptly (using MADV_DONTNEED)... RSS will more accurately reflect' (https://go.dev/doc/go1.16). Verified as correct: Mallacc's 'fast path is only ≈40 static x86 instructions, and can take 18-20 cycles, assuming cache hits' (https://courses.physics.illinois.edu/ece598ms/fa2019/papers/paper34.pdf); ASPLOS'24 figures of 4.3% fleet malloc cycles, 22.2% fragmentation (18.8% external + 3.4% internal), 51% page heap / 29% central free list, 46% under 1 ms, 65% of objects over 1 GB living more than a day, 16% of cycles on prefetch, 1.41% from span prioritisation, and 3 MB → 1.5 MB; the Google Cloud blog's 2.7% → 3.5% with +3.4% RPS and −6.5% peak memory (https://cloud.google.com/blog/topics/systems/trading-off-malloc-costs-and-fleet-efficiency/); Mesh's −16% Firefox and −39% Redis; LLAMA's 'up to 78%'; MADV_COLLAPSE since 6.1, MADV_GUARD_INSTALL since 6.13, and mm_cid and node_id since 6.3.

**Fix.** Replace [15] with a reproducible, versioned benchmark, or drop it. Cite the madvise(2) man page for MADV_COLLAPSE and guard regions, docs.kernel.org/admin-guide/mm/transhuge.html for mTHP, and go.dev/doc/go1.16 for Go.

### 25. [minor] §5.5 'Hugepages' row ('Use MADV_COLLAPSE (6.1+) ... when THP is set to madvise'); §3 L3 mTHP

**Problem.** The semantics of MADV_COLLAPSE and mTHP that affect latency and policy are left out.

**Evidence.** Man page: MADV_COLLAPSE is a 'best-effort synchronous collapse'. It 'is independent of any sysfs ... setting', so it works even when THP is set to never, though it still respects VM_NOHUGEPAGE. Its 'Allocation for the new hugepage may enter direct reclaim and/or compaction' (madvise.2, link above). The transhuge docs say 'madvise_collapse only supports collapsing to PMD-sized THPs and does not attempt mTHP collapses', and that non-PMD mTHP sizes default to enabled='never'. The same docs describe splitting of 'underused' THPs under memory pressure (shrink_underused, thp_underused_split_page); the kernel version that introduced this is UNVERIFIED (https://docs.kernel.org/admin-guide/mm/transhuge.html, labelled 7.3.0-rc7). The kernel takes the mmap read lock for MADV_COLLAPSE (get_lock_mode in mm/madvise.c). Anonymous mTHP landed in 6.8 according to secondary sources only; the commit is 19eaf44954df.

**Fix.** Call MADV_COLLAPSE only from maintenance work, with a latency backoff as TCMalloc does. Treat THP=never as user intent and do not override it. Do not assume mTHP is enabled. Account for the kernel splitting partially used hugeblocks under pressure.

### 26. [minor] §5.5 'Background work' / §5.2 middle tier (locks); overall kernel feature survey

**Problem.** A kernel feature directly relevant to the allocator's locked slow paths is missing.

**Evidence.** Linux 7.0 adds rseq time-slice extension. A thread enables it with prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_SET, PR_RSEQ_SLICE_EXT_ENABLE, 0, 0), then 'can request a time slice extension by setting rseq::slice_ctrl::request to 1'. The default extension is 5 µs, and rseq_slice_yield() ends it early (https://docs.kernel.org/7.0/userspace-api/rseq.html; merge reported at https://phoronix.com/news/linux-7.0-scheduler). It is meant to avoid preemption while holding user-space locks, such as transfer-cache or central-list locks.

**Fix.** Evaluate time-slice extension for the critical sections of the middle and central tiers on 7.0+ kernels, gated by feature detection. Update the kernel support matrix (6.1 LTS through 7.x) in §8.

## Hostile design review

Hostile review of the design in /home/user/sohamalloc/docs/RESEARCH_AND_DESIGN.md (sections 4-9). Verdict: v1 is a collage of mechanisms taken from allocators whose object-flow and ownership models don't fit together. mimalloc-style page-local sharded free lists sit in the front end. TCMalloc-style transfer caches and elastic per-CPU capacity sit in the middle. snmalloc message passing is bolted on per heap. On top of those sit rseq mm_cid slots, a per-object lifetime predictor and occupancy-ordered spans. The pieces that decide whether an allocator works at all are missing:
- a pointer-to-metadata page map;
- the ownership and rseq protocol for slot-indexed caches;
- a virtual-address reservation and commit strategy that survives vm.overcommit_memory=0/2 and RLIMIT_AS;
- page geometry decided at runtime (16K/64K arm64 base pages; PMD = 32 MiB or 512 MiB).

Several headline numbers and claims are wrong or not backed by evidence:
- 4 size classes per doubling gives a worst case of about 20%, not 12.5% (jemalloc's own manual says so).
- mm_cid caches already exist in TCMalloc (RseqVcpuMode::kMM).
- The current kernel bounds mm_cid by min(1.25*min(cpus_allowed, threads), possible_cpus), not min(threads, CPUs).
- The predictor lookup is not "a few cycles": LLAMA measured 22.5 ns for its lookup against an 8.3 ns TCMalloc fast path.
- __builtin_return_address(0) collapses behind libstdc++'s extern-template std::string, operator new, Rust's System allocator and CPython's obmalloc arenas.
- The "out-of-line metadata" claim contradicts the design's own inline encoded free lists.
- The tier-0 double-free bitmap duplicates the free list's bookkeeping.

The kernel-friendliness goals contradict each other. MADV_HUGEPAGE/NOHUGEPAGE on sub-ranges, a PROT_NONE guard fallback, mprotect-based commit and mremap all split VMAs, which breaks the "<10 VMAs" target. MADV_GUARD_INSTALL refuses mlock'd VMAs and splits THPs. khugepaged's default max_ptes_none (HPAGE_PMD_NR-1) and the THP underused-page shrinker undo the allocator's subrelease decisions. The default THP defrag=madvise makes MADV_HUGEPAGE regions stall in direct reclaim. Time-decay purging without a background thread never runs while the application is idle.

The fork, signal, TLS, calloc/realloc/aligned-API and drop-in rules are stated as requirements but never designed. The performance goals can't be falsified as written (for example, Redis ships with its own jemalloc and ignores LD_PRELOAD). The roadmap builds per-thread ownership and then replaces it, and it pushes security and the main research bet to the end.

Recommended redesign, all spelled out in the fixes below:
- a single page-based object flow (mimalloc-v3-like) with rseq vCPU slots and lease-based cross-slot work;
- a region table plus a flat page map;
- a PROT_NONE or MAP_NORESERVE reservation strategy chosen by overcommit mode, with growable regions;
- runtime page and THP geometry, and VMA separation by policy;
- a staged release pipeline (cached, MADV_FREE, MADV_DONTNEED) batched with process_madvise;
- lifetime prediction limited to large allocations, explicit and compile-time hints, and a counterfactual mode first;
- a roadmap reordered with the measurement harness and offline trace simulator in Phase 0.

Method note: the web-search budget ran out partway through. Later evidence comes from direct fetches of primary sources: kernel source and docs, man-pages git, the glibc, musl, mimalloc, TCMalloc, snmalloc, LLVM, libstdc++, CPython and Rust source trees, and the LLAMA and snmalloc papers. Several upstream trees used as evidence were found already downloaded in the shared scratchpad by other workflow agents and were treated as data. Claims that couldn't be checked are marked UNVERIFIED. No files in /home/user/sohamalloc were modified.

### 1. [critical] Section 5 overall (5.4 'Metadata always out-of-line, in a separate reserved region'); no pointer-to-metadata lookup is specified anywhere

**Problem.** The design never says how free(p), realloc, malloc_usable_size, free_sized validation or double-free checks find the page or span metadata for an arbitrary pointer. Unspecified: page-map granularity, structure, VA coverage, bounds checking, interior pointers (aligned allocations, large runs), foreign pointers, and tagged pointers (arm64 TBI/MTE). This is the hottest lookup on the free path. It decides free-path cost, whether 'out-of-line metadata' is real, and whether the design survives 52/57-bit address spaces.

**Evidence.** mimalloc v3 needed an explicit page map. The flat variant uses 'one byte per slice (64 KiB) ... large initial virtual reserve (4 GiB with 48 bit virtual addresses)'. The 2-level variant 'is usually 4 MiB (for 48 bit virtual addresses) and points to sub maps of 64 KiB' (https://github.com/microsoft/mimalloc/blob/dev3/include/mimalloc/heap/page-map.h). The offset byte resolves interior pointers: 'the slice is part of a page, starting at ...' (https://github.com/microsoft/mimalloc/blob/dev3/src/heap/page-map.c). snmalloc's pagemap describes 'the entirety of 48-bit address space as a 16MiB flat map, or alternatively as a two level map with 64KiB leafs' (snmalloc ISMM'19 paper, https://www.microsoft.com/en-us/research/uploads/prod/2020/04/snmalloc.pdf). x86 5-level paging enables a 56-bit user VA, and by default the kernel does not allocate above 47 bits unless given a hint (https://docs.kernel.org/arch/x86/x86_64/5level-paging.html). arm64 52-bit VA is opt-in via an mmap hint (https://docs.kernel.org/arch/arm64/memory.html). A flat map over a 57-bit VA is impossible, and a lookup without a range check faults on foreign or high pointers.

**Fix.** Add a 'pointer lookup' section.
(1) Every sohamalloc byte lives in a registered region (heap reservations plus dedicated huge-object VMAs), recorded in a small region table (at most 8 entries; beyond that, a 2-level radix keyed on address bits above 30).
(2) Each region has a flat array with one 8-byte entry per 64 KiB slice (per base page inside large-run slices), committed lazily. Entry = {page-meta index, kind, size class, offset to run start}.
(3) The free path strips the top byte on arm64, range-checks the region, shifts, and does one dependent load.
(4) A miss is a foreign pointer: abort with a diagnostic in debug and secure tiers, and forward to the next allocator only in non-preload builds.
(5) Budget: one dependent load plus two compares. Benchmark against mimalloc's flat map.

### 2. [critical] 5.5 VMA row: 'Reserve a few large regions (for example 64 GiB of virtual address space) at start-up; commit by touching'

**Problem.** A writable 64 GiB reservation that is 'committed by touching' fails, or turns into a commit charge, under common configurations:
- Overcommit mode 0 (the default heuristic) refuses any single request larger than total RAM plus swap, e.g. 64 GiB on a 32 GiB swapless VM, laptop or CI runner.
- Mode 2 (strict) charges the whole 64 GiB against CommitLimit and ignores MAP_NORESERVE, so the process fails at start-up.
- RLIMIT_AS (ulimit -v, common on HPC and judge sandboxes) makes the mmap fail.
With 'commit by touching' under mode 2, malloc can never return NULL; the process just gets OOM-killed, which breaks databases that rely on strict accounting.

**Evidence.** Kernel __vm_enough_memory under OVERCOMMIT_GUESS: 'if (pages > totalram_pages() + total_swap_pages) goto error' (https://github.com/torvalds/linux/blob/master/mm/util.c). accountable_mapping() charges only writable mappings without VMA_NORESERVE or VMA_SHARED (https://github.com/torvalds/linux/blob/master/mm/vma.c). 'We honor MAP_NORESERVE if allowed to overcommit', i.e. not under OVERCOMMIT_NEVER (https://github.com/torvalds/linux/blob/master/mm/mmap.c). Kernel doc: mode 2 commit is limited to 'swap + a configurable amount (default is 50%) of physical RAM'; 'In mode 2 the MAP_NORESERVE flag is ignored'; private read-only anonymous maps have 0 cost (https://docs.kernel.org/mm/overcommit-accounting.html). mprotect charges commit when an unaccounted private mapping becomes writable, and returns -ENOMEM if the charge fails (https://github.com/torvalds/linux/blob/master/mm/mprotect.c). RLIMIT_AS 'affects calls to brk(2), mmap(2), and mremap(2), which fail with the error ENOMEM' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/getrlimit.2).

**Fix.** Choose the reservation strategy at init.
- Modes 0 and 1: reserve PROT_READ|PROT_WRITE with MAP_NORESERVE (not accounted, no mprotect needed).
- Mode 2: reserve PROT_NONE (not accounted) and commit in hugeblock-sized or larger chunks with mprotect(RW). When mprotect returns ENOMEM, malloc returns NULL/ENOMEM, so strict-overcommit users keep NULL semantics.
- Detect the mode by reading /proc/sys/vm/overcommit_memory with raw open/read, or by try-and-fall-back.
- Size the first reservation as min(64 GiB, RLIMIT_AS/4, user VA/8), and add more reservations on demand.
- Commit in large chunks, because each mprotect splits or merges VMAs and takes the mmap write lock.
- Test matrix: overcommit 0/1/2, ulimit -v, a 4 GiB swapless VM.

### 3. [critical] 5.1/5.4/5.5 hard-coded geometry: 'sub-releasing 4 KiB runs', '2 MiB hugeblocks', 64 KiB pages, guard pages, valloc/pvalloc

**Problem.** Base page and hugepage sizes are baked in. arm64 Linux ships 16K and 64K granule kernels:
- PMD-sized THP is 32 MiB (16K granule) or 512 MiB (64K granule), not 2 MiB.
- contiguous-PTE (mTHP) sizes are 64K, 2M and 2M.
- madvise, guard and protection granularity is 16 or 64 KiB, so '4 KiB runs' cannot be released, and every guard costs a full base page.
Allocators that assumed 4K pages broke on such systems. The design gives no runtime discovery, no mTHP strategy and no test plan for these kernels.

**Evidence.** Kernel arm64 hugetlbpage table: 4K granule = CONT PTE 64K, PMD 2M; 16K granule = CONT PTE 2M, PMD 32M; 64K granule = CONT PTE 2M, PMD 512M (https://docs.kernel.org/arch/arm64/hugetlbpage.html). Asahi Linux's broken-software list: hardened_malloc is broken on 16K ('more changes necessary ... before 16k page support is done'); jemalloc is 'Fixed since 5.3.1'; Chromium cppgc 'crashes on Linux w/ 16KiB pages because of hardcoded kGuardPageSize' (https://asahilinux.org/docs/sw/broken-software/). transhuge.rst exposes hpage_pmd_size and per-size hugepages-<size>kB controls for mTHP (https://docs.kernel.org/admin-guide/mm/transhuge.html). Distros shipping 64K arm64 kernels: UNVERIFIED (not checked in this run).

**Fix.** Make geometry runtime parameters:
- base = getauxval(AT_PAGESZ);
- PMD THP = hpage_pmd_size (fallback: base);
- mTHP sizes = the enabled hugepages-*kB entries.
Define hugeblock = PMD size when it is 4 MiB or less, otherwise a 2 MiB contiguous-PTE unit. Slab/page size = max(64 KiB, base). Sub-release granularity = base. Guard size = base. valloc/pvalloc use the runtime page size. Run CI on qemu arm64 kernels with 16K and 64K pages, and refuse to compile in page-size constants.

### 4. [critical] Section 5 diagram plus 5.1 front end ('pop/push on a sharded free list of the current page'), MIDDLE 'per-LLC-domain transfer caches', 5.1 'Elastic cache capacity ... steals capacity from idle caches'

**Problem.** The front end and the middle tier use incompatible object-flow models. With free-list sharding (mimalloc), freed objects go back to their own page's list and the front end allocates by popping from a page. There are no batches of interchangeable objects to move around. A TCMalloc transfer cache holds pointers from arbitrary spans. If one is added:
- objects parked in it keep their pages non-empty, which defeats 'a page that becomes empty is found immediately';
- the front end has no way to consume a batch, because it pops from a page.
'Elastic capacity' in TCMalloc resizes per-CPU pointer arrays. In a page-based cache, capacity means the number of owned partially-free pages, so the mechanism does not carry over. The design as drawn cannot be implemented coherently.

**Evidence.** mimalloc v3 free path: local frees go to page-local lists via mi_free_block_local, and remote frees go to page->xthread_free (https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/free.c). TCMalloc's per-CPU cache is a slab of object pointers with dynamic resizing (https://github.com/google/tcmalloc/blob/master/tcmalloc/cpu_cache.h; design notes at https://github.com/google/tcmalloc/blob/master/docs/design.md).

**Fix.** Pick one model.
(A) Page-based (recommended, mimalloc-v3-like): the unit of transfer is a page; 'NUCA-aware' becomes per-LLC pools of partially-free pages; 'elastic capacity' becomes a cap on partially-free pages owned per slot, plus reclaiming pages from idle slots.
(B) Object-based (TCMalloc-like): per-slot pointer arrays, transfer caches and a central free list with span prioritisation; drop the page-local free-list claims and the 'empty page found immediately' claim.
Redraw the diagram and rewrite 5.1-5.4 consistently.

### 5. [critical] 5.1 'Index caches by the kernel's rseq mm_cid' combined with free-list sharding and per-page thread-free lists (5.2)

**Problem.** A cid slot is not owned by one thread. Every access to slot state (current page, local free list, used count) has to sit inside an rseq critical section that commits with a single store. A mimalloc-style pop updates both the head and the used count, which is two stores. The cid can change at any preemption, and the kernel switches cid ownership between per-task and per-CPU modes at runtime. Cross-slot work also needs rseq fences:
- stealing capacity;
- draining an idle slot's pages;
- collecting thread-free lists of pages owned by a slot nobody runs on.
With virtual CPU ids, the target physical CPU cannot be identified, so the fence must interrupt every CPU running the process. That IPI cost contradicts 'kernel citizenship'. None of this is designed. 'Hand-written assembly per architecture' is acknowledged, but the protocol is not.

**Evidence.** TCMalloc percpu.cc FenceCpu: 'With virtual CPUs, we cannot identify the true physical core we need to interrupt.' It then calls FenceAllCpus(), and it registers MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ (https://github.com/google/tcmalloc/blob/master/tcmalloc/internal/percpu.cc). The kernel mm_cid comment: 'A CID is either owned by a task ... or by a CPU' with runtime mode switches 'when the user count becomes greater than the maximum number of CIDs' (https://github.com/torvalds/linux/blob/master/kernel/sched/core.c). glibc manual: libraries using rseq should clear rseq_cs before returning (https://sourceware.org/glibc/manual/latest/html_node/Restartable-Sequences.html).

**Fix.** Design the slot protocol before any code.
(1) The pop commits with a single store of the new head. Compute used counts lazily at page retirement or collection (capacity minus the free count).
(2) A free goes into the slot's current page through rseq only when the page's owner equals the current cid. Everything else goes to the page's atomic thread-free list.
(3) Cross-slot work (draining an idle slot, rebalancing) uses a CAS-claimed lease word in the slot instead of membarrier fences, so fast-path rseq sections abort on a lease mismatch.
(4) Model-check the protocol (e.g. GenMC or TLA+) and test it under forced preemption (rseq selftests style).
(5) Keep a per-thread mode that uses the identical page-ownership abstraction, so both modes share the remote-free protocol.

### 6. [critical] 5.3 step 2: 'record hash(__builtin_return_address(0)) ... On the fast path, look up the return address in that table'

**Problem.** The immediate return address identifies the wrapper, not the allocation site, for most real allocations:
- libstdc++ declares basic_string<char> as an explicitly instantiated extern template for C++20 and earlier, so every std::string heap allocation in the program comes from one out-of-line _M_create in libstdc++.so;
- libstdc++'s operator new calls malloc from a single loop, so if sohamalloc's operator new is not the one bound, all C++ allocations share one return address;
- Rust's default System allocator calls libc::malloc from one function;
- CPython's obmalloc serves requests of 512 bytes or less from 256 KiB to 2 MiB arenas, so malloc only sees arena-sized calls from one site;
- plus xmalloc, g_malloc and sqlite3Malloc-style wrappers.
One table entry then mixes every lifetime in the program, and the predictor degenerates or mispredicts. LLAMA, which the design claims to simplify, needed return address plus stack height plus size and still saw aliasing. TCMalloc's lifetime allocator uses full stack traces and only for large allocations.

**Evidence.** libstdc++ basic_string.tcc: '# if __cplusplus <= 202002L && _GLIBCXX_EXTERN_TEMPLATE > 0  extern template class basic_string<char>;'; _M_create returns _S_allocate(...) (https://github.com/gcc-mirror/gcc/blob/master/libstdc++-v3/include/bits/basic_string.tcc). new_op.cc: 'while ((p = malloc (sz)) == 0)' (https://github.com/gcc-mirror/gcc/blob/master/libstdc++-v3/libsupc++/new_op.cc). Rust std: 'unsafe { libc::malloc(layout.size()) as *mut u8 }' (https://github.com/rust-lang/rust/blob/master/library/std/src/sys/alloc/unix.rs). CPython: SMALL_REQUEST_THRESHOLD 512, ARENA_BITS 21/20/18 (https://github.com/python/cpython/blob/main/Include/internal/pycore_obmalloc.h). LLAMA §7: 'we compute a hash of the return address, stack height and object size ... stack height identifies C/C++ stack traces with 68% accuracy ... 14% of predictions disagreed with the currently cached value' (https://cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf). TCMalloc lifetime allocator: 'predicting the lifetime of large allocations ... based on the stack trace' (https://github.com/google/tcmalloc/blob/master/docs/lifetime-based-allocator.md; copy at /home/user/sohamalloc/tcm_lifetime.md).

**Fix.** (1) Key = hash(return address, stack depth measured as stack base minus SP, size class), as in LLAMA.
(2) Detect 'funnel' keys (high size entropy or bimodal lifetimes) and pin them to NORMAL permanently.
(3) Provide hint channels that see the real context:
- the C API hint;
- operator new(size_t, __hot_cold_t)-style overloads compatible with TCMalloc's;
- LLVM MemProf, which clones functions to expose allocation contexts and can rewrite operator new into hot/cold variants.
(4) Start automatic prediction only for medium and large allocations (TCMalloc's scope), and extend to small objects only if counterfactual mode shows wins.

### 7. [critical] 5.3 'The return-address lookup costs a few cycles' / 'On the fast path, look up the return address in that table'

**Problem.** A hash-table lookup on every malloc is not 'a few cycles'. On top of the ~20-cycle fast path it adds: computing the hash, a load from a table that competes for L1d, a compare, and selection among three current pages per class (an extra index, and a branch if a miss path exists). LLAMA's own measured cached lookup (thread-local, expected to hit in L1) costs 2.7 times TCMalloc's entire fast path. A 4096-entry by 8-byte table is 32 KiB, a whole L1d on many cores.

**Evidence.** LLAMA Table 1: 'TCMalloc Fast Path (new/delete) 8.3 ns ... Capture full stack trace 396 ns ± 364 ns ... Look up stack hash (Section 7) 22.5 ns'; 'stack tracing adds 14% end-to-end overhead' (https://cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf).

**Fix.** Do not look up on the small-object fast path. Options:
(a) predict only for allocations of 64 KiB or more, or on page-refill slow paths;
(b) for small objects, rely on explicit hints, heaps and compile-time MemProf annotations;
(c) if a fast-path variant is ever tried, use a 256-entry, 1-byte direct-mapped table per slot (256 B), indexed by return-address bits, with no stack hashing.
Gate it on a measured increase of 1 ns or less at p50 and a positive RSS/TLB result in counterfactual mode.

### 8. [major] 5.4 'Large objects ... still inside the reserved region, with no mmap per object' and 5.5 'few huge VA reservations'

**Problem.** Nothing covers allocations larger than the region, region exhaustion, or VA fragmentation inside the region. A single malloc(100 GiB), which is legitimate for in-memory databases and genomics, cannot be served. Large runs of varying size fragment a fixed 64 GiB region, so allocation can fail even though the process has plenty of VA elsewhere. memalign with huge alignment (1 GiB) wastes up to the alignment in region VA. On arm64 kernels with 4K pages and 3-level tables, user VA is only 512 GiB, so a 64 GiB fixed region is one eighth of the whole address space.

**Evidence.** arm64: 'allowing 39-bit (512GB) or 48-bit (256TB) virtual addresses'; with 64KB pages and 2 levels, '42-bit (4TB)' (https://docs.kernel.org/arch/arm64/memory.html). The design text itself fixes one 64 GiB region and offers no growth path.

**Fix.** Make reservations growable: a list of regions registered in the page-map region table. Allocations of at least max(1 GiB, region/16), or with alignment above the hugeblock size, get a dedicated mmap (over-map by the alignment, then munmap the head and tail). munmap them on free. Only these dedicated objects may use mremap. Inside a region, use an address-ordered best-fit structure for large runs, and coalesce on free.

### 9. [major] 5.4 '`realloc` of large blocks uses `mremap` to grow without copying' combined with 'inside the reserved region, with no mmap per object' and 5.5 'No mmap/munmap on any steady-state path'

**Problem.** mremap works on VMAs, not allocator runs, so the three claims contradict each other.
- Growing in place needs the following VA to be unused by the kernel's view. Inside one big region it is mapped (it is the reservation itself or other objects).
- MREMAP_MAYMOVE without MREMAP_FIXED lets the kernel relocate the pages to an address outside the region, so the page map no longer finds them.
- With MREMAP_FIXED into the allocator's own reservation, each move creates separate VMAs (more VMAs) and takes the mmap write lock.
- Moves that are not PMD-aligned split THPs.
mremap is itself a steady-state mm syscall, contradicting 5.5.

**Evidence.** mremap(2): 'If this flag [MREMAP_MAYMOVE] is specified, then the kernel is permitted to relocate the mapping to a new virtual address'; MREMAP_FIXED behaves like MAP_FIXED and 'any previous mapping at the address range specified by new_address and new_size is unmapped' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/mremap.2). The mremap syscall takes mmap_write_lock_killable (https://github.com/torvalds/linux/blob/master/mm/mremap.c). glibc only uses mremap for chunks that are their own mmap.

**Fix.** Restrict mremap to objects in dedicated VMAs (the huge-object class above). For in-region large runs:
(a) grow in place if the following run is free in the page heap, which is a pure metadata operation;
(b) otherwise allocate and copy, using non-temporal stores above an LLC-sized threshold;
(c) shrink by releasing the tail with MADV_DONTNEED and splitting the run.
Optionally use MREMAP_FIXED|MREMAP_MAYMOVE between hugeblock-aligned ranges inside the reservation, and only after a VMA-count benchmark shows no growth.

### 10. [major] 5.5 'Ideally fewer than 10 VMAs for the whole heap' and the 'Kernel-friendly: few large VMAs' requirement

**Problem.** The design's own mechanisms split VMAs:
- MADV_HUGEPAGE/NOHUGEPAGE on sub-ranges ('MADV_HUGEPAGE ... for hot blocks', adapting THP);
- the PROT_NONE guard fallback;
- mprotect-based commit (needed under strict overcommit);
- mremap moves;
- 'optional mbind per node arena' (per-VMA memory policy);
- MADV_DONTFORK/WIPEONFORK if used for caches.
There is also a hidden cost in the goal itself. With only a few giant VMAs, any VMA-modifying call (mprotect, MADV_HUGEPAGE, mremap) takes the mmap write lock and stalls page faults across the whole heap. Meanwhile DONTNEED, FREE and GUARD_INSTALL now need only a per-VMA read lock.

**Evidence.** mm/madvise.c: 'madvise_update_vma will handle splitting a vm area into separate areas, each area with its own behavior'. MADV_HUGEPAGE and MADV_NOHUGEPAGE go through madvise_update_vma and take MADVISE_MMAP_WRITE_LOCK (the default case). MADV_DONTNEED, MADV_FREE and MADV_GUARD_INSTALL use MADVISE_VMA_READ_LOCK (https://github.com/torvalds/linux/blob/master/mm/madvise.c). hardened_malloc, which relies on PROT_NONE guards, tells users 'It can be necessary to substantially increase the vm.max_map_count sysctl' (https://github.com/GrapheneOS/hardened_malloc/blob/main/README.md).

**Fix.** Specify the VMA layout explicitly and separate VMAs by policy at reservation time:
- a THP-eligible small/medium-object region (MADV_HUGEPAGE set once);
- a NOHUGEPAGE region for sparse, sampled or guarded memory;
- one metadata region;
- one reservation per NUMA node if mbind is used;
- dedicated huge-object VMAs.
Never change VMA flags or protections on sub-ranges at runtime in tier 0. Use MADV_GUARD_INSTALL only. Add a CI gate on /proc/self/maps line count over a long run.

### 11. [major] 5.5 Hugepages row: 'MADV_HUGEPAGE on the heap region. Use MADV_COLLAPSE (6.1+) for hot, dense hugeblocks when THP is set to madvise' and 5.4 'sub-releasing 4 KiB runs under real pressure'

**Problem.** The THP policy fights the kernel in four ways, and one stated condition is wrong.
(a) With the default defrag=madvise, every first-touch fault in a MADV_HUGEPAGE region may enter direct reclaim and compaction. That is a tail-latency hazard and contradicts the latency profile.
(b) With MADV_HUGEPAGE (or THP=always), the first touch of a fresh 2 MiB-aligned block faults in a whole hugepage. Small processes, and slots whose first pages land in different hugeblocks, inflate RSS by 2 MiB per block.
(c) khugepaged's default max_ptes_none is HPAGE_PMD_NR-1 (511 with 4 KiB pages), so it re-collapses hugeblocks the allocator partially released, re-faulting the memory just returned.
(d) The underused-THP shrinker splits THPs with more than max_ptes_none zero pages under pressure, undoing hugeblock choices.
(e) MADV_COLLAPSE ignores the sysfs THP mode, so the 'when THP is set to madvise' condition is meaningless. It is also a synchronous copy into a newly allocated hugepage and must not run on a malloc slow path.
PR_SET_THP_DISABLE, which is inherited across fork/exec, is not considered.

**Evidence.** transhuge.rst: defrag 'madvise will enter direct reclaim like always but only for regions that are have used madvise(MADV_HUGEPAGE). This is the default behaviour'; 'MADV_COLLAPSE ignores these settings and collapses ranges to PMD-sized huge pages unconditionally'; THPs 'will be split under memory pressure if they are considered underused ... above max_ptes_none'; PR_SET_THP_DISABLE 'is inherited across fork(2) and execve(2)' (https://docs.kernel.org/admin-guide/mm/transhuge.html). khugepaged.c: '#define KHUGEPAGED_MAX_PTES_LIMIT (HPAGE_PMD_NR - 1)', assigned as the default max_ptes_none (https://github.com/torvalds/linux/blob/master/mm/khugepaged.c). madvise(2): MADV_COLLAPSE 'Perform a best-effort synchronous collapse ... copied onto a freshly allocated hugepage' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/madvise.2). TCMalloc's filler tracks 'collapse_skipped_due_to_backoff', i.e. it rate-limits collapse (https://github.com/google/tcmalloc/blob/master/tcmalloc/huge_page_filler.h, local copy /home/user/sohamalloc/hpf.h).

**Fix.** (a) Pack all slots' first pages and small-process heaps into the same hugeblocks (bootstrap block).
(b) Set MADV_HUGEPAGE only on the dense-object region, and keep sparse or partially released blocks in a NOHUGEPAGE region chosen at reservation time (no runtime toggling).
(c) At init, read hpage_pmd_size, khugepaged/max_ptes_none, shrink_underused and PR_GET_THP_DISABLE, all with raw syscalls. If max_ptes_none is high, treat sub-release inside THP-eligible blocks as temporary and prefer whole-block release.
(d) Run MADV_COLLAPSE only from the optional background thread, with backoff and a per-second budget.
(e) Measure first-touch fault latency (p99) with THP defrag=madvise in CI.

### 12. [major] 5.1 'Size classes: about 4 per power of two, so internal waste is at most about 12.5%. Classes are exact for 8 KiB and below, and a 1-byte-indexed table maps sizes up to 1 KiB'

**Problem.** The arithmetic is wrong. With 4 classes per doubling (2^k x {1, 1.25, 1.5, 1.75}), a request of 2^k+1 rounds up to 1.25x2^k, so the worst-case waste is about 20% of the block (25% of the request). A 12.5% bound needs 8 classes per doubling, which doubles the number of partially filled current pages per slot (and per lifetime class). 'Exact for 8 KiB and below' also contradicts '4 per power of two', and the 1 KiB table's quantum is undefined.

**Evidence.** jemalloc manual: 'four size classes for each doubling in size, which limits internal fragmentation to approximately 20% for all but the smallest size classes' (https://github.com/jemalloc/jemalloc/blob/dev/doc/jemalloc.xml.in, https://jemalloc.net/jemalloc.3.html). mimalloc's '~12.5% worst internal fragmentation' comment sits on code computing bin = ((b << 2) + ((wsize >> (b - 2)) & 0x03)) - 3, which is also 4 bins per doubling (https://github.com/microsoft/mimalloc/blob/main/src/page-queue.c). Own computation (scratch script): 57 classes from 16 B to 256 KiB, worst waste 20.0% of the block.

**Fix.** Choose and state the scheme explicitly. For example: 16-byte spacing up to 128 B, then 4 per doubling (worst about 20%, mean about 10%), or 8 per doubling up to 8 KiB if measured external fragmentation allows it. Derive the class list from request-size histograms of the benchmark set, as TCMalloc derives its classes from profiles. State the bound as 'at most X% of the block, mean Y% on workload Z'. Define the lookup table as indexed by (size+15)>>4 up to 1 KiB.

### 13. [major] 5.1/5.4 class-to-page geometry ('current page per size class', 64 KiB pages, 'Anything above about 256 KiB goes straight to the page heap')

**Problem.** Nothing says how many bytes back each class. With 64 KiB pages and 4-per-doubling classes above 8 KiB, the tail waste is large: a 40 KiB class holds 1 object and wastes 24 KiB (37.5%); 24 KiB and 48 KiB classes waste 25%; 14/28/56 KiB classes waste 12.5%. Objects from 64 KiB up to 256 KiB cannot fit in a 64 KiB page at all, so medium page kinds are missing.

**Evidence.** Own computation (python, scratch sc.py): 64 KiB page tail waste is 10 KiB 6.2%, 14 KiB 12.5%, 24 KiB 25.0%, 28 KiB 12.5%, 40 KiB 37.5%, 48 KiB 25.0%, 56 KiB 12.5%. mimalloc and TCMalloc both use per-class span lengths or multiple page kinds (mimalloc small/medium/large pages; TCMalloc pages-per-span per class), per their sources (https://github.com/microsoft/mimalloc, https://github.com/google/tcmalloc).

**Fix.** Generate a per-class span length (a multiple of the runtime base page) minimising tail waste to 1/8 or less, subject to at least 8 objects per span for classes up to 8 KiB. Define page kinds: small (64 KiB or base), medium (512 KiB to the hugeblock size) for 8 to 256 KiB classes, and page-heap runs above that. Publish the generated table and its waste.

### 14. [major] 5.1 'Its values are dense and bounded by min(#threads, #CPUs)' and 6.2 'mm_cid-indexed caches' listed as a differentiator

**Problem.** (a) It is not new: TCMalloc already has an mm_cid virtual-CPU mode.
(b) The bound is stated wrong for the current kernel: max_cids = min(1.25 x min(nr_cpus_allowed, users), num_possible_cpus), so up to 25% more slots than threads, bounded by possible (not online) CPUs.
(c) The semantics changed recently. In per-task mode (few threads) the cid follows the thread, so 'per-slot' caches behave like per-thread caches: no per-CPU L1/L2 warmth, and NUCA locality is lost when the thread migrates.
(d) Availability: requires 6.3+; must be detected via AT_RSEQ_FEATURE_SIZE; glibc registration can fail or be disabled (__rseq_size==0); musl has no rseq registration (UNVERIFIED beyond a grep of pthread_impl.h).

**Evidence.** TCMalloc: RseqVcpuMode::kMM behind Experiment::TEST_ONLY_MM_VCPU; 'mm_cid support was introduced in Linux 6.3'; detection 'if (auxv >= offsetof(kernel_rseq, mm_cid) + sizeof(__rseq_abi.mm_cid))' (https://github.com/google/tcmalloc/blob/master/tcmalloc/internal/percpu.cc). Kernel: 'opt_cids = min(mm_cid::nr_cpus_allowed, mm_cid::users); max_cids = min(1.25 * opt_cids, num_possible_cpus());' (https://github.com/torvalds/linux/blob/master/kernel/sched/core.c). The ownership rework was committed to tip core/rseq on 2025-11-25 (https://lkml.iu.edu/2511.3/03605.html); which mainline release carries it is UNVERIFIED. uapi rseq.h states only 'allocated uniquely within a memory map', with no thread or CPU bound (https://github.com/torvalds/linux/blob/master/include/uapi/linux/rseq.h). glibc: __rseq_size is 'zero (if restartable sequence registration failed or has been disabled)' (https://sourceware.org/glibc/manual/latest/html_node/Restartable-Sequences.html).

**Fix.** Remove the novelty claim and cite TCMalloc. Read both cpu_id and mm_cid: index slots by mm_cid, but choose LLC/NUMA page pools by cpu_id at refill time. Detect support via AT_RSEQ_FEATURE_SIZE, and fall back to self-registration when __rseq_size==0 (musl, disabled glibc). Benchmark per-CPU, per-cid and per-thread modes on (4 threads, 256 CPUs), (10k threads, 64 CPUs) and (cgroup cpuset of 2 CPUs), across kernels before and after the rework.

### 15. [major] 5.3 step 2 sampling ('about 1 in every 2 MiB allocated ... at free time, the object's lifetime') and step 4 safety valve

**Problem.** The lifetime-learning loop is underspecified and biased.
- Byte-based sampling almost never samples small, short-lived objects: a site allocating 32-byte objects is sampled about once per 65,536 allocations.
- Measuring lifetime needs a timestamp per sampled object and a way for free() to recognise sampled objects, which is an extra check on the free path.
- Classifying LONG needs survivor tracking, because objects that are never freed never report.
- Missing: table eviction, concurrent updates, the threshold T, hysteresis, program phase changes and fork behaviour.
- 'Demote that call-site if a SHORT span is still live after T' needs periodic span scans, i.e. maintenance work with no scheduler.

**Evidence.** The TCMalloc lifetime allocator needs a LifetimeDatabase (LRU plus refcount, bounded) and LifetimeTrackers 'strung together in a linked list sorted by allocation timestamp ... checked on every operation', with T = 0.5 s default and a Counterfactual mode that measures decisions without acting on them (https://github.com/google/tcmalloc/blob/master/docs/lifetime-based-allocator.md; /home/user/sohamalloc/tcm_lifetime.md).

**Fix.** Adopt TCMalloc's structure:
- sampled objects are allocated in a distinguishable page kind, so free() already branches on kind with no extra check;
- binary SHORT/LONG with T = 0.5 s;
- a sorted tracker list to classify survivors;
- a bounded database with LRU eviction;
- a counterfactual mode as the first deliverable.
Add count-based per-site sampling for small classes, and rebuild the database in the fork child.

### 16. [major] 5.3 step 3 'Each lifetime class gets its own spans and hugepages' and 9 'Lifetime grouping vs size-class density'

**Problem.** Lifetime classes multiply the number of partially filled current pages. Per slot there are classes x 3 lifetime classes, before occupancy buckets, LLC domains and NUMA nodes. Even with one touched base page per (class, lifetime, slot):
- 30 active classes x 3 x 16 slots = 1,440 pages = 5.6 MiB on 4 KiB kernels;
- 90 MiB on 64 KiB-page kernels;
before holding any useful data. This directly attacks the 'low RSS' goal, and the risk is mentioned but not bounded.

**Evidence.** Arithmetic from the design's own structure (current page per size class per cache, 3 lifetime classes); base page sizes from https://docs.kernel.org/arch/arm64/hugetlbpage.html. LLAMA itself segregates by lifetime at the block/hugepage level ('Llama assigns each huge page a predicted LC'), not per size-class page (https://cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf).

**Fix.** Segregate by lifetime at the hugeblock level only. Divert only SHORT (two classes: SHORT and DEFAULT) and only for classes with enough volume. Cap the number of active (class, lifetime) current pages per slot, and fall back to DEFAULT when the cap is hit. Report the per-slot idle-current-page bytes in the explain report.

### 17. [major] 5.4 'Occupancy-prioritised span lists ... Allocate from the fullest span first' and '2 MiB hugeblocks are filled densest-first, as in Temeraire'

**Problem.** (1) 'Fullest first' conflicts with free-list-sharding locality and cost.
- The fullest page has the fewest free objects, scattered in holes, so consecutive allocations touch more lines and pages and the slot exhausts pages faster, meaning more slow-path transitions.
- Keeping 8 occupancy lists exact means re-bucketing pages on frees that cross bucket boundaries, including remote frees that would need locks or atomics.
- The occupancy of pages with uncollected thread-free lists is stale.
(2) Temeraire is misdescribed. Its filler orders hugepages by longest free range, then by quantised allocation count, with freshly donated tails ranked below used pages. Google documents that the simpler ordering caused bad page-heap fragmentation for some workloads.

**Evidence.** TCMalloc huge_page_filler.h: 'The above suggests using the hugepage with the shortest longest empty range, breaking ties in favor of fewest number of allocations. This works well for most workloads but caused bad page heap fragmentation for some: b/63301358 and b/138618726 ... we treat hugepages that are freshly donated as less preferable'; 'using the number of allocations works substantially better than the number of allocated pages' (https://github.com/google/tcmalloc/blob/master/tcmalloc/huge_page_filler.h; local copy /home/user/sohamalloc/hpf.h). The locality cost of fullest-first under page-local free lists is a first-principles argument, UNVERIFIED by measurement.

**Fix.** Order pages only at refill time (slow path), with lazy bucketing at page retirement and thread-free collection. Prefer the fullest page among those with at least 1/8 free, so a refill yields a contiguous run. For hugeblocks, copy Temeraire's actual ordering (longest free range, then allocation count, donated tails last). Measure LLC and dTLB misses against plain FIFO and LIFO page reuse in the benchmark suite.

### 18. [major] 5.2 'If a heap's remote-free rate passes a threshold ... switch that heap to batched message passing'

**Problem.** The adaptive switch is underspecified and racy.
- The rate is measured by the receiver, but the sender must act, so every remote free reads a per-owner mode flag (an extra contended cache line).
- During and after a switch, objects arrive on two channels and the owner must drain both.
- With cid slots, the 'heap' is a slot, so whoever next runs on that cid drains it. Idle slots never collect, so memory is stranded.
- Sender-side buffers hold freed memory that is neither reusable nor releasable (RSS), and must be flushed at thread exit, fork and slow paths.
- Frees into full pages need a notification path (mimalloc has explicit full-page and abandoned-page reclaim logic).
- snmalloc-style MPSC enqueue is two-step, and its own code admits a fork mid-enqueue can detach the queue.

**Evidence.** mimalloc v3 free.c: 'page is local, but is full ... use generic path'; abandoned pages are reclaimed or re-abandoned 'if it went to < 7/8th used' (https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/free.c). snmalloc freelist_queue.h: 'The following non-linearisable effect is normally benign, but could lead to a remote list become completely detached during a fork in a multi-threaded process ... An alternative would be to reset the queue on the child postfork handler' (https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/mem/freelist_queue.h).

**Fix.** Use one channel. Remote frees always go to the per-page thread-free list. 'Message passing' becomes sender-side batching by page: buffer remote frees per destination page in the sender's slot, then push each page batch with one CAS. That captures most of snmalloc's amortisation with no mode switch. Bound sender buffers (e.g. 64 objects or 32 KiB per slot) and flush on slow paths, thread exit and fork prepare. Add a 'has-remote-frees' bit set by the first remote free into a full page, and lease-based collection of idle slots.

### 19. [major] 5.4 'Metadata always out-of-line ... A heap overflow cannot corrupt allocator state ... Data pages stay clean ... snmalloc fits each 64 KiB slab's metadata in 64 bits'

**Problem.** This contradicts the design itself. Free lists are inline: XOR-encoded next pointers stored in freed objects (5.6 tier 0). So an overflow can corrupt allocator state; encoding makes forgery harder, not impossible. Releasing a page whose free list threads through it requires list reset or lazy re-initialisation, i.e. housekeeping. The snmalloc figure is misapplied: in the 2019 paper, 64 bits is the bump-pointer-plus-free-list descriptor per slab, with free lists inline and metadata in the first 64 KiB of each superslab. It does not cover sohamalloc's per-page owner, thread-free list, counts, class, occupancy links, lifetime, known-zero bits and double-free bitmap.

**Evidence.** snmalloc paper: 'we use a data structure that combines bump pointer style allocation with a free list ... terminate it with the high-water mark ... This representation allows us to use just 64 bits of meta-data for each 64KiB slab'; 'The first 64KiB of a superslab is special as it contains the meta-data' (https://www.microsoft.com/en-us/research/uploads/prod/2020/04/snmalloc.pdf).

**Fix.** State the threat model precisely: metadata structures are out-of-line; free-list links are inline but encoded with per-page keys and validated on pop (target inside the page and class-aligned). Specify page-metadata layout and size (e.g. 64-128 B per 64 KiB page, about 0.1-0.2%), and how lists are reset when a page is released or reused. Remove the snmalloc 64-bit claim.

### 20. [major] 5.6 Tier 0: 'Cheap double-free detection through a per-span free bitmap' with target cost ≤ 1-2%

**Problem.** A free bitmap alongside a free list is duplicated bookkeeping on every operation.
- Every malloc clears and every free sets a bit in out-of-line metadata, an extra cache line per operation.
- Remote frees need an atomic OR, or detection is deferred until collection.
- At 1 bit per 16 bytes it costs 512 B per 64 KiB (0.8% of memory), the exact overhead snmalloc designed away.
Allocators that use bitmaps (hardened_malloc) use them instead of free lists. TCMalloc deliberately does not validate small-object sizes at free time for cost reasons. The 1-2% figure is unmeasured.

**Evidence.** snmalloc paper: a bitmap 'would require 512 bytes of meta-data' per 64 KiB (https://www.microsoft.com/en-us/research/uploads/prod/2020/04/snmalloc.pdf). hardened_malloc: 'Every slab allocation slot has a dedicated bit in a bitmap tracking whether it's free' (bitmap is the allocator) (https://github.com/GrapheneOS/hardened_malloc/blob/main/README.md). TCMalloc: 'For small objects, it's too expensive to validate the size argument at the time of deallocation. Instead, tcmalloc validates the sizes only at some later point' (https://github.com/google/tcmalloc/blob/master/docs/mismatched-sized-delete.md).

**Fix.** Tier 0 has no bitmap. Use cheap checks: decoded next pointer stays inside the page and is class-aligned, the pushed block differs from the list head, and full validation happens when a page is collected or retired. Tiers 1 and 2 switch the page to bitmap allocation (the bitmap replaces the free list), hardened_malloc-style. Measure each check's cost separately.

### 21. [major] 5.5 Guard pages row ('MADV_GUARD_INSTALL ... fall back to PROT_NONE') and 5.6 tier 1 'guard pages between spans', tier 2 'guard page per large allocation'

**Problem.** Guards break other goals.
- MADV_GUARD_INSTALL returns EINVAL on mlock'd VMAs, so mlockall(MCL_FUTURE) programs (real-time, audio, some databases) lose guards or fail.
- It zaps any THP-mapped PMD in its range, so guards inside hugeblocks destroy hugepages: 'guard pages between spans' in a hugepage-first heap contradicts itself.
- The PROT_NONE fallback uses mprotect, which splits VMAs (against the fewer-than-10-VMAs goal) and takes the mmap write lock.
- Guard size is one base page, i.e. 64 KiB on 64K kernels.

**Evidence.** madvise(2): MADV_GUARD_INSTALL (since 6.13) 'The mapping must not be mlock'd, map hugetlb ranges, nor contain special mappings ... An EINVAL error is returned' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/madvise.2). mm/madvise.c: is_valid_guard_vma disallows VM_LOCKED; guard_install_pmd_entry '/* If huge return >0 so we abort the operation + zap. */' (https://github.com/torvalds/linux/blob/master/mm/madvise.c).

**Fix.** Put guards only at hugeblock boundaries, or in a dedicated NOHUGEPAGE 'guarded' region holding GWP-ASan samples and tier-1+ spans. Detect VM_LOCKED via mlockall usage or an EINVAL result, and fall back to 'reserved but never handed out' slots. Drop the PROT_NONE fallback in tiers 0 and 1 (degrade to no guard and log once). Size guards from AT_PAGESZ.

### 22. [major] 5.5 RSS honesty row: 'MADV_DONTNEED by default (RSS drops, container-safe). MADV_FREE in the throughput profile or when there is no cgroup limit' and 9 'MADV_DONTNEED by default'

**Problem.** DONTNEED is a reasonable final step near cgroup limits, since lazily-freed pages stay charged and memory.high throttles. But tying the primitive to a profile instead of the memory's age is the wrong axis.
- MADV_FREE is cheaper for memory likely to be reused soon: no refault or zeroing unless the kernel actually reclaimed it.
- Both primitives split THPs on sub-ranges.
- MADV_DONTNEED fails with EINVAL on locked pages, which breaks known-zero bookkeeping if return codes are not checked.
- The batching opportunity is missed: process_madvise on self accepts DONTNEED and FREE, and mainline handles the whole iovec with one mmu_gather (batched TLB flush). DONTNEED and FREE now need only per-VMA read locks.
- 'RSS honesty' can be reported anyway: smaps exposes LazyFree.

**Evidence.** madvise(2): 'MADV_DONTNEED cannot be applied to locked pages'; MADV_FREE 'freeing could be delayed until memory pressure occurs ... the free operation will be canceled if the caller writes into the page' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/madvise.2). mm/madvise.c: process_madvise only restricts remote mms (process_madvise_remote_valid); vector_madvise uses a single 'struct mmu_gather tlb' across the iovec; DONTNEED/FREE use MADVISE_VMA_READ_LOCK (https://github.com/torvalds/linux/blob/master/mm/madvise.c). cgroup-v2: memory.high 'processes of the cgroup are throttled and put under heavy reclaim pressure' (https://docs.kernel.org/admin-guide/cgroup-v2.html). proc.rst: '"LazyFree" shows the amount of memory which is marked by madvise(MADV_FREE)' (https://docs.kernel.org/filesystems/proc.html).

**Fix.** Use a three-stage, age- and pressure-driven pipeline:
1. cached dirty free memory (decay window t1);
2. MADV_FREE stage, skipped when cgroup headroom is below X% or memory.high is near;
3. MADV_DONTNEED after t2 or under pressure, whole hugeblocks first.
Check every return code and fall back to MADV_DONTNEED_LOCKED or 'not released' bookkeeping. Issue releases through process_madvise vectors where available (pidfd of self), else a coalesced madvise. Report LazyFree separately in the explain report.

### 23. [major] 4.5 'No busy background thread' plus 5.5 'Maintenance runs piggy-backed on slow paths ... plus one optional, mostly-sleeping thread that wakes on a PSI trigger fd' plus the decay timer

**Problem.** Time decay without a timer does not work for idle programs. If the application stops allocating, slow paths never run, and memory is never returned exactly when it should be. A PSI-only thread covers 'idle and under pressure' but not 'idle, no pressure, wasting RSS'. A hidden allocator thread also has costs:
- it makes the process multithreaded, so unshare(CLONE_NEWUSER) fails (container runtimes, sandboxes);
- it vanishes in fork children;
- it can trip seccomp filters on clone.

**Evidence.** jemalloc TUNING.md: background threads mean 'unintended purging delay caused by application inactivity is avoided' (https://github.com/jemalloc/jemalloc/blob/dev/TUNING.md). unshare(2): 'CLONE_NEWUSER requires that the calling process is not threaded' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/unshare.2).

**Fix.** Start a timer-driven maintenance thread lazily, only once the process is already multithreaded and its heap exceeds a threshold (e.g. 64 MiB of cached free memory). Never start one in single-threaded processes; there, do 'release on the next call after idle' using a cheap timestamp check every N slow-path events. Provide soham_release() and an opt-out. Restart lazily in the fork child, and document the unshare interaction.

### 24. [major] 5.5 'When to release' row: read memory.high/max/events and /proc/pressure/memory (PSI)

**Problem.** The pressure-feedback design is wrong in the container case it targets.
- Inside a cgroup, the relevant signal is the cgroup's memory.pressure, not system-wide /proc/pressure/memory.
- Unprivileged PSI triggers must use windows that are multiples of 2 s, so reaction is coarse.
- PSI can be compiled out or default-disabled (needs psi=1).
- memory.max and memory.high exist only on non-root cgroups, and the effective limit is the minimum over ancestors.
- memory.current includes page cache and other processes, which the allocator cannot reclaim.
- These files must be read with raw syscalls; glibc warns replacement allocators away from fopen and opendir.
- Sandboxes and seccomp may block the opens.
- Per-process PSI triggers cost the kernel polling work (10 times per window during stalls).

**Evidence.** psi.rst: 'Unprivileged users can also create monitors, with the only limitation that the window size must be a multiple of 2s'; 'The kernel accepts window sizes ranging from 500ms to 10s'; stall-state monitoring 'at a rate of 10 times per tracking window' (https://docs.kernel.org/accounting/psi.html). init/Kconfig PSI_DEFAULT_DISABLED: 'can be enabled through passing psi=1' (https://github.com/torvalds/linux/blob/master/init/Kconfig). cgroup-v2: memory.max/high 'exists on non-root cgroups'; memory.events 'a value change in this file generates a file modified event'; memory.pressure 'Shows pressure stall information for memory' (https://docs.kernel.org/admin-guide/cgroup-v2.html). glibc: 'fopen, opendir, dlopen, and pthread_setspecific use malloc internally' (https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html). Which distros default PSI off: UNVERIFIED.

**Fix.** At init (raw open/read only):
- resolve the cgroup path from /proc/self/cgroup;
- read memory.max and memory.high up the hierarchy, stopping at the cgroupns root;
- note v1 versus v2.
With the background thread: poll memory.events (file-modified events) and the cgroup's memory.pressure trigger with 2 s windows. Without it: sample memory.current at most every 100 ms from slow paths. Base headroom on memory.current versus the effective limit. Any failure silently disables the feature and shows up in soham_stats().

### 25. [major] 5.5 fork row: 'pthread_atfork handlers take all allocator locks. The child resets per-slot caches.'

**Problem.** (1) Taking locks does not cover lock-free multi-step operations in flight in other threads at fork time: MPSC enqueue, thread-free CAS loops, lease claims. The child inherits a half-done state that no thread will ever finish.
(2) 'Resets per-slot caches' leaks every other parent thread's cached pages in long-lived children (prefork servers, Redis BGSAVE, Python multiprocessing).
(3) Handler order matters, and lazy registration can come too late.
(4) The background thread and PSI fds do not survive, and per-process secrets and keys are inherited.

**Evidence.** snmalloc freelist_queue.h fork comment (above) and snmalloc's PreventFork counter 'used to prevent a process from forking. Holding a lock in the allocator while forking can lead to deadlocks' (https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds_aal/prevent_fork.h). pthread_atfork(3): 'the prepare handlers are called in reverse order of registration; the parent and child handlers are called in the order of registration' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man3/pthread_atfork.3).

**Fix.** Register atfork handlers at the very first allocator init.
In prepare: take page-heap and central locks, and wait out in-flight multi-step operations with a PreventFork-style counter (cost is only on remote-free and refill paths).
In the child:
- re-initialise every MPSC and thread-free head that could be detached;
- re-home all slots' pages to the child's single slot, or to a reclaimable abandoned pool, instead of leaking them;
- re-key pages lazily;
- restart maintenance lazily;
- re-create PSI fds.
Add tests that fork from 64 threads mid-free and check the child for leaks and deadlocks.

### 26. [major] 5.5 'calloc' row, 5.7 API list, 11/L11 'realloc', requirement 8; zero-size, overflow, alignment semantics never specified

**Problem.** The drop-in API semantics are not designed. Missing:
- calloc n*size overflow checks;
- rejecting sizes above PTRDIFF_MAX (glibc fails every allocation function above it);
- rounding overflow (size + alignment), the source of past memalign-family CVEs;
- malloc(0) returning a unique non-NULL pointer;
- realloc(p, 0) (glibc frees p and returns NULL);
- realloc failure leaving the old block intact;
- posix_memalign EINVAL rules, and aligned_alloc with a size that is not a multiple of the alignment (glibc deliberately allows it);
- memalign with alignment above a page or above a hugeblock, and free() of the interior pointer returned;
- free preserving errno, which glibc requires and which the design's in-free madvise maintenance would clobber;
- the cases where known-zero is false: MADV_FREE'd pages, failed DONTNEED on locked pages, and MTE retagging.

**Evidence.** glibc NEWS 2.30: allocation functions 'fail now with total object size larger than PTRDIFF_MAX'. glibc malloc.c: REALLOC_ZERO_BYTES_FREES 1, 'the realloc call returns NULL ... p is freed'. glibc NEWS: CVE-2013-4332 'pvalloc, valloc, memalign, posix_memalign and aligned_alloc functions could allocate too few bytes or corrupt the heap when passed very large allocation size values'; aligned_alloc 'deliberately allows the size parameter to not be a multiple of the alignment' (https://sourceware.org/git/?p=glibc.git;a=blob;f=NEWS ; https://sourceware.org/git/?p=glibc.git;a=blob;f=malloc/malloc.c). posix_memalign(3): EINVAL if 'not a power of two, or was not a multiple of sizeof(void *)' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man3/posix_memalign.3). glibc manual: 'the replacement free should preserve errno' (https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html).

**Fix.** Add an 'API semantics' section with glibc-compatible behaviour per entry point:
- __builtin_mul_overflow and __builtin_add_overflow on every size computation;
- a PTRDIFF_MAX cap;
- aligned allocations up to the hugeblock size served by over-allocation inside a class or run, recorded via the page-map offset field; larger ones by dedicated mmap;
- save and restore errno around any syscall reachable from free.
Ship a Phase-0 conformance suite: glibc malloc tests, a custom edge-case suite, and differential fuzzing against glibc semantics.

### 27. [major] 5.7 Drop-in: 'LD_PRELOAD ... or static linking. Overrides all of: ...' and requirement 7

**Problem.** Interposition rules are missing or wrong:
- glibc requires initial-exec TLS for replacement allocators and forbids calling fopen, opendir, dlopen or pthread_setspecific internally;
- static linking must put the library before libc.a with the complete function set;
- mallopt, mallinfo2, malloc_trim, malloc_stats and malloc_info are not overridden, so programs calling malloc_trim do nothing useful;
- reallocarray is not a supported replacement point;
- on musl, replacing malloc without aligned_alloc makes aligned_alloc fail with ENOMEM, and musl provides no rseq registration;
- a direct reference to __rseq_offset (added in glibc 2.35) prevents loading on older glibc (RHEL 8 ships 2.28: UNVERIFIED);
- programs that statically link their own allocator ignore LD_PRELOAD.

**Evidence.** glibc manual Replacing malloc: minimum set malloc/free/calloc/realloc; static linking 'must be linked in before linking against libc.a'; 'If the replacement uses thread-local storage, it must use the initial-exec TLS model'; 'reallocarray and strdup aren't supported for replacement'; mallopt/mallinfo2 'will not have any effect or return incorrect statistics' (https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html). musl dynlink.c sets __malloc_replaced and __aligned_alloc_replaced, and mallocng glue.h '#define DISABLE_ALIGNED_ALLOC (__malloc_replaced && !__aligned_alloc_replaced)', so aligned_alloc returns ENOMEM (https://git.musl-libc.org/cgit/musl/tree/ldso/dynlink.c ; https://git.musl-libc.org/cgit/musl/tree/src/malloc/mallocng/glue.h). glibc 2.35 NEWS introduced __rseq_offset/__rseq_size (https://sourceware.org/git/?p=glibc.git;a=blob;f=NEWS).

**Fix.** Write a compatibility matrix (glibc 2.17+, musl 1.2+, static, static-pie, LD_PRELOAD, dlopen) with tests. Use initial-exec TLS and raw syscalls everywhere in init. Make __rseq_* references weak, with a self-registration fallback. Implement mallopt (accept and ignore), mallinfo/mallinfo2 (from stats), malloc_trim mapped to soham_release, malloc_stats and malloc_info (XML). Override aligned_alloc on musl. Document that statically bundled allocators must be rebuilt (e.g. Redis with MALLOC=libc).

### 28. [major] 8 'Language: C11 with <stdatomic.h>, no dependencies' versus 5.7 'every C++ new/delete overload'

**Problem.** Throwing operator new cannot be implemented correctly in plain C. It needs std::bad_alloc and the new_handler retry loop from the C++ runtime. A C-only build has to abort() on out-of-memory, which breaks programs that catch bad_alloc.

**Evidence.** mimalloc's C override declares a weak _ZSt15get_new_handlerv returning NULL, and on failure: 'if (!nothrow) { abort();  // cannot throw in plain C, use abort' (https://github.com/microsoft/mimalloc/blob/main/src/alloc.c). libstdc++ operator new loops on get_new_handler() and throws bad_alloc (https://github.com/gcc-mirror/gcc/blob/master/libstdc++-v3/libsupc++/new_op.cc).

**Fix.** Keep the core in C11, but ship a small C++ translation unit (libsohamalloc_cxx, or the same .so built with a C++ compiler) that defines every replaceable operator new and delete: throwing, nothrow, aligned, sized, plus the extensions __size_returning_new and the hot/cold overloads. It depends only on the platform's C++ ABI library. Test catching bad_alloc and the new_handler loop.

### 29. [major] 5.7 'free_sized, free_aligned_sized ... sized delete' and 3/L6 (the premise that size information speeds free)

**Problem.** In a free-list-sharded design, size information gives no speed benefit. The page is needed anyway to push onto its list, so the page-map lookup cannot be skipped. Trusting the size is dangerous: mismatched sized delete is a common bug, and validating it costs exactly what was supposedly saved. The extended API also misses existing size-feedback ABIs that standard libraries can use today.

**Evidence.** TCMalloc mismatched-sized-delete doc: 'For small objects, it's too expensive to validate the size argument at the time of deallocation' (https://github.com/google/tcmalloc/blob/master/docs/mismatched-sized-delete.md). TCMalloc exposes __size_returning_new, 'a prototype API for ... size feedback in operator new' (https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h). libc++ implements C++23 allocator_traits::allocate_at_least (https://github.com/llvm/llvm-project/blob/main/libcxx/include/__memory/allocate_at_least.h).

**Fix.** Treat the size passed to free_sized and sized delete as a validation hint only: checked in tier 1 and above, sample-checked in tier 0. Use it for speed only on large objects (to avoid a second-level page-map access). Implement __size_returning_new and its aligned and hot/cold variants alongside soham_malloc_sized, so libc++ and TCMalloc-aware code benefit without source changes.

### 30. [major] 4.8 'Correct in the edge cases: ... signals, early init, thread exit, dlopen' and 5.7 'Explain my memory report (on demand, on a signal, or at exit)'

**Problem.** Signals, TLS and reentrancy are listed as requirements but have no mechanism.
- The per-thread fallback (no rseq) is not reentrant, so a handler calling malloc mid-pop corrupts it.
- Slow paths take locks, so a handler calling malloc deadlocks.
- Producing the report from a signal handler is not async-signal-safe (formatting, locks), and claiming a signal number conflicts with applications.
- If the TLS model is not initial-exec, TLS access may itself allocate; with initial-exec, dlopen of the allocator can fail when static TLS runs out.
- Thread-exit ordering is unspecified: C++ thread_local destructors free after the allocator's own per-thread teardown.

**Evidence.** glibc manual: replacement allocators 'must use the initial-exec TLS model' (https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html). signal-safety(7) lists the async-signal-safe functions; malloc and stdio formatting are not among them (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man7/signal-safety.7).

**Fix.** Policy: malloc is not async-signal-safe (POSIX), but the allocator guarantees no deadlock and no corruption on reentry. A per-thread reentrancy counter routes nested calls to an emergency lock-free path: a small per-thread bump reserve, with frees deferred into an atomic list drained later. The report is produced on demand by API, by SOHAMALLOC_REPORT=<fd or path> at exit, or by an opt-in signal whose handler only sets a flag; all report code is allocation-free. A 'zombie thread' state routes post-teardown frees to the remote-free path.

### 31. [major] 5.7 'Sampling heap profiler, always available: ... with stack traces'

**Problem.** Unwinding from inside malloc is a recursion and deadlock trap. glibc backtrace() dlopens libgcc_s on first use, and dlopen calls malloc. Full unwinding costs hundreds of nanoseconds with high variance. Frame pointers may be omitted. Trace storage itself needs allocation.

**Evidence.** glibc misc/unwind-link.c: '__libc_dlopen (UNWIND_SONAME)' with LIBGCC_S_SO, used by backtrace() via __libc_unwind_link_get (https://sourceware.org/git/?p=glibc.git;a=blob;f=misc/unwind-link.c ; https://sourceware.org/git/?p=glibc.git;a=blob;f=debug/backtrace.c). glibc manual: dlopen uses malloc internally (https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html). LLAMA Table 1: 'Capture full stack trace 396 ns ± 364 ns' (https://cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf).

**Fix.** Use a bounds-checked frame-pointer walker (stop at the thread stack bounds, depth at most 32 or 64), store traces in an allocator-internal arena, and optionally pre-warm libgcc unwinding at init outside any allocator lock. Document that accurate profiles need -fno-omit-frame-pointer, and fall back to recording the return address only when frame pointers are absent.

### 32. [major] 5.7 'One knob: SOHAMALLOC=... Advanced settings live in SOHAMALLOC_OPTS'

**Problem.** Options read with plain getenv are a privilege-escalation surface. For setuid or setgid programs and LSM-triggered secure execution, an unprivileged user could weaken hardening tiers or point profile output at files. This applies when sohamalloc is statically linked or installed system-wide, e.g. via /etc/ld.so.preload.

**Evidence.** getenv(3)/secure_getenv: 'Secure execution is required if ... the process's effective user ID did not match its real user ID ... secure_getenv() function is intended for use in general-purpose libraries to avoid vulnerabilities' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man3/getenv.3).

**Fix.** Read options with secure_getenv semantics (getauxval(AT_SECURE)). In secure execution, ignore every option except ones that only increase security, and never accept file paths or fds from the environment.

### 33. [major] 6 'What would make sohamalloc genuinely different' (items 2, 3, 4, 6)

**Problem.** Several 'differentiators' already exist:
- mm_cid caches: TCMalloc;
- lifetime and hot/cold placement with hints: TCMalloc's __hot_cold_t new and lifetime-based allocator, LLVM MemProf's context cloning and hot/cold new rewriting;
- hugepage-aware filler with adaptive subrelease: Temeraire;
- sampled guarded allocations: TCMalloc GWP-ASan, mimalloc v3 sample-guarded;
- first-class heaps with bulk destroy: mimalloc.
Overclaiming novelty hides the real risk, which is integrating these pieces coherently.

**Evidence.** TCMalloc RseqVcpuMode::kMM (https://github.com/google/tcmalloc/blob/master/tcmalloc/internal/percpu.cc). The hot_cold variant 'accepts a hot_cold hint ... TCMalloc may use these hints for better data placement' (https://github.com/google/tcmalloc/blob/master/docs/reference.md). LLVM MemProfContextDisambiguation: 'Cloning is performed to expose the cold allocation call contexts' (https://github.com/llvm/llvm-project/blob/main/llvm/lib/Transforms/IPO/MemProfContextDisambiguation.cpp). SimplifyLibCalls 'Enable conversion of operator new calls with a MemProf hot or cold hint' (https://github.com/llvm/llvm-project/blob/main/llvm/lib/Transforms/Utils/SimplifyLibCalls.cpp). mimalloc v3 includes profile/sample-guarded.h (https://github.com/microsoft/mimalloc/blob/dev3/include/mimalloc/internal.h). mimalloc readme: first-class heaps that 'can be destroyed at once' (https://github.com/microsoft/mimalloc/blob/main/readme.md).

**Fix.** Rewrite section 6 as 'what we combine, what we measure, and what is actually new'. Candidate genuinely new contributions:
- cgroup-aware staged release with measured container outcomes;
- a single coherent page-based design with rseq vCPU slots that targets RSS rather than throughput;
- an explainable accounting model.
Cite each precedent and make the hint ABI compatible with TCMalloc's and LLVM's (__hot_cold_t, __size_returning_new).

### 34. [major] 7 'How to measure' and 8 exit criteria ('within about 5% of TCMalloc and mimalloc', 'RSS at or below jemalloc on Redis and RocksDB', 'scale linearly', 'Tier 0 costs ≤ 2%', p99 allocation latency)

**Problem.** The goals can't be falsified.
- 'Within 5%' gives no metric, aggregate (geometric mean?), configuration (TCMalloc per-CPU vs per-thread, hugepage options; mimalloc v3.4.5 default vs secure) or hardware.
- Hardware missing from the plan: arm64 16K/64K kernels, a 256-CPU box, a 2-vCPU container.
- Timing each malloc call perturbs an operation of about 20 cycles.
- Redis defaults to its bundled jemalloc, and active defrag works only with it, so LD_PRELOAD measures nothing; comparisons must specify MALLOC=libc builds and jemalloc with and without activedefrag.
- 'Scale linearly' has no tolerance.
- Missing: long-running and adversarial fragmentation tests, hello-world start-up RSS, kernel metrics (TLB-shootdown IPIs, mmap_lock contention, VMA count over time), and correctness gates (fuzzing, sanitizers on the allocator, model checking).

**Evidence.** Redis README: 'Redis is compiled and linked against libc malloc by default, except for jemalloc being the default on Linux systems'; redis.conf: active defrag 'only works if you compiled Redis to use the copy of Jemalloc we ship' (https://github.com/redis/redis/blob/unstable/README.md ; https://github.com/redis/redis/blob/unstable/redis.conf). mimalloc's latest release is v3.4.5 (2026-08-05) (https://github.com/microsoft/mimalloc/blob/main/readme.md).

**Fix.** Write a measurement spec.
- Exact versions, commits and build flags, with configurations for each competitor.
- Hardware matrix: x86 many-core; arm64 with 4K, 16K and 64K pages; small cgroup.
- Metric formulas: peak RSS, RSS-time integral, realized fragmentation (TCMalloc's definition), throughput geometric mean with 95% bootstrap CI over at least 10 runs, application tail latency (e.g. memtier p99), sampled allocator latency (1 in 1024 calls, rdtscp with overhead subtraction).
- Kernel counters: TLB lines in /proc/interrupts, faults, /proc/self/maps count.
- Per-phase pass/fail thresholds.

### 35. [major] 8 Roadmap ordering (phases 0-5)

**Problem.** The phases are in the wrong order.
- Phase 1 builds per-thread ownership, per-page remote-free lists and abandoned-heap reclamation; Phase 3 replaces the ownership model with mm_cid slots, which rewrites the remote-free protocol, the fork logic and the cache structure.
- Hugeblock geometry and the page map (Phase 2 or not planned at all) constrain Phase 0's span layer.
- Tier-0 hardening lands in Phase 4, so every earlier performance result excludes its cost while the doc claims 'secure by default'.
- The section 7 harness has no phase.
- The 'main research bet' (lifetime prediction) comes last although it reshapes the span layer.
- Phase 0's exit test (runs ls, python, gcc) is far too weak for an allocator.

**Evidence.** Internal to the document: section 8 phase table versus 5.1 (mm_cid as the primary design), 5.3 (lifetime classes in the span layer), 5.6 (tier 0 is default) and section 7 (harness). TCMalloc's counterfactual mode shows lifetime policies can be evaluated before enabling them (https://github.com/google/tcmalloc/blob/master/docs/lifetime-based-allocator.md).

**Fix.** Reorder.
- Phase 0: measurement harness, allocation-trace recorder, runtime page geometry, region table and page map, final page-ownership abstraction (shared by rseq and per-thread modes), tier-0 encoding, and the API conformance and fuzz suite. Exit: glibc malloc tests pass, plus fuzzing for 24 h with no failures.
- Phase 0.5: offline trace simulator to choose size classes, occupancy and lifetime policies.
- Phase 1: single-threaded performance against mimalloc.
- Phase 2: rseq slots plus remote frees, with model-checked protocols.
- Phase 3: OS layer (reservation strategy, THP regions, staged release, cgroup).
- Phase 4: hardening tiers 1-2 and the profiler.
- Phase 5: lifetime prediction for large objects, enabled only after counterfactual wins.
- Drop meshing.

### 36. [minor] 5.6 Tier 0: 'Free-list pointers XOR-encoded with a per-process secret (like glibc's "safe-linking")'

**Problem.** This is factually wrong and weak as a design. glibc's safe-linking XORs with the pointer's own address bits (pos >> 12), not a secret. A single per-process secret is shared by every prefork child and every page, so one leak defeats all of them.

**Evidence.** glibc malloc.c: '#define PROTECT_PTR(pos, ptr) ((__typeof (ptr)) ((((size_t) pos) >> 12) ^ ((size_t) ptr)))' (https://sourceware.org/git/?p=glibc.git;a=blob;f=malloc/malloc.c). mimalloc keeps 'uintptr_t keys[2]; // two random keys to encode the free lists' per page (https://github.com/microsoft/mimalloc/blob/main/include/mimalloc/types.h).

**Fix.** Use per-page random keys stored out-of-line and re-keyed whenever a page is (re)initialised, plus address-dependent mixing (glibc-style) so a leaked encoded value is location-bound. In the fork child, newly initialised pages get fresh keys.

### 37. [minor] 5.7 report: 'RSS 812 MiB = in-use 640 | front-end caches 22 | ... | hugepage coverage 87%'

**Problem.** The allocator cannot produce an RSS breakdown that adds up. It knows committed and used bytes, not residency:
- MADV_FREE pages vanish when the kernel reclaims them;
- khugepaged and the THP shrinker change the backing;
- RSS includes code, stacks and file mappings.
Per-block 'hugepage coverage' needs smaps or pagemap scans, which are expensive and can't run in a hot path.

**Evidence.** proc.rst documents per-mapping LazyFree and AnonHugePages in smaps (https://docs.kernel.org/filesystems/proc.html). transhuge.rst describes khugepaged collapse and underused-THP splitting performed by the kernel independently of the allocator (https://docs.kernel.org/admin-guide/mm/transhuge.html).

**Fix.** Report two views. Allocator view: committed, in-use, per-tier cached, released-but-reserved. Kernel view, read on demand from /proc/self/smaps_rollup: Rss, AnonHugePages, LazyFree. Label the difference 'outside sohamalloc / not attributable' instead of inventing a sum. Compute hugepage coverage only on demand.

### 38. [minor] 5.6 tier table target costs (≤1-2%, about 10%) and tier 1 'randomised free-list order'

**Problem.** The tier costs are asserted, not measured. 'About 10%' is borrowed from mimalloc's secure mode, which bundles a different feature set (guard pages, randomised allocation, encrypted free lists). Tier 1 adds quarantine and zero-on-free on top, so 10% is optimistic and unproven. Randomised free-list order cancels the locality that free-list sharding is chosen for (5.1, L5). The tier-0 claim is never measured against the competitors' default configurations.

**Evidence.** mimalloc readme: secure mode adds 'guard pages, randomized allocation, encrypted free lists, etc.' and 'The performance penalty is usually around 10% on average over our benchmarks' (https://github.com/microsoft/mimalloc/blob/main/readme.md).

**Fix.** Treat every tier cost as a hypothesis with a per-feature ablation: encoding, checks, sampling, guards, quarantine, randomisation and zero-on-free measured separately on the benchmark suite. Keep a 'tier -1' build (no hardening) purely as the measurement baseline. Randomise within a small window (e.g. shuffle 8-object chunks) to keep line locality.

### 39. [minor] 5.5 NUMA/NUCA row: 'Transfer caches per last-level-cache domain. Node from rseq's node_id. Optional mbind per node arena.'

**Problem.** (1) Topology discovery is unspecified. It needs sysfs reads of cache sharing per CPU (many opens at init on 256 CPUs; cost UNVERIFIED) in an environment where /sys may be masked.
(2) With cid-indexed slots, a slot is not bound to a CPU or LLC, so slot-level caches are not NUCA-local.
(3) mbind on sub-ranges of a shared reservation sets a per-VMA policy and splits VMAs.
(4) Per-node arenas multiply cached memory with no stated bound.

**Evidence.** uapi rseq.h provides cpu_id and node_id separately from mm_cid (https://github.com/torvalds/linux/blob/master/include/uapi/linux/rseq.h). The VMA splitting behaviour of policy changes follows the same per-VMA attribute model as madvise_update_vma (https://github.com/torvalds/linux/blob/master/mm/madvise.c); the mbind-specific path was not separately checked (UNVERIFIED).

**Fix.** Discover topology lazily at the first slow path using raw syscalls, falling back to 'one domain'. Key page pools by an LLC id looked up from rseq cpu_id at refill time. Give each NUMA node its own reservation (its own VMA) rather than calling mbind on ranges. Bound per-domain cached memory with the global cache budget.

### 40. [minor] 5.1 'Anything above about 256 KiB goes straight to the page heap' and 5.4 page heap

**Problem.** The page heap's concurrency is unspecified (Temeraire's page heap uses a global lock). Workloads dominated by 256 KiB to 2 MiB allocations (I/O buffers, serialization arenas) would hit the page heap on every malloc/free with no front-end caching, which costs both contention and latency.

**Evidence.** TCMalloc huge_page_filler.h is used under the page heap lock (pageheap_lock annotations throughout https://github.com/google/tcmalloc/blob/master/tcmalloc/huge_page_filler.h; local copy /home/user/sohamalloc/hpf.h). Exact contention numbers: UNVERIFIED.

**Fix.** Shard the page heap per NUMA node or LLC domain, each with its own lock. Add a small per-slot cache of recently freed large runs (e.g. up to 4 runs of 2 MiB or less, under the global budget). Expose page-heap lock wait time in stats.

### 41. [minor] 8 Phase 5 'optional meshing for very long-running servers' and 9 'Meshing requires shared file-backed (memfd) mappings'

**Problem.** Meshing conflicts with nearly every other choice in the design.
- It needs MAP_SHARED memfd memory, on which MADV_DONTNEED does not free pages (Mesh uses fallocate punch-hole), so the release policy and the known-zero calloc logic are both invalid there.
- Fork has to copy the entire allocated heap into a new memfd in the child, defeating copy-on-write for prefork servers.
- It works on base pages, so it fights the hugepage-first page heap.

**Evidence.** madvise(2): DONTNEED on shared mappings repopulates 'from the up-to-date contents of the underlying mapped file' and 'might not lead to immediate freeing' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/madvise.2). Mesh meshable_arena.cc afterForkChild opens a new span file and copies every allocated page ('for (auto const &i : bitmap) { int result = internal::copyFile(newFd, oldFd, ...)') then remaps; release uses 'fallocate(_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, ...)' (https://github.com/plasma-umass/Mesh/blob/master/src/meshable_arena.cc ; https://github.com/plasma-umass/Mesh/blob/master/src/meshable_arena.h).

**Fix.** Remove meshing from the roadmap. If it is ever revisited, confine it to a separate opt-in arena for long-lived small objects with no THP, punch-hole release, documented fork cost, and its own zeroing rules.

### 42. [minor] 4.2 'Bounded memory overhead with no tuning' and 5.1 'total front-end budget is a fixed fraction of the heap, with a floor and a ceiling'

**Problem.** No bound is actually given. Nothing caps the memory held in:
- per-slot current pages times classes times lifetimes;
- sender-side remote-free buffers;
- uncollected thread-free lists of idle slots;
- page-heap caches.
A 'fraction of the heap' budget is circular, because the caches are part of the heap. There is no blowup bound like Hoard's, even though competitors claim one.

**Evidence.** mimalloc readme claims: 'bounded: it does not suffer from blowup ... bounded space overhead (~0.2% meta-data, with low internal fragmentation)' (https://github.com/microsoft/mimalloc/blob/main/readme.md). The design text gives no numeric bound.

**Fix.** State explicit bounds:
- front-end cached free bytes at most min(1% of live bytes + 256 KiB per active slot, 64 MiB);
- sender buffers at most 32 KiB per slot;
- current pages at most k per class per slot;
- idle-slot pages reclaimed after t seconds.
Prove a blowup bound against the live heap, in the style of Hoard, in the design doc, and assert the bounds in debug builds.

### 43. [minor] 5.5 'commit by touching' plus 'Each first touch costs a page fault plus zeroing' (3/L8)

**Problem.** Committing by first touch moves page-fault and zeroing cost into random application accesses. With MADV_HUGEPAGE that means zeroing 2 MiB, possibly with direct compaction under the default defrag, which hurts p99. With overcommit it also turns out-of-memory into SIGSEGV/SIGBUS or OOM kills instead of an error return. The design never uses prefaulting.

**Evidence.** madvise(2) MADV_POPULATE_WRITE (since 5.14): 'Populate (prefault) page tables writable ... If populating fails, a SIGBUS signal is not generated; instead, an error is returned' (https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/madvise.2). THP defrag default behaviour (https://docs.kernel.org/admin-guide/mm/transhuge.html).

**Fix.** In the latency profile and the background thread, prefault blocks about to be handed out with MADV_POPULATE_WRITE (falling back to touching). On error, mark the block uncommitted and return NULL for that request instead of crashing later. Account prefault time in stats.

### 44. [minor] 5.1 'Shares the rseq registration with glibc through __rseq_offset' / fallback when the allocator must register rseq itself

**Problem.** Self-registration (musl, glibc registration disabled, static builds on old glibc) is not designed. The rseq area must live in static TLS with the required size and alignment. The design must decide when to register (the first malloc in each thread) and how to handle libcs that reuse thread areas without clearing them. A missed registration means the per-thread fallback is silently used.

**Evidence.** kernel/rseq.c rseq_register: 'If the rseq_cs pointer is non-NULL on registration, clear it ... this would break older libcs that reuse the rseq area for new threads without clearing the fields'; rseq_length_valid requires the original 32-byte size and alignment, or a length of at least offsetof(struct rseq, end) (https://github.com/torvalds/linux/blob/master/kernel/rseq.c). uapi: 'The allocation size needs to be greater or equal to max(getauxval(AT_RSEQ_FEATURE_SIZE), 32)' (https://github.com/torvalds/linux/blob/master/include/uapi/linux/rseq.h).

**Fix.** Specify the self-registration path:
- a static-TLS area sized max(AT_RSEQ_FEATURE_SIZE, 32) and aligned accordingly;
- register on the first allocator call in a thread when __rseq_size==0;
- clear rseq_cs after each critical section;
- unregister in a late TLS destructor;
- expose the active mode (rseq-cid, rseq-cpu, per-thread) in soham_stats().
Unverified opportunity: the rseq time-slice extension (RSEQ_FLAG_SLICE_EXT_DEFAULT_ON, rseq_slice_ctrl in the current uapi header; first kernel release UNVERIFIED) could protect short slow-path lock holds from preemption.

## Completeness review (what v1 left out)

I read /home/user/sohamalloc/docs/RESEARCH_AND_DESIGN.md (v1, 510 lines) in full and checked it against primary sources: kernel docs, man pages, glibc NEWS and manual source, TCMalloc, mimalloc, jemalloc and snmalloc docs and source, the C++ draft, PEP 703, Clang/GCC docs and the ISMM 2024–2026 programs.

v1 has a reasonable high-level layering. What it lacks is the mechanisms that make an allocator real, plus most of what changed in 2024–2026.

**Main gaps:**
1. **No pointer-to-metadata (pagemap) design.** Every free, realloc, sized free, double-free check, guarded memcpy and heap walk depends on this lookup, and v1 never describes it.
2. **Size classes have no design method.** The "≤12.5% waste" claim is wrong: 4 classes per doubling gives up to about 19–25% waste.
3. **The rseq/mm_cid core idea rests on unstable ground.**
   - It is prior art: it came from Google's TCMalloc vCPU work in 2022.
   - glibc only exposes mm_cid through `__rseq_size` from version 2.41.
   - Linux 6.19 changed rseq behaviour and broke TCMalloc.
   - The mm_cid management was rewritten in 2025.
4. **The OS layer contradicts itself under strict overcommit, RLIMIT_AS and VMA budgets.** With `vm.overcommit_memory=2`, `MAP_NORESERVE` is ignored and the writable reservation is charged in full. Switching to `PROT_NONE` plus `mprotect` brings back commit charges and splits VMAs.
5. **The lifetime predictor ignores TCMalloc's own lifetime-based allocator and its counterfactual (shadow) mode.** A copy of that doc is already in the repo.
6. **Compiler-provided allocation information is ignored.** Examples: Clang 22 AllocToken, MemProf hot/cold `new`, `__size_returning_new`, C++ `allocate_at_least`, glibc 2.43 `free_sized`.

**Topics never covered:** C++ semantics (new_handler loop, GCC's default assumption that operator new/delete have no side effects); CPython free-threaded and Rust GlobalAlloc requirements; testing (fuzzing, differential tests, GenMC, sanitizer/Valgrind modes, kernel and glibc version matrix); sanitizer and profiler compatibility; startup cost and idle footprint; page-size portability (16K/64K arm64, Android); the THP shrinker; new kernel APIs (process_madvise, mseal, MAP_DROPPABLE, PR_THP_DISABLE_EXCEPT_ADVISED); hazards of reading pressure signals from inside malloc; failure/OOM semantics; current security work (type partitioning, MTE/EMTE/MIE, guarded memcpy, signal-handler conflicts); TLS/fork/signal/exit specifics; real-time mode; Windows/macOS/musl/32-bit/wasm; formal fragmentation accounting; telemetry and debug modes; deterministic mode; 10k+ threads; heterogeneous cores; CXL/tiering/NUMA; BatchIt; an outdated comparison set (jemalloc 5.4.0 shipped Sep 2026, mimalloc v3.4.5 Aug 2026); arena API research; licensing and provenance (the repo already contains Apache-2.0 TCMalloc files hpf.h and hps.h and has no LICENSE); swap/zswap interplay.

**Limits:** the 200-call web-search budget ran out partway through. After that I only fetched known primary URLs directly (WebFetch or curl; no search engines were used). Claims I could not confirm are marked UNVERIFIED. Scratch downloads are in a local scratch directory (dl/). I did not modify any files in /home/user/sohamalloc.

### 1. [critical] §5.1 Front end / §5.4 'Metadata always out-of-line' / free() path (never specified)

**Problem.** There is no pointer-to-metadata lookup (pagemap) design. free(p), realloc, malloc_usable_size, sized-free validation, the 'per-span free bitmap' double-free check, an object-start lookup (needed for guarded memcpy, MTE retagging and heap walking) and 'is this pointer ours?' all depend on an O(1) pointer→span/class map. v1 never says what structure it is, what granularity it uses, how much memory it costs, whether it stays cache resident, or what happens with foreign pointers (memory allocated before the preload, by ld.so's early allocator, or by another allocator when sohamalloc is dlopen'd) and with interior pointers.

**Evidence.** TCMalloc keeps a 2- or 3-level radix-tree pagemap to map every address to a span and size class. Sized delete 'substantially reduced the number of times that we need to consult the pagemap' (https://raw.githubusercontent.com/google/tcmalloc/master/docs/design.md, https://raw.githubusercontent.com/google/tcmalloc/master/docs/tuning.md). TCMalloc also warns against loading it into a running binary, because objects from the system malloc may be passed to it and 'TCMalloc will not be able to handle such objects' (design.md Caveats). snmalloc's guarded memcpy relies on naturally aligned power-of-two slabs so that remaining_bytes(p) is a mask plus a modulus (https://raw.githubusercontent.com/microsoft/snmalloc/main/docs/security/GuardedMemcpy.md). Arithmetic: a flat map over a 47-bit user address space at 64 KiB granularity with 8-byte entries is 2^31 entries, which is 16 GiB of virtual space. It must therefore be per-reservation or lazily committed. The kernel does not hand out addresses above 47 bits unless the program passes a hint (https://docs.kernel.org/arch/x86/x86_64/5level-paging.html).

**Fix.** Add a 'Pagemap' section with:
- a per-reservation flat array indexed by (p − base) >> slab_shift, committed lazily, holding one compact entry (class id, slab kind, owner) per slab;
- object start computed with a reciprocal multiply;
- a range check that decides ownership, plus an explicit foreign-pointer policy (forward to the RTLD_NEXT free, ignore, or abort in secure mode);
- defined behaviour for interior pointers;
- an idle-memory budget for the map, and a benchmark of lookup cost against the sized-free fast path.

### 2. [critical] §5.1 'about 4 per power of two, so internal waste is at most about 12.5%'

**Problem.** The bound is wrong and there is no method behind the size-class design. With 4 classes per doubling (64, 80, 96, 112, 128), a 65-byte request gets 80 bytes: 15/80 = 18.75% of the block, or 23% of the request. The worst case approaches 20% of the block and 25% of the request. A 12.5% bound needs 8 classes per doubling. v1 also ignores:
- slab tail waste;
- alignment rules (max_align_t, operator new alignment);
- how the class count multiplies partially-filled spans, which gets worse with lifetime classes;
- per-page-size class tables;
- the requirement that sized delete accepts any size between the requested size and the usable size.

**Evidence.** TCMalloc uses '60-80 allocatable size-classes' designed to minimise rounding waste. It uses 8-byte-aligned classes for operator new when __STDCPP_DEFAULT_NEW_ALIGNMENT__ <= 8, and 'Size-classes are determined on a per-page-size basis' (https://raw.githubusercontent.com/google/tcmalloc/master/docs/design.md, https://raw.githubusercontent.com/google/tcmalloc/master/docs/tuning.md). Its size-returning new allows sized delete with any size 'greater or equal to the original requested size, and less than or equal to the capacity' (https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/malloc_extension.h). The waste figures above are my own arithmetic.

**Fix.** Define an objective: rounding waste + slab-tail waste + an expected cost per extra partially-filled span. Generate class tables offline from real allocation-size histograms (traces of Redis, clang, CPython, RocksDB and others), with one table per page size (4K/16K/64K). Publish exact waste bounds. Add property tests that the size→class mapping is monotone and that every size in [requested, usable] maps back to the same class. Treat 'classes × lifetime classes × NUMA nodes' as one span-count budget.

### 3. [critical] §5.1 'Cache slot selection, the new part' (mm_cid), §6 item 2, §9 rseq risk

**Problem.** The headline idea is neither new nor stable.
(a) Prior art: mm_cid came from Google's TCMalloc virtual-CPU work. Google found a 'flat' vCPU model better than per-NUMA vCPUs.
(b) glibc exposes mm_cid through __rseq_size only from 2.41. 2.40 redefined __rseq_size as the active area size (20 bytes), so the documented feature check fails on older glibc.
(c) Linux 6.19's rseq rewrite stopped rewriting cpu_id_start except on migration. That broke TCMalloc. The proposed fix raises the registered rseq size to 33 bytes with 64-byte alignment. The time-slice extension (merged for 7.0) is unavailable to old-size registrations.
(d) The 2025 MM CID rewrite keeps per-task CIDs 'as long as the number of threads is not exceeding the number of possible CPUs'. The bound may now be possible CPUs rather than allowed CPUs, which would undercut v1's claim that a 4-CPU container gets 4 caches.
(e) Other rseq users (glibc, tracing libraries) must keep working.

**Evidence.** Prior art and flat-vCPU result: https://lkml.iu.edu/2208.0/01988.html. glibc NEWS for 2.40 and 2.41: https://raw.githubusercontent.com/bminor/glibc/master/NEWS. LWN, 'Restartable sequences, TCMalloc, and Hyrum's Law', 30 Apr 2026: https://lwn.net/Articles/1070072/. MM CID rewrite cover letter, 29 Oct 2025: https://lkml.iu.edu/2510.3/08526.html and https://lwn.net/Articles/1044027/; the release that merged it is UNVERIFIED. The glibc tunable glibc.pthread.rseq=0 lets an application register rseq itself (https://raw.githubusercontent.com/bminor/glibc/master/manual/tunables.texi). Which distributions ship glibc older than 2.41 (for example Ubuntu 24.04 with 2.39) is UNVERIFIED.

**Fix.** Rewrite §5.1 as 'per-CPU-ish caches on documented rseq ABI only':
- detect features with getauxval(AT_RSEQ_FEATURE_SIZE) and __rseq_size, and never overlay or write kernel-owned fields;
- size the cache count from the observed maximum mm_cid, not from assumptions;
- fall back to cpu_id, then to per-thread caches;
- run a CI matrix over kernels 6.1, 6.6, 6.12, 6.18, 6.19 and 7.x and glibc 2.35–2.44;
- drop the novelty claim and credit TCMalloc's vCPU work.

### 4. [critical] §5.5 OS layer: 'Reserve … 64 GiB … commit by touching … fewer than 10 VMAs'; §5.4 'realloc of large blocks uses mremap'

**Problem.** These goals contradict each other in common environments.
- Under vm.overcommit_memory=2, a private writable reservation is charged in full and MAP_NORESERVE is ignored, so the allocator can fail at start-up on strict-overcommit hosts.
- The workaround (reserve PROT_NONE, mprotect to commit) is charged too, and every mprotect splits VMAs, which breaks the '<10 VMAs' target.
- RLIMIT_AS (ulimit -v) environments fail long before RSS is high.
- mremap-based realloc inside a single reservation moves pages into separate VMAs (my reasoning, not measured).
- Windows has a separate reserve/commit model with system-wide commit charge.

**Evidence.** Mode 2: 'MAP_NORESERVE flag is ignored'; anonymous PRIVATE WRITABLE mappings are charged their full size; 'We account mprotect changes in commit' (https://docs.kernel.org/mm/overcommit-accounting.html). TCMalloc: 'trying to limit an application's memory use by restricting VSS will fail long before the application has used that much physical memory' (https://raw.githubusercontent.com/google/tcmalloc/master/docs/design.md). Windows: 'pre-committing an entire allocation can cause the system to run out of committable pages' (https://learn.microsoft.com/en-us/windows/win32/memory/reserving-and-committing-memory). The claim that mremap splits VMAs is UNVERIFIED and needs a measurement.

**Fix.** Add a commit-strategy module:
- read /proc/sys/vm/overcommit_memory and RLIMIT_AS at init;
- in strict mode, grow the reservation in bounded chunks and commit at coarse granularity, with a hard VMA budget tracked at runtime;
- use mremap for large realloc only while within that budget;
- test under overcommit_memory=2, `ulimit -v`, and vm.max_map_count set low.

### 5. [major] §5.3 Lifetime-aware placement ('LLAMA's idea without the neural network and without offline profiling')

**Problem.** v1 misses close prior art and has no way to evaluate the idea safely.
- TCMalloc already shipped an experimental lifetime-based allocator. It predicts short or long lifetimes from allocation stack traces, places short-lived objects in a 'lifetime region', and has a counterfactual mode that measures prediction accuracy without changing placement. Current TCMalloc also tracks hugepage lifetime histograms.
- One return address is a weak key: allocations through std::vector, std::string or an xmalloc wrapper share one immediate caller (my reasoning).
- Compiler-side help exists: LLVM MemProf can rewrite operator new calls to hot/cold variants.

**Evidence.** TCMalloc lifetime doc, copy at /home/user/sohamalloc/tcm_lifetime.md. The upstream path docs/lifetime.md now returns 404, so where this copy came from is UNVERIFIED. Lifetime histograms are in upstream huge_page_filler.h (https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/huge_page_filler.h, identical to /home/user/sohamalloc/hpf.h). MemProf hot/cold rewrite: 'optimize-hot-cold-new' in https://raw.githubusercontent.com/llvm/llvm-project/main/llvm/lib/Transforms/Utils/SimplifyLibCalls.cpp. Frame pointers became default in Fedora 38 (https://fedoraproject.org/wiki/Changes/fno-omit-frame-pointer) and Ubuntu 24.04 (https://ubuntu.com/blog/ubuntu-performance-engineering-with-frame-pointers-by-default), which makes hashing several frames cheaper.

**Fix.** Credit TCMalloc and say what sohamalloc adds. Make a counterfactual (shadow) mode the gate for the Phase 5 feature. Key predictions on a hash of N frames, or on compiler tokens (MemProf, AllocToken), rather than __builtin_return_address(0). Build a trace-driven simulator so the predictor can be evaluated on recorded traces before it is implemented in the allocator.

### 6. [major] §3 L6 'The C API is information-poor … allocator is forced to guess'; §5.7 Extended API

**Problem.** The information landscape is out of date. Compilers and standards now pass allocation information that v1 neither consumes nor supports:
- Clang 22 allocation tokens (__alloc_token_malloc etc.), with a type hash that splits pointer-containing from pointer-free types;
- MemProf hot/cold operator new;
- TCMalloc's __size_returning_new and __hot_cold_t overloads;
- C++ std::allocator::allocate_at_least;
- C23 free_sized and free_aligned_sized, now in glibc 2.43;
- sized deallocation on by default in Clang 19+ and GCC (C++14 and later).
v1's statement that size feedback is 'still only a proposal (P0901)' is outdated.

**Evidence.** Clang AllocToken docs: https://clang.llvm.org/docs/AllocToken.html. Clang 22 release notes introduce -fsanitize=alloc-token (https://raw.githubusercontent.com/llvm/llvm-project/release/22.x/clang/docs/ReleaseNotes.rst). TCMalloc size-returning and hot/cold API: https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/malloc_extension.h. allocate_at_least in the current C++ draft: https://eel.is/c++draft/allocator.members; that it entered C++23 via P0401 is UNVERIFIED in this session. glibc 2.43 free_sized: https://raw.githubusercontent.com/bminor/glibc/master/NEWS. Clang 19 'Sized deallocation is enabled by default in C++14 onwards': https://raw.githubusercontent.com/llvm/llvm-project/release/19.x/clang/docs/ReleaseNotes.rst. GCC: 'Enabled by default under -std=c++14 and above' (https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Dialect-Options.html).

**Fix.** Define an ABI-compatibility section:
- implement __alloc_token_* in both the argument and fast ABIs;
- implement hot/cold and size-returning new, plus nallocx and sdallocx;
- specify free_sized semantics;
- use tokens for cheap type or pointer-ness partitioning in security tier 0/1, and hot/cold hints for placement;
- treat experimental LLVM modes as unstable.

### 7. [major] §5.7 'every C++ new/delete overload' (C++ contract not specified)

**Problem.** Required C++ semantics are missing.
- operator new must loop calling the current new_handler, or throw bad_alloc.
- An implementation may turn sized delete into unsized delete, so the allocator cannot depend on getting the size.
- new/delete calls from new-expressions may be elided.
- GCC by default assumes replaceable new/delete do not read or modify global state. Stats, hooks or profiler callbacks that the program can observe through new/delete may therefore be reordered or removed.
- Nothing is said about std::pmr, -fnew-alignment / 8-byte classes, or the nothrow and align_val_t contracts.

**Evidence.** [new.delete.single]: 'if the current new_handler … is a null pointer value, throws bad_alloc … Otherwise, the function calls the current new_handler … If the called function returns, the loop repeats'. Also: 'A call to an operator delete with a size parameter may be changed to a call to the corresponding operator delete without a size parameter' (https://eel.is/c++draft/new.delete.single). GCC -fassume-sane-operators-new-delete: 'This behavior is enabled by default … might not be satisfied if a program replaces those operators' (https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Dialect-Options.html). jemalloc 5.4.0 had to fix its new(std::nothrow) contract (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog).

**Fix.** Write a C++ conformance chapter and test suite: new_handler loop, bad_alloc versus nothrow, alignment, and correct behaviour when sized delete arrives as unsized. Keep new/delete free of side effects the program can observe, and document the hook and stat APIs as best-effort under -fassume-sane-operators-new-delete. Provide a std::pmr memory_resource adaptor for sohamalloc heaps.

### 8. [major] §7 real programs ('Lua or CPython test suites'); no section on language runtimes

**Problem.** Runtimes that place extra demands on the allocator are not considered.
- CPython's free-threaded build depends on mimalloc-specific features: separate heaps for GC and non-GC objects, walking allocator structures to find every object, and delayed page reuse so lock-free reads stay safe (QSBR). PEP 703 says wholly replacing the allocator with jemalloc or tcmalloc 'will not work correctly'.
- Rust's GlobalAlloc always passes the layout on dealloc, so every Rust free is a sized free. Allocators must not unwind and have a specific realloc contract.
Without these APIs sohamalloc cannot serve as CPython's allocator in free-threaded builds, and it may underperform for Rust.

**Evidence.** PEP 703: https://peps.python.org/pep-0703/. The free-threading HOWTO says GC objects get their own heaps, 'currently four' (https://docs.python.org/3.14/howto/free-threading-python.html; the exact count comes from a search summary). mimalloc v3 advertises 'more efficient heap-walking (for the CPython GC for example)' (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). Rust GlobalAlloc: 'layout is the same layout that was used to allocate'; 'It's undefined behavior if global allocators unwind' (https://doc.rust-lang.org/std/alloc/trait.GlobalAlloc.html). snmalloc ships a Rust crate (https://raw.githubusercontent.com/microsoft/snmalloc/main/README.md).

**Fix.** Add first-class heaps, a heap-visit API and reuse-delay hooks so sohamalloc can be offered as CPython's allocator. Ship an official Rust crate whose dealloc and realloc use the sized fast path. Add CPython (free-threaded) and Rust std test suites to CI. Note the JVM, Go (cgo only) and Ruby as UNVERIFIED follow-ups.

### 9. [major] §8 Phase 0 'test suite (randomised stress …)'; no testing chapter

**Problem.** There is no testing strategy for code that every process will depend on. Missing pieces:
- fuzzing of API call sequences;
- differential testing against glibc semantics (errno preservation, malloc(0), realloc(p,0), alignment validation, overflow checks, free_sized(NULL));
- model checking of the lock-free remote-free and transfer paths;
- weak-memory testing on arm64;
- TSan and ASan builds of the allocator itself, in a mode that does not override malloc;
- a kernel × glibc version matrix;
- fault injection (ENOMEM, EPERM from seccomp or mseal);
- long soak and fragmentation-regression tests;
- independent and LLM audits.

**Evidence.** The glibc manual says a replacement free 'should also preserve errno' (https://raw.githubusercontent.com/bminor/glibc/master/manual/memory.texi). jemalloc 5.4.0 (Sep 2026) still had to fix errno preservation across free/free_sized and accept NULL in free_sized (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog). C23 made realloc with size 0 undefined (N2464, https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2464.pdf). mimalloc 2026 release notes credit LLM audits and DeepTest with finding bugs, and fixed a 'glibc 2.44 crash' (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). The 6.19 rseq regression shows why a kernel matrix is needed (https://lwn.net/Articles/1070072/). GenMC model-checks C/C++/Rust programs at LLVM IR level (https://raw.githubusercontent.com/MPI-SWS/genmc/master/README.md). mimalloc notes that ASan 'redirects the standard allocation functions' and requires MI_OVERRIDE=OFF on some platforms (readme).

**Fix.** Add a Testing and Verification chapter with exit criteria per phase:
- libFuzzer/AFL harnesses over API call sequences;
- a differential oracle against glibc;
- GenMC or CDSChecker models of every lock-free structure;
- a no-override build for TSan/ASan/MSan;
- QEMU or real arm64 CI;
- a matrix of kernels (6.1 LTS to 7.x) and glibc/musl versions;
- an fd-, seccomp- and ENOMEM-injection harness;
- 72-hour soak tests with RSS-drift alarms.

### 10. [major] §5.7 Drop-in / Extended API; §4.7 'Covers the full libc and C++ allocation surface'

**Problem.** Interaction with sanitizers, Valgrind and heap profilers is not designed. Valgrind intercepts every exported allocation function by default, so under Valgrind sohamalloc is replaced, while the soham_* extended API is not intercepted. Memory allocated through soham_malloc_sized() and then passed to free() would then be misreported. ASan, HWASan, TSan and MSan supply their own malloc. Heap profilers such as heaptrack and bytehound wrap malloc via LD_PRELOAD and need allocator-specific support. Preload ordering is unspecified.

**Evidence.** Valgrind: allocation functions 'are intercepted by default in any shared library or in the executable if they are exported as global symbols'; somalloc synonym (https://valgrind.org/docs/manual/manual-core.html). mimalloc provides MI_TRACK_VALGRIND, MI_TRACK_ASAN and ETW and documents --soname-synonyms=somalloc=*mimalloc* (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). bytehound supports jemalloc only through the jemallocator crate (https://raw.githubusercontent.com/koute/bytehound/master/README.md). heaptrack injects via LD_PRELOAD or GDB (https://raw.githubusercontent.com/KDE/heaptrack/master/README.md).

**Fix.** Add build and run modes:
- Valgrind client requests (MALLOCLIKE/FREELIKE) for all extended entry points;
- ASan poisoning of cached free memory;
- a no-override mode;
- documented LD_PRELOAD ordering with heaptrack, bytehound and gperftools;
- USDT probes on slow paths and sampled events;
- a guarantee that every extended allocation can be released with plain free() and is visible to standard interceptors.

### 11. [major] §4 Requirements / §8 Phase 0 exit ('runs ls, python, gcc'); no idle-footprint or startup targets

**Problem.** There is no budget for start-up cost, binary size or idle memory, even though most processes are tiny and short-lived (coreutils, compiler drivers, build steps run thousands of times). v1's init path reserves 64 GiB, detects the THP mode, reads cgroup and PSI files and may start a thread, all before main(). Per-CPU slabs cost memory on many-core machines. v1's own '1.5 MB per CPU' note is a fleet-level number, not an idle one.

**Evidence.** TCMalloc: 'In per-CPU mode, TCMalloc will reserve a slab of memory per-CPU (typically 256 KiB), which, on systems with large numbers of logical CPUs, can lead to a multi-mebibyte footprint'; and 'TCMalloc requests memory from the OS in large chunks (typically 1 GiB regions)' (https://raw.githubusercontent.com/google/tcmalloc/master/docs/design.md).

**Fix.** Measure glibc and mimalloc v3 first, then set numeric budgets for: syscalls before main, RSS of /bin/true, time for 10k fork+exec runs, .so size and idle metadata. Make init staged: reserve and read sysfs or cgroup files only on the first slow path, create no thread until pressure is seen, and size per-CPU caches lazily.

### 12. [major] §3 L3, §4.4, §5.4 '2 MiB hugeblocks', §5.1 'exact for 8 KiB and below'

**Problem.** Page size and hugepage size are hard-coded at 4 KiB and 2 MiB. On arm64 with 16K pages the PMD hugepage is 32 MiB, and with 64K pages it is 512 MiB. A 2 MiB hugeblock then has no TLB meaning: it either wastes huge amounts of memory or never gets backed by a hugepage. Android now requires 16 KB page support. mTHP sizes and contiguous-PTE sizes (64K on 4K pages) are ignored as allocation units.

**Evidence.** arm64 hugepage sizes: 4K granule: CONT PTE 64K, PMD 2M; 16K: CONT PTE 2M, PMD 32M; 64K: CONT PTE 2M, PMD 512M (https://docs.kernel.org/arch/arm64/hugetlbpage.html). Google Play: from 1 Nov 2025, apps targeting Android 15+ 'must support 16 KB page sizes on 64-bit devices' (https://developer.android.com/16kb-page-size). Per-size mTHP controls live under hugepages-<size>kB (https://docs.kernel.org/admin-guide/mm/transhuge.html). glibc 2.43 enables 2 MB THP by default in malloc on AArch64 (https://raw.githubusercontent.com/bminor/glibc/master/NEWS). That Apple silicon uses 16K pages is UNVERIFIED (Apple's page did not render).

**Fix.** Detect page, PMD and mTHP sizes at runtime. Derive slab size, pagemap granularity, size-class tables and hugeblock size from them, for example hugeblock = PMD size only when PMD ≤ a policy cap, otherwise the largest enabled mTHP or contiguous-PTE size. Run CI on 16K and 64K kernels.

### 13. [major] §3 L3 ('Most allocators are still hugepage-oblivious'); §5.5 Hugepages row

**Problem.** The hugepage strategy ignores kernel behaviour and peer allocators from 2024–2026.
- Since Linux 6.12 the kernel splits 'underused' THPs (zero-filled subpages above max_ptes_none) under memory pressure. Partly-used hugeblocks, and pages the allocator keeps zero for calloc, are exactly what it splits, so densest-first packing can be undone without the allocator noticing.
- MADV_COLLAPSE ignores the 'never' setting, which raises the question of whether the allocator should override an administrator's choice.
- The 6.18 prctl PR_THP_DISABLE_EXCEPT_ADVISED is not honoured.
- The 'oblivious' claim is outdated: jemalloc has HPA, mimalloc v3 sets a 2 MiB minimum purge size when THP is present, and glibc 2.43 enables THP on AArch64.

**Evidence.** 'A THP is underused if the number of zero-filled pages in the THP is above max_ptes_none'; shrink_underused knob; MADV_COLLAPSE ignores 'never'; PR_THP_DISABLE_EXCEPT_ADVISED (https://docs.kernel.org/admin-guide/mm/transhuge.html). The 6.12 changelog lists 'Split underused THPs' (https://kernelnewbies.org/Linux_6.12) and the 6.18 changelog lists the prctl extension (https://kernelnewbies.org/Linux_6.18). mimalloc 2026-01: 'When THP is detected on v3, mimalloc will set the MIMALLOC_MINIMAL_PURGE_SIZE to 2MiB' (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). jemalloc HPA and arenas.hugepage: https://raw.githubusercontent.com/jemalloc/jemalloc/dev/doc/jemalloc.xml.in. ISMM 2025 'EMD: Fair and Efficient Dynamic Memory De-bloating of Transparent Huge Pages' (https://conf.researchr.org/track/ismm-2025/ismm-2025-papers); its content is UNVERIFIED.

**Fix.** Add a hugepage policy module that:
- honours sysfs modes and prctl state, and never collapses under 'never';
- measures real coverage from smaps AnonHugePages instead of assuming it;
- models shrink_underused, so it does not prefault zero tails and releases or collapses with the kernel shrinker in mind;
- is benchmarked against jemalloc HPA and mimalloc v3 THP mode.

### 14. [major] §3 L8 and §5.5 OS-layer table

**Problem.** Kernel facilities from 2023–2026 that bear directly on 'kernel friendliness' are missing.
- process_madvise: from 6.13 any advice is allowed on the calling process, so one syscall can cover many ranges.
- MAP_DROPPABLE (6.11).
- mseal (6.10; glibc 2.43 wrapper): useful for sealing the allocator's own read-only configuration and secret pages, but it blocks DONTNEED/FREE on sealed read-only anonymous memory and must never be applied to heap memory.
- Per-VMA locks (6.4) change v1's mmap_lock-contention argument.
- Unprivileged PSI (6.4), weighted interleave (6.9), PREEMPT_RT (6.12), and SLUB sheaves (6.18) as a kernel analogue of per-CPU array caches.

**Evidence.** process_madvise: 'Since Linux 6.13, when manipulating the calling process, any advice flag is permitted' (https://man7.org/linux/man-pages/man2/process_madvise.2.html). jemalloc 5.3.1 added process_madvise_max_batch (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog). mseal blocks MADV_DONTNEED/MADV_FREE and says 'don't call mseal on ptr return from malloc' (https://docs.kernel.org/userspace-api/mseal.html). Release notes: https://kernelnewbies.org/Linux_6.4 (per-VMA locks, unprivileged PSI, LAM), https://kernelnewbies.org/Linux_6.9, https://kernelnewbies.org/Linux_6.10, https://kernelnewbies.org/Linux_6.11, https://kernelnewbies.org/Linux_6.12, https://kernelnewbies.org/Linux_6.18.

**Fix.** Extend the OS table with a feature-detection column and fallbacks:
- batched purge through process_madvise(pidfd self) where available;
- mseal for allocator configuration and secret pages only;
- re-measure mmap_lock contention on kernels with per-VMA locks before designing around it;
- evaluate MAP_DROPPABLE for a page cache the kernel can reclaim without the allocator's help (research item; free lists must not live in droppable pages).

### 15. [major] §5.5 'When to release' (cgroup memory.high/max/events + /proc/pressure/memory; PSI trigger fd thread)

**Problem.** Reading pressure signals from inside malloc is under-specified and risky.
- glibc's fopen and opendir allocate internally, which risks recursion.
- PSI triggers need one fd per trigger, kept open, plus a polling thread, which contradicts 'no busy background thread'.
- Unprivileged trigger windows must be multiples of 2 s.
- /proc/pressure is system-wide; inside a container the relevant file is the cgroup's memory.pressure.
- fds the allocator holds can leak across exec unless opened with O_CLOEXEC, and can break daemons that close every fd or sandboxes that block open or poll.
- memory.high throttles rather than OOM-killing, and memory.reclaim exists; v1 says nothing about either.

**Evidence.** glibc manual: 'fopen, opendir, dlopen, and pthread_setspecific functions currently use the malloc subsystem internally' (https://raw.githubusercontent.com/bminor/glibc/master/manual/memory.texi). PSI: unprivileged window 'must be a multiple of 2s'; a separate open() per trigger; per-cgroup memory.pressure (https://docs.kernel.org/accounting/psi.html). jemalloc 5.4.0: 'Use O_CLOEXEC when opening the THP sysfs file' (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog). memory.high: 'never invokes the OOM killer'; memory.reclaim (https://docs.kernel.org/admin-guide/cgroup-v2.html).

**Fix.** Specify the mechanism:
- raw open/read syscalls with O_CLOEXEC;
- the cgroup path resolved once from /proc/self/cgroup;
- memory.current, memory.events and memory.pressure sampled on slow paths with rate limits;
- tolerate ENOENT, EPERM and EBADF;
- make the PSI thread opt-in;
- never block or allocate while reading;
- add a test that runs under seccomp with no filesystem access.

### 16. [major] Absent: allocation-failure and OOM semantics (only §5.5 'container-safe')

**Problem.** Failure behaviour is undefined. v1 does not cover:
- errno=ENOMEM;
- NULL versus abort;
- the C++ new_handler path;
- calloc and reallocarray overflow;
- requests above PTRDIFF_MAX;
- malloc(0), realloc(p,0) (undefined in C23) and invalid aligned_alloc alignments.
With overcommit, memory.max causes an OOM kill at page fault rather than a NULL return, so 'container-safe' really needs an optional soft budget that fails allocations gracefully before the kernel kills the process.

**Evidence.** C23 zero-size realloc is undefined (N2464, https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2464.pdf); glibc still frees for realloc(p,0) (per an LWN comment, https://lwn.net/Articles/982956/, secondary source). mimalloc 2026-08 added 'set errno on allocation errors' (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). jemalloc fixed rallocx to set ENOMEM (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog). glibc's PTRDIFF_MAX limit is UNVERIFIED in this session.

**Fix.** Add a semantics table matching glibc wherever glibc defines behaviour, check it with differential tests, and document every deviation. Add SOHAMALLOC_BUDGET, a soft cap derived from memory.max or set explicitly, that returns NULL and invokes the new_handler before the kernel OOM-kills the process.

### 17. [major] §5.6 Security tiers and §6 item 6 ('tier 0 basically free', 'sampled guard allocations')

**Problem.** The tiers miss the 2023–2026 state of the art and contain practical errors.
- No type or partition isolation: Apple xzone malloc, kalloc_type and libpas place allocations by type; PartitionAlloc uses partitions; AllocToken gives a cheap split between pointer-containing and pointer-free types.
- No EMTE/MIE-style always-on synchronous tagging.
- MTE cannot be a tier switched on at runtime: memory must be mapped with PROT_MTE and tag checking is enabled per thread with prctl.
- No guarded memcpy (MSRC: a third of remote-code-execution heap overflows started with a block copy).
- GWP-ASan-style sampling installs SIGSEGV handlers that clash with JVMs and garbage collectors.
- mimalloc already ships sampled guard pages, and separates metadata by default since 2026, which weakens the novelty claim.

**Evidence.** Apple, 'Memory Integrity Enforcement', 9 Sep 2025: kalloc_type, xzone malloc and libpas 'use type information to decide how to organize memory allocations'; EMTE in synchronous mode (https://security.apple.com/blog/memory-integrity-enforcement/). MTE requires PROT_MTE and prctl(PR_SET_TAGGED_ADDR_CTRL); 4-bit tag per 16-byte granule (https://docs.kernel.org/arch/arm64/memory-tagging-extension.html). snmalloc guarded memcpy and MSRC data: https://raw.githubusercontent.com/microsoft/snmalloc/main/docs/security/GuardedMemcpy.md. GWP-ASan InstallSignalHandlers forwards to the previous handler (https://llvm.org/docs/GwpAsan.html). mimalloc MIMALLOC_GUARDED_SAMPLE_RATE and 'by default put page meta info separate from allocated objects (v3,security)' (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). AllocToken pointer split: https://clang.llvm.org/docs/AllocToken.html. Intel/AMD x86 tagging plans are UNVERIFIED; only LAM masking (6.4) is verified (https://kernelnewbies.org/Linux_6.4).

**Fix.** Redesign the tiers:
- tier 0 adds pointer/non-pointer segregation driven by tokens or a type-hash API;
- the MTE profile is chosen at reservation time when HWCAP2_MTE is present;
- add a guarded memcpy/strcpy option and _FORTIFY_SOURCE integration;
- define a signal-handler chaining policy, off by default when another SIGSEGV handler exists;
- benchmark against mimalloc secure mode, Scudo, hardened_malloc and PartitionAlloc before claiming '≤1–2%'.

### 18. [major] §3 L11 and §4.8 (fork, signals, early init, thread exit, dlopen) vs §5 (only pthread_atfork specified); §5.7 'Explain my memory report (on demand, on a signal …)'

**Problem.** The edge cases are listed but no solution is given.
- glibc requires a replacement malloc to use initial-exec TLS; since 2.39 the dynamic linker calls malloc during TLS access, which recurses forever if the allocator uses dynamic TLS.
- Surplus static TLS defaults to 512 bytes, so an allocator dlopen'd late (through JNI or a Python extension) must keep its per-thread state tiny.
- A report produced 'on a signal' must be async-signal-safe: no stdio, no malloc.
- Late frees after thread teardown need handling.
- After fork, the child inherits secrets and loses the background thread.

**Evidence.** glibc manual: replacement malloc 'must use the initial-exec TLS model' (https://raw.githubusercontent.com/bminor/glibc/master/manual/memory.texi). glibc 2.39 NEWS: '…can result in an infinite recursion if a malloc replacement library or its dependencies use dynamic TLS' (https://raw.githubusercontent.com/bminor/glibc/master/NEWS). glibc.rtld.optional_static_tls 'default … is 512 bytes' (https://raw.githubusercontent.com/bminor/glibc/master/manual/tunables.texi). jemalloc 5.4.0 fixed 'TSD lifecycle edge cases … late deallocations after thread teardown' (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog). TCMalloc warns against loading into a running binary (https://raw.githubusercontent.com/google/tcmalloc/master/docs/design.md).

**Fix.** Write a lifecycle specification:
- one initial-exec TLS pointer per thread;
- a reentrancy guard and a bootstrap arena that works before TLS and in ld.so;
- the signal-triggered report only sets a flag, and the work runs later with raw write();
- an exit and late-free policy: never tear down, and handle frees after TLS destruction;
- in the fork child, reset caches and re-key or document secret sharing (zygote), and restart helper threads lazily.

### 19. [major] §5.7 profile 'latency'; no real-time section

**Problem.** There is no bounded-latency or real-time mode, even though PREEMPT_RT has been in mainline Linux since 6.12. v1's slow paths include madvise calls, first-touch page faults, locks without priority inheritance, decay maintenance piggy-backed on allocation, and PSI wakeups. Its 'latency' profile name implies guarantees the design cannot give.

**Evidence.** Linux 6.12 'includes realtime support (PREEMPT_RT)' (https://kernelnewbies.org/Linux_6.12). TLSF: 'constant cost O(1)', 'a maximum of 168 processor instructions in a x86 architecture' (http://www.gii.upv.es/tlsf/). o1heap: constant worst-case execution time with a 'well-characterized … worst-case memory fragmentation' bound (https://raw.githubusercontent.com/pavel-kirienko/o1heap/master/README.md).

**Fix.** Either declare hard real-time a non-goal and point users to TLSF or o1heap, or define an 'rt' profile with:
- a prefaulted, mlocked pool (MAP_POPULATE / MADV_POPULATE_WRITE);
- no syscalls in malloc or free after warm-up;
- PI futexes or lock-free paths;
- maintenance moved off RT threads;
- per-operation work caps;
- published worst-case-latency measurements under cyclictest load.

### 20. [major] §8 'Keep every OS-specific call behind os_linux.c / os_posix.c'; 'portable later'

**Problem.** Portability constraints that should shape the design now are not analysed.
- Windows has no LD_PRELOAD: mimalloc needs a redirection DLL or a static-CRT override. Windows has reserve/commit with commit charge, no fork, and TLS slot limits.
- macOS needs interposition plus malloc-zone registration so frees from system code work. DYLD_INSERT_LIBRARIES is restricted, and TLS slot choices have broken mimalloc.
- musl and Bionic have no glibc __rseq_offset ABI.
- 32-bit and wasm targets cannot reserve 64 GiB.
The POSIX/Linux split is too thin to hide these differences.

**Evidence.** mimalloc on Windows uses a separate redirection DLL and supports static-CRT override. Its 2026 notes 'Extend Windows TLS slot limit from 64 to 1088' and 'revert TLS slots on macOS to 108/109'. On macOS, 'certain security restrictions may apply' to DYLD_INSERT_LIBRARIES (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). mimalloc's zone code calls malloc_zone_register (https://raw.githubusercontent.com/microsoft/mimalloc/main/src/prim/osx/alloc-override-zone.c). Windows reserve/commit: https://learn.microsoft.com/en-us/windows/win32/memory/reserving-and-committing-memory. jemalloc 5.4.0 'Introduce an OS abstraction layer' (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog). Android's default allocator (Scudo) and musl's rseq status are UNVERIFIED.

**Fix.** Define the primitive layer now, modelled on mimalloc's prim layer: reserve, commit, decommit, reset, protect, guard, page and hugepage size, CPU id and TLS. Mark each v1 feature as Linux-only or portable, list non-goals for wasm and 32-bit, and sketch the Windows and macOS override strategy before the front end is frozen.

### 21. [major] §0 Robson's bound, §1 Fragmentation row, §5.7 report

**Problem.** Fragmentation has no formal accounting. v1 does not separate:
- virtual from physical fragmentation;
- rounding waste, slab-tail waste, free bytes cached per layer, and metadata;
- committed-but-not-resident memory, lazyfree (MADV_FREE) pages that are still counted, and THP bloat (zero subpages inside hugepages).
The Bender et al. 2026 paper that v1 cites as [14] shows that allowing a small amount of request fragmentation drops the competitive ratio from Θ(log M) to Θ(log log M). That conflicts with v1's 'No size-class or fit policy escapes this'. v1 also states the bound in terms of the size ratio, while [14] states it in terms of the volume high-water mark M; the two should be reconciled.

**Evidence.** Bender, Conway, Farach-Colton, Komlós, Kuszmaul, Wein, 'Tight Bounds for Memory Allocation With and Without Request Fragmentation', arXiv:2608.28462, 28 Aug 2026 (https://arxiv.org/abs/2608.28462). cgroup v2 memory.stat exposes pglazyfree/pglazyfreed and anon_thp (https://docs.kernel.org/admin-guide/cgroup-v2.html). ISMM 2025 EMD on THP bloat (https://conf.researchr.org/track/ismm-2025/ismm-2025-papers; content UNVERIFIED). Whether real allocators achieve 'request fragmentation' through paging is my interpretation (UNVERIFIED).

**Fix.** Define the counters (requested, rounded, slab-tail, cached-free per layer, metadata, committed, resident from smaps_rollup, lazyfree, THP-resident) and the ratios (peak RSS ÷ peak live; external ÷ resident; per-layer waste). Use the same definitions in the explain report, the benchmarks and the regression gates. Revisit §0's theory claim in light of [14].

### 22. [major] §5.7 soham_stats() and the text report; no debug build; telemetry export unspecified

**Problem.** Observability is only sketched. v1 has no:
- stable, machine-readable stats schema;
- versioned names;
- cost model (per-CPU counters versus atomics);
- export path (JSON, protobuf text, pprof profiles, USDT/eBPF probes, Windows ETW);
- compatibility for mallinfo2, malloc_info and malloc_stats.
There is also no debug build design: padding and canaries, junk fill, detection of double and invalid frees with stack traces, leak reports at exit.

**Evidence.** glibc: mallopt and mallinfo2 'will not have any effect or return incorrect statistics when a replacement malloc is in use' (https://raw.githubusercontent.com/bminor/glibc/master/manual/memory.texi). mimalloc debug mode pads objects, detects double frees and invalid pointers, and supports ETW (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). jemalloc 5.3.1 added debug-build double-free detection and JSON stats alignment (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog). TCMalloc exposes MallocExtension::GetStats and pbtxt output (https://raw.githubusercontent.com/google/tcmalloc/master/docs/stats.md).

**Fix.** Write a telemetry specification:
- a versioned JSON schema plus a pprof heap profile;
- USDT probes on slow paths, sampled events and purges;
- mallinfo2, malloc_info and malloc_stats shims that map onto sohamalloc's counters;
- a cheap per-CPU counter design with a stated cost;
- a SOHAMALLOC_DEBUG build with canaries, junk fill, free-time checks and an at-exit leak report built from the sampler.

### 23. [major] Absent: deterministic / reproducible mode

**Problem.** Randomised free-list order, guard sampling, randomised reservation addresses and time-based decay make bugs and benchmark runs hard to reproduce. There is no seeded or deterministic mode and no reproducible-build policy. How rseq interacts with record-and-replay tools such as rr is not considered (UNVERIFIED).

**Evidence.** mimalloc provides MIMALLOC_GUARDED_SAMPLE_SEED 'to reproduce a buffer overflow', and v3.4.5 (Aug 2026) 'make[s] the build deterministic' (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md). How rr handles rseq is UNVERIFIED.

**Fix.** Add a SOHAMALLOC_SEED setting and a 'deterministic' profile:
- fixed seeds and fixed reservation hints;
- decay driven by allocation counts instead of wall-clock time;
- no background thread;
- rseq off, with per-thread caches.
Also adopt reproducible builds, and test with rr and with fixed-seed fuzz runs.

### 24. [major] §5.1 '10,000-thread process gets at most #CPUs caches'; §8 Phase 1 abandoned-heap reclamation

**Problem.** Behaviour with many threads and with thread churn is untested. Fallback per-thread caches, per-thread TLS initialisation, the cost of flushing a cache at thread exit, reclaiming abandoned heaps, and contention in the middle layer are not budgeted. Google reports processes with 'many hundreds or thousands of threads', which makes per-thread free lists unviable.

**Evidence.** Oskolkov, LKML, 2022: 'the process may often have many hundreds or thousands of threads, so per-thread freelists are not a viable solution' (https://lkml.iu.edu/2208.0/01988.html). The jemalloc 5.4.0 TSD lifecycle fixes show how intricate thread-exit handling is (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog).

**Fix.** Add benchmarks for 10k mostly-idle threads, 1M create/exit cycles, and producer/consumer with 1k threads. Target per-thread overhead in bytes and exit-flush latency. Attach caches lazily, steal from idle caches, and bound the cost of scanning abandoned heaps.

### 25. [major] §5.1 elastic capacity, §5.5 NUMA/NUCA row (no heterogeneous-core treatment)

**Problem.** Heterogeneous CPUs (Intel P+E cores, Arm big.LITTLE and DynamIQ) are not considered. Cores differ in capacity and cache sharing, and the energy-aware scheduler migrates tasks between them. That affects LLC-domain mapping, per-CPU cache sizing, rseq abort rates and energy use.

**Evidence.** Kernel docs: 'Heterogeneous systems are also called asymmetric CPU capacity systems … Arm big.LITTLE systems are an example' (https://docs.kernel.org/scheduler/sched-capacity.html). Specifics of Intel E-core cluster cache sharing and migration rates are UNVERIFIED.

**Fix.** Add a topology module that reads /sys/devices/system/cpu/*/cache and cpu_capacity. Map transfer caches to actual cache-sharing groups, not just the LLC, and weight elastic capacity by core class. Benchmark on hybrid x86 and Arm hardware, and add energy per operation (RAPL or Arm energy model) to the §7 metrics.

### 26. [major] §5.5 NUMA/NUCA row ('Node from rseq node_id. Optional mbind per node arena'); CXL/tiered memory absent

**Problem.** Memory tiering is missing: CXL or slow tiers, kernel demotion and promotion, and weighted interleave. The allocator's main lever there, keeping hot and cold objects on separate pages so the kernel can tier whole pages, is not mentioned. Also missing: AutoNUMA migrating pages underneath per-node heaps, policy for remote frees that cross nodes, and evidence on whether per-node structures pay off.

**Evidence.** TPP: Transparent Page Placement for CXL-Enabled Tiered-Memory (ASPLOS 2023), https://arxiv.org/abs/2206.02878. Weighted interleave memory policies arrived in Linux 6.9 (https://kernelnewbies.org/Linux_6.9). ISMM 2026 'Bandwidth Speaks, We Listen: Dynamic Memory Interleaving for Tiering' (https://conf.researchr.org/track/ismm-2026/ismm-2026-papers; content UNVERIFIED). TCMalloc has __hot_cold_t access hints and numa_aware partitions that it mbinds (https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/malloc_extension.h). Google saw no material gain from per-NUMA vCPUs (https://lkml.iu.edu/2208.0/01988.html).

**Fix.** Add a NUMA and tiering section:
- hot/cold placement classes fed by hints and MemProf, with hot and cold kept off the same pages;
- per-node page heaps only on multi-node machines and only where measured to help;
- cooperation with kernel demotion (avoid mixing temperatures within a hugeblock);
- respect for the process's mempolicy, including weighted interleave;
- per-node stats in the explain report.

### 27. [minor] §3 L4 / §5.2 adaptive remote frees

**Problem.** The remote-free design overlooks recent work specifically on message-passing allocators under producer/consumer load (BatchIt, ISMM 2024). Without it, the adaptive switch and its threshold are guesses with no baseline.

**Evidence.** ISMM 2024 program: 'BatchIt: Optimizing Message-Passing Allocators for Producer-Consumer Workloads: An Intellectual Abstract', Filardo and Parkinson (Microsoft Azure Research) (https://conf.researchr.org/track/ismm-2024/ismm-2024-papers). Its content is UNVERIFIED: I could not fetch the abstract after the search budget ran out.

**Fix.** Read BatchIt and current snmalloc, include their producer/consumer benchmarks, and justify the switching threshold with measurements, possibly using hysteresis per heap pair.

### 28. [minor] §2 landscape and §7 comparison set ('jemalloc 5.3.1, TCMalloc, mimalloc v3, snmalloc')

**Problem.** The landscape and comparison set are out of date.
- jemalloc 5.4.0 shipped on 17 Sep 2026 with adaptive per-bin tcache sizing (overlapping v1's 'elastic capacity').
- jemalloc 5.3.1 added process_madvise batching, calloc_madvise_threshold and free_sized.
- mimalloc v3.4.5 (Aug 2026) adds first-class heaps, a size-returning API, separated metadata and THP-aware purging.
- glibc 2.42 lets tcache hold large blocks; 2.43 adds free_sized and AArch64 THP.
- The security allocators v1 compares its tiers with (Scudo, hardened_malloc, PartitionAlloc) are not in the benchmark set.

**Evidence.** https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog; https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md; https://raw.githubusercontent.com/bminor/glibc/master/NEWS

**Fix.** Refresh §2 and §7. Pin jemalloc 5.4.0, mimalloc v3.4.x, glibc 2.43/2.44, current TCMalloc and snmalloc. Add Scudo, hardened_malloc and PartitionAlloc for security-tier cost comparisons. Re-check which of §6's 'genuinely different' items peers already ship.

### 29. [minor] §5.7 Extended API 'Heaps/arenas with bulk destroy'

**Problem.** The arena API has no semantics and no research behind it: which threads may allocate from a heap, whether destroy frees or only detaches, what free() of a heap object does after destroy, and how heaps appear to profilers. Recent work re-examines when custom and region allocators actually help, and mimalloc fixed races in heap destroy in 2026.

**Evidence.** ISMM 2026: 'Reconsidering "Reconsidering Custom Memory Allocation"', van Kempen and Berger (https://conf.researchr.org/track/ismm-2026/ismm-2026-papers; content UNVERIFIED). mimalloc release notes: 'fix race in mi_heap_destroy (v3)', 'fix concurrency bug in concurrent heap destroy (v3)', 'true first-class heaps where one can allocate in heap from any thread' (https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md).

**Fix.** Specify the heap API contract (thread-safety, destroy versus delete, interaction with free and realloc, visit and stat hooks), model-check heap destroy, and evaluate it against plain malloc on the workloads from van Kempen and Berger before promoting it.

### 30. [minor] Absent: licensing, provenance, governance; repo contents

**Problem.** There is no license, contribution policy, security-disclosure process, release or ABI-stability policy (symbol versioning for the extended API), or record of where borrowed code came from. The repo already holds verbatim Apache-2.0 TCMalloc sources (hpf.h is identical to upstream huge_page_filler.h; hps.h carries the TCMalloc Apache-2.0 header) and has no LICENSE or NOTICE file. An allocator gets linked into almost everything, so licence choice decides who can adopt it. jemalloc's archival and revival show the governance risk.

**Evidence.** /home/user/sohamalloc/hpf.h and /home/user/sohamalloc/hps.h headers ('Copyright 2019 The TCMalloc Authors … Apache License, Version 2.0'); hpf.h is identical to https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/huge_page_filler.h. Peer licences: mimalloc MIT (https://raw.githubusercontent.com/microsoft/mimalloc/main/LICENSE), snmalloc MIT, rpmalloc MIT, hardened_malloc MIT-style, jemalloc BSD-style (https://raw.githubusercontent.com/jemalloc/jemalloc/dev/COPYING). jemalloc governance history is v1 reference [7].

**Fix.** Pick a permissive licence early (MIT, or Apache-2.0 for its patent grant). Add LICENSE and NOTICE files, plus a THIRD_PARTY record for any copied TCMalloc code, or move reference copies out of the source tree. Add SECURITY.md and a CVE process, semantic versioning with versioned symbols for soham_*, reproducible builds, and a maintainer and bus-factor plan.

### 31. [minor] §5.5 RSS honesty row (MADV_DONTNEED by default) — swap, zswap and virtualisation not considered

**Problem.** Free memory that the allocator keeps in caches is still dirty anonymous memory. Under pressure with swap or zswap, the kernel may swap that garbage out, paying I/O for data that will never be read (my reasoning). Hosts reclaiming guest memory also cannot reclaim pages the allocator holds (UNVERIFIED for virtio free page reporting). The DONTNEED versus FREE choice and the decay timers should take this into account.

**Evidence.** cgroup v2 memory.stat reports pglazyfree/pglazyfreed and zswap and swap events, so the effect is measurable (https://docs.kernel.org/admin-guide/cgroup-v2.html). The swap-out-of-free-memory mechanism is reasoning, not a measured result, and is UNVERIFIED.

**Fix.** Detect active swap or zswap (meminfo or memory.swap.current). Under reclaim pressure, discard cached free memory (MADV_FREE, or DONTNEED in strict-RSS mode) before the kernel swaps it, and consider MADV_COLD for caches that are cold but likely to be reused. Add a benchmark under swap pressure that counts swap-outs of freed memory.
