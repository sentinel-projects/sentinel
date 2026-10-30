// SPDX-License-Identifier: GPL-3.0-or-later
//
// Deterministic provider-contract fixtures for the native Claude (Anthropic
// Messages) and Gemini (generateContent) protocols. The real request builders in
// ModelService::nativeEndpointReply and LMStudioLocalInferenceClient run
// unmodified; only the Qt transport is replaced with an in-memory, scripted
// QNetworkAccessManager. No socket is opened, no DNS is resolved, TLS/proxy
// settings are untouched, and production endpoints stay authenticated HTTPS.
#ifndef Q_MOC_RUN
#include "../support/ScriptedProviderTransport.h"
#endif
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/network/NetworkPolicyService.h"
#include "sentinel/core/provider/SelectedEndpointChatProvider.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/LocalInference.h"
#include "sentinel/core/runtime/ProviderRequestRuntime.h"
#include "sentinel/core/security/CredentialStore.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace sentinel::core;
using sentinel::test::RecordedProviderRequest;
using sentinel::test::ScopedOnlineNetworkMode;
using sentinel::test::ScriptedHttpResponse;
using sentinel::test::ScriptedNetworkFactory;
using sentinel::test::ScriptedTransportState;

namespace {

const QString kClaudeKey = QStringLiteral("synthetic-claude-key");
const QString kGeminiKey = QStringLiteral("synthetic-gemini-key");
const QString kClaudeModel = QStringLiteral("claude-sonnet-4-20250514");
const QString kGeminiModel = QStringLiteral("gemini-2.5-pro");

// A deterministic descriptor with a non-trivial JSON Schema so the wire tool
// definition can be compared field-for-field against the authoritative contract.
ToolDescriptor syntheticReadTool() {
    ToolDescriptor tool;
    tool.id = QStringLiteral("read-file");
    tool.name = QStringLiteral("Read File");
    tool.description = QStringLiteral("Read the contents of a workspace file.");
    QJsonObject pathField;
    pathField.insert(QStringLiteral("type"), QStringLiteral("string"));
    pathField.insert(QStringLiteral("description"), QStringLiteral("Workspace-relative path."));
    QJsonObject properties;
    properties.insert(QStringLiteral("path"), pathField);
    tool.inputSchema.insert(QStringLiteral("type"), QStringLiteral("object"));
    tool.inputSchema.insert(QStringLiteral("properties"), properties);
    tool.inputSchema.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("path")});
    return tool;
}

ModelBinding claudeBinding(const QString& modelId = kClaudeModel) {
    ModelBinding binding;
    binding.providerId = QStringLiteral("claude");
    binding.modelId = modelId;
    binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
    return binding;
}

ModelBinding geminiBinding(const QString& modelId = kGeminiModel) {
    ModelBinding binding;
    binding.providerId = QStringLiteral("gemini");
    binding.modelId = modelId;
    binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
    return binding;
}

// Owns the settings, service and injected transport for one fixture request.
struct ProviderHarness {
    AppSettings settings;
    ModelService service;
    std::shared_ptr<ScriptedTransportState> state;

    ProviderHarness()
        : settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore()),
          service(&settings), state(std::make_shared<ScriptedTransportState>()) {
        settings.setNetworkMode(QStringLiteral("LocalOnly"));
        settings.setClaudeApiKey(kClaudeKey);
        settings.setGeminiApiKey(kGeminiKey);
        service.setProviderNetworkFactoryForTesting(
            std::make_shared<ScriptedNetworkFactory>(state));
    }

    void respond(ScriptedHttpResponse response) {
        state->responder = [response](const RecordedProviderRequest&) { return response; };
    }
    void respondBody(const QByteArray& body, int status = 200) {
        respond(ScriptedHttpResponse::body(body, status));
    }
    std::shared_ptr<IChatProvider> provider(const ModelBinding& binding) {
        return service.constructProvider(binding);
    }
    const RecordedProviderRequest& lastRequest() const {
        return state->requests.last();
    }
    QJsonObject lastBody() const {
        return QJsonDocument::fromJson(lastRequest().body).object();
    }
};

LMStudioConfig nativeStreamConfig(const QUrl& endpoint, const QString& apiKey,
                                  const std::shared_ptr<ScriptedTransportState>& state) {
    LMStudioConfig config;
    config.endpoint = endpoint;
    config.apiKey = apiKey;
    config.networkFactory = std::make_shared<ScriptedNetworkFactory>(state);
    return config;
}

} // namespace

class NativeProviderContractTest final : public QObject {
    Q_OBJECT
private slots:

    void nativeTransportParity_data() {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("scenario");
        for (const auto* protocol : {"compatible", "claude", "gemini"})
            for (const auto* scenario : {"success", "timeout", "tls", "malformed",
                                         "dispatch-expired", "dispatch-cancelled", "usage-success"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(protocol, scenario)))
                    << QString(protocol) << QString(scenario);
    }
    void nativeTransportParity() {
        QFETCH(QString, protocol);
        QFETCH(QString, scenario);
        ScopedOnlineNetworkMode online;
        auto state = std::make_shared<ScriptedTransportState>();
        state->responder = [=](const auto&) {
            QByteArray body =
                protocol == "claude"
                    ? R"({"content":[{"type":"text","text":"hello"}],"stop_reason":"end_turn"})"
                : protocol == "gemini"
                    ? R"({"candidates":[{"content":{"role":"model","parts":[{"text":"hello"}]},"finishReason":"STOP"}]})"
                    : R"({"choices":[{"message":{"content":"hello"},"finish_reason":"stop"}]})";

            if (scenario == "usage-success") {
                auto object = QJsonDocument::fromJson(body).object();
                object.insert(protocol == "gemini" ? "usageMetadata" : "usage",
                              protocol == "claude"
                                  ? QJsonObject{{"input_tokens", 3}, {"output_tokens", 2}}
                              : protocol == "gemini" ? QJsonObject{{"promptTokenCount", 3},
                                                                   {"candidatesTokenCount", 2},
                                                                   {"totalTokenCount", 5}}
                                                     : QJsonObject{{"prompt_tokens", 3},
                                                                   {"completion_tokens", 2},
                                                                   {"total_tokens", 5}});
                body = QJsonDocument(object).toJson();
            }
            auto response = ScriptedHttpResponse::body(
                scenario == "malformed" ? QByteArray("PRIVATE_RESPONSE") : body);
            response.emitMetadataSignal = true;
            if (scenario == "timeout") {
                response.frames.clear();
                response.finish = false;
            }
            if (scenario == "tls") {
                response.status = 0;
                response.error = QNetworkReply::SslHandshakeFailedError;
                response.errorString = "PRIVATE_TLS";
            }
            return response;
        };
        qint64 now = 0;
        auto token = std::make_shared<std::atomic_bool>(false);
        if (scenario == "dispatch-expired")
            state->onCreateManager = [&] { now = 30; };
        if (scenario == "dispatch-cancelled")
            state->onCreateManager = [token] { token->store(true); };
        LMStudioConfig config;
        config.endpoint = protocol == "claude"   ? QUrl("https://api.anthropic.com")
                          : protocol == "gemini" ? QUrl("https://generativelanguage.googleapis.com")
                                                 : QUrl("https://fixture.invalid/v1");
        config.apiKey = "PRIVATE_KEY";
        config.networkFactory = std::make_shared<ScriptedNetworkFactory>(state);
        ModelBinding binding{protocol == "claude"   ? "claude"
                             : protocol == "gemini" ? "gemini"
                                                    : "cloud-api",
                             "fixture"};
        binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
        auto provider = makeSelectedEndpointChatProvider(binding, config, 0);
        ChatRequestOptions options;
        options.nativeToolCalling = true;
        options.tools = {syntheticReadTool()};
        options.maxOutputTokens = 128;
        options.deadlineMs = 30;
        options.cancellationToken = token;
        options.operationDeadline = std::make_shared<OperationDeadline>(30, [&] { return now; });
        const auto reply = provider->sendRequest("PRIVATE_PROMPT", options);
        QCOMPARE(reply.success, scenario == "success" || scenario == "usage-success");
        const bool undispatched = scenario.startsWith("dispatch-");
        QCOMPARE(state->requests.size(), undispatched ? 0 : 1);
        const auto frames = reply.diagnostics.value("transport_attempts").toArray();
        QCOMPARE(frames.size(), 1);
        const auto frame = frames.first().toObject();
        QVERIFY(!QUuid(frame.value("attempt_id").toString()).isNull());
        QCOMPARE(frame.value("http_status").isNull(), undispatched || scenario == "tls");
        QCOMPARE(frame.value("response_completed").toBool(),
                 scenario == "success" || scenario == "usage-success" || scenario == "malformed");
        if (scenario == "timeout" || scenario == "dispatch-expired")
            QCOMPARE(reply.category, ChatProviderErrorCategory::Timeout);
        if (scenario == "dispatch-cancelled")
            QCOMPARE(reply.category, ChatProviderErrorCategory::Cancelled);
        QVERIFY(reply.toolCalls.isEmpty());
        if (scenario == "usage-success") {
            QCOMPARE(reply.diagnostics.value("prompt_tokens").toInt(-1), 3);
            QCOMPARE(reply.diagnostics.value("completion_tokens").toInt(-1), 2);
            QCOMPARE(reply.diagnostics.contains("total_tokens"), protocol != "claude");
        } else
            QVERIFY(!reply.diagnostics.contains("total_tokens"));
        QVERIFY(!QJsonDocument(reply.diagnostics).toJson().contains("PRIVATE_"));
    }
    void initTestCase();

