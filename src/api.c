/*
 * src/api.c -- public API, fast paths, init, threads, fork, stats, overrides.
 * Part of the sohamalloc unity build (included by src/sohamalloc.c).
 */

/* ---------------------------------------------------------------------- */
/* Diagnostics (write(2) only; never allocates)                            */
/* ---------------------------------------------------------------------- */

static size_t fmt_u(char *out, uint64_t v) {
  char tmp[24];
  size_t n = 0;
  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  return n;
}

static size_t fmt_x(char *out, uint64_t v) {
  static const char hex[] = "0123456789abcdef";
  char tmp[18];
  size_t n = 0;
  do { tmp[n++] = hex[v & 15]; v >>= 4; } while (v);
  out[0] = '0'; out[1] = 'x';
  for (size_t i = 0; i < n; i++) out[2 + i] = tmp[n - 1 - i];
  return n + 2;
}

typedef struct { char buf[512]; size_t n; int fd; } wbuf_t;

static void wb_flush(wbuf_t *w) { if (w->n) soham_os_write(w->fd, w->buf, w->n); w->n = 0; }
static void wb_str(wbuf_t *w, const char *s) {
  while (*s) { if (w->n == sizeof w->buf) wb_flush(w); w->buf[w->n++] = *s++; }
}
static void wb_u(wbuf_t *w, uint64_t v) { char t[24]; size_t k = fmt_u(t, v); t[k] = 0; wb_str(w, t); }
static void wb_x(wbuf_t *w, uint64_t v) { char t[24]; size_t k = fmt_x(t, v); t[k] = 0; wb_str(w, t); }
/* Bytes as "12.3 MiB". */
static void wb_bytes(wbuf_t *w, size_t b) {
  const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
  int u = 0;
  uint64_t whole = b, frac = 0;
  while (whole >= 1024 && u < 4) { frac = (whole % 1024) * 10 / 1024; whole /= 1024; u++; }
  wb_u(w, whole);
  if (u) { wb_str(w, "."); wb_u(w, frac); }
  wb_str(w, " ");
  wb_str(w, unit[u]);
}

void soham_fatal(const char *msg, uintptr_t p) {
  wbuf_t w = { .n = 0, .fd = 2 };
  wb_str(&w, "sohamalloc: ");
  wb_str(&w, msg);
  wb_str(&w, " at ");
  wb_x(&w, p);
  wb_str(&w, "\n");
  wb_flush(&w);
  soham_os_abort();
}

void soham_warn(const char *msg, uintptr_t p) {
  if (!g_cfg.verbose) return;
  wbuf_t w = { .n = 0, .fd = 2 };
  wb_str(&w, "sohamalloc: warning: ");
  wb_str(&w, msg);
  wb_str(&w, " at ");
  wb_x(&w, p);
  wb_str(&w, "\n");
  wb_flush(&w);
}

void soham_lock_slow(soham_lock_t *l) {
  for (int i = 0; i < 64; i++) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || (defined(__arm__) && __ARM_ARCH >= 7)
    __asm__ __volatile__("yield");
#endif
    uint32_t c = 0;
    if (atomic_load_explicit(&l->v, memory_order_relaxed) == 0 &&
        atomic_compare_exchange_weak_explicit(&l->v, &c, 1, memory_order_acquire, memory_order_relaxed)) return;
  }
  uint32_t c = atomic_exchange_explicit(&l->v, 2, memory_order_acquire);
  while (c != 0) {
    soham_os_wait(&l->v, 2);
    c = atomic_exchange_explicit(&l->v, 2, memory_order_acquire);
  }
}

/* ---------------------------------------------------------------------- */
/* Configuration (§2 environment)                                          */
/* ---------------------------------------------------------------------- */

static bool env_is(const char *name, const char *val) {
  char buf[32];
  long n = soham_os_getenv(name, buf, sizeof buf);
  return n >= 0 && strcmp(buf, val) == 0;
}

static void config_init(void) {
  g_cfg.profile = PROFILE_BALANCED;
  g_cfg.release_mode = SOHAM_OS_RELEASE_DONTNEED;
  g_cfg.decay_ns = 1000000000ull;       /* 1 s */
  g_cfg.max_spares = 2;
  g_cfg.thp_huge = 0;
  /* Provision 4 KiB of blocks at a time (one base page on 4K systems):
   * touching more per class inflates the RSS of small programs. */
  g_cfg.prov_bytes = 4 * KiB;
  g_cfg.dirty_min = 32 * MiB;
  g_cfg.dirty_div = 8;
  g_cfg.peak_num = 4;                   /* keep a recent swing of up to 1x used */
  int secure_env = soham_os_is_secure();
  if (!secure_env) {
    if (env_is("SOHAMALLOC_PROFILE", "memory")) {
      g_cfg.profile = PROFILE_MEMORY;
      g_cfg.decay_ns = 100000000ull;    /* 100 ms */
      g_cfg.max_spares = 0;
      g_cfg.dirty_min = 8 * MiB;
      g_cfg.dirty_div = 32;
      g_cfg.peak_num = 1;               /* up to used/4 */
    } else if (env_is("SOHAMALLOC_PROFILE", "throughput")) {
      g_cfg.profile = PROFILE_THROUGHPUT;
      g_cfg.decay_ns = 10000000000ull;  /* 10 s */
      g_cfg.dirty_min = 256 * MiB;
      g_cfg.dirty_div = 2;
      g_cfg.peak_num = 8;               /* up to 2x used */
      g_cfg.release_mode = SOHAM_OS_RELEASE_FREE;
      g_cfg.thp_huge = soham_os_thp_mode() == 1;
    }
    g_cfg.stats_at_exit = env_is("SOHAMALLOC_STATS", "1");
    g_cfg.verbose = env_is("SOHAMALLOC_VERBOSE", "1");
    g_geo.secure = env_is("SOHAMALLOC_SECURE", "1");
  }
  g_cfg.purge_interval_ns = g_cfg.decay_ns / 8;
  if (g_cfg.purge_interval_ns < 5000000ull) g_cfg.purge_interval_ns = 5000000ull;
}

/* ---------------------------------------------------------------------- */
/* Initialisation (§8): lazy, once, recursion served from the bss arena    */
/* ---------------------------------------------------------------------- */

static void thread_exit_cb(void *v);
static void fork_prepare(void);
static void fork_parent(void);
static void fork_child(void);

static unsigned log2_exact(size_t v) { return (unsigned)__builtin_ctzll((unsigned long long)v); }

static size_t pow2_floor(size_t v) {
  size_t p = 1;
  while (p <= v / 2) p <<= 1;
  return p;
}

