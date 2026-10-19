/*
 * tests/common/test_util.h -- shared helpers for the sohamalloc test suite.
 *
 * Every test is allocator-agnostic: it calls the standard malloc family (and
 * is run under LD_PRELOAD=<lib>), or, when built with -DTEST_PREFIXED, the
 * soham_* entry points directly (linked against libsohamalloc-prefixed.a or
 * the libc shim in tests/prefixed/libc_shim.c).  The T_* macros below select
 * the entry points, so one source serves both builds.
 *
 * Design rules for the helpers:
 *  - Failure reporting never allocates: messages are formatted into a stack
 *    buffer and written with write(2), then the process _exit()s.  A corrupt
 *    heap must not prevent the report from appearing.
 *  - Test bookkeeping (slot tables, the overlap oracle) lives in memory taken
 *    straight from the OS (tu_os_alloc), never from the allocator under test,
 *    so allocator corruption cannot silently corrupt the oracle itself.
 *  - Exit codes: 0 = PASS, 77 = SKIP (automake convention), 3 = watchdog
 *    detected no progress (likely deadlock), anything else = FAIL.
 *
 * This header is meant to be included by exactly one translation unit per
 * test binary; everything is static.
 */
#ifndef SOHAM_TEST_UTIL_H
#define SOHAM_TEST_UTIL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define tu_raw_write(fd, buf, n) _write((fd), (buf), (unsigned)(n))
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <dlfcn.h>
#  define tu_raw_write(fd, buf, n) write((fd), (buf), (n))
#endif

#if !defined(TEST_PREFIXED) && !defined(_WIN32)
#  include <malloc.h>
#endif

#include "soham_api.h"

/* ------------------------------------------------------------------------ */
/* Entry-point selection                                                     */
/* ------------------------------------------------------------------------ */
#ifdef TEST_PREFIXED
#  define T_MALLOC(n)                 soham_malloc(n)
#  define T_CALLOC(c, n)              soham_calloc((c), (n))
#  define T_REALLOC(p, n)             soham_realloc((p), (n))
#  define T_REALLOCARRAY(p, c, n)     soham_reallocarray((p), (c), (n))
#  define T_FREE(p)                   soham_free(p)
#  define T_FREE_SIZED(p, n)          soham_free_sized((p), (n))
#  define T_USABLE(p)                 soham_usable_size(p)
#  define T_POSIX_MEMALIGN(o, a, n)   soham_posix_memalign((o), (a), (n))
#  define T_ALIGNED_ALLOC(a, n)       soham_aligned_alloc((a), (n))
#  define T_MEMALIGN(a, n)            soham_memalign((a), (n))
#  define T_API_NAME                  "soham_* (prefixed)"
#else
#  define T_MALLOC(n)                 malloc(n)
#  define T_CALLOC(c, n)              calloc((c), (n))
#  define T_REALLOC(p, n)             realloc((p), (n))
#  define T_REALLOCARRAY(p, c, n)     reallocarray((p), (c), (n))
#  define T_FREE(p)                   free(p)
#  define T_FREE_SIZED(p, n)          ((void)(n), free(p))
#  define T_USABLE(p)                 malloc_usable_size(p)
#  define T_POSIX_MEMALIGN(o, a, n)   posix_memalign((o), (a), (n))
#  define T_ALIGNED_ALLOC(a, n)       aligned_alloc((a), (n))
#  define T_MEMALIGN(a, n)            memalign((a), (n))
#  define T_API_NAME                  "malloc family (LD_PRELOAD)"
#endif

