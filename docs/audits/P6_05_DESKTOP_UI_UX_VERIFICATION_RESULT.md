# P6-05 — Desktop UI/UX verification foundation

Date: 2026-10-18. Incremental interaction/capture foundation; no UI redesign or native acceptance.

## Verified base and workspace

Existing Sentinel checkout only. Fetched origin and verified clean main == origin/main ==
`6cddd4b38886a40b7167025f367f465dee14ea2b`. P2-M07 is merged at that base; QA-02A, P6-03 accessibility
and P6-04 portal code/audits are present. AGENTS.md, architecture/runtime/security/build/testing
instructions, current production QML/probes and QA registry/criteria were inspected. Personal-Brain
was unavailable on PATH. The supplied repository identity was checked rather than guessing a path.
The recent chat inventory showed this coding chat active and the other checkout chat idle; Git
identity stayed unchanged during work. Created previously absent `test/p6-05-desktop-ui-verification`.

Reused `build/tests` and `cli/target`, with temporary analysis-only compilation metadata. One coding
agent, no additional checkout/build tree, no commit, push, merge, reset, rebase, stash, clean, PR or
workflow dispatch. Changes remain uncommitted for maintainer review.

## Interaction inventory and evidence boundaries

The maintained [Desktop verification guide](../verification/DESKTOP_UI.md) inventories actual Main
navigation, Dashboard/HomeChatSurface, Chat, Agent, model/provider controls, settings, Inspector,
Quick Panel, tray, notifications, approval/recovery/destructive dialogs, errors, keyboard/accessibility
and KDE portal surfaces. It identifies implemented projections, partially accepted flows, disabled
command-palette searches, unsupported headless clipboard/Linux launch, and platform gaps. It also
contains stable scenario/QA mappings, visual review policy and the separate native acceptance matrix.

Before this change the Qt Quick button/palette tests only executed `verify(true)`. Existing C++ UI
probes already mounted substantial production QML and public view models, so they were retained.
Their wrappers are not falsely relabeled as production Main or real daemon execution.

## Automated scenarios implemented

- **UI-P605-SHELL:** added `--shell-smoke` to the existing disposable probe. It loads actual
  production `Main.qml`, with DesktopShellViewModel over a disconnected public DesktopRuntimeClient,
  real QuickPanelController and remote Inspector service/view model. It does not connect/start a
  daemon. Keyboard Space navigates Models/Inspector/Dashboard; Inspector becomes active with empty
  disconnected rows. Ctrl+1/2 route at 780x720 and 1320x720. Disconnected state never becomes ready.
- **UI-P605-CONTROLS:** real SentinelButton name/role/description, exactly one action per
  Return/Enter/Space, disabled action inert. Placeholder assertions removed.
- **UI-P605-COMMANDS:** real palette keyboard filtering, Open Settings signal/category, no-match
  Return inert, unavailable Universal Search cannot activate, Escape restores focus, reduced-motion
  reopen clears the query and accepts typing. No backend/private method is called to simulate clicks.
- **UI-P605-INTERACTION:** production NavigationRail keyboard/selected-state behavior, rendered
  RuntimeStateStrip streaming/cancelled/Denied/error projections, production SentinelOverlayModal
  with synthetic approval-style content. Default Enter/Return/Space denies; Escape cannot dismiss
  the safety prompt; repeated Tab stays in the modal; focus returns on close; explicit Allow requires
  navigation and activation. This is not the actual daemon → Main approval-binding E2E path.
- **Visual preparation:** real components captured at compact/wide widths and an increased action
  text size. Render readiness, geometry/nontransparent pixels and same-run stability are asserted.
- **Evidence negatives:** five Python collector tests reject malformed/incomplete output, absent or
  corrupted/wrong-size images, missing method results, nonzero exit, skip/XFAIL/unexpected pass.
  Source/branch/digest/time/artifact semantics reuse QA-01 validation and QA-02A helpers.

Existing focused probes cover launch/onboarding, settings navigation/persistence/invalid configuration,
Chat input/refusal/empty history/responsive layout, model paging/availability states, notification
lifecycle/DND, Quick Panel state and native-adapter/portal contracts. Those tests include public VM
and signal-interface actions as well as real keyboard events; they are not all mouse-driven GUI E2E.
New streaming/cancellation chips are presentation fixtures, not a new live model certification.

## Minimal production fix and regression evidence

