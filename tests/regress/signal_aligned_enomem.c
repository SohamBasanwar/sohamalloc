/*
 * regress/signal_aligned_enomem -- sohamalloc: an aligned allocation
 * (posix_memalign, aligned_alloc) from a signal handler that interrupted the
 * allocator must not fail with ENOMEM once the static arena is used up.
 *
 * alloc_aligned() sent every call made in handler context straight to the
 * 384 KiB static arena, whose frees are no-ops, instead of trying the shared
 * heap under its trylock as alloc_slow() does for malloc (the bug class of
 * regress/signal_reentrant_enomem).  After ~384 KiB of handler-context
 * aligned allocations every later one failed, forever.  Found by review
 * round 3 (2026-10-21; R3-CONCURRENCY-2).  Fixed: aligned requests in handler
 * context use the shared heap like malloc, and fall back to the static arena
 * (aligned) only when its trylock fails.
 *
 * One allocating thread (so the shared-heap trylock cannot fail) and a
 * signaller that never allocates; the handler alternates posix_memalign(64)
 * and aligned_alloc(256) and checks alignment.  Expected: "PASS", exit 0.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t h_calls, h_null, h_misaligned;

static void handler(int sig)
{
    (void)sig;
    int e = errno;
    h_calls++;
    size_t n = 256 + (size_t)(h_calls % 2000);
    void *p = NULL;
    size_t align;
    if (h_calls & 1) {
        align = 64;
        if (posix_memalign(&p, align, n) != 0) p = NULL;
    } else {
        align = 256;
        p = aligned_alloc(align, (n + align - 1) & ~(align - 1));
    }
    if (!p) h_null++;
    else {
        if ((uintptr_t)p & (align - 1)) h_misaligned++;
        memset(p, 1, 64);
        free(p);
    }
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

    printf("%ld handler calls, %ld NULL, %ld misaligned\n", (long)h_calls, (long)h_null, (long)h_misaligned);
    if (h_null || h_misaligned) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
