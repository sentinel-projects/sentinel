# P8-05 — Advanced Memory Archives, Consistent Snapshots & Recovery

Status: Implemented memory-only archive milestone; combined application backup remains **Blocked**.
Implementation was delivered uncommitted for maintainer review. The maintainer subsequently
authorized committing, separately pushing the feature branch, and integrating/pushing main.
Native-platform production acceptance is not claimed.

## Repository and audit

Base: `086e8dc80988c4b16ae8328515f8570422d77ea7` (verified clean `main == origin/main`).
Branch: `feature/p8-05-memory-backup-recovery`.
Origin: `https://github.com/sentinel-projects/sentinel.git`.
P8-01 `19f223e9`, P8-02 `514d2741`, P8-03 `64f66ac5`, P8-04 `086e8dc8`
were verified integrated before branch creation. Read AGENTS, architecture/runtime/
security/build/testing instructions and all four phase reports. Personal-Brain CLI
was unavailable. One coding chat was active; no delegated writers, clone, worktree,
new build tree, commit, push, merge, stash, reset, clean, rebase or workflow dispatch.
Initial free space approximately 82 GiB. Existing `build/tests` and `cli/target` reused.
Tests use disposable profiles, workspaces, selected files and SQLite databases only.

Existing authoritative SQLiteMemoryStore / SQLiteMemoryManagement implement scoped
CAS mutations, per-operation transactions and FTS5 trigger updates; InMemoryStore
remains the lightweight backend. ApplicationController resolves persisted session
and verified workspace authority. Existing BackupService v1 is bounded and blocks
lossy generic managed-memory backup. P8-04 schema 6 has scoped FTS5 projection health,
bounded explicit repair and no automatic embeddings. Desktop uses canonical typed
`desktop.action` management through the daemon; QML is presentation-only.
The prior v1 archive collected at most 500 records / 96 KiB, lacked a cross-process
snapshot and retained at most 64 correction receipts. Generic restore spans separate
storage owners and cannot roll back all of them atomically.

## New archive and snapshot design

`MemoryArchive` provides bounded QIODevice streaming. Version 2 is deterministic
compact-JSON NDJSON: header fixes format/version/schema/scope; each ordered chunk
contains at most 100 records / 1 MiB and its SHA-256 over canonical record JSON.
The footer manifest binds exact header/chunk bytes, their order, UUID sequence,
record/chunk counts, scope and versions. Record objects retain UUID, scope, owner,
workspace, provenance, source, creation/modification dates, values and all receipts.
Snapshot `exportRevision` is retained in the archive but is not reused as a future
CAS token. Hashes establish integrity, not authorship, encryption or authentication.
Strict metadata, UUID order, chunk shape, versions, footer and EOF checks reject
truncation, duplication, reordering, corrupted metadata and trailing data.

Default limits are 64 MiB / 10,000 records. Valid explicit limits are 96 KiB–256 MiB
and 1–100,000 records; Desktop selects whole MiB. Single serialized records must
fit 1 MiB minus 1024 bytes. Allocation is chunk-bounded, not archive-sized. Native
transfer is 32 KiB/message. At most two private staging jobs exist, so configured
staging payload capacity is at most 512 MiB plus bounded overhead; selected source
and destination files are additional disk use. No disk reservation is promised.
v1 compact/pretty JSON remains readable under its unchanged 500 / 96 KiB / 64
receipt protections; the v1 exporter still refuses receipts at its safety boundary.

SQLite snapshot export uses a dedicated worker-owned read-only Qt SQL connection,
one read transaction and forward-only authorized UUID-ordered cursor. Schema 7 adds
`memory_archive_order(scope,owner_id,workspace_id,id)` to avoid collection-sized
sorting. Schema migration preserves all records/receipts and does not backfill or
expand FTS authority. Snapshot includes revisions and history from one database
state despite concurrent writers. SQL busy wait is one second; workers cooperatively
cancel at row/chunk boundaries after 30 seconds. These are not hard realtime bounds
on stalled OS/filesystem calls. A read snapshot can retain WAL pages temporarily.

## Restore, interruption and index consistency

Upload is private staging; preflight validates full structure, limits, hashes and
exact authorized scope/owner metadata without applying records. Confirmation selects
reject (default) or explicit skip-identical. Conflicting UUIDs are never replaced.
Conflict checks against current authoritative records happen again at restore time;
preflight does not reserve records or promise freedom from later conflicts.

