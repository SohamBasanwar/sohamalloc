/*
 * regress/signal_fork_init -- sohamalloc: a signal handler that allocates
 * while fork()'s prepare handler runs the allocator's lazy init must not wait
 * for that init.
 *
 * When a process's first allocator use was fork(), fork_prepare ran the lazy
 * init before it set the busy byte.  A handler that called malloc inside two
 * windows of a few instructions each (after the init claim and before the
 * thread was marked as initialising; after that mark was cleared and before
 * init was published) found busy clear, went into init itself, and waited
 * forever for the init that its own interrupted frame was running.  Found by
 * review round 5 (2026-11-20).  Fixed: fork_prepare sets busy before it runs
 * init, as thread_prepare does, so such a handler is served from the static
 * arena.
 *
 * Method (x86-64 Linux only): a child process whose first allocator use is
 * fork() single-steps (trap flag) through sohamalloc's prepare handler, from
 * its first instruction until it returns (stepping must stop before fork()
 * blocks signals: a trap while SIGTRAP is blocked kills the process).  At the
 * chosen instructions inside the library a SIGTRAP handler calls malloc(32)
 * and free; a call that has not returned after 2 s is a hang (SIGALRM ends
 * the child).  The parent runs one child per instruction for the first
 * PREFIX instructions (the init claim is among them) and, after that, one
 * child per block of BLOCK instructions that fires at every instruction of
 * its block.  Each child is a fresh exec of this program, so the allocator is
 * not initialised in it; nothing in the child allocates before fork().
 * Expected: "PASS", exit 0.  SKIP (77) on other targets, when the library is
 * not mapped into the process (not preloaded), or when init does not run
 * inside fork() (then the scenario cannot be produced).
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !(defined(__x86_64__) && defined(__linux__))
int main(void) {
    printf("SKIP [signal_fork_init] needs x86-64 Linux (trap-flag single-stepping)\n");
    return 77;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/wait.h>
#include <ucontext.h>

extern char **environ;

#define PREFIX 400L      /* one child per instruction for K = 1..PREFIX */
#define BLOCK 64L        /* then one child per block of BLOCK instructions */
#define MIN_STEPS 1000L  /* fewer steps in fork(): init did not run there */
#define TF 0x100L

static uintptr_t lib_lo, lib_hi;
static volatile sig_atomic_t stepping;
static volatile pid_t step_pid;
static volatile long nsteps, fire_from, fire_to, fired, returned;
static volatile uintptr_t entry_sp;   /* stack pointer at the first library instruction */

static void say(int fd, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) { ssize_t w = write(fd, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1)); (void)w; }
}

static unsigned long hex(const char **s) {
    unsigned long v = 0;
    for (;; (*s)++) {
        char c = **s;
        if (c >= '0' && c <= '9') v = v * 16 + (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned long)(c - 'a' + 10);
        else return v;
    }
}

/* The executable mapping of libsohamalloc, from /proc/self/maps (no
 * allocation: open, read, a static buffer). */
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
        if (strstr(l, "libsohamalloc")) {
            const char *s = l;
            unsigned long lo = hex(&s);
            if (*s == '-') {
                s++;
                unsigned long hi = hex(&s);
                if (*s == ' ' && s[3] == 'x') { lib_lo = lo; lib_hi = hi; }
            }
        }
        if (!nl) break;
        l = nl + 1;
    }
}

static void on_trap(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ucv;
    /* Not stepping, or the fork child (it may inherit the trap flag). */
    if (!stepping || getpid() != step_pid) { uc->uc_mcontext.gregs[REG_EFL] &= ~TF; return; }
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    if (pc < lib_lo || pc >= lib_hi) {
        /* Outside the library above the prepare handler's frame: it has
         * returned (calls it makes into the C library run below it). */
        if (entry_sp && sp > entry_sp) { stepping = 0; uc->uc_mcontext.gregs[REG_EFL] &= ~TF; }
        return;
    }
    if (!entry_sp) entry_sp = sp;
    long k = ++nsteps;
    if (fire_from == 0 || k < fire_from) return;
    if (k >= fire_to) { stepping = 0; uc->uc_mcontext.gregs[REG_EFL] &= ~TF; return; }
    int e = errno;
    fired++;
    alarm(2);
    void *p = malloc(32);
    if (p) { memset(p, 1, 32); free(p); }
    alarm(0);
    returned++;
    errno = e;
}

static void on_start(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ((ucontext_t *)ucv)->uc_mcontext.gregs[REG_EFL] |= TF;
}

/* Registered after sohamalloc's atfork handlers, so this prepare handler
 * runs just before sohamalloc's. */
static void step_on(void) { step_pid = getpid(); stepping = 1; raise(SIGUSR2); }
static void step_off(void) { stepping = 0; }

/* Child: fire at instructions [from, to) of the library (from == 0: count
 * only).  Reports "steps fired returned" on fd 1.  Exit 3: library not found. */
