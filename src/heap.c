/*
 * src/heap.c -- thread heaps, pages, pools, remote frees, runs and solos.
 * Part of the sohamalloc unity build (included by src/sohamalloc.c).
 *
 * Ownership model (§5):
 *  - A heap is held by exactly one thread at a time: its running thread,
 *    or a thread holding it exclusively (heap pool, fork child).  Only the
 *    holder changes the owner word of pages the heap references, so the
 *    local-free check `owner == my_tag` cannot be invalidated between the
 *    check and the commit.  The only interference is a signal handler on
 *    the same thread, excluded by tls.busy.
 *  - I1: owner == tag  =>  the page is bins[c].cur, or on bins[c].avail or
 *    bins[c].full of heap tag (lflags says which list).
 *  - I3: every multi-step change is a chain of single-store commits.
 *  - I4: while ORPHAN or QUEUED is set, xfree.count is the page's total free
 *    count (remote chain + folded); "fully free" is count == cap.
 *  - A page in a class pool or orphaned has owner 0; frees to it take the
 *    remote path, whose CAS may make the freeing thread "responsible"
 *    (PENDING) for moving the page between lists (resolve).
 */

/* ---------------------------------------------------------------------- */
/* Internal metadata arena (heaps, solo table): never freed                */
/* ---------------------------------------------------------------------- */

static soham_lock_t g_meta_lock;
static uintptr_t g_meta_cur, g_meta_end;
static size_t g_meta_total;

static void *meta_alloc(size_t n) {
  n = (n + 63) & ~(size_t)63;
  soham_lock(&g_meta_lock);
  if (g_meta_cur + n > g_meta_end || g_meta_cur == 0) {
    size_t chunk = n > 256 * KiB ? ((n + g_geo.B - 1) & ~(g_geo.B - 1)) : 256 * KiB;
    void *p = soham_os_map_rw(chunk, g_geo.B);
    if (!p) { soham_unlock(&g_meta_lock); return NULL; }
    soham_os_advise_nohuge(p, chunk);
    g_meta_cur = (uintptr_t)p;
    g_meta_end = g_meta_cur + chunk;
    g_meta_total += chunk;
  }
  void *r = (void *)g_meta_cur;
  g_meta_cur += n;
  soham_unlock(&g_meta_lock);
  return r;   /* fresh mapping: zeroed */
}

/* ---------------------------------------------------------------------- */
/* Static arena: init recursion and signal-handler allocations (§5.3)      */
/* ---------------------------------------------------------------------- */

#define BSS_ARENA_SIZE (384 * KiB)
_Static_assert(BSS_ARENA_SIZE == SOHAM_INTERNAL_BSS_ARENA_SIZE, "internal_test.h states the arena size");
static char g_bss_arena[BSS_ARENA_SIZE] __attribute__((aligned(64)));
static _Atomic size_t g_bss_top;
static _Atomic size_t g_bss_freed;

SOHAM_INLINE bool in_bss_arena(uintptr_t a) {
  return a >= (uintptr_t)g_bss_arena && a < (uintptr_t)g_bss_arena + BSS_ARENA_SIZE;
}

/* Lock-free bump allocation; each block has a 16-byte size header. */
static void *bss_alloc(size_t n, size_t align) {
  if (align < 16) align = 16;
  if (n > BSS_ARENA_SIZE || align > 4096) return NULL;
  size_t need = ((n + 15) & ~(size_t)15) + 16 + align;
  if (need > BSS_ARENA_SIZE) return NULL;
  /* The counter only moves when the block fits, so it never passes the
   * arena (a counter that grew on failed requests wrapped on 32-bit). */
  size_t off = atomic_load_explicit(&g_bss_top, memory_order_relaxed);
  do {
    if (off > BSS_ARENA_SIZE - need) return NULL;
  } while (!atomic_compare_exchange_weak_explicit(&g_bss_top, &off, off + need, memory_order_relaxed,
                                                  memory_order_relaxed));
  uintptr_t p = ((uintptr_t)g_bss_arena + off + 16 + align - 1) & ~(uintptr_t)(align - 1);
  ((size_t *)p)[-1] = n;
  return (void *)p;
}

SOHAM_INLINE size_t bss_usable(uintptr_t a) { return ((const size_t *)a)[-1]; }

/* ---------------------------------------------------------------------- */
/* Records                                                                 */
/* ---------------------------------------------------------------------- */

SOHAM_INLINE rec_t *rec_of(uintptr_t a) {
  uintptr_t base = a & g_geo.wbase;
  return (rec_t *)(base + g_geo.rec_off + ((a - base) >> SOHAM_SLICE_SHIFT) * REC_SIZE);
}

SOHAM_INLINE bool in_window(uintptr_t a) {
  uintptr_t gm = atomic_load_explicit(&g_geo.gmask, memory_order_acquire);
  uintptr_t g = (a >> __atomic_load_n(&g_geo.wshift, __ATOMIC_RELAXED)) & gm;   /* see soham_free */
  return atomic_load_explicit(&atomic_load_explicit(&g_geo.gmap, memory_order_acquire)[g], memory_order_relaxed) == GM_WINDOW;
}

SOHAM_INLINE uint32_t pending_threshold(uint32_t cap) { return cap >= 8 ? cap / 8 : 1; }
/* A page on its owner's full list returns to the avail list once this many
 * blocks are free, counting local and remote frees separately.  Returning
 * it at the first free makes a refill yield one block and retire the page
 * again (one slow path per allocation under random-order frees); waiting
 * for 1/8 bounds refills to one per cap/8 allocations, at the cost of at
 * most 1/8 of a full page left idle. */
SOHAM_INLINE uint32_t unfull_threshold(uint32_t cap) { return cap >= 8 ? cap / 8 : 1; }

static uint64_t heap_rand(heap_t *h) {
  /* xorshift64* */
  uint64_t x = h->rng;
  x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
  h->rng = x;
  return x * 0x2545f4914f6cdd1dull;
}

/* Exact block-boundary check (S1, chain walks). */
SOHAM_INLINE bool block_ok(const rec_t *rh, uintptr_t a) {
  uintptr_t off = a - rh->r.pbase;
  return off < (uintptr_t)rh->r.cap * rh->r.bsize && (uint64_t)off * rh->r.divm < rh->r.divm;
}

