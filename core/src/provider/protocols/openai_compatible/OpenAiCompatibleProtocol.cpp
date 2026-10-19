// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>

#include <cmath>
#include <limits>
#include <utility>

namespace sentinel::core::provider {

QString openAiCompatibleChatCompletionsPath(const QUrl& endpoint) {
    return endpoint.adjusted(QUrl::StripTrailingSlash).path().endsWith(QLatin1String("/v1"))
               ? QStringLiteral("/chat/completions")
               : QStringLiteral("/v1/chat/completions");
}

QJsonObject buildOpenAiCompatibleChatBody(const QString& model, const QJsonArray& messages,
                                          int maxTokens, bool stream,
                                          std::optional<double> temperature) {
    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), stream);
    if (temperature)
        body.insert(QStringLiteral("temperature"), *temperature);
    body.insert(QStringLiteral("max_tokens"), maxTokens);
    return body;
}

std::optional<QString> parseOpenAiCompatibleChatText(const QJsonObject& body,
                                                     OpenAiCompatibleChatTextError* error) {
    const auto choices = body.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        if (error)
            *error = OpenAiCompatibleChatTextError::MissingChoices;
        return std::nullopt;
    }
    auto text = choices.first()
                    .toObject()
                    .value(QStringLiteral("message"))
                    .toObject()
                    .value(QStringLiteral("content"))
                    .toString();
    if (text.isEmpty()) {
        if (error)
            *error = OpenAiCompatibleChatTextError::MissingText;
        return std::nullopt;
    }
    if (error)
        *error = OpenAiCompatibleChatTextError::None;
    return text;
}

QList<OpenAiCompatibleToolCall> parseOpenAiCompatibleToolCalls(const QJsonObject& body) {
    QList<OpenAiCompatibleToolCall> calls;
    const auto choices = body.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return calls;
    }
    const auto message = choices.first().toObject().value(QStringLiteral("message")).toObject();
    for (const auto& value : message.value(QStringLiteral("tool_calls")).toArray()) {
        const auto call = value.toObject();
        const auto function = call.value(QStringLiteral("function")).toObject();
        calls.append(OpenAiCompatibleToolCall{call.value(QStringLiteral("id")).toString(),
                                              function.value(QStringLiteral("name")).toString(),
                                              function.value(QStringLiteral("arguments"))});
    }
    return calls;
}

QString openAiCompatibleFunctionName(const QString& toolId) {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    if (valid.match(toolId).hasMatch())
        return toolId;
    return QStringLiteral("tool_%1").arg(QString::fromLatin1(
        QCryptographicHash::hash(toolId.toUtf8(), QCryptographicHash::Sha256).toHex().left(48)));
}

QJsonObject openAiCompatibleToolDefinition(const ToolDescriptor& tool) {
    QJsonObject schema = tool.inputSchema;
    if (schema.isEmpty()) {
        QJsonObject properties;
        QJsonArray required;
        for (const auto& parameter : tool.parameters) {
            properties.insert(parameter.id,
                              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                          {QStringLiteral("description"), parameter.description}});
            if (parameter.required)
                required.append(parameter.id);
        }
        schema = {{QStringLiteral("type"), QStringLiteral("object")},
                  {QStringLiteral("properties"), properties},
                  {QStringLiteral("required"), required}};
    }
    return {{QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), openAiCompatibleFunctionName(tool.id)},
                         {QStringLiteral("description"), tool.description.left(200)},
                         {QStringLiteral("parameters"), schema}}}};
}

