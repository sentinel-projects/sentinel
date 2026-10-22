# Issue fixes — stage 03

Date: 2026-10-22. Base main 596ae034; branch
codex/cli-rejection-and-push-format. Personal-Brain CLI unavailable. Continue the
authorized fix, commit/push and PR integration sequence. No unrelated PRs merged.

## Implemented

- #79: the Rust CLI uses the generated IPC ResponsePayload contract to recognize
  rejected DesktopSetting/DesktopAction acknowledgements before emitting success.
  Rejection returns operation-rejected / exit 1. Text emits stderr only; JSON and
  stream-JSON emit the existing typed error envelope. ID-based mutations and
  queries retain their existing response semantics. No daemon/protocol schema,
  permissions or execution authority changed; no endpoint value is echoed in the
  generic rejection message.
- #77: the Linux quality workflow supplies PR base and push before SHA to a bounded
  Bash formatting helper. PR base takes precedence; the complete pushed range is
  compared even across non-ancestor force pushes. A zero-before event compares
  against the empty tree. Missing shallow objects are fetched by their exact SHA;
  unavailable/invalid bases fail rather than narrowing the checked range.

## Verification

The new CLI mutation regression failed against the old implementation: a rejected
operation exited 0, expected 1. After the fix, real CLI subprocess/Unix-socket
fixtures exercise accepted and rejected endpoint/discovery acknowledgements in
text, JSON and stream-JSON, plus queries and ID-only mutation replies. New sockets
use short disposable /tmp roots; no real daemon/settings/provider was contacted.

Rust 1.94.0, matching the declared project toolchain:

- cargo test --workspace --offline --locked: 144 passed, zero failed/ignored
  (CLI 17, IPC 27, mascot 22, TUI 78; no doc-test cases).
- cargo fmt --all --check: passed.
- cargo clippy --workspace --all-targets --offline --locked -- -D warnings: passed.

Five actual-helper Git fixture tests passed: bad formatting in the first of two
pushed commits; PR-base precedence; initial push; shallow checkout exact-object
fetch; non-ancestor force push; and invalid/unavailable base refusal. The formatter
is a synthetic changed-line marker oracle: this tests Git range selection and
failure propagation, not clang-format's formatting algorithm. All Git commits,
remotes and fetches in these tests are disposable local repositories.

The tests preset configured successfully. Scoped CTest targets
(test_development_helpers and test_ci_format_range) passed, 5.37 seconds; the former
also reruns the seven stage-02 helper cases. Actionlint, ShellCheck, personal-path
scan (zero flags) and git diff --check passed. No full C++ rebuild/suite was repeated
for these CLI/CI helper changes; the stage-02 full-suite #28 limitation remains open.

Remote main CI was still queued/in progress at preflight, so none of these local
results is represented as completed remote Linux/macOS/Windows/Fedora acceptance.
