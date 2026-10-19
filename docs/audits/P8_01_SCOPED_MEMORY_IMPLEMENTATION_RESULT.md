# P8-01 — scoped memory and provenance implementation

Status: production scoped-memory foundation implemented in storage **and actual Chat/Agent
retrieval paths**. This is not complete memory-management UI, a universal context-authorization
system, semantic activation, or production live-provider acceptance. The original implementation evidence below was collected
before commit; see the integration addendum for subsequent delivery validation.

## Verified workspace

Existing supplied checkout only; origin `https://github.com/sentinel-projects/sentinel.git`.
Fetched origin and verified clean main = origin/main = HEAD at
`876d61063472357e266623a23ccad67034d423b2` (P4-B01 merge). Earlier P2-M08, QA-02B and P6-06
milestones are integrated ancestors. AGENTS, architecture, Agent Runtime, security, building and
testing instructions read, plus the integrated P4-B01 findings. Personal-Brain `brain` was
unavailable on PATH. Repository identity was resolved directly; no alternate checkout guessed.
Visible task inventory showed only this coding chat active. Created absent
`feature/p8-01-scoped-memory` from main. HEAD/branch remained stable.

Reused `build/tests` and `cli/target`. Temporary analysis-only compile database is not a build
tree. No clone/worktree/new build, reset/stash/clean/rebase/commit/push/merge/PR/dispatch. All
migration tests used disposable databases. No actual user memory/conversation database was opened,
rewritten, exported or used as evidence. No real daemon startup against the shared Keychain,
provider credentials, live inference, model downloads or paid calls were needed.

## Exact changed files (28)

- `core/CMakeLists.txt`
- `core/include/sentinel/core/agent/ContextEngine.h`
- `core/include/sentinel/core/app/ApplicationController.h`
- `core/include/sentinel/core/chat/ChatModeService.h`
- `core/include/sentinel/core/interfaces/IMemoryStore.h`
- `core/include/sentinel/core/memory/InMemoryStore.h`
- `core/include/sentinel/core/memory/MemoryCandidate.h`
- `core/include/sentinel/core/memory/SQLiteMemoryStore.h`
- `core/include/sentinel/core/memory/ScopedMemory.h`
- `core/include/sentinel/core/runtime/RealToolExecutor.h`
- `core/src/agent/AgentLoop.cpp`
- `core/src/agent/AgentRuntime.cpp`
- `core/src/agent/ContextEngine.cpp`
- `core/src/app/ApplicationController.cpp`
- `core/src/app/BackupService.cpp`
- `core/src/chat/ChatModeService.cpp`
- `core/src/memory/InMemoryStore.cpp`
- `core/src/memory/SQLiteMemoryStore.cpp`
- `core/src/memory/ScopedMemory.cpp`
- `core/src/runtime/RealToolExecutor.cpp`
- `docs/audits/P8_01_SCOPED_MEMORY_IMPLEMENTATION_RESULT.md`
- `docs/development/ARCHITECTURE.md`
- `tests/CMakeLists.txt`
- `tests/core/test_application_controller.cpp`
- `tests/core/test_chat_mode_service.cpp`
- `tests/core/test_real_tool_executor_tools.cpp`
- `tests/core/test_scoped_memory.cpp`
- `tests/core/test_sqlite_memory_store.cpp`

## Existing owners and production changes

