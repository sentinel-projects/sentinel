import argparse
import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import platform
import pwd
import tempfile
import threading
import time

root = Path(__file__).resolve().parents[2]
base_daemon = root / "build/release/apps/sentinel-daemon/sentinel-daemon"


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path not in ("/v1/models", "/api/v1/models", "/api/version"):
            self.send_error(404)
            return
        result = {
            "object": "list",
            "data": [{"id": "sentinel-fixture", "object": "model"}],
            "models": [],
        }
        if self.path == "/api/v1/models":
            result["models"] = [
                {
                    "key": "sentinel-fixture",
                    "display_name": "Sentinel fixture",
                    "max_context_length": 8192,
                }
            ]
        if self.path == "/api/version":
            result = {"version": "0.1.0"}
        body = json.dumps(result).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# A dedicated sandbox must deny the actual home, with a read-only exception
# for this checkout and a write exception only for the evidence directory.
real_home = Path(pwd.getpwuid(os.getuid()).pw_dir)
for protected in (
    real_home / "Library",
    real_home / ".sentinel",
    real_home / "Documents",
):
    try:
        protected.stat()
    except PermissionError:
        continue
    raise RuntimeError(f"Refusing to launch: {protected} is not sandbox-denied")

parser = argparse.ArgumentParser(
    description="Isolated macOS initial-window observation"
)
parser.add_argument("window_helper", type=Path)
parser.add_argument(
    "--smoke",
    action="store_true",
    help="One short trial per mode; not performance evidence",
)
parser.add_argument(
    "--panel",
    action="store_true",
    help="Compare connected initial panels using the Qt test fixture",
)
args = parser.parse_args()
window_helper = args.window_helper.resolve(strict=True)
spec = importlib.util.spec_from_file_location(
    "benchmark", root / "tools/performance/benchmark.py"
)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)
sampler = benchmark.Sampler()
qt = (
    root
    / "build/release/apps/sentinel-desktop/sentinel-desktop.app/Contents/MacOS/sentinel-desktop"
)
if args.panel:
    qt = (
        root
        / "build/release/tests/settings_ui_certification_probe.app/Contents/MacOS/settings_ui_certification_probe"
    )
