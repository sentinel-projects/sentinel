# Fedora RPM construction and Ro-ASD producer boundary

Sentinel remains independently buildable on Fedora, macOS and Windows. Fedora RPM construction
uses `packaging/linux/rpm/sentinel-desktop.spec`. The identical `fedora-kde` spec is retained as
a compatibility input and checked for drift; this does not decide a future Ro-ASD packaging repo.

## Source and binary construction

The release identity is `SENTINEL_APP_VERSION` in `CMakeLists.txt`. RPM maps the first `-` to `~`
(`1.0.0-rc.8` → `1.0.0~rc.8`), preserving ordering before the final version. CPack's RPM override
uses the same mapping; Windows/macOS version rules are unchanged. RPM Release remains `1.fc44`,
independent of the application's RC build number. A previously installed incorrectly labelled
final `1.0.0` will not automatically upgrade to this RC: maintainers must decide migration policy.

On a provisioned Fedora 44 builder, from a clean committed checkout:

```bash
python3 tools/packaging/check_packaging_consistency.py
packaging/linux/fedora-kde/build_copr.sh
rpmbuild --rebuild --define "_topdir $PWD/build-copr" \
  --define 'dist .fc44' build-copr/SRPMS/*.src.rpm
```

These are local construction commands, not release or repository publication. Do not pass the
legacy helper's explicit `--submit` option during validation. The helper refuses missing required
tools, tracked uncommitted changes and an existing output directory; it never deletes previous
artifacts. It obtains the source and spec from the same Git commit. Optional rpmlint failures
are no longer swallowed when the tool is present.

`tools/packaging/create_source_archive.py --output <empty-artifact-directory> --ref <commit>`
can prepare the source archive and matching spec without RPM tooling. Gzip excludes wall-clock
time and filename; tar metadata comes from the selected commit. This proves source archive
reproducibility, not bit-for-bit binary RPM reproducibility across toolchains.
Snapshot regression tests run in checkout CI; their Git-dependent case is explicitly skipped in
source RPM builds, which intentionally omit `.git`. Staged-payload tests still run there.

The RPM builds all four C++ binaries: desktop, daemon, legacy CLI and plugin host. Its `%check`
compares the staged Linux tree with the RPM ownership rules and the CMake install manifest,
validates desktop/AppStream metadata, and runs CTest. The Fedora CI binary job builds the SRPM,
rebuilds it into binary RPMs, extracts the main package, checks payload/identity, and retains the
unsigned artifacts as CI evidence. No hosted run was dispatched by P7-02B.

Rust `sentinel` and `sentinel-tui` remain outside this RPM. Their build checks are not evidence of
packaging: executable naming, package split and distribution Rust-toolchain policy remain open.

## Dependencies and installed files

Fedora provides Qt SQL, SQLite's Qt driver and Qt DBus in `qt6-qtbase`; development headers are
in `qt6-qtbase-devel`. There is no separate `qt6-qtsql` requirement. GUI/QML and Multimedia
dependencies remain explicit. The Wayland runtime requirement accepts either `qt6-qtwayland`
or Qt 6.11+ `qt6-qtbase-gui`, where Fedora's newer Qt packaging contains the Wayland platform
plugin. Native package solving must verify the selected provider on the target Fedora snapshot.

