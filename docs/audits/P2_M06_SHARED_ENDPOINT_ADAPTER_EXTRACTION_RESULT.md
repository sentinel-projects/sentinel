# P2-M06 — Shared endpoint adapter extraction result

Date: 2026-10-18. Status: implemented; deterministic validation complete, changes uncommitted.
All evidence is deterministic fixture evidence, not real provider certification.
**AC-P2-01 and AC-P2-03 remain open.** PD-D10 remains unmodified and unapproved.

## Verified base and workspace

- Existing Sentinel sanitized repository verified via `git rev-parse --show-toplevel`.
- Initial HEAD/main/fetched origin/main: **`e639a0429baf35445daad84eb70a9d5e001cf082`**;
  ahead/behind `0 0`, working tree completely clean.
- P2-M05 final fix `e032ed4f` is an ancestor of main; M01–M05 reports and architecture,
  Agent Runtime, security, building/testing instructions inspected. P3-05A is also integrated;
  its reference contracts do not authorize a lease implementation here.
- Created `refactor/p2-m06-shared-endpoint-adapter` from reconciled main without overwriting a branch.
- Host macOS ARM64, Qt 6.12, AppleClang 21; approximately 88 GiB free initially.
  Personal-Brain `brain` was unavailable on PATH; used the explicitly supplied, verified checkout.
- One coding agent; existing `build/tests` and `cli/target` only. No clone, worktree, repository
  copy, independent build tree, commit, push, merge, reset, stash, rebase, clean, PR or workflow
  dispatch. Changes remain uncommitted for maintainer review.

## Before and after architecture

Before: ModelService.cpp's anonymous namespace contained HealthTrackedProvider, shared tool/error
helpers, nativeEndpointReply and SelectedEndpointChatProvider, alongside selection/binding/configuration
and capability/catalog/health orchestration. The adapter captured ModelBinding, LMStudioConfig and
timeout by value. Its apparent ModelService dependency was translation-unit co-location, not an
application-service reference. The Gemini tool-declaration helper was publicly declared in
ModelService.h but depended on the anonymous nativeToolDefinition helper.

After: `core/src/provider/SelectedEndpointChatProvider.cpp` contains that same private adapter,
its request-only helpers, nativeEndpointReply and unchanged diagnostic logging category. Its
public header provides `makeSelectedEndpointChatProvider(ModelBinding, LMStudioConfig, int)`
returning `std::shared_ptr<IChatProvider>`, plus the existing geminiFunctionDeclaration symbol.
Only forward declarations and Qt JSON/shared_ptr types are required in that header. The adapter
compiles without ModelService.h, AppSettings, IModelRouter or a health registry dependency.
ModelService.h re-exports the helper's new authoritative declaration to preserve existing callers;
there is no second helper implementation or changed namespace/signature.

ModelService still owns registry-driven factory composition and overrides, provider selection,
binding resolution, captured endpoint/credential configuration, capability/provenance merging,
health and catalog orchestration. HealthTrackedProvider stays there and wraps the returned adapter
exactly as before. No new runtime registry, identity table, provider selection or fallback is added.

The adapter still delegates to **LMStudioLocalInferenceClient / LMStudioLocalInferenceStreamClient**,
ProviderRequestRuntime and the existing OpenAI-compatible, Anthropic and Gemini protocol modules.
LMStudioConfig's compatibility name/layout and injection seam are unchanged. No per-vendor HTTP
engine, new discovery parser, provider-management API or transport layer was introduced.

### Dependency audit and retained contracts

| Dependency | Role after extraction |
| --- | --- |
| IChatProvider / ChatRequestOptions / ToolDescriptor | Existing interface, tools, images, continuation and structured request contract |
| ModelBinding / ModelCapabilities | Value snapshot for provider/model/capability checks, not an active-use lease |
| LMStudioConfig / LocalInference | Captured endpoint, credential and injected network factory; existing host/protocol dispatch and streaming clients |
| ProviderRequestRuntime | Existing completion classification, deadlines, cancellation/retry behavior |
| Protocol modules / ChatImageContent | Existing request builders, native response parsing and image encoding |
| Qt JSON, crypto, regex, elapsed/date time, logging | Existing serialization, tool-name normalization, context estimates and allowlisted diagnostics |
| ModelService / ProviderRegistry | Composition callers only; no adapter dependency on their implementations |

## Behavior equivalence evidence

