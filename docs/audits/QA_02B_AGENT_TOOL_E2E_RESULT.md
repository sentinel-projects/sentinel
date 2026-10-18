# QA-02B — provider-driven Agent/tool fixture E2E

Status: bounded E2E-02 fixture chain executable; no live-provider or native-platform certification.
All changes remain uncommitted. PD-D10 remains Proposed; no acceptance decision is promoted.

## Git base and workspace

Existing supplied checkout only. Personal-Brain `brain` was unavailable on PATH. Read AGENTS,
QA-01/QA-02A, P2-M06/M07, P6-05, architecture, Agent Runtime, security, building and testing
instructions. Fetched origin and verified clean main = origin/main = HEAD at
`7417a8faecced96c0c9a17deedc678c5fd0ab8ce`, including the P6-05 merge and preceding milestones.
The earlier preflight observed a maintainer Git update and stopped; the user's renewed check
established this stable base. Created previously absent `feature/qa-02b-agent-tool-e2e` from main.
One coding agent; other visible coding chats were idle/not loaded. Existing CMake `build/tests`
and Rust `cli/target` reused. Analysis-only compile database under temporary output is not a build
tree. No clone/worktree, new build tree, commit, push, merge, reset, stash, clean, rebase, PR or
workflow dispatch. HEAD/branch are rechecked through final receipt collection.

## Actual topology and smallest seam

The test-only Qt executable `test_provider_agent_e2e` composes the existing daemon service and
application builder in its process. A real DaemonClient/local socket sends session.create,
agent.start, approval.respond, run.cancel and session.attach to DaemonIpcServer. The server owns
IPC projection and peer/protocol checks; ApplicationController owns conversation selection,
workspace/model resolution and runtime composition. ModelService's existing registerProvider
factory seam supplies a scripted IChatProvider for the fixture's installed synthetic model.
Provider/catalog discovery responses are synthetic; no external model/network is needed.

ApplicationControllerBuilder constructs the real LlmAgentRuntime when an Agent runtime is
configured without a custom planner. ApplicationController binds the production ModelService
ModelBinding, wraps the backend in AgentRuntime and creates SQLiteAgentRunStore and permission
grant storage. AgentRuntime/AgentLoop use the production ObservationIntentPolicy, tool registry,
ToolExecutionGateway, approval/authorization policy and sandbox checks. RealToolExecutor's
registered handlers use QtFileSystemService. There is no custom step planner, executor wrapper,
fake handler success, fake gateway or replacement permission service in this fixture.

This existing C++ builder/factory seam was sufficient: **no production source, CLI flag, IPC
schema, environment override or public configuration was added**. The synthetic provider supplies
classification JSON, a write-file proposal, a read-file proposal and a verified final proposal.
The real planner parses them and consumes real tool observations. The real loop requires fresh
post-mutation evidence and may replace proposed prose with its evidence-derived final JSON.
Only AgentLoop::acceptFinalAnswer can produce Completed. Automatic context inspection remains
production behavior. Network policy stays LocalOnly; the injected catalog client prevents polling
from contacting a real provider.

## Executable scenarios and independent oracles

`providerDrivenChain` has seven data rows. Each starts with two disposable workspace files whose
exact bytes are `original fixture bytes\n`. The prescribed replacement is
`QA02B independently checked bytes\n`. Independent QFile reads, recursive SHA-256 manifests,
SQL queries, a separately opened SQLiteConversationStore, runtime state and IPC terminal events
are compared. Each passing row emits a bounded `QA02B_ORACLE` JSON record containing relative
filenames/hashes, classification/planner counts, persisted successful-write count, terminal count,
accepted-final indicator and persisted state.

Original SHA-256: `ed680cc34557c52d306ed65d71ad28fac6a27f2dfa1ddcaeb46d4638abded56c`.
Replacement SHA-256: `20f723f5be600224f54231297f68aacd1f33901419cf20d36060071d97256286`.

| Row | Independent outcome |
|---|---|
| allowed | Explicit one-use approval; exactly one persisted successful write and one read step; exact replacement bytes/hash; unchanged sibling hash; provider receives read-back content; Verified accepted final includes observed bytes; one completed IPC event and one matching durable assistant message |
| denied | No mutation, zero successful writes, failed terminal state, late allow replay rejected, no persistent permission grant |
| cancelled | Cancel while awaiting approval; no mutation, zero successful writes, cancelled terminal state, late allow replay rejected |
| outside | Provider proposes a file beside the workspace in the disposable directory; workspace and outside bytes remain unchanged, zero successful writes, failed state |
| malformed | Numeric path/missing required content cannot pass schema validation; no mutation, zero successful writes, failed state |
| unaccepted-final | Real write succeeds once, but no read-back and a Context final cannot complete the requested observed task; failed state and no accepted assistant final; successful side effect remains accurately visible rather than claiming rollback |
| stale-generation | Cancel old pending run; recreate only daemon server with same controller/store; generation changes. Start another approval; old run/approval identity cannot authorize it. New approval remains pending until explicitly denied; zero writes |

