/*
 * tests/common/soham_api.h -- the sohamalloc public API as seen by the tests.
 *
 * If the real header (include/sohamalloc.h, put on the include path by
 * run_tests.sh) exists it is used, so any drift between the contract and the
 * implementation shows up as a compile error in the tests.  Otherwise this
 * file supplies a verbatim copy of the contract in docs/IMPL_SPEC_v0.md §2,
 * which lets the suite build (and the prefixed tests run against the libc
 * shim) before the allocator exists.  Keep the fallback identical to §2.
 */
#ifndef SOHAM_TEST_API_H
#define SOHAM_TEST_API_H

#include <stddef.h>

#if defined(__has_include)
#  if __has_include("sohamalloc.h")
#    include "sohamalloc.h"
#    define SOHAM_TEST_HAVE_REAL_HEADER 1
#  endif
#endif

#ifndef SOHAM_TEST_HAVE_REAL_HEADER
#ifdef __cplusplus
extern "C" {
#endif

#define SOHAMALLOC_VERSION "0.1.0"
void  *soham_malloc(size_t n);
void  *soham_calloc(size_t count, size_t n);
void  *soham_realloc(void *p, size_t n);
void  *soham_reallocarray(void *p, size_t count, size_t n);
void   soham_free(void *p);
void   soham_free_sized(void *p, size_t n);
void   soham_free_aligned_sized(void *p, size_t align, size_t n);
void  *soham_aligned_alloc(size_t align, size_t n);
int    soham_posix_memalign(void **out, size_t align, size_t n);
void  *soham_memalign(size_t align, size_t n);
size_t soham_usable_size(const void *p);
void  *soham_malloc_at_least(size_t n, size_t *usable);
void   soham_collect(int force);
void   soham_stats_print(int fd);
struct soham_stats {
  size_t committed;
  size_t resident_est;
  size_t in_use;
  size_t cached_heaps;
  size_t cached_pools;
  size_t pageheap_free;
  size_t released;
  size_t metadata;
  size_t solo;
  size_t windows, heaps_live, heaps_pooled;
};
int    soham_get_stats(struct soham_stats *out);
int    soham_check_heap(void);
const char *soham_version(void);

#ifdef __cplusplus
}
#endif
#endif /* !SOHAM_TEST_HAVE_REAL_HEADER */

#endif /* SOHAM_TEST_API_H */
