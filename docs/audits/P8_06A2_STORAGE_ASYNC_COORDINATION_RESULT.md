# P8-06A.2 — Storage owners and asynchronous coordination

Date: 2026-10-21 (Europe/Istanbul).

Status: **Partial production coverage. Application-wide quiescence remains Unsupported;
combined backup/restore remains Blocked.** No recovery journal is attached to a live
replacement operation, no permission grants are imported, and no Agent actions are replayed.

## Verified base and execution constraints

Used only the supplied existing checkout. Origin is
`https://github.com/sentinel-projects/sentinel.git`; `git fetch origin` succeeded.
Before branch creation the complete working tree was clean and
`main == origin/main == 9be18213fd62022870348fc66f93d3a14bde0082`.
Integrated ancestors: P8-06 `83db1a47`, P8-06A `6ed10084`, P8-06A.1 `9be18213`.
Read AGENTS.md, architecture, Agent Runtime, security, building/testing and prior
implementation reports. Personal-Brain was unavailable on PATH; used the explicitly
supplied and Git-verified checkout. Preflight initially stopped because OpenCode PID
23316 had this checkout as cwd. The maintainer's subsequent “tamam devam et” was
accepted as confirmation that the competing session would not modify this checkout.
No competing Codex chat was active here in the app inventory. No subagents were used.
Created the previously absent requested branch
`feature/p8-06a2-storage-async-coordination` from that verified main.

Reused `build/tests` and `cli/target`. No clone/worktree/new build tree, commit,
push, merge, rebase, reset, stash, clean, PR, or workflow dispatch. Disposable
profiles, socket paths and analysis/formatter environments are under `/tmp`.
Tests use an outer macOS sandbox denying Keychain paths and securityd access;
no real credentials were used to obtain passing tests.

## Storage ownership and coverage matrix

Paths below are symbolic: P is the selected StandardPathProvider profile; A is
Qt AppDataLocation. Agent/grant paths are independently A, not necessarily P.

| Production owner | Mutation and initialization paths | Admission / remaining limits |
| --- | --- | --- |
| SQLiteChatHistoryStore, P/chat_history.sqlite3, schema 3 | Open/file creation, PRAGMAs, schema/FTS triggers/rebuild, startup sending/streaming interruption, append/upsert status, clear, prune | Shared coordinator and immutable generation; lease before open and each authoritative write. Read methods do not acquire write authority. Separate from ConversationStore. |
| SQLiteAgentRunStore, A/agent_runs.sqlite3, schema 7 | Schema/column migration, interruption reconciliation, record transactions for runs/planning/context/steps/tools/approvals/authorizations/evidence/claims/terminal state, clear, pruning | Shared coordinator and immutable generation. Read queries open with QSQLITE_OPEN_READONLY, so a missing database cannot be silently recreated. Record ownership checks prevent existing run/session, step/run or tool/step redirection. Automatic retention excludes unfinished runs. clearHistory refuses while unfinished evidence exists; events for a missing owning run return failure instead of claiming a zero-row update was recorded. |
| SQLitePermissionGrantStore, A/permission_grants.sqlite3 | Schema/plugin-owner migration, batched grant insertion, remove/revoke, clear | Shared coordinator, immutable generation, admission before filesystem/SQL changes. Load opens with QSQLITE_OPEN_READONLY; a missing file cannot be recreated by a read. No independent expiry writer exists here. |
| PermissionService / plugin-owner grants | Persistent cache publication after successful save; revoke/clear | Failed create does not publish additions. Failed revoke/clear permanently restricts this service to Deny, including session grants, until service reconstruction. Durable rows remain inspectable; failed revoke is returned as false. |
| AgentRuntime / AgentLoop / AgentExecutionScheduler | Async and synchronous runs, delegated loops, approvals, step/tool/terminal callbacks | Production shared authority is supplied at construction. Root run lifetime retains a lease through evidence completion or approval wait; resumed runs retain original authority. Unsupported custom history/run/grant owners refuse launch. Persistence failure stops further tool dispatch and prevents successful completion reporting. |
| ChatModeService / SQLiteConversationStore | User and assistant placeholder, streaming deltas, completion, cancellation, fallback title | Existing conversation store fencing retained. Accepted worker retains admission through queued completion, even when cancelled. Original generation and conversation/message IDs fence callbacks. Failed delta persistence stops further message publication; failed terminal persistence produces Failed in memory. Custom conversation owners fail closed when shared authority is required. |
| DaemonIpcServer automatic title worker | Provider request and queued expected-title update | Captures original authority, generation, store identity and lease through callback. Cancellation/shutdown/stale authority refuses update. Generated-title cache advances only on successful store update. |
| ApplicationController local inference / manual summary | Accepted local provider worker, streaming completion and summary metadata | Captured generation and retained callback lease. Request/conversation ownership checked before writes. Refused persistence is reported as failure; summary metadata save result is checked. |
| BackgroundJobService | Optional jobs table, initial queue/start/retry/cancel/result persistence | Captures immutable database generation; queued/active jobs retain leases. Worker owns a private Job snapshot; completion and SQL execute on QObject/connection owner thread. No production composition site currently instantiates this service. User JobFunction may perform independent side effects and is not treated as an enrolled profile writer. |
| SQLiteMemoryStore / MemoryArchiveJobs | Existing schema, source/FTS, managed mutations/import, snapshot workers and staging | P8-06A fencing preserved. Archive workers inherit original store generation; staging/transfer ownership and cancellation remain separate. |
| AppSettings / WorkspaceService | Settings migration/publication, catalog/profile/selection, basic import | P8-06A.1 admission and atomic applyChanges retained. No new settings coordinator. |
| BackupRecoveryJournal | Private internal metadata, preimages, receipts | Unchanged; separate journal owner lock. Does not become a backup domain or live replacement protocol. |

