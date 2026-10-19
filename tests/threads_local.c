/*
 * threads_local -- N threads (default 8) of heavy thread-local churn.
 *
 * Each thread runs its own randomised malloc/calloc/realloc/free mix with a
 * private slot table, a private overlap oracle and per-block patterns, plus
 * "sawtooth" phases that grow the live set to a peak and free it all (page
 * empty -> refill transitions, spare pages, return to the page heap).  A
 * block handed to two threads at once is caught by the pattern checks; a
 * block handed out twice within one thread by the oracle.
 *
 * Arguments: --threads=N, --seed=S.  Iterations scale with TEST_SCALE.
 */
#include "common/test_util.h"

typedef struct {
    void *p;
    size_t n, u;
    uint64_t seed;
} slot_t;

typedef struct {
    int id;
    size_t ops;
    pthread_t th;
    unsigned long allocs;
} targ_t;

enum { NSLOTS = 2048 };

static _Atomic int start_flag;

static void tl_verify(int id, slot_t *s, const char *when)
{
    size_t bad = tu_check(s->p, s->u, s->u, s->seed);
    if (bad != SIZE_MAX)
        TU_FAIL("thread %d %s: block %p (n=%zu usable=%zu) corrupted at offset %zu", id, when, s->p, s->n, s->u, bad);
}

static void tl_install(int id, tu_ranges *o, slot_t *sl, size_t i, void *p, size_t n, uint64_t seed, const char *what)
{
    if (!p) TU_FAIL("thread %d: %s(%zu) returned NULL", id, what, n);
    size_t a = tu_req_align(n);
    if ((uintptr_t)p & (a - 1)) TU_FAIL("thread %d: %s(%zu) = %p not %zu-aligned", id, what, n, p, a);
    size_t u = T_USABLE(p);
    if (u < n) TU_FAIL("thread %d: %s(%zu) usable %zu", id, what, n, u);
    int32_t c = tu_ranges_insert(o, (int32_t)i, (uintptr_t)p, u);
    if (c >= 0) TU_FAIL("thread %d: %s(%zu) = %p overlaps own live block %p", id, what, n, p, sl[c].p);
    sl[i].p = p;
    sl[i].n = n;
    sl[i].u = u;
    sl[i].seed = seed;
    tu_fill(p, u, seed);
}

static void tl_release(int id, tu_ranges *o, slot_t *sl, size_t i)
{
    tl_verify(id, &sl[i], "free");
    tu_ranges_remove(o, (int32_t)i);
    T_FREE(sl[i].p);
    sl[i].p = NULL;
}