| Owner/path | Existing behavior | P8-01 behavior |
|---|---|---|
| IMemoryStore | Global legacy put/get/entries/FTS search; numeric legacy IDs | Adds typed scoped write/retrieval, explicit status and scoped-backup guard; unsupported custom adapters fail closed |
| SQLiteMemoryStore | Qt SQL database, metadata version3, stable legacy IDs, optional FTS5 | Same database, transactional v4 typed table/index; old table remains Legacy, not copied/reclassified |
| InMemoryStore | Legacy key/value test backend | Same scoped semantics, UUIDs/metadata, read/write locking of scoped state |
| ApplicationController | Global memory entries for recall/planning; remember/review commits used legacy put | Trusted active conversation/workspace resolver; new manual writes are Session; planning/recall see only bounded visible scoped records |
| ChatModeService | Conversation-bound transcript/profile/summary prompt | Adds real scoped retrieval on the main thread before immutable provider request construction; provenance-labelled untrusted JSON |
| AgentLoop/ContextEngine | Automatic legacy memory search | Actual loop state session and configured workspace feed filtered scoped retrieval, stable item IDs and untrusted provenance |
| AgentRuntime/RealToolExecutor | Global memory-search snapshot | Runtime resolves each gateway-owned invocation session against its own configured options, not mutable active selection; model arguments cannot grant scope |
| Memory candidates | Manual approval and separate commit, ephemeral candidate store | Capture UUID and session/workspace binding; cross-context commit refused; unverified excerpts remain Unknown provenance |
| BackupService | Legacy JSON key/value export/import/rollback | Refuses the memory domain when scoped data exists, before lossy export or clearing/rollback |
| Conversation/Agent-run stores | Separate SQLite owners, conversation/message/run identities | Remain separate; not migrated into memory or merged with permissions |
| SemanticRetrieval/CompactionService | Existing semantic activation gates and labelled conversation summaries | Gates remain; no background embeddings, model calls, auto-summary promotion or new memory facts |

QML, IPC schemas/generators, provider registration, authentication, network policy, ModelBinding,
ToolExecutionGateway enforcement, final-answer acceptance and resource/process ownership are unchanged.
No scoped authority is exposed as a CLI flag, environment setting or raw model identifier.

## Typed authority, persistence and migration

`MemoryScope` explicitly encodes Legacy0, Session1, Project2 and Personal3. `MemoryProvenance`
distinguishes Unknown, UserProvided, ConversationContent, ProjectDerived, RetrievalResult,
GeneratedSummary and Inferred. SQL encodings are explicit; source identity never derives authority
from relevance, paths, prose or timestamps.

`MemoryAccessContext` is an internal trusted-caller contract: actual session ID, selected workspace
ID, the authorized workspace set and an explicit personal inclusion flag. The store rejects a
missing session or requested workspace absent from that set. Session data matches **both** owner
session and workspace; Project matches the requested authorized workspace; Personal uses the current
profile database as owner and requires explicit inclusion. Legacy is never visible through this
contract. A set containing several authorized workspaces still retrieves only the selected one.

ApplicationController obtains conversation identity from its real active ConversationStore record,
and workspace identity from selected existing, non-archived WorkspaceService metadata. An unknown
requested ID cannot become authorized by fallback, and no raw filesystem path supplies project
identity. With no workspace configured, a valid session can own empty-workspace Session records;
Project writes are refused. Production composition never sets includePersonal, even when a
workspace's display kind/name is Personal or its old metadata memoryScope string says personal.
Typed internal callers may explicitly opt in; a public personal sharing UI/policy is future work.

Agent identity comes from AgentLoopState/configured session options. Tool memory-search uses the
runtime session field sealed by the existing gateway and a session-specific resolver, preventing a
delegated child from reading its parent's Session records through the shared executor. Unregistered
child sessions fail closed instead of borrowing parent authority; tool/model-provided session_id/workspace_id arguments are rejected by the
existing schema and cannot change that authority. No new permission or filesystem grant is made.

The new `scoped_memory_entries` table lives in **the existing memory database**. It stores stable
UUID identity, scope, owner, workspace, key/value, provenance, source reference and created/updated
UTC timestamps. Composite uniqueness `(scope, owner_id, workspace_id, key)` permits the same key
in different scopes. Updates preserve UUID/creation time. Explicit no-overwrite commits use an
atomic SQL conflict condition, not a bounded pre-read; a conflict rolls back without changing data.
Scope/owner CHECK constraints and an owner/workspace/time index support filtering before selection.

