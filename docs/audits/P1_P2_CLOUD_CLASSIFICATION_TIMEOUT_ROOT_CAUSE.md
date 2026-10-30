# P1/P2 cloud classification timeout: root-cause analysis and diagnostics

Date: 2026-10-30. Scope: Prompt 91. Labels: **Observed** means retained run/test evidence; **Source-derived** means implementation inspection; **Hypothesized** means an explanation requiring further evidence.

## Result and repository ownership

**Observed:** Two local diagnostic defects were reproduced and corrected: observation classification discarded typed provider-failure metadata before AgentLoop/store, and the HTTP transport discarded an already received HTTP status on deadline/cancellation. These defects explain missing evidence, not the remote latency itself. The milestone's diagnostic implementation and focused regression checks pass. Cloud Agent D/E acceptance remains incomplete.

**Observed:** Personal-Brain's `brain` command was unavailable; the explicitly supplied repository was used. AGENTS.md, architecture, Agent Runtime, security, building/testing guidance and Prompt 87–90 audit evidence were reviewed. Before changes, tracked source was clean; four existing untracked audit reports were preserved. Fetch showed main synchronized with origin/main (0 ahead/0 behind), base `87d762c4a053e7211a9f019b6591fa78edeb8631`. The only worktree was this checkout; no other active Sentinel editing chat or index lock was found. Changes are on dedicated branch `fix/p1-cloud-agent-gateway-reliability`, uncommitted. No push, merge, reset, stash, clean or rebase was performed.

## Actual production execution path

**Source-derived:** Desktop Agent submission → typed daemon IPC → ApplicationController → LlmAgentRuntime/AgentRuntime configuration → AgentLoop initialization → ObservationIntentPolicy classification → frozen IModelRouter ModelBinding and IChatProvider → ContextBudgetManager admission → SelectedEndpointChatProvider::sendRequest → LocalInference/OpenAiCompatibleProtocol request construction → HTTP POST and ProviderRequestRuntime wait/deadline/cancellation → typed provider reply → classification JSON/contract interpretation → Agent planning only after determinate classification. Filesystem/process execution subsequently requires the registry, ToolExecutionGateway and existing permission boundaries.

**Source-derived:** Classification runs once when a new AgentLoop run is initialized. It does not run again for each tool step or normal approval resume. A fresh run with the same text intentionally classifies again. There are at most two classifier requests: only typed output truncation permits a changed-output-budget retry, initially 2048 then at most 4096 and bounded by the model output ceiling. Invalid complete JSON, timeout and cancellation do not trigger identical classifier retries. Provider transport attempts are a distinct counter. Existing provider-recovery counters do not prove classifier retry counts.

**Source-derived:** Even a simple compile request takes this semantic inference path. Classification establishes required observation domains, resources, capabilities and evidence/completion obligations; existing concrete-resource guards supplement it. Replacing it with keyword-to-command planning would weaken guarantees. No such shortcut was introduced.

## Historical comparison

**Observed:** Existing configuration is provider `cloud-api`, selected model `deepseek-v4.1-flash`, custom gateway origin `https://evren-llmapi.ssyz.org.tr`, base `/v1`, discovery `/v1/models`, inference `/v1/chat/completions`. These are public configuration identities, not credential-bearing URLs. An `openai` credential slot does not establish the credential issuer or request destination. UI/daemon credential presence and host metadata checks were positive in prior audits; sandbox-only Keychain lookup failure was a visibility mismatch and does not establish a failed save.

All timestamps below are UTC on 2026-10-29. Durations are whole-run wall time, not gateway timings.

| Case | Core run | Start → end | Duration | Outcome |
|---|---|---|---:|---|
| Original C | `d55d4bdc-1d57-41ef-8567-ece8968fc3e6` | 22:35:39.375 → 22:36:39.736 | 60,361 ms | Classification timeout; zero tools |
| Recovery C | `69a871df-f4dd-48fe-a22a-0459040d4daf` | 22:45:49.125 → 22:47:57.690 | 128,565 ms | Read → edit → readback; accepted Completed |
| First D | `07b79e4d-7744-4fe2-b39e-deaf62c5536b` | 22:48:45.500 → 22:50:37.735 | 112,235 ms | Classification timeout; zero tools |
| Shorter D recovery | `5278464c-f906-4e1a-95bd-2b3920bb1932` | 22:52:17.304 → 22:53:17.734 | 60,430 ms | Classification timeout; zero tools |

