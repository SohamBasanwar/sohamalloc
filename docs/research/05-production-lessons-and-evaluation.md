# Allocator failures in production, evaluation methods and drop-in compatibility: evidence report for sohamalloc (as of 2026-10-18)

Each claim carries a source tag such as [S12]; the numbers match the Sources list at the end. "(snippet)" means I saw the source only through a search-engine extract, because the page was behind a login or returned 403/503 when fetched. Treat those as partly verified. **UNVERIFIED** marks claims I could not confirm.

---

## 0. The ten lessons that matter most

1. **The commonest production failure is retained RSS, not a slow fast path.** In case after case the live heap is small and RSS is 2–10x larger. The memory sits in arenas, caches or freed-but-unreleased pages [S1][S2][S4][S5]. Users usually read it as a memory leak.
2. **Thread churn causes most of the "modern allocator does not return memory" reports.** This includes thread pools, short-lived worker threads and Tokio blocking pools, and it affects mimalloc, jemalloc, glibc and Node [S4][S14][S15][S17].
3. **Purging on a timer alone fails when the program goes idle.** jemalloc 5's decay made no progress in idle programs until a background thread was added [S13].
4. **Being honest about RSS beats raw speed when users run monitoring.** Go moved from MADV_FREE back to MADV_DONTNEED by default because of confused users, even though DONTNEED costs 1.5–2x more on its own [S25][S26].
5. **Huge pages cut both ways.** TCMalloc's Temeraire gained 7.7% requests per second while also cutting RAM 2.4% [S22]. Elsewhere THP caused 339 ms fork latency spikes in Redis [S32]. With THP=always, Meta saw 7.7% more memory [S35]. And khugepaged can undo a purge [S33].
6. **Containers break assumptions about CPUs and kernel registration.** Offline or holey CPU ids crashed TCMalloc inside Envoy [S46]. glibc's own rseq registration silently disabled TCMalloc's per-CPU caches [S38]. Ambitious per-CPU designs need defensive fallbacks.
7. **Interposition is fragile.** RTLD_DEEPBIND bypasses LD_PRELOAD and is still unfixed [S69]. `--as-needed` can drop the allocator from the binary [S73]. glibc calls `free(NULL)` before the allocator's constructor runs [S71]. The dynamic linker calls malloc during TLS access [S10]. glibc's hooks are gone [S10].
8. **The glibc baseline is moving, so stale baselines mislead.** glibc 2.43 removed fastbins [S9]. glibc 2.44 cut the default arena limit from 8×cores to max(8, ncores) and fixed malloc_trim for non-main arenas [S8][S10].
9. **Standard benchmarks miss the failures above.** mimalloc-bench reports peak RSS only [S59]. SPEC's object lifetimes are unrealistic [S18]. The trace replayer in glibc's experimental dj/malloc branch records no timestamps [S64]. Tail latency is often measured with coordinated omission [S67].
10. **Applications build their own memory control on top of the allocator when it does not provide one.** Examples: Redis active defrag [S48][S49], ClickHouse's memory worker [S31], JDK native-heap trimming [S6], GitLab's decay settings [S30]. That is direct evidence that these features belong inside the allocator.

---

## 1. Production failure patterns

### 1.1 glibc arenas multiply fragmentation

- **Mechanism.** glibc creates extra arenas under thread contention. The historical cap was 8 × cores on 64-bit systems [S1][S12]. Free memory cannot move between arenas, so each arena fragments separately [S2].
- **Ruby/Sidekiq.** Processes grew from about 300 MB to nearly 1 GB; `MALLOC_ARENA_MAX=2` brought them to about 512 MB [S1]. In a Heroku table relative to the pre-arena baseline [S1]:
  - Unlimited arenas: 1.73x memory, 0.9x response time.
  - `MALLOC_ARENA_MAX=2`: 0.87x memory, 1.03x response time.
  - The author's summary: a 10% speed gain bought with a 75% memory increase.
- **Cloudflare Quicksilver (RocksDB) [S2].**
  - About 25 threads produced 60–80 arenas.
  - RSS grew from about 15 GB at start to about 30 GB over days, while the heap profile was about 3x smaller than RSS.
  - Switching to TCMalloc "decreased memory usage by 2.5 times".
  - The listed workarounds (`MALLOC_ARENA_MAX`, periodic `malloc_trim`, `M_MMAP_THRESHOLD`) each trade off lock contention, syscalls or page faults.
