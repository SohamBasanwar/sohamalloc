/*
 * src/os_posix.c -- POSIX implementation of the sohamalloc OS layer (os.h).
 *
 * Linux is the primary target; macOS paths (behind __APPLE__) and generic
 * Unix fallbacks are compile-reviewed only (docs/IMPL_SPEC_v0.md §8).
 *
 * Every function preserves errno (os.h contract).  Nothing here allocates:
 * environment scanning walks environ directly, files are read with raw
 * open/read/close into caller buffers.
 */
#if !defined(_WIN32)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "os.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/auxv.h>
#  include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#  include <crt_externs.h>
#endif

#ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#  define MAP_NORESERVE 0
#endif

#if defined(__linux__)
/* Values from the kernel uapi, defined here so the file builds against libc
 * headers that lack them (musl-gcc does not see <linux/...> headers). */
#  ifndef MAP_FIXED_NOREPLACE
#    if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || \
        defined(__arm__) || defined(__riscv) || defined(__powerpc__) || defined(__s390x__)
#      define MAP_FIXED_NOREPLACE 0x100000
#    endif
#  endif
#  ifndef SYS_process_madvise
#    define SYS_process_madvise 440  /* unified syscall number on every architecture */
#  endif
#  define SOHAM_PIDFD_SELF (-10000)  /* PIDFD_SELF, Linux 6.15 uapi */
#  define SOHAM_FUTEX_WAIT_PRIVATE (0 | 128)
#  define SOHAM_FUTEX_WAKE_PRIVATE (1 | 128)
#  if defined(SYS_futex)
#    define SOHAM_SYS_FUTEX SYS_futex
#  elif defined(SYS_futex_time64)
#    define SOHAM_SYS_FUTEX SYS_futex_time64
#  endif
#endif

extern char **environ;

/* ---------------------------------------------------------------------- */
/* System facts                                                            */
/* ---------------------------------------------------------------------- */

size_t soham_os_page_size(void) {
  int e = errno;
  size_t b = 0;
#if defined(__linux__)
  b = (size_t)getauxval(AT_PAGESZ);
#endif
  if (b == 0) {
    long v = sysconf(_SC_PAGESIZE);
    b = v > 0 ? (size_t)v : 4096u; /* last-resort default when the OS gives nothing */
  }
  errno = e;
  return b;
}

int soham_os_is_secure(void) {
  int e = errno, s;
#if defined(__linux__)
  s = getauxval(AT_SECURE) != 0;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  s = issetugid() != 0;
#else
  s = getuid() != geteuid() || getgid() != getegid();
#endif
  errno = e;
  return s;
}

size_t soham_os_as_limit(void) {
  int e = errno;
  size_t lim = SIZE_MAX;
  struct rlimit rl;
  if (getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
    lim = (rl.rlim_cur > (rlim_t)SIZE_MAX) ? SIZE_MAX : (size_t)rl.rlim_cur;
  }
  errno = e;
  return lim;
}

