# Prompt 94 — Agent deadline safety, context re-admission and provider parity

Date: 2026-10-30, Europe/Istanbul. Work items **AT-02 / TD-02**. **Observed** identifies executed/repository evidence; **Source-derived** identifies inspected implementation; **Proposed/Hypothesized** identifies unimplemented work or an unproven cause. This report follows ENGINEERING_STANDARD.md; it does not close cloud D/E or the wider phases.

## Result

**Observed:** Bounded Agent classification and each planner decision now own one monotonic parent budget. Child requests, planner recovery/repair, serialization wait and transport waits consume remaining time rather than resetting it. Changed classifier output reserves receive fresh ContextBudgetManager admission. Expired/cancelled operations do not admit another request, and late success cannot become an accepted decision. Local zero/configured timeout compatibility is preserved.

**Observed:** Safe transport frames now survive OpenAI-compatible native completions, Claude/Gemini native completions, and supported compatible/Ollama/Claude/Gemini streaming result adapters, including failures. Missing status/usage remains unknown. Schema stays 8. Focused fake-clock, injected transport and sync/async no-dispatch regressions pass; actual user runtime/profile were preserved. No live billable request was made.

## Repository, ownership and preserved work

**Observed:** Existing checkout and branch `fix/p1-cloud-agent-gateway-reliability`, base `87d762c4a053e7211a9f019b6591fa78edeb8631`. Entry contained **27** known dirty/untracked paths from P91–93. `/tmp/sentinel-p94-entry-manifest.json` captured their hashes; the prior P93 C++ source manifest matched. All **seven** existing audit reports retained identical contents. Existing governance/AGENTS/contributor rules were retained. Only the requested source seams and affected P1/P2/P8 quality findings were extended; no competing worktree, reset, clean, stash, rebase, commit, push or merge.

**Observed:** App chat inventory showed only this active Codex chat in the Sentinel checkout; no conflicting source owner or unexpected source change was found. `brain` was unavailable, so the explicitly supplied repository was used. AGENTS, engineering/timeout/matrix/P93 documents and relevant architecture, Agent, provider, context, cancellation, storage, security, build and testing contracts were reviewed. GitHub open-issue metadata was queried read only into `/tmp/sentinel-p94-open-issues.json`; no remote objects were changed.

## Evidence-based findings and corrections

| Operating condition → failure consequence | Actual evidence | Mitigation | Acceptance oracle |
|---|---|---|---|
| Classifier first request truncates late → second request can restart the full 60 s and exceed the intended operation | **Source-derived:** old loop reused the same deadline option per call. **Observed:** fake clock at 59000/60000/60001 exercises remaining/exact/expired boundaries | One parent starts before admission; second child receives remaining time; check before and after provider return | 59000 ms leaves exactly 1000 ms; at/after 60000 only one request; late valid JSON produces Timeout/indeterminate |
| Planner repair or transient recovery → new 120 s allocation/backoff can reset time | **Source-derived:** per-attempt options and recovery sends used a fresh relative timeout | One parent per nextStep decision; repairs/recovery/backoff share it; post-return fence rejects late finals/tool proposals | Repair gets remaining 1000 ms; expiry during backoff admits no recovery; cancellation and late success are GiveUp with typed failure |
| Serialized provider is busy → blocking mutex wait cannot observe cancellation | **Source-derived:** blocking lock_guard at the provider serialization boundary | Existing mutex retained; try-lock with cancellable, parent-bounded 10 ms backoff, guard again before send | Reentrant two-clone fixture holds mutex, cancels waiting clone, returns under 1000 ms with one total provider invocation |
| First classifier reserve 2048 admitted; retry raises to 4096 → initial context admission no longer applies | **Source-derived:** old retry changed maxOutputTokens without another assemble | Re-run authoritative ContextBudgetManager for changed reserve against frozen binding/effective capacity; refusal is indeterminate with no second dispatch | Known small/exact capacity, unknown fallback, unknown output ceiling and known cap scenarios; refusal blocks second send |
| Already-cancelled oversized request → budget refusal can obscure cancellation | **Observed regression scenario:** cancellation with 100000 synthetic characters, zero provider calls | Stop checks before/after admission; cancellation wins over expiry/refusal | Cancelled, provider attempts 0; not a fabricated budget failure |
| Native completion structs and streaming adapters drop safe lifecycle/usage metadata | **Source-derived:** completion struct lacked transport frames; adapters rebuilt diagnostics rather than carrying result diagnostics | Append optional bounded frames to existing result contract; copy normalized diagnostics/status/attempts through both success/error returns | Three native completion dialects and six streaming paths retain safe frames on success, metadata/body stall, TLS error and cancellation |

