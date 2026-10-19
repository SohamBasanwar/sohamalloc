/*
 * stats_api -- sohamalloc's introspection API (docs/IMPL_SPEC_v0.md §2,
 * plan §5.13 "explain my memory"): soham_get_stats, soham_stats_print,
 * soham_check_heap, soham_collect, and the glibc compatibility functions
 * built on them (malloc_stats, mallinfo2, malloc_trim) in override builds.
 *
 * sohamalloc-only (other allocators and the libc shim SKIP).  The soham_*
 * functions are found with dlsym() under LD_PRELOAD and called directly in
 * the prefixed build (tests/prefixed/stats_api.c).
 *
 * Checks, single-threaded unless noted:
 *   - soham_get_stats returns 0, and every snapshot is coherent:
 *     released <= committed, resident_est <= committed, in_use <= resident,
 *     and the report's breakdown (in_use + cached_heaps + cached_pools +
 *     pageheap_free + metadata + solo) adds up to resident_est;
 *   - the counters move as they should: in_use grows by at least the bytes
 *     allocated (and by at most 30% more), solo grows by >= 64 MiB for a
 *     64 MiB block, everything returns to the baseline after free;
 *     soham_collect(1) releases the freed pages (resident drops, released
 *     grows, nothing left in pageheap_free);
 *   - heaps_live counts live threads' heaps; exited threads' heaps are
 *     pooled and reused by new threads (live + pooled does not grow);
 *   - soham_stats_print writes a sane, complete report (version banner,
 *     the documented lines, newline-terminated, bounded size) to a pipe;
 *     malloc_stats() writes the same report to fd 2 (override builds);
 *     mallinfo2() reflects live bytes (override builds);
 *   - SOHAMALLOC_STATS=1 prints the report at exit (re-exec, native only);
 *   - soham_check_heap() == 0 during (from a concurrent thread) and after a
 *     multi-threaded stress with cross-thread frees and thread exits.
 */
#include "common/test_util.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

#if defined(TEST_PREFIXED) && defined(TEST_SHIM)
int main(int argc, char **argv)
{
    tu_init("stats_api", argc, argv);
    tu_skip("libc shim: no sohamalloc statistics");
}
#else

static int (*f_get_stats)(struct soham_stats *);
static void (*f_stats_print)(int);
static int (*f_check_heap)(void);
static void (*f_collect)(int);
static const char *(*f_version)(void);

static void bind_entry_points(void)
{
#ifdef TEST_PREFIXED
    f_get_stats = soham_get_stats;
    f_stats_print = soham_stats_print;
    f_check_heap = soham_check_heap;
    f_collect = soham_collect;
    f_version = soham_version;
#else
#define BIND(var, name)                                               \
    do {                                                              \
        *(void **)(&var) = tu_sym(name);                              \
        if (!var) TU_FAIL("sohamalloc does not export %s", name);     \
    } while (0)
    BIND(f_get_stats, "soham_get_stats");
    BIND(f_stats_print, "soham_stats_print");
    BIND(f_check_heap, "soham_check_heap");
    BIND(f_collect, "soham_collect");
    BIND(f_version, "soham_version");
#undef BIND
#endif
}

#define MiB ((size_t)1 << 20)

static void snap(struct soham_stats *s, const char *when)
{
    memset(s, 0xA5, sizeof *s);
    int rc = f_get_stats(s);
    if (rc != 0) TU_FAIL("soham_get_stats() = %d at %s", rc, when);
}

static void dump(const struct soham_stats *s, const char *when)
{
    tu_info("%-22s committed %zu resident %zu in_use %zu cached_heaps %zu cached_pools %zu pageheap_free %zu "
            "released %zu metadata %zu solo %zu windows %zu heaps %zu+%zu",
            when, s->committed, s->resident_est, s->in_use, s->cached_heaps, s->cached_pools, s->pageheap_free,
            s->released, s->metadata, s->solo, s->windows, s->heaps_live, s->heaps_pooled);
}

