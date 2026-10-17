# P6-04 — KDE Plasma / Wayland native activation and global shortcuts

Date: 2026-10-18. Status: implemented; native Fedora KDE acceptance **not verified**.
**AC-P6-07 remains incomplete.** Changes are uncommitted for maintainer review.

## Verified baseline and execution boundary

- Repository identity was checked with `git rev-parse --show-toplevel`; the existing checkout was used.
- Base SHA: `432558c8132a8a5764908aee050b1e3e4ce7a949` on `main`.
- `git fetch origin` succeeded; `git rev-list --left-right --count main...origin/main`
  returned `0 0`. The working tree was clean before branch creation.
- Created `feature/p6-04-kde-wayland-integration` from `main`.
- P6-02 commit `a2a81b2e` and P6-03 audit commit `6a0aafe2` are ancestors of this base.
  P6-01, P6-02 and P6-03 reports and the architecture/runtime/security/build/test instructions
  were inspected. Personal-Brain's `brain` executable was unavailable on PATH.
- Disk check: approximately 88 GiB available. Host: macOS ARM64, Qt 6.12.0.
- Reused `build/tests`. No clone, worktree, additional build tree, commit, push, merge,
  reset, stash, clean, rebase, PR or workflow dispatch was performed. One coding agent modified
  this checkout. Temporary logs and the analysis-only compilation database were placed outside
  the repository; the latter is not a second build tree.

## Platform seam and selected APIs

`NativeCompanionAdapter` remains the owner of tray/Quick Panel/window activation. Bootstrap
still creates one adapter, one QuickPanelController and the existing QML windows. The controller
continues to share DesktopRuntimeClient; daemon reconnect does not own shortcut registration.
`GlobalShortcutBackend` supplies an injectable QObject contract for lifecycle, status and
activation. Linux production composes `PortalShortcutBackend`; macOS Carbon and Windows
RegisterHotKey paths remain in the existing adapter.

The backend uses Qt DBus and the session-bus service `org.freedesktop.portal.Desktop`, object
`/org/freedesktop/portal/desktop`. It probes the GlobalShortcuts `version` property and uses
version-1-compatible CreateSession/BindShortcuts calls, Request.Response/Close, Session.Closed/Close,
Activated and ShortcutsChanged. Wire types include `a(sa{sv})`, object-path arguments and the
historical **string** session_handle result. Random handle tokens permit subscribing before calls.
Unexpected handles fail closed; compatibility with legacy non-token request paths is not claimed.
The portal handles consent and assignment; only a returned Quick Panel binding with a nonempty
trigger description becomes registered. Empty results cannot distinguish conflict from refusal.
The actual assigned trigger is displayed, which may differ from the saved preference.

Primary references checked:

- [GlobalShortcuts contract](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html)
- [Request lifecycle](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Request.html)
- [Session lifecycle](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Session.html)
- [Shortcut trigger syntax](https://specifications.freedesktop.org/shortcuts/latest/)
- [KDE portal implementation](https://invent.kde.org/plasma/xdg-desktop-portal-kde/-/blob/master/src/globalshortcuts.cpp)
- [KGlobalAccel API](https://api.kde.org/kglobalaccel.html)
- [Qt Wayland activation, Qt 6.5](https://github.com/qt/qtwayland/blob/6.5/src/plugins/shellintegration/xdg-shell/qwaylandxdgshell.cpp)
- [Qt Wayland activation, Qt 6.12](https://github.com/qt/qtbase/blob/6.12/src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgshell.cpp)

Direct KGlobalAccel was considered: the KDE portal already delegates to it. No direct adapter,
shortcut stealing API, KF6 dependency, X11 key grab or Qt-private header was added. The inspected
base had **no Linux X11 global-hotkey fallback** to preserve. The new portal backend also accepts
Qt's `xcb` platform; absence of portal support remains unavailable rather than simulated success.

## Lifecycle and activation behavior

- Disabled preferences create no portal session. The two existing supported preferences map to
  `CTRL+ALT+space` and `CTRL+ALT+s`; other sequences report unsupported.
- Equal repeated requests are idempotent, including after refusal. Changing/disabling a shortcut
  closes the prior request/session and invalidates pending callbacks before new registration.
- Consent is asynchronous, with a 120-second response deadline. Cancellation, failed calls,
  missing support, empty bindings and timeouts remain unsuccessful. No automatic refusal loop.
- Portal owner loss invalidates registration. A new owner reprobes and registers the still-enabled
  preference through portal consent. Session closure and binding revocation stop activation;
  disabling/re-enabling the preference provides an explicit retry.
- Only the current session's confirmed `quick-panel` action is accepted. The adapter independently
  checks registration and companion-enabled state. It toggles the existing Quick Panel, falling
  back to the existing main window when no panel exists. No windows are instantiated here.
- The portal activation token is scoped to the synchronous show/requestActivate operation through
  Qt Wayland's source-verified `XDG_ACTIVATION_TOKEN` consumption path. The prior environment value
  is restored immediately; tokens are never logged or persisted. This is an implementation
  compatibility point requiring native testing, not a new public Qt activation API.
- Wayland absolute placement and raise requests are skipped; the compositor owns placement/focus.
  Main-window activation clears only minimized state and preserves maximized/fullscreen state.
- aboutToQuit and adapter destruction clean up the backend. The desktop does not shut down the
  daemon, replay mutations, submit prompts or request model loading to reveal the interface.

## Capability matrix

| Environment | Backend / result | Evidence |
| --- | --- | --- |
| KDE Plasma Wayland + working GlobalShortcuts portal | Portal consent and confirmed binding required | Implemented; native registration/focus unverified |
| Other Wayland compositor with GlobalShortcuts | Same capability probe, no desktop-name assumption | Fake portal contract only |
| Wayland/X11 without supported portal or session bus | Explicit unavailable / unsupported status | Deterministic tests |
| X11 (`xcb`) with portal | Portal path; no new raw X11 fallback | Deterministic registration test |
| macOS native | Existing Carbon registration retained | Built; offscreen adapter regression only |
| Windows native | Existing RegisterHotKey retained | Source preservation; Windows build/native run not performed |
| offscreen/minimal | Unavailable in production; injected backend allowed in tests | Deterministic adapter tests |

Linux now requires Qt DBus at configure time. Runtime portal support is capability-probed, not
assumed from Fedora or KDE branding. On macOS, DBus contract tests build when Qt DBus is present;
macOS production activation does not link to the portal backend. Tests require `dbus-daemon` and
launch an isolated bus with separate fake-service/client connections, never the real user portal.

## Changed files

| File | Purpose |
| --- | --- |
| `CMakeLists.txt` | Linux Qt DBus dependency; optional macOS module discovery |
| `apps/sentinel-desktop/CMakeLists.txt` | Shortcut contract and portal library composition |
| `apps/sentinel-desktop/bootstrap/ApplicationBootstrapper.cpp` | Stable desktop-file identity |
| `apps/sentinel-desktop/include/sentinel/desktop/GlobalShortcutBackend.h` | Injectable backend interface |
| `apps/sentinel-desktop/include/sentinel/desktop/PortalShortcutBackend.h` | Portal state and wire types |
| `apps/sentinel-desktop/src/PortalShortcutBackend.cpp` | Async portal lifecycle and activation filtering |
| `apps/sentinel-desktop/include/sentinel/desktop/NativeCompanionAdapter.h` | Backend ownership/injection |
| `apps/sentinel-desktop/src/NativeCompanionAdapter.cpp` | Existing-window activation and backend integration |
| `tests/CMakeLists.txt` | Isolated portal test registration |
| `tests/desktop/test_portal_shortcuts.cpp` | Controllable D-Bus contract tests |
| `tests/desktop/test_native_integration.cpp` | Adapter activation/disconnect and existing-window regressions |
| `docs/development/ARCHITECTURE.md` | Capability-aware shortcut architecture |
| `docs/development/BUILDING.md` | Build/runtime/test dependencies |
| `docs/audits/P6_04_KDE_WAYLAND_NATIVE_INTEGRATION_RESULT.md` | This report |

## Validation

| Check | Result |
| --- | --- |
| `cmake --preset tests` | Passed; existing build tree reused |
| `CCACHE_DISABLE=1 cmake --build --preset tests -j 4` | Passed, including desktop and daemon |
| Portal D-Bus contract | 13 deterministic cases passed on an isolated real D-Bus transport with a fake portal |
| Native adapter | 3 cases passed offscreen, including injected shortcut activation while daemon unavailable and existing-window state restoration |
| `QT_QPA_PLATFORM=offscreen ctest --preset tests --output-on-failure -j 4` | **153/153 passed**, 0 failures, 53.81 seconds; includes desktop/QML/accessibility/tray, IPC, P6-02 and P6-03 regression tests |
| QML lint | 0 errors, 113 warnings, matching the P6-03 documented baseline; no QML files changed |
| C++ formatting | clang-format 23.1.3; diff-range formatting clean, new files formatted. CI's 18.1.8 formatter was not available locally |
| Scoped clang-tidy | Portal backend, native adapter and bootstrap analyzed with repository configuration; 0 compiler errors; remaining diagnostics described below |
| Privacy scan | `python3 tools/analysis/check_personal_paths.py`: 0 files flagged |
| Whitespace | `git diff --check`: passed |

Portal tests cover success, refusal, method failure, timeout with late response, binding revocation,
empty binding/collision, unsupported portal, unavailable service, unsupported platform, repeated
and changed registration, owner restart, session closure and pending shutdown. Activation rejects
foreign sessions and stops after shutdown. The adapter injection exercises the real existing-window
toggle path without a daemon/model; existing IPC tests cover transport reconnect/no-replay.

The initial full-build attempt hit the host's ccache directory sandbox restriction; disabling
ccache allowed validation without creating another build tree or changing project cache settings.
Socket tests ran with the required execution permission. The new regression test caught that
`QWindow::show()` resets maximized state; `setVisible(true)` now preserves it when revealing the
existing main window. Offscreen tests emit expected unsupported-raise and plain-QWindow
`focusPrompt` fixture warnings; they are not native focus evidence.

Scoped clang-tidy reports 50 brace-style diagnostics in the adapter/bootstrap scope, one
pre-existing timer-to-double conversion, and two portal diagnostics: the Qt DBus extraction
operator's required const-reference return and an analyzer ownership warning for a pending-call
watcher. The watcher is parented to the backend and uses `deleteLater()` on completion; parent
destruction owns unfinished calls. These diagnostics were reviewed; no suppression was added.
This is not a claim of warning-free whole-project static analysis or Linux compilation.

## Security and preservation boundaries

Only the user's session bus and portal-mediated desktop shortcut permission are used. There is
no privileged service, input capture, key injection, command execution or permission-policy change.
Request/session identities and generation fencing reject obsolete responses. Shortcut conflicts
are left to the portal; Sentinel never steals or force-registers another application's binding.
Portal consent uses an empty parent identifier (unparented dialog), avoiding private Wayland
window-export APIs; dialog parenting is a documented native UX gap.

AgentRuntime, provider/model routing, daemon IPC and mutation replay policy are untouched.
P6-02 persistence/error projection and P6-03 QML controls/approval focus are unchanged; tests
exercise their regression surfaces. No approval action is connected to shortcut activation.

## Fedora KDE acceptance gap and P6-05 recommendation

No Fedora, KWin or native Wayland session was available. Linux conditional composition, packaged
desktop identity, native consent UI, collision resolution, real keypress delivery, activation-token
consumption, focus, lock/unlock, compositor restart, multi-monitor placement and native shutdown
remain unverified. Fake-portal success proves client contract behavior, not compositor acceptance.
No claim is made that AC-P6-07 is complete.

P6-05 should build/package on Fedora KDE and record Qt/Plasma/portal versions, then verify
disabled → consent → confirmed shortcut → existing panel focus → disable → restart behavior.
Include cancellation, occupied keys, portal restart, daemon reconnect without mutations, locked
session, tray absence, hidden/minimized/maximized main window, approval Deny focus, keyboard-only
navigation, empty/loading/error states and narrow/large-DPI displays. Confirm token consumption
and focus under the installed Qt version; evaluate a narrow supported activation/window-export
adapter only if evidence shows the existing Qt path insufficient. Keep AC-P6-07 open until this
native evidence exists, and keep QML lint baseline cleanup separate.
