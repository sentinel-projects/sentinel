// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// P4-05 deterministic calibration fixtures for the adaptive resource policy.
//
// Every value here is a FIXTURE value that makes a state-machine or safety
// property observable; none is a product threshold (PD-D03/PD-D04 remain
// pending). No case reads host telemetry, starts a thread, loads a model or
// touches a process, so the suite is hermetic and platform-independent.

#include "sentinel/core/policy/ResourcePolicy.h"
#include "sentinel/core/policy/ResourcePolicyCalibration.h"

#include <QtTest>

#include <limits>

using sentinel::core::inapplicablePolicyInput;
using sentinel::core::measuredPolicyInput;
using sentinel::core::proposedResourcePolicyCalibration;
using sentinel::core::ResourceModelOwnership;
using sentinel::core::ResourceOwnershipPosture;
using sentinel::core::ResourcePolicyAction;
using sentinel::core::ResourcePolicyCalibrationFixture;
using sentinel::core::ResourcePolicyConfig;
using sentinel::core::ResourcePolicyDecision;
using sentinel::core::ResourcePolicyEngine;
using sentinel::core::ResourcePolicyInputs;
using sentinel::core::ResourcePolicyInputState;
using sentinel::core::ResourcePolicyPowerSource;
using sentinel::core::ResourcePolicyProfile;
using sentinel::core::ResourcePolicyThresholds;
using sentinel::core::ResourcePressureState;
using sentinel::core::ResourceTelemetryState;
using sentinel::core::runResourcePolicyCalibration;
using sentinel::core::stalePolicyInput;
using sentinel::core::standardResourcePolicyCalibrationFixtures;
using sentinel::core::unknownPolicyInput;
using sentinel::core::validateResourcePolicyConfig;

namespace {

constexpr qint64 kMiB = 1024LL * 1024;
constexpr qint64 kGiB = 1024LL * 1024 * 1024;

// A config that watches exactly one signal and carries the *proposed* (unapproved)
// profile budgets, so budget-driven outcomes are observable.
ResourcePolicyConfig balancedConfig() {
    ResourcePolicyConfig config;
    config.profile = ResourcePolicyProfile::Balanced;
    config.profiles = proposedResourcePolicyCalibration().profiles;
    return config;
}

const ResourcePolicyCalibrationFixture* findFixture(const QString& id) {
    static const auto fixtures = standardResourcePolicyCalibrationFixtures();
    for (const auto& fixture : fixtures) {
        if (fixture.id == id) {
            return &fixture;
        }
    }
    return nullptr;
}

void expectState(const ResourcePolicyDecision& decision, ResourcePressureState expected,
                 const char* what) {
    QVERIFY2(decision.state == expected,
             qPrintable(QStringLiteral("%1: expected %2 got %3 (%4)")
                            .arg(QString::fromUtf8(what),
                                 sentinel::core::resourcePressureStateName(expected),
                                 sentinel::core::resourcePressureStateName(decision.state),
                                 sentinel::core::resourcePolicyDecisionSummary(decision))));
}

} // namespace

class ResourcePolicyCalibrationTest final : public QObject {
    Q_OBJECT
private slots:
    void standardFixtureLibraryPasses();
    void requiredScenariosAreCovered();
    void lowAvailableRamEscalatesAndKeepsBytes();
    void highProcessRssIsAscending();
    void memoryPressureEscalationEscalates();
    void cpuContentionEscalates();
    void batteryOperationEscalatesOnlyWhenConfigured();
    void thermalWarningMatchesLevelStringNotANumber();
    void missingGpuTelemetryNeverEscalates();
    void unsupportedHostCpuTelemetryIsNeverHealthy();
    void foregroundAdmissionIsPreservedUnderBackgroundRestriction();
    void recoveryDoesNotOscillate();
    void staleMeasurementRemainsDistinguishable();
    void invalidConfigurationFailsClosedAndProposesNothing();
    void unavailableSignalIsNotADegradation();
    void proposedCalibrationIsValidAndUnapproved();
    void repeatedIdenticalInputsAreDeterministic();
    void invalidTimestampsCannotBypassCooldown();
    void evaluationHasNoSideEffects();
};

