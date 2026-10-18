# Full System Verification foundation

`registry.json` is the machine-readable inventory; `schema.json` defines registry and evidence
records (`$defs/evidence`). `tools/verification/validate.py` is an offline, standard-library-only
validator. JSON Schema is published as Draft 2020-12, but the dependency-free evaluator deliberately
implements only the keywords used in this schema and fails closed on unsupported keywords. It is
not a general JSON Schema implementation. No server, database, dashboard or new runtime is needed.

From the repository root:

```sh
python3 tools/verification/validate.py
python3 tools/verification/validate.py --format json
python3 tests/verification/test_registry.py
ctest --test-dir build/tests -R 'test_verification_' --output-on-failure
```

The CLI writes only to stdout/stderr, reads Git identity and repository references, and never reads
application settings, conversations, credentials or full environment variables. It performs no
network requests or application-state writes. Exit 0 means **structurally valid inventory**, not
test success, feature acceptance or release readiness. Exit 1 means invalid schema/relationships or
unsupported claims; exit 2 means unreadable/malformed input. Unknown, blocked, unsupported, skipped,
failed, stale and expected-failure evidence can remain represented without invalidating an honest
unverified inventory. Claims beyond their evidence are invalid.

## Independent dimensions

- `implementation`: implemented, partial, planned, not_implemented, stub, unsupported, unknown.
  Source inspection establishes this bounded statement, never roadmap prose alone.
- `coverage`: mapped or unmapped. A map identifies inspected source/scenario and its limitations;
  it is not a test execution or a coverage percentage. A boundary-only prerequisite test does not
  prove the parent feature is implemented (for example unattended automation or WebUI/ACP).
- `verification`: unverified, deterministic, integration, end_to_end, native, blocked, unsupported.
  A claimed tested level requires eligible receipts for **every qualified platform**, not any one
  convenient host. Unknown/conditional platform support remains qualified work, not an exclusion.
- `acceptance`: pending, blocked, accepted. Accepted additionally requires native-level eligible
  evidence, a clean candidate, an explicit reviewer and associated criteria already Passed. The
  validator does not appoint reviewers, approve decisions, edit acceptance records or certify a release.
- `criteria`: the original 65 AC IDs, phase IDs and historical matrix states are retained. Feature
  associations are **phase-level scope**, not assertions that the mapped scenario covers every AC
  oracle. Existing pending decisions remain pending. Owners/reviewers are null where not established.

`FSV-*` IDs are new verification inventory identifiers, not replacements for AC/P0–P12 IDs. Component
IDs (`sentinel.runtime`, `.desktop`, `.terminal`, `.protocol`, `.extensions`, `.packaging`,
`.verification`) do not encode directories or assert independent repositories. All currently point
to `sentinel-projects/sentinel`. The external `roasd.packaging` responsibility is recorded as
`sentinel-packaging` without inventing a GitHub URL, commit, owner or existing repository contents.
Its missing source identity is a warning and blocks evidence whose feature scope depends on it.
`required` records release-closure expectations; it does not poison unrelated runtime-only receipts.
No GitHub resources or repository split were created.

## Receipt provenance and freshness

Each evidence record contains source SHA, branch, dirty flag and digest, component/version closure,
test/scenario ID, platform, architecture, OS version, toolchain, build profile, timestamp, exact
pass/fail/skip/xfail/unknown outcome, verification level, runtime environment, bounded scope,
limitations and repository-relative artifact/log paths with SHA-256 hashes. No receipt is inferred
from a document, workflow, test filename or a passing command mentioned in an old audit.

The initial registry deliberately contains **zero execution receipts**. Older certification reports
are context, not receipts reconstructed with guessed timestamps/OS/toolchain/digests. Unit-test
receipts use explicitly synthetic identities and never enter the production registry. QA-01's own
test results are reported in its audit without promoting the whole ecosystem to tested/accepted.

