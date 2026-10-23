# sohamalloc

A general-purpose memory allocator (`malloc`/`free`/`new`/`delete`) written from scratch in C11. It aims to be fast, memory-efficient, kernel-friendly and safe by default, and to explain its own memory use.

```
LD_PRELOAD=/path/to/libsohamalloc.so ./your-program   # drop-in, no code changes
```

## What makes it different

| Feature | What it means for you |
|---|---|
| **Gives memory back automatically** | Free resident memory above a small limit goes back to the kernel at once, in batched `process_madvise` calls. Freeing 1 GiB leaves about 50 MB RSS. glibc, jemalloc and mimalloc keep about 1 GiB until `malloc_trim`, and tcmalloc keeps about 600 MB. The limit follows the recent swing of the live size, so a program that repeatedly frees and re-allocates big buffers keeps the memory it is about to reuse instead of re-faulting it. |
| **Out-of-line metadata** | Allocator bookkeeping lives in a separate, guard-separated area (192-byte slice records), not next to your data. A heap overflow cannot reach it. The only allocator state inside your memory is the encoded free-list link of each *free* block (next row). |
| **Encoded free lists** | Free-list links are stored as `bswap(next ^ key ^ addr>>12)` with a per-page random key. Every decoded link must name an exact block boundary in its own page, or the allocator aborts. A blind use-after-free or overflow write into a free block is detected rather than turned into an arbitrary write. An attacker who can *read* a free block's link and knows its successor can recover that page's key (as with glibc safe-linking); the damage then stays inside that page. |
| **Invalid and double free detection** | A granule map identifies sohamalloc's memory: `free()` of a stack, global or foreign pointer is leaked with a warning (aborted in secure mode). A free that is not the exact start of a block aborts. Double frees abort, including non-adjacent ones (`free(a); free(b); free(a)`), a free of a block a thread already freed into its page's remote list, and one on a page whose owner has exited. The exception is large mappings (32 MiB and more) in the default tier: a repeated free of one is ignored with a warning; the secure tier aborts. glibc catches the first kind but not the second. The check costs 1–7% on allocation-heavy microbenchmarks. |
| **Signal-handler safe** | The malloc family, the aligned family and C++ `new`/`delete` may be called from a signal handler, even one that interrupted the allocator on its own thread. Such a call never deadlocks or corrupts the allocator: it never waits for a lock and uses a shared heap or a static arena instead. This is part of the design, not luck. POSIX does not require it: glibc and jemalloc hang in our test (`tests/regress/signal_reentrant_enomem.c`), and mimalloc and tcmalloc pass that test but do not promise this. **Limits:** an allocation in a handler can return NULL while memory is available, and the handler must return normally (no `siglongjmp` out of an interrupted call). The full list is in [`docs/IMPLEMENTATION_NOTES.md` §10](docs/IMPLEMENTATION_NOTES.md). |
| **Hugepage-aware page heap** | 2 MiB hugeblocks are allocated best-fit, and memory is released whole-hugeblock first. |
| **Thread-friendly** | Per-thread heaps, lock-free cross-thread frees (one CAS), and owner notification for producer/consumer patterns. Exited threads hand their pages to shared pools, so no memory is stranded. |
| **Explain my memory** | `SOHAMALLOC_STATS=1` or `soham_stats_print(fd)` prints where every resident byte is: in use, cached in thread heaps, in shared pools, in the page heap, metadata, large mappings, or released. |
| **Portable** | Linux x86-64 and arm64 (4K/16K/64K pages), 32-bit ARM, musl, and Windows (`soham_*` API, tested under wine). |

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

| Artifact | Use |
|---|---|
| `build/libsohamalloc.so` | `LD_PRELOAD` override of the whole malloc family, the glibc aliases and C++ `new`/`delete`. Depends on libc only (the C++ operators are written in C), so preloading it into a C program adds no libstdc++ |
| `build/libsohamalloc.a` | Static override: link it into your program |
| `build/libsohamalloc-prefixed.a` | Only the `soham_*` API (no override). Use it to embed sohamalloc next to another allocator, or for sanitizer testing |

