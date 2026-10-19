# P8-06 — Coordinated Application Backup & Recovery

Dates: 2026-10-19–20 (Europe/Istanbul).

**Status: Partial safety foundation; coordinated application backup and restore remain Blocked.**
The milestone's primary Definition of Done is **not achieved**. This change does not
introduce a combined archive or an admitted application restore engine. It provides
a durable internal journal/preimage primitive, production capability reporting,
refusal of unsafe legacy multi-domain operations, explicit Desktop limits and
restore confirmation. The journal is not wired to live storage replacement.
No complete-application backup, crash-safe portable import or verified multi-domain
rollback is advertised. The request expressly permits keeping affected functionality
Blocked when durability, admission and rollback guarantees cannot be established.

## Repository and prerequisites

Existing supplied checkout only; verified origin
`https://github.com/sentinel-projects/sentinel.git`. Fetched origin; clean local
`main == origin/main == 20107bb5cfba8978b0aea97b38d04eb393d68318` before editing.
Created previously absent branch `feature/p8-06-coordinated-backup-recovery` from
that exact base. P8-01 through P8-05 are integrated: P8-01 `19f223e9`, P8-02
`514d2741`, P8-03 `64f66ac5`, P8-04 `086e8dc8`, P8-05 `20107bb5`.
Read AGENTS, architecture, Agent Runtime, security, building/testing, prior five
implementation reports, memory and IPC contracts and the storage owners below.
Personal-Brain CLI was unavailable on PATH; the explicitly supplied checkout was
verified directly. App inventory showed this as the only active coding chat on the
checkout; process inspection showed the shared app server, no separate competing
coding CLI. No subagents or delegated writers. Initial free space approximately
79 GiB. Existing `build/tests` and `cli/target` reused.

No clone/worktree/new build tree, commit/push/merge/rebase/reset/stash/clean/PR or
workflow dispatch. Analysis-only compile database and disposable test profiles use
`/tmp`; these are not additional build trees. Modifications remain uncommitted.
No actual application profile was used for backup/restore tests. C++ validation
uses disposable HOME/XDG/profile paths, in-memory test credentials where injected,
and an outer macOS sandbox denying Keychain file access and securityd lookup.
Tests do not gain access to real credentials to obtain passing results.

## Production storage ownership audit

Locations below are **symbolic internal contracts**, not resolved user paths.
`P` means the directory passed to StandardPathProvider::setProfileDirectory;
`A` means Qt AppDataLocation; `C` means AppConfigLocation; `D` means
DocumentsLocation/Sentinel; `E` means application executable directory.
Production non-portable DaemonService explicitly sets `P=A` before composing owners.
Without explicit P, StandardPathProvider uses E in portable mode, otherwise C for
settings and D for memory/conversation/history/RAG. Portable agent/grant placement
is independently A, not necessarily E. A directory copy is therefore not a profile
inventory. SQLite sidecars must remain under SQLite ownership.

