/*
 * large_and_huge -- large blocks (page-heap runs, 192 KiB .. 32 MiB) and huge
 * blocks (dedicated "solo" mappings, >= 32 MiB) across every size boundary
 * of docs/IMPL_SPEC_v0.md §3-§4 (slice S, hugeblock H = 2 MiB, run classes
 * 192/256 KiB, solo threshold 32 MiB), for any allocator.
 *
 *   1 boundary sweep: sizes at and around 160 KiB .. 1 GiB (256 MiB on
 *     32-bit), each checked for alignment, usable >= n, a writable usable
 *     range (first/last usable byte) and pattern integrity, with up to 16
 *     neighbours live and the overlap oracle over all of them;
 *   2 calloc of large and huge blocks is zero, also when it reuses dirty
 *     memory freed just before;
 *   3 realloc chains that grow 100 KiB -> 512 MiB (128 MiB on 32-bit) by
 *     doubling and +1 steps, then shrink back, preserving content each step
 *     (runs -> solos -> runs, mremap paths);
 *   4 over-aligned large requests: alignment 4 KiB .. 64 MiB (beyond H, i.e.
 *     solo alignment) with sizes 1 B .. 40 MiB via posix_memalign,
 *     aligned_alloc and memalign;
 *   5 a pool of random large blocks (256 KiB .. 8 MiB, plus some solos) with
 *     random replacement order, patterns verified on every free;
 *   6 4 threads churning large blocks and handing them to each other through
 *     an exchange array (cross-thread frees of runs and solos);
 *   7 address-space reuse: thousands of 64 MiB (16 MiB on 32-bit) alloc/free
 *     cycles must not grow the virtual size (no leaked mappings).
 */
#include "common/test_util.h"

#define KiB ((size_t)1 << 10)
#define MiB ((size_t)1 << 20)
#define IS64 (sizeof(void *) == 8)

/* Light content pattern for big blocks: the suite's pattern (test_util.h)
   writes one word per 4 KiB, which faults in every page; here blocks above
   1 MiB get the head and tail 4 KiB plus one word per 64 KiB, so a test of
   gigabytes of large blocks stays fast while every run/solo still carries
   content that is verified (and realloc-preserved). */
#define LF_FULL ((size_t)1 << 20)
#define LF_STRIDE ((size_t)64 << 10)
static size_t lf_regions(unsigned char *p, size_t n, size_t upto, uint64_t seed, tu_region_fn fn)
{
    if (n <= LF_FULL) return tu_regions(p, n, upto, seed, fn);
    if (upto > n) upto = n;
    size_t r, b = TU_EDGE < upto ? TU_EDGE : upto;
    if (b && (r = fn(p, 0, b, seed)) != SIZE_MAX) return r;
    size_t tail = (n - TU_EDGE) & ~(size_t)7;
    for (size_t o = LF_STRIDE; o + 8 <= tail && o + 8 <= upto; o += LF_STRIDE)
        if ((r = fn(p, o, o + 8, seed)) != SIZE_MAX) return r;
    if (tail < upto && (r = fn(p, tail, upto, seed)) != SIZE_MAX) return r;
    return SIZE_MAX;
}
static void lf_fill(void *p, size_t n, uint64_t seed) { lf_regions((unsigned char *)p, n, n, seed, tu_fill_fn); }
static size_t lf_check(const void *p, size_t n_written, size_t upto, uint64_t seed)
{
    return lf_regions((unsigned char *)(uintptr_t)p, n_written, upto, seed, tu_check_fn);
}

static tu_ranges oracle;
enum { ORACLE_CAP = 4096 };

/* Checks a new (or reallocated) block; writes only its slack [n, usable). */
static void check_new(void *p, size_t n, size_t align, const char *what)
{
    if (!p) TU_FAIL("%s(%zu) returned NULL (errno %d)", what, n, errno);
    size_t a = align > tu_req_align(n) ? align : tu_req_align(n);
    if ((uintptr_t)p & (a - 1)) TU_FAIL("%s(%zu) = %p is not %zu-aligned", what, n, p, a);
    size_t u = T_USABLE(p);
    if (u < n) TU_FAIL("%s(%zu) = %p: usable size %zu < n", what, n, p, u);
    /* The usable slack beyond n is writable (bytes [0, n) are left alone:
       callers check content there). */
    unsigned char *c = (unsigned char *)tu_launder(p);
    if (u > n) { c[n] = 0x11; c[u - 1] = 0x22; }
}

