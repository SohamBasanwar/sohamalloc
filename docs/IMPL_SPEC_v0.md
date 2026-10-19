# sohamalloc v0.1 build spec

This spec scopes the first implementation of `docs/SOHAMALLOC_PLAN.md` (the plan). Section numbers like §5.4 refer to the plan.

- **Wins on conflict.** Where this spec and the plan disagree, this spec wins for v0.1.
- **Deviations are deliberate.** Each one says why, and the plan stays the long-term target.
- **The contract is fixed.** §1 and §2 are a contract between the core, test and benchmark work. Do not change names, files or behaviour there without updating this file.

## 0. Non-negotiable principles

1. **Correctness first, then speed.** Every multi-step state change is a chain of single-store commits; plan invariant I3.
2. **Dependencies.** C11 plus GCC/Clang builtins only. No libc calls that allocate on any allocator path: no stdio, `fopen`, `opendir`, `qsort` or `backtrace`. Diagnostics use `write(2)` and a tiny formatter.
3. **No undefined behaviour or data races.** Use `<stdatomic.h>` for every shared word, with an explicit memory order. Relaxed is allowed only where §5 says so.
4. **Fast-path discipline.** No function calls on the fast path (always-inline), and initial-exec TLS. Branches carry `__builtin_expect`.
5. **Comments.** Every lock-free protocol gets a comment block naming its invariant and the plan section it implements.

## 1. Build, files and artifacts (contract)

```
CMakeLists.txt            top level; C11; -O2 default Release; option(SOHAM_BUILD_TESTS ON)
include/sohamalloc.h      public API (below)
src/                      allocator sources (core); one translation unit may #include the rest
src/sohamalloc_cxx.cpp    C++ operator new/delete overrides (compiled into the override libs)
tests/                    test suite (run_tests.sh entry point)
bench/                    benchmark harness (run_bench.sh entry point); bench/out/ is git-ignored
docs/                     documents
```

`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` must produce:

| Artifact | Contents |
|---|---|
| `build/libsohamalloc.so` | Overrides the whole malloc family plus C++ new/delete. Usable with `LD_PRELOAD` |
| `build/libsohamalloc.a` | Same, static |
| `build/libsohamalloc-prefixed.a` | Only the `soham_*` symbols; no override. Built with `-DSOHAM_OVERRIDE=0`, for sanitizer (TSan/ASan/UBSan) testing of the allocator's own code |

CMake options:

- `-DSOHAM_DEBUG=ON`: internal assertions on (`SOHAM_ASSERT`), plus a full heap-consistency check callable through `soham_check_heap()`.
- `-DSOHAM_SANITIZE=thread|address|undefined`: applies only to the prefixed library and to test binaries.

## 2. Public API (contract): `include/sohamalloc.h`

```c
#define SOHAMALLOC_VERSION "0.1.0"
void  *soham_malloc(size_t n);
void  *soham_calloc(size_t count, size_t n);
void  *soham_realloc(void *p, size_t n);
void  *soham_reallocarray(void *p, size_t count, size_t n);
void   soham_free(void *p);
void   soham_free_sized(void *p, size_t n);                      // C23 semantics
void   soham_free_aligned_sized(void *p, size_t align, size_t n);
void  *soham_aligned_alloc(size_t align, size_t n);
int    soham_posix_memalign(void **out, size_t align, size_t n);
void  *soham_memalign(size_t align, size_t n);
size_t soham_usable_size(const void *p);
void  *soham_malloc_at_least(size_t n, size_t *usable);          // size feedback (plan §5.13)
void   soham_collect(int force);   // release free memory to the OS now (force=1: everything releasable)
void   soham_stats_print(int fd);  // the "explain my memory" report (plan §5.13), written with write(2)
struct soham_stats {
  size_t committed;      // bytes committed (RW) in windows + solos
  size_t resident_est;   // committed - released
  size_t in_use;         // bytes handed to the application (block granularity)
  size_t cached_heaps;   // free bytes inside pages owned by thread heaps
  size_t cached_pools;   // free bytes inside pages in class pools / orphans
  size_t pageheap_free;  // free slices not yet released
  size_t released;       // committed but released via madvise
  size_t metadata;       // records, heaps, internal arena
  size_t solo;           // bytes in solo mappings
  size_t windows, heaps_live, heaps_pooled;
};
int    soham_get_stats(struct soham_stats *out);   // 0 on success; a best-effort snapshot
int    soham_check_heap(void);                     // 0 = consistent; SOHAM_DEBUG builds walk everything
const char *soham_version(void);
```

