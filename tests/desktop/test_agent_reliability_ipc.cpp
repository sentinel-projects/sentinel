// SPDX-License-Identifier: GPL-3.0-or-later
#include "../support/DeterministicChatFixture.h"
#include "../support/ShortSocketPath.h"
#include "sentinel/core/agent/AgentRuntime.h"
#include "sentinel/core/agent/IAgentStepPlanner.h"
#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/app/WorkspaceService.h"
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
#include <QLocalServer>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <atomic>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace sentinel;
using desktop::DaemonClient;
using sentinel::test::shortSocketDirTemplate;
namespace {
constexpr int deadlineMs = 5000;
const QByteArray original = "synthetic original\n";
const QByteArray replacement = "authorized mutation\n";
struct Gate {
    QSemaphore entered, release;
    std::atomic_bool timedOut{false};
};
class MultiClientBackpressurePlanner final : public core::IAgentStepPlanner {
public:
    int totalSteps;
    explicit MultiClientBackpressurePlanner(int steps = 8) : totalSteps(steps) {}
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
class MutationPlanner final : public core::IAgentStepPlanner {
public:
    MutationPlanner(QString path, std::shared_ptr<Gate> gate)
        : path_(std::move(path)), gate_(std::move(gate)) {}
    core::AgentStepDecision nextStep(const QString&,
                                     const QList<core::AgentStepRecord>& history) const override {
        core::AgentStepDecision step;
        if (history.isEmpty()) {
            step.kind = core::AgentStepDecision::Kind::ToolCall;
            step.toolId = "write-file";
            step.toolName = "write-file";
            step.riskLevel = core::ToolRiskLevel::High;
            step.arguments = {{"path", path_}, {"content", QString::fromUtf8(replacement)}};
        } else {
            gate_->entered.release(); // Write receipt has already returned, run still active.
            if (!gate_->release.tryAcquire(1, deadlineMs))
                gate_->timedOut.store(true);
            step.kind = core::AgentStepDecision::Kind::FinalAnswer;
            step.answer = "Authorized fixture receipt recorded.";
            step.grounding = core::GroundingMode::Context;
            step.groundingDeclared = true;
        }
        return step;
    }

private:
    QString path_;
    std::shared_ptr<Gate> gate_;
};
class MutationExecutor final : public core::IToolExecutor {
public:
    explicit MutationExecutor(QString path) : path_(std::move(path)) {}
    mutable std::atomic_int calls{0}, unauthorized{0}, contextReads{0};
    core::ToolExecutionResult execute(const core::ToolExecutionRequest& request) const override {
        if (request.plan.invocations.size() == 1 &&
            (request.plan.invocations.first().toolId == "glob" ||
             request.plan.invocations.first().toolId == "memory-search")) {
            ++contextReads;
            return {core::ToolExecutionStatus::Succeeded, QStringLiteral("Fixture result")};
        }
        ++calls;
        if (request.plan.invocations.size() != 1) {
            ++unauthorized;
            return {core::ToolExecutionStatus::Blocked, "Unexpected fixture batch"};
        }
        const auto& invocation = request.plan.invocations.first();
        QHash<QString, QString> args;
        for (const auto& arg : invocation.arguments)
            args.insert(arg.id, arg.value);
        if (invocation.toolId != "write-file" || args.value("path") != path_ ||
            args.value("content").toUtf8() != replacement) {
            ++unauthorized;
            return {core::ToolExecutionStatus::Blocked, "Unprescribed fixture mutation"};
        }
        QFile file(path_);
        if (!file.open(QIODevice::WriteOnly) || file.write(replacement) != replacement.size())
            return {core::ToolExecutionStatus::Failed, "Fixture write failed"};
        file.close();
        core::ToolExecutionResult result{core::ToolExecutionStatus::Succeeded,
                                         "Fixture write receipt"};
        result.mutations.append({path_});
        return result;
    }

private:
    QString path_;
};
QMap<QString, QByteArray> manifest(const QString& root) {
    QMap<QString, QByteArray> result;
    QDirIterator files(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (!file.open(QIODevice::ReadOnly))
            return {{"<unreadable>", {}}};
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
struct Fixture {
    QTemporaryDir directory{shortSocketDirTemplate("sentinel-ipc")};
    core::AppSettings settings{std::make_unique<core::InMemorySettingsStore>(),
                               core::inMemoryTestCredentialStore()};
    test::DeterministicModelServiceFixture models{test::DeterministicChatReply::Final, &settings};
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    MutationExecutor* executor = nullptr;
    std::unique_ptr<core::ApplicationController> controller;
    std::unique_ptr<daemon::DaemonIpcServer> server;
    QString root, path, artifact;
    QMap<QString, QByteArray> before;
    bool start(std::unique_ptr<core::IAgentStepPlanner> customPlanner = nullptr) {
        if (!directory.isValid())
            return false;
        root = directory.filePath("workspace");
        if (!QDir().mkpath(root))
            return false;
        root = QFileInfo(root).canonicalFilePath();
        artifact = QDir(root).filePath("allowed.txt");
        for (const auto& name : {QString("allowed.txt"), QString("unchanged.txt")}) {
            QFile file(QDir(root).filePath(name));
            if (!file.open(QIODevice::WriteOnly) || file.write(original) != original.size())
                return false;
        }
        before = manifest(root);
        auto counted = std::make_unique<MutationExecutor>(artifact);
        executor = counted.get();
        core::ApplicationControllerBuilder builder;
        controller =
            builder.withOllamaRuntimeClient(std::make_unique<CatalogClient>())
                .withModelService(models.takeModelService())
                .withMemoryStore(std::make_unique<core::InMemoryStore>())
                .withAgentRuntime(std::make_unique<core::NullAgentRuntime>(
                    core::NullAgentRuntime::standardTools()))
                .withAgentStepPlanner(customPlanner
                                          ? std::move(customPlanner)
                                          : std::make_unique<MutationPlanner>(artifact, gate))
                .withToolExecutor(std::move(counted))
                .withSandboxPolicy(std::make_unique<core::StaticSandboxPolicy>(
                    QSet<QString>{"tool.metadata.read", "tool.risk.medium", "tool.risk.high"}))
                .build();
        settings.setNetworkMode("LocalOnly");
        core::WorkspaceService workspaces;
        const auto created = workspaces.createWorkspace({}, "IPC corpus", "Coding");
        const auto rooted =
            workspaces.setWorkspaceRoot(created.catalogJson, created.selectedWorkspaceId, root);
        if (!rooted.success)
            return false;
        settings.setWorkspaceCatalogJson(rooted.catalogJson);
        settings.setSelectedWorkspaceId(created.selectedWorkspaceId);
        settings.setWorkspaceProfilesJson(workspaces.updateProfile(
            {}, created.selectedWorkspaceId, {},
            {{"providerId", "ollama"}, {"modelId", "sentinel-test-model"}}));
        controller->attachControlledTaskSettings(settings);
        controller->setAgentAutonomousMode(
            false); // Explicit one-use IPC approval, never auto-grant.
        server = std::make_unique<daemon::DaemonIpcServer>(controller.get());
        server->setSettings(&settings);
        path = directory.filePath("daemon.sock");
        return server->startServer(path);
    }
    ~Fixture() {
        gate->release.release(); // Always release before controller joins borrowed workers.
        server.reset();
        controller.reset();
    }
    bool exactMutation() const {
        auto expected = before;
        expected["allowed.txt"] = QCryptographicHash::hash(replacement, QCryptographicHash::Sha256);
        return manifest(root) == expected && executor->calls.load() == 1 &&
               executor->unauthorized.load() == 0 && executor->contextReads.load() == 1;
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
            for (const auto& row : replies)
                if (row[0].toString() == id) {
                    result = {true, true, row[2].toJsonObject(), {}};
                    return true;
                }
            for (const auto& row : errors)
                if (row[0].toString() == id) {
                    result = {true, false, {}, row[2].toString()};
                    return true;
                }
            return false;
        },
        deadlineMs);
    return result;
}
QJsonObject wireEvent(const QSignalSpy& events, const QString& name) {
    for (const auto& row : events)
        if (row[0].toString() == name)
            return row[1].toJsonObject();
    return {};
}
int terminalCount(const QSignalSpy& events) {
    int count = 0;
    for (const auto& row : events)
        if (QStringList{"run.completed", "run.failed", "run.cancelled"}.contains(row[0].toString()))
            ++count;
    return count;
}
int coreTerminalCount(const Fixture& fixture, const QString& runtimeSession) {
    int count = 0;
    for (const auto& e : fixture.controller->agentRuntime()->eventHistory(runtimeSession))
        if (e.type == core::AgentEventType::AgentCompleted ||
            e.type == core::AgentEventType::AgentFailed ||
            e.type == core::AgentEventType::AgentCancelled)
            ++count;
    return count;
}
// Transparent local-socket relay: every normal request reaches the actual daemon.
// It records real daemon frames and can withhold/replay frames on the client wire.
// No daemon response, model, runtime or approval implementation is replaced here.
class Relay final : public QLocalServer {
public:
    QLocalSocket* downstream = nullptr;
    QLocalSocket* upstream = nullptr;
    QList<QJsonObject> frames, injected;
    QJsonObject heldTerminal;
    QList<QByteArray> heldSnapshots;
    bool holdTerminal = true;
    explicit Relay(const QString& daemonPath) {
        connect(this, &QLocalServer::newConnection, this, [this, daemonPath] {
            downstream = nextPendingConnection();
            upstream = new QLocalSocket(this);
            connect(downstream, &QLocalSocket::readyRead, upstream,
                    [this] { upstream->write(downstream->readAll()); });
            connect(upstream, &QLocalSocket::readyRead, downstream, [this] {
                while (upstream->canReadLine()) {
                    const auto bytes = upstream->readLine();
                    const auto frame = QJsonDocument::fromJson(bytes).object();
                    frames.append(frame);
                    const auto name = frame.value("name").toString();
                    if (holdTerminal && frame.value("type") == "event" &&
                        QStringList{"run.completed", "run.cancelled", "run.failed"}.contains(name))
                        heldTerminal = frame;
                    else if (holdTerminal && !heldTerminal.isEmpty() &&
                             frame.value("type") == "response" &&
                             QStringList{"session.attach", "session.list", "desktop.projection"}
                                 .contains(name))
                        heldSnapshots.append(
                            bytes); // Prevent timer snapshots bypassing the delayed terminal.
                    else
                        downstream->write(bytes);
                }
            });
            upstream->connectToServer(daemonPath);
        });
    }
    void releaseTerminal() {
        holdTerminal = false;
        inject(heldTerminal);
        for (const auto& bytes : heldSnapshots)
            downstream->write(bytes);
        heldSnapshots.clear();
    }
    void inject(const QJsonObject& frame) {
        injected.append(frame);
        downstream->write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
    }
};
} // namespace

class AgentReliabilityIpcTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void e2eDeniedOrCancelledMutation_data();
    void e2eDeniedOrCancelledMutation();
    void cor17_disconnectReattachDuringActiveRun();
    void cor18_staleDuplicateForeignEventsIgnored_data();
    void cor18_staleDuplicateForeignEventsIgnored();
    void cor19_eventSequenceMonotonicAndGenerationFencing();
    void cor20_multiClientBackpressureAndBufferSaturation();
};
void AgentReliabilityIpcTest::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName("sentinel-p1012-" +
                                         QUuid::createUuid().toString(QUuid::WithoutBraces));
}
void AgentReliabilityIpcTest::cleanupTestCase() {
    for (auto location : {QStandardPaths::AppDataLocation, QStandardPaths::AppConfigLocation}) {
        const auto path = QStandardPaths::writableLocation(location);
        if (QDir(path).exists())
            QVERIFY(QDir(path).removeRecursively());
    }
}
void AgentReliabilityIpcTest::e2eDeniedOrCancelledMutation_data() {
    QTest::addColumn<bool>("cancel");
    QTest::newRow("denied") << false;
    QTest::newRow("cancelled") << true;
}
void AgentReliabilityIpcTest::e2eDeniedOrCancelledMutation() {
    QFETCH(bool, cancel);
    Fixture fixture;
    QVERIFY(fixture.start());
    DaemonClient client(fixture.path, deadlineMs, 10000);
    client.connectToDaemon();
    QTRY_VERIFY(client.daemonReachable());
    const auto created = call(client, DaemonClient::Command::session_create, {{"title", "QA-02A"}});
    QVERIFY(created.ok);
    const auto session = created.payload.value("session_id").toString();
    QSignalSpy events(&client, &DaemonClient::eventReceived);
    QVERIFY(call(client, DaemonClient::Command::agent_start,
                 {{"session_id", session}, {"text", "Apply prescribed fixture mutation"}})
                .ok);
    QTRY_VERIFY(!wireEvent(events, "approval.requested").isEmpty());
    const auto approval = wireEvent(events, "approval.requested");
    const auto run = approval.value("run_id");
    if (cancel) {
        QVERIFY(call(client, DaemonClient::Command::run_cancel, {{"run_id", run}}).ok);
    } else {
        QVERIFY(
            call(
                client, DaemonClient::Command::approval_respond,
                {{"run_id", run}, {"approval_id", approval.value("approval_id")}, {"allow", false}})
                .ok);
    }
    QTRY_COMPARE(terminalCount(events), 1);
    QVERIFY(wireEvent(events, "run.completed").isEmpty());
    QVERIFY(
        !call(client, DaemonClient::Command::approval_respond,
              {{"run_id", run}, {"approval_id", approval.value("approval_id")}, {"allow", true}})
             .ok);
    DaemonClient fresh(fixture.path, deadlineMs, 10000);
    fresh.connectToDaemon();
    QTRY_VERIFY(fresh.daemonReachable());
    const auto snapshot =
        call(fresh, DaemonClient::Command::session_attach, {{"session_id", session}});
    QVERIFY(snapshot.ok);
    QCOMPARE(snapshot.payload.value("state").toString(),
             cancel ? QString("cancelled") : QString("failed"));
    QCOMPARE(manifest(fixture.root), fixture.before);
    QCOMPARE(fixture.executor->calls.load(), 0);
    QCOMPARE(fixture.executor->unauthorized.load(), 0);
    QVERIFY(fixture.controller->persistentPermissionGrants().isEmpty());
    QCOMPARE(terminalCount(events), 1);
}
void AgentReliabilityIpcTest::cor17_disconnectReattachDuringActiveRun() {
    Fixture fixture;
    QVERIFY(fixture.start());
    DaemonClient actor(fixture.path, deadlineMs, 10000), observer(fixture.path, deadlineMs, 10000);
    actor.connectToDaemon();
    observer.connectToDaemon();
    QTRY_VERIFY(actor.daemonReachable() && observer.daemonReachable());
    const auto created = call(actor, DaemonClient::Command::session_create, {{"title", "COR-17"}});
    QVERIFY(created.ok);
    const auto session = created.payload.value("session_id").toString();
    QVERIFY(call(observer, DaemonClient::Command::session_attach, {{"session_id", session}}).ok);
    QSignalSpy actorEvents(&actor, &DaemonClient::eventReceived),
        observed(&observer, &DaemonClient::eventReceived);
    const auto started =
        call(actor, DaemonClient::Command::agent_start,
             {{"session_id", session}, {"text", "Apply prescribed fixture mutation"}});
    QVERIFY(started.ok);
    const auto run = started.payload.value("run_id").toString();
    QVERIFY(!run.isEmpty());
    QTRY_VERIFY(!wireEvent(actorEvents, "approval.requested").isEmpty());
    const auto approval = wireEvent(actorEvents, "approval.requested");
    QCOMPARE(fixture.executor->calls.load(), 0);
    QVERIFY(call(actor, DaemonClient::Command::approval_respond,
                 {{"run_id", run}, {"approval_id", approval.value("approval_id")}, {"allow", true}})
                .ok);
    QTRY_VERIFY(fixture.gate->entered.available() == 1);
    const auto runtimeSession = fixture.controller->activeAgentSessionId();
    QVERIFY(fixture.exactMutation());
    const auto generation = actor.serverGeneration();
    actor.disconnectFromDaemon();
    QVERIFY(!actor.daemonReachable());
    const auto active =
        call(observer, DaemonClient::Command::session_attach, {{"session_id", session}});
    QVERIFY(active.ok);
    QCOMPARE(active.payload.value("state").toString(), QString("running"));
    QCOMPARE(active.payload.value("run_id").toString(), run);
    QCOMPARE(terminalCount(observed), 0);
    DaemonClient fresh(fixture.path, deadlineMs, 10000);
    fresh.connectToDaemon();
    QTRY_VERIFY(fresh.daemonReachable());
    QCOMPARE(fresh.serverGeneration(), generation);
    QSignalSpy freshEvents(&fresh, &DaemonClient::eventReceived);
    for (int repeat = 0; repeat < 2; ++repeat) {
        const auto attached =
            call(fresh, DaemonClient::Command::session_attach, {{"session_id", session}});
        QVERIFY(attached.ok);
        QCOMPARE(attached.payload.value("session_id").toString(), session);
        QCOMPARE(attached.payload.value("run_id").toString(), run);
        QCOMPARE(attached.payload.value("server_generation").toString(), generation);
        QCOMPARE(attached.payload.value("state").toString(), QString("running"));
        QCOMPARE(attached.payload.value("provider_id").toString(), QString("ollama"));
        QCOMPARE(attached.payload.value("model_id").toString(), QString("sentinel-test-model"));
        QVERIFY(fixture.exactMutation());
    }
    fixture.gate->release.release();
    QTRY_VERIFY(!wireEvent(observed, "run.completed").isEmpty());
    QTRY_VERIFY(!wireEvent(freshEvents, "run.completed").isEmpty());
    QCOMPARE(terminalCount(observed), 1);
    QCOMPARE(terminalCount(freshEvents), 1);
    QCOMPARE(wireEvent(freshEvents, "run.completed").value("run_id").toString(), run);
    QCOMPARE(coreTerminalCount(fixture, runtimeSession), 1);
    const auto final =
        call(fresh, DaemonClient::Command::session_attach, {{"session_id", session}});
    QVERIFY(final.ok);
    QCOMPARE(final.payload.value("state").toString(), QString("completed"));
    QCOMPARE(final.payload.value("run_id").toString(), run);
    QVERIFY(fixture.exactMutation());
    QCOMPARE(terminalCount(observed), 1);
    QCOMPARE(terminalCount(freshEvents), 1);
    QCOMPARE(coreTerminalCount(fixture, runtimeSession), 1);
    QVERIFY(fixture.controller->persistentPermissionGrants().isEmpty());
    QVERIFY(!fixture.gate->timedOut.load());
}
void AgentReliabilityIpcTest::cor18_staleDuplicateForeignEventsIgnored_data() {
    QTest::addColumn<bool>("cancel");
    QTest::newRow("completed-terminal") << false;
    QTest::newRow("cancelled-terminal") << true;
}
void AgentReliabilityIpcTest::cor18_staleDuplicateForeignEventsIgnored() {
    QFETCH(bool, cancel);
    Fixture fixture;
    QVERIFY(fixture.start());
    Relay relay(fixture.path);
    const auto proxyPath = fixture.directory.filePath("proxy.sock");
    QVERIFY(relay.listen(proxyPath));
    DaemonClient transport(proxyPath, deadlineMs, 10000);
    desktop::DesktopRuntimeClient adapter(transport);
    QSignalSpy delivered(&transport, &DaemonClient::eventReceived);
    DaemonClient observer(fixture.path, deadlineMs, 10000);
    observer.connectToDaemon();
    QTRY_VERIFY(adapter.ready() && !adapter.sessionId().isEmpty() && observer.daemonReachable());
    const auto session = adapter.sessionId();
    QVERIFY(call(observer, DaemonClient::Command::session_attach, {{"session_id", session}}).ok);
    QSignalSpy observed(&observer, &DaemonClient::eventReceived);
    QVERIFY(adapter.dispatch("runAgentRequest", {QString("Apply prescribed fixture mutation")})
                .toBool());
    QTRY_VERIFY(!adapter.pendingApproval().value("approval_id").toString().isEmpty());
    const auto approval = adapter.pendingApproval();
    const auto run = adapter.runId();
    QVERIFY(!run.isEmpty());
    QVERIFY(adapter.dispatch("respondToAgentApproval", {true}).toBool());
    QTRY_VERIFY(fixture.gate->entered.available() == 1);
    QVERIFY(fixture.exactMutation());
    const auto runtimeSession = fixture.controller->activeAgentSessionId();
    QTRY_VERIFY(adapter.ready() &&
                adapter.value("conversationState").toString() == QStringLiteral("running"));
    if (cancel) {
        const auto cancelled = call(observer, DaemonClient::Command::run_cancel, {{"run_id", run}});
        QVERIFY(cancelled.ok);
    }
    fixture.gate->release.release();
    const auto terminalName = cancel ? QString("run.cancelled") : QString("run.completed");
    QTRY_VERIFY(!wireEvent(observed, terminalName).isEmpty());
    QTRY_VERIFY(!relay.heldTerminal.isEmpty());
    const auto canonical = wireEvent(observed, terminalName);
    const auto expectedState = cancel ? QString("cancelled") : QString("completed");
    QCOMPARE(coreTerminalCount(fixture, runtimeSession), 1);
    QCOMPARE(terminalCount(observed), 1);
    const auto independent =
        call(observer, DaemonClient::Command::session_attach, {{"session_id", session}});
    QVERIFY(independent.ok);
    QCOMPARE(independent.payload.value("state").toString(), expectedState);
    QCOMPARE(adapter.value("conversationState").toString(),
             QString("running")); // Terminal deliberately delayed on the client wire.
    const auto unchangedOutput = adapter.value("lastAgentResponse");
    const auto barrier = [&] { return call(transport, DaemonClient::Command::daemon_status).ok; };
    const auto rejected = [&](const QString& field, const QJsonValue& value) {
        auto bad = relay.heldTerminal;
        auto payload = bad.value("payload").toObject();
        payload[field] = value;
        payload["event_sequence"] = canonical.value("event_sequence").toInt() + 10000;
        bad["payload"] = payload;
        relay.inject(bad);
    };
    rejected("session_id", "foreign-session");
    rejected("run_id", "foreign-run");
    rejected("server_generation", "old-generation");
    QVERIFY(barrier());
    QCOMPARE(adapter.value("conversationState").toString(), QString("running"));
    QCOMPARE(adapter.value("lastAgentResponse"), unchangedOutput);
    // Old-sequence output delivered twice must neither change output nor advance the cursor.
    auto stalePayload = canonical;
    stalePayload["event_sequence"] = 1;
    stalePayload["text"] = "stale output must be ignored";
    const QJsonObject stale{{"version", QJsonObject{{"major", 1}, {"minor", 0}}},
                            {"type", "event"},
                            {"id", ""},
                            {"name", "output.delta"},
                            {"payload", stalePayload}};
    relay.inject(stale);
    relay.inject(stale);
    QVERIFY(barrier());
    QCOMPARE(adapter.value("lastAgentResponse"), unchangedOutput);
    relay.releaseTerminal(); // The actual daemon terminal, not a fabricated result.
    QTRY_COMPARE(adapter.value("conversationState").toString(), expectedState);
    QCOMPARE(adapter.value("lastAgentResponse").toString(), canonical.value("text").toString());
    relay.inject(relay.heldTerminal); // Duplicate terminal.
    auto late = relay.heldTerminal;
    auto latePayload = canonical;
    latePayload["event_sequence"] = canonical.value("event_sequence").toInt() + 20000;
    latePayload["state"] = cancel ? "completed" : "cancelled";
    latePayload["text"] = "late conflicting terminal";
    latePayload["detail"] = "late conflicting terminal";
    late["name"] = cancel ? "run.completed" : "run.cancelled";
    late["payload"] = latePayload;
    relay.inject(late);
    late["name"] = "run.started";
    latePayload["state"] = "running";
    late["payload"] = latePayload;
    relay.inject(late); // A delayed start must not reactivate the closed run.
    late["name"] = "approval.requested";
    auto lateApproval = approval;
    lateApproval["event_sequence"] = canonical.value("event_sequence").toInt() + 30000;
    late["payload"] = lateApproval;
    relay.inject(late);
    QVERIFY(barrier());
    QCOMPARE(adapter.value("conversationState").toString(), expectedState);
    QCOMPARE(adapter.value("lastAgentResponse").toString(), canonical.value("text").toString());
    // Prove every injected frame passed transport schema validation and reached the client.
    // State stability must not be a false pass caused by malformed-frame rejection.
    for (const auto& frame : relay.injected) {
        int actualCopies = 0;
        for (const auto& row : delivered)
            if (row[0].toString() == frame.value("name").toString() &&
                row[1].toJsonObject() == frame.value("payload").toObject())
                ++actualCopies;
        QCOMPARE(actualCopies, relay.injected.count(frame));
    }
    QVERIFY(adapter.pendingApproval().isEmpty());
    QVERIFY(!adapter.dispatch("respondToAgentApproval", {true}).toBool());
    const auto replay =
        call(observer, DaemonClient::Command::approval_respond,
             {{"run_id", run}, {"approval_id", approval.value("approval_id")}, {"allow", true}});
    QVERIFY(replay.received && !replay.ok);
    QCOMPARE(replay.error, QString("invalid-approval"));
    adapter.refresh();
    QTRY_VERIFY(adapter.ready());
    QCOMPARE(adapter.runId(), run);
    QCOMPARE(adapter.sessionId(), session);
    QCOMPARE(adapter.value("conversationState").toString(),
             independent.payload.value("state").toString());
    QCOMPARE(adapter.value("lastAgentResponse").toString(),
             independent.payload.value("output").toString());
    QCOMPARE(terminalCount(observed), 1);
    QCOMPARE(coreTerminalCount(fixture, runtimeSession), 1);
    QVERIFY(fixture.controller->persistentPermissionGrants().isEmpty());
    QVERIFY(fixture.exactMutation());
    QVERIFY(!fixture.gate->timedOut.load());
}
void AgentReliabilityIpcTest::cor19_eventSequenceMonotonicAndGenerationFencing() {
    Fixture fixture;
    QVERIFY(fixture.start());
    DaemonClient client(fixture.path, deadlineMs, 10000);
    client.connectToDaemon();
    QTRY_VERIFY(client.daemonReachable());

    const auto created = call(client, DaemonClient::Command::session_create, {{"title", "COR-19"}});
    QVERIFY(created.ok);
    const auto session = created.payload.value("session_id").toString();
    QVERIFY(call(client, DaemonClient::Command::session_attach, {{"session_id", session}}).ok);

    QSignalSpy clientEvents(&client, &DaemonClient::eventReceived);
    const auto started =
        call(client, DaemonClient::Command::agent_start,
             {{"session_id", session}, {"text", "Apply prescribed fixture mutation"}});
    QVERIFY(started.ok);
    const auto run = started.payload.value("run_id").toString();
    QVERIFY(!run.isEmpty());

    QTRY_VERIFY(!wireEvent(clientEvents, "approval.requested").isEmpty());
    const auto approval = wireEvent(clientEvents, "approval.requested");
    QCOMPARE(fixture.executor->calls.load(), 0);

    const auto gen = client.serverGeneration();
    QVERIFY(!gen.isEmpty());

    qint64 lastSeq = 0;
    for (const auto& row : clientEvents) {
        const auto payload = row[1].toJsonObject();
        const auto seq = payload.value("event_sequence").toInteger();
        if (seq > 0) {
            QVERIFY(seq > lastSeq);
            lastSeq = seq;
        }
        QCOMPARE(payload.value("server_generation").toString(), gen);
    }

    QVERIFY(call(client, DaemonClient::Command::approval_respond,
                 {{"run_id", run}, {"approval_id", approval.value("approval_id")}, {"allow", true}})
                .ok);
    QTRY_VERIFY(fixture.gate->entered.available() == 1);
    QVERIFY(fixture.exactMutation());
    fixture.gate->release.release();

    QTRY_VERIFY(!wireEvent(clientEvents, "run.completed").isEmpty());

    lastSeq = 0;
    int terminalEvents = 0;
    for (const auto& row : clientEvents) {
        const auto name = row[0].toString();
        const auto payload = row[1].toJsonObject();
        const auto seq = payload.value("event_sequence").toInteger();
        if (seq > 0) {
            QVERIFY(seq > lastSeq);
            lastSeq = seq;
        }
        if (QStringList{"run.completed", "run.failed", "run.cancelled"}.contains(name)) {
            ++terminalEvents;
        }
        QCOMPARE(payload.value("server_generation").toString(), gen);
        QCOMPARE(payload.value("session_id").toString(), session);
    }
    QCOMPARE(terminalEvents, 1);

    DaemonClient fresh(fixture.path, deadlineMs, 10000);
    fresh.connectToDaemon();
    QTRY_VERIFY(fresh.daemonReachable());
    const auto snap = call(fresh, DaemonClient::Command::session_attach, {{"session_id", session}});
    QVERIFY(snap.ok);
    QCOMPARE(snap.payload.value("state").toString(), QString("completed"));
    QCOMPARE(snap.payload.value("run_id").toString(), run);
    QCOMPARE(fixture.executor->calls.load(), 1);
    QVERIFY(fixture.exactMutation());
}

