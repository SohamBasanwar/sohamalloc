/*
 * sohamalloc -- a general-purpose memory allocator.
 *
 * Public API.  This header is a contract (docs/IMPL_SPEC_v0.md §2): names,
 * signatures and semantics may only change together with that document.
 *
 * Override builds (libsohamalloc.so, libsohamalloc.a) additionally export the
 * standard malloc family, the glibc compatibility functions and every global
 * C++ operator new/delete.  The prefixed build (libsohamalloc-prefixed.a)
 * exports only the soham_* symbols declared here.
 */
#ifndef SOHAMALLOC_H
#define SOHAMALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOHAMALLOC_VERSION "0.1.0"

void  *soham_malloc(size_t n);
void  *soham_calloc(size_t count, size_t n);
void  *soham_realloc(void *p, size_t n);
void  *soham_reallocarray(void *p, size_t count, size_t n);
void   soham_free(void *p);
void   soham_free_sized(void *p, size_t n);                      /* C23 semantics */
void   soham_free_aligned_sized(void *p, size_t align, size_t n);
void  *soham_aligned_alloc(size_t align, size_t n);
int    soham_posix_memalign(void **out, size_t align, size_t n);
void  *soham_memalign(size_t align, size_t n);
size_t soham_usable_size(const void *p);
void  *soham_malloc_at_least(size_t n, size_t *usable);          /* size feedback (plan §5.13) */
void   soham_collect(int force);   /* release free memory to the OS now (force=1: everything releasable) */
void   soham_stats_print(int fd);  /* the "explain my memory" report (plan §5.13), written with write(2) */

struct soham_stats {
  size_t committed;      /* bytes committed (RW) in windows + solos */
  size_t resident_est;   /* committed - released */
  size_t in_use;         /* bytes handed to the application (block granularity) */
  size_t cached_heaps;   /* free bytes inside pages owned by thread heaps */
  size_t cached_pools;   /* free bytes inside pages in class pools / orphans */
  size_t pageheap_free;  /* free slices not yet released */
  size_t released;       /* committed but released via madvise */
  size_t metadata;       /* records, heaps, internal arena */
  size_t solo;           /* bytes in solo mappings */
  size_t windows, heaps_live, heaps_pooled;
};

int    soham_get_stats(struct soham_stats *out);   /* 0 on success; a best-effort snapshot */
int    soham_check_heap(void);                     /* 0 = consistent; SOHAM_DEBUG builds walk everything */
const char *soham_version(void);

#ifdef __cplusplus
}
#endif

#endif /* SOHAMALLOC_H */
