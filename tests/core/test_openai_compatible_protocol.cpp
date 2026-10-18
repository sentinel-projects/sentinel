// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.h"
#include "sentinel/core/runtime/LocalInference.h"
#ifndef Q_MOC_RUN
#include "../support/ScriptedProviderTransport.h"
#endif

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QtTest>

#include <memory>

namespace {

using sentinel::core::provider::OpenAiCompatibleChatTextError;

QJsonArray userMessages(const QString& text) {
    return QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                  {QStringLiteral("content"), text}}};
}

QByteArray compactJson(const QJsonObject& body) {
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QByteArray compactJson(const QJsonArray& array) {
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

// Pre-refactor golden bytes captured from core/src/runtime/LocalInference.cpp at
// base 6035bf74 (see docs/audits/P2_M02_OPENAI_COMPATIBLE_PROTOCOL_EXTRACTION_RESULT.md).
const QByteArray kLocalGolden =
    R"({"max_tokens":512,"messages":[{"content":"hello world","role":"user"}],"model":"fixture-model","stream":false})";
const QByteArray kLocalClampedGolden =
    R"({"max_tokens":1,"messages":[{"content":"hello world","role":"user"}],"model":"fixture-model","stream":false})";

} // namespace

class OpenAiCompatibleProtocolTest final : public QObject {
    Q_OBJECT

private slots:
    void nativeValueNormalization();
    void nativeContinuationValidation();
    void nativeResponseMetadata();
    void objectOnlyDecoding_data();
    void objectOnlyDecoding();
    void chatPathGeneration_data();
    void chatPathGeneration();
    void requestBodyByteEquivalence_data();
    void requestBodyByteEquivalence();
    void standardTextResponseParsing_data();
    void standardTextResponseParsing();
    void emptyAndMalformedResponses_data();
    void emptyAndMalformedResponses();
    void toolPayloadPassThrough();
    void structuredJsonOutputPassThrough();
    void toolCallResponseParsing();
    void validCatalogParsing();
    void malformedCatalogRejected_data();
    void malformedCatalogRejected();
    void missingModelIdentifierRejected();
    void duplicateAndInvalidEntries();
    void providerSpecificCatalogFields();
    void localInferenceBodyByteEquivalence_data();
    void localInferenceBodyByteEquivalence();
};

void OpenAiCompatibleProtocolTest::nativeValueNormalization() {
    using namespace sentinel::core;
    using namespace sentinel::core::provider;
    QCOMPARE(openAiCompatibleFunctionName("read-file"), QString("read-file"));
    QCOMPARE(openAiCompatibleFunctionName(QString(64, 'a')), QString(64, 'a'));
    QCOMPARE(openAiCompatibleFunctionName("read file!/unsafe"),
             QString("tool_2d46bdc68136d0dbc706401fcc2908db3cac482228c7821e"));
    QVERIFY(openAiCompatibleFunctionName(QString(65, 'a')).startsWith("tool_"));
    ToolDescriptor tool;
    tool.id = "read-file";
    tool.description = QString(201, 'd');
    tool.parameters = {{"path", "fixture", true}, {"optional", "", false}};
    auto function = openAiCompatibleToolDefinition(tool).value("function").toObject();
    QCOMPARE(function.value("description").toString(), QString(200, 'd'));
    QCOMPARE(function.value("parameters").toObject().value("required").toArray(),
             QJsonArray{"path"});
    tool.inputSchema = {{"type", "object"}, {"additionalProperties", false}};
    QCOMPARE(openAiCompatibleToolDefinition(tool)
                 .value("function")
                 .toObject()
                 .value("parameters")
                 .toObject(),
             tool.inputSchema);
    ChatRequestOptions options;
    options.nativeToolCalling = true;
    options.tools = {tool};
    options.tools[0].enabled = false;
    options.deadlineMs = 1;
    options.maxOutputTokens = 77;
    options.cancellationToken = std::make_shared<std::atomic_bool>(true);
    const auto request = buildOpenAiCompatibleNativeRequest("model", "prompt", options);
    QVERIFY(request.error.isEmpty());
    // Budget, cancellation and capability policy are not performed by this pure builder.
    QCOMPARE(
        compactJson(request.body),
        QByteArray(
            R"({"messages":[{"content":"prompt","role":"user"}],"model":"model","stream":false})"));
}

void OpenAiCompatibleProtocolTest::nativeContinuationValidation() {
    using namespace sentinel::core;
    using namespace sentinel::core::provider;
    ChatRequestOptions options;
    options.structuredOutput = true;
    options.priorToolCalls = {{"opaque-id", "unregistered", {}}};
    QCOMPARE(buildOpenAiCompatibleNativeRequest("model", "prompt", options).error,
             QString("Native tool results are incomplete."));
    options.toolResults = {{"different-id", "observation"}};
    QCOMPARE(buildOpenAiCompatibleNativeRequest("model", "prompt", options).error,
             QString("Native tool result IDs do not match."));
    options.toolResults[0].callId = "opaque-id";
    QCOMPARE(buildOpenAiCompatibleNativeRequest("model", "prompt", options).error,
             QString("Structured planner schema is missing."));
    options.structuredOutput = false;
    auto request = buildOpenAiCompatibleNativeRequest("model", "prompt", options);
    QVERIFY(request.error.isEmpty()); // Registration is intentionally the adapter/gateway's job.
    QCOMPARE(request.body.value("messages").toArray().size(), 3);
    options.priorToolCalls.clear();
    request = buildOpenAiCompatibleNativeRequest("model", "prompt", options);
    QVERIFY(request.error.isEmpty()); // Preserve legacy orphan-result behavior.
    QCOMPARE(request.body.value("messages").toArray().size(), 1);
}

void OpenAiCompatibleProtocolTest::nativeResponseMetadata() {
    using namespace sentinel::core::provider;
    QVERIFY(!normalizeOpenAiCompatibleNativeResponse({}).hasChoices);
    const auto opaque = normalizeOpenAiCompatibleNativeResponse({{"choices", QJsonArray{7}}});
    QVERIFY(opaque.hasChoices);
    QVERIFY(opaque.text.isEmpty());
    const auto body =
        QJsonDocument::fromJson(
            R"({"choices":[{"finish_reason":"length","message":{"content":"é","tool_calls":[{"function":{"name":"read-file","arguments":"{}"}},{"id":"duplicate","function":{}},{"id":"duplicate","function":{}}]}}],"usage":{"prompt_tokens":1.5,"completion_tokens":-1,"total_tokens":"7","completion_tokens_details":{"reasoning_tokens":0}},"private":"not-diagnostic"})")
            .object();
    const auto response = normalizeOpenAiCompatibleNativeResponse(body);
    QVERIFY(response.outputLimit);
    QCOMPARE(response.text, QString::fromUtf8("é"));
    QCOMPARE(response.diagnostics.value("response_content_bytes").toInt(), 2);
    QCOMPARE(response.diagnostics.value("prompt_tokens").toDouble(), 1.5);
    QVERIFY(!response.diagnostics.contains("completion_tokens"));
    QVERIFY(!response.diagnostics.contains("total_tokens"));
    QVERIFY(!response.diagnostics.contains("private"));
    QCOMPARE(response.toolCalls.size(), 3);
    QVERIFY(response.toolCalls.first().callId.isEmpty()); // Never synthesize OpenAI IDs.
    QCOMPARE(response.toolCalls[1].callId, response.toolCalls[2].callId);
}

void OpenAiCompatibleProtocolTest::objectOnlyDecoding_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("valid");
    QTest::newRow("object") << QString("{\"value\":2}") << true;
    QTest::newRow("empty-object") << QString("{}") << true;
    QTest::newRow("array") << QString("[]") << false;
    QTest::newRow("null") << QString("null") << false;
    QTest::newRow("empty") << QString("") << false;
    QTest::newRow("invalid") << QString("{broken") << false;
    QTest::newRow("trailing-data") << QString("{} trailing") << false;
}

