#!/usr/bin/env python3
"""Collect bounded UI fixture evidence; screenshots never approve a baseline."""

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from e2e import digest, redact
from validate import classify_evidence, git_identity, schema_errors

ROOT = Path(__file__).resolve().parents[2]
CAPTURES = {
    "paper-compact": (640, 480),
    "dark-wide": (900, 480),
    "large-text": (640, 480),
}
METHODS = {
    "test_navigation",
    "test_stream_cancel_and_denial_projection",
    "test_default_denial",
    "test_modal_focus_trap_and_restore",
    "test_explicit_allow_requires_navigation",
    "test_visual_preparation",
}


def inspect_result(xml, code, output):
    try:
        document = ET.fromstring(xml)
    except ET.ParseError:
        return "fail" if code else "unverified", [], "Unavailable"
    kinds = {node.get("type") for node in document.findall(".//Incident")}
    outcome = (
        "fail"
        if code or kinds & {"fail", "xpass"}
        else "skip" if "skip" in kinds else "xfail" if "xfail" in kinds else "pass"
    )
    functions = {
        node.get("name", "").split("::")[-1]
        for node in document.findall("TestFunction")
    }
    if outcome == "pass" and (
        not METHODS <= functions or not kinds or kinds - {"pass"}
    ):
        outcome = "unverified"
    if outcome == "pass" and any(
        not node.findall("Incident")
        for node in document.findall("TestFunction")
        if node.get("name", "").split("::")[-1] in METHODS
    ):
        outcome = "unverified"
    captures = []
    for node in document.findall(".//Description"):
        text = node.text or ""
        if "P605_CAPTURE " in text:
            try:
                captures.append(json.loads(text.split("P605_CAPTURE ", 1)[1]))
            except ValueError:
                return "unverified", [], "Unavailable"
    if outcome == "pass":
        if {c.get("tag") for c in captures} != set(CAPTURES) or len(captures) != 3:
            outcome = "unverified"
        for capture in captures:
            tag = capture.get("tag")
            path = output / (str(tag) + ".png")
            if tag not in CAPTURES or not path.is_file():
                outcome = "unverified"
                continue
            data = path.read_bytes()
            size = tuple(int.from_bytes(data[n : n + 4], "big") for n in (16, 20))
            if (
                data[:8] != b"\x89PNG\r\n\x1a\n"
                or size != CAPTURES[tag]
                or capture.get("dpr") != 1
                or not capture.get("font")
            ):
                outcome = "unverified"
    return outcome, captures, document.findtext("Environment/QtBuild") or "Unavailable"


def verify(directory, identity, binary_path=None):
    binary_path = binary_path or executable()
    receipt = json.loads((directory / "receipt.json").read_text())
    schema = json.loads((ROOT / "verification/schema.json").read_text())
    errors = schema_errors(receipt, schema["$defs"]["evidence"], schema)
    if errors:
        raise ValueError(str(errors))
    state = classify_evidence(
        receipt,
        identity[0],
        identity[1],
        identity[2],
        dt.datetime.now(dt.timezone.utc),
        30,
        identity[3],
    )
    if state["reasons"]:
        raise ValueError(
            "Evidence provenance/freshness mismatch: " + str(state["reasons"])
        )
    for artifact in receipt["artifacts"]:
        path = ROOT / artifact["path"]
        if (
            path.resolve().parent != directory.resolve()
            or digest(path) != artifact["sha256"]
        ):
            raise ValueError("Evidence artifact corrupted or escaped")
    binary = json.loads((directory / "binary.json").read_text())
    if binary["sha256"] is None:
        if receipt["outcome"] != "blocked" or binary_path.exists():
            raise ValueError("Missing binary provenance mismatch")
    elif digest(binary_path) != binary["sha256"]:
        raise ValueError("Test binary provenance mismatch")
    return receipt


