/*
 * api_semantics -- the malloc-family contract of docs/IMPL_SPEC_v0.md §2.
 *
 * Checks, against whatever malloc the process uses (LD_PRELOAD):
 *   malloc(0) unique and aligned; free(NULL); realloc(NULL,n) / realloc(p,0) /
 *   shrink / grow / failure-leaves-block-intact; calloc overflow; reallocarray
 *   overflow; posix_memalign EINVAL cases and alignments 8 B..4 MiB;
 *   aligned_alloc / memalign / valloc / pvalloc alignment; malloc_usable_size
 *   >= n and writable; every failure sets ENOMEM; free preserves errno;
 *   free_sized / free_aligned_sized (C23) when exported; allocation from a
 *   constructor and as a new thread's first call; the sohamalloc-only
 *   extensions and glibc compatibility symbols when the allocator is
 *   sohamalloc.
 *
 * Where the C standard leaves behaviour implementation-defined and the spec
 * pins it to glibc's choice (realloc(p,0), memalign with a non-power-of-two
 * alignment), the check is strict for sohamalloc and glibc and reported as a
 * NOTE for other allocators.
 */
#include "common/test_util.h"

#include <stdbool.h>

#ifndef SIZE_MAX
#error SIZE_MAX
#endif

static bool strict;              /* glibc-pinned semantics apply */
static const char *provider = "?";
static int notes;

#define NOTE(...) do { notes++; tu_info("NOTE " __VA_ARGS__); } while (0)

/* POSIX requires ENOMEM on every allocation failure; C does not.  Strict for
   sohamalloc and glibc, a NOTE (once per call site) for other allocators. */
#define EXPECT_ENOMEM(what) do {                                                  \
        int e_ = errno;                                                           \
        if (e_ != ENOMEM) {                                                       \
            static int noted_;                                                    \
            if (strict) TU_FAIL("%s: errno=%d after failure, want ENOMEM", (what), e_); \
            if (!noted_++) NOTE("%s fails without setting errno=ENOMEM (errno=%d)", (what), e_); \
        }                                                                         \
    } while (0)

static void check_aligned(const void *p, size_t a, const char *what, size_t n)
{
    if (((uintptr_t)p & (a - 1)) != 0)
        TU_FAIL("%s(n=%zu): %p is not %zu-aligned", what, n, p, a);
}

/* The block must be usable for n bytes and its usable size >= n, and all
   usable bytes must be writable.  Writes the full usable range. */
static void check_block(void *p, size_t n, const char *what)
{
    TU_CHECK(p != NULL, "%s(%zu) returned NULL", what, n);
    size_t u = T_USABLE(p);
    if (u < n) TU_FAIL("%s(%zu): malloc_usable_size = %zu < requested", what, n, u);
    memset(p, 0xA5, u);
}

/* Which shared object provides symbol `name` in this process. */
static const char *sym_owner(const char *name)
{
    void *s = dlsym(RTLD_DEFAULT, name);
    Dl_info di;
    if (!s || !dladdr(s, &di) || !di.dli_fname) return NULL;
    return di.dli_fname;
}

static bool same_provider_as_malloc(const char *name)
{
    const char *a = sym_owner(name), *m = sym_owner("malloc");
    return a && m && strcmp(a, m) == 0;
}

/* ---------------------------------------------------------------- early init */
/* Allocation before main(): from a high-priority constructor, as an
   LD_PRELOAD'ed allocator sees from real programs. */
static void *ctor_blocks[16];
__attribute__((constructor(101))) static void early_ctor(void)
{
    free(NULL);
    for (int i = 0; i < 16; i++) {
        ctor_blocks[i] = malloc((size_t)i * 37);
        if (ctor_blocks[i]) memset(ctor_blocks[i], i, (size_t)i * 37);
    }
    void *z = malloc(0);
    free(z);
}

