/* Global C++ operator new/delete, written in C under their mangled names.
 * Included in two ways:
 *  - by api.c, for the shared library: the malloc fast path is inlined,
 *    and the C++ runtime's helpers are weak references with a dlsym
 *    fallback (a C program may never load a C++ runtime);
 *  - by cxx_ops.c, the static archive's separate member: the linker pulls it
 *    only into programs that use operator new, and its strong references
 *    then pull the runtime's helpers too (-static-libstdc++, -static).
 * The includer defines SOHAM_CXX_RT_REF, CXX_MALLOC_FAST, CXX_MALLOC_SLOW
 * and CXX_FATAL. */
#include <dlfcn.h>

#if SIZE_MAX > 0xffffffffu
#  define SOHAM_SZ "m"     /* size_t is unsigned long */
#else
#  define SOHAM_SZ "j"     /* size_t is unsigned int */
#endif

typedef void (*soham_new_handler_t)(void);
extern soham_new_handler_t _ZSt15get_new_handlerv(void) SOHAM_CXX_RT_REF;
extern void _ZSt17__throw_bad_allocv(void) SOHAM_CXX_RT_REF __attribute__((noreturn));

/* The C++ runtime's helpers.  The weak references bind only when the
 * runtime is present when this library is relocated; when it arrives later
 * (a C program dlopens a C++ plugin) or is linked statically
 * (-static-libstdc++), look it up at the first allocation failure.
 * libc++ spells __throw_bad_alloc differently. */
typedef soham_new_handler_t (*get_nh_fn)(void);
typedef void (*throw_ba_fn)(void);
static _Atomic(void *) g_cxx_get_nh, g_cxx_throw_ba;
static void *cxx_sym(_Atomic(void *) *slot, void *weak, const char *n1, const char *n2) {
  if (weak) return weak;
  void *f = atomic_load_explicit(slot, memory_order_acquire);
  if (!f) {
    f = dlsym(RTLD_DEFAULT, n1);
    if (!f && n2) f = dlsym(RTLD_DEFAULT, n2);
    /* A runtime loaded as a dependency of a dlopen()ed library is in that
     * library's local scope: find it by name, without loading anything. */
    static const char *const libs[] = { "libstdc++.so.6", "libc++.so.1" };
    for (unsigned k = 0; !f && k < 2; k++) {
      void *h = dlopen(libs[k], RTLD_LAZY | RTLD_NOLOAD);
      if (!h) continue;
      f = dlsym(h, n1);
      if (!f && n2) f = dlsym(h, n2);
      dlclose(h);   /* drops only the reference RTLD_NOLOAD added */
    }
    if (f) atomic_store_explicit(slot, f, memory_order_release);
  }
  return f;
}
static soham_new_handler_t cxx_get_new_handler(void) {
  get_nh_fn f = (get_nh_fn)cxx_sym(&g_cxx_get_nh, (void *)_ZSt15get_new_handlerv,
                                   "_ZSt15get_new_handlerv", "_ZNSt3__115get_new_handlerEv");
  return f ? f() : NULL;
}
static throw_ba_fn cxx_throw_bad_alloc(void) {
  return (throw_ba_fn)cxx_sym(&g_cxx_throw_ba, (void *)_ZSt17__throw_bad_allocv,
                              "_ZSt17__throw_bad_allocv", "_ZNSt3__117__throw_bad_allocEv");
}

static SOHAM_NOINLINE void *cxx_new_retry(size_t n, size_t align) {
  for (;;) {
    void *p = align ? soham_aligned_alloc(align, n ? n : 1) : soham_malloc(n);
    if (p) return p;
    soham_new_handler_t h = cxx_get_new_handler();
    if (!h) {
      throw_ba_fn tb = cxx_throw_bad_alloc();
      if (tb) tb();
      CXX_FATAL("out of memory in operator new (no C++ runtime to throw std::bad_alloc)", n);
    }
    h();   /* may throw: the C frames have unwind tables */
  }
}

/* nothrow new must return NULL if the new_handler throws, which needs a
   catch that C cannot write. The C++ runtime's own nothrow new is exactly
   "try { return ::operator new(n); } catch (...) { return nullptr; }", and
   its ::operator new resolves to ours, so on failure with a handler
   installed we delegate to it (found once with RTLD_NEXT). Without a
   handler, or without a runtime definition, plain NULL is correct. */
typedef void *(*nt_new_fn)(size_t, const void *);
typedef void *(*nt_new_al_fn)(size_t, size_t, const void *);
static _Atomic(void *) g_nt_next[4];
static const char *const g_nt_names[4] = {
  "_Znw" SOHAM_SZ "RKSt9nothrow_t", "_Zna" SOHAM_SZ "RKSt9nothrow_t",
  "_Znw" SOHAM_SZ "St11align_val_tRKSt9nothrow_t", "_Zna" SOHAM_SZ "St11align_val_tRKSt9nothrow_t" };

static SOHAM_NOINLINE void *cxx_new_nothrow_slow(int which, size_t n, size_t al, const void *nt) {
  if (!cxx_get_new_handler()) return NULL;
  void *f = atomic_load_explicit(&g_nt_next[which], memory_order_acquire);
  if (!f) {
    f = dlsym(RTLD_NEXT, g_nt_names[which]);
    if (!f) return NULL;
    atomic_store_explicit(&g_nt_next[which], f, memory_order_release);
  }
  return which < 2 ? ((nt_new_fn)f)(n, nt) : ((nt_new_al_fn)f)(n, al, nt);
}

