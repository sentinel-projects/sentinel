# P4-B01 — full-system resource benchmark and profiling

Result: repeatable **bounded fixture baseline**, with explicit native/full-system gaps.
Measurement collection success is not production performance acceptance. No production resource
behavior, policy budget, model residency, authorization or lifecycle contract was changed.
PD-D03, PD-D04 and PD-D10 remain pending. All changes are uncommitted.

## Workspace and source identity

Existing supplied checkout and origin `https://github.com/sentinel-projects/sentinel.git` verified.
Fetched origin; clean main = origin/main = HEAD at
`c4179fb052d4ba288839df93b875a72ebc0630fc`. Created absent
`perf/p4-b01-full-system-resource-benchmark` from main. No Git publishing or history operations.
Personal-Brain `brain` was unavailable on PATH; no alternate checkout was guessed. AGENTS and
architecture/runtime/security/build/testing read, plus P4-01–05, QA-02B, P2-M08 and P6-06 reports.
Their implementation/report commits are ancestors of main: P4-01 `a3df4946`, P4-02 `3a7ebadb`,
P4-03 `01ca78ba`, P4-03.1 `0245c966`, P4-04 `f9a596fe`, P4-05 `c30868be`, QA-02B `182c1f5f`,
P2-M08 `e139602c`, P6-06 `33bc79cf`. Visible thread inventory showed only this coding chat active.

Reused `build/tests` and `cli/target`; analysis-only metadata in temporary output is not a build
tree. No clone/worktree/duplicate build, reset/stash/clean/rebase/commit/push/merge/PR/dispatch.
Initial disk availability approximately 85GiB. A pre-existing user daemon was observed and excluded:
no connection, reconfiguration, measurement attribution or termination of that process.

Exact changed files:

- `tests/CMakeLists.txt` — private measurement executable and Python unit test registration.
- `tests/helpers/resource_benchmark_probe.cpp` — credential-isolated headless composition.
- `tools/performance/benchmark.py` — bounded native sampler, fixed workloads, evidence/verifier.
- `tests/performance/test_benchmark.py` — arithmetic/ABI/CPU accuracy/ownership/failure tests.
- `docs/performance/RESOURCE_BENCHMARK.md` — permanent methodology and future profiles.
- `docs/audits/P4_B01_FULL_SYSTEM_RESOURCE_BENCHMARK_RESULT.md` — this result.

## Environment and provenance

MacBook Air Mac14,2, Apple M2, 8 logical cores, 16GiB unified RAM; macOS27.0.1 build26A434,
arm64. Existing tests preset is Debug; Rust executable is Debug. Apple clang21, Qt6.12,
CMake4.4.4; pinned formatting clang-format18.1.8. Exact toolchain/binary hashes are retained
in the machine evidence. No source freshness is inferred from a hash alone: completed build
logs accompany the measurements.

Preflight was on battery13%; no long soak was run. The corrected three-repeat baseline below
was on **AC, charging11%→13%**, with endpoint thermal/performance warnings absent. Host load
average changed from 3.05/2.70/2.45 to 2.32/2.51/2.40. Other desktop apps, WindowServer, coding
and an unrelated user daemon remained competing workload. No global RAM/energy claim is made.
Conditions are captured before/after each trial; brief intervening power changes can be missed.

The detailed numerical table below references the first corrected isolated run
`build/tests/resource-benchmark/run-snigt197/results.json`. It passed source/binary/artifact/
isolation/oracle verification at collection time. Later guard/verifier/tests/report edits make
that historical dirty digest stale. The canonical final source requalification is retained at
`build/tests/resource-benchmark/run-p4b01-final/results.json` and companion `receipt.json`;
its exact dirty digest, binaries, raw samples and outcome are authoritative for the final tree.
Values can vary between repeated runs; do not silently substitute them into the historical table.
The final verifier must pass without further source edits before handoff.

## Actual topology and component inventory