static void test_early_init(void)
{
    for (int i = 0; i < 16; i++) {
        TU_CHECK(ctor_blocks[i] != NULL, "malloc(%d) in a constructor returned NULL", i * 37);
        unsigned char *b = (unsigned char *)ctor_blocks[i];
        for (int k = 0; k < i * 37; k++)
            TU_CHECK(b[k] == (unsigned char)i, "constructor block %d corrupted at %d", i, k);
        free(b);
    }
}

/* ---------------------------------------------------------------- malloc(0) */
static void test_malloc_zero(void)
{
    enum { N = 2000 };
    static void *ps[N];
    tu_ranges t;
    tu_ranges_init(&t, N);
        for (int i = 0; i < N; i++) {
        ps[i] = malloc(0);
        TU_CHECK(ps[i] != NULL, "malloc(0) #%d returned NULL", i);
        check_aligned(ps[i], tu_req_align(0), "malloc", 0);
        size_t u = malloc_usable_size(ps[i]);
        if (u) memset(ps[i], 0x5A, u);
        int32_t c = tu_ranges_insert(&t, i, (uintptr_t)ps[i], u ? u : 1);
        if (c >= 0) TU_FAIL("malloc(0) #%d = %p overlaps/duplicates #%d = %p", i, ps[i], c, ps[c]);
    }
    for (int i = 0; i < N; i++) free(ps[i]);
    tu_ranges_destroy(&t);

    /* calloc with a zero factor behaves as malloc(0) on glibc: unique, non-NULL. */
    void *a = calloc(0, 0), *b = calloc(1, 0), *c = calloc(0, 1000);
    TU_CHECK(a && b && c, "calloc(0,0)=%p calloc(1,0)=%p calloc(0,1000)=%p", a, b, c);
    TU_CHECK(a != b && b != c && a != c, "zero-size calloc results not unique");
    free(a); free(b); free(c);
}

/* ---------------------------------------------------------------- free */
static void test_free_null_and_errno(void)
{
    errno = 0;
    free(NULL);
    TU_CHECK(errno == 0, "free(NULL) changed errno to %d", errno);

    static const size_t sizes[] = { 1, 16, 100, 1024, 5000, 70000, 300000, 3u << 20, 40u << 20 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        void *p = malloc(sizes[i]);
        TU_CHECK(p, "malloc(%zu)", sizes[i]);
        memset(p, 1, sizes[i]);
        errno = EILSEQ;
        free(p);
        TU_CHECK(errno == EILSEQ, "free of a %zu-byte block changed errno to %d", sizes[i], errno);
    }
    /* Also from realloc'ed and aligned blocks. */
    void *p = NULL;
    TU_CHECK(posix_memalign(&p, 4096, 10000) == 0, "posix_memalign");
    errno = ERANGE;
    free(p);
    TU_CHECK(errno == ERANGE, "free of an aligned block changed errno to %d", errno);
}

