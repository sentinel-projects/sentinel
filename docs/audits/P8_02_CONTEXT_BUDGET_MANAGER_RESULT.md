# P8-02 — Context Budget Manager and safe context assembly

Status: shared production Chat/Agent budget admission implemented. Validation results below refer
to synthetic fixtures and an uncommitted candidate, not live-provider acceptance or exact token
accounting. PD-D03, PD-D04 and PD-D10 remain unchanged.

## Verified base and delivery boundary

Existing supplied Sentinel checkout only. Origin verified as
`https://github.com/sentinel-projects/sentinel.git`; fetched origin and confirmed clean
main = origin/main = `1940fb622ac7314bee132940f0219f495d0e0867`.
P8-01 implementation `19f223e9` and audit `6503af29` are integrated ancestors of that merge.
Read AGENTS, architecture, Agent Runtime, security, building/testing and P8-01 audit.
Personal-Brain `brain` is unavailable on PATH; no alternate repository path was guessed.
Visible chat inventory showed only this coding chat active in the checkout. No subagents were used.
Created absent `feature/p8-02-context-budget-manager` from verified main; branch/base/origin are
rechecked at handoff. Reused `build/tests` and `cli/target`; analysis-only compile database under
`/tmp` is not an additional build tree.

The earlier maintainer authorization to publish P8-01 was completed before this phase. This
P8-02 candidate remains uncommitted: no P8-02 commit/push/merge/reset/rebase/stash/clean/PR,
worktree/clone, workflow dispatch, live model, paid call, model download, user-profile database
migration, background indexing or resource-policy change.

## Exact changed files (27)

- `apps/sentinel-daemon/service/DaemonIpcServer.cpp`
- `core/CMakeLists.txt`
- `core/include/sentinel/core/agent/ContextEngine.h`
- `core/include/sentinel/core/agent/LlmAgentRuntime.h`
- `core/include/sentinel/core/agent/ObservationPolicy.h`
- `core/include/sentinel/core/chat/ChatModeService.h`
- `core/include/sentinel/core/context/ContextBudgetManager.h`
- `core/include/sentinel/core/interfaces/IChatProvider.h`
- `core/include/sentinel/core/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.h`
- `core/src/agent/AgentRuntime.cpp`
- `core/src/agent/ContextEngine.cpp`
- `core/src/agent/LlmAgentRuntime.cpp`
- `core/src/agent/ObservationPolicy.cpp`
- `core/src/chat/ChatModeService.cpp`
- `core/src/context/ContextBudgetManager.cpp`
- `core/src/provider/SelectedEndpointChatProvider.cpp`
- `core/src/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.cpp`
- `docs/audits/P8_02_CONTEXT_BUDGET_MANAGER_RESULT.md`
- `docs/development/ARCHITECTURE.md`
- `tests/CMakeLists.txt`
- `tests/core/test_chat_attachments.cpp`
- `tests/core/test_chat_mode_service.cpp`
- `tests/core/test_context_budget_manager.cpp`
- `tests/core/test_llm_agent_runtime.cpp`
- `tests/core/test_observation_policy.cpp`
- `tests/desktop/test_desktop_ipc.cpp`
- `tests/helpers/DesktopDaemonFixture.h`

## Source audit and previous limits

