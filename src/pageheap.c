/*
 * src/pageheap.c -- size classes, windows, hugeblocks and memory release.
 * Part of the sohamalloc unity build (included by src/sohamalloc.c).
 *
 * Address space: each window is W bytes, W-aligned, reserved PROT_NONE
 * (§3).  Its first meta_size bytes (header, hugeblock metadata, slice
 * records) are committed at creation; a PROT_NONE guard gap follows; data
 * starts at data_off and is committed as a growing prefix of hugeblocks.
 *
 * Every hugeblock (H = 32 slices) keeps four slice bitmaps:
 *   used       part of a page or run
 *   dirty      free and resident: the release (decay) engine's input
 *   zero       free and known to read as zero (never touched, or DONTNEED)
 *   releasing  handed to the OS right now: neither free nor used
 * A free slice that is neither dirty nor zero was released lazily
 * (MADV_FREE): not resident, contents indeterminate.
 *
 * Allocation of k <= 32 slices is best-fit: the hugeblock whose longest
 * allocatable run is the smallest that fits (bucket lists indexed by that
 * length), then the smallest fitting run inside it.  Runs of more than 32
 * slices, and H-aligned runs, take whole contiguous hugeblocks.
 *
 * All page-heap state is protected by ph.lock.  The only syscalls made
 * under it are commit growth and window creation (rare); release
 * syscalls run unlocked on ranges marked `releasing` (§6).
 */

/* ---------------------------------------------------------------------- */
/* Size classes (plan §5.3)                                                */
/* ---------------------------------------------------------------------- */

static uint32_t g_class_size[SOHAM_NCLASSES];
static uint32_t g_class_page[SOHAM_NCLASSES];   /* page bytes */
static uint32_t g_class_cap[SOHAM_NCLASSES];
static uint8_t  g_class_kind[SOHAM_NCLASSES];
static uint8_t  g_lut[SOHAM_LUT_MAX / 16 + 1] __attribute__((aligned(64)));

static size_t class_size_formula(unsigned c) {
  if (c < 8) return 16u * (c + 1);
  if (c < 32) {
    unsigned g = (c - 8) / 8;                          /* 128-256, 256-512, 512-1024 */
    return ((size_t)128 << g) + (size_t)((c - 8) % 8 + 1) * ((size_t)16 << g);
  }
  unsigned lg = 10 + (c - 32) / 4;
  return ((size_t)1 << lg) + (size_t)((c - 32) % 4 + 1) * ((size_t)1 << (lg - 2));
}

/* Size -> class for 1024 < n <= 256 KiB (plan §5.3 formula). */
SOHAM_INLINE unsigned size_class_large(size_t n) {
  size_t m = n - 1;
  unsigned lg = 63u - (unsigned)__builtin_clzll((unsigned long long)m);
  return 32u + 4u * (lg - 10u) + (unsigned)((m >> (lg - 2)) & 3u);
}

SOHAM_INLINE unsigned size_class(size_t n) {
  if (likely(n <= SOHAM_LUT_MAX)) return g_lut[(n + 15) >> 4];
  if (n <= SOHAM_MAX_CLASS) return size_class_large(n);
  return SOHAM_NCLASSES;
}

/* Medium page rule (plan §5.3): smallest slice multiple P with P >= 4*bsize
 * and P <= 1 MiB whose tail waste is <= 3.125%; otherwise the P <= 1 MiB
 * with the least tail waste. */
static size_t medium_page_size(size_t bsize, size_t S) {
  size_t best = 0, best_waste = SIZE_MAX;
  for (size_t P = S; P <= MiB; P += S) {
    if (P < 4 * bsize) continue;
    size_t waste = P % bsize;
    if (waste * 32 <= P) return P;
    if (waste < best_waste) { best_waste = waste; best = P; }
  }
  if (best == 0) best = ((4 * bsize + S - 1) / S) * S;
  return best;
}

