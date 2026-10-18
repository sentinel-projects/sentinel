# Resource benchmark methodology (P4-B01)

Measurement only. No CPU/RAM/GPU budget, adaptive admission, unloading, ownership lease,
permission change or production acceptance is established. PD-D03/D04/D10 remain pending.

## Reproduce

Use the existing checkout and `build/tests` / `cli/target`. Read AGENTS, verify clean reconciled
main and milestone integration, record source identity, free disk and exclusive writing access.
Build first; do not benchmark concurrently with compilation or test suites. No credentials,
model downloads, unrelated daemon connections or externally launched model servers are used.

```sh
CCACHE_DISABLE=1 cmake --preset tests
CCACHE_DISABLE=1 cmake --build --preset tests
cargo build --manifest-path cli/Cargo.toml --locked --offline
python3 tests/performance/test_benchmark.py
python3 tools/performance/benchmark.py --scenario all --duration 10 --interval 0.1 --trials 3
python3 tools/performance/benchmark.py --verify build/tests/resource-benchmark/run-REPLACE
```

Native measurement execution currently supports macOS only. Missing platform instrumentation fails explicitly;
it does not emit zero measurements. Existing CMake tests and Rust Debug executables are reused;
Release benchmarks need a separately approved rebuild in an already existing Release tree and
matching provenance. Do not compare these Debug observations as a release regression verdict.

`--scenario` selects a fixed allowlist, never an arbitrary command. Available scenarios are daemon,
telemetry-daemon, clients, fixture, rust, legacy, sqlite, retrieval and telemetry. Trials are bounded
1–5; default 3. Daemon idle duration is 1–900 seconds, interval .05–5 seconds. Readiness has a 10s
bound and commands a 65s bound. SIGTERM waits 8s, then only the unreaped direct child may be killed
and reaped within 3s. No process-name matching or external-server signalling occurs. These test
safety bounds are not product performance budgets. Logs trigger failure above 1MiB at sample
checks and are redacted/capped to 64KiB after exit; allocation between checks can exceed that cap.
Artifacts are bounded by fixed scenarios/trials, no model assets, and ignored existing build output.

Extended modes are explicit and daemon-only:

```sh
# 15-minute idle window per repeat; use --trials 1 for a single window.
python3 tools/performance/benchmark.py --soak 15m --interval 1 --trials 1
# 60-minute run requires this explicit opt-in; document power/environment before execution.
python3 tools/performance/benchmark.py --soak 60m --interval 1 --trials 1
```

Neither soak is automatically executed. Active fixture soak is not implemented; repeat bounded
fixture trials instead. A future active soak needs paced workload scheduling and independent
per-iteration completion/cleanup oracles before it can be reported as measured.

## Isolation and attribution

The production DaemonService creates AppSettings with the platform credential backend and reads
web-search credentials during initialization. Profile/HOME isolation does not isolate macOS
Keychain service IDs. Its executable is therefore **Blocked** under the no-user-credential rule.
Early exploratory executable runs are retained but excluded from accepted measurements; no secret
value was inspected or logged. Do not run that path again to benchmark a disposable profile.

`resource_benchmark_probe` is a private test executable, not a production override. It composes
actual ApplicationController/ModelService, real DaemonIpcServer and SQLite stores with the existing
in-memory credential constructor, Offline policy, an empty injected catalog and no selected model.
It omits DaemonService/DaemonModelHelpers/LocalModelRuntime lifecycle. It optionally starts the
real ResourceMonitor at its default 2s cadence. A unique QStandardPaths test identity and temporary
stores are removed after orderly IPC shutdown; explicit isolation/cleanup markers and real status
responses qualify the observation. Its process cost is a **daemon/application composition fixture**,
not production executable cost. The 50ms signal timer mirrors daemon shutdown polling for bounded
owned cleanup; no other process is signalled. Other temporary paths use fresh HOME/XDG and an
environment allowlist without provider credentials. Existing user daemons/stores are excluded.

The existing P6-06 private Qt fixture supplies scripted provider/catalog data, production Main,
public view models, actual local IPC, application controller, AgentLoop, gateway, executor and
SQLite stores **inside one process**. All twelve existing independent correctness oracles must
pass. Screenshots are disabled here; the P6-06 screenshot-root allowlist remains untouched. This
is combined fixture CPU/memory, not a measured split between daemon, Chat, Agent or UI panels.
GUI uses offscreen/software rendering; native compositor, tray and accessibility costs are absent.
No public test-provider override is added.

The headless composition fixture is a separate process with no selected model, Offline discovery and 1 or 4 actual
connected IPC sockets. Four clients are simultaneously connected; status requests are sequential
round robin, not parallel saturation. Ten requests per connection validate actual `running=true`
and correlate response IDs/names/protocol versions. Measured framed bytes/s are serial round-trip
workload throughput, not a link-capacity benchmark. CLI version/help paths measure process execution;
TUI painting/interactive work and legacy chat are separate unmeasured paths.

