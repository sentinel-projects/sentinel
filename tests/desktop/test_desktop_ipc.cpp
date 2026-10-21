#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/desktop/ExtensionsHubPresentation.h"
#include "sentinel/desktop/RemoteAgentInspectorService.h"
#include "sentinel/desktop/viewmodels/AgentInspectorViewModel.h"
#include "service/DaemonModelHelpers.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../protocol/QtIpcContract.generated.h"
#include "../support/DeterministicChatFixture.h"
#include "sentinel/core/agent/AgentRuntime.h"
#include "sentinel/core/agent/IAgentStepPlanner.h"
#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/app/OnboardingService.h"
#include "sentinel/core/mcp/McpToolProvider.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/memory/InMemoryStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include "sentinel/core/model/ModelLibrary.h"
#include "sentinel/core/security/StaticSandboxPolicy.h"
#include "sentinel/desktop/DaemonClient.h"
#include "sentinel/desktop/DesktopBackupHelper.h"
#include "sentinel/desktop/DesktopControllerBridge.h"
#include "sentinel/desktop/DesktopModelHelper.h"
#include "sentinel/desktop/DesktopRuntimeClient.h"
#include "sentinel/desktop/DesktopSettingsStore.h"
#include "sentinel/desktop/QuickPanelController.h"
#include "service/DaemonIpcServer.h"
#include <QTimer>

