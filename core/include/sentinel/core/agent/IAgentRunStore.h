// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/core/app/StorageAdmission.h"

#include "sentinel/core/agent/AgentEvent.h"

#include <QList>
#include <QStringList>

namespace sentinel::core {

struct StoredAgentRun {
    QString runId;
    QString sessionId;
    QString parentRunId;
    QString parentToolCallId;
    QString runType;
    QString state;
    QString providerId;
    QString modelId;
    QString goalSummary;
    QString finalAnswer;
    QString failure;
    QDateTime startedAt;
    QDateTime finishedAt;
    QString groundingMode;
    int contextTokens = 0;
    int contextItems = 0;
    int contextOmitted = 0;
    bool contextCompacted = false;
    int stepCount = 0;
    QString capabilitySnapshot;
    QString role;
    QString providerErrorCategory;
    int providerHttpStatus = 0;
    int providerAttempts = 0;
    bool providerRetryOccurred = false;
    QString providerRequestLifecycle;
    QString terminalReason;
    QString delegationPurpose;
    int providerRecoveryAttempts = 0;
    QString workspaceId;
    QString workspaceName;
    QString presetId;
    QString profileVersion;
};

struct StoredAgentStep {
    QString stepId;
    QString runId;
    int sequence = 0;
    QString status;
    QString toolCallId;
    QDateTime startedAt;
    QDateTime finishedAt;
    int observationKind = 0;
    int filesystemFailure = 0;
    QString stepType;
};

struct StoredAgentToolCall {
    QString toolCallId;
    QString stepId;
    QString toolId;
    QString source;
    QString status;
    QString resourceSummary;
    QString observationSummary;
    QDateTime startedAt;
    QDateTime finishedAt;
    QString failureCategory;
    QString mutationSummary;
    QString batchId;
    QString sandboxSummary;
};

struct StoredAgentEvidence {
    QString toolCallId;
    QString domain;
    QString resourceSummary;
    int outcome = 0;
    int freshness = 0;
};

struct StoredAgentClaim {
    QString claimId;
    bool assertionValue = false;
    int verdict = 0;
    QString resourceSummary;
    QStringList supportingToolCallIds;
    int claimType = 0;
};

struct StoredAgentAuthorization {
    QString toolCallId;
    QString domain;
    QString access;
    QString resourceSummary;
    QString decision;
    QString scope;
};

class IAgentRunStore {
public:
    virtual ~IAgentRunStore() = default;
    virtual bool coordinatedBy(const std::shared_ptr<StorageAdmission>&) const {
        return false;
    }
    virtual bool clearHistory() { return false; }
    virtual int pruneCompletedBefore(const QDateTime& cutoffUtc, int limit = 100) {
        Q_UNUSED(cutoffUtc)
        Q_UNUSED(limit)
        return -1;
    }
    virtual bool record(const AgentEvent& event) = 0;
    virtual QList<StoredAgentRun> recentRuns(int limit) const = 0;
    virtual QList<StoredAgentRun> runsBefore(const QDateTime& startedAt,
                                             const QString& runId, int limit) const = 0;
    virtual StoredAgentRun runById(const QString& runId) const = 0;
    virtual QList<StoredAgentRun> childRuns(const QString& parentRunId, int limit) const = 0;
    virtual QList<StoredAgentStep> stepsForRun(const QString& runId, int limit) const = 0;
    virtual QList<StoredAgentToolCall> toolCallsForRun(const QString& runId, int limit) const = 0;
    virtual QList<StoredAgentEvidence> evidenceForRun(const QString& runId, int limit) const = 0;
    virtual QList<StoredAgentClaim> claimsForRun(const QString& runId, int limit) const = 0;
    virtual QList<StoredAgentAuthorization> authorizationsForRun(const QString& runId,
                                                                 int limit) const = 0;
    virtual QString lastError() const = 0;
};

} // namespace sentinel::core