    // Claude: native Anthropic Messages protocol.
    void claudeMessagesRequestConstruction();
    void claudeToolUseSchemaAndIdentity();
    void claudeToolNameNormalization();
    void claudeToolContinuationFormatsResults();
    void claudeInvalidToolUseRejected_data();
    void claudeInvalidToolUseRejected();
    void claudeContinuationRejections_data();
    void claudeContinuationRejections();
    void claudeStructuredOutputContract_data();
    void claudeStructuredOutputContract();
    void claudeCapabilityRejectionBeforeTransport();
    void claudeCancellationBeforeDispatch();
    void claudeDeadlineTimeout();
    void claudeStreamingTextAndTerminal();
    void claudeStreamingUnterminatedIsRejected();

    // Gemini: native generateContent protocol.
    void geminiGenerateContentConstruction();
    void geminiFunctionDeclarationSchema();
    void geminiFunctionCallIdentity();
    void geminiSignatureRequirement();
    void geminiFunctionContinuationFormatsResults();
    void geminiBlockedOrSafetyRejected_data();
    void geminiBlockedOrSafetyRejected();
    void geminiMalformedResponseRejected_data();
    void geminiMalformedResponseRejected();
    void geminiStructuredOutputContract();
    void geminiCapabilityRejectionBeforeTransport();
    void geminiCancellationBeforeDispatch();
    void geminiDeadlineTimeout();
    void geminiStreamingFinishReasonTerminal();

    // Non-native cloud chat branches (`infer`) still route through the injected
    // transport with their provider-specific authentication placement.
    void cloudChatRequestConstruction_data();
    void cloudChatRequestConstruction();

    // Shared transport safety and classification, exercised for both protocols.
    void nativeHttpFailureClassification_data();
    void nativeHttpFailureClassification();
    void nativeSafeErrorProjection_data();
    void nativeSafeErrorProjection();
    void injectedTransportCannotBypassNetworkPolicy();
    void defaultTransportRemainsProductionHttps();
    // P2-M03: byte-equivalence of the native request body against pre-refactor
    // golden fixtures captured from ModelService::nativeEndpointReply.
    void nativeRequestBodyByteEquivalence();
    // P2-M04: production-path equivalence for the extracted native response
    // normalization (text + tool-call parts, continuation fidelity).
    void nativeResponseNormalizationEquivalence();
};

void NativeProviderContractTest::initTestCase() {
    // Every fixture is loopback-free: the injected transport records the intended
    // production URL while NetworkPolicyService keeps its restrictive default.
    NetworkPolicyService::instance().setMode(NetworkMode::LocalOnly);
}

// ---------------------------------------------------------------------------
// Claude
// ---------------------------------------------------------------------------

void NativeProviderContractTest::claudeMessagesRequestConstruction() {
    ProviderHarness harness;
    harness.respondBody(
        R"({"id":"msg_fixture","type":"message","role":"assistant","content":[{"type":"text","text":"synthetic answer"}],"stop_reason":"end_turn"})");
    auto provider = harness.provider(claudeBinding());
    QVERIFY(provider);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic claude prompt", options);
    QVERIFY(reply.success);
    QCOMPARE(reply.message, QStringLiteral("synthetic answer"));
    QCOMPARE(reply.lifecycle, ChatRequestLifecycle::Completed);
    QCOMPARE(reply.category, ChatProviderErrorCategory::None);
    QCOMPARE(reply.httpStatus, 200);
    QCOMPARE(harness.state->requests.size(), 1);

    const auto& request = harness.lastRequest();
    QCOMPARE(request.method, QByteArrayLiteral("POST"));
    QCOMPARE(request.url.scheme(), QStringLiteral("https"));
    QCOMPARE(request.url.host(), QStringLiteral("api.anthropic.com"));
    QCOMPARE(request.url.path(), QStringLiteral("/v1/messages"));
    QCOMPARE(request.headers.value("content-type"), QByteArrayLiteral("application/json"));
    QCOMPARE(request.headers.value("x-api-key"), kClaudeKey.toUtf8());
    QCOMPARE(request.headers.value("anthropic-version"), QByteArrayLiteral("2023-06-01"));

    const auto body = harness.lastBody();
    QCOMPARE(body.value("model").toString(), kClaudeModel);
    QCOMPARE(body.value("max_tokens").toInt(), 2048);
    const auto messages = body.value("messages").toArray();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().toObject().value("role").toString(), QStringLiteral("user"));
    QCOMPARE(messages.first().toObject().value("content").toString(),
             QStringLiteral("synthetic claude prompt"));
    QCOMPARE(body.value("tools").toArray().size(), 1);
    QVERIFY(!request.body.contains(kClaudeKey.toUtf8()));
}

