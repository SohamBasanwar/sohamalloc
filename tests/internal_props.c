/*
 * internal_props -- property tests of sohamalloc's size-class tables, page
 * geometry and free-list link encoding (docs/IMPL_SPEC_v0.md §3-§5.2,
 * plan §5.3 and §8.4 "Unit tests"), through src/internal_test.h.
 *
 * sohamalloc-only: the internals are found with dlsym() when the allocator
 * is LD_PRELOAD'ed (or linked with -rdynamic), and called directly in the
 * prefixed build (tests/prefixed/internal_props.c).  Any other allocator,
 * and the libc shim, SKIP.
 *
 * Properties:
 *   - class sizes are positive, 16-byte multiples and strictly increasing;
 *     the last class is SOHAM_INTERNAL_MAX_CLASS_SIZE;
 *   - size -> class is monotone over every n in [0, MAX_CLASS_SIZE];
 *     class_size(class(n)) >= n; class(n) is the smallest such class; and
 *     every size in [n, usable(n)] maps to the same class;
 *   - sizes above the largest class map to SOHAM_INTERNAL_NCLASSES;
 *   - page kinds follow the spec §4 table; page size is a positive multiple
 *     of the slice; SMALL pages are one slice; cap = page/size (at least 1),
 *     so cap * size <= page size; RUN classes have cap 1 and page == size;
 *   - the allocator really uses the tables: usable_size(malloc(n)) equals
 *     class_size(class(n)) at and just above every class boundary, and
 *     every such block is 16-aligned;
 *   - geometry (spec §3): B is the OS page size, S = max(64 KiB, B),
 *     H = max(2 MiB, 32 S), W = 2^wshift, the window layout offsets, the
 *     192-byte record and the 32 MiB solo threshold;
 *   - link encoding (spec §5.2): dec(enc(n, a), a) == n for random key,
 *     next and address, and enc matches bswap(n ^ key ^ (a >> 12));
 *   - the security tier is 1 exactly when SOHAMALLOC_SECURE=1.
 */
#include "common/test_util.h"

#include "../src/internal_test.h"

#if defined(TEST_PREFIXED) && defined(TEST_SHIM)
int main(int argc, char **argv)
{
    tu_init("internal_props", argc, argv);
    tu_skip("libc shim: no sohamalloc internals");
}
#else

/* ------------------------------------------------------------ entry points */
static unsigned (*f_size_class)(size_t);
static size_t (*f_class_size)(unsigned);
static unsigned (*f_class_kind)(unsigned);
static size_t (*f_class_page_size)(unsigned);
static unsigned (*f_class_cap)(unsigned);
static uintptr_t (*f_link_enc)(uintptr_t, uintptr_t, uintptr_t);
static uintptr_t (*f_link_dec)(uintptr_t, uintptr_t, uintptr_t);
static void (*f_geometry)(struct soham_internal_geometry *);
static int (*f_tier)(void);

static int bind_entry_points(void)
{
#ifdef TEST_PREFIXED
    f_size_class = soham_internal_size_class;
    f_class_size = soham_internal_class_size;
    f_class_kind = soham_internal_class_kind;
    f_class_page_size = soham_internal_class_page_size;
    f_class_cap = soham_internal_class_cap;
    f_link_enc = soham_internal_link_enc;
    f_link_dec = soham_internal_link_dec;
    f_geometry = soham_internal_geometry;
    f_tier = soham_internal_security_tier;
    return 1;
#else
#define BIND(var, name)                                   \
    do {                                                  \
        *(void **)(&var) = tu_sym(name);                  \
        if (!var) TU_FAIL("sohamalloc does not export %s", name); \
    } while (0)
    BIND(f_size_class, "soham_internal_size_class");
    BIND(f_class_size, "soham_internal_class_size");
    BIND(f_class_kind, "soham_internal_class_kind");
    BIND(f_class_page_size, "soham_internal_class_page_size");
    BIND(f_class_cap, "soham_internal_class_cap");
    BIND(f_link_enc, "soham_internal_link_enc");
    BIND(f_link_dec, "soham_internal_link_dec");
    BIND(f_geometry, "soham_internal_geometry");
    BIND(f_tier, "soham_internal_security_tier");
#undef BIND
    return 1;
#endif
}

