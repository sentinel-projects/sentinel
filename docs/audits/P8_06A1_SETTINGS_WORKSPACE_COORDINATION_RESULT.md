# P8-06A.1 — Settings and workspace storage coordination

Status: production Settings/Workspace admission integration implemented; **complete
application/profile-wide quiescence and coordinated backup/restore remain Unsupported/Blocked**.
Independent legacy config writers, Desktop presentation persistence, other storage owners
and accepted asynchronous request lifetimes still prevent a global guarantee.

## Verified preflight

Existing checkout and origin were verified, origin fetched, repository AGENTS,
architecture/runtime/security/build/testing guidance and both P8-06/P8-06A audits
read. Personal-Brain CLI was unavailable; the explicitly provided checkout was used.
The earlier P8-06A uncommitted note is historical: maintainer-authorized commit
`6ed10084a803c0943e5dfade3944a57c0c11bafd` was integrated and pushed to main before
this request. HEAD/main/origin/main matched that commit with a clean tree. P8-06
`83db1a47` is its ancestor. No conflicting coding chat or Git conflict was found.
The previously absent branch `feature/p8-06a1-settings-workspace-coordination` was
created from verified main. Existing build/tests and cli/target are reused.
No clone, worktree, reset, stash, rebase, publication, PR or workflow dispatch was used.
Changes remain uncommitted for review.

## Actual admission authority

P8-06A has one process-local StorageAdmission shared by standard memory/conversation
owners and archive workers. It has writer counting and immutable generations, **no
implemented domain registry or profile-wide authority**. This change reuses that
component rather than inventing a second settings coordinator or claiming a registry.
The builder enrolls AppSettings before ModelService/configuration composition and
reuses an already attached coordinator when explicitly supplied. Default production
scope remains LiveApplication: prepareLocal refuses before state mutation, and
prepareApplication is always Unsupported.

AppSettings owns its ISettingsStore exclusively. Its generation is captured once at
attachment. Reattachment to the same coordinator never refreshes it; attachment to
another coordinator is refused. Client data never chooses the trusted generation.
Normal setters, removals, migrations and bulk publication obtain admission before
calling the existing persistence/encryption owners. Leases survive until those calls
return. No mirror settings cache is introduced.

## Audited owners and write coverage

| Path / authoritative mutation | Result |
| --- | --- |
| AppSettings individual setters | Common admitted persistValue/removeValue; refusal returns before successful configuration signals and network-policy changes |
| Provider/model preferences | Admitted writes; Ollama model key and legacy alias use one batch; ModelService stages selection against synchronous callbacks and restores it on refusal without emitting success |
| Workspace catalog + active/default selection | persistWorkspaceState validates catalog shape and selected identity and publishes all three keys together; archive/deletion repairs invalid/archived active/default references atomically |
| Workspace create/edit/root/archive/delete/duplicate | WorkspaceService remains pure transformation/validation; Desktop and daemon commit via the authoritative AppSettings batch |
| Workspace profiles / presets / overrides | Existing profile validation preserved; admitted single-document publication; action results now propagate refusal/Pending |
| SettingsService / DaemonDesktopSettings | Existing typed accepted/code results reflect backend refusal; raw generated desktop.setting dispatch and legacy model-helper preference publication now check persistence result rather than dispatch alone |
| Legacy portable basic Settings import/rollback | Three basic settings are one admitted batch, with original allowlisted validation and exclusions; rejected atomic import returns commit failure without pretending a rollback is needed |
| MCP sanitizing reads/setters and legacy credential cleanup | Admission held before mutation/migration; blocked reads preserve existing safe values, redact raw MCP secret material, and do not migrate; credentials stay excluded from portable backup |
| Onboarding multi-stage flow | Each existing persistent stage admitted; stops on the first reported rejection instead of masking it with a later write; not a new transaction spanning all onboarding stages |
| Async discovery/config fallback AppSettings instances (AppConfigLocation/settings.json) | Explicitly read-only, including credential migrations: no late transient reader can publish through an unenrolled fallback owner |
| DaemonService standard composition | Same settings object, same controller-owned admission; shutdown leaves that object unable to acquire new writers |
| Legacy C++ chat/model CLI | Standard builder enrollment, process-local only |
| Legacy C++ config CLI | Still a separate unenrolled writer; now reports backend failure instead of unconditional Updated output; **profile-wide coverage remains Partial** |
| Legacy status CLI | Configuration reads; no credential migration getter on its status path |
| DesktopSettingsStore local presentation keys / CoreWarden preferences | Separate presentation file, outside daemon storage scope; no second daemon admission authority |
| Desktop window QSettings / platform startup preferences | Separate local/platform owners, excluded |
| Direct standalone JsonSettingsStore/encryption/AppSettings users without enrollment | Not implicitly covered; constructors remain compatible for independent composition and tests |
| Chat history, Agent run evidence, grants, independent processes | Still uncovered by the global barrier; no semantic or permission changes |

