# P3-05A — Model ownership and resource lease safety contracts

Date: 2026-10-18. Status: design/reference fixtures complete; **PD-D10 remains Proposed, not Approved**.
No production lease, deletion fence, unload, reconciliation or shutdown policy is activated.
AC-P3-02 and the PD-D10-dependent P5 shutdown acceptance remain blocked.

## Verified workspace and base

- Repository: the existing Sentinel sanitized checkout, verified with `git rev-parse --show-toplevel`.
- Initial clean base/main/origin: `8085a9ed2e689d44f576d7dced663dd73241c9b7`.
  Created `design/p3-05a-resource-lease-contracts` from main, without overwriting a branch.
- External merges then advanced the checked-out branch and main. Work stopped, preserving the
  two task files. The maintainer confirmed Git operations were finished and explicitly authorized
  continuation. No external merge was performed or undone by this coding agent.
- Resumed, fetched and verified base: **`d48336d848ca136f33478ddbf5c0437767ced4dc`**;
  task HEAD, main and origin/main identical, divergence `0 0`. Only the task's CMake entry and
  reference fixture remained uncommitted. This is the base for the final validation and delta.
- P3-04 lifecycle test commit `94286252` and P5-02.1 security/persistence fix `2799a8c2`
  are ancestors of main. P3-04 report, P5 reliability/security reports and the PD-D10 proposal
  were inspected alongside repository architecture, Agent Runtime, security, building and testing.
- Host: macOS ARM64, Qt 6.12, AppleClang 21; approximately 88 GiB free. `brain` was not
  available on PATH; the user-supplied existing checkout was explicitly verified instead.
- One coding agent; only existing `build/tests` and `cli/target` outputs reused. No clone,
  worktree, source duplication, independent build directory, agent commit, push, merge,
  rebase, reset, stash, clean, PR or workflow dispatch.

## Actual ownership and access inventory

