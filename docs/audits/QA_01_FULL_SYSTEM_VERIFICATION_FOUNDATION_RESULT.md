# QA-01 — Full system verification architecture and evidence foundation

Date: 2026-10-18. Status: infrastructure and deterministic validation complete; uncommitted.
**This is not product certification or release acceptance. Existing pending decisions remain pending.**

## Verified base and workspace

- Existing repository verified with `git rev-parse --show-toplevel`; clean task entry.
- Fetched origin; main and origin/main reconciled at
  **`e639a0429baf35445daad84eb70a9d5e001cf082`**, divergence `0 0`.
- Created `feature/qa-01-full-system-verification` from main without overwriting a branch.
  The previously committed P2-M06 branch remained separate and was not merged/imported. Its
  results are not attributed to this source candidate. QA-01 inspects actual main, including
  ProviderRegistry and P3-05A reference contracts, not unmerged adapter implementation.
- Read AGENTS, architecture/Agent Runtime/security/build/testing, P0–P12 roadmap, acceptance
  matrix/decisions, certification scope statements and current CI/test/packaging/privacy gates.
- Personal-Brain `brain` unavailable; the explicitly supplied existing checkout was verified.
  macOS 27.0.1, Darwin arm64; approximately 88 GiB free initially. Python 3.15.0, CMake 4.4.4,
  Qt 6.12, AppleClang 21; Black 26.3.1 and Ruff 0.15.12 already available, no formatter installed.
- One coding agent. Existing build/tests and cli/target reused; no application source modified.
  No clone, worktree, repository copy, independent build tree, commit, push, merge, reset, stash,
  clean, rebase, PR, workflow dispatch or GitHub resource creation.

## Inventory and source-of-truth model

`verification/registry.json` contains **43 stable FSV feature IDs, all 65 existing acceptance IDs,
all P0–P12 phases, 8 component identities and 49 source-inspected test/gate mappings**.
Implementation, mapped coverage, tested verification, platform qualification and acceptance are
independent dimensions. Initial states: **34 Unverified, 8 Blocked, 1 Unsupported; zero accepted**.
42 entries have bounded maps; native notification delivery is explicitly Unmapped. No coverage
percentage or complete scenario coverage is inferred from these numbers.

The inventory covers Desktop/QML, Quick Panel, tray/notifications/native services, CLI/TUI/legacy
CLI, daemon IPC, Chat, Agent, registry/gateway/approvals, local/cloud providers/discovery, model
storage/lifecycle, telemetry/policy/performance, sessions/stores/memory/context/retrieval/workspaces,
background jobs and future automation, speech/multimodal, plugins/MCP/skills/stubs, diagnostics,
security/privacy, platform compatibility, Fedora/Ro-ASD, install/upgrade/removal and release closure.
Settings/onboarding/recovery, current mode mapping, planned physical lease enforcement and QA
infrastructure are separate entries. Planned, not_implemented and stub entries do not become
implemented because a prerequisite/boundary fixture exists.

AC identifiers and historical matrix statuses are preserved, not rewritten from test results.
The validator checks criterion ID/phase/status against the source matrix row, including rejecting
forged Passed catalog states. Feature-to-AC associations are phase-level scope references, not a
claim that each mapped scenario verifies every associated oracle. Named owners/reviewers remain
null where PD-D11 has not established them; component boundaries identify responsibility without
inventing humans. Unknown future evidence and absent external components remain visible.

## Evidence contract and validator

`verification/schema.json` publishes the registry/evidence contract as JSON Schema; its evidence
definition records SHA, dirty status/digest, branch, scenario, platform/architecture/OS, toolchain,
profile, timestamp, explicit outcome, runtime environment, hashed artifact references, feature
scope, limitations and exact component versions. The standard-library validator implements only
the schema keywords used by this contract and rejects unsupported keywords, not a general-purpose
JSON Schema engine. No new runtime dependency, database, web service or GUI dashboard was added.

Eligibility is exact-candidate and per-platform. SHA/branch/tree/digest mismatch, stale/future or
unqualified time, missing component closure, unsupported platform, changed/missing artifact and
non-pass outcomes cannot promote a feature. Current adverse results are retained even alongside a
passing rerun. Native claims cannot use a synthetic environment. Dirty matching fixtures can be
eligible at their precise digest, but do not become clean accepted candidates. Release certification
is always false: human/decision/release authority is outside this validator.

