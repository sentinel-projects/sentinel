# P2-M08 — native discovery parser extraction

Behavior-preserving extraction; deterministic fixture evidence only. AC-P2-01 remains Unverified
and AC-P2-03 remains Blocked without separately authorized live provider/model certification.
PD-D10, provider lifecycle and physical model ownership are unchanged.

## Verified workspace and exact base

Existing supplied Sentinel checkout only. Personal-Brain `brain` unavailable on PATH; no alternate
checkout was guessed. Read AGENTS and P2-M01–M07 reports plus architecture, Agent Runtime,
security/build/testing instructions. Fetched origin; clean main = origin/main = HEAD at
`6ef8fc3967a3552e809ea9a5b514e5fa16c5e185`, the QA-02B merge. Earlier completed provider and UI/QA
milestones are integrated. Only this coding agent was active; the other visible coding chat was
idle. Created the absent `refactor/p2-m08-native-discovery-parsers` branch from main.

Existing `build/tests` and `cli/target` reused. The temporary analysis-only compile database is not
a build tree. No clone, worktree, separate checkout/build tree, commit, push, merge, reset, rebase,
stash, clean, PR or workflow dispatch. No live credentials, provider requests, model downloads or
unapproved feature enablement. All changes remain uncommitted. Exact HEAD/branch are rechecked at
handoff; local ignored build logs are execution evidence, not clean future-commit certification.

## Before/after source topology

| Responsibility | Before | After |
|---|---|---|
| Gemini catalog model/name/display/token-limit scanning | Inline fetchGeminiCloudModels in OllamaRuntime.cpp | Pure parseGeminiModelCatalog in existing GeminiGenerateContentProtocol |
| Anthropic catalog payload | fetchAnthropicCloudModels calls public OpenAI-compatible fetcher, which uses existing pure data/id scanner | AnthropicMessagesProtocol exposes pure parseAnthropicModelCatalog, delegates the same OpenAI-compatible scanner; no copied scanner |
| Shared data/id HTTP execution | Public fetchOpenAiCompatibleModels body | Same body in private fetchModelCatalog helper receiving a pure parser function; public OpenAI fetcher and Anthropic fetcher delegate |
| URL, key/header construction, getJson, discovery retry/deadline/cancellation, wire/error classification | OllamaRuntime | Retained in OllamaRuntime |
| Gemini error strings, malformed outcome, caller errorOut behavior | OllamaRuntime | Retained in OllamaRuntime |
| Poll dispatch/settings snapshots, sequencing, health, catalog acceptance and capability provenance | ApplicationController/ModelService/ProviderRegistry | Unchanged |

The new pure functions accept only QJsonObject and return optional QList<OllamaModelSummary>:
nullopt means the whole catalog is malformed; a present empty list is a valid empty catalog.
They consult no service, settings, credential, transport, clock or network policy. The existing
legacy OllamaModelSummary value type is reused, as OpenAiCompatibleProtocol already does; the
header naming/type-coupling migration is deferred rather than inventing a second catalog model.
Anthropic's deliberate reuse of the OpenAI scanner preserves its historical metadata behavior
and a single source of truth. No provider implementation, protocol selection or second HTTP path
was introduced. LMStudioConfig and StaticProviderCatalog were not renamed or reconciled.

## Golden characterization and equivalence

Before production edits, added and executed the characterization target through the actual native
fetchers using ScriptedProviderTransport: **44 QtTest passes, zero failed/skipped**. After extraction,
the same baseline again produced **44 passes**. Final suite adds direct pure-parser comparisons
against the same fixtures and four malformed-JSON/DNS wire rows: **48 passes**.

There are **32 catalog golden rows**, eight original HTTP precedence rows, four additional wire
rows, one catalog acceptance case, one cancellation/key case, plus setup/cleanup. Expected compact
JSON bytes are inline literal fixtures in test_native_discovery_characterization.cpp, not generated
by the implementation. Each golden catalog compares every model's ordered name, modifiedAt,
size, token limits, tool/structured/vision/stream/audio support and publisher/family/architecture.
Raw provenance remains Unknown. Both the real fetcher's result and the pure parser must equal the
same expected bytes. GET method, empty request body, exact URL/query, headers, request count,
completed/category/status/request ID and exact parser error strings are checked.

