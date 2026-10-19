/*
 * regress/thread_exit_detach -- sohamalloc on Windows (mingw-w64, run under
 * wine): a pthread's heap is never detached when the thread exits, so heaps
 * leak (heaps_live only grows; nothing is pooled or reused) and the pages
 * they own are never orphaned for other threads to reuse.
 *
 * Cause: mingw-w64 gcc implements `__thread` (t_tls in src/sohamalloc.c) with
 * emutls, whose per-thread storage is freed by a winpthreads key destructor
 * when a pthread exits.  That runs BEFORE the FLS callback that sohamalloc
 * registers for detach (src/os_windows.c os_fls_callback -> api.c
 * thread_exit_cb).  By then &t_tls resolves to a fresh, zeroed emutls block
 * (allocated anew -- through malloc), so `t->heap != h` and thread_exit_cb
 * returns without detaching.  Threads made with CreateThread do not run the
 * winpthreads destructors first and do detach -- compare the two counts.
 *
 * Build/run (from the repo root):
 *   x86_64-w64-mingw32-gcc-posix -O2 -Iinclude tests/regress/thread_exit_detach.c \
 *       build-mingw/libsohamalloc-prefixed.a -lbcrypt -static -o /tmp/r.exe && wine /tmp/r.exe
 * (build-mingw: cmake -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix ...)
 * Expected: heaps_live back to its baseline after the threads are joined, and
 * the exited threads' heaps pooled.  Observed (commit 4ce8508, wine 9.0):
 * "pthreads: heaps_live 1 -> 25 after join (pooled 0)" (3 rounds of 8 threads:
 * every heap leaked) and exit 1, while the
 * CreateThread round returns to the baseline.  On Linux it passes.
 */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "sohamalloc.h"

enum { NT = 8 };

static void *pt_main(void *a)
{
    void *p = soham_malloc(100);
    soham_free(p);
    return a;
}

#ifdef _WIN32
static DWORD WINAPI wt_main(LPVOID a)
{
    void *p = soham_malloc(100);
    soham_free(p);
    (void)a;
    return 0;
}
#endif

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static struct soham_stats st(void)
{
    struct soham_stats s;
    soham_get_stats(&s);
    return s;
}

int main(void)
{
    void *p = soham_malloc(1);
    soham_free(p);
    int fail = 0;
    struct soham_stats a = st();
    for (int round = 0; round < 3; round++) {
        pthread_t t[NT];
        for (int i = 0; i < NT; i++) pthread_create(&t[i], NULL, pt_main, NULL);
        for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
    }
    sleep_ms(100);
    struct soham_stats b = st();
    printf("pthreads: heaps_live %zu -> %zu after join (pooled %zu)\n", a.heaps_live, b.heaps_live, b.heaps_pooled);
    if (b.heaps_live != a.heaps_live) fail = 1;
#ifdef _WIN32
    HANDLE h[NT];
    for (int i = 0; i < NT; i++) h[i] = CreateThread(NULL, 0, wt_main, NULL, 0, NULL);
    WaitForMultipleObjects(NT, h, TRUE, INFINITE);
    for (int i = 0; i < NT; i++) CloseHandle(h[i]);
    sleep_ms(100);
    struct soham_stats c = st();
    printf("CreateThread: heaps_live %zu -> %zu after join (pooled %zu)\n", b.heaps_live, c.heaps_live, c.heaps_pooled);
    if (c.heaps_live != b.heaps_live) fail = 1;
#endif
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