/* ---------------------------------------------------------------- realloc */
static void test_realloc(void)
{
    /* realloc(NULL, n) == malloc(n) */
    for (size_t n = 0; n < 5000; n += 97) {
        void *p = realloc(NULL, n);
        check_block(p, n, "realloc(NULL)");
        check_aligned(p, tu_req_align(n), "realloc(NULL)", n);
        free(p);
    }

    /* realloc(p, 0): glibc (and the spec) free p and return NULL. */
    {
        void *p = malloc(100);
        TU_CHECK(p, "malloc(100)");
        void *q = realloc(p, 0);
        if (q != NULL) {
            if (strict) TU_FAIL("realloc(p, 0) returned %p; spec/glibc require NULL (p freed)", q);
            NOTE("realloc(p,0) returned a non-NULL block (implementation-defined in C17; spec pins glibc's NULL)");
            free(q);
        }
    }

    /* Grow and shrink chains preserve content (pattern over the min size). */
    static const size_t steps[] = { 1, 7, 16, 17, 100, 128, 129, 1000, 1024, 1025, 4096, 8192, 8193,
                                    20000, 65536, 131072, 131073, 163840, 196608, 229376, 262144,
                                    262145, 1u << 20, 5u << 20, 33u << 20, 3u << 20, 300000, 70000,
                                    5000, 200, 16, 1 };
    size_t ns = sizeof steps / sizeof steps[0];
    void *p = malloc(steps[0]);
    TU_CHECK(p, "malloc");
    uint64_t seed = 1;
    tu_fill(p, steps[0], seed);
    for (size_t i = 1; i < ns; i++) {
        size_t oldn = steps[i - 1], n = steps[i];
        void *q = realloc(p, n);
        TU_CHECK(q, "realloc(%zu -> %zu) returned NULL", oldn, n);
        check_aligned(q, tu_req_align(n), "realloc", n);
        size_t bad = tu_check(q, oldn, n < oldn ? n : oldn, seed);
        if (bad != SIZE_MAX)
            TU_FAIL("realloc(%zu -> %zu) lost content at offset %zu", oldn, n, bad);
        TU_CHECK(malloc_usable_size(q) >= n, "usable %zu < %zu after realloc", malloc_usable_size(q), n);
        p = q;
        seed++;
        tu_fill(p, n, seed);
    }
    free(p);

    /* realloc to the same size keeps content. */
    p = malloc(333);
    tu_fill(p, 333, 99);
    p = realloc(p, 333);
    TU_CHECK(p && tu_check(p, 333, 333, 99) == SIZE_MAX, "realloc same size lost content");
    free(p);

    /* realloc of an over-aligned block keeps content. */
    void *a = NULL;
    TU_CHECK(posix_memalign(&a, 1024, 3000) == 0, "posix_memalign");
    tu_fill(a, 3000, 5);
    a = realloc(a, 9000);
    TU_CHECK(a && tu_check(a, 3000, 3000, 5) == SIZE_MAX, "realloc of aligned block lost content");
    free(a);

    /* A failed realloc returns NULL, sets ENOMEM and leaves the block intact. */
    static const size_t bad_sizes[] = { SIZE_MAX, SIZE_MAX - 15, SIZE_MAX - 4095, (size_t)PTRDIFF_MAX + 1,
                                        SIZE_MAX / 2 + 4096 };
    static const size_t keep[] = { 10, 4000, 100000, 3u << 20 };
    for (size_t k = 0; k < sizeof keep / sizeof keep[0]; k++) {
        p = malloc(keep[k]);
        TU_CHECK(p, "malloc");
        tu_fill(p, keep[k], 77 + k);
        for (size_t i = 0; i < sizeof bad_sizes / sizeof bad_sizes[0]; i++) {
            errno = 0;
            void *q = realloc(p, bad_sizes[i]);
            if (q != NULL) TU_FAIL("realloc(%zu-byte block, %zu) succeeded?!", keep[k], bad_sizes[i]);
            EXPECT_ENOMEM("realloc(p, huge)");
            size_t bad = tu_check(p, keep[k], keep[k], 77 + k);
            if (bad != SIZE_MAX) TU_FAIL("failed realloc damaged the original block at %zu", bad);
        }
        free(p);
    }
}