static SOHAM_NOINLINE void *cxx_new_slow(size_t n) {
  void *p = CXX_MALLOC_SLOW(n);
  return likely(p != NULL) ? p : cxx_new_retry(n, 0);
}

/* The malloc fast path inlined, so new costs no more than malloc. */
SOHAM_INLINE void *cxx_new(size_t n) {
  void *p = CXX_MALLOC_FAST(n);
  return likely(p != NULL) ? p : cxx_new_slow(n);
}
SOHAM_INLINE void *cxx_new_aligned(size_t n, size_t al) {
  void *p = soham_aligned_alloc(al, n ? n : 1);
  return likely(p != NULL) ? p : cxx_new_retry(n, al);
}

#define SOHAM_CXX(ret, name, mangled, params) \
  SOHAM_API ret name params __asm__(mangled); SOHAM_API ret name params

SOHAM_CXX(SOHAM_HOT void *, soham_op_new, "_Znw" SOHAM_SZ, (size_t n)) { return cxx_new(n); }
SOHAM_CXX(SOHAM_HOT void *, soham_op_new_a, "_Zna" SOHAM_SZ, (size_t n)) { return cxx_new(n); }
SOHAM_CXX(void *, soham_op_new_nt, "_Znw" SOHAM_SZ "RKSt9nothrow_t", (size_t n, const void *nt)) {
  void *p = soham_malloc(n); return p ? p : cxx_new_nothrow_slow(0, n, 0, nt);
}
SOHAM_CXX(void *, soham_op_new_a_nt, "_Zna" SOHAM_SZ "RKSt9nothrow_t", (size_t n, const void *nt)) {
  void *p = soham_malloc(n); return p ? p : cxx_new_nothrow_slow(1, n, 0, nt);
}
SOHAM_CXX(void *, soham_op_new_al, "_Znw" SOHAM_SZ "St11align_val_t", (size_t n, size_t al)) { return cxx_new_aligned(n, al); }
SOHAM_CXX(void *, soham_op_new_a_al, "_Zna" SOHAM_SZ "St11align_val_t", (size_t n, size_t al)) { return cxx_new_aligned(n, al); }
SOHAM_CXX(void *, soham_op_new_al_nt, "_Znw" SOHAM_SZ "St11align_val_tRKSt9nothrow_t", (size_t n, size_t al, const void *nt)) {
  void *p = soham_aligned_alloc(al, n ? n : 1); return p ? p : cxx_new_nothrow_slow(2, n, al, nt);
}
SOHAM_CXX(void *, soham_op_new_a_al_nt, "_Zna" SOHAM_SZ "St11align_val_tRKSt9nothrow_t", (size_t n, size_t al, const void *nt)) {
  void *p = soham_aligned_alloc(al, n ? n : 1); return p ? p : cxx_new_nothrow_slow(3, n, al, nt);
}

SOHAM_CXX(void, soham_op_del, "_ZdlPv", (void *p)) { soham_free(p); }
SOHAM_CXX(void, soham_op_del_a, "_ZdaPv", (void *p)) { soham_free(p); }
SOHAM_CXX(void, soham_op_del_nt, "_ZdlPvRKSt9nothrow_t", (void *p, const void *nt)) { (void)nt; soham_free(p); }
SOHAM_CXX(void, soham_op_del_a_nt, "_ZdaPvRKSt9nothrow_t", (void *p, const void *nt)) { (void)nt; soham_free(p); }
SOHAM_CXX(void, soham_op_del_sz, "_ZdlPv" SOHAM_SZ, (void *p, size_t n)) { soham_free_sized(p, n); }
SOHAM_CXX(void, soham_op_del_a_sz, "_ZdaPv" SOHAM_SZ, (void *p, size_t n)) { soham_free_sized(p, n); }
SOHAM_CXX(void, soham_op_del_al, "_ZdlPvSt11align_val_t", (void *p, size_t al)) { (void)al; soham_free(p); }
SOHAM_CXX(void, soham_op_del_a_al, "_ZdaPvSt11align_val_t", (void *p, size_t al)) { (void)al; soham_free(p); }
SOHAM_CXX(void, soham_op_del_al_nt, "_ZdlPvSt11align_val_tRKSt9nothrow_t", (void *p, size_t al, const void *nt)) { (void)al; (void)nt; soham_free(p); }
SOHAM_CXX(void, soham_op_del_a_al_nt, "_ZdaPvSt11align_val_tRKSt9nothrow_t", (void *p, size_t al, const void *nt)) { (void)al; (void)nt; soham_free(p); }
SOHAM_CXX(void, soham_op_del_sz_al, "_ZdlPv" SOHAM_SZ "St11align_val_t", (void *p, size_t n, size_t al)) { soham_free_aligned_sized(p, al, n); }
SOHAM_CXX(void, soham_op_del_a_sz_al, "_ZdaPv" SOHAM_SZ "St11align_val_t", (void *p, size_t n, size_t al)) { soham_free_aligned_sized(p, al, n); }