| Component | Implementation / execution / ownership |
|---|---|
| Daemon executable | DaemonService/ApplicationController production authority; **Blocked for accepted native measurements** by shared OS credential initialization, described below |
| Headless application fixture | Actual ApplicationController, ModelService, DaemonIpcServer and SQLite, one test-owned process; in-memory credentials, Offline empty catalog, no selected model; measured |
| Desktop, QML rendering/windows | Production Main and public view models in existing P6-06 test process with in-process daemon/application; offscreen/software measured as one compound; separate native executable cost Blocked |
| Menu bar/tray/native adapters | NativeCompanionAdapter implemented inside Desktop, not standalone process; no native tray A/B measurement |
| Quick Panel | Shares DesktopRuntimeClient and Desktop process; implemented, isolated open/close A/B Blocked |
| Inspector/Dashboard | Production Main navigation and Inspector accepted-run state exercised in compound fixture; no independent memory attribution |
| Chat/streaming | Scripted IChatProvider, actual runtime/IPC/persistence/cancel exercised; no Cloud/local-model token performance claim |
| AgentRuntime/AgentLoop | Actual production planning/final acceptance in compound fixture; no fake successful executor |
| Tools | Production gateway/RealToolExecutor/QtFileSystemService; explicit Deny/Allow, zero/one durable write, exact bytes/SHA verified |
| ModelService | Actual binding/routing owner; headless empty catalog and GUI scripted catalog; no physical model loaded |
| Discovery/health | Controller5s polling exists; test catalog injected/Offline. Production DaemonService60s health and15min remote catalogs are omitted, not measured |
| LocalModelRuntime | Production3s reconcile and owned ProcessExecutor child lifecycle; omitted from headless fixture; real models Blocked |
| Downloads/import/load/inference | Implemented storage/library/runtime paths; inactive, no model/download authorization; all performance Unknown |
| Ollama/llama.cpp/LM Studio | Integrations implemented; external servers excluded; no process attributed to Sentinel |
| Cloud providers | Remote inference cost excluded; no credentials/API calls |
| Rust CLI/TUI | CLI version process observed; interactive TUI implemented but no PTY/first-paint benchmark |
| Legacy C++ CLI | Real usage command observed; separate in-process composition for runtime commands, not exercised |
| IPC | Real authenticated local sockets, correlated hello/status responses;1/4 simultaneously connected clients |
| SQLite/persistence | Actual conversation/run/permission/projection stores in compound; memory-store benchmark500 writes and reads |
| Memory/retrieval | SQLiteMemoryStore and deterministic semantic/vector search benchmarks; no live embedding model |
| Background jobs | BackgroundJobService thread-pool/in-process queue implemented; no submitted job in measured fixture |
| Title generation | AutoTitleGenerator is in-process regex/string logic, not an independent model/process; no isolated benchmark |
| Notifications/native services | Implemented platform adapters, inactive or offscreen; native cost/consent unverified |
| Speech | STT/TTS asset/runtime/system adapters exist; no explicitly authorized speech assets/microphone; Blocked |
| Plugins/MCP | Experimental plugin host may own a separate process; MCP remote/process boundaries exist; none configured, controlled execution Blocked |

No panel, service or library was invented as a separate OS process. The headless probe deliberately
omits DaemonService/DaemonModelHelpers/LocalModelRuntime, so it cannot certify full daemon behavior.
Existing P6-06 compound topology retains real planner/gateway/executor and independent correctness
checks; synthetic inference does not certify live providers.

## Scenario matrix

“Measured compound” means cost belongs to the whole shared fixture, not an individual feature.
All feasible workloads use3 repeats. Blocked means missing secure composition/asset/instrumentation,
not zero consumption or unsupported product implementation.