Restore constructs a connection on its worker, uses `synchronous=FULL` and one
`BEGIN IMMEDIATE`, revalidates the file and invokes the same authoritative mutation
implementation inside that outer transaction. Imports append an actual local import
receipt and regenerate revisions. Source writes and FTS triggers commit together.
After parsing, scoped index health must be Ready; unsupported/missing/stale projection
fails restore with IndexNotReady and rolls back. Existing explicit bounded refresh
is required before retry; no automatic full reindex or schema-recovery bypass.
Legacy is inspectable/exportable only explicitly and remains excluded from recall.

All parse/authority/conflict/cancel/write/index/commit failures return explicit
failure, never partial success. Cancellation before commit rolls back. The commit
boundary is mutex-protected: a cancellation racing after it cannot falsely claim
rollback. Process-interruption tests exit after uncommitted writes; SQLite restart
recovers source and FTS to the prior state. No auto-resume/replay/commit occurs.
Disk-full handling is verified with a quota-failing QIODevice, not exhaustion of the
host filesystem; database write/commit errors use the same rollback path.

Staging is owner-only, marked and exclusively locked; lock age stealing is disabled.
On restart only generated non-symlink `transfer-*.part` files inside the marked
owned directory are reaped under that lock. Inactive jobs expire lazily after ten
minutes on another begin. Data files and selected user files are never cleanup
candidates. Native export writes a private sibling temporary file, verifies size and
wire hash and publishes only a new destination filename, refusing overwrite.

## History and product integration

History is preserved without pruning/compaction and capped at 256 receipts. New
operations needing a receipt fail at capacity rather than silently deleting history.
v2 export/preflight also refuses records already at capacity, so restore always has
room for its required import receipt. Such records require a future explicit history
archival policy before another scope archive can be created.
`history` reads exact authorized record pages of 16 with revision and history hash.
Receipts preceding the latest imported event are labelled unverified imported
history; that locally recorded import and subsequent local events are distinct.
Historical receipts cannot authenticate external source claims. Invalid Unicode,
malformed metadata/receipts and invalid previous digests are rejected. No invented
retention duration, automatic durable promotion or LLM edits were added.

MemoryPage integrates native file dialogs, configurable limits, transfer progress,
worker record counters, explicit cancel, preflight summary, reject/skip-identical
choice, safe initially focused Cancel, completion/error messages and paged History.
Existing v1 controls remain available under Archive options. New content uses plain
text. Scope controls are disabled during transfer and focus is restored on completion.
DesktopBackupHelper validates local regular non-symlink source/new target boundaries;
selected paths never become daemon IPC commands. Core owns all scope authorization.
IPC uses existing canonical manageMemory(object); documented suboperations and types
in IPC.md introduce no top-level schema command, and both generator checks pass.

Jobs freeze session/workspace/scope and explicit Personal/Legacy flags, then check
that binding on each call. Transfer/control of an existing job is admitted during an
Agent run; begin/restore commit remain subject to runtime-busy admission. Tagged
responses/errors prevent silent hanging or redirecting another request. A queued
successful-restore callback invalidates memory projections even without a later poll.
New Chat/Agent requests see imported records under existing budget/authority; in-flight
requests retain immutable accepted context. ToolGateway and AgentLoop acceptance are
unchanged. Personal sharing and P8-02 unknown-image-token refusal are unchanged.

## Combined application backup remains Blocked

Settings/workspace profiles/extensions are owned by AppSettings/ISettingsStore JSON;
conversation/message history by SQLiteConversationStore; scoped/Legacy memory and FTS
by SQLiteMemoryStore. Existing generic BackupService supports its existing non-memory
domains, but has no durable coordinated snapshot/preimage/commit journal across these
owners. Restoring one then failing another cannot guarantee authorized rollback.
Credentials, permission grants and agent-run stores are separate and excluded.
The existing managed-memory generic-backup guard remains intact. This milestone
ships memory-only archives, never a complete application-backup claim.

## Verification

macOS arm64 / Qt offscreen, disposable profiles, deterministic fixtures; no provider
calls, downloads or real user database changes. Logs are ignored `build/tests/p805-*`.

