#!/usr/bin/env python3
"""tools/sigstorm/run.py -- run sigstorm campaigns across allocators.

For every allocator x thread count x handler kind x trial it runs
tools/sigstorm/sigstorm under LD_PRELOAD (glibc: no preload), with a
different seed per trial, and records the outcome: survived, stall (deadlock
or livelock), corruption, or crash (killed by a signal: the allocator
aborting on detected corruption is SIGABRT, a wild access is SIGSEGV).
Allocators are interleaved within each trial so drift in the machine's state
affects all of them alike.

Handler kinds are those of sigstorm, plus the control "nosignal" (sigstorm
--interval-us=0: the workload alone, no signals).  "none" is the other
control: signals whose handler does not allocate.

  tools/sigstorm/run.py [--allocs glibc,jemalloc,...] [--handlers malloc,free,...]
                        [--trials 10] [--seconds 10] [--threads 1,2,4]
                        [--interval-us 20] [--jobs 4] [--out DIR] [--list]
  tools/sigstorm/run.py --resume DIR      finish an interrupted campaign
  tools/sigstorm/run.py --report-only DIR rewrite DIR/report.md

Outputs in DIR (default bench/out/sigstorm-<stamp>/):
  results.csv   one row per trial
  report.md     outcome counts, failure probability within the trial length
                with a 95% Wilson interval, time to first failure, handler
                calls that interrupted an allocator call
  env.txt       machine, kernel, compiler, allocator versions and checksums,
                load average at start and end
  args.txt      the options, so --resume repeats them
"""

import argparse
import concurrent.futures as cf
import csv
import datetime as dt
import glob
import hashlib
import json
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

HANDLERS = ["nosignal", "none", "malloc", "free", "realloc", "calloc", "large", "churn"]
CONTROLS = ("nosignal", "none")
FIELDS = ["trial", "alloc", "threads", "handler", "seed", "outcome", "detail", "elapsed_ms",
          "signals", "handlers", "inside", "handler_null", "handler_bad", "steps", "exit"]


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


def run_one(exe, alloc, lib, handler, threads, seed, a):
    env = dict(os.environ)
    env.pop("LD_PRELOAD", None)
    if lib:
        env["LD_PRELOAD"] = lib
    interval = 0 if handler == "nosignal" else a.interval_us
    cmd = [exe, "--handler=" + ("none" if handler == "nosignal" else handler), "--seconds=%d" % a.seconds,
           "--threads=%d" % threads, "--interval-us=%d" % interval, "--stall-ms=%d" % a.stall_ms,
           "--seed=%d" % seed]
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
    row = {"alloc": alloc, "threads": threads, "handler": handler, "seed": seed, "exit": rc}
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
    row["inside"] = kv.get("handler_inside", "")
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


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()
    except Exception as e:  # noqa: BLE001
        return str(e)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def stamp_line(what):
    return "%s: %s loadavg %s" % (what, dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                                  open("/proc/loadavg").read().split(" ")[0:3])


def env_text(a, allocs):
    lines = ["date: " + dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
             "host: " + platform.node(),
             "cpu: " + sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-"),
             "cpus: %d" % os.cpu_count(),
             "kernel: " + platform.release(),
             "os: " + sh(". /etc/os-release; echo $PRETTY_NAME"),
             "gcc: " + sh("gcc --version | head -1"),
             "sohamalloc git: " + sh("git -C %s describe --always --dirty" % ROOT),
             "options: trials=%d seconds=%d threads=%s interval_us=%d stall_ms=%d jobs=%d" %
             (a.trials, a.seconds, ",".join(map(str, a.threads)), a.interval_us, a.stall_ms, a.jobs),
             stamp_line("start")]
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
        if lib:
            lines.append("  sha256 %s" % sha256(os.path.realpath(lib)))
    return "\n".join(lines) + "\n"


def num(r, k):
    v = str(r.get(k, ""))
    return int(v) if v.isdigit() else 0


def cell(rs):
    """Summary of a set of trials: n, outcome counts, failures, Wilson CI, TTF."""
    n = len(rs)
    c = {k: sum(1 for r in rs if r["outcome"] == k) for k in ("survived", "stall", "corruption", "crash")}
    fails = [r for r in rs if r["outcome"] != "survived"]
    lo, hi = wilson(len(fails), n)
    ttf = sorted(int(r["elapsed_ms"]) for r in fails if str(r.get("elapsed_ms", "")).lstrip("-").isdigit())
    return {"n": n, "c": c, "f": len(fails), "lo": lo, "hi": hi, "ttf": ttf,
            "calls": sum(num(r, "handlers") for r in rs), "inside": sum(num(r, "inside") for r in rs),
            "other": n - sum(c.values())}