| Oracle | Frozen behavior |
|---|---|
| Native successful lists | Trim Anthropic id; preserve original entry order; Gemini trims name once then strips one models/ prefix; displayName becomes modifiedAt or Google Cloud fallback |
| Empty lists | Successful empty catalog, distinct from malformed |
| Missing/wrong list, missing/null/numeric/blank IDs, non-object entries, partially valid lists | Reject entire catalog; never publish partial results |
| Duplicates | Retained in wire order; Gemini prefixed/unprefixed names can normalize to duplicate IDs |
| Pagination | Neither native fetcher currently follows pages. Anthropic has_more/first_id/last_id and Gemini nextPageToken are ignored, including malformed metadata; exactly one GET, no follow-up calls |
| Anthropic metadata | Keeps existing OpenAI scanner behavior: created, integer context_window/max_context_length fallback, optional capability fields and metadata. Native display_name/created_at remain ignored |
| Gemini metadata | Only input/output token limits and display label are read. supportedGenerationMethods and capability objects do not filter models or enable capabilities |
| Error payload with HTTP 200 | MalformedResponse with vendor's existing safe catalog message |
| HTTP 401/403 with non-JSON error body | AuthenticationRequired precedes payload validation; one request |
| HTTP 429/503 with non-JSON body | RateLimited/ProviderUnavailable precede payload validation; two discovery attempts |
| HTTP 200 invalid JSON / DNS failure | MalformedResponse / ConnectionFailed; respectively one/two attempts, private marker absent from public diagnostics |
| Missing key plus cancelled token | AuthenticationRequired wins before dispatch; zero requests |
| Pre-cancel / cancellation while reply held | Cancelled; respectively zero/one requests |
| Catalog acceptance | Duplicates/order retained; first matching duplicate supplies metadata; old sequence ignored; current failed/cancelled observation preserves cache as Stale; successful next catalog replaces rather than appends; successful empty becomes Empty |
| Provenance | Raw parser fields remain Unknown provenance; ModelService supplies ProviderCatalog for cloud-reported numeric limits, ProviderDefault for declared capabilities; unknown structured output stays Unknown |

Compatibility quirks are intentionally frozen: successful Gemini discovery leaves a preexisting
errorOut string unchanged, whereas Anthropic/OpenAI clear it. A positive fractional Gemini limit
passes its historical numeric guard and Qt toInt yields zero; oversized values remain unset.
Gemini `models/ x ` becomes ` x` after the single initial trim and prefix removal. These are not
silently repaired or strengthened during extraction.

The first test attempt had two compiler errors from Qt's void-returning assertion macro inside a
value-returning snapshot helper; assertions were moved into the test slot. Initial execution then
reported 37 passes/7 failures because the test assumed Gemini cleared errorOut. Source inspection
confirmed the legacy behavior; golden expectations were corrected before production extraction.
The passing 44-case before/after logs are retained separately in ignored build output.

## Compatibility and security boundaries

| Surface | Evidence / retained boundary |
|---|---|
| claude / Anthropic | Canonical HTTPS /v1/models; trimmed x-api-key and anthropic-version 2023-06-01; same data/id scanner and generic Model list errors |
| gemini | Canonical HTTPS /v1beta/models?key=...; same query construction, single response and Google Cloud label fallback |
| cloud-api | Existing selected-vendor routing and unknown-vendor construction fallback unchanged; discovery still has no unknown-vendor branch |
| openai-compatible | Existing discovery exclusion preserved; no new polling participation |
| Other OpenAI-compatible/Ollama/llama.cpp | Existing public fetch signatures and HTTP execution reused; optional /props and native /api/tags paths unchanged |
| ProviderRegistry / StaticProviderCatalog | IDs, aliases, order, defaults and placeholder compatibility unchanged; existing registry/catalog/router suites retained |
| Capability declarations / active binding | No capability enablement, provenance promotion, factory override or ModelBinding/lifecycle change |
| Security | Authentication, endpoint policy, network mode, timeouts, retries, cancellation and safe error/redaction remain orchestration responsibilities |

All scripted HTTP is in-process; no socket to a vendor is opened. Temporary Online policy allows
production hostname validation to execute against injected transports and restores its previous
value. Existing native/chat/request golden tests are retained unchanged. No approval, sandbox,
permission, discovery-generation, provider health or catalog-acceptance policy was moved into a
protocol utility. No live TLS/DNS/proxy/rate-limit/model acceptance is inferred from fixtures.

## Validation