The production daemon settings chain is AppSettings -> DpapiEncryptedSettingsStore ->
JsonSettingsStore. Lower owners remain private to that AppSettings instance; direct
standalone instances must not be counted as enrolled. Transient background fallback
readers previously capable of migrations now explicitly disable persistence.

## Persistence contract and failure behavior

ISettingsStore gains applyChanges(values, removals), with a default **unsupported**
result. It must not emulate atomicity by calling several setters. JsonSettingsStore
publishes a single QSaveFile document, checks the full byte count and restores its
existing authoritative cache on any open/write/commit failure. Its schema/version
checks and existing persistent-error behavior remain in place. InMemorySettingsStore
implements equivalent batch semantics. The encryption wrapper transforms values
through its existing encryption rules and delegates one batch; it does not introduce
a new secret format or cross-secret-store transaction claim. On macOS, decrypt-only
master-key lookup no longer creates a Keychain key when missing; read inspection must
not perform hidden persistence before admission. Encryption retains key creation in its
existing admitted write path.

AppSettings workspace publication commits catalog, active selection and repaired
default in one batch. Observers see all committed keys before the first successful
notification. Existing raw catalog setters use this same path. Individual active/
default-ID setters retain their legacy opaque-ID normalization; actual IPC workspace
selection/root actions still validate against authoritative catalog/workspace rules.
No model tool or natural-language-to-path authority was added.

Errors include StorageBusy, StaleStorageGeneration, StorageUnavailable,
SettingsWrongThread, SettingsReadOnly, InvalidWorkspaceState and
AtomicSettingsUnsupported, carried through existing accepted/code/error envelopes.
Unsupported batch owners fail before any fallback write. No-op setters do not mutate
storage. A rejected write leaves stored and cached settings unchanged and emits no
successful AppSettings mutation signal. Native credential operations remain separate
and are not claimed atomic with JSON cleanup. General per-field reset and multi-stage
onboarding operations remain per-stage, not a new global configuration transaction.

## Lifetime, generations, threads and lock order

Admission uses the existing short coordinator mutex before underlying persistence;
it is not held during encryption, file I/O, signals or callbacks. A scoped Writer
counts the synchronous authoritative write to completion. No polling or new worker
thread was added. Settings belongs to its Qt owner thread. Foreign-thread setters
are refused before reading its mutable cache; a small atomic diagnostic flag records
that rejection. IPC writers are serialized on the daemon's owner thread. Workers
must queue explicit work to that owner and keep the original generation owner; they
cannot directly write the cache or refresh a stale owner.

Late callbacks referencing an old AppSettings remain stale after leaf maintenance
and resume. There is no implicit rebind/retry. Cancellation/timeout semantics are
inherited unchanged: wait for real leases, return bounded refusal and reopen admission
without generation advance when preparation is cancelled. Shutdown interrupts waits
and prevents new writers. No accepted provider or Agent work is cancelled to obtain
storage quiescence, and no request/session/workspace identity is redirected.

## Desktop and canonical IPC

Two finite Settings service actions were added through protocol/ipc-v1.json and the
existing generators: persistWorkspaceState and setProviderModelPreference. They
carry explicit catalog/selection or provider/model identity and no maintenance or
client-generation capability. Daemon busy/authorization boundaries remain in place;
workspace publication also rejects active Chat/attachment/Agent work.

Desktop runtime configuration stays an authoritative daemon projection. Its store
returns Pending after submission and does not cache proposed values as success.
Workspace multi-key updates use one daemon operation; default repair uses the daemon's
current authoritative default rather than trusting a stale replica. Transport replies
carry typed refusal through the existing operationFailed/settings result paths, then
refresh authoritative values. A refused action is not automatically replayed.

