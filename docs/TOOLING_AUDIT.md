# sohamalloc: Claude Code setup audit (final report)

Date: 2026-10-22. Scope: the Claude Code setup used for /home/sohambasanwar/sohamalloc (Opus 5.5 at effort "max", Ultracode workflows, Remote Control at startup, repeated usage-limit stops). Inputs: six tool evaluations, six fact-checks and one configuration audit, kept in `docs/research-log/data/tooling-audit-2026-10-22/results.json`. This report applies every fact-check correction. Where a checker found a claim unsupported, the claim is dropped or marked *unverified*. Where a checker disagreed with a verdict, the decision and the reason are given.

Read-only re-checks made for this report on 2026-10-22: the settings and permissions pages on code.claude.com (fetched); `claude plugin --help`, `claude plugin uninstall|disable|details --help` and `claude mcp remove --help`; `claude plugin details <name>` for each plugin, which prints a component inventory and a *projected* token cost; and listings of `~/.headroom`, `~/.claude-mem`, `~/.claude/plugins/data/` and `bench/out/`. Nothing was installed, configured, enabled, disabled, moved or deleted.

The separate check of the official documentation did not finish. That check covered Opus max, Ultracode subagents, Remote Control and usage accounting. Statements about those features rest on doc quotes that the tool checkers fetched themselves, and they are cited as such. Everything else is listed in section 7.

**Provenance.** Written by the synthesis agent of workflow runs `wf_5c927039-e0b` (research and fact-checks; the documentation check was stopped after running past its time limit) and `wf_b255feb5-c5e` (synthesis). Workflow subagents may not write report files, so the main session saved the text from the workflow journal, removing only a harness note on the first line, and added the dated corrections marked "Main-session check".

---

## 1. Summary

### Verdict per tool

| Tool | Verdict | One-line reason |
|---|---|---|
| Headroom (headroom-ai 0.41.0, user MCP server) | **Remove** | In Claude Code's MCP mode it costs tokens rather than saving them, its telemetry beacon is on by default, and it writes verbatim copies of whatever it compresses to `~/.headroom/ccr_store.db`. |
| Headroom (synced skill plugin 1.3.2, Petrovici) | **Keep**, with a digest rule | A local, read-only outline script. Small cost, useful on large logs and CSV files. |
| Claude Code Setup (claude-code-setup 1.0.0) | **Disable** (the evaluation said keep-with-changes) | The proposed change cannot work, because `skillOverrides` does not apply to plugin skills. That leaves a standing cost in every session and subagent and no ongoing benefit. |
| Claude-Mem stack (claude-mem 15.0.1, claude-mem-cowork 0.1.4, claude-memory 1.0.0) | **Remove all three** | Broken today, but it already sends telemetry. Once working, it would capture all tool I/O in plaintext and run an observer turn on every tool call, on the same plan whose limits keep running out. It duplicates auto-memory. |
| Task Observer (personal skill rebelytics v3.7.0; synced plugin 1.3.2) | **Keep (A) as manual-only**; (B) stays disabled | No network code and nothing written to the repo. But its "invoke before the first tool call" trigger and its log-and-defer rule conflict with Ultracode cost and with auto-memory. |
| OmniRoute 3.8.52 (installed, dormant) | **Remove** (uninstall) | Breaks Remote Control. Using it with a Claude subscription breaches Anthropic's terms. It logs request bodies for 90 days by default and forwards them to upstreams outside the trust boundary. It has no use case here. |
| FreeLLMAPI (not installed) | **Do not install** | It replaces Opus with free third-party models, some of which log or train on inputs. It breaks Remote Control and would expose the evidence. |

### The three most important changes

1. **Remove the claude-mem stack before the next Claude Code restart.** Disable claude-mem-cowork together with claude-mem so that cowork never becomes the active capture path, then uninstall all three. Do not run `npx claude-mem install`. The reason for the timing: at the next startup, claude-mem's SessionStart hook `ensure-launcher.cjs` tries to compile a binary into `~/.local/bin` and add a PATH line to `~/.bashrc` (`~/.claude/plugins/cache/thedotmack/claude-mem/15.0.1/scripts/ensure-launcher.cjs`). *Main-session check (2026-10-22): `~/.local/bin` is already on PATH, so the rc-file edit returns early ("already-on-path" in that script); without Bun the compile fails. The timing matters less than stated, but the removal stands.* The three plugins account for 17 of the 18 hook handlers and about 3,000 always-on tokens per session and per subagent (section 4).
2. **Back up the evidence that exists only on this disk.** That means the git-excluded `docs/research-log/data/review3/repros/`, `review3/review3-results.json` and `review4/`, the git-ignored `bench/out/` (which holds the frozen fd735dd sigstorm library the rerun needs), and the workflow resume scripts under `~/.claude/projects/-home-sohambasanwar-sohamalloc/…/workflows/scripts/`. Put the backup in a private, encrypted location, record sha256 sums, and locate the raw data for the B1–B4 cloud-VM campaigns, which is not on this machine.
3. **Replace prose with mechanical guards.** Auto mode is on with no allow, ask or deny rules, Remote Control is on at startup, and many upload-capable connectors are in reach. Add deny rules for upload tools, or set `disableClaudeAiConnectors: true` for this project. Keep `ANTHROPIC_BASE_URL` and `ANTHROPIC_AUTH_TOKEN` unset (no gateways or proxies). Add a local `commit-msg` hook that enforces the no-trailer rule (sections 4 and 6).

---

## 2. Comparison matrix

Token figures marked "projected" come from `claude plugin details` (run 2026-10-22). Claude Code labels them estimates. Other figures are character-count estimates unless marked "measured".

