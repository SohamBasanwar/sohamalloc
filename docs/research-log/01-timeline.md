# sohamalloc research log, part 1: project timeline

**Project:** sohamalloc, a general-purpose memory allocator (a replacement for `malloc`/`free` and C++ `new`/`delete`) written from scratch in C11.
**Author:** Soham Basanwar.
**Repository:** `/home/user/sohamalloc`, branch `sohamalloc-dev`, HEAD `26caa9e` when this was written.
**Period covered:** 2026-10-18T17:55:06Z (first prompt) to 2026-10-19T14:30:25Z. That is the last main-transcript event examined (T:2625); this file was written at about 14:35Z. Work was still running at the cut-off (§3.9).
**All times are UTC.**

This file is the chronological record. It lists:

- every prompt the author gave, verbatim;
- every phase of work;
- every git commit;
- every benchmark campaign;
- every significant defect and how it was fixed;
- interruptions and restarts;
- wall-clock durations;
- the artifacts produced.

Other files in `docs/research-log/` cover the analysis.

---

## 0. Sources, citation keys and method notes

### 0.1 Citation keys

| Key | Source |
|---|---|
| `T:n` | Line *n* (0-based) of the main session transcript, `<session transcript (JSONL)>`. It had 2,631 JSONL events at 14:31:56Z and is still growing; each event has an ISO `timestamp`. Times quoted here are the event timestamps, truncated to the second. |
| `wf_X/agent-Y` | A sub-agent transcript under `…/ddb08eb6-359c-5f87-bd66-daed081ca7fb/subagents/workflows/wf_X/agent-Y.jsonl`. An agent's start and end are its first and last event timestamps. Its role label comes from the `journal.jsonl` file in the same directory. |
| `git abc1234` | A commit in the current history (`git log --format=fuller`). §4.2 maps these to the original hashes that the transcript and `bench/out/run-*/env.txt` mention. |
| `bench/out/…` | Benchmark reports, CSVs and environment files (git-ignored, on local disk). |
| `SP/…` | The session scratch directory `<scratch dir>/` (ad-hoc programs, logs and profiles). |

### 0.2 How the work was done (methods)

- **The author directed the work in natural language.** The author's inputs were the prompts in §2.1 and the decisions in §2.2.
- **AI-assisted tooling did the hands-on work.** That covers literature and source research, critique, design, implementation, testing, benchmarking and documentation. It consisted of:
  - an interactive AI coding assistant in the main session (the "main session" below);
  - several orchestrated multi-agent workflows. These ran AI agents in parallel, each with an assigned role: researcher, critic, designer, judge, synthesiser, red-teamer, implementer, test engineer or benchmark engineer.
- **Every file write in the transcripts was made by this tooling.** The sources cannot show whether the author edited files outside the recorded session.
- **Agents ran at most two at a time.** The main session states "This machine has 4 CPUs, so only two agents run at a time" [T:500]. The agents' start and end times (§8) fit this.

### 0.3 Environment, as recorded in the transcript

| Item | Value | Source |
|---|---|---|
| Machine | Cloud VM, Intel Xeon @ 2.80 GHz, 4 CPUs (1 thread per core), 15 GB RAM | T:346, T:1209, T:1214 |
| Kernel | 6.18.44-fc-v139 | bench/out/run-*/env.txt |
| OS and toolchain | Ubuntu 24.04 packages; gcc 13.3.0, clang 18.1.3, cmake 3.28.3, glibc 2.39 | T:346 |
| Installed at 00:44–00:45Z | libmimalloc (2.1), gperftools tcmalloc_minimal (4.5.16), aarch64 cross-gcc, qemu-user, musl-tools, mingw-w64. jemalloc 5.3.0 was already installed | T:349–354 |
| Installed at 00:58–01:00Z | armhf cross-gcc/g++, aarch64 g++, mingw g++, wine 9.0. i386 multilib conflicted with the cross-compilers and was dropped; armhf became the 32-bit target | T:406–434 |
| Benchmark suite | mimalloc-bench, shallow clone into SP/mimalloc-bench at 00:45:21Z. The bench harness pins commit 3913826 | T:353, T:1335 |
| Profiling | valgrind/callgrind 3.22. No `perf` binary for this kernel | T:970, T:1806 |
| Transparent hugepages | `enabled=madvise`, `defrag=madvise`; mTHP 2048 kB = inherit, other sizes never | T:1481 |

---

## 1. Summary table of phases

Durations are wall-clock. Phases run by parallel agents overlap.

| # | Phase | Start | End | Duration | Key outputs / evidence |
|---|---|---|---|---|---|
| 1 | Survey research and first design (v1) | 2026-10-18T17:55:06Z | 2026-10-18T18:00:34Z | 5 m 28 s | `docs/RESEARCH_AND_DESIGN.md`; git ba06bb7 |
| 2 | Deep research (5 research agents) | 2026-10-18T18:08:03Z | 2026-10-18T19:24:41Z | 1 h 16 m 38 s | `docs/research/01–05` (wf_b8ba2d9a-fb2) |
| 3 | Self-critique of v1 (3 critic agents) | 2026-10-18T18:59:00Z | 2026-10-18T19:59:48Z | 1 h 00 m 48 s | 101 faults → `docs/research/00-v1-critique.md` |
| 4 | Competing designs A, B, C (3 design agents) | 2026-10-18T19:59:51Z | 2026-10-18T20:21:26Z | 21 m 35 s | `docs/designs/A–C` |
| — | *Interruption: usage limit* | 2026-10-18T20:21:28Z | 2026-10-18T23:57:57Z | 3 h 36 m 29 s | T:195–226 |
| 5 | Research material archived and committed | 2026-10-18T23:58:24Z | 2026-10-18T23:59:41Z | 1 m 17 s | git e7aaca2 |
| 6 | Judging (2 judge agents) | 2026-10-18T23:58:21Z | 2026-10-19T00:02:55Z | 4 m 34 s | scores in §3.3 |
| 7 | Synthesis into plan v2 | 2026-10-19T00:02:57Z | 2026-10-19T00:17:27Z | 14 m 30 s | plan v2 (journal) |
| 8 | Red team (2 agents) | 2026-10-19T00:17:29Z | 2026-10-19T00:30:45Z | 13 m 16 s | 60 faults (9 critical) |
| 9 | Final revision into plan v3 | 2026-10-19T00:30:47Z | 2026-10-19T00:53:24Z | 22 m 37 s | `docs/SOHAMALLOC_PLAN.md` |
| 10 | Plan verification and commit | 2026-10-19T00:53:30Z | 2026-10-19T00:57:00Z | 3 m 30 s | git 61279d9 |
| 11 | Toolchain provisioning | 2026-10-19T00:44:39Z | 2026-10-19T01:00:12Z | two bursts: 59 s + 1 m 24 s | T:345–419 |
| 12 | v0.1 implementation spec | 2026-10-19T00:57:10Z | 2026-10-19T01:00:25Z | 3 m 15 s | `docs/IMPL_SPEC_v0.md`; git d295a53, cbdd52c |
| 13 | Parallel build, attempt 1 (lost in container restart) | 2026-10-19T01:02:02Z | 2026-10-19T01:22:37Z | 20 m 35 s | OS layer, header, tests survived on disk |
| 14 | Recovery and build attempt 2 (stopped by usage limit) | 2026-10-19T01:22:47Z | 2026-10-19T01:30:59Z | 8 m 12 s | git 1560b2e |
| — | *Interruption: usage limit* | 2026-10-19T01:30:59Z | 2026-10-19T05:42:27Z | 4 h 11 m 28 s | T:505–604 |
| 15 | Core allocator implementation (main session) | 2026-10-19T05:42:56Z | 2026-10-19T06:01:34Z | 18 m 38 s | git 67a8520 |
| 16 | Debugging round 1: stress, sanitizers, cross-platform | 2026-10-19T06:01:34Z | 2026-10-19T06:09:23Z | 7 m 49 s | git 3d2dd95 |
| 17 | Performance round 1, memory return, defect fixes, docs | 2026-10-19T06:09:32Z | 2026-10-19T06:32:34Z | 23 m 02 s | git 44e39e4 … 5ff3c75 |
| 18 | Test-suite completion (test agent) | 2026-10-19T06:02:21Z | 2026-10-19T07:21:38Z | 1 h 19 m 17 s (cut off) | `tests/…` (wf_dd2453d3-1f1) |
| 19 | Benchmark harness and campaign B1 (bench agent) | 2026-10-19T06:02:21Z | 2026-10-19T07:02:20Z | 59 m 59 s | `bench/…`; B1 |
| — | *Interruption: usage limit, then waiting for the author* | 2026-10-19T07:21:38Z | 2026-10-19T12:42:44Z | 5 h 21 m 06 s | T:1337–1361 |
| 20 | Debugging round 2 and cross-platform verification | 2026-10-19T12:43:03Z | 2026-10-19T13:06:41Z | 23 m 38 s | git 7525796, eabd154 |
| 21 | Performance round 2, including a signal-safety deadlock fix | 2026-10-19T13:06:41Z | 2026-10-19T13:47:37Z | 40 m 56 s | git 273bcb9; B2, B3 |
| 22 | Benchmark campaign B4, and fast-path change in parallel | 2026-10-19T13:47:37Z | 2026-10-19T14:02:41Z | 15 m 04 s | B4; git 13bcf17 |
| 23 | Post-commit verification (native + 3 sanitizers) | 2026-10-19T14:03:05Z | 2026-10-19T14:09:20Z | 6 m 15 s | T:2457 |
| 24 | Authorship and provenance changes (history rewrite, branch) | 2026-10-19T14:09:56Z | 2026-10-19T14:17:36Z | 7 m 40 s | §3.8 |
| 25 | Cross-target re-verification of 13bcf17; 32-bit/LLP64 defect found and fixed | 2026-10-19T14:18:35Z | 2026-10-19T14:30:02Z | 11 m 27 s | git ead7c98, 26caa9e |
| 26 | Research record and adversarial review workflows | 2026-10-19T14:14:54Z | still running | — | §3.9 |

**Totals.** From first prompt to the last event examined is **20 h 35 m 19 s** (17:55:06Z to 14:30:25Z). Of that:

- **Active windows:** 7 h 26 m 16 s.
  - 17:55:06–20:21:28 = 2 h 26 m 22 s
  - 23:57:57–01:30:59 = 1 h 33 m 02 s
  - 05:42:27–07:21:38 = 1 h 39 m 11 s
  - 12:42:44–14:30:25 = 1 h 47 m 41 s, still open
- **Interruptions:** 13 h 09 m 03 s in three blocks (§7).

**By activity:**

| Activity | Elapsed |
|---|---|
| Research to final plan (phases 1–10, excluding the interruption) | 2 h 26 m 22 s + 59 m 03 s (23:57:57–00:57:00) ≈ 3 h 25 m |
| Implementation, testing and optimisation (phase 15 to the commit of 13bcf17 at 13:54:19Z) | 1 h 39 m 11 s (05:42:27–07:21:38) + 1 h 11 m 35 s (12:42:44–13:54:19) ≈ 2 h 51 m of active windows |

---

## 2. Author inputs

### 2.1 Prompts, verbatim

All eight prompts are marked `origin.kind = human` in the transcript. Some were queued while the assistant was working and were delivered at its next step. Both the submission time and the delivery time are given where they differ.

