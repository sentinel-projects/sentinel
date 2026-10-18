// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/provider/protocols/gemini_generate_content/GeminiGenerateContentProtocol.h"

#include <QUrlQuery>
#include <limits>

namespace sentinel::core::provider {

QUrl geminiGenerateContentUrl(const QString& model, const QString& apiKey) {
    const auto percentModel = QString::fromLatin1(QUrl::toPercentEncoding(model));
    QString urlStr = QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent")
                         .arg(percentModel);
    QUrl url(urlStr);
    if (!apiKey.isEmpty()) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("key"), apiKey);
        url.setQuery(query);
    }
    return url;
}

QUrl geminiStreamGenerateContentUrl(const QString& model, const QString& apiKey) {
    const auto percentModel = QString::fromLatin1(QUrl::toPercentEncoding(model));
    QString urlStr = QStringLiteral(
                         "https://generativelanguage.googleapis.com/v1beta/models/%1:streamGenerateContent")
                         .arg(percentModel);
    QUrl url(urlStr);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("alt"), QStringLiteral("sse"));
    if (!apiKey.isEmpty()) {
        query.addQueryItem(QStringLiteral("key"), apiKey);
    }
    url.setQuery(query);
    return url;
}

QJsonObject buildGeminiGenerateContentBody(const QJsonArray& contents,
                                          int maxOutputTokens,
                                          std::optional<double> temperature,
                                          std::optional<double> topP) {
    QJsonObject body;
    body.insert(QStringLiteral("contents"), contents);

    QJsonObject generationConfig;
    if (temperature.has_value()) {
        generationConfig.insert(QStringLiteral("temperature"), *temperature);
    }
    if (topP.has_value()) {
        generationConfig.insert(QStringLiteral("topP"), *topP);
    }
    generationConfig.insert(QStringLiteral("maxOutputTokens"),
                            maxOutputTokens > 0 ? maxOutputTokens : 2048);
    body.insert(QStringLiteral("generationConfig"), generationConfig);
    return body;
}

std::optional<QString>
parseGeminiGenerateContentText(const QJsonObject& body,
                               GeminiGenerateContentTextError* error) {
    if (error) {
        *error = GeminiGenerateContentTextError::None;
    }
    const auto candidatesVal = body.value(QStringLiteral("candidates"));
    if (!candidatesVal.isArray()) {
        if (error) {
            *error = GeminiGenerateContentTextError::MissingCandidates;
        }
        return std::nullopt;
    }
    const auto candidates = candidatesVal.toArray();
    if (candidates.isEmpty()) {
        if (error) {
            *error = GeminiGenerateContentTextError::MissingCandidates;
        }
        return std::nullopt;
    }
    const auto firstCandidate = candidates.first().toObject();
    const auto contentVal = firstCandidate.value(QStringLiteral("content"));
    if (!contentVal.isObject()) {
        if (error) {
            *error = GeminiGenerateContentTextError::MissingContent;
        }
        return std::nullopt;
    }
    const auto partsVal = contentVal.toObject().value(QStringLiteral("parts"));
    if (!partsVal.isArray()) {
        if (error) {
            *error = GeminiGenerateContentTextError::MissingText;
        }
        return std::nullopt;
    }
    const auto parts = partsVal.toArray();
    QString text;
    for (const auto& part : parts) {
        if (part.isObject()) {
            text += part.toObject().value(QStringLiteral("text")).toString();
        }
    }
    if (text.isEmpty()) {
        if (error) {
            *error = GeminiGenerateContentTextError::MissingText;
        }
        return std::nullopt;
    }
    return text;
}

