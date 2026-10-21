// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/agent/AgentExecutionScheduler.h"
#include "sentinel/core/agent/IAgentRuntime.h"
#include "sentinel/core/extension/ExtensionService.h"
#include "sentinel/core/plugin/PluginManager.h"
#include "sentinel/core/runtime/InMemoryToolRegistry.h"
#include "sentinel/core/runtime/ToolHookService.h"
#include "sentinel/core/security/ExternalDirectoryGate.h"
#include "sentinel/core/security/PermissionPolicyService.h"
#include "sentinel/core/security/PermissionService.h"
#include "sentinel/core/skill/SkillService.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace sentinel::core {

class IAgentStepPlanner;
class IToolExecutor;
class IApprovalPolicy;
class ISandboxPolicy;
class IMemoryStore;
class IChatHistoryStore;
class IAgentRunStore;
class AgentLoop;
class McpToolProvider;
class IMcpService;

// Owns the lifecycle of foreground agent sessions. Dependencies are borrowed
// from the composition root and must outlive this runtime.
class AgentRuntime final : public IAgentRuntime {
public:
    AgentRuntime(std::unique_ptr<IAgentRuntime> metadata, IAgentStepPlanner* planner,
                 IToolExecutor& executor, const IApprovalPolicy& approval,
                 const ISandboxPolicy& sandbox, const IMemoryStore* memoryStore = nullptr,
                 const IChatHistoryStore* chatHistoryStore = nullptr,
                 IAgentRunStore* runStore = nullptr,
                 std::shared_ptr<IPermissionGrantStore> permissionGrantStore = {},
                 std::shared_ptr<StorageAdmission> admission = {});
    AgentRuntime(std::unique_ptr<IAgentRuntime> metadata, IAgentStepPlanner& planner,
                 IToolExecutor& executor, const IApprovalPolicy& approval,
                 const ISandboxPolicy& sandbox)
        : AgentRuntime(std::move(metadata), &planner, executor, approval, sandbox) {}
    ~AgentRuntime() override;

    QString name() const override;
    AgentStatus status() const override;
    QList<AgentCapabilityDescriptor> capabilities() const override;
    QList<ToolDescriptor> availableTools() const override;
    ToolInvocationPlan plan(const AgentRequest& request) const override;
    AgentResponse execute(const AgentRequest& request) override;

    QString createSession() override;
    AgentLoopState submit(const QString& sessionId, const QString& goal) override;
    AgentLoopState resume(const QString& sessionId, bool approved) override;
    bool approve(const QString& sessionId, bool alwaysAllow) override;
    QList<PersistentPermissionGrant> persistentPermissionGrants() const;
    bool revokePersistentPermission(const QString& id);
    bool clearPersistentPermissions();
    QString permissionStoreError() const;
    bool cancel(const QString& sessionId) override;
    AgentLoopState sessionState(const QString& sessionId) const override;
    AgentRuntimeError error(const QString& sessionId) const override;
    // A pending approval is a paused run, not an unbounded wait. Zero disables
    // the timeout; the default is five minutes.
    void setApprovalTimeoutMs(int timeoutMs);
    int approvalTimeoutMs() const;