/* ------------------------------------------------------------ 1 sweep */
static void sweep(void)
{
    static const size_t base[] = {
        160 * KiB, 192 * KiB, 224 * KiB, 256 * KiB, 256 * KiB + 1, 320 * KiB, 512 * KiB, 1 * MiB,
        2 * MiB - 64 * KiB, 2 * MiB, 2 * MiB + 1, 2 * MiB + 64 * KiB, 3 * MiB, 4 * MiB, 8 * MiB - 1,
        16 * MiB, 31 * MiB, 32 * MiB - 64 * KiB, 32 * MiB - 1, 32 * MiB, 32 * MiB + 1, 33 * MiB,
        48 * MiB, 64 * MiB, 64 * MiB + 4097, 100 * MiB, 128 * MiB, 256 * MiB,
        512 * MiB, 1024 * MiB,
    };
    const size_t maxn = IS64 ? 1024 * MiB : 256 * MiB;
    enum { LIVE = 16 };
    struct { void *p; size_t n; uint64_t seed; int slot; } live[LIVE] = { { 0 } };
    int next_slot = 0;
    size_t count = 0;
    for (size_t bi = 0; bi < sizeof base / sizeof base[0]; bi++) {
        for (int d = -2; d <= 2; d++) {
            size_t n = base[bi] + (size_t)((ptrdiff_t)d * 4096);
            if (n > maxn || n < 16) continue;
            int li = (int)(count % LIVE);
            if (live[li].p) {
                size_t bad = lf_check(live[li].p, live[li].n, live[li].n, live[li].seed);
                if (bad != SIZE_MAX) TU_FAIL("sweep: %zu-byte block %p corrupted at %zu", live[li].n, live[li].p, bad);
                tu_ranges_remove(&oracle, live[li].slot);
                T_FREE(live[li].p);
                live[li].p = NULL;
            }
            /* Huge blocks are not kept live, to bound memory. */
            int keep = n <= 64 * MiB;
            void *p = (count & 1) ? T_MALLOC(n) : T_CALLOC(1, n);
            check_new(p, n, 0, (count & 1) ? "malloc" : "calloc");
            uint64_t seed = tu_mix64(n ^ 0xb16b00b5);
            lf_fill(p, n, seed);
            int slot = next_slot++ % ORACLE_CAP;
            int32_t c = tu_ranges_insert(&oracle, slot, (uintptr_t)p, T_USABLE(p));
            if (c >= 0) TU_FAIL("sweep: block %p (%zu) overlaps a live block", p, n);
            if (keep) {
                live[li].p = p; live[li].n = n; live[li].seed = seed; live[li].slot = slot;
            } else {
                size_t bad = lf_check(p, n, n, seed);
                if (bad != SIZE_MAX) TU_FAIL("sweep: huge block %p (%zu) corrupted at %zu", p, n, bad);
                tu_ranges_remove(&oracle, slot);
                T_FREE(p);
            }
            count++;
            tu_tick();
        }
    }
    for (int i = 0; i < LIVE; i++) {
        if (!live[i].p) continue;
        size_t bad = lf_check(live[i].p, live[i].n, live[i].n, live[i].seed);
        if (bad != SIZE_MAX) TU_FAIL("sweep: %zu-byte block corrupted at %zu", live[i].n, bad);
        tu_ranges_remove(&oracle, live[i].slot);
        T_FREE(live[i].p);
    }
    tu_info("sweep: %zu sizes up to %zu MiB", count, maxn / MiB);
}

/* ------------------------------------------------------------ 2 calloc */
static void calloc_zero(void)
{
    static const size_t ns[] = { 200 * KiB, 256 * KiB, 300 * KiB, 2 * MiB, 5 * MiB + 3, 31 * MiB,
                                 33 * MiB, 80 * MiB };
    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
            size_t n = ns[i];
            void *d = T_MALLOC(n);
            check_new(d, n, 0, "malloc");
            memset(tu_launder(d), 0xFF, T_USABLE(d));
            T_FREE(d);
            void *z = T_CALLOC(n / 8, 8);
            if (!z) TU_FAIL("calloc(%zu, 8) returned NULL", n / 8);
            size_t bad = tu_check_zero(z, n / 8 * 8, 256 * KiB);
            if (bad != SIZE_MAX) TU_FAIL("calloc(%zu) after a dirty free: byte %zu is non-zero", n, bad);
            check_new(z, n / 8 * 8, 0, "calloc");
            T_FREE(z);
        }
    }
}

