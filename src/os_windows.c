/*
 * src/os_windows.c -- Windows implementation of the sohamalloc OS layer (os.h).
 *
 * Windows builds only the prefixed library (no malloc override), so this
 * layer serves soham_* callers.  The window/prefix model maps directly onto
 * VirtualAlloc: a window is a MEM_RESERVE region, prefix growth is
 * MEM_COMMIT.  Release decommits and recommits (pages read back as zero).
 *
 * Every function preserves errno and GetLastError (os.h contract).
 */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <bcrypt.h>
#include <errno.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

#include "os.h"

#define OS_SAVE   int os_e_ = errno; DWORD os_le_ = GetLastError()
#define OS_RESTORE do { SetLastError(os_le_); errno = os_e_; } while (0)

size_t soham_os_page_size(void) {
  OS_SAVE;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  OS_RESTORE;
  return si.dwPageSize ? (size_t)si.dwPageSize : 4096u;
}

int soham_os_is_secure(void) { return 0; }

size_t soham_os_as_limit(void) { return SIZE_MAX; }

static uint64_t os_mix64(uint64_t z) {
  z += 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

void soham_os_random(void *buf, size_t n) {
  OS_SAVE;
  if (n > 0xffffffffu ||
      BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t seed = os_mix64((uint64_t)q.QuadPart ^ (uint64_t)(uintptr_t)buf ^ GetCurrentProcessId());
    unsigned char *out = (unsigned char *)buf;
    for (size_t got = 0; got < n;) {
      seed = os_mix64(seed);
      size_t k = n - got < 8 ? n - got : 8;
      memcpy(out + got, &seed, k);
      got += k;
    }
  }
  OS_RESTORE;
}

uint64_t soham_os_now_ns(void) {
  OS_SAVE;
  uint64_t t = (uint64_t)GetTickCount64() * 1000000ull;
  OS_RESTORE;
  return t;
}

int soham_os_thp_mode(void) { return -1; }

void *soham_os_reserve_aligned(size_t size, size_t align, uintptr_t hint) {
  OS_SAVE;
  void *res = NULL;
  if (hint != 0 && (hint & (align - 1)) == 0) {
    res = VirtualAlloc((void *)hint, size, MEM_RESERVE, PAGE_NOACCESS);
    if (res && ((uintptr_t)res & (align - 1)) != 0) { VirtualFree(res, 0, MEM_RELEASE); res = NULL; }
  }
  /* Over-reserve, find the aligned address, release and re-reserve exactly
   * there.  Another thread may grab the range in between: retry. */
  for (int tries = 0; !res && tries < 16 && size + align > size; tries++) {
    void *p = VirtualAlloc(NULL, size + align, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) break;
    uintptr_t a = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
    VirtualFree(p, 0, MEM_RELEASE);
    res = VirtualAlloc((void *)a, size, MEM_RESERVE, PAGE_NOACCESS);
  }
  OS_RESTORE;
  return res;
}

int soham_os_commit(void *p, size_t n) {
  OS_SAVE;
  void *r = VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE);
  OS_RESTORE;
  return r ? 0 : -1;
}

void soham_os_unmap(void *p, size_t n) {
  (void)n;  /* Windows releases whole reservations; callers pass the base. */
  OS_SAVE;
  if (p) VirtualFree(p, 0, MEM_RELEASE);
  OS_RESTORE;
}

void *soham_os_map_rw(size_t n, size_t align) {
  OS_SAVE;
  void *res;
  if (align <= 65536) {
    res = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  } else {
    res = soham_os_reserve_aligned(n, align, 0);
    if (res && !VirtualAlloc(res, n, MEM_COMMIT, PAGE_READWRITE)) {
      VirtualFree(res, 0, MEM_RELEASE);
      res = NULL;
    }
  }
  OS_RESTORE;
  return res;
}

void *soham_os_remap(void *p, size_t oldn, size_t newn) {
  (void)p; (void)oldn; (void)newn;
  return NULL;
}

void soham_os_advise_nohuge(void *p, size_t n) { (void)p; (void)n; }
void soham_os_advise_huge(void *p, size_t n) { (void)p; (void)n; }