/* ---------------------------------------------------------------- calloc */
static void test_calloc(void)
{
    struct { size_t c, n; } ov[] = {
        { SIZE_MAX, SIZE_MAX }, { SIZE_MAX / 2 + 1, 2 }, { 2, SIZE_MAX / 2 + 1 },
        { (SIZE_MAX >> 4) + 1, 16 }, { 65537, (SIZE_MAX / 65536) }, { SIZE_MAX, 1 + 1 },
        { (size_t)1 << (sizeof(size_t) * 4), (size_t)1 << (sizeof(size_t) * 4) },
    };
    for (size_t i = 0; i < sizeof ov / sizeof ov[0]; i++) {
        errno = 0;
        void *p = calloc(ov[i].c, ov[i].n);
        if (p) TU_FAIL("calloc(%zu, %zu) overflow returned %p", ov[i].c, ov[i].n, p);
        EXPECT_ENOMEM("calloc(overflowing count*size)");
    }
    /* Too large without overflow. */
    errno = 0;
    TU_CHECK(calloc(1, SIZE_MAX - 100) == NULL, "calloc(1, SIZE_MAX-100)");
    EXPECT_ENOMEM("calloc(1, SIZE_MAX-100)");

    /* calloc memory is zero, including when it reuses dirty blocks. */
    static const size_t sizes[] = { 1, 8, 16, 24, 100, 512, 1000, 1024, 4000, 8192, 10000, 65536,
                                    100000, 196608, 262144, 400000, 2u << 20 };
    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            enum { K = 8 };
            void *d[K];
            for (int k = 0; k < K; k++) {
                d[k] = malloc(sizes[i]);
                TU_CHECK(d[k], "malloc");
                memset(d[k], 0xFF, malloc_usable_size(d[k]));
            }
            for (int k = 0; k < K; k++) free(d[k]);
            for (int k = 0; k < K; k++) {
                d[k] = calloc(1, sizes[i]);
                TU_CHECK(d[k], "calloc(1, %zu)", sizes[i]);
                size_t z = tu_check_zero(d[k], sizes[i], (size_t)4 << 20);
                if (z != SIZE_MAX) TU_FAIL("calloc(1, %zu) round %d: non-zero byte at %zu", sizes[i], round, z);
                check_aligned(d[k], tu_req_align(sizes[i]), "calloc", sizes[i]);
            }
            for (int k = 0; k < K; k++) free(d[k]);
            /* count*size split */
            void *q = calloc(sizes[i], 3);
            TU_CHECK(q && tu_check_zero(q, sizes[i] * 3, (size_t)4 << 20) == SIZE_MAX, "calloc(%zu,3) not zero", sizes[i]);
            free(q);
        }
    }
    /* Big calloc is zero and usable. */
    size_t big = (size_t)256 << 20;
    if (sizeof(size_t) == 4) big = (size_t)64 << 20;
    void *q = calloc(1, big);
    TU_CHECK(q, "calloc(1, %zu)", big);
    TU_CHECK(tu_check_zero(q, big, 1 << 20) == SIZE_MAX, "big calloc not zero");
    free(q);
}

/* ---------------------------------------------------------------- reallocarray */
static void test_reallocarray(void)
{
    errno = 0;
    void *p = reallocarray(NULL, SIZE_MAX / 2, 3);
    TU_CHECK(p == NULL, "reallocarray(NULL, SIZE_MAX/2, 3) = %p", p);
    EXPECT_ENOMEM("reallocarray(NULL, overflow)");
    p = reallocarray(NULL, 10, 20);
    check_block(p, 200, "reallocarray(NULL,10,20)");
    tu_fill(p, 200, 3);
    errno = 0;
    void *q = reallocarray(p, (size_t)1 << (sizeof(size_t) * 4), (size_t)1 << (sizeof(size_t) * 4));
    TU_CHECK(q == NULL, "reallocarray overflow returned %p", q);
    EXPECT_ENOMEM("reallocarray(p, overflow)");
    TU_CHECK(tu_check(p, 200, 200, 3) == SIZE_MAX, "reallocarray overflow damaged block");
    q = reallocarray(p, 100, 30);
    TU_CHECK(q && tu_check(q, 200, 200, 3) == SIZE_MAX, "reallocarray grow lost content");
    TU_CHECK(malloc_usable_size(q) >= 3000, "reallocarray usable");
    free(q);
}