Only production UI change: `NavigationRail` page buttons now set `autoExclusive: true`. The new test
caught that selecting the already-current Dashboard with Space toggled off its checked indicator
without navigating away. The exclusive page-button contract preserves the active indicator and
prevents that keyboard regression. Models/Inspector/Dashboard routes still pass in actual Main.
No layout, permission policy, tool execution, provider or daemon behavior changed.

Initial fixture runs reported 31 passes/2 failures, then 32 passes/1 failure. One was the real checked
indicator regression. The other was fixture readiness: QtTest TestCase is invisible by default, and
its host window dimensions/focus lifecycle must be made explicit. Tests now show their controls,
size the host window, and wait for popup rendering so the queued initial-focus callback finishes
before Tab is sent. The initial assumption about corner coordinates was incorrect: QtTest already
clicks the center by default. No production modal policy was weakened to satisfy these tests.

## Accessibility results

Existing QAccessible role/name/state and base keyboard tests remain green. New tests assert visible
keyboard actions, disabled controls, Escape/no-approval defaults, focus trap/restore and explicit
navigation before Allow. A 28px action label is checked for fit in the compact fixture; actual Main
also runs at its supported 780px minimum and a wide size. These are bounded structural/semantic
checks, not whole-shell 200% text-scaling or full WCAG acceptance.

Dynamic busy/error text is rendered and checked, but live-region/screen-reader announcements are
not certified. VoiceOver/Orca/NVDA, complete shell tab order/IME, native focus rings/contrast and
actual destructive-delete keyboard cancel/confirm/focus remain gaps. Delete confirmation is
source-inspected, not claimed covered by a different approval modal. Native dialogs were not opened.

## Visual design and local evidence

The fixture uses a fixed 1000x720 host window, component canvases 640x480/900x480, DPR 1, DPI 96,
software/basic rendering, reduced motion, fixed backend/data and recorded installed font. Selected
font availability is checked before engine load. Capture waits for rendering; a second same-run
image must match. The controlled backend stream is reset to Idle between scenarios. This caught a
fixture-state leak during manual screenshot review; it was corrected before final collection.

Three PNGs, metadata, sanitized bounded diagnostics, executable hash and a QA-01 schema receipt are
written only beneath ignored `build/tests/ui-verification/run-*`. A candidate capture was manually
viewed; no screenshot baseline was added to Git or automatically approved. A screenshot match does
not certify UX. The software backend cannot certify native shader/glass effects, native fonts,
all status-text truncation, native DPI or display behavior. Approved future comparisons must use
reviewed per-platform/render/font manifests or structural/semantic alternatives.

Collector deadline is 60 seconds, child killed/reaped on timeout; selected QML fixture spawns no
children. Environment is stripped, with disposable profile and no credentials/configuration inherited.
The C++ setup rejects capture directories outside the existing ignored evidence root. Receipts bind
exact HEAD/branch/dirty digest, desktop component identity, timestamp, platform/architecture/OS,
reported Qt/compiler, profile, actual outcome, artifact SHA-256s and limitations. Missing executable
is Blocked; malformed/missing result is Unverified; failed/skipped/XFAIL are not promoted. `--verify`
checks current provenance/freshness/artifact/binary hashes. No receipt is inserted into the registry,
and later source edits stale earlier runs. Rebuild/recollect after maintainer commit for the exact
clean candidate; dirty-tree evidence does not verify the future commit.

## QA and native acceptance

Retained `TEST-QML-PALETTE` and updated its removed placeholder method to the real keyboard/settings
scenario. Added the four stable scenario mappings to existing FSV-DESKTOP, FSV-APPROVAL, FSV-CHAT and
FSV-DIAGNOSTICS where their bounded oracles apply. Primary acceptance relationship is partial
AC-P6-06. All 43 feature identities, all 65 historical criterion records, implementation,
verification and acceptance states are preserved. AC-P6-06/07/08 and existing decisions remain open.

The guide's native matrix keeps macOS, Fedora KDE Plasma Wayland and Windows separate for window
focus, shortcut/permission consent, tray/notifications, native dialogs, accessibility APIs, display
scaling/theme/motion and session restart/reconnect. Every native cell remains Unverified; Windows
native reconnect is Blocked until its IPC path exists. P6-04 fake-portal success is not native KDE
consent/focus evidence. macOS offscreen execution grants no Fedora/Windows acceptance.

## Validation

