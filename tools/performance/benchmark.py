#!/usr/bin/env python3
"""Bounded P4-B01 observations. No resource policy or arbitrary command execution."""

import argparse
import ctypes as C
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/verification"))
from validate import git_identity, schema_errors  # noqa: E402
from e2e import redact  # noqa: E402
from desktop_daemon import parse_markers, validate_rows  # noqa: E402


class Usage(C.Structure):
    # Darwin SDK sys/resource.h rusage_info_v2, uint64 values except UUID.
    _fields_ = [("uuid", C.c_uint8 * 16)] + [
        (n, C.c_uint64)
        for n in (
            "user",
            "system",
            "idle_wakeups",
            "interrupt_wakeups",
            "pageins",
            "wired",
            "rss",
            "footprint",
            "start",
            "exit",
            "child_user",
            "child_system",
            "child_idle",
            "child_interrupt",
            "child_pageins",
            "child_elapsed",
            "read",
            "write",
        )
    ]


class Task(C.Structure):
    _fields_ = [
        (n, C.c_uint64)
        for n in ("virtual", "rss", "user", "system", "threads_user", "threads_system")
    ] + [
        (n, C.c_int32)
        for n in (
            "policy",
            "faults",
            "pageins",
            "cow",
            "sent",
            "received",
            "mach",
            "unix",
            "switches",
            "threads",
            "running",
            "priority",
        )
    ]


class Timebase(C.Structure):
    _fields_ = [("numer", C.c_uint32), ("denom", C.c_uint32)]


class Sampler:
    def __init__(self):
        if platform.system() != "Darwin":
            raise RuntimeError("Native execution currently supports Darwin only")
        clock = C.CDLL("/usr/lib/libSystem.B.dylib")
        tb = Timebase()
        if clock.mach_timebase_info(C.byref(tb)) != 0 or tb.denom == 0:
            raise RuntimeError("Mach timebase unavailable")
        self.seconds_per_tick = tb.numer / tb.denom / 1e9
        self.lib = C.CDLL("/usr/lib/libproc.dylib", use_errno=True)
        self.lib.proc_pid_rusage.argtypes = [C.c_int, C.c_int, C.c_void_p]
        self.lib.proc_pidinfo.argtypes = [
            C.c_int,
            C.c_int,
            C.c_uint64,
            C.c_void_p,
            C.c_int,
        ]

    def sample(self, pid):
        u, t = Usage(), Task()
        if self.lib.proc_pid_rusage(pid, 2, C.byref(u)) != 0:
            raise OSError(C.get_errno(), "proc_pid_rusage unavailable")
        threads = None
        if self.lib.proc_pidinfo(pid, 4, 0, C.byref(t), C.sizeof(t)) == C.sizeof(t):
            threads = t.threads
        return dict(
            pid=pid,
            start=u.start,
            monotonic=time.monotonic(),
            rss_bytes=u.rss,
            footprint_bytes=u.footprint,
            cpu_seconds=(u.user + u.system) * self.seconds_per_tick,
            threads=threads,
            idle_wakeups=u.idle_wakeups,
            interrupt_wakeups=u.interrupt_wakeups,
            read_bytes=u.read,
            write_bytes=u.write,
        )


def cpu_percent(before, after, cores=1):
    elapsed = after["monotonic"] - before["monotonic"]
    delta = after["cpu_seconds"] - before["cpu_seconds"]
    if after["start"] != before["start"] or elapsed <= 0 or delta < 0 or cores < 1:
        raise ValueError("Invalid process identity/counter interval")
    return 100 * delta / elapsed / cores


def stats(values):
    if not values:
        return dict(n=0, median=None, p95=None, peak=None, stdev=None)
    return dict(
        n=len(values),
        median=statistics.median(values),
        p95=(
            sorted(values)[math.ceil(0.95 * len(values)) - 1]
            if len(values) >= 20
            else None
        ),
        peak=max(values),
        stdev=statistics.stdev(values) if len(values) >= 2 else None,
    )