static bool geometry_init(void) {
  size_t B = soham_os_page_size();
  size_t S = B > 64 * KiB ? B : 64 * KiB;
  if (S != ((size_t)1 << SOHAM_SLICE_SHIFT)) return false;   /* rebuild with SOHAM_SLICE_SHIFT */
  g_geo.B = B;
  g_geo.S = S;
  g_geo.H = HB_SLICES * S;
#if SOHAM_64BIT
  size_t W = 4 * GiB;
  size_t Wmin = 64 * MiB;
  unsigned va_bits = 48;
#else
  size_t W = 256 * MiB;
  size_t Wmin = 64 * MiB;
  unsigned va_bits = 32;
#endif
  if (SOHAM_TSAN) W = 256 * MiB;
  size_t lim = soham_os_as_limit();
  if (lim != SIZE_MAX) {
    size_t t = pow2_floor(lim / 8);
    if (t < W) W = t < Wmin ? Wmin : t;
  }
  if (W < 16 * g_geo.H) W = 16 * g_geo.H;
  g_geo.W = W;
  g_geo.wshift = log2_exact(W);
  g_geo.wmask = W - 1;
  g_geo.wbase = ~(uintptr_t)(W - 1);
  size_t nhb_total = W / g_geo.H;
  size_t rec_off = (B + nhb_total * sizeof(hbmeta_t) + B - 1) & ~(B - 1);
  size_t rec_bytes = (W >> SOHAM_SLICE_SHIFT) * REC_SIZE;
  size_t meta = (rec_off + rec_bytes + B - 1) & ~(B - 1);
  size_t data_off = (meta + B + g_geo.H - 1) & ~(g_geo.H - 1);
  if (data_off + 2 * g_geo.H > W) return false;
  g_geo.rec_off = rec_off;
  g_geo.meta_size = meta;
  g_geo.data_off = data_off;
  g_geo.nhb_max = (uint32_t)((W - data_off) / g_geo.H - 1);   /* last hugeblock: tail guard */
  size_t gn = (size_t)1 << (va_bits - g_geo.wshift);
  void *gm = soham_os_map_rw((gn + B - 1) & ~(B - 1), B);
  if (!gm) return false;
  soham_os_advise_nohuge(gm, (gn + B - 1) & ~(B - 1));
  atomic_store_explicit(&g_geo.gmap, (_Atomic uint8_t *)gm, memory_order_relaxed);
  g_gmap_n_pending = gn;
  return true;
}

/* Returns true when the allocator is ready; false only for a recursive call
 * made by the initialising thread itself (serve it from the bss arena). */
static SOHAM_NOINLINE bool soham_init(void) {
  soham_tls_t *t = &t_tls;
  int st = atomic_load_explicit(&g_init_state, memory_order_acquire);
  if (st == 2) return true;
  if (st == 0) {
    int expect = 0;
    if (atomic_compare_exchange_strong_explicit(&g_init_state, &expect, 1, memory_order_acq_rel, memory_order_acquire)) {
      t->state = TLS_INIT;
      soham_os_random(&g_secret, sizeof g_secret);
      soham_os_random(&g_rng_seed, sizeof g_rng_seed);
      bool ok = geometry_init();
      config_init();
      if (!ok) soham_fatal("cannot initialise the address-space geometry", 0);
      classes_init(g_geo.S);
      atomic_store_explicit(&g_sentinel.r.owner, OWNER_BAD, memory_order_relaxed);
      soham_lock(&g_heap_lock);
      g_shared = heap_create_locked();
      if (g_shared) g_shared->is_shared = 1;
      soham_unlock(&g_heap_lock);
      if (!g_shared) soham_fatal("cannot create the shared heap", 0);
      /* These may allocate: the recursion lands in the bss arena. */
      soham_os_thread_key_init(thread_exit_cb);
      soham_os_atfork(fork_prepare, fork_parent, fork_child);
      /* Publish the geometry to the free fast path last: a reader that sees
       * the new mask also sees the new map and shift (acquire). */
      g_geo.amask = g_geo.secure ? ~(uintptr_t)0 : 15;
      atomic_store_explicit(&g_geo.gmask, g_gmap_n_pending - 1, memory_order_release);
      t->state = TLS_NEW;
      atomic_store_explicit(&g_init_state, 2, memory_order_release);
      return true;
    }
    st = expect;
    if (st == 2) return true;
  }
  if (t->state == TLS_INIT) return false;
  while (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2) soham_os_yield();
  return true;
}

/* ---------------------------------------------------------------------- */
/* Threads                                                                 */
/* ---------------------------------------------------------------------- */

/* Mark this thread as inside the allocator.  Every path that takes an
 * allocator lock does this first, so a signal handler that interrupts it
 * sees busy and never waits for a lock its own thread holds.  Returns false
 * when the thread already is inside (we are such a handler): the caller
 * must not take a lock then. */
SOHAM_INLINE bool busy_enter(soham_tls_t *t) {
  if (atomic_load_explicit(&t->busy, memory_order_relaxed)) return false;
  atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
  signal_fence();
  return true;
}
SOHAM_INLINE void busy_leave(soham_tls_t *t) {
  signal_fence();
  atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
}

static heap_t *heap_attach(void) {
  soham_tls_t *t = &t_tls;
  soham_lock(&g_heap_lock);
  heap_t *h = g_heap_pool;
  if (h) g_heap_pool = h->pool_next;
  else h = heap_create_locked();
  if (h) { h->live = 1; h->pool_next = NULL; h->busy_ptr = &t->busy; g_heaps_live++; }
  soham_unlock(&g_heap_lock);
  if (!h) return NULL;
  t->heap = h;
  t->tag = h->tag;
  t->state = TLS_ATTACHED;
  /* Set TLS first: pthread_setspecific may allocate (§5.3). */
  soham_os_thread_set(h);
  return h;
}

/* First allocation of a thread (busy clear on entry): initialise the
 * allocator if needed and attach a heap, with busy held throughout, since
 * both take g_heap_lock (and init is not reentrant).  Returns false when the
 * allocator is not initialised: a recursive call made by init itself. */
static SOHAM_NOINLINE bool thread_prepare(soham_tls_t *t) {
  busy_enter(t);
  bool ready = atomic_load_explicit(&g_init_state, memory_order_acquire) == 2 || soham_init();
  if (ready && !t->heap && t->state == TLS_NEW) heap_attach();
  busy_leave(t);
  return ready;
}

static void deferred_drain(soham_tls_t *t);

/* Thread exit (the OS layer's key destructor / FLS callback).  The heap
 * comes from the callback argument, not from TLS: with emulated TLS
 * (mingw-w64 emutls) the thread's TLS block may already have been destroyed
 * and replaced by a fresh zeroed one when this runs, and the heap must be
 * detached anyway (tests/regress/thread_exit_detach.c). */
static void thread_exit_cb(void *v) {
  heap_t *h = (heap_t *)v;
  if (!h || !h->live) return;
  soham_tls_t *t = &t_tls;
  bool mine = (t->heap == h);
  if (mine) deferred_drain(t);
  /* busy also when the TLS block is not h's (emulated TLS): it is still
   * this thread's, and a handler must see that we hold locks. */
  bool entered = busy_enter(t);
  heap_flush(h);
  if (mine) {
    t->heap = NULL;
    t->tag = 0;
    t->state = TLS_DEAD;
  }
  soham_lock(&g_heap_lock);
  h->live = 0;
  h->busy_ptr = NULL;
  h->pool_next = g_heap_pool;
  g_heap_pool = h;
  g_heaps_live--;
  soham_unlock(&g_heap_lock);
  if (entered) busy_leave(t);
}

