/*
 * prefixed/libc_shim.c -- the soham_* API implemented on top of the libc
 * allocator, so the prefixed tests can be validated (and run under
 * sanitizers) independently of sohamalloc: run_tests.sh --shim links each
 * prefixed test with this file instead of libsohamalloc-prefixed.a.
 *
 * Semantics follow docs/IMPL_SPEC_v0.md §2 (the glibc choices) exactly, on
 * any libc, including Windows (mingw) where libc has no aligned malloc that
 * free() accepts: every block carries a 32-byte header in front of the user
 * pointer holding the raw libc pointer and the requested size, so any
 * alignment works and usable_size == requested size.
 *
 * soham_version() returns "libc-shim ...": test_util.h's tu_is_soham() keys
 * on that prefix, so the sohamalloc-only tests SKIP against the shim.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <io.h>
#  define shim_write(fd, b, n) _write((fd), (b), (unsigned)(n))
#else
#  include <unistd.h>
#  define shim_write(fd, b, n) write((fd), (b), (n))
#endif

#include "common/soham_api.h"

#define SHIM_MAGIC ((size_t)0x5348494d5348494dULL)   /* "SHIMSHIM" */
#define HDR 32

typedef struct {
    void *raw;
    size_t n;
    size_t magic;
    size_t align;
} shim_hdr;

_Static_assert(sizeof(shim_hdr) <= HDR, "header too large");

static _Atomic size_t shim_in_use, shim_blocks;

static void shim_corrupt(const char *what)
{
    static const char msg[] = "libc_shim: invalid pointer passed to ";
    (void)!shim_write(2, msg, sizeof msg - 1);
    (void)!shim_write(2, what, strlen(what));
    (void)!shim_write(2, "\n", 1);
    abort();
}

static shim_hdr *hdr_of(const void *p, const char *what)
{
    shim_hdr *h = (shim_hdr *)((char *)(uintptr_t)p - HDR);
    if (h->magic != (SHIM_MAGIC ^ (uintptr_t)p)) shim_corrupt(what);
    return h;
}

/* align: a power of two. */
static void *shim_alloc(size_t align, size_t n, int zero)
{
    if (align < 16) align = 16;
    if (n > (size_t)PTRDIFF_MAX - align - HDR - 64) { errno = ENOMEM; return NULL; }
    size_t total = n + align + HDR;
    void *raw = zero ? calloc(1, total) : malloc(total);
    if (!raw) { errno = ENOMEM; return NULL; }
    uintptr_t u = ((uintptr_t)raw + HDR + align - 1) & ~(uintptr_t)(align - 1);
    shim_hdr *h = (shim_hdr *)(u - HDR);
    h->raw = raw;
    h->n = n;
    h->align = align;
    h->magic = SHIM_MAGIC ^ u;
    atomic_fetch_add_explicit(&shim_in_use, n, memory_order_relaxed);
    atomic_fetch_add_explicit(&shim_blocks, 1, memory_order_relaxed);
    return (void *)u;
}

void *soham_malloc(size_t n) { return shim_alloc(16, n, 0); }

void *soham_calloc(size_t count, size_t n)
{
    size_t t;
    if (__builtin_mul_overflow(count, n, &t)) { errno = ENOMEM; return NULL; }
    return shim_alloc(16, t, 1);
}

void soham_free(void *p)
{
    if (!p) return;
    int e = errno;
    shim_hdr *h = hdr_of(p, "soham_free");
    atomic_fetch_sub_explicit(&shim_in_use, h->n, memory_order_relaxed);
    atomic_fetch_sub_explicit(&shim_blocks, 1, memory_order_relaxed);
    h->magic = 0;
    free(h->raw);
    errno = e;
}

void soham_free_sized(void *p, size_t n)
{
    if (p && hdr_of(p, "soham_free_sized")->n < n) shim_corrupt("soham_free_sized (size larger than the block)");
    soham_free(p);
}

void soham_free_aligned_sized(void *p, size_t align, size_t n)
{
    (void)align;
    soham_free_sized(p, n);
}

void *soham_realloc(void *p, size_t n)
{
    if (!p) return soham_malloc(n);
    if (n == 0) { soham_free(p); return NULL; }     /* glibc: free, return NULL */
    shim_hdr *h = hdr_of(p, "soham_realloc");
    void *q = shim_alloc(16, n, 0);
    if (!q) return NULL;                            /* p intact, errno = ENOMEM */
    memcpy(q, p, h->n < n ? h->n : n);
    soham_free(p);
    return q;
}

void *soham_reallocarray(void *p, size_t count, size_t n)
{
    size_t t;
    if (__builtin_mul_overflow(count, n, &t)) { errno = ENOMEM; return NULL; }
    return soham_realloc(p, t);
}

void *soham_aligned_alloc(size_t align, size_t n)
{
    if (align == 0) align = 1;
    if (align & (align - 1)) { errno = EINVAL; return NULL; }
    return shim_alloc(align, n, 0);
}

int soham_posix_memalign(void **out, size_t align, size_t n)
{
    if (align == 0 || (align & (align - 1)) || align % sizeof(void *)) return EINVAL;
    int e = errno;
    void *p = shim_alloc(align, n, 0);
    if (!p) { errno = e; return ENOMEM; }
    *out = p;
    return 0;
}

void *soham_memalign(size_t align, size_t n)
{
    size_t a = 1;
    while (a < align) {
        if (a > ((size_t)-1 >> 2)) { errno = EINVAL; return NULL; }
        a <<= 1;
    }
    return shim_alloc(a, n, 0);
}

size_t soham_usable_size(const void *p)
{
    return p ? hdr_of(p, "soham_usable_size")->n : 0;
}

void *soham_malloc_at_least(size_t n, size_t *usable)
{
    void *p = soham_malloc(n);
    if (p && usable) *usable = n;
    return p;
}

void soham_collect(int force) { (void)force; }

int soham_get_stats(struct soham_stats *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->in_use = atomic_load_explicit(&shim_in_use, memory_order_relaxed);
    out->committed = out->resident_est = out->in_use;
    return 0;
}

void soham_stats_print(int fd)
{
    static const char msg[] = "libc-shim " SOHAMALLOC_VERSION " -- the soham_* API mapped onto the libc allocator\n";
    (void)!shim_write(fd, msg, sizeof msg - 1);
}

int soham_check_heap(void) { return 0; }

const char *soham_version(void) { return "libc-shim " SOHAMALLOC_VERSION; }
