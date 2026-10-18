# sohamalloc v2 design: memory efficiency and kernel citizenship first

**Status:** design proposal, 2026-10-18. It replaces v1 §4–§9 and was written against the five research reports and the three critiques.

**Notation:**
- `[tag]` points to the reference list at the end.
- **UNVERIFIED** marks a claim I could not confirm from a primary source.
- **EST** marks my own estimate. It is not a measurement.
- **PARAM** marks an initial design constant. Each one has a validating experiment (E-n, see §19).

No numbers in this document come from runs of sohamalloc, because none exist yet.

---

## 0. Stance and what is actually new

**Primary objective.** Keep resident memory close to live bytes and give memory back promptly and honestly, across three kinds of process:
- a 3 ms CLI tool,
- an idle desktop app,
- a container hitting `memory.high`.

This must hold while fast-path speed stays within a few percent of mimalloc v3.4.x and TCMalloc. Kernel citizenship means: few VMAs, no runtime VMA flag changes, batched release, no `mmap_lock` writes on steady-state paths, and no fighting THP/khugepaged.

**What we reuse, credited:**

| Idea | Source |
|---|---|
| Page-local free lists and owned-bit adoption of abandoned pages | mimalloc [mi-free] |
| vCPU-indexed caches | TCMalloc `RseqVcpuMode::kMM` [tc-percpu]; the kernel's mm_cid commit [k-cid] |
| Hugepage filler ordering and skip-subrelease | Temeraire / ISMM'21 [tc-hpf][tc-sub] |
| Capacity-based sparse/dense split | ASPLOS'24 [asplos24] |
| Remote-free batching per destination slab | BatchIt [batchit] |
| `process_madvise` batching | jemalloc [je-pages] |
| Fork handling that waits for critical sections | snmalloc PreventFork [sn-fork] |
| Lazy slot provisioning | PartitionAlloc [pa] |

**Candidate contributions.** These are combinations, each with a falsifiable claim:
1. **Thread churn strands nothing.** Caches are keyed by concurrency slot, not by thread. Idle slots are reclaimed by a fenced, model-checked protocol. This targets mimalloc #351 and #1025 and Node #51868 [mi-351][mi-1025][node-51868].
2. **Staged, pressure-aware release, honest by default.** Release uses `MADV_DONTNEED` through `process_madvise` vectors. Retention is driven by recent demand and cgroup headroom, and an inline trigger covers single-threaded programs.
3. **A VMA layout fixed at reservation.** The allocator never changes VMA flags or protection on sub-ranges at runtime.
4. **An accounting model that never invents an RSS sum.** It reports what the allocator knows next to what the kernel reports (§16).

**Dropped from v1:**
- mm_cid indexing presented as a novelty.
- The return-address lifetime predictor on the fast path. It is replaced by zero-cost proxies and hint ABIs.
- The adaptive switch to message passing.
- The per-span double-free bitmap in the default tier.
- A writable 64 GiB reservation.
- Hard-coded 4 KiB / 2 MiB geometry.
- `mremap` inside regions.
- Meshing.

---

## 1. Falsifiable targets

All targets are evaluated by the harness in §19.

| Metric | Target | Baseline |
|---|---|---|
| RSS(`/bin/true`-like, 10 size classes touched) | ≤ glibc 2.44 + 64 KiB | glibc 2.44 measured in E1 |
| RSS−time integral, thread-churn benchmark (create/exit 1k threads/s, 60 s) | ≤ 0.7× mimalloc v3.4.5 | E7 |
| RSS 10 s after a burst ends, then idle for 60 s, in a container | ≤ live + max(4 MiB, 12.5% of live) | E8 |
| mimalloc-bench geomean time | ≤ 1.05× mimalloc v3.4.5, same machine, same baseline for speed and memory | E10 |
| Steady-state VMAs owned by the allocator | ≤ 6 + one per huge object (§2.4) | `/proc/self/maps` CI gate |
| `mmap_lock` write acquisitions in steady state | 0 per second (excluding huge-object map/unmap) | tracing in E12 |

---

## 2. Runtime geometry and address-space layout

### 2.1 Geometry is discovered, never compiled in

| Parameter | Source | Typical values |
|---|---|---|
| `P` (base page) | `getauxval(AT_PAGESZ)` | 4 KiB on x86-64; 4, 16 or 64 KiB on arm64 [k-arm64hp] |
| `PMD` | `/sys/kernel/mm/transparent_hugepage/hpage_pmd_size`, read lazily, raw `open(O_CLOEXEC)`/`read` [k-thp] | 2 MiB; 32 MiB (16K granule); 512 MiB (64K granule) [k-arm64hp] |
| `H` (hugeblock) | `PMD` if `PMD ≤ 4 MiB`, else 2 MiB. 2 MiB is the contiguous-PTE size on 16K/64K granules [k-arm64hp] | 2 MiB everywhere common |
| `SL` (slice, page-map granule) | `max(64 KiB, P)` | 64 KiB |
| Small-object limit | 256 KiB | — |

**Release granularity.** The unit is a slice. Sub-slice tails are released at `P` only by realloc-shrink.

### 2.2 Regions

Every byte sohamalloc hands out lives in a registered **region**. A region is a 1 GiB-aligned, 1 GiB-long reservation (PARAM) and is its own VMA. A **region table** `rtab` is a static BSS array indexed by `addr >> 30`:
- 2^17 entries × 8 B = 1 MiB of BSS for a 47-bit user VA. That limit is the x86-64 default without a hint [k-5lvl]; arm64 is 48-bit without a hint [k-arm64mem].
- BSS pages that are never touched cost no RSS.
- On first use, an entry points to the region's descriptor in the metadata region.

The 1 GiB size is TCMalloc's chunk size [tc-design]. It keeps VSZ modest compared with v1's 64 GiB, which alarms operators and breaks `ulimit -v` [hadoop][node-51868].

| Region kind | Count | VMA policy, set once at creation | Contents |
|---|---|---|---|
| HEAP | 1 per NUMA node to start; more on demand | THP policy per §14, named `soham-heap` via `PR_SET_VMA_ANON_NAME` on the whole VMA (5.17+) [k-vmaname] | small/medium pages, large runs |
| META | 1 (grows by a new 1 GiB reservation if exhausted) | `MADV_NOHUGEPAGE`; guards at both ends | page meta, slot array, region descriptors |
| GUARD | 0 until sampling or S1 needs it | `MADV_NOHUGEPAGE` | GWP-ASan slots, guarded slabs |
| HUGE | 1 VMA per object ≥ 32 MiB (PARAM) | inherits | dedicated mmaps; the only place `mremap` is used |

### 2.3 Reservation and commit, by overcommit mode

The mode is read once from `/proc/sys/vm/overcommit_memory` using raw syscalls. If the read fails, mode 0 is assumed.

| Mode | Reservation | Commit | Decommit |
|---|---|---|---|
| 0 or 1 | `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`. With `NORESERVE` honored, the mapping is not accountable, so the size-vs-RAM heuristic is not applied (`accountable_mapping`, "honor MAP_NORESERVE if allowed to overcommit") [k-mmapc][k-vmac] | First touch, or `MADV_POPULATE_WRITE` (5.14+) when prefaulting [madvise] | `MADV_DONTNEED` |
| 2 | `PROT_NONE` (charged nothing [k-overcommit]) | `mprotect(RW)` in contiguous 64 MiB steps from the bottom of the region (PARAM). The new RW range merges with the previous one, so a region has at most 2 VMAs. `ENOMEM` from `mprotect` makes `malloc` return NULL with `errno = ENOMEM`, which preserves strict-accounting semantics | `MADV_DONTNEED` returns RSS. Commit charge is returned only by `mprotect(PROT_NONE)` of whole 64 MiB extents at the top of the RW range, so the region stays at 2 VMAs |

**Address-space limits (`RLIMIT_AS`).** When `RLIMIT_AS` is finite, region size becomes `clamp(pow2floor(RLIMIT_AS/16), 64 MiB, 1 GiB)`, and the huge-object threshold drops to region/4. Reservations use `MAP_FIXED_NOREPLACE` hints (4.17+) [mmap] to keep regions adjacent. If a hint fails, the next free 1 GiB-aligned address is used: over-map by 1 GiB and trim, once per region.

### 2.4 VMA budget, enforced in CI

