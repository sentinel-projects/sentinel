"""Negative UI evidence checks without a window, provider or account."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/verification"))
import desktop_ui as ui


class DesktopEvidenceTest(unittest.TestCase):
    def fixture(self, directory, outcome="pass"):
        functions = "".join(
            f'<TestFunction name="{name}"><Incident type="{outcome}"/></TestFunction>'
            for name in ui.METHODS
        )
        metadata = []
        for tag, (width, height) in ui.CAPTURES.items():
            # Minimal header for exercising the collector's dimension guard, not a rendering oracle.
            (directory / (tag + ".png")).write_bytes(
                b"\x89PNG\r\n\x1a\n"
                + b"\0" * 8
                + width.to_bytes(4, "big")
                + height.to_bytes(4, "big")
            )
            metadata.append(
                "<Message><Description>P605_CAPTURE "
                + json.dumps(dict(tag=tag, dpr=1, font="synthetic"))
                + "</Description></Message>"
            )
        return "<TestCase>" + functions + "".join(metadata) + "</TestCase>"

    def test_complete_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(ui.inspect_result(self.fixture(root), 0, root)[0], "pass")

    def test_corrupt_or_incomplete_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            for value in ("invalid", "<TestCase/>"):
                self.assertEqual(
                    ui.inspect_result(value, 0, Path(tmp))[0], "unverified"
                )

    def test_missing_or_wrong_size_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            xml = self.fixture(root)
            (root / "paper-compact.png").write_bytes(b"corrupted")
            self.assertEqual(ui.inspect_result(xml, 0, root)[0], "unverified")
            (root / "paper-compact.png").unlink()
            self.assertEqual(ui.inspect_result(xml, 0, root)[0], "unverified")

    def test_incident_outcomes_never_promoted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for incident, expected in [
                ("skip", "skip"),
                ("xfail", "xfail"),
                ("xpass", "fail"),
                ("fail", "fail"),
            ]:
                self.assertEqual(
                    ui.inspect_result(self.fixture(root, incident), 0, root)[0],
                    expected,
                )
            self.assertEqual(ui.inspect_result(self.fixture(root), 1, root)[0], "fail")

    def test_missing_method_result_never_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            xml = self.fixture(root).replace('<Incident type="pass"/>', "", 1)
            self.assertEqual(ui.inspect_result(xml, 0, root)[0], "unverified")


if __name__ == "__main__":
    unittest.main()
