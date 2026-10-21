/*
 * CONCURRENCY repro: a signal handler that allocates while its thread holds
 * the page-heap lock (inside the allocator, BUSY_IN set) takes the shared
 * heap in trymode; bin_refill() -> bin_trim_avail() -> page_empty_release()
 * -> page_release() -> ph_free() then calls the blocking soham_lock(&g_ph.lock)
 * and waits for the lock its own interrupted thread holds: self-deadlock.
 *
 * Handler: alternates phases of N mallocs (kept) and N frees of a medium
 * size (12 KiB, 16 blocks per 192 KiB page, no spares for medium pages), so
 * the shared heap's bin gets several full pages that are notified and then
 * become completely free; the next refill trims them.
 * Main thread: keeps g_ph.lock held most of the time it is in the allocator
 * (mallinfo2 -> soham_get_stats -> walk_pages under g_ph.lock).
 *
 * Exit 0 "PASS" when it finishes; a watchdog prints "STALL" and exits 3 when
 * the main thread makes no progress for 5 s.
 */
#define _GNU_SOURCE 1
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
            if (++still >= 5) {
                char msg[128];
                int n = snprintf(msg, sizeof msg, "STALL: main thread stuck (progress %ld, handler calls %ld)\n", p, (long)hcalls);
                if (write(1, msg, (size_t)n) < 0) {}
                if (getenv("WD_ABORT")) abort();
                _exit(3);
            }
        } else still = 0;
        last = p;
    }
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    /* A big heap makes walk_pages (under g_ph.lock) long. */
    static void *keep[200000];
    for (int i = 0; i < 200000; i++) keep[i] = malloc(16 + (size_t)(i % 64) * 16);

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

    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        if (getenv("CHURN")) {
            /* ordinary churn: medium pages are allocated and released, so the
             * thread holds g_ph.lock in ph_alloc_slices/ph_free regularly */
            void *v[64];
            for (int k = 0; k < 64; k++) v[k] = malloc(20000);
            for (int k = 0; k < 64; k++) free(v[k]);
        } else {
            struct mallinfo2 mi = mallinfo2();
            (void)mi;
        }
        progress++;
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while (t.tv_sec - t0.tv_sec < secs);

    pthread_sigmask(SIG_BLOCK, &blk_set, NULL);
    stop = 1;
    pthread_join(sg, NULL);
    printf("%ld iterations, %ld handler calls\nPASS\n", progress, (long)hcalls);
    return 0;
}