Creating the typed table/index and advancing metadata to4 is transactional. Legacy identity and
FTS migrations retain their existing separate transactions/fallback behavior; a failed typed
migration leaves its prior schema version and legacy bytes intact and makes the store unavailable.
Future versions are refused. Existing numeric IDs/key/value bytes are preserved, with Legacy and
Unknown classification on the legacy record contract. No ownership, provenance or creation date
is fabricated for old rows. FTS5 remains a legacy compatibility index; scoped retrieval has no
unrestricted legacy fallback and works without embeddings or a new database framework.

Older binaries that reject v4 must not be used to rewrite/downgrade it. Keep normal backups of a
quiescent profile according to existing storage procedures before deployment; no live user backup
was taken here. Explicit later reclassification requires review, an authorized typed write and a
trustworthy/Unknown provenance decision. Old values are not silently promoted or removed.

## Retrieval and provenance

SQLite predicates filter allowed scopes/owners/workspace in SQL **before** escaped literal matching,
ordering and LIMIT. There is no broad cross-workspace candidate fetch followed only by model ranking.
Matching uses at most6 distinct lexical terms (64 characters each) with literal key/value
containment, not semantic similarity; SQLite's LIKE
case behavior differs from full Unicode Qt matching. Results are ordered by update time with stable
UUID tie-break, capped50 (Chat requests8). Invalid limits/oversized queries are explicit InvalidInput;
unauthorized and unavailable are distinct from Ready-with-no-matches. Unsupported adapters return
Unavailable and never fall back to global entries.

Chat independently checks that the context provider's session matches the real conversation ID.
Provider prompt memory data carries UUID, scope, owner/workspace, provenance and source reference.
JSON escaping, max8 rows, max400 value characters per item and4096 UTF-8 bytes bound this block.
It is labelled untrusted historical data; generated/inferred content is not a direct user statement.
Agent planning and the actual tool result retain the same typed metadata/label rather than flattening
it into an authoritative fact. Storage retains full accepted values; prompt truncation does not edit
records. The scoped query caps candidate selection before serialization.

`remember` is an explicit user action recorded as UserProvided with a session reference. Manually
captured excerpts do not have a verified persisted message/role reference: even after review they
are **Unknown**, with a capture UUID, not direct user statements or verified conversation facts.
Capture identity and originating session/workspace are assigned before review; committing in another
context is refused. Merely approving a candidate never writes memory. No provider reply, generated
summary, compaction or inferred assertion is automatically promoted into durable memory facts.
Other trusted producers can use the explicit provenance types when they have suitable source evidence.

SQLite scoped reads from Agent workers open a thread-owned read-only connection to the same file;
they do not operate the UI thread's QSqlDatabase/driver. Write/maintenance stay on the owning thread.
An in-memory SQLite database cannot be opened by a separate worker connection and returns Unavailable
there. InMemoryStore scoped reads/writes are lock-protected. Model/network work is not introduced.

## Compatibility and deliberate limits

- Legacy raw key/value APIs remain available for compatibility/explicit management. In-repository
  automatic memory prompt entry points use the scoped contract. Legacy records are excluded from
  automatic recall/injection rather than assigned guessed projects. Existing UI does not yet offer
  complete legacy reclassification or scoped CRUD/provenance browsing.
- New remember/review commits are Session scope. Project/Personal persistence exists in the typed
  production store API; no unrelated public scope selector/sharing/export UI is added.
- Chat clear creates a new conversation: previous Session records persist but are no longer recalled
  into that new session. Tests now prove both preservation and exclusion. Existing explicit global
  clearMemory continues to delete legacy and scoped memory transactionally; Chat clear does not.
- Legacy opt-in local-inference salience blocks cannot preserve typed provenance through truncation.
  Their committed-memory blocks are explicitly omitted before provider request construction, with
  a diagnostic reason; other context sources remain. The actual ChatModeService and Agent paths
  consume the new contract. A generated-memory regression verifies the legacy path never flattens
  it into unlabeled prompt content. Provenance-preserving legacy budgeting belongs in P8-02.
