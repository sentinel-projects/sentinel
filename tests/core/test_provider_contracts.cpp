// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef Q_MOC_RUN
#include "../support/ProviderHttpFixture.h"
#include "../support/ScriptedProviderTransport.h"
#endif
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/chat/OllamaChatProvider.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/network/NetworkPolicyService.h"
#include "sentinel/core/provider/ProviderCapabilityNormalizer.h"
#include "sentinel/core/provider/ProviderErrorClassifier.h"
#include "sentinel/core/provider/ProviderMessageNormalizer.h"
#include "sentinel/core/provider/SelectedEndpointChatProvider.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/LocalInference.h"
#include "sentinel/core/runtime/ProviderRequestRuntime.h"
#include "sentinel/core/security/ProviderCredentials.h"
#include <QJsonArray>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QtTest>

using namespace sentinel::core;
using sentinel::test::ProviderHttpFixture;

class ProviderContractsTest final : public QObject {
    Q_OBJECT
private slots:

    void reportedUsageRemainsOptionalAndStrict() {
        QVERIFY(!provider::normalizeOpenAiOutcome(
                     QJsonObject{{"usage", QJsonObject{{"completion_tokens", 1.5}}}})
                     .outputTokens);
        QJsonObject diagnostics;
        provider::preserveReportedUsage(
            QJsonObject{{"usage", QJsonObject{{"prompt_tokens", 0},
                                              {"completion_tokens", 1.5},
                                              {"total_tokens", "PRIVATE_COUNT"},
                                              {"header", "PRIVATE_HEADER"}}}},
            diagnostics);
        QCOMPARE(diagnostics.value("prompt_tokens").toInt(-1), 0);
        QVERIFY(!diagnostics.contains("completion_tokens"));
        QVERIFY(!diagnostics.contains("total_tokens"));
        provider::preserveReportedUsage(QJsonObject{{"usage", QJsonObject{{"prompt_tokens", -1}}}},
                                        diagnostics);
        QCOMPARE(diagnostics.value("prompt_tokens").toInt(-1), 0);
        QVERIFY(!QJsonDocument(diagnostics).toJson().contains("PRIVATE_"));
    }

    void streamTransportParity_data() {
        QTest::addColumn<QString>("backend");
        QTest::addColumn<QString>("scenario");
        for (const auto* backend : {"cloud", "local", "llama", "ollama", "claude", "gemini"})
            for (const auto* scenario : {"success", "partial", "timeout", "cancel", "tls",
                                         "expired", "usage-partial", "truncated"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(backend, scenario)))
                    << QString(backend) << QString(scenario);
    }
    void streamTransportParity() {
        QFETCH(QString, backend);
        QFETCH(QString, scenario);
        sentinel::test::ScopedOnlineNetworkMode online;
        auto state = std::make_shared<sentinel::test::ScriptedTransportState>();
        state->responder = [=](const auto&) {
            QList<QByteArray> frames;
            if (backend == "ollama") {
                frames = {
                    R"({"response":"hello","done":false})"
                    "\n",
                    R"({"done":true,"done_reason":"stop","eval_count":2,"prompt_eval_count":3})"
                    "\n"};
            } else if (backend == "claude") {
                frames = {
                    "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\"hello\"}}\n\n",
                    "data: {\"type\":\"message_stop\"}\n\n"};
            } else if (backend == "gemini") {
                frames = {
                    "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hello\"}]}}]}\n\n",
                    "data: "
                    "{\"candidates\":[{\"content\":{\"parts\":[]},\"finishReason\":\"STOP\"}]}"
                    "\n\n"};
            } else {
                frames = {
                    "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n",
                    "data: "
                    "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_"
                    "tokens\":3,\"completion_tokens\":2,\"total_tokens\":5}}\n\ndata: [DONE]\n\n"};
            }

            if (scenario == "truncated") {
                frames = {
                    frames.first(),
                    backend == "claude"   ? QByteArray("data: "
                                                         "{\"type\":\"message_delta\",\"delta\":{"
                                                         "\"stop_reason\":\"max_tokens\"}}\n\n")
                    : backend == "gemini" ? QByteArray("data: "
                                                       "{\"candidates\":[{\"content\":{\"parts\":[]"
                                                       "},\"finishReason\":\"MAX_TOKENS\"}]}\n\n")
                    : backend == "ollama"
                        ? QByteArray("{\"done\":true,\"done_reason\":\"length\"}\n")
                        : QByteArray(
                              "data: "
                              "{\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n")};
            }
            if (scenario == "usage-partial") {
                if (backend == "claude") {
                    frames = {"data: "
                              "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_"
                              "tokens\":3}}}\n\n",
                              frames.first(),
                              "data: "
                              "{\"type\":\"message_delta\",\"delta\":{},\"usage\":{\"output_"
                              "tokens\":2}}\n\n"};
                } else if (backend == "gemini") {
                    frames = {"data: "
                              "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hello\"}]}}],"
                              "\"usageMetadata\":{\"promptTokenCount\":3,\"candidatesTokenCount\":"
                              "2,\"totalTokenCount\":5}}\n\n"};
                } else if (backend == "ollama") {
                    frames = {
                        R"({"response":"hello","done":false,"prompt_eval_count":3,"eval_count":2})"
                        "\n"};
                } else {
                    frames = {frames.first(), "data: "
                                              "{\"choices\":[],\"usage\":{\"prompt_tokens\":3,"
                                              "\"completion_tokens\":2,\"total_tokens\":5}}\n\n"};
                }
            }
            if (scenario == "partial" || scenario == "cancel")
                frames = {frames.first()};
            if (scenario == "timeout" || scenario == "tls")
                frames.clear();
            auto response = sentinel::test::ScriptedHttpResponse::streaming(
                frames, scenario == "success" || scenario == "tls" || scenario == "truncated");
            response.emitMetadataSignal = scenario != "tls";
            if (scenario == "tls") {
                response.status = 0;
                response.error = QNetworkReply::SslHandshakeFailedError;
                response.errorString = "PRIVATE_TRANSPORT_ERROR";
            }
            return response;
        };
        std::shared_ptr<IChatProvider> provider;
        if (backend == "ollama") {
            auto config = OllamaConfig::fromEndpoint("http://127.0.0.1:11434");
            config.healthCheckEnabled = config.modelDiscoveryEnabled = false;
            config.networkFactory = std::make_shared<sentinel::test::ScriptedNetworkFactory>(state);
            auto ollama = std::make_shared<OllamaChatProvider>(config, 0);
            ollama->setSelectedModel("fixture");
            provider = ollama;
        } else {
            LMStudioConfig config;
            config.endpoint = backend == "claude" ? QUrl("https://api.anthropic.com")
                              : backend == "gemini"
                                  ? QUrl("https://generativelanguage.googleapis.com")
                              : backend == "cloud" ? QUrl("https://fixture.invalid/v1")
                                                   : QUrl("http://127.0.0.1:1234/v1");
            config.apiKey = "PRIVATE_KEY";
            config.networkFactory = std::make_shared<sentinel::test::ScriptedNetworkFactory>(state);
            ModelBinding binding{backend == "local"   ? "lm-studio"
                                 : backend == "llama" ? "llama-cpp-server"
                                                      : "cloud-api",
                                 "fixture"};
            binding.capabilities.streaming = CapabilitySupport::Supported;
            provider = makeSelectedEndpointChatProvider(binding, config, 0);
        }
        auto token = std::make_shared<std::atomic_bool>(false);
        qint64 now = 0;
        ChatRequestOptions options;
        options.deadlineMs = 30;
        options.requireBoundedStreaming = true;
        options.cancellationToken = token;
        options.operationDeadline = std::make_shared<OperationDeadline>(30, [&] { return now; });
        if (scenario == "expired")
            now = 30;
        if (scenario == "cancel")
            QTimer::singleShot(2, [token] { token->store(true); });
        QString visible;
        const auto reply = provider->sendMessageStreaming(
            "PRIVATE_PROMPT", [&](const QString& delta) { visible += delta; }, token, options);
        QCOMPARE(reply.success, scenario == "success");
        QCOMPARE(state->requests.size(), scenario == "expired" ? 0 : 1);
        const auto frames = reply.diagnostics.value("transport_attempts").toArray();
        if (scenario != "expired") {
            QCOMPARE(frames.size(), 1);
            const auto phase = frames.first().toObject();
            QCOMPARE(phase.value("response_completed").toBool(),
                     scenario == "success" || scenario == "truncated");
            QCOMPARE(phase.value("partial_body").toBool(),
                     scenario == "partial" || scenario == "cancel" || scenario == "usage-partial");
            QCOMPARE(phase.value("http_status").isNull(), scenario == "tls");
            QCOMPARE(reply.httpStatus, scenario == "tls" ? 0 : 200);
            QVERIFY(!QJsonDocument(reply.diagnostics).toJson().contains("PRIVATE_"));
        }
        if (scenario == "partial" || scenario == "usage-partial")
            QCOMPARE(visible, QString("hello"));
        if (scenario == "success")
            QCOMPARE(reply.message, QString("hello"));
        if (scenario == "usage-partial") {
            QCOMPARE(reply.diagnostics.value("prompt_tokens").toInt(-1), 3);
            QCOMPARE(reply.diagnostics.value("completion_tokens").toInt(-1), 2);
            QCOMPARE(reply.diagnostics.contains("total_tokens"),
                     backend != "claude" && backend != "ollama");
        } else if (backend == "claude" || backend == "gemini" || scenario != "success")
            QVERIFY(!reply.diagnostics.contains("total_tokens"));
        if (scenario == "partial" || scenario == "timeout" || scenario == "expired" ||
            scenario == "usage-partial")
            QCOMPARE(reply.category, ChatProviderErrorCategory::Timeout);
        if (scenario == "cancel")
            QCOMPARE(reply.category, ChatProviderErrorCategory::Cancelled);
        if (scenario == "truncated") {
            QCOMPARE(reply.category, ChatProviderErrorCategory::RequestRejected);
            QVERIFY(reply.diagnostics.value("output_limit_reached").toBool());
        }
    }
    void initTestCase();
    void capabilityDefaultsAreNotHealth();
    void missingCredentialsNeverFallback();
    void ollamaDiscoveryContract_data();
    void ollamaDiscoveryContract();
    void reportedCapabilitiesKeepUnknownDistinct();
    void structuredOutputContract_data();
    void structuredOutputContract();
    void ollamaRejectsNativeCapabilities();
    void discoveryContract_data();
    void discoveryContract();
    void malformedDiscoveryPreservesCatalogSequencing();
    void unavailableAndStaleCatalogs();
    void nativeToolContract_data();
    void nativeToolContract();
    void unsupportedCapabilitiesRejectBeforeTransport();
    void bindingAndEndpointRemainFrozenDuringRequest();
    void classifierAndRetryContracts();
    void normalizerPreservesProtocolWithoutInventingCapabilities();
    void nativeCloudProtocolCannotBeRedirectedToLocalMock();
    void malformedDiscoveryShapeIsNotHealthy();
    void unsupportedVisionIsRejectedBeforeTransport();
    void visionCapabilityMatrix_data();
    void visionCapabilityMatrix();
    void visionGuardUsesFrozenBinding();
    void echoedSecretsDoNotEnterDiagnostics();
    void publicErrorsAreAllowlisted_data();
    void publicErrorsAreAllowlisted();
    void httpFailureClassification_data();
    void httpFailureClassification();
    void streamingErrorsNeverEmitPayload_data();
    void streamingErrorsNeverEmitPayload();
    void unknownClassifierMessageIsSafe();
};

