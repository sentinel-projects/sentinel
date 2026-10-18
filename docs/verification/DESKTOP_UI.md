# Desktop UI verification foundation

Use the existing `tests` build. Tests load production QML/resources, with disposable stores or
explicit synthetic presentation snapshots. They do not start a daemon/model, read user credentials,
request portal consent or deliver native notifications. QML remains presentation-only.

```sh
CCACHE_DISABLE=1 cmake --build --preset tests
ctest --preset tests -j 1 --output-on-failure -R 'test_(desktop_shell_launch|gui_components|desktop_accessibility|chat_surface|models_surface|settings_surface|tray_surface|onboarding_focus|navigation_settings|native_integration|portal_shortcuts|verification_desktop_ui)$'
python3 tools/verification/desktop_ui.py
python3 tools/verification/desktop_ui.py --verify build/tests/ui-verification/run-EXAMPLE
```

Local socket/process fixtures require an execution environment that permits them. Use a short
isolated profile path on macOS to stay below Unix socket path limits. Full validation uses sequential
CTest; per-test deadlines remain finite. The collector runs only the fixed DesktopInteraction QML
fixture, with a stripped environment, disposable short profile and a 60-second owned-child deadline.
Python kills/reaps that child on timeout. Its selected component fixture spawns no child processes.

## Implemented interaction inventory

Implementation status below is source scope, not acceptance. Source roots: `ui/qml/Main.qml`,
`components/`, `pages/`, `onboarding/`, and `apps/sentinel-desktop` public view models/controllers.

| Surface | Actual interactions / implementation status | Current automated oracle and remaining gap |
|---|---|---|
| Main window/navigation | Implemented: Dashboard, Models, Inspector, Tools, Extensions, Settings; shell keyboard shortcuts | Real Main launch, Space navigation, Ctrl+1/2 at 780/1320 widths; native focus/minimize/restore acceptance open |
| Dashboard | Implemented shared HomeChatSurface, greeting, composer, conversations | Existing Chat surface/empty-history/layout probes; live model greeting/response and whole-shell visual baseline not certified |
| Chat | Implemented input/send/attachment/voice controls, draft/error projections, conversation switching | Existing public-VM send/refusal and input probes plus rendered streaming/cancelled/error chips; no new full UI → daemon → provider send/cancel certification |
| Agent | Partial acceptance: runtime state, tool observations, approval/cancel controls implemented | Rendered Denied projection and modal safety; actual daemon approval → Main binding/receipt still a required follow-up |
| Models | Implemented catalog filtering/grouping/paging/selection, truthful empty/loading/error/readiness | Existing models probe and Main Models route; real downloads/model/provider availability acceptance separate |
| Provider configuration | Implemented local/cloud settings, credentials and capability overrides | Existing settings/provider/endpoint contracts; production keys/paid calls deliberately excluded |
| Settings | Implemented categories/search, preferences, appearance/density, backup/profile controls | Existing persistence/invalid-configuration/reduced-motion/contrast probes; native dialogs and complete invalid-input keyboard coverage open |
| Inspector | Implemented run filters/history/detail/evidence/error projection | Real Inspector navigation and empty disconnected rows via public client; populated history/filter/detail UI path remains a gap |
| Quick Panel | Implemented shared daemon projection, Chat/Agent mode, readiness, cancel/approval, details/Escape | Existing tray surface and client tests; actual OS global activation/focus separate |
| Tray | Platform adapter implemented; availability depends on host tray | Existing adapter enable/disable/activation fixture and tray surface; native presence/absence, menu lifecycle and delivery open |
| Notifications | Implemented in-app history, DND, details/clear/dismiss plus native adapter | Existing phase1 notification keyboard/lifecycle tests; OS banner/consent/history parity remains native work |
| Approval/recovery modals | Implemented safe initial Deny/Restore, Escape policy, focus restoration | Production SentinelOverlayModal tested with approval-style content: Enter/Return/Space deny, Tab required for Allow, focus trap/restore; production Main approval wiring not end-to-end certified |
| Destructive conversation deletion | Implemented confirmation, Cancel/Delete and pending/error behavior in HomeChatSurface | Source-inspected; full keyboard cancel/confirm and focus-restoration regression remains a gap, not inferred from the approval-style fixture |
| Error/recovery | Implemented global banner, reconnect/recovery/draft states | Existing shell-state/recovery probes plus rendered disconnected/error states; real restarted daemon recovery combined with GUI not yet certified |
| Keyboard/accessibility | Implemented six base-control names/roles, keyboard activation and focus indicators | Existing QAccessible checks plus real key/mouse tests; announcements, screen readers, IME and complete whole-shell tab order open |
| KDE/Wayland | Portal backend implemented; confirmation and consent required | Isolated D-Bus portal contract tests only; real KWin/portal/focus/occupied shortcut/restart not accepted |
| Explicitly unavailable | Command palette Search Chats/Universal Search are disabled; headless clipboard and Linux app launch are unsupported | Disabled command cannot activate; no invented search or brokerage capability |