def summary(samples):
    cpus = [cpu_percent(a, b) for a, b in zip(samples, samples[1:])]
    return dict(
        rss_bytes=stats([s["rss_bytes"] for s in samples]),
        footprint_bytes=stats([s["footprint_bytes"] for s in samples]),
        cpu_one_core_percent=stats(cpus),
        cpu_host_percent=stats([v / (os.cpu_count() or 1) for v in cpus]),
        threads_peak=max(
            (s["threads"] for s in samples if s["threads"] is not None), default=None
        ),
        rss_growth_bytes=(
            samples[-1]["rss_bytes"] - samples[0]["rss_bytes"] if samples else None
        ),
        counter_deltas=(
            {
                k: samples[-1][k] - samples[0][k]
                for k in (
                    "cpu_seconds",
                    "read_bytes",
                    "write_bytes",
                    "idle_wakeups",
                    "interrupt_wakeups",
                )
            }
            if len(samples) >= 2
            else {}
        ),
    )


def sha(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def command(args):
    try:
        p = subprocess.run(args, capture_output=True, timeout=5, text=True)
        return p.stdout.strip() if p.returncode == 0 else "Unavailable"
    except (OSError, subprocess.TimeoutExpired):
        return "Unavailable"


def conditions():
    return dict(
        monotonic=time.monotonic(),
        load_average=os.getloadavg(),
        power=command(["pmset", "-g", "batt"]),
        thermal=command(["pmset", "-g", "therm"]),
    )


def environment(profile):
    e = {k: os.environ[k] for k in ("PATH",) if k in os.environ}
    e.update(
        {k: str(profile) for k in ("HOME", "USERPROFILE", "TMPDIR", "TMP", "TEMP")}
    )
    e.update(
        XDG_DATA_HOME=str(profile / "data"),
        XDG_CONFIG_HOME=str(profile / "config"),
        XDG_CACHE_HOME=str(profile / "cache"),
        QT_QPA_PLATFORM="offscreen",
        QT_QUICK_BACKEND="software",
        QSG_RENDER_LOOP="basic",
        QT_SCALE_FACTOR="1",
        QT_FONT_DPI="96",
    )
    return e


class Owned:
    """Only this Popen child may be signalled; never discover or kill external servers."""

    def __init__(self, args, env, profile, log, sampler):
        self.log = log.open("wb")
        self.process = subprocess.Popen(
            args,
            env=env,
            cwd=profile,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self.sampler = sampler
        self.identity = None
        try:
            self.identity = sampler.sample(self.process.pid)["start"]
        except OSError:
            pass  # short-lived commands can exit before the first observation

    def stop(self):
        p = self.process
        if p.poll() is None:
            # An unreaped direct Popen child cannot have its PID reused.
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=8)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=3)
        self.log.close()

    def sample(self):
        if self.process.poll() is not None:
            return None
        row = self.sampler.sample(self.process.pid)
        if self.identity is None:
            self.identity = row["start"]
        if row["start"] != self.identity:
            raise RuntimeError("Process identity changed")
        return row


class Ipc:
    def __init__(self, endpoint):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(2)
        self.s.connect(str(endpoint))
        self.f = self.s.makefile("rb")
        self.sequence = 0
        self.request(
            "hello",
            dict(client_id="sentinel-p4-b01", major=1, minor=0, capabilities=[]),
        )

    def request(self, name, payload):
        self.sequence += 1
        ident = str(self.sequence)
        start = time.monotonic()
        data = (
            json.dumps(
                dict(
                    version=dict(major=1, minor=0),
                    type="request",
                    id=ident,
                    name=name,
                    payload=payload,
                )
            ).encode()
            + b"\n"
        )
        self.s.sendall(data)
        for _ in range(128):
            raw = self.f.readline(1024 * 1024 + 1)
            if not raw or len(raw) > 1024 * 1024:
                raise ValueError("Incomplete/oversized IPC frame")
            r = json.loads(raw)
            if r.get("id") == ident and r.get("type") in ("response", "error"):
                if (
                    r["type"] != "response"
                    or r.get("name") != name
                    or r.get("version", {}).get("major") != 1
                ):
                    raise ValueError("IPC rejected or mismatched")
                return r["payload"], time.monotonic() - start, len(data) + len(raw)
        raise ValueError("No matching response")

    def close(self):
        self.f.close()
        self.s.close()