static void classes_init(size_t S) {
  for (unsigned c = 0; c < SOHAM_NCLASSES; c++) {
    size_t bs = class_size_formula(c);
    g_class_size[c] = (uint32_t)bs;
    if (c < SOHAM_NSMALL) {
      g_class_kind[c] = KIND_SMALL;
      g_class_page[c] = (uint32_t)S;
    } else if (c == SOHAM_CLASS_RUN_A || c == SOHAM_CLASS_RUN_B) {
      g_class_kind[c] = KIND_RUN;
      g_class_page[c] = (uint32_t)(((bs + S - 1) / S) * S);
    } else {
      g_class_kind[c] = KIND_MEDIUM;
      g_class_page[c] = (uint32_t)medium_page_size(bs, S);
    }
    g_class_cap[c] = (uint32_t)(g_class_page[c] / bs);
  }
  unsigned c = 0;
  for (unsigned i = 0; i <= SOHAM_LUT_MAX / 16; i++) {
    size_t n = (size_t)i * 16;
    while (g_class_size[c] < n) c++;
    g_lut[i] = (uint8_t)c;
  }
}

/* ---------------------------------------------------------------------- */
/* Page heap state                                                         */
/* ---------------------------------------------------------------------- */

typedef struct {
  soham_lock_t lock;
  window_t *windows;            /* creation order */
  window_t *last;
  uint32_t nwindows;
  uint64_t bucket_mask;         /* bit b set: bucket[b] non-empty (b = 1..32) */
  hbmeta_t *bucket[HB_SLICES + 1];
  uint64_t last_purge_ns;
  /* Recent peak of s_used: the maximum over this decay epoch and the last
   * one (see ph_dirty_limit). */
  size_t s_peak_cur, s_peak_prev;
  uint64_t peak_epoch_ns;
  /* statistics, in slices */
  size_t s_used, s_dirty, s_zero, s_lazy, s_releasing;
  size_t committed_data;        /* bytes */
  size_t committed_meta;        /* bytes */
} pageheap_t;

static pageheap_t g_ph __attribute__((aligned(64)));

SOHAM_INLINE uintptr_t hb_addr(const hbmeta_t *hb) {
  return hb->win->base + g_geo.data_off + (uintptr_t)hb->idx * g_geo.H;
}

static unsigned longest_run(uint32_t m) {
  unsigned best = 0;
  uint64_t x = m;
  while (x) {
    unsigned s = (unsigned)__builtin_ctzll(x);
    uint64_t y = ~(x >> s);
    unsigned len = (unsigned)__builtin_ctzll(y);  /* y != 0: bits >= 32 are 1 */
    if (len > best) best = len;
    if (s + len >= 64) break;
    x &= ~((((uint64_t)1 << len) - 1) << s);
  }
  return best;
}

/* Start of the smallest run of >= k set bits (lowest address on ties). */
static int best_fit(uint32_t m, unsigned k) {
  int best = -1;
  unsigned best_len = 64;
  uint64_t x = m;
  while (x) {
    unsigned s = (unsigned)__builtin_ctzll(x);
    unsigned len = (unsigned)__builtin_ctzll(~(x >> s));
    if (len >= k && len < best_len) { best_len = len; best = (int)s; if (len == k) break; }
    x &= ~((((uint64_t)1 << len) - 1) << s);
  }
  return best;
}

SOHAM_INLINE uint32_t hb_free_mask(const hbmeta_t *hb) { return ~(hb->used | hb->releasing); }

static void bucket_remove(hbmeta_t *hb) {
  unsigned b = hb->bucket;
  if (!b) return;
  if (hb->bprev) hb->bprev->bnext = hb->bnext; else g_ph.bucket[b] = hb->bnext;
  if (hb->bnext) hb->bnext->bprev = hb->bprev;
  if (!g_ph.bucket[b]) g_ph.bucket_mask &= ~((uint64_t)1 << b);
  hb->bprev = hb->bnext = NULL;
  hb->bucket = 0;
}

/* Recompute the longest allocatable run and move hb to its bucket.
 * Hugeblocks with nothing allocatable are on no list. */