Before extraction, added `test_endpoint_adapter_characterization` and ran it through actual
ModelService factories with ScriptedProviderTransport. Eighteen golden request rows cover every
non-Ollama built-in identity, facade routing (including openai-compatible and unknown-vendor legacy
fallback), dynamic local endpoint overrides and `/v1` path handling. They assert exact compact
request **bytes**, URL, HTTP method, authentication headers, reply text/lifecycle and resulting
provider health. One custom-factory case verifies normalized override, frozen binding/provider
and unchanged identity count. Initial fixture assumptions were corrected *before extraction*:
ModelService local endpoints must be composed through its setters, and Qt records the Bearer
header using lowercase `authorization`. That first new-fixture run reported 8 passes and 13
assertion failures (the other six executables passed); after correcting fixture composition and
header lookup, the full baseline below passed. Production behavior was not changed to satisfy fixtures.

The corrected pre-extraction run passed **7/7 executables, 412 QtTest passes, 0 failures/skips**,
4.15 s: provider_contracts 205, provider_registry 7, new characterization 21, native_provider_contract
78, OpenAI-compatible protocol 57, Anthropic protocol 19 and Gemini protocol 25.
Existing suites already characterize native tool/schema serialization and continuation, malformed
responses, provider/status errors, missing credentials, unsupported capabilities, cancellation,
timeout, stream parsing, network-policy refusal and redaction. Their assertions were not weakened.

After extraction, the identical eighteen golden rows and custom-factory case run unchanged.
Four extra adapter-level streaming cases exercise fragmented SSE success, cancellation before
dispatch (zero requests), a held-open reply deadline, and malformed SSE. These are additional
post-extraction coverage, not falsely described as newly captured before-extraction cases.
Successful streaming asserts both emitted fragments and final text, completed lifecycle and
bounded-streaming metadata through the production factory/wrapper/adapter path.

The entire moved helper/class region was compared to base source using a lexical token sequence
that preserves quoted-string contents while ignoring comments/outside whitespace: **equal**.
Token-sequence SHA256: `8132d75f49095bb6557cede5a26e74b734d6ec4d4ac67a179b454ad0d3e3aebb`.
The relocated geminiFunctionDeclaration implementation is byte-identical. This mechanical check
supplements golden tests; it is not live provider acceptance or a C++ formal equivalence proof.

## Provider compatibility matrix

| Identity | Execution / endpoint / authentication preserved | Scope evidence |
| --- | --- | --- |
| ollama | Dedicated OllamaChatProvider unchanged; not redirected to shared adapter | Existing Ollama/runtime/registry suites |
| lm-studio | Shared OpenAI-compatible local; dynamic loopback endpoint, `/v1` suffix handling | Exact-byte golden override row, provider contracts |
| llama-cpp-server | Shared OpenAI-compatible local; dynamic endpoint, no process ownership inferred | Exact-byte golden override row, ownership tests unchanged |
| openai-compatible-local | Shared local, fixed `http://127.0.0.1:8000` | Golden request row |
| openai | Shared cloud; api.openai.com, Bearer | Golden direct/facade rows |
| openai-compatible | Existing OpenAI alias/config/credential; discovery remains None | Golden direct/facade rows; registry unchanged |
| claude | Native Anthropic Messages, x-api-key and anthropic-version; native tools/continuations | Golden direct/facade rows, existing native byte/continuation fixtures |
| gemini | Native generateContent; existing query-key chat auth vs native path auth unchanged | Golden direct/facade rows, existing signature/continuation/structured fixtures |
| deepseek | Shared cloud; api.deepseek.com, Bearer | Golden direct/facade rows |
| groq | Shared cloud; api.groq.com/openai base, Bearer | Golden direct/facade rows |
| mistral | Shared cloud; api.mistral.ai, Bearer | Golden direct/facade rows |
| cloud-api | Selected vendor facade; unknown-vendor construction still falls back to OpenAI | Golden vendor/fallback rows; no new fallback introduced |

Static declarations/provenance and runtime override semantics remain in ProviderRegistry/ModelService.
No provider default is promoted to model-specific or live-verified support. Native tools, structured
output, combined capability checks, context estimates, output budgets, tool IDs/continuation history,
error precedence, bounded streaming and request timeouts are moved intact. Active requests still
hold their captured binding/config/provider; new selection does not rewrite existing objects.

## Security boundaries

No credential accessor/storage, network permission, endpoint allowlist, redirect/TLS policy, security
gateway, IPC authorization or approval rule changed. Diagnostic category and allowlisting remain
unchanged; no prompts/keys/upstream payloads were added to logging. Fixture credentials are synthetic
and transports replace only network IO after production policy checks. No paid/live API calls or
model downloads were performed. The adapter owns no ProcessExecutor or serving-process handle;
an HTTP endpoint never grants process ownership. No PD-D10 rule, lease, unload, process cleanup or
automatic model switching was implemented. No approved acceptance/decision record was edited.

## Validation results