#define TU_UNUSED __attribute__((unused))
#define TU_NOINLINE __attribute__((noinline))
#define TU_LIKELY(x) __builtin_expect(!!(x), 1)
#define TU_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Hide a pointer's provenance from the compiler.  _FORTIFY_SOURCE=3 (the
   default for gcc on Ubuntu) tracks malloc(n)'s alloc_size through
   __builtin_dynamic_object_size and aborts ("*** buffer overflow detected
   ***") on a memset/memcpy longer than n -- even when the extra bytes are
   within malloc_usable_size(p), which the contract makes writable.  Tests
   that deliberately write the whole usable range go through this. */
static inline void *tu_launder(void *p)
{
    __asm__ volatile("" : "+r"(p));
    return p;
}

#define TU_EXIT_SKIP 77
#define TU_EXIT_STALL 3

/* ------------------------------------------------------------------------ */
/* Reporting (never allocates)                                               */
/* ------------------------------------------------------------------------ */
static const char *tu_name = "test";

static TU_UNUSED void tu_write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        long w = (long)tu_raw_write(fd, s, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static TU_UNUSED void tu_write_str(int fd, const char *s)
{
    tu_write_all(fd, s, strlen(s));
}

static TU_UNUSED void tu_vmsg(int fd, const char *prefix, const char *fmt, va_list ap)
{
    char buf[1024];
    int off = snprintf(buf, sizeof buf, "%s[%s] ", prefix, tu_name);
    if (off < 0) off = 0;
    if ((size_t)off < sizeof buf) {
        int k = vsnprintf(buf + off, sizeof buf - (size_t)off, fmt, ap);
        if (k > 0) off += k;
    }
    if ((size_t)off >= sizeof buf - 1) off = (int)sizeof buf - 2;
    buf[off++] = '\n';
    tu_write_all(fd, buf, (size_t)off);
}

/* Print to stdout without stdio (so output ordering with stderr is sane and
   no allocation happens). */
static TU_UNUSED __attribute__((format(printf, 1, 2))) void tu_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    tu_vmsg(1, "", fmt, ap);
    va_end(ap);
}

static TU_UNUSED __attribute__((noreturn, format(printf, 3, 4)))
void tu_fail_at(const char *file, int line, const char *fmt, ...)
{
    char where[256];
    snprintf(where, sizeof where, "FAIL %s:%d ", file, line);
    va_list ap;
    va_start(ap, fmt);
    tu_vmsg(2, where, fmt, ap);
    va_end(ap);
    _exit(1);
}

#define TU_FAIL(...) tu_fail_at(__FILE__, __LINE__, __VA_ARGS__)
#define TU_CHECK(cond, ...) \
    do { if (TU_UNLIKELY(!(cond))) tu_fail_at(__FILE__, __LINE__, "check `" #cond "` failed: " __VA_ARGS__); } while (0)

static TU_UNUSED __attribute__((noreturn)) void tu_skip(const char *why)
{
    char buf[512];
    int k = snprintf(buf, sizeof buf, "SKIP [%s] %s\n", tu_name, why);
    if (k > 0) tu_write_all(1, buf, (size_t)k < sizeof buf ? (size_t)k : sizeof buf - 1);
    _exit(TU_EXIT_SKIP);
}

/* ------------------------------------------------------------------------ */
/* Configuration: scale and seed                                             */
/* ------------------------------------------------------------------------ */
static double tu_scale_v = 1.0;
static uint64_t tu_seed_v = 0x5eed5eed12345678ULL;

/* Common arguments: --seed=N (or a bare number), --scale=F.  Environment:
   TEST_SCALE (multiplies iteration counts; run_tests.sh --quick sets 0.2),
   TEST_SEED. */
static TU_UNUSED void tu_init(const char *name, int argc, char **argv)
{
    tu_name = name;
    const char *s = getenv("TEST_SCALE");
    if (s && *s) {
        double v = strtod(s, NULL);
        if (v > 0.0) tu_scale_v = v;
    }
    s = getenv("TEST_SEED");
    if (s && *s) tu_seed_v = strtoull(s, NULL, 0);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--seed=", 7) == 0) tu_seed_v = strtoull(argv[i] + 7, NULL, 0);
        else if (strncmp(argv[i], "--scale=", 8) == 0) {
            double v = strtod(argv[i] + 8, NULL);
            if (v > 0.0) tu_scale_v = v;
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9') tu_seed_v = strtoull(argv[i], NULL, 0);
    }
}

/* n scaled by TEST_SCALE, never below lo. */
static TU_UNUSED size_t tu_scaled_min(size_t n, size_t lo)
{
    double v = (double)n * tu_scale_v;
    size_t r = v < 1.0 ? 0 : (size_t)v;
    return r < lo ? lo : r;
}
#define tu_scaled(n) tu_scaled_min((n), 1)

static TU_UNUSED double tu_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static TU_UNUSED void tu_sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

/* ------------------------------------------------------------------------ */
/* RNG: splitmix64 seeding + xoshiro256**                                    */
/* ------------------------------------------------------------------------ */
typedef struct { uint64_t s[4]; } tu_rng;

static inline uint64_t tu_splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint64_t tu_mix64(uint64_t x)
{
    return tu_splitmix64(&x);
}

static TU_UNUSED void tu_rng_seed(tu_rng *r, uint64_t seed)
{
    uint64_t x = seed;
    for (int i = 0; i < 4; i++) r->s[i] = tu_splitmix64(&x);
}

static inline uint64_t tu_rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static inline uint64_t tu_rng_next(tu_rng *r)
{
    uint64_t *s = r->s;
    uint64_t res = tu_rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = tu_rotl(s[3], 45);
    return res;
}

/* Uniform in [0, n); n > 0.  (Modulo bias is irrelevant here.) */
static inline uint64_t tu_rng_below(tu_rng *r, uint64_t n) { return tu_rng_next(r) % n; }
/* Uniform in [lo, hi]. */
static inline uint64_t tu_rng_range(tu_rng *r, uint64_t lo, uint64_t hi)
{
    return lo + tu_rng_below(r, hi - lo + 1);
}
/* Roughly log-uniform in [lo, hi]: pick a power-of-two band, then uniform. */
static TU_UNUSED uint64_t tu_rng_logu(tu_rng *r, uint64_t lo, uint64_t hi)
{
    if (lo >= hi) return lo;
    int lb = 63 - __builtin_clzll(lo | 1), hb = 63 - __builtin_clzll(hi);
    int b = lb + (int)tu_rng_below(r, (uint64_t)(hb - lb + 1));
    uint64_t blo = 1ULL << b, bhi = (b >= 63) ? UINT64_MAX : (blo << 1) - 1;
    if (blo < lo) blo = lo;
    if (bhi > hi) bhi = hi;
    return tu_rng_range(r, blo, bhi);
}

/* The suite's standard mixed size distribution, capped at max. Weighted to
   resemble real programs (most allocations small) while still exercising every
   size class band, medium pages and page-heap runs. */
static TU_UNUSED size_t tu_rand_size(tu_rng *r, size_t max)
{
    uint64_t c = tu_rng_below(r, 1000);
    size_t n;
    if (c < 30) n = 0 + (size_t)tu_rng_below(r, 17);            /* tiny, incl. 0 */
    else if (c < 600) n = (size_t)tu_rng_range(r, 1, 128);
    else if (c < 850) n = (size_t)tu_rng_range(r, 129, 1024);
    else if (c < 950) n = (size_t)tu_rng_logu(r, 1025, 16384);
    else if (c < 990) n = (size_t)tu_rng_logu(r, 16385, 262144);
    else n = (size_t)tu_rng_logu(r, 262145, 4u << 20);
    return n > max ? max : n;
}

/* ------------------------------------------------------------------------ */
/* Memory for test bookkeeping, straight from the OS                         */
/* ------------------------------------------------------------------------ */
static TU_UNUSED void *tu_os_alloc(size_t n)
{
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) TU_FAIL("VirtualAlloc(%zu) failed", n);
    return p;
#else
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) TU_FAIL("mmap(%zu) for test bookkeeping failed: errno %d", n, errno);
    return p;
#endif
}

static TU_UNUSED void tu_os_free(void *p, size_t n)
{
    if (!p) return;
#ifdef _WIN32
    (void)n;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, n);
#endif
}