Direct Chat History initialization and write entry points are now fenced; callers
still use the existing void append/clear and lastError contract. Local inference
history refusal changes its completion status; Agent request history refusal prevents
starting execution. Agent transcript projection after a completed run is distinct
from the authoritative run audit store. Cross-store transcript changes are not an
atomic transaction; partial projection failure is not repaired by replaying actions.

## Agent evidence and failure semantics

RunStarted must persist before the async planner starts. ToolExecutionStarted must
persist before the gateway dispatches either an individual call or a scheduled batch
member. A failed record latches a runtime audit failure. The loop's persistence gate
refuses new side effects; it does not cancel an already executing tool solely because
maintenance was attempted. Completion/observation evidence is attempted once. If that
write fails after a side effect, runtime state and terminal delivery report Failed,
clear the final answer, and explicitly warn that side effects may exist. Existing
in-memory observations remain inspectable; absent durable evidence is never invented.
The run's complete evidence projection is persisted before terminal success delivery,
without publishing an extra Completed event. The trailing public state projection
does not become another writer after that snapshot has committed.
A failing terminal write is delivered as failure and corrected in in-memory event history.
A later independent run may be accepted only if its own required persistence succeeds.

Grant admission is storage authority only. ToolExecutionGateway, authorization,
approval and sandbox policies still decide tool authority. Failed grant creation
never increases permissions. A rejected revoke cannot be made durable while admission
is closed: this process denies authorization and returns the failure; another process
or a restart can still see the unretracted durable grant. A future profile-wide
revocation protocol must coordinate those processes. Ordinary portable backup never
imports these grants.

## Lifetimes, locks, cancellation and shutdown

No polling thread or unbounded new queue is added. Move-only Writer leases and
shared ownership of accepted-operation leases use the existing condition-variable
barrier. SQLite operations finish/rollback before their write lease is released.
Agent store mutex serializes its error/read/write operations; it acquires the brief
admission mutex before opening a transaction. Admission never calls back into an
owner under its mutex. Runtime/job mutexes may protect lease maps, but maintenance
must not wait while holding them or blocking the Qt event loop needed by callbacks.

Long-lived leases make a leaf preparation time out while provider/planning/tool/title
or queued-job work still owns future writes. During Quiescing, nested new store
admission can refuse and must be reported as such; a lifetime lease does not bypass
store admission. Timeout/cancel returns Running without generation advancement.
Maintenance increments generation; immutable old stores remain stale after resume.
Shutdown closes admission and wakes preparation waiters; accepted callbacks retain
lease ownership until their persistence attempt ends. Chat cancellation cannot drop
a worker's lease prematurely. Background completion signals follow the actual SQL
result, never precede it, and run outside the job mutex so a direct subscriber can
submit a follow-up job. That reentrant completion path is covered by the production
SQLite job test. Existing active-tool cancellation policy is unchanged.