- Full C++ build: **Passed**, zero compiler errors/warnings in `p605-build.log`, existing tests
  tree with ccache disabled. Final sequential CTest: **162/162 Passed**, 179.74 seconds
  (`p605-full-final.log`, detailed output `p605-full-final-detail.log`). Across 142 QtTest summaries:
  **2916 passed, 0 failed, 10 skipped**; 2 expected failures and 74 QWARN lines were retained.
  The initial full run was 161/162 because the registry still referenced the removed palette
  placeholder. Preserving its stable ID and mapping the real test fixed that validation failure.
- The 10 skips cover Linux credential helper/native credential persistence, unsupported incremental
  native-tool assembly, four strict macOS child-process shell fixtures, opt-in live classification,
  the missing-node branch with npx installed, and absent model-residency leases. The 2 XFAILs remain
  P3-02 D4 provider-switch owned-child cleanup and D5b endpoint-switch owned-child reconciliation.
  None is promoted to acceptance; no live-provider calls were made.
- Focused suite: **14/14 executables passed**, 24.39 seconds. QtTest summaries: portal 15, native
  adapter 18, GUI components 33, shell state 5 passes, all with zero failures/skips; counts include
  hooks/data rows. Standalone Main probe, existing surface/accessibility probes and collector unit
  suite also passed; their assertions are not all represented as QtTest summary counts.
- New GUI suite: **33 QtTest passes**, zero failures/skips (replacing two placeholder tests and
  adding real interaction/data cases). New collector unit suite: **5 passed**.
- QML lint: 72 production QML files, **0 errors / 113 warnings**; three new/replaced interaction
  tests, **0 errors / 4 unqualified-access warnings** for C++ context properties. Both qmllint invocations exited 0; this is not a warning-free lint claim. Source paths/logs are local.
- CI-pinned clang-format **18.1.8**: new C++ setup and modified probe hunk checks Passed. Formatting
  expanded some existing main-function wrapping in the already-modified probe; no unrelated logic.
- Scoped clang-tidy: both C++ test helpers analyzed, **0 compiler errors**. New QuickTest setup:
  0 diagnostics. Existing extended probe: 55 braces, 1 exception-escape, 1 std-exception-baseclass
  warning; 1144 non-user warnings suppressed by the analyzer. No suppression added or production
  C++ policy change.
- Rust fmt, locked/offline all-target workspace Clippy with `-D warnings`, and workspace tests:
  **Passed**, **142 Rust tests passed**, zero failed/ignored. No Rust or IPC schema changed.
- IPC generators, QA validator/registry tests, Black/Ruff, privacy/credentials and `git diff --check`:
  **Passed**: both IPC generators consistent, registry valid, 31 existing QA unit tests passed,
  Black/Ruff clean, personal-path scanner zero flagged and scoped credential-signature scan zero
  findings. Raw diagnostics remain in existing ignored build output.
- Host/toolchain: macOS ARM64, Qt 6.12, AppleClang 21, CMake 4.4.4, clang-tidy 23.1.3,
  pinned clang-format 18.1.8, Rust/Cargo 1.98.1.
- Final capture is collected after this source report is saved, then independently verified with
  `desktop_ui.py --verify`. Its exact dirty digest and hashes live in the ignored receipt rather
  than this source document, avoiding a self-referential digest. No approved pixel baseline or
  native-platform acceptance is claimed.

## Proposed P6-06

Connect real Main widgets to an isolated daemon/scripted provider and persisted disposable sessions;
assert Chat send/stream/cancel and actual Main approval Deny/Allow/cancel outcomes with independent
mutation/receipt oracles. Add populated Inspector filter/detail and destructive-delete confirmation
keyboard tests. Then run the native matrix with explicit screen-reader/portal/tray evidence and
reviewed per-platform capture manifests. Do not turn offscreen success into native acceptance.

## Exact changed files

- `tests/CMakeLists.txt`
- `tests/gui/tst_main.cpp`
- `tests/gui/components/tst_SentinelButton.qml`
- `tests/gui/components/tst_CommandPalette.qml`
- `tests/gui/components/tst_DesktopInteraction.qml`
- `tests/helpers/settings_ui_certification_probe.cpp`
- `ui/qml/components/navigation/NavigationRail.qml`
- `tools/verification/desktop_ui.py`
- `tests/verification/test_desktop_ui.py`
- `verification/registry.json`
- `docs/verification/DESKTOP_UI.md`
- `docs/audits/P6_05_DESKTOP_UI_UX_VERIFICATION_RESULT.md`