def executable():
    return (
        ROOT
        / "build/tests/tests"
        / ("test_gui_components.exe" if os.name == "nt" else "test_gui_components")
    )


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    identity = git_identity(ROOT)
    if args.verify:
        verify(args.verify, identity)
        return 0
    base = ROOT / "build/tests/ui-verification"
    if not (base.parent / "CMakeCache.txt").is_file():
        parser.error("Reuse the existing tests build")
    base.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=base))
    with tempfile.TemporaryDirectory(
        prefix="sui-", dir="/tmp" if os.name == "posix" else None
    ) as profile:
        env = {
            k: os.environ[k]
            for k in ("PATH", "SystemRoot", "WINDIR")
            if k in os.environ
        }
        env.update(
            HOME=profile,
            USERPROFILE=profile,
            TMPDIR=profile,
            TMP=profile,
            TEMP=profile,
            XDG_DATA_HOME=profile + "/data",
            XDG_CONFIG_HOME=profile + "/config",
            XDG_CACHE_HOME=profile + "/cache",
            QT_QPA_PLATFORM="offscreen",
            QT_QUICK_BACKEND="software",
            QSG_RENDER_LOOP="basic",
            QT_SCALE_FACTOR="1",
            QT_FONT_DPI="96",
            SENTINEL_UI_EVIDENCE_DIR=str(output),
        )
        try:
            result = subprocess.run(
                [
                    str(executable()),
                    "-input",
                    str(ROOT / "tests/gui/components/tst_DesktopInteraction.qml"),
                    "-o",
                    "-,xml",
                ],
                cwd=profile,
                env=env,
                capture_output=True,
                timeout=60,
            )
            outcome, captures, toolchain = inspect_result(
                result.stdout, result.returncode, output
            )
            diagnostic = (result.stdout + result.stderr).decode(errors="replace")
        except FileNotFoundError:
            outcome, captures, toolchain, diagnostic = (
                "blocked",
                [],
                "Unavailable",
                "Missing UI test executable or dependency",
            )
        except subprocess.TimeoutExpired:
            outcome, captures, toolchain, diagnostic = (
                "fail",
                [],
                "Unavailable",
                "UI fixture timed out; owned child killed and reaped",
            )
        (output / "diagnostic.txt").write_text(
            redact(diagnostic, [ROOT, Path.home(), profile])[:65536]
        )
    (output / "capture-settings.json").write_text(json.dumps(captures, indent=2) + "\n")
    (output / "binary.json").write_text(
        json.dumps({"sha256": digest(executable()) if executable().is_file() else None})
    )
    artifacts = [
        {"path": p.relative_to(ROOT).as_posix(), "sha256": digest(p)}
        for p in sorted(output.iterdir())
        if p.is_file()
    ]
    receipt = dict(
        id="UI-P605-" + output.name,
        component="sentinel.desktop",
        source_sha=identity[0],
        dirty=identity[1],
        dirty_digest=identity[2],
        branch=identity[3],
        scenario="UI-P605-INTERACTION",
        platform={"Darwin": "macos", "Windows": "windows"}.get(
            platform.system(), "linux-x11"
        ),
        architecture=platform.machine(),
        os_version=platform.platform(),
        toolchain=toolchain,
        build_profile="tests/offscreen/software/DPR1/disposable",
        timestamp=dt.datetime.now(dt.timezone.utc).isoformat(),
        outcome=outcome,
        level="deterministic",
        runtime_environment="synthetic",
        artifacts=artifacts,
        scope=["FSV-DESKTOP", "FSV-APPROVAL", "FSV-CHAT"],
        component_versions={"sentinel.desktop": identity[0]},
        limitations=[
            "Component projection and keyboard fixtures; not daemon execution or full UX certification",
            "Captures are review candidates; no approved pixel baseline or cross-platform equality",
            "Native screen reader, KDE/Wayland, Windows, real tray delivery and 200% whole-shell scaling remain unverified",
            "Successful build required before collection; binary hash alone does not prove source equivalence",
        ],
    )
    if git_identity(ROOT) != identity:
        raise RuntimeError("Checkout changed during collection; no receipt published")
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    # Missing executables must still produce an honest blocked receipt.
    if executable().is_file():
        verify(output, identity)
    else:
        schema = json.loads((ROOT / "verification/schema.json").read_text())
        if schema_errors(receipt, schema["$defs"]["evidence"], schema):
            raise ValueError("Invalid blocked receipt")
    print(outcome, output.relative_to(ROOT))
    return int(outcome in ("fail", "unverified"))


if __name__ == "__main__":
    raise SystemExit(main())
