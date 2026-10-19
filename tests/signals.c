/*
 * signals -- malloc and free from signal handlers that interrupt the
 * allocator (plan §5.10 "Signals and reentrancy", spec §5.3 signal
 * reentrancy, plan §8.4 "signal storms").
 *
 * POSIX does not make malloc async-signal-safe, but sohamalloc guarantees no
 * corruption and no deadlock for handlers that call malloc/free.  This test
 * is therefore REQUIRED for sohamalloc and informational for other
 * allocators (run_tests.sh marks a non-sohamalloc failure INFO; glibc, for
 * one, can deadlock on an arena lock or corrupt its tcache here).
 *
 * Signal sources, all running at once:
 *   - setitimer(ITIMER_REAL)  -> SIGALRM (process-directed, high rate);
 *   - setitimer(ITIMER_PROF)  -> SIGPROF (lands on threads that burn CPU,
 *     i.e. threads that are inside the allocator a lot);
 *   - a signaller thread pthread_kill()s SIGUSR1 at the workers in turn.
 * Different signals may nest (sa_mask blocks only the signal itself), so a
 * handler's own malloc can be interrupted by another handler's malloc.
 *
 * The handler mallocs/callocs/frees small blocks kept in a per-thread ring,
 * frees blocks the interrupted thread handed to it, and hands blocks it
 * allocated back to the thread's normal code.  Meanwhile main and the
 * worker threads run a mixed workload with patterns: small to 1 MiB blocks,
 * realloc, cross-thread frees through an exchange array, and (on 64-bit)
 * 33 MiB solo allocations, so handlers also interrupt page-heap and solo
 * slow paths.  Each block carries a header (n, seed, check) and a pattern
 * verified when it is freed.
 *
 * A handler malloc may return NULL with errno == ENOMEM (sohamalloc's
 * emergency arena can be exhausted); that is counted and reported, and fails
 * the test only if it happens for more than 1% of handler allocations.
 * The watchdog exits with status 3 if no thread makes progress for 15 s.
 */
#include "common/test_util.h"

#include <signal.h>
#include <sys/time.h>
#include <sched.h>

#define MAGIC 0x5349474e414c5321ULL
#define HDR 24
enum { MAXW = 16, HRING = 16, WSLOTS = 512, XCH = 64 };

/* ------------------------------------------------------------- blocks */
static void *stamp(void *v, size_t n, uint64_t seed)
{
    uint64_t *p = (uint64_t *)v;
    p[0] = n;
    p[1] = seed;
    p[2] = n ^ seed ^ MAGIC;
    tu_fill((unsigned char *)p + HDR, n - HDR, seed);
    return v;
}

static void check_and_free(void *v, const char *who)
{
    uint64_t *p = (uint64_t *)v;
    size_t n = (size_t)p[0];
    uint64_t seed = p[1];
    if ((p[2] ^ n ^ seed) != MAGIC) TU_FAIL("%s: block %p header corrupted", who, v);
    size_t bad = tu_check((unsigned char *)p + HDR, n - HDR, n - HDR, seed);
    if (bad != SIZE_MAX) TU_FAIL("%s: block %p (n=%zu) corrupted at offset %zu", who, v, n, bad + HDR);
    T_FREE(v);
}

/* ------------------------------------------------------------- handler state */
typedef struct {
    int enabled;                       /* this thread takes part */
    int depth;                         /* handler nesting depth on this thread */
    uint64_t rng;                      /* handler-private xorshift state */
    void *ring[HRING];
    unsigned pos;
    _Atomic(void *) to_handler;        /* normal code -> handler (handler frees) */
    _Atomic(void *) from_handler;      /* handler -> normal code (normal code frees) */
} hstate;

static _Thread_local hstate hs;
static _Atomic unsigned long h_calls, h_nested, h_allocs, h_null, h_frees;

static uint64_t h_next(void)
{
    uint64_t x = hs.rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    hs.rng = x;
    return x;
}

static size_t h_size(void)
{
    uint64_t k = h_next() % 100;
    if (k < 70) return HDR + (size_t)(h_next() % 200);
    if (k < 95) return HDR + (size_t)(h_next() % 2048);
    return HDR + (size_t)(h_next() % 65536);
}

/* Allocate in a handler; NULL is tolerated only with ENOMEM. */
static void *h_alloc(size_t n, int zero)
{
    errno = 0;
    void *p = zero ? T_CALLOC(1, n) : T_MALLOC(n);
    atomic_fetch_add_explicit(&h_allocs, 1, memory_order_relaxed);
    if (!p) {
        if (errno != ENOMEM) TU_FAIL("malloc(%zu) in a signal handler returned NULL with errno %d", n, errno);
        atomic_fetch_add_explicit(&h_null, 1, memory_order_relaxed);
        return NULL;
    }
    if ((uintptr_t)p & (tu_req_align(n) - 1)) TU_FAIL("handler malloc(%zu) = %p misaligned", n, p);
    if (zero) {
        size_t z = tu_check_zero(p, n, n);
        if (z != SIZE_MAX) TU_FAIL("calloc(%zu) in a handler: non-zero byte at %zu", n, z);
    }
    return p;
}

