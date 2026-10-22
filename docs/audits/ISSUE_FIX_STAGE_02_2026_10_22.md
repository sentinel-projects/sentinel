# Issue fixes — stage 02

Date: 2026-10-22. Base e51f0e3a; changes are developer helpers, not runtime authority.
Personal-Brain CLI unavailable. User authorized completion, commits, branch pushes
and integration into main. No release publication or unrelated PR integration.

## Implemented

- #83: CMake tracks per-language automatic launcher ownership. ON/OFF and
  ccache/sccache/no-selection transitions update or remove owned assignments.
  Explicit user values, including empty values, win. Unmarked old cache entries
  remain ambiguous and are preserved with an OFF diagnostic; BUILDING documents
  explicit cache cleanup. This checkout's known old automatic assignments were
  explicitly cleared once and re-enrolled. Shell compiler wrappers remain separate.
- #87: install-dev resolves --root, CARGO_INSTALL_ROOT, CARGO_HOME or HOME/.cargo
  and supplies an explicit root to Cargo. Cargo install.root configuration is
  intentionally overridden by this documented helper contract. Assets use the
  same root. Existing directories/valid links are preserved; invalid/broken links,
  link failures and Cargo failures return errors. The installed executable's
  absolute path is used for --version, avoiding an unrelated PATH copy.
- #92: pre-commit uses a standard-library Python QML wrapper with source/generated
  module imports. It checks metadata first, supports an alternate build directory
  and makes warnings fail for selected files. CONTRIBUTING documents Qt PATH and
  build prerequisites. The global .qmllint.ini/CI lint policy was not changed.

## Verification

Seven new helper tests passed, also registered as test_development_helpers:

- An actual C++ fixture compiles with a synthetic CMake launcher, then ON -> OFF
  reconfiguration and a clean rebuild prove the launcher is no longer invoked.
  Switching to sccache, no cache selection, explicit override and explicit empty
  launcher are checked without invoking a real cache service.
- Stub Cargo installs only disposable marker binaries. Tests cover default/home,
  environment, --root PATH, --root=PATH, relative/spaced paths, invalid/duplicate
  roots before install, existing assets, broken links, link failure, Cargo failure
  and an unrelated sentinel on PATH. No real/global installation or user config.
- Missing QML metadata refuses before execution; import arguments/exit propagation
  are checked. Real qmllint accepts SentinelButton and rejects a deliberately
  invalid SentinelButton property. Requires existing Qt/build metadata; that
  integration case explicitly skips when prerequisites are absent.

cmake --preset tests and the complete cmake --build --preset tests succeeded
(with CCACHE_DISABLE=1 for the machine's shell compiler wrappers). Full CTest ran
all 179 targets: 177 passed, two failed in 256.58 seconds. The failures are the
already tracked #28 default macOS temporary socket path issue:

- test_desktop_daemon_e2e: fixture server cannot listen;
- test_desktop_ipc: long-path fixture connection failures.

Only those two were rerun with TMPDIR=/tmp; both passed (39.45 seconds). This is
an environment workaround/evidence separation, not a fix or closure of #28.
Case-level skips/native/live gaps are not promoted to platform acceptance.

ShellCheck, sh -n, Python compilation, pre-commit configuration validation,
git diff --check and the personal-path scan passed (zero flagged files).
Pre-commit's initial validation attempted an inaccessible global cache; validation
was rerun successfully with a disposable PRE_COMMIT_HOME, without fetching hooks.
An existing shell empty-assignment warning was corrected with explicit CDPATH=''.

## Integration boundaries

Manifest work, stage-01 audit/fixes and stage-02 helpers are separated into named
branches/PRs while preserving their existing commit ancestry. The maintainer's
merge request authorizes integrating these changes, not dependency PRs or other
open development branches. Main has no configured branch rules returned by the
GitHub branch-rule API. Remote CI results must be reported separately from these
local results; merging is not release/security/native acceptance.