| Item | VMAs |
|---|---|
| Each HEAP region | 1 (2 under mode 2) |
| Bootstrap NOHUGEPAGE split (THP=always only, §14) | +1 |
| META | 1, plus 0 guards with `MADV_GUARD_INSTALL` (6.13+) or 2 `PROT_NONE` guards otherwise, installed once |
| GUARD region | 1 when active |
| Each HUGE object | 1 |
| Each extra NUMA node | 1 |

**Never done at runtime:**
- `MADV_[NO]HUGEPAGE` or `mprotect` on sub-ranges. Both split VMAs and take `mmap_lock` for write [k-madvise].
- `mbind` on sub-ranges, which splits VMAs [k-numa].
- `mremap` inside a region.

---

## 3. Pointer → metadata lookup

```
// metadata region, per HEAP region (1 GiB = 16384 slices of 64 KiB)
struct region_desc {           // 64 B, in META
  page_meta *meta;             // 16384 * 64 B = 1 MiB VA, lazily committed
  uint16_t  *delta;            // 16384 * 2 B = 32 KiB VA: slices back to page start
  uint64_t   base;  uint32_t node; uint32_t kind_flags; ...
};
struct page_meta {             // 64 B = one cache line, per slice (only first-slice entries live)
  uint64_t free_head;    // [0]  ptr(bits 0..47) | count(bits 48..63); owner-only, rseq-committed
  uint32_t owner_tag;    // [8]  (slot_id << 3) | flags{FULL, ABANDONED, SPECIAL}; 0xFFFFFFF8 unowned
  uint16_t provisioned;  // [12] blocks made available so far (lazy, P at a time)
  uint8_t  cls, kind;    // [14] size class, {SMALL, MEDIUM, RUN, SAMPLED}
  uint64_t key;          // [16] per-page random free-list key, re-drawn at each (re)init
  uint64_t start;        // [24] page start address
  uint32_t len;          // [32] page length
  uint32_t recip;        // [36] 32-bit reciprocal of block size (index / boundary checks, snmalloc-style [sn-sct])
  uint16_t capacity, bump; // [40]
  uint32_t req_or_aux;   // [44] RUN: requested size (for tail release); SMALL: lifetime/occupancy bucket
  _Atomic uint64_t xfree;// [48] remote list head | OWNED bit (mimalloc protocol [mi-free])
  page_meta *next;       // [56] intrusive link in pools / filler buckets
};
```

**Lookup (unsized and sized free alike):**

```
idx = p >> 30;                       if (idx >= RTAB_N) goto foreign;   // also catches non-canonical
r   = rtab[idx];                     if (!r) goto foreign;              // load 1 (rtab line L1-hot: 8 GiB/line)
s   = (p >> 16) & 0x3FFF;
pg  = r->meta + (s - r->delta[s]);   // loads 2 (delta), 3 (pg->*)
```

**arm64.** TBI/MTE tag bits are stripped first: `p & 0x00FF_FFFF_FFFF_FFFF`, one AND.

**Foreign pointers:**
- In S1/S2 or debug builds: abort with a diagnostic.
- In S0: forward to `dlsym(RTLD_NEXT, "free")` only when one was resolved at init; otherwise ignore and count. This matters when sohamalloc is late-loaded, which TCMalloc warns about [tc-design].

**Cost.** `delta` is 0 for single-slice pages and turns medium/run interior slices into a single subtraction. Interior pointers never reach `free` (§11.4).

**Memory cost.** 64 B of meta plus 2 B of delta per 64 KiB slice, about 0.1%. It is committed only for slices in use: one 4 KiB meta page covers 4 MiB of heap.

**Why not mimalloc's arithmetic path (aligned meta, one load) [mi-pagemap]?** It co-locates metadata with data at predictable offsets, and it needs forwarders for multi-slice pages. We pay one extra dependent load (EST about 1–2 cycles with L1 hits) for metadata in a separate VMA.

**Experiment E3:** an free-heavy microbenchmark comparing against the mimalloc flat map. Kill criterion: more than 3% slowdown.

---

## 4. Size classes and page kinds

**The scheme (64 classes), with numbers from my script:**

| Band | Spacing | Classes |
|---|---|---|
| 16–128 B | 16 B | 8 |
| 128 B – 1 KiB | 8 per doubling (144, 160, …, 1024) | 24 |
| 1 KiB – 256 KiB | 4 per doubling (1280, 1536, 1792, 2048, …, 256 Ki) | 32 |

**Worst-case internal waste, as a fraction of the block:**
- 46.9% at 17→32 B (unavoidable with 16 B alignment),
- 10.9% in 129 B–1 KiB (513→576),
- 20.0% in 1–256 KiB (131073→163840).

This corrects v1's 12.5% claim, as the critics showed [je-man]. The mean over a synthetic log-uniform size distribution (17 B–256 KiB) is 9.2%. That is a synthetic number; real numbers come from trace histograms (E2).

Why 8 per doubling below 1 KiB: 98% of fleet objects are under 1 KiB [asplos24]. The cost is 16 more classes, each a potential partially filled page, so E2 decides whether this band stays.

**Lookup:**
- `s ≤ 1024`: `cls = lut[(s+15)>>4]`, a 65-entry byte table. That is 3 instructions.
- `s ≤ 256 KiB`: `b = 63 - clz(s-1); cls = 32 + 4*(b-10) + ((s-1) >> (b-2) & 3)`. That is about 6 instructions.

**Alignment.** Every class is a multiple of 16 (`alignof(max_align_t)` on x86-64/arm64 glibc). No 8-byte classes, because the default C++ new alignment is 16 on these targets [tc-tuning].

**Page kinds.** Lengths are generated per class by minimising tail waste under these constraints:
- at least 8 objects for classes up to 8 KiB,
- page length a multiple of `SL`,
- tail waste ≤ 12.5%.

| Kind | Page length | Classes | Worst tail |
|---|---|---|---|
| SMALL | 64 KiB | 16 B – 8 KiB | 6.25% (5 KiB, 6 KiB); ≤ 1 KiB: 0.98% |
| MEDIUM | 64 KiB – 320 KiB, per class (e.g. 24 KiB→128 KiB × 5; 40 KiB→128 KiB × 3; 160 KiB→320 KiB × 2) | 10 KiB – 256 KiB | 12.5% (14, 28, 56, 112, 224 KiB) |
| RUN | multiple of `SL`, page-heap allocated | > 256 KiB, < 32 MiB | ≤ 64 KiB virtual; resident tail ≤ `P` on first use (§11.3) |
| HUGE | dedicated mmap rounded to `H` | ≥ 32 MiB | ≤ `H` virtual |

On 64 KiB-page kernels the table is regenerated: SMALL pages have one base page, and lazy provisioning works at 64 KiB granularity.

**Lazy provisioning.** A page starts as a bump region. Free blocks are threaded one base page at a time (`provisioned`), so untouched base pages never become resident. This is PartitionAlloc's approach [pa]. A process touching 10 classes therefore uses 10 base pages of data, not 10 × 64 KiB.

---

## 5. Ownership model: concurrency slots

**Abstraction.** A *heap* owns pages. The fast path checks `pg->owner_tag == my_heap_tag`. There are three backends, all sharing that abstraction and the same remote-free protocol:

| Mode | Heap identity | Selected when |
|---|---|---|
| A. **vCPU slot** | `rseq.mm_cid` | glibc registered rseq (`__rseq_size > 0`, weak symbol), `getauxval(AT_RSEQ_FEATURE_SIZE) ≥ offsetof(mm_cid)+4 = 28` (kernel ≥ 6.3), and glibc ≥ 2.41 or auxval-confirmed [glibc-news][k-rseqh][tc-percpu] |
| B. **CPU slot** | `rseq.cpu_id` | rseq present but no mm_cid (kernel 4.18+ **UNVERIFIED primary**) |
| C. **Thread heap** | initial-exec TLS pointer | no rseq (seccomp, old kernel), or the `deterministic` profile |

If `__rseq_size == 0`, meaning musl, `glibc.pthread.rseq=0` or a failed registration, we self-register a 64-byte-aligned TLS area sized `max(AT_RSEQ_FEATURE_SIZE, 32)` on the thread's first call [k-rseqc][glibc-rseq]. We never register when glibc already has, never write kernel-owned fields (7.1 V2 kills the process [k-rseqdoc]), and never overlay `cpu_id_start` (the TCMalloc 6.19 breakage [lwn-hyrum]).

**Why mode A is the default.** On kernels before 6.19 the CID space is compacted toward the number of running threads [k-cid]. From 6.19 on, CIDs are per task while tasks ≤ allowed CPUs, otherwise per CPU, with `max_cids = min(1.25·min(nr_cpus_allowed, users), possible)` [k-sched][k-cidrw]. Either way, slot count follows concurrency. A 4-thread process on a 256-CPU host touches about 4–5 slots, where mode B can touch every CPU it migrates over.

