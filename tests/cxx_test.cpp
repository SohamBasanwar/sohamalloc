/*
 * cxx_test -- the C++ allocation interface (docs/IMPL_SPEC_v0.md §2 "C++
 * operators"), for any allocator under LD_PRELOAD (or linked).
 *
 *   - every global operator new/delete form: plain, nothrow, sized delete,
 *     aligned (std::align_val_t), aligned + sized, aligned + nothrow, and the
 *     array forms; results aligned to __STDCPP_DEFAULT_NEW_ALIGNMENT__ or the
 *     requested alignment, usable for the requested size;
 *   - alignas(64 / 4096 / 1 MiB) objects through new-expressions;
 *   - impossible sizes: plain new throws std::bad_alloc, nothrow new
 *     returns nullptr, and new consults the new_handler in a loop (a handler
 *     that frees a reserve lets the allocation be retried and succeed);
 *   - when the allocator is sohamalloc, the operators come from the same
 *     object as malloc, and operator new / malloc_usable_size agree;
 *   - containers (vector, string, map, unordered_map, deque, list),
 *     shared_ptr / make_shared, exceptions (which allocate through
 *     __cxa_allocate_exception -> malloc), std::function captures,
 *     iostream-free formatting, all verified for content;
 *   - std::thread workers that allocate with new, hand objects to each other
 *     through a locked queue and delete objects created on other threads;
 *     thread_local objects with destructors that allocate and free.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <algorithm>
#include <array>
#include <cstdarg>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <sys/wait.h>
#include <malloc.h>
#include <unistd.h>

static const char *g_name = "cxx_test";
static int g_notes;

[[noreturn]] static void fail_at(int line, const char *msg, long long a = 0, long long b = 0)
{
    char buf[512];
    int k = snprintf(buf, sizeof buf, "FAIL %s:%d [%s] %s (%lld, %lld)\n", __FILE__, line, g_name, msg, a, b);
    if (k > 0) { ssize_t w = write(2, buf, (size_t)k); (void)w; }
    _exit(1);
}
#define FAIL(msg, ...) fail_at(__LINE__, msg, ##__VA_ARGS__)
#define CHECK(c, msg, ...) do { if (!(c)) fail_at(__LINE__, "check `" #c "` failed: " msg, ##__VA_ARGS__); } while (0)

static void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void info(const char *fmt, ...)
{
    char buf[512];
    int k = snprintf(buf, sizeof buf, "[%s] ", g_name);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + k, sizeof buf - (size_t)k - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    size_t n = (size_t)k + std::min((size_t)m, sizeof buf - (size_t)k - 2);
    buf[n++] = '\n';
    ssize_t w = write(1, buf, n);
    (void)w;
}

static bool is_soham()
{
    static int cached = -1;
    if (cached < 0) cached = dlsym(RTLD_DEFAULT, "soham_version") != nullptr;
    return cached != 0;
}

static void *launder_ptr(void *p)
{
    __asm__ volatile("" : "+r"(p));
    return p;
}

static void check_block(void *p, size_t n, size_t align, const char *what)
{
    if (!p) FAIL(what, (long long)n);
    if ((uintptr_t)p & (align - 1)) fail_at(__LINE__, what, (long long)n, (long long)align);
    if (n) {
        unsigned char *c = (unsigned char *)launder_ptr(p);
        c[0] = 0x5a;
        c[n - 1] = 0xa5;
        if (n > 2) memset(c + 1, 0x3c, std::min<size_t>(n - 2, 1 << 16));
    }
}

/* ------------------------------------------------------------ operators */
static void test_operator_forms()
{
    const size_t defa = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    static const size_t sizes[] = { 0, 1, 7, 16, 24, 100, 1000, 4096, 10000, 70000, 300000, 3u << 20, 40u << 20 };
    for (size_t n : sizes) {
        /* sohamalloc: always __STDCPP_DEFAULT_NEW_ALIGNMENT__.  Others: the
           alignment of any object that fits in n bytes ([basic.stc.dynamic.
           allocation]); mimalloc returns 8-aligned blocks for n <= 8. */
        size_t want = defa;
        if (!is_soham()) while (want > sizeof(void *) && want > n) want >>= 1;
        void *p = ::operator new(n);
        check_block(p, n, want, "operator new(n)");
        ::operator delete(p);

        p = ::operator new(n, std::nothrow);
        check_block(p, n, want, "operator new(n, nothrow)");
        ::operator delete(p, std::nothrow);

        p = ::operator new(n);
        check_block(p, n, want, "operator new(n) / sized delete");
        ::operator delete(p, n);

        p = ::operator new[](n);
        check_block(p, n, want, "operator new[](n)");
        ::operator delete[](p);

        p = ::operator new[](n, std::nothrow);
        check_block(p, n, want, "operator new[](n, nothrow)");
        ::operator delete[](p, std::nothrow);

        p = ::operator new[](n);
        check_block(p, n, want, "operator new[](n) / sized delete[]");
        ::operator delete[](p, n);

        for (size_t a = 1; a <= ((size_t)4 << 20); a <<= 1) {
            std::align_val_t al{a};
            p = ::operator new(n, al);
            check_block(p, n, a, "operator new(n, align)");
            ::operator delete(p, al);

            p = ::operator new(n, al, std::nothrow);
            check_block(p, n, a, "operator new(n, align, nothrow)");
            ::operator delete(p, al, std::nothrow);

            p = ::operator new(n, al);
            check_block(p, n, a, "operator new(n, align) / sized aligned delete");
            ::operator delete(p, n, al);

            p = ::operator new[](n, al);
            check_block(p, n, a, "operator new[](n, align)");
            ::operator delete[](p, al);

            p = ::operator new[](n, al, std::nothrow);
            check_block(p, n, a, "operator new[](n, align, nothrow)");
            ::operator delete[](p, al, std::nothrow);

            p = ::operator new[](n, al);
            check_block(p, n, a, "operator new[](n, align) / sized aligned delete[]");
            ::operator delete[](p, n, al);
        }
    }
    ::operator delete(nullptr);
    ::operator delete[](nullptr);
    ::operator delete(nullptr, std::align_val_t{64});
}