static void hb_rebucket(hbmeta_t *hb) {
  unsigned l = longest_run(hb_free_mask(hb));
  hb->longest = (uint8_t)l;
  if (hb->bucket == l) return;
  bucket_remove(hb);
  if (l == 0) return;
  hb->bnext = g_ph.bucket[l];
  hb->bprev = NULL;
  if (hb->bnext) hb->bnext->bprev = hb;
  g_ph.bucket[l] = hb;
  g_ph.bucket_mask |= (uint64_t)1 << l;
  hb->bucket = (uint8_t)l;
}

/* ---------------------------------------------------------------------- */
/* Windows and commit growth                                               */
/* ---------------------------------------------------------------------- */

static uint64_t g_rng_seed;

static uint64_t mix64(uint64_t z) {
  z += 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

/* Create a window (ph.lock held). */
static window_t *window_create(void) {
  if (g_ph.nwindows >= 255) return NULL;
  uintptr_t hint = 0;
#if SOHAM_64BIT
  /* A random W-aligned hint in [2^40, 2^46): far from the usual mmap area
   * and below 2^47 on every 4-level/48-bit target (§3, plan §5.1). */
  uint64_t r = mix64(g_rng_seed + 0x51ed27ull * (g_ph.nwindows + 1));
  uintptr_t lo = (uintptr_t)1 << 40, span = ((uintptr_t)1 << 46) - lo;
  hint = (lo + (uintptr_t)(r % span)) & ~g_geo.wmask;
  if (SOHAM_TSAN) hint = 0;
#endif
  void *p = NULL;
  for (int tries = 0; tries < 4 && !p; tries++) {
    p = soham_os_reserve_aligned(g_geo.W, g_geo.W, hint);
#if SOHAM_64BIT
    if (hint) {
      hint = (hint + ((uintptr_t)(mix64(r + (uint64_t)tries) % 61 + 1) << g_geo.wshift)) & (((uintptr_t)1 << 46) - 1) & ~g_geo.wmask;
      if (hint < ((uintptr_t)1 << 40)) hint += (uintptr_t)1 << 40;
    }
#endif
  }
  if (!p) return NULL;
  uintptr_t base = (uintptr_t)p;
  if ((base >> g_geo.wshift) >= g_gmap_n_pending) { soham_os_unmap(p, g_geo.W); return NULL; }
  if (soham_os_commit(p, g_geo.meta_size) != 0) { soham_os_unmap(p, g_geo.W); return NULL; }
  soham_os_advise_nohuge(p, g_geo.meta_size);
  window_t *w = (window_t *)p;
  w->magic = WINDOW_MAGIC ^ (uint64_t)g_secret_word();
  w->base = base;
  w->hb = (hbmeta_t *)(base + g_geo.B);
  w->nhb = 0;
  w->nhb_max = g_geo.nhb_max;
  w->index = g_ph.nwindows;
  w->next = NULL;
  /* hb[i].win and .idx are set when hugeblock i is committed (ph_grow), so
   * window creation touches none of the 2047 metadata entries. */
  if (g_ph.last) g_ph.last->next = w; else g_ph.windows = w;
  g_ph.last = w;
  g_ph.nwindows++;
  g_ph.committed_meta += g_geo.meta_size;
  /* Publish: free() of pointers in this window becomes possible only after
   * the creating thread hands out blocks, which happens after this store. */
  atomic_store_explicit(&atomic_load_explicit(&g_geo.gmap, memory_order_relaxed)[base >> g_geo.wshift], GM_WINDOW, memory_order_release);
  return w;
}

/* Commit at least n more hugeblocks in some window (ph.lock held).
 * New hugeblocks are free and zero.  Returns false on exhaustion. */
static bool ph_grow(uint32_t n) {
  window_t *w = g_ph.last;
  if (!w || w->nhb + n > w->nhb_max) {
    w = window_create();
    if (!w) return false;
    if (n > w->nhb_max) return false;
  }
  uint32_t step = w->nhb < 2 ? 2 : (w->nhb > 32 ? 32 : w->nhb);   /* doubling, 4 MiB .. 64 MiB */
  if (step < n) step = n;
  if (w->nhb + step > w->nhb_max) step = w->nhb_max - w->nhb;
  if (step < n) return false;
  uintptr_t from = w->base + g_geo.data_off + (uintptr_t)w->nhb * g_geo.H;
  size_t len = (size_t)step * g_geo.H;
  if (soham_os_commit((void *)from, len) != 0) {
    /* Commit refused (overcommit accounting): try exactly n. */
    step = n;
    len = (size_t)step * g_geo.H;
    if (soham_os_commit((void *)from, len) != 0) return false;
  }
  if (g_cfg.thp_huge) soham_os_advise_huge((void *)from, len);
  for (uint32_t i = 0; i < step; i++) {
    hbmeta_t *hb = &w->hb[w->nhb + i];
    hb->win = w;
    hb->idx = w->nhb + i;
    hb->used = 0; hb->dirty = 0; hb->zero = HB_FULL; hb->releasing = 0;
    hb->dirty_since = 0; hb->bucket = 0; hb->bprev = hb->bnext = NULL;
    hb_rebucket(hb);
  }
  w->nhb += step;
  g_ph.s_zero += (size_t)step * HB_SLICES;
  g_ph.committed_data += len;
  return true;
}

/* ---------------------------------------------------------------------- */
/* Slice allocation                                                        */
/* ---------------------------------------------------------------------- */

/* Mark [s, s+k) of hb used; returns true when all were known zero. */
static bool hb_take(hbmeta_t *hb, unsigned s, unsigned k) {
  uint32_t bits = (k >= 32) ? HB_FULL : (((uint32_t)1 << k) - 1) << s;
  SOHAM_ASSERT((hb->used & bits) == 0 && (hb->releasing & bits) == 0);
  bool zero = (hb->zero & bits) == bits;
  g_ph.s_dirty -= (size_t)__builtin_popcount(hb->dirty & bits);
  g_ph.s_zero  -= (size_t)__builtin_popcount(hb->zero & bits);
  g_ph.s_lazy  -= (size_t)__builtin_popcount(bits & ~hb->dirty & ~hb->zero);
  hb->used |= bits;
  hb->dirty &= ~bits;
  hb->zero &= ~bits;
  if (!hb->dirty) hb->dirty_since = 0;
  g_ph.s_used += k;
  if (g_ph.s_used > g_ph.s_peak_cur) g_ph.s_peak_cur = g_ph.s_used;
  hb_rebucket(hb);
  return zero;
}

static void hb_give(hbmeta_t *hb, unsigned s, unsigned k, uint64_t now) {
  uint32_t bits = (k >= 32) ? HB_FULL : (((uint32_t)1 << k) - 1) << s;
  SOHAM_ASSERT((hb->used & bits) == bits);
  hb->used &= ~bits;
  if (!hb->dirty) hb->dirty_since = now ? now : 1;
  hb->dirty |= bits;
  g_ph.s_used -= k;
  g_ph.s_dirty += k;
  hb_rebucket(hb);
}

/* Release modes: decay (age >= decay_ns), excess (down to the dirty limit,
 * whole free hugeblocks first), all (soham_collect(1), malloc_trim). */
enum { PURGE_DECAY = 0, PURGE_ALL = 1, PURGE_EXCESS = 2 };
static void ph_purge(int mode);

/* Free resident memory the page heap may keep (§6 profiles):
 * max(dirty_min, used/dirty_div, recent swing), where the recent swing is
 * how far s_used fell below its peak of the last one or two decay epochs,
 * capped at used*peak_num/4.  A program whose live size oscillates (free a
 * 20 MiB buffer, allocate another) keeps the memory it is about to reuse
 * instead of releasing and re-faulting it on every cycle; the decay engine
 * still returns it once the swing stops.  The cap keeps a program that
 * frees nearly everything from holding its old peak. */
SOHAM_INLINE size_t ph_dirty_limit(void) {
  size_t used = g_ph.s_used;
  size_t lim = (used << SOHAM_SLICE_SHIFT) / g_cfg.dirty_div;
  if (lim < g_cfg.dirty_min) lim = g_cfg.dirty_min;
  size_t peak = g_ph.s_peak_cur > g_ph.s_peak_prev ? g_ph.s_peak_cur : g_ph.s_peak_prev;
  if (peak > used) {
    size_t swing = peak - used, cap = used * g_cfg.peak_num / 4;
    if (swing > cap) swing = cap;
    swing <<= SOHAM_SLICE_SHIFT;
    if (swing > lim) lim = swing;
  }
  return lim;
}

/* Start a new peak epoch every decay_ns (lock held). */
SOHAM_INLINE void ph_peak_roll(uint64_t now) {
  if (now - g_ph.peak_epoch_ns < g_cfg.decay_ns) return;
  /* More than one epoch without a roll: the old peak is stale too. */
  g_ph.s_peak_prev = now - g_ph.peak_epoch_ns < 2 * g_cfg.decay_ns ? g_ph.s_peak_cur : g_ph.s_used;
  g_ph.s_peak_cur = g_ph.s_used;
  g_ph.peak_epoch_ns = now;
}

/* Called after a page-heap operation (lock not held): run the release
 * engine when the dirty limit is exceeded or a decay interval passed. */
SOHAM_INLINE void ph_after_op(bool excess, bool decay_due) {
  if (excess) ph_purge(PURGE_EXCESS);
  else if (decay_due) ph_purge(PURGE_DECAY);
}

/* In handler context (tls.trymode) every allocator lock is a trylock, so
 * a handler never waits for a lock its own interrupted thread may hold. */
SOHAM_INLINE bool lock_or_try(soham_lock_t *l) {
  if (unlikely(t_tls.trymode)) return soham_trylock(l);
  soham_lock(l);
  return true;
}

/* Allocate k (1..32) contiguous slices inside one hugeblock. */
static uintptr_t ph_alloc_slices(unsigned k, bool *zero) {
  if (!lock_or_try(&g_ph.lock)) return 0;
  hbmeta_t *hb = NULL;
  for (;;) {
    uint64_t m = g_ph.bucket_mask >> k;
    if (m) { hb = g_ph.bucket[k + (unsigned)__builtin_ctzll(m)]; break; }
    if (!ph_grow(1)) { soham_unlock(&g_ph.lock); return 0; }
  }
  int s = best_fit(hb_free_mask(hb), k);
  SOHAM_ASSERT(s >= 0);
  *zero = hb_take(hb, (unsigned)s, k);
  uintptr_t a = hb_addr(hb) + ((uintptr_t)s << SOHAM_SLICE_SHIFT);
  bool decay_due = g_ph.s_dirty && !t_tls.trymode && soham_os_now_ns() - g_ph.last_purge_ns >= g_cfg.purge_interval_ns;
  soham_unlock(&g_ph.lock);
  if (unlikely(decay_due)) ph_after_op(false, true);
  return a;
}

/* Allocate k slices starting on a hugeblock boundary (k may exceed 32).
 * Among the runs of free hugeblocks long enough, take the one with the
 * most resident (dirty) slices: reusing them costs no page faults, and a
 * fully dirty run ends the scan. */
static uintptr_t ph_alloc_hbs(size_t k, bool *zero) {
  uint32_t nh = (uint32_t)((k + HB_SLICES - 1) / HB_SLICES);
  if (!lock_or_try(&g_ph.lock)) return 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    window_t *bw = NULL;
    uint32_t bfirst = 0;
    /* int64_t, not long: with a 32-bit long (ILP32, LLP64) "long * 32u" is
     * unsigned and bscore = -1 would compare as the largest value. */
    const int64_t perfect = (int64_t)nh * (int64_t)HB_SLICES;
    int64_t bscore = -1;
    for (window_t *w = g_ph.windows; w && bscore < perfect; w = w->next) {
      uint32_t run = 0;
      int64_t score = 0;
      for (uint32_t i = 0; i < w->nhb; i++) {
        hbmeta_t *hb = &w->hb[i];
        if (hb->used != 0 || hb->releasing != 0) { run = 0; score = 0; continue; }
        score += __builtin_popcount(hb->dirty);
        if (++run > nh) score -= __builtin_popcount(w->hb[i - nh].dirty);
        if (run >= nh && score > bscore) {
          bw = w; bfirst = i + 1 - nh; bscore = score;
          if (score == perfect) break;
        }
      }
    }
    if (bw) {
      /* Take exactly k slices: the last hugeblock's unused tail keeps its
       * state (untouched or released memory is not counted as dirty). */
      bool z = true;
      size_t left = k;
      for (uint32_t j = bfirst; j < bfirst + nh; j++) {
        unsigned take = left < HB_SLICES ? (unsigned)left : HB_SLICES;
        z &= hb_take(&bw->hb[j], 0, take);
        left -= take;
      }
      *zero = z;
      uintptr_t a = bw->base + g_geo.data_off + (uintptr_t)bfirst * g_geo.H;
      soham_unlock(&g_ph.lock);
      return a;
    }
    /* Grow the last window (its free top hugeblocks extend the run). */
    if (!ph_grow(nh)) break;
  }
  soham_unlock(&g_ph.lock);
  return 0;
}