int soham_os_release(soham_os_range_t *ranges, size_t n, int mode) {
  OS_SAVE;
  int zero = 1;
  for (size_t i = 0; i < n; i++) {
    if (!ranges[i].len) continue;
    if (mode == SOHAM_OS_RELEASE_FREE) {
      VirtualAlloc(ranges[i].base, ranges[i].len, MEM_RESET, PAGE_READWRITE);
      zero = 0;
    } else if (!VirtualFree(ranges[i].base, ranges[i].len, MEM_DECOMMIT) ||
               !VirtualAlloc(ranges[i].base, ranges[i].len, MEM_COMMIT, PAGE_READWRITE)) {
      /* Recommit failure would leave the range inaccessible; it cannot be
       * reported here, so treat the contents as indeterminate.  A later touch
       * faults, which is the honest outcome of commit exhaustion. */
      zero = 0;
    }
  }
  OS_RESTORE;
  return zero;
}

const char *soham_os_release_method(void) { return "VirtualFree(MEM_DECOMMIT)+recommit"; }

void soham_os_wait(_Atomic uint32_t *addr, uint32_t val) {
  if (atomic_load_explicit(addr, memory_order_relaxed) == val) {
    OS_SAVE;
    SwitchToThread();
    OS_RESTORE;
  }
}

void soham_os_wake(_Atomic uint32_t *addr) { (void)addr; }

void soham_os_yield(void) {
  OS_SAVE;
  SwitchToThread();
  OS_RESTORE;
}

static DWORD os_fls_index = FLS_OUT_OF_INDEXES;
static void (*os_thread_dtor)(void *);

static VOID NTAPI os_fls_callback(PVOID value) {
  if (value && os_thread_dtor) os_thread_dtor(value);
}

int soham_os_thread_key_init(void (*dtor)(void *)) {
  OS_SAVE;
  os_thread_dtor = dtor;
  os_fls_index = FlsAlloc(os_fls_callback);
  OS_RESTORE;
  return os_fls_index == FLS_OUT_OF_INDEXES ? -1 : 0;
}

void soham_os_thread_set(void *value) {
  OS_SAVE;
  if (os_fls_index != FLS_OUT_OF_INDEXES) FlsSetValue(os_fls_index, value);
  OS_RESTORE;
}

int soham_os_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
  (void)prepare; (void)parent; (void)child;
  return 0;  /* no fork on Windows */
}

void soham_os_write(int fd, const char *buf, size_t n) {
  OS_SAVE;
  HANDLE h = fd == 1 ? GetStdHandle(STD_OUTPUT_HANDLE) : fd == 2 ? GetStdHandle(STD_ERROR_HANDLE) : NULL;
  if (h && h != INVALID_HANDLE_VALUE) {
    while (n > 0) {
      DWORD w = 0, chunk = n > 0x10000000u ? 0x10000000u : (DWORD)n;
      if (!WriteFile(h, buf, chunk, &w, NULL) || w == 0) break;
      buf += w;
      n -= w;
    }
  } else {
    while (n > 0) {
      int w = _write(fd, buf, n > 0x10000000u ? 0x10000000u : (unsigned)n);
      if (w <= 0) break;
      buf += w;
      n -= (size_t)w;
    }
  }
  OS_RESTORE;
}

long soham_os_read_file(const char *path, char *buf, size_t cap) {
  OS_SAVE;
  long total = -1;
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (h != INVALID_HANDLE_VALUE && cap > 0) {
    DWORD r = 0;
    total = ReadFile(h, buf, (DWORD)(cap - 1), &r, NULL) ? (long)r : 0;
    buf[total] = '\0';
  }
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  OS_RESTORE;
  return total;
}

long soham_os_getenv(const char *name, char *buf, size_t cap) {
  OS_SAVE;
  char tmp[1];
  DWORD r = GetEnvironmentVariableA(name, cap ? buf : tmp, cap ? (DWORD)cap : 1);
  long res;
  if (r == 0) {
    res = GetLastError() == ERROR_ENVVAR_NOT_FOUND ? -1 : 0;
    if (cap) buf[0] = '\0';
  } else if (r >= cap) {
    /* Too small: r is the required size including the NUL. */
    res = (long)r - 1;
    if (cap) buf[0] = '\0';
  } else {
    res = (long)r;
  }
  OS_RESTORE;
  return res;
}

void soham_os_abort(void) {
  abort();
}

#endif /* _WIN32 */
