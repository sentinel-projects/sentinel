"""Isolated regression coverage for developer build/install helpers."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
CMAKE = Path(shutil.which("cmake")).as_posix()


class CompilerCacheTest(unittest.TestCase):
    def test_owned_launcher_transitions_and_user_precedence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, build = root / "source", root / "build"
            source.mkdir()
            log = root / "calls.txt"
            wrapper = root / "launcher.cmake"
            wrapper.write_text('''file(APPEND "${CMAKE_CURRENT_LIST_DIR}/calls.txt" "call\\n")
set(args)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE 3 ${last})
    list(APPEND args "${CMAKE_ARGV${i}}")
endforeach()
execute_process(COMMAND ${args} RESULT_VARIABLE code)
if(NOT "${code}" STREQUAL "0")
    message(FATAL_ERROR "Compiler failed")
endif()
''')
            launcher = f"{CMAKE};-P;{wrapper.as_posix()}"
            secondary = root / "secondary.cmake"
            secondary.write_text(wrapper.read_text())
            sccache = f"{CMAKE};-P;{secondary.as_posix()}"
            (source / "main.cpp").write_text("int main() { return 0; }\n")
            (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.27)
project(CacheProbe LANGUAGES CXX)
include("{(ROOT / 'cmake/SentinelCcache.cmake').as_posix()}")
file(WRITE "${{CMAKE_BINARY_DIR}}/launcher.txt" "${{CMAKE_CXX_COMPILER_LAUNCHER}}")
add_executable(probe main.cpp)
''')
            env = {**os.environ, "CCACHE_DISABLE": "1"}

            def configure(*args):
                subprocess.run([CMAKE, "-S", str(source), "-B", str(build), "-G", "Ninja", *args],
                               env=env, capture_output=True, text=True, check=True)
                return (build / "launcher.txt").read_text()

            def compile_probe():
                subprocess.run([CMAKE, "--build", str(build), "--clean-first"], env=env,
                               capture_output=True, text=True, check=True)

            self.assertEqual(configure(f"-DCCACHE_PROGRAM={launcher}", "-DSCCACHE_PROGRAM="), launcher)
            compile_probe()
            calls = log.read_text()
            self.assertEqual(configure("-DSENTINEL_USE_CCACHE=OFF"), "")
            compile_probe()
            self.assertEqual(log.read_text(), calls, "OFF must stop invoking the owned launcher")
            self.assertEqual(configure("-DSENTINEL_USE_CCACHE=ON", "-DCCACHE_PROGRAM=",
                                       f"-DSCCACHE_PROGRAM={sccache}"), sccache)
            compile_probe()
            self.assertNotEqual(log.read_text(), calls)
            self.assertEqual(configure("-DSCCACHE_PROGRAM="), "")
            self.assertEqual(configure(f"-DSCCACHE_PROGRAM={sccache}"), sccache)
            user = f"{CMAKE};-E;env;CCACHE_DISABLE=1"
            self.assertEqual(configure(f"-DCMAKE_CXX_COMPILER_LAUNCHER={user}"), user)
            self.assertEqual(configure("-DSENTINEL_USE_CCACHE=OFF"), user)
            self.assertEqual(configure("-DSENTINEL_USE_CCACHE=ON",
                                       "-DCMAKE_CXX_COMPILER_LAUNCHER="), "")


@unittest.skipIf(os.name == "nt", "POSIX install helper requires a POSIX shell")
class InstallRootTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
                    "HOME": str(self.root / "home"), "CARGO_HOME": str(self.root / "cargo home"),
                    "AUDIT_CALLS": str(self.root / "calls.json")}
        self.env.pop("CARGO_INSTALL_ROOT", None)
        cargo = self.bin / "cargo"
        cargo.write_text(f"#!{sys.executable}\n" + '''import json, os, pathlib, sys
args = sys.argv[1:]
pathlib.Path(os.environ["AUDIT_CALLS"]).write_text(json.dumps(args))
if os.environ.get("FAIL_CARGO"): sys.exit(7)
root = next((x.split("=", 1)[1] for x in args if x.startswith("--root=")), None)
if root is None: root = args[args.index("--root") + 1]
binary = pathlib.Path(root) / "bin/sentinel"
binary.parent.mkdir(parents=True, exist_ok=True)
binary.write_text("#!/bin/sh\\nprintf '%s\\\\n' 'sentinel fixture'\\n")
binary.chmod(0o755)
''')
        cargo.chmod(0o755)
        shadow = self.bin / "sentinel"
        shadow.write_text("#!/bin/sh\nexit 91\n")
        shadow.chmod(0o755)

    def run_helper(self, *args, success=True):
        result = subprocess.run(["sh", str(ROOT / "tools/terminal/install-dev.sh"), *args],
                                cwd=self.root, env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        if success:
            arguments = json.loads(Path(self.env["AUDIT_CALLS"]).read_text())
            self.assertEqual(arguments[0], "install")
            self.assertIn("--locked", arguments)
        return result

    def test_default_environment_and_explicit_roots(self):
        for args, env_root, expected in [
            ([], None, Path(self.env["CARGO_HOME"])),
            ([], self.root / "environment root", self.root / "environment root"),
            (["--root", "relative root"], self.root / "unused", self.root / "relative root"),
            ([f"--root={self.root / 'equals root'}"], None, self.root / "equals root"),
        ]:
            if env_root is None:
                self.env.pop("CARGO_INSTALL_ROOT", None)
            else:
                self.env["CARGO_INSTALL_ROOT"] = str(env_root)
            result = self.run_helper(*args)
            link = expected / "share/sentinel/core-warden"
            self.assertEqual(link.resolve(), ROOT / "assets/core-warden")
            self.assertIn(str(expected / "bin/sentinel"), result.stdout)

    def test_preserves_existing_assets_and_refuses_invalid_location(self):
        target = self.root / "install"
        assets = target / "share/sentinel/core-warden"
        assets.mkdir(parents=True)
        marker = assets / "keep"
        marker.write_text("keep")
        self.run_helper("--root", str(target))
        self.assertEqual(marker.read_text(), "keep")
        marker.unlink()
        assets.rmdir()
        assets.symlink_to(self.root / "missing")
        self.run_helper("--root", str(target), success=False)
        self.assertTrue(assets.is_symlink())

    def test_rejects_bad_roots_before_install_and_propagates_cargo_failure(self):
        for args in [("--root",), ("--root=",), ("--root=a", "--root=b"), ("--",)]:
            self.run_helper(*args, success=False)
            self.assertFalse(Path(self.env["AUDIT_CALLS"]).exists())
        self.env["FAIL_CARGO"] = "1"
        target = self.root / "failed"
        self.run_helper("--root", str(target), success=False)
        self.assertFalse((target / "share").exists())

    def test_home_fallback_and_link_failure(self):
        self.env.pop("CARGO_HOME")
        target = Path(self.env["HOME"]) / ".cargo"
        self.run_helper()
        self.assertTrue((target / "share/sentinel/core-warden").is_dir())
        failing_link = self.bin / "ln"
        failing_link.write_text("#!/bin/sh\nexit 31\n")
        failing_link.chmod(0o755)
        self.run_helper("--root", str(self.root / "link-failure"), success=False)


class QmlLintTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("qmllint") and
                         (ROOT / "build/tests/apps/sentinel-desktop/Sentinel/Desktop/sentinel-desktop.qmltypes").is_file(),
                         "Requires qmllint and generated tests-preset metadata")
    def test_real_project_component_and_invalid_property(self):
        command = [sys.executable, str(ROOT / "tools/analysis/qml_lint.py"),
                   "--build-dir", str(ROOT / "build/tests")]
        valid = subprocess.run(command + [str(ROOT / "ui/qml/components/base/SentinelButton.qml")],
                               capture_output=True, text=True)
        self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)
        with tempfile.TemporaryDirectory() as temporary:
            invalid = Path(temporary) / "invalid.qml"
            invalid.write_text("import QtQuick\nimport Sentinel.Desktop\n"
                               "Item { SentinelButton { sentinelRegressionMissingProperty: true } }\n")
            result = subprocess.run(command + [str(invalid)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("sentinelRegressionMissingProperty", result.stdout + result.stderr)

    def test_metadata_prerequisite_and_project_arguments(self):
        spec = importlib.util.spec_from_file_location("sentinel_qml_lint", ROOT / "tools/analysis/qml_lint.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            with patch.object(module.subprocess, "run") as run:
                self.assertEqual(module.main(["--build-dir", str(build), "example.qml"]), 2)
                run.assert_not_called()
            metadata = build / "apps/sentinel-desktop/Sentinel/Desktop"
            metadata.mkdir(parents=True)
            for name in ("qmldir", "sentinel-desktop.qmltypes"):
                (metadata / name).touch()
            with patch.object(module.shutil, "which", return_value="qmllint"), \
                 patch.object(module.subprocess, "run") as run:
                run.return_value.returncode = 9
                self.assertEqual(module.main(["--build-dir", str(build), "example.qml"]), 9)
                args = run.call_args.args[0]
                self.assertIn(str(build / "apps/sentinel-desktop"), args)
                self.assertIn(str(ROOT / "ui/qml"), args)


if __name__ == "__main__":
    unittest.main()