A real QML smoke uncovered SettingToggleRow overwriting its own checked binding even
when persistence refused. It now emits an intent and restores the switch's binding
to the owner's value, retaining keyboard focus on Pending/refusal. QML remains a
presentation layer. The existing settings smoke now exercises a real blocked Space
key toggle, unchanged visible value and focus, followed by an explicit successful
retry after resume, along with its existing responsive, keyboard, localization,
contrast and empty/error surface checks.

## Backup/recovery and remaining work

P8-05 scoped archives, P8-06 journal format/security/durability, memory/conversation
leases and permission exclusions are preserved. No journal-to-live-settings replacement,
combined backup or restore has been enabled. The live scope still refuses preparation;
this change does not certify profile-wide Settings quiescence while legacy config,
presentation and other independent owners exist.

P8-06A.2 must enroll/drain accepted asynchronous lifetimes, remaining persistent owners
and independent process writers; define verified owner close/reopen and cache
invalidation, a real coverage registry and maintenance capability lifecycle; and only
then consider enabling broader preparation. General multi-stage configuration flows
need their own atomic operation contract if all-or-nothing publication across those
stages is required. Never substitute LeafStores for the production live scope.

## Focused evidence and validation

All new tests use disposable
profiles, in-memory credential backends or an injected test encryption key. C++ tests
retain the protective outer macOS sandbox denying real Keychain access. Existing six
sandbox/process failures remain separately classified, never converted to passes.

## Changed-file inventory

- `apps/sentinel-cli/commands/ConfigCommand.cpp`
- `apps/sentinel-daemon/service/DaemonDesktopSettings.cpp`
- `apps/sentinel-daemon/service/DaemonIpcServer.cpp`
- `apps/sentinel-daemon/service/DesktopProjection.generated.h`
- `apps/sentinel-desktop/include/sentinel/desktop/DesktopSettingsStore.h`
- `apps/sentinel-desktop/src/DesktopShellViewModel.cpp`
- `core/include/sentinel/core/app/AppSettings.h`
- `core/include/sentinel/core/interfaces/ISettingsStore.h`
- `core/include/sentinel/core/memory/InMemorySettingsStore.h`
- `core/include/sentinel/core/memory/JsonSettingsStore.h`
- `core/include/sentinel/core/platform/DpapiEncryptedSettingsStore.h`
- `core/src/app/AppSettings.cpp`
- `core/src/app/ApplicationController.cpp`
- `core/src/app/ApplicationControllerBuilder.cpp`
- `core/src/app/BackupService.cpp`
- `core/src/app/OnboardingService.cpp`
- `core/src/app/SettingsService.cpp`
- `core/src/memory/InMemorySettingsStore.cpp`
- `core/src/memory/JsonSettingsStore.cpp`
- `core/src/model/ModelService.cpp`
- `core/src/platform/DpapiEncryptedSettingsStore.cpp`
- `docs/audits/P8_06A1_SETTINGS_WORKSPACE_COORDINATION_RESULT.md`
- `docs/development/ARCHITECTURE.md`
- `docs/interfaces/IPC.md`
- `protocol/QtIpcContract.generated.h`
- `protocol/ipc-v1.json`
- `tests/CMakeLists.txt`
- `tests/core/test_settings_storage_coordination.cpp`
- `tests/desktop/test_desktop_ipc.cpp`
- `tests/helpers/settings_ui_certification_probe.cpp`
- `tools/ipc/generate_desktop.py`
- `ui/qml/components/base/SettingToggleRow.qml`

### Focused production-owner evidence

The new `test_settings_storage_coordination` has 13 Qt rows, covering real JSON and
encryption-wrapper publication, cache/disk/signal preservation on refusal, atomic
workspace create/edit/archive/delete/default repair and observer consistency, actual
publication fault rollback, stale callbacks and no generation refresh on attachment,
foreign-thread refusal, cancellation/resume, shutdown, memory-lease compatibility,
model-selection cache rollback, read-only fallback credential migration and unsupported
batch refusal without sequential writes. The pre-existing storage admission suite
remains passing.

