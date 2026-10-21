// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/agent/SQLiteAgentRunStore.h"
#include "sentinel/core/app/StorageAdmission.h"
#include "sentinel/core/chat/SQLiteChatHistoryStore.h"
#include "sentinel/core/security/PermissionService.h"
#include "sentinel/core/security/SQLitePermissionGrantStore.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace sentinel::core;
using namespace std::chrono_literals;
class StorageAsyncCoordinationTest final : public QObject {
    Q_OBJECT
private slots:
    void readsCannotRecreateMissingDatabases() {
        QTemporaryDir profile;
        auto authority = std::make_shared<StorageAdmission>();
        const auto runsPath = profile.filePath("runs.sqlite");
        const auto grantsPath = profile.filePath("grants.sqlite");
        SQLiteAgentRunStore runs(runsPath, authority);
        SQLitePermissionGrantStore grants(grantsPath, authority);
        QVERIFY(QFile::remove(runsPath));
        QVERIFY(QFile::remove(grantsPath));
        QCOMPARE(authority->prepareLocal(0ms), StorageAdmission::Result::Ready);
        QVERIFY(runs.recentRuns(10).isEmpty());
        QList<PersistentPermissionGrant> loaded;
        QVERIFY(!grants.load(loaded));
        QVERIFY(!QFileInfo::exists(runsPath));
        QVERIFY(!QFileInfo::exists(grantsPath));
    }

    void historyRowsAndStartupAreFenced() {
        QTemporaryDir profile;
        auto authority = std::make_shared<StorageAdmission>();
        SQLiteChatHistoryStore history(profile.filePath("history.sqlite"), authority);
        QVERIFY(history.isAvailable());
        ChatMessage message;
        message.id = 1;
        message.role = ChatRole::User;
        message.content = "original";
        message.timestamp = QDateTime::currentDateTimeUtc();
        message.status = ChatMessageStatus::Sent;
        history.appendMessage(message);
        QVERIFY(history.lastError().isEmpty());
        QCOMPARE(authority->prepareLocal(0ms), StorageAdmission::Result::Ready);
        message.content = "refused";
        history.appendMessage(message);
        QVERIFY(!history.lastError().isEmpty());
        history.clear();
        QCOMPARE(history.pruneCompletedBefore(message.timestamp.addDays(1)), -1);
        SQLiteChatHistoryStore unopened(profile.filePath("not-created.sqlite"), authority);
        QVERIFY(!unopened.isAvailable());
        QVERIFY(!QFileInfo::exists(profile.filePath("not-created.sqlite")));
        QCOMPARE(history.loadMessages().first().content, QString("original"));
        QCOMPARE(authority->enterMaintenance(), StorageAdmission::Result::Ready);
        QCOMPARE(authority->resume(), StorageAdmission::Result::Ready);
        history.appendMessage(message);
        QCOMPARE(history.loadMessages().first().content, QString("original"));
        SQLiteChatHistoryStore replacement(profile.filePath("history.sqlite"), authority);
        replacement.appendMessage(message);
        QCOMPARE(replacement.loadMessages().first().content, QString("refused"));
    }
    void agentEvidenceRemainsUnchangedOnRefusal() {
        QTemporaryDir profile;
        auto authority = std::make_shared<StorageAdmission>();
        SQLiteAgentRunStore runs(profile.filePath("runs.sqlite"), authority);
        AgentEvent event;
        event.turnId = "run";
        event.sessionId = "session";
        event.timestamp = QDateTime::currentDateTimeUtc();
        event.type = AgentEventType::RunStarted;
        event.payload = AgentRunStartedEvent{};
        QVERIFY(runs.record(event));
        QVERIFY(!runs.clearHistory());
        event.type = AgentEventType::AgentCompleted;
        event.turnId = "missing-run";
        event.payload = AgentTextEvent{QStringLiteral("must not be recorded")};
        QVERIFY(!runs.record(event));
        event.turnId = "run";
        event.type = AgentEventType::ToolRequested;
        event.stepId = "step";
        event.toolCallId = "tool";
        event.stepIndex = 1;
        AgentToolEvent tool;
        tool.toolId = "fixture";
        event.payload = tool;
        QVERIFY(runs.record(event));
        event.type = AgentEventType::ToolApprovalRequired;
        QVERIFY(runs.record(event));
        QCOMPARE(runs.toolCallsForRun("run", 10).size(), 1);
        const auto before = runs.toolCallsForRun("run", 10).first().status;
        event.sessionId = "other-session";
        QVERIFY(!runs.record(event));
        QCOMPARE(runs.toolCallsForRun("run", 10).first().status, before);
        event.sessionId = "session";
        QCOMPARE(authority->prepareLocal(0ms), StorageAdmission::Result::Ready);
        event.type = AgentEventType::ToolExecutionCompleted;
        QVERIFY(!runs.record(event));
        QVERIFY(!runs.clearHistory());
        QCOMPARE(runs.pruneCompletedBefore(event.timestamp.addDays(1)), -1);
        QCOMPARE(runs.toolCallsForRun("run", 10).first().status, before);
        QCOMPARE(runs.runById("run").state, QString("Running"));
        QCOMPARE(authority->resume(), StorageAdmission::Result::Ready);
        QVERIFY(runs.record(event));
    }
    void permissionCreationAndRevocationFailClosed() {
        QTemporaryDir profile;
        auto authority = std::make_shared<StorageAdmission>();
        auto store = std::make_shared<SQLitePermissionGrantStore>(profile.filePath("grants.sqlite"),
                                                                  authority);
        PermissionService permissions(store);
        AuthorizationRequest request{SecurityDomain::Network, AccessMode::Read, "fixture.invalid"};
        request.resourceKind = AuthorizationResourceKind::Host;
        QVERIFY(permissions.grantAuthorization(request, "session", true));
        QCOMPARE(permissions.evaluateAuthorization(request, "session"), PermissionEffect::Allow);
        const auto id = permissions.persistentGrants().first().id;
        QCOMPARE(authority->prepareLocal(0ms), StorageAdmission::Result::Ready);
        AuthorizationRequest another = request;
        another.resource = "another.invalid";
        QVERIFY(!permissions.grantAuthorization(another, "session", true));
        QCOMPARE(permissions.persistentGrants().size(), 1);
        QVERIFY(!permissions.removePersistentGrant(id));
        QCOMPARE(permissions.evaluateAuthorization(request, "session"), PermissionEffect::Deny);
        QList<PersistentPermissionGrant> durable;
        QVERIFY(store->load(durable));
        QCOMPARE(durable.size(), 1);
        QCOMPARE(authority->resume(), StorageAdmission::Result::Ready);
        // A failed revoke never silently restores the cached authority.
        QCOMPARE(permissions.evaluateAuthorization(request, "session"), PermissionEffect::Deny);
        QVERIFY(permissions.removePersistentGrant(id));
        QVERIFY(store->load(durable));
        QVERIFY(durable.isEmpty());
    }
};
QTEST_GUILESS_MAIN(StorageAsyncCoordinationTest)
#include "test_storage_async_coordination.moc"
