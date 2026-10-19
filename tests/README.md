# sohamalloc test suite

Everything here is allocator-agnostic unless marked *sohamalloc-only*: a test
exercises whatever `malloc` the process uses, so the suite is validated against
glibc, jemalloc, mimalloc and tcmalloc as well as sohamalloc. The contract
under test is `docs/IMPL_SPEC_v0.md` §2 (glibc semantics where both define
them).

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4

tests/run_tests.sh --quick                               # libc allocator (validates the tests)
tests/run_tests.sh --lib build/libsohamalloc.so          # sohamalloc via LD_PRELOAD (full)
tests/run_tests.sh --link build/libsohamalloc.a          # sohamalloc linked statically
tests/run_tests.sh --prefixed                            # soham_* API, build/libsohamalloc-prefixed.a
tests/run_tests.sh --shim                                # prefixed tests against the libc shim
tests/preload_smoke.sh --lib build/libsohamalloc.so      # real programs, outputs diffed
```

`--quick` sets `TEST_SCALE=0.2` (about 40 s per allocator natively); a full
run takes a few minutes. `--only=a,b` / `--skip=a,b` select tests, `--seed=N`
and `--scale=F` override `TEST_SEED` / `TEST_SCALE`, `--cc=clang` switches
compilers, `--verbose` prints every log tail, `--list` lists the tests.
Binaries and logs go to `build-tests/<variant>/` (git-ignored); the log of a
test is `build-tests/<variant>/logs/<test>.log`.

Results: `PASS`, `FAIL`, `SKIP` (exit 77: the test does not apply, e.g. a
sohamalloc-only test under glibc) and `INFO` (an informational test failed on
a non-sohamalloc allocator). Exit status 3 from a test means its watchdog saw
no progress (probable deadlock). `NOTE` lines in a log record behaviour that
the C standard allows but the spec pins differently; they fail only for
sohamalloc and glibc.

## Tests (`tests/*.c`, `tests/*.cpp`)

| Test | What it checks |
|---|---|
| `api_semantics` | Spec §2 contract: malloc(0), free(NULL), realloc edge cases and failure, calloc/reallocarray overflow, posix_memalign/aligned_alloc/memalign/valloc/pvalloc, usable size, ENOMEM everywhere, free preserves errno, C23 free_sized, allocation from a constructor and as a thread's first call, sohamalloc extensions and glibc compatibility symbols |
| `sizes_sweep` | Every size class boundary, alignment, usable range writable, overlap oracle |
| `random_ops` | Randomised differential test of 2e6 operations against a shadow oracle |
| `threads_local` | Per-thread allocate/free workloads |
| `producer_consumer` | Cross-thread frees through lock-free queues; orphaned pages |
| `thread_churn` | Thread creation/exit storms, TSD and C11 tss destructors that allocate |
| `fork_test` | fork() while other threads allocate; child allocator usable |
| `signals` | malloc/free from SIGALRM, SIGPROF and SIGUSR1 handlers that interrupt the allocator (nested), 8 s; *required for sohamalloc, informational elsewhere* (glibc, jemalloc and tcmalloc deadlock, mimalloc corrupts) |
| `large_and_huge` | Runs (192 KiB..32 MiB) and solos (>= 32 MiB, up to 1 GiB): boundary sweep, calloc of dirty memory, realloc chains to 512 MiB, alignments up to 64 MiB, a pool of large blocks, cross-thread large frees, address-space reuse |
| `cxx_test` | Every operator new/delete form, alignas, bad_alloc and the new_handler loop, provenance of the operators (sohamalloc), containers, exceptions, threads |
| `memory_return` | *sohamalloc-only.* RSS (from `/proc/self/statm`) must drop by >= 80% after freeing 1 GiB (256 MiB on 32-bit) and calling `soham_collect(1)`/`malloc_trim(0)`: small, medium, large/solo, exited threads, remote frees, partial fragmentation, reuse |
| `stats_api` | *sohamalloc-only.* `soham_get_stats` coherence and counters, heap pooling, `soham_stats_print`/`malloc_stats`/`mallinfo2` output, `SOHAMALLOC_STATS=1` at-exit report, `soham_check_heap() == 0` during and after a stress |
| `internal_props` | *sohamalloc-only.* Property tests over `src/internal_test.h`: size->class monotone and tight, class sizes, page kinds/sizes/caps, geometry, free-list link encoding |
| `regress_*` | Reproducers of allocator bugs found by this suite (`tests/regress/`), run only against sohamalloc |

`tests/prefixed/` holds the variants that call `soham_*` directly
(`api_semantics`, `random_ops`, `producer_consumer`, `thread_churn`,
`stats_api`, `internal_props`; most just `#include` the main source with
`-DTEST_PREFIXED`) and `libc_shim.c`, which implements the `soham_*` API on top
of libc so the prefixed tests themselves can be validated (`--shim`).

## Sanitizers

The prefixed tests run under TSan, ASan and UBSan against a sanitizer-built
prefixed library:

```sh
cmake -S . -B build-tsan  -DCMAKE_BUILD_TYPE=Release -DSOHAM_SANITIZE=thread    && cmake --build build-tsan  --target sohamalloc_prefixed
cmake -S . -B build-asan  -DCMAKE_BUILD_TYPE=Release -DSOHAM_SANITIZE=address   && cmake --build build-asan  --target sohamalloc_prefixed
cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Release -DSOHAM_SANITIZE=undefined && cmake --build build-ubsan --target sohamalloc_prefixed
tests/run_tests.sh --sanitize=thread    --prefixed-lib build-tsan/libsohamalloc-prefixed.a
tests/run_tests.sh --sanitize=address   --prefixed-lib build-asan/libsohamalloc-prefixed.a
tests/run_tests.sh --sanitize=undefined --prefixed-lib build-ubsan/libsohamalloc-prefixed.a
tests/run_tests.sh --sanitize=thread --shim               # the tests alone under TSan
```

A sanitizer report anywhere in a log fails the test.

## Cross targets

```sh
# aarch64 / armhf (32-bit) under qemu-user
cmake -S . -B build-aarch64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
      -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ && cmake --build build-aarch64 -j4
tests/run_tests.sh --cross aarch64 --quick --lib build-aarch64/libsohamalloc.so
cmake -S . -B build-armhf -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
      -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc -DCMAKE_CXX_COMPILER=arm-linux-gnueabihf-g++ && cmake --build build-armhf -j4
tests/run_tests.sh --cross armhf --quick --lib build-armhf/libsohamalloc.so

# musl (x86-64; C only: musl-gcc has no C++), allocator built by hand without the C++ operators
mkdir -p build-musl
F="-std=c11 -O2 -fPIC -fvisibility=hidden -ftls-model=initial-exec -fno-builtin-malloc -fno-builtin-free -fno-builtin-calloc -fno-builtin-realloc -Iinclude"
musl-gcc $F -DSOHAM_OVERRIDE=1 -shared src/sohamalloc.c src/os_posix.c -o build-musl/libsohamalloc.so
tests/run_tests.sh --cross musl --quick --lib build-musl/libsohamalloc.so

# mingw-w64 under wine: prefixed library and prefixed tests only
cmake -S . -B build-mingw -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix
cmake --build build-mingw --target sohamalloc_prefixed
tests/run_tests.sh --cross mingw --quick --prefixed-lib build-mingw/libsohamalloc-prefixed.a
tests/run_tests.sh --cross mingw --quick --shim
```

Timeouts are multiplied for emulated targets. Tests that re-exec themselves
(`stats_api`'s `SOHAMALLOC_STATS=1` check) only do so natively
(`TEST_NATIVE=1`, exported by the runner).

## preload_smoke.sh

`tests/preload_smoke.sh [--lib PATH] [--only=a,b] [--keep] [--verbose]` runs
real programs twice, without and with `LD_PRELOAD=PATH` (default
`build/libsohamalloc.so`), in a fresh directory with a fixed environment, and
fails on a crash, a different exit status, different stdout/produced files, or
stderr the reference run did not print. Cases: `python` (tests/data/workload.py),
`gcc` (tests/data/hello.c and the generated tests/data/bigger.c; object files
must be byte-identical), `gxx` (tests/data/templates.cpp), `git` (temporary
repository: init, add, commits, log, gc, fsck), `perl` (hash-heavy one-liner),
`sort` (`sort -S 10M` of 200k lines), `find` (`find /usr/include | wc -l`),
`tar` (czf/xzf of /usr/include, listing and extracted tree compared), `bash`
(string/array loop). About 30 s.

`tests/data/bigger.c` is generated: `python3 -I tests/data/gen_bigger.py > tests/data/bigger.c`.

## Writing tests

Use `tests/common/test_util.h`: `T_MALLOC` and friends select the malloc
family or `soham_*` (`-DTEST_PREFIXED`); failures are reported with
`TU_FAIL`/`TU_CHECK`, which never allocate; bookkeeping lives in memory from
`tu_os_alloc` (straight from the OS); `tu_fill`/`tu_check` write and verify
seeded patterns; `tu_ranges` is the overlap oracle; `tu_watchdog_main` +
`tu_tick` turn a deadlock into exit status 3; `tu_launder` hides a pointer
from `_FORTIFY_SOURCE` when a test writes the whole usable size; and
`tu_is_soham` is cached, so it is safe in signal handlers once called from
`main`. Exit 77 to SKIP. New tests go in the table in `run_tests.sh`.
