// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/agent/AgentLoopState.h"
#include "sentinel/core/runtime/ProcessExecutor.h"

#include <QDateTime>
#include <QString>
#include <variant>

namespace sentinel::core {

enum class AgentEventType {
    SessionCreated,
    RunStarted,
    ContextSnapshot,
    ModelRequestStarted,
    ModelOutputDelta,
    ModelRequestCompleted,
    ToolRequested,
    ToolApprovalRequired,
    ToolApprovalResolved,
    ToolExecutionStarted,
    ToolOutput,
    ToolExecutionCompleted,
    ToolExecutionFailed,
    AgentStepCompleted,
    AgentCompleted,
    AgentFailed,
    AgentCancelled,
    RuntimeStateChanged,
};

struct AgentStateEvent {
    AgentLoopPhase phase = AgentLoopPhase::Idle;
    QString completionStatus;
};

struct AgentTextEvent {
    QString text;
};

struct AgentRunStartedEvent {
    QString goalSummary;
    QString runType = QStringLiteral("interactive");
    QString parentRunId;
    QString parentToolCallId;
    QString role;
    QString delegationPurpose;
    QString workspaceId;
    QString workspaceName;
    QString presetId;
    QString profileVersion;
};

struct AgentContextEvent {
    int estimatedTokens = 0;
    int includedItems = 0;
    int omittedItems = 0;
    bool compacted = false;
};

struct AgentToolEvent {
    QString toolId;
    ToolRiskLevel risk = ToolRiskLevel::Low;
    QString output;
    QList<AuthorizationRequest> authorizationRequests;
    QString displayName;
    QString source;
    QString batchId;
};

struct AgentStepEvent {
    AgentStepRecord step;
};

struct AgentToolOutputEvent {
    QString processId;
    ProcessStream stream = ProcessStream::Stdout;
    QByteArray chunk;
};

struct AgentRunEvent {
    AgentLoopPhase phase = AgentLoopPhase::Idle;
    AgentTerminalReason terminalReason = AgentTerminalReason::None;
    int providerRecoveryAttempts = 0;
    QString finalAnswer;
    QString abortReason;
    std::optional<ProviderFailureMetadata> providerFailure;
    int completedSteps = 0;
    AgentToolEvent pendingTool;
    QList<AuthorizationRequest> pendingAuthorizationRequests;
    QString pendingThought;
    ObservationIntent observationIntent;
    QList<EvidenceRecord> evidence;
    FinalAnswerGrounding grounding;
    QList<ClaimAssertion> finalClaims;
};

using AgentEventPayload =
    std::variant<std::monostate, AgentStateEvent, AgentTextEvent, AgentRunStartedEvent,
                 AgentContextEvent, AgentToolEvent,
                 AgentToolOutputEvent, AgentStepEvent, AgentRunEvent>;

struct AgentEvent {
    QString id;
    QString sessionId;
    QString turnId;
    QString stepId;
    QString toolCallId;
    QString providerId;
    QString modelId;
    QString capabilitySnapshot;
    int stepIndex = 0;
    AgentEventType type = AgentEventType::SessionCreated;
    QDateTime timestamp;
    AgentEventPayload payload;
};

} // namespace sentinel::core