/* An owned block failed the fast free checks (o_block, head, o_maybe_free):
 * abort on an invalid or double free, return for a false alarm.  The owner
 * holds the page, so its local list is stable; the remote chain is
 * push-only for other threads, so the nodes below a snapshot of its head
 * are stable too. */
static SOHAM_NOINLINE SOHAM_COLD void owned_free_check(rec_t *rh, uintptr_t a) {
  if (!o_block(rh, a)) soham_fatal("free of an interior or invalid pointer", a);
  uintptr_t key = rh->o.key;
  uintptr_t p = o_head(rh);
  for (uint32_t n = o_cnt(rh); p != 0 && n > 0; n--) {
    if (p == a) soham_fatal("double free detected", a);
    if (!o_block(rh, p)) soham_fatal("heap corruption: free-list link overwritten", p);
    p = link_dec(key, *(uintptr_t *)p, p);
  }
  uint64_t x = atomic_load_explicit(&rh->x.xfree, memory_order_acquire);
  p = xf_ptr(x);
  for (uint32_t n = xf_cnt(x); p != 0 && n > 0; n--) {
    if (p == a) soham_fatal("double free detected (block is on the remote free list)", a);
    if (!o_block(rh, p)) break;
    p = link_dec(key, *(uintptr_t *)p, p);
  }
}

static void page_local_event(heap_t *h, rec_t *rh, uint32_t cnt);

SOHAM_INLINE void owned_free_commit(heap_t *h, rec_t *rh, uintptr_t a, uintptr_t head) {
  *(uintptr_t *)a = link_enc(rh->o.key, head, a);
  uint32_t cnt = o_cnt(rh) + 1;
  o_set(rh, a, cnt);
  if (unlikely(cnt == rh->o.event_at)) page_local_event(h, rh, cnt);
}

/* The rare path finishes the whole free itself, so the fast path keeps
 * nothing live across it (no callee-saved spills on every free). */
static SOHAM_NOINLINE SOHAM_COLD void owned_free_checked(heap_t *h, rec_t *rh, uintptr_t a) {
  owned_free_check(rh, a);
  owned_free_commit(h, rh, a, o_head(rh));
}

/* Owner free of block a in page rh (busy held, rh owned by the caller). */
SOHAM_INLINE void owned_free(heap_t *h, rec_t *rh, uintptr_t a) {
  uintptr_t head = o_head(rh);
  if (unlikely(!o_block(rh, a) || head == a || o_maybe_free(rh, a))) { owned_free_checked(h, rh, a); return; }
  owned_free_commit(h, rh, a, head);
}

static void page_init(heap_t *h, rec_t *r, unsigned c, uintptr_t base, uint32_t nslices, uint32_t owner) {
  uint8_t kind = g_class_kind[c];
  for (uint32_t i = 1; i < nslices; i++) {
    r[i].r.head_delta = i * REC_SIZE;
    r[i].r.kind = kind;
  }
  r->r.head_delta = 0;
  r->r.kind = kind;
  r->r.cls = (uint8_t)c;
  r->r.bsize = g_class_size[c];
  r->r.cap = g_class_cap[c];
  r->r.psize = nslices << SOHAM_SLICE_SHIFT;
  r->r.nslices = nslices;
  r->r.pbase = base;
  r->r.divm = UINT64_MAX / r->r.bsize + 1;
  r->r.key = (uintptr_t)heap_rand(h);
  atomic_store_explicit(&r->r.oheap, h, memory_order_relaxed);
  r->o.key = r->r.key;
  r->o.pbase = base;
  r->o.divm = r->r.divm;
  r->o.prov = 0;
  r->o.event_at = 0;
  r->o.bump_off = 0;
  r->r.lflags = 0;
  r->o.lnext = r->o.lprev = NULL;
  o_set(r, 0, 0);
  atomic_store_explicit(&r->x.xfree, 0, memory_order_relaxed);
  r->x.folded = 0;
  r->x.pnext = r->x.pprev = NULL;
  /* pend_state and nstate are NOT reset: a stale stack entry for this
   * record may still exist; only the stacks' drains clear them (a stale
   * entry is harmless: both drains re-validate the page state). */
  atomic_store_explicit(&r->r.owner, owner, memory_order_relaxed);
}

/* Return a page's slices to the page heap.  The page must be on no list and
 * have no outstanding blocks. */
static void page_release(rec_t *rh) {
  uint32_t ns = rh->r.nslices;
  uintptr_t base = rh->r.pbase;
  atomic_store_explicit(&rh->r.owner, OWNER_NONE, memory_order_relaxed);
  for (uint32_t i = 0; i < ns; i++) { rh[i].r.kind = KIND_NONE; rh[i].r.head_delta = 0; }
  ph_free(base, ns);
}

/* ---------------------------------------------------------------------- */
/* Local list operations (owner only)                                      */
/* ---------------------------------------------------------------------- */

