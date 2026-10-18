// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/runtime/OllamaRuntime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>

#include <optional>

namespace sentinel::core::provider {

// OpenAI-compatible chat completions request path. An endpoint whose configured
// path already ends in "/v1" (for example "https://api.groq.com/openai/v1")
// resolves to "/chat/completions"; every other endpoint resolves to
// "/v1/chat/completions".
QString openAiCompatibleChatCompletionsPath(const QUrl& endpoint);

// Deterministic OpenAI-compatible chat body. Keys and values are written exactly
// as supplied: callers resolve max_tokens (the local inference path clamps to at
// least one while cloud and streaming fall back to 2048) and decide whether a
// temperature field is present. Message objects are inserted verbatim so tool
// call and structured payloads keep their existing byte layout.
QJsonObject buildOpenAiCompatibleChatBody(const QString& model, const QJsonArray& messages,
                                          int maxTokens, bool stream,
                                          std::optional<double> temperature);

// Why a successful HTTP OpenAI-compatible chat response still has no usable
// assistant text.
enum class OpenAiCompatibleChatTextError {
    None,
    MissingChoices,
    MissingText,
};

// Extracts choices[0].message.content. Returns std::nullopt when the response
// omits the choices array or the assistant text; `error`, when provided,
// distinguishes the two. Emitted text is returned verbatim.
std::optional<QString>
parseOpenAiCompatibleChatText(const QJsonObject& body,
                              OpenAiCompatibleChatTextError* error = nullptr);

// One normalized OpenAI-compatible assistant tool call. The nested
// {function:{name,arguments}} envelope is flattened; `arguments` is forwarded
// unchanged (normally a JSON-encoded string) so callers keep their existing
// decode-and-validate step.
struct OpenAiCompatibleToolCall {
    QString callId;       // top-level "id"; empty when the provider omitted it
    QString name;         // "function"."name"; empty when omitted
    QJsonValue arguments; // "function"."arguments", unmodified
};

// Normalizes choices[0].message.tool_calls in provider order. A missing or
// non-array `choices`, a missing/non-object first choice or `message`, or a
// missing/non-array `tool_calls` yields an empty list. No registration or
// argument-decoding policy is applied here.
QList<OpenAiCompatibleToolCall> parseOpenAiCompatibleToolCalls(const QJsonObject& body);

// Shared function envelope used by the endpoint adapter, including its native
// Anthropic/Gemini declaration bridges. This is serialization, not authorization.
QString openAiCompatibleFunctionName(const QString& toolId);
QJsonObject openAiCompatibleToolDefinition(const ToolDescriptor& tool);

struct OpenAiCompatibleNativeRequest {
    QJsonObject body;
    QString error;
};

// Builds the native-capability body without token budgets, capability checks,
// endpoint policy or transport. `content` is the caller's encoded text/images.
// Continuation IDs are compared verbatim; observation text is clipped to 6800.
OpenAiCompatibleNativeRequest buildOpenAiCompatibleNativeRequest(const QString& model,
                                                                 const QJsonValue& content,
                                                                 const ChatRequestOptions& options);

struct OpenAiCompatibleNativeResponse {
    bool hasChoices = false;
    bool outputLimit = false;
    QString text;
    QList<OpenAiCompatibleToolCall> toolCalls;
    QJsonObject diagnostics;
};

// Normalizes the first choice and allowlisted usage/finish metadata. Tool IDs
// remain opaque and missing IDs stay empty. Registration and error taxonomy
// remain adapter responsibilities; no service or transport is consulted.
OpenAiCompatibleNativeResponse normalizeOpenAiCompatibleNativeResponse(const QJsonObject& body);

// Strict object-only JSON decoding shared by tool arguments and structured text.
std::optional<QJsonObject> parseOpenAiCompatibleJsonObject(const QString& text);

struct OpenAiCompatibleCatalogParse {
    QList<OllamaModelSummary> models;
    bool malformed = false;
};

// Validates and parses an OpenAI-compatible "/v1/models" catalog. Every entry
// must be an object with a non-empty trimmed string `id`; otherwise the entire
// catalog is rejected as malformed and no model is returned (P2-DEF-01). A valid
// empty catalog yields no models with malformed == false.
OpenAiCompatibleCatalogParse parseOpenAiCompatibleModelCatalog(const QJsonObject& body);

} // namespace sentinel::core::provider