/* Frees made by signal handlers that could not run in handler context. */
static void deferred_push(soham_tls_t *t, void *p) {
  *(void **)p = t->deferred;
  signal_fence();
  t->deferred = p;
}

static void deferred_drain(soham_tls_t *t) {
  while (t->deferred) {
    void *l = t->deferred;
    t->deferred = NULL;
    signal_fence();
    while (l) {
      void *nx = *(void **)l;
      soham_free(l);
      l = nx;
    }
  }
}

/* ---------------------------------------------------------------------- */
/* Allocation                                                              */
/* ---------------------------------------------------------------------- */

/* Allocate n bytes from heap h (busy held).  *zero: memory known zero. */
static void *heap_alloc(heap_t *h, size_t n, bool *zero) {
  *zero = false;
  if (n <= SOHAM_MAX_CLASS) {
    unsigned c = size_class(n);
    if (g_class_kind[c] != KIND_RUN) return bin_alloc(h, c);
    return run_alloc(g_class_size[c], false, zero);
  }
  if (n > (size_t)PTRDIFF_MAX) return NULL;
  if (n < SOHAM_SOLO_MIN) return run_alloc(n, false, zero);
  *zero = true;
  return solo_alloc(n, 0);
}

/* Every allocation that misses the fast path ends up here. */
static SOHAM_NOINLINE void *alloc_slow(size_t n, bool *zero) {
  soham_tls_t *t = &t_tls;
  *zero = false;
  if (unlikely(atomic_load_explicit(&t->busy, memory_order_relaxed))) {
    /* A signal handler interrupted the allocator on this thread: use the
     * shared heap if its lock is free, with every lock below it taken by
     * trylock only (no handler ever waits); otherwise the static arena. */
    void *p = NULL;
    if (atomic_load_explicit(&g_init_state, memory_order_acquire) == 2 && !t->trymode &&
        soham_trylock(&g_shared_lock)) {
      t->trymode = 1;
      signal_fence();
      p = heap_alloc(g_shared, n, zero);
      signal_fence();
      t->trymode = 0;
      soham_unlock(&g_shared_lock);
    }
    if (!p) { *zero = false; p = bss_alloc(n, 16); }
    if (!p) errno = ENOMEM;
    return p;
  }
  if (unlikely(!t->heap) && !thread_prepare(t)) {
    void *p = bss_alloc(n, 16);
    if (!p) errno = ENOMEM;
    return p;
  }
  if (unlikely(t->deferred != NULL)) deferred_drain(t);
  heap_t *h = t->heap;
  void *p;
  atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
  signal_fence();
  if (likely(h != NULL)) {
    p = heap_alloc(h, n, zero);
  } else {
    soham_lock(&g_shared_lock);
    p = heap_alloc(g_shared, n, zero);
    soham_unlock(&g_shared_lock);
  }
  signal_fence();
  atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
  if (unlikely(!p)) errno = ENOMEM;
  return p;
}

static SOHAM_NOINLINE void *malloc_slow(size_t n) {
  bool zero;
  return alloc_slow(n, &zero);
}

SOHAM_API void *soham_malloc(size_t n) {
  soham_tls_t *t = &t_tls;
  heap_t *h = t->heap;
  if (likely(h != NULL && n <= SOHAM_FAST_MAX && !atomic_load_explicit(&t->busy, memory_order_relaxed))) {
    unsigned c = likely(n <= SOHAM_LUT_MAX) ? g_lut[(n + 15) >> 4] : size_class_large(n);
    atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
    signal_fence();
    rec_t *r = h->bins[c].cur;
    uintptr_t b = o_head(r);
    if (likely(b != 0)) {
      uintptr_t nx = link_dec(r->o.key, *(uintptr_t *)b, b);
      if (likely(nx - r->o.pbase < r->o.psize || nx == 0)) {
        o_set(r, nx, o_cnt(r) - 1);
        signal_fence();
        atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
        return (void *)b;
      }
      soham_fatal("heap corruption: free-list link overwritten", b);
    }
    signal_fence();
    atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
  }
  return malloc_slow(n);
}

SOHAM_API void *soham_calloc(size_t count, size_t n) {
  size_t total;
  if (unlikely(__builtin_mul_overflow(count, n, &total) || total > (size_t)PTRDIFF_MAX)) {
    errno = ENOMEM;
    return NULL;
  }
  if (total <= SOHAM_FAST_MAX) {
    void *p = soham_malloc(total);
    if (p) memset(p, 0, total);
    return p;
  }
  bool zero;
  void *p = alloc_slow(total, &zero);
  if (p && !zero) memset(p, 0, total);
  return p;
}

SOHAM_API void *soham_malloc_at_least(size_t n, size_t *usable) {
  void *p = soham_malloc(n);
  if (usable) *usable = p ? soham_usable_size(p) : 0;
  return p;
}

/* ---------------------------------------------------------------------- */
/* Free                                                                    */
/* ---------------------------------------------------------------------- */

static SOHAM_COLD void invalid_free(uintptr_t a, const char *why) { soham_fatal(why, a); }

static void free_nonarena(void *p, uintptr_t a, soham_tls_t *t, bool in_handler) {
  if (!p) return;
  if (in_bss_arena(a)) { atomic_fetch_add_explicit(&g_bss_freed, 1, memory_order_relaxed); return; }
  if (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2) {
    soham_warn("free of a pointer not allocated by sohamalloc (before init); leaked", a);
    return;
  }
  if (in_handler) { deferred_push(t, p); return; }
  atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
  signal_fence();
  bool was_solo = solo_free(a);
  signal_fence();
  atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
  if (was_solo) return;
  if (g_geo.secure) invalid_free(a, "free of a pointer not allocated by sohamalloc");
  soham_warn("free of a pointer not allocated by sohamalloc; leaked", a);
}