/* Fork child: a release pass of another thread was cut off between its
 * unlocked syscall and purge_finish.  Its ranges were possibly released:
 * make them free, neither dirty nor known zero. */
static void ph_fork_child(void) {
  for (window_t *w = g_ph.windows; w; w = w->next) {
    for (uint32_t i = 0; i < w->nhb; i++) {
      hbmeta_t *hb = &w->hb[i];
      if (!hb->releasing) continue;
      unsigned cnt = (unsigned)__builtin_popcount(hb->releasing);
      hb->releasing = 0;
      g_ph.s_releasing -= cnt;
      g_ph.s_lazy += cnt;
      hb_rebucket(hb);
    }
  }
}

/* Return k slices starting at address a (may span hugeblocks). */
static void ph_free(uintptr_t a, size_t k) {
  uint64_t now = soham_os_now_ns();
  bool purge = false;
  soham_lock(&g_ph.lock);
  window_t *w = (window_t *)(a & ~g_geo.wmask);
  size_t off = a - w->base - g_geo.data_off;
  uint32_t i = (uint32_t)(off / g_geo.H);
  unsigned s = (unsigned)((off % g_geo.H) >> SOHAM_SLICE_SHIFT);
  while (k > 0) {
    unsigned take = HB_SLICES - s;
    if (take > k) take = (unsigned)k;
    hb_give(&w->hb[i], s, take, now);
    k -= take;
    s = 0;
    i++;
  }
  if (now - g_ph.last_purge_ns >= g_cfg.purge_interval_ns) purge = true;
  ph_peak_roll(now);
  /* Batch: act once the excess reaches 1/8 of the limit (at least 1 MiB). */
  size_t lim = ph_dirty_limit();
  bool excess = (g_ph.s_dirty << SOHAM_SLICE_SHIFT) > lim + (lim / 8 > MiB ? lim / 8 : MiB);
  soham_unlock(&g_ph.lock);
  ph_after_op(excess, purge);
}