#define NC SOHAM_INTERNAL_NCLASSES
#define MAXC SOHAM_INTERNAL_MAX_CLASS_SIZE

static struct soham_internal_geometry geo;

/* ------------------------------------------------------------ classes */
static void test_class_table(void)
{
    size_t prev = 0;
    for (unsigned c = 0; c < NC; c++) {
        size_t s = f_class_size(c);
        if (s == 0) TU_FAIL("class %u has size 0", c);
        if (s % 16) TU_FAIL("class %u size %zu is not a multiple of 16", c, s);
        if (s <= prev) TU_FAIL("class sizes not strictly increasing: class %u = %zu after %zu", c, s, prev);
        prev = s;
    }
    TU_CHECK(f_class_size(0) == 16, "class 0 is %zu bytes, want 16", f_class_size(0));
    TU_CHECK(f_class_size(NC - 1) == MAXC, "last class %zu != SOHAM_INTERNAL_MAX_CLASS_SIZE %zu",
             f_class_size(NC - 1), (size_t)MAXC);
    TU_CHECK(f_class_size(NC) == 0 && f_class_size(NC + 7) == 0 && f_class_size(~0u) == 0,
             "class_size of an invalid id must be 0");
    TU_CHECK(f_class_kind(NC) == 0 && f_class_page_size(NC) == 0 && f_class_cap(NC) == 0,
             "kind/page/cap of an invalid id must be 0");
}

static void test_size_to_class(void)
{
    unsigned prevc = 0;
    TU_CHECK(f_size_class(0) == 0, "size_class(0) = %u, want 0", f_size_class(0));
    for (size_t n = 0; n <= MAXC; n++) {
        unsigned c = f_size_class(n);
        if (c >= NC) TU_FAIL("size_class(%zu) = %u out of range", n, c);
        if (c < prevc) TU_FAIL("size_class not monotone: size_class(%zu) = %u < size_class(%zu) = %u", n, c, n - 1, prevc);
        size_t u = f_class_size(c);
        if (u < n) TU_FAIL("class_size(size_class(%zu) = %u) = %zu < n", n, c, u);
        if (c > 0 && f_class_size(c - 1) >= n)
            TU_FAIL("size_class(%zu) = %u is not the smallest fitting class (class %u holds %zu)", n, c, c - 1,
                    f_class_size(c - 1));
        /* Every size in [n, usable] maps to c: with monotonicity it is enough
           that the usable size itself maps to c. */
        if (f_size_class(u) != c) TU_FAIL("size_class(%zu) = %u but its usable size %zu maps to %u", n, c, u, f_size_class(u));
        prevc = c;
        if ((n & 0xffff) == 0) tu_tick();
    }
    static const size_t big[] = { MAXC + 1, MAXC + 16, (size_t)1 << 20, (size_t)32 << 20, SIZE_MAX / 2, SIZE_MAX };
    for (size_t i = 0; i < sizeof big / sizeof big[0]; i++)
        if (f_size_class(big[i]) != NC) TU_FAIL("size_class(%zu) = %u, want %u", big[i], f_size_class(big[i]), NC);
    /* Both lookup paths (LUT for n <= LUT_MAX, clz above) meet correctly. */
    TU_CHECK(f_size_class(SOHAM_INTERNAL_LUT_MAX) < f_size_class(SOHAM_INTERNAL_LUT_MAX + 1) ||
             f_class_size(f_size_class(SOHAM_INTERNAL_LUT_MAX)) > SOHAM_INTERNAL_LUT_MAX,
             "LUT/clz boundary");
}

