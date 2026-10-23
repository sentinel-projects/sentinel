// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/app/ControlledTaskService.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/WorkspaceService.h"

#include <QMetaObject>
#include <QPointer>
#include <utility>

namespace sentinel::core {

ControlledTaskService::ControlledTaskService(AppSettings& settings, IAgentRuntime& runtime,
                                             ModelService& modelService,
                                             BindResolvedModel bindModel,
                                             InteractiveBusy interactiveBusy, QObject* parent)
    : QObject(parent), settings_(settings), runtime_(runtime), modelService_(modelService),
      bindModel_(std::move(bindModel)), interactiveBusy_(std::move(interactiveBusy)),
      tasks_(domain_.tasksFromJson(settings_.controlledAgentTasksJson())),
      permissions_(domain_.permissionsFromJson(settings_.controlledAgentPermissionsJson())) {
    bool recovered = false;
    for (auto& task : tasks_) {
        if (task.provider == QLatin1String("Local")) {
            task.provider.clear();
            recovered = true;
        }
        if (task.model == QLatin1String("Selected model")) {
            task.model.clear();
            recovered = true;
        }
        if (task.state == ControlledTaskState::Running ||
            task.state == ControlledTaskState::WaitingApproval) {
            task = domain_.applyRuntimeState(
                task, QStringLiteral("Failed"),
                QStringLiteral("Agent session ended when the application closed."),
                task.runtimeSessionId, task.runtimeRunId);
            recovered = true;
        }
    }
    if (recovered)
        saveTasks();

    QPointer<ControlledTaskService> self(this);
    subscriptionId_ = runtime_.subscribe([self](const AgentEvent& event) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, event] {
                if (self)
                    self->onAgentEvent(event);
            },
            Qt::QueuedConnection);
    });
}

ControlledTaskService::~ControlledTaskService() {
    runtime_.unsubscribe(subscriptionId_);
}

ControlledAgentTask ControlledTaskService::task(const QString& taskId) const {
    return domain_.taskById(tasks_, taskId);
}

void ControlledTaskService::saveTasks() {
    settings_.setControlledAgentTasksJson(domain_.tasksToJson(tasks_));
    emit tasksChanged();
}

void ControlledTaskService::savePermissions() {
    settings_.setControlledAgentPermissionsJson(domain_.permissionsToJson(permissions_));
    emit tasksChanged();
}

void ControlledTaskService::upsert(ControlledAgentTask task) {
    tasks_ = domain_.upsertTask(std::move(tasks_), task);
    saveTasks();
}

QString ControlledTaskService::createTask(const QString& goal, const QString& workspaceId,
                                          const QStringList& resources) {
    const WorkspaceService workspaces;
    const auto id = workspaces.normalizedWorkspaceId(workspaceId, settings_.workspaceCatalogJson());
    const auto selected = modelService_.selectedModel();
    const auto profile = workspaces.resolveProfile(settings_.workspaceProfilesJson(), id,
        {{QStringLiteral("providerId"), selected.providerId},
         {QStringLiteral("modelId"), selected.modelId}});
    const ModelSelection selection{
        profile.configured.value(QStringLiteral("providerId")).toString(),
        profile.configured.value(QStringLiteral("modelId")).toString()};
    auto created = domain_.createPlan(goal, id, selection.providerId, selection.modelId,
                                      resources, tasks_);
    const auto taskId = created.id;
    upsert(std::move(created));
    return taskId;
}

bool ControlledTaskService::editPlan(const QString& taskId, const QStringList& steps) {
    auto current = task(taskId);
    if (current.id.isEmpty() || sessionByTask_.contains(taskId))
        return false;
    upsert(domain_.setSteps(std::move(current), steps));
    return true;
}

bool ControlledTaskService::approveTask(const QString& taskId, const QString& choice) {
    auto current = task(taskId);
    if (current.id.isEmpty())
        return false;
    if (current.state == ControlledTaskState::WaitingApproval) {
        const auto sessionId = sessionByTask_.value(taskId);
        return !sessionId.isEmpty() && runtime_.approve(sessionId, false) &&
               runtime_.continueSession(sessionId, true);
    }
    if (sessionByTask_.contains(taskId))
        return false;
    upsert(domain_.approve(std::move(current), choice));
    return true;
}

