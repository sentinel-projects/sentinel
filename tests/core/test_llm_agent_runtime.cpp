// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <algorithm>

#include "sentinel/core/agent/LlmAgentRuntime.h"
#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/model/ModelLibrary.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/IToolHandler.h"
#include "sentinel/core/runtime/InMemoryToolRegistry.h"
#include "sentinel/core/runtime/ToolOutputTruncator.h"

using namespace sentinel::core;

namespace {

class FakeChatProvider final : public IChatProvider {
public:
    QString name() const override {
        return QStringLiteral("FakeChatProvider");
    }
    ChatProviderStatus status() const override {
        return ChatProviderStatus::Ready;
    }

    ChatProviderReply sendMessage(const QString& message) override {
        prompts.append(message);
        if (failNext) {
            failNext = false;
            return {false, {}, QStringLiteral("provider offline")};
        }
        return {true, scriptedReply, {}};
    }

    ChatProviderReply sendRequest(const QString& message,
                                  const ChatRequestOptions& options) override {
        prompts.append(message);
        requestOptions.append(options);
        if (!options.structuredOutput && !options.nativeToolCalling) {
            if (failNext) {
                failNext = false;
                return {false, {}, QStringLiteral("provider offline")};
            }
            if (!scriptedPlainReplies.isEmpty())
                return {true, scriptedPlainReplies.takeFirst(), {}};
            return {true, scriptedReply, {}};
        }
        if (scriptedRequests.isEmpty())
            return {false, {}, QStringLiteral("missing scripted request")};
        return scriptedRequests.takeFirst();
    }

    QStringList prompts;
    QString scriptedReply;
    QStringList scriptedPlainReplies;
    QList<ChatProviderReply> scriptedRequests;
    QList<ChatRequestOptions> requestOptions;
    bool failNext = false;
};

AgentStepRecord sampleRecord() {
    AgentStepRecord record;
    record.index = 1;
    record.thought = QStringLiteral("first step");
    record.toolId = QStringLiteral("run-command");
    record.toolName = QStringLiteral("Run Command");
    record.statusText = QStringLiteral("Succeeded");
    record.observation = QStringLiteral("hello-from-step");
    return record;
}

} // namespace