OpenAiCompatibleNativeRequest
buildOpenAiCompatibleNativeRequest(const QString& model, const QJsonValue& content,
                                   const ChatRequestOptions& options) {
    QJsonArray messages{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("content"), content}}};
    if (!options.priorToolCalls.isEmpty()) {
        if (options.priorToolCalls.size() != options.toolResults.size())
            return {{}, QStringLiteral("Native tool results are incomplete.")};
        QJsonArray calls;
        for (const auto& call : options.priorToolCalls) {
            calls.append(QJsonObject{
                {QStringLiteral("id"), call.callId},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"),
                 QJsonObject{{QStringLiteral("name"), openAiCompatibleFunctionName(call.toolId)},
                             {QStringLiteral("arguments"),
                              QString::fromUtf8(QJsonDocument(call.arguments)
                                                    .toJson(QJsonDocument::Compact))}}}});
        }
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                    {QStringLiteral("tool_calls"), calls}});
        for (int i = 0; i < options.toolResults.size(); ++i) {
            const auto& result = options.toolResults.at(i);
            if (result.callId != options.priorToolCalls.at(i).callId)
                return {{}, QStringLiteral("Native tool result IDs do not match.")};
            messages.append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("tool")},
                {QStringLiteral("tool_call_id"), result.callId},
                {QStringLiteral("content"),
                 options.completeToolResults ? result.content : result.content.left(6800)}});
        }
    }
    QJsonObject body{{QStringLiteral("model"), model},
                     {QStringLiteral("messages"), messages},
                     {QStringLiteral("stream"), false}};
    if (options.nativeToolCalling) {
        QJsonArray tools;
        for (const auto& tool : options.tools)
            if (tool.enabled && tool.exposedToModel)
                tools.append(openAiCompatibleToolDefinition(tool));
        if (!tools.isEmpty()) {
            body.insert(QStringLiteral("tools"), tools);
            body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
        }
    }
    if (options.structuredOutput) {
        if (options.structuredSchemaName.isEmpty() || options.structuredSchema.isEmpty())
            return {{}, QStringLiteral("Structured planner schema is missing.")};
        body.insert(
            QStringLiteral("response_format"),
            QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                        {QStringLiteral("json_schema"),
                         QJsonObject{{QStringLiteral("name"), options.structuredSchemaName},
                                     {QStringLiteral("strict"), options.strictStructuredOutput},
                                     {QStringLiteral("schema"), options.structuredSchema}}}});
    }
    return {body, {}};
}

OpenAiCompatibleNativeResponse normalizeOpenAiCompatibleNativeResponse(const QJsonObject& body) {
    OpenAiCompatibleNativeResponse parsed;
    const auto choices = body.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return parsed;
    }
    parsed.hasChoices = true;
    const auto replyMessage =
        choices.first().toObject().value(QStringLiteral("message")).toObject();
    const auto rawFinish = choices.first().toObject().value(QStringLiteral("finish_reason"));
    if (rawFinish.isString()) {
        const auto reason = rawFinish.toString();
        parsed.diagnostics.insert(QStringLiteral("finish_reason"),
                                  QStringList{QStringLiteral("stop"), QStringLiteral("length"),
                                              QStringLiteral("tool_calls"),
                                              QStringLiteral("content_filter")}
                                          .contains(reason)
                                      ? reason
                                      : QStringLiteral("other"));
    }
    const auto usage = body.value(QStringLiteral("usage")).toObject();
    for (const auto& key : {"prompt_tokens", "completion_tokens", "total_tokens"}) {
        const auto value = usage.value(QLatin1String(key));
        if (value.isDouble() && value.toDouble() >= 0)
            parsed.diagnostics.insert(QString::fromLatin1(key), value);
    }
    const auto reasoningTokens = usage.value(QStringLiteral("completion_tokens_details"))
                                     .toObject()
                                     .value(QStringLiteral("reasoning_tokens"));
    if (reasoningTokens.isDouble() && reasoningTokens.toDouble() >= 0)
        parsed.diagnostics.insert(QStringLiteral("reasoning_tokens"), reasoningTokens);
    parsed.diagnostics.insert(
        QStringLiteral("response_content_bytes"),
        replyMessage.value(QStringLiteral("content")).toString().toUtf8().size());
    parsed.diagnostics.insert(QStringLiteral("response_tool_calls"),
                              replyMessage.value(QStringLiteral("tool_calls")).toArray().size());
    parsed.text = replyMessage.value(QStringLiteral("content")).toString();
    parsed.outputLimit = rawFinish.toString() == QLatin1String("length");
    parsed.toolCalls = parseOpenAiCompatibleToolCalls(body);
    return parsed;
}