/* ---------------------------------------------------------------- aligned */
static void test_posix_memalign(void)
{
    /* EINVAL: not a power of two, or not a multiple of sizeof(void*). */
    size_t badal[] = { 0, 1, 2, 3, sizeof(void *) / 2, sizeof(void *) + 1, 3 * sizeof(void *), 24, 48, 96,
                       4095, 4097, 12288, (size_t)1 << (sizeof(size_t) * 8 - 1) | 8 };
    for (size_t i = 0; i < sizeof badal / sizeof badal[0]; i++) {
        size_t a = badal[i];
        bool pow2 = a && !(a & (a - 1));
        if (pow2 && a % sizeof(void *) == 0) continue;     /* valid on this ABI */
        void *sentinel = (void *)(uintptr_t)0xdeadbee0u, *out = sentinel;
        int r = posix_memalign(&out, a, 64);
        if (r != EINVAL) TU_FAIL("posix_memalign(align=%zu) returned %d, want EINVAL", a, r);
        TU_CHECK(out == sentinel, "posix_memalign(align=%zu) modified *memptr on failure", a);
    }
    /* ENOMEM for impossible sizes; *memptr unmodified. */
    {
        void *sentinel = (void *)(uintptr_t)0xdeadbee0u, *out = sentinel;
        int r = posix_memalign(&out, 64, SIZE_MAX - 4096);
        TU_CHECK(r == ENOMEM, "posix_memalign(64, huge) returned %d want ENOMEM", r);
        TU_CHECK(out == sentinel, "posix_memalign(huge) modified *memptr on failure");
    }
    /* Alignments sizeof(void*) .. 4 MiB with assorted sizes. */
    for (size_t a = sizeof(void *); a <= ((size_t)4 << 20); a <<= 1) {
        size_t ns[] = { 0, 1, a / 2 + 1, a - 1, a, a + 1, 3 * a, 100, 5000, 70000 };
        void *keep[sizeof ns / sizeof ns[0]];
        for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
            void *p = NULL;
            int r = posix_memalign(&p, a, ns[i]);
            TU_CHECK(r == 0, "posix_memalign(align=%zu, n=%zu) returned %d", a, ns[i], r);
            check_aligned(p, a, "posix_memalign", ns[i]);
            check_aligned(p, tu_req_align(ns[i]), "posix_memalign(min)", ns[i]);
            check_block(p, ns[i], "posix_memalign");
            keep[i] = p;
        }
        for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) free(keep[i]);
    }
}

static void test_aligned_alloc_memalign(void)
{
    for (size_t a = 1; a <= ((size_t)4 << 20); a <<= 1) {
        size_t ns[] = { 1, a, 2 * a, a + 16, 100, 4096 };
        for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
            void *p = aligned_alloc(a, ns[i]);
            TU_CHECK(p, "aligned_alloc(%zu, %zu) returned NULL (errno %d)", a, ns[i], errno);
            check_aligned(p, a, "aligned_alloc", ns[i]);
            check_block(p, ns[i], "aligned_alloc");
            free(p);
            p = memalign(a, ns[i]);
            TU_CHECK(p, "memalign(%zu, %zu) returned NULL", a, ns[i]);
            check_aligned(p, a, "memalign", ns[i]);
            check_block(p, ns[i], "memalign");
            free(p);
        }
    }
    /* Failure: ENOMEM. */
    errno = 0;
    TU_CHECK(aligned_alloc(64, SIZE_MAX - 8192) == NULL, "aligned_alloc(huge)");
    EXPECT_ENOMEM("aligned_alloc(64, huge)");
    errno = 0;
    TU_CHECK(memalign(4096, SIZE_MAX - 8192) == NULL, "memalign(huge)");
    EXPECT_ENOMEM("memalign(4096, huge)");

    /* memalign with a non-power-of-two alignment: glibc rounds up. */
    size_t odd[] = { 3, 24, 48, 100, 1000, 3000, 12288 };
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) {
        size_t want = 1;
        while (want < odd[i]) want <<= 1;
        void *p = memalign(odd[i], 200);
        if (!p) {
            if (strict) TU_FAIL("memalign(%zu, 200) returned NULL; glibc/spec round the alignment up", odd[i]);
            NOTE("memalign(non-power-of-two %zu) rejected (errno %d); spec requires rounding up", odd[i], errno);
            continue;
        }
        if (((uintptr_t)p & (want - 1)) != 0) {
            if (strict) TU_FAIL("memalign(%zu) = %p, not aligned to %zu", odd[i], p, want);
            NOTE("memalign(%zu) = %p not aligned to %zu", odd[i], p, want);
        }
        check_block(p, 200, "memalign(odd)");
        free(p);
    }
}