The RPM owns its four `/usr/bin` executables, desktop entry, icons, AppStream metadata,
`/usr/share/sentinel` runtime assets, CMake-installed documentation and notices, user service
and `/etc/sentinel` template directory. The directory is owned explicitly; the template is
`%config(noreplace)`. Documentation/license flags mark the CMake-installed paths rather than
creating another RPM-managed copy. Installation never downloads models or installs credentials.
The CMake user-unit destination stays under `lib/systemd/user` (not Fedora's `lib64`) and is
relative to the install prefix, so staging cannot accidentally write into the configured host prefix.

The package ships a **user** service only. It does not enable/start/preset that service or install
a privileged system service. Desktop's existing detached-daemon startup is unchanged; selecting
systemd as the automatic daemon owner requires a maintainer decision. Do not independently enable
both launch policies. Existing daemon socket/lock checks remain the ownership boundary.

Upgrades and removal do not delete home-directory settings, model files, conversation history,
permission grants or credentials. RPM removal removes owned system files; ordinary RPM config-file
preservation rules apply to a locally modified template. Package removal does not promise to stop
an already running detached process. Users should close Desktop and their daemon before removal;
an explicitly managed user service must be stopped through that user's service manager.

Bubblewrap remains a recommended runtime dependency and a build/test dependency. If unavailable,
approved sandboxed process execution fails closed. No SELinux policy, permissive mode, blanket
allow rule or sandbox relaxation is installed. Native enforcing-mode tests must verify daemon
socket access, user namespaces, approved tool execution and any device/GPU access affected by the
service's existing PrivateDevices setting.

This P7-02B branch starts at main without P6-04. Installing Wayland libraries does not add its
portal shortcut implementation or establish AC-P6-07 acceptance. Integrating that separate
feature and verifying KDE consent/focus remain explicit release-candidate checks.

## Confirmed Ro-ASD contract and proposals

Rechecked read-only on 2026-10-18 against:

- [ro-app-template](https://github.com/Project-Ro-ASD/ro-app-template/tree/00e70781ce7367f76f6b81930a21b155579b71a3)
- [Producer Registry V2](https://github.com/Project-Ro-ASD/Ro-Repo/blob/65409834e8c1b66b954f2d610971f8d407e1e79e/docs/PRODUCER-REGISTRY-V2.md)
- [Artifact schema](https://github.com/Project-Ro-ASD/.github/blob/7ce7e9217031c113094b5d84f2417db2537a4ff2/contracts/component-artifact-manifest-v1.schema.json)
- [Compose profile](https://github.com/Project-Ro-ASD/Ro-image-compose/blob/38ec42653bf71e8caa40ad180385e0315f84ed32/profiles/desktop-standard/profile.json)

The template defines `.roasd/app.json`, `tools/project-ci`, `tools/build-rpm` and the stable
`ro-app-gate` check. Its release workflow requires tag/commit, RPM/SRPM and attestation parity.
Ro-Repo owns producer onboarding, acceptance, central production signing and repository promotion.
Read-only access to the private Ro-image-compose repository was available in this task, unlike
P7-01. Its `profiles/desktop-standard/profile.json`, `schemas/desktop-profile-v1.schema.json`
and `docs/SNAPSHOT-CONTRACT.md` confirm compose ownership: `ro_package_names` selects semantic
package names, while actual build inputs bind exact package NEVRAs to an immutable snapshot.
The current profile does not list Sentinel. The separate `fedora_package_names` field is restricted
to the baseline dependency and is not an application onboarding slot. Sentinel must not install
a speculative repo file or compose fragment; profile and snapshot changes belong to Compose/Ro-Repo.

The following is a **proposal**, not installed production metadata:

| Decision | Proposed direction | Required confirmation |
| --- | --- | --- |
| Producer location | Keep upstream Sentinel independent; use a reviewed packaging producer or approved contract extension | Current schema restricts source/workflow URLs to Project-Ro-ASD repositories |
| Component/package | Component `sentinel`, existing package `sentinel-desktop` | Registry package allow-list and package split approval |
| Builder | Fedora 44 with a verified immutable digest | No digest selected or invented here |
| Architecture | Establish x86_64 first | Template advertises aarch64, but artifact schema's enum excludes it |
| RC artifacts | Preserve RPM-correct tilde ordering | Schema filename pattern excludes `~`, so ordinary RC RPM filenames fail the current contract |
| Daemon | Preserve current Desktop startup until ownership is decided | User-service activation policy |
| Release trust | Generate checksums/provenance only in the approved producer workflow | Exact tag/version mapping, attestation identity, SBOM and required-test policy |
| Delivery | Ro-Repo acceptance/signing; compose-owned preinstallation | Channel, risk class and compose policy |

No `.roasd/app.json`, producer registry entry, release attestation, signing key, repo configuration,
or preinstallation rule is emitted while these decisions remain open. A green Fedora container
build would establish RPM construction evidence only; clean installation, upgrade, removal and
native Ro-ASD/KDE acceptance are separate release gates.