**Override builds** also export:

- **Standard functions:** `malloc`, `free`, `calloc`, `realloc`, `reallocarray`, `aligned_alloc`, `posix_memalign`, `memalign`, `valloc`, `pvalloc`, `malloc_usable_size`, `free_sized`, `free_aligned_sized`.
- **Compatibility functions:**
  - `malloc_trim` (calls `soham_collect(1)` and returns 1),
  - `mallopt` (returns 1, ignored),
  - `malloc_stats` (calls `soham_stats_print(2)`),
  - `mallinfo` and `mallinfo2` (best effort from the stats),
  - the glibc aliases `__libc_malloc`, `__libc_free`, `__libc_calloc`, `__libc_realloc`, `__libc_memalign`, `__libc_valloc` and `__libc_pvalloc`.
- **C++ operators:** every global `operator new`/`delete` form (plain, nothrow, sized, aligned, aligned+sized, array). `new` loops on `std::get_new_handler()` and throws `std::bad_alloc`.

**Environment** (read once at init from `environ` without allocating; ignored when `getauxval(AT_SECURE)`):

| Variable | Meaning |
|---|---|
| `SOHAMALLOC_STATS=1` | Print the explain report to fd 2 at exit, through a destructor in the library |
| `SOHAMALLOC_SECURE=1` | Security tier S1 (§7) |
| `SOHAMALLOC_PROFILE=balanced\|memory\|throughput` | Release-policy presets (§6). Default `balanced` |
| `SOHAMALLOC_VERBOSE=1` | One-line diagnostics, such as a foreign-pointer free |

**Semantics** (must match glibc where both define them):

- `malloc(0)` returns a unique non-NULL pointer.
- `free(NULL)` is a no-op.
- `realloc(NULL, n)` behaves as `malloc(n)`. `realloc(p, 0)` frees `p` and returns NULL, as glibc does. A failed `realloc` leaves `p` intact.
- `calloc` overflow returns NULL with `errno = ENOMEM`.
- The minimum alignment is 16 on 64-bit and 8 on 32-bit, but blocks are 16-aligned everywhere.
- `posix_memalign` returns EINVAL for an alignment that is not a power of two or not a multiple of `sizeof(void*)`.
- `aligned_alloc` and `memalign` accept any power-of-two alignment. `memalign` rounds a non-power-of-two alignment up.
- `valloc` aligns to the page size. `pvalloc` also rounds the size up to the page size.
- `malloc_usable_size(p) >= requested`, and that many bytes are writable.
- Every failure sets `errno = ENOMEM`. `free` preserves `errno`.

## 3. Geometry (deviations from plan §5.1, with reasons)

| Symbol | v0.1 value | Note |
|---|---|---|
| B (base page) | `getauxval(AT_PAGESZ)`, else `sysconf(_SC_PAGESIZE)` | 4K/16K/64K must all work |
| S (slice) | max(64 KiB, B) | |
| H (hugeblock) | max(2 MiB, 32·S): 2 MiB with 4K or 16K base pages, also 2 MiB with 64K base pages (S = 64 KiB) | Simple and portable. Hugeblocks are a page-heap bookkeeping unit, so H need not equal the PMD size |
| W (window size and alignment) | **64-bit: 4 GiB. 32-bit: 256 MiB.** If RLIMIT_AS is finite: W = clamp(pow2_floor(RLIMIT_AS/8), 64 MiB, default). W is a runtime value held in the `geo` line as `wshift` | **Deviation.** The plan's 64 GiB VSZ and the 64 GiB alignment are too costly, and impossible on 32-bit. Arithmetic with a runtime shift costs one extra L1 load from the same line |
| gmap | One byte per W-granule of the address space: 64-bit 2^(48 − wshift) entries; 32-bit 2^(32 − wshift). `.bss`, lazily touched | 64 KiB on 64-bit with 4 GiB windows |
| Solo threshold | ≥ 32 MiB, or alignment > H | |
| Multiple windows | When a window fills, create another with the same W; at most 255 | |

