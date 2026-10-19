#!/usr/bin/env bash
# Build the sohamalloc benchmark suite into bench/build/ (git-ignored).
#
#   bench/build_bench.sh [--mb DIR] [--no-download] [--jobs N]
#
#   --mb DIR        mimalloc-bench checkout to build from (default: $MIMALLOC_BENCH_DIR,
#                   else bench/build/mimalloc-bench, cloned at the pinned commit if absent)
#   --no-download   never touch the network (shbench is then skipped if not cached)
#   --jobs N        parallel build jobs (default: nproc)
#
# What it produces (see bench/README.md for the full list and the skips):
#   bench/build/mb/<name>           mimalloc-bench executables (cfrac, espresso, ...)
#   bench/build/mb-src/             private, patched copy of mimalloc-bench/bench
#   bench/build/workloads/          generated inputs for the realistic workloads
#   bench/build/manifest.txt        what was built, what was skipped and why
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(dirname "$HERE")
BUILD=$HERE/build
MB_COMMIT=391382606681408e34d96ec54e9e6c79805aa3ac   # mimalloc-bench master, 2026-10-17
MB_URL=https://github.com/daanx/mimalloc-bench
SH6_URL=http://www.microquill.com/smartheap/shbench/bench.zip
SH8_URL=http://www.microquill.com/smartheap/SH8BENCH.zip
SH6_SHA=506354d66b9eebef105d757e055bc55e8d4aea1e7b51faab3da35b0466c923a1
SH8_SHA=12a8e75248c9dcbfee28245c12bc937a16ef56ec9cbfab88d0e348271667726f

MB_DIR=${MIMALLOC_BENCH_DIR:-}
DOWNLOAD=1
JOBS=$(nproc)
while [ $# -gt 0 ]; do
  case "$1" in
    --mb) MB_DIR=$2; shift 2;;
    --no-download) DOWNLOAD=0; shift;;
    --jobs) JOBS=$2; shift 2;;
    -h|--help) sed -n '2,16p' "$0"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done

mkdir -p "$BUILD"/dl "$BUILD"/workloads
MANIFEST=$BUILD/manifest.txt
: > "$MANIFEST"
note() { echo "$*" | tee -a "$MANIFEST"; }
phase() { echo; echo "=== $*"; }

# ---------------------------------------------------------------- sources
phase "locate mimalloc-bench"
if [ -z "$MB_DIR" ]; then
  MB_DIR=$BUILD/mimalloc-bench
  if [ ! -d "$MB_DIR/bench" ]; then
    if [ "$DOWNLOAD" = 1 ]; then
      git clone --quiet "$MB_URL" "$MB_DIR" && git -C "$MB_DIR" checkout --quiet "$MB_COMMIT" \
        || { echo "cannot clone $MB_URL; pass --mb DIR" >&2; exit 1; }
    else
      echo "no mimalloc-bench: pass --mb DIR or set MIMALLOC_BENCH_DIR" >&2; exit 1
    fi
  fi
fi
[ -d "$MB_DIR/bench" ] || { echo "$MB_DIR/bench not found" >&2; exit 1; }
MB_REV=$(git -C "$MB_DIR" rev-parse HEAD 2>/dev/null || echo unknown)
note "mimalloc-bench: $MB_DIR (rev $MB_REV)"
[ "$MB_REV" = "$MB_COMMIT" ] || note "  note: harness was validated at $MB_COMMIT"

# Private copy: we patch shbench in place and never write into the checkout.
rm -rf "$BUILD/mb-src"
cp -r "$MB_DIR/bench" "$BUILD/mb-src"

# Scale patches for the benchmarks whose work is a compile-time constant, so
# that a 5-allocator x 3-repetition run fits in ~25 minutes on 4 cores.
# Every patch only lowers an iteration count; the access pattern is unchanged.
MALLOC_LARGE_ITERS=${MALLOC_LARGE_ITERS:-500}      # upstream 2000
ALLOC_TEST_ITERS=${ALLOC_TEST_ITERS:-40000000}     # upstream 100000000
sed -i "s/kNumIterations = 2000;/kNumIterations = $MALLOC_LARGE_ITERS;/" \
    "$BUILD/mb-src/malloc-large/malloc-large.cpp"
