/*
 * regress/signal_trim_deadlock -- sohamalloc: a signal handler that
 * allocates while its thread holds the page-heap lock must not deadlock.
 *
 * Handler mallocs that interrupt the allocator (tls.busy set) use the shared
 * heap in trymode, where every lock is a trylock.  bin_refill() called
 * bin_trim_avail() -> page_empty_release() -> page_release() -> ph_free(),
 * which takes g_ph.lock with the *blocking* soham_lock(): when the
 * interrupted thread held g_ph.lock (in ph_alloc_slices / ph_free, or in
 * soham_get_stats via mallinfo2), the handler waited for its own thread
 * forever.  Found by the concurrency review of 2026-10-21 (3 of 3 runs
 * stalled).  Fixed: refills in trymode never trim (and SOHAM_DEBUG builds
 * abort if a blocking lock is taken in trymode).
 *
 * The handler alternates phases of 256 mallocs and 256 frees of 12000 bytes
 * (16 blocks per medium page, no spares), so the shared heap's bin gets
 * full pages that become completely free and are trimmed by a later refill.
 * The main thread alternates ordinary churn of 20000-byte blocks (page-heap
 * lock in ph_alloc_slices / ph_free) and soham_get_stats() (page-heap lock
 * in the statistics walk; found with dlsym, so the test also builds on musl,
 * which has no mallinfo2).  A watchdog reports STALL (exit 3) when the main thread
 * makes no progress for 4 s.  Expected: "PASS", exit 0.
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

#define N 256
#define SZ 12000

static void *blk[N];
static int pos, phase;
static volatile sig_atomic_t hcalls;
static volatile long progress;
static volatile int stop;
static pthread_t main_thr;

static void handler(int sig)
{
    (void)sig;
    hcalls++;
    if (phase == 0) {
        blk[pos] = malloc(SZ);
        if (blk[pos]) memset(blk[pos], 0x5a, 64);
        if (++pos == N) { pos = 0; phase = 1; }
    } else {
        free(blk[pos]);
        blk[pos] = NULL;
        if (++pos == N) { pos = 0; phase = 0; }
    }
}

static void *signaller(void *arg)
{
    (void)arg;
    while (!stop) {
        pthread_kill(main_thr, SIGUSR1);
        struct timespec ts = { 0, 2000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void *watchdog(void *arg)
{
    (void)arg;
    long last = -1;
    int still = 0;
    for (;;) {
        sleep(1);
        if (stop) return NULL;
        long p = progress;
        if (p == last) {
            if (++still >= 4) {
                char msg[160];
                int n = snprintf(msg, sizeof msg,
                                 "STALL: main thread made no progress for 4 s (%ld iterations, %ld handler calls)\n",
                                 p, (long)hcalls);
                if (write(1, msg, (size_t)n) < 0) {}
                _exit(3);
            }
        } else {
            still = 0;
        }
        last = p;
    }
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 8;
    /* A big heap makes the statistics walk (under g_ph.lock) long. */
    static void *keep[100000];
    for (int i = 0; i < 100000; i++) keep[i] = malloc(16 + (size_t)(i % 64) * 16);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    main_thr = pthread_self();

    sigset_t blk_set, old;
    sigemptyset(&blk_set);
    sigaddset(&blk_set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blk_set, &old);
    pthread_t sg, wd;
    pthread_create(&sg, NULL, signaller, NULL);
    pthread_create(&wd, NULL, watchdog, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    int (*get_stats)(struct soham_stats *) =
        (int (*)(struct soham_stats *))dlsym(RTLD_DEFAULT, "soham_get_stats");
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        if (progress & 1) {
            void *v[64];
            for (int k = 0; k < 64; k++) v[k] = malloc(20000);
            for (int k = 0; k < 64; k++) free(v[k]);
        } else if (get_stats) {
            struct soham_stats st;
            get_stats(&st);
        }
        progress++;
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while (t.tv_sec - t0.tv_sec < secs);

    pthread_sigmask(SIG_BLOCK, &blk_set, NULL);
    stop = 1;
    pthread_join(sg, NULL);
    for (int i = 0; i < 100000; i++) free(keep[i]);
    printf("%ld iterations, %ld handler calls\nPASS\n", progress, (long)hcalls);
    return 0;
}
