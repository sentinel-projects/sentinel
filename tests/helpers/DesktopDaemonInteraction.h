// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "DesktopDaemonFixture.h"
#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/desktop/DesktopRuntimeClient.h"
#include "sentinel/desktop/DesktopShellViewModel.h"
#include "sentinel/desktop/viewmodels/AgentInspectorViewModel.h"
#include <QAccessible>
#include <QApplication>
#include <QDirIterator>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>
namespace sentinel::test {
inline QQuickItem* desktopItem(QQuickItem* parent, const QString& name, bool prefix = false) {
    if ((prefix ? parent->objectName().startsWith(name) : parent->objectName() == name) &&
        parent->isVisible()) {
        return parent;
    }
    for (auto* child : parent->childItems()) {
        if (auto* found = desktopItem(child, name, prefix)) {
            return found;
        }
    }
    return nullptr;
}
inline QByteArray desktopBytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
inline bool desktopContains(QQuickItem* item, const QString& text) {
    if (item->isVisible() && item->property("text").toString().contains(text)) {
        return true;
    }
    for (auto* child : item->childItems()) {
        if (desktopContains(child, text)) {
            return true;
        }
    }
    return false;
}
inline int desktopSuccessfulWrites(const QString& session) {
    const auto connection = QUuid::createUuid().toString();
    int count = -1;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                               .filePath("agent_runs.sqlite3"));
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare("SELECT COUNT(*) FROM agent_tool_calls c JOIN agent_steps s ON "
                          "c.step_id=s.step_id JOIN agent_runs r ON s.run_id=r.run_id WHERE "
                          "r.session_id=? AND c.tool_id='write-file' AND c.status='Succeeded'");
            query.addBindValue(session);
            if (query.exec() && query.next()) {
                count = query.value(0).toInt();
            }
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return count;
}
inline int runDesktopDaemonInteraction(QQuickWindow* window, desktop::DesktopShellViewModel& vm,
                                       desktop::DesktopRuntimeClient& runtime,
                                       desktop::DaemonClient& transport,
                                       desktop::AgentInspectorViewModel& inspector,
                                       daemon::DaemonIpcServer& server,
                                       core::ApplicationController& controller,
                                       const std::shared_ptr<DesktopScript>& script,
                                       const QDir& profile) {
    auto check = [&](bool ok, const char* expression, int line) {
        if (!ok) {
            qCritical().noquote() << "P606_FAIL" << line << expression;
        }
        return ok;
    };
#define P606_CHECK(expression)                                                                     \
    do {                                                                                           \
        if (!check(bool(expression), #expression, __LINE__))                                       \
            return 220;                                                                            \
    } while (false)
    auto item = [&](const QString& name) { return desktopItem(window->contentItem(), name); };
    auto wait = [&](auto condition) { return QTest::qWaitFor(condition, 5000); };
    auto activate = [&](const QString& name) {
        auto* target = item(name);
        if (!target || !target->isEnabled()) {
            return false;
        }
        target->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTest::qWait(30);
        return true;
    };
    auto type = [&](const QByteArray& text) {
        auto* prompt = item("chatPromptInput");
        if (!prompt || !prompt->isEnabled()) {
            return false;
        }
        prompt->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Backspace);
        for (auto c : text) {
            QTest::keyClick(window, c);
        }
        return true;
    };
    QSignalSpy wireEvents(&transport, &desktop::DaemonClient::eventReceived);
    auto approvalRun = [&](const QJsonValue& approvalId) {
        for (const auto& event : wireEvents) {
            const auto payload = event[1].toJsonObject();
            if (event[0].toString() == "approval.requested" &&
                payload.value("approval_id") == approvalId) {
                return payload.value("run_id").toString();
            }
        }
        return QString{};
    };
    auto state = [&] {
        return controller.agentRuntime()->sessionState(controller.activeAgentSessionId());
    };
    auto record = [&](const QString& scenario, const QJsonObject& oracle = QJsonObject{}) {
        auto o = oracle;
        o.insert("scenario", scenario);
        o.insert("outcome", "pass");
        qInfo().noquote() << "P606_SCENARIO" << QJsonDocument(o).toJson(QJsonDocument::Compact);
    };
    auto renderedComposer = [&] {
        auto* prompt = item("chatComposer");
        if (!prompt) {
            return false;
        }
        QTest::qWait(50);
        const auto image = window->grabWindow();
        const auto region = prompt->mapRectToScene(prompt->boundingRect())
                                .toAlignedRect()
                                .intersected(image.rect());
        int dark = 0;
        for (int y = region.top(); y <= region.bottom(); ++y) {
            for (int x = region.left(); x <= region.right(); ++x) {
                if (qGray(image.pixel(x, y)) < 160) {
                    ++dark;
                }
            }
        }
        return dark > 10;
    };
    const auto captureRoot = qEnvironmentVariable("SENTINEL_DESKTOP_DAEMON_EVIDENCE_DIR");
    auto capture = [&](const QString& tag) {
        if (captureRoot.isEmpty()) {
            return true;
        }
        QTest::mouseMove(window, QPoint(window->width() - 5, 5));
        QTest::qWait(100);
        const auto image = window->grabWindow();
        if (image.isNull() || !image.save(QDir(captureRoot).filePath(tag + ".png"))) {
            return false;
        }
        qInfo().noquote() << "P606_CAPTURE"
                          << QJsonDocument(
                                 QJsonObject{{"tag", tag},
                                             {"width", image.width()},
                                             {"height", image.height()},
                                             {"dpr", window->devicePixelRatio()},
                                             {"font", QApplication::font().family()},
                                             {"fontPointSize", QApplication::font().pointSizeF()},
                                             {"theme", vm.themeName()},
                                             {"renderer", "software"},
                                             {"motion", "reduced"}})
                                 .toJson(QJsonDocument::Compact);
        return true;
    };
    if (!captureRoot.isEmpty()) {
        const auto allowed =
            QFileInfo(QStringLiteral(CERTIFICATION_DAEMON_EVIDENCE_ROOT)).canonicalFilePath();
        const auto requested = QFileInfo(captureRoot).canonicalFilePath();
        P606_CHECK(!allowed.isEmpty() && requested.startsWith(allowed + QDir::separator()));
    }
#if defined(_MSC_VER)
    qInfo().noquote() << "P606_ENV" << QString::fromLatin1(qVersion()) << " compiler MSVC "
                      << _MSC_VER;
#else
    qInfo().noquote() << "P606_ENV" << QString::fromLatin1(qVersion()) << " compiler "
                      << QString::fromLatin1(__VERSION__);
#endif
    P606_CHECK(window);
    window->setWidth(780);
    window->setHeight(720);
    window->requestActivate();
    P606_CHECK(!vm.daemonConnected() && !runtime.ready());
    P606_CHECK(desktopContains(window->contentItem(), "Disconnected"));
    P606_CHECK(type("disconnected text"));
    P606_CHECK(item("chatSendButton") && !item("chatSendButton")->isEnabled());
    P606_CHECK(renderedComposer());
    P606_CHECK(capture("disconnected-compact"));
    record("disconnected", {{"rendered_composer", true}});
    P606_CHECK(server.startServer(profile.filePath("absent.sock")));
    transport.connectToDaemon();
    P606_CHECK(wait([&] { return runtime.ready() && vm.daemonConnected(); }));
    P606_CHECK(vm.localChatSendAvailable());
    P606_CHECK(item("chatSendButton")->isEnabled());
    record("connected", {{"ready", runtime.ready()}, {"send_enabled", true}});
    window->setWidth(1320);
    P606_CHECK(type("p606 streaming message"));
    QTest::keyClick(window, Qt::Key_Return);
    const auto firstSession = runtime.sessionId();
    P606_CHECK(wait([&] {
        return script->entered.available() == 1 &&
               desktopContains(window->contentItem(), "p606_delta");
    }));
    script->entered.acquire();
    P606_CHECK(runtime.sessionId() == firstSession);
    P606_CHECK(!item("chatPromptInput")->isEnabled());
    P606_CHECK(renderedComposer());
    P606_CHECK(capture("streaming-wide"));
    record("chat-stream", {{"session_matches", true}, {"rendered_composer", true}});
    script->release.release();
    P606_CHECK(wait([&] { return vm.chatSendLifecycleState() == "completed"; }));
    core::SQLiteConversationStore persisted(profile.filePath("chat.sqlite3"));
    auto stored = persisted.loadMessages(firstSession);
    P606_CHECK(!stored.isEmpty() && stored.last().content == "p606_delta completed" &&
               stored.last().status == core::ChatMessageStatus::Completed);
    P606_CHECK(desktopContains(window->contentItem(), stored.last().content));
    record("chat-persisted", {{"output", stored.last().content}});
    P606_CHECK(activate("chatNewConversation"));
    P606_CHECK(wait([&] { return runtime.sessionId() != firstSession && runtime.ready(); }));
    const auto cancelSession = runtime.sessionId();
    P606_CHECK(type("p606 cancelled message"));
    QTest::keyClick(window, Qt::Key_Return);
    P606_CHECK(wait([&] {
        return script->entered.available() == 1 &&
               desktopContains(window->contentItem(), "p606_delta");
    }));
    script->entered.acquire();
    P606_CHECK(activate("chatSendButton"));
    P606_CHECK(wait([&] { return vm.chatSendLifecycleState() == "cancelled"; }));
    P606_CHECK(wait([&] { return desktopContains(window->contentItem(), "Response cancelled."); }));
    P606_CHECK(capture("cancelled-wide"));
    stored = persisted.loadMessages(cancelSession);
    P606_CHECK(!stored.isEmpty() && stored.last().status == core::ChatMessageStatus::Cancelled);
    P606_CHECK(persisted.loadMessages(firstSession).last().content == "p606_delta completed");
    record("chat-cancel",
           {{"state", "cancelled"}, {"other_session_unchanged", true}, {"visible_notice", true}});
    P606_CHECK(activate("chatModeAgent"));
    P606_CHECK(vm.currentModeName() == "Agent");
    script->decisions = 0;
    P606_CHECK(type("update allowed.txt and read back its content"));
    QTest::keyClick(window, Qt::Key_Return);
    P606_CHECK(
        wait([&] { return !runtime.pendingApproval().isEmpty() && item("daemonApprovalDeny"); }));
    QTest::qWait(100);
    auto pending = runtime.pendingApproval();
    P606_CHECK(state().phase == core::AgentLoopPhase::AwaitingApproval);
    P606_CHECK(wait([&] {
        return !pending.value("approval_id").toString().isEmpty() &&
               approvalRun(pending.value("approval_id")) == runtime.runId();
    }));
    // Event envelopes carry run_id; the nested snapshot approval intentionally does not.
    if (pending.contains("run_id")) {
        P606_CHECK(pending.value("run_id").toString() == runtime.runId());
    }
    P606_CHECK(desktopBytes(script->path) == "p606 original bytes\n");
    P606_CHECK(item("daemonApprovalDeny")->hasActiveFocus());
    auto* accessibleDeny = QAccessible::queryAccessibleInterface(item("daemonApprovalDeny"));
    P606_CHECK(accessibleDeny && accessibleDeny->role() == QAccessible::Button &&
               accessibleDeny->text(QAccessible::Name) == "Deny");
    auto* deny = item("daemonApprovalDeny");
    auto* modalContent = deny->parentItem();
    for (int i = 0; i < 7; ++i) {
        QTest::keyClick(window, Qt::Key_Tab);
        P606_CHECK(modalContent->isAncestorOf(window->activeFocusItem()));
    }
    QTest::keyClick(window, Qt::Key_Escape);
    P606_CHECK(runtime.pendingApproval().value("approval_id") == pending.value("approval_id"));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
    P606_CHECK(modalContent->isAncestorOf(window->activeFocusItem()));
    for (int i = 0; i < 3 && !deny->hasActiveFocus(); ++i) {
        QTest::keyClick(window, Qt::Key_Tab);
    }
    P606_CHECK(deny->hasActiveFocus());
    P606_CHECK(capture("approval-deny"));
    record("approval-focus", {{"correct_run", true}, {"default", "deny"}});
    QTest::keyClick(window, Qt::Key_Return);
    P606_CHECK(wait([&] {
        return state().phase == core::AgentLoopPhase::Failed && runtime.pendingApproval().isEmpty();
    }));
    P606_CHECK(desktopBytes(script->path) == "p606 original bytes\n");
    P606_CHECK(controller.persistentPermissionGrants().isEmpty());
    P606_CHECK(desktopSuccessfulWrites(controller.activeAgentSessionId()) == 0);
    P606_CHECK(!item("daemonApprovalDeny"));
    P606_CHECK(wait([&] { return item("chatPromptInput")->hasActiveFocus(); }));
    record("approval-deny", {{"mutation", false}, {"focus_restored", true}});
    script->decisions = 0;
    P606_CHECK(type("update allowed.txt and read back its content again"));
    QTest::keyClick(window, Qt::Key_Return);
    P606_CHECK(
        wait([&] { return !runtime.pendingApproval().isEmpty() && item("daemonApprovalDeny"); }));
    QTest::qWait(100);
    P606_CHECK(runtime.pendingApproval().value("approval_id") != pending.value("approval_id"));
    P606_CHECK(item("daemonApprovalDeny")->hasActiveFocus());
    P606_CHECK(desktopBytes(script->path) == "p606 original bytes\n");
    QTest::keyClick(window, Qt::Key_Tab);
    P606_CHECK(item("daemonApprovalAllow")->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Return);
    P606_CHECK(wait([&] {
        return state().phase == core::AgentLoopPhase::Completed &&
               runtime.pendingApproval().isEmpty();
    }));
    P606_CHECK(script->sawObservation.load());
    P606_CHECK(desktopBytes(script->path) == "p606 authorized bytes\n");
    P606_CHECK(controller.persistentPermissionGrants().isEmpty());
    P606_CHECK(state().steps.size() == 2);
    P606_CHECK(desktopSuccessfulWrites(controller.activeAgentSessionId()) == 1);
    const auto final = state().finalAnswer;
    P606_CHECK(wait([&] {
        auto m = persisted.loadMessages(cancelSession);
        return !m.isEmpty() && m.last().content == final;
    }));
    record("approval-allow",
           {{"sha256", QString::fromLatin1(QCryptographicHash::hash(desktopBytes(script->path),
                                                                    QCryptographicHash::Sha256)
                                               .toHex())},
            {"steps", state().steps.size()},
            {"successful_writes", 1}});
    transport.disconnectFromDaemon();
    P606_CHECK(wait([&] { return !vm.daemonConnected() && !runtime.ready(); }));
    transport.connectToDaemon();
    P606_CHECK(wait([&] { return vm.daemonConnected() && runtime.ready(); }));
    P606_CHECK(runtime.sessionId() == cancelSession);
    P606_CHECK(desktopBytes(script->path) == "p606 authorized bytes\n");
    record("reconnect", {{"session_matches", true}});
    QString completedRun;
    for (const auto& event :
         controller.agentRuntime()->eventHistory(controller.activeAgentSessionId())) {
        if (event.type == core::AgentEventType::AgentCompleted) {
            completedRun = event.turnId;
        }
    }
    P606_CHECK(!completedRun.isEmpty());
    P606_CHECK(activate("InspectorNavigationButton"));
    P606_CHECK(wait([&] { return inspector.runs()->rowCount() >= 2; }));
    auto* row = item("inspectorRun-" + completedRun);
    P606_CHECK(row);
    row->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Space);
    P606_CHECK(wait([&] {
        return inspector.selectedRun().value("state").toString() == "Completed" &&
               item("inspectorRunState");
    }));
    P606_CHECK(inspector.selectedRunId() == completedRun);
    P606_CHECK(item("inspectorRunState")->property("text").toString().contains("Completed"));
    P606_CHECK(capture("inspector-wide"));
    record("inspector",
           {{"state", inspector.selectedRun().value("state").toString()}, {"correct_run", true}});
    window->setWidth(780);
    auto larger = QApplication::font();
    larger.setPointSize(20);
    QApplication::setFont(larger);
    P606_CHECK(capture("inspector-compact-large-font"));
    record("layouts",
           {{"compact_width", 780}, {"wide_width", 1320}, {"application_font_points", 20}});
    P606_CHECK(activate("DashboardNavigationButton"));
    window->setWidth(1320);
    QTest::qWait(50);
    auto conversationExists = [&] {
        for (const auto& c : persisted.listConversations()) {
            if (c.id == cancelSession) {
                return true;
            }
        }
        return false;
    };
    P606_CHECK(conversationExists());
    if (item("chatSidebarToggle")) {
        P606_CHECK(activate("chatSidebarToggle"));
    }
    P606_CHECK(wait([&] { return item("chatConversationMenu-" + cancelSession) != nullptr; }));
    P606_CHECK(activate("chatConversationMenu-" + cancelSession));
    P606_CHECK(activate("chatDeleteConversation"));
    P606_CHECK(wait([&] { return item("chatDeleteCancel") && item("chatDeleteConfirm"); }));
    QTest::keyClick(window, Qt::Key_Return);
    P606_CHECK(conversationExists());
    P606_CHECK(capture("delete-confirmation"));
    P606_CHECK(activate("chatDeleteCancel"));
    P606_CHECK(conversationExists());
    P606_CHECK(activate("chatConversationMenu-" + cancelSession));
    P606_CHECK(activate("chatDeleteConversation"));
    P606_CHECK(activate("chatDeleteConfirm"));
    P606_CHECK(wait([&] { return !conversationExists(); }));
    P606_CHECK(persisted.loadMessages(cancelSession).isEmpty());
    P606_CHECK(persisted.loadMessages(firstSession).last().content == "p606_delta completed");
    record("delete-confirmation",
           {{"deliberate_activation", true}, {"other_session_unchanged", true}, {"deleted", true}});
    P606_CHECK(!script->timedOut.load());
#undef P606_CHECK
    return 0;
}
} // namespace sentinel::test
