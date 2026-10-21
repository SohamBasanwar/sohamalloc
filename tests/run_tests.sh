#!/usr/bin/env bash
# tests/run_tests.sh -- build and run the sohamalloc test suite.
#
# The suite is allocator-agnostic: every test exercises the malloc family of
# whatever allocator the process uses, so it can be validated against glibc,
# jemalloc, mimalloc and tcmalloc as well as sohamalloc.
#
# Usage:
#   tests/run_tests.sh [--lib /path/lib.so] [--quick] [--cross aarch64|armhf|musl|mingw]
#                      [--prefixed [--prefixed-lib PATH | --shim]] [--sanitize=thread|address|undefined]
#                      [--link /path/lib.a] [--only=a,b,...] [--skip=a,b,...] [--scale=F] [--seed=N]
#                      [--cc=COMPILER] [--verbose] [--list]
#
#   --lib PATH        run every test with LD_PRELOAD=PATH (default: none = the libc allocator)
#   --quick           TEST_SCALE=0.2 (whole suite in ~1-2 min on 4 cores)
#   --cross T         build with the T cross toolchain and run under qemu/wine:
#                       aarch64 (qemu-aarch64), armhf (qemu-arm, 32-bit), musl (x86-64 musl, native),
#                       mingw (x86-64 Windows under wine; implies --prefixed)
#   --prefixed        build tests/prefixed/*.c, which call soham_* directly, and link them against
#                     the prefixed library (default build/libsohamalloc-prefixed.a, override with
#                     --prefixed-lib PATH) -- or with --shim against tests/prefixed/libc_shim.c,
#                     which maps soham_* onto the libc allocator (validates the tests themselves)
#   --sanitize=S      prefixed tests only (implies --prefixed): -fsanitize=S on the test binaries.
#                     Pair it with a prefixed library built with -DSOHAM_SANITIZE=S.
#   --link PATH.a     link an override archive (e.g. build/libsohamalloc.a) into every test binary
#                     instead of using LD_PRELOAD
#   --only / --skip   comma-separated test names
#   --scale / --seed  override TEST_SCALE / TEST_SEED
#   --cc              C compiler for native builds (default gcc; e.g. clang). C++ uses the matching
#                     g++/clang++.
#
# Binaries go to $REPO/build-tests/<variant>/ (git-ignored), logs to
# $REPO/build-tests/<variant>/logs/<test>.log.  Exit status is 0 only if no
# test failed.  Exit codes of the test programs: 0 PASS, 77 SKIP,
# 3 STALL (watchdog saw no progress: probable deadlock), anything else FAIL.
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TESTS="$REPO/tests"

LIB=""
QUICK=0
CROSS=""
PREFIXED=0
PREFIXED_LIB=""
SHIM=0
SANITIZE=""
LINK=""
ONLY=""
SKIP=""
SCALE=""
SEED=""
CC_NATIVE="gcc"
VERBOSE=0
LIST=0

die() { echo "run_tests.sh: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --lib) [ $# -ge 2 ] || die "--lib needs a path"; LIB="$2"; shift ;;
        --lib=*) LIB="${1#--lib=}" ;;
        --quick) QUICK=1 ;;
        --cross) [ $# -ge 2 ] || die "--cross needs a target"; CROSS="$2"; shift ;;
        --cross=*) CROSS="${1#--cross=}" ;;
        --prefixed) PREFIXED=1 ;;
        --prefixed-lib) [ $# -ge 2 ] || die "--prefixed-lib needs a path"; PREFIXED_LIB="$2"; PREFIXED=1; shift ;;
        --prefixed-lib=*) PREFIXED_LIB="${1#--prefixed-lib=}"; PREFIXED=1 ;;
        --shim) SHIM=1; PREFIXED=1 ;;
        --sanitize) [ $# -ge 2 ] || die "--sanitize needs a value"; SANITIZE="$2"; PREFIXED=1; shift ;;
        --sanitize=*) SANITIZE="${1#--sanitize=}"; PREFIXED=1 ;;
        --link) [ $# -ge 2 ] || die "--link needs a path"; LINK="$2"; shift ;;
        --link=*) LINK="${1#--link=}" ;;
        --only=*) ONLY="${1#--only=}" ;;
        --skip=*) SKIP="${1#--skip=}" ;;
        --scale=*) SCALE="${1#--scale=}" ;;
        --seed=*) SEED="${1#--seed=}" ;;
        --cc=*) CC_NATIVE="${1#--cc=}" ;;
        --verbose|-v) VERBOSE=1 ;;
        --list) LIST=1 ;;
        -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument '$1' (try --help)" ;;
    esac
    shift
