"""Regression checks for source snapshots and RPM ownership failures."""

import hashlib
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
from render_release_manifests import render, validate_identity
from validate_release_manifests import check_cask, check_winget


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


class ReleaseManifestTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.output = self.directory / "rendered"
        self.artifacts = {}
        for extension in ("dmg", "exe", "msi"):
            artifact = self.directory / f"fixture.{extension}"
            artifact.write_bytes(f"distinct {extension} final signed payload".encode())
            self.artifacts[extension] = artifact
        self.arguments = ["--version", "1.2.3-rc.4", "--tag", "v1.2.3-rc.4"]
        for extension, path in self.artifacts.items():
            self.arguments += [f"--{extension}", str(path)]

    def command(self, tool, arguments):
        return subprocess.run(
            [sys.executable, str(ROOT / "tools/packaging" / tool), *arguments],
            capture_output=True,
            text=True,
            check=False,
        )

    def generate(self):
        result = self.command(
            "render_release_manifests.py",
            self.arguments + ["--output-dir", str(self.output)],
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def validate(self):
        return self.command(
            "validate_release_manifests.py",
            self.arguments
            + [
                "--cask",
                str(self.output / "sentinel.rb"),
                "--winget",
                str(self.output / "Sopwit.Sentinel.yaml"),
            ],
        )

    def test_artifact_round_trip_and_mutation(self):
        self.generate()
        self.assertEqual(self.validate().returncode, 0)
        for extension in self.artifacts:
            digest = hashlib.sha256(self.artifacts[extension].read_bytes()).hexdigest()
            manifest = "sentinel.rb" if extension == "dmg" else "Sopwit.Sentinel.yaml"
            self.assertIn(digest, (self.output / manifest).read_text())
        self.artifacts["exe"].write_bytes(b"artifact changed after manifest generation")
        self.assertNotEqual(self.validate().returncode, 0)

    def test_swapped_installer_hashes_are_rejected(self):
        self.generate()
        path = self.output / "Sopwit.Sentinel.yaml"
        hashes = {
            kind.upper(): hashlib.sha256(self.artifacts[kind].read_bytes()).hexdigest()
            for kind in ("exe", "msi")
        }
        text = path.read_text().replace(hashes["EXE"], "TEMP_HASH")
        text = text.replace(hashes["MSI"], hashes["EXE"]).replace(
            "TEMP_HASH", hashes["MSI"]
        )
        path.write_text(text)
        self.assertNotEqual(self.validate().returncode, 0)

    def test_cask_cannot_hide_a_wrong_url_in_a_comment(self):
        self.generate()
        path = self.output / "sentinel.rb"
        canonical = "https://github.com/sentinel-projects/sentinel/"
        path.write_text(
            path.read_text().replace(canonical, "https://invalid.example/")
            + "\n# "
            + canonical
        )
        self.assertNotEqual(self.validate().returncode, 0)

    def test_identity_rejects_mismatches_and_code_injection(self):
        for version, tag in (
            ("1.2.3", "v1.2.4"),
            ('1.2.3"; system("bad")', "v1.2.3"),
            ("1.2.3\nInstallerUrl: bad", "v1.2.3"),
        ):
            with self.subTest(version=version), self.assertRaises(ValueError):
                validate_identity(version, tag)
        validate_identity("1.2.3-rc.4", "v1.2.3-rc.4")

    def test_missing_artifact_does_not_create_manifests(self):
        self.artifacts["msi"].unlink()
        result = self.command(
            "render_release_manifests.py",
            self.arguments + ["--output-dir", str(self.output)],
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_unknown_template_token_is_rejected(self):
        template = self.directory / "unknown.in"
        template.write_text("__VERSION__ __UNEXPECTED__")
        with self.assertRaises(ValueError):
            render(template, {"__VERSION__": "1.2.3"})

    def test_unrendered_templates_are_not_valid_manifests(self):
        self.assertTrue(
            check_cask(
                (ROOT / "packaging/macos/Cask/sentinel.rb.in").read_text(),
                "1.2.3",
                "a" * 64,
            )
        )
        self.assertTrue(
            check_winget(
                (ROOT / "packaging/windows/winget/Sopwit.Sentinel.yaml.in").read_text(),
                "1.2.3",
                "v1.2.3",
                {"EXE": "a" * 64, "MSI": "b" * 64},
            )
        )


if __name__ == "__main__":
    unittest.main()
