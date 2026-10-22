// SPDX-License-Identifier: GPL-3.0-or-later
#include "../support/DeterministicChatFixture.h"
#include "sentinel/core/agent/AgentRuntime.h"
#include "sentinel/core/agent/IAgentStepPlanner.h"
#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/memory/InMemoryStore.h"
#include "sentinel/core/runtime/RealToolExecutor.h"
#include "sentinel/core/security/StaticSandboxPolicy.h"
#include "service/DaemonIpcServer.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "../support/ShortSocketPath.h"
#include <QTest>
#include <QUuid>
#ifdef Q_OS_UNIX
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
using namespace sentinel;
using sentinel::test::shortSocketDirTemplate;
namespace {
QJsonObject message(const QString& name, const QJsonObject& payload = {}, int major = 1) {
    return {{"version", QJsonObject{{"major", major}, {"minor", 0}}},
            {"type", "request"},
            {"id", "test-id"},
            {"name", name},
            {"payload", payload}};
}
QJsonObject receive(QLocalSocket& socket, int timeoutMs = 2000) {
    QElapsedTimer timer;
    timer.start();
    while (!socket.canReadLine() && timer.elapsed() < timeoutMs)
        QTest::qWait(1);
    return QJsonDocument::fromJson(socket.readLine()).object();
}
QJsonObject request(QLocalSocket& socket, const QString& name, QJsonObject payload = {},
                    int major = 1, int timeoutMs = 2000) {
    socket.write(QJsonDocument(message(name, payload, major)).toJson(QJsonDocument::Compact) +
                 '\n');
    socket.flush();
    return receive(socket, timeoutMs);
}
void hello(QLocalSocket& socket, const QString& path) {
    socket.connectToServer(path);
    QVERIFY(socket.waitForConnected(1000));
    QCOMPARE(
        request(socket, "hello",
                {{"client_id", "test"}, {"major", 1}, {"minor", 0}, {"capabilities", QJsonArray{}}})
            .value("type")
            .toString(),
        QString("response"));
}
class ApprovalPlanner final : public core::IAgentStepPlanner {
public:
    core::AgentStepDecision nextStep(const QString&,
                                     const QList<core::AgentStepRecord>& history) const override {
        core::AgentStepDecision step;
        if (history.isEmpty()) {
            step.kind = core::AgentStepDecision::Kind::ToolCall;
            step.toolId = "run-command";
            step.toolName = "run-command";
            step.riskLevel = core::ToolRiskLevel::High;
            step.arguments.append({QStringLiteral("command"), QStringLiteral("pwd")});
        } else {
            step.kind = core::AgentStepDecision::Kind::FinalAnswer;
            step.answer = "Fixture finished";
        }
        return step;
    }
};
class FixtureExecutor final : public core::IToolExecutor {
public:
    core::ToolExecutionResult execute(const core::ToolExecutionRequest&) const override {
        return {core::ToolExecutionStatus::Succeeded, QStringLiteral("Fixture result")};
    }
};
class LargeCatalog final : public core::IModelLibrarySourceAdapter {
public:
    mutable int reads = 0;
    QList<core::ModelLibraryEntry> rows;
    QString sourceId() const override {
        return "test-large-catalog";
    }
    QString providerId() const override {
        return {};
    }
    QList<core::ModelLibraryEntry> entries() const override {
        ++reads;
        return rows;
    }
};
class ShutdownGatePlanner final : public core::IAgentStepPlanner {
public:
    QSemaphore& entered;
    QSemaphore& release;
    ShutdownGatePlanner(QSemaphore& enteredSignal, QSemaphore& releaseSignal)
        : entered(enteredSignal), release(releaseSignal) {}
    core::AgentStepDecision nextStep(const QString&,
                                     const QList<core::AgentStepRecord>&) const override {
        entered.release();
        release.tryAcquire(1, 5000);
        core::AgentStepDecision decision;
        decision.kind = core::AgentStepDecision::Kind::FinalAnswer;
        decision.answer = QStringLiteral("late provider answer");
        return decision;
    }
};
class BackpressurePlanner final : public core::IAgentStepPlanner {
public:
    int totalSteps;
    explicit BackpressurePlanner(int steps = 8) : totalSteps(steps) {}
    core::AgentStepDecision nextStep(const QString&,
                                     const QList<core::AgentStepRecord>& history) const override {
        core::AgentStepDecision step;
        if (history.size() < totalSteps) {
            step.kind = core::AgentStepDecision::Kind::ToolCall;
            step.toolId = "memory-search";
            step.toolName = "memory-search";
            step.riskLevel = core::ToolRiskLevel::Low;
            step.arguments.append(
                {QStringLiteral("query"),
                 QString::number(history.size()) + QStringLiteral("_") + QString(160000, 'P')});
        } else {
            step.kind = core::AgentStepDecision::Kind::FinalAnswer;
            step.answer = "Backpressure test finished";
        }
        return step;
    }
};
struct Harness {
    QTemporaryDir directory{shortSocketDirTemplate("sentinel-ipc")};
    test::DeterministicModelServiceFixture models;
    std::unique_ptr<core::ApplicationController> controller;
    std::unique_ptr<daemon::DaemonIpcServer> server;
    QString path;
    explicit Harness(test::DeterministicChatReply reply = test::DeterministicChatReply::Final,
                     bool agent = false, core::AppSettings* settings = nullptr,
                     bool realFiles = false, std::unique_ptr<core::IAgentStepPlanner> planner = {})
        : models(reply, settings) {
        core::ApplicationControllerBuilder builder;
        if (agent || realFiles) {
            builder
                .withAgentRuntime(std::make_unique<core::NullAgentRuntime>(
                    core::NullAgentRuntime::standardTools()))
                .withAgentStepPlanner(planner ? std::move(planner)
                                              : std::make_unique<ApprovalPlanner>())
                .withToolExecutor(realFiles ? std::unique_ptr<core::IToolExecutor>(
                                                  std::make_unique<core::RealToolExecutor>())
                                            : std::unique_ptr<core::IToolExecutor>(
                                                  std::make_unique<FixtureExecutor>()))
                .withSandboxPolicy(std::make_unique<core::StaticSandboxPolicy>());
        }
        controller = builder.withModelService(models.takeModelService())
                         .withMemoryStore(std::make_unique<core::InMemoryStore>())
                         .build();
        server = std::make_unique<daemon::DaemonIpcServer>(controller.get());
        path = directory.filePath("daemon.sock");
    }
};
} // namespace
class DaemonIpcServerTest : public QObject {
    Q_OBJECT
private slots:
    void shutdownAckDoesNotWaitForUnresponsivePlanner() {
        // WIN-02: Daemon IPC requires Unix peer-credential authentication (SO_PEERCRED).
        // Windows named-pipe peer-process authentication is not implemented (PD-DWIN-01).
        // All connections are rejected fail-closed on Windows; this test cannot succeed.
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        QSemaphore entered, release;
        Harness h(test::DeterministicChatReply::Final, true, nullptr, false,
                  std::make_unique<ShutdownGatePlanner>(entered, release));
        auto cleanup = qScopeGuard([&] { release.release(); });
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket client;
        hello(client, h.path);
        auto* runtime = h.controller->agentRuntime();
        const auto session = runtime->createSession();
        QVERIFY(runtime->start(session, QStringLiteral("task")));
        QVERIFY(entered.tryAcquire(1, 1000));
        QSignalSpy drained(h.server.get(), &daemon::DaemonIpcServer::shutdownDrained);
        const auto response = request(client, "daemon.shutdown");
        QVERIFY(response.value("payload").toObject().value("shutting_down").toBool());
        QCOMPARE(drained.size(), 0);
        QVERIFY(!runtime->tryShutdown(0));
        h.server->requestShutdown();
        release.release();
        QTRY_COMPARE(drained.size(), 1);
        QCOMPARE(runtime->sessionState(session).phase, core::AgentLoopPhase::Cancelled);
    }

