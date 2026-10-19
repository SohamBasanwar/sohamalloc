/*
 * thread_churn -- thread attach/detach and late frees (plan §5.10, §8.4
 * lifecycle tests; spec §5.3 attach/detach and DEAD-thread paths).
 *
 * Phase 1: 20k short-lived threads (scaled), created in waves of W running
 * concurrently.  Thread kinds rotate:
 *   0 plain churn: malloc/calloc/realloc/free a mix of sizes;
 *   1 leaves blocks behind for main (or a later thread) to free;
 *   2 pthread key destructor frees a block, then re-arms itself with a fresh
 *     malloc for several destructor rounds, so mallocs and frees happen after
 *     the allocator's own TSD destructor may already have detached the heap;
 *   3 C11 thread (thrd_create) with a tss_t destructor that frees, plus a
 *     _Thread_local pointer released by that destructor;
 *   4 first allocator call is a free() of blocks left by dead threads;
 *   5 first allocator call is malloc(0), result left for main;
 *   6 large allocations (page-heap runs and, on 64-bit, solo mappings);
 *   7 many small blocks, half freed locally, half left behind.
 * Phase 2: many simultaneously live threads (default 1024) each holding
 * blocks at a barrier, so many heaps exist at once, then all exit and main
 * frees what they left.
 *
 * Every hand-over block carries a pattern verified by whoever frees it, and
 * destructor invocations are counted.
 */
#include "common/test_util.h"

#include <sched.h>
#if defined(__has_include)
#  if __has_include(<threads.h>) && !defined(__STDC_NO_THREADS__) && !defined(_WIN32)
#    include <threads.h>
#    define HAVE_C11_THREADS 1
#  endif
#endif

/* ------------------------------------------------------------- hand-over pool */
typedef struct { void *p; size_t n; uint64_t seed; } hblk;
enum { POOL_CAP = 1 << 16 };
static hblk *pool;
static size_t pool_n;
static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic size_t handed, reclaimed;

static void *make(size_t n, uint64_t seed)
{
    void *p = T_MALLOC(n);
    if (!p) TU_FAIL("malloc(%zu) returned NULL", n);
    size_t a = tu_req_align(n);
    if ((uintptr_t)p & (a - 1)) TU_FAIL("malloc(%zu) = %p misaligned", n, p);
    tu_fill(p, n, seed);
    return p;
}

static void verify_free(void *p, size_t n, uint64_t seed, const char *who)
{
    size_t bad = tu_check(p, n, n, seed);
    if (bad != SIZE_MAX) TU_FAIL("%s: block %p (n=%zu) corrupted at %zu", who, p, n, bad);
    T_FREE(p);
}

static void hand_over(void *p, size_t n, uint64_t seed)
{
    pthread_mutex_lock(&pool_mu);
    if (pool_n == POOL_CAP) {
        pthread_mutex_unlock(&pool_mu);
        verify_free(p, n, seed, "hand_over(full)");
        return;
    }
    pool[pool_n].p = p;
    pool[pool_n].n = n;
    pool[pool_n].seed = seed;
    pool_n++;
    pthread_mutex_unlock(&pool_mu);
    atomic_fetch_add_explicit(&handed, 1, memory_order_relaxed);
}

/* Take up to max blocks out of the pool and free them here. */
static size_t reclaim(size_t max, const char *who)
{
    hblk local[64];
    size_t got = 0;
    if (max > 64) max = 64;
    pthread_mutex_lock(&pool_mu);
    while (got < max && pool_n > 0) local[got++] = pool[--pool_n];
    pthread_mutex_unlock(&pool_mu);
    for (size_t i = 0; i < got; i++) verify_free(local[i].p, local[i].n, local[i].seed, who);
    atomic_fetch_add_explicit(&reclaimed, got, memory_order_relaxed);
    return got;
}

/* ------------------------------------------------------------- destructors */
static pthread_key_t key_rearm, key_plain;
static _Atomic unsigned long dtor_rearm_calls, dtor_plain_calls, tss_dtor_calls;

typedef struct { size_t n; uint64_t seed; int rounds_left; } dtag;

/* A destructor-owned block: a dtag header followed by the pattern for
   (n, seed) over [sizeof(dtag), n).  Sizes stay below TU_FULL_LIMIT, so the
   pattern is dense. */
