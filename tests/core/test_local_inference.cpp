// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../support/LocalInferenceWorkerFixture.h"
#ifndef Q_MOC_RUN
#include "../support/ProviderHttpFixture.h"
#include "../support/ScriptedProviderTransport.h"
#endif
#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/network/NetworkPolicyService.h"
#include "sentinel/core/provider/SelectedEndpointChatProvider.h"
#include "sentinel/core/runtime/LocalInference.h"

#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

using sentinel::core::LMStudioConfig;
using sentinel::core::LMStudioLocalInferenceClient;
using sentinel::core::LocalInferenceError;
using sentinel::core::LocalInferenceRequest;
using sentinel::core::LocalInferenceStatus;
using sentinel::core::LocalInferenceStreamStatus;
using sentinel::core::NullLocalInferenceClient;
using sentinel::core::NullLocalInferenceStreamClient;
using sentinel::core::OllamaConfig;
using sentinel::core::OllamaLocalInferenceClient;

class LocalInferenceTest final : public QObject {
    Q_OBJECT

private slots:
    void nullClientDeterministicallyRefuses();
    void blankPromptRejectedBeforeOllamaCall();
    void missingModelRejectedBeforeOllamaCall();
    void unavailableModelRejectedBeforeGeneration();
    void invalidEndpointIsBlocked();
    void streamSkeletonIsDeterministicallyDisabled();
    void workerStreamsOrderedChunksOnceAndCleansUp();
    void workerRejectsConcurrentStreamWhileTerminalDeliveryIsQueued();
    void workerCancellationPropagatesToGatedStreamClient();
    void workerCompletesWithFakeClient();
    void workerReportsClientFailure();
    void workerRejectsConcurrentInference();
    void workerPropagatesCancellationToInferenceClient();
    void cloudEndpointWithoutKeyIsBlocked();
    void cloudOpenAiCompatibleRequestCarriesBearerKey();
    void completedReasoningOnlyIsInvalid();
    void cloudEmptyAndTruncatedAnswers_data();
    void cloudEmptyAndTruncatedAnswers();
    void zeroTimeoutCancelsOutstandingRequest();
    void mockGenerationContract_data();
    void mockGenerationContract();
    void mockStreamContract_data();
    void mockStreamContract();
    void mockStreamCancellation_data();
    void mockStreamCancellation();
    void mockCancellationAndDisconnect_data();
    void mockCancellationAndDisconnect();
    void boundedPlainRequest_data();
    void boundedPlainRequest();
};

void LocalInferenceTest::nullClientDeterministicallyRefuses() {
    NullLocalInferenceClient client;

    const auto response = client.infer(LocalInferenceRequest{
        QStringLiteral("request-1"),
        QStringLiteral("hello"),
        {.model = QStringLiteral("llama3.2"), .timeoutMs = 100},
    });

    QCOMPARE(response.status, LocalInferenceStatus::Refused);
    QCOMPARE(response.error, LocalInferenceError::ClientUnavailable);
    QVERIFY(response.summary.contains(QStringLiteral("unavailable")));
    QCOMPARE(response.traces.size(), 1);
}

void LocalInferenceTest::blankPromptRejectedBeforeOllamaCall() {
    OllamaLocalInferenceClient client{
        OllamaConfig::fromEndpoint(QStringLiteral("http://127.0.0.1:11434")), 1};

    const auto response = client.infer(LocalInferenceRequest{
        QStringLiteral("request-1"),
        QStringLiteral("   "),
        {.model = QStringLiteral("llama3.2"), .timeoutMs = 1},
    });

    QCOMPARE(response.status, LocalInferenceStatus::InvalidRequest);
    QCOMPARE(response.error, LocalInferenceError::BlankPrompt);
    QVERIFY(response.summary.contains(QStringLiteral("prompt is blank")));
}

void LocalInferenceTest::missingModelRejectedBeforeOllamaCall() {
    OllamaLocalInferenceClient client{
        OllamaConfig::fromEndpoint(QStringLiteral("http://127.0.0.1:11434")), 1};

    const auto response = client.infer(LocalInferenceRequest{
        QStringLiteral("request-1"),
        QStringLiteral("hello"),
        {.model = QString(), .timeoutMs = 1},
    });

    QCOMPARE(response.status, LocalInferenceStatus::InvalidRequest);
    QCOMPARE(response.error, LocalInferenceError::MissingModel);
    QVERIFY(response.summary.contains(QStringLiteral("model is required")));
}