`AgentInspectorPage` and command palette are distinct from the daemon's execution authority. An
empty Inspector is not proof that a disconnected daemon has no historical runs. A connected
transport is not evidence that a provider/model is ready. These distinctions must remain visible.

## Stable scenario and acceptance mappings

| ID | Executed source and oracle | Existing features | Acceptance relationship |
|---|---|---|---|
| UI-P605-CONTROLS | `tst_SentinelButton.qml`: accessible contract, disabled action, exactly one activation per Enter/Return/Space | FSV-DESKTOP | Partial AC-P6-06 keyboard oracle |
| UI-P605-COMMANDS | `tst_CommandPalette.qml`: keyboard filter/settings route, disabled commands, no-match, Escape restore and reduced-motion reopen | FSV-DESKTOP | Partial AC-P6-06 navigation/focus oracle |
| UI-P605-INTERACTION | `tst_DesktopInteraction.qml`: navigation, rendered runtime projections, safe modal decisions/trap/restore, compact/wide/large-text capture preparation | FSV-DESKTOP, FSV-APPROVAL, FSV-CHAT | Partial AC-P6-06; projection fixtures do not satisfy backend authorization or live Chat acceptance |
| UI-P605-SHELL | `settings_ui_certification_probe --shell-smoke`: production Main with disconnected public client, real nav/Inspector/shortcuts | FSV-DESKTOP, FSV-DIAGNOSTICS | Partial AC-P6-06; does not close AC-P6-07 |

Existing Quick Panel/tray/accessibility/settings/portal mappings and all 43 feature IDs/65 historical
criteria remain unchanged. Four mappings are appended; TEST-QML-PALETTE retains its ID and now points to the real keyboard test replacing its removed placeholder. No acceptance/verification state is
promoted. The UI collector uses the QA-01 evidence schema and QA-02A provenance/redaction helpers;
receipts are kept in ignored build output, never hashed into their own source digest. Do not import
a receipt into the historical registry without reviewed component provenance matching the candidate.

## Visual preparation and baseline review

Selected component canvas captures are Paper compact 640x480, Midnight Blue wide 900x480 and
Paper with a 28px action label at 640x480. Host test window is explicitly 1000x720. The selected
installed font is chosen from DejaVu Sans/Arial/Noto Sans/Liberation Sans, recorded in metadata,
and applied before loading QML. Missing all supported fonts fails preparation. DPI is 96, DPR is
forced to 1 and checked against actual image dimensions. The software scene graph/basic render loop,
reduced motion, fixed strings, theme, layout, unavailable backend state and idle stream are fixed.
No date-dependent greeting, live telemetry, cursor blink or active animation is captured.

Capture waits for rendering, checks label geometry and nontransparent pixels, and requires two
same-run captures to match. This is a stability/preparation oracle, **not a historical screenshot
regression gate**. Semantic key/role/state/geometry oracles remain primary across platforms. Images,
metadata, sanitized diagnostics, executable digest and schema-valid receipt go only into a unique
`build/tests/ui-verification/run-*` directory, with SHA-256 hashes and exact source HEAD/dirty digest.
A source edit makes earlier receipts stale. Rebuild and collect again after maintainer commit for
the exact clean candidate; a dirty run cannot certify the future SHA.

There is no baseline-accept/update command. A maintainer must review candidate images on each target
platform/render/font configuration and explicitly approve an artifact manifest. Future CI compares
only compatible approved configurations; no cross-platform pixel-perfect equality is assumed.
Software captures do not certify shader/glass effects, native font rasterization, all status text
truncation, whole-application 200% scaling, native contrast or complete UX. No binary baselines enter
Git in this milestone. Screenshot equality alone cannot close any AC.