def binary(name):
    if name == "daemon-fixture":
        return ROOT / "build/tests/tests/resource_benchmark_probe"
    if name == "rust":
        return ROOT / "cli/target/debug/sentinel"
    if name == "fixture":
        return (
            ROOT
            / "build/tests/tests/settings_ui_certification_probe.app/Contents/MacOS/settings_ui_certification_probe"
        )
    if name in ("sqlite", "retrieval", "telemetry"):
        return (
            ROOT
            / "build/tests/tests"
            / {
                "sqlite": "bench_sqlite_store",
                "retrieval": "bench_semantic_search",
                "telemetry": "bench_resource_telemetry",
            }[name]
        )
    return ROOT / "build/tests/apps" / name / name


def trial(kind, number, duration, interval, directory, sampler):
    out = directory / f"{kind}-{number}"
    out.mkdir()
    result = dict(
        kind=kind,
        trial=number,
        outcome="Failed",
        samples=[],
        conditions_before=conditions(),
    )
    with tempfile.TemporaryDirectory(prefix="sb1-", dir="/tmp") as tmp:
        profile = Path(tmp)
        env = environment(profile)
        owned, clients = [], []
        try:
            args = []
            if kind in ("daemon", "telemetry-daemon", "clients"):
                env["SENTINEL_RESOURCE_TELEMETRY"] = (
                    "1" if kind == "telemetry-daemon" else "0"
                )
                args = [
                    str(binary("daemon-fixture")),
                    str(profile / "d.sock"),
                    "--isolated-benchmark",
                ]
            else:
                target = dict(
                    fixture="fixture",
                    rust="rust",
                    legacy="sentinel-cli",
                    sqlite="sqlite",
                    retrieval="retrieval",
                    telemetry="telemetry",
                )[kind]
                args = [str(binary(target))]
                if kind == "fixture":
                    args += ["--daemon-e2e"]
                elif kind == "rust":
                    args += ["--version"]
                elif kind in ("sqlite", "retrieval"):
                    args += ["-iterations", "1"]
            exe = Path(args[0])
            result["binary"] = dict(path=str(exe.relative_to(ROOT)), sha256=sha(exe))
            baseline = sampler.sample(os.getpid())
            result["sampler_baseline"] = baseline
            start = time.monotonic()
            child = Owned(args, env, profile, out / "workload.log", sampler)
            owned.append(child)
            result["process"] = dict(
                pid=child.process.pid,
                provenance="direct test-owned Popen child",
                attribution=(
                    "combined Desktop+in-process daemon fixture"
                    if kind == "fixture"
                    else (
                        "credential-isolated daemon/application fixture"
                        if kind in ("daemon", "telemetry-daemon", "clients")
                        else kind
                    )
                ),
            )
            if kind in ("daemon", "telemetry-daemon", "clients"):
                deadline = start + 10
                while True:
                    row = child.sample()
                    if row:
                        result["samples"].append(row)
                    try:
                        client = Ipc(profile / "d.sock")
                        payload, latency, _ = client.request("daemon.status", {})
                        if payload.get("running") is not True:
                            raise ValueError("Daemon not running")
                        clients.append(client)
                        break
                    except (OSError, ValueError):
                        if (
                            time.monotonic() >= deadline
                            or child.process.poll() is not None
                        ):
                            raise RuntimeError("Daemon readiness failed")
                        time.sleep(0.02)
                result["readiness_seconds"] = time.monotonic() - start
                if kind == "clients":
                    clients.extend(Ipc(profile / "d.sock") for _ in range(3))
                result["ipc_latencies_seconds"] = []
                byte_count = 0
                ipc_start = time.monotonic()
                for _ in range(10):
                    for c in clients:
                        payload, latency, size = c.request("daemon.status", {})
                        if payload.get("running") is not True:
                            raise ValueError("Wrong daemon state")
                        result["ipc_latencies_seconds"].append(latency)
                        byte_count += size
                result["ipc_bytes_per_second"] = byte_count / (
                    time.monotonic() - ipc_start
                )
                result["connected_clients"] = len(clients)
                result["idle_sample_start"] = len(result["samples"])
                end = time.monotonic() + duration
            else:
                end = start + 65
            while time.monotonic() < end and child.process.poll() is None:
                if (out / "workload.log").stat().st_size > 1024 * 1024:
                    raise RuntimeError("Workload log cap exceeded")
                row = child.sample()
                if row:
                    result["samples"].append(row)
                time.sleep(interval)
            if (
                kind not in ("daemon", "telemetry-daemon", "clients")
                and child.process.poll() is None
            ):
                raise TimeoutError("Bounded workload timed out")
            result["execution_seconds"] = time.monotonic() - start
            if kind == "fixture":
                rows, _, _ = parse_markers(
                    (out / "workload.log").read_text(errors="replace")
                )
                validate_rows(rows)
                result["oracles"] = rows
            if (
                kind not in ("daemon", "telemetry-daemon", "clients")
                and child.process.returncode != 0
            ):
                raise RuntimeError("Workload failed")
            result["outcome"] = "Measured"
        except (OSError, ValueError, RuntimeError, TimeoutError) as error:
            result["error"] = redact(
                type(error).__name__ + ": " + str(error), [ROOT, Path.home(), profile]
            )
        finally:
            for c in clients:
                c.close()
            stop = time.monotonic()
            for p in reversed(owned):
                p.stop()
            result["shutdown_seconds"] = time.monotonic() - stop
            result["owned_children_reaped"] = all(
                p.process.poll() is not None for p in owned
            )
            result["exit_codes"] = [p.process.returncode for p in owned]
            if kind in ("daemon", "telemetry-daemon", "clients"):
                diagnostic = (out / "workload.log").read_text(errors="replace")
                result["offline_profile_verified"] = (
                    "B01_ISOLATION in-memory-credentials offline empty-catalog no-selected-model"
                    in diagnostic
                )
                result["native_profile_removed"] = "B01_CLEANUP complete" in diagnostic
                if (
                    not result["offline_profile_verified"]
                    or not result["native_profile_removed"]
                    or result["exit_codes"] != [0]
                ):
                    result["outcome"] = "Failed"
                    result["error"] = (
                        "Private composition isolation/cleanup not confirmed"
                    )
            result["profile_bytes_before_cleanup"] = sum(
                p.stat().st_size for p in profile.rglob("*") if p.is_file()
            )
            result["sampler_after"] = sampler.sample(os.getpid())
            result["conditions_after"] = conditions()
            log = out / "workload.log"
            if log.exists():
                result["log_truncated"] = log.stat().st_size > 65536
                log.write_text(
                    redact(
                        log.read_text(errors="replace")[:65536],
                        [ROOT, Path.home(), profile],
                    )
                )
    result["profile_removed"] = not profile.exists()
    result["summary"] = summary(result["samples"])
    result["idle_summary"] = summary(
        result["samples"][result.get("idle_sample_start", 0) :]
    )
    result["ipc_latency_summary_seconds"] = stats(
        result.get("ipc_latencies_seconds", [])
    )
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def verify(directory, identity):
    directory = directory.resolve()
    allowed = (ROOT / "build/tests/resource-benchmark").resolve()
    if directory.parent != allowed:
        raise ValueError("Evidence directory outside benchmark root")
    receipt = json.loads((directory / "results.json").read_text())
    actual = (
        receipt["source_sha"],
        receipt["dirty"],
        receipt["dirty_digest"],
        receipt["branch"],
    )
    if actual != tuple(identity):
        raise ValueError("Stale source identity")
    if not receipt.get("artifacts") or not receipt.get("results"):
        raise ValueError("Incomplete collection")
    for name, expected in receipt["artifacts"].items():
        path = (directory / name).resolve()
        if not path.is_relative_to(directory) or sha(path) != expected:
            raise ValueError("Artifact hash/path mismatch")
    allowed_binaries = {
        binary(k).resolve()
        for k in (
            "rust",
            "fixture",
            "sqlite",
            "retrieval",
            "telemetry",
            "sentinel-cli",
            "daemon-fixture",
        )
    }
    for row in receipt["results"]:
        candidate = (ROOT / row["binary"]["path"]).resolve()
        if candidate not in allowed_binaries:
            raise ValueError("Binary outside fixed workload allowlist")
        if sha(candidate) != row["binary"]["sha256"]:
            raise ValueError("Binary changed")
        if (
            row["outcome"] != "Measured"
            or not row["profile_removed"]
            or not row["owned_children_reaped"]
        ):
            raise ValueError("Failed/incomplete scenario")
        if row["kind"] in ("daemon", "telemetry-daemon", "clients") and not (
            row.get("offline_profile_verified") and row.get("native_profile_removed")
        ):
            raise ValueError("Unverified isolation")
        if row["kind"] == "fixture":
            validate_rows(row["oracles"])
    qa = json.loads((directory / "receipt.json").read_text())
    schema = json.loads((ROOT / "verification/schema.json").read_text())
    if (
        schema_errors(qa, schema["$defs"]["evidence"], schema)
        or qa["outcome"] != "pass"
    ):
        raise ValueError("Invalid QA companion receipt")
    if (qa["source_sha"], qa["dirty"], qa["dirty_digest"], qa["branch"]) != tuple(
        identity
    ):
        raise ValueError("Stale QA companion")
    if qa["artifacts"] != [
        dict(
            path=str((directory / "results.json").relative_to(ROOT)),
            sha256=sha(directory / "results.json"),
        )
    ]:
        raise ValueError("QA results hash mismatch")
    return receipt