**P1. 2026-10-18T17:55:06Z** [T:0, T:2]
> I actually was studying about mallocs, and I want to make my own malloc that is most efficient and also the best that is possibly can be made, different from all the others that are available in the internet by all the providers, and make it as efficient and as user friendly as possible and kernel friendly as well. Effective use of memory and so on. Do some research about it to understand what kind of things are the biggest limitation for all the kinds of malloc that are available in the current time, and what are the prospects that are needed to make a malloc, and how can we overcome the current limitations and make a malloc that is the best overall.

**P2. 2026-10-18T18:05:52Z** [T:154, T:156]
> now do more research on it and try to dig out the problems in your own research and create a plan to make a new malloc that is the best and most efficient in the current era, and try to get as much information as possible on how you can make it, what the current limitations are, how you can overcome them, and how you can add even better features, methods to make the malloc better and the whole process of it. Basically, dynamic allocation of memory is what we are trying to make. Pick faults in your own solutions to create the best solution at the end.

**P3. 2026-10-18T23:57:57Z** [T:234, T:238] (queued; delivered while the assistant was working)
> Continue

**P4. 2026-10-19T00:38:51Z** [T:315, T:317]
> Update

**P5. 2026-10-19T00:44:20Z** [T:334, T:336]
> After that you do a final check to make it in C for best and fast, make a presentation with images and finalize the code, for Sohammalloc
> Make the malloc compatible with devices and also check it, find faults, remake , and make presentations and workflow step by step diagram in presentation only after the score is very very high and the malloc is unique and best of all existing malloc

**P6. 2026-10-19T05:42:27Z / 05:42:29Z** [T:602, T:614] (delivered right after an interruption marker, see §7)
> Continue

**P6b. 2026-10-19T05:42:34Z** [T:617, T:624] (a second, queued copy)
> Continue

**P7. 2026-10-19T12:42:44Z** [T:1359, T:1377]
> Continue

**P8. Submitted 2026-10-19T14:05:20Z, delivered 14:09:20Z** [T:2458, T:2460] (queued while the verification matrix ran)
> Just to confirm, we are trying to create our own malloc. Soham malloc. And not trying to improve the existing one. Is that what is happening? Also, a question. All the work that is being done will have me as the sole creator of this, and no relation to Claude or Anthropic or anything. And last, is it, I would want you to record like every sort of time things and everything, and if it is worth a research paper or a scientific paper, I want to hear if it is worth making a paper or not, and if it is, why, and also if yes, record everything from the beginning. That might be needed to make a research paper / scientific method / scientific paper.

### 2.2 Decisions taken through structured questions

The answers are verbatim. The author's e-mail address is redacted.

| Asked | Answered | Question (paraphrased) | Author's answer |
|---|---|---|---|
| 14:10:26Z [T:2479] | 14:11:47Z [T:2484] | Rewrite the 19 existing commits so the author is the committer and the tool-generated attribution trailers are removed, or change only future commits? | "Rewrite all history" |
| 14:10:26Z | 14:11:47Z | Name to use for commits and for the paper record | "Soham Basanwar  [e-mail redacted]" |
| 14:10:26Z | 14:11:47Z | Keep or rename the tool-named working branch | "Rename to sohamalloc-dev" |
| 14:15:04Z [T:2547] | 14:17:12Z [T:2548] | The remote refused the push because of the account's e-mail privacy setting. Re-author with a no-reply address, or turn the block off? | "I'll turn off the email block" |

### 2.3 Automated, non-author inputs (not prompts)

- **Repository stop hook** asking for untracked or uncommitted changes to be committed: 2026-10-18T18:00:09Z, 23:58:57Z; 2026-10-19T06:32:06Z, 13:52:53Z [T:129, T:281, T:1270, T:2417].
- **Workflow and task completion notifications:**
  - 2026-10-18T20:21:30Z [T:194]
  - 2026-10-19T00:53:27Z [T:365]
  - 2026-10-19T01:30:59Z [T:504]
  - 2026-10-19T07:21:38Z [T:1335]
  - 2026-10-19T14:02:41Z [T:2438], the background benchmark
- **Container-restart notice:** 2026-10-19T01:22:37Z [T:452–453].
- **Worker-restart notices:** 20:30:09Z, 20:44:38Z, 23:57:52Z, 01:31:40Z, 01:51:34Z, 02:54:52Z, 04:42:10Z, 05:42:29Z, 08:34:19Z and 12:42:45Z [T:200, 213, 226, 510, 523, 536, 568, 604, 1342, 1361].
- **Scheduled self check-in** (created 01:23:35Z for 02:54:00Z) [T:497–498]:
  - delivered as queued notifications at 02:54:53–54Z and again at 04:42:12–13Z [T:544–595];
  - read at 05:42:55Z [T:620].
- **Automatic context compaction** of the main session: 2026-10-19T13:02:05Z [T:1553–1556].

---

## 3. Detailed chronology

### 3.1 Phase 1: survey research and v1 design (2026-10-18T17:55:06Z – 18:00:34Z)

- **17:55:11Z** The first action inspected the repository. It was empty, with no commits [T:21, T:150].
- **17:55:17Z – 17:56:30Z** 19 web searches and 1 document fetch ran in parallel batches, plus PDF text extraction of the ASPLOS'24 warehouse-scale TCMalloc characterisation paper [T:36–110]. Topics:
  - mimalloc, tcmalloc, jemalloc, snmalloc and Mesh;
  - TCMalloc Temeraire, the hugepage-aware allocator;
  - rseq per-CPU caches;
  - glibc arena bloat;
  - `MADV_FREE`, `MADV_DONTNEED`, `process_madvise` and `MADV_COLLAPSE`;
  - jemalloc's archival;
  - P0901 and C23 `free_sized`;
  - hardened allocators and MTE;
  - the "datacenter tax";
  - mseal and mTHP;
  - the Robson bound;
  - LLAMA;
  - `MADV_GUARD_INSTALL`;
  - MiraclePtr;
  - mimalloc v3;
  - Mallacc.
- **17:57:42Z** The main session reported that research was done. At **17:59:39Z** it wrote `docs/RESEARCH_AND_DESIGN.md` (v1, 510 lines) [T:120–121].
- **18:00:07Z** Summary to the author [T:128]:
  - There is no single "best possible malloc": Robson's bound rules it out, and the goals trade off against each other.
  - It listed ten current limitations.
  - It proposed differentiators, including cache indexing by rseq `mm_cid`, lifetime-grouped placement and container-pressure-driven memory release. The self-critique later found several of these to have prior art (§3.2).
- **18:00:22Z** Commit **ba06bb7**, pushed at 18:00:29Z to a new tool-named branch. It was the repository's first commit, so that branch became the remote default branch [T:138–150].

### 3.2 Phases 2–4: deep research, self-critique, competing designs (2026-10-18T18:05:52Z – 20:21:30Z)

- **18:06:17Z** The main session loaded its workflow-authoring reference. At **18:08:03Z** it launched workflow **wf_b8ba2d9a-fb2**, "Deep research on allocators, adversarial critique of v1 design, competing designs, judged synthesis, red-team, final plan" [T:169–188]. The plan had six stages and 17 agents:
  - 5 research sweeps;
  - 3 critics;
  - 3 designs;
  - 2 judges;
  - 1 synthesis;
  - 2 red-team reviewers;
  - 1 final revision.

The **research agents** produced five reports, saved verbatim later (§3.3) [wf_b8ba2d9a-fb2]:

| Agent (role) | Start | End | Tool calls | Output |
|---|---|---|---|---|
| research:internals | 18:08:03Z | 18:41:57Z | 177 | `docs/research/01-allocator-internals.md`: source read at mimalloc `dev3`@1d03ecbd19a0, tcmalloc `master`@1c7cf2496293, jemalloc `dev`@6390a79a935f, snmalloc `main`@f126f1c94d62, hardened_malloc `main`@bf37d54560a6, rpmalloc `develop`@038e1d77e9f8 [T:272] |
| research:kernel | 18:08:06Z | 18:35:12Z | 137 | `02-linux-kernel-interfaces.md`: kernel docs at 7.3.0-rc7. Findings include `process_madvise` self-targeting changes in 6.12–6.17 [T:256] |
| research:academic | 18:35:15Z | 18:55:47Z | 122 | `03-academic-research.md`: literature 2017–2026 |
| research:security | 18:41:59Z | 18:58:58Z | 94 | `04-security-and-hardening.md` |
| research:production | 18:55:49Z | 19:24:41Z | 172 | `05-production-lessons-and-evaluation.md` |

The **self-critique** of v1 used three critic agents. Their faults were merged into `docs/research/00-v1-critique.md` (counts from the journal):

| Critic | Start | End | Critical | Major | Minor |
|---|---|---|---|---|---|
| critic:facts (fact-check) | 18:59:00Z | 19:29:10Z | 1 | 12 | 13 |
| critic:design (hostile design review) | 19:24:43Z | 19:59:48Z | 7 | 28 | 9 |
| critic:gaps (completeness) | 19:29:13Z | 19:52:43Z | 4 | 22 | 5 |
| **Total** | | | **12** | **62** | **27** |

The total is 101 faults [T:264]. The main session's summary of the most important ones [T:280, T:313]:

- `mm_cid` cache indexing is not new: TCMalloc already does it.
- The size-class waste was wrong: 4 classes per doubling wastes up to about 20–25%, not 12.5%.
- "Most allocators are hugepage-oblivious" is out of date.
- The draft had no design for the pointer-to-metadata lookup.
- The lifetime predictor breaks under wrappers such as `operator new`.
- A TCMalloc document found in the downloads shows that TCMalloc already has an experimental lifetime-based allocator. This is prior art for the v1 lifetime idea.

The **competing designs** came from three design agents:

| Design | Agent | Start | End |
|---|---|---|---|
| A: performance and locality first | design:perf | 19:59:51Z | 20:12:43Z |
| B: memory efficiency and kernel citizenship first | design:memory | 19:59:56Z | 20:10:01Z |
| C: robustness, security and usability first | design:robust | 20:10:03Z | 20:21:26Z |

- **20:21:28Z – 20:21:30Z** The next six agents (judge:1, judge:2, synthesize, redteam:1, redteam:2, finalize) failed within a second each, with "You've hit your session limit · resets 11:50pm (UTC)". The workflow ended with `final_summary: null` and `v1_fault_count: 101` [T:194–195; wf_b8ba2d9a-fb2 agents a39fb478…, a9c68b66…, a431b5b3…, a68ac67a…, a3466eb2…, a8ff66cb…].

### 3.3 Phases 5–12: resume, judging, synthesis, red team, final plan, spec (2026-10-18T23:57:52Z – 2026-10-19T01:00:25Z)

**Resume and archive (23:57:52Z – 23:59:41Z)**

- **23:57:52Z** Worker restart. At **23:57:57Z** came prompt P3, "Continue".
- **23:58:20Z** The workflow was resumed from its run ID. Completed agents replayed from cache, and the six failed agents re-ran [T:250–251].
- **23:58:42Z** The five reports, three designs and the merged critique were written to `docs/` [T:263–264].
- **23:59:04Z** Commit **e7aaca2** [T:283–284].
- **23:59:09Z – 23:59:35Z** Three files that the research agents had downloaded into the repository root (two TCMalloc headers and one TCMalloc document) were moved out to the scratch directory and not committed [T:288–313].

