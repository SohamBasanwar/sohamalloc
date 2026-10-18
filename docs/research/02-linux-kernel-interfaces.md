# Linux kernel interfaces for user-space allocators (2024–2026), and what "kernel friendly" means for sohamalloc

*Report date: 2026-10-18. The current kernel docs are built from **7.3.0-rc7** [20][41]. I checked version numbers against the man pages, kernel docs, or the UAPI headers and source at specific `torvalds/linux` tags. Bracketed numbers point to the Sources list. **INFERENCE** marks my own reasoning from a mechanism, with no document stating it. **UNVERIFIED** marks things I could not confirm from a primary source.*

---

## 0. Summary: findings that change the v1 design

1. **`process_madvise()` now works as a batched `madvise` on yourself.**
   - 6.12: self-targeting no longer needs `CAP_SYS_NICE`.
   - 6.13: any advice value is allowed on the calling process.
   - 6.15: `PIDFD_SELF` (−10000) means no `pidfd_open` is needed.
   - 6.16: `MADV_DONTNEED`/`FREE` across the whole iovec are flushed with **one** TLB flush.
   - 6.17: `DONTNEED`/`FREE` take only a **per-VMA read lock**, never `mmap_lock`, when each range sits inside one VMA.
   - These come from source at each tag [3][4][5][6][8]. The man page still says `CAP_SYS_NICE` is required. That is out of date for the self case: the code checks capability only when `mm != current->mm` [2][3].
   - jemalloc `dev` already uses `process_madvise(PIDFD_SELF, iov, n, MADV_DONTNEED, 0)` behind `opt.process_madvise_max_batch`, which is off by default (0) [9][10].
2. **`MADV_HUGEPAGE` and `MADV_NOHUGEPAGE` take `mmap_lock` for write and change VMA flags.** They are the default case in `get_lock_mode()` [3]. Each sub-range you toggle can split a VMA. Go hit VMA explosions in 2015 [54], and 100+ ms direct-reclaim stalls in Go 1.21 [52][53]. The v1 line "`MADV_HUGEPAGE` on the heap region" needs version and mode gating (§4).
3. **Freeing 4 KiB inside a PMD-mapped THP does not free memory right away.** The THP becomes partially mapped and goes on the deferred-split queue, which "are going to be split under memory pressure" [12]. Sub-release inside a hugepage is therefore weaker than v1 assumes.
4. **The 6.12 underused-THP shrinker splits THPs by counting zero-filled 4 KiB subpages**, controlled by `max_ptes_none` and `shrink_underused` [12][14][15]. **INFERENCE:** a hardened tier that zeroes on free will make freed-but-retained memory look "underused". THPs get split and the zero subpages are remapped to the shared zeropage.
5. **TCMalloc does *not* index its caches by `mm_cid` by default.** `RseqVcpuMode::kMM` is chosen only under `Experiment::TEST_ONLY_MM_VCPU`; otherwise it uses `cpu_id` [31][32].
   - TCMalloc registers its **own** `__rseq_abi`. With glibc ≥2.35 that registration fails with `EBUSY` and TCMalloc drops to its slow mode [35][31].
   - MongoDB tells users to set `GLIBC_TUNABLES=glibc.pthread.rseq=0` [34]. That is the opposite of "drop-in".
   - sohamalloc's `mm_cid` cache idea is still uncommon in shipping allocators, but the semantics are subtle (§5).
6. **rseq changed a lot in 6.19, 7.0 and 7.1.**
   - 6.19: `mm_cid` was rewritten [24][25][94].
   - 7.0: time-slice extension added (`slice_ctrl`) [21][26].
   - 7.1: a separate "optimized V2" mode. Registrations larger than 32 bytes **kill the process if user space writes kernel-owned fields** [20][22][100].
   - So no TCMalloc-style overlay on `cpu_id_start` is possible on a glibc-owned V2 area.
   - Feature detection has to handle glibc's history of `__rseq_size`: 32 for glibc 2.35–2.39, 20 for 2.40, and `AT_RSEQ_FEATURE_SIZE` from 2.41 on [27][28][29].
7. **`MADV_FREE` pages stay in RSS and are moved to the inactive *file* LRU** [47][48]. smaps reports them as `LazyFree`, which "might be lower than the real value" [46]. Go returned to `MADV_DONTNEED` in 1.16 because monitoring tools were confused [49][51].
8. **PSI triggers are open to unprivileged processes since 6.4, but the window must be a multiple of 2 s** [41][42][43]. A library cannot get sub-second pressure notifications without privilege.
9. **`mremap()` of faulted anonymous memory "can almost never" merge with neighbours afterwards** [67]. v1's "`mremap` for realloc inside one reserved region" would split the reservation into many unmergeable VMAs.

---

## 1. Address-space operations: cost, locking, VMA economy

**`mmap_lock` and per-VMA locks.**
- Most VMA changes require `mmap_write_lock()`. A VMA write lock requires the mmap write lock first. `munmap()` detaches VMAs under the mmap write lock, and `mremap()` takes the mmap lock, the VMA lock and the rmap locks [69].
- Per-VMA locks arrived in **6.4** [43][70]. Page faults look up the VMA under RCU with `lock_vma_under_rcu()` and fall back to `mmap_lock` if that fails [69].
- Consequence: every `mmap`, `munmap`, `mprotect`, `mremap`, and `madvise(MADV_[NO]HUGEPAGE)` in a steady state serialises against the other address-space writers, and it forces faulting threads onto the slow path when they collide [3][69].
- `/proc/pid/maps` reads moved to per-VMA locks in mid-2025 (commit 5631da56c9; release UNVERIFIED, likely 6.17). That matters if the allocator reads its own maps.

**madvise lock modes on master** [3]:

| Advice | Lock |
|---|---|
| `DONTNEED`, `DONTNEED_LOCKED`, `FREE` (6.17+); `GUARD_INSTALL`/`REMOVE` (6.19+) | Per-VMA read lock, only when the range is inside one VMA, the mm is local, and userfaultfd is not armed |
| `WILLNEED`, `COLD`, `PAGEOUT`, `POPULATE_*`, `COLLAPSE`, `REMOVE` | `mmap_lock` read |
| `HUGEPAGE`, `NOHUGEPAGE`, `DONTFORK`, `WIPEONFORK`, `DONTDUMP`, `MERGEABLE`, everything else | `mmap_lock` write |

