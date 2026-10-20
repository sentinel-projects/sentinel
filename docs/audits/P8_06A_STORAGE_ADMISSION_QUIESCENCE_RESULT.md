# P8-06A — Storage admission, quiescence and write fencing

Status: **Partial — application-wide preparation remains Unsupported/Blocked.**
No combined backup/restore or live database replacement is enabled. This change is
an enforced storage-owner foundation, not a completed consistent application backup point.

## Prerequisites and base

The existing checkout and remote were verified. Personal-Brain's `brain` executable
was unavailable; no alternative workspace was guessed. Repository AGENTS and
architecture, Agent Runtime, security, building and testing guidance were read.
P8-01 through P8-05 are ancestors of the P8-06 candidate. The maintainer
explicitly authorized committing/pushing P8-06 and then integrating main. Following
an initial automatic rejection of a default-branch push, the maintainer explicitly
approved `git push origin main`; it succeeded. Before branch creation the tree was
clean, main and origin/main both resolved to
`83db1a473afbf3da26b4590737cdb87018cf8da7`.
P8-06 is committed/integrated but its coordinated backup definition of done remains
unmet, as stated in its audit. Branch `feature/p8-06a-storage-quiescence` was created
without overwriting an existing branch. No competing active coding chat or unresolved
conflicts were found. Existing build/tests and cli/target were reused. P8-06A changes
remain uncommitted; no P8-06A publication or PR was performed.

## P8-06 validation gate

The six failing executables were reproduced using disposable profiles, offscreen Qt,
and the existing outer credential-protection sandbox. Real Keychain access remained
denied. Logs: `build/tests/p806a-preflight-<executable>.log`.

| Executable | Narrow reproduction / disposition |
| --- | --- |
| test_mcp_integration | realStdioAgentLoop: nested sandbox-exec reports sandbox_apply Operation not permitted; environment blocked |
| test_process_executor | runtimeCanonicalizationPreservesReadBoundary / pluginRuntimeStartsInsideSandbox: same nested launch refusal, exit 71; environment blocked |
| test_real_tool_executor_tools | registeredProcessHandlerUsesFrozenWorkspace / runCommandDockerFixtureContracts: sandbox launch refusal, exit 71 prevents fixture behavior; environment blocked |
| test_desktop_ipc | extensionsDesktopMcpRealProcessDisconnectAndRecovery: real MCP connect fails through same launcher; environment blocked |
| test_plugin_tool_integration | loadedPluginExecutesAndUnloadsSafely: environment-triggered diagnostic mismatch; PluginCrashed instead of PluginSandboxUnavailable |
| test_plugin_manager | testSamplePluginsLoading: same diagnostic mismatch; mixed environment/product classification limitation |

`PluginHostSession` maps a started launcher that exits after a sandbox_apply failure
to PluginCrashed when ProcessExecutor's record already says Enforced. The tests expect
PluginSandboxUnavailable before their platform skip. This is an explained existing
diagnostic defect, not a passing test or proof of a storage defect. Product sandbox
policies and these assertions were not weakened. Maintainer integration approval and
independent source/log analysis satisfy the gate for continuing; failures remain visible.

## Ownership and coverage

| Owner / authoritative mutation source | Coverage |
| --- | --- |
| SQLiteMemoryStore constructor/schema/FTS initialization | Lease before open/schema, when explicitly enrolled |
| put, clear, writeScopedMemory | Lease through SQL completion/rollback |
| mutateManagedMemory (create/correct/delete/move/import), clearManagedMemory | Lease around complete transaction; private applyManagedMutations inherits it |
| refreshMemoryIndex / FTS triggers | Lease around refresh; trigger projection changes share their mutation transaction |
| restoreArchive | Lease around validation/import/index checks/commit; failure rollback precedes release |
| MemoryArchiveJobs | Shared originating coordinator and immutable store generation passed into worker connection; delayed job cannot acquire the current generation afresh |
| exportArchiveSnapshot / scoped retrieval | Read-only connections; not a proof of cross-domain snapshot consistency |
| SQLiteConversationStore constructor/schema | Lease before open/schema when enrolled |
| create/append/rename/title/expected-title/summary | Lease at authoritative store entry, including streaming/completion/failure updates routed through appendMessage |
| archive/unarchive/delete, pin/unpin, import rollback, clear/prune | Lease in direct mutator or private metadata helper, including transaction rollback |
| SQLiteChatHistoryStore | Uncovered: append, clear, prune, startup schema and transcript callbacks |
| SQLiteAgentRunStore | Uncovered: event record/recovery/clear/prune; durable evidence behavior preserved |
| SQLitePermissionGrantStore | Uncovered: save/remove/clear/schema; authorization behavior preserved |
| AppSettings / ISettingsStore / JsonSettingsStore / encryption wrapper | Uncovered: all setters/removes, credential migrations and sanitizing reads; no false settings barrier |
| WorkspaceService | Pure catalog/profile transformation; persistence and active selection are AppSettings/controller writes and remain uncovered |
| ApplicationController | Standard composition owns shared coordinator; destruction shuts admission after Agent runtime shutdown; remaining async request lifetimes are not enrolled |
| DaemonService | Uses standard builder after claiming endpoint; no new IPC maintenance authority. Existing request-drain lifecycle preserved |
| Legacy CLI | Uses standard builder, thus its own process-local coordinator; cannot establish cross-process authority over daemon files |
| Independent clients/processes/custom store composition | Uncovered; no profile-wide registry/lock or claim of exclusive profile authority |
| BackupService | Existing coordinated operation refusal unchanged |
| BackupRecoveryJournal | Unchanged independent durable primitive; metadata is not admission authority |