**Judging (23:58:21Z – 00:02:55Z)** [wf_b8ba2d9a-fb2/agent-a53d5de7…, agent-a1f3f155…; T:328–332]

Each judge scored the designs 0–10 on nine criteria (throughput, memory, kernel friendliness, security, usability, correctness risk, feasibility, novelty, evidence quality; maximum 90):

| Judge | Perspective | A | B | C | Recommended base |
|---|---|---|---|---|---|
| Judge 1 | production allocator | 59 | 60 | 66 | A's data path, shipped in C's staged mode |
| Judge 2 | kernel and research | 54.5 | 60.5 | 62.5 | B |

C had the highest total under both judges, but neither judge named C as the base.

Judge 2 found two defects that all three designs shared:

- The pointer map assumed 47-bit addresses, but arm64 can return 48-bit addresses.
- `mprotect(PROT_NONE)` does not return commit charge for pages already faulted.

**Synthesis, red team, final plan (00:02:57Z – 00:53:27Z)**

| Step | Agent | Start | End | Notes |
|---|---|---|---|---|
| Synthesis (plan v2, about 110 KB) | synthesize | 00:02:57Z | 00:17:27Z | |
| Red team 1 | redteam:1 | 00:17:29Z | 00:30:45Z | 5 critical, 22 major, 8 minor |
| Red team 2 | redteam:2 | 00:17:33Z | 00:27:13Z | 4 critical, 17 major, 4 minor |
| Final revision (plan v3) | finalize | 00:30:47Z | 00:53:24Z | workflow completion notified at 00:53:27Z [T:365] |

The red team found 60 faults in total: 9 critical, 39 major and 12 minor. The critical faults included [T:329–332]:

- the free-list link encoding did not invert, so every allocation after a `free` would have returned garbage;
- a time-of-check/time-of-use race in `free()`, where page ownership was checked outside the critical section;
- a signal handler interrupting `malloc` could hand out the same block twice;
- memory-ordering assumptions that hold on x86 but not on arm64;
- a futex wait on a 1-byte field, which the kernel rejects.

`docs/SOHAMALLOC_PLAN.md` came to 24,933 words [T:374].

The author's prompts and actions in this window:

- **00:38:51Z** Prompt P4, "Update". Status reply at 00:39:25Z [T:332].
- **00:44:20Z** Prompt P5. Reply at 00:44:36Z, a four-step plan: verify the plan, build in C, test/review/fix/benchmark repeatedly, and only then the presentation [T:344].
- **00:44:39Z – 00:45:38Z** Toolchain and competitor allocators installed; mimalloc-bench cloned (§0.3) [T:345–361].

**Plan verification and commit (00:53:30Z – 00:57:00Z)**

- **00:53:30Z – 00:56:59Z** The main session read the architecture sections of the plan and checked the two critical fixes: the link encoding now inverts, and the ownership check now runs inside the critical section [T:368–386].
- **00:57:00Z** Commit **61279d9**. Its message reads: "Synthesized from 5 research reports, 3 critiques of v1 (101 faults), 3 competing designs, 2 judges and 2 red-team passes (60 faults fixed or answered in section 7.2)" [T:386].

**Implementation spec (00:57:10Z – 01:00:25Z)**

- **00:58:37Z** The main session wrote `docs/IMPL_SPEC_v0.md`, the v0.1 scope and fixed interface contract [T:396]. Its documented deviations from the plan [T:446]:
  - 4 GiB windows on 64-bit and 256 MiB on 32-bit, instead of one 64 GiB window;
  - one thread per heap, with no idle-thread reclaim, `membarrier` or rseq in v0.1;
  - signal-handler frees always take the lock-free remote path.
- **00:58:46Z** An edit fixed the hugeblock-size row: H = max(2 MiB, 32·S) [T:404].
- **00:58:48Z – 01:00:12Z** `.gitignore` added and cross toolchains installed. armhf was made the 32-bit target [T:406–419].
- **01:00:16Z** Commit **d295a53**. At **01:00:25Z** came commit **cbdd52c** [T:420–434].

### 3.4 Phases 13–14: parallel build attempts, container restart, usage limit (2026-10-19T01:02:02Z – 05:42:27Z)

- **01:02:02Z** The main session launched workflow **wf_c39d1a28-c96**, "Implement sohamalloc v0.1 in C, build tests and bench harness, integrate, port, and run adversarial review/fix rounds". Its stages were Build, Integrate, Port, Review and Fix [T:439–446].
  - The core agent ran 01:02:02Z – 01:21:58Z (17 tool calls). It wrote `include/sohamalloc.h`, `src/internal_test.h`, `src/os.h`, `src/os_posix.c` and `src/os_windows.c`, from 01:14:09Z to 01:21:54Z.
  - The test agent ran 01:02:07Z – 01:22:04Z (36 tool calls). It wrote `tests/common/*` and seven test programs, from 01:06:02Z to 01:21:18Z [wf_c39d1a28-c96].
- **01:22:37Z** **The container restarted.** The workflow was lost, and no result arrived [T:452–453].
- **01:22:47Z – 01:23:28Z** Recovery [T:462–500]:
  - The files on disk had survived: 3,994 lines of OS layer, header and tests [T:475].
  - A resume note was added to the workflow script.
  - The workflow was relaunched as **wf_c4a68997-243** at 01:23:19Z.
  - Checkpoint commit **1560b2e** at 01:23:26Z.
  - A self check-in was scheduled for 02:54Z.
- **01:28:25Z, 01:30:04Z** Before it was cut off, the relaunched test agent wrote `tests/run_tests.sh` and a partial `tests/signals.c` [wf_c4a68997-243/agent-a3b9cf08…].
- **01:30:05Z – 01:30:59Z** Every agent failed: core, tests, bench, integrate, port and four reviewers. The message was "You've hit your session limit · resets 4:50am (UTC)". The workflow returned an empty result [T:504; wf_c4a68997-243].
- **01:31:40Z, 01:51:34Z, 02:54:52Z and 04:42:10Z** Worker restarts. Each reply was the usage-limit message, and the scheduled check-in could not run [T:505–597].

### 3.5 Phases 15–17: core implementation, first debugging and performance rounds (2026-10-19T05:42:27Z – 06:32:34Z)

**Resumption and implementation (05:42:27Z – 06:01:34Z)**

- **05:42:27Z – 05:42:34Z** Prompts P6 and P6b ("Continue"), with an interruption marker and a worker restart at 05:42:29Z [T:602–624]. Uptime readings place the container boot at about 05:42Z: "06:22:05 up 39 min" [T:1119], and processes started at 05:42 [T:1209]. The repository on disk was intact [T:623].
- **05:43:09Z – 05:47:29Z** The main session read the surviving OS layer, header and test runner, and decided to reuse them [T:635–659].
- **05:50:53Z – 06:00:04Z** The **core allocator was written in the main session**, file by file [T:664–729]:

| Time | File | Contents |
|---|---|---|
| 05:50:53Z | `src/internal.h` | |
| 05:52:12Z | `src/pageheap.c` | |
| 05:55:09Z | `src/heap.c` | per-thread heaps, remote free, page detach/claim, pools, runs, solos |
| 05:59:08Z | `src/api.c` | |
| 05:59:41Z | `src/sohamalloc.c` | unity translation unit |
| 05:59:52Z | `src/sohamalloc_cxx.cpp` | |
| 06:00:04Z | `CMakeLists.txt` | |

- At 05:55:35Z the main session closed a gap it had found itself: a page could be pushed onto the pending stack twice. The fix was a per-page membership flag [T:695–703].

**First build and tests (06:00:05Z – 06:01:34Z)**

- **06:00:05Z – 06:00:24Z** The first build succeeded on the first attempt. Smoke tests passed: `ls` and python3 ran under `LD_PRELOAD` [T:731–746].
- **06:00:49Z – 06:01:25Z** The test suite was first validated against glibc [T:757–772]:
  - `api_semantics` failed under glibc itself ("buffer overflow detected", a fortify abort), so the defect was in the test.
  - All 7 runnable tests passed under sohamalloc: 10 s, against 17 s for the glibc run.
- **06:01:34Z** Commit **67a8520**, "Implement sohamalloc v0.1 core allocator in C11" (9 files, 3,279 insertions).

**Test and bench agents start (06:02:20Z)**

- **06:02:20Z** The main session launched workflow **wf_dd2453d3-1f1**, "Complete the sohamalloc test suite and build the benchmark harness". It ran two agents in disjoint directories (§3.6) [T:788].

**Debugging round 1 (06:03:22Z – 06:09:23Z)** [T:797–917]

- 06:03:22Z: A checksum stress program (SP/stress/stress.c) reported CORRUPT. It also failed under glibc and mimalloc, which pointed to the test.
  - Test bug 1: a `realloc` below 16 B left a stale size header.
  - Test bug 2: `(int)(p >> 4) % NT` could be negative. It segfaulted under jemalloc too.
  - With both fixed at 06:04:43Z, all six configurations passed: glibc 4.54 s, mimalloc 6.16 s, jemalloc 0.81 s, tcmalloc 2.97 s, sohamalloc 2.71 s, sohamalloc debug 3.04 s.
- 06:05:23Z – 06:06:41Z: Sanitizers. The clang sanitizer runtimes were missing, so gcc was used.
  - ThreadSanitizer aborted with "mmap at bad address". The fix was 256 MiB windows without address hints under TSan.
  - The stress test then passed under TSan, ASan and UBSan.
- 06:06:51Z – 06:07:11Z: The stress test passed on aarch64 and armhf (qemu) and on musl.
- 06:07:18Z – 06:09:04Z: Windows (mingw + wine) crashed in `page_pop`.
  - Root cause: `__builtin_clzl`/`ctzl` on `unsigned long` truncate 64-bit values on LLP64 Windows.
  - The fix used the `ll` variants, after which the stress test passed under wine.
- **06:09:23Z** Commit **3d2dd95**.

**Performance round 1 (06:09:32Z – 06:29:42Z)** [T:926–1234]

- 06:09:33Z – 06:10:13Z: Disassembly of the fast paths and micro-optimisations.
- 06:10:39Z: First microbenchmarks (SP/stress/micro.c) [T:957]. Times in seconds:

| Allocator | small-1t | small-4t | mixed-1t | mixed-4t | prod-cons | Peak RSS |
|---|---|---|---|---|---|---|
| glibc | 0.484 | 0.800 | 0.277 | 0.479 | 0.323 | 7,704 KB |
| jemalloc | 0.187 | 0.222 | 0.116 | 0.130 | 0.057 | 5,396 KB |
| mimalloc | 0.099 | 0.139 | 0.091 | 0.132 | 0.139 | 10,952 KB |
| tcmalloc | 0.167 | 0.201 | 0.045 | 0.103 | 0.084 | 16,748 KB |
| sohamalloc | 0.189 | 0.302 | 0.160 | 0.389 | 0.138 | 14,048 KB |