void LocalInferenceTest::unavailableModelRejectedBeforeGeneration() {
    auto config = OllamaConfig::fromEndpoint(QStringLiteral("http://127.0.0.1:11434"));
    config.modelDiscoveryEnabled = false;
    OllamaLocalInferenceClient client{config, 1};
    sentinel::core::OllamaModelDiscoveryResult discovery;
    discovery.lifecycle = sentinel::core::ChatRequestLifecycle::Completed;
    discovery.models.append({QStringLiteral("installed-model")});

    const auto response = client.infer(LocalInferenceRequest{
        QStringLiteral("request-1"),
        QStringLiteral("hello"),
        {.model = QStringLiteral("__sentinel_missing_model__"),
         .modelValidation = discovery,
         .timeoutMs = 1},
    });

    QCOMPARE(response.status, LocalInferenceStatus::ModelUnavailable);
    QCOMPARE(response.error, LocalInferenceError::ModelUnavailable);
    QVERIFY(response.summary.contains(QStringLiteral("selected model is unavailable")));
}

void LocalInferenceTest::invalidEndpointIsBlocked() {
    OllamaConfig config;
    config.endpoint.url = QUrl(QStringLiteral("https://example.com"));
    config.endpoint.valid = false;
    config.endpoint.normalizedFromInvalid = false;
    OllamaLocalInferenceClient client{config, 1};

    const auto response = client.infer(LocalInferenceRequest{
        QStringLiteral("request-1"),
        QStringLiteral("hello"),
        {.model = QStringLiteral("llama3.2"), .timeoutMs = 1},
    });

    QCOMPARE(response.status, LocalInferenceStatus::Blocked);
    QCOMPARE(response.error, LocalInferenceError::EndpointBlocked);
    QVERIFY(response.summary.contains(QStringLiteral("loopback HTTP")));
}

void LocalInferenceTest::streamSkeletonIsDeterministicallyDisabled() {
    NullLocalInferenceStreamClient client;

    const auto result = client.startStream(
        LocalInferenceRequest{
            QStringLiteral("stream-request-1"),
            QStringLiteral("hello"),
            {.model = QStringLiteral("llama3.2"), .timeoutMs = 1, .streamingRequested = true},
        },
        {});

    QCOMPARE(result.status, LocalInferenceStreamStatus::Disabled);
    QCOMPARE(result.summary,
             QStringLiteral("Local inference streaming is disabled; no stream was opened."));
    QVERIFY(result.chunks.isEmpty());
    QCOMPARE(client.statusSummary(), QStringLiteral("Local inference streaming is disabled."));
    QVERIFY(!client.isAvailable());
}

void LocalInferenceTest::workerStreamsOrderedChunksOnceAndCleansUp() {
    using namespace sentinel::test;
    auto state = std::make_shared<StreamClientState>();
    state->chunks = {{1, QStringLiteral("hello "), false, false, {}},
                     {2, QStringLiteral("world"), true, false, {}}};
    state->result.status = LocalInferenceStreamStatus::Completed;
    state->result.accumulatedText = QStringLiteral("hello world");
    QObject context;
    sentinel::core::LocalInferenceWorker worker(
        nullptr, std::make_unique<GatedInferenceStreamClient>(state), &context, true, true);
    QStringList received;
    int finalCallbacks = 0;
    QString finalId;
    LocalInferenceStreamStatus finalStatus = LocalInferenceStreamStatus::NotStarted;
    LocalInferenceRequest request;
    request.id = QStringLiteral("stream-ordered-1");
    request.prompt = QStringLiteral("test prompt");
    request.options.model = QStringLiteral("test-model");

    QVERIFY(worker.startStream(
        request,
        [&received](const QString&, const sentinel::core::LocalInferenceStreamChunk& chunk) {
            received.append(chunk.text);
        },
        [&finalCallbacks, &finalId, &finalStatus](
            const QString& id, const sentinel::core::LocalInferenceStreamResult& result) {
            ++finalCallbacks;
            finalId = id;
            finalStatus = result.status;
        }));
    QTRY_VERIFY_WITH_TIMEOUT(state->entered.load(), 3000);
    state->entryRelease.release();
    state->chunkRelease.release(2);
    state->finalRelease.release();
    QTRY_COMPARE_WITH_TIMEOUT(finalCallbacks, 1, 3000);
    QCOMPARE(state->calls.load(), 1);
    QCOMPARE(state->requestId, request.id);
    QCOMPARE(state->chunkCallbacks.load(), 2);
    QCOMPARE(received, QStringList({QStringLiteral("hello "), QStringLiteral("world")}));
    QCOMPARE(finalId, request.id);
    QCOMPARE(finalStatus, LocalInferenceStreamStatus::Completed);
}