static void *make_tagged(size_t n, uint64_t seed, int rounds)
{
    if (n < sizeof(dtag) + 8) n = sizeof(dtag) + 8;
    dtag *t = (dtag *)T_MALLOC(n);
    if (!t) TU_FAIL("malloc(%zu) returned NULL", n);
    t->n = n;
    t->seed = seed;
    t->rounds_left = rounds;
    tu_fill_range((unsigned char *)t, sizeof(dtag), n, seed);
    return t;
}

static void check_tagged(dtag *t, const char *who)
{
    size_t bad = tu_check_range((const unsigned char *)t, sizeof(dtag), t->n, t->seed);
    if (bad != SIZE_MAX) TU_FAIL("%s: destructor block %p (n=%zu) corrupted at %zu", who, (void *)t, t->n, bad);
}

/* Re-arming destructor: each round frees its block and, while rounds remain,
   mallocs a new one and re-registers it, which makes glibc run another
   destructor round (up to PTHREAD_DESTRUCTOR_ITERATIONS = 4). */
static void dtor_rearm(void *v)
{
    dtag *t = (dtag *)v;
    atomic_fetch_add_explicit(&dtor_rearm_calls, 1, memory_order_relaxed);
    check_tagged(t, "dtor_rearm");
    int left = t->rounds_left;
    uint64_t seed = t->seed;
    T_FREE(t);
    if (left > 0) {
        void *nx = make_tagged(64 + (size_t)left * 1000, tu_mix64(seed), left - 1);
        if (pthread_setspecific(key_rearm, nx) != 0) T_FREE(nx);
    }
}

static void dtor_plain(void *v)
{
    atomic_fetch_add_explicit(&dtor_plain_calls, 1, memory_order_relaxed);
    check_tagged((dtag *)v, "dtor_plain");
    T_FREE(v);
}

#ifdef HAVE_C11_THREADS
static tss_t tss_key;
static _Thread_local void *tl_block;     /* released by the tss destructor */
static void tss_dtor(void *v)
{
    atomic_fetch_add_explicit(&tss_dtor_calls, 1, memory_order_relaxed);
    check_tagged((dtag *)v, "tss_dtor");
    T_FREE(v);
    if (tl_block) {
        T_FREE(tl_block);
        tl_block = NULL;
    }
}
#endif

/* ------------------------------------------------------------- thread bodies */
typedef struct { unsigned idx; } targ;