| Domain and actual owner | Location / version | Live writes and lifecycle | Classification for coordinated backup |
|---|---|---|---|
| Memory: SQLiteMemoryStore, SQLiteMemoryManagement/Archive | pathProvider.memoryDatabasePath(): P/memory.sqlite3; schema 7 | Owner-thread mutations, worker-owned read-only snapshots and transactional import workers; scoped source/FTS triggers, receipts/revisions | Blocked in combined operation; explicit authorized scope archives remain supported |
| Conversations: SQLiteConversationStore / ChatModeService | pathProvider.conversationDatabasePath(): P/conversations.sqlite3; schema 6 | Conversation/message/summary/title mutations, streamed placeholder/completion and asynchronous title callbacks; owner Qt SQL connection | Blocked; v1 single-domain portable Chat export/import is independent and loses session IDs |
| Chat history: SQLiteChatHistoryStore | pathProvider.chatHistoryDatabasePath(): P/chat_history.sqlite3; schema 3 | Separate append/status/prune/clear owner; not identical to conversation database | Explicitly excluded; cannot infer it is wholly reconstructable |
| Agent runs: SQLiteAgentRunStore / AgentRuntime | A/agent_runs.sqlite3; schema 7 | Per-connection transactions for events/steps/tools, startup interruption handling and pruning | Explicitly excluded; never restore or replay tool actions |
| Permission grants: SQLitePermissionGrantStore / AgentRuntime | A/permission_grants.sqlite3; permission_grants table, plugin-owner migration | Load/save/delete via separate owner/transactions and runtime in-memory grants | Explicitly excluded; serialized grants never authorize restoration |
| Settings: AppSettings → DpapiEncryptedSettingsStore → JsonSettingsStore | pathProvider.settingsFilePath(): P/settings.json; settings schema 1 | Cached values; each setter saves the document with QSaveFile and emits live configuration signals | Blocked in combined operation; v1 portable single-domain basic theme/language/network selection retained |
| Workspace catalog, selection and profiles: AppSettings / WorkspaceService | Keys workspaceCatalogJson, selectedWorkspaceId, defaultWorkspaceId, workspaceProfilesJson in settings owner; profile format version 1 | Catalog/profile mutations change runtime resolution and authority | Requires explicit sensitive export; combined restore Blocked; profile JSON alone is not the complete workspace catalog or stable application-profile identity |
| Extensions: AppSettings / ExtensionService / McpService | mcpServersJson and workspace extension profile overrides in settings; plugin credential backend separate | Configuration signals can reconcile live clients/extensions; raw file replacement bypasses these owners | Only existing filtered single-domain portable endpoints/secret references export; complete configuration and combined restore Blocked |
| Plugin packages: PluginManager / ExtensionService | Default A/plugins; discovered package plugin.json and configured scan roots | Load/unload/reload and removal; enabled plugin actions are daemon-lifetime, not a complete persisted portable configuration | Explicitly excluded; third-party package files are not restored as trusted executable configuration |
| Native credentials: CredentialStore / provider and plugin backends | OS secret backend; legacy settings migration remains a separate concern | Explicit secret operations and migration, independent of JSON backup | Explicitly excluded; never copy raw settings as a substitute for an allowlisted export |
| IPC session projections: DaemonSessionProjectionStore | settings-file parent/ipc-sessions.sqlite3 in DaemonService | Live clients/session ownership/generation projections | Explicitly excluded; old client generations must not regain authority after future restore |
| Scoped FTS5 | Tables/triggers in memory.sqlite3 | Transactionally updated with source; explicit bounded maintenance | Reconstructable projection, with availability/integrity requirements; not independently restored |
| Local RAG: LocalRagStore | pathProvider.localRagDatabasePath(): P/local_rag.sqlite3 | Independent retrieval/index lifecycle | Reconstructable cache; excluded, no automatic embedding regeneration |
| Scope transfer jobs: MemoryArchiveJobs | memory DB parent/memory-archive-staging/{format,owner.lock,transfer-*.part} | At most two context-bound jobs; worker tasks, private staging, cancellation, marker/lock cleanup | Ephemeral staging excluded; marker is not an application recovery journal |
| Existing BackupService v1 | User-selected portable JSON, not an owned database | Bounded synchronous logical export and best-effort in-process rollback | Partial legacy functionality only; multi-domain requests now refused |
| BackupRecoveryJournal (new internal owner) | Trusted profile/backup-recovery/journal.sqlite3, application_id 1396855370/user_version1; owner.lock | Private bounded preimages and durable caller-owned state receipts; no production restore composition yet | Internal recovery metadata; excluded from portable archives; live integration Blocked |
| RecoveryService | A/recovery-status.json, version 1 | Bounded diagnostic conditions via QSaveFile | Diagnostic only; explicitly not trusted restore metadata, preimages or a replay instruction |
| Models/operations, alarms, captures/logs and presentation preferences | ModelOperationService A/model-operations.json; AlarmStore memory-parent/alarms.json; other owner-specific locations | Independent side effects/runtime operations; Desktop has separate preferences | Explicitly excluded from claimed application coverage; no external server termination |

This inventory follows ApplicationControllerBuilder::withStandardDefaults,
ApplicationController composition, DaemonService::initialize, StandardPathProvider,
AppSettings/JsonSettingsStore, the named SQLite owners, MemoryArchiveJobs,
DesktopBackupHelper and RecoveryService. It does not open real user stores or infer
ownership from similarly named files. Included coordinated domains: **none**.

## Consistency and admission findings

Independent per-database transactions do not establish one application-wide point.
The production owner interfaces expose neither a shared admission lease nor a
bounded owner quiescence operation. Daemon backup import has an Agent running/
approval check, but that is not a cross-owner barrier: Chat workers, accepted title
callbacks, other clients, archive workers and separate legacy C++ CLI owners are
not fenced by it. The daemon endpoint lock prevents a second daemon from owning
that endpoint; it is not a profile-wide restore/write lock across all surfaces.

