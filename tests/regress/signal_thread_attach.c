/*
 * regress/signal_thread_attach -- sohamalloc: a signal handler that mallocs
 * while its thread is attaching a heap (its first allocation) deadlocks.
 *
 * heap_attach() and soham_init() took g_heap_lock without setting tls.busy,
 * so a handler that interrupted them took the normal allocation path,
 * found no heap, called heap_attach() itself and waited forever for the
 * lock its own thread held.  Seen as an intermittent hang of
 * regress_signal_enomem on musl (about 1 run in 10).  The same held for
 * soham_get_stats(), soham_check_heap() and the release done by
 * soham_collect().
 *
 * This program starts short-lived threads one after another under a
 * 20 us SIGPROF timer; every thread's first malloc attaches a heap, and
 * the handler mallocs and frees.  The main thread also calls
 * soham_get_stats/soham_check_heap/soham_collect (found with dlsym) in a
 * loop.  Expected: "PASS" well within the timeout.
 *
 * Status: fixed (thread_prepare() and busy_enter() in src/api.c).
 */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static volatile sig_atomic_t h_calls;

static void handler(int sig)
{
    (void)sig;
    h_calls++;
    void *p = malloc(32 + (size_t)(h_calls % 500));
    if (p) { memset(p, 1, 16); free(p); }
}

static void *worker(void *arg)
{
    (void)arg;
    void *v[16];
    for (int i = 0; i < 16; i++) v[i] = malloc(16 + 40 * i);
    for (int i = 0; i < 16; i++) free(v[i]);
    return NULL;
}

struct stats_buf { size_t pad[64]; };

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);

    int (*get_stats)(void *) = (int (*)(void *))dlsym(RTLD_DEFAULT, "soham_get_stats");
    int (*check_heap)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "soham_check_heap");
    void (*collect)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "soham_collect");

    struct itimerval it = { { 0, 20 }, { 0, 20 } };
    setitimer(ITIMER_PROF, &it, NULL);

    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long threads = 0;
    do {
        pthread_t th[4];
        int n = 0;
        for (int i = 0; i < 4; i++) if (pthread_create(&th[n], NULL, worker, NULL) == 0) n++;
        for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
        threads += n;
        struct stats_buf sb;
        if (get_stats) get_stats(&sb);
        if (check_heap && check_heap() != 0) { printf("FAIL: soham_check_heap reported errors\n"); return 1; }
        if (collect && threads % 64 == 0) collect(0);
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec) < 3000000000L);

    struct itimerval z;
    memset(&z, 0, sizeof z);
    setitimer(ITIMER_PROF, &z, NULL);
    printf("%ld threads, %ld handler calls\nPASS\n", threads, (long)h_calls);
    return 0;
}