void NativeProviderContractTest::claudeToolUseSchemaAndIdentity() {
    const auto tool = syntheticReadTool();
    ProviderHarness harness;
    harness.respondBody(
        R"({"content":[{"type":"tool_use","id":"toolu_fixture_1","name":"read-file","input":{"path":"fixture.txt"}}],"stop_reason":"tool_use"})");
    auto provider = harness.provider(claudeBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {tool};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("please read fixture.txt", options);
    QVERIFY2(reply.success, qPrintable(reply.errorMessage));
    QCOMPARE(reply.toolCalls.size(), 1);
    const auto call = reply.toolCalls.first();
    QCOMPARE(call.callId, QStringLiteral("toolu_fixture_1"));
    QCOMPARE(call.toolId, QStringLiteral("read-file"));
    const QJsonObject expectedArgs{{QStringLiteral("path"), QStringLiteral("fixture.txt")}};
    QCOMPARE(call.arguments, expectedArgs);
    QVERIFY(!call.providerContinuation.isEmpty());
    const auto continuation = QJsonDocument::fromJson(call.providerContinuation.toUtf8()).array();
    QVERIFY(continuation.size() >= 2);
    QCOMPARE(continuation.last().toObject().value("role").toString(), QStringLiteral("assistant"));

    const auto body = harness.lastBody();
    const auto tools = body.value("tools").toArray();
    QCOMPARE(tools.size(), 1);
    const auto wireTool = tools.first().toObject();
    QCOMPARE(wireTool.value("name").toString(), QStringLiteral("read-file"));
    QCOMPARE(wireTool.value("description").toString(), tool.description);
    QCOMPARE(wireTool.value("input_schema").toObject(), tool.inputSchema);
    // The Anthropic Messages tool shape is flat; it must not reuse the OpenAI
    // nested {type,function} envelope.
    QVERIFY(!wireTool.contains(QStringLiteral("type")));
    QVERIFY(!wireTool.contains(QStringLiteral("function")));
}

void NativeProviderContractTest::claudeToolNameNormalization() {
    ToolDescriptor tool = syntheticReadTool();
    tool.id = QStringLiteral("read file!/unsafe");
    const auto expected = QStringLiteral("tool_%1").arg(QString::fromLatin1(
        QCryptographicHash::hash(tool.id.toUtf8(), QCryptographicHash::Sha256).toHex().left(48)));
    ProviderHarness harness;
    const QByteArray toolUse =
        QJsonDocument(
            QJsonObject{{QStringLiteral("content"),
                         QJsonArray{QJsonObject{
                             {QStringLiteral("type"), QStringLiteral("tool_use")},
                             {QStringLiteral("id"), QStringLiteral("toolu_hashed")},
                             {QStringLiteral("name"), expected},
                             {QStringLiteral("input"),
                              QJsonObject{{QStringLiteral("path"), QStringLiteral("x")}}}}}}})
            .toJson(QJsonDocument::Compact);
    harness.respondBody(toolUse);
    auto provider = harness.provider(claudeBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {tool};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic prompt", options);
    QVERIFY2(reply.success, qPrintable(reply.errorMessage));
    QCOMPARE(reply.toolCalls.size(), 1);
    // The normalized wire name maps back to the original registered tool id.
    QCOMPARE(reply.toolCalls.first().toolId, tool.id);
    const auto wireName =
        harness.lastBody().value("tools").toArray().first().toObject().value("name").toString();
    QCOMPARE(wireName, expected);
}

void NativeProviderContractTest::claudeToolContinuationFormatsResults() {
    const auto tool = syntheticReadTool();
    ProviderHarness harness;
    auto provider = harness.provider(claudeBinding());
    harness.respondBody(
        R"({"content":[{"type":"tool_use","id":"toolu_fixture_1","name":"read-file","input":{"path":"fixture.txt"}}]})");
    ChatRequestOptions firstOptions;
    firstOptions.deadlineMs = 500;
    firstOptions.nativeToolCalling = true;
    firstOptions.tools = {tool};
    ScopedOnlineNetworkMode online;
    const auto first = provider->sendRequest("read fixture.txt", firstOptions);
    QVERIFY2(first.success, qPrintable(first.errorMessage));
    QCOMPARE(first.toolCalls.size(), 1);
    const auto call = first.toolCalls.first();

    harness.respondBody(
        R"({"content":[{"type":"text","text":"summary ready"}],"stop_reason":"end_turn"})");
    ChatRequestOptions secondOptions;
    secondOptions.deadlineMs = 500;
    secondOptions.nativeToolCalling = true;
    secondOptions.tools = {tool};
    secondOptions.priorToolCalls = {call};
    secondOptions.toolResults = {{call.callId, QStringLiteral("fixture file contents")}};
    const auto second = provider->sendRequest("summarize the file", secondOptions);
    QVERIFY2(second.success, qPrintable(second.errorMessage));
    QCOMPARE(second.message, QStringLiteral("summary ready"));
    QCOMPARE(harness.state->requests.size(), 2);

    const auto messages = harness.lastBody().value("messages").toArray();
    QCOMPARE(messages.size(), 3);
    QCOMPARE(messages.at(0).toObject().value("role").toString(), QStringLiteral("user"));
    QCOMPARE(messages.at(1).toObject().value("role").toString(), QStringLiteral("assistant"));
    QCOMPARE(messages.at(1)
                 .toObject()
                 .value("content")
                 .toArray()
                 .first()
                 .toObject()
                 .value("type")
                 .toString(),
             QStringLiteral("tool_use"));
    const auto resultContent = messages.at(2).toObject().value("content").toArray();
    QCOMPARE(resultContent.size(), 1);
    QCOMPARE(resultContent.first().toObject().value("type").toString(),
             QStringLiteral("tool_result"));
    QCOMPARE(resultContent.first().toObject().value("tool_use_id").toString(), call.callId);
    QCOMPARE(resultContent.first().toObject().value("content").toString(),
             QStringLiteral("fixture file contents"));
}

void NativeProviderContractTest::claudeInvalidToolUseRejected_data() {
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<bool>("native");
    QTest::addColumn<bool>("structured");
    QTest::addColumn<bool>("ok");
    QTest::newRow("valid")
        << QByteArray(
               R"({"content":[{"type":"tool_use","id":"toolu_1","name":"read-file","input":{"path":"a"}}]})")
        << true << false << true;
    QTest::newRow("mixed-text-and-tool")
        << QByteArray(
               R"({"content":[{"type":"text","text":"thinking"},{"type":"tool_use","id":"toolu_1","name":"read-file","input":{"path":"a"}}]})")
        << true << false << true;
    QTest::newRow("unknown-tool")
        << QByteArray(
               R"({"content":[{"type":"tool_use","id":"toolu_1","name":"not-registered","input":{}}]})")
        << true << false << false;
    QTest::newRow("empty-call-id")
        << QByteArray(
               R"({"content":[{"type":"tool_use","id":"","name":"read-file","input":{"path":"a"}}]})")
        << true << false << false;
    QTest::newRow("non-object-input")
        << QByteArray(
               R"({"content":[{"type":"tool_use","id":"toolu_1","name":"read-file","input":"oops"}]})")
        << true << false << false;
    QTest::newRow("missing-content")
        << QByteArray(R"({"stop_reason":"end_turn"})") << true << false << false;
    QTest::newRow("empty-content") << QByteArray(R"({"content":[]})") << true << false << false;
    // A tool_use block returned when tool calling was not requested is rejected by
    // the native builder, independent of the transport.
    QTest::newRow("tool-use-without-native")
        << QByteArray(
               R"({"content":[{"type":"tool_use","id":"toolu_1","name":"read-file","input":{"path":"a"}}]})")
        << false << true << false;
    QTest::newRow("text-only") << QByteArray(R"({"content":[{"type":"text","text":"hi"}]})")
                               << false << false << true;
}

void NativeProviderContractTest::claudeInvalidToolUseRejected() {
    QFETCH(QByteArray, body);
    QFETCH(bool, native);
    QFETCH(bool, structured);
    QFETCH(bool, ok);
    ProviderHarness harness;
    harness.respondBody(body);
    auto binding = claudeBinding();
    if (structured)
        binding.capabilities.structuredOutput = CapabilitySupport::Supported;
    auto provider = harness.provider(binding);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = native;
    options.structuredOutput = structured;
    if (native)
        options.tools = {syntheticReadTool()};
    if (structured) {
        options.structuredSchemaName = QStringLiteral("answer");
        options.structuredSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};
    }
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic prompt", options);
    QCOMPARE(reply.success, ok);
    QCOMPARE(harness.state->requests.size(), 1);
    if (!ok) {
        QVERIFY(reply.toolCalls.isEmpty());
        QCOMPARE(reply.category, ChatProviderErrorCategory::MalformedResponse);
        QCOMPARE(reply.error, ChatProviderReply::Error::InvalidResponse);
    }
}

void NativeProviderContractTest::claudeContinuationRejections_data() {
    // 0 size mismatch, 1 unregistered tool, 2 invalid JSON history, 3 non-array
    // history, 4 empty-array history, 5 differing histories, 6 callId mismatch,
    // 7 valid control.
    QTest::addColumn<int>("mode");
    QTest::addColumn<QByteArray>("resultBody");
    for (int mode = 0; mode <= 7; ++mode)
        QTest::newRow(qPrintable(QStringLiteral("mode%1").arg(mode)))
            << mode << QByteArray(R"({"content":[{"type":"text","text":"ok"}]})");
}

void NativeProviderContractTest::claudeContinuationRejections() {
    QFETCH(int, mode);
    const auto tool = syntheticReadTool();
    ProviderHarness harness;
    QFETCH(QByteArray, resultBody);
    harness.respondBody(resultBody);
    auto provider = harness.provider(claudeBinding());

    const QByteArray validHistory = R"([{"role":"user","content":"hi"}])";
    ChatProviderReply::ToolCall call;
    call.toolId = QStringLiteral("read-file");
    call.arguments = QJsonObject{{QStringLiteral("path"), QStringLiteral("a")}};
    call.providerContinuation = QString::fromUtf8(validHistory);
    call.callId = QStringLiteral("toolu_x");

    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {tool};
    options.priorToolCalls = {call};
    options.toolResults = {{call.callId, QStringLiteral("contents")}};

    switch (mode) {
    case 0:
        options.toolResults.clear();
        break;
    case 1:
        options.priorToolCalls.first().toolId = QStringLiteral("unregistered");
        break;
    case 2:
        options.priorToolCalls.first().providerContinuation = QStringLiteral("{broken");
        break;
    case 3:
        options.priorToolCalls.first().providerContinuation = QStringLiteral("{\"role\":\"user\"}");
        break;
    case 4:
        options.priorToolCalls.first().providerContinuation = QStringLiteral("[]");
        break;
    case 5: {
        auto second = call;
        second.callId = QStringLiteral("toolu_y");
        second.providerContinuation = QStringLiteral(R"([{"role":"user","content":"other"}])");
        options.priorToolCalls = {call, second};
        options.toolResults = {{call.callId, QStringLiteral("a")},
                               {second.callId, QStringLiteral("b")}};
        break;
    }
    case 6:
        options.toolResults = {{QStringLiteral("toolu_other"), QStringLiteral("contents")}};
        break;
    default:
        break;
    }

    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("continue", options);
    if (mode == 7) {
        QVERIFY2(reply.success, qPrintable(reply.errorMessage));
        QCOMPARE(harness.state->requests.size(), 1);
    } else {
        QVERIFY2(!reply.success, "expected a malformed continuation rejection");
        QCOMPARE(reply.category, ChatProviderErrorCategory::MalformedResponse);
        QCOMPARE(reply.error, ChatProviderReply::Error::InvalidResponse);
        // Rejections happen during request construction; nothing reaches transport.
        QCOMPARE(harness.state->requests.size(), 0);
    }
}

void NativeProviderContractTest::claudeStructuredOutputContract_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("ok");
    QTest::newRow("object") << QStringLiteral(R"({"answer":42})") << true;
    QTest::newRow("not-json") << QStringLiteral("plain text") << false;
    QTest::newRow("array") << QStringLiteral("[]") << false;
}