namespace {
// Capture real Qt diagnostics with the provider category enabled, restoring global
// logging state at scope exit. Fixtures are sequential; handler access is locked.
class ScopedProviderLogs {
public:
    ScopedProviderLogs() {
        active_ = this;
        oldHandler_ = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext&, const QString& message) {
                QMutexLocker lock(&active_->mutex_);
                active_->messages_.append(message);
            });
        oldFilter_ = QLoggingCategory::installFilter([](QLoggingCategory* category) {
            if (QByteArray(category->categoryName()) == "sentinel.provider.diagnostics")
                category->setEnabled(QtInfoMsg, true);
        });
    }
    ~ScopedProviderLogs() {
        QLoggingCategory::installFilter(oldFilter_);
        qInstallMessageHandler(oldHandler_);
        active_ = nullptr;
    }
    QString text() {
        QMutexLocker lock(&mutex_);
        return messages_.join('\n');
    }

private:
    static inline ScopedProviderLogs* active_ = nullptr;
    QMutex mutex_;
    QStringList messages_;
    QtMessageHandler oldHandler_;
    QLoggingCategory::CategoryFilter oldFilter_;
};
const QStringList localIds{"ollama", "llama-cpp-server", "lm-studio", "openai-compatible-local"};
const QStringList cloudIds{
    "cloud-api", "openai", "openai-compatible", "claude", "gemini", "deepseek", "groq", "mistral"};
ModelBinding nativeBinding(const QString& id) {
    ModelBinding binding{id, "fixture"};
    binding.capabilities.nativeToolCalling = CapabilitySupport::Supported;
    binding.capabilities.contextWindow = 16384;
    return binding;
}
void endpoint(ModelService& service, const QString& id, const QUrl& url) {
    if (id == "llama-cpp-server")
        service.setLlamaCppEndpoint(url.toString());
    else
        service.setLmStudioEndpoint(url.toString());
}
} // namespace

void ProviderContractsTest::initTestCase() {
    // Every new request is loopback or rejected before transport. No OS credential store.
    NetworkPolicyService::instance().setMode(NetworkMode::LocalOnly);
}

void ProviderContractsTest::capabilityDefaultsAreNotHealth() {
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    for (const auto& id : localIds + cloudIds) {
        QVERIFY(service.isKnownProvider(id));
        const auto status = service.providerStatusSnapshot(id, "fixture");
        QCOMPARE(status.health, ProviderHealth::Unknown);
        const auto caps = service.capabilities(id, "fixture");
        QCOMPARE(caps.visionInput, CapabilitySupport::Unknown);
        QCOMPARE(caps.audioInput, CapabilitySupport::Unknown);
        QCOMPARE(caps.streaming, CapabilitySupport::Supported);
        QCOMPARE(caps.nativeToolCalling, id == "claude" || id == "gemini"
                                             ? CapabilitySupport::Supported
                                             : CapabilitySupport::Unsupported);
        if (id != "ollama")
            QCOMPARE(status.catalog, ProviderCatalogState::Unverified);
    }
}

