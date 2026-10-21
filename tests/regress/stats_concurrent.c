/*
 * regress/stats_concurrent -- soham_get_stats() must not race with the
 * page heap.  It read g_ph.s_used and g_ph.s_dirty without g_ph.lock while
 * other threads changed them under the lock: a data race (undefined
 * behaviour) that ThreadSanitizer reported 21 times in a stress run (review
 * of 2026-10-21).  Fixed: the counters are read under g_ph.lock.
 *
 * Built against the prefixed library with -fsanitize=thread by
 * run_tests.sh --sanitize=thread, where any race report fails the test; in
 * other builds it is a functional check of concurrent statistics.
 * Expected: "PASS", exit 0.
 */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sohamalloc.h"

#ifndef TEST_PREFIXED
#define soham_malloc malloc
#define soham_free free
#endif

static atomic_int stop;

static void *worker(void *arg)
{
    unsigned r = (unsigned)(uintptr_t)arg * 2654435761u + 1;
    void *v[256] = { 0 };
    while (!atomic_load(&stop)) {
        for (int i = 0; i < 256; i++) {
            r = r * 1103515245u + 12345u;
            soham_free(v[i]);
            size_t n = (r >> 8) % 4 == 0 ? 20000 + (r >> 12) % 200000 : 16 + (r >> 12) % 2000;
            v[i] = soham_malloc(n);
            if (v[i]) memset(v[i], 1, 16);
        }
    }
    for (int i = 0; i < 256; i++) soham_free(v[i]);
    return NULL;
}

int main(void)
{
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)(i + 1));
    struct soham_stats s;
    size_t calls = 0;
    for (int k = 0; k < 2000; k++) {
        if (soham_get_stats(&s) == 0) calls++;
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    if (calls == 0) { printf("FAIL: soham_get_stats never succeeded\n"); return 1; }
    printf("%zu concurrent statistics snapshots\nPASS\n", calls);
    return 0;
}
