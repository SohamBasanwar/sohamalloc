# Heap Allocator Security and What Hardening Costs (2020–2026): Evidence Report and Tiered Design for sohamalloc

Cutoff for this research: 2026-10-18. Numbers in [brackets] point to the Sources list. **UNVERIFIED** marks anything I could not confirm from a primary source. "Analysis" marks my own reasoning, not a sourced fact.

---

## 1. Bottom line

1. **Some defenses are nearly free and should always be on.** The cheapest defenses with the most value change *where things live*, not how often you check:
   - out-of-line metadata,
   - type and data separation,
   - never reusing virtual address space across types,
   - encoded or signed free-list links,
   - state checks on `free`.

   Apple set a **0% CPU and 0% memory budget** for kalloc_type and reports "near-zero CPU overhead" [18]. glibc's safe-linking overhead was "not measurable" over 1 billion malloc/free calls [1]. All of snmalloc's checks together cost **under 5%** on the application benchmarks in mimalloc-bench [35].
2. **Defenses that *hold memory back* are expensive.** That means quarantine, per-allocation guard pages, never-reuse schemes and pointer sweeping. They cost **11–98% extra memory**, and some cost double-digit CPU:
   - MineSweeper: 5.4% CPU and +11.1% memory in its own paper [38]. Independent reruns on SPEC CPU2017 measured 33.4% CPU and 32.5% memory [43].
   - FFmalloc: +61% memory on SPEC [40], and +98.4% in a rerun [43].
   - *Scan in Chrome: +12% memory [17].
3. **Zeroing costs depend on when you do it.** Zero-on-alloc is close to free in practice: −0.13% wall time on a kernel build. Zero-on-free is not: +8.38% wall time on the same build [47]. Production allocators that zero on free cap it: xzone only zeroes blocks under 1 KiB [19], and hardened_malloc only zeroes small slots [12].
4. **Memory tagging (MTE) is the only hardware path to probabilistic-to-deterministic protection at single-digit cost, but it is fragile.**
   - Asynchronous mode costs **1.08–1.18×** on Pixel 8 cores. Synchronous mode costs **1.56×** on the Cortex-X3, with outliers up to **6.64×** [22].
   - Apple ships only synchronous EMTE, and only after redesigning both silicon and allocator [20].
   - Tags can leak through speculation (TikTag: over 95% success in under 4 s) [23].
   - glibc removed its "broken" MTE support in 2.44 [4][5].
   - The Pixel 11 reportedly ships with MTE disabled in firmware [27] (UNVERIFIED, secondary source).
5. **Detection in the field is cheap.** GWP-ASan-style sampling costs a fixed ~40–70 KiB per process and negligible CPU [30][31]. It found 60+ bugs in Chrome by 2019 [33]. Ship it everywhere, but treat it as detection, not prevention.

---

## 2. What production shipped, 2020–2026

### 2.1 glibc ptmalloc2

**Safe-linking** (glibc 2.32, Aug 2020) stores free-list links as `PROTECT(P) = (L >> PAGE_SHIFT) XOR P`, where L is the address the link is stored at, and checks that the decoded pointer is aligned [1].
- It adds 2–3 instructions to `free` and 3–4 to `malloc`.
- glibc overhead was "not measurable" over 1e9 operations. In gperftools it averaged 0.02%, worst case 1.5%.
- It uses no memory.
- On 64-bit, a misaligned forgery fails 15 times in 16.
- Google's TCMalloc rejected it on performance grounds [1].

Its stated limits [1]:
- It only raises the bar to "needs a heap or pointer leak".
- It is probabilistic.
- It covers only singly-linked lists.

**Current glibc** also includes:
- A random process-wide `tcache_key` written into freed chunks to detect same-thread double frees. The source admits this "does not cover" a double free first freed on a different thread.
- A full tcache scan when the key matches.
- "unaligned tcache chunk" checks [3].

**Attacks that still work.** how2heap's table, which goes up to glibc 2.43, lists these as working on "latest" or recent glibc [2]:
- `house_of_water`, `house_of_einherjar`, `house_of_spirit`, `house_of_lore`, `poison_null_byte`, `unsafe_unlink`, `mmap_overlapping_chunks`
- `tcache_poisoning` (needs a heap leak after 2.32), `house_of_botcake`, `tcache_stashing_unlink_attack`
- `decrypt_safe_linking` and `safe_link_double_protect` (both ≥ 2.32)
- `tcache_metadata_poisoning`, and a new `tcache_metadata_hijacking` (≥ 2.42)

