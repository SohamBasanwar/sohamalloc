#!/usr/bin/env bash
# tools/bench-host.sh -- put a Linux laptop or server into a stable state for
# benchmark campaigns, and back.
#
#   tools/bench-host.sh status            show governor, turbo, power profile, perf access, topology
#   sudo tools/bench-host.sh tune         performance governor + EPP, turbo off, perf access
#   sudo tools/bench-host.sh tune --turbo same, but leave turbo on (report it in the paper)
#   sudo tools/bench-host.sh restore      put back the values saved by the last 'tune'
#
# Turbo off trades peak speed for repeatable numbers: with turbo on, a laptop's
# clock depends on temperature and on how long it has been busy.  Keep the
# machine on AC power, lid open, and quiet (no browser, no updates) while a
# campaign runs.  ASLR stays on: layout variation is part of what repetitions
# have to average over.

set -euo pipefail
SAVE=/var/tmp/sohamalloc-bench-host.saved
CPUS=(/sys/devices/system/cpu/cpu[0-9]*)

rd() { cat "$1" 2>/dev/null || echo "n/a"; }

status() {
    echo "governor:    $(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
    echo "EPP:         $(rd /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference)"
    echo "turbo off:   $(rd /sys/devices/system/cpu/intel_pstate/no_turbo)  (intel_pstate; 1 = turbo disabled)"
    echo "boost:       $(rd /sys/devices/system/cpu/cpufreq/boost)  (acpi-cpufreq/amd; 0 = disabled)"
    echo "SMT:         $(rd /sys/devices/system/cpu/smt/active)"
    echo "perf access: kernel.perf_event_paranoid = $(rd /proc/sys/kernel/perf_event_paranoid)  (<= 1 lets perf read hardware counters)"
    command -v powerprofilesctl >/dev/null && echo "profile:     $(powerprofilesctl get 2>/dev/null || echo n/a)"
    for s in /sys/class/power_supply/A*/online; do [ -r "$s" ] && echo "AC power:    $(cat "$s")"; done
    for t in cpu_core cpu_atom; do [ -r /sys/devices/$t/cpus ] && echo "$t CPUs:  $(cat /sys/devices/$t/cpus)"; done
    echo "THP:         $(rd /sys/kernel/mm/transparent_hugepage/enabled)"
    echo
    echo "Pin a benchmark to the performance cores with: taskset -c \$(cat /sys/devices/cpu_core/cpus) <command>"
}

need_root() { [ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }; }

tune() {
    need_root
    local keep_turbo=0; [ "${1:-}" = --turbo ] && keep_turbo=1
    {
        for c in "${CPUS[@]}"; do
            [ -w "$c/cpufreq/scaling_governor" ] && echo "$c/cpufreq/scaling_governor $(cat "$c/cpufreq/scaling_governor")"
            [ -w "$c/cpufreq/energy_performance_preference" ] && echo "$c/cpufreq/energy_performance_preference $(cat "$c/cpufreq/energy_performance_preference")"
        done
        for f in /sys/devices/system/cpu/intel_pstate/no_turbo /sys/devices/system/cpu/cpufreq/boost /proc/sys/kernel/perf_event_paranoid; do
            [ -w "$f" ] && echo "$f $(cat "$f")"
        done
    } > "$SAVE"
    command -v powerprofilesctl >/dev/null && echo "powerprofilesctl $(powerprofilesctl get)" >> "$SAVE" && powerprofilesctl set performance || true
    for c in "${CPUS[@]}"; do
        [ -w "$c/cpufreq/scaling_governor" ] && echo performance > "$c/cpufreq/scaling_governor" || true
        [ -w "$c/cpufreq/energy_performance_preference" ] && echo performance > "$c/cpufreq/energy_performance_preference" 2>/dev/null || true
    done
    if [ "$keep_turbo" = 0 ]; then
        [ -w /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo || true
        [ -w /sys/devices/system/cpu/cpufreq/boost ] && echo 0 > /sys/devices/system/cpu/cpufreq/boost || true
    fi
    echo 1 > /proc/sys/kernel/perf_event_paranoid
    echo "tuned (previous values saved in $SAVE)"; echo; status
}

restore() {
    need_root
    [ -r "$SAVE" ] || { echo "nothing saved ($SAVE missing)" >&2; exit 1; }
    while read -r f v; do
        if [ "$f" = powerprofilesctl ]; then powerprofilesctl set "$v" || true; else echo "$v" > "$f" 2>/dev/null || true; fi
    done < "$SAVE"
    rm -f "$SAVE"
    echo "restored"; echo; status
}

case "${1:-status}" in
    status) status ;;
    tune) shift; tune "$@" ;;
    restore) restore ;;
    *) sed -n 2,15p "$0"; exit 2 ;;
esac