void NativeProviderContractTest::claudeStructuredOutputContract() {
    QFETCH(QString, text);
    QFETCH(bool, ok);
    ProviderHarness harness;
    const QJsonObject message{
        {QStringLiteral("content"),
         QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                {QStringLiteral("text"), text}}}}};
    harness.respondBody(QJsonDocument(message).toJson(QJsonDocument::Compact));
    auto binding = claudeBinding();
    binding.capabilities.structuredOutput = CapabilitySupport::Supported;
    auto provider = harness.provider(binding);

    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.structuredOutput = true;
    options.structuredSchemaName = QStringLiteral("answer");
    options.structuredSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("answer"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}}}};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic structured request", options);
    QCOMPARE(reply.success, ok);
    QCOMPARE(harness.state->requests.size(), 1);
    if (ok) {
        QVERIFY(reply.structuredResult.has_value());
        QCOMPARE(reply.structuredResult->value("answer").toInt(), 42);
    } else {
        QCOMPARE(reply.category, ChatProviderErrorCategory::MalformedResponse);
    }
    // The schema is carried through the Anthropic output_config envelope unchanged.
    const auto format =
        harness.lastBody().value("output_config").toObject().value("format").toObject();
    QCOMPARE(format.value("type").toString(), QStringLiteral("json_schema"));
    QCOMPARE(format.value("schema").toObject(), options.structuredSchema);
}

void NativeProviderContractTest::claudeCapabilityRejectionBeforeTransport() {
    ProviderHarness harness;
    auto provider = harness.provider(claudeBinding());
    ChatRequestOptions structuredOptions;
    structuredOptions.deadlineMs = 500;
    structuredOptions.structuredOutput = true;
    structuredOptions.structuredSchema =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};
    ScopedOnlineNetworkMode online;
    // Claude's advertised native-tool capability is Supported, but structured
    // output defaults to Unknown; a fixture must not silently upgrade it.
    const auto structuredReply = provider->sendRequest("synthetic", structuredOptions);
    QVERIFY(!structuredReply.success);
    QCOMPARE(structuredReply.error, ChatProviderReply::Error::CapabilityRejected);
    QCOMPARE(structuredReply.category, ChatProviderErrorCategory::CapabilityUnsupported);
    QCOMPARE(harness.state->requests.size(), 0);
}

void NativeProviderContractTest::claudeCancellationBeforeDispatch() {
    ProviderHarness harness;
    auto provider = harness.provider(claudeBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    auto token = std::make_shared<std::atomic_bool>(true);
    options.cancellationToken = token;
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic", options);
    QVERIFY(!reply.success);
    QCOMPARE(reply.category, ChatProviderErrorCategory::Cancelled);
    QCOMPARE(reply.lifecycle, ChatRequestLifecycle::Cancelled);
    QCOMPARE(harness.state->requests.size(), 0);
}

void NativeProviderContractTest::claudeDeadlineTimeout() {
    ProviderHarness harness;
    harness.respond(ScriptedHttpResponse::streaming({}, false));
    auto provider = harness.provider(claudeBinding());
    ChatRequestOptions options;
    options.deadlineMs = 150;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic", options);
    QVERIFY(!reply.success);
    QCOMPARE(reply.category, ChatProviderErrorCategory::Timeout);
    QCOMPARE(reply.lifecycle, ChatRequestLifecycle::TimedOut);
    QCOMPARE(reply.attempts, 1);
    QCOMPARE(harness.state->requests.size(), 1);
}

void NativeProviderContractTest::claudeStreamingTextAndTerminal() {
    ScopedOnlineNetworkMode online;
    auto state = std::make_shared<ScriptedTransportState>();
    state->responder = [](const RecordedProviderRequest&) {
        return ScriptedHttpResponse::streaming({QByteArray(
            "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\"claude \"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\"stream\"}}\n\n"
            "data: {\"type\":\"message_stop\"}\n\n")});
    };
    auto config =
        nativeStreamConfig(QUrl(QStringLiteral("https://api.anthropic.com")), kClaudeKey, state);
    LocalInferenceRequest request;
    request.prompt = QStringLiteral("synthetic claude stream");
    request.options.model = kClaudeModel;
    request.options.timeoutMs = 1000;
    QString deltas;
    const auto result = LMStudioLocalInferenceStreamClient(config, 1000)
                            .startStream(request, [&](const LocalInferenceStreamChunk& chunk) {
                                deltas += chunk.text;
                            });
    QCOMPARE(result.status, LocalInferenceStreamStatus::Completed);
    QCOMPARE(deltas, QStringLiteral("claude stream"));
    QCOMPARE(result.accumulatedText, QStringLiteral("claude stream"));
    QCOMPARE(state->requests.size(), 1);
    QCOMPARE(state->requests.first().url.host(), QStringLiteral("api.anthropic.com"));
    QCOMPARE(state->requests.first().url.path(), QStringLiteral("/v1/messages"));
    QCOMPARE(state->requests.first().headers.value("x-api-key"), kClaudeKey.toUtf8());
    const auto body = QJsonDocument::fromJson(state->requests.first().body).object();
    QVERIFY(body.value("stream").toBool());
    QCOMPARE(body.value("model").toString(), kClaudeModel);
}

void NativeProviderContractTest::claudeStreamingUnterminatedIsRejected() {
    ScopedOnlineNetworkMode online;
    auto state = std::make_shared<ScriptedTransportState>();
    state->responder = [](const RecordedProviderRequest&) {
        // Text without a message_stop terminal frame is not an accepted answer.
        return ScriptedHttpResponse::streaming({QByteArray(
            "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\"half\"}}\n\n")});
    };
    auto config =
        nativeStreamConfig(QUrl(QStringLiteral("https://api.anthropic.com")), kClaudeKey, state);
    LocalInferenceRequest request;
    request.prompt = QStringLiteral("synthetic claude stream");
    request.options.model = kClaudeModel;
    request.options.timeoutMs = 1000;
    QString deltas;
    const auto result = LMStudioLocalInferenceStreamClient(config, 1000)
                            .startStream(request, [&](const LocalInferenceStreamChunk& chunk) {
                                deltas += chunk.text;
                            });
    QVERIFY(result.status != LocalInferenceStreamStatus::Completed);
    QCOMPARE(result.providerErrorCategory, int(ChatProviderErrorCategory::MalformedResponse));
    QCOMPARE(deltas, QStringLiteral("half"));
}

// ---------------------------------------------------------------------------
// Gemini
// ---------------------------------------------------------------------------

void NativeProviderContractTest::geminiGenerateContentConstruction() {
    ProviderHarness harness;
    harness.respondBody(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"synthetic gemini answer"}]},"finishReason":"STOP"}]})");
    auto provider = harness.provider(geminiBinding());
    QVERIFY(provider);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic gemini prompt", options);
    QVERIFY2(reply.success, qPrintable(reply.errorMessage));
    QCOMPARE(reply.message, QStringLiteral("synthetic gemini answer"));
    QCOMPARE(reply.lifecycle, ChatRequestLifecycle::Completed);
    QCOMPARE(harness.state->requests.size(), 1);

    const auto& request = harness.lastRequest();
    QCOMPARE(request.method, QByteArrayLiteral("POST"));
    QCOMPARE(request.url.scheme(), QStringLiteral("https"));
    QCOMPARE(request.url.host(), QStringLiteral("generativelanguage.googleapis.com"));
    QCOMPARE(request.url.path(), QStringLiteral("/v1beta/models/gemini-2.5-pro:generateContent"));
    // The native generateContent path authenticates with the header, not a query key.
    QCOMPARE(request.headers.value("x-goog-api-key"), kGeminiKey.toUtf8());
    QVERIFY(!request.url.query().contains(kGeminiKey));
    const auto body = harness.lastBody();
    // The model is carried in the URL for generateContent, not the body.
    QVERIFY(!body.contains(QStringLiteral("model")));
    const auto contents = body.value("contents").toArray();
    QCOMPARE(contents.size(), 1);
    QCOMPARE(contents.first().toObject().value("role").toString(), QStringLiteral("user"));
    QCOMPARE(contents.first()
                 .toObject()
                 .value("parts")
                 .toArray()
                 .first()
                 .toObject()
                 .value("text")
                 .toString(),
             QStringLiteral("synthetic gemini prompt"));
    QVERIFY(!body.value("tools").toArray().isEmpty());
    QVERIFY(!request.body.contains(kGeminiKey.toUtf8()));
}

