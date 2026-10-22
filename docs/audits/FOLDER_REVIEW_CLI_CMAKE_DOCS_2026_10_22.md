# Folder review: cli, cmake and docs

Date: 2026-10-22. Candidate `2cc7a301a991e031ded9c7b8f4a7cd6e21e6e2b1`;
branch `fix/26-release-manifests`. Personal-Brain CLI unavailable.

Maintainer authorized discovery and direct issue creation. Reviewed the complete
available issue title/body inventory before posting, including the previous
folder-review findings. No product fixes, issue closures, release dispatch,
commit or push. Prior untracked audit reports preserved.

## Findings recorded on GitHub

| Issue | Folder | Finding | Verification |
| --- | --- | --- | --- |
| [#79](https://github.com/sentinel-projects/sentinel/issues/79) | cli | Rejected daemon setting mutation returns CLI exit 0 | Current binary + disposable socket fixture |
| [#80](https://github.com/sentinel-projects/sentinel/issues/80) | cli | Interactive approval loop stops consuming terminal run events | Current binary + disposable PTY/socket fixture |
| [#81](https://github.com/sentinel-projects/sentinel/issues/81) | cmake | Opt-in signing hook fails cross-directory target configuration | Actual module + minimal parent/subdirectory CMake fixture |
| [#82](https://github.com/sentinel-projects/sentinel/issues/82) | cmake | Package signing failure still exits successfully | Actual script + failing dummy signer and fake package |
| [#83](https://github.com/sentinel-projects/sentinel/issues/83) | cmake | Turning compiler caching OFF leaves the module-installed cached launcher | Actual module ON/OFF cache-preservation probe |
| [#67 comment](https://github.com/sentinel-projects/sentinel/issues/67#issuecomment-6336210668) | docs | Production data-path guidance, draft/publication terminology and missing evidence links | Source trace and bounded local-file link scan |

Each new issue includes impact, source evidence, reproduction level and acceptance
criteria. Suggested priorities are proposals; fixture success does not establish
native release or live-provider acceptance.

## Discovery coverage

- `cli`: workspace/crate layout and version source; CLI parser, task input,
  query/mutation output, approval/cancellation flow and command fixtures; IPC
  envelope/schema, correlation, deadlines, event queue and transport behavior;
  TUI worker, input/editor, command registry, transcript rendering cache and
  selected update/export/reconnect paths; mascot asset/capability ownership.
  Existing Windows transport, unsupported-command and attachment gaps were not
  duplicated (#53, #57, #58, #60).
- `cmake`: all eight module files read: CPack, Ccache, CompilerFlags,
  Dependencies, Pch, QmlHelpers, SignPackages and Signing; root include ordering
  and target ownership traced. Existing package-version and release artifact
  directory findings remain #25 and #72. Signing workflow behavior is distinct
  from the CMake opt-in module; the release workflow already checks its own
  signer exit codes.
- `docs`: navigation/current CLI, TUI, installation, configuration, build,
  packaging and data-location guides compared with relevant production code;
  provider development guidance sampled; Markdown local-file references scanned
  across docs. Historical audit evidence was not treated as current acceptance.
  This is not a semantic review of every document, nor validation of every
  Markdown anchor, external URL, image or artifact.

## Executed checks

1. `cargo build --offline --locked -p sentinel-cli`: passed.
2. `cargo test --workspace --offline --locked`: 142 passed, 0 failed, 0 ignored
   (15 CLI command fixtures, 27 IPC, 22 mascot, 78 TUI; no doc-test cases).
   Fixture sockets needed execution outside the restricted sandbox. No live
   provider/daemon or personal profile was used.
3. `cargo fmt --all --check`: passed.
4. `cargo clippy --workspace --all-targets --offline --locked -- -D warnings`:
   passed.
5. CLI rejection fixture, rerun after current binary build: schema-valid
   `desktop.setting` response `accepted:false`; exit 0, stdout exactly
   `{"accepted":false}`, empty stderr.
6. Approval fixture: PTY stdin and private Unix socket; after valid approval and
   subsequent authoritative `run.failed`, process remained at `Allow? [y/N]`
   until the bounded probe terminated its own child. Source confirms the stdin
   subloop contains no daemon read; this is not a five-minute live expiry test.
7. CMake signing configuration probe used the actual module, executable target
   in a child directory and a dummy non-secret identity. Configure failed at
   TARGET POST_BUILD with `TARGET 'sentinel-desktop' was not created in this
   directory.` No real signing occurred. Windows branch shares the same pattern
   but was not run natively.
8. Direct package script test used a fake `.exe` and shell signer exiting 1.
   CMake printed the signing failure warning and returned 0. No certificate,
   installer or executable payload was used.
9. Compiler-cache module probe: harmless fake launcher remained set after
   ON -> OFF. This is the module's cache assignment, not shell PATH wrappers.
10. Local Markdown scan: 819 relative-file references checked after excluding
    fenced blocks/external URLs/anchors; five missing-path hits. Four evidence
    log links were manually confirmed; the fifth is a quoted README image
    reference inside a historical inventory, excluded from the defect count.
11. `git diff --check`: passed.

Scratch fixtures are under `/private/tmp/sentinel-cli-review`,
`/private/tmp/sentinel-cmake-review`, and
`/private/tmp/sentinel-review-cli-cmake-docs`; they do not mutate repository
sources or application state. Those paths are local execution aids, not permanent
published evidence or test-suite additions.

## Documentation evidence added to #67

- DATA_LOCATIONS describes default StandardPathProvider roots but omits the
  production daemon's explicit AppDataLocation profile override. Runtime store
  locations therefore differ from Desktop presentation and legacy CLI roots.
  Source: DaemonService::initialize and StandardPathProvider explicit-profile
  branches. No user data was inspected or deleted.
- PACKAGING says the workflow publishes artifacts although release.yml creates
  an unapproved draft; align current guidance with RELEASE_PROCESS/INSTALLATION.
- Missing historical linked logs: tui-v33c/tests.log, tui-v33c/clippy.log,
  tui-v34/tests.log and tui-v33b/tests.log. Their absence does not itself disprove
  historical test outcomes. Describe unavailable evidence or provide legitimate
  redacted artifacts; do not regenerate fictional historical logs.

## Limits and remaining depth

No signed installer, real certificate, native Windows terminal/pipe, physical
terminal graphics negotiation, real provider run, or native macOS app signing
verification was performed. No full C++ rebuild/CTest was needed for this
source-unchanged discovery pass. TUI rendering/keyboard states have automated
fixture coverage, not a new physical user-flow acceptance in this pass.

Remaining depth includes terminal-control sanitation, concurrent session/approval
flows beyond the demonstrated cases, physical emulator cleanup/signal behavior,
full compiler/sanitizer/LTO matrices, MSI upgrade/native signing verification,
external/anchor documentation links and semantic reconciliation of the remaining
guides. Folder inventory is not a claim that no additional defects remain.
