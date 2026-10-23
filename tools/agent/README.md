# Portable live coding-agent acceptance test

Run `live_coding.py` from any directory with Python 3.10+, Git, a built/deployed Sentinel daemon, and an installed model exposed by a local server. `--provider` accepts `lm-studio`
(the default), `ollama` or `llama-cpp-server`; `--endpoint` defaults to that provider's
standard loopback address. The selected provider identity is preserved throughout discovery
and generation. This is a test helper, not a Python application backend. It does not load/download models or change the real Sentinel profile.

```sh
python tools/agent/live_coding.py --daemon build/tests/apps/sentinel-daemon/sentinel-daemon --model qwen/qwen3-4b-2507
```

Use the executable location produced by your build. Windows executables have an `.exe` suffix and multi-configuration builds may place them under `Debug` or `Release`. For example, in PowerShell:

```powershell
python tools/agent/live_coding.py --daemon build/tests/apps/sentinel-daemon/Debug/sentinel-daemon.exe --model qwen/qwen3-4b-2507 --test-command "py -3 test_calc.py"
```

On Windows the test environment also needs PySide6 for Qt's named-pipe IPC; macOS/Linux use Python's Unix sockets. Dependencies are never installed automatically. Use `--runtime-dir` with a deployed daemon directory when runtime libraries/plugins must accompany the executable. Keep the appropriate Qt runtime/DLL environment configured. Python must also be available inside Sentinel's process sandbox for the chosen `--test-command`.

The default output directory and private IPC directory come from the OS temporary-directory API, with short Unix socket paths. `--output-dir` must name a new directory. `--endpoint` accepts loopback addresses only. `--inspect-only` exercises read-only acceptance; `--timeout` bounds each case's wait. Run `--help` for parameters.

The test stages an isolated daemon and creates a disposable project. It approves reads only in that project, edits only to `calc.py`, and only the exact specified test command. Test and unrelated files must remain unchanged. A successful independent test alone does not pass acceptance: every agent case must terminate as `run.completed`. Failure or a loop returns a nonzero exit code.

`live-qwen-result.json` replaces generated project, runtime and IPC paths with logical placeholders before writing. Raw `daemon.log` and the staged runtime/profile are local debugging artifacts; do not publish them without inspection. No real user files are supplied to the model. Output directories remain available for inspection; remove them using your normal file-management workflow when finished.

Portability of this helper is separate from platform support of the runtime tools. macOS execution has been tested; Linux and Windows native acceptance remains to be run. Unsupported sandbox/tool dependencies must fail visibly rather than silently bypass isolation.

Before publishing source/evidence, run:

```sh
python tools/analysis/check_personal_paths.py
```

This scans Git-tracked and nonignored untracked files, including binary debug paths, for concrete user-home paths. It is a focused path check, not a full secrets/privacy audit. Generic placeholders and standard system prefixes remain valid.

The independent verifier accepts only the fixture arithmetic AST, reconstructs it without comments or encoding cookies, and runs the trusted baseline test in a separate temporary directory. Unexpected model code is refused rather than executed outside the daemon sandbox. Fixture snapshots reject symlinks and do not dump unexpected files. Acceptance also checks that no other fixture source file changed.

For a narrow mutation/read-back completion check, use `--completion-only`. It
changes only the disposable `calc.py`, then independently checks its permitted AST
and runs the trusted baseline test. The run must still produce `run.completed`.
Example for an already running llama.cpp server:

```sh
python tools/agent/live_coding.py --daemon build/tests/apps/sentinel-daemon/sentinel-daemon --provider llama-cpp-server --model <loaded-model-id> --completion-only --timeout 300
```
