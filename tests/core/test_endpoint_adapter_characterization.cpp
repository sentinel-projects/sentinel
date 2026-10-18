#ifndef Q_MOC_RUN
#include "../support/ScriptedProviderTransport.h"
#endif
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/security/ProviderCredentials.h"
#include <QtTest>

using namespace sentinel::core;
using namespace sentinel::test;

class EndpointAdapterCharacterizationTest final : public QObject {
    Q_OBJECT
private slots:
    void adapterStreamingOutcome_data() {
        QTest::addColumn<QString>("outcome");
        QTest::newRow("fragmented-success") << QString("success");
        QTest::newRow("cancel-before-dispatch") << QString("cancel");
        QTest::newRow("deadline") << QString("timeout");
        QTest::newRow("malformed-stream") << QString("malformed");
    }

    void adapterStreamingOutcome() {
        QFETCH(QString, outcome);
        AppSettings settings(std::make_unique<InMemorySettingsStore>(),
                             inMemoryTestCredentialStore());
        ModelService service(&settings);
        auto state = std::make_shared<ScriptedTransportState>();
        state->responder = [outcome](const RecordedProviderRequest&) {
            if (outcome == "timeout") {
                return ScriptedHttpResponse::streaming({}, false);
            }
            if (outcome == "malformed") {
                return ScriptedHttpResponse::streaming({"data: not-json\n\n"});
            }
            return ScriptedHttpResponse::streaming(
                {"data: {\"choices\":[{\"delta\":{\"content\":\"frag",
                 "mented\"}}]}\n\ndata: {\"choices\":[{\"delta\":{\"content\":\" answer\"}}]}\n\n",
                 "data: [DONE]\n\n"});
        };
        service.setProviderNetworkFactoryForTesting(
            std::make_shared<ScriptedNetworkFactory>(state));
        ModelBinding binding{"lm-studio", "fixture-model"};
        binding.capabilities.streaming = CapabilitySupport::Supported;
        const auto client = service.constructProvider(binding);
        QVERIFY(client);
        QVERIFY(client->supportsBoundedStreaming());
        ChatRequestOptions options;
        options.requireBoundedStreaming = true;
        options.deadlineMs = 100;
        auto token = std::make_shared<std::atomic_bool>(outcome == "cancel");
        QString deltas;
        const auto reply = client->sendMessageStreaming(
            "fixture prompt", [&](const QString& delta) { deltas += delta; }, token, options);
        if (outcome == "success") {
            QVERIFY2(reply.success, qPrintable(reply.errorMessage));
            QCOMPARE(reply.message, QString("fragmented answer"));
            QCOMPARE(deltas, reply.message);
            QCOMPARE(reply.lifecycle, ChatRequestLifecycle::Completed);
            QCOMPARE(reply.diagnostics.value("bounded_streaming").toBool(), true);
        } else {
            QVERIFY(!reply.success);
            QVERIFY(deltas.isEmpty());
            QCOMPARE(reply.category, outcome == "cancel" ? ChatProviderErrorCategory::Cancelled
                                     : outcome == "timeout"
                                         ? ChatProviderErrorCategory::Timeout
                                         : ChatProviderErrorCategory::MalformedResponse);
        }
        QCOMPARE(state->requests.size(), outcome == "cancel" ? 0 : 1);
    }

    void customFactoryOverrideKeepsCapturedBinding() {
        class FixtureProvider final : public IChatProvider {
        public:
            explicit FixtureProvider(QString model) : model_(std::move(model)) {}
            QString name() const override {
                return "fixture-override";
            }
            ChatProviderStatus status() const override {
                return ChatProviderStatus::Ready;
            }
            ChatProviderReply sendMessage(const QString&) override {
                return {true, model_, {}};
            }

        private:
            QString model_;
        };
        ModelService service;
        int constructions = 0;
        service.registerProvider(" LM-STUDIO ", [&](const ModelBinding& binding) {
            ++constructions;
            return std::make_shared<FixtureProvider>(binding.modelId);
        });
        const auto original = service.resolve("lm-studio", "captured-model");
        QVERIFY(original.ok());
        service.setSelectedModel({"openai", "different-model"});
        service.setLmStudioEndpoint("http://127.0.0.1:19999");
        QCOMPARE(original.provider->name(), QString("fixture-override"));
        QCOMPARE(original.provider->sendMessage("fixture").message, QString("captured-model"));
        QCOMPARE(original.binding.modelId, QString("captured-model"));
        QCOMPARE(constructions, 1);
        QCOMPARE(service.knownProviderIds().size(), 12);
    }