- Controller's old key/value display/planning adapter is bounded50 and sorted by key; it is not a
  total inventory/export API. Rich provenance is retained by actual Chat/Agent context contracts;
  a complete management surface is reserved for later P8 work.
- Legacy JSON memory backup cannot represent scopes, stable UUIDs or provenance. Both memory-domain
  export and import are refused if scoped records exist, including merge/rollback paths. This avoids
  an apparently successful incomplete backup or destructive restore. Other backup domains and
  legacy-only backups retain their existing behavior. A scope-preserving format is future work.
- Ephemeral memory candidates remain a separate review store; capture references identify the event,
  not a promise that its full source remains resolvable after restart. Unknown provenance is explicit.
- Global legacy ChatHistoryStore/history-search, conversation export and existing conversation/run
  authorization are separate contracts, not reimplemented as scoped memory here. This milestone
  does not certify every historical context source as project-isolated or add multi-user RBAC.
- Personal authority is profile-local and explicit, with production inclusion disabled. Per-user/profile
  database separation and existing OS/file permissions remain; SQLite contents are **not encrypted**.
- No resource admission, leases, process ownership, automatic unload, new embedding generation or
  budget threshold. PD-D10 and resource-policy decisions remain pending.

## Focused regression evidence

Disposable SQLite and InMemory cases cover same-session retrieval, other-session exclusion,
same-project sharing, other-project exclusion (including same conversation ID in another workspace),
unauthorized workspace denial, personal opt-in/write/read rules, Legacy/Unknown exclusion, bounded
selection and explicit global clear. Conflict tests verify no-overwrite preserves existing value.

Restart/migration tests seed a v3 database with numeric ID42 and exact legacy bytes, migrate to4,
verify old ID/value intact and invisible to scoped retrieval, reopen and compare scoped UUID,
created time, provenance/source, update while preserving identity, and read from a real Qt worker
through the new read-only connection. Future-schema and injected malformed-table migration failures
preserve legacy bytes/ID/schema metadata and return unavailable. Legacy backup cannot discard scoped
records or claim a scope-preserving export.

The real production ChatModeService test sends a natural multi-word request through a scripted IChatProvider which echoes the
actual delivered prompt: own Session and Project data appear with GeneratedSummary/source metadata;
foreign Session/Project, Personal without opt-in and Legacy values do not. ContextEngine independently
checks actual AgentContextItems/UUID/untrusted provenance. Controller tests switch real conversations/workspaces and invalidate cached recall queries/results, including equal-count scopes;
refuse cross-session candidate commit, retain old records after Chat clear and classify unverified
captures Unknown. RealToolExecutor tests verify schema rejection of forged IDs, permission denial
without authority and no parent Session leak through a delegated runtime session ID, and no rerouting of an old request when active workspace selection changes. Existing
QA-02B and P6-06 production-path fixtures remain in the validation scope.

## Validation results

Validation platform: macOS27.0.1, Darwin arm64. Validation uses the existing tests preset, disposable HOME/XDG directories and offscreen Qt.
Socket-dependent suites run with local IPC permitted. No live-provider opt-in is enabled.

| Check | Result |
|---|---|
| `cmake --preset tests` | Passed; existing build reused |
| `CCACHE_DISABLE=1 cmake --build --preset tests` | Passed; final resolver build, zero compiler warnings/errors |
| Final focused Chat/scoped store/controller/AgentRuntime/executor/native integration | 6/6 passed, 9.75 seconds |
| Final sequential full CTest | 168/168 passed, 239.37 sec |
| Rust `cargo fmt --all -- --check` | Passed |
| Rust workspace all-target Clippy, locked/offline, `-D warnings` | Passed |
| Rust workspace tests, locked/offline | 142 passed, 0 failed/ignored; permitted rerun |
| Both IPC generators `--check` | Passed; no wire/type changes |
| QA registry/schema validation | Passed; candidate dirty=true, exact base SHA above; historical statuses unchanged |
| Pinned clang-format 18.1.8 | New files fully checked; modified C++ hunks produce empty format diff |
| Scoped clang-tidy | Zero reported project diagnostics in new scoped contract and modified-line storage/Chat/ContextEngine scope; 3 non-blocking AgentRuntime brace-style warnings |
| Personal-path privacy scan | 0 files flagged |
| Credential-signature scan | 28 changed files, 0 flagged; no credential contents emitted |
| `git diff --check` | Passed |