struct alignas(64) A64 { unsigned char b[200]; };
struct alignas(4096) A4K { unsigned char b[5000]; };
struct alignas(1 << 20) A1M { unsigned char b[100]; };

static void test_alignas()
{
    std::vector<A64 *> v64;
    std::vector<A4K *> v4k;
    for (int i = 0; i < 200; i++) {
        A64 *a = new A64;
        CHECK(((uintptr_t)a & 63) == 0, "alignas(64) new", (long long)(uintptr_t)a);
        memset(a->b, i, sizeof a->b);
        v64.push_back(a);
        A4K *b = new A4K[3];
        CHECK(((uintptr_t)b & 4095) == 0, "alignas(4096) new[]", (long long)(uintptr_t)b);
        memset(b, i, sizeof(A4K) * 3);
        v4k.push_back(b);
    }
    for (int i = 0; i < 200; i++) {
        for (unsigned char c : v64[i]->b) CHECK(c == (unsigned char)i, "alignas(64) object corrupted", i);
        delete v64[i];
        CHECK(v4k[i][2].b[4999] == (unsigned char)i, "alignas(4096) array corrupted", i);
        delete[] v4k[i];
    }
    for (int i = 0; i < 8; i++) {
        A1M *m = new A1M;
        CHECK(((uintptr_t)m & ((1 << 20) - 1)) == 0, "alignas(1 MiB) new", (long long)(uintptr_t)m);
        m->b[99] = 1;
        delete m;
    }
    auto sp = std::make_shared<A64>();
    CHECK(((uintptr_t)sp.get() & 63) == 0, "make_shared<alignas(64)>", 0);
}