Options:
- `-DSOHAM_DEBUG=ON`: internal assertions.
- `-DSOHAM_SANITIZE=thread|address|undefined`: sanitizer build of the prefixed library.
- `-DSOHAM_CXX=OFF`: leave out the C++ operator overrides.

## Configure (environment)

| Variable | Effect |
|---|---|
| `SOHAMALLOC_PROFILE=balanced` (default) | 1 s decay. Keeps at most max(32 MiB, 1/8 of in-use) of free resident memory. Releases with `MADV_DONTNEED` (RSS drops at once) |
| `SOHAMALLOC_PROFILE=memory` | 100 ms decay. Keeps at most max(8 MiB, 1/32 of in-use). No spare pages |
| `SOHAMALLOC_PROFILE=throughput` | 10 s decay. Keeps up to max(256 MiB, 1/2 of in-use). `MADV_FREE`; transparent hugepages when THP is set to `madvise` |
| `SOHAMALLOC_SECURE=1` | Security tier S1: shuffled block order, exact block-boundary checks on `free`, abort on foreign pointers, zeroing of small blocks on free |
| `SOHAMALLOC_STATS=1` | Print the memory report to stderr at exit |
| `SOHAMALLOC_VERBOSE=1` | Warn about leaked foreign pointers and similar events |

These variables are ignored in set-uid programs.

## API (`include/sohamalloc.h`)

**Standard functions.** `soham_malloc`, `soham_calloc`, `soham_realloc`, `soham_reallocarray`, `soham_free`, `soham_free_sized`, `soham_free_aligned_sized`, `soham_aligned_alloc`, `soham_posix_memalign`, `soham_memalign`, `soham_usable_size`.

**Extensions:**
- `soham_malloc_at_least(n, &usable)`: size feedback.
- `soham_collect(force)`: release free memory now.
- `soham_stats_print(fd)`: the memory report.
- `soham_get_stats(&s)`: a statistics snapshot.
- `soham_check_heap()`: consistency check.
- `soham_version()`.

The override builds also export `malloc`, `free`, `calloc`, `realloc`, `reallocarray`, `aligned_alloc`, `posix_memalign`, `memalign`, `valloc`, `pvalloc`, `malloc_usable_size`, `free_sized`, `free_aligned_sized`, `malloc_trim`, `mallopt`, `malloc_stats`, `mallinfo`, `mallinfo2`, the `__libc_*` aliases, and every global C++ `operator new`/`delete`.

## Verified

- **Test suite** (`tests/run_tests.sh`): API semantics, size sweeps, randomized differential checking, threads, producer/consumer, thread churn, fork, signals, large and huge blocks, C++, memory return, statistics, and internal properties.
- **Regression tests** (`tests/regress/`): one per defect found in review, each shown to fail on the code before its fix and pass after it (`docs/research-log/03-defects-and-faults.md`).
- **Signal storms** (`tools/sigstorm`): allocation from signal handlers at a high rate, against sohamalloc and eight other allocators.
- **Real programs** under `LD_PRELOAD` (`tests/preload_smoke.sh`): python3, gcc, g++, git, perl, sort, find, tar and bash.
- **Concurrency stress** with checksums, clean under ThreadSanitizer (0 races), AddressSanitizer and UBSan.
- **Same stress** on aarch64 and armhf (qemu), musl, and Windows (mingw + wine).

## Design documents

| Document | Contents |
|---|---|
| [`docs/RESEARCH_AND_DESIGN.md`](docs/RESEARCH_AND_DESIGN.md) | First survey and design (v1) |
| [`docs/research/`](docs/research) | Five deep-research reports, plus a 101-fault critique of v1 |
| [`docs/designs/`](docs/designs) | Three competing designs |
| [`docs/SOHAMALLOC_PLAN.md`](docs/SOHAMALLOC_PLAN.md) | The judged, red-teamed architecture plan |
| [`docs/IMPL_SPEC_v0.md`](docs/IMPL_SPEC_v0.md) | The v0.1 build spec |
| [`docs/IMPLEMENTATION_NOTES.md`](docs/IMPLEMENTATION_NOTES.md) | The design as built: data structures, lock-free protocols, deviations and limitations |
| `bench/` | Benchmark harness and results against glibc, jemalloc, mimalloc and tcmalloc |