def report(rows, a, path):
    allocs = sorted({r["alloc"] for r in rows}, key=lambda x: list(ALLOCATORS).index(x) if x in ALLOCATORS else 99)
    hs = [h for h in HANDLERS if any(r["handler"] == h for r in rows)]
    ts = sorted({int(r["threads"]) for r in rows})
    sig = [h for h in hs if h not in CONTROLS]
    out = ["# sigstorm: allocation from signal handlers", "",
           "Each trial runs 1, 2 or 4 workload threads for up to %d s while a signaller sends SIGUSR1 "
           "to each in turn every 0-%d us (uniformly random) and the handler calls the allocator as "
           "named; controls: `nosignal` (no signals at all) and `none` (signals, handler does not "
           "allocate). Outcomes: **survived**; **stall** = a thread made no progress for %d ms "
           "(deadlock or livelock); **corruption** = a block changed contents or two live blocks "
           "overlapped; **crash** = killed by a signal (SIGABRT: the allocator detected corruption "
           "and aborted). `P(fail)` is the fraction of trials that failed within the trial length, "
           "with a 95%% Wilson interval; `TTF` is the time to failure of the failed trials (median "
           "and range, ms). `inside` counts handler calls that interrupted an allocator call of the "
           "workload (the exposure that tests signal safety)." % (a.seconds, 2 * a.interval_us, a.stall_ms), ""]
    out += ["## Summary: signal-handler kinds pooled (%s), per thread count" % ", ".join(sig), "",
            "| allocator | threads | trials | failed | P(fail) [95% CI] | median TTF (ms) | handler calls | inside an allocator call |",
            "|---|---:|---:|---:|---|---:|---:|---:|"]
    for name in allocs:
        for t in ts + ["all"]:
            rs = [r for r in rows if r["alloc"] == name and r["handler"] in sig and (t == "all" or int(r["threads"]) == t)]
            if not rs:
                continue
            x = cell(rs)
            out.append("| %s | %s | %d | %d | %.3f [%.3f, %.3f] | %s | %d | %d |" % (
                name, t, x["n"], x["f"], x["f"] / x["n"], x["lo"], x["hi"],
                "%d" % statistics.median(x["ttf"]) if x["ttf"] else "-", x["calls"], x["inside"]))
    out += ["", "## Failures / trials per allocator and handler (all thread counts)", "",
            "| allocator | " + " | ".join(hs) + " |", "|---|" + "---:|" * len(hs)]
    for name in allocs:
        cells = []
        for h in hs:
            rs = [r for r in rows if r["alloc"] == name and r["handler"] == h]
            cells.append("%d/%d" % (cell(rs)["f"], len(rs)) if rs else "-")
        out.append("| %s | %s |" % (name, " | ".join(cells)))
    out += ["", "## Every cell", "",
            "| allocator | threads | handler | trials | survived | stall | corruption | crash | P(fail) [95% CI] | TTF median [min, max] (ms) | inside/trial |",
            "|---|---:|---|---:|---:|---:|---:|---:|---|---|---:|"]
    for name in allocs:
        for t in ts:
            for h in hs:
                rs = [r for r in rows if r["alloc"] == name and r["handler"] == h and int(r["threads"]) == t]
                if not rs:
                    continue
                x = cell(rs)
                ttf = "%d [%d, %d]" % (statistics.median(x["ttf"]), x["ttf"][0], x["ttf"][-1]) if x["ttf"] else "-"
                out.append("| %s | %d | %s | %d | %d | %d | %d | %d | %.2f [%.2f, %.2f] | %s | %d |" % (
                    name, t, h, x["n"], x["c"]["survived"], x["c"]["stall"], x["c"]["corruption"], x["c"]["crash"],
                    x["f"] / x["n"], x["lo"], x["hi"], ttf, x["inside"] // max(1, x["n"])))
    out += ["", "## Failure details", ""]
    for name in allocs:
        rs = [r for r in rows if r["alloc"] == name and r["outcome"] != "survived"]
        if not rs:
            continue
        kinds = {}
        for r in rs:
            key = "%s %s: %s" % (r["handler"], r["outcome"], r.get("detail", ""))
            kinds[key] = kinds.get(key, 0) + 1
        out.append("- **%s**: " % name + "; ".join("%s (x%d)" % (k, v) for k, v in sorted(kinds.items())))
    bad_controls = [r for r in rows if r["handler"] in CONTROLS and r["outcome"] != "survived"]
    out += ["", "## Controls", "",
            "%d control trials, %d failed%s." % (
                sum(1 for r in rows if r["handler"] in CONTROLS), len(bad_controls),
                "" if not bad_controls else " (the harness or the machine is suspect for those cells)"), ""]
    with open(path, "w") as f:
        f.write("\n".join(out))


def load_rows(csv_path):
    with open(csv_path, newline="") as f:
        return [r for r in csv.DictReader(f)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--allocs", default=",".join(ALLOCATORS))
    ap.add_argument("--handlers", default=",".join(HANDLERS))
    ap.add_argument("--trials", type=int, default=10)
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--threads", default="1,2,4")
    ap.add_argument("--interval-us", type=int, default=20)
    ap.add_argument("--stall-ms", type=int, default=2000)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--out", default=None)
    ap.add_argument("--resume", default=None, metavar="DIR")
    ap.add_argument("--report-only", default=None, metavar="DIR")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.report_only:
        with open(os.path.join(a.report_only, "args.txt")) as f:
            saved = json.load(f)
        a.seconds, a.interval_us, a.stall_ms = saved["seconds"], saved["interval_us"], saved["stall_ms"]
        report(load_rows(os.path.join(a.report_only, "results.csv")), a, os.path.join(a.report_only, "report.md"))
        print("report:  " + os.path.join(a.report_only, "report.md"))
        return
    if a.resume:
        with open(os.path.join(a.resume, "args.txt")) as f:
            saved = json.load(f)
        jobs_now = a.jobs
        for k, v in saved.items():
            setattr(a, k, v)
        a.jobs, a.out = jobs_now, a.resume
    else:
        a.threads = [int(t) for t in str(a.threads).split(",") if t]
        a.handlers = [h for h in a.handlers.split(",") if h]
    for h in a.handlers:
        if h not in HANDLERS:
            sys.exit("unknown handler kind %s (known: %s)" % (h, ", ".join(HANDLERS)))
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
    csv_path = os.path.join(out_dir, "results.csv")
    exe = os.path.join(out_dir, "sigstorm")
    done_keys = set()
    if a.resume:
        for r in load_rows(csv_path):
            done_keys.add((int(r["trial"]), r["alloc"], int(r["threads"]), r["handler"]))
        with open(os.path.join(out_dir, "env.txt"), "a") as f:
            f.write(stamp_line("resumed (%d trials already done)" % len(done_keys)) + "\n")
    else:
        exe = build(out_dir)
        with open(os.path.join(out_dir, "env.txt"), "w") as f:
            f.write(env_text(a, allocs))
        with open(os.path.join(out_dir, "args.txt"), "w") as f:
            json.dump({k: getattr(a, k) for k in ("allocs", "handlers", "trials", "seconds", "threads",
                                                    "interval_us", "stall_ms")}, f)
            f.write("\n")
        shutil.copy(os.path.join(HERE, "sigstorm.c"), out_dir)

    jobs = []
    for t in range(a.trials):
        order = allocs[t % len(allocs):] + allocs[:t % len(allocs)]
        for name, lib in order:
            for th in a.threads:
                for h in a.handlers:
                    if (t, name, th, h) not in done_keys:
                        jobs.append((t, name, lib, th, h, 1000 + t))
    new_file = not os.path.exists(csv_path) or os.path.getsize(csv_path) == 0
    with open(csv_path, "a", newline="") as f, cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if new_file:
            w.writeheader()
        futs = {ex.submit(run_one, exe, name, lib, h, th, seed, a): (t, name, th, h)
                for (t, name, lib, th, h, seed) in jobs}
        done = 0
        for fu in cf.as_completed(futs):
            t, name, th, h = futs[fu]
            r = fu.result()
            r["trial"] = t
            w.writerow({k: r.get(k, "") for k in FIELDS})
            f.flush()
            done += 1
            print("[%4d/%d] trial %d %-16s t=%d %-8s %-10s %6s ms  %s" % (
                done, len(jobs), t, name, th, h, r["outcome"], r.get("elapsed_ms", ""), r.get("detail", "")),
                flush=True)
    with open(os.path.join(out_dir, "env.txt"), "a") as f:
        f.write(stamp_line("end") + "\n")
    rows = load_rows(csv_path)
    rows.sort(key=lambda r: (r["alloc"], int(r["threads"]), r["handler"], int(r["trial"])))
    report(rows, a, os.path.join(out_dir, "report.md"))
    print("results: " + csv_path)
    print("report:  " + os.path.join(out_dir, "report.md"))


if __name__ == "__main__":
    main()
