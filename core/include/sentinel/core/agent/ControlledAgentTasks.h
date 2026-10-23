// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace sentinel::core {

enum class ControlledTaskState {
    Draft,
    PendingApproval,
    Running,
    WaitingApproval,
    Completed,
    Failed,
    Cancelled,
};

struct ControlledAgentStep {
    QString id;
    int order = 0;
    QString title;
    QString reason;
    QStringList resourcesUsed;
    QStringList toolsRequested;
    QString outcome;
    ControlledTaskState state = ControlledTaskState::Draft;
};

struct ControlledAgentApproval {
    QString timestampUtc;
    QString choice;
    QString summary;
};

struct ControlledAgentTask {
    QString id;
    QString title;
    QString description;
    QString createdAtUtc;
    QString completedAtUtc;
    QString workspaceId;
    QString provider;
    QString model;
    QString runtimeSessionId;
    QString runtimeRunId;
    QList<ControlledAgentStep> steps;
    QList<ControlledAgentApproval> approvals;
    QString resultSummary;
    QString completionStatus;
    ControlledTaskState state = ControlledTaskState::Draft;
    int currentStepIndex = -1;
};

struct ControlledWorkspacePermission {
    QString workspaceId;
    QString category;
    QString choice;
    QString updatedAtUtc;
};

struct ControlledAgentDiagnostics {
    QString activeTask;
    QString lastCompletedTask;
    QString approvalStatistics;
    QString failureStatistics;
};

QString controlledTaskStateName(ControlledTaskState state);
ControlledTaskState controlledTaskStateFromName(const QString& name);
QString controlledAgentStepSummary(const ControlledAgentStep& step);
QString controlledAgentTaskSummary(const ControlledAgentTask& task);
QString controlledAgentExplainabilitySummary(const ControlledAgentTask& task,
                                             const ControlledAgentStep& step);

class ControlledAgentTaskService final {
public:
    ControlledAgentTaskService();

    QList<ControlledAgentTask> tasksFromJson(const QString& json) const;
    QString tasksToJson(const QList<ControlledAgentTask>& tasks) const;
    QList<ControlledWorkspacePermission> permissionsFromJson(const QString& json) const;
    QString permissionsToJson(const QList<ControlledWorkspacePermission>& permissions) const;

    ControlledAgentTask createPlan(const QString& goal, const QString& workspaceId,
                                   const QString& provider, const QString& model,
                                   const QStringList& resources,
                                   const QList<ControlledAgentTask>& existingTasks) const;
    ControlledAgentTask taskById(const QList<ControlledAgentTask>& tasks,
                                 const QString& taskId) const;
    QList<ControlledAgentTask> upsertTask(QList<ControlledAgentTask> tasks,
                                          const ControlledAgentTask& task) const;

    ControlledAgentTask setSteps(ControlledAgentTask task, const QStringList& orderedSteps) const;
    ControlledAgentTask approve(ControlledAgentTask task, const QString& choice) const;
    ControlledAgentTask deny(ControlledAgentTask task) const;
    ControlledAgentTask cancel(ControlledAgentTask task) const;
    ControlledAgentTask start(ControlledAgentTask task,
                              const QList<ControlledAgentTask>& allTasks) const;
    ControlledAgentTask applyRuntimeState(ControlledAgentTask task, const QString& state,
                                          const QString& detail, const QString& sessionId,
                                          const QString& runId) const;
    ControlledAgentTask skipCurrentStep(ControlledAgentTask task) const;
    ControlledAgentTask retryCurrentStep(ControlledAgentTask task) const;
    QList<ControlledAgentTask> reorderQueue(QList<ControlledAgentTask> tasks, const QString& taskId,
                                            int newIndex) const;

    QList<ControlledWorkspacePermission>
    grantPermission(QList<ControlledWorkspacePermission> permissions, const QString& workspaceId,
                    const QString& category, const QString& choice) const;
    QString permissionChoice(const QList<ControlledWorkspacePermission>& permissions,
                             const QString& workspaceId, const QString& category) const;

    QStringList queueSummaries(const QList<ControlledAgentTask>& tasks,
                               const QString& workspaceId) const;
    QStringList timelineSummaries(const QList<ControlledAgentTask>& tasks,
                                  const QString& workspaceId, const QString& filter) const;
    QStringList permissionSummaries(const QList<ControlledWorkspacePermission>& permissions,
                                    const QString& workspaceId) const;
    QStringList explainabilitySummaries(const ControlledAgentTask& task) const;
    QStringList notificationCategories() const;
    QStringList exportCenterSummaries() const;
    QStringList safetyGuarantees() const;
    ControlledAgentDiagnostics diagnostics(const QList<ControlledAgentTask>& tasks) const;
    QByteArray exportTaskReport(const ControlledAgentTask& task, const QString& format) const;

private:
    QString nextTaskId(const QList<ControlledAgentTask>& tasks) const;
    QString timestampUtc() const;
};

} // namespace sentinel::core
