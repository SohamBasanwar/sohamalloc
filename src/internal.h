/*
 * src/internal.h -- shared definitions of the sohamalloc core.
 *
 * The core is built as one translation unit (src/sohamalloc.c includes the
 * other .c parts) so the fast paths inline across files.
 *
 * Section references (§n) point to docs/IMPL_SPEC_v0.md; "plan §n" points to
 * docs/SOHAMALLOC_PLAN.md.
 */
#ifndef SOHAM_INTERNAL_H
#define SOHAM_INTERNAL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "os.h"
#include "../include/sohamalloc.h"

/* ---------------------------------------------------------------------- */
/* Build configuration                                                     */
/* ---------------------------------------------------------------------- */

#ifndef SOHAM_OVERRIDE
#define SOHAM_OVERRIDE 1
#endif
#ifndef SOHAM_DEBUG
#define SOHAM_DEBUG 0
#endif

#if defined(_WIN32) || defined(__APPLE__)
/* Override of the system allocator is only implemented for ELF platforms;
 * elsewhere only the soham_* API is provided (§8). */
#  undef SOHAM_OVERRIDE
#  define SOHAM_OVERRIDE 0
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define SOHAM_INLINE static inline __attribute__((always_inline))
#define SOHAM_NOINLINE __attribute__((noinline))
#define SOHAM_COLD __attribute__((cold, noinline))

#if defined(_WIN32)
#  define SOHAM_API __declspec(dllexport)
#else
#  define SOHAM_API __attribute__((visibility("default")))
#endif

#if defined(__ELF__) && !defined(_WIN32)
#  define SOHAM_TLS_MODEL __attribute__((tls_model("initial-exec")))
#else
#  define SOHAM_TLS_MODEL
#endif

#if UINTPTR_MAX > 0xffffffffu
#  define SOHAM_64BIT 1
#  define SOHAM_PTR_BITS 48          /* address bits used by heap pointers */
#else
#  define SOHAM_64BIT 0
#  define SOHAM_PTR_BITS 32
#endif

/* Strip the arm64 top-byte tag (TBI/MTE); all other targets use the full
 * pointer (§3 "Pointer -> record"). */
#if defined(__aarch64__)
#  define SOHAM_ADDR_MASK ((uintptr_t)0x00ffffffffffffffull)
#else
#  define SOHAM_ADDR_MASK (~(uintptr_t)0)
#endif

/* ThreadSanitizer reserves most of the 47-bit address space for shadow
 * memory: use small windows and let the kernel place them. */
#if defined(__SANITIZE_THREAD__)
#  define SOHAM_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define SOHAM_TSAN 1
#  endif
#endif
#ifndef SOHAM_TSAN
#  define SOHAM_TSAN 0
#endif

#if SOHAM_DEBUG
#  define SOHAM_ASSERT(c) do { if (unlikely(!(c))) soham_fatal("assertion failed: " #c, (uintptr_t)__LINE__); } while (0)
#else
#  define SOHAM_ASSERT(c) do { (void)sizeof(c); } while (0)
#endif

#define KiB ((size_t)1024)
#define MiB ((size_t)1024 * 1024)
#define GiB ((size_t)1024 * 1024 * 1024)

/* ---------------------------------------------------------------------- */
/* Size classes (plan §5.3)                                                */
/* ---------------------------------------------------------------------- */

#define SOHAM_NCLASSES      64u
#define SOHAM_LUT_MAX       1024u
#define SOHAM_NSMALL        44u              /* ids 0-43: one-slice pages */
#define SOHAM_MAX_CLASS     ((size_t)256 * 1024)
#define SOHAM_FAST_MAX      ((size_t)128 * 1024)   /* classes 0-59: bins */
#define SOHAM_CLASS_RUN_A   61u              /* 192 KiB: served as a run */
#define SOHAM_CLASS_RUN_B   63u              /* 256 KiB: served as a run */
#define SOHAM_SOLO_MIN      ((size_t)32 * 1024 * 1024)

enum { KIND_NONE = 0, KIND_SMALL = 1, KIND_MEDIUM = 2, KIND_RUN = 3 };

/* ---------------------------------------------------------------------- */
/* Packed words                                                            */
/* ---------------------------------------------------------------------- */

/* Owner-side free word (record line O): count in the high bits, list head
 * in the low SOHAM_PTR_BITS bits.  Every local pop/push commits with one
 * store of this word (invariant I3). */
