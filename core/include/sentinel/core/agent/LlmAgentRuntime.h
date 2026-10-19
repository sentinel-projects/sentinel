// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/agent/ContextEngine.h"
#include "sentinel/core/agent/IAgentRuntime.h"
#include "sentinel/core/agent/IAgentStepPlanner.h"
#include "sentinel/core/context/ContextBudgetManager.h"
#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/model/ModelRouting.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <memory>
#include <mutex>

namespace sentinel::core {
class IToolRegistry;

// IAgentRuntime + IAgentStepPlanner implementation that asks the configured
// chat provider (local or cloud) for the next agent action in JSON form and
// retries one malformed response, then reports planning failure.
class LlmAgentRuntime final : public IAgentRuntime, public IAgentStepPlanner {
public:
    LlmAgentRuntime(QList<ToolDescriptor> tools, IChatProvider* provider);

    QString name() const override;
    AgentStatus status() const override;
    QList<AgentCapabilityDescriptor> capabilities() const override;
    QList<ToolDescriptor> availableTools() const override;
    ToolInvocationPlan plan(const AgentRequest& request) const override;
    AgentResponse execute(const AgentRequest& request) override;

    AgentStepDecision nextStep(const QString& goal,
                               const QList<AgentStepRecord>& history) const override;

    bool lastDecisionUsedLlm() const;
    QJsonObject budgetDiagnostics() const {
        return budgetDiagnostics_;
    }
    int lastProviderRecoveryAttempts() const override {
        return lastProviderRecoveryAttempts_;
    }
    bool hasModelProvider() const {
        return provider_ != nullptr;
    }
    IChatProvider* modelProvider() const {
        return provider_;
    }
    void setObservationIntent(const ObservationIntent& intent) override {
        activeIntent_ = intent;
        observationIntentClassified_ = true;
    }
    void setEvidence(const QList<EvidenceRecord>& evidence) override {
        activeEvidence_ = evidence;
    }
    void setPlannerFeedback(const QString& feedback) override {
        plannerFeedback_ = feedback;
    }
    void setStructuredFacts(const QList<StructuredFact>& facts) override {
        structuredFacts_ = facts;
    }
    void setPlanningContext(const AgentPlanningContext& context) override {
        planningContext_ = context;
    }
    // Called before an accepted run; the owned provider and binding stay fixed
    // for every planner iteration in that run.
    using ProviderFactory = std::function<std::shared_ptr<IChatProvider>(const ModelBinding&)>;
    void bindModel(ModelBinding binding, std::shared_ptr<IChatProvider> provider,
                   ProviderFactory providerFactory = {});
    ModelBinding modelBinding() const {
        return modelBinding_;
    }
    void setStreamObserver(std::function<void(const QString&)> onDelta,
                           std::shared_ptr<std::atomic_bool> cancellationToken = {}) const;
    void setToolRegistry(const IToolRegistry* registry) {
        registry_ = registry;
    }
    void setInputImages(const QList<ChatImage>& images) { inputImages_ = images; }
    void setAllowedToolIds(const QStringList& ids) {
        allowedToolIds_ = ids;
        allowedToolFilterSet_ = true;
    }
    std::unique_ptr<LlmAgentRuntime> forkForSubagent(const QStringList& allowedTools) const;

private:
    QString buildPlannerPrompt(const QString& goal, const QList<AgentStepRecord>& history,
                               const AgentPlanningContext* selected = nullptr) const;
    AgentStepDecision decisionFromLlmOutput(const QString& output) const;
    AgentStepDecision decisionFromObject(const QJsonObject& object) const;
    AgentStepDecision decisionFromNativeCall(const ChatProviderReply::ToolCall& call) const;

    QList<ToolDescriptor> tools_;
    const IToolRegistry* registry_ = nullptr;
    QStringList allowedToolIds_;
    bool allowedToolFilterSet_ = false;
    IChatProvider* provider_ = nullptr;
    std::shared_ptr<IChatProvider> boundProvider_;
    ProviderFactory providerFactory_;
    std::shared_ptr<std::mutex> providerSerialization_;
    bool serializeProviderRequests_ = false;
    ModelBinding modelBinding_;
    QList<ChatImage> inputImages_;
    mutable ObservationIntent activeIntent_;
    bool observationIntentClassified_ = false;
    mutable QList<EvidenceRecord> activeEvidence_;
    mutable QList<StructuredFact> structuredFacts_;
    mutable AgentPlanningContext planningContext_;
    mutable QJsonObject budgetDiagnostics_;
    mutable QString plannerFeedback_;
    mutable QList<ChatProviderReply::ToolCall> nativeCalls_;
    mutable QList<ChatRequestOptions::ToolResult> nativeResults_;
    mutable int awaitingNativeResults_ = 0;
    mutable qsizetype nativeHistoryCursor_ = 0;
    mutable bool lastDecisionUsedLlm_ = false;
    mutable int lastProviderRecoveryAttempts_ = 0;
    mutable std::function<void(const QString&)> streamObserver_;
    mutable std::shared_ptr<std::atomic_bool> streamCancellationToken_;
};

} // namespace sentinel::core