Static analysis covered ScopedMemory.cpp/header in full, and changed lines of SQLiteMemoryStore.cpp,
InMemoryStore.cpp, ChatModeService.cpp and ContextEngine.cpp. Raw audited logs report 433,447,438,504,
495 generated warnings respectively, all suppressed as non-user code, outside changed lines, or
existing NOLINT (one). These are not a whole-repository zero-warning claim. AgentRuntime.cpp and
RealToolExecutor.cpp additionally received changed-line analysis after the session resolver change:
AgentRuntime reports 3 `readability-braces-around-statements` warnings at the three short resolver
conditionals; RealToolExecutor reports none. These style warnings follow the surrounding short-if
style and are reported, not counted as errors. Their generated/suppressed totals are 642/640 and
751/752 respectively (tool counts include header and existing NOLINT diagnostics). No static-analysis
compiler errors occurred.

Earlier validation attempts are retained rather than hidden: initial focused assertions exposed
obsolete global-after-clear visibility and UI ordering expectations, forged tool IDs rejected earlier
by schema validation, and a missing local-model discovery fixture; these were corrected without
weakening authorization. One intermediate full run had test_native_integration killed while a build
overlapped test execution: 167/168 passed; that run is inconclusive and excluded from acceptance.
Independent sequential runs after completed builds passed 168/168 (243.39,240.80,240.96 seconds)
before the final resolver change. The first resolver rebuild stopped because ccache could not write
its external-volume temporary directory (EPERM); disabling cache completed the build. The repeated
Rust CLI tests initially hit local-socket EPERM under the restricted sandbox (6 passed,9 failed);
the permitted rerun is the authoritative receipt. No production code was changed to bypass these
execution restrictions.
Raw logs remain under existing ignored `build/tests/p801-*`; they contain disposable fixtures,
not production user data. Rust/IPC/registry validation changes no historical acceptance status.

### Final CTest internal skips and warnings

Final raw Qt output: **10 SKIP, 2 XFAIL, 81 QWARN**; these are internal rows, not failed
CTest executables. No new live/native acceptance is inferred. Full output is retained as
`build/tests/p801-ctest-final-resolver-detail.log`; summary is
`build/tests/p801-ctest-final-resolver.log`.


SKIP rows (existing platform/contract limitations):

- `CredentialStoreTest::missingLinuxHelperRefusesPersistence() Linux Secret Service helper contract.`
- `CredentialStoreTest::testPlatformCredentialBackend() Native credential write refused; no real smoke success claimed.`
- `AgentReliabilityHarnessTest::cor11_cancelAfterFirstStreamedDelta(native-tool-assembly) COR-11 native incremental tool-call assembly remains explicitly blocked: IChatProvider exposes no typed incremental tool-argument assembly event, so a partially streamed native call could only be dispatched after a validated terminal assembly. This fixture asserts the native request stays on the bounded non-streaming path (one sendRequest, zero streamed deltas, zero public deltas) and does not invent an assembly contract.`
- `AgentRuntimeTest::asyncRunCommandStreamsAndContinues() This shell fixture requires child creation; strict macOS plans deny forks. Native host lifecycle is covered by plugin integration tests.`
- `AgentRuntimeTest::asyncRunCommandCancellationAndTimeout() This shell fixture requires child creation; macOS strict no-detached-child plans now deny all forks. Plugin host cancellation is covered by real host integration tests.`
- `AgentRuntimeTest::asyncDockerUsesProcessExecutorAndPreservesRestrictions() The fake Docker shell fixture requires child creation; strict macOS plans deny forks.`
- `AgentRuntimeTest::shutdownStopsActiveCommand() This shell fixture requires child creation; strict macOS plans deny forks. Native host shutdown is covered by plugin integration tests.`
- `ObservationPolicyTest::optInLiveClassification() Explicit opt-in required for production local-provider probe`
- `RealToolExecutorToolsTest::browserToolsReportMissingNodeGracefully() npx is installed on this machine; the missing-node branch cannot run.`
- `LocalModelRuntimeTest::removalDuringActualUseRemainsBlocked() No model-residency lease/removal contract exists; a live provider handle is not proof of active model use. Source-only D6.`

