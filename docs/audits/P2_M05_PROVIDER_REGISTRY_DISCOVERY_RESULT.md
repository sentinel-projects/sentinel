# P2-M05 — Provider registry and discovery architecture result

Date: 2026-10-18. Status: implemented; final validation in progress. Changes are uncommitted.
AC-P2-01 and AC-P2-03 remain open without authorized live backend/model verification.

## Verified base and workspace

- Base/main/fetched origin/main: `8085a9ed2e689d44f576d7dced663dd73241c9b7`; ahead/behind `0 0`.
- Clean initial working tree. P2-M02, M03 and M04 commits/reports were verified on main; M01–M04
  audits and architecture/runtime/security/build/test instructions inspected.
- Created `refactor/p2-m05-provider-registry` from main. The previously committed P7 branch is
  preserved; its unmerged packaging changes were not imported into this task.
- Host: macOS ARM64, Qt 6.12.0, AppleClang 21; approximately 88 GiB initially available.
  Personal-Brain's `brain` was unavailable on PATH; the explicitly supplied repository was verified.
- One coding agent; existing `build/tests` reused. No clone, worktree, source duplication,
  additional build tree, commit, push, merge, rebase, reset, stash, clean, PR or workflow dispatch.
- An external actor switched to main during the focused build. Reflog and file inventories proved
  this was a tree-neutral checkout: both branches remained at the exact base SHA and only the seven
  expected implementation/test files were modified/untracked. The requested branch was safely
  reselected without changing contents. Subsequent identity checks remained stable. This does not
  substitute for maintainers serializing access to the shared checkout.

## Actual inventory

The current source registers **11 backend IDs plus the `cloud-api` compatibility facade**, twelve
ModelService factory entries total. There are still two runtime adapter boundaries: dedicated
OllamaChatProvider and the file-local SelectedEndpointChatProvider. Registry protocol tags describe
the existing implementation; they do not add independent provider modules or select a new transport.

| ID / public identity | Protocol family | Endpoint configuration | Current discovery strategy | Adapter |
| --- | --- | --- | --- | --- |
| `ollama` / Ollama | Native Ollama | Existing Ollama endpoint/config | Native `/api/tags` | OllamaChatProvider |
| `lm-studio` / LM Studio | OpenAI-compatible | Existing configurable LM Studio endpoint | `/v1/models`, local | SelectedEndpointChatProvider |
| `openai-compatible-local` / OpenAI-compatible local endpoint | OpenAI-compatible | Fixed `http://127.0.0.1:8000` | `/v1/models`, local | SelectedEndpointChatProvider |
| `llama-cpp-server` / llama.cpp server | OpenAI-compatible | Existing configurable llama.cpp endpoint | `/v1/models` plus existing optional `/props` metadata | SelectedEndpointChatProvider |
| `cloud-api` / Cloud API | Selected-cloud facade | Existing selectedCloudProvider and credentials | Selected vendor; unknown vendor means no catalog request | SelectedEndpointChatProvider |
| `openai` / OpenAI | OpenAI-compatible | `https://api.openai.com`, existing key | `/v1/models`, cloud | SelectedEndpointChatProvider |
| `openai-compatible` / OpenAI-compatible | OpenAI-compatible | Same OpenAI config/key | **None in current poll coordinator**, preserved | SelectedEndpointChatProvider |
| `claude` / Claude | Anthropic Messages | `https://api.anthropic.com`, existing key | Native Anthropic model fetch | SelectedEndpointChatProvider |
| `gemini` / Gemini | Gemini generateContent | `https://generativelanguage.googleapis.com`, existing key | Native Gemini model fetch | SelectedEndpointChatProvider |
| `deepseek` / DeepSeek | OpenAI-compatible | `https://api.deepseek.com`, existing key | `/v1/models`, cloud | SelectedEndpointChatProvider |
| `groq` / Groq | OpenAI-compatible | `https://api.groq.com/openai`, existing key | `/openai/v1/models`, cloud | SelectedEndpointChatProvider |
| `mistral` / Mistral | OpenAI-compatible | `https://api.mistral.ai`, existing key | `/v1/models`, cloud | SelectedEndpointChatProvider |

The M01 discovery table included `openai-compatible` in cloud polling, but the actual current
pollOllama predicate excludes it. This refactor deliberately preserves that source behavior rather
than enabling an extra network request. Likewise an invalid selected cloud uses the legacy OpenAI
endpoint fallback for construction but **no** discovery branch. These are compatibility asymmetries,
not newly declared provider support.

## Descriptor architecture and migrated responsibilities

`ProviderRegistry` owns an immutable QList of ProviderRegistration descriptors, with const access
and pure exact-ID lookup. Descriptors carry ID/name, local/cloud kind, protocol, endpoint category,
adapter identity, discovery identity, static base URL where applicable, and declared ModelCapabilities
with existing provenance. Duplicate, empty or noncanonical descriptor IDs throw invalid_argument.
Built-ins initialize once, retain historical registration order, and require no network/settings access.

This is the single built-in registration metadata table, not another executable factory owner.
ModelService's existing factory map remains authoritative for constructed providers and composition-root
overrides/custom providers. Its public registerProvider replacement semantics are unchanged; duplicate
descriptor rejection does not silently remove the existing runtime override seam.

