# P6-06 — production Main / daemon interaction verification

Status: bounded production-UI integration fixture verified; no interface redesign or native desktop
acceptance. AC-P6-06/07/08 remain open; PD-D10 and existing authorization policy are unchanged.

## Verified Git base and workspace

Existing supplied checkout only. Personal-Brain `brain` unavailable on PATH. Read AGENTS,
architecture/runtime/security/build/testing and P6-03 accessibility, P6-04 KDE, P6-05, QA-01,
QA-02A/02B reports. Fetched origin and verified clean main = origin/main = HEAD at
`2d335bda5cb6514b7d3364c32c050a8b73784f26` (P2-M08 merge, with QA-02B and earlier UI work integrated).
Only this coding chat was active in the visible inventory. Created absent
`test/p6-06-desktop-daemon-e2e` from main; subsequent HEAD/branch checks stayed stable.

Reused `build/tests` and `cli/target`; analysis-only compilation metadata under temporary output
is not a build tree. During implementation there was no clone, worktree, duplicate checkout/build,
commit, push, merge, reset, rebase, stash, clean, PR or workflow dispatch. Changes were uncommitted
at the validation checkpoint. The maintainer subsequently explicitly authorized scoped commits,
merging this branch into main and pushing main; publication is separate from the fixture acceptance.

## Real UI topology

The existing test-only settings_ui_certification_probe gains `--daemon-e2e`, not a production CLI
option. It mounts actual Main.qml from the production module and existing resources. Main uses
DesktopShellViewModel's remote constructor, DesktopRuntimeClient, DaemonClient, real
QuickPanelController, RemoteAgentInspectorService and AgentInspectorViewModel. Production native
adapter composition remains present but offscreen native acceptance is not inferred.

A private test header composes DaemonIpcServer and ApplicationController in this same owned Qt
process, with a real local socket and isolated SQLite conversation/memory/run/permission/projection
stores. The existing builder/ModelService factory seam injects a scripted IChatProvider and
catalog client. Chat really streams through ChatModeService and IPC. Agent really uses
LlmAgentRuntime, ObservationIntentPolicy, AgentLoop, registry, ToolExecutionGateway,
RealToolExecutor and QtFileSystemService. No planner, gateway, executor, authorization service or
persisted-success substitute is injected. No public IPC/environment/provider setting backdoor
was added; capture-directory input exists only in the test executable and is canonical-root gated.

QtTest sends actual QWindow keyboard/mouse events: text characters, Enter, Space, Tab, Escape,
outside click and navigation. Test helpers locate stable objectName controls and set initial Qt
focus before ordinary activation; they never invoke private QML send/click/approval methods.
Approval's Deny initial focus and explicit Tab-to-Allow are exercised through the actual modal.
This proves the bounded controls, not every possible sequential Tab path in the application.
Transport connect/disconnect and provider semaphore barriers are fixture orchestration/fault
injection; user operations still originate at production controls.

The test-owned provider waits at bounded Chat barriers. It returns synthetic inference only;
all socket, runtime, authorization, filesystem and persistence effects are production paths.
Five-second barriers release before worker destruction; the collector kills/reaps its owned Qt
process on a 65-second timeout. No separate daemon/model-server child is launched. Unique app
profiles and temporary workspaces are cleaned; no real provider credentials, user conversations,
model download or paid API is used. Windows execution is explicitly Blocked/CTest-skipped because
the production peer/transport path is not available; no insecure substitute is used.

## Scenario results and independent oracles

The single bounded flow emits twelve scenario records covering all fourteen requested priorities.
Each must be present exactly once and Passed for a passing UI-P606-MAIN receipt.

