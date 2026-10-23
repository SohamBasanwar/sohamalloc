/*
 * regress/fork_during_init -- sohamalloc: fork() while another thread is
 * initialising the allocator must leave a child that can allocate.
 *
 * Init set its state to "running" at the start and registered the fork
 * handlers at its end, so a child forked in between inherited "running" with
 * no thread left to finish init, and its first allocation waited forever.
 * Found by review round 4 (2026-10-23).  Fixed: the fork handlers are
 * registered at load and fork_prepare waits for an init in progress; a child
 * that still inherits an unfinished init (a fork at its very start) takes it
 * over, since its process did not start it.
 *
 * Prefixed build (soham_* only, so nothing initialises the allocator early).
 * Each trial is a fresh process: one thread makes the first allocation (init)
 * while the trial's main thread forks children that allocate, each under a
 * 3 s alarm.  Expected: "PASS", exit 0.
 */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sohamalloc.h"

#define TRIALS 40

static _Atomic int init_done;

static void *initialiser(void *arg)
{
    (void)arg;
    void *p = soham_malloc(64);       /* the process's first allocation: init */
    soham_free(p);
    atomic_store(&init_done, 1);
    return NULL;
}

/* One trial, in a fresh process: 0 if every child could allocate. */
static int trial(void)
{
    alarm(20);
    pthread_t t;
    if (pthread_create(&t, NULL, initialiser, NULL) != 0) return 3;
    int stuck = 0;
    for (int k = 0; k < 64 && !stuck; k++) {
        pid_t c = fork();
        if (c == 0) {
            alarm(3);                 /* default action ends a child that hangs */
            void *p = soham_malloc(32);
            _exit(p ? 0 : 2);
        }
        if (c < 0) break;
        int st = 0;
        while (waitpid(c, &st, 0) < 0) {}
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM) stuck = 1;
        if (atomic_load(&init_done) && k > 8) break;
    }
    pthread_join(t, NULL);
    return stuck;
}

int main(void)
{
    int failed = 0;
    for (int i = 0; i < TRIALS; i++) {
        pid_t p = fork();
        if (p == 0) _exit(trial());
        if (p < 0) { printf("FAIL: fork\n"); return 1; }
        int st = 0;
        while (waitpid(p, &st, 0) < 0) {}
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) failed++;
    }
    printf("%d trials: %d had a child whose first allocation hung\n", TRIALS, failed);
    if (failed) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