void ProviderContractsTest::missingCredentialsNeverFallback() {
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    int fallbackCalls = 0;
    service.registerProvider("ollama", [&](const ModelBinding&) {
        ++fallbackCalls;
        return std::shared_ptr<IChatProvider>{};
    });
    for (const auto& id : cloudIds) {
        const auto resolution = service.resolve(id, "fixture");
        QVERIFY(!resolution.ok());
        QCOMPARE(resolution.error, ModelBindingError::AuthenticationRequired);
        QCOMPARE(resolution.binding.providerId, id);
        QVERIFY(!resolution.provider);
    }
    QCOMPARE(service.resolve("not-a-provider", "fixture").error,
             ModelBindingError::ProviderNotFound);
    QCOMPARE(service.resolve("lm-studio", "").error, ModelBindingError::ModelNotFound);
    QCOMPARE(fallbackCalls, 0);
}

void ProviderContractsTest::discoveryContract_data() {
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("status");
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("category");
    QTest::newRow("catalog") << QByteArray(R"({"data":[{"id":"fixture"}]})") << 200 << 1
                             << int(ChatProviderErrorCategory::None);
    QTest::newRow("empty") << QByteArray(R"({"data":[]})") << 200 << 0
                           << int(ChatProviderErrorCategory::None);
    QTest::newRow("auth") << QByteArray(R"({"error":"denied"})") << 401 << 0
                          << int(ChatProviderErrorCategory::AuthenticationRequired);
    QTest::newRow("missing-model") << QByteArray(R"({"error":"missing"})") << 404 << 0
                                   << int(ChatProviderErrorCategory::ModelNotFound);
    QTest::newRow("invalid-json") << QByteArray("{broken") << 200 << 0
                                  << int(ChatProviderErrorCategory::MalformedResponse);
    for (const auto& row : QList<QPair<QByteArray, QByteArray>>{
             {"missing-data", R"({"PRIVATE_DISCOVERY":"body"})"},
             {"data-object", R"({"data":{}})"},
             {"data-string", R"({"data":"PRIVATE_DISCOVERY"})"},
             {"data-null", R"({"data":null})"},
             {"entry-null", R"({"data":[null]})"},
             {"entry-array", R"({"data":[[]]})"},
             {"entry-string", R"({"data":["PRIVATE_DISCOVERY"]})"},
             {"entry-number", R"({"data":[42]})"},
             {"missing-id", R"({"data":[{"name":"PRIVATE_DISCOVERY"}]})"},
             {"numeric-id", R"({"data":[{"id":42}]})"},
             {"null-id", R"({"data":[{"id":null}]})"},
             {"empty-id", R"({"data":[{"id":""}]})"},
             {"blank-id", R"({"data":[{"id":"  "}]})"},
             {"mixed-catalog", R"({"data":[{"id":"fixture"},{"missing":"PRIVATE_DISCOVERY"}]})"},
             {"top-array", R"([{"id":"fixture"}])"}})
        QTest::newRow(row.first.constData())
            << row.second << 200 << 0 << int(ChatProviderErrorCategory::MalformedResponse);
    QTest::newRow("multiple-valid")
        << QByteArray(R"({"data":[{"id":"fixture"},{"id":"fixture-two"}]})") << 200 << 2
        << int(ChatProviderErrorCategory::None);
    for (int status : {403, 429, 500})
        QTest::newRow(qPrintable(QString("http-%1").arg(status)))
            << QByteArray(R"({"error":"PRIVATE_DISCOVERY"})") << status << 0
            << int(ProviderRequestRuntime::classify(status, QNetworkReply::NoError));
}

void ProviderContractsTest::discoveryContract() {
    QFETCH(QByteArray, body);
    QFETCH(int, status);
    QFETCH(int, count);
    QFETCH(int, category);
    auto state = std::make_shared<sentinel::test::ScriptedTransportState>();
    state->responder = [=](const auto&) {
        return sentinel::test::ScriptedHttpResponse::body(body, status);
    };
    auto factory = std::make_shared<sentinel::test::ScriptedNetworkFactory>(state);
    sentinel::test::ScopedOnlineNetworkMode online;
    ProviderDiscoveryOutcome outcome;
    QString error;
    const auto models = fetchOpenAiCloudModels(QUrl("https://fixture.invalid/v1/models"),
                                               "synthetic-key", 500, &error, {}, &outcome, factory);
    QCOMPARE(models.size(), count);
    QCOMPARE(int(outcome.category), category);
    QCOMPARE(outcome.completed, category == int(ChatProviderErrorCategory::None));
    QVERIFY(!outcome.requestId.isEmpty());
    QCOMPARE(state->requests.size(), status == 429 ? 2 : 1);
    QCOMPARE(state->requests.first().headers.value("authorization"),
             QByteArray("Bearer synthetic-key"));
    QVERIFY(!error.contains("synthetic-key"));
    QVERIFY(!error.contains("PRIVATE_DISCOVERY"));
    QVERIFY(error.size() < 256);
    QCOMPARE(outcome.httpStatus, status);
    if (count) {
        QCOMPARE(models.first().name, QString("fixture"));
        if (count == 2)
            QCOMPARE(models.last().name, QString("fixture-two"));
    }
    if (!models.isEmpty()) {
        QCOMPARE(models.first().capabilities.nativeToolCalling, CapabilitySupport::Unknown);
        QCOMPARE(models.first().capabilities.visionInput, CapabilitySupport::Unknown);
        QVERIFY(!models.first().capabilities.contextWindow);
    }
}

void ProviderContractsTest::unavailableAndStaleCatalogs() {
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    ProviderDiscoveryOutcome success{true, ChatProviderErrorCategory::None, 200, "new"};
    ProviderDiscoveryOutcome failure{false, ChatProviderErrorCategory::ConnectionFailed, 0, "old"};
    for (const auto& id : QStringList{"llama-cpp-server", "lm-studio", "openai-compatible-local"}) {
        const auto old = service.beginProviderHealthObservation();
        const auto current = service.beginProviderHealthObservation();
        service.acceptProviderDiscovery(id, {{"fixture"}}, success, current);
        service.acceptProviderDiscovery(id, {}, failure, old);
        QCOMPARE(service.providerStatusSnapshot(id).catalog, ProviderCatalogState::Available);
        QCOMPARE(service.resolve(id, "absent").error, ModelBindingError::ModelNotFound);
        service.acceptProviderDiscovery(id, {}, failure, service.beginProviderHealthObservation());
        QCOMPARE(service.providerStatusSnapshot(id).catalog, ProviderCatalogState::Stale);
        QCOMPARE(service.providerStatusSnapshot(id).modelIds, QStringList{"fixture"});
    }
    OllamaModelDiscoveryResult catalog;
    catalog.lifecycle = ChatRequestLifecycle::Completed;
    catalog.models = {{"fixture"}};
    service.acceptOllamaDiscovery(catalog, service.beginProviderHealthObservation());
    QCOMPARE(service.resolve("ollama", "absent").error, ModelBindingError::ModelNotFound);
    catalog.models.clear();
    service.acceptOllamaDiscovery(catalog, service.beginProviderHealthObservation());
    QCOMPARE(service.resolve("ollama", "fixture").error, ModelBindingError::ModelNotFound);
}

