// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/agent/IAgentStepPlanner.h"
#include "sentinel/core/runtime/ToolInvocationPlan.h"

#include <QList>
#include <QString>
#include <QStringList>

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
    // Observation classification could not produce a usable intent, so no
    // grounded step could be planned. Distinct from a provider outage so the
    // user is told to check the model rather than the network.
    ObservationIndeterminate,
    // Required instructions, execution evidence or continuation cannot fit in
    // the effective context window and no safe compaction remained.
    ContextBudgetExhausted,
    // A required post-mutation or final verification could not be observed.
    VerificationFailed,
    // Every remaining action is blocked, denied or out of scope.
    Blocked,
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
    case AgentTerminalReason::ObservationIndeterminate:
        return QStringLiteral("ObservationIndeterminate");
    case AgentTerminalReason::ContextBudgetExhausted:
        return QStringLiteral("ContextBudgetExhausted");
    case AgentTerminalReason::VerificationFailed:
        return QStringLiteral("VerificationFailed");
    case AgentTerminalReason::Blocked:
        return QStringLiteral("Blocked");
    }
    return QStringLiteral("None");
}

// Maps a typed planner stop onto the run's terminal reason. Unknown values stay
// `UnableToComplete` rather than `None`: a run that stopped has a reason.
inline AgentTerminalReason agentTerminalReasonFor(AgentStepFailure failure) {
    switch (failure) {
    case AgentStepFailure::ObservationIndeterminate:
        return AgentTerminalReason::ObservationIndeterminate;
    case AgentStepFailure::ContextBudgetExhausted:
        return AgentTerminalReason::ContextBudgetExhausted;
    case AgentStepFailure::VerificationFailed:
        return AgentTerminalReason::VerificationFailed;
    case AgentStepFailure::ProviderFailure:
        return AgentTerminalReason::ProviderFailure;
    case AgentStepFailure::Blocked:
        return AgentTerminalReason::Blocked;
    case AgentStepFailure::IterationLimit:
        return AgentTerminalReason::IterationLimit;
    case AgentStepFailure::None:
        break;
    }
    return AgentTerminalReason::UnableToComplete;
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
    // Typed reason the most recent final answer was rejected. `None` means the
    // rejection came from the general evidence gate and keeps the generic
    // terminal reason; verification-specific rejections stay distinguishable.
    AgentStepFailure lastFinalRejection = AgentStepFailure::None;
    int blockedSteps = 0;
    int consecutiveBlockedSteps = 0;
    int repeatedReadRepairs = 0;
    int repeatedActionRepairs = 0;
    int repeatedFailureRepairs = 0;
    bool repeatedActionRecoveryPending = false;
    int finalVerificationRecoveries = 0;
    // Normalized resources of mutations already verified by an automatic
    // read-back in this run. A mutation is never read back twice and never
    // re-executed by the recovery path.
    QStringList mutationReadBacks;
    int providerRecoveryAttempts = 0;
    QString finalAnswer;
    QString abortReason;
    std::optional<ProviderFailureMetadata> providerFailure;
    ToolInvocationPlan pendingApprovalPlan;
    QString pendingApprovalThought;
    QList<AuthorizationRequest> pendingAuthorizationRequests;
};

} // namespace sentinel::core