GeminiGenerateContentResponse parseGeminiGenerateContentResponse(const QJsonObject& body,
                                                                 const QString& requestId) {
    GeminiGenerateContentResponse response;
    const auto candidates = body.value(QStringLiteral("candidates")).toArray();
    if (candidates.isEmpty()) {
        const auto blockReason = body.value(QStringLiteral("promptFeedback"))
                                     .toObject()
                                     .value(QStringLiteral("blockReason"))
                                     .toString();
        response.status = blockReason.isEmpty() ? GeminiGenerateContentResponseStatus::NoContent
                                                : GeminiGenerateContentResponseStatus::Blocked;
        return response;
    }
    const auto candidate = candidates.first().toObject();
    response.finishReason = candidate.value(QStringLiteral("finishReason")).toString();
    if (response.finishReason == QLatin1String("SAFETY") ||
        response.finishReason == QLatin1String("PROHIBITED_CONTENT")) {
        response.status = GeminiGenerateContentResponseStatus::Rejected;
        return response;
    }
    response.modelContent = candidate.value(QStringLiteral("content")).toObject();
    const auto parts = response.modelContent.value(QStringLiteral("parts")).toArray();
    if (parts.isEmpty()) {
        response.status = GeminiGenerateContentResponseStatus::NoContent;
        return response;
    }
    response.status = GeminiGenerateContentResponseStatus::Ok;
    int generatedId = 0;
    for (const auto& value : parts) {
        const auto part = value.toObject();
        if (part.contains(QStringLiteral("text"))) {
            response.text += part.value(QStringLiteral("text")).toString();
            continue;
        }
        if (!part.contains(QStringLiteral("functionCall"))) {
            continue;
        }
        const auto call = part.value(QStringLiteral("functionCall")).toObject();
        QString callId = call.value(QStringLiteral("id")).toString();
        if (callId.isEmpty()) {
            callId = QStringLiteral("sentinel-gemini-%1-%2").arg(requestId).arg(++generatedId);
        }
        response.functionCalls.append(
            GeminiResponseFunctionCall{callId, call.value(QStringLiteral("name")).toString(),
                                       call.value(QStringLiteral("args")),
                                       part.value(QStringLiteral("thoughtSignature")).toString()});
    }
    return response;
}

QJsonObject buildGeminiNativeRequestBody(const QString& model, const QJsonArray& contents,
                                         int maxOutputTokens,
                                         const QJsonArray& functionDeclarations,
                                         const QJsonObject& structuredSchema) {
    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("contents"), contents);
    if (!functionDeclarations.isEmpty()) {
        body.insert(QStringLiteral("tools"),
                    QJsonArray{QJsonObject{
                        {QStringLiteral("functionDeclarations"), functionDeclarations}}});
    }
    QJsonObject generationConfig;
    if (maxOutputTokens > 0) {
        generationConfig.insert(QStringLiteral("maxOutputTokens"), maxOutputTokens);
    }
    if (!structuredSchema.isEmpty()) {
        generationConfig.insert(QStringLiteral("responseMimeType"),
                                QStringLiteral("application/json"));
        generationConfig.insert(QStringLiteral("responseSchema"), structuredSchema);
    }
    if (!generationConfig.isEmpty()) {
        body.insert(QStringLiteral("generationConfig"), generationConfig);
    }
    return body;
}

QJsonArray geminiFunctionResponseParts(const QList<GeminiFunctionResult>& results,
                                       int maxContentLength) {
    QJsonArray parts;
    for (const auto& result : results) {
        QJsonObject functionResponse{
            {QStringLiteral("name"), result.functionName},
            {QStringLiteral("response"),
             QJsonObject{{QStringLiteral("output"), result.content.left(maxContentLength)}}}};
        if (!result.callId.startsWith(QLatin1String("sentinel-gemini-"))) {
            functionResponse.insert(QStringLiteral("id"), result.callId);
        }
        parts.append(QJsonObject{{QStringLiteral("functionResponse"), functionResponse}});
    }
    return parts;
}

std::optional<QList<OllamaModelSummary>> parseGeminiModelCatalog(const QJsonObject& body) {
    const auto modelsValue = body.value(QStringLiteral("models"));
    if (!modelsValue.isArray())
        return std::nullopt;
    QList<OllamaModelSummary> models;
    const auto modelsArray = modelsValue.toArray();
    for (const auto& val : modelsArray) {
        const auto obj = val.toObject();
        auto name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (name.startsWith(QStringLiteral("models/"))) {
            name = name.mid(7);
        }
        if (!val.isObject() || !obj.value(QStringLiteral("name")).isString() || name.isEmpty())
            return std::nullopt;
        const auto displayName = obj.value(QStringLiteral("displayName")).toString().trimmed();
        OllamaModelSummary model{
            name, displayName.isEmpty() ? QStringLiteral("Google Cloud") : displayName, 0};
        const auto inputLimit = obj.value(QStringLiteral("inputTokenLimit"));
        const auto outputLimit = obj.value(QStringLiteral("outputTokenLimit"));
        if (inputLimit.isDouble() && inputLimit.toDouble() > 0 &&
            inputLimit.toDouble() <= std::numeric_limits<int>::max())
            model.capabilities.contextWindow = inputLimit.toInt();
        if (outputLimit.isDouble() && outputLimit.toDouble() > 0 &&
            outputLimit.toDouble() <= std::numeric_limits<int>::max())
            model.capabilities.maxOutputTokens = outputLimit.toInt();
        models.append(std::move(model));
    }
    return models;
}

} // namespace sentinel::core::provider