| Owner | Before P8-02 / remaining ownership |
|---|---|
| ChatModeService | 12000-character user entry cap; prompt selected at most 24 messages, 10000-character running history cap, older messages right-clipped to 3000, attachment text outside that cap; summary up to 2000 substituted for covered history; profile up to 2000; independent most-recent four images; no common model-bound input/output admission |
| ContextEngine | Unknown window 8192; reserve max(1024,window/5,model output maximum); minimum input 256 even when window insufficient; 24000 candidate cap and 550 assumed framing; characters/4+16 per item; Critical goal bypassed limits; section limits index1000/tools2600/skills1200/facts1100/observations1700/conversation650/history350/memory350, proportional remaining fractions; optional text excerpts 160/300/400/420, observations320/500 |
| LlmAgentRuntime | Final rendered security/planner contract, JSON escaping, repeated active goal, evidence inventory, repair prompt and response schema were outside ContextEngine estimate; fixed requested output2048/deadline120000; native result preview first/last1000, latest legacy result left1500 |
| Native protocols | OpenAI-compatible serializer clipped tool result text at6800; Anthropic/Gemini helper defaults6800; opaque continuation histories have provider-owned structure; provider transport/capability validation remains in existing owner |
| SelectedEndpointChatProvider | Existing OpenAI-native serialized-body guard estimates UTF-8 bytes/3+256, reserves clamped output and checks reported context before transport; retained as additional provider defense, not new model authority. Native Claude/Gemini did not share that guard |
| ModelService/ModelBinding | IModelRouter resolves one binding; merged capabilities include context/output optional values and ModelMetadataSource provenance; binding contextWindowTokens snapshots that capacity. No new selection/resolution authority added |
| ScopedMemory/SQLiteMemoryStore | P8-01 SQL filters session/workspace/project/personal before matching/order/LIMIT; bounded literal query4096, up to6 terms of64, max50 candidates; Chat fetch8, Agent fetch up to3. Existing scoped presentation helper limits8/400-value-chars/4096bytes; new context memory items preserve complete records instead of using that excerpt helper |
| ObservationIntentPolicy / daemon title | Classifier previously bounded conversation1500 or prefix1000+workspace suffix600 with output2048/deadline60000; title used first6 eligible message excerpts1000 each, at most3 completed turns, output title60 with80 rejection but no explicit provider output bound. Both now admit the complete helper prompt through the same manager using the existing binding; classification failure stays indeterminate, title refusal preserves existing title; title reserve64 |
| SemanticRetrieval | Existing deterministic/simulation/embedding activation and prompt-authority gates; scoring is not scope authority; unchanged, no activation or indexing |
| CompactionService | Legacy separate summarizer, rough chars/4 estimate, default threshold3500 and configurable recent preservation; can inject generated summary as System in its own legacy path. Not promoted into the new Chat/Agent admission authority |

The source-discovery ceilings in ContextEngine remain bounded candidate preparation. Its cost
uses the shared uncertainty-bearing estimator; unknown window/default output reserve share the
manager constants, the output *maximum* is no longer mistaken for a required output reserve,
and the minimum256 input floor is removed. Critical units cannot bypass final admission. Required task, requirements, workspace,
profile, facts and non-stale observations reach final admission intact. Old observation clipping
and duplicate-resource suppression are removed. Optional discovery/skill/history sections retain
their bounded existing preparation/excerpt policy, explicitly distinct from request admission.

## Shared contract and fallback rules

`ContextBudgetManager` takes an already-resolved binding, request options, typed candidate units,
and an optional renderer. It never obtains providers, changes model/workspace/session selection,
retrieves data or grants authority. Its result carries the binding and session/workspace snapshot,
capacity/provenance, effective policy ceiling, reserved output, estimate uncertainty, required-input,
tool/structured-schema and continuation estimates, per-unit kind/inclusion/omission decisions,
prompt and typed Ready/Reduced/refusal classification.

Positive reported capacity is honored with its source; a smaller legacy binding snapshot may
only narrow it. Unknown capacity uses **8192**, preserving the former Agent fallback as a finite
policy ceiling, never a claim that the actual model has8192 tokens. Positive values without source
metadata, or with only ProviderDefault provenance, may narrow this fallback but cannot expand it.
A provider default is not model-specific runtime verification. Invalid nonpositive reported values,
negative binding bounds, nonpositive reported output maximum or negative requested output refuse.
Reported capacity remains optional/JSON null when absent, rather than becoming zero.

Default output reserve is **2048**, limited by a positive binding output maximum. Admission uses
that reserve before optional context. Production Chat and Agent both send this bound through
existing options-carrying provider APIs. Chat uses existing bounded streaming when implemented,
otherwise bounded sendRequest; it no longer discards output limits via an unbounded streaming
entry point. Deadlines/cancellation remain enforced by existing provider implementations.

