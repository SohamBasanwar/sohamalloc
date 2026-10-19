/*
 * src/sohamalloc.c -- the sohamalloc core, built as one translation unit.
 *
 *   pageheap.c   size classes, windows, hugeblocks, release (decay)
 *   heap.c       thread heaps, pages, pools, remote frees, runs, solos
 *   api.c        public API, fast paths, init, threads, fork, stats, overrides
 *
 * Read docs/IMPLEMENTATION_NOTES.md for the data structures and protocols.
 */
#include "internal.h"
#include "internal_test.h"

/* ---------------------------------------------------------------------- */
/* Global state                                                            */
/* ---------------------------------------------------------------------- */

static _Atomic uint8_t g_gmap_zero[1];   /* the granule map before init: all foreign */
static soham_geo_t g_geo __attribute__((aligned(64))) = { .gmap = g_gmap_zero, .amask = 15 };
static soham_cfg_t g_cfg;
static uintptr_t g_secret;
static size_t g_gmap_n_pending;          /* gmap entries (published at init end) */
static _Atomic int g_init_state;         /* 0 none, 1 running, 2 done */

static _Thread_local soham_tls_t t_tls SOHAM_TLS_MODEL;

static inline uintptr_t g_secret_word(void) { return g_secret; }

#include "pageheap.c"
#include "heap.c"
#include "api.c"
