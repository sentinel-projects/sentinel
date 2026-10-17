// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// P4-05 shadow evaluation tests: disabled by default, strictly observational,
// bounded and redacted. No case reads host telemetry, starts a thread, loads a
// model or touches a process.

#include "sentinel/core/policy/ResourcePolicy.h"
#include "sentinel/core/policy/ResourcePolicyCalibration.h"
#include "sentinel/core/policy/ResourcePolicyShadowEvaluation.h"
#include "sentinel/core/policy/ResourcePolicyTelemetryAdapter.h"
#include "sentinel/core/resource/ResourceTelemetry.h"

#include <QtTest>

#include <type_traits>
#include <utility>

using sentinel::core::HostResourceSnapshot;
using sentinel::core::measuredMetric;
using sentinel::core::measuredPolicyInput;
using sentinel::core::proposedResourcePolicyCalibration;
using sentinel::core::ResourceMeasurementOrigin;
using sentinel::core::ResourcePolicyConfig;
using sentinel::core::ResourcePolicyInputs;
using sentinel::core::ResourcePolicyShadowConfig;
using sentinel::core::ResourcePolicyShadowEvaluator;
using sentinel::core::ResourcePolicyShadowRecord;
using sentinel::core::ResourcePolicyTelemetryAdapter;
using sentinel::core::ResourcePolicyTelemetryMapping;
using sentinel::core::ResourcePressureState;
using sentinel::core::ResourceReliability;
using sentinel::core::ResourceSampleTimestamp;
using sentinel::core::ResourceTelemetryState;

namespace {

constexpr qint64 kGiB = 1024LL * 1024 * 1024;

// Compile-time structural pins: the shadow output type must be unable to express
// authority or ownership. A field named `permission`, `sandbox`, `approval`,
// `provider`, `processId`, `modelHandle`, `routingMode` or `budget` would fail to
// compile.
template <typename T, typename = void> struct HasPermissionField : std::false_type {};
template <typename T>
struct HasPermissionField<T, std::void_t<decltype(std::declval<T>().permission)>> : std::true_type {
};

template <typename T, typename = void> struct HasSandboxField : std::false_type {};
template <typename T>
struct HasSandboxField<T, std::void_t<decltype(std::declval<T>().sandbox)>> : std::true_type {};

template <typename T, typename = void> struct HasApprovalField : std::false_type {};
template <typename T>
struct HasApprovalField<T, std::void_t<decltype(std::declval<T>().approval)>> : std::true_type {};

template <typename T, typename = void> struct HasProviderField : std::false_type {};
template <typename T>
struct HasProviderField<T, std::void_t<decltype(std::declval<T>().provider)>> : std::true_type {};

template <typename T, typename = void> struct HasProcessIdField : std::false_type {};
template <typename T>
struct HasProcessIdField<T, std::void_t<decltype(std::declval<T>().processId)>> : std::true_type {};

template <typename T, typename = void> struct HasModelHandleField : std::false_type {};
template <typename T>
struct HasModelHandleField<T, std::void_t<decltype(std::declval<T>().modelHandle)>>
    : std::true_type {};

template <typename T, typename = void> struct HasRoutingModeField : std::false_type {};
template <typename T>
struct HasRoutingModeField<T, std::void_t<decltype(std::declval<T>().routingMode)>>
    : std::true_type {};

template <typename T, typename = void> struct HasBudgetField : std::false_type {};
template <typename T>
struct HasBudgetField<T, std::void_t<decltype(std::declval<T>().budget)>> : std::true_type {};

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

// A policy that watches memory pressure and thermal level; unapproved proposal
// budgets, exactly as the calibration foundation would supply them.
ResourcePolicyConfig shadowPolicy() {
    ResourcePolicyConfig config;
    config.profile = sentinel::core::ResourcePolicyProfile::Balanced;
    config.profiles = proposedResourcePolicyCalibration().profiles;
    config.thresholds.memoryPressureEscalateAt = 0.75;
    config.thresholds.memoryPressureDeescalateAt = 0.45;
    config.thresholds.thermalWarningLevels = {QStringLiteral("serious")};
    return config;
}

ResourcePolicyInputs pressuredInputs() {
    ResourcePolicyInputs inputs;
    inputs.memoryPressureLevel = measuredPolicyInput<double>(0.90);
    inputs.thermalLevel = measuredPolicyInput<QString>(QStringLiteral("nominal"));
    return inputs;
}

ResourcePolicyInputs calmInputs() {
    ResourcePolicyInputs inputs;
    inputs.memoryPressureLevel = measuredPolicyInput<double>(0.10);
    inputs.thermalLevel = measuredPolicyInput<QString>(QStringLiteral("nominal"));
    return inputs;
}

} // namespace