/* ------------------------------------------------------------------------ */
/* Content patterns                                                          */
/*                                                                           */
/* A block of n bytes filled with seed s holds, at byte offset o, byte (o&7) */
/* of word W(s, o>>3).  Blocks up to TU_FULL_LIMIT are written completely;    */
/* larger blocks are written in a fixed sparse set of regions (head, tail     */
/* and one 8-byte word per 4 KiB) so that multi-megabyte blocks stay cheap.   */
/* The region set depends only on n, so a check can be restricted to the      */
/* intersection of "written for n_written" and "[0, upto)": that is what a    */
/* realloc content-preservation check needs.                                 */
/* ------------------------------------------------------------------------ */
#define TU_FULL_LIMIT ((size_t)64 * 1024)
#define TU_EDGE ((size_t)4096)
#define TU_STRIDE ((size_t)4096)

static inline uint64_t tu_pword(uint64_t seed, size_t widx)
{
    uint64_t x = seed ^ ((uint64_t)widx * 0xD6E8FEB86659FD93ULL);
    x ^= x >> 32;
    x *= 0xD6E8FEB86659FD93ULL;
    x ^= x >> 32;
    return x;
}

static inline unsigned char tu_pbyte(uint64_t seed, size_t off)
{
    return (unsigned char)(tu_pword(seed, off >> 3) >> (8 * (off & 7)));
}