The allowed row holds inference at a finite semaphore barrier after write/read observations but
before final delivery. Run remains Running with zero terminal events. Replaying the consumed
approval is rejected and the manifest stays unchanged. Releasing the provider permits accepted
completion; persisted successful-write count remains one. Barriers have five-second bounds and
are released in fixture destruction before controller workers join. No test-owned child process,
model server, download or shell tool is launched. The collector owns/reaps its bounded Qt test
subprocess; QTemporaryDir resources and the unique application profile are removed.

The stale-generation row checks actual prior-generation authorization identities against a fresh
server with a new pending approval, not an invented request-generation field. Existing COR-18/19
client stale-event/sequence tests remain complementary. This is **not whole-process restart
fencing**; the controller survives. No daemon crash recovery or concurrent-session isolation is
inferred. E2E-05 and E2E-07 remain Blocked.

## Persistence boundaries

SQLite session_projection intentionally stores metadata, not output text. The test independently
queries its session/run/state and confirms output is absent. session.attach's output must equal
the accepted loop final. Conversation text stays in the separate SQLiteConversationStore; only
accepted terminal content is found there. SQLite agent_runs must have matching terminal state,
final answer and accurate failure text on adverse rows; joined agent_tool_calls/agent_steps records
count successful write-file executions. Failed/cancelled runs do not acquire persistent grants.
These are durable local records, not proof of a new-process recovery flow or E2E-07 isolation.

## QA integration and receipts

Preserved E2E-02's stable ID and updated its mapping to the real provider-driven test slot.
All **43 features, 65 historical criterion records and 8 component records are unchanged**,
including every feature status, acceptance state and test-ID list. Other E2E mappings remain.
No receipts are inserted into source registry and no component provenance is silently rebased.

The existing allowlisted QA-02A runner now executes all seven E2E-02 rows. It requires exactly
seven passing tagged incidents, completed cleanup and seven matching oracle records. Missing,
duplicate, corrupt or inconsistent rows/hashes/counters become Unverified. The JSON oracle
manifest is an independently hashed artifact beside sanitized capped diagnostics and binary hash
manifest; --verify checks it as well as schema, exact source SHA/branch/dirty digest, platform,
component closure, timestamp/freshness and executable bytes. Fail/SKIP/XFAIL never become pass.
Receipts remain integration-level synthetic fixture evidence. Native macOS/Fedora/Windows and
real Cloud/local provider acceptance remain open; AC-P2-01/03 remain open.

Initial collector dry run: six bounded scopes Passed, E2E-05 and E2E-07 Blocked. Its ignored
`run-px0kug5y` evidence is stale after subsequent source edits and must not be reused. Final
collection follows the saved report without source edits; its exact dirty digest and binary/artifact
hashes live in its unique ignored receipt to avoid a self-referential source digest.

## Validation

Existing ignored build output retains exact diagnostics; no warning-free certification is implied.

- Complete C++ build Passed using tests preset with CCACHE_DISABLE=1; no compiler errors/warnings.
  Local unsigned/ad-hoc macOS signing configuration remains expected.
- Focused 11 executables Passed in 19.44 seconds. Provider-driven fixture: 9 QtTest passes
  (seven data rows plus setup/cleanup), zero failures/skips. Final complete sequential CTest:
  **163/163 executables Passed**, **240.69 seconds** (`qa02b-full.log` and
  `qa02b-full-detail.log`). Across 143 QtTest summaries: **2925 passed, 0 failed, 10 skipped**.
  Two existing XFAIL incidents remain. There are **81 QWARN lines**, including seven expected
  invalid-approval warnings from the new fixture's rejected replays; prior suite warnings remain
  visible in the detailed log.
- The 10 skips remain: Linux credential helper, refused native credential persistence, missing
  typed native incremental tool assembly, four strict macOS child-process shell fixtures,
  opt-in live classification, unavailable missing-node branch because npx is installed, and
  absent model-residency lease/removal contract. XFAILs remain P3-02 D4 provider-switch owned-child
  cleanup and D5b endpoint-switch owned-child reconciliation. None becomes accepted.