bool ControlledTaskService::denyTask(const QString& taskId) {
    auto current = task(taskId);
    if (current.id.isEmpty())
        return false;
    if (current.state == ControlledTaskState::WaitingApproval) {
        const auto sessionId = sessionByTask_.value(taskId);
        return !sessionId.isEmpty() && runtime_.continueSession(sessionId, false);
    }
    if (sessionByTask_.contains(taskId))
        return false;
    upsert(domain_.deny(std::move(current)));
    return true;
}

bool ControlledTaskService::startTask(const QString& taskId) {
    auto current = task(taskId);
    if (current.id.isEmpty() || !runtime_.supportsSessions() || !sessionByTask_.isEmpty() ||
        interactiveBusy_())
        return false;

    auto prepared = domain_.start(current, tasks_);
    if (prepared.state != ControlledTaskState::Running) {
        upsert(std::move(prepared));
        return false;
    }
    const WorkspaceService workspaces;
    if (workspaces.normalizedWorkspaceId(prepared.workspaceId, settings_.workspaceCatalogJson()) !=
        prepared.workspaceId) {
        prepared = domain_.applyRuntimeState(prepared, QStringLiteral("Failed"),
            QStringLiteral("Captured workspace is unavailable."), {}, {});
        upsert(std::move(prepared));
        return false;
    }
    const auto workspace = workspaces.selectedWorkspace(prepared.workspaceId,
                                                        settings_.workspaceCatalogJson());
    const auto selected = modelService_.selectedModel();
    const auto profile = workspaces.resolveProfile(settings_.workspaceProfilesJson(),
        prepared.workspaceId, {{QStringLiteral("providerId"), selected.providerId},
                               {QStringLiteral("modelId"), selected.modelId}});
    prepared.provider = profile.configured.value(QStringLiteral("providerId")).toString();
    prepared.model = profile.configured.value(QStringLiteral("modelId")).toString();
    const bool localOnly = profile.configured.value(QStringLiteral("privacy")).toString() ==
                           QLatin1String("local-only");
    if (localOnly && modelService_.currentModelMetadata(prepared.provider, prepared.model)
                         .providerKind == ProviderKind::Cloud) {
        prepared = domain_.applyRuntimeState(prepared, QStringLiteral("Failed"),
            QStringLiteral("Local Only blocks the captured cloud provider."), {}, {});
        upsert(std::move(prepared));
        return false;
    }
    const auto resolved = modelService_.resolve(prepared.provider.trimmed(),
                                                prepared.model.trimmed());
    if (!resolved.ok() || !bindModel_(resolved)) {
        prepared = domain_.applyRuntimeState(
            prepared, QStringLiteral("Failed"),
            QStringLiteral("Captured provider or model is unavailable. %1")
                .arg(modelBindingFailureSummary(resolved)),
            {}, {});
        upsert(std::move(prepared));
        return false;
    }

    const auto sessionId = runtime_.createSession();
    if (sessionId.isEmpty()) {
        prepared = domain_.applyRuntimeState(
            prepared, QStringLiteral("Failed"),
            QStringLiteral("Agent runtime could not create a session."), {}, {});
        upsert(std::move(prepared));
        return false;
    }
    AgentSessionOptions options;
    options.runType = QStringLiteral("controlled-task");
    options.workspaceContext.id = prepared.workspaceId;
    options.workspaceContext.rootPath = workspace.rootPath;
    const auto context = profile.configured.value(QStringLiteral("context")).toObject();
    for (const auto& value : context.value(QStringLiteral("include")).toArray())
        if (value.isString()) options.workspaceContext.includeHints.append(value.toString());
    for (const auto& value : context.value(QStringLiteral("exclude")).toArray())
        if (value.isString()) options.workspaceContext.excludeHints.append(value.toString());
    options.workspaceContext.retrievalPreference =
        context.value(QStringLiteral("retrieval")).toString();
    options.workspaceContext.memoryScope =
        context.value(QStringLiteral("memoryScope")).toString();
    options.workspaceName = workspace.name;
    options.presetId = profile.presetId;
    options.profileVersion = QStringLiteral("1");
    const auto toolPreferences = profile.configured.value(QStringLiteral("tools")).toObject();
    const auto extensionPreferences =
        profile.configured.value(QStringLiteral("extensions")).toObject();
    options.restrictAvailableTools = !toolPreferences.isEmpty() || !extensionPreferences.isEmpty();
    for (const auto& descriptor : runtime_.availableTools()) {
        if (toolPreferences.value(descriptor.id).isBool() &&
            !toolPreferences.value(descriptor.id).toBool()) continue;
        if (extensionPreferences.value(descriptor.providerId).isBool() &&
            !extensionPreferences.value(descriptor.providerId).toBool()) continue;
        options.availableToolIds.append(descriptor.id);
    }
    runtime_.configureSession(sessionId, std::move(options));
    sessionByTask_.insert(taskId, sessionId);
    taskBySession_.insert(sessionId, taskId);
    prepared.runtimeSessionId = sessionId;
    prepared.runtimeRunId.clear();
    prepared.state = ControlledTaskState::PendingApproval;
    prepared.resultSummary = QStringLiteral("Agent session starting.");
    upsert(prepared);
    if (!runtime_.start(sessionId, prepared.description)) {
        sessionByTask_.remove(taskId);
        taskBySession_.remove(sessionId);
        prepared = domain_.applyRuntimeState(
            prepared, QStringLiteral("Failed"),
            QStringLiteral("Agent runtime is busy or could not start."), sessionId, {});
        upsert(std::move(prepared));
        return false;
    }
    return true;
}

