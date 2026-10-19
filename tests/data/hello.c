/* tests/data/hello.c -- smallest compile-and-run check for preload_smoke.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *s = malloc(64);
    if (!s) return 1;
    strcpy(s, "hello, world");
    printf("%s (%zu)\n", s, strlen(s));
    free(s);
    return 0;
}
