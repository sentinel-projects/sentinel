// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/agent/ObservationEvidence.h"
#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/runtime/ToolDescriptor.h"
#include "sentinel/core/runtime/ToolExecution.h"
#include "sentinel/core/runtime/ToolInvocationPlan.h"

#include <QList>
#include <QString>

namespace sentinel::core {
struct AgentPlanningContext;

struct AgentStepRecord {
    int index = 0;
    QString thought;
    QString toolId;
    QString toolName;
    QList<ToolInvocationArgument> arguments;
    QString observation;
    QString statusText;
    bool succeeded = false;
    StructuredObservationPtr structuredObservation;
    QString batchId;
    SandboxExecutionResult sandbox;
    ToolFailureCategory failureCategory = ToolFailureCategory::None;
};

struct AgentToolAction {
    QString toolId;
    QString toolName;
    ToolRiskLevel riskLevel = ToolRiskLevel::Low;
    ToolExecutionMode executionMode = ToolExecutionMode::Local;
    QList<ToolInvocationArgument> arguments;
    QList<int> dependsOn;
    QString providerCallId;
};

// Typed reason a planner stopped instead of producing another tool call.
// `AgentLoop` maps this to the run's terminal reason so a context-budget
// refusal, an indeterminate observation classification and a verification
// failure stay distinguishable instead of collapsing into one generic abort.
enum class AgentStepFailure {
    None,
    // Required instructions, execution evidence or continuation cannot fit.
    ContextBudgetExhausted,
    // Observation classification could not produce a usable intent.
    ObservationIndeterminate,
    // A required verification could not be observed or did not hold.
    VerificationFailed,
    // The provider failed and no safe recovery remains.
    ProviderFailure,
    // Every remaining action is blocked, denied or out of scope.
    Blocked,
    // The run reached its iteration limit.
    IterationLimit,
};

inline QString agentStepFailureName(AgentStepFailure failure) {
    switch (failure) {
    case AgentStepFailure::ContextBudgetExhausted:
        return QStringLiteral("ContextBudgetExhausted");
    case AgentStepFailure::ObservationIndeterminate:
        return QStringLiteral("ObservationIndeterminate");
    case AgentStepFailure::VerificationFailed:
        return QStringLiteral("VerificationFailed");
    case AgentStepFailure::ProviderFailure:
        return QStringLiteral("ProviderFailure");
    case AgentStepFailure::Blocked:
        return QStringLiteral("Blocked");
    case AgentStepFailure::IterationLimit:
        return QStringLiteral("IterationLimit");
    case AgentStepFailure::None:
        break;
    }
    return QStringLiteral("None");
}

struct AgentStepDecision {
    enum class Kind {
        ToolCall,
        FinalAnswer,
        GiveUp,
    };

    Kind kind = Kind::GiveUp;
    QString thought;
    QString toolId;
    QString toolName;
    ToolRiskLevel riskLevel = ToolRiskLevel::Low;
    ToolExecutionMode executionMode = ToolExecutionMode::Local;
    QList<ToolInvocationArgument> arguments;
    QList<AgentToolAction> toolBatch;
    QString answer;
    QString reason;
    std::optional<ProviderFailureMetadata> providerFailure;
    bool requiresObservation = false;
    bool observationRequirementDeclared = false;
    GroundingMode grounding = GroundingMode::Context;
    bool groundingDeclared = false;
    QList<ClaimAssertion> claims;
    // Typed stop reason for `Kind::GiveUp`. Absent decisions keep `None`.
    AgentStepFailure failure = AgentStepFailure::None;
};

class IAgentStepPlanner {
public:
    virtual ~IAgentStepPlanner() = default;

    virtual AgentStepDecision nextStep(const QString& goal,
                                       const QList<AgentStepRecord>& history) const = 0;
    virtual void setObservationIntent(const ObservationIntent&) {}
    virtual void setEvidence(const QList<EvidenceRecord>&) {}
    virtual void setPlannerFeedback(const QString&) {}
    virtual void setStructuredFacts(const QList<StructuredFact>&) {}
    virtual void setPlanningContext(const AgentPlanningContext&) {}
    virtual int lastProviderRecoveryAttempts() const {
        return 0;
    }
};

inline QString agentStepRecordSummary(const AgentStepRecord& record) {
    QStringList argumentParts;
    for (const auto& argument : record.arguments) {
        argumentParts.append(QStringLiteral("%1=%2").arg(argument.id, argument.value));
    }
    return QStringLiteral("Step %1: %2 (%3) -> %4")
        .arg(QString::number(record.index), record.toolName,
             argumentParts.join(QStringLiteral(", ")), record.statusText);
}

} // namespace sentinel::core