| Owner / source | Actual accesses and lifetime | Authority and missing boundary |
| --- | --- | --- |
| `apps/sentinel-daemon/service/LocalModelRuntime.cpp` | Observes provider/endpoint/model/busy; probes `/health` then `/props`; resolves local GGUF entries and starts fixed `llama-server --model` command only after refused connection; retains `processId_`, `modelId_`, probe revision and attempted key | Private ProcessExecutor-owned opaque handle only. Terminates on model change when not busy; destructor shuts down its own executor. Provider early return leaves D4; same-model endpoint change leaves D5b. `busy` is Agent-loop or Chat, not all consumers |
| `core/src/runtime/ProcessExecutor.cpp` | Creates process records/opaque IDs; start, write, record, terminate, kill, terminal callbacks, shutdown/reap | Rejects unknown or terminal handles; sandbox/process-group behavior stays here. No adoption by PID, listener, executable name or HTTP response |
| `core/src/model/ModelService.cpp` and `ModelRouting.h` | Resolve configured selection into value ModelBinding and shared provider; endpoint/auth configuration, health and catalog snapshots | C++ object lifetime, not model residency or file retention. Resolution alone is not request admission. Current registry metadata confers no lifecycle/security authority |
| `core/src/app/ApplicationController.cpp` | Resolves Agent binding before planner binding; starts runtime/controlled work, background discovery/title requests, cancellation and cooperative shutdown | Future admission/terminal integration must span real consumers, not selection-change signals or discovery alone. `tryShutdownRuntime` must remain truthful while work survives |
| `core/src/agent/AgentRuntime.cpp`, `LlmAgentRuntime.cpp` | Active run/child context, frozen binding/provider, loop terminal events, tool continuation and cooperative cancellation | Accepted AgentLoop final answer alone completes successful run. Nested work must not release a parent's lease at a step/tool completion; cancellation signal alone is not physical provider quiescence |
| `core/src/chat/ChatModeService.cpp` | Resolve, readiness/admission checks, retained provider/worker, streaming completion, stop and terminal message projection | Chat cancellation/requested UI state must not release protection before the worker has stopped touching the resource. No production physical lease exists |
| `core/src/model/ModelOperationService.cpp` | Queues pull/download/import/refresh/removal, recovery journal; RemoveRegistration calls library; RemoveManagedArtifact calls storage; Ollama Remove sends `/api/delete` | Removal is inventory/artifact mutation, not unload. No active-consumer fence. Journal cleanup also removes owned download/journal artifacts and must be classified separately from serving model deletion |
| `core/src/model/ModelStorageManager.cpp` | Tracks roots/device, reads bounded sidecars, accounts bytes; removeArtifact unlinks file then sidecar; removes orphan sidecars; reveal path | Existing ownership proof uses known root, canonical containment and repository/revision/filename-derived sidecar identity. No active-use fence or atomic transaction with admission. Partial file/sidecar failure is not rollback |
| `core/src/model/ModelLibrary.cpp` | Reads GGUF header, persists/imports local registrations, merges provider inventory, removes registration, exposes local paths | Registration removal is not necessarily file deletion. Alias/registration/path may refer to the same physical file; all aliases need a shared artifact identity before enforcement |
| `LocalInference`, `OllamaRuntime`, protocol providers | Dispatch/probe/cancel HTTP through existing transport/policy; local/provider endpoint config is captured or consulted by existing adapters | Own the request, never the server. Ollama, LM Studio, user-started llama-server and remote/cloud endpoints remain external even on loopback |
| `core/src/voice/WhisperTranscription.cpp` and speech asset catalog | Validates binary/model paths and explicit process-execution permission; voice model is distinct from selected chat model | Future consumer inventory must include speech and any embedding/internal work; a selected chat binding cannot cover a separate speech artifact or worker |
| `DaemonService.cpp` | Claims IPC endpoint before stores; starts runtime; shutdownStarted stops health/probes; destruction stops Chat/Agent before LocalModelRuntime and controller teardown | Do not duplicate daemon lifecycle or add a second process supervisor. Existing cooperative behavior is not a finite hard-stop guarantee |
| `DaemonIpcServer.cpp` | Same-user IPC authorization, generation UUID, event fencing, nonblocking drain; SQLite restart reconciliation marks interrupted running/approval sessions failed with `daemon-restarted` | Restart projections are not physical lease/process recovery. Never replay mutations, restore approvals, or assume a surviving child is owned by the new generation |

Additional mutation/access entry points must not be missed in later implementation:
`OllamaModelPuller::removeModel` in `core/src/runtime/OllamaRuntime.cpp` independently exposes
`/api/delete`; a fence solely in ModelOperationService would not cover that API. Download sidecar
and registration failure cleanup in ModelOperationService directly unlinks the newly committed
destination, and its journal recovery cleanup also needs incarnation-aware classification.
`HuggingFaceModelSource::clearMetadataCache` removes catalog cache, not serving model residency.
`core/src/voice/UnifiedAudioService.cpp` passes Piper/Kokoro model and voice asset paths to
`executeSpeechProcess`; generated audio cleanup is not model deletion. Desktop's
`bootstrap/ApplicationBootstrapper.cpp` starts a detached daemon, not a model process, and gains no
model termination authority. Generic authorized filesystem delete/patch operations can address
files independently of model-management APIs; later lease scope must explicitly decide whether
and how those paths participate without bypassing IFileSystemService/ToolExecutionGateway.
External OS tools remain outside Sentinel's ability to prevent file removal.

No process, model or credential was acquired from a live backend during this task. Artifacts,
registrations, shared observations, process handles and active run bindings remain distinct.
HTTP reachability never proves process ownership. Artifact sidecars do not prove process ownership.

## Candidate identity and lifecycle contracts — Proposed only

1. **Owned process identity:** daemon generation + executor instance/opaque handle + process start
   identity + provider/config generation + model. Endpoint belongs to configuration identity, not
   authority. An OS PID alone is reusable and insufficient; keep private executor lookup authoritative.
