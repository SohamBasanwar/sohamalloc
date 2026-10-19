/*
 * sizes_sweep -- every size 0..4096 (step 1), then step 7 up to 70000, then
 * random sizes up to 64 MiB.
 *
 * For every size: the block is non-NULL, aligned (16 for sohamalloc; the C
 * rule for others), malloc_usable_size >= n, and the whole usable range is
 * writable.  A sliding window of the most recent blocks stays live, so every
 * new block is checked against the overlap oracle and every block's pattern
 * is verified when it leaves the window (catches overlap, neighbours writing
 * past their usable size, and corruption of freed-then-reused memory).
 * The sweep is repeated with calloc (all-zero check) and as a realloc chain
 * growing one size step at a time (prefix preservation).
 */
#include "common/test_util.h"

enum { WIN = 509 };   /* prime, so slot reuse does not line up with size steps */

typedef struct {
    void *p;
    size_t n, u;
    uint64_t seed;
} blk;

static blk win[WIN];
static tu_ranges oracle;
static unsigned long nblocks;

static void retire(int s)
{
    blk *b = &win[s];
    if (!b->p) return;
    size_t bad = tu_check(b->p, b->u, b->u, b->seed);
    if (bad != SIZE_MAX)
        TU_FAIL("block %p (n=%zu usable=%zu) corrupted at offset %zu while live", b->p, b->n, b->u, bad);
    tu_ranges_remove(&oracle, s);
    T_FREE(b->p);
    b->p = NULL;
}

static void admit(int s, void *p, size_t n, const char *what)
{
    if (!p) TU_FAIL("%s(%zu) returned NULL", what, n);
    size_t a = tu_req_align(n);
    if ((uintptr_t)p & (a - 1)) TU_FAIL("%s(%zu) = %p is not %zu-aligned", what, n, p, a);
    size_t u = T_USABLE(p);
    if (u < n) TU_FAIL("%s(%zu): usable size %zu < n", what, n, u);
    int32_t c = tu_ranges_insert(&oracle, s, (uintptr_t)p, u);
    if (c >= 0)
        TU_FAIL("%s(%zu) = %p [usable %zu] overlaps live block %p [n=%zu usable %zu]", what, n, p, u,
                win[c].p, win[c].n, win[c].u);
    win[s].p = p;
    win[s].n = n;
    win[s].u = u;
    win[s].seed = tu_mix64(++nblocks);
    tu_fill(p, u, win[s].seed);
}

static size_t next_size(size_t n)
{
    return n < 4096 ? n + 1 : n + 7;
}

static void sweep_malloc(size_t limit)
{
    int s = 0;
    for (size_t n = 0; n <= limit; n = next_size(n)) {
        retire(s);
        admit(s, T_MALLOC(n), n, "malloc");
        s = (s + 1) % WIN;
    }
    for (int i = 0; i < WIN; i++) retire(i);
}

static void sweep_calloc(size_t limit)
{
    int s = 0;
    for (size_t n = 0; n <= limit; n = next_size(n)) {
        retire(s);   /* frees a dirty block first, so calloc often reuses dirty memory */
        /* Alternate the count/size split. */
        void *p = (n & 1) || n == 0 ? T_CALLOC(1, n) : T_CALLOC(n / 2, 2);
        if (!p) TU_FAIL("calloc(%zu) returned NULL", n);
        size_t z = tu_check_zero(p, n, (size_t)1 << 20);
        if (z != SIZE_MAX) TU_FAIL("calloc(%zu) = %p: non-zero byte at offset %zu", n, p, z);
        admit(s, p, n, "calloc");
        s = (s + 1) % WIN;
    }
    for (int i = 0; i < WIN; i++) retire(i);
}

static void sweep_realloc(size_t limit)
{
    /* Several interleaved chains, so in-place growth is not always possible. */
    enum { CH = 4 };
    void *p[CH] = { 0 };
    size_t n[CH] = { 0 };
    uint64_t seed[CH] = { 0 };
    size_t sz = 0;
    for (;;) {
        for (int c = 0; c < CH; c++) {
            void *q = T_REALLOC(p[c], sz);
            if (!q && sz) TU_FAIL("realloc(%p, %zu) returned NULL", p[c], sz);
            if (q) {
                size_t m = n[c] < sz ? n[c] : sz;
                size_t bad = p[c] ? tu_check(q, n[c], m, seed[c]) : SIZE_MAX;
                if (bad != SIZE_MAX) TU_FAIL("realloc %zu -> %zu lost content at %zu", n[c], sz, bad);
                size_t a = tu_req_align(sz);
                if ((uintptr_t)q & (a - 1)) TU_FAIL("realloc(.., %zu) = %p not %zu-aligned", sz, q, a);
                if (T_USABLE(q) < sz) TU_FAIL("realloc(.., %zu) usable %zu", sz, T_USABLE(q));
                seed[c] = tu_mix64(++nblocks);
                tu_fill(q, sz, seed[c]);
            }
            p[c] = q;          /* realloc(p, 0) may return NULL (p freed) */
            n[c] = q ? sz : 0;
        }
        if (sz >= limit) break;
        sz = next_size(sz);
        if (sz > limit) sz = limit;
    }
    for (int c = 0; c < CH; c++) T_FREE(p[c]);
}

static void random_big(size_t count, size_t max)
{
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v);
    enum { LIVE = 6 };
    int s = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = (size_t)tu_rng_logu(&r, 65536, max);
        retire(s);
        void *p;
        if (i % 3 == 2) {
            p = T_CALLOC(1, n);
            if (!p) TU_FAIL("calloc(1, %zu) returned NULL", n);
            size_t z = tu_check_zero(p, n, (size_t)256 << 10);
            if (z != SIZE_MAX) TU_FAIL("calloc(1, %zu): non-zero byte at %zu", n, z);
        } else {
            p = T_MALLOC(n);
        }
        admit(s, p, n, i % 3 == 2 ? "calloc" : "malloc");
        s = (s + 1) % LIVE;
    }
    for (int i = 0; i < LIVE; i++) retire(i);
}

int main(int argc, char **argv)
{
    tu_init("sizes_sweep", argc, argv);
    tu_ranges_init(&oracle, WIN);
    size_t limit = 70000;
    double t0 = tu_now();
    sweep_malloc(limit);
    double t1 = tu_now();
    sweep_calloc(limit);
    double t2 = tu_now();
    sweep_realloc(limit);
    double t3 = tu_now();
    size_t maxbig = sizeof(size_t) == 4 ? (size_t)32 << 20 : (size_t)64 << 20;
    random_big(tu_scaled_min(400, 40), maxbig);
    double t4 = tu_now();
    tu_info("PASS malloc %.2fs, calloc %.2fs, realloc %.2fs, random-big %.2fs (%lu blocks)",
            t1 - t0, t2 - t1, t3 - t2, t4 - t3, nblocks);
    tu_ranges_destroy(&oracle);
    return 0;
}