| Requested scenario | Result and oracle |
|---|---|
| 1. Truthful disconnected launch | Passed: actual Main shows Disconnected, remote ready=false, typed draft cannot enable Send; rendered composer pixels checked |
| 2. Connected readiness | Passed: actual handshake/settings/session projection loads, remote ready/daemonConnected=true, real Send control enabled |
| 3. Enter/send Chat | Passed: real TextArea key input and Enter cause IPC chat.send; no backend-send call in test |
| 4. Streaming conversation | Passed: held provider delta appears in a visible production bubble in the same session, input disabled, rendered composer checked |
| 5. Completed persistence | Passed: separate SQLiteConversationStore reader and actual visible UI equal `p606_delta completed`, Completed status |
| 6. Cancellation | Passed: real Stop button, authoritative cancelled lifecycle and persisted Cancelled message; existing `Response cancelled.` notice visibly rendered; other conversation unchanged |
| 7. Agent transitions | Passed: actual Agent mode control, request through AwaitingApproval, denied Failed run and subsequently accepted Completed run |
| 8. Correct approval binding | Passed: modal's pending approval/run identity agrees with actual active IPC run; next request has a fresh approval ID |
| 9. Deny side effect | Passed: default Deny + Return; original bytes unchanged, independent SQL successful-write count zero, no persistent grant |
| 10. Explicit Allow | Passed: default remains Deny until Tab + Return on Allow once; actual bytes/hash, independent SQL successful-write count one, two tool steps, read-back observation, accepted final and matching durable assistant content |
| 11. Modal focus | Passed: Accessible.Button/name Deny, seven Tab moves stay within modal actions, Escape/outside click cannot dismiss or authorize, focus restores to composer after denial |
| 12. Reconnect | Passed: explicit IPC disconnection clears readiness; reconnect restores the same session, accepted file bytes stay unchanged; no mutation replay |
| 13. Inspector | Passed bounded details: navigate through actual rail, select actual completed run ID, displayed Completed state agrees with authoritative runtime/remote detail; populated Failed/Completed history |
| 14. Destructive delete | Passed: actual sidebar/overflow/Delete menu; Return cannot delete, Cancel preserves, deliberate confirm Space deletes correct conversation/messages in SQLite; separate completed Chat remains unchanged |

The disposable mutation starts with `p606 original bytes\n` and ends with
`p606 authorized bytes\n`, SHA-256
`fa2bbf37c7f215feace35d61be845ca8b691e718f9afc6a015702aa0b9db36fa`.
No permanent grant is created. Tool success alone does not complete the run: the real loop accepts
a Verified read-back final, persisted separately from daemon metadata. Cancellation's partial
content retains an explicit cancelled notice rather than being mislabeled completed.

Inspector verification binds the selected UI run ID to the actual AgentCompleted turn ID. It does
not certify every timeline/evidence/filter panel or independent process-restart recovery. Quick
Panel is composed over the same remote runtime but its separate window interaction is not exercised
by this new flow; existing scoped P6-05 tests remain supporting coverage. Whole-process restart,
concurrent-client/session isolation and QA-02C crash fencing are Blocked here rather than claimed.

## Rendering correction discovered by visual inspection

Initial screenshots exposed a real issue: ShellPanel's unconditional MultiEffect layer made the
entire Dashboard invisible under Qt's software scene graph, while invisible QML controls could
still accept test keyboard events. Those images are not visual success evidence. A narrow
production fallback disables unsupported effect layers only when GraphicsInfo.api is Software
on ShellPanel and the chat surface. Hardware rendering retains existing effect expressions.
No layout, theme, approval policy or interface redesign was introduced.

The test now checks actual dark composer pixels as well as item-tree state; a blank Dashboard
cannot satisfy the flow. The first rendering assertion targeted the disabled TextArea's ink during
streaming; it was corrected to the whole composer, including its visible Stop control. This is a
structural rendering guard, not OCR or complete visual certification. Seven final-stage captures
were inspected: Chat content, cancelled notice, modal, Inspector and destructive dialog visibly
render. Unsupported GPU effects are not certified by software output.

Other iteration findings: first compile lacked the private evidence-root definition; a later
compile needed an explicit QStandardPaths include. Both were fixed. The first delete attempt
correctly failed because compact-layout use had closed the sidebar; the driver now opens the real
sidebar toggle. Final collection additionally exposed a test assumption: live approval event envelopes carry
run_id, while the nested snapshot approval object omits it. The test now cross-checks the actual
wire approval ID/run with active runtime identity, and validates nested run_id only when present;
no protocol/authorization behavior was changed. Failed `run-m1m4kfis` is retained honestly.
One early collector correctly rejected a concurrent dirty-source edit and published
no receipt. A second pre-report collector succeeded; later source/report edits make its receipt
stale. None is silently relabeled as final evidence.