I checked these versions by diffing `mm/madvise.c` at v6.16 and v6.17 (VMA lock for DONTNEED/FREE), and v6.17 and v6.19 (guards moved over) [3].

**VMA count.**
- `vm.max_map_count` defaults to 65530 (`USHRT_MAX − 5`) [61][62]. Fedora 39 and Arch (2024) raised it to 1048576 for games [63][64]. Do not count on either value.
- `mmap` returns `ENOMEM` when the mapping limit is exceeded. **`munmap` in the middle of a mapping can also fail this way**, because it leaves two mappings [65].
- Things that split VMAs or block merges:
  - `mprotect` on sub-ranges (flag change).
  - `MADV_[NO]HUGEPAGE` sub-ranges [3].
  - `mbind` on part of a mapping: "Linux splits the existing virtual memory area into 2 or 3 VMAs" [85].
  - `PR_SET_VMA_ANON_NAME`: a different name "might prevent it from being merged" (5.17+, 80-byte limit) [89].
  - `mremap`-moved faulted anonymous VMAs [67].
  - `PROT_NONE` guard mappings. hardened_malloc tells users to raise `max_map_count` to 1048576 because of these [78].

**`mremap`.**
- `MREMAP_DONTUNMAP` arrived in 5.7 (private anon only before 5.13). Since **6.17**, a pure move (`MAYMOVE|FIXED`, equal sizes) may span several VMAs, keeping the gaps [66][68].
- The man page says mremap "can be used to implement a very efficient realloc" [66]. It moves page tables, not data.
- The cost is mergeability. The proposed `MREMAP_RELOCATE_ANON` fix (LSFMM 2025) is not documented as merged [67]. UNVERIFIED whether it landed.

**Useful `mmap` flags.**
- `MAP_FIXED_NOREPLACE` (4.17) fails with `EEXIST` instead of clobbering an existing mapping [65]. TCMalloc uses it for hinted reservations [36].
- `MAP_NORESERVE` skips swap reservation, so a write can fail with SIGSEGV when memory runs out [65].
- `MAP_POPULATE` hides errors. `MADV_POPULATE_READ`/`WRITE` (5.14) report them [1][65].

---

## 2. TLB shootdowns and batching

- Zapping user PTEs (`munmap`, `MADV_DONTNEED`, `MADV_FREE` on present pages, `mprotect` downgrades) requires invalidating the TLBs of every CPU that may cache that mm.
- On x86 this has meant IPIs. The kernel flushes either the whole TLB or individual pages with `invlpg`. The trade-off is "cycles you spend doing invlpg" against "cycles that you spend refilling the TLB later". The threshold is the debugfs knob `tlb_single_page_flush_ceiling` [71]. I could not confirm its default here (UNVERIFIED).
- **INVLPGB broadcast invalidation (AMD Zen 3+, merged 6.15)** removes the IPIs [5][72]:
  - It is used only for processes active on ≥3 CPUs, a threshold that rises as PCID space runs out.
  - It cut TLB invalidation overhead in one workload from "40% of the total CPU time to only around 4%".
  - will-it-scale `tlb_flush2_threads` went from 527k to 1157k loops/s, but only once `lru_add_drain` was also removed. INVLPGB alone moved the contention to the LRU lock [72].
  - Intel RAR follow-up work exists. Its merge status is UNVERIFIED.
- **madvise TLB batching (6.16):** one `mmu_gather` per call or per `process_madvise` vector, flushed once [3][8][97]. The RFC measured an improvement of roughly 6.9% (batch of 2) to ~40% (batch of 128) for `process_madvise(MADV_DONTNEED)` over 256 MiB. That is a microbenchmark [7].
- Research context: Amit et al. (EuroSys 2020 best paper) show that aggressive batching can create correctness problems, and that faster shootdowns plus skipping some (for example on CoW faults) work better [73].
- **Allocator rules:**
  - Release rarely and in large, contiguous, preferably 2 MiB-aligned ranges.
  - Release from one place, in batches.
  - Avoid `mprotect` on the hot path.
  - Expect the cost to scale with the number of CPUs the process has run on.

---

## 3. `madvise()` advice values for an allocator

| Advice | Since | Semantics for an allocator | Pitfalls |
|---|---|---|---|
| `MADV_DONTNEED` | BSD | Private anon pages come back zero-filled, and RSS drops at once [1] | Fault plus zeroing on reuse. Fails with `EINVAL` on locked pages [1] |
| `MADV_DONTNEED_LOCKED` | 5.18 | Same, but works on mlocked ranges [91] | TCMalloc probes for it and prefers it [36] |
| `MADV_FREE` | 4.5 (4.12 for swapless and LRU fixes) | Freeing "could be delayed until memory pressure"; a later write cancels the free; private anon only [1] | Contents are indeterminate: either old data or zero. Stays in RSS (§7) |
| `MADV_COLD` / `MADV_PAGEOUT` | 5.4 | Deactivate, or reclaim now (anon is swapped out) [1] | Never use on *free* memory: PAGEOUT would swap out garbage. Only useful for cold *live* data |
| `MADV_HUGEPAGE` / `NOHUGEPAGE` | 2.6.38 | Opt a range in or out of THP. "Can very easily waste memory" [1] | mmap write lock, VMA split [3], direct-reclaim stalls when `defrag=madvise` [52] |
| `MADV_COLLAPSE` | 6.1 | Best-effort synchronous collapse that ignores the THP sysfs mode [1][12] | Can enter direct reclaim. Go removed its use after production stalls under the heap lock [53]. Fails `EBUSY` over the memcg limit [1] |
| `MADV_POPULATE_READ`/`WRITE` | 5.14 | Prefault with error reporting [1] | Holds `mmap_lock` read [3]. Partial population on failure [1] |
| `MADV_GUARD_INSTALL`/`REMOVE` | 6.13 (file and read-only mappings in 6.15) | PTE-marker guards with no VMA cost; they survive DONTNEED, FREE, PAGEOUT and COLD [1][5][76][77] | Install zaps existing pages. mlock and POPULATE fail on guarded ranges [1]. Visible in pagemap since 6.15 [5] |
| `MADV_WIPEONFORK` | 4.14 | Child sees zeros [1] | Good for per-process secrets such as free-list XOR keys |
| `MADV_DONTDUMP` | 3.4 | Excludes the range from core dumps [1] | Mesh uses it on free spans [83] |