- 06:11:34Z – 06:11:45Z: A callgrind profile of the mixed workload. `free` took 24.9% of instructions and `heap_alloc` 15.6% [T:970].
- 06:13:13Z – 06:15:05Z: **Design change: owner-retained pages with remote-free notification.** Bins now keep a current page plus owner "avail" and "full" lists. A remote free to a full page clears a WATCH flag and pushes the page onto the owner's lock-free notify stack. The malloc fast path was extended to every bin class up to 128 KiB [T:979–1005; git 44e39e4 message].
- 06:15:24Z: Thread-scaling measurements. They were noisy because the bench agent was running heavy benchmarks at the same time; the main session noted a cfrac run at full CPU for about 2.5 minutes [T:1015–1026].
- 06:16:03Z – 06:21:54Z: Instruction-count reduction, measured with callgrind on SP/stress/scale.c [T:1042, T:1067, T:1093, T:1077, T:1098].

| Step | `free` (Ir) | `malloc` (Ir) | `free` per call | `malloc` per call |
|---|---|---|---|---|
| Before | 1,290,880,048 | 901,119,781 | 63 | 44 |
| After first pass (06:19:31Z) | 1,189,760,044 | 901,119,781 | 58 | 44 |
| After splitting the free word (06:21:15Z) | 1,107,840,044 | 839,679,794 | 54 | 41 |
| mimalloc, same test | 590,080,038 | 363,640,056 | | |

  Per-call counts are as stated in the transcript.

- **06:22:03Z** Commit **44e39e4**.
- 06:22:15Z – 06:22:46Z: TSan, aarch64 and armhf stress re-runs on the new lock-free protocol all passed [T:1124].
- 06:23:01Z – 06:24:29Z: **Memory-return experiment** (SP/stress/rss.c). The program allocated about 1 GiB, freed it, waited 2 s and then called trim [T:1131, T:1144]. Values are RSS in MB:

| Allocator | Peak | After free | After 2 s | After trim |
|---|---|---|---|---|
| glibc | 1017 | 1017 | 1017 | 9 |
| jemalloc | 1070 | 1070 | 1070 | 1070 |
| mimalloc | 1035 | 1035 | 1035 | 1035 |
| tcmalloc | 1047 | 628 | 628 | 628 |
| sohamalloc, before the fix | 1038 | 1038 | 1038 | 14 |
| sohamalloc, after the dirty-limit release (06:24:29Z) | 1038 | 50 | 50 | 14 |

  The dirty-limit ("excess") release is in commit e70903f, although that commit's message does not mention it (`git show e70903f -- src/pageheap.c`).

- 06:25:07Z – 06:26:28Z: The test agent had left two regression reproducers, which the main session ran and fixed (§6, D7–D8). After the fixes: 0 of 100 concurrent checks reported errors, and 26,110 handler calls returned no NULL [T:1164–1183].
- **06:26:51Z** Commit **e70903f**.
- **06:27:49Z** `docs/IMPLEMENTATION_NOTES.md` written. Commit **bd03cab** at 06:27:56Z.
- 06:28:05Z: Thread scaling on an idle machine, single small-object loop, in seconds [T:1214]:

| Allocator | 1 thread | 2 threads | 4 threads |
|---|---|---|---|
| mimalloc | 0.101 | 0.143 | 0.164 |
| sohamalloc | 0.159 | 0.218 | 0.305 |
| jemalloc | 0.086 | 0.113 | 0.190 |

- 06:29:04Z: **Ablation** of three safety features on the tiny-object loop, 3 runs each, in seconds [T:1229]:

| Variant | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| base | 0.173 | 0.162 | 0.217 |
| no busy flag | 0.140 | 0.107 | 0.154 |
| no link encoding | 0.120 | 0.128 | 0.141 |
| no bounds check | 0.163 | 0.126 | 0.109 |
| none of the three | 0.095 | 0.110 | 0.103 |
| mimalloc | 0.084 | 0.076 | 0.075 |

  Per the session summary, together these cost about 40% on this microbenchmark [T:1556].

- **06:29:42Z** Commit **c0d4822**: load owner and head_delta in parallel.

**README and checkpoints (06:30:18Z – 06:32:34Z)**

- 06:30:18Z: `README.md` written.
- 06:30:28Z – 06:31:38Z: A claim taken from the test agent was checked before publication: "glibc, jemalloc and tcmalloc deadlock when malloc is called from a signal handler". In the test, glibc and jemalloc hung and were killed by the 30 s timeout; mimalloc, tcmalloc and sohamalloc passed. The README was corrected to say exactly that [T:1253–1258].
- **06:31:47Z** Commit **56dacf2**.
- **06:32:18Z** Checkpoint commit **67652de** of the agents' work in progress. At **06:32:27Z** came commit **5ff3c75**, which ignores Python bytecode caches [T:1278–1293].
- **06:32:34Z** The main session went idle, waiting for the agents.

### 3.6 Phases 18–19: test-suite and benchmark agents (2026-10-19T06:02:21Z – 07:21:38Z)

**Bench agent** (wf_dd2453d3-1f1/agent-a51a77326c3ba65ff), 06:02:21Z – 07:02:20Z, 111 tool calls, returned a result:

| Time | Activity |
|---|---|
| 06:05Z – 06:10Z | Wrote `bench/cmake/CMakeLists.txt` and the workloads (`gen_cxx.py`, `py_alloc.py`, `sqlite_workload.sql`), tested Redis over a Unix socket, and wrote `build_bench.sh`. Built everything at 06:09:58Z |
| 06:10:26Z – 06:20:53Z | Timed each benchmark under glibc and applied scale patches |
| 06:22:28Z – 06:23:40Z | Wrote `bench/lib/runner.py`, `report.py` and `run_bench.sh`. Smoke runs at 06:24:20Z and 06:27:35Z (SP/bout) |
| 06:28:01Z – 06:34Z | 1-rep full pass, 120 runs, 362 s, at the original commit 1c9095f (= bd03cab). Kept in SP/bout1, not in `bench/out` |
| **06:35:14Z – 06:52:51Z** | **Campaign B1** (§5): 360 runs in 1,043 s, no failures |
| 06:54:27Z – 06:55:25Z | Probed the malloc-large slowdown: RSS drops at t=0, so freed large runs are released before the 1 s decay and then re-faulted |
| 06:56:12Z – 07:01Z | `--quick` validation for all 5 allocators, 298 s (SP/bquick) |
| 07:02:20Z | Agent finished. Its summary ranked sohamalloc second of five by combined score [T:1335] |

**Test agent** (wf_dd2453d3-1f1/agent-a4beed04150b641d0), 06:02:21Z – 07:21:38Z, 143 tool calls, cut off by the usage limit with no result returned:

| Time | Activity |
|---|---|
| 06:04:21Z | Fixed the fortify bug in `api_semantics` (`tu_launder`) |
| 06:04:48Z | Finished `signals.c` |
| 06:09:20Z | Wrote the signal reentrancy reproducer `tests/regress/signal_reentrant_enomem.c` |
| 06:12Z – 06:27Z | Wrote `internal_props.c`, `stats_api.c` and `memory_return.c`, `large_and_huge.c` and `cxx_test.cpp` |
| 06:15:38Z | Wrote `tests/regress/check_heap_concurrent.c` |
| 06:29Z – 06:31Z | Prefixed wrappers and the libc shim |
| 06:32Z – 06:36Z | Sanitizer runs, plus fixes for TSan incompatibilities in `thread_churn` |
| 06:38:50Z | `tests/preload_smoke.sh` |
| 06:42Z – 06:53Z | Quick suites on aarch64, armhf, musl and mingw. Test fixes for 32-bit "impossible size" constants and the musl build |
| 06:53:18Z | Wrote `tests/regress/thread_exit_detach.c` (§6, D9) |
| 06:54Z – 07:05Z | Full native matrix across all five allocators (SP/full-*.txt). sohamalloc 14/14 in 83 s [SP/full-sohamalloc.txt] |
| 06:58:14Z | `tests/README.md` |
| 07:05Z – 07:18Z | Builds with clang and static linking, full sanitizer runs, mingw full run, final cross quick runs |

- **07:21:38Z** The workflow completion notification reported the test agent as failed because of the usage limit ("resets 10:40am (UTC)"). At **07:21:39Z** the main session hit the same limit [T:1335–1337].
- **08:34:19Z** Worker restart, still limited [T:1342–1355].

### 3.7 Phases 20–23: second debugging, cross-platform and performance rounds (2026-10-19T12:42:44Z – 14:09:20Z)

**Resumption and integration (12:42:44Z – 12:48:00Z)**

- **12:42:44Z** Prompt P7, "Continue". Worker restart at 12:42:45Z [T:1359–1361].
- 12:43:03Z – 12:44:10Z: The main session read the test agent's last reproducer, `thread_exit_detach.c`, and fixed the defect (§6, D9). Afterwards, pthreads and CreateThread rounds both returned to the baseline `heaps_live` [T:1383–1394].
- **12:44:20Z** Commit **7525796**. It also checkpointed the agents' finished test files.
- 12:44:19Z – 12:45:40Z: Full native suite under sohamalloc: **14/14** in 71 s [T:1400].
- 12:45:46Z – 12:46:43Z: Debug build 14/14. Preload smoke test **9/9** with python, gcc, g++, git, perl, sort, find, tar and bash [T:1411].
- 12:46:50Z – 12:48:00Z: Prefixed tests under TSan, ASan and UBSan, **8/8 each** [T:1417].

**Cross-platform verification (12:48:05Z – 12:54:33Z)**

- 12:48:05Z – 12:50:53Z: The first cross-target run had exercised the libc allocator, not sohamalloc: it skipped sohamalloc-only tests and the mingw link failed (§6, M1) [T:1422–1448].
- 12:51:41Z: Cross variants of the library were built: aarch64, armhf, musl (C-only) and mingw. The CMake option `SOHAM_CXX` was added [T:1454].
- 12:51:54Z – 12:54:21Z: With sohamalloc loaded [T:1465]:

| Target | Result |
|---|---|
| aarch64 | 14/14 |
| armhf | 14/14 |
| musl | 13/13 (C++ test skipped) |
| mingw + wine | 8/8 (prefixed) |

- **12:54:33Z** Commit **eabd154**.

**Small-program memory and C++ operators (12:55:09Z – 13:06:36Z)**

- 12:55:09Z – 12:57:12Z: **Small-program RSS** [T:1481–1500]:
  - A trivial program's RSS was 3,836 kB under sohamalloc, against 1,444 kB for glibc and 1,936 kB for mimalloc.
  - Causes: the library depended on libstdc++ for the C++ operators, initialised hugeblock metadata eagerly, and used an 8 KiB provisioning step.
  - Fixes: the C++ operators were rewritten in C under their mangled names, the metadata initialisation was made lazy, and the provisioning step was cut to 4 KiB. RSS fell to **1,760 kB**, and the library now depends on libc only.
- 12:57:57Z – 13:05:19Z: After the C++ change, `cxx_test` aborted with "terminate called after throwing an instance of 'std::bad_alloc'" (§6, D11) [T:1511–1546].
- **13:02:05Z** The main session's context was compacted automatically [T:1553].
- 13:03:55Z: The nothrow `new` forms were changed to delegate to the C++ runtime's own nothrow `new` (found via `dlsym(RTLD_NEXT)`) when a `new_handler` is installed. The full suite then passed **14/14** [T:1593–1602]. The preload smoke test passed 9/9, and `readelf` showed `libc.so.6` as the only dependency [T:1607–1611].
- 13:06:05Z – 13:06:36Z: Fresh debug and cross builds. The mingw DLL needed `bcrypt` (§6, D12). Debug and cross-target suites were launched in the background [T:1631–1654].