/* ------------------------------------------------------------ failures */
static int g_handler_calls;
static void *g_reserve;
static void handler_free_reserve()
{
    g_handler_calls++;
    if (g_reserve) { free(g_reserve); g_reserve = nullptr; return; }
    std::set_new_handler(nullptr);            /* next failure throws */
}

static void test_failures()
{
    /* Larger than PTRDIFF_MAX: impossible everywhere (PTRDIFF_MAX - 4 KiB
       is a valid 2 GiB request on 32-bit targets with a 4 GiB address space). */
    const size_t huge = (size_t)PTRDIFF_MAX + 4096;
    bool threw = false;
    try {
        void *p = ::operator new(huge);
        ::operator delete(p);
    } catch (const std::bad_alloc &) { threw = true; }
    CHECK(threw, "operator new(huge) did not throw std::bad_alloc", 0);

    threw = false;
    try {
        void *p = ::operator new[](huge, std::align_val_t{64});
        ::operator delete[](p, std::align_val_t{64});
    } catch (const std::bad_alloc &) { threw = true; }
    CHECK(threw, "operator new[](huge, align) did not throw", 0);

    CHECK(::operator new(huge, std::nothrow) == nullptr, "nothrow new(huge) != nullptr", 0);
    CHECK(::operator new[](huge, std::nothrow) == nullptr, "nothrow new[](huge) != nullptr", 0);
    CHECK(::operator new(huge, std::align_val_t{4096}, std::nothrow) == nullptr, "nothrow aligned new(huge)", 0);

    /* The new_handler loop: each failure calls the handler, which frees a
       reserve once (the retry still fails: the size is impossible) and then
       uninstalls itself, so new must throw after exactly 2 handler calls. */
    g_reserve = malloc(1000);
    g_handler_calls = 0;
    std::set_new_handler(handler_free_reserve);
    threw = false;
    try {
        void *p = ::operator new(huge);
        ::operator delete(p);
    } catch (const std::bad_alloc &) { threw = true; }
    std::set_new_handler(nullptr);
    CHECK(threw, "new with a new_handler did not throw in the end", 0);
    if (g_handler_calls != 2) {
        if (is_soham()) FAIL("new_handler call count", g_handler_calls, 2);
        info("NOTE new_handler called %d times (want 2: operator new must loop on the handler)", g_handler_calls);
        g_notes++;
    }

    /* A handler that throws its own exception: it propagates out of new. */
    std::set_new_handler([] { throw std::runtime_error("from handler"); });
    bool got = false;
    try {
        void *p = ::operator new(huge, std::align_val_t{32});
        ::operator delete(p, std::align_val_t{32});
    } catch (const std::runtime_error &) { got = true; } catch (...) {}
    std::set_new_handler(nullptr);
    CHECK(got, "exception from new_handler did not propagate", 0);

    /* nothrow new also consults the handler, and returns nullptr when the
       handler throws bad_alloc. */
    g_handler_calls = 0;
    std::set_new_handler([] { g_handler_calls++; throw std::bad_alloc(); });
    CHECK(::operator new(huge, std::nothrow) == nullptr, "nothrow new with a throwing handler", 0);
    std::set_new_handler(nullptr);
    if (g_handler_calls < 1) {
        if (is_soham()) FAIL("nothrow new did not call the new_handler", g_handler_calls);
        info("NOTE nothrow operator new does not consult the new_handler");
        g_notes++;
    }
}