Only direct test-owned PIDs are sampled, identified by Popen ownership and Darwin start absolute
counter. PID reuse/counter regression fails. No descendants are expected by these allowlisted
workloads. Unexpected child-tree discovery/measurement is not implemented; model/plugin trees
remain Blocked. External Ollama, LM Studio, user llama-server and Cloud costs are excluded.

## Counters and statistics

Darwin libproc `proc_pid_rusage(RUSAGE_INFO_V2)` yields RSS, physical footprint, user+system
CPU Mach absolute ticks converted with mach_timebase_info to seconds, disk-I/O bytes and package-idle/interrupt wakeup counters. `proc_pidinfo`
PROC_PIDTASKINFO yields thread count. Layouts match local SDK `sys/resource.h` and
`sys/proc_info.h` and are ABI-tested. RSS and footprint are different metrics; neither is total
system RAM or Linux PSS. Shared/unified pages cannot be uniquely attributed; never sum group
RSS as unique physical memory. Sampled maxima can miss startup/sub-interval/lifetime peaks.
Very short CLI commands may have no meaningful resident sample; duration remains available.

Durations and CPU intervals use monotonic time. CPU = 100 * delta CPU seconds / elapsed seconds:
100% means one logical core. Host-normalized CPU divides by logical CPU count (8 on reference
M2). Thread count and sampled maxima remain instantaneous observations. Cumulative disk bytes
are process counters, not SQLite transaction totals; fixture SQL/write assertions independently
verify correctness. No physical device I/O accounting claim is made.

Headless fixture idle summary excludes readiness/startup samples; the whole-run summary retains them.
Windows are short and have no 120s stabilization. Fresh profile/process startup is called process
cold; OS filesystem/dyld caches are uncontrolled, so disk-cold and warm-inference comparisons are
not claimed. Summary provides n, median, sample SD and observed peak. Nearest-rank p95 requires
at least 20 observations; three trial latencies have median/range/SD, no claimed p95. No confidence
interval or significant performance difference is inferred from three short trials.

The sampler records itself before workload launch and after cleanup; a separate 100-call microprobe
records native collection latency/CPU/footprint. Whole-run runner CPU also includes IPC driving,
validation and log processing, not just sampling. Conditions before/after each trial include load,
power/charge and `pmset` thermal-warning text. This is endpoint power-state detection; changes
which occur and revert between endpoints can be missed. No thermal warning is not proof of a
measured Celsius value. GPU activity/per-process unified GPU memory and hardware joules/watts
remain Unknown/Unavailable. No sudo or privileged agents are used.

## Evidence and review

`build/tests/resource-benchmark/run-*` retains each result/log, raw timestamped samples, correctness
oracles, hashes, binary path/hash, source SHA/branch/dirty digest, environment and cleanup results.
Failed/incomplete runs are retained; verification rejects failure, stale source/binary identity,
artifact traversal/tampering, absent isolation/cleanup and incomplete fixture oracles. Exact dirty
source evidence cannot certify a future clean commit. After final maintainer commit, rebuild without
source edits, rerun, verify and retain the clean-SHA result/receipt together. Binary hashes alone do
not establish source freshness; build logs must accompany the run.

A QA-schema-compatible companion receipt references the results hash; its pass means measurement
collection/oracles passed, never product performance acceptance. It is not inserted into the QA
registry and changes no historical feature/criterion status. Review machine results and logs,
including failures, before deriving hypotheses or requesting an optimization milestone.

## Future native profiles

| Platform | Memory | GPU/power | Required next work |
|---|---|---|---|
| macOS Apple Silicon | RSS and physical footprint separately; unified sharing unresolved | Native Metal/process instrumentation only where attributable; no fabricated watts | Native display/tray A/B, controlled active soak, approved existing model |
| Fedora KDE Wayland | `/proc` RSS plus `smaps_rollup` PSS/private where permitted | DRM/vendor availability; battery/sysfs; compositor separate | Peer-authenticated isolated daemon, native portal consent/display, PID start-time ownership checks |
| Windows x64 | Working set and private commit separately | DXGI/vendor counters only if available; native battery/thermal limitations | Implement safe native sampler, process-handle ownership, actual named-pipe/client support and native focus |

Do not extrapolate one platform into another. Future tray/Quick Panel/Inspector A/B should toggle
one feature in the same process against settled idle and record compositor conditions. Full-system
aggregates must list disjoint PIDs and explicitly retain shared-page uncertainty. Model profiles
require an already installed, explicitly authorized model and verified owned process handle;
never terminate or unload a shared external runtime to create a benchmark.
