/*
 * regress/signal_fastpath_defer -- sohamalloc: a run freed by a signal
 * handler that interrupted a FAST path must be released by the time the
 * interrupted call returns.
 *
 * In handler context a run whose page-heap lock cannot be taken (trylock) is
 * put on the thread's deferred list.  Review round 4 made the slow paths
 * drain that list when they finish, but the malloc and free fast paths
 * returned without looking at it, so a thread that kept to the fast paths
 * kept such runs until its next slow path, however long that took.  A fast
 * path meets this when the free it makes releases a page (it then holds the
 * page-heap lock itself) or when another thread holds that lock.  Found by
 * review round 5 (2026-11-20).  Fixed: the fast paths look at the list after
 * their final busy store and drain it when it is not empty.
 *
 * Method (x86-64 Linux only): the main thread single-steps (trap flag); at
 * every instruction inside libsohamalloc a SIGTRAP handler frees one run of
 * 300 KB from a stash.
 * Part 1 (deterministic): main frees 32 medium blocks, all through the free
 * fast path; the frees that empty a page release it under the page-heap lock.
 * Part 2: while a second thread calls soham_get_stats in a loop (it walks
 * every page under the page-heap lock, so it holds that lock much of the
 * time), main makes single malloc(32) and free calls, both fast paths, each
 * stepped on its own; the handler frees one run at the K-th library
 * instruction of the call, for K = 1..KMAX, ROUNDS times over.  A handler
 * call that lands where busy is clear frees in normal context and drains the
 * list itself, which would hide the defect, so in_use is checked after every
 * call.
 * In_use is read by a helper thread, with stepping off (its soham_get_stats
 * drains only its own list): every run the handler freed must be gone.  Expected: "PASS", exit 0.  SKIP (77) when the allocator is not
 * sohamalloc, when the library is not mapped in the process (not preloaded),
 * or on other targets.
 */
#define _GNU_SOURCE 1
#include <stdio.h>

#if !(defined(__x86_64__) && defined(__linux__))
int main(void) {
    printf("SKIP [signal_fastpath_defer] needs x86-64 Linux (trap-flag single-stepping)\n");
    return 77;
}
#else
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include "sohamalloc.h"

#define NRUNS 3000
#define RUN ((size_t)300000)
#define NB 32
#define KMAX 64
#define ROUNDS 8
#define TF 0x100L

static uintptr_t lib_lo, lib_hi;
static volatile sig_atomic_t stepping;
static void **stash;
static volatile int next_run, nstash;
static volatile long ksteps, fire_at;   /* part 2: free one run at library instruction fire_at */
static int (*get_stats)(struct soham_stats *);

static void find_lib(void) {
    static char buf[1 << 17];
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return;
    size_t tot = 0;
    ssize_t r;
    while (tot < sizeof buf - 1 && (r = read(fd, buf + tot, sizeof buf - 1 - tot)) > 0) tot += (size_t)r;
    close(fd);
    buf[tot] = 0;
    for (char *l = buf; *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        unsigned long lo, hi;
        char perm[8];
        if (strstr(l, "libsohamalloc") && sscanf(l, "%lx-%lx %7s", &lo, &hi, perm) == 3 && perm[2] == 'x') {
            lib_lo = lo;
            lib_hi = hi;
        }
        if (!nl) break;
        l = nl + 1;
    }
}

static void on_trap(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ucv;
    if (!stepping) { uc->uc_mcontext.gregs[REG_EFL] &= ~TF; return; }
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    if (pc < lib_lo || pc >= lib_hi) return;
    if (fire_at && ++ksteps != fire_at) return;
    int k = next_run;
    if (k >= nstash) return;
    next_run = k + 1;
    int e = errno;
    free(stash[k]);
    errno = e;
}
static void on_start(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ((ucontext_t *)ucv)->uc_mcontext.gregs[REG_EFL] |= TF;
}
static void step_on(void) { stepping = 1; raise(SIGUSR2); }
static void step_off(void) {
    stepping = 0;
    /* The trap flag is cleared at the next trap. */
    for (volatile int i = 0; i < 1000; i++) {}
}

/* in_use, read by another thread. */
static sem_t mreq, mrsp;
static size_t meas;
static void *measurer(void *arg) {
    (void)arg;
    for (;;) {
        sem_wait(&mreq);
        struct soham_stats st;
        memset(&st, 0, sizeof st);
        get_stats(&st);
        meas = st.in_use;
        sem_post(&mrsp);
    }
    return NULL;
}
static size_t in_use(void) { sem_post(&mreq); sem_wait(&mrsp); return meas; }

/* Part 2's lock holder: soham_get_stats in a loop while `churn` is set. */
static sem_t cstart, cparked;
static atomic_int churn, quit, walks;
static void *churner(void *arg) {
    (void)arg;
    for (;;) {
        sem_wait(&cstart);
        if (atomic_load(&quit)) return NULL;
        while (atomic_load_explicit(&churn, memory_order_relaxed)) {
            struct soham_stats st;
            get_stats(&st);
            atomic_fetch_add_explicit(&walks, 1, memory_order_relaxed);
            /* A pause, so that the threads that wait for the lock get it. */
            for (int i = 0; i < 64; i++) __builtin_ia32_pause();
        }
        sem_post(&cparked);
    }
}

