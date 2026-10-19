# sohamalloc v0.1: implementation notes

This document describes the allocator **as built**:

- the file map and data layouts;
- every lock-free protocol, with its invariant;
- the lock order;
- deviations from `IMPL_SPEC_v0.md`, with reasons;
- known limitations.

Read it alongside `src/`. Section references (§n) point to the spec; "plan §n" points to `SOHAMALLOC_PLAN.md`.

## 1. File map

| File | Contents |
|---|---|
| `include/sohamalloc.h` | Public API (contract, spec §2) |
| `src/internal.h` | Configuration, packed-word helpers, record/hugeblock/window/heap/TLS layouts, locks, link encoding |
| `src/sohamalloc.c` | The core is one translation unit (inlining across parts). It holds the globals and includes the three parts below |
| `src/pageheap.c` | Size classes; windows and commit growth; hugeblock bitmaps; best-fit slice allocation; the release (decay / excess) engine |
| `src/heap.c` | Metadata arena; static (bss) arena; records; provisioning; local lists; remote free; owner notification; detach/claim; class pools; pending stack; refill; runs; solos |
| `src/api.c` | Diagnostics; locks; configuration; init; thread attach/exit; malloc/free fast and slow paths; calloc/realloc/aligned; stats; explain report; check; fork; internals for tests; overrides, including every global C++ `operator new`/`delete`, written in C under their mangled names |
| `src/os.h`, `src/os_posix.c`, `src/os_windows.c` | The OS layer. Every OS call goes through it; errno is preserved |
| `src/internal_test.h` | Internals exported for property tests |

**Build outputs:**

| Library | Contents |
|---|---|
| `libsohamalloc.so` / `.a` | Override of malloc, the glibc aliases and C++ operators. Depends on libc only: no libstdc++, so preloading it into a C program loads nothing else |
| `libsohamalloc-prefixed.a` | `soham_*` only |

## 2. Address space

```
window (W bytes, W-aligned; W = 4 GiB on 64-bit, 256 MiB on 32-bit or under TSan,
        smaller when RLIMIT_AS is finite)
+0          window_t header (one base page)
+B          hbmeta_t[W/H]                 hugeblock bitmaps (committed, NOHUGEPAGE)
+rec_off    rec_t[W/S], 192 B each        slice records (committed, NOHUGEPAGE)
            PROT_NONE guard gap           >= 1 base page
+data_off   hugeblocks, committed as a growing prefix (2 .. 64 MiB steps)
...         last hugeblock never committed (tail guard)
```

- **Granule map.** `gmap` is a byte map with one entry per W-sized granule of the 48-bit address space (2^16 entries with 4 GiB windows). It is mapped at init.
  - The free fast path computes `g = (a >> wshift) & gmask`. Before init, `gmask` is 0 and `gmap` points to a 1-byte zero map, so every pointer is foreign.
  - The mask is published last, with release semantics.
- **Record lookup.** `rec = (a & ~(W-1)) + rec_off + ((a & (W-1)) >> 16) * 192`.
  - Multi-slice pages and runs store `head_delta` (in bytes) in every non-head slice record. `rec_head(r)` is a single subtraction.
- **Slice size.** S = 64 KiB, a compile-time constant (`SOHAM_SLICE_SHIFT`). Base pages of 4, 16 and 64 KiB all give S = 64 KiB.
- **Hugeblock size.** H = 32 S = 2 MiB.

## 3. Records (192 B = three cache lines with distinct writers)

| Line | Writer | Fields |
|---|---|---|
| O | owning heap only | `fhead`, `fcnt` (local free list), `prov`, `event_at`, `psize`, `bump`, owner copies of `key`/`pbase`, list links |
| R | page init, claim, detach | `owner` (heap tag or `OWNER_*`), `head_delta`, `key`, `pbase`, `psize`, `bsize`, `cap`, `nslices`, `cls`, `kind`, `lflags`, `oheap` |
| X | remote freers | `xfree` (count \| head \| flags), `folded`, pool links, pending/notify stack links and states |

- **Why the fast paths are cheap.** The owner's malloc touches only line O and the block. A local free reads line R (owner check) and writes line O.
- **Why remote frees don't bounce the owner's line.** A remote free reads line R and writes line X; it never touches line O.

**Link encoding** (§5.2, corrected in plan §5.5.2):

- `K = key ^ (a >> 12)`, `enc(n) = bswap(n ^ K)`, `dec(E) = bswap(E) ^ K`.
- bswap is the outermost operation on the stored value, so a partial overwrite corrupts the decoded high bits, and the next-pointer bounds check aborts.

## 4. Thread heaps

- **Heap layout.** `heap_t` = tag, RNG, two empty-page spares, 64 bins of `{cur, avail, full}`, and a `notify` stack on its own cache line.
- **Thread state.** The thread's state lives in initial-exec TLS: `heap`, `tag`, `busy`, `state`, `trymode`, `deferred`.

**Invariants:**

