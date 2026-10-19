# P8-03 — Scoped Memory Management, Correction & Safe Backup

Date: 2026-10-19 (Europe/Istanbul).

## Delivery status and repository identity

Production scope management is implemented in the existing Core, daemon and Desktop
paths. Generic combined backup integration remains deliberately **Blocked** for
managed memory; the explicit versioned scope archive is the supported memory
export/import path. This is not a full application backup claim.

- Base/main/origin-main: `7e9dc22b4d594d38d22d7a89e1ff6b5c51f87c34`.
- Branch: `feature/p8-03-memory-management`, created from verified clean main.
- Origin: `https://github.com/sentinel-projects/sentinel.git`.
- P8-01 (`19f223e9`) and P8-02 (`514d2741`) were verified ancestors of main;
  both implementation reports were read.
- Repository AGENTS and architecture/runtime/security/build/testing instructions
  were inspected. Personal-Brain was unavailable (`brain` absent), so the explicitly
  supplied existing repository identity was verified directly.
- Origin was fetched before branch creation; main equaled origin/main and the tree
  was clean. The app thread inventory and process inspection showed no competing
  coding agent modifying this checkout. Approximately 84 GiB disk space remained.
- Existing `build/tests` and `cli/target` were reused. No clone, worktree, reset,
  stash, clean, PR, workflow dispatch, commit, push or merge was performed.
- Changes were left uncommitted at implementation handoff. The maintainer subsequently
  explicitly authorized committing and pushing this feature branch. Branch/base/remote
  were checked again before delivery; main integration is outside that authorization.

## Existing architecture audit and limitations

P8-01 supplied `ScopedMemoryRecord`, `MemoryAccessContext`, provenance, scoped
read/write in `IMemoryStore`, `SQLiteMemoryStore` and `InMemoryStore`, plus trusted
conversation/workspace context resolution. ChatModeService and AgentRuntime consumed
only scoped recall; Legacy was excluded and Personal remained explicit. P8-02 added
ContextBudgetManager and immutable context assembly without granting memory authority.
Those boundaries remain authoritative.

Before this change there was no production management contract for bounded authorized
listing, stable-ID user correction, revision conflicts, scope migration, exact/batch
deletion or safe scoped archives. Legacy key/value records lacked stable exported
identities and historical timestamps. The old JSON backup could not preserve scope,
so P8-01 correctly blocked it with scoped records. The existing settings clear action
was global; it is now blocked in favor of explicit authorized scope management.
The existing Desktop backup transfer and other application backup domains remain.

## Newly implemented production paths

`IMemoryStore` now has typed management list/read/mutate/clear operations with
fail-closed default implementations. Shared preparation/validation implements scope
rules, metadata parsing, receipts, revision conflicts and archive validation.
SQLite uses SQL authority predicates before pagination; InMemoryStore applies the
same rules under its store lock before returning a bounded page. Foreign IDs are
hidden as NotFound. Result states distinguish Unavailable, Unauthorized, InvalidInput,
Conflict, NotFound, LimitExceeded and Ready (including an empty list).

`ApplicationController::manageMemory` resolves the actual persisted conversation and
verified workspace, rejects unknown fields/types and checks UI context against the
actual context for mutations. It emits recall/projection invalidation after changes.
`desktop.action` uses the existing session ownership and busy-run gate. Canonical IPC
schema/bridge metadata and generated clients/descriptors were updated using the
existing generators. Memory responses refresh projection without reattaching the
conversation, preserving consecutive operations and chunked reads.

The existing navigation rail now opens a Memory page. It provides scope selection,
search/keyset pagination, metadata/provenance inspection, full-value chunked reads,
creation, correction, individual/selected deletion, confirmed clear, explicit scope
migration, and user-selected file export/import. QML presents Core results; it does
not implement an independent authorization mechanism. Plain text is used for values
and source metadata. Personal consent is per management call, not a recall setting.
Dialogs initially focus Cancel; deletion/migration require DELETE/MOVE text and
explicit confirmation. Focus returns to the editor or scope selector. Editor buttons
wrap in compact layouts. Busy, empty, unavailable, unauthorized/conflict and bounded
operation messages are surfaced.