**Observed, additional reproduced defect:** Claude `max_tokens` and Gemini `MAX_TOKENS` streaming stop reasons previously yielded reply.success=true for incomplete output. Both deterministic red rows failed before the correction (`/tmp/sentinel-p94-truncation-red-test.log`). A narrow stop-reason mapping now marks output truncated/RequestRejected; all six backend truncation rows pass, with completed HTTP transport still distinguished from rejected model completion. This is a verified correction within the requested no-false-success boundary, not a speculative gateway fix.

**Source-derived/Observed:** Usage-only SSE frames (including choices=[]), Claude message_start/message_delta usage, Gemini usageMetadata and Ollama reported eval counters are merged through the shared normalizer. Partial-body timeout fixtures retain supplied counts. Missing/invalid/string/fractional counts stay unknown; actual zero stays zero, and no total is synthesized. The previous generic numeric normalizer cast fractional counts to integers; the scoped strict-integer regression now prevents that.

**Observed:** These are local reliability/contract corrections. They do not explain the gateway's historical P92 no-status delay. No increased timeout, undocumented reasoning field, classifier bypass, model-capability expansion, alternate router/runtime or tool-authorization change was introduced.

## Actual parent/child contract

**Source-derived:** `OperationDeadline` uses steady_clock milliseconds, immutable start/limit and an optional injected monotonic clock. A shared owner is carried by ChatRequestOptions → LocalInferenceOptions/native completion options → transport. Zero means the existing unbounded-local policy, never “expired but sent as timeout 0”. Remaining time is clamped nonnegative; callers check expiry before dispatch so an exhausted budget cannot become an unbounded request.

- Classification owns a parent for the **classification operation**, including initial admission and its single changed-budget retry. Cloud/unknown initial budget remains 60000 ms.
- LlmAgentRuntime owns a parent for **one planning decision**, including context assembly, serialization, transient recovery/backoff and bounded protocol repair. Cloud/unknown initial budget remains 120000 ms. This is not a new wall-clock deadline for the entire multi-step Agent run.
- Every child send recomputes its allocation from that same parent. Supported production adapters carry the owner through request preparation; POST checks again after manager creation/body serialization and before network handoff. Transport wait uses the smaller child/parent remainder.
- Deadline-bound JSON keeps the existing one-transport-attempt policy. Existing unbounded-local transient retry behavior/count remains; it does not acquire an implicit cloud cap. Planner retries remain bounded and do not retry after emitted stream output under the existing transient policy.
- Transient backoff is bounded by remaining time and cancellation; it cannot extend the parent. Serialization polls cancellation/expiry without inventing permission to use a busy provider.
- Cancellation wins when cancellation and expiry coincide. A returned reply is checked again before parsing/acceptance; observed HTTP/status/usage/transport evidence is retained while late text/tool proposals are discarded. The legacy default provider rejects an unsupported parent-only bounded contract rather than silently using unbounded sendMessage.

**Source-derived limit:** Qt/OS scheduling is not hard real time, and arbitrary non-cooperative third-party code cannot be forcibly interrupted in-process. The enforced contract is bounded allocations/transport abort, no newly admitted send after observed expiry, and no late accepted decision. Physical worker join/remote cancellation is a separate contract; neither is claimed by this patch. Supported synchronous provider implementations finish callbacks before returning; existing runtime generation/terminal fences remain authoritative. No new asynchronous execution authority was added.

**Observed:** Tests cover before/at/after expiry, two children, retry/backoff expiry, serialized cancellation, cancellation at the deadline and late valid JSON. Actual classification failure/re-admission paths were run through AgentLoop in both sync/async modes: **one provider call, zero planner calls, zero tool dispatch, Failed/ObservationIndeterminate**, with one async terminal callback. Existing no-duplicate/late-event/permission regressions remain passing.

## Context re-admission

**Source-derived:** Initial and changed classifier budgets use the same ContextBudgetManager, frozen ModelBinding and unchanged prompt/security contract. Effective capacity is the binding's validated context capacity or existing 8192 unknown-capacity policy fallback; estimates remain estimates, not actual tokenizer usage. Existing model output ceilings still apply. A known ceiling that leaves no room to grow stops as truncation without a redundant retry.

**Observed:** Deterministic cases include:

