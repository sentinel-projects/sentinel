// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef Q_MOC_RUN
#include "../support/ScriptedProviderTransport.h"
#endif
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/provider/protocols/anthropic_messages/AnthropicMessagesProtocol.h"
#include "sentinel/core/provider/protocols/gemini_generate_content/GeminiGenerateContentProtocol.h"
#include "sentinel/core/runtime/OllamaRuntime.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include <QTimer>
using namespace sentinel::core;
using namespace sentinel::test;
namespace {
QByteArray snapshot(const QList<OllamaModelSummary>& models) {
    QJsonArray rows;
    for (const auto& m : models) {
        const auto& c = m.capabilities;
        rows.append(QJsonArray{m.name, m.modifiedAt, double(m.sizeBytes),
                               c.contextWindow ? QJsonValue(*c.contextWindow) : QJsonValue(),
                               c.maxOutputTokens ? QJsonValue(*c.maxOutputTokens) : QJsonValue(),
                               int(c.nativeToolCalling), int(c.structuredOutput),
                               int(c.visionInput), int(c.streaming), m.publisher, m.family,
                               m.architecture, int(c.audioInput), int(c.audioOutput)});
    }
    return QJsonDocument(rows).toJson(QJsonDocument::Compact);
}
QList<OllamaModelSummary> fetch(bool gemini, const QString& key, QString* error,
                                const std::shared_ptr<std::atomic_bool>& cancel,
                                ProviderDiscoveryOutcome* outcome,
                                const std::shared_ptr<ILocalInferenceNetworkFactory>& factory) {
    return gemini ? fetchGeminiCloudModels(key, 100, error, cancel, outcome, factory)
                  : fetchAnthropicCloudModels(key, 100, error, cancel, outcome, factory);
}
} // namespace
class NativeDiscoveryCharacterization final : public QObject {
    Q_OBJECT
private slots:
    void golden_data() {
        QTest::addColumn<bool>("gemini");
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<QByteArray>("expected");
        QTest::addColumn<bool>("malformed");
        auto row = [](const char* tag, bool gemini, const char* payload, const char* expected,
                      bool bad = false) {
            QTest::newRow(tag) << gemini << QByteArray(payload) << QByteArray(expected) << bad;
        };
        row("claude-normal", false,
            R"({"data":[{"id":" c-b ","display_name":"Ignored","created_at":"Ignored","created":123},{"id":"c-a"}]})",
            R"([["c-b","123",0,null,null,0,0,0,0,"","","",0,0],["c-a","",0,null,null,0,0,0,0,"","","",0,0]])");
        row("claude-duplicates", false, R"({"data":[{"id":"x"},{"id":"x"}]})",
            R"([["x","",0,null,null,0,0,0,0,"","","",0,0],["x","",0,null,null,0,0,0,0,"","","",0,0]])");
        row("claude-pagination", false,
            R"({"data":[],"has_more":true,"last_id":"next","first_id":"first"})", "[]");
        row("claude-bad-pagination", false, R"({"data":[],"has_more":{},"last_id":42})", "[]");
        row("claude-metadata", false,
            R"({"data":[{"id":"x","context_window":2.5,"max_context_length":512,"max_output_tokens":7,"tool_calling":true,"structured_output":false,"vision":true,"streaming":false,"publisher":"p","family":"f","arch":"a","audio_input":true,"audio_output":false}]})",
            R"([["x","",0,512,7,2,1,2,1,"p","f","a",2,1]])");
        row("gemini-normal", true,
            R"({"models":[{"name":" models/g-b ","displayName":" Label ","inputTokenLimit":100,"outputTokenLimit":20},{"name":"g-a"}]})",
            R"([["g-b","Label",0,100,20,0,0,0,0,"","","",0,0],["g-a","Google Cloud",0,null,null,0,0,0,0,"","","",0,0]])");
        row("gemini-duplicates", true, R"({"models":[{"name":"models/x"},{"name":"x"}]})",
            R"([["x","Google Cloud",0,null,null,0,0,0,0,"","","",0,0],["x","Google Cloud",0,null,null,0,0,0,0,"","","",0,0]])");
        row("gemini-pagination", true, R"({"models":[],"nextPageToken":"next"})", "[]");
        row("gemini-bad-pagination", true, R"({"models":[],"nextPageToken":{}})", "[]");
        row("gemini-whitespace-after-prefix", true, R"({"models":[{"name":"models/ x "}]})",
            R"([[" x","Google Cloud",0,null,null,0,0,0,0,"","","",0,0]])");
        row("gemini-limits-and-methods", true,
            R"({"models":[{"name":"x","inputTokenLimit":2.5,"outputTokenLimit":2147483648,"supportedGenerationMethods":["embedContent"],"capabilities":{"vision":true}}]})",
            R"([["x","Google Cloud",0,0,null,0,0,0,0,"","","",0,0]])");
        for (bool g : {false, true}) {
            const QByteArray key = g ? "models" : "data";
            auto add = [&](const QByteArray& tag, const QByteArray& payload, bool bad) {
                QTest::newRow(((g ? "gemini-" : "claude-") + tag).constData())
                    << g << payload << QByteArray("[]") << bad;
            };
            add("empty", "{\"" + key + "\":[]}", false);
            add("missing", "{}", true);
            add("wrong-list", "{\"" + key + "\":{}}", true);
            add("bad-entry", "{\"" + key + "\":[{}]}", true);
            add("nonobject", "{\"" + key + "\":[42]}", true);
            const QByteArray id = g ? "name" : "id";
            for (const auto& val : {QByteArray("null"), QByteArray("42"), QByteArray("\" \"")}) {
                add("bad-id-" + val, "{\"" + key + "\":[{\"" + id + "\":" + val + "}]}", true);
            }
            add("partial-invalid", "{\"" + key + "\":[{\"" + id + "\":\"valid\"},{}]}", true);
            add("error", R"({"error":{"message":"PRIVATE_CATALOG"}})", true);
        }
        row("gemini-empty-prefix", true, R"({"models":[{"name":"models/"}]})", "[]", true);
    }
    void golden() {
        QFETCH(bool, gemini);
        QFETCH(QByteArray, payload);
        QFETCH(QByteArray, expected);
        QFETCH(bool, malformed);
        ScopedOnlineNetworkMode online;
        auto state = std::make_shared<ScriptedTransportState>();
        state->responder = [&](const auto&) { return ScriptedHttpResponse::body(payload); };
        auto factory = std::make_shared<ScriptedNetworkFactory>(state);
        ProviderDiscoveryOutcome outcome;
        QString error = "previous";
        const auto models = fetch(gemini, " fixture-key ", &error, {}, &outcome, factory);
        for (const auto& m : models) {
            QCOMPARE(m.capabilities.provenance.contextWindow, ModelMetadataSource::Unknown);
            QCOMPARE(m.capabilities.provenance.nativeToolCalling, ModelMetadataSource::Unknown);
        }
        const auto object = QJsonDocument::fromJson(payload).object();
        const auto pure = gemini ? provider::parseGeminiModelCatalog(object)
                                 : provider::parseAnthropicModelCatalog(object);
        QCOMPARE(pure.has_value(), !malformed);
        QCOMPARE(snapshot(pure.value_or(QList<OllamaModelSummary>{})), expected);
        QCOMPARE(snapshot(models), expected);
        QCOMPARE(outcome.completed, !malformed);
        QCOMPARE(outcome.category, malformed ? ChatProviderErrorCategory::MalformedResponse
                                             : ChatProviderErrorCategory::None);
        QCOMPARE(outcome.httpStatus, 200);
        QVERIFY(!outcome.requestId.isEmpty());
        QCOMPARE(error, malformed
                            ? (gemini ? QString("Gemini returned an invalid model catalog.")
                                      : QString("Model list returned an invalid model catalog."))
                            : (gemini ? QString("previous") : QString()));
        QCOMPARE(state->requests.size(), 1);
        const auto& request = state->requests.first();
        QCOMPARE(request.method, QByteArray("GET"));
        QVERIFY(request.body.isEmpty());
        QCOMPARE(
            request.url.toString(),
            gemini
                ? QString("https://generativelanguage.googleapis.com/v1beta/models?key=fixture-key")
                : QString("https://api.anthropic.com/v1/models"));
        QCOMPARE(request.headers.value("x-api-key"),
                 gemini ? QByteArray() : QByteArray("fixture-key"));
        QCOMPARE(request.headers.value("anthropic-version"),
                 gemini ? QByteArray() : QByteArray("2023-06-01"));
        QVERIFY(!error.contains("PRIVATE_CATALOG"));
    }
    void precedence_data() {
        QTest::addColumn<bool>("gemini");
        QTest::addColumn<int>("status");
        QTest::addColumn<int>("category");
        QTest::addColumn<int>("requests");
        for (bool g : {false, true}) {
            auto row = [&](const char* tag, int status, ChatProviderErrorCategory category,
                           int requests) {
                QTest::newRow(((g ? QByteArray("g-") : QByteArray("c-")) + tag).constData())
                    << g << status << int(category) << requests;
            };
            row("401", 401, ChatProviderErrorCategory::AuthenticationRequired, 1);
            row("403", 403, ChatProviderErrorCategory::AuthenticationRequired, 1);
            row("429", 429, ChatProviderErrorCategory::RateLimited, 2);
            row("503", 503, ChatProviderErrorCategory::ProviderUnavailable, 2);
            row("invalid-json", 200, ChatProviderErrorCategory::MalformedResponse, 1);
            row("transport", 0, ChatProviderErrorCategory::ConnectionFailed, 2);
        }
    }
    void precedence() {
        QFETCH(bool, gemini);
        QFETCH(int, status);
        QFETCH(int, category);
        QFETCH(int, requests);
        ScopedOnlineNetworkMode online;
        auto state = std::make_shared<ScriptedTransportState>();
        state->responder = [&](const auto&) {
            auto response = ScriptedHttpResponse::body("not-json PRIVATE_CATALOG", status);
            if (status == 0) {
                response.error = QNetworkReply::HostNotFoundError;
                response.errorString = "PRIVATE_CATALOG";
            }
            return response;
        };
        ProviderDiscoveryOutcome outcome;
        QString error;
        QVERIFY(fetch(gemini, "fixture-key", &error, {}, &outcome,
                      std::make_shared<ScriptedNetworkFactory>(state))
                    .isEmpty());
        QCOMPARE(int(outcome.category), category);
        QCOMPARE(outcome.httpStatus, status);
        QCOMPARE(state->requests.size(), requests);
        QVERIFY(!outcome.completed);
        QVERIFY(!error.contains("PRIVATE_CATALOG"));
    }
    void catalogAcceptance() {
        AppSettings settings(std::make_unique<InMemorySettingsStore>(),
                             inMemoryTestCredentialStore());
        settings.setNetworkMode("Online");
        settings.setClaudeApiKey("fixture-key");
        settings.setGeminiApiKey("fixture-key");
        ModelService service(&settings);
        for (const auto& id : QStringList{"claude", "gemini"}) {
            const auto old = service.beginProviderHealthObservation();
            const auto current = service.beginProviderHealthObservation();
            OllamaModelSummary model{"z"};
            model.capabilities.contextWindow = 100;
            service.acceptProviderDiscovery(id, {model, {"a"}, {"z"}},
                                            {true, ChatProviderErrorCategory::None, 200, "ok"},
                                            current);
            QCOMPARE(service.providerStatusSnapshot(id).modelIds, (QStringList{"z", "a", "z"}));
            QCOMPARE(service.capabilities(id, "z").contextWindow, std::optional<int>(100));
            QCOMPARE(service.capabilities(id, "z").provenance.contextWindow,
                     ModelMetadataSource::ProviderCatalog);
            QCOMPARE(service.capabilities(id, "z").structuredOutput, CapabilitySupport::Unknown);
            QCOMPARE(service.capabilities(id, "z").provenance.structuredOutput,
                     ModelMetadataSource::Unknown);
            service.acceptProviderDiscovery(
                id, {{"stale"}}, {true, ChatProviderErrorCategory::None, 200, "old"}, old);
            QCOMPARE(service.providerStatusSnapshot(id).modelIds, (QStringList{"z", "a", "z"}));
            service.acceptProviderDiscovery(
                id, {}, {false, ChatProviderErrorCategory::Cancelled, 0, "cancel"},
                service.beginProviderHealthObservation());
            QCOMPARE(service.providerStatusSnapshot(id).catalog, ProviderCatalogState::Stale);
            QCOMPARE(service.providerStatusSnapshot(id).modelIds, (QStringList{"z", "a", "z"}));
            service.acceptProviderDiscovery(id, {{"replacement"}},
                                            {true, ChatProviderErrorCategory::None, 200, "new"},
                                            service.beginProviderHealthObservation());
            QCOMPARE(service.providerStatusSnapshot(id).modelIds, QStringList{"replacement"});
            service.acceptProviderDiscovery(id, {},
                                            {true, ChatProviderErrorCategory::None, 200, "empty"},
                                            service.beginProviderHealthObservation());
            QCOMPARE(service.providerStatusSnapshot(id).catalog, ProviderCatalogState::Empty);
        }
    }
    void cancelledAndMissingKey() {
        ScopedOnlineNetworkMode online;
        for (bool gemini : {false, true}) {
            auto state = std::make_shared<ScriptedTransportState>();
            auto factory = std::make_shared<ScriptedNetworkFactory>(state);
            auto cancel = std::make_shared<std::atomic_bool>(true);
            ProviderDiscoveryOutcome outcome;
            QString error;
            QVERIFY(fetch(gemini, " ", &error, cancel, &outcome, factory).isEmpty());
            QCOMPARE(outcome.category, ChatProviderErrorCategory::AuthenticationRequired);
            QCOMPARE(state->requests.size(), 0);
            QVERIFY(fetch(gemini, "fixture-key", &error, cancel, &outcome, factory).isEmpty());
            QCOMPARE(outcome.category, ChatProviderErrorCategory::Cancelled);
            QCOMPARE(state->requests.size(), 0);
            cancel->store(false);
            state->responder = [](const auto&) {
                return ScriptedHttpResponse::streaming({}, false);
            };
            QTimer::singleShot(5, [cancel] { cancel->store(true); });
            QVERIFY(fetch(gemini, "fixture-key", &error, cancel, &outcome, factory).isEmpty());
            QCOMPARE(outcome.category, ChatProviderErrorCategory::Cancelled);
            QCOMPARE(state->requests.size(), 1);
        }
    }
};
QTEST_GUILESS_MAIN(NativeDiscoveryCharacterization)
#include "test_native_discovery_characterization.moc"