**Window layout:**

```
+0          header (B bytes): magic ^ secret, wshift, commit marks, data_off
+B          hugeblock meta array: (W/H) entries
+REC_OFF    slice records (W/S) x 192 B; REC_OFF = round_up(B + (W/H)*sizeof(hbmeta), B)
            PROT_NONE guard gap >= 1 base page
DATA_OFF    = round_up(REC_OFF + (W/S)*192 + B, H)
...         data, committed as a growing prefix
```

**Reservation:** `mmap(NULL-or-hint, W + W, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`, then trim to a W-aligned W. On Windows: `VirtualAlloc(MEM_RESERVE)` with the same over-reserve-and-retry approach. Then:

- `MADV_NOHUGEPAGE` over the metadata area.
- **Commit by prefix:** `mprotect(RW)` (Windows: `MEM_COMMIT`), in steps of at least 4 MiB that double up to 64 MiB. Data and records grow together.
- Never un-commit; release uses madvise (§6).
- All of this happens under a dedicated `commit_lock`, never under the page-heap lock.

**Pointer → record** (plan §5.2):

```
a = p & ADDR_MASK;  g = a >> geo.wshift
if (g >= geo.gmap_n || geo.gmap[g] != GM_WINDOW) -> nonarena(p)
base = a & ~(W-1);  r = base + REC_OFF + ((a - base) >> SLICE_SHIFT) * 192
```

`SLICE_SHIFT` is a runtime `geo` value (16, or larger when B > 64 KiB).

## 4. Size classes and pages (plan §5.3)

- **Classes.** Use the plan's 64-class table exactly, with the plan's `lut` formula for n ≤ 1024 and its `clz` formula for larger sizes.
- **Page sizes.** Compute them at init into a `.bss` table using the plan's medium-page rule. Do not use a generator script.
- **Property tests.** Add them to `tests/`, using the internal header exposed for tests: `src/internal_test.h`.

Page kinds:

| Kind | Classes / sizes | Layout |
|---|---|---|
| SMALL | ids 0–43 | One slice |
| MEDIUM | ids 44–59, 160 KiB, 224 KiB | Multi-slice pages |
| RUN | 192 KiB, 256 KiB, and every size > 256 KiB and < 32 MiB | Slice-granular page-heap runs. Usable size = the run size |
| SOLO | ≥ 32 MiB | Dedicated mapping, tracked in a locked solo table. Its gmap byte is GM_SOLO with a per-granule refcount |

**Lazy provisioning.** A fresh page is a bump region. Blocks are threaded onto the free list max(B, 4 KiB) bytes at a time, in address order in S0 and in shuffled order in S1.

## 5. Concurrency model (v0.1, simplified from plan §5.4–5.6, §5.10)

**Deviation.** v0.1 has only the **tls-exclusive** mode, with **no idle-slot leasing and no membarrier**. Plan §5.5.6 and rseq-cid are P4. The plan's own judges recommended shipping this mode first.

**Why it is safe without leases.** A heap ("slot") is *held* by exactly one thread at a time: either its running thread, or a thread holding it exclusively while it is in the heap pool. Only the holder ever changes the ownership of the pages that heap references. Therefore:

- The local-free owner check `rec.owner == my_tag` cannot be invalidated between check and commit.
- The only interference is a **signal handler on the same thread**, handled by the `busy` flag.

### 5.1 Records (192 B = 3 cache lines; plan §5.4)

