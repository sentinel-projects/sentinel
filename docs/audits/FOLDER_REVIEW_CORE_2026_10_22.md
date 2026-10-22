# Core folder review

Date: 2026-10-22. Candidate `2cc7a301a991e031ded9c7b8f4a7cd6e21e6e2b1`,
branch `fix/26-release-manifests`. Personal-Brain CLI unavailable. Read repository
instructions, architecture, Agent Runtime, security model, building and testing
before this pass. User authorized folder discovery and direct GitHub recording.
Existing six untracked audit reports preserved. No product fixes, commit, push,
release acceptance, live provider calls or personal-profile operations.

## Findings recorded

| Issue | Finding | Reachability / evidence |
| --- | --- | --- |
| [#93](https://github.com/sentinel-projects/sentinel/issues/93) | Partial storage migration rewrites references to files it leaves in the old root | Active startup helper, actual-source temporary-root reproduction |
| [#94](https://github.com/sentinel-projects/sentinel/issues/94) | Truncated tool outputs overwrite earlier artifacts and advertise failed writes | Active Agent observation path, actual-class burst and failed-write probes |
| [#95](https://github.com/sentinel-projects/sentinel/issues/95) | Full-output recovery points outside filesystem authorization | Active Agent path, actual gate/truncator boundary probe and composition inspection |
| [#96](https://github.com/sentinel-projects/sentinel/issues/96) | AsyncQueue worker accesses a destroyed owner | Compiled service with no production call site found; ASan reproduction |
| [#97](https://github.com/sentinel-projects/sentinel/issues/97) | Queue signals deadlock reentrant public operations while holding their mutex | AsyncQueue/BackgroundJobService API probes; production wiring not found |
| [#98](https://github.com/sentinel-projects/sentinel/issues/98) | Todo snapshot restoration leaves its allocator at 1 and overwrites restored items | Compiled service with no production caller found; actual-class reproduction |

[#68 evidence comment](https://github.com/sentinel-projects/sentinel/issues/68#issuecomment-6336901093)
records missing regression scenarios.
[#65 evidence comment](https://github.com/sentinel-projects/sentinel/issues/65#issuecomment-6336915196)
adds queue wiring/status distinctions. Existing issue bodies were read before
posting. No issue was closed or declared fixed.

### Partial migration

The actual internal migrateRoot helper was compiled unchanged into a scratch
translation unit and run on temporary legacy/current roots. Both roots contained
a models directory; only the legacy root contained old.gguf and a catalog pointing
to it. The current root contained a different marker model. Migration adopted the
catalog, skipped the colliding legacy models directory, and rewrote the catalog
reference to current/models/old.gguf. That path did not exist; the old file remained
intact under the legacy root. Result returned 1. This is a reference-integrity
failure, not proof of deleted model bytes or a full startup reproduction.

### Tool output publication and recovery

Actual ToolOutputTruncator, maxBytes=12, disposable root, 100 sequential same-tool
writes: six unique paths and 94 collisions, without time mocking. A regular file
used as outputDir forced publication failure; fullOutputPath was still nonempty
and its target did not exist. Production AgentLoop advertises that field as full
output. The normal constructor uses a temporary directory outside the workspace.
The actual ExternalDirectoryGate denied both access and a permission request for
a separate temporary output artifact. readFullOutput has no production caller.
The issue requests an authorized artifact boundary, not a relaxation allowing all
temporary paths. No full model run or measured sensitive-data exposure was claimed.

### Queue lifetime and reentrancy

A semaphore-held actual AsyncQueue task was started, its owner deleted, and the
task released. Scratch AddressSanitizer build reported heap-use-after-free at the
completion lambda's QMutexLocker; process exit 134. Symbolizer emitted warnings,
but the report includes AsyncQueue::processNext and the freed-allocation trace.

Separate child probes connected taskStarted to runningCount and jobSubmitted to
cancelJob. Both printed slot-entered and failed to return by a two-second parent
deadline. Parent subprocess.run killed/reaped the probes. No blocked child was
left running. Both calls reacquire a nonrecursive mutex held by the emitter.

TodoService add -> snapshot -> new owner restore -> add produced one item, with
only the second content. These three service issues are latent defects: source
search found no production construction site, and no current user-task loss or
Desktop/daemon crash is asserted.

## Scope of source review

Inventory: core/src has 241 files across 56 module directories; core/include has
345 files; core/data has one model catalog. Build registration and the issue
inventory were checked. Inventory is not a claim of line-by-line review.

The detailed risk-based pass followed these boundaries:

- app/chat: controller/composition entry points, storage migration and admission,
  workspace/onboarding/settings/retention, backup refusal, conversation persistence,
  attachment extraction and Chat turn/worker/cancellation ownership;
- agent/context/runtime: AgentRuntime run binding, shutdown and subagent composition,
  AgentLoop observation/final evidence, claim grounding, execution scheduler/shared
  budget, tool gateway descriptors/authorization snapshots, file resolution and
  process ownership, output truncation and selected tool error paths;
- security: permission/grant persistence and revocation, descriptor authorization,
  canonical resource snapshots, PathGuard and external-directory policy;
- memory: archive worker ownership and context binding, transactional management,
  scope filtering, retrieval/index boundaries and supported admission limits;
- model/provider: managed storage/download publication and source identity, library
  registration, lifecycle/routing boundaries and selected endpoint cancellation;
- MCP/plugin/skill: catalog contracts, service callback/generation search, extension
  composition/requirements and hot reload ownership; fixture integration tests;
- jobs/asyncqueue/session/event: queue callbacks and lifecycle, persistence completion,
  restored IDs, export and durable-event implementation; dormant status checked;
- supporting modules: selected config substitution, resource-policy/telemetry,
  voice metadata/output control, logging, proxy, instruction and snapshot sources.

The remaining small utility/platform/prototype files received inventory/reference
coverage, not complete correctness or security verification. The data catalog was
inventoried, not independently revalidated against live model publishers. Platform
implementations were not executed on Fedora/Windows. This is a bounded core audit,
not a certification of every implementation or every header contract.

Existing #19-#23, #29-#43, #54/#56, #61/#65/#68 remain relevant. In particular,
unsupported global live-owner quiescence and coordinated backup remain deliberate
safe refusals; no issue asks to bypass them. Existing tool false-success #35,
subagent approval #21, compaction #36, provider parity #42 and lifecycle #43 were
not duplicated. Scope findings do not assert these old issues are resolved.

## Executed checks

33 existing CTest targets passed, using the existing tests build:

First group (16, 8.49 seconds): test_chat_mode_service, test_chat_attachments,
test_sqlite_conversation_store, test_context_budget_manager, test_memory_archive,
test_memory_retrieval, test_memory_management, test_sqlite_memory_store,
test_mcp_tool_provider, test_tool_execution_gateway, test_storage_async_coordination,
test_storage_admission, test_background_job_persistence, test_agent_loop,
test_agent_runtime and test_model_lifecycle_ownership.

Second group (17, 47.98 seconds): test_mcp_integration,
test_plugin_tool_integration, test_static_approval_policy, test_static_sandbox_policy,
test_provider_contracts, test_bounded_streaming_contract, test_credential_store,
test_provider_credentials, test_permission_policy_service,
test_tool_argument_validator, test_skill_extension_runtime, test_storage_migration,
test_settings_recovery_runtime, test_security_utilities, test_process_executor,
test_native_resource_telemetry and test_resource_policy.

CTest exit statuses were successful. QtTest case-level native/containment skips
remain unverified; target success does not imply every platform branch ran.
Local fixture process/socket tests ran outside the restricted sandbox.

Actual-source scratch probes were built under /private/tmp/sentinel-core-review,
with Qt 6.12, Apple Clang and CMake AUTOMOC. Initial default compiler-wrapper setup
failed because ccache attempted to write outside the permitted area; retry used
/usr/bin/clang++ directly and succeeded. Sanitizer build was limited to the scratch
harness/classes, not a full sanitized application build. No dependencies installed.

No full rebuild or complete 176-target suite was run: product source is unchanged.
No live model/API, package installation, real microphone, user credentials or
personal stores were exercised. No inference of whole-system acceptance is made
from the passing fixture tests. git diff --check passed.
