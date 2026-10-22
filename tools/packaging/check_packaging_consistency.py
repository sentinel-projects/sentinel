#!/usr/bin/env python3
"""Verify that distribution packaging metadata matches the canonical build.

The canonical release identity is ``SENTINEL_APP_VERSION`` in the top-level
``CMakeLists.txt`` (see the comment there). Platform package metadata must not
drift from it, must not duplicate each other inconsistently, and must only
reference binaries that the ``package-ready`` build preset actually builds.

Checks performed:

1. RPM spec ``Version``/``Release`` agree with the canonical version.
2. AppStream metainfo advertises the canonical version.
3. The Snap manifest advertises the canonical version.
4. Duplicated packaging inputs (RPM specs, AppStream metainfo) stay identical.
5. Every binary shipped by an RPM spec is built by the ``package-ready`` preset.
6. The desktop entry launches a binary the spec ships.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

APP_VERSION_RE = re.compile(r'set\(SENTINEL_APP_VERSION\s+"([^"]+)"')
NUMERIC_PREFIX_RE = re.compile(r"^\d+\.\d+\.\d+")
METAINFO_RELEASE_RE = re.compile(r"<release\s+version=\"([^\"]+)\"")
SPEC_VERSION_RE = re.compile(r"^Version:\s*(\S+)\s*$", re.MULTILINE)
SPEC_RELEASE_RE = re.compile(r"^Release:\s*(\S+)\s*$", re.MULTILINE)
SNAP_VERSION_RE = re.compile(r"^version:\s*'?([^'\s]+)'?\s*$", re.MULTILINE)
SPEC_BINDIR_RE = re.compile(r"^%\{_bindir\}/(\S+)\s*$", re.MULTILINE)
DESKTOP_EXEC_RE = re.compile(r"^Exec=(\S+)", re.MULTILINE)

RPM_SPECS = (
    "packaging/linux/rpm/sentinel-desktop.spec",
    "packaging/linux/fedora-kde/sentinel-desktop.spec",
)
METAINFO_FILES = (
    "packaging/linux/dev.sentinel.Sentinel.metainfo.xml",
    "packaging/linux/fedora-kde/dev.sentinel.Sentinel.metainfo.xml",
)
SNAP_MANIFEST = "packaging/linux/snap/snapcraft.yaml"
DESKTOP_ENTRY = "packaging/linux/dev.sentinel.Sentinel.desktop"
PACKAGE_PRESET = "package-ready"


def rpm_version(canonical: str) -> str:
    """Encode the canonical version as an RPM ``Version:`` value.

    RPM forbids ``-`` inside ``Version`` (it separates version and release), so
    the pre-release suffix is expressed with the ``~`` ordering marker, which
    also sorts the pre-release before the final release. Ro-ASD's artifact
    manifest schema accepts ``~`` in release strings for the same reason.
    """
    return canonical.replace("-", "~", 1)


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


def check(root: Path) -> list[str]:
    errors: list[str] = []

    cmake = read(root, "CMakeLists.txt")
    match = APP_VERSION_RE.search(cmake)
    if not match:
        return ["CMakeLists.txt: SENTINEL_APP_VERSION is not defined"]
    canonical = match.group(1)
    if not NUMERIC_PREFIX_RE.match(canonical):
        errors.append(f"CMakeLists.txt: SENTINEL_APP_VERSION {canonical!r} has no major.minor.patch prefix")
    expected_rpm = rpm_version(canonical)
    desktop_cmake = read(root, "apps/sentinel-desktop/CMakeLists.txt")
    if "DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user" in desktop_cmake:
        errors.append("User unit destination must respect the staged install prefix")
    rust_version = re.search(r'^version\s*=\s*"([^"]+)"', read(root, "cli/Cargo.toml"), re.MULTILINE)
    if not rust_version or rust_version.group(1) != canonical:
        errors.append("cli/Cargo.toml: workspace version differs from the canonical version")
    cpack = read(root, "cmake/SentinelCPack.cmake")
    if 'string(REPLACE "-" "~" SENTINEL_PACKAGE_VERSION "${SENTINEL_APP_VERSION}")' not in cmake:
        errors.append("CMakeLists.txt: distro package version must derive from SENTINEL_APP_VERSION")
    cpack_contract = (
        'set(CPACK_PACKAGE_VERSION "${SENTINEL_PROJECT_VERSION}")',
        'set(CPACK_PACKAGE_FILE_NAME "sentinel-desktop-${SENTINEL_APP_VERSION}-${CMAKE_SYSTEM_NAME}")',
        'set(CPACK_DEBIAN_PACKAGE_VERSION "${SENTINEL_PACKAGE_VERSION}")',
        'set(CPACK_RPM_PACKAGE_VERSION "${SENTINEL_PACKAGE_VERSION}")',
        'set(CPACK_DMG_VOLUME_NAME "Sentinel ${SENTINEL_APP_VERSION}")',
        'set(CPACK_NSIS_PACKAGE_NAME "Sentinel ${SENTINEL_APP_VERSION}")',
    )
    for required in cpack_contract:
        if required not in cpack:
            errors.append(f"cmake/SentinelCPack.cmake: missing version contract {required}")

    plist = read(root, "packaging/macos/bundle/Info.plist.in")
    if "<string>@SENTINEL_PROJECT_VERSION@</string>" not in plist:
        errors.append("macOS Info.plist: CFBundleShortVersionString must stay numeric")
    if "<string>@SENTINEL_PROJECT_VERSION@.@SENTINEL_BUILD_NUMBER@</string>" not in plist:
        errors.append("macOS Info.plist: CFBundleVersion must include the build number")

    for spec in RPM_SPECS:
        text = read(root, spec)
        version_match = SPEC_VERSION_RE.search(text)
        release_match = SPEC_RELEASE_RE.search(text)
        if "qt6-qtsql" in text:
            errors.append(f"{spec}: Qt SQL is supplied by Fedora qt6-qtbase, not qt6-qtsql")
        for required in ("%{_bindir}/sentinel-plugin-host", "%dir %{_sysconfdir}/sentinel",
                         "%license %{_datadir}/licenses/sentinel", "%doc %{_datadir}/doc/Sentinel",
                         "qt6-qtbase-gui", "qt6-qtwayland"):
            if required not in text:
                errors.append(f"{spec}: missing packaging contract entry {required}")
        if not version_match or version_match.group(1) != expected_rpm:
            actual = version_match.group(1) if version_match else "<missing>"
            errors.append(f"{spec}: Version {actual!r} != expected {expected_rpm!r}")
        if not release_match or release_match.group(1) != "1%{?dist}":
            actual = release_match.group(1) if release_match else "<missing>"
            errors.append(f"{spec}: Release {actual!r} != expected '1%{{?dist}}'")

    for metainfo in METAINFO_FILES:
        text = read(root, metainfo)
        releases = METAINFO_RELEASE_RE.findall(text)
        if not releases:
            errors.append(f"{metainfo}: no <release> entry")
        elif releases[0] != canonical:
            errors.append(f"{metainfo}: newest release {releases[0]!r} != canonical {canonical!r}")

    snap_version = SNAP_VERSION_RE.search(read(root, SNAP_MANIFEST))
    if not snap_version or snap_version.group(1) != canonical:
        actual = snap_version.group(1) if snap_version else "<missing>"
        errors.append(f"{SNAP_MANIFEST}: version {actual!r} != canonical {canonical!r}")

    for group in (RPM_SPECS, METAINFO_FILES):
        first = read(root, group[0])
        for duplicate in group[1:]:
            if read(root, duplicate) != first:
                errors.append(f"{duplicate}: diverges from {group[0]}")

    presets = json.loads(read(root, "CMakePresets.json"))
    targets = {
        preset["name"]: preset.get("targets")
        for preset in presets.get("buildPresets", [])
        if preset.get("name") == PACKAGE_PRESET
    }
    package_targets = targets.get(PACKAGE_PRESET)
    if package_targets is None:
        errors.append(f"CMakePresets.json: build preset {PACKAGE_PRESET!r} must declare explicit targets")
        package_targets = []
    for spec in RPM_SPECS:
        text = read(root, spec)
        for binary in sorted(set(SPEC_BINDIR_RE.findall(text))):
            if binary not in package_targets:
                errors.append(
                    f"{spec}: ships %{{_bindir}}/{binary} but the {PACKAGE_PRESET} preset does not build it"
                )

    desktop_exec = DESKTOP_EXEC_RE.search(read(root, DESKTOP_ENTRY))
    if not desktop_exec:
        errors.append(f"{DESKTOP_ENTRY}: no Exec= entry")
    else:
        launched = desktop_exec.group(1)
        if launched not in set(SPEC_BINDIR_RE.findall(read(root, RPM_SPECS[0]))):
            errors.append(f"{DESKTOP_ENTRY}: Exec={launched} is not shipped by the RPM spec")

    print(f"canonical SENTINEL_APP_VERSION: {canonical}")
    print(f"expected RPM Version: {expected_rpm}")
    print(f"{PACKAGE_PRESET} binaries: {', '.join(sorted(package_targets))}")
    return errors


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()

    problems = check(args.repo)
    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        sys.exit(1)
    print("packaging metadata is consistent")