ISettingsStore has individual value setters and no multi-key transaction/reload
contract. JsonSettingsStore persists each setter separately and caches the document.
IConversationStore has domain mutation APIs but no identity-preserving coordinated
restore/snapshot/reopen contract. BackupService's Chat serializer includes titles,
roles, content and timestamps but omits persisted conversation IDs, workspace
relationships, message identifiers/status/provider metadata and summary ownership.
Its import creates new conversations, so combining it with preserved Session memory
would orphan that memory. Existing in-memory rollback is lost on process interruption
and cannot provide durable preimages or independently verified global rollback.
There is no stable application-profile UUID contract shared by these owners.

Consequently no multi-domain snapshot is published, no combined restore is admitted,
and no partial multi-domain success is returned. Core rejects valid multi-domain
exports and restores with appended `BackupError::CoordinatedOperationBlocked` (8).
The native transfer path rejects export/importBegin with the same bounded code before
creating transfer state. Core repeats validation for direct callers and existing
settings actions. Malformed/unsupported input retains its earlier validation errors.
Legacy multi-domain files can be inspected and one domain explicitly selected for
an independent portable import. Repeating single-domain operations is not a
coordinated operation and has no application-wide recovery guarantee.

No owner locks are acquired for the blocked path, no active runs are cancelled,
and no external model processes are stopped. Existing single-domain and P8-05
cooperative deadlines/cancellation remain as documented; no new wait guarantees.

## Combined format, durable journal and restore contract

The **combined container and live restore orchestration remain Blocked**. No newly
supported combined archive format/version exists. Legacy portable JSON remains v1
(16 MiB bounded, whole-document parsing), not a streaming combined backup. Existing
scoped-memory v2 streaming, limits, strict EOF/truncation/hash/version/authority
checks and v1 compatibility are unchanged. Hashes are integrity checks, not
authenticity; no encryption, signing or key management.

### Durable internal journal primitive added on continuation

`BackupRecoveryJournal` is an internal Qt SQL persistence owner, not a parallel
backup service or an IPC restore endpoint. Its caller must supply the already
trusted canonical profile directory and profile UUID, own quiescent authoritative
domain readers, perform archive validation, and control any domain replacement.
It **does not create a production profile identity, admit writes, validate an
archive, replace a live file or replay a tool/action**. Consequently the original
combined-operation guard remains enforced. `validated()` records the caller's
completed validation decision; it does not itself parse an archive.

The fixed local path is trusted-profile/backup-recovery/journal.sqlite3, accompanied
by owner.lock. The journal has application_id 1396855370, user_version 1, exact
allowlisted recovery/preimages SQL schemas, canonical bounded versioned JSON metadata,
and per-domain preimage hashes plus the expected restored digest. Metadata binds the
caller-provided profile and operation UUIDs, archive digest, state, monotonic sequence,
domain IDs and schema versions. Domain IDs are Memory=1/schema7 and
Conversations=2/schema6; no permission/credential/settings domain is accepted.
Metadata and preimages stay local and private; they are not implicitly exported.
The primitive stores opaque already-prepared owner snapshots, not a claim that raw
copies of open SQLite files are safe snapshots.

The implemented transitions are Preflight → Validated → Prepared → Applying →
Committed, with explicit RecoveryRequired from Prepared/Applying and verified
RolledBack alternatives. Preparation stores preimages and the Prepared receipt in
one SQLite transaction and reads the stored chunks back to verify hashes before
commit. Cancellation/read/write/limit failure during preparation rolls the journal
transaction back to Validated. Applying rechecks every stored preimage before its
receipt can be committed. Committed requires all supplied authoritative readers to
match expected postimage hashes; RolledBack after preparation requires all of them
to match verified preimage hashes and sizes. A mismatch retains the nonterminal
state and its recovery-required status. Cancelling before preparation needs no
preimage proof because no Applying receipt could have been issued.

Both terminal decisions are **journal receipts about trusted caller-provided
readers**, not proof that production owners were quiesced or reinitialized. No live
orchestrator currently calls them. A caller that substitutes unrelated readers
cannot establish application consistency; that binding is a remaining integration
requirement, not delegated to QML or serialized archive metadata.

