# P4-05 Adaptive Resource Policy Calibration & Shadow Evaluation — Result

Phase: P4 — Adaptive Resource Management  
Priority: High  
Branch Context: `feature/p4-05-resource-policy-calibration`  
Result Type: Deterministic calibration foundation and disabled-by-default, strictly observational shadow evaluation for Sentinel's adaptive resource profiles.

> **Non-claims & Boundaries.** Adaptive resource optimization is **not** active or operational in production. This work does **not** adopt any universal numeric thresholds. Acceptance criteria AC-P4-01, AC-P4-02, and AC-P4-03 are **not** claimed accepted solely from fixture tests. The shadow evaluator is disabled by default, executes strictly in memory, owns no processes or models, grants no permissions, triggers no production actions, and enforces nothing. All changes are left uncommitted for maintainer review.

---

## 1. Base Identity & Workspace Verification

| Item | Value |
| --- | --- |
| Repository Root | `<personal-home>/Desktop/Projects/Sentinel-sanitized` |
| Working Branch | Ongoing task phase in working tree (`feature/p4-05-resource-policy-calibration` context) |
| Reconciled Base SHA | `017174e1e83db56c071de846c483a9cefdad4f61` (`origin/main` / `HEAD`) |
| Local Feature Branch Pointer | `3f85864136de077191dfbf8a7f4fd3f9c0d37151` (direct ancestor of reconciled `main`) |
| Available Disk Space | `90 GiB` available (`56%` capacity) |
| P4-02 Native Telemetry Dependency | Integrated (`HostResourceSnapshot`, `ResourceTelemetry.h`, `ResourceMonitor`) |
| P4-03.1 Hardened Engine Dependency | Integrated (`ResourcePolicy.h`, `ResourcePolicy.cpp`, hardened arithmetic & fail-closed) |
| P4-04 Telemetry Adapter Dependency | Integrated (`ResourcePolicyTelemetryAdapter.h`, `ResourcePolicyTelemetryAdapter.cpp`) |
| Single Working Agent | Verified. No extraneous worktrees, clones, or separate checkouts. |

### 1.1 Verification of the Five P4-03.1 Safety Corrections

The five safety fixes introduced in P4-03.1 were verified present in `core/src/policy/ResourcePolicy.cpp`:

1. **Comparison Direction for Process RSS:** `processResidentBytes` strictly uses `ascendingBytesSignal` (high value = pressure), while `hostAvailableBytes` strictly uses `descendingBytesSignal` (low value = pressure).
2. **Power-Source Usability:** In `powerSource` evaluation, presence checks `inputs.powerSource.usable()` to distinguish measured/stale states from unavailable ones.
3. **Finite Fractions & Byte Sign Guards:** Fractions outside `[0.0, 1.0]` or non-finite (`NaN`/`Inf`) fail verification; negative byte inputs fail verification.
4. **Invalid-Configuration Fail-Closed:** The engine explicitly enters the inert fail-closed path when `!configurationProblems_.isEmpty() || !thresholdsCalibrated || applicableWatchedCount == 0`, returning `ResourcePressureState::Unknown`, `ResourceTelemetryState::Interrupted`, and proposing no budget limits.
5. **Saturating Cooldown Arithmetic:** All deadline evaluations use `saturatingDeadlineMs(base, delta)` to prevent integer overflow and deadline bypass.

---

## 2. Exact Changed Files

