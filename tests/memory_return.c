/*
 * memory_return -- freed memory goes back to the OS (plan §5.7-5.9 decay and
 * release, spec §6; plan §8.4 "RSS returns").
 *
 * sohamalloc-only (other allocators SKIP: their release policies differ and
 * are not under test).  RSS is read from /proc/self/statm.
 *
 * Phases, each: measure RSS, allocate and touch ~1 GiB (256 MiB on 32-bit),
 * free it all, call soham_collect(1) and malloc_trim(0), measure again; the
 * RSS increase must shrink by at least 80%.
 *   1 small blocks (16 B .. 1 KiB, SMALL pages);
 *   2 medium blocks (8 KiB .. 256 KiB, MEDIUM pages and RUN classes);
 *   3 large runs (300 KiB .. 8 MiB) and solos (32 .. 128 MiB on 64-bit);
 *   4 a mix of all of the above allocated by 4 threads that exit, freed
 *     afterwards by main (remote frees into orphaned pages);
 *   5 a mix allocated by main and freed by 4 threads (remote frees into a
 *     live heap's pages), then collected by main.
 * Also: fragmentation -- keeping the first 1/8 of 512 MiB of small blocks
 * live must still return >= 80% of the rest (only whole free slices are
 * released, spec §6, so blocks scattered over every page are reported but
 * not judged) -- and reuse: after a release cycle, released memory is
 * reused and calloc of it is zero.
 */
#include "common/test_util.h"

#if defined(TEST_PREFIXED) || defined(_WIN32)
int main(int argc, char **argv)
{
    tu_init("memory_return", argc, argv);
    tu_skip("memory_return runs only against the override library (LD_PRELOAD or --link)");
}
#else

#define MiB ((size_t)1 << 20)

static void (*f_collect)(int);
static int (*f_trim)(size_t);

static size_t target;            /* bytes per phase */

typedef struct { void *p; size_t n; } blk;

static void release(void)
{
    f_collect(1);
    if (f_trim) f_trim(0);
}

static void touch(void *p, size_t n)
{
    unsigned char *c = (unsigned char *)tu_launder(p);
    size_t pg = tu_page_size();
    for (size_t o = 0; o < n; o += pg) c[o] = (unsigned char)(o >> 12 | 1);
    if (n) c[n - 1] = 0x5a;
}

/* Allocate blocks with sizes from gen() until `total` bytes are live. */
typedef size_t (*size_gen)(tu_rng *);
static size_t gen_small(tu_rng *r) { return (size_t)tu_rng_logu(r, 16, 1024); }
static size_t gen_medium(tu_rng *r) { return (size_t)tu_rng_logu(r, 8192, 256 * 1024); }
static size_t gen_large(tu_rng *r)
{
    if (sizeof(void *) == 8 && tu_rng_below(r, 8) == 0) return (size_t)tu_rng_range(r, 32 * MiB, 128 * MiB);
    return (size_t)tu_rng_logu(r, 300 * 1024, 8 * MiB);
}
static size_t gen_mix(tu_rng *r)
{
    switch (tu_rng_below(r, 3)) {
    case 0: return gen_small(r);
    case 1: return gen_medium(r);
    default: return (size_t)tu_rng_logu(r, 300 * 1024, 4 * MiB);
    }
}

/* Bookkeeping comes from the OS and is pre-faulted before the "before"
   measurement, so it does not distort the RSS deltas. */
static size_t book_cap(size_t total) { return total / 64 + 1024; }
static blk *book_new(size_t total)
{
    size_t bytes = book_cap(total) * sizeof(blk);
    blk *b = (blk *)tu_os_alloc(bytes);
    memset(b, 0, bytes);
    return b;
}
static void book_free(blk *b, size_t total) { tu_os_free(b, book_cap(total) * sizeof(blk)); }

