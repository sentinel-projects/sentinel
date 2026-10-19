# P8-04 — Intelligent Memory Retrieval & Lifecycle

Date: 2026-10-19 (Europe/Istanbul).

## Status and verified Git base

Production scoped lexical ranking and event-driven SQLite index lifecycle are
implemented in the existing memory store, Chat, Agent and Desktop paths. Semantic
similarity is explicitly unavailable; no fake/overlap score is represented as an
embedding measurement. Large/chunked archives and multi-domain backup recovery remain
P8-05 work, with existing protective blocks intact.

- Existing supplied checkout; origin `https://github.com/sentinel-projects/sentinel.git`.
- Base: `64f66ac533f4923eac4eb65239edca01bfc72b8b`.
- Branch: `feature/p8-04-memory-retrieval-lifecycle`, created only after verifying
  clean main equal to origin/main and confirming the branch did not already exist.
- P8-01 `19f223e9`, P8-02 `514d2741` and P8-03 `64f66ac5` are integrated ancestors.
- Initial preflight correctly stopped because P8-03 was not on main. The maintainer
  then explicitly authorized its integration and continuation. Main was fast-forwarded
  to the already tested P8-03 commit and pushed before starting P8-04. This prerequisite
  integration is separate from P8-04, whose modifications remain uncommitted.
- AGENTS, architecture, runtime/security/build/testing instructions, prior phase
  reports and the production source flow were inspected. Personal-Brain was absent
  from PATH; no alternate workspace was guessed. The thread inventory showed this as
  the only active coding agent on the checkout; no subagents were spawned.
- Approximately 84 GiB disk space was available. Existing build/tests and cli/target
  were reused; temporary analysis databases are not build trees. No clone, worktree,
  duplicate checkout/build, reset/stash/clean/rebase, P8-04 commit/push/merge, PR or
  workflow dispatch was performed. No actual user database was opened for testing.

## Architecture audit: existing versus new

| Owner | Existing limitation | P8-04 behavior |
|---|---|---|
| SQLiteMemoryStore / IMemoryStore | Scoped LIKE term matches, recent-first LIMIT; FTS5 only for Legacy | Same scoped contract, bounded authorized candidate pool, shared lexical ranking, optional scoped FTS5 and explicit status/repair |
| InMemoryStore | Scoped substring matching sorted by recency | Locked direct retrieval with the same ranking and bounded scoring; honest Direct index state |
| ScopedMemory / MemoryManagement | Authority, metadata, CAS CRUD, explicit moves and versioned archives | Preserved; retrievalScope is a restriction only, ranking metadata separate from durable record metadata |
| ChatModeService | Scoped recall but budget priorities re-promoted Session scope | Ranked candidates become descending optional-memory priorities in existing ContextBudgetManager |
| ContextEngine / AgentLoop | Recent candidates then unconditional Session-first sort | Keeps shared ranked order; actual AgentLoop still supplies runtime-owned session/workspace |
| AgentRuntime / tool search | Gateway-owned session resolver, no model-selected scope | Unchanged authority; calls the improved existing retrieveMemory implementation |
| ContextBudgetManager | ModelBinding/output reserve, optional units, security/evidence refusals | Unchanged admission; improved candidate order only, no bigger limits or required-content clipping |
| SemanticRetrieval | Explicit fake test embeddings, metadata overlap simulation, gated local provider/vector operations | Audited, not activated or reused as a production similarity signal |
| CompactionService | Separate provider-backed conversation summarization | Unchanged; no durable memory facts, promotion or new inference |
| ApplicationController / Desktop | Authorized CRUD/inventory/archives through typed manageMemory | Adds scoped ranked recall preview, relevance explanations, index state and explicit confirmed bounded refresh through the same action |
| BackupService / archives | Transactional scope archive; combined memory backup blocked | Format unchanged; triggers update imported projection in the same transaction; protective limits/blocks remain |

FTS5 legacy bm25 remains a compatibility search API, excluded from automatic scoped
recall. The existing SemanticRetrieval implementation also contains token-overlap
metadata experiments and fake vectors; those are not validated semantic measurements
for scoped memory. A real local provider adapter exists behind its own explicit
policy/network gates, but this milestone neither calls nor enables it. Provenance
and timestamps describe stored historical data, not verified current facts.

## Retrieval and relevance rules