void ProviderContractsTest::nativeToolContract_data() {
    QTest::addColumn<QString>("id");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<bool>("ok");
    for (const auto& id : QStringList{"llama-cpp-server", "lm-studio"}) {
        const QList<QPair<QByteArray, QByteArray>> rows{
            {"valid",
             R"({"choices":[{"message":{"tool_calls":[{"id":"call-1","function":{"name":"read-file","arguments":"{\"path\":\"fixture.txt\"}"}}]}}]})"},
            {"unknown-tool",
             R"({"choices":[{"message":{"tool_calls":[{"id":"call-1","function":{"name":"unknown","arguments":"{}"}}]}}]})"},
            {"array-arguments",
             R"({"choices":[{"message":{"tool_calls":[{"id":"call-1","function":{"name":"read-file","arguments":"[]"}}]}}]})"},
            {"invalid-arguments",
             R"({"choices":[{"message":{"tool_calls":[{"id":"call-1","function":{"name":"read-file","arguments":"{broken"}}]}}]})"},
            {"missing-call-id",
             R"({"choices":[{"message":{"tool_calls":[{"function":{"name":"read-file","arguments":"{}"}}]}}]})"},
            {"no-choices", R"({"choices":[]})"}};
        for (const auto& row : rows)
            QTest::newRow(qPrintable(id + '-' + row.first))
                << id << row.second << (row.first == "valid");
    }
}

void ProviderContractsTest::nativeToolContract() {
    QFETCH(QString, id);
    QFETCH(QByteArray, body);
    QFETCH(bool, ok);
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [=](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(socket, body);
    };
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    endpoint(service, id, server.endpoint());
    auto provider = service.constructProvider(nativeBinding(id));
    QVERIFY(provider);
    ChatRequestOptions options;
    options.nativeToolCalling = true;
    options.tools = BuiltInToolProvider::descriptors();
    options.deadlineMs = 500;
    const auto reply = provider->sendRequest("synthetic tool request", options);
    QCOMPARE(reply.success, ok);
    QCOMPARE(server.requests.size(), 1);
    const auto tools = server.requests.first().body.value("tools").toArray();
    QVERIFY(!tools.isEmpty());
    const auto definition = tools.first().toObject().value("function").toObject();
    QCOMPARE(definition.value("parameters").toObject(), options.tools.first().inputSchema);
    if (ok) {
        QCOMPARE(reply.toolCalls.size(), 1);
        QCOMPARE(reply.toolCalls.first().arguments.value("path").toString(),
                 QString("fixture.txt"));
    } else {
        QVERIFY(reply.toolCalls.isEmpty());
        QCOMPARE(reply.category, ChatProviderErrorCategory::MalformedResponse);
    }
}

void ProviderContractsTest::unsupportedCapabilitiesRejectBeforeTransport() {
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    for (const auto& id : QStringList{"llama-cpp-server", "lm-studio"}) {
        endpoint(service, id, server.endpoint());
        ModelBinding binding{id, "fixture"};
        auto provider = service.constructProvider(binding);
        for (bool native : {true, false}) {
            ChatRequestOptions options;
            options.nativeToolCalling = native;
            options.structuredOutput = !native;
            const auto reply = provider->sendRequest("unsupported", options);
            QVERIFY(!reply.success);
            QCOMPARE(reply.category, ChatProviderErrorCategory::CapabilityUnsupported);
        }
        const auto reply = provider->sendMessageStreaming("unsupported", {}, {});
        QCOMPARE(reply.category, ChatProviderErrorCategory::CapabilityUnsupported);
    }
    QCOMPARE(server.requests.size(), 0);
}

void ProviderContractsTest::bindingAndEndpointRemainFrozenDuringRequest() {
    ProviderHttpFixture server, future;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QVERIFY(future.listen(QHostAddress::LocalHost, 0));
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    service.setLmStudioEndpoint(server.endpoint().toString());
    auto binding = nativeBinding("lm-studio");
    service.setSelectedModel({binding.providerId, binding.modelId});
    auto provider = service.constructProvider(binding);
    server.respond = [&](QTcpSocket* socket, const auto&) {
        service.setSelectedModel({"llama-cpp-server", "next-model"});
        service.setLmStudioEndpoint(future.endpoint().toString());
        binding.modelId = "mutated-caller-copy";
        ProviderHttpFixture::reply(socket,
                                   R"({"choices":[{"message":{"content":"original answer"}}]})");
    };
    ChatRequestOptions options;
    options.nativeToolCalling = true;
    options.deadlineMs = 500;
    const auto reply = provider->sendRequest("synthetic frozen request", options);
    QVERIFY(reply.success);
    QCOMPARE(server.requests.first().body.value("model").toString(), QString("fixture"));
    QCOMPARE(reply.diagnostics.value("model_id").toString(), QString("fixture"));
    QCOMPARE(service.selectedModel().modelId, QString("next-model"));
    QCOMPARE(future.requests.size(), 0);
    QVERIFY(provider->sendRequest("second lease request", options).success);
    QCOMPARE(server.requests.size(), 2);
}

void ProviderContractsTest::classifierAndRetryContracts() {
    for (const auto& id : localIds + cloudIds) {
        const auto auth = ProviderErrorClassifier::classify(401, "synthetic-key", id);
        QCOMPARE(auth.category, ProviderErrorCategory::Authentication);
        QVERIFY(!auth.retryable);
        QVERIFY(!ProviderErrorClassifier::userMessage(auth).contains("synthetic-key"));
        QCOMPARE(ProviderErrorClassifier::classify(400, "maximum context length", id).category,
                 ProviderErrorCategory::InvalidRequest);
        QCOMPARE(ProviderErrorClassifier::classify(429, "quota exceeded", id).category,
                 ProviderErrorCategory::QuotaExceeded);
        QCOMPARE(ProviderErrorClassifier::classify(400, "content filter", id).category,
                 ProviderErrorCategory::ContentFilter);
    }
    ProviderTransportResult result;
    result.httpStatus = 503;
    QVERIFY(ProviderRequestRuntime::retryable(result, ProviderRequestMode::Generation, false));
    QVERIFY(
        !ProviderRequestRuntime::retryable(result, ProviderRequestMode::StreamingGeneration, true));
    result.cancelled = true;
    QVERIFY(!ProviderRequestRuntime::retryable(result, ProviderRequestMode::Generation, false));
    result.cancelled = false;
    result.timedOut = true;
    QVERIFY(!ProviderRequestRuntime::retryable(result, ProviderRequestMode::Generation, false));
}

