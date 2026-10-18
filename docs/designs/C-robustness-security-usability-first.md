# sohamalloc v2: robustness-, security- and usability-first design

**Snapshot:** 2026-10-18
**Scope:** Linux x86-64 and arm64, glibc and musl.
**Status:** design. Every performance number below is either cited or marked **[estimate]** (counted from my own pseudo-code) or **[derived]** (my arithmetic). Nothing here has been measured on sohamalloc yet.

---

## 0. Summary and what changed from v1

**The design in one paragraph.** sohamalloc v2 is a page-based allocator in which the owner frees its own blocks, in the mimalloc family. It makes one unusual choice: **small and medium objects have no inline metadata at all.**
- Each page keeps two out-of-line bitmaps: a *local* free bitmap written only by the owner, and a *remote* free bitmap that other threads update with `atomic_fetch_or`.
- `malloc` pops a bit from a 64-bit word cached in the thread's heap, using `tzcnt` and `blsr`. The commit is one store, and the object's memory is never read.
- `free` finds the page through a two-level, range-checked page map. It checks that the pointer lands exactly on a slot boundary and that the slot is not already free, then sets the bit.

**What that buys:**
- Free-list poisoning cannot happen, because there is no free list.
- Invalid frees and double frees are detected deterministically in the default tier.
- Fresh pages are never dirtied by the allocator.
- Remote frees need no CAS loop.
- The pop commits with a single store, so a later rseq per-vCPU mode can reuse the same fast path unchanged.

**What it costs:**
- 2 bits of metadata per slot (1.56% for 16 B slots) **[derived]**.
- About 3× the instruction count of mimalloc's free fast path **[estimate]**.
- Address-ordered rather than LIFO reuse inside a page.

Section 19 covers these costs and the other weaknesses honestly.

**Fixes to v1's critical and major faults:**

| v1 fault (critique) | v2 fix (§) |
|---|---|
| No pointer-to-metadata lookup | 2-level, range-checked region/page map with 16 B entries; foreign-pointer policy (§3) |
| 64 GiB writable reservation fails under overcommit 0/2 and RLIMIT_AS | Reservation strategy chosen per overcommit mode; 1 GiB growable regions; commit-prefix rule for mode 2 (§2) |
| Hard-coded 4 KiB / 2 MiB | All geometry read at run time (`AT_PAGESZ`, `hpage_pmd_size`); hugeblock capped at 4 MiB (§2.1) |
| Size-class waste stated as "12.5%" | Explicit 52-class scheme; worst case ~20% of the block above 128 B; generated per-class span lengths (§4) |
| Incoherent object-flow model (sharded lists + transfer caches) | One model: pages are the unit of transfer; no object transfer caches (§6, §7) |
| mm_cid sold as novel, protocol missing | rseq is **not used in v1.0**. Optional Phase-4 vCPU mode reuses the single-store commit and credits TCMalloc (§8.7) |
| Lifetime predictor on the fast path | Removed from the fast path. Counterfactual experiment for large objects only, Phase 5 (§17) |
| VMA-splitting features vs "<10 VMAs" | Per-policy VMAs fixed at reservation; no sub-range flag changes at run time; per-feature VMA budget (§2.3) |
| THP policy fights the kernel | Honours sysfs/prctl; `MADV_HUGEPAGE` only when defrag cannot stall; whole-hugeblock release (§10.3) |
| Guard pages vs THP and mlock | Guards only in slack or at hugeblock boundaries, or in a NOHUGEPAGE guarded region; no `PROT_NONE` fallback in S0 (§12) |
| Inline free list contradicts "out-of-line metadata" | No inline free list at all (§5) |
| Duplicated bitmap + free list | The bitmap *is* the allocator (§5) |
| XOR "secret" described as safe-linking | Moot: no links to encode (§12) |
| `MADV_FREE` by profile; no batching | `MADV_DONTNEED` default, `process_madvise` vectors, return codes checked (§10, §11) |
| No timer for idle programs; PSI misuse | Lazily started maintenance thread (multithreaded processes only); cgroup files re-opened per read; PSI opt-in (§11.3) |
| Fork, signals, TLS, init, exit not designed | Full lifecycle specification (§8) |
| API semantics missing | glibc-compatible semantics table plus C++ TU (§13) |
| `getenv` privilege surface | `AT_SECURE`: only options that strengthen security are honoured (§14) |
| Unfalsifiable goals; roadmap order | Measurement specification and reordered phases (§18) |
| Meshing | Dropped (§17) |

---

## 1. Principles and non-goals

