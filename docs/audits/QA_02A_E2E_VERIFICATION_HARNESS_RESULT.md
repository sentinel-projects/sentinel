# QA-02A — bounded verification harness and evidence collection

Status: incremental fixture infrastructure; no complete system certification.

## Verified Git base and scope

Repository: `sentinel-projects/sentinel`, existing supplied checkout. Personal-Brain was not on
PATH; the supplied checkout was verified through its Git root and origin. Origin was fetched;
clean `main` and `origin/main` both resolved to `4f4ceefe2c48116970a29bdebc90ceca8d9c4a26`.
Main contains P2-M06 (`add5b071`), QA-01 (`0c4c73fa`) and P3-05B (`4f4ceefe`). Created the previously
absent `feature/qa-02a-e2e-verification` branch from that base. No commit, push, merge, reset, stash,
clean, PR or workflow dispatch. Existing CMake tests tree and Rust target directory were reused.
One coding agent worked on this change.

Modified/added files:

- `tools/verification/e2e.py`
- `tests/verification/test_e2e.py`
- `tests/core/test_daemon_ipc.cpp`
- `tests/desktop/test_agent_reliability_ipc.cpp`
- `tests/CMakeLists.txt`
- `verification/schema.json`
- `verification/registry.json`
- `verification/README.md`
- `docs/verification/COVERAGE.md`
- `docs/audits/QA_02A_E2E_VERIFICATION_HARNESS_RESULT.md`

## Inspected topology and classification

Read architecture, Agent Runtime, security, building/testing, QA-01 schema/validator/inventory and
QA-02 proposal. Chat follows a local socket request through DaemonIpcServer, ApplicationController,
ChatModeService, ModelService binding and IChatProvider. The deterministic provider replaces remote
inference. Streaming output and terminal state are observed through IPC. The extended Chat test
opens the real SQLite session projection store, destroys/recreates the IPC server, reloads that
store and compares the completed output. This proves durable daemon projection, not independent
conversation-store restart recovery.

Agent IPC tests compose the production controller/runtime/AgentLoop and approval boundary with a
synthetic planner and narrowly prescribed executor. A real disposable file mutation is checked
independently through the entire workspace SHA-256 manifest and executor counters. AgentLoop's
`acceptFinalAnswer` remains authoritative; tool success alone is not completion. The fixture final
uses Context grounding; it does not establish provider-generated, independently Verified grounding.
The full provider-driven Agent/tool chain is therefore Blocked for E2E-02.

Existing registry, tool-policy, provider-protocol and SQLite store tests are component/contract
coverage. The daemon and desktop IPC suites cross actual socket, controller and runtime boundaries.
Rust command integration tests launch a real CLI against a simulated socket server; they establish
client contract behavior, not production daemon/Agent completion. Existing COR-18 wire tests inject
stale/duplicate/foreign frames; COR-19 checks monotonic generation-tagged events. Neither proves
process crash/restart fencing. Existing restart tests reconstruct the server while retaining the
controller; their independently queried SQLite reconciliation oracle is useful supporting evidence.
No isolated unit test has been relabeled as full product certification.

## Scenarios and oracles

| ID | Requested scope status | Implemented probe/oracle | Registry mapping |
|---|---|---|---|
| E2E-01 | Bounded fixture scope executable | Streaming deltas equal expected response; reloaded SQLite projection is completed with identical output | CHAT, IPC |
| E2E-02 | Blocked | Authorized prescribed mutation, tool observation, one accepted final and reconnect tested; missing provider-driven planner/production tool executor chain | AGENT, APPROVAL |
| E2E-03 | Bounded fixture scope executable | Denied approval; entire workspace unchanged; zero mutation calls; no grant; replay rejected | APPROVAL, TOOLS |
| E2E-04 | Bounded fixture scope executable | Cancel before authorization; one terminal event, no completion; fresh client sees cancelled; workspace unchanged and replay rejected | AGENT, APPROVAL |
| E2E-05 | Blocked | Interrupted projection reconciles to failed/daemon-restarted; missing whole-process restart plus stale generation/no-replay oracle | IPC |
| E2E-06 | Bounded fixture scope executable | Socket disconnect during delayed Chat; reconnect snapshot remains completed with output | IPC, CHAT |
| E2E-07 | Blocked | Completed projection survives server recreation; missing independent conversation-store cross-session recovery/isolation flow | SESSIONS |
| E2E-08 | Bounded fixture scope executable | Client events and daemon snapshot agree on terminal run; one mutation, monotonic generation-tagged sequence and exact manifest | IPC, AGENT |