ModelService now delegates built-in enumeration/defaults, adapter choice, cloud membership,
cloud-key catalog invalidation and static endpoint/facade identity lookup. It still orchestrates
selection, binding, credential retrieval, dynamic local endpoint values, network-factory injection,
capability merging and provider health/catalog state. LMStudioConfig was not renamed or replaced.
Raw providerConfig unknown/case-mismatched IDs keep the legacy LM Studio fallback; normal binding
resolution still rejects unknown IDs. Ollama construction still uses OllamaConfig and its own endpoint;
the legacy direct providerConfig(ollama) fallback remains unchanged and is characterized.

The old StaticProviderCatalog remains a routing/catalog placeholder surface (including its older
metadata aliases), not a second source of executable built-in registrations. Its availability,
model metadata and routing behavior were not changed. Unifying that presentation/catalog surface
needs a separately characterized migration.

## Discovery coordination

ApplicationController::pollOllama uses descriptor discovery/endpoint categories instead of repeated
provider membership and facade vendor predicates. Ollama health/tag polling, selected-provider
fan-out, credential accessors, exact URLs, request ordering, local/cloud timeouts, cancellation,
generation fencing and result acceptance remain in their existing owners. The worker's settings
snapshot behavior was not changed. No new catalog parser, management API, live probe or request
execution path was introduced. Existing catalog parsers remain in their protocol/runtime modules.
An HTTP endpoint does not imply process ownership; no process lifecycle policy moved into the table.

## Capabilities and compatibility evidence

All twelve descriptors preserve streaming Supported. Ordinary entries preserve native tools and
structured output Unsupported; Claude/Gemini preserve native tools Supported and structured output
Unknown. The facade's existing selected-native handling remains in ModelService. Vision, audio,
combined-tool/structured support and numeric limits retain Unknown/unset defaults.

Known static values carry ProviderDefault; unknown values retain Unknown provenance. ProviderCatalog,
ProviderOwnedMetadata, RuntimeReported, ProgrammaticMetadata and UserOverride merge behavior is
untouched. No default is promoted to tested/live-verified capability, no new confidence category is
invented, and provider defaults do not certify every model. Runtime-reported metadata is not treated
as authorized live acceptance merely because its provenance enum exists.

Two deterministic service characterization cases were added and run before migration, then retained:
sorted registered IDs, defaults/provenance, persisted normalized selection, unknown-provider errors,
local/cloud endpoint mapping, facade mapping and credential selection. Three further registry cases
cover immutable order/ID/protocol/adapter lookup, duplicate rejection versus factory override, every
discovery strategy and facade edge cases. Existing provider contracts cover malformed/missing catalogs,
empty/stale catalogs, frozen model binding, authentication/network restrictions, unsupported capability
rejection and error categories; parser and request code was not moved in this task.

## Validation results

| Gate | Actual result |
| --- | --- |
| Pre-migration characterization | 2 service cases passed against original ModelService |
| Pre-migration provider/native/protocol baseline | 5/5 CTest executables passed |
| Initial post-migration focused run | Registry, provider and native contract executables: 3/3 passed |
| Clean configure | Existing directory, `CCACHE_DISABLE=1 cmake --fresh --preset tests` passed; initial wrapper-cache write denial corrected by disabling ccache |
| Full C++ build | Pending final result |
| Final provider/protocol/catalog gates | Pending final counts |
| Full sequential CTest | Pending final count; explicit `-j 1` |
| CI clang-format 18.1.8 | New C++ files formatted and modified-hunk diff checked |
| Scoped clang-tidy | Registry and ModelService configured analysis completed; controller analysis and final counts recorded after completion |
| Generated IPC | Both Python generators passed `--check` |
| Rust 1.94 fmt/Clippy/tests | Pending final results; no Rust or IPC schema change |
| Privacy | 0 personal-path findings |
| Credentials / whitespace | Final candidate scan and `git diff --check` recorded after completion |

All test requests use deterministic fixtures or isolated local transports; no authorized live vendor
or model acceptance was attempted. Native Fedora/KDE, Windows and live backend behavior are not
certified by the macOS tests. No credentials, model download or provider permission was introduced.

## Remaining work and P2-M06

SelectedEndpointChatProvider remains file-local in ModelService and still owns protocol execution,
streaming, tool resolution, continuations, structured-output validation and error mapping. ProviderRequestRuntime,
IChatProvider and IModelRouter retain their contracts. Endpoint authentication remains in existing owners.
Independent adapter modules, native catalog parser extraction, the remaining primary OpenAI-compatible
request/response normalization, and legacy catalog identity reconciliation remain incremental follow-ups.

P2-M06 should extract the shared endpoint adapter behind the existing factory seam, with frozen
request/response/error/cancellation fixtures, then split provider-specific discovery parsing only when
behavior is characterized. Decide the openai-compatible polling discrepancy separately; do not hide
it inside extraction. LMStudioConfig migration, provider-owned process lifecycle, CSEC policy and
PD-D10 remain separate decisions. Registry existence does not complete provider modularization or
close AC-P2-01/AC-P2-03.

## Exact changed files

- `core/include/sentinel/core/provider/ProviderRegistry.h`
- `core/src/provider/ProviderRegistry.cpp`
- `core/src/model/ModelService.cpp`
- `core/src/app/ApplicationController.cpp`
- `core/CMakeLists.txt`
- `tests/core/test_provider_registry.cpp`
- `tests/CMakeLists.txt`
- `docs/development/ARCHITECTURE.md`
- `docs/audits/P2_M05_PROVIDER_REGISTRY_DISCOVERY_RESULT.md`
