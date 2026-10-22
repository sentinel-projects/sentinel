"""Test the real CI range helper using Git repositories and a synthetic formatter."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class FormatRangeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repository"
        self.repo.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        formatter = self.bin / "clang-format-diff.py"
        formatter.write_text(f"#!{sys.executable}\n" + '''import sys
# Stand in only for clang-format: any newly added BAD_FORMAT line needs repair.
if any(line.startswith("+") and "BAD_FORMAT" in line for line in sys.stdin):
    print("fixture formatting violation")
''')
        formatter.chmod(0o755)
        self.env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
                    "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
                    "BASE_SHA": "", "BEFORE_SHA": ""}
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        (self.repo / "core").mkdir()
        (self.repo / "core/test.cpp").write_text("int main() { return 0; }\n")
        self.base = self.commit("initial")
        (self.repo / "core/test.cpp").write_text("int main(){return 1;} // BAD_FORMAT\n")
        self.bad = self.commit("bad formatting in first pushed commit")
        (self.repo / "note.md").write_text("Unrelated final commit\n")
        self.tip = self.commit("unrelated final pushed commit")

    def git(self, *args, cwd=None):
        return subprocess.run(["git", *args], cwd=cwd or self.repo, env=self.env,
                              capture_output=True, text=True, check=True).stdout.strip()

    def commit(self, message):
        self.git("add", ".")
        self.git("commit", "-m", message)
        return self.git("rev-parse", "HEAD")

    def run_helper(self, before="", base="", cwd=None):
        return subprocess.run(["bash", str(ROOT / "tools/analysis/check_cpp_format.sh")],
                              cwd=cwd or self.repo,
                              env={**self.env, "BEFORE_SHA": before, "BASE_SHA": base},
                              capture_output=True, text=True)

    def assert_format_failure(self, result):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("fixture formatting violation", result.stdout)

    def test_entire_push_and_pr_base_precedence(self):
        self.assertEqual(self.run_helper().returncode, 0, "HEAD^ misses the earlier bad commit")
        self.assert_format_failure(self.run_helper(before=self.base))
        self.assert_format_failure(self.run_helper(before=self.bad, base=self.base))

    def test_initial_push_checks_the_entire_tree(self):
        self.assert_format_failure(self.run_helper(before="0" * 40))

    def test_shallow_checkout_fetches_the_event_base(self):
        clone = self.root / "shallow"
        self.git("clone", "--depth=1", self.repo.as_uri(), str(clone))
        self.assert_format_failure(self.run_helper(before=self.base, cwd=clone))
        self.git("cat-file", "-e", self.base, cwd=clone)

    def test_force_push_compares_non_ancestor_tips(self):
        self.git("checkout", "-b", "rewritten", self.base)
        (self.repo / "core/test.cpp").write_text("// BAD_FORMAT\nint main() { return 2; }\n")
        self.commit("replacement history")
        self.assert_format_failure(self.run_helper(before=self.tip))

    def test_unavailable_or_invalid_base_fails_closed(self):
        self.assertNotEqual(self.run_helper(before="f" * 40).returncode, 0)
        self.assertEqual(self.run_helper(before="--invalid").returncode, 2)


if __name__ == "__main__":
    unittest.main()
