#!/usr/bin/env python3
"""Check a staged Linux install against the RPM file ownership contract."""

from __future__ import annotations

import argparse
import os
import re
from pathlib import Path, PurePosixPath

from check_packaging_consistency import RPM_SPECS, SPEC_BINDIR_RE, read


def ownership(spec: str) -> list[tuple[str, bool]]:
    section = spec.split("\n%files\n", 1)[1].split("\n%changelog", 1)[0]
    macros = {
        "_bindir": "/usr/bin",
        "_datadir": "/usr/share",
        "_prefix": "/usr",
        "_sysconfdir": "/etc",
    }
    owned = []
    for line in section.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        recursive = not line.startswith("%dir ")
        line = re.sub(r"^%(?:license|doc|dir|config(?:\([^)]*\))?)\s+", "", line)
        for macro, value in macros.items():
            line = line.replace("%{" + macro + "}", value)
        if not line.startswith("/") or "%" in line or ".." in PurePosixPath(line).parts:
            raise ValueError(f"Unsupported RPM ownership entry: {line}")
        owned.append((line, recursive))
    return owned


def check(root: Path, image: Path, manifest: Path | None = None) -> list[str]:
    image = image.resolve()
    spec = read(root, RPM_SPECS[0])
    owned = ownership(spec)
    errors = []
    actual = set()
    for file in image.rglob("*"):
        if not file.is_file() and not file.is_symlink():
            continue
        relative = "/" + file.relative_to(image).as_posix()
        actual.add(relative)
        if not file.resolve().is_relative_to(image):
            errors.append(f"Escaping symlink: {relative}")
        if not any(
            relative == path or (recursive and relative.startswith(path + "/"))
            for path, recursive in owned
        ):
            errors.append(f"Unowned installed file: {relative}")
    for path, _ in owned:
        if not (image / path.lstrip("/")).exists():
            errors.append(f"Missing RPM-owned path: {path}")
    for binary in SPEC_BINDIR_RE.findall(spec):
        file = image / "usr/bin" / binary
        if not file.is_file() or not os.access(file, os.X_OK):
            errors.append(f"Missing executable: {binary}")
    for source, destination in (
        (
            "packaging/linux/dev.sentinel.Sentinel.desktop",
            "/usr/share/applications/dev.sentinel.Sentinel.desktop",
        ),
        (
            "packaging/linux/dev.sentinel.Sentinel.metainfo.xml",
            "/usr/share/metainfo/dev.sentinel.Sentinel.metainfo.xml",
        ),
        (
            "packaging/linux/systemd/sentinel-desktop.service",
            "/usr/lib/systemd/user/sentinel-desktop.service",
        ),
        (
            "packaging/linux/etc/sentinel/config.json.template",
            "/etc/sentinel/config.json.template",
        ),
        ("LICENSE", "/usr/share/licenses/sentinel/LICENSE"),
        (
            "THIRD_PARTY_NOTICES.md",
            "/usr/share/licenses/sentinel/THIRD_PARTY_NOTICES.md",
        ),
        (
            "assets/core-warden/state-contract.json",
            "/usr/share/sentinel/core-warden/state-contract.json",
        ),
        (
            "assets/core-warden/animations/manifest.json",
            "/usr/share/sentinel/core-warden/animations/manifest.json",
        ),
    ):
        file = image / destination.lstrip("/")
        if not file.is_file() or file.read_bytes() != (root / source).read_bytes():
            errors.append(f"Missing or changed installed metadata: {destination}")
    for source in (root / "assets/core-warden/animations").rglob("*.png"):
        destination = (
            image
            / "usr/share/sentinel/core-warden/animations"
            / source.relative_to(root / "assets/core-warden/animations")
        )
        if not destination.is_file() or destination.read_bytes() != source.read_bytes():
            errors.append(f"Missing or changed runtime asset: {source.name}")
    if manifest is not None:
        listed = set()
        for line in manifest.read_text().splitlines():
            file = Path(line)
            if file.is_relative_to(image):
                line = "/" + file.relative_to(image).as_posix()
            if not line.startswith("/") or ".." in PurePosixPath(line).parts:
                errors.append(f"Invalid install manifest path: {line}")
            listed.add(line)
        if listed != actual:
            errors.append(
                f"Install manifest mismatch: missing={sorted(listed - actual)}, extra={sorted(actual - listed)}"
            )
    return errors


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo", type=Path, default=Path(__file__).resolve().parents[2]
    )
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    problems = check(args.repo, args.image, args.manifest)
    for problem in problems:
        print(f"ERROR: {problem}")
    if not problems:
        print("Linux install manifest and RPM ownership are consistent")
    raise SystemExit(bool(problems))
