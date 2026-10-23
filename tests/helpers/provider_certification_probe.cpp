// SPDX-License-Identifier: GPL-3.0-or-later
// Explicit live certification helper; never run by CTest.
#include "ProviderCertificationRoot.h"
#include "sentinel/core/agent/AgentRuntime.h"
#include "sentinel/core/agent/LlmAgentRuntime.h"
#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationController.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/mcp/McpService.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/network/NetworkPolicyService.h"
#include "sentinel/core/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/RealToolExecutor.h"
#include "sentinel/core/security/StaticApprovalPolicy.h"
#include "sentinel/core/security/StaticSandboxPolicy.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <cstdio>
using namespace sentinel::core;
void report(const QJsonObject& value) {
    std::puts(QJsonDocument(value).toJson(QJsonDocument::Compact).constData());
    std::fflush(stdout);
}
// Diagnostic forwarding only: every request still executes on the real bound provider.
class SkillRequestAudit final : public IChatProvider {
    std::shared_ptr<IChatProvider> provider_;

public:
    explicit SkillRequestAudit(std::shared_ptr<IChatProvider> provider)
        : provider_(std::move(provider)) {}
    QString name() const override {
        return provider_->name();
    }
    ChatProviderStatus status() const override {
        return provider_->status();
    }
    ChatProviderConcurrency concurrency() const override {
        return provider_->concurrency();
    }
    bool supportsStreaming() const override {
        return provider_->supportsStreaming();
    }
    ChatProviderReply sendMessage(const QString& message) override {
        return provider_->sendMessage(message);
    }
    ChatProviderReply sendRequest(const QString& message,
                                  const ChatRequestOptions& options) override {
        report({{"skill_marker_in_model_request", message.contains("[SKILL_OK]")}});
        return provider_->sendRequest(message, options);
    }
    ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& delta,
                         const std::shared_ptr<std::atomic_bool>& cancellation) override {
        report({{"skill_marker_in_model_request", message.contains("[SKILL_OK]")}});
        return provider_->sendMessageStreaming(message, delta, cancellation);
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const QString certificationRoot = validatedProviderCertificationRoot(
        qEnvironmentVariable("SENTINEL_CERTIFICATION_WORKSPACE_ROOT"));
    if (certificationRoot.isEmpty()) {
        report({{"configuration_error",
                 "A canonical absolute existing readable workspace directory is required in "
                 "SENTINEL_CERTIFICATION_WORKSPACE_ROOT; no fallback is allowed."}});
        return 2;
    }
    if (args.size() < 4)
        return 2;
    const bool skillProbe = args.size() > 4 && args[4].startsWith("skill-");
    const bool pluginProbe = args.size() > 4 && args[4] == "plugin";
    const bool proseFinalProbe = args.size() > 4 && args[4] == "prose-final";
    bool proseFinalOk = true;
    QString skillPreferences;
    if (skillProbe || pluginProbe) {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName("sentinel-live-skill-" +
                                             QUuid::createUuid().toString(QUuid::WithoutBraces));
        skillPreferences = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                           "/skill_preferences.json";
    }
    const bool fileRoundtrip = args.size() > 4 && args[4] == "file-roundtrip";
    const auto fixtureFile = QDir(certificationRoot).filePath("agent-roundtrip.txt");
    if (fileRoundtrip && QFile::exists(fixtureFile)) {
        report(
            {{"configuration_error", "file-roundtrip refuses to overwrite an existing fixture"}});
        return 2;
    }
    AppSettings settings(std::make_unique<InMemorySettingsStore>());
    auto ownedModels = std::make_unique<ModelService>(&settings);
    auto& models = *ownedModels;
    const auto provider = args[1];
    const auto model = args[2];
    ProviderDiscoveryOutcome discovery;
    QList<OllamaModelSummary> catalog;
    if (provider == "llama-cpp-server") {
        const QUrl endpoint(
            qEnvironmentVariable("SENTINEL_CERTIFICATION_LOCAL_ENDPOINT", "http://127.0.0.1:8081"));
        if (!endpoint.isValid() || (endpoint.scheme() != "http" && endpoint.scheme() != "https") ||
            !QStringList{"127.0.0.1", "localhost", "::1"}.contains(endpoint.host()) ||
            !endpoint.userInfo().isEmpty() || endpoint.hasQuery() || endpoint.hasFragment()) {
            report({{"configuration_error",
                     "Local certification requires an explicit clean loopback endpoint"}});
            return 2;
        }
        models.setLlamaCppEndpoint(endpoint.toString());
        catalog = fetchLlamaCppModels(provider::openAiCompatibleModelsUrl(endpoint), 4000, {},
                                      &discovery);
    } else if (provider == "gemini") {
        report({{"credential_present", !settings.geminiApiKey().isEmpty()}});
        catalog = fetchGeminiCloudModels(settings.geminiApiKey(), 15000, nullptr, {}, &discovery);
    } else if (provider == "cloud-api") {
        const QUrl endpoint(qEnvironmentVariable("SENTINEL_CERTIFICATION_CLOUD_ENDPOINT"));
        if (endpoint.scheme() != "https" || !LMStudioConfig::isSecureTransportEndpoint(endpoint) ||
            endpoint.hasQuery() || endpoint.hasFragment()) {
            report({{"configuration_error", "An explicit clean HTTPS cloud endpoint is required"}});
            return 2;
        }
        settings.setCloudApiEndpoint(endpoint.toString());
        const auto config = models.providerConfig({provider, model});
        catalog = fetchOpenAiCloudModels(provider::openAiCompatibleModelsUrl(config.endpoint),
                                         config.apiKey, 15000, nullptr, {}, &discovery);
    } else
        return 2;
    models.acceptProviderDiscovery(provider, catalog, discovery,
                                   models.beginProviderHealthObservation());
    auto resolved = models.resolve(provider, model);
    report({{"binding_ok", resolved.ok()},
            {"reason", resolved.reason},
            {"capabilities", capabilitySnapshotSummary(resolved.binding.capabilities)}});
    if (!resolved.ok())
        return 1;
    if (skillProbe)
        resolved.provider = std::make_shared<SkillRequestAudit>(resolved.provider);
    if (args[3] == "network-final") {
        bool all = true;
        auto& policy = NetworkPolicyService::instance();
        for (auto mode : {NetworkMode::Online, NetworkMode::LocalOnly, NetworkMode::Offline}) {
            policy.setMode(mode);
            const auto reply = resolved.provider->sendMessage("Reply only NETWORK_OK.");
            const auto cloud = policy.check(QUrl("https://api.openai.com"));
            const auto modeName = mode == NetworkMode::Online      ? "Online"
                                  : mode == NetworkMode::LocalOnly ? "LocalOnly"
                                                                   : "Offline";
            report({{"mode", modeName},
                    {"local_success", reply.success},
                    {"message", reply.message},
                    {"cloud_policy", NetworkPolicyService::code(cloud)}});
            all &=
                reply.success && (mode == NetworkMode::Online ? cloud == NetworkDecision::Allowed
                                                              : cloud != NetworkDecision::Allowed);
        }
        policy.setMode(NetworkMode::Online);
        return all ? 0 : 1;
    }
    if (args[3] == "chat-final") {
        ApplicationControllerBuilder builder;
        builder.withStandardDefaults(StandardPathProvider{}, settings);
        auto controller = builder.withModelService(std::move(ownedModels)).build();
        controller->setLlamaCppEndpoint("http://127.0.0.1:8081");
        models.acceptProviderDiscovery(provider, catalog, discovery,
                                       models.beginProviderHealthObservation());
        controller->setSelectedRuntimeProvider(provider);
        controller->setSelectedLocalModel(model);
        bool all = true;
        quint64 cancelledId = 0;
        for (const auto& prompt : QStringList{"Yalnızca MAVI yaz.", "1'den 10'a kadar sırayla yaz.",
                                              "Çok uzun bir hikaye yaz. En az 4000 kelime olsun.",
                                              "Yalnızca DEVAM yaz."}) {
            const bool cancel = prompt.contains("4000");
            QEventLoop wait;
            QTimer poll, deadline;
            bool stopped = false, timeout = false;
            int deltas = 0;
            QString lastPartial;
            QObject::connect(&poll, &QTimer::timeout, &app, [&] {
                const auto history = controller->chatHistory();
                if (history.isEmpty())
                    return;
                const auto& last = history.last();
                if (last.content != lastPartial) {
                    ++deltas;
                    lastPartial = last.content;
                }
                if (cancel && !stopped && last.role == ChatRole::Assistant &&
                    !last.content.isEmpty() &&
                    (last.status == ChatMessageStatus::Sending ||
                     last.status == ChatMessageStatus::Streaming)) {
                    stopped = controller->stopChatGeneration();
                    cancelledId = last.id;
                }
                if (last.role == ChatRole::Assistant && last.status != ChatMessageStatus::Sending &&
                    last.status != ChatMessageStatus::Streaming)
                    wait.quit();
            });
            QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
                timeout = true;
                controller->stopChatGeneration();
                wait.quit();
            });
            deadline.setSingleShot(true);
            deadline.start(90000);
            poll.start(10);
            const bool accepted = controller->sendMessage(prompt);
            if (accepted)
                wait.exec();
            const auto history = controller->chatHistory();
            const auto last = history.isEmpty() ? ChatMessage{} : history.last();
            all &= accepted && !timeout &&
                   (cancel ? last.status == ChatMessageStatus::Cancelled
                           : last.status == ChatMessageStatus::Completed);
            report({{"prompt", prompt},
                    {"accepted", accepted},
                    {"message", last.content},
                    {"status", chatMessageStatusName(last.status)},
                    {"deltas", deltas},
                    {"stopAccepted", stopped},
                    {"timeout", timeout},
                    {"rows", history.size()}});
        }
        int cancelledRows = 0;
        for (const auto& message : controller->chatHistory())
            if (message.id == cancelledId && message.status == ChatMessageStatus::Cancelled)
                ++cancelledRows;
        int streamedDeltas = 0;
        auto stream = resolved.provider->sendMessageStreaming(
            "1'den 10'a kadar sırayla yaz.", [&](const QString&) { ++streamedDeltas; }, {});
        report({{"independent_stream_success", stream.success},
                {"deltas", streamedDeltas},
                {"message", stream.message},
                {"error", stream.errorMessage}});
        all &= stream.success && streamedDeltas > 0 && stream.message.contains("10");
        report({{"cancelled_row_preserved", cancelledRows == 1}, {"all", all}});
        return all && cancelledRows == 1 ? 0 : 1;
    }
    if (args[3] == "chat") {
        auto controller =
            ApplicationControllerBuilder().withModelService(std::move(ownedModels)).build();
        controller->setSelectedRuntimeProvider(provider);
        controller->setSelectedLocalModel(model);
        for (const auto& prompt :
             QStringList{"Yalnızca MAVI yaz.", "Bir önceki cevabını aynen tekrar et."}) {
            QEventLoop chatLoop;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &app, [&] {
                const auto history = controller->chatHistory();
                if (!history.isEmpty() && chatMessageStatusName(history.last().status) != "sending")
                    chatLoop.quit();
            });
            poll.start(100);
            const bool accepted = controller->sendMessage(prompt);
            if (accepted)
                chatLoop.exec();
            const auto messages = controller->chatHistory();
            const auto last = messages.isEmpty() ? ChatMessage{} : messages.last();
            report({{"prompt", prompt},
                    {"accepted", accepted},
                    {"message", last.content},
                    {"status", chatMessageStatusName(last.status)},
                    {"provider", last.providerUsed},
                    {"model", last.modelUsed}});
        }
        if (args.size() > 4 && args[4] == "continuity-only")
            return 0;
        int deltas = 0;
        auto streamed = resolved.provider->sendMessageStreaming(
            "1'den 20'ye kadar sayıları ayrı satırlarda yaz.", [&](const QString&) { ++deltas; },
            {});
        report({{"stream_success", streamed.success},
                {"deltas", deltas},
                {"message", streamed.message},
                {"error", streamed.errorMessage}});
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        QTimer::singleShot(1500, &app, [cancellation] { cancellation->store(true); });
        auto cancelled = resolved.provider->sendMessageStreaming(
            "Çok uzun bir hikaye yaz. En az 2000 kelime olsun.", [](const QString&) {},
            cancellation);
        report({{"cancel_success", cancelled.success},
                {"cancel_category", chatProviderErrorCategoryName(cancelled.category)}});
        auto recovery = resolved.provider->sendMessage("Yalnızca DEVAM yaz.");
        report({{"post_cancel_success", recovery.success},
                {"message", recovery.message},
                {"error", recovery.errorMessage}});
        return 0;
    }
    auto descriptors = BuiltInToolProvider::descriptors();
    const QString exactCommand =
        args[3] == "pwd komutunu çalıştır ve yalnızca gerçek çıktıyı söyle."  ? "pwd"
        : args[3] == "git status --short komutunu çalıştır ve sonucu özetle." ? "git status --short"
                                                                              : QString();
    // Narrow the probe's tool contract, not the production permission policy.
    if (!exactCommand.isEmpty())
        for (auto& descriptor : descriptors)
            if (descriptor.id == "run-command") {
                auto properties = descriptor.inputSchema.value("properties").toObject();
                auto command = properties.value("command").toObject();
                command.insert("enum", QJsonArray{exactCommand});
                properties.insert("command", command);
                descriptor.inputSchema.insert("properties", properties);
            }
    if (fileRoundtrip) {
        for (auto& descriptor : descriptors) {
            if (descriptor.id != "write-file" && descriptor.id != "read-file" &&
                descriptor.id != "stat-path")
                continue;
            auto properties = descriptor.inputSchema.value("properties").toObject();
            auto path = properties.value("path").toObject();
            path.insert("enum", QJsonArray{fixtureFile});
            properties.insert("path", path);
            descriptor.inputSchema.insert("properties", properties);
        }
    }
    LlmAgentRuntime planner(descriptors, resolved.provider.get());
    planner.bindModel(resolved.binding, resolved.provider);
    if (fileRoundtrip)
        planner.setStreamObserver([](const QString&) {});
    RealToolExecutor executor;
    std::shared_ptr<McpService> mcp;
    if (args.size() > 4 && args[4] == "mcp") {
        mcp = std::make_shared<McpService>();
        McpServerConfig config;
        config.name = "certification";
        config.type = "local";
        config.command = QCoreApplication::applicationDirPath() + "/test_mcp_server";
        config.arguments = {"", "--certification"};
        mcp->addServer(config);
        if (!mcp->connectToServer(config.name)) {
            report({{"mcp_error", mcp->lastError(config.name)}});
            return 1;
        }
        executor.setMcpService(mcp);
    }
    StaticApprovalPolicy approval;
    StaticSandboxPolicy sandbox;
    AgentRuntime runtime(std::make_unique<NullAgentRuntime>(descriptors), planner, executor,
                         approval, sandbox);
    QStringList pluginTools;
    if (pluginProbe) {
        auto& plugins = runtime.pluginManager();
        const auto root = QCoreApplication::applicationDirPath() +
                          "/../plugins/samples/custom_agent_tool/custom-tool";
        report({{"plugins_discovered", plugins.discoverPlugins(root)}});
        const QString id = "dev.sentinel.plugin.custom-tool";
        if (!plugins.startPlugin(id)) {
            report({{"plugin_failure", plugins.descriptor(id)->failureCategory}});
            return 1;
        }
        report({{"plugin_host_running", plugins.descriptor(id)->host->isRunning()}});
        for (const auto& tool : runtime.toolRegistry().enabledTools())
            if (tool.source == ToolSource::Plugin) {
                pluginTools.append(tool.id);
                report({{"plugin_tool", tool.id}});
            }
    }
    if (skillProbe) {
        report(
            {{"skill_discovered", runtime.skillService().discoverSkills("tests/fixtures/skills")}});
        if (!runtime.skillService().setEnabled("certification", args[4] == "skill-enabled"))
            return 1;
        report({{"skill_content_available",
                 !runtime.skillService().getSkillContent("certification").isEmpty()}});
    }
    auto session = runtime.createSession();
    AgentSessionOptions options;
    options.workspaceContext.rootPath = certificationRoot;
    options.workspaceContext.id = "certification";
    options.workspaceName = "Sentinel";
    // Limit this diagnostic to the user's read-only certification operations.
    options.availableToolIds = {"list-directory"};
    if (fileRoundtrip)
        options.availableToolIds = {"write-file", "read-file", "stat-path"};
    if (pluginProbe)
        options.availableToolIds = pluginTools;
    if (mcp)
        options.availableToolIds = {"mcp.certification.add", "mcp.certification.echo_5f_value"};
    if (provider == "llama-cpp-server" && !exactCommand.isEmpty())
        options.availableToolIds = {"run-command"};
    if (provider == "llama-cpp-server" && args[3].contains("CMakeLists.txt"))
        options.availableToolIds = {"read-file"};
    if (provider == "llama-cpp-server" && args[3].contains("definitely-does-not-exist-12345.txt"))
        options.availableToolIds = {"glob"};
    if (proseFinalProbe)
        options.availableToolIds = {"read-file"};
    options.restrictAvailableTools = true;
    options.onStatus = [](const QString& status) { report({{"status", status}}); };
    options.onStep = [](const AgentStepRecord& step) {
        report({{"tool", step.toolId},
                {"observation", step.observation},
                {"status", step.statusText}});
    };
    QEventLoop loop;
    options.onFinished = [&](const AgentLoopState& state) {
        if (state.phase == AgentLoopPhase::AwaitingApproval) {
            report({{"state", "Awaiting Approval"}});
            return;
        }
        QJsonArray requirements;
        for (const auto& item : state.observationIntent.requirements)
            requirements.append(QJsonObject{{"domain", observationDomainName(item.domain)},
                                            {"resource", item.resourceHint}});
        report({{"requirements", requirements}, {"evidence_count", state.evidence.size()}});
        report({{"terminal", agentLoopPhaseName(state.phase)},
                {"final", state.finalAnswer},
                {"failure", state.abortReason},
                {"steps", state.steps.size()}});
        if (proseFinalProbe) {
            const auto answer = state.finalAnswer.trimmed();
            const bool canonical = answer.startsWith(QStringLiteral("{\"source\":")) ||
                                   answer.startsWith(QStringLiteral("{\"explanations\":")) ||
                                   answer.startsWith(QStringLiteral("{\"operations\":"));
            proseFinalOk =
                state.phase == AgentLoopPhase::Completed && !answer.isEmpty() && !canonical;
            report({{"prose_final_contract", proseFinalOk}});
        }
        loop.quit();
    };
    runtime.configureSession(session, options);
    QTimer approvalPoll;
    QObject::connect(&approvalPoll, &QTimer::timeout, &app, [&] {
        if (runtime.sessionState(session).phase == AgentLoopPhase::AwaitingApproval) {
            report({{"approval", "approved once"}});
            runtime.approve(session, false);
            runtime.continueSession(session, true);
        }
    });
    approvalPoll.start(100);
    QTimer bound;
    bound.setSingleShot(true);
    QObject::connect(&bound, &QTimer::timeout, &app, [&] {
        report({{"experiment", "bounded cancellation"}});
        runtime.cancel(session);
    });
    bound.start(180000);
    QString cancelledSession;
    const bool cancellationProbe = args.size() > 4 && args[4] == "agent-cancel";
    if (cancellationProbe) {
        QTimer::singleShot(100, &app, [&] { runtime.cancel(session); });
        runtime.start(session, "Inspect all workspace files in detail and write a long report.");
        loop.exec();
        if (runtime.sessionState(session).phase != AgentLoopPhase::Cancelled)
            return 1;
        report({{"cancelled_before_restart", true}});
        cancelledSession = session;
        session = runtime.createSession();
        runtime.configureSession(session, options);
    }
    const auto goal =
        fileRoundtrip
            ? QStringLiteral(
                  "Create the file %1 with exactly SENTINEL_FILE_OK as its complete content. "
                  "Then read it back and report the verified result. Do not touch any other file.")
                  .arg(fixtureFile)
            : args[3];
    runtime.start(session, goal);
    loop.exec();
    if (cancellationProbe) {
        const auto previous = runtime.sessionState(cancelledSession).phase;
        report({{"cancelled_session_after_next_run", agentLoopPhaseName(previous)}});
        if (previous != AgentLoopPhase::Cancelled)
            return 1;
    }
    if (pluginProbe) {
        const QString id = "dev.sentinel.plugin.custom-tool";
        auto host = runtime.pluginManager().descriptor(id)->host;
        const bool accepted = runtime.pluginManager().unloadPlugin(id);
        QEventLoop teardown;
        QTimer poll, deadline;
        QObject::connect(&poll, &QTimer::timeout, &app, [&] {
            if (!host->isRunning())
                teardown.quit();
        });
        QObject::connect(&deadline, &QTimer::timeout, &app, [&] { teardown.quit(); });
        deadline.setSingleShot(true);
        deadline.start(5000);
        poll.start(10);
        if (host->isRunning())
            teardown.exec();
        report({{"plugin_unload_accepted", accepted}, {"plugin_host_stopped", !host->isRunning()}});
        if (!accepted || host->isRunning())
            return 1;
    }
    if (skillProbe && QFile::exists(skillPreferences))
        QFile::remove(skillPreferences);
    if (fileRoundtrip) {
        QFile file(fixtureFile);
        const bool verified =
            file.open(QIODevice::ReadOnly) && file.readAll() == "SENTINEL_FILE_OK";
        report({{"fixture_content_verified", verified}});
        if (!verified)
            return 1;
    }
    if (!proseFinalOk)
        return 1;
    return runtime.sessionState(session).phase == AgentLoopPhase::Completed ? 0 : 1;
}