| File | Change Type | Purpose |
| --- | --- | --- |
| `core/include/sentinel/core/policy/ResourcePolicyCalibration.h` | Added (Header) | Deterministic calibration sample/expectation/fixture definitions and scenario library |
| `core/src/policy/ResourcePolicyCalibration.cpp` | Added (Source) | Execution logic for fixtures, proposal metadata, and 14 scenario implementations |
| `core/include/sentinel/core/policy/ResourcePolicyShadowEvaluation.h` | Added (Header) | Disabled-by-default shadow evaluator and bounded/redacted diagnostic contracts |
| `core/src/policy/ResourcePolicyShadowEvaluation.cpp` | Added (Source) | Shadow evaluator implementation, ring buffer management, and redacted formatting |
| `tests/core/test_resource_policy_calibration.cpp` | Added (Tests) | Deterministic unit tests for all 10 required scenarios, safety, and hysteresis |
| `tests/core/test_resource_policy_shadow.cpp` | Added (Tests) | Tests for shadow observational purity, non-enforcement, privacy, and compile assertions |
| `core/CMakeLists.txt` | Modified | Registered `ResourcePolicyCalibration` and `ResourcePolicyShadowEvaluation` |
| `tests/CMakeLists.txt` | Modified | Registered test targets `test_resource_policy_calibration` and `test_resource_policy_shadow` |
| `docs/audits/P4_05_RESOURCE_POLICY_CALIBRATION_RESULT.md` | Added (Audit) | This audit and calibration deliverable report |

---

## 3. Supported Telemetry Signals & Platform Availability

The P4-04 adapter and P4-05 calibration distinguish strictly between **Measured**, **Stale**, **Unknown**, **Unavailable**, and **Unsupported** signals across operating systems:

| Signal | Unit | macOS (Darwin) | Linux (Fedora/Debian) | Windows | Classification Rules |
| --- | --- | --- | --- | --- | --- |
| `hostAvailableBytes` | Bytes (`qint64`) | Measured (`mach_vm / vm_statistics64`) | Measured (`/proc/meminfo` MemAvailable) | Measured (`GlobalMemoryStatusEx`) | Measured when authoritative; descending comparison |
| `processResidentBytes` | Bytes (`qint64`) | Measured (`proc_pidinfo / task_info`) | Measured (`/proc/self/statm`) | Measured (`GetProcessMemoryInfo`) | Measured; ascending comparison |
| `memoryPressureLevel` | `0..1` (`double`) | Measured (`DISPATCH_SOURCE_TYPE_MEMORYPRESSURE`) | Inferred / Platform (`PSI` if available) | Inferred (`CommitLimit` ratio) | Monotonic fraction; values outside `[0.0, 1.0]` rejected |
| `cpuBusyFraction` | `0..1` (`double`) | Inferred / Sampler | Measured (`/proc/stat` delta) | Measured (`GetSystemTimes` / PDH) | Unknown if no authoritative sampler calibrated |
| `powerSource` | Enum (`Ac`/`Battery`) | Measured (`IOPowerSources`) | Measured (`/sys/class/power_supply`) | Measured (`GetSystemPowerStatus`) | Unavailable on desktop AC-only machines (never degraded) |
| `batteryChargeFraction` | `0..1` (`double`) | Measured (`IOPowerSources`) | Measured (`/sys/class/power_supply`) | Measured (`GetSystemPowerStatus`) | Inapplicable / Unavailable if no battery present |
| `thermalLevel` | Enum / String | Measured (Darwin Thermal Notification) | Measured (`/sys/class/thermal`) | Inferred / Unsupported | String token match (`nominal`, `fair`, `serious`, `critical`) |
| `gpuUtilizationFraction`| `0..1` (`double`) | Unsupported / Metal | Platform (DRM / vendor sysfs) | Platform (D3D / PDH) | Unsupported signal does not degrade overall telemetry |

---

## 4. Proposed Profile Calibration Matrix (Non-Production)

> [!IMPORTANT]
> The matrix below represents an **unapproved technical proposal** derived solely from the P4-01 empirical baseline measured on Apple M2 (16 GiB RAM, macOS arm64). It is explicitly **not approved** (`approved = false`) and must not be used as universal production thresholds.

### 4.1 Proposed Profile Budgets (`ResourceProfileBudgetSet`)

| Profile | Proposed Concurrency Limit (`tool`) | Proposed Background Limit (`bg`) | Proposed Threshold Scale | Proposed Sampling Interval | Proposed Background Ceiling |
| --- | --- | --- | --- | --- | --- |
| **Eco** | `1` | `0` (restricted) | `0.85` | `4000 ms` | `Normal` |
| **Balanced** | `2` | `1` | `1.00` | `2000 ms` | `Elevated` |
| **Performance** | `4` | `2` | `1.15` | `1000 ms` | `High` |