static int child_main(long from, long to) {
    find_lib();
    if (!lib_lo) return 3;
    fire_from = from;
    fire_to = to;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = on_trap;
    sigaction(SIGTRAP, &sa, NULL);
    sa.sa_sigaction = on_start;
    sigaction(SIGUSR2, &sa, NULL);
    pthread_atfork(step_on, step_off, NULL);
    pid_t c = fork();
    if (c == 0) _exit(0);
    step_off();
    int st;
    while (c > 0 && waitpid(c, &st, 0) < 0 && errno == EINTR) {}
    say(1, "%ld %ld %ld\n", (long)nsteps, (long)fired, (long)returned);
    return fired == returned ? 0 : 1;
}

static pid_t spawn_child(long from, long to, int out_fd) {
    char a[24], b[24];
    snprintf(a, sizeof a, "%ld", from);
    snprintf(b, sizeof b, "%ld", to);
    char *argv[] = { (char *)"signal_fork_init", (char *)"--child", a, b, NULL };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
    pid_t pid;
    int r = posix_spawn(&pid, "/proc/self/exe", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    return r == 0 ? pid : -1;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--child") == 0) return child_main(atol(argv[2]), atol(argv[3]));
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 1. Count the library instructions that fork() runs. */
    int pc[2];
    if (pipe(pc) != 0) { printf("FAIL [signal_fork_init] pipe\n"); return 1; }
    pid_t p0 = spawn_child(0, 0, pc[1]);
    close(pc[1]);
    char line[128] = { 0 };
    ssize_t got = p0 > 0 ? read(pc[0], line, sizeof line - 1) : -1;
    close(pc[0]);
    int st0 = 0;
    if (p0 > 0) waitpid(p0, &st0, 0);
    if (p0 > 0 && WIFEXITED(st0) && WEXITSTATUS(st0) == 3) {
        printf("SKIP [signal_fork_init] libsohamalloc is not mapped in the process (not preloaded)\n");
        return 77;
    }
    long total = got > 0 ? atol(line) : 0;
    if (!(p0 > 0 && WIFEXITED(st0) && WEXITSTATUS(st0) == 0)) {
        printf("FAIL [signal_fork_init] the counting child failed (status 0x%x)\n", st0);
        return 1;
    }
    if (total < MIN_STEPS) {
        printf("SKIP [signal_fork_init] fork() ran only %ld library instructions: the allocator was initialised before it\n", total);
        return 77;
    }

    /* 2. The sweep: [k, k+1) for k <= PREFIX, then blocks of BLOCK. */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int par = ncpu > 8 ? 8 : ncpu > 1 ? (int)ncpu : 1;
    int rp[2];
    if (pipe(rp) != 0) { printf("FAIL [signal_fork_init] pipe\n"); return 1; }
    fcntl(rp[0], F_SETFL, O_NONBLOCK);
    long last = total + BLOCK;
    long nchild = 0, hangs = 0, errors = 0, fired_total = 0;
    struct { pid_t pid; long from, to; } run[8];
    int nrun = 0;
    char bad[1024] = { 0 };
    size_t badn = 0;
    long next = 1;
    static char rbuf[1 << 16];
    size_t rlen = 0;
    while (next <= last || nrun > 0) {
        while (nrun < par && next <= last) {
            long from = next, to = next <= PREFIX ? next + 1 : next + BLOCK;
            pid_t p = spawn_child(from, to, rp[1]);
            if (p < 0) { errors++; next = to; continue; }
            run[nrun].pid = p; run[nrun].from = from; run[nrun].to = to; nrun++;
            nchild++;
            next = to;
        }
        int st;
        pid_t p = waitpid(-1, &st, 0);
        if (p < 0) { if (errno == EINTR) continue; break; }
        ssize_t r;
        while ((r = read(rp[0], rbuf + rlen, sizeof rbuf - 1 - rlen)) > 0) rlen += (size_t)r;
        for (int i = 0; i < nrun; i++) {
            if (run[i].pid != p) continue;
            int hang = WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM;
            int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
            if (hang) hangs++;
            else if (!ok) errors++;
            if ((hang || !ok) && badn < sizeof bad - 64)
                badn += (size_t)snprintf(bad + badn, sizeof bad - badn, " K=%ld..%ld:%s", run[i].from, run[i].to - 1,
                                         hang ? "hang" : WIFEXITED(st) ? "exit" : "signal");
            run[i] = run[--nrun];
            break;
        }
    }
    close(rp[1]);
    ssize_t r;
    while ((r = read(rp[0], rbuf + rlen, sizeof rbuf - 1 - rlen)) > 0) rlen += (size_t)r;
    close(rp[0]);
    rbuf[rlen] = 0;
    for (char *l = rbuf; *l; ) {
        long s, f, ret;
        if (sscanf(l, "%ld %ld %ld", &s, &f, &ret) == 3) fired_total += f;
        char *nl = strchr(l, '\n');
        if (!nl) break;
        l = nl + 1;
    }
    printf("fork() runs %ld library instructions; %ld children; handler calls made %ld; hangs %ld; other failures %ld\n",
           total, nchild, fired_total, hangs, errors);
    if (fired_total == 0) { printf("FAIL [signal_fork_init] no handler call was made: the test did not exercise fork()\n"); return 1; }
    if (hangs || errors) {
        printf("FAIL [signal_fork_init] a handler that allocated inside fork() did not return:%s\n", bad);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
#endif