| Check | Actual result |
|---|---|
| Configure / complete C++ build | Passed tests preset; full build p805-build-full.log, final full delivery rebuild p805-build-delivery.log exit 0 |
| Sequential full CTest | **172/172 passed, zero failures, 240.10 seconds**, `-j1 --timeout 180 -V`, p805-ctest-final.log |
| Final focused delivery | **8/8 passed, 37.53 seconds**, p805-focused-delivery.log, including the late history-capacity guard |
| Archive / P8 regressions | Archive 9, scoped 9, management 7, retrieval 8, SQLite 15, ContextBudgetManager 10 Qt cases passed |
| Chat / Agent | ChatModeService 20 Qt cases; actual Agent memory context and full runtime/loop/evidence suites passed under existing budgets |
| Desktop / IPC / QML | Desktop IPC 74 Qt cases; real daemon-backed native file workflow, safe keyboard confirmations and History passed; compact/complete screenshots visually inspected |
| Rust fmt / Clippy | Passed fmt check and workspace/all-targets/locked/offline Clippy with `-D warnings`; existing cli/target reused |
| Rust workspace tests | **142 passed**, zero failures/ignored; p805-rust-tests.log |
| IPC generators | Both canonical generators passed `--check`; existing object action signature reused |
| QA registry/schema | Valid, zero errors; existing missing external `roasd.packaging` evidence warning retained |
| Pinned clang-format | 18.1.8; new C++ files fully checked and changed-line diff empty |
| Scoped clang-tidy | All 12 changed production translation units exit 0, zero reported project diagnostics; final MemoryArchive guard rerun exit 0 |
| Privacy / credentials | Personal-path scan zero flagged files; changed-file credential signatures zero flags, contents suppressed |
| Whitespace / Git | `git diff --check` passed; base/main/origin refs unchanged, all 33 changed files reviewed before the subsequently authorized publication |

Full CTest includes **10 internal Qt SKIPs, 2 XFAILs and 81 QWARN lines**.
Preserved conditions (not relabelled as successful native coverage):

- SKIP   : CredentialStoreTest::missingLinuxHelperRefusesPersistence() Linux Secret Service helper contract.
- SKIP   : CredentialStoreTest::testPlatformCredentialBackend() Native credential write refused; no real smoke success claimed.
- SKIP   : AgentReliabilityHarnessTest::cor11_cancelAfterFirstStreamedDelta(native-tool-assembly) COR-11 native incremental tool-call assembly remains explicitly blocked: IChatProvider exposes no typed incremental tool-argument assembly event, so a partially streamed native call could only be dispatched after a validated terminal assembly. This fixture asserts the native request stays on the bounded non-streaming path (one sendRequest, zero streamed deltas, zero public deltas) and does not invent an assembly contract.
- SKIP   : AgentRuntimeTest::asyncRunCommandStreamsAndContinues() This shell fixture requires child creation; strict macOS plans deny forks. Native host lifecycle is covered by plugin integration tests.
- SKIP   : AgentRuntimeTest::asyncRunCommandCancellationAndTimeout() This shell fixture requires child creation; macOS strict no-detached-child plans now deny all forks. Plugin host cancellation is covered by real host integration tests.
- SKIP   : AgentRuntimeTest::asyncDockerUsesProcessExecutorAndPreservesRestrictions() The fake Docker shell fixture requires child creation; strict macOS plans deny forks.
- SKIP   : AgentRuntimeTest::shutdownStopsActiveCommand() This shell fixture requires child creation; strict macOS plans deny forks. Native host shutdown is covered by plugin integration tests.
- SKIP   : ObservationPolicyTest::optInLiveClassification() Explicit opt-in required for production local-provider probe
- SKIP   : RealToolExecutorToolsTest::browserToolsReportMissingNodeGracefully() npx is installed on this machine; the missing-node branch cannot run.
- SKIP   : LocalModelRuntimeTest::removalDuringActualUseRemainsBlocked() No model-residency lease/removal contract exists; a live provider handle is not proof of active model use. Source-only D6.
- XFAIL  : LocalModelRuntimeTest::providerSwitchRetainsOwnedChild() P3-02 D4: switching provider returns before owned cleanup
- XFAIL  : LocalModelRuntimeTest::endpointSwitchDoesNotReconcileOwnedChild() P3-02 D5b: same-model endpoint switch leaves old owned child


Focused evidence covers 650-record snapshot/round-trip with a concurrent writer;
byte-limited and record-limited chunk boundaries; v1 compatibility; future versions;
wrong workspace; malformed/tampered/reordered/duplicated/missing/truncated payloads;
export/import/precommit cancellation; duplicate conflict and skip-identical; failed
restore rollback retaining unrelated records; crash-after-uncommitted-write recovery;
FTS health/restart; 70-correction v2 history preservation; Chat/Agent prompt visibility
without budget changes; private staging recovery; real daemon/native 550-record
transfer and heartbeat, no-overwrite, confirmation cancellation, restore invalidation,
and keyboard-driven real QML export/import/History paths.