#define FW_PTRMASK (((uint64_t)1 << SOHAM_PTR_BITS) - 1)
SOHAM_INLINE uintptr_t fw_ptr(uint64_t w) { return (uintptr_t)(w & FW_PTRMASK); }
SOHAM_INLINE uint32_t fw_cnt(uint64_t w) { return (uint32_t)(w >> SOHAM_PTR_BITS); }
SOHAM_INLINE uint64_t fw_make(uintptr_t p, uint32_t c) { return (uint64_t)p | ((uint64_t)c << SOHAM_PTR_BITS); }

/* Remote free word (record line X): count | head (16-aligned) | flags.  §5.5 */
#define XF_ORPHAN  ((uint64_t)1)   /* detached; on no list; count = total free */
#define XF_QUEUED  ((uint64_t)2)   /* in a class pool; count = total free */
#define XF_PENDING ((uint64_t)4)   /* one thread is responsible for resolve() */
#define XF_WATCH   ((uint64_t)8)   /* owned page on its owner's full list: the
                                      next remote free notifies the owner */
#define XF_FLAGS   ((uint64_t)15)
SOHAM_INLINE uintptr_t xf_ptr(uint64_t w) { return (uintptr_t)(w & FW_PTRMASK & ~(uint64_t)15); }
SOHAM_INLINE uint32_t xf_cnt(uint64_t w) { return (uint32_t)(w >> SOHAM_PTR_BITS); }
SOHAM_INLINE uint64_t xf_make(uintptr_t p, uint32_t c, uint64_t flags) {
  return (uint64_t)p | ((uint64_t)c << SOHAM_PTR_BITS) | flags;
}

/* Owner word values.  A heap tag is 1..2^24-1; every special value has a
 * bit at or above bit 24 set so it never equals a tag (§5.1 line R). */
#define OWNER_NONE  0u
#define OWNER_RUN   0x40000000u
#define OWNER_SPARE 0x20000000u
#define OWNER_BAD   0x80000000u   /* sentinel record */
#define SOHAM_MAX_TAG 0x00ffffffu

/* ---------------------------------------------------------------------- */
/* Slice records: 192 B, three cache lines with distinct writers (§5.1)    */
/* ---------------------------------------------------------------------- */

typedef struct rec_s rec_t;
typedef struct heap_s heap_t;

/* lflags: which owner list a page is on (cur is on none). */
#define LF_AVAIL 1u
#define LF_FULL  2u

typedef struct {                 /* line O: written by the owning heap */
  /* Local free list.  Only the holder of the owning heap reads or writes
   * these (signal handlers never do: tls.busy routes them elsewhere), so
   * head and count need not be packed into one word.  Relaxed atomics only
   * because statistics read them racily; they compile to plain moves. */
  _Atomic uintptr_t fhead;
  _Atomic uint32_t fcnt;
  uint32_t prov;                 /* blocks provisioned so far */
  uint32_t event_at;             /* local count at which a local free calls
                                    page_local_event: prov, or 1 on the full list */
  uint32_t psize;                /* copy of r.psize */
  uintptr_t bump;                /* next unprovisioned byte */
  /* Owner-side copies of line R, so the owner's fast paths touch one line. */
  uintptr_t key;
  uintptr_t pbase;
  rec_t *lnext, *lprev;          /* owner's avail/full list */
} rec_o_t;

typedef struct {                 /* line R: written only at page init */
  _Atomic uint32_t owner;        /* heap tag, OWNER_* */
  uint32_t head_delta;           /* bytes back to the head record (0 = head) */
  uintptr_t key;                 /* per-page link key */
  uintptr_t pbase;               /* page base address */
  uint32_t psize;                /* page bytes (slice multiple) */
  uint32_t bsize;                /* block bytes (RUN: psize) */
  uint32_t cap;                  /* blocks per page (RUN: 1) */
  uint32_t nslices;              /* slices in the page */
  uint8_t cls, kind, zero, lflags; /* lflags: LF_*, owner-written on list moves */
  _Atomic(heap_t *) oheap;       /* owning heap (valid while owner is a tag) */
} rec_r_t;

typedef struct {                 /* line X: remote frees and pool links */
  _Atomic uint64_t xfree;        /* xf */
  uint32_t folded;               /* local + unprovisioned count at detach */
  uint32_t bucket;
  rec_t *pnext, *pprev;          /* class-pool list */
  rec_t *pend_next;              /* pending-pages stack */
  _Atomic uint32_t pend_state;   /* 1 while on the pending stack */
  _Atomic uint32_t nstate;       /* 1 while on an owner's notify stack */
  rec_t *nnext;                  /* owner notify stack link */
} rec_x_t;