Raw output remains in the existing ignored build tree.

- Baseline: 44 passes before and 44 after extraction; final characterization **48 passes**, no
  failures/skips. Additional wire rows exercise untouched getJson/ProviderRequestRuntime behavior.
- Focused provider/protocol/catalog suite: **13/13 executables Passed**, **14.17 seconds**;
  **779 QtTest passes, 0 failed/skipped**, including existing native/request/protocol goldens.
- Complete C++ tests-preset build Passed with CCACHE_DISABLE=1, existing tree; final incremental
  complete-target build also Passed, **zero compiler errors/warnings**. Expected local unsigned/
  ad-hoc macOS signing configuration remains unchanged.
- Rust fmt, locked/offline workspace/all-target Clippy with -D warnings and workspace tests Passed:
  **142 passed, zero failed/ignored**; existing target directory reused.
- Pinned clang-format **18.1.8**: new test dry-run and all modified production hunk checks Passed.
  Existing unrelated native-protocol wrapping was preserved rather than reformatted.
- Scoped clang-tidy on both native protocol implementations, OllamaRuntime and the new test:
  **0 compiler errors**. Anthropic 1 braces warning; Gemini 4 braces warnings in the transferred
  guards; OllamaRuntime 109 diagnostics; new test 0 diagnostics. Total 114 diagnostics:
  56 braces, 53 narrowing, 2 implicit multiplication widening, 2 use-std-move,
  1 unnecessary-copy-initialization. Analyzer suppressed 1958 non-user warnings; no suppressions
  added. These are recorded findings, not a warning-free static-analysis claim.
- Full sequential CTest: **164/164 executables Passed**, **235.33 seconds**, explicit -j 1 with
  isolated short profile and finite deadlines. Detailed log `m08-full-detail.log`; summary
  `m08-full.log`. Across 144 QtTest summaries: **2973 passed, 0 failed, 10 skipped**.
  **2 existing XFAILs and 81 QWARN lines** remain visible; none is promoted to acceptance.
- The ten skips cover Linux Secret Service helper, refused native credential persistence,
  unavailable typed native incremental tool assembly, four strict macOS child-process shell
  fixtures, opt-in live classification, unavailable missing-node branch because npx is installed,
  and absent model-residency lease/removal contract. XFAILs remain P3-02 D4 provider-switch
  owned-child cleanup and D5b endpoint-switch owned-child reconciliation.
- Both IPC generators --check Passed. QA schema/registry validation Passed; existing QA-01 and
  collector tests also passed in full CTest. No registry/criteria/acceptance records changed.
- Personal-path scanner **0 files flagged**; changed-source credential signature scan
  **0 findings across all 8 changed files**. `git diff --check` Passed.
- Host/toolchain: macOS 27.0.1 ARM64, Qt 6.12.0, AppleClang 21, CMake 4.4.4,
  clang-tidy 23.1.3, pinned clang-format 18.1.8, Rust/Cargo 1.98.1.

## Remaining P2 acceptance gaps

AC-P2-01/03 remain open. No live Claude/Gemini/OpenAI-compatible provider or actual model capability
has been certified. Discovery pagination remains unsupported; adding it would change requests,
sequencing/merging/cancellation behavior and needs a separately characterized feature scope.
openai-compatible participation and cloud-facade asymmetries require an explicit migration, not
an extraction side effect. StaticProviderCatalog identities remain a separately approved migration.
Legacy catalog value/header naming, LMStudioConfig naming, orphaned classifier/normalizer cleanup,
provider-owned process management and PD-D10 remain separate work. Fixture provenance is not
live capability evidence. No product decision or historical QA/acceptance status was rewritten.

## Exact changed files

- `core/include/sentinel/core/provider/protocols/anthropic_messages/AnthropicMessagesProtocol.h`
- `core/include/sentinel/core/provider/protocols/gemini_generate_content/GeminiGenerateContentProtocol.h`
- `core/src/provider/protocols/anthropic_messages/AnthropicMessagesProtocol.cpp`
- `core/src/provider/protocols/gemini_generate_content/GeminiGenerateContentProtocol.cpp`
- `core/src/runtime/OllamaRuntime.cpp`
- `tests/CMakeLists.txt`
- `tests/core/test_native_discovery_characterization.cpp`
- `docs/audits/P2_M08_NATIVE_DISCOVERY_PARSERS_RESULT.md`
