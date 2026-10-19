# Handoff: continuing sohamalloc on the laptop

If `claude --teleport` brought the conversation over, you can skip this file. Otherwise start `claude` in `~/sohamalloc` and paste the prompt at the end.

## Where the project stands (2026-10-19)

- **Code.** sohamalloc v0.1 is complete: C11, written from scratch, on branch `sohamalloc-dev`.
  - Design as built: `docs/IMPLEMENTATION_NOTES.md`. Status: `docs/STATUS.md`.
- **Tests.** Every target passed:

  | Target | Result |
  |---|---|
  | x86-64 | 15/15 |
  | debug build | 15/15 |
  | aarch64 | 15/15 |
  | armhf | 15/15 |
  | musl | 14 + 1 skipped |
  | Windows | 8/8 |
  | TSan, ASan, UBSan | 8/8 each |
  | real programs under preload | 9/9 |

- **Benchmarks.** Measured on a 4-vCPU cloud VM (noisy), 24 workloads. sohamalloc's combined score is **1.142**, tied first with mimalloc; glibc is 1.276, jemalloc 1.344 and tcmalloc 1.451.
  - It is the only allocator never the worst on memory.
  - The single busy-byte fast path (`13bcf17`) has not been benchmarked yet.
- **Research log.** `docs/research-log/01-timeline.md` (full chronology) and `05-novelty-analysis.json` (per-mechanism novelty verdicts, 50 references).
  - Most mechanisms are known individually.
  - The candidates are async-signal-safe malloc/free at full speed through the busy byte, and the overall combination.
- **Authorship.** Commits are authored by Soham Basanwar with no AI trailers. Keep it that way: no `Co-Authored-By` lines.

## The plan: thesis "Async-signal-safe memory allocation, made practical"

1. **Signal-injection tester** (`tools/sigfuzz/`). It interrupts a target thread inside `malloc`/`free` at every instruction and calls `malloc`/`free` from the signal handler, flagging deadlocks (timeout) and corruption (checksums, heap checks).
   - Possible mechanisms: ptrace single-stepping, or perf breakpoints over the allocator's text range, with sampled instruction offsets.
   - Run it against glibc, jemalloc, tcmalloc, mimalloc, snmalloc, Scudo, hardened_malloc and sohamalloc. This is an empirical study nobody has published.
2. **Real-world evidence.** Document software that allocates inside signal handlers (profilers, crash reporters, language runtimes, logging libraries), with public bug reports of the resulting deadlocks.
3. **Model-check the reentrancy and lock-free protocols** (GenMC or TLA+): the busy discipline, remote free, WATCH and threshold, detach and claim.
4. **Rigorous evaluation on this laptop.**
   - Tune with `tools/bench-host.sh` first.
   - 10–30 repetitions with confidence intervals.
   - Code-layout variation (link order, environment size).
   - Pin to P-cores, and separately study P- against E-core behaviour.
   - Then a many-core server run for scalability.
5. **Also needed before writing:**
   - finish `docs/research-log/02-experiments.md`, `03-defects-and-faults.md`, `04-design-decisions.md` and `06-paper-assessment.md`;
   - re-run the four-lens adversarial code review (concurrency, correctness/API, security, kernel/perf/portability, with refutation-based verification) and fix what it confirms.

## Prompt to paste into a fresh session

> Continue the sohamalloc project in this repository (branch sohamalloc-dev). Read docs/HANDOFF.md, docs/STATUS.md and docs/IMPLEMENTATION_NOTES.md first. Commit as Soham Basanwar with no AI co-author trailers.
>
> Start by running `tools/bench-host.sh status` and the full test suite on this machine (`tests/run_tests.sh --lib build/libsohamalloc.so`). Then re-run the four-lens adversarial review and fix what it confirms. Then follow the plan in HANDOFF.md, beginning with the signal-injection tester.