static void test_pages(void)
{
    size_t S = geo.slice;
    for (unsigned c = 0; c < NC; c++) {
        size_t s = f_class_size(c), pg = f_class_page_size(c);
        unsigned k = f_class_kind(c), cap = f_class_cap(c);
        unsigned want_kind;
        if (c <= 43) want_kind = SOHAM_INTERNAL_KIND_SMALL;
        else if (c <= 59) want_kind = SOHAM_INTERNAL_KIND_MEDIUM;
        else if (s == 160 * 1024 || s == 224 * 1024) want_kind = SOHAM_INTERNAL_KIND_MEDIUM;
        else if (s == 192 * 1024 || s == 256 * 1024) want_kind = SOHAM_INTERNAL_KIND_RUN;
        else want_kind = 0;
        if (k != want_kind) TU_FAIL("class %u (%zu B) has kind %u, want %u (spec §4)", c, s, k, want_kind);
        if (pg == 0) TU_FAIL("class %u page size 0", c);
        if (pg % S) TU_FAIL("class %u page size %zu is not a multiple of the slice %zu", c, pg, S);
        if (cap == 0) TU_FAIL("class %u cap 0", c);
        if ((size_t)cap * s > pg) TU_FAIL("class %u: cap %u * size %zu = %zu > page size %zu", c, cap, s, (size_t)cap * s, pg);
        if (k == SOHAM_INTERNAL_KIND_RUN) {
            if (cap != 1 || pg != s) TU_FAIL("RUN class %u: cap %u page %zu, want cap 1 and page == size %zu", c, cap, pg, s);
        } else {
            if (cap != pg / s) TU_FAIL("class %u: cap %u != page %zu / size %zu", c, cap, pg, s);
            if (k == SOHAM_INTERNAL_KIND_SMALL && pg != S) TU_FAIL("SMALL class %u page %zu != slice %zu", c, pg, S);
            if (k == SOHAM_INTERNAL_KIND_MEDIUM && cap < 2 && s < MAXC)
                TU_FAIL("MEDIUM class %u (%zu B) holds only %u block(s) per %zu-byte page", c, s, cap, pg);
            /* Tail waste stays under one block (cap is maximal) and is not
               absurd: at most a quarter of the page. */
            if (pg - (size_t)cap * s > pg / 4)
                TU_FAIL("class %u: page %zu wastes %zu bytes (> 25%%)", c, pg, pg - (size_t)cap * s);
        }
    }
}

/* The allocator's real usable sizes agree with the tables. */
static void test_tables_match_allocator(void)
{
    for (unsigned c = 0; c < NC; c++) {
        size_t s = f_class_size(c);
        size_t ns[3] = { s, s - 15 > 0 ? s - 15 : 1, c ? f_class_size(c - 1) + 1 : 0 };
        for (int i = 0; i < 3; i++) {
            void *p = T_MALLOC(ns[i]);
            TU_CHECK(p, "malloc(%zu)", ns[i]);
            size_t u = T_USABLE(p);
            if (u != s) TU_FAIL("usable_size(malloc(%zu)) = %zu but the class table says %zu (class %u)", ns[i], u, s, c);
            if ((uintptr_t)p & 15) TU_FAIL("malloc(%zu) = %p not 16-aligned", ns[i], p);
            T_FREE(p);
        }
    }
}

/* ------------------------------------------------------------ geometry */
static int is_pow2(size_t x) { return x && !(x & (x - 1)); }

