// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/agent/IAgentStepPlanner.h"
#include "sentinel/core/runtime/ToolInvocationPlan.h"

#include <QList>
#include <QString>

namespace sentinel::core {

enum class AgentLoopPhase {
    Idle,
    Running,
    Cancelling,
    AwaitingApproval,
    Completed,
    Cancelled,
    Failed,
    Stuck,
};

enum class AgentTerminalReason {
    None,
    Completed,
    Cancelled,
    ApprovalDenied,
    ProviderFailure,
    ToolFailure,
    CapabilityUnavailable,
    SecurityDenied,
    IterationLimit,
    UnableToComplete,
};

inline QString agentTerminalReasonName(AgentTerminalReason reason) {
    switch (reason) {
    case AgentTerminalReason::None:
        return QStringLiteral("None");
    case AgentTerminalReason::Completed:
        return QStringLiteral("Completed");
    case AgentTerminalReason::Cancelled:
        return QStringLiteral("Cancelled");
    case AgentTerminalReason::ApprovalDenied:
        return QStringLiteral("ApprovalDenied");
    case AgentTerminalReason::ProviderFailure:
        return QStringLiteral("ProviderFailure");
    case AgentTerminalReason::ToolFailure:
        return QStringLiteral("ToolFailure");
    case AgentTerminalReason::CapabilityUnavailable:
        return QStringLiteral("CapabilityUnavailable");
    case AgentTerminalReason::SecurityDenied:
        return QStringLiteral("SecurityDenied");
    case AgentTerminalReason::IterationLimit:
        return QStringLiteral("IterationLimit");
    case AgentTerminalReason::UnableToComplete:
        return QStringLiteral("UnableToComplete");
    }
    return QStringLiteral("None");
}

inline QString agentLoopPhaseName(AgentLoopPhase phase) {
    switch (phase) {
    case AgentLoopPhase::Idle:
        return QStringLiteral("Idle");
    case AgentLoopPhase::Running:
        return QStringLiteral("Running");
    case AgentLoopPhase::Cancelling:
        return QStringLiteral("Cancelling");
    case AgentLoopPhase::AwaitingApproval:
        return QStringLiteral("Awaiting Approval");
    case AgentLoopPhase::Completed:
        return QStringLiteral("Completed");
    case AgentLoopPhase::Cancelled:
        return QStringLiteral("Cancelled");
    case AgentLoopPhase::Failed:
        return QStringLiteral("Failed");
    case AgentLoopPhase::Stuck:
        return QStringLiteral("Stuck");
    }

    return QStringLiteral("Idle");
}

struct AgentLoopState {
    QString sessionId;
    QString goal;
    AgentLoopPhase phase = AgentLoopPhase::Idle;
    AgentTerminalReason terminalReason = AgentTerminalReason::None;
    QList<AgentStepRecord> steps;
    ObservationIntent observationIntent;
    QList<EvidenceRecord> evidence;
    FinalAnswerGrounding finalGrounding;
    QList<ClaimAssertion> finalClaims;
    int rejectedFinalAnswers = 0;
    int blockedSteps = 0;
    int consecutiveBlockedSteps = 0;
    int repeatedReadRepairs = 0;
    int repeatedActionRepairs = 0;
    bool repeatedActionRecoveryPending = false;
    int finalVerificationRecoveries = 0;
    int providerRecoveryAttempts = 0;
    QString finalAnswer;
    QString abortReason;
    std::optional<ProviderFailureMetadata> providerFailure;
    ToolInvocationPlan pendingApprovalPlan;
    QString pendingApprovalThought;
    QList<AuthorizationRequest> pendingAuthorizationRequests;
};

} // namespace sentinel::core