static SOHAM_NOINLINE void free_slow(void *p) {
  uintptr_t a = (uintptr_t)p & SOHAM_ADDR_MASK;
  soham_tls_t *t = &t_tls;
  bool in_handler = atomic_load_explicit(&t->busy, memory_order_relaxed) != 0;
  if (!in_window(a)) { free_nonarena(p, a, t, in_handler); return; }
  rec_t *r = rec_of(a);
  rec_t *rh = rec_head(r);
  uint8_t kind = rh->r.kind;
  if (unlikely(kind == KIND_NONE)) invalid_free(a, "free of an invalid pointer (not an allocated block)");
  if (unlikely(a & 15)) invalid_free(a, "free of a misaligned pointer");
  if (kind == KIND_RUN) {
    if (unlikely(a != rh->r.pbase)) invalid_free(a, "free of an interior pointer");
    if (in_handler) { deferred_push(t, p); return; }
    atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
    signal_fence();
    run_free(rh);
    signal_fence();
    atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
    return;
  }
  if (g_geo.secure) {
    if (unlikely(!block_ok(rh, a) || a >= rh->o.bump)) invalid_free(a, "free of an interior or invalid pointer");
    if (rh->r.bsize <= 1024) memset((void *)a, 0, rh->r.bsize);
  }
  if (!in_handler) {
    atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
    signal_fence();
    heap_t *h = t->heap;
    if (h && atomic_load_explicit(&rh->r.owner, memory_order_relaxed) == t->tag) {
      uintptr_t head = o_head(rh);
      if (unlikely(head == a)) soham_fatal("double free detected", a);
      *(uintptr_t *)a = link_enc(rh->o.key, head, a);
      uint32_t cnt = o_cnt(rh) + 1;
      o_set(rh, a, cnt);
      if (cnt == rh->o.event_at) page_local_event(h, rh, cnt);
    } else {
      remote_free(rh, a, false);
    }
    signal_fence();
    atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
  } else {
    /* Handler context: the lock-free remote path is always safe (§5.3). */
    remote_free(rh, a, true);
  }
}

SOHAM_API void soham_free(void *p) {
  uintptr_t a = (uintptr_t)p & SOHAM_ADDR_MASK;
  uintptr_t g = (a >> g_geo.wshift) & atomic_load_explicit(&g_geo.gmask, memory_order_acquire);
  soham_tls_t *t = &t_tls;
  if (likely(atomic_load_explicit(&atomic_load_explicit(&g_geo.gmap, memory_order_relaxed)[g], memory_order_relaxed) == GM_WINDOW &&
             (a & g_geo.amask) == 0 && t->heap != NULL &&
             !atomic_load_explicit(&t->busy, memory_order_relaxed))) {
    rec_t *r = rec_of(a);
    atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
    signal_fence();
    /* Load head_delta and owner in parallel: single-slice pages (the common
     * case) need no second dependent load. */
    uint32_t hd = r->r.head_delta;
    uint32_t own = atomic_load_explicit(&r->r.owner, memory_order_relaxed);
    rec_t *rh = r;
    if (unlikely(hd != 0)) { rh = (rec_t *)((char *)r - hd); own = atomic_load_explicit(&rh->r.owner, memory_order_relaxed); }
    if (likely(own == t->tag)) {
      uintptr_t head = o_head(rh);
      if (unlikely(head == a)) soham_fatal("double free detected", a);
      *(uintptr_t *)a = link_enc(rh->o.key, head, a);
      uint32_t cnt = o_cnt(rh) + 1;
      o_set(rh, a, cnt);
      if (unlikely(cnt == rh->o.event_at)) page_local_event(t->heap, rh, cnt);
      signal_fence();
      atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
      return;
    }
    signal_fence();
    atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
  }
  free_slow(p);
}

/* ---------------------------------------------------------------------- */
/* Sizes, realloc, aligned allocation                                      */
/* ---------------------------------------------------------------------- */

SOHAM_API size_t soham_usable_size(const void *p) {
  uintptr_t a = (uintptr_t)p & SOHAM_ADDR_MASK;
  if (!p) return 0;
  if (in_window(a)) {
    rec_t *r = rec_of(a);
    rec_t *rh = rec_head(r);
    if (rh->r.kind == KIND_RUN) return rh->r.psize - (a - rh->r.pbase);
    if (rh->r.kind == KIND_NONE) return 0;
    return rh->r.bsize;
  }
  if (in_bss_arena(a)) return bss_usable(a);
  if (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2) return 0;
  return solo_usable(a);
}

SOHAM_API void *soham_realloc(void *p, size_t n) {
  if (!p) return soham_malloc(n);
  if (n == 0) { soham_free(p); return NULL; }
  uintptr_t a = (uintptr_t)p & SOHAM_ADDR_MASK;
  size_t us;
  if (in_window(a)) {
    rec_t *r = rec_of(a);
    rec_t *rh = rec_head(r);
    if (rh->r.kind == KIND_NONE) invalid_free(a, "realloc of an invalid pointer");
    us = rh->r.kind == KIND_RUN ? rh->r.psize : rh->r.bsize;
    /* Keep the block when it still fits and would not waste over half. */
    if (n <= us && (n >= us / 2 || us <= 16)) return p;
  } else if (in_bss_arena(a)) {
    us = bss_usable(a);
  } else {
    us = atomic_load_explicit(&g_init_state, memory_order_acquire) == 2 ? solo_usable(a) : 0;
    if (us == 0) {
      if (g_geo.secure) invalid_free(a, "realloc of a pointer not allocated by sohamalloc");
      return NULL;
    }
    if (n >= SOHAM_SOLO_MIN && n <= (size_t)PTRDIFF_MAX && !atomic_load_explicit(&t_tls.busy, memory_order_relaxed)) {
      if (n <= us && n >= us / 2) return p;
      void *q = solo_realloc(a, n);
      if (q) return q;
    }
  }
  void *q = soham_malloc(n);
  if (!q) return NULL;
  memcpy(q, p, us < n ? us : n);
  soham_free(p);
  return q;
}

SOHAM_API void *soham_reallocarray(void *p, size_t count, size_t n) {
  size_t total;
  if (__builtin_mul_overflow(count, n, &total)) { errno = ENOMEM; return NULL; }
  return soham_realloc(p, total);
}

/* Allocation with alignment align (a power of two). */
static void *alloc_aligned(size_t align, size_t n) {
  if (align <= 16) return soham_malloc(n);
  if (n == 0) n = 1;
  if (n > (size_t)PTRDIFF_MAX) { errno = ENOMEM; return NULL; }
  soham_tls_t *t = &t_tls;
  if (unlikely(atomic_load_explicit(&t->busy, memory_order_relaxed)) ||
      (unlikely(!t->heap) && !thread_prepare(t))) {
    void *p = bss_alloc(n, align);
    if (!p) errno = ENOMEM;
    return p;
  }
  /* Size classes whose block size is a multiple of align are aligned,
   * because pages start on slice boundaries (plan §5.7.4). */
  if (align <= g_geo.S && n <= SOHAM_MAX_CLASS) {
    size_t want = n < align ? align : n;
    for (unsigned c = size_class(want); c < SOHAM_NCLASSES; c++) {
      if (g_class_size[c] % align == 0) {
        if (g_class_kind[c] == KIND_RUN) break;
        void *p = soham_malloc(g_class_size[c]);
        SOHAM_ASSERT(!p || ((uintptr_t)p & (align - 1)) == 0);
        return p;
      }
    }
  }
  bool zero;
  void *p = NULL;
  if (t->deferred) deferred_drain(t);
  atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
  signal_fence();
  if (align <= g_geo.H && n < SOHAM_SOLO_MIN) p = run_alloc(n, align > g_geo.S, &zero);
  else p = solo_alloc(n, align);
  signal_fence();
  atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
  if (!p) errno = ENOMEM;
  return p;
}