    void configureSession(const QString& sessionId, AgentSessionOptions options) override;
    bool start(const QString& sessionId, const QString& goal) override;
    bool continueSession(const QString& sessionId, bool approved) override;
    enum class ShutdownState { Running, Stopping, Stopped };
    ShutdownState shutdownState() const;
    bool tryShutdown(int timeoutMs) override;
    void shutdown() override;
    bool supportsSessions() const override;
    QString subscribe(AgentEventCallback callback) override;
    void unsubscribe(const QString& id) override;
    QList<AgentEvent> eventHistory(const QString& sessionId) const override;
    IToolRegistry& toolRegistry() {
        return toolRegistry_;
    }
    const IToolRegistry& toolRegistry() const {
        return toolRegistry_;
    }
    QList<ToolCompleteness> toolCompleteness() const;
    ToolHookService& toolHooks() {
        return toolHooks_;
    }
    plugin::PluginManager& pluginManager() {
        return pluginManager_;
    }
    SkillService& skillService() {
        return skillService_;
    }
    ExtensionService& extensionService() {
        return extensionService_;
    }
    const PermissionService& permissionService() const {
        return permissionService_;
    }
    // Bounded read-only terminal inspection uses the same descriptor, resource,
    // approval, permission and sandbox gates as Agent execution.
    ToolExecutionResult inspectWorkspace(const QString& sessionId, const QString& root,
                                         const QString& toolId, const QString& path);
    void setToolPermissionState(QString state) override;
    void setMcpService(std::shared_ptr<IMcpService> service);

private:
    bool drainShutdown(QDeadlineTimer deadline);
    AgentLoopState advance(const QString& sessionId, bool isResume, bool approved,
                           const QString& goal, bool prepared = false);
    void advanceAsync(const QString& sessionId, bool isResume, bool approved, const QString& goal,
                      QObject* context);
    void configureLoop(AgentLoop& loop, const QString& sessionId,
                       const AgentSessionOptions& options, const QString& goal);
    void commitResult(const QString& sessionId, AgentLoopState& result);
    void scheduleApprovalTimeout(const QString& sessionId, quint64 generation);
    void expireApproval(const QString& sessionId, quint64 generation);
    void recordDeniedAuthorization(const QString& sessionId);
    bool launch(const QString& sessionId, bool isResume, bool approved, const QString& goal);
    void prepareExecution(const QStringList& toolIds);
    QStringList toolIds() const;
    void publish(const QString& sessionId, AgentEventType type, AgentEventPayload payload = {},
                 int stepIndex = 0, bool toolCall = false);
    void beginTurn(const QString& sessionId, bool child = false);
    void finishTurn(const QString& sessionId, const AgentLoopState& state);
    struct TurnContext {
        QString id;
        QHash<int, QString> stepIds;
        QHash<int, QString> toolCallIds;
        bool terminal = false;
    };

    std::unique_ptr<IAgentRuntime> metadata_;
    InMemoryToolRegistry toolRegistry_;
    ToolHookService toolHooks_;
    plugin::PluginManager pluginManager_;
    SkillService skillService_;
    ExtensionService extensionService_;
    PermissionPolicyService toolPermissionPolicy_;
    PermissionService permissionService_;
    ExternalDirectoryGate externalDirectoryGate_;
    QString toolPermissionState_ = QStringLiteral("ask-every-time");
    std::unique_ptr<McpToolProvider> mcpToolProvider_;
    IAgentStepPlanner* planner_;
    IToolExecutor& executor_;
    const IApprovalPolicy& approval_;
    const ISandboxPolicy& sandbox_;
    const IMemoryStore* memoryStore_;
    const IChatHistoryStore* chatHistoryStore_;
    IAgentRunStore* runStore_ = nullptr;
    std::atomic_bool persistenceWarningEmitted_{false};
    std::atomic_bool persistenceFailed_{false};
    std::shared_ptr<StorageAdmission> admission_;
    std::uint64_t generation_ = 0;
    bool coordinatedStores_ = true;
    QHash<QString, std::shared_ptr<StorageAdmission::Writer>> operationLeases_;
    std::atomic_int subagentsThisRun_{0};
    QHash<QString, int> delegatedEvidenceRevision_;
    QString activeGoal_;
    std::shared_ptr<AgentExecutionScheduler::Budget> executionBudget_;
    mutable std::mutex mutex_;
    std::timed_mutex workerMutex_;
    std::mutex executionMutex_;
    QHash<QString, AgentLoopState> sessions_;
    QHash<QString, AgentSessionOptions> options_;
    QHash<QString, AgentRuntimeError> errors_;
    int approvalTimeoutMs_ = 300000;
    quint64 approvalTimerSequence_ = 0;
    QHash<QString, quint64> approvalTimerGenerations_;
    // Timer context: destroying it cancels every outstanding approval callback.
    QObject approvalTimerContext_;
    QString activeSessionId_;
    std::atomic<bool> cancelRequested_{false};
    std::shared_ptr<std::atomic_bool> modelCancellationToken_ =
        std::make_shared<std::atomic_bool>(false);
    std::condition_variable cancellationPublished_;
    bool cancellationEventPending_ = false;
    QThread* worker_ = nullptr;
    QObject* workerContext_ = nullptr;
    AgentLoop* activeLoop_ = nullptr;
    bool shuttingDown_ = false;
    bool shutdownComplete_ = false;
    mutable std::mutex eventMutex_;
    struct Subscription {
        AgentEventCallback callback;
        std::mutex mutex;
        std::condition_variable idle;
        bool active = true;
        bool inFlight = false;
        std::thread::id callbackThread;
    };
    QHash<QString, std::shared_ptr<Subscription>> subscribers_;
    QHash<QString, QList<AgentEvent>> history_;
    QHash<QString, TurnContext> turns_;
    QList<AgentEvent> pendingEvents_;
    bool dispatching_ = false;
};

} // namespace sentinel::core