| | Net token effect for this user | Research reliability | Security risk | Overlap | Opus max | Ultracode | Remote Control | Verdict |
|---|---|---|---|---|---|---|---|---|
| **Headroom (headroom-ai MCP)** | **Negative.** headroom_compress takes only a `content` string, so the model must re-send content already in context as output tokens. The compressed copy is then added on top and nothing is removed. Measured offline on this install: 0% on text, CSV and logs (no-op without the ML extras); about 59% (o200k counter) to 68% (character estimator) on repeated JSON arrays, lossless in the tests. headroom_stats prices savings at a flat $3/1M and ignores the re-send cost. | Medium. Lossless in the tests, but lossy code paths exist: the SmartCrusher row-drop sentinel, and Kompress (≈15% kept) if `[ml]` is installed or `HEADROOM_KOMPRESS_ENDPOINT` is set. Proxy and wrap modes would rewrite every request; neither is installed. | Medium. Beacon on by default (counters plus a persistent install_id sent to a Cloudflare Workers endpoint). Daily PyPI check. tiktoken vocab download even with `HEADROOM_OFFLINE=1`. Verbatim copies in ccr_store.db that can outlive the 1 h TTL. | Auto-compaction, Read offset/limit and Grep, claude-mem smart_*; name collision with the Headroom skill. | No effect in MCP mode. Proxy mode would lower thinking effort (not installed). | Deferred tools are visible to subagents. A separate server process per subagent is *unverified* (one process observed). | No interaction in MCP mode. Proxy mode would break it. | **Remove** |
| **Headroom (synced skill plugin)** | Small positive on large files. Projected ~88 tokens always-on, ~470 on invoke. | Low. Outlines are approximate by design. A model-written digest can drift from the raw data. | Low. Stdlib-only, no network, no writes. Synced, so it can auto-update. | Read offset/limit, Grep, jq; claude-mem smart_outline. | None. | Listing entry in each subagent (~88 tokens). | None. | **Keep** (digest rule) |
| **Claude Code Setup** | Small negative. Projected ~141 tokens always-on in every session and subagent; ~4.1k on invoke, plus discovery commands and web search. No ongoing benefit. | No direct effect. Indirect: its advice includes blanket Edit/Write and `Bash(git commit:*)` allow rules, headless `claude -p --allowedTools Edit,Write` pipelines, format hooks and a "Memory MCP". | Low. Markdown only, first-party. Auto-updates from claude-plugins-official. | Fully overlaps this audit, the sohamalloc-* skills, find-skills and /plugin. | None. | Listed in workflow subagents (confirmed). Its description matches setup and audit wording, so a subagent could trigger it by accident. | None. | **Disable** (re-enable for a one-off run) |
| **Claude-Mem stack** | Today: projected ~3,017 tokens always-on (claude-mem 2,512 + cowork 389 + claude-memory 116), plus 19 deferred MCP tool names and hook-error notices. If made functional: one Haiku/Sonnet observer turn per tool call, subagents included. Older versions reported 10x amplification (#3497) and 76M tokens a day (#2315). | High if functional: the read gate replaces full Reads of unchanged large code files with LLM-written observation timelines, LLM summaries are injected at SessionStart, per prompt and per file, and the injected memory instructions compete with auto-memory. Today: none. | High if functional: plaintext SQLite and Chroma never purged (retention 0), redaction off, an unauthenticated API on 127.0.0.1:37700, cloud sync to cmem.ai, alternative providers, Telegram. Today: low; a PostHog `worker_start_failed` POST was attempted at 22:26:17Z (arrival unobserved). | Auto-memory, docs/STATUS.md and HANDOFF.md, task-observer, Notion memory. cowork and claude-memory are redundant with each other and with auto-memory. | The observer competes for the same plan quota (accounting *unverified*, section 7). | `SKIP_SUBAGENT_OBSERVATIONS=false`, so every subagent tool call is captured. The read gate and timeline skip subagents. cowork would rewrite every Agent/Task prompt and auto-allow it. | No mechanical conflict. Notices appear in the remote view. | **Remove** all three |
| **Task Observer** | (A) Negative. About 250 tokens of listing per session and subagent (estimate; `claude plugin details` attributes 0 to the skills-dir plugin layer, because the skill is counted as a personal skill). About 11–13k per invocation, 50–80k in the first session, about 5k automatic re-attach after compaction (more if re-invoked), and 50–100k+ per weekly review. | Does not alter numbers or evidence. Its log-and-defer rule would park "from now on" rules as observations instead of auto-memory. Imperative prompt pressure. Observations become future instructions. | Low. No network in normal operation and no telemetry. Writes only to its workspace outside the repo. Its upstream feedback flow would post publicly if the user started it. | Auto-memory (direct conflict), claude-mem, the synced (B) plugin of the same name, the schedule and loop skills. | Heavy procedural text added to every invoked session. | Its description tells every session to invoke it first. Script-dispatched workflow subagents are not exempt by its own rules, so a 20-subagent run could add about 250k tokens if each one complies (estimate; compliance rate not measured). | No mechanical conflict. `skillOverrides: "off"` also hides it from Remote Control command lists (docs). | **Keep (A) manual-only**; (B) stays disabled |
| **OmniRoute** | No legitimate saving of Claude quota. The "~89%" figure is arithmetic on assumed per-engine averages, not a measurement. The only savings would come from substituting other models or (opt-in) compression. | Conditional-high: combos and auto-fallback can silently substitute models (none configured today). Compression, PII masking and radar are off by default; the injection guard only warns. | **Unacceptable** if connected: routing the subscription through its Claude OAuth provider breaches the terms; call logs up to 512 KB per request kept 90 days (on by default); upstreams outside the boundary; binds 0.0.0.0:20128; `REQUIRE_API_KEY=false`; default dashboard password CHANGEME. Dormant install today: low (never connected, 0 requests). | headroom, five memory systems, MCP servers. | Effort and thinking pass-through *unverified*. Cannot hold with non-Anthropic upstreams. | Per-request routing makes multi-agent runs non-reproducible. | **Incompatible** (docs: not available when `ANTHROPIC_BASE_URL` points at a gateway or proxy). | **Remove** |
| **FreeLLMAPI** | Uses no Anthropic quota because Opus is never called: `opus→auto` (free pool). Quality drops as daily caps run out. | High: random model per request, silent failover, opt-in lossy compression, Fusion, response cache. Relays may return a different model; AI Horde drops tool calling. | **Unacceptable**: Kilo logs prompts for training, Gemini's unpaid tier allows human review, AI Horde runs on anonymous volunteer workers, relays are unknown. Setting only `ANTHROPIC_BASE_URL` would send the claude.ai OAuth credential to the proxy. | Replaces the backend entirely; compression, cache and memory overlap the others. | Incompatible in practice. | 429 storms that fail over to the weakest models. | **Incompatible** (docs). | **Do not install** |

---

## 3. Per tool

### 3.1 Headroom (a): headroom-ai 0.41.0, user-scope MCP server "headroom"

**Findings**
- Identity: PyPI headroom-ai 0.41.0, Apache-2.0, github.com/headroomlabs-ai/headroom, open-core with a paid Enterprise tier. Installed via `uv tool` with only the `[mcp]` extra (`~/.local/share/uv/tools/headroom-ai/uv-receipt.toml`). Registered in `~/.claude.json` `mcpServers.headroom` as stdio `headroom mcp serve` with `env {}`. `~/.headroom` still holds only `update_check.json` (re-checked 2026-10-22), so headroom_compress has never been called.
- Tools: headroom_compress, headroom_retrieve, headroom_stats. headroom_read stays off unless `HEADROOM_MCP_READ=on` (`headroom/ccr/mcp_server.py`).
- Token effect, corrected by the fact-check:
  - The evaluation's 67% savings figure was counted with Headroom's 3.5 chars/token fallback estimator. With the o200k_base counter Headroom normally uses for claude-* models, the result is about 59%: 12,949→5,281 and 87,296→35,979 tokens, lossless. Neither counter is Claude's tokenizer.
  - Text, CSV and logs: 0% in this install.
  - The net effect in Claude Code is negative regardless, because compress needs the content re-sent as output tokens.
- Telemetry, corrected:
  - The beacon is on by default (`telemetry/beacon.py BEACON_DEFAULT_ON = True`). It fires, and the savings ledger is written, **only when a compress call actually saves tokens** (`_record_savings` returns early if `before <= after`). install_id is created on the first beacon POST, not on every call.
  - Working opt-outs in 0.41.0: `HEADROOM_BEACON=off`, `DO_NOT_TRACK=1`, `HEADROOM_OFFLINE=1`. `HEADROOM_TELEMETRY=off` is honoured only on the unreleased main branch, so it does not work in 0.41.0. The packaged 0.41.0 README does not claim it does; the main-branch README is ahead of the release.
- Verbatim copies: every compress call stores the original in `~/.headroom/ccr_store.db` (0600), even when compression is a no-op. Expired rows (1 h TTL) are purged only on open or on a write, at most once every 60 s (`cache/backends/sqlite.py`), so a copy can outlive the TTL.
- An extra outbound path the evaluation missed: on first use, tiktoken downloads `o200k_base.tiktoken` from openaipublic.blob.core.windows.net into `/tmp/data-gym-cache`, even with `HEADROOM_OFFLINE=1`. It is a GET for a public file and carries no content. The fact-checker's own offline run created `/tmp/data-gym-cache/fb374d419588a4632f3f557e76b4b70aebbca790` (3.6 MB, still present). It is not evidence; the user may delete it.
- `headroom learn` (never run) writes not only `CLAUDE.local.md` but also the project's auto-memory `~/.claude/projects/-home-sohambasanwar-sohamalloc/memory/MEMORY.md` (`learn/writer.py`, `learn/plugins/claude.py`).
- Correction to the configuration audit: the audit called headroom "lossy on JSON arrays". In the tests the JSON arrays were re-encoded losslessly into a sorted-key columnar form. The row-drop path exists in `transforms/smart_crusher.py`, but the conditions under which it triggers are *unverified*.
- *Unverified, not used here*: the repo statistics (stars, commits); the tagline "20% fewer tokens for coding agents, 60–95% … JSON" (not found in either README); whether the beacon payload's client name is filled in on the MCP path; whether there is one server process per subagent.

**Actions** (the user runs these, not an agent)
1. Read-only check first: `ls -la ~/.headroom`. Expected: only `update_check.json`.
2. Unregister (reversible): `claude mcp remove headroom -s user`, then restart Claude Code.
3. DESTRUCTIVE (removes the tool venv; can be reinstalled): `uv tool uninstall headroom-ai`.
4. DESTRUCTIVE (Headroom state only; run step 1 first so that no `ccr_store.db` is lost unseen): `rm -rf ~/.headroom`.
5. Optional cleanup of the fact-checker's side effect (not evidence): `rm /tmp/data-gym-cache/fb374d419588a4632f3f557e76b4b70aebbca790`.
6. If kept against this advice:
   - Set the env block of the `~/.claude.json` headroom entry to `HEADROOM_OFFLINE=1`, `HEADROOM_BEACON=off`, `DO_NOT_TRACK=1`, `HEADROOM_UPDATE_CHECK=off`, `HEADROOM_STATELESS=1` (blocks every workspace write) and `HEADROOM_CCR_BACKEND=memory`. Pre-populate `TIKTOKEN_CACHE_DIR` or block egress.
   - Pin the version with `uv tool install 'headroom-ai[mcp]==0.41.0'`.
   - Never install `[ml]` or `[all]`, and never set `HEADROOM_KOMPRESS_ENDPOINT`.
   - Never run `headroom proxy`, `wrap`, `mcp install`, `learn` or `update`, and never point `ANTHROPIC_BASE_URL` at it.
   - Never pass `bench/out` or `docs/research-log/data` content to headroom_compress.

### 3.2 Headroom (b): synced skill plugin "headroom" 1.3.2 (Stefan Petrovici, MIT)

**Findings**
- Contents: `SKILL.md` (29 lines), the `/headroom:peek` command and `scripts/peek.py` (98 lines). Stdlib only, no network, no writes (synced dir `~/.claude/plugins/synced/e41a7153-…/b9db7a50-…/`). It has nothing to do with headroom-ai.
- Risk: SKILL.md suggests `notes/<file>.digest.md` and "Refer back to the digest instead of re-reading the source". A digest written into the working tree could be committed under the push-after-every-update rule, and a digest of benchmark data can drift from the raw data.
- *Unverified*: whether listing in the Anthropic plugin directory involved any code review.

**Actions**
1. Keep it enabled.
2. Add a project rule (in the sohamalloc-research skill or a CLAUDE.md): digests go only in the session scratchpad, never in the repo or `docs/research-log/data`; outline and digest text is for navigation only; quoted numbers come from the raw file by line range or from a script.
3. Re-read `scripts/peek.py` whenever the synced version changes.
4. Optional, reversible: `"headroom@synced": false` in `/home/sohambasanwar/sohamalloc/.claude/settings.local.json`, if the two "headroom" names cause confusion.

### 3.3 Claude Code Setup (claude-code-setup@claude-plugins-official 1.0.0)

**Findings**
- First-party Anthropic plugin (Apache-2.0) with one skill, `claude-automation-recommender`. No hooks, MCP, scripts or agents. The local copy matches upstream main byte for byte (sha256 check by the fact-checker). Never invoked.
- Corrections:
  - The evaluation's key action, `skillOverrides: "name-only"`, **cannot work**. code.claude.com/docs/en/skills states "Plugin skills are not affected by `skillOverrides`. Manage those through `/plugin` instead."
  - The non-standard frontmatter field `tools:` is silently ignored (docs: unknown fields are ignored without error), so nothing restricts the skill's tools.
  - SKILL.md is 10,995 bytes, not 10,976.
  - The README says "read-only - it analyzes but doesn't modify files". The "does NOT create or modify any files" wording is only in SKILL.md.
- Missed by the evaluation: SKILL.md "Configuration Tips" (≈ lines 255–290) recommends blanket `"Edit"`, `"Write"` and `"Bash(git commit:*)"` allow rules and headless `claude -p … --allowedTools Edit,Write` pipelines. For a repo with evidence to preserve, that advice is riskier than the format-hook example the evaluation cited.
- Cost (projected, `claude plugin details`): ~141 tokens always-on, ~4.1k on invoke. The listing entry appears in workflow subagents (confirmed by two checkers and visible in this subagent).
- Auto-update is on by default for claude-plugins-official and for marketplaces added from claude.ai (code.claude.com/docs/en/discover-plugins).

**Decision on the verdict disagreement:** the evaluation said keep-with-changes and the checker said disable. **Disable.** The only change that would have made "keep" cheap is not available for plugin skills. The evaluation's own findings (standing cost in every session and subagent, accidental-trigger risk in setup-audit workflows, full overlap, no ongoing benefit) then point to disabling. Disabling is reversible, and the plugin can be re-enabled for an explicit one-off run.

**Actions**
1. Reversible: `claude plugin disable claude-code-setup@claude-plugins-official -s user`. Alternatively, for this project only, add `"claude-code-setup@claude-plugins-official": false` to `enabledPlugins` in `/home/sohambasanwar/sohamalloc/.claude/settings.local.json`, next to the existing `claude-code-setup@synced: false`.
2. If it is kept enabled instead: add the deny rule `"Skill(claude-code-setup:claude-automation-recommender)"`. Whether that also removes the listing cost is *unverified*.
3. For a one-off run, re-enable it, invoke it explicitly ("hooks only, read-only, no installs"), and audit anything it names (context7, GitHub MCP, Memory MCP, hookify) before installing it. Reject its blanket allow rules.
4. Optional (no data loss; removes only the cache): `claude plugin uninstall claude-code-setup@claude-plugins-official`.

### 3.4 Claude-Mem stack (claude-mem@thedotmack 15.0.1, claude-mem-cowork@thedotmack 0.1.4, claude-memory@anthropic-plugin-directory 1.0.0)

**Findings**
- Three projects:
  - claude-mem: Alex Newman, Apache-2.0.
  - claude-mem-cowork: CMEM, Inc., PolyForm-Noncommercial; a shim that sends hook data to cmem.ai.
  - claude-memory: Creighton Chang, MIT. Its README says "Archived 2026-09-23" because auto-memory now does the same.
  - All three were installed at 22:18–22:26Z (`~/.claude/plugins/installed_plugins.json`).
- State (re-checked): Bun is absent, `~/.claude-mem/CAPTURE_BROKEN` exists, and there is no DB, Chroma or spool. `~/.claude/plugins/data/claude-mem*-thedotmack/` are empty, so uninstalling loses no data. The hooks fail fast as non-blocking errors ("Executable not found in $PATH: \"claude-mem\"").
- Telemetry, corrected and strengthened: telemetry.json was created at 22:26:17.946Z, 21 ms after CAPTURE_BROKEN. In `mcp-server.cjs` the installId generator is called only inside the PostHog POST body, after the consent check. So a `worker_start_failed` POST to us.i.posthog.com **was attempted**; whether it arrived is unobserved. The payload is allowlisted metadata.
- Read gate, narrowed:
  - It applies only to the main agent, to code files of 32 KiB or more that resolve inside the workspace, have past observations and are **unchanged since the newest observation**, and only to Reads with offset ≤1.
  - The deny reason tells the model that partial reads are allowed.
  - The file-context handler (both timeline injection and deny) returns early for subagents. This settles an item the evaluation had left unverified (`worker-service.cjs`, file-context handler).
- Retention, verified: `TOOL_USES_RETENTION_DAYS=0` means never purged (`APe`/`OPe` in worker-service.cjs).
- Outbound integrations are less close than "one setting away":
  - Cloud sync needs a token, a user ID and a hub URL.
  - Telegram alerts need a bot token, a chat ID and `TELEGRAM_OBSERVATION_ALERTS_ENABLED=true`, which is currently false.
  - The cloud-sync skill's pairing flow can still set all the sync fields at once, and cowork needs only the token, through its `CLOUD_SYNC_TOKEN` fallback.
- Usage reports corrected:
  - #3839: the 681k-char prompt (495,631 cache-write tokens) is from the 13.21.2 report, closed "Not planned".
  - The "falls back to the Anthropic subscription when the CMEM Pro allowance runs out" comment comes from another user on 13.24.0.
  - The cmem.ai changelog supports "the quota guard did not fire" (#3838), but the exact phrase "never fired" was not found in the scraped page.
- *Unverified*: #2315, #2643, #1742, #1826 and the DataCamp figure were not re-fetched; whether the 15.0.1 history cap and quota guard bound cost; star counts.
- Terms: the observer drives the user's OAuth login through the Agent SDK. code.claude.com/docs/en/legal-and-compliance says Agent SDK developers "should use API key authentication", and also allows "an end user … signing in to the unmodified Claude Code binary with their own Claude subscription". That makes this a grey zone, not a settled breach.

**Actions**
1. Do NOT run `npx claude-mem install` or `repair`, and do not install Bun or uv for this plugin, despite the SessionStart message.
2. Do this before the next restart. Disable all three in one step (reversible):
   `claude plugin disable claude-mem-cowork@thedotmack -s user`
   `claude plugin disable claude-mem@thedotmack -s user`
   `claude plugin disable claude-memory@anthropic-plugin-directory -s user`
   Then restart Claude Code.
3. Interim telemetry opt-out, until the uninstall: add `"enabled": false` to `~/.claude-mem/telemetry.json`. This is the path the code honours. Adding `DO_NOT_TRACK` to the `env` block of `settings.json` relies on Claude Code passing that env to MCP server processes, which is *unverified*.
4. Uninstall (removes the plugin cache and the empty plugin data dirs; no captured data exists):
   `claude plugin uninstall claude-mem-cowork@thedotmack`
   `claude plugin uninstall claude-mem@thedotmack`
   `claude plugin uninstall claude-memory@anthropic-plugin-directory`
   `claude plugin marketplace remove thedotmack`
   Then delete `extraKnownMarketplaces.thedotmack` from `~/.claude/settings.json`.
   (Command syntax confirmed with `claude plugin … --help`, Claude Code 2.1.308.)
5. Verify:
   - `ls ~/.local/bin/claude-mem` should fail.
   - `grep -n claude-mem ~/.bashrc ~/.profile` should print nothing.
   - `ss -ltnp | grep 37700` should print nothing.
   - There should be no `*claude-mem-observer-sessions*` directory under `~/.claude/projects`.
6. DESTRUCTIVE, optional: `ls -la ~/.claude-mem` first (expected: settings.json, two migration markers, logs/, CAPTURE_BROKEN, telemetry.json, and no `*.db`, `chroma/` or `cowork-spool.jsonl`), then `rm -rf ~/.claude-mem`.
7. Keep built-in auto-memory plus docs/STATUS.md as the memory system. Keep evidence out of the Notion memory connector and out of cmem.ai.

### 3.5 Task Observer

**Findings**
- Two unrelated projects share the name:
  - (A) Personal skill: rebelytics/one-skill-to-rule-them-all v3.7.0, CC-BY-4.0, in `~/.claude/skills/task-observer/`. Installed with `npx skills add … -g -a claude-code` (`~/.bash_history` lines 244 and 252).
  - (B) Synced plugin: "Task Observer: Learn Your Preferences" v1.3.2, Petrovici, MIT. Already disabled in `sohamalloc/.claude/settings.local.json`.
- Correction (missed by the evaluation): (A) also loads as a **skills-directory plugin** `task-observer@skills-dir`, because its folder contains `.claude-plugin/plugin.json` (code.claude.com/docs/en/plugins/create.md; `~/.claude.json` usage table).
  - `claude plugin details task-observer@skills-dir` (run for this report) shows 0 skills, 0 hooks and 0 MCP servers in that plugin layer. The SKILL.md is accounted for as a personal skill.
  - Consequence 1: `skillOverrides` should apply to it. It is a personal skill, not a plugin skill. This still needs confirming in `/skills`.
  - Consequence 2: a future update that adds `hooks/hooks.json` would load automatically with no install prompt.
- Missed name conflict: by the docs' precedence rules, a skills-dir plugin wins over a synced plugin of the same name. This has no effect in sohamalloc, where (B) is disabled.
- Corrections:
  - The "a memory note" quotation was wrong. The log-and-defer paragraph (SKILL.md ≈575–589) lists "a state file …, a prompt, a handoff doc, a project instruction file", with the test "anywhere a later run reads it". The conclusion stands: auto-memory is read by later runs.
  - The Co-authored-by trailer in its published-skill flow credits human GitHub contributors in the skill's own repo. It is not an AI trailer and does not conflict with the sohamalloc rule.
  - Step 0 (scheduled autonomous review) is conditional. It is "fallback mode only", skipped with no open observations, skipped if config forbids it, and suppressed for 30 days after a decline.
  - After compaction, about 5k tokens of the skill re-attach automatically (docs: first 5,000 tokens per skill, 25k budget in total). The full cost returns only if the skill is re-invoked.
  - find-skills was installed by the skills CLI's own prompt during the first task-observer install, 1.4 s later (`~/.agents/.skill-lock.json`), not by a separate command.
  - The description is 999 characters.
- Observed in this report-writing session: this workflow subagent's skill listing includes task-observer with "IMPORTANT: invoke this skill before the FIRST tool call of any session". It was not invoked.
- The lock's `skillFolderHash` is a record the skills CLI uses for update checks, not an enforced pin.

**Decision:** the evaluation said keep-with-changes and the checker agreed but called "remove" defensible. **Keep (A) as manual-only.** The user installed it deliberately, twice. Manual-only removes the Ultracode cost and the auto-memory conflict unless the user invokes it. If the restriction cannot be confirmed to take effect, fall back to removal.

**Actions**
1. In `~/.claude/settings.json`: `"skillOverrides": { "task-observer": "user-invocable-only" }`. Use `"off"` to hide it everywhere, including Remote Control command lists. Then confirm in `/skills` that it shows as user-only, and check that new sessions and subagents no longer list it.
2. If it is still listed: set `"enabledPlugins": { "task-observer@skills-dir": false }` (whether that also hides the root SKILL.md is *unverified*), or move the directory out of `~/.claude/skills/`. Moving or deleting the directory is DESTRUCTIVE to the install only; no observation data exists. Reinstall with `npx skills add rebelytics/one-skill-to-rule-them-all --skill task-observer -g -a claude-code`.
3. Diff before any `npx skills update`. This is mandatory, because new hooks would auto-load.
4. When invoking it manually, decline the following:
   - the repo CLAUDE.md activation block;
   - the SessionStart, Stop and PreToolUse hook recipes (the skill-file write gate would also block edits to the sohamalloc-* skills);
   - scheduled autonomous reviews;
   - the first-run backfill over `docs/`.
   Pin its workspace to `/home/sohambasanwar/.claude/skill-observations`, mark observations about findings or benchmarks `type: internal`, and keep "from now on" rules in auto-memory.
5. Do not run its upstream feedback pre-flight with project content.
6. (B): no change; it is already disabled.

### 3.6 OmniRoute (npm omniroute 3.8.52, installed, dormant)

**Findings**
- Identity: github.com/cheaper-inference/OmniRoute (formerly diegosouzapw), MIT. Installed with `npm install -g --allow-scripts=… omniroute@3.8.52` (`~/.bash_history` 262–278), 2.8 GB under `~/.nvm/versions/node/v24.22.1/lib/node_modules/omniroute`.
- It ran once, 22:23:40–22:24:49Z, with "Draining 0 request(s)", 0 keys, 0 providers and 0 call logs. It is not running and Claude Code was never pointed at it (`~/.omniroute/logs/application/app.log`).
- Corrections (all reduce inherent risk, none changes the verdict):
  - Compression is **off** by default and on this install (`open-sse/services/compression/types.ts`; local DB). The per-key compressionEnabled flag defaults to true for every key, but it matters only if global compression is enabled.
  - The PII masker is opt-in. The prompt-injection guard runs in warn mode, not block mode.
  - The judge model client is playground-only.
  - radar.omniroute.online is opt-in (`RADAR_ENABLED` false).
  - The "~89%" figure is a formula, `1-(1-0.80)(1-0.46)`, on an illustrative request.
  - Discussion #11127 was answered by a contributor, not the maintainer.
  - The GitHub advisory GHSA-hf57-cqmx-p4gr gives severity critical with no CVSS number. "9.5" comes from the OSV vector or a secondary source.
  - A second `.env` (a copy of `.env.example`, with `INITIAL_PASSWORD=CHANGEME` and `REQUIRE_API_KEY=false`) sits in the package directory and is removed by `npm uninstall`.
- Findings that hold under any configuration:
  - Remote Control is unavailable when `ANTHROPIC_BASE_URL` points at a proxy, and when `ANTHROPIC_AUTH_TOKEN` is in use (code.claude.com/docs/en/remote-control).
  - Its "claude" OAuth provider stores and refreshes Claude.ai tokens. The docs say developers "may not collect, store, or intermediate Claude.ai credentials or session tokens" (code.claude.com/docs/en/legal-and-compliance).
  - The call-log pipeline is on by default and keeps request bodies up to 512 KB for 90 days.
  - It binds 0.0.0.0 by default.
- *Unverified*: CVE-2026-49352 and the other "auth-bypass CVEs" (secondary, paywalled source); whether the dashboard password was changed; effort pass-through; whether the dashboard exposes stored bodies to LAN peers.

**Actions**
1. Never run `omniroute launch` or `omniroute setup-claude`, never set `ANTHROPIC_BASE_URL` or `ANTHROPIC_AUTH_TOKEN`, and never connect its Claude OAuth provider.
2. Check `ss -ltnp | grep -E '20128|20131|20132'`; it should print nothing.
3. Uninstall in an nvm shell: `nvm use 24 && npm uninstall -g omniroute`.
4. DESTRUCTIVE, optional: copy `~/.omniroute/logs/application/app.log` into the research log first if wanted, then `rm -rf ~/.omniroute ~/.config/configstore/update-notifier-omniroute.json`.
5. DESTRUCTIVE, optional: `nvm uninstall 24` only after `ls ~/.nvm/versions/node/v24.22.1/lib/node_modules` shows nothing else is needed.
6. Record the install, the single run and the removal in the research log (AI-tooling rule).

### 3.7 FreeLLMAPI (tashfeenahmed/freellmapi v0.16.7, not installed)

**Findings**
- MIT, with a paid Premium catalog ($19/yr or $49 lifetime) and mobile and desktop apps.
- Not installed: none of `~/freellmapi`, `~/.config/FreeLLMAPI` or `~/.freellmapi` exists, and nothing listens on 3001 or 31415.
- `anthropic-map.ts` maps `default/opus/sonnet/haiku` to `auto`, the free pool.
- Corrections:
  - Anthropic's docs say they do not support "routing Claude Code to non-Claude models through any gateway" and do not audit third-party gateways (code.claude.com/docs/en/llm-gateway). This use is unsupported, not merely "documented". Whether it breaches the terms was not checked.
  - Credential risk the evaluation missed: with only `ANTHROPIC_BASE_URL` set, "a saved claude.ai login remains the active credential" (same page). The OAuth credential would then go to the local proxy.
  - The project has 102 contributors (not 98). The app is rated 1.7 stars (not 1.8) and its developer is listed as "Tash A".
  - OpenCode Zen free rows have been disabled since 2026-09, so it is no longer an active training-data path.
  - Kilo and OVH appear to need the user to add the platform, even without a key. Only AI Horde claims to auto-configure, and no seeding code was found, so its state on a fresh install is *unverified*.
  - It has 43 built-in platforms; 34 is the catalog count.
  - AI Horde drops tool calling.
- Related finding: `~/.hermes/config.yaml` already routes the Hermes agent to OpenAI Codex (chatgpt.com). That is outside the stated trust boundary if Hermes is ever run on this repo.
- *Unverified*: the 30-minute model stickiness; whether analytics or logs keep request bodies; the "catalog never sees prompts" claim beyond catalog-sync.ts; relay operators' access, logging and resale rights.

**Actions**
1. Do not install it and do not point any client (Claude Code, the claude-mem observer, Hermes) at it. Do not run the `curl … | bash` installer, `npx freellmapi setup-claude`, `launch` or `setup-hermes`.
2. Keep `ANTHROPIC_BASE_URL` unset. `env | grep -i ANTHROPIC_BASE_URL` should print nothing.
3. Record the rejection in the research log (v0.16.7, 2026-10-22, with the reasons).
4. Do not run Hermes inside `/home/sohambasanwar/sohamalloc` while it points at an external provider.

---

## 4. Configuration audit

Inventory source: the read-only audit of 2026-10-22, 22:30–22:45 UTC. Main files: `~/.claude/settings.json`, `sohamalloc/.claude/settings.local.json`, `~/.claude.json`, `~/.claude/plugins/installed_plugins.json`, synced manifest, `~/.claude/skills/`. Claude Code is 2.1.308.

### 4.1 Findings by severity (corrections applied)

**High**
1. **claude-mem is installed but cannot run.**
   - Its 8 `"command": "claude-mem"` handlers fail because the command is not on PATH, and Bun is missing.
   - The sync PreToolUse handler (15 s) uses the matcher `Read|^Bash$|^Grep$|^Glob$|…`. Its `Read` part is unanchored, so it also matches tools such as ReadMcpResourceTool. The sync UserPromptSubmit handler also runs on every prompt.
   - Each failure is a fast, non-blocking error (code.claude.com/docs/en/hooks), but the errors add noise.
   - Fix: section 3.4.
2. **claude-mem behaviour once working** would conflict with the project:
   - the read gate (narrowed scope: main agent only, unchanged files only);
   - an observer turn per tool call, including subagents;
   - redaction off, transcripts on, retention never-purge.
   - That the observer bills to the 5-hour and weekly windows is an *inference* from `CLAUDE_MEM_CLAUDE_AUTH_METHOD=subscription` (section 7).
3. **Duplicate memory systems.** Besides the canonical auto-memory (`~/.claude/projects/-home-sohambasanwar-sohamalloc/memory/`: MEMORY.md 609 B plus 4 files), the setup has:
   - claude-mem (broken);
   - claude-mem-cowork (inert by design while claude-mem is installed; it **becomes live** if claude-mem is removed while a cmem key exists);
   - claude-memory (its path bug, the `sed 's|^-||'` in `memory-context.sh`, makes it output nothing);
   - Notion Memory, announced by the connector;
   - task-observer's observation log (inactive).
   Fix: remove the three plugins and keep auto-memory plus STATUS.md.
4. **Evidence only on this disk, or missing.** See section 5.

**Medium**
5. **Auto mode with no allow, ask or deny rules, plus Remote Control at startup.** Many upload-capable tools are in reach:
   - Gmail send and forward;
   - Drive create and share;
   - Notion pages and file upload;
   - Linear attachments;
   - Canva and Figma uploads;
   - Publora (public social posts);
   - spArxx timestamping;
   - Firecrawl queries;
   - claude-in-chrome;
   - engineering@synced Slack and GitHub;
   - find-skills `npx … -y`.
   The prose in `autoMode.environment` is the only guard. Verified on code.claude.com/docs/en/permissions: deny rules use `mcp__claude_ai_<server>__<tool>` for claude.ai connectors and `mcp__plugin_<plugin>_<server>__*` for plugin servers. Verified on code.claude.com/docs/en/settings: `disableClaudeAiConnectors: true` is honoured from any scope, and `permissions.defaultMode` `auto` does not take effect from project or local settings.
6. **Skills that force their own invocation.** task-observer (A) gets around `task-observer@synced: false` and also loads as `task-observer@skills-dir`. claude-mem's deny message advertises its own tools. Some connector instructions (Figma, Consensus) claim MANDATORY use; they appear in this subagent's context as well.
7. **headroom.** The name collides with the skill; the MCP route is net negative; the beacon is on by default. Correction to the audit wording: the JSON re-encoding was lossless in the tests, and a lossy path exists in code.
8. **Duplicate skills and MCP servers.**
   - Several skills cover code review (engineering:code-review, built-in code-review and security-review, sohamalloc-security-audit).
   - Standup is covered twice and memory four times.
   - claude-mem:version-bump ends with `npm publish`, learn-codebase reads every file, and handoff writes a HANDOFF.md that competes with docs/HANDOFF.md.
   - engineering@synced declares Linear and Notion MCP servers that duplicate the claude.ai connectors.
   - The git-commit skill writes Conventional Commits subjects, which do not match the repo's plain subjects.
9. **Hooks on every event.** There are 18 handlers: 17 from thedotmack and 1 from claude-memory. Each tool call starts up to 3 processes. A no-op start of cmem-hook.mjs measured 0.07–0.08 s and about 59 MB RSS. Under Ultracode this multiplies, because plugin PreToolUse and PostToolUse hooks fire inside subagents (code.claude.com/docs/en/hooks). Removing the three plugins brings this to 0.
10. **Change outside the repo at the next startup.** ensure-launcher.cjs (sync, 180 s) would try to edit `~/.bashrc`. *Main-session check: it does not here, because `~/.local/bin` is already on PATH, and the Bun compile fails.* Settle claude-mem first anyway.
11. **docs/HANDOFF.md is stale** (4.3).

**Low**
12. **docs/STATUS.md.**
    - "Last updated 17:55 UTC" does not match the commit time of a811d6f, 17:39Z.
    - Next item 2 still lists "Finish campaign 1" and "aligned handler kind", both done (f3809c0).
    - It cites `bench/out/report.md`, which is absent (confirmed: `ls bench/out` shows only the sigstorm and smoke directories).
    - The resume scripts live outside git.
13. **Update channels.**
    - Claude Code: `autoUpdatesChannel=stable` and `minimumVersion=2.1.306` in settings.json, while `~/.claude.json` has `autoUpdates=false`. How these interact is *unverified*.
    - Plugin auto-update is on for claude-plugins-official and for marketplaces added from claude.ai, and off for third-party marketplaces such as thedotmack. Whether anthropic-plugin-directory counts as "added from claude.ai" is *unverified*.
    - headroom-ai checks PyPI daily. OmniRoute's update-notifier checks npm daily.
14. **Telemetry.** claude-mem PostHog is on by default and one POST was attempted. GROK_BOT and CCS_ALIGN are on, and TELEGRAM_ENABLED is true but inactive (alerts flag false, no token). This is resolved by removal.
15. **Commit trailers.** The system reminder of every session asks for `Co-Authored-By` and `Claude-Session` trailers; this report-writing subagent received the same reminder. Only the memory rule `git-authorship-no-trailers.md` prevents them, and there is no git hook. `git log` shows 0 such trailers today.

**Info**
16. clangd runs without `compile_commands.json`. If one is added, `.cache/clangd/` is not git-ignored.

### 4.2 Context cost

Projected always-on tokens per session from `claude plugin details` (2026-10-22, Claude Code's estimate). They apply again in every subagent that receives the listing. This subagent's listing contains all of them.

| Source | Always-on (projected) | Notes |
|---|---:|---|
| claude-mem@thedotmack | ~2,512 | 27 skills, plus 19 MCP tool schemas resolved at runtime (not counted) |
| claude-mem-cowork@thedotmack | ~389 | 3 skills |
| claude-memory@anthropic-plugin-directory | ~116 | 1 skill |
| engineering@synced | ~873 | 10 skills; 10 MCP servers declared (not counted) |
| claude-code-setup@claude-plugins-official | ~141 | ~4.1k on invoke |
| headroom@synced | ~88 | |
| clangd-lsp | 0 | out-of-process |
| task-observer@skills-dir | 0 | the skill itself is counted as a personal skill, ≈250 tokens (estimate) |
| account-synced skills (14) | ≈2,100 (8,382 chars ÷ 4, estimate) | removed by `syncClaudeAiSkills: false` |

- Removing the claude-mem stack, claude-code-setup and engineering@synced saves about 4,031 projected tokens per session and per subagent. Making task-observer manual-only saves about 250 more, and `syncClaudeAiSkills: false` about 2,100 more (estimates).
- The audit's character totals agree in order of magnitude: about 21,300 characters of descriptions in all. The corrected SKILL.md size for claude-code-setup is 10,995 B.
- Deferred MCP tool names: about 450, mostly claude.ai connectors (Linear 74, Notion 56, Figma 39, Canva 38, Publora 32, Gmail 28), estimated at roughly 16,000 characters. How deferred names are billed is *unverified*.
- MCP server instruction blocks visible in this session: Notion, Figma, Firecrawl, Publora, Claude Docs, Consensus, Gamma, Linear, claude-in-chrome. Estimated at 12,000–16,000 characters, not counted exactly. `disableClaudeAiConnectors: true` for the project would remove both the names and the instruction blocks.
- Hook-injected text today is 0 bytes for all three memory plugins (two are broken, cowork is inert). additionalContext is capped at 10,000 characters per string (hooks docs).
- Auto-memory: MEMORY.md is 609 B and loads every session.

### 4.3 HANDOFF.md corrections

docs/HANDOFF.md says "Where the project stands (2026-10-19)" and is stale against docs/STATUS.md (2026-10-22) and the git log:
1. The test table is outdated. Current figures on fd735dd: 23/23 native, debug, aarch64 and armhf; musl 22/22 + 1 skipped; mingw (wine) 9/9; sanitizers 9/9; preload 9/9; sigstorm (debug build) 54/54. Source: `docs/research-log/data/verify/2026-10-21-review3-fixes-matrix.txt`.
2. "v0.1 is complete" omits the review rounds since then:
   - R2-1…R2-5, fixed in c1a9791;
   - R3-1 (critical) and R3-2/3/4 (major), fixed in fd735dd;
   - R3-5/6, recorded as limitations;
   - round 4 not done: wf_a26b2874-42a stopped at the usage limit, and wf_f93f99a2-398 was stopped by the user.
3. Plan step 1 ("tools/sigfuzz", a ptrace or perf tester) was built as **tools/sigstorm** instead (911d2ed, 85f3b78, f3809c0).
   - Campaign 1 is finished: sohamalloc had 0 failures in 540 trials (95% CI [0, 0.007]), and the other allocators failed in 50–100% of trials.
   - The fd735dd rerun stopped at 456/810 trials, all of which survived.
   - Data: `docs/research-log/data/sigstorm-campaign1/` and `sigstorm-c1-soham-fd735dd-partial/`.
4. Plan step 2 (real-world evidence) was done in 55e6900.
5. Plan step 3 (GenMC or TLA+) was dropped without a recorded decision; `05-novelty-analysis.json` notes that no GenMC run exists. Either record the decision or restore the item.
6. Step 5 (re-run the four-lens review): rounds 2 and 3 happened. Research-log `03-defects-and-faults.md` exists, and 02, 04 and 06 are still missing.
7. The prototype handler-context fallback for requests above 16 KiB (`docs/research-log/data/proto-handler-fallback/`, not applied) is not mentioned.
8. The benchmark claim ("1.142, tied first") matches STATUS. But both cite `bench/out/report.md`, which is absent, and HANDOFF leaves out that no code after 273bcb9 has been benchmarked and that gate 4 is pending.
9. The paste-in prompt sends a new session to steps that are already done. It also leaves out the four gates, the usage-limit checkpoint rule, and STATUS as the source of truth.
10. Still correct: the authorship rule, the branch name, and the build and test commands.

Replacement (about 10 lines): "Read docs/STATUS.md first; it is the single source of truth." Then the current state in one line (fd735dd; gate-2 review round 4 pending; prototype patch pending; fd735dd sigstorm rerun at 456/810), the authorship rule, and the relaunch prompt: "Continue sohamalloc on branch sohamalloc-dev. Read docs/STATUS.md and docs/IMPLEMENTATION_NOTES.md. Start with STATUS 'Next' item 1 (review round 4 on fd735dd). Commit as Soham Basanwar with no AI trailers." Move the 2026-10-19 snapshot into `docs/research-log/01-timeline.md`. Also fix the three STATUS lines listed in 4.1 item 12.

---

## 5. Evidence preservation

### 5.1 Where the evidence lives (repo /home/sohambasanwar/sohamalloc)
1. **Tracked and pushed** to the private origin: `docs/research-log/data/`, 31 files:
   - machine records;
   - `proto-handler-fallback/`, `regress-before-after/`, `review2/`;
   - `review3/`: 3 tracked files;
   - `sigstorm-campaign1/` (1.2 MB);
   - `sigstorm-c1-soham-fd735dd-partial/`;
   - `verify/2026-10-21-review3-fixes-matrix.txt`.
2. **Git-excluded** (`.git/info/exclude` lines 7–9, re-checked; local to this clone, no off-machine copy):
   - `docs/research-log/data/review3/repros/` (8 reproducers);
   - `review3/review3-results.json`;
   - `review4/` (scratch snapshot, 36 KB).
3. **Git-ignored** (`.gitignore:3`, `bench/out/`, 1.8 MB; re-checked):
   - `sigstorm-campaign1/` and its .log;
   - `sigstorm-c1-soham-fd735dd/` and its .log (the frozen fd735dd library the rerun needs);
   - `smoke-resume/` and `smoke-runner/`.
   The `build*/` directories hold the binaries that the round-4 sha256 checks refer to.
4. **Missing from this machine:** `bench/out/report.md` and the B1–B4 cloud-VM campaigns (`01-timeline.md:735`, `:948`; cited by STATUS line 7).
5. **Outside the repo:**
   - session and subagent transcripts under `~/.claude/projects/-home-sohambasanwar-sohamalloc/` (5.4 MB, 7.0 MB and 11.4 MB jsonl, among others), which hold review prompts and findings verbatim;
   - the `workflows/scripts/*.js` files that STATUS uses for resume.
   The `/tmp/claude-1000/…` scratchpad is volatile and was lost once already, in the 2026-10-21/22 reboot.

### 5.2 Components that could touch or export it

None of the evaluated components modifies or deletes evidence files on disk. The risks are copying, upload and misreporting.

| Component | Path to the evidence | State |
|---|---|---|
| claude-mem | PostToolUse `*` copies every Read, cat and grep output into plaintext SQLite and Chroma under `~/.claude-mem`, never purged; sends it to the observer; cloud sync to cmem.ai once paired; unauthenticated 127.0.0.1:37700 API. | Broken; remove. |
| claude-mem-cowork | POSTs prompts (4 KB) and tool I/O (16 KB per field, regex redaction) to cmem.ai if claude-mem is disabled **and** a key exists (including the `CLAUDE_MEM_CLOUD_SYNC_TOKEN` fallback). Spools to `~/.claude-mem/cowork-spool.jsonl`. | Inert; remove together with claude-mem. |
| headroom-ai MCP | Verbatim copies of compressed content in `~/.headroom/ccr_store.db`; beacon metadata. Remote Kompress (content off-box) only if `HEADROOM_KOMPRESS_ENDPOINT` is set. A compressed view could be quoted as fact. | Never used; remove. |
| Headroom skill | A digest written into the working tree could be committed. | Keep; digests in scratch only. |
| task-observer (A) | Backfill and "context preservation" copy or paraphrase evidence into the observation workspace; an anchoring mistake could place it inside the repo; the feedback flow could post to public GitHub. | Inactive; manual-only. |
| OmniRoute / FreeLLMAPI / any gateway | Every file read goes to the proxy, its logs (OmniRoute: 90 days, 512 KB per request), and third-party upstreams; FreeLLMAPI keyless and free tiers log or train. | Dormant / not installed; uninstall / never install. |
| claude.ai connectors and plugin MCPs | Gmail, Drive, Notion, Linear, Canva, Figma, Publora, spArxx, Firecrawl, claude-in-chrome, Slack, GitHub. | Enabled; no deny rules. |
| find-skills | Installs third-party skills with `npx … -y`. | Enabled. |
| Hermes agent | Configured for OpenAI Codex. | Not used on the repo. |
| Claude-written skills | claude-mem:handoff writes a HANDOFF.md; claude-memory:structured-memory writes `.claude/rules/`. | Removed with their plugins. |

### 5.3 Safeguards before enabling any memory, compression or routing tool
1. **Back up first.** Make an encrypted archive of items 5.1-2, 5.1-3 and the workflow scripts, in a private location under the user's control (not public pastes, not third-party clouds). Record `sha256sum` output in `docs/research-log/data/`. Find the B1–B4 raw data on the cloud VM; if it is gone, mark the benchmark table in STATUS and HANDOFF as having no raw data.
2. **Copy the two resume scripts into the repo**, for example `docs/research-log/workflows/`.
3. **Keep `ANTHROPIC_BASE_URL` and `ANTHROPIC_AUTH_TOKEN` unset** in `~/.claude/settings.json`, project settings and shell rc files. Any gateway breaks Remote Control and takes the request stream out of Anthropic's path.
4. **Use mechanical controls**, deny rules or `disableClaudeAiConnectors`, rather than prose in `autoMode.environment`.
5. **Before enabling any memory, compression or routing tool:**
   - Read its data flows from source.
   - Turn telemetry off before the first start.
   - Set its store outside the repo.
   - Exclude sohamalloc, or test it in a separate project for a day.
   - Never let a compressed or summarised view be the source of a quoted number; re-read the raw file by line range or compute the value with a script.
6. **Never delete or move evidence as part of tool cleanup.** Every DESTRUCTIVE step in section 3 is limited to tool state outside the repo (`~/.headroom`, `~/.claude-mem`, `~/.omniroute`, tool venvs and caches), and each starts with a listing.

---

## 6. Recommended target configuration (ordered)

Steps 1–4 come before the next Claude Code restart. The user makes every edit; an agent must not apply them.

1. **Evidence backup** (5.3 steps 1–2). No tool changes yet.
2. **claude-mem stack.** `claude plugin disable` for claude-mem-cowork, claude-mem and claude-memory (`-s user`) in one sitting. Optionally set `"enabled": false` in `~/.claude-mem/telemetry.json`. Then `claude plugin uninstall` all three, run `claude plugin marketplace remove thedotmack`, delete `extraKnownMarketplaces.thedotmack` from `~/.claude/settings.json`, and verify (3.4 step 5). Optionally, DESTRUCTIVE: `rm -rf ~/.claude-mem` after listing it.
3. **headroom-ai MCP.** `claude mcp remove headroom -s user`. Optionally, DESTRUCTIVE: `uv tool uninstall headroom-ai`, and `rm -rf ~/.headroom` after listing it.
4. **OmniRoute.** `nvm use 24 && npm uninstall -g omniroute`. Optionally, DESTRUCTIVE: `rm -rf ~/.omniroute ~/.config/configstore/update-notifier-omniroute.json` after saving app.log if wanted.
5. **`~/.claude/settings.json`** (user scope; `defaultMode: auto` stays here, since project and local settings cannot set auto):
   ```json
   {
     "skillOverrides": { "task-observer": "user-invocable-only" },
     "enabledPlugins": {
       "claude-code-setup@claude-plugins-official": false
     },
     "permissions": {
       "deny": [
         "mcp__claude_ai_Gmail__send_message",
         "mcp__claude_ai_Gmail__forward",
         "mcp__claude_ai_Gmail__create_draft",
         "mcp__claude_ai_Google_Drive__create_file",
         "mcp__claude_ai_Google_Drive__update_file",
         "mcp__claude_ai_Google_Drive__share_file",
         "mcp__claude_ai_Notion__notion-create-pages",
         "mcp__claude_ai_Notion__notion-update-page",
         "mcp__claude_ai_Notion__notion-create-file-upload",
         "mcp__claude_ai_Linear__create_attachment",
         "mcp__claude_ai_Canva__upload-asset-from-url",
         "mcp__claude_ai_Figma__upload_assets",
         "mcp__claude_ai_Publora__*",
         "mcp__claude_ai_spArxx_io_Zero-Knowledge_Blockchain_Timestamping__create_timestamp",
         "mcp__plugin_engineering_slack__*",
         "mcp__plugin_engineering_github__*",
         "Bash(gh gist *)",
         "Bash(npx skills add *)"
       ]
     }
   }
   ```
   Merge these keys into the existing file; do not replace it. The rule name forms follow code.claude.com/docs/en/permissions and the tool names as listed in this session. Bash deny patterns are best-effort, because the command can be rewritten. A PreToolUse hook that blocks uploads (for example `curl -F`, `--data-binary @`, `gh gist`) is the stronger mechanism. If the user also wants task-observer hidden from Remote Control command lists, use `"off"` instead of `"user-invocable-only"`. After the change, confirm in `/skills` (3.5).
6. **`/home/sohambasanwar/sohamalloc/.claude/settings.local.json`** (project scope, git-ignored). Add to the existing content:
   - `"engineering@synced": false` in `enabledPlugins` (keep the existing 12 entries);
   - `"syncClaudeAiSkills": false` (honoured from local scope per code.claude.com/docs/en/settings; removes about 2,100 tokens of account skills such as job-hunt, morning and pptx from this project);
   - optionally `"disableClaudeAiConnectors": true` (honoured from any scope). This removes the ~450 connector tool names and their instruction blocks, but also removes Consensus and Firecrawl for literature search, so it is the user's call. If it is set, the connector deny rules in step 5 become redundant for this project;
   - optionally `"syncClaudeAiPlugins": false`, which freezes account plugins for this project.
7. **No env or gateway keys.** Keep `ANTHROPIC_BASE_URL` and `ANTHROPIC_AUTH_TOKEN` out of every settings file and rc file. Keep `remoteControlAtStartup: true` at user scope as now. Never install FreeLLMAPI.
8. **Plugins and MCP servers that remain:**
   - clangd-lsp (keep);
   - headroom@synced (keep, digest rule in the sohamalloc-research skill or a CLAUDE.md);
   - claude-code-setup (disabled);
   - task-observer (manual-only);
   - user MCP servers: none;
   - claude.ai connectors: deny-listed for uploads, or disabled for the project.
9. **Commit-trailer enforcement.** Add a local `.git/hooks/commit-msg` (not versioned, so not pushed) that rejects AI trailers:
   ```sh
   #!/bin/sh
   if grep -qiE '^(Co-Authored-By|Claude-Session):' "$1"; then
     echo "commit-msg: AI trailer lines are not allowed in this repo" >&2
     exit 1
   fi
   ```
   Then `chmod +x .git/hooks/commit-msg`.
10. **Repo documentation.**
    - Rewrite docs/HANDOFF.md as in 4.3.
    - Fix the three STATUS lines.
    - Add the workflow scripts under `docs/research-log/workflows/`.
    - Add a research-log entry recording the AI tooling decisions: installed and removed (claude-mem stack, headroom-ai, OmniRoute), rejected (FreeLLMAPI), restricted (task-observer, claude-code-setup).
    - Commit as Soham Basanwar with no trailers, then push (per the memory rules).
11. **Optional:**
    - Export `compile_commands.json` from the build directory and add `.cache/` to `.gitignore` (clangd).
    - Review plugin auto-update for claude-plugins-official in the `/plugin` Marketplaces tab.
    - Decide whether find-skills (unattended `npx -y` installs) and git-commit (Conventional Commits style) should stay; neither was evaluated in depth.

---

## 7. Open questions and unverified items

### 7.1 Official documentation check (did not finish)
These points need confirmation from code.claude.com before anyone relies on them:
- **Opus 5.5 at "max" effort:**
  - how max effort is set and persisted (settings key, `/model`, or both);
  - whether Workflow subagents inherit the main session's effort level;
  - how the `maxEffortLevel` cap (it exists per code.claude.com/docs/en/settings) interacts with workflows.
  None of the retained tools changes effort; the Headroom proxy and gateways would, but they are being removed.
- **Ultracode subagents:**
  - whether every workflow subagent receives the full skill listing and the deferred-tool list (observed for this subagent and two fact-checkers; not documented);
  - whether SessionStart hooks fire per subagent (the hooks docs mention only tool events and SubagentStart/SubagentStop);
  - whether MCP stdio servers are spawned per subagent;
  - whether permission deny rules and `skillOverrides` apply identically inside subagents.
- **Remote Control:** incompatibility with `ANTHROPIC_BASE_URL` proxies and with `ANTHROPIC_AUTH_TOKEN` is quoted by two checkers from code.claude.com/docs/en/remote-control. Still open: how auto-mode classification and permission prompts behave in a remotely driven session, and whether hook-error notices are visible there.
- **Usage accounting:**
  - whether claude-mem's observer (Agent SDK, subscription OAuth) counts against the same 5-hour and weekly windows;
  - how cached context and deferred tool names count against plan limits;
  - whether subscription-OAuth background observer use is "ordinary, individual usage" under the legal page (a policy question).
- **Settings and CLI:**
  - how `autoUpdates=false` in `~/.claude.json` interacts with `autoUpdatesChannel` and `minimumVersion`;
  - whether the `settings.json` `env` block reaches MCP server processes;
  - whether a `Skill(...)` deny rule removes the listing entry;
  - whether `enabledPlugins: {"task-observer@skills-dir": false}` hides the root SKILL.md;
  - whether `skillOverrides` takes effect for task-observer (likely, since `claude plugin details` attributes its skill to the personal layer; confirm in `/skills`);
  - whether anthropic-plugin-directory plugins auto-update;
  - whether `defaultMode` values other than auto or bypass set in project local settings would override the user-scope auto mode for this project only.

### 7.2 Tool-specific items still unverified
- **Headroom:**
  - SmartCrusher's lossy row-drop trigger conditions;
  - server-side retention at the beacon receiver;
  - whether a newer release changes beacon defaults;
  - docs.headroomlabs.ai not read;
  - repo statistics and the quoted tagline.
- **Claude Code Setup:** per-run token total at max effort (only projected figures exist); whether disabling it removes the listing from subagents (expected, not tested).
- **Claude-Mem:**
  - whether the attempted PostHog POST arrived;
  - injection size with 50 observations;
  - whether the 15.0.1 history cap and quota guard bound cost;
  - cloud-sync payload contents;
  - #2315, #2643, #1742, #1826 and the DataCamp figure not re-fetched;
  - whether hook-error notices add model context on every call.
- **Task Observer:**
  - how often subagents obey the "invoke before the FIRST tool call" trigger;
  - whether the description is truncated by the 1% listing budget;
  - whether the skills CLI sends install telemetry;
  - the exact remove syntax in the skills CLI;
  - that the unread reference files match upstream (likely, given the matching hash).
- **OmniRoute:**
  - effort and thinking pass-through;
  - CVE-2026-49352 and the other cited GHSA IDs;
  - whether CVE-2026-88062 is fully fixed in 3.8.52 (the code shows the route-guard fix; sources disagree on the fixed version);
  - whether the dashboard password is still CHANGEME;
  - dashboard exposure of stored bodies;
  - radar payloads.
- **FreeLLMAPI:**
  - AI Horde's state on a fresh install;
  - relay operators' access, logging and resale rights;
  - whether analytics or logs keep bodies;
  - the "catalog never sees prompts" claim beyond catalog-sync.ts;
  - thinking and effort translation;
  - current data policies of the Mistral, OpenRouter :free, NVIDIA, Groq and Cerebras free tiers;
  - the 30-minute stickiness.
- **Configuration:**
  - B1–B4 raw data location;
  - exact token accounting for MCP instruction blocks (12–16k characters estimated, not counted);
  - find-skills and git-commit not evaluated.

### 7.3 Side effects of this audit
- The headroom fact-checker's offline run created `/tmp/data-gym-cache/fb374d419588a4632f3f557e76b4b70aebbca790` (public tiktoken vocabulary; no user content sent). It is outside the repo and is not evidence.
- No other change was made by the audit, the checks or this report. Read-only commands run for this report: `claude plugin --help` and its subcommand help, `claude plugin details` for 8 plugins, `claude mcp remove --help`, and directory listings.