| Gate | Actual result |
| --- | --- |
| Configure | `CCACHE_DISABLE=1 cmake --preset tests` passed, existing build directory |
| Full C++ build | Full-target build passed; final full-target incremental rebuild after formatting also passed |
| Pre-extraction baseline | 7/7 executables, 412 QtTest passes, 0 failures/skips; 4.15 s |
| Post-extraction focused gates | **12/12 executables**, 473 QtTest passes, 0 failures/skips; 9.49 s |
| New characterization suite | 25 passes including lifecycle hooks: 18 golden rows, custom override, 4 streaming outcomes |
| Full sequential CTest | **157/157 executables passed**, 0 failures, 220.73 s, explicit `-j1`; 142 QtTest suites: 2,873 passes, 0 failures, 9 skips; 2 preserved XFAILs |
| CI clang-format 18.1.8 | New module/header/test dry-run `--Werror` passed; ModelService modified-hunk formatting diff empty |
| Scoped clang-tidy | Final adapter, ModelService and new test analyzed; 0 compiler errors and 0 new-test diagnostics; 63 existing moved adapter brace warnings, 33 retained ModelService warnings (30 braces, 2 move-const, 1 value-param); no suppressions or unrelated fixes |
| IPC consistency | Both generators passed `--check`; no schema changes |
| Rust 1.94 compatibility | fmt, locked/offline Clippy with `-D warnings` and workspace tests passed; 142 tests, 0 failures/ignored, 8 test/doc-test suites |
| Privacy / credentials | Personal-path scan 0 flagged; all 9 candidate files scanned, 0 high-confidence private-key/token findings |
| Diff whitespace | `git diff --check` passed |

Focused QtTest totals: static_model_router 8, static_provider_catalog 7, runtime_provider 8,
provider_contracts 205, provider_registry 7, new characterization 25, native_provider_contract 78,
OpenAI-compatible protocol 57, Anthropic protocol 19, Gemini protocol 25, bounded_streaming_contract
18, model_lifecycle_ownership 16. Counts include QtTest lifecycle hooks.

The initial full build and final incremental build each emitted the same pre-existing
HuggingFaceModelSource.cpp shadow warning. Scoped analysis uses the existing helper to strip PCH flags
in a temporary analysis database, not another build tree. The 96 retained/moved lint diagnostics
are unchanged legacy style/performance findings, not newly suppressed or resolved by extraction.
The analyzer also suppressed 1,511 non-user warnings. Test logs and the final full-suite summary
are retained in the existing build's `Testing/Temporary/LastTest.log`; task logs use the
`/tmp/sentinel-p2m06-*.log` prefix. No Fedora KDE/Wayland, Windows or live model certification was
performed on this macOS host; deterministic fixtures do not close acceptance rows.

The nine explicit skips are: Linux Secret Service helper on macOS (1), unavailable native
incremental tool-assembly contract (1), strict macOS child-process fixtures (4), opt-in live
classification (1), missing-node branch unavailable because npx is installed (1), and D6's missing
physical residency/deletion contract (1). D4/D5b remain the two expected failures; neither was
converted to production success. The full-suite log contains 72 QWARN diagnostic lines, not test
failures. No ownership acceptance or native platform acceptance was inferred from these counts.

## Remaining work and Proposed P2-M07

The shared adapter still contains common tool-schema/name conversion, native continuation orchestration,
OpenAI-compatible native request assembly/validation and request-specific reply classification.
LocalInference still hosts common HTTP/protocol dispatch and stream transport. ModelService retains
its orchestration/health wrapper; discovery coordination/parsers stay in existing owners. The legacy
StaticProviderCatalog metadata aliases are unchanged. Discovery's openai-compatible exclusion and
facade unknown-vendor asymmetry are still intentional compatibility boundaries, not fixes here.

Proposed P2-M07: characterize and extract remaining pure OpenAI-compatible native request/schema/
continuation operations into its existing protocol module, without changing transport or exposing
vendor engines. Separate native discovery-parser extraction and catalog identity reconciliation
require their own golden error/catalog fixtures and migration decision. Do not rename LMStudioConfig,
enable new discovery requests, adopt PD-D10, or claim provider modularization complete as part of
that pure extraction. AC-P2-01/AC-P2-03 require separately authorized real backend/model certification.

## Exact changed files

- `core/include/sentinel/core/provider/SelectedEndpointChatProvider.h`
- `core/src/provider/SelectedEndpointChatProvider.cpp`
- `core/include/sentinel/core/model/ModelService.h`
- `core/src/model/ModelService.cpp`
- `core/CMakeLists.txt`
- `tests/core/test_endpoint_adapter_characterization.cpp`
- `tests/CMakeLists.txt`
- `docs/development/ARCHITECTURE.md`
- `docs/audits/P2_M06_SHARED_ENDPOINT_ADAPTER_EXTRACTION_RESULT.md`