SQLite first constrains scope/owner/workspace with the authoritative predicates,
then discovers literal or ready-FTS token candidates before delivering source rows.
InMemoryStore checks visibleMemory before matching. Personal stays explicitly opt-in,
Legacy never enters automatic recall, and a scope restriction cannot expand access.
Stable UUIDs/source/provenance/timestamps survive selection unchanged. Corrupted
persisted record metadata fails explicitly rather than being silently rewritten.

Candidates are capped at 200; matching considers at most six terms of 64 characters.
SQL candidate preparation prioritizes literal exact key, key phrase, value phrase,
modification time and UUID. A ready scoped FTS5 index supplements token discovery;
its global bm25/IDF statistics are deliberately not used for ranking or authority.
This bounded pool is not an exhaustive global top-k guarantee on arbitrarily large
profiles. At most 50 results are returned (Chat 8, Memory preview 10, existing Agent
candidate limits preserved).

Within that authorized pool, the shared deterministic integer lexical score is:

- Case-folded, whitespace-simplified exact key: +10000.
- Key phrase: +1200; value phrase: +600.
- Each distinct term in key: +80; in value: +20.
- All query terms covered: +200.
- Equal scores: Session, Project, explicit Personal; then modification time descending;
  then UUID ascending. No recency/provenance signal grants authority.

Result metadata distinguishes exact key, phrase, matched/total terms and lexical
score. Semantic similarity is unavailable/null, never zero masquerading as a measured
embedding similarity. Empty queries give bounded scoped candidates without claiming
query relevance. SQLite LIKE/lower fallback has ASCII case behavior; the shared
post-discovery scorer uses Qt case-folding and ready FTS adds Unicode token discovery.
The fallback does not promise stemming or exhaustive Unicode equivalence.

## Index lifecycle and recovery

Schema 6 adds one optional scoped FTS5 projection in the existing Qt SQL memory
owner/database; no parallel storage/embedding subsystem was created. Existing v5
UUIDs, corrections, sources, dates, scopes and Legacy keys/native IDs are unchanged.
The projection carries UUID/revision and scope/ownership alongside key/value, enabling
validation against the authoritative source row rather than trusting stale postings.
Unsupported future schema refusal remains intact.

Three scoped insert/update/delete triggers maintain only the affected projection
row atomically with the source operation. They cover raw scoped writes, management
create/correct/delete/clear, explicit moves, import and restore. A failed source
transaction rolls back projection changes too. Workspace changes alter the trusted
query context and Desktop state, never reclassify records or change index authority.

Index status validates the FTS object with a read-only MATCH probe, expected trigger
SQL, authorized row UUID/revision/content/scope consistency and authorized orphans.
States are Ready, Stale, Unavailable, or Direct for the index-free InMemoryStore.
Missing/corrupt rows and dropped hooks are detected without returning foreign content
or counts. Stale/unavailable indexed retrieval uses authoritative lexical fallback,
never unrestricted Legacy/global search. A failed MATCH during retrieval also falls
back and reports unavailable rather than hiding the failure as semantic readiness.

Explicit REFRESH repairs at most 100 authorized source rows and 100 authorized orphan
projection rows per transaction. Missing tables/hooks can be recreated without a
full source rebuild. Other workspaces are not fetched as repair candidates. Further
batches remain explicit user actions; there is no polling, automatic embedding job,
full rebuild per mutation or memory expiration/deletion. Restart/interruption recovery
is based on persisted source/projection discrepancies, so no volatile progress cursor
can skip records. A batch rollback leaves the prior state intact.

Migration adds the optional projection/hooks without an unbounded startup backfill;
existing rows use lexical fallback until explicit bounded refresh completes. SQL may
scan authorized records for health and candidate discovery: the row/result caps are
not a hard CPU-time or database-scan guarantee. Physical SQLite/FTS corruption beyond
supported projection repair can remain unavailable; the probe is not a full integrity
certification and no destructive automatic database repair is attempted.

## Production integration and protections

Chat snapshots scope and retrieves before asynchronous provider dispatch, then gives
ranked memory items descending optional priorities under the actual resolved
ModelBinding. Agent ContextEngine keeps ranked order, so an exact Project key can
beat a weak newer Session match. AgentLoop's runtime-owned identity, required security
instructions, necessary execution evidence, existing output reserve, overflow refusal,
cancellation and accepted-final-answer completion rules are unchanged.