- Rust fmt, locked/offline workspace/all-target Clippy with -D warnings Passed; **142 tests Passed,
  0 failed/ignored**. Existing target tree reused.
- Both IPC generators --check Passed; no schema/type drift. QA registry/schema valid; existing
  QA-01 suite **31 passed**, collector suite **14 passed**. New checks reject incomplete oracles,
  changed hashes/write counts/terminal states, missing cleanup and missing oracle artifacts.
- Pinned clang-format 18.1.8, Black and Ruff Passed. Scoped clang-tidy: no compiler errors;
  two bugprone-throwing-static-initialization warnings for immutable Qt QByteArray fixture byte
  constants; 623 non-user warnings suppressed by the analyzer. Braces findings from the first
  analysis were fixed in the new test; no suppressions added.
- Initial fixture run: 2 setup/cleanup passes and 6 failures from incorrectly expecting output
  in metadata-only projection. Corrected test assertions follow the actual separate transcript
  owner, then all rows passed. A temporary collector-unit XML fixture lacked newly required row
  tags/cleanup; it was updated and all 14 tests passed. No production behavior was relaxed.
- Personal-path scanner: **0 files flagged**. Scoped private-key/GitHub/AWS/API-key signature
  scan: **0 findings across all 8 changed files**. `git diff --check` Passed. No real credentials,
  user conversations or external provider requests were used.
- Host/toolchain: macOS 27.0.1 ARM64, Qt 6.12.0, AppleClang 21, CMake 4.4.4,
  clang-tidy 23.1.3, pinned clang-format 18.1.8, Rust/Cargo 1.98.1.

## Clean-SHA maintainer procedure

After the maintainer commits this change, use this same checkout and existing build directories:

1. Verify a clean working tree and record exact HEAD/branch. Rebuild with
   `CCACHE_DISABLE=1 cmake --build --preset tests`; binary hashes alone cannot prove freshness.
2. Run focused provider/Agent/gateway/IPC tests, then
   `ctest --preset tests -j 1 --timeout 180 --output-on-failure` with a disposable short profile,
   offscreen Qt and finite deadlines. Set HOME/USERPROFILE, TMPDIR/TMP/TEMP and XDG config/data/cache
   paths to that profile; preserve only PATH/SystemRoot/WINDIR from the parent environment.
   Run `cargo fmt --all -- --check`,
   `cargo clippy --workspace --all-targets --locked --offline -- -D warnings` and
   `cargo test --workspace --locked --offline` from cli; run both tools/ipc generators with --check
   and `python3 tools/verification/validate.py --format json` from the repository root.
   Local socket authorization is required; no real provider credentials are needed.
3. Run `python3 tools/verification/e2e.py`, inspect its unique printed
   `build/tests/verification/run-*` directory. Require E2E-02 pass and seven complete oracles;
   E2E-05/07 should still be Blocked. Inspect diagnostics, manifests and binary hash.
4. Without editing source or rebuilding binaries, run
   `python3 tools/verification/e2e.py --verify build/tests/verification/run-EXAMPLE` with the
   actual run directory. Confirm dirty=false, exact clean HEAD/branch/component versions and
   current artifact/binary hashes. Any source/binary change requires rebuild/recollection.

Current dirty-tree evidence does not certify that future clean commit. Do not import historical
receipts or silently combine components from different candidates.

## QA-02C remaining plan

Create a bounded test-owned daemon executable using the same private builder seam, explicit short
endpoint/profile and deterministic provider barriers. Track its PID and bounded graceful/forced
shutdown independently. Kill/restart at awaiting approval and post-write/pre-final barriers;
verify a new generation, old approval/completion rejection, no replay, direct SQL reconciliation
and exact mutation counts. Then use two conversations with distinct payloads and workspace
identities, concurrent clients and independent SQL/read-back oracles; prove recovery and absence
of cross-session content/grants. Only then reconsider E2E-05/07. Keep process ownership/model
leases/approval policy unchanged and PD-D10 Proposed; physical ownership enforcement needs its
separate authorized implementation/acceptance work.

## Changed files

- `tests/CMakeLists.txt`
- `tests/desktop/test_provider_agent_e2e.cpp`
- `tests/verification/test_e2e.py`
- `tools/verification/e2e.py`
- `verification/registry.json`
- `verification/README.md`
- `docs/verification/COVERAGE.md`
- `docs/audits/QA_02B_AGENT_TOOL_E2E_RESULT.md`
