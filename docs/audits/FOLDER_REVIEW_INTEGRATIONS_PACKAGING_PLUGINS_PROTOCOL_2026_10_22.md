# Folder review: integrations, packaging, plugins, protocol and distribution repo

Date: 2026-10-22.
Application candidate: `2cc7a301a991e031ded9c7b8f4a7cd6e21e6e2b1`.
Packaging candidate: `sentinel-projects/sentinel-packaging` commit
`0dc76fff8d43972455427d76a95634e9c5dff3c2`.
Packaging lock upstream: `efc26e27d6a96768ac07eb7c93523b51b4535031`.

Maintainer authorized folder discovery and direct issue recording, including the
linked organizational packaging repository. Personal-Brain CLI unavailable.
Desktop project inventory listed Sentinel but no separate packaging checkout;
reviewed 16 relevant packaging text files at the immutable commit via GitHub API
in a private temporary snapshot. No clone, project creation or task delegation.
Read the existing issue bodies in both repos before posting. No application or
recipe implementation changed; no package/store publication, issue closure,
commit or push. Previous untracked reports preserved.

## Recorded findings

| Repository / issue | Finding | Evidence level |
| --- | --- | --- |
| [Sentinel #84](https://github.com/sentinel-projects/sentinel/issues/84) | macOS helper reports 100/100 readiness with zero tests and skipped bundle | Actual helper execution |
| [Sentinel #85](https://github.com/sentinel-projects/sentinel/issues/85) | Windows uninstall ignores MSI product code and selects broad name matches | Source confirmed; not run on Windows |
| [Sentinel #86](https://github.com/sentinel-projects/sentinel/issues/86) | Windows silent install/sign helpers discard native failure status | Source confirmed; no installer/certificate operations |
| [Packaging #1](https://github.com/sentinel-projects/sentinel-packaging/issues/1) | Cross-repo active recipe ownership/consumer contract is unfinished | Both trees and active generators compared; decision/workflow gap |
| [Packaging #2](https://github.com/sentinel-projects/sentinel-packaging/issues/2) | Failed final source copy leaves truncated canonical archive and blocks retry | Actual Python module with synthetic archive and injected write failure |

Existing issues extended without duplicates:

- [#26 comment](https://github.com/sentinel-projects/sentinel/issues/26#issuecomment-6336421774): main Cask accepts Monterey while CMake minimum is Ventura; separate repo uses Ventura. External Cask's uninstall helper needs native artifact validation before binary rendering is enabled.
- [#27 comment](https://github.com/sentinel-projects/sentinel/issues/27#issuecomment-6336422274): main Flatpak dir source ../.. resolves to packaging/, which has no CMakeLists.txt; helper prefers org.sentinel.Sentinel while install action uses dev.sentinel.Sentinel. External repo has an immutable archive source and canonical dev identity.

## Ownership assessment

The external repo explicitly owns distribution recipes, input locks and local
source-package helpers. Its extraction map intentionally retains main files;
that is a preparation contract, not completed removal or consumer migration.

Main still owns CMake/CPack, executable/bundle/installed metadata, resources,
platform service files, runtime, SDK, tests and application CI. Its active release
workflow renders its own Cask/Winget templates. There is no reference to the
external repo in the inspected main packaging guidance/workflow/helper paths.
Main Cask differs in minimum OS and auto-update policy; Flatpak helper identity
also differs from the external canonical choice. Packaging #1 requests an explicit
owner/consumer/synchronization decision rather than silently deleting main files.

The older application commit in upstream.lock.json is deliberately immutable;
being older than main is not by itself a defect. Binary artifact descriptors are
empty and external binary manifest rendering deliberately refuses missing inputs.
No verified DMG/EXE/MSI, actual source-archive checksum remeasurement, or accepted
native distribution build was inferred from source-recipe tests.

## Coverage

- `integrations`: contains only the documented reserved-directory README. No
  production target was claimed or filed as missing merely for this layout.
- `plugins`: README, SDK interface/ABI and CMake, both sample implementations,
  manifests and staging rules reviewed with relevant host/manifest call sites.
  Build-only Provider sample metadata does not prove provider registration.
  SDK installation and inert config/log hooks already belong to #62; no additional
  proven plugins-specific issue was opened merely to fill a folder quota.
- `protocol`: canonical IPC JSON, Desktop bridge inventory and generated C++/Rust
  consumers traced; generator freshness and existing contract tests executed.
  Additive optional attachment/model-helper fields are declared. No new
  protocol-specific defect was established by this pass; no complete version-
  interoperability or nested-payload fuzzing certification is claimed.
- Main `packaging`: Linux RPM/Flatpak/Snap inputs and helpers, service/path/identity
  references, macOS Cask/bundle/test/uninstall/appcast paths, Windows manifest and
  deployment scripts, release QA entrypoint. Existing #24/#25/#26/#27/#63/#72
  overlap was preserved rather than reopened.
- Separate packaging repo: README/extraction map/upstream lock, recipe renderer,
  source verifier/fetch, all source and binary templates, local build helpers,
  requirements, eight tests and the scoped CI workflow. Pinned upstream tree
  checked via API to avoid mistaking later main-file removals for bugs in an
  intentionally older lock (its D-Bus service source still exists at that lock).

## Validation

- Main packaging regression suite: 12 tests passed.
- `tools/ipc/generate.py --check` and `generate_desktop.py --check`: passed.
- Six existing offscreen CTest targets passed: plugin tool integration, plugin
  hook registry, plugin manager, packaging contract, Desktop generated contract,
  IPC generated contract. Fixture processes/sockets ran outside the restricted
  sandbox; no real plugin or live model was installed.
- External repo: 8 unittest cases passed, `tools/packaging.py validate` passed,
  ShellCheck on its build helpers and actionlint on its CI passed. Pinned Python
  requirements installed only under a temporary dependency directory; initial
  missing-PyYAML environment error was resolved before these runs.
- Actual macOS helper printed `No tests were found!!!`, skipped the missing
  `build/apps/...` bundle, then printed `100/100 READY`. Those messages demonstrate
  its defect, not acceptance. The separate ShellCheck invocation reported SC1007
  on release-qa's empty CDPATH assignment; that warning was not treated as proof
  of broken runtime behavior or a separate defect.
- Source-publication fault probe used a checksum-valid synthetic tar containing
  required files and mocked network input. Final target write injected failure
  after 8 bytes: final file remained at size 8; retry raised FileExistsError.
  It does not demonstrate bypass of later RPM checksum checks.
- Flatpak source-root resolution checked against actual directories; no build,
  install, unsigned remote registration or deletion ran.
- `git diff --check`: passed.

## Remaining verification

No Fedora RPM/Flatpak/Snap build, native Windows installer/uninstaller/signing,
macOS uninstall/keychain operation, live provider plugin, external source download
(approximately 61 MB), or store/COPR publication ran. Windows script findings are
source-confirmed; native exit/result and product-selection regression fixtures
remain part of their acceptance. Physical platform and artifact compatibility
must remain unverified until exercised.

This is a risk-driven folder pass, not proof of complete product correctness.
Further depth includes nested IPC fuzzing, manifest schema/community validators,
native service and upgrade ordering, staged SDK consumption and exhaustive
extension lifecycle concurrency. The organization boundary remains a recorded
issue, not an implicit authorization to move files or change upstream locks.
