# sohamalloc v2: performance- and locality-first design

*Date: 2026-10-18.*

How claims are marked:
- A URL after a claim is its source.
- **[derived]** marks my own arithmetic.
- **[est]** marks an estimate of instruction or cycle counts. None of these have been measured yet.
- **UNVERIFIED** marks anything I could not confirm from a primary source.

This document replaces v1 §4–§9 for the performance and locality angle. Every critical and major fault the critics raised that falls under this angle is addressed. The table in §0 maps each one.

---

## 0. What changed from v1 and why

| v1 fault (critic) | v2 resolution | § |
|---|---|---|
| No pointer→metadata lookup (critical) | A 2 MiB granule map plus arena-aligned descriptor arithmetic: one byte load, then one 64 B descriptor load | 3 |
| Front end and middle tier use incompatible object flows (critical) | A single **page-based** flow. Pages are the only unit of transfer. No object transfer caches | 5, 7 |
| mm_cid sold as novel; rseq protocol missing (critical) | mm_cid comes from TCMalloc's vCPU work (`RseqVcpuMode::kMM`, [percpu.cc](https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/percpu.cc); [kernel commit af7f588d8f73](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/patch/?id=af7f588d8f7355bc4298dd1962d7826358fc95f0)). v2 specifies a single-commit-store protocol, a lease and a fence | 5–7, 9 |
| Hard-coded 4 KiB / 2 MiB geometry (critical) | All geometry is read at runtime. Packing unit H = min(PMD, 2 MiB) | 2 |
| 64 GiB RW reservation fails under overcommit 2 / RLIMIT_AS (critical) | Growable 64 MiB arenas. The reservation mode is chosen per overcommit mode | 11 |
| Predictor lookup on the fast path; `__builtin_return_address(0)` collapses inside wrappers (critical) | **No per-object prediction on the small fast path.** Zero-cost proxies and compile-time hints instead. Stack-keyed prediction only for runs ≥ 256 KiB, counterfactual first | 10.4 |
| "4 classes per doubling = 12.5% waste" (major) | Explicit 64-class table with worst-case waste per band | 4 |
| mremap inside the reservation; VMA budget (major) | mremap only for *solo* VMAs. VMA budget set per feature | 10, 11 |
| THP fights the kernel (major) | Policy keyed on sysfs mode, defrag, max_ptes_none and shrink_underused. `MADV_HUGEPAGE` applied once per arena, never toggled | 11.3 |
| No idle release without a thread (major) | Lazily created maintenance thread (never in single-threaded processes), plus an idle check on slow paths | 12 |
| PSI read from /proc/pressure (major) | The process's own cgroup files, read with raw syscalls and O_CLOEXEC | 12 |
| Fork, signals, TLS, API semantics unspecified (major) | §9 and §15 | 9, 15 |
| Double-free bitmap duplicates the free list; "metadata out of line" contradicted (major) | S0 has no bitmap. Inline links are explicitly in the threat model, encoded and validated on pop | 14 |
| Fullest-first conflicts with locality; Temeraire misdescribed (major) | Page refill takes the fullest page that still has ≥1/8 free. The hugeblock filler uses Temeraire's real order | 7.4, 10.1 |

---

## 1. Thesis and the numbers it has to beat

**What we optimise.** The target is application throughput, not malloc nanoseconds.

