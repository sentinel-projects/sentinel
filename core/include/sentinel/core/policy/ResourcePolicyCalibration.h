// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// P4-05 resource-policy calibration foundation (NON-PRODUCTION evaluation support).
//
// This unit exists so a proposed Eco/Balanced/Performance calibration can be
// exercised deterministically against *explicit fixtures* before any maintainer
// adopts a numeric value. It is evaluation support, not enforcement:
//
//   * It owns no scheduler, executor, gateway, provider, model runtime or
//     process handle. It cannot grant permission, change routing, unload a model
//     or terminate a process. The only vocabulary it produces is the closed
//     P4-03 decision contract plus plain numbers.
//   * Every numeric value here is either a fixture value or a proposal marked
//     `approved == false`. Nothing is a universal device threshold. The P4-01
//     baseline is evidence for one measured host (Apple M2, 16 GiB, macOS arm64)
//     and is reused only as *evidence from that environment*.
//   * It holds no thread, no timer, no IO and no clock. All time is an explicit
//     monotonic argument, so every run is reproducible.
//   * Units are preserved exactly: memory in bytes (`qint64`), fractions in
//     `0..1`, durations in monotonic milliseconds. No unit is converted or
//     normalized here.
//
// A fixture never reads host telemetry. It supplies `ResourcePolicyInputs`
// directly, exactly as the P4-04 adapter would, so a scenario is deterministic on
// every platform.

#include "sentinel/core/policy/ResourcePolicy.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace sentinel::core {

// One deterministic input sample at an explicit monotonic time. `inputs` carries
// telemetry-shaped values (never host facts), so a fixture can express any
// availability state on any platform.
struct ResourcePolicyCalibrationSample {
    qint64 atMonotonicMs = 0;
    ResourcePolicyInputs inputs;
    // Optional per-sample assertion, used to pin hysteresis: the tier the engine
    // must report *after* this sample. Empty means "not asserted here".
    std::optional<ResourcePressureState> expectStateAfter;
};

// What a fixture expects to observe. Expectations are structural facts about the
// policy contract (tier, telemetry coverage, admission posture), never a
// performance target. The safety invariants (`expectForegroundAdmissionAlwaysAllowed`,
// `expectOwnershipPreserved`, `expectPolicyPreservesSecurity`) are checked after
// *every* sample, not only the last.
struct ResourcePolicyCalibrationExpectation {
    ResourcePressureState finalState = ResourcePressureState::Unknown;
    ResourceTelemetryState finalTelemetry = ResourceTelemetryState::Interrupted;
    bool expectRefuseNewAdmission = false;
    bool expectRestrictBackgroundAdmission = false;
    // When set, the final decision's `toolConcurrencyLimit` must equal this and
    // `backgroundConcurrencyLimit` is also asserted when the budget provides one.
    std::optional<int> expectToolConcurrencyLimit;
    bool expectForegroundAdmissionAlwaysAllowed = true;
    bool expectOwnershipPreserved = true;
    bool expectPolicyPreservesSecurity = true;
};

// A named, self-contained scenario. `config` carries the fixture thresholds and
// the *proposed* profile budget under evaluation; `samples` is the scenario
// timeline.
struct ResourcePolicyCalibrationFixture {
    QString id;
    QString description;
    ResourcePolicyProfile profile = ResourcePolicyProfile::Balanced;
    ResourcePolicyConfig config;
    QList<ResourcePolicyCalibrationSample> samples;
    ResourcePolicyCalibrationExpectation expectation;
};

// Outcome of running one fixture.
struct ResourcePolicyCalibrationCaseResult {
    QString fixtureId;
    bool passed = false;
    // Deterministic, redacted one-line summary of the last observed decision.
    QString observedSummary;
    // Non-sensitive failure detail. The runner only ever writes enumerated names
    // and numbers here, never a prompt, credential or personal path.
    QString failureDetail;
    // True when this fixture's config was rejected by the validator (used by the
    // fail-closed fixture).
    bool configurationWasInvalid = false;
};

struct ResourcePolicyCalibrationReport {
    QList<ResourcePolicyCalibrationCaseResult> cases;
    int passed = 0;
    int failed = 0;

    bool allPassed() const {
        return failed == 0 && !cases.isEmpty();
    }
    QStringList failingFixtureIds() const;
    // Deterministic, bounded, non-localized summary (names and counts only).
    QString summary() const;
};

// Runs every fixture with a fresh engine and returns a deterministic report.
// Side-effect-free: no thread, no timer, no IO, no process interaction.
ResourcePolicyCalibrationReport
runResourcePolicyCalibration(const QList<ResourcePolicyCalibrationFixture>& fixtures);

// A proposed (NOT approved) numeric calibration for one profile. `approved` is
// always false: P4-05 never adopts a numeric value.
struct ResourcePolicyProfileProposal {
    ResourcePolicyProfile profile = ResourcePolicyProfile::Balanced;
    ResourceProfileBudget budget;
    bool approved = false;
    // Which measured environment the fixture values were reasoned from.
    QString evidenceScope;
    // Decisions that must be taken before this proposal can be adopted.
    QStringList unresolvedDecisions;
};

// Proposed thresholds + timings + per-profile budgets for evaluation only.
struct ResourcePolicyCalibrationProposal {
    ResourcePolicyThresholds thresholds;
    ResourcePolicyTiming timing;
    ResourceProfileBudgetSet profiles;
    bool approved = false;
    QString evidenceScope;
    QStringList unresolvedDecisions;
    // The decision record that must be satisfied before adoption; always
    // contains PD-D03, PD-D04 and PD-D10.
    QStringList adoptionRequirements;
};

// The proposed Eco/Balanced/Performance calibration. Every numeric value is a
// fixture value reasoned from the P4-01 measured host, marked `approved == false`
// and accompanied by its unresolved decisions. This function never changes
// engine or production behaviour.
ResourcePolicyCalibrationProposal proposedResourcePolicyCalibration();

// The standard scenario library (Part B): low available RAM, high process RSS,
// memory-pressure escalation, CPU contention, battery operation, thermal
// warning, missing GPU telemetry, unsupported host CPU telemetry, foreground
// versus background activity, and recovery after sustained pressure — plus the
// fail-closed and oscillation fixtures required by Part D.
QList<ResourcePolicyCalibrationFixture> standardResourcePolicyCalibrationFixtures();

} // namespace sentinel::core
