// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/agent/ControlledAgentTasks.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

#include <algorithm>

namespace sentinel::core {
namespace {

QString normalizedFormat(const QString& format) {
    const auto value = format.trimmed().toLower();
    if (value == QStringLiteral("pdf")) {
        return QStringLiteral("PDF");
    }
    if (value == QStringLiteral("txt") || value == QStringLiteral("text")) {
        return QStringLiteral("TXT");
    }
    if (value == QStringLiteral("json")) {
        return QStringLiteral("JSON");
    }
    return QStringLiteral("Markdown");
}

QByteArray simplePdfFromText(const QString& title, const QString& text) {
    const auto escaped = (title + QStringLiteral("\n\n") + text)
                             .left(12000)
                             .replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
                             .replace(QLatin1Char('('), QStringLiteral("\\("))
                             .replace(QLatin1Char(')'), QStringLiteral("\\)"));
    const auto lines = escaped.split(QLatin1Char('\n'));
    QString content;
    QTextStream stream(&content);
    stream << "BT /F1 10 Tf 48 792 Td 12 TL\n";
    for (const auto& line : lines) {
        stream << "(" << line.left(96) << ") Tj T*\n";
    }
    stream << "ET\n";

    QByteArray pdf;
    QList<int> offsets;
    auto appendObject = [&pdf, &offsets](const QByteArray& object) {
        offsets.append(pdf.size());
        pdf.append(object);
    };
    pdf.append("%PDF-1.4\n");
    appendObject("1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n");
    appendObject("2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n");
    appendObject("3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 842] "
                 "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >> endobj\n");
    appendObject("4 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n");
    const auto contentBytes = content.toUtf8();
    appendObject(QByteArray("5 0 obj << /Length ") + QByteArray::number(contentBytes.size()) +
                 QByteArray(" >> stream\n") + contentBytes + QByteArray("\nendstream endobj\n"));
    const auto xrefOffset = pdf.size();
    pdf.append("xref\n0 6\n0000000000 65535 f \n");
    for (const auto offset : offsets) {
        pdf.append(QByteArray::number(offset).rightJustified(10, '0'));
        pdf.append(" 00000 n \n");
    }
    pdf.append("trailer << /Size 6 /Root 1 0 R >>\nstartxref\n");
    pdf.append(QByteArray::number(xrefOffset));
    pdf.append("\n%%EOF\n");
    return pdf;
}

QJsonArray stringArrayToJson(const QStringList& values) {
    QJsonArray array;
    for (const auto& value : values) {
        array.append(value);
    }
    return array;
}

QStringList stringArrayFromJson(const QJsonArray& array) {
    QStringList values;
    for (const auto& value : array) {
        values.append(value.toString());
    }
    return values;
}

QJsonObject stepToJson(const ControlledAgentStep& step) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), step.id);
    object.insert(QStringLiteral("order"), step.order);
    object.insert(QStringLiteral("title"), step.title);
    object.insert(QStringLiteral("reason"), step.reason);
    object.insert(QStringLiteral("resourcesUsed"), stringArrayToJson(step.resourcesUsed));
    object.insert(QStringLiteral("toolsRequested"), stringArrayToJson(step.toolsRequested));
    object.insert(QStringLiteral("outcome"), step.outcome);
    object.insert(QStringLiteral("state"), controlledTaskStateName(step.state));
    return object;
}

ControlledAgentStep stepFromJson(const QJsonObject& object) {
    return ControlledAgentStep{
        object.value(QStringLiteral("id")).toString(),
        object.value(QStringLiteral("order")).toInt(),
        object.value(QStringLiteral("title")).toString(),
        object.value(QStringLiteral("reason")).toString(),
        stringArrayFromJson(object.value(QStringLiteral("resourcesUsed")).toArray()),
        stringArrayFromJson(object.value(QStringLiteral("toolsRequested")).toArray()),
        object.value(QStringLiteral("outcome")).toString(),
        controlledTaskStateFromName(object.value(QStringLiteral("state")).toString()),
    };
}

