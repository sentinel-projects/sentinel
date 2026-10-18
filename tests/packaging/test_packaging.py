"""Regression checks for source snapshots and RPM ownership failures."""

from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/packaging"))
from check_install_manifest import check, ownership
from check_packaging_consistency import RPM_SPECS, check as check_metadata
from create_source_archive import archive


class PackagingTest(unittest.TestCase):
    def test_complete_staged_payload_and_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary) / "image"
            for path, _ in ownership((ROOT / RPM_SPECS[0]).read_text()):
                target = stage / path.lstrip("/")
                if target.suffix or "/bin/" in path:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.touch()
                    if "/bin/" in path:
                        target.chmod(0o755)
                else:
                    target.mkdir(parents=True, exist_ok=True)
            for source, destination in (
                (
                    "packaging/linux/dev.sentinel.Sentinel.desktop",
                    "usr/share/applications/dev.sentinel.Sentinel.desktop",
                ),
                (
                    "packaging/linux/dev.sentinel.Sentinel.metainfo.xml",
                    "usr/share/metainfo/dev.sentinel.Sentinel.metainfo.xml",
                ),
                (
                    "packaging/linux/systemd/sentinel-desktop.service",
                    "usr/lib/systemd/user/sentinel-desktop.service",
                ),
                (
                    "packaging/linux/etc/sentinel/config.json.template",
                    "etc/sentinel/config.json.template",
                ),
                ("LICENSE", "usr/share/licenses/sentinel/LICENSE"),
                (
                    "THIRD_PARTY_NOTICES.md",
                    "usr/share/licenses/sentinel/THIRD_PARTY_NOTICES.md",
                ),
                (
                    "assets/core-warden/state-contract.json",
                    "usr/share/sentinel/core-warden/state-contract.json",
                ),
                (
                    "assets/core-warden/animations/manifest.json",
                    "usr/share/sentinel/core-warden/animations/manifest.json",
                ),
            ):
                target = stage / destination
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / source, target)
            for source in (ROOT / "assets/core-warden/animations").rglob("*.png"):
                target = (
                    stage
                    / "usr/share/sentinel/core-warden/animations"
                    / source.relative_to(ROOT / "assets/core-warden/animations")
                )
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            manifest = Path(temporary) / "manifest.txt"
            manifest.write_text(
                "\n".join(
                    "/" + file.relative_to(stage).as_posix()
                    for file in stage.rglob("*")
                    if file.is_file()
                )
            )
            self.assertEqual(check(ROOT, stage, manifest), [])
            (stage / "usr/bin/sentinel-plugin-host").unlink()
            self.assertTrue(
                any(
                    "manifest mismatch" in error
                    for error in check(ROOT, stage, manifest)
                )
            )

    def test_repository_metadata(self):
        self.assertEqual(check_metadata(ROOT), [])

    def test_staged_install_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            errors = check(ROOT, stage)
            self.assertTrue(any("sentinel-plugin-host" in error for error in errors))
            extra = stage / "usr/bin/unlisted-helper"
            extra.parent.mkdir(parents=True)
            extra.write_text("unowned")
            self.assertIn(
                "Unowned installed file: /usr/bin/unlisted-helper", check(ROOT, stage)
            )
            extra.unlink()
            extra.symlink_to("/etc/passwd")
            self.assertIn(
                "Escaping symlink: /usr/bin/unlisted-helper", check(ROOT, stage)
            )

    def test_ownership_directory_does_not_claim_siblings(self):
        spec = "\n%files\n%dir %{_sysconfdir}/sentinel\n%config(noreplace) %{_sysconfdir}/sentinel/config.json.template\n%changelog\n"
        self.assertEqual(
            ownership(spec),
            [("/etc/sentinel", False), ("/etc/sentinel/config.json.template", True)],
        )

    @unittest.skipUnless(
        (ROOT / ".git").exists(),
        "Source RPMs omit Git metadata; snapshot tests run in checkout CI",
    )
    def test_source_snapshot_is_reproducible_and_uses_committed_spec(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            first, spec = archive(ROOT, directory / "first")
            second, _ = archive(ROOT, directory / "second")
            self.assertEqual(first.read_bytes(), second.read_bytes())
            committed = subprocess.check_output(
                ["git", "-C", str(ROOT), "show", "HEAD:" + RPM_SPECS[0]]
            )
            self.assertEqual(spec.read_bytes(), committed)
            with tarfile.open(first) as source:
                member = source.extractfile(
                    first.name.removesuffix(".tar.gz") + "/" + RPM_SPECS[0]
                )
                self.assertEqual(member.read(), committed)
            with self.assertRaises(FileExistsError):
                archive(ROOT, directory / "first")


if __name__ == "__main__":
    unittest.main()