void OpenAiCompatibleProtocolTest::objectOnlyDecoding() {
    QFETCH(QString, text);
    QFETCH(bool, valid);
    QCOMPARE(sentinel::core::provider::parseOpenAiCompatibleJsonObject(text).has_value(), valid);
}

void OpenAiCompatibleProtocolTest::chatPathGeneration_data() {
    QTest::addColumn<QString>("endpoint");
    QTest::addColumn<QString>("expected");
    QTest::newRow("loopback-root")
        << QStringLiteral("http://127.0.0.1:1234") << QStringLiteral("/v1/chat/completions");
    QTest::newRow("loopback-trailing-slash")
        << QStringLiteral("http://127.0.0.1:1234/") << QStringLiteral("/v1/chat/completions");
    QTest::newRow("loopback-v1") << QStringLiteral("http://127.0.0.1:1234/v1")
                                 << QStringLiteral("/chat/completions");
    QTest::newRow("openai-root") << QStringLiteral("https://api.openai.com")
                                 << QStringLiteral("/v1/chat/completions");
    QTest::newRow("openai-v1") << QStringLiteral("https://api.openai.com/v1")
                               << QStringLiteral("/chat/completions");
    QTest::newRow("deepseek-v1") << QStringLiteral("https://api.deepseek.com/v1")
                                 << QStringLiteral("/chat/completions");
    QTest::newRow("groq-prefixed-v1")
        << QStringLiteral("https://api.groq.com/openai/v1") << QStringLiteral("/chat/completions");
    QTest::newRow("mistral-v1") << QStringLiteral("https://api.mistral.ai/v1")
                                << QStringLiteral("/chat/completions");
}