/* Fill bytes [from, to) of block p (block-relative offsets). */
static TU_UNUSED void tu_fill_range(unsigned char *p, size_t from, size_t to, uint64_t seed)
{
    size_t o = from;
    while (o < to && (o & 7)) { p[o] = tu_pbyte(seed, o); o++; }
    /* p is at least 8-aligned (every malloc result is), so p+o is aligned. */
    for (; o + 8 <= to; o += 8) {
        uint64_t w = tu_pword(seed, o >> 3);
        memcpy(p + o, &w, 8);
    }
    while (o < to) { p[o] = tu_pbyte(seed, o); o++; }
}

/* Returns the first mismatching offset in [from, to), or SIZE_MAX. */
static TU_UNUSED size_t tu_check_range(const unsigned char *p, size_t from, size_t to, uint64_t seed)
{
    size_t o = from;
    while (o < to && (o & 7)) { if (p[o] != tu_pbyte(seed, o)) return o; o++; }
    for (; o + 8 <= to; o += 8) {
        uint64_t w;
        memcpy(&w, p + o, 8);
        if (TU_UNLIKELY(w != tu_pword(seed, o >> 3))) {
            for (size_t k = 0; k < 8; k++)
                if (p[o + k] != tu_pbyte(seed, o + k)) return o + k;
        }
    }
    while (o < to) { if (p[o] != tu_pbyte(seed, o)) return o; o++; }
    return SIZE_MAX;
}

/* Iterate the written regions of an n-byte block, clipped to [0, upto).
   fn returns non-zero to stop; the stop value is returned. */
typedef size_t (*tu_region_fn)(unsigned char *p, size_t from, size_t to, uint64_t seed);

static TU_UNUSED size_t tu_fill_fn(unsigned char *p, size_t from, size_t to, uint64_t seed)
{
    tu_fill_range(p, from, to, seed);
    return SIZE_MAX;
}
static TU_UNUSED size_t tu_check_fn(unsigned char *p, size_t from, size_t to, uint64_t seed)
{
    return tu_check_range(p, from, to, seed);
}