QJsonObject approvalToJson(const ControlledAgentApproval& approval) {
    QJsonObject object;
    object.insert(QStringLiteral("timestampUtc"), approval.timestampUtc);
    object.insert(QStringLiteral("choice"), approval.choice);
    object.insert(QStringLiteral("summary"), approval.summary);
    return object;
}

ControlledAgentApproval approvalFromJson(const QJsonObject& object) {
    return {object.value(QStringLiteral("timestampUtc")).toString(),
            object.value(QStringLiteral("choice")).toString(),
            object.value(QStringLiteral("summary")).toString()};
}

QJsonObject taskToJson(const ControlledAgentTask& task) {
    QJsonArray steps;
    for (const auto& step : task.steps) {
        steps.append(stepToJson(step));
    }
    QJsonArray approvals;
    for (const auto& approval : task.approvals) {
        approvals.append(approvalToJson(approval));
    }
    QJsonObject object;
    object.insert(QStringLiteral("id"), task.id);
    object.insert(QStringLiteral("title"), task.title);
    object.insert(QStringLiteral("description"), task.description);
    object.insert(QStringLiteral("createdAtUtc"), task.createdAtUtc);
    object.insert(QStringLiteral("completedAtUtc"), task.completedAtUtc);
    object.insert(QStringLiteral("workspaceId"), task.workspaceId);
    object.insert(QStringLiteral("provider"), task.provider);
    object.insert(QStringLiteral("model"), task.model);
    object.insert(QStringLiteral("runtimeSessionId"), task.runtimeSessionId);
    object.insert(QStringLiteral("runtimeRunId"), task.runtimeRunId);
    object.insert(QStringLiteral("steps"), steps);
    object.insert(QStringLiteral("approvals"), approvals);
    object.insert(QStringLiteral("resultSummary"), task.resultSummary);
    object.insert(QStringLiteral("completionStatus"), task.completionStatus);
    object.insert(QStringLiteral("state"), controlledTaskStateName(task.state));
    object.insert(QStringLiteral("currentStepIndex"), task.currentStepIndex);
    return object;
}

ControlledAgentTask taskFromJson(const QJsonObject& object) {
    ControlledAgentTask task;
    task.id = object.value(QStringLiteral("id")).toString();
    task.title = object.value(QStringLiteral("title")).toString();
    task.description = object.value(QStringLiteral("description")).toString();
    task.createdAtUtc = object.value(QStringLiteral("createdAtUtc")).toString();
    task.completedAtUtc = object.value(QStringLiteral("completedAtUtc")).toString();
    task.workspaceId = object.value(QStringLiteral("workspaceId")).toString();
    task.provider = object.value(QStringLiteral("provider")).toString();
    task.model = object.value(QStringLiteral("model")).toString();
    task.runtimeSessionId = object.value(QStringLiteral("runtimeSessionId")).toString();
    task.runtimeRunId = object.value(QStringLiteral("runtimeRunId")).toString();
    for (const auto& value : object.value(QStringLiteral("steps")).toArray()) {
        task.steps.append(stepFromJson(value.toObject()));
    }
    for (const auto& value : object.value(QStringLiteral("approvals")).toArray()) {
        task.approvals.append(approvalFromJson(value.toObject()));
    }
    task.resultSummary = object.value(QStringLiteral("resultSummary")).toString();
    task.completionStatus = object.value(QStringLiteral("completionStatus")).toString();
    task.state = controlledTaskStateFromName(object.value(QStringLiteral("state")).toString());
    task.currentStepIndex = object.value(QStringLiteral("currentStepIndex")).toInt(-1);
    return task;
}

} // namespace

ControlledAgentTaskService::ControlledAgentTaskService() = default;