## Authorization and ownership

- Session records belong to the actual authorized persisted conversation and its
  workspace identity. Another session's records cannot be edited/deleted.
- Project records require the existing trusted workspace resolver/allowlist;
  raw paths and serialized model text cannot establish identity.
- Personal management requires explicit profile intent for the current operation.
  It does not enable Personal inclusion in Chat/Agent recall.
- Legacy records are profile-local unclassified data, inspected only through explicit
  management selection; they remain absent from recall until explicit authorized
  reclassification. No destination owner can be supplied to bypass Core resolution.
- Source and destination authorization are both required for a separate scope move.
  Ordinary correction cannot silently change scope.
- Explicit manual create records UserProvided with `manual:<actual-session>` source.
  GeneratedSummary/Inferred/Unknown remain distinguishable; correction preserves
  historical provenance/source and adds a separate attributable receipt. No LLM
  writes, background promotion, embeddings or indexing were added.

## SQLite migration, correction and deletion

Memory schema advances from 4 to 5 in the existing transaction. Scoped records gain
revision/receipt columns; Legacy gains UUID/revision/receipt and nullable timestamps.
Existing native legacy IDs, keys and values are retained. Unknown historical creation
time is not invented; explicit Legacy classification records a new classification
creation time and separate reclassification receipt. Unsupported future schemas and
failed migration retain existing fail-closed behavior.

Corrections preserve UUID, original creation time/source/provenance; modification time
advances and a receipt records actual actor, action, time and previous metadata digest.
Exact record revisions are required, so stale concurrent correction/deletion fails.
SQLite batches commit atomically and roll back on any conflict/failure; InMemoryStore
commits locked copies only after successful validation. Corrupted persisted metadata
is rejected rather than silently rewritten. Revision tokens change even for old
internal key/value/scoped writer calls and after import to avoid stale-token reuse.

Single/selected deletion and authorized scope clear touch only memory, not transcript
or agent-run stores. There is no implicit global clear in the product management path.
Subsequent Chat/Agent retrieval sees creates/corrections/deletions; cached recall status
is invalidated. Accepted in-flight request/cancellation semantics stay unchanged.
Already sent provider data, existing transcripts and historical backups are not
retracted; neither physical erasure nor backup erasure is promised.

## Scope archive format and failure recovery

JSON `format:"sentinel-scoped-memory"`, `version:1` contains full records and SHA-256
of the compact canonical records JSON. Fields preserve UUID, scope, owner/workspace,
key/value, provenance/source, creation/modification timestamps and correction receipts.
Internal concurrency tokens are deliberately regenerated after import. Strict schema,
UUID, enum, ownership, timestamps, receipt shape and integrity checks precede writes.
Hash integrity is not authenticity. Imports retain claimed historical provenance but
append actual import attribution; prompt memory marks this history importedUnverified.

Export is explicitly one authorized scope and cannot silently include Personal data.
Import requires matching actual scope/owner authority; it never rebinds a foreign
workspace to the current project. Duplicate policy is explicit: reject the whole
batch (default), or skip identical logical records and reject differing conflicts.
Import-only receipts are ignored for identical replay comparison; different correction
history is not treated as identical. Entire imports roll back on failure, unsupported
future versions or malformed data. No commands/paths are executed from an archive.

DesktopBackupHelper reads a bounded local user-selected file and validates it; export
uses QSaveFile, private file permissions and atomic replacement. Providers, tokens,
permission grants, settings and unrelated databases are absent from this archive.
BackupService owns the shared scope export/import calls. Old combined JSON memory
export/replace stays blocked when any managed records exist, including Legacy UUIDs
and receipts; this strengthens P8-01 rather than bypassing its safeguard. Settings,
workspace profiles, extensions and chat backup domains keep their prior paths. Old
key/value imports into an empty store remain unclassified Legacy compatibility.