/* ---------------------------------------------------------------------- */
/* Release (decay) engine (§6)                                             */
/* ---------------------------------------------------------------------- */

#define PURGE_MAX_RANGES 64

typedef struct { window_t *w; uint32_t hb_first; uint32_t s_first; uint32_t nslices; } purge_span_t;

/* Mark each slice of span as releasing->released. */
static void purge_finish(const purge_span_t *sp, int zeroed) {
  window_t *w = sp->w;
  uint32_t i = sp->hb_first, s = sp->s_first;
  size_t k = sp->nslices;
  while (k > 0) {
    unsigned take = HB_SLICES - s;
    if (take > k) take = (unsigned)k;
    hbmeta_t *hb = &w->hb[i];
    uint32_t bits = (take >= 32) ? HB_FULL : (((uint32_t)1 << take) - 1) << s;
    hb->releasing &= ~bits;
    if (zeroed) { hb->zero |= bits; g_ph.s_zero += take; }
    else g_ph.s_lazy += take;
    g_ph.s_releasing -= take;
    hb_rebucket(hb);
    k -= take;
    s = 0;
    i++;
  }
}

/* Run the release engine in the given mode (see PURGE_*).  Never makes a
 * syscall while holding ph.lock: ranges are marked `releasing` (neither
 * free nor used), released unlocked, then marked free again. */
