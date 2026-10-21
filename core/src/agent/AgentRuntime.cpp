// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/agent/AgentRuntime.h"
#include "sentinel/core/agent/AgentLoop.h"
#include "sentinel/core/agent/IAgentRunStore.h"
#include "sentinel/core/agent/LlmAgentRuntime.h"
#include "sentinel/core/agent/ObservationPolicy.h"
#include "sentinel/core/chat/IChatHistoryStore.h"
#include "sentinel/core/interfaces/IMemoryStore.h"
#include "sentinel/core/mcp/McpToolProvider.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/RealToolExecutor.h"
#include "sentinel/core/security/AuthorizationResolver.h"
#include "sentinel/core/security/IApprovalPolicy.h"
#include "sentinel/core/security/ISandboxPolicy.h"
#include "sentinel/core/security/PathGuard.h"
#include "sentinel/core/security/ResourceAuthorizationResolver.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

namespace sentinel::core {

namespace {
AgentTerminalReason terminalReasonFor(const AgentLoopState& state) {
    if (state.terminalReason != AgentTerminalReason::None)
        return state.terminalReason;
    if (state.phase == AgentLoopPhase::Completed)
        return AgentTerminalReason::Completed;
    if (state.phase == AgentLoopPhase::Cancelled)
        return AgentTerminalReason::Cancelled;
    if (state.phase == AgentLoopPhase::Failed || state.phase == AgentLoopPhase::Stuck)
        return state.providerFailure ? AgentTerminalReason::ProviderFailure
                                     : AgentTerminalReason::UnableToComplete;
    return AgentTerminalReason::None;
}

AgentRuntimeErrorCode errorCodeFor(const AgentLoopState& state) {
    switch (state.terminalReason) {
    case AgentTerminalReason::ApprovalDenied:
        return AgentRuntimeErrorCode::ApprovalDenied;
    case AgentTerminalReason::ProviderFailure:
        return AgentRuntimeErrorCode::ProviderFailure;
    case AgentTerminalReason::ToolFailure:
        return AgentRuntimeErrorCode::ToolFailure;
    case AgentTerminalReason::CapabilityUnavailable:
        return AgentRuntimeErrorCode::CapabilityUnavailable;
    case AgentTerminalReason::SecurityDenied:
        return AgentRuntimeErrorCode::SecurityDenied;
    case AgentTerminalReason::IterationLimit:
        return AgentRuntimeErrorCode::IterationLimit;
    case AgentTerminalReason::Cancelled:
        return AgentRuntimeErrorCode::Cancelled;
    case AgentTerminalReason::UnableToComplete:
        return AgentRuntimeErrorCode::ExecutionFailed;
    case AgentTerminalReason::Completed:
    case AgentTerminalReason::None:
        break;
    }
    if (state.phase == AgentLoopPhase::Cancelled)
        return AgentRuntimeErrorCode::Cancelled;
    if (state.phase == AgentLoopPhase::Stuck)
        return AgentRuntimeErrorCode::Stuck;
    if (state.phase == AgentLoopPhase::Failed)
        return AgentRuntimeErrorCode::ExecutionFailed;
    return AgentRuntimeErrorCode::None;
}

class ExecutorCompatibilityHandler final : public IToolHandler {
public:
    explicit ExecutorCompatibilityHandler(IToolExecutor& executor) : executor_(executor) {}
    IToolExecutor::Cancel execute(const ToolExecutionRequest& request, const QString& sessionId,
                                  const QString& toolCallId, IToolExecutor::Output output,
                                  IToolExecutor::Completion completion) override {
        return executor_.executeAsync(request, sessionId, toolCallId, std::move(output),
                                      std::move(completion));
    }

private:
    IToolExecutor& executor_;
};
} // namespace

AgentRuntime::AgentRuntime(std::unique_ptr<IAgentRuntime> metadata, IAgentStepPlanner* planner,
                           IToolExecutor& executor, const IApprovalPolicy& approval,
                           const ISandboxPolicy& sandbox, const IMemoryStore* memoryStore,
                           const IChatHistoryStore* chatHistoryStore, IAgentRunStore* runStore,
                           std::shared_ptr<IPermissionGrantStore> permissionGrantStore,
                           std::shared_ptr<StorageAdmission> admission)
    : metadata_(std::move(metadata)),
      extensionService_(nullptr, &pluginManager_, &skillService_, &toolRegistry_),
      permissionService_(permissionGrantStore), planner_(planner), executor_(executor),
      approval_(approval), sandbox_(sandbox), memoryStore_(memoryStore),
      chatHistoryStore_(chatHistoryStore), runStore_(runStore) {
    admission_ = std::move(admission);
    generation_ = admission_ ? admission_->snapshot().generation : 0;
    coordinatedStores_ =
        !admission_ || (runStore_ && runStore_->coordinatedBy(admission_) && permissionGrantStore &&
                        permissionGrantStore->coordinatedBy(admission_) && chatHistoryStore_ &&
                        chatHistoryStore_->coordinatedBy(admission_));
    externalDirectoryGate_.setAuthorizationCheck(
        [this](const QString& path, bool write, const QString& sessionId) {
            const QList<AccessMode> modes =
                write ? QList<AccessMode>{AccessMode::Write, AccessMode::Delete}
                      : QList<AccessMode>{AccessMode::Read};
            for (const auto mode : modes) {
                const AuthorizationRequest request{SecurityDomain::FileSystem, mode, path,
                                                   ToolRiskLevel::Medium,      {},   {}};
                if (permissionService_.evaluateAuthorization(request, sessionId) ==
                    PermissionEffect::Allow)
                    return true;
            }
            return false;
        });
    if (auto* nativeExecutor = dynamic_cast<RealToolExecutor*>(&executor_)) {
        nativeExecutor->setExternalDirectoryGate(&externalDirectoryGate_);
        nativeExecutor->setMemoryContextResolver([this](const QString& sessionId) {
            std::lock_guard lock(mutex_);
            MemoryAccessContext context;
            if (sessionId.isEmpty() || !sessions_.contains(sessionId))
                return context;
            context.sessionId = sessionId;
            const auto options = options_.constFind(sessionId);
            if (options != options_.cend())
                context.workspaceId = options->workspaceContext.id;
            if (!context.workspaceId.isEmpty())
                context.authorizedWorkspaceIds.insert(context.workspaceId);
            return context;
        });
        BuiltInToolProvider::registerTools(toolRegistry_, *nativeExecutor);
    } else {
        auto compatibility = std::make_shared<ExecutorCompatibilityHandler>(executor_);
        for (auto descriptor : metadata_->availableTools())
            toolRegistry_.registerTool({std::move(descriptor), compatibility});
    }
    if (auto* llm = dynamic_cast<LlmAgentRuntime*>(planner_))
        llm->setToolRegistry(&toolRegistry_);
    if (auto* nativeExecutor = dynamic_cast<RealToolExecutor*>(&executor_))
        setMcpService(nativeExecutor->mcpService());
    pluginManager_.setToolRegistry(&toolRegistry_);
    pluginManager_.setExternalDirectoryGate(&externalDirectoryGate_);
    if (dynamic_cast<RealToolExecutor*>(&executor_)) {
        pluginManager_.discoverPlugins({});
        pluginManager_.startAll();
        skillService_.discoverSkills(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            QStringLiteral("/skills"));
        extensionService_.refreshSkillRequirements();
    }
}

void AgentRuntime::setMcpService(std::shared_ptr<IMcpService> service) {
    mcpToolProvider_.reset();
    extensionService_.setMcpService(dynamic_cast<McpService*>(service.get()));
    if (service)
        mcpToolProvider_ = std::make_unique<McpToolProvider>(std::move(service), toolRegistry_);
    extensionService_.refreshSkillRequirements();
}

AgentRuntime::~AgentRuntime() {
    shutdown();
    pluginManager_.unloadAll();
    if (auto* executor = dynamic_cast<RealToolExecutor*>(&executor_)) {
        executor->setMemoryContextResolver({});
        executor->setMemoryAccessContext({});
    }
}

void AgentRuntime::setToolPermissionState(QString state) {
    std::lock_guard lock(mutex_);
    toolPermissionState_ = std::move(state);
}

ToolExecutionResult AgentRuntime::inspectWorkspace(const QString& sessionId, const QString& root,
                                                   const QString& toolId, const QString& path) {
    if (toolId != QLatin1String("glob") && toolId != QLatin1String("read-file"))
        return {ToolExecutionStatus::Blocked, QStringLiteral("Terminal inspection is read-only.")};
    const auto canonicalRoot = PathGuard::canonicalPath(root);
    if (root.isEmpty() || !QFileInfo(canonicalRoot).isDir())
        return {ToolExecutionStatus::Blocked,
                QStringLiteral("Select a workspace with an existing root.")};
    ExternalDirectoryGate gate;
    gate.setWorkingDirectory(canonicalRoot);
    gate.setSecuritySessionId(sessionId);
    const auto canonicalPath =
        gate.resolvePath(path.isEmpty() ? canonicalRoot : path, canonicalRoot);
    if (!PathGuard::contains(canonicalRoot, canonicalPath))
        return {ToolExecutionStatus::Blocked,
                QStringLiteral("Terminal inspection cannot leave the workspace.")};
    const auto descriptor = toolRegistry_.findToolById(toolId);
    if (!descriptor || descriptor->source != ToolSource::BuiltIn)
        return {ToolExecutionStatus::Blocked, QStringLiteral("Built-in file tool is unavailable.")};
    PlannedToolInvocation invocation;
    invocation.toolId = toolId;
    invocation.arguments = {{QStringLiteral("path"), canonicalPath}};
    if (toolId == QLatin1String("glob"))
        invocation.arguments.append({QStringLiteral("pattern"), QStringLiteral("*")});
    ToolInvocationPlan plan;
    plan.status = ToolInvocationPlanStatus::Planned;
    plan.invocations = {invocation};
    ToolExecutionGateway gateway(&toolRegistry_);
    gateway.setResourceGate(&gate);
    gateway.setHookService(&toolHooks_);
    gateway.setPermissionService(&permissionService_);
    gateway.setPermissionPolicy(&toolPermissionPolicy_, toolPermissionState_);
    const auto validated = gateway.validatePlan(plan);
    if (validated.status != ToolExecutionStatus::Succeeded)
        return validated;
    auto resources = ResourceAuthorizationResolver::resolve(*descriptor, plan.invocations.first(),
                                                            canonicalRoot, &gate);
    if (!resources.ok())
        return {ToolExecutionStatus::Blocked, resources.reason};
    for (const auto& request : resources.snapshot.requests)
        if (permissionService_.evaluateAuthorization(request, sessionId) ==
                PermissionEffect::Deny ||
            !toolPermissionPolicy_.allowsAuthorization(request, toolPermissionState_, false))
            return {ToolExecutionStatus::Blocked,
                    QStringLiteral("File inspection is denied by authorization policy.")};
    resources = ResourceAuthorizationResolver::authorize(resources.snapshot, &gate,
                                                         &permissionService_, sessionId);
    if (!resources.ok())
        return {ToolExecutionStatus::Blocked, resources.reason};
    plan.invocations.first().resourceSnapshot =
        std::make_shared<ResourceAuthorizationSnapshot>(resources.snapshot);
    const auto approval = approval_.evaluate(plan);
    if (approval.status != ApprovalStatus::NotRequired)
        return {ToolExecutionStatus::Blocked,
                QStringLiteral("File inspection requires approval through an Agent request.")};
    const auto sandbox = sandbox_.evaluate(plan, approval);
    struct Inspection {
        ToolExecutionResult result;
        std::atomic_bool completed{false};
    };
    auto inspection = std::make_shared<Inspection>();
    ToolExecutionRequest inspectionRequest{plan, approval, sandbox, {toolId}};
    inspectionRequest.synchronousReadOnly = true;
    auto cancel = gateway.executeAsync(
        inspectionRequest, executor_, sessionId, QUuid::createUuid().toString(QUuid::WithoutBraces),
        {}, [inspection](ToolExecutionResult value) {
            inspection->result = std::move(value);
            inspection->completed.store(true, std::memory_order_release);
        });
    if (!inspection->completed.load(std::memory_order_acquire)) {
        if (cancel)
            cancel();
        return {ToolExecutionStatus::Blocked,
                QStringLiteral("File inspection requires the asynchronous Agent path.")};
    }
    return inspection->result;
}

QString AgentRuntime::name() const {
    return metadata_->name();
}
AgentStatus AgentRuntime::status() const {
    return metadata_->status();
}
QList<AgentCapabilityDescriptor> AgentRuntime::capabilities() const {
    return metadata_->capabilities();
}
QList<ToolDescriptor> AgentRuntime::availableTools() const {
    return toolRegistry_.enabledTools();
}
QList<ToolCompleteness> AgentRuntime::toolCompleteness() const {
    return ToolExecutionGateway(&toolRegistry_).toolCompleteness();
}
ToolInvocationPlan AgentRuntime::plan(const AgentRequest& request) const {
    return metadata_->plan(request);
}
AgentResponse AgentRuntime::execute(const AgentRequest& request) {
    Q_UNUSED(request)
    return {false, QStringLiteral("Agent Mode requires a runtime session."), status()};
}

bool AgentRuntime::supportsSessions() const {
    return planner_ != nullptr;
}

QString AgentRuntime::subscribe(AgentEventCallback callback) {
    if (!callback)
        return {};
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::lock_guard lock(eventMutex_);
    auto subscription = std::make_shared<Subscription>();
    subscription->callback = std::move(callback);
    subscribers_.insert(id, std::move(subscription));
    return id;
}

void AgentRuntime::unsubscribe(const QString& id) {
    std::shared_ptr<Subscription> subscription;
    {
        std::lock_guard lock(eventMutex_);
        subscription = subscribers_.take(id);
    }
    if (!subscription)
        return;
    std::unique_lock lock(subscription->mutex);
    subscription->active = false;
    if (subscription->callbackThread != std::this_thread::get_id()) {
        subscription->idle.wait(lock, [&] { return !subscription->inFlight; });
    }
}

QList<AgentEvent> AgentRuntime::eventHistory(const QString& sessionId) const {
    std::lock_guard lock(eventMutex_);
    return history_.value(sessionId);
}

void AgentRuntime::publish(const QString& sessionId, AgentEventType type, AgentEventPayload payload,
                           int stepIndex, bool toolCall) {
    if (cancelRequested_ &&
        (type == AgentEventType::ModelRequestStarted || type == AgentEventType::ModelOutputDelta ||
         type == AgentEventType::ModelRequestCompleted || type == AgentEventType::ToolRequested ||
         type == AgentEventType::ContextSnapshot || type == AgentEventType::ToolApprovalRequired ||
         type == AgentEventType::ToolApprovalResolved ||
         type == AgentEventType::ToolExecutionStarted || type == AgentEventType::ToolOutput ||
         type == AgentEventType::ToolExecutionCompleted ||
         type == AgentEventType::ToolExecutionFailed || type == AgentEventType::AgentStepCompleted))
        return;
    AgentEvent event;
    event.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    event.sessionId = sessionId;
    event.type = type;
    event.stepIndex = stepIndex;
    if (const auto* llm = dynamic_cast<const LlmAgentRuntime*>(planner_)) {
        const auto binding = llm->modelBinding();
        event.providerId = binding.providerId;
        event.modelId = binding.modelId;
        event.capabilitySnapshot = capabilitySnapshotSummary(binding.capabilities);
    }
    event.timestamp = QDateTime::currentDateTimeUtc();
    event.payload = std::move(payload);
    {
        std::lock_guard lock(eventMutex_);
        auto& turn = turns_[sessionId];
        if (turn.terminal && type != AgentEventType::AgentCompleted &&
            type != AgentEventType::AgentFailed && type != AgentEventType::AgentCancelled &&
            type != AgentEventType::RuntimeStateChanged)
            return;
        event.turnId = turn.id;
        if (stepIndex > 0) {
            auto& stepId = turn.stepIds[stepIndex];
            if (stepId.isEmpty())
                stepId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            event.stepId = stepId;
            if (toolCall) {
                auto& callId = turn.toolCallIds[stepIndex];
                if (callId.isEmpty())
                    callId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                event.toolCallId = callId;
            }
        }
        auto& events = history_[sessionId];
        events.append(event);
        if (events.size() > 256)
            events.remove(0, events.size() - 256);
        pendingEvents_.append(event);
        if (dispatching_)
            return;
        dispatching_ = true;
    }
    while (true) {
        AgentEvent next;
        QList<std::shared_ptr<Subscription>> listeners;
        {
            std::lock_guard lock(eventMutex_);
            if (pendingEvents_.isEmpty()) {
                dispatching_ = false;
                break;
            }
            next = pendingEvents_.takeFirst();
            listeners = subscribers_.values();
        }
        const auto* projectedRun = std::get_if<AgentRunEvent>(&next.payload);
        const bool terminalProjection = next.type == AgentEventType::RuntimeStateChanged &&
                                        projectedRun &&
                                        projectedRun->phase != AgentLoopPhase::AwaitingApproval;
        // finishTurn already committed this complete snapshot before terminal delivery.
        // Its trailing UI projection must not become a new writer after that commit.
        if (runStore_ && !terminalProjection) {
            if (!runStore_->record(next)) {
                persistenceFailed_ = true;
                if (!persistenceWarningEmitted_.exchange(true))
                    qWarning("Agent execution history persistence is unavailable.");
            } else {
                persistenceWarningEmitted_ = false;
            }
        }
        if (persistenceFailed_ && next.type == AgentEventType::AgentCompleted) {
            next.type = AgentEventType::AgentFailed;
            next.payload = AgentTextEvent{QStringLiteral(
                "Agent terminal audit persistence failed; completion is unverified.")};
        }
        if (persistenceFailed_) {
            if (auto* run = std::get_if<AgentRunEvent>(&next.payload);
                run && run->phase == AgentLoopPhase::Completed) {
                run->phase = AgentLoopPhase::Failed;
                run->terminalReason = AgentTerminalReason::UnableToComplete;
                run->finalAnswer.clear();
                run->abortReason = QStringLiteral(
                    "Agent audit persistence failed; completed side effects may exist.");
            }
            std::lock_guard lock(eventMutex_);
            auto& events = history_[next.sessionId];
            for (auto& prior : events)
                if (prior.id == next.id) {
                    prior = next;
                    break;
                }
        }
        for (const auto& listener : listeners) {
            {
                std::lock_guard lock(listener->mutex);
                if (!listener->active)
                    continue;
                listener->inFlight = true;
                listener->callbackThread = std::this_thread::get_id();
            }
            try {
                listener->callback(next);
            } catch (...) {
                // Subscriber failures must not stop delivery to other consumers.
            }
            {
                std::lock_guard lock(listener->mutex);
                listener->inFlight = false;
                listener->callbackThread = {};
            }
            listener->idle.notify_all();
        }
    }
}

void AgentRuntime::beginTurn(const QString& sessionId, bool child) {
    if (!child)
        subagentsThisRun_ = 0;
    std::lock_guard lock(eventMutex_);
    turns_[sessionId] =
        TurnContext{QUuid::createUuid().toString(QUuid::WithoutBraces), {}, {}, false};
}

void AgentRuntime::finishTurn(const QString& sessionId, const AgentLoopState& state) {
    AgentRunEvent run;
    run.phase = state.phase;
    run.terminalReason = terminalReasonFor(state);
    run.providerRecoveryAttempts = state.providerRecoveryAttempts;
    run.finalAnswer = state.finalAnswer;
    run.abortReason = state.abortReason;
    run.providerFailure = state.providerFailure;
    run.completedSteps = static_cast<int>(state.steps.size());
    run.pendingThought = state.pendingApprovalThought;
    run.pendingAuthorizationRequests = state.pendingAuthorizationRequests;
    run.observationIntent = state.observationIntent;
    run.evidence = state.evidence;
    run.grounding = state.finalGrounding;
    run.finalClaims = state.finalClaims;
    if (!state.pendingApprovalPlan.invocations.isEmpty()) {
        const auto& invocation = state.pendingApprovalPlan.invocations.first();
        run.pendingTool = {
            invocation.toolId, invocation.riskLevel, {}, state.pendingAuthorizationRequests};
        run.pendingTool.displayName = invocation.toolName;
    }
    if (state.phase == AgentLoopPhase::AwaitingApproval) {
        publish(sessionId, AgentEventType::RuntimeStateChanged, run);
        return;
    }
    {
        std::lock_guard lock(eventMutex_);
        auto& turn = turns_[sessionId];
        if (turn.terminal)
            return;
        turn.terminal = true;
    }
    // Persist the complete evidence projection before delivering terminal success, without
    // publishing a second Completed state to consumers that append the accepted answer.
    if (runStore_) {
        AgentEvent snapshot;
        snapshot.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        snapshot.sessionId = sessionId;
        snapshot.type = AgentEventType::RuntimeStateChanged;
        snapshot.timestamp = QDateTime::currentDateTimeUtc();
        snapshot.payload = run;
        {
            std::lock_guard lock(eventMutex_);
            snapshot.turnId = turns_.value(sessionId).id;
        }
        if (!runStore_->record(snapshot)) {
            persistenceFailed_ = true;
        }
    }
    if (persistenceFailed_) {
        run.phase = AgentLoopPhase::Failed;
        run.terminalReason = AgentTerminalReason::UnableToComplete;
        run.finalAnswer.clear();
        run.abortReason = QStringLiteral("Agent audit persistence failed; completed side effects "
                                         "may exist. No actions were replayed.");
    }
    AgentEventType type;
    if (run.phase == AgentLoopPhase::Completed)
        type = AgentEventType::AgentCompleted;
    else if (run.phase == AgentLoopPhase::Cancelled)
        type = AgentEventType::AgentCancelled;
    else
        type = AgentEventType::AgentFailed;
    publish(
        sessionId, type,
        AgentTextEvent{run.phase == AgentLoopPhase::Completed ? run.finalAnswer : run.abortReason});
    // Preserve the public terminal-then-state event order after the evidence preflight.
    publish(sessionId, AgentEventType::RuntimeStateChanged, run);
}

QString AgentRuntime::createSession() {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    AgentLoopState state;
    state.sessionId = id;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) {
            return {};
        }
        sessions_.insert(id, state);
    }
    publish(id, AgentEventType::SessionCreated);
    return id;
}