`test_desktop_ipc::settingsStorageAdmissionRefusalIsTruthfulOverIpc` exercises real
DaemonIpcServer + DesktopRuntimeClient + DesktopSettingsStore on a private disposable
socket. It verifies typed StorageBusy for raw and atomic workspace settings, Pending
without projected-value changes, no successful mutation signal, no automatic replay
on resume, explicit retry, and authoritative disk/catalog/selection agreement. Its
standalone run passed 3 Qt rows; the full executable retains its separate old MCP
sandbox failure.

The extended real QML settings smoke passed, including the new blocked keyboard/focus
case and explicit retry. The existing suite covers loading/disconnected/empty/error
surfaces separately. Screenshots `build/tests/p806a1-ui-evidence/storage-blocked.png`,
`appearance.png` and `rtl.png` were retained; the blocked Notifications screenshot was
visually inspected (off state, visible focus ring, no geometry corruption). Responsive
settings geometry is checked at 1320, 780 and 560 widths. These are offscreen Qt/IPC
receipts on macOS, not native Fedora KDE or Windows acceptance.

Initial focused failures exposed model-selection notification reentrancy and archived
active/default handling; both were corrected and passed. The new keyboard test also
caught an actual optimistic QML switch state, corrected in SettingToggleRow. They
were not attributed to the pre-existing sandbox baseline.

### Existing sandbox/process failures (still failures)

| Executable | Disposition |
| --- | --- |
| test_mcp_integration | Nested macOS sandbox-exec sandbox_apply refusal |
| test_real_tool_executor_tools | Same process-launch restriction prevents fixture execution |
| test_process_executor | Same nested sandbox launch refusal, exit 71 |
| test_desktop_ipc | Existing real MCP process/disconnect case blocked; new settings IPC case passes |
| test_plugin_tool_integration | Environment-triggered PluginCrashed versus PluginSandboxUnavailable diagnostic mismatch |
| test_plugin_manager | Same existing mixed environment/product diagnostic classification limitation |

No security assertion or process sandbox policy was weakened. The P8-06A narrow
reproductions and current full-run logs preserve the distinction between launch
restrictions and the plugin error-category limitation. SKIP/XFAIL counts are retained.

### Other validation receipts

| Check | Result / evidence |
| --- | --- |
| Existing CMake tests preset | Configure passed; p806a1-configure.log |
| Full C++ build | Passed; p806a1-build-validated.log |
| Final sequential full CTest | 169/175 passed; 6 failed; 212.33 seconds. `build/tests/p806a1-ctest-validated.log`; preserved Qt detail: `build/tests/p806a1-ctest-lasttest-final.log`. |
| Rust fmt / Clippy, warnings denied | Passed offline; p806a1-rust-fmt.log and p806a1-rust-clippy.log |
| Rust workspace | 142 passed in isolated TMPDIR; p806a1-rust-tests-isolated.log |
| Canonical IPC generators | Both --check passed after schema/generator changes |
| QA registry tests / validator | 31 passed; zero validation errors; existing missing roasd.packaging external evidence warning |
| clang-format 18.1.8 | Changed C++ lines and new test clean; p806a1-format-final.log and p806a1-format-new.log |
| qmllint | Changed SettingToggleRow clean with project imports; p806a1-qmllint-final.log |
| Scoped clang-tidy | 14 production units analyzed. Introduced diagnostics corrected; rechecked AppSettings, ConfigCommand, DaemonIpcServer and changed DesktopSettingsStore header are clean on changed lines. See p806a1-tidy-final.log, p806a1-tidy-verified.log and p806a1-tidy-desktop-store-final.log. Header pass excludes clang-analyzer checks; whole-repository cleanliness is not claimed |
| Privacy and credential signatures | Zero flags; p806a1-privacy-final.log and p806a1-credentials.json |
| git diff --check | Passed |

The initial Rust run encountered AddrInUse in the existing PID-named mock socket
fixture (p806a1-rust-tests.log). A fresh disposable TMPDIR, without deleting prior
files or changing assertions, passed the complete 142-test workspace. That failed
initial receipt is retained rather than described as a pass.

Final preserved Qt detail contains 11 SKIP rows, 2 XFAIL rows, 70 QWARN rows and 25 FAIL! rows across the six failed executables listed above. These diagnostic row counts are distinct from the 175 CTest executable results.