SOHAM_API void *soham_aligned_alloc(size_t align, size_t n) {
  if (align == 0 || (align & (align - 1))) { errno = EINVAL; return NULL; }
  return alloc_aligned(align, n);
}

SOHAM_API int soham_posix_memalign(void **out, size_t align, size_t n) {
  if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
  int e = errno;
  void *p = alloc_aligned(align, n);
  if (!p) { errno = e; return ENOMEM; }
  *out = p;
  return 0;
}

SOHAM_API void *soham_memalign(size_t align, size_t n) {
  if (align & (align - 1)) {
    size_t a = 1;
    while (a < align && a) a <<= 1;
    if (!a) { errno = EINVAL; return NULL; }
    align = a;
  }
  return alloc_aligned(align, n);
}

SOHAM_API void soham_free_sized(void *p, size_t n) {
  if (p && g_geo.secure) {
    size_t us = soham_usable_size(p);
    if (us && n > us) invalid_free((uintptr_t)p, "free_sized with a size larger than the block");
  }
  soham_free(p);
}

SOHAM_API void soham_free_aligned_sized(void *p, size_t align, size_t n) {
  (void)align;
  soham_free_sized(p, n);
}

/* ---------------------------------------------------------------------- */
/* Collection, statistics, consistency checks                              */
/* ---------------------------------------------------------------------- */

SOHAM_API void soham_collect(int force) {
  if (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2) return;
  soham_tls_t *t = &t_tls;
  if (atomic_load_explicit(&t->busy, memory_order_relaxed)) return;
  if (t->deferred) deferred_drain(t);
  atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
  signal_fence();
  pending_drain();
  if (force && t->heap) heap_flush(t->heap);
  ph_purge(force ? PURGE_ALL : PURGE_DECAY);
  signal_fence();
  atomic_store_explicit(&t->busy, 0, memory_order_relaxed);
}

typedef struct {
  size_t in_use, cached_heaps, cached_pools, tail_waste, pages, runs;
} walk_t;

/* Racy snapshot of every page (values are read without the owners' locks). */
static SOHAM_RACY_SNAPSHOT void walk_pages(walk_t *wk) {
  for (window_t *w = g_ph.windows; w; w = w->next) {
    for (uint32_t i = 0; i < w->nhb; i++) {
      hbmeta_t *hb = &w->hb[i];
      uint32_t used = hb->used;
      while (used) {
        unsigned s = (unsigned)__builtin_ctz(used);
        used &= used - 1;
        uintptr_t a = hb_addr(hb) + ((uintptr_t)s << SOHAM_SLICE_SHIFT);
        rec_t *r = rec_of(a);
        if (r->r.head_delta != 0) continue;
        if (r->r.kind == KIND_RUN) { wk->in_use += r->r.psize; wk->runs++; continue; }
        if (r->r.kind != KIND_SMALL && r->r.kind != KIND_MEDIUM) continue;
        wk->pages++;
        uint32_t cap = r->r.cap, bs = r->r.bsize;
        uint32_t owner = atomic_load_explicit(&r->r.owner, memory_order_relaxed);
        uint64_t x = atomic_load_explicit(&r->x.xfree, memory_order_relaxed);
        uint32_t freeb;
        if (x & (XF_ORPHAN | XF_QUEUED)) freeb = xf_cnt(x);
        else freeb = o_cnt(r) + xf_cnt(x) + (cap - r->o.prov);
        if (freeb > cap) freeb = cap;
        wk->in_use += (size_t)(cap - freeb) * bs;
        if (owner >= 1 && owner <= SOHAM_MAX_TAG) wk->cached_heaps += (size_t)freeb * bs;
        else wk->cached_pools += (size_t)freeb * bs;
        wk->tail_waste += r->r.psize - (size_t)cap * bs;
      }
    }
  }
}

SOHAM_API int soham_get_stats(struct soham_stats *out) {
  if (!out) return -1;
  memset(out, 0, sizeof *out);
  if (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2) return 0;
  soham_tls_t *t = &t_tls;
  if (!busy_enter(t)) return -1;     /* called from a handler inside the allocator */
  walk_t wk = { 0 };
  soham_lock(&g_ph.lock);
  walk_pages(&wk);
  size_t S = g_geo.S;
  out->committed = g_ph.committed_data + g_ph.committed_meta;
  out->pageheap_free = g_ph.s_dirty * S;
  out->released = (g_ph.s_zero + g_ph.s_lazy) * S;
  out->resident_est = (g_ph.s_used + g_ph.s_dirty + g_ph.s_releasing) * S;
  out->windows = g_ph.nwindows;
  soham_unlock(&g_ph.lock);
  out->in_use = wk.in_use;
  out->cached_heaps = wk.cached_heaps;
  out->cached_pools = wk.cached_pools + wk.tail_waste;
  soham_lock(&g_meta_lock);
  out->metadata = g_meta_total;
  soham_unlock(&g_meta_lock);
  /* Committed metadata is mostly untouched; count the records of used
   * slices and the hugeblock table as the resident part. */
  out->metadata += g_ph.nwindows * (size_t)g_geo.nhb_max * sizeof(hbmeta_t) +
                   (g_ph.s_used + g_ph.s_dirty) * REC_SIZE;
  soham_lock(&g_solo_lock);
  out->solo = g_solo_bytes;
  soham_unlock(&g_solo_lock);
  out->committed += out->solo;
  out->resident_est += out->solo + out->metadata;
  soham_lock(&g_heap_lock);
  out->heaps_live = g_heaps_live;
  size_t pooled = 0;
  for (heap_t *h = g_heap_pool; h; h = h->pool_next) pooled++;
  out->heaps_pooled = pooled;
  soham_unlock(&g_heap_lock);
  busy_leave(t);
  return 0;
}

