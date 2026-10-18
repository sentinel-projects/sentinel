#!/usr/bin/env python3
"""Bounded fixture boundary runner. Never executes registry-provided commands."""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from validate import classify_evidence, git_identity, schema_errors

ROOT = Path(__file__).resolve().parents[2]
SCOPES = {
    "E2E-01": ["FSV-CHAT", "FSV-IPC"],
    "E2E-02": ["FSV-AGENT", "FSV-APPROVAL"],
    "E2E-03": ["FSV-APPROVAL", "FSV-TOOLS"],
    "E2E-04": ["FSV-AGENT", "FSV-APPROVAL"],
    "E2E-05": ["FSV-IPC"],
    "E2E-06": ["FSV-IPC", "FSV-CHAT"],
    "E2E-07": ["FSV-SESSIONS"],
    "E2E-08": ["FSV-IPC", "FSV-AGENT"],
}
# Fixed executable/slot allowlist; partial supporting tests cannot pass a blocked scenario.
SCENARIOS = {
    "E2E-01": (
        "test_daemon_ipc",
        "deterministicChatEvents",
        "Streaming and persisted projection",
    ),
    "E2E-02": (
        "test_provider_agent_e2e",
        "providerDrivenChain",
        "Scripted provider; production planner/gateway/executor; server recreation retains controller",
    ),
    "E2E-03": (
        "test_agent_reliability_ipc",
        "e2eDeniedOrCancelledMutation:denied",
        "Zero workspace mutation after denial",
    ),
    "E2E-04": (
        "test_agent_reliability_ipc",
        "e2eDeniedOrCancelledMutation:cancelled",
        "Cancellation while awaiting approval only",
    ),
    "E2E-05": (
        "test_daemon_ipc",
        "interruptedRunningRunReconciledOnRestart",
        "Blocked: server reconstruction retains controller; no daemon process restart seam",
    ),
    "E2E-06": (
        "test_daemon_ipc",
        "disconnectDoesNotCancelChat",
        "Reconnect snapshot; synthetic delayed provider",
    ),
    "E2E-07": (
        "test_daemon_ipc",
        "completedRunSurvivesDaemonRestart",
        "Blocked: projection persistence does not prove conversation cross-session isolation",
    ),
    "E2E-08": (
        "test_agent_reliability_ipc",
        "cor19_eventSequenceMonotonicAndGenerationFencing",
        "Client snapshot, exact mutation and terminal event consistency",
    ),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def redact(text, paths=()):
    for path in sorted((str(p) for p in paths if str(p)), key=len, reverse=True):
        text = text.replace(path, "<isolated-path>")
    text = re.sub(
        r"(?i)authorization\s*[:=]\s*[^\r\n<]*", "Authorization: <redacted>", text
    )
    text = re.sub(r"\bsk-[A-Za-z0-9_-]{8,}\b", "<redacted>", text)
    text = re.sub(
        r"(?i)(authorization\s*[:=]\s*|api[_-]?key\s*[:=]\s*|token\s*[:=]\s*)\S+",
        r"\1<redacted>",
        text,
    )
    return re.sub(
        r"(?:/Users/|/home/|[A-Za-z]:\\Users\\)[^\s<>\"']+", "<personal-path>", text
    )


def qt_outcome(xml, returncode, expected=None):
    try:
        root = ET.fromstring(xml)
        functions = root.findall("TestFunction")
        cases = [
            f
            for f in functions
            if f.get("name") not in ("initTestCase", "cleanupTestCase")
        ]
        if expected and (
            len(cases) != 1 or cases[0].get("name") != expected.split(":")[0]
        ):
            return "unverified"
        incidents = root.findall(".//Incident")
        types = [i.get("type") for i in incidents]
        if returncode or "fail" in types or "xpass" in types:
            return "fail"
        if "skip" in types:
            return "skip"
        if "xfail" in types:
            return "xfail"
        if (
            len(cases) != 1
            or not cases[0].findall("Incident")
            or not types
            or any(t != "pass" for t in types)
        ):
            return "unverified"
        return "pass"
    except ET.ParseError:
        return "unverified"


PROVIDER_ROWS = {
    "allowed",
    "denied",
    "cancelled",
    "outside",
    "malformed",
    "unaccepted-final",
    "stale-generation",
}


def provider_oracles(xml):
    """Require every bounded side-effect oracle; partial rows cannot close E2E-02."""
    root = ET.fromstring(xml)
    incidents = root.findall("./TestFunction[@name='providerDrivenChain']/Incident")
    if (
        len(incidents) != len(PROVIDER_ROWS)
        or {i.findtext("DataTag") for i in incidents} != PROVIDER_ROWS
        or any(i.get("type") != "pass" for i in incidents)
        or root.find("./TestFunction[@name='cleanupTestCase']/Incident[@type='pass']")
        is None
    ):
        raise ValueError("Incomplete provider test rows or cleanup")
    rows = []
    for description in root.findall(".//Message/Description"):
        text = description.text or ""
        if text.startswith("QA02B_ORACLE "):
            rows.append(json.loads(text.removeprefix("QA02B_ORACLE ")))
    validate_provider_oracles(rows)
    return rows


def validate_provider_oracles(rows):
    if (
        len(rows) != len(PROVIDER_ROWS)
        or {r["scenario"] for r in rows} != PROVIDER_ROWS
    ):
        raise ValueError("Incomplete provider scenario oracles")
    original = hashlib.sha256(b"original fixture bytes\n").hexdigest()
    replacement = hashlib.sha256(b"QA02B independently checked bytes\n").hexdigest()
    for row in rows:
        mode = row["scenario"]
        before = {"allowed.txt": original, "unchanged.txt": original}
        after = dict(before)
        mutates = mode in {"allowed", "unaccepted-final"}
        if mutates:
            after["allowed.txt"] = replacement
        if (
            row["before"] != before
            or row["after"] != after
            or row["successful_writes"] != int(mutates)
            or row["terminal_events"] != 1
            or row["accepted_final"] is not (mode == "allowed")
            or row["persisted_state"]
            != (
                "completed"
                if mode == "allowed"
                else "cancelled" if mode == "cancelled" else "failed"
            )
            or row["classifications"] != (2 if mode == "stale-generation" else 1)
            or not 1 <= row["planner_calls"] <= 8
        ):
            raise ValueError("Invalid provider side-effect oracle")


def execute(executable, slot, profile, timeout):
    env = {
        k: os.environ[k] for k in ("PATH", "SystemRoot", "WINDIR") if k in os.environ
    }
    env.update(
        {
            "HOME": str(profile),
            "USERPROFILE": str(profile),
            "XDG_CONFIG_HOME": str(profile / "config"),
            "XDG_DATA_HOME": str(profile / "data"),
            "XDG_CACHE_HOME": str(profile / "cache"),
            "TMPDIR": str(profile),
            "TMP": str(profile),
            "TEMP": str(profile),
            "QT_QPA_PLATFORM": "offscreen",
        }
    )
    try:
        result = subprocess.run(
            [str(executable), slot, "-o", "-,xml"],
            env=env,
            cwd=profile,
            capture_output=True,
            timeout=timeout,
        )
        # Only sanitized bounded diagnostics are retained. Qt XML itself is the oracle.
        return qt_outcome(result.stdout, result.returncode, slot), (
            result.stdout + result.stderr
        ).decode(errors="replace")
    except FileNotFoundError:
        return "blocked", "Missing test executable or runtime dependency"
    except subprocess.TimeoutExpired:
        # subprocess.run kills and waits for its owned process. Allowlisted slots spawn no children.
        return "fail", "Scenario timeout; owned test process killed and reaped"


def verify_bundle(directory, identity):
    bundle = json.loads((directory / "receipts.json").read_text())
    schema = json.loads((ROOT / "verification/schema.json").read_text())
    for receipt in bundle:
        errors = schema_errors(receipt, schema["$defs"]["evidence"], schema)
        if errors:
            raise ValueError(str(errors))
        if (
            receipt["source_sha"],
            receipt["dirty"],
            receipt["dirty_digest"],
            receipt["branch"],
        ) != identity:
            raise ValueError("Evidence provenance mismatch")
        freshness = classify_evidence(
            receipt,
            identity[0],
            identity[1],
            identity[2],
            dt.datetime.now(dt.timezone.utc),
            30,
            identity[3],
        )
        if freshness["reasons"]:
            raise ValueError(
                "Evidence freshness mismatch: " + str(freshness["reasons"])
            )
        if receipt["scenario"] == "E2E-02" and receipt["outcome"] == "pass":
            oracle_files = [
                a
                for a in receipt["artifacts"]
                if a["path"].endswith("/E2E-02-oracles.json")
            ]
            if len(oracle_files) != 1:
                raise ValueError("Missing provider side-effect oracle artifact")
        for artifact in receipt["artifacts"]:
            path = ROOT / artifact["path"]
            if (
                path.resolve().parent != directory.resolve()
                or digest(path) != artifact["sha256"]
            ):
                raise ValueError("Corrupted or escaped evidence artifact")
            if path.name == "E2E-02-oracles.json":
                validate_provider_oracles(json.loads(path.read_text()))
            if path.name.endswith("-binary.json"):
                binary = json.loads(path.read_text())
                expected = SCENARIOS[receipt["scenario"]][0]
                executable = (
                    ROOT
                    / "build/tests/tests"
                    / (expected + (".exe" if platform.system() == "Windows" else ""))
                )
                if (
                    binary["binary"] != expected
                    or digest(executable) != binary["sha256"]
                ):
                    raise ValueError("Test binary provenance mismatch")
    if {r["scenario"] for r in bundle} != set(SCENARIOS) or len(bundle) != len(
        SCENARIOS
    ):
        raise ValueError("Incomplete evidence bundle")
    return bundle


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument(
        "--verify", type=Path, help="Verify an existing local receipt directory"
    )
    args = parser.parse_args()
    identity = git_identity(ROOT)
    if args.verify:
        verify_bundle(args.verify, identity)
        return 0
    output = ROOT / "build/tests/verification"
    if not (ROOT / "build/tests/CMakeCache.txt").is_file():
        parser.error("Reuse the existing tests build; configuration is missing")
    output.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    schema = json.loads((ROOT / "verification/schema.json").read_text())
    receipts = []
    system = platform.system()
    host = {"Darwin": "macos", "Windows": "windows"}.get(system, "linux-x11")
    for scenario, (binary, slot, limitation) in SCENARIOS.items():
        exe = (
            ROOT
            / "build/tests/tests"
            / (binary + (".exe" if system == "Windows" else ""))
        )
        # Keep nested local-socket paths below macOS sockaddr_un limits.
        with tempfile.TemporaryDirectory(
            prefix="sentinel-e2e-", dir="/tmp" if os.name == "posix" else None
        ) as temp:
            outcome, diagnostic = execute(exe, slot, Path(temp), 45)
            oracles = None
            if scenario == "E2E-02" and outcome == "pass":
                try:
                    oracles = provider_oracles(diagnostic)
                except (ValueError, KeyError, TypeError, ET.ParseError):
                    outcome = "unverified"
            try:
                toolchain = (
                    ET.fromstring(diagnostic).findtext("Environment/QtBuild")
                    or "Unknown Qt toolchain"
                )
            except ET.ParseError:
                toolchain = (
                    "Unavailable: executable failed before Qt environment report"
                )
            diagnostic = redact(diagnostic, [ROOT, Path.home(), temp])[:65536]
        # Keep supporting failure, skip and xfail visible even when the requested scope is blocked.
        if limitation.startswith("Blocked:") and outcome == "pass":
            outcome = "blocked"
        log = output / (scenario + ".txt")
        log.write_text(diagnostic)
        artifacts = [{"path": log.relative_to(ROOT).as_posix(), "sha256": digest(log)}]
        if oracles is not None:
            oracle_path = output / "E2E-02-oracles.json"
            oracle_path.write_text(json.dumps(oracles, indent=2) + "\n")
            artifacts.append(
                {
                    "path": oracle_path.relative_to(ROOT).as_posix(),
                    "sha256": digest(oracle_path),
                }
            )
        if exe.is_file():
            # Bind executable bytes without copying binaries into evidence or permitting source hashes.
            binary_manifest = output / (scenario + "-binary.json")
            binary_manifest.write_text(
                json.dumps({"binary": binary, "sha256": digest(exe)}, sort_keys=True)
            )
            artifacts.append(
                {
                    "path": binary_manifest.relative_to(ROOT).as_posix(),
                    "sha256": digest(binary_manifest),
                }
            )
        components = ["sentinel.runtime", "sentinel.protocol"]
        if binary in {"test_agent_reliability_ipc", "test_provider_agent_e2e"}:
            components.append("sentinel.desktop")
        receipt = dict(
            id=scenario + "-" + output.name,
            component="sentinel.runtime",
            source_sha=identity[0],
            dirty=identity[1],
            dirty_digest=identity[2],
            branch=identity[3],
            scenario=scenario,
            platform=host,
            architecture=platform.machine(),
            os_version=platform.platform(),
            toolchain=toolchain,
            build_profile="tests/offscreen/disposable-profile",
            timestamp=dt.datetime.now(dt.timezone.utc).isoformat(),
            outcome=outcome,
            level="integration",
            runtime_environment="synthetic",
            artifacts=artifacts,
            scope=SCOPES[scenario],
            limitations=[
                limitation,
                "Fixture boundary evidence only; no native GUI or live provider acceptance",
                "Binary freshness requires a successful build before collection; source SHA alone cannot establish it",
            ],
            component_versions={c: identity[0] for c in components},
        )
        errors = schema_errors(receipt, schema["$defs"]["evidence"], schema)
        if errors:
            raise ValueError(str(errors))
        receipts.append(receipt)
        print(scenario, outcome, flush=True)
    if git_identity(ROOT) != identity:
        raise RuntimeError(
            "Checkout identity changed during collection; no receipts published"
        )
    (output / "receipts.json").write_text(json.dumps(receipts, indent=2) + "\n")
    verify_bundle(output, identity)
    print(output.relative_to(ROOT))
    return int(any(r["outcome"] in ("fail", "unverified") for r in receipts))


if __name__ == "__main__":
    raise SystemExit(main())