**Performance round 2 (13:06:41Z – 13:47:37Z)**

- 13:06:41Z – 13:09:02Z: **malloc-large** (§6, P1) [T:1659–1728]:
  - Added a recent-peak allowance to the page heap's dirty limit, and made large runs prefer resident hugeblocks.
  - Campaign **B2** at 13:08:47Z: sohamalloc 1.62 s, 1.18× glibc's 1.37 s, with the lowest RSS (420.0 MiB).
- 13:09:59Z – 13:14:53Z: **alloc-test** (§6, P2) [T:1783–1901]. Single-thread alloc-test1 took 2,505 ms under glibc, 1,714 ms under mimalloc and 2,228 ms under sohamalloc. Callgrind showed that 28% of mallocs took the slow path. After the "unfull" threshold (cap/8), sohamalloc took 1,966 ms against mimalloc's 1,777 ms, and the slow-path share fell to 0.07% (git 273bcb9 message).
- 13:15:41Z – 13:16:43Z: Native suite 14/14 [T:1918].
- 13:16:47Z: Background cross results: debug, aarch64, armhf and mingw passed, but on musl `regress_signal_enomem` **timed out after 60 s**, a probable deadlock [T:1929].
- 13:16:52Z – 13:27:50Z: **Signal-safety deadlock investigation** (§6, D13). The hang reproduced on runs 5 and 21. The fix passed 60/60 runs. A new regression test, `tests/regress/signal_thread_attach.c`, hung in 5 of 6 runs on the old build and passed 15 of 15 on the fixed build (glibc and musl) [T:1933–2090].
- 13:27:51Z – 13:29:57Z: Native and debug suites, **15/15 each** [T:2091].
- **13:30:07Z** Commit **273bcb9** [T:2095].
- **13:30:15Z – 13:33:48Z** Campaign **B3** (§5) [T:2105].
- 13:33:57Z – 13:43:45Z: **glibc-simple regression** (§6, P3) [T:2111–2222]. sohamalloc took about 4.1 s, against about 2.1–2.6 s for an older build and 2.4–2.7 s for mimalloc:
  - System time and page faults showed no cause.
  - A JCC-erratum mitigation build only recovered 4.1 → 3.6 s.
  - Bisection pointed at the unfull threshold (2.46 s with the threshold set to 1).
  - A per-case driver could not reproduce the difference.
  - Aligning the hot entry points to 64 bytes made both variants run at 2.6–2.9 s. The cause was **code placement**, not logic.
  - Alignment variants, 3 runs each: al64 2.63/2.70/2.92 s; none 4.08/4.12/4.14 s; al32 2.18/2.25/2.29 s. Plain 64-byte alignment was kept after a cross-benchmark comparison.
- 13:43:49Z – 13:47:02Z: **larson** (§6, P4) [T:2229–2275]. Under callgrind the slow path was 2.6% of allocator work, and `free_sized` was a plain tail call. Four repetitions:

| Benchmark | mimalloc mean | sohamalloc mean |
|---|---|---|
| larson | 83.0 M ops/s | 68.6 M ops/s |
| larson-sized | 78.9 M ops/s | 63.2 M ops/s |

  An event-counting build was run (counter values recorded at T:2275). The gap remained open.

**Campaign B4 and the busy-byte change (13:47:37Z – 14:02:41Z)**

- **13:47:37Z** The full benchmark suite started in the background as campaign **B4**, finishing at 14:02:41Z [T:2280, T:2438].
- 13:48:04Z: `docs/IMPLEMENTATION_NOTES.md` updated [T:2295].
- 13:48:49Z – 13:50:47Z: The fast path was changed. `tls.busy` became two bits (BUSY_IN, BUSY_NOHEAP), so one byte test gates both fast paths. The small-size check comes first, and the malloc fast path is inlined into `operator new` [T:2299–2350].
- 13:51:03Z: The main session chose not to compile while B4 ran [T:2363].
- 13:52:39Z: README updated [T:2411].
- 13:53:03Z – 13:54:21Z: Built and tested **at nice 19 in a scratch directory** while B4 was running: quick suite 15/15, signal-attach test 3/3, preload smoke 9/9 [T:2421–2430].
- **13:54:19Z** Commit **13bcf17**.

**Post-commit verification (14:02:45Z – 14:09:20Z)**

- 14:02:45Z: B4 report read [T:2440].
- 14:03:05Z – 14:09:20Z: On 13bcf17, the native suite passed **15/15** in 61 s. Prefixed TSan passed 8/8 in 235 s, ASan 8/8 in 36 s and UBSan 8/8 in 24 s [T:2457–2459].

### 3.8 Phase 24: authorship and provenance changes (2026-10-19T14:05:20Z – 14:17:36Z)

- **14:05:20Z** Prompt P8, delivered at 14:09:20Z.
- 14:09:56Z: All 19 commits were found to carry the tool's default author identity and two tool-generated attribution trailers. Scratch-directory paths containing a tool name appeared in two research documents [T:2465–2470].
- 14:10:26Z – 14:11:47Z: Structured questions; the decisions are in §2.2.
- **14:12:46Z – 14:12:55Z** The history was rewritten with `git filter-branch` [T:2509–2510]:
  - author and committer set to Soham Basanwar;
  - attribution trailers removed;
  - three phrases that mentioned agents in commit messages replaced: "(agent checkpoint)" became "(checkpoint)", "bench-harness agent work" became "bench-harness work", and "Reproducers from the test agent:" became "Regression reproducers:";
  - three scratch-path strings in `docs/designs/B-memory-and-kernel-first.md` and `docs/research/00-v1-critique.md` replaced with neutral text. The diff against the backup reference shows "2 files changed, 3 insertions(+), 3 deletions(-)".
  - Commit dates were kept: author date equals commit date, and both match the original times.
  - The original history was kept in a local backup reference, `refs/backup/…`.
- 14:13:02Z: The branch was renamed to `sohamalloc-dev`. The push was refused by the remote's e-mail privacy restriction [T:2515–2516].
- 14:13:31Z: A second rewrite to a no-reply address was blocked by the session's permission policy until the author approved it [T:2532–2533]. The author then chose to lift the restriction [T:2548].
- **14:17:18Z** `sohamalloc-dev` was pushed. HEAD is 13bcf17 [T:2553].
- 14:17:24Z, 14:17:36Z and 14:29:44Z: Deleting the original branch on the remote failed three times ("remote end hung up", then "Everything up-to-date" with the branch still listed). The remote still reported that branch as its default HEAD branch, which is a likely reason [T:2559–2568, T:2608]. At 14:30:25Z the main session asked the author to delete it through the hosting service's web interface [T:2625]. *Status at the time of writing: the original branch still exists on the remote.*

### 3.9 Phases 25–26: cross-target re-verification, and work still running (2026-10-19T14:14:54Z – )

- **14:14:54Z** Workflow **wf_b8a82d1d-4a5** started, "Reconstruct the full research record … assess paper-worthiness". It runs the agents `novelty` and `record:timeline` (this document) [T:2538–2539].
- **14:18:25Z** Workflow **wf_cc266cad-f25** started, a four-lens adversarial review with refutation-based verification, run against HEAD 13bcf17. At 14:25Z its journal shows `review:concurrency` and `review:correctness` started [T:2573–2574].
- **14:18:35Z – 14:24:56Z** Cross-target and debug suites ran on 13bcf17 [T:2582–2583]:

| Target | Result |
|---|---|
| aarch64 | 15/15 (175 s) |
| **armhf** | **5 passed, 10 failed** |
| musl | 14 passed, 1 skipped |
| **mingw** | **4 passed, 4 failed** |
| debug | 15/15 |

  Every armhf and mingw failure was an allocation of 2 MiB or more returning NULL, for example `malloc(3145728)` and `soham_aligned_alloc(262144)` with errno 12. The cross builds also emitted `-Wsign-compare` warnings at `src/pageheap.c:394` and `:404`. See §6, D14.
- **14:25:08Z – 14:25:30Z** Diagnosis and fix: the score variables in the dirty-preferring hugeblock scan now use `int64_t` [T:2588–2599].
- **14:29:04Z** After the fix: armhf 15/15 (153 s), mingw 8/8 (43 s). At 14:29:44Z the native quick suite passed 15/15 [T:2600, T:2608].
- **14:29:41Z** Commit **ead7c98**, "Fix large-run placement on 32-bit and LLP64 targets", pushed [T:2607–2608].
- **14:30:01Z – 14:30:04Z** `docs/STATUS.md` written. Commit **26caa9e** at 14:30:02Z, pushed [T:2617–2620].
- **14:30:25Z** The main session replied to prompt P8 [T:2625]:
  - it summarised the authorship changes and the test matrix;
  - it gave a preliminary assessment of whether the work merits a paper, which is covered in other research-log files;
  - it noted that AI assistance must be disclosed to publishers.
- **Still running at the cut-off:** wf_b8a82d1d-4a5 and wf_cc266cad-f25.

---

## 4. Git commits

### 4.1 All commits, in order

These are the current hashes after the history rewrite of 14:12Z (§3.8). Times are commit times, which equal the author times.

| # | Hash | Time (UTC) | Subject | Change |
|---|---|---|---|---|
| 1 | ba06bb7 | 2026-10-18T18:00:22Z | Add malloc research notes and sohamalloc design proposal | 1 file, +510 |
| 2 | e7aaca2 | 2026-10-18T23:59:04Z | Add deep-research reports, v1 critique and three candidate designs | 9 files, +5696 |
| 3 | 61279d9 | 2026-10-19T00:57:00Z | Add final red-team-revised sohamalloc architecture and build plan (v3) | 1 file, +1811 |
| 4 | d295a53 | 2026-10-19T01:00:16Z | Add v0.1 implementation spec and gitignore | 2 files, +381 |
| 5 | cbdd52c | 2026-10-19T01:00:25Z | Spec: armhf is the 32-bit target | 1 file, +2/−2 |
| 6 | 1560b2e | 2026-10-19T01:23:26Z | WIP: partial OS layer, public header and tests (checkpoint after container restart) | 14 files, +3994 |
| 7 | 67a8520 | 2026-10-19T06:01:34Z | Implement sohamalloc v0.1 core allocator in C11 | 9 files, +3279 |
| 8 | 3d2dd95 | 2026-10-19T06:09:23Z | Fix LLP64 builtin truncation; TSan-compatible window placement | 3 files, +21/−4 |
| 9 | 44e39e4 | 2026-10-19T06:22:03Z | Owner-retained pages with remote-free notification; leaner fast paths | 5 files, +296/−140 |
| 10 | e70903f | 2026-10-19T06:26:51Z | Fix concurrent soham_check_heap false positives; handler mallocs reuse memory | 6 files, +325/−75 |
| 11 | bd03cab | 2026-10-19T06:27:56Z | Add implementation notes (as-built design, protocols, deviations) | 1 file, +200 |
| 12 | c0d4822 | 2026-10-19T06:29:42Z | free fast path: load owner and head_delta in parallel | 1 file, +7/−2 |
| 13 | 56dacf2 | 2026-10-19T06:31:47Z | Add README | 1 file, +84 |
| 14 | 67652de | 2026-10-19T06:32:18Z | WIP: test suite and benchmark harness in progress (checkpoint) | 28 files, +4119/−16 |
| 15 | 5ff3c75 | 2026-10-19T06:32:27Z | Ignore Python bytecode caches | 2 files, +2 |
| 16 | 7525796 | 2026-10-19T12:44:20Z | Detach heaps at thread exit even when emulated TLS is already gone | 18 files, +5326/−27 |
| 17 | eabd154 | 2026-10-19T12:54:33Z | CMake: SOHAM_CXX option (C-only builds, e.g. musl) | 1 file, +11/−3 |
| 18 | 273bcb9 | 2026-10-19T13:30:07Z | C++ operators in C, no libstdc++ dependency; refill and release hysteresis; busy discipline for init/attach | 9 files, +356/−155 |
| 19 | 13bcf17 | 2026-10-19T13:54:19Z | One busy byte gates both fast paths; inline new; aligned hot entries | 5 files, +93/−78 |
| 20 | ead7c98 | 2026-10-19T14:29:41Z | Fix large-run placement on 32-bit and LLP64 targets | 1 file, +7/−4 |
| 21 | 26caa9e | 2026-10-19T14:30:02Z | Add project status and resume notes | 1 file, +57 |