/* Provision the next blocks of r's bump region onto its local list. */
static void provision(rec_t *r) {
  uint32_t bs = r->r.bsize;
  uint32_t avail = r->r.cap - r->o.prov;
  uint32_t n = (uint32_t)(g_cfg.prov_bytes / bs);
  if (n < 2) n = 2;
  if (n > avail) n = avail;
  if (n == 0) return;
  uintptr_t b = r->o.pbase + r->o.bump_off;
  uintptr_t head = o_head(r);
  uint32_t lcnt = o_cnt(r);
  uintptr_t key = r->r.key;
  if (!g_geo.secure || n < 3) {
    /* Address order (S0). */
    for (uint32_t i = 0; i + 1 < n; i++) {
      uintptr_t a = b + (uintptr_t)i * bs;
      *(uintptr_t *)a = link_enc(key, a + bs, a);
    }
    uintptr_t last = b + (uintptr_t)(n - 1) * bs;
    *(uintptr_t *)last = link_enc(key, head, last);
    head = b;
  } else {
    /* Shuffled order (S1): Fisher-Yates over at most 1024 blocks. */
    uint16_t idx[1024];
    if (n > 1024) n = 1024;
    for (uint32_t i = 0; i < n; i++) idx[i] = (uint16_t)i;
    uint64_t rs = r->r.key ^ mix64((uint64_t)b);
    for (uint32_t i = n - 1; i > 0; i--) {
      rs = mix64(rs);
      uint32_t j = (uint32_t)(rs % (i + 1));
      uint16_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    for (uint32_t i = 0; i < n; i++) {
      uintptr_t a = b + (uintptr_t)idx[i] * bs;
      uintptr_t nx = (i + 1 < n) ? b + (uintptr_t)idx[i + 1] * bs : head;
      *(uintptr_t *)a = link_enc(key, nx, a);
    }
    head = b + (uintptr_t)idx[0] * bs;
  }
  r->o.prov += n;
  if (r->r.lflags != LF_FULL) r->o.event_at = r->o.prov;
  r->o.bump_off += n * bs;
  o_set(r, head, lcnt + n);
}

/* Every block of r is on its local list or its remote chain at most once.
 * Counts alone decide when a page whose blocks all look free is released,
 * and a page claimed from a pool hands out every block on its lists, so a
 * block freed twice (once onto each list) must be caught here: walk both,
 * mark each block in a bitmap, abort on a repeat. */
#define SOHAM_MAX_CAP ((1u << SOHAM_SLICE_SHIFT) / 16u)
static SOHAM_NOINLINE void free_lists_unique(rec_t *r, uintptr_t chain, uint32_t n) {
  uint64_t seen[SOHAM_MAX_CAP / 64];
  memset(seen, 0, sizeof seen);
  uintptr_t key = r->r.key;
  uint32_t bs = r->r.bsize;
  for (int list = 0; list < 2; list++) {
    uintptr_t p = list ? chain : o_head(r);
    uint32_t k = list ? n : o_cnt(r);
    for (; k > 0 && p != 0; k--) {
      if (unlikely(!o_block(r, p))) soham_fatal("heap corruption: free-list link overwritten", p);
      uint32_t i = (uint32_t)((p - r->o.pbase) / bs);
      if (unlikely(i >= SOHAM_MAX_CAP || (seen[i >> 6] >> (i & 63)) & 1)) soham_fatal("double free detected", p);
      seen[i >> 6] |= (uint64_t)1 << (i & 63);
      p = link_dec(key, *(uintptr_t *)p, p);
    }
  }
}

/* Validate a chain of exactly n blocks starting at head and splice it in
 * front of r's local list with one commit.  Aborts on a corrupt chain
 * (racing double frees, overwritten links).  full_check: also check that no
 * block is on both lists (a page not owned by the caller: claim, resolve);
 * an owner's collect checks only when every block looks free. */
static void merge_chain(rec_t *r, uintptr_t head, uint32_t n, bool full_check) {
  if (n == 0) return;
  uintptr_t key = r->r.key;
  uintptr_t p = head;
  for (uint32_t i = 1;; i++) {
    if (unlikely(!o_block(r, p))) soham_fatal("corrupt remote free chain", p);
    uintptr_t nx = link_dec(key, *(uintptr_t *)p, p);
    if (i == n) {
      if (unlikely(nx != 0)) soham_fatal("corrupt remote free chain (length)", p);
      break;
    }
    p = nx;
  }
  uint32_t cnt = o_cnt(r) + n;
  if (unlikely(cnt > r->o.prov)) soham_fatal("double free detected (free count exceeds blocks)", head);
  if (full_check || cnt == r->o.prov) free_lists_unique(r, head, n);
  *(uintptr_t *)p = link_enc(key, o_head(r), p);
  o_set(r, head, cnt);
}

/* Owned page: take every remote free.  An owned page's xfree carries no
 * flag except WATCH, which only pages on the full list carry (set on a zero
 * count, cleared by the remote free reaching unfull_threshold or by
 * page_unwatch), and full-list pages are never collected. */
static uint32_t collect(rec_t *r) {
  uint64_t x0 = atomic_load_explicit(&r->x.xfree, memory_order_relaxed);
  if (xf_cnt(x0) == 0) return 0;
  uint64_t x = atomic_exchange_explicit(&r->x.xfree, 0, memory_order_acquire);
  SOHAM_ASSERT((x & XF_FLAGS) == 0);
  uint32_t n = xf_cnt(x);
  merge_chain(r, xf_ptr(x), n, false);
  return n;
}

SOHAM_INLINE bool page_has_free(rec_t *r) {
  return o_head(r) != 0;
}

SOHAM_INLINE void *page_pop(rec_t *r) {
  uintptr_t b = o_head(r);
  if (!b) return NULL;
  uintptr_t nx = link_dec(r->o.key, *(uintptr_t *)b, b);
  if (unlikely(nx != 0 && !o_block(r, nx))) soham_fatal("heap corruption: free-list link overwritten", b);
  *(uintptr_t *)b = 0;   /* see o_maybe_free */
  o_set(r, nx, o_cnt(r) - 1);
  return (void *)b;
}

/* Owner lists (avail, full) are doubly linked through line O. */
static void list_push(rec_t **head, rec_t *r, uint32_t which) {
  r->r.lflags = which;
  /* On the full list the local free reaching unfull_threshold is an event;
   * otherwise only the free that makes the page completely free is. */
  r->o.event_at = (which == LF_FULL) ? o_cnt(r) + unfull_threshold(r->r.cap) : r->o.prov;
  r->o.lprev = NULL;
  r->o.lnext = *head;
  if (*head) (*head)->o.lprev = r;
  *head = r;
}

static void list_unlink(bin_t *bin, rec_t *r) {
  rec_t **head = (r->r.lflags == LF_FULL) ? &bin->full : &bin->avail;
  if (r->o.lprev) r->o.lprev->o.lnext = r->o.lnext; else *head = r->o.lnext;
  if (r->o.lnext) r->o.lnext->o.lprev = r->o.lprev;
  r->o.lnext = r->o.lprev = NULL;
  r->r.lflags = 0;
  r->o.event_at = r->o.prov;
}

/* ---------------------------------------------------------------------- */
/* Class pools, the pending stack and owner notification                   */
/* ---------------------------------------------------------------------- */

typedef struct {
  soham_lock_t lock;
  _Atomic uint32_t n;      /* pages queued (hint, read without the lock) */
  rec_t *head[3];          /* by free fraction at insertion: [1/8,1/4) [1/4,1/2) [1/2,1) */
} __attribute__((aligned(64))) pool_t;

static pool_t g_pools[SOHAM_NCLASSES];

static unsigned pool_bucket(uint32_t cnt, uint32_t cap) {
  uint64_t f8 = (uint64_t)cnt * 8;
  if (f8 < 2ull * cap) return 0;
  if (f8 < 4ull * cap) return 1;
  return 2;
}

static void pool_insert(pool_t *pl, rec_t *r, unsigned b) {
  r->x.bucket = b;
  r->x.pprev = NULL;
  r->x.pnext = pl->head[b];
  if (r->x.pnext) r->x.pnext->x.pprev = r;
  pl->head[b] = r;
  atomic_fetch_add_explicit(&pl->n, 1, memory_order_relaxed);
}

static void pool_unlink(pool_t *pl, rec_t *r) {
  unsigned b = r->x.bucket;
  if (r->x.pprev) r->x.pprev->x.pnext = r->x.pnext; else pl->head[b] = r->x.pnext;
  if (r->x.pnext) r->x.pnext->x.pprev = r->x.pprev;
  r->x.pnext = r->x.pprev = NULL;
  atomic_fetch_sub_explicit(&pl->n, 1, memory_order_relaxed);
}

/* Called by the thread whose CAS set PENDING (plan §5.4 resolve). */
static void resolve(rec_t *r) {
  /* The class is valid only while PENDING is set: a stale pending-stack
   * entry may name a page that was released and reused (even as a run, whose
   * cls is 0xff).  Check PENDING first, and again under the class's lock. */
  uint64_t x = atomic_load_explicit(&r->x.xfree, memory_order_acquire);
  pool_t *pl;
  for (;;) {
    if (!(x & XF_PENDING)) return;
    unsigned c = r->r.cls;
    if (c >= SOHAM_NCLASSES) return;
    pl = &g_pools[c];
    soham_lock(&pl->lock);
    x = atomic_load_explicit(&r->x.xfree, memory_order_acquire);
    if (r->r.cls == c) break;
    soham_unlock(&pl->lock);
  }
  bool give_back = false;
  for (;;) {
    if (!(x & XF_PENDING)) break;          /* a claimer took the page */
    uint32_t cnt = xf_cnt(x), cap = r->r.cap;
    if (cnt == cap) {
      if (atomic_compare_exchange_weak_explicit(&r->x.xfree, &x, 0, memory_order_acq_rel, memory_order_acquire)) {
        if (x & XF_QUEUED) pool_unlink(pl, r);
        /* Walk the chain before releasing, as pool_claim does: a remote
         * A,B,A double free reaches count == cap with a live block left on
         * the page, and only the exact-length walk exposes it (abort). */
        merge_chain(r, xf_ptr(x), cnt - r->x.folded, true);
        give_back = true;
        break;
      }
    } else if ((x & XF_ORPHAN) && cnt >= pending_threshold(cap)) {
      uint64_t nw = (x & ~(XF_ORPHAN | XF_PENDING)) | XF_QUEUED;
      if (atomic_compare_exchange_weak_explicit(&r->x.xfree, &x, nw, memory_order_acq_rel, memory_order_acquire)) {
        pool_insert(pl, r, pool_bucket(cnt, cap));
        break;
      }
    } else {
      if (atomic_compare_exchange_weak_explicit(&r->x.xfree, &x, x & ~XF_PENDING, memory_order_acq_rel, memory_order_acquire)) break;
    }
  }
  soham_unlock(&pl->lock);
  if (give_back) page_release(r);
}

/* Pages whose PENDING was set in handler context, resolved later by the
 * next slow path of any thread.  pend_state keeps a page from being pushed
 * twice: a second responsible handler finds it already queued, and the
 * drain's resolve() re-reads xfree, so the newer PENDING is handled too. */
static _Atomic(rec_t *) g_pending;

static void pending_push(rec_t *r) {
  /* Store-buffering, as in the notify protocol: either the drain below sees
   * our PENDING (set by the caller's xfree CAS), or this CAS sees its 0. */
  atomic_thread_fence(memory_order_seq_cst);
  uint32_t z = 0;
  if (!atomic_compare_exchange_strong_explicit(&r->x.pend_state, &z, 1, memory_order_acq_rel, memory_order_relaxed)) return;
  rec_t *old = atomic_load_explicit(&g_pending, memory_order_relaxed);
  do { r->x.pend_next = old; }
  while (!atomic_compare_exchange_weak_explicit(&g_pending, &old, r, memory_order_release, memory_order_relaxed));
}

static void pending_drain(void) {
  if (!atomic_load_explicit(&g_pending, memory_order_relaxed)) return;
  rec_t *r = atomic_exchange_explicit(&g_pending, NULL, memory_order_acquire);
  while (r) {
    rec_t *nx = r->x.pend_next;
    r->x.pend_next = NULL;
    atomic_store_explicit(&r->x.pend_state, 0, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);   /* pairs with pending_push */
    resolve(r);
    r = nx;
  }
}

/* A remote free cleared WATCH on a page of h's full list: tell h.  Lock-free
 * (also used from signal handlers).  nstate keeps the page on at most one
 * stack; a stale entry (page detached or reused since) is ignored by the
 * drain, which re-validates ownership and list membership. */
/* heap_flush closes h's stack (thread exit, collect) until the heap is
 * attached again.  A push that finds it closed leaves nstate clear: the
 * page is being detached, and an entry left on a stack nobody drains would
 * keep nstate set and silence every later owner's notifications. */
#define NOTIFY_CLOSED ((rec_t *)(uintptr_t)1)

/* The shared heap may have pages for shared_trim() to release: set by every
 * allocation from it and every notification to it (see shared_trim). */
static _Atomic uint8_t g_shared_trim;

static void notify_push(heap_t *h, rec_t *r) {
  if (atomic_load_explicit(&r->r.owner, memory_order_relaxed) != h->tag) return;   /* moved on */
  uint32_t z = 0;
  if (!atomic_compare_exchange_strong_explicit(&r->x.nstate, &z, 1, memory_order_seq_cst, memory_order_relaxed)) return;
  rec_t *old = atomic_load_explicit(&h->notify, memory_order_relaxed);
  do {
    if (old == NOTIFY_CLOSED) { atomic_store_explicit(&r->x.nstate, 0, memory_order_release); return; }
    r->x.nnext = old;
  } while (!atomic_compare_exchange_weak_explicit(&h->notify, &old, r, memory_order_seq_cst, memory_order_relaxed));
  if (h->is_shared) atomic_store_explicit(&g_shared_trim, 1, memory_order_seq_cst);
}

/* Owner: move notified full pages back to the avail list. */
static void notify_drain(heap_t *h) {
  rec_t *top = atomic_load_explicit(&h->notify, memory_order_relaxed);
  if (!top || top == NOTIFY_CLOSED) return;
  rec_t *r = atomic_exchange_explicit(&h->notify, NULL, memory_order_acquire);
  while (r) {
    rec_t *nx = r->x.nnext;
    r->x.nnext = NULL;
    atomic_store_explicit(&r->x.nstate, 0, memory_order_release);
    /* Store-buffering: either we see the freer's WATCH clear below, or its
     * nstate CAS (seq_cst, after its xfree CAS) sees our 0 and pushes again. */
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&r->r.owner, memory_order_relaxed) == h->tag && r->r.lflags == LF_FULL &&
        !(atomic_load_explicit(&r->x.xfree, memory_order_relaxed) & XF_WATCH)) {
      bin_t *bin = &h->bins[r->r.cls];
      list_unlink(bin, r);
      list_push(&bin->avail, r, LF_AVAIL);
    }
    r = nx;
  }
}