**Observed:** Failed runs terminate Failed/ObservationIndeterminate (“Completion Indeterminate”), not successful completion. Historical provider category, HTTP status and attempt columns are NULL. Each displayed final request reported a 60,000 ms timeout. Both D runs stopped before compiler/execution dispatch; no `smoke-result` executable exists. The successful C used four planning phases and changed the disposable fixture's `a - b` to `a + b`, verified by readback. Its context estimate was 4218; this is not actual token usage. Historical A and B also passed. Successful C on unchanged runtime code argues against a deterministic filesystem/compiler defect as the cause of these pre-tool failures.

**Source-derived:** C and D share classifier context construction, bounded schema-shaped JSON response, request mode and local deadline. Context is admitted before provider invocation. The classification prompt includes the goal, bounded context, available capability/tool identifiers and standard directories. Full historical classifier prompt/request byte counts were not retained, so exact C/D size comparisons cannot be recovered honestly. Shortening D did not eliminate the observed failure. New diagnostics record lengths without contents.

**Source-derived:** Classifier requests ReasoningControl::Disabled. The generic cloud protocol intentionally does not emit an undocumented model-specific reasoning field. This is not proof that this gateway/model disables hidden reasoning. Discovery uses a 4-second request; controller polling is 5 seconds with no overlapping controller discovery worker. Daemon catalog refresh is 60 seconds and can cancel/requeue discovery. Discovery and inference can overlap; they are separate request paths. Cached successful catalogs can become Stale; cold discovery failures have different admission consequences. No catalog failure was correlated with these classification timeouts.

## Verified timeout behavior and evidence limits

**Source-derived:** The selected cloud binding requests a 60,000 ms classifier deadline. ProviderRequestRuntime checks cancellation/deadline and aborts the local pending network reply. Local abort does not prove cancellation of already executing upstream inference. HTTP 0 means no status was observed, not that dispatch failed. Invoking the provider is not proof of socket dispatch or provider receipt. A local request UUID is not a gateway receipt ID.

**Observed:** Retained run boundaries and error text establish local timeout outcomes. They cannot split DNS/TLS/connect time, gateway queue time, upstream inference time, response transfer, or local event-loop scheduling. Desktop's inspected log was empty; daemon stdout/stderr were directed to null. No correlated gateway/admin timing, response, usage or billing receipts were available. No historical HTTP status or usage was fabricated from the error text. Earlier Chat/C successes prove connectivity for those requests only.

| Candidate | Assessment |
|---|---|
| Gateway queue/upstream latency, possibly hidden reasoning | **Hypothesized, highest investigative priority:** intermittent success and near-deadline failures are compatible; no evidence splits queueing from inference or proves either. |
| Truncation followed by changed-budget retry | **Hypothesized:** source permits this and could explain the 112-second D run ending in a 60-second request. Historical attempt/truncation evidence is absent; not a finding. |
| Network/TLS, concurrent discovery, local scheduling | **Hypothesized:** compatible with missing transport evidence; no correlated proof or reason to rank one conclusively. |
| Context admission or response parsing | **Observed/Source-derived:** distinct failure paths exist, but retained failures report timeout rather than budget refusal or invalid JSON. These are not established causes of those runs. |
| User cancellation or authentication/model error | **Observed:** no supporting typed historical evidence. Timeout text alone cannot establish these categories. |
| ProcessExecutor/filesystem | **Observed:** no D tool dispatch occurred. These components were not reached and are not implicated by these failures. |

## Confirmed diagnostic corrections

**Observed:** A regression test first failed because `intent.providerFailure` was absent after a typed provider timeout (`/tmp/sentinel-p91-red-test.log`). The corrected production runtime integration now preserves Timeout, HTTP 200 already received by the fake transport, TimedOut lifecycle and request identity through AgentLoop into SQLite, while retaining Failed/ObservationIndeterminate and zero tool calls.