**The flip side.** In per-task mode, sleeping threads keep their CIDs, so idle slots exist and must be reclaimed (§10.2). We do not claim CID slots are NUCA-local. LLC and node choice happens at refill using `cpu_id`/`node_id` read in the same rseq section.

**Slot struct (1 KiB, PARAM):**

```
struct slot {                         // slots[] sized by possible CPUs, VA in META, committed lazily
  page_meta *page[64];                // 512 B: current page per class (sentinel = &empty_page)
  struct { page_meta *pg; void *head; } rb[16];  // 256 B remote batch, direct-mapped by page
  _Atomic uint32_t lease;             // 0 | SLOWPATH(tid) | RECLAIMING
  uint32_t last_epoch;                // coarse 100 ms epoch of last slow path
  page_meta *partial[64];             // ≤2 extra partial pages/class, linked through pg->next (head only here)
  page_meta *pending;                 // MPSC: full pages that got remote frees
  ...
};
```

The sentinel `empty_page` has `free_head == 0`, so `malloc` before initialization, or into an unpopulated slot, falls into the slow path with no init check. This is snmalloc's static-default trick [sn-pagemap].

---

## 6. `malloc` fast path (mode A)

```
void *malloc(size_t n):
  if (n > 1024) goto mid_or_large;                  // 1 cmp
  c = lut[(n + 15) >> 4];                           // 2 ops + 1 load (n==0 maps to class 16 B: unique non-NULL)
  RSEQ_BEGIN(cs_malloc):                            // 1 store: rseq->rseq_cs = &cs_malloc
    cid = rseq->mm_cid;                             // 1 load (TP + __rseq_offset)
    sl  = slots_base + (cid << 10);                 // 1 shift+add
    pg  = sl->page[c];                              // 1 load
    w   = pg->free_head;                            // 1 load
    b   = w & PTR48;  if (!b) ABORT_TO(refill);     // 2 ops + branch
    nx  = *(uint64*)b ^ pg->key ^ (b >> 12);        // 1 load (block) + 1 load (key, same line) + 2 xor + shift
    if ((nx - pg->start) >= pg->len && nx) ABORT_TO(corrupt);   // ~4 ops (start/len same line)
    COMMIT: pg->free_head = nx | ((w & CNT) - (1ull << 48));    // 2 ops + 1 store  (single commit)
  RSEQ_END; rseq->rseq_cs = 0;                      // 1 store (glibc recommends clearing [glibc-rseq])
  return b;
```

**Cost (EST):** about 24 instructions, 6 loads (4 on the same 2 lines), 3 stores, 2 predictable branches. That is roughly 10–14 cycles with L1 hits, against mimalloc's "about 7 to 10 instructions" [mi-alloc]. Closing this gap is the main performance risk (Weakness 1).

**Mitigations:**
- In mode C the two rseq stores and the `cid` load disappear.
- In all modes, the same-page decode check moves to S1 if E10 shows it costs more than 1%. The weaker S0 check would then be "nx is 16-aligned", as in glibc [glibc-malloc].

**Remaining points:**
- The used count is implicit: `capacity − count(free_head) − bump_remaining − |xfree|`, computed only at collection. One commit store is therefore enough for rseq.
- Sampling has no cost on the fast path. It is decided at refill (§15).
- `mid_or_large`: the clz class path adds about 6 instructions for sizes up to 256 KiB, then runs the same code. Anything larger goes to §11.

---

## 7. `free` fast path (mode A)

```
void free(void *p):
  if (!p) return;
  [arm64: p = strip_tag(p)]
  lookup → pg (§3)                                   // ~7 ops, 3 dependent loads
  RSEQ_BEGIN(cs_free):
    cid = rseq->mm_cid;
    if (pg->owner_tag != (cid << 3)) ABORT_TO(not_local);  // flags in low bits force slow path:
                                                     //   FULL, ABANDONED, SPECIAL(RUN/SAMPLED/guarded)
    w = pg->free_head;  h = w & PTR48;
    if (p == h) ABORT_TO(double_free);               // cheap S0 check
    *(uint64*)p = h ^ pg->key ^ ((uint64)p >> 12);   // store into the block: idempotent on restart
    nw = (uint64)p | ((w & CNT) + (1ull << 48));
    COMMIT: pg->free_head = nw;
  RSEQ_END; rseq->rseq_cs = 0;
  if ((nw >> 48) + pg->bump_left == pg->capacity)  page_empty(pg);   // rare, outside rseq
```

**Cost (EST):** about 26 instructions, 3 stores, 6–7 loads.

**Sized free** (`free_sized`, sized `delete`) takes the same path. The size is a validation hint only, checked against `cls` at refill or collection time in S0 and on every call in S1. Size-based lookup does not help a page-local design, because the page is needed anyway [tc-msd]. Both the requested size and the size returned by `__size_returning_new` are accepted.

**errno** is saved and restored around every syscall reachable from `free` [glibc-replace].

---

## 8. Slow paths

**`refill(c)`**, entered when the current page's list is empty:
1. **Collect.** Atomically exchange `xfree` to empty (keeping the OWNED bit) and splice it into `free_head`, inside a short rseq commit. If the result is non-empty, return.
2. **Bump-provision** the next base page of the current page, if any is left.
3. Otherwise retire the current page. If it is full, set the FULL flag in `owner_tag` (it stays owned).
4. Take a page from `sl->partial[c]`. Among the at most 2 there, pick the fullest that is at least 1/8 free. Never take an exact "fullest-first", which hurts refill yield [critique-design].
5. Otherwise take from the node's **abandoned pool** for `c`. That is a locked list in 4 occupancy buckets (0–25, 25–50, 50–87.5, ≥ 87.5% used). Scan the fullest bucket that still has at least 1/8 free, then the others. This takes TCMalloc's occupancy ranking [tc-cfl] and mimalloc's 1/8 rule [mi-free].
6. Otherwise get a fresh page from the page heap (§11).
7. **Install** by preparing outside rseq, then committing `sl->page[c] = new` inside an rseq section that re-reads the cid. If the cid changed, retry with the new slot, so the page is never lost. TCMalloc uses the same prepare/commit split [tc-rseq].

**`not_local`:**
- **owner = me with the FULL flag:** clear FULL, push the block, and move the page into `partial[c]`. If partial already holds 2, abandon the least-occupied page to the pool.
- **owner = me with SPECIAL:** handle RUN, SAMPLED or guarded.
- **owner = someone else:** remote free (§9).

**`page_empty`.** Keep at most one empty page per class per slot, as a hot spare. Any further empty page goes back to the page heap immediately. The empty-page-found-immediately property holds because pages, not pointer batches, are the unit of transfer. v1's design critique flagged the object-batch versus page-flow conflict; this design uses only page flow, with no transfer caches.

---

## 9. Remote frees

**Channel.** There is exactly one channel: the page's `xfree` list, using mimalloc's protocol, including the OWNED bit [mi-free]. The sender batches frees by page before pushing:

```
remote(p, pg):
  RSEQ: e = &sl->rb[hash(pg) & 15];
        if (e->pg == pg) { *(p) = enc(e->head); COMMIT e->head = p; return; }   // 1 commit store
  // miss → outside rseq: evict e (CAS-push its chain onto e->pg->xfree in one CAS), install (pg, p)
```

**Flush points.** The batch is flushed on:
- every slow path of that slot,
- eviction,
- idle reclaim,
- fork prepare,
- a chain reaching 32 blocks, where the count is kept in the block.

**Bound.** Each slot holds at most 16 × 32 blocks, capped at 32 KiB total (PARAM).

**Evidence.** BatchIt measured −9.9% to −27.6% time on xmalloc-test and up to 20% on back-pressured message passing. It also measured 6–8% regressions elsewhere [batchit]. Experiment E6 decides whether batching is on by default or gated on a per-slot miss rate.

**Full and abandoned pages:**
- If the CAS sees the OWNED bit clear (page abandoned), the freer **adopts** it. The page joins the freer's slot `partial[c]`, or the pool if partial is full. Unowned pages therefore never strand frees [mi-free].
- If it is the first remote free into a FULL page (old `xfree` head empty and the FULL flag set), the page is also pushed once onto the owner slot's `pending` MPSC list, so a refill finds it.

---

## 10. Threads, idle reclaim, fork, init, signals

### 10.1 Thread lifecycle