/* ------------------------------------------------------------ 3 realloc */
static void realloc_chains(void)
{
    const size_t top = IS64 ? 512 * MiB : 128 * MiB;
    size_t n = 100 * KiB;
    uint64_t seed = 77;
    void *p = T_MALLOC(n);
    check_new(p, n, 0, "malloc");
    lf_fill(p, n, seed);
    size_t steps = 0;
    /* Grow: double, with an occasional +1 / +4 KiB nudge. */
    while (n < top) {
        size_t m = (steps % 3 == 1) ? n + 1 : (steps % 3 == 2) ? n + 4096 : n * 2;
        if (m > top) m = top;
        void *q = T_REALLOC(p, m);
        if (!q) TU_FAIL("realloc(%zu -> %zu) returned NULL", n, m);
        check_new(q, m, 0, "realloc");
        size_t bad = lf_check(q, n, n, seed);
        if (bad != SIZE_MAX) TU_FAIL("realloc grow %zu -> %zu lost content at %zu", n, m, bad);
        seed++;
        lf_fill(q, m, seed);
        p = q;
        n = m;
        steps++;
        tu_tick();
    }
    /* Shrink back in uneven steps. */
    while (n > 64 * KiB) {
        size_t m = n / 3 + 4093;
        void *q = T_REALLOC(p, m);
        if (!q) TU_FAIL("realloc(%zu -> %zu) returned NULL", n, m);
        check_new(q, m, 0, "realloc");
        size_t bad = lf_check(q, n, m, seed);
        if (bad != SIZE_MAX) TU_FAIL("realloc shrink %zu -> %zu lost content at %zu", n, m, bad);
        seed++;
        lf_fill(q, m, seed);
        p = q;
        n = m;
        steps++;
        tu_tick();
    }
    T_FREE(p);

    /* Ping-pong across the solo threshold and the run/class boundary. */
    static const size_t pp[] = { 250 * KiB, 40 * MiB, 300 * KiB, 33 * MiB, 31 * MiB, 33 * MiB, 192 * KiB,
                                 64 * MiB, 64 * MiB + 1, 1 * MiB, 32 * MiB, 32 * MiB - 1, 100 };
    p = T_MALLOC(pp[0]);
    check_new(p, pp[0], 0, "malloc");
    lf_fill(p, pp[0], 1000);
    for (size_t i = 1; i < sizeof pp / sizeof pp[0]; i++) {
        void *q = T_REALLOC(p, pp[i]);
        if (!q) TU_FAIL("realloc(%zu -> %zu) NULL", pp[i - 1], pp[i]);
        check_new(q, pp[i], 0, "realloc");
        size_t keep = pp[i] < pp[i - 1] ? pp[i] : pp[i - 1];
        size_t bad = lf_check(q, pp[i - 1], keep, 1000 + i - 1);
        if (bad != SIZE_MAX) TU_FAIL("realloc %zu -> %zu lost content at %zu", pp[i - 1], pp[i], bad);
        lf_fill(q, pp[i], 1000 + i);
        p = q;
    }
    T_FREE(p);
    tu_info("realloc: %zu chain steps up to %zu MiB", steps, top / MiB);
}

/* ------------------------------------------------------------ 4 aligned */
static void aligned_large(void)
{
    static const size_t ns[] = { 1, 4096, 100 * KiB, 300 * KiB, 3 * MiB, 33 * MiB, 40 * MiB };
    size_t maxa = IS64 ? 64 * MiB : 32 * MiB;
    size_t count = 0;
    for (size_t a = 4096; a <= maxa; a <<= 1) {
        for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
            size_t n = ns[i];
            if (!IS64 && n > 3 * MiB && a > 4 * MiB) continue;
            for (int how = 0; how < 3; how++) {
                void *p = NULL;
                const char *what;
                if (how == 0) {
                    int r = T_POSIX_MEMALIGN(&p, a, n);
                    if (r) TU_FAIL("posix_memalign(%zu, %zu) = %d", a, n, r);
                    what = "posix_memalign";
                } else if (how == 1) {
                    size_t nn = (n + a - 1) / a * a;        /* C11 aligned_alloc: n a multiple of a */
                    p = T_ALIGNED_ALLOC(a, nn);
                    n = nn;
                    what = "aligned_alloc";
                } else {
                    p = T_MEMALIGN(a, n);
                    what = "memalign";
                }
                check_new(p, n, a, what);
                lf_fill(p, n, a ^ n);
                if (lf_check(p, n, n, a ^ n) != SIZE_MAX) TU_FAIL("%s(%zu, %zu) block corrupted", what, a, n);
                T_FREE(p);
                n = ns[i];
                count++;
            }
            tu_tick();
        }
    }
    tu_info("aligned: %zu requests, alignment 4 KiB..%zu MiB", count, maxa / MiB);
}