The included mutation set is **explicitly enrolled memory and conversation SQLite
instances and their shared memory archive worker connections, within one process**.
Default standalone constructors remain compatible and unenrolled. Custom replacement
stores do not acquire coverage by being attached to a controller. No claim is made
that every SQLite connection in a profile is enrolled.

## Contract, ownership and states

StorageAdmission is owned by standard ApplicationController composition. Its shared
internal state outlives outstanding move-only Writer leases. No polling worker,
scheduler, queue, provider unloading or process termination was added. Normal writes
use one short admission mutex acquisition/release plus RAII lifetime accounting.

LeafStores scope permits Running -> Quiescing -> Quiescent -> Maintenance -> Resuming
-> Running. Preparation accepts a 0..30 second deadline, closes admission atomically,
and waits on a condition variable for the **actual writer count to reach zero**.
Concurrent preparation returns Busy. Cancellation/timeout returns Running without
advancing generation; an active writer is never forcibly cancelled. Shutdown enters
Unavailable and wakes waiters; already admitted work retains its lease until release.
The caller must explicitly resume Quiescent/Maintenance. Cancellation interrupts a
quiescing wait; it does not declare an interrupted maintenance transaction safe.

The production builder deliberately selects **LiveApplication** scope. In that scope,
prepareLocal returns Unsupported **before changing state**. prepareApplication always
returns Unsupported in either scope. This prevents an incomplete owner barrier from
interrupting accepted provider/tool callbacks or losing their durable evidence.
No IPC, QML or model-callable maintenance command was introduced.

enterMaintenance requires Quiescent and zero writers, increments generation exactly
once, and rejects repeated acquisition. Stores capture their generation at construction
and never refresh it on a write. After a leaf maintenance transition, old stores and
callbacks stay fenced after resume. A future trusted orchestrator must reconstruct
owners deliberately; this phase does not rebind live owners or redirect callbacks.
Request/session/workspace identity and existing cancellation semantics are unchanged.
No Agent action is replayed.

### Lock order

Acquire admission before opening a database, transaction, SQL mutation or archive
commit callback. The admission mutex is released before SQL or application callbacks.
Rollback/SQL guards are destroyed before Writer release. The archive job mutex may
be acquired during an admitted transaction; admission never calls into job/application
code while holding its mutex. prepareLocal must run outside a Qt event loop needed
by writers and without a writer lease, SQL transaction, job mutex or application lock.
A self-held writer yields bounded timeout rather than pretending to be quiescent.
LiveApplication refuses preparation entirely while these lifecycle integrations are
incomplete.

## Async safety and remaining limits

Store-level fencing covers actual title and append entry points: once a generation
advances, a delayed callback referencing the old store cannot regain write authority.
Memory archive worker construction is similarly fenced before schema access. This
is not complete request-lifetime coordination: accepted chat, title/summary, local
inference, Agent execution and background task lifetimes must be enrolled before live
maintenance can be enabled. The live scope refusal is essential, not a UI busy flag.
Settings/workspace mutations can still proceed while a separate leaf coordinator is
quiescent; therefore leaf success must never be presented as application success.
SQLite readers remain open; no checkpoint/close/reinitialize barrier or replacement
receipt is provided. Reads are not included in the writer-count guarantee.

## Journal compatibility and P8-06B requirements

The journal schema, native file security, ownership checks, hashes, durability and
rollback verification remain unchanged. No live database replacement or replay is
wired. P8-06B must enroll settings/workspace/history and all required owner lifetimes,
establish profile-wide authority against legacy/independent writers, drain accepted
provider/tool work without losing evidence, close/reopen owners and invalidate caches,
then connect a trusted orchestrator to verified journal preimages and authoritative
post-restore readers. It must define an exclusive maintenance capability lifetime,
startup recovery and failed-resume handling before enabling application preparation.
Do not switch the production scope to LeafStores to bypass these requirements.