| ID | Outcome / precise scope |
|---|---|
| B01 | Native daemon cold startup **Blocked**; headless fixture IPC readiness measured as substitute, not equivalent |
| B02 | Native idle **Blocked**; credential-isolated headless no-model idle measured10s/trial |
| B03 | Headless real ResourceMonitor opt-in measured at2s; production daemon integration not certified |
| B04 | Separate native Desktop cold startup **Blocked**; compound launches actual Main offscreen |
| B05 | Separate Desktop+daemon idle **Blocked**; no settled two-process fixture |
| B06 | Native minimized/tray-only A/B **Blocked**; offscreen lacks native presentation oracle |
| B07 | Quick Panel open/close A/B **Blocked**; no added controlled timing/idle phases |
| B08 | Inspector/compact/wide navigation measured compound; independent panel RSS Unknown |
| B09 | Streaming Chat, persisted output and cancellation measured compound |
| B10 | Scripted production Agent planning measured compound |
| B11 | Authorized real filesystem mutation/read-back and durable accepted final measured compound |
| B12 | Four real clients connected to headless fixture;40 status requests, serial round robin measured |
| B13 | Empty fake catalog/Offline state only; real discovery/health cycle cost **Blocked** |
| B14 | Chat session/persisted output/delete confirmation in compound; separate history scaling **Blocked** |
| B15 | Real SQLite and deterministic retrieval benchmarks measured; live embedding cost Unknown |
| B16 | Supported background job execution **Blocked**; no isolated workload/oracle added |
| B17 | No selected/loaded model in headless fixture measured; production native equivalent Blocked |
| B18–B22 | Cold start/loaded idle/inference/repetition/shutdown **Blocked**: no explicitly authorized installed model; no downloads or external-server termination |
| B23 | Rust CLI --version exit observation measured; real interactive TUI **Blocked** |
| B24 | Legacy CLI usage exit observation measured; runtime commands not benchmarked |
| B25 | Speech **Blocked**: no authorized disposable assets/native audio flow |
| B26 | Plugin/MCP **Blocked**: no configured authorized disposable workload/process oracle |
| B27 | Actual disconnect/reconnect state verified within Main compound; separate reconnect latency Unknown |
| B28 | Owned child orderly shutdown/reaping, profile removal measured; global post-exit RAM recovery Unknown |

## Observed resources (first corrected three-repeat baseline)

RSS/footprint in MiB (2^20 bytes). RSS median column is median of trial medians, peak is max
sampled whole-process RSS including startup; CPU median is median of trial sample medians.
CPU peak includes startup (100%=one core, host normalization divides by8); peaks are sampled,
not lifetime high-water marks. Exact per-trial n/p95/SD/deltas remain in results.json. All rows n=3.

| Workload | RSS median / observed peak | Footprint median | CPU median / peak (% one core) | Observed time |
|---|---:|---:|---:|---|
| Headless fixture idle | 41.30 / 43.75 | 8.55 | 0.123 / 101.21 | readiness median0.286s; range0.253–0.731s |
| Headless + ResourceMonitor | 41.53 / 43.92 | 8.59 | 0.119 / 100.30 | readiness median0.253s |
| Headless +4 IPC clients | 43.75 / 43.91 | 8.77 | 0.130 / 100.41 | readiness median0.249s |
| Main+daemon+Chat+Agent+tools compound | 182.14 / 198.13 | 97.17 | 48.147 / 100.00 | total exit observation median3.138s |
| Rust CLI version | Unknown: too short for loaded-state sample | Unknown | Unknown | exit-observation upper bound median0.107s |
| Legacy CLI usage | Unknown: early startup samples only | Unknown | Unknown | exit-observation upper bound median0.210s |
| SQLite benchmark | 25.27 / 25.52 | 5.72 | 78.688 / 83.22 | complete Qt benchmark process median0.317s |
| Deterministic retrieval benchmark | 26.49 / 26.86 | 5.27 | 100.00 / 100.00 | complete process median0.422s |
| Native telemetry adapter/monitor benchmark | 23.83 / 23.84 | 4.44 | 0.465 / 56.72 | complete process median3.739s |

Per-trial raw counters are authoritative. Short CLI latency includes polling quantization up
to the100ms sample interval and is not precise time-to-main/first-paint. Compound total includes
intentional QtTest waits, approval/focus actions and teardown; it is not model inference latency.

Headless window-integrated CPU (median across trials) was0.216% one core without monitor,
0.250% with monitor, and0.257% with4 clients (host normalized approximately0.027/0.031/0.032%).
These are sequential, short, non-randomized observations, not statistically significant overhead
claims. IRQ wakeup counter deltas were209–219,216–228,214–225 respectively across~10s windows;
package-idle wakeups are separate counters, not scheduler context switches.