/* Coherence of one quiescent snapshot. */
static void coherent(const struct soham_stats *s, const char *when)
{
    if (s->released > s->committed) { dump(s, when); TU_FAIL("%s: released %zu > committed %zu", when, s->released, s->committed); }
    if (s->resident_est > s->committed) { dump(s, when); TU_FAIL("%s: resident_est %zu > committed %zu", when, s->resident_est, s->committed); }
    if (s->in_use > s->resident_est) { dump(s, when); TU_FAIL("%s: in_use %zu > resident_est %zu", when, s->in_use, s->resident_est); }
    if (s->solo > s->committed) { dump(s, when); TU_FAIL("%s: solo %zu > committed %zu", when, s->solo, s->committed); }
    size_t sum = s->in_use + s->cached_heaps + s->cached_pools + s->pageheap_free + s->metadata + s->solo;
    size_t diff = sum > s->resident_est ? sum - s->resident_est : s->resident_est - sum;
    size_t tol = s->resident_est / 50 > MiB ? s->resident_est / 50 : MiB;
    if (diff > tol) {
        dump(s, when);
        TU_FAIL("%s: breakdown sums to %zu but resident_est is %zu (diff %zu > %zu)", when, sum, s->resident_est, diff, tol);
    }
    if (s->windows < 1 || s->windows > 255) { dump(s, when); TU_FAIL("%s: windows = %zu", when, s->windows); }
    if (s->heaps_live < 1) { dump(s, when); TU_FAIL("%s: heaps_live = %zu with a live allocating thread", when, s->heaps_live); }
    if (s->metadata == 0) { dump(s, when); TU_FAIL("%s: metadata = 0", when); }
}