static void *worker(void *argp)
{
    targ_t *ta = (targ_t *)argp;
    int id = ta->id;
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ ((uint64_t)id << 40));
    slot_t *sl = (slot_t *)tu_os_alloc(NSLOTS * sizeof(slot_t));
    tu_ranges o;
    tu_ranges_init(&o, NSLOTS);
    while (!atomic_load_explicit(&start_flag, memory_order_acquire)) { }

    size_t maxsz = 256 * 1024;
    uint64_t op = 0;
    for (size_t round = 0; op < ta->ops; round++) {
        /* Phase A: random churn. */
        size_t churn = ta->ops / 8 + 1;
        for (size_t k = 0; k < churn && op < ta->ops; k++, op++) {
            size_t i = (size_t)tu_rng_below(&r, NSLOTS);
            uint64_t c = tu_rng_below(&r, 100);
            uint64_t seed = tu_mix64(((uint64_t)id << 48) ^ op);
            if (!sl[i].p) {
                size_t n = tu_rand_size(&r, maxsz);
                if (c < 80) {
                    tl_install(id, &o, sl, i, T_MALLOC(n), n, seed, "malloc");
                } else {
                    void *p = T_CALLOC(1, n);
                    if (!p) TU_FAIL("thread %d calloc(%zu) NULL", id, n);
                    size_t z = tu_check_zero(p, n, (size_t)1 << 20);
                    if (z != SIZE_MAX) TU_FAIL("thread %d calloc(%zu) non-zero at %zu", id, n, z);
                    tl_install(id, &o, sl, i, p, n, seed, "calloc");
                }
                ta->allocs++;
            } else if (c < 60) {
                tl_release(id, &o, sl, i);
            } else {
                tl_verify(id, &sl[i], "pre-realloc");
                size_t n = c < 80 ? sl[i].n + (size_t)tu_rng_below(&r, 2048) : (size_t)tu_rng_below(&r, sl[i].n + 1);
                if (n == 0) n = 1;
                tu_ranges_remove(&o, (int32_t)i);
                void *q = T_REALLOC(sl[i].p, n);
                if (!q) TU_FAIL("thread %d realloc(%zu) NULL", id, n);
                size_t keep = sl[i].n < n ? sl[i].n : n;
                size_t bad = tu_check(q, sl[i].u, keep, sl[i].seed);
                if (bad != SIZE_MAX) TU_FAIL("thread %d realloc %zu->%zu lost content at %zu", id, sl[i].n, n, bad);
                sl[i].p = NULL;
                tl_install(id, &o, sl, i, q, n, seed, "realloc");
                ta->allocs++;
            }
        }
        /* Phase B: sawtooth -- fill every slot with one size class band, then
           free everything in a random order. */
        size_t band = (size_t)1 << (4 + round % 12);     /* 16 B .. 32 KiB */
        for (size_t i = 0; i < NSLOTS; i++) {
            if (sl[i].p) tl_release(id, &o, sl, i);
            size_t n = band / 2 + (size_t)tu_rng_below(&r, band / 2 + 1);
            tl_install(id, &o, sl, i, T_MALLOC(n), n, tu_mix64(op + i), "malloc(sawtooth)");
            ta->allocs++;
        }
        for (size_t k = 0; k < NSLOTS; k++) {
            size_t i = (k * 1031) % NSLOTS;              /* 1031 coprime with 2048: a permutation */
            tl_release(id, &o, sl, i);
        }
        op += NSLOTS;
        tu_tick();
    }
    for (size_t i = 0; i < NSLOTS; i++)
        if (sl[i].p) tl_release(id, &o, sl, i);
    tu_ranges_destroy(&o);
    tu_os_free(sl, NSLOTS * sizeof(slot_t));
    return NULL;
}

int main(int argc, char **argv)
{
    tu_init("threads_local", argc, argv);
    int nth = 8;
    for (int a = 1; a < argc; a++)
        if (strncmp(argv[a], "--threads=", 10) == 0) nth = atoi(argv[a] + 10);
    if (nth < 1 || nth > 256) nth = 8;
    size_t ops = tu_scaled_min(1000000, 20000);
    tu_info("API %s, %d threads x %zu ops, seed %llu", T_API_NAME, nth, ops, (unsigned long long)tu_seed_v);
    targ_t *ta = (targ_t *)tu_os_alloc((size_t)nth * sizeof(targ_t));
    pthread_t wd = tu_thread_start(tu_watchdog_main, NULL);
    double t0 = tu_now();
    for (int i = 0; i < nth; i++) {
        ta[i].id = i;
        ta[i].ops = ops;
        ta[i].th = tu_thread_start(worker, &ta[i]);
    }
    atomic_store_explicit(&start_flag, 1, memory_order_release);
    unsigned long total = 0;
    for (int i = 0; i < nth; i++) {
        tu_thread_join(ta[i].th);
        total += ta[i].allocs;
    }
    atomic_store(&tu_watchdog_stop, 1);
    tu_thread_join(wd);
    tu_info("PASS %.2fs, %lu allocations", tu_now() - t0, total);
    tu_os_free(ta, (size_t)nth * sizeof(targ_t));
    return 0;
}