static void test_geometry(void)
{
    size_t B = geo.base_page;
#ifdef _WIN32
    TU_CHECK(is_pow2(B), "base page %zu", B);
#else
    TU_CHECK(B == tu_page_size(), "base page %zu != OS page size %zu", B, tu_page_size());
#endif
    size_t S = B > 65536 ? B : 65536;
    TU_CHECK(geo.slice == S, "slice %zu, want max(64 KiB, B) = %zu", geo.slice, S);
    size_t H = 32 * S > ((size_t)2 << 20) ? 32 * S : ((size_t)2 << 20);
    TU_CHECK(geo.hugeblock == H, "hugeblock %zu, want max(2 MiB, 32 S) = %zu", geo.hugeblock, H);
    TU_CHECK(geo.wshift < sizeof(size_t) * 8 && ((size_t)1 << geo.wshift) == geo.window,
             "window %zu != 1 << wshift %u", geo.window, geo.wshift);
    size_t wdef = sizeof(void *) == 8 ? ((size_t)4 << 30) : ((size_t)256 << 20);
    TU_CHECK(geo.window <= wdef && geo.window >= ((size_t)64 << 20),
             "window %zu outside [64 MiB, %zu]", geo.window, wdef);
    if (geo.window != wdef) tu_info("NOTE window %zu != default %zu (finite RLIMIT_AS?)", geo.window, wdef);
    TU_CHECK(geo.record_size == 192, "record size %zu", geo.record_size);
    TU_CHECK(geo.solo_threshold == ((size_t)32 << 20), "solo threshold %zu", geo.solo_threshold);
    /* (no % in TU_CHECK conditions: the condition text becomes part of the format) */
    TU_CHECK((geo.rec_off & (B - 1)) == 0 && geo.rec_off > B, "rec_off %zu (B %zu)", geo.rec_off, B);
    TU_CHECK((geo.data_off & (H - 1)) == 0, "data_off %zu not hugeblock-aligned", geo.data_off);
    size_t rec_end = geo.rec_off + geo.window / S * geo.record_size;
    TU_CHECK(geo.data_off >= rec_end + B, "data_off %zu leaves no guard page after the records (end %zu)",
             geo.data_off, rec_end);
    TU_CHECK(geo.data_off < geo.window / 2, "data_off %zu absurd for window %zu", geo.data_off, geo.window);

    const char *sec = getenv("SOHAMALLOC_SECURE");
    int want = sec && strcmp(sec, "1") == 0;
    int tier = f_tier();
    TU_CHECK(tier == 0 || tier == 1, "security tier %d", tier);
    TU_CHECK(tier == want, "security tier %d but SOHAMALLOC_SECURE=%s", tier, sec ? sec : "(unset)");
    tu_info("geometry: B=%zu S=%zu H=%zu W=%zu (shift %u) rec_off=%zu data_off=%zu, tier S%d", B, geo.slice,
            geo.hugeblock, geo.window, geo.wshift, geo.rec_off, geo.data_off, tier);
}

/* ------------------------------------------------------------ links */
static uintptr_t bswap_word(uintptr_t x)
{
#if UINTPTR_MAX > 0xffffffffu
    return (uintptr_t)__builtin_bswap64((uint64_t)x);
#else
    return (uintptr_t)__builtin_bswap32((uint32_t)x);
#endif
}

static void test_links(void)
{
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ 0x11fe);
    size_t n = tu_scaled_min(2000000, 100000);
    for (size_t i = 0; i < n; i++) {
        uintptr_t key = (uintptr_t)tu_rng_next(&r);
        uintptr_t addr = (uintptr_t)tu_rng_next(&r) & ~(uintptr_t)15;
        uintptr_t next;
        switch (i & 3) {
        case 0: next = 0; break;                                     /* end of list */
        case 1: next = addr + 16 * (uintptr_t)tu_rng_below(&r, 4096); break; /* same page */
        default: next = (uintptr_t)tu_rng_next(&r); break;
        }
        if (i % 7 == 0) key = 0;
        uintptr_t e = f_link_enc(key, next, addr);
        uintptr_t d = f_link_dec(key, e, addr);
        if (d != next)
            TU_FAIL("link dec(enc(next=%#jx, addr=%#jx), key=%#jx) = %#jx", (uintmax_t)next, (uintmax_t)addr,
                    (uintmax_t)key, (uintmax_t)d);
        uintptr_t want = bswap_word(next ^ key ^ (addr >> 12));
        if (e != want)
            TU_FAIL("link enc(key=%#jx, next=%#jx, addr=%#jx) = %#jx, spec formula gives %#jx", (uintmax_t)key,
                    (uintmax_t)next, (uintmax_t)addr, (uintmax_t)e, (uintmax_t)want);
        if ((i & 0xffff) == 0) tu_tick();
    }
    /* A NULL link stored at an address is not stored as zero when the key
       is non-zero (so a zeroed word is not a valid end-of-list). */
    uintptr_t e0 = f_link_enc((uintptr_t)0x9e3779b97f4a7c15ULL, 0, (uintptr_t)0x7f0000123450ULL);
    TU_CHECK(e0 != 0, "enc(0) with a non-zero key is 0");
}

int main(int argc, char **argv)
{
    tu_init("internal_props", argc, argv);
    if (!tu_is_soham()) tu_skip("allocator is not sohamalloc: internals not available");
    bind_entry_points();
    f_geometry(&geo);
    test_geometry();
    test_class_table();
    test_size_to_class();
    test_pages();
    test_tables_match_allocator();
    test_links();
    tu_info("PASS: %u classes, sizes 0..%zu checked", NC, (size_t)MAXC);
    return 0;
}
#endif