void LocalInferenceTest::workerRejectsConcurrentStreamWhileTerminalDeliveryIsQueued() {
    using namespace sentinel::test;
    auto state = std::make_shared<StreamClientState>();
    state->result.status = LocalInferenceStreamStatus::Completed;
    QObject context;
    sentinel::core::LocalInferenceWorker worker(
        nullptr, std::make_unique<GatedInferenceStreamClient>(state), &context, true, true);
    LocalInferenceRequest request;
    request.id = QStringLiteral("stream-gated-1");
    request.prompt = QStringLiteral("test prompt");
    request.options.model = QStringLiteral("test-model");
    int firstFinalCallbacks = 0;
    QVERIFY(worker.startStream(
        request, {},
        [&firstFinalCallbacks](const QString&, const sentinel::core::LocalInferenceStreamResult&) {
            ++firstFinalCallbacks;
        }));
    QTRY_VERIFY_WITH_TIMEOUT(state->entered.load(), 3000);
    state->entryRelease.release();
    state->finalRelease.release();
    // The worker thread may finish before its queued terminal callback runs.
    // It must still reserve the request slot until that callback releases it.
    QVERIFY(!worker.startStream(request, {}, {}));
    QCOMPARE(state->calls.load(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(firstFinalCallbacks, 1, 3000);
    QVERIFY(worker.startStream(request, {}, {}));
    state->entryRelease.release();
    state->finalRelease.release();
}

void LocalInferenceTest::workerCancellationPropagatesToGatedStreamClient() {
    using namespace sentinel::test;
    auto state = std::make_shared<StreamClientState>();
    state->chunks = {{1, QStringLiteral("never delivered"), false, false, {}}};
    state->result.status = LocalInferenceStreamStatus::Completed;
    QObject context;
    sentinel::core::LocalInferenceWorker worker(
        nullptr, std::make_unique<GatedInferenceStreamClient>(state), &context, true, true);
    int finalCallbacks = 0;
    LocalInferenceStreamStatus finalStatus = LocalInferenceStreamStatus::NotStarted;
    LocalInferenceRequest request;
    request.id = QStringLiteral("stream-cancel-1");
    request.prompt = QStringLiteral("test prompt");
    request.options.model = QStringLiteral("test-model");
    QVERIFY(worker.startStream(
        request, {},
        [&finalCallbacks, &finalStatus](const QString&,
                                        const sentinel::core::LocalInferenceStreamResult& result) {
            ++finalCallbacks;
            finalStatus = result.status;
        }));
    QTRY_VERIFY_WITH_TIMEOUT(state->entered.load(), 3000);
    worker.cancel(request.id);
    state->entryRelease.release();
    state->chunkRelease.release();
    state->finalRelease.release();
    QTRY_COMPARE_WITH_TIMEOUT(finalCallbacks, 1, 3000);
    QVERIFY(state->cancelled.load());
    QCOMPARE(state->chunkCallbacks.load(), 0);
    QCOMPARE(finalStatus, LocalInferenceStreamStatus::Cancelled);
}

void LocalInferenceTest::workerCompletesWithFakeClient() {
    sentinel::test::LocalInferenceWorkerFixture fixture;
    QVERIFY(fixture.start());
    fixture.state->release.release();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.callbacks, 1, 3000);
    QCOMPARE(fixture.state->calls.load(), 1);
    QCOMPARE(fixture.response.status, LocalInferenceStatus::Succeeded);
    QCOMPARE(fixture.response.text, QStringLiteral("worker result"));
    QCOMPARE(fixture.completedId, QStringLiteral("worker-1"));
    QVERIFY(fixture.state->exited.load());
}

void LocalInferenceTest::workerReportsClientFailure() {
    sentinel::test::LocalInferenceWorkerFixture fixture;
    fixture.state->outcome = LocalInferenceStatus::Error;
    QVERIFY(fixture.start());
    fixture.state->release.release();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.callbacks, 1, 3000);
    QCOMPARE(fixture.response.status, LocalInferenceStatus::Error);
    QCOMPARE(fixture.response.error, LocalInferenceError::ClientUnavailable);
    QVERIFY(fixture.response.text.isEmpty());
}