/* ------------------------------------------------------------ 5 pool */
static void pool(void)
{
    enum { N = 96 };
    struct { void *p; size_t n; uint64_t seed; } b[N] = { { 0 } };
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ 0x9001);
    size_t ops = tu_scaled_min(6000, 600), live_bytes = 0;
    const size_t budget = IS64 ? 1536 * MiB : 384 * MiB;
    for (size_t op = 0; op < ops; op++) {
        int i = (int)tu_rng_below(&r, N);
        if (b[i].p) {
            size_t bad = lf_check(b[i].p, b[i].n, b[i].n, b[i].seed);
            if (bad != SIZE_MAX) TU_FAIL("pool: %zu-byte block %p corrupted at %zu (op %zu)", b[i].n, b[i].p, bad, op);
            tu_ranges_remove(&oracle, i);
            T_FREE(b[i].p);
            live_bytes -= b[i].n;
            b[i].p = NULL;
        }
        size_t n = (tu_rng_below(&r, 16) == 0) ? (size_t)tu_rng_range(&r, 32 * MiB, IS64 ? 96 * MiB : 40 * MiB)
                                               : (size_t)tu_rng_logu(&r, 256 * KiB, 8 * MiB);
        if (live_bytes + n > budget) continue;
        void *p = tu_rng_below(&r, 4) ? T_MALLOC(n) : T_CALLOC(1, n);
        check_new(p, n, 0, "pool alloc");
        int32_t c = tu_ranges_insert(&oracle, i, (uintptr_t)p, T_USABLE(p));
        if (c >= 0) TU_FAIL("pool: block %p (%zu) overlaps live block in slot %d", p, n, c);
        b[i].p = p; b[i].n = n; b[i].seed = tu_mix64(op);
        lf_fill(p, n, b[i].seed);
        live_bytes += n;
        if ((op & 31) == 0) tu_tick();
    }
    for (int i = 0; i < N; i++) {
        if (!b[i].p) continue;
        if (lf_check(b[i].p, b[i].n, b[i].n, b[i].seed) != SIZE_MAX) TU_FAIL("pool: final check");
        tu_ranges_remove(&oracle, i);
        T_FREE(b[i].p);
    }
    tu_info("pool: %zu ops", ops);
}

/* ------------------------------------------------------------ 6 threads */
enum { NT = 4, XN = 32 };
typedef struct { void *p; size_t n; uint64_t seed; } xblk;
static xblk *xch;                       /* OS memory, XN entries, guarded by xlock */
static pthread_mutex_t xlock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned long x_cross;

static void verify_free(xblk *b, const char *who)
{
    size_t bad = lf_check(b->p, b->n, b->n, b->seed);
    if (bad != SIZE_MAX) TU_FAIL("%s: %zu-byte block %p corrupted at %zu", who, b->n, b->p, bad);
    T_FREE(b->p);
    b->p = NULL;
}