static void ph_purge(int mode) {
  purge_span_t spans[PURGE_MAX_RANGES];
  soham_os_range_t ranges[PURGE_MAX_RANGES];
  for (int round = 0; round < 256; round++) {
    size_t n = 0;
    bool more = false;
    uint64_t now = soham_os_now_ns();
    soham_lock(&g_ph.lock);
    if (mode == PURGE_DECAY && round == 0) {
      if (now - g_ph.last_purge_ns < g_cfg.purge_interval_ns) { soham_unlock(&g_ph.lock); return; }
      g_ph.last_purge_ns = now;
      ph_peak_roll(now);
    }
    size_t lim = ph_dirty_limit();
    size_t want = 0;   /* PURGE_EXCESS: slices still to release */
    if (mode == PURGE_EXCESS) {
      size_t dirty = g_ph.s_dirty << SOHAM_SLICE_SHIFT;
      if (dirty <= lim) { soham_unlock(&g_ph.lock); return; }
      want = (dirty - lim) >> SOHAM_SLICE_SHIFT;
    }
    /* Excess mode makes two passes: wholly free hugeblocks first (one range
     * each, no THP split), then partially used ones. */
    for (int pass = (mode == PURGE_EXCESS ? 0 : 1); pass < 2 && !more; pass++) {
      for (window_t *w = g_ph.windows; w && !more; w = w->next) {
        for (uint32_t i = 0; i < w->nhb; i++) {
          hbmeta_t *hb = &w->hb[i];
          if (!hb->dirty) continue;
          if (pass == 0 && (hb->used | hb->releasing)) continue;
          if (mode == PURGE_DECAY && now - hb->dirty_since < g_cfg.decay_ns) continue;
          if (mode == PURGE_EXCESS && want == 0) { more = false; goto done_scan; }
          if (n == PURGE_MAX_RANGES) { more = true; break; }
          uint32_t d = hb->dirty;
          unsigned cnt = (unsigned)__builtin_popcount(d);
          hb->releasing |= d;
          hb->dirty = 0;
          hb->dirty_since = 0;
          g_ph.s_dirty -= cnt;
          g_ph.s_releasing += cnt;
          want = want > cnt ? want - cnt : 0;
          hb_rebucket(hb);
          /* One span per maximal run of dirty slices; merge with the previous
           * span when it ends exactly where this run starts. */
          uint64_t x = d;
          while (x) {
            unsigned s = (unsigned)__builtin_ctzll(x);
            unsigned len = (unsigned)__builtin_ctzll(~(x >> s));
            uint64_t runmask = (((uint64_t)1 << len) - 1) << s;
            x &= ~runmask;
            uintptr_t a = hb_addr(hb) + ((uintptr_t)s << SOHAM_SLICE_SHIFT);
            if (n > 0 && spans[n - 1].w == w &&
                (uintptr_t)ranges[n - 1].base + ranges[n - 1].len == a) {
              spans[n - 1].nslices += len;
              ranges[n - 1].len += (size_t)len << SOHAM_SLICE_SHIFT;
            } else if (n < PURGE_MAX_RANGES) {
              spans[n] = (purge_span_t){ w, i, s, len };
              ranges[n] = (soham_os_range_t){ (void *)a, (size_t)len << SOHAM_SLICE_SHIFT };
              n++;
            } else {
              /* Out of span slots mid-hugeblock: put the rest back as dirty. */
              uint32_t rest = (uint32_t)((x | runmask) & HB_FULL);
              unsigned rc = (unsigned)__builtin_popcount(rest);
              hb->releasing &= ~rest;
              hb->dirty |= rest;
              hb->dirty_since = now;
              g_ph.s_dirty += rc;
              g_ph.s_releasing -= rc;
              hb_rebucket(hb);
              x = 0;
              more = true;
            }
          }
        }
      }
    }
  done_scan:
    soham_unlock(&g_ph.lock);
    if (n == 0) return;
    int zeroed = soham_os_release(ranges, n, g_cfg.release_mode);
    soham_lock(&g_ph.lock);
    for (size_t j = 0; j < n; j++) purge_finish(&spans[j], zeroed);
    soham_unlock(&g_ph.lock);
    if (!more && mode != PURGE_EXCESS) return;
  }
}