AgentLoopState AgentRuntime::submit(const QString& sessionId, const QString& goal) {
    // Compatibility API for direct synchronous callers. Desktop sessions use
    // start(), whose AgentLoop continuation runs on the Qt worker event loop.
    return advance(sessionId, false, false, goal);
}

AgentLoopState AgentRuntime::resume(const QString& sessionId, bool approved) {
    if (!approved)
        recordDeniedAuthorization(sessionId);
    return advance(sessionId, true, approved, {});
}

void AgentRuntime::recordDeniedAuthorization(const QString& sessionId) {
    std::lock_guard lock(mutex_);
    const auto state = sessions_.value(sessionId);
    if (state.phase != AgentLoopPhase::AwaitingApproval)
        return;
    for (const auto& invocation : state.pendingApprovalPlan.invocations) {
        const auto registration = toolRegistry_.findRegistration(invocation.toolId);
        const auto* descriptor = invocation.descriptorSnapshot
                                     ? invocation.descriptorSnapshot.get()
                                     : (registration ? &registration->descriptor : nullptr);
        if (!descriptor)
            continue;
        for (const auto& request : AuthorizationResolver::resolve(
                 *descriptor, invocation, &externalDirectoryGate_, QDir::currentPath()))
            permissionService_.setAuthorization(request, PermissionEffect::Deny, sessionId, false);
    }
}

