// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/policy/ResourcePolicyCalibration.h"

namespace sentinel::core {

namespace {

constexpr qint64 kMiB = 1024LL * 1024;
constexpr qint64 kGiB = 1024LL * 1024 * 1024;

// The action vocabulary is closed: these four are the only outcomes the P4-03
// contract can express, and none of them is destructive or authority-granting.
// This switch is the runtime proof that a calibration run can never emit a
// "unload", "terminate", "kill" or "grant" action: such a value does not exist.
bool isNonDestructiveAction(ResourcePolicyAction action) {
    switch (action) {
    case ResourcePolicyAction::None:
    case ResourcePolicyAction::RefuseNewAdmission:
    case ResourcePolicyAction::RestrictBackgroundAdmission:
    case ResourcePolicyAction::LimitToolConcurrency:
        return true;
    }
    return false;
}

// A scenario config that watches only the signals the scenario supplies, so a
// fixture's telemetry coverage is unambiguous. The per-profile budgets come from
// the unapproved proposal because evaluating a proposed budget is the point.
ResourcePolicyConfig scenarioConfig(ResourcePolicyProfile profile) {
    ResourcePolicyConfig config;
    config.profile = profile;
    config.profiles = proposedResourcePolicyCalibration().profiles;
    return config;
}

ResourcePolicyCalibrationFixture lowAvailableRam() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("low_available_ram");
    fixture.description =
        QStringLiteral("Host reclaimable memory falls below the proposed floor; bytes preserved.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.availableMemoryEscalateAtBytes = 1 * kGiB;
    fixture.config.thresholds.availableMemoryDeescalateAtBytes = 2 * kGiB;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.hostAvailableBytes = measuredPolicyInput<qint64>(8 * kGiB);
             return inputs;
         }(),
         ResourcePressureState::Normal},
        {1000,
         [] {
             ResourcePolicyInputs inputs;
             inputs.hostAvailableBytes = measuredPolicyInput<qint64>(512 * kMiB);
             return inputs;
         }(),
         ResourcePressureState::Elevated},
    };
    fixture.expectation.finalState = ResourcePressureState::Elevated;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectRestrictBackgroundAdmission = false; // Balanced ceiling is Elevated
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture highProcessRss() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("high_process_rss");
    fixture.description =
        QStringLiteral("Sentinel process RSS rises above the proposed ceiling; ascending signal.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.processResidentEscalateAtBytes = 1 * kGiB;
    fixture.config.thresholds.processResidentDeescalateAtBytes = 768 * kMiB;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.processResidentBytes = measuredPolicyInput<qint64>(256 * kMiB);
             return inputs;
         }(),
         ResourcePressureState::Normal},
        {1000,
         [] {
             ResourcePolicyInputs inputs;
             inputs.processResidentBytes = measuredPolicyInput<qint64>(1536 * kMiB);
             return inputs;
         }(),
         ResourcePressureState::Elevated},
    };
    fixture.expectation.finalState = ResourcePressureState::Elevated;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture memoryPressureEscalation() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("memory_pressure_escalation");
    fixture.description = QStringLiteral("Native memory-pressure indicator crosses the threshold.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.memoryPressureEscalateAt = 0.75;
    fixture.config.thresholds.memoryPressureDeescalateAt = 0.45;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.memoryPressureLevel = measuredPolicyInput<double>(0.20);
             return inputs;
         }(),
         ResourcePressureState::Normal},
        {1000,
         [] {
             ResourcePolicyInputs inputs;
             inputs.memoryPressureLevel = measuredPolicyInput<double>(0.85);
             return inputs;
         }(),
         ResourcePressureState::Elevated},
    };
    fixture.expectation.finalState = ResourcePressureState::Elevated;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture cpuContention() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("cpu_contention");
    fixture.description = QStringLiteral("Host-wide CPU busy fraction crosses the threshold.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.cpuBusyEscalateAt = 0.85;
    fixture.config.thresholds.cpuBusyDeescalateAt = 0.60;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.cpuBusyFraction = measuredPolicyInput<double>(0.30);
             return inputs;
         }(),
         ResourcePressureState::Normal},
        {1000,
         [] {
             ResourcePolicyInputs inputs;
             inputs.cpuBusyFraction = measuredPolicyInput<double>(0.95);
             return inputs;
         }(),
         ResourcePressureState::Elevated},
    };
    fixture.expectation.finalState = ResourcePressureState::Elevated;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture batteryOperation() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("battery_operation");
    fixture.description =
        QStringLiteral("Power source switches AC -> battery; escalation is opt-in.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.escalateOnBattery = true;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.powerSource = measuredPolicyInput(ResourcePolicyPowerSource::Ac);
             return inputs;
         }(),
         ResourcePressureState::Normal},
        {1000,
         [] {
             ResourcePolicyInputs inputs;
             inputs.powerSource = measuredPolicyInput(ResourcePolicyPowerSource::Battery);
             return inputs;
         }(),
         ResourcePressureState::Elevated},
    };
    fixture.expectation.finalState = ResourcePressureState::Elevated;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture thermalWarning() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("thermal_warning");
    fixture.description =
        QStringLiteral("Native thermal level string reaches a configured warning level.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.thermalWarningLevels = {QStringLiteral("serious"),
                                                      QStringLiteral("critical")};
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.thermalLevel = measuredPolicyInput<QString>(QStringLiteral("nominal"));
             return inputs;
         }(),
         ResourcePressureState::Normal},
        {1000,
         [] {
             ResourcePolicyInputs inputs;
             inputs.thermalLevel = measuredPolicyInput<QString>(QStringLiteral("serious"));
             return inputs;
         }(),
         ResourcePressureState::Elevated},
    };
    fixture.expectation.finalState = ResourcePressureState::Elevated;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture missingGpuTelemetry() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("missing_gpu_telemetry");
    fixture.description = QStringLiteral(
        "GPU signals are Unsupported on this host and must not escalate or degrade.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.processResidentEscalateAtBytes = 1 * kGiB;
    fixture.config.thresholds.gpuUtilizationEscalateAt = 0.90;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.processResidentBytes = measuredPolicyInput<qint64>(256 * kMiB);
             inputs.gpuAvailable =
                 inapplicablePolicyInput<bool>(ResourcePolicyInputState::Unsupported);
             inputs.gpuUtilizationFraction =
                 inapplicablePolicyInput<double>(ResourcePolicyInputState::Unsupported);
             return inputs;
         }(),
         ResourcePressureState::Normal},
    };
    fixture.expectation.finalState = ResourcePressureState::Normal;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture unsupportedHostCpuTelemetry() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("unsupported_host_cpu_telemetry");
    fixture.description = QStringLiteral(
        "Host-wide CPU is Unknown (no approved sampler) and must never read healthy.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.cpuBusyEscalateAt = 0.85;
    fixture.config.thresholds.cpuBusyDeescalateAt = 0.60;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.cpuBusyFraction = unknownPolicyInput<double>();
             return inputs;
         }(),
         ResourcePressureState::Unknown},
    };
    fixture.expectation.finalState = ResourcePressureState::Unknown;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Interrupted;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture foregroundVersusBackground() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("foreground_versus_background");
    fixture.description =
        QStringLiteral("Eco restricts new background starts; foreground admission is preserved.");
    fixture.profile = ResourcePolicyProfile::Eco;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Eco);
    fixture.config.thresholds.memoryPressureEscalateAt = 0.75;
    fixture.config.thresholds.memoryPressureDeescalateAt = 0.45;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.memoryPressureLevel = measuredPolicyInput<double>(0.20);
             inputs.activeForegroundRuns = 1;
             return inputs;
         }(),
         ResourcePressureState::Normal},
        {1000,
         [] {
             ResourcePolicyInputs inputs;
             inputs.memoryPressureLevel = measuredPolicyInput<double>(0.85);
             inputs.activeForegroundRuns = 1;
             inputs.activeBackgroundRuns = 1;
             return inputs;
         }(),
         ResourcePressureState::Elevated},
    };
    fixture.expectation.finalState = ResourcePressureState::Elevated;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectRefuseNewAdmission = false;
    fixture.expectation.expectRestrictBackgroundAdmission = true; // Eco ceiling is Normal
    fixture.expectation.expectToolConcurrencyLimit = 1;
    return fixture;
}

