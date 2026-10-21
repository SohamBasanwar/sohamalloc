#!/usr/bin/env python3
"""tools/sigstorm/run.py -- run sigstorm campaigns across allocators.

For every allocator x handler kind x trial it runs tools/sigstorm/sigstorm
under LD_PRELOAD (glibc: no preload), with a different seed per trial, and
records the outcome: survived, stall (deadlock or livelock), corruption, or
crash (killed by a signal: the allocator aborting on detected corruption is
SIGABRT, a wild access is SIGSEGV).  Allocators are interleaved within each
trial so drift in the machine's state affects all of them alike.

  tools/sigstorm/run.py [--allocs glibc,jemalloc,...] [--handlers malloc,free,...]
                        [--trials 10] [--seconds 10] [--threads 2]
                        [--interval-us 20] [--jobs 4] [--out DIR] [--list]

Outputs in DIR (default bench/out/sigstorm-<stamp>/):
  results.csv   one row per trial
  report.md     outcome counts, failure probability within the trial length
                with a 95% Wilson interval, time to first failure
  env.txt       machine, kernel, compiler and allocator versions
"""

import argparse
import concurrent.futures as cf
import csv
import datetime as dt
import glob
import math
import os
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LIBDIR = "/usr/lib/x86_64-linux-gnu"
EXT = os.path.join(ROOT, "bench", "build", "ext")


def newest(stem):
    c = [p for p in glob.glob(os.path.join(LIBDIR, stem + ".so.*"))
         if p.rsplit(".so.", 1)[1].isdigit()]
    return max(c, key=lambda p: int(p.rsplit(".so.", 1)[1])) if c else None


def first(*paths):
    for p in paths:
        for q in glob.glob(p):
            return q
    return None


ALLOCATORS = {
    "glibc": "",
    "jemalloc": newest("libjemalloc"),
    "mimalloc": newest("libmimalloc"),
    "tcmalloc": newest("libtcmalloc_minimal"),
    "tbbmalloc": newest("libtbbmalloc_proxy"),
    "scudo": first("/usr/lib/llvm-*/lib/clang/*/lib/linux/libclang_rt.scudo_standalone-x86_64.so"),
    "snmalloc": first(os.path.join(EXT, "snmalloc/build/libsnmallocshim.so")),
    "hardened_malloc": first(os.path.join(EXT, "hardened_malloc/out/libhardened_malloc.so")),
    "sohamalloc": os.environ.get("SOHAM_LIB", os.path.join(ROOT, "build", "libsohamalloc.so")),
}

HANDLERS = ["none", "malloc", "free", "realloc", "calloc", "large", "churn"]
FIELDS = ["trial", "alloc", "handler", "seed", "outcome", "detail", "elapsed_ms",
          "signals", "handlers", "handler_null", "handler_bad", "steps", "exit"]


def build(out_dir):
    exe = os.path.join(out_dir, "sigstorm")
    subprocess.run(["gcc", "-O2", "-g", "-Wall", "-Wextra", "-pthread",
                    os.path.join(HERE, "sigstorm.c"), "-o", exe], check=True)
    return exe


def parse(line):
    kv = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            kv[k] = v
    return kv