struct rec_s {
  union { rec_o_t o; char o_pad[64]; };
  union { rec_r_t r; char r_pad[64]; };
  union { rec_x_t x; char x_pad[64]; };
};
#define REC_SIZE 192u
_Static_assert(sizeof(rec_t) == REC_SIZE, "record must be 192 bytes");

/* ---------------------------------------------------------------------- */
/* Hugeblocks and windows (§3, §6)                                         */
/* ---------------------------------------------------------------------- */

#define HB_SLICES 32u                 /* slices per hugeblock: always 32 */

/* Slice size S = max(64 KiB, base page).  Every mainstream base page (4, 16
 * and 64 KiB) gives S = 64 KiB, so the shift is a compile-time constant;
 * build with -DSOHAM_SLICE_SHIFT=18 for 256 KiB-page systems. */
#ifndef SOHAM_SLICE_SHIFT
#define SOHAM_SLICE_SHIFT 16u
#endif
#define HB_FULL   0xffffffffu

typedef struct window_s window_t;
typedef struct hbmeta_s hbmeta_t;

struct hbmeta_s {
  uint32_t used;        /* slice is part of a page or run */
  uint32_t dirty;       /* free and resident (eligible for release) */
  uint32_t zero;        /* free and known to read as zero */
  uint32_t releasing;   /* being released right now: not allocatable */
  hbmeta_t *bprev, *bnext;
  uint64_t dirty_since; /* ns when dirty became non-zero; 0 if no dirty */
  window_t *win;
  uint32_t idx;         /* hugeblock index within the window */
  uint8_t longest;      /* longest run of allocatable slices */
  uint8_t bucket;       /* bucket list it is on, or 0 */
  uint16_t pad;
};

struct window_s {
  uint64_t magic;
  uintptr_t base;
  window_t *next;
  hbmeta_t *hb;         /* hb[nhb_max] */
  uint32_t nhb;         /* committed hugeblocks */
  uint32_t nhb_max;
  uint32_t index;
};

#define WINDOW_MAGIC 0x736f68616d77696eull   /* "sohamwin" */

/* ---------------------------------------------------------------------- */
/* Locks: a 3-state futex mutex (Drepper), never allocating                */
/* ---------------------------------------------------------------------- */

typedef struct { _Atomic uint32_t v; } soham_lock_t;

SOHAM_HIDDEN void soham_lock_slow(soham_lock_t *l);
SOHAM_INLINE void soham_lock(soham_lock_t *l) {
  uint32_t c = 0;
  if (likely(atomic_compare_exchange_strong_explicit(&l->v, &c, 1, memory_order_acquire, memory_order_relaxed))) return;
  soham_lock_slow(l);
}
SOHAM_INLINE bool soham_trylock(soham_lock_t *l) {
  uint32_t c = 0;
  return atomic_compare_exchange_strong_explicit(&l->v, &c, 1, memory_order_acquire, memory_order_relaxed);
}
SOHAM_INLINE void soham_unlock(soham_lock_t *l) {
  if (unlikely(atomic_exchange_explicit(&l->v, 0, memory_order_release) == 2)) soham_os_wake(&l->v);
}
SOHAM_INLINE void soham_lock_reset(soham_lock_t *l) { atomic_store_explicit(&l->v, 0, memory_order_relaxed); }

/* ---------------------------------------------------------------------- */
/* Heaps (slots) and thread state (§5.3)                                   */
/* ---------------------------------------------------------------------- */

typedef struct { rec_t *cur; rec_t *avail; rec_t *full; uintptr_t pad; } bin_t;

struct heap_s {
  uint32_t tag;
  uint8_t live;            /* attached to a running thread */
  uint8_t is_shared;
  uint8_t nspares;
  uint8_t pad0;
  uint64_t rng;
  rec_t *spares[2];
  heap_t *pool_next;       /* heap pool (LIFO) */
  heap_t *all_next;        /* registry of every heap ever created */
  _Atomic uint8_t *busy_ptr; /* owning thread's tls busy byte (fork child) */
  bin_t bins[SOHAM_NCLASSES];
  /* Pages on this heap's full lists that received a remote free.  Written
   * by other threads: kept on its own cache line. */
  _Atomic(rec_t *) notify __attribute__((aligned(64)));
};