- **Node.js worker threads [S4].** 50 workers each loaded a 20 MB file. RSS was about 1100 MB on glibc and about 80 MB with jemalloc. The maintainer closed it as glibc behaviour.
- **LinkedIn Venice (RocksDB block cache through JNI) [S5].** gdb-heap showed unused memory "as high as 30%". jemalloc stabilised RSS, but no absolute figures were published.
- **Hadoop.** One report showed 14 GB of virtual memory against about 300 MB RSS, and Hadoop's scripts set `MALLOC_ARENA_MAX=4` [S80] (snippet). Virtual size alone alarms operators.
- **JDK added native-heap trimming (JDK-8293114, fixed in 22, backported to 17 and 21) [S6].**
  - The rationale: "Temporary malloc spikes often carry over as permanent RSS increase", and the memory is invisible to NMT (the JVM's Native Memory Tracking) [S7].
  - `malloc_trim` locks each arena while it trims. Pauses reached "up to a second" with about 32 GB reclaimable [S7].
- **glibc has started to address this.**
  - A commit dated 2026-04-02 limits arenas to max(8, ncores), noting "big servers have 256 cores" and that the limit "should be lowered further" [S8].
  - It is in the 2.44 tag, which is about 2 months old [S11]. That placement is my inference from the commit and tag dates.
  - 2.44 also fixes bug 30769, "malloc_trim is not working correctly for arenas other than arena 0" [S10].
  - Fastbins were removed in December 2025, so 2.43 [S9].
  - In 2.42 the tcache can hold large blocks via `glibc.malloc.tcache_max`, up to 4 MiB [S10].
  - **Lesson:** benchmark against glibc 2.43/2.44, not the 2.31 that much published data uses [S40].

### 1.2 "Modern" allocators retain memory too: thread churn and idle programs

- **jemalloc #1398 [S13].**
  - Redis's maintainer saw resident memory stay near 599 MB on 5.1, where 4.0.3 dropped back to about 14 MB.
  - Maintainer: "if the application goes completely idle, there won't be progress". The fix was `background_thread:true`.
  - A later user found `opt.retain=true` was the real cause in their case [S13].
- **mimalloc #351, still open [S14].**
  - Each time a new thread from a pool of 20 parsed large XML files, RSS grew about 150 MB.
  - After the thread exited RSS stayed at about 580 MB; `mi_collect(true)` brought it to about 182 MB.
  - Maintainer: mimalloc "is not eager to share memory between threads". There is no confirmed fix.
- **mimalloc #1025 [S15].**
  - The rspack workload grew from 600 MB to over 1 GB across rebuilds on macOS, on both v2 and v3.
  - Thread count rose from 339 to 526, against 199 on Linux. Capping Tokio's `max_blocking_threads` reduced memory.
  - The issue was closed "not planned" in August 2026.
  - rspack's continuous-integration (CI) runs on v3 were mixed. One development-mode hot-reload case went from 1247 to 1470 MiB (+17.9%, with high variance) [S16].
- **jemalloc thread caches.** Jason Evans: thread caches "average ~2.5 MiB per thread under the worst conditions". He recommended flushing from threads about to go idle [S17].
- **TCMalloc per-thread mode.**
  - Each thread cache has a minimum of about 512 KiB, so the total limit can be exceeded [S19].
  - Google moved to per-CPU caches because per-thread caches "strand memory when the threads become idle" and scale worse with thousands of threads [S18].
  - Per-CPU mode costs about 256 KiB per CPU, a multi-MiB footprint on large machines [S19].
  - Google also reports 4x more hyperthreads per server over five platform generations, which motivated dense vCPU ids [S18].
- **Hoard's formal framing.** "Blowup" is peak memory divided by peak memory of an ideal uniprocessor allocator. Under producer-consumer patterns many allocators show blowup from a factor of P (the number of processors) up to unbounded [S79].

### 1.3 musl and static binaries

- Tweag measured a wasmtime workload on 48 cores [S23]:

  | Build | Time |
  |---|---|
  | glibc, dynamic | 4.311 s |
  | musl, static, default malloc | 93.07 s |
  | musl, static, mimalloc | 3.734 s |

  With parallel compilation disabled, glibc and musl performed about the same, which points to allocator contention.
- **Linking was the painful part [S23].**
  - musl's malloc symbols are strong, so `--whole-archive` failed with "multiple definition of `posix_memalign`".
  - The working fix was to patch `libc.a` with `ar` scripts.
- **BetterUp (python:3.13-alpine, musl) [S24].**
  - RSS crept about 1.25 MB/hour; with jemalloc (`background_thread:true`, decay 1000 ms) it was about 0.12 MB/hour.
  - Memory-based autoscaling had been adding pods unnecessarily.

### 1.4 Honest RSS and the MADV_FREE trap

- **Go 1.16 switched its default to MADV_DONTNEED** so that "RSS will more accurately reflect" physical use [S25].
  - The proposal cited "confusing stats in `top`" and "bad integration with management systems that respond to memory usage" [S26].
  - DONTNEED is "1.5-2x" slower on its own [S26].
- **How the kernel shows lazily freed memory.**
  - MADV_FREE pages are freed only under memory pressure [S27].
  - They show up as `LazyFree` in smaps (possibly under-reported) [S28] and as `pglazyfree`/`pglazyfreed` in cgroup `memory.stat` [S29].
- **Applications tune for honesty themselves.**
  - GitLab ships `MALLOC_CONF=dirty_decay_ms:1000,muzzy_decay_ms:1000` and reports "much more stable memory usage" [S30].
  - ClickHouse runs a memory worker that reads cgroup usage every 50 ms (100 ms from jemalloc stats) [S31].
  - It sets jemalloc `dirty_decay_ms=0` after 5000 ms of pressure, and forces a purge when dirty pages exceed 20% of available memory or total use exceeds 90% [S31].
  - This is precedent for a pressure-adaptive allocator, built by hand on top of one.

### 1.5 Transparent hugepage (THP) problems

- **Fork latency and copy-on-write.**
  - antirez (Redis's author) saw a 339 ms max spike with jemalloc against 21 ms with libc malloc, at 675k writes/s with 15M keys. Only jemalloc's region showed `AnonHugePages`. The cause was THP [S32].
  - He also wrote that huge pages are "horrible for memory usage" under copy-on-write [S32].
- **Purges defeated.**
  - A 2014 report: RSS grew while jemalloc's active and allocated stats stayed flat, and only on kernels with THP enabled [S33].
  - A 2012 thread reported khugepaged "merges all the small pages back together" after a partial `MADV_DONTNEED` [S34] (snippet).
  - The kernel's `max_ptes_none` setting controls how many empty pages a collapse may include [S36].
- **Bloat measured at Meta.** On CPU-bound workloads, compared with THP=madvise [S35]:

  | Setting | Memory | Performance |
  |---|---|---|
  | THP=madvise | 54.6 GB | baseline |
  | THP=always | 58.8 GB (+7.7%) | +1.8% |
  | THP=always + underused-THP shrinker, `max_ptes_none=409` | 55.9 GB (+2.4%) | +1.7% |

  Meta runs madvise mode in production because THP=always "vastly overprovisions THPs in sparsely accessed memory areas" [S35]. The shrinker is now documented as `shrink_underused` [S36].
- **TCMalloc's opposite stance.**
  - Temeraire was written because the old `MADV_DONTNEED` release broke up hugepages. Some services gained throughput by "disabling release" [S21].
  - It packs small allocations into the fewest hugepages, ordered by the longest free range, so that fully backed hugepages can be kept and empty ones freed whole [S21].
  - Google builds and tests with THP `always`, defrag `defer+madvise`, `max_ptes_none=0` and `overcommit_memory=1` [S20].
  - MongoDB 8.0 reversed its long-standing "disable THP" advice and now requires exactly those settings for its new TCMalloc [S37].
- **khugepaged CPU stalls.** DataStax docs describe khugepaged at up to 100% CPU blocking other processes [S39]. **UNVERIFIED**: the page now redirects; snippet only.
- **New kernel and glibc controls.**
  - `prctl(PR_SET_THP_DISABLE, 1, PR_THP_DISABLE_EXCEPT_ADVISED, ...)` gives per-process madvise-only THP [S36]. MongoDB's doc says THP cannot be set per process [S37]; the kernel doc now shows otherwise.
  - mTHP (multi-size THP, e.g. 16–64 KiB) gives fewer faults with "much less prominent" latency spikes [S36].
  - glibc 2.44 adds the `glibc.elf.thp` tunable [S10]. glibc 2.43 enables 2 MB THP in malloc by default on AArch64 [S10].
  - mimalloc v3 raises its minimum purge size to 2 MiB when THP is detected [S40].

### 1.6 Kernel interaction: locks, VMAs, address space

- **mmap_lock and per-VMA locks.** Page faults, mmap and munmap have historically contended on the process-wide `mmap_lock`. Per-VMA locks were merged in 6.4; days after release the Go build broke and others saw random crashes, and the fixes followed [S41].
- **VMA limit.** `vm.max_map_count` defaults to 65530; malloc debuggers can use "one or two maps per allocation" [S43].
- **Strict overcommit.** In mode 2, `MAP_NORESERVE` is ignored and private writable anonymous mappings are charged at full size. Read-only private mappings cost nothing [S44]. Large reservations must therefore be made PROT_NONE.
- **Virtual-size limits.** TCMalloc reserves about 1 GiB regions. Its docs warn that limiting memory via virtual size (VSS) "will fail long before" physical exhaustion [S19]. Node users hit OOM under `ulimit -v` [S4].
- **Lightweight guard pages have a hidden cost.** Guard regions installed with `MADV_GUARD_INSTALL` are page-table markers, so "/proc/<pid>/maps and /proc/<pid>/smaps interfaces are useless" for finding them. glibc's adoption broke CRIU's CI [S45].

### 1.7 CPU topology and rseq in containers

- **TCMalloc #188 [S46].**
  - Envoy 1.24 segfaulted in `CpuCache::Refill` on hosts with CPUs 1 and 9 offline, and in containers using lxcfs.
  - Maintainer: "If the kernel provides us a CPU ID that's out of bounds, data corruption can/will occur."
  - Fixed by sizing arrays from the possible-CPU count, plus an assertion added in 2025.
- **rseq registration.**
  - glibc ≥ 2.35 registers rseq (restartable sequences) for every thread [S10].
  - MongoDB found that on glibc ≥ 2.34 this "is incompatible with the new TCMalloc", which silently "fall[s] back to the per-thread cache". The workaround is `GLIBC_TUNABLES=glibc.pthread.rseq=0` [S38][S37].
  - Google's fleetbench requires the same variable [S63].
  - glibc 2.41 exposes the extended rseq fields `mm_cid` and `node_id` through `__rseq_size` [S10].
- **PSI.** Unprivileged users can create pressure monitors only with windows that are a multiple of 2 s (the general range is 500 ms–10 s) [S47].

### 1.8 Applications need allocator APIs to fix fragmentation

- **Redis active defrag [S48][S49].**
  - Off by default and works only with Redis's bundled jemalloc.
  - Defaults: start above 100 MB of waste and 10% fragmentation, full effort at 100%, using 1–25% CPU.
  - Redis patched jemalloc with a function that tells it whether to move a pointer. It compares that slab's use with the average across non-full slabs, with a 12.5% bias. Moved data is reallocated through a path that bypasses the thread cache (`zmalloc_no_tcache`).
- **Mesh** compacts pages without moving objects and cut memory by 16% in Firefox and 39% in Redis [S50].
- **CPython's free-threaded build (PEP 703) [S51].**
  - It needs mimalloc because "pymalloc ... is not thread-safe without the GIL".
  - It also needs heap traversal so the garbage collector (GC) can find objects, separate heaps per object category, and page reuse gated by sequence numbers so that lock-free reads stay safe.
  - Free threading costs 5–8% overall, mostly from biased reference counting, not the allocator.
- **CPython's own allocator (pymalloc).** It frees an arena (1 MiB on 64-bit) only when the arena is entirely empty [S53]. One live object pins the whole arena.

### 1.9 Long-term maintenance is a risk

- jemalloc's repository was archived in June 2025 [S54][S56]. Polars' maintainers wrote "we have been feeling the degradation of jemalloc" [S56].
- **The postmortem's lessons [S54]:**
  - Removing Valgrind support angered Rust and probably got jemalloc "booted from Rust binaries" sooner.
  - The hugepage allocator (HPA) work stalled as "tweaks piled on top of each other".
  - Fleet telemetry was what validated changes.
- Meta unarchived the repository in March 2026 and plans HPA, packing and AArch64 work [S55].
- Rust's 1.32 reasons for dropping jemalloc as default: about 300 KB per binary, and it is "not always" faster [S57].

**Not found:** I found no primary-source allocator incident for Discord, Pinterest or TiKV. Any claim about them is **UNVERIFIED**.

---

## 2. Evaluation methodology

### 2.1 Known flaws in the usual benchmarks

- **mimalloc-bench measures memory once.** It uses GNU `time -f "%E %M %U %S %F %R"`, so memory is a single **peak RSS** value; RSS over time is not recorded [S59].
- **Larson results are converted.** larson is reported as "relative time" computed as 2,000,000 / ops per second [S59].
- **Its own README admits caveats** [S58]:
  - xmalloc-testN memory "should be disregarded" because it "allocates more the faster the program runs". Fixed-time benchmarks reward hoarding memory.
  - The published results come from one 2019 4-core machine.
- **mimalloc's published performance section [S40]:**
  - Last updated 2021-01-30, on glibc 2.31.
  - The "tcmalloc" in it is gperftools-2.8.1, not Google's per-CPU TCMalloc.
  - It states the benchmarks "do not measure the behaviour on very large and long-running server workloads, or worst-case latencies".
- **"Irreproducible malloc benchmarks" (jwb, 2021) [S60]** (snippet: the page sits behind a login).
  - TCMalloc and jemalloc beat mimalloc on larsonN, where the report claimed both were about 3x slower.
  - The comparison used the Ubuntu gperftools package.
  - mimalloc used many huge pages by default.
  - The harness instructions did not work as written.
- **SPEC CPU is a poor allocator benchmark.** Google's ASPLOS'24 paper finds SPEC CPU2006 lifetimes "much less diverse" than the fleet's, making SPEC "unsuitable for evaluating the performance of memory allocators" [S18].
- **Fleet ground truth from the same paper [S18]:**
  - malloc takes 4.3% of fleet cycles, 3.6–10.1% in the top 5 applications.
  - Cycle breakdown: per-CPU cache 53%, central free list 12%, prefetch 16%, sampling 4%.
  - Fragmentation is 22.2% of heap (18.8% external, 3.4% internal). The central free list holds 29% of it and the page heap 51%.
  - Objects under 1 KB are 98% of allocations but 28% of bytes. Objects over 8 KB hold 50% of bytes.
  - 46% of small objects live under 1 ms; 65% of objects over 1 GB live more than a day.
- **Measurement bias.**
  - Link order alone made -O3 look 8% better than -O2. None of the 133 surveyed papers handled such bias properly [S61].
  - STABILIZER re-randomises code, stack and heap layout to make comparisons statistically sound [S62].
  - Allocator changes shift heap layout, so they are especially exposed to layout bias.

### 2.2 Realistic workloads and trace replay

- **Server workloads.**
  - mimalloc-bench includes redis-benchmark with 1M requests [S58].
  - RocksDB is the recurring fragmentation case (Cloudflare, LinkedIn) [S2][S5], so `db_bench` with a large block cache and compaction belongs in the suite.
  - ClickHouse and Redis show the value of testing under a cgroup memory limit [S31][S48].
- **Google fleetbench.** Its TCMalloc "empirical driver" replays size and lifetime distributions "derived from data collected in production". A full run takes about 1 hour [S63].
- **glibc trace and replay (experimental dj/malloc branch) [S64].**
  - The trace has "No timestamps!", so time-based decay cannot be evaluated by replay.
  - The workload file keeps threads and synchronisation, and the replay is multi-threaded.
  - It reports Max RSS against "Max Ideal RSS", for example 242,515,968 bytes against 153,649 KB.
  - Open issues listed: thread ordering and ownership (mremap), and the cost of reading RSS during simulation.
  - The project warned "Really really really don't run this in production" [S64] (snippet).
- **Firefox logalloc / logalloc-replay [S65].**
  - Text log lines of the form `pid tid fn(args)=result`.
  - A preprocessing script replaces pointers with slot indexes, so the replayer avoids its own allocations.
  - Replay runs per process. The README does not describe concurrent multi-thread replay.

### 2.3 What to measure, and how

- **RSS over time.** Sample `/proc/PID/smaps_rollup` (`RssAnon`, `AnonHugePages`, `LazyFree`); it is much cheaper than full smaps [S28]. Also sample cgroup `memory.current` and `memory.stat` (`anon_thp`, `pglazyfree`). `memory.peak` can be reset per open file descriptor, giving per-phase peaks [S29].
- **Fragmentation.** Report RSS ÷ "ideal RSS" (live bytes) over time, as the glibc simulator does [S64]. Peak RSS alone hides phase-specific retention.
- **Counters.**
  - `perf stat -I <ms>` gives time series; `-r N` gives mean ± stddev.
  - `-d -d` adds dTLB/iTLB events; `--topdown` gives the front-end/back-end breakdown [S66].
  - Google judges hugepage work by the change in dTLB page-walk cycles and hugepage coverage, for example 54.4% → 56.2% coverage [S18].
- **Tail latency.** Use constant-rate load (wrk2 style). Closed-loop clients hide stalls through coordinated omission; in wrk2's pause test the corrected p99 was about 200x the uncorrected value [S67].
- **Production A/B as the final judge.** Google tests on 1% experiment and control slices of the fleet [S18]. jemalloc treated "thousands of distinct workflows" with no outliers as the safety bar [S54].

### 2.4 A protocol sohamalloc should adopt (derived from the above)

1. Run each workload in phases: ramp-up, steady state, burst, **idle at least 60 s**, then shutdown. Sample RSS and live bytes at 100 ms or faster.
2. Add scenarios no current suite covers:
   - Thread-pool churn: creating and destroying N threads per second [S14][S15][S4].
   - Fork under write load, for copy-on-write and THP [S32].
   - cgroup `memory.max` at about 1.2x the working set.
   - THP set to always, madvise and never, with `max_ptes_none` at 0 and 511 [S20][S35].
   - Offline or holey CPU sets, and glibc's rseq registration both on and off [S46][S38].
3. Baselines: glibc 2.44 (and 2.39 for older distributions), Google TCMalloc in per-CPU mode (confirm per-CPU caches are actually active [S37]), jemalloc 5.3.x with and without `background_thread`, mimalloc v3.4.x, snmalloc. Never use gperftools as a stand-in for "TCMalloc" [S40].
4. Use layout randomisation or many re-links and report confidence intervals [S61][S62]. Use trace replay only for packing quality, never for decay or idle behaviour [S64].

---

## 3. What "drop-in and user friendly" means in practice

### 3.1 glibc's replacement contract [S68]

- **Must provide:** `malloc`, `free`, `calloc`, `realloc`.
- **Should provide:** `aligned_alloc`, `free_sized`, `free_aligned_sized` (C23, added in glibc 2.43 [S10]), `malloc_usable_size`, `memalign`, `posix_memalign`, `pvalloc`, `valloc`; very old programs use `cfree`.
- Before glibc 2.25, `__libc_memalign` was also required.
- **Rules:**
  - Do not use glibc functions that call malloc internally from inside the allocator: `fopen`, `opendir`, `dlopen`, `pthread_setspecific`.
  - Any thread-local storage (TLS) must use the initial-exec model.
  - `free` must preserve `errno`; returned pointers must be aligned to `alignof(max_align_t)`.
  - "Failure to provide a complete set ... can lead to ... heap corruption."
- Replacing `reallocarray` and `strdup` "is not supported"; glibc's versions already call the replacement.
- `mallopt` and `mallinfo2` will "return incorrect statistics".
- glibc 2.44's documentation now says future versions may relax the alignment of small allocations [S10].

### 3.2 Hooks are gone, and RTLD_DEEPBIND is unsolved

- glibc 2.34 removed `__malloc_hook`, `__free_hook` and related hooks. `MALLOC_CHECK_`, `mtrace` and `mcheck` moved to `libc_malloc_debug.so`, and glibc recommends "writing and preloading their own malloc interposition library" [S10].
- **glibc bug 30186 (still UNCONFIRMED) [S69].**
  - A library loaded with `RTLD_DEEPBIND` ignores LD_PRELOAD for libc symbols, so mixed-allocator frees crash.
  - jemalloc's old hook-based fix "will fail silently now".
  - Siddhesh Poyarekar proposed a `glibc.malloc.impl=` tunable, and noted "we haven't had any volunteers".
- BIND linked with jemalloc crashed when Samba's DLZ (dynamically loadable zones) module loaded with DEEPBIND; Samba stopped using the flag by default [S70] (snippet).

### 3.3 Initialisation, TLS and fork

- **Calls before init.** Under glibc 2.44, `__newlocale` calls `free(NULL)` during libstdc++ static initialisation, before mimalloc's constructor runs. mimalloc crashed, and v3.4.4 fixed it with a static initial page map [S71][S40]. **Every entry point must work before the constructor runs.**
- **Dynamic linker recursion.** Since glibc 2.39 the dynamic linker calls malloc/free during TLS access. That causes "infinite recursion if a malloc replacement library ... use[s] dynamic TLS" [S10].
- **Fork.**
  - jemalloc deadlocked in a forked child when another thread was mid-initialisation [S72] (snippet).
  - A FreeBSD jemalloc bug: the pre-fork step did not take every internal mutex [S72] (snippet).
  - Chromium hit rare deadlocks because dbus/glib call `opendir()`, and therefore malloc, between fork and exec [S72] (snippet).

### 3.4 Linking pitfalls

- **`--as-needed`.** Ruby built `--with-jemalloc` crashed with `free(): invalid pointer` because `--as-needed` dropped `libjemalloc` from `DT_NEEDED`. The fix was `-Wl,--no-as-needed`, verified with `readelf` [S73].
- **musl's strong symbols** force patching `libc.a` [S23].
- **mimalloc static linking.** mimalloc recommends linking `mimalloc.o` as the first object. Its macro-override header risks "mixing of pointers from different heaps" [S40].
- **Late loading.** TCMalloc: "Don't try to load TCMalloc into a running binary (e.g., using JNI)" [S19].

### 3.5 C++

- **Sized-delete mismatches.** TCMalloc aborts on them. Small objects are checked only later, so the crash stack can be unrelated to the cause. AddressSanitizer checks every sized delete [S74].
- **Sized deallocation flags.** As of 2020, Clang did not enable sized deallocation by default [S20]. LLVM PR #83774 (March 2024) enabled it by default for C++14 and later [S75] (snippet). The release it first shipped in is **UNVERIFIED**, likely Clang 19.
- **Alignment.** `__STDCPP_DEFAULT_NEW_ALIGNMENT__ <= 8` reduces waste for 24- and 40-byte objects [S20].
- **Failing `operator new`.** TCMalloc does not throw when `operator new` fails, which differs from expected C++ semantics [S20].

### 3.6 Behaviour programs rely on

- **`malloc_usable_size`.** systemd wrote into the usable-size slack, and `_FORTIFY_SOURCE=3` aborted it. BIND9 removed `malloc_usable_size` entirely [S76] (snippet).
- **`realloc(p, 0)`.** C23 makes it undefined behaviour. glibc frees the block and returns NULL, while musl and the BSDs return a non-NULL pointer [S77] (snippet; gnu.org returned 503, **UNVERIFIED** directly). A Linux drop-in must match glibc.

### 3.7 Tuning knobs

- TCMalloc: "None of these tuning parameters are clear wins, otherwise they would be the default." A release rate "is not a panacea"; provision for peak use [S20].
- jemalloc: defaults are "conservative and suboptimal"; proper tuning gives "a few percent" [S78].
- Users mostly cargo-cult `MALLOC_ARENA_MAX=2`, which sometimes helps and sometimes hurts [S1].

---

## 4. Problems in the v1 design these findings expose

These points map evidence onto `/home/user/sohamalloc/docs/RESEARCH_AND_DESIGN.md` (v1). They are my judgements, not sourced facts.

1. **"MADV_FREE in the throughput profile or when there is no cgroup limit" (§5.5) repeats Go's mistake [S26].** Use DONTNEED by default everywhere. If MADV_FREE is ever used, report `LazyFree` explicitly in the "explain my memory" report [S28].
2. **"Reserve ~64 GiB of virtual address space at start-up" will break some environments.**
   - It breaks programs under `RLIMIT_AS`/`ulimit -v` [S4][S19] and fails under overcommit mode 2 unless the reservation is PROT_NONE [S44].
   - It also alarms operators who watch virtual size [S80].
   - Reserve PROT_NONE, grow on demand, and degrade under `RLIMIT_AS`.
3. **Indexing caches by `mm_cid` is fragile.**
   - Use glibc's registration through `__rseq_offset` and `__rseq_size`; `mm_cid` needs glibc ≥ 2.41 and kernel ≥ 6.3 [S10].
   - Never assume the allocator can register rseq itself [S38].
   - Size arrays from the possible-CPU count and bounds-check every id [S46].
   - Expose an "actually running in per-CPU mode?" status, since MongoDB had to tell users how to check theirs [S37].
4. **"MADV_HUGEPAGE on the heap region" is too blunt.**
   - Packing allocations densely into hugepages, Temeraire-style, is what the evidence supports [S21][S22].
   - Advising hugepages over sparse memory causes bloat [S35] and copy-on-write latency for programs that fork [S32].
   - khugepaged can undo partial purges [S33][S34].
   - Prefer `PR_THP_DISABLE_EXCEPT_ADVISED` plus advising only dense regions, and test mTHP [S36].
5. **"No always-on background thread" conflicts with jemalloc #1398 [S13].** Idle programs need time-driven release.
   - A PSI-triggered thread is limited to 2 s windows when unprivileged [S47].
   - A thread inside a preloaded library adds fork and seccomp exposure. **UNVERIFIED** as a general claim.
   - Use a lazily created timer thread, with a non-thread fallback that triggers on `free`.
6. **Guard pages via `MADV_GUARD_INSTALL` cannot be seen in `/proc` maps and broke CRIU [S45].** Document this and provide an opt-out.
7. **v1 lists `reallocarray` as an override, but glibc says replacing it "is not supported" [S68].** Implement it on top of sohamalloc's own `realloc`, and test static linking for symbol clashes.
8. **The benchmark plan (§7) leans on mimalloc-bench's peak-RSS numbers.** Add the phases and scenarios in §2.4, use the glibc 2.43/2.44 baseline [S8][S9], and add constant-rate latency [S67].
9. **Missing features with clear demand:**
   - A defrag hint plus a thread-cache-bypassing allocation (Redis) [S49].
   - A heap-walk/visitor API (CPython GC) [S51].
   - Exporting cgroup-aware statistics (ClickHouse, Envoy) [S31].
   - A per-thread cache flush hook for thread pools [S17].
10. **Bootstrap robustness must be a test-suite item:** `free(NULL)` before init [S71], initial-exec TLS only [S10][S68], fork in the middle of initialisation [S72], RTLD_DEEPBIND detection with a warning [S69], `DT_NEEDED` survival [S73].
11. **Maintainability.** jemalloc's history shows complexity and a single sponsoring company are risks [S54][S55]. Keep the code small, keep debugging-tool support (Valgrind), and build production-style telemetry in from day one.

---

## Sources

- [S1] https://www.speedshop.co/blog/malloc-doubles-ruby-memory/
- [S2] https://blog.cloudflare.com/the-effect-of-switching-to-tcmalloc-on-rocksdb-memory-use/
- [S4] https://github.com/nodejs/node/issues/51868
- [S5] https://www.linkedin.com/blog/engineering/infrastructure/taming-memory-fragmentation-in-venice-with-jemalloc
- [S6] https://bugs.openjdk.org/browse/JDK-8293114
- [S7] https://mail.openjdk.org/pipermail/hotspot-dev/2023-July/076405.html and https://mail.openjdk.org/pipermail/hotspot-dev/2023-July/076451.html
- [S8] https://sourceware.org/git/?p=glibc.git;a=commit;h=93e6135904eec91f6aaf5500175e7593d260dbe7
- [S9] https://sourceware.org/git/?p=glibc.git;a=commit;h=bb5a4f5295ced26532939703867c35f2ce8c149b
- [S10] https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=NEWS;hb=HEAD
- [S11] https://sourceware.org/git/?p=glibc.git;a=tags
- [S12] https://man7.org/linux/man-pages/man3/mallopt.3.html
- [S13] https://github.com/jemalloc/jemalloc/issues/1398
- [S14] https://github.com/microsoft/mimalloc/issues/351
- [S15] https://github.com/microsoft/mimalloc/issues/1025
- [S16] https://github.com/web-infra-dev/rspack/pull/9533
- [S17] https://jemalloc.net/mailman/jemalloc-discuss/2014-May/000769.html
- [S18] https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf
- [S19] https://github.com/google/tcmalloc/blob/master/docs/design.md
- [S20] https://github.com/google/tcmalloc/blob/master/docs/tuning.md
- [S21] https://github.com/google/tcmalloc/blob/master/docs/temeraire.md
- [S22] https://www.usenix.org/conference/osdi21/presentation/hunter
- [S23] https://tweag.io/blog/2023-08-10-rust-static-link-with-mimalloc/
- [S24] https://build.betterup.com/chasing-a-memory-leak-in-our-async-fastapi-service-how-jemalloc-fixed-our-rss-creep/
- [S25] https://go.dev/doc/go1.16
- [S26] https://github.com/golang/go/issues/42330
- [S27] https://man7.org/linux/man-pages/man2/madvise.2.html
- [S28] https://docs.kernel.org/filesystems/proc.html
- [S29] https://docs.kernel.org/admin-guide/cgroup-v2.html
- [S30] https://docs.gitlab.com/omnibus/settings/memory_constrained_envs/
- [S31] https://clickhouse.com/docs/reference/settings/server-settings/settings/memory-worker
- [S32] https://www.antirez.com/news/84
- [S33] https://jemalloc.net/mailman/jemalloc-discuss/2014-May/000781.html
- [S34] https://jemalloc.net/mailman/jemalloc-discuss/2012-February/000081.html (snippet)
- [S35] https://lwn.net/Articles/986242/
- [S36] https://docs.kernel.org/admin-guide/mm/transhuge.html
- [S37] https://www.mongodb.com/docs/manual/tutorial/transparent-huge-pages/
- [S38] https://jira.mongodb.org/browse/SERVER-90036
- [S39] https://docs.datastax.com/en/dse-trblshoot/doc/troubleshooting/highCPU.html (snippet, UNVERIFIED)
- [S40] https://github.com/microsoft/mimalloc/blob/main/readme.md
- [S41] https://lwn.net/Articles/937943
- [S43] https://docs.kernel.org/admin-guide/sysctl/vm.html
- [S44] https://docs.kernel.org/mm/overcommit-accounting.html
- [S45] https://lkml.rescloud.iu.edu/2503.2/05377.html
- [S46] https://github.com/google/tcmalloc/issues/188
- [S47] https://docs.kernel.org/accounting/psi.html
- [S48] https://raw.githubusercontent.com/redis/redis/unstable/redis.conf
- [S49] https://github.com/redis/redis/blob/unstable/deps/jemalloc/include/jemalloc/internal/jemalloc_internal_inlines_c.h and https://github.com/redis/redis/blob/unstable/src/defrag.c
- [S50] https://arxiv.org/abs/1902.04738
- [S51] https://peps.python.org/pep-0703/
- [S53] https://github.com/python/cpython/blob/main/Include/internal/pycore_obmalloc.h
- [S54] https://jasone.github.io/2025/06/12/jemalloc-postmortem/
- [S55] https://engineering.fb.com/2026/03/02/data-infrastructure/investing-in-infrastructure-metas-renewed-commitment-to-jemalloc/
- [S56] https://github.com/pola-rs/polars/issues/23222
- [S57] https://blog.rust-lang.org/2019/01/17/Rust-1.32.0/
- [S58] https://github.com/daanx/mimalloc-bench
- [S59] https://github.com/daanx/mimalloc-bench/blob/master/bench.sh
- [S60] https://paper.dropbox.com/doc/Irreproducible-malloc-benchmarks--BW~~drKA3xsbBtf9XP2xtvSRAQ-xgvyGU2f6EJ14r5oqHrn5 (snippet) and https://news.ycombinator.com/item?id=29356991
- [S61] https://sape.inf.usi.ch/publications/asplos09
- [S62] https://people.cs.umass.edu/~emery/pubs/stabilizer-asplos13.pdf
- [S63] https://github.com/google/fleetbench
- [S64] https://blog.linuxplumbersconf.org/2016/ocw/system/presentations/3921/original/LPC%202016%20-%20linux%20and%20glibc_%20The%204.5TiB%20malloc%20API%20trace.pdf and https://sourceware.org/glibc/wiki/MallocTracing (snippet)
- [S65] https://github.com/mozilla-firefox/firefox/blob/main/memory/replace/logalloc/README
- [S66] https://man7.org/linux/man-pages/man1/perf-stat.1.html
- [S67] https://github.com/giltene/wrk2
- [S68] https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html
- [S69] https://sourceware.org/bugzilla/show_bug.cgi?id=30186
- [S70] https://gitlab.com/samba-team/samba/-/merge_requests/3810 and https://bug493541.bugzilla.mozilla.org (snippet)
- [S71] https://github.com/microsoft/mimalloc/issues/1341
- [S72] https://jemalloc.net/mailman/jemalloc-discuss/2012-May/000424.html, https://bugs.freebsd.org/271490 and https://codereview.chromium.org/1715903002 (snippet)
- [S73] https://bugs.ruby-lang.org/issues/18409
- [S74] https://github.com/google/tcmalloc/blob/master/docs/mismatched-sized-delete.md
- [S75] https://github.com/llvm/llvm-project/pull/83774 (snippet)
- [S76] https://github.com/systemd/systemd/issues/19203, https://inbox.sourceware.org/libc-alpha/87o7smrnlh.fsf@igel.home/t/ and https://gitlab.isc.org/isc-projects/bind9/-/merge_requests/9419 (snippet)
- [S77] https://www.gnu.org/software/gnulib/manual/html_node/realloc.html (snippet)
- [S78] https://github.com/jemalloc/jemalloc/blob/dev/TUNING.md
- [S79] https://people.cs.umass.edu/~emery/pubs/berger-asplos2000.pdf
- [S80] https://issues.apache.org/jira/browse/HADOOP-7154 (snippet)