ResourcePolicyCalibrationFixture recoveryAfterSustainedPressure() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("recovery_after_sustained_pressure");
    fixture.description =
        QStringLiteral("Sustain then residence/cooldown: escalate slowly, recover one tier.");

    auto pressure = [](double value, int foregroundRuns) {
        ResourcePolicyInputs inputs;
        inputs.memoryPressureLevel = measuredPolicyInput<double>(value);
        inputs.activeForegroundRuns = foregroundRuns;
        return inputs;
    };

    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.memoryPressureEscalateAt = 0.75;
    fixture.config.thresholds.memoryPressureDeescalateAt = 0.45;
    fixture.config.timing.escalationSustainMs = 1000;
    fixture.config.timing.minimumResidenceMs = 2000;
    fixture.config.timing.cooldownMs = 500;
    fixture.samples = {
        {0, pressure(0.90, 1), ResourcePressureState::Unknown},
        {500, pressure(0.90, 1), ResourcePressureState::Unknown},
        {1000, pressure(0.90, 1), ResourcePressureState::Elevated},
        {3000, pressure(0.20, 1), ResourcePressureState::Normal},
        {3500, pressure(0.20, 0), ResourcePressureState::Normal},
    };
    fixture.expectation.finalState = ResourcePressureState::Normal;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture oscillationAtThresholdBoundary() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("oscillation_at_threshold_boundary");
    fixture.description =
        QStringLiteral("Alternating pressure around the boundary is damped by the cooldown.");

    auto pressure = [](double value) {
        ResourcePolicyInputs inputs;
        inputs.memoryPressureLevel = measuredPolicyInput<double>(value);
        return inputs;
    };

    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.memoryPressureEscalateAt = 0.75;
    fixture.config.thresholds.memoryPressureDeescalateAt = 0.45;
    fixture.config.timing.cooldownMs = 2000;
    fixture.samples = {
        {0, pressure(0.90), ResourcePressureState::Elevated},
        {500, pressure(0.20), ResourcePressureState::Elevated},  // cooldown blocks recovery
        {1000, pressure(0.90), ResourcePressureState::Elevated}, // no re-transition
        {2500, pressure(0.20), ResourcePressureState::Normal},   // cooldown elapsed
        {3000, pressure(0.90), ResourcePressureState::Elevated},
    };
    fixture.expectation.finalState = ResourcePressureState::Elevated;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture invalidConfigurationFailsClosed() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("invalid_configuration_fails_closed");
    fixture.description =
        QStringLiteral("An inconsistent config is rejected and the engine proposes nothing.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    // Recovery threshold above the escalation threshold is inconsistent.
    fixture.config.thresholds.cpuBusyEscalateAt = 0.30;
    fixture.config.thresholds.cpuBusyDeescalateAt = 0.80;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.cpuBusyFraction = measuredPolicyInput<double>(0.99);
             return inputs;
         }(),
         ResourcePressureState::Unknown},
    };
    fixture.expectation.finalState = ResourcePressureState::Unknown;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Interrupted;
    fixture.expectation.expectRefuseNewAdmission = false;
    fixture.expectation.expectRestrictBackgroundAdmission = false;
    // No budget may be proposed: `expectToolConcurrencyLimit` stays empty.
    return fixture;
}

