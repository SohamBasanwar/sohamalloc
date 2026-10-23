/*
 * regress/signal_large_fallback -- sohamalloc: a large allocation (300 KB)
 * from a signal handler that interrupted the allocator must not fail with
 * ENOMEM merely because its own thread holds the page-heap lock.
 *
 * Handler-context allocations come from the shared heap under trylock; a
 * run of 300 KB needs the page-heap lock, which the interrupted thread may
 * hold.  The fallback was the static arena (384 KiB, never reused): the
 * first large fallback used most of it, and every later one returned NULL
 * (sigstorm campaign 1: 2-3% of the large handler calls that interrupted
 * the allocator).  Fixed (2026-10-22): requests above 16 KiB skip the arena
 * and take a mapping of their own (a solo, its table taken by trylock).
 *
 * One thread allocates and frees 200-400 KB blocks (runs: the page-heap
 * lock) while a signaller that never allocates interrupts it; the handler
 * mallocs 300 KB, writes its ends and frees it.
 * Expected: "PASS" (0 NULL), exit 0.  On the unfixed code: tens of NULL.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t h_calls, h_null;

static void handler(int sig)
{
    (void)sig;
    int e = errno;
    h_calls++;
    char *p = malloc(300000);
    if (!p) h_null++;
    else { p[0] = 1; p[299999] = 2; free(p); }
    errno = e;
}

static volatile int stop;
static pthread_t main_thr;

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
    sigaction(SIGUSR1, &sa, NULL);
    main_thr = pthread_self();
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blk, &old);
    pthread_t sg;
    pthread_create(&sg, NULL, signaller, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    void *ring[8] = { 0 };
    unsigned long i = 0;
    do {
        /* Runs come from the page heap: the thread holds its lock often. */
        unsigned k = (unsigned)(i++ % 8);
        free(ring[k]);
        ring[k] = malloc(200000 + (i * 7919) % 200000);
        if (!ring[k]) { fprintf(stderr, "main malloc failed\n"); return 2; }
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec) < 3000000000L);
    pthread_sigmask(SIG_BLOCK, &blk, NULL);
    stop = 1;
    pthread_join(sg, NULL);
    for (int k = 0; k < 8; k++) free(ring[k]);

    printf("%ld handler calls, %ld NULL\n", (long)h_calls, (long)h_null);
    if (h_null) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