/* ------------------------------------------------------------ provenance */
static void test_provenance()
{
    if (!is_soham()) return;
    Dl_info m{}, n{};
    void *pm = dlsym(RTLD_DEFAULT, "malloc");
    void *pn = dlsym(RTLD_DEFAULT, "_Znwm");          /* operator new(unsigned long) */
    if (!pn) pn = dlsym(RTLD_DEFAULT, "_Znwj");       /* 32-bit: operator new(unsigned int) */
    CHECK(pm && pn && dladdr(pm, &m) && dladdr(pn, &n), "dladdr", 0);
    if (strcmp(m.dli_fname, n.dli_fname) != 0) {
        char buf[600];
        snprintf(buf, sizeof buf, "operator new comes from %s, malloc from %s", n.dli_fname, m.dli_fname);
        FAIL(buf);
    }
    static const char *const syms[] = {
#if UINTPTR_MAX > 0xffffffffu
        "_Znwm", "_Znam", "_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t", "_ZnwmSt11align_val_t", "_ZnamSt11align_val_t",
        "_ZnwmSt11align_val_tRKSt9nothrow_t", "_ZnamSt11align_val_tRKSt9nothrow_t", "_ZdlPvm", "_ZdaPvm",
        "_ZdlPvmSt11align_val_t", "_ZdaPvmSt11align_val_t",
#else
        "_Znwj", "_Znaj", "_ZnwjRKSt9nothrow_t", "_ZnajRKSt9nothrow_t", "_ZnwjSt11align_val_t", "_ZnajSt11align_val_t",
        "_ZdlPvj", "_ZdaPvj", "_ZdlPvjSt11align_val_t", "_ZdaPvjSt11align_val_t",
#endif
        "_ZdlPv", "_ZdaPv", "_ZdlPvRKSt9nothrow_t", "_ZdaPvRKSt9nothrow_t", "_ZdlPvSt11align_val_t",
        "_ZdaPvSt11align_val_t", "_ZdlPvSt11align_val_tRKSt9nothrow_t", "_ZdaPvSt11align_val_tRKSt9nothrow_t",
    };
    for (const char *s : syms) {
        Dl_info d{};
        void *p = dlsym(RTLD_DEFAULT, s);
        if (!p || !dladdr(p, &d) || strcmp(d.dli_fname, m.dli_fname) != 0) {
            char buf[600];
            snprintf(buf, sizeof buf, "%s is not provided by the sohamalloc object (%s)", s, p && d.dli_fname ? d.dli_fname : "missing");
            FAIL(buf);
        }
    }
    for (size_t sz = 1; sz < 2000000; sz = sz * 3 + 1) {
        void *p = ::operator new(sz);
        CHECK(malloc_usable_size(p) >= sz, "malloc_usable_size(operator new(n)) < n", (long long)sz);
        free(p);                                         /* new/free interoperate in sohamalloc */
        p = malloc(sz);
        ::operator delete(p, sz);
    }
}

/* ------------------------------------------------------------ library use */
struct Node {
    std::string key;
    std::vector<int> vals;
    std::unique_ptr<Node> next;
};