2. **Artifact identity:** storage authority/root + stable managed artifact key/repository revision
   and filename + file incarnation. Canonical path alone cannot fence replacement/ABA or aliases.
   Unmanaged registrations and remote provider model IDs require different namespaces. Two local
   registrations for one file must not yield independent deletion fences. Do not expose credentials
   in keys, endpoints, tokens or diagnostics.
3. **Owner/token identity:** daemon generation + unique run/request/subrequest ID; opaque serial
   never reused within a generation. Composite acquisition is all-or-nothing across required process
   and artifact resources, or rolls back provisional acquisitions. Retries for the same owner/resource
   are idempotent; a new inference request receives a new owner, not a UI session ID reused forever.
4. **Admission:** selection/catalog lookup reserves nothing. Acquire only after existing authorization,
   policy, binding resolution and readiness checks, immediately before dispatch, serialized with any
   deletion intent. Revalidate generation before dispatch; startup uses a separately bounded provisional
   reservation. Failed startup/readiness/admission rolls it back without manufacturing a live lease.
5. **Terminal release:** one authoritative terminal transition releases each held resource once.
   A cancel request merely signals intent; terminal cancellation must include provider/worker quiescence.
   Tool-step completion, streamed delta, UI disconnect, approval wait and provider-selection change are
   not terminal. Terminal-before-admission must fence delayed admission. Duplicate terminal events and
   late callbacks are no-ops for accounting, not negative counts. Completion cannot grant permission.
6. **Switching:** proposed default refuses a conflicting destructive reconciliation, retaining old
   resources and the active binding. Non-destructive selection for future requests may proceed only
   with a separately fenced generation. The reference fixture explores a *retire-old-admission*
   alternative, not an adopted provider-switch UX. Stop idle owned children only after approved M3
   policy and a zero-consumer check inside the same serialized lifecycle boundary.
7. **Deletion:** proposed refusal-first. Serialize acquisition and delete intent/tombstone, then run
   existing artifact ownership and authorization checks before filesystem/provider mutation. A zero
   count is necessary, not deletion authority. Failures need explicit intent recovery; never reopen
   an old incarnation on an ABA path. Ollama server-side use by other applications is unknowable:
   local counts cannot prove a shared provider model is globally unused.
8. **Restart/stale state:** old-generation tokens cannot release new work, even with reused serials.
   Session reconciliation does not prove old workers/children stopped. Do not carry lease counts into
   a new daemon or auto-reap orphans from a PID/port/name. Until authenticated durable identity and
   native recovery policy exist, report unresolved resources and refuse destructive recovery.
   No elapsed-time TTL alone reclaims a live/unknown owner's protection.
9. **Multiple consumers:** Agent, Chat, controlled tasks, child/subagent inference, background title
   generation and speech/embedding where actually dispatched need explicit admission ownership.
   Read-only catalog probes need no residency claim unless they actually load/use an artifact.
   One central accounting seam is preferable to per-UI counters; it does not replace runtime routing.

## Decision-neutral deterministic evidence

`tests/core/test_resource_lease_reference_contract.cpp` is explicitly isolated in
`proposed_reference_only`. It is compiled solely as a test executable. No production header,
factory, flag, IPC schema or runtime is wired to it. Its `eraseArtifact` changes only an in-memory
closed bit, never a file. It has **no process start/terminate/kill, HTTP or tool execution API**.
`retire` returns accounting eligibility, never permission to terminate. The mutex linearizes the
toy admission/delete/terminal operations; this does not certify production concurrency.

