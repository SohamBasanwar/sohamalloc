# sigstorm: allocation from signal handlers

Each trial runs 1, 2 or 4 workload threads for up to 5 s while a signaller sends SIGUSR1 to each in turn every 0-40 us (uniformly random) and the handler calls the allocator as named; controls: `nosignal` (no signals at all) and `none` (signals, handler does not allocate). Outcomes: **survived**; **stall** = a thread made no progress for 2000 ms (deadlock or livelock); **corruption** = a block changed contents or two live blocks overlapped; **crash** = killed by a signal (SIGABRT: the allocator detected corruption and aborted). `P(fail)` is the fraction of trials that failed within the trial length, with a 95% Wilson interval; `TTF` is the time to failure of the failed trials (median and range, ms). `inside` counts handler calls that interrupted an allocator call of the workload (the exposure that tests signal safety). A trial whose own counters are impossible (the allocator overwrote the harness's records, which come from it) is counted as corruption.

## Summary: signal-handler kinds pooled (aligned), per thread count

| allocator | threads | trials | failed | P(fail) [95% CI] | median TTF (ms) | handler calls | inside an allocator call |
|---|---:|---:|---:|---|---:|---:|---:|
| glibc | 1 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 797 | 40 |
| glibc | 2 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 98435 | 97281 |
| glibc | 4 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 52412 | 49245 |
| glibc | all | 90 | 90 | 1.000 [0.959, 1.000] | 28 | 151644 | 146566 |
| jemalloc | 1 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 7421 | 153 |
| jemalloc | 2 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 62055 | 48067 |
| jemalloc | 4 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 169981 | 145938 |
| jemalloc | all | 90 | 90 | 1.000 [0.959, 1.000] | 28 | 239457 | 194158 |
| mimalloc | 1 | 30 | 25 | 0.833 [0.664, 0.927] | 2359 | 1197592 | 27863 |
| mimalloc | 2 | 30 | 30 | 1.000 [0.886, 1.000] | 1856 | 0 | 0 |
| mimalloc | 4 | 30 | 30 | 1.000 [0.886, 1.000] | 2383 | 2809 | 114 |
| mimalloc | all | 90 | 85 | 0.944 [0.876, 0.976] | 2286 | 1200401 | 27977 |
| tcmalloc | 1 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 9907 | 884 |
| tcmalloc | 2 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 17691 | 2747 |
| tcmalloc | 4 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 4883 | 821 |
| tcmalloc | all | 90 | 90 | 1.000 [0.959, 1.000] | 28 | 32481 | 4452 |
| tbbmalloc | 1 | 30 | 30 | 1.000 [0.886, 1.000] | 1132 | 6813 | 575 |
| tbbmalloc | 2 | 30 | 30 | 1.000 [0.886, 1.000] | 1148 | 4705 | 866 |
| tbbmalloc | 4 | 30 | 30 | 1.000 [0.886, 1.000] | 1920 | 2723 | 717 |
| tbbmalloc | all | 90 | 90 | 1.000 [0.959, 1.000] | 1152 | 14241 | 2158 |
| scudo | 1 | 30 | 10 | 0.333 [0.192, 0.512] | 2237 | 5592020 | 195795 |
| scudo | 2 | 30 | 14 | 0.467 [0.302, 0.639] | 1719 | 4905261 | 213492 |
| scudo | 4 | 30 | 15 | 0.500 [0.332, 0.668] | 1767 | 4986974 | 483376 |
| scudo | all | 90 | 39 | 0.433 [0.336, 0.536] | 1767 | 15484255 | 892663 |
| snmalloc | 1 | 30 | 16 | 0.533 [0.361, 0.698] | 1936 | 3734421 | 125688 |
| snmalloc | 2 | 30 | 30 | 1.000 [0.886, 1.000] | 1706 | 934768 | 399667 |
| snmalloc | 4 | 30 | 30 | 1.000 [0.886, 1.000] | 1228 | 478255 | 128774 |
| snmalloc | all | 90 | 76 | 0.844 [0.756, 0.905] | 1500 | 5147444 | 654129 |
| hardened_malloc | 1 | 30 | 30 | 1.000 [0.886, 1.000] | 29 | 33754 | 3783 |
| hardened_malloc | 2 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 104120 | 55143 |
| hardened_malloc | 4 | 30 | 30 | 1.000 [0.886, 1.000] | 28 | 130761 | 36706 |
| hardened_malloc | all | 90 | 90 | 1.000 [0.959, 1.000] | 28 | 268635 | 95632 |

## Failures / trials per allocator and handler (all thread counts)

| allocator | aligned |
|---|---:|
| glibc | 90/90 |
| jemalloc | 90/90 |
| mimalloc | 85/90 |
| tcmalloc | 90/90 |
| tbbmalloc | 90/90 |
| scudo | 39/90 |
| snmalloc | 76/90 |
| hardened_malloc | 90/90 |

## Handler results (all thread counts)

Handler calls that completed, and among them the allocations that returned NULL (`null`: the `free` handler counts an empty pool instead) and results that were wrong (`bad`: calloc memory not zero, realloc contents lost, aligned memory misaligned). Trials that failed are included up to their failure.

| allocator | handler | calls | null | bad |
|---|---|---:|---:|---:|
| glibc | aligned | 151644 | 0 | 0 |
| jemalloc | aligned | 239457 | 0 | 0 |
| mimalloc | aligned | 1200401 | 0 | 0 |
| tcmalloc | aligned | 32481 | 0 | 0 |
| tbbmalloc | aligned | 14241 | 0 | 0 |
| scudo | aligned | 15484255 | 0 | 0 |
| snmalloc | aligned | 5147444 | 0 | 0 |
| hardened_malloc | aligned | 268635 | 0 | 0 |

## Every cell

| allocator | threads | handler | trials | survived | stall | corruption | crash | P(fail) [95% CI] | TTF median [min, max] (ms) | inside/trial |
|---|---:|---|---:|---:|---:|---:|---:|---|---|---:|
| glibc | 1 | aligned | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [25, 28] | 1 |
| glibc | 2 | aligned | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [26, 28] | 3242 |
| glibc | 4 | aligned | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [7, 28] | 1641 |
| jemalloc | 1 | aligned | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [26, 48] | 5 |
| jemalloc | 2 | aligned | 30 | 0 | 27 | 0 | 3 | 1.00 [0.89, 1.00] | 28 [26, 1102] | 1602 |
| jemalloc | 4 | aligned | 30 | 0 | 26 | 0 | 4 | 1.00 [0.89, 1.00] | 28 [26, 1118] | 4864 |
| mimalloc | 1 | aligned | 30 | 5 | 0 | 0 | 25 | 0.83 [0.66, 0.93] | 2359 [1084, 7244] | 928 |
| mimalloc | 2 | aligned | 30 | 0 | 0 | 0 | 30 | 1.00 [0.89, 1.00] | 1856 [1100, 4590] | 0 |
| mimalloc | 4 | aligned | 30 | 0 | 0 | 1 | 29 | 1.00 [0.89, 1.00] | 2383 [60, 4232] | 3 |
| tcmalloc | 1 | aligned | 30 | 0 | 29 | 0 | 1 | 1.00 [0.89, 1.00] | 28 [26, 2020] | 29 |
| tcmalloc | 2 | aligned | 30 | 0 | 27 | 0 | 3 | 1.00 [0.89, 1.00] | 28 [26, 3036] | 91 |
| tcmalloc | 4 | aligned | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [26, 29] | 27 |
| tbbmalloc | 1 | aligned | 30 | 0 | 9 | 0 | 21 | 1.00 [0.89, 1.00] | 1132 [27, 3210] | 19 |
| tbbmalloc | 2 | aligned | 30 | 0 | 13 | 0 | 17 | 1.00 [0.89, 1.00] | 1148 [26, 4198] | 28 |
| tbbmalloc | 4 | aligned | 30 | 0 | 9 | 0 | 21 | 1.00 [0.89, 1.00] | 1920 [21, 4076] | 23 |
| scudo | 1 | aligned | 30 | 20 | 10 | 0 | 0 | 0.33 [0.19, 0.51] | 2237 [28, 4484] | 6526 |
| scudo | 2 | aligned | 30 | 16 | 10 | 4 | 0 | 0.47 [0.30, 0.64] | 1719 [27, 4665] | 7116 |
| scudo | 4 | aligned | 30 | 15 | 13 | 2 | 0 | 0.50 [0.33, 0.67] | 1767 [28, 4325] | 16112 |
| snmalloc | 1 | aligned | 30 | 14 | 0 | 8 | 8 | 0.53 [0.36, 0.70] | 1936 [60, 5839] | 4189 |
| snmalloc | 2 | aligned | 30 | 0 | 4 | 3 | 23 | 1.00 [0.89, 1.00] | 1706 [80, 5979] | 13322 |
| snmalloc | 4 | aligned | 30 | 0 | 4 | 5 | 21 | 1.00 [0.89, 1.00] | 1228 [32, 3052] | 4292 |
| hardened_malloc | 1 | aligned | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 29 [25, 129] | 126 |
| hardened_malloc | 2 | aligned | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [26, 128] | 1838 |
| hardened_malloc | 4 | aligned | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [26, 108] | 1223 |

## Failure details

- **glibc**: aligned stall: main:calloc handler:posix_memalign in_handler=1 (x14); aligned stall: main:malloc handler:posix_memalign in_handler=1 (x61); aligned stall: main:posix_memalign handler:posix_memalign in_handler=1 (x11); aligned stall: main:realloc handler:posix_memalign in_handler=1 (x4)
- **jemalloc**: aligned crash: SIGSEGV (x7); aligned stall: main:calloc handler:free in_handler=1 (x1); aligned stall: main:calloc handler:posix_memalign in_handler=1 (x14); aligned stall: main:free handler:posix_memalign in_handler=1 (x3); aligned stall: main:free-remote handler:posix_memalign in_handler=1 (x1); aligned stall: main:malloc handler:free in_handler=1 (x3); aligned stall: main:malloc handler:posix_memalign in_handler=1 (x50); aligned stall: main:none handler:free in_handler=1 (x1); aligned stall: main:posix_memalign handler:posix_memalign in_handler=1 (x4); aligned stall: main:realloc handler:posix_memalign in_handler=1 (x6)
- **mimalloc**: aligned corruption: content (x1); aligned crash: SIGSEGV (x84)
- **tcmalloc**: aligned crash: SIGSEGV (x4); aligned stall: main:calloc handler:posix_memalign in_handler=1 (x8); aligned stall: main:free handler:free in_handler=1 (x9); aligned stall: main:free handler:posix_memalign in_handler=1 (x1); aligned stall: main:malloc handler:free in_handler=1 (x1); aligned stall: main:malloc handler:posix_memalign in_handler=1 (x37); aligned stall: main:none handler:posix_memalign in_handler=1 (x20); aligned stall: main:posix_memalign handler:posix_memalign in_handler=1 (x4); aligned stall: main:realloc handler:free in_handler=1 (x2); aligned stall: main:realloc handler:posix_memalign in_handler=1 (x4)
- **tbbmalloc**: aligned crash: SIGSEGV (x59); aligned stall: main:calloc handler:posix_memalign in_handler=1 (x10); aligned stall: main:free handler:posix_memalign in_handler=1 (x2); aligned stall: main:malloc handler:posix_memalign in_handler=1 (x16); aligned stall: main:none handler:posix_memalign in_handler=1 (x3)
- **scudo**: aligned corruption: content (x6); aligned stall: main:calloc handler:posix_memalign in_handler=1 (x1); aligned stall: main:malloc handler:posix_memalign in_handler=1 (x19); aligned stall: main:none handler:posix_memalign in_handler=1 (x5); aligned stall: main:posix_memalign handler:posix_memalign in_handler=1 (x2); aligned stall: main:realloc handler:posix_memalign in_handler=1 (x6)
- **snmalloc**: aligned corruption: content (x16); aligned crash: SIGSEGV (x52); aligned stall: main:calloc handler:none in_handler=0 (x1); aligned stall: main:calloc handler:posix_memalign in_handler=1 (x1); aligned stall: main:malloc handler:none in_handler=0 (x2); aligned stall: main:malloc handler:posix_memalign in_handler=1 (x1); aligned stall: main:none handler:posix_memalign in_handler=1 (x1); aligned stall: main:realloc handler:posix_memalign in_handler=1 (x2)
- **hardened_malloc**: aligned stall: main:calloc handler:posix_memalign in_handler=1 (x17); aligned stall: main:free handler:posix_memalign in_handler=1 (x10); aligned stall: main:malloc handler:posix_memalign in_handler=1 (x49); aligned stall: main:none handler:posix_memalign in_handler=1 (x4); aligned stall: main:posix_memalign handler:posix_memalign in_handler=1 (x5); aligned stall: main:realloc handler:posix_memalign in_handler=1 (x5)

## Controls

0 control trials, 0 failed.