The live application's prepareLocal remains Unsupported before state mutation;
prepareApplication remains Unsupported in every scope. No maintenance IPC or model
authority was added. Holding a process-local lease is not proof of cross-process
exclusion, crash durability, recoverability, authorization, or model lifecycle state.
PD-D03, PD-D04 and PD-D10 and resource-admission policy remain unchanged.

## Independent processes and uncovered writers

| Process / owner | Remaining boundary |
| --- | --- |
| Production daemon | Composes shared standard stores; DaemonSessionProjectionStore still directly creates/migrates/updates ipc-sessions.sqlite3 outside StorageAdmission. Endpoint ownership is not a profile lock. |
| Legacy C++ CLI | Own in-process composition can access the same profile stores. ConfigCommand and independent settings readers/writers have no cross-process admission handshake. |
| Rust CLI/TUI | Chat/Agent/permission mutations go through daemon IPC. TUI mascot diagnostics/assets/terminal output are independent local files, not proof that profile ownership is exclusive. |
| Desktop | IPC client for runtime stores; independent QSettings window state, presentation preferences, autostart platform files, export destinations, DesktopBackupHelper staging/destination commits remain independent writers. |
| Plugin/process hosts and external tools | Plugin files, MCP/plugin subprocesses and tool filesystem/process side effects are outside the profile coordinator. Gateway authority is unchanged. |

Additional uncovered production owners found by write-site audit: AlarmStore JSON,
RecoveryService diagnostic JSON, LocalRagStore/index lifecycle, ModelStorageManager
settings, ModelOperationService operation journal/downloads/sidecars, OllamaRuntime
catalog snapshots, SkillService preferences, SnapshotService files, AutoUpdateService
artifacts, ToolOutputTruncator spill files, editor files, logs/captures, OS credentials,
and extension package installation/removal. Some are caches or explicit user outputs;
none can silently be counted as covered application storage. Initialization/migrations
of these independent owners are also not enrolled. Native secret migrations continue
to have their own security authority, distinct from ordinary settings publication.

P8-06A.3 needs a stable physical profile identity spanning P and A, one verifiable
lock owner for all independent mutation-capable processes, authenticated bounded
prepare/drain/abort/resume with generation and process-death handling, registration
of every additional owner above, and callbacks fenced through durable completion.
It must also define shutdown/approval-wait ownership and reconstruction after
maintenance. Combined backup/restore additionally needs verified SQLite snapshot
consistency, live owner close/reopen, journal validation/durability/rollback and
crash recovery. This milestone enables none of those by inference.

## Regression and validation receipts