The lesson is that inline metadata keeps producing new exploitation techniques, even after hardening.

**Structural change.** glibc removed fastbins on 2025-12-17 ("Remove fastbin infrastructure", "remove malloc_consolidate"), which explains how2heap's "< 2.43" cut-off for the fastbin techniques. In May 2026 it removed "broken memory tagging" on aarch64 [4]. The glibc NEWS file confirms the `--enable-memory-tagging` configure option was removed in 2.44 [5].

### 2.2 Scudo (LLVM; default for Android 11+ native code and Fuchsia)

Design [6]:
- A **primary** allocator of fixed-size blocks carved from reserved regions.
- A **secondary** allocator for large blocks, built directly on mmap and surrounded by guard pages.
- An **8-byte header** per chunk holding:
  - class ID, state (available, allocated or quarantined) and allocation type (malloc, new, new[] or memalign),
  - size or unused bytes, and offset,
  - a **16-bit CRC32 checksum** over a global secret, the chunk pointer and the header. The docs say it is "not intended to be cryptographically strong".
- Headers are updated atomically with compare-and-exchange to avoid double fetches.
- Allocation within the primary is randomized.
- The **quarantine** is "fairly costly in terms of performance and memory footprint" and off by default.

Current defaults in `flags.inc` [7]:
- `quarantine_size_kb=0`
- `delete_size_mismatch=true`
- `dealloc_align_mismatch=true`
- `dealloc_type_mismatch=false`
- `zero_contents=false`

Android's Scudo page still lists a 256 KiB quarantine on 64-bit [10]. Which value current Android builds use is **UNVERIFIED**.

Android's configuration [8]:
- MTE-capable (`MaySupportMemoryTagging`)
- shared TSDs (max 8)
- 256 MiB regions
- random region offset
- secondary cache of 32 entries, up to 2 MiB each, 14 MiB resident

For MTE, Scudo gives adjacent blocks tags of opposite parity (odd and even) and excludes the previous tag when retagging [9]. The Android docs say the default "buffer overflow" tuning detects linear overflows deterministically, while the UAF tuning detects both bug classes with about 93% probability [21].

**Cost:** I found no primary benchmark. ProtonAOSP reports jemalloc "up to 4x better than Scudo" in Bionic trace-replay tests at "nearly the same" memory [11] (secondary source, methodology not shown). Low-memory Android devices still use jemalloc [10].

### 2.3 GrapheneOS hardened_malloc

Design [12]:
- All mutable state lives in a dedicated metadata region.
- Each size class has its own region with a random base.
- Guard slabs, random slot selection, and random-array plus FIFO quarantines.
- Zero-on-free plus a write-after-free check when slots are reused.
- 8-byte canaries whose first byte is zero.
- MTE: random tags that exclude 0, the slot's previous tag and both neighbours' tags. Free slots are set to tag 0.

The defaults are aggressive:
- a guard slab between every slab (`GUARD_SLABS_INTERVAL=1`)
- quarantine length 1 for the largest class, scaled to 1024–8192 slots for 16-byte classes
- 4 arenas

The README calls the default "quite aggressive in terms of sacrificing performance and memory usage for security" [12]. Its cost statements:
- Slot randomization "has a measurable performance cost".
- Canaries "slightly" raise memory use and CPU.
- Large allocations take 2× the syscalls to allocate and 3× to free.
- Pkey sealing of metadata stays off because of "a significant performance cost".
- Guard slabs need `vm.max_map_count = 1048576`. The kernel default is 65530 [50].

The **light** variant drops quarantines, randomization and the write-after-free check, and uses a guard slab only every 8 slabs. It keeps zeroing and canaries.

GrapheneOS enables MTE for base-OS slab allocations up to 128 KiB [13]. I found no published percentages (**UNVERIFIED**).

### 2.4 Chromium PartitionAlloc, BackupRefPtr (MiraclePtr) and *Scan

PartitionAlloc guarantees [14]:
- Partitions live in disjoint address ranges, with guard pages at both ends.
- Metadata sits out-of-line in guard-wrapped pages, except free-list pointers.
- Free-list pointers are **byte-reversed** (so partial overwrites fault) and have a **shadow copy** encoded differently.

**BackupRefPtr** (in-slot reference count, quarantine and poisoning while `raw_ptr` references remain) [15][16]:
- Mitigated **57% of 168 in-scope use-after-free (UAF) reports**.
- Costs **5.5–8% of browser-process memory on desktop**, about 2% on Android, and 1–3% across all processes.
- CPU regressions: +10% p99 main-thread contention, +1.5% First Input Delay on ChromeOS, +1.5% tab startup on Android.
- The cost comes from the 4-byte refcount pushing power-of-two sizes into bigger buckets (4096 → 5120 B). The quarantine itself was only 0.01% of memory [15].