void LocalInferenceTest::workerRejectsConcurrentInference() {
    sentinel::test::LocalInferenceWorkerFixture fixture;
    QVERIFY(fixture.start());
    QTRY_VERIFY_WITH_TIMEOUT(fixture.state->entered.load(), 3000);
    QVERIFY(!fixture.start(QStringLiteral("duplicate")));
    QCOMPARE(fixture.state->calls.load(), 1);
    fixture.state->release.release();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.callbacks, 1, 3000);
    QVERIFY(fixture.start(QStringLiteral("worker-2")));
    fixture.state->release.release();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.callbacks, 2, 3000);
    QCOMPARE(fixture.completedId, QStringLiteral("worker-2"));
}

void LocalInferenceTest::workerPropagatesCancellationToInferenceClient() {
    sentinel::test::LocalInferenceWorkerFixture fixture;
    QVERIFY(fixture.start());
    QTRY_VERIFY_WITH_TIMEOUT(fixture.state->entered.load(), 3000);
    fixture.worker->cancel(QStringLiteral("worker-1"));
    fixture.state->release.release();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.callbacks, 1, 3000);
    QVERIFY(fixture.state->cancelled.load());
    QCOMPARE(fixture.response.status, LocalInferenceStatus::Succeeded);
}

QTEST_MAIN(LocalInferenceTest)

void LocalInferenceTest::cloudEndpointWithoutKeyIsBlocked() {
    // The fixed HTTPS cloud endpoint requires a credential before dispatch.
    LMStudioConfig config;
    config.endpoint = QUrl(QStringLiteral("https://api.openai.com"));
    config.apiKey = QString();

    LMStudioLocalInferenceClient client(config, 1000);
    LocalInferenceRequest request;
    request.prompt = QStringLiteral("hello");
    request.options.model = QStringLiteral("gpt-4o-mini");

    const auto response = client.infer(request);
    QCOMPARE(response.status, LocalInferenceStatus::Blocked);
    QCOMPARE(response.error, LocalInferenceError::EndpointBlocked);
    QVERIFY(response.summary.contains(QStringLiteral("API key")));
}

void LocalInferenceTest::cloudOpenAiCompatibleRequestCarriesBearerKey() {
    auto state = std::make_shared<sentinel::test::ScriptedTransportState>();
    state->responder = [](const auto&) {
        return sentinel::test::ScriptedHttpResponse::body(
            R"({"choices":[{"message":{"role":"assistant","content":"cloud says hi"}}]})");
    };
    LMStudioConfig config;
    config.endpoint = QUrl(QStringLiteral("https://fixture.invalid"));
    config.apiKey = QStringLiteral("test-key-123");
    config.networkFactory = std::make_shared<sentinel::test::ScriptedNetworkFactory>(state);
    sentinel::test::ScopedOnlineNetworkMode online;
    LMStudioLocalInferenceClient client(config, 5000);
    LocalInferenceRequest request;
    request.prompt = QStringLiteral("hello cloud");
    request.options.model = QStringLiteral("gpt-test");
    const auto response = client.infer(request);
    QCOMPARE(response.status, LocalInferenceStatus::Succeeded);
    QCOMPARE(response.text, QStringLiteral("cloud says hi"));
    QCOMPARE(state->requests.size(), 1);
    QCOMPARE(state->requests.first().headers.value("authorization"),
             QByteArray("Bearer test-key-123"));
    QCOMPARE(state->requests.first().url.path(), QStringLiteral("/v1/chat/completions"));
}