Text estimate: ceil(UTF-8 bytes/3), plus25% uncertainty rounded upward, plus256 framing tokens.
The rendered prompt is JSON-escaped before measurement. Native tool definitions reuse the actual
shared OpenAI function/schema conversion, plus full description/parameter and per-declaration
framing allowance to cover native bridges. Structured-output schema/name and whole native
arguments, result text, IDs and distinct opaque provider continuation histories are charged.
A continuation may overcount a regenerated prompt that its native provider replaces with retained
history; conservative rejection is preferable to hiding that history. This is a documented policy
estimate, **not a tokenizer, exact count, mathematical upper bound for every model, or provider
usage receipt**. Provider overflow errors remain possible and remain failures.

Required units and render-generated security/repair instructions must fit first. Optional units
use descending priority, stable input-order ties; selected units render in original order. The
entire final prompt is remeasured after each tentative addition, including JSON expansion.
No text slices are made during admission. An oversized optional unit is omitted whole; oversized
required instructions/task/evidence/schemas/continuation return typed refusal with no dispatch.

Native call/result counts, ordered IDs, duplicate/missing IDs, registered/enabled tool identity,
and opaque history JSON array consistency are checked before admission. The batch/opaque history
is indivisible. Budgeted production requests explicitly set `completeToolResults` in the authoritative
ChatRequestOptions contract. Their complete text survives OpenAI serialization, and production
Anthropic/Gemini use existing helpers with a complete-content bound. Legacy/direct calls and
standalone compatibility-helper defaults stay6800, preserving existing golden bytes. A manager
caller requesting a larger native result without the complete-unit option gets InvalidMetadata
rather than approval followed by preview slicing. Native transport, credentials, network policy and response
protocol are unchanged; under-budget protocol fields remain identical.

## Production Chat and Agent integration

Agent observation classification now receives the same runtime-owned binding snapshot and
budgets its complete compact classification request before dispatch. Daemon title generation
also budgets its already-bounded prompt and sends a64-token output bound with its existing
resolved provider. Neither helper selects a new model through the budget manager. Existing
classification authorization/evidence checks and title race/cancellation rules remain.

Chat resolves the existing ModelBinding once. It freezes scoped access and retrieved candidates
on the owning thread before creating its worker. It preserves system instructions, response
profile and the active user turn, including attachment text. Recent complete user/assistant turns
rank before older turns; authorized Session, Project, explicitly enabled Personal memory follow.
No persisted generated summary replaces retained verified transcript. Replaced/failed assistant
messages remain filtered by existing rules. A refusal closes the persisted assistant placeholder
as Failed/RequestRejected without calling the provider. Diagnostics expose only counts/estimates.
A worker captures prompt/options/provider/token/conversation/message, preserving late-callback
and cancellation checks. Switching workspace or selected model cannot redirect an accepted turn.

Agent ContextEngine prepares bounded candidates and carries runtime session/workspace identity.
LlmAgentRuntime budgets the **rendered** planner/security contract, immutable goal, workspace and
requirements, evidence/facts/observations, output schema, actual native declarations/results and
repair suffix before every planning attempt. Native tool metadata omitted from the text renderer
is not counted as an included text unit; its actual schema options are charged separately.
Required observation or continuation overflow gives GiveUp/RequestRejected before provider
execution, never an accepted FinalAnswer. Whole current observations replace previous previews;
no failure tail is hidden by budgeting. Existing upstream ToolOutputTruncator and evidence coverage
flags still describe bounded tool results; this milestone cannot recreate data already excluded
by a tool. ToolExecutionGateway authorization and AgentLoop final-answer acceptance are unchanged.

Scoped records are independently checked with P8-01 `visibleMemory` after store retrieval, including
session/workspace and explicit Personal policy. Malformed ID/provenance/time metadata is excluded.
Included memory carries stable ID, scope, owner/workspace, source, provenance and explicit
untrusted classification, with full value. GeneratedSummary/Inferred/Unknown stays historical data,
never security/user instructions. Large records are omitted whole, not flattened or reclassified.
No legacy-global fallback, cross-project access or implicit Personal inclusion is introduced.