## Accessibility, capture and native boundaries

Fixed Paper theme, reduced motion, installed-font priority, software/basic render loop, DPI96/DPR1,
780x720 compact and 1320x720 wide windows. Application font starts at 14 points; Inspector and delete
also run after a 20-point application font change. Hardcoded QML pixel fonts remain; this does not
establish whole-shell 200% scaling. Actual selected font/DPR/dimensions are recorded, not guessed.

Seven unedited PNG stages: disconnected-compact, streaming-wide, cancelled-wide, approval-deny,
inspector-wide, inspector-compact-large-font, delete-confirmation. Captures are review candidates;
**no screenshot baseline was automatically accepted or updated**. Run/session UUIDs, temporary
paths, greeting time/date and durations legitimately vary. Future exact comparisons require
reviewed per-platform/render/font manifests and masks or structural/semantic oracles. Current
receipt hashes bind actual artifacts, not a claim of cross-run pixel identity.

Deny has an actual Qt accessibility Button role/name. Disabled controls, safe default, modal
containment and restoration are asserted. Visual review also shows long Inspector history summaries
elide/clip within fixed-height rows and compact details require scrolling; complete text reflow and
all Inspector detail fields remain future work. The delete dialog is tested for inert Return and
deliberate activation, not a universal native focus/announcement contract.

| Native acceptance | Result |
|---|---|
| VoiceOver, Orca, NVDA / native accessibility trees | Unverified; Qt role checks are not screen-reader acceptance |
| Fedora KDE portal consent, real activation/focus | Unverified; existing P6-04 portal fixtures are not native consent |
| OS tray/notifications, multi-monitor/DPI/session lifecycle | Unverified |
| Windows daemon transport/native focus | Blocked pending production transport, plus native acceptance |
| Real local/cloud model UX | Unverified; inference is scripted |
| Approved per-platform screenshot baseline | Unverified; explicit review remains required |

## QA evidence and registry

Added UI-P606-MAIN mapping to existing FSV-DESKTOP/CHAT/AGENT/APPROVAL/IPC/SESSIONS. Preserved all
43 feature identities and every feature field/status except appended test IDs, all 65 historical
criteria and 8 component records. No historical acceptance/verification state is promoted.

The collector reuses QA-01/QA-02 evidence schema and existing provenance/hash verification. The
shared Desktop verifier now accepts an explicit binary path, retaining the P6-05 default and tests.
Allowlisted invocation only; no registry-provided commands execute. Missing executable or unsupported
transport is Blocked, nonzero execution/timeout Failed, incomplete/mismatching markers or captures
Unverified. It requires all twelve scenario oracles, independent expected hash/write count/state,
seven actual PNGs, settings and diagnostic/binary manifests. Artifacts and sanitized capped64KiB
logs stay under ignored `build/tests/desktop-daemon-verification/run-*`.

Receipts record exact HEAD/branch/dirty digest, platform/OS/architecture/toolchain, desktop/runtime/
protocol component versions, timestamp, actual outcome, binary and artifact hashes, and limitations.
Final collection follows the saved source report, then --verify runs without source edits. Exact
final digest/hash values remain in that receipt to avoid self-referential report identity. Rebuild
and recollect after maintainer commit for the exact clean candidate; this dirty-tree evidence does
not certify a future clean SHA.

## Validation

- Full C++ tests-preset build and final complete-target rebuild Passed in the existing tree with
  CCACHE_DISABLE=1; zero compiler errors/warnings in final build. Earlier compile mistakes above
  remain documented, not final errors.
- Focused GUI/IPC/accessibility suite: **15/15 executables Passed**, **59.39 seconds**. Later
  rendering/cancelled-notice assertions also run in the final complete suite and collector.
- QML lint: **72 production files, 0 errors / 113 warnings**, exit0, existing warning baseline.
- New collector unit tests **4 passed**; retained P6-05 collector tests **5 passed**. IPC generators
  and QA schema/registry validation Passed. New negative tests cover incomplete/duplicate/failed
  scenario rows, false mutation/state/hash, absent rendering oracle, bad capture metadata/header.