SOHAM_API void soham_stats_print(int fd) {
  struct soham_stats s;
  if (soham_get_stats(&s) != 0) return;
  wbuf_t w = { .n = 0, .fd = fd };
  wb_str(&w, "sohamalloc " SOHAMALLOC_VERSION " -- memory report\n");
  wb_str(&w, "  resident (est.)     "); wb_bytes(&w, s.resident_est); wb_str(&w, "\n");
  wb_str(&w, "    in use by app     "); wb_bytes(&w, s.in_use); wb_str(&w, "\n");
  wb_str(&w, "    cached in heaps   "); wb_bytes(&w, s.cached_heaps); wb_str(&w, "   (free blocks in thread-owned pages)\n");
  wb_str(&w, "    cached in pools   "); wb_bytes(&w, s.cached_pools); wb_str(&w, "   (free blocks in shared pages, page tails)\n");
  wb_str(&w, "    page-heap free    "); wb_bytes(&w, s.pageheap_free); wb_str(&w, "   (free, resident, released after decay)\n");
  wb_str(&w, "    metadata          "); wb_bytes(&w, s.metadata); wb_str(&w, "\n");
  wb_str(&w, "    large mappings    "); wb_bytes(&w, s.solo); wb_str(&w, "\n");
  wb_str(&w, "  released to OS      "); wb_bytes(&w, s.released); wb_str(&w, "   (committed, not resident)\n");
  wb_str(&w, "  committed           "); wb_bytes(&w, s.committed); wb_str(&w, "\n");
  wb_str(&w, "  windows "); wb_u(&w, s.windows);
  wb_str(&w, ", heaps live "); wb_u(&w, s.heaps_live);
  wb_str(&w, ", pooled "); wb_u(&w, s.heaps_pooled); wb_str(&w, "\n");
  wb_str(&w, "  profile ");
  wb_str(&w, g_cfg.profile == PROFILE_MEMORY ? "memory" : g_cfg.profile == PROFILE_THROUGHPUT ? "throughput" : "balanced");
  wb_str(&w, ", security S"); wb_u(&w, g_geo.secure ? 1 : 0);
  wb_str(&w, ", release via "); wb_str(&w, soham_os_release_method()); wb_str(&w, "\n");
  wb_flush(&w);
}

/* Verify the invariants that can be checked without stopping other
 * threads: the page heap's bitmaps and buckets (stable under its lock) and,
 * for pages owned by the calling thread, the page geometry and the full
 * local free list.  Pages of other threads are skipped: their records are
 * written outside the page-heap lock.  Returns the error count. */
SOHAM_API int soham_check_heap(void) {
  if (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2) return 0;
  soham_tls_t *t = &t_tls;
  if (!busy_enter(t)) return 0;      /* called from a handler inside the allocator */
  int errors = 0;
  soham_lock(&g_ph.lock);
  for (window_t *w = g_ph.windows; w; w = w->next) {
    for (uint32_t i = 0; i < w->nhb; i++) {
      hbmeta_t *hb = &w->hb[i];
      if (hb->used & hb->releasing) errors++;
      if ((hb->dirty | hb->zero) & (hb->used | hb->releasing)) errors++;
      if (hb->longest != longest_run(hb_free_mask(hb))) errors++;
      if (!t->heap) continue;
      uint32_t used = hb->used;
      while (used) {
        unsigned s = (unsigned)__builtin_ctz(used);
        used &= used - 1;
        rec_t *r = rec_of(hb_addr(hb) + ((uintptr_t)s << SOHAM_SLICE_SHIFT));
        if (atomic_load_explicit(&r->r.owner, memory_order_relaxed) != t->tag) continue;
        if (r->r.head_delta) continue;
        if (r->r.kind != KIND_SMALL && r->r.kind != KIND_MEDIUM) { errors++; continue; }
        if (r->r.cap == 0 || (size_t)r->r.cap * r->r.bsize > r->r.psize) { errors++; continue; }
        uint32_t n = o_cnt(r), k = 0;
        uintptr_t p = o_head(r);
        while (p && k <= n) {
          if (!block_ok(r, p) || p >= r->o.bump) { errors++; break; }
          p = link_dec(r->r.key, *(uintptr_t *)p, p);
          k++;
        }
        if (k != n || n > r->o.prov) errors++;
      }
    }
  }
  soham_unlock(&g_ph.lock);
  busy_leave(t);
  return errors;
}

SOHAM_API const char *soham_version(void) { return SOHAMALLOC_VERSION; }

/* ---------------------------------------------------------------------- */
/* Fork (§8)                                                               */
/* ---------------------------------------------------------------------- */

static uint8_t g_fork_saved_busy;

static void fork_prepare(void) {
  soham_tls_t *t = &t_tls;
  g_fork_saved_busy = atomic_load_explicit(&t->busy, memory_order_relaxed);
  atomic_store_explicit(&t->busy, 1, memory_order_relaxed);
  signal_fence();
  soham_lock(&g_heap_lock);
  soham_lock(&g_shared_lock);
  for (unsigned c = 0; c < SOHAM_NCLASSES; c++) soham_lock(&g_pools[c].lock);
  soham_lock(&g_ph.lock);
  soham_lock(&g_solo_lock);
  soham_lock(&g_meta_lock);
}

static void fork_parent(void) {
  soham_unlock(&g_meta_lock);
  soham_unlock(&g_solo_lock);
  soham_unlock(&g_ph.lock);
  for (unsigned c = SOHAM_NCLASSES; c-- > 0;) soham_unlock(&g_pools[c].lock);
  soham_unlock(&g_shared_lock);
  soham_unlock(&g_heap_lock);
  signal_fence();
  atomic_store_explicit(&t_tls.busy, g_fork_saved_busy, memory_order_relaxed);
}

static void fork_child(void) {
  soham_tls_t *t = &t_tls;
  soham_lock_reset(&g_meta_lock);
  soham_lock_reset(&g_solo_lock);
  soham_lock_reset(&g_ph.lock);
  for (unsigned c = 0; c < SOHAM_NCLASSES; c++) soham_lock_reset(&g_pools[c].lock);
  soham_lock_reset(&g_shared_lock);
  soham_lock_reset(&g_heap_lock);
  /* Other threads do not exist in the child.  Their heaps go to the pool
   * as-is (no page metadata is touched: copy-on-write friendly); a heap
   * that was inside an operation is leaked (§8). */
  uint32_t live = 0;
  for (heap_t *h = g_heap_all; h; h = h->all_next) {
    if (!h->live || h == t->heap) { if (h->live) live++; continue; }
    uint8_t b = h->busy_ptr ? atomic_load_explicit(h->busy_ptr, memory_order_relaxed) : 1;
    h->live = 0;
    h->busy_ptr = NULL;
    if (!b) { h->pool_next = g_heap_pool; g_heap_pool = h; }
  }
  g_heaps_live = live;
  uint64_t r;
  soham_os_random(&r, sizeof r);
  g_rng_seed ^= r;
  if (t->heap) t->heap->rng ^= mix64(r) | 1;
  if (t->heap) t->heap->busy_ptr = &t->busy;
  signal_fence();
  atomic_store_explicit(&t->busy, g_fork_saved_busy, memory_order_relaxed);
}

/* ---------------------------------------------------------------------- */
/* Internals for property tests (src/internal_test.h)                     */
/* ---------------------------------------------------------------------- */

static void ensure_init(void) {
  if (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2) soham_init();
}