```
line O (owner-written; head record only): atomic u64 free  // [63:48] count of blocks on the local list, [47:0] head address
                                          u32 prov; u32 cap; u64 bump (cursor | ZERO_FRESH bit); bins linkage
line R (read-mostly; head only, except head_delta which every slice carries):
        atomic u32 owner (heap tag; 0 = unowned; flag bits CLAIMING etc. never equal a tag)
        u32 head_delta (bytes back to the head record; 0 = head); u64 key; u64 pbase; u32 psize; u32 bsize;
        u8 cls; u8 kind
line X (remote; head only): atomic u64 xfree // [63:48] count, [47:4] head, bit2 PENDING, bit1 QUEUED, bit0 ORPHAN
                            u32 folded; pool links (prev/next)
```

**Deviation:** owner, key, pbase and the other R fields live only in the head record, and non-head slices carry only `head_delta` and `kind`. The free path always computes `rh = r - r->head_delta`. For single-slice pages `rh == r`, so the cost is the same. This removes the plan's multi-copy ownership updates.

On 32-bit, the address fields are 32-bit inside the same 64-bit words, and the count field has the same position.

### 5.2 Link encoding (plan §5.5.2, corrected)

```
K(r, a)   = r.key ^ ((uintptr)a >> 12)
enc(n, a) = bswap64(n ^ K)   // stored in the first 8 bytes of block a; on 32-bit, bswap32 with 32-bit words
dec(E, a) = bswap64(E) ^ K
```

- NULL is stored as `bswap(K)`.
- One `static inline` pair is used by every writer and reader.
- Property test: `dec(enc(x)) == x`.

### 5.3 Heaps and TLS

- **Heap (slot):** `busy` byte; `tag`; `bins[64] {cur, alt}` (alt only for SMALL); 2 empty-page spares; stats counters.
  - Heaps come from the internal metadata arena and are never freed: they are pooled.
  - The tag is the heap index + 1 and is stable for the heap's life.
- **TLS** (`__thread`, `tls_model("initial-exec")`; Windows: `__declspec(thread)`): `heap*`, `tag` (0 = none), `state` (NEW / ATTACHED / DEAD).
- **Attach** at a thread's first malloc or free that needs a heap:
  1. Pop a pooled heap or create one.
  2. Set TLS *before* `pthread_setspecific` (which may allocate).
- **Detach** (key destructor):
  1. Detach every page (§5.6) and return the spares.
  2. Push the heap to the pool.
  3. Set state DEAD and tag 0.
- **DEAD or NEW threads with tag 0:**
  - `free` → the remote path.
  - `malloc` → the **shared heap**, a normal heap guarded by a spinlock with trylock.
- **Signal reentrancy (plan §5.10):**
  - On entry, if `heap->busy`, then malloc → the shared heap via trylock. If that fails, use the 256 KiB `.bss` emergency bump arena, whose frees are counted no-ops.
  - A reentrant free → the remote path, which is lock-free and always safe. **Deviation:** the plan's deferred list is not needed because the remote path never locks. The one exception is `resolve()` (§5.6), which needs the pool lock, so a reentrant free that would become responsible instead leaves PENDING set and pushes the page onto a lock-free "pending pages" stack, drained at the next slow path of any thread.

### 5.4 Fast paths (plan §5.5.4)

```
malloc(n <= SMALL max):
  heap h = tls.heap; if (!h) slow
  if (h->busy) reentrant
  c = class(n); h->busy = 1 (relaxed store + signal fence)
  r = h->bins[c].cur; f = r->free; b = f & PTR; if (!b) refill
  S0 checks: b within [pbase, pbase+psize); nx = dec(*b, b); nx == 0 or within page
  commit: r->free = nx | (count-1)
  signal fence; h->busy = 0
  return b

free(p):
  r = record(p) (else nonarena); rh = r - r->head_delta
  S0: p aligned to 16
  h = tls.heap; if (!h || h->busy) remote_free
  h->busy = 1
  if (rh->owner != h->tag) { h->busy = 0; remote_free }
  f = rh->free; if ((f & PTR) == p) double_free_abort
  *p = enc(f & PTR, p); commit rh->free = p | (count+1)
  if (count+1 == rh->prov && page is not cur) page_empty_handling
  h->busy = 0
```