class ResourcePolicyShadowTest final : public QObject {
    Q_OBJECT
private slots:
    void disabledByDefaultPerformsNoEvaluation();
    void enabledProducesObservationalRecords();
    void recordsAreBounded();
    void diagnosticsAreRedacted();
    void mappingFreeTextIsDropped();
    void observationsAreDeterministic();
    void shadowDiagnosticsContainNoDestructiveVerb();
    void shadowNeverChangesInputsOrPolicy();
    void resetClearsHysteresisButKeepsRecords();
    void summaryIsBounded();
};

void ResourcePolicyShadowTest::disabledByDefaultPerformsNoEvaluation() {
    ResourcePolicyShadowEvaluator evaluator; // default: disabled
    QVERIFY(!evaluator.enabled());

    // Even a maximal-pressure input must produce nothing while disabled.
    ResourcePolicyInputs inputs;
    inputs.memoryPressureLevel = measuredPolicyInput<double>(1.0);
    inputs.activeForegroundRuns = 1;
    inputs.activeBackgroundRuns = 3;
    const auto result = evaluator.observe(inputs, 0);
    QVERIFY(!result.has_value());
    QCOMPARE(evaluator.recordCount(), 0);
    QCOMPARE(evaluator.records().size(), 0);
}

void ResourcePolicyShadowTest::enabledProducesObservationalRecords() {
    ResourcePolicyShadowConfig config;
    config.enabled = true;
    config.policy = shadowPolicy();
    ResourcePolicyShadowEvaluator evaluator(config);

    const auto observation = evaluator.observe(pressuredInputs(), 1000);
    QVERIFY(observation.has_value());
    QCOMPARE(observation->state, ResourcePressureState::Elevated);
    QCOMPARE(observation->telemetry, ResourceTelemetryState::Fresh);
    QCOMPARE(observation->evaluatedAtMonotonicMs, 1000);
    QCOMPARE(evaluator.recordCount(), 1);
    // The observation is data, not an enforcement: it reports a hypothetical
    // admission posture but performs no action.
    QVERIFY(observation->wouldRefuseNewAdmission ==
            (observation->state == ResourcePressureState::Critical));
}

void ResourcePolicyShadowTest::recordsAreBounded() {
    ResourcePolicyShadowConfig config;
    config.enabled = true;
    config.policy = shadowPolicy();
    config.maxRecords = 4;
    ResourcePolicyShadowEvaluator evaluator(config);

    for (int i = 0; i < 10; ++i) {
        (void)evaluator.observe(pressuredInputs(), i * 100);
    }
    QCOMPARE(evaluator.recordCount(), 4);
    QVERIFY(evaluator.recordCount() <= evaluator.maxRecords());

    // The ring keeps the most recent observations (oldest first).
    const auto records = evaluator.records();
    QCOMPARE(records.size(), 4);
    QCOMPARE(records.first().evaluatedAtMonotonicMs, qint64(600));
    QCOMPARE(records.last().evaluatedAtMonotonicMs, qint64(900));

    // A caller cannot request an unbounded ring.
    ResourcePolicyShadowConfig zero;
    zero.enabled = true;
    zero.maxRecords = 0;
    ResourcePolicyShadowEvaluator zeroEvaluator(zero);
    QCOMPARE(zeroEvaluator.maxRecords(), 1);
}

