/*
 * random_ops -- randomised differential test of the allocation API against a
 * reference oracle (plan §8.4 "randomised differential testing").
 *
 * 2e6 random operations (scaled by TEST_SCALE) over a table of slots:
 * malloc, calloc, realloc (grow, shrink, NULL, to 0), posix_memalign,
 * aligned_alloc, memalign and free (sized half the time in the prefixed
 * build).  The oracle is the glibc contract plus a shadow map:
 *   - no two live blocks overlap (exact interval check over usable ranges);
 *   - every result is aligned (requested alignment and the minimum);
 *   - usable size >= request, and every usable byte is writable;
 *   - each block carries a per-block seeded pattern over its usable range,
 *     verified on free, on realloc (preserved prefix) and in periodic full
 *     sweeps, so any overlap or stray write is detected;
 *   - calloc memory is all zero, both fresh and reused;
 *   - impossible requests fail with NULL and leave state untouched.
 * The run is deterministic for a given seed: argv[1] or --seed=N or
 * TEST_SEED.  Every failure prints the op index and seed to reproduce it.
 *
 * Built twice: against the malloc family (LD_PRELOAD) and, with
 * -DTEST_PREFIXED, against soham_* directly (tests/prefixed/).
 */
#include "common/test_util.h"

typedef struct {
    void *p;
    size_t n;        /* requested */
    size_t u;        /* usable at allocation; pattern covers [0, u) */
    uint64_t seed;
} slot_t;

static slot_t *slots;
static size_t nslots;
static tu_ranges oracle;
static uint64_t opno;
static uint64_t seed0;

static unsigned long cnt_malloc, cnt_calloc, cnt_realloc, cnt_aligned, cnt_free, cnt_fail_ok;

static void opfail_hdr(void)
{
    char b[96];
    snprintf(b, sizeof b, "op %llu, seed %llu", (unsigned long long)opno, (unsigned long long)seed0);
    tu_write_str(2, "random_ops context: ");
    tu_write_str(2, b);
    tu_write_str(2, "\n");
}

static void verify_slot(size_t i, const char *when)
{
    slot_t *s = &slots[i];
    size_t bad = tu_check(s->p, s->u, s->u, s->seed);
    if (bad != SIZE_MAX) {
        opfail_hdr();
        TU_FAIL("%s: block %p (slot %zu, n=%zu, usable=%zu) corrupted at offset %zu", when, s->p, i, s->n,
                s->u, bad);
    }
}

static void install(size_t i, void *p, size_t n, size_t align, const char *what)
{
    if (!p) { opfail_hdr(); TU_FAIL("%s(%zu) returned NULL", what, n); }
    size_t a = tu_req_align(n);
    if (align > a) a = align;
    if ((uintptr_t)p & (a - 1)) {
        /* jemalloc 5.3 ignores the alignment of zero-byte memalign requests;
           nothing can be stored there, so it is a NOTE for non-strict
           allocators, but a failure for sohamalloc/glibc. */
        static int noted;
        if (n != 0 || tu_strict()) {
            opfail_hdr();
            TU_FAIL("%s(n=%zu) = %p not %zu-aligned", what, n, p, a);
        }
        if (!noted++) tu_info("NOTE %s(align=%zu, n=0) = %p ignores the alignment (quirk)", what, align, p);
    }
    size_t u = T_USABLE(p);
    if (u < n) { opfail_hdr(); TU_FAIL("%s(%zu) = %p: usable %zu < n", what, n, p, u); }
    int32_t c = tu_ranges_insert(&oracle, (int32_t)i, (uintptr_t)p, u);
    if (c >= 0) {
        opfail_hdr();
        TU_FAIL("%s(%zu) = %p [usable %zu] overlaps live block %p [n=%zu usable=%zu] (slot %d)", what, n, p, u,
                slots[c].p, slots[c].n, slots[c].u, (int)c);
    }
    slots[i].p = p;
    slots[i].n = n;
    slots[i].u = u;
    slots[i].seed = tu_mix64(seed0 ^ (opno * 0x9E3779B97F4A7C15ULL) ^ i);
    tu_fill(p, u, slots[i].seed);
}

