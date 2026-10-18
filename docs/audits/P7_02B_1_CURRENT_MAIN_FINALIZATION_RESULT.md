# P7-02B.1 — Current-main reconciliation and packaging finalization

Date: 2026-10-18. Status: **locally validated and safe for maintainer review/commit**;
native Fedora/Ro-ASD acceptance remains unverified. Finalization changes are uncommitted.
This report supersedes the interrupted P7-02B report's checkout/validation status, not its historical evidence.

## Verified identities and preservation

| Identity | Actual value |
| --- | --- |
| Starting and retained branch | `feature/p7-02b-roasd-packaging-integration` |
| Starting/current source HEAD | `a647259702f19b5eb0f48956a1ba7462950d677c` |
| Local main, fetched origin/main, merge-base | `8085a9ed2e689d44f576d7dced663dd73241c9b7` |
| Main vs origin/main | `0 0` ahead/behind |
| Branch relation | Four existing P7-02B commits on current main; P6-04 is an ancestor |
| Starting staged/unstaged/untracked/deleted | All empty |
| Host / disk | macOS ARM64, Qt 6.12.0; approximately 88 GiB free |

Historical assumptions were rechecked before editing. The maintainer had already committed the
interrupted files in `4a41fc3a`, `76bbc46f`, `9dd36193`, and `a6472597`. Reflog confirmed the branch
was moved to the updated main before those commits. This task did not perform those commits or
the preceding merge. No uncommitted work from another task was present. The existing branch
already met the intended ancestry, so it was retained without switching, recreating or force-moving it.
Main and the original candidate commits were not modified. No clone, worktree, duplicated source
checkout, additional build directory, reset, clean, stash, rebase, merge, cherry-pick, commit, push
or workflow dispatch was performed. One agent operated on this checkout.

AGENTS.md, the interrupted audit and architecture/runtime/security/build/test documents were read.
Personal-Brain's `brain` was unavailable on PATH; the explicitly supplied checkout was verified.

Recovery backup: `/tmp/sentinel-p702b1-recovery-5y3f37vp/recovery.tar.gz` (about 50 KiB).
SHA-256: `ba5941e3951302fd763390887ef1beb5993798384a2496f9781a2b85498454e6`.
The unique new location does not overwrite an INT-07 archive. It contains all 14 committed candidate
files relative to main, a binary-safe candidate patch, manifest hashes, Git identities/status and
reflog evidence. There were no uncommitted or deleted files to recover. External `manifest.json`
and `candidate.patch` are alongside the archive. Every archived file and patch hash was verified.
Reverse applicability was checked against the candidate tree; forward applicability was checked
against main in a temporary alternate index, without touching the real index or checkout.

## File-by-file reconciliation

| Candidate file | Reconciliation against current main |
| --- | --- |
| `.github/workflows/ci-fedora.yml` | Retain strict RPM construction/payload pipeline; add dbus-daemon to RPM-build and CTest jobs for merged P6-04 |
| `apps/sentinel-desktop/CMakeLists.txt` | Retain only the relative user-unit install destination; current-main portal libraries preserved |
| `cmake/SentinelCPack.cmake` | Retain canonical RC version and Qt corrections; recommend desktop portal without forcing KDE |
| `packaging/linux/rpm/sentinel-desktop.spec` | Retain version/dependencies/ownership/manifest checks; declare isolated D-Bus test dependency and portal recommendation |
| `packaging/linux/fedora-kde/sentinel-desktop.spec` | Same corrections; byte-identical compatibility input retained |
| `packaging/linux/fedora-kde/build_copr.sh` | Retain fail-closed tools/dirty-tree/output checks and same-commit source/spec preparation |
| `tools/packaging/check_packaging_consistency.py` | Retain canonical/Rust/CPack/spec/preset and install-prefix guards |
| `tools/packaging/check_install_manifest.py` | Retain executable/assets/metadata/ownership/symlink/manifest validation |
| `tools/packaging/create_source_archive.py` | Final Ruff wrapping only; committed-version guard, safe identity, refusal to overwrite and deterministic archive checks unchanged |
| `tests/CMakeLists.txt` | Retain packaging test registration alongside current-main portal tests |
| `tests/packaging/test_packaging.py` | Retain five regression cases and explicit Git-only snapshot test boundary |
| `docs/release/PACKAGING.md` | Retain integration guide link |
| `docs/release/FEDORA_RPM_ROASD.md` | Correct historical P6-04 absence; add exact future-commit artifact verification procedure |
| `docs/audits/P7_02B_FEDORA_RPM_ROASD_INTEGRATION_RESULT.md` | Preserve historical interruption report unchanged |

No P6-04 implementation, daemon/provider/Agent logic, approval UI or other current-main source
was imported or rewritten. Version remains `1.0.0-rc.8`, RPM `1.0.0~rc.8-1.fc44`; package-ready
still builds Desktop, daemon, legacy CLI and plugin host. Rust packaging remains an open decision.
Qt DBus comes from Fedora qtbase. The newly declared dbus-daemon dependency closes the test
prerequisite gap introduced by current main's portal tests. Runtime portal support is optional and
capability-probed; its recommendation does not claim confirmed KDE registration or AC-P6-07 acceptance.

## Validation

Validation uses the unchanged HEAD above plus the explicitly listed uncommitted finalization delta.
No mixed checkout or external identity change has been observed in this run.