void NativeProviderContractTest::geminiFunctionDeclarationSchema() {
    const auto tool = syntheticReadTool();
    ProviderHarness harness;
    harness.respondBody(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"ok"}]},"finishReason":"STOP"}]})");
    auto provider = harness.provider(geminiBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {tool};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic", options);
    QVERIFY2(reply.success, qPrintable(reply.errorMessage));
    const auto declarations = harness.lastBody()
                                  .value("tools")
                                  .toArray()
                                  .first()
                                  .toObject()
                                  .value("functionDeclarations")
                                  .toArray();
    QCOMPARE(declarations.size(), 1);
    const auto declaration = declarations.first().toObject();
    // The advertised declaration must equal the authoritative public converter.
    QCOMPARE(declaration, geminiFunctionDeclaration(tool));
    QCOMPARE(declaration.value("parametersJsonSchema").toObject(), tool.inputSchema);
    QVERIFY(!declaration.contains(QStringLiteral("parameters")));
}

void NativeProviderContractTest::geminiFunctionCallIdentity() {
    const auto tool = syntheticReadTool();
    ProviderHarness harness;
    harness.respondBody(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"read-file","args":{"path":"fixture.txt"}}}]},"finishReason":"STOP"}]})");
    auto provider = harness.provider(geminiBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {tool};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic", options);
    QVERIFY2(reply.success, qPrintable(reply.errorMessage));
    QCOMPARE(reply.toolCalls.size(), 1);
    const auto call = reply.toolCalls.first();
    QCOMPARE(call.toolId, QStringLiteral("read-file"));
    const QJsonObject expectedArgs{{QStringLiteral("path"), QStringLiteral("fixture.txt")}};
    QCOMPARE(call.arguments, expectedArgs);
    // Gemini omits ids, so Sentinel synthesizes a deterministic-shaped, unique id.
    QVERIFY(call.callId.startsWith(QStringLiteral("sentinel-gemini-")));
    QVERIFY(!call.providerContinuation.isEmpty());
}

void NativeProviderContractTest::geminiSignatureRequirement() {
    const auto tool = syntheticReadTool();
    // gemini-3 responses require a thought signature on the first function call.
    for (bool signedResponse : {false, true}) {
        ProviderHarness harness;
        const QJsonObject call{
            {QStringLiteral("name"), QStringLiteral("read-file")},
            {QStringLiteral("args"), QJsonObject{{QStringLiteral("path"), QStringLiteral("a")}}}};
        QJsonObject part{{QStringLiteral("functionCall"), call}};
        if (signedResponse)
            part.insert(QStringLiteral("thoughtSignature"), QStringLiteral("sig_fixture"));
        const QJsonObject body{
            {QStringLiteral("candidates"),
             QJsonArray{QJsonObject{{QStringLiteral("content"),
                                     QJsonObject{{QStringLiteral("role"), QStringLiteral("model")},
                                                 {QStringLiteral("parts"), QJsonArray{part}}}},
                                    {QStringLiteral("finishReason"), QStringLiteral("STOP")}}}}};
        harness.respondBody(QJsonDocument(body).toJson(QJsonDocument::Compact));
        auto provider = harness.provider(geminiBinding(QStringLiteral("gemini-3-pro-preview")));
        ChatRequestOptions options;
        options.deadlineMs = 500;
        options.nativeToolCalling = true;
        options.tools = {tool};
        ScopedOnlineNetworkMode online;
        const auto reply = provider->sendRequest("synthetic", options);
        if (signedResponse) {
            QVERIFY2(reply.success, qPrintable(reply.errorMessage));
            QCOMPARE(reply.toolCalls.size(), 1);
        } else {
            QVERIFY(!reply.success);
            QCOMPARE(reply.category, ChatProviderErrorCategory::MalformedResponse);
            QCOMPARE(reply.error, ChatProviderReply::Error::InvalidResponse);
        }
        QCOMPARE(harness.state->requests.size(), 1);
    }
}