### 4.2 Proposed Thresholds & Timings (`ResourcePolicyThresholds` & `ResourcePolicyTiming`)

| Parameter | Unit | Proposed Evaluated Value | Direction / Note |
| --- | --- | --- | --- |
| `availableMemoryEscalateAtBytes` | Bytes | `1 GiB` (`1,073,741,824`) | Descending threshold |
| `availableMemoryDeescalateAtBytes` | Bytes | `2 GiB` (`2,147,483,648`) | Hysteresis clearance |
| `processResidentEscalateAtBytes` | Bytes | `1 GiB` (`1,073,741,824`) | Ascending threshold |
| `processResidentDeescalateAtBytes` | Bytes | `768 MiB` (`805,306,368`) | Hysteresis clearance |
| `memoryPressureEscalateAt` | Fraction | `0.75` | Ascending threshold |
| `memoryPressureDeescalateAt` | Fraction | `0.45` | Hysteresis clearance |
| `cpuBusyEscalateAt` | Fraction | `0.85` | Ascending threshold |
| `cpuBusyDeescalateAt` | Fraction | `0.60` | Hysteresis clearance |
| `thermalWarningLevels` | Strings | `{"serious", "critical"}` | Exact string token matches |
| `escalateOnBattery` | Boolean | `false` | Disabled by default in proposal |
| `escalationSustainMs` | Monotonic ms | `5,000 ms` | Prevents transient spikes |
| `minimumResidenceMs` | Monotonic ms | `10,000 ms` | Minimum time spent in elevated tier |
| `cooldownMs` | Monotonic ms | `15,000 ms` | Prevents rapid oscillation |
| `telemetryGraceMs` | Monotonic ms | `30,000 ms` | Window before missing signals degrade |

---

## 5. Deterministic Evidence: Calibration Fixture Scenarios

The calibration suite implements 14 hermetic, deterministic scenarios executed by `runResourcePolicyCalibration()` with zero external clock, thread, or process dependencies:

1. `low_available_ram`: Host available memory drops below 1 GiB; escalates to `Elevated`; preserves byte units.
2. `high_process_rss`: Sentinel process RSS crosses 1 GiB; ascending signal verified.
3. `memory_pressure_escalation`: Native memory pressure level reaches `0.85`; transitions to `Elevated`.
4. `cpu_contention`: Host CPU busy fraction reaches `0.95`; transitions to `Elevated`.
5. `battery_operation`: Verifies battery escalation is strictly opt-in and does not escalate when disabled.
6. `thermal_warning`: Matches explicit level tokens (`serious`), ignoring unconfigured tokens (`fair`).
7. `missing_gpu_telemetry`: Unsupported GPU telemetry is excluded from degradation and does not trigger pressure.
8. `unsupported_host_cpu_telemetry`: Unknown CPU telemetry yields `ResourcePressureState::Unknown` and `ResourceTelemetryState::Interrupted`; never healthy.
9. `foreground_versus_background`: Under `Eco`, new background tasks are restricted (`restrictBackgroundAdmission = true`), while foreground tasks are strictly preserved (`foregroundAdmissionAllowed = true`).
10. `recovery_after_sustained_pressure`: Step-down recovery verified across sustain window (`1000 ms`), minimum residence (`2000 ms`), and cooldown (`500 ms`).
11. `oscillation_at_threshold_boundary`: Cooldown timer prevents flapping between `Elevated` and `Normal` when input oscillates around boundaries.
12. `invalid_configuration_fails_closed`: Inconsistent thresholds (de-escalate > escalate) cause validation failure, resulting in fail-closed inert evaluation with no proposed budget limits.
13. `stale_measurement_cannot_clear`: Stale input yields `ResourceTelemetryState::Degraded` and cannot de-escalate verified pressure.
14. `unavailable_signals_are_not_a_degradation`: A host lacking a battery remains `Fresh` because an absent signal is classified as `Unavailable`, not degraded.

---

## 6. Shadow Evaluation Implementation & Privacy Protections

The shadow evaluator (`ResourcePolicyShadowEvaluator`) provides a safe, strictly observational observation harness:

```
[Telemetry / Snapshot] ──> [ResourcePolicyTelemetryAdapter] ──> [ResourcePolicyInputs]
                                                                        │
                                                                 (if enabled)
                                                                        ▼
                                                        [ResourcePolicyShadowEvaluator]
                                                                        │
                                                        ┌───────────────┴───────────────┐
                                                        ▼                               ▼
                                            [ResourcePolicyEngine]            [Bounded Ring Buffer]
                                            (Pure In-Memory Eval)         (Max 128 redacted records)
                                                        │                               │
                                                        └───────────────┬───────────────┘
                                                                        ▼
                                                        [ResourcePolicyShadowRecord]
                                                        (Data only — No authority/No action)
```

### 6.1 Safety Properties of Shadow Evaluation
- **Disabled by default:** `ResourcePolicyShadowConfig::enabled == false`. When disabled, `observe()` immediately returns `std::nullopt` without invoking the engine.
- **Ring buffer bounds:** Retention is bounded by `maxRecords` (default 128, clamped to $\ge 1$). Memory allocation cannot grow unbounded.
- **Redaction and privacy guarantee:** Diagnostics format exclusively enumerated names, numbers, and boolean flags (`kResourcePolicyShadowMaxDiagnosticLength = 512`). Free-text telemetry provenance (such as `sourceId` and adapter `detail`) is dropped and never forwarded to the record.
- **Privacy test verification:** Synthetic personal paths (e.g. `<user-home>/alice/` or `<personal-home>/bob/`) and credential tokens (`Bearer sk-live-...`) were injected into inputs and verified completely absent from output diagnostics. The repository's privacy scanner `check_personal_paths.py` reports `0 file(s) flagged`.

---

## 7. No-Enforcement Proof

### 7.1 Structural Invariant Verification (Compile-Time Proof)
Compile-time static assertions in `tests/core/test_resource_policy_shadow.cpp` prove that `ResourcePolicyShadowRecord` cannot express executive authority:

```cpp
static_assert(!HasPermissionField<ResourcePolicyShadowRecord>::value,
              "shadow records must not carry permission authority");
static_assert(!HasSandboxField<ResourcePolicyShadowRecord>::value,
              "shadow records must not carry sandbox authority");
static_assert(!HasApprovalField<ResourcePolicyShadowRecord>::value,
              "shadow records must not carry approval authority");
static_assert(!HasProviderField<ResourcePolicyShadowRecord>::value,
              "shadow records must not carry provider selection");
static_assert(!HasProcessIdField<ResourcePolicyShadowRecord>::value,
              "shadow records must not reference a process");
static_assert(!HasModelHandleField<ResourcePolicyShadowRecord>::value,
              "shadow records must not reference a model handle");
static_assert(!HasRoutingModeField<ResourcePolicyShadowRecord>::value,
              "shadow records must not carry routing mode");
static_assert(!HasBudgetField<ResourcePolicyShadowRecord>::value,
              "shadow records must not mutate a resource budget");
```

### 7.2 Action Vocabulary Proof
The action vocabulary in `ResourcePolicyAction` is strictly closed:
- `ResourcePolicyAction::None`
- `ResourcePolicyAction::RefuseNewAdmission`
- `ResourcePolicyAction::RestrictBackgroundAdmission`
- `ResourcePolicyAction::LimitToolConcurrency`

There are no actions representing "unload", "terminate", "kill", "grant", "evict", or "route". Tests systematically verify that neither the engine nor shadow diagnostics contain any destructive verbs.

---

## 8. Full Validation Results

All validation suites and static checks were executed on the working tree:

| Gate | Command | Result | Notes |
| --- | --- | --- | --- |
| **P4-05 Calibration Tests** | `ctest --preset tests -R test_resource_policy_calibration` | **Passed** (`0.15s`) | All 19 test slots and 14 fixtures passed |
| **P4-05 Shadow Tests** | `ctest --preset tests -R test_resource_policy_shadow` | **Passed** (`0.72s`) | All 10 test slots and compile assertions passed |
| **Resource Suite** | `ctest --preset tests -R "resource"` | **Passed** (`9/9` passed) | P4-02, P4-03, P4-03.1, P4-04, P4-05 suites |
| **Full CTest Suite** | `ctest --preset tests` | **Passed** (`152/152` passed) | 100% pass across all core, desktop, agent, and IPC suites |
| **Rust Verification** | `cargo test --manifest-path cli/Cargo.toml` | **Passed** (`100/100` passed) | 78 CLI/TUI + 22 mascot tests passed |
| **Clang-Format** | `clang-format --dry-run -Werror <files>` | **Passed** (`0` errors) | CI-pinned code style compliant across all 6 files |
| **Git Diff Check** | `git diff --check` | **Passed** (`0` errors) | No whitespace, newline, or conflict errors |
| **Privacy Path Scan** | `python3 tools/analysis/check_personal_paths.py` | **Passed** (`0` flagged) | No personal paths or home directories in files |

---

## 9. Pending Product Decisions (PD-D03, PD-D04, PD-D10)

The calibration foundation establishes technical evidence but requires the following formal maintainer decisions before any profile or automatic policy may be enabled:

### PD-D03: Supported Hardware & Environment Classification
- **Issue:** Universal device thresholds do not exist. An 8 GiB RAM Linux server, a 16 GiB macOS workstation, and a 64 GiB developer desktop exhibit vastly different baseline memory consumption.
- **Technical Recommendation:** Define three explicit hardware environment classes:
  1. *Resource-Constrained Device* ($\le 8\text{ GiB RAM}$, battery-driven or shared desktop).
  2. *Standard Workstation* ($16\text{ GiB RAM}$, multi-core, local inference capable).
  3. *High-Capacity Workstation / Server* ($\ge 32\text{ GiB RAM}$, dedicated GPU, dedicated execution budget).
- **Adoption Requirement:** Hardware classification must be resolved before associating profiles with hardware defaults.

### PD-D04: Numeric Thresholds, Timings, Hysteresis, and Budget Calibration
- **Issue:** The values in `proposedResourcePolicyCalibration()` are calibrated against the P4-01 single-machine baseline (Apple M2, 16 GiB).
- **Technical Recommendation:** Maintain the proposed thresholds as a baseline for the *Standard Workstation* class. Keep sustain window at $\ge 5\text{ s}$ and cooldown at $\ge 15\text{ s}$ to eliminate threshold thrashing.
- **Adoption Requirement:** Explicit maintainer approval of numeric matrices per hardware class; until approved, default configuration remains `Unconfigured` (fail-closed, fixed limits).

### PD-D10: Resource Ownership Dependencies
- **Issue:** Model lifecycles and background providers may be managed as Sentinel children (owned) or externally shared (e.g. system Ollama or cloud endpoints).
- **Technical Recommendation:** Maintain strict adherence to `ResourceOwnershipPosture::PreserveAllResidency`. Adaptive policy must never terminate, unload, or evict shared provider processes or active Agent model bindings.
- **Adoption Requirement:** Formal ownership lease protocol definition before any de-escalation or eviction action can be considered.

---

## 10. Proposed P4-06 Controlled Activation Scope

When maintainers choose to proceed to P4-06, the scope should be strictly bounded:

1. **Activation Target:** Opt-in shadow logging in developer builds to collect live trace evidence across diverse platforms without affecting execution.
2. **Admission Integration:** Connect the policy engine strictly to `AgentExecutionScheduler` advisory checks.
3. **Hard Boundaries Preserved:**
   - No automatic profile switching without user interaction.
   - Foreground user queries must **never** be refused admission.
   - Active Agent runs must **never** lose their model binding.
   - Security permissions and tool sandboxes must **never** be modified by resource pressure.
   - Provider routing must **never** bypass user configuration.

---

## 11. Final Summary

Phase P4-05 successfully provides a deterministic calibration foundation and a completely isolated, disabled-by-default shadow evaluation pipeline. All safety criteria, hysteresis rules, redactions, compile-time invariants, and 152/152 tests have been validated cleanly on the shared repository tree. All changes remain uncommitted for maintainer review.