void OpenAiCompatibleProtocolTest::chatPathGeneration() {
    QFETCH(QString, endpoint);
    QFETCH(QString, expected);
    QCOMPARE(sentinel::core::provider::openAiCompatibleChatCompletionsPath(QUrl(endpoint)),
             expected);
}

void OpenAiCompatibleProtocolTest::requestBodyByteEquivalence_data() {
    QTest::addColumn<int>("maxTokens");
    QTest::addColumn<bool>("stream");
    QTest::addColumn<bool>("includeTemperature");
    QTest::addColumn<double>("temperature");
    QTest::addColumn<QByteArray>("expected");

    QTest::newRow("cloud-explicit")
        << 512 << false << true << 0.7
        << QByteArray(
               R"({"max_tokens":512,"messages":[{"content":"hello world","role":"user"}],"model":"fixture-model","stream":false,"temperature":0.7})");
    QTest::newRow("cloud-fallback-2048")
        << 2048 << false << true << 0.0
        << QByteArray(
               R"({"max_tokens":2048,"messages":[{"content":"hello world","role":"user"}],"model":"fixture-model","stream":false,"temperature":0})");
    QTest::newRow("local-explicit") << 512 << false << false << 0.0 << kLocalGolden;
    QTest::newRow("local-clamped-to-one") << 1 << false << false << 0.0 << kLocalClampedGolden;
    QTest::newRow("stream-fallback-2048")
        << 2048 << true << false << 0.0
        << QByteArray(
               R"({"max_tokens":2048,"messages":[{"content":"hello world","role":"user"}],"model":"fixture-model","stream":true})");
}

void OpenAiCompatibleProtocolTest::requestBodyByteEquivalence() {
    QFETCH(int, maxTokens);
    QFETCH(bool, stream);
    QFETCH(bool, includeTemperature);
    QFETCH(double, temperature);
    QFETCH(QByteArray, expected);

    const std::optional<double> temperatureField =
        includeTemperature ? std::optional<double>{temperature} : std::nullopt;
    const auto body = sentinel::core::provider::buildOpenAiCompatibleChatBody(
        QStringLiteral("fixture-model"), userMessages(QStringLiteral("hello world")), maxTokens,
        stream, temperatureField);
    QCOMPARE(compactJson(body), expected);
}