The initial registry intentionally has **zero execution receipts**. Legacy certification reports
are historical context, not receipts with guessed provenance. Unit fixtures use explicit synthetic
identities and never enter the inventory. QA-01 regression results below establish infrastructure
behavior only; they do not mark application features Passed or silently combine old task branches.
This also avoids claiming that a final documentation edit is the exact dirty snapshot of an earlier
run. Run checkpoint identity and later report edits remain distinguishable.

The CLI produces JSON and concise Markdown to stdout; invalid structure/claims exit nonzero.
Honest pending/blocked/unverified inventories remain valid, with freshness reasons and external
component warnings. Registry source refs/test scenarios and evidence paths/hashes are checked;
absolute paths, traversal and root-escaping symlinks are refused. The tool never executes scenario
strings, contacts services, reads app profiles/credentials or writes application state.

The proposed 30-day curation window is not an adopted performance/retention decision. Ignored
build outputs are not source identity; binary/config/model artifact hashes and independent oracles
are still needed for actual certification. Store new receipts/redacted logs in the existing ignored
build output area to avoid self-referential dirty digests; publication requires explicit rebaseline.
See the [contract](../../verification/README.md) for supported semantics and limitations.

## Coverage and evidence gaps

- Source/scenario maps cover actual QtTest methods, Rust IPC/TUI unit functions, QML component
  initialization, QML lint workflow, IPC generators, packaging/archive tests, upgrade store tests,
  privacy guard and Linux/macOS/Windows/Fedora/Rust pipeline definitions. A workflow is not a run.
- Current native shortcut fixtures inject a backend; no KDE consent/focus acceptance is inferred.
  Notification consent/delivery lacks a mapped focused acceptance scenario.
- Provider fixtures cannot certify real models, native streaming/tool reliability or cloud parity.
  Static capabilities remain separate from readiness and live model verification.
- D4/D5b remain XFAIL and D6 remains SKIP. The proposed lease reference model is not enforcement.
  Resource-policy fake calibration does not adopt PD-D04 budgets or PD-D10 ownership rules.
- No installed CLI/TUI/Desktop shared-session E2E parity, live memory/retrieval quality, native
  microphone/voice quality, authorized unattended automation or full package upgrade/removal
  acceptance was claimed. Generic store migration is not an RPM transaction.
- Existing SEC-P1, Windows trust/pipe, native multi-user IPC, supervisor restart and release/admin
  blockers remain open. Full application regression is not closure of P0–P12 acceptance.

## Component and organization boundary

Current application verification remains authoritative in `sentinel-projects/sentinel`.
Repository-neutral IDs describe runtime, Desktop, terminal, protocol/SDK, extensions, packaging
and verification as components, not approved independent repositories. Exact component SHAs and
tree state can be pinned independently in future multi-repository receipts. External packaging
responsibility is labeled `roasd.packaging` / `sentinel-packaging`; no repository URL, owner,
source SHA or consumer result was invented. The explicit missing-external-component warning
blocks affected scope, not unrelated runtime-only evidence. QA-02 must compose reviewed release
manifests across exact producer/consumer versions; no repository split occurred.

## Security and privacy

No keys, provider tokens, user conversations, private prompts, personal home paths or environment
dumps were collected into the inventory. Test metadata is synthetic and output is controlled IDs,
statuses and reasons. Evidence hashes prove byte identity, not truth or successful redaction;
review is still required before publishing logs. Raw regression logs remain local temporary/build
artifacts and are not imported as public registry receipts. Existing permissions, sandbox,
ToolExecutionGateway and daemon/provider credential boundaries are unchanged.

## Validation