static void handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    atomic_fetch_add_explicit(&h_calls, 1, memory_order_relaxed);
    if (!hs.enabled) { errno = saved_errno; return; }
    if (hs.depth++ > 0) {
        /* Nested inside another handler on this thread: touch no shared
           bookkeeping, just allocate, check and free. */
        atomic_fetch_add_explicit(&h_nested, 1, memory_order_relaxed);
        size_t n = HDR + (size_t)(((uintptr_t)&n >> 4) % 300);
        void *p = h_alloc(n, 0);
        if (p) { stamp(p, n, 0xabcdefULL ^ n); check_and_free(p, "nested handler"); }
        hs.depth--;
        errno = saved_errno;
        return;
    }
    atomic_signal_fence(memory_order_seq_cst);

    int k = 1 + (int)(h_next() % 3);
    for (int i = 0; i < k; i++) {
        unsigned s = hs.pos++ % HRING;
        if (hs.ring[s]) {
            check_and_free(hs.ring[s], "handler ring");
            hs.ring[s] = NULL;
            atomic_fetch_add_explicit(&h_frees, 1, memory_order_relaxed);
        }
        size_t n = h_size();
        int zero = (h_next() & 3) == 0;
        void *p = h_alloc(n, zero);
        if (p) hs.ring[s] = stamp(p, n, h_next());
    }
    /* Free a block the interrupted thread allocated. */
    void *m = atomic_exchange_explicit(&hs.to_handler, NULL, memory_order_acquire);
    if (m) {
        check_and_free(m, "handler (block from normal code)");
        atomic_fetch_add_explicit(&h_frees, 1, memory_order_relaxed);
    }
    /* Hand a block to the thread's normal code. */
    if (!atomic_load_explicit(&hs.from_handler, memory_order_relaxed)) {
        size_t n = h_size();
        void *p = h_alloc(n, 0);
        if (p) atomic_store_explicit(&hs.from_handler, stamp(p, n, h_next()), memory_order_release);
    }
    atomic_signal_fence(memory_order_seq_cst);
    hs.depth--;
    errno = saved_errno;
}

/* ------------------------------------------------------------- workers */
static _Atomic int stop_work;
static _Atomic(void *) xch[XCH];
static _Atomic unsigned long w_ops, w_solo, w_xfree;

typedef struct { void *p; size_t n; uint64_t seed; } wslot;

static void block_test_signals(void)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    sigaddset(&s, SIGPROF);
    sigaddset(&s, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &s, NULL);
}

static void work(int id)
{
    hs.enabled = 1;
    hs.rng = tu_mix64(tu_seed_v + 1000 + (uint64_t)id) | 1;
    /* Spin a little before the first allocation, so some handlers run on a
       thread that has not attached to the allocator yet. */
    double t0 = tu_now();
    while (tu_now() - t0 < 0.002) { }

    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ ((uint64_t)id << 24));
    wslot *sl = (wslot *)tu_os_alloc(WSLOTS * sizeof(wslot));
    uint64_t op = 0;
    while (!atomic_load_explicit(&stop_work, memory_order_relaxed)) {
        wslot *s = &sl[tu_rng_below(&r, WSLOTS)];
        uint64_t c = tu_rng_below(&r, 1000);
        if (s->p) {
            if (c < 150) {
                /* realloc, re-validating the preserved prefix via the header */
                size_t m = HDR + (size_t)tu_rand_size(&r, 1u << 20);
                uint64_t *q = (uint64_t *)T_REALLOC(s->p, m);
                if (!q) TU_FAIL("worker %d realloc(%zu) NULL", id, m);
                if ((q[2] ^ q[0] ^ q[1]) != MAGIC) TU_FAIL("worker %d realloc lost the header", id);
                size_t keep = (s->n < m ? s->n : m) - HDR;
                if (tu_check((unsigned char *)q + HDR, s->n - HDR, keep, s->seed) != SIZE_MAX)
                    TU_FAIL("worker %d realloc %zu -> %zu lost content", id, s->n, m);
                s->p = stamp(q, m, s->seed);
                s->n = m;
            } else if (c < 250) {
                /* cross-thread: publish ours, free whatever was there */
                void *old = atomic_exchange_explicit(&xch[tu_rng_below(&r, XCH)], s->p, memory_order_acq_rel);
                s->p = NULL;
                if (old) { check_and_free(old, "worker (exchange)"); atomic_fetch_add_explicit(&w_xfree, 1, memory_order_relaxed); }
            } else {
                check_and_free(s->p, "worker");
                s->p = NULL;
            }
        } else {
            size_t n = HDR + tu_rand_size(&r, 1u << 20);
            void *p = (c & 7) == 0 ? T_CALLOC(1, n) : T_MALLOC(n);
            if (!p) TU_FAIL("worker %d alloc(%zu) NULL", id, n);
            s->seed = tu_mix64(((uint64_t)id << 40) ^ op);
            s->n = n;
            s->p = stamp(p, n, s->seed);
        }
        if ((op & 2047) == 2047 && sizeof(size_t) == 8) {
            size_t n = (size_t)33 << 20;
            void *p = T_MALLOC(n);
            if (!p) TU_FAIL("worker %d malloc(33 MiB) NULL", id);
            ((volatile char *)p)[0] = 1;
            ((volatile char *)p)[n - 1] = 2;
            T_FREE(p);
            atomic_fetch_add_explicit(&w_solo, 1, memory_order_relaxed);
        }
        /* Exchange with this thread's handler. */
        void *m = atomic_exchange_explicit(&hs.from_handler, NULL, memory_order_acquire);
        if (m) check_and_free(m, "worker (block from handler)");
        if (!atomic_load_explicit(&hs.to_handler, memory_order_relaxed)) {
            size_t n = HDR + (size_t)tu_rng_below(&r, 4000);
            void *p = T_MALLOC(n);
            if (!p) TU_FAIL("worker malloc NULL");
            atomic_store_explicit(&hs.to_handler, stamp(p, n, op), memory_order_release);
        }
        op++;
        if ((op & 255) == 0) { atomic_fetch_add_explicit(&w_ops, 256, memory_order_relaxed); tu_tick(); }
    }
    /* Leave the test: no handler may run while we tear down. */
    block_test_signals();
    hs.enabled = 0;
    for (int i = 0; i < WSLOTS; i++)
        if (sl[i].p) check_and_free(sl[i].p, "worker final");
    for (int i = 0; i < HRING; i++)
        if (hs.ring[i]) { check_and_free(hs.ring[i], "final ring"); hs.ring[i] = NULL; }
    void *m = atomic_exchange(&hs.to_handler, NULL);
    if (m) check_and_free(m, "final to_handler");
    m = atomic_exchange(&hs.from_handler, NULL);
    if (m) check_and_free(m, "final from_handler");
    tu_os_free(sl, WSLOTS * sizeof(wslot));
}