void NativeProviderContractTest::geminiFunctionContinuationFormatsResults() {
    const auto tool = syntheticReadTool();
    ProviderHarness harness;
    auto provider = harness.provider(geminiBinding());
    harness.respondBody(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"read-file","args":{"path":"a"}}}]},"finishReason":"STOP"}]})");
    ChatRequestOptions firstOptions;
    firstOptions.deadlineMs = 500;
    firstOptions.nativeToolCalling = true;
    firstOptions.tools = {tool};
    ScopedOnlineNetworkMode online;
    const auto first = provider->sendRequest("read a", firstOptions);
    QVERIFY2(first.success, qPrintable(first.errorMessage));
    const auto call = first.toolCalls.first();
    QVERIFY(call.callId.startsWith(QStringLiteral("sentinel-gemini-")));

    harness.respondBody(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"done"}]},"finishReason":"STOP"}]})");
    ChatRequestOptions secondOptions;
    secondOptions.deadlineMs = 500;
    secondOptions.nativeToolCalling = true;
    secondOptions.tools = {tool};
    secondOptions.priorToolCalls = {call};
    secondOptions.toolResults = {{call.callId, QStringLiteral("contents")}};
    const auto second = provider->sendRequest("summarize", secondOptions);
    QVERIFY2(second.success, qPrintable(second.errorMessage));
    QCOMPARE(second.message, QStringLiteral("done"));
    QCOMPARE(harness.state->requests.size(), 2);

    auto contents = harness.lastBody().value("contents").toArray();
    QCOMPARE(contents.size(), 3);
    const auto functionResponse = contents.last()
                                      .toObject()
                                      .value("parts")
                                      .toArray()
                                      .first()
                                      .toObject()
                                      .value("functionResponse")
                                      .toObject();
    QCOMPARE(functionResponse.value("name").toString(), QStringLiteral("read-file"));
    QCOMPARE(functionResponse.value("response").toObject().value("output").toString(),
             QStringLiteral("contents"));
    // A synthesized id is internal bookkeeping; it is not echoed back to Gemini.
    QVERIFY(!functionResponse.contains(QStringLiteral("id")));

    // A provider-supplied id is preserved and forwarded verbatim.
    auto explicitCall = call;
    explicitCall.callId = QStringLiteral("call_explicit");
    explicitCall.providerContinuation =
        QStringLiteral(R"([{"role":"user","parts":[{"text":"x"}]}])");
    ChatRequestOptions thirdOptions;
    thirdOptions.deadlineMs = 500;
    thirdOptions.nativeToolCalling = true;
    thirdOptions.tools = {tool};
    thirdOptions.priorToolCalls = {explicitCall};
    thirdOptions.toolResults = {{explicitCall.callId, QStringLiteral("contents")}};
    const auto third = provider->sendRequest("continue", thirdOptions);
    QVERIFY2(third.success, qPrintable(third.errorMessage));
    const auto explicitResponse = harness.lastBody()
                                      .value("contents")
                                      .toArray()
                                      .last()
                                      .toObject()
                                      .value("parts")
                                      .toArray()
                                      .first()
                                      .toObject()
                                      .value("functionResponse")
                                      .toObject();
    QCOMPARE(explicitResponse.value("id").toString(), QStringLiteral("call_explicit"));
}

void NativeProviderContractTest::geminiBlockedOrSafetyRejected_data() {
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("blocked") << QByteArray(R"({"promptFeedback":{"blockReason":"SAFETY"}})");
    QTest::newRow("safety") << QByteArray(
        R"({"candidates":[{"content":{"parts":[{"text":"x"}]},"finishReason":"SAFETY"}]})");
    QTest::newRow("prohibited") << QByteArray(
        R"({"candidates":[{"content":{"parts":[{"text":"x"}]},"finishReason":"PROHIBITED_CONTENT"}]})");
}

void NativeProviderContractTest::geminiBlockedOrSafetyRejected() {
    QFETCH(QByteArray, body);
    ProviderHarness harness;
    harness.respondBody(body);
    auto provider = harness.provider(geminiBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic", options);
    QVERIFY(!reply.success);
    QCOMPARE(reply.category, ChatProviderErrorCategory::RequestRejected);
    QCOMPARE(harness.state->requests.size(), 1);
}

void NativeProviderContractTest::geminiMalformedResponseRejected_data() {
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<bool>("ok");
    QTest::newRow("empty") << QByteArray(R"({})") << false;
    QTest::newRow("no-parts") << QByteArray(R"({"candidates":[{"finishReason":"STOP"}]})") << false;
    QTest::newRow("args-not-object")
        << QByteArray(
               R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"read-file","args":[]}}]}}]})")
        << false;
    QTest::newRow("unknown-function")
        << QByteArray(
               R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"nope","args":{}}}]}}]})")
        << false;
    QTest::newRow("missing-name")
        << QByteArray(R"({"candidates":[{"content":{"parts":[{"functionCall":{"args":{}}}]}}]})")
        << false;
    QTest::newRow("valid-text")
        << QByteArray(
               R"({"candidates":[{"content":{"parts":[{"text":"ok"}]},"finishReason":"STOP"}]})")
        << true;
}

void NativeProviderContractTest::geminiMalformedResponseRejected() {
    QFETCH(QByteArray, body);
    QFETCH(bool, ok);
    ProviderHarness harness;
    harness.respondBody(body);
    auto provider = harness.provider(geminiBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic", options);
    QCOMPARE(reply.success, ok);
    QCOMPARE(harness.state->requests.size(), 1);
    if (!ok) {
        QCOMPARE(reply.category, ChatProviderErrorCategory::MalformedResponse);
        QCOMPARE(reply.error, ChatProviderReply::Error::InvalidResponse);
    }
}

void NativeProviderContractTest::geminiStructuredOutputContract() {
    ProviderHarness harness;
    harness.respondBody(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"{\"answer\":7}"}]},"finishReason":"STOP"}]})");
    auto binding = geminiBinding();
    binding.capabilities.structuredOutput = CapabilitySupport::Supported;
    auto provider = harness.provider(binding);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.structuredOutput = true;
    options.structuredSchemaName = QStringLiteral("answer");
    options.structuredSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("answer"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}}}};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic structured request", options);
    QVERIFY2(reply.success, qPrintable(reply.errorMessage));
    QVERIFY(reply.structuredResult.has_value());
    QCOMPARE(reply.structuredResult->value("answer").toInt(), 7);
    const auto generationConfig = harness.lastBody().value("generationConfig").toObject();
    QCOMPARE(generationConfig.value("responseMimeType").toString(),
             QStringLiteral("application/json"));
    QCOMPARE(generationConfig.value("responseSchema").toObject(), options.structuredSchema);
}

void NativeProviderContractTest::geminiCapabilityRejectionBeforeTransport() {
    ProviderHarness harness;
    auto provider = harness.provider(geminiBinding());
    ChatRequestOptions structuredOptions;
    structuredOptions.deadlineMs = 500;
    structuredOptions.structuredOutput = true;
    structuredOptions.structuredSchema =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};
    ScopedOnlineNetworkMode online;
    // Gemini's advertised structured-output capability defaults to Unknown.
    const auto reply = provider->sendRequest("synthetic", structuredOptions);
    QVERIFY(!reply.success);
    QCOMPARE(reply.category, ChatProviderErrorCategory::CapabilityUnsupported);
    QCOMPARE(reply.error, ChatProviderReply::Error::CapabilityRejected);
    QCOMPARE(harness.state->requests.size(), 0);
}