void OpenAiCompatibleProtocolTest::standardTextResponseParsing_data() {
    QTest::addColumn<QByteArray>("response");
    QTest::addColumn<QString>("expected");

    QTest::newRow("plain")
        << QByteArray(
               R"({"choices":[{"message":{"role":"assistant","content":"fixture answer"}}]})")
        << QStringLiteral("fixture answer");
    QTest::newRow("whitespace-preserved")
        << QByteArray(R"({"choices":[{"message":{"content":" leading and trailing "}}]})")
        << QStringLiteral(" leading and trailing ");
    QTest::newRow("unicode") << QByteArray(
                                    R"({"choices":[{"message":{"content":"naïve — ünïcøde ✅"}}]})")
                             << QStringLiteral("naïve — ünïcøde ✅");
    QTest::newRow("extra-choices-ignored")
        << QByteArray(
               R"({"choices":[{"message":{"content":"first"}},{"message":{"content":"second"}}]})")
        << QStringLiteral("first");
    QTest::newRow("finish-reason-present")
        << QByteArray(
               R"({"choices":[{"finish_reason":"stop","message":{"content":"with reason"}}]})")
        << QStringLiteral("with reason");
}

void OpenAiCompatibleProtocolTest::standardTextResponseParsing() {
    QFETCH(QByteArray, response);
    QFETCH(QString, expected);
    OpenAiCompatibleChatTextError error = OpenAiCompatibleChatTextError::MissingChoices;
    const auto text = sentinel::core::provider::parseOpenAiCompatibleChatText(
        QJsonDocument::fromJson(response).object(), &error);
    QVERIFY(text.has_value());
    QCOMPARE(*text, expected);
    QCOMPARE(error, OpenAiCompatibleChatTextError::None);
}

void OpenAiCompatibleProtocolTest::emptyAndMalformedResponses_data() {
    QTest::addColumn<QByteArray>("response");
    QTest::addColumn<int>("expectedError");

    const int missingChoices = int(OpenAiCompatibleChatTextError::MissingChoices);
    const int missingText = int(OpenAiCompatibleChatTextError::MissingText);
    QTest::newRow("empty-object") << QByteArray("{}") << missingChoices;
    QTest::newRow("empty-choices") << QByteArray(R"({"choices":[]})") << missingChoices;
    QTest::newRow("choices-not-array") << QByteArray(R"({"choices":"nope"})") << missingChoices;
    QTest::newRow("choices-null") << QByteArray(R"({"choices":null})") << missingChoices;
    QTest::newRow("empty-choice-object") << QByteArray(R"({"choices":[{}]})") << missingText;
    QTest::newRow("missing-message")
        << QByteArray(R"({"choices":[{"finish_reason":"stop"}]})") << missingText;
    QTest::newRow("empty-content")
        << QByteArray(R"({"choices":[{"message":{"content":""}}]})") << missingText;
    QTest::newRow("numeric-content")
        << QByteArray(R"({"choices":[{"message":{"content":123}}]})") << missingText;
    QTest::newRow("null-content") << QByteArray(R"({"choices":[{"message":{"content":null}}]})")
                                  << missingText;
    QTest::newRow("choice-not-object") << QByteArray(R"({"choices":["nope"]})") << missingText;
    QTest::newRow("content-array")
        << QByteArray(R"({"choices":[{"message":{"content":[{"type":"text"}]}}]})") << missingText;
}

void OpenAiCompatibleProtocolTest::emptyAndMalformedResponses() {
    QFETCH(QByteArray, response);
    QFETCH(int, expectedError);
    OpenAiCompatibleChatTextError error = OpenAiCompatibleChatTextError::None;
    const auto text = sentinel::core::provider::parseOpenAiCompatibleChatText(
        QJsonDocument::fromJson(response).object(), &error);
    QVERIFY(!text.has_value());
    QCOMPARE(int(error), expectedError);
}