void ResourcePolicyShadowTest::diagnosticsAreRedacted() {
    ResourcePolicyShadowConfig config;
    config.enabled = true;
    config.policy = shadowPolicy();
    ResourcePolicyShadowEvaluator evaluator(config);

    ResourcePolicyInputs inputs = pressuredInputs();
    // A hostile value: a personal path smuggled in through the one string input
    // the policy contract has. It must never reach the diagnostics.
    const QString hostilePath =
        QStringLiteral("/Users") + QStringLiteral("/alice/secret/prompt.txt");
    inputs.thermalLevel = measuredPolicyInput<QString>(hostilePath);

    const auto observation = evaluator.observe(inputs, 0);
    QVERIFY(observation.has_value());
    const QString diagnostics = observation->diagnostics;
    QVERIFY(!diagnostics.isEmpty());
    QVERIFY(!diagnostics.contains(QStringLiteral("alice")));
    QVERIFY(!diagnostics.contains(QStringLiteral("/Users")));
    QVERIFY(!diagnostics.contains(QStringLiteral("prompt")));
    QVERIFY(diagnostics.size() <= sentinel::core::kResourcePolicyShadowMaxDiagnosticLength);
}

void ResourcePolicyShadowTest::mappingFreeTextIsDropped() {
    // A full P4-04 mapping whose provenance detail carries a credential-like
    // string and a personal path.
    ResourceSampleTimestamp capturedAt;
    capturedAt.monotonicMs = 0;

    HostResourceSnapshot snapshot;
    snapshot.adapterId = QStringLiteral("test.native");
    snapshot.capturedAt = capturedAt;
    snapshot.hostMemory.availableBytes =
        measuredMetric<qint64>(8 * kGiB, capturedAt, ResourceMeasurementOrigin::HostKernel,
                               ResourceReliability::Authoritative, QStringLiteral("test.mem"));
    const QString syntheticHome = QStringLiteral("/home") + QStringLiteral("/bob/private/notes.md");
    snapshot.thermal.level = measuredMetric<QString>(
        syntheticHome, capturedAt, ResourceMeasurementOrigin::PlatformService,
        ResourceReliability::Approximate, QStringLiteral("test.thermal"));

    ResourcePolicyTelemetryAdapter adapter;
    ResourcePolicyTelemetryMapping mapping = adapter.adapt(snapshot, 0);
    mapping.diagnostics.thermalLevel.detail = QStringLiteral("Bearer sk-live-do-not-leak");
    const QString syntheticSshPath = QStringLiteral("/Users") + QStringLiteral("/bob/.ssh/id_rsa");
    mapping.diagnostics.hostAvailableBytes.detail = syntheticSshPath;

    ResourcePolicyShadowConfig config;
    config.enabled = true;
    config.policy = shadowPolicy();
    ResourcePolicyShadowEvaluator evaluator(config);

    const auto observation = evaluator.observe(mapping, 0);
    QVERIFY(observation.has_value());
    const QString diagnostics = observation->diagnostics;
    QVERIFY(!diagnostics.contains(QStringLiteral("Bearer")));
    QVERIFY(!diagnostics.contains(QStringLiteral("sk-live")));
    QVERIFY(!diagnostics.contains(QStringLiteral(".ssh")));
    QVERIFY(!diagnostics.contains(QStringLiteral("bob")));
    QVERIFY(!diagnostics.contains(QStringLiteral("notes.md")));
}

void ResourcePolicyShadowTest::observationsAreDeterministic() {
    auto makeEvaluator = [] {
        ResourcePolicyShadowConfig config;
        config.enabled = true;
        config.policy = shadowPolicy();
        return ResourcePolicyShadowEvaluator(config);
    };
    auto first = makeEvaluator();
    auto second = makeEvaluator();

    const double values[] = {0.10, 0.90, 0.90, 0.20, 0.20, 0.95};
    for (int i = 0; i < 6; ++i) {
        ResourcePolicyInputs inputs;
        inputs.memoryPressureLevel = measuredPolicyInput<double>(values[i]);
        const auto a = first.observe(inputs, i * 500);
        const auto b = second.observe(inputs, i * 500);
        QCOMPARE(a.has_value(), b.has_value());
        QCOMPARE(a->state, b->state);
        QCOMPARE(a->telemetry, b->telemetry);
        QCOMPARE(a->diagnostics, b->diagnostics);
    }
    QCOMPARE(first.summary(), second.summary());
}