static void test_valloc_pvalloc(void)
{
    size_t pg = tu_page_size();
    if (same_provider_as_malloc("valloc")) {
        void *(*vf)(size_t) = (void *(*)(size_t))dlsym(RTLD_DEFAULT, "valloc");
        size_t ns[] = { 0, 1, 100, pg - 1, pg, pg + 1, 10 * pg + 3, 1u << 20 };
        for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
            void *p = vf(ns[i]);
            TU_CHECK(p, "valloc(%zu)", ns[i]);
            check_aligned(p, pg, "valloc", ns[i]);
            check_block(p, ns[i], "valloc");
            free(p);
        }
        errno = 0;
        TU_CHECK(vf(SIZE_MAX - 2 * pg) == NULL, "valloc(huge)");
        EXPECT_ENOMEM("valloc(huge)");
    } else {
        NOTE("valloc is not provided by the malloc provider (%s); mixing allocators, not tested", provider);
    }
    if (same_provider_as_malloc("pvalloc")) {
        void *(*pf)(size_t) = (void *(*)(size_t))dlsym(RTLD_DEFAULT, "pvalloc");
        size_t ns[] = { 1, 100, pg - 1, pg, pg + 1, 10 * pg + 3 };
        for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
            void *p = pf(ns[i]);
            TU_CHECK(p, "pvalloc(%zu)", ns[i]);
            check_aligned(p, pg, "pvalloc", ns[i]);
            size_t want = (ns[i] + pg - 1) / pg * pg;
            check_block(p, want, "pvalloc(rounded)");
            free(p);
        }
        void *p = pf(0);
        if (p) {
            check_aligned(p, pg, "pvalloc", 0);
            free(p);
        }
    } else {
        NOTE("pvalloc is not provided by the malloc provider (%s); mixing allocators, not tested", provider);
    }
}

/* ---------------------------------------------------------------- usable size */
static void test_usable_size(void)
{
    TU_CHECK(malloc_usable_size(NULL) == 0, "malloc_usable_size(NULL) = %zu", malloc_usable_size(NULL));
    /* Every size 0..20000 plus boundaries: usable >= n and the whole usable
       range is writable without damaging neighbours. */
    enum { LIVE = 64 };
    void *ring[LIVE] = { 0 };
    size_t ringu[LIVE] = { 0 };
    uint64_t ringseed[LIVE] = { 0 };
    for (size_t n = 0; n <= 20000; n += (n < 2048 ? 1 : 13)) {
        size_t slot = n % LIVE;
        if (ring[slot]) {
            size_t bad = tu_check(ring[slot], ringu[slot], ringu[slot], ringseed[slot]);
            if (bad != SIZE_MAX) TU_FAIL("block of usable %zu corrupted at %zu (neighbour wrote past its usable size?)", ringu[slot], bad);
            free(ring[slot]);
        }
        void *p = malloc(n);
        TU_CHECK(p, "malloc(%zu)", n);
        size_t u = malloc_usable_size(p);
        TU_CHECK(u >= n, "malloc_usable_size(malloc(%zu)) = %zu", n, u);
        ring[slot] = p;
        ringu[slot] = u;
        ringseed[slot] = n * 31 + 7;
        tu_fill(p, u, ringseed[slot]);
    }
    for (int i = 0; i < LIVE; i++) {
        if (!ring[i]) continue;
        TU_CHECK(tu_check(ring[i], ringu[i], ringu[i], ringseed[i]) == SIZE_MAX, "final usable check");
        free(ring[i]);
    }
}

