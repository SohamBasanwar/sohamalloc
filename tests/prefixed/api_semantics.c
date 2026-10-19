/*
 * prefixed/api_semantics -- the docs/IMPL_SPEC_v0.md §2 contract checked
 * through the soham_* entry points directly (the prefixed library, built
 * with or without a sanitizer, or the libc shim).  A subset of
 * tests/api_semantics.c without the parts that need the override build
 * (constructors, dlsym provenance, valloc/pvalloc, glibc aliases), plus the
 * soham-only entry points: soham_free_sized, soham_free_aligned_sized,
 * soham_malloc_at_least, soham_version.  Semantics are always strict: the
 * prefixed library and the shim both promise the glibc behaviour.
 */
#ifndef TEST_PREFIXED
#error "build with -DTEST_PREFIXED=1 (tests/run_tests.sh --prefixed)"
#endif
#include "../common/test_util.h"

#define EXPECT_ENOMEM(what) \
    do { if (errno != ENOMEM) TU_FAIL("%s: errno=%d after failure, want ENOMEM", (what), errno); } while (0)

static void check_block(void *p, size_t n, size_t align, const char *what)
{
    if (!p) TU_FAIL("%s(%zu) returned NULL (errno %d)", what, n, errno);
    size_t a = align > tu_req_align(n) ? align : tu_req_align(n);
    if ((uintptr_t)p & (a - 1)) TU_FAIL("%s(%zu) = %p not %zu-aligned", what, n, p, a);
    size_t u = soham_usable_size(p);
    if (u < n) TU_FAIL("%s(%zu): usable %zu < n", what, n, u);
    if (u) memset(tu_launder(p), 0xA5, u);
}

static void test_malloc_zero(void)
{
    enum { N = 2000 };
    void **ps = (void **)tu_os_alloc(N * sizeof(void *));
    tu_ranges t;
    tu_ranges_init(&t, N);
    for (int i = 0; i < N; i++) {
        ps[i] = soham_malloc(0);
        check_block(ps[i], 0, 0, "soham_malloc");
        size_t u = soham_usable_size(ps[i]);
        int32_t c = tu_ranges_insert(&t, i, (uintptr_t)ps[i], u ? u : 1);
        if (c >= 0) TU_FAIL("soham_malloc(0) #%d = %p duplicates #%d", i, ps[i], c);
    }
    for (int i = 0; i < N; i++) soham_free(ps[i]);
    tu_ranges_destroy(&t);
    tu_os_free(ps, N * sizeof(void *));
    void *a = soham_calloc(0, 0), *b = soham_calloc(1, 0), *c = soham_calloc(0, 99);
    TU_CHECK(a && b && c && a != b && b != c && a != c, "zero-size calloc results must be unique non-NULL");
    soham_free(a); soham_free(b); soham_free(c);
}

static void test_free(void)
{
    errno = 0;
    soham_free(NULL);
    soham_free_sized(NULL, 0);
    soham_free_aligned_sized(NULL, 64, 0);
    TU_CHECK(errno == 0, "free(NULL) changed errno to %d", errno);
    static const size_t sizes[] = { 1, 16, 100, 1024, 5000, 70000, 300000, 3u << 20, 40u << 20 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t n = sizes[i];
        void *p = soham_malloc(n);
        check_block(p, n, 0, "soham_malloc");
        errno = EILSEQ;
        soham_free(p);
        TU_CHECK(errno == EILSEQ, "free of %zu bytes changed errno to %d", n, errno);
        p = soham_malloc(n);
        check_block(p, n, 0, "soham_malloc");
        errno = EDOM;
        soham_free_sized(p, n);
        TU_CHECK(errno == EDOM, "free_sized changed errno");
        p = soham_calloc(n, 1);
        check_block(p, n, 0, "soham_calloc");
        soham_free_sized(p, n);
        for (size_t a = 16; a <= ((size_t)1 << 20); a <<= 2) {
            size_t m = (n + a - 1) / a * a;
            p = soham_aligned_alloc(a, m);
            check_block(p, m, a, "soham_aligned_alloc");
            soham_free_aligned_sized(p, a, m);
        }
    }
}

