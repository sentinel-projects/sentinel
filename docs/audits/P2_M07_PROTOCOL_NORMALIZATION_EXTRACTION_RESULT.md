# P2-M07 — OpenAI-compatible protocol normalization extraction

Date: 2026-10-18. Behavior-preserving refactor; deterministic fixture evidence only.
AC-P2-01 and AC-P2-03 remain open for separately authorized live-provider acceptance.

## Verified Git base and workspace

Existing supplied Sentinel checkout, origin `sentinel-projects/sentinel`. Fetched origin and verified
clean main == origin/main == `d4e8da6a362c62211bb1d54ba79395cfdc229c38`. P2-M06 merge `add5b071` and
completed QA-02A through `28e5f7b7` are ancestors. Root AGENTS.md, architecture, Agent Runtime,
security/build/testing instructions and P2-M01–M06 reports were inspected. Personal-Brain was not
available on PATH; the supplied checkout was verified instead of guessing another workspace.
Only this coding agent was active; the other loaded checkout chat was idle. Created the absent
`refactor/p2-m07-protocol-normalization` branch from verified main.

Reused `build/tests` and `cli/target`. No clone, worktree, duplicate checkout, new build tree,
commit, push, merge, reset, rebase, stash, clean, PR or workflow dispatch. The temporary analysis-only
compile database strips production PCH flags using the existing helper; it is not a build tree.
All changes remain uncommitted. Subsequent Git identity checks retained the exact base and branch.

## Inventory and dependency boundaries

Before: SelectedEndpointChatProvider executed requests and also defined function-name/schema
conversion, the native-capability OpenAI body and continuation messages, first-choice/usage scanning,
and JSON object decoding. OpenAiCompatibleProtocol already owned the simpler infer/stream body,
chat path, text/raw-tool-call parsing and model catalog parsing. Anthropic/Gemini modules already
owned their request and response shapes; LocalInference and ProviderRequestRuntime owned shared
HTTP, protocol dispatch, retry/deadline/cancellation and transport status classification.

After: the existing OpenAiCompatibleProtocol gains these pure functions, with no new HTTP engine:

| Extracted function | Contract |
|---|---|
| `openAiCompatibleFunctionName` | Existing 1–64 character ASCII name rule; otherwise `tool_` plus first 48 SHA-256 hex digits |
| `openAiCompatibleToolDefinition` | Existing schema passthrough or parameter fallback, required list and 200-character description bound; same shared envelope used by native declaration bridges |
| `buildOpenAiCompatibleNativeRequest` | Caller-provided encoded content, messages/tool-result continuation, compact arguments, 6800-character observation bound, visible/enabled declarations, structured schema envelope and unchanged validation messages/order |
| `normalizeOpenAiCompatibleNativeResponse` | First-choice content, raw tool-call parts, output-limit flag and allowlisted finish/usage diagnostics; no tool execution or registry lookup |
| `parseOpenAiCompatibleJsonObject` | Object-only JSON decoding for argument strings and structured text; no schema-validation claim beyond the previous object check |

Request options and descriptors remain the authoritative existing value contracts. No parallel
options/tool schema type or runtime registry was introduced. The existing raw-tool-call parser
remains the single scanner used by response normalization. The old adapter implementations were
removed, not duplicated. Native body serialization remains distinct from the simpler infer/stream
profile because its established optional fields and token-budget ownership differ.

Retained in the adapter: provider/model/config snapshots, endpoint/host routing, capability and
combined-capability gates, image admission/encoding calls, exposed-tool registration policy,
context/output budgets, request execution, cancellation/deadlines, transport/error precedence,
lifecycle/health metadata, diagnostic timing/logging, accepted reply construction and native
Claude/Gemini continuation/signature orchestration. ModelService, ProviderRegistry,
ProviderRequestRuntime, LocalInference and StaticProviderCatalog implementations are unchanged.

## Characterization and equivalence evidence

Before changing production code, extended the existing endpoint characterization suite with three
exact request-byte goldens and seven response edge rows, then ran the 12 focused suites:
**483 QtTest passes, 0 failures, 0 skips; 12/12 executables, 5.37 seconds**.