bool AgentRuntime::approve(const QString& sessionId, bool alwaysAllow) {
    std::lock_guard lock(mutex_);
    const auto state = sessions_.value(sessionId);
    if (state.phase != AgentLoopPhase::AwaitingApproval || !activeSessionId_.isEmpty()) {
        return false;
    }
    if (alwaysAllow) {
        QList<AuthorizationRequest> requests;
        for (const auto& invocation : state.pendingApprovalPlan.invocations) {
            const auto registration = toolRegistry_.findRegistration(invocation.toolId);
            const auto* descriptor = invocation.descriptorSnapshot
                                         ? invocation.descriptorSnapshot.get()
                                         : (registration ? &registration->descriptor : nullptr);
            if (!descriptor)
                return false;
            requests.append(AuthorizationResolver::resolve(
                *descriptor, invocation, &externalDirectoryGate_, QDir::currentPath()));
        }
        if (!permissionService_.grantAuthorizations(requests, {}, true))
            return false;
    }
    return true;
}

QList<PersistentPermissionGrant> AgentRuntime::persistentPermissionGrants() const {
    std::lock_guard lock(mutex_);
    return permissionService_.persistentGrants();
}

bool AgentRuntime::revokePersistentPermission(const QString& id) {
    std::lock_guard lock(mutex_);
    return permissionService_.removePersistentGrant(id);
}

