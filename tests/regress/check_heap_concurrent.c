/*
 * regress/check_heap_concurrent -- sohamalloc: soham_check_heap() reports
 * errors while other threads allocate, although the heap is consistent.
 *
 * soham_check_heap() (src/api.c) holds only the page-heap lock and, for every
 * used slice, reads the slice record (kind, cap, bsize, psize) and counts an
 * error for kind == KIND_NONE ("E4").  But the records are written outside
 * that lock: page_init() and run_alloc() (src/heap.c) fill in kind/cap/...
 * after ph_alloc_*() has marked the slices used and dropped the lock, and
 * page_release() resets kind to KIND_NONE before ph_free() takes it.  A
 * concurrent check therefore sees "used slice, kind NONE" and returns a
 * non-zero error count; the plain (non-atomic) record fields are also a data
 * race between the checker and the allocating thread (TSan reports it).  The
 * function is documented as checking "the invariants that can be checked
 * without stopping other threads", so it must return 0 here.  The same
 * unlocked record reads happen in soham_get_stats() -> walk_pages().
 *
 * Build/run:  gcc -O2 -pthread -Iinclude tests/regress/check_heap_concurrent.c \
 *                 build/libsohamalloc-prefixed.a -o /tmp/r && /tmp/r 4
 * Expected: "checks with errors: 0".  Observed (commit 9c944b4): with 1 thread
 * 0; with 4 threads 70-85 of 100 checks report errors (e.g. "= 10"); the final
 * check after all threads joined is 0.
 *
 * Status: fixed in ae792ec (records are validated only for the calling
 * thread's own pages).  Kept as a regression test: run_tests.sh --prefixed
 * runs it as regress_check_heap.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "sohamalloc.h"

enum { SLOTS = 4096, OPS = 400000 };

static void *run(void *arg)
{
    unsigned long x = (unsigned long)arg * 7919 + 1;
    static __thread void *sl[SLOTS];
    long errs = 0;
    for (int op = 0; op < OPS; op++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        int i = (int)(x % SLOTS);
        if (sl[i]) { soham_free(sl[i]); sl[i] = NULL; }
        else sl[i] = soham_malloc((x >> 20) % ((x & 1) ? 200 : 300000));
        if ((op & 0x3fff) == 0) {
            int e = soham_check_heap();
            if (e) { errs++; if (errs == 1) printf("soham_check_heap() = %d during allocation\n", e); }
        }
    }
    for (int i = 0; i < SLOTS; i++) soham_free(sl[i]);
    return (void *)errs;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 4;
    if (n < 1 || n > 64) n = 4;
    pthread_t t[64];
    for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, run, (void *)(long)i);
    long tot = 0;
    for (int i = 0; i < n; i++) { void *r; pthread_join(t[i], &r); tot += (long)r; }
    int fin = soham_check_heap();
    printf("threads %d, checks with errors: %ld of %d, final check %d\n", n, tot, n * ((OPS + 0x3fff) / 0x4000), fin);
    if (tot || fin) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