static void churn_body(unsigned idx)
{
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ ((uint64_t)idx << 20));
    unsigned kind = idx % 8;
    uint64_t base = tu_mix64(idx);
    switch (kind) {
    case 0: {
        void *a[32];
        size_t n[32];
        for (int i = 0; i < 32; i++) {
            n[i] = tu_rand_size(&r, 65536);
            a[i] = (i & 3) == 3 ? T_CALLOC(1, n[i]) : T_MALLOC(n[i]);
            if (!a[i]) TU_FAIL("thread alloc NULL");
            tu_fill(a[i], n[i], base + (uint64_t)i);
        }
        for (int i = 0; i < 32; i += 2) {
            size_t m = n[i] * 2 + 1;
            void *q = T_REALLOC(a[i], m);
            if (!q) TU_FAIL("thread realloc NULL");
            if (tu_check(q, n[i], n[i], base + (uint64_t)i) != SIZE_MAX) TU_FAIL("realloc lost content");
            a[i] = q;
            n[i] = m;
            tu_fill(q, m, base + (uint64_t)i);
        }
        for (int i = 0; i < 32; i++) verify_free(a[i], n[i], base + (uint64_t)i, "kind0");
        break;
    }
    case 1:
        for (int i = 0; i < 8; i++) {
            size_t n = tu_rand_size(&r, 32768);
            hand_over(make(n, base + (uint64_t)i), n, base + (uint64_t)i);
        }
        break;
    case 2: {
        void *t = make_tagged(200 + idx % 500, base, 3);
        if (pthread_setspecific(key_rearm, t) != 0) TU_FAIL("pthread_setspecific");
        void *u = make_tagged(5000, base + 1, 0);
        if (pthread_setspecific(key_plain, u) != 0) TU_FAIL("pthread_setspecific");
        break;
    }
    case 3:
        /* Body for C11 threads is in c11_main (needs tss); fallback here. */
        for (int i = 0; i < 4; i++) {
            size_t n = 100 + (size_t)i * 300;
            verify_free(make(n, base + (uint64_t)i), n, base + (uint64_t)i, "kind3");
        }
        break;
    case 4:
        /* First allocator call: free of blocks owned by (mostly dead) threads. */
        reclaim(16, "kind4(first call)");
        break;
    case 5: {
        void *p = T_MALLOC(0);               /* first call */
        if (!p) TU_FAIL("malloc(0) as first call");
        hand_over(p, 0, 0);
        break;
    }
    case 6: {
        size_t n = (size_t)tu_rng_logu(&r, 300000, 4u << 20);
        void *p = make(n, base);
        size_t m = n + 70000;
        void *q = T_REALLOC(p, m);
        if (!q) TU_FAIL("realloc large NULL");
        if (tu_check(q, n, n, base) != SIZE_MAX) TU_FAIL("large realloc lost content");
        tu_fill(q, m, base + 9);
        if (idx % 64 == 6 && sizeof(size_t) == 8) {
            size_t s = (size_t)34 << 20;
            void *solo = T_MALLOC(s);
            if (!solo) TU_FAIL("malloc(%zu) NULL", s);
            ((volatile char *)solo)[0] = 1;
            ((volatile char *)solo)[s - 1] = 2;
            hand_over(solo, 0, 0);           /* freed by someone else later */
        }
        hand_over(q, m, base + 9);
        break;
    }
    case 7: {
        enum { K = 200 };
        void *a[K];
        size_t n[K];
        for (int i = 0; i < K; i++) {
            n[i] = 8 + (size_t)tu_rng_below(&r, 256);
            a[i] = make(n[i], base + (uint64_t)i);
        }
        for (int i = 0; i < K; i++) {
            if (i & 1) verify_free(a[i], n[i], base + (uint64_t)i, "kind7");
            else hand_over(a[i], n[i], base + (uint64_t)i);
        }
        break;
    }
    }
}

static void *pthread_main(void *arg)
{
    churn_body(((targ *)arg)->idx);
    return NULL;
}

#ifdef HAVE_C11_THREADS
static int c11_main(void *arg)
{
    unsigned idx = ((targ *)arg)->idx;
    void *t = make_tagged(300 + idx % 700, tu_mix64(idx), 0);
    if (tss_set(tss_key, t) != thrd_success) TU_FAIL("tss_set");
    tl_block = T_MALLOC(48 + idx % 100);
    if (!tl_block) TU_FAIL("malloc for _Thread_local");
    memset(tl_block, 7, 48 + idx % 100);
    churn_body(idx);
    return 0;
}
#endif

/* ------------------------------------------------------------- phase 2 */
static pthread_barrier_t bar;
typedef struct { unsigned idx; void *blocks[16]; size_t n[16]; } p2arg;

static void *p2_main(void *arg)
{
    p2arg *a = (p2arg *)arg;
    for (int i = 0; i < 16; i++) {
        a->n[i] = 16 + (size_t)((a->idx * 131 + (unsigned)i * 977) % 3000);
        a->blocks[i] = make(a->n[i], tu_mix64(((uint64_t)a->idx << 8) + (uint64_t)i));
    }
    pthread_barrier_wait(&bar);              /* all threads (and heaps) live at once */
    for (int i = 0; i < 16; i += 2) {
        verify_free(a->blocks[i], a->n[i], tu_mix64(((uint64_t)a->idx << 8) + (uint64_t)i), "p2");
        a->blocks[i] = NULL;
    }
    pthread_barrier_wait(&bar);
    return NULL;
}