done

case "$SANITIZE" in ""|thread|address|undefined) ;; *) die "--sanitize must be thread, address or undefined" ;; esac
[ "$CROSS" = mingw ] && PREFIXED=1
if [ -n "$LIB" ]; then
    [ -f "$LIB" ] || die "library not found: $LIB"
    LIB=$(cd "$(dirname "$LIB")" && pwd)/$(basename "$LIB")
fi
if [ -n "$LINK" ]; then
    [ -f "$LINK" ] || die "archive not found: $LINK"
    LINK=$(cd "$(dirname "$LINK")" && pwd)/$(basename "$LINK")
    [ -z "$LIB" ] || die "--lib and --link are mutually exclusive"
fi
[ "$PREFIXED" = 1 ] && [ -n "$LIB" ] && die "--prefixed tests link the allocator; --lib does not apply"

# ---------------------------------------------------------------- toolchain
RUNNER=()          # command prefix to run a test binary
EXE=""
CC="$CC_NATIVE"
case "$CC_NATIVE" in
    clang*) CXX="${CC_NATIVE/clang/clang++}" ;;
    *gcc*) CXX="${CC_NATIVE/gcc/g++}" ;;
    *) CXX="g++" ;;
esac
HAVE_CXX=1
TIMEOUT_MULT=1
TARGET_LIBS="-ldl"
EXTRA_LDFLAGS=""
case "$CROSS" in
    "") ;;
    aarch64)
        CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++
        RUNNER=(qemu-aarch64 -L /usr/aarch64-linux-gnu)
        TIMEOUT_MULT=8 ;;
    armhf)
        CC=arm-linux-gnueabihf-gcc; CXX=arm-linux-gnueabihf-g++
        RUNNER=(qemu-arm -L /usr/arm-linux-gnueabihf)
        TIMEOUT_MULT=8 ;;
    musl)
        CC=musl-gcc; HAVE_CXX=0     # musl-gcc wraps the C compiler only
        TARGET_LIBS="" ;;
    mingw)
        CC=x86_64-w64-mingw32-gcc-posix; CXX=x86_64-w64-mingw32-g++-posix
        RUNNER=(wine); EXE=".exe"
        TARGET_LIBS="-lbcrypt"; EXTRA_LDFLAGS="-static"   # os_windows.c: BCryptGenRandom
        TIMEOUT_MULT=4 ;;
    *) die "unknown --cross target '$CROSS' (aarch64, armhf, musl, mingw)" ;;
esac
command -v "$CC" >/dev/null 2>&1 || die "compiler $CC not found"
[ "$HAVE_CXX" = 1 ] && ! command -v "$CXX" >/dev/null 2>&1 && HAVE_CXX=0
if [ ${#RUNNER[@]} -gt 0 ]; then
    command -v "${RUNNER[0]}" >/dev/null 2>&1 || die "${RUNNER[0]} not found"
fi

# ---------------------------------------------------------------- what is under test
VARIANT="${CROSS:-native}"
# A non-default native compiler gets its own output directory.
if [ -z "$CROSS" ] && [ "$CC_NATIVE" != gcc ]; then VARIANT="native-$(basename "$CC_NATIVE")"; fi
SOHAM=0            # the allocator under test is sohamalloc (exports soham_version)
lib_exports_soham() { nm -D --defined-only "$1" 2>/dev/null | grep -qw soham_version || nm --defined-only "$1" 2>/dev/null | grep -qw soham_version; }
if [ "$PREFIXED" = 1 ]; then
    if [ "$SHIM" = 1 ]; then
        PREFIXED_LIB="$TESTS/prefixed/libc_shim.c"
        VARIANT="$VARIANT-prefixed-shim"
    else
        [ -n "$PREFIXED_LIB" ] || PREFIXED_LIB="$REPO/build/libsohamalloc-prefixed.a"
        [ -f "$PREFIXED_LIB" ] || die "prefixed library not found: $PREFIXED_LIB (build it with cmake, or use --shim)"
        PREFIXED_LIB=$(cd "$(dirname "$PREFIXED_LIB")" && pwd)/$(basename "$PREFIXED_LIB")
        VARIANT="$VARIANT-prefixed"
        lib_exports_soham "$PREFIXED_LIB" && SOHAM=1
    fi
    [ -n "$SANITIZE" ] && VARIANT="$VARIANT-$SANITIZE"
else
    if [ -n "$LIB" ]; then lib_exports_soham "$LIB" && SOHAM=1; fi
    if [ -n "$LINK" ]; then lib_exports_soham "$LINK" && SOHAM=1; VARIANT="$VARIANT-linked"; fi
fi
OUT="$REPO/build-tests/$VARIANT"
LOGS="$OUT/logs"
mkdir -p "$OUT" "$LOGS"

# ---------------------------------------------------------------- flags
# -fno-builtin-*: the compiler must not elide or fold allocation calls (it
# deletes unused malloc/free pairs and assumes facts about results), since
# the calls themselves are what is being tested.
NOBUILTIN="-fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -fno-builtin-aligned_alloc -fno-builtin-posix_memalign -fno-builtin-memalign -fno-builtin-valloc -fno-builtin-pvalloc -fno-builtin-reallocarray -fno-builtin-malloc_usable_size"
WARN="-Wall -Wextra -Wno-alloc-size-larger-than -Wno-unused-function"
case "$CC" in *gcc*) WARN="$WARN -Wno-use-after-free -Wno-free-nonheap-object -Wno-stringop-overflow -Wno-array-bounds" ;; esac
CFLAGS="-std=c11 -O2 -g -pthread $WARN $NOBUILTIN -I$TESTS -I$REPO/include"
CXXFLAGS="-std=c++17 -O2 -g -pthread $WARN $NOBUILTIN -I$TESTS -I$REPO/include"
# (clang++ first: "clang++" also matches *g++*)
case "$CXX" in
    *clang++*) CXXFLAGS="$CXXFLAGS -fno-assume-sane-operator-new -fsized-deallocation" ;;
    *g++*) CXXFLAGS="$CXXFLAGS -fno-allocation-dce -Wno-mismatched-new-delete" ;;
