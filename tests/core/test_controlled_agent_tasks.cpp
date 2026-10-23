// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/agent/ControlledAgentTasks.h"

#include <QtTest/QtTest>

using sentinel::core::ControlledAgentTaskService;
using sentinel::core::ControlledTaskState;

class ControlledAgentTasksTest final : public QObject {
    Q_OBJECT

private slots:
    void acceptedFinalStaysCompleted() {
        ControlledAgentTaskService service;
        auto task = service.createPlan("Write fixture", "coding", "Local", "fixture", {}, {});
        task = service.applyRuntimeState(task, "Completed", "Verified final", "session", "run");
        QCOMPARE(task.state, ControlledTaskState::Completed);
        const auto restored = service.tasksFromJson(service.tasksToJson({task}));
        QCOMPARE(restored.first().state, ControlledTaskState::Completed);
    }
    void plannerLifecycleRequiresApproval();
    void queueAllowsSingleActiveTask();
    void permissionsPersistByWorkspace();
    void exportContainsApprovalLog();
};

void ControlledAgentTasksTest::plannerLifecycleRequiresApproval() {
    ControlledAgentTaskService service;
    QList<sentinel::core::ControlledAgentTask> tasks;

    auto task = service.createPlan(QStringLiteral("Prepare a summary of these documents"),
                                   QStringLiteral("research"), QStringLiteral("Local Ollama"),
                                   QStringLiteral("llama3"), {QStringLiteral("paper.pdf")}, tasks);

    QCOMPARE(task.state, ControlledTaskState::PendingApproval);
    QCOMPARE(task.steps.size(), 1);
    QCOMPARE(task.steps.first().title, QStringLiteral("Run agent task"));
    QVERIFY(service.start(task, tasks).state != ControlledTaskState::Running);

    task = service.approve(task, QStringLiteral("Approve Once"));
    tasks = service.upsertTask(tasks, task);
    task = service.start(task, tasks);
    QCOMPARE(task.state, ControlledTaskState::Running);
    QCOMPARE(task.currentStepIndex, 0);

    task = service.applyRuntimeState(task, QStringLiteral("Awaiting Approval"),
                                     QStringLiteral("Tool approval required"),
                                     QStringLiteral("session-1"), QStringLiteral("run-1"));
    QCOMPARE(task.state, ControlledTaskState::WaitingApproval);
    QCOMPARE(task.runtimeSessionId, QStringLiteral("session-1"));
}

void ControlledAgentTasksTest::queueAllowsSingleActiveTask() {
    ControlledAgentTaskService service;
    QList<sentinel::core::ControlledAgentTask> tasks;
    auto first = service.approve(
        service.createPlan(QStringLiteral("First"), QStringLiteral("coding"),
                           QStringLiteral("Local"), QStringLiteral("model"), {}, tasks),
        QStringLiteral("Approve Once"));
    tasks = service.upsertTask(tasks, first);
    first = service.start(first, tasks);
    tasks = service.upsertTask(tasks, first);

    auto second = service.approve(
        service.createPlan(QStringLiteral("Second"), QStringLiteral("coding"),
                           QStringLiteral("Local"), QStringLiteral("model"), {}, tasks),
        QStringLiteral("Approve Once"));
    second = service.start(second, tasks);
    QCOMPARE(second.state, ControlledTaskState::PendingApproval);
    QVERIFY(second.resultSummary.contains(QStringLiteral("another task is already running")));
}

void ControlledAgentTasksTest::permissionsPersistByWorkspace() {
    ControlledAgentTaskService service;
    auto permissions =
        service.grantPermission({}, QStringLiteral("writing"), QStringLiteral("Files"),
                                QStringLiteral("Allow For Workspace"));
    permissions = service.grantPermission(permissions, QStringLiteral("writing"),
                                          QStringLiteral("Terminal"), QStringLiteral("Deny"));
    const auto json = service.permissionsToJson(permissions);
    const auto restored = service.permissionsFromJson(json);

    QCOMPARE(service.permissionChoice(restored, QStringLiteral("writing"), QStringLiteral("Files")),
             QStringLiteral("Allow For Workspace"));
    QCOMPARE(
        service.permissionChoice(restored, QStringLiteral("research"), QStringLiteral("Files")),
        QStringLiteral("Deny"));
}

void ControlledAgentTasksTest::exportContainsApprovalLog() {
    ControlledAgentTaskService service;
    auto task = service.createPlan(QStringLiteral("Summarize notes"), QStringLiteral("personal"),
                                   QStringLiteral("Local"), QStringLiteral("model"), {}, {});
    task = service.approve(task, QStringLiteral("Approve Once"));

    const auto report =
        QString::fromUtf8(service.exportTaskReport(task, QStringLiteral("Markdown")));
    QVERIFY(report.contains(QStringLiteral("Controlled Agent Task Report")));
    QVERIFY(report.contains(QStringLiteral("Approval Log")));
    QVERIFY(report.contains(QStringLiteral("Approve Once")));

    const auto pdf = service.exportTaskReport(task, QStringLiteral("PDF"));
    QVERIFY(pdf.startsWith("%PDF-1.4"));
}

QTEST_MAIN(ControlledAgentTasksTest)

#include "test_controlled_agent_tasks.moc"