`IORING_OP_MADVISE` (5.6) can issue madvise asynchronously through io_uring [90]. It is an alternative to a background thread, but setting up io_uring inside a malloc is heavy. Not recommended by default.

---

## 4. `process_madvise()`: batched release on yourself

- **Syscall:** `process_madvise(pidfd, iov, n, advice, 0)`. It has existed since 5.10 (glibc 2.36). `n ≤ IOV_MAX`, and the kernel's `UIO_MAXIOV` is 1024 [2][93]. It returns the bytes advised and may stop partway [2].
- **Self-targeting by version** (from source at tags [3], plus [4][6]):
  - ≤6.11: requires `CAP_SYS_NICE` even for self, so it is unusable by normal processes.
  - 6.12: capability checked only for remote mms.
  - 6.13: any advice allowed on self. Remote targets are limited to `COLD`, `PAGEOUT`, `WILLNEED`, `COLLAPSE` [2][3].
  - 6.15: `PIDFD_SELF` = `PIDFD_SELF_THREAD` = −10000 [6]. Before that, `pidfd_open(getpid())` costs an fd and is subject to `RLIMIT_NOFILE`, which was one motivation for `PIDFD_SELF` [5].
  - 6.16: one TLB flush per vector for `DONTNEED`, `DONTNEED_LOCKED`, `FREE` [3].
  - 6.17: per-VMA read locking per iovec element. `vector_madvise()` takes no `mmap_lock` up front for these advice values [3].
- **Probe the way jemalloc does:** treat `EPERM`, `EINVAL`, `ENOSYS`, `EBADF` as "unsupported" and latch a global gate off [9].

---

## 5. THP, mTHP, khugepaged, and the underused-THP shrinker

**Modes.**
- Top-level and per-size `enabled` take `always|madvise|never`, and per-size can be `inherit`. The PMD size defaults to `inherit`, other sizes to `never` [12].
- `defrag` takes `always|defer|defer+madvise|madvise|never`; the default is `madvise` [12].
- `MADV_COLLAPSE` ignores all of these [12].
- A proposed `enabled=defer` mode had not landed in the current docs I checked [98].

**mTHP and khugepaged.**
- Anonymous mTHP (sizes from `hugepages-<size>kB`) arrived in **6.8**. It shows up first in the v6.8 docs [13][16].
- khugepaged collapse of *anonymous* mTHP first appears in the **v7.2** docs, with `max_ptes_none` limited to 0 or 511 for mTHP. I inferred 7.2 from diffing the docs; the patch series reached v15 in February 2026 [13][17].
- The series authors warned that `max_ptes_none > 255` causes "creep" through mTHP sizes [99].

**Underused shrinker (6.12)** [12][14][15].
- Every THP created at fault or collapse time goes on the deferred list.
- A THP whose count of zero-filled subpages exceeds `max_ptes_none` is split under pressure, and the zero subpages are remapped to the shared zeropage. Writing 0 to `shrink_underused` disables it.
- Meta's numbers: THP=always used 58.8 GB without the shrinker and 55.9 GB with it, against 54.6 GB for THP=madvise. Performance was +1.7% with it and +1.8% without, compared with madvise [15].
- Stats: `thp_underused_split_page`, plus per-size `split_deferred`, `nr_anon_partially_mapped` [12].

**Per-process control.**
- `PR_SET_THP_DISABLE` (3.15).
- `PR_THP_DISABLE_EXCEPT_ADVISED` (**6.18**, from the doc diff) turns THP=always into madvise mode for this process only. It is inherited across fork and kept across execve [13][18][19].
- mimalloc calls `PR_SET_THP_DISABLE` when its `allow_thp=0` [60].

**What other allocators recommend.**
- TCMalloc builds and tests with `enabled=always`, `defrag=defer+madvise`, `max_ptes_none=0`, `overcommit_memory=1` [39].
- TCMalloc calls `MADV_COLLAPSE` from its background thread (`usermode_hugepage_collapse` on in optimised builds) and marks cold heaps `MADV_NOHUGEPAGE` [36][37][38][96].

**Partial release.** Partial `munmap` or `mprotect` splits only the page-table entries, and the folio waits for a deferred split [12]. Releasing fragments of a hugepage therefore recovers memory slowly.

---

## 6. rseq: `cpu_id`, `node_id`, `mm_cid`, glibc, TCMalloc

**ABI layout** [21]:

| Offset | Field |
|---|---|
| 0 | `cpu_id_start` |
| 4 | `cpu_id` |
| 8 | `rseq_cs` |
| 16 | `flags` |
| 20 | `node_id` |
| 24 | `mm_cid` |
| 28 | `slice_ctrl` |
| 32 | `__reserved` |

- `node_id` and `mm_cid` arrived in **6.3**: they are absent from the v6.2 header and present in v6.3, and TCMalloc's comment agrees [31].
- `mm_cid` is "the current thread's concurrency ID (allocated uniquely within a memory map)" [21]. The proposal describes it as allowing "ideal scaling (down or up) of user-space per-cpu data structures", taking into account running threads, affinity and cpusets [23].
- The proposed `mm_numa_cid` is **not** in the current UAPI [21][23].

**6.19 rewrite** [24][25][94].
- Prompted by glibc's rseq enablement, which caused a 3–4% I/O regression.
- Keeps "per task CIDs as long as the number of threads is not exceeding the number of possible CPUs". Beyond that it uses per-CPU ownership with mode switches in fork and exit.
- Phoronix reports PostgreSQL gains of about 14–18% (secondary source).

**7.0 time-slice extension** [20][26].
- Set `slice_ctrl.request=1` around a critical section. If `granted` is set at exit, call `rseq_slice_yield()`.
- Enable with `prctl(PR_RSEQ_SLICE_EXTENSION, …)`. Default length 5 µs, tunable up to 50 µs.
- Any syscall during the extension revokes it.