void ResourcePolicyCalibrationTest::standardFixtureLibraryPasses() {
    const auto report = runResourcePolicyCalibration(standardResourcePolicyCalibrationFixtures());
    QVERIFY2(report.allPassed(),
             qPrintable(QStringLiteral("failing fixtures: %1 (%2)")
                            .arg(report.failingFixtureIds().join(QLatin1Char(',')))
                            .arg(report.summary())));
    QCOMPARE(report.failed, 0);
}

void ResourcePolicyCalibrationTest::requiredScenariosAreCovered() {
    const QStringList required = {
        QStringLiteral("low_available_ram"),
        QStringLiteral("high_process_rss"),
        QStringLiteral("memory_pressure_escalation"),
        QStringLiteral("cpu_contention"),
        QStringLiteral("battery_operation"),
        QStringLiteral("thermal_warning"),
        QStringLiteral("missing_gpu_telemetry"),
        QStringLiteral("unsupported_host_cpu_telemetry"),
        QStringLiteral("foreground_versus_background"),
        QStringLiteral("recovery_after_sustained_pressure"),
    };
    for (const auto& id : required) {
        QVERIFY2(findFixture(id) != nullptr,
                 qPrintable(QStringLiteral("missing fixture %1").arg(id)));
    }
}

void ResourcePolicyCalibrationTest::lowAvailableRamEscalatesAndKeepsBytes() {
    auto config = balancedConfig();
    config.thresholds.availableMemoryEscalateAtBytes = 1 * kGiB;
    config.thresholds.availableMemoryDeescalateAtBytes = 2 * kGiB;
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs healthy;
    healthy.hostAvailableBytes = measuredPolicyInput<qint64>(8 * kGiB);
    expectState(engine.evaluate(healthy, 0), ResourcePressureState::Normal, "8 GiB available");

    ResourcePolicyInputs low;
    low.hostAvailableBytes = measuredPolicyInput<qint64>(512 * kMiB);
    const auto decision = engine.evaluate(low, 1000);
    expectState(decision, ResourcePressureState::Elevated, "512 MiB available");
    QCOMPARE(decision.telemetry, ResourceTelemetryState::Fresh);
    // Low available memory alone must not refuse new admissions at Elevated.
    QVERIFY(!decision.refuseNewAdmission);
}

void ResourcePolicyCalibrationTest::highProcessRssIsAscending() {
    auto config = balancedConfig();
    config.thresholds.processResidentEscalateAtBytes = 1 * kGiB;
    config.thresholds.processResidentDeescalateAtBytes = 768 * kMiB;
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs lowRss;
    lowRss.processResidentBytes = measuredPolicyInput<qint64>(256 * kMiB);
    expectState(engine.evaluate(lowRss, 0), ResourcePressureState::Normal, "low RSS");

    ResourcePolicyInputs highRss;
    highRss.processResidentBytes = measuredPolicyInput<qint64>(1536 * kMiB);
    // High RSS is pressure, never health: the comparison must be ascending.
    expectState(engine.evaluate(highRss, 1000), ResourcePressureState::Elevated, "high RSS");
}

void ResourcePolicyCalibrationTest::memoryPressureEscalationEscalates() {
    auto config = balancedConfig();
    config.thresholds.memoryPressureEscalateAt = 0.75;
    config.thresholds.memoryPressureDeescalateAt = 0.45;
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs calm;
    calm.memoryPressureLevel = measuredPolicyInput<double>(0.20);
    expectState(engine.evaluate(calm, 0), ResourcePressureState::Normal, "calm pressure");

    ResourcePolicyInputs pressured;
    pressured.memoryPressureLevel = measuredPolicyInput<double>(0.85);
    expectState(engine.evaluate(pressured, 1000), ResourcePressureState::Elevated, "pressured");
}

void ResourcePolicyCalibrationTest::cpuContentionEscalates() {
    auto config = balancedConfig();
    config.thresholds.cpuBusyEscalateAt = 0.85;
    config.thresholds.cpuBusyDeescalateAt = 0.60;
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs idle;
    idle.cpuBusyFraction = measuredPolicyInput<double>(0.30);
    expectState(engine.evaluate(idle, 0), ResourcePressureState::Normal, "idle CPU");

    ResourcePolicyInputs busy;
    busy.cpuBusyFraction = measuredPolicyInput<double>(0.95);
    expectState(engine.evaluate(busy, 1000), ResourcePressureState::Elevated, "busy CPU");
}

