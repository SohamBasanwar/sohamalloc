# Allocation inside signal handlers: evidence from real software

Survey compiled on 2026-10-21 (UTC) for the thesis "async-signal-safe memory allocation, made practical".

- **Purpose:** to establish that real programs allocate inside signal handlers, what goes wrong when they do, and how projects and libraries have responded.
- **Method:** web research, preferring primary sources (advisories, man pages, source code, project bug trackers, commits).
- **Source labels:** each entry says whether it rests on a *primary* source or on a *secondary* write-up.
- **Security incidents** are described by cause, consequence and fix only.

## 1. The rule programs break

- **POSIX:** only the functions on the async-signal-safe list may be called from a handler that can interrupt arbitrary code. `malloc`, `free` and the stdio functions are not on it. The Linux man page gives two ways to write safe handlers: call only async-signal-safe functions, or block the signal around unsafe code in the main program. It calls the second "difficult in programs of any complexity". (primary: [signal-safety(7)](https://man7.org/linux/man-pages/man7/signal-safety.7.html))
- **CERT C:** rule SIG30-C ("Call only asynchronous-safe functions within signal handlers") uses `free()` inside a handler as its example of heap corruption. (primary: [SEI CERT wiki](https://wiki.sei.cmu.edu/confluence/x/CeYxBQ))
- **How it fails.** A handler that allocates while its own thread is inside the allocator either:
  - waits for a lock that the interrupted code holds, which is a **deadlock**: the thread waits on itself; or
  - modifies allocator state in the middle of an update, which is **heap corruption**.

  Both are the failure classes that `tools/sigstorm` measures.

## 2. Cases in real software

| Software | What allocates in the handler | Consequence | Response | Source |
|---|---|---|---|---|
| OpenSSH sshd, CVE-2006-5051 and its regression CVE-2024-6387 ("regreSSHion"; 8.5p1 ≤ version < 9.8p1, and < 4.4p1) | The SIGALRM (LoginGraceTime) handler called `syslog()`, which on glibc calls `malloc()`/`free()` | Heap corruption, remotely exploitable on glibc-based Linux for unauthenticated code execution as root. Qualys estimates ~6–8 hours on average per target in its test setup | 9.8p1 moved the unsafe code out of the handler. Workaround: `LoginGraceTime 0`, at the cost of a denial-of-service exposure. OpenBSD was not affected because its handler uses `syslog_r()` | primary: [Qualys advisory](https://www.qualys.com/2024/07/01/cve-2024-6387/regresshion.txt); secondary: [oss-security](https://seclists.org/oss-sec/2024/q3/2) |
| sendmail 8.11.0 and 8.12.0-Beta5 (2001) | Handlers for SIGINT, SIGTERM and others called `malloc`, `free` and `syslog` and touched global buffers | Heap corruption by re-entering `malloc()`; the vendor judged it hard to exploit | Advisory and the paper "Delivering signals for fun and profit" (M. Zalewski, 28 May 2001), which set out this class of signal-handler races | primary: [Bugtraq 2001-05](https://seclists.org/bugtraq/2001/May/271), [announcement](https://datwww.mit.edu/menelaus/bt/20795) |
| Perl before 5.8.0 | Perl-level `%SIG` handlers ran inside the C signal handler | "Unpredictable behavior—often, a core dump" when a handler re-entered `malloc` | 5.8.0 deferred signals: the C handler sets a flag and Perl runs the handler at a safe point. `PERL_SIGNALS=unsafe` restores the old behaviour "despite possible memory corruption" | primary: [perlipc](https://perldoc.perl.org/perlipc) |
| Perl 5.23.5 with an unsafe `POSIX::sigaction` handler (2015) | The handler allocated an SV, which called `malloc` | Deadlock in glibc's malloc lock: the handler interrupted `malloc_consolidate` | Open. Proposals (pre-allocated SVs, a signal pipe) were not implemented | primary: [perl5 issue #15014](https://github.com/Perl/perl5/issues/15014) |
| GNU Bash | Bash's handlers (trapped SIGINT/SIGCHLD) can allocate | Problems reported when bash is built with the system malloc | Bash's bundled malloc blocks signals with `sigprocmask` around its critical sections. The maintainer notes that "many malloc implementations (e.g., the Linux libc malloc) do not do signal manipulation" | primary: [bug-bash 2013](https://mail.gnu.org:443/archive/html/bug-bash/2013-06/msg00009.html), [bug-bash 2010](https://lists.libreplanet.org/archive/html/bug-bash/2010-02/msg00132.html) |
| GCC (2011) | The `crash_signal` handler printed diagnostics after a SIGSEGV inside `_int_malloc` | Deadlock in `__lll_lock_wait_private` | Bug report | primary: [GCC Bugzilla 50041 thread](https://gcc.gnu.org/ml/gcc-bugs/2011-08/msg01050.html) |
| g3log (C++ logging library) | The crash handler allocates when backtraces are enabled | Deadlock whenever the crash happened inside `malloc()` or `free()` | Issue | primary: [g3log #324](https://github.com/KjellKod/g3log/issues/324) |
| A program using jemalloc 3.4.0 (2013) | A SEGV handler dumped stack traces | Hang on jemalloc's locks | jemalloc's author: the handler called functions outside the async-signal-safe list | primary: [jemalloc-discuss](https://jemalloc.net/mailman/jemalloc-discuss/2013-October/000656.html) |
| GNU make (2017) | SIGINT handling reached `malloc` through locale code | Hang in `__lll_lock_wait_private` | Report | primary: [bug-make 2017-03](https://lists.nongnu.org/archive/html/bug-make/2017-03/msg00019.html) |
| Red Hat Enterprise Linux 6 | A SIGTERM handler calls `malloc()` while the application is inside `malloc()` | "Permanent deadlock" (knowledge-base title) | Knowledge-base solution | primary: [Red Hat solution 48701](https://access.redhat.com/solutions/48701) |
| PostgreSQL postmaster (before 16) | Signal handlers did non-trivial state management and called `fork()`, with signals blocked elsewhere | A long-standing source of complexity; TSan runs flagged `free()` in handler paths (2015, judged mostly false positives) | Commit 7389aad636 (2023-01-12) moved the work to a latch-based event loop; the handlers only set flags | primary: [commit announcement](https://www.postgresql.org/message-id/E1pFoVu-003E30-9H%40gemulon.postgresql.org), [2015 TSan thread](https://postgresql.org/message-id/20150817174020.GE10786%40awork2.anarazel.de) |
| Crash handlers using glibc `backtrace()` | `backtrace()` loads libgcc on first use, and the dynamic loading "usually triggers a call to malloc(3)" | Allocation inside a crash handler, which is unsafe if the crash happened inside the allocator | The man page advises loading libgcc beforehand | primary: [backtrace(3)](https://man7.org/linux/man-pages/man3/backtrace.3.html) |
| RocksDB | The stack-trace handler makes signal-unsafe calls | TSan warnings | Judged impractical to fix; documented as expected | primary: [commit 0b06af9](https://git.nextgraph.org/NextGraph/rocksdb/commit/0b06af91463d5b6068ad794ac7307075440f8e4b) |
| lldb-mi | `operator new` was reached from a SIGWINCH handler | TSan warning | Logging removed from the handlers | primary: [LLVM review D8256](https://reviews.llvm.org/D8256) |
| GWP-ASan (LLVM) | A handler took a non-recursive lock that the interrupted thread held | Deadlock | Issue | primary: [llvm-project #145281](https://github.com/llvm/llvm-project/issues/145281) |

## 3. Detection tooling

- **ThreadSanitizer** has a dedicated diagnostic, "signal-unsafe call inside of a signal", for `malloc` and `free` called from a handler. Its regression test is `signal_malloc.cpp`. That such a check exists is itself evidence that the pattern is common. (primary: [compiler-rt test](https://codebrowser.dev/llvm/compiler-rt/test/tsan/signal_malloc.cpp.html))
- **csigsafe** is a GCC-plugin static analyser for signal-handler rule violations, built for Red Hat to check source packages. It was evaluated on 37 open-source projects. (primary: [Brno University of Technology thesis](https://www.fit.vut.cz/study/thesis/21637/.en))

## 4. How signal-safe allocation has been provided so far (related work)

| Approach | Example | Cost or limit |
|---|---|---|
| Do not allocate in handlers: defer the work to a safe point (flag, self-pipe, latch) | Perl 5.8, PostgreSQL 16, OpenSSH 9.8 | Requires changing the program. It does not help code that cannot be changed: crash reporters, third-party libraries, `syslog`, `backtrace` |
| Block signals around allocator critical sections | Bash's bundled malloc. Abseil's `LowLevelAlloc` with `kAsyncSignalSafe` arenas: `pthread_sigmask` blocks every signal, then a spinlock, then direct `mmap` | Two system calls per operation. Abseil's header calls the allocator "slow and wasteful of memory" and says it "should not be used when performance is key" (primary: [low_level_alloc.cc](https://raw.githubusercontent.com/abseil/abseil-cpp/master/absl/base/internal/low_level_alloc.cc), [low_level_alloc.h](https://fuchsia.googlesource.com/third_party/abseil-cpp/+/HEAD/absl/base/internal/low_level_alloc.h)) |
| A pre-allocated pool for handlers | libatomic_ops `AO_malloc`, "intended as a safe way to allocate memory from a signal handler" | Fixed 2 MB static heap by default, objects under ~64 KB, the handler must not be reentrant, and outside handlers it needs a lock. It notes "there is no portable async-signal-safe way to obtain large chunks of memory from the OS" (primary: [README_malloc](https://ctan.csail.mit.edu/graphics/asymptote/libatomic_ops/README_malloc.txt)) |
| A completely lock-free allocator | Michael, "Scalable lock-free dynamic memory allocation", PLDI 2004 (PLDI 2014 Most Influential Paper): lists async-signal safety and use in interrupt handlers | Lock-freedom gives signal safety for its shared structures. Modern allocators get their speed from thread caches and per-thread state, which lock-freedom alone does not make reentrant (primary: [paper PDF](https://www.cs.tufts.edu/~nr/cs257/archive/neal-glew/mcrt/Non-blocking%20data%20structures/p35-michael.pdf)) |

**sohamalloc's position.** It is a general-purpose, thread-caching allocator in which a handler may call the whole malloc API (including calloc, realloc, aligned allocation, C++ new/delete and large blocks):

- the fast path tests a per-thread busy byte instead of making a system call;
- calls in handler context use trylocks on the shared heap, with a static fallback arena.

The claim under test is async-signal safety *at competitive speed*. The comparison points above have to be addressed in the paper's related work.

One limit carries over from libatomic_ops' remark. Large handler-context allocations rely on `mmap`/`munmap` being safe in handlers. That holds for Linux system calls, but POSIX does not list them as async-signal-safe. The limits section of `docs/IMPLEMENTATION_NOTES.md` has to state this.

## 5. What this survey does not establish

- **How common the pattern is.** No peer-reviewed study measuring that was found. The case list shows the pattern recurring over 25 years in widely deployed software, not a frequency.
- **Coverage.** Some reports were found through secondary search results. Each entry links its primary source where one was reached. The Emacs input-blocking history was looked at but left out, because only secondary mirrors were found.