/* Owner: put exhausted page r on the full list, arming WATCH.  Fails (and
 * collects instead) when remote frees arrived meanwhile.  Returns true when
 * r went to the full list. */
static bool page_retire_full(bin_t *bin, rec_t *r) {
  uint64_t x = atomic_load_explicit(&r->x.xfree, memory_order_relaxed);
  for (;;) {
    if (xf_cnt(x)) { collect(r); return false; }
    if (atomic_compare_exchange_weak_explicit(&r->x.xfree, &x, x | XF_WATCH, memory_order_acq_rel, memory_order_relaxed)) break;
  }
  list_push(&bin->full, r, LF_FULL);
  return true;
}

/* Owner: disarm WATCH (the page is leaving the full list). */
static void page_unwatch(rec_t *r) {
  uint64_t x = atomic_load_explicit(&r->x.xfree, memory_order_relaxed);
  while (x & XF_WATCH) {
    if (atomic_compare_exchange_weak_explicit(&r->x.xfree, &x, x & ~XF_WATCH, memory_order_acq_rel, memory_order_relaxed)) break;
  }
}

/* Remote free of block a in page rh (one CAS; plan §5.4 remote CAS rule).
 * in_handler: never take a lock; defer resolve through the pending stack. */
static void remote_free(rec_t *rh, uintptr_t a, bool in_handler) {
  uintptr_t key = rh->r.key;
  uint32_t cap = rh->r.cap;
  uint64_t old = atomic_load_explicit(&rh->x.xfree, memory_order_relaxed);
  for (;;) {
    uintptr_t head = xf_ptr(old);
    if (unlikely(head == a)) soham_fatal("double free detected", a);
    uint32_t cnt = xf_cnt(old) + 1;
    if (unlikely(cnt > cap)) soham_fatal("double free detected (remote count exceeds capacity)", a);
    *(uintptr_t *)a = link_enc(key, head, a);
    uint64_t flags = old & XF_FLAGS;
    bool notify = false;
    if ((flags & XF_WATCH) && cnt >= unfull_threshold(cap)) { flags &= ~XF_WATCH; notify = true; }
    uint64_t nw = xf_make(a, cnt, flags);
    bool responsible = false;
    if ((old & (XF_ORPHAN | XF_QUEUED)) && !(old & XF_PENDING) &&
        (((old & XF_ORPHAN) && cnt >= pending_threshold(cap)) || cnt == cap)) {
      nw |= XF_PENDING;
      responsible = true;
    }
    if (atomic_compare_exchange_weak_explicit(&rh->x.xfree, &old, nw, memory_order_acq_rel, memory_order_relaxed)) {
      if (notify) {
        /* The CAS acquired the owner's WATCH publication, so oheap is the
         * heap that owns the page now (or owned it until a later detach). */
        heap_t *oh = atomic_load_explicit(&rh->r.oheap, memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);   /* pairs with notify_drain */
        if (oh) notify_push(oh, rh);
      }
      if (responsible) {
        if (in_handler) pending_push(rh); else resolve(rh);
      }
      return;
    }
  }
}