/* ------------------------------------------------------------ counters */
static void test_counters(void)
{
    struct soham_stats s0, s1, s2, s3, s4, s5;
    void *warm = T_MALLOC(100);                /* attach this thread's heap */
    T_FREE(warm);
    f_collect(1);
    snap(&s0, "baseline");
    coherent(&s0, "baseline");
    dump(&s0, "baseline");

    /* Small blocks: 20000 x 1000 B (class 1024). */
    enum { NSMALL = 20000 };
    void **small = (void **)tu_os_alloc(NSMALL * sizeof(void *));
    size_t req = 0;
    for (int i = 0; i < NSMALL; i++) {
        size_t n = 1000;
        small[i] = T_MALLOC(n);
        TU_CHECK(small[i], "malloc");
        memset(small[i], 1, n);
        req += n;
    }
    snap(&s1, "small live");
    coherent(&s1, "small live");
    size_t d = s1.in_use - s0.in_use;
    if (s1.in_use < s0.in_use || d < req || d > req + req * 3 / 10 + MiB) {
        dump(&s1, "small live");
        TU_FAIL("in_use grew by %zd for %zu requested bytes (want [n, 1.3 n + 1 MiB])", (ssize_t)(s1.in_use - s0.in_use), req);
    }
    TU_CHECK(s1.resident_est >= s0.resident_est + req, "resident_est grew by %zd < %zu",
             (ssize_t)(s1.resident_est - s0.resident_est), req);

    /* Large: 8 x 1 MiB runs and a 64 MiB solo (64-bit; 40 MiB on 32-bit). */
    void *runs[8];
    for (int i = 0; i < 8; i++) {
        runs[i] = T_MALLOC(MiB);
        TU_CHECK(runs[i], "malloc(1 MiB)");
        memset(runs[i], 2, MiB);
    }
    size_t solon = sizeof(void *) == 8 ? 64 * MiB : 40 * MiB;
    void *solo = T_MALLOC(solon);
    TU_CHECK(solo, "malloc(%zu)", solon);
    memset(solo, 3, solon);
    snap(&s2, "large live");
    coherent(&s2, "large live");
    if (s2.in_use < s1.in_use + 8 * MiB) { dump(&s2, "large live"); TU_FAIL("in_use grew by %zd for 8 MiB of runs", (ssize_t)(s2.in_use - s1.in_use)); }
    if (s2.solo < s1.solo + solon) { dump(&s2, "large live"); TU_FAIL("solo grew by %zd for a %zu-byte block", (ssize_t)(s2.solo - s1.solo), solon); }
    if (s2.committed < s1.committed + solon) { dump(&s2, "large live"); TU_FAIL("committed grew by %zd for a %zu-byte solo", (ssize_t)(s2.committed - s1.committed), solon); }
    dump(&s2, "large live");

    /* Free everything. */
    for (int i = 0; i < NSMALL; i++) T_FREE(small[i]);
    for (int i = 0; i < 8; i++) T_FREE(runs[i]);
    T_FREE(solo);
    snap(&s3, "freed");
    coherent(&s3, "freed");
    if (s3.in_use > s0.in_use + MiB) { dump(&s3, "freed"); TU_FAIL("in_use %zu after freeing everything (baseline %zu)", s3.in_use, s0.in_use); }
    if (s3.solo > s0.solo) { dump(&s3, "freed"); TU_FAIL("solo %zu after free (baseline %zu)", s3.solo, s0.solo); }

    /* soham_collect(1) gives the freed pages back. */
    f_collect(1);
    snap(&s4, "collected");
    coherent(&s4, "collected");
    dump(&s4, "collected");
    if (s4.pageheap_free > MiB) TU_FAIL("pageheap_free %zu after soham_collect(1)", s4.pageheap_free);
    if (s4.released < s3.released) TU_FAIL("released went down on collect: %zu -> %zu", s3.released, s4.released);
    size_t freed = req + 8 * MiB;
    if (s4.resident_est > s0.resident_est + freed / 5 + 2 * MiB)
        TU_FAIL("resident_est %zu after collect, baseline %zu: less than 80%% of %zu freed bytes returned",
                s4.resident_est, s0.resident_est, freed);

    /* A snapshot is cheap and changes nothing. */
    snap(&s5, "again");
    TU_CHECK(s5.in_use == s4.in_use && s5.committed == s4.committed, "two quiescent snapshots differ");
    TU_CHECK(f_get_stats(NULL) != 0, "soham_get_stats(NULL) must fail");
    TU_CHECK(f_check_heap() == 0, "soham_check_heap() after the counter test");
    tu_os_free(small, NSMALL * sizeof(void *));
}

/* ------------------------------------------------------------ heaps */
static pthread_barrier_t hb_ready, hb_go;

static void *heap_thread(void *arg)
{
    (void)arg;
    void *p = T_MALLOC(64);
    TU_CHECK(p, "thread malloc");
    pthread_barrier_wait(&hb_ready);
    pthread_barrier_wait(&hb_go);
    T_FREE(p);
    return NULL;
}