ResourcePolicyCalibrationFixture staleMeasurementCannotClear() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("stale_measurement_cannot_clear");
    fixture.description =
        QStringLiteral("A stale value stays distinguishable and cannot clear a verified tier.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.memoryPressureEscalateAt = 0.75;
    fixture.config.thresholds.memoryPressureDeescalateAt = 0.45;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.memoryPressureLevel = measuredPolicyInput<double>(0.90);
             return inputs;
         }(),
         ResourcePressureState::Elevated},
        {1000,
         [] {
             ResourcePolicyInputs inputs;
             inputs.memoryPressureLevel = stalePolicyInput<double>(0.20);
             return inputs;
         }(),
         ResourcePressureState::Elevated},
    };
    fixture.expectation.finalState = ResourcePressureState::Elevated;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Degraded;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationFixture unavailableSignalsAreNotADegradation() {
    ResourcePolicyCalibrationFixture fixture;
    fixture.id = QStringLiteral("unavailable_signals_are_not_a_degradation");
    fixture.description = QStringLiteral(
        "A host without a battery stays Fresh; an absent signal is not degradation.");
    fixture.profile = ResourcePolicyProfile::Balanced;
    fixture.config = scenarioConfig(ResourcePolicyProfile::Balanced);
    fixture.config.thresholds.memoryPressureEscalateAt = 0.75;
    fixture.config.thresholds.memoryPressureDeescalateAt = 0.45;
    fixture.config.thresholds.batteryChargeEscalateAt = 0.20;
    fixture.samples = {
        {0,
         [] {
             ResourcePolicyInputs inputs;
             inputs.memoryPressureLevel = measuredPolicyInput<double>(0.20);
             inputs.powerSource = inapplicablePolicyInput<ResourcePolicyPowerSource>(
                 ResourcePolicyInputState::Unavailable);
             return inputs;
         }(),
         ResourcePressureState::Normal},
    };
    fixture.expectation.finalState = ResourcePressureState::Normal;
    fixture.expectation.finalTelemetry = ResourceTelemetryState::Fresh;
    fixture.expectation.expectToolConcurrencyLimit = 2;
    return fixture;
}