Use a sentinel empty record for `bins[c].cur` so the fast path has no NULL check.

### 5.5 Remote free (plan §5.4 remote CAS rule, one CAS per object)

**Deviation:** v0.1 has no batching buffer. The plan's E6 makes per-object CAS the default unless batching wins.

```
loop: old = xfree; *p = enc(old.head, p) (head may be 0)
      new = {count+1, head=p, flags}
      if ((old.ORPHAN||old.QUEUED) && !old.PENDING && ((old.ORPHAN && new.count >= cap/8) || new.count == cap)) new.PENDING=1, responsible
      CAS(xfree, old, new) else retry
if responsible: resolve(page) [or push to the pending stack if reentrant]
```

`resolve` follows plan §5.4 exactly: the pool lock is held, PENDING is re-checked, and a `count == cap` page goes back to the page heap. **Deviation:** v0.1 has *no orphan list*. A page with ORPHAN set is on no list until it becomes PENDING. The waste this strands is bounded: under 1/8 of such pages.

### 5.6 Page lifecycle (plan §5.4–5.6)

- **Detach** (the owner, or the exclusive holder):
  1. owner = CLAIMING.
  2. Replace `bins` with the sentinel or the replacement.
  3. folded = local free count + (cap − prov).
  4. CAS: `xfree.count += folded`, set ORPHAN. If the new count ≥ cap/8 (or == cap), set PENDING and resolve now.
  5. owner = 0.
- **Claim** from a class pool:
  1. Under the pool lock, unlink the page and `old = xchg(xfree, 0)`.
  2. Drop the lock.
  3. Walk exactly `old.count − folded` chain nodes. Abort on a non-NULL last link or on an overflowing count.
  4. Merge into the local list with one commit.
  5. owner = CLAIMING|tag, then install as `cur`, then owner = tag.
- **Owned-page collection** (refill): `xchg(xfree, 0)`, walk exactly `count` nodes, merge with one commit.
- **Refill order:**
  1. Collect `cur`.
  2. Provision `cur` (`prov < cap`).
  3. Swap in `alt`.
  4. Detach `cur` and claim from the class pool, fullest bucket first (3 buckets: [1/8,1/4), [1/4,1/2), [1/2,1)).
  5. A fresh page from the page heap.
- **Page empty on a local free** (`count == prov`):
  - If the page is `cur`, keep it.
  - If it is `alt`, move it to a spare (at most 2) or return it to the page heap.
- **Pool lists:** per class, doubly linked through line X, with one mutex per class.

## 6. Page heap and release (plan §5.7–5.9, simplified)

- **One page heap per process**, with one mutex.
- **Per-hugeblock meta:** used-slice bitmap, dirty bitmap (free but resident), zero-known bitmap, longest free run, allocation count.
- **Allocation** of k slices (k ≤ slices per H): choose the hugeblock with the smallest longest-free-run ≥ k, ties broken by the highest allocation count, through bucket lists indexed by longest-free-run plus a non-empty bitmask.
- **Runs ≥ H** use contiguous fully free hugeblocks (address-ordered first fit over a fully-free bitmap), and the window commit grows when needed.
- **Release (decay):**
  - Free slices become dirty and are timestamped per hugeblock.
  - At most every 10 ms, from page-heap slow paths, release dirty slices idle ≥ D, with **no syscall under the page-heap lock**:
    1. Under the lock, mark the ranges RELEASING (unallocatable) and copy them to a local array of up to 64 ranges.
    2. Unlock and `madvise` them. Use `process_madvise(PIDFD_SELF, …)` when it works (Linux 6.15+; probe once, cache the result), otherwise a `madvise` loop.
    3. Relock and mark the ranges free + released + zero-known (DONTNEED only).
  - Whole free hugeblocks go first.
- **Profiles:**

  | Profile | D | Advice |
  |---|---|---|
  | `balanced` | 1 s | DONTNEED |
  | `memory` | 100 ms | DONTNEED; heap spares 0 |
  | `throughput` | 10 s | MADV_FREE where available |