static void test_heaps(void)
{
    enum { NT = 4 };
    struct soham_stats a, b, c, d, e;
    snap(&a, "heaps: before");
    pthread_t t[NT];
    pthread_barrier_init(&hb_ready, NULL, NT + 1);
    pthread_barrier_init(&hb_go, NULL, NT + 1);
    for (int i = 0; i < NT; i++) t[i] = tu_thread_start(heap_thread, NULL);
    pthread_barrier_wait(&hb_ready);
    snap(&b, "heaps: threads live");
    pthread_barrier_wait(&hb_go);
    for (int i = 0; i < NT; i++) tu_thread_join(t[i]);
    snap(&c, "heaps: joined");
    if (b.heaps_live < a.heaps_live + NT) TU_FAIL("heaps_live %zu with %d extra threads live (before %zu)", b.heaps_live, NT, a.heaps_live);
    if (c.heaps_live != a.heaps_live) TU_FAIL("heaps_live %zu after joining (before %zu)", c.heaps_live, a.heaps_live);
    if (c.heaps_pooled < NT) TU_FAIL("heaps_pooled %zu after %d threads exited", c.heaps_pooled, NT);

    /* New threads reuse pooled heaps. */
    for (int i = 0; i < NT; i++) t[i] = tu_thread_start(heap_thread, NULL);
    pthread_barrier_wait(&hb_ready);
    snap(&d, "heaps: reuse live");
    pthread_barrier_wait(&hb_go);
    for (int i = 0; i < NT; i++) tu_thread_join(t[i]);
    snap(&e, "heaps: reuse joined");
    if (d.heaps_live + d.heaps_pooled > c.heaps_live + c.heaps_pooled)
        TU_FAIL("heaps were created although %zu were pooled: live+pooled %zu -> %zu", c.heaps_pooled,
                c.heaps_live + c.heaps_pooled, d.heaps_live + d.heaps_pooled);
    if (e.heaps_live + e.heaps_pooled != c.heaps_live + c.heaps_pooled)
        TU_FAIL("live+pooled heaps %zu -> %zu over a reuse cycle", c.heaps_live + c.heaps_pooled, e.heaps_live + e.heaps_pooled);
    pthread_barrier_destroy(&hb_ready);
    pthread_barrier_destroy(&hb_go);
    tu_info("heaps: live %zu -> %zu -> %zu, pooled %zu after exit, %zu while reused", a.heaps_live, b.heaps_live,
            c.heaps_live, c.heaps_pooled, d.heaps_pooled);
}

/* ------------------------------------------------------------ report */
#ifndef _WIN32
/* Runs fn(fd) with fd = the write end of a pipe; returns what was written. */
static size_t capture(void (*fn)(int), int via_stderr, char *out, size_t cap)
{
    int pfd[2];
    TU_CHECK(pipe(pfd) == 0, "pipe");
    int saved = -1;
    if (via_stderr) {
        saved = dup(2);
        dup2(pfd[1], 2);
        fn(2);
        dup2(saved, 2);
        close(saved);
    } else {
        fn(pfd[1]);
    }
    close(pfd[1]);
    size_t n = 0;
    for (;;) {
        long k = read(pfd[0], out + n, cap - 1 - n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        n += (size_t)k;
        if (n >= cap - 1) break;
    }
    close(pfd[0]);
    out[n] = 0;
    return n;
}

static void print_report(int fd) { f_stats_print(fd); }
#ifndef TEST_PREFIXED
static void call_malloc_stats(int fd)
{
    (void)fd;
    void (*ms)(void) = (void (*)(void))tu_sym("malloc_stats");
    TU_CHECK(ms, "malloc_stats not exported");
    ms();
}
#endif

static void check_report(const char *r, size_t n, const char *what)
{
    if (n == 0) TU_FAIL("%s wrote nothing", what);
    if (n > 7000) TU_FAIL("%s wrote %zu bytes (runaway?)", what, n);
    if (strlen(r) != n) TU_FAIL("%s output contains a NUL byte", what);
    if (r[n - 1] != '\n') TU_FAIL("%s output does not end with a newline", what);
    char banner[64];
    snprintf(banner, sizeof banner, "sohamalloc %s", f_version());
    if (strncmp(r, banner, strlen(banner)) != 0) TU_FAIL("%s output does not start with '%s': %.60s", what, banner, r);
    static const char *const want[] = { "resident", "in use", "cached", "page-heap free", "metadata",
                                        "released", "committed", "windows", "heaps live" };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
        if (!strstr(r, want[i])) TU_FAIL("%s output lacks '%s':\n%s", what, want[i], r);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)r[i];
        if (ch != '\n' && (ch < 0x20 || ch > 0x7e)) TU_FAIL("%s output has a control/non-ASCII byte 0x%02x at %zu", what, ch, i);
    }
}