#include <QDir>
#include <QCryptographicHash>
#include <QJSEngine>
#include <QJsonDocument>
#include <QLocalServer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using sentinel::desktop::DaemonClient;
namespace {
class CountingCredentials final : public sentinel::core::ICredentialBackend {
public:
    sentinel::core::CredentialStoreBackend backend() const override {
        return delegate.backend();
    }
    sentinel::core::CredentialStoreSummary summary() const override {
        return delegate.summary();
    }
    sentinel::core::CredentialBackendResult
    storeCredential(const sentinel::core::CredentialKey& key, const QString& secret) override {
        return delegate.storeCredential(key, secret);
    }
    sentinel::core::CredentialReadResult
    readCredential(const sentinel::core::CredentialKey& key) const override {
        ++reads;
        return delegate.readCredential(key);
    }
    sentinel::core::CredentialBackendResult
    deleteCredential(const sentinel::core::CredentialKey& key) override {
        return delegate.deleteCredential(key);
    }
    sentinel::core::CredentialBackendResult
    containsCredential(const sentinel::core::CredentialKey& key) const override {
        ++reads;
        return delegate.containsCredential(key);
    }
    mutable int reads = 0;
    sentinel::core::InMemoryCredentialBackend delegate;
};
QJsonObject frame(const QString& type, const QString& id, const QString& name,
                  const QJsonObject& payload, int major = 1, int minor = 0) {
    return {{"version", QJsonObject{{"major", major}, {"minor", minor}}},
            {"type", type},
            {"id", id},
            {"name", name},
            {"payload", payload}};
}
void send(QLocalSocket* socket, const QJsonObject& object) {
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
}
QJsonObject read(QLocalSocket* socket) {
    QElapsedTimer timer;
    timer.start();
    while (!socket->canReadLine() && timer.elapsed() < 2000) {
        QTest::qWait(1);
    }
    return QJsonDocument::fromJson(socket->readLine()).object();
}
class DictationStt final : public sentinel::core::ISpeechToTextRuntime {
public:
    sentinel::core::SpeechProviderInfo info() const override {
        sentinel::core::SpeechProviderInfo result;
        result.readiness = sentinel::core::AudioRuntimeReadiness::Ready;
        return result;
    }
    sentinel::core::SpeechTranscript transcribeFile(const QString&, const QString&,
                                                    std::shared_ptr<std::atomic_bool>) override {
        return {.finalText = "Private dictation"};
    }
};
class MetadataClient final : public sentinel::core::IOllamaRuntimeClient {
public:
    sentinel::core::OllamaConfig config() const override {
        return {};
    }
    sentinel::core::OllamaHealthCheckResult healthCheck() const override {
        sentinel::core::OllamaHealthCheckResult result;
        result.connectionStatus = sentinel::core::OllamaConnectionStatus::Connected;
        result.healthStatus = sentinel::core::OllamaHealthStatus::Healthy;
        return result;
    }
    QList<sentinel::core::OllamaModelSummary> installedModels() const override {
        return {{QStringLiteral("sentinel-test-model"), {}, 0}};
    }
};
class ApprovalPlanner final : public sentinel::core::IAgentStepPlanner {
public:
    sentinel::core::AgentStepDecision
    nextStep(const QString&, const QList<sentinel::core::AgentStepRecord>& history) const override {
        sentinel::core::AgentStepDecision step;
        if (history.isEmpty()) {
            step.kind = sentinel::core::AgentStepDecision::Kind::ToolCall;
            step.toolId = "run-command";
            step.toolName = "run-command";
            step.riskLevel = sentinel::core::ToolRiskLevel::High;
            step.arguments.append({QStringLiteral("command"), QStringLiteral("pwd")});
        } else {
            step.kind = sentinel::core::AgentStepDecision::Kind::FinalAnswer;
            step.answer = "Fixture finished";
        }
        return step;
    }
};
class FixtureExecutor final : public sentinel::core::IToolExecutor {
public:
    sentinel::core::ToolExecutionResult
    execute(const sentinel::core::ToolExecutionRequest&) const override {
        return {sentinel::core::ToolExecutionStatus::Succeeded, QStringLiteral("Fixture result")};
    }
};
struct Peer {
    QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
    QLocalServer server;
    QLocalSocket* socket = nullptr;
    QString path;
    Peer() : path(directory.filePath("fixture.sock")) {
        server.listen(path);
    }
    bool connect(DaemonClient& client, int major = 1, bool generation = true) {
        client.connectToDaemon();
        QElapsedTimer timer;
        timer.start();
        while (!server.hasPendingConnections() && timer.elapsed() < 2000) {
            QTest::qWait(1);
        }
        socket = server.nextPendingConnection();
        if (!socket) {
            return false;
        }
        const auto hello = read(socket);
        if (hello.value("name") != "hello") {
            return false;
        }
        QJsonObject payload{{"major", major},
                            {"minor", 0},
                            {"daemon_version", "fixture"},
                            {"capabilities", QJsonArray{}}};
        if (generation) {
            payload.insert("server_generation", "fixture-generation");
        }
        send(socket, frame("response", hello.value("id").toString(), "hello", payload, major));
        return true;
    }
    void status() {
        const auto request = read(socket);
        send(socket, frame("response", request.value("id").toString(), "daemon.status",
                           {{"running", true},
                            {"daemon_version", "fixture"},
                            {"uptime_ms", 0},
                            {"active_runs", 0},
                            {"sessions", 0}}));
    }
};
// A wire peer with explicit barriers, not timers, for initial history and
// reattachment. All replies use the actual typed IPC projection contract.
struct RuntimePeer : Peer {
    QJsonObject snapshot{{"session_id", "fixture-session"},
                         {"run_id", "fixture-run"},
                         {"run_type", "agent"},
                         {"state", "running"},
                         {"output", ""},
                         {"server_generation", "fixture-generation"},
                         {"event_sequence", 1},
                         {"properties", QJsonObject{}}};
    QJsonObject historyRequest;
    bool holdHistory = true;
    bool holdExtensions = false;
    QJsonObject extensionsRequest, extensionActionRequest;
    static QJsonValue emptyValue(const QString& type) {
        if (type == "string") {
            return QString{};
        }
        if (type == "boolean") {
            return false;
        }
        if (type == "array") {
            return QJsonArray{};
        }
        if (type == "object") {
            return QJsonObject{};
        }
        return 0;
    }
    void replyHistory() {
        const QJsonArray messages{
            QJsonObject{{"id", 1}, {"role", "user"}, {"content", "First recorded message"}},
            QJsonObject{{"id", 2}, {"role", "assistant"}, {"content", "Second recorded message"}}};
        send(socket,
             frame("response", historyRequest.value("id").toString(), "session.messages",
                   {{"session_id", historyRequest.value("payload").toObject().value("session_id")},
                    {"messages", messages}}));
        historyRequest = {};
    }
    // A fresh conversation only carries the startup system notice; the greeting
    // screen must stay visible because no user/assistant history exists yet.
    void replySystemOnlyHistory() {
        const QJsonArray messages{QJsonObject{{"id", 1},
                                              {"role", "system"},
                                              {"content", "Sentinel Core online."}}};
        send(socket,
             frame("response", historyRequest.value("id").toString(), "session.messages",
                   {{"session_id", historyRequest.value("payload").toObject().value("session_id")},
                    {"messages", messages}}));
        historyRequest = {};
    }
    void pump() {
        while (socket && socket->canReadLine()) {
            const auto request = read(socket);
            const auto name = request.value("name").toString();
            QJsonObject payload;
            // Background metadata requests (for example voice.state) remain
            // valid even when a barrier spans a timer tick under ASan.
            const auto required = sentinel::ipc::responses().value(name).toObject();
            for (auto it = required.begin(); it != required.end(); ++it) {
                payload[it.key()] = emptyValue(it.value().toString());
            }
            if (name == "daemon.status") {
                payload = {{"running", true},
                           {"daemon_version", "fixture"},
                           {"uptime_ms", 0},
                           {"active_runs", 1},
                           {"sessions", 1}};
            } else if (name == "desktop.projection") {
                QJsonObject properties;
                const auto fields = sentinel::ipc::desktopProjectionFields();
                for (auto it = fields.begin(); it != fields.end(); ++it) {
                    const auto field = it.value().toObject();
                    if (field.value("scope") != "global" || !field.value("eager").toBool()) {
                        continue;
                    }
                    const auto type = field.value("type").toString();
                    properties[it.key()] = emptyValue(type);
                }
                payload = {{"page", 0}, {"pages", 1}, {"properties", properties}};
            } else if (name == "desktop.settings") {
                payload = {{"values", QJsonObject{}}};
            } else if (name == "session.list") {
                payload = {{"sessions", QJsonArray{snapshot}}};
            } else if (name == "session.attach") {
                payload = snapshot;
            } else if (name == "desktop.settings_service" && holdExtensions) {
                const auto action = request.value("payload").toObject().value("action").toString();
                if (action == "extensionsHubState") {
                    extensionsRequest = request;
                    continue;
                }
                if (action == "performExtensionSettingsAction") {
                    extensionActionRequest = request;
                    continue;
                }
            } else if (name == "session.messages") {
                historyRequest = request;
                if (!holdHistory) {
                    replyHistory();
                }
                continue;
            }
            send(socket, frame("response", request.value("id").toString(), name, payload));
        }
    }
};
} // namespace
class DesktopIpcTest final : public QObject {
    Q_OBJECT
private slots:
    void settingsStorageAdmissionRefusalIsTruthfulOverIpc() {
        using namespace sentinel::core;
        using namespace std::chrono_literals;
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        const auto path = profile.filePath("settings.json");
        auto admission = std::make_shared<StorageAdmission>();
        AppSettings settings(std::make_unique<JsonSettingsStore>(path),
                             inMemoryTestCredentialStore());
        QVERIFY(settings.attachStorageAdmission(admission));
        settings.setLocalKnowledgeBaseEnabled(false);
        sentinel::test::DeterministicModelServiceFixture models;
        ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                              .withMemoryStore(std::make_unique<InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        server.setSettings(&settings);
        QVERIFY(server.startServer(profile.filePath("settings.sock")));
        DaemonClient transport(profile.filePath("settings.sock"), 3000, 10);
        sentinel::desktop::DesktopRuntimeClient runtime(transport);
        QTRY_VERIFY(runtime.ready());
        AppSettings projected(std::make_unique<sentinel::desktop::DesktopSettingsStore>(
                                  runtime, profile.filePath("presentation.json")),
                              inMemoryTestCredentialStore());
        QSignalSpy failed(&runtime, &sentinel::desktop::DesktopRuntimeClient::operationFailed);
        QSignalSpy changed(&projected, &AppSettings::workspaceSettingsChanged);
        QCOMPARE(admission->prepareLocal(0ms), StorageAdmission::Result::Ready);
        projected.setLocalKnowledgeBaseEnabled(true);
        QCOMPARE(projected.storageErrorCode(), QString("Pending"));
        QVERIFY(!projected.localKnowledgeBaseEnabled());
        QCOMPARE(changed.count(), 0);
        QTRY_VERIFY(!failed.isEmpty());
        QCOMPARE(failed.last().first().toString(), QString("StorageBusy"));
        QVERIFY(!settings.localKnowledgeBaseEnabled());
        auto created = WorkspaceService{}.createWorkspace({}, "IPC fixture", "Personal");
        QVERIFY(created.success);
        failed.clear();
        QVERIFY(!projected.persistWorkspaceState(created.catalogJson, created.selectedWorkspaceId));
        QCOMPARE(projected.storageErrorCode(), QString("Pending"));
        QTRY_VERIFY(!failed.isEmpty());
        QCOMPARE(failed.last().first().toString(), QString("StorageBusy"));
        QVERIFY(settings.workspaceCatalogJson().isEmpty());
        QCOMPARE(settings.selectedWorkspaceId(), QString("personal"));
        QCOMPARE(admission->resume(), StorageAdmission::Result::Ready);
        QTest::qWait(20); // A refused write is not replayed on resume.
        QVERIFY(settings.workspaceCatalogJson().isEmpty());
        QVERIFY(!projected.persistWorkspaceState(created.catalogJson, created.selectedWorkspaceId));
        QTRY_COMPARE(settings.selectedWorkspaceId(), created.selectedWorkspaceId);
        QTRY_COMPARE(runtime.settingsValue("selectedWorkspaceId", {}), created.selectedWorkspaceId);
        JsonSettingsStore disk(path);
        QCOMPARE(disk.value("workspaceCatalogJson"), created.catalogJson);
        QCOMPARE(disk.value("selectedWorkspaceId"), created.selectedWorkspaceId);
        projected.setLocalKnowledgeBaseEnabled(true);
        QTRY_VERIFY(settings.localKnowledgeBaseEnabled());
        transport.disconnectFromDaemon();
    }
    void largeMemoryArchiveUsesNativeDesktopBoundary() {
        using namespace sentinel::core;
        QTemporaryDir directory{QDir::tempPath() + "/ma-XXXXXX"};
        QVERIFY(directory.isValid());
        sentinel::test::DeterministicModelServiceFixture fixture;
        auto memory = std::make_unique<SQLiteMemoryStore>(directory.filePath("memory.db"));
        auto* store = memory.get();
        ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(fixture.takeModelService())
                              .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                              .withMemoryStore(std::move(memory))
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        QVERIFY(server.startServer(directory.filePath("archive.sock")));
        DaemonClient transport(directory.filePath("archive.sock"), 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QTRY_VERIFY(adapter.ready());
        const auto listed = controller->manageMemory({{"operation", "list"}, {"scope", 1}});
        QCOMPARE(listed.value("status").toString(), QString("Ready"));
        const auto session = listed.value("session").toString(),
                   workspace = listed.value("workspace").toString();
        MemoryAccessContext access{session, workspace, {workspace}, false};
        QString id;
        for (int i = 0; i < 550; ++i) {
            const auto result = store->writeScopedMemory(
                {MemoryScope::Session, QString("large%1").arg(i), QString(400, 'x')}, access);
            QCOMPARE(result.status, ScopedMemoryStatus::Ready);
            if (i == 549) {
                id = result.records.first().id;
            }
        }
        QVariantMap context{{"scope", 1},
                            {"session", session},
                            {"workspace", workspace},
                            {"maxBytes", 64 * 1024 * 1024},
                            {"maxRecords", 10000}};
        sentinel::desktop::DesktopBackupHelper helper(&adapter);
        const auto url = QUrl::fromLocalFile(directory.filePath("scope.smem"));
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, &helper, [&] { ++ticks; });
        heartbeat.start(1);
        QVERIFY(helper.exportMemoryFile(url, context));
        QTRY_VERIFY_WITH_TIMEOUT(!helper.busy(), 15000);
        QVERIFY2(helper.status().contains("saved successfully"), qPrintable(helper.status()));
        QVERIFY(ticks > 0);
        QVERIFY(QFileInfo(url.toLocalFile()).size() > 98304);
        QVERIFY(!helper.exportMemoryFile(url, context));
        auto clear = context;
        clear["operation"] = "clear";
        clear["confirmed"] = true;
        clear["confirmation"] = "DELETE";
        clear.remove("maxBytes");
        clear.remove("maxRecords");
        QCOMPARE(controller->manageMemory(clear).value("status").toString(), QString("Ready"));
        QVERIFY(helper.inspectMemoryFile(url, context));
        QTRY_VERIFY_WITH_TIMEOUT(helper.memoryReady(), 15000);
        QVERIFY(helper.preview().contains("550 records"));
        QCOMPARE(store->retrieveMemory("large549", access).records.size(), 0);
        helper.cancel();
        QTRY_VERIFY(!helper.memoryTransferActive());
        QCOMPARE(store->retrieveMemory("large549", access).records.size(), 0);
        QVERIFY(helper.inspectMemoryFile(url, context));
        QTRY_VERIFY_WITH_TIMEOUT(helper.memoryReady(), 15000);
        QSignalSpy invalidated(controller.get(), &ApplicationController::memoryRecallChanged);
        QVERIFY(helper.restoreMemoryFile(false));
        QTRY_VERIFY_WITH_TIMEOUT(!helper.busy(), 15000);
        QVERIFY2(helper.status().contains("restored successfully"), qPrintable(helper.status()));
        QTRY_VERIFY(!invalidated.isEmpty());
        const auto restored = store->retrieveMemory("large549", access);
        QCOMPARE(restored.records.first().id, id);
        QVERIFY(isImportedMemory(restored.records.first()));
        const auto cancelled = QUrl::fromLocalFile(directory.filePath("cancelled.smem"));
        QVERIFY(helper.exportMemoryFile(cancelled, context));
        helper.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!helper.busy(), 15000);
        QVERIFY(!QFile::exists(cancelled.toLocalFile()));
    }
    void scopedMemoryManagementUsesRealDesktopAction() {
        QTemporaryDir directory{QDir::tempPath() + "/mm-XXXXXX"};
        QVERIFY(directory.isValid());
        sentinel::test::DeterministicModelServiceFixture fixture;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(fixture.takeModelService())
                              .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        QVERIFY(server.startServer(directory.filePath("memory.sock")));
        DaemonClient transport(directory.filePath("memory.sock"), 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QTRY_VERIFY(adapter.ready());
        int tag = 0;
        const auto call = [&](QVariantMap request) {
            request["requestTag"] = QString::number(++tag);
            QSignalSpy done(&adapter,
                            &sentinel::desktop::DesktopRuntimeClient::memoryManagementCompleted);
            adapter.dispatch("manageMemory", {request});
            if (!QTest::qWaitFor([&] { return !done.isEmpty(); }, 5000)) {
                return QVariantMap{{"status", "Timeout"}};
            }
            return done.last().first().toMap();
        };
        auto listed = call({{"operation", "list"}, {"scope", 1}});
        QCOMPARE(listed.value("status").toString(), QString("Ready"));
        QVariantMap base{{"scope", 1},
                         {"session", listed.value("session")},
                         {"workspace", listed.value("workspace")}};
        auto make = base;
        make["operation"] = "create";
        make["key"] = "orchid";
        make["value"] = "before";
        QSignalSpy invalidated(controller.get(),
                               &sentinel::core::ApplicationController::memoryRecallChanged);
        QCOMPARE(call(make).value("status").toString(), QString("Ready"));
        QVERIFY(!invalidated.isEmpty());
        const auto ranked = call({{"operation", "search"}, {"scope", 1}, {"search", "orchid"}});
        QCOMPARE(ranked.value("status").toString(), QString("Ready"));
        QCOMPARE(ranked.value("semantic").toString(), QString("Unavailable"));
        const auto relevance =
            ranked.value("records").toList().first().toMap().value("relevance").toMap();
        QVERIFY(relevance.value("exactKey").toBool());
        QVERIFY(relevance.value("semanticSimilarity").isNull());
        auto refresh = base;
        refresh["operation"] = "refreshIndex";
        QCOMPARE(call(refresh).value("status").toString(), QString("InvalidInput"));
        refresh["confirmed"] = true;
        refresh["confirmation"] = "REFRESH";
        QCOMPARE(call(refresh).value("status").toString(), QString("Ready"));
        listed = call({{"operation", "list"}, {"scope", 1}});
        auto row = listed.value("records").toList().first().toMap();
        auto exported = call({{"operation", "export"}, {"scope", 1}});
        QCOMPARE(exported.value("status").toString(), QString("Ready"));
        sentinel::desktop::DesktopBackupHelper helper(&adapter);
        const auto path = QUrl::fromLocalFile(directory.filePath("scope.json"));
        QVERIFY(helper.saveMemoryArchive(path, exported.value("archive").toString()));
        QCOMPARE(helper.loadMemoryArchive(path).value("status").toString(), QString("Ready"));
        auto edit = base;
        edit["operation"] = "correct";
        edit["id"] = row.value("id");
        edit["revision"] = row.value("revision");
        edit["key"] = "orchid";
        edit["value"] = "after";
        QCOMPARE(call(edit).value("status").toString(), QString("Ready"));
        QCOMPARE(call(edit).value("status").toString(), QString("Conflict"));
        auto stale = make;
        stale["workspace"] = "foreign";
        QCOMPARE(call(stale).value("status").toString(), QString("Conflict"));
        listed = call({{"operation", "list"}, {"scope", 1}});
        row = listed.value("records").toList().first().toMap();
        auto remove = base;
        remove["operation"] = "delete";
        remove["id"] = row.value("id");
        remove["revision"] = row.value("revision");
        invalidated.clear();
        QCOMPARE(call(remove).value("status").toString(), QString("InvalidInput"));
        QCOMPARE(invalidated.count(), 0);
        remove["confirmed"] = true;
        remove["confirmation"] = "DELETE";
        QCOMPARE(call(remove).value("status").toString(), QString("Ready"));
        QVERIFY(!invalidated.isEmpty());
        QCOMPARE(call({{"operation", "list"}, {"scope", 1}}).value("records").toList().size(), 0);
        QCOMPARE(controller->memoryEntryCount(), 0);
        auto restore = base;
        restore["operation"] = "import";
        restore["confirmed"] = true;
        restore["conflict"] = "reject";
        restore["archive"] = exported.value("archive");
        QCOMPARE(call(restore).value("status").toString(), QString("Ready"));
        QCOMPARE(controller->memoryEntryCount(), 1);
        QCOMPARE(call({{"operation", "list"}, {"scope", 3}}).value("status").toString(),
                 QString("Unauthorized"));
    }

    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName("sentinel-phase3-test-" +
                                             QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    void cleanupTestCase() {
        for (auto location : {QStandardPaths::AppDataLocation, QStandardPaths::AppConfigLocation}) {
            const auto path = QStandardPaths::writableLocation(location);
            if (QDir(path).exists()) {
                QVERIFY(QDir(path).removeRecursively());
            }
        }
    }
    void delayedInitialHistoryHasExplicitReadiness_data() {
        QTest::addColumn<bool>("switchSession");
        QTest::newRow("old-session-response") << true;
        QTest::newRow("old-run-response") << false;
    }
    void delayedInitialHistoryHasExplicitReadiness() {
        QFETCH(bool, switchSession);
        RuntimePeer peer;
        DaemonClient transport(peer.path, 3000, 1000);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QVERIFY(peer.connect(transport));
        QTRY_VERIFY(([&] {
            peer.pump();
            return !peer.historyRequest.isEmpty();
        })());
        QVERIFY(adapter.ready());
        QVERIFY(!adapter.historyLoaded());
        QVERIFY(adapter.messages().isEmpty());
        peer.replyHistory();
        QTRY_VERIFY(adapter.historyLoaded());
        QCOMPARE(adapter.messages().size(), 2);
        QCOMPARE(adapter.messages().at(0).content, QString("First recorded message"));
        QCOMPARE(adapter.messages().at(1).content, QString("Second recorded message"));
        adapter.refresh();
        QTRY_VERIFY(([&] {
            peer.pump();
            return !peer.historyRequest.isEmpty();
        })());
        const auto previousSessionRequest = peer.historyRequest;
        peer.historyRequest = {};
        const auto targetSession =
            switchSession ? QStringLiteral("other-session") : QStringLiteral("fixture-session");
        peer.snapshot["session_id"] = targetSession;
        peer.snapshot["run_id"] = "other-run";
        if (switchSession) {
            QVERIFY(adapter.dispatch("switchConversation", {targetSession}).toBool());
        } else {
            adapter.refresh();
        }
        QTRY_VERIFY(([&] {
            peer.pump();
            return adapter.sessionId() == targetSession && !peer.historyRequest.isEmpty();
        })());
        QVERIFY(!adapter.historyLoaded());
        const int retainedMessages = switchSession ? 0 : 2;
        QCOMPARE(adapter.messages().size(), retainedMessages);
        const auto currentSessionRequest = peer.historyRequest;
        QSignalSpy replies(&transport, &DaemonClient::responseReceived);
        peer.historyRequest = previousSessionRequest;
        peer.replyHistory();
        QTRY_COMPARE(replies.size(), 1);
        QVERIFY(!adapter.historyLoaded()); // An old session/run cannot complete the new barrier.
        QCOMPARE(adapter.messages().size(), retainedMessages);
        if (!switchSession) {
            QCOMPARE(adapter.messages().at(0).content, QString("First recorded message"));
            QCOMPARE(adapter.messages().at(1).content, QString("Second recorded message"));
        }
        peer.historyRequest = currentSessionRequest;
        peer.replyHistory();
        QTRY_VERIFY(adapter.historyLoaded());
        QCOMPARE(adapter.messages().size(), 2);
        transport.disconnectFromDaemon();
        QVERIFY(!adapter.historyLoaded());
    }
    // The greeting/welcome screen depends on conversationHistoryMessageCount
    // staying at zero while a session only holds the startup system notice.
    void systemOnlyHistoryKeepsGreetingVisible() {
        RuntimePeer peer;
        DaemonClient transport(peer.path, 3000, 1000);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QVERIFY(peer.connect(transport));
        QTRY_VERIFY(([&] {
            peer.pump();
            return !peer.historyRequest.isEmpty();
        })());
        peer.replySystemOnlyHistory();
        QTRY_VERIFY(adapter.historyLoaded());
        QCOMPARE(adapter.messages().size(), 1);
        QCOMPARE(adapter.value(QStringLiteral("conversationHistoryMessageCount")).toInt(), 0);
    }

    void terminalNotificationSurvivesReattach_data() {
        QTest::addColumn<QString>("state");
        QTest::addColumn<bool>("switchSession");
        QTest::addColumn<bool>("reenter");
        QTest::newRow("denied") << QString("failed") << false << false;
        QTest::newRow("cancelled") << QString("cancelled") << false << false;
        QTest::newRow("completed") << QString("completed") << false << false;
        QTest::newRow("different-session-discards-pending") << QString("failed") << true << false;
        QTest::newRow("reentrant-refresh-not-duplicated") << QString("failed") << false << true;
    }
    void terminalNotificationSurvivesReattach() {
        QFETCH(QString, state);
        QFETCH(bool, switchSession);
        QFETCH(bool, reenter);
        RuntimePeer peer;
        peer.holdHistory = false;
        DaemonClient transport(peer.path, 3000, 1000);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        sentinel::desktop::QuickPanelController panel(adapter);
        QSignalSpy notices(&panel, &sentinel::desktop::QuickPanelController::notificationRequested);
        if (reenter) {
            connect(&panel, &sentinel::desktop::QuickPanelController::notificationRequested,
                    &adapter, [&] { adapter.refresh(); });
        }
        QVERIFY(peer.connect(transport));
        QTRY_VERIFY(([&] {
            peer.pump();
            return adapter.ready() && adapter.historyLoaded();
        })());
        QCOMPARE(notices.size(), 0);
        adapter.refresh(); // Withhold the attach reply, making ready() false.
        QVERIFY(!adapter.ready());
        peer.snapshot["state"] = state;
        peer.snapshot["event_sequence"] = 2;
        transport.eventReceived(state == "failed"      ? "run.failed"
                                : state == "cancelled" ? "run.cancelled"
                                                       : "run.completed",
                                peer.snapshot);
        QCOMPARE(adapter.value("conversationState").toString(), state);
        QCOMPARE(notices.size(), 0); // No delivery during the unavailable state.
        if (switchSession) {
            peer.snapshot["session_id"] = "other-session";
            peer.snapshot["run_id"] = "other-run";
            QTRY_VERIFY(([&] {
                peer.pump();
                return adapter.ready() && adapter.sessionId() == "other-session";
            })());
            QCOMPARE(notices.size(), 0); // Never deliver an old run into a different session.
            return;
        }
        QTRY_VERIFY(([&] {
            peer.pump();
            return adapter.ready();
        })());
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().at(2).toString(), QString("fixture-session"));
        QVERIFY(notices.first().at(0).toString().contains(state));
        // Replaying the terminal snapshot or a late success cannot duplicate
        // the notification or turn denial/cancellation into success.
        adapter.refresh();
        QTRY_VERIFY(([&] {
            peer.pump();
            return adapter.ready();
        })());
        transport.eventReceived("run.completed", {{"session_id", "fixture-session"},
                                                  {"run_id", "fixture-run"},
                                                  {"server_generation", "fixture-generation"},
                                                  {"event_sequence", 3},
                                                  {"state", "completed"}});
        QCOMPARE(notices.size(), 1);
        QCOMPARE(adapter.value("conversationState").toString(), state);
    }
    void extensionsDesktopMcpRealProcessDisconnectAndRecovery() {
        sentinel::core::AppSettings settings(std::make_unique<sentinel::core::InMemorySettingsStore>(),
                                             sentinel::core::inMemoryTestCredentialStore());
        QTemporaryDir directory{QDir::tempPath() + "/em-XXXXXX"};
        sentinel::test::DeterministicModelServiceFixture models;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
            .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
            .withAgentRuntime(std::make_unique<sentinel::core::NullAgentRuntime>()).build();
        auto* runtime = dynamic_cast<sentinel::core::AgentRuntime*>(controller->agentRuntime());
        QVERIFY(runtime);
        auto mcp = std::make_shared<sentinel::core::McpService>();
        sentinel::core::McpServerConfig config;
        config.name = "desktop-hub-process";
        config.type = "local";
        config.command = QString::fromUtf8(TEST_MCP_SERVER_PATH);
        config.arguments = {"", "--certification"};
        QVERIFY(mcp->addServer(config));
        sentinel::core::McpToolProvider provider(mcp, runtime->toolRegistry());
        runtime->extensionService().setMcpService(mcp.get());
        QVERIFY(mcp->connectToServer(config.name));
        sentinel::daemon::DaemonIpcServer server(controller.get());
        server.setSettings(&settings);
        QVERIFY(server.startServer(directory.filePath("extensions.sock")));
        DaemonClient transport(directory.filePath("extensions.sock"), 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QTRY_VERIFY(adapter.ready());
        auto selected = [&] {
            const auto snapshot = adapter.settingsService("extensionsHubState", {}, true).toMap();
            for (const auto& value : snapshot.value("entries").toList()) {
                if (value.toMap().value("id").toString() == "mcp:desktop-hub-process") return value.toMap();
            }
            return QVariantMap{};
        };
        QTRY_VERIFY(selected().value("available").toBool());
        QCOMPARE(selected().value("toolCount").toInt(), 8);
        QVERIFY(adapter.performExtensionsHubAction("mcp:desktop-hub-process", "disconnect"));
        QTRY_COMPARE(adapter.extensionsHubActionState().value("phase").toString(), QString("accepted"));
        QTRY_COMPARE(selected().value("connection").toString(), QString("Disconnected"));
        QVERIFY(!selected().value("available").toBool());
        QVERIFY(mcp->tools().isEmpty());
        QVERIFY(mcp->connectToServer(config.name));
        adapter.refreshExtensionsHub();
        QTRY_VERIFY(selected().value("available").toBool());
        const auto persistedConfig = settings.mcpServersJson();
        QVERIFY(adapter.performExtensionsHubAction("mcp:desktop-hub-process", "disable"));
        QTRY_COMPARE(adapter.extensionsHubActionState().value("phase").toString(), QString("accepted"));
        QTRY_COMPARE(selected().value("health").toString(), QString("Disabled"));
        QCOMPARE(settings.mcpServersJson(), persistedConfig);
    }
    void extensionsLiveInventoryAndSafeSkillDeactivation() {
        sentinel::core::AppSettings settings(std::make_unique<sentinel::core::InMemorySettingsStore>(),
                                             sentinel::core::inMemoryTestCredentialStore());
        QTemporaryDir directory{QDir::tempPath() + "/ex-XXXXXX"};
        sentinel::test::DeterministicModelServiceFixture models;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
            .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
            .withAgentRuntime(std::make_unique<sentinel::core::NullAgentRuntime>()).build();
        auto* runtime = dynamic_cast<sentinel::core::AgentRuntime*>(controller->agentRuntime());
        QVERIFY(runtime);
        sentinel::core::Skill skill;
        skill.name = "hub-live-fixture";
        skill.content = "Private fixture instructions";
        QVERIFY(runtime->skillService().addSkill(skill));
        const auto cleanup = qScopeGuard([&] { runtime->skillService().removeSkill(skill.name); });
        sentinel::daemon::DaemonIpcServer server(controller.get());
        server.setSettings(&settings);
        QVERIFY(server.startServer(directory.filePath("extensions.sock")));
        DaemonClient transport(directory.filePath("extensions.sock"), 2000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QTRY_VERIFY(adapter.ready());
        auto inventory = [&] { return adapter.settingsService("extensionsHubState", {}, true).toMap(); };
        QTRY_VERIFY(inventory().contains("entries"));
        auto selected = [&] {
            for (const auto& row : inventory().value("entries").toList()) {
                if (row.toMap().value("id").toString() == "skill:hub-live-fixture") return row.toMap();
            }
            return QVariantMap{};
        };
        QVERIFY(selected().value("available").toBool());
        QVERIFY(!adapter.performExtensionsHubAction("skill:hub-live-fixture", "enable"));
        QVERIFY(!adapter.performExtensionsHubAction("skill:missing", "disable"));
        QVERIFY(adapter.performExtensionsHubAction("skill:hub-live-fixture", "disable"));
        QVERIFY(!adapter.performExtensionsHubAction("skill:hub-live-fixture", "disable"));
        QTRY_COMPARE(adapter.extensionsHubActionState().value("phase").toString(), QString("accepted"));
        QTRY_VERIFY(selected().value("health").toString() == "Disabled");
        QVERIFY(!selected().value("available").toBool());
        QVERIFY(runtime->skillService().getSkillContent(skill.name).isEmpty());
        transport.disconnectFromDaemon();
        QCOMPARE(adapter.extensionsHubActionState().value("phase").toString(), QString("disconnected"));
    }
    void extensionsRejectStaleReadsAndSettleActions_data() {
        QTest::addColumn<QString>("outcome");
        QTest::newRow("accepted") << QString("accepted");
        QTest::newRow("rejected") << QString("failed");
        QTest::newRow("timeout") << QString("unconfirmed");
    }
    void extensionsRejectStaleReadsAndSettleActions() {
        QFETCH(QString, outcome);
        RuntimePeer peer;
        peer.holdHistory = false;
        peer.holdExtensions = true;
        DaemonClient transport(peer.path, 1000, 1000);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QVERIFY(peer.connect(transport));
        QTRY_VERIFY((peer.pump(), adapter.ready()));
        auto inventory = [&] { return adapter.settingsService("extensionsHubState", {}, true).toMap(); };
        inventory();
        QTRY_VERIFY((peer.pump(), !peer.extensionsRequest.isEmpty()));
        const auto old = peer.extensionsRequest;
        peer.extensionsRequest = {};
        adapter.refreshExtensionsHub();
        inventory();
        QTRY_VERIFY((peer.pump(), !peer.extensionsRequest.isEmpty()));
        auto reply = [&](const QJsonObject& request, const QString& name) {
            send(peer.socket, frame("response", request.value("id").toString(), "desktop.settings_service",
                {{"result", QJsonObject{{"entries", QJsonArray{QJsonObject{{"id", "mcp:fixture"},
                    {"type", "mcp"}, {"name", name}, {"actions", QJsonArray{"disconnect"}}}}},
                    {"toolEntries", QJsonArray{}}}}}));
        };
        reply(peer.extensionsRequest, "Newest inventory");
        QTRY_VERIFY(inventory().contains("entries"));
        reply(old, "Stale inventory");
        reply(peer.extensionsRequest, "Duplicate inventory");
        QVERIFY(adapter.performExtensionsHubAction("mcp:fixture", "disconnect"));
        QVERIFY(!adapter.performExtensionsHubAction("mcp:fixture", "disconnect"));
        QTRY_VERIFY((peer.pump(), !peer.extensionActionRequest.isEmpty()));
        QCOMPARE(inventory().value("entries").toList().first().toMap().value("name").toString(), QString("Newest inventory"));
        if (outcome != "unconfirmed") {
            send(peer.socket, frame("response", peer.extensionActionRequest.value("id").toString(),
                "desktop.settings_service", {{"result", QJsonObject{{"accepted", outcome == "accepted"}}}}));
        }
        QTRY_COMPARE((peer.pump(), adapter.extensionsHubActionState().value("phase").toString()), outcome);
        // A timed-out mutation is never replayed; a late acceptance cannot rewrite its visible outcome.
        send(peer.socket, frame("response", peer.extensionActionRequest.value("id").toString(),
            "desktop.settings_service", {{"result", QJsonObject{{"accepted", true}}}}));
        peer.extensionsRequest = {};
        inventory();
        QTRY_VERIFY((peer.pump(), !peer.extensionsRequest.isEmpty()));
        reply(peer.extensionsRequest, "Post-action inventory");
        QTRY_VERIFY(inventory().contains("entries"));
        QCOMPARE(adapter.extensionsHubActionState().value("phase").toString(), outcome);
        transport.disconnectFromDaemon();
        QCOMPARE(adapter.extensionsHubActionState().value("phase").toString(), QString("disconnected"));
    }
    void extensionsReadExistingSettingsWithoutExecuting() {
        sentinel::core::AppSettings settings(std::make_unique<sentinel::core::InMemorySettingsStore>(),
                                             sentinel::core::inMemoryTestCredentialStore());
        QTemporaryDir directory{QDir::tempPath() + "/ext-XXXXXX"};
        QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner |
                                     QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        sentinel::test::DeterministicModelServiceFixture models;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
            .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
            .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>()).build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        server.setSettings(&settings);
        QVERIFY(server.startServer(directory.filePath("extensions.sock")));
        DaemonClient transport(directory.filePath("extensions.sock"), 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QTRY_VERIFY(adapter.ready());
        QTRY_VERIFY(adapter.historyLoaded());
        const auto before = adapter.messages().size();
        auto readInventory = [&] { return adapter.settingsService("productSettings", {}, true); };
        QTRY_VERIFY(readInventory().toMap().contains("value"));
        const auto result = sentinel::desktop::ExtensionsHubPresentation::inventory(true, true,
            readInventory().toMap(), adapter.value("availableToolIds").toStringList(), {});
        QCOMPARE(result.value("phase").toString(), "ready");
        adapter.refreshExtensionsHub();
        QTRY_VERIFY(readInventory().toMap().contains("value"));
        auto missing = [&] { return adapter.settingsService("extensionSettingsState", {"plugin:not-installed"}, true); };
        QTRY_VERIFY(missing().isValid());
        QCOMPARE(sentinel::desktop::ExtensionsHubPresentation::detail(true, "plugin:not-installed",
            missing().toMap(), true).value("phase").toString(), "unavailable");
        QCOMPARE(adapter.messages().size(), before);
        transport.disconnectFromDaemon();
        QVERIFY(!adapter.ready());
        const auto offline = sentinel::desktop::ExtensionsHubPresentation::inventory(false, false,
            readInventory().toMap(), {"stale.tool"}, {});
        QVERIFY(offline.value("tools").toStringList().isEmpty());
    }
    void modelCatalogFiltersAndSortsReportedMetadata() {
        QFile script(":/catalog-test/ModelCatalog.js");
        QVERIFY(script.open(QIODevice::ReadOnly));
        QJSEngine engine;
        const auto loaded = engine.evaluate(QString::fromUtf8(script.readAll()));
        QVERIFY2(!loaded.isError(), qPrintable(loaded.toString()));
        const auto check = [&](const QString& expression) {
            const auto result = engine.evaluate(expression);
            return !result.isError() && result.toBool();
        };
        QVERIFY(check("matchesSource({catalogSource:'lmstudio', provider:'Meta'}, 'lmstudio')"));
        QVERIFY(check("!matchesSource({catalogSource:'lmstudio', provider:'Meta'}, 'ollama')"));
        QVERIFY(check("matchesSource({gguf:true}, 'llamacpp') && "
                      "!matchesSource({format:'safetensors'}, 'llamacpp')"));
        QVERIFY(check("matchesCategory({category:'Vision',tags:['reasoning']}, 'Think')"));
        QVERIFY(check("!matchesCategory({category:'Video',tags:['text-to-video']}, 'Image')"));
        QVERIFY(
            check("matchesSearch({name:'LocateAnything-Q4.gguf',gguf:true}, 'locate-anything')"));
        QVERIFY(check("matchesSearch({repositoryId:'publisher/hidden-name'}, 'hidden-name')"));
        QVERIFY(check("matchesSearch({tags:['multilingual']}, 'multilingual')"));
        QVERIFY(check("!matchesSearch({name:'Alpha'}, 'missing')"));
        engine.evaluate(
            "var models = [{id:'unknown',name:'Alpha'}, "
            "{id:'zero',name:'Beta',downloads:0,sizeBytes:100,lastUpdated:'2026-01-01'}, "
            "{id:'largest',name:'Gamma',downloads:100,sizeBytes:200,lastUpdated:'2026-10-08'}, "
            "{id:'bad',name:'Delta',downloads:'999',sizeBytes:-1,lastUpdated:'invalid'}]; var "
            "flags = [false,true,false,false]; function ids(mode) { return "
            "sortModels(models,mode,flags).map(function(m){return m.id}).join(','); }");
        QCOMPARE(engine.evaluate("ids('installed')").toString(),
                 QString("zero,unknown,largest,bad"));
        QCOMPARE(engine.evaluate("ids('name')").toString(), QString("unknown,zero,bad,largest"));
        QCOMPARE(engine.evaluate("ids('nameDesc')").toString(),
                 QString("largest,bad,zero,unknown"));
        QCOMPARE(engine.evaluate("ids('downloads')").toString(),
                 QString("largest,zero,unknown,bad"));
        QCOMPARE(engine.evaluate("ids('updated')").toString(), QString("largest,zero,unknown,bad"));
        QCOMPARE(engine.evaluate("ids('size')").toString(), QString("zero,largest,unknown,bad"));
        QCOMPARE(engine.evaluate("models[0].id").toString(), QString("unknown"));
    }
    void modelVariantsShareCardsWithoutLosingArtifactIdentity() {
        QFile script(":/catalog-test/ModelCatalog.js");
        QVERIFY(script.open(QIODevice::ReadOnly));
        QJSEngine engine;
        QVERIFY(!engine.evaluate(QString::fromUtf8(script.readAll())).isError());
        const auto result = engine.evaluate(R"JS(
            var source = [
                {id:'small', name:'Qwen 2.5 7B', ollamaId:'qwen2.5:7b', category:'LLM'},
                {id:'large', name:'Qwen 2.5 14B', ollamaId:'qwen2.5:14b', category:'LLM'},
                {id:'coder', name:'Qwen 2.5 Coder 7B', ollamaId:'qwen2.5-coder:7b', category:'LLM'},
                {id:'next', name:'Qwen 3 7B', ollamaId:'qwen3:7b', category:'LLM'},
                {id:'q4', name:'Alpha 8B', repositoryId:'publisher/Alpha-8B-GGUF', filename:'Alpha-Q4_K_M.gguf', gguf:true},
                {id:'q8', name:'Alpha 8B', repositoryId:'publisher/Alpha-8B-GGUF', filename:'Alpha-Q8_0.gguf', gguf:true},
                {id:'other', name:'Alpha 8B', repositoryId:'other/Alpha-8B-GGUF', gguf:true}
            ];
            var grouped = groupModels(source);
            grouped.length === 5 && grouped[0].variants.length === 2 &&
            grouped[0].familyName === 'Qwen 2.5' &&
            grouped[0].variants[1].ollamaId === 'qwen2.5:14b' &&
            grouped[3].variants[1].filename === 'Alpha-Q8_0.gguf' &&
            grouped[3].variants[1].variantLabel === 'Alpha-Q8_0.gguf' &&
            source[0].variants === undefined && groupModels([]).length === 0 &&
            groupModels([source[0],source[0]]).length === 1 &&
            groupModels([source[0],source[0]])[0].variants.length === 1 &&
            matchesSearch(source[1], '14b');
        )JS");
        QVERIFY2(!result.isError(), qPrintable(result.toString()));
        QVERIFY(result.toBool());
    }

    void deletingConversationsRefreshesHistoryAndRebindsActiveSession() {
        QTemporaryDir directory{QDir::tempPath() + "/delete-XXXXXX"};
        sentinel::test::DeterministicModelServiceFixture models;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        QVERIFY(server.startServer(directory.filePath("daemon.sock")));
        DaemonClient transport(directory.filePath("daemon.sock"), 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QSignalSpy deletions(&adapter, &sentinel::desktop::DesktopRuntimeClient::conversationDeleteCompleted);
        sentinel::desktop::DesktopControllerBridge bridge({nullptr, &adapter});
        QSignalSpy failures(&adapter, &sentinel::desktop::DesktopRuntimeClient::operationFailed);
        QTRY_VERIFY(adapter.ready());
        const auto first = bridge.activeConversationId();
        QVERIFY(!first.isEmpty());
        bridge.createConversation(QStringLiteral("Deletion regression"));
        QTRY_VERIFY(bridge.activeConversationId() != first);
        const auto second = bridge.activeConversationId();
        QVERIFY(bridge.requestPermanentDeleteConversation(first));
        QTRY_VERIFY(!bridge.conversationIds().contains(first));
        QCOMPARE(deletions.size(), 1);
        QCOMPARE(deletions.first().at(0).toString(), first);
        QVERIFY(deletions.first().at(1).toBool());
        QCOMPARE(bridge.activeConversationId(), second);
        QVERIFY(bridge.requestPermanentDeleteConversation(second));
        QTRY_VERIFY(!bridge.conversationIds().contains(second));
        QTRY_VERIFY(!bridge.activeConversationId().isEmpty() && bridge.activeConversationId() != second);
        const auto replacement = bridge.activeConversationId();
        QVERIFY(bridge.renameConversation(replacement, QStringLiteral("Still usable")));
        QTRY_VERIFY(bridge.conversationTitles().contains("Still usable"));
        QCOMPARE(failures.size(), 0);
        QVERIFY(bridge.requestPermanentDeleteConversation(QStringLiteral("missing-conversation")));
        QTRY_COMPARE(deletions.size(), 3);
        QVERIFY(!deletions.last().at(1).toBool());
        QVERIFY(!deletions.last().at(2).toString().isEmpty());
    }
    void onboardingProgressPersistsWithoutDaemon() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DaemonClient client(directory.filePath("absent.sock"), 1000, 1000);
        sentinel::desktop::DesktopRuntimeClient runtime(client);
        const auto path = directory.filePath("preferences.json");
        {
            sentinel::core::AppSettings settings(
                std::make_unique<sentinel::desktop::DesktopSettingsStore>(runtime, path));
            sentinel::core::OnboardingService onboarding(settings, nullptr);
            QCOMPARE(onboarding.snapshot().step, sentinel::core::OnboardingStep::Welcome);
            QVERIFY(onboarding.advance().accepted);
            QCOMPARE(onboarding.snapshot().step, sentinel::core::OnboardingStep::ProcessingMode);
        }
        sentinel::core::AppSettings reopened(
            std::make_unique<sentinel::desktop::DesktopSettingsStore>(runtime, path));
        sentinel::core::OnboardingService onboarding(reopened, nullptr);
        QCOMPARE(onboarding.snapshot().step, sentinel::core::OnboardingStep::ProcessingMode);
        QVERIFY(onboarding.back().accepted);
        QCOMPARE(onboarding.snapshot().step, sentinel::core::OnboardingStep::Welcome);
        QVERIFY(!client.daemonReachable());
    }
    void disconnectSettlesPendingRequestsOnceAndRejectsLateReplies() {
        Peer peer;
        DaemonClient client(peer.path, 1000, 1000);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        QSignalSpy failed(&client, &DaemonClient::requestFailed);
        const auto first =
            client.request(DaemonClient::Command::session_create, {{"title", "pending"}});
        const auto second = client.request(DaemonClient::Command::model_current);
        read(peer.socket);
        read(peer.socket);
        client.disconnectFromDaemon();
        client.disconnectFromDaemon();
        int firstCount = 0, secondCount = 0;
        for (const auto& failure : failed) {
            if (failure[0].toString() == first || failure[0].toString() == second) {
                QCOMPARE(failure[1].value<DaemonClient::Error>(),
                         DaemonClient::Error::RequestCancelled);
            }
            if (failure[0].toString() == first) {
                ++firstCount;
            }
            if (failure[0].toString() == second) {
                ++secondCount;
            }
        }
        QCOMPARE(firstCount, 1);
        QCOMPARE(secondCount, 1);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        QSignalSpy replies(&client, &DaemonClient::responseReceived);
        send(peer.socket, frame("response", second, "model.current",
                                {{"provider_id", "stale"}, {"model_id", "stale"}}));
        const auto barrier = client.request(DaemonClient::Command::daemon_status);
        peer.status();
        QTRY_VERIFY(!replies.isEmpty());
        QTRY_COMPARE(replies.last()[0].toString(), barrier);
        for (const auto& response : replies) {
            QVERIFY(response[0].toString() != second);
        }
    }
    void handshakeStatusAndDisconnect() {
        Peer peer;
        DaemonClient client(peer.path, 1000, 1000);
        QSignalSpy status(&client, &DaemonClient::statusReceived);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        QCOMPARE(client.serverGeneration(), QString("fixture-generation"));
        QCOMPARE(client.connectionEpoch(), quint64(1));
        peer.status();
        QTRY_COMPARE(status.size(), 1);
        client.disconnectFromDaemon();
        QCOMPARE(client.connectionState(), DaemonClient::ConnectionState::Disconnected);
        QVERIFY(!client.daemonReachable());
    }
    void olderServerWithoutGeneration() {
        Peer peer;
        DaemonClient client(peer.path, 1000, 1000);
        QVERIFY(peer.connect(client, 1, false));
        QTRY_VERIFY(client.daemonReachable());
        QVERIFY(client.serverGeneration().isEmpty());
        QCOMPARE(client.connectionEpoch(), quint64(1));
        peer.status();
    }
    void invalidRequestIsRejectedLocally() {
        Peer peer;
        DaemonClient client(peer.path, 1000, 1000);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        QSignalSpy errors(&client, &DaemonClient::requestFailed);
        const auto id =
            client.request(DaemonClient::Command::chat_send, {{"session_id", "s"}, {"text", 123}});
        QTRY_VERIFY(!errors.isEmpty());
        QCOMPARE(errors.last().at(0).toString(), id);
        QCOMPARE(errors.last().at(1).value<DaemonClient::Error>(),
                 DaemonClient::Error::InvalidRequest);
        QVERIFY(!peer.socket->canReadLine());
    }
    void unavailable() {
        QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
        DaemonClient client(directory.filePath("absent.sock"), 1000, 1000);
        QTRY_COMPARE(client.connectionState(), DaemonClient::ConnectionState::Unavailable);
        QSignalSpy errors(&client, &DaemonClient::requestFailed);
        const auto id = client.request(DaemonClient::Command::model_current);
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(errors.at(0).at(0).toString(), id);
        QCOMPARE(errors.at(0).at(1).value<DaemonClient::Error>(),
                 DaemonClient::Error::DaemonUnavailable);
        client.disconnectFromDaemon();
    }
    void incompatibleMajor() {
        Peer peer;
        DaemonClient client(peer.path, 1000, 10);
        QVERIFY(peer.connect(client, 2));
        QTRY_COMPARE(client.connectionState(), DaemonClient::ConnectionState::VersionMismatch);
        QVERIFY(!client.daemonReachable());
        QTest::qWait(50);
        QVERIFY(!peer.server.hasPendingConnections());
    }
    void requestCorrelationAndDuplicateSuppression() {
        Peer peer;
        DaemonClient client(peer.path, 1000, 1000);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        QSignalSpy replies(&client, &DaemonClient::responseReceived);
        const auto first = client.request(DaemonClient::Command::model_current);
        const auto second = client.request(DaemonClient::Command::session_list);
        read(peer.socket);
        read(peer.socket);
        send(peer.socket, frame("response", second, "session.list", {{"sessions", QJsonArray{}}}));
        const auto model = frame("response", first, "model.current",
                                 {{"provider_id", "fixture"}, {"model_id", "m"}});
        send(peer.socket, model);
        send(peer.socket, model);
        QTRY_VERIFY(replies.size() >= 2);
        int firstCount = 0, secondCount = 0;
        for (const auto& reply : replies) {
            firstCount += reply.at(0).toString() == first;
            secondCount += reply.at(0).toString() == second;
        }
        QCOMPARE(firstCount, 1);
        QCOMPARE(secondCount, 1);
    }
    void fragmentedFrame() {
        Peer peer;
        DaemonClient client(peer.path, 1000, 1000);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        QSignalSpy events(&client, &DaemonClient::eventReceived);
        const auto bytes =
            QJsonDocument(frame("event", {}, "output.delta",
                                {{"run_id", "r"}, {"session_id", "s"}, {"text", "hello"}}))
                .toJson(QJsonDocument::Compact) +
            '\n';
        peer.socket->write(bytes.left(10));
        QTest::qWait(10);
        QCOMPARE(events.size(), 0);
        peer.socket->write(bytes.mid(10));
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(events.at(0).at(1).toJsonObject().value("text").toString(), QString("hello"));
    }
    void malformedEvent_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("bad-json") << QByteArray("not-json\n");
        QTest::newRow("wrong-payload")
            << (QJsonDocument(frame("event", {}, "output.delta",
                                    {{"run_id", "r"}, {"session_id", "s"}, {"text", 1}}))
                    .toJson(QJsonDocument::Compact) +
                '\n');
        QTest::newRow("oversized-frame") << QByteArray(sentinel::ipc::max_frame_bytes, 'x');
    }
    void malformedEvent() {
        QFETCH(QByteArray, bytes);
        Peer peer;
        DaemonClient client(peer.path, 1000, 1000);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        QSignalSpy errors(&client, &DaemonClient::requestFailed);
        peer.socket->write(bytes);
        QTRY_VERIFY(!client.daemonReachable());
        bool malformed = false;
        for (const auto& error : errors) {
            malformed |=
                error.at(1).value<DaemonClient::Error>() == DaemonClient::Error::MalformedResponse;
        }
        QVERIFY(malformed);
    }
    void timeoutDoesNotReplayMutation() {
        Peer peer;
        DaemonClient client(peer.path, 150, 1000);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        QSignalSpy errors(&client, &DaemonClient::requestFailed);
        const auto id = client.request(DaemonClient::Command::session_create, {{"title", "test"}});
        read(peer.socket);
        QTRY_VERIFY(!errors.isEmpty());
        bool timedOut = false;
        for (const auto& error : errors) {
            timedOut |= error.at(0).toString() == id && error.at(1).value<DaemonClient::Error>() ==
                                                            DaemonClient::Error::RequestTimeout;
        }
        QVERIFY(timedOut);
        QVERIFY(!peer.socket->canReadLine());
    }
    void reconnectInvalidatesPendingIds() {
        Peer peer;
        DaemonClient client(peer.path, 1000, 10);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        const auto oldId = client.request(DaemonClient::Command::model_current);
        read(peer.socket);
        peer.socket->abort();
        QTRY_VERIFY(!client.daemonReachable());
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        QCOMPARE(client.connectionEpoch(), quint64(2));
        QSignalSpy replies(&client, &DaemonClient::responseReceived);
        send(peer.socket, frame("response", oldId, "model.current",
                                {{"provider_id", "stale"}, {"model_id", "stale"}}));
        QTest::qWait(20);
        for (const auto& reply : replies) {
            QVERIFY(reply.at(0).toString() != oldId);
        }
    }
    void daemonInspectorReadsDurableHistoryAndDetails() {
        QTemporaryDir directory{QDir::tempPath() + "/ah-XXXXXX"};
        QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner |
                                                            QFileDevice::WriteOwner |
                                                            QFileDevice::ExeOwner));
        sentinel::test::DeterministicModelServiceFixture models;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller =
            builder.withModelService(models.takeModelService())
                .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                .withAgentRuntime(std::make_unique<sentinel::core::NullAgentRuntime>())
                .build();
        auto* store = controller->mutableAgentRunStore();
        QVERIFY(store);
        const auto prefix = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QString selected;
        for (int i = 0; i < 31; ++i) {
            sentinel::core::AgentEvent event;
            event.turnId = prefix + QString::number(i);
            event.sessionId = prefix;
            event.id = event.turnId + "-start";
            event.timestamp = QDateTime::currentDateTimeUtc().addSecs(300 + i);
            event.type = sentinel::core::AgentEventType::RunStarted;
            event.providerId = "llama-cpp-server";
            event.modelId = "inspector-model";
            event.payload =
                sentinel::core::AgentRunStartedEvent{QStringLiteral("Inspect durable history")};
            QVERIFY(store->record(event));
            event.id = event.turnId + "-end";
            event.type = sentinel::core::AgentEventType::AgentCompleted;
            event.payload = sentinel::core::AgentTextEvent{QStringLiteral("History was persisted")};
            QVERIFY(store->record(event));
            selected = event.turnId;
        }
        sentinel::daemon::DaemonIpcServer server(controller.get());
        QVERIFY(server.startServer(directory.filePath("history.sock")));
        DaemonClient transport(directory.filePath("history.sock"), 3000, 10);
        sentinel::desktop::RemoteAgentInspectorService history(transport);
        sentinel::desktop::AgentInspectorViewModel inspector(history);
        connect(&history, &sentinel::desktop::RemoteAgentInspectorService::updated, &inspector,
                [&] { inspector.refreshFromRemote(history.recentRuns(500), history.hasMore()); });
        QTRY_VERIFY(history.recentRuns(50).size() >= 26);
        QVERIFY(history.error().isEmpty());
        inspector.selectRun(selected);
        QTRY_COMPARE(inspector.selectedRun().value("answer").toString(),
                     QString("History was persisted"));
        QCOMPARE(inspector.selectedRun().value("provider").toString(), QString("llama-cpp-server"));
        QVERIFY(inspector.hasMore());
        inspector.loadMore();
        QTRY_VERIFY(history.recentRuns(500).size() >= 31);
        QVERIFY(inspector.errorMessage().isEmpty());
    }
    void inspectorSelectionDuringPendingRead_data() {
        QTest::addColumn<bool>("failedRead");
        QTest::newRow("late-response") << false;
        QTest::newRow("failed-read") << true;
    }
    void inspectorSelectionDuringPendingRead() {
        QFETCH(bool, failedRead);
        Peer peer;
        DaemonClient transport(peer.path, 2000, 1000);
        sentinel::desktop::RemoteAgentInspectorService history(transport);
        sentinel::desktop::AgentInspectorViewModel inspector(history);
        connect(&history, &sentinel::desktop::RemoteAgentInspectorService::updated, &inspector,
                [&] { inspector.refreshFromRemote(history.recentRuns(500), history.hasMore()); });
        QVERIFY(peer.connect(transport));
        QTRY_VERIFY(transport.daemonReachable());
        auto nextHistory = [&] {
            auto request = read(peer.socket);
            while (request.value("name").toString() == "daemon.status") {
                send(peer.socket, frame("response", request.value("id").toString(),
                                        "daemon.status", {{"running", true}, {"daemon_version", "fixture"},
                                                          {"uptime_ms", 0}, {"active_runs", 0},
                                                          {"sessions", 0}}));
                request = read(peer.socket);
            }
            return request;
        };
        const auto initial = nextHistory();
        QCOMPARE(initial.value("name").toString(), QString("agent.history"));
        inspector.selectRun("old-run");
        inspector.selectRun("new-run");
        QCOMPARE(inspector.selectedRunId(), QString("new-run"));
        QVERIFY(inspector.selectedRun().isEmpty());
        auto reply = [&](const QJsonObject& request, const QString& run) {
            QJsonArray runs;
            for (int i = 0; i < 26; ++i)
                runs.append(QJsonObject{{"runId", QString("history-%1").arg(i)}});
            send(peer.socket, frame("response", request.value("id").toString(), "agent.history",
                 {{"history", QJsonObject{{"runs", runs},
                     {"run", QJsonObject{{"runId", run}, {"state", "completed"},
                                         {"finalAnswer", "Confirmed answer"}}}}}}));
        };
        if (failedRead)
            send(peer.socket, frame("error", initial.value("id").toString(), "agent.history",
                                    {{"code", "request-timeout"}}));
        else
            reply(initial, "old-run");
        const auto selected = nextHistory();
        QCOMPARE(selected.value("payload").toObject().value("run_id").toString(), QString("new-run"));
        QVERIFY(inspector.selectedRun().isEmpty());
        reply(selected, "new-run");
        QTRY_COMPARE(inspector.selectedRun().value("answer").toString(), QString("Confirmed answer"));
        QVERIFY(history.hasMore());
        transport.disconnectFromDaemon();
        QTRY_VERIFY(history.recentRuns(500).isEmpty());
        QVERIFY(!history.hasMore());
        QVERIFY(inspector.selectedRun().isEmpty());
        QCOMPARE(inspector.selectedRunId(), QString("new-run"));
        QVERIFY(!inspector.errorMessage().isEmpty());
        QVERIFY(peer.connect(transport));
        QTRY_VERIFY(transport.daemonReachable());
        const auto reattached = nextHistory();
        QCOMPARE(reattached.value("payload").toObject().value("run_id").toString(), QString("new-run"));
        reply(reattached, "new-run");
        QTRY_COMPARE(inspector.selectedRun().value("answer").toString(), QString("Confirmed answer"));
        QVERIFY(inspector.errorMessage().isEmpty());
    }
    void modelRowsUseAuthoritativeInstallation_data() {
        QTest::addColumn<bool>("missing");
        QTest::newRow("installed-runtime-unverified") << false;
        QTest::newRow("missing-registered-file") << true;
    }
    void modelRowsUseAuthoritativeInstallation() {
        QFETCH(bool, missing);
        QTemporaryDir directory;
        sentinel::test::DeterministicModelServiceFixture models;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        QSet<QString> installedBefore;
        for (const auto& entry : controller->modelLibrary()->entries())
            if (entry.installed == sentinel::core::ModelLibraryInstalledState::Installed)
                installedBefore.insert(entry.localFile.isEmpty()
                    ? entry.provider.id + ":" + entry.nativeModelId
                    : QFileInfo(entry.localFile).canonicalFilePath());
        const auto path = directory.filePath("fixture.gguf");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray header(24, '\0');
        header.replace(0, 4, "GGUF");
        header[4] = 3;
        header[8] = 1;
        QCOMPARE(file.write(header), qint64(24));
        file.close();
        QVERIFY(controller->modelLibrary()->registerLocalGguf(path));
        const auto id = sentinel::core::ModelLibraryService::entryId(
            "llama-cpp-server", "llama-cpp-server", "local-file", QFileInfo(path).canonicalFilePath());
        const auto cleanup = qScopeGuard([&] { controller->modelLibrary()->removeLocalGgufRegistration(id); });
        if (missing) QVERIFY(QFile::remove(path));
        sentinel::daemon::DaemonModelHelpers helper(controller.get());
        QJsonObject projected;
        QTRY_VERIFY(([&] {
            for (const auto& row : helper.state("ggufLibraryFetcher").value("models").toArray())
                if (row.toObject().value("id").toString() == id) projected = row.toObject();
            return !projected.isEmpty();
        })());
        QCOMPARE(projected.value("installed").toBool(), !missing);
        QCOMPARE(helper.state("ggufLibraryFetcher").value("installedCount").toInt(), static_cast<int>(installedBefore.size()) + (missing ? 0 : 1));
    }
    void modelHelperRecoversAfterReadTimeout() {
        Peer peer;
        DaemonClient transport(peer.path, 150, 1000);
        sentinel::desktop::DesktopModelHelper helper(transport, "ggufLibraryFetcher");
        QVERIFY(peer.connect(transport));
        QTRY_VERIFY(transport.daemonReachable());
        // The helper's first request precedes the transport's status refresh.
        auto first = read(peer.socket);
        QCOMPARE(first.value("name").toString(), QString("model.helper_state"));
        peer.status();
        QTRY_COMPARE(helper.errorText(), QString("request-timeout"));
        QVERIFY(!helper.fetching());
        QTRY_VERIFY_WITH_TIMEOUT(peer.socket->canReadLine(), 3000);
        const auto retry = read(peer.socket);
        QCOMPARE(retry.value("name").toString(), QString("model.helper_state"));
        send(peer.socket, frame("response", retry.value("id").toString(), "model.helper_state",
            {{"component", "ggufLibraryFetcher"}, {"properties", QJsonObject{{"models", QJsonArray{}}, {"fetching", false}, {"errorText", ""}}}}));
        QTRY_VERIFY(helper.errorText().isEmpty());
    }
    void modelHelperPublishesChangedPagesWithoutRepeatingIdenticalSnapshots() {
        Peer peer;
        DaemonClient transport(peer.path, 5000, 1000);
        sentinel::desktop::DesktopModelHelper helper(transport, "ggufLibraryFetcher");
        QSignalSpy changed(&helper, &sentinel::desktop::DesktopModelHelper::changed);
        QVERIFY(peer.connect(transport));
        QTRY_VERIFY(transport.daemonReachable());
        const auto nextStateRequest = [&]() {
            for (int i = 0; i < 10; ++i) {
                const auto request = read(peer.socket);
                const auto name = request.value("name").toString();
                if (name == "model.helper_state")
                    return request;
                send(peer.socket, frame("response", request.value("id").toString(), name,
                                        name == "model.helper_action"
                                            ? QJsonObject{{"accepted", true}}
                                            : QJsonObject{{"running", true},
                                                          {"daemon_version", "fixture"},
                                                          {"uptime_ms", 0},
                                                          {"active_runs", 0},
                                                          {"sessions", 0}}));
            }
            return QJsonObject{};
        };
        const auto publish = [&](const QJsonObject& request, const QString& id) {
            send(peer.socket,
                 frame("response", request.value("id").toString(), "model.helper_state",
                       {{"component", "ggufLibraryFetcher"},
                        {"properties",
                         QJsonObject{{"models", QJsonArray{QJsonObject{{"id", id}, {"name", id}}}},
                                     {"hasMore", true},
                                     {"errorText", ""},
                                     {"fetching", false}}}}));
        };
        publish(nextStateRequest(), "page-one");
        QTRY_COMPARE(helper.models().size(), 1);
        QCOMPARE(helper.models().first().toMap().value("id").toString(), QString("page-one"));
        helper.nextPage();
        const auto next = nextStateRequest();
        publish(next, "page-two");
        QTRY_VERIFY(helper.models().size() == 1 &&
                    helper.models().first().toMap().value("id").toString() == "page-two");
        helper.fetch();
        const auto same = nextStateRequest();
        const auto before = changed.size();
        publish(same, "page-two");
        QTest::qWait(20);
        QVERIFY(changed.size() > before);
        QCOMPARE(helper.models().first().toMap().value("id").toString(), QString("page-two"));
        helper.fetch("fixture-query");
        auto chunkRequest = nextStateRequest();
        QJsonArray first, second;
        for (int i = 0; i < 90; ++i) {
            QJsonObject row{{"id", QString::number(i)}, {"name", QString("Model %1").arg(i)}};
            if (i < 45) first.append(row); else second.append(row);
        }
        send(peer.socket, frame("response", chunkRequest.value("id").toString(), "model.helper_state",
            {{"component", "ggufLibraryFetcher"}, {"properties", QJsonObject{{"models", first}, {"modelOffset", 0}, {"nextOffset", 45}, {"query", "fixture-query"}}}}));
        chunkRequest = nextStateRequest();
        QCOMPARE(chunkRequest.value("payload").toObject().value("offset").toInt(), 45);
        QVERIFY(helper.models().isEmpty());
        send(peer.socket, frame("response", chunkRequest.value("id").toString(), "model.helper_state",
            {{"component", "ggufLibraryFetcher"}, {"properties", QJsonObject{{"models", second}, {"modelOffset", 45}, {"nextOffset", -1}, {"query", "fixture-query"}}}}));
        QTRY_COMPARE(helper.models().size(), 90);
        QCOMPARE(QJsonDocument::fromJson(helper.modelsJson().toUtf8()).array().size(), 90);
        QCOMPARE(helper.models().last().toMap().value("id").toString(), QString("89"));
        const auto json = helper.modelsJson();
        QSignalSpy modelChanges(&helper, &sentinel::desktop::DesktopModelHelper::modelsChanged);
        const auto retained = nextStateRequest(); // The regular status poll reuses its cached models.
        QVERIFY(retained.value("payload").toObject().value("retain_models").toBool());
        send(peer.socket, frame("response", retained.value("id").toString(), "model.helper_state",
            {{"component", "ggufLibraryFetcher"}, {"properties", QJsonObject{{"models", QJsonArray{}},
                {"modelsUnchanged", true}, {"modelOffset", 0}, {"nextOffset", -1}, {"query", "fixture-query"}, {"statusText", "Updated status"}}}}));
        QTRY_COMPARE(helper.statusText(), QString("Updated status"));
        QCOMPARE(helper.models().size(), 90);
        QCOMPARE(helper.modelsJson(), json);
        QCOMPARE(modelChanges.size(), 0);
    }
    void modelCatalogIsProviderIndependentAndMediaTypesAreExplicit() {
        sentinel::test::DeterministicModelServiceFixture models;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonModelHelpers helper(controller.get());
        const auto catalog = helper.state("ggufLibraryFetcher").value("catalog").toArray();
        QVERIFY(catalog.size() > 40);
        QSet<QString> sources;
        int videos = 0;
        QSet<QString> runtimes;
        for (const auto& value : catalog) {
            const auto model = value.toObject();
            const auto source = model.value("externalUrl").toString();
            QVERIFY(!source.isEmpty());
            QVERIFY(!sources.contains(source));
            sources.insert(source);
            if (model.value("category") == "Runtime") {
                runtimes.insert(model.value("id").toString());
                QVERIFY(!model.value("platforms").toString().isEmpty());
                QVERIFY(!model.value("modelFormats").toString().isEmpty());
                QVERIFY(!model.value("integration").toString().isEmpty());
                QVERIFY(!model.value("downloadable").toBool());
                QCOMPARE(QUrl(source).scheme(), QString("https"));
            }
            if (model.value("category") == "Video") {
                ++videos;
                QVERIFY(model.value("ollamaId").toString().isEmpty());
            }
            if (model.value("format") == "MLX")
                QVERIFY(model.value("ollamaId").toString().isEmpty());
        }
        QVERIFY(videos >= 10);
        for (const auto& id :
             {"ollama", "lmstudio", "llama-cpp", "vllm", "mlx-lm", "whisper-cpp", "piper-runtime"})
            QVERIFY(runtimes.contains(id));
        controller->setSelectedRuntimeProvider("llama-cpp-server");
        QCOMPARE(helper.state("ggufLibraryFetcher").value("catalog").toArray(), catalog);
        controller->setSelectedRuntimeProvider("lm-studio");
        QCOMPARE(helper.state("ggufLibraryFetcher").value("catalog").toArray(), catalog);
        QVERIFY(!helper.action("ggufLibraryFetcher", "download", "unknown-artifact", {}));
    }
    void providerExecutionFailureNeverProjectsSuccess() {
        QTemporaryDir directory{QDir::tempPath() + "/pf-XXXXXX"};
        sentinel::test::DeterministicModelServiceFixture models(
            sentinel::test::DeterministicChatReply::Failure);
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        QVERIFY(server.startServer(directory.filePath("daemon.sock")));
        DaemonClient transport(directory.filePath("daemon.sock"), 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        sentinel::desktop::DesktopControllerBridge bridge({nullptr, &adapter});
        QTRY_VERIFY(adapter.ready());
        QVERIFY(bridge.sendMessage(QStringLiteral("Read-only failure fixture")));
        QTRY_COMPARE(bridge.chatSendLifecycleState(), QString("failed"));
        QTRY_VERIFY(!adapter.messages().isEmpty() &&
                    adapter.messages().last().role == sentinel::core::ChatRole::Assistant);
        QCOMPARE(adapter.messages().last().status, sentinel::core::ChatMessageStatus::Failed);
        QVERIFY(!bridge.chatGenerationActive());
        models.state->reply = sentinel::test::DeterministicChatReply::Final;
        QVERIFY(bridge.sendMessage(QStringLiteral("Read-only recovery fixture")));
        QTRY_COMPARE(bridge.chatSendLifecycleState(), QString("completed"));
        QTRY_COMPARE(adapter.messages().last().status, sentinel::core::ChatMessageStatus::Completed);
    }
    void rejectedSendRemainsVisibleAcrossPollingAndRecovers() {
        QTemporaryDir directory{QDir::tempPath() + "/se-XXXXXX"};
        QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner |
                                         QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        sentinel::test::DeterministicModelServiceFixture models;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                                 .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                                 .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                                 .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        QVERIFY(server.startServer(directory.filePath("send.sock")));
        DaemonClient transport(directory.filePath("send.sock"), 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        sentinel::desktop::DesktopControllerBridge bridge({nullptr, &adapter});
        QTRY_VERIFY(adapter.ready());
        controller->modelService()->registerProvider("ollama", [](const sentinel::core::ModelBinding&) {
            return std::shared_ptr<sentinel::core::IChatProvider>{};
        });
        QVERIFY(bridge.sendMessage(QStringLiteral("The server stopped after discovery")));
        QTRY_COMPARE(bridge.chatSendLifecycleState(), QString("failed"));
        QCOMPARE(bridge.chatErrorCategory(), QString("provider-unavailable"));
        QVERIFY(bridge.chatSendLifecycleSummary().contains("server"));
        QVERIFY(!bridge.chatGenerationActive());
        // Explicit refresh includes session.list; a prior idle session cannot hide the error.
        adapter.refresh();
        QTest::qWait(2200);
        QCOMPARE(bridge.chatSendLifecycleState(), QString("failed"));
        QCOMPARE(bridge.chatErrorCategory(), QString("provider-unavailable"));
        controller->modelService()->registerProvider("ollama", [state = models.state](const sentinel::core::ModelBinding&) {
            return std::make_shared<sentinel::test::DeterministicChatProvider>(state);
        });
        QVERIFY(bridge.sendMessage(QStringLiteral("The provider is available again")));
        QTRY_COMPARE(bridge.chatSendLifecycleState(), QString("completed"));
        QVERIFY(bridge.chatErrorCategory().isEmpty());
        QVERIFY(adapter.messages().last().content.contains("SENTINEL_TEST_RESPONSE"));
    }
    void backgroundConversationTitleIsDurableAndProjected_data() {
        QTest::addColumn<bool>("manualRename");
        QTest::addColumn<bool>("shutdownStorage");
        QTest::newRow("generated-title") << false << false;
        QTest::newRow("manual-title-wins") << true << false;
        QTest::newRow("late-title-storage-refused") << false << true;
    }
    void backgroundConversationTitleIsDurableAndProjected() {
        QFETCH(bool, manualRename);
        QFETCH(bool, shutdownStorage);
        auto storageAuthority = std::make_shared<sentinel::core::StorageAdmission>();
        QTemporaryDir directory{QDir::tempPath() + "/st-XXXXXX"};
        QVERIFY(directory.isValid());
        QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner |
                                         QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        auto gate = std::make_shared<QSemaphore>();
        auto started = std::make_shared<std::atomic_bool>(false);
        const auto releaseOnExit = qScopeGuard([gate] { gate->release(); });
        class Provider final : public sentinel::core::IChatProvider {
        public:
            std::shared_ptr<QSemaphore> gate;
            std::shared_ptr<std::atomic_bool> started;
            sentinel::core::ChatProviderReply
            sendRequest(const QString& prompt,
                        const sentinel::core::ChatRequestOptions& options) override {
                Q_ASSERT(options.maxOutputTokens > 0);
                return sendMessage(prompt);
            }
            QString name() const override { return "title-fixture"; }
            sentinel::core::ChatProviderStatus status() const override {
                return sentinel::core::ChatProviderStatus::Ready;
            }
            sentinel::core::ChatProviderReply sendMessage(const QString& prompt) override {
                if (prompt.startsWith("Create a concise conversation title")) {
                    started->store(true);
                    if (!gate->tryAcquire(1, 5000)) return {false, {}, "title timeout"};
                    return {true, "Qt Interface Design", {}};
                }
                return {true, "A useful answer about Qt.", {}};
            }
        };
        sentinel::test::DeterministicModelServiceFixture fixture;
        auto models = fixture.takeModelService();
        models->registerProvider("ollama", [gate, started](const sentinel::core::ModelBinding&) {
            auto provider = std::make_shared<Provider>();
            provider->gate = gate;
            provider->started = started;
            return provider;
        });
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller =
            builder.withModelService(std::move(models))
                .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                .withConversationStore(std::make_unique<sentinel::core::SQLiteConversationStore>(
                    directory.filePath("conversations.sqlite"), storageAuthority))
                .build();
        controller->attachStorageAdmission(storageAuthority);
        sentinel::daemon::DaemonIpcServer server(controller.get());
        QVERIFY(server.startServer(directory.filePath("title.sock")));
        DaemonClient transport(directory.filePath("title.sock"), 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        sentinel::desktop::DesktopControllerBridge bridge({nullptr, &adapter});
        sentinel::desktop::QuickPanelController panel(adapter);
        QTRY_VERIFY(adapter.ready());
        QVERIFY(!bridge.createConversation(QStringLiteral("New Chat")).isEmpty());
        QTRY_COMPARE(adapter.value("activeConversationTitle").toString(), QString("New Chat"));
        const auto sid = adapter.sessionId();
        QVERIFY(panel.ask(QStringLiteral("Help me design a Qt interface")));
        QTRY_COMPARE(bridge.chatSendLifecycleState(), QString("completed"));
        QTRY_VERIFY(started->load());
        // The answer finished and IPC remains usable while title generation waits.
        if (manualRename)
            QVERIFY(controller->renameConversation(sid, QStringLiteral("My chosen title")));
        QString titleBefore;
        for (const auto& record : controller->conversationStore()->listConversations())
            if (record.id == sid)
                titleBefore = record.title;
        if (shutdownStorage)
            storageAuthority->shutdown();
        gate->release();
        if (shutdownStorage) {
            QTRY_COMPARE(storageAuthority->snapshot().writers, 0u);
            for (const auto& record : controller->conversationStore()->listConversations())
                if (record.id == sid)
                    QCOMPARE(record.title, titleBefore);
            return;
        }
        const auto expected = manualRename ? QStringLiteral("My chosen title")
                                           : QStringLiteral("Qt Interface Design");
        QTRY_COMPARE(adapter.value("activeConversationTitle").toString(), expected);
        QTRY_COMPARE(adapter.value("conversationListCurrentTitle").toString(), expected);
        QVERIFY(adapter.value("conversationTitles").toStringList().contains(expected));
        for (const auto& record : controller->conversationStore()->listConversations())
            if (record.id == sid) QCOMPARE(record.title, expected);
    }
    void compatibleFutureEvent() {
        Peer peer;
        DaemonClient client(peer.path, 1000, 1000);
        QVERIFY(peer.connect(client));
        QTRY_VERIFY(client.daemonReachable());
        peer.status();
        QSignalSpy events(&client, &DaemonClient::eventReceived);
        send(peer.socket, frame("event", {}, "future.event", {}, 1, sentinel::ipc::minor + 1));
        QTest::qWait(20);
        QVERIFY(client.daemonReachable());
        QCOMPARE(events.size(), 0);
    }
    void quickPanelRejectsUnboundedLinks_data() {
        QTest::addColumn<QString>("url");
        for (const auto& url :
             {"https://settings", "sentinel://session/../settings", "sentinel://session/a/b",
              "sentinel://user@session/a", "sentinel://session/a?run=1", "sentinel://session/a#x",
              "sentinel://session/%2fetc", "sentinel://run/a", "sentinel://session/",
              "sentinel://settings:42"})
            QTest::newRow(url) << QString::fromLatin1(url);
    }
    void quickPanelRejectsUnboundedLinks() {
        QFETCH(QString, url);
        QVERIFY(sentinel::desktop::QuickPanelController::parseLink(url).isEmpty());
    }
    void modelLibraryProjectionDoesNotReadCloudCredentials() {
        auto credentials = std::make_shared<CountingCredentials>();
        sentinel::core::AppSettings settings(
            std::make_unique<sentinel::core::InMemorySettingsStore>(),
            sentinel::core::CredentialStore(credentials));
        sentinel::core::ModelService models(&settings);
        sentinel::core::ModelLibraryService library(models);
        const auto before = credentials->reads;
        QVERIFY(!library.providerStates().isEmpty());
        library.entries();
        QCOMPARE(credentials->reads, before);
        // Real binding configuration still reads through the authorized credential backend.
        models.providerConfig(sentinel::core::ModelBinding{"gemini", "test-model"});
        QVERIFY(credentials->reads > before);
    }
    void connectedDoesNotMeanReadyUntilSettingsAndSessionArrive() {
        Peer peer;
        DaemonClient transport(peer.path, 5000, 1000);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        QVERIFY(peer.connect(transport));
        QTRY_COMPARE(transport.connectionState(), DaemonClient::ConnectionState::Connected);
        QVERIFY(!adapter.ready());
        QHash<QString, QJsonObject> pending;
        for (int i = 0; i < 4; ++i) {
            const auto request = read(peer.socket);
            pending[request.value("name").toString()] = request;
        }
        const auto reply = [&](const QString& name, QJsonObject payload) {
            send(peer.socket,
                 frame("response", pending.value(name).value("id").toString(), name, payload));
            QTest::qWait(2);
        };
        QJsonObject properties;
        const auto fields = sentinel::ipc::desktopProjectionFields();
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            const auto type = it.value().toObject().value("type").toString();
            properties[it.key()] = type == "string"    ? QJsonValue(QString{})
                                   : type == "boolean" ? QJsonValue(false)
                                   : type == "array"   ? QJsonValue(QJsonArray{})
                                   : type == "object"  ? QJsonValue(QJsonObject{})
                                                       : QJsonValue(0);
        }
        reply("desktop.projection", {{"page", 0}, {"pages", 1}, {"properties", properties}});
        QVERIFY(!adapter.ready());
        reply("desktop.settings", {{"values", QJsonObject{}}});
        QVERIFY(!adapter.ready());
        reply("session.list",
              {{"sessions", QJsonArray{QJsonObject{{"session_id", "readiness-session"}}}}});
        QVERIFY(!adapter.ready());
        auto attach = read(peer.socket);
        QCOMPARE(attach.value("name").toString(), QString("session.attach"));
        send(peer.socket, frame("response", attach.value("id").toString(), "session.attach",
                                {{"session_id", "readiness-session"},
                                 {"state", "idle"},
                                 {"output", ""},
                                 {"run_id", ""},
                                 {"event_sequence", 0},
                                 {"properties", QJsonObject{}}}));
        QTRY_VERIFY(adapter.ready());
        peer.socket->abort();
        QTRY_VERIFY(!adapter.ready());
        QVERIFY(adapter.messages().isEmpty());
    }
    void voicePcmLeaseBoundsPrivacyAndConfigurationChange() {
        QTemporaryDir directory{QDir::tempPath() + "/sv-XXXXXX"};
        QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner |
                                                            QFileDevice::WriteOwner |
                                                            QFileDevice::ExeOwner));
        sentinel::core::AppSettings settings(
            std::make_unique<sentinel::core::InMemorySettingsStore>(),
            sentinel::core::inMemoryTestCredentialStore());
        sentinel::test::DeterministicModelServiceFixture models(
            sentinel::test::DeterministicChatReply::Final, &settings);
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        QVERIFY(server.startServer(directory.filePath("voice.sock")));
        QLocalSocket owner, other;
        const auto connectClient = [&](QLocalSocket& socket) {
            socket.connectToServer(directory.filePath("voice.sock"));
            QVERIFY(socket.waitForConnected(2000));
            send(&socket, frame("request", "hello", "hello",
                                {{"client_id", "voice-test"},
                                 {"major", 1},
                                 {"minor", 1},
                                 {"capabilities", QJsonArray{}}}));
            QCOMPARE(read(&socket).value("type").toString(), QString("response"));
        };
        const auto request = [&](QLocalSocket& socket, const QString& name,
                                 QJsonObject payload = {}) {
            send(&socket, frame("request", name, name, payload));
            return read(&socket);
        };
        connectClient(owner);
        connectClient(other);
        QCOMPARE(request(owner, "voice.action", {{"action", "start"}}).value("type").toString(),
                 QString("error"));
        controller->audioSession()->setSttRuntime(std::make_shared<DictationStt>());
        QCOMPARE(request(owner, "voice.action", {{"action", "start"}}).value("type").toString(),
                 QString("response"));
        QCOMPARE(request(other, "voice.action", {{"action", "start"}})
                     .value("payload")
                     .toObject()
                     .value("code")
                     .toString(),
                 QString("voice-busy"));
        const QJsonObject chunk{{"pcm", "AAA="}, {"final", false}, {"speech", true}};
        QCOMPARE(request(other, "voice.audio", chunk).value("type").toString(), QString("error"));
        QCOMPARE(request(owner, "voice.audio",
                         {{"pcm", "not base64!"}, {"final", true}, {"speech", true}})
                     .value("type")
                     .toString(),
                 QString("error"));
        request(owner, "voice.action", {{"action", "start"}});
        controller->audioSession()->setSttRuntime(std::make_shared<DictationStt>());
        QCOMPARE(request(owner, "voice.audio", chunk)
                     .value("payload")
                     .toObject()
                     .value("code")
                     .toString(),
                 QString("voice-configuration-changed"));
        request(owner, "voice.action", {{"action", "start"}});
        QCOMPARE(request(owner, "voice.audio", {{"pcm", "AAA="}, {"final", true}, {"speech", true}})
                     .value("payload")
                     .toObject()
                     .value("accepted")
                     .toBool(),
                 true);
        QTRY_COMPARE(controller->audioSession()->state(),
                     sentinel::core::VoiceInteractionState::Completed);
        QCOMPARE(request(owner, "voice.state")
                     .value("payload")
                     .toObject()
                     .value("transcript")
                     .toString(),
                 QString("Private dictation"));
        QVERIFY(request(other, "voice.state")
                    .value("payload")
                    .toObject()
                    .value("transcript")
                    .toString()
                    .isEmpty());
        request(owner, "voice.action", {{"action", "start"}});
        request(owner, "voice.audio", chunk);
        owner.abort();
        QTest::qWait(20);
        QCOMPARE(request(other, "voice.action", {{"action", "start"}}).value("type").toString(),
                 QString("response"));
        QVERIFY(request(other, "voice.state")
                    .value("payload")
                    .toObject()
                    .value("transcript")
                    .toString()
                    .isEmpty());
        QVERIFY(!controller->audioSession()->devices()->isCapturing());
        QVERIFY(!controller->audioSession()->privacy().retainRawRecordings);
    }
    void quickPanelConnectionAndLinkProjection() {
        QTemporaryDir directory;
        DaemonClient transport(directory.filePath("absent.sock"), 500, 1000);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        sentinel::desktop::QuickPanelController panel(adapter);
        QTRY_COMPARE(transport.connectionState(), DaemonClient::ConnectionState::Unavailable);
        QCOMPARE(panel.state().value("connection").toString(), QString("Unavailable"));
        QVERIFY(!panel.state().value("ready").toBool());
        QCOMPARE(panel.state().value("approvalCount").toInt(), 0);
        QCOMPARE(panel.state().value("model").toString(), QString("Unavailable"));
        QVERIFY(!panel.ask("No fallback"));
        QVERIFY(!panel.agent("No fallback"));
        QVERIFY(!panel.error().isEmpty());
        QVERIFY(!panel.voiceAction("start"));
        QVERIFY(!panel.state().value("voiceAvailable").toBool());
        QVERIFY(!panel.cancel());
        QVERIFY(!panel.approve(true));
        QSignalSpy open(&panel, &sentinel::desktop::QuickPanelController::openRequested);
        QVERIFY(panel.openLink("sentinel://settings"));
        QCOMPARE(open.last().first().toString(), QString("Settings"));
        QVERIFY(!panel.openLink("sentinel://session/abc-123"));
        QCOMPARE(sentinel::desktop::QuickPanelController::parseLink("sentinel://session/abc-123")
                     .value("sessionId")
                     .toString(),
                 QString("abc-123"));
    }
    void presentationAdapterNoFallback() {
        QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
        DaemonClient transport(directory.filePath("absent.sock"), 500, 1000);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        sentinel::desktop::DesktopControllerBridge bridge({nullptr, &adapter});
        QTRY_COMPARE(transport.connectionState(), DaemonClient::ConnectionState::Unavailable);
        QVERIFY(bridge.isRemote());
        QVERIFY(!bridge.modelService());
        QVERIFY(!bridge.sendMessage(QStringLiteral("Must not run locally")));
        QVERIFY(!bridge.runAgentRequest(QStringLiteral("Must not run locally")));
        QVERIFY(adapter.messages().isEmpty());
        QVERIFY(!bridge.localChatSendAvailable());
    }
    void presentationAdapterChatSessionsCancelRecover() {
        QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
        sentinel::core::AppSettings settings(
            std::make_unique<sentinel::core::InMemorySettingsStore>(),
            sentinel::core::inMemoryTestCredentialStore());
        sentinel::test::DeterministicModelServiceFixture models(
            sentinel::test::DeterministicChatReply::Delayed, &settings);
        auto completionGate = std::make_shared<QSemaphore>();
        models.state->completionGate = completionGate;
        auto releaseOnExit = qScopeGuard([completionGate] { completionGate->release(); });
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        const auto path = directory.filePath("daemon.sock");
        QVERIFY(server.openSessionProjectionStore(directory.filePath("sessions.sqlite3")));
        QVERIFY(server.startServer(path));
        DaemonClient transport(path, 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        sentinel::desktop::DesktopControllerBridge bridge({nullptr, &adapter});
        sentinel::desktop::QuickPanelController panel(adapter);
        QSignalSpy failures(&adapter, &sentinel::desktop::DesktopRuntimeClient::operationFailed);
        QSignalSpy responses(&transport, &DaemonClient::responseReceived);
        QTRY_VERIFY2(
            adapter.ready(),
            qPrintable(transport.statusSummary() + " / " + QString::number(responses.size()) +
                       " responses / " + QString::number(failures.size()) + " failures / " +
                       (failures.isEmpty() ? QString{} : failures.last().first().toString())));
        QTRY_VERIFY(!adapter.sessionId().isEmpty());
        QCOMPARE(bridge.selectedLocalModel(), QString("sentinel-test-model"));
        QCOMPARE(bridge.selectedRuntimeProvider(), QString("ollama"));
        QTRY_VERIFY(!adapter.voiceState().isEmpty());
        QVERIFY(!adapter.voiceState().value("available").toBool());
        QVERIFY(!panel.voiceAction("invalid"));
        const auto first = adapter.sessionId();
        QSignalSpy open(&panel, &sentinel::desktop::QuickPanelController::openRequested);
        panel.continueConversation();
        QCOMPARE(open.last().at(1).toString(), first);
        QVERIFY(panel.state().value("ready").toBool());
        QVERIFY(!panel.state().value("network").toString().isEmpty());
        QVERIFY(panel.ask(QStringLiteral("First turn")));
        QTRY_VERIFY(bridge.chatGenerationActive() && !adapter.runId().isEmpty());
        bridge.setSelectedRuntimeProvider(QStringLiteral("llama-cpp-server"));
        QTRY_COMPARE(bridge.selectedRuntimeProvider(), QString("llama-cpp-server"));
        QCOMPARE(adapter.value("activeChatProviderId").toString(), QString("ollama"));
        bridge.setSelectedRuntimeProvider(QStringLiteral("ollama"));
        QTRY_COMPARE(bridge.selectedRuntimeProvider(), QString("ollama"));
        QVERIFY(panel.cancel());
        QTRY_COMPARE(bridge.chatSendLifecycleState(), QString("cancelled"));
        QVERIFY(!bridge.chatGenerationActive());
        completionGate->release();
        models.state->completionGate.reset();
        models.state->reply = sentinel::test::DeterministicChatReply::Streaming;
        QVERIFY(panel.ask(QStringLiteral("Recovery turn")));
        QTRY_COMPARE(bridge.chatSendLifecycleState(), QString("completed"));
        QTRY_VERIFY(adapter.messages().size() >= 4);
        QCOMPARE(adapter.messages().last().content, QString("SENTINEL_TEST_RESPONSE"));
        const auto savedQuickChat = controller->conversationStore()->loadMessages(first);
        QVERIFY(!savedQuickChat.isEmpty());
        QCOMPARE(savedQuickChat.last().content, QString("SENTINEL_TEST_RESPONSE"));
        QVERIFY(panel.ask(QStringLiteral("Next turn")));
        QTRY_COMPARE(bridge.chatSendLifecycleState(), QString("completed"));
        QTRY_VERIFY(adapter.messages().size() >= 6);
        QVERIFY(!bridge.createConversation(QStringLiteral("Second session")).isEmpty());
        QTRY_VERIFY(adapter.sessionId() != first);
        QVERIFY(bridge.switchConversation(first));
        QTRY_COMPARE(adapter.sessionId(), first);
        QTRY_VERIFY(adapter.messages().size() >= 6);
        transport.disconnectFromDaemon();
        QVERIFY(!panel.ask(QStringLiteral("No fallback")));
        transport.connectToDaemon();
        QTRY_VERIFY(adapter.ready());
        QCOMPARE(adapter.sessionId(), first);
        QVERIFY(!bridge.chatGenerationActive());
        const auto finishedRun = adapter.runId();
        transport.disconnectFromDaemon();
        server.stopServer();
        sentinel::daemon::DaemonIpcServer recovered(controller.get());
        QVERIFY(recovered.openSessionProjectionStore(directory.filePath("sessions.sqlite3")));
        QVERIFY(recovered.startServer(path));
        transport.connectToDaemon();
        QTRY_VERIFY(adapter.ready());
        QTRY_COMPARE(adapter.runId(), finishedRun);
        QCOMPARE(adapter.value("conversationState").toString(), QString("completed"));
    }
    void presentationAdapterAgentApproval_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("allow") << QString("allow");
        QTest::newRow("deny") << QString("deny");
        QTest::newRow("cancel") << QString("cancel");
    }
    void presentationAdapterAgentApproval() {
        QFETCH(QString, action);
        QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
        sentinel::core::AppSettings settings(
            std::make_unique<sentinel::core::InMemorySettingsStore>(),
            sentinel::core::inMemoryTestCredentialStore());
        sentinel::test::DeterministicModelServiceFixture models(
            sentinel::test::DeterministicChatReply::Final, &settings);
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller =
            builder.withModelService(models.takeModelService())
                .withOllamaRuntimeClient(std::make_unique<MetadataClient>())
                .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                .withAgentRuntime(std::make_unique<sentinel::core::NullAgentRuntime>(
                    sentinel::core::NullAgentRuntime::standardTools()))
                .withAgentStepPlanner(std::make_unique<ApprovalPlanner>())
                .withToolExecutor(std::make_unique<FixtureExecutor>())
                .withSandboxPolicy(std::make_unique<sentinel::core::StaticSandboxPolicy>())
                .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        server.setSettings(&settings);
        const auto path = directory.filePath("daemon.sock");
        QVERIFY(server.startServer(path));
        DaemonClient transport(path, 3000, 10);
        sentinel::desktop::DesktopRuntimeClient adapter(transport);
        sentinel::desktop::DesktopControllerBridge bridge({nullptr, &adapter});
        sentinel::desktop::QuickPanelController panel(adapter);
        QSignalSpy notices(&panel, &sentinel::desktop::QuickPanelController::notificationRequested);
        QTRY_VERIFY(adapter.ready());
        QTRY_VERIFY(!adapter.sessionId().isEmpty());
        DaemonClient observerTransport(path, 3000, 10);
        sentinel::desktop::DesktopRuntimeClient observer(observerTransport);
        sentinel::desktop::QuickPanelController observerPanel(observer);
        QTRY_VERIFY(observer.ready() && !observer.sessionId().isEmpty());
        const auto actorSession = adapter.sessionId();
        QVERIFY(!observer.dispatch("createConversation", {QStringLiteral("Observer session")})
                     .toString()
                     .isEmpty());
        QTRY_VERIFY(observer.sessionId() != actorSession);
        QVERIFY(panel.agent(QStringLiteral("Execute fixture action")));
        QTRY_VERIFY(!adapter.pendingApproval().value("approval_id").toString().isEmpty());
        QCOMPARE(panel.state().value("approvalCount").toInt(), 1);
        QCOMPARE(notices.size(), 1);
        observer.refresh();
        QTRY_COMPARE(observerPanel.state().value("approvalCount").toInt(), 1);
        QCOMPARE(observerPanel.state().value("approvalSession").toString(), actorSession);
        const auto approval = adapter.pendingApproval();
        const auto run = adapter.runId();
        transport.eventReceived("output.delta", {{"session_id", adapter.sessionId()},
                                                 {"run_id", run},
                                                 {"text", "INTERNAL_PLAN_MUST_NOT_APPEAR"}});
        QCOMPARE(panel.state().value("preview").toString(), QString{});
        QVERIFY(bridge.agentAwaitingApproval());
        // A resumed run snapshot may carry an old payload, but no live approval ID.
        transport.eventReceived("run.started", {{"session_id", adapter.sessionId()},
                                                {"run_id", run},
                                                {"run_type", "agent"},
                                                {"state", "running"},
                                                {"approval", approval}});
        QVERIFY(adapter.pendingApproval().isEmpty());
        QVERIFY(!panel.approve(true));
        adapter.refresh();
        QTRY_COMPARE(adapter.pendingApproval().value("approval_id"), approval.value("approval_id"));
        transport.disconnectFromDaemon();
        QVERIFY(adapter.pendingApproval().isEmpty());
        QVERIFY(!panel.approve(true));
        transport.connectToDaemon();
        QTRY_VERIFY(adapter.ready());
        QTRY_COMPARE(adapter.pendingApproval().value("approval_id"), approval.value("approval_id"));
        QCOMPARE(adapter.runId(), run);
        QCOMPARE(notices.size(), 1);
        QVERIFY(!bridge.latestAgentActivitySummary().isEmpty());
        if (action == "cancel")
            QVERIFY(panel.cancel());
        else if (action == "allow")
            QVERIFY(observerPanel.approve(true));
        else
            QVERIFY(panel.approve(false));
        QTRY_VERIFY(!bridge.agentLoopActive());
        QTRY_VERIFY(adapter.pendingApproval().isEmpty());
        QCOMPARE(adapter.value("conversationState").toString(),
                 action == "allow"    ? QString("completed")
                 : action == "cancel" ? QString("cancelled")
                                      : QString("failed"));
        if (action == "allow")
            QCOMPARE(bridge.lastAgentResponse(), QString("Fixture finished"));
        if (action == "allow") {
            const auto saved = controller->conversationStore()->loadMessages(actorSession);
            QVERIFY(!saved.isEmpty());
            QCOMPARE(saved.last().content, QString("Fixture finished"));
        }

        // Terminal state and its notification arrive through queued IPC updates.
        QTRY_COMPARE(notices.size(), 2);
        QVERIFY(!panel.approve(true));
        // A late completion from this closed run cannot turn cancellation/denial into success.
        const auto terminal = adapter.value("conversationState");
        transport.eventReceived(QStringLiteral("run.completed"),
                                {{"run_id", run},
                                 {"session_id", adapter.sessionId()},
                                 {"state", "completed"},
                                 {"text", "Late result"},
                                 {"detail", ""},
                                 {"server_generation", transport.serverGeneration()},
                                 {"event_sequence", 99999}});
        QCOMPARE(adapter.value("conversationState"), terminal);
        QVERIFY(panel.agent(QStringLiteral("Next fixture action")));
        QTRY_VERIFY(!adapter.pendingApproval().value("approval_id").toString().isEmpty());
        QVERIFY(panel.cancel());
        QTRY_VERIFY(!bridge.agentLoopActive());
    }
    void backupTransferIntegrityAndIsolation() {
        QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
        sentinel::core::AppSettings settings(std::make_unique<sentinel::core::InMemorySettingsStore>(), sentinel::core::inMemoryTestCredentialStore());
        sentinel::test::DeterministicModelServiceFixture models(sentinel::test::DeterministicChatReply::Final, &settings);
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService()).withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>()).build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        server.setSettings(&settings);
        const auto path = directory.filePath("daemon.sock");
        QVERIFY(server.startServer(path));
        DaemonClient owner(path, 2000, 10), other(path, 2000, 10);
        QTRY_VERIFY(owner.daemonReachable() && other.daemonReachable());
        auto call = [](DaemonClient& client, const QString& action, const QJsonObject& value) {
            QSignalSpy responses(&client, &DaemonClient::responseReceived);
            const auto id = client.request(DaemonClient::Command::backup_transfer, {{"action", action}, {"value", value}});
            QElapsedTimer timer; timer.start();
            while (timer.elapsed() < 2000) {
                for (const auto& response : responses)
                    if (response.at(0).toString() == id) return response.at(2).toJsonObject().value("result").toObject();
                QTest::qWait(1);
            }
            return QJsonObject{};
        };
        QCOMPARE(call(owner, "export", {{"domains", QJsonArray{"settings", "chat"}}})
                     .value("code")
                     .toString(),
                 QString("CoordinatedOperationBlocked"));
        const auto refused = call(owner, "importBegin",
                                  {{"domains", QJsonArray{"settings", "chat"}},
                                   {"size", 1},
                                   {"sha256", QString(64, '0')},
                                   {"replace", true}});
        QCOMPARE(refused.value("code").toString(), QString("CoordinatedOperationBlocked"));
        QVERIFY(refused.value("transferId").toString().isEmpty());
        const auto exported = call(owner, "export", {{"domains", QJsonArray{"settings"}}});
        QVERIFY(exported.value("succeeded").toBool());
        const auto transfer = exported.value("transferId").toString();
        QCOMPARE(call(other, "read", {{"transferId", transfer}, {"offset", 0}}).value("code").toString(), QString("UnknownTransfer"));
        QByteArray backup;
        while (backup.size() < exported.value("size").toInt()) {
            const auto chunk = call(owner, "read", {{"transferId", transfer}, {"offset", backup.size()}});
            QVERIFY(chunk.value("succeeded").toBool());
            backup += QByteArray::fromBase64(chunk.value("data").toString().toLatin1());
        }
        QCOMPARE(QCryptographicHash::hash(backup, QCryptographicHash::Sha256).toHex(), exported.value("sha256").toString().toLatin1());
        // Pad valid JSON beyond one IPC frame to exercise real chunking.
        backup += QByteArray(300000, ' ');
        auto begin = [&](const QByteArray& digest) {
            return call(owner, "importBegin", {{"domains", QJsonArray{"settings"}}, {"size", backup.size()}, {"sha256", QString::fromLatin1(digest)}, {"replace", false}}).value("transferId").toString();
        };
        auto upload = [&](const QString& id) {
            for (int offset = 0; offset < backup.size(); offset += 48 * 1024) {
                const auto result = call(owner, "write", {{"transferId", id}, {"offset", offset}, {"data", QString::fromLatin1(backup.mid(offset, 48 * 1024).toBase64())}});
                if (!result.value("succeeded").toBool()) return false;
            }
            return true;
        };
        const auto bad = begin(QByteArray(64, '0'));
        QVERIFY(!bad.isEmpty());
        QCOMPARE(call(owner, "write", {{"transferId", bad}, {"offset", 1}, {"data", "eA=="}}).value("code").toString(), QString("InvalidChunk"));
        QVERIFY(upload(bad));
        QCOMPARE(call(owner, "commit", {{"transferId", bad}}).value("code").toString(), QString("IntegrityFailure"));
        const auto good = begin(QCryptographicHash::hash(backup, QCryptographicHash::Sha256).toHex());
        QVERIFY(upload(good));
        QVERIFY(call(owner, "commit", {{"transferId", good}}).value("succeeded").toBool());
        const auto cancelled = begin(QCryptographicHash::hash(backup, QCryptographicHash::Sha256).toHex());
        QVERIFY(call(owner, "cancel", {}).value("succeeded").toBool());
        QCOMPARE(call(owner, "commit", {{"transferId", cancelled}}).value("code").toString(), QString("UnknownTransfer"));
        sentinel::desktop::DesktopRuntimeClient runtime(owner);
        QTRY_VERIFY(runtime.ready());
        QTRY_COMPARE(runtime.settingsService("productBackupAvailability", {}, true)
                         .toMap()
                         .value("coordinatedState")
                         .toString(),
                     QString("Blocked"));
        QVERIFY(!runtime.settingsService("productBackupAvailability", {}, true)
                     .toMap()
                     .value("coordinatedRestoreAvailable")
                     .toBool());
        sentinel::desktop::DesktopBackupHelper helper(&runtime);
        const auto file = QUrl::fromLocalFile(directory.filePath("backup.json"));
        QVERIFY(helper.exportFile(file, {"settings"}));
        QTRY_VERIFY(!helper.busy());
        QVERIFY2(helper.status().contains("saved successfully"), qPrintable(helper.status()));
        QVERIFY(helper.inspectFile(file));
        QCOMPARE(helper.importDomains(), QStringList{"settings"});
        QVERIFY(!helper.preview().isEmpty());
        QVERIFY(helper.importFile({"settings"}, false));
        QTRY_VERIFY(!helper.busy());
        QVERIFY2(helper.status().contains("restored successfully"), qPrintable(helper.status()));
        QVERIFY(!helper.inspectFile(QUrl::fromLocalFile(directory.filePath("absent.json"))));
        QVERIFY(helper.importDomains().isEmpty());
        QTRY_VERIFY(runtime.ready());
        runtime.setSetting("responseProfileInstructions", "Use short technical explanations.");
        QTRY_COMPARE(settings.responseProfileInstructions(), QString("Use short technical explanations."));
        QTRY_COMPARE(runtime.settingsValue("responseProfileInstructions", {}), QString("Use short technical explanations."));
    }
    void daemonSettingsRejectMalformedAndUnknownActions() {
        QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
        sentinel::core::AppSettings settings(
            std::make_unique<sentinel::core::InMemorySettingsStore>(),
            sentinel::core::inMemoryTestCredentialStore());
        sentinel::test::DeterministicModelServiceFixture models(
            sentinel::test::DeterministicChatReply::Final, &settings);
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        server.setSettings(&settings);
        const auto path = directory.filePath("daemon.sock");
        QVERIFY(server.startServer(path));
        DaemonClient client(path, 2000, 10);
        QTRY_VERIFY(client.daemonReachable());
        QSignalSpy errors(&client, &DaemonClient::requestFailed);
        QSignalSpy responses(&client, &DaemonClient::responseReceived);
        client.request(DaemonClient::Command::desktop_settings_service,
                       {{"action", "arbitraryInvocation"}, {"arguments", QJsonArray{}}});
        QTRY_COMPARE(errors.size(), 1);
        client.request(
            DaemonClient::Command::desktop_settings_service,
            {{"action", "setProductSetting"}, {"arguments", QJsonArray{true, "Trusted"}}});
        QTRY_COMPARE(errors.size(), 2);
        QCOMPARE(settings.defaultPermissionPolicyState(), QString("Disabled"));
        const auto request = client.request(
            DaemonClient::Command::desktop_settings_service,
            {{"action", "setProductSetting"},
             {"arguments", QJsonArray{"privacy.permission-policy", "Ask Every Time"}}});
        QTRY_VERIFY(([&] {
            for (const auto& response : responses)
                if (response.at(0) == request)
                    return response.at(2)
                        .toJsonObject()
                        .value("result")
                        .toObject()
                        .value("accepted")
                        .toBool();
            return false;
        })());
        QCOMPARE(settings.defaultPermissionPolicyState(), QString("Ask Every Time"));
    }
    void realChatCancelAndRecover() {
        QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
        sentinel::test::DeterministicModelServiceFixture models(
            sentinel::test::DeterministicChatReply::Delayed);
        models.state->delayMs = 500;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        const auto path = directory.filePath("daemon.sock");
        QVERIFY(server.startServer(path));
        DaemonClient client(path, 2000, 10);
        QTRY_VERIFY(client.daemonReachable());
        QSignalSpy replies(&client, &DaemonClient::responseReceived);
        const auto createId =
            client.request(DaemonClient::Command::session_create, {{"title", "Cancel/recover"}});
        QString session;
        QTRY_VERIFY(([&] {
            for (const auto& reply : replies) {
                if (reply.at(0).toString() == createId) {
                    session = reply.at(2).toJsonObject().value("session_id").toString();
                }
            }
            return !session.isEmpty();
        })());
        QSignalSpy events(&client, &DaemonClient::eventReceived);
        const auto sendId = client.request(DaemonClient::Command::chat_send,
                                           {{"session_id", session}, {"text", "cancel this turn"}});
        QString run;
        QTRY_VERIFY(([&] {
            for (const auto& reply : replies) {
                if (reply.at(0).toString() == sendId) {
                    run = reply.at(2).toJsonObject().value("run_id").toString();
                }
            }
            return !run.isEmpty();
        })());
        client.request(DaemonClient::Command::run_cancel, {{"run_id", run}});
        QTRY_VERIFY(([&] {
            for (const auto& event : events) {
                if (event.at(0) == "run.cancelled") {
                    return true;
                }
            }
            return false;
        })());
        QTest::qWait(600);
        for (const auto& event : events) {
            QVERIFY(event.at(0) != "run.completed");
        }
        events.clear();
        client.request(DaemonClient::Command::chat_send,
                       {{"session_id", session}, {"text", "post-cancel turn"}});
        QTRY_VERIFY(([&] {
            for (const auto& event : events) {
                if (event.at(0) == "run.completed") {
                    return true;
                }
            }
            return false;
        })());
        client.disconnectFromDaemon();
        server.stopServer();
    }
    void realAgentApproval_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("allow") << QString("allow");
        QTest::newRow("deny") << QString("deny");
        QTest::newRow("cancel") << QString("cancel");
    }
    void realAgentApproval() {
        QFETCH(QString, action);
        QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
        sentinel::test::DeterministicModelServiceFixture models;
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller =
            builder.withModelService(models.takeModelService())
                .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                .withAgentRuntime(std::make_unique<sentinel::core::NullAgentRuntime>(
                    sentinel::core::NullAgentRuntime::standardTools()))
                .withAgentStepPlanner(std::make_unique<ApprovalPlanner>())
                .withToolExecutor(std::make_unique<FixtureExecutor>())
                .withSandboxPolicy(std::make_unique<sentinel::core::StaticSandboxPolicy>())
                .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        const auto path = directory.filePath("daemon.sock");
        QVERIFY(server.startServer(path));
        DaemonClient desktop(path, 2000, 10);
        QTRY_VERIFY(desktop.daemonReachable());
        QSignalSpy replies(&desktop, &DaemonClient::responseReceived);
        const auto createId =
            desktop.request(DaemonClient::Command::session_create, {{"title", "Agent IPC"}});
        QString session;
        QTRY_VERIFY(([&] {
            for (const auto& reply : replies) {
                if (reply.at(0).toString() == createId) {
                    session = reply.at(2).toJsonObject().value("session_id").toString();
                }
            }
            return !session.isEmpty();
        })());
        QSignalSpy events(&desktop, &DaemonClient::eventReceived);
        desktop.request(DaemonClient::Command::agent_start,
                        {{"session_id", session}, {"text", "perform action"}});
        QJsonObject approval;
        QTRY_VERIFY(([&] {
            for (const auto& event : events) {
                if (event.at(0) == "approval.requested") {
                    approval = event.at(1).toJsonObject();
                }
            }
            return !approval.isEmpty();
        })());
        QVERIFY(!approval.value("approval_id").toString().isEmpty());
        // Reattach with a fresh Desktop client while the daemon retains the pending approval.
        desktop.disconnectFromDaemon();
        DaemonClient reattached(path, 2000, 10);
        QTRY_VERIFY(reattached.daemonReachable());
        QSignalSpy snapshots(&reattached, &DaemonClient::responseReceived);
        const auto attachId =
            reattached.request(DaemonClient::Command::session_attach, {{"session_id", session}});
        QJsonObject snapshot;
        QTRY_VERIFY(([&] {
            for (const auto& reply : snapshots) {
                if (reply.at(0).toString() == attachId) {
                    snapshot = reply.at(2).toJsonObject();
                }
            }
            return !snapshot.isEmpty();
        })());
        QCOMPARE(snapshot.value("state").toString(), QString("approval"));
        QCOMPARE(snapshot.value("approval_id"), approval.value("approval_id"));
        QSignalSpy terminal(&reattached, &DaemonClient::eventReceived);
        QSignalSpy errors(&reattached, &DaemonClient::requestFailed);
        if (action == "cancel") {
            reattached.request(DaemonClient::Command::run_cancel,
                               {{"run_id", approval.value("run_id")}});
        } else {
            reattached.request(DaemonClient::Command::approval_respond,
                               {{"run_id", approval.value("run_id")},
                                {"approval_id", approval.value("approval_id")},
                                {"allow", action == "allow"}});
        }
        QTRY_VERIFY(([&] {
            for (const auto& event : terminal) {
                if (event.at(0) == "run.completed" || event.at(0) == "run.failed" ||
                    event.at(0) == "run.cancelled") {
                    return true;
                }
            }
            return false;
        })());
        if (action == "allow") {
            bool completed = false;
            for (const auto& event : terminal) {
                if (event.at(0) == "run.completed") {
                    completed = true;
                    QCOMPARE(event.at(1).toJsonObject().value("text").toString(),
                             QString("Fixture finished"));
                }
            }
            QVERIFY(completed);
        } else {
            for (const auto& event : terminal) {
                QVERIFY(event.at(0) != "run.completed");
            }
        }
        reattached.request(DaemonClient::Command::approval_respond,
                           {{"run_id", approval.value("run_id")},
                            {"approval_id", approval.value("approval_id")},
                            {"allow", true}});
        QTRY_VERIFY(!errors.isEmpty());
        QCOMPARE(errors.last().at(1).value<DaemonClient::Error>(),
                 DaemonClient::Error::ApprovalExpired);
        reattached.disconnectFromDaemon();
        server.stopServer();
    }
    void realDaemonServiceChatAndMultiClient() {
        QTemporaryDir directory{QDir::tempPath() + "/sd-XXXXXX"};
        sentinel::test::DeterministicModelServiceFixture models(
            sentinel::test::DeterministicChatReply::Streaming);
        sentinel::core::ApplicationControllerBuilder builder;
        auto controller = builder.withModelService(models.takeModelService())
                              .withMemoryStore(std::make_unique<sentinel::core::InMemoryStore>())
                              .build();
        sentinel::daemon::DaemonIpcServer server(controller.get());
        const auto path = directory.filePath("daemon.sock");
        QVERIFY(server.startServer(path));
        DaemonClient desktop(path, 2000, 10), second(path, 2000, 10);
        QTRY_VERIFY(desktop.daemonReachable());
        QTRY_VERIFY(second.daemonReachable());
        QVERIFY(!desktop.serverGeneration().isEmpty());
        QCOMPARE(desktop.serverGeneration(), second.serverGeneration());
        QSignalSpy replies(&desktop, &DaemonClient::responseReceived);
        const auto createId =
            desktop.request(DaemonClient::Command::session_create, {{"title", "IPC desktop"}});
        QString session;
        QTRY_VERIFY(([&] {
            for (const auto& reply : replies) {
                if (reply.at(0).toString() == createId) {
                    session = reply.at(2).toJsonObject().value("session_id").toString();
                }
            }
            return !session.isEmpty();
        })());
        QSignalSpy events(&desktop, &DaemonClient::eventReceived);
        QSignalSpy otherEvents(&second, &DaemonClient::eventReceived);
        desktop.request(DaemonClient::Command::chat_send,
                        {{"session_id", session}, {"text", "fixture prompt"}});
        QTRY_VERIFY(([&] {
            for (const auto& event : events) {
                if (event.at(0) == "run.completed") {
                    return true;
                }
            }
            return false;
        })());
        bool delta = false;
        for (const auto& event : events) {
            delta |= event.at(0) == "output.delta";
            if (event.at(0) == "run.completed") {
                QCOMPARE(event.at(1).toJsonObject().value("text").toString(),
                         QString("SENTINEL_TEST_RESPONSE"));
            }
        }
        QVERIFY(delta);
        QCOMPARE(otherEvents.size(), 0);
        const auto generation = desktop.serverGeneration();
        desktop.disconnectFromDaemon();
        QVERIFY(second.daemonReachable());
        server.stopServer();
        QTRY_VERIFY(!second.daemonReachable());
        QVERIFY(server.startServer(path));
        QTRY_VERIFY(second.daemonReachable());
        QVERIFY(second.serverGeneration() != generation);
        second.disconnectFromDaemon();
        server.stopServer();
        QVERIFY(!QFileInfo::exists(path));
    }
};
QTEST_GUILESS_MAIN(DesktopIpcTest)
#include "test_desktop_ipc.moc"