    void factoryRequestBytes_data() {
        QTest::addColumn<QString>("providerId");
        QTest::addColumn<QString>("vendor");
        QTest::addColumn<QString>("expectedUrl");
        QTest::addColumn<QByteArray>("expectedBody");
        const QByteArray local =
            R"({"max_tokens":128,"messages":[{"content":"fixture prompt","role":"user"}],"model":"fixture-model","stream":false})";
        const QByteArray cloud =
            R"({"max_tokens":128,"messages":[{"content":"fixture prompt","role":"user"}],"model":"fixture-model","stream":false,"temperature":0.7})";
        const QByteArray claude =
            R"({"max_tokens":128,"messages":[{"content":"fixture prompt","role":"user"}],"model":"fixture-model","stream":false})";
        const QByteArray gemini =
            R"({"contents":[{"parts":[{"text":"fixture prompt"}],"role":"user"}],"generationConfig":{"maxOutputTokens":128,"temperature":0.7,"topP":0.9}})";
        const auto row = [&](const char* name, const QString& id, const QString& selected,
                             const QString& url, const QByteArray& body) {
            QTest::newRow(name) << id << selected << url << body;
        };
        row("lm-studio-v1-override", "lm-studio", {}, "http://127.0.0.1:19201/v1/chat/completions",
            local);
        row("llama-base-override", "llama-cpp-server", {},
            "http://127.0.0.1:19202/v1/chat/completions", local);
        row("fixed-local", "openai-compatible-local", {},
            "http://127.0.0.1:8000/v1/chat/completions", local);
        const QList<QPair<QString, QString>> endpoints{
            {"openai", "https://api.openai.com/v1/chat/completions"},
            {"openai-compatible", "https://api.openai.com/v1/chat/completions"},
            {"claude", "https://api.anthropic.com/v1/messages"},
            {"gemini", "https://generativelanguage.googleapis.com/v1beta/models/"
                       "fixture-model:generateContent?key=synthetic-key"},
            {"deepseek", "https://api.deepseek.com/v1/chat/completions"},
            {"groq", "https://api.groq.com/openai/v1/chat/completions"},
            {"mistral", "https://api.mistral.ai/v1/chat/completions"}};
        for (const auto& [id, url] : endpoints) {
            const auto body = id == "claude" ? claude : id == "gemini" ? gemini : cloud;
            row(qPrintable(id), id, {}, url, body);
            row(qPrintable("facade-" + id), "cloud-api", id, url, body);
        }
        row("facade-unknown-legacy-fallback", "cloud-api", "unknown",
            "https://api.openai.com/v1/chat/completions", cloud);
    }

    void factoryRequestBytes() {
        QFETCH(QString, providerId);
        QFETCH(QString, vendor);
        QFETCH(QString, expectedUrl);
        QFETCH(QByteArray, expectedBody);
        AppSettings settings(std::make_unique<InMemorySettingsStore>(),
                             inMemoryTestCredentialStore());
        settings.setLmStudioEndpoint("http://127.0.0.1:19201/v1");
        settings.setLlamaCppEndpoint("http://127.0.0.1:19202");
        settings.setSelectedCloudProvider(vendor);
        settings.setOpenAiApiKey("synthetic-key");
        settings.setClaudeApiKey("synthetic-key");
        settings.setGeminiApiKey("synthetic-key");
        settings.setDeepseekApiKey("synthetic-key");
        settings.setGroqApiKey("synthetic-key");
        settings.setMistralApiKey("synthetic-key");
        ModelService service(&settings);
        service.setLmStudioEndpoint(settings.lmStudioEndpoint());
        service.setLlamaCppEndpoint(settings.llamaCppEndpoint());
        auto state = std::make_shared<ScriptedTransportState>();
        state->responder = [](const RecordedProviderRequest& request) {
            if (request.url.host() == "api.anthropic.com") {
                return ScriptedHttpResponse::body(
                    R"({"content":[{"type":"text","text":"fixture answer"}]})");
            }
            if (request.url.host() == "generativelanguage.googleapis.com") {
                return ScriptedHttpResponse::body(
                    R"({"candidates":[{"content":{"parts":[{"text":"fixture answer"}]}}]})");
            }
            return ScriptedHttpResponse::body(
                R"({"choices":[{"message":{"content":"fixture answer"}}]})");
        };
        service.setProviderNetworkFactoryForTesting(
            std::make_shared<ScriptedNetworkFactory>(state));
        const auto client = service.constructProvider(ModelBinding{providerId, "fixture-model"});
        QVERIFY(client);
        ChatRequestOptions options;
        options.deadlineMs = 500;
        options.maxOutputTokens = 128;
        ScopedOnlineNetworkMode online;
        const auto reply = client->sendRequest("fixture prompt", options);
        QVERIFY2(reply.success, qPrintable(reply.errorMessage));
        QCOMPARE(reply.message, QString("fixture answer"));
        QCOMPARE(reply.lifecycle, ChatRequestLifecycle::Completed);
        QCOMPARE(state->requests.size(), 1);
        const auto& request = state->requests.first();
        QCOMPARE(request.url, QUrl(expectedUrl));
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.body, expectedBody);
        if (expectedUrl.contains("anthropic")) {
            QCOMPARE(request.headers.value("x-api-key"), QByteArray("synthetic-key"));
            QCOMPARE(request.headers.value("anthropic-version"), QByteArray("2023-06-01"));
        } else if (expectedUrl.startsWith("https://") && !expectedUrl.contains("googleapis")) {
            QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer synthetic-key"));
        }
        QCOMPARE(service.providerHealth(providerId), ProviderHealth::Available);
    }
};

QTEST_GUILESS_MAIN(EndpointAdapterCharacterizationTest)
#include "test_endpoint_adapter_characterization.moc"
