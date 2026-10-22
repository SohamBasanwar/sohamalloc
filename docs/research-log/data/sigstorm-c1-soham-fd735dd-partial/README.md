# sohamalloc rerun of campaign 1 on the fixed code `fd735dd` (partial, stopped)

**What it is.** Campaign 1's design for sohamalloc only:

- the 8 handler kinds of campaign 1 plus `aligned`, × 1, 2 and 4 threads × 30 trials of 5 s, which is 810 trials;
- the frozen library `bench/out/sigstorm-c1-soham-fd735dd/libsohamalloc-fd735dd.so`, sha256 `77ab3a6b…`, built from `fd735dd`;
- the tool `tools/sigstorm` at commit `f3809c0`, which adds the `aligned` kind.

**Timeline:**

| When (UTC) | Event |
|---|---|
| 2026-10-21 ~21:46 | started |
| overnight | killed by a reboot at 168 trials |
| 2026-10-22 17:33 | resumed |
| 2026-10-22 ~17:50 | stopped at the author's request, at **456 of 810 trials** |

**Interim result** (not final): 456 of 456 trials survived. Handler NULL results, not counting the `free` handler's empty-pool counts: large 6743, churn 70, aligned 39, all others 0.

**To finish it:**

```sh
SOHAM_LIB=$PWD/bench/out/sigstorm-c1-soham-fd735dd/libsohamalloc-fd735dd.so \
  python3 tools/sigstorm/run.py --resume bench/out/sigstorm-c1-soham-fd735dd --jobs 4
```

The library and the binary are in the git-ignored `bench/out/` directory. If they are gone, rebuild `fd735dd` and check the sha256.