static void *churn(void *arg)
{
    int id = (int)(intptr_t)arg;
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ ((uint64_t)id << 32) ^ 0xc4u);
    xblk mine[8] = { { 0 } };
    size_t ops = tu_scaled_min(3000, 300);
    for (size_t op = 0; op < ops; op++) {
        int i = (int)tu_rng_below(&r, 8);
        if (mine[i].p) {
            if (tu_rng_below(&r, 2)) {
                verify_free(&mine[i], "churn");
            } else {
                /* Swap with the exchange: the other thread frees ours. */
                pthread_mutex_lock(&xlock);
                int k = (int)tu_rng_below(&r, XN);
                xblk old = xch[k];
                xch[k] = mine[i];
                pthread_mutex_unlock(&xlock);
                mine[i].p = NULL;
                if (old.p) { verify_free(&old, "churn (cross-thread)"); atomic_fetch_add(&x_cross, 1); }
            }
        }
        size_t n = tu_rng_below(&r, 20) == 0 ? (size_t)tu_rng_range(&r, 32 * MiB, 40 * MiB)
                                             : (size_t)tu_rng_logu(&r, 200 * KiB, 4 * MiB);
        void *p = T_MALLOC(n);
        check_new(p, n, 0, "churn malloc");
        mine[i].p = p; mine[i].n = n; mine[i].seed = tu_mix64(((uint64_t)id << 48) ^ op);
        lf_fill(p, n, mine[i].seed);
        if ((op & 15) == 0) tu_tick();
    }
    for (int i = 0; i < 8; i++) if (mine[i].p) verify_free(&mine[i], "churn final");
    return NULL;
}

static void threads(void)
{
    xch = (xblk *)tu_os_alloc(XN * sizeof(xblk));
    pthread_t t[NT];
    for (int i = 0; i < NT; i++) t[i] = tu_thread_start(churn, (void *)(intptr_t)i);
    for (int i = 0; i < NT; i++) tu_thread_join(t[i]);
    for (int k = 0; k < XN; k++) if (xch[k].p) verify_free(&xch[k], "exchange final");
    tu_os_free(xch, XN * sizeof(xblk));
    tu_info("threads: %d threads, %lu cross-thread large frees", NT, atomic_load(&x_cross));
}

/* ------------------------------------------------------------ 7 VA reuse */
#ifndef _WIN32
static size_t vsize(void)
{
    char buf[256];
    int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    long k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0) return 0;
    buf[k] = 0;
    return (size_t)strtoull(buf, NULL, 10) * tu_page_size();
}
#endif

static void va_reuse(void)
{
    size_t n = IS64 ? 64 * MiB : 16 * MiB;
    size_t iters = tu_scaled_min(3000, 300);
#ifndef _WIN32
    size_t v0 = vsize();
#endif
    for (size_t i = 0; i < iters; i++) {
        size_t m = n + (i % 7) * 4096 * 3;
        void *p = (i & 3) ? T_MALLOC(m) : T_CALLOC(1, m);
        if (!p) TU_FAIL("iteration %zu: malloc(%zu) failed (leaked address space?)", i, m);
        unsigned char *c = (unsigned char *)p;
        if (!(i & 3) && (c[0] | c[m / 2] | c[m - 1])) TU_FAIL("calloc(%zu) not zero", m);
        c[0] = 1; c[m / 2] = 2; c[m - 1] = 3;
        T_FREE(p);
        if ((i & 63) == 0) tu_tick();
    }
#ifndef _WIN32
    size_t v1 = vsize();
    tu_info("va reuse: %zu x %zu MiB, VSZ %zu -> %zu MiB", iters, n / MiB, v0 / MiB, v1 / MiB);
    if (v0 && v1 > v0 + (IS64 ? 2048 : 256) * MiB)
        TU_FAIL("virtual size grew from %zu to %zu MiB over %zu alloc/free cycles of %zu MiB: leaked mappings",
                v0 / MiB, v1 / MiB, iters, n / MiB);
#endif
}

int main(int argc, char **argv)
{
    tu_init("large_and_huge", argc, argv);
    tu_info("API %s, %s", T_API_NAME, IS64 ? "64-bit" : "32-bit");
    tu_ranges_init(&oracle, ORACLE_CAP);
    tu_watchdog_secs = 60;
    pthread_t wd = tu_thread_start(tu_watchdog_main, NULL);
    static const struct { const char *name; void (*fn)(void); } phases[] = {
        { "sweep", sweep }, { "calloc_zero", calloc_zero }, { "realloc_chains", realloc_chains },
        { "aligned_large", aligned_large }, { "pool", pool }, { "threads", threads }, { "va_reuse", va_reuse },
    };
    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++) {
        double t0 = tu_now();
        phases[i].fn();
        tu_info("  %s: %.2f s", phases[i].name, tu_now() - t0);
    }
    atomic_store(&tu_watchdog_stop, 1);
    tu_thread_join(wd);
    tu_ranges_destroy(&oracle);
    tu_info("PASS");
    return 0;
}