static void release(size_t i)
{
    verify_slot(i, "free");
    tu_ranges_remove(&oracle, (int32_t)i);
    if (opno & 1) T_FREE_SIZED(slots[i].p, slots[i].n);
    else T_FREE(slots[i].p);
    slots[i].p = NULL;
    cnt_free++;
}

static size_t rand_align(tu_rng *r)
{
    /* 8 B .. 64 KiB mostly; occasionally up to 4 MiB (beyond H: solo path). */
    int lg = (int)tu_rng_range(r, 3, 16);
    if (tu_rng_below(r, 100) == 0) lg = (int)tu_rng_range(r, 17, 22);
    size_t a = (size_t)1 << lg;
    if (a < sizeof(void *)) a = sizeof(void *);
    return a;
}

static void op_alloc(tu_rng *r, size_t i, size_t maxsz)
{
    size_t n = tu_rand_size(r, maxsz);
    uint64_t k = tu_rng_below(r, 100);
    if (k < 55) {
        install(i, T_MALLOC(n), n, 0, "malloc");
        cnt_malloc++;
    } else if (k < 72) {
        void *p;
        if (n && (k & 1) && n % 4 == 0) p = T_CALLOC(n / 4, 4);
        else p = T_CALLOC(1, n);
        if (!p) { opfail_hdr(); TU_FAIL("calloc(%zu) returned NULL", n); }
        size_t z = tu_check_zero(p, n, (size_t)1 << 20);
        if (z != SIZE_MAX) { opfail_hdr(); TU_FAIL("calloc(%zu) = %p: non-zero byte at %zu", n, p, z); }
        install(i, p, n, 0, "calloc");
        cnt_calloc++;
    } else if (k < 78) {
        install(i, T_REALLOC(NULL, n), n, 0, "realloc(NULL)");
        cnt_realloc++;
    } else if (k < 86) {
        size_t a = rand_align(r);
        void *p = NULL;
        int e = T_POSIX_MEMALIGN(&p, a, n);
        if (e) { opfail_hdr(); TU_FAIL("posix_memalign(%zu, %zu) = %d", a, n, e); }
        install(i, p, n, a, "posix_memalign");
        cnt_aligned++;
    } else if (k < 92) {
        size_t a = rand_align(r);
        install(i, T_ALIGNED_ALLOC(a, n), n, a, "aligned_alloc");
        cnt_aligned++;
    } else if (k < 95) {
        size_t a = rand_align(r);
        install(i, T_MEMALIGN(a, n), n, a, "memalign");
        cnt_aligned++;
    } else {
        /* tiny and zero-sized */
        n = (size_t)tu_rng_below(r, 9);
        install(i, T_MALLOC(n), n, 0, "malloc(tiny)");
        cnt_malloc++;
    }
}

static void op_realloc(tu_rng *r, size_t i, size_t maxsz)
{
    slot_t *s = &slots[i];
    verify_slot(i, "pre-realloc");
    uint64_t k = tu_rng_below(r, 100);
    size_t n;
    if (k < 35) n = s->n + (size_t)tu_rng_below(r, s->n / 2 + 64);            /* grow a bit */
    else if (k < 55) n = s->n * 2 + (size_t)tu_rng_below(r, 4096);            /* grow a lot */
    else if (k < 80) n = s->n ? (size_t)tu_rng_below(r, s->n + 1) : 0;        /* shrink */
    else if (k < 82) n = 0;                                                    /* realloc(p, 0) */
    else if (k < 84) n = s->n;                                                 /* same size */
    else n = tu_rand_size(r, maxsz);                                           /* anything */
    if (n > 2 * maxsz) n = 2 * maxsz;

    tu_ranges_remove(&oracle, (int32_t)i);
    void *old = s->p;
    void *q = T_REALLOC(old, n);
    cnt_realloc++;
    if (!q) {
        if (n != 0) { opfail_hdr(); TU_FAIL("realloc(%p, %zu) returned NULL", old, n); }
        /* realloc(p, 0) returned NULL: p was freed (glibc semantics). */
        s->p = NULL;
        return;
    }
    size_t keep = s->n < n ? s->n : n;
    size_t bad = tu_check(q, s->u, keep, s->seed);
    if (bad != SIZE_MAX) {
        opfail_hdr();
        TU_FAIL("realloc(%p [n=%zu], %zu) = %p lost content at offset %zu", old, s->n, n, q, bad);
    }
    s->p = NULL;
    install(i, q, n, 0, "realloc");
}