**7.1 "optimized V2"** [20][22][100].
- Chosen when `rseq_len > 32`. IDs are updated only when they change.
- Kernel-owned fields are enforced read-only: "If user space modifies them, the process is killed".
- Required for slice extension. Registering at exactly 32 bytes keeps legacy mode.

**glibc.**
- Since 2.35 glibc registers rseq for every thread and exports `__rseq_offset`, `__rseq_size` and `__rseq_flags` [29].
- `__rseq_size` was 32 for 2.35–2.39 although only 20 bytes were active, 20 in 2.40, and `AT_RSEQ_FEATURE_SIZE` from 2.41 [27][28][29].
- The kernel added a reserved byte (feature size 33) so that 32 is no longer ambiguous [21][27].
- Field check: `__rseq_size >= offsetofend(struct rseq, field)`, with special handling for 32 and 20: use `min(32, getauxval(AT_RSEQ_FEATURE_SIZE))` [27][28].
- Opt-out: `glibc.pthread.rseq=0` [30].

**TCMalloc.**
- Defines its own TLS `__rseq_abi`, laid out so that its cached slabs pointer overlaps `cpu_id_start`. Its docs say "this makes `__rseq_abi.cpu_id_start` unusable for its original purpose" [32][33].
- Calls `syscall(__NR_rseq, &__rseq_abi, …)`, which fails if glibc has already registered [31][35].
- Indexes by `mm_cid` only under a test-only experiment [31].
- Registers `MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ` so it can fence other CPUs' critical sections [31].
- Its `kernel_rseq` struct is the 32-byte layout plus a prototype vCPU union, so it stays in legacy mode (INFERENCE from [22] and the struct).

---

## 7. Pressure signals and accounting

**PSI** [41][42][43].
- `/proc/pressure/memory` and per-cgroup `memory.pressure` report `some` and `full` averages over 10, 60 and 300 s, plus `total` in µs.
- Triggers: write `"some 150000 1000000"`, then `poll()` for `POLLPRI`. Each trigger needs its own fd. Windows run from 500 ms to 10 s, and events are rate-limited to one per window.
- Unprivileged users can create triggers **since 6.4**, but only with windows that are multiples of 2 s.
- PSI is *reactive*: it fires after tasks have stalled.

**cgroup v2** [44].
- `memory.current` is total usage, including page cache and some kernel memory per the introduction.
- `memory.high` throttles the cgroup and "never invokes the OOM killer". `memory.max` invokes the OOM killer.
- `memory.events` holds `low/high/max/oom/oom_kill/oom_group_kill/sock_throttled`, and "a value change … generates a file modified event", so it can be watched with inotify.
- `memory.stat` includes `anon`, `file`, `inactive_file`, `pglazyfree`, `pglazyfreed`. The docs warn that `inactive_foo + active_foo` may not equal `foo`.
- `memory.reclaim` (5.19, doc diff) and `memory.peak` (5.19) also exist.
- Cost: reading `memory.stat` can trigger an rstat flush. Tail latencies in the milliseconds were reported, and "a call to cgroup_rstat_flush() is not cheap" [45]. `memory.current` and `memory.max` are simple counters (INFERENCE; I did not benchmark them).
- A process finds its cgroup from `/proc/self/cgroup`, then reads `/sys/fs/cgroup/<path>/memory.*`. That costs a few syscalls with no privilege needed.

**How `MADV_FREE` pages are accounted.**
- They stay mapped and counted as anonymous memory until reclaim, "so application's RSS becomes bigger" [48].
- Since 4.12 they are moved to `LRU_INACTIVE_FILE` and marked !SwapBacked [47]. They show as smaps `LazyFree`, which may be under-reported [46].
- **INFERENCE:**
  - They stay charged in `memory.current` until reclaimed.
  - They appear in `inactive_file`. Kubernetes and cAdvisor working set is usage minus `inactive_file` (secondary source [92]), so lazyfree pages are probably *excluded* from working-set eviction metrics.
  - Under `memory.high` reclaim, clean lazyfree pages are dropped cheaply.
- Go's verdict: MADV_FREE "doesn't affect many of the statistics … until the memory is actually reclaimed", which means "confusing stats in `top`" and "bad integration with management systems" [49].

---

## 8. Security-adjacent interfaces

- **`mseal`** (6.10, from the header diff [75][87]):
  - Once sealed, a mapping blocks `munmap`, `mremap`, `mmap` over it, `mprotect`, and destructive madvise (`DONTNEED`, `FREE`, …) on anonymous memory the caller cannot write. 64-bit only, no unseal [74].
  - The docs warn: do not `mseal` memory returned by `malloc` [74].
  - Possible use: seal the allocator's *reservation layout* when it never needs to change. Writable data VMAs can still use `DONTNEED`.
  - Since 6.15, `CONFIG_MSEAL_SYSTEM_MAPPINGS` seals the vDSO and similar mappings [5][74].
- **`MAP_DROPPABLE`** (6.11) [79][87]:
  - Pages can be zeroed by the kernel under pressure, are never swapped, are not counted as mlocked, and a failed allocation does not OOM.
  - Useless for live objects.
  - **INFERENCE:** it could back a purely optional "free-page cache". Not recommended without measurement.
- **userfaultfd:**
  - `vm.unprivileged_userfaultfd` defaults to 0, which limits unprivileged users to user-mode faults [61].
  - `UFFD_USER_MODE_ONLY` arrived in 5.11. Read-write-protect mode (`UFFD_FEATURE_RWP`, `UFFDIO_RWPROTECT`) arrived in **7.2** [80].
  - Not suitable for a general allocator: privilege, a handler thread, and high latency. RWP could be a research tool for cold-page detection.

---

## 9. memfd and Mesh-style remapping

- `memfd_create` (3.17) creates RAM-backed tmpfs files with seals; `MFD_HUGETLB` since 4.14 [81].
- **Mesh (PLDI 2019)** cut memory use by 16% in Firefox and 39% in Redis [82]. From its source [83]:
  - The arena is `MAP_SHARED` over a memfd.
  - Meshing is `mmap(MAP_SHARED|MAP_FIXED)` of one span's file offset over another.
  - Release is `fallocate(FALLOC_FL_PUNCH_HOLE)`.
  - After `fork`, the child **copies every allocated page into a new memfd** and remaps the arena.