void NativeProviderContractTest::geminiCancellationBeforeDispatch() {
    ProviderHarness harness;
    auto provider = harness.provider(geminiBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    options.cancellationToken = std::make_shared<std::atomic_bool>(true);
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic", options);
    QVERIFY(!reply.success);
    QCOMPARE(reply.category, ChatProviderErrorCategory::Cancelled);
    QCOMPARE(harness.state->requests.size(), 0);
}

void NativeProviderContractTest::geminiDeadlineTimeout() {
    ProviderHarness harness;
    harness.respond(ScriptedHttpResponse::streaming({}, false));
    auto provider = harness.provider(geminiBinding());
    ChatRequestOptions options;
    options.deadlineMs = 150;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    ScopedOnlineNetworkMode online;
    const auto reply = provider->sendRequest("synthetic", options);
    QVERIFY(!reply.success);
    QCOMPARE(reply.category, ChatProviderErrorCategory::Timeout);
    QCOMPARE(reply.lifecycle, ChatRequestLifecycle::TimedOut);
    QCOMPARE(harness.state->requests.size(), 1);
}

void NativeProviderContractTest::geminiStreamingFinishReasonTerminal() {
    ScopedOnlineNetworkMode online;
    auto state = std::make_shared<ScriptedTransportState>();
    state->responder = [](const RecordedProviderRequest&) {
        return ScriptedHttpResponse::streaming({QByteArray(
            "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"gemini \"}]}}]}\n\n"
            "data: "
            "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"stream\"}]},\"finishReason\":"
            "\"STOP\"}]}\n\n")});
    };
    auto config = nativeStreamConfig(
        QUrl(QStringLiteral("https://generativelanguage.googleapis.com")), kGeminiKey, state);
    LocalInferenceRequest request;
    request.prompt = QStringLiteral("synthetic gemini stream");
    request.options.model = kGeminiModel;
    request.options.timeoutMs = 1000;
    QString deltas;
    const auto result = LMStudioLocalInferenceStreamClient(config, 1000)
                            .startStream(request, [&](const LocalInferenceStreamChunk& chunk) {
                                deltas += chunk.text;
                            });
    QCOMPARE(result.status, LocalInferenceStreamStatus::Completed);
    QCOMPARE(deltas, QStringLiteral("gemini stream"));
    QCOMPARE(state->requests.size(), 1);
    QCOMPARE(state->requests.first().url.host(),
             QStringLiteral("generativelanguage.googleapis.com"));
    QCOMPARE(state->requests.first().url.path(),
             QStringLiteral("/v1beta/models/gemini-2.5-pro:streamGenerateContent"));
    QVERIFY(state->requests.first().url.query().contains(QStringLiteral("alt=sse")));
}

// ---------------------------------------------------------------------------
// Shared transport safety and classification
// ---------------------------------------------------------------------------

void NativeProviderContractTest::cloudChatRequestConstruction_data() {
    QTest::addColumn<QString>("provider");
    for (const auto& provider : QStringList{QStringLiteral("claude"), QStringLiteral("gemini")})
        QTest::newRow(qPrintable(provider)) << provider;
}

void NativeProviderContractTest::cloudChatRequestConstruction() {
    QFETCH(QString, provider);
    const bool claude = provider == QLatin1String("claude");
    ProviderHarness harness;
    harness.respondBody(
        claude ? QByteArray(R"({"content":[{"type":"text","text":"cloud answer"}]})")
               : QByteArray(R"({"candidates":[{"content":{"parts":[{"text":"cloud answer"}]}}]})"));
    auto client = harness.provider(claude ? claudeBinding() : geminiBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    ScopedOnlineNetworkMode online;
    const auto reply = client->sendRequest("synthetic cloud chat", options);
    QVERIFY2(reply.success, qPrintable(reply.errorMessage));
    QCOMPARE(reply.message, QStringLiteral("cloud answer"));
    QCOMPARE(harness.state->requests.size(), 1);

    const auto& request = harness.lastRequest();
    QCOMPARE(request.url.scheme(), QStringLiteral("https"));
    if (claude) {
        QCOMPARE(request.url.host(), QStringLiteral("api.anthropic.com"));
        QCOMPARE(request.url.path(), QStringLiteral("/v1/messages"));
        QCOMPARE(request.headers.value("x-api-key"), kClaudeKey.toUtf8());
        QCOMPARE(request.headers.value("anthropic-version"), QByteArrayLiteral("2023-06-01"));
        const auto body = harness.lastBody();
        QCOMPARE(body.value("model").toString(), kClaudeModel);
        QCOMPARE(body.value("max_tokens").toInt(), 2048);
        QVERIFY(!body.value("stream").toBool());
        QVERIFY(!request.body.contains(kClaudeKey.toUtf8()));
    } else {
        QCOMPARE(request.url.host(), QStringLiteral("generativelanguage.googleapis.com"));
        QCOMPARE(request.url.path(),
                 QStringLiteral("/v1beta/models/gemini-2.5-pro:generateContent"));
        // The non-native Gemini chat branch authenticates with the query key.
        QVERIFY(request.url.query().contains(kGeminiKey));
        const auto body = harness.lastBody();
        QCOMPARE(body.value("contents")
                     .toArray()
                     .first()
                     .toObject()
                     .value("parts")
                     .toArray()
                     .first()
                     .toObject()
                     .value("text")
                     .toString(),
                 QStringLiteral("synthetic cloud chat"));
        QCOMPARE(body.value("generationConfig").toObject().value("maxOutputTokens").toInt(), 2048);
    }
}

void NativeProviderContractTest::nativeHttpFailureClassification_data() {
    QTest::addColumn<QString>("provider");
    QTest::addColumn<int>("status");
    QTest::addColumn<int>("category");
    struct Row {
        int status;
        ChatProviderErrorCategory category;
    };
    const QList<Row> rows{
        {400, ChatProviderErrorCategory::CapabilityUnsupported},
        {401, ChatProviderErrorCategory::AuthenticationRequired},
        {403, ChatProviderErrorCategory::AuthenticationRequired},
        {404, ChatProviderErrorCategory::ModelNotFound},
        {408, ChatProviderErrorCategory::Timeout},
        {422, ChatProviderErrorCategory::CapabilityUnsupported},
        {429, ChatProviderErrorCategory::RateLimited},
        {500, ChatProviderErrorCategory::ProviderUnavailable},
        {501, ChatProviderErrorCategory::CapabilityUnsupported},
        {503, ChatProviderErrorCategory::ProviderUnavailable},
    };
    for (const auto& provider : QStringList{QStringLiteral("claude"), QStringLiteral("gemini")})
        for (const auto& row : rows)
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(provider).arg(row.status)))
                << provider << row.status << int(row.category);
}

void NativeProviderContractTest::nativeHttpFailureClassification() {
    QFETCH(QString, provider);
    QFETCH(int, status);
    QFETCH(int, category);
    ProviderHarness harness;
    harness.respondBody(R"({"error":{"message":"PRIVATE_UPSTREAM_ECHO"}})", status);
    const auto binding = provider == QLatin1String("claude") ? claudeBinding() : geminiBinding();
    auto client = harness.provider(binding);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    ScopedOnlineNetworkMode online;
    const auto reply = client->sendRequest("synthetic", options);
    QVERIFY(!reply.success);
    QCOMPARE(reply.category, static_cast<ChatProviderErrorCategory>(category));
    QCOMPARE(reply.httpStatus, status);
    QCOMPARE(reply.attempts, 1);
    QVERIFY(reply.lifecycle != ChatRequestLifecycle::Completed);
    QVERIFY(!reply.errorMessage.contains(QStringLiteral("PRIVATE_")));
    QVERIFY(!reply.errorMessage.contains(kClaudeKey));
    QVERIFY(!reply.errorMessage.contains(kGeminiKey));
    QVERIFY(!QJsonDocument(reply.diagnostics).toJson().contains("PRIVATE_"));
    QCOMPARE(harness.state->requests.size(), 1);
}

void NativeProviderContractTest::nativeSafeErrorProjection_data() {
    QTest::addColumn<QString>("provider");
    for (const auto& provider : QStringList{QStringLiteral("claude"), QStringLiteral("gemini")})
        QTest::newRow(qPrintable(provider)) << provider;
}

void NativeProviderContractTest::nativeSafeErrorProjection() {
    QFETCH(QString, provider);
    const bool claude = provider == QLatin1String("claude");
    const QString key = claude ? kClaudeKey : kGeminiKey;
    ProviderHarness harness;
    const QByteArray body =
        QJsonDocument(
            QJsonObject{{QStringLiteral("error"),
                         QJsonObject{{QStringLiteral("message"),
                                      QStringLiteral("rejected %1 PRIVATE_PAYLOAD").arg(key)}}}})
            .toJson(QJsonDocument::Compact);
    harness.respondBody(body, 401);
    auto client = harness.provider(claude ? claudeBinding() : geminiBinding());
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = true;
    options.tools = {syntheticReadTool()};
    ScopedOnlineNetworkMode online;
    const auto reply = client->sendRequest("synthetic prompt", options);
    QCOMPARE(reply.category, ChatProviderErrorCategory::AuthenticationRequired);
    QVERIFY(!reply.errorMessage.contains(QStringLiteral("PRIVATE_")));
    QVERIFY(!reply.errorMessage.contains(key));
    QVERIFY(!QJsonDocument(reply.diagnostics).toJson().contains("PRIVATE_"));
    QVERIFY(!QJsonDocument(reply.diagnostics).toJson().contains(key.toUtf8()));
    QVERIFY(reply.errorMessage.size() < 512);
}

void NativeProviderContractTest::injectedTransportCannotBypassNetworkPolicy() {
    ProviderHarness harness;
    // The injected factory is installed, but the ambient policy stays LocalOnly:
    // the production NetworkPolicyService gate must still refuse the cloud host,
    // and the synthetic transport must never be consulted.
    harness.respondBody(R"({"content":[{"type":"text","text":"should not run"}]})");
    auto provider = harness.provider(claudeBinding());
    QVERIFY(provider);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    const auto reply = provider->sendRequest("synthetic", options);
    QVERIFY(!reply.success);
    QCOMPARE(reply.category, ChatProviderErrorCategory::Offline);
    QCOMPARE(harness.state->requests.size(), 0);
}