static TU_UNUSED size_t tu_regions(unsigned char *p, size_t n, size_t upto, uint64_t seed, tu_region_fn fn)
{
    if (upto > n) upto = n;
    if (n <= TU_FULL_LIMIT) return upto ? fn(p, 0, upto, seed) : SIZE_MAX;
    size_t r;
    /* head */
    size_t a = 0, b = TU_EDGE < upto ? TU_EDGE : upto;
    if (a < b && (r = fn(p, a, b, seed)) != SIZE_MAX) return r;
    /* one word per 4 KiB stride in the middle */
    size_t tail_start = (n - TU_EDGE) & ~(size_t)7;
    for (size_t o = TU_EDGE; o + 8 <= tail_start; o += TU_STRIDE) {
        if (o + 8 > upto) break;
        if ((r = fn(p, o, o + 8, seed)) != SIZE_MAX) return r;
    }
    /* tail */
    a = tail_start; b = upto;
    if (a < b && (r = fn(p, a, b, seed)) != SIZE_MAX) return r;
    return SIZE_MAX;
}

static TU_UNUSED void tu_fill(void *p, size_t n, uint64_t seed)
{
    tu_regions((unsigned char *)p, n, n, seed, tu_fill_fn);
}

/* Check the pattern written for n_written bytes, restricted to [0, upto).
   Returns the first bad offset or SIZE_MAX. */
static TU_UNUSED size_t tu_check(const void *p, size_t n_written, size_t upto, uint64_t seed)
{
    return tu_regions((unsigned char *)(uintptr_t)p, n_written, upto, seed, tu_check_fn);
}

/* All-zero check: full up to full_limit bytes, then sampled (head, tail and
   one word per 512 bytes).  Returns first non-zero offset or SIZE_MAX. */
static TU_UNUSED size_t tu_check_zero(const void *pv, size_t n, size_t full_limit)
{
    const unsigned char *p = (const unsigned char *)pv;
    if (n <= full_limit) {
        size_t o = 0;
        for (; o + 8 <= n; o += 8) {
            uint64_t w;
            memcpy(&w, p + o, 8);
            if (w) { for (size_t k = 0; k < 8; k++) if (p[o + k]) return o + k; }
        }
        for (; o < n; o++) if (p[o]) return o;
        return SIZE_MAX;
    }
    for (size_t o = 0; o < full_limit / 2; o++) if (p[o]) return o;
    for (size_t o = full_limit / 2; o < n - full_limit / 2; o += 512) if (p[o]) return o;
    for (size_t o = n - full_limit / 2; o < n; o++) if (p[o]) return o;
    return SIZE_MAX;
}

/* ------------------------------------------------------------------------ */
/* Overlap oracle: a treap of live [addr, addr+len) ranges keyed by address. */
/* Nodes are indexed by the caller's slot number; storage is OS memory.      */
/* ------------------------------------------------------------------------ */
typedef struct {
    uintptr_t key;
    size_t len;
    uint32_t prio;
    int32_t l, r;
} tu_rnode;

typedef struct {
    tu_rnode *n;
    size_t cap;
    int32_t root;
    size_t live;
    tu_rng rng;
} tu_ranges;

static TU_UNUSED void tu_ranges_init(tu_ranges *t, size_t cap)
{
    t->n = (tu_rnode *)tu_os_alloc(cap * sizeof(tu_rnode));
    t->cap = cap;
    t->root = -1;
    t->live = 0;
    tu_rng_seed(&t->rng, 0xabcdef);
}

static TU_UNUSED void tu_ranges_destroy(tu_ranges *t)
{
    tu_os_free(t->n, t->cap * sizeof(tu_rnode));
    t->n = NULL;
}

/* Split by key: *l gets keys < key, *r gets keys >= key. */
static void tu_tr_split(tu_ranges *t, int32_t x, uintptr_t key, int32_t *l, int32_t *r)
{
    if (x < 0) { *l = *r = -1; return; }
    if (t->n[x].key < key) {
        tu_tr_split(t, t->n[x].r, key, &t->n[x].r, r);
        *l = x;
    } else {
        tu_tr_split(t, t->n[x].l, key, l, &t->n[x].l);
        *r = x;
    }
}

static int32_t tu_tr_merge(tu_ranges *t, int32_t a, int32_t b)
{
    if (a < 0) return b;
    if (b < 0) return a;
    if (t->n[a].prio > t->n[b].prio) {
        t->n[a].r = tu_tr_merge(t, t->n[a].r, b);
        return a;
    }
    t->n[b].l = tu_tr_merge(t, a, t->n[b].l);
    return b;
}