def qa_receipt(directory, identity, success):
    schema = json.loads((ROOT / "verification/schema.json").read_text())
    receipt = dict(
        id="P4-B01-" + directory.name,
        component="sentinel.runtime",
        source_sha=identity[0],
        dirty=identity[1],
        dirty_digest=identity[2],
        branch=identity[3],
        scenario="P4-B01-MEASUREMENT",
        platform="macos",
        architecture=platform.machine(),
        os_version=platform.platform(),
        toolchain=command(["c++", "--version"])[:512],
        build_profile="tests/Debug/offscreen/software; Rust debug",
        timestamp=dt.datetime.now(dt.timezone.utc).isoformat(),
        outcome="pass" if success else "fail",
        level="integration",
        runtime_environment="isolated-local",
        artifacts=[
            dict(
                path=str((directory / "results.json").relative_to(ROOT)),
                sha256=sha(directory / "results.json"),
            )
        ],
        scope=["FSV-IPC"],
        component_versions={
            "sentinel.runtime": identity[0],
            "sentinel.protocol": identity[0],
        },
        limitations=[
            "Measurement collection only; no product performance acceptance or numeric budget",
            "Debug short-window macOS observations; no native Fedora/Windows or real-model certification",
            "Combined fixture cost cannot be attributed independently to Desktop and daemon",
        ],
    )
    errors = schema_errors(receipt, schema["$defs"]["evidence"], schema)
    if errors:
        raise ValueError(errors)
    (directory / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument(
        "--duration",
        type=float,
        default=10,
        help="Idle seconds per daemon trial (1..900)",
    )
    p.add_argument(
        "--interval", type=float, default=0.1, help="Sample seconds (.05..5)"
    )
    p.add_argument("--trials", type=int, default=3)
    p.add_argument(
        "--soak", choices=["15m", "60m"], help="Explicit opt-in; daemon only"
    )
    p.add_argument(
        "--scenario",
        choices=[
            "daemon",
            "telemetry-daemon",
            "clients",
            "fixture",
            "rust",
            "legacy",
            "sqlite",
            "retrieval",
            "telemetry",
            "all",
        ],
        default="all",
    )
    p.add_argument("--verify", type=Path)
    p.add_argument(
        "--run-name", help="New run-* evidence directory name; never overwritten"
    )
    a = p.parse_args()
    if a.verify:
        verify(a.verify, git_identity(ROOT))
        print("Verified source, binaries, artifacts, isolation and oracles")
        return 0
    if (
        not 1 <= a.duration <= 900
        or not 0.05 <= a.interval <= 5
        or not 1 <= a.trials <= 5
    ):
        p.error("Invalid bounded configuration")
    if platform.system() != "Darwin":
        p.error(
            "Blocked: native sampler not implemented on this platform; see methodology"
        )
    if not (ROOT / "build/tests/CMakeCache.txt").is_file():
        p.error("Reuse existing tests build")
    sampler = Sampler()
    identity = git_identity(ROOT)
    base = ROOT / "build/tests/resource-benchmark"
    base.mkdir(exist_ok=True)
    if a.run_name:
        if not re.fullmatch(r"run-[A-Za-z0-9_-]{1,48}", a.run_name):
            p.error("Invalid evidence name")
        directory = base / a.run_name
        directory.mkdir()  # Never overwrite existing evidence.
    else:
        directory = Path(tempfile.mkdtemp(prefix="run-", dir=base))
    before = sampler.sample(os.getpid())
    timings = []
    for _ in range(100):
        tick = time.monotonic()
        sampler.sample(os.getpid())
        timings.append(time.monotonic() - tick)
    after = sampler.sample(os.getpid())
    receipt = dict(
        source_sha=identity[0],
        dirty=identity[1],
        dirty_digest=identity[2],
        branch=identity[3],
        timestamp=time.time(),
        platform=platform.platform(),
        architecture=platform.machine(),
        hardware=command(
            [
                "sysctl",
                "-n",
                "hw.model",
                "hw.memsize",
                "hw.logicalcpu",
                "machdep.cpu.brand_string",
            ]
        ),
        build_profile="tests/Debug/offscreen/software; Rust debug",
        topology="Private test-only daemon/application composition; real DaemonService executable is Blocked by shared credential backend",
        cpu_normalization="100% = one logical core; host percent divides by logical CPUs",
        unavailable=dict(
            gpu="Unknown: no per-process GPU counters",
            energy="Unavailable: no privileged instrumentation",
            model="Blocked: no explicitly authorized installed model",
            peak_rss="Sampled peak only; lifetime peak unavailable",
        ),
        sampler_cost_seconds=stats(timings),
        mach_seconds_per_tick=sampler.seconds_per_tick,
        sampler_cpu_seconds=after["cpu_seconds"] - before["cpu_seconds"],
        sampler_footprint_bytes=after["footprint_bytes"],
        results=[],
    )
    kinds = (
        ["daemon"]
        if a.soak
        else (
            [
                "daemon",
                "telemetry-daemon",
                "clients",
                "fixture",
                "rust",
                "legacy",
                "sqlite",
                "retrieval",
                "telemetry",
            ]
            if a.scenario == "all"
            else [a.scenario]
        )
    )
    duration = {"15m": 900, "60m": 3600}.get(a.soak, a.duration)
    for kind in kinds:
        for n in range(a.trials):
            if git_identity(ROOT) != identity:
                raise RuntimeError("Git identity changed; stopped safely")
            result = trial(kind, n + 1, duration, a.interval, directory, sampler)
            receipt["results"].append(result)
            (directory / "results.json").write_text(
                json.dumps(receipt, indent=2) + "\n"
            )
            print(kind, n + 1, result["outcome"], flush=True)
    if git_identity(ROOT) != identity:
        raise RuntimeError("Git identity changed; evidence stale")
    receipt["artifacts"] = {
        str(f.relative_to(directory)): sha(f)
        for f in sorted(directory.rglob("*"))
        if f.is_file() and f.name != "results.json"
    }
    (directory / "results.json").write_text(json.dumps(receipt, indent=2) + "\n")
    qa_receipt(
        directory, identity, all(r["outcome"] == "Measured" for r in receipt["results"])
    )
    print(directory.relative_to(ROOT), flush=True)
    return int(any(r["outcome"] != "Measured" for r in receipt["results"]))


if __name__ == "__main__":
    sys.exit(main())