SOHAM_API unsigned soham_internal_size_class(size_t n) { ensure_init(); return size_class(n); }
SOHAM_API size_t soham_internal_class_size(unsigned c) { ensure_init(); return c < SOHAM_NCLASSES ? g_class_size[c] : 0; }
SOHAM_API unsigned soham_internal_class_kind(unsigned c) { ensure_init(); return c < SOHAM_NCLASSES ? g_class_kind[c] : 0; }
SOHAM_API size_t soham_internal_class_page_size(unsigned c) { ensure_init(); return c < SOHAM_NCLASSES ? g_class_page[c] : 0; }
SOHAM_API unsigned soham_internal_class_cap(unsigned c) { ensure_init(); return c < SOHAM_NCLASSES ? g_class_cap[c] : 0; }
SOHAM_API uintptr_t soham_internal_link_enc(uintptr_t key, uintptr_t next, uintptr_t addr) { return link_enc(key, next, addr); }
SOHAM_API uintptr_t soham_internal_link_dec(uintptr_t key, uintptr_t stored, uintptr_t addr) { return link_dec(key, stored, addr); }
SOHAM_API int soham_internal_security_tier(void) { ensure_init(); return g_geo.secure ? 1 : 0; }

SOHAM_API void soham_internal_geometry(struct soham_internal_geometry *out) {
  ensure_init();
  out->base_page = g_geo.B;
  out->slice = g_geo.S;
  out->hugeblock = g_geo.H;
  out->window = g_geo.W;
  out->wshift = g_geo.wshift;
  out->rec_off = g_geo.rec_off;
  out->data_off = g_geo.data_off;
  out->record_size = REC_SIZE;
  out->solo_threshold = SOHAM_SOLO_MIN;
}

/* ---------------------------------------------------------------------- */
/* Exit report                                                             */
/* ---------------------------------------------------------------------- */

#if !defined(_WIN32)
__attribute__((destructor)) static void soham_exit_report(void) {
  if (g_cfg.stats_at_exit && atomic_load_explicit(&g_init_state, memory_order_acquire) == 2) soham_stats_print(2);
}
#endif

/* ---------------------------------------------------------------------- */
/* Overrides of the system allocator (§2)                                  */
/* ---------------------------------------------------------------------- */

#if SOHAM_OVERRIDE

#define SOHAM_ALIAS(name, target) __typeof__(target) name __attribute__((alias(#target), visibility("default"), used))

SOHAM_ALIAS(malloc, soham_malloc);
SOHAM_ALIAS(free, soham_free);
SOHAM_ALIAS(calloc, soham_calloc);
SOHAM_ALIAS(realloc, soham_realloc);
SOHAM_ALIAS(reallocarray, soham_reallocarray);
SOHAM_ALIAS(aligned_alloc, soham_aligned_alloc);
SOHAM_ALIAS(posix_memalign, soham_posix_memalign);
SOHAM_ALIAS(memalign, soham_memalign);
SOHAM_ALIAS(malloc_usable_size, soham_usable_size);
SOHAM_ALIAS(free_sized, soham_free_sized);
SOHAM_ALIAS(free_aligned_sized, soham_free_aligned_sized);
SOHAM_ALIAS(__libc_malloc, soham_malloc);
SOHAM_ALIAS(__libc_free, soham_free);
SOHAM_ALIAS(__libc_calloc, soham_calloc);
SOHAM_ALIAS(__libc_realloc, soham_realloc);
SOHAM_ALIAS(__libc_memalign, soham_memalign);
SOHAM_ALIAS(cfree, soham_free);

SOHAM_API void *valloc(size_t n) { return soham_memalign(g_geo.B ? g_geo.B : soham_os_page_size(), n); }
SOHAM_API void *pvalloc(size_t n) {
  size_t B = g_geo.B ? g_geo.B : soham_os_page_size();
  if (n > SIZE_MAX - B) { errno = ENOMEM; return NULL; }
  n = (n + B - 1) & ~(B - 1);
  return soham_memalign(B, n ? n : B);
}
SOHAM_ALIAS(__libc_valloc, valloc);
SOHAM_ALIAS(__libc_pvalloc, pvalloc);

SOHAM_API int malloc_trim(size_t pad) { (void)pad; soham_collect(1); return 1; }
SOHAM_API int mallopt(int param, int value) { (void)param; (void)value; return 1; }
SOHAM_API void malloc_stats(void) { soham_stats_print(2); }