/* Returns the slot of a live range overlapping [addr, addr+len), or -1. */
static TU_UNUSED int32_t tu_ranges_find_overlap(tu_ranges *t, uintptr_t addr, size_t len)
{
    if (len == 0) len = 1;
    int32_t x = t->root, pred = -1, succ = -1;
    while (x >= 0) {                       /* predecessor: max key <= addr */
        if (t->n[x].key <= addr) { pred = x; x = t->n[x].r; }
        else x = t->n[x].l;
    }
    if (pred >= 0 && t->n[pred].key + (t->n[pred].len ? t->n[pred].len : 1) > addr) return pred;
    x = t->root;
    while (x >= 0) {                       /* successor: min key > addr */
        if (t->n[x].key > addr) { succ = x; x = t->n[x].l; }
        else x = t->n[x].r;
    }
    if (succ >= 0 && t->n[succ].key < addr + len) return succ;
    return -1;
}

/* Insert slot's range; returns -1 or the slot it overlaps (not inserted). */
static TU_UNUSED int32_t tu_ranges_insert(tu_ranges *t, int32_t slot, uintptr_t addr, size_t len)
{
    int32_t c = tu_ranges_find_overlap(t, addr, len);
    if (c >= 0) return c;
    tu_rnode *nd = &t->n[slot];
    nd->key = addr;
    nd->len = len ? len : 1;
    nd->prio = (uint32_t)tu_rng_next(&t->rng);
    nd->l = nd->r = -1;
    int32_t a, b;
    tu_tr_split(t, t->root, addr, &a, &b);
    t->root = tu_tr_merge(t, tu_tr_merge(t, a, slot), b);
    t->live++;
    return -1;
}

static TU_UNUSED void tu_ranges_remove(tu_ranges *t, int32_t slot)
{
    uintptr_t key = t->n[slot].key;
    int32_t a, b, m, c;
    tu_tr_split(t, t->root, key, &a, &b);
    tu_tr_split(t, b, key + 1, &m, &c);
    if (m != slot) TU_FAIL("overlap oracle: slot %d not found at %p", (int)slot, (void *)key);
    t->root = tu_tr_merge(t, a, c);
    t->live--;
}

/* ------------------------------------------------------------------------ */
/* Allocator identification                                                  */
/* ------------------------------------------------------------------------ */
#ifdef TEST_PREFIXED
/* The libc shim's soham_version() returns a string starting "libc-shim". */
static TU_UNUSED int tu_is_soham(void)
{
    const char *v = soham_version();
    return v && strncmp(v, "libc-shim", 9) != 0;
}
#elif !defined(_WIN32)
static TU_UNUSED void *tu_sym(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}
/* True when the process's malloc is sohamalloc (LD_PRELOAD'ed or linked).
   Cached: dlsym takes the dynamic linker's lock, so it must not run in a
   signal handler (signals.c calls tu_req_align from handlers).  Call it once
   from main before any handler is installed. */
static _Atomic int tu_is_soham_cached = -1;
static TU_UNUSED int tu_is_soham(void)
{
    int v = atomic_load_explicit(&tu_is_soham_cached, memory_order_relaxed);
    if (v < 0) {
        v = tu_sym("soham_version") != NULL;
        atomic_store_explicit(&tu_is_soham_cached, v, memory_order_relaxed);
    }
    return v;
}
#endif

/* True when glibc-pinned semantics (spec §2) must hold exactly: the
   allocator is sohamalloc or glibc itself (or, prefixed, the libc shim).
   Other allocators get the C-standard reading, with quirks reported as
   NOTEs instead of failures. */