## Diagnostics and behavior changes

`budgetDiagnostics()` in real Chat and LlmAgentRuntime returns bounded count/status/estimate JSON.
No prompt, record ID/value, workspace path, tool arguments/result text, credentials or retrieved
content is logged. Unknown input estimates after early refusal and actual provider token counts
are null. Native schemas/continuation/required estimates remain distinct from actual usage.
Per-unit omission reasons are capacity, unauthorized or invalid metadata; Agent also reports its
source-preparation omission count separately. Decisions describe current candidate units;
indivisible native history may also contain previously included context and is charged in full.

Short Chat characterization remains exactly `User: Explain recursion`; existing protocol goldens
remain within-budget fixtures. Deliberate changes: whole-turn/whole-record selection replaces
character slicing; generated summaries no longer displace transcript; large necessary observations
and all-tool native declarations may refuse under the unknown fallback. Native protocol-focused
fixtures now declare an explicit synthetic131072 capacity so they test protocol behavior within a
budget rather than bypass admission. A separate small/unknown capacity test verifies refusal.
Production planner fixtures now assert complete text, including failure evidence. Legacy native
preview goldens remain unchanged; a separate complete-unit serialization test verifies the new
production option.

## Compatibility limitations

- No provider contract exposes a trustworthy image token/tiling bound. Image-bearing Chat/Agent
  requests return `MultimodalUncertain` before dispatch, even on vision-capable bindings. Base64
  byte length is deliberately not treated as image tokens. Historical image-bearing Chat turns
  are omitted whole with an explicit invalid/unrepresentable metadata reason. Text attachments
  remain budgeted in full. Provider-specific visual accounting is follow-up work; this is a
  visible safety reduction, not a claim of completed vision accounting.
- Legacy ApplicationController local-inference context injection retains its existing policy.
  P8-01 already excludes committed memory from that flattened path because it cannot preserve
  typed provenance. It is not silently routed through this manager or certified by P8-02.
- ContextEngine still has bounded source discovery and excerpt policy for optional global chat
  history/skills/tool discovery. This does not certify global conversation history as universally
  workspace-isolated; P8-01 scoped-memory authority is the contract preserved here.
- No model-specific tokenizer/exact provider input usage, automatic summary replacement, memory
  promotion, embeddings/indexing, model residency or resource admission is introduced.
- Unknown-capacity/all-tool requests can now refuse earlier. Large required execution evidence
  intentionally stops the run, preserving prior approved changes for inspection.

## Focused regression evidence

New deterministic manager tests cover short requests, complete-unit priority/order, output reserve,
known insufficient/unknown/malformed capacity, scope/Personal rules, missing metadata, large memory,
provenance after omission, schema overhead, native result ID/history integrity, large observations,
Unicode boundaries and explicit visual uncertainty. They independently inspect actual serialized
OpenAI-compatible native messages and confirm a9000-character result retains its full tail.
Production Chat tests inspect delivered prompts for long-history reduction and immutable accepted
model/workspace; existing cancellation/late-callback tests remain. Agent tests assert refusal
before provider/final success when required capacity/evidence cannot fit. Existing native protocol,
Chat/Agent, storage/scope and reliability suites provide regression coverage without live APIs.

## Validation results

Results use macOS27.0.1 arm64, Qt6.12, Apple LLVM21; CTest runs sequentially with
`TMPDIR=/tmp`, disposable HOME/XDG directories, offscreen Qt and permitted local fixture IPC.
No build overlapped a CTest run. Runtime tests exercise the real daemon/Desktop/Chat/Agent paths
with synthetic providers, including loading/disconnected/error/streaming/cancellation, focus and
responsive shell flows; no live-provider/native platform acceptance is promoted.