**\*Scan** (quarantine plus heap scanning) regressed Speedometer2 by 8%, or 2% after optimization, and memory by about 12%. Run on MTE hardware so that scanning happens only when a tag wraps around (about 16× fewer scans), the regression was "within noise" with about +1% memory. MTE plus zeroing cost about 2% [17].

### 2.5 Apple: kalloc_type, xzone malloc and Memory Integrity Enforcement (MIE)

**kalloc_type** [18]:
- The compiler generates a type "signature" (pointer versus data per 8 bytes).
- At boot, signature groups are spread **randomly across 200 zones**.
- Virtual address ranges are **sequestered**: never reused across zones.
- Pure-data allocations go to a separate heap.
- Freelists are replaced by an **external bitmap**, so a UAF cannot touch allocator metadata.
- Budget: 0% CPU and 0% memory. Result: "near-zero CPU overhead".

Its stated weaknesses:
- Signature collisions (the median type shares a group with three others).
- Pointers stored in integer-typed fields.

**xzone malloc** (iOS 17; partly derived from mimalloc) [19]:
- Out-of-line segment metadata at unpredictable offsets.
- Only free-list links are inline, and those are **PAC-signed with a sequence number**.
- Separate buckets for data, Objective-C and 1–4 "pointer" types.
- Types come from compiler type descriptors, falling back to **caller PC**.
- Probabilistic guard pages.
- Zero-on-free below 1 KiB.
- MTE tags on blocks up to 32 KiB: retagged on free up to 4 KiB, on allocation from 4–32 KiB. Pure-data allocations are untagged by default.

**MIE** (Sept 2025, iPhone 17 and A19) [20]:
- Synchronous EMTE plus typed allocators plus Tag Confidentiality Enforcement.
- Covers the kernel and more than 70 userland processes.
- EMTE is used "only [for] smaller individual allocations within a type bucket" because tag checking has a cost.
- Apple refuses asynchronous mode because of its race window.
- Apple's offensive team could not rebuild any of six real exploit chains against it.

### 2.6 Android and Pixel MTE

- Pixel 8 (2023) was the first production handset that lets users enable MTE [24].
- Google recommends ASYNC (or ASYMM where available) in production and SYNC for testing [21].
- Android 16 Advanced Protection can turn MTE on; a CTS change notes this shows up in DevicePolicyManager [28].
- Known bypasses: the TikTag speculative tag leak [23], and GPU (Mali) memory accesses that are not tag-checked [25].
- Linux note: `MADV_DONTNEED` and `MADV_FREE` "may have the allocation tags cleared" [26]. Tags cost 4 bits per 16 bytes, which works out to **3.125% tag storage** (my arithmetic from [26]).

### 2.7 GWP-ASan

| Implementation | Defaults and cost |
|---|---|
| LLVM | 1/5000 sample rate, 16 slots, about 40 KiB per process [30] |
| Android | About 70 KiB per process. "Recoverable" mode is the default for apps from Android 14 and is active on about 1% of launches [31] |
| TCMalloc | Samples every 100 MB allocated by default with negligible CPU; under 0.5% CPU at 8 MB; only allocations ≤ 8 KB [29] |
| Chrome on Linux | Mean interval of 1500 × 16^U allocations, i.e. 1500–24000; allocations larger than a page are not sampled [32]. Allocation-heavy microbenchmarks regressed up to 5% from the hooks alone [33] |

### 2.8 mimalloc and snmalloc

**mimalloc secure mode** [34]:
- guard pages around metadata
- per-page-key encoded free lists
- double-free detection
- random free-list initialization

It costs "usually around 10%". Full secure mode adds guard pages at the end of each 64 KiB page, which risks hitting the VMA limit. A separate guarded mode samples allocations and places them next to guard pages, at ≥ 8 KiB each. v3.4.1 (July 2026) separates all metadata from heap objects.

**snmalloc 0.6** protects free lists with an involution on the forward link and a keyed product (a "signed back-edge") that must verify on pop, which also catches double frees. That adds one branch, one multiply, five loads and one store to the fast path. All checks together cost **under 5%** [35][36].

### 2.9 Research systems (all-in costs)

