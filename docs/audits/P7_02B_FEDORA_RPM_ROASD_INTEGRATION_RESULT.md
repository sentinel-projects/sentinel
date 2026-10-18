# P7-02B — Fedora RPM integration and Ro-ASD producer readiness

Date: 2026-10-18. Status: **Interrupted by an external branch/base change; not a clean-base
completion certification.** Sentinel-side changes remain uncommitted. Native RPM installation
and Ro-ASD production readiness remain unverified.

## Checkout integrity interruption

The final identity check found that another actor switched this shared checkout from
`feature/p7-02b-roasd-packaging-integration` to `main` and merged P6-04. Reflog records the external
checkout followed by `8085a9ed2e689d44f576d7dced663dd73241c9b7` (merge of P6-04). Main and origin/main
then matched, while the requested P7-02B branch still pointed to the original `432558c8` base.
These operations were not performed by this task.

All P7-02B changes remain present in the working tree. No automatic branch restoration, reset,
stash or merge was attempted. Work stopped on discovery because the requested serialized checkout
and fixed starting-base conditions could no longer be certified. Test results below are real
executed observations but may include the externally integrated P6-04 tree; they must not be
represented as validation of the original clean P7-02B branch. Reconcile the intended branch/base
and serialize checkout access before final validation. One new Python helper still needs its
final formatter pass following its last version-consistency guard change.

## Base and environment

- Existing Sentinel checkout, verified with `git rev-parse --show-toplevel`; one coding agent.
- Clean starting tree. Base/main/origin-main: `432558c8132a8a5764908aee050b1e3e4ce7a949`.
- `git fetch origin` succeeded; ahead/behind `0 0`; created
  `feature/p7-02b-roasd-packaging-integration` from `main`.
- P7-01 audit (`b257dce8`) and P7-02A audit/path correction (`438cf756`) are on main.
  P7-02A production changes were rechecked rather than reapplied.
- Starting checked-out branch was the committed P6-04 branch at `b50e8b93`. It is preserved;
  P6-04 is **not on this task's required main base**. No merge/cherry-pick was performed.
- Host: macOS ARM64; Qt 6.12.0; Rust/cargo 1.98.1; approximately 89 GiB available initially.
  `brain` unavailable on PATH. Architecture, runtime, security, build/test and packaging documents read.
- No existing Fedora environment found: Docker's selected OrbStack endpoint is absent; no image
  was pulled or started. `rpmbuild`, `rpmspec`, `rpmlint` and `systemd-analyze` are unavailable.
  Per Part C, native checks remain unverified; no system packages were installed.
- Reused `build/tests` and the existing Rust target directory. The temporary install prefix and
  source archives are validation artifacts, not additional source checkouts or build trees.
  No clone, worktree, commit, push, merge, reset, stash, clean, rebase, PR or workflow dispatch.

## P7-02A verification and defects resolved

| Area | Verified state / P7-02B result |
| --- | --- |
| Canonical version | `SENTINEL_APP_VERSION=1.0.0-rc.8`; Rust workspace matches |
| Native spec version | `1.0.0~rc.8`, Release `1%{?dist}` retained |
| CPack RPM version | Previously inherited numeric `1.0.0`; now explicitly maps canonical RC version |
| Package-ready preset | Already builds desktop, daemon, C++ CLI and plugin host; preserved |
| Rust packaging | CLI/TUI sources and checks exist; neither binary is in the RPM; no component silently removed |
| AppStream / desktop | Existing inputs retained; validators passed; AppStream has informational developer metadata gaps |
| User service | Existing user-only hardening retained; no automatic enable/start/preset or system service added |
| Service install path | Absolute configured-prefix destination ignored later staging prefixes; changed to relative `lib/systemd/user`, preserving Fedora's non-lib64 unit path |
| Bubblewrap | Existing build requirement and runtime recommendation retained; absence still fails sandbox execution closed |
| Qt SQL | Removed nonexistent `qt6-qtsql[-devel]` requirements from specs/CI/CPack; Fedora supplies SQL/SQLite and DBus via qtbase |
| Wayland runtime | Added GUI dependency and a rich alternative for legacy qtwayland vs Qt 6.11+ qtbase-gui provider |
| File ownership | Added explicit `/etc/sentinel` directory; mark CMake-installed license/doc trees with RPM flags instead of duplicating their installation |
| Build number | Removed the ineffective RPM Release override; CMake still derives numeric RC build 8 from the canonical version |
| P7-01 personal path | Already corrected by P7-02A follow-up; privacy scan reports 0 flagged files |