- **I1.** `owner == tag` ⇒ the page is `bins[c].cur`, or on `bins[c].avail` or `bins[c].full` of that heap. `r.lflags` says which list.
- **H1.** A heap is held by exactly one thread at a time: its running thread, or a thread holding it exclusively (the pool, or the fork child). Only the holder changes owner words of its pages. So the local-free check `owner == tag` cannot be invalidated between the check and the store.
- **H2.** `tls.busy` is set around every allocator operation of the thread that touches its heap or takes any allocator lock: the fast paths, the slow paths, init and heap attach (`thread_prepare`), thread exit, `soham_collect`, `soham_get_stats` and `soham_check_heap` (`busy_enter`/`busy_leave`). A signal handler that finds it set never touches the thread's heap and never waits for a lock:
  - its mallocs use the shared heap via trylock (`trymode`), or the static arena;
  - its frees use the lock-free remote path, or the deferred list for runs and solos.

## 5. Lock-free protocols

### 5.1 Remote free (one CAS per block)

```
loop: old = xfree; *a = enc(old.head)
      new = {old.count + 1, a, old.flags}; if WATCH && count + 1 >= unfull: drop WATCH (notify)
      if (ORPHAN|QUEUED) && !PENDING && ((ORPHAN && count >= cap/8) || count == cap): new |= PENDING  (responsible)
      CAS(old -> new, acq_rel)
after: if WATCH dropped -> notify_push(oheap, page)
       if responsible   -> resolve(page)  (handler context: pending_push)
```

Checks on every remote free: head == a (immediate double free), and count > cap (double free).

### 5.2 Owner notification (WATCH)

When a heap retires its exhausted `cur` page to the full list, it sets WATCH with a CAS. The CAS succeeds only on a zero remote count; otherwise the page collects its remote frees and stays `cur`.

- The remote free that brings the remote count to `unfull = max(1, cap/8)` clears WATCH in its CAS and pushes the page onto `oheap->notify`, a Treiber stack. While WATCH is set the chain may be non-empty; full-list pages are never collected, and detach drops WATCH in its own CAS.
- `nstate` keeps the page on at most one stack.
- The owner's refill drains the stack (one exchange). It moves a page from full to avail only if the page is still owned by it, still on the full list, and not re-armed. Stale entries are ignored.
- The local free that brings the local count to `unfull` disarms WATCH and moves the page to avail (`event_at = unfull` on the full list).
- **Why a threshold.** Returning a page at its first free made each refill yield a single block and retire the page again: under random-order frees (alloc-test) 28% of all mallocs took the slow path. With the threshold a refill yields at least cap/8 blocks (0.07% slow path), at the cost of at most cap/8 − 1 idle blocks per full page, counted separately for local and remote frees.

### 5.3 Detach and claim (exited threads)

- **Detach** (the holder only, for example at thread exit):
  1. Set `owner = 0`.
  2. Set `folded = local count + unprovisioned`.
  3. CAS `xfree`: count += folded, flags = ORPHAN (WATCH dropped). If count ≥ cap/8 or count == cap, also set PENDING and run `resolve`.

  Clearing the owner *before* the ORPHAN publication means a later claimer's owner store can never be overwritten.
- **resolve** (under the class pool lock, by the thread holding PENDING):
  - count == cap: CAS to 0, unlink if QUEUED, release the page.
  - ORPHAN with count ≥ cap/8: move it to the pool (QUEUED).
  - otherwise: clear PENDING.

  A failed CAS re-evaluates.
- **Claim** (under the pool lock):
  1. Unlink the page.
  2. `x = xchg(xfree, 0)`, which clears QUEUED and PENDING. A waiting resolver then sees no PENDING and does nothing.
  3. Walk exactly `x.count − folded` chain nodes, validating each (bounds, block boundary, terminator). Merge with one store.
  4. Set `oheap`, then `owner`.

### 5.4 Pending and deferred work (handler context)

- A handler that becomes responsible pushes the page onto the global pending stack (`pend_state` prevents double insertion). The next slow path of any thread runs `resolve`.
- Handler frees of runs and solos go onto the thread's TLS deferred list, which is drained at the thread's next allocator call.

### 5.5 Page heap release (§6)

The page heap tracks four slice states: used, dirty (free and resident), zero (free and known zero), and releasing.

**Release modes:**

| Mode | What it releases |
|---|---|
| decay | Hugeblocks dirty for ≥ `decay_ns`. Checked at most every `purge_interval_ns`, on page-heap allocations and frees |
| excess | Free resident memory above the dirty limit (below). Wholly free hugeblocks go first |
| all | Everything free: `soham_collect(1)`, `malloc_trim` |

**Dirty limit.** `max(dirty_min, used / dirty_div, swing)`, where `swing = min(peak − used, used · peak_num / 4)` and `peak` is the largest `used` of the current and previous decay epoch. A program whose live size oscillates (malloc-large frees a 20 MiB buffer and allocates another) keeps the memory it is about to reuse instead of releasing and re-faulting it on every cycle; decay still returns it once the swing stops. The cap means a program that frees nearly everything keeps almost nothing (freeing 1 GiB still drops to about 50 MB). Large runs (≥ one hugeblock) pick, among the free hugeblock runs long enough, the one with the most resident slices, so reuse costs no page faults.

