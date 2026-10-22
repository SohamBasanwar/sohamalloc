# Prototype: handler-context fallback for large requests (work in progress, 2026-10-22)

**Status.** Not applied to `src/`. This is a prototype to review together with any round-4 fixes, and it then needs the verification matrix and a review round. Made 2026-10-22 ~17:40 UTC on top of `fd735dd`.

**Problem.** Measured in sigstorm campaign 1, not a review finding. A handler allocation that interrupted the allocator, and that the shared heap cannot serve because its own thread holds the page-heap lock, falls back to the static arena. The arena is 384 KiB and never reused. The first 300 KB fallback takes most of it, and every later large fallback returns NULL.

- In campaign 1 (`c1a9791`), 2–3% of the `large` handler calls that interrupted an allocator call returned NULL.
- This is within documented limit 1 of IMPLEMENTATION_NOTES §10, but it limits practicality.

**Change** (`handler-solo-fallback.patch`, against `src/api.c` at `fd735dd`):

- Requests above 16 KiB never use the static arena.
- When the shared heap cannot serve a request, and the arena cannot either, the handler takes a mapping of its own: a solo, whose table is taken by trylock (trymode, so H3 holds).
- trymode is saved and restored, because a nested handler may already be in it.

**Measured** on this laptop: sigstorm `--handler=large`, 5 s, seed 11.

| Build | Threads 1 / 2 / 4: NULL results |
|---|---|
| `fd735dd` release (the frozen review library) | 95 / 143 / 115 |
| prototype, release | 0 / 0 / 0 |
| prototype, debug (H3 assertion active) | 0 / 0 / 0 |

**Prototype debug build, the other handler kinds:** malloc, free, realloc, calloc, churn, aligned and none, at 1 and 4 threads for 3 s each, plus churn, aligned and large with `--workload=page`. All survived, with 0 NULL allocations; the `free` handler's "null" counts mean an empty pool.

**Partial rerun of `fd735dd`** (`../sigstorm-c1-soham-fd735dd-partial/`): it shows the same NULLs: `large` 6743, `churn` 70, `aligned` 39, in 456 trials.

**Regression test** `signal_large_fallback.c` (for `tests/regress/`): written, but **not yet run** against either build. Next steps:

1. Confirm it fails on `fd735dd` and passes with the patch.
2. Register it in `tests/run_tests.sh`.
3. Update IMPLEMENTATION_NOTES §10, limit 1.
