# Pending work integration — 2026-10-23

## Scope

Integrated the saved local-agent work, macOS bundled daemon, prerelease package
identity, IPC socket-path guards, pending CI branch and all six dependency
branches. The older legacy CLI exit-code branch is reconciled with the newer
portable implementation already on main; its older tests do not replace the
current portable tests.

The local-agent work adds cancellable local inference without a default wall-clock
deadline, normalized provider completion/capacity metadata, typed planner failures
and bounded read-back/repeated-failure recovery. Integration preserves rejection
of truncated reply text, unambiguous loaded-model identity for llama.cpp metadata,
and the complete-checklist recovery budget. Fixtures now report typed truncation
and assert actual gateway read-back before accepted completion; fabricated claims
remain rejected with VerificationFailed.

Ratatui 0.30, Crossterm 0.29, ratatui-image 11.1, rustix 1.1 and signal-hook 0.4
are resolved together. Mascot code uses typed font/viewport sizes and poll timeouts.
Kitty IDs are read from the owned encoded upload, whose placeholder boundary
replaces the removed cursor-save boundary; cleanup remains per owned image.

## Validation

- Rust workspace tests: 144 passed (17 CLI, 27 IPC, 22 mascot, 78 TUI).
- Rust workspace/all-target Clippy with warnings denied and rustfmt check passed.
- Actionlint, packaging consistency, changed-line C++ formatting, whitespace and
  personal-path checks passed.
- Tests-preset configure and full build passed. The final parallel CTest run
  passed 179/180 targets in 168.92 seconds; the remaining test used an obsolete
  error-text assertion. After replacing it with the typed OutputTruncated
  assertion, the rebuilt observation-policy target passed all 60 cases and
  `ctest --preset tests --rerun-failed --output-on-failure` passed (1/1).
  All 180 targets therefore have passing final validation.

Validation uses synthetic/local fixtures. No live LM Studio acceptance or native
Fedora/Windows execution is claimed by this integration.
