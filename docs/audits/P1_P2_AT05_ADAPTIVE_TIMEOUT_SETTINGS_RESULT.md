# P96 — AT-05 adaptive timeout settings and effective Chat policy

Date: 2026-10-30. Scope: real Desktop settings/IPC, existing settings persistence and supported plain-text Chat inference. Labels distinguish observed test evidence, source-derived ownership and deferred work. This does not certify calibrated hardware defaults, native Linux/Windows or Cloud Agent D/E.

## Baseline and preserved ownership

Observed entry: main at `f6edb704224b28c0a7b14b83634d435a1d1c5f78`, with the known 14 P95 modified/untracked paths. `/tmp/sentinel-p96-entry-manifest.json` captured their SHA-256 values. P95 runtime/provider changes and its historical result are preserved; timeout design/matrix are deliberately extended. Personal-Brain was unavailable; the explicitly supplied checkout was used. AGENTS, engineering standard, affected matrix, architecture, Agent/security/build/test contracts, P94/P95 results and current settings/IPC/provider/cancellation owners informed this work.

No reset, stash, clean, rebase, commit, push, merge or worktree was performed. Existing Debug and Release CMake trees were reused. There is no second configuration subsystem. No user daemon was started/restarted/stopped, no production settings/history/profile was migrated and no billable inference or model download occurred. Read-only final process inventory found no matching Sentinel Desktop/daemon process; disk artifacts are not evidence of loaded runtime activation.

Entry Release hashes matched P95: daemon `ac08f88356d4c2030ab87c59c391fdc3fcb7f548db3e0a63709e0c36a4984ee2`; Desktop `6666b80d51a3bd7ab589f8cccb50223db364e47e0d1f6cb3cbb927192334ae5d`.

## Actual settings architecture

Desktop `InferenceTimeoutSettings.qml` in Settings → Models & Providers/AI → Chat response waiting → existing `setProductSetting`/`productSettings` methods → typed `desktop.settings_service` IPC → DaemonDesktopSettings → SettingsService → AppSettings → existing ISettingsStore. ModelService retains a pointer to that same daemon AppSettings owner; new Chat admission snapshots it once into the captured ModelBinding and request options. SelectedEndpointChatProvider → LocalInference → ProviderRequestRuntime enforces the P95 clocks. No new controller, provider router, executor, settings file or per-widget inference constants were introduced.

`models.inference-timeout` is the validated writable settings ID; `models.inference-timeout-effective` is a read-only snapshot row from the same authoritative resolver. Both use the existing typed QVariant/JSON settings-service envelope. IPC specifications and generated contracts require no new field/action/schema. AppSettings owns serialized persistence; ModelService resolves backend/model capability and provider ceiling; ChatModeService owns a request's frozen effective options and terminal projection. AgentRuntime/AgentLoop, ContextBudgetManager and ToolExecutionGateway remain authoritative for their own scopes.

A selected-model preview is not a promise about a different workspace model. Runtime re-resolves policy for the actual captured workspace/model and operation, and explicitly refuses unsupported requirements. Image-bearing or non-streaming operations cannot silently acquire a visible-text progress contract. Text attachments use the existing text context admission. Active requests retain their frozen snapshot when preferences change.

## Implemented modes and resolver

- **Compatibility:** absent legacy key or explicit reset. Keeps the historical Chat 120000 ms request default and its existing provider-option precedence. Direct/local provider timeout zero and explicit provider values are untouched. No new initial/idle checks or Chat parent-admission lifetime is imposed by migration. This is distinct from the existing zero default for direct local inference and local Agent decisions.
- **Extended:** user supplies positive initial, idle and finite total limits. No hardware-derived or guessed preset is selected.
- **Custom:** finite positive total; zero initial and/or idle disables that particular check. All durations are integer milliseconds internally; the Desktop edits seconds with at most three decimal places and explicitly uses a decimal point.

`InferenceTimeoutSettings.h` validates the versioned profile and owns `resolveChatTimeout`. ModelService supplies actual registered protocol/capability support, provider class (local/cloud/unknown) and existing explicit provider timeout. Initial/idle values and total are capped by a shorter explicit provider ceiling for explicit profiles, and by a bounded parent's remaining lifetime when supplied. Initial/idle never exceed the resolved total. Cancellation outranks expiry. Missing hardware calibration is explicitly unavailable, not performance/capacity. No numerical default depends on guessed RAM/GPU/CPU/model metadata.