Source: `git log --reverse --shortstat`. Commits 20 and 21 were made after the history rewrite, so they have no "original" hash.

**Commit spacing:**

- The longest gaps between consecutive commits are 5 h 59 m (ba06bb7 at 18:00:22Z → e7aaca2 at 23:59:04Z), 4 h 38 m (1560b2e → 67a8520) and 6 h 12 m (5ff3c75 → 7525796). Each covers an interruption.
- 13 of the first 19 commits fall inside two windows, 06:01–06:32Z (9 commits) and 12:44–13:54Z (4 commits).

### 4.2 Original hashes, as they appear in the transcript and in `bench/out/run-*/env.txt`

Source: the rewrite log [T:2470, T:2510], matched by order and subject.

| Current | Original | Current | Original |
|---|---|---|---|
| ba06bb7 | 9cd1261 | 7525796 | 54676d8 |
| e7aaca2 | 8613696 | eabd154 | 734ba15 |
| 61279d9 | 00fa827 | 273bcb9 | 4661e1a |
| d295a53 | f7802eb | 13bcf17 | 6b10c57 |
| cbdd52c | 2abe530 | 56dacf2 | a3728fc |
| 1560b2e | 6f9340d | 67652de | 23aff68 |
| 67a8520 | 4e611d0 | 5ff3c75 | 4ce8508 |
| 3d2dd95 | 9c944b4 | c0d4822 | 667082a |
| 44e39e4 | 29e16ae | bd03cab | 1c9095f |
| e70903f | ae792ec | | |

---

## 5. Benchmark campaigns

### 5.1 Formal campaigns (`bench/run_bench.sh`, results in `bench/out/`)

Shared settings for every campaign, from `env.txt`:

- host: VM, Intel Xeon @ 2.80 GHz, 4 CPUs, kernel 6.18.44;
- threads N = 4, timeout 180 s;
- glibc 2.39; jemalloc, mimalloc and tcmalloc_minimal from the distribution (sha256 prefixes recorded);
- Python 3.13.16, g++ 13.3.0, sqlite3 3.45.1, Redis 7.0.15.

The runner copies each allocator library into the run directory and preloads the copy (`bench/lib/runner.py`, lines 360–378). Each campaign therefore uses one consistent library version, even if the build directory changes during the run.

**B1.** 2026-10-19T06:35:14Z – 06:52:51Z (1,043 s), run by the bench agent.

| | |
|---|---|
| Build | commit 4ce8508 (= 5ff3c75), clean tree; lib sha256 a083551eafea44c8 |
| Scope | glibc, jemalloc, mimalloc, tcmalloc, sohamalloc × 24 benchmarks × 3 reps = 360 runs |
| Failures | none; all output digests matched |
| Concurrent load | the test agent was running test suites on the same 4 CPUs (35 actions between 06:35Z and 06:53Z, including qemu cross runs at 06:42–06:53Z) |
| Score, lower is better | mimalloc 1.149, **sohamalloc 1.241**, glibc 1.289, jemalloc 1.343, tcmalloc 1.417 |
| sohamalloc geomeans | time 1.183, RSS 1.303 |
| Fastest on | barnes, cache-scratch1, cache-scratchN, rptestN, python, python-malloc, redis |
| Worst factors | malloc-large 2.88×, sh8benchN 1.71×, glibc-thread 1.54×, alloc-test1 1.40×, larsonN-sized 1.40×, xmalloc-testN 1.38× |
| Files | `bench/out/report-20261019-063514.md`, `results-20261019-063514.csv` |

**B2.** 2026-10-19T13:08:47Z (about 15 s), main session.

| | |
|---|---|
| Build | 734ba15 (= eabd154) plus uncommitted changes (C++ operators in C, lazy metadata, peak hysteresis, dirty-first runs); lib 6d624c6a34a5a2ca |
| Scope | malloc-large only; glibc, mimalloc, sohamalloc × 3 reps = 9 runs |
| Time | glibc 1.370 s, sohamalloc 1.620 s (1.18×), mimalloc 1.860 s |
| RSS | **sohamalloc lowest**, 420.0 MiB |
| Files | `report-20261019-130847.md` |

**B3.** 2026-10-19T13:30:15Z – 13:33:48Z (212 s), main session.

| | |
|---|---|
| Build | 4661e1a (= 273bcb9), clean; lib 046910db4d39f4a9 |
| Scope | jemalloc, mimalloc, sohamalloc × 11 multi-threaded and fast-path benchmarks × 3 reps = 99 runs |
| Score | mimalloc 1.106, **sohamalloc 1.130**, jemalloc 1.396 |
| Notes | glibc-simple 1.65× (the code-placement effect in §6, P3). Factors are relative to the best of these three allocators only, so they are not comparable with B1 or B4 |
| Files | `report-20261019-133015.md` |

**B4.** 2026-10-19T13:47:39Z – 14:02:41Z (903 s), main session, background.

| | |
|---|---|
| Build | 4661e1a (= 273bcb9) plus uncommitted 64-byte alignment of the hot entry points; lib 590743fec1ef73e0. **Not identical to any commit**: 13bcf17 adds the busy-byte change and inlined `new` on top. No formal campaign has been run on 13bcf17 yet |
| Scope | 5 allocators × 24 benchmarks × 3 reps = 360 runs |
| Failures | none |
| Concurrent load | a nice-19 build and test run, 13:53:03Z – 13:54:21Z |
| Score | mimalloc 1.142, **sohamalloc 1.142** (equal to 3 decimals), glibc 1.276, jemalloc 1.344, tcmalloc 1.451 |
| sohamalloc geomeans | time 1.124, RSS 1.162. sohamalloc was never the fattest on any benchmark |
| Fastest on | cfrac, cache-scratch1, rptestN, python-malloc, gxx |
| Slowest on | barnes, cache-thrashN, python |
| Files | `report-20261019-134738.md` (also `report.md`) |

**B1 → B4 changes for sohamalloc** (factor against the best allocator):

| Benchmark | B1 | B4 |
|---|---|---|
| malloc-large | 2.88 | 1.38 |
| sh8benchN | 1.71 | 1.07 |
| glibc-thread | 1.54 | 1.05 |
| alloc-test1 | 1.40 | 1.21 |
| larsonN | 1.24 | 1.07 |
| larsonN-sized | 1.40 | 1.20 |
| xmalloc-testN | 1.38 | 1.23 |
| barnes | 1.00 | 1.20 (worse) |
| cache-thrashN | 1.10 | 1.31 (worse) |
| cache-scratchN | 1.00 | 1.32 (worse) |
| glibc-simple | 1.17 | 1.25 (worse) |

**Caveat for cross-campaign comparison.** Absolute times moved for every allocator between B1 and B4. For example, cfrac under glibc took 2.560 s in B1 and 1.690 s in B4, and espresso under glibc 7.310 s and 6.710 s. The machine conditions therefore differed, with concurrent agent load during B1. The run-to-run "spread" column reaches 171% (B1, cache-scratchN) and 117% (B4, cache-scratchN). Normalised factors are less affected but not immune.

### 5.2 Other benchmark runs (outside `bench/out`)

| Time | Run | Location |
|---|---|---|
| 06:24:20Z, 06:27:35Z | Harness smoke runs | SP/bout |
| 06:28:01Z – 06:34Z | 1-rep full pass, 120 runs, 362 s, at 1c9095f | SP/bout1 |
| 06:56:12Z – 07:01Z | `--quick` pass, 1 rep, 298 s, at 4ce8508 | SP/bquick |

### 5.3 Ad-hoc measurements (main session)

| Time | Measurement | Source |
|---|---|---|
| 06:03–06:05Z | Concurrency stress across 6 allocator builds | T:833 |
| 06:10:39Z, 06:15:05Z, 06:24:39Z | Microbenchmarks (micro.c) | T:957, 1005, 1154 |
| 06:15:24Z, 06:28:05Z, 06:28:41Z | Thread scaling (scale.c) | T:1015, 1214, 1219 |
| 06:11Z, 06:16–06:21Z | Callgrind instruction counts | SP/stress/cg2.out, cgs.out.* |
| 06:23–06:24Z | Memory return after freeing 1 GiB (rss.c) | T:1131, 1144 |
| 06:29:04Z | Ablation (SP/exp/lib_{base,nobusy,noenc,nocheck,none}.so) | T:1229 |
| 06:30–06:31Z | Signal-handler malloc across allocators | T:1254 |
| 12:55Z | Small-program RSS (tiny*.c) | T:1481–1500 |
| 13:09–13:14Z | alloc-test timing and callgrind | SP/cg.mi.32265, cg.soham.10644 |
| 13:34–13:43Z | glibc-simple: callgrind, page faults, JCC build, bisection, alignment variants | SP/cg.gs.30569, SP/bjcc, SP/v-* |
| 13:44–13:47Z | larson callgrind, 4-rep runs, event counters | SP/cg.la.32386; T:2249, T:2275 |

---

## 6. Significant defects: discovery and fix

### 6.1 Design-stage defects (found by review before any code existed)

| # | Found | By | Defect | Resolution |
|---|---|---|---|---|
| R1 | 18:59–19:59Z, 2026-10-18 | 3 critic agents | 101 faults in v1 (12 critical, 62 major, 27 minor), including claimed novelty that had prior art (mm_cid indexing, lifetime prediction), wrong size-class waste figures, and no design for the pointer-to-metadata lookup | Addressed in plan v3 §2.1 (`docs/SOHAMALLOC_PLAN.md`) |
| R2 | 00:00–00:03Z | Judge 2 | Pointer map assumed 47-bit addresses (arm64 returns 48-bit); `mprotect(PROT_NONE)` does not uncommit faulted pages | Plan v3 |
| R3 | 00:17–00:31Z | 2 red-team agents | 60 faults (9 critical), including non-inverting link encoding, TOCTOU in `free()`, double hand-out under signals, x86-only memory ordering, and a futex on a 1-byte field | Plan v3 §7.2. The encoding and TOCTOU fixes were checked by the main session at 00:53–00:57Z [T:385] |