/* Detach an owned page from its heap (holder only; the page is already off
 * the heap's lists).  plan §5.4 Detach, with owner cleared before the
 * ORPHAN publication so a later claimer's owner store cannot be undone. */
static void page_detach(rec_t *r) {
  atomic_store_explicit(&r->r.owner, OWNER_NONE, memory_order_relaxed);
  r->r.lflags = 0;
  r->o.lnext = r->o.lprev = NULL;
  uint32_t lc = o_cnt(r);
  uint32_t folded = lc + (r->r.cap - r->o.prov);
  r->x.folded = folded;
  uint32_t cap = r->r.cap;
  uint64_t old = atomic_load_explicit(&r->x.xfree, memory_order_relaxed);
  bool responsible;
  for (;;) {
    SOHAM_ASSERT((old & (XF_ORPHAN | XF_QUEUED | XF_PENDING)) == 0);
    uint32_t cnt = xf_cnt(old) + folded;
    uint64_t nw = xf_make(xf_ptr(old), cnt, XF_ORPHAN);      /* drops WATCH */
    responsible = cnt >= pending_threshold(cap) || cnt == cap;
    if (responsible) nw |= XF_PENDING;
    if (atomic_compare_exchange_weak_explicit(&r->x.xfree, &old, nw, memory_order_acq_rel, memory_order_relaxed)) break;
  }
  if (responsible) resolve(r);
}

/* Claim the fullest queued page of class c for heap h, or NULL. */
static rec_t *pool_claim(heap_t *h, unsigned c) {
  pool_t *pl = &g_pools[c];
  if (atomic_load_explicit(&pl->n, memory_order_relaxed) == 0) return NULL;
  if (!lock_or_try(&pl->lock)) return NULL;
  rec_t *r = pl->head[0] ? pl->head[0] : pl->head[1] ? pl->head[1] : pl->head[2];
  if (!r) { soham_unlock(&pl->lock); return NULL; }
  pool_unlink(pl, r);
  uint64_t x = atomic_exchange_explicit(&r->x.xfree, 0, memory_order_acq_rel);  /* clears QUEUED, PENDING */
  soham_unlock(&pl->lock);
  uint32_t chain = xf_cnt(x) - r->x.folded;
  merge_chain(r, xf_ptr(x), chain, true);
  r->r.lflags = 0;
  r->o.event_at = r->o.prov;
  r->o.lnext = r->o.lprev = NULL;
  atomic_store_explicit(&r->r.oheap, h, memory_order_relaxed);
  atomic_store_explicit(&r->r.owner, h->tag, memory_order_relaxed);
  return r;
}

