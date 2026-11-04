# sigstorm: allocation from signal handlers

Each trial runs 1, 2 or 4 workload threads for up to 5 s while a signaller sends SIGUSR1 to each in turn every 0-40 us (uniformly random) and the handler calls the allocator as named; controls: `nosignal` (no signals at all) and `none` (signals, handler does not allocate). Outcomes: **survived**; **stall** = a thread made no progress for 2000 ms (deadlock or livelock); **corruption** = a block changed contents or two live blocks overlapped; **crash** = killed by a signal (SIGABRT: the allocator detected corruption and aborted). `P(fail)` is the fraction of trials that failed within the trial length, with a 95% Wilson interval; `TTF` is the time to failure of the failed trials (median and range, ms). `inside` counts handler calls that interrupted an allocator call of the workload (the exposure that tests signal safety). A trial whose own counters are impossible (the allocator overwrote the harness's records, which come from it) is counted as corruption.

## Summary: signal-handler kinds pooled (malloc, free, realloc, calloc, large, churn, aligned), per thread count

| allocator | threads | trials | failed | P(fail) [95% CI] | median TTF (ms) | handler calls | inside an allocator call |
|---|---:|---:|---:|---|---:|---:|---:|
| sohamalloc | 1 | 210 | 0 | 0.000 [0.000, 0.018] | - | 47697929 | 820028 |
| sohamalloc | 2 | 210 | 0 | 0.000 [0.000, 0.018] | - | 47040180 | 901984 |
| sohamalloc | 4 | 210 | 0 | 0.000 [0.000, 0.018] | - | 45547639 | 954455 |
| sohamalloc | all | 630 | 0 | 0.000 [0.000, 0.006] | - | 140285748 | 2676467 |

## Failures / trials per allocator and handler (all thread counts)

| allocator | nosignal | none | malloc | free | realloc | calloc | large | churn | aligned |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sohamalloc | 0/90 | 0/90 | 0/90 | 0/90 | 0/90 | 0/90 | 0/90 | 0/90 | 0/90 |

## Handler results (all thread counts)

Handler calls that completed, and among them the allocations that returned NULL (`null`: the `free` handler counts an empty pool instead) and results that were wrong (`bad`: calloc memory not zero, realloc contents lost, aligned memory misaligned). Trials that failed are included up to their failure.

| allocator | handler | calls | null | bad |
|---|---|---:|---:|---:|
| sohamalloc | malloc | 20071393 | 0 | 0 |
| sohamalloc | free | 20059666 | 16676800 | 0 |
| sohamalloc | realloc | 19951556 | 0 | 0 |
| sohamalloc | calloc | 20010541 | 0 | 0 |
| sohamalloc | large | 19941425 | 31 | 0 |
| sohamalloc | churn | 20097150 | 2 | 0 |
| sohamalloc | aligned | 20154017 | 0 | 0 |

## Every cell

| allocator | threads | handler | trials | survived | stall | corruption | crash | P(fail) [95% CI] | TTF median [min, max] (ms) | inside/trial |
|---|---:|---|---:|---:|---:|---:|---:|---|---|---:|
| sohamalloc | 1 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| sohamalloc | 1 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4081 |
| sohamalloc | 1 | malloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 3773 |
| sohamalloc | 1 | free | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 3760 |
| sohamalloc | 1 | realloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 3759 |
| sohamalloc | 1 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 3740 |
| sohamalloc | 1 | large | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 3995 |
| sohamalloc | 1 | churn | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4425 |
| sohamalloc | 1 | aligned | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 3880 |
| sohamalloc | 2 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| sohamalloc | 2 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4148 |
| sohamalloc | 2 | malloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4094 |
| sohamalloc | 2 | free | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4136 |
| sohamalloc | 2 | realloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4168 |
| sohamalloc | 2 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4197 |
| sohamalloc | 2 | large | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4474 |
| sohamalloc | 2 | churn | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4578 |
| sohamalloc | 2 | aligned | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4416 |
| sohamalloc | 4 | nosignal | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 0 |
| sohamalloc | 4 | none | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4726 |
| sohamalloc | 4 | malloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4658 |
| sohamalloc | 4 | free | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4464 |
| sohamalloc | 4 | realloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4554 |
| sohamalloc | 4 | calloc | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4496 |
| sohamalloc | 4 | large | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4463 |
| sohamalloc | 4 | churn | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4634 |
| sohamalloc | 4 | aligned | 30 | 30 | 0 | 0 | 0 | 0.00 [0.00, 0.11] | - | 4543 |

## Failure details


## Controls

180 control trials, 0 failed.
