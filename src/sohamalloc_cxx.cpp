/*
 * src/sohamalloc_cxx.cpp -- global C++ operator new/delete on top of sohamalloc.
 * Compiled into the override libraries only (docs/IMPL_SPEC_v0.md §2).
 *
 * new: on failure call std::get_new_handler() and retry; throw
 * std::bad_alloc when there is no handler.  nothrow forms return nullptr.
 */
#include <cstddef>
#include <new>

#include "../include/sohamalloc.h"

#define SOHAM_CXX_API __attribute__((visibility("default")))

namespace {

void *alloc_or_handle(std::size_t n) {
  for (;;) {
    void *p = soham_malloc(n);
    if (p) return p;
    std::new_handler h = std::get_new_handler();
    if (!h) throw std::bad_alloc();
    h();
  }
}

void *alloc_aligned_or_handle(std::size_t n, std::size_t al) {
  for (;;) {
    void *p = soham_aligned_alloc(al, n ? n : 1);
    if (p) return p;
    std::new_handler h = std::get_new_handler();
    if (!h) throw std::bad_alloc();
    h();
  }
}

void *alloc_nothrow(std::size_t n) noexcept {
  try { return alloc_or_handle(n); } catch (...) { return nullptr; }
}

void *alloc_aligned_nothrow(std::size_t n, std::size_t al) noexcept {
  try { return alloc_aligned_or_handle(n, al); } catch (...) { return nullptr; }
}

}  // namespace

SOHAM_CXX_API void *operator new(std::size_t n) { return alloc_or_handle(n); }
SOHAM_CXX_API void *operator new[](std::size_t n) { return alloc_or_handle(n); }
SOHAM_CXX_API void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return alloc_nothrow(n); }
SOHAM_CXX_API void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return alloc_nothrow(n); }

SOHAM_CXX_API void operator delete(void *p) noexcept { soham_free(p); }
SOHAM_CXX_API void operator delete[](void *p) noexcept { soham_free(p); }
SOHAM_CXX_API void operator delete(void *p, const std::nothrow_t &) noexcept { soham_free(p); }
SOHAM_CXX_API void operator delete[](void *p, const std::nothrow_t &) noexcept { soham_free(p); }
SOHAM_CXX_API void operator delete(void *p, std::size_t n) noexcept { soham_free_sized(p, n); }
SOHAM_CXX_API void operator delete[](void *p, std::size_t n) noexcept { soham_free_sized(p, n); }

#if defined(__cpp_aligned_new)
SOHAM_CXX_API void *operator new(std::size_t n, std::align_val_t al) { return alloc_aligned_or_handle(n, static_cast<std::size_t>(al)); }
SOHAM_CXX_API void *operator new[](std::size_t n, std::align_val_t al) { return alloc_aligned_or_handle(n, static_cast<std::size_t>(al)); }
SOHAM_CXX_API void *operator new(std::size_t n, std::align_val_t al, const std::nothrow_t &) noexcept {
  return alloc_aligned_nothrow(n, static_cast<std::size_t>(al));
}
SOHAM_CXX_API void *operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t &) noexcept {
  return alloc_aligned_nothrow(n, static_cast<std::size_t>(al));
}
SOHAM_CXX_API void operator delete(void *p, std::align_val_t) noexcept { soham_free(p); }
SOHAM_CXX_API void operator delete[](void *p, std::align_val_t) noexcept { soham_free(p); }
SOHAM_CXX_API void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { soham_free(p); }
SOHAM_CXX_API void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { soham_free(p); }
SOHAM_CXX_API void operator delete(void *p, std::size_t n, std::align_val_t al) noexcept {
  soham_free_aligned_sized(p, static_cast<std::size_t>(al), n);
}
SOHAM_CXX_API void operator delete[](void *p, std::size_t n, std::align_val_t al) noexcept {
  soham_free_aligned_sized(p, static_cast<std::size_t>(al), n);
}
#endif