**Each release pass:**

1. Under the lock, mark the ranges `releasing`; they are now neither allocatable nor free.
2. Unlock and call `process_madvise(PIDFD_SELF, ...)` with up to 64 merged ranges. It is probed once; on failure the pass falls back to a `madvise` loop.
3. Lock again and mark the ranges free: zero for DONTNEED, lazy for MADV_FREE.

No release syscall runs under the lock. The only syscalls made under the page-heap lock are commit growth and window creation, both rare.

**Profiles:**

| Profile | decay | dirty limit | swing cap | advice |
|---|---|---|---|---|
| balanced | 1 s | max(32 MiB, used/8) | used | DONTNEED |
| memory | 100 ms | max(8 MiB, used/32) | used/4 | DONTNEED, no spares |
| throughput | 10 s | max(256 MiB, used/2) | 2 × used | MADV_FREE (+MADV_HUGEPAGE if THP=madvise) |

## 6. Locks and their order

All locks are a 3-state futex mutex (`soham_lock_t`). The only order that ever nests:

```
heap_lock -> shared_lock -> pool[c].lock -> ph.lock -> solo_lock -> meta_lock
```

The fork-prepare handler takes them in exactly this order; the parent and child handlers release or reset them.

- `resolve` releases the pool lock before `page_release` takes `ph.lock`, so those two never nest.
- In handler context (`trymode`) every lock below `shared_lock` is a trylock. A failed trylock makes the allocation fall back to the static arena.

## 7. Fork

- **prepare:** take every lock in the order above.
- **parent:** release them.
- **child:** reset every lock. Every other thread's heap that was not inside an operation (its owner's `busy` byte is readable in the child) goes to the heap pool as-is, touching no page metadata, which keeps the child copy-on-write friendly. A heap that was inside an operation is leaked. The child re-seeds the RNG.

## 8. Deviations from IMPL_SPEC_v0 (and why)

1. **Owner lists plus notification instead of detach-on-full.**
   - The spec detached a full page to ORPHAN so frees could make it shared. Measured on a single-thread mixed workload, that sent most frees through the remote CAS path and left sohamalloc at 0.16 s against mimalloc's 0.09 s.
   - Pages now stay owned (cur/avail/full lists), as in mimalloc. Remote frees to full pages notify the owner per page.
   - The detach/pool protocol remains for thread exit, so exited threads strand nothing.
2. **The free word is split into a head and a count.** Neither signal handlers nor other threads ever touch the owner line, so a packed single-store commit buys nothing. The split removes packing work from both fast paths and the 64-bit atomics on 32-bit targets.
3. **Excess release.** Not in the spec. The page heap releases free memory beyond the dirty limit immediately, so a program that frees 1 GiB drops to about 50 MB RSS without `malloc_trim`. glibc, jemalloc and mimalloc keep about 1 GiB, and tcmalloc keeps 628 MB (see `bench/`).
4. **Window size.** 4 GiB on 64-bit, not the plan's 64 GiB: less VSZ, and 32-bit targets are possible.
5. **No orphan list.** An orphaned page with less than 1/8 free waits on no list until frees make it PENDING. This strands at most 1/8 of each such page.
6. **Handler mallocs.** The shared heap under trylock, in `trymode`, instead of the static arena only. This fixes `tests/regress/signal_reentrant_enomem.c`.
7. **C++ operators in C.** The spec had a C++ translation unit. The operators are now C functions with the mangled names (`__asm__` labels). The throwing forms call the new_handler through a weak `std::get_new_handler` and throw through a weak `std::__throw_bad_alloc` (the C frames have unwind tables). The nothrow forms must return null when the handler throws, which needs a catch that C cannot write. On failure with a handler installed, they delegate to the C++ runtime's own nothrow operator (found once with `dlsym(RTLD_NEXT)`), which is exactly `try { return ::operator new(n); } catch (...) { return nullptr; }`, and its `::operator new` resolves to ours.
8. **Owner field placement.** owner, key and pbase live in the head record only (plus owner-side copies of key, pbase and psize in line O). Non-head slices carry only `head_delta` and `kind`.

## 9. Known limitations (v0.1)

- **Idle threads keep memory.** A thread that stops allocating keeps its owned pages until it exits. The plan's idle-slot lease (membarrier) is P4.
- **No background thread.** Decay runs only on allocator activity. Excess release covers bursts.
- **Alignments above 64 KiB** use hugeblock-aligned runs up to 2 MiB, and solos (an mmap each) beyond that.
- **No allocator override on macOS or Windows.** The `soham_*` API works there; Windows passes the stress suite under wine.
- **The `rseq` per-CPU mode is not implemented** (plan P4).
- **The fast paths are longer than mimalloc's**: about 41 instructions for malloc and 50 for free, against about 18 and 29. The difference is the security and safety work: link encoding with a bounds check, foreign-pointer detection through the granule map, and the busy byte for signal safety. `soham_malloc` and `soham_free` are 64-byte aligned: on the test machine glibc-simple ran 2.6–2.9 s aligned and 4.1 s unaligned with identical code, depending only on what the linker placed before them.