ResourcePolicyCalibrationCaseResult runFixture(const ResourcePolicyCalibrationFixture& fixture) {
    ResourcePolicyCalibrationCaseResult result;
    result.fixtureId = fixture.id;

    ResourcePolicyEngine engine(fixture.config);
    result.configurationWasInvalid = !engine.configurationProblems().isEmpty();

    QString failure;
    ResourcePolicyDecision last;
    for (const auto& sample : fixture.samples) {
        last = engine.evaluate(sample.inputs, sample.atMonotonicMs);

        if (sample.expectStateAfter.has_value() && last.state != *sample.expectStateAfter) {
            failure = QStringLiteral("t=%1 expected state %2 observed %3")
                          .arg(sample.atMonotonicMs)
                          .arg(resourcePressureStateName(*sample.expectStateAfter),
                               resourcePressureStateName(last.state));
        }
        if (failure.isEmpty() && fixture.expectation.expectForegroundAdmissionAlwaysAllowed &&
            !last.foregroundAdmissionAllowed) {
            failure =
                QStringLiteral("t=%1 foreground admission was withdrawn").arg(sample.atMonotonicMs);
        }
        if (failure.isEmpty() && fixture.expectation.expectOwnershipPreserved &&
            last.ownershipPosture != ResourceOwnershipPosture::PreserveAllResidency) {
            failure = QStringLiteral("t=%1 ownership posture was not preserved")
                          .arg(sample.atMonotonicMs);
        }
        if (failure.isEmpty() && fixture.expectation.expectPolicyPreservesSecurity &&
            !resourcePolicyDecisionPreservesSecurity()) {
            failure = QStringLiteral("t=%1 security invariant was not preserved")
                          .arg(sample.atMonotonicMs);
        }
        if (failure.isEmpty()) {
            for (const auto action : last.actions) {
                if (!isNonDestructiveAction(action)) {
                    failure = QStringLiteral("t=%1 emitted a destructive action")
                                  .arg(sample.atMonotonicMs);
                    break;
                }
            }
        }
        if (!failure.isEmpty()) {
            break;
        }
    }

    if (failure.isEmpty()) {
        const auto& expectation = fixture.expectation;
        if (last.state != expectation.finalState) {
            failure = QStringLiteral("final state: expected %1 observed %2")
                          .arg(resourcePressureStateName(expectation.finalState),
                               resourcePressureStateName(last.state));
        } else if (last.telemetry != expectation.finalTelemetry) {
            failure = QStringLiteral("final telemetry: expected %1 observed %2")
                          .arg(resourceTelemetryStateName(expectation.finalTelemetry),
                               resourceTelemetryStateName(last.telemetry));
        } else if (last.refuseNewAdmission != expectation.expectRefuseNewAdmission) {
            failure = QStringLiteral("refuseNewAdmission: expected %1 observed %2")
                          .arg(expectation.expectRefuseNewAdmission ? QStringLiteral("1")
                                                                    : QStringLiteral("0"),
                               last.refuseNewAdmission ? QStringLiteral("1") : QStringLiteral("0"));
        } else if (last.restrictBackgroundAdmission !=
                   expectation.expectRestrictBackgroundAdmission) {
            failure = QStringLiteral("restrictBackgroundAdmission: expected %1 observed %2")
                          .arg(expectation.expectRestrictBackgroundAdmission ? QStringLiteral("1")
                                                                             : QStringLiteral("0"),
                               last.restrictBackgroundAdmission ? QStringLiteral("1")
                                                                : QStringLiteral("0"));
        } else if (expectation.expectToolConcurrencyLimit.has_value() &&
                   last.toolConcurrencyLimit != expectation.expectToolConcurrencyLimit) {
            failure = QStringLiteral("toolConcurrencyLimit mismatch");
        } else if (!expectation.expectToolConcurrencyLimit.has_value() &&
                   last.toolConcurrencyLimit.has_value()) {
            failure = QStringLiteral("toolConcurrencyLimit: expected unset, observed a limit");
        }
    }

    result.passed = failure.isEmpty();
    result.failureDetail = failure;
    result.observedSummary = resourcePolicyDecisionSummary(last);
    return result;
}

} // namespace

