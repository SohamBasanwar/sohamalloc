# sigstorm: allocation from signal handlers

Each trial runs 1, 2 or 4 workload threads for up to 5 s while a signaller sends SIGUSR1 to each in turn every 0-40 us (uniformly random) and the handler calls the allocator as named; controls: `nosignal` (no signals at all) and `none` (signals, handler does not allocate). Outcomes: **survived**; **stall** = a thread made no progress for 2000 ms (deadlock or livelock); **corruption** = a block changed contents or two live blocks overlapped; **crash** = killed by a signal (SIGABRT: the allocator detected corruption and aborted). `P(fail)` is the fraction of trials that failed within the trial length, with a 95% Wilson interval; `TTF` is the time to failure of the failed trials (median and range, ms). `inside` counts handler calls that interrupted an allocator call of the workload (the exposure that tests signal safety). A trial whose own counters are impossible (the allocator overwrote the harness's records, which come from it) is counted as corruption.

## Summary: signal-handler kinds pooled (malloc, free, realloc, calloc, large, churn), per thread count

| allocator | threads | trials | failed | P(fail) [95% CI] | median TTF (ms) | handler calls | inside an allocator call |
|---|---:|---:|---:|---|---:|---:|---:|
| glibc | 1 | 180 | 157 | 0.872 [0.816, 0.913] | 28 | 5519713 | 163327 |
| glibc | 2 | 180 | 159 | 0.883 [0.828, 0.922] | 28 | 6653359 | 1283187 |
| glibc | 4 | 180 | 168 | 0.933 [0.887, 0.961] | 27 | 5000430 | 1271659 |
| glibc | all | 540 | 484 | 0.896 [0.868, 0.919] | 28 | 17173502 | 2718173 |
| jemalloc | 1 | 180 | 150 | 0.833 [0.772, 0.881] | 69 | 9942128 | 425868 |
| jemalloc | 2 | 180 | 153 | 0.850 [0.791, 0.895] | 88 | 11529253 | 1954388 |
| jemalloc | 4 | 180 | 165 | 0.917 [0.867, 0.949] | 126 | 8156044 | 1172008 |
| jemalloc | all | 540 | 468 | 0.867 [0.835, 0.893] | 103 | 29627425 | 3552264 |
| mimalloc | 1 | 180 | 100 | 0.556 [0.483, 0.626] | 1918 | 20629869 | 557525 |
| mimalloc | 2 | 180 | 128 | 0.711 [0.641, 0.772] | 1604 | 13067614 | 382291 |
| mimalloc | 4 | 180 | 129 | 0.717 [0.647, 0.777] | 1620 | 11963179 | 349469 |
| mimalloc | all | 540 | 357 | 0.661 [0.620, 0.700] | 1668 | 45660662 | 1289285 |
| tcmalloc | 1 | 180 | 180 | 1.000 [0.979, 1.000] | 48 | 385233 | 15772 |
| tcmalloc | 2 | 180 | 180 | 1.000 [0.979, 1.000] | 48 | 2647478 | 2468447 |
| tcmalloc | 4 | 180 | 180 | 1.000 [0.979, 1.000] | 46 | 2999255 | 2907142 |
| tcmalloc | all | 540 | 540 | 1.000 [0.993, 1.000] | 48 | 6031966 | 5391361 |
| tbbmalloc | 1 | 180 | 90 | 0.500 [0.428, 0.572] | 540 | 20758762 | 614486 |
| tbbmalloc | 2 | 180 | 90 | 0.500 [0.428, 0.572] | 444 | 20474484 | 680495 |
| tbbmalloc | 4 | 180 | 90 | 0.500 [0.428, 0.572] | 1177 | 18443892 | 759851 |
| tbbmalloc | all | 540 | 270 | 0.500 [0.458, 0.542] | 560 | 59677138 | 2054832 |
| scudo | 1 | 180 | 107 | 0.594 [0.521, 0.663] | 129 | 19941651 | 688230 |
| scudo | 2 | 180 | 114 | 0.633 [0.561, 0.700] | 227 | 19221173 | 1915135 |
| scudo | 4 | 180 | 121 | 0.672 [0.601, 0.737] | 209 | 16842183 | 2642978 |
| scudo | all | 540 | 342 | 0.633 [0.592, 0.673] | 189 | 56005007 | 5246343 |
| snmalloc | 1 | 180 | 121 | 0.672 [0.601, 0.737] | 1496 | 16086250 | 547592 |
| snmalloc | 2 | 180 | 159 | 0.883 [0.828, 0.922] | 1146 | 8257925 | 869879 |
| snmalloc | 4 | 180 | 168 | 0.933 [0.887, 0.961] | 1247 | 6087348 | 813646 |
| snmalloc | all | 540 | 448 | 0.830 [0.796, 0.859] | 1248 | 30431523 | 2231117 |
| hardened_malloc | 1 | 180 | 180 | 1.000 [0.979, 1.000] | 89 | 2021768 | 190799 |
| hardened_malloc | 2 | 180 | 180 | 1.000 [0.979, 1.000] | 68 | 2756844 | 608736 |
| hardened_malloc | 4 | 180 | 180 | 1.000 [0.979, 1.000] | 59 | 3753941 | 775600 |
| hardened_malloc | all | 540 | 540 | 1.000 [0.993, 1.000] | 69 | 8532553 | 1575135 |
| sohamalloc | 1 | 180 | 0 | 0.000 [0.000, 0.021] | - | 42039666 | 952374 |
| sohamalloc | 2 | 180 | 0 | 0.000 [0.000, 0.021] | - | 41327676 | 1046479 |
| sohamalloc | 4 | 180 | 0 | 0.000 [0.000, 0.021] | - | 38773514 | 1016568 |
| sohamalloc | all | 540 | 0 | 0.000 [0.000, 0.007] | - | 122140856 | 3015421 |

## Failures / trials per allocator and handler (all thread counts)

| allocator | nosignal | none | malloc | free | realloc | calloc | large | churn |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| glibc | 0/90 | 0/90 | 90/90 | 90/90 | 90/90 | 34/90 | 90/90 | 90/90 |
| jemalloc | 0/90 | 0/90 | 72/90 | 90/90 | 90/90 | 36/90 | 90/90 | 90/90 |
| mimalloc | 0/90 | 0/90 | 84/90 | 1/90 | 78/90 | 25/90 | 83/90 | 86/90 |
| tcmalloc | 0/90 | 0/90 | 90/90 | 90/90 | 90/90 | 90/90 | 90/90 | 90/90 |
| tbbmalloc | 0/90 | 0/90 | 0/90 | 90/90 | 0/90 | 0/90 | 90/90 | 90/90 |
| scudo | 0/90 | 0/90 | 35/90 | 89/90 | 38/90 | 0/90 | 90/90 | 90/90 |
| snmalloc | 0/90 | 0/90 | 85/90 | 52/90 | 79/90 | 52/90 | 90/90 | 90/90 |
| hardened_malloc | 0/90 | 0/90 | 90/90 | 90/90 | 90/90 | 90/90 | 90/90 | 90/90 |
| sohamalloc | 0/90 | 0/90 | 0/90 | 0/90 | 0/90 | 0/90 | 0/90 | 0/90 |

## Handler results (all thread counts)

Handler calls that completed, and among them the allocations that returned NULL (`null`: the `free` handler counts an empty pool instead) and results that were wrong (`bad`: calloc memory not zero, realloc contents lost, aligned memory misaligned). Trials that failed are included up to their failure.

| allocator | handler | calls | null | bad |
|---|---|---:|---:|---:|
| glibc | malloc | 203697 | 0 | 0 |
| glibc | free | 1614225 | 1591428 | 0 |
| glibc | realloc | 28210 | 0 | 0 |
| glibc | calloc | 15027629 | 0 | 0 |
| glibc | large | 102449 | 0 | 0 |
| glibc | churn | 197292 | 0 | 0 |
| jemalloc | malloc | 9596742 | 0 | 0 |
| jemalloc | free | 2653322 | 2553912 | 0 |
| jemalloc | realloc | 1345859 | 0 | 0 |
| jemalloc | calloc | 15585919 | 0 | 0 |
| jemalloc | large | 66760 | 0 | 0 |
| jemalloc | churn | 378823 | 0 | 0 |
| mimalloc | malloc | 1555498 | 0 | 0 |
| mimalloc | free | 20643394 | 14908447 | 0 |
| mimalloc | realloc | 2768884 | 0 | 0 |
| mimalloc | calloc | 14902109 | 0 | 0 |
| mimalloc | large | 1902606 | 7961 | 0 |
| mimalloc | churn | 3888171 | 2736 | 0 |
| tcmalloc | malloc | 694833 | 0 | 0 |
| tcmalloc | free | 2123008 | 2056929 | 0 |
| tcmalloc | realloc | 1158835 | 0 | 0 |
| tcmalloc | calloc | 1560545 | 0 | 0 |
| tcmalloc | large | 23835 | 0 | 0 |
| tcmalloc | churn | 470910 | 0 | 0 |
| tbbmalloc | malloc | 19848583 | 0 | 0 |
| tbbmalloc | free | 0 | 0 | 0 |
| tbbmalloc | realloc | 19834022 | 0 | 0 |
| tbbmalloc | calloc | 19852166 | 0 | 0 |
| tbbmalloc | large | 80442 | 0 | 0 |
| tbbmalloc | churn | 61925 | 0 | 0 |
| scudo | malloc | 15113073 | 0 | 0 |
| scudo | free | 4771003 | 4312290 | 0 |
| scudo | realloc | 15485753 | 0 | 0 |
| scudo | calloc | 19878848 | 0 | 0 |
| scudo | large | 114387 | 0 | 0 |
| scudo | churn | 641943 | 0 | 0 |
| snmalloc | malloc | 4289239 | 0 | 0 |
| snmalloc | free | 9150156 | 7009370 | 0 |
| snmalloc | realloc | 3906554 | 0 | 8549 |
| snmalloc | calloc | 12147587 | 0 | 0 |
| snmalloc | large | 676679 | 0 | 0 |
| snmalloc | churn | 261308 | 0 | 0 |
| hardened_malloc | malloc | 673595 | 0 | 0 |
| hardened_malloc | free | 788728 | 679508 | 0 |
| hardened_malloc | realloc | 733191 | 0 | 0 |
| hardened_malloc | calloc | 5365953 | 0 | 0 |
| hardened_malloc | large | 611584 | 0 | 0 |
| hardened_malloc | churn | 359502 | 0 | 0 |
| sohamalloc | malloc | 20476838 | 0 | 0 |
| sohamalloc | free | 20337064 | 15387030 | 0 |
| sohamalloc | realloc | 20427239 | 0 | 0 |
| sohamalloc | calloc | 20453258 | 0 | 0 |
| sohamalloc | large | 20305876 | 13838 | 0 |
| sohamalloc | churn | 20140581 | 12 | 0 |

## Every cell

| allocator | threads | handler | trials | survived | stall | corruption | crash | P(fail) [95% CI] | TTF median [min, max] (ms) | inside/trial |
|---|---:|---|---:|---:|---:|---:|---:|---|---|---:|
| glibc | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| glibc | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6913 |
| glibc | 1 | malloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [25, 32] | 1 |
| glibc | 1 | free | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 29 [25, 109] | 29 |
| glibc | 1 | realloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 29 [27, 31] | 1 |
| glibc | 1 | calloc | 30 | 23 | 7 | 0 | 0 | 0.23 [0.12, 0.41] | 29 [26, 251] | 5407 |
| glibc | 1 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [26, 30] | 2 |
| glibc | 1 | churn | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [26, 31] | 1 |
| glibc | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| glibc | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7241 |
| glibc | 2 | malloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [25, 30] | 80 |
| glibc | 2 | free | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 29 [26, 67] | 27175 |
| glibc | 2 | realloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 27 [25, 29] | 2 |
| glibc | 2 | calloc | 30 | 21 | 9 | 0 | 0 | 0.30 [0.17, 0.48] | 27 [26, 5044] | 9094 |
| glibc | 2 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [9, 29] | 3203 |
| glibc | 2 | churn | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 27 [25, 37] | 3216 |
| glibc | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| glibc | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7996 |
| glibc | 4 | malloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 27 [20, 34] | 415 |
| glibc | 4 | free | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 26 [26, 72] | 23593 |
| glibc | 4 | realloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 26 [7, 34] | 785 |
| glibc | 4 | calloc | 30 | 12 | 18 | 0 | 0 | 0.60 [0.42, 0.75] | 27 [22, 475] | 14352 |
| glibc | 4 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 27 [24, 31] | 11 |
| glibc | 4 | churn | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 27 [24, 34] | 3230 |
| jemalloc | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| jemalloc | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4777 |
| jemalloc | 1 | malloc | 30 | 10 | 20 | 0 | 0 | 0.67 [0.49, 0.81] | 1220 [27, 3523] | 2364 |
| jemalloc | 1 | free | 30 | 0 | 23 | 1 | 6 | 1.00 [0.89, 1.00] | 157 [27, 5034] | 7785 |
| jemalloc | 1 | realloc | 30 | 0 | 17 | 1 | 12 | 1.00 [0.89, 1.00] | 420 [27, 3448] | 156 |
| jemalloc | 1 | calloc | 30 | 20 | 7 | 1 | 2 | 0.33 [0.19, 0.51] | 2940 [301, 4218] | 3853 |
| jemalloc | 1 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [26, 52] | 3 |
| jemalloc | 1 | churn | 30 | 0 | 29 | 0 | 1 | 1.00 [0.89, 1.00] | 49 [27, 1931] | 32 |
| jemalloc | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| jemalloc | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5563 |
| jemalloc | 2 | malloc | 30 | 8 | 20 | 0 | 2 | 0.73 [0.56, 0.86] | 1853 [27, 4389] | 4078 |
| jemalloc | 2 | free | 30 | 0 | 29 | 0 | 1 | 1.00 [0.89, 1.00] | 87 [26, 1186] | 44709 |
| jemalloc | 2 | realloc | 30 | 0 | 16 | 1 | 13 | 1.00 [0.89, 1.00] | 679 [26, 2308] | 5101 |
| jemalloc | 2 | calloc | 30 | 19 | 5 | 3 | 3 | 0.37 [0.22, 0.54] | 3077 [28, 4966] | 6366 |
| jemalloc | 2 | large | 30 | 0 | 28 | 0 | 2 | 1.00 [0.89, 1.00] | 28 [26, 163] | 1602 |
| jemalloc | 2 | churn | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 69 [46, 97] | 3287 |
| jemalloc | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| jemalloc | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6579 |
| jemalloc | 4 | malloc | 30 | 0 | 24 | 2 | 4 | 1.00 [0.89, 1.00] | 1159 [23, 4959] | 5717 |
| jemalloc | 4 | free | 30 | 0 | 21 | 0 | 9 | 1.00 [0.89, 1.00] | 121 [26, 1299] | 22020 |
| jemalloc | 4 | realloc | 30 | 0 | 16 | 1 | 13 | 1.00 [0.89, 1.00] | 408 [26, 4101] | 4414 |
| jemalloc | 4 | calloc | 30 | 15 | 6 | 0 | 9 | 0.50 [0.33, 0.67] | 3765 [207, 4650] | 5061 |
| jemalloc | 4 | large | 30 | 0 | 24 | 0 | 6 | 1.00 [0.89, 1.00] | 28 [26, 2158] | 9 |
| jemalloc | 4 | churn | 30 | 0 | 28 | 0 | 2 | 1.00 [0.89, 1.00] | 82 [66, 2167] | 1843 |
| mimalloc | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| mimalloc | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6456 |
| mimalloc | 1 | malloc | 30 | 6 | 0 | 1 | 23 | 0.80 [0.63, 0.90] | 1560 [323, 4462] | 1247 |
| mimalloc | 1 | free | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6344 |
| mimalloc | 1 | realloc | 30 | 9 | 0 | 0 | 21 | 0.70 [0.52, 0.83] | 3199 [200, 5821] | 1910 |
| mimalloc | 1 | calloc | 30 | 25 | 0 | 0 | 5 | 0.17 [0.07, 0.34] | 1732 [1340, 4165] | 5437 |
| mimalloc | 1 | large | 30 | 7 | 0 | 1 | 22 | 0.77 [0.59, 0.88] | 2526 [116, 5832] | 1654 |
| mimalloc | 1 | churn | 30 | 3 | 0 | 21 | 6 | 0.90 [0.74, 0.97] | 1286 [60, 5281] | 1990 |
| mimalloc | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| mimalloc | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6648 |
| mimalloc | 2 | malloc | 30 | 0 | 0 | 2 | 28 | 1.00 [0.89, 1.00] | 1549 [119, 4962] | 63 |
| mimalloc | 2 | free | 30 | 29 | 0 | 0 | 1 | 0.03 [0.01, 0.17] | 4090 [4090, 4090] | 6504 |
| mimalloc | 2 | realloc | 30 | 2 | 0 | 0 | 28 | 0.93 [0.79, 0.98] | 1997 [525, 5806] | 418 |
| mimalloc | 2 | calloc | 30 | 21 | 0 | 0 | 9 | 0.30 [0.17, 0.48] | 4225 [670, 5786] | 4892 |
| mimalloc | 2 | large | 30 | 0 | 0 | 0 | 30 | 1.00 [0.89, 1.00] | 1800 [109, 3794] | 0 |
| mimalloc | 2 | churn | 30 | 0 | 0 | 18 | 12 | 1.00 [0.89, 1.00] | 1062 [100, 4323] | 864 |
| mimalloc | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| mimalloc | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6813 |
| mimalloc | 4 | malloc | 30 | 0 | 0 | 2 | 28 | 1.00 [0.89, 1.00] | 1339 [20, 3952] | 46 |
| mimalloc | 4 | free | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6697 |
| mimalloc | 4 | realloc | 30 | 1 | 0 | 0 | 29 | 0.97 [0.83, 0.99] | 1582 [281, 3317] | 157 |
| mimalloc | 4 | calloc | 30 | 19 | 0 | 0 | 11 | 0.37 [0.22, 0.54] | 2378 [486, 5111] | 3765 |
| mimalloc | 4 | large | 30 | 0 | 0 | 1 | 29 | 1.00 [0.89, 1.00] | 1926 [255, 5947] | 97 |
| mimalloc | 4 | churn | 30 | 1 | 0 | 11 | 18 | 0.97 [0.83, 0.99] | 2012 [140, 4070] | 885 |
| tcmalloc | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| tcmalloc | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6003 |
| tcmalloc | 1 | malloc | 30 | 0 | 27 | 0 | 3 | 1.00 [0.89, 1.00] | 29 [25, 2241] | 49 |
| tcmalloc | 1 | free | 30 | 0 | 19 | 2 | 9 | 1.00 [0.89, 1.00] | 200 [28, 3031] | 257 |
| tcmalloc | 1 | realloc | 30 | 0 | 23 | 1 | 6 | 1.00 [0.89, 1.00] | 39 [26, 1206] | 60 |
| tcmalloc | 1 | calloc | 30 | 0 | 18 | 1 | 11 | 1.00 [0.89, 1.00] | 69 [27, 1169] | 58 |
| tcmalloc | 1 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 29 [27, 70] | 36 |
| tcmalloc | 1 | churn | 30 | 0 | 22 | 1 | 7 | 1.00 [0.89, 1.00] | 48 [26, 2024] | 63 |
| tcmalloc | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| tcmalloc | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6006 |
| tcmalloc | 2 | malloc | 30 | 0 | 20 | 2 | 8 | 1.00 [0.89, 1.00] | 48 [26, 1202] | 10855 |
| tcmalloc | 2 | free | 30 | 0 | 22 | 1 | 7 | 1.00 [0.89, 1.00] | 87 [26, 2035] | 27159 |
| tcmalloc | 2 | realloc | 30 | 0 | 16 | 1 | 13 | 1.00 [0.89, 1.00] | 64 [26, 1953] | 19260 |
| tcmalloc | 2 | calloc | 30 | 0 | 17 | 1 | 12 | 1.00 [0.89, 1.00] | 66 [27, 2065] | 17447 |
| tcmalloc | 2 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 28 [10, 75] | 27 |
| tcmalloc | 2 | churn | 30 | 0 | 17 | 0 | 13 | 1.00 [0.89, 1.00] | 71 [26, 2083] | 7531 |
| tcmalloc | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| tcmalloc | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6676 |
| tcmalloc | 4 | malloc | 30 | 0 | 14 | 2 | 14 | 1.00 [0.89, 1.00] | 108 [26, 1240] | 10253 |
| tcmalloc | 4 | free | 30 | 0 | 15 | 1 | 14 | 1.00 [0.89, 1.00] | 123 [25, 2239] | 31656 |
| tcmalloc | 4 | realloc | 30 | 0 | 19 | 0 | 11 | 1.00 [0.89, 1.00] | 46 [7, 3163] | 17502 |
| tcmalloc | 4 | calloc | 30 | 0 | 21 | 1 | 8 | 1.00 [0.89, 1.00] | 58 [26, 2061] | 31889 |
| tcmalloc | 4 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 26 [6, 32] | 12 |
| tcmalloc | 4 | churn | 30 | 0 | 13 | 2 | 15 | 1.00 [0.89, 1.00] | 150 [25, 1365] | 5589 |
| tbbmalloc | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| tbbmalloc | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7004 |
| tbbmalloc | 1 | malloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6828 |
| tbbmalloc | 1 | free | 30 | 0 | 0 | 0 | 30 | 1.00 [0.89, 1.00] | 1144 [159, 1892] | 0 |
| tbbmalloc | 1 | realloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6739 |
| tbbmalloc | 1 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6865 |
| tbbmalloc | 1 | large | 30 | 0 | 0 | 0 | 30 | 1.00 [0.89, 1.00] | 1111 [103, 1262] | 0 |
| tbbmalloc | 1 | churn | 30 | 0 | 13 | 0 | 17 | 1.00 [0.89, 1.00] | 123 [47, 2135] | 49 |
| tbbmalloc | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| tbbmalloc | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7569 |
| tbbmalloc | 2 | malloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7448 |
| tbbmalloc | 2 | free | 30 | 0 | 0 | 0 | 30 | 1.00 [0.89, 1.00] | 1159 [158, 1669] | 0 |
| tbbmalloc | 2 | realloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7618 |
| tbbmalloc | 2 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7569 |
| tbbmalloc | 2 | large | 30 | 0 | 0 | 0 | 30 | 1.00 [0.89, 1.00] | 1129 [94, 1234] | 0 |
| tbbmalloc | 2 | churn | 30 | 0 | 8 | 0 | 22 | 1.00 [0.89, 1.00] | 235 [67, 2175] | 46 |
| tbbmalloc | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| tbbmalloc | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7595 |
| tbbmalloc | 4 | malloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7495 |
| tbbmalloc | 4 | free | 30 | 0 | 0 | 0 | 30 | 1.00 [0.89, 1.00] | 1249 [158, 1817] | 0 |
| tbbmalloc | 4 | realloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7547 |
| tbbmalloc | 4 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7573 |
| tbbmalloc | 4 | large | 30 | 0 | 2 | 0 | 28 | 1.00 [0.89, 1.00] | 1159 [15, 1741] | 2642 |
| tbbmalloc | 4 | churn | 30 | 0 | 5 | 0 | 25 | 1.00 [0.89, 1.00] | 561 [107, 2936] | 69 |
| scudo | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| scudo | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 8051 |
| scudo | 1 | malloc | 30 | 21 | 6 | 3 | 0 | 0.30 [0.17, 0.48] | 2016 [40, 4166] | 6377 |
| scudo | 1 | free | 30 | 1 | 19 | 10 | 0 | 0.97 [0.83, 0.99] | 692 [40, 4146] | 1536 |
| scudo | 1 | realloc | 30 | 21 | 6 | 2 | 1 | 0.30 [0.17, 0.48] | 3163 [47, 4791] | 6704 |
| scudo | 1 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7914 |
| scudo | 1 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 48 [27, 308] | 86 |
| scudo | 1 | churn | 30 | 0 | 29 | 0 | 1 | 1.00 [0.89, 1.00] | 120 [66, 1012] | 320 |
| scudo | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| scudo | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 9675 |
| scudo | 2 | malloc | 30 | 15 | 14 | 1 | 0 | 0.50 [0.33, 0.67] | 1608 [748, 4060] | 13150 |
| scudo | 2 | free | 30 | 0 | 23 | 7 | 0 | 1.00 [0.89, 1.00] | 661 [146, 2817] | 31247 |
| scudo | 2 | realloc | 30 | 21 | 6 | 3 | 0 | 0.30 [0.17, 0.48] | 2025 [27, 4105] | 9466 |
| scudo | 2 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 9585 |
| scudo | 2 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 38 [21, 479] | 85 |
| scudo | 2 | churn | 30 | 0 | 27 | 1 | 2 | 1.00 [0.89, 1.00] | 129 [40, 456] | 302 |
| scudo | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| scudo | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 12406 |
| scudo | 4 | malloc | 30 | 19 | 9 | 2 | 0 | 0.37 [0.22, 0.54] | 1323 [21, 3951] | 14865 |
| scudo | 4 | free | 30 | 0 | 19 | 11 | 0 | 1.00 [0.89, 1.00] | 295 [20, 1158] | 40832 |
| scudo | 4 | realloc | 30 | 10 | 16 | 3 | 1 | 0.67 [0.49, 0.81] | 2354 [16, 4992] | 18839 |
| scudo | 4 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 12657 |
| scudo | 4 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 57 [26, 749] | 96 |
| scudo | 4 | churn | 30 | 0 | 29 | 1 | 0 | 1.00 [0.89, 1.00] | 207 [26, 539] | 806 |
| snmalloc | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| snmalloc | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 7927 |
| snmalloc | 1 | malloc | 30 | 5 | 0 | 20 | 5 | 0.83 [0.66, 0.93] | 841 [60, 5094] | 2839 |
| snmalloc | 1 | free | 30 | 18 | 0 | 1 | 11 | 0.40 [0.25, 0.58] | 1024 [294, 5299] | 4741 |
| snmalloc | 1 | realloc | 30 | 11 | 0 | 8 | 11 | 0.63 [0.46, 0.78] | 2413 [220, 4537] | 3773 |
| snmalloc | 1 | calloc | 30 | 25 | 0 | 0 | 5 | 0.17 [0.07, 0.34] | 3292 [1408, 4570] | 6728 |
| snmalloc | 1 | large | 30 | 0 | 5 | 1 | 24 | 1.00 [0.89, 1.00] | 1046 [26, 5036] | 9 |
| snmalloc | 1 | churn | 30 | 0 | 4 | 2 | 24 | 1.00 [0.89, 1.00] | 1525 [68, 4399] | 160 |
| snmalloc | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| snmalloc | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 9098 |
| snmalloc | 2 | malloc | 30 | 0 | 0 | 21 | 9 | 1.00 [0.89, 1.00] | 752 [60, 6742] | 1067 |
| snmalloc | 2 | free | 30 | 12 | 0 | 3 | 15 | 0.60 [0.42, 0.75] | 3719 [830, 6330] | 4017 |
| snmalloc | 2 | realloc | 30 | 0 | 3 | 6 | 21 | 1.00 [0.89, 1.00] | 1049 [60, 6414] | 1692 |
| snmalloc | 2 | calloc | 30 | 9 | 10 | 1 | 10 | 0.70 [0.52, 0.83] | 1390 [409, 5033] | 14114 |
| snmalloc | 2 | large | 30 | 0 | 10 | 1 | 19 | 1.00 [0.89, 1.00] | 462 [20, 3118] | 7980 |
| snmalloc | 2 | churn | 30 | 0 | 9 | 0 | 21 | 1.00 [0.89, 1.00] | 1040 [106, 3418] | 123 |
| snmalloc | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| snmalloc | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 10168 |
| snmalloc | 4 | malloc | 30 | 0 | 1 | 22 | 7 | 1.00 [0.89, 1.00] | 1070 [160, 3176] | 3030 |
| snmalloc | 4 | free | 30 | 8 | 0 | 2 | 20 | 0.73 [0.56, 0.86] | 2317 [104, 6343] | 2554 |
| snmalloc | 4 | realloc | 30 | 0 | 0 | 9 | 21 | 1.00 [0.89, 1.00] | 1425 [40, 2499] | 507 |
| snmalloc | 4 | calloc | 30 | 4 | 8 | 5 | 13 | 0.87 [0.70, 0.95] | 2220 [341, 5047] | 10727 |
| snmalloc | 4 | large | 30 | 0 | 19 | 0 | 11 | 1.00 [0.89, 1.00] | 39 [2, 4081] | 10083 |
| snmalloc | 4 | churn | 30 | 0 | 8 | 2 | 20 | 1.00 [0.89, 1.00] | 653 [180, 5257] | 218 |
| hardened_malloc | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| hardened_malloc | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 22487 |
| hardened_malloc | 1 | malloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 58 [25, 208] | 287 |
| hardened_malloc | 1 | free | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 88 [27, 288] | 375 |
| hardened_malloc | 1 | realloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 78 [28, 410] | 339 |
| hardened_malloc | 1 | calloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 761 [48, 2553] | 4256 |
| hardened_malloc | 1 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 159 [21, 651] | 869 |
| hardened_malloc | 1 | churn | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 48 [25, 229] | 231 |
| hardened_malloc | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| hardened_malloc | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 26133 |
| hardened_malloc | 2 | malloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 48 [27, 268] | 2234 |
| hardened_malloc | 2 | free | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 66 [26, 209] | 5235 |
| hardened_malloc | 2 | realloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 66 [25, 347] | 5371 |
| hardened_malloc | 2 | calloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 639 [29, 3017] | 6614 |
| hardened_malloc | 2 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 87 [25, 229] | 495 |
| hardened_malloc | 2 | churn | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 47 [25, 209] | 339 |
| hardened_malloc | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| hardened_malloc | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 31036 |
| hardened_malloc | 4 | malloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 27 [25, 147] | 3332 |
| hardened_malloc | 4 | free | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 66 [25, 206] | 5786 |
| hardened_malloc | 4 | realloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 46 [7, 126] | 2848 |
| hardened_malloc | 4 | calloc | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 418 [26, 2096] | 10075 |
| hardened_malloc | 4 | large | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 156 [26, 1169] | 1491 |
| hardened_malloc | 4 | churn | 30 | 0 | 30 | 0 | 0 | 1.00 [0.89, 1.00] | 27 [26, 151] | 2318 |
| sohamalloc | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| sohamalloc | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5163 |
| sohamalloc | 1 | malloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5005 |
| sohamalloc | 1 | free | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5151 |
| sohamalloc | 1 | realloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5155 |
| sohamalloc | 1 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5272 |
| sohamalloc | 1 | large | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5424 |
| sohamalloc | 1 | churn | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5736 |
| sohamalloc | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| sohamalloc | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5501 |
| sohamalloc | 2 | malloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5610 |
| sohamalloc | 2 | free | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5821 |
| sohamalloc | 2 | realloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5771 |
| sohamalloc | 2 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5756 |
| sohamalloc | 2 | large | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5932 |
| sohamalloc | 2 | churn | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5989 |
| sohamalloc | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| sohamalloc | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 6044 |
| sohamalloc | 4 | malloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5496 |
| sohamalloc | 4 | free | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5392 |
| sohamalloc | 4 | realloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5868 |
| sohamalloc | 4 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5799 |
| sohamalloc | 4 | large | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5678 |
| sohamalloc | 4 | churn | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 5649 |

## Failure details

- **glibc**: calloc stall: main:calloc handler:calloc in_handler=1 (x2); calloc stall: main:free-remote handler:none in_handler=0 (x1); calloc stall: main:malloc handler:calloc in_handler=1 (x24); calloc stall: main:posix_memalign handler:calloc in_handler=1 (x5); calloc stall: main:realloc handler:calloc in_handler=1 (x2); churn stall: main:calloc handler:malloc in_handler=1 (x12); churn stall: main:malloc handler:malloc in_handler=1 (x66); churn stall: main:posix_memalign handler:malloc in_handler=1 (x10); churn stall: main:realloc handler:malloc in_handler=1 (x2); free stall: main:calloc handler:free in_handler=1 (x10); free stall: main:free handler:free in_handler=1 (x1); free stall: main:malloc handler:free in_handler=1 (x63); free stall: main:posix_memalign handler:free in_handler=1 (x5); free stall: main:realloc handler:free in_handler=1 (x11); large stall: main:calloc handler:malloc in_handler=1 (x15); large stall: main:free handler:malloc in_handler=1 (x2); large stall: main:malloc handler:malloc in_handler=1 (x55); large stall: main:posix_memalign handler:malloc in_handler=1 (x17); large stall: main:realloc handler:malloc in_handler=1 (x1); malloc stall: main:calloc handler:malloc in_handler=1 (x15); malloc stall: main:malloc handler:malloc in_handler=1 (x63); malloc stall: main:posix_memalign handler:malloc in_handler=1 (x12); realloc stall: main:calloc handler:malloc in_handler=1 (x15); realloc stall: main:malloc handler:malloc in_handler=1 (x60); realloc stall: main:malloc handler:realloc in_handler=1 (x3); realloc stall: main:posix_memalign handler:malloc in_handler=1 (x10); realloc stall: main:realloc handler:malloc in_handler=1 (x2)
- **jemalloc**: calloc corruption: content (x4); calloc crash: SIGSEGV (x14); calloc stall: main:calloc handler:free in_handler=1 (x1); calloc stall: main:free handler:calloc in_handler=1 (x5); calloc stall: main:free handler:free in_handler=1 (x3); calloc stall: main:malloc handler:calloc in_handler=1 (x4); calloc stall: main:malloc handler:free in_handler=1 (x1); calloc stall: main:posix_memalign handler:free in_handler=1 (x1); calloc stall: main:realloc handler:calloc in_handler=1 (x2); calloc stall: main:realloc handler:free in_handler=1 (x1); churn crash: SIGSEGV (x3); churn stall: main:calloc handler:free in_handler=1 (x3); churn stall: main:calloc handler:malloc in_handler=1 (x11); churn stall: main:free handler:free in_handler=1 (x5); churn stall: main:free handler:malloc in_handler=1 (x14); churn stall: main:free-remote handler:malloc in_handler=1 (x2); churn stall: main:malloc handler:free in_handler=1 (x6); churn stall: main:malloc handler:malloc in_handler=1 (x19); churn stall: main:none handler:free in_handler=1 (x3); churn stall: main:none handler:malloc in_handler=1 (x5); churn stall: main:posix_memalign handler:malloc in_handler=1 (x5); churn stall: main:realloc handler:free in_handler=1 (x1); churn stall: main:realloc handler:malloc in_handler=1 (x13); free corruption: content (x1); free crash: SIGSEGV (x16); free stall: main:calloc handler:free in_handler=1 (x11); free stall: main:free handler:free in_handler=1 (x4); free stall: main:free-remote handler:free in_handler=1 (x1); free stall: main:malloc handler:free in_handler=1 (x38); free stall: main:malloc handler:none in_handler=0 (x1); free stall: main:posix_memalign handler:free in_handler=1 (x7); free stall: main:realloc handler:free in_handler=1 (x11); large crash: SIGSEGV (x8); large stall: main:calloc handler:malloc in_handler=1 (x14); large stall: main:free handler:malloc in_handler=1 (x2); large stall: main:malloc handler:malloc in_handler=1 (x56); large stall: main:none handler:free in_handler=1 (x1); large stall: main:posix_memalign handler:malloc in_handler=1 (x7); large stall: main:realloc handler:malloc in_handler=1 (x2); malloc corruption: content (x2); malloc crash: SIGSEGV (x6); malloc stall: main:calloc handler:malloc in_handler=1 (x11); malloc stall: main:free handler:malloc in_handler=1 (x6); malloc stall: main:malloc handler:free in_handler=1 (x4); malloc stall: main:malloc handler:malloc in_handler=1 (x28); malloc stall: main:posix_memalign handler:malloc in_handler=1 (x6); malloc stall: main:realloc handler:free in_handler=1 (x1); malloc stall: main:realloc handler:malloc in_handler=1 (x8); realloc corruption: content (x2); realloc corruption: realloc-content (x1); realloc crash: SIGSEGV (x38); realloc stall: main:calloc handler:free in_handler=1 (x2); realloc stall: main:calloc handler:realloc in_handler=1 (x2); realloc stall: main:free handler:free in_handler=1 (x2); realloc stall: main:free handler:realloc in_handler=1 (x6); realloc stall: main:free-remote handler:realloc in_handler=1 (x4); realloc stall: main:malloc handler:free in_handler=1 (x7); realloc stall: main:malloc handler:malloc in_handler=1 (x3); realloc stall: main:malloc handler:realloc in_handler=1 (x9); realloc stall: main:posix_memalign handler:realloc in_handler=1 (x2); realloc stall: main:realloc handler:free in_handler=1 (x3); realloc stall: main:realloc handler:realloc in_handler=1 (x9)
- **mimalloc**: calloc crash: SIGSEGV (x25); churn corruption: content (x50); churn crash: SIGSEGV (x36); free crash: SIGSEGV (x1); large corruption: calloc-nonzero (x2); large crash: SIGSEGV (x81); malloc corruption: calloc-nonzero (x1); malloc corruption: content (x4); malloc crash: SIGSEGV (x79); realloc crash: SIGSEGV (x78)
- **tcmalloc**: calloc corruption: content (x3); calloc crash: SIGSEGV (x31); calloc stall: main:free handler:calloc in_handler=1 (x1); calloc stall: main:free handler:free in_handler=1 (x31); calloc stall: main:free-remote handler:free in_handler=1 (x6); calloc stall: main:malloc handler:free in_handler=1 (x7); calloc stall: main:realloc handler:free in_handler=1 (x11); churn corruption: content (x3); churn crash: SIGSEGV (x35); churn stall: main:calloc handler:malloc in_handler=1 (x1); churn stall: main:free handler:free in_handler=1 (x21); churn stall: main:free handler:malloc in_handler=1 (x4); churn stall: main:free-remote handler:free in_handler=1 (x1); churn stall: main:malloc handler:malloc in_handler=1 (x12); churn stall: main:none handler:malloc in_handler=1 (x4); churn stall: main:realloc handler:free in_handler=1 (x6); churn stall: main:realloc handler:malloc in_handler=1 (x3); free corruption: content (x4); free crash: SIGSEGV (x30); free stall: main:calloc handler:free in_handler=1 (x1); free stall: main:free handler:free in_handler=1 (x18); free stall: main:malloc handler:free in_handler=1 (x21); free stall: main:none handler:free in_handler=1 (x2); free stall: main:realloc handler:free in_handler=1 (x14); large stall: main:calloc handler:free in_handler=1 (x1); large stall: main:calloc handler:malloc in_handler=1 (x13); large stall: main:free handler:malloc in_handler=1 (x5); large stall: main:malloc handler:malloc in_handler=1 (x38); large stall: main:none handler:free in_handler=1 (x1); large stall: main:none handler:malloc in_handler=1 (x26); large stall: main:posix_memalign handler:malloc in_handler=1 (x3); large stall: main:realloc handler:malloc in_handler=1 (x3); malloc corruption: content (x4); malloc crash: SIGSEGV (x25); malloc stall: main:calloc handler:free in_handler=1 (x1); malloc stall: main:calloc handler:malloc in_handler=1 (x5); malloc stall: main:free handler:free in_handler=1 (x29); malloc stall: main:free handler:malloc in_handler=1 (x2); malloc stall: main:free-remote handler:free in_handler=1 (x1); malloc stall: main:malloc handler:free in_handler=1 (x3); malloc stall: main:malloc handler:malloc in_handler=1 (x10); malloc stall: main:none handler:malloc in_handler=1 (x3); malloc stall: main:realloc handler:free in_handler=1 (x7); realloc corruption: content (x2); realloc crash: SIGSEGV (x30); realloc stall: main:calloc handler:free in_handler=1 (x1); realloc stall: main:calloc handler:realloc in_handler=1 (x4); realloc stall: main:free handler:free in_handler=1 (x1); realloc stall: main:free handler:realloc in_handler=1 (x27); realloc stall: main:free-remote handler:realloc in_handler=1 (x4); realloc stall: main:malloc handler:free in_handler=1 (x1); realloc stall: main:malloc handler:malloc in_handler=1 (x2); realloc stall: main:malloc handler:realloc in_handler=1 (x5); realloc stall: main:none handler:realloc in_handler=1 (x2); realloc stall: main:posix_memalign handler:realloc in_handler=1 (x1); realloc stall: main:realloc handler:realloc in_handler=1 (x10)
- **tbbmalloc**: churn crash: SIGSEGV (x64); churn stall: main:calloc handler:malloc in_handler=1 (x6); churn stall: main:malloc handler:free in_handler=1 (x1); churn stall: main:malloc handler:malloc in_handler=1 (x6); churn stall: main:none handler:malloc in_handler=1 (x7); churn stall: main:posix_memalign handler:malloc in_handler=1 (x2); churn stall: main:realloc handler:malloc in_handler=1 (x4); free crash: SIGSEGV (x90); large crash: SIGSEGV (x88); large stall: main:free handler:malloc in_handler=1 (x1); large stall: main:malloc handler:malloc in_handler=1 (x1)
- **scudo**: churn corruption: content (x2); churn crash: SIGABRT (x1); churn crash: SIGSEGV (x2); churn stall: main:calloc handler:free in_handler=1 (x1); churn stall: main:calloc handler:malloc in_handler=1 (x3); churn stall: main:free handler:free in_handler=1 (x8); churn stall: main:free handler:malloc in_handler=1 (x17); churn stall: main:free-remote handler:free in_handler=1 (x2); churn stall: main:malloc handler:free in_handler=1 (x3); churn stall: main:malloc handler:malloc in_handler=1 (x8); churn stall: main:none handler:free in_handler=1 (x9); churn stall: main:none handler:malloc in_handler=1 (x18); churn stall: main:realloc handler:free in_handler=1 (x6); churn stall: main:realloc handler:malloc in_handler=1 (x10); free corruption: content (x28); free stall: main:calloc handler:free in_handler=1 (x10); free stall: main:free handler:free in_handler=1 (x7); free stall: main:malloc handler:free in_handler=1 (x21); free stall: main:none handler:free in_handler=1 (x4); free stall: main:posix_memalign handler:free in_handler=1 (x3); free stall: main:realloc handler:free in_handler=1 (x16); large stall: main:calloc handler:malloc in_handler=1 (x5); large stall: main:free handler:malloc in_handler=1 (x30); large stall: main:free-remote handler:malloc in_handler=1 (x2); large stall: main:malloc handler:malloc in_handler=1 (x12); large stall: main:none handler:malloc in_handler=1 (x27); large stall: main:realloc handler:malloc in_handler=1 (x14); malloc corruption: content (x6); malloc stall: main:calloc handler:malloc in_handler=1 (x4); malloc stall: main:malloc handler:malloc in_handler=1 (x20); malloc stall: main:none handler:malloc in_handler=1 (x3); malloc stall: main:realloc handler:malloc in_handler=1 (x2); realloc corruption: content (x8); realloc crash: SIGSEGV (x2); realloc stall: main:calloc handler:malloc in_handler=1 (x5); realloc stall: main:calloc handler:realloc in_handler=1 (x1); realloc stall: main:malloc handler:malloc in_handler=1 (x16); realloc stall: main:malloc handler:realloc in_handler=1 (x1); realloc stall: main:none handler:malloc in_handler=1 (x2); realloc stall: main:none handler:realloc in_handler=1 (x1); realloc stall: main:posix_memalign handler:realloc in_handler=1 (x1); realloc stall: main:realloc handler:malloc in_handler=1 (x1)
- **snmalloc**: calloc corruption: content (x6); calloc crash: SIGSEGV (x28); calloc stall: main:calloc handler:calloc in_handler=1 (x4); calloc stall: main:calloc handler:none in_handler=0 (x2); calloc stall: main:malloc handler:calloc in_handler=1 (x2); calloc stall: main:malloc handler:none in_handler=0 (x3); calloc stall: main:none handler:calloc in_handler=1 (x1); calloc stall: main:realloc handler:calloc in_handler=1 (x4); calloc stall: main:realloc handler:none in_handler=0 (x2); churn corruption: content (x4); churn crash: SIGSEGV (x65); churn stall: main:free handler:malloc in_handler=1 (x1); churn stall: main:malloc handler:free in_handler=1 (x3); churn stall: main:malloc handler:malloc in_handler=1 (x6); churn stall: main:none handler:free in_handler=1 (x1); churn stall: main:none handler:malloc in_handler=1 (x6); churn stall: main:realloc handler:malloc in_handler=1 (x4); free corruption: content (x6); free crash: SIGSEGV (x46); large corruption: content (x1); large corruption: harness counters overwritten (x1); large crash: SIGSEGV (x54); large stall: main:calloc handler:malloc in_handler=1 (x3); large stall: main:free handler:malloc in_handler=1 (x1); large stall: main:malloc handler:malloc in_handler=1 (x14); large stall: main:none handler:malloc in_handler=1 (x12); large stall: main:posix_memalign handler:malloc in_handler=1 (x1); large stall: main:realloc handler:malloc in_handler=1 (x3); malloc corruption: content (x63); malloc crash: SIGSEGV (x21); malloc stall: main:posix_memalign handler:malloc in_handler=1 (x1); realloc corruption: content (x23); realloc crash: SIGABRT (x1); realloc crash: SIGSEGV (x52); realloc stall: main:malloc handler:realloc in_handler=1 (x2); realloc stall: main:realloc handler:realloc in_handler=1 (x1)
- **hardened_malloc**: calloc stall: main:calloc handler:calloc in_handler=1 (x10); calloc stall: main:free handler:calloc in_handler=1 (x26); calloc stall: main:malloc handler:calloc in_handler=1 (x38); calloc stall: main:realloc handler:calloc in_handler=1 (x16); churn stall: main:calloc handler:free in_handler=1 (x4); churn stall: main:calloc handler:malloc in_handler=1 (x4); churn stall: main:free handler:free in_handler=1 (x8); churn stall: main:free handler:malloc in_handler=1 (x27); churn stall: main:malloc handler:free in_handler=1 (x9); churn stall: main:malloc handler:malloc in_handler=1 (x25); churn stall: main:none handler:malloc in_handler=1 (x1); churn stall: main:posix_memalign handler:malloc in_handler=1 (x1); churn stall: main:realloc handler:free in_handler=1 (x2); churn stall: main:realloc handler:malloc in_handler=1 (x9); free stall: main:calloc handler:free in_handler=1 (x13); free stall: main:free handler:free in_handler=1 (x23); free stall: main:malloc handler:free in_handler=1 (x33); free stall: main:posix_memalign handler:free in_handler=1 (x6); free stall: main:realloc handler:free in_handler=1 (x15); large stall: main:calloc handler:malloc in_handler=1 (x6); large stall: main:free handler:free in_handler=1 (x1); large stall: main:free handler:malloc in_handler=1 (x25); large stall: main:free-remote handler:malloc in_handler=1 (x3); large stall: main:malloc handler:free in_handler=1 (x1); large stall: main:malloc handler:malloc in_handler=1 (x12); large stall: main:none handler:free in_handler=1 (x8); large stall: main:none handler:malloc in_handler=1 (x12); large stall: main:posix_memalign handler:free in_handler=1 (x2); large stall: main:posix_memalign handler:malloc in_handler=1 (x2); large stall: main:realloc handler:malloc in_handler=1 (x18); malloc stall: main:calloc handler:malloc in_handler=1 (x8); malloc stall: main:free handler:malloc in_handler=1 (x25); malloc stall: main:malloc handler:malloc in_handler=1 (x37); malloc stall: main:posix_memalign handler:malloc in_handler=1 (x7); malloc stall: main:realloc handler:malloc in_handler=1 (x13); realloc stall: main:calloc handler:malloc in_handler=1 (x7); realloc stall: main:calloc handler:realloc in_handler=1 (x1); realloc stall: main:free handler:malloc in_handler=1 (x31); realloc stall: main:free handler:realloc in_handler=1 (x3); realloc stall: main:malloc handler:malloc in_handler=1 (x28); realloc stall: main:malloc handler:realloc in_handler=1 (x7); realloc stall: main:posix_memalign handler:realloc in_handler=1 (x1); realloc stall: main:realloc handler:malloc in_handler=1 (x12)

## Controls

1620 control trials, 0 failed.