## Bounds and known remaining limitations

- Scope archive limit: 500 records / 96 KiB, all-or-nothing. Listing: store pages at
  most 50, Desktop pages 10; search 256 characters, exact reads in 4096-character chunks.
  No partial success is reported for oversized scopes/archives.
- Correction receipts are bounded to 64. Further corrections fail explicitly; export
  refuses a record at the receipt bound so that restore can add its import receipt
  without losing historical receipts. Larger archives and history lifecycle need a
  separate reviewed format/transfer milestone.
- Export is serialized within a daemon request but is not a cross-process SQLite
  snapshot. Coordinate external database writers when exporting a scope.
- Combined full backup/restore of scoped memory remains Blocked pending an authorized
  multi-domain migration/rollback design. The Memory page exports memory-only data.
- No cross-profile/foreign-workspace owner remapping or automatic cross-project sharing.
- No automatic memory promotion, provider calls, model downloads, resource policy,
  process ownership, PD-D10 leases or automatic unloading changes.
- P8-02 image-token accounting remains fail-closed; this change does not restore vision
  by bypassing ContextBudgetManager. AgentLoop final acceptance and ToolExecutionGateway
  authorization are unchanged.

## Focused evidence and complete validation

All validation used the existing build/tests and cli/target directories and
isolated temporary profiles, SQLite databases and workspaces. No real user database
was edited, cleared, exported or restored during tests.

| Check | Actual result / evidence |
|---|---|
| Configure | `cmake --preset tests` passed (`p803-configure.log`) |
| Complete C++ build | Passed; final production build `p803-build-verified-final.log`, later test-only rebuilds `p803-build-delivery*.log`; no warning-free whole-project claim |
| Focused memory/context/Chat/controller/IPC | 7/7 CTest programs passed, 35.82 sec (`p803-focused-third.log`) |
| **Full sequential CTest** | **170/170 passed, zero failed, 240.74 sec** (`p803-ctest-verified.log`), `-j 1 --timeout 180 -V` |
| Final expanded assertions | 2/2 CTest programs passed, 30.26 sec (`p803-focused-delivery.log`); store 7 and IPC 73 Qt cases passed after test-only additions |
| Store management | 7 Qt test cases passed in full run; both SQLite and InMemory backends, CAS rollback, pagination, corruption rejection and archive recovery |
| P8-01/P8-02 regressions | Scoped memory 9, SQLite store 15, ContextBudgetManager 10, ChatModeService 18 Qt cases passed |
| Product controller / settings | ApplicationController 131 and SettingsRecoveryRuntime 17 Qt cases passed |
| Desktop / real IPC | View-model 75 and IPC 73 Qt cases passed; real daemon QML E2E passed (4.65 sec) |
| Rust fmt | `cargo fmt --manifest-path cli/Cargo.toml --all -- --check` passed, empty log |
| Rust Clippy | Workspace/all-targets/locked/offline, `-D warnings`, passed (`p803-clippy.log`) |
| Rust workspace tests | **142 passed**, zero failed/ignored, locked/offline (`p803-rust-tests-final.log`) |
| IPC generators | `generate.py --check` and `generate_desktop.py --check` passed |
| QA registry/schema | Valid; preserved existing `missing external component evidence: roasd.packaging` warning (`p803-qa-final.json`); no certification/status promotion |
| Pinned clang-format | 18.1.8; full new C++ files clean and changed hunks produce empty diff (`p803-format-verified.diff`) |
| Scoped clang-tidy | 13 production translation units, all exit 0; new sources/header fully analyzed, changed lines elsewhere; zero reported project diagnostics in final logs |
| Privacy / credentials | Personal path scan zero flagged; 46 changed files scanned for private-key/GitHub/AWS/OpenAI credential signatures, zero flagged; content suppressed |
| Whitespace | `git diff --check` passed |