Mapping names above use the `FSV-` prefix in the registry. Cancellation coverage is deliberately
limited to awaiting approval; cancel/complete races and other stages retain their existing scoped
tests and need broader E2E work. No physical model quiescence is inferred from UI cancellation.

## Collector, evidence and failure handling

The runner allowlists executable/slot pairs in code. It never executes strings from registry data.
Each subprocess gets a temporary profile, stripped environment, offscreen Qt and 45-second timeout.
Selected probes spawn no child processes. Timeout kills/reaps the owned test process. Qt fixtures
release bounded worker gates, destroy servers/controllers and clean temporary resources. No model
server is invoked or terminated and no commercial API, user credentials, model files or conversation
profile is used. Diagnostic output is capped at 64 KiB after path/credential redaction; unredacted
subprocess output is never saved by the collector.

Each receipt uses the existing evidence schema: exact HEAD/branch, dirty state and QA-01 digest,
scenario, component closure, OS/architecture, reported Qt/compiler toolchain, profile, UTC timestamp,
actual outcome, limitations and hashed diagnostic/binary manifests. Binary manifests bind exact
executable bytes, and `--verify` detects changed binaries. Receipts live only under existing ignored
build output directories, never in the hashed source tree. No execution receipts are inserted into
the historical registry and no component provenance is automatically rewritten.

Outcomes `pass`, `fail`, `skip`, `xfail`, `blocked`, `unverified` are distinct; legacy `unknown` is
preserved. Only pass is eligible under QA-01 rules. A successful supporting probe cannot promote a
Blocked scenario. Nonzero execution, unexpected pass, malformed XML, wrong slot, absent result,
timeout, missing executable and incomplete bundles cannot become pass. Qt SKIP/XFAIL incidents
remain in diagnostics. Unit coverage includes malformed/incomplete evidence, hash corruption,
provenance mismatch, missing executable/dependency, timeout, wrong slot, redaction, environment
credential exclusion and nonpassing eligibility. Existing native authorization and COR stale-event,
duplicate-completion, cancellation and disconnect tests retain their original limitations.

Example receipt shape (illustrative fields; actual values are generated locally):

```json
{
  "scenario": "E2E-03",
  "source_sha": "4f4ceefe2c48116970a29bdebc90ceca8d9c4a26",
  "dirty": true,
  "outcome": "pass",
  "level": "integration",
  "runtime_environment": "synthetic"
}
```

This is an excerpt, not a standalone schema-valid execution receipt. Full examples are in each
local `build/tests/verification/run-*/receipts.json`. Early sandbox-restricted failures are retained
in separate run directories. Source edits after earlier runs make those receipts stale; they are
not silently replaced. After maintainer commit, rebuild and rerun collection, then `--verify` against
the exact clean candidate without source edits. This dirty-tree work does not certify a future SHA.

## Validation

Local raw build/test output remains ignored. Initial sandbox-only Rust/IPC attempts failed at
socket creation; reruns used approved local socket access. The first complete CTest run had
156 passing and 4 failing executables (217.97 seconds): agent reliability harness, daemon IPC,
desktop IPC and agent reliability IPC. Its long macOS temporary profile exceeded Unix socket
path limits. That run and its detailed SKIP/XFAIL output remain in separate ignored logs; the
final sequential rerun uses a short disposable `/tmp` profile. The collector now enforces a short
POSIX temporary base for the same reason. No application behavior was changed to hide the failure.