The new byte goldens freeze fallback tool schema/hidden declaration omission, strict structured JSON,
and continuation argument encoding/role/ID/6800-character truncation plus output-budget clamping.
They also verify POST, URL, completed lifecycle and resulting provider health through ModelService.
The seven edge rows freeze whitespace IDs, object-valued rather than string-encoded arguments,
length rejection before tool validation, empty/non-object choices, invalid structured arrays and
allowlisted diagnostics. The legacy OpenAI response lookup uses supplied descriptors without
filtering exposure; this remains unchanged and is explicitly characterized, not newly authorized.
Tool execution still requires the authoritative gateway and permissions.

All eighteen P2-M06 provider/facade request-byte goldens, factory override/frozen binding test and
four streaming outcome cases remain intact. Existing provider/native protocol suites supplied the
baseline for malformed names/arguments, missing IDs, missing/empty choices, schema failures,
authentication, HTTP error precedence, context limits, cancellation, deadlines and vendor behavior.
Streaming/native structured or tool output remains rejected; ordinary streamed text/body contracts
remain covered by the existing streaming/protocol fixtures. No streamed native-tool equivalence is
claimed where the implementation explicitly does not support it.

Post-extraction added ten direct pure-protocol cases and one adapter preflight guard case. These
additional checks were not part of the pre-extraction execution claim. They cover hashed name and
length boundaries, schema fallback/passthrough, exact minimal body, no protocol-owned cancellation
or budget policy, continuation error order/orphan-result compatibility, missing/duplicate opaque IDs,
Unicode content byte counts, object-only JSON decoding, unsupported combined capabilities and zero
transport on invalid continuation/schema or structured/tool streaming requests.

Post-extraction focused execution: **494 QtTest passes, 0 failures, 0 skips; 12/12 executables,
9.66 seconds**. Counts include QtTest lifecycle hooks. The only later C++ edits were braces around
four new thin-wrapper control-flow statements, followed by a full rebuild and full-suite execution.

Mechanical checks against the exact base:

- Shared function-name/schema helper token sequences match after symbol renaming; token SHA-256
  `321a213ef3d69c2c848cf6032942a57a3b11ece8566fa5e739893e8e78882670`.
- Budget, diagnostic timing, transport and HTTP error-precedence block is token-identical.
- Native Claude/Gemini orchestration is token-identical after qualifying the relocated helpers.
- Simple Chat and streaming regions are token-identical.

These checks supplement golden tests; they are not formal equivalence or live provider certification.
Local evidence is under ignored `build/tests/p2m07-*` logs and `p2m07-equivalence.json`.

## Provider compatibility matrix

| Identity | Preserved behavior / evidence |
|---|---|
| `ollama` | Dedicated native path; unchanged registry/runtime tests |
| `lm-studio` | Dynamic local endpoint, OpenAI native tools/structured/continuations; exact new and P2-M06 bytes |
| `llama-cpp-server` | Same shared adapter and local protocol; frozen binding and provider contracts; no process ownership added |
| `openai-compatible-local` | Fixed loopback endpoint and original body; P2-M06 golden |
| `openai` / `openai-compatible` | Existing OpenAI alias, Bearer headers and endpoint; direct/facade goldens; compatible discovery still excluded |
| `deepseek` | Existing OpenAI-compatible body/endpoint/auth, direct/facade goldens |
| `groq` | Existing `/openai/v1` URL handling and auth, direct/facade goldens |
| `mistral` | Existing endpoint/auth/body, direct/facade goldens |
| `claude` | Native Messages path, headers, schemas, continuation history and response validation unchanged; native/golden suites |
| `gemini` | Native generateContent, existing query/header credential conventions, schema/signature/history/ID rules unchanged; native/golden suites |
| `cloud-api` | Selected-vendor facade and legacy unknown-vendor OpenAI fallback unchanged; custom factory override unchanged |

Native and OpenAI continuation policies intentionally differ. OpenAI call IDs are neither synthesized
nor trimmed; missing IDs are still rejected by the adapter. Native Gemini synthesized IDs and
signature policies remain in their existing owners. No catalog identity or discovery migration was
hidden in this refactor.

## Security impact

No new authorization, tool execution, endpoint/network admission, credential access/storage,
redirect/TLS, process/lease ownership or model lifecycle rule. Function name conversion is wire
serialization, not a permission grant. Descriptor registration and capability checks stay outside
pure helpers. Diagnostics retain only existing numeric/status allowlists; no prompt, tool argument,
provider payload or credential was added to logging. Synthetic credentials and injected transports
were used; no live API, real credential, model download or paid call occurred. No LMStudioConfig
rename, discovery participation change, pending decision adoption or acceptance promotion.