bool ControlledTaskService::cancelTask(const QString& taskId) {
    const auto sessionId = sessionByTask_.value(taskId);
    if (!sessionId.isEmpty())
        return runtime_.cancel(sessionId);
    auto current = task(taskId);
    if (current.id.isEmpty() || current.state == ControlledTaskState::Completed ||
        current.state == ControlledTaskState::Failed ||
        current.state == ControlledTaskState::Cancelled)
        return false;
    upsert(domain_.cancel(std::move(current)));
    return true;
}

bool ControlledTaskService::skipTask(const QString& taskId) {
    Q_UNUSED(taskId)
    return false; // A session owns its steps; skipping cannot complete an agent task.
}

bool ControlledTaskService::retryTask(const QString& taskId) {
    const auto current = task(taskId);
    if (current.state != ControlledTaskState::Failed &&
        current.state != ControlledTaskState::Cancelled)
        return false;
    return startTask(taskId);
}

bool ControlledTaskService::reorderTask(const QString& taskId, int index) {
    if (task(taskId).id.isEmpty() || sessionByTask_.contains(taskId))
        return false;
    tasks_ = domain_.reorderQueue(std::move(tasks_), taskId, index);
    saveTasks();
    return true;
}

bool ControlledTaskService::setPermission(const QString& workspaceId, const QString& category,
                                          const QString& choice) {
    if (workspaceId.trimmed().isEmpty() || category.trimmed().isEmpty())
        return false;
    permissions_ = domain_.grantPermission(std::move(permissions_), workspaceId, category, choice);
    savePermissions();
    return true;
}

void ControlledTaskService::onAgentEvent(const AgentEvent& event) {
    const auto taskId = taskBySession_.value(event.sessionId);
    if (taskId.isEmpty())
        return;
    auto current = task(taskId);
    if (current.id.isEmpty())
        return;
    if (event.type == AgentEventType::RuntimeStateChanged) {
        if (const auto* run = std::get_if<AgentRunEvent>(&event.payload)) {
            const QString detail = run->phase == AgentLoopPhase::Completed
                                       ? run->finalAnswer
                                       : (run->phase == AgentLoopPhase::AwaitingApproval
                                              ? QStringLiteral("Approval required for %1.")
                                                    .arg(run->pendingTool.toolId)
                                              : run->abortReason);
            current = domain_.applyRuntimeState(current, agentLoopPhaseName(run->phase), detail,
                                                event.sessionId, event.turnId);
            current.completionStatus = agentCompletionStatusName(
                agentLoopPhaseName(run->phase), agentTerminalReasonName(run->terminalReason));
            if (run->phase != AgentLoopPhase::AwaitingApproval &&
                run->phase != AgentLoopPhase::Running && run->phase != AgentLoopPhase::Cancelling) {
                sessionByTask_.remove(taskId);
                taskBySession_.remove(event.sessionId);
            }
            upsert(std::move(current));
        } else if (const auto* state = std::get_if<AgentStateEvent>(&event.payload)) {
            if (state->phase == AgentLoopPhase::Running) {
                current = domain_.applyRuntimeState(current, QStringLiteral("Running"), {},
                                                    event.sessionId, event.turnId);
                if (!state->completionStatus.isEmpty())
                    current.completionStatus = state->completionStatus;
                upsert(std::move(current));
            }
        }
    }
}

} // namespace sentinel::core