slint = root / "tools/slint-desktop-prototype/target/release/sentinel-slint-prototype"
result = {
    "platform": platform.platform(),
    "scope": (
        "Short harness smoke; not performance evidence"
        if args.smoke
        else "No chat; after 5s startup, 5s idle, window visibility from CoreGraphics; same daemon; prototype feature sets differ"
    ),
    "smoke": args.smoke,
    "panel_fixture": args.panel,
    "qt_fixture_limit": (
        "Hidden production Main plus in-process test controller; not production process cost"
        if args.panel
        else None
    ),
    "trials": [],
}
result["sha256"] = {
    str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
    for p in (qt, slint, base_daemon)
}
with tempfile.TemporaryDirectory(prefix="sentinel-bench-", dir="/tmp") as temporary:
    isolated = Path(temporary)
    endpoint = isolated / "d.sock"
    daemon = isolated / "sentinel-daemon"
    shutil.copy2(base_daemon, daemon)
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    (isolated / "settings.json").write_text(
        json.dumps(
            {
                "sentinelSettingsSchemaVersion": 1,
                "networkMode": "LocalOnly",
                "localChatInferenceEnabled": "true",
                "selectedRuntimeProvider": "lm-studio",
                "selectedLocalModel": "sentinel-fixture",
                "selectedModel.lm-studio": "sentinel-fixture",
                "lmStudioEndpoint": f"http://127.0.0.1:{httpd.server_port}",
                "ollamaEndpoint": f"http://127.0.0.1:{httpd.server_port}",
            }
        )
    )
    env = {key: os.environ[key] for key in ("PATH", "LANG") if key in os.environ}
    env.update(TMPDIR=temporary, SENTINEL_RESOURCE_TELEMETRY="0")
    server = subprocess.Popen(
        [
            str(daemon),
            "--portable",
            "--profile-name",
            isolated.name,
            "--socket",
            str(endpoint),
        ],
        env=env,
        cwd=isolated,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    app = None
    try:
        for _ in range(100):
            try:
                probe = benchmark.Ipc(endpoint)
                probe.close()
                break
            except (OSError, ValueError, AssertionError):
                if server.poll() is not None:
                    raise RuntimeError("Daemon exited before hello")
                time.sleep(0.05)
        else:
            raise RuntimeError("Daemon hello timeout")
        for index in range(1 if args.smoke else 3):
            for kind in ("qt", "slint-femtovg", "slint-software"):
                trial_dir = isolated / (kind + str(index))
                trial_dir.mkdir()
                trial_env = dict(env, TMPDIR=str(trial_dir))
                if kind == "qt" and args.panel:
                    command = [str(qt), "--quick-panel-observation", str(endpoint)]
                elif kind == "qt":
                    command = [
                        str(qt),
                        "--daemon-socket",
                        str(endpoint),
                        "--no-daemon-start",
                        "--preferences-directory",
                        str(trial_dir),
                    ]
                else:
                    command = [str(slint), "--socket", str(endpoint)]
                    trial_env["SLINT_BACKEND"] = "winit-" + kind.split("-", 1)[1]
                    if args.panel:
                        trial_env["SENTINEL_BENCHMARK_READY"] = "1"
                log_path = trial_dir / "client.log"
                log = log_path.open("w+")
                app = subprocess.Popen(
                    command,
                    env=trial_env,
                    cwd=trial_dir,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                try:
                    readiness = None
                    contents = ""
                    if args.panel:
                        deadline = time.monotonic() + 15
                        while True:
                            log.seek(0)
                            contents = log.read()
                            if "PANEL_OBSERVATION_READY" in contents:
                                readiness = next(
                                    line
                                    for line in contents.splitlines()
                                    if line.startswith("PANEL_OBSERVATION_READY")
                                )
                                break
                            if app.poll() is not None or time.monotonic() >= deadline:
                                raise RuntimeError(
                                    f"{kind} readiness failed: {contents[-2000:]}"
                                )
                            time.sleep(0.05)
                    time.sleep(3 if args.smoke else 5)
                    if app.poll() is not None:
                        raise RuntimeError(f"{kind} exited {app.returncode}")
                    deadline = time.monotonic() + 10
                    while True:
                        windows = json.loads(
                            subprocess.check_output([str(window_helper), str(app.pid)])
                        )
                        matched = bool(windows) and (
                            not args.panel
                            or any(
                                w.get("bounds", {}).get("Width") == 580
                                and w.get("bounds", {}).get("Height") == 442
                                for w in windows
                            )
                        )
                        if matched:
                            break
                        if app.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError(
                                f"{kind} window geometry not observed: {windows}; log: {contents[-2000:]}"
                            )
                        time.sleep(0.25)
                    samples, dsamples = [], []
                    for _ in range(2 if args.smoke else 21):
                        samples.append(sampler.sample(app.pid))
                        dsamples.append(sampler.sample(server.pid))
                        time.sleep(0.25)
                    result["trials"].append(
                        {
                            "kind": kind,
                            "trial": index + 1,
                            "pid": app.pid,
                            "windows": windows,
                            "readiness_marker": readiness,
                            "samples": samples,
                            "daemon_samples": dsamples,
                            "summary": benchmark.summary(samples),
                            "daemon_summary": benchmark.summary(dsamples),
                        }
                    )
                    print(
                        kind,
                        index + 1,
                        "windows",
                        len(windows),
                        "footprint",
                        benchmark.summary(samples)["footprint_bytes"]["median"],
                        flush=True,
                    )
                finally:
                    if app.poll() is None:
                        app.terminate()
                        app.wait(timeout=5)
                    app = None
                    log.close()
    finally:
        if app and app.poll() is None:
            app.terminate()
            app.wait(timeout=5)
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=10)
        httpd.shutdown()
        httpd.server_close()
evidence_dir = root / "build/slint-phase1"
evidence_dir.mkdir(parents=True, exist_ok=True)
with tempfile.NamedTemporaryFile(
    mode="w",
    prefix=(
        ("macos-panel-smoke-" if args.smoke else "macos-panel-")
        if args.panel
        else ("macos-smoke-" if args.smoke else "macos-comparison-")
    ),
    suffix=".json",
    dir=evidence_dir,
    delete=False,
) as receipt:
    json.dump(result, receipt, indent=2)
    receipt.write("\n")
out = Path(receipt.name)
print(out, flush=True)