void AgentReliabilityIpcTest::cor20_multiClientBackpressureAndBufferSaturation() {
#ifdef Q_OS_UNIX
    Fixture fixture;
    QVERIFY(fixture.start(std::make_unique<MultiClientBackpressurePlanner>(8)));

    DaemonClient fastClient(fixture.path, deadlineMs, 10000);
    fastClient.connectToDaemon();
    QTRY_VERIFY(fastClient.daemonReachable());

    const auto created =
        call(fastClient, DaemonClient::Command::session_create, {{"title", "COR-20"}});
    QVERIFY(created.ok);
    const auto session = created.payload.value("session_id").toString();
    QVERIFY(call(fastClient, DaemonClient::Command::session_attach, {{"session_id", session}}).ok);

    int slowFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(slowFd >= 0);
    int rcvBuf = 8192;
    ::setsockopt(slowFd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto pathBytes = fixture.path.toLocal8Bit();
    std::memcpy(address.sun_path, pathBytes.constData(), pathBytes.size() + 1);
    QCOMPARE(::connect(slowFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

    const auto helloMsg =
        QJsonDocument(QJsonObject{{"version", QJsonObject{{"major", 1}, {"minor", 0}}},
                                  {"type", "request"},
                                  {"id", "h1"},
                                  {"name", "hello"},
                                  {"payload", QJsonObject{{"client_id", "slow-observer"},
                                                          {"major", 1},
                                                          {"minor", 0},
                                                          {"capabilities", QJsonArray{}}}}})
            .toJson(QJsonDocument::Compact) +
        '\n';
    QCOMPARE(::write(slowFd, helloMsg.constData(), helloMsg.size()), helloMsg.size());
    QTest::qWait(50);
    char buf[8192];
    ssize_t n = ::read(slowFd, buf, sizeof(buf));
    QVERIFY(n > 0);

    const auto attachMsg =
        QJsonDocument(QJsonObject{{"version", QJsonObject{{"major", 1}, {"minor", 0}}},
                                  {"type", "request"},
                                  {"id", "a1"},
                                  {"name", "session.attach"},
                                  {"payload", QJsonObject{{"session_id", session}}}})
            .toJson(QJsonDocument::Compact) +
        '\n';
    QCOMPARE(::write(slowFd, attachMsg.constData(), attachMsg.size()), attachMsg.size());
    QTest::qWait(50);
    n = ::read(slowFd, buf, sizeof(buf));
    QVERIFY(n > 0);

    ::fcntl(slowFd, F_SETFL, O_NONBLOCK);

    QSignalSpy fastEvents(&fastClient, &DaemonClient::eventReceived);
    const auto started = call(fastClient, DaemonClient::Command::agent_start,
                              {{"session_id", session}, {"text", "Run saturation task"}});
    QVERIFY(started.ok);

    QTRY_VERIFY_WITH_TIMEOUT(!wireEvent(fastEvents, "run.completed").isEmpty(), 15000);
    QCOMPARE(terminalCount(fastEvents), 1);

    char drainBuf[1024];
    while (::read(slowFd, drainBuf, sizeof(drainBuf)) > 0) {
    }
    const ssize_t closedRead = ::read(slowFd, drainBuf, sizeof(drainBuf));
    QVERIFY(closedRead <= 0);
    ::close(slowFd);

    DaemonClient fresh(fixture.path, deadlineMs, 10000);
    fresh.connectToDaemon();
    QTRY_VERIFY(fresh.daemonReachable());
    const auto snap = call(fresh, DaemonClient::Command::session_attach, {{"session_id", session}});
    QVERIFY(snap.ok);
    QCOMPARE(snap.payload.value("state").toString(), QString("completed"));
#else
    QSKIP("Slow client backpressure is a Unix platform contract.");
#endif
}

QTEST_MAIN(AgentReliabilityIpcTest)
#include "test_agent_reliability_ipc.moc"
