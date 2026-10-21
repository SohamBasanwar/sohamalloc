/*
 * tools/sigstorm/sigstorm.c -- does an allocator survive allocation from
 * signal handlers that interrupt it?
 *
 * Workload threads run a randomized malloc / calloc / realloc /
 * posix_memalign / free mix, including frees of blocks that another thread
 * allocated, and keep a shadow of every block they own (size and fill byte).
 * A signaller thread, which never allocates, sends SIGUSR1 to the workload
 * threads at random intervals of a few microseconds, so signals land at
 * arbitrary points, including inside the allocator.  The handler calls the
 * allocator in one of several ways (--handler).  The main thread is the
 * watchdog: a workload thread that makes no progress for --stall-ms is a
 * stall (deadlock or livelock), and the workload's own checks report
 * corruption (a block whose contents changed, two live blocks that overlap,
 * calloc memory that is not zero).
 *
 * Result: one line on stdout, and the exit status
 *   0 survived    3 stall    4 corruption    2 usage error
 * A crash (the allocator aborting, or a segfault) kills the process with
 * that signal; tools/sigstorm/run.py records it.
 *
 *   sigstorm --handler=malloc|free|realloc|calloc|large|none
 *            [--seconds=10] [--interval-us=20] [--threads=2]
 *            [--stall-ms=2000] [--seed=1]
 *
 * --interval-us=0 sends no signals (a control run of the workload alone);
 * --handler=none sends signals whose handler does not allocate (a control
 * run of the signal delivery alone).  Both must survive under every
 * allocator, or the harness itself is at fault.
 *
 * Nothing on the hot path or in the report uses stdio or the allocator.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------ options */

enum { HK_NONE, HK_MALLOC, HK_FREE, HK_REALLOC, HK_CALLOC, HK_LARGE };
static const char *const hk_names[] = { "none", "malloc", "free", "realloc", "calloc", "large" };

static int opt_handler = HK_MALLOC;
static long opt_seconds = 10;
static long opt_interval_us = 20;
static int opt_threads = 2;
static long opt_stall_ms = 2000;
static unsigned long opt_seed = 1;

/* ------------------------------------------------- allocation-free output */

static char outbuf[1024];
static size_t outlen;