## Validation

Focused tests use real SQLite owners and throwaway profile paths. No real daemon endpoint, credentials or user model inventory
is required by the new tests.

## Changed-file inventory

- `core/CMakeLists.txt`
- `core/include/sentinel/core/app/ApplicationController.h`
- `core/include/sentinel/core/app/ApplicationControllerBuilder.h`
- `core/include/sentinel/core/app/StorageAdmission.h`
- `core/include/sentinel/core/chat/SQLiteConversationStore.h`
- `core/include/sentinel/core/memory/MemoryArchiveJobs.h`
- `core/include/sentinel/core/memory/SQLiteMemoryStore.h`
- `core/src/app/ApplicationController.cpp`
- `core/src/app/ApplicationControllerBuilder.cpp`
- `core/src/app/ApplicationMemoryManagement.cpp`
- `core/src/app/StorageAdmission.cpp`
- `core/src/chat/SQLiteConversationStore.cpp`
- `core/src/memory/MemoryArchiveJobs.cpp`
- `core/src/memory/SQLiteMemoryArchive.cpp`
- `core/src/memory/SQLiteMemoryManagement.cpp`
- `core/src/memory/SQLiteMemoryRetrieval.cpp`
- `core/src/memory/SQLiteMemoryStore.cpp`
- `docs/audits/P8_06A_STORAGE_ADMISSION_QUIESCENCE_RESULT.md`
- `docs/development/ARCHITECTURE.md`
- `tests/CMakeLists.txt`
- `tests/core/test_storage_admission.cpp`

### Focused evidence and explicit omissions

`build/tests/p806a-focused-admission.log`: 8 Qt rows passed, zero failures/skips.
Tests cover simultaneous leases, drain with new-writer exclusion, bounded self-held
writer refusal, cancellation and resume without invalidation, maintenance invalidation,
shutdown wakeup and lease survival, real memory CRUD/bulk/index/archive refusal,
worker connection/schema refusal, stale worker generation, and real conversation
placeholder/completion/title/metadata fencing. Callback tests invoke the authoritative
store boundary through captured lambdas; they do not simulate full accepted provider
request lifetimes. Live scope Unsupported is tested before state mutation.

Settings/workspace mutation fencing, Agent lifecycle drain, full asynchronous title/
summary orchestration, profile-wide exclusion, and real daemon maintenance/resume
are **not implemented or certified**. Existing daemon/chat/Agent regressions test
preserved behavior, not support for a new maintenance flow. These limitations are why
live preparation remains Unsupported. The P8-06A application-wide objective is Partial.

### Final validation receipts

| Check | Result / evidence |
| --- | --- |
| CMake tests preset configure | Passed; p806a-configure.log |
| Full C++ build, existing build/tests | Passed; p806a-build-verified.log |
| Focused admission test | 8/8 Qt rows, zero skipped; p806a-focused-admission.log |
| Sequential full CTest | 168/174 passed, six failed; p806a-ctest-delivery.log and p806a-ctest-lasttest.log |
| Memory/scoped/management/retrieval/archive/conversation and P8-06 journal | All passed within full run |
| Chat, Agent, daemon, Desktop regressions | Included in full run; six failing executables listed in validation gate remain failures |
| Rust fmt / Clippy with warnings denied | Passed offline in existing cli/target; p806a-rust-fmt.log, p806a-rust-clippy.log |
| Rust workspace | 142 tests passed, zero failed; p806a-rust-tests.log |
| Both canonical IPC generators --check | Passed; no IPC contract change |
| QA registry tests | 31 passed; p806a-registry.log |
| QA validation | Zero errors; existing missing external component evidence roasd.packaging warning; p806a-qa-final.json |
| Pinned clang-format 18.1.8 | New/small files and changed lines in established large units clean; p806a-format-final.log, p806a-format-established.log |
| Scoped clang-tidy | New coordinator zero reported project diagnostics; six storage units zero diagnostics on changed lines. Existing diagnostics outside changed lines retained; p806a-admission-tidy-final.log, p806a-storage-tidy-changed.log |
| Privacy / credentials | Zero flagged files; p806a-privacy-final.log and p806a-credentials.json (21 changed files) |
| git diff --check | Passed |

Full sequential CTest took 207.52 sec. Preserved internal Qt classifications: 11 SKIP, 2 XFAIL; 69 QWARN lines. These counts use LastTest.log once, excluding the repeated on-failure console replay.
No UI files changed; this phase adds no new user-facing maintenance flow. No real Keychain access or sandbox-policy weakening was used to obtain validation.