### 6.2 Implementation defects (found by testing, review or measurement)

Times are 2026-10-19.

| # | Found | How found | Defect | Fix | Commit | Verification |
|---|---|---|---|---|---|---|
| T1 | 06:00:49Z | Suite validated against glibc first | Test bug: `api_semantics` triggered a glibc fortify abort | Test agent, 06:04:21Z (`tu_launder`) | 67652de | Passes on all allocators |
| T2 | 06:03:22Z | Stress test cross-checked on glibc and mimalloc | Test bug: stale size header after `realloc` below 16 B | 06:03:47Z | (scratch test) | 6/6 configurations pass, 06:05:03Z |
| T3 | 06:04:09Z | Same, plus gdb | Test bug: negative modulo index | 06:04:39Z | (scratch test) | Same |
| D1 | 06:05:39Z | TSan run | TSan "mmap at bad address": window placement collides with the TSan shadow | 256 MiB windows, no address hint under TSan | 3d2dd95 | TSan stress passes, 06:06:41Z |
| D2 | 06:07:45Z | Windows (wine) stress | Crash in `page_pop`: `__builtin_clzl`/`ctzl` truncate on LLP64, giving wrong size classes and window shift | `ll` builtins | 3d2dd95 | Stress passes under wine, 06:09:04Z |
| D3 | 05:55:28Z | Self-review while writing | A page could be pushed twice onto the pending stack | Per-page membership flag | 67a8520 | — |
| P0 | 06:10–06:15Z | Microbenchmarks, callgrind | High fast-path cost and slow small-object paths | Owner-retained pages + notify protocol; fast path for all classes ≤ 128 KiB; instruction trimming | 44e39e4, c0d4822 | TSan, arm64 and armhf stress pass, 06:22:46Z |
| D4 | 06:23:17Z | Memory-return experiment | Freed memory not returned without an explicit trim (1038 MB resident after free) | Dirty-limit (excess) release in the page heap | e70903f | 50 MB after free |
| D7 | 06:15Z (test agent) | `regress/check_heap_concurrent.c` | `soham_check_heap()` false errors under concurrency, plus a data race on record fields | Validate only the caller's own pages; snapshots annotated as racy | e70903f | 0 of 100 checks with errors |
| D8 | 06:09Z (test agent) | `regress/signal_reentrant_enomem.c` | Signal-handler mallocs exhausted the no-free static arena | Handler uses the shared heap via trylock ("trymode"), falling back to the arena | e70903f | 26,110 handler calls, 0 NULL |
| C1 | 06:30:28Z | Main session re-measured a test-agent claim | README claim overstated: only glibc and jemalloc hung, not tcmalloc | README corrected | 56dacf2 | T:1258 |
| D9 | 06:53Z (test agent); fixed 12:43Z | `regress/thread_exit_detach.c` under wine | On mingw, emutls is destroyed before the FLS callback, so exited pthreads' heaps leaked (`heaps_live` 1 → 25) | `thread_exit_cb` trusts its heap argument and `live` flag | 7525796 | pthreads and CreateThread back to baseline |
| M1 | 12:48–12:51Z | Inspection of cross results | Methodology error: cross-target runs had tested the libc allocator, not sohamalloc | Built per-target sohamalloc libraries and passed `--lib` / `--prefixed-lib`; added `SOHAM_CXX` option | eabd154 | aarch64 14/14, armhf 14/14, musl 13/13, mingw 8/8 |
| D10 | 12:55:10Z | RSS of a trivial program | Baseline RSS 3,836 kB (libstdc++ dependency, eager metadata, 8 KiB provisioning) | C++ operators in C, lazy metadata, 4 KiB step | 273bcb9 | 1,760 kB |
| D11 | 12:59:28Z | `cxx_test` | A throwing `new_handler` in nothrow `new` caused `std::terminate` (C frames cannot catch) | nothrow forms delegate to the C++ runtime's nothrow `new` via `RTLD_NEXT` when a handler is installed | 273bcb9 | 14/14, 13:05:19Z |
| D12 | 13:06:18Z | Fresh mingw build | Windows DLL link failed: missing `bcrypt` | Link bcrypt on WIN32 | 273bcb9 | DLL built, 13:06:31Z |
| P1 | B1 (06:52Z); fixed 13:08Z | Benchmark; bench-agent probe | malloc-large 2.88×: large runs released and re-faulted (excess-release thrash) | Recent-peak swing in the dirty limit; dirty-preferring hugeblock runs | 273bcb9 | B2 1.18× glibc, lowest RSS; B4 1.38× of tcmalloc |
| P2 | 13:12Z | callgrind call counts | alloc-test: 28% of mallocs on the slow path (full pages returned at their first free) | "unfull" threshold cap/8 for local and remote frees | 273bcb9 | 0.07% slow path; 2,228 → 1,966 ms |
| D13 | 13:16:47Z | musl cross suite timeout | **Signal-safety deadlock**: init/attach, thread exit, stats and `check_heap` took allocator locks without `busy` set, so a handler could wait on a lock held by its own thread | `busy_enter`/`busy_leave` discipline on those paths; new test `signal_thread_attach.c` | 273bcb9 | 60/60 musl runs; new test 5/6 hangs before, 15/15 passes after |
| P3 | 13:33Z | B3 and timing | glibc-simple 4.1 s against 2.1–2.6 s: code placement (linker position of `malloc`/`free`) | 64-byte aligned hot entry points | 13bcf17 | 2.6–2.9 s |
| D14 | 14:24:56Z | Cross-target suites on 13bcf17 (armhf 10 fails, mingw 4 fails), plus `-Wsign-compare` warnings | **Regression introduced in 273bcb9** by the dirty-preferring hugeblock scan. With a 32-bit `long` (ILP32 armhf, LLP64 Windows), `(long)nh * 32u` is unsigned, so the initial best score of −1 compared as the largest value and the scan never ran: every allocation ≥ 2 MiB failed. x86-64 and aarch64 Linux were unaffected. It went undetected from 13:30:07Z to 14:24:56Z because the cross runs before 273bcb9 (13:06–13:16Z) predated the change | `int64_t` scores | ead7c98 | armhf 15/15, mingw 8/8, native quick 15/15 |
| P4 | 13:44–13:47Z | larson repeated runs | larson about 1.2× slower than mimalloc (83.0 M vs 68.6 M ops/s) | **Open** | — | — |
| O1 | 12:45:46Z | signals test note | 101 handler allocations returned NULL with ENOMEM (within the test's threshold) | **Open** (noted as "could improve" [T:1556]) | — | — |

---

## 7. Interruptions, restarts and idle periods

| # | From | To | Duration | Cause | Effect |
|---|---|---|---|---|---|
| I1 | 2026-10-18T20:21:28Z | 2026-10-18T23:57:57Z | 3 h 36 m 29 s | Usage limit ("resets 11:50pm (UTC)"). Worker restarts at 20:30:09Z, 20:44:38Z and 23:57:52Z | 6 downstream planning agents failed; resumed from cache at 23:58:20Z |
| — | 2026-10-19T01:22:37Z | 01:22:47Z | — | **Container restart** | Build workflow wf_c39d1a28-c96 lost after 20 m 35 s; files on disk survived |
| I2 | 2026-10-19T01:30:59Z | 05:42:27Z | 4 h 11 m 28 s | Usage limit ("resets 4:50am (UTC)"). Worker restarts at 01:31:40Z, 01:51:34Z, 02:54:52Z, 04:42:10Z and 05:42:29Z; scheduled check-in delivered but not actionable | All 9 agents of wf_c4a68997-243 failed. Container rebooted at about 05:42Z (uptime evidence, T:1022, T:1119) |
| I3 | 2026-10-19T07:21:38Z | 12:42:44Z | 5 h 21 m 06 s | Usage limit ("resets 10:40am (UTC)"), then no activity until the author's next prompt. Worker restart at 08:34:19Z | Test agent terminated without a result; its files on disk were kept and committed at 12:44:20Z |
| — | 2026-10-19T13:02:05Z | — | — | Automatic context compaction of the main session | The earlier conversation was summarised [T:1556] |
| — | 2026-10-18T18:00:34Z / 2026-10-19T00:39:25Z | 18:05:52Z / 00:44:20Z | 5 m 18 s / 4 m 55 s | Waiting for the author's next prompt | — |

---

## 8. Agent roster (all workflows)

Role, start, end and tool calls come from each agent transcript's first and last event timestamps and its count of `tool_use` blocks.

| Workflow | Agent role | Start | End | Tool calls | Outcome |
|---|---|---|---|---|---|
| wf_b8ba2d9a-fb2 (research/plan) | research:internals | 10-18T18:08:03Z | 18:41:57Z | 177 | result |
| | research:kernel | 18:08:06Z | 18:35:12Z | 137 | result |
| | research:academic | 18:35:15Z | 18:55:47Z | 122 | result |
| | research:security | 18:41:59Z | 18:58:58Z | 94 | result |
| | research:production | 18:55:49Z | 19:24:41Z | 172 | result |
| | critic:facts | 18:59:00Z | 19:29:10Z | 143 | result |
| | critic:design | 19:24:43Z | 19:59:48Z | 109 | result |
| | critic:gaps | 19:29:13Z | 19:52:43Z | 97 | result |
| | design:perf (A) | 19:59:51Z | 20:12:43Z | 2 | result |
| | design:memory (B) | 19:59:56Z | 20:10:01Z | 2 | result |
| | design:robust (C) | 20:10:03Z | 20:21:26Z | 0 | result |
| | judge:1, judge:2, synthesize, redteam:1, redteam:2, finalize (first attempt) | 20:21:28Z | 20:21:30Z | 0 | failed (usage limit) |
| | judge:1 | 23:58:21Z | 10-19T00:00:35Z | 1 | result |
| | judge:2 | 23:58:26Z | 00:02:55Z | 5 | result |
| | synthesize | 00:02:57Z | 00:17:27Z | 1 | result |
| | redteam:1 | 00:17:29Z | 00:30:45Z | 6 | result |
| | redteam:2 | 00:17:33Z | 00:27:13Z | 18 | result |
| | finalize | 00:30:47Z | 00:53:24Z | 4 | result |
| wf_c39d1a28-c96 (build, attempt 1) | core | 10-19T01:02:02Z | 01:21:58Z | 17 | lost (container restart) |
| | tests | 01:02:07Z | 01:22:04Z | 36 | lost (container restart) |
| wf_c4a68997-243 (build, attempt 2) | core | 01:23:19Z | 01:30:58Z | 12 | failed (usage limit) |
| | tests | 01:23:22Z | 01:30:05Z | 17 | failed (usage limit) |
| | bench, integrate, port, review ×4 | 01:30:05Z | 01:30:59Z | 0 | failed (usage limit) |
| wf_dd2453d3-1f1 (tests + bench) | tests | 06:02:21Z | 07:21:38Z | 143 | cut off (usage limit) |
| | bench | 06:02:21Z | 07:02:20Z | 111 | result |
| wf_b8a82d1d-4a5 (research record) | novelty, record:timeline, … | 14:14:55Z | running | — | running |
| wf_cc266cad-f25 (adversarial review) | review:concurrency, review:correctness, … | 14:18:25Z | running | — | running |

Workflow scripts are in `<session transcript dir>/workflows/scripts/`:

- `sohamalloc-research-critique-plan-…js`
- `sohamalloc-build-v0-…js`
- `sohamalloc-tests-bench-…js`
- `sohamalloc-research-record-…js`
- `sohamalloc-adversarial-review-…js`

---

## 9. Artifacts produced

### 9.1 Version-controlled files

Line counts are at HEAD 13bcf17. "Produced by" is taken from the transcripts. All producers are AI-assisted tooling under the author's direction (§0.2). "Main" means the main session; "Agent:" names a workflow agent.

| File | Created in commit | Commit time | Lines | Produced by |
|---|---|---|---|---|
| `docs/RESEARCH_AND_DESIGN.md` | ba06bb7 | 2026-10-18T18:00:22Z | 510 | Main (17:59:39Z) |
| `docs/research/00-v1-critique.md` | e7aaca2 | 2026-10-18T23:59:04Z | 1325 | Main, merging the 3 critic agents' outputs |
| `docs/research/01-allocator-internals.md` | e7aaca2 | 2026-10-18T23:59:04Z | 218 | Agent: research:internals |
| `docs/research/02-linux-kernel-interfaces.md` | e7aaca2 | 2026-10-18T23:59:04Z | 517 | Agent: research:kernel |
| `docs/research/03-academic-research.md` | e7aaca2 | 2026-10-18T23:59:04Z | 376 | Agent: research:academic |
| `docs/research/04-security-and-hardening.md` | e7aaca2 | 2026-10-18T23:59:04Z | 421 | Agent: research:security |
| `docs/research/05-production-lessons-and-evaluation.md` | e7aaca2 | 2026-10-18T23:59:04Z | 431 | Agent: research:production |
| `docs/designs/A-performance-first.md` | e7aaca2 | 2026-10-18T23:59:04Z | 826 | Agent: design:perf |
| `docs/designs/B-memory-and-kernel-first.md` | e7aaca2 | 2026-10-18T23:59:04Z | 809 | Agent: design:memory |
| `docs/designs/C-robustness-security-usability-first.md` | e7aaca2 | 2026-10-18T23:59:04Z | 773 | Agent: design:robust |
| `docs/SOHAMALLOC_PLAN.md` | 61279d9 | 2026-10-19T00:57:00Z | 1811 | Agent: finalize (plan v3) |
| `docs/IMPL_SPEC_v0.md` | d295a53 | 2026-10-19T01:00:16Z | 376 | Main |
| `.gitignore` | d295a53 | 2026-10-19T01:00:16Z | 7 | Main |
| `include/sohamalloc.h` | 1560b2e | 2026-10-19T01:23:26Z | 59 | Agent: core (attempt 1) |
| `src/internal_test.h` | 1560b2e | 2026-10-19T01:23:26Z | 83 | Agent: core (attempt 1) |
| `src/os.h` | 1560b2e | 2026-10-19T01:23:26Z | 127 | Agent: core (attempt 1) |
| `src/os_posix.c` | 1560b2e | 2026-10-19T01:23:26Z | 497 | Agent: core (attempt 1) |
| `src/os_windows.c` | 1560b2e | 2026-10-19T01:23:26Z | 254 | Agent: core (attempt 1) |
| `tests/common/soham_api.h`, `tests/common/test_util.h` | 1560b2e | 2026-10-19T01:23:26Z | 64, 709 | Agent: tests (attempt 1) |
| `tests/api_semantics.c`, `sizes_sweep.c`, `random_ops.c`, `threads_local.c`, `producer_consumer.c`, `thread_churn.c`, `fork_test.c` | 1560b2e | 2026-10-19T01:23:26Z | 672, 165, 251, 163, 316, 404, 281 | Agent: tests (attempt 1), later edited by the test agent |
| `tests/run_tests.sh` | 67a8520 | 2026-10-19T06:01:34Z | 375 | Agent: tests (attempt 2, 01:28Z), extended later |
| `tests/signals.c` | 67a8520 | 2026-10-19T06:01:34Z | 383 | Agent: tests (attempt 2, partial), completed by the test agent at 06:04Z |
| `CMakeLists.txt` | 67a8520 | 2026-10-19T06:01:34Z | 77 | Main |
| `src/internal.h` | 67a8520 | 2026-10-19T06:01:34Z | 409 | Main |
| `src/pageheap.c` | 67a8520 | 2026-10-19T06:01:34Z | 566 | Main |
| `src/heap.c` | 67a8520 | 2026-10-19T06:01:34Z | 770 | Main |
| `src/api.c` | 67a8520 | 2026-10-19T06:01:34Z | 1134 | Main |
| `src/sohamalloc.c` | 67a8520 | 2026-10-19T06:01:34Z | 30 | Main |
| `src/sohamalloc_cxx.cpp` | 67a8520 | 2026-10-19T06:01:34Z | (deleted in 273bcb9) | Main |
| `tests/regress/check_heap_concurrent.c` | e70903f | 2026-10-19T06:26:51Z | 67 | Agent: tests |
| `tests/regress/signal_reentrant_enomem.c` | e70903f | 2026-10-19T06:26:51Z | 113 | Agent: tests |
| `docs/IMPLEMENTATION_NOTES.md` | bd03cab | 2026-10-19T06:27:56Z | 203 | Main |
| `README.md` | 56dacf2 | 2026-10-19T06:31:47Z | 85 | Main |
| `bench/README.md` | 67652de | 2026-10-19T06:32:18Z | 163 | Agent: bench |
| `bench/build_bench.sh` | 67652de | 2026-10-19T06:32:18Z | 194 | Agent: bench |
| `bench/run_bench.sh` | 67652de | 2026-10-19T06:32:18Z | 23 | Agent: bench |
| `bench/cmake/CMakeLists.txt` | 67652de | 2026-10-19T06:32:18Z | 103 | Agent: bench |
| `bench/lib/runner.py`, `bench/lib/report.py` | 67652de | 2026-10-19T06:32:18Z | 493, 229 | Agent: bench |
| `bench/workloads/gen_cxx.py`, `py_alloc.py`, `sqlite_workload.sql` | 67652de | 2026-10-19T06:32:18Z | 139, 119, 25 | Agent: bench |
| `bench/lib/__pycache__/report.cpython-313.pyc` | 67652de | 2026-10-19T06:32:18Z | (deleted in 5ff3c75) | accidental |
| `tests/cxx_test.cpp`, `internal_props.c`, `large_and_huge.c`, `memory_return.c`, `stats_api.c` | 67652de | 2026-10-19T06:32:18Z | 556, 286, 419, 295, 488 | Agent: tests |
| `tests/prefixed/api_semantics.c`, `libc_shim.c`, `internal_props.c`, `producer_consumer.c`, `random_ops.c`, `stats_api.c`, `thread_churn.c` | 67652de | 2026-10-19T06:32:18Z | 271, 193, 10 each for the other five | Agent: tests |
| `tests/README.md` | 7525796 | 2026-10-19T12:44:20Z | 133 | Agent: tests |
| `tests/preload_smoke.sh` | 7525796 | 2026-10-19T12:44:20Z | 207 | Agent: tests |
| `tests/data/bigger.c`, `gen_bigger.py`, `hello.c`, `templates.cpp`, `workload.py` | 7525796 | 2026-10-19T12:44:20Z | 4534, 52, 14, 107, 74 | Agent: tests |
| `tests/regress/thread_exit_detach.c` | 7525796 | 2026-10-19T12:44:20Z | 98 | Agent: tests |
| `tests/regress/signal_thread_attach.c` | 273bcb9 | 2026-10-19T13:30:07Z | 89 | Main |
| `docs/STATUS.md` | 26caa9e | 2026-10-19T14:30:02Z | 57 | Main |
| `docs/research-log/01-timeline.md` (this file) | not committed | written about 2026-10-19T14:35Z | — | Agent: record:timeline |

**Sizes at HEAD** (counted at 13bcf17; ead7c98 adds 3 net lines to `src/pageheap.c`):

- `src/` + `include/`: 3,929 lines.
- Tests (C/C++ sources, regressions, prefixed wrappers, common headers and shell scripts): 6,915 lines.
- Benchmark harness: 1,325 lines.

Source: `wc -l`.

### 9.2 Non-versioned artifacts kept for the record

- **Benchmark data:** `bench/out/` holds four campaigns. Each has a CSV, a report and a `run-*/` directory with `env.txt`, library snapshots and logs. `bench/out/report.md` is a copy of the B4 report.
- **Scratch directory SP:**
  - ad-hoc programs: `stress/` (`stress.c`, `micro.c`, `scale.c`, `rss.c`, `mixed1.c`, `tiny*.c`, `ba.cpp`, cross binaries);
  - ablation builds: `exp/`;
  - callgrind profiles: `cg.*`, `stress/cg*.out`;
  - extra benchmark passes: `bout/`, `bout1/`, `bquick/`;
  - test-matrix outputs: `full-*.txt`, `quick-*.txt`;
  - benchmark logs: `full.log` (B1), `fullbench.log` (B4);
  - variant builds: `bjcc/`, `bnew/`, `v-*`;
  - downloaded reference material: `dl/`, `dl2/`, `papers/`, `pdfs/`, `tcmalloc-ref/`, `mimalloc-bench/`.
- **Transcripts:** the main session transcript, 7 workflow journals with their agent transcripts, and 5 workflow scripts (§0.1, §8). Large tool outputs are in `…/tool-results/`.
- **Original git history:** the local backup reference `refs/backup/…` created at 14:12:46Z [T:2509].

---

## 10. Uncertainties and limitations of this record

1. **Phase boundaries are inferred** from event timestamps. Where phases ran in parallel, as with the test and bench agents next to the main session, their durations overlap and cannot be added up.
2. **"Active" time is a wall-clock window, not effort.** For example, the main session was idle from 06:32:34Z to 07:21:38Z while the agents worked.
3. **The original transcript hashes differ from today's hashes** because of the 14:12Z history rewrite. §4.2 gives the mapping. The rewrite also changed three strings in two documents, so the tree hashes of e7aaca2 and later commits differ from the originals.
4. **The container reboot at about 05:42Z is inferred**, from uptime and process start times. The transcript records only a worker restart at that time, not an explicit container-restart notice.
5. **Benchmark comparability is limited.** B1 ran beside a busy test agent. B4 overlapped briefly with a nice-19 build. B4's library matches no commit exactly. Spreads are high (§5.1). The formal benchmarks were not run with CPU isolation or pinning.
6. **Some per-call instruction counts and percentages are quoted from the session's own statements**, without independent recomputation here: malloc 41 and free 54 instructions; the 28% → 0.07% slow-path share; "about 40%" for the ablation. The callgrind files they come from are in SP.
7. **The final states of wf_b8a82d1d-4a5 and wf_cc266cad-f25 are unknown** at the time of writing.
8. **Line counts in §9.1 were taken at 13bcf17.** Of the files listed, only `src/pageheap.c` changed afterwards (ead7c98, +7/−4).
9. **The fixed build has not been benchmarked.** D14 affected only 32-bit and LLP64 targets, so the x86-64 benchmark campaigns B1–B4 are not affected by it. No formal benchmark campaign has been run on 13bcf17, ead7c98 or 26caa9e.