- Full C++ build: Passed, existing `tests` preset. The first ccache attempt could not write its
  configured external cache; the successful build used `CCACHE_DISABLE=1` in the same build tree.
- Rust fmt and workspace/all-target Clippy with `-D warnings`: Passed.
- Rust workspace tests: 142 passed, 0 failed (15 CLI integration, 27 IPC, 22 mascot, 78 TUI;
  zero-test binary/doc targets also passed).
- Registry validator: valid; release certification remains false. QA-01 unit suite: 31 passed.
- Collector unit suite: 13 passed on macOS, including a real timeout/PID oracle proving the owned
  child was killed and reaped. POSIX PID oracle is explicitly skipped on other platforms.
- Both IPC generator `--check` commands, clang-format on changed C++ files, Black, Ruff, Python
  compilation, personal-path scan and changed-source credential signature checks: Passed.
- Historical inventory comparison: 43 feature identities, 65 byte-equivalent criterion records,
  and all feature fields other than appended test mappings preserved.
- `git diff --check`: Passed.

- Final full sequential CTest: **160/160 executables passed**, 0 failed, 179.78 seconds,
  using an isolated short profile and finite test deadlines. The raw detailed log preserves
  **10 Qt SKIP incidents and 2 XFAIL incidents** inside otherwise passing executables.
  These include unavailable native credential/shell paths, opt-in live classification, missing
  incremental tool assembly, unavailable model leases, and the existing provider/endpoint-switch
  owned-child expected failures. They remain gaps rather than newly Passed acceptance criteria.
- Fixture receipt collection: **5 bounded scopes Passed; E2E-02, E2E-05 and E2E-07 Blocked**.
  Final collection runs after this report is saved and is checked with `--verify` without further
  source edits. Actual timestamps, digest, binary hashes and bounded diagnostics are in its unique
  ignored run directory. Full schema/provenance/artifact validation is separate from acceptance.

## Remaining gaps and QA-02B plan

1. Add a test-only daemon composition/launch seam with a scripted `IChatProvider`, production
   LLM planner, production ToolExecutionGateway/RealToolExecutor and SQLite conversation store.
   Drive it through the client socket, verify a real authorized write through read-back evidence,
   then require AgentLoop to accept a grounded final answer. Keep this injection out of production
   CLI/environment options and never expose an insecure public test-provider switch.
2. Run that composition as a test-owned child with an explicit PID/endpoint/profile and bounded
   shutdown. Interrupt it at deterministic barriers, restart from the same disposable store, prove
   generation change, reject stale completion/approval frames and compare mutation manifests/counts.
3. Exercise two concurrent conversations, independent persisted histories and new-process recovery;
   compare direct SQL state with fresh client snapshots and check no content crosses sessions.
4. Expand cancellation barriers through provider streaming and tool execution, including duplicate
   completion and cancel/complete races. Distinguish logical cancellation from physical model release.
   Physical leases need separate implementation/acceptance; PD-D10 remains Proposed.
5. Add opt-in real Cloud/local model acceptance with explicit authorized resources and reproducible
   model/provider/binary identities. Fixture receipts must remain separate from live-provider results.
6. Add native GUI flows (loading, empty/error, keyboard/focus, responsive states), Fedora KDE/Wayland
   portal/focus behavior, Windows named-pipe transport/authorization, disposable package install and
   upgrade, and producer/consumer release verification with exact external repository identities.
   Native platform, GUI, package and cross-repository gaps remain open until those runs exist.

The registry still contains 43 feature identities and all 65 unchanged historical criteria. Only
scenario mappings were appended; no implementation, verification, acceptance or criterion status
was promoted. Coverage is mapping-level information, not a numerical system-certification claim.