- First reserve admitted, known effective capacity insufficient for the larger reserve → BudgetRefused, one invocation, nondispatch retry diagnostic.
- Known capacity exactly equal to estimated admitted input plus 4096 reserve → second request admitted at 4096.
- Known small context permitting only the first reserve; known output cap preventing growth.
- Unknown capacity with large synthetic input → initial 2048 reserve fits the existing fallback, 4096 does not; second dispatch blocked.
- Unknown capacity with small input and unknown output cap → safe changed-budget retry remains allowed.
- Truncation without token counts → no actual usage is invented.
- Cancellation or parent expiry before retry, and cancellation before an oversized admission → no second/initial dispatch as appropriate.

A budget refusal remains indeterminate; it cannot finalize a task or authorize an operation. No context/security/output bound was weakened and ContextBudgetManager implementation was not duplicated or replaced.

## TD-02 native/streaming parity

**Source-derived:** The P93 sanitizer/observer remains the single phase contract. Native OpenAiCompletionResult now appends bounded sanitized transport frames for success and failure. SelectedEndpointChatProvider preserves them through OpenAI-compatible native, Claude and Gemini parsing/error returns. Compatible and Ollama streaming results capture transport diagnostics before timeout/cancel/error early returns. Their adapters copy safe normalized diagnostics, actual status/request identity/attempts and optional usage instead of replacing them with a small new object. Shared preserveReportedUsage handles valid provider-native usage shapes even without visible text/choices; it merges observations rather than overwriting them with absent fields. Usage received before a stream stops is retained; absent usage stays absent.

| Supported path | Deterministic result evidence |
|---|---|
| OpenAI-compatible JSON inference/classifier | Existing P93 capture retained, shared parent now reaches the same transport |
| Compatible native/structured completion | Native success, timeout, TLS failure, malformed body, expiry/cancel during manager creation all preserve frames; no POST after dispatch-boundary stop |
| Claude Messages native | Same native matrix through actual adapter/builders with injected transport |
| Gemini generateContent native | Same native matrix through actual adapter/builders with injected transport |
| Cloud compatible stream | Success, partial body stall, no-body timeout, cancel, TLS, pre-expiry matrix |
| LM Studio / llama.cpp compatible streams | Same matrix, no network contacts; local timeout defaults retained |
| Ollama stream | Same matrix via the existing network-factory seam added to OllamaConfig; production empty factory still uses Qt network and unchanged network policy |
| Claude / Gemini streams | Supported parser/adapter paths retain lifecycle frames on the same six scenarios; no claim of extra native capabilities |

**Observed:** **48** streaming cases and **21** native completion cases passed. A malformed model body can have `response_completed=true` for HTTP transport while model success remains false. A stalled partial body has `partial_body=true`, transport completion false and no retry; HTTP 200 remains observable when received. A TLS failure leaves HTTP status unknown. Expiry/cancellation during manager creation yields zero POSTs. Unknown DNS/TCP/reuse fields stay null/unknown; local UUIDs are not gateway IDs.

**Scope limit:** This adds **result-contract parity**, not a new durable history schema for every planner/discovery request. Classification retains its existing schema-8 persistence; native/stream replies carry safe metadata to existing consumers/logging. A broader planner/discovery store/projection requires a separately scoped retention/consumer contract. No unsupported API milestone, confirmed remote cancellation or complete provider acceptance is invented.

## Privacy, durability and platform review

**Observed:** Existing secret/redaction and bounded retention tests pass with the extended results. Synthetic PRIVATE prompt/key/error/body markers do not enter diagnostic JSON. Returned actual usage is preserved; missing usage stays absent, not zero-filled. Classification adds boolean parent-expired/cancelled flags; request IDs in persisted failure metadata remain UUID-validated. No raw body/header/URL/file content logging, credential changes, global verbose tracing, profile export or network policy bypass.

**Source-derived:** Storage remains schema **8** with the existing optional JSON and at most two classifier frames, each at most three HTTP frames. Existing 30-day/newest-256 finished-run diagnostic retention, history preservation, writer admission and migration/older-reader refusal remain unchanged. Operation clocks/pointers are in-process types, never serialized to IPC/settings/SQLite. No client-server wire contract or QML types changed; generated contract checks pass.

**Platform review:** New timing uses standard C++20 steady_clock and Qt request/event APIs, with no macOS-only core branch. Qt signal availability/reuse limitations stay explicitly unknown. macOS arm64/Qt 6.12.0 host and controlled fixtures were executed. Linux/KDE/Wayland and Windows native execution were unavailable, so native platform acceptance is **not claimed**. Existing four macOS shell/fork fixture skips remain. There was no presentation/layout change; loading/focus/responsive redesign is not applicable. Isolated Desktop/daemon smoke verifies affected binary composition, not every native UX cell.

