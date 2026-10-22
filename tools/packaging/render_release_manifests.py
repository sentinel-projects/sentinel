#!/usr/bin/env python3
"""Render the Homebrew Cask and Winget manifests from release artifacts.

The checked-in files are templates because a valid manifest needs the SHA-256
of the exact upload. The tagged release workflow calls this after all platform
artifacts have been collected, then validates the rendered files before the
draft release is created.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CASK_TEMPLATE = ROOT / "packaging/macos/Cask/sentinel.rb.in"
WINGET_TEMPLATE = ROOT / "packaging/windows/winget/Sopwit.Sentinel.yaml.in"


VERSION_RE = re.compile(
    r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?"
)


def canonical_version() -> str:
    match = re.search(
        r'set\(SENTINEL_APP_VERSION\s+"([^\"]+)"',
        (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
    )
    if not match:
        raise ValueError("canonical SENTINEL_APP_VERSION is missing")
    return match.group(1)


def validate_identity(version: str, tag: str) -> None:
    if not VERSION_RE.fullmatch(version):
        raise ValueError(
            "version must be a numeric major.minor.patch with an optional prerelease suffix"
        )
    if tag != f"v{version}":
        raise ValueError(f"tag {tag!r} does not match version {version!r}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def render(template: Path, replacements: dict[str, str]) -> str:
    text = template.read_text(encoding="utf-8")
    for token, value in replacements.items():
        text = text.replace(token, value)
    remaining = sorted(set(re.findall(r"__[A-Z][A-Z0-9_]*__", text)))
    if remaining:
        raise ValueError(f"{template}: unrendered tokens: {', '.join(remaining)}")
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--version",
        default=canonical_version(),
        help="defaults to SENTINEL_APP_VERSION",
    )
    parser.add_argument("--tag", required=True, help="Git tag, e.g. v1.0.0-rc.8")
    parser.add_argument("--dmg", required=True, type=Path)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--msi", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    try:
        validate_identity(args.version, args.tag)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for artifact in (args.dmg, args.exe, args.msi):
        if not artifact.is_file():
            print(f"error: release artifact not found: {artifact}", file=sys.stderr)
            return 1

    cask = render(
        CASK_TEMPLATE,
        {"__VERSION__": args.version, "__SHA256__": sha256(args.dmg)},
    )
    winget = render(
        WINGET_TEMPLATE,
        {
            "__VERSION__": args.version,
            "__TAG__": args.tag,
            "__EXE_SHA256__": sha256(args.exe),
            "__MSI_SHA256__": sha256(args.msi),
        },
    )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "sentinel.rb").write_text(cask, encoding="utf-8")
    (args.output_dir / "Sopwit.Sentinel.yaml").write_text(winget, encoding="utf-8")
    print(f"rendered {args.output_dir / 'sentinel.rb'}")
    print(f"rendered {args.output_dir / 'Sopwit.Sentinel.yaml'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