void LocalInferenceTest::cloudEmptyAndTruncatedAnswers_data() {
    QTest::addColumn<QString>("finishReason");
    QTest::newRow("empty-visible-content") << QStringLiteral("stop");
    QTest::newRow("output-limit") << QStringLiteral("length");
}
void LocalInferenceTest::cloudEmptyAndTruncatedAnswers() {
    QFETCH(QString, finishReason);
    auto state = std::make_shared<sentinel::test::ScriptedTransportState>();
    state->responder = [&](const auto&) {
        const QJsonObject body{
            {"choices",
             QJsonArray{QJsonObject{
                 {"finish_reason", finishReason},
                 {"message", QJsonObject{{"content", ""},
                                         {"reasoning_content", "private reasoning fixture"}}}}}},
            {"usage", QJsonObject{{"completion_tokens", 2048}}}};
        return sentinel::test::ScriptedHttpResponse::body(QJsonDocument(body).toJson());
    };
    LMStudioConfig config;
    config.endpoint = QUrl("https://fixture.invalid/v1");
    config.apiKey = "fixture-key";
    config.networkFactory = std::make_shared<sentinel::test::ScriptedNetworkFactory>(state);
    sentinel::test::ScopedOnlineNetworkMode online;
    LocalInferenceRequest request;
    request.prompt = "synthetic planner request";
    request.options.model = "fixture";
    request.options.maxTokens = 2048;
    const auto response = LMStudioLocalInferenceClient(config, 1000).infer(request);
    QCOMPARE(response.status, LocalInferenceStatus::Error);
    QVERIFY(response.text.isEmpty());
    QCOMPARE(response.httpStatus, 200);
    QCOMPARE(response.diagnostics.value("finish_reason").toString(), finishReason);
    QCOMPARE(response.diagnostics.value("completion_tokens").toInt(), 2048);
    QCOMPARE(response.diagnostics.value("output_limit_reached").toBool(), finishReason == "length");
    QVERIFY(!QJsonDocument(response.diagnostics).toJson().contains("private reasoning fixture"));
    if (finishReason == "length") {
        QCOMPARE(response.providerErrorCategory,
                 static_cast<int>(sentinel::core::ChatProviderErrorCategory::RequestRejected));
        QVERIFY(response.summary.contains("output limit"));
    } else
        QVERIFY(response.diagnostics.value("empty_visible_content").toBool());
    QCOMPARE(state->requests.size(), 1);
    state->responder = [&](const auto&) {
        const QJsonObject chunk{
            {"choices",
             QJsonArray{QJsonObject{
                 {"finish_reason", finishReason},
                 {"delta", QJsonObject{{"content", finishReason == "length" ? "partial" : ""},
                                       {"reasoning_content", "private reasoning fixture"}}}}}}};
        return sentinel::test::ScriptedHttpResponse::streaming(
            {"data: " + QJsonDocument(chunk).toJson(QJsonDocument::Compact) + "\n\n",
             "data: [DONE]\n\n"});
    };
    sentinel::core::ModelBinding binding{"cloud-api", "fixture"};
    binding.capabilities.streaming = sentinel::core::CapabilitySupport::Supported;
    const auto provider = makeSelectedEndpointChatProvider(binding, config, 1000);
    sentinel::core::ChatRequestOptions options;
    options.deadlineMs = 1000;
    options.requireBoundedStreaming = true;
    const auto streamed =
        provider->sendMessageStreaming("synthetic planner", [](const auto&) {}, {}, options);
    QVERIFY(!streamed.success);
    QCOMPARE(streamed.category, finishReason == "length"
                                    ? sentinel::core::ChatProviderErrorCategory::RequestRejected
                                    : sentinel::core::ChatProviderErrorCategory::MalformedResponse);
    QCOMPARE(streamed.diagnostics.value("empty_visible_content").toBool(), finishReason == "stop");
    QCOMPARE(streamed.diagnostics.value("output_limit_reached").toBool(), finishReason == "length");
    QVERIFY(!QJsonDocument(streamed.diagnostics).toJson().contains("private reasoning fixture"));
}

