# Folder review: .github, apps and assets

Date: 2026-10-22. Candidate: `2cc7a301a991e031ded9c7b8f4a7cd6e21e6e2b1`.
Branch: `fix/26-release-manifests`. Personal-Brain CLI unavailable.

Discovery pass authorized by the maintainer, including direct GitHub issue creation.
Compared new findings with all 53 existing issue entries (titles and bodies) before
posting. No product fixes, issue closures, commit, push or workflow dispatch.
The previous untracked ISSUE_COVERAGE_REVIEW report was preserved.

## Posted findings

| Issue | Folder | Finding | Validation |
| --- | --- | --- | --- |
| [#72](https://github.com/sentinel-projects/sentinel/issues/72) | .github | Release jobs collect CPack output from a different directory | Source trace and disposable CPack output-directory reproduction |
| [#73](https://github.com/sentinel-projects/sentinel/issues/73) | apps | Safe mode advertises defaults/extensions policy it does not enforce | Current source and call-site trace; historical A10 corroboration |
| [#74](https://github.com/sentinel-projects/sentinel/issues/74) | apps | Unrelated IPC query failures clear pending Chat state | Production-class signal injection probe |
| [#75](https://github.com/sentinel-projects/sentinel/issues/75) | apps | Cold-start session deep links are discarded before readiness | Production-class rejection probe and startup call graph |
| [#76](https://github.com/sentinel-projects/sentinel/issues/76) | assets | Frame validators mix current catalog with stale historical assumptions | Both entrypoints executed with report writes redirected to scratch |
| [#77](https://github.com/sentinel-projects/sentinel/issues/77) | .github | Push formatting range excludes earlier commits in a multi-commit push | Source-confirmed Git range mismatch; no CI dispatch |
| [#78](https://github.com/sentinel-projects/sentinel/issues/78) | apps | Windows bootstrap recreates login startup after user opt-out | Two registry writer paths traced; Windows not executed |

Every issue records candidate/source evidence, verification limits and acceptance
criteria. Severity is a triage proposal, not proof of native release acceptance.

## Coverage

This is a risk-driven first pass across the three folders, not a claim of
line-by-line or whole-product certification. File inventory and entrypoint mapping
covered all three directories. Detailed source reads focused on:

- `.github`: every workflow, Dependabot, CODEOWNERS, label configuration and issue
  templates; release command/output agreement, permissions and quality/test scope.
- `apps/sentinel-desktop`: bootstrap, single-instance/deep-link ingress, logging,
  graphics/platform initialization, daemon transport/runtime reply routing,
  model helper, backup/archive helper, Inspector adapter, Core Warden loader,
  window persistence and selected shell settings/update/export/task paths.
- `apps/sentinel-daemon`: startup/composition/shutdown, local-model runtime,
  request validation, history/projection, sessions, approvals, attachment dispatch
  and desktop settings handlers.
- `apps/sentinel-cli`: command dispatch and Chat/model/config/status contracts;
  legacy independent store ownership already tracked by #56, storage errors #59.
- `apps/sentinel-plugin-host`: protocol reader, capability requests, invocation
  lifetime, plugin load/start/stop/unload and exception handling. Existing host
  configuration/log limitations overlap #62; no additional proven defect posted.
- `assets`: current manifest/state contract, frame inventory/decode and both
  frame-validation entrypoints; historical preview/build script assumptions.

Existing package lifecycle, update integrity, attachment-capability and notification
findings were not duplicated (#24–#28, #44, #47, #51, #63). Historical audit claims
were not promoted without checking current code: for example the Inspector now
has a real RemoteAgentInspectorService, so its old missing-adapter claim was not
reopened. Native OS behavior remains separate from source reachability.

## Checks and outcomes

- `actionlint .github/workflows/*.yml`: passed. Syntax lint does not detect the
  CPack artifact-directory or formatting-range behavior defects.
- Existing built tests, offscreen: `test_desktop_shell_state`,
  `test_desktop_generated_contract`, `test_core_warden`: passed.
- `test_legacy_cli_exit_codes` initially failed fixture listeners under the
  restricted sandbox. Repeated outside that restriction, with isolated fixture
  profiles: passed. `test_daemon_ipc` and `test_desktop_ipc` also passed.
  Final scoped result: six distinct CTest targets passed. This was not a fresh
  full build or full CTest acceptance run.
- Independent Pillow check: 10 states, 462 frame paths; all files decode and
  have declared dimensions and RGBA mode. CW03B validator also completed its
  frame/preview comparisons before failing historical inventory checks.
- Original frame validator: fails requesting `previews/cw03a/listening.webp`.
- CW03B validator: fails on an absent historical evaluation tarball from its
  protected inventory. Its later pre-integration mapping assertions were not
  reached; they are recorded as stale source assumptions, not an observed failure.
- All validator report/inventory output was redirected to scratch; original
  artwork, validation receipts and user data were not rewritten.
- Disposable CPack TGZ experiment: same `cpack --config
  build/package-ready/CPackConfig.cmake` invocation generates the archive in its
  working directory, not beside the config file. Native package jobs not run.
- Scratch app probe linked existing Desktop libraries with test-only private
  access and injected a tracked voice.state timeout while submission was pending:
  pending became false; chat error became request-timeout. A valid session link
  against the not-ready runtime returned false. No event-loop connection or
  microphone capture was initiated. Qt emitted audio-device enumeration warnings.
- `git diff --check`: passed.

## Remaining verification and depth

No native Fedora/KDE or Windows session, installed URL-handler cold launch,
existing-daemon safe-mode user journey, signed installer, Blender scene replay,
full keyboard/responsive/accessibility walkthrough, or real provider acceptance
was executed. These cannot be inferred from offscreen class/fixture checks.

For the next pass inside these folders, prioritize interleaved Desktop operations
(settings/voice/session switching while submitting), projection/message pagination
and truncation, Inspector history pagination, native startup/shortcut failure
feedback, and platform package install/start/stop. DesktopShellViewModel and
DaemonIpcServer were inspected by behavior paths, not every method independently.
Remaining folders are outside this pass; the earlier core/runtime discoveries
remain in ISSUE_COVERAGE_REVIEW_2026_10_22.md and were not silently included here.