Explicit Chat profiles create one OperationDeadline before context admission; the child transport receives the same parent. Context admission/output bounds are unchanged. Stop checks occur before dispatch and after provider return; late success cannot complete the turn. P95 useful-progress validation, byte/chunk ceilings and optional usage/truncation behavior remain unchanged. Partial output is a failed/partial turn on timeout, not a completed answer or retried tool proposal. Actual applied policy is retained in the existing bounded per-turn budget diagnostics; no new durable diagnostic schema is claimed.

Agent classification/planning remain separate: cloud/unknown 60/120 s parents and local zero/configured compatibility remain unchanged. Approval expiry, process timeout, RPC acknowledgement timeout, model startup, leases and physical worker drain do not inherit these Chat preferences. UI states retain truthful Sending/Streaming/Failed/Cancelled semantics; no synthetic connecting/token/CPU progress was added.

## Persistence, migration and rollback

One canonical JSON string under `inferenceTimeoutProfile.v1` contains exactly version, mode, initialMs, idleMs and totalMs. Legacy absence reads Compatibility without any eager write. Reset writes a canonical Compatibility profile and leaves all old local/provider preferences unchanged. Single-value publication uses the existing store's atomic file/cache and StorageAdmission behavior; it is not a multi-owner transaction or new SQLite migration. Existing settings schema/history schema 8 are unchanged.

Validation rejects malformed/oversized (>1024-character) profiles, unknown versions/modes/extra fields, missing fields, wrong types, negative/fractional durations and values above signed-int milliseconds. Extended requires positive initial/idle/total; Custom requires positive total. Unsupported selected providers are rejected before saving via SettingsService, and runtime rechecks after provider/model changes. Invalid/future persisted profiles fail closed at inference rather than silently falling back. Store/admission refusal preserves prior bytes/cache and emits no profile-change signal.

An older binary ignores the added key and retains legacy behavior; that is data-format compatibility, not rollback behavioral parity. Reset to Compatibility and verify idle ownership before any separately authorized rollback/activation. Do not open incompatible history profiles with older executables. No live profile upgrade/downgrade was attempted. Unknown settings keys remain under the existing store's preservation contract; no credential/unrelated-setting migration was added.

## Provider coverage

| Backend/operation | Profile exposure/enforcement |
|---|---|
| OpenAI-compatible cloud, LM Studio, llama.cpp-compatible local | Explicit modes available only with a selected model and supported streaming capability on the existing compatible parser. Runtime also requires the actual provider's bounded-streaming contract. |
| Cloud facade/custom gateway | Uses the registered selected endpoint semantics; only OpenAI facade uses its custom gateway override. Native Claude/Gemini override hosts are not advertised as compatible progress. No preview reads API keys or constructs a credential-bearing provider. |
| Ollama NDJSON, native Claude/Gemini, unknown capability/provider | Compatibility remains available; explicit profiles rejected, UI explains the limit. No dialect parity claim. |
| Structured/native tool Agent generation, image-bearing/non-streaming Chat | No fabricated text-progress support. Agent keeps separate budgets; an explicit unsupported Chat profile refuses before provider dispatch. |

Declared model/provider capability is not live model verification. Low-performance CPU, cold startup, long prefill, pressure and slow generation receive no speculative tuning. A user can choose longer finite supported waits, while token/context/memory/cancellation/approval boundaries remain independent. AT-03-DIALECT and AT-03-CAL remain open.

## Desktop, save acknowledgement and reconnect

The compact themed card uses existing base components, progressive fields, seconds, validation messages, effective resolved values, save confirmation and explicit reset. Unsupported selections only offer Compatibility. Loading/disconnected/invalid/unsupported state is shown; an older daemon missing the new snapshot rows reaches an unavailable/version message instead of an infinite loading state. Controls remain disabled until authoritative values are available.

Save returns the existing Pending state. The UI confirms the desired saved value by an authoritative settings snapshot, never by optimistic local assignment. After disconnect or a bounded 10 s presentation wait, outcome is unknown/unconfirmed; it instructs the user to reconnect/inspect and does not replay the mutation. The 10 s presentation wait and one-second monotonic read-cache pacing are not inference deadlines. Read refresh is bounded and avoids response/query feedback. Reconnection queries stored values; a previously dispatched inference or Agent mutation is never restarted by this feature.

A captured request's Cancel and generation/message identity remain in existing runtime/daemon ownership. Stale replies cannot overwrite a cancelled/replacement turn. Native tool approvals are untouched. Noncooperative worker/process join and remote cancellation acknowledgement are not solved here (AT-04).

## Material findings and independent oracles