static void fill(blk *b, size_t total, size_gen gen, uint64_t seed, size_t *count)
{
    size_t cap = book_cap(total);
    tu_rng r;
    tu_rng_seed(&r, seed);
    size_t live = 0, k = 0;
    while (live < total) {
        if (k == cap) TU_FAIL("bookkeeping full (%zu blocks)", k);
        size_t n = gen(&r);
        if (n > total - live && total - live >= 16) n = total - live;
        void *p = T_MALLOC(n);
        if (!p) TU_FAIL("malloc(%zu) failed with %zu bytes live", n, live);
        touch(p, n);
        b[k].p = p;
        b[k].n = n;
        k++;
        live += n;
        if ((k & 1023) == 0) tu_tick();
    }
    *count = k;
}

static void drain(blk *b, size_t k, size_t keep_every)
{
    for (size_t i = 0; i < k; i++) {
        if (!b[i].p) continue;
        if (keep_every && i % keep_every == 0) continue;
        T_FREE(b[i].p);
        b[i].p = NULL;
    }
}

static void check_drop(const char *phase, size_t before, size_t peak, size_t after, double need)
{
    size_t grew = peak > before ? peak - before : 0;
    size_t kept = after > before ? after - before : 0;
    double ret = grew ? 1.0 - (double)kept / (double)grew : 1.0;
    tu_info("%-28s rss %6zu MiB -> peak %6zu MiB -> %6zu MiB after release: %.1f%% of the increase returned",
            phase, before / MiB, peak / MiB, after / MiB, 100.0 * ret);
    if (grew < target / 2)
        TU_FAIL("%s: RSS only grew by %zu MiB for %zu MiB touched (RSS accounting broken?)", phase, grew / MiB, target / MiB);
    if (ret < need)
        TU_FAIL("%s: only %.1f%% of the %zu MiB RSS increase returned after soham_collect(1)/malloc_trim(0), want >= %.0f%%",
                phase, 100.0 * ret, grew / MiB, 100.0 * need);
}

static void single_phase(const char *name, size_gen gen, uint64_t seed)
{
    blk *b = book_new(target);
    release();
    size_t before = tu_rss(), k;
    fill(b, target, gen, seed, &k);
    size_t peak = tu_rss();
    drain(b, k, 0);
    release();
    size_t after = tu_rss();
    check_drop(name, before, peak, after, 0.80);
    book_free(b, target);
}

/* ---------------------------------------------------------------- threads */
enum { NT = 4 };
typedef struct { blk *b; size_t k; size_t total; uint64_t seed; } targ;

static void *alloc_thread(void *arg)
{
    targ *a = (targ *)arg;
    fill(a->b, a->total, gen_mix, a->seed, &a->k);
    return NULL;                   /* exits with its blocks live */
}

static void *free_thread(void *arg)
{
    targ *a = (targ *)arg;
    drain(a->b, a->k, 0);
    return NULL;
}

static void phase_threads_alloc(void)
{
    targ a[NT];
    pthread_t t[NT];
    for (int i = 0; i < NT; i++) {
        a[i].total = target / NT;
        a[i].seed = tu_seed_v + 400 + (uint64_t)i;
        a[i].b = book_new(a[i].total);
    }
    release();
    size_t before = tu_rss();
    for (int i = 0; i < NT; i++) t[i] = tu_thread_start(alloc_thread, &a[i]);
    for (int i = 0; i < NT; i++) tu_thread_join(t[i]);
    size_t peak = tu_rss();
    for (int i = 0; i < NT; i++) drain(a[i].b, a[i].k, 0);
    release();
    size_t after = tu_rss();
    check_drop("4 exited threads, main frees", before, peak, after, 0.80);
    for (int i = 0; i < NT; i++) book_free(a[i].b, a[i].total);
}

static void phase_threads_free(void)
{
    targ a[NT];
    pthread_t t[NT];
    for (int i = 0; i < NT; i++) {
        a[i].total = target / NT;
        a[i].b = book_new(a[i].total);
    }
    release();
    size_t before = tu_rss();
    for (int i = 0; i < NT; i++) fill(a[i].b, a[i].total, gen_mix, tu_seed_v + 500 + (uint64_t)i, &a[i].k);
    size_t peak = tu_rss();
    for (int i = 0; i < NT; i++) t[i] = tu_thread_start(free_thread, &a[i]);
    for (int i = 0; i < NT; i++) tu_thread_join(t[i]);
    /* Main's own pages received only remote frees: a collect from main must
       still find them. */
    release();
    size_t after = tu_rss();
    check_drop("main allocs, 4 threads free", before, peak, after, 0.80);
    for (int i = 0; i < NT; i++) book_free(a[i].b, a[i].total);
}