sed -i "s/iterCount = 100000000;/iterCount = $ALLOC_TEST_ITERS;/" \
    "$BUILD/mb-src/alloc-test/allocator_tester.cpp"
# rptest sleeps 1 s (wall) ~20 times between its measured phases; its metric is
# memory ops per *CPU* second, so shortening the sleeps to 100 ms removes ~18 s
# of idle wall time per run without changing what is measured.
sed -i "s/thread_sleep(1000);/thread_sleep(100);/g" "$BUILD/mb-src/rptest/rptest.c"
grep -q "thread_sleep(1000)" "$BUILD/mb-src/rptest/rptest.c" \
  && note "scale: rptest sleep patch did not apply" || note "scale: rptest inter-phase sleeps 1000 ms -> 100 ms"
grep -q "kNumIterations = $MALLOC_LARGE_ITERS;" "$BUILD/mb-src/malloc-large/malloc-large.cpp" \
  && note "scale: malloc-large kNumIterations 2000 -> $MALLOC_LARGE_ITERS" \
  || note "scale: malloc-large patch did not apply (upstream changed?)"
grep -q "iterCount = $ALLOC_TEST_ITERS;" "$BUILD/mb-src/alloc-test/allocator_tester.cpp" \
  && note "scale: alloc-test iterCount 100000000 -> $ALLOC_TEST_ITERS" \
  || note "scale: alloc-test patch did not apply (upstream changed?)"

phase "shbench (MicroQuill, not redistributable: download + checksum + patch)"
fetch() {  # <url> <file> <sha256>
  local f=$BUILD/dl/$2
  if [ ! -f "$f" ] && [ "$DOWNLOAD" = 1 ]; then
    curl -sS -m 60 -L -o "$f.tmp" "$1" && mv "$f.tmp" "$f" || rm -f "$f.tmp"
  fi
  [ -f "$f" ] || return 1
  echo "$3  $f" | sha256sum -c --quiet - || { rm -f "$f"; return 1; }
}
SH=$BUILD/mb-src/shbench
if fetch "$SH6_URL" bench.zip "$SH6_SHA"; then
  (cd "$SH" && unzip -qo "$BUILD/dl/bench.zip" && dos2unix -q sh6bench.patch sh6bench.c \
     && patch -s -p1 -o sh6bench-new.c sh6bench.c sh6bench.patch) \
     || { note "skip sh6bench: patch failed"; rm -f "$SH/sh6bench-new.c"; }
else
  note "skip sh6bench: source not available offline ($SH6_URL)"
fi
if fetch "$SH8_URL" SH8BENCH.zip "$SH8_SHA"; then
  (cd "$SH" && unzip -qo "$BUILD/dl/SH8BENCH.zip" && dos2unix -q sh8bench.patch SH8BENCH.C \
     && patch -s -p1 -o sh8bench-new.c SH8BENCH.C sh8bench.patch) \
     || { note "skip sh8bench: patch failed"; rm -f "$SH/sh8bench-new.c"; }
else
  note "skip sh8bench: source not available offline ($SH8_URL)"
fi

# ---------------------------------------------------------------- mimalloc-bench
phase "build mimalloc-bench subset"
MB_TARGETS="cfrac espresso barnes larson larson-sized alloc-test sh6bench sh8bench \
  cache-scratch cache-thrash xmalloc-test malloc-large mstress mleak rptest glibc-simple glibc-thread"
cmake -S "$HERE/cmake" -B "$BUILD/mb" -DCMAKE_BUILD_TYPE=Release -DMB_SRC="$BUILD/mb-src" \
      -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ > "$BUILD/cmake-configure.log" 2>&1 \
  || { cat "$BUILD/cmake-configure.log"; exit 1; }
