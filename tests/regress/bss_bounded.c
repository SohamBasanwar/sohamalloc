/*
 * regress/bss_bounded -- sohamalloc: the static arena's bump counter must
 * stay within the arena when requests fail.
 *
 * bss_alloc() advanced g_bss_top by the request size on every call, including
 * the calls that failed because the 384 KiB arena was full.  On 32-bit
 * targets the size_t counter wrapped after ~4 GiB of failed handler-context
 * requests; the bounds check then passed and bss_alloc returned (and wrote a
 * size header through) a pointer outside the arena.  Found by review round 3
 * (2026-10-21; R3-CONCURRENCY-3 = R3-CORRECTNESS-3).  Fixed: a bounded bump
 * (compare-and-swap) that never moves the counter past the arena.
 *
 * Through the internal test hooks: fill the arena, then make 20000 more
 * requests of 300000 bytes (6 GB in total, enough to wrap a 32-bit counter)
 * and check that every one fails and the counter does not move.  On 64-bit
 * targets the unfixed code shows only the counter running past the arena; on
 * 32-bit targets it also returns pointers (or crashes writing the header).
 * Expected: "PASS", exit 0; SKIP (77) when the hooks are absent.
 */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../src/internal_test.h"

static void on_crash(int sig)
{
    static const char msg[] = "FAIL: crashed in bss_alloc on a full arena (counter wrapped)\n";
    (void)sig;
    if (write(1, msg, sizeof msg - 1) < 0) {}
    _exit(1);
}

int main(void)
{
    void *(*bss_alloc)(size_t, size_t) = (void *(*)(size_t, size_t))dlsym(RTLD_DEFAULT, "soham_internal_bss_alloc");
    size_t (*bss_top)(void) = (size_t (*)(void))dlsym(RTLD_DEFAULT, "soham_internal_bss_top");
    if (!bss_alloc || !bss_top) { printf("SKIP: internal hooks absent\n"); return 77; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_crash;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    unsigned filled = 0;
    while (bss_alloc(4000, 64)) {
        if (++filled > SOHAM_INTERNAL_BSS_ARENA_SIZE / 4000) { printf("FAIL: arena larger than declared\n"); return 1; }
    }
    size_t top = bss_top();
    int bad = 0;
    if (top > SOHAM_INTERNAL_BSS_ARENA_SIZE) { printf("counter %zu beyond the arena after filling\n", top); bad = 1; }
    long returned = 0, first = -1;
    for (int i = 0; i < 20000; i++) {
        if (bss_alloc(300000, 64)) { returned++; if (first < 0) first = i; }
    }
    size_t top2 = bss_top();
    printf("arena filled with %u blocks; counter %zu after filling, %zu after 20000 failed requests\n",
           filled, top, top2);
    if (returned) { printf("%ld requests on a full arena returned a pointer (first: request %ld)\n", returned, first); bad = 1; }
    if (top2 != top) { printf("the counter grows on failed requests\n"); bad = 1; }
    if (bad) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