## Focused verification

**Observed:** Existing Release tree reused, incremental Desktop/daemon/test builds; no fresh tree or unrelated full suite. Final command selected ten targets: Desktop daemon E2E/shell launch, context budget, provider contracts, native provider, local inference, AgentLoop, AgentRuntime, LlmAgentRuntime and ObservationPolicy.

Final result: **10/10 CTest targets passed in 23.33 seconds** (exact QtTest counts are recorded in `/tmp/sentinel-p94-verified-final-tests.log` / CTest LastTest.log).

| Target | QtTest passed | Existing skips |
|---|---:|---:|
| context_budget_manager | 10 | 0 |
| provider_contracts | 254 | 0 |
| native_provider_contract | 99 | 0 |
| local_inference | 53 | 0 |
| agent_loop | 84 | 0 |
| agent_runtime | 29 | 4 macOS strict shell/fork fixtures |
| llm_agent_runtime | 79 | 0 |
| observation_policy | 96 | 1 explicit opt-in live model probe |
| desktop_daemon_e2e / desktop_shell_launch | Both CTest passed | — |

**Observed:** Intermediate classifier fake-clock/context matrix: 14 passed; planner fake-clock/boundary matrix: 9 passed; stream parity: 38 passed; native parity: 20 passed (those totals include test init/cleanup). Later serialization/default-provider/sync-async/pre-cancel cases are included in final targets. First build found a local variable-name collision (`payload`) introduced during request serialization guarding; it was renamed, with the original failing log retained at `/tmp/sentinel-p94-build.log`. Subsequent focused builds/tests passed. No product failure was suppressed or test disabled. Existing pre-cancel tests were updated to require **zero dispatch**, and deadline assertions now accept the legitimate remaining allocation instead of requiring a fresh full duration.

Final build log `/tmp/sentinel-p94-complete-final-build.log`; final usage/truncation focused logs `/tmp/sentinel-p94-stream-final-tests.log` (**51 passed**) and `/tmp/sentinel-p94-native-final-tests.log` (**23 passed**, including init/cleanup); focused first run `/tmp/sentinel-p94-focused-tests-1.log`; classifier/planner/native/stream first logs have distinct `p94-*-tests-1.log` names. Formatting used installed clang-format 23.1.4 on changed lines; CI-pinned 18 was not installed. IPC/Desktop generators passed `--check`; whitespace, personal-path and documentation-link checks were run.

## Release provenance and preserved runtime

**Observed:** Candidate version stays `1.0.0-rc.8`; embedded commit identifies the base, not these uncommitted sources. Final SHA-256:

| Artifact | SHA-256 |
|---|---|
| `build/release/apps/sentinel-daemon/sentinel-daemon` | `9ca00a31296657386e501ab37376479799c385ec88567d6fcd927b4d38239d10` |
| `build/release/apps/sentinel-desktop/sentinel-desktop.app/Contents/MacOS/sentinel-desktop` | `1db285f07cf04ab9d49a6d54a69c85b682119b1362e49f9c49f8a258e5843f12` |
| Sorted changed C++ path→content SHA manifest digest | `9e68f9525bffd087cf55042ccceabde9368956062059b8201fd907183c70a3da` |

Manifest: `/tmp/sentinel-p94-source-manifest.json`; audit/docs excluded from its C++ digest. Desktop/daemon were rebuilt because internal request/result contracts changed; builds alone did not activate the user runtime.

**Observed:** User Desktop PID **22048**, daemon PID **22051**, generation `0cbf1c45-a7f5-40cf-bd15-b4f91766b463`, protocol 1.1, remain unchanged and still load the earlier P91/P92 code. Selected binding remains cloud-api / deepseek-v4.1-flash; active_runs **0**, production schema **8**, **82** runs, quick_check **ok**. No restart, competing daemon, profile migration or live Agent request. New candidate remains **built/tested, not activated** in that process. P93 disk hashes are historical, superseded by the table above; prior reports were not rewritten.

## AT-03 readiness decision

**Implemented/verified:** Common bounded parent/remaining-time ownership, changed-reserve admission, cancellation/late-result fences and supported native/stream result metadata now provide a deterministic foundation. **Ready for a bounded design/prototype slice with fake clocks; not ready to enable broad adaptive defaults.** No numerical adaptive profile was calibrated or adopted here.