static void test_containers(uint64_t seed)
{
    std::vector<std::string> words;
    uint64_t x = seed | 1;
    auto rnd = [&x] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    for (int i = 0; i < 20000; i++) {
        std::string s;
        size_t len = rnd() % 64;
        for (size_t k = 0; k < len; k++) s.push_back((char)('a' + rnd() % 26));
        s += std::to_string(i);
        words.push_back(std::move(s));
    }
    std::map<std::string, int> m;
    std::unordered_map<std::string, std::vector<int>> um;
    for (size_t i = 0; i < words.size(); i++) {
        m[words[i]] = (int)i;
        um[words[i]].push_back((int)i);
    }
    CHECK(m.size() == words.size() && um.size() == words.size(), "map sizes", (long long)m.size());
    for (size_t i = 0; i < words.size(); i += 7) {
        CHECK(m.at(words[i]) == (int)i, "map lookup", (long long)i);
        CHECK(um.at(words[i]).at(0) == (int)i, "unordered_map lookup", (long long)i);
    }
    std::vector<std::string> sorted = words;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::is_sorted(sorted.begin(), sorted.end()), "sort", 0);
    std::deque<std::vector<char>> dq;
    std::list<std::string> lst;
    for (int i = 0; i < 5000; i++) {
        dq.emplace_back((size_t)(rnd() % 5000), (char)i);
        if (dq.size() > 300) dq.pop_front();
        lst.push_back(words[(size_t)i % words.size()]);
        if (lst.size() > 1000) lst.erase(lst.begin());
    }
    for (auto &v : dq) for (char c : v) CHECK(c == v[0], "deque content", 0);

    /* A linked structure of unique_ptrs. */
    std::unique_ptr<Node> head;
    for (int i = 0; i < 3000; i++) {
        auto n = std::make_unique<Node>();
        n->key = words[(size_t)i];
        n->vals.assign((size_t)(i % 50), i);
        n->next = std::move(head);
        head = std::move(n);
    }
    int count = 0;
    for (Node *n = head.get(); n; n = n->next.get()) {
        int i = 2999 - count;
        CHECK(n->key == words[(size_t)i], "node key", i);
        CHECK(n->vals.size() == (size_t)(i % 50), "node vals", i);
        count++;
    }
    /* Destroy iteratively (a recursive unique_ptr chain is fine at 3000). */
    head.reset();

    /* Growth patterns that exercise realloc-like reallocation via new/delete. */
    std::vector<uint64_t> big;
    for (int i = 0; i < 3000000; i++) big.push_back((uint64_t)i * 2654435761u);
    for (int i = 0; i < 3000000; i += 4999) CHECK(big[(size_t)i] == (uint64_t)i * 2654435761u, "vector content", i);
    big.shrink_to_fit();
    std::string s(1 << 20, 'x');
    for (int i = 0; i < 10; i++) s += s.substr(0, 100000);
    CHECK(s.size() == (1u << 20) + 1000000, "string size", (long long)s.size());

    /* shared_ptr across copies, weak_ptr expiry, std::function captures. */
    std::vector<std::shared_ptr<std::vector<int>>> sps;
    std::weak_ptr<std::vector<int>> w;
    {
        auto sp = std::make_shared<std::vector<int>>(1000, 7);
        w = sp;
        for (int i = 0; i < 100; i++) sps.push_back(sp);
    }
    CHECK(!w.expired(), "weak_ptr expired early", 0);
    sps.clear();
    CHECK(w.expired(), "weak_ptr did not expire", 0);
    std::vector<std::function<int()>> fns;
    for (int i = 0; i < 1000; i++) {
        std::string cap = words[(size_t)i];
        std::array<char, 200> pad{};
        pad[0] = (char)i;
        fns.emplace_back([cap, pad, i] { return (int)cap.size() + pad[0] - (char)i + i; });
    }
    for (int i = 0; i < 1000; i++) CHECK(fns[(size_t)i]() == (int)words[(size_t)i].size() + i, "std::function", i);
}

static void test_exceptions()
{
    int caught = 0;
    for (int i = 0; i < 20000; i++) {
        try {
            if (i % 3 == 0) throw std::runtime_error(std::string(100 + i % 300, 'e'));
            if (i % 3 == 1) throw std::vector<int>((size_t)(i % 1000), i);
            throw i;
        } catch (const std::runtime_error &e) {
            CHECK(strlen(e.what()) == (size_t)(100 + i % 300), "exception message", i);
            caught++;
        } catch (const std::vector<int> &v) {
            CHECK(v.size() == (size_t)(i % 1000), "exception payload", i);
            caught++;
        } catch (int k) {
            CHECK(k == i, "int exception", k, i);
            caught++;
        }
    }
    CHECK(caught == 20000, "exceptions caught", caught);
    /* Exceptions thrown on other threads and rethrown here. */
    std::vector<std::exception_ptr> eps(8);
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; t++)
        ts.emplace_back([&eps, t] {
            try { throw std::runtime_error("thread " + std::to_string(t)); }
            catch (...) { eps[(size_t)t] = std::current_exception(); }
        });
    for (auto &t : ts) t.join();
    for (int t = 0; t < 8; t++) {
        try { std::rethrow_exception(eps[(size_t)t]); }
        catch (const std::runtime_error &e) { CHECK(std::string(e.what()) == "thread " + std::to_string(t), "rethrow", t); }
    }
}