static void test_report(void)
{
    static char buf[8192];
    void *keep = T_MALLOC(3 * MiB);
    size_t n = capture(print_report, 0, buf, sizeof buf);
    check_report(buf, n, "soham_stats_print");
    tu_write_str(1, buf);
    f_stats_print(-1);                         /* a bad fd is harmless */
#ifndef TEST_PREFIXED
    n = capture(call_malloc_stats, 1, buf, sizeof buf);
    check_report(buf, n, "malloc_stats");

    /* mallinfo2 (glibc 2.33 layout: ten size_t fields). */
    struct mi2 { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
    struct mi2 (*mi)(void) = (struct mi2 (*)(void))tu_sym("mallinfo2");
    TU_CHECK(mi, "mallinfo2 not exported");
    struct mi2 m = mi();
    TU_CHECK(m.uordblks >= 3 * MiB, "mallinfo2().uordblks = %zu with 3 MiB live", m.uordblks);
    int (*trim)(size_t) = (int (*)(size_t))tu_sym("malloc_trim");
    TU_CHECK(trim && trim(0) == 1, "malloc_trim(0) must return 1");
#endif
    T_FREE(keep);
}

/* SOHAMALLOC_STATS=1: the report is printed to fd 2 at exit. */
static void test_stats_at_exit(const char *self)
{
    if (!getenv("TEST_NATIVE") || strcmp(getenv("TEST_NATIVE"), "1") != 0) {
        tu_info("NOTE SOHAMALLOC_STATS=1 at-exit report not checked (not a native run)");
        return;
    }
    int pfd[2];
    TU_CHECK(pipe(pfd) == 0, "pipe");
    pid_t pid = fork();
    TU_CHECK(pid >= 0, "fork");
    if (pid == 0) {
        dup2(pfd[1], 2);
        close(pfd[0]);
        setenv("SOHAMALLOC_STATS", "1", 1);
        execl(self, self, "--child-exit-report", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    static char buf[8192];
    size_t n = 0;
    for (;;) {
        long k = read(pfd[0], buf + n, sizeof buf - 1 - n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        n += (size_t)k;
        if (n >= sizeof buf - 1) break;
    }
    close(pfd[0]);
    buf[n] = 0;
    int st;
    waitpid(pid, &st, 0);
    TU_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child with SOHAMALLOC_STATS=1 failed (status %#x): %s", st, buf);
    const char *r = strstr(buf, "sohamalloc ");
    if (!r) TU_FAIL("SOHAMALLOC_STATS=1 printed no report at exit (stderr: '%s')", buf);
    check_report(r, strlen(r), "SOHAMALLOC_STATS=1 at exit");
}
#endif

/* ------------------------------------------------------------ check_heap */
enum { SNT = 6, SSLOTS = 4096 };
static _Atomic(void *) xslots[1024];
static _Atomic int stress_stop;
static _Atomic unsigned long check_calls, check_errors;

static void *stress_thread(void *arg)
{
    int id = (int)(intptr_t)arg;
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ (0x5157ULL << 20) ^ (uint64_t)id);
    void **sl = (void **)tu_os_alloc(SSLOTS * sizeof(void *));
    size_t ops = tu_scaled_min(400000, 20000);
    for (size_t op = 0; op < ops; op++) {
        size_t i = (size_t)tu_rng_below(&r, SSLOTS);
        if (sl[i]) {
            if (tu_rng_below(&r, 4) == 0) {
                void *old = atomic_exchange(&xslots[tu_rng_below(&r, 1024)], sl[i]);
                if (old) T_FREE(old);
            } else if (tu_rng_below(&r, 5) == 0) {
                void *q = T_REALLOC(sl[i], tu_rand_size(&r, 1u << 20) + 1);
                TU_CHECK(q, "realloc");
                sl[i] = q;
                continue;
            } else {
                T_FREE(sl[i]);
            }
            sl[i] = NULL;
        } else {
            size_t n = tu_rand_size(&r, 1u << 20);
            sl[i] = (op & 7) ? T_MALLOC(n) : T_CALLOC(1, n);
            TU_CHECK(sl[i], "alloc(%zu)", n);
            if (n) ((unsigned char *)sl[i])[n - 1] = (unsigned char)op;
        }
        if ((op & 0x3fff) == 0) {
            int e = f_check_heap();
            if (e) atomic_fetch_add(&check_errors, 1);   /* reported at the end */
            tu_tick();
        }
    }
    int e = f_check_heap();
    if (e) atomic_fetch_add(&check_errors, 1);
    /* Leave half the blocks for main to free after this thread is gone. */
    for (size_t i = 0; i < SSLOTS; i++) {
        if (!sl[i]) continue;
        if (i & 1) T_FREE(sl[i]);
        else {
            void *old = atomic_exchange(&xslots[i % 1024], sl[i]);
            if (old) T_FREE(old);
        }
    }
    tu_os_free(sl, SSLOTS * sizeof(void *));
    return NULL;
}

static void *checker_thread(void *arg)
{
    (void)arg;
    while (!atomic_load(&stress_stop)) {
        int e = f_check_heap();
        atomic_fetch_add(&check_calls, 1);
        if (e) {
            if (atomic_fetch_add(&check_errors, 1) == 0)
                tu_info("first concurrent soham_check_heap() error: %d", e);
        }
        tu_sleep_ms(2);
    }
    return NULL;
}

static void test_check_heap(void)
{
    TU_CHECK(f_check_heap() == 0, "soham_check_heap() before the stress");
    pthread_t chk = tu_thread_start(checker_thread, NULL);
    for (int round = 0; round < 2; round++) {
        pthread_t t[SNT];
        for (int i = 0; i < SNT; i++) t[i] = tu_thread_start(stress_thread, (void *)(intptr_t)(round * SNT + i));
        for (int i = 0; i < SNT; i++) tu_thread_join(t[i]);
        TU_CHECK(f_check_heap() == 0, "soham_check_heap() after stress round %d", round);
    }
    atomic_store(&stress_stop, 1);
    tu_thread_join(chk);
    for (int i = 0; i < 1024; i++) {
        void *p = atomic_exchange(&xslots[i], NULL);
        if (p) T_FREE(p);
    }
    TU_CHECK(f_check_heap() == 0, "soham_check_heap() after freeing everything");
    f_collect(1);
    TU_CHECK(f_check_heap() == 0, "soham_check_heap() after soham_collect(1)");
    struct soham_stats s;
    snap(&s, "after stress");
    coherent(&s, "after stress");
    dump(&s, "after stress");
    unsigned long ce = atomic_load(&check_errors);
    tu_info("check_heap: %lu concurrent checker calls; %lu calls (checker + stress threads) reported errors",
            atomic_load(&check_calls), ce);
    /* Last, so the quiescent checks above still run when this fails. */
    if (ce) TU_FAIL("soham_check_heap() returned non-zero %lu times while other threads allocated "
                    "(the quiescent checks all passed; see tests/regress/check_heap_concurrent.c)", ce);
}

int main(int argc, char **argv)
{
    tu_init("stats_api", argc, argv);
    if (!tu_is_soham()) tu_skip("allocator is not sohamalloc");
    bind_entry_points();
    if (argc > 1 && strcmp(argv[1], "--child-exit-report") == 0) {
        void *p = T_MALLOC(1000);
        T_FREE(p);
        return 0;
    }
    tu_watchdog_secs = 60;
    pthread_t wd = tu_thread_start(tu_watchdog_main, NULL);
    test_counters();
    test_heaps();
#ifndef _WIN32
    test_report();
    test_stats_at_exit("/proc/self/exe");
#endif
    test_check_heap();
    atomic_store(&tu_watchdog_stop, 1);
    tu_thread_join(wd);
    tu_info("PASS");
    return 0;
}
#endif