| Scenario | Reference fixture / production characterization | Meaning |
| --- | --- | --- |
| Two consumers, delete while active | twoRunsRetainArtifactUntilBothTerminate | Model refuses in-memory removal until both terminal; production D6 still open |
| Provider and endpoint switches | switchRetainsOldResource, 2 rows; existing frozen-binding ownership tests | Candidate closes old admission while retaining old count; existing production binding remains stable, physical reconciliation not fixed |
| Cancel, duplicate terminal, duplicate admission | terminalAndCancellationReleaseExactlyOnce; cancellationDuringReleaseCannotDecrementTwice | Terminal cleanup clears process and artifact independently, repeated notification/release no-op, including concurrent release/cancel |
| Startup failure/readiness/permission refusal | failedAdmissionHasNoLease, 3 rows; existing synchronousFailedStartSetsUnavailable | Candidate booleans gate admission; only existing LocalModelRuntime fixture tests actual production failure projection |
| Daemon restart/token ABA | restartRejectsOldGenerationEvenWhenSerialIsReused; existing daemon restart IPC fixtures | No old-generation mutation of new counts; no orphan reaping claim |
| Stale resource incarnation | staleIdentityCannotReopenRetiredResource | Old identity cannot reopen; new incarnation distinct |
| Shared Ollama/LM Studio/remote | externalObservationsHaveNoDestructiveAuthority, 3 rows; unknownProcessCannotBeTerminated | Toy observations carry no destructive API; production executor rejects non-owned handles |
| Concurrent owners/repeated cancel | concurrentAcquisitionAndCancellationRemainBalanced, 8 threads | Balanced linearizable toy counts without timing sleeps |
| Delete/admit and terminal/admit races | deleteAndAdmissionSerializeWithoutCheckThenActGap; cancellationRacingAdmissionCannotLeaveTerminalOwnerLeased | Invariants hold whichever thread wins; not a scheduler/interleaving coverage or TSAN claim |

Reference fixture limitations: generic synthetic keys (not a production path/PID identity validator),
caller-supplied authorization/readiness booleans (not an authorization engine), no atomic multi-resource
reservation, no startup orchestration, persistence, tombstone GC, filesystem mutation recovery, worker
quiescence or authenticated orphan recovery. It deliberately has no timeout reclamation. Production
implementation needs these decisions/tests, not a copy of this toy class. Passing reference tests
do not convert any unresolved scenario into production success.

### Existing gaps remain unchanged

- **D4:** providerSwitchRetainsOwnedChild remains XFAIL; provider early return precedes reconciliation.
- **D5b:** endpointSwitchDoesNotReconcileOwnedChild remains XFAIL; same-model endpoint switch does
  not reconcile old owned child.
- **D6:** removalDuringActualUseRemainsBlocked remains SKIP. The existing positive
  modelDeletionDoesNotInvalidateActiveRunBinding still witnesses unfenced artifact deletion while
  the in-memory binding survives; it is not a claim of safe delete-during-inference.
- No existing lifecycle test or expected-failure directive was changed.

## Security and permission boundaries

- Leases are retention/accounting facts, not capabilities or grants. ToolExecutionGateway,
  descriptor validation, approval/permission/sandbox authorization remain mandatory for every tool.
- Existing daemon peer authorization and generation fencing remain ahead of requests; token
  possession must not authorize IPC, cross-session access or mutation replay after reconnect.
- Existing provider credentials and network restrictions remain in provider/configuration owners;
  a binding/lease must neither expose credentials nor bypass readiness/network checks.
- Only the originating ProcessExecutor may act on its owned record. An Agent binding or artifact
  lease never authorizes provider termination. Shared local and remote servers must never be killed.
- Existing known-root/sidecar/deletion restrictions remain necessary even with zero consumers.
  Importing a registration cannot authorize deleting an unmanaged file or unlinking another root.
- No hard-stop, automatic unload, idle cleanup or orphan termination was implemented. No approved
  decision record or CSEC policy was edited. PD-D10 and PD-D04 decisions remain outside this task.

## Maintainer M1–M10 decision matrix — every recommendation Proposed