static uint64_t os_mix64(uint64_t z) {
  z += 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

void soham_os_random(void *buf, size_t n) {
  int e = errno;
  unsigned char *out = (unsigned char *)buf;
  size_t got = 0;
#if defined(__linux__) && defined(SYS_getrandom)
  while (got < n) {
    long r = syscall(SYS_getrandom, out + got, n - got, 1 /* GRND_NONBLOCK */);
    if (r > 0) { got += (size_t)r; continue; }
    if (r < 0 && errno == EINTR) continue;
    break;
  }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  arc4random_buf(out, n);
  got = n;
#endif
  if (got < n) {
    /* Fallback: AT_RANDOM (16 kernel-provided bytes) mixed with a clock and
     * addresses.  Weaker, but never fails and never allocates. */
    uint64_t seed = 0x5eed5eed5eedull;
#if defined(__linux__)
    const uint64_t *ar = (const uint64_t *)(uintptr_t)getauxval(AT_RANDOM);
    if (ar) { uint64_t a0, a1; memcpy(&a0, ar, 8); memcpy(&a1, (const char *)ar + 8, 8); seed ^= a0 ^ os_mix64(a1); }
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    seed ^= os_mix64((uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32));
    seed ^= os_mix64((uint64_t)(uintptr_t)&ts ^ (uint64_t)(uintptr_t)buf);
    while (got < n) {
      seed = os_mix64(seed);
      size_t k = n - got < 8 ? n - got : 8;
      memcpy(out + got, &seed, k);
      got += k;
    }
  }
  errno = e;
}

uint64_t soham_os_now_ns(void) {
  int e = errno;
  struct timespec ts;
#if defined(CLOCK_MONOTONIC_COARSE)
  if (clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0)
#endif
    clock_gettime(CLOCK_MONOTONIC, &ts);
  errno = e;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int soham_os_thp_mode(void) {
#if defined(__linux__)
  char buf[128];
  long n = soham_os_read_file("/sys/kernel/mm/transparent_hugepage/enabled", buf, sizeof buf);
  if (n <= 0) return -1;
  const char *sel = strchr(buf, '[');
  if (!sel) return -1;
  if (strncmp(sel, "[always]", 8) == 0) return 2;
  if (strncmp(sel, "[madvise]", 9) == 0) return 1;
  if (strncmp(sel, "[never]", 7) == 0) return 0;
  return -1;
#else
  return -1;
#endif
}

/* ---------------------------------------------------------------------- */
/* Address space                                                           */
/* ---------------------------------------------------------------------- */

void *soham_os_reserve_aligned(size_t size, size_t align, uintptr_t hint) {
  int e = errno;
  const int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
  void *res = NULL;
#if defined(__linux__) && defined(MAP_FIXED_NOREPLACE)
  if (hint != 0 && (hint & (align - 1)) == 0) {
    void *p = mmap((void *)hint, size, PROT_NONE, flags | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED) {
      /* Kernels before 4.17 treat the flag as a hint and may place the
       * mapping elsewhere: accept it only if it is aligned. */
      if (((uintptr_t)p & (align - 1)) == 0) { res = p; goto out; }
      munmap(p, size);
    }
  }
#else
  (void)hint;
#endif
  if (size + align < size) goto out;
  {
    size_t total = size + align;
    void *p = mmap(NULL, total, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) goto out;
    uintptr_t lo = (uintptr_t)p;
    uintptr_t a = (lo + align - 1) & ~(uintptr_t)(align - 1);
    if (a > lo) munmap(p, a - lo);
    uintptr_t end = lo + total;
    if (end > a + size) munmap((void *)(a + size), end - (a + size));
    res = (void *)a;
  }
out:
  errno = e;
  return res;
}

int soham_os_commit(void *p, size_t n) {
  int e = errno;
  int r = mprotect(p, n, PROT_READ | PROT_WRITE);
  errno = e;
  return r == 0 ? 0 : -1;
}

void soham_os_unmap(void *p, size_t n) {
  int e = errno;
  if (p && n) munmap(p, n);
  errno = e;
}

void *soham_os_map_rw(size_t n, size_t align) {
  int e = errno;
  void *res = NULL;
  size_t b = soham_os_page_size();
  const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (align <= b) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, flags, -1, 0);
    res = p == MAP_FAILED ? NULL : p;
  } else if (n + align > n) {
    size_t total = n + align;
    void *p = mmap(NULL, total, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p != MAP_FAILED) {
      uintptr_t lo = (uintptr_t)p;
      uintptr_t a = (lo + align - 1) & ~(uintptr_t)(align - 1);
      if (a > lo) munmap(p, a - lo);
      uintptr_t end = lo + total;
      if (end > a + n) munmap((void *)(a + n), end - (a + n));
      res = (void *)a;
    }
  }
  errno = e;
  return res;
}

