// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/core/agent/IAgentRunStore.h"
#include <QJsonArray>
#include <QJsonObject>
namespace sentinel::core::agent_wire {
inline QJsonObject encode(const StoredAgentRun& value) {
    QJsonObject o;
    o.insert("runId", value.runId.left(2048));
    o.insert("sessionId", value.sessionId.left(2048));
    o.insert("parentRunId", value.parentRunId.left(2048));
    o.insert("parentToolCallId", value.parentToolCallId.left(2048));
    o.insert("runType", value.runType.left(2048));
    o.insert("state", value.state.left(2048));
    o.insert("completionStatus", agentCompletionStatusName(value.state, value.terminalReason));
    o.insert("providerId", value.providerId.left(2048));
    o.insert("modelId", value.modelId.left(2048));
    o.insert("goalSummary", value.goalSummary.left(2048));
    o.insert("finalAnswer", value.finalAnswer.left(2048));
    o.insert("failure", value.failure.left(2048));
    o.insert("startedAt", value.startedAt.toString(Qt::ISODateWithMs));
    o.insert("finishedAt", value.finishedAt.toString(Qt::ISODateWithMs));
    o.insert("groundingMode", value.groundingMode.left(2048));
    o.insert("contextTokens", value.contextTokens);
    o.insert("contextItems", value.contextItems);
    o.insert("contextOmitted", value.contextOmitted);
    o.insert("contextCompacted", value.contextCompacted);
    o.insert("stepCount", value.stepCount);
    o.insert("capabilitySnapshot", value.capabilitySnapshot.left(2048));
    o.insert("role", value.role.left(2048));
    o.insert("providerErrorCategory", value.providerErrorCategory.left(2048));
    o.insert("providerHttpStatus", value.providerHttpStatus);
    o.insert("providerAttempts", value.providerAttempts);
    o.insert("providerRetryOccurred", value.providerRetryOccurred);
    o.insert("providerRequestLifecycle", value.providerRequestLifecycle.left(2048));
    o.insert("terminalReason", value.terminalReason.left(2048));
    o.insert("delegationPurpose", value.delegationPurpose.left(2048));
    o.insert("providerRecoveryAttempts", value.providerRecoveryAttempts);
    o.insert("workspaceId", value.workspaceId.left(2048));
    o.insert("workspaceName", value.workspaceName.left(2048));
    o.insert("presetId", value.presetId.left(2048));
    o.insert("profileVersion", value.profileVersion.left(2048));
    return o;
}
inline StoredAgentRun decodeStoredAgentRun(const QJsonObject& o) {
    StoredAgentRun value;
    value.runId = o.value("runId").toString();
    value.sessionId = o.value("sessionId").toString();
    value.parentRunId = o.value("parentRunId").toString();
    value.parentToolCallId = o.value("parentToolCallId").toString();
    value.runType = o.value("runType").toString();
    value.state = o.value("state").toString();
    value.providerId = o.value("providerId").toString();
    value.modelId = o.value("modelId").toString();
    value.goalSummary = o.value("goalSummary").toString();
    value.finalAnswer = o.value("finalAnswer").toString();
    value.failure = o.value("failure").toString();
    value.startedAt = QDateTime::fromString(o.value("startedAt").toString(), Qt::ISODateWithMs);
    value.finishedAt = QDateTime::fromString(o.value("finishedAt").toString(), Qt::ISODateWithMs);
    value.groundingMode = o.value("groundingMode").toString();
    value.contextTokens = o.value("contextTokens").toInt();
    value.contextItems = o.value("contextItems").toInt();
    value.contextOmitted = o.value("contextOmitted").toInt();
    value.contextCompacted = o.value("contextCompacted").toBool();
    value.stepCount = o.value("stepCount").toInt();
    value.capabilitySnapshot = o.value("capabilitySnapshot").toString();
    value.role = o.value("role").toString();
    value.providerErrorCategory = o.value("providerErrorCategory").toString();
    value.providerHttpStatus = o.value("providerHttpStatus").toInt();
    value.providerAttempts = o.value("providerAttempts").toInt();
    value.providerRetryOccurred = o.value("providerRetryOccurred").toBool();
    value.providerRequestLifecycle = o.value("providerRequestLifecycle").toString();
    value.terminalReason = o.value("terminalReason").toString();
    value.delegationPurpose = o.value("delegationPurpose").toString();
    value.providerRecoveryAttempts = o.value("providerRecoveryAttempts").toInt();
    value.workspaceId = o.value("workspaceId").toString();
    value.workspaceName = o.value("workspaceName").toString();
    value.presetId = o.value("presetId").toString();
    value.profileVersion = o.value("profileVersion").toString();
    return value;
}
inline QJsonObject encode(const StoredAgentStep& value) {
    QJsonObject o;
    o.insert("stepId", value.stepId.left(2048));
    o.insert("runId", value.runId.left(2048));
    o.insert("sequence", value.sequence);
    o.insert("status", value.status.left(2048));
    o.insert("toolCallId", value.toolCallId.left(2048));
    o.insert("startedAt", value.startedAt.toString(Qt::ISODateWithMs));
    o.insert("finishedAt", value.finishedAt.toString(Qt::ISODateWithMs));
    o.insert("observationKind", value.observationKind);
    o.insert("filesystemFailure", value.filesystemFailure);
    o.insert("stepType", value.stepType.left(2048));
    return o;
}
inline StoredAgentStep decodeStoredAgentStep(const QJsonObject& o) {
    StoredAgentStep value;
    value.stepId = o.value("stepId").toString();
    value.runId = o.value("runId").toString();
    value.sequence = o.value("sequence").toInt();
    value.status = o.value("status").toString();
    value.toolCallId = o.value("toolCallId").toString();
    value.startedAt = QDateTime::fromString(o.value("startedAt").toString(), Qt::ISODateWithMs);
    value.finishedAt = QDateTime::fromString(o.value("finishedAt").toString(), Qt::ISODateWithMs);
    value.observationKind = o.value("observationKind").toInt();
    value.filesystemFailure = o.value("filesystemFailure").toInt();
    value.stepType = o.value("stepType").toString();
    return value;
}
inline QJsonObject encode(const StoredAgentToolCall& value) {
    QJsonObject o;
    o.insert("toolCallId", value.toolCallId.left(2048));
    o.insert("stepId", value.stepId.left(2048));
    o.insert("toolId", value.toolId.left(2048));
    o.insert("source", value.source.left(2048));
    o.insert("status", value.status.left(2048));
    o.insert("resourceSummary", value.resourceSummary.left(2048));
    o.insert("observationSummary", value.observationSummary.left(2048));
    o.insert("startedAt", value.startedAt.toString(Qt::ISODateWithMs));
    o.insert("finishedAt", value.finishedAt.toString(Qt::ISODateWithMs));
    o.insert("failureCategory", value.failureCategory.left(2048));
    o.insert("mutationSummary", value.mutationSummary.left(2048));
    o.insert("batchId", value.batchId.left(2048));
    o.insert("sandboxSummary", value.sandboxSummary.left(2048));
    return o;
}
inline StoredAgentToolCall decodeStoredAgentToolCall(const QJsonObject& o) {
    StoredAgentToolCall value;
    value.toolCallId = o.value("toolCallId").toString();
    value.stepId = o.value("stepId").toString();
    value.toolId = o.value("toolId").toString();
    value.source = o.value("source").toString();
    value.status = o.value("status").toString();
    value.resourceSummary = o.value("resourceSummary").toString();
    value.observationSummary = o.value("observationSummary").toString();
    value.startedAt = QDateTime::fromString(o.value("startedAt").toString(), Qt::ISODateWithMs);
    value.finishedAt = QDateTime::fromString(o.value("finishedAt").toString(), Qt::ISODateWithMs);
    value.failureCategory = o.value("failureCategory").toString();
    value.mutationSummary = o.value("mutationSummary").toString();
    value.batchId = o.value("batchId").toString();
    value.sandboxSummary = o.value("sandboxSummary").toString();
    return value;
}
inline QJsonObject encode(const StoredAgentEvidence& value) {
    QJsonObject o;
    o.insert("toolCallId", value.toolCallId.left(2048));
    o.insert("domain", value.domain.left(2048));
    o.insert("resourceSummary", value.resourceSummary.left(2048));
    o.insert("outcome", value.outcome);
    o.insert("freshness", value.freshness);
    return o;
}
inline StoredAgentEvidence decodeStoredAgentEvidence(const QJsonObject& o) {
    StoredAgentEvidence value;
    value.toolCallId = o.value("toolCallId").toString();
    value.domain = o.value("domain").toString();
    value.resourceSummary = o.value("resourceSummary").toString();
    value.outcome = o.value("outcome").toInt();
    value.freshness = o.value("freshness").toInt();
    return value;
}
inline QJsonObject encode(const StoredAgentClaim& value) {
    QJsonObject o;
    o.insert("claimId", value.claimId.left(2048));
    o.insert("assertionValue", value.assertionValue);
    o.insert("verdict", value.verdict);
    o.insert("resourceSummary", value.resourceSummary.left(2048));
    o.insert("supportingToolCallIds", QJsonArray::fromStringList(value.supportingToolCallIds));
    o.insert("claimType", value.claimType);
    return o;
}
inline StoredAgentClaim decodeStoredAgentClaim(const QJsonObject& o) {
    StoredAgentClaim value;
    value.claimId = o.value("claimId").toString();
    value.assertionValue = o.value("assertionValue").toBool();
    value.verdict = o.value("verdict").toInt();
    value.resourceSummary = o.value("resourceSummary").toString();
    value.supportingToolCallIds = o.value("supportingToolCallIds").toVariant().toStringList();
    value.claimType = o.value("claimType").toInt();
    return value;
}
inline QJsonObject encode(const StoredAgentAuthorization& value) {
    QJsonObject o;
    o.insert("toolCallId", value.toolCallId.left(2048));
    o.insert("domain", value.domain.left(2048));
    o.insert("access", value.access.left(2048));
    o.insert("resourceSummary", value.resourceSummary.left(2048));
    o.insert("decision", value.decision.left(2048));
    o.insert("scope", value.scope.left(2048));
    return o;
}
inline StoredAgentAuthorization decodeStoredAgentAuthorization(const QJsonObject& o) {
    StoredAgentAuthorization value;
    value.toolCallId = o.value("toolCallId").toString();
    value.domain = o.value("domain").toString();
    value.access = o.value("access").toString();
    value.resourceSummary = o.value("resourceSummary").toString();
    value.decision = o.value("decision").toString();
    value.scope = o.value("scope").toString();
    return value;
}
template <class T> QJsonArray encodeList(const QList<T>& list) {
    QJsonArray a;
    for (const auto& item : list)
        a.append(encode(item));
    return a;
}
} // namespace sentinel::core::agent_wire
