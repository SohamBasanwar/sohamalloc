/*
 * fork_test -- fork while threads hammer the allocator (spec §8 fork
 * protocol; plan §5.10 and §8.4 "fork from many threads mid-free").
 *
 * 8 hammer threads run malloc/calloc/realloc/free with patterns.  The test
 * forks 50 times (scaled), alternating between forking from the main thread
 * and from a non-main thread.  The child must:
 *   - verify and free blocks the forking thread allocated before the fork;
 *   - verify and free every hammer-thread block that was in a stable state
 *     at the fork instant (frees into heaps whose threads do not exist in
 *     the child);
 *   - run a heavy single-threaded alloc/free workload, then a short
 *     multi-threaded one with cross-thread frees;
 *   - exit with status 0 (via exit() on odd iterations, so atexit handlers
 *     and allocator destructors run, and _exit() on even ones).
 * A child that does not finish within 30 s is killed and reported as a
 * deadlock.
 */
#include "common/test_util.h"

#include <signal.h>
#include <sys/wait.h>

enum { HAMMERS = 8, HSLOTS = 256, INHERIT = 128 };

/* A slot is published with state STABLE (release) only after the block and
   its pattern are complete; the child reads state (acquire) on its
   snapshot and touches only STABLE slots. */
enum { EMPTY = 0, BUSY = 1, STABLE = 2 };
typedef struct {
    _Atomic int state;
    void *p;
    size_t n;
    uint64_t seed;
} hslot;

static hslot (*hs)[HSLOTS];
static _Atomic int stop;
static _Atomic unsigned long hammer_ops;

static void *hammer(void *arg)
{
    int id = (int)(intptr_t)arg;
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ ((uint64_t)id << 32));
    hslot *sl = hs[id];
    uint64_t op = 0;
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        hslot *s = &sl[tu_rng_below(&r, HSLOTS)];
        /* BUSY must be visible before any change to the slot, as seen by a
           fork snapshot taken on another CPU: a full fence orders the BUSY
           store before the later plain stores. */
        atomic_store_explicit(&s->state, BUSY, memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        if (s->p) {
            size_t bad = tu_check(s->p, s->n, s->n, s->seed);
            if (bad != SIZE_MAX) TU_FAIL("hammer %d: block %p (n=%zu) corrupted at %zu", id, s->p, s->n, bad);
            if (tu_rng_below(&r, 4) == 0) {
                size_t m = tu_rand_size(&r, 200000);
                void *q = T_REALLOC(s->p, m ? m : 1);
                if (!q) TU_FAIL("hammer realloc NULL");
                size_t keep = s->n < m ? s->n : m;
                if (tu_check(q, s->n, keep, s->seed) != SIZE_MAX) TU_FAIL("hammer realloc lost content");
                s->p = q;
                s->n = m ? m : 1;
                s->seed = tu_mix64(((uint64_t)id << 40) ^ op);
                tu_fill(q, s->n, s->seed);
            } else {
                T_FREE(s->p);
                s->p = NULL;
            }
        } else {
            size_t n = tu_rand_size(&r, 300000);
            void *p = (op & 7) == 0 ? T_CALLOC(1, n) : T_MALLOC(n);
            if (!p) TU_FAIL("hammer alloc(%zu) NULL", n);
            s->p = p;
            s->n = n;
            s->seed = tu_mix64(((uint64_t)id << 40) ^ op);
            tu_fill(p, n, s->seed);
        }
        atomic_store_explicit(&s->state, s->p ? STABLE : EMPTY, memory_order_release);
        op++;
        if ((op & 255) == 0) atomic_fetch_add_explicit(&hammer_ops, 256, memory_order_relaxed);
    }
    for (int i = 0; i < HSLOTS; i++)
        if (sl[i].p) {
            if (tu_check(sl[i].p, sl[i].n, sl[i].n, sl[i].seed) != SIZE_MAX) TU_FAIL("hammer final check");
            T_FREE(sl[i].p);
            sl[i].p = NULL;
        }
    return NULL;
}