void ProviderContractsTest::normalizerPreservesProtocolWithoutInventingCapabilities() {
    ProviderMessageNormalizer normalizer;
    const QJsonObject original{
        {"role", "assistant"},
        {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "answer"}},
                               QJsonObject{{"type", "image_url"}, {"image_url", "synthetic"}}}},
        {"tool_calls", QJsonArray{QJsonObject{{"id", "call-1"}}}}};
    for (const auto& id : localIds + cloudIds) {
        const auto message = normalizer.normalize(original, id);
        QCOMPARE(message.provider, id);
        QCOMPARE(message.content, QString("answer"));
        QCOMPARE(message.toolCalls.size(), 1);
        QCOMPARE(normalizer.denormalize(message), original);
    }
}

void ProviderContractsTest::nativeCloudProtocolCannotBeRedirectedToLocalMock() {
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    LMStudioConfig config;
    config.endpoint = server.endpoint();
    config.apiKey = "synthetic-key";
    LMStudioLocalInferenceClient client(config, 500);
    for (auto protocol : {LMStudioLocalInferenceClient::NativeProtocol::Claude,
                          LMStudioLocalInferenceClient::NativeProtocol::Gemini}) {
        const auto reply = client.completeNativeChat(protocol, {{"model", "fixture"}});
        QVERIFY(!reply.ok);
        QCOMPARE(reply.httpStatus, 501);
    }
    QCOMPARE(server.requests.size(), 0);
}

void ProviderContractsTest::malformedDiscoveryShapeIsNotHealthy() {
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(socket, R"({"unexpected":"object"})");
    };
    ProviderDiscoveryOutcome outcome;
    const auto models =
        fetchOpenAiCompatibleModels(server.endpoint(), 500, {}, nullptr, {}, &outcome);
    QVERIFY(models.isEmpty());
    QVERIFY(!outcome.completed);
}

void ProviderContractsTest::unsupportedVisionIsRejectedBeforeTransport() {
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(socket,
                                   R"({"choices":[{"message":{"content":"accepted image"}}]})");
    };
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    service.setLmStudioEndpoint(server.endpoint().toString());
    ModelBinding binding{"lm-studio", "fixture"};
    binding.capabilities.visionInput = CapabilitySupport::Unsupported;
    auto provider = service.constructProvider(binding);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.images.append(ChatImage{"image/png", "synthetic-image-bytes"});
    const auto reply = provider->sendRequest("image", options);
    QCOMPARE(reply.category, ChatProviderErrorCategory::CapabilityUnsupported);
    QCOMPARE(server.requests.size(), 0);
}

void ProviderContractsTest::echoedSecretsDoNotEnterDiagnostics() {
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(
            socket, R"({"error":{"message":"rejected synthetic-secret-echo"}})", 401);
    };
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    service.setLmStudioEndpoint(server.endpoint().toString());
    auto provider = service.constructProvider(nativeBinding("lm-studio"));
    ChatRequestOptions options;
    options.nativeToolCalling = true;
    options.deadlineMs = 500;
    const auto reply = provider->sendRequest("synthetic prompt", options);
    QCOMPARE(reply.category, ChatProviderErrorCategory::AuthenticationRequired);
    QVERIFY(!QJsonDocument(reply.diagnostics).toJson().contains("synthetic-secret-echo"));
    QVERIFY(!reply.errorMessage.contains("synthetic-secret-echo"));
}

void ProviderContractsTest::ollamaDiscoveryContract_data() {
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("status");
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("category");
    QTest::newRow("catalog") << QByteArray(R"({"models":[{"name":"fixture","size":42}]})") << 200
                             << 1 << int(ChatProviderErrorCategory::None);
    QTest::newRow("empty") << QByteArray(R"({"models":[]})") << 200 << 0
                           << int(ChatProviderErrorCategory::None);
    QTest::newRow("wrong-shape") << QByteArray(R"({"unexpected":[]})") << 200 << 0
                                 << int(ChatProviderErrorCategory::MalformedResponse);
    QTest::newRow("auth") << QByteArray(R"({"error":"denied"})") << 401 << 0
                          << int(ChatProviderErrorCategory::AuthenticationRequired);
}
void ProviderContractsTest::ollamaDiscoveryContract() {
    QFETCH(QByteArray, body);
    QFETCH(int, status);
    QFETCH(int, count);
    QFETCH(int, category);
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [=](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(socket, body, status);
    };
    auto config = OllamaConfig::fromEndpoint(server.endpoint().toString());
    config.modelDiscoveryTimeoutMs = 500;
    const auto result = OllamaHttpRuntimeClient(config, 500).discoverModels();
    QCOMPARE(result.models.size(), count);
    QCOMPARE(int(result.errorCategory), category);
    QCOMPARE(result.succeeded(), category == int(ChatProviderErrorCategory::None));
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().target, QByteArray("/api/tags"));
}
void ProviderContractsTest::reportedCapabilitiesKeepUnknownDistinct() {
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(
            socket,
            R"({"data":[{"id":"explicit","tool_calling":true,"structured_output":false,"streaming":true,"context_window":4096,"capabilities":{"vision":false}},{"id":"unknown","tool_calling":"true","vision":"false","context_window":-1}]})");
    };
    ProviderDiscoveryOutcome outcome;
    const auto models =
        fetchOpenAiCompatibleModels(server.endpoint(), 500, {}, nullptr, {}, &outcome);
    QVERIFY(outcome.completed);
    QCOMPARE(models.size(), 2);
    QCOMPARE(models[0].capabilities.nativeToolCalling, CapabilitySupport::Supported);
    QCOMPARE(models[0].capabilities.structuredOutput, CapabilitySupport::Unsupported);
    QCOMPARE(models[0].capabilities.visionInput, CapabilitySupport::Unsupported);
    QCOMPARE(models[0].capabilities.contextWindow.value_or(0), 4096);
    QCOMPARE(models[1].capabilities.nativeToolCalling, CapabilitySupport::Unknown);
    QCOMPARE(models[1].capabilities.visionInput, CapabilitySupport::Unknown);
    QVERIFY(!models[1].capabilities.contextWindow);
}
void ProviderContractsTest::structuredOutputContract_data() {
    QTest::addColumn<QString>("content");
    QTest::addColumn<bool>("ok");
    QTest::newRow("object") << QString(R"({"answer":42})") << true;
    QTest::newRow("array") << QString("[]") << false;
    QTest::newRow("invalid") << QString("{broken") << false;
}
void ProviderContractsTest::structuredOutputContract() {
    QFETCH(QString, content);
    QFETCH(bool, ok);
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [=](QTcpSocket* socket, const auto&) {
        const QJsonObject body{
            {"choices", QJsonArray{QJsonObject{{"message", QJsonObject{{"content", content}}}}}}};
        ProviderHttpFixture::reply(socket, QJsonDocument(body).toJson(QJsonDocument::Compact));
    };
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    service.setLmStudioEndpoint(server.endpoint().toString());
    ModelBinding binding{"lm-studio", "fixture"};
    binding.capabilities.structuredOutput = CapabilitySupport::Supported;
    auto provider = service.constructProvider(binding);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.structuredOutput = true;
    options.structuredSchemaName = "answer";
    options.structuredSchema = {
        {"type", "object"},
        {"properties", QJsonObject{{"answer", QJsonObject{{"type", "integer"}}}}}};
    const auto reply = provider->sendRequest("synthetic structured request", options);
    QCOMPARE(reply.success, ok);
    QCOMPARE(reply.structuredResult.has_value(), ok);
    const auto format = server.requests.first().body.value("response_format").toObject();
    QCOMPARE(format.value("json_schema").toObject().value("schema").toObject(),
             options.structuredSchema);
    if (!ok)
        QCOMPARE(reply.category, ChatProviderErrorCategory::MalformedResponse);
}
void ProviderContractsTest::ollamaRejectsNativeCapabilities() {
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    auto provider = service.constructProvider({"ollama", "fixture"});
    for (bool native : {true, false}) {
        ChatRequestOptions options;
        options.nativeToolCalling = native;
        options.structuredOutput = !native;
        const auto reply = provider->sendRequest("unsupported", options);
        QVERIFY(!reply.success);
        QCOMPARE(reply.category, ChatProviderErrorCategory::CapabilityUnsupported);
    }
}