In modes A and B, a thread owns nothing. Its exit leaves no stranded pages, which directly addresses the thread-churn failure class [mi-351][mi-1025].

In mode C, thread exit uses a `pthread_key` destructor registered at init. The key index is below 32, so `pthread_setspecific` does not allocate (**UNVERIFIED** against the glibc source revision). The destructor:
- returns empty pages,
- abandons partial pages to the pool,
- marks the heap ZOMBIE, so later frees from TLS destructors take the remote path.

### 10.2 Idle-slot reclaim (modes A/B): the fenced protocol

A slot whose `last_epoch` is at least 1 s old (PARAM) while it holds more than 64 KiB of free capacity is reclaimed by the maintenance tick (§13). Reclaim runs in batches of at most 64 slots, at most once per second:

```
for each victim v:   CAS v->lease 0→RECLAIMING   (slow paths that see it spin/yield; fast paths don't read it)
                     for c: v->page[c] = &empty_page;  for owned pages: owner_tag = UNOWNED|ABANDONED
membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, 0, 0)   // aborts/drains any in-flight cs on v
for each victim: collect free_head+xfree, flush rb[], return empty pages, abandon partials, lease=0
```

**Why it is safe.** After the fence, no in-flight critical section can still be committing against `v`'s pages. New sections see the sentinel and go to the slow path, which sees the lease.

**Cost.** One process-wide expedited membarrier per batch. With virtual CPU ids the target cannot be narrowed: TCMalloc's FenceCpu falls back to FenceAllCpus for this reason [tc-percpu]. The IPIs are bounded by the process's running threads, at most once per second.

**Registration and availability.** `MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ` is registered lazily the first time reclaim is needed. Its minimum kernel version is **UNVERIFIED**. Without it, idle reclaim is disabled, and the bound in §13.4 then includes idle slots.

**Verification.** The protocol must be model-checked (GenMC [genmc] or TLA+) and stress-tested under forced preemption before Phase 2 exits.

### 10.3 Fork

`pthread_atfork` is registered during the first init.

**prepare:**
- Increment a "fork pending" flag and wait for the PreventFork counter to reach 0. The counter is incremented only by multi-step lock-free sequences (remote CAS splices, lease claims), so the fast path has no cost [sn-fork].
- Flush the calling thread's slot remote batch.
- Take the page-heap shard locks, the pool locks and the stats lock, in a fixed order.

**child:**
- The child is single-threaded, so reclaim all slots other than the current one *without* a fence.
- Reset every MPSC `pending` list and lease.
- Re-draw keys for pages initialized from now on (existing keys are inherited, as documented for zygote-style forks).
- Mark the background thread dead, so it restarts lazily.
- Mark cgroup state stale.

**Test.** Fork from 64 threads that are in the middle of freeing, then check the child for leaks and deadlocks.

### 10.4 Initialization and bootstrap

**Before init.** All entry points work before the constructor runs: `rtab` is zero, so free goes to the foreign or ignore path, and malloc hits the sentinel and initializes. This covers glibc 2.44 calling `free(NULL)` early [mi-1341].

**TLS.** Initial-exec TLS only, at most 16 B per thread (one heap pointer plus a reentrancy counter). This respects glibc's rule [glibc-replace], the 2.39 dynamic-TLS recursion problem [glibc-news], and the 512 B surplus static TLS available for late `dlopen` [glibc-tun].

**Init syscalls:** `mmap` HEAP, `mmap` META, `getrlimit(RLIMIT_AS)`, and read `overcommit_memory` (open/read/close). Expected total is about 6 syscalls (EST), with no thread created. THP, cgroup and topology files are read lazily, the first time committed memory exceeds 4 MiB (PARAM).

### 10.5 Signals and reentrancy

`malloc` is not async-signal-safe (POSIX), but reentry must neither deadlock nor corrupt state:
- **Modes A/B:** rseq sections abort on signal delivery, so a nested fast path is safe. Nested slow paths are handled by a per-thread reentrancy counter. When it is non-zero, allocations come from a 64 KiB lock-free per-process emergency bump area, and frees are deferred onto an atomic list drained later.
- **Mode C:** the reentrancy counter covers the fast path too.

No signal is claimed by default.

---

## 11. Page heap, large and huge allocations

### 11.1 Hugeblock filler

The heap region is carved into hugeblocks of size `H`. Each hugeblock has a 32-byte descriptor in META, assuming `H/SL = 32` slices:

```
struct hb { uint32_t free_mask, released_mask, zero_mask; uint8_t longest_free, nalloc, set, flags;
            uint32_t last_used_epoch; hb *next, *prev; };   // 32 B
```

**Allocation of k slices.** Choose the hugeblock with the **smallest `longest_free` ≥ k**, breaking ties by **largest `nalloc`**. Freshly donated tails rank last. This is Temeraire's actual ordering, including its note that allocation count beats allocated pages [tc-hpf].

**Data structure.** Buckets are indexed by (longest_free 1..32) × (nalloc quantized into 4 levels). A 128-bit non-empty bitmap plus find-first-set makes the operation O(1).

**Two hugeblock sets:**
- **dense:** SMALL pages with more than 16 objects,
- **sparse:** MEDIUM pages, RUNs, pages with at most 16 objects.

This is the ASPLOS'24 capacity split, which gained +1.02% throughput and −0.82% memory fleet-wide [asplos24] at zero runtime cost.

**Sharding.** One shard and lock per NUMA node. The lock is held only for bitmap work and never across syscalls. On 7.0+ kernels with rseq V2, the lock holder requests a time-slice extension (`slice_ctrl.request`, 5 µs default) [k-rseqdoc]. This is gated on `RSEQ_CS_FLAG_SLICE_EXT_AVAILABLE` and goes through experiment E13.

### 11.2 Large runs (> 256 KiB, < 32 MiB)

**Multi-hugeblock runs.** Runs larger than `H` take contiguous hugeblocks from a per-region, address-ordered best-fit tree of free hugeblock extents, which coalesce on free. The slack in the last hugeblock is donated to the filler, which is Temeraire's HugeRegion idea, simplified [tc-regions].

**Per-slot run cache.** Each slot caches at most 2 runs of at most 1 MiB each (PARAM). It is flushed by idle reclaim and when the process is under pressure.

### 11.3 Run tails and realloc

**Run tails.** `req_or_aux` records the requested size. When a reused run is longer than the request by at least 2 × `P`, the dirty tail pages are queued for `DONTNEED` by the release engine rather than inline.

**`realloc` rules:**
- Within the same class, the pointer is returned unchanged.
- Growth of a RUN extends in place if the following slices are free. That is pure metadata, under the shard lock.
- Shrinking a RUN splits it, and the freed tail slices go to the release queue.
- Otherwise allocate, copy and free.
- `mremap(MREMAP_MAYMOVE)` is used **only** for HUGE objects, which own their VMA [mremap]. In-region `mremap` was rejected because moved anonymous VMAs "can almost never" merge [lwn-mremap], and because the kernel may relocate pages outside the region.

### 11.4 Aligned allocation

There are no interior pointers:
- **`a ≤ 16`:** normal path.
- **`16 < a ≤ 64 KiB`:** use the smallest class with `size ≥ n` and `size % a == 0`. Blocks sit at `page_start + i·size` and pages are 64 KiB-aligned, so the block is naturally aligned. A suitable class always exists: classes of 2^k·{1.25, 1.5, 1.75, 2} are multiples of 2^(k−2).
- **`64 KiB < a ≤ H`:** RUN with an aligned start slice.
- **Above that:** HUGE with over-map and trim.

The cost is extra internal waste for aligned requests. For example, `aligned_alloc(4096, 4097)` gets 8 KiB.

### 11.5 Huge objects (≥ 32 MiB)

- `mmap` rounded to `H`, registered in `rtab`, `munmap`ped on free.
- A cache keeps at most 2 recently freed mappings, at most 25% of live heap, for at most 2 s (PARAM), to avoid fault and zero storms. Scudo's secondary cache is the precedent [scudo-cfg].

---

## 12. OS and kernel layer