- **`soham_collect(1)`** releases everything free immediately, drains the heaps of the calling thread, and processes pending pages.
- **THP:** read `/sys/kernel/mm/transparent_hugepage/enabled` with raw `open`/`read`. If it is `madvise`, apply `MADV_HUGEPAGE` once over the window's data VMA, but only in the `throughput` profile (plan §5.8.3 keeps the default conservative).
- **calloc:** skip memset only for bump-provisioned blocks of a ZERO_FRESH page, and for runs whose slices are all zero-known. The overflow check uses `__builtin_mul_overflow`.
- **realloc:**
  - Same class, or a run that still fits within ½..1× → return p.
  - Solo → `mremap(MREMAP_MAYMOVE)` on Linux.
  - Otherwise malloc + copy(min) + free.

## 7. Security tiers (plan §5.12)

- **S0 (default):**
  - Out-of-line metadata and the guard gap.
  - Encoded links.
  - Head double-free check.
  - Bounds checks on decoded links and on chain walks.
  - 16-byte alignment check in `free`.
  - Inside-our-ranges-but-invalid (record kind 0, or a metadata slice) → abort with a message.
  - A foreign pointer is logged once (VERBOSE) and leaked.
- **S1 (`SOHAMALLOC_SECURE=1`):**
  - All of S0.
  - Shuffled provisioning.
  - Exact block-boundary check on `free`, through the reciprocal.
  - Abort on a foreign pointer.
  - Zero-on-free for blocks ≤ 1 KiB.
- **Aborts** print `sohamalloc: <reason> at <ptr>` via `write(2)`, then call `abort()`.

## 8. Fork, early init and portability

- **Init:**
  - A lazy, atomic once-state, with no constructor dependency.
  - A recursion during init is served from the 128 KiB `.bss` bootstrap arena.
  - `pthread_atfork` and `pthread_key_create` are registered inside init.
  - Init runs at most 6 syscalls, plus 1 per extra probe.
- **Fork:**
  - **prepare:** lock, in fixed order, the heap-pool lock, the class-pool locks, the page-heap lock, the commit lock, the solo lock, the shared-heap lock and the pending lock.
  - **parent:** unlock.
  - **child:**
    1. Re-initialise the locks.
    2. Every other thread's heap with `busy == 0` → pool, as-is.
    3. Heaps with `busy == 1` are leaked (documented).
    4. Re-seed the RNG.
- **Portability matrix** (all must build; those marked *run* must pass the test suite):

  | Platform | Status |
  |---|---|
  | x86-64 glibc | run |
  | x86-64 musl, static and dynamic | run |
  | aarch64 glibc, under `qemu-aarch64` | run |
  | i386 (`-m32`) | run, if multilib is available |
  | armhf, under `qemu-arm` | run, if the cross-compiler is available |
  | x86-64 Windows (mingw) | builds the prefixed library and tests; runs them under `wine` (installed) |
  | macOS | code paths behind `__APPLE__` (`MADV_FREE_REUSABLE`, no `getauxval`); compile-reviewed only |

- **OS layer:** keep all OS calls in `src/os_posix.c` and `src/os_windows.c`, behind a small `os_*` interface.

## 9. Gates (definition of done for v0.1)

| Gate | Requirement |
|---|---|
| G1 correctness | `tests/run_tests.sh` passes 100% against `build/libsohamalloc.so` on x86-64 in both Release and `SOHAM_DEBUG` builds. The prefixed-library tests pass under TSan (0 reports), ASan and UBSan |
| G2 portability | The matrix in §8 |
| G3 compatibility | The `LD_PRELOAD` smoke suite (python3, gcc, git, perl, sort, find, a C++ program) gives correct output, with no crash and no stderr noise |
| G4 review | An adversarial review leaves 0 unresolved critical or major findings |
| G5 performance | Measured by `bench/run_bench.sh` against glibc, jemalloc, mimalloc and tcmalloc: time and peak RSS per benchmark, plus geometric means. Report honestly; the targets are set in the benchmarking phase |