bool AgentRuntime::clearPersistentPermissions() {
    std::lock_guard lock(mutex_);
    return permissionService_.clearPersistentGrants();
}

QString AgentRuntime::permissionStoreError() const {
    std::lock_guard lock(mutex_);
    return permissionService_.lastError();
}

bool AgentRuntime::cancel(const QString& sessionId) {
    bool waitingForApproval = false;
    {
        std::lock_guard lock(mutex_);
        if (sessionId.isEmpty() || !sessions_.contains(sessionId)) {
            return false;
        }
        if (activeSessionId_ != sessionId) {
            if (!activeSessionId_.isEmpty() ||
                sessions_.value(sessionId).phase != AgentLoopPhase::AwaitingApproval)
                return false;
            waitingForApproval = true;
        } else if (cancelRequested_)
            return false;
        approvalTimerGenerations_.remove(sessionId);
        ++approvalTimerSequence_;
        cancelRequested_ = true;
        modelCancellationToken_->store(true);
        cancellationEventPending_ = true;
        sessions_[sessionId].phase = AgentLoopPhase::Cancelling;
        if (workerContext_)
            QMetaObject::invokeMethod(
                workerContext_,
                [this] {
                    AgentLoop* loop = nullptr;
                    {
                        std::lock_guard loopLock(mutex_);
                        loop = activeLoop_;
                    }
                    if (loop)
                        loop->cancelAsync();
                },
                Qt::QueuedConnection);
    }
    publish(sessionId, AgentEventType::RuntimeStateChanged,
            AgentStateEvent{AgentLoopPhase::Cancelling});
    {
        std::lock_guard lock(mutex_);
        cancellationEventPending_ = false;
    }
    cancellationPublished_.notify_all();
    if (waitingForApproval) {
        AgentLoopState finalState;
        {
            std::lock_guard lock(mutex_);
            finalState = sessions_.value(sessionId);
            finalState.phase = AgentLoopPhase::Cancelled;
            finalState.abortReason = QStringLiteral("Agent run cancelled by user.");
            sessions_[sessionId] = finalState;
            errors_[sessionId] = {AgentRuntimeErrorCode::Cancelled, finalState.abortReason, false};
        }
        permissionService_.clearSessionGrants(sessionId);
        finishTurn(sessionId, finalState);
        std::lock_guard lock(mutex_);
        operationLeases_.remove(sessionId);
    }
    return true;
}

AgentLoopState AgentRuntime::sessionState(const QString& sessionId) const {
    std::lock_guard lock(mutex_);
    return sessions_.value(sessionId);
}

AgentRuntimeError AgentRuntime::error(const QString& sessionId) const {
    std::lock_guard lock(mutex_);
    return errors_.value(sessionId);
}

void AgentRuntime::setApprovalTimeoutMs(int timeoutMs) {
    std::lock_guard lock(mutex_);
    approvalTimeoutMs_ = qMax(0, timeoutMs);
}

int AgentRuntime::approvalTimeoutMs() const {
    std::lock_guard lock(mutex_);
    return approvalTimeoutMs_;
}

void AgentRuntime::configureSession(const QString& sessionId, AgentSessionOptions options) {
    std::lock_guard lock(mutex_);
    if (sessions_.contains(sessionId) && activeSessionId_.isEmpty()) {
        options_.insert(sessionId, std::move(options));
    }
}

bool AgentRuntime::start(const QString& sessionId, const QString& goal) {
    return launch(sessionId, false, false, goal);
}

bool AgentRuntime::continueSession(const QString& sessionId, bool approved) {
    if (!approved)
        recordDeniedAuthorization(sessionId);
    return launch(sessionId, true, approved, {});
}

bool AgentRuntime::launch(const QString& sessionId, bool isResume, bool approved,
                          const QString& goal) {
    std::lock_guard workerLock(workerMutex_);
    QString runType;
    AgentSessionOptions runOptions;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) {
            errors_[sessionId] = {AgentRuntimeErrorCode::InvalidState,
                                  QStringLiteral("Agent runtime is shutting down."), false};
            return false;
        }
        if (!planner_ || !activeSessionId_.isEmpty() || !sessions_.contains(sessionId)) {
            if (sessions_.contains(sessionId) && activeSessionId_.isEmpty()) {
                auto& state = sessions_[sessionId];
                state.phase = AgentLoopPhase::Failed;
                state.abortReason = QStringLiteral("Agent runtime could not start.");
                errors_[sessionId] = {AgentRuntimeErrorCode::ExecutionFailed, state.abortReason,
                                      false};
            }
            return false;
        }
        const auto phase = sessions_.value(sessionId).phase;
        if (isResume ? phase != AgentLoopPhase::AwaitingApproval : phase != AgentLoopPhase::Idle) {
            // Reject the operation, not the already accepted session lifecycle.
            errors_[sessionId] = {AgentRuntimeErrorCode::InvalidState,
                                  QStringLiteral("Agent session is in the wrong state."), false};
            return false;
        }
        if (admission_) {
            if (!coordinatedStores_) {
                errors_[sessionId] = {
                    AgentRuntimeErrorCode::ExecutionFailed,
                    QStringLiteral("Agent storage owner does not support shared admission."),
                    false};
                return false;
            }
            if (!isResume) {
                auto lease =
                    std::make_shared<StorageAdmission::Writer>(admission_->admit(generation_));
                if (!*lease) {
                    errors_[sessionId] = {AgentRuntimeErrorCode::ExecutionFailed,
                                          QStringLiteral("Agent storage admission refused."),
                                          false};
                    return false;
                }
                operationLeases_[sessionId] = std::move(lease);
            } else if (!operationLeases_.contains(sessionId)) {
                return false;
            }
        }
        activeSessionId_ = sessionId;
        runType = options_.value(sessionId).runType;
        runOptions = options_.value(sessionId);
        sessions_[sessionId].phase = AgentLoopPhase::Running;
        approvalTimerGenerations_.remove(sessionId);
        ++approvalTimerSequence_;
        cancelRequested_ = false;
        if (!isResume) {
            modelCancellationToken_ = std::make_shared<std::atomic_bool>(false);
            executionBudget_ = std::make_shared<AgentExecutionScheduler::Budget>(4);
            subagentsThisRun_.store(0);
            delegatedEvidenceRevision_.clear();
            activeGoal_ = goal;
        }
    }
    if (worker_) {
        worker_->wait();
        delete worker_;
        worker_ = nullptr;
    }
    if (!isResume) {
        persistenceFailed_ = false;
        beginTurn(sessionId);
    }
    if (isResume) {
        const auto state = sessionState(sessionId);
        const int index = static_cast<int>(state.steps.size()) + 1;
        publish(sessionId, AgentEventType::ToolApprovalResolved,
                AgentTextEvent{approved ? QStringLiteral("Approved") : QStringLiteral("Denied")},
                index, true);
    } else {
        publish(sessionId, AgentEventType::RunStarted,
                AgentRunStartedEvent{goal,
                                     runType,
                                     {},
                                     {},
                                     {},
                                     {},
                                     runOptions.workspaceContext.id,
                                     runOptions.workspaceName,
                                     runOptions.presetId,
                                     runOptions.profileVersion});
    }
    publish(sessionId, AgentEventType::RuntimeStateChanged,
            AgentStateEvent{AgentLoopPhase::Running});
    if (persistenceFailed_) {
        std::lock_guard lock(mutex_);
        auto& state = sessions_[sessionId];
        state.phase = AgentLoopPhase::Failed;
        state.abortReason =
            QStringLiteral("Agent audit persistence failed; execution was not started.");
        errors_[sessionId] = {AgentRuntimeErrorCode::ExecutionFailed, state.abortReason, false};
        activeSessionId_.clear();
        operationLeases_.remove(sessionId);
        return false;
    }
    worker_ = new QThread;
    auto* context = new QObject;
    context->moveToThread(worker_);
    {
        std::lock_guard lock(mutex_);
        workerContext_ = context;
    }
    QObject::connect(worker_, &QThread::started, context,
                     [this, sessionId, isResume, approved, goal, context] {
                         advanceAsync(sessionId, isResume, approved, goal, context);
                     });
    worker_->start();
    return true;
}

