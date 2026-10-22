# Release process

Releases are triggered by a Git tag matching `v*`. `-rc` and `-pre` tags are marked prerelease by the workflow. The workflow configures and builds `package-ready`, creates platform artifacts, generates SHA-256 checksum files, and creates a draft GitHub Release for maintainer review.

macOS signing/notarization runs only when the required Apple secrets are configured; otherwise the build is signed ad hoc. Windows signing runs only when the Windows certificate secrets are configured. Release credentials must remain GitHub secrets and never enter this repository.

Draft creation is not approval to publish. A maintainer must resolve licensing, Corresponding Source, artwork provenance, platform acceptance and signing policy before manually publishing. TUI V4-B remains conditional local evaluation only; the Desktop workflow does not build or clear the Rust package. No administrator settings are changed by repository files.

Each platform retains a unique `SHA256SUMS-<platform>.txt`. Collection verifies each checksum, refuses duplicate filenames and produces a combined `SHA256SUMS.txt`. Checksums detect corruption; they do not authenticate a compromised publisher. Verify signing/notarization separately.

Before tagging, run the relevant build/tests, `git diff --check`, and `packaging/release-qa.sh` where supported. Confirm release notes, artifact names, checksums, signing status, and installer smoke tests. This process does not make Flatpak, Snap, Homebrew, or Winget publication automatic.

## Homebrew and Winget manifests

The checked-in Cask and Winget files are `.in` templates, not installable release
manifests. After collecting and checking the final platform artifacts, the draft
workflow renders `sentinel.rb` and `Sopwit.Sentinel.yaml`, validates them against the
exact DMG/EXE/MSI bytes, and adds their hashes to the combined `SHA256SUMS.txt`.
Rendering happens after signing/notarization because those operations change hashes.
The tag must equal `v` plus `SENTINEL_APP_VERSION` from `CMakeLists.txt`; mismatches
stop draft creation. Neither template contains an unchecked or placeholder checksum
in the generated output.

For local verification, run `tools/packaging/render_release_manifests.py` with
`--tag`, `--dmg`, `--exe`, `--msi` and `--output-dir`. Then run
`tools/packaging/validate_release_manifests.py` with the same artifact/tag arguments
plus `--cask` and `--winget` pointing to the generated files. Both tools default
`--version` to the canonical CMake version and use only the Python standard library.
The validator requires exact template output, including the pairing of each installer
URL with its own checksum; hand-edited equivalent manifests are intentionally refused.
Run `python3 tests/packaging/test_packaging.py` for regression checks. These generated
files are draft attachments for maintainer review; they do not publish a Homebrew tap
or submit a Winget package, and do not bypass the publication gates below.

## Required publication gates

Public binary distribution remains **BLOCKED** by [SEC-P1.2](../development/SEC_P1_2_DEPENDENCY_SECURITY_REPORT.md) and [SEC-P1.3](../development/SEC_P1_3_DISTRIBUTION_COMPLIANCE_REPORT.md). CI success, checksums, a signed package or a release draft do not close these gates. Record evidence for the exact candidate and applicable platform:

- [ ] Dependency advisories resolved through compatibility-reviewed changes; no undocumented waiver.
- [ ] Original notices, Qt licensing, Corresponding Source delivery and artwork/sound redistribution permissions verified.
- [ ] Secret/privacy review complete for the distributed source, binary, metadata and evidence; historical exposure decisions documented.
- [ ] Native build/runtime, terminal and signal acceptance complete where applicable; binary and resource versions/hashes match.
- [ ] Package, dependency, notice and matching source inventories and checksums inspected; source download mechanism tested.
- [ ] Signing/notarization verified for the target, or an explicitly documented distribution policy approved by the maintainer; ad hoc signing is not Developer ID clearance.
- [ ] Maintainer separately approves publication after reviewing all evidence and remaining blockers.

Administrator protections are assessed in [SEC-P1.4](../development/SEC_P1_4_GITHUB_HARDENING_REPORT.md). Restrict release tags and signing credentials through an owner-approved policy; repository files do not establish remote protection. Do not publish private vulnerability details in a release-check issue.