| Operating condition → failure consequence | Evidence → mitigation | Closure oracle |
|---|---|---|
| Local Chat uses its own 120 s request despite direct-local zero → slow model prematurely stops | ChatModeService hard-coded deadline, separate local provider setting → explicit finite Chat profiles, Compatibility preserves baseline | Saved Extended 600 s reaches resolver/runtime; old provider zero remains zero; no default migration |
| UI only shows requested values / changed provider → runtime differs silently | Shared resolver now feeds read-only snapshot and actual captured request; recheck after model changes | Explicit provider 30 s caps requested 600 s and initial/idle; selected unsupported model cannot POST |
| Unknown hardware/model → fabricated performance policy | No reliable hardware calibration in current owner → explicitly unused/unknown, no automatic presets | Missing model/unknown streaming rejects explicit profile; no model acquisition/lease changes |
| Partial multi-field save / storage refusal → drift or false success | One validated/versioned value and existing StorageAdmission | Save/reload/restart preserves fields; refusal leaves bytes/cache/signals unchanged |
| Legacy daemon lacks fields / lost ack → endless loading or duplicate mutation | Bounded unavailable state and Pending/read reconciliation | Controlled IPC timeout, late acceptance and reconnect retain one mutation; no automatic resend |
| Cancellation occurs before worker dispatch / late provider return → unnecessary send or false complete | New pre/post stop fences | Late noncooperative streaming reply fails by total; queued cancel does not require dispatch; old late-cancel terminal fence remains |
| Old shutdown fixture assumes cancelled work must run → invalid completion counter | First Release/Debug failures counted zero started workers, not a leaked join → explicit entry semaphores | Both workers enter before cancellation/reset; destructor joins and exactly two actual completions are observed |
| Custom Cloud facade endpoint confused with named providers → incorrect feature exposure | Inspected providerConfig endpoint ownership → metadata check follows the same override condition without credentials | DeepSeek ignores facade custom URL; actual OpenAI/native-host override refuses; native/unknown support stays false |
| Slow/hostile endpoint streams meaningless data → extended hang or memory growth | P95 clocks/parser safety unchanged | Existing fake-clock heartbeat/partial/total/cancel tests and byte caps continue passing |

## Changes and validation

P96 source owners: InferenceTimeoutSettings.h; AppSettings; SettingsService; ModelService; ChatModeService; DesktopRuntimeClient read cache; InferenceTimeoutSettings.qml and its ModelSettingsTab/CMake registration. Tests extend settings recovery/storage, Chat Mode, controlled Desktop IPC and the actual DesktopDaemonInteraction fixture. P95 source/results remain owned by the preceding work. Generated IPC, QML presentation boundaries, history and permission stores are unchanged.

First evidence is retained, not replaced:

- `/tmp/sentinel-p96-tests-build.log`: test-only missing LocalInference include (incomplete LMStudioConfig); corrected include, no production change.
- `/tmp/sentinel-p96-final-ui-ctest.log`: screenshot attempt refused the fixture's required evidence-root guard. Corrected the output location to the existing Release verification root; the guard stayed intact.
- `/tmp/sentinel-p96-release-ctest.log` and `/tmp/sentinel-p96-full-ctest.log`: worker-shutdown fixture failure after the correct pre-dispatch cancellation guard. Tests now wait on actual provider-entry semaphores; no production cancellation check was weakened.

Observed final validation:

- Existing Release tree: complete build passed. Focused CTest covered 13 targets: Desktop daemon/UI, settings recovery/storage, AppSettings/JSON store, Chat Mode, provider contracts, local inference, AgentLoop/AgentRuntime, ObservationPolicy, context budget and Desktop IPC. First run: 12/13 passed, only the stale worker fixture failed. Corrected Release rerun: Chat Mode plus latest Desktop UI **2/2 passed**, 12.18 s. All 13 targets therefore have passing latest results; this is not described as one initially clean run.
- Existing Debug tree: configure/full build passed. Full CTest **179/180 passed initially**, 483.91 s, same fixture failure. After explicit provider-entry barriers, rebuilt final Desktop/probe/Chat test and reran both affected/latest-UI targets: **2/2 passed**, 13.33 s. Every one of the 180 targets has a passing latest result. No production/runtime correction was needed for that fixture failure and no security check was relaxed.
- Full Debug internal QtTest exclusions: **nine existing skipped cases**, not passes: Linux-only Secret Service helper; blocked native incremental-tool assembly; four macOS no-fork shell/Docker/shutdown fixtures; opt-in live classification; missing-node branch when npx exists; unimplemented model-residency/removal lease acceptance. No skip was introduced here.
- New settings tests cover absence/Compatibility, Extended/Custom, zero, negative/fractional/overflow/type/version validation, save/reload with a newly constructed ModelService (restart fixture), preserved legacy provider values, configured ceiling, parent/cancel boundary, missing metadata, native/unknown dialect refusal and actual Cloud override ownership. Storage refusal has byte/cache/signal oracles.
- The real Chat adapter test uses injected transport and fake progress time; the saved 100/30/1000 ms test profile reaches applied request diagnostics and the real compatible parser, then times out at the idle boundary with one POST. A deliberately noncooperative streaming provider cannot complete after the saved total. These are fixtures, not live model measurements.
- Actual Desktop QML/viewmodel/daemon IPC flow validated keyboard selection/save/reset, invalid zero-total no-write, effective values, disconnect/reconnect persistence, unsupported-provider exposure, focus and 560 px light/dark layout. Screenshots under `build/release/desktop-daemon-verification/p96/timeout-Paper.png` and `timeout-Liquid Glass Dark.png` were visually inspected and show readable controls/content without horizontal clipping. Their sample 120/60/600 s values are fixture input, not shipped presets.
- Controlled wire IPC specifically withheld the timeout-profile mutation acknowledgement, let the request fail, sent a late acceptance and reconnected; exactly one mutation and one failure remained. Existing Agent duplicate/permission/cancel regressions passed. No real user permission grant or live API call was made.
- Both IPC generator `--check` commands, changed-line clang-format 18 plus new-header format check, final QML lint, personal-path scan (zero flagged), local documentation links and whitespace checks passed.

Limit: the missing-row/old-daemon unavailable branch and presentation-only 10 s unconfirmed wait are compiled/source-reviewed behavior; an actual older daemon/profile was not launched and native IME/Wayland/Windows is not accepted here. The controlled IPC timeout/late-reply mechanism and actual UI disconnect branch were executed separately. Complete per-active-phase diagnostic/countdown projection is intentionally not fabricated.

Final Release SHA-256:

- daemon: `351d9a0e09a80e6b412e11039f8661ca1f73b3868d83c42589f1fed6a3363c35`
- Desktop executable: `fa9e1d515cb38766846a0829af8564656095fdccb68c09493bdc1b02b99e1d5f`

Candidate is base commit plus the uncommitted P95/P96 sources, not a clean release tag. `/tmp/sentinel-p96-final-source-manifest.json` and `/tmp/sentinel-p96-provenance.json` pin source/artifact identity. Rebuilt disk files do not prove activation or release approval. Commands reused `cmake --preset tests`, `cmake --build --preset tests`, `ctest --preset tests --output-on-failure`, full `cmake --build build/release` and the recorded focused/rerun regexes. Logs are `/tmp/sentinel-p96-*`, including full/first-failure, corrected Debug/Release and QML receipts.

## Remaining acceptance and next priority

1. **AT-03-CAL / P2/P4, High:** measured CPU-only/constrained-memory/cold/warm/large-prefill/long-output cells; independent phase times and resource receipts before offering calibrated presets. Unknown telemetry never selects a budget.
2. **AT-03-DIALECT / P2, High before parity:** separately validate Ollama and native Claude/Gemini semantic event/cancellation/resource contracts before exposing explicit modes. Structured/tool assembly remains separate and bounded.
3. **AT-04 / P3/P5, High:** real startup/readiness/lease and noncooperative drain ownership; Stop/terminal UI is not a worker join or provider billing acknowledgement. Historical #43/#54 linkage remains open.
4. **AT-06 / P5/P6/P12:** exact candidate on native Fedora KDE/Linux and Windows, including slow scheduling, settings restart/reconnect/rollback, keyboard/IME/locale/theme/accessibility and cancellation. macOS offscreen fixtures do not certify Wayland/native Windows.
5. **Next development priority:** native/backend calibration and AT-03-DIALECT evidence, then any measured preset adoption. AT-05's first supported Chat settings slice is implemented; active per-phase countdown/progress visualization and broader dialect/operation profiles require their own truthful IPC evidence, not guessed states.

Mappings: AT-02/03/05/06; AC-P2-03/04, AC-P4-01–03, AC-P5-01–05, P6/EP-P6. Historical provider parity #42, shutdown #54 and negative-path coverage #68 are related tracking, not freshly closed/modified issues. No remote issue/roadmap rewrite or Cloud Agent D/E acceptance was performed.