void ResourcePolicyShadowTest::shadowDiagnosticsContainNoDestructiveVerb() {
    const QStringList forbidden = {QStringLiteral("unload"), QStringLiteral("terminate"),
                                   QStringLiteral("kill"),   QStringLiteral("grant"),
                                   QStringLiteral("evict"),  QStringLiteral("permission")};

    // The closed action vocabulary never contains a destructive verb.
    const sentinel::core::ResourcePolicyAction actions[] = {
        sentinel::core::ResourcePolicyAction::None,
        sentinel::core::ResourcePolicyAction::RefuseNewAdmission,
        sentinel::core::ResourcePolicyAction::RestrictBackgroundAdmission,
        sentinel::core::ResourcePolicyAction::LimitToolConcurrency,
    };
    for (const auto action : actions) {
        const QString name = sentinel::core::resourcePolicyActionName(action).toLower();
        for (const auto& word : forbidden) {
            QVERIFY2(!name.contains(word),
                     qPrintable(QStringLiteral("action %1 contains %2").arg(name, word)));
        }
    }

    ResourcePolicyShadowConfig config;
    config.enabled = true;
    config.policy = shadowPolicy();
    ResourcePolicyShadowEvaluator evaluator(config);
    for (int i = 0; i < 8; ++i) {
        ResourcePolicyInputs inputs;
        inputs.memoryPressureLevel = measuredPolicyInput<double>(i % 2 == 0 ? 0.95 : 0.10);
        inputs.activeBackgroundRuns = i;
        const auto observation = evaluator.observe(inputs, i * 100);
        const QString diagnostics = observation->diagnostics.toLower();
        for (const auto& word : forbidden) {
            QVERIFY2(!diagnostics.contains(word),
                     qPrintable(QStringLiteral("diagnostics contains %1").arg(word)));
        }
    }
}

void ResourcePolicyShadowTest::shadowNeverChangesInputsOrPolicy() {
    ResourcePolicyShadowConfig config;
    config.enabled = true;
    config.policy = shadowPolicy();
    ResourcePolicyShadowEvaluator evaluator(config);

    const ResourcePolicyConfig policyBefore = evaluator.policy();
    (void)evaluator.observe(pressuredInputs(), 0);
    const ResourcePolicyConfig policyAfter = evaluator.policy();

    // The observed policy is never rewritten by an observation.
    QCOMPARE(policyBefore.profile, policyAfter.profile);
    QCOMPARE(policyBefore.thresholds.memoryPressureEscalateAt.value_or(-1.0),
             policyAfter.thresholds.memoryPressureEscalateAt.value_or(-1.0));
    QCOMPARE(policyBefore.profiles.balanced.maxConcurrentToolStarts.value_or(-1),
             policyAfter.profiles.balanced.maxConcurrentToolStarts.value_or(-1));
    QCOMPARE(policyBefore.timing.cooldownMs, policyAfter.timing.cooldownMs);
    QVERIFY(sentinel::core::resourcePolicyDecisionPreservesSecurity());
}

void ResourcePolicyShadowTest::resetClearsHysteresisButKeepsRecords() {
    ResourcePolicyShadowConfig config;
    config.enabled = true;
    config.policy = shadowPolicy();
    config.policy.timing.cooldownMs = 10000;
    ResourcePolicyShadowEvaluator evaluator(config);

    (void)evaluator.observe(pressuredInputs(), 0);
    QCOMPARE(evaluator.recordCount(), 1);

    evaluator.reset();
    QCOMPARE(evaluator.recordCount(), 1); // records survive a hysteresis reset

    // After reset the engine starts from Unknown again, so calm evidence lands on
    // Normal rather than being held back by the previous tier.
    const auto observation = evaluator.observe(calmInputs(), 100);
    QVERIFY(observation.has_value());
    QCOMPARE(observation->state, ResourcePressureState::Normal);

    evaluator.clearRecords();
    QCOMPARE(evaluator.recordCount(), 0);
}

void ResourcePolicyShadowTest::summaryIsBounded() {
    ResourcePolicyShadowConfig config;
    config.enabled = true;
    config.policy = shadowPolicy();
    ResourcePolicyShadowEvaluator evaluator(config);
    (void)evaluator.observe(pressuredInputs(), 0);

    const QString summary = evaluator.summary();
    QVERIFY(summary.contains(QStringLiteral("enabled=1")));
    QVERIFY(summary.contains(QStringLiteral("records=1/")));
    QVERIFY(summary.size() <= sentinel::core::kResourcePolicyShadowMaxDiagnosticLength);
}

QTEST_MAIN(ResourcePolicyShadowTest)

#include "test_resource_policy_shadow.moc"