void OpenAiCompatibleProtocolTest::toolPayloadPassThrough() {
    QJsonArray toolCalls;
    toolCalls.append(
        QJsonObject{{QStringLiteral("id"), QStringLiteral("call-1")},
                    {QStringLiteral("type"), QStringLiteral("function")},
                    {QStringLiteral("function"),
                     QJsonObject{{QStringLiteral("name"), QStringLiteral("fs_read")},
                                 {QStringLiteral("arguments"), QStringLiteral("{}")}}}});
    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), "call"}});
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                {QStringLiteral("tool_calls"), toolCalls}});
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                                {QStringLiteral("tool_call_id"), QStringLiteral("call-1")},
                                {QStringLiteral("content"), QStringLiteral("result")}});

    const auto body = sentinel::core::provider::buildOpenAiCompatibleChatBody(
        QStringLiteral("fixture-model"), messages, 256, false, std::nullopt);
    QCOMPARE(body.value(QStringLiteral("messages")).toArray(), messages);
    QCOMPARE(compactJson(body.value(QStringLiteral("messages")).toArray()), compactJson(messages));
    QVERIFY(!compactJson(body).contains("tool_choice"));
}

void OpenAiCompatibleProtocolTest::structuredJsonOutputPassThrough() {
    const QString structured = QStringLiteral("{\"requirements\":[],\"confidence\":0.5}");
    const auto body = sentinel::core::provider::buildOpenAiCompatibleChatBody(
        QStringLiteral("fixture-model"), userMessages(structured), 64, false, std::nullopt);
    QCOMPARE(body.value(QStringLiteral("messages"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("content"))
                 .toString(),
             structured);
    OpenAiCompatibleChatTextError error = OpenAiCompatibleChatTextError::None;
    const auto parsed = sentinel::core::provider::parseOpenAiCompatibleChatText(
        QJsonObject{
            {QStringLiteral("choices"),
             QJsonArray{QJsonObject{{QStringLiteral("message"),
                                     QJsonObject{{QStringLiteral("content"), structured}}}}}}},
        &error);
    QVERIFY(parsed.has_value());
    QCOMPARE(*parsed, structured);
    QCOMPARE(error, OpenAiCompatibleChatTextError::None);
}

void OpenAiCompatibleProtocolTest::validCatalogParsing() {
    const QByteArray response = R"({
        "object": "list",
        "data": [
            {"id": "plain", "created": 1700000000},
            {"id": " rich ", "context_window": 8192, "max_output_tokens": 1024,
             "tool_calling": true, "structured_output": false, "streaming": true,
             "vision": true, "audio_input": false, "audio_output": true,
             "publisher": "acme", "family": "fam", "architecture": "arch-a"},
            {"id": "caps", "max_context_length": 4096,
             "capabilities": {"trained_for_tool_use": false, "vision": false}}
        ]
    })";
    const auto parsed = sentinel::core::provider::parseOpenAiCompatibleModelCatalog(
        QJsonDocument::fromJson(response).object());
    QVERIFY(!parsed.malformed);
    QCOMPARE(parsed.models.size(), 3);

    QCOMPARE(parsed.models.at(0).name, QStringLiteral("plain"));
    QCOMPARE(parsed.models.at(0).modifiedAt, QStringLiteral("1.7e+09"));
    QVERIFY(!parsed.models.at(0).capabilities.contextWindow.has_value());
    QCOMPARE(parsed.models.at(0).capabilities.streaming,
             sentinel::core::CapabilitySupport::Unknown);

    QCOMPARE(parsed.models.at(1).name, QStringLiteral("rich"));
    QCOMPARE(parsed.models.at(1).capabilities.contextWindow.value_or(0), 8192);
    QCOMPARE(parsed.models.at(1).capabilities.maxOutputTokens.value_or(0), 1024);
    QCOMPARE(parsed.models.at(1).capabilities.nativeToolCalling,
             sentinel::core::CapabilitySupport::Supported);
    QCOMPARE(parsed.models.at(1).capabilities.structuredOutput,
             sentinel::core::CapabilitySupport::Unsupported);
    QCOMPARE(parsed.models.at(1).capabilities.streaming,
             sentinel::core::CapabilitySupport::Supported);
    QCOMPARE(parsed.models.at(1).capabilities.visionInput,
             sentinel::core::CapabilitySupport::Supported);
    QCOMPARE(parsed.models.at(1).capabilities.audioInput,
             sentinel::core::CapabilitySupport::Unsupported);
    QCOMPARE(parsed.models.at(1).capabilities.audioOutput,
             sentinel::core::CapabilitySupport::Supported);
    QCOMPARE(parsed.models.at(1).publisher, QStringLiteral("acme"));
    QCOMPARE(parsed.models.at(1).family, QStringLiteral("fam"));
    QCOMPARE(parsed.models.at(1).architecture, QStringLiteral("arch-a"));

    QCOMPARE(parsed.models.at(2).name, QStringLiteral("caps"));
    QCOMPARE(parsed.models.at(2).capabilities.contextWindow.value_or(0), 4096);
    QCOMPARE(parsed.models.at(2).capabilities.nativeToolCalling,
             sentinel::core::CapabilitySupport::Unsupported);
    QCOMPARE(parsed.models.at(2).capabilities.visionInput,
             sentinel::core::CapabilitySupport::Unsupported);
}