static int fill_stash(void) {
    for (int i = 0; i < NRUNS; i++) {
        stash[i] = malloc(RUN);
        if (!stash[i]) return 0;
        *(char *)stash[i] = 1;
    }
    next_run = 0;
    nstash = NRUNS;
    return 1;
}
static void empty_stash(void) {
    nstash = 0;
    for (int k = next_run; k < NRUNS; k++) free(stash[k]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    get_stats = (int (*)(struct soham_stats *))dlsym(RTLD_DEFAULT, "soham_get_stats");
    if (!get_stats) { printf("SKIP [signal_fastpath_defer] the allocator is not sohamalloc\n"); return 77; }
    find_lib();
    if (!lib_lo) { printf("SKIP [signal_fastpath_defer] libsohamalloc is not mapped in the process (not preloaded)\n"); return 77; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = on_trap;
    sigaction(SIGTRAP, &sa, NULL);
    sa.sa_sigaction = on_start;
    sigaction(SIGUSR2, &sa, NULL);
    sem_init(&mreq, 0, 0); sem_init(&mrsp, 0, 0);
    sem_init(&cstart, 0, 0); sem_init(&cparked, 0, 0);
    pthread_t mt, ct;
    pthread_create(&mt, NULL, measurer, NULL);
    pthread_create(&ct, NULL, churner, NULL);
    stash = malloc(NRUNS * sizeof *stash);
    if (!stash || !fill_stash()) { printf("FAIL [signal_fastpath_defer] malloc failed\n"); return 1; }
    size_t acct = malloc_usable_size(stash[0]);
    int fail = 0;

    /* Part 1: the free fast path releases pages under the page-heap lock. */
    static void *blk[NB];
    for (int i = 0; i < NB; i++) { blk[i] = malloc(40000); *(char *)blk[i] = 1; }
    size_t bacct = malloc_usable_size(blk[0]);
    { void *w = malloc(32); free(w); }   /* class 32 has a current page */
    size_t before = in_use();
    step_on();
    for (int i = 0; i < NB; i++) free(blk[i]);
    step_off();
    int f1 = next_run;
    long missing = (long)(f1 * acct + NB * bacct) - (long)(before - in_use());
    for (int r = 0; r < 100000; r++) { void *p = malloc(32); *(volatile char *)p = 1; free(p); }
    long missing_later = (long)(f1 * acct + NB * bacct) - (long)(before - in_use());
    printf("part 1: the handler freed %d runs of %zu B; still allocated after the free() calls returned: %ld, after 100000 more fast-path pairs: %ld\n",
           f1, acct, missing / (long)acct, missing_later / (long)acct);
    empty_stash();
    if (f1 == 0) { printf("FAIL [signal_fastpath_defer] part 1: the handler never ran (no step inside the library)\n"); return 1; }
    if (missing >= (long)acct) {
        printf("FAIL [signal_fastpath_defer] part 1: runs freed in a handler stay allocated after the interrupted free() returned\n");
        fail = 1;
    }

    /* Part 2: single fast-path calls while another thread holds the lock. */
    if (!fill_stash()) { printf("FAIL [signal_fastpath_defer] malloc failed\n"); return 1; }
    void *keep = malloc(32);   /* the calls below reuse one block of a current page */
    atomic_store(&churn, 1);
    sem_post(&cstart);
    while (atomic_load(&walks) < 3) {}   /* the lock holder is running */
    size_t base = in_use();
    long calls = 0, worst = 0;
    const char *worst_call = "";
    long worst_k = 0;
    for (int round = 0; round < ROUNDS && next_run < NRUNS - 2; round++) {
        for (long k = 1; k <= KMAX && next_run < NRUNS - 2; k++) {
            for (int which = 0; which < 2; which++) {
                static void *p;
                ksteps = 0;
                fire_at = k;
                step_on();
                if (which == 0) { p = malloc(32); } else { free(p); }
                step_off();
                fire_at = 0;
                if (which == 0) *(volatile char *)p = 1;
                calls++;
                long m = (long)(next_run * acct) - (long)(base - in_use());
                if (m > worst) { worst = m; worst_call = which ? "free" : "malloc"; worst_k = k; }
            }
        }
    }
    atomic_store(&churn, 0);
    sem_wait(&cparked);
    int f2 = next_run;
    printf("part 2: %ld stepped calls, the handler freed %d runs (page walks meanwhile: %d); most runs still allocated right after a call returned: %ld",
           calls, f2, atomic_load(&walks), worst / (long)acct);
    if (worst >= (long)acct) printf(" (after %s, run freed at its library instruction %ld)", worst_call, worst_k);
    printf("\n");
    empty_stash();
    free(keep);
    if (f2 == 0) { printf("FAIL [signal_fastpath_defer] part 2: the handler never ran\n"); return 1; }
    if (worst >= (long)acct) {
        printf("FAIL [signal_fastpath_defer] part 2: a run freed in a handler stayed allocated after the interrupted fast path returned\n");
        fail = 1;
    }

    atomic_store(&quit, 1);
    sem_post(&cstart);
    pthread_join(ct, NULL);
    free(stash);
    if (fail) return 1;
    printf("PASS\n");
    return 0;
}
#endif