void LocalInferenceTest::completedReasoningOnlyIsInvalid() {
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto* socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [socket] {
            socket->readAll();
            const QByteArray body =
                R"({"choices":[{"finish_reason":"stop","message":{"content":"","reasoning_content":"Inspect current state","tool_calls":[]}}],"usage":{"completion_tokens_details":{"reasoning_tokens":12}}})";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    LMStudioConfig config;
    config.endpoint = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    LMStudioLocalInferenceClient client(config);
    LocalInferenceRequest request;
    request.prompt = QStringLiteral("Classify");
    request.options.model = QStringLiteral("fixture");
    const auto response = client.infer(request);
    QCOMPARE(response.status, LocalInferenceStatus::Error);
    QCOMPARE(response.error, LocalInferenceError::InvalidResponse);
    QVERIFY(response.text.isEmpty());
}

void LocalInferenceTest::zeroTimeoutCancelsOutstandingRequest() {
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    auto token = std::make_shared<std::atomic_bool>(false);
    bool received = false;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto* socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            socket->readAll();
            received = true;
            QTimer::singleShot(50, &server, [token] { token->store(true); });
        });
    });
    // Test watchdog, not a provider or production timeout.
    QTimer::singleShot(3000, &server, [token] { token->store(true); });
    LMStudioConfig config;
    config.endpoint = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    LMStudioLocalInferenceClient client(config, 0);
    LocalInferenceRequest request;
    request.prompt = QStringLiteral("Classify");
    request.options.model = QStringLiteral("fixture");
    request.options.timeoutMs = 0;
    request.options.cancellationToken = token;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto response = client.infer(request);
    QVERIFY(received);
    QVERIFY(elapsed.elapsed() < 1000);
    QCOMPARE(response.providerErrorCategory,
             static_cast<int>(sentinel::core::ChatProviderErrorCategory::Cancelled));
    QVERIFY(response.text.isEmpty());
}

void LocalInferenceTest::boundedPlainRequest_data() {
    QTest::addColumn<bool>("stall");
    QTest::newRow("stall") << true;
    QTest::newRow("length") << false;
}

void LocalInferenceTest::boundedPlainRequest() {
    QFETCH(bool, stall);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QByteArray captured;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto* socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            captured += socket->readAll();
            if (stall)
                return;
            const QByteArray body =
                R"({"choices":[{"finish_reason":"length","message":{"content":"{\"requirements\":[]}"}}],"usage":{"completion_tokens":7}})";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    LMStudioConfig config;
    config.endpoint = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    LMStudioLocalInferenceClient client(config);
    LocalInferenceRequest request;
    request.prompt = QStringLiteral("Classify synthetic goal");
    request.options.model = QStringLiteral("fixture");
    request.options.maxTokens = 512;
    request.options.timeoutMs = 100;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto response = client.infer(request);
    QVERIFY(captured.contains("\"max_tokens\":512"));
    QVERIFY(elapsed.elapsed() < 1500);
    QCOMPARE(response.status, LocalInferenceStatus::Error);
    QCOMPARE(response.attempts, 1);
    QCOMPARE(response.providerErrorCategory,
             static_cast<int>(stall ? sentinel::core::ChatProviderErrorCategory::Timeout
                                    : sentinel::core::ChatProviderErrorCategory::RequestRejected));
    if (!stall) {
        QCOMPARE(response.httpStatus, 200);
        QCOMPARE(response.diagnostics.value("finish_reason").toString(), QStringLiteral("length"));
        QCOMPARE(response.diagnostics.value("completion_tokens").toInt(), 7);
    }
    QVERIFY(response.text.isEmpty());
}

