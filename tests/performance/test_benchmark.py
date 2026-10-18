"""Counter arithmetic, provenance, bounded ownership and native sampler checks."""

import ctypes
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "benchmark", ROOT / "tools/performance/benchmark.py"
)
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


class BenchmarkTest(unittest.TestCase):
    def test_cpu_normalization(self):
        a = dict(start=1, monotonic=10, cpu_seconds=1)
        z = dict(start=1, monotonic=12, cpu_seconds=2)
        self.assertEqual(b.cpu_percent(a, z), 50)
        self.assertEqual(b.cpu_percent(a, z, 8), 6.25)
        for field, value in [("start", 2), ("monotonic", 10), ("cpu_seconds", 0)]:
            with self.assertRaises(ValueError):
                b.cpu_percent(a, dict(z, **{field: value}))

    def test_small_samples_do_not_invent_p95(self):
        self.assertIsNone(b.stats([])["median"])
        self.assertIsNone(b.stats([1, 2, 3])["p95"])
        self.assertEqual(b.stats(list(range(1, 21)))["p95"], 19)
        self.assertEqual(b.stats([1, 2, 3])["median"], 2)

    def test_abi(self):
        self.assertEqual(ctypes.sizeof(b.Usage), 160)
        self.assertEqual(ctypes.sizeof(b.Task), 96)

    def test_pid_reuse_rejected(self):
        child = object.__new__(b.Owned)
        child.identity = 100
        child.process = Mock(pid=123)
        child.process.poll.return_value = None
        child.sampler = Mock()
        child.sampler.sample.return_value = dict(start=200)
        with self.assertRaises(RuntimeError):
            child.sample()
        child.process.send_signal.assert_not_called()

    def test_cleanup_only_direct_child(self):
        child = object.__new__(b.Owned)
        child.process = Mock()
        child.process.poll.return_value = None
        child.process.wait.side_effect = [subprocess.TimeoutExpired("owned", 8), 0]
        child.log = Mock()
        child.stop()
        child.process.send_signal.assert_called_once_with(b.signal.SIGTERM)
        child.process.kill.assert_called_once_with()
        child.log.close.assert_called_once()

    def test_environment_drops_credentials(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = b.environment(Path(tmp))
            self.assertEqual(env["HOME"], tmp)
            self.assertNotIn("OPENAI_API_KEY", env)
            self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")

    @unittest.skipUnless(platform.system() == "Darwin", "Darwin sampler only")
    def test_native_accuracy_and_missing_pid(self):
        s = b.Sampler()
        first = s.sample(os.getpid())
        # Real touched anonymous allocation, independently known byte count.
        allocation = bytearray(16 * 1024 * 1024)
        for i in range(0, len(allocation), 4096):
            allocation[i] = 1
        second = s.sample(os.getpid())
        self.assertGreater(second["rss_bytes"], first["rss_bytes"])
        self.assertGreaterEqual(second["cpu_seconds"], first["cpu_seconds"])
        self.assertEqual(first["start"], second["start"])
        self.assertGreater(second["threads"], 0)
        with self.assertRaises(OSError):
            s.sample(2147483647)

    @unittest.skipUnless(platform.system() == "Darwin", "Darwin sampler only")
    def test_native_cpu_units_against_process_clock(self):
        sampler = b.Sampler()
        first = sampler.sample(os.getpid())
        start = time.process_time()
        deadline = time.monotonic() + 0.1
        while time.monotonic() < deadline:
            pass
        last = sampler.sample(os.getpid())
        independent = time.process_time() - start
        observed = last["cpu_seconds"] - first["cpu_seconds"]
        # Accuracy tolerance, not a product CPU/timing performance budget.
        self.assertAlmostEqual(observed, independent, delta=0.015)

    def test_ipc_failure_paths(self):
        client = object.__new__(b.Ipc)
        client.sequence = 0
        client.s = Mock()
        for frame in [
            b"",
            b"x" * (1024 * 1024 + 1),
            json.dumps(
                dict(id="1", type="error", name="daemon.status", version=dict(major=1))
            ).encode()
            + b"\n",
        ]:
            client.sequence = 0
            client.f = io.BytesIO(frame)
            with self.assertRaises(ValueError):
                client.request("daemon.status", {})

    def test_short_lived_child_not_signalled(self):
        child = object.__new__(b.Owned)
        child.process = Mock()
        child.process.poll.return_value = 0
        child.log = Mock()
        child.stop()
        child.process.send_signal.assert_not_called()
        child.process.kill.assert_not_called()

    def test_verify_rejects_outside_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                b.verify(Path(tmp), ("0" * 40, False, None, "main"))

    def test_verifier_rejects_stale_hash_and_failed_collection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = root / "build/tests/resource-benchmark/run-test"
            directory.mkdir(parents=True)
            exe = root / "binary"
            exe.write_bytes(b"test binary")
            artifact = directory / "result.json"
            artifact.write_text("{}")
            identity = ("0" * 40, False, None, "main")
            data = dict(
                source_sha=identity[0],
                dirty=False,
                dirty_digest=None,
                branch="main",
                artifacts={"result.json": b.sha(artifact)},
                results=[
                    dict(
                        binary=dict(path="binary", sha256=b.sha(exe)),
                        outcome="Failed",
                        kind="daemon",
                        profile_removed=True,
                        owned_children_reaped=True,
                    )
                ],
            )
            with patch.object(b, "ROOT", root):
                for changed in [
                    dict(data, branch="other"),
                    dict(data, artifacts={"../escape": "0" * 64}),
                    dict(data, artifacts={"result.json": "0" * 64}),
                    data,
                ]:
                    (directory / "results.json").write_text(json.dumps(changed))
                    with self.assertRaises((ValueError, OSError)):
                        b.verify(directory, identity)

    def test_summary_missing_values(self):
        self.assertIsNone(b.summary([])["rss_growth_bytes"])
        self.assertIsNone(b.summary([])["rss_bytes"]["peak"])


if __name__ == "__main__":
    unittest.main()