| Check | Result |
|---|---|
| `cmake --preset tests` | Passed, existing build reused |
| Full `CCACHE_DISABLE=1 cmake --build --preset tests` | Passed; final receipt `p802-build-final3.log`, zero errors; one existing unchanged HuggingFaceModelSource.cpp:91 `-Wshadow` warning |
| Pre-change Chat/Agent/provider characterization | 3/3 executables passed, 5.79 sec (`p802-characterization.log`) |
| Focused shared manager/Chat/Agent/memory/provider suites | 11/11 passed, 13.46 sec (`p802-focused-final.log`) |
| Additional helper/Desktop/attachment/serialized-protocol scope | 6/6 passed, 35.01 sec (`p802-focused-complete.log`); final source validated again in full run |
| New manager Qt test | 10 passed rows including init/cleanup, zero failed/skipped (`p802-ctest-final.log`, test29) |
| **Final sequential full CTest** | **169/169 passed, zero failed, 241.44 sec** (`p802-ctest-final.log`) |
| Rust fmt | Passed `cargo fmt --manifest-path cli/Cargo.toml --all -- --check` |
| Rust Clippy | Passed workspace/all-targets/locked/offline with `-D warnings` (`p802-clippy.log`) |
| Rust workspace tests | 142 passed, zero failed/ignored, locked/offline (`p802-rust-tests.log`) |
| Both IPC generators `--check` | Passed `generate.py` and `generate_desktop.py`; no IPC schema/wire changes |
| QA registry/schema | Passed; existing missing external `roasd.packaging` evidence warning; candidate remains dirty on verified base; no feature-status promotion |
| CI-pinned clang-format18.1.8 | New files fully clean; all changed C++ hunks produce empty diff (`p802-format-verified.diff`) |
| Scoped clang-tidy | Nine production sources, new manager/header in full and changed lines elsewhere: all exit0, zero reported project diagnostics (`p802-tidy-verified-*`) |
| Personal-path scan | Zero flagged files (`p802-privacy-verified.log`) |
| Credential-signature scan | 27 changed files, zero flagged; private-key/GitHub/AWS/OpenAI signatures, content suppressed (`p802-credentials-final.log`) |
| `git diff --check` | Passed |

Static analysis uses the documented analysis-only compile database, preserving production
compiler/PCH. Zero diagnostics is a scoped changed-line/new-manager claim, not whole-repository
certification; non-user/out-of-scope diagnostics are suppressed. The first extended analysis found
one new DaemonIpcServer brace-style warning; braces were added before the final verified run.

Intermediate failures are retained, not reclassified: first focused run passed3/4 executables
(Agent fixture had8 internal failures because previously unbounded all-tool native fixtures had
no capacity metadata); second passed3/4 (one obsolete clipped-result expectation). These fixtures
were corrected to declare synthetic capacity or assert complete admitted observations. First full
CTest passed165/169 in274.18 sec: Desktop streaming fixture lacked the bounded overload, attachment
fixture expected unaccounted visual dispatch, one legacy native preview golden was temporarily
changed, and the Desktop title fixture only implemented sendMessage. Legacy native golden bytes
were restored with explicit production `completeToolResults` opt-in; helpers/fixtures now use the
bounded contract and visual refusal is tested explicitly. A four-fixture rerun passed3/4 with a
fatal fixture assertion because title requests still had output0; title production admission64
and the constructor-bound classification path were added, then all six focused executables and
the final169/169 run passed. One intermediate C++ build failed with2 lambda capture diagnostics
in the title helper; the existing cancellation capture was restored before the passing full build.
No product authorization or evidence acceptance was weakened to make tests pass.

### Internal SKIP, XFAIL and warning rows

The final verbose Qt output has **10 SKIP, 2 XFAIL, 81 QWARN**. These are internal test rows,
not failed CTest executables; they retain existing platform/contract limitations. Additional
non-QWARN stderr diagnostics:11 font-family alias messages and3 deliberately invalid-PNG messages
from the corrupted-frame fixture. No new live/native success is inferred.

SKIP rows:

