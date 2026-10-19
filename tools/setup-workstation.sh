#!/usr/bin/env bash
# tools/setup-workstation.sh -- prepare an Ubuntu 22.04/24.04 machine (native or
# WSL2) to build, test and benchmark sohamalloc, and record the machine's
# configuration for the research log.
#
# Usage (from the repository root):
#   tools/setup-workstation.sh            install packages, build, quick test, build benchmarks
#   tools/setup-workstation.sh --dry-run  print what would be installed and run, change nothing
#   tools/setup-workstation.sh --no-bench skip downloading and building the benchmark suite
#
# It uses sudo for apt-get only.  Safe to run again.

set -euo pipefail

DRY=0; BENCH=1
for a in "$@"; do
    case "$a" in
        --dry-run) DRY=1 ;;
        --no-bench) BENCH=0 ;;
        -h|--help) sed -n 2,12p "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
say() { printf '\n== %s\n' "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "+ $*"; else "$@"; fi; }

# ------------------------------------------------------------------ checks
. /etc/os-release 2>/dev/null || true
if [ "${ID:-}" != ubuntu ]; then
    echo "warning: this script targets Ubuntu (found '${PRETTY_NAME:-unknown}'); package names may differ" >&2
fi
WSL=0
grep -qi microsoft /proc/version 2>/dev/null && WSL=1
say "machine"
echo "OS:     ${PRETTY_NAME:-unknown}$([ $WSL = 1 ] && echo '  (WSL2: fine for development and tests, not for paper-quality timings)')"
echo "kernel: $(uname -r)"
echo "CPU:    $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')  ($(nproc) logical CPUs)"
echo "memory: $(awk '/MemTotal/{printf "%.1f GiB", $2/1048576}' /proc/meminfo)"

# ---------------------------------------------------------------- packages
PKGS=(
  # build and debug
  build-essential cmake git clang gdb valgrind curl ca-certificates time
  # analysis and plots for the research log
  python3 python3-numpy python3-scipy python3-matplotlib
  # allocators compared in the benchmarks
  libjemalloc2 libjemalloc-dev libmimalloc2.0 libmimalloc-dev
  libtcmalloc-minimal4t64 libgoogle-perftools-dev
  # application benchmarks
  redis-server redis-tools sqlite3
  # cross-platform verification: aarch64, armhf (qemu), musl, Windows (mingw + wine)
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
  qemu-user qemu-user-binfmt musl-tools musl-dev
  gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix wine64 wine
  # machine topology and noise checks
  numactl hwloc stress-ng
)
if [ $WSL = 0 ]; then
  # perf for the running kernel (not available for the WSL kernel)
  PKGS+=(linux-tools-common linux-tools-generic)
  apt-cache show "linux-tools-$(uname -r)" >/dev/null 2>&1 && PKGS+=("linux-tools-$(uname -r)")
fi
say "installing ${#PKGS[@]} packages"
run sudo apt-get update
run sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${PKGS[@]}"

# ------------------------------------------------------------ build + test
say "building sohamalloc (release and debug)"
run cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
run cmake --build build -j"$(nproc)"
run cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Release -DSOHAM_DEBUG=ON -DSOHAM_BUILD_TESTS=OFF
run cmake --build build-debug -j"$(nproc)"

say "quick test suite (about a minute)"
run tests/run_tests.sh --quick --lib build/libsohamalloc.so

if [ "$BENCH" = 1 ]; then
    say "downloading and building the benchmark suite"
    run bench/build_bench.sh
fi

# --------------------------------------------------- record the machine
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="docs/research-log/data/machine-$(hostname -s)-$STAMP.txt"
say "recording the machine configuration in $OUT"
if [ "$DRY" = 0 ]; then
    mkdir -p "$(dirname "$OUT")"
    {
        echo "# sohamalloc workstation record, $STAMP"
        echo "## os";      cat /etc/os-release; uname -a
        echo "## wsl";     echo "$WSL"
        echo "## cpu";     lscpu
        echo "## hybrid topology (Intel P/E cores)"
        for t in cpu_core cpu_atom; do [ -r /sys/devices/$t/cpus ] && echo "$t: $(cat /sys/devices/$t/cpus)"; done
        echo "## memory";  free -h; cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null
        echo "## governor / turbo"
        cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true
        cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true
        echo "## compilers"; gcc --version | head -1; clang --version | head -1; cmake --version | head -1
        echo "## allocators"; dpkg -l libjemalloc2 libmimalloc2.0 libtcmalloc-minimal4t64 2>/dev/null | tail -n +6
        echo "## kernel command line"; cat /proc/cmdline
    } > "$OUT"
    echo "wrote $OUT"
fi

say "done"
echo "Next: install Claude Code (docs/LOCAL_SETUP.md, part 5) and continue the session."