void OpenAiCompatibleProtocolTest::malformedCatalogRejected_data() {
    QTest::addColumn<QByteArray>("response");
    QTest::addColumn<bool>("malformed");
    QTest::addColumn<int>("expectedCount");

    QTest::newRow("empty-object") << QByteArray("{}") << true << 0;
    QTest::newRow("data-object") << QByteArray(R"({"data":{}})") << true << 0;
    QTest::newRow("data-string") << QByteArray(R"({"data":"x"})") << true << 0;
    QTest::newRow("data-null") << QByteArray(R"({"data":null})") << true << 0;
    QTest::newRow("wrong-shape") << QByteArray(R"({"unexpected":[]})") << true << 0;
    QTest::newRow("entry-not-object") << QByteArray(R"({"data":["x"]})") << true << 0;
    QTest::newRow("entry-null") << QByteArray(R"({"data":[null]})") << true << 0;
    QTest::newRow("missing-id") << QByteArray(R"({"data":[{}]})") << true << 0;
    QTest::newRow("empty-id") << QByteArray(R"({"data":[{"id":""}]})") << true << 0;
    QTest::newRow("whitespace-id") << QByteArray(R"({"data":[{"id":"   "}]})") << true << 0;
    QTest::newRow("numeric-id") << QByteArray(R"({"data":[{"id":7}]})") << true << 0;
    QTest::newRow("bool-id") << QByteArray(R"({"data":[{"id":true}]})") << true << 0;
    QTest::newRow("null-id") << QByteArray(R"({"data":[{"id":null}]})") << true << 0;
    QTest::newRow("valid-then-invalid")
        << QByteArray(R"({"data":[{"id":"ok"},{"id":""}]})") << true << 0;
    QTest::newRow("empty-catalog") << QByteArray(R"({"data":[]})") << false << 0;
    QTest::newRow("single-valid") << QByteArray(R"({"data":[{"id":"ok"}]})") << false << 1;
    QTest::newRow("trimmed-id") << QByteArray(R"({"data":[{"id":" ok "}]})") << false << 1;
}

void OpenAiCompatibleProtocolTest::malformedCatalogRejected() {
    QFETCH(QByteArray, response);
    QFETCH(bool, malformed);
    QFETCH(int, expectedCount);
    const auto parsed = sentinel::core::provider::parseOpenAiCompatibleModelCatalog(
        QJsonDocument::fromJson(response).object());
    QCOMPARE(parsed.malformed, malformed);
    QCOMPARE(parsed.models.size(), expectedCount);
    if (malformed)
        QVERIFY(parsed.models.isEmpty());
}

