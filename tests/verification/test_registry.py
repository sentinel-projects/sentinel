import copy
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "verification_validator", ROOT / "tools/verification/validate.py"
)
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)
SHA = "a" * 40
NOW = dt.datetime(2026, 10, 18, 1, tzinfo=dt.timezone.utc)


class RegistryTest(unittest.TestCase):
    def setUp(self):
        self.schema = json.loads((ROOT / "verification/schema.json").read_text())
        self.registry = json.loads((ROOT / "verification/registry.json").read_text())
        feature = next(
            item for item in self.registry["features"] if item["id"] == "FSV-CHAT"
        )
        self.registry["features"] = [feature]
        feature["platforms"] = [{"id": "macos", "support": "conditional"}]
        self.registry["tests"] = [
            item for item in self.registry["tests"] if item["id"] in feature["test_ids"]
        ]
        self.registry["components"] = [
            item
            for item in self.registry["components"]
            if item["id"] == "sentinel.runtime"
        ]
        self.registry["components"][0]["source_sha"] = SHA
        self.feature = feature

    def check(self, **kwargs):
        kwargs.setdefault("candidate_branch", "synthetic-fixture")
        return VALIDATOR.validate(
            self.registry, self.schema, ROOT, SHA, now=NOW, **kwargs
        )

    def receipt(self, outcome="pass", platform="macos"):
        artifact = "verification/schema.json"
        record = {
            "id": "EV-SYNTHETIC-UNIT",
            "component": "sentinel.runtime",
            "source_sha": SHA,
            "dirty": False,
            "dirty_digest": None,
            "branch": "synthetic-fixture",
            "scenario": self.feature["test_ids"][0],
            "platform": platform,
            "architecture": "fixture-arch",
            "os_version": "fixture-os",
            "toolchain": "fixture-toolchain",
            "build_profile": "fixture",
            "timestamp": "2026-10-18T00:00:00Z",
            "outcome": outcome,
            "level": "deterministic",
            "runtime_environment": "synthetic",
            "artifacts": [
                {
                    "path": artifact,
                    "sha256": hashlib.sha256(
                        (ROOT / artifact).read_bytes()
                    ).hexdigest(),
                }
            ],
            "scope": [self.feature["id"]],
            "limitations": ["Synthetic validator test, never product evidence"],
            "component_versions": {"sentinel.runtime": SHA},
        }
        self.registry["evidence"].append(record)
        self.feature["evidence_ids"].append(record["id"])
        return record

    def test_valid_registry(self):
        self.assertTrue(self.check()["valid"])

    def test_error_order_is_deterministic(self):
        self.feature["platforms"] = []
        self.feature["implementation"] = "invalid"
        self.feature["acceptance"] = "invalid"
        errors = self.check()["errors"]
        self.assertGreater(len(errors), 1)
        self.assertEqual(errors, sorted(errors))

    def test_valid_current_receipt(self):
        self.receipt()
        self.feature["verification"] = "deterministic"
        result = self.check()
        self.assertTrue(result["valid"], result["errors"])
        self.assertTrue(result["evidence"][0]["eligible"])
        self.assertFalse(result["release_certified"])

    def test_duplicate_ids(self):
        self.registry["features"].append(copy.deepcopy(self.feature))
        self.assertFalse(self.check()["valid"])

    def test_missing_evidence(self):
        self.feature["evidence_ids"] = ["missing"]
        self.assertFalse(self.check()["valid"])

    def test_broken_artifact(self):
        self.receipt()["artifacts"][0]["path"] = "verification/missing.json"
        self.assertFalse(self.check()["valid"])

    def test_changed_artifact_digest(self):
        self.receipt()["artifacts"][0]["sha256"] = "0" * 64
        self.assertFalse(self.check()["valid"])

    def test_stale_source_sha(self):
        self.receipt()["source_sha"] = "b" * 40
        result = self.check()
        self.assertTrue(result["valid"])
        self.assertIn("sha_mismatch", result["evidence"][0]["reasons"])
        self.assertFalse(result["evidence"][0]["eligible"])

    def test_stale_timestamp(self):
        self.receipt()["timestamp"] = "2020-01-01T00:00:00Z"
        self.assertIn("stale_time", self.check()["evidence"][0]["reasons"])

    def test_invalid_future_and_unqualified_timestamps(self):
        record = self.receipt()
        for timestamp, reason in [
            ("not-a-date", "invalid_timestamp"),
            ("2030-01-01T00:00:00Z", "future_timestamp"),
            ("2026-10-18T00:00:00", "timestamp_unqualified"),
        ]:
            record["timestamp"] = timestamp
            self.assertIn(reason, self.check()["evidence"][0]["reasons"])

    def test_failed_tests(self):
        self.receipt("fail")
        self.assertFalse(self.check()["evidence"][0]["eligible"])
        self.feature["verification"] = "deterministic"
        self.assertFalse(self.check()["valid"])

    def test_skipped_tests(self):
        self.receipt("skip")
        result = self.check()
        self.assertEqual(result["features"][0]["outcomes"], {"skip": 1})
        self.assertFalse(result["evidence"][0]["eligible"])

    def test_expected_failures(self):
        self.receipt("xfail")
        result = self.check()
        self.assertEqual(result["evidence"][0]["outcome"], "xfail")
        self.assertFalse(result["evidence"][0]["eligible"])

    def test_passing_rerun_does_not_erase_failure(self):
        failed = self.receipt("fail")
        passed = copy.deepcopy(failed)
        passed["id"] = "EV-RERUN"
        passed["outcome"] = "pass"
        self.registry["evidence"].append(passed)
        self.feature["evidence_ids"].append(passed["id"])
        self.feature["verification"] = "deterministic"
        self.assertFalse(self.check()["valid"])

    def test_unsupported_platform(self):
        self.feature["implementation"] = "unsupported"
        self.feature["verification"] = "unsupported"
        self.feature["platforms"][0]["support"] = "unsupported"
        self.assertTrue(self.check()["valid"])
        self.feature["verification"] = "native"
        self.assertFalse(self.check()["valid"])

    def test_unsupported_platform_receipt_is_not_eligible(self):
        self.receipt()
        self.feature["platforms"][0]["support"] = "unsupported"
        self.feature["verification"] = "unsupported"
        self.assertFalse(self.check()["evidence"][0]["eligible"])

    def test_other_branch_is_not_aggregated(self):
        self.receipt()["branch"] = "other-branch"
        self.assertIn(
            "branch_mismatch_or_unqualified", self.check()["evidence"][0]["reasons"]
        )

    def test_external_component_has_independent_exact_sha(self):
        component = self.registry["components"][0]
        component.update(repository="external-fixture", source_sha="b" * 40)
        record = self.receipt()
        record.update(source_sha="b" * 40, branch="external-release")
        record["component_versions"][component["id"]] = "b" * 40
        self.assertTrue(self.check()["evidence"][0]["eligible"])

    def test_component_version_mismatch(self):
        self.receipt()["component_versions"]["sentinel.runtime"] = "b" * 40
        self.assertFalse(self.check()["evidence"][0]["eligible"])

    def test_malformed_component_version(self):
        self.receipt()["component_versions"]["sentinel.runtime"] = ["not-a-sha"]
        self.assertFalse(self.check()["valid"])

    def test_blocked_product_decision(self):
        self.feature["verification"] = "blocked"
        self.feature["blockers"] = ["PD-D10 remains pending"]
        self.assertTrue(self.check()["valid"])
        self.feature["verification"] = "deterministic"
        self.receipt()
        self.assertFalse(self.check()["valid"])

    def test_dirty_tree_evidence(self):
        record = self.receipt()
        record["dirty"] = True
        record["dirty_digest"] = "d" * 64
        self.assertFalse(self.check()["evidence"][0]["eligible"])
        self.assertTrue(
            self.check(candidate_dirty=True, candidate_digest="d" * 64)["evidence"][0][
                "eligible"
            ]
        )
        self.assertFalse(
            self.check(candidate_dirty=True, candidate_digest="e" * 64)["evidence"][0][
                "eligible"
            ]
        )

    def test_multi_platform_evidence(self):
        first = self.receipt()
        self.feature["platforms"].append(
            {"id": "linux-fedora-kde-wayland", "support": "conditional"}
        )
        self.feature["verification"] = "deterministic"
        self.assertFalse(self.check()["valid"])
        second = copy.deepcopy(first)
        second.update(id="EV-LINUX", platform="linux-fedora-kde-wayland")
        self.registry["evidence"].append(second)
        self.feature["evidence_ids"].append(second["id"])
        self.assertTrue(self.check()["valid"])

    def test_contradictory_acceptance_status(self):
        self.receipt()
        self.feature["verification"] = "deterministic"
        self.feature["acceptance"] = "accepted"
        self.assertFalse(self.check()["valid"])

    def test_cannot_forge_authoritative_criterion_status(self):
        self.registry["criteria"][0]["status"] = "Passed"
        self.assertFalse(self.check()["valid"])

    def test_missing_external_component_evidence(self):
        self.registry["components"].append(
            {
                "id": "roasd.packaging",
                "repository": "sentinel-packaging",
                "source_sha": None,
                "dirty": False,
                "dirty_digest": None,
                "required": True,
                "boundary": "External fixture",
            }
        )
        self.feature["components"].append("roasd.packaging")
        self.receipt()
        result = self.check()
        self.assertTrue(result["warnings"])
        self.assertIn("missing_component_evidence", result["evidence"][0]["reasons"])

    def test_native_claim_cannot_use_mock(self):
        self.receipt()["level"] = "native"
        self.assertFalse(self.check()["valid"])

    def test_missing_platform_qualification(self):
        self.feature["platforms"] = []
        self.assertFalse(self.check()["valid"])

    def test_unknown_schema_field(self):
        self.feature["secret_payload"] = "not emitted"
        self.assertFalse(self.check()["valid"])

    def test_path_traversal(self):
        self.receipt()["artifacts"][0]["path"] = "../outside"
        self.assertFalse(self.check()["valid"])

    def test_invalid_structure_cli_nonzero(self):
        process = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/verification/validate.py"),
                "--registry",
                "verification/schema.json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(process.returncode, 0)
        self.assertNotIn(str(ROOT), process.stdout + process.stderr)


if __name__ == "__main__":
    unittest.main()