| Condition/backend | Ready foundation | Remaining gate before adaptive operation defaults |
|---|---|---|
| Cloud compatible | Parent total bound, network phase/status, partial vs complete | Define initial-response and useful-progress/idle semantics; authorized correlated queue/inference receipts and model/provider profile measurements |
| Ollama | NDJSON parsed-output/terminal data, cancellation, same parent/result metadata | Cold/warm native model timing and current explicit timeout preference migration; no new 60 s local cap |
| llama.cpp / LM Studio | Compatible stream parsers, configured request bounds, unbounded-local parent preserved | Low-performance hardware and cold model calibration; protocol-native useful progress, not socket traffic/utilization |
| Claude/Gemini native | Result diagnostic parity and bounded completion/streaming paths | Document useful visible/tool-call progress separately from reasoning, usage/heartbeat metadata and partial structured responses; verify advertised capabilities/live parity |
| Slow/low-memory hardware | No global local deadline reduction; remaining-time mechanism can carry a future explicit extension | Measured ENV/BE/BUD profiles, memory/lease protection, missing telemetry handling, explicit finite extended profile with working Stop |
| Model cold start | Existing lifecycle ownership retained; no automatic wait expansion | P3 physical readiness/leases and worker drain evidence. UI idle/semantic final is not physical quiescence |
| User cancellation | Stop before/after admission, during wait/backoff/serialization, late-output fence tested | Native latency/teardown and non-cooperative process isolation/physical-stop acceptance remain separate |

Initial response, semantic idle and total clocks must remain distinct; network activity, arbitrary SSE comments or CPU/GPU use cannot reset useful-progress time, and useful progress cannot reset the parent total. A future user extension needs an explicit reviewed operation/configuration contract, not repeated child-timeout resets. Existing local zero timeout and explicitly configured provider timeout behavior remain intact pending that migration.

## Files and affected quality findings

Production: new `core/include/sentinel/core/runtime/OperationDeadline.h`; IChatProvider, ObservationPolicy, LlmAgentRuntime, ProviderRequestRuntime and LocalInference headers/implementations; OllamaConfig/OllamaChatProvider; SelectedEndpointChatProvider; established ClassificationDiagnostics sanitizer. ContextBudgetManager and ToolExecutionGateway authority were reused, not modified/replaced.

Tests: `test_observation_policy.cpp`, `test_llm_agent_runtime.cpp`, `test_agent_loop.cpp`, `test_agent_runtime.cpp`, `test_provider_contracts.cpp`, `test_native_provider_contract.cpp`, and ScriptedProviderTransport's manager-creation injection hook. ProviderCapabilityNormalizer header/implementation now merge strictly numeric reported usage without inventing totals. Docs: Agent Runtime contract, a scoped implementation note in ADAPTIVE_TIMEOUT_DESIGN, affected P1/P2/P8 rows and AT-02/TD-02 follow-ups in MODULE_QUALITY_MATRIX, and this report. Other P0–P12 rows retain their declared inspection scope.

| Classification | Finding / closure criterion |
|---|---|
| Implemented and verified | AT-02 bounded operation/retry/admission safety and TD-02 supported result-path parity; deterministic oracles above |
| Confirmed but deferred | Local unbounded/default wait policy is intentionally unchanged; AT-03 requires calibrated initial/idle/total design. Planner/discovery durable transport history is not added; needs explicit retention/consumer tests. Non-cooperative physical stop/model lease safety needs P3/P5 acceptance |
| Hypothesis requiring evidence | Gateway queueing/inference/TLS/local scheduling explanation for P92; no correlated upstream receipts available |
| Not applicable | New UI layout, credentials/grants, schema migration, packaging/distribution policy, automation authority and unrelated phase implementation |

**Observed mapping:** AT-02/TD-02 are local work keys, not new GitHub issues. Read-only open snapshot confirms related #29 (live corpus), #37/#38 (context policy/recovery), #42 (provider parity), #43 (model ownership), #54 (non-cooperative shutdown), #68 (negative paths). These old titles do not prove each defect still applies or is closed by this patch. No issue or phase was closed.

**Exact next priority:** AT-03's first scoped initial-response/useful-progress/total design and fake-clock prototype on the existing compatible path, preserving this parent contract and current local preference compatibility; calibrate before enabling defaults or extending backend coverage. For remote diagnosis, obtain authorized sanitized receipts for retained P92 windows before proposing another D attempt or budget change. Activate a future candidate only through the validated idle/backup/lifecycle process when necessary.

**Acceptance:** AT-02/TD-02 pass for supported inspected paths and deterministic scope. New binaries are not live-activated. Cloud A/B/C remain historical passes; D still failed twice before tools in P92, E remains blocked. No actual Agent compile/run/output-15 or full E2E success is claimed.