| Need | Primary call | Minimum kernel | Fallback |
|---|---|---|---|
| Reserve | `mmap(RW, NORESERVE)` / `PROT_NONE` (mode 2) | any | — |
| Commit (mode 2) | `mprotect` in 64 MiB steps | any | — |
| Prefault (latency profile, background) | `MADV_POPULATE_WRITE` (reports errors, no SIGBUS) | 5.14 [madvise] | touch |
| Batched release | `process_madvise(PIDFD_SELF, iov≤1024, MADV_DONTNEED, 0)` | self-advice any flag 6.13; `PIDFD_SELF` 6.15; one TLB flush per vector 6.16; per-VMA read lock 6.17 [k-madvise][k-pidfd][kr-kernel] | 6.13–6.14: cached `pidfd_open(getpid())` fd, re-validated with `fstat` against our own pidfd inode before each use (an app may close it). ≤ 6.12: coalesced `madvise`. Probe once on a scratch page; latch off on `EPERM`/`EINVAL`/`ENOSYS`/`EBADF`, as jemalloc does [je-pages] |
| Locked pages | `MADV_DONTNEED` → `EINVAL` | — | Latch "mlocked"; release only under pressure with `MADV_DONTNEED_LOCKED` (5.18) [k-dnl] |
| Guards | `MADV_GUARD_INSTALL` once at region creation, at hugeblock boundaries or in the GUARD region only | 6.13 [madvise] | 2 one-time `PROT_NONE` guards for META; none elsewhere in S0 (log once). Never `MADV_POPULATE` over guards [madvise] |
| THP | §14 | — | — |
| Collapse | `MADV_COLLAPSE`, background thread only | 6.1 | skip |
| VMA names | `prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME)` on whole VMAs | 5.17 [k-vmaname] | skip |
| NUMA | `mbind(MPOL_PREFERRED)` on a whole per-node region, only if the process mempolicy is default (`get_mempolicy`) | — | no binding; never on sub-ranges [k-numa] |
| CPU / node / cid | rseq fields via `__rseq_offset` | 4.18 / 6.3 | `sched_getcpu`, thread heaps |
| Pressure | cgroup v2 files (§13) | memory.events notify since cgroup v2; unprivileged PSI 6.4 with 2 s window multiples [k-psi] | time-only decay |
| Seal | `mseal` on META's guard and secret-key page only, never on heap memory [k-mseal] | 6.10 | skip |

**Syscalls are never issued while holding allocator locks.** The release engine marks slices RELEASING under the lock, unlocks, makes the syscall, then relocks to mark them released. Released private anonymous slices are **known-zero**, so `calloc` skips `memset` on them. MADV_FREE'd slices, and slices whose `DONTNEED` failed, are not known-zero.

**TLB note.** The cost of IPI shootdowns is x86-specific: arm64 uses broadcast TLBI [k-arm64tlb], and AMD INVLPGB, merged in 6.15, avoids IPIs for mms active on at least 3 CPUs [lwn-invlpgb]. Batching still saves page-table walks and lock round-trips on every architecture.

---

## 13. Memory-release policy

### 13.1 Quantities (page heap, per node shard)

| Symbol | Meaning |
|---|---|
| `U` | slices allocated to pages or runs (bytes) |
| `F` | committed, resident-assumed free slices (dirty) |
| `D_s` | max `U` over the trailing 10 s, from a ring of 10 one-second maxima |
| `D_l` | max `U` over 5 min, from a ring of 30 ten-second maxima |
| `L` | effective cgroup limit = min over ancestors of `memory.high` and `memory.max`. Only meaningful for non-root cgroups, read up to the cgroup-namespace root; ∞ if unknown [k-cg2] |
| `h` | headroom = `(L − memory.current)/L` |

### 13.2 Retention target

```
R_cap = max(2 MiB, U/8)                                    (PARAM)
R     = min(max(D_s − U, 0), R_cap)                        pressure level p0: h ≥ 25% or L = ∞
R     = min(max(D_s − U, 0), 2 MiB)                        p1: 10% ≤ h < 25%
R     = 0                                                  p2: h < 10%, or memory.events high/max/oom increased
R     = 0  if no page-heap activity for T_idle = 30 s      (idle desktop / idle server)
excess X = F − R; release only if X ≥ 1 hugeblock, or p ≥ p1
```

### 13.3 What is released, in order

1. Fully free hugeblocks, oldest `last_used_epoch` first. One iovec entry each.
2. Free slices inside partially used hugeblocks, only if:
   - THP is not backing the block (THP=never, or the block was previously sub-released), or
   - `p ≥ p1`, or
   - the block has stayed below 25% used for the whole `D_l` window.

   This is skip-subrelease in spirit [tc-sub]. Partial release inside a PMD-mapped THP only queues a deferred split, so it frees memory slowly [k-thp].
3. Run tails (§11.3).

**Front-end shrinking under p1/p2:**
- Spare empty pages are dropped.
- Partial caps fall to 1, then 0.
- Remote batches are flushed.
- Idle-slot reclaim runs immediately (threshold 100 ms instead of 1 s).

### 13.4 Primitive

The primitive is `MADV_DONTNEED` by default, everywhere. Go moved back from MADV_FREE because of monitoring confusion [go116][go-42330]. MADV_FREE pages stay in RSS and show as `LazyFree` [k-proc]. MADV_FREE is offered only in the `throughput` profile, and even there only when `h ≥ 25%`, for blocks that have been free for less than 2 s. Its use is reported separately.

**Rate limit.** Time spent in release syscalls is capped at 1% of wall time per second, following Go's scavenger budget [go-scav]. Under p2 the cap is lifted.

**Front-end overhead bound.** Asserted in debug builds and reported in stats:

```
FE ≤ Σ_active slots [ (#classes touched)·(1 spare page + 2 partial pages) + 32 KiB remote batch + 2 MiB run cache ]
```

Idle slots drop to zero within 1 s plus a tick when the membarrier is available. I make no Robson-beating claim for partial pages. Robson's Θ(log M) bound applies to placement within address space. Page-granular release is exactly the request-fragmentation escape analysed in [bender26]: releasing whole free slices lowers RSS at the cost of a larger fragment count k, meaning page tables and TLB.

### 13.5 Who runs the policy

1. **Inline trigger, in any process.** In the free slow path, if `X > max(32 MiB, U/4)` (PARAM), release synchronously, rate-limited and outside locks. This covers single-threaded "spike, then idle" programs, which have no timer.
2. **Background thread, started lazily.** It starts only when the process already has at least 2 threads *and* `F > 16 MiB` for more than 1 s. This keeps `unshare(CLONE_NEWUSER)` working for single-threaded programs [unshare].
   - It ticks every 1 s while there is anything to release.
   - Otherwise it blocks on a futex, woken by slow paths when `F` grows. Idle processes get zero wakeups.
   - Every tick it samples `memory.current` with `open/read/close`. The fd is not kept open: a daemon that closes all fds could hand the number to another file.
   - It watches `memory.events` via inotify, and creates a cgroup `memory.pressure` trigger (`some 150000 2000000`) only when the PSI file exists. The default thresholds are PARAM; the 2 s window is the unprivileged minimum [k-psi].
   - `memory.stat` is never read, because of the rstat-flush cost [lwn-rstat].
3. **Slow-path sampling** when there is no thread: `memory.current` at most every 100 ms, checked by vDSO `clock_gettime` every 64 slow paths.
4. **Explicit release:** `soham_release(level)`, and `malloc_trim` mapped onto it.

---

## 14. THP policy

