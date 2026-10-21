I'll start by reading the implementation notes and the code.

---

Let me set up a repro for a suspected handler-context blocking path (bin_trim_avail reachable in trymode). First, a quick check of the call graph for blocking locks reachable from trymode.

---

The repro stalls every time. Let me confirm the deadlock's stack with gdb.

---

C reused only the cur page's 540 blocks; the four full pages stayed stranded. Let me confirm the cause by applying the proposed fix to a scratch copy of the source.

---

The fix restores reuse (5175 of 6000), confirming the cause. Next, a TSan pass over a producer/consumer and thread-churn stress on the prefixed library.

---

As suspected, the crash is my test's bug (`pthread_kill` on a zero handle), not the allocator. Fixing the thread start order and rerunning.

---

Ordinary churn reproduces the deadlock too (3/3 stalls within about 530 handler calls; the fixed build passes). Let me collect exact line numbers for the report.

---

HEAD moved to c9090cf and an untracked `tools/sigstorm/` appeared; those come from other concurrent jobs, not from me. Let me check whether `src/` changed, since my line numbers refer to ee1f70d.