- Costs:
  - Each meshed span adds a mapping (VMA pressure) and needs a TLB shootdown.
  - Memory is shmem, so it counts as `file`/`shmem` in memcg [44] and `RssShmem` in RSS [46], not anonymous.
  - THP follows `shmem_enabled`, not `enabled` [12].
  - `MADV_DONTNEED` does not free shmem backing; `MADV_REMOVE` or punch-hole is needed [1].
- **Verdict:** keep meshing opt-in and limited to long-lived, low-occupancy spans.

---

## 10. NUMA and memory tiering (CXL)

- **Policies:** `MPOL_PREFERRED_MANY` (5.15), `set_mempolicy_home_node` (5.17, from unistd.h), `MPOL_WEIGHTED_INTERLEAVE` (6.9; auto-tuning in 6.16), `MPOL_F_NUMA_BALANCING` (5.12) [8][84][87].
- A VMA policy applies only to pages allocated *after* it is installed. Placement happens at fault time [85].
- TCMalloc uses `mbind(MPOL_BIND|MPOL_F_STATIC_NODES)` on per-node regions [36].
- **Tiering:** `kernel.numa_balancing=2` (`NUMA_BALANCING_MEMORY_TIERING`, 5.18) promotes hot pages to fast memory, with `numa_balancing_promote_rate_limit_MBps` [86][87]. 6.16 enforces `mems_effective` during demotion [8].
- CXL memory usually appears as CPU-less NUMA nodes (UNVERIFIED as a general rule, since it depends on the platform).
- **INFERENCE for allocators:**
  - Keep metadata and per-CPU caches on top-tier nodes.
  - Do not mix tiers in one span.
  - Expect free pages that sit in caches to be demoted.
  - Pages released with `DONTNEED` and refaulted land according to the *current* policy.
- `getcpu`'s answer is current only at the moment of the call [88]. rseq `node_id` gives the same information without a syscall [21].

---

## 11. How other runtimes release memory

| Runtime | Mechanism | Defaults / lessons |
|---|---|---|
| Go | `MADV_FREE` from 1.12 [50]; back to `MADV_DONTNEED` from 1.16 [51]. Background scavenger capped at 1% of mutator time, keeping 10% over the heap goal, with a memory-limit mode [56] | `MADV_DONTNEED` "in a vacuum, 1.5–2x" slower; a rough estimate, not a benchmark [49]. Dropped `MADV_COLLAPSE` and eager `MADV_HUGEPAGE` after stalls [52][53]. Still has `sysHugePageCollapseOS` [55] |
| Java G1 | JEP 346 (JDK 12): periodic GC when idle, then uncommit | A prototype cut committed memory 85% for an idle Tomcat [57] |
| Java ZGC | JEP 351 (JDK 13): uncommits cached ZPages after `ZUncommitDelay` via `fallocate(PUNCH_HOLE)` on tmpfs/hugetlbfs | Never goes below `-Xms` [58] |
| jemalloc | Dirty pages decay (default 10 s) on a sigmoidal curve to muzzy (`MADV_FREE`) or clean; muzzy decay is "disabled by default (with decay time 0)" [10] | Optional background thread [11]; opt-in `process_madvise` batching [9][10] |
| TCMalloc | Releases with `MADV_REMOVE`, then `MADV_DONTNEED_LOCKED` or `DONTNEED`; the madvise preference defaults to `kDontNeed` [36]. Background release rate in bytes/s, where "Zero inhibits the release path" [40] | The open-source build initialises the rate to `BytesPerSecond{0}` [37][38], while the docs call "the default release rate" appropriate [39]. Which default actually applies is UNVERIFIED |
| mimalloc | `purge_delay` 10 ms; `purge_decommits=1`, meaning `MADV_DONTNEED` on Linux "as it decreases rss immediately"; `MADV_FREE` for reset [59][60] | Names its regions with `PR_SET_VMA_ANON_NAME` [60] |
| glibc | `glibc.malloc.hugetlb=1` → `MADV_HUGEPAGE`, `2` → `MAP_HUGETLB`; trim and mmap thresholds of 128 KiB, adjusted dynamically [95] | `glibc.pthread.stack_hugetlb` [30] |

---

## 12. Master table: kernel feature, minimum version, use for allocator, pitfalls