static void out_str(const char *s) {
    while (*s && outlen + 1 < sizeof outbuf) outbuf[outlen++] = *s++;
}
static void out_u64(uint64_t v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && outlen + 1 < sizeof outbuf) outbuf[outlen++] = t[--n];
}
static void out_hex(uint64_t v) {
    out_str("0x");
    char t[16];
    int n = 0;
    do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    while (n && outlen + 1 < sizeof outbuf) outbuf[outlen++] = t[--n];
}
static void out_kv(const char *k, uint64_t v) { out_str(" "); out_str(k); out_str("="); out_u64(v); }
static void out_ks(const char *k, const char *v) { out_str(" "); out_str(k); out_str("="); out_str(v); }
static void out_flush(void) {
    outbuf[outlen++] = '\n';
    size_t off = 0;
    while (off < outlen) {
        ssize_t w = write(1, outbuf + off, outlen - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* --------------------------------------------------------- thread state */

#define MAXLIVE 1024
#define HPOOL 32
#define XSLOTS 64

enum { OP_NONE, OP_MALLOC, OP_CALLOC, OP_REALLOC, OP_MEMALIGN, OP_FREE, OP_FREE_REMOTE };
static const char *const op_names[] = { "none", "malloc", "calloc", "realloc", "posix_memalign", "free", "free-remote" };

typedef struct { unsigned char *p; size_t n; unsigned char fill; } blk_t;

typedef struct {
    pthread_t th;
    int idx;
    uint64_t rng;
    _Atomic uint64_t steps, h_entered, h_done, h_null, h_bad;
    _Atomic int op, hop, in_handler;
    blk_t live[MAXLIVE];
    blk_t sorted[MAXLIVE];
    void *_Atomic hpool[HPOOL];          /* blocks the "free" handler releases */
    void *_Atomic xin[XSLOTS];           /* blocks other threads hand us to free */
} __attribute__((aligned(64))) ws_t;

static ws_t *ws;
static __thread ws_t *me;

static _Atomic int g_bad;                /* 0, or a corruption code */
static _Atomic int g_bad_thread;
static _Atomic uintptr_t g_bad_addr;
static _Atomic uint64_t g_signals;
static _Atomic int g_stop;
static pthread_barrier_t g_start;

enum { BAD_CONTENT = 1, BAD_OVERLAP, BAD_CALLOC, BAD_REALLOC, BAD_REMOTE, BAD_HANDLER };
static const char *const bad_names[] = { "none", "content", "overlap", "calloc-nonzero", "realloc-content",
                                         "remote-content", "handler-block" };

static inline uint64_t rnd(ws_t *w) {
    uint64_t x = w->rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return w->rng = x;
}

static void report_bad(ws_t *w, int code, const void *p) {
    int z = 0;
    if (atomic_compare_exchange_strong(&g_bad, &z, code)) {
        atomic_store(&g_bad_thread, w->idx);
        atomic_store(&g_bad_addr, (uintptr_t)p);
    }
    /* Park: the watchdog writes the report and exits. */
    for (;;) { struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL); }
}

/* ---------------------------------------------------------------- handler */

static void handler(int sig) {
    (void)sig;
    ws_t *w = me;
    if (!w) return;
    int e = errno;
    atomic_store(&w->in_handler, 1);
    atomic_fetch_add(&w->h_entered, 1);
    switch (opt_handler) {
    case HK_NONE:
        break;
    case HK_MALLOC: {
        atomic_store(&w->hop, OP_MALLOC);
        size_t n = 1 + (size_t)(atomic_load(&w->h_entered) * 37 % 600);
        unsigned char *p = malloc(n);
        if (!p) { atomic_fetch_add(&w->h_null, 1); break; }
        memset(p, 0x5a, n);
        atomic_store(&w->hop, OP_FREE);
        free(p);
        break;
    }
    case HK_FREE: {
        void *p = NULL;
        for (int i = 0; i < HPOOL && !p; i++) p = atomic_exchange(&w->hpool[i], NULL);
        if (!p) { atomic_fetch_add(&w->h_null, 1); break; }
        atomic_store(&w->hop, OP_FREE);
        free(p);
        break;
    }
    case HK_REALLOC: {
        atomic_store(&w->hop, OP_MALLOC);
        unsigned char *p = malloc(24);
        if (!p) { atomic_fetch_add(&w->h_null, 1); break; }
        memset(p, 0x5c, 24);
        atomic_store(&w->hop, OP_REALLOC);
        unsigned char *q = realloc(p, 3000);
        if (!q) { atomic_store(&w->hop, OP_FREE); free(p); atomic_fetch_add(&w->h_null, 1); break; }
        for (int i = 0; i < 24; i++) if (q[i] != 0x5c) { atomic_fetch_add(&w->h_bad, 1); break; }
        memset(q, 0x5b, 3000);
        atomic_store(&w->hop, OP_FREE);
        free(q);
        break;
    }
    case HK_CALLOC: {
        atomic_store(&w->hop, OP_CALLOC);
        unsigned char *p = calloc(1, 200);
        if (!p) { atomic_fetch_add(&w->h_null, 1); break; }
        for (int i = 0; i < 200; i++) if (p[i]) { atomic_fetch_add(&w->h_bad, 1); break; }
        memset(p, 0x5d, 200);
        atomic_store(&w->hop, OP_FREE);
        free(p);
        break;
    }
    case HK_LARGE: {
        atomic_store(&w->hop, OP_MALLOC);
        unsigned char *p = malloc(300000);
        if (!p) { atomic_fetch_add(&w->h_null, 1); break; }
        p[0] = 1; p[150000] = 2; p[299999] = 3;
        atomic_store(&w->hop, OP_FREE);
        free(p);
        break;
    }
    }
    atomic_store(&w->hop, OP_NONE);
    atomic_fetch_add(&w->h_done, 1);
    atomic_store(&w->in_handler, 0);
    errno = e;
}

/* --------------------------------------------------------------- workload */

/* Mostly small blocks (the fast paths), regular medium and large ones (page
 * refills, runs, mmap thresholds) and rare multi-megabyte ones. */
static size_t pick_size(ws_t *w) {
    uint64_t r = rnd(w), v = r >> 8;
    unsigned k = (unsigned)(r % 64);
    if (k < 32) return 1 + v % 128;
    if (k < 48) return 128 + v % 1920;
    if (k < 56) return 2048 + v % 30000;
    if (k < 62) return 32768 + v % 300000;
    if (k < 63) return 1 + v % 64;
    return (v % 4 == 0) ? ((size_t)2 << 20) + (size_t)(r >> 20) % ((size_t)4 << 20) : 300000 + v % 700000;
}

static int content_ok(const blk_t *b) {
    for (size_t i = 0; i < b->n; i++)
        if (b->p[i] != b->fill) return 0;
    return 1;
}

static void fill_blk(blk_t *b) {
    if (b->n) memset(b->p, b->fill, b->n);
}

/* A block handed to another thread carries its size and fill byte in its
 * first 16 bytes; the rest holds the fill byte. */
static void hand_over(ws_t *w, blk_t *b) {
    ws_t *to = &ws[(w->idx + 1) % opt_threads];
    memcpy(b->p, &b->n, sizeof b->n);
    b->p[8] = b->fill;
    for (int k = 0; k < XSLOTS; k++) {
        void *exp = NULL;
        if (atomic_compare_exchange_strong(&to->xin[k], &exp, b->p)) { b->p = NULL; return; }
    }
    atomic_store(&w->op, OP_FREE);
    free(b->p);                              /* the receiver is full: free it here */
    atomic_store(&w->op, OP_NONE);
    b->p = NULL;
}

static void drain_incoming(ws_t *w) {
    for (int k = 0; k < XSLOTS; k++) {
        unsigned char *p = atomic_exchange(&w->xin[k], NULL);
        if (!p) continue;
        size_t n;
        memcpy(&n, p, sizeof n);
        unsigned char f = p[8];
        for (size_t i = 16; i < n; i++)
            if (p[i] != f) report_bad(w, BAD_REMOTE, p);
        atomic_store(&w->op, OP_FREE_REMOTE);
        free(p);
        atomic_store(&w->op, OP_NONE);
    }
}

static void sift(blk_t *s, int i, int n) {
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n) return;
        if (c + 1 < n && s[c + 1].p > s[c].p) c++;
        if (s[c].p <= s[i].p) return;
        blk_t t = s[i]; s[i] = s[c]; s[c] = t;
        i = c;
    }
}