def run_one(exe, alloc, lib, handler, seed, a):
    env = dict(os.environ)
    env.pop("LD_PRELOAD", None)
    if lib:
        env["LD_PRELOAD"] = lib
    cmd = [exe, "--handler=" + handler, "--seconds=%d" % a.seconds, "--threads=%d" % a.threads,
           "--interval-us=%d" % a.interval_us, "--stall-ms=%d" % a.stall_ms, "--seed=%d" % seed]
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True,
                           timeout=a.seconds + a.stall_ms / 1000 + 20)
        rc, out = p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        rc, out = None, (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
    wall_ms = int((time.monotonic() - t0) * 1000)
    lines = [l for l in out.splitlines() if l.startswith("outcome=")]
    kv = parse(lines[-1]) if lines else {}
    row = {"alloc": alloc, "handler": handler, "seed": seed, "exit": rc}
    if rc is None:
        row.update(outcome="stall", detail="harness-timeout", elapsed_ms=wall_ms)
    elif rc < 0:
        row.update(outcome="crash", detail=signal.Signals(-rc).name, elapsed_ms=wall_ms)
    elif kv:
        row["outcome"] = kv.get("outcome", "unknown")
        det = [kv[k] for k in ("what", "op", "handler_op") if k in kv]
        if kv.get("outcome") == "stall":
            det = ["main:" + kv.get("op", "?"), "handler:" + kv.get("handler_op", "?"),
                   "in_handler=" + kv.get("in_handler", "?")]
        row["detail"] = " ".join(det)
        row["elapsed_ms"] = int(kv.get("elapsed_ms", wall_ms))
    else:
        row.update(outcome="error", detail="exit %d, no result line" % rc, elapsed_ms=wall_ms)
    for k in ("signals", "handlers", "handler_null", "handler_bad", "steps"):
        row[k] = kv.get(k, "")
    if row["outcome"] == "stall" and row.get("detail", "").startswith("main"):
        # a stall is only meaningful after the watchdog's window
        row["elapsed_ms"] = max(0, int(row["elapsed_ms"]) - a.stall_ms)
    return row


def wilson(k, n, z=1.96):
    if n == 0:
        return (0.0, 1.0)
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, c - h), min(1.0, c + h))


def env_text(a, allocs):
    def sh(cmd):
        try:
            return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()
        except Exception as e:  # noqa: BLE001
            return str(e)
    lines = ["date: " + dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
             "host: " + platform.node(),
             "cpu: " + sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-"),
             "cpus: %d" % os.cpu_count(),
             "kernel: " + platform.release(),
             "os: " + sh(". /etc/os-release; echo $PRETTY_NAME"),
             "gcc: " + sh("gcc --version | head -1"),
             "sohamalloc git: " + sh("git -C %s describe --always --dirty" % ROOT),
             "options: trials=%d seconds=%d threads=%d interval_us=%d stall_ms=%d" %
             (a.trials, a.seconds, a.threads, a.interval_us, a.stall_ms)]
    for name, lib in allocs:
        ver = ""
        if lib and lib.startswith("/usr/"):
            pkg = sh("dpkg -S %s 2>/dev/null | head -1 | cut -d: -f1" % os.path.realpath(lib))
            ver = sh("dpkg-query -W -f='${Package} ${Version}' %s 2>/dev/null" % pkg) if pkg else ""
        elif lib and lib.startswith(EXT):
            repo = os.path.join(EXT, os.path.relpath(lib, EXT).split(os.sep)[0])
            ver = "git " + sh("git -C %s log -1 --format='%%h %%cd' --date=short" % repo)
        elif name == "glibc":
            ver = sh("ldd --version | head -1")
        lines.append("allocator %s: %s %s" % (name, lib or "(system)", ver))
    return "\n".join(lines) + "\n"