| Gate | Actual result |
| --- | --- |
| Registry unit tests | 31 deterministic tests passed; valid records, duplicates, missing/changed artifacts, SHA/time/branch/tree mismatch, fail/skip/XFAIL, unsupported/blocked states, dirty digests, platform coverage, acceptance contradictions, authoritative criterion status, external component closure, malformed structure and deterministic error ordering |
| Registry CLI | Valid JSON/Markdown; 43 features, 49 maps, 65 criteria; 1 explicit missing external component warning, 0 errors; release_certified false |
| Focused final QA CTest | 2/2 passed, 0.72 s, after final map additions |
| Configure/full build | Existing tests preset configure and full-target build passed; 1 pre-existing HuggingFaceModelSource.cpp shadow warning |
| Initial full sequential CTest | 158/158 passed, 228.38 s; 141 QtTest suites: 2,848 passes, 0 failures, 9 skips, 2 preserved XFAILs; 72 QWARN diagnostic lines |
| Final verbose full regression | **158/158 passed**, 173.14 s; 141 QtTest suites: 2,848 passes, 0 failures, 9 skips, 2 preserved XFAILs; 72 QWARN diagnostic lines. Includes the final 31-test validator and inventory gates |
| Rust 1.94 | fmt, locked/offline Clippy with -D warnings and workspace tests passed: 142 tests, 0 failures/ignored, 8 test/doc-test suites |
| Python format/static checks | Scoped Black 26.3.1 check and Ruff 0.15.12 check passed; no C++/QML source changed, so clang-format/QML lint changes are not applicable |
| IPC consistency | Both generators passed --check; no schema/API contract changed |
| Privacy/credentials/diff | Personal-path scan: 0 flagged; all 8 changed/untracked candidate files scanned: 0 high-confidence private-key/token findings; git diff --check passed |

The earlier verbose run also passed 158/158 in 176.65 s with the then-current 30 validator
cases. A final Python-only diagnostic ordering guard/test was added, Black/Ruff and 31 unit tests
passed, and the complete suite was rerun again as the final 173.14 s result above. Results are
separate attempts, not summed or silently substituted. The final execution checkpoint was HEAD
`e639a0429baf35445daad84eb70a9d5e001cf082`, dirty=true, source digest
`6ce912e4a93d9ee55dd973bacfce40390557e2631132ef43115accecea9699c3` on the QA branch.
Final report-result edits necessarily change the final documentation-tree digest; no execution
receipt is attached to claim that changed snapshot was certified.

Local logs: `/tmp/sentinel-qa01-full-tests.log` (initial),
`/tmp/sentinel-qa01-full-final.log` (earlier verbose),
`/tmp/sentinel-qa01-full-confirmed.log` (final verbose),
`/tmp/sentinel-qa01-confirmed-checkpoint.json` and `confirmed-start.txt` with the same prefix,
`/tmp/sentinel-qa01-validator-tests.log`, `/tmp/sentinel-qa01-focused.log`, and
`/tmp/sentinel-qa01-rust.log`. Verbose raw test logs can contain local runner command paths;
they remain private local diagnostics, not public/redacted evidence receipts. Do not publish
them without sanitization. The public inventory, coverage snapshot and audit contain no personal
home paths or private test payloads. Only bounded counts/statuses are projected here.

Nine existing QtTest skips: Linux Secret Service helper on macOS (1), missing incremental native
tool assembly contract (1), strict macOS child-process fixtures (4), opt-in live classification (1),
missing-node branch unavailable because npx exists (1), D6 physical lease/deletion scenario (1).
The two XFAILs are D4/D5b, not newly passing production behavior. No native Fedora, Windows,
live provider/model, installation/upgrade/removal or release acceptance was performed.

## Proposed QA-02

The [contract's QA-02 plan](../../verification/README.md#proposed-qa-02-end-to-end-architecture)
specifies separate bounded collectors, immutable source/binary/config receipts and independently
checked oracles. Prioritize Chat → Agent → authorized tool → accepted final, denial/cancellation,
provider/model switching, daemon generation/restart/no-replay, cross-client parity, committed
memory/isolation, background authorization, native focus/consent and disposable package transactions.
Fixture integration, actual E2E and native/live runs require separate labels, declared cells and
all attempts. Real providers/models/costs and OS/supervisor/package environments remain opt-in and
authorized. No dashboard or auto-approval of pending product decisions is included.

## Exact changed files

- `verification/registry.json`
- `verification/schema.json`
- `verification/README.md`
- `tools/verification/validate.py`
- `tests/verification/test_registry.py`
- `tests/CMakeLists.txt`
- `docs/verification/COVERAGE.md`
- `docs/audits/QA_01_FULL_SYSTEM_VERIFICATION_FOUNDATION_RESULT.md`

All changes remain uncommitted for maintainer review.
