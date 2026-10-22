#!/usr/bin/env python3
"""Run local QML checks with Sentinel's generated module context."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default=os.environ.get("SENTINEL_QML_BUILD_DIR", "build/tests"))
    parser.add_argument("files", nargs="+")
    args = parser.parse_args(argv)
    build = Path(args.build_dir)
    if not build.is_absolute():
        build = ROOT / build
    imports = build / "apps/sentinel-desktop"
    module = imports / "Sentinel/Desktop"
    if not all((module / name).is_file() for name in ("qmldir", "sentinel-desktop.qmltypes")):
        print("QML module metadata missing. Configure/build the tests preset first, or set "
              "SENTINEL_QML_BUILD_DIR to a configured and built preset directory.", file=sys.stderr)
        return 2
    executable = shutil.which("qmllint")
    if not executable:
        print("qmllint missing: add your Qt bin directory to PATH.", file=sys.stderr)
        return 2
    files = [str(Path(name).resolve()) for name in args.files]
    return subprocess.run([executable, "--max-warnings", "0", "-I", str(ROOT / "ui/qml"),
                           "-I", str(imports), *files], cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())
