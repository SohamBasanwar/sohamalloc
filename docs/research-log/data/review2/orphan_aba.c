/* Remote double free A,B,A on a page orphaned by an exited thread.
 * The remote count reaches cap, resolve() releases the page while block L
 * is still live; nothing walks the chain, so no abort.  Then show that a
 * later allocation overlaps the live block L. */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#define SZ 3000   /* class 3072: 21 blocks per 64 KiB page, unlikely used by libc */

static void *(*volatile my_malloc)(size_t) = malloc;
static void (*volatile my_free)(void *) = free;

static char *A, *B, *L;

static void *t1(void *arg) {
    (void)arg;
    A = my_malloc(SZ); B = my_malloc(SZ); L = my_malloc(SZ);
    strcpy(L, "L is live and must keep this text");
    return NULL;   /* thread exit: page detached (ORPHAN), then QUEUED */
}

int main(void) {
    pthread_t t;
    pthread_create(&t, NULL, t1, NULL);
    pthread_join(t, NULL);
    printf("A=%p B=%p L=%p usable(L)=%zu\n", (void *)A, (void *)B, (void *)L, malloc_usable_size(L));
    my_free(A);
    my_free(B);
    my_free(A);            /* double free: A,B,A pattern, remote path */
    printf("double free A,B,A accepted; usable(L)=%zu (0 = page released)\n", malloc_usable_size(L));
    /* Reuse the released slice: each new size class needs a fresh page. */
    size_t sizes[] = { 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        for (int k = 0; k < 65536 / (int)sizes[i] + 1; k++) {
            char *p = my_malloc(sizes[i]);
            if ((uintptr_t)p >= (uintptr_t)L && (uintptr_t)p < (uintptr_t)L + SZ) {
                memset(p, 'X', sizes[i]);
                printf("malloc(%zu) = %p overlaps live L=%p; L now reads \"%.20s\"\n", sizes[i], (void *)p, (void *)L, L);
                return 1;
            }
        }
    }
    printf("no overlap found\n");
    return 0;
}