static void op_impossible(tu_rng *r, size_t i)
{
    /* Requests that must fail cleanly, without disturbing the heap. */
    static const size_t huge[] = { SIZE_MAX, SIZE_MAX - 4095, (size_t)PTRDIFF_MAX + 1 };
    size_t n = huge[tu_rng_below(r, 3)];
    void *p = T_MALLOC(n);
    if (p) { opfail_hdr(); TU_FAIL("malloc(%zu) succeeded", n); }
    p = T_CALLOC(n / 2 + 1, 2 + (size_t)tu_rng_below(r, 3));
    if (p) { opfail_hdr(); TU_FAIL("overflowing calloc succeeded"); }
    if (slots[i].p) {
        void *q = T_REALLOC(slots[i].p, n);
        if (q) { opfail_hdr(); TU_FAIL("realloc(p, %zu) succeeded", n); }
        verify_slot(i, "after failed realloc");
    }
    cnt_fail_ok++;
}

int main(int argc, char **argv)
{
    tu_init("random_ops", argc, argv);
    size_t ops = tu_scaled_min(2000000, 20000);
    nslots = 8192;
    size_t maxsz = sizeof(size_t) == 4 ? (size_t)1 << 20 : (size_t)4 << 20;
    for (int a = 1; a < argc; a++) {
        if (strncmp(argv[a], "--ops=", 6) == 0) ops = strtoull(argv[a] + 6, NULL, 0);
        if (strncmp(argv[a], "--slots=", 8) == 0) nslots = strtoull(argv[a] + 8, NULL, 0);
    }
    seed0 = tu_seed_v;
    tu_info("API %s, seed %llu, %zu ops, %zu slots", T_API_NAME, (unsigned long long)seed0, ops, nslots);

    slots = (slot_t *)tu_os_alloc(nslots * sizeof(slot_t));
    tu_ranges_init(&oracle, nslots);
    tu_rng r;
    tu_rng_seed(&r, seed0);

    size_t sweep_every = ops / 20 + 1;
    double t0 = tu_now();
    for (opno = 0; opno < ops; opno++) {
        size_t i = (size_t)tu_rng_below(&r, nslots);
        uint64_t k = tu_rng_below(&r, 1000);
        if (k == 0) {
            op_impossible(&r, i);
        } else if (!slots[i].p) {
            op_alloc(&r, i, maxsz);
        } else if (k < 450) {
            release(i);
        } else if (k < 850) {
            op_realloc(&r, i, maxsz);
        } else {
            verify_slot(i, "spot check");
        }
        if (opno % sweep_every == sweep_every - 1) {
            for (size_t j = 0; j < nslots; j++)
                if (slots[j].p) verify_slot(j, "periodic sweep");
        }
    }
    for (size_t j = 0; j < nslots; j++)
        if (slots[j].p) release(j);
    TU_CHECK(oracle.live == 0, "oracle still has %zu live ranges", oracle.live);
    tu_info("PASS %.2fs: malloc %lu calloc %lu realloc %lu aligned %lu free %lu impossible %lu", tu_now() - t0,
            cnt_malloc, cnt_calloc, cnt_realloc, cnt_aligned, cnt_free, cnt_fail_ok);
    tu_ranges_destroy(&oracle);
    tu_os_free(slots, nslots * sizeof(slot_t));
    return 0;
}
