#!/usr/bin/env python3
"""Production Main UI/private daemon fixture evidence; never native/live acceptance."""

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile

from desktop_ui import verify as verify_common
from e2e import digest, redact
from validate import git_identity

ROOT = Path(__file__).resolve().parents[2]
SCENARIOS = {
    "disconnected",
    "connected",
    "chat-stream",
    "chat-persisted",
    "chat-cancel",
    "approval-focus",
    "approval-deny",
    "approval-allow",
    "reconnect",
    "inspector",
    "layouts",
    "delete-confirmation",
}
CAPTURES = {
    "disconnected-compact": (780, 720),
    "streaming-wide": (1320, 720),
    "cancelled-wide": (1320, 720),
    "approval-deny": (1320, 720),
    "inspector-wide": (1320, 720),
    "inspector-compact-large-font": (780, 720),
    "delete-confirmation": (1320, 720),
}


def executable():
    folder = ROOT / "build/tests/tests"
    if platform.system() == "Darwin":
        return (
            folder
            / "settings_ui_certification_probe.app/Contents/MacOS/settings_ui_certification_probe"
        )
    return folder / (
        "settings_ui_certification_probe.exe"
        if os.name == "nt"
        else "settings_ui_certification_probe"
    )


def parse_markers(text):
    rows, captures, toolchain = [], [], "Unavailable"
    for line in text.splitlines():
        if line.startswith("P606_SCENARIO "):
            rows.append(json.loads(line.removeprefix("P606_SCENARIO ")))
        elif line.startswith("P606_CAPTURE "):
            captures.append(json.loads(line.removeprefix("P606_CAPTURE ")))
        elif line.startswith("P606_ENV "):
            toolchain = line.removeprefix("P606_ENV ")
    return rows, captures, toolchain


def validate_rows(rows):
    if (
        len(rows) != len(SCENARIOS)
        or {r["scenario"] for r in rows} != SCENARIOS
        or any(r["outcome"] != "pass" for r in rows)
    ):
        raise ValueError("Incomplete Main interaction oracles")
    by_id = {r["scenario"]: r for r in rows}
    expected = {
        "disconnected": {"rendered_composer": True},
        "connected": {"ready": True, "send_enabled": True},
        "chat-stream": {"session_matches": True, "rendered_composer": True},
        "chat-persisted": {"output": "p606_delta completed"},
        "chat-cancel": {
            "state": "cancelled",
            "other_session_unchanged": True,
            "visible_notice": True,
        },
        "approval-focus": {"correct_run": True, "default": "deny"},
        "approval-deny": {"mutation": False, "focus_restored": True},
        "approval-allow": {
            "sha256": "fa2bbf37c7f215feace35d61be845ca8b691e718f9afc6a015702aa0b9db36fa",
            "steps": 2,
            "successful_writes": 1,
        },
        "reconnect": {"session_matches": True},
        "inspector": {"state": "Completed", "correct_run": True},
        "layouts": {
            "compact_width": 780,
            "wide_width": 1320,
            "application_font_points": 20,
        },
        "delete-confirmation": {
            "deliberate_activation": True,
            "other_session_unchanged": True,
            "deleted": True,
        },
    }
    for scenario, fields in expected.items():
        if any(by_id[scenario].get(k) != v for k, v in fields.items()):
            raise ValueError("Invalid Main side-effect or state oracle")


def validate_captures(captures, directory):
    if len(captures) != len(CAPTURES) or {r["tag"] for r in captures} != set(CAPTURES):
        raise ValueError("Incomplete Main captures")
    for capture in captures:
        tag = capture["tag"]
        data = (directory / (tag + ".png")).read_bytes()
        size = tuple(int.from_bytes(data[n : n + 4], "big") for n in (16, 20))
        if (
            data[:8] != b"\x89PNG\r\n\x1a\n"
            or size != CAPTURES[tag]
            or (capture["width"], capture["height"]) != size
            or capture["dpr"] != 1
            or not capture["font"]
            or capture["renderer"] != "software"
            or capture.get("theme") != "Paper"
            or capture.get("motion") != "reduced"
            or capture.get("fontPointSize")
            != (
                20
                if tag in {"inspector-compact-large-font", "delete-confirmation"}
                else 14
            )
        ):
            raise ValueError("Invalid Main capture metadata")