| Kernel feature | Min version | Use for allocator | Pitfalls |
|---|---|---|---|
| Per-VMA locks (faults) | 6.4 [43] | Faults skip `mmap_lock`, so keep the heap in a few VMAs | mmap/munmap/mprotect still need the write lock [69] |
| madvise DONTNEED/FREE under per-VMA lock | 6.17 [3] | Release without blocking other threads' mmap or faults | Only when the range is in one VMA, local, not uffd-armed |
| madvise TLB batching | 6.16 [3][8] | One flush per call or vector | Only `DONTNEED*` and `FREE` |
| `process_madvise` on self without CAP | 6.12 [3] | — | Man page still says CAP_SYS_NICE [2] |
| `process_madvise` any advice on self | 6.13 [2][3] | Vectored release, up to 1024 iovecs [93] | Partial success returns bytes [2] |
| `PIDFD_SELF` | 6.15 [6] | No fd needed | Before 6.15 use `pidfd_open` (uses an fd under RLIMIT_NOFILE) |
| `MADV_FREE` | 4.5 / 4.12 [1] | Cheap warm release | RSS and monitoring confusion [49]; contents indeterminate |
| `MADV_DONTNEED_LOCKED` | 5.18 [91] | Release mlocked heaps | — |
| `MADV_POPULATE_*` | 5.14 [1] | Prefault hugeblocks off the hot path | mmap read lock |
| `MADV_COLLAPSE` | 6.1 [1] | Background collapse of dense blocks | Synchronous, direct reclaim; never under locks [53] |
| `MADV_GUARD_INSTALL` | 6.13 (file-backed 6.15) [1][5] | Guards with no VMA cost | Install zaps pages; no mlock or populate |
| anon mTHP | 6.8 [13][16] | 16–64 KiB folios. Contiguous-PTE benefits on arm64 are UNVERIFIED | Fault-time over-allocation [99] |
| Underused-THP shrinker | 6.12 [12][14] | THP=always becomes less wasteful | Zero-filled pages get split (zero-on-free interaction) |
| `PR_THP_DISABLE_EXCEPT_ADVISED` | 6.18 [13][18] | Per-process "madvise mode" | Inherited across fork and exec [18] |
| khugepaged anon mTHP collapse | 7.2 (doc diff) [13][17] | — | `max_ptes_none` 0 or 511 only for mTHP [12] |
| rseq `cpu_id` | 4.18 per MongoDB [34] (UNVERIFIED primary) | Per-CPU caches | glibc owns the registration (2.35+) [29] |
| rseq `node_id`, `mm_cid` | 6.3 [21][31] | Per-node arenas; dense cache index | Semantics changed in 6.19 [24] |
| rseq time-slice extension | 7.0 [21][26] | Shield central-lock holders from preemption | Needs V2 (7.1 docs) and prctl; 5 µs default [20] |
| rseq optimized V2 | 7.1 [20][100] | Lower scheduler cost | Writing kernel fields kills the process [20] |
| `membarrier` expedited rseq | (TCMalloc uses it) [31] | Fence remote per-CPU caches | Must be registered; version UNVERIFIED here |
| PSI unprivileged triggers | 6.4 [42][43] | Event-driven shrinking | 2 s window multiple; reactive |
| `memory.events` notify | cgroup v2 [44] | inotify on high/max events | Hierarchical vs `.local` |
| `memory.reclaim`, `memory.peak` | 5.19 (doc diff) | Explain mode / tests | `memory.stat` reads may be slow [45] |
| `mremap` multi-VMA move | 6.17 [66] | — | Moved anon VMAs don't merge [67] |
| `MREMAP_DONTUNMAP` | 5.7 [66] | Not needed for malloc | — |
| `MAP_FIXED_NOREPLACE` | 4.17 [65] | Safe hinted reservations | — |
| `PR_SET_VMA_ANON_NAME` | 5.17 [89] | Readable `/proc/pid/maps` | Different names block merges |
| `mseal` | 6.10 [74][87] | Lock down the reservation layout | Blocks mprotect, munmap, mremap forever |
| `MAP_DROPPABLE` | 6.11 [79][87] | (Research) droppable caches | Contents can vanish |
| `MPOL_PREFERRED_MANY` | 5.15 [84] | DRAM-first placement on tiered systems | mbind splits VMAs [85] |
| `set_mempolicy_home_node` | 5.17 [87] | Node affinity for VMA policies | — |
| Weighted interleave | 6.9 [84] | Bandwidth-heavy large arrays (opt-in) | System-wide weights |
| NUMA tiering mode | 5.18 [86][87] | — | Demotes cold cached pages |
| INVLPGB | 6.15, AMD Zen 3+ servers [5][72] | Cheaper release on large machines | Only for mms active on ≥3 CPUs |
| userfaultfd RWP | 7.2 [80] | (Research) cold detection | Privilege; latency |
| `vm.max_map_count` | 65530 default [61] | Budget for VMAs | Distros vary [63][64] |

---

## 13. Recommendations for sohamalloc (version-gated, with fallbacks)

1. **Probe once at start-up and log the result in the explain report.**
   - Kernel version is a hint only; probe each feature: `process_madvise(PIDFD_SELF, …)` on a 4 KiB scratch page, `MADV_GUARD_INSTALL`, `MADV_COLLAPSE`, `AT_RSEQ_FEATURE_SIZE`.
   - Read the THP `enabled`/`defrag`/`shrink_underused`, `max_ptes_none`, `overcommit_memory`, `max_map_count`, the cgroup path, and `memory.max`/`memory.high`.