| Check | Result / receipt under build/tests |
| --- | --- |
| CMake tests preset | Configure passed; p806a2-configure.log. Existing build/tests reused. |
| Full C++ build | Passed; p806a2-build-delivery-final.log (complete default C++ target build). |
| Focused regression set | 18/18 executables passed, 27.46 s; p806a2-focused-final.log (earlier 16/16 set: p806a2-focused.log). Covers new owners, runtime, loop, Chat, memory archives, settings/workspace and prior journal/admission suites. |
| Daemon/Desktop background title | Generated title, manual rename wins, and late shutdown refusal passed (3 data rows; 5 Qt totals including setup/cleanup); p806a2-title.log. |
| Sequential full CTest | 170/176 passed; 6 existing sandbox/process failures; 224.59 s; p806a2-ctest-delivery.log. Full Qt detail preserved in p806a2-ctest-delivery-detail.log. |
| Rust fmt / Clippy | Passed offline, all workspace/targets, warnings denied; p806a2-rust-fmt.log / p806a2-rust-clippy.log. |
| Rust workspace tests | 142 passed with fresh disposable TMPDIR; p806a2-rust-tests-isolated.log. First restricted run failed 9 socket fixtures with EPERM; p806a2-rust-tests.log retained. |
| IPC generators | Both --check passed; p806a2-ipc-final.log. No schema/type contract changes. |
| QA tests / schema registry | 54 Python verification tests passed; registry valid, existing roasd.packaging external-evidence warning; p806a2-qa-tests.log / p806a2-qa-registry.log. Registry validity is not release certification. |
| CI clang-format 18.1.8 | Changed tracked C++ lines clean; new test whole-file clean; p806a2-format-final.log / p806a2-format-new.log. Formatter installed only in disposable /tmp environment. |
| Scoped clang-tidy | 12 production translation units analyzed using analysis-only compile database. Default store/config pass retained in p806a2-tidy-stores.log; analyzer/bugprone runtime pass in p806a2-tidy-runtime.log. Job callback null-pointer diagnostics corrected with QPointer and checked queued ownership; stores rechecked in p806a2-tidy-stores-verified.log. Read-only connection changes and the final runtime snapshot projection were additionally checked in p806a2-tidy-readonly.log and p806a2-tidy-runtime-final.log. Final evidence and background callback changes were rechecked in p806a2-tidy-evidence-final.log / p806a2-tidy-background-final.log. No compiler errors; style diagnostics and pre-existing Qt narrowing/static-initialization, optional-access and destructor diagnostics remain visible. Default style-check cleanliness is not claimed. Whole-repository tidy cleanliness is not claimed. |
| Privacy / credential signatures | Zero personal path flags and zero private-key/provider-token signature flags; p806a2-privacy-final.log / p806a2-credentials.json. Signature scan reports locations only, never secret values. |
| git diff --check | Passed. |

The first focused pass caught a new terminal event ordering regression; fixed by
retaining the public terminal-then-state delivery contract after evidence preflight.
That failed receipt remains p806a2-focused-initial.log. The first full run
(p806a2-ctest-full.log, 168/176 passed, 212.99 s) additionally exposed duplicate
accepted-answer projection; corrected by persisting the snapshot without emitting
a duplicate Completed event. That run also encountered an overlong disposable Unix
socket path in desktop-daemon E2E; the identical test passed with the same security
sandbox and a shorter disposable profile (p806a2-short-path-e2e.log). The final
sequential run uses that shorter path and includes read-only connection hardening. Initial sandboxed ccache writes
were refused; final builds use CCACHE_DISABLE=1 without changing production compiler
or PCH policy. Intermediate build receipts are not substituted for final validation.

New deterministic tests verify authoritative history rows and status/pruning/clear
refusal; constructor refusal without file creation; old-generation store rejection;
Agent run/tool/approval evidence refusal, active-run clear refusal, missing-run
record refusal and cross-session ownership rejection;
read-only Agent/grant connections that cannot recreate missing database files;
permission create/revoke durable/cache behavior; background completion on owner
thread and refused post-shutdown persistence; run creation refusal before planning;
Agent audit failure before dispatch and after one completed fixture side effect;
Chat provider lifetime drain refusal and late success refusal; and real daemon title
completion/rename/shutdown races. Existing tests exercise archive transfer cancellation,
maintenance timeout/cancel/resume, concurrent leases, shutdown wait interruption,
summary failure/continuity, Memory/Conversation/Settings compatibility and explicit
Unsupported application preparation.

A generation transition while a retained provider/title/job lease is alive is
intentionally impossible: the test observes bounded preparation refusal instead.
Old-generation mutation rejection is separately tested against the real stores after
maintenance/resume. No live accepted manual-summary success-to-stale-worker fixture
was added; that path is guarded in production but its new generation check lacks a
dedicated positive late-worker test. This is a validation gap, not a passing claim.

### Existing sandbox/process failures

| Executable | Preserved failure classification |
| --- | --- |
| test_mcp_integration | Nested sandbox process cannot start (sandbox_apply refusal). |
| test_plugin_tool_integration | Existing plugin host start failures; enforced-launch diagnostic maps to PluginCrashed rather than the expected PluginSandboxUnavailable. |
| test_real_tool_executor_tools | Process/fixture execution denied by nested sandbox, including exit 71. |
| test_process_executor | Nested macOS sandbox launch denied; process containment fixtures cannot run. |
| test_plugin_manager | Existing plugin launch diagnostic classification mismatch. |
| test_desktop_ipc | Existing real MCP host/process scenario cannot complete under nested sandbox; new title rows pass. |

