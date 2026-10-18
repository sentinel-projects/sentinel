"""Failure-oriented collector tests; no application or network access."""

import datetime as dt
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/verification"))
import e2e
from validate import classify_evidence


class E2eCollectorTest(unittest.TestCase):
    def xml(self, kind="pass"):
        return f'<TestCase><TestFunction name="scenario"><Incident type="{kind}"/></TestFunction></TestCase>'

    def test_outcomes_preserved(self):
        for kind, expected in [
            ("pass", "pass"),
            ("fail", "fail"),
            ("skip", "skip"),
            ("xfail", "xfail"),
            ("xpass", "fail"),
        ]:
            with self.subTest(kind=kind):
                self.assertEqual(e2e.qt_outcome(self.xml(kind), 0), expected)

    def test_wrong_slot_cannot_pass(self):
        self.assertEqual(e2e.qt_outcome(self.xml(), 0, "differentSlot"), "unverified")

    def test_nonzero_cannot_pass(self):
        self.assertEqual(e2e.qt_outcome(self.xml(), 1), "fail")

    def test_incomplete_or_corrupt_cannot_pass(self):
        for xml in [
            "garbage",
            "<TestCase/>",
            '<TestCase><TestFunction name="initTestCase"><Incident type="pass"/></TestFunction></TestCase>',
        ]:
            self.assertEqual(e2e.qt_outcome(xml, 0), "unverified")

    def test_provider_oracles_require_all_side_effects(self):
        original = e2e.hashlib.sha256(b"original fixture bytes\n").hexdigest()
        updated = e2e.hashlib.sha256(b"QA02B independently checked bytes\n").hexdigest()
        rows = []
        for mode in sorted(e2e.PROVIDER_ROWS):
            before = {"allowed.txt": original, "unchanged.txt": original}
            after = dict(before)
            mutates = mode in {"allowed", "unaccepted-final"}
            if mutates:
                after["allowed.txt"] = updated
            rows.append(
                dict(
                    scenario=mode,
                    before=before,
                    after=after,
                    successful_writes=int(mutates),
                    terminal_events=1,
                    accepted_final=mode == "allowed",
                    planner_calls=3,
                    classifications=2 if mode == "stale-generation" else 1,
                    persisted_state=(
                        "completed"
                        if mode == "allowed"
                        else "cancelled" if mode == "cancelled" else "failed"
                    ),
                )
            )
        e2e.validate_provider_oracles(rows)
        xml = (
            "<TestCase><TestFunction name='providerDrivenChain'>"
            + "".join(
                "<Incident type='pass'><DataTag>"
                + r["scenario"]
                + "</DataTag></Incident>"
                + "<Message><Description>QA02B_ORACLE "
                + json.dumps(r)
                + "</Description></Message>"
                for r in rows
            )
            + "</TestFunction><TestFunction name='cleanupTestCase'><Incident type='pass'/></TestFunction></TestCase>"
        )
        self.assertEqual(e2e.provider_oracles(xml), rows)
        with self.assertRaises(ValueError):
            e2e.provider_oracles(
                xml.replace("name='cleanupTestCase'", "name='missing'")
            )
        for bad in [rows[:-1], rows + [rows[0]], []]:
            with self.assertRaises(ValueError):
                e2e.validate_provider_oracles(bad)
        for key, value in [
            ("successful_writes", 2),
            ("terminal_events", 2),
            ("after", {}),
            ("persisted_state", "running"),
            ("accepted_final", False),
        ]:
            altered = json.loads(json.dumps(rows))
            allowed = next(r for r in altered if r["scenario"] == "allowed")
            allowed[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                e2e.validate_provider_oracles(altered)

    def test_missing_executable(self):
        with patch.object(subprocess, "run", side_effect=FileNotFoundError):
            self.assertEqual(
                e2e.execute(Path("/missing"), "slot", Path("/tmp"), 1)[0], "blocked"
            )

    def test_timeout(self):
        with patch.object(
            subprocess, "run", side_effect=subprocess.TimeoutExpired("fixture", 1)
        ):
            self.assertEqual(
                e2e.execute(Path("/missing"), "slot", Path("/tmp"), 1)[0], "fail"
            )

    @unittest.skipUnless(e2e.os.name == "posix", "POSIX PID cleanup oracle")
    def test_timeout_reaps_owned_process(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp)
            script = profile / "owned.py"
            script.write_text(
                "import os,time\nfrom pathlib import Path\n"
                "Path('pid').write_text(str(os.getpid()))\ntime.sleep(10)\n"
            )
            outcome, _ = e2e.execute(Path(sys.executable), str(script), profile, 1)
            self.assertEqual(outcome, "fail")
            pid = int((profile / "pid").read_text())
            with self.assertRaises(ProcessLookupError):
                e2e.os.kill(pid, 0)

    def test_environment_does_not_inherit_credentials(self):
        with patch.dict(e2e.os.environ, {"SENTINEL_API_KEY": "secret"}):
            with patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess(
                    [], 0, self.xml().encode(), b""
                ),
            ) as run:
                e2e.execute(Path("/fixture"), "slot", Path("/tmp"), 1)
                self.assertNotIn("SENTINEL_API_KEY", run.call_args.kwargs["env"])
                self.assertEqual(run.call_args.kwargs["timeout"], 1)

    def test_redaction(self):
        result = e2e.redact(
            "Authorization: Bearer secret\ntoken=secret /Users/"
            + "alice/private /home/"
            + "alice/config C:"
            + "\\Users\\"
            + "alice\\config"
        )
        for private in ["Bearer secret", "token=secret", "alice"]:
            self.assertNotIn(private, result)

    def test_nonpassing_receipts_never_eligible(self):
        now = dt.datetime.now(dt.timezone.utc)
        record = dict(
            timestamp=now.isoformat(),
            source_sha="a" * 40,
            branch="fixture",
            dirty=False,
            id="fixture",
            platform="macos",
            level="integration",
        )
        for outcome in ["blocked", "unverified", "skip", "xfail", "fail", "unknown"]:
            record["outcome"] = outcome
            self.assertFalse(
                classify_evidence(record, "a" * 40, False, None, now, 30, "fixture")[
                    "eligible"
                ]
            )

    def test_sha_mismatch(self):
        now = dt.datetime.now(dt.timezone.utc)
        record = dict(
            timestamp=now.isoformat(),
            source_sha="a" * 40,
            branch="fixture",
            dirty=False,
            id="fixture",
            platform="macos",
            level="integration",
            outcome="pass",
        )
        self.assertIn(
            "sha_mismatch",
            classify_evidence(record, "b" * 40, False, None, now, 30, "fixture")[
                "reasons"
            ],
        )

    def test_bundle_corruption_and_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "artifact.txt"
            log.write_text("synthetic diagnostic")
            identity = ("a" * 40, False, None, "fixture")
            receipts = []
            for sid in e2e.SCENARIOS:
                receipts.append(
                    dict(
                        id=sid,
                        component="sentinel.runtime",
                        source_sha=identity[0],
                        dirty=False,
                        dirty_digest=None,
                        branch="fixture",
                        scenario=sid,
                        platform="macos",
                        architecture="synthetic",
                        os_version="synthetic",
                        toolchain="synthetic",
                        build_profile="synthetic",
                        timestamp=dt.datetime.now(dt.timezone.utc).isoformat(),
                        outcome="blocked",
                        level="integration",
                        runtime_environment="synthetic",
                        artifacts=[dict(path="artifact.txt", sha256=e2e.digest(log))],
                        scope=["FSV-IPC"],
                        limitations=["Synthetic unit test"],
                        component_versions={"sentinel.runtime": identity[0]},
                    )
                )
            (root / "receipts.json").write_text(json.dumps(receipts))
            # Schema remains real; only repository artifact resolution is isolated.
            schema = json.loads((e2e.ROOT / "verification/schema.json").read_text())
            (root / "verification").mkdir()
            (root / "verification/schema.json").write_text(json.dumps(schema))
            with patch.object(e2e, "ROOT", root):
                self.assertEqual(len(e2e.verify_bundle(root, identity)), 8)
                provider = next(r for r in receipts if r["scenario"] == "E2E-02")
                provider["outcome"] = "pass"
                (root / "receipts.json").write_text(json.dumps(receipts))
                with self.assertRaisesRegex(ValueError, "Missing provider"):
                    e2e.verify_bundle(root, identity)
                provider["outcome"] = "blocked"
                (root / "receipts.json").write_text(json.dumps(receipts))
                with self.assertRaisesRegex(ValueError, "provenance"):
                    e2e.verify_bundle(root, ("b" * 40, False, None, "fixture"))
                log.write_text("corrupted diagnostic")
                with self.assertRaisesRegex(ValueError, "Corrupted"):
                    e2e.verify_bundle(root, identity)

    def test_incomplete_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "receipts.json").write_text("[]")
            with self.assertRaisesRegex(ValueError, "Incomplete"):
                e2e.verify_bundle(Path(tmp), ("a" * 40, False, None, "fixture"))


if __name__ == "__main__":
    unittest.main()