/* Contents of every live block of this thread, then no two overlap. */
static void full_check(ws_t *w) {
    int n = 0;
    for (int i = 0; i < MAXLIVE; i++) {
        if (!w->live[i].p) continue;
        if (!content_ok(&w->live[i])) report_bad(w, BAD_CONTENT, w->live[i].p);
        w->sorted[n++] = w->live[i];
    }
    for (int i = n / 2 - 1; i >= 0; i--) sift(w->sorted, i, n);
    for (int e = n - 1; e > 0; e--) {
        blk_t t = w->sorted[0]; w->sorted[0] = w->sorted[e]; w->sorted[e] = t;
        sift(w->sorted, 0, e);
    }
    for (int i = 1; i < n; i++) {
        uintptr_t end = (uintptr_t)w->sorted[i - 1].p + (w->sorted[i - 1].n ? w->sorted[i - 1].n : 1);
        if (end > (uintptr_t)w->sorted[i].p) report_bad(w, BAD_OVERLAP, w->sorted[i].p);
    }
}

static void step(ws_t *w) {
    blk_t *b = &w->live[rnd(w) % MAXLIVE];
    if (b->p) {
        if (!content_ok(b)) report_bad(w, BAD_CONTENT, b->p);
        uint64_t r = rnd(w) % 8;
        if (r == 0) {
            size_t n = pick_size(w);
            atomic_store(&w->op, OP_REALLOC);
            unsigned char *q = realloc(b->p, n);
            atomic_store(&w->op, OP_NONE);
            if (q) {
                size_t keep = b->n < n ? b->n : n;
                for (size_t k = 0; k < keep; k++)
                    if (q[k] != b->fill) report_bad(w, BAD_REALLOC, q);
                b->p = q; b->n = n; b->fill = (unsigned char)(rnd(w) | 1); fill_blk(b);
            }
            return;
        }
        if (r == 1 && opt_threads > 1 && b->n >= 16) { hand_over(w, b); return; }
        if (r == 2 && opt_handler == HK_FREE) {
            for (int k = 0; k < HPOOL; k++) {
                void *exp = NULL;
                if (atomic_compare_exchange_strong(&w->hpool[k], &exp, b->p)) { b->p = NULL; return; }
            }
        }
        atomic_store(&w->op, OP_FREE);
        free(b->p);
        atomic_store(&w->op, OP_NONE);
        b->p = NULL;
        return;
    }
    size_t n = pick_size(w);
    uint64_t how = rnd(w) % 16;
    if (how < 3) {
        atomic_store(&w->op, OP_CALLOC);
        b->p = calloc(1, n);
        atomic_store(&w->op, OP_NONE);
        if (b->p)
            for (size_t k = 0; k < n; k++)
                if (b->p[k]) report_bad(w, BAD_CALLOC, b->p);
    } else if (how < 4) {
        void *q = NULL;
        atomic_store(&w->op, OP_MEMALIGN);
        if (posix_memalign(&q, (size_t)64 << (rnd(w) % 6), n) == 0) b->p = q;
        atomic_store(&w->op, OP_NONE);
    } else {
        atomic_store(&w->op, OP_MALLOC);
        b->p = malloc(n);
        atomic_store(&w->op, OP_NONE);
    }
    if (!b->p) return;
    b->n = n;
    b->fill = (unsigned char)(rnd(w) | 1);
    fill_blk(b);
}