Fedora dependency facts were checked against the official
[qtbase package](https://packages.fedoraproject.org/pkgs/qt6-qtbase/qt6-qtbase/fedora-44-updates.html),
[development package](https://packages.fedoraproject.org/pkgs/qt6-qtbase/qt6-qtbase-devel/fedora-44.html),
[older qtwayland package](https://packages.fedoraproject.org/pkgs/qt6-qtwayland/qt6-qtwayland/fedora-44.html)
and [Qt 6.11 GUI payload](https://packages.fedoraproject.org/pkgs/qt6-qtbase/qt6-qtbase-gui/fedora-44-updates.html).
Native dependency solving and spec macro expansion were not executed here.

## Source and artifact parity

`create_source_archive.py` resolves a commit, reads its canonical spec and CMake identity, rejects
version mismatch, and streams `git archive` through gzip with fixed metadata. The matching spec
comes from the same commit, avoiding the prior worktree-spec/HEAD-source mismatch. Existing output
is refused rather than deleted. Two independently emitted archives were byte-identical and their
embedded/external specs matched the selected commit. This does not establish reproducible binary RPMs.
Uncommitted changes are intentionally absent from commit archives. Tests resolve HEAD at execution;
because of the external base change, the snapshot cannot be attributed to the initial base here.
No release containing the uncommitted fixes was invented.

The COPR helper now fails for missing required tools, tracked dirty source and existing outputs;
it no longer deletes prior output, swallows an available rpmlint failure or falls back to an unrelated
remote source if SRPM generation produces nothing. Its existing explicit submission option was not run.
The canonical spec remains `packaging/linux/rpm/sentinel-desktop.spec`; the historical duplicate
is retained byte-identical and checked, avoiding an unapproved repository-layout decision.

`check_install_manifest.py` verifies executable completeness, metadata/assets, spec ownership,
escaping symlinks and CMake-manifest parity for a Linux `/usr` + `/etc` staging image. Missing plugin
host, unknown files and manifest discrepancies fail. The spec invokes this before CTest. CI also
checks the extracted binary RPM tree and compares its RPM version/release and daemon version.

| Artifact | Actual host staging | Native RPM evidence |
| --- | --- | --- |
| Desktop | Existing macOS application bundle installed | Not built |
| Daemon | Installed executable; `--version` reports `1.0.0-rc.8` | Not built |
| C++ CLI | Installed executable; usage reports `1.0.0-rc.8` | Not built |
| Plugin host | Installed executable | Not built |
| Notices / assets | Bundle resources/licenses installed | Linux staged fixture verified; real RPM ownership unverified |
| Rust CLI/TUI | Workspace checks/tests pass | No Rust package produced or claimed |

## Fedora CI readiness

The existing Fedora 44 workflow now has a strict SRPM → binary RPM rebuild → payload verification
path. It retains unsigned RPM/SRPM artifacts as build evidence, without publication or signing.
This replaces duplicate configure/build staging with the canonical spec's actual install/check path.
The test job's invalid SQL development package was also removed. Existing advisory rpmlint,
systemd/container and Rust-feasibility steps were not falsely promoted to verified native gates.

Actionlint and shellcheck pass. No hosted workflow was dispatched. Fedora container package names,
RPM dependency solving, `%{_vpath_builddir}` install-manifest location, rpmlint severity, debuginfo
generation and full native `%check` remain to be demonstrated by the prepared CI path. In particular,
user-namespace restrictions must fail honestly rather than trigger an unsandboxed fallback.

## Validation results

| Check | Result |
| --- | --- |
| Packaging consistency | Passed; canonical/Rust/RPM/CPack/duplicate/preset checks |
| Packaging regressions | 5 passed: valid payload/manifest, missing/unowned/escaping payload, directory ownership, metadata and reproducible commit snapshot |
| CMake configure/build | Existing `tests` preset passed; ccache disabled for the host cache sandbox restriction |
| Full CTest | **154/154 passed**, 0 failures, 59.04 s; includes packaging, desktop/QML and IPC regressions |
| CMake install | `cmake --install build/tests --prefix <temporary-stage>` passed on macOS; all four installed application components present |
| Rust | Workspace offline/locked cargo check and fmt check passed; **142 tests passed**, 0 failures, after granting local test sockets |
| Desktop metadata | desktop-file-validate passed |
| AppStream | appstreamcli validation passed; 2 informational messages and 1 pedantic message |
| Workflow / shell | actionlint and shellcheck passed |
| Python lint/format | Ruff lint passed; E402 excluded for test import-path bootstrap. Final format check requires reformatting create_source_archive.py after its last change; interrupted before correction |
| Privacy / whitespace | 0 personal-path flags; `git diff --check` passed |
| Native tools | rpmbuild/rpmspec/rpmlint/systemd-analyze not run; tools absent |

The initial sandboxed Rust test run failed to bind fixture Unix sockets; rerunning with the required
execution permission passed. No application security setting was relaxed. Source snapshot tests
require `.git` and run in checkout CI; their single Git-dependent case is explicitly skipped when
CTest runs from an unpacked SRPM. Linux payload checks still run there.

## Ro-ASD contract revalidation

Current documents were read through GitHub's API, without cloning or modifying external repositories:

| Repository / commit | Confirmed boundary |
| --- | --- |
| ro-app-template `00e70781ce7367f76f6b81930a21b155579b71a3` | App metadata, two build/CI hooks, stable gate, release asset/attestation parity; no producer signing keys |
| Ro-Repo `65409834e8c1b66b954f2d610971f8d407e1e79e` | Producer V2 repository identity and component/package policy; centralized acceptance/signing |
| .github `7ce7e9217031c113094b5d84f2417db2537a4ff2` | Fedora 44, digest-pinned builder, Project-Ro-ASD URL scope and artifact schema |
| Ro-image-compose `38ec42653bf71e8caa40ad180385e0315f84ed32` | Private repo was readable; actual profile, schema and immutable snapshot contract inspected |

Unlike P7-01, compose access was available. `profiles/desktop-standard/profile.json` does not select
Sentinel. Its `ro_package_names` field selects Ro packages; exact NEVRAs and immutable snapshot
evidence govern actual image inputs. `fedora_package_names` is constrained to the baseline dependency,
not an arbitrary application integration list. Profile/preinstallation changes remain Compose-owned.

Four unresolved manifest mismatches are material: Sentinel's upstream namespace is outside the
accepted producer URL pattern; no immutable builder digest has been selected; aarch64 is allowed
by template app metadata but absent from the artifact enum; and the artifact **filename** pattern
excludes `~` despite normal RC RPM filenames containing it. P7-01's release-field observation did
not resolve this filename restriction. No schema, filename or trust workaround was invented.

The [Fedora/Ro-ASD integration guide](../release/FEDORA_RPM_ROASD.md) gives confirmed paths and
explicit proposals only. No speculative `.roasd/app.json`, signing, repo onboarding, release
manifest/attestation, channel configuration or preinstallation rule was installed.

## Security, maintainer decisions and P7-03

No model downloads, API credentials, privileged service, automatic daemon activation or user-data
deletion were introduced. RPM template preservation and removal/process semantics are documented.
The user service's NoNewPrivileges/PrivateDevices settings and tool approval/sandbox boundary remain
unchanged. SELinux enforcing-mode behavior, GPU/device access and user namespace support remain native
acceptance gaps; no permissive-mode policy or blanket allow rule was added.

Maintainers must decide producer repository/contract changes, RC manifest/version mapping, builder
digest, package split and Rust executable naming, architecture/risk/test policy, channel, signing/
attestation identity, daemon ownership and compose snapshot/profile selection. Migration from any
previously mislabelled final-version RPM also needs an explicit policy; no Epoch was invented.

P7-03 should run the prepared Fedora 44 RPM build and installation tests, dependency solve,
rpmlint, file conflicts, clean install, upgrade from the prior package, modified-template retention,
removal with preserved user data, SELinux-enforcing sandbox checks and an unprivileged user session.
Then validate native Ro-ASD/KDE launch/tray/Wayland behavior, actual model-free first launch,
daemon ownership, approved producer provenance, repository acceptance and exact-snapshot compose
selection. Container success alone is not native distribution acceptance. Production readiness
must remain open until those installation and native checks have evidence.

## Exact changed files

- `.github/workflows/ci-fedora.yml`
- `apps/sentinel-desktop/CMakeLists.txt`
- `cmake/SentinelCPack.cmake`
- `packaging/linux/rpm/sentinel-desktop.spec`
- `packaging/linux/fedora-kde/sentinel-desktop.spec`
- `packaging/linux/fedora-kde/build_copr.sh`
- `tools/packaging/check_packaging_consistency.py`
- `tools/packaging/check_install_manifest.py`
- `tools/packaging/create_source_archive.py`
- `tests/CMakeLists.txt`
- `tests/packaging/test_packaging.py`
- `docs/release/PACKAGING.md`
- `docs/release/FEDORA_RPM_ROASD.md`
- `docs/audits/P7_02B_FEDORA_RPM_ROASD_INTEGRATION_RESULT.md`