void OpenAiCompatibleProtocolTest::missingModelIdentifierRejected() {
    for (const QByteArray& response :
         {QByteArray(R"({"data":[{}]})"), QByteArray(R"({"data":[{"other":"x"}]})"),
          QByteArray(R"({"data":[{"id":null}]})"), QByteArray(R"({"data":[{"id":false}]})"),
          QByteArray(R"({"data":[{"id":[]}]})")}) {
        const auto parsed = sentinel::core::provider::parseOpenAiCompatibleModelCatalog(
            QJsonDocument::fromJson(response).object());
        QVERIFY2(parsed.malformed, response.constData());
        QVERIFY(parsed.models.isEmpty());
    }
}

void OpenAiCompatibleProtocolTest::duplicateAndInvalidEntries() {
    const auto duplicates = sentinel::core::provider::parseOpenAiCompatibleModelCatalog(
        QJsonDocument::fromJson(R"({"data":[{"id":"a"},{"id":"a"},{"id":"b"}]})").object());
    QVERIFY(!duplicates.malformed);
    QCOMPARE(duplicates.models.size(), 3);
    QCOMPARE(duplicates.models.at(0).name, QStringLiteral("a"));
    QCOMPARE(duplicates.models.at(1).name, QStringLiteral("a"));
    QCOMPARE(duplicates.models.at(2).name, QStringLiteral("b"));

    const auto invalidAfterValid = sentinel::core::provider::parseOpenAiCompatibleModelCatalog(
        QJsonDocument::fromJson(R"({"data":[{"id":"a"},{"id":7},{"id":"b"}]})").object());
    QVERIFY(invalidAfterValid.malformed);
    QVERIFY(invalidAfterValid.models.isEmpty());
}

void OpenAiCompatibleProtocolTest::providerSpecificCatalogFields() {
    const QByteArray response = R"({
        "data": [
            {"id": "provider-entry", "created": "2024-01-01T00:00:00Z",
             "arch": "llama", "streaming": true, "extra_unknown": {"ignored": true},
             "capabilities": {"vision": true}}
        ]
    })";
    const auto parsed = sentinel::core::provider::parseOpenAiCompatibleModelCatalog(
        QJsonDocument::fromJson(response).object());
    QVERIFY(!parsed.malformed);
    QCOMPARE(parsed.models.size(), 1);
    QCOMPARE(parsed.models.at(0).name, QStringLiteral("provider-entry"));
    QCOMPARE(parsed.models.at(0).modifiedAt, QStringLiteral("2024-01-01T00:00:00Z"));
    QCOMPARE(parsed.models.at(0).architecture, QStringLiteral("llama"));
    QCOMPARE(parsed.models.at(0).capabilities.streaming,
             sentinel::core::CapabilitySupport::Supported);
    QCOMPARE(parsed.models.at(0).capabilities.visionInput,
             sentinel::core::CapabilitySupport::Supported);
}

void OpenAiCompatibleProtocolTest::localInferenceBodyByteEquivalence_data() {
    QTest::addColumn<int>("maxTokens");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("explicit") << 512 << kLocalGolden;
    QTest::newRow("clamped") << 0 << kLocalClampedGolden;
}