def report(rows, a, allocs, path):
    out = ["# sigstorm: allocation from signal handlers", "",
           "Each trial runs %d workload threads for up to %d s while a signaller sends SIGUSR1 "
           "every 0-%d us (uniformly random) and the handler calls the allocator as named. "
           "Outcomes: **survived**; **stall** = a thread made no progress for %d ms (deadlock or "
           "livelock); **corruption** = a block changed contents or two live blocks overlapped; "
           "**crash** = killed by a signal (SIGABRT: the allocator detected corruption and "
           "aborted). `P(fail)` is the fraction of trials that failed within the trial length, "
           "with a 95%% Wilson interval; `median TTF` is the median time to failure of the "
           "failed trials." % (a.threads, a.seconds, 2 * a.interval_us, a.stall_ms), ""]
    hs = [h for h in a.handlers]
    out.append("## Outcome per allocator and handler")
    out.append("")
    out.append("| allocator | handler | trials | survived | stall | corruption | crash | P(fail) [95% CI] | median TTF (ms) | handler calls/trial |")
    out.append("|---|---|---:|---:|---:|---:|---:|---|---:|---:|")
    for name, _ in allocs:
        for h in hs:
            rs = [r for r in rows if r["alloc"] == name and r["handler"] == h]
            if not rs:
                continue
            n = len(rs)
            c = {k: sum(1 for r in rs if r["outcome"] == k) for k in ("survived", "stall", "corruption", "crash")}
            fails = [r for r in rs if r["outcome"] != "survived"]
            lo, hi = wilson(len(fails), n)
            ttf = statistics.median(int(r["elapsed_ms"]) for r in fails) if fails else None
            calls = [int(r["handlers"]) for r in rs if str(r.get("handlers", "")).isdigit()]
            out.append("| %s | %s | %d | %d | %d | %d | %d | %.2f [%.2f, %.2f] | %s | %s |" % (
                name, h, n, c["survived"], c["stall"], c["corruption"], c["crash"],
                len(fails) / n, lo, hi, "%d" % ttf if ttf is not None else "-",
                "%d" % statistics.median(calls) if calls else "-"))
    out.append("")
    out.append("## Failure details")
    out.append("")
    for name, _ in allocs:
        rs = [r for r in rows if r["alloc"] == name and r["outcome"] != "survived"]
        if not rs:
            continue
        kinds = {}
        for r in rs:
            key = "%s %s: %s" % (r["handler"], r["outcome"], r.get("detail", ""))
            kinds[key] = kinds.get(key, 0) + 1
        out.append("- **%s**: " % name + "; ".join("%s (x%d)" % (k, v) for k, v in sorted(kinds.items())))
    out.append("")
    with open(path, "w") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--allocs", default=",".join(ALLOCATORS))
    ap.add_argument("--handlers", default=",".join(HANDLERS))
    ap.add_argument("--trials", type=int, default=10)
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--interval-us", type=int, default=20)
    ap.add_argument("--stall-ms", type=int, default=2000)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--out", default=None)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    a.handlers = [h for h in a.handlers.split(",") if h]
    allocs = []
    for name in a.allocs.split(","):
        if name not in ALLOCATORS:
            sys.exit("unknown allocator %s (known: %s)" % (name, ", ".join(ALLOCATORS)))
        lib = ALLOCATORS[name]
        if name != "glibc" and (not lib or not os.path.exists(lib)):
            print("skip %s: library not found" % name, file=sys.stderr)
            continue
        allocs.append((name, lib))
    if a.list:
        for name, lib in allocs:
            print("%-16s %s" % (name, lib or "(system allocator)"))
        return
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = a.out or os.path.join(ROOT, "bench", "out", "sigstorm-" + stamp)
    os.makedirs(out_dir, exist_ok=True)
    exe = build(out_dir)
    with open(os.path.join(out_dir, "env.txt"), "w") as f:
        f.write(env_text(a, allocs))
    shutil.copy(os.path.join(HERE, "sigstorm.c"), out_dir)

    jobs = []
    for t in range(a.trials):
        order = allocs[t % len(allocs):] + allocs[:t % len(allocs)]
        for name, lib in order:
            for h in a.handlers:
                jobs.append((t, name, lib, h, 1000 + t))
    rows = []
    csv_path = os.path.join(out_dir, "results.csv")
    with open(csv_path, "w", newline="") as f, cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        futs = {ex.submit(run_one, exe, name, lib, h, seed, a): (t, name, h) for (t, name, lib, h, seed) in jobs}
        done = 0
        for fu in cf.as_completed(futs):
            t, name, h = futs[fu]
            r = fu.result()
            r["trial"] = t
            rows.append(r)
            w.writerow({k: r.get(k, "") for k in FIELDS})
            f.flush()
            done += 1
            print("[%4d/%d] trial %d %-16s %-8s %-10s %6s ms  %s" % (
                done, len(jobs), t, name, h, r["outcome"], r.get("elapsed_ms", ""), r.get("detail", "")), flush=True)
    rows.sort(key=lambda r: (r["alloc"], r["handler"], r["trial"]))
    report(rows, a, allocs, os.path.join(out_dir, "report.md"))
    print("results: " + csv_path)
    print("report:  " + os.path.join(out_dir, "report.md"))


if __name__ == "__main__":
    main()