**Source-derived:** The focused change adds:

- A local classification trace UUID shared across at most two attempts; validated UUID request IDs, UTC start, elapsed/deadline, attempt and provider-attempt counters, stage, outcome/lifecycle, cancellation/timeout/truncation flags, output budget and prompt-byte/input-estimate metadata.
- Numeric usage only when returned; missing usage remains absent. Provider/model identities remain the existing parent-run binding fields, avoiding copies of arbitrary provider strings.
- Typed provider failure propagation in both synchronous/asynchronous AgentLoop paths. Existing provider failure columns now receive classification failures.
- Preservation of an already observed HTTP status on transport timeout/cancellation. This is not evidence that the provider finished responding.
- A strict shared sanitizer used before persistence/logging. It accepts only fixed enum/boolean fields, UUIDs, valid normalized times and finite nonnegative bounded integer counters. It inspects at most two frames. No arbitrary diagnostic strings, prompts, private source, headers, output or URLs enter this contract.
- Additive nullable `classification_diagnostics` in run-store schema 8 (previously 7), storing attempts and enum classification result. No IPC schema or configuration/credential changes. Existing older binaries reject a newer schema; this candidate is not a transparent downgrade once a profile is migrated.
- Retention on initialization and diagnostic writes: clear diagnostics for finished runs older than 30 days by start time or outside the newest 256 runs. Active rows are retained; run history/evidence is not deleted by this retention. Existing history/prune controls continue to work.

**Source-derived:** Info logging remains opt-in under `sentinel.classification.diagnostics`; safe run-store diagnostics do not depend on enabling verbose logging. `provider_invoked` is expressly not named “HTTP dispatched”. `upstream_cancellation=unconfirmed` never asserts remote cancellation. A process crash before return may leave only an enabled entry log, rather than a completed persisted attempt; gateway correlation remains necessary for remote root cause.

Changed production files:

- `core/include/sentinel/core/agent/ClassificationDiagnostics.h` (new sanitizer)
- `core/include/sentinel/core/agent/ObservationEvidence.h`
- `core/include/sentinel/core/agent/IAgentRunStore.h`
- `core/src/agent/ObservationPolicy.cpp`
- `core/src/agent/AgentLoop.cpp`
- `core/src/agent/SQLiteAgentRunStore.cpp`
- `core/src/runtime/LocalInference.cpp`

Regression files: `tests/core/test_observation_policy.cpp`, `tests/core/test_agent_loop.cpp`, `tests/core/test_agent_runtime.cpp`.

## Deterministic verification and Release provenance

**Observed:** Existing `build/release` was reused for incremental builds of Desktop, daemon and focused test targets. No unrelated full suite or new build tree was used. Final CTest run: **8/8 targets passed**, 17.35 seconds:

| Target | QtTest cases passed | Existing skips |
|---|---:|---:|
| test_provider_contracts | 205 | 0 |
| test_native_provider_contract | 78 | 0 |
| test_local_inference | 53 | 0 |
| test_agent_loop | 80 | 0 |
| test_agent_runtime | 29 | 4 macOS strict fork/shell fixtures |
| test_observation_policy | 73 | 1 explicit opt-in live probe |
| test_desktop_daemon_e2e | CTest passed | — |
| test_desktop_shell_launch | CTest passed | — |

**Observed:** Injected real endpoint-adapter/fake-network cases cover successful classification, deadline, cancellation, TLS transport error, missing usage, malformed response and private diagnostic markers. Additional checks cover bounded truncation retry, budget refusal without provider invocation, typed terminal failure in sync/async loops, no planner/tool dispatch, production runtime/store propagation, schema 7→8 migration, history preservation and diagnostic retention. Initial integration-fixture failure was corrected by implementing its bounded `sendRequest` contract; production behavior was not changed to accommodate the fake.

