/*
 * src/os.h -- the operating-system interface of sohamalloc.
 *
 * Every OS call the allocator makes goes through these functions, which are
 * implemented in src/os_posix.c (Linux, other Unixes, macOS) and
 * src/os_windows.c (Windows).  Rules for implementations
 * (docs/IMPL_SPEC_v0.md §0, §8):
 *
 *  - Never allocate memory through malloc, and never call libc functions
 *    that may (stdio, fopen, opendir, qsort, backtrace, getenv on some libcs).
 *  - Preserve errno: every function saves and restores errno, so callers
 *    can promise "free preserves errno" and successful allocations do not
 *    clobber it.  Failures are reported through return values only.
 *  - Thread-safe and fork-safe without internal state other than latched
 *    probe results (stored in C11 atomics).
 */
#ifndef SOHAM_OS_H
#define SOHAM_OS_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#  define SOHAM_HIDDEN
#else
#  define SOHAM_HIDDEN __attribute__((visibility("hidden")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- System facts ------------------------------------------------------ */

/* Base page size B (getauxval(AT_PAGESZ), else sysconf, else 4096). */
SOHAM_HIDDEN size_t soham_os_page_size(void);
/* Non-zero when running set-uid/set-gid (getauxval(AT_SECURE)). */
SOHAM_HIDDEN int soham_os_is_secure(void);
/* Soft RLIMIT_AS in bytes, or SIZE_MAX when unlimited or unknown. */
SOHAM_HIDDEN size_t soham_os_as_limit(void);
/* Fill buf with n random bytes.  Never fails: falls back to AT_RANDOM mixed
 * with a clock and address entropy when the kernel source is unavailable. */
SOHAM_HIDDEN void soham_os_random(void *buf, size_t n);
/* Monotonic coarse time in nanoseconds (vDSO on Linux: no syscall). */
SOHAM_HIDDEN uint64_t soham_os_now_ns(void);
/* Transparent huge page mode from sysfs: 0 never, 1 madvise, 2 always, -1 unknown. */
SOHAM_HIDDEN int soham_os_thp_mode(void);

/* --- Address space ----------------------------------------------------- */

/* Reserve size bytes of inaccessible address space aligned to align (a power
 * of two >= page size).  hint may be 0 or an align-aligned address to try
 * first.  Returns NULL on failure.  Linux: PROT_NONE + MAP_NORESERVE, with
 * MAP_FIXED_NOREPLACE at the hint, then over-reserve and trim. */
SOHAM_HIDDEN void *soham_os_reserve_aligned(size_t size, size_t align, uintptr_t hint);
/* Make [p, p+n) readable and writable (commit).  0 on success. */
SOHAM_HIDDEN int soham_os_commit(void *p, size_t n);
/* Return [p, p+n) to the OS entirely (unmap / release a reservation). */
SOHAM_HIDDEN void soham_os_unmap(void *p, size_t n);
/* Map n bytes of zeroed read-write memory aligned to align (power of two
 * >= page size), with normal commit accounting (no MAP_NORESERVE).  Returns
 * NULL on failure.  The mapping is exactly [ret, ret+n). */
SOHAM_HIDDEN void *soham_os_map_rw(size_t n, size_t align);
/* Resize a mapping made by soham_os_map_rw, possibly moving it.  Returns the
 * new address, or NULL when unsupported or on failure (old mapping intact). */
SOHAM_HIDDEN void *soham_os_remap(void *p, size_t oldn, size_t newn);
/* Huge-page advice over whole VMAs (fixed areas only; never sub-ranges). */
SOHAM_HIDDEN void soham_os_advise_nohuge(void *p, size_t n);
SOHAM_HIDDEN void soham_os_advise_huge(void *p, size_t n);

/* --- Release (decay) --------------------------------------------------- */

typedef struct soham_os_range {
  void  *base;
  size_t len;
} soham_os_range_t;

#define SOHAM_OS_RELEASE_DONTNEED 0   /* contents become zero on next touch */
#define SOHAM_OS_RELEASE_FREE     1   /* lazy free; contents indeterminate */

/* Release the physical memory behind n ranges, keeping them committed.
 * Uses process_madvise(PIDFD_SELF, ...) when the kernel supports it (probed
 * on the first call and latched), otherwise a madvise loop.  Returns 1 when
 * every range is now known to read as zero (DONTNEED succeeded), 0 when the
 * ranges were released lazily or a call failed (contents indeterminate but
 * valid). */
SOHAM_HIDDEN int soham_os_release(soham_os_range_t *ranges, size_t n, int mode);
/* Human-readable name of the release mechanism currently latched. */
SOHAM_HIDDEN const char *soham_os_release_method(void);

/* --- Synchronisation --------------------------------------------------- */

/* Block while *addr == val (may wake spuriously). */
SOHAM_HIDDEN void soham_os_wait(_Atomic uint32_t *addr, uint32_t val);
/* Wake one waiter blocked on addr. */
SOHAM_HIDDEN void soham_os_wake(_Atomic uint32_t *addr);
SOHAM_HIDDEN void soham_os_yield(void);
/* The process id (a fork child has its own). */
SOHAM_HIDDEN int soham_os_getpid(void);

/* --- Threads and processes --------------------------------------------- */

/* Create the thread-exit hook: dtor(value) runs at thread exit for every
 * thread that called soham_os_thread_set(non-NULL).  0 on success. */
SOHAM_HIDDEN int soham_os_thread_key_init(void (*dtor)(void *));
SOHAM_HIDDEN void soham_os_thread_set(void *value);
/* Delete the key (library unload): the destructor is not called again,
 * so no exiting thread calls into unmapped code. */
SOHAM_HIDDEN void soham_os_thread_key_fini(void);
/* Register fork handlers (no-op where fork does not exist).  0 on success. */
SOHAM_HIDDEN int soham_os_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

/* --- I/O for diagnostics ----------------------------------------------- */

/* write(2) loop on EINTR; best effort. */
SOHAM_HIDDEN void soham_os_write(int fd, const char *buf, size_t n);
/* Read up to cap-1 bytes of a small file with raw open/read/close and
 * NUL-terminate.  Returns the byte count, or -1. */
SOHAM_HIDDEN long soham_os_read_file(const char *path, char *buf, size_t cap);
/* Copy the value of environment variable name into buf (NUL-terminated,
 * truncated to cap-1).  Returns the full value length, or -1 when unset.
 * Never allocates (scans environ directly on POSIX). */
SOHAM_HIDDEN long soham_os_getenv(const char *name, char *buf, size_t cap);
/* Abnormal termination (abort()). */
SOHAM_HIDDEN void soham_os_abort(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* SOHAM_OS_H */