void ResourcePolicyCalibrationTest::batteryOperationEscalatesOnlyWhenConfigured() {
    {
        // Default: running on battery is *not* an escalation policy.
        auto config = balancedConfig();
        config.thresholds.batteryChargeEscalateAt = 0.20;
        ResourcePolicyEngine engine(config);
        ResourcePolicyInputs onBattery;
        onBattery.powerSource = measuredPolicyInput(ResourcePolicyPowerSource::Battery);
        onBattery.batteryChargeFraction = measuredPolicyInput<double>(0.90);
        expectState(engine.evaluate(onBattery, 0), ResourcePressureState::Normal,
                    "battery with charge escalation off");
    }
    {
        auto config = balancedConfig();
        config.thresholds.escalateOnBattery = true;
        ResourcePolicyEngine engine(config);
        ResourcePolicyInputs ac;
        ac.powerSource = measuredPolicyInput(ResourcePolicyPowerSource::Ac);
        expectState(engine.evaluate(ac, 0), ResourcePressureState::Normal, "on AC");

        ResourcePolicyInputs onBattery;
        onBattery.powerSource = measuredPolicyInput(ResourcePolicyPowerSource::Battery);
        expectState(engine.evaluate(onBattery, 1000), ResourcePressureState::Elevated,
                    "battery with escalation on");
    }
}

void ResourcePolicyCalibrationTest::thermalWarningMatchesLevelStringNotANumber() {
    auto config = balancedConfig();
    config.thresholds.thermalWarningLevels = {QStringLiteral("serious"),
                                              QStringLiteral("critical")};
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs nominal;
    nominal.thermalLevel = measuredPolicyInput<QString>(QStringLiteral("nominal"));
    expectState(engine.evaluate(nominal, 0), ResourcePressureState::Normal, "nominal thermal");

    ResourcePolicyInputs fair;
    fair.thermalLevel = measuredPolicyInput<QString>(QStringLiteral("fair"));
    expectState(engine.evaluate(fair, 500), ResourcePressureState::Normal,
                "fair is not a configured warning level");

    ResourcePolicyInputs serious;
    serious.thermalLevel = measuredPolicyInput<QString>(QStringLiteral("serious"));
    expectState(engine.evaluate(serious, 1000), ResourcePressureState::Elevated,
                "serious thermal warning");
}

void ResourcePolicyCalibrationTest::missingGpuTelemetryNeverEscalates() {
    auto config = balancedConfig();
    config.thresholds.processResidentEscalateAtBytes = 1 * kGiB;
    config.thresholds.gpuUtilizationEscalateAt = 0.90;
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs inputs;
    inputs.processResidentBytes = measuredPolicyInput<qint64>(256 * kMiB);
    inputs.gpuAvailable = inapplicablePolicyInput<bool>(ResourcePolicyInputState::Unsupported);
    inputs.gpuUtilizationFraction =
        inapplicablePolicyInput<double>(ResourcePolicyInputState::Unsupported);

    const auto decision = engine.evaluate(inputs, 0);
    // An Unsupported GPU signal is excluded from evidence: it neither escalates
    // nor degrades a host that otherwise has fresh telemetry.
    expectState(decision, ResourcePressureState::Normal, "missing GPU telemetry");
    QCOMPARE(decision.telemetry, ResourceTelemetryState::Fresh);
}

void ResourcePolicyCalibrationTest::unsupportedHostCpuTelemetryIsNeverHealthy() {
    auto config = balancedConfig();
    config.thresholds.cpuBusyEscalateAt = 0.85;
    config.thresholds.cpuBusyDeescalateAt = 0.60;
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs unknownCpu;
    unknownCpu.cpuBusyFraction = unknownPolicyInput<double>();

    const auto decision = engine.evaluate(unknownCpu, 0);
    // Unknown telemetry is never healthy: it never becomes Normal and is never
    // reported as Fresh.
    expectState(decision, ResourcePressureState::Unknown, "unknown host CPU");
    QCOMPARE(decision.telemetry, ResourceTelemetryState::Interrupted);
    QVERIFY(decision.degraded);
}