Memory values remain explicitly untrusted historical data. GeneratedSummary,
Inferred, Unknown and importedUnverified remain distinguishable. Ranking does not
create/correct/promote durable facts or make a retrieval success a Completed Agent
run. ToolExecutionGateway and PD-D03/PD-D04/PD-D10 are unchanged. P8-02's image-token
limitation remains fail-closed; no vision bypass or live-provider certification claim.

The Memory page keeps its established layout and CRUD confirmations. Ranked preview
shows the top authorized lexical records with explanations and cap status. Index
state and refresh/import errors use production controller/daemon replies. Refresh
requires explicit REFRESH text, initially focused Cancel and restores focus after
success/cancel. QML only presents Core authority. Existing DELETE/MOVE and Personal
consent remain. Content/metadata is plain text, never trusted instructions.

`manageMemory` adds documented search/indexStatus/refreshIndex operations within
its existing canonical typed object request/reply. No wire command, new method or
schema type changed; both existing generators pass check. Search diagnostics expose
only authorized candidate counts and flags, no foreign records, query text, secrets
or synthetic similarity scores in logs.

## Archive compatibility and remaining limitations

Version 1 scope archives remain compatible: 500 records, 96 KiB, 64 receipt history
bound, UUID/scope/source/provenance/date preservation and explicit conflict policy.
Import and index writes are transactional; retrieval after import/restart is tested.
No archived owner is rebound to a foreign workspace. Existing explicit capacity
errors and failed-import rollback remain covered by P8-03 regressions. Index refresh
changes projection only, not values, receipts or archive contents.

Larger/chunked archives, receipt lifecycle, cross-process snapshot exports and
multi-domain rollback remain P8-05. Generic combined memory backup remains blocked
for managed data; no guard was bypassed to claim a complete application backup.
No automatic expiration, durable promotion, hidden inference, embeddings, downloads,
external provider requests, cross-project sharing, resource-policy enforcement,
model lifecycle/process ownership or automatic unload changes were introduced.

## Focused regression and complete validation

Validation ran on macOS arm64 with Qt offscreen/software rendering, using disposable
HOME/XDG profiles, workspaces and SQLite databases. No live provider, model download,
user credential writes or native-platform certification was performed. Logs use the
ignored build/tests/p804-* prefix.

| Check | Actual result |
|---|---|
| Configure / complete C++ build | Passed cmake tests preset; full build p804-build-full/verified/final logs and final UI rebuild p804-build-delivery-complete.log |
| Final UI/retrieval delivery check | 2/2 passed, 5.50 sec (p804-focused-delivery-final.log); refresh popup closed, scope-selector focus restored, final screenshot visually inspected |
| Initial focused production suites | 8/8 CTest programs passed, 37.45 sec (p804-focused-first.log) |
| **Sequential full CTest** | **171/171 passed, zero failed, 250.68 sec** (p804-ctest-final.log), `-j 1 --timeout 180 -V` |
| Retrieval regressions | 8 Qt cases passed: both backends, exact/phrase/term relevance, isolation, deterministic tie/candidate cap, CRUD/move/import/restart, v5 migration and stale/corrupt/missing projection recovery |
| P8-01/P8-02/P8-03 | Scoped 9, SQLite store 15, budget manager 10, management 7 Qt cases passed in full run |
| Actual Chat / Agent | ChatModeService 19 cases passed including old exact Project key ahead of newer weak Session candidates; ContextEngine selected ranked memory under existing budget; full AgentLoop/Runtime/evidence suites passed |
| Product / IPC | ApplicationController 131, settings/recovery 17, Desktop view-model 75 and Desktop IPC 73 Qt cases passed |
| Rust fmt / Clippy | fmt check and workspace/all-targets/locked/offline Clippy `-D warnings` passed |
| Rust workspace tests | **142 passed**, zero failed/ignored, locked/offline with local fixture IPC permitted (p804-rust-tests.log) |
| Canonical IPC checks | Both existing generators passed `--check`; no new wire method/type to regenerate |
| QA registry/schema | Valid, preserving existing missing external `roasd.packaging` evidence warning; no release/native/live status promotion |
| Pinned clang-format | 18.1.8, new C++ sources/test fully clean; changed hunks checked with empty format diff |
| Scoped clang-tidy | Seven changed production translation units all exit 0; new sources/contract additions and changed lines elsewhere; zero reported project diagnostics in p804-tidy-verified-summary.log and corresponding logs |
| Privacy / credentials | Personal-path scan zero flags; changed-file credential-signature scan zero flags, content suppressed |
| Whitespace | `git diff --check` passed |

