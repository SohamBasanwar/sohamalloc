/*
 * regress/fork_concurrent_busy -- sohamalloc: fork() called by two threads
 * at the same time must not mix up the threads' busy bytes.
 *
 * fork_prepare saved the forking thread's tls.busy in one process-wide
 * variable, and fork_parent/fork_child restored it from there.  glibc runs
 * the atfork handlers of concurrently forking threads at the same time
 * (measured on glibc 2.43: two threads inside prepare at once), so a thread
 * that had never allocated (busy = BUSY_NOHEAP, no heap) could get the busy
 * byte of a thread that had one (0).  Its next malloc then took the fast
 * path with a NULL heap: SIGSEGV in the parent or in the child.  Found by
 * review round 3 (2026-10-21; R3-CONCURRENCY-1 = R3-CORRECTNESS-1).  Fixed:
 * each thread saves its own busy byte.
 *
 * The window is the forking thread's fork(2) call, a few tens of
 * microseconds, so the two forks must start together: a barrier that sleeps
 * (pthread_barrier_wait) wakes the second thread too late on an idle
 * machine.  Both threads spin instead.  Every round a fresh thread that has
 * never allocated starts its fork first and the main thread (which has a
 * heap) follows within a fraction of a microsecond, which is the order that
 * hands the heapless thread a busy byte of 0.  Both then allocate in the
 * child and in the parent.
 * Expected: "PASS", exit 0.  On the unfixed code the parent crashes within
 * a few rounds (FAIL, from the SIGSEGV handler below), or children fail.
 * Build it with -fno-builtin-malloc (run_tests.sh does): otherwise the
 * compiler may delete the allocations that would crash.
 */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static _Atomic int started;
static _Atomic long child_bad;
static pid_t parent_pid;

/* Inherited by the children: say which process crashed. */
static void on_crash(int sig)
{
    static const char in_parent[] = "FAIL: the parent crashed after a concurrent fork\n";
    static const char in_child[] = "the child of a concurrent fork crashed\n";
    (void)sig;
    if (getpid() == parent_pid) { if (write(1, in_parent, sizeof in_parent - 1) < 0) {} }
    else if (write(1, in_child, sizeof in_child - 1) < 0) {}
    _exit(1);
}

static void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || (defined(__arm__) && __ARM_ARCH >= 7)
    __asm__ __volatile__("yield");
#endif
}

static void do_fork(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        char *p = malloc(16);         /* crashes when the busy byte was swapped */
        if (p) p[0] = 1;
        _exit(p ? 0 : 2);
    }
    if (pid > 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0) {}
        if (WIFSIGNALED(st) || WEXITSTATUS(st) != 0) atomic_fetch_add(&child_bad, 1);
    }
}

/* Never allocates before its fork, and forks first. */
static void *heapless(void *arg)
{
    (void)arg;
    atomic_store(&started, 1);
    do_fork();
    char *q = malloc(16);             /* the parent side */
    if (q) q[0] = 1;
    free(q);
    return NULL;
}

int main(int argc, char **argv)
{
    double scale = getenv("TEST_SCALE") ? atof(getenv("TEST_SCALE")) : 1.0;
    int rounds = argc > 1 ? atoi(argv[1]) : (int)(1000 * scale);
    if (rounds < 100) rounds = 100;
    parent_pid = getpid();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_crash;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    void *keep = malloc(64);          /* the main thread has a heap */
    for (int i = 0; i < rounds; i++) {
        free(malloc(16));             /* busy back to 0 (a swapped NOHEAP heals here) */
        atomic_store(&started, 0);
        pthread_t a;
        if (pthread_create(&a, NULL, heapless, NULL) != 0) { printf("FAIL: pthread_create\n"); return 1; }
        while (!atomic_load(&started)) cpu_relax();
        do_fork();
        pthread_join(a, NULL);
    }
    free(keep);
    printf("%d rounds of two concurrent forks: %ld children failed\n", rounds, atomic_load(&child_bad));
    if (atomic_load(&child_bad)) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