/* ------------------------------------------------------------- child side */
typedef struct { void *p; size_t n; uint64_t seed; } blk;
static blk inherit[INHERIT];

static void child_fail(const char *msg)
{
    tu_write_str(2, "FAIL [fork_test] child: ");
    tu_write_str(2, msg);
    tu_write_str(2, "\n");
    _exit(1);
}

static void *child_thread(void *arg)
{
    /* Cross-thread frees inside the child: free blocks the child's main
       thread allocated, and allocate a few of our own. */
    blk *b = (blk *)arg;
    for (int i = 0; i < 64; i++) {
        if (tu_check(b[i].p, b[i].n, b[i].n, b[i].seed) != SIZE_MAX) child_fail("cross-thread block corrupted");
        T_FREE(b[i].p);
        void *q = T_MALLOC(64 + (size_t)i * 100);
        if (!q) child_fail("malloc in child thread");
        memset(q, i, 64 + (size_t)i * 100);
        T_FREE(q);
    }
    return NULL;
}

static void child_main(int iter, int forker_id)
{
    /* 1. Blocks the forking thread allocated before the fork. */
    if (forker_id < 0) {
        for (int i = 0; i < INHERIT; i++) {
            if (tu_check(inherit[i].p, inherit[i].n, inherit[i].n, inherit[i].seed) != SIZE_MAX)
                child_fail("inherited block corrupted");
            if (i & 1) {
                T_FREE(inherit[i].p);
            } else {
                void *q = T_REALLOC(inherit[i].p, inherit[i].n * 2 + 10);
                if (!q) child_fail("realloc of inherited block");
                if (tu_check(q, inherit[i].n, inherit[i].n, inherit[i].seed) != SIZE_MAX)
                    child_fail("realloc of inherited block lost content");
                T_FREE(q);
            }
        }
    }
    /* 2. Stable blocks of all hammer threads (none of which exist here). */
    size_t freed = 0;
    for (int t = 0; t < HAMMERS; t++)
        for (int i = 0; i < HSLOTS; i++) {
            hslot *s = &hs[t][i];
            if (atomic_load_explicit(&s->state, memory_order_acquire) != STABLE) continue;
            if (tu_check(s->p, s->n, s->n, s->seed) != SIZE_MAX) child_fail("stable hammer block corrupted");
            T_FREE(s->p);
            freed++;
        }
    /* 3. Heavy single-threaded workload. */
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v ^ (uint64_t)iter);
    enum { CS = 512 };
    static blk cb[CS];
    memset(cb, 0, sizeof cb);
    size_t ops = tu_scaled_min(40000, 4000);
    for (size_t k = 0; k < ops; k++) {
        blk *b = &cb[tu_rng_below(&r, CS)];
        if (b->p) {
            if (tu_check(b->p, b->n, b->n, b->seed) != SIZE_MAX) child_fail("child block corrupted");
            T_FREE(b->p);
            b->p = NULL;
        } else {
            b->n = tu_rand_size(&r, 1u << 20);
            b->p = T_MALLOC(b->n);
            if (!b->p) child_fail("child malloc NULL");
            b->seed = tu_mix64(k);
            tu_fill(b->p, b->n, b->seed);
        }
    }
    for (int i = 0; i < CS; i++)
        if (cb[i].p) T_FREE(cb[i].p);
    /* 4. Threads in the child, with cross-thread frees. */
    static blk xb[2][64];
    pthread_t th[2];
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < 64; i++) {
            xb[t][i].n = 16 + (size_t)i * 77;
            xb[t][i].p = T_MALLOC(xb[t][i].n);
            if (!xb[t][i].p) child_fail("malloc for child threads");
            xb[t][i].seed = (uint64_t)(t * 1000 + i);
            tu_fill(xb[t][i].p, xb[t][i].n, xb[t][i].seed);
        }
        if (pthread_create(&th[t], NULL, child_thread, xb[t]) != 0) child_fail("pthread_create in child");
    }
    for (int t = 0; t < 2; t++) pthread_join(th[t], NULL);
    (void)freed;
    if (iter & 1) exit(0);
    _exit(0);
}

