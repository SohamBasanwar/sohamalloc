/*
 * regress/fork_notify_reuse -- sohamalloc: after fork(), a thread that takes
 * over a pooled heap must reuse the pages other threads freed into it.
 *
 * In the fork child, the heap of a parent thread that does not exist in the
 * child goes to the heap pool as it is, with its pages on its full list.
 * When the child frees those blocks, the frees (remote: the owner tag is
 * the pooled heap's) notify the heap's notify stack.  heap_attach() then
 * reset the stack to NULL unconditionally (meant to reopen a stack closed
 * by heap_flush), dropping the notifications: the full pages were never
 * moved back to the avail list and the new thread allocated fresh memory
 * instead.  Measured before the fix: 540 of 6000 blocks reused (the cur
 * page's remainder only).  Found by the concurrency review of 2026-10-21.
 * Fixed: heap_attach reopens only a closed stack (CAS NOTIFY_CLOSED -> NULL).
 *
 * Thread B allocates NB blocks and waits; the process forks; in the child
 * the main thread frees B's blocks, then a new thread C allocates NB blocks
 * of the same size.  At least half of C's blocks must reuse B's addresses.
 * Expected: "PASS", exit 0.
 */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define NB 6000
#define SZ 48

static void *bblk[NB], *cblk[NB];
static uintptr_t baddr[NB];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int b_ready, b_quit;

static void *thread_b(void *arg)
{
    (void)arg;
    for (int i = 0; i < NB; i++) bblk[i] = malloc(SZ);
    pthread_mutex_lock(&mu);
    b_ready = 1;
    pthread_cond_broadcast(&cv);
    while (!b_quit) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void *thread_c(void *arg)
{
    (void)arg;
    for (int i = 0; i < NB; i++) cblk[i] = malloc(SZ);
    return NULL;
}

static int cmp(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

int main(void)
{
    pthread_t b;
    pthread_create(&b, NULL, thread_b, NULL);
    pthread_mutex_lock(&mu);
    while (!b_ready) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);

    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < NB; i++) baddr[i] = (uintptr_t)bblk[i];
        qsort(baddr, NB, sizeof baddr[0], cmp);
        for (int i = 0; i < NB; i++) free(bblk[i]);
        pthread_t c;
        pthread_create(&c, NULL, thread_c, NULL);
        pthread_join(c, NULL);
        int reused = 0;
        for (int i = 0; i < NB; i++) {
            uintptr_t a = (uintptr_t)cblk[i];
            if (bsearch(&a, baddr, NB, sizeof baddr[0], cmp)) reused++;
        }
        printf("child: %d of %d new blocks reuse freed blocks of the pooled heap\n", reused, NB);
        fflush(stdout);
        _exit(reused >= NB / 2 ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    pthread_mutex_lock(&mu);
    b_quit = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    pthread_join(b, NULL);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
