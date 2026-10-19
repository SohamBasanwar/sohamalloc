/*
 * producer_consumer -- cross-thread frees through lock-free queues.
 *
 * Phase 1 (producer/consumer with back-pressure): P producers allocate
 *   blocks, stamp a header and a seeded pattern, and push them on a bounded
 *   MPMC queue; C consumers pop, verify, sometimes realloc (a realloc of a
 *   block owned by another thread), and free.  Producers finish and exit
 *   while their blocks are still queued, so frees land on pages whose owner
 *   thread is gone (orphaned pages).
 * Phase 2 (all-to-all): T threads each own an inbox; every thread allocates
 *   and sends each block to a random inbox (its own included) and frees what
 *   arrives in its own inbox.  Termination is exact: every block allocated is
 *   freed exactly once and verified.
 *
 * The queue is Vyukov's bounded MPMC ring.  Each cell's sequence number
 * carries the hand-off: the producer writes `data` then stores seq=pos+1 with
 * release; the consumer's acquire load of seq makes `data` (and the block
 * contents written before the push) visible.  The enqueue/dequeue cursors
 * are only claimed with relaxed CAS because the cell sequence numbers, not
 * the cursors, publish data.
 */
#include "common/test_util.h"

#include <sched.h>

/* ------------------------------------------------------------- MPMC ring */
typedef struct {
    _Atomic size_t seq;
    void *data;
} cell_t;

typedef struct {
    cell_t *buf;
    size_t mask;
    _Alignas(64) _Atomic size_t enq;
    _Alignas(64) _Atomic size_t deq;
    char pad[64 - sizeof(size_t)];
} mpmc_t;

static void q_init(mpmc_t *q, size_t cap_pow2)
{
    q->buf = (cell_t *)tu_os_alloc(cap_pow2 * sizeof(cell_t));
    q->mask = cap_pow2 - 1;
    for (size_t i = 0; i < cap_pow2; i++) atomic_init(&q->buf[i].seq, i);
    atomic_init(&q->enq, 0);
    atomic_init(&q->deq, 0);
}

static void q_destroy(mpmc_t *q)
{
    tu_os_free(q->buf, (q->mask + 1) * sizeof(cell_t));
}

static int q_push(mpmc_t *q, void *v)
{
    size_t pos = atomic_load_explicit(&q->enq, memory_order_relaxed);
    cell_t *c;
    for (;;) {
        c = &q->buf[pos & q->mask];
        size_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->enq, &pos, pos + 1, memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
        } else if (dif < 0) {
            return 0;                                   /* full */
        } else {
            pos = atomic_load_explicit(&q->enq, memory_order_relaxed);
        }
    }
    c->data = v;
    atomic_store_explicit(&c->seq, pos + 1, memory_order_release);   /* publish */
    return 1;
}

static void *q_pop(mpmc_t *q)
{
    size_t pos = atomic_load_explicit(&q->deq, memory_order_relaxed);
    cell_t *c;
    for (;;) {
        c = &q->buf[pos & q->mask];
        size_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->deq, &pos, pos + 1, memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
        } else if (dif < 0) {
            return NULL;                                /* empty */
        } else {
            pos = atomic_load_explicit(&q->deq, memory_order_relaxed);
        }
    }
    void *v = c->data;
    atomic_store_explicit(&c->seq, pos + q->mask + 1, memory_order_release);  /* recycle cell */
    return v;
}

/* ------------------------------------------------------------- blocks */
/* Layout: [0] n, [1] seed, [2] n ^ seed ^ MAGIC, pattern over [24, n). */
#define HDR 24
#define MAGIC 0x50434d41474943ULL

static size_t pc_size(tu_rng *r)
{
    uint64_t k = tu_rng_below(r, 10000);
    if (k == 0 && sizeof(size_t) == 8) return (size_t)tu_rng_range(r, 33u << 20, 40u << 20);  /* solo */
    if (k < 100) return (size_t)tu_rng_logu(r, 262145, 2u << 20);                            /* runs */
    size_t n = tu_rand_size(r, 256 * 1024);
    return n < HDR ? HDR : n;
}

static void *make_block(tu_rng *r, uint64_t seed)
{
    size_t n = pc_size(r);
    uint64_t *p = (uint64_t *)T_MALLOC(n);
    if (!p) TU_FAIL("malloc(%zu) returned NULL", n);
    size_t a = tu_req_align(n);
    if ((uintptr_t)p & (a - 1)) TU_FAIL("malloc(%zu) = %p misaligned", n, (void *)p);
    p[0] = n;
    p[1] = seed;
    p[2] = n ^ seed ^ MAGIC;
    tu_fill((unsigned char *)p + HDR, n - HDR, seed);
    return p;
}

