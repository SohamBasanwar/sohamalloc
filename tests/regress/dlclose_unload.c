/*
 * regress/dlclose_unload -- sohamalloc: a module that embeds the prefixed
 * archive can be unloaded with dlclose().
 *
 * soham_init creates a pthread key whose destructor (thread_exit_cb) lives in
 * the module.  Nothing deleted it, so after dlclose() unmapped the module,
 * the next exiting thread that had allocated through it made glibc call the
 * stale destructor: SIGSEGV.  Found by review round 4 (2026-10-23).  Fixed:
 * an ELF destructor deletes the key (soham_os_thread_key_fini).
 *
 * One source, two roles.  Built with -DTEST_PLUGIN it is the module (linked
 * with the prefixed archive into <test>.plugin.so by tests/run_tests.sh);
 * otherwise it is the host: a thread allocates through the module, the host
 * unloads the module, then the thread exits.
 * Expected: "PASS", exit 0; SKIP (77) when the module stays loaded.
 */
#define _GNU_SOURCE 1
#include <stddef.h>

#ifdef TEST_PLUGIN

#include "sohamalloc.h"

void *plugin_alloc(size_t n) { return soham_malloc(n); }
void plugin_free(void *p) { soham_free(p); }

#else

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void *(*p_alloc)(size_t);
static void (*p_free)(void *);
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int allocated, may_exit;

static void on_crash(int sig)
{
    static const char msg[] = "FAIL: a thread exiting after dlclose crashed (stale key destructor)\n";
    (void)sig;
    if (write(1, msg, sizeof msg - 1) < 0) {}
    _exit(1);
}

/* Runs in the host: allocates through the module (which sets the module's
 * thread key for this thread), then waits until the module is gone. */
static void *worker(void *arg)
{
    (void)arg;
    void *p = p_alloc(64);
    if (p) { memset(p, 1, 64); p_free(p); }
    pthread_mutex_lock(&mu);
    allocated = 1;
    pthread_cond_broadcast(&cv);
    while (!may_exit) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return NULL;   /* thread exit: TSD destructors run here */
}

int main(int argc, char **argv)
{
    (void)argc;
    char path[4096];
    snprintf(path, sizeof path, "%s.plugin.so", argv[0]);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("FAIL: dlopen %s: %s\n", path, dlerror()); return 1; }
    p_alloc = (void *(*)(size_t))dlsym(h, "plugin_alloc");
    p_free = (void (*)(void *))dlsym(h, "plugin_free");
    if (!p_alloc || !p_free) { printf("FAIL: plugin symbols missing\n"); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_crash;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) != 0) { printf("FAIL: pthread_create\n"); return 1; }
    pthread_mutex_lock(&mu);
    while (!allocated) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);

    p_alloc = NULL;
    p_free = NULL;
    dlclose(h);
    int unloaded = dlopen(path, RTLD_NOW | RTLD_NOLOAD) == NULL;

    pthread_mutex_lock(&mu);
    may_exit = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    pthread_join(t, NULL);

    if (!unloaded) { printf("SKIP: the module stayed loaded after dlclose\n"); return 77; }
    printf("module unloaded; the thread that had allocated through it exited cleanly\n");
    printf("PASS\n");
    return 0;
}

#endif
