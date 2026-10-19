/*
 * regress/signal_reentrant_enomem -- sohamalloc: a malloc() from a signal
 * handler that interrupted the allocator returns NULL/ENOMEM forever once
 * the .bss emergency arena is used up.
 *
 * Spec docs/IMPL_SPEC_v0.md §5.3 "Signal reentrancy": "On entry, if
 * heap->busy, then malloc -> the shared heap via trylock.  If that fails,
 * use the 256 KiB .bss emergency bump arena, whose frees are counted
 * no-ops."  src/api.c alloc_slow() skips the shared-heap trylock and goes
 * straight to bss_alloc() whenever tls.busy is set.  bss frees are no-ops,
 * so after ~384 KiB of reentrant allocations every later reentrant malloc
 * fails, although here (one allocating thread) the shared-heap trylock
 * could never fail.
 *
 * Build/run:  gcc -O2 -pthread tests/regress/signal_reentrant_enomem.c -o /tmp/r
 *             LD_PRELOAD=build/libsohamalloc.so /tmp/r
 * Expected: "0 NULL" and exit 0.  Observed (v0.1, commit 4e611d0): thousands
 * of NULL results (e.g. "7847 of 26190 handler mallocs returned NULL") and
 * exit 1.  Only meaningful for sohamalloc: glibc, jemalloc and tcmalloc
 * deadlock in this program (malloc is not async-signal-safe for them).
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t h_calls, h_null, h_enomem_other;

static void handler(int sig)
{
    (void)sig;
    int e = errno;
    h_calls++;
    void *p = malloc(256 + (size_t)(h_calls % 2000));
    if (!p) { h_null++; if (errno != ENOMEM) h_enomem_other++; }
    else { memset(p, 1, 64); free(p); }
    errno = e;
}

static volatile int stop;
static pthread_t main_thr;

/* Sends SIGUSR1 to the main thread as fast as it can (never allocates). */
static void *signaller(void *arg)
{
    (void)arg;
    while (!stop) {
        pthread_kill(main_thr, SIGUSR1);
        struct timespec ts = { 0, 20000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    main_thr = pthread_self();
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigaddset(&blk, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &blk, &old);
    pthread_t sg;
    pthread_create(&sg, NULL, signaller, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    struct itimerval it = { { 0, 50 }, { 0, 50 } };
    setitimer(ITIMER_PROF, &it, NULL);

    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    void *ring[256] = { 0 };
    unsigned long i = 0;
    do {
        /* Keep the thread inside malloc/free most of the time. */
        unsigned k = (unsigned)(i++ % 256);
        free(ring[k]);
        ring[k] = malloc(16 + (i * 7919) % 3000);
        if (!ring[k]) { fprintf(stderr, "main malloc failed\n"); return 2; }
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec) < 3000000000L);
    struct itimerval z;
    memset(&z, 0, sizeof z);
    setitimer(ITIMER_PROF, &z, NULL);
    pthread_sigmask(SIG_BLOCK, &blk, NULL);
    stop = 1;
    pthread_join(sg, NULL);
    for (int k = 0; k < 256; k++) free(ring[k]);

    printf("%ld handler calls, %ld NULL (%ld without ENOMEM)\n", (long)h_calls, (long)h_null, (long)h_enomem_other);
    if (h_null) {
        printf("FAIL: %ld of %ld handler mallocs returned NULL\n", (long)h_null, (long)h_calls);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