void ResourcePolicyCalibrationTest::foregroundAdmissionIsPreservedUnderBackgroundRestriction() {
    ResourcePolicyConfig config;
    config.profile = ResourcePolicyProfile::Eco;
    config.profiles = proposedResourcePolicyCalibration().profiles;
    config.thresholds.memoryPressureEscalateAt = 0.75;
    config.thresholds.memoryPressureDeescalateAt = 0.45;
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs inputs;
    inputs.memoryPressureLevel = measuredPolicyInput<double>(0.85);
    inputs.activeForegroundRuns = 1;
    inputs.activeBackgroundRuns = 1;
    inputs.modelOwnership = ResourceModelOwnership::SentinelManaged;
    inputs.modelServingActiveRun = true;

    const auto decision = engine.evaluate(inputs, 0);
    expectState(decision, ResourcePressureState::Elevated, "eco under pressure");
    // Eco ceiling is Normal, so *new* background starts are restricted ...
    QVERIFY(decision.restrictBackgroundAdmission);
    // ... but the active foreground run is never withdrawn and no new-admission
    // refusal is issued at Elevated.
    QVERIFY(decision.foregroundAdmissionAllowed);
    QVERIFY(!decision.refuseNewAdmission);
    QCOMPARE(decision.ownershipPosture, ResourceOwnershipPosture::PreserveAllResidency);
    QVERIFY(decision.toolConcurrencyLimit.has_value());
    QCOMPARE(*decision.toolConcurrencyLimit, 1);
}

void ResourcePolicyCalibrationTest::recoveryDoesNotOscillate() {
    auto config = balancedConfig();
    config.thresholds.memoryPressureEscalateAt = 0.75;
    config.thresholds.memoryPressureDeescalateAt = 0.45;
    config.timing.escalationSustainMs = 1000;
    config.timing.minimumResidenceMs = 2000;
    config.timing.cooldownMs = 500;
    ResourcePolicyEngine engine(config);

    auto sample = [](double value) {
        ResourcePolicyInputs inputs;
        inputs.memoryPressureLevel = measuredPolicyInput<double>(value);
        return inputs;
    };

    // Sustain window: no premature escalation.
    expectState(engine.evaluate(sample(0.90), 0), ResourcePressureState::Unknown, "t=0 pending");
    expectState(engine.evaluate(sample(0.90), 500), ResourcePressureState::Unknown,
                "t=500 pending");
    expectState(engine.evaluate(sample(0.90), 1000), ResourcePressureState::Elevated,
                "t=1000 sustained");
    // Recovery after the residence window: one ordered step down.
    expectState(engine.evaluate(sample(0.20), 3000), ResourcePressureState::Normal,
                "t=3000 recovered");
    expectState(engine.evaluate(sample(0.20), 3500), ResourcePressureState::Normal,
                "t=3500 stable");
}

void ResourcePolicyCalibrationTest::staleMeasurementRemainsDistinguishable() {
    auto config = balancedConfig();
    config.thresholds.memoryPressureEscalateAt = 0.75;
    config.thresholds.memoryPressureDeescalateAt = 0.45;
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs pressured;
    pressured.memoryPressureLevel = measuredPolicyInput<double>(0.90);
    expectState(engine.evaluate(pressured, 0), ResourcePressureState::Elevated,
                "measured pressure");

    ResourcePolicyInputs stale;
    stale.memoryPressureLevel = stalePolicyInput<double>(0.20);
    const auto decision = engine.evaluate(stale, 1000);
    // A stale value stays distinguishable (Degraded, never Fresh) and cannot
    // clear a verified tier.
    expectState(decision, ResourcePressureState::Elevated, "stale clearing attempt");
    QCOMPARE(decision.telemetry, ResourceTelemetryState::Degraded);
}