def verify(directory, identity):
    receipt = verify_common(directory, identity, executable())
    if receipt["outcome"] == "pass":
        required = {
            "scenario-results.json",
            "capture-settings.json",
            "binary.json",
            "diagnostic.txt",
        } | {s + ".png" for s in CAPTURES}
        if not required <= {Path(a["path"]).name for a in receipt["artifacts"]}:
            raise ValueError("Missing Main evidence artifacts")
        validate_rows(json.loads((directory / "scenario-results.json").read_text()))
        validate_captures(
            json.loads((directory / "capture-settings.json").read_text()), directory
        )
    return receipt


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    identity = git_identity(ROOT)
    if args.verify:
        verify(args.verify, identity)
        return 0
    base = ROOT / "build/tests/desktop-daemon-verification"
    if not (base.parent / "CMakeCache.txt").is_file():
        parser.error("Reuse existing tests build")
    base.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=base))
    with tempfile.TemporaryDirectory(
        prefix="sd6-", dir="/tmp" if os.name == "posix" else None
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
            SENTINEL_DESKTOP_DAEMON_EVIDENCE_DIR=str(output),
        )
        outcome, diagnostic, rows, captures, toolchain = (
            "unverified",
            "",
            [],
            [],
            "Unavailable",
        )
        try:
            result = subprocess.run(
                [str(executable()), "--daemon-e2e"],
                cwd=profile,
                env=env,
                capture_output=True,
                timeout=65,
            )
            diagnostic = (result.stdout + result.stderr).decode(errors="replace")
            outcome = (
                "blocked"
                if result.returncode == 77
                else "fail" if result.returncode else "pass"
            )
            if outcome == "pass":
                try:
                    rows, captures, toolchain = parse_markers(diagnostic)
                    validate_rows(rows)
                    validate_captures(captures, output)
                except (ValueError, KeyError, TypeError, OSError):
                    outcome = "unverified"
        except FileNotFoundError:
            outcome, diagnostic = (
                "blocked",
                "Missing Main fixture executable or dependency",
            )
        except subprocess.TimeoutExpired:
            outcome, diagnostic = (
                "fail",
                "Main fixture timeout; owned child killed and reaped",
            )
        (output / "diagnostic.txt").write_text(
            redact(diagnostic, [ROOT, Path.home(), profile])[:65536]
        )
    (output / "scenario-results.json").write_text(json.dumps(rows, indent=2) + "\n")
    (output / "capture-settings.json").write_text(json.dumps(captures, indent=2) + "\n")
    (output / "binary.json").write_text(
        json.dumps({"sha256": digest(executable()) if executable().is_file() else None})
    )
    receipt = dict(
        id="UI-P606-" + output.name,
        component="sentinel.desktop",
        source_sha=identity[0],
        dirty=identity[1],
        dirty_digest=identity[2],
        branch=identity[3],
        scenario="UI-P606-MAIN",
        platform={"Darwin": "macos", "Windows": "windows"}.get(
            platform.system(), "linux-x11"
        ),
        architecture=platform.machine(),
        os_version=platform.platform(),
        toolchain=toolchain,
        build_profile="tests/Main/offscreen/software/DPR1/disposable",
        timestamp=dt.datetime.now(dt.timezone.utc).isoformat(),
        outcome=outcome,
        level="integration",
        runtime_environment="synthetic",
        artifacts=[
            {"path": p.relative_to(ROOT).as_posix(), "sha256": digest(p)}
            for p in sorted(output.iterdir())
            if p.is_file()
        ],
        scope=[
            "FSV-DESKTOP",
            "FSV-CHAT",
            "FSV-AGENT",
            "FSV-APPROVAL",
            "FSV-IPC",
            "FSV-SESSIONS",
        ],
        component_versions={
            k: identity[0]
            for k in ["sentinel.desktop", "sentinel.runtime", "sentinel.protocol"]
        },
        limitations=[
            "Production Main and daemon service in one test process; scripted inference, real IPC/gateway/executor/persistence",
            "Screenshots are review candidates; no automatically accepted pixel baseline",
            "Native screen reader/portal/tray/notifications/Windows focus and whole-process restart remain unverified",
            "Application-font scaling is bounded; hardcoded QML pixel sizes do not establish full 200% shell scaling",
            "Successful rebuild required before collection; binary hash alone does not establish source freshness",
        ],
    )
    if git_identity(ROOT) != identity:
        raise RuntimeError("Checkout changed during collection; no receipt published")
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    verify(output, identity)
    print(outcome, output.relative_to(ROOT))
    return int(outcome in {"fail", "unverified"})


if __name__ == "__main__":
    raise SystemExit(main())
