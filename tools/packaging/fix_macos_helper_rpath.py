#!/usr/bin/env python3
"""Point a bundled macOS helper at the app's Frameworks directory.

``macdeployqt`` rewrites the main application but leaves extra executables in
``Contents/Helpers`` pointing at the build machine's Qt installation. Rewrite
those absolute install names to ``@rpath`` so the signed bundle is relocatable,
and ensure the helper carries the same relative rpath as the app.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

FRAMEWORK_RE = re.compile(
    r"^(?P<prefix>.*/)?(?P<name>[^/]+)\.framework/Versions/(?P<version>[^/]+)/(?P=name)$"
)
SYSTEM_PREFIXES = ("/System/", "/usr/lib/", "/Library/Frameworks/")


def dependencies(binary: Path) -> list[str]:
    output = subprocess.run(
        ["otool", "-L", str(binary)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()[1:]
    return [line.strip().split(" (", 1)[0] for line in output if line.strip()]


def has_rpath(binary: Path, rpath: str) -> bool:
    output = subprocess.run(
        ["otool", "-l", str(binary)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return re.search(rf"path {re.escape(rpath)} \(offset", output) is not None


def rewrite(binary: Path, frameworks: Path, rpath: str) -> list[str]:
    changes: list[str] = []
    for dependency in dependencies(binary):
        if not dependency.startswith("/"):
            continue
        if dependency.startswith(SYSTEM_PREFIXES):
            continue
        framework = FRAMEWORK_RE.match(dependency)
        if framework:
            replacement = (
                f"@rpath/{framework.group('name')}.framework/"
                f"Versions/{framework.group('version')}/{framework.group('name')}"
            )
        else:
            bundled = frameworks / Path(dependency).name
            if not bundled.exists():
                continue
            replacement = f"@rpath/{bundled.name}"
        if replacement != dependency:
            subprocess.run(
                ["install_name_tool", "-change", dependency, replacement, str(binary)],
                check=True,
            )
            changes.append(f"{dependency} -> {replacement}")
    if not has_rpath(binary, rpath):
        subprocess.run(
            ["install_name_tool", "-add_rpath", rpath, str(binary)],
            check=True,
        )
        changes.append(f"added rpath {rpath}")
    return changes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("frameworks", type=Path)
    parser.add_argument("--rpath", default="@executable_path/../Frameworks")
    args = parser.parse_args()
    if not args.binary.is_file():
        print(f"error: helper not found: {args.binary}", file=sys.stderr)
        return 1
    if not args.frameworks.is_dir():
        print(f"error: frameworks directory not found: {args.frameworks}", file=sys.stderr)
        return 1
    for change in rewrite(args.binary, args.frameworks, args.rpath):
        print(change)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
