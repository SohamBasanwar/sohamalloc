/* The C++ operators as a separate member of the static override archive
 * (see cxx_ops.inc).  Strong references to libstdc++'s helpers: this
 * object is linked only into programs that call operator new, which
 * link a C++ runtime anyway. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include "../include/sohamalloc.h"

#define likely(x) __builtin_expect(!!(x), 1)
#define SOHAM_NOINLINE __attribute__((noinline))
#define SOHAM_INLINE static inline __attribute__((always_inline))
#define SOHAM_HOT __attribute__((aligned(64)))
#define SOHAM_API __attribute__((visibility("default")))

#define SOHAM_CXX_RT_REF
#define CXX_MALLOC_FAST(n) soham_malloc(n)
#define CXX_MALLOC_SLOW(n) soham_malloc(n)
static void cxx_fatal(const char *msg) {
  static const char pre[] = "sohamalloc: ";
  if (write(2, pre, sizeof pre - 1) < 0 || write(2, msg, strlen(msg)) < 0 || write(2, "\n", 1) < 0) {}
  abort();
}
#define CXX_FATAL(msg, n) ((void)(n), cxx_fatal(msg))

#include "cxx_ops.inc"