XFAIL rows (existing platform/contract limitations):

- `LocalModelRuntimeTest::providerSwitchRetainsOwnedChild() P3-02 D4: switching provider returns before owned cleanup`
- `LocalModelRuntimeTest::endpointSwitchDoesNotReconcileOwnedChild() P3-02 D5b: same-model endpoint switch leaves old owned child`

QWARN message counts (including deliberate rejection-path diagnostics):

| Count | Message |
|---|---|
| 1 | `Duplicate MCP tool "mcp.test.alpha"` |
| 1 | `"McpService: Server 'certification' already exists"` |
| 13 | `QProcess: Destroyed while process ("/usr/bin/sandbox-exec") is still running.` |
| 1 | `qt.sql.qsqldatabase: QSqlDatabasePrivate::removeDatabase: connection 'session-patterns' is still in use, all queries will cease to work.` |
| 1 | `qt.sql.qsqldatabase: QSqlDatabasePrivate::removeDatabase: connection 'job-persistence' is still in use, all queries will cease to work.` |
| 1 | `QProcess: Destroyed while process ("/bin/sh") is still running.` |
| 1 | `This plugin does not support QPlatformServices::openUrl() for 'https://github.com/Sopwit/Sentinel/releases'.` |
| 1 | `IPC protocol error: daemon-shutting-down` |
| 2 | `IPC protocol error: runtime-busy` |
| 1 | `IPC protocol error: unknown-workspace` |
| 2 | `IPC protocol error: workspace-root-rejected` |
| 1 | `IPC protocol error: permission-denied` |
| 1 | `IPC protocol error: unknown-command` |
| 1 | `IPC protocol error: incompatible-major` |
| 1 | `IPC protocol error: handshake-required` |
| 1 | `Daemon socket already owned` |
| 1 | `IPC protocol error: malformed-json` |
| 1 | `IPC protocol error: invalid-payload` |
| 1 | `IPC protocol error: message-too-large` |
| 2 | `IPC protocol error: unknown-session` |
| 18 | `IPC protocol error: invalid-approval` |
| 1 | `QSoundEffect: Error decoding source qrc:/sounds/not-a-real-cue.wav` |
| 1 | `IPC protocol error: provider-unavailable` |
| 1 | `IPC protocol error: voice-unavailable` |
| 1 | `IPC protocol error: voice-busy` |
| 1 | `IPC protocol error: voice-not-owned` |
| 1 | `IPC protocol error: invalid-captured-audio` |
| 1 | `IPC protocol error: voice-configuration-changed` |
| 1 | `IPC protocol error: invalid-settings-action` |
| 1 | `IPC protocol error: invalid-settings-arguments` |
| 11 | `This plugin does not support raise()` |
| 8 | `QMetaObject::invokeMethod: No such method QWindow::focusPrompt()` |

No test failure was suppressed or reclassified as a pass.

## Reproduction and receipt boundaries