AgentRuntime::ShutdownState AgentRuntime::shutdownState() const {
    std::lock_guard lock(mutex_);
    return shutdownComplete_ ? ShutdownState::Stopped
           : shuttingDown_   ? ShutdownState::Stopping
                             : ShutdownState::Running;
}

void AgentRuntime::shutdown() {
    // Compatibility/destruction fence: dependencies must outlive all borrowed work.
    // Event-loop owners use tryShutdown(nonnegative budget) and retain us on false.
    (void)drainShutdown(QDeadlineTimer(QDeadlineTimer::Forever));
}

bool AgentRuntime::tryShutdown(int timeoutMs) {
    if (timeoutMs < 0) {
        return false;
    }
    return drainShutdown(QDeadlineTimer(timeoutMs));
}

bool AgentRuntime::drainShutdown(QDeadlineTimer deadline) {
    QStringList sessionsToCancel;
    {
        std::lock_guard lock(mutex_);
        if (shutdownComplete_) {
            return true;
        }
        shuttingDown_ = true;
        if (!activeSessionId_.isEmpty())
            sessionsToCancel.append(activeSessionId_);
        for (auto it = sessions_.cbegin(); it != sessions_.cend(); ++it) {
            if (it.value().phase == AgentLoopPhase::AwaitingApproval) {
                sessionsToCancel.append(it.key());
            }
        }
    }
    if (!sessionsToCancel.isEmpty())
        cancel(sessionsToCancel.first());
    std::unique_lock workerLock(workerMutex_, std::defer_lock);
    if (deadline.isForever()) {
        workerLock.lock();
    } else if (!workerLock.try_lock_for(
                   std::chrono::milliseconds(qMax<qint64>(0, deadline.remainingTime())))) {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        shuttingDown_ = true;
        cancelRequested_ = true;
        modelCancellationToken_->store(true);
    }
    if (worker_) {
        if (QThread::currentThread() == worker_) {
            return false;
        }
        if (!worker_->wait(deadline)) {
            return false;
        }
        delete worker_;
        worker_ = nullptr;
    }
    for (const auto& session : sessionsToCancel)
        cancel(session);
    QStringList subscriberIds;
    {
        std::lock_guard eventLock(eventMutex_);
        subscriberIds = subscribers_.keys();
    }
    for (const auto& id : subscriberIds)
        unsubscribe(id);
    {
        std::lock_guard lock(mutex_);
        operationLeases_.clear();
        shutdownComplete_ = true;
    }
    return true;
}

QStringList AgentRuntime::toolIds() const {
    QStringList ids;
    for (const auto& tool : toolRegistry_.enabledTools()) {
        ids.append(tool.id);
    }
    return ids;
}