QString controlledTaskStateName(ControlledTaskState state) {
    switch (state) {
    case ControlledTaskState::Draft:
        return QStringLiteral("Draft");
    case ControlledTaskState::PendingApproval:
        return QStringLiteral("Pending Approval");
    case ControlledTaskState::Running:
        return QStringLiteral("Running");
    case ControlledTaskState::WaitingApproval:
        return QStringLiteral("Waiting Approval");
    case ControlledTaskState::Completed:
        return QStringLiteral("Completed");
    case ControlledTaskState::Failed:
        return QStringLiteral("Failed");
    case ControlledTaskState::Cancelled:
        return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Draft");
}

ControlledTaskState controlledTaskStateFromName(const QString& name) {
    const auto normalized = name.trimmed().toLower();
    if (normalized == QStringLiteral("pending approval")) {
        return ControlledTaskState::PendingApproval;
    }
    if (normalized == QStringLiteral("running")) {
        return ControlledTaskState::Running;
    }
    if (normalized == QStringLiteral("waiting approval")) {
        return ControlledTaskState::WaitingApproval;
    }
    if (normalized == QStringLiteral("completed")) {
        return ControlledTaskState::Completed;
    }
    if (normalized == QStringLiteral("failed")) {
        return ControlledTaskState::Failed;
    }
    if (normalized == QStringLiteral("cancelled") || normalized == QStringLiteral("canceled")) {
        return ControlledTaskState::Cancelled;
    }
    return ControlledTaskState::Draft;
}

QString controlledAgentStepSummary(const ControlledAgentStep& step) {
    return QStringLiteral("%1. %2 [%3] - %4")
        .arg(step.order)
        .arg(step.title, controlledTaskStateName(step.state),
             step.outcome.isEmpty() ? step.reason : step.outcome);
}

QString controlledAgentTaskSummary(const ControlledAgentTask& task) {
    return QStringLiteral("%1 [%2] - %3 / %4 steps / workspace %5")
        .arg(task.title, controlledTaskStateName(task.state))
        .arg(task.resultSummary.isEmpty()
                 ? task.description
                 : (task.completionStatus.isEmpty()
                        ? task.resultSummary
                        : task.completionStatus + ": " + task.resultSummary))
        .arg(task.steps.size())
        .arg(task.workspaceId);
}

QString controlledAgentExplainabilitySummary(const ControlledAgentTask& task,
                                             const ControlledAgentStep& step) {
    return QStringLiteral(
               "Step executed by: %1 | Reason: %2 | Resources used: %3 | Tools requested: %4 | "
               "Outcome: %5")
        .arg(task.model.isEmpty() ? QStringLiteral("Selected local model") : task.model,
             step.reason,
             step.resourcesUsed.isEmpty() ? QStringLiteral("none")
                                          : step.resourcesUsed.join(QStringLiteral(", ")),
             step.toolsRequested.isEmpty() ? QStringLiteral("none")
                                           : step.toolsRequested.join(QStringLiteral(", ")),
             step.outcome.isEmpty() ? QStringLiteral("Not executed yet.") : step.outcome);
}

QList<ControlledAgentTask> ControlledAgentTaskService::tasksFromJson(const QString& json) const {
    QList<ControlledAgentTask> tasks;
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    const auto array = document.isArray()
                           ? document.array()
                           : document.object().value(QStringLiteral("tasks")).toArray();
    for (const auto& value : array) {
        const auto task = taskFromJson(value.toObject());
        if (!task.id.trimmed().isEmpty()) {
            tasks.append(task);
        }
    }
    return tasks;
}

QString ControlledAgentTaskService::tasksToJson(const QList<ControlledAgentTask>& tasks) const {
    QJsonArray array;
    for (const auto& task : tasks) {
        array.append(taskToJson(task));
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("sentinel.controlledAgentTasks.v1"));
    root.insert(QStringLiteral("tasks"), array);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QList<ControlledWorkspacePermission>
ControlledAgentTaskService::permissionsFromJson(const QString& json) const {
    QList<ControlledWorkspacePermission> permissions;
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    const auto array = document.isArray()
                           ? document.array()
                           : document.object().value(QStringLiteral("permissions")).toArray();
    for (const auto& value : array) {
        const auto object = value.toObject();
        const auto workspaceId = object.value(QStringLiteral("workspaceId")).toString();
        const auto category = object.value(QStringLiteral("category")).toString();
        if (!workspaceId.isEmpty() && !category.isEmpty()) {
            permissions.append({workspaceId, category,
                                object.value(QStringLiteral("choice")).toString(),
                                object.value(QStringLiteral("updatedAtUtc")).toString()});
        }
    }
    return permissions;
}

QString ControlledAgentTaskService::permissionsToJson(
    const QList<ControlledWorkspacePermission>& permissions) const {
    QJsonArray array;
    for (const auto& permission : permissions) {
        QJsonObject object;
        object.insert(QStringLiteral("workspaceId"), permission.workspaceId);
        object.insert(QStringLiteral("category"), permission.category);
        object.insert(QStringLiteral("choice"), permission.choice);
        object.insert(QStringLiteral("updatedAtUtc"), permission.updatedAtUtc);
        array.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("sentinel.controlledAgentPermissions.v1"));
    root.insert(QStringLiteral("permissions"), array);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

ControlledAgentTask ControlledAgentTaskService::createPlan(
    const QString& goal, const QString& workspaceId, const QString& provider, const QString& model,
    const QStringList& resources, const QList<ControlledAgentTask>& existingTasks) const {
    const auto cleanGoal =
        goal.trimmed().isEmpty() ? QStringLiteral("Untitled controlled task") : goal.trimmed();
    ControlledAgentTask task;
    task.id = nextTaskId(existingTasks);
    task.title = cleanGoal.left(80);
    task.description = cleanGoal;
    task.createdAtUtc = timestampUtc();
    task.workspaceId = workspaceId.trimmed().isEmpty() ? QStringLiteral("personal") : workspaceId;
    task.provider = provider.trimmed();
    task.model = model.trimmed();
    task.state = ControlledTaskState::PendingApproval;
    task.currentStepIndex = -1;
    task.resultSummary = QStringLiteral("Plan generated. Approval required before execution.");
    const QStringList titles{QStringLiteral("Run agent task")};
    for (int index = 0; index < titles.size(); ++index) {
        task.steps.append(
            {QStringLiteral("%1-step-%2").arg(task.id).arg(index + 1), index + 1, titles.at(index),
             QStringLiteral("This step exists to satisfy: %1").arg(cleanGoal), resources,
             QStringList{QStringLiteral("Notes")}, QStringLiteral("Waiting for approval."),
             ControlledTaskState::PendingApproval});
    }
    task.approvals.append({timestampUtc(), QStringLiteral("Plan Generated"),
                           QStringLiteral("Plan is editable and cannot run until approved.")});
    return task;
}

ControlledAgentTask ControlledAgentTaskService::taskById(const QList<ControlledAgentTask>& tasks,
                                                         const QString& taskId) const {
    for (const auto& task : tasks) {
        if (task.id == taskId.trimmed()) {
            return task;
        }
    }
    return {};
}

QList<ControlledAgentTask>
ControlledAgentTaskService::upsertTask(QList<ControlledAgentTask> tasks,
                                       const ControlledAgentTask& task) const {
    for (auto& existing : tasks) {
        if (existing.id == task.id) {
            existing = task;
            return tasks;
        }
    }
    tasks.append(task);
    return tasks;
}

ControlledAgentTask ControlledAgentTaskService::setSteps(ControlledAgentTask task,
                                                         const QStringList& orderedSteps) const {
    if (task.state == ControlledTaskState::Running ||
        task.state == ControlledTaskState::WaitingApproval) {
        task.approvals.append({timestampUtc(), QStringLiteral("Modify Denied"),
                               QStringLiteral("Running tasks cannot be self-modified.")});
        return task;
    }
    task.steps.clear();
    int order = 1;
    for (const auto& stepTitle : orderedSteps) {
        const auto title = stepTitle.trimmed();
        if (title.isEmpty()) {
            continue;
        }
        task.steps.append({QStringLiteral("%1-step-%2").arg(task.id).arg(order),
                           order,
                           title,
                           QStringLiteral("User-edited plan step."),
                           {},
                           QStringList{QStringLiteral("Notes")},
                           QStringLiteral("Waiting for approval."),
                           ControlledTaskState::PendingApproval});
        ++order;
    }
    task.state = ControlledTaskState::PendingApproval;
    task.currentStepIndex = -1;
    task.resultSummary = QStringLiteral("Plan modified. Approval required before execution.");
    task.approvals.append({timestampUtc(), QStringLiteral("Plan Modified"),
                           QStringLiteral("User edited controlled task steps.")});
    return task;
}

ControlledAgentTask ControlledAgentTaskService::approve(ControlledAgentTask task,
                                                        const QString& choice) const {
    if (task.steps.isEmpty()) {
        task.state = ControlledTaskState::Failed;
        task.resultSummary = QStringLiteral("Approval refused because the plan has no steps.");
        return task;
    }
    task.state = ControlledTaskState::PendingApproval;
    task.approvals.append(
        {timestampUtc(),
         choice.trimmed().isEmpty() ? QStringLiteral("Approve Once") : choice.trimmed(),
         QStringLiteral("User approved visible controlled execution.")});
    task.resultSummary = QStringLiteral("Approved once. Start is still explicit.");
    return task;
}

ControlledAgentTask ControlledAgentTaskService::deny(ControlledAgentTask task) const {
    task.state = ControlledTaskState::Cancelled;
    task.completedAtUtc = timestampUtc();
    task.resultSummary = QStringLiteral("Task denied before execution.");
    task.approvals.append({timestampUtc(), QStringLiteral("Deny"),
                           QStringLiteral("User denied execution approval.")});
    return task;
}

ControlledAgentTask ControlledAgentTaskService::cancel(ControlledAgentTask task) const {
    task.state = ControlledTaskState::Cancelled;
    task.completedAtUtc = timestampUtc();
    task.resultSummary = QStringLiteral("Task cancelled by user.");
    task.approvals.append({timestampUtc(), QStringLiteral("Cancel"),
                           QStringLiteral("User cancelled visible task execution.")});
    return task;
}

ControlledAgentTask
ControlledAgentTaskService::start(ControlledAgentTask task,
                                  const QList<ControlledAgentTask>& allTasks) const {
    for (const auto& other : allTasks) {
        if (other.id != task.id && (other.state == ControlledTaskState::Running ||
                                    other.state == ControlledTaskState::WaitingApproval)) {
            task.resultSummary = QStringLiteral("Start refused: another task is already running.");
            task.approvals.append({timestampUtc(), QStringLiteral("Start Refused"),
                                   QStringLiteral("Single active task only in this phase.")});
            return task;
        }
    }
    const bool approved = std::any_of(task.approvals.cbegin(), task.approvals.cend(),
                                      [](const ControlledAgentApproval& approval) {
                                          return approval.choice.contains(QStringLiteral("Approve"),
                                                                          Qt::CaseInsensitive);
                                      });
    if (!approved) {
        task.resultSummary = QStringLiteral("Start refused: approval is required.");
        return task;
    }
    task.state = ControlledTaskState::Running;
    task.runtimeSessionId.clear();
    task.runtimeRunId.clear();
    task.completedAtUtc.clear();
    task.currentStepIndex = 0;
    for (qsizetype index = 0; index < task.steps.size(); ++index) {
        task.steps[index].state =
            index == 0 ? ControlledTaskState::Running : ControlledTaskState::PendingApproval;
    }
    task.resultSummary = QStringLiteral("Task ready for an AgentRuntime session.");
    task.approvals.append({timestampUtc(), QStringLiteral("Task Started"),
                           QStringLiteral("Foreground controlled execution started.")});
    return task;
}

ControlledAgentTask ControlledAgentTaskService::applyRuntimeState(
    ControlledAgentTask task, const QString& state, const QString& detail,
    const QString& sessionId, const QString& runId) const {
    if (task.id.isEmpty())
        return task;
    task.completionStatus = state;
    task.runtimeSessionId = sessionId;
    if (!runId.isEmpty())
        task.runtimeRunId = runId;
    if (state == QLatin1String("Awaiting Approval")) {
        task.state = ControlledTaskState::WaitingApproval;
        task.resultSummary = detail.isEmpty()
                                 ? QStringLiteral("Agent approval is required to continue.")
                                 : detail;
    } else if (state == QLatin1String("Completed") && !detail.trimmed().isEmpty()) {
        task.state = ControlledTaskState::Completed;
        task.resultSummary = detail;
    } else if (state == QLatin1String("Cancelled")) {
        task.state = ControlledTaskState::Cancelled;
        task.resultSummary = detail.isEmpty() ? QStringLiteral("Agent run cancelled.") : detail;
    } else if (state == QLatin1String("Failed") || state == QLatin1String("Stuck") ||
               state == QLatin1String("Completed")) {
        task.state = ControlledTaskState::Failed;
        task.resultSummary = detail.isEmpty() ? QStringLiteral("Agent run failed.") : detail;
    } else {
        task.state = ControlledTaskState::Running;
        if (!detail.isEmpty())
            task.resultSummary = detail;
    }
    if (task.state == ControlledTaskState::Completed || task.state == ControlledTaskState::Failed ||
        task.state == ControlledTaskState::Cancelled) {
        task.completedAtUtc = timestampUtc();
        if (task.currentStepIndex >= 0 && task.currentStepIndex < task.steps.size()) {
            auto& step = task.steps[task.currentStepIndex];
            step.state = task.state;
            step.outcome = task.resultSummary;
        }
        task.currentStepIndex = -1;
    }
    return task;
}

ControlledAgentTask ControlledAgentTaskService::skipCurrentStep(ControlledAgentTask task) const {
    if (task.state != ControlledTaskState::Running || task.currentStepIndex < 0 ||
        task.currentStepIndex >= task.steps.size()) {
        return task;
    }
    auto& step = task.steps[task.currentStepIndex];
    step.state = ControlledTaskState::Cancelled;
    step.outcome = QStringLiteral("Skipped by user.");
    const auto nextIndex = task.currentStepIndex + 1;
    if (nextIndex >= task.steps.size()) {
        task.state = ControlledTaskState::Completed;
        task.completedAtUtc = timestampUtc();
        task.currentStepIndex = -1;
        task.resultSummary = QStringLiteral("Task cancelled after its final step was skipped.");
    } else {
        task.currentStepIndex = nextIndex;
        task.steps[nextIndex].state = ControlledTaskState::Running;
        task.resultSummary = QStringLiteral("Step skipped by user. Next step is visible.");
    }
    return task;
}

ControlledAgentTask ControlledAgentTaskService::retryCurrentStep(ControlledAgentTask task) const {
    if (task.state != ControlledTaskState::Running || task.currentStepIndex < 0 ||
        task.currentStepIndex >= task.steps.size()) {
        return task;
    }
    auto& step = task.steps[task.currentStepIndex];
    step.state = ControlledTaskState::Running;
    step.outcome = QStringLiteral("Retry requested by user; no automatic retry occurred.");
    task.approvals.append(
        {timestampUtc(), QStringLiteral("Retry Step"),
         QStringLiteral("User explicitly requested retry for the visible step.")});
    task.resultSummary = QStringLiteral("Retry ready for current visible step.");
    return task;
}

QList<ControlledAgentTask>
ControlledAgentTaskService::reorderQueue(QList<ControlledAgentTask> tasks, const QString& taskId,
                                         int newIndex) const {
    auto index = -1;
    for (qsizetype i = 0; i < tasks.size(); ++i) {
        if (tasks.at(i).id == taskId.trimmed()) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0 || tasks.at(index).state == ControlledTaskState::Running) {
        return tasks;
    }
    const auto item = tasks.takeAt(index);
    tasks.insert(std::clamp(newIndex, 0, static_cast<int>(tasks.size())), item);
    return tasks;
}

QList<ControlledWorkspacePermission>
ControlledAgentTaskService::grantPermission(QList<ControlledWorkspacePermission> permissions,
                                            const QString& workspaceId, const QString& category,
                                            const QString& choice) const {
    const auto normalizedChoice =
        choice == QStringLiteral("Allow For Workspace")
            ? choice
            : (choice == QStringLiteral("Allow Once") ? choice : QStringLiteral("Deny"));
    for (auto& permission : permissions) {
        if (permission.workspaceId == workspaceId && permission.category == category) {
            permission.choice = normalizedChoice;
            permission.updatedAtUtc = timestampUtc();
            return permissions;
        }
    }
    permissions.append({workspaceId, category, normalizedChoice, timestampUtc()});
    return permissions;
}

QString ControlledAgentTaskService::permissionChoice(
    const QList<ControlledWorkspacePermission>& permissions, const QString& workspaceId,
    const QString& category) const {
    for (const auto& permission : permissions) {
        if (permission.workspaceId == workspaceId && permission.category == category) {
            return permission.choice;
        }
    }
    return QStringLiteral("Deny");
}

QStringList ControlledAgentTaskService::queueSummaries(const QList<ControlledAgentTask>& tasks,
                                                       const QString& workspaceId) const {
    QStringList summaries;
    for (const auto& task : tasks) {
        if (task.workspaceId == workspaceId) {
            summaries.append(controlledAgentTaskSummary(task));
        }
    }
    return summaries;
}

QStringList ControlledAgentTaskService::timelineSummaries(const QList<ControlledAgentTask>& tasks,
                                                          const QString& workspaceId,
                                                          const QString& filter) const {
    QStringList summaries;
    const auto normalized = filter.trimmed().toLower();
    for (const auto& task : tasks) {
        if (task.workspaceId != workspaceId) {
            continue;
        }
        const auto state = controlledTaskStateName(task.state).toLower();
        if (!normalized.isEmpty() && normalized != QStringLiteral("all") &&
            !state.contains(normalized)) {
            continue;
        }
        summaries.append(controlledAgentTaskSummary(task));
    }
    return summaries;
}

QStringList ControlledAgentTaskService::permissionSummaries(
    const QList<ControlledWorkspacePermission>& permissions, const QString& workspaceId) const {
    const QStringList categories{QStringLiteral("Files"),    QStringLiteral("Clipboard"),
                                 QStringLiteral("Terminal"), QStringLiteral("Browser"),
                                 QStringLiteral("Calendar"), QStringLiteral("Email"),
                                 QStringLiteral("Notes"),    QStringLiteral("Downloads")};
    QStringList summaries;
    for (const auto& category : categories) {
        summaries.append(QStringLiteral("%1: %2").arg(
            category, permissionChoice(permissions, workspaceId, category)));
    }
    return summaries;
}

QStringList
ControlledAgentTaskService::explainabilitySummaries(const ControlledAgentTask& task) const {
    QStringList summaries;
    for (const auto& step : task.steps) {
        summaries.append(controlledAgentExplainabilitySummary(task, step));
    }
    return summaries;
}

QStringList ControlledAgentTaskService::notificationCategories() const {
    return {QStringLiteral("Task Planned"), QStringLiteral("Approval Needed"),
            QStringLiteral("Task Started"), QStringLiteral("Task Completed"),
            QStringLiteral("Task Failed"),  QStringLiteral("Task Cancelled")};
}

QStringList ControlledAgentTaskService::exportCenterSummaries() const {
    return {QStringLiteral("Task reports - Markdown, PDF, TXT, JSON"),
            QStringLiteral("Execution summaries - Markdown, PDF, TXT, JSON"),
            QStringLiteral("Approval logs - Markdown, PDF, TXT, JSON")};
}

QStringList ControlledAgentTaskService::safetyGuarantees() const {
    return {QStringLiteral("No autonomous execution."),
            QStringLiteral("Tools run only after explicit approval."),
            QStringLiteral("Every execution passes the sandbox policy."),
            QStringLiteral("No background approvals."),
            QStringLiteral("No self-modifying tasks."),
            QStringLiteral("No recursive task generation."),
            QStringLiteral("No automatic retries without approval.")};
}

ControlledAgentDiagnostics
ControlledAgentTaskService::diagnostics(const QList<ControlledAgentTask>& tasks) const {
    int approvals = 0;
    int denied = 0;
    int failed = 0;
    QString active = QStringLiteral("No active task");
    QString completed = QStringLiteral("No completed task");
    for (const auto& task : tasks) {
        if (task.state == ControlledTaskState::Running) {
            active = controlledAgentTaskSummary(task);
        }
        if (task.state == ControlledTaskState::Completed) {
            completed = controlledAgentTaskSummary(task);
        }
        if (task.state == ControlledTaskState::Failed) {
            ++failed;
        }
        for (const auto& approval : task.approvals) {
            if (approval.choice.contains(QStringLiteral("Approve"), Qt::CaseInsensitive)) {
                ++approvals;
            }
            if (approval.choice.contains(QStringLiteral("Deny"), Qt::CaseInsensitive)) {
                ++denied;
            }
        }
    }
    return {active, completed,
            QStringLiteral("%1 approval(s), %2 denial(s)").arg(approvals).arg(denied),
            QStringLiteral("%1 failed task(s)").arg(failed)};
}

QByteArray ControlledAgentTaskService::exportTaskReport(const ControlledAgentTask& task,
                                                        const QString& format) const {
    const auto outputFormat = normalizedFormat(format);
    if (outputFormat == QStringLiteral("JSON")) {
        return QJsonDocument(taskToJson(task)).toJson(QJsonDocument::Indented);
    }
    QString output;
    QTextStream stream(&output);
    const bool markdown = outputFormat == QStringLiteral("Markdown");
    stream << (markdown ? "# " : "") << "Controlled Agent Task Report\n\n";
    stream << "Title: " << task.title << "\n";
    stream << "Status: " << controlledTaskStateName(task.state) << "\n";
    stream << "Workspace: " << task.workspaceId << "\n";
    stream << "Provider: " << task.provider << "\n";
    stream << "Model: " << task.model << "\n";
    stream << "Created: " << task.createdAtUtc << "\n";
    stream << "Completed: "
           << (task.completedAtUtc.isEmpty() ? QStringLiteral("not completed")
                                             : task.completedAtUtc)
           << "\n\n";
    stream << (markdown ? "## Steps\n" : "Steps\n");
    for (const auto& step : task.steps) {
        stream << controlledAgentStepSummary(step) << "\n";
    }
    stream << "\n" << (markdown ? "## Approval Log\n" : "Approval Log\n");
    for (const auto& approval : task.approvals) {
        stream << approval.timestampUtc << " / " << approval.choice << " / " << approval.summary
               << "\n";
    }
    stream << "\nResult: " << task.resultSummary << "\n";
    if (outputFormat == QStringLiteral("PDF")) {
        return simplePdfFromText(QStringLiteral("Controlled Agent Task Report"), output);
    }
    return output.toUtf8();
}

QString ControlledAgentTaskService::nextTaskId(const QList<ControlledAgentTask>& tasks) const {
    return QStringLiteral("controlled-task-%1").arg(tasks.size() + 1);
}

QString ControlledAgentTaskService::timestampUtc() const {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

} // namespace sentinel::core