void LocalInferenceTest::mockGenerationContract_data() {
    QTest::addColumn<bool>("ollama");
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("category");
    using C = sentinel::core::ChatProviderErrorCategory;
    for (bool ollama : {true, false}) {
        const auto prefix = ollama ? QByteArray("ollama-") : QByteArray("compatible-");
        QTest::newRow((prefix + "success").constData())
            << ollama << 200
            << (ollama ? QByteArray(R"({"response":"fixture answer","done":true})")
                       : QByteArray(R"({"choices":[{"message":{"content":"fixture answer"}}]})"))
            << int(C::None);
        for (const auto& row : QList<QPair<int, C>>{{401, C::AuthenticationRequired},
                                                    {403, C::AuthenticationRequired},
                                                    {404, C::ModelNotFound},
                                                    {429, C::RateLimited},
                                                    {500, C::ProviderUnavailable},
                                                    {501, C::CapabilityUnsupported},
                                                    {422, C::RequestRejected}})
            QTest::newRow((prefix + QByteArray::number(row.first)).constData())
                << ollama << row.first << QByteArray(R"({"error":{"message":"fixture rejection"}})")
                << int(row.second);
        QTest::newRow((prefix + "malformed-json").constData())
            << ollama << 200 << QByteArray("{broken") << int(C::MalformedResponse);
    }
}

void LocalInferenceTest::mockGenerationContract() {
    QFETCH(bool, ollama);
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(int, category);
    using namespace sentinel::core;
    sentinel::test::ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [=](QTcpSocket* socket, const auto&) {
        sentinel::test::ProviderHttpFixture::reply(socket, body, status);
    };
    LocalInferenceRequest request;
    request.prompt = "synthetic prompt";
    request.options.model = "fixture";
    request.options.timeoutMs = 500;
    LocalInferenceResponse response;
    if (ollama) {
        auto config = OllamaConfig::fromEndpoint(server.endpoint().toString());
        config.modelDiscoveryEnabled = false;
        response = OllamaLocalInferenceClient(config, 500).infer(request);
    } else {
        LMStudioConfig config;
        config.endpoint = server.endpoint();
        config.apiKey = "synthetic-key-never-personal";
        response = LMStudioLocalInferenceClient(config, 500).infer(request);
    }
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().target,
             ollama ? QByteArray("/api/generate") : QByteArray("/v1/chat/completions"));
    QCOMPARE(server.requests.first().body.value("model").toString(), QString("fixture"));
    QCOMPARE(response.httpStatus, status);
    QCOMPARE(response.providerErrorCategory, category);
    QCOMPARE(response.attempts, 1);
    QVERIFY(!response.requestId.isEmpty());
    if (category == int(ChatProviderErrorCategory::None)) {
        QCOMPARE(response.status, LocalInferenceStatus::Succeeded);
        QCOMPARE(response.text, QString("fixture answer"));
    } else {
        QVERIFY(response.status != LocalInferenceStatus::Succeeded);
        QVERIFY(response.text.isEmpty());
    }
    QVERIFY(!response.summary.contains("synthetic-key-never-personal"));
    QVERIFY(!QJsonDocument(response.diagnostics).toJson().contains("synthetic-key-never-personal"));
}

void LocalInferenceTest::mockStreamContract_data() {
    QTest::addColumn<bool>("ollama");
    QTest::addColumn<bool>("terminal");
    for (bool ollama : {true, false})
        for (bool terminal : {true, false})
            QTest::newRow(qPrintable(QString("%1-%2").arg(ollama ? "ollama" : "compatible",
                                                          terminal ? "complete" : "truncated")))
                << ollama << terminal;
}

void LocalInferenceTest::mockStreamContract() {
    QFETCH(bool, ollama);
    QFETCH(bool, terminal);
    using namespace sentinel::core;
    sentinel::test::ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    server.respond = [=](QTcpSocket* socket, const auto&) {
        QByteArray body =
            ollama
                ? QByteArray("{\"response\":\"hello \"}\n{\"response\":\"world\"}\n")
                : QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"hello \"}}]}\n\ndata: "
                             "{\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\n");
        if (terminal)
            body += ollama ? QByteArray("{\"done\":true}\n") : QByteArray("data: [DONE]\n\n");
        sentinel::test::ProviderHttpFixture::reply(
            socket, body, 200, ollama ? "application/x-ndjson" : "text/event-stream");
    };
    LocalInferenceRequest request;
    request.prompt = "synthetic stream";
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
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(server.requests.first().body.value("stream").toBool());
    QCOMPARE(deltas, QString("hello world"));
    QCOMPARE(result.status == LocalInferenceStreamStatus::Completed, terminal);
    if (terminal)
        QCOMPARE(result.accumulatedText, deltas);
    else
        QCOMPARE(result.providerErrorCategory, int(ChatProviderErrorCategory::MalformedResponse));
}

