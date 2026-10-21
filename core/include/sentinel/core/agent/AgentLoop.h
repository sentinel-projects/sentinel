// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/agent/AgentLoopState.h"
#include "sentinel/core/agent/AgentExecutionScheduler.h"
#include "sentinel/core/agent/ContextEngine.h"
#include "sentinel/core/agent/IAgentStepPlanner.h"
#include "sentinel/core/agent/ObservationPolicy.h"
#include "sentinel/core/doomloop/DoomLoopDetector.h"
#include "sentinel/core/runtime/IToolExecutor.h"
#include "sentinel/core/runtime/ToolExecutionGateway.h"
#include "sentinel/core/runtime/ToolInvocationPlan.h"
#include "sentinel/core/runtime/ToolOutputTruncator.h"
#include "sentinel/core/security/ResourceAuthorizationResolver.h"

#include <atomic>
#include <functional>
#include <memory>

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace sentinel::core {

class IApprovalPolicy;
class ISandboxPolicy;
class IToolExecutor;
class IToolRegistry;
class IToolHookService;
class ExternalDirectoryGate;
class PermissionService;
class IMemoryStore;
class IChatHistoryStore;
class AgentExecutionScheduler;

class AgentLoop {
public:
    struct Config {
        int maxIterations = 12;
        bool autonomousMode = false;
        int observationPreviewLines = 60;
        qint64 observationMaxBytes = 8192;
        int maxParallelTools = 3;
        std::shared_ptr<AgentExecutionScheduler::Budget> executionBudget;
    };

    void setPersistenceCheck(std::function<bool()> check) {
        persistenceCheck_ = std::move(check);
    }

    using StepCallback = std::function<void(const AgentStepRecord&)>;
    using StatusCallback = std::function<void(const QString&)>;
    using CancelQuery = std::function<bool()>;
    enum class ToolTransition { Requested, ApprovalRequired, ExecutionStarted, ExecutionFinished };
    using ToolCallback =
        std::function<void(ToolTransition, int, const ToolInvocationPlan&, const AgentStepRecord*)>;
    using PlanningCallback = std::function<void(int, bool)>;
    using ContextCallback = std::function<void(int, const AgentPlanningContext&)>;
    using OutputCallback =
        std::function<void(int, const QString&, ProcessStream, const QByteArray&)>;
    using CompletionCallback = std::function<void(const AgentLoopState&)>;
    using ToolCallIdProvider = std::function<QString(int)>;

    AgentLoop(IAgentStepPlanner& planner, const IToolExecutor& executor,
              const IApprovalPolicy& approvalPolicy, const ISandboxPolicy& sandboxPolicy,
              QStringList knownToolIds)
        : AgentLoop(planner, executor, approvalPolicy, sandboxPolicy, std::move(knownToolIds),
                    Config{}) {}

    AgentLoop(IAgentStepPlanner& planner, const IToolExecutor& executor,
              const IApprovalPolicy& approvalPolicy, const ISandboxPolicy& sandboxPolicy,
              QStringList knownToolIds, Config config);

    void setExternalDirectoryGate(ExternalDirectoryGate* gate) {
        externalDirectoryGate_ = gate;
        gateway_.setResourceGate(gate);
    }
    void setPermissionService(PermissionService* service) {
        permissionService_ = service;
        gateway_.setPermissionService(service);
    }
    void setStepCallback(StepCallback callback);
    void setStatusCallback(StatusCallback callback);
    void setCancelQuery(CancelQuery query);
    void setCancellationToken(std::shared_ptr<std::atomic_bool> token);
    void setToolCallback(ToolCallback callback);
    void setPlanningCallback(PlanningCallback callback);
    void setContextCallback(ContextCallback callback) { contextCallback_ = std::move(callback); }
    void setOutputCallback(OutputCallback callback);
    void setToolCallIdProvider(ToolCallIdProvider provider);
    void setToolRegistry(const IToolRegistry* registry) {
        toolRegistry_ = registry;
        gateway_.setRegistry(registry);
    }
    void setObservationIntentPolicy(std::shared_ptr<IObservationIntentPolicy> policy) {
        observationIntentPolicy_ = std::move(policy);
    }
    void setObservationContext(QString context) { observationContext_ = std::move(context); }
    void setResponseProfileInstructions(QString instructions) { responseProfileInstructions_ = instructions.left(2000); }
    void setWorkspaceContext(AgentContextInput::WorkspaceContext context) {
        workspaceContext_ = std::move(context);
    }
    void setResourceScope(QString root) { resourceScope_ = std::move(root); }
    void setSkills(QList<Skill> skills) {
        skills_ = std::move(skills);
    }
    void setContextSources(const IMemoryStore* memory, const IChatHistoryStore* history,
                           int contextWindowTokens, int maxOutputTokens = 0) {
        memoryStore_ = memory;
        chatHistoryStore_ = history;
        contextWindowTokens_ = contextWindowTokens;
        maxOutputTokens_ = maxOutputTokens;
    }
    void setToolHookService(IToolHookService* hooks) {
        gateway_.setHookService(hooks);
    }
    void setPermissionPolicy(const PermissionPolicyService* policy, QString defaultState) {
        defaultPermissionState_ = std::move(defaultState);
        permissionPolicy_ = policy;
        gateway_.setPermissionPolicy(policy, defaultPermissionState_);
    }

    AgentLoopState run(const QString& goal, const QString& sessionId = QString());
    AgentLoopState resume(AgentLoopState state, bool approved);
    void runAsync(const QString& goal, const QString& sessionId, QObject* context,
                  CompletionCallback completion);
    void resumeAsync(AgentLoopState state, bool approved, QObject* context,
                     CompletionCallback completion);
    void cancelAsync();

private:
    QString workingDirectory() const;
    QStringList externalPathsRequiringApproval(const ToolInvocationPlan& plan,
                                               const QString& sessionId) const;
    void grantExternalPaths(const ToolInvocationPlan& plan, const QString& sessionId);
    bool hasAuthorizationGrants(const ToolInvocationPlan& plan, const QString& sessionId) const;
    bool hasAuthorizationDeny(const ToolInvocationPlan& plan, const QString& sessionId) const;
    void applyPermissionPolicy(const ToolInvocationPlan& plan, const QString& sessionId,
                               ApprovalDecision& approval) const;
    QList<AuthorizationRequest> resolveAuthorizationRequests(
        const ToolInvocationPlan& plan) const;
    ResourceAuthorizationResult prepareResources(ToolInvocationPlan& plan) const;
    ResourceAuthorizationResult authorizeResources(ToolInvocationPlan& plan,
                                                   const QString& sessionId) const;
    AgentLoopState advance(AgentLoopState state);
    void executeStep(AgentLoopState& state, const ToolInvocationPlan& plan, const QString& thought,
                     ApprovalDecision approval);
    void appendBlockedStep(AgentLoopState& state, const ToolInvocationPlan& plan,
                           const QString& thought, const QString& statusText,
                           const QString& observation,
                           StructuredObservationPtr structuredObservation = {});
    void initializeObservationIntent(AgentLoopState& state);
    void preparePlanningContext(const AgentLoopState& state);
    bool acceptFinalAnswer(AgentLoopState& state, const AgentStepDecision& decision);
    void recordEvidence(AgentLoopState& state, const ToolDescriptor& descriptor,
                        const ToolInvocationPlan& plan, ToolExecutionStatus status,
                        const QString& summary, int stepIndex, StructuredObservationPtr structuredObservation = {},
                        const QList<FileMutation>& mutations = {});
    ToolInvocationPlan planFromDecision(const AgentStepDecision& decision) const;
    bool cancellationRequested() const;
    void advanceAsync();
    void executeStepAsync(const ToolInvocationPlan& plan, const QString& thought,
                          ApprovalDecision approval);
    void executeBatchAsync(const ToolInvocationPlan& plan, const QString& thought,
                           ApprovalDecision approval);
    void completeAsync();
    void scheduleAsyncAdvance();

    ExternalDirectoryGate* externalDirectoryGate_ = nullptr;
    PermissionService* permissionService_ = nullptr;
    const PermissionPolicyService* permissionPolicy_ = nullptr;
    QString defaultPermissionState_;
    IAgentStepPlanner& planner_;
    const IToolRegistry* toolRegistry_ = nullptr;
    std::shared_ptr<IObservationIntentPolicy> observationIntentPolicy_;
    QString observationContext_;
    QString responseProfileInstructions_;
    AgentContextInput::WorkspaceContext workspaceContext_;
    QString resourceScope_;
    ContextEngine contextEngine_;
    QList<Skill> skills_;
    const IMemoryStore* memoryStore_ = nullptr;
    const IChatHistoryStore* chatHistoryStore_ = nullptr;
    int contextWindowTokens_ = 0;
    int maxOutputTokens_ = 0;
    const IToolExecutor& executor_;
    const IApprovalPolicy& approvalPolicy_;
    const ISandboxPolicy& sandboxPolicy_;
    ToolExecutionGateway gateway_;
    DoomLoopDetector doomDetector_{DoomLoopDetector::Config{}};
    ToolOutputTruncator truncator_;
    QStringList knownToolIds_;
    Config config_;
    std::atomic<bool> cancelled_{false};
    StepCallback stepCallback_;
    StatusCallback statusCallback_;
    CancelQuery cancelQuery_;
    std::shared_ptr<std::atomic_bool> cancellationToken_;
    ToolCallback toolCallback_;
    std::function<bool()> persistenceCheck_;
    PlanningCallback planningCallback_;
    ContextCallback contextCallback_;
    OutputCallback outputCallback_;
    ToolCallIdProvider toolCallIdProvider_;
    CompletionCallback completionCallback_;
    AgentLoopState asyncState_;
    QObject* asyncContext_ = nullptr;
    IToolExecutor::Cancel cancelTool_;
    std::shared_ptr<AgentExecutionScheduler> scheduler_;
    bool asyncFinished_ = false;
    bool waitingForTool_ = false;
};

} // namespace sentinel::core