The first broad run was 169/170 because an existing Desktop test still expected
successful global memory clear. It was updated to assert the new blocked result,
retained record and status notification, then the full sequential run passed.
Earlier focused iterations caught a QML read-only implicit-height assignment, an
archive replay comparison and a client refresh/reattach race. These were corrected
and the real IPC/UI paths passed again. Rust tests initially encountered sandbox
socket PermissionDenied; the offline disposable local-IPC run passed with the
required execution permission. Static analysis found enum sizing, numeric-conversion
and redundant-copy diagnostics; final narrow corrections were rebuilt and validated.
Non-user/out-of-scope analyzer warnings are suppressed; this is not a claim of zero
warnings across all dependencies or compilation.

Focused behavior evidence includes Session CRUD; Project/Personal isolation and
explicit opt-in; Legacy inspection/reclassification; stale revisions across two SQLite
connections; all-or-nothing batch/import rollback; UUID duplicate rejection/identical
replay; malformed/future-version/invalid provenance and corrupt stored metadata
rejection; schema migration/reopen; subsequent actual Chat prompt and Agent context
visibility after correction/deletion; and real Desktop IPC create/correct/delete,
file archive save/load/restore and explicit Personal denial. Additional final focused
assertions cover Project and Personal correction/archive/authorized and unauthorized
delete, Legacy archive UUID/Unknown/unknown-creation preservation, and deletion cache
invalidation. These final assertions change tests only; the full run used the final
production sources.

The real QML/daemon keyboard flow created a record, inspected/corrected it, verified
that initial Return cancels deletion without data loss, typed DELETE and deleted the
record. A 640-pixel compact empty state was captured and visually inspected in
`build/tests/desktop-daemon-verification/p803/memory-compact.png`. It shows wrapped
actions, readable ownership/status, safe disabled destructive controls and pagination.
The existing E2E also verifies the disconnected shell state; Memory reports loading,
empty and blocked/error results through the same production bridge. No live provider
calls or opt-in live classification probe were activated.

### Preserved legitimate SKIPs and XFAILs

The final full log contains **10 internal Qt SKIPs, 2 XFAILs and 81 QWARN lines**.
All 170 CTest programs completed successfully. The expected/conditional results were:

- **SKIP** `CredentialStoreTest::missingLinuxHelperRefusesPersistence()` Linux Secret Service helper contract.
- **SKIP** `CredentialStoreTest::testPlatformCredentialBackend()` Native credential write refused; no real smoke success claimed.
- **SKIP** `AgentReliabilityHarnessTest::cor11_cancelAfterFirstStreamedDelta(native-tool-assembly)` COR-11 native incremental tool-call assembly remains explicitly blocked: IChatProvider exposes no typed incremental tool-argument assembly event, so a partially streamed native call could only be dispatched after a validated terminal assembly. This fixture asserts the native request stays on the bounded non-streaming path (one sendRequest, zero streamed deltas, zero public deltas) and does not invent an assembly contract.
- **SKIP** `AgentRuntimeTest::asyncRunCommandStreamsAndContinues()` This shell fixture requires child creation; strict macOS plans deny forks. Native host lifecycle is covered by plugin integration tests.
- **SKIP** `AgentRuntimeTest::asyncRunCommandCancellationAndTimeout()` This shell fixture requires child creation; macOS strict no-detached-child plans now deny all forks. Plugin host cancellation is covered by real host integration tests.
- **SKIP** `AgentRuntimeTest::asyncDockerUsesProcessExecutorAndPreservesRestrictions()` The fake Docker shell fixture requires child creation; strict macOS plans deny forks.
- **SKIP** `AgentRuntimeTest::shutdownStopsActiveCommand()` This shell fixture requires child creation; strict macOS plans deny forks. Native host shutdown is covered by plugin integration tests.
- **SKIP** `ObservationPolicyTest::optInLiveClassification()` Explicit opt-in required for production local-provider probe
- **SKIP** `RealToolExecutorToolsTest::browserToolsReportMissingNodeGracefully()` npx is installed on this machine; the missing-node branch cannot run.
- **XFAIL** `LocalModelRuntimeTest::providerSwitchRetainsOwnedChild()` P3-02 D4: switching provider returns before owned cleanup
- **XFAIL** `LocalModelRuntimeTest::endpointSwitchDoesNotReconcileOwnedChild()` P3-02 D5b: same-model endpoint switch leaves old owned child
- **SKIP** `LocalModelRuntimeTest::removalDuringActualUseRemainsBlocked()` No model-residency lease/removal contract exists; a live provider handle is not proof of active model use. Source-only D6.

