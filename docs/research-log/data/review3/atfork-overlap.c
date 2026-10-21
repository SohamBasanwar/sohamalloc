/* Per round: which thread's prepare ran first, and did the second prepare start
 * while the first thread was still between its prepare and parent handlers? */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
static _Atomic int inside, first, overlap, nprep;
static __thread int me;
static void prep(void) {
  int k = atomic_fetch_add(&nprep, 1);
  if (k == 0) atomic_store(&first, me);
  if (atomic_fetch_add(&inside, 1) > 0) atomic_store(&overlap, 1);
}
static void parent(void) { atomic_fetch_sub(&inside, 1); }
static _Atomic int started;
static void *th(void *a) { (void)a; me = 1; atomic_store(&started, 1); pid_t p = fork(); if (p == 0) _exit(0); waitpid(p, NULL, 0); return NULL; }
int main(int argc, char **argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 1000;
  long tf_ov = 0, tf_no = 0, mf_ov = 0, mf_no = 0;
  me = 2;
  pthread_atfork(prep, parent, NULL);
  for (int i = 0; i < rounds; i++) {
    atomic_store(&started, 0); atomic_store(&nprep, 0); atomic_store(&overlap, 0);
    pthread_t t; pthread_create(&t, NULL, th, NULL);
    while (!atomic_load(&started)) {}
    pid_t p = fork(); if (p == 0) _exit(0); waitpid(p, NULL, 0);
    pthread_join(t, NULL);
    if (first == 1) { if (overlap) tf_ov++; else tf_no++; } else { if (overlap) mf_ov++; else mf_no++; }
  }
  printf("thread first: overlap %ld, none %ld | main first: overlap %ld, none %ld\n", tf_ov, tf_no, mf_ov, mf_no);
  return 0;
}