No policy or security assertion was weakened, no real Keychain access enabled, and
none of these failures was converted to SKIP/XFAIL/passing assertions. The preceding
complete run was 170/176 passed in 217.63 s (p806a2-ctest-final.log); after the final
active-evidence/missing-run guards, the owners-only production test passed all 7 Qt
rows in 142 ms (p806a2-owners-final.log). The final all-target relink (p806a2-build-delivery-final.log) and sequential
full-suite receipt (p806a2-ctest-delivery.log, 170/176 passed, 224.59 s) supersede
those intermediate receipts.

Preserved final Qt diagnostic rows: SKIP 11, XFAIL 2, QWARN 74, FAIL! 25. These rows are distinct from the 176 CTest executable results.

## Remaining Partial / Blocked work

Profile-wide coordination and combined backup/restore are intentionally unavailable.
Independent writers above are explicit gaps. No native Fedora/Windows acceptance
or live-model acceptance was performed. Retained Chat History/Conversation SQL
connections remain owner-thread-only; existing foreign-thread context retrieval
consumers are not newly certified by this writer-admission change. BackgroundJobService admission covers its
metadata, not arbitrary persistence inside supplied job functions. Destruction joins
workers but does not pump queued completions; a pending result can remain a durable
Running row for startup interruption recovery. removeJob still removes only runtime
metadata, as before; durable retention/removal is not implemented by that service. Optional unbound
standalone stores retain their compatibility behavior; only explicitly enrolled
stores participate, and unsupported custom implementations cannot impersonate the
shared standard runtime's supported owners. There is no guarantee that a failed
revoke has removed a grant from another process or from durable storage.

## Exact changed-file inventory

- `apps/sentinel-daemon/service/DaemonIpcServer.cpp`
- `core/include/sentinel/core/agent/AgentExecutionScheduler.h`
- `core/include/sentinel/core/agent/AgentLoop.h`
- `core/include/sentinel/core/agent/AgentRuntime.h`
- `core/include/sentinel/core/agent/IAgentRunStore.h`
- `core/include/sentinel/core/agent/SQLiteAgentRunStore.h`
- `core/include/sentinel/core/app/ApplicationController.h`
- `core/include/sentinel/core/app/StorageAdmission.h`
- `core/include/sentinel/core/chat/ChatModeService.h`
- `core/include/sentinel/core/chat/IChatHistoryStore.h`
- `core/include/sentinel/core/chat/IConversationStore.h`
- `core/include/sentinel/core/chat/SQLiteChatHistoryStore.h`
- `core/include/sentinel/core/chat/SQLiteConversationStore.h`
- `core/include/sentinel/core/job/BackgroundJobService.h`
- `core/include/sentinel/core/security/IPermissionGrantStore.h`
- `core/include/sentinel/core/security/PermissionService.h`
- `core/include/sentinel/core/security/SQLitePermissionGrantStore.h`
- `core/src/agent/AgentExecutionScheduler.cpp`
- `core/src/agent/AgentLoop.cpp`
- `core/src/agent/AgentRuntime.cpp`
- `core/src/agent/SQLiteAgentRunStore.cpp`
- `core/src/app/ApplicationController.cpp`
- `core/src/app/ApplicationControllerBuilder.cpp`
- `core/src/chat/ChatModeService.cpp`
- `core/src/chat/SQLiteChatHistoryStore.cpp`
- `core/src/job/BackgroundJobService.cpp`
- `core/src/security/PermissionService.cpp`
- `core/src/security/SQLitePermissionGrantStore.cpp`
- `docs/audits/P8_06A2_STORAGE_ASYNC_COORDINATION_RESULT.md`
- `docs/development/ARCHITECTURE.md`
- `tests/CMakeLists.txt`
- `tests/core/test_agent_runtime.cpp`
- `tests/core/test_chat_mode_service.cpp`
- `tests/core/test_storage_async_coordination.cpp`
- `tests/desktop/test_desktop_ipc.cpp`