static size_t check_block(void *v, const char *who)
{
    uint64_t *p = (uint64_t *)v;
    size_t n = (size_t)p[0];
    uint64_t seed = p[1];
    if ((p[2] ^ n ^ seed) != MAGIC) TU_FAIL("%s: block %p header corrupted (n=%zu)", who, v, n);
    if (T_USABLE(v) < n) TU_FAIL("%s: block %p usable %zu < %zu", who, v, T_USABLE(v), n);
    size_t bad = tu_check((unsigned char *)p + HDR, n - HDR, n - HDR, seed);
    if (bad != SIZE_MAX) TU_FAIL("%s: block %p (n=%zu) corrupted at offset %zu", who, v, n, bad + HDR);
    return n;
}

/* Remote realloc: grow or shrink a block allocated by another thread; the
   header and the preserved part of the pattern are re-validated. */
static void *remote_realloc(void *v, tu_rng *r)
{
    size_t n = check_block(v, "pre-realloc");
    size_t m = HDR + (size_t)tu_rng_below(r, 2 * n);
    uint64_t *q = (uint64_t *)T_REALLOC(v, m);
    if (!q) TU_FAIL("realloc(%p, %zu) returned NULL", v, m);
    uint64_t seed = q[1];
    if ((q[2] ^ q[0] ^ seed) != MAGIC) TU_FAIL("realloc lost the header");
    size_t keep = (n < m ? n : m) - HDR;
    size_t bad = tu_check((unsigned char *)q + HDR, n - HDR, keep, seed);
    if (bad != SIZE_MAX) TU_FAIL("remote realloc %zu -> %zu lost content at %zu", n, m, bad + HDR);
    /* Re-stamp for the new size. */
    q[0] = m;
    q[2] = m ^ seed ^ MAGIC;
    tu_fill((unsigned char *)q + HDR, m - HDR, seed);
    return q;
}

/* ------------------------------------------------------------- phase 1 */
static mpmc_t q1;
static _Atomic size_t p1_produced, p1_consumed;
static _Atomic int p1_producers_left;
static size_t p1_per_producer;

static void *producer(void *arg)
{
    int id = (int)(intptr_t)arg;
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ (0x1000u + (uint64_t)id));
    void *burst[64];
    size_t made = 0;
    while (made < p1_per_producer) {
        /* Bursts: allocate a batch first, then push it (back-pressure when the
           ring is full: yield until consumers catch up). */
        size_t b = 1 + (size_t)tu_rng_below(&r, 64);
        if (b > p1_per_producer - made) b = p1_per_producer - made;
        for (size_t i = 0; i < b; i++)
            burst[i] = make_block(&r, tu_mix64(((uint64_t)id << 40) + made + i));
        for (size_t i = 0; i < b; i++) {
            while (!q_push(&q1, burst[i])) sched_yield();
        }
        made += b;
        atomic_fetch_add_explicit(&p1_produced, b, memory_order_relaxed);
        tu_tick();
    }
    atomic_fetch_sub_explicit(&p1_producers_left, 1, memory_order_release);
    return NULL;   /* thread exits while its blocks are still queued */
}

static void *consumer(void *arg)
{
    int id = (int)(intptr_t)arg;
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ (0x2000u + (uint64_t)id));
    for (;;) {
        void *v = q_pop(&q1);
        if (!v) {
            if (atomic_load_explicit(&p1_producers_left, memory_order_acquire) == 0) {
                /* All pushes happened before the producers' release
                   decrement, so one more pop that finds the ring empty
                   means it is drained for good. */
                v = q_pop(&q1);
                if (!v) break;
            } else {
                sched_yield();
                continue;
            }
        }
        if (tu_rng_below(&r, 10) == 0) v = remote_realloc(v, &r);
        check_block(v, "consumer");
        T_FREE(v);
        atomic_fetch_add_explicit(&p1_consumed, 1, memory_order_relaxed);
    }
    return NULL;
}

/* ------------------------------------------------------------- phase 2 */
enum { MAXT = 64 };
static mpmc_t inbox[MAXT];
static int p2_threads;
static size_t p2_per_thread;
static _Atomic int p2_done_sending;
static _Atomic size_t p2_allocated, p2_freed;