void *soham_os_remap(void *p, size_t oldn, size_t newn) {
#if defined(__linux__) && defined(MREMAP_MAYMOVE)
  int e = errno;
  void *q = mremap(p, oldn, newn, MREMAP_MAYMOVE);
  errno = e;
  return q == MAP_FAILED ? NULL : q;
#else
  (void)p; (void)oldn; (void)newn;
  return NULL;
#endif
}

void soham_os_advise_nohuge(void *p, size_t n) {
#if defined(MADV_NOHUGEPAGE)
  int e = errno;
  madvise(p, n, MADV_NOHUGEPAGE);
  errno = e;
#else
  (void)p; (void)n;
#endif
}

void soham_os_advise_huge(void *p, size_t n) {
#if defined(MADV_HUGEPAGE)
  int e = errno;
  madvise(p, n, MADV_HUGEPAGE);
  errno = e;
#else
  (void)p; (void)n;
#endif
}

/* ---------------------------------------------------------------------- */
/* Release                                                                 */
/* ---------------------------------------------------------------------- */

/* process_madvise probe latch: 0 unknown, 1 works, 2 unsupported.
 * Relaxed is enough: it is a monotone hint; a racing thread that reads a
 * stale 0 merely probes again (docs/IMPL_SPEC_v0.md §6). */
static _Atomic int os_pm_state;

_Static_assert(sizeof(soham_os_range_t) == sizeof(struct iovec), "range must match iovec");
_Static_assert(offsetof(soham_os_range_t, len) == offsetof(struct iovec, iov_len), "range must match iovec");

static int os_madvise_loop(soham_os_range_t *r, size_t n, int adv) {
  int ok = 1;
  for (size_t i = 0; i < n; i++) {
    if (r[i].len == 0) continue;
    if (madvise(r[i].base, r[i].len, adv) != 0) ok = 0;
  }
  return ok;
}

int soham_os_release(soham_os_range_t *ranges, size_t n, int mode) {
  int e = errno;
  int adv = MADV_DONTNEED;
#if defined(__APPLE__) && defined(MADV_FREE_REUSABLE)
  adv = MADV_FREE_REUSABLE;   /* Darwin: the accounting-friendly lazy release */
  (void)mode;
#elif defined(MADV_FREE)
  if (mode == SOHAM_OS_RELEASE_FREE) adv = MADV_FREE;
#else
  (void)mode;
#endif
  int zero = (adv == MADV_DONTNEED);
#if defined(__linux__)
  size_t i = 0;
  if (atomic_load_explicit(&os_pm_state, memory_order_relaxed) != 2) {
    while (i < n) {
      size_t cnt = n - i < 1024 ? n - i : 1024;   /* UIO_MAXIOV */
      size_t want = 0;
      for (size_t k = 0; k < cnt; k++) want += ranges[i + k].len;
      long r = syscall(SYS_process_madvise, SOHAM_PIDFD_SELF, (struct iovec *)(ranges + i), cnt, adv, 0u);
      if (r < 0) {
        int err = errno;
        if (atomic_load_explicit(&os_pm_state, memory_order_relaxed) == 0 &&
            (err == EPERM || err == EINVAL || err == ENOSYS || err == EBADF)) {
          atomic_store_explicit(&os_pm_state, 2, memory_order_relaxed);
        }
        break;   /* finish this batch with madvise */
      }
      atomic_store_explicit(&os_pm_state, 1, memory_order_relaxed);
      if ((size_t)r >= want) { i += cnt; continue; }
      /* Partial: skip the bytes done and resume (the kernel stopped inside
       * a range; finish that range and the rest with madvise). */
      size_t done = (size_t)r;
      while (done > 0 && i < n && ranges[i].len <= done) { done -= ranges[i].len; i++; }
      if (i < n && done > 0) {
        char *b = (char *)ranges[i].base + done;
        if (madvise(b, ranges[i].len - done, adv) != 0) zero = 0;
        i++;
      }
    }
  }
  if (i < n && !os_madvise_loop(ranges + i, n - i, adv)) zero = 0;
#else
  if (!os_madvise_loop(ranges, n, adv)) zero = 0;
#endif
  errno = e;
  return zero;
}