for t in $MB_TARGETS; do
  if cmake --build "$BUILD/mb" --target "$t" -j "$JOBS" > "$BUILD/build-$t.log" 2>&1 \
     && [ -x "$BUILD/mb/$t" ]; then
    note "built $t"
  else
    note "skip $t: build failed (see bench/build/build-$t.log)"
  fi
done
# Input files used by the runner, kept next to the binaries.
cp "$BUILD/mb-src/espresso/largest.espresso" "$BUILD/mb/" 2>/dev/null
# barnes: halve the body count (line 2 of the input; line 1 is an empty
# input-file name meaning "generate a Plummer model"; upstream 163840).
BARNES_BODIES=${BARNES_BODIES:-81920}
sed "s/^163840\$/$BARNES_BODIES/" "$BUILD/mb-src/barnes/input" > "$BUILD/mb/barnes.input"
grep -qx "$BARNES_BODIES" "$BUILD/mb/barnes.input" \
  && note "scale: barnes nbody 163840 -> $BARNES_BODIES" || note "scale: barnes patch did not apply"

# ---------------------------------------------------------------- realistic workloads
phase "realistic workloads"
if command -v python3 >/dev/null; then
  note "built python (py_alloc.py, $(python3 -V 2>&1))"
else
  note "skip python: python3 not found"
fi

if command -v g++ >/dev/null; then
  python3 "$HERE/workloads/gen_cxx.py" "$BUILD/workloads/tmpl.cpp" 3
  python3 "$HERE/workloads/gen_cxx.py" "$BUILD/workloads/tmpl-quick.cpp" 1
  note "built gxx (generated 3-module template-heavy TU, g++ $(g++ -dumpfullversion))"
else
  note "skip gxx: g++ not found"
fi

try_apt() {  # <command> <package>
  command -v "$1" >/dev/null && return 0
  [ "$DOWNLOAD" = 1 ] && [ "$(id -u)" = 0 ] && command -v apt-get >/dev/null || return 1
  echo "installing $2 with apt-get ..."
  apt-get install -y "$2" > "$BUILD/apt-$2.log" 2>&1 \
    || { apt-get update > /dev/null 2>&1 && apt-get install -y "$2" >> "$BUILD/apt-$2.log" 2>&1; }
  command -v "$1" >/dev/null
}

if try_apt sqlite3 sqlite3; then
  sed 's/@ROWS@/250000/' "$HERE/workloads/sqlite_workload.sql" > "$BUILD/workloads/sqlite.sql"
  sed 's/@ROWS@/60000/'  "$HERE/workloads/sqlite_workload.sql" > "$BUILD/workloads/sqlite-quick.sql"
  note "built sqlite ($(sqlite3 -version | cut -d' ' -f1), 250000 rows)"
else
  note "skip sqlite: sqlite3 not installable"
fi

if try_apt redis-server redis-server && command -v redis-benchmark >/dev/null; then
  note "built redis ($(redis-server --version | cut -d' ' -f3,6))"
else
  note "skip redis: redis-server not installable"
fi

if ! try_apt /usr/bin/time time; then
  echo "WARNING: /usr/bin/time (GNU time) is required by run_bench.sh" >&2
fi

# ---------------------------------------------------------------- allocators
phase "allocators"
LIBDIR=/usr/lib/x86_64-linux-gnu
for a in "jemalloc $LIBDIR/libjemalloc.so.2" "mimalloc $LIBDIR/libmimalloc.so.2" \
         "tcmalloc $LIBDIR/libtcmalloc_minimal.so.4" "sohamalloc $ROOT/build/libsohamalloc.so"; do
  set -- $a
  if [ -f "$2" ]; then note "allocator $1: $2"; else note "allocator $1 MISSING: $2"; fi
done
[ -f "$ROOT/build/libsohamalloc.so" ] || echo "build sohamalloc first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4" >&2

phase "done"
echo "manifest: $MANIFEST"