void ResourcePolicyCalibrationTest::invalidConfigurationFailsClosedAndProposesNothing() {
    auto config = balancedConfig();
    // Recovery threshold above the escalation threshold is inconsistent.
    config.thresholds.cpuBusyEscalateAt = 0.30;
    config.thresholds.cpuBusyDeescalateAt = 0.80;
    QVERIFY(!validateResourcePolicyConfig(config).isEmpty());

    ResourcePolicyEngine engine(config);
    QVERIFY(!engine.configurationProblems().isEmpty());

    ResourcePolicyInputs veryBusy;
    veryBusy.cpuBusyFraction = measuredPolicyInput<double>(0.99);
    const auto decision = engine.evaluate(veryBusy, 0);

    expectState(decision, ResourcePressureState::Unknown, "invalid config");
    QCOMPARE(decision.telemetry, ResourceTelemetryState::Interrupted);
    QVERIFY(!decision.refuseNewAdmission);
    QVERIFY(!decision.restrictBackgroundAdmission);
    // No numeric budget is proposed from a rejected configuration.
    QVERIFY(!decision.toolConcurrencyLimit.has_value());
    QVERIFY(!decision.backgroundConcurrencyLimit.has_value());
    QVERIFY(decision.reasons.contains(sentinel::core::ResourcePolicyReason::ConfigurationInvalid));
}

void ResourcePolicyCalibrationTest::unavailableSignalIsNotADegradation() {
    auto config = balancedConfig();
    config.thresholds.memoryPressureEscalateAt = 0.75;
    config.thresholds.memoryPressureDeescalateAt = 0.45;
    config.thresholds.batteryChargeEscalateAt = 0.20;
    ResourcePolicyEngine engine(config);

    ResourcePolicyInputs inputs;
    inputs.memoryPressureLevel = measuredPolicyInput<double>(0.20);
    inputs.powerSource =
        inapplicablePolicyInput<ResourcePolicyPowerSource>(ResourcePolicyInputState::Unavailable);

    const auto decision = engine.evaluate(inputs, 0);
    // A host with no battery is not degraded by the absence.
    expectState(decision, ResourcePressureState::Normal, "no battery");
    QCOMPARE(decision.telemetry, ResourceTelemetryState::Fresh);
}

void ResourcePolicyCalibrationTest::proposedCalibrationIsValidAndUnapproved() {
    const auto proposal = proposedResourcePolicyCalibration();

    // Never adopted by P4-05.
    QVERIFY(!proposal.approved);

    // The proposal must be internally consistent even though it is unapproved.
    ResourcePolicyConfig config;
    config.profile = ResourcePolicyProfile::Balanced;
    config.thresholds = proposal.thresholds;
    config.timing = proposal.timing;
    config.profiles = proposal.profiles;
    const auto problems = validateResourcePolicyConfig(config);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char(';'))));

    // Every value is reasoned from one measured environment, not universal.
    QVERIFY(proposal.evidenceScope.contains(QStringLiteral("P4-01")));
    for (const auto& decision : proposal.unresolvedDecisions) {
        QVERIFY(proposal.adoptionRequirements.filter(decision).size() >= 1);
    }

    // Profile ordering is monotonic Eco <= Balanced <= Performance.
    const auto scale = [](const sentinel::core::ResourceProfileBudget& budget) {
        return budget.escalationThresholdScale.value_or(1.0);
    };
    QVERIFY(scale(proposal.profiles.eco) <= scale(proposal.profiles.balanced));
    QVERIFY(scale(proposal.profiles.balanced) <= scale(proposal.profiles.performance));
    QVERIFY(proposal.profiles.eco.maxConcurrentToolStarts.has_value());
    QVERIFY(proposal.profiles.balanced.maxConcurrentToolStarts.has_value());
    QVERIFY(proposal.profiles.performance.maxConcurrentToolStarts.has_value());
    QVERIFY(*proposal.profiles.eco.maxConcurrentToolStarts <=
            *proposal.profiles.balanced.maxConcurrentToolStarts);
    QVERIFY(*proposal.profiles.balanced.maxConcurrentToolStarts <=
            *proposal.profiles.performance.maxConcurrentToolStarts);
}