static void wait_child(pid_t pid, int iter)
{
    double t0 = tu_now();
    for (;;) {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return;
            if (WIFSIGNALED(st)) TU_FAIL("fork %d: child killed by signal %d", iter, WTERMSIG(st));
            TU_FAIL("fork %d: child exited with status %d", iter, WEXITSTATUS(st));
        }
        if (w < 0 && errno != EINTR) TU_FAIL("waitpid: errno %d", errno);
        if (tu_now() - t0 > 30.0) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            TU_FAIL("fork %d: child did not finish in 30 s (deadlock after fork?)", iter);
        }
        tu_sleep_ms(1);
    }
}

/* ------------------------------------------------------------- forker thread */
typedef struct { int iter; pid_t pid; } fork_req;

static void *forker(void *arg)
{
    fork_req *fr = (fork_req *)arg;
    /* Allocate in this thread right before forking, so the forking thread
       has a live heap with recent activity. */
    void *mine[16];
    for (int i = 0; i < 16; i++) {
        mine[i] = T_MALLOC(32 + (size_t)i * 512);
        if (!mine[i]) TU_FAIL("forker malloc");
        memset(mine[i], 0x33, 32 + (size_t)i * 512);
    }
    pid_t pid = fork();
    if (pid < 0) TU_FAIL("fork: errno %d", errno);
    if (pid == 0) {
        for (int i = 0; i < 16; i++) T_FREE(mine[i]);
        child_main(fr->iter, 1);
    }
    for (int i = 0; i < 16; i++) T_FREE(mine[i]);
    fr->pid = pid;
    return NULL;
}

int main(int argc, char **argv)
{
    tu_init("fork_test", argc, argv);
    int iters = (int)tu_scaled_min(50, 10);
    hs = (hslot(*)[HSLOTS])tu_os_alloc(sizeof(hslot) * HAMMERS * HSLOTS);
    pthread_t th[HAMMERS];
    for (int i = 0; i < HAMMERS; i++) th[i] = tu_thread_start(hammer, (void *)(intptr_t)i);
    tu_rng r;
    tu_rng_seed(&r, tu_seed_v);
    double t0 = tu_now();
    for (int it = 0; it < iters; it++) {
        /* Let the hammers run a little between forks. */
        tu_sleep_ms(5 + (unsigned)tu_rng_below(&r, 20));
        if (it % 2 == 0) {
            for (int i = 0; i < INHERIT; i++) {
                inherit[i].n = tu_rand_size(&r, 2u << 20);
                inherit[i].p = T_MALLOC(inherit[i].n);
                if (!inherit[i].p) TU_FAIL("malloc");
                inherit[i].seed = tu_mix64(((uint64_t)it << 16) + (uint64_t)i);
                tu_fill(inherit[i].p, inherit[i].n, inherit[i].seed);
            }
            pid_t pid = fork();
            if (pid < 0) TU_FAIL("fork: errno %d", errno);
            if (pid == 0) child_main(it, -1);
            for (int i = 0; i < INHERIT; i++) {
                if (tu_check(inherit[i].p, inherit[i].n, inherit[i].n, inherit[i].seed) != SIZE_MAX)
                    TU_FAIL("parent: inherited block corrupted after fork");
                T_FREE(inherit[i].p);
            }
            wait_child(pid, it);
        } else {
            fork_req fr = { it, -1 };
            pthread_t ft = tu_thread_start(forker, &fr);
            tu_thread_join(ft);
            wait_child(fr.pid, it);
        }
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < HAMMERS; i++) tu_thread_join(th[i]);
    tu_info("PASS %d forks (half from a non-main thread) in %.2fs; hammer ops %lu", iters, tu_now() - t0,
            atomic_load(&hammer_ops));
    tu_os_free(hs, sizeof(hslot) * HAMMERS * HSLOTS);
    return 0;
}