| Decision / alternatives | Proposed default and safety consequences | User-visible behavior | Cross-platform / migration risk | Required deterministic evidence | Required native acceptance |
| --- | --- | --- | --- | --- | --- |
| M1: ratify opaque owned identity / amend / infer ownership | Ratify owned-only, separate artifact identity; never infer from HTTP/PID | External services explicitly external | PID reuse, Windows handles, canonical paths/case/aliases; do not adopt old records | Unknown handle, ABA incarnation, alias identity, wrong generation | Owned vs user-started llama/Ollama/LM Studio on Fedora, macOS, Windows |
| M2: refuse / drain / mixed | Refuse destructive collision first; queued drain separate approval | Typed in-use refusal; retain active run and old resource | Concurrent switch/delete/admit race; do not change saved selection semantics silently | Linearized admission/delete; 2 owners; switch without binding rewrite | Real in-flight streaming while switching/deleting; zero interrupted consumers |
| M3: stop idle / keep warm / until shutdown | Stop only verified idle owned child after explicit approval, not implemented here | Old owned child may reconcile after last consumer; no shared cleanup | Existing D4/D5b behavior changes; stop acknowledgement and reused ports vary | Zero-count reconciliation, failed stop/start, endpoint ABA | Owned child exits/reaps on every OS; external process survives |
| M4: graceful / graceful then force | Cooperative drain first; escalation only separate approved, calibrated policy | Truthful Stopping; no invented deadline guarantee | Process groups vs Windows jobs; current executor shutdown behavior must be reconciled, not expanded | Borrowed worker, hung provider, duplicate shutdown, no external kill | Signal/job handling and sandboxed descendants; boundedness only when authorized |
| M5: isolated worker now / defer / reject | Defer separate supervised-worker project; no hard-stop claim | Non-cooperative provider can remain Stopping | New protocol, credential transfer and crash boundary are large migrations | Worker-death/IPC/credential/cancel fixtures before activation | Supervised non-cooperative process and crash containment on target OSes |
| M6: Agent+Chat / controlled / all consumers | All actual resource users, inventory first; no model-selection lease | Internal/background use may explain in-use refusal | Missing voice/title/child consumer creates false zero; avoid double counting controlled tasks | Admission/terminal tests for each actual caller, failed readiness, nested lifetime | Concurrent chat/Agent/background/voice with real artifacts |
| M7: persisted orphan adoption / leave unresolved | Leave unresolved until durable non-reusable identity and approved recovery; stricter first milestone than proposal's eventual narrow reaping | Restart reports interrupted/unresolved; no automatic kill | Old daemon may overlap, PID recycled, boot/session changes; old counts are not proof | Old token collision, failed persistence, restart, no replay, unverifiable identity refusal | Kill daemon during real inference; prove child identity before any separately approved reap |
| M8: refuse delete / queue deferred delete | Refuse with bounded typed in-use reason; no deferred queue | Retry explicitly after completion; no surprising late deletion | IPC error/schema compatibility and file/sidecar partial failure | Alias/shared artifact counts, delete intent failure, unmanaged paths, active consumers | Unix unlink vs Windows sharing, open model handles, external shared users |
| M9: truthful Stopping / block release on worker | Accept truthful Stopping for limited scope; keep hard-stop acceptance blocked | No false stopped or unloaded status | Existing CLI/supervisor deadlines must not imply runtime stopped | Non-cooperative provider, disconnect/reconnect, borrowed lifetime | Native systemd/launchd/service supervisor interruption and recovery |
| M10: assign accountable reviewers / defer | Named model/IPC/security owners and independent maintainer signoff, dated | Scope and unsupported behavior explicit in release notes | Unowned policy/rollout defaults risk accidental adoption | Reviewed decision-to-test traceability and unchanged acceptance records | Named owners approve platform evidence; no fixture-only acceptance |

M7 intentionally splits *eventual authenticated orphan recovery* from the safest first milestone;
the proposal recommends eventual narrow persisted-identity reaping. Neither recommendation is adopted.
M4 has no new timeout number; calibration remains pending. This matrix is not a decision record.

## Validation

