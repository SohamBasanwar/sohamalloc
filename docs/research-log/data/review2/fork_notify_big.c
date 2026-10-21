/* Scale variant of fork_notify.c: B builds ~96 MiB of 48-byte blocks; in the
 * fork child, main frees them all, then thread C (which attaches B's heap
 * from the pool) allocates the same amount.  Reports child RSS growth. */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#define NB 2000000
#define SZ 48
static void **bblk;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int b_ready, b_quit;
static long rss_mib(void) {
    long sz = 0, res = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%ld %ld", &sz, &res) != 2) res = 0; fclose(f); }
    return res * 4096 / (1024 * 1024);
}
static void *thread_b(void *a) {
    (void)a;
    for (int i = 0; i < NB; i++) bblk[i] = malloc(SZ);
    pthread_mutex_lock(&mu); b_ready = 1; pthread_cond_broadcast(&cv);
    while (!b_quit) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return NULL;
}
static void *thread_c(void *a) {
    (void)a;
    for (int i = 0; i < NB; i++) bblk[i] = malloc(SZ);
    return NULL;
}
int main(void) {
    bblk = calloc(NB, sizeof(void *));
    pthread_t b;
    pthread_create(&b, NULL, thread_b, NULL);
    pthread_mutex_lock(&mu); while (!b_ready) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu);
    pid_t pid = fork();
    if (pid == 0) {
        long r0 = rss_mib();
        for (int i = 0; i < NB; i++) free(bblk[i]);
        long r1 = rss_mib();
        pthread_t c;
        pthread_create(&c, NULL, thread_c, NULL);
        pthread_join(c, NULL);
        long r2 = rss_mib();
        printf("child RSS: %ld MiB at fork, %ld after freeing B's blocks, %ld after C reallocates them (+%ld)\n", r0, r1, r2, r2 - r1);
        fflush(stdout);
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    pthread_mutex_lock(&mu); b_quit = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    pthread_join(b, NULL);
    return 0;
}