Eligibility requires a passing outcome plus matching candidate SHA, branch, tree state and dirty
digest; valid UTC-qualified time, no future timestamp and age within `max_age_days`; matching
component closure; and current artifact hashes. A dirty candidate can support exact-digest fixture
evidence but cannot be accepted as a clean release. A current failure/skip/XFAIL/unknown prevents a
tested-level claim even alongside a passing rerun; history is never silently erased. Native evidence
must explicitly identify a native runtime environment; a mocked test cannot claim native acceptance.
External components bind their own exact SHAs rather than falsely using the application's SHA.

The initial 30-day evidence age window is a **proposed curation interval**, not an adopted performance
budget, retention policy or acceptance decision. SHA/branch/digest mismatch invalidates eligibility
immediately even within that window. Any real certification also needs review of binary/config/model
identity and independent oracles; source SHA alone cannot establish binary equivalence.

The dirty digest hashes `git diff HEAD --binary` and sorted nonignored untracked regular files.
Ignored build outputs are not source identity: bind binary/log artifacts independently with hashes.
Generate receipts and redacted logs under the **existing ignored build directory**, and pass a
receipt-bearing registry with `--registry build/tests/verification/receipt-registry.json` if needed.
This prevents a receipt from including its own bytes in the source digest. Publishing receipts into
tracked source changes the candidate and requires explicit rebaselining, never automatic inheritance.
The validator does not collect or write these receipts, nor execute stored scenario strings.

Artifact/source references reject absolute paths, parent traversal and symlinks escaping the root.
Do not store keys, tokens, user profiles, conversations, raw private prompts, personal home paths or
environment dumps in metadata/artifacts. Use synthetic/redacted bounded logs. Hashing proves byte
identity, not that an artifact is truthful or redacted. Human review and the existing privacy gates
remain necessary; do not point this registry at unreviewed private logs. Diagnostics show controlled
IDs/reasons, not evidence payloads or arbitrary environment dumps.

## Coverage and gaps

See [coverage snapshot](../docs/verification/COVERAGE.md) and the authoritative registry. Mappings
include CTest/QtTest, Rust, QML/CI lint, IPC generators, packaging and privacy gates. Existing Linux,
macOS, Windows, Fedora and Rust workflow files define execution plans, not successful runs. Native
KDE consent/focus, installed CLI/TUI/Desktop consistency, authorized real providers, memory quality,
background authorization, multi-user IPC, actual RPM upgrade/removal and release/security closure
remain required evidence gaps. Exact SHA/component receipts must precede any promotion.

## Proposed QA-02 end-to-end architecture

1. Keep collectors separate from this read-only validator. Run existing CTest/Rust/QML/package gates
   in disposable profiles against declared source/binary/config digests; collect all attempts and
   terminal statuses, including skips/XFAILs. No command is taken from a registry string for execution.
2. Build a bounded scenario graph over existing AgentRuntime, ToolExecutionGateway and daemon IPC:
   Chat → Agent → approved tool → independent artifact oracle → accepted final. Include denial,
   cancellation at every stage, repeated/foreign callbacks and failure with partial artifacts.
3. Add provider/model switching and missing-capability truthfulness with a frozen ModelBinding;
   daemon restart/reconnect with generation fencing and no mutation replay; CLI/TUI/Desktop observe
   the same session state. Start with synthetic fixtures, label integration separately from native.
4. Add committed-memory persistence/isolation and context oracles, explicit background admission
   authorization, synthetic attachments and speech null/refusal paths. Live speech/model/native cases
   are separately opt-in; never obtain credentials/models or incur cost implicitly.
5. Define native cells for Fedora KDE Wayland, X11 where supported, macOS and Windows. Capture
   real keyboard focus/consent/permissions, terminal input and package install/upgrade/removal in
   disposable OS environments. Missing cells remain open, not substituted by offscreen Qt tests.
6. Collect packaging-consumer receipts against exact producer/consumer versions and component SHAs.
   Compose a reviewed release manifest only after PD-D01/D02/D03/D04/D06/D07/D10/D11 and other
   applicable decisions close. Keep external component gaps explicit. Signing and native supervisor
   acceptance require separate authorized environments; no release dashboard is proposed here.