## Validation

- Existing build configuration regenerated successfully; complete C++ build and final incremental
  full-target rebuild passed with `CCACHE_DISABLE=1`. Final builds: 0 compiler errors/warnings.
- Pinned clang-format **18.1.8** dry-run with `--Werror`: all five changed C++ files Passed.
- Scoped clang-tidy: both production sources and both changed test sources analyzed; 0 compiler
  errors. Production: 54 adapter and 19 protocol `readability-braces-around-statements` warnings,
  retaining existing/moved style. Tests: 5 existing OpenAI test warnings (2 static-initialization,
  2 unchecked optional access, 1 braces); endpoint characterization and new pure tests: 0 diagnostics.
  No suppression or unrelated cleanup. Final production run suppressed 953 non-user warnings;
  test run suppressed 1088 non-user warnings. Logs retain the individual locations/messages.
- Rust fmt, locked/offline workspace/all-target Clippy with `-D warnings`: Passed. Locked/offline
  workspace tests: 142 passed, 0 failed/ignored; 8 test/doc-test suites, including zero-test targets.
- Both IPC generator `--check` commands: Passed; no IPC schema changed.
- Personal-path scan: 0 flagged files. Credential signature and final whitespace checks follow below.

Two fixture-authoring build errors occurred before production extraction: an incorrect health enum
name (`Healthy`, corrected to the existing `Available`) and Qt moc failing to discover the test
class when the new raw JSON fixture bodies were inline. Moving those slot bodies out of the class
resolved moc/vtable generation. The corrected baseline had no assertion failures; no production
behavior was changed to satisfy either fixture error.

Final full sequential CTest: **160/160 executables passed, 0 failed; 228.09 seconds**, explicit
`-j 1`, finite test deadlines and a short disposable profile. The 142 QtTest summaries report
2895 passes, 0 failures and 10 skipped cases, including lifecycle hooks.
Detailed output preserves **10 SKIP incidents, 2 XFAIL incidents and 74 QWARN diagnostic lines**.
The skipped contracts are Linux Secret Service helper/native credential smoke availability,
incremental native tool assembly, four strict macOS shell-child fixtures, opt-in live classification,
unavailable missing-node branch and the absent model-residency lease/removal contract. D4 provider
switch owned-child cleanup and D5b endpoint switch cleanup remain the two expected failures.
No skip or XFAIL is promoted to accepted production behavior. Warnings are diagnostic output,
not additional test failures. Raw logs stay in existing ignored build outputs.

Final privacy/credential/whitespace gates: 0 personal paths, 0 high-confidence private-key/token
signatures across all 7 changed candidate files, and `git diff --check` Passed. Host/toolchain:
macOS ARM64, Qt 6.12.0/AppleClang 21, CMake 4.4.4, clang-tidy LLVM 23.1.3,
Rust/Cargo 1.98.1; formatter pinned separately to 18.1.8. This host run does not certify native
Fedora KDE/Wayland, Windows IPC or any real provider/model.

## Remaining migration work and candidate P2-M08

OpenAI-compatible discovery participation remains exactly as registered, including the existing
`openai-compatible` exclusion. Native Anthropic/Gemini catalog parsing and discovery coordination
remain in their existing owners. StaticProviderCatalog aliases and the facade unknown-vendor
asymmetry remain unchanged; reconciliation requires separately approved identity migration and
compatibility fixtures.

Candidate P2-M08: first characterize native discovery catalog success/empty/malformed entries,
pagination and error classification, then extract only pure Anthropic/Gemini catalog parsing into
the existing protocol modules. Keep dispatch, authentication, cancellation, transport, health/catalog
sequencing and capability provenance in their current services. Do not expand discovery fan-out,
rename LMStudioConfig, alter catalog aliases or implement model leases/process ownership as part
of that extraction. Real-model acceptance for AC-P2-01/03 remains separately authorized work.

## Exact changed files

- `core/include/sentinel/core/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.h`
- `core/src/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.cpp`
- `core/src/provider/SelectedEndpointChatProvider.cpp`
- `tests/core/test_endpoint_adapter_characterization.cpp`
- `tests/core/test_openai_compatible_protocol.cpp`
- `docs/development/ARCHITECTURE.md`
- `docs/audits/P2_M07_PROTOCOL_NORMALIZATION_EXTRACTION_RESULT.md`