void ProviderContractsTest::publicErrorsAreAllowlisted_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("category");
    const QByteArray secret =
        "Authorization: Bearer PRIVATE_BEARER api-key=PRIVATE_KEY prompt=PRIVATE_PAYLOAD";
    for (int status : {401, 403, 404, 408, 429, 500, 501}) {
        const auto category = ProviderRequestRuntime::classify(status, QNetworkReply::NoError);
        QTest::newRow(qPrintable(QString::number(status)))
            << status
            << QJsonDocument(
                   QJsonObject{{"error", QJsonObject{{"message", QString::fromUtf8(secret)}}}})
                   .toJson()
            << int(category);
    }
    QTest::newRow("malformed") << 500 << QByteArray("{broken PRIVATE_PAYLOAD")
                               << int(ChatProviderErrorCategory::ProviderUnavailable);
    QTest::newRow("oversized") << 401
                               << QJsonDocument(
                                      QJsonObject{{"error", QString::fromUtf8(
                                                                secret + QByteArray(100000, 'x'))}})
                                      .toJson()
                               << int(ChatProviderErrorCategory::AuthenticationRequired);
    QTest::newRow("context")
        << 422 << QByteArray(R"({"error":{"message":"maximum context length PRIVATE_PAYLOAD"}})")
        << int(ChatProviderErrorCategory::RequestRejected);
}

void ProviderContractsTest::publicErrorsAreAllowlisted() {
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(int, category);
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [=](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(socket, body, status);
    };
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    service.setLmStudioEndpoint(server.endpoint().toString());
    ScopedProviderLogs logs;
    for (bool native : {false, true}) {
        auto binding = nativeBinding("lm-studio");
        auto provider = service.constructProvider(binding);
        ChatRequestOptions options;
        options.nativeToolCalling = native;
        options.deadlineMs = 500;
        const auto reply = provider->sendRequest("PRIVATE_PAYLOAD", options);
        QVERIFY(!reply.success);
        QVERIFY(reply.lifecycle != ChatRequestLifecycle::Completed);
        // Native 501 retains the existing capability rejection contract (equal to the
        // classified status). HTTP 408/429 now surface their transport categories
        // (Timeout/RateLimited) instead of collapsing to MalformedResponse solely
        // because Qt reports UnknownContentError for those statuses.
        QCOMPARE(int(reply.category), category);
        QVERIFY(reply.errorMessage.size() < 512);
        QVERIFY(!reply.errorMessage.contains("PRIVATE_"));
        QVERIFY(!QJsonDocument(reply.diagnostics).toJson().contains("PRIVATE_"));
        QCOMPARE(reply.httpStatus, status);
        if (status == 422 && native)
            QCOMPARE(reply.diagnostics.value("outcome").toString(), QString("context_exhausted"));
    }
    QCOMPARE(server.requests.size(), 2); // bounded requests are never retried
    QVERIFY(logs.text().contains("http_status"));
    QVERIFY(!logs.text().contains("PRIVATE_"));
}

void ProviderContractsTest::httpFailureClassification_data() {
    QTest::addColumn<int>("mode");      // 0 ordinary, 1 native tool, 2 structured, 3 combined
    QTest::addColumn<QString>("fault"); // empty, disconnect, timeout or cancel
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("category");

    using C = ChatProviderErrorCategory;
    const QByteArray secret =
        R"({"error":{"message":"PRIVATE_PAYLOAD PRIVATE_KEY PRIVATE_BEARER"}})";
    struct HttpRow {
        const char* name;
        int status;
        QByteArray body;
        C category;
    };
    // Qt reports UnknownContentError for unmapped HTTP statuses such as 408/429, so
    // these rows reproduce the transport/status precedence defect directly.
    const QList<HttpRow> rows{
        {"native-408", 408, secret, C::Timeout},
        {"native-429", 429, secret, C::RateLimited},
        {"408-malformed-body", 408, QByteArray("{broken PRIVATE_PAYLOAD"), C::Timeout},
        {"429-malformed-body", 429, QByteArray("{broken PRIVATE_PAYLOAD"), C::RateLimited},
        {"401", 401, secret, C::AuthenticationRequired},
        {"403", 403, secret, C::AuthenticationRequired},
        {"404", 404, secret, C::ModelNotFound},
        {"500", 500, secret, C::ProviderUnavailable},
        {"501", 501, secret, C::CapabilityUnsupported},
        {"200-malformed", 200, QByteArray("{broken PRIVATE_PAYLOAD"), C::MalformedResponse},
    };
    struct FaultRow {
        const char* name;
        const char* fault;
        C category;
    };
    const QList<FaultRow> faults{
        {"disconnect", "disconnect", C::ConnectionFailed},
        {"timeout", "timeout", C::Timeout},
        {"cancel", "cancel", C::Cancelled},
    };
    for (int mode = 0; mode < 4; ++mode) {
        for (const auto& row : rows)
            QTest::newRow(qPrintable(QString("mode%1-%2").arg(mode).arg(row.name)))
                << mode << QString() << row.status << row.body << int(row.category);
        for (const auto& fault : faults)
            QTest::newRow(qPrintable(QString("mode%1-%2").arg(mode).arg(fault.name)))
                << mode << QString(fault.fault) << 0 << QByteArray() << int(fault.category);
    }
}