esac
SANFLAGS=""
case "$SANITIZE" in
    thread) SANFLAGS="-fsanitize=thread -O1" ;;
    address) SANFLAGS="-fsanitize=address -fno-omit-frame-pointer" ;;
    undefined) SANFLAGS="-fsanitize=undefined -fno-sanitize-recover=all" ;;
esac
LINK_LIBS=""
if [ -n "$LINK" ]; then
    # -rdynamic: the tests find soham_* with dlsym(RTLD_DEFAULT), which only
    # sees the executable's symbols if they are in its dynamic symbol table.
    LINK_LIBS="-rdynamic -Wl,--whole-archive $LINK -Wl,--no-whole-archive -lstdc++"
fi

# ---------------------------------------------------------------- test table
# name | source | kind (c, cxx) | full timeout (s) | flags
#   flags: soham  = needs sohamalloc (self-skips otherwise; the regress/
#                   reproducers do not, so the runner skips them)
#          info   = informational unless the allocator is sohamalloc
if [ "$PREFIXED" = 1 ]; then
    TABLE="
api_semantics      prefixed/api_semantics.c      c    180
random_ops         prefixed/random_ops.c         c    600
producer_consumer  prefixed/producer_consumer.c  c    600
thread_churn       prefixed/thread_churn.c       c    600
stats_api          prefixed/stats_api.c          c    180  soham
internal_props     prefixed/internal_props.c     c    120  soham
regress_check_heap regress/check_heap_concurrent.c c  120  soham
regress_thread_detach regress/thread_exit_detach.c c   60  soham
regress_stats_race regress/stats_concurrent.c c   60  soham
"
else
    TABLE="
api_semantics      api_semantics.c      c    120
sizes_sweep        sizes_sweep.c        c    300
random_ops         random_ops.c         c    400
threads_local      threads_local.c      c    400
producer_consumer  producer_consumer.c  c    400
thread_churn       thread_churn.c       c    400
fork_test          fork_test.c          c    400
signals            signals.c            c    180  info
large_and_huge     large_and_huge.c     c    400
cxx_test           cxx_test.cpp         cxx  400
memory_return      memory_return.c      c    300  soham
stats_api          stats_api.c          c    180  soham
internal_props     internal_props.c     c    120  soham
regress_signal_enomem regress/signal_reentrant_enomem.c c 60 soham
regress_signal_attach regress/signal_thread_attach.c c 60 soham
regress_signal_solo regress/signal_solo_realloc.c c 60 soham
regress_invalid_frees regress/invalid_frees.c c 60 soham
regress_trim_deadlock regress/signal_trim_deadlock.c c 60 soham
regress_fork_notify regress/fork_notify_reuse.c c 60 soham
regress_fork_busy  regress/fork_concurrent_busy.c c 120 soham
regress_sig_aligned regress/signal_aligned_enomem.c c 60 soham
regress_shared_trim regress/signal_shared_release.c c 60 soham
regress_bss_bounded regress/bss_bounded.c c 60 soham
"
fi