2. **Release engine.**
   - Collect page runs into ≤1024-entry iovecs.
   - On ≥6.13, issue `process_madvise(self, iov, n, MADV_DONTNEED)`. Use `PIDFD_SELF` on ≥6.15, otherwise a cached `pidfd`.
   - On older kernels, fall back to coalesced `madvise`.
   - On ≥6.16 this is one TLB flush per batch. On ≥6.17 with a single-VMA heap, no `mmap_lock` is taken.
   - Never release under allocator locks. Rate-limit the work (Go's 1% CPU cap is a good model [56]).
3. **RSS honesty.**
   - Default to `MADV_DONTNEED`.
   - Use `MADV_FREE` only for a short "warm" stage, jemalloc style, and only in a throughput profile. Even then, drain to DONTNEED when `memory.current` approaches `memory.high`, or when a monitoring-sensitive profile is selected.
   - Report smaps `LazyFree` in the explain output.
4. **Hugepages.**
   - If THP=always, do nothing. Release whole 2 MiB blocks only, and avoid sub-release unless under pressure (deferred split, §5).
   - If THP=madvise, apply `MADV_HUGEPAGE` **once per 2 MiB-aligned reservation chunk**, never toggled, because toggling costs the mmap write lock and VMA splits.
   - Do not use `MADV_HUGEPAGE` at all when `defrag=madvise` and the user has not asked for it: Go saw 100+ ms stalls [52].
   - Run `MADV_COLLAPSE` only from the optional background thread, on dense blocks, with retry limits. TCMalloc retries at most 3 times on `EAGAIN`/`EINTR` [36].
   - Do not zero-on-free memory you intend to keep THP-backed. Zero it on allocation or with `MADV_DONTNEED` instead (§0.4 INFERENCE).
5. **rseq.**
   - Never call `rseq()` yourself when `__rseq_size != 0`.
   - Read fields through `__rseq_offset`, with librseq-style size detection [27][28].
   - Never write kernel-owned fields; V2 kills the process [20].
   - Use `mm_cid` (≥6.3) as the slot index **only inside rseq critical sections or behind per-slot atomics**. It is unique among the mm's *running* threads at a given instant, not a stable per-thread slot [21][25] (INFERENCE from the semantics).
   - Fall back to `cpu_id`, then per-thread caches. If glibc rseq is disabled and nobody else has registered, register yourself and handle `EBUSY`.
   - Use slice extension around transfer-cache or central locks only on ≥7.1 V2 and only when `RSEQ_CS_FLAG_SLICE_EXT_AVAILABLE` is set [21][22].
6. **Pressure.**
   - Every ≥100 ms of slow-path activity, read `memory.current` and the cached `memory.max`/`memory.high`.
   - Watch `memory.events` with inotify, or use a PSI trigger with a 2 s window, but only in the optional thread.
   - Never read `memory.stat` on the slow path [45].
   - Handle cgroup v1 and missing cgroupfs: treat the limit as "unknown" and decay by time.
7. **VMA economy.**
   - Reserve with `PROT_READ|PROT_WRITE` plus `MAP_NORESERVE`, not `PROT_NONE` plus `mprotect` (which splits VMAs).
   - Under `overcommit_memory=2`, fall back to smaller chunked reservations, as mimalloc's `unix_detect_overcommit` handling does [60].
   - Guards: `MADV_GUARD_INSTALL`. The `PROT_NONE` fallback must be **budgeted** against `max_map_count` minus the current map count.
   - Name whole regions only. Use one reservation per NUMA node instead of `mbind` slices.
   - Use `mremap` only for huge objects that own their mapping.
8. **Things to skip:** `MADV_PAGEOUT` or `MADV_COLD` on free memory, userfaultfd, `MAP_DROPPABLE`, and default-on Mesh.

---

## 14. Problems in v1 found by this research

- **"mm_cid … dense and bounded by min(#threads, #CPUs)" is imprecise.** The kernel guarantees uniqueness within the mm. The 6.19 rewrite keeps per-task CIDs only while threads ≤ possible CPUs, then changes mode [21][25]. CIDs change on preemption or migration, so cache access must be rseq-protected.
- **"Share via `__rseq_offset`" is correct but incomplete.** It omits the `__rseq_size` 32/20/feature-size quirk [27][28] and the 7.1 read-only enforcement [20].
- **"`MADV_HUGEPAGE` on the heap region" and "`MADV_COLLAPSE` for hot blocks" leave out the Go 1.21 stalls and lock-ordering hazards** [52][53], and the mmap write lock [3].
- **"Release whole hugepages, one IPI round" is right.** But v1 assumes sub-release frees memory immediately, which partially-mapped THPs contradict [12].
- **"Wake on a PSI trigger fd" assumes sub-second responsiveness.** Unprivileged triggers have a minimum of 2 s [41].
- **"`mremap` for realloc inside the reservation" creates unmergeable VMAs** [67]. "Optional `mbind` per node arena" splits VMAs [85].
- **"Reserve 64 GiB, commit by touching" fails under `overcommit_memory=2`.** If the commit is done with `mprotect`, it also splits VMAs [61][69].
- **v1 never mentions `process_madvise` self-batching**, the biggest new release-side lever since 6.12–6.17 [3].

## 15. Items I could not verify

- The x86 `tlb_single_page_flush_ceiling` default.
- The exact merge release for `/proc/pid/maps` per-VMA reads (5631da56c9).
- Whether `MREMAP_RELOCATE_ANON` merged.
- Whether musl registers rseq.
- Whether the effective TCMalloc background release-rate default is non-zero outside the open-source build.
- Explicit documentation that lazyfree pages remain in `memory.current` and `inactive_file`. My claim is INFERENCE from [44][47][48].
- The Kubernetes working-set formula, which comes only from secondary sources [92].
- The minimum kernel version for `membarrier` expedited rseq.
- CPU-less NUMA-node presentation of CXL as a general rule.
- The minimum version of rseq `cpu_id` for TCMalloc (4.18, from MongoDB docs only).

---

## Sources

1. https://man7.org/linux/man-pages/man2/madvise.2.html
2. https://man7.org/linux/man-pages/man2/process_madvise.2.html
3. https://raw.githubusercontent.com/torvalds/linux/master/mm/madvise.c (plus the same file at tags v6.10–v7.0, e.g. https://raw.githubusercontent.com/torvalds/linux/v6.17/mm/madvise.c)
4. https://lkml.rescloud.iu.edu/2409.3/00497.html
5. https://kernelnewbies.org/Linux_6.15
6. https://raw.githubusercontent.com/torvalds/linux/v6.15/include/uapi/linux/pidfd.h
7. https://lkml.iu.edu/hypermail/linux/kernel/2503.0/07224.html
8. https://kernelnewbies.org/Linux_6.16
9. https://raw.githubusercontent.com/jemalloc/jemalloc/dev/src/pages.c
10. https://raw.githubusercontent.com/jemalloc/jemalloc/dev/doc/jemalloc.xml.in
11. https://raw.githubusercontent.com/jemalloc/jemalloc/dev/TUNING.md
12. https://docs.kernel.org/admin-guide/mm/transhuge.html
13. https://www.kernel.org/doc/html/v7.2/admin-guide/mm/transhuge.html (also the v6.7, v6.8, v6.12, v6.17, v6.18, v7.1 versions of the same page)
14. https://kernelnewbies.org/Linux_6.12
15. https://lkml.iu.edu/hypermail/linux/kernel/2407.3/09215.html ; https://lwn.net/Articles/986242/
16. https://lkml.iu.edu/hypermail/linux/kernel/2312.0/08512.html
17. https://lkml.iu.edu/hypermail/linux/kernel/2602.3/04237.html
18. https://www.man7.org/linux/man-pages/man2/pr_set_thp_disable.2const.html
19. https://lkml.iu.edu/2511.0/08482.html
20. https://docs.kernel.org/userspace-api/rseq.html
21. https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/rseq.h (also the v6.2, v6.3, v7.0 versions)
22. https://raw.githubusercontent.com/torvalds/linux/master/kernel/rseq.c (also the v6.19, v7.0, v7.1 versions)
23. https://lwn.net/Articles/915817/
24. https://lkml.iu.edu/hypermail/linux/kernel/2512.0/00080.html
25. https://lkml.iu.edu/2511.2/08641.html
26. https://www.phoronix.com/news/Linux-7.0-Scheduler
27. https://lkml.iu.edu/2602.2/07723.html
28. https://lkml.iu.edu/hypermail/linux/kernel/2410.1/01689.html
29. https://sourceware.org/glibc/manual/latest/html_node/Restartable-Sequences.html
30. https://sourceware.org/glibc/manual/latest/html_node/POSIX-Thread-Tunables.html
31. https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/percpu.cc
32. https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/percpu.h
33. https://raw.githubusercontent.com/google/tcmalloc/master/docs/rseq.md
34. https://www.mongodb.com/docs/manual/administration/tcmalloc-performance/
35. https://lkml.iu.edu/hypermail/linux/kernel/2002.3/03683.html
36. https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/system_allocator.h
37. https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/parameters.cc
38. https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/background.cc
39. https://raw.githubusercontent.com/google/tcmalloc/master/docs/tuning.md
40. https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/malloc_extension.h
41. https://docs.kernel.org/accounting/psi.html
42. https://lkml.rescloud.iu.edu/2304.0/05624.html
43. https://kernelnewbies.org/Linux_6.4
44. https://docs.kernel.org/admin-guide/cgroup-v2.html (also the v5.18, v5.19, v6.1 versions)
45. https://lwn.net/Articles/951695/ ; https://lkml.rescloud.iu.edu/2311.3/05457.html
46. https://docs.kernel.org/filesystems/proc.html
47. https://lore.kernel.org/all/20170224021218.GD9818@bbox/T/
48. https://lkml.iu.edu/hypermail/linux/kernel/1702.0/00007.html
49. https://github.com/golang/go/issues/42330
50. https://go.dev/doc/go1.12
51. https://go.googlesource.com/go/+/f7342596daa892400e91a407cac5843bc43dcdd0/doc/go1.16.html
52. https://github.com/golang/go/issues/61718
53. https://fuchsia.googlesource.com/third_party/go/+/ef6993f3270c95e1158b11d9fc1a047528b34da6
54. https://go.googlesource.com/go/+/44078a32288ee8890882cb1578160715324407ff
55. https://raw.githubusercontent.com/golang/go/master/src/runtime/mem_linux.go
56. https://raw.githubusercontent.com/golang/go/master/src/runtime/mgcscavenge.go
57. https://openjdk.org/jeps/346
58. https://openjdk.org/jeps/351
59. https://raw.githubusercontent.com/microsoft/mimalloc/main/src/options.c
60. https://raw.githubusercontent.com/microsoft/mimalloc/main/src/prim/unix/prim.c
61. https://docs.kernel.org/admin-guide/sysctl/vm.html
62. https://raw.githubusercontent.com/torvalds/linux/master/include/linux/mm.h
63. https://fedoraproject.org/wiki/Changes/IncreaseVmMaxMapCount
64. https://gamingonlinux.com/2024/04/arch-linux-changes-vmmax-map-count-to-match-fedora-ubuntu-for-better-gaming
65. https://man7.org/linux/man-pages/man2/mmap.2.html
66. https://man7.org/linux/man-pages/man2/mremap.2.html
67. https://lwn.net/Articles/1015762/
68. https://lwn.net/Articles/1028877/
69. https://docs.kernel.org/mm/process_addrs.html
70. https://lwn.net/Articles/906852/
71. https://docs.kernel.org/arch/x86/tlb.html
72. https://lwn.net/Articles/1008065/
73. https://research.vmware.com/publications/don-t-shoot-down-tlb-shootdowns
74. https://docs.kernel.org/userspace-api/mseal.html
75. https://www.phoronix.com/news/Linux-6.10-Merges-mseal
76. https://lwn.net/Articles/1000908/
77. https://lkml.iu.edu/hypermail/linux/kernel/2410.2/11246.html
78. https://raw.githubusercontent.com/GrapheneOS/hardened_malloc/main/README.md
79. https://lkml.iu.edu/hypermail/linux/kernel/2407.1/08104.html
80. https://man7.org/linux/man-pages/man2/userfaultfd.2.html
81. https://man7.org/linux/man-pages/man2/memfd_create.2.html
82. https://arxiv.org/abs/1902.04738
83. https://raw.githubusercontent.com/plasma-umass/Mesh/master/src/meshable_arena.cc ; https://raw.githubusercontent.com/plasma-umass/Mesh/master/src/meshable_arena.h
84. https://man7.org/linux/man-pages/man2/set_mempolicy.2.html
85. https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html
86. https://docs.kernel.org/admin-guide/sysctl/kernel.html
87. https://raw.githubusercontent.com/torvalds/linux/v5.17/include/uapi/asm-generic/unistd.h ; https://raw.githubusercontent.com/torvalds/linux/v6.9/include/uapi/linux/mempolicy.h ; https://raw.githubusercontent.com/torvalds/linux/v5.18/include/linux/sched/sysctl.h ; https://raw.githubusercontent.com/torvalds/linux/v6.11/include/uapi/linux/mman.h ; https://raw.githubusercontent.com/torvalds/linux/v6.10/include/uapi/asm-generic/unistd.h
88. https://man7.org/linux/man-pages/man2/getcpu.2.html
89. https://man7.org/linux/man-pages/man2/pr_set_vma.2const.html
90. https://man7.org/linux/man-pages/man2/io_uring_enter.2.html
91. https://lkml.iu.edu/hypermail/linux/kernel/2203.0/05125.html
92. https://itnext.io/from-rss-to-wss-navigating-the-depths-of-kubernetes-memory-metrics-4d7d77d8fdcb
93. https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/uio.h
94. https://www.phoronix.com/news/Linux-6.19-Core-RSEQ-CID
95. https://sourceware.org/glibc/manual/latest/html_node/Memory-Allocation-Tunables.html
96. https://raw.githubusercontent.com/google/tcmalloc/master/docs/temeraire.md
97. https://lore.kernel.org/linux-kernel/20250410000022.1901-1-sj@kernel.org/T/
98. https://lwn.net/Articles/1017855/
99. https://lkml.iu.edu/hypermail/linux/kernel/2509.0/02433.html
100. https://www.kernel.org/doc/html/v7.1/userspace-api/rseq.html (compared with https://www.kernel.org/doc/html/v7.0/userspace-api/rseq.html)