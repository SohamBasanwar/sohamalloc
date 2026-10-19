#!/usr/bin/env bash
# Run the sohamalloc benchmark suite against several allocators.
#
#   bench/run_bench.sh [--allocs "glibc jemalloc mimalloc tcmalloc sohamalloc"]
#                      [--reps N] [--quick] [--only a,b,prefix*] [--skip a,b]
#                      [--threads N] [--timeout SEC] [--keep-logs] [--list]
#
# Each benchmark runs once per allocator per repetition (allocators interleaved,
# order rotated) under `env LD_PRELOAD=<lib>` (glibc = no preload), measured by
# `/usr/bin/time -f '%e %M ...'`.  Outputs:
#   bench/out/results-<stamp>.csv   one row per run
#   bench/out/report.md             medians, normalised tables, geomeans, wins/losses
#   bench/out/run-<stamp>/          allocator snapshots, env.txt, logs of failed runs
# Build first with bench/build_bench.sh; sohamalloc comes from build/libsohamalloc.so
# (override with SOHAM_LIB=/path/to/lib.so).  See bench/README.md.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
case "${1:-}" in -h|--help) sed -n '2,16p' "$0"; exit 0;; esac
if [ ! -d "$HERE/build/mb" ]; then
  echo "bench/build/ is missing: run bench/build_bench.sh first" >&2
  exit 2
fi
exec python3 "$HERE/lib/runner.py" "$@"
