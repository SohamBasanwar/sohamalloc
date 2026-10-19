/*
 * src/internal_test.h -- internals of sohamalloc exposed for property tests.
 *
 * These functions are exported by every sohamalloc library (the override
 * libraries and libsohamalloc-prefixed.a).  They are not part of the public
 * API contract and may change between versions; only the test suite uses
 * them (docs/IMPL_SPEC_v0.md §4, plan §8.4 "Unit tests").
 *
 * Every function initialises the allocator on first use, so the tables are
 * valid whenever they are read.
 */
#ifndef SOHAM_INTERNAL_TEST_H
#define SOHAM_INTERNAL_TEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of size classes (plan §5.3). */
#define SOHAM_INTERNAL_NCLASSES 64u
/* Largest size served by a size class (class 63); larger sizes are runs or solos. */
#define SOHAM_INTERNAL_MAX_CLASS_SIZE ((size_t)256 * 1024)
/* Largest size whose class is found by the lookup table (plan §5.3). */
#define SOHAM_INTERNAL_LUT_MAX 1024u

/* Page kinds (docs/IMPL_SPEC_v0.md §4). */
#define SOHAM_INTERNAL_KIND_SMALL  1u   /* ids 0-43: one slice */
#define SOHAM_INTERNAL_KIND_MEDIUM 2u   /* ids 44-59, 160 KiB, 224 KiB: multi-slice pages */
#define SOHAM_INTERNAL_KIND_RUN    3u   /* 192 KiB, 256 KiB and every size in (256 KiB, 32 MiB) */
#define SOHAM_INTERNAL_KIND_SOLO   4u   /* >= 32 MiB, or alignment > hugeblock */

/* Size -> class id.  Defined for 0 <= n <= SOHAM_INTERNAL_MAX_CLASS_SIZE;
 * n == 0 maps to class 0.  Returns SOHAM_INTERNAL_NCLASSES for larger n. */
unsigned soham_internal_size_class(size_t n);

/* Block size of class c (c < SOHAM_INTERNAL_NCLASSES), else 0. */
size_t soham_internal_class_size(unsigned c);

/* Page kind of class c: SMALL, MEDIUM or RUN; 0 for an invalid id. */
unsigned soham_internal_class_kind(unsigned c);

/* Page size in bytes for class c.  For SMALL it is one slice, for MEDIUM the
 * multi-slice page chosen by the plan's medium-page rule, and for the RUN
 * classes (192 KiB, 256 KiB) it is the run size, which equals the class size.
 * Always a multiple of the slice size.  0 for an invalid id. */
size_t soham_internal_class_page_size(unsigned c);

/* Blocks per page of class c (page_size / class_size; 1 for RUN classes). */
unsigned soham_internal_class_cap(unsigned c);

/* Encoded free-list links (docs/IMPL_SPEC_v0.md §5.2, plan §5.5.2):
 *   K(key, a)  = key ^ (a >> 12)
 *   enc(n, a)  = bswap(n ^ K)        (bswap of the native word size)
 *   dec(E, a)  = bswap(E) ^ K        so dec(enc(n, a), a) == n
 * On 32-bit targets key, n, a and the stored word are 32 bits wide. */
uintptr_t soham_internal_link_enc(uintptr_t key, uintptr_t next, uintptr_t addr);
uintptr_t soham_internal_link_dec(uintptr_t key, uintptr_t stored, uintptr_t addr);

/* Runtime geometry (docs/IMPL_SPEC_v0.md §3). */
struct soham_internal_geometry {
  size_t base_page;       /* B */
  size_t slice;           /* S = max(64 KiB, B) */
  size_t hugeblock;       /* H = max(2 MiB, 32 * S) */
  size_t window;          /* W */
  unsigned wshift;        /* log2(W) */
  size_t rec_off;         /* offset of the slice-record array in a window */
  size_t data_off;        /* offset of the first data hugeblock in a window */
  size_t record_size;     /* 192 */
  size_t solo_threshold;  /* 32 MiB */
};
void soham_internal_geometry(struct soham_internal_geometry *out);

/* Security tier in force: 0 = S0, 1 = S1 (SOHAMALLOC_SECURE=1). */
int soham_internal_security_tier(void);

#ifdef __cplusplus
}
#endif

#endif /* SOHAM_INTERNAL_TEST_H */
