// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/policy/ResourcePolicyShadowEvaluation.h"

#include <algorithm>

namespace sentinel::core {

namespace {

int clampShadowMaxRecords(int requested) {
    return requested < 1 ? 1 : requested;
}

QString boundedDiagnostics(QString text) {
    if (text.size() > kResourcePolicyShadowMaxDiagnosticLength) {
        text.truncate(kResourcePolicyShadowMaxDiagnosticLength);
    }
    return text;
}

} // namespace

QString resourcePolicyShadowDiagnostics(const ResourcePolicyDecision& decision) {
    // Built exclusively from enumerated names and numbers. `ResourcePolicyDecision`
    // has no free-text field, and this function introduces none, so a prompt,
    // credential or personal path cannot reach the output.
    QStringList actions;
    actions.reserve(decision.actions.size());
    for (const auto action : decision.actions) {
        actions.append(resourcePolicyActionName(action));
    }
    const QString toolLimit = decision.toolConcurrencyLimit.has_value()
                                  ? QString::number(*decision.toolConcurrencyLimit)
                                  : QStringLiteral("unset");
    return boundedDiagnostics(
        QStringLiteral("shadow profile=%1 state=%2 telemetry=%3 actions=[%4] "
                       "refuseNewAdmission=%5 restrictBackgroundAdmission=%6 toolLimit=%7 "
                       "degraded=%8")
            .arg(resourcePolicyProfileName(decision.profile),
                 resourcePressureStateName(decision.state),
                 resourceTelemetryStateName(decision.telemetry), actions.join(QLatin1Char(',')),
                 decision.refuseNewAdmission ? QStringLiteral("1") : QStringLiteral("0"),
                 decision.restrictBackgroundAdmission ? QStringLiteral("1") : QStringLiteral("0"),
                 toolLimit, decision.degraded ? QStringLiteral("1") : QStringLiteral("0")));
}

ResourcePolicyShadowEvaluator::ResourcePolicyShadowEvaluator(ResourcePolicyShadowConfig config)
    : config_(std::move(config)), engine_(config_.policy) {
    config_.maxRecords = clampShadowMaxRecords(config_.maxRecords);
}

void ResourcePolicyShadowEvaluator::setEnabled(bool enabled) {
    config_.enabled = enabled;
}

void ResourcePolicyShadowEvaluator::setPolicy(ResourcePolicyConfig config) {
    config_.policy = std::move(config);
    engine_.setConfig(config_.policy);
}

void ResourcePolicyShadowEvaluator::clearRecords() {
    records_.clear();
}

void ResourcePolicyShadowEvaluator::reset() {
    engine_.reset();
}

void ResourcePolicyShadowEvaluator::appendRecord(ResourcePolicyShadowRecord record) {
    records_.append(std::move(record));
    while (static_cast<int>(records_.size()) > config_.maxRecords) {
        records_.removeFirst();
    }
}

std::optional<ResourcePolicyShadowRecord>
ResourcePolicyShadowEvaluator::observe(const ResourcePolicyInputs& inputs, qint64 nowMonotonicMs) {
    if (!config_.enabled) {
        // Disabled: no evaluation, no state change, no record.
        return std::nullopt;
    }

    const ResourcePolicyDecision decision = engine_.evaluate(inputs, nowMonotonicMs);

    ResourcePolicyShadowRecord record;
    record.evaluatedAtMonotonicMs = nowMonotonicMs;
    record.profile = decision.profile;
    record.state = decision.state;
    record.telemetry = decision.telemetry;
    record.wouldRefuseNewAdmission = decision.refuseNewAdmission;
    record.wouldRestrictBackgroundAdmission = decision.restrictBackgroundAdmission;
    record.wouldLimitToolConcurrencyTo = decision.toolConcurrencyLimit;
    record.isTransition = decision.isTransition;
    record.diagnostics = resourcePolicyShadowDiagnostics(decision);

    appendRecord(record);
    return record;
}

std::optional<ResourcePolicyShadowRecord>
ResourcePolicyShadowEvaluator::observe(const ResourcePolicyTelemetryMapping& mapping,
                                       qint64 nowMonotonicMs) {
    // Only the translated inputs are observed. `mapping.diagnostics` (sourceId,
    // detail, provenance) is intentionally ignored so no free text can leak.
    return observe(mapping.inputs, nowMonotonicMs);
}

QString ResourcePolicyShadowEvaluator::summary() const {
    return QStringLiteral("shadow enabled=%1 records=%2/%3")
        .arg(config_.enabled ? QStringLiteral("1") : QStringLiteral("0"))
        .arg(records_.size())
        .arg(config_.maxRecords);
}

} // namespace sentinel::core