**Observed:** Opt-in synthetic logging captured a Timeout/HTTP 200 frame with the production policy's requested deadline 60000, elapsed 24 ms and upstream cancellation unconfirmed. The fixture deliberately capped transport waiting at 25 ms; this is not a live gateway timing. No PRIVATE marker appeared; absent token usage stayed absent. Logs: `/tmp/sentinel-p91-synthetic-trace.log`, `/tmp/sentinel-p91-final-tests.log`. IPC and Desktop generators passed `--check`; personal-path scan flagged zero files; `git diff --check` passed. Changed-line formatting used available clang-format 23.1.4; CI-pinned 18 was unavailable and was not installed.

**Observed:** Release candidate version remains `1.0.0-rc.8`; embedded commit alone names the base and cannot identify these uncommitted changes. SHA-256:

| Artifact | SHA-256 |
|---|---|
| `build/release/apps/sentinel-daemon/sentinel-daemon` | `1faff8c5da18109e5d564d439e9e24f7ab8ac9b1eb7748b74f5293b0e9ab27ec` |
| `build/release/apps/sentinel-desktop/sentinel-desktop.app/Contents/MacOS/sentinel-desktop` | `addf3f3cefc73dbb134ce7ee4fe551373b62570d84ff98a86005883f36383a17` |
| Source/test manifest (ten changed C++ files, sorted path→content-SHA JSON) | `8a8452533cacc418a31d6322cf0f51db431548bb81942a0b831c2c3967e69169` |

Manifest: `/tmp/sentinel-p91-source-manifest.json`. Source manifest excludes audit documents and build artifacts.

**Observed:** The previously inspected Desktop PID 87372 and daemon PID 87393 used the expected Release paths/user profile, server generation `76530e88-29dd-4678-815a-df49cd7a1206`. They were not restarted; their already loaded code predates this rebuild. Rebuilding files does not upgrade a running process. No production profile was migrated by these isolated store tests. No new live gateway request was made in Prompt 91, and successful A/B/C were not repeated. A D request against that old loaded daemon would not validate these diagnostics; without gateway receipts it would add limited causal evidence. Optional live D was therefore omitted.

## Recommended correction and acceptance

**Source-derived recommendation:** Keep current deadlines and semantic/security gates. Before one controlled D, deliberately load the rebuilt daemon through the normal application lifecycle, verify its artifact/profile/server generation, and arrange sanitized gateway correlation for the test's UTC window. Use the synthetic workspace only; retain normal command approval. Compare per-attempt elapsed, status, lifecycle, usage/truncation and classifier result with gateway queue/upstream timings. Local UUIDs are not automatically propagated gateway trace IDs. Never request raw keys, headers, prompts or source as diagnostic evidence.

**Hypothesized optimization, not implemented:** If returned usage/finish reasons demonstrate hidden reasoning or truncation, investigate a documented gateway/model-supported non-reasoning structured-output configuration or a separately validated cheaper classifier binding. Require equivalent semantic contract/evidence tests, advertised capabilities and context admission before adopting it. Do not send guessed reasoning parameters or infer commands from natural language. No current evidence justifies a longer deadline, bypass, model capability expansion or hardcoded gateway workaround. The existing changed-output-budget retry also deserves a separate context-admission regression review; this patch leaves its policy unchanged.

**Observed acceptance:** A Chat passed; B read passed; C edit/readback passed historically. D compiler/execution still not accepted, E full task blocked. Prompt 91 diagnostics and confirmed-local-defect regressions pass; remote latency root cause is unresolved pending correlated evidence. Existing session authorization/daily allowance was respected without additional billable calls.

**Observed references:** Prior local audits: `P1_CLOUD_AGENT_RELEASE_SMOKE_RESULT.md`, `P1_CLOUD_AGENT_CREDENTIAL_DIAGNOSTIC_RESULT.md`, `P1_CLOUD_AGENT_LIVE_TEST_A_RESULT.md`, `P1_P2_CLOUD_GATEWAY_AGENT_E2E_RESULT.md`, `P1_P2_CLOUD_AGENT_TIMEOUT_RECOVERY_RESULT.md`. Acceptance tracking remains P1-02 / AC-P1-02 / AC-P2-03. Existing #94/#95 references concern other work and are not claimed as the gateway root-cause issue; no GitHub issue was created or modified in this task.