static size_t drain(int id, tu_rng *r, size_t max)
{
    size_t k = 0;
    void *v;
    while (k < max && (v = q_pop(&inbox[id])) != NULL) {
        if (tu_rng_below(r, 16) == 0) v = remote_realloc(v, r);
        check_block(v, "all-to-all");
        T_FREE(v);
        k++;
    }
    if (k) atomic_fetch_add_explicit(&p2_freed, k, memory_order_relaxed);
    return k;
}

static void *all_to_all(void *arg)
{
    int id = (int)(intptr_t)arg;
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ (0x3000u + (uint64_t)id));
    for (size_t i = 0; i < p2_per_thread; i++) {
        void *v = make_block(&r, tu_mix64(((uint64_t)id << 40) + i));
        atomic_fetch_add_explicit(&p2_allocated, 1, memory_order_relaxed);
        int t = (int)tu_rng_below(&r, (uint64_t)p2_threads);
        while (!q_push(&inbox[t], v)) {
            if (!drain(id, &r, 64)) sched_yield();     /* make room by freeing our own inbox */
        }
        if ((i & 7) == 0) drain(id, &r, 16);
        if ((i & 1023) == 0) tu_tick();
    }
    atomic_fetch_add_explicit(&p2_done_sending, 1, memory_order_release);
    /* Every push by every thread happens-before its release increment; once
       all have incremented, an empty inbox stays empty. */
    while (atomic_load_explicit(&p2_done_sending, memory_order_acquire) < p2_threads) {
        if (!drain(id, &r, 256)) sched_yield();
    }
    while (drain(id, &r, SIZE_MAX)) { }
    return NULL;
}

int main(int argc, char **argv)
{
    tu_init("producer_consumer", argc, argv);
    int np = 4, nc = 4;
    p2_threads = 8;
    for (int a = 1; a < argc; a++) {
        if (strncmp(argv[a], "--producers=", 12) == 0) np = atoi(argv[a] + 12);
        if (strncmp(argv[a], "--consumers=", 12) == 0) nc = atoi(argv[a] + 12);
        if (strncmp(argv[a], "--threads=", 10) == 0) p2_threads = atoi(argv[a] + 10);
    }
    if (np < 1) np = 1;
    if (nc < 1) nc = 1;
    if (p2_threads < 2 || p2_threads > MAXT) p2_threads = 8;
    pthread_t wd = tu_thread_start(tu_watchdog_main, NULL);

    /* Phase 1 */
    double t0 = tu_now();
    p1_per_producer = tu_scaled_min(250000, 2000);
    q_init(&q1, 1024);
    atomic_store(&p1_producers_left, np);
    pthread_t th[2 * MAXT];
    for (int i = 0; i < nc; i++) th[i] = tu_thread_start(consumer, (void *)(intptr_t)i);
    for (int i = 0; i < np; i++) th[nc + i] = tu_thread_start(producer, (void *)(intptr_t)i);
    for (int i = 0; i < np + nc; i++) tu_thread_join(th[i]);
    size_t prod = atomic_load(&p1_produced), cons = atomic_load(&p1_consumed);
    TU_CHECK(prod == (size_t)np * p1_per_producer, "produced %zu", prod);
    TU_CHECK(cons == prod, "consumed %zu of %zu blocks", cons, prod);
    TU_CHECK(q_pop(&q1) == NULL, "queue not empty at the end");
    q_destroy(&q1);
    double t1 = tu_now();
    tu_info("phase 1: %d producers -> %d consumers, %zu blocks, %.2fs", np, nc, prod, t1 - t0);

    /* Phase 2 */
    p2_per_thread = tu_scaled_min(150000, 2000);
    for (int i = 0; i < p2_threads; i++) q_init(&inbox[i], 256);
    for (int i = 0; i < p2_threads; i++) th[i] = tu_thread_start(all_to_all, (void *)(intptr_t)i);
    for (int i = 0; i < p2_threads; i++) tu_thread_join(th[i]);
    size_t al = atomic_load(&p2_allocated), fr = atomic_load(&p2_freed);
    TU_CHECK(al == (size_t)p2_threads * p2_per_thread, "allocated %zu", al);
    TU_CHECK(fr == al, "freed %zu of %zu blocks", fr, al);
    for (int i = 0; i < p2_threads; i++) {
        TU_CHECK(q_pop(&inbox[i]) == NULL, "inbox %d not empty", i);
        q_destroy(&inbox[i]);
    }
    double t2 = tu_now();
    tu_info("phase 2: all-to-all over %d threads, %zu blocks, %.2fs", p2_threads, al, t2 - t1);

    atomic_store(&tu_watchdog_stop, 1);
    tu_thread_join(wd);
    tu_info("PASS %.2fs", t2 - t0);
    return 0;
}