Records use SQLite DELETE journaling, synchronous=EXTRA (read back as 3),
fullfsync=ON (read back as 1) and a one-second SQLite busy timeout. EXTRA adds a
parent-directory sync at DELETE-journal commit; see the primary
[SQLite synchronous contract](https://www.sqlite.org/pragma.html#pragma_synchronous).
The platform helper fsyncs the profile directory after journal-directory creation
and the journal directory after schema creation. A successful state transition is
reported only after the SQLite commit and subsequent metadata readback. This is
subject to OS/filesystem/device honoring sync operations; tests simulate process
interruption, not power loss or broken storage hardware.

At most two domains, 256 MiB/4,096 chunks per domain, 64 KiB per chunk, 8 KiB metadata
and a 600 MiB existing journal-file opening cap are accepted. Chunk processing has
cooperative 30-second checks and preparation supports cancellation before publication;
these are not hard bounds on stalled OS/QIODevice calls. SQLite storage overhead,
rollback journal and source/destination staging require additional disk space; the
primitive does not reserve disk space for future application replacement. No complete
combined backup is loaded into memory. Selected preimage reads verify first, copy to
caller-owned staging and verify the copied stream again; partial/failed output must
never be published by a caller. No destination path is read from journal metadata.

The Unix platform boundary validates canonical trusted profile ancestry, owner UID,
non-writable ancestors (with root-owned sticky temporary directories allowed), private
journal directory/file permissions, regular non-symlink files, single-link file
identity and device/inode continuity. Replacement, symlinks, hardlinks, wrong profile,
unsupported versions, unexpected schema/triggers/views, malformed metadata, undeclared
domain chunks and content corruption fail closed. Lock acquisition is immediate;
QLockFile age-based stealing is disabled. A second live owner cannot open the slot;
the crash tests verify dead-process lock recovery. Operations refuse use from another thread; destruction remains an owner-thread
caller requirement, as with other Qt SQL owners. Files/directories are created atomically with 0600/0700 modes, including under
umask 0; no permissive-creation window is introduced. Relative paths are rejected.
Darwin extended-ACL-bearing paths are currently refused rather than assuming mode
bits fully describe their access. The native file-security/directory-sync checks are isolated behind
RecoveryJournalFileSecurity; Windows currently returns UnsupportedPlatform. Unix
source portability is preserved, but native Fedora behavior was not run here.

An inspect result explicitly says MetadataOnly and never automatically applies anything.
Prepared/Applying/RecoveryRequired demand a completion/recovery decision; they are not
silently relabelled RolledBack. Unsupported/corrupt metadata inspection reports recoveryRequired rather than
Healthy. MetadataOnly inspection does not claim that every retained preimage was
rehashed; applying/readPreimage perform those separate checks. Only the fixed, owned, profile-bound journal is parsed; arbitrary
staging files and RecoveryService's diagnostic JSON are not trusted restore metadata.
Hashes do not authenticate artifacts against an attacker controlling the same OS user;
there is no automatic destructive action even for a structurally valid journal.
Terminal preimages remain retained. There is deliberately no reset/delete/prune API;
a subsequent lifecycle/archive policy is needed to reuse the occupied slot safely.

Before enabling a combined operation, integration must still establish stable profile
identity; fence admission across every mutator and stale callback; acquire bounded
owner quiescence without terminating active work; create supported SQLite snapshots;
preserve conversation/workspace IDs and authority; validate versions/space/limits;
bind journal readers to actual owners; define replacement/commit/reinitialization
boundaries; invalidate caches/client generations; and independently verify all domains.
No cross-filesystem atomic-rename or automatic destructive recovery promise is made.

## Core, daemon and Desktop behavior delivered

BackupService::availability is the capability authority, reused by SettingsService
and the existing generated settings query. It reports Blocked, finite domain
inventory, reason OwnerQuiescenceAndDurableRecoveryUnavailable, no included coordinated
domains, maximumSelectedDomains=1 and unavailable recovery inspection. A null last
verified coordinated backup is unknown/unimplemented tracking, not a successful
verification or proof that no interrupted operation exists. Portable availability
now checks actual store readiness and refuses the managed-memory legacy format.
Diagnostics contain fixed domain/status codes, no content or personal absolute paths.

Settings/Backup initially selects basic settings only. Selecting another portable
domain deselects the previous one; sensitive workspace export is not enabled by
default. The visible warning explains coordinated recovery is unavailable and directs
managed memory to authorized scope archives. Existing daemon recovery-required
conditions remain visible in the recovery card. New deliberate restore confirmation
initially focuses Cancel and returns focus to the unchecked review checkbox after
closing; Core remains authoritative even if QML selection is bypassed. Loading,
disconnected, file-error, empty import selection, busy/progress and cancellation
remain explicit. No unavailable recovery/rollback action is presented as functional.
No new top-level IPC method or generated signature; both generators must remain clean.

## Memory, index and history preservation

No P8-01–P8-05 storage, authorization, import mutation, ContextBudgetManager,
ModelBinding, ToolExecutionGateway, AgentLoop or process lifecycle implementation
is changed. Existing scope-authorized memory archives preserve UUIDs, provenance,
workspace/session binding and receipts; import regenerates revisions and updates
source/FTS in one SQLite transaction. Accepted in-flight requests retain their
existing immutable context, and scoped restore retains existing cache invalidation.
These are memory-only guarantees; no combined-restore guarantee is inferred.

The 256-receipt limit remains non-destructive. Existing 16-receipt history inspection
and v2 history preservation remain the available foundation. Archives still refuse
records at capacity so a required import receipt cannot silently drop history.
A separate explicit external-history format and attribution/retention decision is
needed before capacity relief; no automatic pruning/compaction/retention duration
or background embedding generation is added.

## Process-interruption and focused evidence

New production-owner tests use disposable JsonSettingsStore,
SQLiteConversationStore and SQLiteMemoryStore. They verify multi-domain refusal for
both Merge and Replace, empty rejected export payload, unchanged settings bytes and
conversation UUID/count, bounded privacy-safe capability state and managed-memory
availability. Real daemon transfer tests verify no transfer ID is allocated on
multi-domain export/importBegin, and the existing single-domain integrity,
chunking, cancellation and socket isolation path remains exercised.

The real production QML/daemon keyboard flow covers blocked availability, a selected
single-domain file export/inspection, deliberate restore prompt, Cancel focus and
accessible Button role, cancellation without settings-byte changes, focus restoration,
unchecked confirmation, invalid file/empty selection and disconnected export refusal.
Compact 640 px and wide 1280 px screenshots, plus confirmation, were rendered and
visually inspected under build/tests/desktop-daemon-verification/p806.

**Blocked, not tested as implemented:** multi-domain concurrent-writer exclusion,
active Chat/Agent quiescence, profile-compatible combined container validation,
combined disk-space/write failure, interrupted combined preflight/apply/commit,
automatic journal replay, verified live multi-domain rollback, service reinitialization,
stale-client fencing and cross-domain session/FTS recovery. Existing P8-05
crash-before-memory-commit tests establish only their memory transaction guarantee.
The new real subprocess journal tests call _Exit at Preflight, Validated,
Prepared, Applying and Committed, plus inside an uncommitted preimage insertion.
Reopening retains the prior committed journal receipt and verified preimages; an
interrupted prepare remains Validated, and post-Applying interruption is visible
as requiring recovery. Verified rollback is exercised after restart. This proves
journal boundaries, not atomic live application restoration.

A production-owner offline preimage test creates real SQLiteMemoryStore and
SQLiteConversationStore data, closes both owning Qt SQL connections, stages their
quiescent database bytes, recovers into new test-owned files and reopens through
the production owners. Conversation/session UUID, scoped memory UUID/owner,
foreign-session exclusion and Ready scoped FTS5 are verified. No open/live database
is overwritten. Additional tests cover profile/lock mismatch, future format,
wrong application ID, unexpected triggers, corrupt metadata/preimages, removed
receipts, symlink/hardlink/file replacement, permissions, umask-0 creation, relative-path
refusal, cancellation, unsupported
domain schema, independent two-domain terminal proof and directory-write failure.
These cases are implemented against the actual journal SQLite owner, not a mock
that reports success.

## Validation results

Platform: macOS 27.0.1 arm64, Qt 6.12.0, Apple LLVM 21. Existing ignored
build/tests holds raw attempts; none is relabelled as a pass.

| Check | Actual result |
|---|---|
| Configure | `cmake --preset tests` passed; p806-configure.log |
| Complete C++ build | Passed p806-build-final-security.log after atomic creation/path hardening; existing tests preset/build reused, CCACHE_DISABLE=1, -j6 |
| Focused first attempt | 7/9 passed, 47.69 sec; UI focus defect plus existing nested-sandbox MCP fixture failure; p806-focused.log |
| Journal / interruption / production preimages | **22 Qt rows passed, zero failed/skipped**, 1.02 sec CTest; p806-journal-tests-security-final.log |
| Earlier full with journal | **167/173 passed, 6 nested-sandbox-related executable failures**, 210.20 sec; p806-journal-ctest-full.log; precedes final atomic creation/path hardening |
| Earlier focused Core/P8 + QML | **8/8 passed, 9.42 sec**; p806-focused-final.log |
| Isolated real backup IPC | **3 Qt rows passed, zero failed/skipped, 2.478 sec**, including init/cleanup; p806-backup-ipc-final.log |
| Corrected real daemon/QML flow | 1/1 passed, 8.99 sec; p806-ui-final.log; compact/wide and Cancel screenshots inspected |
| Initial full before journal | **166/172 passed, 6 failed, 211.67 sec**; p806-ctest-full.log; retained earlier receipt |
| Final sequential full CTest | **167/173 passed, 6 failed, 214.64 sec**; p806-ctest-delivery.log; -j1, disposable profiles and Keychain denial; not a passing full-suite receipt |
| Focused set in final full run | All **9/9** journal/P8/Core + daemon-QML executables passed; backupTransferIntegrityAndIsolation also passed inside the otherwise failing Desktop IPC executable |
| Rust fmt | Passed cargo fmt --all -- --check; p806-rust-fmt.log |
| Rust Clippy | Passed --workspace --all-targets --locked --offline -- -D warnings; p806-clippy.log |
| Rust workspace tests | 142 passed, zero failed/ignored, locked/offline; p806-rust-tests-permitted.log |
| IPC generators | Both --check passed; p806-ipc.log; no generated method/signature changes |
| QA registry/schema | Passed; existing missing external roasd.packaging evidence warning retained; p806-qa-final.json; 31 registry tests passed |
| Pinned clang-format | 18.1.8 changed C++ hunks produce empty diff; p806-format.diff |
| Scoped clang-tidy | Original three changed units and both new journal/platform sources exited 0 with zero reported project diagnostics after corrections; p806-tidy-0-final.log, p806-tidy-{1,2}.log, p806-journal-tidy-delivery.log |
| Personal paths / credentials | Privacy scan zero files flagged; 20 changed files scanned for private-key/GitHub/AWS/OpenAI signatures, zero flags, contents suppressed |
| Whitespace / refs | git diff --check passed; HEAD/main/origin-main remain the verified base |

Initial build p806-build.log failed because the test used a query on
DesktopRuntimeClient that belongs to DesktopShellViewModel; it now calls the
existing generated settingsService action. The UI focus failure was corrected by
returning focus to the enabled review checkbox instead of the now-disabled restore
button. The first Rust run failed on local socket PermissionDenied; a subsequent
permitted offline fixture run passed. First tidy found two missing braces in new
guards; corrected and rerun. New journal analysis additionally found brace style,
static QString initialization, numeric conversions and condition-side-effect
warnings; these were corrected before the clean final journal analysis. The first
journal build exposed a QVariant::toInt signature mistake; fixed before passing
journal/process tests. The first ACL-hardened run failed because Darwin returns
ENOENT for an existing vnode with no extended ACL; native no-ACL handling was
corrected and the final 22-row run passed with ACL-bearing paths still refused.
Absolute-path checks and atomic 0600/0700 creation were added during final review;
the final complete build and full 173-executable run include those changes. These intermediate failures remain in their raw logs.
Full builds retain expected ad-hoc macOS signing messages; test font fallback and
intentional rejection warnings remain. Scoped tidy suppresses unrelated/dependency
diagnostics (final new journal/platform analysis suppresses 876 non-user warnings;
final BackupService 589; SettingsService and DaemonIpcServer retain their
own suppression totals); this is not a whole-repository zero-warning claim.

The full suite remains under a protective outer sandbox. Existing MCP/plugin/process
fixtures that need to invoke sandbox-exec themselves can fail with
`sandbox_apply: Operation not permitted` under that nested sandbox. Such results
are validation Blocked/failures, not new successful native coverage; no policy is
weakened and no fixture is rewritten to always succeed. Synthetic provider tests
never establish live-model or native Fedora/Windows production acceptance.

Full-suite failed executables (retained, not converted to SKIP):
`test_mcp_integration`, `test_plugin_tool_integration`,
`test_real_tool_executor_tools`, `test_process_executor`, `test_plugin_manager`,
and `test_desktop_ipc`. The process failures include explicit nested sandbox-exec
Operation-not-permitted errors; related plugin/MCP expectations consequently fail.
The Desktop suite's sole failed case is its existing real MCP subprocess lifecycle
case; its backupTransferIntegrityAndIsolation case passes. The eight focused Core/P8 executables (including the new journal) and the
production Desktop daemon E2E all pass in the final full run. No unsandboxed C++ rerun accessing the user's Keychain was used to hide these
validation limitations.

The verbose full run records **11 internal Qt SKIPs, 2 XFAILs and 69 QWARN rows**
(counting the CTest-prefixed original rows, not duplicated on-failure replay).
Preserved conditional rows:

- CredentialStore: missingLinuxHelperRefusesPersistence (Linux-only) and
  testPlatformCredentialBackend (native credential write refused).
- AgentReliabilityHarness: cor11 native incremental tool-call assembly remains blocked.
- AgentRuntime: asyncRunCommandStreamsAndContinues,
  asyncRunCommandCancellationAndTimeout,
  asyncDockerUsesProcessExecutorAndPreservesRestrictions and shutdownStopsActiveCommand
  retain the strict macOS child-creation limitation.
- ObservationPolicy: optInLiveClassification remains opt-in and was not activated.
- RealToolExecutorTools: browserToolsReportMissingNodeGracefully skips because npx is
  installed; gitInspectionUsesFixedSandboxedArguments additionally skips because
  this host run cannot enforce its required nested process sandbox.
- LocalModelRuntime: removalDuringActualUseRemainsBlocked retains the missing lease
  contract. XFAIL providerSwitchRetainsOwnedChild and
  endpointSwitchDoesNotReconcileOwnedChild preserve P3-02 D4/D5b.

Warnings include intentional rejection paths and font/platform limitations. Lower
warning counts than an earlier phase do not imply repaired process behavior; some
process fixtures fail before their later warning paths. The raw logs remain the
source of exact diagnostics and outcomes.


## Remaining work

1. Introduce owner lifecycle/admission contracts covering all persistence writers and
   asynchronous callbacks, including external legacy composition and worker jobs.
2. Establish stable profile identity and identity-preserving conversation/workspace
   snapshots; explicitly exclude grants/credentials and sensitive scopes by default.
3. Implement the bounded combined stream and integrate the journal primitive with
   owner-bound replacement/recovery, space checks and platform persistence acceptance.
4. Add process interruption/fault injection against the actual composed owners, then
   daemon generation fencing, reload/cache invalidation and explicit recovery UI.
5. Review external history archival independently; keep Personal sharing, encryption,
   embeddings and process/resource ownership changes separate.

## Complete changed-file inventory

- apps/sentinel-daemon/service/DaemonIpcServer.cpp
- core/CMakeLists.txt
- core/include/sentinel/core/app/BackupRecoveryJournal.h
- core/include/sentinel/core/platform/RecoveryJournalFileSecurity.h
- core/src/app/BackupRecoveryJournal.cpp
- core/src/platform/RecoveryJournalFileSecurity.cpp
- core/include/sentinel/core/app/BackupService.h
- core/src/app/BackupService.cpp
- core/src/app/SettingsService.cpp
- docs/audits/P8_06_COORDINATED_BACKUP_RECOVERY_RESULT.md
- docs/concepts/MEMORY.md
- docs/development/ARCHITECTURE.md
- docs/interfaces/IPC.md
- tests/CMakeLists.txt
- tests/core/test_backup_recovery_journal.cpp
- tests/core/test_scoped_memory.cpp
- tests/core/test_settings_recovery_runtime.cpp
- tests/desktop/test_desktop_ipc.cpp
- tests/helpers/DesktopDaemonInteraction.h
- ui/qml/pages/settings/SystemSettingsTab.qml

## Maintainer-authorized publication

After the uncommitted handoff, the maintainer explicitly authorized committing and
pushing this P8-06 feature branch. The validation receipts above remain evidence for
the implementation candidate, with the six failures preserved. Publication does not
complete the blocked live restore integration or authorize a merge into main.