- `CredentialStoreTest::missingLinuxHelperRefusesPersistence() Linux Secret Service helper contract.`
- `CredentialStoreTest::testPlatformCredentialBackend() Native credential write refused; no real smoke success claimed.`
- `AgentReliabilityHarnessTest::cor11_cancelAfterFirstStreamedDelta(native-tool-assembly) COR-11 native incremental tool-call assembly remains explicitly blocked: IChatProvider exposes no typed incremental tool-argument assembly event, so a partially streamed native call could only be dispatched after a validated terminal assembly. This fixture asserts the native request stays on the bounded non-streaming path (one sendRequest, zero streamed deltas, zero public deltas) and does not invent an assembly contract.`
- `AgentRuntimeTest::asyncRunCommandStreamsAndContinues() This shell fixture requires child creation; strict macOS plans deny forks. Native host lifecycle is covered by plugin integration tests.`
- `AgentRuntimeTest::asyncRunCommandCancellationAndTimeout() This shell fixture requires child creation; macOS strict no-detached-child plans now deny all forks. Plugin host cancellation is covered by real host integration tests.`
- `AgentRuntimeTest::asyncDockerUsesProcessExecutorAndPreservesRestrictions() The fake Docker shell fixture requires child creation; strict macOS plans deny forks.`
- `AgentRuntimeTest::shutdownStopsActiveCommand() This shell fixture requires child creation; strict macOS plans deny forks. Native host shutdown is covered by plugin integration tests.`
- `ObservationPolicyTest::optInLiveClassification() Explicit opt-in required for production local-provider probe`
- `RealToolExecutorToolsTest::browserToolsReportMissingNodeGracefully() npx is installed on this machine; the missing-node branch cannot run.`
- `LocalModelRuntimeTest::removalDuringActualUseRemainsBlocked() No model-residency lease/removal contract exists; a live provider handle is not proof of active model use. Source-only D6.`

XFAIL rows:

- `LocalModelRuntimeTest::providerSwitchRetainsOwnedChild() P3-02 D4: switching provider returns before owned cleanup`
- `LocalModelRuntimeTest::endpointSwitchDoesNotReconcileOwnedChild() P3-02 D5b: same-model endpoint switch leaves old owned child`

Privacy-safe QWARN counts by fixture class (includes intentional rejection paths):

| Fixture | QWARN |
|---|---:|
| `McpToolProviderTest` | 1 |
| `McpIntegrationTest` | 1 |
| `PluginToolIntegrationTest` | 10 |
| `SessionExecutionPatternsTest` | 1 |
| `BackgroundJobPersistenceTest` | 1 |
| `ProcessExecutorTest` | 1 |
| `PluginManagerTest` | 3 |
| `DesktopShellViewModelTest` | 1 |
| `DaemonIpcServerTest` | 19 |
| `SentinelGuiTests` | 1 |
| `DesktopIpcTest` | 12 |
| `ProviderAgentE2eTest` | 7 |
| `AgentReliabilityIpcTest` | 4 |
| `NativeIntegrationTest` | 19 |

Receipts are the uncommitted candidate on the exact base above. Final QA validation is run after
this audit is written; its digest is a point-in-time candidate digest, not a clean-commit receipt.
After maintainer commit, repeat checks if clean-SHA evidence is required. All modifications are
left uncommitted for review as requested.

## Recommended P8-03 scope

Build explicit scoped memory inspection/correction/deletion and scope-preserving export/import
with source resolution and review. Keep retrieval authority and provenance separate from budgets.
Specify provider-specific visual-token bounds and optional model tokenizer adapters as distinct
accounting work. Do not add automatic memory promotion, background indexing or residency/resource
policy to scoped memory management. Preserve PD-D03/PD-D04/PD-D10 pending decisions.

## Maintainer-authorized publication after handoff

After the uncommitted implementation handoff, the maintainer authorized committing P8-02 on its
feature branch, publishing it and integrating/pushing main. Before commit, origin was fetched,
main matched origin/main and the candidate digest matched the tested handoff exactly. The
implementation and tests are commit `514d2741`; architecture and this audit are recorded separately.
The validation receipts above remain pre-commit candidate evidence, not a new clean-SHA test run.