void AgentRuntime::prepareExecution(const QStringList& availableToolIds) {
    std::lock_guard executionLock(executionMutex_);
    auto* executor = dynamic_cast<RealToolExecutor*>(&executor_);
    if (!executor) {
        return;
    }
    executor->setSearchStores(memoryStore_, chatHistoryStore_);

    executor->setSubagentRunnerWithContext([this, availableToolIds](
                                               const RealToolExecutor::SubagentAssignment&
                                                   assignment,
                                               const QString& invokingToolCallId)
                                               -> ToolExecutionResult {
        if (cancelRequested_.load() || modelCancellationToken_->load())
            return {ToolExecutionStatus::Cancelled, QStringLiteral("Subagent run was cancelled.")};
        QString parentSessionId;
        {
            std::lock_guard lock(mutex_);
            parentSessionId = activeSessionId_;
        }
        QString parentRunId;
        QString parentToolCallId = invokingToolCallId;
        {
            std::lock_guard lock(eventMutex_);
            const auto parent = turns_.value(parentSessionId);
            parentRunId = parent.id;
            if (parentToolCallId.isEmpty()) {
                int latestStep = 0;
                for (auto it = parent.toolCallIds.cbegin(); it != parent.toolCallIds.cend(); ++it)
                    if (it.key() > latestStep) {
                        latestStep = it.key();
                        parentToolCallId = it.value();
                    }
            }
        }
        AgentLoop::Config config;
        config.autonomousMode = true;
        config.maxIterations = 12;
        config.maxParallelTools = 1;
        {
            std::lock_guard lock(mutex_);
            config.executionBudget = executionBudget_;
        }
        static const QSet<QString> kSubagentTools{
            QStringLiteral("read-file"),     QStringLiteral("grep"),
            QStringLiteral("glob"),          QStringLiteral("list-code-definitions"),
            QStringLiteral("web-search"),    QStringLiteral("web-fetch"),
            QStringLiteral("memory-search"), QStringLiteral("history-search"),
            QStringLiteral("current-time"),  QStringLiteral("system-info"),
        };
        QStringList readOnlyToolIds;
        for (const auto& id : availableToolIds) {
            if (kSubagentTools.contains(id) &&
                (assignment.allowedToolIds.isEmpty() || assignment.allowedToolIds.contains(id))) {
                readOnlyToolIds.append(id);
            }
        }
        if (readOnlyToolIds.isEmpty())
            return {ToolExecutionStatus::Blocked,
                    QStringLiteral("Subagent has no permitted read-only tools.")};
        if (readOnlyToolIds.size() == 1)
            return {ToolExecutionStatus::Blocked,
                    QStringLiteral("A one-tool task should be handled by the parent agent.")};
        if (cancelRequested_.load() || modelCancellationToken_->load())
            return {ToolExecutionStatus::Cancelled, QStringLiteral("Subagent run was cancelled.")};
        auto* parentPlanner = dynamic_cast<LlmAgentRuntime*>(planner_);
        if (!assignment.modelId.isEmpty() &&
            (!parentPlanner || parentPlanner->modelBinding().modelId != assignment.modelId))
            return {ToolExecutionStatus::Blocked,
                    QStringLiteral("Subagent model differs from the active model binding.")};
        const QString root = PathGuard::canonicalPath(QDir::currentPath());
        const QString childScope =
            assignment.workspace.isEmpty()
                ? root
                : PathGuard::canonicalPath(QDir(root).absoluteFilePath(assignment.workspace));
        if (!PathGuard::contains(root, childScope))
            return {ToolExecutionStatus::Blocked,
                    QStringLiteral("Subagent workspace is outside the parent workspace.")};
        QStringList sortedTools = readOnlyToolIds;
        sortedTools.sort();
        const QString normalizedGoal = assignment.goal.simplified().toCaseFolded();
        const QString delegationKey = parentSessionId + QLatin1Char('|') + assignment.purpose +
                                      QLatin1Char('|') + childScope + QLatin1Char('|') +
                                      sortedTools.join(QLatin1Char(',')) + QLatin1Char('|') +
                                      normalizedGoal;
        int evidenceRevision = 0;
        {
            std::lock_guard lock(eventMutex_);
            for (const auto& event : history_.value(parentSessionId)) {
                const auto* step = std::get_if<AgentStepEvent>(&event.payload);
                if (step && step->step.succeeded &&
                    step->step.toolId != QLatin1String("spawn-agent"))
                    evidenceRevision = qMax(evidenceRevision, step->step.index);
            }
        }
        {
            std::lock_guard lock(mutex_);
            if (normalizedGoal == activeGoal_.simplified().toCaseFolded())
                return {ToolExecutionStatus::Blocked,
                        QStringLiteral("The parent goal must not be delegated unchanged.")};
            if (delegatedEvidenceRevision_.contains(delegationKey) &&
                delegatedEvidenceRevision_.value(delegationKey) >= evidenceRevision)
                return {
                    ToolExecutionStatus::Blocked,
                    QStringLiteral("Equivalent subagent work already ran without new evidence.")};
            if (subagentsThisRun_.fetch_add(1) >= 3)
                return {ToolExecutionStatus::Blocked,
                        QStringLiteral("Subagent limit reached for this run.")};
            delegatedEvidenceRevision_.insert(delegationKey, evidenceRevision);
        }
        auto childPlanner = parentPlanner ? parentPlanner->forkForSubagent(readOnlyToolIds)
                                          : std::unique_ptr<LlmAgentRuntime>{};
        auto* subagentPlanner = childPlanner.get();
        AgentLoop loop(childPlanner ? static_cast<IAgentStepPlanner&>(*childPlanner) : *planner_,
                       executor_, approval_, sandbox_, readOnlyToolIds, config);
        loop.setResourceScope(childScope);
        loop.setToolRegistry(&toolRegistry_);
        loop.setContextSources(
            memoryStore_, nullptr,
            subagentPlanner ? subagentPlanner->modelBinding().capabilities.contextWindow.value_or(0)
                            : 0,
            subagentPlanner
                ? subagentPlanner->modelBinding().capabilities.maxOutputTokens.value_or(0)
                : 0);
        if (auto* llm = dynamic_cast<LlmAgentRuntime*>(planner_))
            loop.setObservationIntentPolicy(std::make_shared<ObservationIntentPolicy>(
                llm->modelProvider(), modelCancellationToken_, llm->modelBinding()));
        loop.setExternalDirectoryGate(&externalDirectoryGate_);
        loop.setPermissionService(&permissionService_);
        loop.setToolHookService(&toolHooks_);
        QString permissionState;
        {
            std::lock_guard lock(mutex_);
            permissionState = toolPermissionState_;
        }
        loop.setPermissionPolicy(&toolPermissionPolicy_, permissionState);
        loop.setPersistenceCheck([this] { return !persistenceFailed_.load(); });
        loop.setCancelQuery(
            [this] { return cancelRequested_.load() || persistenceFailed_.load(); });
        loop.setCancellationToken(modelCancellationToken_);
        const auto subagentSessionId =
            QStringLiteral("subagent-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        beginTurn(subagentSessionId, true);
        publish(subagentSessionId, AgentEventType::RunStarted,
                AgentRunStartedEvent{assignment.goal, QStringLiteral("subagent"), parentRunId,
                                     parentToolCallId, assignment.role, assignment.purpose});
        loop.setPlanningCallback([this, subagentSessionId](int index, bool started) {
            publish(subagentSessionId,
                    started ? AgentEventType::ModelRequestStarted
                            : AgentEventType::ModelRequestCompleted,
                    {}, index);
        });
        loop.setContextCallback(
            [this, subagentSessionId](int index, const AgentPlanningContext& context) {
                publish(subagentSessionId, AgentEventType::ContextSnapshot,
                        AgentContextEvent{context.estimatedTokens,
                                          static_cast<int>(context.items.size()),
                                          context.omittedItems, context.compacted},
                        index);
            });
        loop.setStepCallback([this, subagentSessionId](const AgentStepRecord& step) {
            publish(subagentSessionId, AgentEventType::AgentStepCompleted, AgentStepEvent{step},
                    step.index, !step.toolId.isEmpty());
        });
        loop.setToolCallIdProvider([this, subagentSessionId](int index) {
            std::lock_guard lock(eventMutex_);
            return turns_.value(subagentSessionId).toolCallIds.value(index);
        });
        loop.setToolCallback([this, subagentSessionId](AgentLoop::ToolTransition transition,
                                                       int index, const ToolInvocationPlan& plan,
                                                       const AgentStepRecord* record) {
            if (plan.invocations.isEmpty())
                return;
            const auto& tool = plan.invocations.first();
            AgentToolEvent payload{tool.toolId, tool.riskLevel, {}, {}, tool.toolName};
            payload.batchId = plan.batchId;
            if (tool.descriptorSnapshot) {
                switch (tool.descriptorSnapshot->source) {
                case ToolSource::BuiltIn:
                    payload.source = QStringLiteral("built-in");
                    break;
                case ToolSource::MCP:
                    payload.source = QStringLiteral("mcp");
                    break;
                case ToolSource::Plugin:
                    payload.source = QStringLiteral("plugin");
                    break;
                case ToolSource::Internal:
                    payload.source = QStringLiteral("internal");
                    break;
                }
            }
            AgentEventType type = AgentEventType::ToolRequested;
            switch (transition) {
            case AgentLoop::ToolTransition::Requested:
                break;
            case AgentLoop::ToolTransition::ApprovalRequired:
                type = AgentEventType::ToolApprovalRequired;
                break;
            case AgentLoop::ToolTransition::ExecutionStarted:
                type = AgentEventType::ToolExecutionStarted;
                break;
            case AgentLoop::ToolTransition::ExecutionFinished:
                type = record && record->succeeded ? AgentEventType::ToolExecutionCompleted
                                                   : AgentEventType::ToolExecutionFailed;
                break;
            }
            publish(subagentSessionId, type, payload, index, true);
        });
        AgentLoopState state;
        try {
            state = loop.run(assignment.goal, subagentSessionId);
        } catch (...) {
            state.sessionId = subagentSessionId;
            state.goal = assignment.goal;
            state.phase = AgentLoopPhase::Failed;
            state.abortReason = QStringLiteral("Subagent execution failed.");
        }
        finishTurn(subagentSessionId, state);
        permissionService_.clearSessionGrants(subagentSessionId);
        if (state.phase == AgentLoopPhase::Completed) {
            return {ToolExecutionStatus::Succeeded, state.finalAnswer};
        }
        if (state.phase == AgentLoopPhase::Cancelled) {
            return {ToolExecutionStatus::Cancelled, QStringLiteral("Subagent run was cancelled.")};
        }
        return {ToolExecutionStatus::Failed,
                QStringLiteral("Subagent could not finish: %1")
                    .arg(state.abortReason.isEmpty() ? QStringLiteral("unknown reason")
                                                     : state.abortReason)};
    });
}

void AgentRuntime::configureLoop(AgentLoop& loop, const QString& sessionId,
                                 const AgentSessionOptions& options, const QString& goal) {
    loop.setResponseProfileInstructions(options.responseProfileInstructions);
    loop.setWorkspaceContext(options.workspaceContext);
    loop.setSkills(skillService_.skills());
    const auto workspaceRoot = options.workspaceContext.rootPath.trimmed().isEmpty()
                                   ? QString{}
                                   : PathGuard::canonicalPath(options.workspaceContext.rootPath);
    if (!workspaceRoot.isEmpty())
        loop.setResourceScope(workspaceRoot);
    if (auto* llm = dynamic_cast<LlmAgentRuntime*>(planner_)) {
        llm->setInputImages(options.inputImages);
        llm->setAllowedToolIds(
            (options.restrictAvailableTools || !options.availableToolIds.isEmpty())
                ? options.availableToolIds
                : toolIds());
        loop.setObservationIntentPolicy(std::make_shared<ObservationIntentPolicy>(
            llm->modelProvider(), modelCancellationToken_, llm->modelBinding()));
    }
    const auto* boundPlanner = dynamic_cast<LlmAgentRuntime*>(planner_);
    loop.setContextSources(
        memoryStore_, chatHistoryStore_,
        boundPlanner ? boundPlanner->modelBinding().capabilities.contextWindow.value_or(0) : 0,
        boundPlanner ? boundPlanner->modelBinding().capabilities.maxOutputTokens.value_or(0) : 0);
    if (chatHistoryStore_ && chatHistoryStore_->isAvailable()) {
        const auto messages = chatHistoryStore_->recentMessages(7);
        QStringList recent;
        for (int i = qMax(0, static_cast<int>(messages.size()) - 7); i < messages.size(); ++i) {
            const auto& message = messages.at(i);
            if (!message.content.trimmed().isEmpty())
                recent.append(message.content.simplified().left(240));
        }
        loop.setObservationContext(recent.join(QLatin1Char('\n')));
    }
    loop.setPersistenceCheck([this] { return !persistenceFailed_.load(); });
    loop.setCancelQuery([this] { return cancelRequested_.load() || persistenceFailed_.load(); });
    loop.setCancellationToken(modelCancellationToken_);
    loop.setStepCallback([this, sessionId, callback = options.onStep](const AgentStepRecord& step) {
        publish(sessionId, AgentEventType::AgentStepCompleted, AgentStepEvent{step}, step.index,
                !step.toolId.isEmpty());
        if (callback)
            callback(step);
    });
    loop.setStatusCallback(options.onStatus);
    loop.setPlanningCallback([this, sessionId, goal](int index, bool started) {
        const auto* llm = dynamic_cast<const LlmAgentRuntime*>(planner_);
        if (llm && llm->hasModelProvider() && !goal.trimmed().isEmpty()) {
            if (started) {
                llm->setStreamObserver(
                    [this, sessionId, index](const QString& delta) {
                        publish(sessionId, AgentEventType::ModelOutputDelta, AgentTextEvent{delta},
                                index);
                    },
                    modelCancellationToken_);
            } else {
                llm->setStreamObserver({});
            }
            publish(sessionId,
                    started ? AgentEventType::ModelRequestStarted
                            : AgentEventType::ModelRequestCompleted,
                    {}, index);
        }
    });
    loop.setContextCallback([this, sessionId](int index, const AgentPlanningContext& context) {
        publish(sessionId, AgentEventType::ContextSnapshot,
                AgentContextEvent{context.estimatedTokens, static_cast<int>(context.items.size()),
                                  context.omittedItems, context.compacted},
                index);
    });
    loop.setToolCallback([this, sessionId, workspaceRoot](AgentLoop::ToolTransition transition,
                                                          int index, const ToolInvocationPlan& plan,
                                                          const AgentStepRecord* record) {
        if (plan.invocations.isEmpty())
            return;
        const auto& invocation = plan.invocations.first();
        QList<AuthorizationRequest> authorizationRequests;
        if (transition == AgentLoop::ToolTransition::ApprovalRequired) {
            for (const auto& item : plan.invocations) {
                if (item.descriptorSnapshot)
                    authorizationRequests.append(AuthorizationResolver::resolve(
                        *item.descriptorSnapshot, item, &externalDirectoryGate_,
                        workspaceRoot.isEmpty() ? QDir::currentPath() : workspaceRoot));
            }
        }
        AgentToolEvent payload{invocation.toolId, invocation.riskLevel,
                               record ? record->observation : QString{}, authorizationRequests,
                               invocation.toolName};
        payload.batchId = plan.batchId;
        const auto registration = toolRegistry_.findRegistration(invocation.toolId);
        const auto* descriptor = invocation.descriptorSnapshot
                                     ? invocation.descriptorSnapshot.get()
                                     : (registration ? &registration->descriptor : nullptr);
        if (descriptor) {
            switch (descriptor->source) {
            case ToolSource::BuiltIn:
                payload.source = QStringLiteral("built-in");
                break;
            case ToolSource::MCP:
                payload.source = QStringLiteral("mcp");
                break;
            case ToolSource::Plugin:
                payload.source = QStringLiteral("plugin");
                break;
            case ToolSource::Internal:
                payload.source = QStringLiteral("internal");
                break;
            }
        }
        AgentEventType type;
        switch (transition) {
        case AgentLoop::ToolTransition::Requested:
            type = AgentEventType::ToolRequested;
            break;
        case AgentLoop::ToolTransition::ApprovalRequired:
            type = AgentEventType::ToolApprovalRequired;
            break;
        case AgentLoop::ToolTransition::ExecutionStarted:
            type = AgentEventType::ToolExecutionStarted;
            break;
        case AgentLoop::ToolTransition::ExecutionFinished:
            type = record && record->succeeded ? AgentEventType::ToolExecutionCompleted
                                               : AgentEventType::ToolExecutionFailed;
            break;
        default:
            return;
        }
        publish(sessionId, type, payload, index, true);
    });
    loop.setOutputCallback([this, sessionId](int index, const QString& processId,
                                             ProcessStream stream, const QByteArray& bytes) {
        publish(sessionId, AgentEventType::ToolOutput,
                AgentToolOutputEvent{processId, stream, bytes}, index, true);
    });
    loop.setToolCallIdProvider([this, sessionId](int index) {
        std::lock_guard lock(eventMutex_);
        return turns_.value(sessionId).toolCallIds.value(index);
    });
}

void AgentRuntime::scheduleApprovalTimeout(const QString& sessionId, quint64 generation) {
    int timeoutMs = 0;
    {
        std::lock_guard lock(mutex_);
        if (approvalTimerGenerations_.value(sessionId) != generation)
            return;
        timeoutMs = approvalTimeoutMs_;
    }
    if (timeoutMs <= 0 || !QCoreApplication::instance())
        return;
    QTimer::singleShot(timeoutMs, &approvalTimerContext_,
                       [this, sessionId, generation] { expireApproval(sessionId, generation); });
}

void AgentRuntime::expireApproval(const QString& sessionId, quint64 generation) {
    QString reason;
    {
        std::lock_guard lock(mutex_);
        if (approvalTimerGenerations_.value(sessionId) != generation)
            return;
        approvalTimerGenerations_.remove(sessionId);
        const auto state = sessions_.value(sessionId);
        if (state.phase != AgentLoopPhase::AwaitingApproval || !activeSessionId_.isEmpty() ||
            shuttingDown_)
            return;
        reason = QStringLiteral("Approval timed out after %1 seconds. No action was executed. "
                                "Retry the task or cancel it explicitly.")
                     .arg(qMax(1, (approvalTimeoutMs_ + 999) / 1000));
    }
    // A timeout is not consent. Record an explicit denial for the paused plan so
    // nothing can be granted implicitly by a later resume or reconnect.
    recordDeniedAuthorization(sessionId);
    AgentLoopState finalState;
    {
        std::lock_guard lock(mutex_);
        auto& state = sessions_[sessionId];
        if (state.phase != AgentLoopPhase::AwaitingApproval || !activeSessionId_.isEmpty())
            return;
        state.phase = AgentLoopPhase::Failed;
        state.terminalReason = AgentTerminalReason::ApprovalDenied;
        state.abortReason = reason;
        state.pendingApprovalPlan = {};
        state.pendingApprovalThought.clear();
        state.pendingAuthorizationRequests.clear();
        errors_[sessionId] = {AgentRuntimeErrorCode::ApprovalDenied, reason, true};
        operationLeases_.remove(sessionId);
        finalState = state;
    }
    permissionService_.clearSessionGrants(sessionId);
    finishTurn(sessionId, finalState);
}

void AgentRuntime::commitResult(const QString& sessionId, AgentLoopState& result) {
    bool awaitingApproval = false;
    quint64 approvalGeneration = 0;
    {
        std::unique_lock lock(mutex_);
        cancellationPublished_.wait(lock, [this] { return !cancellationEventPending_; });
        if (cancelRequested_ && result.phase != AgentLoopPhase::Completed) {
            result.phase = AgentLoopPhase::Cancelled;
            result.terminalReason = AgentTerminalReason::Cancelled;
            result.abortReason = QStringLiteral("Agent run cancelled by user.");
        }
        if (persistenceFailed_) {
            result.phase = AgentLoopPhase::Failed;
            result.terminalReason = AgentTerminalReason::UnableToComplete;
            result.finalAnswer.clear();
            result.abortReason =
                QStringLiteral("Agent audit persistence failed; completed tool side effects may "
                               "exist. No actions were replayed.");
        }
        result.terminalReason = terminalReasonFor(result);
        sessions_[sessionId] = result;
        const auto code = errorCodeFor(result);
        if (code != AgentRuntimeErrorCode::None)
            errors_[sessionId] = {code, result.abortReason, false};
        activeSessionId_.clear();
        activeLoop_ = nullptr;
        workerContext_ = nullptr;
        if (result.phase == AgentLoopPhase::AwaitingApproval) {
            awaitingApproval = true;
            approvalGeneration = ++approvalTimerSequence_;
            approvalTimerGenerations_[sessionId] = approvalGeneration;
        } else {
            approvalTimerGenerations_.remove(sessionId);
            ++approvalTimerSequence_;
        }
    }
    if (result.phase != AgentLoopPhase::AwaitingApproval)
        permissionService_.clearSessionGrants(sessionId);
    finishTurn(sessionId, result);
    if (result.phase != AgentLoopPhase::AwaitingApproval) {
        std::lock_guard lock(mutex_);
        operationLeases_.remove(sessionId);
    }
    if (awaitingApproval)
        scheduleApprovalTimeout(sessionId, approvalGeneration);
    if (persistenceFailed_) {
        std::lock_guard lock(mutex_);
        result.phase = AgentLoopPhase::Failed;
        result.terminalReason = AgentTerminalReason::UnableToComplete;
        result.finalAnswer.clear();
        result.abortReason = QStringLiteral("Agent audit persistence failed; completed tool side "
                                            "effects may exist. No actions were replayed.");
        sessions_[sessionId] = result;
        errors_[sessionId] = {AgentRuntimeErrorCode::ExecutionFailed, result.abortReason, false};
    }
}

void AgentRuntime::advanceAsync(const QString& sessionId, bool isResume, bool approved,
                                const QString& goal, QObject* context) {
    AgentLoopState seed;
    AgentSessionOptions options;
    QString permissionState;
    {
        std::lock_guard lock(mutex_);
        seed = sessions_.value(sessionId);
        options = options_.value(sessionId);
        permissionState = toolPermissionState_;
        errors_.remove(sessionId);
    }
    const auto availableToolIds =
        (options.restrictAvailableTools || !options.availableToolIds.isEmpty())
            ? options.availableToolIds
            : toolIds();
    prepareExecution(availableToolIds);
    auto executionLock = std::make_shared<std::unique_lock<std::mutex>>(executionMutex_);
    AgentLoop::Config config;
    config.autonomousMode = options.autonomousMode;
    config.maxParallelTools = qBound(1, options.maxParallelTools, 4);
    config.executionBudget = executionBudget_;
    auto loop = std::make_shared<AgentLoop>(*planner_, executor_, approval_, sandbox_,
                                            availableToolIds, config);
    loop->setToolRegistry(&toolRegistry_);
    loop->setExternalDirectoryGate(&externalDirectoryGate_);
    loop->setPermissionService(&permissionService_);
    loop->setToolHookService(&toolHooks_);
    loop->setPermissionPolicy(&toolPermissionPolicy_, permissionState);
    // The worker context owns the loop until its queued deletion on the same thread.
    QObject::connect(context, &QObject::destroyed, [loop] {});
    configureLoop(*loop, sessionId, options, seed.goal.isEmpty() ? goal : seed.goal);
    {
        std::lock_guard lock(mutex_);
        activeLoop_ = loop.get();
    }
    auto finish = [this, sessionId, context, executionLock,
                   callback = options.onFinished](const AgentLoopState& state) {
        AgentLoopState result = state;
        commitResult(sessionId, result);
        bool shuttingDown;
        {
            std::lock_guard lock(mutex_);
            shuttingDown = shuttingDown_;
        }
        if (!shuttingDown && callback)
            callback(result);
        context->deleteLater();
        worker_->quit();
    };
    if (isResume)
        loop->resumeAsync(seed, approved, context, std::move(finish));
    else
        loop->runAsync(goal, sessionId, context, std::move(finish));
}

AgentLoopState AgentRuntime::advance(const QString& sessionId, bool isResume, bool approved,
                                     const QString& goal, bool prepared) {
    if (!planner_) {
        AgentLoopState failure;
        failure.sessionId = sessionId;
        failure.phase = AgentLoopPhase::Failed;
        failure.abortReason = QStringLiteral("Agent step planner is unavailable.");
        return failure;
    }
    AgentLoopState seed;
    AgentSessionOptions options;
    QString permissionState;
    {
        std::lock_guard lock(mutex_);
        if (!sessions_.contains(sessionId) ||
            (prepared ? activeSessionId_ != sessionId : !activeSessionId_.isEmpty())) {
            AgentLoopState failure;
            failure.sessionId = sessionId;
            failure.phase = AgentLoopPhase::Failed;
            failure.abortReason = QStringLiteral("Agent session is unavailable or busy.");
            errors_[sessionId] = {!activeSessionId_.isEmpty()
                                      ? AgentRuntimeErrorCode::Busy
                                      : AgentRuntimeErrorCode::InvalidSession,
                                  failure.abortReason, !activeSessionId_.isEmpty()};
            return failure;
        }
        seed = sessions_.value(sessionId);
        options = options_.value(sessionId);
        permissionState = toolPermissionState_;
        const auto expectedPhase = prepared   ? AgentLoopPhase::Running
                                   : isResume ? AgentLoopPhase::AwaitingApproval
                                              : AgentLoopPhase::Idle;
        if (seed.phase != expectedPhase &&
            !(prepared && seed.phase == AgentLoopPhase::Cancelling)) {
            seed.phase = AgentLoopPhase::Failed;
            seed.abortReason = QStringLiteral("Agent session is in the wrong state.");
            errors_[sessionId] = {AgentRuntimeErrorCode::InvalidState, seed.abortReason, false};
            return seed;
        }
        if (admission_ && !prepared) {
            auto lease = std::make_shared<StorageAdmission::Writer>(admission_->admit(generation_));
            if (!coordinatedStores_ || !*lease) {
                seed.phase = AgentLoopPhase::Failed;
                seed.abortReason = QStringLiteral("Agent storage admission refused.");
                errors_[sessionId] = {AgentRuntimeErrorCode::ExecutionFailed, seed.abortReason,
                                      false};
                return seed;
            }
            if (!isResume)
                operationLeases_[sessionId] = std::move(lease);
            else if (!operationLeases_.contains(sessionId))
                return seed;
        }
        if (!prepared) {
            activeSessionId_ = sessionId;
            cancelRequested_ = false;
            if (!isResume) {
                modelCancellationToken_ = std::make_shared<std::atomic_bool>(false);
                executionBudget_ = std::make_shared<AgentExecutionScheduler::Budget>(4);
                subagentsThisRun_.store(0);
                delegatedEvidenceRevision_.clear();
                activeGoal_ = goal;
            }
        }
        if (!cancelRequested_)
            sessions_[sessionId].phase = AgentLoopPhase::Running;
        errors_.remove(sessionId);
    }

    if (!prepared) {
        if (!isResume) {
            persistenceFailed_ = false;
            beginTurn(sessionId);
        }
        if (isResume) {
            const int index = static_cast<int>(seed.steps.size()) + 1;
            publish(
                sessionId, AgentEventType::ToolApprovalResolved,
                AgentTextEvent{approved ? QStringLiteral("Approved") : QStringLiteral("Denied")},
                index, true);
        } else
            publish(sessionId, AgentEventType::RunStarted,
                    AgentRunStartedEvent{goal,
                                         options.runType,
                                         {},
                                         {},
                                         {},
                                         {},
                                         options.workspaceContext.id,
                                         options.workspaceName,
                                         options.presetId,
                                         options.profileVersion});
        publish(sessionId, AgentEventType::RuntimeStateChanged,
                AgentStateEvent{AgentLoopPhase::Running});
    }

    if (persistenceFailed_) {
        seed.phase = AgentLoopPhase::Failed;
        seed.abortReason =
            QStringLiteral("Agent audit persistence failed; execution was not started.");
        commitResult(sessionId, seed);
        return seed;
    }
    const auto availableToolIds =
        (options.restrictAvailableTools || !options.availableToolIds.isEmpty())
            ? options.availableToolIds
            : toolIds();
    AgentLoop::Config config;
    config.autonomousMode = options.autonomousMode;
    config.maxParallelTools = qBound(1, options.maxParallelTools, 4);
    config.executionBudget = executionBudget_;
    AgentLoopState result;
    try {
        prepareExecution(availableToolIds);
        std::lock_guard executionLock(executionMutex_);
        AgentLoop loop(*planner_, executor_, approval_, sandbox_, availableToolIds, config);
        loop.setToolRegistry(&toolRegistry_);
        loop.setExternalDirectoryGate(&externalDirectoryGate_);
        loop.setPermissionService(&permissionService_);
        loop.setToolHookService(&toolHooks_);
        loop.setPermissionPolicy(&toolPermissionPolicy_, permissionState);
        configureLoop(loop, sessionId, options, seed.goal.isEmpty() ? goal : seed.goal);
        result = isResume ? loop.resume(seed, approved) : loop.run(goal, sessionId);
    } catch (...) {
        result = std::move(seed);
        result.phase = AgentLoopPhase::Failed;
        result.abortReason = QStringLiteral("Agent execution failed.");
    }
    commitResult(sessionId, result);
    return result;
}

} // namespace sentinel::core
