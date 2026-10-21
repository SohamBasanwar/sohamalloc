/*
 * regress/signal_shared_release -- sohamalloc: memory that signal handlers
 * allocated from the shared heap must go back to the page heap once it is
 * freed, both on its own (as the program keeps allocating) and on
 * malloc_trim/soham_collect.
 *
 * Handler-context allocations come from the shared heap, which no thread
 * owns: no thread exit ever flushes it, its refills run in trymode where
 * trimming is skipped, and soham_collect flushed only the calling thread's
 * heap.  Once freed, its pages stayed cached in the shared heap for the life
 * of the process (64 MiB stayed resident after malloc_trim(0) in the
 * reviewer's reproducer).  Found by review round 3 (2026-10-21;
 * R3-CORRECTNESS-2).  Fixed: the shared heap is flushed by soham_collect and,
 * after it was used, from a thread's periodic refill tick.
 *
 * A handler that interrupted the allocator (soham_get_stats returns -1 there)
 * allocates 16 MiB in 2 KiB blocks; the main thread frees them, then
 *  1. allocates and frees 100k small blocks (ordinary refills), and
 *  2. calls soham_collect(1).
 * After each step the bytes cached in heaps must be below 4 MiB.
 * Expected: "PASS", exit 0; SKIP (77) when the allocator is not sohamalloc.
 */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sohamalloc.h"

#define BS 2048
#define NBLK ((16u << 20) / BS)
#define LIMIT ((size_t)4 << 20)

static int (*get_stats)(struct soham_stats *);
static void (*collect)(int);
static void *blk[NBLK];
static volatile int done;
static pthread_t main_thr;

static void handler(int sig)
{
    (void)sig;
    if (done) return;
    struct soham_stats st;
    if (get_stats(&st) != -1) return;          /* only when it interrupted the allocator */
    for (unsigned i = 0; i < NBLK; i++) {
        blk[i] = malloc(BS);
        if (blk[i]) memset(blk[i], 1, BS);
    }
    done = 1;
}

static void *signaller(void *arg)
{
    (void)arg;
    while (!done) {
        pthread_kill(main_thr, SIGUSR1);
        struct timespec ts = { 0, 20000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static size_t cached(void)
{
    struct soham_stats st;
    memset(&st, 0, sizeof st);
    get_stats(&st);
    return st.cached_heaps;
}

int main(void)
{
    get_stats = (int (*)(struct soham_stats *))dlsym(RTLD_DEFAULT, "soham_get_stats");
    collect = (void (*)(int))dlsym(RTLD_DEFAULT, "soham_collect");
    if (!get_stats || !collect) { printf("SKIP: not sohamalloc\n"); return 77; }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    main_thr = pthread_self();
    pthread_t t;
    pthread_create(&t, NULL, signaller, NULL);
    while (!done) free(malloc(32));
    pthread_join(t, NULL);

    unsigned got = 0;
    for (unsigned i = 0; i < NBLK; i++) got += blk[i] != NULL;
    size_t c0 = cached();
    for (unsigned i = 0; i < NBLK; i++) free(blk[i]);
    size_t c1 = cached();

    /* 1. Ordinary activity: enough small-block refills for the periodic tick. */
    static void *v[100000];
    for (int i = 0; i < 100000; i++) v[i] = malloc(64);
    for (int i = 0; i < 100000; i++) free(v[i]);
    size_t c2 = cached();
    /* 2. Explicit collection. */
    collect(1);
    size_t c3 = cached();

    printf("handler allocated %u of %u blocks; cached in heaps (KiB): allocated %zu, freed %zu, "
           "after activity %zu, after soham_collect(1) %zu\n",
           got, NBLK, c0 >> 10, c1 >> 10, c2 >> 10, c3 >> 10);
    if (got != NBLK) { printf("FAIL: handler allocations failed\n"); return 1; }
    if (c2 >= LIMIT || c3 >= LIMIT) { printf("FAIL: freed handler memory stays cached\n"); return 1; }
    printf("PASS\n");
    return 0;
}