enum { TLS_NEW = 0, TLS_ATTACHED = 1, TLS_DEAD = 2, TLS_INIT = 3 };

typedef struct {
  heap_t *heap;            /* NULL until attached */
  uint32_t tag;            /* heap->tag, or 0 */
  _Atomic uint8_t busy;    /* inside the allocator: reentry is a signal handler */
  uint8_t state;
  uint16_t pad;
  void *deferred;          /* frees deferred from handler context */
} soham_tls_t;

/* ---------------------------------------------------------------------- */
/* Read-mostly global state                                                */
/* ---------------------------------------------------------------------- */

#define GM_FOREIGN 0u
#define GM_WINDOW  1u

typedef struct {
  /* line 1: everything the fast paths read */
  _Atomic uintptr_t gmask;     /* gmap entries - 1; 0 before init */
  _Atomic(_Atomic uint8_t *) gmap; /* granule map; a 1-byte zero map before init */
  uintptr_t amask;             /* free fast path alignment mask: 15 (S0), all ones (S1) */
  uintptr_t wmask;             /* W - 1 */
  uintptr_t wbase;             /* ~(W - 1) */
  size_t rec_off;              /* offset of record[0] in a window */
  unsigned wshift;             /* log2(W) */
  unsigned sshift;             /* log2(S) */
  int secure;                  /* security tier S1 */
  int pad;
  /* geometry */
  size_t B, S, H, W;
  size_t meta_size;            /* committed metadata bytes at the window start */
  size_t data_off;             /* first data byte */
  uint32_t nhb_max;            /* hugeblocks per window */
  uint32_t pad2;
} soham_geo_t;

typedef struct {
  int profile;                 /* 0 balanced, 1 memory, 2 throughput */
  int stats_at_exit;
  int verbose;
  int max_spares;
  uint64_t decay_ns;           /* release free slices idle this long */
  uint64_t purge_interval_ns;  /* minimum time between release passes */
  int release_mode;            /* SOHAM_OS_RELEASE_* */
  int thp_huge;                /* apply MADV_HUGEPAGE to data */
  size_t prov_bytes;           /* provisioning step */
} soham_cfg_t;

enum { PROFILE_BALANCED = 0, PROFILE_MEMORY = 1, PROFILE_THROUGHPUT = 2 };

/* Link encoding (§5.2; plan §5.5.2, corrected):
 *   K = key ^ (a >> 12);  enc(n) = bswap(n ^ K);  dec(E) = bswap(E) ^ K.
 * bswap is outermost on the stored value, so a partial overwrite of the
 * low bytes corrupts the high bytes of the decoded pointer. */
#if SOHAM_64BIT
#  define SOHAM_BSWAP(x) ((uintptr_t)__builtin_bswap64((uint64_t)(x)))
#else
#  define SOHAM_BSWAP(x) ((uintptr_t)__builtin_bswap32((uint32_t)(x)))
#endif
SOHAM_INLINE uintptr_t link_enc(uintptr_t key, uintptr_t next, uintptr_t a) {
  return SOHAM_BSWAP(next ^ key ^ (a >> 12));
}
SOHAM_INLINE uintptr_t link_dec(uintptr_t key, uintptr_t stored, uintptr_t a) {
  return SOHAM_BSWAP(stored) ^ key ^ (a >> 12);
}

SOHAM_INLINE uintptr_t o_head(const rec_t *r) { return atomic_load_explicit(&r->o.fhead, memory_order_relaxed); }
SOHAM_INLINE uint32_t o_cnt(const rec_t *r) { return atomic_load_explicit(&r->o.fcnt, memory_order_relaxed); }
SOHAM_INLINE void o_set(rec_t *r, uintptr_t head, uint32_t cnt) {
  atomic_store_explicit(&r->o.fhead, head, memory_order_relaxed);
  atomic_store_explicit(&r->o.fcnt, cnt, memory_order_relaxed);
}

SOHAM_INLINE rec_t *rec_head(rec_t *r) { return (rec_t *)((char *)r - r->r.head_delta); }

SOHAM_INLINE void signal_fence(void) { atomic_signal_fence(memory_order_seq_cst); }

/* Diagnostics. */
SOHAM_HIDDEN void soham_fatal(const char *msg, uintptr_t p) __attribute__((noreturn, cold));
SOHAM_HIDDEN void soham_warn(const char *msg, uintptr_t p);

#endif /* SOHAM_INTERNAL_H */