void NativeProviderContractTest::defaultTransportRemainsProductionHttps() {
    // Production never installs a factory: the default configuration keeps the
    // real Qt transport, the authenticated HTTPS endpoint, and the credential
    // requirement enforced by isAllowedEndpoint().
    LMStudioConfig config;
    QVERIFY(!config.networkFactory);
    QVERIFY(config.isLoopbackHttp());
    QVERIFY(config.isAllowedEndpoint());
    config.endpoint = QUrl(QStringLiteral("https://api.anthropic.com"));
    QVERIFY(config.isCloud());
    QVERIFY(!config.isAllowedEndpoint());
    config.apiKey = kClaudeKey;
    QVERIFY(config.isAllowedEndpoint());
    QCOMPARE(config.endpoint.scheme(), QStringLiteral("https"));
    QCOMPARE(config.endpoint.host(), QStringLiteral("api.anthropic.com"));
}

void NativeProviderContractTest::nativeRequestBodyByteEquivalence() {
    // The exact bytes POSTed to the provider must be unchanged by the P2-M03
    // extraction of the native request serialization. The golden strings are
    // captured from the pre-refactor ModelService::nativeEndpointReply assembly.
    {
        // Native tool calling forces the ModelService::nativeEndpointReply path
        // (a plain request without native capabilities routes through infer()).
        ProviderHarness harness;
        harness.respondBody(R"({"content":[{"type":"text","text":"ok"}]})");
        auto provider = harness.provider(claudeBinding());
        QVERIFY(provider);
        ChatRequestOptions options;
        options.deadlineMs = 500;
        options.nativeToolCalling = true;
        options.tools = {syntheticReadTool()};
        ScopedOnlineNetworkMode online;
        const auto reply = provider->sendRequest("synthetic claude prompt", options);
        QVERIFY2(reply.success, qPrintable(reply.errorMessage));
        // Anthropic native body: max_tokens (2048 fallback), messages, model, flat
        // tool declarations. No `stream` key on the native path.
        QCOMPARE(
            harness.lastRequest().body,
            QByteArrayLiteral(
                R"({"max_tokens":2048,"messages":[{"content":"synthetic claude prompt","role":"user"}],"model":"claude-sonnet-4-20250514","tools":[{"description":"Read the contents of a workspace file.","input_schema":{"properties":{"path":{"description":"Workspace-relative path.","type":"string"}},"required":["path"],"type":"object"},"name":"read-file"}]})"));
    }
    {
        ProviderHarness harness;
        harness.respondBody(
            R"({"candidates":[{"content":{"role":"model","parts":[{"text":"ok"}]},"finishReason":"STOP"}]})");
        auto provider = harness.provider(geminiBinding());
        QVERIFY(provider);
        ChatRequestOptions options;
        options.deadlineMs = 500;
        options.nativeToolCalling = true;
        options.tools = {syntheticReadTool()};
        ScopedOnlineNetworkMode online;
        const auto reply = provider->sendRequest("synthetic gemini prompt", options);
        QVERIFY2(reply.success, qPrintable(reply.errorMessage));
        // Gemini generateContent removes `model` before dispatch (the URL carries it)
        // and omits generationConfig when no output bound is set. Declarations use
        // the parametersJsonSchema field.
        QCOMPARE(
            harness.lastRequest().body,
            QByteArrayLiteral(
                R"({"contents":[{"parts":[{"text":"synthetic gemini prompt"}],"role":"user"}],"tools":[{"functionDeclarations":[{"description":"Read the contents of a workspace file.","name":"read-file","parametersJsonSchema":{"properties":{"path":{"description":"Workspace-relative path.","type":"string"}},"required":["path"],"type":"object"}}]}]})"));
    }
}

void NativeProviderContractTest::nativeResponseNormalizationEquivalence() {
    // The P2-M04 extraction must preserve the exact production response shape:
    // text is concatenated in provider order, tool calls are normalized with the
    // registered tool id, and the continuation embeds the raw response content
    // blocks unmodified. These assertions run through the real
    // ModelService::nativeEndpointReply path.
    const QString claudeContent =
        R"([{"type":"text","text":"reading "},)"
        R"({"type":"tool_use","id":"toolu_a","name":"read-file","input":{"path":"a"}},)"
        R"({"type":"text","text":"now"}])";
    {
        ProviderHarness harness;
        harness.respondBody(
            QJsonDocument(QJsonObject{{QStringLiteral("content"),
                                       QJsonDocument::fromJson(claudeContent.toUtf8()).array()}})
                .toJson(QJsonDocument::Compact));
        auto provider = harness.provider(claudeBinding());
        QVERIFY(provider);
        ChatRequestOptions options;
        options.deadlineMs = 500;
        options.nativeToolCalling = true;
        options.tools = {syntheticReadTool()};
        ScopedOnlineNetworkMode online;
        const auto reply = provider->sendRequest("please read a", options);
        QVERIFY2(reply.success, qPrintable(reply.errorMessage));
        QCOMPARE(reply.message, QStringLiteral("reading now"));
        QCOMPARE(reply.toolCalls.size(), 1);
        QCOMPARE(reply.toolCalls.first().callId, QStringLiteral("toolu_a"));
        QCOMPARE(reply.toolCalls.first().toolId, QStringLiteral("read-file"));
        const auto continuation =
            QJsonDocument::fromJson(reply.toolCalls.first().providerContinuation.toUtf8()).array();
        QCOMPARE(continuation.size(), 2);
        QCOMPARE(continuation.first().toObject().value("role").toString(), QStringLiteral("user"));
        QCOMPARE(continuation.last().toObject().value("content").toArray(),
                 QJsonDocument::fromJson(claudeContent.toUtf8()).array());
    }
    {
        // Multiple tool calls are accepted in provider order.
        ProviderHarness harness;
        harness.respondBody(
            R"({"content":[{"type":"tool_use","id":"toolu_1","name":"read-file","input":{"path":"a"}},{"type":"tool_use","id":"toolu_2","name":"read-file","input":{"path":"b"}}]})");
        auto provider = harness.provider(claudeBinding());
        QVERIFY(provider);
        ChatRequestOptions options;
        options.deadlineMs = 500;
        options.nativeToolCalling = true;
        options.tools = {syntheticReadTool()};
        ScopedOnlineNetworkMode online;
        const auto reply = provider->sendRequest("read both", options);
        QVERIFY2(reply.success, qPrintable(reply.errorMessage));
        QCOMPARE(reply.toolCalls.size(), 2);
        QCOMPARE(reply.toolCalls.at(0).callId, QStringLiteral("toolu_1"));
        QCOMPARE(reply.toolCalls.at(1).callId, QStringLiteral("toolu_2"));
    }
    {
        // Gemini: mixed text + functionCall; the candidate content is embedded
        // verbatim in the continuation.
        ProviderHarness harness;
        harness.respondBody(
            R"({"candidates":[{"content":{"role":"model","parts":[{"text":"ready "},{"functionCall":{"name":"read-file","args":{"path":"a"}}}]},"finishReason":"STOP"}]})");
        auto provider = harness.provider(geminiBinding());
        QVERIFY(provider);
        ChatRequestOptions options;
        options.deadlineMs = 500;
        options.nativeToolCalling = true;
        options.tools = {syntheticReadTool()};
        ScopedOnlineNetworkMode online;
        const auto reply = provider->sendRequest("read a", options);
        QVERIFY2(reply.success, qPrintable(reply.errorMessage));
        QCOMPARE(reply.message, QStringLiteral("ready "));
        QCOMPARE(reply.toolCalls.size(), 1);
        QVERIFY(reply.toolCalls.first().callId.startsWith(QStringLiteral("sentinel-gemini-")));
        const auto continuation =
            QJsonDocument::fromJson(reply.toolCalls.first().providerContinuation.toUtf8()).array();
        QCOMPARE(continuation.last().toObject().value("role").toString(), QStringLiteral("model"));
    }
}

QTEST_GUILESS_MAIN(NativeProviderContractTest)
#include "test_native_provider_contract.moc"