/* ---------------------------------------------------------------------- */
/* Heaps                                                                   */
/* ---------------------------------------------------------------------- */

static rec_t g_sentinel __attribute__((aligned(64)));

static soham_lock_t g_heap_lock;      /* heap pool and registry */
static heap_t *g_heap_pool;
static heap_t *g_heap_all;
static uint32_t g_heap_count;
static uint32_t g_heaps_live;
static heap_t *g_shared;               /* for threads without a heap */
static soham_lock_t g_shared_lock;

static heap_t *heap_create_locked(void) {
  if (g_heap_count >= SOHAM_MAX_TAG) return NULL;
  heap_t *h = (heap_t *)meta_alloc(sizeof(heap_t));
  if (!h) return NULL;
  h->tag = ++g_heap_count;
  h->rng = mix64(g_rng_seed ^ ((uint64_t)h->tag << 32)) | 1;
  for (unsigned c = 0; c < SOHAM_NCLASSES; c++) {
    h->bins[c].cur = &g_sentinel;
    h->bins[c].avail = h->bins[c].full = NULL;
  }
  h->all_next = g_heap_all;
  g_heap_all = h;
  return h;
}

static rec_t *page_new(heap_t *h, unsigned c) {
  if (g_class_kind[c] == KIND_SMALL && h->nspares) {
    rec_t *r = h->spares[--h->nspares];
    page_init(h, r, c, r->r.pbase, 1, h->tag);
    return r;
  }
  uint32_t k = g_class_page[c] >> SOHAM_SLICE_SHIFT;
  bool zero;
  uintptr_t a = ph_alloc_slices(k, &zero);
  if (!a) return NULL;
  rec_t *r = rec_of(a);
  page_init(h, r, c, a, k, h->tag);
  return r;
}

/* Make r usable: collect its remote frees, provision if needed. */
SOHAM_INLINE bool page_ready(rec_t *r) {
  if (page_has_free(r)) return true;
  if (collect(r)) return true;
  if (r->o.prov < r->r.cap) { provision(r); return true; }
  return false;
}

/* Allocation from bin c when cur is empty (busy held). */
/* A page of h that has no allocated blocks and is on no list: keep it as
 * a spare (small pages) or give it back to the page heap. */
static void page_empty_release(heap_t *h, rec_t *rh) {
  if (rh->r.kind == KIND_SMALL && h->nspares < (uint8_t)g_cfg.max_spares) {
    atomic_store_explicit(&rh->r.owner, OWNER_SPARE, memory_order_relaxed);
    h->spares[h->nspares++] = rh;
    return;
  }
  page_release(rh);
}

/* Pages that became completely free through remote frees never see a local
 * event.  Look at a few avail pages per refill and release the empty ones,
 * so a consumer thread's frees reach the page heap while the owner lives. */
static void bin_trim_avail(heap_t *h, bin_t *bin) {
  rec_t *r = bin->avail;
  for (int i = 0; i < 2 && r; i++) {
    rec_t *nx = r->o.lnext;
    collect(r);
    if (o_cnt(r) == r->o.prov) { list_unlink(bin, r); page_empty_release(h, r); }
    r = nx;
  }
}

/* The shared heap has no running owner, so no refill of its own releases
 * the pages that handler-context allocations filled and other threads then
 * freed.  Any thread does it here (busy held, not in trymode), under the
 * shared lock taken by trylock: notified full pages move to the avail list,
 * and avail pages whose blocks are all free are unlinked, then released
 * after the unlock, so a handler that finds the lock taken (and falls back
 * to the static arena) waits on nothing slow.  The current page of each
 * class stays.  g_shared_trim is cleared once no avail page and no
 * notification is left; full pages with less than 1/8 free stay, as on any
 * heap's full list. */
static void shared_trim(void) {
  heap_t *h = g_shared;
  if (!soham_trylock(&g_shared_lock)) return;
  notify_drain(h);
  rec_t *rel = NULL;
  bool keep = false;
  for (unsigned c = 0; c < SOHAM_NCLASSES; c++) {
    bin_t *bin = &h->bins[c];
    for (rec_t *r = bin->avail, *nx; r; r = nx) {
      nx = r->o.lnext;
      uint32_t remote = xf_cnt(atomic_load_explicit(&r->x.xfree, memory_order_acquire));
      if (o_cnt(r) + remote != r->o.prov) { keep = true; continue; }
      list_unlink(bin, r);
      r->o.lnext = rel;
      rel = r;
    }
  }
  if (!keep) {
    /* Clear, then look again: a push that set the flag before the clear is
     * visible after the fence (seq_cst push, flag store and fence). */
    atomic_store_explicit(&g_shared_trim, 0, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&h->notify, memory_order_relaxed)) atomic_store_explicit(&g_shared_trim, 1, memory_order_relaxed);
  }
  soham_unlock(&g_shared_lock);
  /* Off the heap's lists: no other thread reaches these pages, and with
   * every block free no remote free can arrive.  collect() walks the remote
   * chain at its exact length first, as resolve() does before a release. */
  while (rel) {
    rec_t *nx = rel->o.lnext;
    rel->o.lnext = NULL;
    collect(rel);
    page_release(rel);
    rel = nx;
  }
}

/* Every 64th refill of a thread heap: trim the shared heap when it may have
 * pages to release, at most once per purge interval across all threads. */
static _Atomic uint64_t g_shared_trim_ns;

static void shared_trim_tick(void) {
  if (!atomic_load_explicit(&g_shared_trim, memory_order_relaxed)) return;
  uint64_t now = soham_os_now_ns();
  uint64_t last = atomic_load_explicit(&g_shared_trim_ns, memory_order_relaxed);
  if (now - last < g_cfg.purge_interval_ns) return;
  if (!atomic_compare_exchange_strong_explicit(&g_shared_trim_ns, &last, now, memory_order_relaxed, memory_order_relaxed)) return;
  shared_trim();
}