Use `cmake --preset tests`, then `CCACHE_DISABLE=1 cmake --build --preset tests`; wait for
build completion before `ctest --preset tests -j 1 --timeout 180 --output-on-failure`.
Use disposable HOME/XDG paths and `QT_QPA_PLATFORM=offscreen`; permit local fixture IPC sockets.
Focused selection is `test_chat_mode_service`, `test_scoped_memory`, `test_application_controller`,
`test_agent_runtime`, `test_real_tool_executor_tools`, `test_native_integration`.
Run `cargo fmt --manifest-path cli/Cargo.toml --all -- --check`,
`cargo clippy --manifest-path cli/Cargo.toml --workspace --all-targets --locked --offline -- -D warnings`
and `cargo test --manifest-path cli/Cargo.toml --workspace --locked --offline` with local IPC permitted.
Run both `tools/ipc/generate*.py --check`, `tools/verification/validate.py --format json`,
`tools/analysis/check_personal_paths.py`, pinned format checks and `git diff --check`.

These receipts describe a dirty implementation on the exact base, not a future clean commit.
After maintainer review/commit, rebuild and repeat checks against that clean SHA before claiming
clean-SHA evidence. No historical QA feature status or native/live-provider acceptance is promoted.
The registry validator's candidate digest is a point-in-time digest; editing this audit after its
run changes the digest, while the base SHA and dirty state remain the same.

## P8-02 ContextBudgetManager requirements

Unify Chat transcript/profile/summary/memory and Agent ContextEngine's separate caps under one
provider-ModelBinding token budget with output reserve and multimodal accounting. Consume already
authorized scoped candidates; budget/relevance must never grant scope. Freeze request identity,
selected workspace and personal policy before asynchronous planning; reject stale work when those
change. Preserve stable record/source/provenance through truncation and expose omission reasons
without leaking excluded values. Keep generated/inferred/unknown content distinguishable from user
statements. Define deterministic fallback when tokenization is unavailable. No automatic memory
promotion, embedding jobs, model downloads, residency changes or permission expansion. Scoped
management/export/correction/deletion and richer verified source resolution remain separate work.

## Maintainer-authorized integration addendum (2026-10-19)

The maintainer explicitly authorized separate commits, feature-branch publication and main
integration after the original implementation handoff. Implementation and its regression tests
are committed together as `19f223e9`; architecture and this evidence are a separate documentation
commit on `feature/p8-01-scoped-memory`. No other feature branch is needed for these cohesive changes.
Fetched origin and confirmed main/origin/main remained the verified base `876d6106` before delivery.

Delivery checks reused `build/tests` and `cli/target`:

- Configure and full C++ build passed (`p801-integration-configure.log`,
  `p801-integration-build.log`).
- Initial sandboxed sequential CTest: 132/168 passed, 36 failed, 146.10 seconds;
  local socket/process restrictions prevent treating that run as acceptance.
- Permitted sequential full CTest: 167/168 passed, 184.36 seconds. The sole failure was
  Desktop-daemon E2E opening `absent.sock` under the long platform temporary path.
  Repeating that test with `TMPDIR=/tmp` and disposable HOME/XDG passed 1/1 in 3.89 seconds.
  These are separate receipts, not a claim that the initial full run passed 168/168.
- Rust fmt and all-target locked/offline Clippy passed. Sandboxed Rust tests had 6 passes and
  9 socket-permission failures in CLI commands; permitted workspace repeat passed all 142 tests,
  zero failed/ignored.
- Both actual IPC generators (`generate.py`, `generate_desktop.py`) passed `--check`.
  An initial mistaken invocation of nonexistent `generate_rust.py` failed without changing files.
- QA registry/schema validation passed, retaining the existing missing external
  `roasd.packaging` evidence warning and no release-certification promotion.
- Personal-path scan: zero flagged files. Credential-signature scan: 28 changed files,
  zero flagged (private-key/GitHub/AWS/OpenAI signatures, contents suppressed).
- `git diff --check` passed. Earlier formatting/static-analysis receipts and their limitations
  remain as recorded above; no new whole-repository static-analysis claim is made.

Delivery logs use the ignored `build/tests/p801-integration-*` prefix. No production user database,
provider credentials or live paid inference were used. The socket-path workaround changes only the
test environment, not product code. The separate clean-commit full-suite receipt is retained in
`build/tests/p801-integration-clean-ctest.log`; consult its result independently of prior receipts.