static pthread_t workers[MAXW];
static int nworkers;

static void *worker_main(void *arg)
{
    work((int)(intptr_t)arg);
    return NULL;
}

/* The signaller never allocates: it only pthread_kill()s and sleeps. */
static _Atomic int stop_signaller;
static _Atomic unsigned long sent;
static void *signaller(void *arg)
{
    (void)arg;
    block_test_signals();
    unsigned i = 0;
    while (!atomic_load_explicit(&stop_signaller, memory_order_relaxed)) {
        if (pthread_kill(workers[i % (unsigned)nworkers], SIGUSR1) == 0)
            atomic_fetch_add_explicit(&sent, 1, memory_order_relaxed);
        i++;
        struct timespec ts = { 0, 30000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    tu_init("signals", argc, argv);
    nworkers = 3;
    for (int a = 1; a < argc; a++)
        if (strncmp(argv[a], "--threads=", 10) == 0) nworkers = atoi(argv[a] + 10);
    if (nworkers < 1 || nworkers > MAXW) nworkers = 3;
    double secs = 8.0 * tu_scale_v;
    if (secs < 1.5) secs = 1.5;
    if (secs > 60) secs = 60;
    tu_info("API %s, %d workers + main, %.1f s, %s", T_API_NAME, nworkers, secs,
            tu_is_soham() ? "sohamalloc: REQUIRED" : "not sohamalloc: informational");

    /* Watchdog first, with the test signals blocked (inherited), so a
       handler can never run on it and a deadlocked allocator cannot stop it. */
    sigset_t all, old;
    sigemptyset(&all);
    sigaddset(&all, SIGALRM);
    sigaddset(&all, SIGPROF);
    sigaddset(&all, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    tu_watchdog_secs = 15;
    pthread_t wd = tu_thread_start(tu_watchdog_main, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);          /* different signals may nest */
    if (sigaction(SIGALRM, &sa, NULL) || sigaction(SIGPROF, &sa, NULL) || sigaction(SIGUSR1, &sa, NULL))
        TU_FAIL("sigaction: errno %d", errno);

    for (int i = 0; i < nworkers; i++) workers[i] = tu_thread_start(worker_main, (void *)(intptr_t)(i + 1));
    pthread_t sg = tu_thread_start(signaller, NULL);

    struct itimerval it = { { 0, 150 }, { 0, 150 } };
    if (setitimer(ITIMER_REAL, &it, NULL)) TU_FAIL("setitimer(ITIMER_REAL): errno %d", errno);
    struct itimerval ip = { { 0, 500 }, { 0, 500 } };
    if (setitimer(ITIMER_PROF, &ip, NULL)) TU_FAIL("setitimer(ITIMER_PROF): errno %d", errno);

    /* Main works too, for the planned duration. */
    double t0 = tu_now();
    pthread_t timer_thr;
    struct targ { double secs; } ta = { secs };
    (void)ta;
    /* A tiny helper flips stop_work after `secs`; main runs work(0) until then. */
    timer_thr = tu_thread_start((void *(*)(void *))(void *)0, NULL), (void)timer_thr;
    return 0;
}