void LocalInferenceTest::mockCancellationAndDisconnect_data() {
    QTest::addColumn<bool>("ollama");
    QTest::addColumn<QString>("fault");
    for (bool ollama : {true, false})
        for (const auto& fault : QStringList{"cancel", "disconnect", "timeout"})
            QTest::newRow(qPrintable(QString("%1-%2").arg(ollama ? "ollama" : "compatible", fault)))
                << ollama << fault;
}

void LocalInferenceTest::mockCancellationAndDisconnect() {
    QFETCH(bool, ollama);
    QFETCH(QString, fault);
    using namespace sentinel::core;
    sentinel::test::ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    auto token = std::make_shared<std::atomic_bool>(false);
    server.respond = [&](QTcpSocket* socket, const auto&) {
        if (fault == "cancel")
            token->store(true); // Received request is the gate, not a scheduling sleep.
        else if (fault == "disconnect")
            socket->abort();
        // Timeout deliberately retains the connection without responding.
    };
    LocalInferenceRequest request;
    request.prompt = "synthetic interruption";
    request.options.model = "fixture";
    request.options.timeoutMs = 100;
    request.options.cancellationToken = token;
    LocalInferenceResponse response;
    if (ollama) {
        auto config = OllamaConfig::fromEndpoint(server.endpoint().toString());
        config.modelDiscoveryEnabled = false;
        response = OllamaLocalInferenceClient(config, 100).infer(request);
    } else {
        LMStudioConfig config;
        config.endpoint = server.endpoint();
        response = LMStudioLocalInferenceClient(config, 100).infer(request);
    }
    QVERIFY(!server.requests.isEmpty());
    QVERIFY(response.status != LocalInferenceStatus::Succeeded);
    QVERIFY(response.text.isEmpty());
    const auto expected = fault == "cancel"    ? ChatProviderErrorCategory::Cancelled
                          : fault == "timeout" ? ChatProviderErrorCategory::Timeout
                                               : ChatProviderErrorCategory::ConnectionFailed;
    QCOMPARE(response.providerErrorCategory, int(expected));
    QCOMPARE(response.attempts, 1);
}

void LocalInferenceTest::mockStreamCancellation_data() {
    QTest::addColumn<bool>("ollama");
    QTest::newRow("ollama") << true;
    QTest::newRow("compatible") << false;
}
void LocalInferenceTest::mockStreamCancellation() {
    QFETCH(bool, ollama);
    using namespace sentinel::core;
    sentinel::test::ProviderHttpFixture server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    auto token = std::make_shared<std::atomic_bool>(false);
    server.respond = [=](QTcpSocket* socket, const auto&) {
        // Partial body with no terminal marker, held open until cancellation.
        socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
        socket->write(
            ollama ? QByteArray("{\"response\":\"first\"}\n")
                   : QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"first\"}}]}\n\n"));
    };
    LocalInferenceRequest request;
    request.prompt = "synthetic stream cancellation";
    request.options.model = "fixture";
    request.options.timeoutMs = 500;
    request.options.cancellationToken = token;
    QString text;
    const auto onChunk = [&](const LocalInferenceStreamChunk& chunk) {
        text += chunk.text;
        if (!chunk.text.isEmpty())
            token->store(true);
    };
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
    QCOMPARE(text, QString("first"));
    QCOMPARE(result.status, LocalInferenceStreamStatus::Cancelled);
    QCOMPARE(result.providerErrorCategory, int(ChatProviderErrorCategory::Cancelled));
    QCOMPARE(server.requests.size(), 1);
    QCoreApplication::processEvents();
    QCOMPARE(text, QString("first"));
}

#include "test_local_inference.moc"