class LlmAgentRuntimeTest final : public QObject {
    Q_OBJECT

private slots:
    void budgetRefusesBeforeProviderAndNeverReturnsSuccess() {
        auto provider = std::make_shared<FakeChatProvider>();
        provider->scriptedReply =
            "{\"action\":\"final\",\"grounding\":\"context\",\"answer\":\"Successful answer\"}";
        LlmAgentRuntime runtime({}, provider.get());
        ModelBinding binding;
        binding.providerId = "fixture";
        binding.modelId = "small";
        binding.capabilities.contextWindow = 1024;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::RuntimeReported;
        runtime.bindModel(binding, provider);
        const auto result = runtime.nextStep("Do the task", {});
        QCOMPARE(result.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(result.reason.startsWith("Context budget refused"));
        QVERIFY(provider->prompts.isEmpty());
        QCOMPARE(runtime.modelBinding().modelId, QString("small"));
    }
    void oversizedNativeObservationStopsBeforeFinalDispatch() {
        auto provider = std::make_shared<FakeChatProvider>();
        ChatProviderReply call;
        call.success = true;
        call.toolCalls = {{"call-1", "read-file", {{"path", "example"}}, {}}};
        provider->scriptedRequests = {call};
        ToolDescriptor tool;
        tool.id = "read-file";
        LlmAgentRuntime runtime({tool}, provider.get());
        ModelBinding binding;
        binding.providerId = "fixture";
        binding.modelId = "fixed";
        binding.capabilities.contextWindow = 8192;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::RuntimeReported;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        runtime.bindModel(binding, provider);
        QCOMPARE(runtime.nextStep("Read example", {}).kind, AgentStepDecision::Kind::ToolCall);
        auto observation = sampleRecord();
        observation.toolId = "read-file";
        observation.observation = QString(50000, 'x') + "NECESSARY_FAILURE_EVIDENCE";
        const auto result = runtime.nextStep("Read example", {observation});
        QCOMPARE(result.kind, AgentStepDecision::Kind::GiveUp);
        QCOMPARE(provider->prompts.size(), 1);
        QCOMPARE(runtime.budgetDiagnostics().value("status").toInt(),
                 static_cast<int>(ContextBudgetStatus::InsufficientCapacity));
    }
    void skippedNativeCallNeverReusesOldHistory_data() {
        QTest::addColumn<bool>("feedback");
        QTest::newRow("loop-skip-repair") << true;
        QTest::newRow("missing-result-fails-closed") << false;
    }
    void skippedNativeCallNeverReusesOldHistory() {
        QFETCH(bool, feedback);
        auto provider = std::make_shared<FakeChatProvider>();
        ChatProviderReply call;
        call.success = true;
        call.toolCalls.append({"new-call", "read-file", QJsonObject{{"path", "calc.py"}}, {}});
        ChatProviderReply final;
        final.success = true;
        final.message = "No new observation was executed.";
        provider->scriptedRequests = {call, final};
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), provider.get());
        ModelBinding binding;
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        runtime.bindModel(binding, provider);
        auto old = sampleRecord();
        old.toolId = "read-file";
        old.observation = "OLDER_OBSERVATION_SHOULD_NOT_BE_REBOUND";
        QCOMPARE(runtime.nextStep("Read calc.py", {old}).kind, AgentStepDecision::Kind::ToolCall);
        if (feedback)
            runtime.setPlannerFeedback(
                "Repeated read was not executed. Choose missing verification or final.");
        const auto decision = runtime.nextStep("Read calc.py", {old});
        if (feedback) {
            QCOMPARE(provider->requestOptions.size(), 2);
            QVERIFY(provider->requestOptions.last().priorToolCalls.isEmpty());
            QVERIFY(provider->requestOptions.last().toolResults.isEmpty());
            QCOMPARE(decision.kind, AgentStepDecision::Kind::FinalAnswer);
        } else {
            QCOMPARE(provider->requestOptions.size(), 1);
            QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
            QVERIFY(decision.reason.contains("missing"));
        }
    }
    void mismatchedNativeResultFailsClosed() {
        auto provider = std::make_shared<FakeChatProvider>();
        ChatProviderReply call;
        call.success = true;
        call.toolCalls.append({"read-call", "read-file", QJsonObject{{"path", "calc.py"}}, {}});
        provider->scriptedRequests = {call};
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), provider.get());
        ModelBinding binding;
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        runtime.bindModel(binding, provider);
        QCOMPARE(runtime.nextStep("Read calc.py", {}).kind, AgentStepDecision::Kind::ToolCall);
        auto wrong = sampleRecord();
        wrong.toolId = "git-diff";
        const auto rejected = runtime.nextStep("Read calc.py", {wrong});
        QCOMPARE(rejected.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(rejected.reason.contains("does not match"));
        QCOMPARE(provider->requestOptions.size(), 1);
    }

    void pendingVerificationIsExplicitAndDisappearsAfterObservation() {
        FakeChatProvider provider;
        provider.scriptedReply = R"({"action":"giveup","reason":"fixture stop"})";
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), &provider);
        ObservationIntent intent;
        intent.requirements = {{ObservationDomain::FileSystem, "/fixture/calc.py",
                                EvidenceFreshness::TurnScoped, ObservationPurpose::Operate},
                               {ObservationDomain::FileSystem, "/fixture/test_calc.py"},
                               {ObservationDomain::Workspace, "/fixture"}};
        intent.requirements[1].toolId = "read-file";
        intent.requirements[2].toolId = "git-diff";
        runtime.setObservationIntent(intent);
        runtime.setEvidence({});
        runtime.nextStep("Fix calc.py, run the test and inspect git-diff", {});
        auto pending = provider.prompts.last().section("REMAINING REQUIRED OBSERVATIONS", 1);
        QVERIFY(pending.contains("/fixture/test_calc.py"));
        QVERIFY(pending.contains("git-diff"));
        EvidenceRecord read;
        read.domain = ObservationDomain::FileSystem;
        read.resource = "/fixture/test_calc.py";
        read.scope = EvidenceScope::ExactResource;
        read.outcome = EvidenceOutcome::Verified;
        read.toolId = "read-file";
        runtime.setEvidence({read});
        runtime.nextStep("Fix calc.py, run the test and inspect git-diff", {});
        pending = provider.prompts.last().section("REMAINING REQUIRED OBSERVATIONS", 1);
        QVERIFY(!pending.contains("/fixture/test_calc.py"));
        QVERIFY(pending.contains("git-diff"));
        EvidenceRecord diff = read;
        diff.domain = ObservationDomain::Workspace;
        diff.resource = "/fixture";
        diff.toolId = "git-diff";
        runtime.setEvidence({read, diff});
        runtime.nextStep("Fix calc.py, run the test and inspect git-diff", {});
        QVERIFY(!provider.prompts.last().contains("REMAINING REQUIRED OBSERVATIONS"));
    }

    void focusesLocalCodingToolsWithoutExpandingProfile() {
        FakeChatProvider provider;
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), &provider);
        ObservationIntent intent;
        intent.requirements.append({ObservationDomain::FileSystem, QStringLiteral("fixture.cpp")});
        runtime.setObservationIntent(intent);
        const auto ids = [&] {
            QStringList result;
            for (const auto& tool : runtime.availableTools())
                result.append(tool.id);
            return result;
        };
        QVERIFY(ids().contains(QStringLiteral("read-file")));
        QVERIFY(ids().contains(QStringLiteral("git-diff")));
        QVERIFY(!ids().contains(QStringLiteral("write-file")));
        QVERIFY(!ids().contains(QStringLiteral("run-command")));
        QVERIFY(!ids().contains(QStringLiteral("web-search")));
        intent.requirements.first().purpose = ObservationPurpose::Operate;
        runtime.setObservationIntent(intent);
        QVERIFY(ids().contains(QStringLiteral("edit-file")));
        QVERIFY(ids().contains(QStringLiteral("run-command")));
        QVERIFY(ids().contains(QStringLiteral("todo-write")));
        QVERIFY(!ids().contains(QStringLiteral("voice-speak")));
        intent.requirements.append(
            {ObservationDomain::Network, QStringLiteral("https://example.test")});
        runtime.setObservationIntent(intent);
        QVERIFY(ids().contains(QStringLiteral("web-search")));
        runtime.setAllowedToolIds({QStringLiteral("read-file")});
        QCOMPARE(ids(), QStringList{QStringLiteral("read-file")});
    }
    void nativeRequestHonorsPerRequestDeadline_data() {
        QTest::addColumn<int>("requestedBudget");
        QTest::addColumn<int>("modelLimit");
        QTest::addColumn<int>("expectedBudget");
        QTest::newRow("unknown-small") << 128 << 0 << 128;
        QTest::newRow("unknown-reasoning-budget") << 2048 << 0 << 2048;
        QTest::newRow("known-model-cap") << 2048 << 512 << 512;
    }
    void nativeRequestHonorsPerRequestDeadline() {
        QFETCH(int, requestedBudget);
        QFETCH(int, modelLimit);
        QFETCH(int, expectedBudget);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QList<QTcpSocket*> sockets;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection();
            sockets.append(socket); // Accept and deliberately never respond.
        });
        ModelService service;
        service.setLmStudioEndpoint(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        ModelBinding binding;
        binding.providerId = "lm-studio";
        binding.modelId = "fixture";
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        binding.capabilities.contextWindow = 8192;
        if (modelLimit > 0)
            binding.capabilities.maxOutputTokens = modelLimit;
        auto provider = service.constructProvider(binding);
        ChatRequestOptions options;
        options.nativeToolCalling = true;
        options.deadlineMs = 100;
        options.maxOutputTokens = requestedBudget;
        QElapsedTimer timer;
        timer.start();
        const auto reply = provider->sendRequest("Synthetic stall", options);
        QVERIFY(!reply.success);
        QCOMPARE(reply.category, ChatProviderErrorCategory::Timeout);
        QCOMPARE(reply.attempts, 1);
        QVERIFY(timer.elapsed() < 2000);
        QCOMPARE(reply.diagnostics.value("requested_output_tokens").toInt(), expectedBudget);
        QCOMPARE(sockets.size(), 1);
    }

    void nativeOutputLimitIsNotAnEmptyResponse() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                const QByteArray body =
                    R"({"model":"fixture","choices":[{"finish_reason":"length","message":{"content":"","reasoning_content":"PRIVATE_REASONING","tool_calls":[]}}],"usage":{"prompt_tokens":123,"completion_tokens":1024,"completion_tokens_details":{"reasoning_tokens":1024}}})";
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                    QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        ModelService service;
        service.setLmStudioEndpoint(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        ModelBinding binding;
        binding.providerId = QStringLiteral("lm-studio");
        binding.modelId = QStringLiteral("fixture");
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        binding.capabilities.contextWindow = 8192;
        auto provider = service.constructProvider(binding);
        QVERIFY(provider);
        ChatRequestOptions options;
        options.nativeToolCalling = true;
        const auto reply = provider->sendRequest(QStringLiteral("PRIVATE_PROMPT"), options);
        QVERIFY(!reply.success);
        QCOMPARE(reply.category, ChatProviderErrorCategory::RequestRejected);
        QVERIFY(reply.errorMessage.contains(QStringLiteral("output budget")));
        QVERIFY(!reply.errorMessage.contains(QStringLiteral("PRIVATE_REASONING")));
        QVERIFY(reply.toolCalls.isEmpty());
        QCOMPARE(reply.diagnostics.value(QStringLiteral("finish_reason")).toString(),
                 QStringLiteral("length"));
        QCOMPARE(reply.diagnostics.value(QStringLiteral("completion_tokens")).toInt(), 1024);
        QCOMPARE(reply.diagnostics.value(QStringLiteral("reasoning_tokens")).toInt(), 1024);
    }
    void nativeDiagnostics_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QString>("outcome");
        QTest::addColumn<bool>("success");
        QTest::newRow("content-usage")
            << QByteArray(
                   R"({"choices":[{"finish_reason":"stop","message":{"content":"answer","reasoning_content":"PRIVATE_REASONING"}}],"usage":{"prompt_tokens":12,"completion_tokens":3,"total_tokens":15,"private_field":"PRIVATE_SECRET"}})")
            << 200 << QStringLiteral("content") << true;
        QTest::newRow("empty-without-usage")
            << QByteArray(
                   R"({"choices":[{"finish_reason":"stop","message":{"content":"","reasoning_content":"PRIVATE_REASONING"}}]})")
            << 200 << QStringLiteral("empty_response") << false;
        QTest::newRow("malformed-tool")
            << QByteArray(
                   R"({"choices":[{"finish_reason":"tool_calls","message":{"tool_calls":[{"id":"x","function":{"name":"read-file","arguments":"invalid"}}]}}]})")
            << 200 << QStringLiteral("response_format_failure") << false;
        QTest::newRow("length-with-call")
            << QByteArray(
                   R"({"choices":[{"finish_reason":"length","message":{"tool_calls":[{"id":"x","function":{"name":"read-file","arguments":"{}"}}]}}]})")
            << 200 << QStringLiteral("output_limit") << false;
        QTest::newRow("context-http400")
            << QByteArray(R"({"error":{"message":"Context size has been exceeded."}})") << 400
            << QStringLiteral("context_exhausted") << false;
        QTest::newRow("schema-http400")
            << QByteArray(R"({"error":{"message":"Invalid tool schema for get_context."}})") << 400
            << QStringLiteral("provider_request_rejected") << false;
    }
    void nativeDiagnostics() {
        QFETCH(QByteArray, body);
        QFETCH(int, httpStatus);
        QFETCH(QString, outcome);
        QFETCH(bool, success);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, body, httpStatus] {
                socket->readAll();
                socket->write("HTTP/1.1 " + QByteArray::number(httpStatus) +
                              " Reply\r\nContent-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" +
                              body);
                socket->disconnectFromHost();
            });
        });
        ModelService service;
        service.setLmStudioEndpoint(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        ModelBinding binding;
        binding.providerId = QStringLiteral("lm-studio");
        binding.modelId = QStringLiteral("fixture");
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        binding.capabilities.contextWindow = 8192;
        auto provider = service.constructProvider(binding);
        QVERIFY(provider);
        ChatRequestOptions options;
        options.nativeToolCalling = true;
        options.tools = BuiltInToolProvider::descriptors();
        options.priorToolCalls.append(
            {QStringLiteral("call-1"), QStringLiteral("read-file"),
             QJsonObject{{QStringLiteral("path"), QStringLiteral("PRIVATE_PATH")}}});
        options.toolResults.append(
            {QStringLiteral("call-1"), QStringLiteral("PRIVATE_OBSERVATION")});
        const auto reply = provider->sendRequest(QStringLiteral("PRIVATE_PROMPT"), options);
        QCOMPARE(reply.success, success);
        QCOMPARE(reply.httpStatus, httpStatus);
        QCOMPARE(reply.diagnostics.value(QStringLiteral("outcome")).toString(), outcome);
        QCOMPARE(reply.diagnostics.value(QStringLiteral("reported_context_tokens")).toInt(), 8192);
        QCOMPARE(reply.diagnostics.value(QStringLiteral("requested_output_tokens")).toInt(), 1024);
        QCOMPARE(reply.diagnostics.value(QStringLiteral("continuation_calls")).toInt(), 1);
        const auto sizes =
            reply.diagnostics.value(QStringLiteral("tool_observation_bytes")).toArray();
        QCOMPARE(sizes.size(), 1);
        QCOMPARE(sizes.first().toInt(), int(QByteArray("PRIVATE_OBSERVATION").size()));
        if (!success)
            QVERIFY(reply.toolCalls.isEmpty());
        const auto encoded = QJsonDocument(reply.diagnostics).toJson();
        QVERIFY(!encoded.contains("PRIVATE_"));
        QVERIFY(!encoded.contains("private_field"));
        if (success) {
            QCOMPARE(reply.diagnostics.value(QStringLiteral("prompt_tokens")).toInt(), 12);
            QCOMPARE(reply.diagnostics.value(QStringLiteral("total_tokens")).toInt(), 15);
        } else if (httpStatus == 200) {
            QVERIFY(!reply.diagnostics.contains(QStringLiteral("prompt_tokens")));
        }
        if (outcome == QLatin1String("context_exhausted"))
            QCOMPARE(reply.category, ChatProviderErrorCategory::RequestRejected);
        else if (httpStatus == 400)
            QCOMPARE(reply.category, ChatProviderErrorCategory::CapabilityUnsupported);
    }
    void byteBoundedToolPreviewRetainsFailureTailAndUtf8() {
        QTemporaryDir dir;
        TruncationConfig config;
        config.outputDir = dir.path();
        config.maxBytes = 512;
        config.previewLines = 50;
        const auto original = (QStringLiteral("build started\n") + QString(4000, QChar(0x0131)) +
                               QStringLiteral("\nTEST_FAILURE: expected 5 got -1"))
                                  .toUtf8();
        ToolOutputTruncator truncator(config);
        const auto result = truncator.truncate(original, QStringLiteral("run-command"));
        QVERIFY(result.truncated);
        QVERIFY(result.preview.toUtf8().size() <= config.maxBytes);
        QVERIFY(result.preview.contains(QStringLiteral("TEST_FAILURE: expected 5 got -1")));
        QVERIFY(!result.preview.contains(QChar(0xfffd)));
        QCOMPARE(truncator.readFullOutput(result.fullOutputPath), original);
    }
    void processObservationRetainsFailureTailInPlanningContext() {
        AgentContextInput input;
        input.goal = QStringLiteral("Diagnose the failed build");
        AgentStepRecord step;
        step.index = 1;
        step.toolId = QStringLiteral("run-command");
        step.statusText = QStringLiteral("Failed");
        step.observation = QStringLiteral("Command execution failed. (exit=1)\n") +
                           QString(12000, QLatin1Char('x')) +
                           QStringLiteral("\nTEST_FAILURE: expected 5 got -1");
        input.steps.append(step);
        const auto context = ContextEngine{}.build(input);
        bool found = false;
        for (const auto& item : context.items) {
            if (item.kind != AgentContextKind::Observation)
                continue;
            found = true;
            QVERIFY(item.content.contains(QStringLiteral("exit=1")));
            QVERIFY(item.content.contains(QStringLiteral("TEST_FAILURE: expected 5 got -1")));
            QVERIFY(item.content.contains(step.observation));
            QVERIFY(item.untrusted);
        }
        QVERIFY(found);
        QCOMPARE(input.steps.first().observation, step.observation);
    }
    void serializedNativeRequestIsBudgetedBeforeNetwork() {
        ModelService service;
        service.setLmStudioEndpoint(QStringLiteral("http://127.0.0.1:1"));
        ModelBinding binding;
        binding.providerId = QStringLiteral("lm-studio");
        binding.modelId = QStringLiteral("synthetic-budget-test");
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        binding.capabilities.contextWindow = 512;
        auto provider = service.constructProvider(binding);
        QVERIFY(provider);
        ChatRequestOptions options;
        options.nativeToolCalling = true;
        options.tools = BuiltInToolProvider::descriptors();
        const auto reply = provider->sendRequest(QStringLiteral("Read config.json"), options);
        QVERIFY(!reply.success);
        QCOMPARE(reply.category, ChatProviderErrorCategory::RequestRejected);
        QVERIFY(reply.errorMessage.startsWith(QStringLiteral("Context budget exceeded")));
        QCOMPARE(reply.httpStatus, 0);
        QVERIFY(!reply.diagnostics.value(QStringLiteral("transport_attempted")).toBool());
        QVERIFY(!reply.diagnostics.contains(QStringLiteral("prompt_tokens")));
    }
    void loadedContextIsNotPublishedMaximum() {
        LMStudioNativeCatalogAdapter catalog;
        QVERIFY(catalog.acceptResponse(
            R"({"models":[{"key":"m","max_context_length":1048576,"loaded_instances":[{"config":{"context_length":8192}},{"config":{"context_length":4096}}]}]})"));
        QCOMPARE(catalog.discoveredModels().first().capabilities.contextWindow.value_or(0), 4096);
        QVERIFY(catalog.acceptResponse(
            R"({"models":[{"key":"m","max_context_length":1048576,"loaded_instances":[]}]})"));
        QCOMPARE(catalog.discoveredModels().first().capabilities.contextWindow.value_or(0),
                 1048576);
    }
    void contextExhaustionStopsWithoutRetryOrClaimingSuccess() {
        // The native scripted response exercises provider context rejection.
        auto shared = std::make_shared<FakeChatProvider>();
        ChatProviderReply rejected;
        rejected.errorMessage = QStringLiteral("Context size has been exceeded.");
        rejected.category = ChatProviderErrorCategory::ProviderFailure;
        shared->scriptedRequests.append(rejected);
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), shared.get());
        ModelBinding binding;
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        runtime.bindModel(binding, shared);
        const auto decision = runtime.nextStep(QStringLiteral("Read math.h"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(decision.reason.startsWith(QStringLiteral("Context exhausted:")));
        QCOMPARE(shared->prompts.size(), 1);
    }
    void toolIndexPreservesDiscoveryUnderDetailedContractBudget() {
        AgentContextInput input;
        input.goal = QStringLiteral("Fix the arithmetic function");
        input.tools = BuiltInToolProvider::descriptors();
        std::sort(input.tools.begin(), input.tools.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        const auto context = ContextEngine{}.build(input);
        QString catalog;
        for (const auto& item : context.items)
            if (item.kind == AgentContextKind::Tool)
                catalog += item.content + QLatin1Char('\n');
        for (const auto& tool : input.tools)
            QVERIFY2(catalog.contains(tool.id + QLatin1Char('(')) ||
                         catalog.contains(tool.id + QStringLiteral(" |")),
                     qPrintable(tool.id));
        QVERIFY(catalog.contains(QStringLiteral("content!")));
        QVERIFY(context.estimatedTokens < 8192);
    }

    void responseProfileReachesPlannerAsPreferenceExactlyOnce() {
        FakeChatProvider provider;
        provider.scriptedReply = "{\"action\":\"final\",\"grounding\":\"context\",\"answer\":\"Helpful answer\"}";
        LlmAgentRuntime runtime({}, &provider);
        AgentContextInput input;
        input.goal = "Explain recursion";
        input.responseProfileInstructions = "Use [PROFILE_EXAMPLE] concrete examples.";
        runtime.setPlanningContext(ContextEngine{}.build(input));
        runtime.nextStep(input.goal, {});
        QCOMPARE(provider.prompts.last().count("[PROFILE_EXAMPLE]"), 1);
        QVERIFY(provider.prompts.last().contains("grants no tool, workspace, network or credential authority"));
        input.responseProfileInstructions.clear();
        runtime.setPlanningContext(ContextEngine{}.build(input));
        runtime.nextStep(input.goal, {});
        QVERIFY(!provider.prompts.last().contains("[PROFILE_EXAMPLE]"));
    }
    void budgetedSkillContextReachesProviderExactlyOnce() {
        FakeChatProvider provider;
        provider.scriptedReply = "{\"action\":\"final\",\"grounding\":\"context\",\"answer\":"
                                 "\"Specific contextual answer\"}";
        LlmAgentRuntime runtime({}, &provider);
        AgentContextInput input;
        input.goal = "task";
        Skill skill;
        skill.name = "test";
        skill.content = "Append [SKILL_OK].";
        input.skills = {skill};
        runtime.setPlanningContext(ContextEngine{}.build(input));
        runtime.nextStep(input.goal, {});
        QCOMPARE(provider.prompts.last().count("[SKILL_OK]"), 1);
        input.skills.first().state = SkillState::Disabled;
        runtime.setPlanningContext(ContextEngine{}.build(input));
        runtime.nextStep(input.goal, {});
        QVERIFY(!provider.prompts.last().contains("[SKILL_OK]"));
    }
    void registryChangesPlannerDiscovery() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral("{\"action\":\"final\",\"answer\":\"done\"}");
        LlmAgentRuntime runtime({}, &provider);
        InMemoryToolRegistry registry;
        ToolDescriptor first{QStringLiteral("tool-a"), QStringLiteral("Tool A"),
                             QStringLiteral("First tool")};
        ToolDescriptor second{QStringLiteral("tool-b"), QStringLiteral("Tool B"),
                              QStringLiteral("Second tool")};
        ToolDescriptor disabled{QStringLiteral("disabled-tool"), QStringLiteral("Disabled"),
                                QStringLiteral("Disabled tool")};
        disabled.enabled = false;
        class Handler final : public IToolHandler {
        public:
            IToolExecutor::Cancel execute(const ToolExecutionRequest&, const QString&,
                                          const QString&, IToolExecutor::Output,
                                          IToolExecutor::Completion completion) override {
                completion({ToolExecutionStatus::Succeeded, QStringLiteral("done")});
                return {};
            }
        };
        auto handler = std::make_shared<Handler>();
        QVERIFY(registry.registerTool({first, handler}));
        QVERIFY(registry.registerTool({second, handler}));
        QVERIFY(registry.registerTool({disabled, handler}));
        runtime.setToolRegistry(&registry);
        QCOMPARE(runtime.availableTools().size(), 2);
        runtime.nextStep(QStringLiteral("goal"), {});
        QVERIFY(provider.prompts.last().contains(QStringLiteral("tool-a")));
        QVERIFY(provider.prompts.last().contains(QStringLiteral("tool-b")));
        QVERIFY(!provider.prompts.last().contains(QStringLiteral("disabled-tool")));
        QVERIFY(registry.setEnabled(QStringLiteral("tool-a"), false));
        runtime.nextStep(QStringLiteral("goal"), {});
        QVERIFY(!provider.prompts.last().contains(QStringLiteral("tool-a")));
        QVERIFY(registry.setEnabled(QStringLiteral("tool-a"), true));
        runtime.nextStep(QStringLiteral("goal"), {});
        QVERIFY(provider.prompts.last().contains(QStringLiteral("tool-a")));
        QVERIFY(registry.unregisterTool(QStringLiteral("tool-b")));
        runtime.nextStep(QStringLiteral("goal"), {});
        QVERIFY(!provider.prompts.last().contains(QStringLiteral("tool-b")));
    }
    void turkishDirectoryRequestsNeverBecomeShellFallback() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral("malformed planner output");
        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        for (const QString& goal :
             {QStringLiteral("masaüstümde hangi dosyalar var bunu bana söyle"),
              QStringLiteral(
                  "~/Desktop dizinindeki dosya ve klasörleri listele. Bunun için uygun filesystem "
                  "tool'unu kullan, kullanıcı mesajımı shell komutu olarak çalıştırma.")}) {
            const auto decision = runtime.nextStep(goal, {});
            QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
            QCOMPARE(provider.prompts.size() % 2, 0);
        }
    }

    void modelCanSelectDirectoryToolForTurkishGoal() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral("{\"action\":\"tool\",\"tool\":\"list-directory\","
                                                "\"args\":{\"path\":\"~/Desktop\"}}");
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), &provider);
        const auto decision =
            runtime.nextStep(QStringLiteral("masaüstümde hangi dosyalar var bunu bana söyle"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::ToolCall);
        QCOMPARE(decision.toolId, QStringLiteral("list-directory"));
        QCOMPARE(decision.arguments.first().value, QStringLiteral("~/Desktop"));
    }

    void parsesPlainToolJson() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral(
            "{\"thought\":\"list files\",\"action\":\"tool\",\"tool\":\"run-command\","
            "\"args\":{\"command\":\"ls -la\"}}");

        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        const auto decision = runtime.nextStep(QStringLiteral("show home files"), {});

        QCOMPARE(decision.kind, AgentStepDecision::Kind::ToolCall);
        QCOMPARE(decision.toolId, QStringLiteral("run-command"));
        QVERIFY(decision.thought.isEmpty());
        QCOMPARE(decision.arguments.size(), 1);
        QCOMPARE(decision.arguments.first().id, QStringLiteral("command"));
        QCOMPARE(decision.arguments.first().value, QStringLiteral("ls -la"));
        QVERIFY(runtime.lastDecisionUsedLlm());
        QVERIFY(provider.prompts.first().contains(QStringLiteral("run-command")));
        QVERIFY(provider.prompts.first().contains(QStringLiteral("show home files")));
    }

    void parsesFencedJson() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral(
            "Here is my decision:\n```json\n{\"action\":\"tool\",\"tool\":\"web-search\","
            "\"args\":{\"query\":\"quantum news\"},\"thought\":\"search\"}\n```\nThanks.");

        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        const auto decision = runtime.nextStep(QStringLiteral("news"), {});

        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(!runtime.lastDecisionUsedLlm());
    }

    void parsesFinalAnswer() {
        FakeChatProvider provider;
        provider.scriptedReply =
            QStringLiteral("{\"action\":\"final\",\"grounding\":\"context\","
                           "\"answer\":\"Everything is done.\"}");

        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        const auto decision = runtime.nextStep(QStringLiteral("goal"), {});

        QCOMPARE(decision.kind, AgentStepDecision::Kind::FinalAnswer);
        QCOMPARE(decision.answer, QStringLiteral("Everything is done."));
        QVERIFY(runtime.lastDecisionUsedLlm());
    }

    void conversationalFinalDoesNotRequestTools() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral(
            "{\"action\":\"final\",\"grounding\":\"context\","
            "\"answer\":\"Merhaba! Sana nasıl yardımcı olabilirim?\"}");
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), &provider);
        runtime.setObservationIntent({});
        const auto decision = runtime.nextStep(QStringLiteral("merhaba"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::FinalAnswer);
        QCOMPARE(decision.grounding, GroundingMode::Context);
        QVERIFY(decision.toolId.isEmpty());
        QVERIFY(provider.prompts.first().contains(QStringLiteral("Tools are optional")));
        QVERIFY(!provider.prompts.first().contains(QStringLiteral("authorization=")));
    }

    void acceptsBriefConversationalGreeting() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral(
            "{\"action\":\"final\",\"grounding\":\"context\",\"answer\":\"Merhaba!\"}");
        LlmAgentRuntime runtime({}, &provider);
        runtime.setObservationIntent({});
        QCOMPARE(runtime.nextStep(QStringLiteral("Merhaba"), {}).kind,
                 AgentStepDecision::Kind::FinalAnswer);
    }

    void nativeProviderCanAnswerWithoutToolsOrEvidence() {
        auto provider = std::make_shared<FakeChatProvider>();
        ChatProviderReply reply;
        reply.success = true;
        reply.message = QStringLiteral("Hello! How can I help?");
        provider->scriptedRequests = {reply};
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), provider.get());
        ModelBinding binding;
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        runtime.bindModel(binding, provider);
        runtime.setObservationIntent({});
        const auto decision = runtime.nextStep(QStringLiteral("hello"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::FinalAnswer);
        QCOMPARE(decision.grounding, GroundingMode::Context);
        QVERIFY(decision.toolId.isEmpty());
    }

    void allExposedDescriptorsResolveWithoutDanglingMetadata() {
        const auto tools = BuiltInToolProvider::descriptors();
        FakeChatProvider provider;
        LlmAgentRuntime runtime(tools, &provider);
        const auto exposed = runtime.availableTools();
        QVERIFY(!exposed.isEmpty());
        for (const auto& tool : exposed) {
            QVERIFY(tool.enabled);
            QVERIFY(tool.exposedToModel);
            QVERIFY(toolSupportedOnCurrentPlatform(tool));
            QJsonObject arguments;
            const auto properties = tool.inputSchema.value("properties").toObject();
            for (const auto& required : tool.inputSchema.value("required").toArray()) {
                const auto name = required.toString();
                const auto field = properties.value(name).toObject();
                const auto choices = field.value("enum").toArray();
                const auto type = field.value("type").toString();
                arguments.insert(name, !choices.isEmpty() ? choices.first()
                                      : type == "integer" ? QJsonValue(1)
                                      : type == "array" ? QJsonValue(QJsonArray{})
                                      : type == "object" ? QJsonValue(QJsonObject{})
                                                         : QJsonValue("fixture"));
            }
            provider.scriptedReply = QString::fromUtf8(QJsonDocument(QJsonObject{
                {"action", "tool"}, {"tool", tool.id}, {"args", arguments}})
                    .toJson(QJsonDocument::Compact));
            const auto decision = runtime.nextStep(QStringLiteral("explicit tool request"), {});
            QCOMPARE(decision.kind, AgentStepDecision::Kind::ToolCall);
            QCOMPARE(decision.toolId, tool.id);
            QCOMPARE(decision.toolName, tool.name);
            QCOMPARE(decision.riskLevel, tool.riskLevel);
            QCOMPARE(decision.executionMode, tool.executionMode);
        }
        const auto appLaunch =
            std::find_if(tools.cbegin(), tools.cend(), [](const ToolDescriptor& t) {
                return t.id == QLatin1String("app-launch");
            });
        QVERIFY(appLaunch != tools.cend());
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
        // On Linux, app-launch detached desktop launching has no sandbox guarantee;
        // it must never be advertised as exposed or available.
        QVERIFY(!toolSupportedOnCurrentPlatform(*appLaunch));
        QVERIFY(std::none_of(exposed.cbegin(), exposed.cend(), [](const ToolDescriptor& t) {
            return t.id == QLatin1String("app-launch");
        }));
#else
        QVERIFY(toolSupportedOnCurrentPlatform(*appLaunch));
        QVERIFY(std::any_of(exposed.cbegin(), exposed.cend(), [](const ToolDescriptor& t) {
            return t.id == QLatin1String("app-launch");
        }));
#endif
    }

    void unsupportedPlatformCapabilityFailsClosedWithoutDispatch() {
        // Deterministic fixture: tool explicitly marked Unsupported on all platforms.
        ToolDescriptor unsupportedProbe;
        unsupportedProbe.id = QStringLiteral("unsupported-probe");
        unsupportedProbe.name = QStringLiteral("Unsupported Platform Probe");
        unsupportedProbe.category = QStringLiteral("System");
        unsupportedProbe.description =
            QStringLiteral("A capability not available on this platform.");
        unsupportedProbe.riskLevel = ToolRiskLevel::High;
        unsupportedProbe.executionMode = ToolExecutionMode::Local;
        unsupportedProbe.enabled = true;
        unsupportedProbe.exposedToModel = true;
        unsupportedProbe.inputSchema = QJsonObject{
            {QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("properties"),
             QJsonObject{{QStringLiteral("command"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}},
            {QStringLiteral("required"), QJsonArray{QStringLiteral("command")}}};
        unsupportedProbe.platforms.linux = ToolPlatformSupport::Unsupported;
        unsupportedProbe.platforms.macOS = ToolPlatformSupport::Unsupported;
        unsupportedProbe.platforms.windows = ToolPlatformSupport::Unsupported;

        QVERIFY(!toolSupportedOnCurrentPlatform(unsupportedProbe));

        auto catalog = BuiltInToolProvider::descriptors();
        catalog.append(unsupportedProbe);

        FakeChatProvider provider;
        LlmAgentRuntime runtime(catalog, &provider);

        // 1. Must never be exposed/advertised as available to the model.
        const auto available = runtime.availableTools();
        QVERIFY(std::none_of(available.cbegin(), available.cend(), [](const ToolDescriptor& t) {
            return t.id == QLatin1String("unsupported-probe");
        }));

        // 2. Unsolicited model proposal for this unsupported capability must fail closed.
        provider.scriptedReply = QStringLiteral("{\"action\":\"tool\",\"tool\":\"unsupported-"
                                                "probe\",\"args\":{\"command\":\"status\"}}");

        const auto decision = runtime.nextStep(QStringLiteral("run unsupported capability"), {});
        // Expected terminal outcome: GiveUp, never ToolCall.
        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        // No unauthorized tool dispatch.
        QVERIFY(decision.toolId.isEmpty());
        // No invented success or LLM acceptance.
        QVERIFY(!runtime.lastDecisionUsedLlm());
        // Bounded retry attempts: exactly initial proposal + 1 repair attempt, no infinite loop.
        QCOMPARE(provider.prompts.size(), 2);
        QVERIFY(provider.prompts.last().contains(QStringLiteral("Return one valid JSON action.")));

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
        // Linux-specific regression check for app-launch
        provider.prompts.clear();
        provider.scriptedReply = QStringLiteral(
            "{\"action\":\"tool\",\"tool\":\"app-launch\",\"args\":{\"app\":\"calculator\"}}");
        const auto linuxDecision = runtime.nextStep(QStringLiteral("launch calculator"), {});
        QCOMPARE(linuxDecision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(linuxDecision.toolId.isEmpty());
        QVERIFY(!runtime.lastDecisionUsedLlm());
        QCOMPARE(provider.prompts.size(), 2);
#endif
    }

    void disabledAndInternalToolsAreNeverAdvertisedOrDispatched() {
        ToolDescriptor disabledProbe;
        disabledProbe.id = QStringLiteral("disabled-probe");
        disabledProbe.name = QStringLiteral("Disabled Probe");
        disabledProbe.category = QStringLiteral("System");
        disabledProbe.description = QStringLiteral("Administratively disabled tool.");
        disabledProbe.riskLevel = ToolRiskLevel::Low;
        disabledProbe.executionMode = ToolExecutionMode::Local;
        disabledProbe.enabled = false;
        disabledProbe.exposedToModel = true;
        disabledProbe.inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};

        ToolDescriptor internalProbe;
        internalProbe.id = QStringLiteral("internal-probe");
        internalProbe.name = QStringLiteral("Internal Probe");
        internalProbe.category = QStringLiteral("Agent");
        internalProbe.description = QStringLiteral("Internal metadata tool; not for model.");
        internalProbe.riskLevel = ToolRiskLevel::Low;
        internalProbe.executionMode = ToolExecutionMode::MetadataOnly;
        internalProbe.enabled = true;
        internalProbe.exposedToModel = false;
        internalProbe.inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};

        auto catalog = BuiltInToolProvider::descriptors();
        catalog.append(disabledProbe);
        catalog.append(internalProbe);

        FakeChatProvider provider;
        LlmAgentRuntime runtime(catalog, &provider);

        // Neither disabled nor internal tools may be visible in available tools.
        const auto available = runtime.availableTools();
        QVERIFY(std::none_of(available.cbegin(), available.cend(), [](const ToolDescriptor& t) {
            return t.id == QLatin1String("disabled-probe");
        }));
        QVERIFY(std::none_of(available.cbegin(), available.cend(), [](const ToolDescriptor& t) {
            return t.id == QLatin1String("internal-probe");
        }));
        // Built-in local-plan-summary is also internal and must never be exposed.
        QVERIFY(std::none_of(available.cbegin(), available.cend(), [](const ToolDescriptor& t) {
            return t.id == QLatin1String("local-plan-summary");
        }));

        // Attempting to invoke disabled tool fails closed.
        provider.scriptedReply =
            QStringLiteral("{\"action\":\"tool\",\"tool\":\"disabled-probe\",\"args\":{}}");
        auto decision = runtime.nextStep(QStringLiteral("invoke disabled"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(decision.toolId.isEmpty());
        QVERIFY(!runtime.lastDecisionUsedLlm());

        // Attempting to invoke internal tool fails closed.
        provider.scriptedReply =
            QStringLiteral("{\"action\":\"tool\",\"tool\":\"internal-probe\",\"args\":{}}");
        decision = runtime.nextStep(QStringLiteral("invoke internal"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(decision.toolId.isEmpty());
        QVERIFY(!runtime.lastDecisionUsedLlm());
    }

    void missingHandlerOrUnavailableIntegrationFailsClosedInRegistry() {
        InMemoryToolRegistry registry;

        // 1. Tool descriptor with missing handler (nullptr).
        ToolDescriptor missingHandlerTool;
        missingHandlerTool.id = QStringLiteral("missing-handler-tool");
        missingHandlerTool.name = QStringLiteral("Missing Handler Tool");
        missingHandlerTool.category = QStringLiteral("System");
        missingHandlerTool.description = QStringLiteral("Tool without execution handler.");
        missingHandlerTool.enabled = true;
        missingHandlerTool.exposedToModel = true;
        missingHandlerTool.inputSchema =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};
        missingHandlerTool.platforms = {ToolPlatformSupport::Supported,
                                        ToolPlatformSupport::Supported,
                                        ToolPlatformSupport::Supported};
        QVERIFY(registry.registerTool(missingHandlerTool));

        // 2. Tool descriptor with unavailable OS integration.
        ToolDescriptor unavailableOsTool;
        unavailableOsTool.id = QStringLiteral("unavailable-os-tool");
        unavailableOsTool.name = QStringLiteral("Unavailable OS Tool");
        unavailableOsTool.category = QStringLiteral("System");
        unavailableOsTool.description = QStringLiteral("Tool requiring missing OS service.");
        unavailableOsTool.enabled = true;
        unavailableOsTool.exposedToModel = true;
        unavailableOsTool.inputSchema =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};
        unavailableOsTool.platforms = {ToolPlatformSupport::Supported,
                                       ToolPlatformSupport::Supported,
                                       ToolPlatformSupport::Supported};

        class FixtureUnavailableHandler final : public IToolHandler {
        public:
            QString unavailableReason() const override {
                return QStringLiteral("Fixture OS service is not present in this session.");
            }
            IToolExecutor::Cancel execute(const ToolExecutionRequest&, const QString&,
                                          const QString&, IToolExecutor::Output,
                                          IToolExecutor::Completion completion) override {
                completion({ToolExecutionStatus::Failed, unavailableReason()});
                return {};
            }
        };

        auto mockHandler = std::make_shared<FixtureUnavailableHandler>();
        QVERIFY(registry.registerTool(IToolRegistry::Registration{unavailableOsTool, mockHandler}));

        // Both are registered in registry listTools(), but excluded from enabledTools().
        QCOMPARE(registry.listTools().size(), 2);
        QCOMPARE(registry.enabledTools().size(), 0);

        FakeChatProvider provider;
        LlmAgentRuntime runtime({}, &provider);
        runtime.setToolRegistry(&registry);

        // Neither tool may be available to the agent runtime.
        QCOMPARE(runtime.availableTools().size(), 0);

        // Attempting to invoke missing-handler tool must fail closed with GiveUp.
        provider.scriptedReply =
            QStringLiteral("{\"action\":\"tool\",\"tool\":\"missing-handler-tool\",\"args\":{}}");
        auto decision = runtime.nextStep(QStringLiteral("run missing handler"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(decision.toolId.isEmpty());
        QVERIFY(!runtime.lastDecisionUsedLlm());

        // Attempting to invoke unavailable OS integration tool must fail closed with GiveUp.
        provider.scriptedReply =
            QStringLiteral("{\"action\":\"tool\",\"tool\":\"unavailable-os-tool\",\"args\":{}}");
        decision = runtime.nextStep(QStringLiteral("run unavailable os"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(decision.toolId.isEmpty());
        QVERIFY(!runtime.lastDecisionUsedLlm());
    }

    void descriptorSchemaIdentityAndPlatformContract() {
        const auto tools = BuiltInToolProvider::descriptors();
        QVERIFY(tools.size() >= 38);
        for (const auto& tool : tools) {
            QVERIFY(!tool.id.trimmed().isEmpty());
            QVERIFY(!tool.name.trimmed().isEmpty());
            QVERIFY(!tool.category.trimmed().isEmpty());
            QVERIFY(!tool.description.trimmed().isEmpty());

            // Schema identity: valid JSON Schema object.
            QCOMPARE(tool.inputSchema.value(QStringLiteral("type")).toString(),
                     QStringLiteral("object"));
            const auto properties = tool.inputSchema.value(QStringLiteral("properties")).toObject();
            const auto required = tool.inputSchema.value(QStringLiteral("required")).toArray();
            for (const auto& req : required) {
                QVERIFY2(
                    properties.contains(req.toString()),
                    qPrintable(QStringLiteral(
                                   "Tool %1 required property %2 missing from schema properties")
                                   .arg(tool.id, req.toString())));
            }

            // Platform support contract: every platform entry is classified.
            const auto validPlatformSupport = [](ToolPlatformSupport s) {
                return s == ToolPlatformSupport::Supported ||
                       s == ToolPlatformSupport::PartiallySupported ||
                       s == ToolPlatformSupport::Unsupported;
            };
            QVERIFY(validPlatformSupport(tool.platforms.macOS));
            QVERIFY(validPlatformSupport(tool.platforms.linux));
            QVERIFY(validPlatformSupport(tool.platforms.windows));

            // Platform invariants.
            if (tool.id == QLatin1String("app-launch")) {
                QCOMPARE(tool.platforms.linux, ToolPlatformSupport::Unsupported);
            }
        }
    }

    void repairsArgumentsAgainstRegisteredSchemaBeforeExecution() {
        FakeChatProvider provider;
        provider.scriptedPlainReplies = {
            QStringLiteral("{\"action\":\"tool\",\"tool\":\"read-file\",\"args\":{\"filename\":\"audit.txt\"}}"),
            QStringLiteral("{\"action\":\"tool\",\"tool\":\"read-file\",\"args\":{\"path\":\"audit.txt\"}}")};
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), &provider);
        const auto decision = runtime.nextStep(QStringLiteral("read audit.txt"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::ToolCall);
        QCOMPARE(decision.arguments.first().id, QStringLiteral("path"));
        QCOMPARE(provider.prompts.size(), 2);
        QVERIFY(provider.prompts.last().contains(QStringLiteral("Invalid arguments for read-file")));
    }

    void nativeArgumentRepairDoesNotPublishUnexecutedCalls() {
        auto provider = std::make_shared<FakeChatProvider>();
        ChatProviderReply invalid;
        invalid.success = true;
        invalid.toolCalls.append(
            {QStringLiteral("bad-call"),
             QStringLiteral("read-file"),
             QJsonObject{{QStringLiteral("filename"), QStringLiteral("audit.txt")}},
             {}});
        ChatProviderReply corrected;
        corrected.success = true;
        corrected.toolCalls.append(
            {QStringLiteral("good-call"),
             QStringLiteral("read-file"),
             QJsonObject{{QStringLiteral("path"), QStringLiteral("audit.txt")}},
             {}});
        ChatProviderReply final;
        final.success = true;
        final.message = QStringLiteral("Observed audit content.");
        provider->scriptedRequests = {invalid, corrected, final};
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), provider.get());
        ModelBinding binding;
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        runtime.bindModel(binding, provider);
        const auto tool = runtime.nextStep(QStringLiteral("Read audit.txt"), {});
        QCOMPARE(tool.kind, AgentStepDecision::Kind::ToolCall);
        QCOMPARE(provider->requestOptions.size(), 2);
        QVERIFY(provider->requestOptions.at(1).priorToolCalls.isEmpty());
        QVERIFY(provider->requestOptions.at(1).toolResults.isEmpty());
        auto record = sampleRecord();
        record.toolId = QStringLiteral("read-file");
        record.observation =
            QString(11970, QLatin1Char('x')) + QStringLiteral("\nTEST_FAILURE: expected 5 got -1");
        const auto answer = runtime.nextStep(QStringLiteral("Read audit.txt"), {record});
        QCOMPARE(answer.kind, AgentStepDecision::Kind::FinalAnswer);
        QCOMPARE(provider->requestOptions.last().toolResults.size(), 1);
        QCOMPARE(provider->requestOptions.last().toolResults.first().callId,
                 QStringLiteral("good-call"));
        const auto excerpt = provider->requestOptions.last().toolResults.first().content;
        QVERIFY(excerpt.contains(record.observation));
        QVERIFY(!excerpt.contains(QStringLiteral("excerpt truncated")));
        QVERIFY(excerpt.contains(QStringLiteral("TEST_FAILURE: expected 5 got -1")));
        QVERIFY(record.observation.size() >= 12000); // Authoritative evidence stays intact.
    }

    void unknownNativeCallCanRepairBeforeAnyExecution() {
        auto provider = std::make_shared<FakeChatProvider>();
        ChatProviderReply invalid;
        invalid.success = true;
        invalid.toolCalls.append(
            {QStringLiteral("bad-call"), QStringLiteral("invented-tool"), {}, {}});
        ChatProviderReply corrected;
        corrected.success = true;
        corrected.toolCalls.append(
            {QStringLiteral("good-call"),
             QStringLiteral("read-file"),
             QJsonObject{{QStringLiteral("path"), QStringLiteral("audit.txt")}},
             {}});
        provider->scriptedRequests = {invalid, corrected};
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), provider.get());
        ModelBinding binding;
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        runtime.bindModel(binding, provider);
        const auto tool = runtime.nextStep(QStringLiteral("Read audit.txt"), {});
        QCOMPARE(tool.kind, AgentStepDecision::Kind::ToolCall);
        QCOMPARE(tool.toolId, QStringLiteral("read-file"));
        QCOMPARE(provider->requestOptions.size(), 2);
        QVERIFY(provider->requestOptions.last().priorToolCalls.isEmpty());
    }

    void invalidNativeCallsStopAfterTwoPlanningAttempts() {
        auto provider = std::make_shared<FakeChatProvider>();
        ChatProviderReply invalid;
        invalid.success = true;
        invalid.toolCalls.append(
            {QStringLiteral("bad-call"), QStringLiteral("invented-tool"), {}, {}});
        provider->scriptedRequests = {invalid, invalid};
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), provider.get());
        ModelBinding binding;
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        runtime.bindModel(binding, provider);
        QCOMPARE(runtime.nextStep(QStringLiteral("Read audit.txt"), {}).kind,
                 AgentStepDecision::Kind::GiveUp);
        QCOMPARE(provider->requestOptions.size(), 2);
        QVERIFY(!runtime.lastDecisionUsedLlm());
        ChatProviderReply corrected;
        corrected.success = true;
        corrected.toolCalls.append(
            {QStringLiteral("fresh-call"),
             QStringLiteral("read-file"),
             QJsonObject{{QStringLiteral("path"), QStringLiteral("audit.txt")}},
             {}});
        provider->scriptedRequests = {corrected};
        QCOMPARE(runtime.nextStep(QStringLiteral("Read audit.txt"), {}).kind,
                 AgentStepDecision::Kind::ToolCall);
        QVERIFY(provider->requestOptions.last().priorToolCalls.isEmpty());
    }

    void normalizesOnlyRegisteredToolAction() {
        FakeChatProvider provider;
        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), &provider);
        provider.scriptedReply = QStringLiteral(
            "{\"action\":\"read-file\",\"args\":{\"path\":\"audit.txt\"}}");
        auto decision = runtime.nextStep(QStringLiteral("read audit.txt"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::ToolCall);
        QCOMPARE(decision.toolId, QStringLiteral("read-file"));
        provider.scriptedReply = QStringLiteral(
            "{\"action\":\"invented-tool\",\"args\":{}}");
        decision = runtime.nextStep(QStringLiteral("read audit.txt"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
    }

    void acceptsSingleFencedDecision() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral(
            "```json\n{\"action\":\"final\",\"grounding\":\"context\","
            "\"answer\":\"A useful explanation.\"}\n```");
        LlmAgentRuntime runtime({}, &provider);
        QCOMPARE(runtime.nextStep(QStringLiteral("explain"), {}).kind,
                 AgentStepDecision::Kind::FinalAnswer);
    }

    void acceptsNativeToolContinuationFinalWithEvidenceClaims() {
        auto provider = std::make_shared<FakeChatProvider>();
        ChatProviderReply toolCall;
        toolCall.success = true;
        toolCall.toolCalls.append({QStringLiteral("call-1"),
                                   QStringLiteral("list-directory"),
                                   QJsonObject{{QStringLiteral("path"), QStringLiteral(".")}},
                                   {}});
        ChatProviderReply finalReply;
        finalReply.success = true;
        finalReply.message = QStringLiteral("The workspace contains CMakeLists.txt.");
        provider->scriptedRequests = {toolCall, finalReply};

        LlmAgentRuntime runtime(BuiltInToolProvider::descriptors(), provider.get());
        ModelBinding binding;
        binding.providerId = QStringLiteral("local");
        binding.modelId = QStringLiteral("model");
        binding.capabilities.contextWindow = 131072;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProgrammaticMetadata;
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        runtime.bindModel(binding, provider);
        ObservationIntent intent;
        intent.requirements.append({ObservationDomain::FileSystem, QStringLiteral("CMakeLists.txt"),
                                    EvidenceFreshness::TurnScoped, ObservationPurpose::Inspect,
                                    ClaimType::FileExists, QStringLiteral("claim-1")});
        runtime.setObservationIntent(intent);
        runtime.setStructuredFacts({{QStringLiteral("claim-1"),
                                     QStringLiteral("CMakeLists.txt"),
                                     true,
                                     {QStringLiteral("call-1")}}});

        const auto tool = runtime.nextStep(QStringLiteral("List the workspace"), {});
        QCOMPARE(tool.kind, AgentStepDecision::Kind::ToolCall);
        QCOMPARE(tool.toolId, QStringLiteral("list-directory"));
        QVERIFY(provider->requestOptions.first().nativeToolCalling);
        QVERIFY(provider->requestOptions.first().tools.size() > 0);
        QVERIFY(!provider->prompts.first().contains(QStringLiteral("risk=")));
        QVERIFY(provider->prompts.first().contains(QStringLiteral("native tool protocol")));
        QVERIFY(!provider->prompts.first().contains(QStringLiteral("Return one JSON decision")));
        QVERIFY(!provider->prompts.first().contains(QStringLiteral("NEXT JSON ACTION")));

        auto record = sampleRecord();
        record.toolId = QStringLiteral("list-directory");
        record.observation = QStringLiteral("CMakeLists.txt");
        const auto final = runtime.nextStep(QStringLiteral("List the workspace"), {record});
        QCOMPARE(final.kind, AgentStepDecision::Kind::FinalAnswer);
        QCOMPARE(final.answer, finalReply.message);
        QCOMPARE(final.grounding, GroundingMode::Verified);
        QVERIFY(final.groundingDeclared);
        QCOMPARE(final.claims.size(), 1);
        QCOMPARE(final.claims.first().id, QStringLiteral("claim-1"));
        QVERIFY(final.claims.first().value);
        QCOMPARE(provider->requestOptions.last().toolResults.first().callId,
                 QStringLiteral("call-1"));
        QVERIFY(!provider->prompts.last().contains(QStringLiteral("risk=")));
        QVERIFY(provider->prompts.last().contains(QStringLiteral("only observed evidence")));
    }

    void fallsBackToHeuristicOnGarbage() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral("Sorry, I cannot produce JSON right now.");

        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        const auto decision = runtime.nextStep(QStringLiteral("run echo hi"), {});

        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(!runtime.lastDecisionUsedLlm());
    }

    void fallsBackToSummaryWhenProviderFailsWithHistory() {
        FakeChatProvider provider;
        provider.failNext = true;

        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        const auto decision =
            runtime.nextStep(QStringLiteral("goal"), QList<AgentStepRecord>{sampleRecord()});

        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(!runtime.lastDecisionUsedLlm());
    }

    void rejectsUnknownToolFromLlm() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral(
            "{\"action\":\"tool\",\"tool\":\"warp-drive\",\"args\":{},\"thought\":\"go\"}");

        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        const auto decision = runtime.nextStep(QStringLiteral("run echo hi"), {});

        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(!runtime.lastDecisionUsedLlm());
    }

    void planReturnsPlannedInvocationFromLlm() {
        FakeChatProvider provider;
        provider.scriptedReply =
            QStringLiteral("{\"action\":\"tool\",\"tool\":\"web-search\",\"thought\":\"lookup\","
                           "\"args\":{\"query\":\"latest ai news\"}}");

        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        const auto plan = runtime.plan(AgentRequest{QStringLiteral("latest ai news"), {}});

        QCOMPARE(plan.status, ToolInvocationPlanStatus::Planned);
        QCOMPARE(plan.invocations.size(), 1);
        QCOMPARE(plan.invocations.first().toolId, QStringLiteral("web-search"));
    }

    void planReportsEmptyRequest() {
        FakeChatProvider provider;
        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        const auto plan = runtime.plan(AgentRequest{QStringLiteral("   "), {}});

        QCOMPARE(plan.status, ToolInvocationPlanStatus::EmptyRequest);
        QCOMPARE(provider.prompts.size(), 0);
    }

    void providerlessRuntimeStillPlansHeuristically() {
        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), nullptr);
        QCOMPARE(runtime.status(), AgentStatus::Unavailable);

        const auto decision = runtime.nextStep(QStringLiteral("run echo hi"), {});
        QCOMPARE(decision.kind, AgentStepDecision::Kind::GiveUp);
        QVERIFY(!runtime.lastDecisionUsedLlm());
    }

    void promptContainsStepHistory() {
        FakeChatProvider provider;
        provider.scriptedReply = QStringLiteral("{\"action\":\"final\",\"answer\":\"done\"}");

        LlmAgentRuntime runtime(NullAgentRuntime::standardTools(), &provider);
        runtime.nextStep(QStringLiteral("goal"), QList<AgentStepRecord>{sampleRecord()});

        QVERIFY(provider.prompts.first().contains(QStringLiteral("hello-from-step")));
        QVERIFY(provider.prompts.first().contains(QStringLiteral("Succeeded")));
    }
};

QTEST_MAIN(LlmAgentRuntimeTest)
#include "test_llm_agent_runtime.moc"
