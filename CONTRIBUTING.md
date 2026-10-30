# Contributing

Sentinel is a C++20, Qt 6, QML, CMake, and SQLite agent platform. Start with the repository [agent contract](AGENTS.md); read the [architecture](docs/development/ARCHITECTURE.md), [agent runtime](docs/concepts/AGENT_RUNTIME.md), and [security model](docs/security/SECURITY_MODEL.md) before architecture-sensitive changes.

## Development Rules

Use the [engineering standard](docs/development/ENGINEERING_STANDARD.md) and the affected row of the [P0–P12 module quality matrix](docs/roadmaps/MODULE_QUALITY_MATRIX.md). Substantive changes should describe concrete operating conditions, failure consequences, evidence and acceptance oracles, including relevant positive/negative/boundary/regression cases. Keep implemented work separate from proposed follow-ups. Trivial documentation/comment changes use the lightweight path; unrelated project defects do not block safe scoped work.

- Preserve MVVM boundaries: QML presents state and calls view-model actions; C++ owns logic.
- Keep provider behavior behind `IChatProvider`.
- Keep memory, settings, and chat history persistence separate.
- Keep platform-specific behavior behind service interfaces.
- Do not add telemetry, hidden cloud calls, silent update checks, autonomous behavior, or new
  runtime authority without an explicit phase.

## Local Validation

```bash
cmake --preset tests
cmake --build --preset tests
ctest --preset tests --output-on-failure
```

Before release-oriented changes, also run:

```bash
git diff --check
cmake --preset release
cmake --build --preset release --target sentinel-desktop
```

Optional checks:

- `python3 tools/analysis/qml_lint.py path/to/changed.qml` (also used by pre-commit)
- `clang-format --dry-run --Werror`
- `clang-tidy` after a project profile exists

The QML helper requires `qmllint` on PATH and generated module metadata from
`cmake --preset tests` and `cmake --build --preset tests --target sentinel-desktop`.
Set `SENTINEL_QML_BUILD_DIR` (or pass `--build-dir`) for another build directory.
It adds the source/generated import paths and fails on warnings in the selected
files; a missing build is reported before invoking the linter.

## Pull Requests

- Keep changes scoped.
- Follow the repository issue and pull-request templates.
- Update docs when architecture, privacy, security, packaging, or phase boundaries change.
- Do not include signing credentials, API keys, secrets, update feed credentials, or generated
  packages.

## Public repository hygiene

Use synthetic credentials, transcripts and fixtures. Inspect screenshots and diagnostics for personal paths and user content before sharing. Ignore rules do not remove already tracked artifacts. Keep Cargo.lock for the application workspace and retain original third-party notices with redistributed assets.

For workflow changes, run `actionlint` when available. Keep actions pinned to full commit IDs, checkout credentials disabled, PR jobs read-only, and privileged metadata jobs free of PR checkout/execution. Rust checks are `cargo +1.94.0 fmt --all --check`, `cargo +1.94.0 clippy --locked --all-targets -- -D warnings`, and `cargo +1.94.0 test --locked` from `cli/`. Security disclosures follow [SECURITY.md](SECURITY.md), not public issue forms.