#ifdef TEST_PREFIXED
static TU_UNUSED int tu_strict(void) { return 1; }
#elif !defined(_WIN32)
static TU_UNUSED int tu_strict(void)
{
    static int cached = -1;
    if (cached < 0) {
        Dl_info di;
        void *m = dlsym(RTLD_DEFAULT, "malloc");
        cached = tu_is_soham() ||
                 (m && dladdr(m, &di) && di.dli_fname && strstr(di.dli_fname, "libc.so") != NULL) ||
                 (m && dladdr(m, &di) && di.dli_fname && strstr(di.dli_fname, "ld-musl") != NULL);
    }
    return cached;
}
#endif

/* The alignment a malloc(n) result must have.  sohamalloc promises 16 for
   every size on every platform (spec §2).  Other allocators are held to the
   C rule: alignment suitable for any object with fundamental alignment that
   fits in n bytes, i.e. min(max_align_t, largest power of two <= n), but at
   least pointer alignment (jemalloc, mimalloc and tcmalloc return 8-aligned
   blocks for n <= 8, which C permits). */
static TU_UNUSED size_t tu_req_align(size_t n)
{
#if !defined(_WIN32) || defined(TEST_PREFIXED)
    if (tu_is_soham()) return 16;
#endif
    size_t a = _Alignof(max_align_t);
    while (a > sizeof(void *) && a > n) a >>= 1;
    return a;
}

/* ------------------------------------------------------------------------ */
/* Process memory                                                            */
/* ------------------------------------------------------------------------ */
#ifndef _WIN32
static TU_UNUSED size_t tu_page_size(void)
{
    long v = sysconf(_SC_PAGESIZE);
    return v > 0 ? (size_t)v : 4096;
}

/* Resident set size in bytes from /proc/self/statm (no stdio). */
static TU_UNUSED size_t tu_rss(void)
{
    char buf[256];
    int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    long k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0) return 0;
    buf[k] = 0;
    char *s = buf;
    while (*s && *s != ' ') s++;           /* skip size */
    unsigned long long res = strtoull(s, NULL, 10);
    return (size_t)res * tu_page_size();
}
#endif

/* ------------------------------------------------------------------------ */
/* Threads                                                                   */
/* ------------------------------------------------------------------------ */
static TU_UNUSED pthread_t tu_thread_start(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    int e = pthread_create(&t, NULL, fn, arg);
    if (e) TU_FAIL("pthread_create failed: %d", e);
    return t;
}

static TU_UNUSED void tu_thread_join(pthread_t t)
{
    int e = pthread_join(t, NULL);
    if (e) TU_FAIL("pthread_join failed: %d", e);
}

/* ------------------------------------------------------------------------ */
/* Watchdog: exits with TU_EXIT_STALL if the progress counter stops moving.  */
/* It only reads an atomic, sleeps and writes, so it keeps working when the  */
/* allocator under test is deadlocked.                                       */
/* ------------------------------------------------------------------------ */
static _Atomic unsigned long tu_progress;
static _Atomic int tu_watchdog_stop;
static unsigned tu_watchdog_secs = 20;

static TU_UNUSED void *tu_watchdog_main(void *arg)
{
    (void)arg;
    unsigned long last = atomic_load_explicit(&tu_progress, memory_order_relaxed);
    double since = tu_now();
    while (!atomic_load_explicit(&tu_watchdog_stop, memory_order_relaxed)) {
        tu_sleep_ms(100);
        unsigned long now = atomic_load_explicit(&tu_progress, memory_order_relaxed);
        if (now != last) { last = now; since = tu_now(); continue; }
        if (tu_now() - since > (double)tu_watchdog_secs) {
            char buf[200];
            int k = snprintf(buf, sizeof buf,
                             "STALL [%s] no progress for %u s (progress=%lu): probable deadlock\n",
                             tu_name, tu_watchdog_secs, now);
            if (k > 0) tu_write_all(2, buf, (size_t)k);
            _exit(TU_EXIT_STALL);
        }
    }
    return NULL;
}

static TU_UNUSED void tu_tick(void)
{
    atomic_fetch_add_explicit(&tu_progress, 1, memory_order_relaxed);
}

#endif /* SOHAM_TEST_UTIL_H */