in_list() { local x="$1" l=",$2,"; [[ "$l" == *",$x,"* ]]; }

NAMES=()
declare -A SRC KIND TMO FLAGS
while read -r name src kind tmo flags; do
    [ -n "$name" ] || continue
    if [ -n "$ONLY" ] && ! in_list "$name" "$ONLY"; then continue; fi
    if [ -n "$SKIP" ] && in_list "$name" "$SKIP"; then continue; fi
    NAMES+=("$name"); SRC[$name]="$src"; KIND[$name]="$kind"; TMO[$name]="$tmo"; FLAGS[$name]="${flags:-}"
done <<< "$TABLE"
if [ "$LIST" = 1 ]; then printf '%s\n' "${NAMES[@]}"; exit 0; fi
[ ${#NAMES[@]} -gt 0 ] || die "no tests selected"

# ---------------------------------------------------------------- build
echo "== sohamalloc test suite"
echo "   allocator : $( if [ "$PREFIXED" = 1 ]; then echo "prefixed: $PREFIXED_LIB"; elif [ -n "$LINK" ]; then echo "linked: $LINK"; elif [ -n "$LIB" ]; then echo "LD_PRELOAD=$LIB"; else echo "libc default"; fi )"
echo "   sohamalloc: $( [ "$SOHAM" = 1 ] && echo yes || echo no )   target: ${CROSS:-native} ($CC)   sanitizer: ${SANITIZE:-none}   quick: $QUICK"
echo "   output    : $OUT"

build_one() {    # name -> writes $OUT/name.buildlog, returns status
    local name="$1" src="$TESTS/${SRC[$1]}" kind="${KIND[$1]}" bin="$OUT/$1$EXE" log="$OUT/$1.buildlog"
    local cmd
    if [ "$kind" = cxx ]; then
        if [ "$HAVE_CXX" != 1 ]; then echo "no C++ compiler for this target" > "$log"; return 99; fi
        cmd=("$CXX" $CXXFLAGS $SANFLAGS "$src" -o "$bin" $EXTRA_LDFLAGS $LINK_LIBS $TARGET_LIBS)
    elif [ "$PREFIXED" = 1 ]; then
        cmd=("$CC" $CFLAGS $SANFLAGS -DTEST_PREFIXED=1 "$src" "$PREFIXED_LIB" -o "$bin" $EXTRA_LDFLAGS $TARGET_LIBS -lm)
        [ "$SHIM" = 1 ] && cmd+=(-DTEST_SHIM=1)
        # The prefixed archive may contain the C++ translation unit's support code.
        [ "$SHIM" = 1 ] || cmd+=(-lpthread)
    else
        cmd=("$CC" $CFLAGS "$src" -o "$bin" $EXTRA_LDFLAGS $LINK_LIBS $TARGET_LIBS -lm)
    fi
    echo "${cmd[*]}" > "$log"
    "${cmd[@]}" >> "$log" 2>&1
}

declare -A BUILD_RC
pids=()
for name in "${NAMES[@]}"; do
    build_one "$name" &
    pids+=("$!:$name")
done
for pn in "${pids[@]}"; do
    pid="${pn%%:*}"; name="${pn#*:}"
    wait "$pid"; BUILD_RC[$name]=$?
done

# ---------------------------------------------------------------- run
scale="${SCALE:-$( [ "$QUICK" = 1 ] && echo 0.2 || echo 1 )}"
export TEST_SCALE="$scale"
[ -n "$SEED" ] && export TEST_SEED="$SEED"
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1 exitcode=66 second_deadlock_stack=1 history_size=4}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1 detect_leaks=1 abort_on_error=0 allocator_may_return_null=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1 print_stacktrace=1}"
export TEST_IS_SOHAM="$SOHAM"
# Tests that re-exec themselves (stats_api) only do so when no emulator runs them.
if [ ${#RUNNER[@]} -eq 0 ]; then export TEST_NATIVE=1; else export TEST_NATIVE=0; fi
export WINEDEBUG="${WINEDEBUG:--all}"

npass=0; nfail=0; nskip=0; ninfo=0
RESULTS=()
total_t0=$(date +%s.%N)
printf '\n%-20s %-14s %8s  %s\n' TEST RESULT TIME DETAIL
printf '%-20s %-14s %8s  %s\n' -------------------- -------------- -------- ------------------------------
for name in "${NAMES[@]}"; do
    bin="$OUT/$name$EXE"; log="$LOGS/$name.log"
    detail=""; status=""
    if [ "${BUILD_RC[$name]}" = 99 ]; then
        status=SKIP; detail="no C++ compiler for this target"
        nskip=$((nskip + 1))
        printf '%-20s %-14s %8s  %s\n' "$name" "$status" "-" "$detail"
        RESULTS+=("$name $status")
        continue
    fi
    if [ "${BUILD_RC[$name]}" != 0 ]; then
        status=FAIL; detail="build failed (see $OUT/$name.buildlog)"
        nfail=$((nfail + 1))
        printf '%-20s %-14s %8s  %s\n' "$name" "$status" "-" "$detail"
        [ "$VERBOSE" = 1 ] && tail -30 "$OUT/$name.buildlog" | sed 's/^/    | /'
        RESULTS+=("$name $status")
        continue
    fi
    if [[ "${SRC[$name]}" == regress/* ]] && [[ " ${FLAGS[$name]} " == *" soham "* ]] && [ "$SOHAM" != 1 ]; then
        status=SKIP; detail="sohamalloc-only regression reproducer"
        nskip=$((nskip + 1))
        printf '%-20s %-14s %8s  %s\n' "$name" "$status" "-" "$detail"
        RESULTS+=("$name $status")
        continue
    fi
    tmo=$(( ${TMO[$name]} * TIMEOUT_MULT ))
    [ "$QUICK" = 1 ] && tmo=$(( tmo / 2 > 60 ? tmo / 2 : 60 ))
    cmd=()
    if [ -n "$LIB" ]; then
        if [ ${#RUNNER[@]} -gt 0 ] && [[ "${RUNNER[0]}" == qemu-* ]]; then
            cmd=("${RUNNER[@]}" -E "LD_PRELOAD=$LIB" "$bin")
        else
            cmd=(env "LD_PRELOAD=$LIB" "$bin")
        fi
    else
        cmd=("${RUNNER[@]}" "$bin")
    fi
    t0=$(date +%s.%N)
    ( cd "$OUT" && exec timeout -k 10 "$tmo" "${cmd[@]}" ) > "$log" 2>&1 < /dev/null
    rc=$?
    t1=$(date +%s.%N)
    dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1fs", b - a }')
    case $rc in
        0) status=PASS ;;
        77) status=SKIP; detail=$(grep -m1 '^SKIP' "$log" | sed 's/^SKIP \[[^]]*\] //') ;;
        3) status=FAIL; detail="STALL (probable deadlock)" ;;
        124|137) status=FAIL; detail="TIMEOUT after ${tmo}s (probable deadlock)" ;;
        66) status=FAIL; detail="sanitizer report" ;;
        *) if [ $rc -gt 128 ]; then status=FAIL; detail="crashed: signal $((rc - 128))"; else status=FAIL; detail="exit status $rc"; fi ;;
    esac
    if [ "$status" = FAIL ]; then
        first=$(grep -m1 -E '^(FAIL|STALL)|ERROR: (Thread|Address|Undefined)Sanitizer|runtime error|WARNING: ThreadSanitizer' "$log" | cut -c1-160)
        [ -n "$first" ] && detail="$detail: $first"
    fi
    if [ "$status" = PASS ]; then
        notes=$(grep -c '\] NOTE' "$log" 2>/dev/null)
        [ "${notes:-0}" -gt 0 ] && detail="$notes note(s) in log"
        # Sanitizer reports that did not change the exit status still count.
        if grep -q -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:' "$log"; then
            status=FAIL; detail="sanitizer report in log"
        fi
    fi
    if [[ " ${FLAGS[$name]} " == *" info "* ]] && [ "$SOHAM" != 1 ] && [ "$status" = FAIL ]; then
        status=INFO; detail="informational for non-sohamalloc: $detail"
    fi
    case $status in
        PASS) npass=$((npass + 1)) ;;
        SKIP) nskip=$((nskip + 1)) ;;
        INFO) ninfo=$((ninfo + 1)) ;;
        *) nfail=$((nfail + 1)) ;;
    esac
    printf '%-20s %-14s %8s  %s\n' "$name" "$status" "$dt" "$detail"
    if [ "$VERBOSE" = 1 ] || [ "$status" = FAIL ]; then
        tail -n 15 "$log" | sed 's/^/    | /'
    fi
    RESULTS+=("$name $status")
done
total_t1=$(date +%s.%N)
echo
awk -v a="$total_t0" -v b="$total_t1" -v p=$npass -v f=$nfail -v s=$nskip -v i=$ninfo \
    'BEGIN { printf "== %d passed, %d failed, %d skipped, %d informational in %.0fs\n", p, f, s, i, b - a }'
echo "   logs: $LOGS"
[ $nfail -eq 0 ]
