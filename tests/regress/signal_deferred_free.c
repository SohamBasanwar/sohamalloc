/*
 * regress/signal_deferred_free -- sohamalloc: runs freed by a signal handler
 * that interrupted the allocator must go back to the page heap, also when the
 * interrupted thread only ever takes the fast paths, and also when the
 * handler runs during the thread's exit.
 *
 * In handler context every free of a run or solo was put on the thread's
 * deferred list, which only the allocation slow paths, soham_collect and the
 * start of thread exit drained.  A thread that stayed in the fast paths kept
 * them indefinitely, and frees made during the rest of thread exit were never
 * drained.  Found by review round 4 (2026-10-23).  Fixed: in handler context a
 * run is released at once when the page-heap lock is free (trylock), a solo
 * when the solo table is; anything put off is drained when the interrupted
 * call finishes and at the end of thread exit.
 *
 * Part 1: the main thread allocates 200 runs of 300 KB, then runs a malloc/free
 * loop of one small size (fast paths only) while a signaller interrupts it;
 * a handler that finds it interrupted the allocator frees one run.  Afterwards
 * the bytes in use must have dropped by the freed runs.
 * Part 2: 48 threads each hold a run and exit after building up pages; a
 * handler that interrupts a thread's exit frees that thread's run.  After all
 * threads are joined, no handler-freed run may remain in use.
 * Expected: "PASS", exit 0; SKIP (77) when the allocator is not sohamalloc.
 */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sohamalloc.h"

#define RUN ((size_t)300000)
#define NRUNS 200
#define NTHREADS 48

static int (*get_stats)(struct soham_stats *);
static void *runs[NRUNS];
static _Atomic int next_run, freed1;
static _Atomic int stop;

/* soham_get_stats returns -1 exactly when it is called from a handler that
 * interrupted the allocator on this thread. */
static int interrupted(void)
{
    struct soham_stats st;
    return get_stats(&st) == -1;
}

static size_t in_use(void)
{
    struct soham_stats st;
    memset(&st, 0, sizeof st);
    get_stats(&st);
    return st.in_use;
}

/* ---- part 1 */
static void handler1(int sig)
{
    (void)sig;
    if (!interrupted()) return;
    int k = atomic_fetch_add(&next_run, 1);
    if (k >= NRUNS) return;
    free(runs[k]);
    atomic_fetch_add(&freed1, 1);
}

static pthread_t target;

static void *signaller(void *arg)
{
    (void)arg;
    while (!atomic_load(&stop)) {
        pthread_kill(target, SIGUSR1);
        struct timespec ts = { 0, 20000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- part 2: each thread's run sits in a slot that the handler or, after
 * the join, the main thread empties (whoever takes it frees it). */
static void *_Atomic slot[NTHREADS];
static __thread void *_Atomic *my_slot;
static __thread volatile int exiting;
static _Atomic int freed2;

static void handler2(int sig)
{
    (void)sig;
    if (!exiting || !my_slot || !interrupted()) return;
    void *p = atomic_exchange(my_slot, NULL);
    if (!p) return;
    free(p);
    atomic_fetch_add(&freed2, 1);
}

static void *exiter(void *arg)
{
    my_slot = &slot[(long)arg];
    atomic_store(my_slot, malloc(RUN));
    /* Pages in many classes, so the heap flush at exit takes a while. */
    static const size_t sizes[] = { 16, 48, 112, 400, 1500, 3000, 9000, 20000 };
    void *blocks[4000];
    for (int i = 0; i < 4000; i++) blocks[i] = malloc(sizes[i % 8]);
    for (int i = 0; i < 4000; i++) if (i % 4) free(blocks[i]);
    for (int i = 0; i < 4000; i += 4) free(blocks[i]);
    exiting = 1;
    return NULL;     /* thread exit: a handler may free the run in there */
}

int main(void)
{
    get_stats = (int (*)(struct soham_stats *))dlsym(RTLD_DEFAULT, "soham_get_stats");
    if (!get_stats) { printf("SKIP: not sohamalloc\n"); return 77; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    /* Part 1 */
    sa.sa_handler = handler1;
    sigaction(SIGUSR1, &sa, NULL);
    for (int i = 0; i < NRUNS; i++) {
        runs[i] = malloc(RUN);
        if (!runs[i]) { printf("FAIL: malloc of a run\n"); return 1; }
        ((char *)runs[i])[0] = 1;
    }
    size_t before = in_use();
    target = pthread_self();
    pthread_t sg;
    pthread_create(&sg, NULL, signaller, NULL);
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        for (int i = 0; i < 1000; i++) { void *p = malloc(32); if (p) *(volatile char *)p = 1; free(p); }
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while (atomic_load(&next_run) < NRUNS &&
             (t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec) < 3000000000L);
    atomic_store(&stop, 1);
    pthread_join(sg, NULL);
    size_t after = in_use();
    int f1 = atomic_load(&freed1);
    size_t expect_drop = (size_t)f1 * RUN;
    size_t drop = before > after ? before - after : 0;
    printf("part 1: %d runs freed by interrupting handlers; in use dropped by %zu KiB (expected about %zu KiB)\n",
           f1, drop >> 10, expect_drop >> 10);
    int bad = 0;
    if (f1 < NRUNS / 4) { printf("part 1: too few interrupting handler calls to judge\n"); }
    else if (drop + 4 * RUN < expect_drop) { printf("part 1: runs freed in handlers were kept\n"); bad = 1; }
    /* No handler may free a run while the leftovers are freed here. */
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blk, NULL);
    for (int i = atomic_load(&next_run); i < NRUNS; i++) free(runs[i]);

    /* Part 2 */
    sa.sa_handler = handler2;
    sigaction(SIGUSR1, &sa, NULL);
    pthread_sigmask(SIG_UNBLOCK, &blk, NULL);
    /* A clean baseline: process anything still deferred. */
    void (*collect)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "soham_collect");
    if (collect) collect(0);
    size_t base = in_use();
    size_t kept = 0;
    for (int i = 0; i < NTHREADS; i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, exiter, (void *)(long)i) != 0) { printf("FAIL: pthread_create\n"); return 1; }
        /* Interrupt it all the way through its exit. */
        for (int k = 0; k < 5000 && pthread_kill(th, SIGUSR1) == 0; k++) {
            struct timespec ts = { 0, 10000 };
            nanosleep(&ts, NULL);
        }
        pthread_join(th, NULL);
        void *left = atomic_exchange(&slot[i], NULL);
        if (left) { free(left); kept++; }
    }
    size_t end = in_use();
    int f2 = atomic_load(&freed2);
    size_t leaked = end > base ? end - base : 0;
    printf("part 2: %d of %d runs freed by handlers during thread exit; in use grew by %zu KiB\n",
           f2, NTHREADS, leaked >> 10);
    if (f2 == 0) printf("part 2: no handler call landed in a thread exit\n");
    else if (leaked >= 2 * RUN) { printf("part 2: runs freed in handlers during thread exit stayed in use\n"); bad = 1; }

    if (bad) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
