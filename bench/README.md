# sohamalloc benchmark harness

This directory compares sohamalloc against glibc malloc, jemalloc, mimalloc and tcmalloc. It runs each benchmark under each allocator via `LD_PRELOAD` and reports wall time, peak RSS and their geometric means. It implements gate G5 of `docs/IMPL_SPEC_v0.md` §9.

```
bench/build_bench.sh          build everything into bench/build/ (git-ignored)
bench/run_bench.sh            run, write bench/out/results-<stamp>.csv + bench/out/report.md
bench/lib/runner.py           the runner (run_bench.sh execs it)
bench/lib/report.py           CSV -> report.md (re-runnable: python3 bench/lib/report.py <csv>)
bench/cmake/CMakeLists.txt    builds the mimalloc-bench subset with upstream's flags
bench/workloads/              realistic workloads: py_alloc.py, gen_cxx.py, sqlite_workload.sql
```

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4   # sohamalloc
MIMALLOC_BENCH_DIR=/path/to/mimalloc-bench bench/build_bench.sh            # or let it clone
bench/run_bench.sh                       # all 5 allocators, 3 reps, ~22 min on 4 vCPUs
bench/run_bench.sh --quick               # 1 rep, smaller arguments, ~4 min
bench/run_bench.sh --only cfrac,larsonN,redis --allocs "mimalloc sohamalloc" --reps 5
bench/run_bench.sh --list                # benchmark names and full commands
```

Options for `run_bench.sh`:

| Option | Meaning |
|---|---|
| `--allocs "a b c"` | Allocators to compare. Default `glibc jemalloc mimalloc tcmalloc sohamalloc` |
| `--reps N` | Number of repetitions. Default 3, or 1 with `--quick`. The report uses the median |
| `--quick` | Smaller arguments where a benchmark has a size knob, and 1 repetition |
| `--only a,b,x*` | Run only these benchmarks. A trailing `*` matches a prefix, e.g. `cache-*` |
| `--skip a,b` | Leave these benchmarks out |
| `--threads N` | Thread count for the `*N` benchmarks. Default 4, the core count of the reference machine |
| `--timeout S` | Per-run timeout in seconds. Default 180; a hang is recorded as `timeout` |
| `--keep-logs` | Keep stdout and stderr of successful runs. Logs of failed runs are always kept |

`SOHAM_LIB=/path/libsohamalloc.so` benchmarks a different sohamalloc build.

## What is measured

| Benchmark | Command (in `bench/build/mb`, N = 4) | Metric | Notes |
|---|---|---|---|
| cfrac | `./cfrac 175451865205073170563711388363274837927895` | wall | Upstream uses a 44-digit number (about 9 s); we use the 42-digit one from older mimalloc-bench (about 3 s) |
| espresso | `./espresso largest.espresso` | wall | |
| barnes | `./barnes < barnes.input` | wall | 81920 bodies instead of upstream's 163840 |
| larsonN | `./larson 2 8 1000 5000 100 4141 N` | relative † | Upstream runs 5 s; we run 2 s |
| larsonN-sized | `./larson-sized 2 8 1000 5000 100 4141 N` | relative † | Same as larsonN, with sized `delete` |
| sh6benchN | `./sh6bench N` | wall | Upstream uses 2×procs threads; we use N = 4 |
| sh8benchN | `./sh8bench N` | wall | Same as sh6benchN |
| xmalloc-testN | `./xmalloc-test -w N -t 2 -s 64` | relative † | Upstream runs `-t 5` |
| cache-thrash1 / N | `./cache-thrash {1,N} 1000 1 2000000 N` | wall | |
| cache-scratch1 / N | `./cache-scratch {1,N} 1000 1 2000000 N` | wall | |
| malloc-large | `./malloc-large` | wall | 500 iterations instead of 2000 (jemalloc needs 26 s for 2000) |
| mstressN | `./mstress N 50 25` | wall | |
| rptestN | `rptest N 0 1 2 150 1000 100 8 16000` | relative † | 150 loops instead of 500. Its 1 s idle sleeps between phases are cut to 100 ms |
| glibc-simple | `./glibc-simple` | wall | |
| glibc-thread | `./glibc-thread N` | relative † | Runs a fixed 2 s; relative time = 1e9 / iterations |
| alloc-test1 / N | `./alloc-test {1,N}` | wall | 40M operations instead of 100M |
| python | `python3 py_alloc.py 5` | wall | CPython with pymalloc: only objects over 512 B, plus arenas, reach malloc |
| python-malloc | `PYTHONMALLOC=malloc python3 py_alloc.py 4` | wall | Every Python object goes through malloc |
| gxx | `g++ -std=c++17 -O2 -c tmpl.cpp` | wall | Compiles a generated 3-module template-heavy TU. `LD_PRELOAD` reaches the driver, cc1plus and as |
| sqlite | `sqlite3 :memory: < sqlite.sql` | wall | 250k rows: bulk insert, two indexes, GROUP BY, sort, self-join, UPDATE/DELETE, JSON aggregation, REINDEX |
| redis | `redis-server` on a unix socket; client `redis-benchmark -n 100000 -P 16 -r 100000 -d 64 -t set,get,lpush,lpop,sadd,hset,zadd,lrange_100` | relative † | Only the server is preloaded. Time = Σ n / rps over the 9 tests. RSS is the server's |

† These benchmarks run for a fixed time and report throughput. As in mimalloc-bench's `bench.sh`, the throughput is turned into a "relative time" (lower is better). The CSV still records the wall time.

**Correctness checks.** Deterministic benchmarks are checked so that a corrupted run cannot count as a fast run:

- **Output digest:** cfrac, espresso, python, python-malloc and sqlite (stdout), and gxx (the `.o` file). Each digest is compared with the first glibc run. A mismatch is recorded as `wrong-output`.
- **Expected-output regex:** barnes, shbench, alloc-test and mstress. A missing match is recorded as `bad-output`.
- **Exit status, signals and hangs:** recorded as `exit-N`, `signal-N` or `timeout`.

A failing run is excluded from the medians. It appears in the report's Failures table with its command line and the tail of its stderr.

**Build-time patches.** `build_bench.sh` applies only the four scale patches listed in the table to its private copy of the sources (malloc-large, alloc-test, rptest's sleeps, barnes' input). Each patch lowers an iteration count or an idle wait, so the access pattern is unchanged. `bench/build/manifest.txt` records what was applied.

## Methodology

- **Interleaving.** For each repetition, every benchmark runs once per allocator, so allocators are interleaved and slow drift on the VM spreads over all of them. The allocator order rotates by one position per repetition and per benchmark, so no allocator always runs first, for example right after a memory-hungry neighbour.
- **Isolation.**
  - `LD_PRELOAD` is set through `env` after `/usr/bin/time`, so only the benchmark's process tree uses the allocator under test, not the measuring tools.
  - The environment is scrubbed of `MALLOC_*`, `MIMALLOC_*`, `JEMALLOC_*`, `TCMALLOC_*`, `SOHAMALLOC_*`, `GLIBC_TUNABLES` and `PYTHONMALLOC` (except where a benchmark sets it).
  - Every allocator runs with its default configuration.
- **Measurement.** Each run is wrapped in `/usr/bin/time -f '%e %M %U %S'`:
  - `%e` is wall seconds, with 10 ms resolution.
  - `%M` is peak RSS in KiB. It is the maximum over the waited-for process tree, so for gxx it is cc1plus's peak.
  - `%U` and `%S` are user and system CPU time, recorded in the CSV.
- **Snapshots.** At start the runner copies every allocator `.so` into `bench/out/run-<stamp>/` and runs the copies. A concurrent rebuild of `build/libsohamalloc.so` therefore cannot mix versions within a run. `env.txt` records each library's sha256 and the sohamalloc git revision, flagged if `src/` was dirty.
- **Aggregation** (`lib/report.py`):
  - Each cell is the median over the successful repetitions.
  - Each benchmark is normalised to its best allocator (1.00 = best), separately for time and for RSS.
  - Geometric means of the normalised time and RSS are computed over the benchmarks where every allocator succeeded, for all benchmarks and separately for mimalloc-bench only and for the five apps.
  - The combined score is `sqrt(geomean(time_norm × rss_norm))`, where lower is better and 1.00 means best at both everywhere.
  - Wins and losses count best and worst per benchmark. A result within 2% of the best counts as best.
  - A head-to-head table compares sohamalloc with each other allocator.
- **Noise indicator.** The `spread` column is the largest (max − min) / median of the time over the repetitions among the allocators. Treat differences smaller than the spread as noise.

## Limitations (read before quoting numbers)

- **Shared, noisy VM.** The reference machine is a 4-vCPU cloud VM (Xeon @ 2.8 GHz, Linux 6.18) with no CPU pinning, frequency control or isolation, and other tenants. Run-to-run spread of 5–10% is normal, and more on the multi-threaded and fixed-duration benchmarks. Three repetitions with a median give a rough estimate, not a statistical test. For decisions, rerun the benchmarks in question with `--reps 7` or more.
- **Only 4 cores.** All `*N` benchmarks use 4 threads. Allocators built for many cores (per-CPU caches, many arenas) cannot show scaling behaviour here, and contention effects are muted.
- **Competitor versions** are the Ubuntu 24.04 packages, not the upstream heads that mimalloc-bench builds:
  - jemalloc 5.3.0 (`libjemalloc2 5.3.0-2build1`), with default config and background threads off.
  - mimalloc 2.1.2 (`libmimalloc2.0 2.1.2+ds-2`). That is the v2 line, not mimalloc v3, and the distro build has secure mode off.
  - tcmalloc means gperftools tcmalloc-minimal 2.15 (`libtcmalloc-minimal4t64`). It is **not** Google's current TCMalloc (github.com/google/tcmalloc, which has per-CPU caches via rseq and huge-page awareness); results say nothing about the latter.
  - glibc 2.39 ptmalloc2 with default tunables.
- **Scaled-down arguments.** Arguments are scaled down from mimalloc-bench (see the table) so that 5 allocators × 3 reps fit in about 25 minutes. Absolute times are therefore not comparable with published mimalloc-bench numbers, though ratios mostly are. Short benchmarks (cache-thrashN and cache-scratchN at about 0.5 s, mstress at about 0.7 s) are close to the 10 ms timer resolution and to process start-up cost.
- **RSS includes the baseline process.** For small benchmarks (cfrac, espresso, the cache tests and glibc-*) peak RSS is 2–8 MiB. Its normalised ratio is then dominated by fixed per-allocator overhead (library size, initial arenas, metadata reservations) rather than by fragmentation. A 2× RSS ratio on a 3 MiB process is 3 MiB. Look at the absolute MiB table as well as the ratios.
- **Peak RSS only.** `%M` is a peak, not an average or the final footprint. Allocators that return memory late (decay timers) and allocators that never return it look the same if the peak is the same.
- **redis is the distro build.** It links libjemalloc for `mallctl`, `nallocx` and `malloc_stats_print`, while `malloc`, `free`, `calloc`, `realloc` and `malloc_usable_size` are interposed by the preload. mimalloc-bench instead builds redis with `USE_JEMALLOC=no`. Under preload, redis' `INFO memory` allocator statistics are meaningless, but the workload itself is valid. The client runs with glibc, and the client and server share the 4 cores.
- **CPython uses pymalloc.** The `python` benchmark mostly stresses pymalloc arenas and medium or large objects. `python-malloc` (`PYTHONMALLOC=malloc`) is the variant where the allocator under test sees every object.
- **No security benchmark.** mimalloc-bench's `security` suite and the large external workloads (lean, z3, rocksdb, SPEC, Linux build, ruby) are not part of this harness, because they are unavailable offline or too long for the time budget.

## Build notes and skips

- **mimalloc-bench** is pinned at commit `391382606681408e34d96ec54e9e6c79805aa3ac` (2026-10-17). `build_bench.sh` uses `--mb DIR` or `$MIMALLOC_BENCH_DIR`, or clones the pinned commit into `bench/build/mimalloc-bench` when the network is available. It builds from a private copy in `bench/build/mb-src`, with upstream's compile flags: `-O3` Release, `-std=gnu89` for the K&R programs, and the same `-D` defines.
- **shbench (sh6bench, sh8bench)** is MicroQuill code that is not redistributable. `build_bench.sh` downloads `bench.zip` and `SH8BENCH.zip`, checks the same sha256 sums as upstream's `build-bench-env.sh`, and applies upstream's patches. With `--no-download` and no cached zips, both benchmarks are skipped, and the skip is recorded in `manifest.txt`.
- **Not built:**
  - `mleak` is built but not run, because it measures leaks rather than performance.
  - `malloc-large-old` is superseded by `malloc-large`.
  - `security/` is out of scope; see above.
- **Realistic workloads:**
  - `python3`, `g++` and `sqlite3` are taken from the system. `build_bench.sh` installs `sqlite3`, `redis-server` and `time` with `apt-get` when they are missing and it runs as root.
  - The g++ input is generated deterministically by `workloads/gen_cxx.py`.
  - The sqlite script is deterministic. `randomblob()` only affects payload bytes, never output.
- **Every benchmark can be skipped.** The runner skips a benchmark whose binary or input is missing and lists it in `env.txt` and the report header.

## Outputs

`bench/out/` is git-ignored.

- **`results-<stamp>.csv`** has one row per run, with the columns `stamp,rep,bench,alloc,status,time_s,wall_s,rss_kb,user_s,sys_s,metric,detail`:
  - `time_s` is the metric used in the report (wall time, or relative time for † benchmarks).
  - `detail` holds the output digest, or for a failure the command and the tail of stderr.
- **`report.md`** is the report of the latest run. `report-<stamp>.md` is a copy kept per run.
- **`run-<stamp>/`** holds the allocator snapshots, `env.txt`, and `logs/<bench>.<alloc>.<rep>.{out,err,time}` for failed runs (for all runs with `--keep-logs`).