static void test_realloc(void)
{
    for (size_t n = 0; n < 5000; n += 97) {
        void *p = soham_realloc(NULL, n);
        check_block(p, n, 0, "soham_realloc(NULL)");
        soham_free(p);
    }
    void *p = soham_malloc(100);
    TU_CHECK(soham_realloc(p, 0) == NULL, "soham_realloc(p, 0) must free p and return NULL");

    static const size_t steps[] = { 1, 7, 16, 17, 100, 128, 129, 1000, 1024, 1025, 4096, 8193, 20000, 65536,
                                    131073, 163840, 196608, 229376, 262144, 262145, 1u << 20, 5u << 20,
                                    33u << 20, 3u << 20, 300000, 70000, 5000, 200, 16, 1 };
    size_t ns = sizeof steps / sizeof steps[0];
    p = soham_malloc(steps[0]);
    tu_fill(p, steps[0], 1);
    for (size_t i = 1; i < ns; i++) {
        size_t o = steps[i - 1], n = steps[i];
        void *q = soham_realloc(p, n);
        TU_CHECK(q, "soham_realloc(%zu -> %zu) returned NULL", o, n);
        TU_CHECK(((uintptr_t)q & (tu_req_align(n) - 1)) == 0, "realloc result misaligned");
        size_t bad = tu_check(q, o, n < o ? n : o, i);
        if (bad != SIZE_MAX) TU_FAIL("soham_realloc(%zu -> %zu) lost content at %zu", o, n, bad);
        TU_CHECK(soham_usable_size(q) >= n, "usable after realloc");
        p = q;
        tu_fill(p, n, i + 1);
    }
    soham_free(p);

    /* A failed realloc leaves the block intact and sets ENOMEM. */
    static const size_t bad_sizes[] = { SIZE_MAX, SIZE_MAX - 15, (size_t)PTRDIFF_MAX + 1, SIZE_MAX / 2 + 4096 };
    static const size_t keep[] = { 10, 4000, 100000, 3u << 20 };
    for (size_t k = 0; k < sizeof keep / sizeof keep[0]; k++) {
        p = soham_malloc(keep[k]);
        tu_fill(p, keep[k], 77 + k);
        for (size_t i = 0; i < sizeof bad_sizes / sizeof bad_sizes[0]; i++) {
            errno = 0;
            TU_CHECK(soham_realloc(p, bad_sizes[i]) == NULL, "realloc(%zu) succeeded", bad_sizes[i]);
            EXPECT_ENOMEM("soham_realloc(p, huge)");
            TU_CHECK(tu_check(p, keep[k], keep[k], 77 + k) == SIZE_MAX, "failed realloc damaged the block");
        }
        soham_free(p);
    }
    /* reallocarray */
    errno = 0;
    TU_CHECK(soham_reallocarray(NULL, SIZE_MAX / 2, 3) == NULL, "reallocarray overflow");
    EXPECT_ENOMEM("soham_reallocarray(overflow)");
    p = soham_reallocarray(NULL, 10, 20);
    check_block(p, 200, 0, "soham_reallocarray");
    tu_fill(p, 200, 3);
    void *q = soham_reallocarray(p, 100, 30);
    TU_CHECK(q && tu_check(q, 200, 200, 3) == SIZE_MAX, "reallocarray grow");
    soham_free(q);
}

static void test_calloc(void)
{
    struct { size_t c, n; } ov[] = { { SIZE_MAX, SIZE_MAX }, { SIZE_MAX / 2 + 1, 2 }, { (SIZE_MAX >> 4) + 1, 16 },
                                     { (size_t)1 << (sizeof(size_t) * 4), (size_t)1 << (sizeof(size_t) * 4) } };
    for (size_t i = 0; i < sizeof ov / sizeof ov[0]; i++) {
        errno = 0;
        TU_CHECK(soham_calloc(ov[i].c, ov[i].n) == NULL, "calloc(%zu, %zu) overflow succeeded", ov[i].c, ov[i].n);
        EXPECT_ENOMEM("soham_calloc(overflow)");
    }
    static const size_t sizes[] = { 1, 24, 100, 1000, 4000, 10000, 65536, 100000, 262144, 400000, 2u << 20, 40u << 20 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        void *d[6];
        for (int k = 0; k < 6; k++) {
            d[k] = soham_malloc(sizes[i]);
            check_block(d[k], sizes[i], 0, "soham_malloc");      /* dirties the usable range */
        }
        for (int k = 0; k < 6; k++) soham_free(d[k]);
        for (int k = 0; k < 6; k++) {
            d[k] = soham_calloc(1, sizes[i]);
            TU_CHECK(d[k], "soham_calloc(1, %zu)", sizes[i]);
            size_t z = tu_check_zero(d[k], sizes[i], (size_t)4 << 20);
            if (z != SIZE_MAX) TU_FAIL("soham_calloc(1, %zu): byte %zu non-zero", sizes[i], z);
        }
        for (int k = 0; k < 6; k++) soham_free(d[k]);
    }
}