ASPLOS'24 puts malloc at 4.3% of fleet cycles. Of those cycles, 53% are in the per-CPU cache and 16% in prefetching, and a heap layout aware of hugepages and NUCA moved fleet throughput by +1.4% ([ASPLOS'24](https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf)). Temeraire, a hugepage-aware page heap, gave +7.7% requests per second across 8 applications. One service that spent *more* time in malloc still gained throughput ([OSDI'21](https://www.usenix.org/system/files/osdi21-hunter.pdf)).

So the allocator's real lever is where blocks land: cache lines, TLB reach and NUCA domain. A few instructions on the fast path matter less. Even so, the fast path must stay in the same class as mimalloc's. The mimalloc source describes its fast path as "about 7 to 10 instructions with a single test" ([alloc.c](https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/alloc.c)).

**Design principles:**
1. **LIFO reuse inside a page.** The block freed last is handed out next, so it is still warm in L1/L2.
2. **Page-local free lists (sharding).** Consecutive allocations stay within a few lines and one base page.
3. **Pages packed densest-first into 2 MiB hugeblocks.** Freed memory is released as whole hugeblocks.
4. **Caches indexed by the kernel's concurrency ID (mm_cid), falling back to TLS.** Memory scales with *running* threads, and a slot outlives the threads that use it. That fixes the thread-pool-churn stranding in mimalloc #351 ([issue](https://github.com/microsoft/mimalloc/issues/351)).
5. **No atomics on the fast path, including remote frees.** Remote frees are buffered and handed over with one CAS per destination page, in the style of BatchIt ([preprint](https://microsoft.com/en-us/research/uploads/prodnew/2024/05/preprint_batchit.pdf)).
6. **Every rare condition is folded into a branch the fast path already takes.** Sentinel encodings, flag bits in the owner word, and a single countdown.

---

## 2. Runtime geometry

All geometry is computed once at init and stored in a read-mostly global line. No page-size constant is compiled in (the Asahi breakage list shows why: [asahilinux](https://asahilinux.org/docs/sw/broken-software/)).

| Symbol | Value | Source |
|---|---|---|
| B (base page) | `getauxval(AT_PAGESZ)`: 4K, 16K or 64K | |
| PMD | `/sys/kernel/mm/transparent_hugepage/hpage_pmd_size`, falling back to B | [transhuge](https://docs.kernel.org/admin-guide/mm/transhuge.html) |
| **H (hugeblock)** | PMD if PMD ≤ 2 MiB, else 2 MiB. On arm64 16K/64K granules, 2 MiB is the contiguous-PTE size | [arm64 hugetlbpage](https://docs.kernel.org/arch/arm64/hugetlbpage.html) |
| Slice | 64 KiB = max(64 KiB, B) | |
| Small page | 1 slice (64 KiB) | |
| Medium page | 512 KiB, 1 MiB or 2 MiB, chosen per class (§4). Never crosses an H boundary | |
| **Arena** | 64 MiB, aligned to 64 MiB = 32 hugeblocks = 1024 slices | |
| Solo threshold | ≥ 32 MiB, or alignment > H | |

**Why 64 MiB arenas** rather than mimalloc's 256 MiB chunk ([page-map.h](https://github.com/microsoft/mimalloc/blob/dev3/include/mimalloc/heap/page-map.h)) or a 1 GiB region:
- A single 64 MiB request passes overcommit mode 0's "pages > RAM + swap" refusal on any realistic host ([mm/util.c](https://github.com/torvalds/linux/blob/master/mm/util.c)).
- It grows in fine steps under RLIMIT_AS.
- It is 1/8192 of a 39-bit arm64 address space ([arm64 memory](https://docs.kernel.org/arch/arm64/memory.html)).

**Cost:** more arenas for large heaps. Contiguous placement keeps them merged into a few VMAs (§11.1).

**Arena layout (64 MiB):**

```
slice 0..2  (192 KiB) header:
   +0       arena hdr (4 KiB): magic, node, epoch, hugeblock bitmaps, commit HWM
   +4K      hugeblock meta[32] x 192 B
   +64K     hot descriptors[1024] x 64 B   (owner-written lines)
   +128K    remote descriptors[1024] x 64 B (cross-thread-written lines)
   last B   guard (MADV_GUARD_INSTALL; else none, see 14)
slice 3..1023 data
```

**Overhead [derived].** Metadata is 128 B per 64 KiB slice, i.e. 0.2%. Only descriptors for slices actually in use become resident. Hot and remote descriptors sit in **separate arrays**. A remote CAS therefore never invalidates the line the owner reads on every pop, and an adjacent-line prefetcher cannot pair the two lines either.

---

## 3. Pointer → metadata lookup (the hottest lookup)

**Granule map (gmap).**
- One byte per 64 MiB of user address space below 2^47: 2^21 entries = 2 MiB, reserved with `MAP_NORESERVE`. Touched pages: in practice one 4 KiB page covers 256 GiB.
- Entry values: `0` = foreign, `1` = arena, `2` = solo head, `3` = solo body or other.
- The kernel does not place mappings above 47 bits without a hint ([5-level paging](https://docs.kernel.org/arch/x86/x86_64/5level-paging.html); [arm64 52-bit opt-in](https://docs.kernel.org/arch/arm64/memory.html)). We never pass such a hint, so higher pointers are foreign by construction.
- Before init, `gmap` points at a static zero page, so `free(NULL)` and early frees of foreign pointers work with no init check. This is snmalloc's static-default-entry trick ([pagemap.h](https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds/pagemap.h)), fixing the mimalloc #1341 class of bug ([issue](https://github.com/microsoft/mimalloc/issues/1341)).

```
lookup(p):                       // [est] 7 instr, 1 byte load + 1 line load
  a  = p & ADDR_MASK             // strip arm64 TBI / MTE tag (top byte); x86 LAM same mask
  g  = a >> 26
  if (g >= 1<<21) -> foreign     // also catches >47-bit
  if (gmap[g] != 1) -> nonarena  // solo / foreign / NULL (gmap[0]==0)
  base = a & ~(64MiB-1)
  d   = base + HOT_OFF + ((a >> 16) & 1023) * 64
```

**Latency.** `gmap[g]` is a single L1-resident byte. Its branch predicts well, and the descriptor load issues speculatively in parallel with it. The descriptor address does not depend on the gmap value, so the check adds no dependent latency in the common case [est].

**Interior pointers.** `free` never receives one:
- Aligned small allocations go to a class whose size is a multiple of the alignment, and pages are 64 KiB-aligned, so blocks are naturally aligned.
- Aligned runs are trimmed so the run starts at the returned pointer.
- Solo regions handle alignments larger than H.

Medium pages span several slices. Their non-head slices carry a TAIL flag in the owner word, which sends them to the generic path, where the head is `d − delta·64`.

**Foreign-pointer policy:**
- **Preload/default builds:** abort with a diagnostic in S1+. In S0, write one log line and ignore the pointer, mirroring how glibc's own `free` handles invalid pointers.
- **Non-preload library builds:** forward to `dlsym(RTLD_NEXT,"free")`, resolved at init outside any lock.

**Alternatives considered:**
- mimalloc's flat byte-per-slice map. It needs a 4 GiB VA reserve at 48 bits ([page-map.h](https://github.com/microsoft/mimalloc/blob/dev3/include/mimalloc/heap/page-map.h)) and a second dependent load for the page pointer.
- TCMalloc's 3-level radix tree ([pagemap.h](https://github.com/google/tcmalloc/blob/master/tcmalloc/pagemap.h)): 3 dependent loads.
- Rejected both. Ours costs 1 byte load plus 1 descriptor load.

**Risk:** a stray write corrupting an arena header. Mitigated by the guard page and by validating the magic in S1.

**Experiment:** run `free` microbench (local, remote, foreign) against mimalloc dev3 and TCMalloc on Zen 4 and Neoverse V2. Gate: ≤ 1 cycle median penalty versus mimalloc's arithmetic lookup.

---

## 4. Size classes and page kinds

There are 64 classes; `cls` fits in 6 bits.

| Band | Classes | Step | Worst waste (fraction of block) [derived] |
|---|---|---|---|
| 16–128 B | 8 | 16 B | 46.9% at 17→32 (forced by 16 B alignment) |
| 129 B–1 KiB | 24 | 8 per doubling (16/32/64 B steps) | ≈11.1% (e.g. 257→288 is 10.8%) |
| 1–8 KiB | 12 | 4 per doubling | ≈20% (1025→1280 is 19.9%) |
| 8–256 KiB | 20 | 4 per doubling | ≈20% |

The jemalloc manual confirms that four classes per doubling cap waste at "approximately 20%" ([jemalloc.xml.in](https://raw.githubusercontent.com/jemalloc/jemalloc/dev/doc/jemalloc.xml.in)). v1's 12.5% figure was wrong. The 129 B–1 KiB band is denser because objects under 1 KB are 98% of allocations ([ASPLOS'24](https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf)).

**Lookup:**
- For n ≤ 1024: `cls = lut[(n+15)>>4]`, a 65-byte table.
- Above that: `lg = 63−clz(n−1); cls = 32 + (lg−10)·4 + ((n−1)>>(lg−2) & 3)`, about 6 instructions [est].

**Page kinds:**
- **Small (≤ 8 KiB): 64 KiB pages.** Tail waste is ≤ 6.25% for every class; the worst cases are 5120 and 6144 B, each wasting 4 KiB [derived]. Each page holds ≥ 8 objects.
- **Medium (8–256 KiB):** the page size is generated per class from {512 KiB, 1 MiB, 2 MiB}. The generator minimises tail waste subject to ≤ 12.5% waste and ≥ 4 objects per page. Example: 160 KiB on a 1 MiB page fits 6 objects and wastes 6.25% [derived].
- The table and its waste report are generated offline and shipped as a header, as the critics asked.

**Lifecycle of the table:**
- The table above is the *bootstrap*.
- Phase 0.5 regenerates it from request-size histograms recorded with our trace recorder on Redis, RocksDB, clang, CPython and Envoy. TCMalloc does the same with fleet-annotated classes ([size_classes.cc](https://github.com/google/tcmalloc/blob/master/tcmalloc/size_classes.cc)).
- Objective function: rounding waste + tail waste + λ × (expected partially filled pages per class per active slot).
- Property tests: the mapping is monotone, and every size in [requested, usable] maps back to the same class, which sized-delete correctness requires.

**Alternative:** 8 classes per doubling everywhere, giving ~11% worst case. Rejected for > 1 KiB, because it doubles the partially filled pages per slot, and RSS is a goal. **Experiment:** compare RSS-time integral against throughput for 4 and 8 classes per doubling in the 1–8 KiB band on the trace set.

---

## 5. Front end: slots

### 5.1 One abstraction, two binding modes

A **slot** is the unit that owns pages. Fast paths operate on `slot*`, and the slot can be bound in one of three ways:

- **rseq-cid mode (default when available).**
  - `slot = slots + (rseq.mm_cid & 8191)·4 KiB`.
  - Every slot access happens inside an rseq critical section (CS). A CS is aborted on preemption, migration or signal delivery ([rseq docs](https://docs.kernel.org/userspace-api/rseq.html)).
  - mm_cid is unique among the mm's running threads ([uapi rseq.h](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/rseq.h)).
  - Since the 6.19-cycle rewrite, a CID is owned per task while tasks ≤ allowed CPUs, and per CPU above that ([cover letter](https://lkml.iu.edu/2511.2/08641.html)). The current bound is `max_cids = min(1.25·min(nr_cpus_allowed, users), num_possible_cpus)` ([kernel/sched/core.c](https://github.com/torvalds/linux/blob/master/kernel/sched/core.c)).
  - Effect: with few threads, a slot behaves like a per-thread cache. With many threads it behaves like a per-CPU cache. A thread that exits leaves its warm slot for the next thread that gets the CID.
- **tls-exclusive mode.** `slot = tls->slot`. Each thread owns its slot, so a CS is just a plain store. Used when rseq is unavailable.
- **tls-shared mode.** Once exclusive slots exceed 2 × allowed CPUs, new threads bind to a shared slot protected by a per-slot trylock, like Scudo's shared TSD pool ([tsd_shared.h](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/scudo/standalone/tsd_shared.h)). This bounds memory with 10k threads.

### 5.2 rseq detection, with no ABI overlays

- `__rseq_offset` and `__rseq_size` are referenced **weakly**, so the library loads on glibc < 2.35.
- Effective size calculation:
  - If `__rseq_size ∈ {20, 32}` (glibc 2.35–2.40 quirk): `eff = min(32, getauxval(AT_RSEQ_FEATURE_SIZE))`.
  - Otherwise `eff = __rseq_size`.
  - Sources: [LKML 2602.2/07723](https://lkml.iu.edu/2602.2/07723.html), [LKML 2410.1/01689](https://lkml.iu.edu/hypermail/linux/kernel/2410.1/01689.html), [glibc manual](https://sourceware.org/glibc/manual/latest/html_node/Restartable-Sequences.html).
- mm_cid is usable iff `eff ≥ 28` (`offsetofend(mm_cid)`); node_id iff `eff ≥ 24`.
- If `__rseq_size == 0` (registration disabled, or musl), we self-register a static-TLS area of `max(AT_RSEQ_FEATURE_SIZE, 32)` bytes, suitably aligned ([uapi rseq.h](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/rseq.h)). On `EBUSY` we fall back to TLS mode.
- We **only read** `cpu_id`, `node_id` and `mm_cid`, and only write `rseq_cs`. We never overlay `cpu_id_start`, which broke TCMalloc on 6.19 ([LWN](https://lwn.net/Articles/1070072/)). Under 7.1 "V2", writing kernel-owned fields kills the process ([rseq docs](https://docs.kernel.org/userspace-api/rseq.html)).
- rseq-cid mode **also requires** `membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ)`, available since Linux 5.10 ([membarrier(2)](https://man7.org/linux/man-pages/man2/membarrier.2.html)). It is needed for idle-slot reclaim (§9.2). Without it we use TLS mode.
- **UNVERIFIED:** that glibc registers with the standard per-architecture `RSEQ_SIG`. Our abort handlers must be preceded by the signature the registrant chose. Init therefore runs a self-test CS that forces an abort.

### 5.3 Slot layout (4 KiB, lazily committed)

```
line0:  u32 lease; u32 flags; u64 rbuf_enc; u64 owned_prov_bytes;
        u64 last_slow_ns; u8 large_n; ...
bins[64]   x 16 B : { u64 cur_enc; u64 alt_enc }      // 1 KiB
tbins[64]  x 16 B : pointer-free / cold partition     // only if tokens/hints seen
large[4]   x 8 B
```

The slot array reserves VA for 8192 slots (32 MiB, `MAP_NORESERVE`). It is sized from the possible-CPU count when RLIMIT_AS is tight. Accesses are masked with `& 8191` as a defence against the out-of-bounds CPU IDs seen in TCMalloc #188 ([issue](https://github.com/google/tcmalloc/issues/188)).

**Zero-is-sentinel encoding.** `cur_enc` stores `page ^ &SENTINEL_PAGE`, so an untouched (zero) slot decodes to a static sentinel page with `free == 0`. That sends the first malloc to refill with no "slot initialised?" branch. Cost: 1 XOR with a RIP-relative constant [est]. `rbuf_enc` works the same way, pointing at a sentinel buffer that is always full.

### 5.4 Per-thread TLS (initial-exec, ≤ 48 B)

```
i64 countdown; slot* slot (tls modes); u32 depth; u32 state; u64 seed
```

This stays small to fit glibc's 512-byte optional static TLS when we are dlopen'd late ([tunables](https://sourceware.org/glibc/manual/latest/html_node/POSIX-Thread-Tunables.html)). Initial-exec TLS is required for replacement allocators ([Replacing malloc](https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html)).

**Alternatives considered:**
- (a) Pure per-thread heaps (mimalloc). Simplest, but they strand memory under thread churn ([#351](https://github.com/microsoft/mimalloc/issues/351)) and scale with the thread count.
- (b) TCMalloc per-CPU pointer arrays: ownership-free and atomic-free. But objects migrate between pages, which forces transfer caches and destroys page coherence. That is the incoherence v1 had.
- (c) **Chosen:** a page-owning slot, rseq-indexed.

**Cost:** about 3–4 extra instructions per operation for rseq_cs setup and clear [est], plus hand-written asm for 5 CS kinds per architecture.

**Risk:** rseq ABI churn (6.19, 7.1); a wrong RSEQ_SIG crashes the process.

**Experiment:** run (4 threads, 256 CPUs), (10k threads, 64 CPUs) and (cpuset of 2) in rseq-cid, tls-exclusive and tls-shared modes, on kernels 6.6, 6.12, 6.18, 6.19 and 7.1. Report throughput, RSS and abort rate (`perf` rseq events, or a counter in abort handlers).

---

## 6. Fast paths

### 6.1 Descriptors

**Hot descriptor (64 B, written only from the owning domain):**

```
+0  u64 free     // [63:48] count of blocks on list, [47:0] head (raw)
+8  u32 owner    // cid+1 | flags<<16 (TAIL, SAMPLED, GUARDED, CLAIMING, LARGE, FULLCHK)
+12 u16 prov     // blocks provisioned so far
+14 u16 cap
+16 u64 key      // per-page random, set at page (re)init
+24 u32 bsize; u32 recip
+32 u8 cls, kind, node, llc; u32 epoch
+40 ...           // provisioning cursor, zero-fresh flag, alt link
```

**Remote descriptor (64 B):** `atomic u64 xfree` (bits [63:48] count, [47:4] head, bit 1 QUEUED, bit 0 ORPHAN), pool linkage, and claim epoch.

Packing the count with the head lets the pop and the push each commit **with one store**, which keeps rseq correctness intact. The count gives exact "page fully free" detection with no second counter. TBI/MTE tags are stripped before storing.

**Encoded link** stored in a free block: `enc(next, at) = bswap(next) ^ key ^ (at >> 12)`. Byte-swapping makes partially overwritten links non-canonical (the PartitionAlloc idea, [encoded_next_freelist.h](https://github.com/chromium/chromium/blob/main/base/allocator/partition_allocator/src/partition_alloc/encoded_next_freelist.h)). The address term binds a link to its location, as glibc does ([malloc.c](https://sourceware.org/git/?p=glibc.git;a=blob;f=malloc/malloc.c;hb=HEAD)). The per-page key means one leak does not decode every page.

### 6.2 malloc (n ≤ 1024), rseq-cid mode

```
c   = lut[(n+15)>>4]
t   = tls; t->countdown -= n; if (t->countdown < 0) goto event   // sampling + heartbeat
CS_BEGIN(cs_pop)                               // store &cs_pop -> rseq->rseq_cs
  cid  = rseq->mm_cid & 8191
  s    = SLOTS + cid*4096
  if (s->lease) goto leased                    // same line as bins? no: line0, L1-hot
  pg   = s->bins[c].cur_enc ^ SENTINEL
  h    = pg->free
  b    = h & PTR48; if (!b) goto refill
  nx   = bswap(*b ^ pg->key ^ (b>>12))         // decode
  if ((nx ^ b) >> 16 && nx) goto corrupt       // same 64 KiB page or NULL
  pg->free = nx | ((h - CNT1) & CNTMASK)       // COMMIT (single store)
CS_END                                         // clear rseq_cs
prefetchw(nx)                                  // next pop target; TCMalloc prefetches too
return b
```

- **Estimate:** ~26 instructions, 4 dependent L1 loads (cid → bin → page→free → *b), so roughly 18–22 cycles of latency, overlappable [est]. That is 2–3× mimalloc's instruction count. The bet is that the locality wins outweigh it, so the gate in §6.5 is application-level.
- **TLS mode:** drops the CS setup and clear, the cid load and the lease check, giving ~20 instructions [est].
- The `countdown` store happens *before* the CS and is only statistical. If a CS aborts after it, sampling is slightly perturbed, which is harmless. The single-commit rule applies only to allocator state.
- The **sized path** (n known at compile time, or `operator new` with a constant) skips nothing beyond `lut`. Sizes are used for validation, not speed (§15).

### 6.3 free, rseq-cid mode

```
d = lookup(p)                                  // §3, ~7 instr
if (p & 15) goto bad                            // S0 cheap check
CS_BEGIN(cs_push)
  cid = rseq->mm_cid & 8191
  if (d->owner != cid+1) goto not_local        // flags or other owner -> generic/remote
  h = d->free
  if ((h & PTR48) == p) goto dfree             // trivial double free
  *p = enc(h & PTR48, p)
  d->free = p | ((h + CNT1) & CNTMASK)         // COMMIT
CS_END
```

Estimate: ~24 instructions, 2 dependent descriptor loads, no atomics [est]. The lease is not checked on the push: a lease reclaimer first sets `owner = CLAIMING` from inside the domain (§7.1), so pushes divert on their own.

**not_local** (remote, or flagged):

```
if (flags) goto generic                        // TAIL / SAMPLED / LARGE ...
if (d->bsize > 1024) goto remote_direct        // bigger objects: 1 CAS each
CS_BEGIN(cs_rbuf)
  s = SLOTS + cid*4096; rb = s->rbuf_enc ^ SENTINEL_RB
  n = rb->n; if (n == 64) goto flush           // sentinel has n==64
  rb->p[n] = p; rb->n = n+1                    // COMMIT (the n store)
CS_END
```

Estimate: ~20 instructions, no atomics [est]. Buffered remote bytes are bounded at ≤ 64 × 1 KiB = 64 KiB per slot.

### 6.4 The countdown event (one branch, many jobs)

When `countdown < 0`, which happens every exponentially distributed ~2 MiB allocated per thread:
- take a heap-profile sample (TCMalloc's mean of 2 MiB, [tcmalloc.cc](https://github.com/google/tcmalloc/blob/master/tcmalloc/tcmalloc.cc));
- place a guarded sample (§14);
- run the slot sweep (§8.2);
- run the idle and pressure check (§12);
- do lazy per-thread init (a new thread's countdown starts at 0).

This copies jemalloc's single-threshold fold ([inlines_c.h](https://github.com/jemalloc/jemalloc/blob/dev/include/jemalloc/internal/jemalloc_internal_inlines_c.h)).

### 6.5 Gates

**Microbenchmarks:**
- Median malloc+free pair ≤ 1.3× mimalloc v3.4.x's.
- Single-threaded larson/glibc-thread within 5%.

**Application gate (the one that matters):**
- Redis (MALLOC=libc build), RocksDB db_bench, clang self-build, Envoy and fleetbench.
- Throughput geometric mean ≥ mimalloc, with 95% bootstrap CI over ≥ 10 runs.
- Plus dTLB walk cycles and LLC misses measured with `perf stat -d -d`.

---

## 7. Page ownership, remote frees, pools

### 7.1 State machine and invariants

States: OWNED(s) · OWNED-CLAIMING(s) · UNOWNED-EXCLUSIVE · ORPHAN · QUEUED.

**Invariants:**
- **I1.** `owner == s+1` implies the page is referenced by slot s's `cur` or `alt`.
- **I2.** `page->free` is written only (a) inside a CS in domain s while s references the page, or (b) by a thread holding the page exclusively while it is unreferenced and `owner ∈ {0, CLAIMING}`.
- **I3.** Every multi-step transition is a sequence of single-commit CSes. Every intermediate state is valid and **resumable by any later thread on slot s**: whoever next runs refill on that slot finishes the transition.

**Detach** (the current page ran out after collecting xfree):
1. CS commit `owner = CLAIMING|s`. Pops still work; pushes from slot s now take the remote path.
2. CS commit `cur = replacement`.
3. The page is now unreferenced and exclusive to us. Set `owner = 0` and atomically OR ORPHAN into xfree, then link it onto the node's orphan list (under lock; this is a slow path).
- If the CID changes between steps 1 and 2, we stop. The next refill on slot s sees `cur` with owner CLAIMING and an empty list, and resumes.

**Claim** (refill from pool):
1. Take the page from the pool under lock.
2. Merge `xchg(xfree, 0)` into `free`, and set the provisioning cursor.
3. Set `owner = CLAIMING`.
4. CS commit `bins[c].cur = pg`.
5. CS (on the same CID) commit `owner = cid+1`, iff `bins[c].cur == pg`.
- If the CID changed before step 5, the page stays allocatable on its slot with frees going remote. The next refill there finalises the owner.

### 7.2 Remote free and flush

**Flush:**
- Pop a spare buffer from the per-LLC buffer pool (tagged-pointer stack).
- CS commit `s->rbuf_enc = spare`. The old buffer is now exclusively ours.
- Leave the CS. Sort the 64 pointers by descriptor address and build one chain per destination page.
- For each page: one `CAS(xfree, {head, cnt+k})`.
- Push the old buffer back to the pool.

**Cost [est]:**
- per object: ~20 fast instructions plus ~8–15 amortised cycles for the sort and chaining;
- per destination page: one CAS;
- per flush: 2 atomics on the pool.

**Compared with others:**
- mimalloc pays one CAS per object ([free.c](https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/free.c)).
- BatchIt measured −9.9% to −27.6% time on xmalloc-test, but also regressions of up to 8% elsewhere ([BatchIt](https://microsoft.com/en-us/research/uploads/prodnew/2024/05/preprint_batchit.pdf)). Our design has no mode switch: v1's adaptive message-passing switch is gone.

**Orphan and queue rule:**
- A remote CAS whose new count reaches ≥ cap/8 on a page with ORPHAN set (and QUEUED clear) also sets QUEUED and clears ORPHAN in the same CAS. That thread then moves the page from the orphan list to the class pool's bucket.
- If the count reaches cap (fully free) while QUEUED, that thread removes the page and returns it to the page heap.
- The 7/8 threshold matches mimalloc's re-abandon rule ([free.c](https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/free.c)).

### 7.3 Collection

The owner collects xfree:
- when `free` is empty (refill);
- on sweep, for `cur` and `alt`.

The list is walked once to count it and merged as a single chain via a CS, which writes the tail link before the commit.

### 7.4 Pools

Each (LLC domain, class) pool is protected by one mutex and has 3 buckets by free fraction: [1/8, 1/4), [1/4, 1/2), [1/2, 1).

**Refill order:**
1. Own `alt`.
2. Same-LLC pool, fullest non-empty bucket.
3. Same-node pools of other LLCs.
4. Orphan list: claim pages with count > 0, scanning at most 8.
5. Fresh page from the page heap.

Taking the fullest page *with ≥ 1/8 free* gives a refill a reasonable run of free blocks rather than scattered holes, which answers the design critic's locality objection. Because pages are packed densely, it still raises occupancy, which is TCMalloc's span-prioritisation result: −1.41% fleet memory ([ASPLOS'24](https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf)).

**Experiment:** compare FIFO, LIFO, fullest-first and fullest-with-≥1/8 by LLC misses, dTLB misses and RSS on the trace set.

---

## 8. Slow paths

### 8.1 Refill (malloc found an empty `cur`)

1. Collect xfree on `cur`. If anything is found, retry the pop.
2. If `prov < cap`, provision the next base page's worth of blocks. The list is built in address order, so allocation walks forward through lines. Untouched memory is never dirtied ([PartitionAlloc](https://github.com/chromium/chromium/blob/main/base/allocator/partition_allocator/PartitionAlloc.md)).
3. Swap in `alt` if it has free blocks.
4. Detach `cur` (§7.1) and claim from pools (§7.4).
5. Update `owned_prov_bytes`. If it is over budget (§12), detach the least-recently-refilled class's pages.

Estimate: 150–600 cycles without locks, ~1–3 µs with a pool lock [est].

### 8.2 Sweep (on a countdown event, at most once per 10 ms per slot)

- Scan `cur`/`alt` of all 64 bins: 128 descriptor reads [est ~300 cycles].
- Detach pages that are fully free and were not refilled since the last sweep.
- Detach medium `cur` pages unused for one sweep interval.
- Flush the remote buffer if it is older than 50 ms.

---

## 9. Lifecycle, fork, signals, bootstrap

### 9.1 Bootstrap

- Zero-filled globals are a valid "uninitialised" state: sentinel slot, sentinel page, gmap pointing at the static zero page, countdown 0.
- The first malloc goes to the event path, which runs `init()` under a once-flag with a recursion guard.
- During init, raw `open/read/close` with O_CLOEXEC replace `fopen`/`opendir`, which allocate ([Replacing malloc](https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html)).
- Init steps:
  1. geometry;
  2. overcommit mode;
  3. RLIMIT_AS;
  4. THP sysfs;
  5. rseq probe;
  6. membarrier registration;
  7. first arena;
  8. `pthread_atfork` registration (at first init, so prepare runs last).
- cgroup, topology and PSI discovery are **deferred** to the first event after the heap exceeds 8 MiB, which keeps `/bin/true` cheap.

### 9.2 Threads and idle slots

**Thread exit:**
- *rseq-cid mode:* nothing slot-related. The thread's TLS is marked ZOMBIE, so late frees after TLS teardown take `remote_direct`, with no buffer. jemalloc 5.4.0 had to fix exactly this class of bug ([ChangeLog](https://raw.githubusercontent.com/jemalloc/jemalloc/dev/ChangeLog)).
- *TLS modes:* flush the remote buffer, detach all pages, and release the slot for reuse.

**Idle slot reclaim (rseq-cid mode).** A slot is reclaimed when its `last_slow_ns` is older than 1 s and `owned_prov_bytes` is above 256 KiB (or there is pressure). The reclaimer, either the maintenance thread or any slow path at most once per second:
1. `CAS(slot->lease, 0, ME)`;
2. `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, 0, 0)`, which restarts any in-flight CS. After it returns, every CS on that slot sees the lease, and malloc/remote paths divert to direct pool paths;
3. detach every page, drain the buffer, reset bins to the sentinel;
4. clear the lease.

The fence must interrupt every CPU running the process, because a vCPU-indexed slot cannot be mapped to a physical core. TCMalloc says the same in `FenceCpu` ([percpu.cc](https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/percpu.cc)). It is therefore rate-limited to ≤ 1/s and counted in stats. The lease check costs 2 instructions on the malloc and remote-buffer paths [est].

### 9.3 Fork

**prepare:**
- set `g_forking`;
- take every page-heap shard lock and pool lock in a fixed order;
- wait until the per-slot `inflight` counters sum to 0. These counters are incremented around orphan-claim+enqueue and around flush processing, the only lock-free multi-step operations. This is the snmalloc `PreventFork` idea ([prevent_fork.h](https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds_aal/prevent_fork.h)).

rseq CS state is fork-safe by construction: a single commit is either in the copied image or not.

**child:**
- reinitialise all locks;
- mark every slot other than its own as idle-reclaimable. A single-threaded child needs no fence; the reclaim happens lazily on the first event;
- new pages get keys from a fresh secret;
- the maintenance thread and cgroup fds are recreated lazily.

**Test:** fork from 64 threads in the middle of free; the child must neither leak nor deadlock.

### 9.4 Signals and reentrancy

- POSIX does not require malloc to be async-signal-safe ([signal-safety(7)](https://man7.org/linux/man-pages/man7/signal-safety.7.html)), but we guarantee no corruption.
- In rseq mode, signal delivery aborts the interrupted CS ([rseq docs](https://docs.kernel.org/userspace-api/rseq.html)), so a handler's malloc is safe on the fast path.
- Slow paths raise `tls->depth`. A nested call with `depth > 0` uses an emergency lock-free bump arena (1 MiB, atomic add); its frees are deferred.
- **Residual hole:** in TLS-exclusive mode, the fast path's plain stores are *not* safe against a handler that mallocs between the load and the store. This is documented and listed in §20.
- The explain report is produced only by API, at exit (`SOHAMALLOC_REPORT=fd:N`), or by an opt-in signal whose handler sets a flag. The report code itself does not allocate.

---

## 10. Page heap, large and huge allocations

### 10.1 Page heap (one shard per NUMA node, one mutex each)

**Hugeblock filler** for pages and runs smaller than H:
- Pick the hugeblock with the *shortest longest-free-range* that fits.
- Break ties by *most allocations*.
- Rank freshly donated tails last.
- This is TCMalloc's real order. TCMalloc notes that a simpler order caused bad page-heap fragmentation, and that allocation count beat allocated pages ([huge_page_filler.h](https://github.com/google/tcmalloc/blob/master/tcmalloc/huge_page_filler.h)).
- Implementation: a bitmap index over (quantised longest free range × quantised allocation count) gives an O(1) find with `tzcnt`.

**Density split:** pages from classes with ≤ 16 objects per page (medium pages and runs) go to *sparse* hugeblocks; small pages go to *dense* ones. This is the span-capacity lifetime proxy (+1.02% throughput, −0.82% memory, [ASPLOS'24](https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf)), and it costs nothing at runtime.

**Hint partitions:** cold-hinted and pointer-free allocations (§15) get their own hugeblock sets.

**Whole hugeblocks:** taken from an address-ordered first fit across the node's arenas. This keeps the heap compact at low addresses, which strict-overcommit decommit needs (§11.1) and which matches First-Fit's O(log M) competitive behaviour ([arXiv 2608.28462](https://arxiv.org/abs/2608.28462)).

**Hugeblock meta (192 B):**
- used bitmap: 64 B at 4K, 4 B at 64K;
- released bitmap;
- zero-known bitmap;
- longest free range, allocation count, state, first-touch epoch, density class.

**Lock hold times** are short: no syscalls under the lock. On kernels ≥ 7.0 with a registration larger than 32 bytes (V2), shard locks request an rseq time-slice extension (`slice_ctrl.request`; 5 µs default) ([rseq docs](https://docs.kernel.org/userspace-api/rseq.html)).

**Experiment:** measure shard lock wait (exported in stats) on a 192-thread run of 256 KiB–2 MiB I/O-buffer churn.

### 10.2 Large runs (256 KiB < n < 32 MiB)

- A run is a page-aligned run of base pages, recorded in its head slice descriptor (kind LARGE).
- Per-slot large cache: 4 entries for runs ≤ 2 MiB, inside `owned_prov_bytes`. A CS commit on the count.
- Remote large frees go directly to the page-heap shard.
- Runs ≥ H are whole hugeblocks; slack below H is donated to the filler.
- **Experiment:** compare against Temeraire's HugeRegion idea ([regions doc](https://github.com/google/tcmalloc/blob/master/docs/regions-are-not-optional.md)). Add a region only if slack waste exceeds 2% of RSS on traces.

### 10.3 Solo regions (≥ 32 MiB, or alignment > H)

- A dedicated `mmap`, over-mapped and trimmed to 64 MiB alignment, with a header in the first base page.
- The gmap marks it SOLO head or body; a small locked hash table resolves body granules.
- `munmap` on free.
- These are the **only** objects realloc grows with `mremap(MREMAP_MAYMOVE)`. They own their VMA, so the critics' VMA/region contradiction disappears ([mremap(2)](https://man7.org/linux/man-pages/man2/mremap.2.html); moved anonymous VMAs do not merge back, [LWN](https://lwn.net/Articles/1015762/)).

### 10.4 Lifetime prediction (deliberately narrow)

- There is **no small-object prediction**. LLAMA's cached lookup cost 22.5 ns against an 8.3 ns TCMalloc fast path ([LLAMA](https://cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf)). A return-address key collapses behind libstdc++ `_M_create`, Rust's `System` allocator and CPython's obmalloc arenas, as the design critic showed.
- Small objects get only the zero-cost density split, plus explicit hints (§15).
- **Large runs only:** on sampled runs, a key of hash(return address, stack depth, size class) is walked with a bounds-checked frame-pointer walk, *only on the sampled path*.
  - The predictor uses binary SHORT/LONG with T = 0.5 s, a sorted tracker list for survivors (handling right-censoring), and an LRU-bounded database.
  - It ships in **counterfactual mode** (measure, do not act), as TCMalloc's design does ([lifetime-based-allocator.md](https://github.com/google/tcmalloc/blob/master/docs/lifetime-based-allocator.md)). It is enabled only after a counterfactual win on traces.
  - Prior art: Barrett & Zorn (PLDI'93), LLAMA, TCMalloc.
  - TCMalloc HEAD appears to have no such code any more (**UNVERIFIED**), which is a warning sign.

### 10.5 calloc, realloc, aligned allocation

**calloc:** `__builtin_mul_overflow` and a PTRDIFF_MAX cap. Zeroing is skipped when the memory is known to be zero:
- for small blocks: from a page whose memory came from never-touched or DONTNEED'd base pages (`zero_fresh` flag) and lies above the provisioning high-water mark;
- for runs: from the zero-known bitmap.
- Otherwise libc `memset`. mimalloc #1104 shows `rep stosb` is a hazard at ≥ 128 B on some CPUs ([issue](https://github.com/microsoft/mimalloc/issues/1104)).
- Zero-known bits are set only after the release *succeeds*. They are never set for MADV_FREE'd memory, whose contents are indeterminate ([madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html)).

**realloc:**
- same class: return p;
- runs: grow in place if the following pages are free in the hugeblock bitmap (a metadata-only operation), otherwise allocate and copy;
- shrinking a run returns the tail to the filler;
- solo: mremap;
- `realloc(p, 0)` frees p and returns NULL, matching glibc ([malloc.c](https://sourceware.org/git/?p=glibc.git;a=blob;f=malloc/malloc.c;hb=HEAD));
- on failure the old block is left intact.

**aligned:** see §3. `posix_memalign` returns EINVAL as specified ([posix_memalign(3)](https://man7.org/linux/man-pages/man3/posix_memalign.3.html)). `aligned_alloc` accepts sizes that are not a multiple of the alignment, as glibc does.

---

## 11. OS layer

### 11.1 Reservation and commit by overcommit mode

| Mode (`/proc/sys/vm/overcommit_memory`) | Arena reservation | Commit | Decommit |
|---|---|---|---|
| 0, 1 | `mmap(RW, PRIVATE|ANON|NORESERVE)` | first touch, or MADV_POPULATE_WRITE (§11.4) | DONTNEED |
| 2 (strict) | `mmap(PROT_NONE)`, which is not accounted ([overcommit](https://docs.kernel.org/mm/overcommit-accounting.html)) | `mprotect(RW)` that *moves a single high-water boundary* in steps of ≥ 4 H. ENOMEM → malloc returns NULL/ENOMEM | DONTNEED. Commit charge is returned only by lowering the boundary when the tail hugeblocks are free |

- **VMA count:** ≤ 2 per arena in mode 2, and ≤ 1 per contiguous arena group in modes 0/1.
- **Placement:** each new arena is hinted at `prev_end` with `MAP_FIXED_NOREPLACE` (4.17+, [mmap(2)](https://man7.org/linux/man-pages/man2/mmap.2.html)), so neighbouring arenas merge into one VMA. If that fails, over-map by 64 MiB and trim.
- **Naming:** `PR_SET_VMA_ANON_NAME "sohamalloc"` applies to whole arenas; the same name keeps merges intact ([pr_set_vma](https://man7.org/linux/man-pages/man2/pr_set_vma.2const.html)).
- **RLIMIT_AS:** read at init; arena growth stops at limit/2, and the remainder is left to solos and the application.
- **Per-feature VMA budget:**
  - heap: ≤ #arena groups (×2 in mode 2);
  - guarded pool: 1, or ≤ 130 with the PROT_NONE fallback;
  - one per solo;
  - one per NUMA node's arena window;
  - metadata (gmap, slots): 2.
- **CI gate:** the line count of `/proc/self/maps` during a 72 h soak.

### 11.2 Release engine

- Candidates (whole hugeblocks first, then subrelease runs ≥ 64 KiB) are marked RELEASING under the shard lock and gathered into an iovec of ≤ 1024 entries ([uio.h](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/uio.h)).
- The lock is dropped before the syscall.
- **Primary:** `process_madvise(PIDFD_SELF, iov, n, MADV_DONTNEED, 0)`.
  - Any advice is allowed on self from 6.13. `PIDFD_SELF` arrived in 6.15; before that we use a cached `pidfd_open(getpid(), 0)` fd, reopened on EBADF.
  - From 6.16, a vector costs one TLB flush. From 6.17, DONTNEED/FREE take only a per-VMA read lock ([madvise.c](https://raw.githubusercontent.com/torvalds/linux/master/mm/madvise.c); [process_madvise(2)](https://man7.org/linux/man-pages/man2/process_madvise.2.html)).
  - jemalloc ships the same mechanism behind a gate ([pages.c](https://github.com/jemalloc/jemalloc/blob/dev/src/pages.c)).
  - The probe latches it off on EPERM, EINVAL, ENOSYS or EBADF.
  - Partial returns are resumed from the next byte.
- **Fallback:** coalesced `madvise`. EINVAL on locked pages → `MADV_DONTNEED_LOCKED` (5.18) → else record as "not released".
- `errno` is saved and restored around every syscall reachable from `free` ([Replacing malloc](https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html)).
- **TLB note:** on x86 without INVLPGB, each flush IPIs every CPU the mm has run on. INVLPGB (6.15, AMD) applies only when the process is active on ≥ 3 CPUs ([LWN](https://lwn.net/Articles/1008065/)). arm64 uses broadcast TLBI. Batching is therefore worth most on x86 and large processes.

### 11.3 THP policy (no toggling, no surprises)

At init we read `enabled`, `defrag`, `khugepaged/max_ptes_none` and `shrink_underused`, plus `prctl(PR_GET_THP_DISABLE)` ([transhuge](https://docs.kernel.org/admin-guide/mm/transhuge.html)).

| System state | Action |
|---|---|
| enabled=always | No madvise. Prefer whole-hugeblock release. Subrelease only at pressure ≥ moderate: freeing 4 KiB inside a PMD THP only partially maps it until deferred split |
| enabled=madvise, defrag ∈ {defer, never} | `MADV_HUGEPAGE` **once per arena at reservation** (one VMA flag, not per range) |
| enabled=madvise, defrag ∈ {madvise, always, defer+madvise} (the default is madvise) | No MADV_HUGEPAGE: those defrag modes direct-reclaim on fault, which caused Go's 100+ ms stalls ([go#61718](https://github.com/golang/go/issues/61718)). Instead, the maintenance thread `MADV_COLLAPSE`s hugeblocks that are ≥ 95% used and older than 10 s (jemalloc's HPA thresholds, [hpa_opts.h](https://github.com/jemalloc/jemalloc/blob/dev/include/jemalloc/internal/hpa_opts.h)), with backoff on latency > 1 ms and ≤ 4 collapses/s. Never on a malloc path, never under locks |
| enabled=never, or THP disabled by prctl (including `PR_THP_DISABLE_EXCEPT_ADVISED` without opt-in) | Never collapse. `MADV_COLLAPSE` would ignore the admin's choice ([madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html)) |

- When `max_ptes_none` is high (511 is khugepaged's default, [khugepaged.c](https://github.com/torvalds/linux/blob/master/mm/khugepaged.c)), subreleased ranges can be re-collapsed. Subreleased hugeblocks are therefore ranked first for refill, to restore their density.
- S0 does no zero-on-free, which avoids making retained memory look "underused" to the 6.12 shrinker (INFERENCE from [transhuge](https://docs.kernel.org/admin-guide/mm/transhuge.html)).
- Measured coverage comes from `smaps_rollup` AnonHugePages, read on demand.

### 11.4 Prefault

On a hugeblock *first touch* in the throughput profile, or when the maintenance thread exists, `MADV_POPULATE_WRITE` (5.14) is issued off the fast path. Failure returns an error instead of SIGBUS ([madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html)) and malloc returns NULL. This targets minor faults, which reach up to 54% of runtime in short-lived workloads per one non-peer-reviewed study ([arXiv 2607.14789](https://arxiv.org/abs/2607.14789)). Never over guarded ranges: POPULATE fails on them.

### 11.5 Kernel matrix

| Feature | ≥ | Fallback |
|---|---|---|
| rseq mm_cid/node_id | 6.3 (+ glibc ≥ 2.35 or self-registration) | cpu_id → TLS |
| membarrier expedited rseq | 5.10 | TLS mode |
| process_madvise on self | 6.13 (PIDFD_SELF 6.15) | madvise |
| MADV_GUARD_INSTALL | 6.13 | §14 |
| MADV_COLLAPSE | 6.1 | none |
| per-VMA lock for DONTNEED | 6.17 | — |
| time-slice extension | 7.0 (needs V2) | none |
| unprivileged PSI triggers | 6.4 (2 s windows) | polling |

---

## 12. Release policy (formulas)

**Per node shard:**
- U = bytes in pages or runs handed to slots or users;
- F = free bytes still backed (resident);
- P10 = max U over a sliding 10 s window, sampled on slow paths and by the maintenance thread at 100 ms;
- P120 = max U over 120 s.

**Retained-free target** (skip-subrelease after Maas et al. [ISMM'21](https://research.google/pubs/pub50436/) and TCMalloc's 10 s/120 s windows, [parameters.cc](https://github.com/google/tcmalloc/blob/master/tcmalloc/parameters.cc)):

```
C* = min( max(P10 − U, 0) + 0.25·max(P120 − P10, 0),
          κ·(0.10·U + 4 MiB) )
release R = max(0, F − C*)          // whole free hugeblocks (oldest-free first),
                                    // then subrelease runs if pressure ≥ moderate
```

**Pressure** comes from the process's own cgroup: the path is resolved from `/proc/self/cgroup`, and L = min(memory.high, memory.max) over ancestors up to the cgroupns root ([cgroup-v2](https://docs.kernel.org/admin-guide/cgroup-v2.html)). Headroom h = (L − memory.current)/L, sampled at most every 100 ms with `pread` on a cached O_CLOEXEC fd. `memory.stat` is never read on a slow path, because flushes are slow ([LWN](https://lwn.net/Articles/951695/)).

| Level | Condition | κ | Extra action |
|---|---|---|---|
| none | no limit known | 1 | time decay only |
| normal | h > 20% | 1 | — |
| moderate | 5% < h ≤ 20%, or `memory.events` high incremented | 0.25 | subrelease allowed; slot budget halved |
| severe | h ≤ 5% | 0 | reclaim idle slots (fence); flush every remote buffer at the next event |

**Time decay** (no limit known): free hugeblocks idle ≥ 2 s are released; subrelease candidates idle ≥ 30 s.

**CPU cap:** release work ≤ 1% of process CPU per 1 s window, Go's scavenger model ([mgcscavenge.go](https://raw.githubusercontent.com/golang/go/master/src/runtime/mgcscavenge.go)).

**Front-end budget:**

```
cap_slot = clamp( L_live / (16·S_active), 512 KiB, 8 MiB )
```

Here S_active is the number of slots with a slow path in the last 1 s. Total owned-but-free memory across slots stays ≤ L_live/16 + 512 KiB·S_active. Buffered remote frees add ≤ 64 KiB per slot.

**Blowup bound [derived, informal]:** RSS ≤ live + Σ_slots cap_slot + 64 KiB·S + pool fragmentation + C* + metadata (0.2%). Pool fragmentation is bounded per page by "queued only when ≥ 1/8 free", i.e. orphaned pages waste < 1/8 of their size, plus fully free pages, which are returned at once.

**Maintenance thread:**
- Created lazily only when ≥ 2 slots have been active and F + C* > 64 MiB.
- Never created in single-threaded processes, because `unshare(CLONE_NEWUSER)` requires a single-threaded caller ([unshare(2)](https://man7.org/linux/man-pages/man2/unshare.2.html)).
- Without the thread, an idle check runs on the next slow path: if now − last_release > 1 s, release.
- `SOHAMALLOC=nothread` turns it off. `soham_release()` and `malloc_trim` force a release.

**MADV_FREE** is an opt-in "warm stage" in the throughput profile only. It is drained to DONTNEED at moderate pressure, and `LazyFree` is reported. Go returned to DONTNEED over monitoring confusion ([go1.16](https://go.dev/doc/go1.16), [go#42330](https://github.com/golang/go/issues/42330)).

**Experiment:** an RSS-time integral in phased runs (ramp, steady, burst, 60 s idle) and under memory.max at 1.2× the working set, against jemalloc with `background_thread` and mimalloc v3.

---

## 13. NUMA, NUCA, topology

- **Topology:** read lazily via raw sysfs reads of `cpu*/cache/index*/shared_cpu_list`, plus `cpu_capacity` on asymmetric systems ([sched-capacity](https://docs.kernel.org/scheduler/sched-capacity.html)). Any failure means one domain.
- **NUCA:** cid-indexed slots are not CPU-bound, so locality is chosen at **refill**. `rseq.cpu_id` → LLC id selects the pool, and the node id selects the page-heap shard. The NUCA-aware transfer caches in ASPLOS'24 gave +0.32% fleet throughput and −4.37% LLC load misses ([ASPLOS'24](https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf)).
- **NUMA:**
  - Each node gets its own arena window, i.e. a separate VMA. There is no `mbind` on sub-ranges, which would split VMAs ([numa policy](https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html)).
  - First-touch places pages, and the process mempolicy, including weighted interleave, is respected.
  - Frees are origin-aware by construction: blocks go back to the owning page, and orphans go to the page's node pool ([NUMAlloc](https://par.nsf.gov/servlets/purl/10418579)).
  - Threads are never bound.
  - Per-node shards exist only when more than one node has memory. Google saw no material gain from per-NUMA vCPU ids ([LKML](https://lkml.iu.edu/2208.0/01988.html)).
- **Tiering:** cold-hinted hugeblocks are kept separate, so kernel demotion moves whole pages ([OBASE](https://arxiv.org/abs/2603.00378)).

---

## 14. Security tiers and their cost (performance budget first)

| Tier | Contents | Cost (target / evidence) |
|---|---|---|
| **S0** (default) | Encoded links (bswap ⊕ page key ⊕ address); same-page check on pop; head≠block on push; 16-byte alignment check; deterministic foreign-pointer handling; no VA reuse across partitions; GWP-ASan-style pool; sized-delete mismatch checked *lazily* when a page is collected | ≈ +7 instructions per pair [est]. glibc's safe-linking was "not measurable" ([Check Point](https://research.checkpoint.com/2020/safe-linking-eliminating-a-20-year-old-malloc-exploit-primitive/)). Target ≤ 2%, measured by ablation |
| **S1** (`secure`) | Full slot-boundary check via `recip` multiply on every free; page switched to a **bitmap allocator** (replacing the free list, hardened_malloc style); shuffled 8-block windows; zero-on-free ≤ 1 KiB with check on reuse; 2–4 call-site buckets for classes ≤ 1 KiB; byte-budgeted quarantine (≤ 1% of heap) | ≤ 10%. Analogues: snmalloc < 5% ([docs](https://github.com/microsoft/snmalloc/blob/main/docs/security/README.md)), mimalloc secure ~10% ([readme](https://github.com/microsoft/mimalloc/blob/main/readme.md)) |
| **S2** (arm64 MTE) | Tag ≤ 32 KiB, guards above; exclude neighbours' and previous tags; retag on free ≤ 4 KiB; arenas mapped `PROT_MTE` at reservation; retag after purge, since DONTNEED may clear tags ([kernel MTE](https://docs.kernel.org/arch/arm64/memory-tagging-extension.html)) | 1.02×–1.56×, outlier 6.64× ([arXiv 2601.11786](https://arxiv.org/abs/2601.11786)). Treated as probabilistic ([TikTag](https://arxiv.org/abs/2406.08719)) |

**Guarded pool:**
- 16–64 slots of size ≤ B in one dedicated arena, reserved with `MADV_NOHUGEPAGE` (set once).
- Guards are installed once with `MADV_GUARD_INSTALL` (6.13+). Install zaps THPs and fails on mlocked VMAs ([madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html)), so guards never touch the THP-eligible heap.
- Fallback is PROT_NONE in this pool only (≤ 130 VMAs). Under `mlockall`, EINVAL → the pool runs without guards and logs once.
- Sampling rides the existing countdown; roughly 1 in tens of thousands of small allocations [derived from a 2 MiB mean and ~64 B average].
- The SIGSEGV handler chains to the previous handler and is not installed if a non-default handler already exists (JVMs).

**AllocToken / pointer split:** when `__alloc_token_*` entry points are called ([Clang AllocToken](https://clang.llvm.org/docs/AllocToken.html)), pointer-free requests use `tbins`, a constant offset with no branch (TCMalloc's partition-offset trick, [sizemap.h](https://github.com/google/tcmalloc/blob/master/tcmalloc/sizemap.h)). **Cost:** up to 2× partial pages for those classes, counted within the slot budget.

---

## 15. API, observability, configuration

**Standard interface:**
- `malloc`, `free`, `calloc`, `realloc`, `reallocarray` (implemented on top of our own realloc), `aligned_alloc`, `posix_memalign`, `memalign`, `valloc`, `pvalloc` (runtime B), `malloc_usable_size`, `free_sized`, `free_aligned_sized`.
- Shims: `mallopt` (accepts and ignores), `mallinfo`/`mallinfo2`, `malloc_trim` → `soham_release`, `malloc_stats`, `malloc_info` (XML).
- glibc's replacement contract ([Replacing malloc](https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html)): errno is preserved, and nothing over PTRDIFF_MAX is accepted.

**C++:**
- A separate C++ translation unit, `libsohamalloc_cxx`. Throwing `new` loops on `get_new_handler()` and then throws `bad_alloc` ([new.delete.single](https://eel.is/c++draft/new.delete.single)).
- Also provided: nothrow, aligned and sized forms.
- Sized delete may legally arrive as unsized, so size is a **validation hint only**. It is used for speed only on runs, where it skips the head lookup.
- TCMalloc-compatible `__size_returning_new` and hot/cold `operator new(size_t, __hot_cold_t)` ([malloc_extension.h](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h)). These are the overloads MemProf rewrites calls into ([MemProf](https://www.llvm.org/docs/MemProf.html)). A cold hint selects the cold partition.

**Extensions:**
- `soham_malloc_sized`, `soham_release`, `soham_thread_flush`, `soham_stats_json(buf, len)` (versioned schema).
- A Rust `GlobalAlloc` crate: every Rust free is sized, and Rust forbids unwinding allocators ([GlobalAlloc](https://doc.rust-lang.org/std/alloc/trait.GlobalAlloc.html)).
- First-class heaps and a heap visitor for CPython free-threading are planned for Phase 4 with a model-checked destroy ([PEP 703](https://peps.python.org/pep-0703/)).

**Stats:**
- Counters are updated on slow paths only (page claim, detach, flush) plus per-page counts at retirement. The fast path updates nothing.
- The explain report has two views:
  - the allocator view: committed, in use, slot-owned, buffered, pooled, orphan, retained C*, released-reserved, metadata;
  - the kernel view, on demand, from `smaps_rollup`: Rss, AnonHugePages, LazyFree. The difference is labelled "outside sohamalloc".
- Also reported: mode (rseq-cid, tls-exclusive or tls-shared, with the reason), probe results, fence count, lock wait, abort rate.
- USDT probes on slow paths; Valgrind client requests and an ASan-poison build mode.

**Configuration:**
- `SOHAMALLOC=<profile>[,k=v]`. Profiles: default, throughput, lowmem, latency, secure, deterministic (fixed seed, count-driven decay, no thread, TLS mode), debug.
- Under `getauxval(AT_SECURE)`, only options that strengthen security are honoured, and no paths or fds ([getenv(3)](https://man7.org/linux/man-pages/man3/getenv.3.html)).

---

## 16. Portability

The prim layer provides: `reserve(align)`, `commit`, `decommit(batch)`, `guard`, `name`, page and PMD size, `cpu_slot_begin/commit` (rseq asm or a TLS stub), `fence_all`, `now_coarse`, and raw file reads.

| Platform | Notes |
|---|---|
| Linux x86-64, arm64 | Full design |
| musl | Self-registered rseq, else TLS. Override `aligned_alloc`, or musl returns ENOMEM ([glue.h](https://git.musl-libc.org/cgit/musl/tree/src/malloc/mallocng/glue.h)) |
| 32-bit, wasm | Out of scope: the arena/gmap scheme assumes 64-bit VA |
| macOS, Windows | Later, via zone registration and a redirect DLL (mimalloc's approach, [readme](https://github.com/microsoft/mimalloc/blob/main/readme.md)). Windows needs reserve/commit in place of overcommit modes |

---

## 17. Failure modes

| Failure | Behaviour |
|---|---|
| mmap/mprotect ENOMEM, RLIMIT_AS | NULL plus ENOMEM; C++ goes through new_handler, then bad_alloc |
| Overcommit OOM at fault time | Outside our control in modes 0/1. Optional `SOHAMALLOC_BUDGET` (default off) fails allocations before memory.max |
| Corrupt link or descriptor magic | Abort with a report (S0 decoded-link check) |
| rseq registration lost, CID out of range | Mask plus a sentinel slot; the self-test detects it and switches to TLS mode |
| membarrier fails at runtime | Stop reclaiming idle slots, log, and count stranded bytes |
| process_madvise latched off | madvise fallback |
| cgroup files unavailable (seccomp, no /sys) | Pressure level "none", time decay only |
| Thread creation blocked | No maintenance thread; slow-path idle checks only |
| Foreign pointer | §3 policy |
| Signal reentry in a slow path | Emergency bump arena |
| Fork mid-operation | inflight counter wait plus child re-home |

---

## 18. Decision register

| Decision | Alternatives (rejected because) | Cost | Risk | Validating experiment |
|---|---|---|---|---|
| Page-based slots, rseq-cid default | Per-thread heaps (churn stranding [#351](https://github.com/microsoft/mimalloc/issues/351)); per-CPU object arrays (transfer caches, page mixing) | ~4 instructions per op [est]; asm per architecture | ABI churn; RSEQ_SIG | Mode matrix §5.4 on 3 kernel lines |
| Packed count+head, single commit | Separate used counter (2 stores, breaks rseq); lazy walk counts (no prompt empty detection) | 48-bit pointer limit | 57-bit VA future; MTE tag bits | 5-level-paging box with `ulimit`; MTE build path |
| Remote buffer plus per-page CAS | Per-object CAS (mimalloc); snmalloc MPSC queues (fork-detach hazard, [freelist_queue.h](https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/mem/freelist_queue.h)) | ≤ 64 KiB/slot delay; sort cost | Producer/consumer regressions as in BatchIt | xmalloc-test, back-pressured message passing, glibc-thread |
| 64 MiB arenas + gmap | 1 GiB regions (overcommit/RLIMIT); radix tree (3 loads) | More mmaps | Arena exhaustion pattern | `ulimit -v` 2 GiB; 100 GiB genomics trace |
| Temeraire-ordered filler + density split | Plain first fit; v1 "fullest first" | Shard lock | Lock contention for large-run workloads | 192-thread 256 KiB–2 MiB churn; dTLB walks |
| THP via sysfs-keyed policy | Blanket MADV_HUGEPAGE (Go stalls); 2 MiB purge minimum (mimalloc +81% RSS, [#1282](https://github.com/microsoft/mimalloc/issues/1282)) | Lower coverage under defrag=madvise | Collapse latency | THP {always, madvise, never} × max_ptes_none {0, 511}; p99 fault latency |
| process_madvise batching | Per-range madvise | Probe | Pre-6.13 kernels gain nothing | IPIs per MiB released (`/proc/interrupts` TLB line) |
| No small-object prediction | LLAMA-like return-address table (22.5 ns, wrapper collapse) | Loses a possible fragmentation win | — | Counterfactual mode on traces |
| Lazy maintenance thread | No thread (idle never releases, [jemalloc#1398](https://github.com/jemalloc/jemalloc/issues/1398)); always-on thread (breaks unshare) | Complexity | seccomp clone denial | Idle-60 s phase RSS drop |

**Validation spine:**
- **Phase 0:** harness, trace recorder, geometry, gmap, ownership state machine modelled in GenMC/TLA+, glibc and C++ conformance plus differential fuzzing.
- **0.5:** trace simulator for classes and pools.
- **1:** single-threaded performance.
- **2:** rseq plus remote frees under forced-preemption tests.
- **3:** OS layer.
- **4:** security tiers.
- **5:** large-run prediction (counterfactual).

**Baselines** are pinned: glibc 2.44, jemalloc 5.4.0, mimalloc v3.4.5, Google TCMalloc confirmed running in per-CPU mode, and snmalloc. The gperftools build ("tc" in mimalloc-bench) is never accepted as a stand-in for TCMalloc ([mimalloc-bench](https://github.com/daanx/mimalloc-bench)).

---

## 19. Expected per-operation budget (all [est], to be replaced by measurement)

| Operation | Instructions | Dependent loads | Atomics |
|---|---|---|---|
| malloc ≤ 1 KiB, rseq | ~26 | 4 | 0 |
| malloc, TLS mode | ~20 | 3 | 0 |
| free local, rseq | ~31 (lookup + push) | 2 | 0 |
| free remote (buffered) | ~27 + ~10 amortised | 2 | 1 per page batch + 2 per 64 frees |
| free 1–256 KiB remote | ~30 | 2 | 1 CAS |
| refill from pool | ~300–3000 cycles | — | 1 lock |
| idle-slot reclaim | membarrier (all CPUs) | — | ≤ 1/s |

---

## 20. Top 10 weaknesses of this design

1. **The fast path is fatter than mimalloc's.** About 26 against "7 to 10" instructions for malloc, because of rseq setup and clear, sentinel XOR, decode and validation. The whole bet is that locality and TLB wins pay for it at the application level. If the §6.5 application gate fails, rseq-cid mode is not justified and TLS mode becomes the default.
2. **The rseq dependency is fragile.** We need glibc's registration to cover mm_cid, the correct RSEQ_SIG (UNVERIFIED), membarrier rseq, and hand-written asm for 5 critical-section kinds on 2 architectures. The kernel changed rseq in 6.19 and 7.1; another change could break us the way 6.19 broke TCMalloc.
3. **The ownership state machine is subtle.** CLAIMING, resumable transitions and leases are exactly where allocators hide bugs. The design is not shippable until a model checker passes. mimalloc's 2026 "LLM audit" fix list is a warning ([readme](https://github.com/microsoft/mimalloc/blob/main/readme.md)).
4. **Idle-slot reclaim needs a process-wide membarrier.** On a 256-CPU box running 200 threads, each reclaim IPIs every running CPU. Rate-limiting bounds the cost but leaves memory stranded for up to ~1 s, and longer if pressure never appears.
5. **The 48-bit packed head blocks future layouts.** It rules out 57-bit heaps, and in the MTE tier the count competes with tag bits. That tier needs a different list encoding, which means a second code path.
6. **In per-CPU CID mode (threads > CPUs), most frees become "remote".** CIDs move with the CPU, not the thread, so frees go through buffered CAS hand-off. The cost is amortised but not zero, and freed blocks become reusable later than in an ownership-free per-CPU array (TCMalloc). Workloads with heavy thread oversubscription may lose to TCMalloc.
7. **The THP default is conservative.** On the common enabled=madvise / defrag=madvise setting, we rely on background `MADV_COLLAPSE` rather than fault-time hugepages. Hugepage coverage may then trail Temeraire under THP=always, and the background thread does not run in single-threaded processes.
8. **The RSS bounds are additive per active slot and only loosely proven.** The slot budget formula, the 1/8 orphan rule and the 64 KiB buffers bound waste, but a 256-slot burst can still pin ~128 MiB minimum [derived from 512 KiB × 256] until reclaim. No Hoard-style proof has been written yet.
9. **TLS-exclusive mode is not signal-reentrancy-safe on the fast path,** and the emergency arena leaks its blocks by design. Programs that malloc in signal handlers on systems without rseq are exposed.
10. **Much of the evidence is borrowed, and the hardest parts are unmeasured.** The page-heap ordering, the density split and the release windows come from Google's fleet results, which have no public replication. Every number in §19 is an estimate. The size-class table is a bootstrap until traces exist. Whether a 64 MiB arena plus gmap lookup really matches mimalloc's single-arithmetic lookup is untested.