Earlier verification failures are retained in raw logs. The first long Agent fixture
record correctly exceeded the existing memory budget; the fixture now uses a short
value instead of changing policy. Standard QML Button confirmation uses Space, not
Return; the keyboard fixture was corrected without weakening defaults. Existing Qt
PCH mismatches required refreshing generated PCH dependencies in the same build tree.
An intermediate QVariant default conversion and a misplaced daemon admission block
were corrected before the final build. The late 256-receipt export/preflight guard
was added while the full suite was underway; the delivery rebuild and focused
regressions cover that localized change. A ccache sandbox write denial during that
rebuild was resolved by disabling ccache, without changing source or build location. The restricted full-test attempt was stopped
because sockets/sandbox-exec fixture processes were denied; its log is separate from
the final run with local fixture permissions. Compilation is not claimed warning-free:
existing HuggingFaceModelSource.cpp:91 shadow warning and platform/signing/font/test
rejection warnings remain. No synthetic result establishes native Fedora/Windows or
live-provider production acceptance.

## Remaining limitations and recommended P8-06

- Combined multi-domain backup remains blocked pending a durable coordinated recovery
  contract across actual storage owners.
- Archives are plaintext with integrity only; encryption, signing and key management
  require a separately reviewed format/product decision.
- Deadline/cancellation is cooperative, and ordinary snapshot WAL retention is possible.
- Jobs are context-bound: changing the active conversation/workspace can reject later
  controls. Workers retain the original authority and deadline; no redirected restore
  occurs. Transport/context loss can leave client outcome unknown: inspect before retry.
- Preflight validates the archive, not a reserved database snapshot of future conflict
  decisions. Differing UUIDs report conflict at atomic restore, with no replacement.
- History capacity is finite, with no pruning; long-term non-destructive compaction/
  external history storage and explicit retention policy remain future work.
- Severe SQLite/FTS corruption needs a separate bounded recovery milestone; no physical
  erasure, removal from historical backups or authenticated historical evidence claim.
- Native disk-full is injected at the writer boundary, not real host disk exhaustion;
  no crash-at-every-machine-instruction durability certification is claimed.

Recommended P8-06: coordinated backup/recovery journal and recovery UI across storage
owners, explicit archive encryption/authentication design, non-destructive history
archival/compaction, and stronger bounded corruption recovery. Keep provider-specific
image accounting, Personal sharing, semantic embeddings and process/resource ownership
changes as separate authorized work.

## Complete changed-file inventory

- `apps/sentinel-daemon/service/DaemonIpcServer.cpp`
- `apps/sentinel-desktop/CMakeLists.txt`
- `apps/sentinel-desktop/include/sentinel/desktop/DesktopBackupHelper.h`
- `apps/sentinel-desktop/src/DesktopBackupHelper.cpp`
- `apps/sentinel-desktop/src/DesktopMemoryArchive.cpp`
- `apps/sentinel-desktop/src/DesktopRuntimeClient.cpp`
- `core/CMakeLists.txt`
- `core/include/sentinel/core/app/ApplicationController.h`
- `core/include/sentinel/core/memory/MemoryArchive.h`
- `core/include/sentinel/core/memory/MemoryArchiveJobs.h`
- `core/include/sentinel/core/memory/SQLiteMemoryStore.h`
- `core/src/app/ApplicationController.cpp`
- `core/src/app/ApplicationMemoryManagement.cpp`
- `core/src/memory/MemoryArchive.cpp`
- `core/src/memory/MemoryArchiveJobs.cpp`
- `core/src/memory/MemoryManagement.cpp`
- `core/src/memory/SQLiteMemoryArchive.cpp`
- `core/src/memory/SQLiteMemoryManagement.cpp`
- `core/src/memory/SQLiteMemoryStore.cpp`
- `docs/audits/P8_05_MEMORY_BACKUP_RECOVERY_RESULT.md`
- `docs/concepts/MEMORY.md`
- `docs/development/ARCHITECTURE.md`
- `docs/interfaces/IPC.md`
- `tests/CMakeLists.txt`
- `tests/core/test_chat_mode_service.cpp`
- `tests/core/test_memory_archive.cpp`
- `tests/core/test_memory_management.cpp`
- `tests/core/test_memory_retrieval.cpp`
- `tests/core/test_scoped_memory.cpp`
- `tests/core/test_sqlite_memory_store.cpp`
- `tests/desktop/test_desktop_ipc.cpp`
- `tests/helpers/DesktopDaemonInteraction.h`
- `ui/qml/pages/MemoryPage.qml`