## Recommended P8-04

A separately reviewed larger/chunked scope-archive and consistent SQLite snapshot
format, with correction-history lifecycle and multi-domain backup rollback. Keep
Personal sharing and provider-specific image token accounting separate authorized
milestones; do not implicitly add durable promotion or cross-project authority.

## Complete changed-file inventory

- `apps/sentinel-daemon/service/DesktopProjection.generated.h`
- `apps/sentinel-desktop/CMakeLists.txt`
- `apps/sentinel-desktop/include/sentinel/desktop/DesktopBackupHelper.h`
- `apps/sentinel-desktop/include/sentinel/desktop/DesktopBridge.generated.inc`
- `apps/sentinel-desktop/include/sentinel/desktop/DesktopRuntimeClient.h`
- `apps/sentinel-desktop/include/sentinel/desktop/DesktopShellViewModel.h`
- `apps/sentinel-desktop/src/DesktopBackupHelper.cpp`
- `apps/sentinel-desktop/src/DesktopRuntimeClient.cpp`
- `apps/sentinel-desktop/src/DesktopShellViewModel.cpp`
- `core/CMakeLists.txt`
- `core/include/sentinel/core/app/ApplicationController.h`
- `core/include/sentinel/core/app/BackupService.h`
- `core/include/sentinel/core/interfaces/IMemoryStore.h`
- `core/include/sentinel/core/memory/InMemoryStore.h`
- `core/include/sentinel/core/memory/MemoryManagement.h`
- `core/include/sentinel/core/memory/SQLiteMemoryStore.h`
- `core/include/sentinel/core/memory/ScopedMemory.h`
- `core/src/app/ApplicationController.cpp`
- `core/src/app/ApplicationMemoryManagement.cpp`
- `core/src/app/BackupService.cpp`
- `core/src/context/ContextBudgetManager.cpp`
- `core/src/memory/InMemoryManagement.cpp`
- `core/src/memory/InMemoryStore.cpp`
- `core/src/memory/MemoryManagement.cpp`
- `core/src/memory/SQLiteMemoryManagement.cpp`
- `core/src/memory/SQLiteMemoryStore.cpp`
- `core/src/memory/ScopedMemory.cpp`
- `docs/audits/P8_03_SCOPED_MEMORY_MANAGEMENT_RESULT.md`
- `docs/concepts/MEMORY.md`
- `docs/interfaces/IPC.md`
- `protocol/DesktopBridgeMethods.json`
- `protocol/QtIpcContract.generated.h`
- `protocol/ipc-v1.json`
- `tests/CMakeLists.txt`
- `tests/core/test_application_controller.cpp`
- `tests/core/test_chat_mode_service.cpp`
- `tests/core/test_memory_management.cpp`
- `tests/core/test_scoped_memory.cpp`
- `tests/core/test_settings_recovery_runtime.cpp`
- `tests/core/test_sqlite_memory_store.cpp`
- `tests/desktop/test_desktop_ipc.cpp`
- `tests/desktop/test_desktop_shell_view_model.cpp`
- `tests/helpers/DesktopDaemonInteraction.h`
- `ui/qml/Main.qml`
- `ui/qml/components/navigation/NavigationRail.qml`
- `ui/qml/pages/MemoryPage.qml`