ResourcePolicyCalibrationProposal proposedResourcePolicyCalibration() {
    ResourcePolicyCalibrationProposal proposal;
    proposal.approved = false;
    proposal.evidenceScope = QStringLiteral(
        "P4-01 measured host (Apple M2, 8 logical cores, 16 GiB RAM, macOS arm64); fixture values "
        "reasoned from that single environment, not a universal device threshold.");
    proposal.unresolvedDecisions = {QStringLiteral("PD-D03"), QStringLiteral("PD-D04"),
                                    QStringLiteral("PD-D10")};
    proposal.adoptionRequirements = {
        QStringLiteral("PD-D03: declare the supported hardware/environment profile classes."),
        QStringLiteral("PD-D04: adopt numeric thresholds, timings, hysteresis and budgets."),
        QStringLiteral("PD-D10: decide provider ownership before any ownership action."),
        QStringLiteral("Resolve D-6/D-7/D-8 open policy semantics before enabling de-escalation."),
    };

    auto& thresholds = proposal.thresholds;
    thresholds.cpuBusyEscalateAt = 0.85;
    thresholds.cpuBusyDeescalateAt = 0.60;
    thresholds.memoryPressureEscalateAt = 0.75;
    thresholds.memoryPressureDeescalateAt = 0.45;
    thresholds.availableMemoryEscalateAtBytes = 1 * kGiB;
    thresholds.availableMemoryDeescalateAtBytes = 2 * kGiB;
    thresholds.processResidentEscalateAtBytes = 1 * kGiB;
    thresholds.processResidentDeescalateAtBytes = 768 * kMiB;
    thresholds.thermalWarningLevels = {QStringLiteral("serious"), QStringLiteral("critical")};
    thresholds.escalateOnBattery = false; // opt-in policy, off in the proposal
    thresholds.batteryChargeEscalateAt = 0.20;
    thresholds.batteryChargeDeescalateAt = 0.35;
    thresholds.escalateWhenGpuUnavailable = false; // opt-in policy, off in the proposal
    thresholds.gpuUtilizationEscalateAt = 0.90;
    thresholds.gpuUtilizationDeescalateAt = 0.70;

    auto& timing = proposal.timing;
    timing.escalationSustainMs = 5000;
    timing.minimumResidenceMs = 10000;
    timing.cooldownMs = 15000;
    timing.telemetryGraceMs = 30000;

    auto& eco = proposal.profiles.eco;
    eco.calibrated = true;
    eco.maxConcurrentToolStarts = 1;
    eco.maxConcurrentBackgroundStarts = 0;
    eco.escalationThresholdScale = 0.85;
    eco.samplingIntervalMs = 4000;
    eco.backgroundAdmissionCeiling = ResourcePressureState::Normal;

    auto& balanced = proposal.profiles.balanced;
    balanced.calibrated = true;
    balanced.maxConcurrentToolStarts = 2;
    balanced.maxConcurrentBackgroundStarts = 1;
    balanced.escalationThresholdScale = 1.0;
    balanced.samplingIntervalMs = 2000;
    balanced.backgroundAdmissionCeiling = ResourcePressureState::Elevated;

    auto& performance = proposal.profiles.performance;
    performance.calibrated = true;
    performance.maxConcurrentToolStarts = 4;
    performance.maxConcurrentBackgroundStarts = 2;
    performance.escalationThresholdScale = 1.15;
    performance.samplingIntervalMs = 1000;
    performance.backgroundAdmissionCeiling = ResourcePressureState::High;

    return proposal;
}

QList<ResourcePolicyCalibrationFixture> standardResourcePolicyCalibrationFixtures() {
    return {
        lowAvailableRam(),
        highProcessRss(),
        memoryPressureEscalation(),
        cpuContention(),
        batteryOperation(),
        thermalWarning(),
        missingGpuTelemetry(),
        unsupportedHostCpuTelemetry(),
        foregroundVersusBackground(),
        recoveryAfterSustainedPressure(),
        oscillationAtThresholdBoundary(),
        invalidConfigurationFailsClosed(),
        staleMeasurementCannotClear(),
        unavailableSignalsAreNotADegradation(),
    };
}

ResourcePolicyCalibrationReport
runResourcePolicyCalibration(const QList<ResourcePolicyCalibrationFixture>& fixtures) {
    ResourcePolicyCalibrationReport report;
    report.cases.reserve(fixtures.size());
    for (const auto& fixture : fixtures) {
        const auto result = runFixture(fixture);
        if (result.passed) {
            ++report.passed;
        } else {
            ++report.failed;
        }
        report.cases.append(result);
    }
    return report;
}

QStringList ResourcePolicyCalibrationReport::failingFixtureIds() const {
    QStringList failures;
    for (const auto& result : cases) {
        if (!result.passed) {
            failures.append(result.fixtureId);
        }
    }
    return failures;
}

QString ResourcePolicyCalibrationReport::summary() const {
    return QStringLiteral("calibration: %1 passed, %2 failed").arg(passed).arg(failed);
}

} // namespace sentinel::core