/* Only whole free slices are released (spec §6), so live blocks scattered
   over every page pin them all; that case is only reported.  Blocks that
   leave whole pages empty must not prevent their release: keeping the first
   1/8 of the blocks (allocation order = address order within pages) must
   return >= 80% of the other 7/8. */
static void phase_fragmented(void)
{
    blk *b = book_new(target / 2);
    release();
    size_t before = tu_rss(), k;
    fill(b, target / 2, gen_small, tu_seed_v + 600, &k);
    size_t peak = tu_rss();
    drain(b, k, 64);               /* keep 1 in 64, spread over all pages */
    release();
    size_t spread = tu_rss();
    tu_info("%-28s rss %6zu MiB -> peak %6zu MiB -> %6zu MiB (informational: partly used pages stay resident)",
            "small, 1/64 kept (spread)", before / MiB, peak / MiB, spread / MiB);
    drain(b, k, 0);
    release();
    book_free(b, target / 2);

    b = book_new(target / 2);
    release();
    before = tu_rss();
    fill(b, target / 2, gen_small, tu_seed_v + 601, &k);
    peak = tu_rss();
    for (size_t i = k / 8; i < k; i++) { T_FREE(b[i].p); b[i].p = NULL; }
    release();
    size_t mid = tu_rss();
    check_drop("small, first 1/8 kept", before, peak, mid, 0.80 * 7 / 8);
    drain(b, k, 0);
    release();
    size_t after = tu_rss();
    check_drop("small, rest freed", before, peak, after, 0.80);
    book_free(b, target / 2);
}

/* Released memory is reused, and calloc of it is zero. */
static void phase_reuse(void)
{
    size_t n = 4 * MiB, cnt = target / n / 4;
    void **v = (void **)tu_os_alloc(cnt * sizeof(void *));
    for (size_t i = 0; i < cnt; i++) {
        v[i] = T_MALLOC(n);
        TU_CHECK(v[i], "malloc");
        memset(tu_launder(v[i]), 0xEE, n);
    }
    for (size_t i = 0; i < cnt; i++) T_FREE(v[i]);
    release();
    for (size_t i = 0; i < cnt; i++) {
        v[i] = T_CALLOC(1, n);
        TU_CHECK(v[i], "calloc");
        size_t z = tu_check_zero(v[i], n, 64 * 1024);
        if (z != SIZE_MAX) TU_FAIL("calloc(%zu) of released memory: byte %zu is not zero", n, z);
    }
    for (size_t i = 0; i < cnt; i++) T_FREE(v[i]);
    release();
    tu_os_free(v, cnt * sizeof(void *));
}

int main(int argc, char **argv)
{
    tu_init("memory_return", argc, argv);
    if (!tu_is_soham()) tu_skip("allocator is not sohamalloc (release policy not under test)");
    *(void **)(&f_collect) = tu_sym("soham_collect");
    *(void **)(&f_trim) = tu_sym("malloc_trim");
    TU_CHECK(f_collect, "soham_collect not exported");
    if (tu_rss() == 0) tu_skip("/proc/self/statm not readable");
    target = sizeof(void *) == 8 ? ((size_t)1 << 30) : ((size_t)256 << 20);
    tu_watchdog_secs = 60;
    pthread_t wd = tu_thread_start(tu_watchdog_main, NULL);
    tu_info("%zu MiB per phase", target / MiB);

    single_phase("small (16 B..1 KiB)", gen_small, tu_seed_v + 1);
    single_phase("medium (8..256 KiB)", gen_medium, tu_seed_v + 2);
    single_phase("large runs + solos", gen_large, tu_seed_v + 3);
    phase_threads_alloc();
    phase_threads_free();
    phase_fragmented();
    phase_reuse();
    single_phase("small again (reuse)", gen_small, tu_seed_v + 7);

    atomic_store(&tu_watchdog_stop, 1);
    tu_thread_join(wd);
    tu_info("PASS");
    return 0;
}
#endif