std::optional<QJsonObject> parseOpenAiCompatibleJsonObject(const QString& text) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

OpenAiCompatibleCatalogParse parseOpenAiCompatibleModelCatalog(const QJsonObject& body) {
    OpenAiCompatibleCatalogParse parsed;
    const auto data = body.value(QStringLiteral("data"));
    if (!data.isArray()) {
        parsed.malformed = true;
        return parsed;
    }

    const auto positiveLimit = [](const QJsonObject& object,
                                  const QString& key) -> std::optional<int> {
        const auto field = object.value(key);
        if (!field.isDouble())
            return std::nullopt;
        const auto number = field.toDouble();
        if (number <= 0 || number > std::numeric_limits<int>::max() || number != std::floor(number))
            return std::nullopt;
        return static_cast<int>(number);
    };
    const auto reportedSupport = [](const QJsonObject& object, const QString& key) {
        const auto field = object.value(key);
        return field.isBool() ? (field.toBool() ? CapabilitySupport::Supported
                                                : CapabilitySupport::Unsupported)
                              : CapabilitySupport::Unknown;
    };

    const auto dataValues = data.toArray();
    for (const auto& value : dataValues) {
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("id")).toString().trimmed();
        if (!value.isObject() || !object.value(QStringLiteral("id")).isString() || id.isEmpty()) {
            parsed.models.clear();
            parsed.malformed = true;
            return parsed;
        }

        QString createdStr;
        if (object.contains(QStringLiteral("created"))) {
            const auto createdVal = object.value(QStringLiteral("created"));
            if (createdVal.isDouble()) {
                createdStr = QString::number(createdVal.toDouble());
            } else {
                createdStr = createdVal.toString();
            }
        }

        OllamaModelSummary model{id, createdStr, 0};
        model.capabilities.contextWindow = positiveLimit(object, QStringLiteral("context_window"));
        if (!model.capabilities.contextWindow)
            model.capabilities.contextWindow =
                positiveLimit(object, QStringLiteral("max_context_length"));
        model.capabilities.maxOutputTokens =
            positiveLimit(object, QStringLiteral("max_output_tokens"));

        const auto reported = object.value(QStringLiteral("capabilities")).toObject();
        const auto booleanCapability = [&reported](const QString& key) {
            const auto field = reported.value(key);
            return field.isBool() ? (field.toBool() ? CapabilitySupport::Supported
                                                    : CapabilitySupport::Unsupported)
                                  : CapabilitySupport::Unknown;
        };
        model.capabilities.nativeToolCalling =
            booleanCapability(QStringLiteral("trained_for_tool_use"));
        if (model.capabilities.nativeToolCalling == CapabilitySupport::Unknown)
            model.capabilities.nativeToolCalling =
                reportedSupport(object, QStringLiteral("tool_calling"));
        model.capabilities.structuredOutput =
            reportedSupport(object, QStringLiteral("structured_output"));
        model.capabilities.visionInput = booleanCapability(QStringLiteral("vision"));
        if (model.capabilities.visionInput == CapabilitySupport::Unknown)
            model.capabilities.visionInput = reportedSupport(object, QStringLiteral("vision"));
        model.capabilities.audioInput = reportedSupport(object, QStringLiteral("audio_input"));
        model.capabilities.audioOutput = reportedSupport(object, QStringLiteral("audio_output"));
        model.capabilities.streaming = reportedSupport(object, QStringLiteral("streaming"));
        model.publisher = object.value(QStringLiteral("publisher")).toString();
        model.family = object.value(QStringLiteral("family")).toString();
        model.architecture = object.value(QStringLiteral("architecture")).toString();
        if (model.architecture.isEmpty())
            model.architecture = object.value(QStringLiteral("arch")).toString();
        parsed.models.append(std::move(model));
    }
    return parsed;
}

} // namespace sentinel::core::provider