void ProviderContractsTest::httpFailureClassification() {
    QFETCH(int, mode);
    QFETCH(QString, fault);
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(int, category);
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    auto token = std::make_shared<std::atomic_bool>(false);
    server.respond = [&](QTcpSocket* socket, const auto&) {
        if (fault == "cancel")
            token->store(true);
        else if (fault == "disconnect")
            socket->abort();
        else if (fault == "timeout")
            return; // Retain the connection until the request deadline fires.
        else
            ProviderHttpFixture::reply(socket, body, status);
    };
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    service.setLmStudioEndpoint(server.endpoint().toString());
    auto binding = nativeBinding("lm-studio");
    binding.capabilities.structuredOutput = CapabilitySupport::Supported;
    binding.capabilities.combinedToolsAndStructuredOutput = CapabilitySupport::Supported;
    auto provider = service.constructProvider(binding);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = mode & 1;
    options.structuredOutput = mode & 2;
    options.structuredSchemaName = "answer";
    options.structuredSchema = {
        {"type", "object"},
        {"properties", QJsonObject{{"answer", QJsonObject{{"type", "integer"}}}}}};
    options.tools = BuiltInToolProvider::descriptors();
    options.cancellationToken = token;
    ScopedProviderLogs logs;
    const auto reply = provider->sendRequest("PRIVATE_PAYLOAD synthetic request", options);
    QCOMPARE(server.requests.size(), 1); // bounded requests are never retried
    QVERIFY(!reply.success);
    QVERIFY(reply.lifecycle != ChatRequestLifecycle::Completed);
    QCOMPARE(int(reply.category), category);
    QCOMPARE(reply.attempts, 1);
    if (fault.isEmpty())
        QCOMPARE(reply.httpStatus, status);
    QVERIFY(reply.errorMessage.size() < 512);
    QVERIFY(!reply.errorMessage.contains("PRIVATE_"));
    QVERIFY(!QJsonDocument(reply.diagnostics).toJson().contains("PRIVATE_"));
    QVERIFY(!logs.text().contains("PRIVATE_"));
}

void ProviderContractsTest::streamingErrorsNeverEmitPayload_data() {
    QTest::addColumn<bool>("ollama");
    QTest::addColumn<int>("status");
    for (bool ollama : {false, true})
        for (int status : {200, 401, 403, 429, 500})
            QTest::newRow(qPrintable(QString("%1-%2").arg(ollama ? "ollama" : "sse").arg(status)))
                << ollama << status;
}

void ProviderContractsTest::streamingErrorsNeverEmitPayload() {
    QFETCH(bool, ollama);
    QFETCH(int, status);
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [=](QTcpSocket* socket, const auto&) {
        const QByteArray error = status == 200 ? R"("error":{"message":"PRIVATE_ERROR"},)" : "";
        const QByteArray body =
            ollama
                ? "{" + error +
                      R"("response":"PRIVATE_PAYLOAD","done":true})"
                      "\n"
                : "data: {" + error +
                      R"("choices":[{"delta":{"content":"PRIVATE_PAYLOAD"},"finish_reason":"stop"}]})"
                      "\n\ndata: [DONE]\n\n";
        ProviderHttpFixture::reply(socket, body, status,
                                   ollama ? "application/x-ndjson" : "text/event-stream");
    };
    LocalInferenceRequest request;
    request.prompt = "synthetic prompt";
    request.options.model = "fixture";
    request.options.timeoutMs = 500;
    QString deltas;
    const auto onChunk = [&](const LocalInferenceStreamChunk& chunk) { deltas += chunk.text; };
    LocalInferenceStreamResult result;
    if (ollama) {
        auto config = OllamaConfig::fromEndpoint(server.endpoint().toString());
        config.modelDiscoveryEnabled = false;
        result = OllamaLocalInferenceStreamClient(config, 500).startStream(request, onChunk);
    } else {
        LMStudioConfig config;
        config.endpoint = server.endpoint();
        result = LMStudioLocalInferenceStreamClient(config, 500).startStream(request, onChunk);
    }
    QVERIFY(result.status != LocalInferenceStreamStatus::Completed);
    QVERIFY(deltas.isEmpty());
    QVERIFY(result.accumulatedText.isEmpty());
    QVERIFY(!result.summary.contains("PRIVATE_"));
    const auto expected = status == 200
                              ? ChatProviderErrorCategory::MalformedResponse
                              : ProviderRequestRuntime::classify(status, QNetworkReply::NoError);
    QCOMPARE(result.providerErrorCategory, int(expected));
    if (!ollama) {
        AppSettings settings(std::make_unique<InMemorySettingsStore>(),
                             inMemoryTestCredentialStore());
        settings.setNetworkMode("LocalOnly");
        ModelService service(&settings);
        service.setLmStudioEndpoint(server.endpoint().toString());
        auto binding = nativeBinding("lm-studio");
        binding.capabilities.streaming = CapabilitySupport::Supported;
        auto provider = service.constructProvider(binding);
        const auto reply = provider->sendMessageStreaming(
            "synthetic", [&](const QString& delta) { deltas += delta; }, {});
        QVERIFY(!reply.success);
        QCOMPARE(reply.category, expected);
        QVERIFY(deltas.isEmpty());
        QVERIFY(!reply.errorMessage.contains("PRIVATE_"));
    }
}

void ProviderContractsTest::unknownClassifierMessageIsSafe() {
    const auto error =
        ProviderErrorClassifier::classify(0, "PRIVATE_KEY PRIVATE_PAYLOAD", "fixture");
    QCOMPARE(error.category, ProviderErrorCategory::Unknown);
    QVERIFY(!error.retryable);
    QVERIFY(!ProviderErrorClassifier::userMessage(error).contains("PRIVATE_"));
}

void ProviderContractsTest::visionCapabilityMatrix_data() {
    QTest::addColumn<QString>("id");
    QTest::addColumn<int>("vision");
    QTest::addColumn<int>("images");
    QTest::addColumn<int>("mode");
    for (const auto& id : QStringList{"lm-studio", "llama-cpp-server"})
        for (auto vision : {CapabilitySupport::Unsupported, CapabilitySupport::Supported,
                            CapabilitySupport::Unknown})
            for (int images : {0, 1, 3})
                for (int mode = 0; mode < 4; ++mode)
                    QTest::newRow(qPrintable(QString("%1-vision%2-images%3-mode%4")
                                                 .arg(id)
                                                 .arg(int(vision))
                                                 .arg(images)
                                                 .arg(mode)))
                        << id << int(vision) << images << mode;
}