One-client status RTT medians were0.160/0.178/0.164ms (10 samples/trial, p95 intentionally absent).
Four-client RTT medians0.144/0.144/0.154ms; per-trial p95=0.176/0.173/0.188ms (40 samples each).
Request IDs, response names/versions and running=true were checked independently from timing.
Raw framed byte throughput is recorded; serial round trips are not capacity/saturation testing.

Idle headless disk-I/O read/write deltas were0 in these windows; startup is excluded there.
Compound process disk read deltas22,597,632 /856,064 /253,952 bytes and writes7,720,960 /
8,015,872 /7,606,272 bytes include SQLite, Qt/font loads and all fixture work, not only the one
allowed file write. Each compound independently verifies exact replacement SHA
`fa2bbf37c7f215feace35d61be845ca8b691e718f9afc6a015702aa0b9db36fa`, one successful persisted
write, Deny=no mutation, accepted AgentLoop final, correct run/approval identity, conversation
persistence, truthful cancellation/reconnect and deliberate delete. Each trial child exits/reaps
and removes test stores. No rollback or system-RAM recovery is inferred from exit.

GPU utilization/attributed unified GPU memory: **Unknown**. Hardware joules/watts: **Unavailable**.
No sudo, privileged agent, power-setting change or invented wattage. No model-associated memory,
TTFT, tokens/s, warm/cold inference or loaded-idle result. Footprint is not RSS and RSS is not total
RAM; group RSS must not be summed as unique physical usage.

## Sampler overhead, profiling interpretation and aggregates

Native external sampler100-call probe: median2.792µs, p95=3.666µs, peak10.417µs,
CPU total0.323ms, runner footprint15.38MiB. CPU conversion uses actual Mach timebase
41.6667ns/tick on this M2 and is independently checked against time.process_time.
Whole-run runner CPU/footprint before/after each workload also includes IPC driving/validation.
Existing production native adapter benchmark medians147.79/149.00/148.54µs and p95
166.83/167.33/168.29µs,500 samples each. These are different samplers; do not conflate them.

Observed headless RSS grew~2.34–2.42MiB from first idle sample over10s. There is no long settled
window or repeat growth across retained sessions, so this is **not a leak finding**. The periodic
5s controller probe/thread and lazy initialization are hypotheses to isolate next. The50ms signal
poll is a confirmed source timer, but no causal energy/performance bottleneck is established.
Offscreen compound CPU is active software rendering plus test/Agent work, not evidence that
native GPU rendering is inefficient. No unbounded cache or redundant live discovery was proven.

| Requested aggregate | Evidence qualification |
|---|---|
| Daemon only | Real executable Blocked; headless fixture table is explicitly narrower |
| Daemon+Desktop | Compound fixture only; shared one-process cost, no separate aggregate sum |
| Daemon+Desktop+tray | Blocked; no native tray delta |
| Chat active / Agent active | Same compound trace, no disjoint phase allocation or double-counting |
| Local model loaded / generating | Blocked; no authorized model |
| Background service activity | Native telemetry benchmark only; jobs/speech/plugins not measured |
| Full supported application workload | Blocked pending native UI, real daemon credential isolation, model and background workloads |

No confirmed optimization bottleneck is claimed. Startup first-trial0.731s vs later0.286/0.253s
may reflect caches/competing work; no cache flush or randomized study was performed.

## Historical comparison and platform limits

P4-01 Release real daemon reported48.86–48.97MiB idle RSS medians and0.240–0.320% one-core CPU
with120s settle/300s sampling; extended1020s case49MiB/0.227%. This task uses Debug private
composition, different services, short unsettled windows and different power/load state. It
establishes no regression/improvement percentage. P4-01's description of macOS RSS as footprint
is not adopted: this runner reports RSS and physical footprint separately. Historical temporary
artifacts were not revalidated here.

Fedora Wayland and Windows results remain unmeasured. The permanent methodology documents
PSS/private/working-set distinctions, native GPU/power availability, compositor/tray A/B,
process-handle/start identity and local IPC requirements. Offscreen fixture success does not close
AC-P6-06/07/08, live provider AC-P2-01/03 or other historical acceptance. Registry unchanged.