static void *worker(void *arg) {
    ws_t *w = arg;
    me = w;
    for (int k = 0; k < HPOOL; k++) atomic_store(&w->hpool[k], malloc(16 + rnd(w) % 200));
    pthread_barrier_wait(&g_start);
    for (uint64_t it = 1; !atomic_load_explicit(&g_stop, memory_order_relaxed); it++) {
        step(w);
        drain_incoming(w);
        if ((it & 4095) == 0) full_check(w);
        if (opt_handler == HK_FREE && (it & 31) == 0) {
            for (int k = 0; k < HPOOL; k++) {
                if (atomic_load(&w->hpool[k])) continue;
                atomic_store(&w->op, OP_MALLOC);
                void *q = malloc(16 + rnd(w) % 200);
                atomic_store(&w->op, OP_NONE);
                void *exp = NULL;
                if (q && !atomic_compare_exchange_strong(&w->hpool[k], &exp, q)) {
                    atomic_store(&w->op, OP_FREE);
                    free(q);
                    atomic_store(&w->op, OP_NONE);
                }
                break;
            }
        }
        atomic_store_explicit(&w->steps, it, memory_order_relaxed);
    }
    return NULL;
}

/* Sends SIGUSR1 to the workload threads in turn, waiting a uniformly random
 * 0 .. 2*interval microseconds (spinning on the vDSO clock) before each. */