const char *soham_os_release_method(void) {
#if defined(__linux__)
  switch (atomic_load_explicit(&os_pm_state, memory_order_relaxed)) {
    case 1: return "process_madvise(PIDFD_SELF)";
    case 2: return "madvise loop (process_madvise unsupported)";
    default: return "process_madvise(PIDFD_SELF) (not yet probed)";
  }
#else
  return "madvise loop";
#endif
}

/* ---------------------------------------------------------------------- */
/* Synchronisation                                                         */
/* ---------------------------------------------------------------------- */

void soham_os_wait(_Atomic uint32_t *addr, uint32_t val) {
  int e = errno;
#if defined(__linux__) && defined(SOHAM_SYS_FUTEX)
  syscall(SOHAM_SYS_FUTEX, (void *)addr, SOHAM_FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
#else
  if (atomic_load_explicit(addr, memory_order_relaxed) == val) sched_yield();
#endif
  errno = e;
}

void soham_os_wake(_Atomic uint32_t *addr) {
#if defined(__linux__) && defined(SOHAM_SYS_FUTEX)
  int e = errno;
  syscall(SOHAM_SYS_FUTEX, (void *)addr, SOHAM_FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
  errno = e;
#else
  (void)addr;
#endif
}

void soham_os_yield(void) {
  int e = errno;
  sched_yield();
  errno = e;
}

/* ---------------------------------------------------------------------- */
/* Threads and processes                                                   */
/* ---------------------------------------------------------------------- */

static pthread_key_t os_thread_key;

int soham_os_thread_key_init(void (*dtor)(void *)) {
  int e = errno;
  int r = pthread_key_create(&os_thread_key, dtor);
  errno = e;
  return r == 0 ? 0 : -1;
}

void soham_os_thread_set(void *value) {
  int e = errno;
  pthread_setspecific(os_thread_key, value);
  errno = e;
}

int soham_os_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
  int e = errno;
  int r = pthread_atfork(prepare, parent, child);
  errno = e;
  return r == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------------- */
/* Diagnostics I/O                                                         */
/* ---------------------------------------------------------------------- */

void soham_os_write(int fd, const char *buf, size_t n) {
  int e = errno;
  while (n > 0) {
    ssize_t w = write(fd, buf, n);
    if (w < 0) { if (errno == EINTR) continue; break; }
    if (w == 0) break;
    buf += w;
    n -= (size_t)w;
  }
  errno = e;
}

long soham_os_read_file(const char *path, char *buf, size_t cap) {
  int e = errno;
  long total = -1;
  if (cap == 0) goto out;
#ifdef O_CLOEXEC
  int fd = open(path, O_RDONLY | O_CLOEXEC);
#else
  int fd = open(path, O_RDONLY);
#endif
  if (fd < 0) goto out;
  total = 0;
  while ((size_t)total < cap - 1) {
    ssize_t r = read(fd, buf + total, cap - 1 - (size_t)total);
    if (r < 0) { if (errno == EINTR) continue; break; }
    if (r == 0) break;
    total += r;
  }
  buf[total] = '\0';
  close(fd);
out:
  errno = e;
  return total;
}

long soham_os_getenv(const char *name, char *buf, size_t cap) {
#if defined(__APPLE__)
  char **env = *_NSGetEnviron();
#else
  char **env = environ;
#endif
  if (!env || !name) return -1;
  size_t nl = strlen(name);
  for (char **e = env; *e; e++) {
    const char *s = *e;
    if (strncmp(s, name, nl) == 0 && s[nl] == '=') {
      const char *v = s + nl + 1;
      size_t vl = strlen(v);
      if (cap) {
        size_t k = vl < cap - 1 ? vl : cap - 1;
        memcpy(buf, v, k);
        buf[k] = '\0';
      }
      return (long)vl;
    }
  }
  return -1;
}

void soham_os_abort(void) {
  abort();
}

#endif /* !_WIN32 */
