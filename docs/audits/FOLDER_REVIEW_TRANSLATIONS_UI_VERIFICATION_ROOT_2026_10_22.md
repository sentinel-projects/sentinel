# Folder review: translations, ui, verification and repository-root files

Date: 2026-10-22. Candidate `2cc7a301a991e031ded9c7b8f4a7cd6e21e6e2b1`,
branch `fix/26-release-manifests`. Personal-Brain CLI unavailable. Existing local
instructions and architecture/security/runtime/build/testing guidance apply.

Maintainer authorized discovery and direct GitHub issue recording. No product
fixes, acceptance promotion, commit or push performed. Previous reports preserved.

## Findings

- [#91](https://github.com/sentinel-projects/sentinel/issues/91): hidden UI
  surfaces keep infinite decorative animations running. Actual registered
  Atmosphere, visible=false, changed its glow x during a 180ms Qt Quick Test
  wait (-89.99803263520185 to -89.7714973271449). Onboarding's six loops and
  SplashScreen's loop have the same source-level missing activity guard; those
  two surfaces were not separately dynamically measured. No CPU/battery claim.
- [#92](https://github.com/sentinel-projects/sentinel/issues/92): root pre-commit
  qmllint hook lacks the module import context used in CI. Plain qmllint on
  SentinelButton emits failed-import/unresolved singleton warnings; adding
  `-I ui/qml -I build/tests/apps/sentinel-desktop` is clean. Both exit 0 because
  MaxWarnings=-1: this degrades analysis rather than blocking commits.
- [#50 evidence comment](https://github.com/sentinel-projects/sentinel/issues/50#issuecomment-6336739505):
  refreshed catalog counts and source RTL scope. No duplicate localization issue.

## Review coverage

- translations: all eight catalogs via existing auditor; README and extraction
  target configuration. Each has 1,213 active source keys, zero missing source
  keys. Turkish 644 finished (53.09%), English 598 (49.30%), other six 478 each
  (39.41%). No placeholder mismatches flagged. Four expansion-review candidates
  (German one, Spanish two, French one) are review hints, not proven clipping.
- ui: module registration, Main lifetime/navigation, onboarding/provider/finish
  states and keyboard focus, modal/button/text/combobox accessibility contracts,
  model grouping helpers, selected memory/runtime/settings/chat surfaces and
  decorative animation/timer lifetime. Risk-based source review, not every line
  of every QML file. Existing #48/#49/#50/#51/#66/#70 remain separate open work.
- verification: schema/registry/README semantics and existing collector tests.
  Validator reports valid=true, errors=[], release_certified=false; missing
  external packaging evidence remains explicit. No new concrete defect found
  in these records; structural validity does not establish feature acceptance.
- repository-root tracked files: AGENTS.md; CMakeLists/CMakePresets;
  README/CONTRIBUTING/CHANGELOG/PRIVACY/SECURITY/CODE_OF_CONDUCT/LICENSE and
  THIRD_PARTY_NOTICES; .gitignore/.gitattributes/.editorconfig, formatter,
  clangd/clang-tidy, pre-commit and qmllint configuration. Previously recorded
  packaging/provenance/documentation findings were not duplicated. Ignored local
  .DS_Store and aqtinstall.log are not treated as shipped source defects.
  AGENTS.md/.gitignore yielded no additional confirmed issue in this pass.

## Executed checks and limits

Six scoped CTest targets passed: GUI components, Desktop accessibility,
verification registry, Desktop UI collector, E2E collector and inventory.
Accessibility initially aborted under the restricted sandbox with audio-device
warnings; the same isolated test passed on the escalated rerun. This is recorded
as an environment-sensitive attempt, not erased or treated as native acceptance.

The existing Desktop UI collector also passed its offscreen/software disposable
fixture flow: keyboard navigation, approval focus/denial, stream/error projection
and compact/wide/large-text captures. Inspected all three captures. They exercise
selected production components in a synthetic fixture, not full Main or native
KDE/macOS accessibility, Arabic layout, microphone or live-provider behavior.

Evidence directory: `build/tests/ui-verification/run-llnwakia`. Its receipt binds
the pre-report dirty digest; adding this report changes the candidate digest.
The receipt is historical fixture evidence, not automatically current evidence
for the resulting dirty tree. Existing build binary was reused without a new
full build. Scratch reproduction/body files: /private/tmp/sentinel-hidden-ui-audit.

No complete C++ test suite, native platform installation, live provider or personal
profile was exercised. No issue was closed. git diff --check passed.