void OpenAiCompatibleProtocolTest::localInferenceBodyByteEquivalence() {
    QFETCH(int, maxTokens);
    QFETCH(QByteArray, expected);

    auto state = std::make_shared<sentinel::test::ScriptedTransportState>();
    state->responder = [](const auto&) {
        return sentinel::test::ScriptedHttpResponse::body(
            R"({"choices":[{"message":{"role":"assistant","content":"fixture answer"}}]})");
    };
    sentinel::core::LMStudioConfig config;
    config.endpoint = QUrl(QStringLiteral("http://127.0.0.1:1234"));
    config.networkFactory = std::make_shared<sentinel::test::ScriptedNetworkFactory>(state);
    sentinel::test::ScopedOnlineNetworkMode online;

    sentinel::core::LMStudioLocalInferenceClient client(config, 500);
    sentinel::core::LocalInferenceRequest request;
    request.prompt = QStringLiteral("hello world");
    request.options.model = QStringLiteral("fixture-model");
    request.options.maxTokens = maxTokens;
    request.options.timeoutMs = 500;

    const auto response = client.infer(request);
    QCOMPARE(response.status, sentinel::core::LocalInferenceStatus::Succeeded);
    QCOMPARE(response.text, QStringLiteral("fixture answer"));
    QCOMPARE(state->requests.size(), 1);
    QCOMPARE(state->requests.first().body, expected);
    QCOMPARE(state->requests.first().url.path(), QStringLiteral("/v1/chat/completions"));
}

void OpenAiCompatibleProtocolTest::toolCallResponseParsing() {
    // Multiple assistant tool calls are normalized in provider order; the nested
    // envelope is flattened and the raw arguments string is forwarded unmodified.
    const QByteArray response = R"({
        "choices": [{"message": {"content": "",
            "tool_calls": [
                {"id": "call-1", "type": "function", "function": {"name": "fs_read", "arguments": "{\"path\":\"a\"}"}},
                {"id": "call-2", "type": "function", "function": {"name": "fs_write", "arguments": "{}"}}
            ]}}]
    })";
    const auto calls = sentinel::core::provider::parseOpenAiCompatibleToolCalls(
        QJsonDocument::fromJson(response).object());
    QCOMPARE(calls.size(), 2);
    QCOMPARE(calls.at(0).callId, QStringLiteral("call-1"));
    QCOMPARE(calls.at(0).name, QStringLiteral("fs_read"));
    QCOMPARE(calls.at(0).arguments.toString(), QStringLiteral("{\"path\":\"a\"}"));
    QCOMPARE(calls.at(1).callId, QStringLiteral("call-2"));
    QCOMPARE(calls.at(1).name, QStringLiteral("fs_write"));

    // Empty or malformed envelopes yield no calls.
    for (const QByteArray& malformed :
         {QByteArray("{}"), QByteArray(R"({"choices":[]})"), QByteArray(R"({"choices":"x"})"),
          QByteArray(R"({"choices":[{"finish_reason":"stop"}]})"),
          QByteArray(R"({"choices":[{"message":{"tool_calls":"x"}}]})"),
          QByteArray(R"({"choices":[{"message":{"content":"hi"}}]})")}) {
        QVERIFY2(sentinel::core::provider::parseOpenAiCompatibleToolCalls(
                     QJsonDocument::fromJson(malformed).object())
                     .isEmpty(),
                 malformed.constData());
    }

    // A non-object entry is surfaced with empty fields (the adapter rejects it).
    const auto degenerate = sentinel::core::provider::parseOpenAiCompatibleToolCalls(
        QJsonDocument::fromJson(R"({"choices":[{"message":{"tool_calls":["x"]}}]})").object());
    QCOMPARE(degenerate.size(), 1);
    QVERIFY(degenerate.first().callId.isEmpty());
    QVERIFY(degenerate.first().name.isEmpty());
    QVERIFY(!degenerate.first().arguments.isString());

    // A non-string arguments value is surfaced unmodified.
    const auto numericArgs = sentinel::core::provider::parseOpenAiCompatibleToolCalls(
        QJsonDocument::fromJson(
            R"({"choices":[{"message":{"tool_calls":[{"id":"c","function":{"name":"n","arguments":7}}]}}]})")
            .object());
    QCOMPARE(numericArgs.size(), 1);
    QVERIFY(numericArgs.first().arguments.isDouble());
}

QTEST_GUILESS_MAIN(OpenAiCompatibleProtocolTest)
#include "test_openai_compatible_protocol.moc"
