// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/runtime/OllamaRuntime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>

#include <optional>

namespace sentinel::core::provider {

// Canonical Anthropic Messages endpoint URL.
QUrl anthropicMessagesUrl();

// Builds an Anthropic /v1/messages request body.
// If maxTokens is <= 0, defaults to 2048.
// Messages array is inserted verbatim.
QJsonObject buildAnthropicMessagesBody(const QString& model,
                                       const QJsonArray& messages,
                                       int maxTokens,
                                       bool stream = false,
                                       const QString& systemPrompt = QString());

// Error classification when parsing Anthropic messages response text.
enum class AnthropicMessagesTextError {
    None,
    MissingContent,
    MissingText,
};

// Extracts and concatenates text from all blocks where type == "text".
// Returns std::nullopt if the "content" array is missing or contains no text.
std::optional<QString> parseAnthropicMessagesText(const QJsonObject& body,
                                                 AnthropicMessagesTextError* error = nullptr);

// One normalized `tool_use` block from a native Anthropic Messages response.
struct AnthropicResponseToolUse {
    // Provider-supplied "id"; empty when the provider omitted it. Callers reject
    // an empty call id, matching the native continuation contract.
    QString callId;
    // Provider-supplied "name"; empty when omitted.
    QString name;
    // Raw "input" value, forwarded unchanged. Callers require a JSON object.
    QJsonValue input;
};

// Normalized scan of a native Anthropic Messages `content` array.
struct AnthropicMessagesResponse {
    // The raw `content` array, in provider order, for continuation assembly.
    QJsonArray contentBlocks;
    // True when `content` is an array with at least one block.
    bool hasContent = false;
    // Concatenation of every block whose `type` is "text", in provider order.
    QString text;
    // Every block whose `type` is "tool_use", in provider order.
    QList<AnthropicResponseToolUse> toolUses;
};

// Scans a native Anthropic Messages response body exactly as the pre-refactor
// ModelService response loop did: a "text" block contributes its `text`, a
// "tool_use" block contributes a normalized tool use, and every other type
// (including non-object blocks) is ignored. `hasContent` mirrors the native
// "content present and non-empty" guard. No capability or tool-registration
// policy is applied here.
AnthropicMessagesResponse parseAnthropicMessagesResponse(const QJsonObject& body);

// Converts an OpenAI-style tool definition envelope
// ({type:"function", function:{name, description, parameters}}) into the flat
// Anthropic Messages tool declaration ({name, description, input_schema}).
// The Anthropic shape must not reuse the nested envelope.
QJsonObject anthropicToolDeclaration(const QJsonObject& nativeToolDefinition);

// Builds the Anthropic native /v1/messages request body. `messages` and `tools`
// (already Anthropic-shaped via anthropicToolDeclaration) are supplied by the
// caller. Unlike the streaming/infer body this never inserts a `stream` key, so
// the native non-streaming request bytes are preserved. A non-empty
// `structuredSchema` adds the output_config json_schema envelope.
QJsonObject buildAnthropicNativeRequestBody(const QString& model, const QJsonArray& messages,
                                            int maxTokens, const QJsonArray& tools = {},
                                            const QJsonObject& structuredSchema = {});

// One Anthropic tool_result entry for a native tool continuation round-trip.
struct AnthropicToolResult {
    QString callId;
    QString content;
};

// Converts provider tool-use ids and result text into Anthropic tool_result
// content blocks. Result text is truncated to `maxContentLength` (default 6800),
// matching the native continuation limit.
QJsonArray anthropicToolResultBlocks(const QList<AnthropicToolResult>& results,
                                     int maxContentLength = 6800);

// Pure discovery payload normalization. nullopt rejects the whole malformed catalog;
// a present empty list is valid. Pagination metadata is intentionally ignored,
// matching the existing single-response discovery contract. No network/policy access.
std::optional<QList<OllamaModelSummary>> parseAnthropicModelCatalog(const QJsonObject& body);

} // namespace sentinel::core::provider
