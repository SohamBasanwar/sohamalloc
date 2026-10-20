/*
 * regress/invalid_frees -- sohamalloc must abort on double and interior
 * frees in the default mode, and must not mistake valid patterns for them.
 *
 * Before the fix: free(A); free(B); free(A) was accepted (only an immediate
 * repeat was caught) and the next mallocs returned A twice; free(A + 16) of
 * a live block was accepted and the next malloc returned A + 16, inside a
 * live block.  Found by the security review (2026-10-20).
 *
 * Each case runs in a child process.  Bad cases must die with SIGABRT;
 * good cases must exit 0.
 * Status: fixed (owned_free / owned_free_check, block_ok on every free).
 */
#define _GNU_SOURCE 1
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

#define SZ 48

/* Through volatile pointers: the compiler may not elide or reorder the
 * malloc/free pairs these cases depend on. */
static void *(*volatile my_malloc)(size_t) = malloc;
static void (*volatile my_free)(void *) = free;
#define malloc(n) my_malloc(n)
#define free(p) my_free(p)

static void *volatile sink;

static void bad_aba(void) { char *a = malloc(SZ), *b = malloc(SZ); free(a); free(b); free(a); }
static void bad_deep(void) {
    char *a = malloc(SZ), *b = malloc(SZ);
    free(a);
    for (int i = 0; i < 20; i++) free(malloc(SZ));
    free(b);
    free(a);
}
static void bad_interior(void) { char *a = malloc(SZ); free(a + 16); }
static void bad_interior_big(void) { char *a = malloc(3000); free(a + 1024); }
static void bad_remote_then_local(void);
static void good_zeroed(void) { char *a = malloc(SZ); memset(a, 0, SZ); free(a); }
static void good_untouched(void) {
    /* Blocks never written by the program, freed in every order. */
    void *v[256];
    for (int i = 0; i < 256; i++) v[i] = malloc(SZ);
    for (int i = 0; i < 256; i += 2) free(v[i]);
    for (int i = 1; i < 256; i += 2) free(v[i]);
    for (int i = 0; i < 256; i++) v[i] = malloc(SZ);
    for (int i = 255; i >= 0; i--) free(v[i]);
}
static void good_link_lookalike(void) {
    /* A live block whose first word holds a pointer into its own page. */
    void **a = malloc(SZ), **b = malloc(SZ);
    a[0] = b; b[0] = a;
    free(a); free(b);
}

static void *remote_free_fn(void *p) { free(p); return NULL; }
static void bad_remote_then_local(void) {
    char *a = malloc(SZ);
    sink = malloc(SZ);
    pthread_t t;
    pthread_create(&t, NULL, remote_free_fn, a);
    pthread_join(t, NULL);
    free(a);            /* a is on its page's remote free list */
}

static int run(const char *name, void (*fn)(void), int expect_abort)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        sink = malloc(SZ);   /* use the heap after the free */
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int aborted = WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
    int ok = expect_abort ? aborted : (WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("%-22s %s (%s)\n", name, ok ? "ok" : "WRONG",
           aborted ? "aborted" : WIFEXITED(st) ? "exited" : "other signal");
    return ok;
}

int main(void)
{
    int ok = 1;
    ok &= run("double free A,B,A", bad_aba, 1);
    ok &= run("double free deep", bad_deep, 1);
    ok &= run("interior free", bad_interior, 1);
    ok &= run("interior free 3000", bad_interior_big, 1);
    ok &= run("remote, then local", bad_remote_then_local, 1);
    ok &= run("zeroed block", good_zeroed, 0);
    ok &= run("untouched blocks", good_untouched, 0);
    ok &= run("link look-alike", good_link_lookalike, 0);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
