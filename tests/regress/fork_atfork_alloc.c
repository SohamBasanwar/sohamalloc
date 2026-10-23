/*
 * regress/fork_atfork_alloc -- sohamalloc: atfork handlers that were
 * registered before the allocator's first use can allocate.
 *
 * Such handlers run inside the fork window: their prepare handler runs after
 * fork_prepare has taken every allocator lock, and their parent and child
 * handlers before fork_parent/fork_child release them.  Their allocations
 * were treated like signal-handler allocations: small ones used up the static
 * arena, larger ones (and then all) returned NULL.  Found by review round 4
 * (2026-10-23).  Fixed: the forking thread holds every lock in that window,
 * so its blocking locks become no-ops and it allocates normally.
 *
 * pthread_atfork is called before any allocation; each handler allocates
 * 64 bytes and 64 KiB and frees them; the process forks 300 times.
 * Expected: "PASS" (0 NULL), exit 0.
 */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile long calls, nulls;

static void use(void)
{
    calls++;
    char *a = malloc(64), *b = malloc(65536);
    if (!a || !b) nulls++;
    if (a) a[0] = 1;
    if (b) { b[0] = 1; b[65535] = 2; }
    free(a);
    free(b);
}

static void prepare(void) { use(); }
static void parent(void) { use(); }
static void child(void) { use(); }

int main(void)
{
    /* First, before any allocation: these handlers precede the allocator's. */
    pthread_atfork(prepare, parent, child);
    long child_nulls = 0;
    for (int i = 0; i < 300; i++) {
        pid_t pid = fork();
        if (pid == 0) _exit(nulls ? 1 : 0);   /* the child handler ran here */
        if (pid < 0) { printf("FAIL: fork\n"); return 1; }
        int st = 0;
        while (waitpid(pid, &st, 0) < 0) {}
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) child_nulls++;
    }
    printf("%ld handler calls in the parent, %ld NULL; %ld children saw NULL\n", calls, nulls, child_nulls);
    if (nulls || child_nulls) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