void ProviderContractsTest::visionCapabilityMatrix() {
    QFETCH(QString, id);
    QFETCH(int, vision);
    QFETCH(int, images);
    QFETCH(int, mode);
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(socket,
                                   R"({"choices":[{"message":{"content":"{\"answer\":42}"}}]})");
    };
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    endpoint(service, id, server.endpoint());
    auto binding = nativeBinding(id);
    binding.capabilities.visionInput = static_cast<CapabilitySupport>(vision);
    binding.capabilities.structuredOutput = CapabilitySupport::Supported;
    binding.capabilities.combinedToolsAndStructuredOutput = CapabilitySupport::Supported;
    auto provider = service.constructProvider(binding);
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.nativeToolCalling = mode & 1;
    options.structuredOutput = mode & 2;
    options.structuredSchemaName = "answer";
    options.structuredSchema = {{"type", "object"}};
    for (int i = 0; i < images; ++i)
        options.images.append(
            ChatImage{"image/png", QByteArray("PRIVATE_IMAGE_") + QByteArray::number(i)});
    ScopedProviderLogs logs;
    const auto reply = provider->sendRequest("synthetic vision request", options);
    const bool rejected =
        images > 0 && binding.capabilities.visionInput == CapabilitySupport::Unsupported;
    QCOMPARE(server.requests.size(), rejected ? 0 : 1);
    QCOMPARE(reply.success, !rejected);
    QVERIFY(!reply.errorMessage.contains("PRIVATE_IMAGE"));
    QVERIFY(!QJsonDocument(reply.diagnostics).toJson().contains("PRIVATE_IMAGE"));
    QVERIFY(!logs.text().contains("PRIVATE_IMAGE"));
    QCOMPARE(int(binding.capabilities.visionInput), vision);
    // A successful synthetic request must not promote unknown metadata to Supported.
    QCOMPARE(service.capabilities(id, "fixture").visionInput, CapabilitySupport::Unknown);
    if (rejected) {
        QCOMPARE(reply.category, ChatProviderErrorCategory::CapabilityUnsupported);
        QCOMPARE(reply.error, ChatProviderReply::Error::CapabilityRejected);
        QCOMPARE(reply.lifecycle, ChatRequestLifecycle::Failed);
    } else {
        const auto content = server.requests.first()
                                 .body.value("messages")
                                 .toArray()
                                 .first()
                                 .toObject()
                                 .value("content");
        if (!images) {
            QCOMPARE(content.toString(), QString("synthetic vision request"));
        } else {
            const auto parts = content.toArray();
            QCOMPARE(parts.size(), images + 1);
            for (int i = 0; i < images; ++i)
                QVERIFY(parts[i + 1]
                            .toObject()
                            .value("image_url")
                            .toObject()
                            .value("url")
                            .toString()
                            .endsWith(QString::fromLatin1(options.images[i].bytes.toBase64())));
        }
        QCOMPARE(reply.category, ChatProviderErrorCategory::None);
    }
}

void ProviderContractsTest::visionGuardUsesFrozenBinding() {
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(socket,
                                   R"({"choices":[{"message":{"content":"fixture answer"}}]})");
    };
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    service.setLmStudioEndpoint(server.endpoint().toString());
    auto binding = nativeBinding("lm-studio");
    binding.capabilities.visionInput = CapabilitySupport::Unsupported;
    auto rejectedProvider = service.constructProvider(binding);
    binding.capabilities.visionInput = CapabilitySupport::Supported;
    auto supportedProvider = service.constructProvider(binding);
    binding.capabilities.visionInput = CapabilitySupport::Unsupported;
    binding.modelId = "mutated-caller";
    service.setSelectedModel({"lm-studio", "future-model"});
    ChatRequestOptions options;
    options.deadlineMs = 500;
    options.images.append(ChatImage{"image/png", "PRIVATE_IMAGE_FROZEN"});
    const auto rejected = rejectedProvider->sendRequest("synthetic", options);
    QCOMPARE(server.requests.size(), 0);
    QCOMPARE(rejected.category, ChatProviderErrorCategory::CapabilityUnsupported);
    QVERIFY(!rejected.success);
    const auto supported = supportedProvider->sendRequest("synthetic", options);
    QVERIFY(supported.success);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().body.value("model").toString(), QString("fixture"));
}

void ProviderContractsTest::malformedDiscoveryPreservesCatalogSequencing() {
    ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QByteArray body = R"({"data":"PRIVATE_DISCOVERY"})";
    server.respond = [&](QTcpSocket* socket, const auto&) {
        ProviderHttpFixture::reply(socket, body);
    };
    AppSettings settings(std::make_unique<InMemorySettingsStore>(), inMemoryTestCredentialStore());
    settings.setNetworkMode("LocalOnly");
    ModelService service(&settings);
    service.setLmStudioEndpoint(server.endpoint().toString());
    int fallbacks = 0;
    service.registerProvider("ollama", [&](const ModelBinding&) {
        ++fallbacks;
        return std::shared_ptr<IChatProvider>{};
    });
    int cloudFallbacks = 0;
    service.registerProvider("openai", [&](const ModelBinding&) {
        ++cloudFallbacks;
        return std::shared_ptr<IChatProvider>{};
    });
    service.setSelectedModel({"lm-studio", "fixture"});
    ProviderDiscoveryOutcome malformed;
    QString error;
    const auto invalid =
        fetchOpenAiCompatibleModels(server.endpoint(), 500, {}, &error, {}, &malformed);
    service.acceptProviderDiscovery("lm-studio", invalid, malformed,
                                    service.beginProviderHealthObservation());
    QCOMPARE(service.providerStatusSnapshot("lm-studio").catalog, ProviderCatalogState::Failed);
    QCOMPARE(service.providerStatusSnapshot("lm-studio").errorCategory,
             ChatProviderErrorCategory::MalformedResponse);
    QCOMPARE(service.resolve("lm-studio", "fixture").error, ModelBindingError::ProviderUnavailable);
    QVERIFY(!error.contains("PRIVATE_DISCOVERY"));
    const auto older = service.beginProviderHealthObservation();
    body = R"({"data":[{"id":"fixture"}]})";
    ProviderDiscoveryOutcome valid;
    const auto models =
        fetchOpenAiCompatibleModels(server.endpoint(), 500, {}, nullptr, {}, &valid);
    service.acceptProviderDiscovery("lm-studio", models, valid,
                                    service.beginProviderHealthObservation());
    service.acceptProviderDiscovery("lm-studio", invalid, malformed, older);
    QCOMPARE(service.providerStatusSnapshot("lm-studio").catalog, ProviderCatalogState::Available);
    QCOMPARE(service.resolve("lm-studio", "absent").error, ModelBindingError::ModelNotFound);
    service.acceptProviderDiscovery("lm-studio", invalid, malformed,
                                    service.beginProviderHealthObservation());
    const auto stale = service.providerStatusSnapshot("lm-studio");
    QCOMPARE(stale.catalog, ProviderCatalogState::Stale);
    QCOMPARE(stale.health, ProviderHealth::Unknown);
    QCOMPARE(stale.modelIds, QStringList{"fixture"});
    QCOMPARE(stale.errorCategory, ChatProviderErrorCategory::MalformedResponse);
    QVERIFY(!stale.safeDetail.contains("PRIVATE_DISCOVERY"));
    QVERIFY(service.resolve("lm-studio", "fixture").ok());
    body = R"({"data":[]})";
    ProviderDiscoveryOutcome empty;
    const auto noModels =
        fetchOpenAiCompatibleModels(server.endpoint(), 500, {}, nullptr, {}, &empty);
    QVERIFY(empty.completed);
    service.acceptProviderDiscovery("lm-studio", noModels, empty,
                                    service.beginProviderHealthObservation());
    QCOMPARE(service.providerStatusSnapshot("lm-studio").catalog, ProviderCatalogState::Empty);
    QCOMPARE(service.resolve("lm-studio", "fixture").error, ModelBindingError::ModelNotFound);
    QCOMPARE(service.selectedModel().modelId, QString("fixture"));
    QCOMPARE(service.selectedModel().providerId, QString("lm-studio"));
    QCOMPARE(fallbacks, 0);
    QCOMPARE(cloudFallbacks, 0);
    QCOMPARE(server.requests.size(), 3);
}

QTEST_GUILESS_MAIN(ProviderContractsTest)
#include "test_provider_contracts.moc"
