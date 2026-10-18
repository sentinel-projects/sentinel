#!/usr/bin/env python3
"""Create a deterministic source archive and matching spec from one Git commit."""

from __future__ import annotations

import argparse
import gzip
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from check_packaging_consistency import APP_VERSION_RE, rpm_version


def archive(root: Path, output: Path, ref: str = "HEAD") -> tuple[Path, Path]:
    commit = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "--verify", f"{ref}^{{commit}}"],
        text=True,
    ).strip()
    spec = subprocess.check_output(
        [
            "git",
            "-C",
            str(root),
            "show",
            f"{commit}:packaging/linux/rpm/sentinel-desktop.spec",
        ]
    )
    name = re.search(rb"^Name:\s*(\S+)", spec, re.MULTILINE).group(1).decode()
    version = re.search(rb"^Version:\s*(\S+)", spec, re.MULTILINE).group(1).decode()
    cmake = subprocess.check_output(
        ["git", "-C", str(root), "show", f"{commit}:CMakeLists.txt"], text=True
    )
    canonical = APP_VERSION_RE.search(cmake)
    if canonical is None or rpm_version(canonical.group(1)) != version:
        raise ValueError(
            "Committed spec version does not match the committed canonical version"
        )
    if not re.fullmatch(r"[A-Za-z0-9+_.~-]+", name + version):
        raise ValueError("Unsafe source archive identity")
    output.mkdir(parents=True, exist_ok=True)
    destination = output / f"{name}-{version}.tar.gz"
    spec_path = output / "sentinel-desktop.spec"
    if destination.exists() or spec_path.exists():
        raise FileExistsError(
            "Output already contains source artifacts; use an empty output directory"
        )
    with tempfile.TemporaryDirectory(prefix=".source-", dir=output) as temporary:
        staged = Path(temporary) / destination.name
        with staged.open("wb") as raw:
            with gzip.GzipFile(
                filename="", mode="wb", fileobj=raw, mtime=0
            ) as compressed:
                with subprocess.Popen(
                    [
                        "git",
                        "-C",
                        str(root),
                        "-c",
                        "tar.umask=0022",
                        "archive",
                        "--format=tar",
                        f"--prefix={name}-{version}/",
                        commit,
                    ],
                    stdout=subprocess.PIPE,
                ) as process:
                    shutil.copyfileobj(process.stdout, compressed)
                    if process.wait() != 0:
                        raise RuntimeError("git archive failed")
        staged.replace(destination)
    spec_path.write_bytes(spec)
    return destination, spec_path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo", type=Path, default=Path(__file__).resolve().parents[2]
    )
    parser.add_argument("--ref", default="HEAD")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for artifact in archive(args.repo, args.output, args.ref):
        print(artifact)