/* ---------------------------------------------------------------- ENOMEM */
static void test_enomem(void)
{
    size_t bad[] = { SIZE_MAX, SIZE_MAX - 1, SIZE_MAX - 15, SIZE_MAX - 16, SIZE_MAX - 4096,
                     SIZE_MAX - (2u << 20), (size_t)PTRDIFF_MAX + 1, (size_t)PTRDIFF_MAX };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        errno = 0;
        void *p = malloc(bad[i]);
        if (p) TU_FAIL("malloc(%zu) succeeded", bad[i]);
        EXPECT_ENOMEM("malloc(huge)");
    }
    /* Larger than RAM + swap: with overcommit heuristic mode 0 this must fail
       (plan §8.4 conformance).  Only checked in mode 0 and on 64-bit. */
    char buf[16] = { 0 };
    int fd = open("/proc/sys/vm/overcommit_memory", O_RDONLY);
    if (fd >= 0 && read(fd, buf, sizeof buf - 1) > 0 && buf[0] == '0' && sizeof(size_t) == 8) {
        long pages = sysconf(_SC_PHYS_PAGES);
        size_t ram = (size_t)pages * tu_page_size();
        /* Swap total from /proc/meminfo would need parsing; 2*RAM + 64 GiB is
           comfortably above RAM + swap on any test machine. */
        size_t huge = 2 * ram + ((size_t)64 << 30);
        errno = 0;
        void *p = malloc(huge);
        if (p) {
            if (tu_is_soham()) TU_FAIL("malloc(%zu) (> RAM+swap) succeeded under overcommit mode 0", huge);
            NOTE("malloc(%zu) (> RAM+swap) succeeded under overcommit mode 0", huge);
            free(p);
        } else {
            EXPECT_ENOMEM("malloc(> RAM+swap)");
        }
    }
    if (fd >= 0) close(fd);
}

