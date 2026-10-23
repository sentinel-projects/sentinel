// SPDX-License-Identifier: GPL-3.0-or-later
#include "../support/ShortSocketPath.h"
#include "sentinel/core/agent/AgentRuntime.h"
#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/app/WorkspaceService.h"
#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/memory/InMemoryStore.h"
#include "sentinel/core/model/ModelManagement.h"
#include "sentinel/core/runtime/RealToolExecutor.h"
#include "sentinel/core/security/StaticSandboxPolicy.h"
#include "sentinel/desktop/DaemonClient.h"
#include "sentinel/desktop/DesktopRuntimeClient.h"
#include "service/DaemonIpcServer.h"
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <algorithm>
#include <atomic>

using namespace sentinel;
using desktop::DaemonClient;
using sentinel::test::shortSocketDirTemplate;
namespace {
constexpr int deadlineMs = 5000;
const auto original = QByteArrayLiteral("original fixture bytes\n");
const auto replacement = QByteArrayLiteral("QA02B independently checked bytes\n");
QMap<QString, QByteArray> manifest(const QString& root) {
    QMap<QString, QByteArray> result;
    QDirIterator files(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (!file.open(QIODevice::ReadOnly)) {
            return {{"<unreadable>", {}}};
        }
        result.insert(QDir(root).relativeFilePath(file.fileName()),
                      QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
    }
    return result;
}
class CatalogClient final : public core::IOllamaRuntimeClient {
public:
    core::OllamaConfig config() const override {
        return {};
    }
    core::OllamaHealthCheckResult healthCheck() const override {
        core::OllamaHealthCheckResult result;
        result.connectionStatus = core::OllamaConnectionStatus::Connected;
        result.healthStatus = core::OllamaHealthStatus::Healthy;
        return result;
    }
    QList<core::OllamaModelSummary> installedModels() const override {
        return {{QStringLiteral("sentinel-test-model"), {}, 0}};
    }
};
struct Script {
    QString path;
    std::atomic_int decisions{0}, classifications{0};
    std::atomic_bool sawObservation{false};
    QString mode;
    QSemaphore finalEntered, finalRelease;
    std::atomic_bool timedOut{false};
};
// Only inference is synthetic. No gateway/executor/permission substitute.
class ScriptedProvider final : public core::IChatProvider {
public:
    explicit ScriptedProvider(std::shared_ptr<Script> state) : state_(std::move(state)) {}
    QString name() const override {
        return "QA02B-script";
    }
    core::ChatProviderStatus status() const override {
        return core::ChatProviderStatus::Ready;
    }
    core::ChatProviderReply sendMessage(const QString& prompt) override {
        return sendRequest(prompt, {});
    }
    core::ChatProviderReply sendRequest(const QString& prompt,
                                        const core::ChatRequestOptions&) override {
        QJsonObject object;
        if (prompt.startsWith("Classify the USER GOAL")) {
            ++state_->classifications;
            object = {{"requirements", QJsonArray{QJsonObject{{"domain", "filesystem"},
                                                              {"resource", state_->path},
                                                              {"purpose", "operate"}},
                                                  QJsonObject{{"domain", "filesystem"},
                                                              {"resource", state_->path},
                                                              {"purpose", "inspect"},
                                                              {"tool", "read-file"}}}}};
            if (state_->mode == "omitted-action") {
                auto requirements = object.value("requirements").toArray();
                requirements.removeAt(0);
                object.insert("requirements", requirements);
            }
            if (state_->mode == "wrong-content") {
                auto requirements = object.value("requirements").toArray();
                auto claim = requirements[1].toObject();
                claim.insert("operation", "contains");
                claim.insert("query", QString::fromUtf8(replacement).trimmed());
                requirements[1] = claim;
                object.insert("requirements", requirements);
            }
        } else {
            const int step = state_->decisions++;
            if (state_->mode == "malformed") {
                object = {{"action", "tool"},
                          {"tool", "write-file"},
                          {"args", QJsonObject{{"path", 42}}}};
            } else if (step == 0 && state_->mode == "omitted-action") {
                object = {{"action", "tool"},
                          {"tool", "read-file"},
                          {"args", QJsonObject{{"path", state_->path}}}};
            } else if (step == 0) {
                object = {{"action", "tool"},
                          {"tool", "write-file"},
                          {"args",
                           QJsonObject{{"path", state_->path},
                                       {"content", QString::fromUtf8(state_->mode == "wrong-content"
                                                                         ? original
                                                                         : replacement)}}}};
            } else if (state_->mode == "unaccepted-final") {
                object = {{"action", "final"},
                          {"grounding", "context"},
                          {"answer", "Pretended success without verification."}};
            } else if (step == 1) {
                // External fault injection: the just-written resource disappears before
                // verification.
                if (state_->mode == "verification-failed")
                    QFile::remove(state_->path);
                object = {{"action", "tool"},
                          {"tool", "read-file"},
                          {"args", QJsonObject{{"path", state_->path}}}};
            } else {
                state_->sawObservation = prompt.contains(QString::fromUtf8(replacement).trimmed());
                if (state_->mode == "allowed") {
                    state_->finalEntered.release();
                    if (!state_->finalRelease.tryAcquire(1, deadlineMs)) {
                        state_->timedOut = true;
                    }
                }
                if (state_->mode == "repeat-after-read" && step == 2) {
                    core::ChatProviderReply repeated;
                    repeated.success = true;
                    repeated.message = QString::fromUtf8(
                        QJsonDocument(
                            QJsonObject{
                                {"action", "tool"},
                                {"tool", "write-file"},
                                {"args", QJsonObject{{"path", state_->path},
                                                     {"content", QString::fromUtf8(replacement)}}}})
                            .toJson(QJsonDocument::Compact));
                    return repeated;
                }
                if (state_->mode == "context-final") {
                    core::ChatProviderReply refused;
                    refused.category = core::ChatProviderErrorCategory::RequestRejected;
                    refused.errorMessage = "maximum context length exceeded";
                    return refused;
                }
                if (state_->mode == "malformed-final") {
                    core::ChatProviderReply malformed;
                    malformed.success = true;
                    malformed.message = "Finished";
                    return malformed;
                }
                object = {{"action", "final"},
                          {"grounding", state_->mode == "verification-failed" ? "unable_to_verify"
                                        : state_->mode == "repair-final" && step == 2 ? "context"
                                                                                      : "verified"},
                          {"answer", state_->mode == "empty-final"
                                         ? ""
                                         : "Fixture file updated and independently read back."}};
                if (state_->mode == "wrong-content")
                    object.insert("claims",
                                  QJsonArray{QJsonObject{{"id", "claim-2"}, {"assertion", true}}});
            }
        }
        core::ChatProviderReply reply;
        reply.success = true;
        reply.lifecycle = core::ChatRequestLifecycle::Completed;
        reply.message = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
        return reply;
    }

private:
    std::shared_ptr<Script> state_;
};
struct Fixture {
    QTemporaryDir directory{shortSocketDirTemplate("qb")};
    core::AppSettings settings{std::make_unique<core::InMemorySettingsStore>(),
                               core::inMemoryTestCredentialStore()};
    std::shared_ptr<Script> script = std::make_shared<Script>();
    std::unique_ptr<core::ApplicationController> controller;
    std::unique_ptr<daemon::DaemonIpcServer> server;
    QString root, path, artifact, projection, conversations;
    QMap<QString, QByteArray> before;
    bool start(const QString& mode = {}, const QString& providerId = "ollama") {
        root = directory.filePath("workspace");
        if (!directory.isValid() || !QDir().mkpath(root)) {
            return false;
        }
        root = QFileInfo(root).canonicalFilePath();
        artifact = QDir(root).filePath("allowed.txt");
        for (const auto& name : {QString("allowed.txt"), QString("unchanged.txt")}) {
            QFile f(QDir(root).filePath(name));
            if (!f.open(QIODevice::WriteOnly) || f.write(original) != original.size()) {
                return false;
            }
        }
        before = manifest(root);
        script->path = artifact;
        script->mode = mode;
        if (mode == "outside") {
            script->path = directory.filePath("outside.txt");
            QFile f(script->path);
            if (!f.open(QIODevice::WriteOnly) || f.write(original) != original.size()) {
                return false;
            }
        }
        auto models = std::make_unique<core::ModelService>(&settings);
        models->registerProvider(providerId, [shared = script](const core::ModelBinding&) {
            return std::make_shared<ScriptedProvider>(shared);
        });
        core::OllamaModelDiscoveryResult catalog;
        catalog.models = {{"sentinel-test-model", {}, 0}};
        catalog.lifecycle = core::ChatRequestLifecycle::Completed;
        models->acceptOllamaDiscovery(catalog, models->beginProviderHealthObservation());
        if (providerId != "ollama") {
            core::ProviderDiscoveryOutcome outcome;
            outcome.completed = true;
            models->acceptProviderDiscovery(providerId, catalog.models, outcome,
                                            models->beginProviderHealthObservation());
        }
        models->setSelectedModel({providerId, "sentinel-test-model"});
        conversations = directory.filePath("conversations.sqlite3");
        core::ApplicationControllerBuilder builder;
        controller =
            builder.withOllamaRuntimeClient(std::make_unique<CatalogClient>())
                .withModelService(std::move(models))
                .withMemoryStore(std::make_unique<core::InMemoryStore>())
                .withConversationStore(
                    std::make_unique<core::SQLiteConversationStore>(conversations))
                .withAgentRuntime(std::make_unique<core::NullAgentRuntime>(
                    core::NullAgentRuntime::standardTools()))
                .withToolExecutor(std::make_unique<core::RealToolExecutor>())
                .withSandboxPolicy(std::make_unique<core::StaticSandboxPolicy>(
                    QSet<QString>{"tool.metadata.read", "tool.risk.medium", "tool.risk.high"}))
                .build();
        settings.setNetworkMode("LocalOnly");
        core::WorkspaceService workspaces;
        const auto created = workspaces.createWorkspace({}, "QA02B", "Coding");
        const auto rooted =
            workspaces.setWorkspaceRoot(created.catalogJson, created.selectedWorkspaceId, root);
        if (!rooted.success) {
            return false;
        }
        settings.setWorkspaceCatalogJson(rooted.catalogJson);
        settings.setSelectedWorkspaceId(created.selectedWorkspaceId);
        settings.setWorkspaceProfilesJson(workspaces.updateProfile(
            {}, created.selectedWorkspaceId, {},
            {{"providerId", providerId}, {"modelId", "sentinel-test-model"}}));
        controller->attachControlledTaskSettings(settings);
        controller->setAgentAutonomousMode(false);
        server = std::make_unique<daemon::DaemonIpcServer>(controller.get());
        server->setSettings(&settings);
        projection = directory.filePath("projection.sqlite3");
        path = directory.filePath("daemon.sock");
        return server->openSessionProjectionStore(projection) && server->startServer(path);
    }
    ~Fixture() {
        script->finalRelease.release();
        server.reset();
        controller.reset();
    }
    core::AgentLoopState state() const {
        return controller->agentRuntime()->sessionState(controller->activeAgentSessionId());
    }
};
struct Reply {
    bool received = false, ok = false;
    QJsonObject payload;
    QString error;
};
Reply call(DaemonClient& client, DaemonClient::Command command, const QJsonObject& payload = {}) {
    QSignalSpy replies(&client, &DaemonClient::responseReceived),
        errors(&client, &DaemonClient::requestFailed);
    const auto id = client.request(command, payload);
    Reply result;
    (void)QTest::qWaitFor(
        [&] {
            for (const auto& row : replies) {
                if (row[0].toString() == id) {
                    result = {true, true, row[2].toJsonObject(), {}};
                    return true;
                }
            }
            for (const auto& row : errors) {
                if (row[0].toString() == id) {
                    result = {true, false, {}, row[2].toString()};
                    return true;
                }
            }
            return false;
        },
        deadlineMs);
    return result;
}
QJsonObject wireEvent(const QSignalSpy& events, const QString& name) {
    for (const auto& row : events) {
        if (row[0].toString() == name) {
            return row[1].toJsonObject();
        }
    }
    return {};
}
int terminalCount(const QSignalSpy& events) {
    int count = 0;
    for (const auto& row : events) {
        if (QStringList{"run.completed", "run.failed", "run.cancelled"}.contains(
                row[0].toString())) {
            ++count;
        }
    }
    return count;
}
} // namespace
class ProviderAgentE2eTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
#ifndef Q_OS_UNIX
        QSKIP("Production daemon peer authentication requires Unix; Windows transport acceptance "
              "remains blocked.");
#endif
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName("sentinel-qa02b-" +
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
    void completionRegression_data() {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<QString>("mode");
        for (const auto& provider : {"ollama", "lm-studio", "llama-cpp-server"})
            for (const auto& mode : {"create-success", "empty-final", "repair-final",
                                     "malformed-final", "wrong-content", "verification-failed",
                                     "context-final", "repeat-after-read", "omitted-action"})
                QTest::newRow(qPrintable(QString(provider) + "-" + mode))
                    << QString(provider) << QString(mode);
    }
    void completionRegression() {
        QFETCH(QString, provider);
        QFETCH(QString, mode);
        Fixture fixture;
        QVERIFY(fixture.start(mode, provider));
        if (mode == "create-success")
            QVERIFY(QFile::remove(fixture.artifact));
        DaemonClient client(fixture.path, deadlineMs, 10000);
        client.connectToDaemon();
        QTRY_VERIFY(client.daemonReachable());
        const auto created = call(client, DaemonClient::Command::session_create,
                                  {{"title", "Completion regression"}});
        QVERIFY(created.ok);
        DaemonClient observer(fixture.path, deadlineMs, 10000);
        desktop::DesktopRuntimeClient ui(observer);
        ui.setPreferredSession(created.payload.value("session_id").toString());
        observer.connectToDaemon();
        QTRY_VERIFY(ui.ready());
        QSignalSpy events(&client, &DaemonClient::eventReceived);
        QVERIFY(ui.dispatch(
                      "runAgentRequest",
                      {QStringLiteral(
                          "Update allowed.txt with fixture bytes and independently read it back.")})
                    .toBool());
        QTRY_VERIFY(!ui.runId().isEmpty());
        const auto runId = ui.runId();
        QTRY_VERIFY(!wireEvent(events, "approval.requested").isEmpty() || terminalCount(events));
        const auto approval = wireEvent(events, "approval.requested");
        if (!approval.isEmpty()) {
            QVERIFY(call(client, DaemonClient::Command::approval_respond,
                         {{"run_id", runId},
                          {"approval_id", approval.value("approval_id")},
                          {"allow", true}})
                        .ok);
        }
        QTRY_COMPARE_WITH_TIMEOUT(terminalCount(events), 1, 10000);
        QFile bytes(fixture.artifact);
        if (mode == "verification-failed") {
            QVERIFY(!bytes.exists());
        } else {
            QVERIFY(bytes.open(QIODevice::ReadOnly));
            QCOMPARE(bytes.readAll(), (mode == "wrong-content" || mode == "omitted-action")
                                          ? original
                                          : replacement);
        }
        QFile unchanged(QDir(fixture.root).filePath("unchanged.txt"));
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), original);
        const auto state = fixture.state();
        QCOMPARE(state.steps.size(), mode == "omitted-action"
                                         ? 1
                                         : 2); // No repeated mutation or unnecessary read/retry.
        const bool completed = mode != "malformed-final" && mode != "wrong-content" &&
                               mode != "verification-failed" && mode != "context-final" &&
                               mode != "omitted-action";
        QCOMPARE(state.phase,
                 completed ? core::AgentLoopPhase::Completed : core::AgentLoopPhase::Failed);
        const auto terminal = wireEvent(events, completed ? "run.completed" : "run.failed");
        QCOMPARE(
            terminal.value("completion_status").toString(),
            completed ? QString("Completed")
            : (mode == "wrong-content" || mode == "verification-failed" || mode == "omitted-action")
                ? QString("Verification Failed")
                : QString("Completion Indeterminate"));
        QTRY_COMPARE(ui.value("agentCompletionStatus").toString(),
                     terminal.value("completion_status").toString());
        QVERIFY(fixture.controller->agentRunStore());
        const auto runs = fixture.controller->agentRunStore()->recentRuns(1);
        QVERIFY(!runs.isEmpty());
        QCOMPARE(runs.first().state, core::agentLoopPhaseName(state.phase));
        QCOMPARE(runs.first().terminalReason, core::agentTerminalReasonName(state.terminalReason));
        if (completed)
            QTRY_COMPARE(ui.value("lastAgentResponse").toString(), state.finalAnswer);
        if (mode == "wrong-content" || mode == "verification-failed" || mode == "omitted-action") {
            QCOMPARE(state.terminalReason, core::AgentTerminalReason::VerificationFailed);
            QVERIFY(state.finalAnswer.isEmpty());
        } else if (!completed) {
            QCOMPARE(state.terminalReason,
                     mode == "context-final" ? core::AgentTerminalReason::ContextBudgetExhausted
                                             : core::AgentTerminalReason::CompletionIndeterminate);
            QVERIFY(fixture.controller->lastAgentResponse().contains("Completion Indeterminate"));
            QVERIFY(!fixture.controller->lastAgentResponse().contains("Agent Task Failed"));
        } else {
            QVERIFY(!state.finalAnswer.isEmpty());
            QCOMPARE(state.finalGrounding.mode, core::GroundingMode::Verified);
        }
    }
    void providerDrivenChain_data() {
        QTest::addColumn<QString>("mode");
        for (const auto& mode : {"allowed", "denied", "cancelled", "outside", "malformed",
                                 "unaccepted-final", "stale-generation"}) {
            QTest::newRow(mode) << QString(mode);
        }
    }
    void providerDrivenChain() {
        QFETCH(QString, mode);
        Fixture fixture;
        QVERIFY(fixture.start(mode));
        DaemonClient client(fixture.path, deadlineMs, 10000);
        client.connectToDaemon();
        QTRY_VERIFY(client.daemonReachable());
        const auto created =
            call(client, DaemonClient::Command::session_create, {{"title", "QA02B isolated"}});
        QVERIFY(created.ok);
        auto session = created.payload.value("session_id").toString();
        QSignalSpy events(&client, &DaemonClient::eventReceived);
        const auto started = call(
            client, DaemonClient::Command::agent_start,
            {{"session_id", session},
             {"text",
              "Update allowed.txt with the requested fixture bytes and read back its content."}});
        QVERIFY(started.ok);
        auto run = started.payload.value("run_id");
        QJsonObject approval;
        if (mode != "malformed") {
            QTRY_VERIFY(!wireEvent(events, "approval.requested").isEmpty() ||
                        terminalCount(events));
            approval = wireEvent(events, "approval.requested");
            if (!approval.isEmpty()) {
                QCOMPARE(manifest(fixture.root), fixture.before);
                if (mode == "cancelled" || mode == "stale-generation") {
                    QVERIFY(call(client, DaemonClient::Command::run_cancel, {{"run_id", run}}).ok);
                } else {
                    QVERIFY(call(client, DaemonClient::Command::approval_respond,
                                 {{"run_id", run},
                                  {"approval_id", approval.value("approval_id")},
                                  {"allow", mode != "denied"}})
                                .ok);
                }
            }
        }
        if (mode == "allowed") {
            QTRY_COMPARE(fixture.script->finalEntered.available(), 1);
            QCOMPARE(terminalCount(events), 0); // Tool success is not terminal completion.
            QCOMPARE(fixture.state().phase, core::AgentLoopPhase::Running);
            const auto afterWrite = manifest(fixture.root);
            QVERIFY(!call(client, DaemonClient::Command::approval_respond,
                          {{"run_id", run},
                           {"approval_id", approval.value("approval_id")},
                           {"allow", true}})
                         .ok);
            QCOMPARE(manifest(fixture.root), afterWrite);
            fixture.script->finalRelease.release();
        }
        if (mode == "stale-generation") {
            QTRY_COMPARE(terminalCount(events), 1);
            const auto generation = client.serverGeneration();
            const auto staleRun = run;
            const auto staleApproval = approval.value("approval_id");
            client.disconnectFromDaemon();
            fixture.server.reset();
            fixture.server = std::make_unique<daemon::DaemonIpcServer>(fixture.controller.get());
            fixture.server->setSettings(&fixture.settings);
            QVERIFY(fixture.server->openSessionProjectionStore(fixture.projection));
            QVERIFY(fixture.server->startServer(fixture.path));
            fixture.script->decisions = 0;
            client.connectToDaemon();
            QTRY_VERIFY(client.daemonReachable());
            QVERIFY(client.serverGeneration() != generation);
            const auto fresh =
                call(client, DaemonClient::Command::session_create, {{"title", "New generation"}});
            QVERIFY(fresh.ok);
            session = fresh.payload.value("session_id").toString();
            events.clear();
            const auto freshRun = call(
                client, DaemonClient::Command::agent_start,
                {{"session_id", session}, {"text", "Update allowed.txt and verify its bytes."}});
            QVERIFY(freshRun.ok);
            run = freshRun.payload.value("run_id");
            QTRY_VERIFY(!wireEvent(events, "approval.requested").isEmpty());
            approval = wireEvent(events, "approval.requested");
            QVERIFY(approval.value("approval_id") != staleApproval);
            QVERIFY(!call(client, DaemonClient::Command::approval_respond,
                          {{"run_id", staleRun}, {"approval_id", staleApproval}, {"allow", true}})
                         .ok);
            QCOMPARE(fixture.state().phase, core::AgentLoopPhase::AwaitingApproval);
            QCOMPARE(manifest(fixture.root), fixture.before);
            QVERIFY(call(client, DaemonClient::Command::approval_respond,
                         {{"run_id", run},
                          {"approval_id", approval.value("approval_id")},
                          {"allow", false}})
                        .ok);
        }
        QTRY_COMPARE_WITH_TIMEOUT(terminalCount(events), 1, 10000);
        const auto state = fixture.state();
        qInfo() << "QA02B" << mode << core::agentLoopPhaseName(state.phase) << state.abortReason
                << state.finalAnswer;
        if (mode == "allowed") {
            QCOMPARE(state.phase, core::AgentLoopPhase::Completed);
            QVERIFY(!approval.isEmpty());
            QVERIFY(fixture.script->sawObservation.load());
            QCOMPARE(fixture.script->classifications.load(), 1);
            QCOMPARE(state.steps.size(), 2);
            QCOMPARE(state.steps[0].toolId, QString("write-file"));
            QCOMPARE(state.steps[1].toolId, QString("read-file"));
            QVERIFY(!state.finalAnswer.isEmpty());
            QVERIFY(!fixture.script->timedOut.load());
            QFile bytes(fixture.artifact);
            QVERIFY(bytes.open(QIODevice::ReadOnly));
            QCOMPARE(bytes.readAll(), replacement);
            // Coding finals stay natural-language prose. The canonical observed
            // text remains in evidence and is not injected into the user-facing
            // answer just because a file was read.
            QVERIFY(!state.finalAnswer.trimmed().startsWith(QLatin1Char('{')));
            QCOMPARE(state.finalAnswer,
                     QStringLiteral("Fixture file updated and independently read back."));
            const bool observedBytes =
                std::any_of(state.evidence.cbegin(), state.evidence.cend(), [&](const auto& e) {
                    return e.toolId == QLatin1String("read-file") && e.structuredObservation &&
                           e.structuredObservation->kind ==
                               core::StructuredObservationKind::FileContent &&
                           e.structuredObservation->data.value(QStringLiteral("content"))
                                   .toString()
                                   .toUtf8() == replacement;
                });
            QVERIFY(observedBytes);
            QCOMPARE(state.finalGrounding.mode, core::GroundingMode::Verified);
            auto expected = fixture.before;
            expected["allowed.txt"] =
                QCryptographicHash::hash(replacement, QCryptographicHash::Sha256);
            QCOMPARE(manifest(fixture.root), expected);
        } else {
            QVERIFY(state.phase != core::AgentLoopPhase::Completed);
            QVERIFY(wireEvent(events, "run.completed").isEmpty());
            if (mode != "unaccepted-final") {
                QCOMPARE(manifest(fixture.root), fixture.before);
            }
            if (mode == "outside") {
                QFile f(fixture.script->path);
                QVERIFY(f.open(QIODevice::ReadOnly));
                QCOMPARE(f.readAll(), original);
            }
        }
        if (!approval.isEmpty()) {
            QVERIFY(!call(client, DaemonClient::Command::approval_respond,
                          {{"run_id", run},
                           {"approval_id", approval.value("approval_id")},
                           {"allow", true}})
                         .ok);
        }
        const auto snapshot =
            call(client, DaemonClient::Command::session_attach, {{"session_id", session}});
        QVERIFY(snapshot.ok);
        QCOMPARE(snapshot.payload.value("state").toString(),
                 mode == "allowed"     ? QString("completed")
                 : mode == "cancelled" ? QString("cancelled")
                                       : QString("failed"));
        const auto connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(fixture.projection);
            QVERIFY(db.open());
            QSqlQuery query(db);
            query.prepare("SELECT payload FROM session_projection WHERE session_id = ?");
            query.addBindValue(session);
            QVERIFY(query.exec());
            QVERIFY(query.next());
            const auto disk = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
            QCOMPARE(disk.value("state"), snapshot.payload.value("state"));
            QVERIFY(!disk.contains("output")); // Transcript has its own authoritative store.
        }
        QSqlDatabase::removeDatabase(connection);
        // Independent conversation reader: terminal success is durable assistant content.
        core::SQLiteConversationStore persisted(fixture.conversations);
        const auto messages = persisted.loadMessages(session);
        int acceptedMessages = 0;
        for (const auto& message : messages) {
            if (message.role == core::ChatRole::Assistant && message.content == state.finalAnswer &&
                !state.finalAnswer.isEmpty()) {
                ++acceptedMessages;
            }
            QVERIFY(!message.content.contains("Pretended success"));
        }
        QCOMPARE(acceptedMessages, mode == "allowed" ? 1 : 0);
        QCOMPARE(snapshot.payload.value("output").toString(), state.finalAnswer);
        const auto runConnection = QUuid::createUuid().toString();
        int writes = 0;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", runConnection);
            db.setDatabaseName(
                QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                    .filePath("agent_runs.sqlite3"));
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.prepare("SELECT state, final_answer, failure FROM agent_runs WHERE session_id = ? "
                      "ORDER BY started_at DESC LIMIT 1");
            q.addBindValue(fixture.controller->activeAgentSessionId());
            QVERIFY(q.exec());
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), mode == "allowed"     ? QString("Completed")
                                            : mode == "cancelled" ? QString("Cancelled")
                                                                  : QString("Failed"));
            QCOMPARE(q.value(1).toString(), state.finalAnswer);
            if (mode != "allowed") {
                QVERIFY(!q.value(2).toString().isEmpty());
            }
            q.prepare("SELECT COUNT(*) FROM agent_tool_calls c JOIN agent_steps s ON c.step_id = "
                      "s.step_id JOIN agent_runs r ON s.run_id = r.run_id WHERE r.session_id = ? "
                      "AND c.tool_id = 'write-file' AND c.status = 'Succeeded'");
            q.addBindValue(fixture.controller->activeAgentSessionId());
            QVERIFY(q.exec());
            QVERIFY(q.next());
            writes = q.value(0).toInt();
            QCOMPARE(writes, mode == "allowed" || mode == "unaccepted-final" ? 1 : 0);
        }
        QSqlDatabase::removeDatabase(runConnection);
        QJsonObject hashes;
        for (auto it = fixture.before.cbegin(); it != fixture.before.cend(); ++it) {
            hashes.insert(it.key(), QString::fromLatin1(it.value().toHex()));
        }
        QJsonObject after;
        const auto finalManifest = manifest(fixture.root);
        for (auto it = finalManifest.cbegin(); it != finalManifest.cend(); ++it) {
            after.insert(it.key(), QString::fromLatin1(it.value().toHex()));
        }
        qInfo().noquote() << "QA02B_ORACLE"
                          << QJsonDocument(
                                 QJsonObject{
                                     {"scenario", mode},
                                     {"before", hashes},
                                     {"after", after},
                                     {"successful_writes", writes},
                                     {"planner_calls", fixture.script->decisions.load()},
                                     {"classifications", fixture.script->classifications.load()},
                                     {"terminal_events", terminalCount(events)},
                                     {"accepted_final", mode == "allowed"},
                                     {"persisted_state", snapshot.payload.value("state")}})
                                 .toJson(QJsonDocument::Compact);
        QVERIFY(fixture.controller->persistentPermissionGrants().isEmpty());
        QCOMPARE(terminalCount(events), 1);
    }
};
QTEST_GUILESS_MAIN(ProviderAgentE2eTest)
#include "test_provider_agent_e2e.moc"