static SOHAM_NOINLINE void *bin_refill(heap_t *h, unsigned c) {
  if (likely(!t_tls.trymode)) {
    pending_drain();
    if (unlikely((++h->refills & 63) == 0)) {
      ph_decay_tick();
      if (!h->is_shared) shared_trim_tick();
    }
  }
  bin_t *bin = &h->bins[c];
  rec_t *r = bin->cur;
  if (r != &g_sentinel) {
    if (page_ready(r)) return page_pop(r);
    /* cur is exhausted: retire it to the full list (WATCH armed), unless
     * remote frees arrived in the meantime. */
    bin->cur = &g_sentinel;
    if (!page_retire_full(bin, r)) { bin->cur = r; return page_pop(r); }
  }
  notify_drain(h);
  /* Owned pages with free space. */
  while (bin->avail) {
    rec_t *a = bin->avail;
    list_unlink(bin, a);
    /* No trim in handler context: releasing a page takes the blocking
     * page-heap lock, which the interrupted thread may hold. */
    if (page_ready(a)) { bin->cur = a; if (likely(!t_tls.trymode)) bin_trim_avail(h, bin); return page_pop(a); }
    if (!page_retire_full(bin, a)) { bin->cur = a; return page_pop(a); }
  }
  /* Shared pages, then a fresh page. */
  rec_t *n = pool_claim(h, c);
  if (!n) n = page_new(h, c);
  if (!n) return NULL;
  bin->cur = n;
  if (!page_ready(n)) return NULL;   /* cannot happen: a claimed page has >= 1/8 free */
  return page_pop(n);
}

SOHAM_INLINE void *bin_alloc(heap_t *h, unsigned c) {
  void *p = page_pop(h->bins[c].cur);
  return likely(p != NULL) ? p : bin_refill(h, c);
}

/* A local free hit a page that is on the full list, or made every
 * provisioned block of the page free (busy held). */
static SOHAM_NOINLINE void page_local_event(heap_t *h, rec_t *rh, uint32_t cnt) {
  bin_t *bin = &h->bins[rh->r.cls];
  if (rh->r.lflags == LF_FULL) {
    page_unwatch(rh);
    list_unlink(bin, rh);
    list_push(&bin->avail, rh, LF_AVAIL);
  }
  if (cnt != rh->o.prov || bin->cur == rh) return;
  /* Fully free and not the current page: the remote chain must be empty
   * (every provisioned block is on the local list). */
  list_unlink(bin, rh);
  SOHAM_ASSERT(xf_cnt(atomic_load_explicit(&rh->x.xfree, memory_order_relaxed)) == 0);
  page_empty_release(h, rh);
}

/* Detach every page of h (thread exit, collect). */
static void heap_flush(heap_t *h) {
  /* Close the stack (see NOTIFY_CLOSED) and clear what is on it: only
   * pages we are about to detach. */
  rec_t *n = atomic_exchange_explicit(&h->notify, NOTIFY_CLOSED, memory_order_acquire);
  if (n == NOTIFY_CLOSED) n = NULL;
  while (n) {
    rec_t *nx = n->x.nnext;
    n->x.nnext = NULL;
    atomic_store_explicit(&n->x.nstate, 0, memory_order_release);
    n = nx;
  }
  for (unsigned c = 0; c < SOHAM_NCLASSES; c++) {
    bin_t *bin = &h->bins[c];
    rec_t *cur = bin->cur;
    bin->cur = &g_sentinel;
    if (cur != &g_sentinel) { collect(cur); page_detach(cur); }
    while (bin->avail) { rec_t *r = bin->avail; list_unlink(bin, r); collect(r); page_detach(r); }
    while (bin->full) { rec_t *r = bin->full; list_unlink(bin, r); page_detach(r); }
  }
  while (h->nspares) page_release(h->spares[--h->nspares]);
}

/* soham_collect(1), malloc_trim: detach every page of the shared heap,
 * which stays in use (busy held). */
static void shared_flush(void) {
  soham_lock(&g_shared_lock);
  heap_flush(g_shared);
  atomic_store_explicit(&g_shared->notify, NULL, memory_order_release);
  soham_unlock(&g_shared_lock);
}

/* ---------------------------------------------------------------------- */
/* Runs (192 KiB .. 32 MiB) and solos (>= 32 MiB)                         */
/* ---------------------------------------------------------------------- */

/* Allocate a run of at least n bytes.  hb_align: start on a hugeblock. */
static void *run_alloc(size_t n, bool hb_align, bool *zero) {
  size_t k = (n + g_geo.S - 1) >> SOHAM_SLICE_SHIFT;
  uintptr_t a = (k <= HB_SLICES && !hb_align) ? ph_alloc_slices((unsigned)k, zero) : ph_alloc_hbs(k, zero);
  if (!a) return NULL;
  rec_t *r = rec_of(a);
  for (size_t i = 1; i < k; i++) { r[i].r.head_delta = (uint32_t)(i * REC_SIZE); r[i].r.kind = KIND_RUN; }
  r->r.head_delta = 0;
  r->r.kind = KIND_RUN;
  r->r.cls = 0xff;
  r->r.pbase = a;
  r->r.psize = (uint32_t)(k << SOHAM_SLICE_SHIFT);
  r->r.bsize = r->r.psize;
  r->r.cap = 1;
  r->r.nslices = (uint32_t)k;
  r->r.key = 0;
  atomic_store_explicit(&r->x.xfree, 0, memory_order_relaxed);
  atomic_store_explicit(&r->r.owner, OWNER_RUN, memory_order_relaxed);
  return (void *)a;
}

static void run_free(rec_t *rh) { page_release(rh); }

/* run_free in handler context: only when the page-heap lock is free
 * (trylock).  The release engine is left to the next ordinary operation. */
static bool run_free_try(rec_t *rh) {
  if (!soham_trylock(&g_ph.lock)) return false;
  uint32_t ns = rh->r.nslices;
  uintptr_t base = rh->r.pbase;
  atomic_store_explicit(&rh->r.owner, OWNER_NONE, memory_order_relaxed);
  for (uint32_t i = 0; i < ns; i++) { rh[i].r.kind = KIND_NONE; rh[i].r.head_delta = 0; }
  bool excess, purge;
  ph_free_locked(base, ns, soham_os_now_ns(), &excess, &purge);
  soham_unlock(&g_ph.lock);
  return true;
}