## Native acceptance matrix

Native acceptance remains open: cells are **Unverified**, with the explicit Windows IPC blocker below. macOS offscreen execution is a separate fixture cell.
A native receipt must identify exact candidate/source/binary/config, OS/display/Qt/platform versions,
oracles, failures/skips/limitations, and reviewer. Record the actual target environment, not branding.

| Native check | macOS | Fedora KDE Plasma Wayland | Windows |
|---|---|---|---|
| Window/focus, minimize/maximize and return focus | Unverified: native AppKit activation | Unverified: KWin/compositor token/focus | Unverified: HWND activation |
| Permission/shortcut consent and refusal/collision | Unverified: Carbon/native permissions | Unverified: real GlobalShortcuts portal consent/deny/collision | Unverified: RegisterHotKey/collision |
| Global shortcut reveal/dismiss | Unverified | Unverified; fake portal is not native acceptance | Unverified |
| Tray presence/absence, menu shutdown/restart | Unverified | Unverified: Plasma tray/session behavior | Unverified: notification area |
| Notification delivery/DND/consent | Unverified | Unverified: real desktop notification service | Unverified |
| Native dialogs and parenting | Unverified | Unverified; portal parenting remains a known gap | Unverified |
| Screen-reader roles/names/actions/announcements | Unverified: VoiceOver | Unverified: Orca/Qt accessibility | Unverified: NVDA/UIA |
| Display scaling/multi-monitor, large text/IME | Unverified | Unverified: fractional Wayland scale | Unverified: per-monitor DPI |
| System theme/reduced motion/high contrast | Unverified | Unverified | Unverified |
| Session restart, daemon reconnect/no mutation replay | Unverified in complete native GUI flow | Unverified in complete native GUI flow | Blocked until Windows IPC path is implemented, then native verification |

AC-P6-06/07/08 and existing decision gates remain open. Portal tests do not adopt native approval,
and UI denial fixtures do not bypass or replace ToolExecutionGateway permissions.

## Next milestone

P6-06 should connect an isolated production daemon/scripted provider to real Main approval and Chat
widgets, assert render/state parity after cancellation/reconnect, and exercise populated Inspector
filters/details. Then run the native matrix on disposable macOS, Fedora KDE Wayland and Windows
hosts with explicit screen-reader/consent evidence and per-platform reviewed visual manifests.

## P6-06 production Main / daemon fixture

Run `python3 tools/verification/desktop_daemon.py` after a successful existing tests build.
The allowlisted `settings_ui_certification_probe --daemon-e2e` mounts actual Main with public
remote view models, real IPC/daemon/application/runtime/gateway/executor and a private scripted
provider. Keyboard/mouse controls drive Chat streaming/cancel, Deny/Allow, Inspector and permanent
delete; independent SQLite reads, accepted runtime final, file bytes/hash and write counts are
required. UI-P606-MAIN maps these bounded integration oracles without promoting feature status.

The collector requires every scenario, explicit side-effect/state fields and seven capture stages.
Artifacts live under existing ignored `build/tests/desktop-daemon-verification/run-*`; use its
printed directory with `--verify`. Receipts bind exact SHA/dirty digest, binary/component versions,
platform/toolchain and artifact hashes. Missing/partial/wrong outcomes cannot become Passed.
No screenshot is auto-approved. Fixed Paper theme, reduced motion, software rendering, DPI96/DPR1,
selected installed font, compact780/wide1320 x720 and a separate 20-point application-font scene
make layout stages repeatable. Real run/session UUIDs, disposable paths, timestamps and durations
remain variable and are excluded from any future exact pixel baseline; use reviewed masks or
semantic/structural oracles. Raw captures remain unedited. Hardcoded QML pixel fonts mean the
application-font scene does not establish full 200% shell scaling.

Software rendering now skips unsupported MultiEffect layers on ShellPanel and the chat surface;
otherwise Main's Dashboard can be invisible while its QML controls still accept keyboard events.
The integration test requires rendered composer ink in addition to object-tree visibility.
Native GPU shadows/effects, VoiceOver/Orca/NVDA, KDE portal consent, notifications/tray, Windows
focus and process restart remain separate acceptance. AC-P6-06/07/08 stay open.