/* ------------------------------------------------------------ threads */
struct Obj {
    uint64_t tag;
    std::vector<uint64_t> data;
    explicit Obj(uint64_t t) : tag(t), data((size_t)(t % 300) + 1, t) {}
    bool ok() const {
        for (uint64_t v : data) if (v != tag) return false;
        return data.size() == (size_t)(tag % 300) + 1;
    }
};

struct TLHolder {
    std::vector<std::string> v;
    TLHolder() { for (int i = 0; i < 50; i++) v.emplace_back(200, 'q'); }
    ~TLHolder() {
        /* Allocate and free during thread teardown. */
        std::vector<std::string> tmp(v.begin(), v.end());
        for (auto &s : tmp) if (s.size() != 200) _exit(9);
    }
};
static thread_local TLHolder tl_holder;

static void test_threads()
{
    const int NT = 6;
    std::mutex mu;
    std::deque<Obj *> q;
    std::atomic<long> made{0}, freed{0}, foreign{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < NT; t++) {
        ts.emplace_back([&, t] {
            (void)tl_holder.v.size();
            uint64_t x = 0x9e3779b97f4a7c15ull * (uint64_t)(t + 1);
            for (int i = 0; i < 60000; i++) {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                Obj *o = new Obj(((uint64_t)t << 40) | (uint64_t)i);
                made++;
                Obj *other = nullptr;
                {
                    std::lock_guard<std::mutex> g(mu);
                    q.push_back(o);
                    if (q.size() > 500 || (x & 1)) { other = q.front(); q.pop_front(); }
                }
                if (other) {
                    if (!other->ok()) FAIL("object corrupted", (long long)other->tag);
                    if ((int)(other->tag >> 40) != t) foreign++;
                    delete other;
                    freed++;
                }
                if ((x & 0xff) == 0) {
                    auto *arr = new std::string[17];
                    for (int k = 0; k < 17; k++) arr[k].assign(33, 'z');
                    delete[] arr;
                }
            }
        });
    }
    for (auto &t : ts) t.join();
    for (Obj *o : q) {
        CHECK(o->ok(), "queued object corrupted", (long long)o->tag);
        delete o;
        freed++;
    }
    CHECK(made == freed, "objects leaked", made.load(), freed.load());
    info("threads: %ld objects, %ld deleted by a thread other than their creator", made.load(), foreign.load());
}

int main()
{
    info("allocator: %s", is_soham() ? "sohamalloc" : "not sohamalloc");
    test_operator_forms();
    test_alignas();
    if (is_soham()) {
        test_failures();
    } else {
        /* Other allocators' C++ overrides may abort() instead of throwing
           (mimalloc's C new does): run the failure checks in a child and
           report an abort as a NOTE. */
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) { test_failures(); _exit(0); }
        int st = 0;
        CHECK(pid > 0 && waitpid(pid, &st, 0) == pid, "fork/waitpid", 0);
        if (WIFSIGNALED(st)) {
            info("NOTE operator new(huge) failure handling killed the process with signal %d "
                 "(std::bad_alloc / new_handler semantics not provided)", WTERMSIG(st));
            g_notes++;
        } else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            FAIL("failure-semantics child failed", st);
        }
    }
    test_provenance();
    test_containers(12345);
    test_exceptions();
    test_threads();
    /* Containers again, concurrently, on fresh threads. */
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++) ts.emplace_back([t] { test_containers(777 + (uint64_t)t); });
    for (auto &t : ts) t.join();
    info("PASS (%d notes)", g_notes);
    return 0;
}