int main(int argc, char **argv)
{
    tu_init("thread_churn", argc, argv);
    size_t total = tu_scaled_min(20000, 400);
    unsigned wave = 8;
    unsigned live = (unsigned)tu_scaled_min(1024, 64);
    for (int a = 1; a < argc; a++) {
        if (strncmp(argv[a], "--threads=", 10) == 0) total = strtoull(argv[a] + 10, NULL, 0);
        if (strncmp(argv[a], "--live=", 7) == 0) live = (unsigned)atoi(argv[a] + 7);
    }
    pool = (hblk *)tu_os_alloc(POOL_CAP * sizeof(hblk));
    if (pthread_key_create(&key_rearm, dtor_rearm) || pthread_key_create(&key_plain, dtor_plain))
        TU_FAIL("pthread_key_create");
#ifdef HAVE_C11_THREADS
    if (tss_create(&tss_key, tss_dtor) != thrd_success) TU_FAIL("tss_create");
#endif
    pthread_t wd = tu_thread_start(tu_watchdog_main, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    double t0 = tu_now();
    targ args[64];
    unsigned long n_c11 = 0, n_kind2 = 0;
    for (size_t done = 0; done < total; done += wave) {
        pthread_t th[64];
#ifdef HAVE_C11_THREADS
        thrd_t ct[64];
        int is_c11[64];
#endif
        unsigned w = (unsigned)((total - done) < wave ? (total - done) : wave);
        for (unsigned i = 0; i < w; i++) {
            args[i].idx = (unsigned)(done + i);
            if (args[i].idx % 8 == 2) n_kind2++;
#ifdef HAVE_C11_THREADS
            is_c11[i] = (args[i].idx % 8 == 3);
            if (is_c11[i]) {
                if (thrd_create(&ct[i], c11_main, &args[i]) != thrd_success) TU_FAIL("thrd_create");
                n_c11++;
                continue;
            }
#endif
            int e = pthread_create(&th[i], &attr, pthread_main, &args[i]);
            if (e) TU_FAIL("pthread_create: %d (after %zu threads)", e, done + i);
        }
        for (unsigned i = 0; i < w; i++) {
#ifdef HAVE_C11_THREADS
            if (is_c11[i]) { thrd_join(ct[i], NULL); continue; }
#endif
            tu_thread_join(th[i]);
        }
        /* Main frees some leftovers now, leaves the rest to later threads. */
        if ((done / wave) % 3 == 0) reclaim(64, "main");
        tu_tick();
    }
    while (reclaim(64, "main(final)")) { }
    double t1 = tu_now();
    TU_CHECK(atomic_load(&handed) == atomic_load(&reclaimed), "handed %zu reclaimed %zu",
             atomic_load(&handed), atomic_load(&reclaimed));
    /* kind 2: one plain destructor call and 4 re-arm rounds (3 re-arms). */
    TU_CHECK(atomic_load(&dtor_plain_calls) == n_kind2, "plain destructor calls %lu, want %lu",
             atomic_load(&dtor_plain_calls), n_kind2);
    TU_CHECK(atomic_load(&dtor_rearm_calls) == 4 * n_kind2, "re-arm destructor calls %lu, want %lu",
             atomic_load(&dtor_rearm_calls), 4 * n_kind2);
#ifdef HAVE_C11_THREADS
    TU_CHECK(atomic_load(&tss_dtor_calls) == n_c11, "tss destructor calls %lu want %lu",
             atomic_load(&tss_dtor_calls), n_c11);
#endif
    tu_info("phase 1: %zu threads in %.2fs (%zu blocks handed over, %lu C11 threads)", total, t1 - t0,
            atomic_load(&handed), n_c11);

    /* Phase 2 */
    p2arg *pa = (p2arg *)tu_os_alloc(live * sizeof(p2arg));
    pthread_t *pt = (pthread_t *)tu_os_alloc(live * sizeof(pthread_t));
    pthread_barrier_init(&bar, NULL, live);
    for (unsigned i = 0; i < live; i++) {
        pa[i].idx = i;
        int e = pthread_create(&pt[i], &attr, p2_main, &pa[i]);
        if (e) TU_FAIL("pthread_create #%u of %u live: %d", i, live, e);
    }
    for (unsigned i = 0; i < live; i++) tu_thread_join(pt[i]);
    for (unsigned i = 0; i < live; i++)
        for (int k = 1; k < 16; k += 2)
            verify_free(pa[i].blocks[k], pa[i].n[k], tu_mix64(((uint64_t)i << 8) + (uint64_t)k), "p2 main");
    pthread_barrier_destroy(&bar);
    double t2 = tu_now();
    tu_info("phase 2: %u simultaneously live threads in %.2fs", live, t2 - t1);

    atomic_store(&tu_watchdog_stop, 1);
    tu_thread_join(wd);
    pthread_attr_destroy(&attr);
    tu_info("PASS %.2fs", t2 - t0);
    return 0;
}