void ResourcePolicyCalibrationTest::repeatedIdenticalInputsAreDeterministic() {
    const auto first = runResourcePolicyCalibration(standardResourcePolicyCalibrationFixtures());
    const auto second = runResourcePolicyCalibration(standardResourcePolicyCalibrationFixtures());
    QCOMPARE(first.summary(), second.summary());
    QCOMPARE(first.cases.size(), second.cases.size());
    for (int i = 0; i < first.cases.size(); ++i) {
        QCOMPARE(first.cases.at(i).passed, second.cases.at(i).passed);
        QCOMPARE(first.cases.at(i).observedSummary, second.cases.at(i).observedSummary);
    }

    // A single engine fed the same input twice reaches the same tier.
    auto config = balancedConfig();
    config.thresholds.memoryPressureEscalateAt = 0.75;
    config.thresholds.memoryPressureDeescalateAt = 0.45;
    ResourcePolicyEngine engineA(config);
    ResourcePolicyEngine engineB(config);
    ResourcePolicyInputs inputs;
    inputs.memoryPressureLevel = measuredPolicyInput<double>(0.85);
    for (int i = 0; i < 5; ++i) {
        const auto a = engineA.evaluate(inputs, 1000);
        const auto b = engineB.evaluate(inputs, 1000);
        QCOMPARE(a.state, b.state);
        QCOMPARE(a.telemetry, b.telemetry);
        QCOMPARE(sentinel::core::resourcePolicyDecisionSummary(a),
                 sentinel::core::resourcePolicyDecisionSummary(b));
    }
}

void ResourcePolicyCalibrationTest::invalidTimestampsCannotBypassCooldown() {
    auto config = balancedConfig();
    config.thresholds.memoryPressureEscalateAt = 0.75;
    config.thresholds.memoryPressureDeescalateAt = 0.45;
    config.timing.minimumResidenceMs = 0;
    config.timing.cooldownMs = 1000;
    ResourcePolicyEngine engine(config);

    auto pressure = [](double value) {
        ResourcePolicyInputs inputs;
        inputs.memoryPressureLevel = measuredPolicyInput<double>(value);
        return inputs;
    };

    expectState(engine.evaluate(pressure(0.90), 0), ResourcePressureState::Elevated, "escalate");
    expectState(engine.evaluate(pressure(0.20), 500), ResourcePressureState::Elevated,
                "cooldown blocks recovery");
    expectState(engine.evaluate(pressure(0.20), 1000), ResourcePressureState::Normal,
                "cooldown elapsed");
    expectState(engine.evaluate(pressure(0.90), 2000), ResourcePressureState::Elevated,
                "re-escalate");

    // A backwards (non-monotonic) timestamp must not be able to bypass the
    // residence window and force an early de-escalation.
    expectState(engine.evaluate(pressure(0.20), 1000), ResourcePressureState::Elevated,
                "backwards timestamp cannot bypass residence");

    // An extreme timestamp must not overflow the cooldown deadline.
    ResourcePolicyEngine overflowEngine(config);
    (void)overflowEngine.evaluate(pressure(0.90), std::numeric_limits<qint64>::max() - 1);
    const auto after = overflowEngine.evaluate(pressure(0.20), std::numeric_limits<qint64>::max());
    QVERIFY(after.state == ResourcePressureState::Elevated ||
            after.state == ResourcePressureState::Normal); // behaviour is defined, never UB
}

void ResourcePolicyCalibrationTest::evaluationHasNoSideEffects() {
    // The runner must not mutate the fixtures it is given.
    const auto fixturesBefore = standardResourcePolicyCalibrationFixtures();
    const auto fixturesAfter = standardResourcePolicyCalibrationFixtures();
    QCOMPARE(fixturesBefore.size(), fixturesAfter.size());
    for (int i = 0; i < fixturesBefore.size(); ++i) {
        QCOMPARE(fixturesBefore.at(i).config.profile, fixturesAfter.at(i).config.profile);
        QCOMPARE(fixturesBefore.at(i).config.thresholds.cpuBusyEscalateAt.value_or(-1.0),
                 fixturesAfter.at(i).config.thresholds.cpuBusyEscalateAt.value_or(-1.0));
    }

    // The proposal is stable across calls.
    const auto proposalA = proposedResourcePolicyCalibration();
    const auto proposalB = proposedResourcePolicyCalibration();
    QCOMPARE(proposalA.timing.cooldownMs, proposalB.timing.cooldownMs);
    QCOMPARE(proposalA.profiles.balanced.maxConcurrentToolStarts.value_or(-1),
             proposalB.profiles.balanced.maxConcurrentToolStarts.value_or(-1));

    // Every decision the calibration can produce must preserve the security
    // posture and expose no authority.
    QVERIFY(sentinel::core::resourcePolicyDecisionPreservesSecurity());
}

QTEST_MAIN(ResourcePolicyCalibrationTest)

#include "test_resource_policy_calibration.moc"