- Rust fmt, locked/offline workspace/all-target Clippy -D warnings and workspace tests Passed:
  **142 passed, 0 failed/ignored**.
- Pinned clang-format18.1.8: new headers and modified probe hunks; Black/Ruff Passed.
- Scoped clang-tidy: new test headers **0 diagnostics** after fixing brace findings. Extended
  existing probe **61 diagnostics**: 59 braces, 1 exception-escape, 1 std-exception-baseclass;
  **0 compiler errors**, 660 non-user warnings suppressed by analyzer. No suppression added. A final compiler-metadata MSVC guard changes only the test log branch;
  complete build and the final Main collector validate the host branch, with Windows still Blocked.
- The initial full run also passed 166/166 in 273.04 seconds. The final run after wire-oracle
  correction is recorded separately; no failing receipt was promoted.
- Final sequential CTest: **166/166 executables Passed**, **190.00 seconds**, explicit -j1,
  short isolated profile and finite deadlines. `p606-full-final.log` / `p606-full-final-detail.log` retain raw
  output. Across 144 QtTest summaries: **2973 passed, 0 failed, 10 skipped**; the standalone Main
  probe additionally emits **12 Passed scenario records**, with zero P606 failures/ReferenceError/
  TypeError diagnostics. **2 existing XFAILs and 81 QWARN lines** remain visible.
- The ten skips remain Linux credential helper, refused native credential persistence, unavailable
  typed native incremental tool assembly, four strict macOS shell child-process fixtures, opt-in
  live classification, missing-node branch unavailable because npx exists, and absent model-residency
  lease/removal contract. XFAILs remain P3-02 D4 provider-switch owned-child cleanup and D5b
  endpoint-switch owned-child reconciliation. None is promoted to verified acceptance.
- Personal-path scan: **0 files flagged**. Scoped credential/private-key signature scan:
  **0 findings across all 14 changed files**. `git diff --check` Passed; HEAD/branch unchanged.
- Host/toolchain: macOS27.0.1 ARM64, Qt6.12.0, AppleClang21, CMake4.4.4, clang-tidy23.1.3,
  pinned clang-format18.1.8, Rust/Cargo1.98.1. Capture manifest records selected Arial font,
  DPR1 and actual window dimensions.
- Pre-report seven-stage visual inspection used ignored `run-47lq3cr_`; its source identity is
  stale after report edits and it is not the final candidate receipt. Final collection runs
  after this report is saved and --verify checks the newly printed directory without source edits.
  No approved image baseline or native-platform acceptance is inferred.

## Next P6 milestone

P6-07 should execute the native Fedora KDE/Wayland matrix with recorded Qt/Plasma/portal versions,
real shortcut consent/activation/focus and screen reader evidence; keep macOS/Windows separate.
Extend Main interactions to Quick Panel, keyboard-only Inspector filtering/timeline/evidence/detail
navigation and readable variable-height history/reflow. Review per-platform captures explicitly;
exercise whole-shell text scaling, multi-monitor/DPI, tray/notifications and native dialogs.
Pair restart/session-isolation testing with QA-02C's test-owned daemon process seam. Do not change
PD-D10/model/process ownership or treat fixture UI results as acceptance of AC-P6-06/07/08.

## Exact changed files

- `tests/CMakeLists.txt`
- `tests/helpers/settings_ui_certification_probe.cpp`
- `tests/helpers/DesktopDaemonFixture.h`
- `tests/helpers/DesktopDaemonInteraction.h`
- `tests/verification/test_desktop_daemon.py`
- `tools/verification/desktop_daemon.py`
- `tools/verification/desktop_ui.py`
- `ui/qml/Main.qml`
- `ui/qml/components/chat/HomeChatSurface.qml`
- `ui/qml/components/navigation/ShellPanel.qml`
- `ui/qml/pages/AgentInspectorPage.qml`
- `verification/registry.json`
- `docs/verification/DESKTOP_UI.md`
- `docs/audits/P6_06_DESKTOP_DAEMON_E2E_RESULT.md`