/* ---------------------------------------------------------------- C23 sized free */
static void test_free_sized(void)
{
    void (*fs)(void *, size_t) = (void (*)(void *, size_t))dlsym(RTLD_DEFAULT, "free_sized");
    void (*fas)(void *, size_t, size_t) =
        (void (*)(void *, size_t, size_t))dlsym(RTLD_DEFAULT, "free_aligned_sized");
    if (!fs && !fas) {
        if (tu_is_soham()) TU_FAIL("sohamalloc must export free_sized and free_aligned_sized");
        NOTE("free_sized/free_aligned_sized not exported (C23); skipped");
        return;
    }
    if (fs) {
        if (!same_provider_as_malloc("free_sized")) {
            NOTE("free_sized comes from a different object than malloc; skipped");
        } else {
            fs(NULL, 0);
            fs(NULL, 123);
            for (size_t n = 1; n < 300000; n = n * 3 + 1) {
                void *p = malloc(n);
                TU_CHECK(p, "malloc");
                memset(p, 1, n);
                errno = EDOM;
                fs(p, n);
                TU_CHECK(errno == EDOM, "free_sized changed errno");
                p = calloc(n, 2);
                fs(p, 2 * n);
                p = malloc(n);
                p = realloc(p, n + 100);
                fs(p, n + 100);
            }
        }
    }
    if (fas) {
        if (!same_provider_as_malloc("free_aligned_sized")) {
            NOTE("free_aligned_sized comes from a different object than malloc; skipped");
        } else {
            fas(NULL, 64, 0);
            for (size_t a = 1; a <= ((size_t)1 << 20); a <<= 1) {
                for (size_t n = a; n <= 4 * a && n < ((size_t)4 << 20); n += a) {
                    void *p = aligned_alloc(a, n);
                    TU_CHECK(p, "aligned_alloc(%zu,%zu)", a, n);
                    memset(p, 2, n);
                    fas(p, a, n);
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- threads */
static void *g_handoff;
static void *first_call_malloc0(void *arg)
{
    (void)arg;
    void *p = malloc(0);                       /* very first allocator call of this thread */
    TU_CHECK(p, "malloc(0) as a thread's first call");
    check_aligned(p, tu_req_align(0), "malloc(0) first call", 0);
    free(p);
    return NULL;
}
static void *first_call_free(void *arg)
{
    (void)arg;
    TU_CHECK(tu_check(g_handoff, 5000, 5000, 42) == SIZE_MAX, "handoff block corrupted");
    free(g_handoff);                           /* first call is a free of a foreign-thread block */
    void *p = malloc(10);
    free(p);
    return NULL;
}
static void *first_call_free_null(void *arg)
{
    (void)arg;
    free(NULL);
    void *p = calloc(3, 5);
    TU_CHECK(p && tu_check_zero(p, 15, 64) == SIZE_MAX, "calloc after free(NULL) first call");
    free(p);
    return NULL;
}

static void test_thread_first_calls(void)
{
    for (int i = 0; i < 20; i++) {
        tu_thread_join(tu_thread_start(first_call_malloc0, NULL));
        g_handoff = malloc(5000);
        tu_fill(g_handoff, 5000, 42);
        tu_thread_join(tu_thread_start(first_call_free, NULL));
        tu_thread_join(tu_thread_start(first_call_free_null, NULL));
    }
}

/* ---------------------------------------------------------------- sohamalloc */
static void test_soham_extensions(void)
{
    if (!tu_is_soham()) return;
    const char *(*ver)(void) = (const char *(*)(void))tu_sym("soham_version");
    const char *v = ver();
    TU_CHECK(v && v[0] >= '0' && v[0] <= '9', "soham_version() = %s", v ? v : "(null)");
    tu_info("sohamalloc version %s", v);

    void *(*mal)(size_t, size_t *) = (void *(*)(size_t, size_t *))tu_sym("soham_malloc_at_least");
    TU_CHECK(mal, "soham_malloc_at_least not exported");
    for (size_t n = 0; n < 1000000; n = n * 2 + 3) {
        size_t u = 0;
        void *p = mal(n, &u);
        TU_CHECK(p, "soham_malloc_at_least(%zu)", n);
        TU_CHECK(u >= n, "soham_malloc_at_least(%zu) usable %zu", n, u);
        TU_CHECK(u == malloc_usable_size(p), "at_least usable %zu != malloc_usable_size %zu", u, malloc_usable_size(p));
        memset(p, 3, u);
        free(p);
    }

    /* Compatibility functions must come from the same object as malloc. */
    static const char *const compat[] = { "malloc_trim", "mallopt", "malloc_stats", "mallinfo2",
                                          "__libc_malloc", "__libc_free", "__libc_calloc", "__libc_realloc",
                                          "__libc_memalign", "__libc_valloc", "__libc_pvalloc",
                                          "free_sized", "free_aligned_sized", "reallocarray", "valloc",
                                          "pvalloc", "malloc_usable_size", "aligned_alloc", "memalign" };
    for (size_t i = 0; i < sizeof compat / sizeof compat[0]; i++) {
        if (!same_provider_as_malloc(compat[i]))
            TU_FAIL("%s is not exported by the sohamalloc object (owner %s)", compat[i],
                    sym_owner(compat[i]) ? sym_owner(compat[i]) : "none");
    }
    int (*trim)(size_t) = (int (*)(size_t))tu_sym("malloc_trim");
    TU_CHECK(trim(0) == 1, "malloc_trim must return 1");
    int (*mo)(int, int) = (int (*)(int, int))tu_sym("mallopt");
    TU_CHECK(mo(M_MMAP_THRESHOLD, 1 << 20) == 1, "mallopt must return 1");
    void *(*lm)(size_t) = (void *(*)(size_t))tu_sym("__libc_malloc");
    void (*lf)(void *) = (void (*)(void *))tu_sym("__libc_free");
    void *p = lm(100);
    TU_CHECK(p && malloc_usable_size(p) >= 100, "__libc_malloc");
    free(p);
    p = malloc(100);
    lf(p);
}

int main(int argc, char **argv)
{
    tu_init("api_semantics", argc, argv);
    provider = sym_owner("malloc");
    if (!provider) provider = "?";
    strict = tu_is_soham() || strstr(provider, "libc.so") != NULL;
    tu_info("malloc provided by %s (%s semantics)", provider, strict ? "strict glibc" : "C standard");

    test_early_init();
    test_malloc_zero();
    test_free_null_and_errno();
    test_realloc();
    test_calloc();
    test_reallocarray();
    test_posix_memalign();
    test_aligned_alloc_memalign();
    test_valloc_pvalloc();
    test_usable_size();
    test_enomem();
    test_free_sized();
    test_thread_first_calls();
    test_soham_extensions();

    tu_info("PASS (%d notes)", notes);
    return 0;
}