    void shutdownAcknowledgesAndDrainsExactlyOnce() {
        // WIN-02: Requires Unix peer-credential authentication. See PD-DWIN-01.
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket client;
        hello(client, h.path);
        QSignalSpy drained(h.server.get(), &daemon::DaemonIpcServer::shutdownDrained);
        const auto response = request(client, "daemon.shutdown");
        QCOMPARE(response.value("type").toString(), QStringLiteral("response"));
        QVERIFY(response.value("payload").toObject().value("shutting_down").toBool());
        h.server->requestShutdown();
        QVERIFY(!h.server->startServer(h.path));
        QTRY_COMPARE(drained.size(), 1);
        h.server->requestShutdown();
        QCOMPARE(drained.size(), 1);
        const auto rejected = request(client, "daemon.status");
        QCOMPARE(rejected.value("payload").toObject().value("code").toString(),
                 QStringLiteral("daemon-shutting-down"));
    }

    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName("sentinel-phase2-ipc-" +
                                             QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    void cleanupTestCase() {
        for (const auto location :
             {QStandardPaths::AppDataLocation, QStandardPaths::AppConfigLocation}) {
            const auto path = QStandardPaths::writableLocation(location);
            if (QDir(path).exists())
                QVERIFY(QDir(path).removeRecursively());
        }
    }
    void modelPagesFreezeSnapshotAndDoNotRebuildCatalog() {
        // WIN-02: Requires Unix peer-credential authentication. See PD-DWIN-01.
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        LargeCatalog catalog;
        Harness h;
        for (int i = 0; i < 500; ++i) {
            core::ModelLibraryEntry row;
            row.id = QString("fixture-%1").arg(i);
            row.artifactId = row.id;
            row.repositoryId = row.id;
            row.displayName = QString(1000, QLatin1Char('x'));
            row.format = "GGUF";
            row.source.id = "hugging-face";
            catalog.rows.append(row);
        }
        h.controller->modelLibrary()->addSourceAdapter(&catalog);
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket socket;
        hello(socket, h.path);
        auto page = request(socket, "model.helper_state",
                            {{"component", "ggufLibraryFetcher"}, {"offset", 0}})
                        .value("payload")
                        .toObject()
                        .value("properties")
                        .toObject();
        QTRY_VERIFY_WITH_TIMEOUT(
            ([&] {
                page = request(socket, "model.helper_state",
                               {{"component", "ggufLibraryFetcher"}, {"offset", 0}})
                           .value("payload")
                           .toObject()
                           .value("properties")
                           .toObject();
                return page.value("totalModels").toInt() == 500;
            })(),
            5000);
        QCOMPARE(page.value("totalModels").toInt(), 500);
        QVERIFY(page.value("nextOffset").toInt() > 0);
        const auto reads = catalog.reads;
        catalog.rows.clear(); // Background discovery changes while this client transfers pages.
        int count = page.value("models").toArray().size();
        while (page.value("nextOffset").toInt() >= 0) {
            QCOMPARE(request(socket, "daemon.status").value("type").toString(),
                     QString("response"));
            page =
                request(socket, "model.helper_state",
                        {{"component", "ggufLibraryFetcher"}, {"offset", page.value("nextOffset")}})
                    .value("payload")
                    .toObject()
                    .value("properties")
                    .toObject();
            count += page.value("models").toArray().size();
            QCOMPARE(page.value("totalModels").toInt(), 500);
            QCOMPARE(catalog.reads, reads);
        }
        QCOMPARE(count, 500);
        const auto retained =
            request(socket, "model.helper_state",
                    {{"component", "ggufLibraryFetcher"}, {"offset", 0}, {"retain_models", true}})
                .value("payload")
                .toObject()
                .value("properties")
                .toObject();
        QVERIFY(retained.value("modelsUnchanged").toBool());
        QVERIFY(retained.value("models").toArray().isEmpty());
        QCOMPARE(retained.value("totalModels").toInt(), 500);
        QCOMPARE(catalog.reads, reads);
        request(socket, "model.helper_action",
                {{"component", "ggufLibraryFetcher"}, {"action", "cancel"}, {"value", ""}});
        QTRY_VERIFY_WITH_TIMEOUT(
            ([&] {
                const auto state = request(socket, "model.helper_state",
                                           {{"component", "ggufLibraryFetcher"}, {"offset", 0}})
                                       .value("payload")
                                       .toObject()
                                       .value("properties")
                                       .toObject();
                return state.value("totalModels").toInt() == 0 && !state.value("fetching").toBool();
            })(),
            5000);
    }
    void deletesInactiveConversationWhileResponseIsRunning() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Delayed);
        auto gate = std::make_shared<QSemaphore>();
        h.models.state->completionGate = gate;
        auto release = qScopeGuard([gate] { gate->release(); });
        const auto inactive = h.controller->createConversation("Inactive");
        const auto active = h.controller->createConversation("Active");
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket chat, control;
        hello(chat, h.path);
        hello(control, h.path);
        QCOMPARE(request(chat, "chat.send", {{"session_id", active}, {"text", "Hold response"}})
                     .value("type")
                     .toString(),
                 QString("response"));
        const auto deleted = request(control, "desktop.action",
                                     {{"action", "requestPermanentDeleteConversation"},
                                      {"session_id", active},
                                      {"arguments", QJsonArray{inactive}}});
        QVERIFY(deleted.value("payload").toObject().value("accepted").toBool());
        QCOMPARE(h.controller->activeConversationId(), active);
        const auto refused = request(control, "desktop.action",
                                     {{"action", "requestPermanentDeleteConversation"},
                                      {"session_id", active},
                                      {"arguments", QJsonArray{active}}});
        QCOMPARE(refused.value("payload").toObject().value("code").toString(),
                 QString("runtime-busy"));
    }
    void respondsToPing() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        QCOMPARE(request(s, "daemon.status")["payload"].toObject()["running"].toBool(), true);
    }
    void respondsToStatusWithControllerData() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        QCOMPARE(request(s, "model.current")["payload"].toObject()["model_id"].toString(),
                 QString("sentinel-test-model"));
    }
    void terminalDiagnosticsAreSafeAndAuthoritative() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        core::AppSettings settings(std::make_unique<core::InMemorySettingsStore>(),
                                   core::inMemoryTestCredentialStore());
        Harness h(test::DeterministicChatReply::Final, false, &settings);
        settings.setOpenAiApiKey("fixture-secret-never-print");
        const auto cached = h.controller->modelService()->providerStatusSnapshot("openai");
        QCOMPARE(cached.catalog, core::ProviderCatalogState::Unverified);
        h.server->setSettings(&settings);
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket socket;
        hello(socket, h.path);
        const auto reply = request(socket, "terminal.state", {}, 1, 10000);
        QCOMPARE(reply["type"].toString(), QString("response"));
        const auto properties = reply["payload"].toObject()["properties"].toObject();
        QVERIFY(properties.contains("permission_service_available"));
        QVERIFY(properties.contains("availableToolIds"));
        QVERIFY(!properties.contains("contextReasoningDeveloperTraces"));
        QVERIFY(!properties.contains("apiKey"));
        QVERIFY(reply["payload"].toObject()["providers"].isArray());
        QVERIFY(!QJsonDocument(reply).toJson().contains("fixture-secret-never-print"));
        const QJsonValue sid =
            request(socket, "session.create", {{"title", "safe attach"}})["payload"]
                .toObject()
                .value("session_id");
        const auto attached = request(socket, "terminal.attach", {{"session_id", sid}});
        QCOMPARE(attached["type"].toString(), QString("response"));
        QVERIFY(!attached["payload"].toObject()["properties"].toObject().contains(
            "contextReasoningDeveloperTraces"));
        QVERIFY(!QJsonDocument(attached).toJson().contains("fixture-secret-never-print"));
        QCOMPARE(properties["activeRuntimeModelLabel"].toString(), QString("sentinel-test-model"));
    }
    void workspaceSelectionAndRootAreValidated() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        core::AppSettings settings(std::make_unique<core::InMemorySettingsStore>(),
                                   core::inMemoryTestCredentialStore());
        Harness h(test::DeterministicChatReply::Final, false, &settings);
        h.server->setSettings(&settings);
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket socket;
        hello(socket, h.path);
        QCOMPARE(request(socket, "workspace.select", {{"workspace_id", "missing"}})["payload"]
                     .toObject()["code"]
                     .toString(),
                 QString("unknown-workspace"));
        QCOMPARE(request(socket, "workspace.select", {{"workspace_id", "coding"}})["payload"]
                     .toObject()["workspace_id"]
                     .toString(),
                 QString("coding"));
        QCOMPARE(settings.selectedWorkspaceId(), QString("coding"));
        QCOMPARE(request(socket, "workspace.root",
                         {{"workspace_id", "coding"}, {"path", h.directory.path()}})["payload"]
                     .toObject()["code"]
                     .toString(),
                 QString("workspace-root-rejected"));
        const QJsonValue created =
            request(socket, "workspace.create",
                    {{"name", "Terminal fixture"}, {"template", "Coding"}})["payload"]
                .toObject()
                .value("workspace_id");
        QVERIFY(!created.toString().isEmpty());
        QCOMPARE(request(socket, "workspace.root",
                         {{"workspace_id", created}, {"path", h.directory.path()}})["type"]
                     .toString(),
                 QString("response"));
        QCOMPARE(
            request(socket, "workspace.root",
                    {{"workspace_id", "coding"}, {"path", "/nonexistent/sentinel-test"}})["payload"]
                .toObject()["code"]
                .toString(),
            QString("workspace-root-rejected"));
    }
    void terminalFilesRespectWorkspaceAndToolBoundaries() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        core::AppSettings settings(std::make_unique<core::InMemorySettingsStore>(),
                                   core::inMemoryTestCredentialStore());
        Harness h(test::DeterministicChatReply::Final, false, &settings, true);
        core::WorkspaceService workspaces;
        const auto created = workspaces.createWorkspace({}, "File fixture", "Coding");
        const auto rooted = workspaces.setWorkspaceRoot(
            created.catalogJson, created.selectedWorkspaceId, h.directory.path());
        QVERIFY(rooted.success);
        settings.setWorkspaceCatalogJson(rooted.catalogJson);
        settings.setSelectedWorkspaceId(created.selectedWorkspaceId);
        h.controller->attachControlledTaskSettings(settings);
        h.server->setSettings(&settings);
        QFile greeting(h.directory.filePath("greeting.txt"));
        QVERIFY(greeting.open(QIODevice::WriteOnly));
        greeting.write("hello\n");
        greeting.close();
        QFile secret(h.directory.filePath(".env"));
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("PRIVATE_FIXTURE=hidden\n");
        secret.close();
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket socket;
        hello(socket, h.path);
        const QJsonValue sid =
            request(socket, "session.create", {{"title", "files"}})["payload"].toObject().value(
                "session_id");
        auto* runtime = dynamic_cast<core::AgentRuntime*>(h.controller->agentRuntime());
        QVERIFY(runtime);
        const auto inspected = runtime->inspectWorkspace(sid.toString(), h.directory.path(), "glob",
                                                         h.directory.path());
        QVERIFY2(inspected.status == core::ToolExecutionStatus::Succeeded,
                 qPrintable(inspected.summary));
        const auto listing = request(socket, "workspace.files", {{"session_id", sid}}, 1, 10000);
        QCOMPARE(listing["type"].toString(), QString("response"));
        const auto files = listing["payload"].toObject()["files"].toArray();
        QVERIFY(files.contains(QFileInfo(greeting).canonicalFilePath()));
        QVERIFY(!files.contains(QFileInfo(secret).canonicalFilePath()));
        QCOMPARE(runtime
                     ->inspectWorkspace(sid.toString(), h.directory.path(), "read-file",
                                        secret.fileName())
                     .status,
                 core::ToolExecutionStatus::Blocked);
        QCOMPARE(runtime
                     ->inspectWorkspace(sid.toString(), h.directory.path(), "write-file",
                                        greeting.fileName())
                     .status,
                 core::ToolExecutionStatus::Blocked);
        QTemporaryDir outside{shortSocketDirTemplate("profile")};
        QVERIFY(outside.isValid());
        QCOMPARE(
            runtime->inspectWorkspace(sid.toString(), h.directory.path(), "glob", outside.path())
                .status,
            core::ToolExecutionStatus::Blocked);
        settings.setWorkspaceProfilesJson(workspaces.updateProfile(
            {}, created.selectedWorkspaceId, {}, {{"tools", QJsonObject{{"glob", false}}}}));
        QCOMPARE(request(socket, "workspace.files", {{"session_id", sid}})["payload"]
                     .toObject()["code"]
                     .toString(),
                 QString("permission-denied"));
    }
    void activeBindingCannotBeSwitched() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Delayed);
        h.models.state->delayMs = 300;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket stream, control;
        hello(stream, h.path);
        hello(control, h.path);
        const QJsonValue sid =
            request(stream, "session.create", {{"title", "binding"}})["payload"].toObject().value(
                "session_id");
        QCOMPARE(request(stream, "chat.send", {{"session_id", sid}, {"text", "hello"}})["type"]
                     .toString(),
                 QString("response"));
        QCOMPARE(request(control, "model.select",
                         {{"provider_id", "anything"}, {"model_id", "anything"}})["payload"]
                     .toObject()["code"]
                     .toString(),
                 QString("runtime-busy"));
        h.controller->stopChatGeneration();
        QTest::qWait(350);
    }
    void rejectsUnknownCommand() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        QCOMPARE(request(s, "bogus")["payload"].toObject()["code"].toString(),
                 QString("unknown-command"));
    }
    void incompatibleMajor() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        s.connectToServer(h.path);
        QVERIFY(s.waitForConnected());
        QCOMPARE(
            request(s, "hello", {{"client_id", "test"}, {"major", 2}, {"minor", 0}}, 2)["payload"]
                .toObject()["code"]
                .toString(),
            QString("incompatible-major"));
    }
    void handshakeRequired() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        s.connectToServer(h.path);
        QVERIFY(s.waitForConnected());
        QCOMPARE(request(s, "daemon.status")["payload"].toObject()["code"].toString(),
                 QString("handshake-required"));
    }
    void multipleClientsAndReconnect() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket a, b;
        hello(a, h.path);
        hello(b, h.path);
        const auto created =
            request(a, "session.create", {{"title", "shared"}})["payload"].toObject();
        const auto attached = request(b, "session.attach", {{"session_id", created["session_id"]}});
        QCOMPARE(attached["type"].toString(), QString("response"));
        a.abort();
        QLocalSocket c;
        hello(c, h.path);
        QCOMPARE(request(c, "session.attach", {{"session_id", created["session_id"]}})["payload"]
                     .toObject()["session_id"],
                 created["session_id"]);
    }
    void collisionDoesNotUnlinkOwner() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        daemon::DaemonIpcServer second;
        QVERIFY(!second.startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        QCOMPARE(request(s, "daemon.status")["type"].toString(), QString("response"));
    }
    void malformedAndInvalidPayload() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        s.write("{bad}\n");
        s.flush();
        QCOMPARE(receive(s)["type"].toString(), QString("error"));
        QCOMPARE(request(s, "approval.respond",
                         {{"run_id", "x"}, {"approval_id", "x"}, {"allow", "yes"}})["payload"]
                     .toObject()["code"]
                     .toString(),
                 QString("invalid-payload"));
    }
    void framingAcrossChunks() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        const auto bytes = QJsonDocument(message("daemon.status")).toJson(QJsonDocument::Compact);
        s.write(bytes.left(10));
        s.flush();
        QTest::qWait(10);
        QVERIFY(!s.canReadLine());
        s.write(bytes.mid(10) + '\n');
        s.flush();
        QCOMPARE(receive(s)["id"].toString(), QString("test-id"));
    }
    void oversizedFrame() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        s.write(QByteArray(262145, 'a'));
        s.flush();
        QCOMPARE(receive(s)["payload"].toObject()["code"].toString(), QString("message-too-large"));
    }
    void rejectsUnknownSessionAndApprovalReplay() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        QCOMPARE(request(s, "session.attach", {{"session_id", "missing"}})["payload"]
                     .toObject()["code"]
                     .toString(),
                 QString("unknown-session"));
        QCOMPARE(request(s, "approval.respond",
                         {{"run_id", "old"}, {"approval_id", "old"}, {"allow", true}})["payload"]
                     .toObject()["code"]
                     .toString(),
                 QString("invalid-approval"));
    }
    void deterministicChatEvents() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Streaming);
        const auto dbPath = h.directory.filePath("stream.sqlite3");
        QVERIFY(h.server->openSessionProjectionStore(dbPath));
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        auto created = request(s, "session.create", {{"title", "chat"}})["payload"].toObject();
        auto started =
            request(s, "chat.send", {{"session_id", created["session_id"]}, {"text", "hello"}});
        QCOMPARE(started["type"].toString(), QString("response"));
        QString output;
        bool completed = false;
        for (int i = 0; i < 8 && !completed; ++i) {
            auto e = receive(s);
            if (e["name"] == "output.delta")
                output += e["payload"].toObject()["text"].toString();
            completed = e["name"] == "run.completed";
        }
        QVERIFY(completed);
        QCOMPARE(output, QString("SENTINEL_TEST_RESPONSE"));
        s.abort();
        h.server->stopServer();
        h.server.reset();
        daemon::DaemonIpcServer recovered(h.controller.get());
        QVERIFY(recovered.openSessionProjectionStore(dbPath));
        QVERIFY(recovered.startServer(h.path));
        QLocalSocket fresh;
        hello(fresh, h.path);
        const auto snapshot =
            request(fresh, "session.attach", {{"session_id", created["session_id"]}})["payload"]
                .toObject();
        QCOMPARE(snapshot["state"].toString(), QString("completed"));
        QCOMPARE(snapshot["output"].toString(), output);
    }
    void cancellationHasNoLateCompletion() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Delayed);
        h.models.state->delayMs = 300;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s, c;
        hello(s, h.path);
        hello(c, h.path);
        auto created = request(s, "session.create", {{"title", "cancel"}})["payload"].toObject();
        auto started =
            request(s, "chat.send",
                    {{"session_id", created["session_id"]}, {"text", "hello"}})["payload"]
                .toObject();
        QCOMPARE(request(c, "run.cancel", {{"run_id", started["run_id"]}})["type"].toString(),
                 QString("response"));
        bool cancelled = false;
        for (int i = 0; i < 8 && !cancelled; ++i) {
            auto e = receive(s);
            QVERIFY(e["name"] != "run.completed");
            cancelled = e["name"] == "run.cancelled";
        }
        QVERIFY(cancelled);
        QTest::qWait(400);
        QVERIFY(!s.canReadLine());
    }
    void agentApprovalRoundTrip() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Final, true);
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s, control;
        hello(s, h.path);
        hello(control, h.path);
        const auto created =
            request(s, "session.create", {{"title", "Agent"}})["payload"].toObject();
        QCOMPARE(
            request(s, "agent.start",
                    {{"session_id", created["session_id"]}, {"text", "perform action"}})["type"]
                .toString(),
            QString("response"));
        QJsonObject approval;
        for (int i = 0; i < 10 && approval.isEmpty(); ++i) {
            const auto e = receive(s);
            if (e["name"] == "approval.requested")
                approval = e["payload"].toObject();
        }
        QVERIFY(!approval.isEmpty());
        QCOMPARE(approval["state"].toString(), QString("approval"));
        QCOMPARE(request(control, "approval.respond",
                         {{"run_id", approval["run_id"]},
                          {"approval_id", approval["approval_id"]},
                          {"allow", true}})["type"]
                     .toString(),
                 QString("response"));
        QCOMPARE(request(control, "approval.respond",
                         {{"run_id", approval["run_id"]},
                          {"approval_id", approval["approval_id"]},
                          {"allow", true}})["payload"]
                     .toObject()["code"]
                     .toString(),
                 QString("invalid-approval"));
        bool completed = false;
        QElapsedTimer completionTimer;
        completionTimer.start();
        while (!completed && completionTimer.elapsed() < 5000) {
            const auto e = receive(s, 100);
            completed = e["name"] == "run.completed";
            if (completed)
                QCOMPARE(e["payload"].toObject()["text"].toString(), QString("Fixture finished"));
        }
        QVERIFY(completed);
    }
    void disconnectDoesNotCancelChat() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Delayed);
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        const auto id = request(s, "session.create", {{"title", "Reconnect"}})["payload"]
                            .toObject()["session_id"]
                            .toString();
        QCOMPARE(
            request(s, "chat.send", {{"session_id", id}, {"text", "hello"}})["type"].toString(),
            QString("response"));
        s.abort();
        QTest::qWait(500);
        QLocalSocket reconnected;
        hello(reconnected, h.path);
        const auto snapshot =
            request(reconnected, "session.attach", {{"session_id", id}})["payload"].toObject();
        QCOMPARE(snapshot["state"].toString(), QString("completed"));
        QVERIFY(!snapshot["output"].toString().isEmpty());
    }
    void cancelledProjectionCannotLeakIntoNewConversation() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Final, true);
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        const auto id = request(s, "session.create", {{"title", "old"}})["payload"]
                            .toObject()["session_id"]
                            .toString();
        request(s, "agent.start", {{"session_id", id}, {"text", "perform action"}});
        QTRY_VERIFY_WITH_TIMEOUT(h.controller->agentAwaitingApproval(), 5000);
        QString next;
        const auto subscription =
            h.controller->agentRuntime()->subscribe([&](const core::AgentEvent& event) {
                if (event.type == core::AgentEventType::AgentCancelled)
                    QMetaObject::invokeMethod(
                        h.controller.get(),
                        [&] { next = h.controller->createConversation(QStringLiteral("new")); },
                        Qt::QueuedConnection);
            });
        QVERIFY(h.controller->cancelAgentRun());
        QTRY_VERIFY_WITH_TIMEOUT(!next.isEmpty(), 5000);
        QTest::qWait(20);
        h.controller->agentRuntime()->unsubscribe(subscription);
        QCOMPARE(h.controller->activeConversationId(), next);
        for (const auto& message : h.controller->chatHistory())
            QVERIFY2(!message.content.contains(QStringLiteral("Agent Run Cancelled")),
                     qPrintable(message.content));
    }
    void agentCancellationDuringApproval() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Final, true);
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s, control;
        hello(s, h.path);
        hello(control, h.path);
        const auto id = request(s, "session.create", {{"title", "Cancel agent"}})["payload"]
                            .toObject()["session_id"]
                            .toString();
        request(s, "agent.start", {{"session_id", id}, {"text", "perform action"}});
        QJsonObject approval;
        for (int i = 0; i < 10 && approval.isEmpty(); ++i) {
            const auto e = receive(s);
            if (e["name"] == "approval.requested")
                approval = e["payload"].toObject();
        }
        QVERIFY(!approval.isEmpty());
        QCOMPARE(approval["state"].toString(), QString("approval"));
        QCOMPARE(
            request(control, "run.cancel", {{"run_id", approval["run_id"]}})["type"].toString(),
            QString("response"));
        bool cancelled = false;
        for (int i = 0; i < 10 && !cancelled; ++i) {
            const auto e = receive(s);
            QVERIFY(e["name"] != "run.completed");
            cancelled = e["name"] == "run.cancelled";
        }
        QVERIFY(cancelled);
        QTest::qWait(100);
        QVERIFY(!s.canReadLine());
        QCOMPARE(request(control, "approval.respond",
                         {{"run_id", approval["run_id"]},
                          {"approval_id", approval["approval_id"]},
                          {"allow", true}})["type"]
                     .toString(),
                 QString("error"));
    }
    void refusesRegularFileAtEndpoint() {
        Harness h;
        QFile file(h.path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("owned data");
        file.close();
        QVERIFY(!h.server->startServer(h.path));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("owned data"));
    }
    void staleSocketRecovered() {
#ifdef Q_OS_UNIX
        Harness h;
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(fd >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto path = h.path.toLocal8Bit();
        QVERIFY(path.size() < static_cast<int>(sizeof(address.sun_path)));
        std::memcpy(address.sun_path, path.constData(), path.size() + 1);
        QCOMPARE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        ::close(fd);
        QVERIFY(QFileInfo::exists(h.path));
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
#else
        QSKIP("Stale Unix socket recovery is a Unix platform contract.");
#endif
    }
    void ownedSocketRemovedOnStop() {
#ifndef Q_OS_UNIX
        QSKIP("Socket filesystem node cleanup is a Unix platform contract.");
#endif
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        h.server->stopServer();
        QVERIFY(!QFileInfo::exists(h.path));
        QVERIFY(h.server->startServer(h.path));
    }
    void peerAuthorizationNativeSameUid() {
#ifdef Q_OS_UNIX
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        hello(s, h.path);
        QVERIFY(s.state() == QLocalSocket::ConnectedState);
        const auto fd = s.socketDescriptor();
        const auto peerUid = daemon::DaemonIpcServer::querySocketPeerUid(fd);
        QVERIFY(peerUid.has_value());
        QCOMPARE(peerUid.value(), ::getuid());
        const auto created = request(s, "session.create", {{"title", "Native Auth Session"}});
        QCOMPARE(created.value("type").toString(), QString("response"));
        QVERIFY(!created.value("payload").toObject().value("session_id").toString().isEmpty());
#else
        QSKIP("Peer authorization is a Unix platform contract.");
#endif
    }
    void peerAuthorizationDirectDescriptorValidation() {
#ifdef Q_OS_UNIX
        const auto invalidPeer = daemon::DaemonIpcServer::querySocketPeerUid(-1);
        QVERIFY(!invalidPeer.has_value());
#else
        QSKIP("Peer authorization is a Unix platform contract.");
#endif
    }
    void peerAuthorizationCrossUidRejectionSimulated() {
#ifdef Q_OS_UNIX
        Harness h;
        h.server->setPeerUidResolverForTesting(
            [](qintptr) { return std::optional<uid_t>(::getuid() + 1000); });
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        s.connectToServer(h.path);
        s.waitForConnected(1000);
        s.write("test\n");
        s.flush();
        QTRY_COMPARE(s.state(), QLocalSocket::UnconnectedState);
        QCOMPARE(s.error(), QLocalSocket::PeerClosedError);
        QCOMPARE(s.bytesAvailable(), 0);
        QVERIFY(s.readAll().isEmpty());
#else
        QSKIP("Peer authorization is a Unix platform contract.");
#endif
    }
    void peerAuthorizationMissingCredentialsRejectionSimulated() {
#ifdef Q_OS_UNIX
        Harness h;
        h.server->setPeerUidResolverForTesting([](qintptr) { return std::nullopt; });
        QVERIFY(h.server->startServer(h.path));
        QLocalSocket s;
        s.connectToServer(h.path);
        s.waitForConnected(1000);
        s.write("test\n");
        s.flush();
        QTRY_COMPARE(s.state(), QLocalSocket::UnconnectedState);
        QCOMPARE(s.error(), QLocalSocket::PeerClosedError);
        QCOMPARE(s.bytesAvailable(), 0);
        QVERIFY(s.readAll().isEmpty());
#else
        QSKIP("Peer authorization is a Unix platform contract.");
#endif
    }
    void peerAuthorizationRootUidRejectedWhenNonRootDaemon() {
#ifdef Q_OS_UNIX
        Harness h;
        h.server->setPeerUidResolverForTesting([](qintptr) { return std::optional<uid_t>(0); });
        QVERIFY(h.server->startServer(h.path));
        if (::getuid() != 0) {
            QLocalSocket s;
            s.connectToServer(h.path);
            s.waitForConnected(1000);
            s.write("test\n");
            s.flush();
            QTRY_COMPARE(s.state(), QLocalSocket::UnconnectedState);
            QCOMPARE(s.error(), QLocalSocket::PeerClosedError);
            QCOMPARE(s.bytesAvailable(), 0);
            QVERIFY(s.readAll().isEmpty());
        }
#else
        QSKIP("Peer authorization is a Unix platform contract.");
#endif
    }
    void peerAuthorizationNoFallbackToUnauthenticated() {
#ifdef Q_OS_UNIX
        Harness h;
        h.server->setPeerUidResolverForTesting(
            [](qintptr) { return std::optional<uid_t>(::getuid() + 999); });
        QVERIFY(h.server->startServer(h.path));
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(fd >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto path = h.path.toLocal8Bit();
        std::memcpy(address.sun_path, path.constData(), path.size() + 1);
        ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        const auto raw =
            QJsonDocument(message("session.list")).toJson(QJsonDocument::Compact) + '\n';
        ::write(fd, raw.constData(), raw.size());
        QTest::qWait(100);
        char buf[1024];
        const auto readBytes = ::read(fd, buf, sizeof(buf));
        QVERIFY(readBytes <= 0);
        ::close(fd);
#else
        QSKIP("Peer authorization is a Unix platform contract.");
#endif
    }
    void slowClientPendingBytesBackpressure() {
#ifdef Q_OS_UNIX
        Harness h(test::DeterministicChatReply::Final, true, nullptr, false,
                  std::make_unique<BackpressurePlanner>(8));
        QVERIFY(h.server->startServer(h.path));

        QLocalSocket fastClient;
        hello(fastClient, h.path);
        const auto created =
            request(fastClient, "session.create", {{"title", "Backpressure Session"}})["payload"]
                .toObject();
        const auto sessionId = created["session_id"].toString();
        QVERIFY(!sessionId.isEmpty());
        QCOMPARE(request(fastClient, "session.attach",
                         {{ "session_id",
                            sessionId }})["type"]
                     .toString(),
                 QString("response"));

        int slowFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(slowFd >= 0);
        int rcvBuf = 8192;
        ::setsockopt(slowFd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto path = h.path.toLocal8Bit();
        std::memcpy(address.sun_path, path.constData(), path.size() + 1);
        QCOMPARE(::connect(slowFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

        const auto helloMsg = QJsonDocument(message("hello", {{"client_id", "slow-reader"},
                                                              {"major", 1},
                                                              {"minor", 0},
                                                              { "capabilities",
                                                                QJsonArray {} }}))
                                  .toJson(QJsonDocument::Compact) +
                              '\n';
        QCOMPARE(::write(slowFd, helloMsg.constData(), helloMsg.size()), helloMsg.size());
        QTest::qWait(50);
        char buf[8192];
        ssize_t n = ::read(slowFd, buf, sizeof(buf));
        QVERIFY(n > 0);

        const auto attachMsg = QJsonDocument(message("session.attach",
                                                     {{ "session_id",
                                                        sessionId }}))
                                   .toJson(QJsonDocument::Compact) +
                               '\n';
        QCOMPARE(::write(slowFd, attachMsg.constData(), attachMsg.size()), attachMsg.size());
        QTest::qWait(50);
        n = ::read(slowFd, buf, sizeof(buf));
        QVERIFY(n > 0);

        ::fcntl(slowFd, F_SETFL, O_NONBLOCK);

        core::OllamaModelDiscoveryResult catalog;
        catalog.models = {{QStringLiteral("sentinel-test-model"), {}, 0}};
        catalog.lifecycle = core::ChatRequestLifecycle::Completed;
        catalog.errorCategory = core::ChatProviderErrorCategory::None;
        catalog.safeDetail = QStringLiteral("Deterministic test catalog is ready.");
        h.controller->modelService()->acceptOllamaDiscovery(
            catalog, h.controller->modelService()->beginProviderHealthObservation());

        const auto startResp =
            request(fastClient, "agent.start",
                    {{"session_id", sessionId}, {"text", "produce backpressure"}});
        QCOMPARE(startResp["type"].toString(), QString("response"));
        const auto runId = startResp["payload"].toObject()["run_id"].toString();
        QVERIFY(!runId.isEmpty());

        bool completed = false;
        qint64 lastSequence = 0;
        QElapsedTimer timer;
        timer.start();
        while (!completed && timer.elapsed() < 10000) {
            const auto event = receive(fastClient, 200);
            if (event.isEmpty())
                continue;
            qDebug() << "FastClient got:" << event.value("name").toString()
                     << "type:" << event.value("type").toString()
                     << "detail:" << event.value("payload").toObject()["detail"].toString();
            QVERIFY(event.value("type").toString() == QString("event"));
            const auto payload = event.value("payload").toObject();
            const auto seq = payload.value("event_sequence").toInteger();
            if (seq > 0) {
                QVERIFY(seq > lastSequence);
                lastSequence = seq;
            }
            if (event.value("name").toString() == QString("run.completed")) {
                completed = true;
                QCOMPARE(payload.value("text").toString(), QString("Backpressure test finished"));
            }
        }
        QVERIFY(completed);

        QTest::qWait(100);
        char drainBuf[1024];
        while (::read(slowFd, drainBuf, sizeof(drainBuf)) > 0) {
        }
        const ssize_t closedRead = ::read(slowFd, drainBuf, sizeof(drainBuf));
        QVERIFY(closedRead <= 0);
        ::close(slowFd);

        QLocalSocket freshClient;
        hello(freshClient, h.path);
        const auto snapshot = request(freshClient, "session.attach", {{ "session_id", sessionId }});
        QCOMPARE(snapshot["type"].toString(), QString("response"));
        const auto snapPayload = snapshot["payload"].toObject();
        QCOMPARE(snapPayload["state"].toString(), QString("completed"));
        QCOMPARE(snapPayload["run_id"].toString(), runId);
#else
        QSKIP("Slow client backpressure is a Unix platform contract.");
#endif
    }
    void interruptedRunningRunReconciledOnRestart() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Final, true);
        const auto dbPath = h.directory.filePath("sessions.sqlite3");
        QVERIFY(h.server->openSessionProjectionStore(dbPath));
        QVERIFY(h.server->startServer(h.path));

        QLocalSocket s;
        hello(s, h.path);
        const auto created =
            request(s, "session.create", {{"title", "Interrupted Run"}})["payload"].toObject();
        const auto sessionId = created["session_id"].toString();
        QVERIFY(!sessionId.isEmpty());

        const auto started =
            request(s, "agent.start", {{"session_id", sessionId}, {"text", "Run task"}});
        QCOMPARE(started["type"].toString(), QString("response"));
        const auto runId = started["payload"].toObject()["run_id"].toString();
        QVERIFY(!runId.isEmpty());

        bool runStartedEvent = false;
        for (int i = 0; i < 5 && !runStartedEvent; ++i) {
            const auto e = receive(s, 200);
            if (e["name"] == "run.started")
                runStartedEvent = true;
        }
        QVERIFY(runStartedEvent);

        s.disconnectFromServer();
        h.server->stopServer();
        h.controller->cancelAgentRun();
        h.server.reset();

        daemon::DaemonIpcServer recovered(h.controller.get());
        QVERIFY(recovered.openSessionProjectionStore(dbPath));
        QVERIFY(recovered.startServer(h.path));

        QLocalSocket s2;
        hello(s2, h.path);
        const auto attachResp = request(s2, "session.attach", {{"session_id", sessionId}});
        QCOMPARE(attachResp["type"].toString(), QString("response"));
        const auto payload = attachResp["payload"].toObject();

        QCOMPARE(payload["session_id"].toString(), sessionId);
        QCOMPARE(payload["run_id"].toString(), runId);
        QCOMPARE(payload["state"].toString(), QString("failed"));
        QCOMPARE(payload["detail"].toString(), QString("daemon-restarted"));
        QCOMPARE(payload["output"].toString(), QString{});
        QVERIFY(payload["approval_id"].toString().isEmpty());
        QVERIFY(h.controller->persistentPermissionGrants().isEmpty());

        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "test_verify_interrupted_run");
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QString("SELECT payload FROM session_projection WHERE session_id = '%1'")
                               .arg(sessionId)));
            QVERIFY(q.next());
            const auto diskVal = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
            QCOMPARE(diskVal["state"].toString(), QString("failed"));
            QCOMPARE(diskVal["detail"].toString(), QString("daemon-restarted"));
            db.close();
        }
        QSqlDatabase::removeDatabase("test_verify_interrupted_run");
    }
    void interruptedApprovalRunReconciledOnRestart() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Final, true);
        const auto dbPath = h.directory.filePath("sessions.sqlite3");
        QVERIFY(h.server->openSessionProjectionStore(dbPath));
        QVERIFY(h.server->startServer(h.path));

        QLocalSocket s;
        hello(s, h.path);
        const auto created =
            request(s, "session.create", {{"title", "Interrupted Approval"}})["payload"].toObject();
        const auto sessionId = created["session_id"].toString();
        QVERIFY(!sessionId.isEmpty());

        const auto started =
            request(s, "agent.start", {{"session_id", sessionId}, {"text", "Run with approval"}});
        const auto runId = started["payload"].toObject()["run_id"].toString();
        QVERIFY(!runId.isEmpty());

        QJsonObject approval;
        for (int i = 0; i < 10 && approval.isEmpty(); ++i) {
            const auto e = receive(s, 200);
            if (e["name"] == "approval.requested")
                approval = e["payload"].toObject();
        }
        QVERIFY(!approval.isEmpty());
        const auto approvalId = approval["approval_id"].toString();
        QVERIFY(!approvalId.isEmpty());

        s.disconnectFromServer();
        h.server->stopServer();
        h.controller->cancelAgentRun();
        h.server.reset();

        daemon::DaemonIpcServer recovered(h.controller.get());
        QVERIFY(recovered.openSessionProjectionStore(dbPath));
        QVERIFY(recovered.startServer(h.path));

        QLocalSocket s2;
        hello(s2, h.path);
        const auto attachResp = request(s2, "session.attach", {{"session_id", sessionId}});
        QCOMPARE(attachResp["type"].toString(), QString("response"));
        const auto payload = attachResp["payload"].toObject();

        QCOMPARE(payload["session_id"].toString(), sessionId);
        QCOMPARE(payload["run_id"].toString(), runId);
        QCOMPARE(payload["state"].toString(), QString("failed"));
        QCOMPARE(payload["detail"].toString(), QString("daemon-restarted"));
        QCOMPARE(payload["output"].toString(), QString{});
        QVERIFY(payload["approval_id"].toString().isEmpty());

        const auto replayResp =
            request(s2, "approval.respond",
                    {{"run_id", runId}, {"approval_id", approvalId}, {"allow", true}});
        QCOMPARE(replayResp["type"].toString(), QString("error"));
        QCOMPARE(replayResp["payload"].toObject()["code"].toString(), QString("invalid-approval"));
    }
    void completedRunSurvivesDaemonRestart() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h(test::DeterministicChatReply::Final, true);
        const auto dbPath = h.directory.filePath("sessions.sqlite3");
        QVERIFY(h.server->openSessionProjectionStore(dbPath));
        QVERIFY(h.server->startServer(h.path));

        QLocalSocket s, control;
        hello(s, h.path);
        hello(control, h.path);
        const auto created =
            request(s, "session.create", {{"title", "Completed Run"}})["payload"].toObject();
        const auto sessionId = created["session_id"].toString();
        QVERIFY(!sessionId.isEmpty());

        const auto started =
            request(s, "agent.start", {{"session_id", sessionId}, {"text", "Run to completion"}});
        const auto runId = started["payload"].toObject()["run_id"].toString();

        QJsonObject approval;
        for (int i = 0; i < 10 && approval.isEmpty(); ++i) {
            const auto e = receive(s, 200);
            if (e["name"] == "approval.requested")
                approval = e["payload"].toObject();
        }
        QVERIFY(!approval.isEmpty());
        QCOMPARE(request(control, "approval.respond",
                         {{"run_id", runId},
                          {"approval_id", approval["approval_id"]},
                          {"allow", true}})["type"]
                     .toString(),
                 QString("response"));

        bool completed = false;
        for (int i = 0; i < 15 && !completed; ++i) {
            const auto e = receive(s, 200);
            if (e["name"] == "run.completed")
                completed = true;
        }
        QVERIFY(completed);

        s.disconnectFromServer();
        control.disconnectFromServer();
        h.server->stopServer();
        h.controller->cancelAgentRun();
        h.server.reset();

        daemon::DaemonIpcServer recovered(h.controller.get());
        QVERIFY(recovered.openSessionProjectionStore(dbPath));
        QVERIFY(recovered.startServer(h.path));

        QLocalSocket s2;
        hello(s2, h.path);
        const auto attachResp = request(s2, "session.attach", {{"session_id", sessionId}});
        QCOMPARE(attachResp["type"].toString(), QString("response"));
        const auto payload = attachResp["payload"].toObject();

        QCOMPARE(payload["session_id"].toString(), sessionId);
        QCOMPARE(payload["run_id"].toString(), runId);
        QCOMPARE(payload["state"].toString(), QString("completed"));
        QCOMPARE(payload["output"].toString(), QString("Fixture finished"));
    }
    void multipleInterruptedRunsReconciledAtomicallyOnRestart() {
#ifndef Q_OS_UNIX
        QSKIP("Daemon IPC peer authentication requires SO_PEERCRED (Unix-only); "
              "Windows named-pipe transport is not implemented (see PD-DWIN-01).");
#endif
        Harness h;
        const auto s1 = h.controller->createConversation("Session 1");
        const auto s2 = h.controller->createConversation("Session 2");
        const auto s3 = h.controller->createConversation("Session 3");
        const auto dbPath = h.directory.filePath("multi_sessions.sqlite3");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "seed_multi_sessions");
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE session_projection (session_id TEXT PRIMARY KEY, payload "
                           "TEXT NOT NULL)"));
            q.prepare("INSERT INTO session_projection (session_id, payload) VALUES (?, ?)");

            q.bindValue(0, s1);
            q.bindValue(1,
                        QString::fromUtf8(QJsonDocument(QJsonObject{{"run_id", "r1"},
                                                                    {"state", "running"},
                                                                    {"run_type", "agent"},
                                                                    {"output", "running output"}})
                                              .toJson(QJsonDocument::Compact)));
            QVERIFY(q.exec());

            q.bindValue(0, s2);
            q.bindValue(1, QString::fromUtf8(QJsonDocument(QJsonObject{{"run_id", "r2"},
                                                                       {"state", "approval"},
                                                                       {"run_type", "agent"},
                                                                       {"approval_id", "app1"}})
                                                 .toJson(QJsonDocument::Compact)));
            QVERIFY(q.exec());

            q.bindValue(0, s3);
            q.bindValue(1, QString::fromUtf8(QJsonDocument(QJsonObject{{"run_id", "r3"},
                                                                       {"state", "completed"},
                                                                       {"run_type", "agent"},
                                                                       {"output", "final result"}})
                                                 .toJson(QJsonDocument::Compact)));
            QVERIFY(q.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase("seed_multi_sessions");

        // First restart: reconciles s1 and s2, keeps s3 intact
        {
            daemon::DaemonIpcServer server1(h.controller.get());
            QVERIFY(server1.openSessionProjectionStore(dbPath));
            QVERIFY(server1.startServer(h.path));

            QLocalSocket s;
            hello(s, h.path);

            const auto snap1 =
                request(s, "session.attach", {{"session_id", s1}})["payload"].toObject();
            QCOMPARE(snap1["state"].toString(), QString("failed"));
            QCOMPARE(snap1["detail"].toString(), QString("daemon-restarted"));
            QVERIFY(snap1["output"].toString().isEmpty());

            const auto snap2 =
                request(s, "session.attach", {{"session_id", s2}})["payload"].toObject();
            QCOMPARE(snap2["state"].toString(), QString("failed"));
            QCOMPARE(snap2["detail"].toString(), QString("daemon-restarted"));
            QVERIFY(snap2["output"].toString().isEmpty());

            const auto snap3 =
                request(s, "session.attach", {{"session_id", s3}})["payload"].toObject();
            QCOMPARE(snap3["state"].toString(), QString("completed"));
            QCOMPARE(snap3["output"].toString(), QString("final result"));

            s.disconnectFromServer();
            server1.stopServer();
        }

        // Verify disk storage directly agrees with in-memory projections for all rows
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "verify_multi_disk");
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);

            QVERIFY(q.exec(
                QString("SELECT payload FROM session_projection WHERE session_id = '%1'").arg(s1)));
            QVERIFY(q.next());
            auto d1 = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
            QCOMPARE(d1["state"].toString(), QString("failed"));
            QCOMPARE(d1["detail"].toString(), QString("daemon-restarted"));

            QVERIFY(q.exec(
                QString("SELECT payload FROM session_projection WHERE session_id = '%1'").arg(s2)));
            QVERIFY(q.next());
            auto d2 = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
            QCOMPARE(d2["state"].toString(), QString("failed"));
            QCOMPARE(d2["detail"].toString(), QString("daemon-restarted"));

            QVERIFY(q.exec(
                QString("SELECT payload FROM session_projection WHERE session_id = '%1'").arg(s3)));
            QVERIFY(q.next());
            auto d3 = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
            QCOMPARE(d3["state"].toString(), QString("completed"));
            QCOMPARE(d3["output"].toString(), QString("final result"));

            db.close();
        }
        QSqlDatabase::removeDatabase("verify_multi_disk");

        // Second restart: prove idempotency across repeated restarts
        {
            daemon::DaemonIpcServer server2(h.controller.get());
            QVERIFY(server2.openSessionProjectionStore(dbPath));
            QVERIFY(server2.startServer(h.path));

            QLocalSocket s;
            hello(s, h.path);

            const auto snap1 =
                request(s, "session.attach", {{"session_id", s1}})["payload"].toObject();
            QCOMPARE(snap1["state"].toString(), QString("failed"));
            QCOMPARE(snap1["detail"].toString(), QString("daemon-restarted"));

            const auto snap2 =
                request(s, "session.attach", {{"session_id", s2}})["payload"].toObject();
            QCOMPARE(snap2["state"].toString(), QString("failed"));
            QCOMPARE(snap2["detail"].toString(), QString("daemon-restarted"));

            const auto snap3 =
                request(s, "session.attach", {{"session_id", s3}})["payload"].toObject();
            QCOMPARE(snap3["state"].toString(), QString("completed"));
            QCOMPARE(snap3["output"].toString(), QString("final result"));

            s.disconnectFromServer();
            server2.stopServer();
        }
    }
    void testSeamCannotBeModifiedWhileListening() {
#ifdef Q_OS_UNIX
        Harness h;
        QVERIFY(h.server->startServer(h.path));
        // Attempting to modify the test resolver while listening must be rejected
        h.server->setPeerUidResolverForTesting(
            [](qintptr) { return std::optional<uid_t>(::getuid() + 999); });
        // Native same-UID connection must still succeed because mutation was ignored
        QLocalSocket s;
        hello(s, h.path);
        const auto resp = request(s, "daemon.status");
        QCOMPARE(resp["type"].toString(), QString("response"));
        s.disconnectFromServer();
#else
        QSKIP("Peer authorization is a Unix platform contract.");
#endif
    }
    void rejectsSocketPathBeyondPlatformLimit() {
#ifdef Q_OS_UNIX
        Harness h;
        const QString name(200, QLatin1Char('x'));
        const auto path = h.directory.filePath(name + QStringLiteral(".sock"));
        QVERIFY(!h.server->startServer(path));
        QVERIFY2(h.server->lastServerError().contains(QStringLiteral("platform limit")),
                 qPrintable(h.server->lastServerError()));
#else
        QSKIP("sockaddr_un path limits are a Unix platform contract.");
#endif
    }
};
QTEST_GUILESS_MAIN(DaemonIpcServerTest)
#include "test_daemon_ipc.moc"