/* glibc <malloc.h> layouts, declared here to avoid including malloc.h. */
struct soham_mallinfo2 { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
struct soham_mallinfo { int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };

static struct soham_mallinfo2 soham_mallinfo2_impl(void) {
  struct soham_stats s;
  soham_get_stats(&s);
  struct soham_mallinfo2 m = { 0 };
  m.arena = s.committed - s.solo;
  m.hblks = 0;
  m.hblkhd = s.solo;
  m.uordblks = s.in_use;
  m.fordblks = s.cached_heaps + s.cached_pools + s.pageheap_free;
  m.keepcost = s.pageheap_free;
  return m;
}
SOHAM_API struct soham_mallinfo2 mallinfo2(void) { return soham_mallinfo2_impl(); }
SOHAM_API struct soham_mallinfo mallinfo(void) {
  struct soham_mallinfo2 m2 = soham_mallinfo2_impl();
  struct soham_mallinfo m;
#define CLAMPI(v) ((v) > (size_t)INT_MAX ? INT_MAX : (int)(v))
  m.arena = CLAMPI(m2.arena); m.ordblks = CLAMPI(m2.ordblks); m.smblks = CLAMPI(m2.smblks);
  m.hblks = CLAMPI(m2.hblks); m.hblkhd = CLAMPI(m2.hblkhd); m.usmblks = CLAMPI(m2.usmblks);
  m.fsmblks = CLAMPI(m2.fsmblks); m.uordblks = CLAMPI(m2.uordblks); m.fordblks = CLAMPI(m2.fordblks);
  m.keepcost = CLAMPI(m2.keepcost);
#undef CLAMPI
  return m;
}


/* ---------------------------------------------------------------------- */
/* C++ operator new/delete, written in C (§2)                              */
/* ---------------------------------------------------------------------- */

/* The operators are defined under their Itanium-ABI mangled names, so the
 * library needs no C++ runtime: a C program that preloads sohamalloc does
 * not load libstdc++ (about 1.6 MiB of RSS and startup time).  The two C++
 * runtime services operator new needs are weak references, resolved at run
 * time in C++ programs (libstdc++; libc++ provides get_new_handler):
 *   std::get_new_handler()   _ZSt15get_new_handlerv
 *   std::__throw_bad_alloc() _ZSt17__throw_bad_allocv
 * The library is built with unwind tables, so bad_alloc propagates through
 * these frames.  Without a runtime (impossible in a real C++ program) an
 * allocation failure in throwing new aborts with a diagnostic. */
#if SOHAM_CXX_OPERATORS
#include <dlfcn.h>

#if SIZE_MAX > 0xffffffffu
#  define SOHAM_SZ "m"     /* size_t is unsigned long */
#else
#  define SOHAM_SZ "j"     /* size_t is unsigned int */
#endif

typedef void (*soham_new_handler_t)(void);
extern soham_new_handler_t _ZSt15get_new_handlerv(void) __attribute__((weak));
extern void _ZSt17__throw_bad_allocv(void) __attribute__((weak, noreturn));

static SOHAM_NOINLINE void *cxx_new_retry(size_t n, size_t align) {
  for (;;) {
    void *p = align ? soham_aligned_alloc(align, n ? n : 1) : soham_malloc(n);
    if (p) return p;
    soham_new_handler_t h = _ZSt15get_new_handlerv ? _ZSt15get_new_handlerv() : NULL;
    if (!h) {
      if (_ZSt17__throw_bad_allocv) _ZSt17__throw_bad_allocv();
      soham_fatal("out of memory in operator new (no C++ runtime to throw std::bad_alloc)", (uintptr_t)n);
    }
    h();   /* may throw: the C frames have unwind tables */
  }
}

/* nothrow new must return NULL if the new_handler throws, which needs a
   catch that C cannot write. The C++ runtime's own nothrow new is exactly
   "try { return ::operator new(n); } catch (...) { return nullptr; }", and
   its ::operator new resolves to ours, so on failure with a handler
   installed we delegate to it (found once with RTLD_NEXT). Without a
   handler, or without a runtime definition, plain NULL is correct. */
typedef void *(*nt_new_fn)(size_t, const void *);
typedef void *(*nt_new_al_fn)(size_t, size_t, const void *);
static _Atomic(void *) g_nt_next[4];
static const char *const g_nt_names[4] = {
  "_Znw" SOHAM_SZ "RKSt9nothrow_t", "_Zna" SOHAM_SZ "RKSt9nothrow_t",
  "_Znw" SOHAM_SZ "St11align_val_tRKSt9nothrow_t", "_Zna" SOHAM_SZ "St11align_val_tRKSt9nothrow_t" };

static SOHAM_NOINLINE void *cxx_new_nothrow_slow(int which, size_t n, size_t al, const void *nt) {
  if (!_ZSt15get_new_handlerv || !_ZSt15get_new_handlerv()) return NULL;
  void *f = atomic_load_explicit(&g_nt_next[which], memory_order_acquire);
  if (!f) {
    f = dlsym(RTLD_NEXT, g_nt_names[which]);
    if (!f) return NULL;
    atomic_store_explicit(&g_nt_next[which], f, memory_order_release);
  }
  return which < 2 ? ((nt_new_fn)f)(n, nt) : ((nt_new_al_fn)f)(n, al, nt);
}

SOHAM_INLINE void *cxx_new(size_t n) {
  void *p = soham_malloc(n);
  return likely(p != NULL) ? p : cxx_new_retry(n, 0);
}
SOHAM_INLINE void *cxx_new_aligned(size_t n, size_t al) {
  void *p = soham_aligned_alloc(al, n ? n : 1);
  return likely(p != NULL) ? p : cxx_new_retry(n, al);
}

#define SOHAM_CXX(ret, name, mangled, params) \
  SOHAM_API ret name params __asm__(mangled); SOHAM_API ret name params

SOHAM_CXX(void *, soham_op_new, "_Znw" SOHAM_SZ, (size_t n)) { return cxx_new(n); }
SOHAM_CXX(void *, soham_op_new_a, "_Zna" SOHAM_SZ, (size_t n)) { return cxx_new(n); }
SOHAM_CXX(void *, soham_op_new_nt, "_Znw" SOHAM_SZ "RKSt9nothrow_t", (size_t n, const void *nt)) {
  void *p = soham_malloc(n); return p ? p : cxx_new_nothrow_slow(0, n, 0, nt);
}
SOHAM_CXX(void *, soham_op_new_a_nt, "_Zna" SOHAM_SZ "RKSt9nothrow_t", (size_t n, const void *nt)) {
  void *p = soham_malloc(n); return p ? p : cxx_new_nothrow_slow(1, n, 0, nt);
}
SOHAM_CXX(void *, soham_op_new_al, "_Znw" SOHAM_SZ "St11align_val_t", (size_t n, size_t al)) { return cxx_new_aligned(n, al); }
SOHAM_CXX(void *, soham_op_new_a_al, "_Zna" SOHAM_SZ "St11align_val_t", (size_t n, size_t al)) { return cxx_new_aligned(n, al); }
SOHAM_CXX(void *, soham_op_new_al_nt, "_Znw" SOHAM_SZ "St11align_val_tRKSt9nothrow_t", (size_t n, size_t al, const void *nt)) {
  void *p = soham_aligned_alloc(al, n ? n : 1); return p ? p : cxx_new_nothrow_slow(2, n, al, nt);
}
SOHAM_CXX(void *, soham_op_new_a_al_nt, "_Zna" SOHAM_SZ "St11align_val_tRKSt9nothrow_t", (size_t n, size_t al, const void *nt)) {
  void *p = soham_aligned_alloc(al, n ? n : 1); return p ? p : cxx_new_nothrow_slow(3, n, al, nt);
}

SOHAM_CXX(void, soham_op_del, "_ZdlPv", (void *p)) { soham_free(p); }
SOHAM_CXX(void, soham_op_del_a, "_ZdaPv", (void *p)) { soham_free(p); }
SOHAM_CXX(void, soham_op_del_nt, "_ZdlPvRKSt9nothrow_t", (void *p, const void *nt)) { (void)nt; soham_free(p); }
SOHAM_CXX(void, soham_op_del_a_nt, "_ZdaPvRKSt9nothrow_t", (void *p, const void *nt)) { (void)nt; soham_free(p); }
SOHAM_CXX(void, soham_op_del_sz, "_ZdlPv" SOHAM_SZ, (void *p, size_t n)) { soham_free_sized(p, n); }
SOHAM_CXX(void, soham_op_del_a_sz, "_ZdaPv" SOHAM_SZ, (void *p, size_t n)) { soham_free_sized(p, n); }
SOHAM_CXX(void, soham_op_del_al, "_ZdlPvSt11align_val_t", (void *p, size_t al)) { (void)al; soham_free(p); }
SOHAM_CXX(void, soham_op_del_a_al, "_ZdaPvSt11align_val_t", (void *p, size_t al)) { (void)al; soham_free(p); }
SOHAM_CXX(void, soham_op_del_al_nt, "_ZdlPvSt11align_val_tRKSt9nothrow_t", (void *p, size_t al, const void *nt)) { (void)al; (void)nt; soham_free(p); }
SOHAM_CXX(void, soham_op_del_a_al_nt, "_ZdaPvSt11align_val_tRKSt9nothrow_t", (void *p, size_t al, const void *nt)) { (void)al; (void)nt; soham_free(p); }
SOHAM_CXX(void, soham_op_del_sz_al, "_ZdlPv" SOHAM_SZ "St11align_val_t", (void *p, size_t n, size_t al)) { soham_free_aligned_sized(p, al, n); }
SOHAM_CXX(void, soham_op_del_a_sz_al, "_ZdaPv" SOHAM_SZ "St11align_val_t", (void *p, size_t n, size_t al)) { soham_free_aligned_sized(p, al, n); }

#endif /* SOHAM_CXX_OPERATORS */

#endif /* SOHAM_OVERRIDE */