Full CTest includes real daemon-backed keyboard scenarios for ranked preview,
semantic-unavailable labeling, explicit bounded REFRESH and preserved create/correct/
DELETE confirmation flows. Captures use the existing
build/tests/desktop-daemon-verification/p804 directory. Visual inspection found the
refresh-success popup remained open; its close/focus restoration was fixed afterward
and verified separately in the final narrowed UI run. Thus the full-suite receipt
precedes that final localized QML correction; final rebuild and focused UI/retrieval
receipts cover the delivered version. Earlier narrow checks initially used the
visible-only QQuickItem helper incorrectly for a Popup/hidden control, causing one
fixture segfault and one failed assertion. The helper assertions now require absence
of a visible Cancel control and restored scope-selector focus; no product authority
or confirmation was weakened. Initial compile checks also corrected the Agent context
source member and replaced a deprecated Qt UTC test constructor. Raw failed receipts
are retained; none was relabeled as a pass. Whole-project warning-free compilation
is not claimed; existing ad-hoc signing/font/platform/rejection warnings remain.

Focused tests exercise authorized source filtering before scoring; Personal opt-in;
Legacy exclusion; old exact Project facts ahead of recent weak Session facts; UUID
and provenance retention; correction/deletion/move and import-trigger consistency;
partial refresh followed by restart/resume; corrupted values, dropped FTS table and
dropped trigger repair; foreign-scope repair exclusion; and explicit unavailable
semantic similarity. Production IPC verifies exact-key explanations with null semantic
similarity and REFRESH confirmation. ContextBudgetManager still rejects mandatory
security/evidence overflow and unknown image accounting in its existing regressions.

### Preserved SKIPs, XFAILs and warnings

The full run has **10 internal Qt SKIPs, 2 XFAILs and 81 QWARN lines**; all 171 CTest
programs passed. These do not certify native Fedora/KDE, platform credentials, real
provider reliability or unresolved process/resource contracts.

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

## Recommended P8-05

Review a consistent snapshot/chunked scope archive and multi-domain rollback format,
with explicit receipt-history lifecycle and stronger recoverable FTS corruption
maintenance. Keep optional validated embedding provenance/authorization, Personal
sharing and provider image accounting separate reviewed milestones. Improve bounded
Unicode fallback/candidate guarantees without using global scores or authority.

## Maintainer-authorized delivery

After the uncommitted implementation handoff, the maintainer explicitly authorized
committing P8-04, publishing its feature branch and integrating/pushing main. Origin
was fetched and main/origin/main still matched the verified base before delivery.
The validation receipts above describe the implementation candidate and final scoped
UI verification; publication does not constitute a new clean-commit full test run.

## Exact changed-file inventory

- `core/CMakeLists.txt`
- `core/include/sentinel/core/interfaces/IMemoryStore.h`
- `core/include/sentinel/core/memory/InMemoryStore.h`
- `core/include/sentinel/core/memory/SQLiteMemoryStore.h`
- `core/include/sentinel/core/memory/ScopedMemory.h`
- `core/src/agent/ContextEngine.cpp`
- `core/src/app/ApplicationMemoryManagement.cpp`
- `core/src/chat/ChatModeService.cpp`
- `core/src/memory/InMemoryStore.cpp`
- `core/src/memory/SQLiteMemoryRetrieval.cpp`
- `core/src/memory/SQLiteMemoryStore.cpp`
- `core/src/memory/ScopedMemoryRetrieval.cpp`
- `docs/audits/P8_04_MEMORY_RETRIEVAL_LIFECYCLE_RESULT.md`
- `docs/concepts/MEMORY.md`
- `docs/development/ARCHITECTURE.md`
- `docs/interfaces/IPC.md`
- `tests/CMakeLists.txt`
- `tests/core/test_chat_mode_service.cpp`
- `tests/core/test_memory_management.cpp`
- `tests/core/test_memory_retrieval.cpp`
- `tests/core/test_scoped_memory.cpp`
- `tests/core/test_sqlite_memory_store.cpp`
- `tests/desktop/test_desktop_ipc.cpp`
- `tests/helpers/DesktopDaemonInteraction.h`
- `ui/qml/pages/MemoryPage.qml`
