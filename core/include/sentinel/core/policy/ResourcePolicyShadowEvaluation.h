// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// P4-05 shadow policy evaluation: DISABLED BY DEFAULT, strictly observational.
//
// The shadow evaluator computes *hypothetical* policy decisions so a proposed
// calibration can be observed in place before anyone adopts it. It is not wired
// into any production path and it can enforce nothing:
//
//   * it owns no scheduler, executor, gateway, provider, model runtime or process
//     handle, and holds no reference to any of them;
//   * its output type carries no authority — there is no permission, approval,
//     sandbox, routing, ownership or process field, and no destructive verb in
//     the action vocabulary it can echo;
//   * `observe` never rejects an Agent request, restricts a real tool execution,
//     changes tool concurrency, unloads a model, terminates a process, changes
//     provider selection, changes security permissions, changes the active
//     profile or modifies any resource budget. It returns data and touches
//     nothing else.
//
// Default state is DISABLED: while disabled `observe` performs no evaluation at
// all and returns `std::nullopt`.
//
// Diagnostics are bounded and redacted. The record is built exclusively from
// enumerated names and numbers, so a prompt, a credential or a personal path can
// never reach the output even if it is present in an upstream telemetry mapping.

#include "sentinel/core/policy/ResourcePolicy.h"
#include "sentinel/core/policy/ResourcePolicyTelemetryAdapter.h"

#include <QList>
#include <QString>

#include <optional>

namespace sentinel::core {

// Hard bound on a single redacted diagnostics line. The line is constructed from
// enumerations, so this is a defensive ceiling rather than a normal limit.
inline constexpr int kResourcePolicyShadowMaxDiagnosticLength = 512;
inline constexpr int kResourcePolicyShadowDefaultMaxRecords = 128;

// One hypothetical observation. This is data, never a directive: it exposes what
// the policy *would have* proposed so a maintainer can review a calibration
// before adopting it.
struct ResourcePolicyShadowRecord {
    qint64 evaluatedAtMonotonicMs = 0;
    ResourcePolicyProfile profile = ResourcePolicyProfile::Balanced;
    ResourcePressureState state = ResourcePressureState::Unknown;
    ResourceTelemetryState telemetry = ResourceTelemetryState::Interrupted;
    bool wouldRefuseNewAdmission = false;
    bool wouldRestrictBackgroundAdmission = false;
    std::optional<int> wouldLimitToolConcurrencyTo;
    bool isTransition = false;
    // Bounded, redacted, non-localized diagnostics: enumerated names and numbers
    // only. Never a prompt, credential or personal path.
    QString diagnostics;
};

struct ResourcePolicyShadowConfig {
    // Disabled by default. Nothing is evaluated unless a caller enables it.
    bool enabled = false;
    // The proposed policy to observe in shadow mode.
    ResourcePolicyConfig policy;
    // Bounded retention of observations; the ring never holds more than this.
    // Values below one are raised to one.
    int maxRecords = kResourcePolicyShadowDefaultMaxRecords;
};

// A single-thread-owned, deterministic shadow evaluator. It is not thread-safe
// and must be externally serialized, exactly like `ResourcePolicyEngine`.
class ResourcePolicyShadowEvaluator final {
public:
    explicit ResourcePolicyShadowEvaluator(ResourcePolicyShadowConfig config = {});

    bool enabled() const {
        return config_.enabled;
    }
    // Enabling or disabling never touches anything outside this object.
    void setEnabled(bool enabled);

    // Replaces the observed policy and resets the shadow engine's hysteresis
    // state. Never touches any production state.
    void setPolicy(ResourcePolicyConfig config);
    const ResourcePolicyConfig& policy() const {
        return config_.policy;
    }

    int maxRecords() const {
        return config_.maxRecords;
    }

    // Evaluates one input snapshot in shadow mode. Returns `std::nullopt` when
    // disabled (no evaluation is performed at all). Never throws, never blocks,
    // never touches anything outside this object.
    std::optional<ResourcePolicyShadowRecord> observe(const ResourcePolicyInputs& inputs,
                                                      qint64 nowMonotonicMs);

    // Convenience overload for an already-translated telemetry mapping. The
    // mapping's free-text provenance (sourceId/detail) is deliberately dropped:
    // only the translated inputs enter the shadow policy, so nothing sensitive
    // can leak into a record.
    std::optional<ResourcePolicyShadowRecord> observe(const ResourcePolicyTelemetryMapping& mapping,
                                                      qint64 nowMonotonicMs);

    // Bounded copies of retained observations, oldest first.
    QList<ResourcePolicyShadowRecord> records() const {
        return records_;
    }
    int recordCount() const {
        return static_cast<int>(records_.size());
    }
    void clearRecords();
    // Resets shadow hysteresis state only; retained records are kept.
    void reset();

    // Deterministic, bounded summary containing only names, numbers and the
    // record count.
    QString summary() const;

private:
    void appendRecord(ResourcePolicyShadowRecord record);

    ResourcePolicyShadowConfig config_;
    ResourcePolicyEngine engine_;
    QList<ResourcePolicyShadowRecord> records_;
};

// Formats one decision as a bounded, redacted diagnostic line. Deliberately uses
// only the decision's enumerated names and numbers, so no free text from any
// telemetry source can be echoed.
QString resourcePolicyShadowDiagnostics(const ResourcePolicyDecision& decision);

} // namespace sentinel::core
