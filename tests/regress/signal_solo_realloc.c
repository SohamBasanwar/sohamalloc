/*
 * regress/signal_solo_realloc -- sohamalloc: a signal handler that allocates
 * or frees a large block (a solo, >= 32 MiB) while its thread is inside
 * realloc() or malloc_usable_size() of a solo deadlocks.
 *
 * Those paths took the solo-table lock without setting tls.busy, so the
 * handler took the normal path and waited for the lock its own thread held
 * (mremap ran under that lock, which made the window wide).  Found by the
 * concurrency and correctness reviews (2026-10-20).
 *
 * Expected: "PASS" within a few seconds.  Before the fix: hangs at once.
 * Status: fixed (solo_usable_busy(), solo_realloc() under busy_enter, mremap
 * outside the lock).
 */
#define _GNU_SOURCE 1
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static volatile sig_atomic_t hits;

static void handler(int sig)
{
    (void)sig;
    void *p = malloc((size_t)33 << 20);
    free(p);
    hits++;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = { { 0, 50 }, { 0, 50 } };
    setitimer(ITIMER_REAL, &it, NULL);

    void *p = malloc((size_t)40 << 20);
    for (long i = 0; i < 20000; i++) {
        void *q = realloc(p, (i & 1) ? ((size_t)80 << 20) : ((size_t)40 << 20));
        if (q) p = q;
        if (malloc_usable_size(p) == 0) { printf("FAIL: usable size 0\n"); return 1; }
    }
    struct itimerval z;
    memset(&z, 0, sizeof z);
    setitimer(ITIMER_REAL, &z, NULL);
    free(p);
    printf("%ld handler calls\nPASS\n", (long)hits);
    return 0;
}