| Gate | Actual result |
| --- | --- |
| Configure / full build | Existing tests preset configure and full-target build passed; no new build tree |
| Focused production and reference lifecycle / IPC | 6/6 CTest executables passed, 11.65 s; original reference fixture had 17 passes; final reference extension recorded below |
| Final reference executable | 18 QtTest passes including init/cleanup; 0 failures/skips |
| Full sequential CTest | **156/156 executables passed**, 0 failures, 220.86 s, explicit `-j1`; 141 QtTest suites reported 2,848 passes, 0 failures, 9 skips and 2 preserved XFAILs |
| clang-format | CI-pinned 18.1.8 dry-run/Werror on the new C++ file passed |
| Scoped clang-tidy | New test source analyzed using existing no-PCH analysis database helper: 0 user-code diagnostics/errors; 499 non-user warnings suppressed by the tool |
| Rust 1.94 | fmt, locked/offline Clippy with `-D warnings`, locked/offline workspace tests passed: 142 tests, 0 failures/ignored, 8 test/doc-test suites |
| IPC schema consistency | Both generators passed `--check`; no schema/type changes |
| Privacy / credentials | Personal-path scanner: 0 flagged; all 3 changed/untracked files scanned: 0 high-confidence private-key/token findings |
| Whitespace | `git diff --check` passed |

Focused executable totals including lifecycle hooks: ProcessExecutor 11; daemon IPC 42;
Agent reliability IPC 7; production ownership 16; initial reference 17; LocalModelRuntime 52
with 1 skip and 2 explicit expected failures. The extra cancellation-during-release case raises
the final reference total to 18. XFAILs are preserved witnesses, not newly fixed production behavior.

The nine full-suite skips are: Linux Secret Service helper on macOS (1), unavailable native
incremental tool-assembly contract (1), strict macOS child-process shell fixtures (4), opt-in live
classification (1), missing-node branch unavailable because npx is installed (1), and D6 (1).
The full log contains 72 QWARN diagnostic lines, not test failures. Build logs contain no compiler
warnings/errors for this task. The first scoped analysis identified enum size/vector reservation
suggestions and six optional-access warnings where QtTest macros were not understood; these were
resolved in the new fixture without suppressions, then analysis and the final reference executable
were rerun. Final CTest includes that 18-pass executable. Logs were captured in
`/tmp/sentinel-p305a-full-tests.log`, `/tmp/sentinel-p305a-focused.log`,
`/tmp/sentinel-p305a-reference-final.log`, `/tmp/sentinel-p305a-tidy-final.log` and
`/tmp/sentinel-p305a-rust.log`; the existing build's `Testing/Temporary/LastTest.log` retains
full-suite output. No TSAN or exhaustive scheduler-interleaving proof is claimed.

All tests are local deterministic fixtures; none authorize real model loads, external provider
cleanup or native acceptance. Fedora KDE/Wayland, real backend residency, Windows file sharing,
native process identity/reaping and supervisor restart acceptance were not performed on this
macOS host. Rust checks are compatibility evidence, not proof of native supervisor shutdown.

## Smallest implementation milestone after explicit approval

1. Obtain dated M1/M2/M6/M8 dispositions and named M10 reviewers, including exact consumer
   coverage and typed refusal compatibility. Do not infer approval from this report.
2. Add inert internal process/artifact identity and accounting interfaces behind the existing
   daemon composition, with transactional multi-resource admission and terminal/quiescence fixtures.
   First prove no behavior change when not connected. Keep credentials/security authority out.
3. Wire actual Agent/Chat/controlled/background admissions and authoritative terminal cleanup;
   account for child work and failure rollback before allowing a zero-count deletion decision.
4. Introduce refusal-only managed-artifact delete fencing, serialized with admissions and existing
   storage ownership checks, initially excluding any unsupported shared-provider administration.
   Resolve D6 only with actual production witnesses plus native open-file acceptance.
5. Separate milestone after M3 approval: owned-child provider/endpoint reconciliation. Only then
   replace D4/D5b expected failures with production success tests and native evidence. Shutdown
   escalation, model unload, keep-warm and M7 recovery remain separately gated work.

## Exact changed files

- `tests/core/test_resource_lease_reference_contract.cpp` — test-only candidate reference model.
- `tests/CMakeLists.txt` — one test registration.
- `docs/audits/P3_05A_RESOURCE_LEASE_SAFETY_CONTRACTS_RESULT.md` — inventory, candidate contracts,
  decision matrix and evidence. No production source, approved decision record or generated schema changed.