| sysfs `enabled` / `defrag` | Action |
|---|---|
| `never`, or `PR_GET_THP_DISABLE` set | Nothing. Hugeblocks remain release units. `MADV_COLLAPSE` is never used, as user intent wins even though collapse ignores sysfs [madvise] |
| `madvise` with defrag `madvise` (the default) [k-thp] | **No** `MADV_HUGEPAGE`, because first-touch faults enter direct reclaim (Go's 100+ ms stalls [go-61718]). The background thread may `MADV_COLLAPSE` hugeblocks that have been at least 95% used for at least 10 s (jemalloc HPA defaults [je-hpa]), at most 4 per second. After an `EAGAIN` it retries at most 3 times [tc-sysalloc], and it backs off for 60 s when a collapse takes more than 10 ms (PARAM) |
| `madvise` with defrag `defer` or `never` | `MADV_HUGEPAGE` once on the whole HEAP region at creation (one write lock, no split) |
| `always` (incl. `PR_THP_DISABLE_EXCEPT_ADVISED`, 6.18 [k-thp]) | Nothing per range. Make the region's first hugeblock (the bootstrap block) `MADV_NOHUGEPAGE` once at creation (+1 VMA). Small processes then pack into it and avoid a 2 MiB first-touch fault |

The `always` row supersedes v1's "MADV_HUGEPAGE on the heap" and its claim that MADV_COLLAPSE is unused (TCMalloc, jemalloc HPA and mimalloc v3 are all hugepage-aware [tc-hpf][je-hpa][mi-readme]).

**Interaction with khugepaged and the shrinker:**
- If `khugepaged/max_ptes_none > 255`, any sub-release inside a THP-eligible block is treated as temporary, because khugepaged will re-collapse it [k-khp]. Whole-hugeblock release is preferred.
- The underused-THP shrinker (6.12) splits THPs whose zero subpages exceed `max_ptes_none` [k-thp]. We never zero-on-free inside THP-eligible blocks: S0 has no zeroing, and S1 zeroes only blocks of at most 1 KiB.

**Measurement.** Coverage is measured from `AnonHugePages` in `smaps_rollup`, on demand only.

---

## 15. Security tiers

| Tier | Mechanism | Expected cost (to be measured per feature, E11) |
|---|---|---|
| **S0** (default) | Out-of-line metadata in the META VMA with one-time guards and random offset. Inline links encoded `next ^ page_key ^ (addr >> 12)`: a per-page key re-drawn at each page init, plus glibc-style address mixing (safe-linking itself uses no secret [glibc-malloc]). Same-page decode check. Head ≠ block double-free check. Full count and alignment validation at collection, where a count above capacity means abort. Sized-free and class mismatches checked at collection. `operator new`/`free` origin is not tracked. GWP-ASan-style sampling in the GUARD region | Encoding: glibc reports "not measurable" [cp-safelink]. Sampling: zero fast-path cost, about 32 slots × 2 pages |
| **S1** (`secure`) | Bitmap-allocated pages, where the bitmap *replaces* the free list, hardened_malloc-style [hm-readme], so cross-thread double frees are caught with an atomic OR. Window-of-8 randomized slot order. Zero-on-free for blocks ≤ 1 KiB, with a write-after-free check [xzone]. 2–4 call-site buckets for classes ≤ 1 KiB. AllocToken pointer/data split [alloctoken]. Per-class VA sequestering. Byte-budgeted quarantine (≤ 1% of heap). Guard slab every 8 slabs, only via `MADV_GUARD_INSTALL`, only in the GUARD region | Hypothesis 5–10% CPU and 2–6% RSS. Analogues: snmalloc < 5% [sn-sec], mimalloc secure about 10% [mi-readme]; not measured |
| **S2** (arm64 MTE) | `PROT_MTE` set at reservation. Tag blocks ≤ 32 KiB. Retag on free ≤ 4 KiB, on alloc 4–32 KiB. Exclude neighbours' tags and 0. Retag after purge. Mode chosen by the kernel's per-CPU preference; `secure` forces SYNC [k-mte][mte-perf][xzone] | 1.08–1.18× async; 1.56× sync on Cortex-X3 [mte-perf] |

**Sampling at refill.** One refill in N (N = 64, PARAM) dedicates its first allocation to a guarded slot. This approximates byte-rate sampling with zero fast-path cost, at the price of bias toward post-refill allocations.

**Signal handler.** The SIGSEGV handler is installed only if no handler exists, and it chains to the previous one [gwp-llvm].

**Why S0 is memory-cheap.** S0 deliberately omits type and data buckets, VA sequestering and the bitmap. Each costs RSS: more partially filled pages, unpackable hugeblocks, and 512 B per 64 KiB for the bitmap [snmalloc-paper]. In the memory-first default that cost is not paid. This is a stated trade-off against the security report's S0, not an oversight.

---

## 16. API, observability, configuration

### 16.1 API

**Drop-in surface (glibc-compatible semantics):**
- `malloc`, `free`, `calloc` (with `__builtin_mul_overflow`), `realloc`, `reallocarray` (built on our realloc; it is not exported as a replacement point [glibc-replace]), `aligned_alloc`, `posix_memalign`, `memalign`, `valloc`/`pvalloc` (runtime `P`), `malloc_usable_size`, `free_sized`, `free_aligned_sized`.
- `mallopt` accepts and ignores; `mallinfo2`/`mallinfo` are filled from stats; `malloc_trim` calls `soham_release`; `malloc_stats` and `malloc_info` (XML) are provided.

**Semantics:**
- Sizes above `PTRDIFF_MAX` are rejected with `ENOMEM`.
- `malloc(0)` returns a unique 16 B block.
- `realloc(p, 0)` frees and returns NULL, as glibc does.
- A failed `realloc` keeps the old block.
- `posix_memalign` returns `EINVAL` per its man page.
- `aligned_alloc` accepts a size that is not a multiple of the alignment, as glibc does [glibc-news].

**C++.** A separate C++ translation unit provides all replaceable `operator new`/`delete` variants, including the new_handler loop and `bad_alloc` [cxx-new], plus `__size_returning_new` and the `__hot_cold_t` overloads (ABI-compatible with TCMalloc [tc-mallocext]). Allocations made with the hot/cold hint go to the sparse hugeblock set.

**Extensions:**
- `soham_release(level)`, `soham_stats(buf, len)` (versioned JSON), `soham_explain(fd)`.
- `soham_thread_idle()`: flush hook for thread pools.
- `__alloc_token_*` entry points, used only in S1.

### 16.2 Explain report

The report gives two views and never a fabricated sum:
- **Allocator view:** live bytes, rounding waste, tail waste, per-layer cached free (slots / pools / page heap / run caches / remote batches), metadata, committed, released-but-reserved, mode (A/B/C), kernel features latched.
- **Kernel view**, read on demand from `/proc/self/smaps_rollup`: `Rss`, `AnonHugePages`, `LazyFree`.

The difference between the two is labelled "not attributable".

**Triggers:**
- the API,
- `SOHAMALLOC_REPORT=exit` at exit, written to stderr.

File paths are never taken from the environment. Report code does not allocate and writes with raw `write`.

**Tooling.**
- **Valgrind:** `MALLOCLIKE`/`FREELIKE` client requests on the extended entry points [valgrind].
- **Tracing:** USDT probes on refill, release, reclaim and collapse.
- **Sanitizer builds:** a no-override build for ASan/TSan.

### 16.3 Configuration

`SOHAMALLOC=<profile>` selects a profile:

| Profile | Effect |
|---|---|
| `default` | as specified above |
| `lean` | `R_cap = 2 MiB`, `T_idle = 5 s`, partial cap 1 |
| `throughput` | MADV_FREE stage, `R_cap = U/4`, MADV_HUGEPAGE even under defrag `madvise` |
| `secure` | S1 |
| `deterministic` | mode C, fixed seeds, decay counted in allocations instead of time, no thread |

`SOHAMALLOC_OPTS` holds individual PARAMs.

**Secure execution.** Under `AT_SECURE`, every option except those that only increase security is ignored [getenv].

---

## 17. Portability

| Target | Plan |
|---|---|
| Primitive layer | reserve, commit, decommit, release(vector), guard, page/hugepage size, slot id, TLS. Modelled on mimalloc's prim layer [mi-readme] |
| musl | Self-registered rseq; export `aligned_alloc`, or musl's returns `ENOMEM` [musl-glue] |
| arm64 16K/64K | Geometry tables are generated at init; CI runs on QEMU with 16K/64K kernels |
| macOS / Windows | Later. Zone registration [mi-zone]; reserve/commit with commit charge (§2.3 mode-2 logic maps onto it) |
| 32-bit / wasm | Non-goal for v2; regions shrink to 64 MiB |

---

## 18. Failure modes

| Failure | Behaviour |
|---|---|
| Region `mmap` fails (`RLIMIT_AS`, VA exhausted) | Halve the region size, retry down to 64 MiB, then NULL/`ENOMEM` |
| Mode 2 `mprotect` fails with `ENOMEM` | NULL/`ENOMEM`; the C++ new_handler is invoked |
| `max_map_count` near its limit (budget = limit − current count, read lazily) | Stop creating HUGE VMAs: serve them as RUNs in a new region. Drop guards |
| Free-list decode fails, or count exceeds capacity | Abort with page, class and a hex dump, written by raw `write` |
| `process_madvise` partial success | Advance the iovec by the returned byte count; latch off on hard errors |
| seccomp blocks `membarrier` / `rseq` / `open` | Idle reclaim off, mode C, time-only decay. Shown in stats |
| Pointer from a `RTLD_DEEPBIND` library's other allocator [glibc-30186] | Foreign path (§3). Diagnostic in S1 |

---

## 19. Decision register and experiments

| # | Decision | Alternatives rejected (why) | Cost / risk | Validating experiment |
|---|---|---|---|---|
| D1 | Page-based flow; caches = slot-owned pages | TCMalloc object batches + transfer caches (incompatible with page-local lists; strands pages) [critique-design] | One more page per class per slot | E4: RSS vs TCMalloc per-CPU mode on Redis (`MALLOC=libc` build) and RocksDB `db_bench` |
| D2 | mm_cid slots (mode A) as default | Per-thread heaps (churn stranding); `cpu_id` (touches all CPUs migrated over) | rseq asm per architecture; membarrier IPIs for reclaim | E7: churn benchmark across modes A/B/C on 6.12 and 6.19+, at (4 threads, 256 CPUs), (10k threads, 64 CPUs), (2-CPU cpuset) |
| D3 | 1 GiB regions + `rtab` + delta map | mimalloc arithmetic (inline meta, forwarders); TCMalloc 3-level radix (one more load) | +1 dependent load | E3 |
| D4 | 64 classes (8/doubling 128 B–1 KiB) | 4/doubling everywhere (52 classes, worse waste in the hottest band) | More partially filled pages | E2: trace histograms (clang, CPython, Redis, RocksDB), RSS vs 52-class table |
| D5 | DONTNEED + `process_madvise` | MADV_FREE default (RSS dishonesty [go-42330]); per-range `madvise` | Refault and zeroing on reuse | E5: refault rate, faults per second, release syscalls per GiB |
| D6 | Demand-window retention + cgroup levels | Fixed decay (jemalloc); app-driven release (TCMalloc) | Wrong windows cause churn | E8: phased workload (ramp/steady/burst/60 s idle) under `memory.max` = 1.2× working set, RSS sampled every 100 ms |
| D7 | No runtime VMA changes; THP matrix §14 | MADV_HUGEPAGE everywhere (stalls, VMA splits) | Less THP coverage under defrag=madvise | E9: dTLB walk cycles, AnonHugePages, first-touch p99 under THP always/madvise/never with `max_ptes_none` 0/511 |
| D8 | Sender batching by page | Unbatched CAS (mimalloc); snmalloc message queues (second channel) | Delayed reuse; ≤ 32 KiB per slot | E6: xmalloc-test + back-pressured producer/consumer [batchit] |
| D9 | S0 = encoding + lazy checks | Bitmap / type buckets by default | Weaker default than Scudo or hardened_malloc | E11: per-feature ablation against a "tier −1" build |
| D10 | Lazy thread, futex-idle | Always-on thread; PSI only | Single-threaded idle processes release only through the inline trigger | E8 single-threaded variant |
| D11 | No fast-path lifetime predictor; capacity split + hints | Return-address hash (22.5 ns lookup in LLAMA vs an 8.3 ns fast path; wrappers collapse sites) [llama] | Lost potential packing gains | Later: large-object predictor in counterfactual mode only [tc-lifetime] |

**Harness rules.** Pinned versions: glibc 2.44, mimalloc v3.4.5, jemalloc 5.4.0, current TCMalloc with per-CPU mode verified active, snmalloc, Scudo. Speed and memory use the same baseline. At least 10 runs with bootstrap CIs. Layout randomization [stabilizer]. Constant-rate tail latency, wrk2-style [wrk2]. Kernel matrix 6.1 / 6.6 / 6.12 / 6.18 / 6.19 / 7.x; glibc 2.35–2.44; musl.

**E1.** Startup RSS and syscalls for `/bin/true`, plus 10k fork+exec runs.

**E10.** mimalloc-bench geomean.

**E12.** `mmap_lock` acquisitions measured with lock tracepoints.

**E13.** Time-slice extension on/off for shard-lock hold times.

---

## 20. Critique faults and where they are fixed

| Fault | Fixed in |
|---|---|
| mm_cid novelty and semantics; glibc `__rseq_size` history | §0, §5, §10.2 |
| No pagemap | §3 |
| 64 GiB writable reservation, overcommit, `RLIMIT_AS` | §2.3 |
| Huge objects and VA exhaustion | §11.5, §18 |
| `mremap` contradiction | §11.3 |
| VMA splitting | §2.4, §12 |
| THP fights | §14 |
| Hard-coded geometry | §2.1 |
| 12.5% waste error; page tail waste | §4 |
| Incompatible flow models | §8, D1 |
| Slot rseq protocol | §6, §7, §10.2 |
| Predictor cost and keying | §0, D11 |
| Lifetime classes multiplying partially filled pages | dropped |
| Fullest-first and Temeraire ordering | §8, §11.1 |
| Adaptive remote switch | §9 |
| "Out-of-line" contradiction, bitmap duplication, safe-linking misdescription | §15 |
| Guards vs THP and mlock | §12 |
| DONTNEED vs FREE, batching | §13.4 |
| Idle programs, hidden thread | §13.5 |
| PSI / cgroup specifics | §13 |
| Fork | §10.3 |
| API semantics, interposition, C++ | §16 |
| Signals / TLS | §10.4–§10.5 |
| Profiler unwinding | sampled path only, frame-pointer walker, bounded |
| Report honesty | §16.2 |
| `secure_getenv` | §16.3 |
| Unmeasured tier costs | E11 |
| Falsifiable goals and roadmap | §1, §19 |
| Meshing | dropped |
| Front-end bound | §13.4 |
| Prefault | §12 |
| Self-registration | §5 |
| Page-heap concurrency | §11.1–§11.2 |

**Roadmap (reordered):**
- **P0:** harness + trace recorder + geometry + regions/pagemap + mode C + S0 + conformance/fuzz suite. Exit: glibc malloc tests pass and 24 h of fuzzing is clean.
- **P0.5:** offline trace simulator for classes, retention windows and occupancy policy.
- **P1:** single-threaded performance.
- **P2:** modes A/B, remote frees, model-checked reclaim.
- **P3:** release engine, THP, cgroup.
- **P4:** S1/S2, profiler.
- **P5:** large-object lifetime counterfactual.

---

## 21. Top 10 weaknesses of this design

1. **The fast path is longer than mimalloc's.** About 24 instructions versus their "7 to 10" [mi-alloc]. That comes from the 3-load lookup, the rseq bookkeeping stores, decode and the same-page check. "Within a few %" is unproven. If E10 fails, the same-page check and the address mixing move to S1 and the rseq clear store is dropped, weakening S0.
2. **Correctness hinges on hand-written rseq assembly per architecture plus a fenced reclaim protocol.** That is the most bug-prone part of the design. It changed under TCMalloc in 6.19 [lwn-hyrum] and in 7.1 (V2 enforcement). A single error corrupts the heap silently.
3. **Idle reclaim depends on `MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ`.** Its availability is **UNVERIFIED**, it costs process-wide IPIs, and seccomp may block it. Without it, sleeping threads in 6.19 per-task CID mode strand their slots' pages, which is the very problem D2 claims to solve. Mode C has no cross-heap reclaim at all.
4. **Page meta layout causes false sharing.** `xfree` and `free_head` share one cache line, so remote CASes bounce the owner's line. Batching mitigates this, but a 128 B meta (0.2% memory) may be needed. E6 decides.
5. **The retention constants are guesses.** The 10 s and 5 min windows, the 30 s idle timeout, the 25%/10% headroom bands, the 32 MiB inline trigger and the 1 s slot idle threshold all need validation. Wrong values either churn faults (D5 refault cost) or keep RSS high. Trace replay cannot validate time-based policy, because glibc traces carry no timestamps [glibc-trace].
6. **Single-threaded idle processes release only through the inline trigger.** A program that frees 20 MiB in small steps and then sleeps can sit below the trigger indefinitely. Without a thread or a timer signal there is no fix.
7. **THP coverage is likely below Temeraire's under default distro settings.** We refuse `MADV_HUGEPAGE` under defrag=`madvise` and collapse only from a background thread. That costs dTLB performance relative to TCMalloc's THP=always assumptions [tc-tuning]. Memory is deliberately prioritized here.
8. **The S0 default is weaker than Scudo, hardened_malloc or Apple's typed allocators.** It has no type isolation, no VA sequestering, no cross-thread double-free detection until collection, and inline (encoded) links remain the target class how2heap keeps exploiting [how2heap]. This is a conscious memory-for-security trade that some users will reject.
9. **Some allocation shapes are paid for in waste:**
   - aligned allocation through class search (up to about 2×, e.g. 4097@4096 → 8 KiB),
   - run slack up to 64 KiB virtual,
   - 16 B granularity below 128 B (46.9% worst),
   - no 8-byte classes.
   Programs with many 24 B or 40 B objects pay 33% or 20% more than an 8 B-granular allocator.
10. **Mode-2 commit charge is effectively never returned from the middle of a region.** It is returned only from the top, in 64 MiB extents. NUMA per-node regions also multiply cached memory per node, with only a global bound, and topology detection may be masked in containers. The design is also large: 4 region kinds, 3 slot modes, 2 hugeblock sets and 3 security tiers. That is exactly the accumulated complexity that sank jemalloc's HPA [je-postmortem]. The test matrix in §19 is the only defence, and it is expensive.

---

## References

[mi-free] https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/free.c · [mi-alloc] https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/alloc.c · [mi-pagemap] https://github.com/microsoft/mimalloc/blob/dev3/include/mimalloc/heap/page-map.h · [mi-readme] https://raw.githubusercontent.com/microsoft/mimalloc/main/readme.md · [mi-351] https://github.com/microsoft/mimalloc/issues/351 · [mi-1025] https://github.com/microsoft/mimalloc/issues/1025 · [mi-1341] https://github.com/microsoft/mimalloc/issues/1341 · [mi-zone] https://raw.githubusercontent.com/microsoft/mimalloc/main/src/prim/osx/alloc-override-zone.c
[tc-percpu] https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/percpu.cc · [tc-rseq] https://github.com/google/tcmalloc/blob/master/docs/rseq.md · [tc-design] https://github.com/google/tcmalloc/blob/master/docs/design.md · [tc-tuning] https://github.com/google/tcmalloc/blob/master/docs/tuning.md · [tc-hpf] https://github.com/google/tcmalloc/blob/master/tcmalloc/huge_page_filler.h · [tc-sub] https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/huge_page_subrelease.h · [tc-regions] https://github.com/google/tcmalloc/blob/master/docs/regions-are-not-optional.md · [tc-cfl] https://github.com/google/tcmalloc/blob/master/tcmalloc/central_freelist.h · [tc-msd] https://github.com/google/tcmalloc/blob/master/docs/mismatched-sized-delete.md · [tc-mallocext] https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/malloc_extension.h · [tc-sysalloc] https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/system_allocator.h · [tc-lifetime] https://github.com/google/tcmalloc/blob/master/docs/lifetime-based-allocator.md
[asplos24] https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf · [batchit] https://microsoft.com/en-us/research/uploads/prodnew/2024/05/preprint_batchit.pdf · [llama] https://cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf · [bender26] https://arxiv.org/abs/2608.28462 · [snmalloc-paper] https://www.microsoft.com/en-us/research/uploads/prod/2020/04/snmalloc.pdf · [stabilizer] https://people.cs.umass.edu/~emery/pubs/stabilizer-asplos13.pdf · [wrk2] https://github.com/giltene/wrk2 · [genmc] https://raw.githubusercontent.com/MPI-SWS/genmc/master/README.md
[je-pages] https://raw.githubusercontent.com/jemalloc/jemalloc/dev/src/pages.c · [je-hpa] https://github.com/jemalloc/jemalloc/blob/dev/include/jemalloc/internal/hpa_opts.h · [je-man] https://raw.githubusercontent.com/jemalloc/jemalloc/dev/doc/jemalloc.xml.in · [je-postmortem] https://jasone.github.io/2025/06/12/jemalloc-postmortem/
[sn-fork] https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds_aal/prevent_fork.h · [sn-pagemap] https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds/pagemap.h · [sn-sct] https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds/sizeclasstable.h · [sn-sec] https://github.com/microsoft/snmalloc/blob/main/docs/security/README.md · [pa] https://github.com/chromium/chromium/blob/main/base/allocator/partition_allocator/PartitionAlloc.md · [scudo-cfg] https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/scudo/standalone/allocator_config.h · [hm-readme] https://github.com/GrapheneOS/hardened_malloc/blob/main/README.md · [xzone] https://cs.iossec.tech/libmalloc/latest/source/doc/xzone_malloc.md · [alloctoken] https://clang.llvm.org/docs/AllocToken.html · [gwp-llvm] https://llvm.org/docs/GwpAsan.html · [mte-perf] https://arxiv.org/abs/2601.11786 · [how2heap] https://github.com/shellphish/how2heap · [cp-safelink] https://research.checkpoint.com/2020/safe-linking-eliminating-a-20-year-old-malloc-exploit-primitive/
[glibc-malloc] https://sourceware.org/git/?p=glibc.git;a=blob;f=malloc/malloc.c;hb=HEAD · [glibc-news] https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=NEWS;hb=HEAD · [glibc-replace] https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html · [glibc-rseq] https://sourceware.org/glibc/manual/latest/html_node/Restartable-Sequences.html · [glibc-tun] https://raw.githubusercontent.com/bminor/glibc/master/manual/tunables.texi · [glibc-30186] https://sourceware.org/bugzilla/show_bug.cgi?id=30186 · [glibc-trace] https://sourceware.org/glibc/wiki/MallocTracing · [musl-glue] https://git.musl-libc.org/cgit/musl/tree/src/malloc/mallocng/glue.h · [cxx-new] https://eel.is/c++draft/new.delete.single · [getenv] https://man7.org/linux/man-pages/man3/getenv.3.html · [valgrind] https://valgrind.org/docs/manual/manual-core.html
[k-cid] https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/patch/?id=af7f588d8f7355bc4298dd1962d7826358fc95f0 · [k-cidrw] https://lkml.iu.edu/2511.2/08641.html · [k-sched] https://github.com/torvalds/linux/blob/master/kernel/sched/core.c · [k-rseqh] https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/rseq.h · [k-rseqc] https://github.com/torvalds/linux/blob/master/kernel/rseq.c · [k-rseqdoc] https://docs.kernel.org/userspace-api/rseq.html · [lwn-hyrum] https://lwn.net/Articles/1070072/ · [k-madvise] https://raw.githubusercontent.com/torvalds/linux/master/mm/madvise.c · [madvise] https://man7.org/linux/man-pages/man2/madvise.2.html · [k-pidfd] https://raw.githubusercontent.com/torvalds/linux/v6.15/include/uapi/linux/pidfd.h · [kr-kernel] https://kernelnewbies.org/Linux_6.16 · [k-dnl] https://lkml.iu.edu/hypermail/linux/kernel/2203.0/05125.html · [k-thp] https://docs.kernel.org/admin-guide/mm/transhuge.html · [k-khp] https://github.com/torvalds/linux/blob/master/mm/khugepaged.c · [k-arm64hp] https://docs.kernel.org/arch/arm64/hugetlbpage.html · [k-arm64mem] https://docs.kernel.org/arch/arm64/memory.html · [k-arm64tlb] https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm64/include/asm/tlbflush.h · [lwn-invlpgb] https://lwn.net/Articles/1008065/ · [k-5lvl] https://docs.kernel.org/arch/x86/x86_64/5level-paging.html · [k-overcommit] https://docs.kernel.org/mm/overcommit-accounting.html · [k-mmapc] https://github.com/torvalds/linux/blob/master/mm/mmap.c · [k-vmac] https://github.com/torvalds/linux/blob/master/mm/vma.c · [mmap] https://man7.org/linux/man-pages/man2/mmap.2.html · [mremap] https://man7.org/linux/man-pages/man2/mremap.2.html · [lwn-mremap] https://lwn.net/Articles/1015762/ · [k-vmaname] https://man7.org/linux/man-pages/man2/pr_set_vma.2const.html · [k-numa] https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html · [k-mseal] https://docs.kernel.org/userspace-api/mseal.html · [k-mte] https://docs.kernel.org/arch/arm64/memory-tagging-extension.html · [k-psi] https://docs.kernel.org/accounting/psi.html · [k-cg2] https://docs.kernel.org/admin-guide/cgroup-v2.html · [lwn-rstat] https://lwn.net/Articles/951695/ · [k-proc] https://docs.kernel.org/filesystems/proc.html · [unshare] https://man7.org/linux/man-pages/man2/unshare.2.html
[go116] https://go.dev/doc/go1.16 · [go-42330] https://github.com/golang/go/issues/42330 · [go-61718] https://github.com/golang/go/issues/61718 · [go-scav] https://raw.githubusercontent.com/golang/go/master/src/runtime/mgcscavenge.go · [node-51868] https://github.com/nodejs/node/issues/51868 · [hadoop] https://issues.apache.org/jira/browse/HADOOP-7154 · [critique-design] v1 design critique (supplied to this task; it is not a public source)

Size-class figures (worst case 46.9% / 10.9% / 20.0%, synthetic log-uniform mean 9.2%, page tails ≤ 12.5%) were computed by `a local size-class script (sc.py)`.