| System | Mechanism | CPU | Memory |
|---|---|---|---|
| MarkUs (S&P'20) | quarantine + marking | 1.1× avg, 2× max [37]; 14.8% [40]; 21% on SPEC2017 [43] | 28.1% [40]; 31.1% [43] |
| MineSweeper (ASPLOS'22) | quarantine + linear sweep (jemalloc) | 5.4% + 9.6% extra thread CPU [38]; 33.4% [43] | 11.1% [38]; 32.5% [43] |
| HushVac (NDSS'24) | page-level sweeping | 4.7% vs 11.4% for prior state of the art [39] | — |
| FFmalloc (Sec'21) | one-time allocation | 2.3% SPEC, 33.1% PARSEC [40] | 61.0% [40]; 98.4% [43] |
| Cling (Sec'10) | call-site type pools | low; peak memory up to +33% (povray) [41] | address space < 150% more |
| Type-After-Type (ACSAC'18) | static types → pools | 4.3% [42] | 17.4% [42]; 23.5% [43] |
| CHERI Cornucopia | capability revocation | < 2% avg, 8.9% worst (FPGA) [44] | quarantine up to ¼ of the heap [45] |
| Cornucopia Reloaded (ASPLOS'24) | load barriers | 4% cycles, 9% bus traffic (Morello, Cornucopia baseline); pauses nearly gone [45] | same |

PoisonCap (2026) adds strict UAF and initialization safety on CHERI [46].

### 2.10 Kernel mechanisms relevant to a userspace allocator

- **`MADV_GUARD_INSTALL`** (Linux 6.13): guard regions as page-table markers instead of VMAs.
  - Early tests: **5× faster mapping calls and 13% fewer VMAs** on an idle Android system [48].
  - Works only on private, anonymous, writable, non-mlocked memory, and zaps the range's existing contents. Guards survive `MADV_DONTNEED` and `MADV_FREE` [49].
  - A late-2025 patch makes install take the **mmap write lock** [55]. Whether it was merged is **UNVERIFIED**.
- **`mseal`** blocks munmap, mremap and mprotect on sealed ranges. Destructive madvise calls are blocked only for callers without write permission [51]. The kernel version that introduced it is **UNVERIFIED**.
- **`init_on_alloc` / `init_on_free`** (kernel zeroing on allocate or free) [47]:

  | Option | hackbench sys time | kernel build wall time | kernel build sys time |
  |---|---|---|---|
  | `init_on_alloc` | +7.75% | −0.13% | +0.57% |
  | `init_on_free` | +7.62% | +8.38% | +24.42% |

- **`RANDOM_KMALLOC_CACHES`**: 16 copies of the kmalloc caches, chosen by caller address plus a per-boot seed. CPU difference "minor"; memory overhead grows with the number of copies [54].
- **`TYPED_KMALLOC_CACHES`** (proposed 2026): driven by Clang 22 **AllocToken** [53].
- **Clang AllocToken** rewrites `malloc(n)` into `__alloc_token_malloc(n, id)`. The default `typehashpointersplit` mode reserves the top half of the ID space for pointer-containing types, and the instrumentation "aims to remain performance neutral" [52].

---

## 3. The summary table

| Defense | What it stops | CPU cost | Memory cost | Who ships it | Source |
|---|---|---|---|---|---|
| Free-list pointer mangling with storage address + alignment check | Blind or partial free-list poisoning (needs a heap leak to bypass); 15/16 forgeries caught | glibc: not measurable; gperftools 0.02% avg, 1.5% worst | 0 | glibc ≥ 2.32, uClibc-NG | [1] |
| Byte-swapped link + shadow copy | Partial overwrite forming a valid pointer; corruption | Not published (UNVERIFIED) | 1 word per *free* slot | PartitionAlloc | [14] |
| Per-page-key encoded links | Overwrite with a known pointer | Part of ~10% secure mode | 0 | mimalloc secure | [34] |
| Signed back-edge queue | Free-list corruption and double free | All snmalloc checks < 5% | 0 | snmalloc ≥ 0.6 | [35][36] |
| PAC-signed links + sequence number | Forged or replayed links | Not published | 0 | xzone (arm64e) | [19] |
| Fully out-of-line metadata (bitmap, no inline list) | Metadata corruption through UAF or overflow | "Near-zero" (kalloc_type) | Bitmap bits | XNU kalloc_type, hardened_malloc, mimalloc v3 | [18][12][34] |
| Metadata region with guards and random placement | Linear overflow into metadata | ~0 | Address space only | PartitionAlloc, snmalloc, hardened_malloc, xzone | [14][36][12][19] |
| Header checksum + chunk state | Header tampering, double free, invalid free | No isolated number (UNVERIFIED) | 8 B per chunk | Scudo | [6] |
| tcache double-free key | Same-thread double free only | ~0 | Uses freed space | glibc | [3] |
| Sized-delete / alignment / type-mismatch checks | Type confusion through delete, mismatched new/free | ~0 | 0 | Scudo (2 of 3 on by default), hardened_malloc | [7][12] |
| Random slot / free-list order | Heap feng shui (layout control) | "Measurable" (hardened_malloc) | 0 | hardened_malloc, Scudo, mimalloc secure, snmalloc | [12][6][34] |
| Random type→bucket assignment per boot or process | Reliable cross-type reallocation | Near-zero; "minor" | Fragmentation; grows with copies | XNU, Linux ≥ 6.6 | [18][54] |
| Type / data partitioning | Pointer/data overlap UAF; data spraying | 0% budget (Apple); TaT 4.3% | 17–24% (TaT) | XNU, xzone, libpas, Chrome partitions | [18][20][42][43] |
| Address-space sequestering (no VA reuse across types) | Cross-type UAF | ~0 | Address space and page tables | XNU, hardened_malloc, Cling | [18][12][41] |
| Guards on large allocations and regions | Overflow past the end | 2–3× syscalls (hardened_malloc) | 1–2 VMAs each (0 with `MADV_GUARD_INSTALL`) | Scudo, hardened_malloc, PartitionAlloc | [6][12][14][48] |
| Guard slab between every slab | Slab-to-slab overflow | "Indirect" | VMAs; needs `max_map_count` ≈ 1M | hardened_malloc default | [12] |
| Sampled guarded allocation (GWP-ASan) | *Detects* UAF and overflow in the field | Negligible; < 0.5% at 8 MB; ≤ 5% hook cost | 40–70 KiB; ≤ 512 KB | Chrome, Android, Scudo, TCMalloc | [29]–[33] |
| Zero on allocation | Uninitialized reads, information leaks | Build −0.13% wall; hackbench +7.75% sys | 0 | kernel `init_on_alloc`, Scudo option | [47][7] |
| Zero on free (+ write-after-free check) | Leaks, some UAF | Build +8.38% wall; scales with size | 0 | hardened_malloc (small), xzone (< 1 KiB) | [47][12][19] |
| 8-byte canary with zero first byte | Small and C-string overflows | "Slight" | 8 B per slot | hardened_malloc | [12] |
| Bounded FIFO / random quarantine | Short-window UAF | "Fairly costly"; off by default | Quarantine budget | Scudo (option), hardened_malloc | [6][12] |
| BackupRefPtr refcount | Exploitation of 57% of in-scope UAFs | p99 contention +10% | +5.5–8% browser process; +1–3% overall | Chrome (non-renderer processes) | [15][16] |
| Heap-scan quarantine | UAF reuse | 2–33% | +11–33% | Research; Chrome experiment | [17][37][38][43] |
| One-time allocation | All UAF reuse | 2.3–33% | +61–98% | Research | [40][43] |
| MTE, asynchronous | Probabilistic UAF and overflow; deterministic for adjacent overflow | Pixel 8 1.08–1.18× | 3.125% tag storage + 16 B rounding | Android (opt-in), GrapheneOS | [21][22][26][13] |
| MTE / EMTE, synchronous | Same, with no race window | X3 1.56× (6.64× outlier); AmpereOne 1.10×; M5 1.02× | Same | Apple MIE | [20][22] |
| Pkey-sealed metadata | Stray writes to metadata | "Significant" | 0 | Off everywhere | [12] |
| CHERI revocation | Deterministic heap UAF | < 2% avg (FPGA); 4% cycles (Morello) | ≤ ¼ of the heap | CheriBSD (research) | [44][45] |

**How the costs group.**
- **Nearly free:** where metadata lives, link encoding, state checks, mismatch checks, type and VA isolation, random bucket assignment, sampled guards, and guards on large allocations or regions (if `MADV_GUARD_INSTALL` is available).
- **Moderate (2–10%):** slot randomization, full free-list integrity checks, canaries, capped zero-on-free, asynchronous MTE, BackupRefPtr.
- **Expensive:** quarantines that are large enough to matter, per-allocation guards, never-reuse schemes, pointer sweeping, synchronous MTE on big cores, pkey sealing.

---

## 4. Faults in the v1 security tiers (RESEARCH_AND_DESIGN.md §5.6)

1. **v1 confuses two mechanisms.** It describes "XOR-encoded with a per-process secret (like glibc's safe-linking)". Safe-linking keys on the *storage address*, protected by ASLR, not on a secret [1].
   - Analysis: a single global XOR secret is recovered from one leaked encoded/decoded pair, after which every link can be forged.
   - Better options: snmalloc-style signed back-edges [36], or PAC on arm64e [19]. Best: no inline links at all, as kalloc_type does [18].
2. **v1 contradicts itself on metadata.** It claims "out-of-line metadata" but keeps inline free lists, the same exception PartitionAlloc documents [14]. These inline links are exactly what how2heap's tcache techniques target [2].
3. **The "per-span free bitmap" does not cover remote frees for free.** Frees that arrive from other threads must set the bit atomically, or the bitmap misses cross-thread double frees, which is precisely the gap glibc's tcache key admits [3]. The cost of an atomic test-and-set on remote frees is **UNVERIFIED** and must be measured.
4. **Tier 0 is missing the defense with the best cost-to-value ratio: type and data isolation plus VA sequestering.** Apple built it on a 0%/0% budget [18], and it is the base of MIE [20].
5. **"1 in 10⁵" sampling is sparser than every production default** (LLVM 1/5000 [30], Chrome 1/1500–1/24000 [32]). The CPU cost comes from the hook and counter, not from the rate [33]. So a sparser rate buys little speed and loses detection.
6. **Tier 1 "guard pages between spans" reproduces hardened_malloc's VMA blow-up** [12] unless it uses `MADV_GUARD_INSTALL`. That call needs Linux 6.13+ [48], and may now take the mmap write lock [55], so it cannot run on hot paths.
7. **Tier 1 "zero-on-free" is uncapped.** It costs +8.38% wall time on a kernel build [47]. Production allocators cap it (xzone < 1 KiB [19]).
8. **Tier 2 MTE is underspecified.** It ignores:
   - asynchronous mode's race window [20],
   - synchronous mode's outliers [22],
   - TikTag [23],
   - tag clearing after `MADV_DONTNEED` [26],
   - glibc dropping MTE support [5],
   - uncertain hardware availability [27].
9. **v1 has no quarantine design.** A small FIFO quarantine is flushable. Analysis: an attacker who can trigger N bytes of allocations flushes it. Only sweeping or revocation gives determinism [38][45].

---

## 5. Recommended tiered security design for sohamalloc

Principles:
- Spend the security budget on *placement* (free), not on *retention* (expensive).
- Every check fails closed with an explainable report.
- One knob: `SOHAMALLOC=…secure`.

### S0: default, always on (target ≤ 1–2% CPU, ≤ 1% memory; must be measured)

1. **No inline metadata for small objects.**
   - Thread and CPU caches hold pointers in out-of-line arrays.
   - Each slab keeps an allocated-bitmap in a separate metadata region. That region is reserved once, surrounded by guards, randomly offset, and its reservation sealed with `mseal` where available [51].
   - Rationale: kalloc_type's external bitmap made UAF unable to reach metadata at about 0% CPU [18].
   - Caveat (**UNVERIFIED**): jemalloc's cache_bin and TCMalloc's per-CPU caches already store pointers in arrays. I did not re-verify their exact layouts in this pass.
2. **If any in-band list survives** (for example a slab-local refill list), use one of:
   - `link = P ^ (L >> 12) ^ slab_secret`, plus a check that the decoded pointer lies inside the *same slab* on a slot boundary. Analysis: this is strictly tighter than glibc's alignment-only check [1].
   - snmalloc's signed queue (< 5% for all checks) [35][36].
3. **Deterministic `free` validation.** A pagemap lookup, which the allocator needs anyway, plus a slot-boundary check plus a bitmap bit catches invalid free and double free, including cross-thread, using atomic set on the remote path. Also check sized-delete and alignment mismatches, as Scudo does by default [7].
4. **Isolation by construction.**
   - (a) Never hand a slab's address range to a different size class or bucket; purge pages but keep the virtual range [18][12].
   - (b) Large and huge allocations live in a separate reserved range with guards installed **once at reservation** via `MADV_GUARD_INSTALL`, falling back to `PROT_NONE`. Reuse cached large spans with their guards intact; guards survive `MADV_DONTNEED` [48][49].
   - (c) When the binary is built with Clang ≥ 22 AllocToken, export `__alloc_token_*` and split pointer-free from pointer-containing sizes, at zero runtime cost [52][53].
5. **Initialization-time randomization only.** Random slab start offsets and a shuffled initial free order, like mimalloc secure's "random order" initialization [34]. Per-operation randomization is moved to S1.
6. **Production bug detection.** A GWP-ASan-style pool:
   - 16–64 slots, mean sample interval about 1/5000–1/20000, allocations ≤ 1 page only,
   - guards installed once,
   - alloc and free stack traces,
   - a recoverable mode modelled on Android's [30][31][32].
   - Fixed memory of about 40–70 KiB.
7. **`calloc`** reuses known-zero pages; there is no general zero-on-alloc. This matches v1 §5.5 and avoids hackbench-style costs [47].

### S1: hardened (target ≤ 5–10%; analogues are snmalloc < 5% and mimalloc ~10% [35][34])

- Random slot selection on each allocation [12].
- Zero-on-free for blocks ≤ 1 KiB [19], with a write-after-free check on reuse [12].
- **Call-site buckets:** 2–4 buckets per size class for classes ≤ 1 KiB, chosen by `hash(return address ^ per-exec secret)`, like xzone's PC fallback [19] and `RANDOM_KMALLOC_CACHES` [54]. This is cheap because it reads one return address, not a backtrace [18].
- Guard slab every N slabs via `MADV_GUARD_INSTALL`, never `PROT_NONE` VMAs on ≥ 6.13 kernels [12][48].
- Optional 8-byte canary with a zero first byte for classes where it does not push the size into the next class. This avoids BackupRefPtr's 4096 → 5120 problem [15][12].
- A **byte-budgeted** FIFO + random quarantine, for example ≤ 1% of the heap or 256 KiB [10][12], documented as best-effort.
- `dealloc_type_mismatch` checks [7].

### S2: hardware-assisted (arm64 with MTE; target 5–20% depending on the core [22])

- Tag only allocations ≤ 32 KiB and use guards above that. This improved some benchmarks by up to 0.2× (e.g. 1.29× → 1.08×) and the overall geomean by 0–2% [22]. xzone uses the same threshold [19].
- Retag on free for blocks ≤ 4 KiB and on allocation for 4–32 KiB [19].
- Exclude the previous tag and both neighbours' tags; reserve tag 0 for free slots, so overflows into neighbours are caught deterministically and UAF until reuse [12].
- Retag after any purge, because `MADV_DONTNEED` and `MADV_FREE` may clear tags [26].
- Use the fastest tagging instruction mix, `DC GVA` + `ST2G` + `STG` [22].
- Mode: request SYNC|ASYNC and let the kernel pick the per-CPU preferred mode (async, asymm or sync) [26].
  - `secure` profile forces SYNC, following Apple's position [20].
  - Default is ASYMM where available [21].
- Keep the S0 and S1 type buckets underneath, because MTE inside a bucket is how Apple combines the two [20].
- Assume tags can leak through speculation [23] and that DMA and GPU accesses are unchecked [25].
- x86-64 has no shipping MTE equivalent: **UNVERIFIED** from primary sources here. S2 degrades to S1 there.

### S3: paranoid or opt-in

- MineSweeper-style background sweeping quarantine: 5–33% CPU, 11–33% memory [38][43].
- On MTE hardware, the *Scan hybrid that scans only when a tag wraps, for about 16× fewer scans [17].
- An optional C++ `soham::ref_ptr` / BackupRefPtr-style API with an out-of-line refcount, so the 4-byte bucket bump does not happen [15]. The cost of the extra lookup is **UNVERIFIED**.
- FFmalloc-style one-time allocation for short-lived processes only [40].

### Weaknesses of this recommendation

- **Array caches and bitmaps** add metadata traffic, an extra cache line per operation. This is unmeasured here and needs a mimalloc-bench run before S0 is called "≤ 2%".
- **AllocToken needs recompilation.** `LD_PRELOAD` users get only call-site buckets. Analysis: wrapper functions like `xmalloc` collapse all call sites into one PC, which is the problem Cling paid to fix by unwinding wrappers [41].
- **More buckets mean more fragmentation**, which conflicts with sohamalloc's low-RSS goal. Apple capped kalloc_type at 200 zones for this reason [18]. The memory cost per bucket count must be measured.
- **Probabilistic defenses with crash-on-detect** (16-bit checksums, 4-bit tags) can be brute-forced against servers that fork without re-randomizing. Analysis: re-key secrets in children via `pthread_atfork`.
- **Kernel dependencies:** `MADV_GUARD_INSTALL` needs 6.13+, and older kernels hit the 65530 `max_map_count` default [50]. MTE availability is shrinking on some hardware [27].

---

## Sources

1. https://research.checkpoint.com/2020/safe-linking-eliminating-a-20-year-old-malloc-exploit-primitive/
2. https://github.com/shellphish/how2heap
3. https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=malloc/malloc.c;hb=HEAD
4. https://sourceware.org/git/?p=glibc.git;a=history;f=malloc/malloc.c;hb=HEAD
5. https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=NEWS;hb=HEAD
6. https://www.llvm.org/docs/ScudoHardenedAllocator.html
7. https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/scudo/standalone/flags.inc
8. https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/scudo/standalone/allocator_config.h
9. https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/scudo/standalone/combined.h
10. https://source.android.com/docs/security/test/scudo
11. https://protonaosp.org/performance
12. https://github.com/GrapheneOS/hardened_malloc/blob/main/README.md
13. https://grapheneos.org/features
14. https://chromium.googlesource.com/chromium/src/+/main/base/allocator/partition_allocator/PartitionAlloc.md
15. https://security.googleblog.com/2022/09/use-after-freedom-miracleptr.html
16. https://security.googleblog.com/2024/01/miracleptr-protecting-users-from-use.html
17. https://security.googleblog.com/2022/05/retrofitting-temporal-memory-safety-on-c.html
18. https://security.apple.com/blog/towards-the-next-generation-of-xnu-memory-safety/
19. https://cs.iossec.tech/libmalloc/latest/source/doc/xzone_malloc.md
20. https://security.apple.com/blog/memory-integrity-enforcement/
21. https://source.android.com/docs/security/test/memory-safety/arm-mte
22. https://arxiv.org/html/2601.11786v1 (USENIX Security '26: https://www.usenix.org/conference/usenixsecurity26/presentation/noh)
23. https://arxiv.org/abs/2406.08719
24. https://projectzero.google/2023/11/first-handset-with-mte-on-market.html
25. https://github.blog/security/vulnerability-research/gaining-kernel-code-execution-on-an-mte-enabled-pixel-8/
26. https://docs.kernel.org/arch/arm64/memory-tagging-extension.html
27. https://alternativeto.net/news/2026/8/grapheneos-might-skip-the-pixel-11-series-over-the-lack-of-mte-hardware-security-support/ ; https://www.phonearena.com/news/pixel-11-hardware-memory-tagging-mte-grapheneos_id182967 (secondary)
28. https://android.googlesource.com/platform/cts/+/d4455acf83d
29. https://google.github.io/tcmalloc/gwp-asan.html
30. https://llvm.org/docs/GwpAsan.html
31. https://developer.android.com/ndk/guides/gwp-asan
32. https://chromium.googlesource.com/chromium/src/+/main/docs/gwp_asan.md
33. https://www.chromium.org/Home/chromium-security/articles/gwp-asan
34. https://github.com/microsoft/mimalloc/blob/main/readme.md
35. https://github.com/microsoft/snmalloc/blob/main/docs/security/README.md
36. https://github.com/microsoft/snmalloc/blob/main/docs/security/FreelistProtection.md
37. https://www.cl.cam.ac.uk/~tmj32/papers/docs/ainsworth20-sp.pdf
38. https://www.cl.cam.ac.uk/~tmj32/papers/docs/erdos22-asplos.pdf
39. https://www.ndss-symposium.org/wp-content/uploads/2024-804-paper.pdf
40. https://www.usenix.org/system/files/sec21-wickman.pdf
41. https://www.usenix.org/legacy/event/sec10/tech/full_papers/Akritidis.pdf
42. https://download.vusec.net/papers/tat_acsac18.pdf
43. https://arxiv.org/pdf/2402.03373
44. https://www.microsoft.com/en-us/research/publication/cornucopia-temporal-safety-for-cheri-heaps/
45. https://www.microsoft.com/en-us/research/uploads/prod/2024/02/revocation3-paper.pdf
46. https://arxiv.org/pdf/2605.13210
47. https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=6471384af2a6530696fc0203bafe4de41a23c9ef
48. https://lwn.net/Articles/996126/
49. https://lwn.net/Articles/1000908/
50. https://docs.kernel.org/admin-guide/sysctl/vm.html
51. https://docs.kernel.org/userspace-api/mseal.html
52. https://clang.llvm.org/docs/AllocToken.html
53. https://lkml.iu.edu/hypermail/linux/kernel/2604.0/04797.html
54. https://lkml.rescloud.iu.edu/2307.1/07505.html
55. https://lkml.iu.edu/hypermail/linux/kernel/2510.3/07966.html