static void test_aligned(void)
{
    size_t badal[] = { 0, 1, 2, 3, sizeof(void *) + 1, 24, 48, 4095, 12288 };
    for (size_t i = 0; i < sizeof badal / sizeof badal[0]; i++) {
        size_t a = badal[i];
        if (a && !(a & (a - 1)) && a % sizeof(void *) == 0) continue;
        void *s = (void *)(uintptr_t)0xdeadbee0u, *out = s;
        TU_CHECK(soham_posix_memalign(&out, a, 64) == EINVAL, "posix_memalign(align=%zu) != EINVAL", a);
        TU_CHECK(out == s, "posix_memalign modified *out on failure");
    }
    void *s = (void *)(uintptr_t)0xdeadbee0u, *out = s;
    TU_CHECK(soham_posix_memalign(&out, 64, SIZE_MAX - 4096) == ENOMEM && out == s, "posix_memalign(huge)");
    for (size_t a = sizeof(void *); a <= ((size_t)4 << 20); a <<= 1) {
        size_t ns[] = { 0, 1, a / 2 + 1, a - 1, a, a + 1, 3 * a, 100, 5000, 70000 };
        for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
            void *p = NULL;
            TU_CHECK(soham_posix_memalign(&p, a, ns[i]) == 0, "posix_memalign(%zu, %zu)", a, ns[i]);
            check_block(p, ns[i], a, "soham_posix_memalign");
            soham_free(p);
            p = soham_aligned_alloc(a, ns[i]);
            check_block(p, ns[i], a, "soham_aligned_alloc");
            soham_free(p);
            p = soham_memalign(a, ns[i]);
            check_block(p, ns[i], a, "soham_memalign");
            soham_free(p);
        }
    }
    size_t odd[] = { 3, 24, 48, 100, 1000, 3000, 12288 };
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) {
        size_t want = 1;
        while (want < odd[i]) want <<= 1;
        void *p = soham_memalign(odd[i], 200);
        check_block(p, 200, want, "soham_memalign(non-power-of-two, rounded up)");
        soham_free(p);
    }
    errno = 0;
    TU_CHECK(soham_aligned_alloc(64, SIZE_MAX - 8192) == NULL, "aligned_alloc(huge)");
    EXPECT_ENOMEM("soham_aligned_alloc(huge)");
    errno = 0;
    TU_CHECK(soham_memalign(4096, SIZE_MAX - 8192) == NULL, "memalign(huge)");
    EXPECT_ENOMEM("soham_memalign(huge)");
}

static void test_usable_and_enomem(void)
{
    TU_CHECK(soham_usable_size(NULL) == 0, "soham_usable_size(NULL)");
    for (size_t n = 0; n <= 20000; n += (n < 2048 ? 1 : 13)) {
        void *p = soham_malloc(n);
        check_block(p, n, 0, "soham_malloc");
        soham_free(p);
        size_t u = 0;
        p = soham_malloc_at_least(n, &u);
        TU_CHECK(p && u >= n && u == soham_usable_size(p), "soham_malloc_at_least(%zu): usable %zu", n, u);
        memset(tu_launder(p), 3, u);
        soham_free(p);
    }
    size_t bad[] = { SIZE_MAX, SIZE_MAX - 16, SIZE_MAX - 4096, (size_t)PTRDIFF_MAX + 1,
                     /* PTRDIFF_MAX itself (2 GiB - 1) can succeed on 32-bit with a 4 GiB address space */
                     sizeof(size_t) == 8 ? (size_t)PTRDIFF_MAX : SIZE_MAX - 4097 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        errno = 0;
        TU_CHECK(soham_malloc(bad[i]) == NULL, "soham_malloc(%zu) succeeded", bad[i]);
        EXPECT_ENOMEM("soham_malloc(huge)");
        errno = 0;
        TU_CHECK(soham_malloc_at_least(bad[i], NULL) == NULL, "soham_malloc_at_least(huge)");
        EXPECT_ENOMEM("soham_malloc_at_least(huge)");
    }
    const char *v = soham_version();
    TU_CHECK(v && *v, "soham_version()");
    tu_info("soham_version() = %s", v);
}

static void *g_handoff;
static void *first_malloc0(void *a) { (void)a; void *p = soham_malloc(0); check_block(p, 0, 0, "first soham_malloc(0)"); soham_free(p); return NULL; }
static void *first_free(void *a)
{
    (void)a;
    TU_CHECK(tu_check(g_handoff, 5000, 5000, 42) == SIZE_MAX, "handoff block corrupted");
    soham_free(g_handoff);
    void *p = soham_malloc(10);
    soham_free(p);
    return NULL;
}

static void test_threads(void)
{
    for (int i = 0; i < 20; i++) {
        tu_thread_join(tu_thread_start(first_malloc0, NULL));
        g_handoff = soham_malloc(5000);
        tu_fill(g_handoff, 5000, 42);
        tu_thread_join(tu_thread_start(first_free, NULL));
    }
}

int main(int argc, char **argv)
{
    tu_init("api_semantics", argc, argv);
    tu_info("API %s, %s", T_API_NAME, tu_is_soham() ? "sohamalloc" : "libc shim");
    test_malloc_zero();
    test_free();
    test_realloc();
    test_calloc();
    test_aligned();
    test_usable_and_enomem();
    test_threads();
    tu_info("PASS");
    return 0;
}
