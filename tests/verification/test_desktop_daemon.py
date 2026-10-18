"""Failure-oriented Main evidence checks without a UI, daemon, profile or account."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/verification"))
import desktop_daemon as ui


class MainEvidenceTest(unittest.TestCase):
    def rows(self):
        fields = {
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
        return [
            dict(scenario=s, outcome="pass", **fields[s]) for s in sorted(ui.SCENARIOS)
        ]

    def test_complete_markers_and_oracles(self):
        rows = self.rows()
        text = "P606_ENV synthetic\n" + "\n".join(
            "P606_SCENARIO " + json.dumps(r) for r in rows
        )
        actual, captures, toolchain = ui.parse_markers(text)
        self.assertEqual(actual, rows)
        self.assertEqual(captures, [])
        self.assertEqual(toolchain, "synthetic")
        ui.validate_rows(actual)

    def test_incomplete_duplicate_and_failed_rows(self):
        rows = self.rows()
        for bad in [
            [],
            rows[:-1],
            rows + [rows[0]],
            [dict(r, outcome="skip") for r in rows],
        ]:
            with self.assertRaises(ValueError):
                ui.validate_rows(bad)
        with self.assertRaises(ValueError):
            ui.parse_markers("P606_SCENARIO broken")

    def test_false_mutation_or_state_cannot_pass(self):
        for scenario, key, value in [
            ("approval-allow", "successful_writes", 2),
            ("approval-allow", "sha256", "wrong"),
            ("approval-deny", "mutation", True),
            ("chat-cancel", "state", "completed"),
            ("disconnected", "rendered_composer", False),
            ("delete-confirmation", "other_session_unchanged", False),
        ]:
            rows = self.rows()
            next(r for r in rows if r["scenario"] == scenario)[key] = value
            with self.subTest(scenario=scenario, key=key), self.assertRaises(
                ValueError
            ):
                ui.validate_rows(rows)

    def fixture(self, directory):
        captures = []
        for tag, size in ui.CAPTURES.items():
            # Minimal PNG header only for validator guards; never a rendering oracle.
            (directory / (tag + ".png")).write_bytes(
                b"\x89PNG\r\n\x1a\n"
                + b"\0" * 8
                + size[0].to_bytes(4, "big")
                + size[1].to_bytes(4, "big")
            )
            captures.append(
                dict(
                    tag=tag,
                    width=size[0],
                    height=size[1],
                    dpr=1,
                    font="fixture",
                    renderer="software",
                    theme="Paper",
                    motion="reduced",
                    fontPointSize=(
                        20
                        if tag
                        in {"inspector-compact-large-font", "delete-confirmation"}
                        else 14
                    ),
                )
            )
        return captures

    def test_capture_integrity_guards(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            captures = self.fixture(root)
            ui.validate_captures(captures, root)
            for bad in [[], captures[:-1], captures + [captures[0]]]:
                with self.assertRaises(ValueError):
                    ui.validate_captures(bad, root)
            captures[0]["dpr"] = 2
            with self.assertRaises(ValueError):
                ui.validate_captures(captures, root)
            captures[0]["dpr"] = 1
            (root / (captures[0]["tag"] + ".png")).write_bytes(b"broken")
            with self.assertRaises(ValueError):
                ui.validate_captures(captures, root)


if __name__ == "__main__":
    unittest.main()
