#!/usr/bin/env python3
"""Validate rendered Homebrew Cask and Winget manifests against artifacts."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

from render_release_manifests import (
    CASK_TEMPLATE,
    WINGET_TEMPLATE,
    canonical_version,
    render,
    validate_identity,
)

ZERO_SHA256 = "0" * 64
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
CASK_VERSION_RE = re.compile(r'^\s*version\s+"([^"]+)"', re.MULTILINE)
CASK_SHA_RE = re.compile(r'^\s*sha256\s+"([^"]+)"', re.MULTILINE)
WINGET_VERSION_RE = re.compile(r"^PackageVersion:\s*(\S+)\s*$", re.MULTILINE)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read(path: Path) -> str:
    if not path.is_file():
        raise ValueError(f"manifest not found: {path}")
    return path.read_text(encoding="utf-8")


def check_cask(text: str, version: str, dmg_sha: str) -> list[str]:
    errors: list[str] = []
    match = CASK_VERSION_RE.search(text)
    if not match or match.group(1) != version:
        errors.append(
            f"cask version {match.group(1) if match else '<missing>'!r} != {version!r}"
        )
    match = CASK_SHA_RE.search(text)
    if not match or match.group(1) != dmg_sha:
        errors.append("cask SHA-256 does not match the DMG artifact")
    if ":no_check" in text:
        errors.append("cask must not disable checksum verification")
    if "https://github.com/sentinel-projects/sentinel/" not in text:
        errors.append("cask must use the canonical sentinel-projects repository")
    if "sopwit/sentinel" in text or "github.com/sentinel/sentinel" in text:
        errors.append("cask contains a stale repository URL")
    if 'app "Sentinel.app"' not in text:
        errors.append("cask must install Sentinel.app")
    if 'pkill: "sentinel-daemon"' not in text:
        errors.append("cask uninstall must stop the bundled daemon")
    expected = render(CASK_TEMPLATE, {"__VERSION__": version, "__SHA256__": dmg_sha})
    if text != expected:
        errors.append("cask differs from the artifact-bound release template")
    return errors


def check_winget(
    text: str, version: str, tag: str, hashes: dict[str, str]
) -> list[str]:
    errors: list[str] = []
    match = WINGET_VERSION_RE.search(text)
    if not match or match.group(1) != version:
        errors.append(
            f"winget PackageVersion {match.group(1) if match else '<missing>'!r} != {version!r}"
        )
    if "PackageIdentifier: Sopwit.Sentinel" not in text:
        errors.append("winget PackageIdentifier is missing")
    for name, digest in hashes.items():
        if digest == ZERO_SHA256 or not SHA256_RE.match(digest):
            errors.append(f"winget {name} hash is invalid")
        if digest not in text:
            errors.append(f"winget {name} hash is missing from the manifest")
    for installer in ("Sentinel-Windows-x64.exe", "Sentinel-Windows-x64.msi"):
        url = f"https://github.com/sentinel-projects/sentinel/releases/download/{tag}/{installer}"
        if url not in text:
            errors.append(f"winget URL is missing or stale: {url}")
    if "0000000000000000000000000000000000000000000000000000000000000000" in text:
        errors.append("winget manifest contains a zero SHA-256")
    if "sopwit/sentinel" in text or "github.com/sentinel/sentinel" in text:
        errors.append("winget manifest contains a stale repository URL")

    expected = render(
        WINGET_TEMPLATE,
        {
            "__VERSION__": version,
            "__TAG__": tag,
            "__EXE_SHA256__": hashes["EXE"],
            "__MSI_SHA256__": hashes["MSI"],
        },
    )
    if text != expected:
        errors.append(
            "winget differs from the artifact-bound release template (including installer/hash pairing)"
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cask", required=True, type=Path)
    parser.add_argument("--winget", required=True, type=Path)
    parser.add_argument("--version", default=canonical_version())
    parser.add_argument("--tag", required=True)
    parser.add_argument("--dmg", required=True, type=Path)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--msi", required=True, type=Path)
    args = parser.parse_args()

    try:
        validate_identity(args.version, args.tag)
        cask = read(args.cask)
        winget = read(args.winget)
        dmg_sha = sha256(args.dmg)
        hashes = {"EXE": sha256(args.exe), "MSI": sha256(args.msi)}
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    errors = check_cask(cask, args.version, dmg_sha)
    errors += check_winget(winget, args.version, args.tag, hashes)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("release manifests are consistent with the artifacts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