| Gate | Result |
| --- | --- |
| Packaging consistency | Passed |
| Packaging regression tests | 5/5 passed |
| COPR fail-closed probe | Missing rpmbuild returned exit 1 before creating output; checkout/status and output-directory presence unchanged |
| Configure / build | `cmake --preset tests`, `CCACHE_DISABLE=1 cmake --build --preset tests -j 4` passed in existing build/tests |
| Full sequential CTest | **154/154 passed**, 0 failures, 228.70 s; `QT_QPA_PLATFORM=offscreen ctest --preset tests --output-on-failure -j 1` |
| Desktop/QML/IPC | Included in full CTest; native KDE remains unverified |
| QML lint / host install | 0 errors, existing 113 warnings; repeated macOS staging install passed with all four components and matching daemon/CLI version |
| Rust fmt / Clippy / tests | Installed CI Rust 1.94.0: fmt passed; workspace/all-target Clippy with `--locked --offline -- -D warnings` passed; complete offline/locked tests with `--test-threads=1`: **142 passed**, 0 failures across 8 groups |
| clang-format | Existing pinned 18.1.8 executable used; main-to-candidate diff has no changed C++ lines, formatter diff empty |
| Ruff | Lint passed with E402 excluded for test import bootstrap; all three new helpers/test files pass format check |
| actionlint / shellcheck | Passed |
| Desktop / AppStream | Passed; AppStream retains 2 informational and 1 pedantic metadata messages |
| Generated IPC contracts | Both Python generators passed `--check` |
| Privacy | 0 personal-path findings |
| Credential scan | 15 candidate files scanned for high-confidence private-key/cloud/token patterns; 0 findings. No dedicated secret-scanner executable available; not a comprehensive credential certification |
| Git whitespace | `git diff --check` passed |

The generated AppBuildConfig identifies `a647259702f1` and canonical version `1.0.0-rc.8`.
No system packages or large container images were installed. `rpmbuild`, `rpmspec`, `rpmlint`
and `systemd-analyze` are unavailable here; RPM build/install/upgrade/removal, dependency solving,
SELinux enforcing behavior and native Ro-ASD/KDE results remain unverified. The prepared Fedora
workflow was validated statically, not dispatched. Existing advisory native/container gates were
not promoted to verified acceptance.

## Commit-based archive evidence and release boundary

An archive explicitly created from `a647259702f19b5eb0f48956a1ba7462950d677c` had SHA-256
`e023c48cdc02c0f60df3f5c6bcee6915eefa9007d0c0b514b92774e6f3ef3dc1`.
Its embedded spec and companion spec matched `git show` at that exact commit and differed from
the current uncommitted spec: this proves the new D-Bus dependency changes were **excluded**.
The archive is not a finalization release artifact. Two-archive reproducibility is separately
covered by the regression suite. Binary RPM reproducibility is not claimed.

After review/commit, a clean checkout at the future candidate SHA must create a new source archive
and matching spec, record hashes, run rpmspec/rpmbuild, inspect RPM/SRPM identity and payload,
run rpmlint and install/upgrade/remove in an authorized Fedora environment. The exact commands
are in [the integration guide](../release/FEDORA_RPM_ROASD.md). Do not reuse the pre-finalization
archive or claim it includes these uncommitted changes. No source archive or build is published here.

Open Ro-ASD decisions remain producer identity, immutable builder digest, RC filename/manifest
conventions, aarch64 schema support, signing/attestation, repository onboarding, exact immutable
compose snapshot/preinstallation policy, package split and Rust CLI/TUI inclusion. No schema
workaround, producer metadata, signing, repository or activation policy was invented.

## Review and maintainer handoff

The four existing candidate commits are already in this feature branch. Review them with
`git diff main...HEAD` and review the finalization delta with `git diff`. No staging or commit
has been performed by this task. Before staging, verify branch/HEAD/main identities and ensure
exclusive checkout access. Stage only the finalization files listed below, inspect
`git diff --cached --check` and `git diff --cached`, then use the maintainer's normal commit process.
Do not merge/rebase the branch as part of this handoff; it already descends from the verified main.

Finalization files (relative to starting HEAD):

- `.github/workflows/ci-fedora.yml`
- `cmake/SentinelCPack.cmake`
- `packaging/linux/rpm/sentinel-desktop.spec`
- `packaging/linux/fedora-kde/sentinel-desktop.spec`
- `tools/packaging/create_source_archive.py`
- `docs/release/FEDORA_RPM_ROASD.md`
- `docs/audits/P7_02B_1_CURRENT_MAIN_FINALIZATION_RESULT.md`

Maintainer-only commands, after reviewing both diffs and the final validation results:

```bash
test "$(git branch --show-current)" = feature/p7-02b-roasd-packaging-integration
test "$(git rev-parse HEAD)" = a647259702f19b5eb0f48956a1ba7462950d677c
git add -- .github/workflows/ci-fedora.yml cmake/SentinelCPack.cmake \
  packaging/linux/rpm/sentinel-desktop.spec packaging/linux/fedora-kde/sentinel-desktop.spec \
  tools/packaging/create_source_archive.py docs/release/FEDORA_RPM_ROASD.md \
  docs/audits/P7_02B_1_CURRENT_MAIN_FINALIZATION_RESULT.md
git diff --cached --check
git diff --cached
git commit -m "build(packaging): finalize P7-02B against current main"
```

These commands were not executed by the agent. After the maintainer commit, record the new SHA
and follow the committed-candidate artifact procedure; do not publish the older verification archive.

P7-03 must supply real Fedora RPM construction, clean install/upgrade/removal with retained user
data and modified-template handling, enforcing SELinux/sandbox checks, daemon ownership, native
KDE activation/focus and approved producer/snapshot acceptance evidence. Local validation does
not establish Ro-ASD production readiness.