typedef struct { uintptr_t p; size_t len; } solo_ent_t;
static soham_lock_t g_solo_lock;
static solo_ent_t *g_solo_tab;
static size_t g_solo_cap, g_solo_n, g_solo_bytes;

static size_t solo_hash(uintptr_t p, size_t cap) {
  return (size_t)(mix64((uint64_t)p) & (cap - 1));
}

static void solo_insert_raw(solo_ent_t *tab, size_t cap, uintptr_t p, size_t len) {
  size_t i = solo_hash(p, cap);
  while (tab[i].p) i = (i + 1) & (cap - 1);
  tab[i].p = p;
  tab[i].len = len;
}

static bool solo_grow_locked(void) {
  size_t ncap = g_solo_cap ? g_solo_cap * 2 : 256;
  solo_ent_t *nt = (solo_ent_t *)soham_os_map_rw(ncap * sizeof(solo_ent_t), g_geo.B);
  if (!nt) return false;
  for (size_t i = 0; i < g_solo_cap; i++)
    if (g_solo_tab[i].p) solo_insert_raw(nt, ncap, g_solo_tab[i].p, g_solo_tab[i].len);
  if (g_solo_tab) soham_os_unmap(g_solo_tab, g_solo_cap * sizeof(solo_ent_t));
  g_solo_tab = nt;
  g_solo_cap = ncap;
  return true;
}

/* Find p; returns the slot index or SIZE_MAX (lock held). */
static size_t solo_find_locked(uintptr_t p) {
  if (!g_solo_cap) return SIZE_MAX;
  size_t i = solo_hash(p, g_solo_cap);
  while (g_solo_tab[i].p) {
    if (g_solo_tab[i].p == p) return i;
    i = (i + 1) & (g_solo_cap - 1);
  }
  return SIZE_MAX;
}

/* Backward-shift deletion keeps probe sequences intact. */
static void solo_remove_at_locked(size_t i) {
  size_t cap = g_solo_cap;
  size_t j = i;
  for (;;) {
    j = (j + 1) & (cap - 1);
    if (!g_solo_tab[j].p) break;
    size_t h = solo_hash(g_solo_tab[j].p, cap);
    /* Move j to i if its home h is not in (i, j] cyclically. */
    bool in_range = (i <= j) ? (h > i && h <= j) : (h > i || h <= j);
    if (!in_range) { g_solo_tab[i] = g_solo_tab[j]; i = j; }
  }
  g_solo_tab[i].p = 0;
  g_solo_tab[i].len = 0;
  g_solo_n--;
}

static void *solo_alloc(size_t n, size_t align) {
  size_t B = g_geo.B;
  if (n > (size_t)PTRDIFF_MAX - B) return NULL;
  size_t len = (n + B - 1) & ~(B - 1);
  if (len == 0) len = B;
  if (align < B) align = B;
  void *p = soham_os_map_rw(len, align);
  if (!p) return NULL;
  if (!lock_or_try(&g_solo_lock)) { soham_os_unmap(p, len); return NULL; }
  if ((g_solo_n + 1) * 2 > g_solo_cap && !solo_grow_locked()) {
    soham_unlock(&g_solo_lock);
    soham_os_unmap(p, len);
    return NULL;
  }
  solo_insert_raw(g_solo_tab, g_solo_cap, (uintptr_t)p, len);
  g_solo_n++;
  g_solo_bytes += len;
  soham_unlock(&g_solo_lock);
  return p;
}

/* 0 when p is not a solo; SOLO_UNKNOWN when try_only and the table is
 * locked (the caller is a signal handler that may have interrupted the
 * lock holder). */
#define SOLO_UNKNOWN SIZE_MAX
static size_t solo_usable(uintptr_t p, bool try_only) {
  if (try_only) { if (!soham_trylock(&g_solo_lock)) return SOLO_UNKNOWN; }
  else soham_lock(&g_solo_lock);
  size_t i = solo_find_locked(p);
  size_t len = i == SIZE_MAX ? 0 : g_solo_tab[i].len;
  soham_unlock(&g_solo_lock);
  return len;
}

static bool solo_free(uintptr_t p) {
  soham_lock(&g_solo_lock);
  size_t i = solo_find_locked(p);
  if (i == SIZE_MAX) { soham_unlock(&g_solo_lock); return false; }
  size_t len = g_solo_tab[i].len;
  solo_remove_at_locked(i);
  g_solo_bytes -= len;
  soham_unlock(&g_solo_lock);
  soham_os_unmap((void *)p, len);
  return true;
}

/* solo_free in handler context, with the table taken by trylock:
 * 1 freed, 0 not a solo, -1 the table is taken (unknown). */
static int solo_free_try(uintptr_t p) {
  if (!soham_trylock(&g_solo_lock)) return -1;
  size_t i = solo_find_locked(p);
  if (i == SIZE_MAX) { soham_unlock(&g_solo_lock); return 0; }
  size_t len = g_solo_tab[i].len;
  solo_remove_at_locked(i);
  g_solo_bytes -= len;
  soham_unlock(&g_solo_lock);
  soham_os_unmap((void *)p, len);
  return 1;
}

/* Resize a solo in place or by moving it (mremap).  NULL: not possible. */
static void *solo_realloc(uintptr_t p, size_t n) {
  size_t B = g_geo.B;
  if (n > (size_t)PTRDIFF_MAX - B) return NULL;
  size_t nlen = (n + B - 1) & ~(B - 1);
  /* Take the entry out, remap unlocked (no syscall under a lock), put the
   * result back.  Removing and reinserting one entry never grows the table.
   * Meanwhile p is unknown to the table: only a racing free of the block
   * being reallocated, a program bug, can notice. */
  soham_lock(&g_solo_lock);
  size_t i = solo_find_locked(p);
  if (i == SIZE_MAX) { soham_unlock(&g_solo_lock); return NULL; }
  size_t olen = g_solo_tab[i].len;
  solo_remove_at_locked(i);
  soham_unlock(&g_solo_lock);
  void *q = soham_os_remap((void *)p, olen, nlen);
  soham_lock(&g_solo_lock);
  if (q) solo_insert_raw(g_solo_tab, g_solo_cap, (uintptr_t)q, nlen);
  else solo_insert_raw(g_solo_tab, g_solo_cap, p, olen);
  g_solo_n++;
  if (q) g_solo_bytes += nlen - olen;
  soham_unlock(&g_solo_lock);
  return q;
}