## Failed/excluded observations and safety correction

- `run-lpmsmom8`: initial real executable probe, wrong assumed HOME isolation; native Qt stores
  were under a uniquely named actual Application Support path. That exact test-created directory
  was removed. Excluded; not accepted resource evidence.
- `run-j1acla88`: exploratory real daemon trials and other workloads. CPU wrongly treated Mach
  ticks as nanoseconds, independently disproven by150ms CPU-clock check;3 UI trials failed
  existing screenshot-root allowlist. All raw failures retained, no allowlist weakened.
- Further review found production DaemonService calls AppSettings::webSearchApiKey and the
  platform backend uses shared Keychain service/account IDs. Earlier initialization **may have
  queried real credential entries**; no returned secret was inspected, printed or persisted in
  evidence. We cannot certify that no read occurred. These trials were excluded and never
  rerun; final helper injects in-memory credentials and adds no production override. Review
  dispositions beside both pilot runs mark them ineligible.
- CPU timebase corrected and independently tested; capture output disabled for benchmark
  fixture, preserving all12 real interaction oracles. A later unit test found unclosed hash-file
  handles; context-managed file hashing fixed it and ResourceWarning-as-error tests now pass.

## Validation

- Complete CMake tests-preset configure/build Passed; final helper cleanup-guard rebuild Passed.
  Compiler errors0/warnings0 in final logs (`p4b01-build-complete.log`); existing build reused.
- Full sequential CTest **167/167 Passed,249.13s**, `-j1`, finite timeout, isolated profile.
  `p4b01-ctest.log` / `p4b01-ctest-detail.log`. Runner had12 tests at that checkpoint;
  final standalone suite adds tamper/failure verification: **13/13 Passed**, ResourceWarning as error.
- Existing **10 SKIPs**: Linux secret helper; refused native credential smoke; absent native
  incremental tool assembly contract;4 strict-macOS child-shell fixtures; opt-in live provider;
  missing-node branch inapplicable with npx installed; absent model residency/removal lease.
- Existing **2 XFAILs**: P3-02 D4 provider-switch child cleanup and D5b endpoint-switch child
  reconciliation. **81 QWARN lines**, existing suite; no new acceptance promotion.
- Rust fmt and locked/offline all-target workspace Clippy `-D warnings` Passed. First sandboxed
  Rust test attempt had9 socket permission failures (`Operation not permitted`); native IPC
  rerun **142 Passed,0 failed/ignored** (`p4b01-rust-tests-native.log`).
- Both IPC generators --check and QA registry/schema validation Passed, historical registry unchanged.
- Pinned clang-format18.1.8, Black and Ruff Passed. Scoped clang-tidy new helper: **0 user-code
  diagnostics/0 compiler errors**,581 non-user warnings suppressed by tool. Initial8 brace findings
  fixed; no source suppression added. Python AST/style and negative unit tests cover runner.
- Privacy path scan0flagged; scoped credential-signature scan0matches in6changed files and
  git diff --check Passed. No secret values or user conversations included in final evidence.
- Corrected fixture baseline **27/27 Measured**; final source requalification repeats the same
  nine workloads3 times. No15min or60min soak executed;15min idle and explicit60min idle modes
  supported, active soak remains future work.

## P4-B02 candidates and clean-SHA procedure

First enable measurement confidence, then propose optimizations from evidence: a reviewed private
DaemonService credential-construction seam; native two-process Desktop/tray/Quick Panel A/B;
settled15min idle and paced active retention tests; isolated controller5s polling attribution;
PTY TUI first paint; authorized existing-model cold/warm timing with owned-process handles;
background/speech/plugin workload oracles. These are candidates, not approved behavior changes.
No leak fix, automatic unload/termination, admission policy or new numeric budget is justified here.

After maintainer commit: record the exact clean SHA and empty status, rebuild existing tests/Rust
outputs, run the methodology's3-trial command with a **new** evidence directory, run --verify
without editing source, retain result+QA receipt+binary hashes+build logs. Dirty evidence here
cannot certify that future SHA. The QA companion pass means collection/correctness only and is
not inserted into historical acceptance records.