static void *signaller(void *arg) {
    (void)arg;
    uint64_t r = opt_seed * 0x9e3779b97f4a7c15ull + 7;
    pthread_barrier_wait(&g_start);
    uint64_t span = (uint64_t)opt_interval_us * 2000u + 1;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        for (int i = 0; i < opt_threads; i++) {
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            uint64_t until = now_ns() + r % span;
            while (now_ns() < until) {}
            if (pthread_kill(ws[i].th, SIGUSR1) == 0) atomic_fetch_add_explicit(&g_signals, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

/* ----------------------------------------------------------------- report */

static void report(const char *outcome, uint64_t elapsed_ns) {
    outlen = 0;
    out_str("outcome="); out_str(outcome);
    out_ks("handler", hk_names[opt_handler]);
    out_kv("threads", (uint64_t)opt_threads);
    out_kv("interval_us", (uint64_t)opt_interval_us);
    out_kv("seed", opt_seed);
    out_kv("elapsed_ms", elapsed_ns / 1000000u);
    uint64_t steps = 0, he = 0, hd = 0, hn = 0, hb = 0;
    for (int i = 0; i < opt_threads; i++) {
        steps += atomic_load(&ws[i].steps);
        he += atomic_load(&ws[i].h_entered);
        hd += atomic_load(&ws[i].h_done);
        hn += atomic_load(&ws[i].h_null);
        hb += atomic_load(&ws[i].h_bad);
    }
    out_kv("signals", atomic_load(&g_signals));
    out_kv("handlers", hd);
    out_kv("handler_null", hn);
    out_kv("handler_bad", hb);
    out_kv("steps", steps);
    (void)he;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--handler=", 10)) {
            opt_handler = -1;
            for (int k = 0; k < (int)(sizeof hk_names / sizeof *hk_names); k++)
                if (!strcmp(a + 10, hk_names[k])) opt_handler = k;
        }
        else if (!strncmp(a, "--seconds=", 10)) opt_seconds = atol(a + 10);
        else if (!strncmp(a, "--interval-us=", 14)) opt_interval_us = atol(a + 14);
        else if (!strncmp(a, "--threads=", 10)) opt_threads = atoi(a + 10);
        else if (!strncmp(a, "--stall-ms=", 11)) opt_stall_ms = atol(a + 11);
        else if (!strncmp(a, "--seed=", 7)) opt_seed = strtoul(a + 7, NULL, 10);
        else opt_handler = -1;
        if (opt_handler < 0) {
            static const char u[] = "usage: sigstorm --handler=none|malloc|free|realloc|calloc|large [--seconds=N] "
                                    "[--interval-us=N] [--threads=N] [--stall-ms=N] [--seed=N]\n";
            if (write(2, u, sizeof u - 1) < 0) {}
            return 2;
        }
    }
    if (opt_threads < 1 || opt_threads > 64 || opt_seconds < 1 || opt_stall_ms < 100) return 2;

    /* Per-thread state comes from mmap-free static storage: one page-aligned
     * array sized for 64 threads would be large, so take it from the
     * allocator under test once, before any signal is sent. */
    ws = aligned_alloc(64, sizeof(ws_t) * (size_t)opt_threads);
    if (!ws) return 2;
    memset(ws, 0, sizeof(ws_t) * (size_t)opt_threads);
    for (int i = 0; i < opt_threads; i++) {
        ws[i].idx = i;
        ws[i].rng = (opt_seed + 1) * 0x2545f4914f6cdd1dull + (uint64_t)i * 0x9e3779b97f4a7c15ull;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    /* Only the workload threads take SIGUSR1. */
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blk, &old);

    /* Workers, the signaller (when signals are sent) and this thread start
     * together. */
    int signals = opt_interval_us > 0;
    pthread_barrier_init(&g_start, NULL, (unsigned)opt_threads + 1u + (signals ? 1u : 0u));
    pthread_t sig_th;
    for (int i = 0; i < opt_threads; i++) {
        pthread_sigmask(SIG_SETMASK, &old, NULL);       /* workers inherit an open mask */
        if (pthread_create(&ws[i].th, NULL, worker, &ws[i]) != 0) return 2;
        pthread_sigmask(SIG_BLOCK, &blk, NULL);
    }
    if (signals && pthread_create(&sig_th, NULL, signaller, NULL) != 0) return 2;
    pthread_barrier_wait(&g_start);

    /* Watchdog. */
    uint64_t start = now_ns(), stall_ns = (uint64_t)opt_stall_ms * 1000000u;
    uint64_t *last = __builtin_alloca(sizeof(uint64_t) * (size_t)opt_threads);
    uint64_t *since = __builtin_alloca(sizeof(uint64_t) * (size_t)opt_threads);
    for (int i = 0; i < opt_threads; i++) { last[i] = 0; since[i] = start; }
    for (;;) {
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, NULL);
        uint64_t now = now_ns();
        int bad = atomic_load(&g_bad);
        if (bad) {
            int t = atomic_load(&g_bad_thread);
            report("corruption", now - start);
            out_ks("what", bad_names[bad]);
            out_kv("thread", (uint64_t)t);
            out_str(" addr="); out_hex(atomic_load(&g_bad_addr));
            out_flush();
            _exit(4);
        }
        for (int i = 0; i < opt_threads; i++) {
            uint64_t prog = atomic_load(&ws[i].steps) + atomic_load(&ws[i].h_done);
            if (prog != last[i]) { last[i] = prog; since[i] = now; continue; }
            if (now - since[i] >= stall_ns) {
                report("stall", now - start);
                out_kv("thread", (uint64_t)i);
                out_ks("op", op_names[atomic_load(&ws[i].op)]);
                out_kv("in_handler", (uint64_t)atomic_load(&ws[i].in_handler));
                out_ks("handler_op", op_names[atomic_load(&ws[i].hop)]);
                out_flush();
                _exit(3);
            }
        }
        if (now - start >= (uint64_t)opt_seconds * 1000000000u) {
            report("survived", now - start);
            out_flush();
            _exit(0);
        }
    }
}