**Principles:**
1. Correctness before speed. Every multi-word state transition is either under a lock or committed by a single store, or a single atomic RMW. A thread stopped anywhere (signal, fork, preemption) leaves state consistent.
2. Security comes from **placement and structure**, not retention. The default tier holds no memory back (no quarantine), following the security report's grouping: changing where metadata lives is nearly free, while holding memory back costs 11–98% ([MineSweeper](https://www.cl.cam.ac.uk/~tmj32/papers/docs/erdos22-asplos.pdf), [rerun study](https://arxiv.org/pdf/2402.03373)).
3. RSS honesty. Use `MADV_DONTNEED` by default, as Go 1.16 did: "RSS will more accurately reflect" physical use ([go1.16](https://go.dev/doc/go1.16), [golang#42330](https://github.com/golang/go/issues/42330)).
4. The kernel stays in charge. Respect THP sysfs and prctl settings, the process mempolicy and `overcommit_memory`. Never toggle VMA flags on sub-ranges at run time.
5. Small enough for 2–3 engineers to verify. The core target is ≤ 8k lines of C11 (a target, not a measurement). Lock-free structures are limited to three: the bitmap OR, a Treiber notify stack, and a lease word. Each one is model-checked.

**Non-goals for v1.0:**
- Hard real-time; point users to TLSF or o1heap.
- wasm.
- Moving or meshing objects.
- ML prediction on the fast path.
- Windows (designed for in §15, not built).

---

## 2. Geometry and address-space layout

### 2.1 Runtime geometry (no compiled-in page constants)

| Parameter | Source | Typical x86-64 | arm64 16K | arm64 64K |
|---|---|---|---|---|
| `BASE` | `getauxval(AT_PAGESZ)` | 4 KiB | 16 KiB | 64 KiB |
| `PMD` | `/sys/kernel/mm/transparent_hugepage/hpage_pmd_size`, falling back to `BASE` | 2 MiB | 32 MiB | 512 MiB |
| `HB` (hugeblock) | `PMD` if ≤ 4 MiB, else 2 MiB (the contiguous-PTE unit on 16K/64K granules [kernel arm64 hugetlbpage](https://docs.kernel.org/arch/arm64/hugetlbpage.html)) | 2 MiB | 2 MiB | 2 MiB |
| `SLICE` | `max(64 KiB, BASE)` | 64 KiB | 64 KiB | 64 KiB |
| Release granule | `BASE` | 4 KiB | 16 KiB | 64 KiB |
| Guard size | `BASE` | 4 KiB | 16 KiB | 64 KiB |

`valloc` and `pvalloc` use `BASE`. CI covers qemu arm64 with 4K, 16K and 64K kernels; Asahi documents allocators that broke on 16K ([Asahi](https://asahilinux.org/docs/sw/broken-software/)).

### 2.2 Reservation strategy, chosen by overcommit mode and RLIMIT_AS

At first reservation, `/proc/sys/vm/overcommit_memory` is read with a raw `open`/`read`/`close` syscall sequence (`O_CLOEXEC`), along with `getrlimit(RLIMIT_AS)`.

- **Region size** `R = clamp(min(1 GiB, RLIMIT_AS/16), 32 MiB, 1 GiB)`. Each region is aligned to `R` by over-mapping and trimming once per region.
- **Modes 0 and 1:** `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`. These mappings are not commit-charged ([overcommit-accounting](https://docs.kernel.org/mm/overcommit-accounting.html)). Mode 0 refuses a single request larger than RAM plus swap, so on `ENOMEM` the size is halved repeatedly down to 32 MiB.
- **Mode 2:** `MAP_NORESERVE` is ignored ([same doc](https://docs.kernel.org/mm/overcommit-accounting.html)), so the region is reserved `PROT_NONE` (cost 0) and committed with `mprotect(RW)` in `HB`-sized chunks.
  - **Commit-prefix rule:** pages are allocated low address first, and decommit (`mprotect(PROT_NONE)` after `MADV_DONTNEED`) happens only at the top of the committed prefix. Each region therefore stays at 2 VMAs or fewer.
  - If `mprotect` returns `ENOMEM`, `malloc` returns NULL with `errno=ENOMEM`. Strict-accounting users keep their NULL semantics.
- **More regions** are reserved on demand and registered in the region table (§3). The design never assumes one region.

### 2.3 VMA layout and budget

Each policy class is a separate VMA, set at reservation time and never toggled on sub-ranges. `MADV_[NO]HUGEPAGE` takes the mmap write lock and splits VMAs ([mm/madvise.c](https://raw.githubusercontent.com/torvalds/linux/master/mm/madvise.c)).

| VMA | Contents | THP policy (set once) | Name (`PR_SET_VMA_ANON_NAME`, 5.17+) |
|---|---|---|---|
| META | Page headers, bitmaps, page maps, heap structs | Default | `soham-meta` |
| SMALL ×n | Small and medium pages | §10.3 decision | `soham-small` |
| LARGE ×n | Large runs | Default | `soham-large` |
| GUARDED ×1 | Sampled (GWP-ASan) slots | `MADV_NOHUGEPAGE` | `soham-guarded` |
| HUGE ×k | One per huge object | Default | `soham-huge` |

**Budget:** at most 6 + 2·(regions) + k VMAs. A CI gate counts `/proc/self/maps` lines during a 1-hour soak test.

### 2.4 Robustness rules for VA

- Never register a region below 1 GiB. `top[0]` stays NULL so that `free(NULL)` resolves through the map (§3).
- Never pass mmap hints above 47 bits. The kernel does not return addresses above 47 bits without a hint ([x86 5-level](https://docs.kernel.org/arch/x86/x86_64/5level-paging.html); arm64 52-bit VA is likewise opt-in by hint, [arm64 memory](https://docs.kernel.org/arch/arm64/memory.html)).

---

## 3. Pointer-to-metadata lookup

**Two-level map:**
- `top[]` has 2^17 entries of 8 bytes, covering address bits [46:30] (1 MiB of VA, about 4 KiB resident). Each entry is NULL, a pointer to that region's slice map, or a tagged pointer to a "huge list" (§9.3).
- Each slice map has `R/SLICE` entries of 16 B (256 KiB per GiB, 0.024%, committed lazily) **[derived]**.

```
entry (16 B):
 w0: owner_tag:16 | kind:4 | class:8 | slice_off:6 | flags:6 | gen:24
     kind ∈ {FREE, SMALL, LARGE, LARGE_ALIGNED, EMERGENCY, RETIRING}
     owner_tag = heap id (12 bits) | EXCLUSIVE bit | 3 must-be-zero special bits
 w1: SMALL → pointer to local bitmap (remote bitmap = local + nwords[class])
     LARGE → pointer to run header
```

**Lookup:**

```
a = (uintptr)p & ADDR_MASK        // arm64: clear top byte (TBI/MTE); x86: no-op
if (a >> 47) → rare               // foreign or high
t = top[a >> 30]; if (!t) → rare  // NULL pointer, foreign pointer, pre-init
e = t[(a >> 16) & (R/SLICE - 1)]  // 2 dependent loads total
```

`slice_off` lets a multi-slice page (up to 32 slices = 2 MiB) find its base: `base = (a & ~(SLICE-1)) - (slice_off << 16)`.

**Foreign-pointer policy:**

| Pointer | S0 | S1 and above |
|---|---|---|
| Outside every region (from glibc before preload, `RTLD_DEEPBIND` libraries, ld.so) | Log once, count, **leak** | Abort with diagnostic |
| Inside our regions, failing validation | Abort with diagnostic | Abort with diagnostic |

The reasoning for S0: we never write to memory we do not own, so leaking a foreign pointer is safe. A pointer inside our regions that fails validation means corruption, so it always aborts.

---

## 4. Size classes and page kinds

**Scheme (52 classes, minimum alignment 16 = `alignof(max_align_t)` on x86-64 and arm64):**
- **Classes 0–7:** 16, 32, …, 128 (16 B step).
- **Classes 8–51:** 4 per doubling from 160 to 262144 (`2^k × {1.25, 1.5, 1.75, 2}`).
- **Lookup:**
  - `n ≤ 1024`: `cls_lut[(n+15)>>4]` (65-byte table).
  - Otherwise: `lg = 63-clz(n-1)`, then `cls = 8 + 4*(lg-7) + (((n-1) >> (lg-2)) & 3)`, about 6 instructions **[estimate]**.
- **Waste:** worst case 20% of the block (25% of the request) just above each power of two from 128 B up, matching jemalloc's documented bound ([jemalloc manual](https://raw.githubusercontent.com/jemalloc/jemalloc/dev/doc/jemalloc.xml.in)). Below 128 B the worst case is 47% at 17 B, which is inherent to 16 B alignment.
- **Proper validation:** expected waste on real size histograms (Redis, clang, CPython, RocksDB traces) is to be *measured* (E1), and classes may be re-cut from traces the way TCMalloc tunes its classes against fleet data ([size_classes.cc](https://github.com/google/tcmalloc/blob/master/tcmalloc/size_classes.cc)).

**Page kinds and span lengths** come from an offline generator that is checked into the repo as a table.
- **Objective:** choose span length L ∈ {64K, 128K, 256K, 512K, 1M, 2M} minimising `tail_waste/L ≤ 1/8`, subject to ≥ 8 objects per span for classes ≤ 16 KiB.
- **Small pages:** 64 KiB for classes ≤ 1 KiB.
- **Medium pages:** 128 KiB to 2 MiB for 1–256 KiB. Examples: 10240 → 128 KiB (12 objects, 6.25% tail); 262144 → 2 MiB (8 objects, 0 tail) **[derived]**.
- **Large:** anything above 256 KiB goes to page-heap runs (§9).
- **Density split:** spans holding ≤ 16 objects (all medium classes) are placed on *different hugeblocks* from dense small pages. This is ASPLOS'24's capacity-as-lifetime proxy, which gave +1.02% throughput and −0.82% memory fleet-wide at zero runtime cost ([ASPLOS'24](https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf)).

**Property tests:**
- Class mapping is monotone.
- Every size in `[n, usable(n)]` maps to the same class. This is what lets sized free accept the requested size, the usable size, or anything in between.
- `slot_size × nslots ≤ L`.

---

## 5. Metadata structures (out of line, in META)

**Page header**, 64 B, at an arithmetic location `META_HDR(region, first_slice)`:

```
u64  base            // page start
u32  nslots; u16 nwords; u8 class; u8 kind
u16  owner_heap; u16 alloc_cursor       // next word to scan
u64  fresh_groups    // bit g = group g never handed out since (re)provisioning → known-zero
_Atomic u32 remote_pending              // >0 ⇒ remote bits may be set
_Atomic u32 flags    // FULL, NOTIFIED, IN_POOL, RETIRING, UNRELEASABLE
u64  next, prev      // queue links (header indices)
u32  bitmap_gen; u32 rng_seed; u64 reserved
```

**Bitmaps:**
- `local[nwords]` and `remote[nwords]` are 64-bit words allocated from an internal meta slab, sized `16·nwords`, rounded to 64 B.
- Local bit 1 means free and available. Remote bit 1 means freed by another thread and not yet merged.
- Pad bits beyond `nslots` in the last word stay set in `local` and are masked out when the word is claimed. With that, "word became all ones" (`== ~0`) means the whole group is free (§6.3).
- **Cost:** 2 bits per slot, i.e. 1.56% at 16 B, 0.39% at 64 B, 0.098% at 256 B **[derived]**. Freed bitmap blocks pass through a 1024-entry FIFO before reuse, which narrows the stale-remote-OR race (§7.4).

**Heap**, about 2 KiB, one per heap slot (not one per thread):

```
u8  busy; u8 lease_req          // same cache line as cache[0..]
u16 tag                          // owner_tag value to XOR with
s64 countdown                    // bytes until next tick (sampling/stats/maintenance)
u32 free_tick
cache[52] × 32 B:
   u64 bits                      // claimed word (free slots reserved for this heap)
   u64 group_base                // address of slot 0 of the claimed group
   u64* word_ptr                 // &local[w] the word came from (double-free check)
   u32 slot_size; u16 page_hdr_lo; u8 fresh; u8 pad
partial[52]: up to K=2 header indices
_Atomic u64 notify_head          // Treiber stack of pages that got remote frees while FULL
remote_batch[8] × {bitmap*, word, mask}   // optional sender-side batching (E7)
large_cache[4]
```

**Thread-local storage:** exactly one initial-exec pointer, `__thread heap_t *soham_tls` (8 bytes). glibc requires initial-exec TLS for a replacement malloc, and a small footprint fits glibc's 512 B surplus static TLS for late `dlopen` ([Replacing malloc](https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html), [tunables](https://raw.githubusercontent.com/bminor/glibc/master/manual/tunables.texi)).

**Metadata cost per heap:** 52 classes × 32 B = 1664 B **[derived]**.

---

## 6. Fast paths

### 6.1 malloc

```c
void *malloc(size_t n) {
  heap_t *h = soham_tls;                         // 1  (fs:/tpidr_el0 load; static empty heap before init)
  if (likely(n <= 1024)) {                        // 2
    unsigned c = cls_lut[(n + 15) >> 4];          // 3
    cache_t *k = &h->cache[c];                    // 2
    h->busy = 1; cbarrier();                      // 1
    if (unlikely(h->lease_req)) goto slow;        // 2
    uint64_t w = k->bits;                         // 1
    if (likely(w)) {                              // 1
      k->bits = w & (w - 1);                      // 2  (blsr + store) — the ONLY commit
      char *p = k->group_base + tzcnt(w) * k->slot_size;  // 4
      cbarrier(); h->busy = 0;                    // 1
      if (unlikely((h->countdown -= k->slot_size) < 0)) return malloc_tick(h, p); // 3
      return p;                                   // 1
    }
  }
  return malloc_slow(h, n);
}
```

- **Cost:** about 24 instructions, one taken-branch-free path, no read of object memory **[estimate]**. A free-list pop has to load `*block` from possibly cold freed memory; this path does not.
- **`malloc(0)`** maps to class 0 and returns a unique 16 B block, matching glibc.
- **The single `countdown`** covers GWP sampling, stats flush and the maintenance tick, using jemalloc's single-threshold trick ([jemalloc_internal_inlines_c.h](https://github.com/jemalloc/jemalloc/blob/dev/include/jemalloc/internal/jemalloc_internal_inlines_c.h)).

### 6.2 free (S0)

```c
void free(void *p) {
  uintptr_t a = (uintptr_t)p & ADDR_MASK;
  if (unlikely(a >> 47)) goto rare;                          // 2
  ent_t *t = top[a >> 30]; if (unlikely(!t)) goto rare;     // 4   (NULL lands here)
  ent_t e = t[(a >> 16) & SLICE_MASK];                       // 3
  heap_t *h = soham_tls;                                      // 1
  if (unlikely((e.w0 ^ h->tag) & OWNER_KIND_SPECIAL_MASK)) goto nonlocal;  // 3
  unsigned c = EXTRACT_CLASS(e.w0);                           // 2
  uintptr_t off = a - ((a & ~SLICE_M1) - (EXTRACT_SOFF(e.w0) << 16));      // 4
  uint32_t idx = (uint32_t)((off * cls_recip[c]) >> 32);      // 3   exact division by reciprocal
  if (unlikely(idx * cls_size[c] != off || idx >= cls_nslots[c])) goto bad_free; // 5
  uint64_t *lb = (uint64_t*)e.w1 + (idx >> 6), m = 1ull << (idx & 63);    // 4
  h->busy = 1; cbarrier(); if (unlikely(h->lease_req)) goto slow;         // 3
  uint64_t v = *lb;                                           // 1
  cache_t *k = &h->cache[c];
  if (unlikely((v & m) | ((k->word_ptr == lb) & !!(k->bits & m)))) goto double_free; // 6
  *lb = v | m;                                                // 1  commit
  cbarrier(); h->busy = 0;                                    // 1
  if (unlikely((v | m) == ~0ull)) group_became_free(h, e);    // 2  page-empty candidate
}
```

- **Cost:** about 45 instructions **[estimate]**. The critical path has 3 dependent loads (`top`, entry, bitmap word): roughly 12–16 cycles when everything hits L1 **[estimate]**. mimalloc's documented fast path is "about 7 to 10 instructions" ([alloc.c](https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/alloc.c)). This gap is the price of deterministic validation, and it is weakness #1 in §19.
- **What this path does not touch:** errno, any atomic, and any object memory.
- **Sized free** (`free_sized`, sized `delete`) adds `if (cls(n) != c) mismatch_abort` (+3 instructions). The size is never trusted for speed.

### 6.3 Why "group became free" instead of a used counter

- There is no per-free used counter. Page emptiness is computed with popcount when a group becomes all ones, or during sweeps.
- That keeps free to one commit store, and a lost update cannot cause a page to be released early.
- Any pointer that fails the shape check goes to the rare path, never to undefined behaviour.

---

## 7. Slow paths

### 7.1 Refill (`malloc_slow`)

1. If `h == &empty_heap`, call `thread_attach()` (§8.1).
2. If a TLS `in_slow` flag is already set (reentry from a signal or from libc during init), use `emergency_alloc` (§8.4).
3. Scan `k->page.local` from `alloc_cursor` for a non-zero word.
   - Claim it: `k->bits = word & valid_mask`, then set `local[w] = word & ~valid_mask` (pad bits only).
   - Set `k->fresh = (fresh_groups >> w) & 1`, then clear that bit.
4. If `remote_pending` is set, merge each word with `r = atomic_exchange(&remote[w], 0, acquire)`. If `r & (local[w] | claimed bits of w)` is non-zero, report a **double free** and abort. Otherwise `local[w] |= r`, then go back to step 3 from word 0.
5. The page is exhausted: set FULL and move to the next page in `h->partial[c]`.
6. Drain `h->notify_head` (pages that received remote frees while FULL), merge them, and put the best page into `partial`.
7. Take a page from the global per-class pool (one mutex per class per NUMA node). Choose the **fullest page with ≥ 1/8 of slots free**, so a refill yields contiguous runs. Ordering is maintained lazily, only when pages enter the pool. Rewrite the owner fields of its slice entries.
8. Get a new page from the page heap (§9.1). Bitmaps start all ones. `alloc_cursor` starts at a random group (S0 layout entropy, no fast-path cost). `fresh_groups` starts all ones.
9. On failure: run one synchronous release-and-retry, then return NULL with `errno=ENOMEM`.

**Per-heap retention (Hoard-style bound).** At ticks and at thread exit, a heap donates partial pages to the global pool while its owned free bytes exceed `max(K·S, f·A)`, with K = 4 pages, f = 1/4, and A = bytes allocated from the heap's pages. This mirrors Hoard's emptiness invariant, which bounds blowup ([Hoard](https://people.cs.umass.edu/~emery/pubs/berger-asplos2000.pdf)).

### 7.2 Remote free (`nonlocal`)

```
validate shape exactly as in §6.2               // same bad_free / double_free outcomes
old = atomic_fetch_or(&remote[w], m, release)   // 1 RMW, no CAS loop, no ABA
if (old & m) → double_free (abort)
racy hint: if (local[w] & m) → double_free (abort; owner re-checks at merge)
atomic_fetch_add(&hdr->remote_pending, 1, relaxed)   // (E8: replace by a flag if contended)
if (hdr->flags & FULL && !(test_and_set(NOTIFIED))):
    prevent_fork_enter();                        // per-thread counter, snmalloc PreventFork style
    treiber_push(&heap[hdr->owner_heap].notify_head, hdr_index);
    prevent_fork_exit();
```

- **Cost:** about 2 atomic RMWs per remote free **[estimate]**. mimalloc pays 1 CAS loop ([free.c](https://github.com/microsoft/mimalloc/blob/dev3/src/alloc/free.c)).
- **Ownership races.** Ownership changes only in the owner's own slow path, or under the pool lock while a heap has no thread. If a notify lands at the old owner, the old owner forwards it when it drains: it clears NOTIFIED, re-tests `remote_pending`, and re-notifies the current owner.
- **Sender-side batching (E7).** Up to 8 `{bitmap, word, mask}` entries; consecutive frees to the same word are OR-ed together, so one atomic covers up to 64 objects. This is the BatchIt idea ([BatchIt](https://microsoft.com/en-us/research/uploads/prodnew/2024/05/preprint_batchit.pdf)). The buffer is flushed on slow paths, at thread exit and in fork-prepare. It ships **off** until E7 passes, because BatchIt reported 6–8% regressions on some benchmarks.

### 7.3 Shared heaps (more threads than heaps)

- `H_max = clamp(4 × CPUs in sched_getaffinity, 8, 4096)`. Threads beyond `H_max` attach to *shared* heaps; the EXCLUSIVE bit in `tag` is clear for them.
- `malloc` on a shared heap takes a per-heap trylock, falling back to the next shared heap. This is Scudo's shared-TSD model ([tsd_shared.h](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/scudo/standalone/tsd_shared.h)).
- `free` from a shared-heap thread always takes the remote path (lock-free). The XOR test fails when EXCLUSIVE is clear.
- **Result:** memory is bounded by `H_max` heaps, not by thread count.

### 7.4 Page retirement and its residual race

When a group becomes free, or a sweep finds the page empty (all words `~0`, nothing claimed, `remote_pending == 0` after a final merge):
1. Set the slice entries' kind to RETIRING.
2. Drain the remote words. Any bit found there is a double free.
3. Set kind to FREE and return the page to the page heap.

**Residual race:** a buggy program whose double free races with retirement can OR a stale bit into a retired bitmap. Mitigations: the remote freer re-reads the entry after its OR and aborts if it changed; bitmap blocks are recycled FIFO-delayed; and remote bits for groups still marked fresh are rejected at merge. Detection under racing double frees is **best-effort** (weakness #6).

---

## 8. Lifecycle: init, threads, idle heaps, fork, signals, dlopen

### 8.1 Early init (no constructor dependency)

- **Static state:** all globals are zero-initialised. `soham_tls` starts as `&empty_heap` (all `bits == 0`, `lease_req == 0`). `top[]` is all NULL, so `free(NULL)` and `free` before init work; `malloc_usable_size(NULL)` returns 0.
- **`init_once`** uses an atomic state of 0, 1 (in progress) or 2 (done). Steps, all raw syscalls with no libc calls that allocate:
  1. `getauxval(AT_PAGESZ, AT_SECURE, AT_HWCAP2, AT_RANDOM)`.
  2. `getrandom(GRND_NONBLOCK)`, falling back to `AT_RANDOM` mixed with the TSC or `cntvct`.
  3. Parse `SOHAMALLOC` from `environ`. If `environ` is not set yet, use defaults and re-parse at the first tick.
  4. Read `overcommit_memory`.
  5. Reserve META.
  6. `pthread_atfork(prepare, parent, child)` and `pthread_key_create(&key, thread_detach)`.
- **Nested allocation during init** (for example `pthread_atfork` growing its own array) gets the **bootstrap arena**: a 256 KiB `.bss` bump area, untouched until used. Its pointers fall in a known address range, and `free` of them is a counted no-op.
- **Lazy reservations:** SMALL, LARGE and GUARDED are reserved on first need; cgroup and THP sysfs are read when SMALL is reserved.
- **Syscall budget:** ≤ 15 before the first `malloc` returns, to be measured against glibc (E12).
- **Bug class avoided:** mimalloc crashed when glibc 2.44 called `free(NULL)` before its constructor ran ([mimalloc#1341](https://github.com/microsoft/mimalloc/issues/1341)). This design has no constructor dependency.

### 8.2 Thread attach and detach

**Attach** (first slow path in a thread):
1. Pop a heap from the **heap pool**, preferring the most recently released one (it is warm), or create one if fewer than `H_max` exist; otherwise attach to a shared heap.
2. Set `soham_tls` *before* calling `pthread_setspecific`, so any malloc inside it finds a heap. The glibc manual says `pthread_setspecific` may allocate ([Replacing malloc](https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html)); exactly when it does is UNVERIFIED.

**Detach** (TSD destructor):
1. Flush the remote batch.
2. Return claimed bits to their pages.
3. Apply the retention bound with K = 0 for partial pages above the bound.
4. Push the heap, with its pages, onto the pool.
5. Set `soham_tls = &detached_heap`. Its tag never matches, so later frees (C++ `thread_local` destructors) take the remote path, which is correct because the pooled heap's pages still exist. Later mallocs use a shared heap.

**Why a pool:** thread-pool churn (mimalloc #351, where RSS grew about 150 MB per new thread, [issue](https://github.com/microsoft/mimalloc/issues/351)) becomes heap *reuse* instead of page abandonment. The maintenance tick collects heaps that have sat in the pool for more than 2 s: their pages go to the global pool and empty pages retire.

### 8.3 Idle-heap leases (memory stranded in sleeping threads)

The problem: a live thread that never reaches a slow path again strands its free slots. PartitionAlloc admits it cannot purge sleeping threads ([thread_cache.cc](https://github.com/chromium/chromium/blob/main/base/allocator/partition_allocator/src/partition_alloc/thread_cache.cc)).

Protocol (asymmetric Dekker):
- **Maintenance side:** set `lease_req = 1` on candidate heaps (no slow path for 10 s and owned free bytes > 256 KiB). Issue one `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)`, which acts as the owners' full fence. Then read each candidate's `busy`. If it is 0, the heap is leased: return claimed bits, donate partial pages, and clear `lease_req` with release ordering.
- **Owner side:** the fast path already does store `busy`, then load `lease_req` (two instructions, §6). If the request is set, it falls to the slow path and spins until the lease clears.
- **Rate limit:** at most one `membarrier` per second, batched across all candidates.
- **Fallback:** if `membarrier` registration fails (old kernel or seccomp), skip stealing and accept up to K·S stranded per idle thread. `membarrier` PRIVATE_EXPEDITED needs prior registration ([man](https://man7.org/linux/man-pages/man2/membarrier.2.html)); the first kernel version with it is UNVERIFIED (I believe 4.14).

### 8.4 Signals and reentrancy

- **Policy:** `malloc` is not async-signal-safe (POSIX), but sohamalloc **never corrupts and never deadlocks** on reentry.
- **Mechanism:** a handler that interrupts a fast path sees `h->busy == 1` (the same flag as §8.3) and is routed to `emergency_alloc`. That is a global atomic bump allocator over a 4 MiB EMERGENCY slice range. Its blocks are never reused; `free` of them is a counted no-op, so the leak is bounded. A handler that interrupts a slow path (holding locks) sees TLS `in_slow` and also uses the emergency path, never a lock.
- The explain report counts emergency bytes.
- The report is never generated inside a signal handler. An opt-in `SOHAMALLOC=report_signal=USR2` handler only sets a flag.

### 8.5 fork

- **prepare:**
  1. Flush this thread's remote batch.
  2. Take the locks in a fixed order: heap-pool, per-class pools, page-heap shards, region table, stats.
  3. Wait until the `prevent_fork` counters are zero (only the notify-push sequence increments them), using bounded `sched_yield` spinning.
- **parent:** unlock.
- **child:**
  1. Re-initialise all locks.
  2. Every heap other than the forking thread's moves to the pool as-is. This is safe because every fast path commits with a single store (§1), so an interrupted thread left consistent state. It costs O(#heaps) and touches no page metadata, which matters for copy-on-write servers such as Redis BGSAVE.
  3. Reseed the RNG.
  4. Mark the maintenance thread absent (restarted lazily).
- **Why not lock every mutex:** that is jemalloc's and hardened_malloc's approach, and it still misses lock-free multi-step operations. snmalloc's own comments note that its queue can detach during fork ([freelist_queue.h](https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/mem/freelist_queue.h)). This design has only one multi-step lock-free sequence, and it is fenced by `PreventFork` ([prevent_fork.h](https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/ds_aal/prevent_fork.h)).

### 8.6 dlopen, static linking, musl

- **Late `dlopen`:** the 8-byte initial-exec TLS fits the surplus. Earlier glibc pointers are foreign and follow the §3 policy, so S0 does not crash. On glibc, `SOHAMALLOC=forward_foreign=1` forwards them to `__libc_free` instead (weak reference; leak if it is absent).
- **`RTLD_DEEPBIND`** remains unsolved upstream ([glibc 30186](https://sourceware.org/bugzilla/show_bug.cgi?id=30186)). sohamalloc tolerates the direction where glibc pointers reach our `free`; our pointers freed by glibc crash inside glibc, which we cannot prevent. This is documented.
- **glibc static linking:** link before `libc.a` with the full symbol set. A CI test checks `DT_NEEDED` survives `--as-needed` ([ruby#18409](https://bugs.ruby-lang.org/issues/18409)).
- **musl:**
  - Override `aligned_alloc`; otherwise musl disables it once malloc is replaced ([glue.h](https://git.musl-libc.org/cgit/musl/tree/src/malloc/mallocng/glue.h)).
  - Static musl has strong symbols ([tweag](https://tweag.io/blog/2023-08-10-rust-static-link-with-mimalloc/)), so ship a `soham-musl-static` script that weakens `libc.a` malloc objects with `objcopy`.
  - No rseq dependency in v1.0.

### 8.7 rseq: deferred, with the protocol already in place

- v1.0 never calls `rseq()`, so glibc's ownership, the 6.19 `cpu_id_start` regression and 7.1's V2 kill-on-write are all irrelevant ([LWN](https://lwn.net/Articles/1070072/), [rseq docs](https://docs.kernel.org/userspace-api/rseq.html)).
- **Phase-4 vCPU mode:**
  - Read `mm_cid` read-only through `__rseq_offset`. Weak references keep glibc < 2.35 loadable. Detect it with `getauxval(AT_RSEQ_FEATURE_SIZE) ≥ offsetof(mm_cid)+4`, given the glibc 2.35–2.40 `__rseq_size` history ([glibc NEWS](https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=NEWS;hb=HEAD)).
  - Index heap slots by cid and size the slot array from possible CPUs. Pick NUMA/LLC pools from `cpu_id` at refill.
  - The §6 pop and free already commit with one store, so they map directly onto an rseq critical section (the `busy` flag is not needed there). Cross-slot work reuses the §8.3 lease. Under per-task CID mode the cid follows the thread, and mode switches are handled by the lease.
- **Prior art:** TCMalloc's vCPU work and `RseqVcpuMode::kMM` ([percpu.cc](https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/internal/percpu.cc)). This is not a novelty claim.

---

## 9. Page heap, large and huge objects

### 9.1 Hugeblock filler (per region shard, one mutex)

- Hugeblocks are `HB` units carved into slices.
- **Choosing a hugeblock for k slices** follows Temeraire's actual ordering: longest free range ascending (≥ k), then allocation count descending, with freshly donated hugeblocks last ([huge_page_filler.h](https://github.com/google/tcmalloc/blob/master/tcmalloc/huge_page_filler.h)).
- Dense small pages and sparse medium spans use separate hugeblock sets (§4).
- Per hugeblock state: a 32-bit slice bitmap (2 MiB / 64 KiB), a released-slice bitmap, a sparse-since timestamp and an allocation count.
- **Lock discipline:** release ranges are gathered under the lock and syscalls run after unlocking (§11). The mutex is a futex with bounded spinning.

### 9.2 Large runs (256 KiB < n < `HUGE_T`)

- **Placement:** runs are whole slices in LARGE regions, allocated by segregated fit (power-of-two bins of free runs, lowest address within a bin, coalescing on free). Bender et al. note that worst-case Best-Fit is Ω(M) while First-Fit is O(log M) ([arXiv 2608.28462](https://arxiv.org/abs/2608.28462)); address ordering is a cheap hedge.
- **Waste:** rounding to 64 KiB costs **VA, not RSS**. The allocator never writes data pages, so untouched tail pages are never resident, except under THP.
- **Run header (64 B, out of line):** `state` (ALLOCATED/FREE, changed by CAS, so large double frees are detected deterministically), requested size, returned pointer (for aligned runs), and a known-zero slice bitmap.
- **Per-heap cache:** up to 4 runs of ≤ 2 MiB, with a global cap of 64 MiB, decayed by the release engine. This addresses the critique that 256 KiB–2 MiB workloads would otherwise hit the page-heap lock every time.

### 9.3 Huge objects (≥ `HUGE_T = max(32 MiB, 16·HB)`, or alignment > HB)

- **Mapping:** a dedicated mmap (`MAP_NORESERVE` in modes 0/1). Alignment is done by over-mapping and trimming.
- **Guard:** `MADV_GUARD_INSTALL` on one `BASE` page placed in the trimmed slack *after* a hugeblock boundary, so no in-use PMD is split.
- **Registration:** the `top[]` 1 GiB slot gets a tagged "huge list" (up to 32 sorted descriptors per GiB, binary search, slow path only).
- **`realloc`:** `mremap(MREMAP_MAYMOVE)` is used **only here**, because mremap of faulted anonymous memory creates VMAs that cannot merge ([LWN 1015762](https://lwn.net/Articles/1015762/)).
- **`free`:** `munmap`, after a 1-entry cache for objects ≤ 256 MiB.

### 9.4 realloc of large runs

| Case | Action |
|---|---|
| Grow, next run free | Absorb it in place (metadata only) |
| Grow, otherwise | Allocate and copy (non-temporal stores above 4 MiB, E10) |
| Shrink | Split the run and release the tail with `MADV_DONTNEED` if it is ≥ 64 KiB |

---

## 10. OS layer

### 10.1 Probe table (run lazily, cached, reported in `soham_explain`)

| Feature | Probe | Min kernel | Use | Fallback |
|---|---|---|---|---|
| `process_madvise(self)` with any advice | Call on a scratch page | 6.13 (6.12 dropped CAP for self) | Vectored release | Coalesced `madvise` |
| `PIDFD_SELF` | Same, with −10000 | 6.15 | No fd needed | `pidfd_open(getpid())` if RLIMIT_NOFILE has headroom, else `madvise` |
| One TLB flush per vector | — | 6.16 | — | — |
| `DONTNEED`/`FREE` under per-VMA lock | — | 6.17 | — | — |
| `MADV_DONTNEED_LOCKED` | `EINVAL` test | 5.18 | mlocked heaps | Mark UNRELEASABLE |
| `MADV_GUARD_INSTALL` | Scratch page | 6.13 | Guards | S0: no guard, logged once |
| `MADV_COLLAPSE` | Not probed by default | 6.1 | Experiment E9 only | — |
| `MADV_POPULATE_WRITE` | `EINVAL` test | 5.14 | Latency profile prefault | Touch pages |
| `PR_SET_VMA_ANON_NAME` | prctl result | 5.17 | Naming | — |
| `PR_GET_THP_DISABLE` | prctl | 3.15 / 6.18 (`EXCEPT_ADVISED`) | THP decision | — |
| `mseal` | Syscall | 6.10 | Seal the config page | Skip |
| `membarrier` PRIVATE_EXPEDITED | Register | UNVERIFIED | §8.3 | No stealing |

Sources for the version table: the kernel report's §4 and §12 ([mm/madvise.c](https://raw.githubusercontent.com/torvalds/linux/master/mm/madvise.c), [process_madvise(2)](https://man7.org/linux/man-pages/man2/process_madvise.2.html), [pidfd.h v6.15](https://raw.githubusercontent.com/torvalds/linux/v6.15/include/uapi/linux/pidfd.h), [madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html)). jemalloc's probe gate treats `EPERM`, `EINVAL`, `ENOSYS` and `EBADF` as "unsupported" ([pages.c](https://github.com/jemalloc/jemalloc/blob/dev/src/pages.c)), and this design does the same.

**Self-batching requires 6.13.** On 6.12, `process_madvise` on self only allowed the remote-safe advice values.

### 10.2 Syscall hygiene

- No syscalls on fast paths. Every syscall reachable from `free` saves and restores errno.
- Every return code is checked. A partial `process_madvise` (it returns bytes advised) continues from where it stopped.
- `madvise`, `mprotect` and `munmap` are never issued while holding allocator locks.
- File reads use `open(O_RDONLY|O_CLOEXEC)`, `read`, `close`, and **no fd is ever cached**. Daemons that close every fd would otherwise make us read from a reused descriptor, and seccomp `EPERM` or `ENOENT` simply disables the feature.

### 10.3 THP decision (made once per SMALL reservation)

```
if PR_GET_THP_DISABLE or enabled==never:          no madvise; HB used only for packing
elif enabled==always:                              no madvise (kernel already does it)
elif enabled==madvise (or EXCEPT_ADVISED):
    if defrag ∈ {defer, never} or SOHAMALLOC=thp=on: MADV_HUGEPAGE once on the whole SMALL VMA
    else: no madvise   // defrag=madvise|always|defer+madvise ⇒ direct reclaim stalls (Go #61718)
thp_effective = (always) or (madvised above)
subrelease_ok = !thp_effective or max_ptes_none < 64
```

- `MADV_COLLAPSE` is **off** in v1.0. When enabled (E9) it runs only in the maintenance thread, with a backoff, and never when THP is `never`. TCMalloc rate-limits collapse ([huge_page_filler.h](https://github.com/google/tcmalloc/blob/master/tcmalloc/huge_page_filler.h)), and Go removed `MADV_COLLAPSE` after stalls ([go commit](https://fuchsia.googlesource.com/third_party/go/+/ef6993f3270c95e1158b11d9fc1a047528b34da6)).
- S0 never zeroes on free, so it does not feed the underused-THP shrinker. That shrinker splits THPs whose zero-filled subpages exceed `max_ptes_none` ([transhuge](https://docs.kernel.org/admin-guide/mm/transhuge.html)).

---

## 11. Memory release policy

### 11.1 Stages

1. **Cached:** free slices are kept committed and dirty.
2. **Released:** `MADV_DONTNEED` is the default.
   - `MADV_FREE` is only an opt-in stage for the `throughput` profile, and is skipped whenever cgroup headroom is below 20%.
   - `LazyFree` from `smaps_rollup` is always reported ([proc.rst](https://docs.kernel.org/filesystems/proc.html)).
3. **Decommitted** (mode 2 only): `mprotect(PROT_NONE)` of top-of-prefix commit chunks.

### 11.2 Formulas (evaluated per page-heap shard at each tick; Δt = time since last tick)

```
U(t)     = bytes in allocated pages/runs of the shard
F(t)     = cached free committed bytes
P10      = max U over the last 10 s ; P120 = max over last 120 s of 10-s minima of U
Demand   = max(P10, P120)                                 // skip-subrelease style (ISMM'21)
R        = clamp(Demand − U + H_min, H_min, R_max)        // retain target
H_min    = max(2·HB, U/128)  ; R_max = max(64 MiB, U/4)
E        = F − R
release_now = roundup_HB( E · (1 − 2^(−Δt/τ)) ),  τ = 5 s half-life
```

- **Order:** whole empty hugeblocks first. Sub-hugeblock runs of ≥ 64 KiB are released only if `subrelease_ok`, or under pressure, or if the hugeblock has been sparse (< 1/8 occupied) for more than 30 s. Partial unmaps of a THP wait on the deferred-split queue ([transhuge](https://docs.kernel.org/admin-guide/mm/transhuge.html)). The demand-window idea follows adaptive subrelease ([ISMM'21](https://research.google/pubs/pub50436/), [huge_page_subrelease.h](https://raw.githubusercontent.com/google/tcmalloc/master/tcmalloc/huge_page_subrelease.h)).
- **Pressure:** `h = (L − memory.current)/L`, where L is the minimum of `memory.max` and `memory.high` over ancestors (from `/proc/self/cgroup`, cgroup v2, up to the namespace root).
  - `h < 10%`: set R = H_min and release immediately.
  - `h < 3%`: also lease idle heaps (§8.3), drain pooled heaps and flush large caches.
  - `memory.current` is read at most every 100 ms from slow paths, and every 1 s in the background (100 ms when h < 20%).
  - `memory.stat` is never read, because of the rstat flush cost ([LWN 951695](https://lwn.net/Articles/951695/)).
- **Soft budget** (`SOHAMALLOC=budget=…` or `budget=auto` = 0.95·L): when exceeded, release synchronously; if still over, call the C++ `new_handler` path or return NULL **before** the kernel OOM-kills the process.
- **Rate limit:** maintenance CPU is capped at 1% of a CPU over 10 s, Go's scavenger model ([mgcscavenge.go](https://raw.githubusercontent.com/golang/go/master/src/runtime/mgcscavenge.go)).

### 11.3 Who runs it

- **Maintenance thread** (`soham-bg`): created lazily *only* when the process already has ≥ 2 heaps attached **and** (F > 64 MiB **or** a cgroup limit was found). It blocks all signals, sleeps on a futex when F ≈ 0, and is never created in single-threaded processes, because `unshare(CLONE_NEWUSER)` requires a single-threaded caller ([unshare(2)](https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/plain/man/man2/unshare.2)). The fork child restarts it lazily. Opt out with `bg=0`, force it with `bg=1`.
- **Without the thread:** ticks piggy-back on slow paths and on `group_became_free`, with a `CLOCK_MONOTONIC_COARSE` vDSO read every 64th event. `soham_release()` and `malloc_trim()` force a full release.
- PSI triggers are opt-in (`psi=1`): unprivileged windows must be multiples of 2 s ([psi.rst](https://docs.kernel.org/accounting/psi.html)).

---

## 12. Security tiers and their costs

Every cost below is a **hypothesis**, measured feature by feature against a "tier −1" build with no checks (E5).

**S0 (default):**

| Feature | Mechanism | Expected cost |
|---|---|---|
| No inline small-object metadata | Bitmaps (§5) | Memory: 2 bits/slot; free fast path +about 30 instructions vs mimalloc [estimate]; shared with deterministic checks |
| Invalid / interior free detection | Range + region + exact reciprocal boundary check | Inside the above |
| Double free | Local exact; remote at OR and at merge; large via CAS state | +6 instructions [estimate] |
| Sized free / sized delete check | class(n) == entry class | +3 instructions |
| Metadata isolation | META is its own mapping, its own random mmap base, never adjacent to data; `MADV_GUARD_INSTALL` at its ends | ≈0 |
| Layout entropy | Random start group per fresh page; region ASLR | 0 on fast path |
| Reentrancy and lease safety | `busy` flag | +4 instructions [estimate] |
| GWP-ASan-style sampling | Mean interval 1/8192 allocations ≤ 1 BASE page; 16 slots on 4K kernels, 4 slots on 64K; GUARDED region; guards via `MADV_GUARD_INSTALL` on free (zaps the freed slot); alloc/free traces from a bounds-checked frame-pointer walk (≤ 32 frames, no `backtrace()`, which dlopens libgcc ([unwind-link.c](https://sourceware.org/git/?p=glibc.git;a=blob;f=misc/unwind-link.c))) | Countdown already exists. 16 slots ×(slot + guard) on 4K kernels: 128 KiB [derived]; LLVM's default is about 40 KiB ([GwpAsan](https://llvm.org/docs/GwpAsan.html)) |
| SIGSEGV handler for sampled faults | Installed only if `sigaction` shows the default handler; chains to previous | 0 |
| Pointer vs data partition | Only when the binary calls `__alloc_token_*` (Clang AllocToken, [docs](https://clang.llvm.org/docs/AllocToken.html)): two page sets per class | RSS: more partial pages (E6) |
| Secure execution | `AT_SECURE` ⇒ ignore all weakening options, paths and fds | 0 |
| Seal the config page | `mseal` once, read-only after init (6.10+) | 0 |

**Not in S0, and why:**
- Quarantine: it holds memory back.
- Zero-on-free: +8.38% wall time on a kernel build for `init_on_free` ([commit](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=6471384af2a6530696fc0203bafe4de41a23c9ef)), and it feeds the THP shrinker.
- Per-class VA sequestering: VA and RSS growth.
- Guard slabs: VMA cost on kernels before 6.13.

**S1 (`SOHAMALLOC=hardened`, target ≤ 10%):**
- Random bit selection within the claimed word: `rotr(w, r)` before `tzcnt`, +3 instructions.
- Per-class VA sequestering: a slice once used by class c returns only to class c's pool, and the physical memory is released with DONTNEED.
- Zero-on-free for slots ≤ 1 KiB, as xzone does ([xzone](https://cs.iossec.tech/libmalloc/latest/source/doc/xzone_malloc.md)), plus a write-after-free check when a slot is handed out again.
- Byte-budgeted FIFO quarantine of min(1% of U, 4 MiB), best-effort: an attacker who can allocate can flush it.
- Two call-site buckets for classes ≤ 1 KiB, chosen by `hash(ret_addr ^ secret)`.
- A guard `BASE` page at the end of every hugeblock in SMALL: only with `MADV_GUARD_INSTALL`, VA cost 1/512 on 4K kernels [derived], no THP split because it sits at the boundary.
- Foreign-pointer abort.
- Comparison targets: snmalloc's checks (< 5%) and mimalloc secure mode (about 10%) ([snmalloc security](https://github.com/microsoft/snmalloc/blob/main/docs/security/README.md), [mimalloc readme](https://github.com/microsoft/mimalloc/blob/main/readme.md)).

**S2 (`mte`, arm64 with `HWCAP2_MTE`; chosen at reservation because `PROT_MTE` must be set at map time):**
- Tag slots ≤ 32 KiB; larger allocations use guards.
- Random tags that exclude the previous tag and both neighbours. Tag 0 means free.
- Retag on free for ≤ 4 KiB, on allocation for 4–32 KiB.
- Lazy tagging at allocation time, never span-wide.
- Retag after purge, because DONTNEED and FREE may clear tags.
- ASYMM by default, SYNC under `mte,secure`.
- META is never `PROT_MTE`.
- Sources: [arm64 MTE](https://docs.kernel.org/arch/arm64/memory-tagging-extension.html), [MTE perf](https://arxiv.org/html/2601.11786v1). TikTag ([arXiv](https://arxiv.org/abs/2406.08719)) means tags are treated as probabilistic.

**S3 (quarantine with sweeping, one-time allocation):** out of scope for v1.

**What S0 does not stop, stated honestly:**
- Use-after-free after the slot has been reused.
- A double free after the block was reallocated.
- In-object overflow into an adjacent live object.
- Cross-class type confusion after a page is reused (S1 covers this).

---

## 13. API semantics (glibc-compatible) and C++

| Entry point | Behaviour |
|---|---|
| All sizes | `__builtin_*_overflow` checks; sizes > `PTRDIFF_MAX` → NULL with `ENOMEM` (glibc 2.30 behaviour) |
| `malloc(0)` | Unique 16 B block |
| `calloc` | Overflow check. No memset if `k->fresh`, or the large run's slices are known-zero (cleared on `MADV_FREE`, on failed DONTNEED, and on MTE retag paths) |
| `realloc(NULL,n)` | Same as `malloc(n)` |
| `realloc(p,0)` | glibc: free p, return NULL |
| `realloc` small→small | In place if the class is unchanged |
| `realloc` failure | Old block intact, `errno=ENOMEM` |
| `aligned_alloc` / `memalign` / `posix_memalign` | Alignment ≤ 16 is normal. Alignment ≤ 64 KiB uses the smallest class whose size is a multiple of the alignment (page bases are 64 KiB-aligned, so every slot is aligned and there are no interior pointers). Larger alignments use an aligned large run (LARGE_ALIGNED: `free` checks p == the returned pointer) or a huge mapping. `posix_memalign` returns `EINVAL` per POSIX. `aligned_alloc` accepts sizes that are not a multiple of the alignment, as glibc does |
| `valloc` / `pvalloc` | Runtime `BASE` |
| `free_sized`, `free_aligned_sized` | Validate (§6.2); NULL accepted |
| `malloc_usable_size` | Slot or run size. Writing into the slack is legal for us |
| `reallocarray` | Not overridden on glibc (its version calls our `realloc`; replacing it is unsupported per the manual) |
| `mallopt` | Accept, return 1, log once |
| `mallinfo` / `mallinfo2` | From stats |
| `malloc_trim` | `soham_release()` |
| `malloc_stats` / `malloc_info` | `write(2)` text / XML |
| `free` | Preserves errno |

**C++** lives in a separate translation unit, `libsohamalloc_cxx` (C++17), linked into the same `.so` build:
- All replaceable `operator new`/`delete` forms: throwing (with the `new_handler` loop and `bad_alloc`), nothrow, `align_val_t`, and sized. A sized delete that arrives as unsized is handled correctly ([new.delete.single](https://eel.is/c++draft/new.delete.single)).
- `__size_returning_new` and the `__hot_cold_t` overloads for ABI compatibility with TCMalloc ([malloc_extension.h](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h)). In v1.0 hints are counted and otherwise ignored.
- A `std::pmr` adaptor over soham heaps.
- New/delete have no observable side effects, so they stay correct under GCC's `-fassume-sane-operators-new-delete`.

**First-class heaps (v1.1):** `soham_heap_new/destroy/visit` with a reuse-delay hook, which is what CPython free-threading needs ([PEP 703](https://peps.python.org/pep-0703/)). An official Rust `GlobalAlloc` crate uses `free_sized`.

---

## 14. Observability and configuration

**Explain report** (`soham_explain(fd)`, `SOHAMALLOC=report=exit`, or the stats API). Allocation-free, built from a static buffer, written with raw `write`. It has two views that are never forced to add up:
- **Allocator view:** requested, rounded (internal waste), page tail waste, free slots in owned pages, pooled heaps, page-heap cached, released-but-reserved, metadata, emergency, bootstrap.
- **Kernel view** (on demand, from `smaps_rollup`): Rss, AnonHugePages, LazyFree. The difference is labelled "not attributable".

It also prints the probe results and the active modes, for example `front-end: exclusive-heaps 37/256, shared 0; THP: madvise, MADV_HUGEPAGE applied=no (defrag=madvise); release: process_madvise+PIDFD_SELF; overcommit=0; bg-thread: running`.

**Other observability:**
- `soham_stats_json()` uses a versioned schema. Counters are per heap and are summed on read, so the fast path does no atomics.
- USDT probes on slow paths, releases and samples.
- Valgrind client requests (`MALLOCLIKE`/`FREELIKE`) in a `-DSOHAM_VALGRIND` build, and an ASan-compatible no-override build. Keeping these avoids jemalloc's lesson from removing Valgrind support ([postmortem](https://jasone.github.io/2025/06/12/jemalloc-postmortem/)).

**Configuration:** one variable, `SOHAMALLOC=<profile>[,key=value…]`.
- **Profiles:** `default`, `lowmem` (τ=1 s, R_max = U/16, K=2), `throughput` (τ=20 s, the MADV_FREE stage), `latency` (`MADV_POPULATE_WRITE` prefault of the next hugeblock from the background thread), `hardened`, `mte`, `debug` (junk fill, leak report from the sampler), `deterministic` (fixed seed, count-based decay, no background thread).
- **Keys:** `bg`, `thp`, `budget`, `sample`, `seed`, `report`, `forward_foreign`, `psi`.
- Unknown keys are reported once, never fatal. Under `AT_SECURE` only `hardened`, `mte` and higher `sample` rates are honoured.

---

## 15. Portability

**OS primitive layer:**
- `reserve(size, mode)`, `commit`, `decommit`, `release(iov[])`, `guard(range)`, `name(range)`
- `page_geometry()`, `thp_state()`, `limits()` (overcommit, RLIMIT_AS, cgroup)
- `random()`, `now_coarse()`, `futex_wait`/`wake`, `asym_fence()`, `cpus_allowed()`

**Per-platform plan:**

| Platform | Plan |
|---|---|
| Linux glibc ≥ 2.17, musl ≥ 1.2, Bionic | v1.0 targets |
| 32-bit | R = 64 MiB; `top[]` over bits [31:26] |
| FreeBSD | `madvise` fallback paths |
| macOS | Malloc-zone registration (designed later) |
| Windows | Reserve/commit maps onto mode 2; redirection DLL |
| wasm | Non-goal |

---

## 16. Failure modes

| Situation | Behaviour |
|---|---|
| mmap `ENOMEM` (any mode) | Halve the region size (down to 32 MiB), then release-and-retry, then NULL with `ENOMEM` |
| Mode 2 `mprotect` `ENOMEM` | NULL. No SIGSEGV later |
| RLIMIT_AS small | Regions shrink to RLIMIT_AS/16 |
| `max_map_count` near its limit | Huge objects fall back to large runs; GWP slots shrink |
| `madvise` `EINVAL` on mlocked memory | `DONTNEED_LOCKED`, else UNRELEASABLE, logged once |
| seccomp blocks `open` / `process_madvise` | Feature disabled, shown in `soham_explain` |
| No `membarrier` | No idle-heap stealing |
| cgroup v1 or no cgroupfs | Limit unknown; time-based decay only |
| Invalid free / double free inside our regions | Abort with report |
| Foreign pointer | S0: leak and log; S1: abort |
| Signal reentry | Emergency arena (bounded leak) |
| Bootstrap arena exhausted (256 KiB) | Abort with diagnostic. It only serves init-time nesting |
| Fork during a notify push | Prepare waits for `PreventFork` |

---

## 17. Decision records

| # | Decision | Alternatives rejected (why) | Cost | Risk | Validating experiment |
|---|---|---|---|---|---|
| D1 | Out-of-line bitmaps instead of free lists | Encoded inline lists (mimalloc/snmalloc: faster, but metadata stays reachable through UAF and needs per-op decode checks); pointer-array tcache (glibc/jemalloc: ownership-free, mixes pages) | 2 bits/slot; longer free path | Throughput gap on free-heavy microbenchmarks | **E2:** mimalloc-bench plus Redis/RocksDB; pass if geometric-mean time ≤ 1.10× mimalloc v3.4.x and memory ≤ 1.0× |
| D2 | Page-based owner-returns with heap pool | Object-transfer caches (TCMalloc: incompatible with page-local state); abandon/reclaim only (mimalloc #351 churn) | Remote free = 2 atomics | Producer/consumer cost | **E3:** xmalloc-test with back-pressure, 1M thread create/exit, 10k idle threads |
| D3 | No rseq in v1.0 | mm_cid slots now (fragile ABI, per-arch asm, 6.19/7.1 churn) | Per-thread scaling, bounded by H_max | Many-core memory | **E4 (Phase 4):** per-thread vs vCPU at (4 threads/256 CPUs), (10k threads/64), (cpuset 2) |
| D4 | 1 GiB growable regions, mode-specific commit | One fixed 64 GiB region (fails under modes 0/2 and RLIMIT_AS) | More regions and VMAs | Map lookups across many regions | **E11:** matrix of overcommit 0/1/2 × `ulimit -v` 2 GiB × 4 GiB swapless VM |
| D5 | 16 B page-map entries, 2 dependent loads | mimalloc aligned-meta arithmetic (no range check: unsafe for foreign pointers); 3-level radix (extra load) | 0.024% VA | L1 pressure on `top`/entries | **E2** plus perf L1/LLC misses on free |
| D6 | 4 per doubling, 52 classes | 8 per doubling (more partial pages); TCMalloc-style fleet table (no fleet data) | ≤ 20% rounding | Bad fit for some programs | **E1:** trace-derived waste; regenerate the table if mean waste > 12% |
| D7 | DONTNEED default, process_madvise vectors | MADV_FREE default (RSS confusion, [golang#42330](https://github.com/golang/go/issues/42330)) | Refault plus zeroing on reuse | Throughput regression on churny loads | **E13:** RSS-time integral vs throughput across profiles, with and without INVLPGB |
| D8 | Lazy background thread, never in single-threaded processes | Always-on (breaks `unshare`, wasted on small processes); none (idle processes keep memory, jemalloc#1398) | One thread in larger processes | seccomp `clone`; idle single-threaded processes keep their cache | **E14:** 60 s idle phase, RSS drop |
| D9 | THP respect and no runtime VMA toggling | `MADV_HUGEPAGE` everywhere (stalls, bloat); `MADV_COLLAPSE` on slow path | Lower THP coverage than Temeraire on `madvise` hosts | Missed TLB wins | **E9:** dTLB walk cycles, coverage, p99 first-touch, THP always/madvise/never × `max_ptes_none` 0/511 |
| D10 | Single-store commits plus `busy` flag | Atomics on fast path (slow); "malloc in signal handlers is UB" (deadlocks) | ~4 instructions | — | **E5** ablation |
| D11 | Hoard-style per-heap bound plus idle leases | Unbounded per-thread caches; TCMalloc per-CPU budget | Donation churn | Thrash at the boundary | **E3, E14** |
| D12 | Lifetime prediction deferred to large objects, counterfactual mode only | Return-address lookup on fast path (LLAMA measured 22.5 ns lookups vs an 8.3 ns fast path, [LLAMA](https://colinraffel.com/publications/cacm2022combining.pdf); wrapper funnels) | None in v1.0 | Forgoing possible gains | **E15:** counterfactual on RocksDB/CPython traces; enable only if RSS drops ≥ 5% |
| D13 | No meshing | Mesh (memfd, fork copies the whole heap, breaks DONTNEED, [meshable_arena.cc](https://raw.githubusercontent.com/plasma-umass/Mesh/master/src/meshable_arena.cc)) | Forgoes 16–39% wins on some programs | — | Revisit after v2 |

**Further experiments:**
- **E6:** AllocToken partitions, RSS cost.
- **E7:** remote batching.
- **E8:** `remote_pending` counter vs a flag under contention.
- **E10:** non-temporal copy threshold.
- **E12:** startup syscalls and `/bin/true` RSS vs glibc and mimalloc.

---

## 18. Validation and roadmap

**Measurement specification:**
- **Pinned baselines:** glibc 2.44 and 2.39; jemalloc 5.4.0 with and without `background_thread`; mimalloc v3.4.5; Google TCMalloc with per-CPU mode *confirmed active*; snmalloc; Scudo and hardened_malloc for tier costs.
- **Hardware:** x86 many-core; arm64 4K/16K/64K; a 2-vCPU cgroup.
- **Metrics:**
  - Peak RSS, RSS-time integral, RSS ÷ live bytes over time.
  - Throughput geometric mean with 95% bootstrap CI over ≥ 10 runs with re-linking ([Mytkowicz](https://sape.inf.usi.ch/publications/asplos09)).
  - wrk2-style constant-rate p99.
  - VMA count; TLB IPIs from `/proc/interrupts`.
- **Phases per run:** ramp, steady, burst, ≥ 60 s idle.
- **Required scenarios:** fork under write load, thread churn, `memory.max` at 1.2× the working set, `GLIBC_TUNABLES=glibc.pthread.rseq=0`.

**Correctness:**
- libFuzzer over API call sequences, with a differential oracle against glibc semantics.
- GenMC models of the three lock-free structures and the lease protocol.
- TSan/ASan builds of the allocator with override off.
- Fork from 64 threads in the middle of frees.
- `free(NULL)` before init; `dlopen` late; static glibc and musl.
- A 72-hour soak test with RSS-drift alarms.

**Phases:**
- **P0:** harness, trace recorder, geometry, regions, page map, bitmap pages, S0 checks, conformance and fuzz suite. Exit: glibc malloc tests pass and 24 h of fuzzing finds nothing.
- **P1:** single-thread performance.
- **P2:** heaps, pool, remote frees, fork and signals (model-checked).
- **P3:** OS layer and release engine.
- **P4:** S1/S2 and the optional rseq mode.
- **P5:** counterfactual lifetime experiments.

---

## 19. Top 10 weaknesses of this design

1. **The free fast path is long.** About 45 instructions and 3 dependent loads against mimalloc's 7–10 instructions [estimate]. Free-heavy microbenchmarks (larson, mstress) may lose 10–30% [estimate, unmeasured]. Every check is on by default, and the tier −1 ablation may show it is not "near-zero".
2. **Bitmap metadata for tiny objects.** 1.56% for 16 B slots and 0.78% for 32 B, versus mimalloc's claimed ~0.2% [derived]. The remote bitmap doubles it for pages that never see remote frees. Allocating remote bitmaps lazily is unexplored.
3. **Address-ordered reuse inside a page loses LIFO cache warmth.** A just-freed (hot) slot is reused only after the claimed word and earlier groups are used up. mimalloc's `local_free` delay is similar but not identical; locality could regress (E2).
4. **Per-thread heaps still scale with threads up to `H_max = 4×CPUs`.** On a 256-CPU host, 1024 heaps each with partially filled pages can strand hundreds of MB (bounded by K·S per heap, [derived]). Shared heaps add a lock to `malloc`. A real fix needs the deferred rseq mode.
5. **Idle-heap stealing relies on `membarrier`.** It sends IPIs to CPUs running the process (kernel-friendliness cost) and is unavailable under some seccomp profiles. Without it, sleeping threads still strand memory.
6. **Double-free detection is best-effort under races.** A double free that races with page retirement can leave a stale bit in a recycled bitmap. A double free after the block was reallocated is undetectable without a quarantine. The guarantees are weaker than "deterministic" sounds.
7. **Remote frees cost 2 atomics plus an occasional notify CAS.** Producer/consumer workloads may trail snmalloc's batched messaging until E7 lands, and batching itself regressed some benchmarks in BatchIt.
8. **THP coverage may trail Temeraire and jemalloc HPA.** On the common `enabled=madvise, defrag=madvise` configuration we deliberately do not madvise, so TLB benefits are left on the table, by design.
9. **Single-threaded idle processes keep their cache.** With no background thread, a single-threaded program that frees a lot and then sleeps holds up to R_max until it next calls into the allocator, unless it calls `malloc_trim`.
10. **The pool of unverified assumptions is large.** Untested so far: `pthread_setspecific` and `pthread_atfork` behaviour during init, `membarrier` and `pidfd_open` details, the musl static recipe, foreign-pointer leakage in `RTLD_DEEPBIND` programs, and whether the 52-class scheme fits real traces. Several features (AllocToken partitions, MTE tier, GWP slot sizing on 64K kernels) carry RSS costs that have not been measured. The "small team can verify it" claim depends on keeping the scope this narrow.