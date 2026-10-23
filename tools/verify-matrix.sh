#!/usr/bin/env bash
# tools/verify-matrix.sh -- the sohamalloc pre-commit verification matrix, run
# unattended: release, debug and sanitizer builds, every test suite (native,
# debug, TSan, ASan, UBSan, real programs, aarch64, armhf, musl, mingw), then
# the per-commit signal storm on the debug build (H3 assertion active).
#
#   tools/verify-matrix.sh OUTDIR
#
# Writes OUTDIR/summary.txt (one line per step, with the runner's tally) and
# one log per step.  The exact commands are those of tests/README.md.
set -u
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO" || exit 2
OUT=${1:?usage: tools/verify-matrix.sh OUTDIR}
mkdir -p "$OUT"
SUM="$OUT/summary.txt"
note() { echo "$(date -u +%FT%TZ) $*" | tee -a "$SUM"; }
step() {   # step NAME CMD...: run, log, record the runner's tally line
    local name=$1; shift
    local log="$OUT/$name.log"
    "$@" > "$log" 2>&1
    local rc=$?
    local tally
    tally=$(grep -E '^== [0-9]+ passed|passed, [0-9]+ failed' "$log" | tail -1)
    note "$name rc=$rc ${tally}"
    return $rc
}

note "matrix start: $(git describe --always --dirty) on $(uname -r)"
git status --short >> "$SUM"

# ------------------------------------------------------------------ builds
step build-release  bash -c 'cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8'
step build-debug    bash -c 'cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Release -DSOHAM_DEBUG=ON && cmake --build build-debug -j8'
for S in thread address undefined; do
    step build-$S bash -c "cmake -S . -B build-$S -DCMAKE_BUILD_TYPE=Release -DSOHAM_SANITIZE=$S && cmake --build build-$S --target sohamalloc_prefixed -j8"
done
step build-aarch64 bash -c 'cmake -S . -B build-aarch64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ && cmake --build build-aarch64 -j8'
step build-armhf bash -c 'cmake -S . -B build-armhf -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc -DCMAKE_CXX_COMPILER=arm-linux-gnueabihf-g++ && cmake --build build-armhf -j8'
step build-musl bash -c 'mkdir -p build-musl && F="-std=c11 -O2 -fPIC -fvisibility=hidden -ftls-model=initial-exec -fno-builtin-malloc -fno-builtin-free -fno-builtin-calloc -fno-builtin-realloc -Iinclude" && musl-gcc $F -DSOHAM_OVERRIDE=1 -shared src/sohamalloc.c src/os_posix.c -o build-musl/libsohamalloc.so'
step build-mingw bash -c 'cmake -S . -B build-mingw -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix && cmake --build build-mingw --target sohamalloc_prefixed -j8'

# ------------------------------------------------------------------ suites
step native   tests/run_tests.sh --lib build/libsohamalloc.so
step debug    tests/run_tests.sh --lib build-debug/libsohamalloc.so
step prefixed tests/run_tests.sh --prefixed-lib build/libsohamalloc-prefixed.a
step tsan     tests/run_tests.sh --sanitize=thread --prefixed-lib build-thread/libsohamalloc-prefixed.a
step asan     tests/run_tests.sh --sanitize=address --prefixed-lib build-address/libsohamalloc-prefixed.a
step ubsan    tests/run_tests.sh --sanitize=undefined --prefixed-lib build-undefined/libsohamalloc-prefixed.a
step preload  tests/preload_smoke.sh --lib build/libsohamalloc.so
step aarch64  tests/run_tests.sh --cross aarch64 --quick --lib build-aarch64/libsohamalloc.so
step armhf    tests/run_tests.sh --cross armhf --quick --lib build-armhf/libsohamalloc.so
step musl     tests/run_tests.sh --cross musl --quick --lib build-musl/libsohamalloc.so
step mingw    tests/run_tests.sh --cross mingw --quick --prefixed-lib build-mingw/libsohamalloc-prefixed.a

# ------------------------------------------------------------------ sigstorm (debug build: H3 assertion active)
SG="$OUT/sigstorm-bin"
gcc -O2 -g -Wall -Wextra -pthread tools/sigstorm/sigstorm.c -o "$SG" || note "sigstorm build FAILED"
LIB="$REPO/build-debug/libsohamalloc.so"
JOBS="$OUT/sigstorm-jobs.txt"
: > "$JOBS"
for K in none malloc free realloc calloc large churn aligned; do
    for T in 1 2 4; do for S in 1 2; do echo "--handler=$K --threads=$T --seed=$S" >> "$JOBS"; done; done
done
for K in churn malloc aligned; do
    for T in 1 2 4; do for S in 1 2; do echo "--handler=$K --threads=$T --seed=$S --workload=page" >> "$JOBS"; done; done
done
SGLOG="$OUT/sigstorm.log"
: > "$SGLOG"
export SG LIB SGLOG
xargs -P 4 -I{} bash -c 'out=$(LD_PRELOAD="$LIB" "$SG" {} --seconds=4 2>&1); rc=$?; echo "rc=$rc {} :: $(echo "$out" | tail -1)" >> "$SGLOG"' < "$JOBS"
n=$(wc -l < "$SGLOG"); bad=$(grep -vc '^rc=0 ' "$SGLOG")
note "sigstorm runs=$n failures=$bad"

note "matrix end"
